// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "eventlists.h"

#include "content_ids.h"
// The game reads a field named "slots", which is a Qt macro.
#pragma push_macro("slots")
#undef slots
#include "egs.pb.h"
#pragma pop_macro("slots")

#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStringList>
#include <QtEndian>

#include <algorithm>

namespace {

Q_LOGGING_CATEGORY(lcEvents, "events")

// EGSConstants.EventListType: NONE, CAMPAIGN, TIME_TRIAL, CHALLENGE, TOURNAMENT, MULTIPLAYER, ...
constexpr int kTypeMultiplayer = 5;
// A slot id is (list number * kSlotsPerList + slot number) * races + race: the game takes the race
// (its template) from id % races and the track from (id / races) % tracks.
constexpr qint64 kSlotsPerList = 1000000;

struct Race {
    QString mode = QStringLiteral("Race");
    QList<qint64> tracks;
    QString trackName; // the first track's, e.g. uk_road_circuit_01_n
    QList<qint64> vehicles;
    QString vehicleClass;
    qint64 laps = 2;
    qint64 duration = 0; // time trial and drift, as the game counts it
    qint64 maxPlayers = 12;
    QString weather = QStringLiteral("Clear");
    QString precipitation = QStringLiteral("Dry");
    double timeOfDay = 12.0;
    qint64 speedOfTime = 1;
};

struct List {
    qint64 id = 0;
    QString name;
    QString vehicleClass;
    qint64 everyMinutes = 2;
    QList<Race> races;
};

// A private lobby's championship, run by its host: the races are the host's own lobby settings
// (the game's 60-byte settings block), added one by one.
struct Championship {
    QList<QByteArray> races;
    bool running = false;
    qsizetype race = 0; // the race being driven while running
    qint64 seq = 0;     // changes whenever the runtime has to write the lobby settings again
    qint64 changed = 0; // ms since epoch of the last race change
    bool startFirst = false; // the runtime starts race 1 as well (a random championship)
};

struct State {
    QMutex mutex;
    QString title = QStringLiteral("DRIVECLUB Community");
    QList<List> lists;
    QHash<qint64, EventSlot> known; // handed out by queryUpcomingMatchSlots
    QHash<QString, Championship> championships; // lower-case host name
};

State& state() {
    static State s;
    return s;
}

const char kExample[] = R"({
  "title": "DRIVECLUB Community",
  "event_lists": [
    {
      "id": 1,
      "name": "Hot Hatch Championship",
      "every_minutes": 2,
      "vehicle_class": "Hot Hatch",
      "races": [
        { "mode": "Race", "track": "uk_road_circuit_01_n", "laps": 2 },
        { "mode": "Race", "track": "canada_road_circuit_02_n", "laps": 2, "weather": "Cloudy" },
        { "mode": "Race", "track": "japan_race_track_01_v1", "laps": 3, "time_of_day": 19.5 }
      ]
    },
    {
      "id": 2,
      "name": "Supercar Series",
      "every_minutes": 3,
      "vehicle_class": "Super",
      "races": [
        { "mode": "Race", "track": "chile_road_point_01_n" },
        { "mode": "Time Trial", "track": "norway_race_track_01_v1" },
        { "mode": "Race", "track": "india_road_circuit_01_n", "laps": 3, "weather": "Stormy",
          "precipitation": "Light Precipitation" }
      ]
    },
    {
      "id": 3,
      "name": "Drift Cup",
      "every_minutes": 2,
      "vehicle_class": "Sports",
      "races": [
        { "mode": "Drift", "track": "india_road_point_01_n" },
        { "mode": "Drift", "track": "canada_road_point_01_n" }
      ]
    }
  ]
}
)";

std::optional<qint64> contentId(const QJsonValue& v, const ContentId* table, size_t count) {
    if (v.isDouble()) {
        return static_cast<qint64>(v.toDouble());
    }
    const QString name = v.toString().trimmed();
    for (size_t i = 0; i < count; ++i) {
        if (name.compare(QLatin1String(table[i].name), Qt::CaseInsensitive) == 0) {
            return table[i].id;
        }
    }
    return std::nullopt;
}

QList<qint64> contentIds(const QJsonObject& o, const char* one, const char* many,
                         const ContentId* table, size_t count, const QString& where) {
    QJsonArray values = o.value(QLatin1String(many)).toArray();
    if (o.contains(QLatin1String(one))) {
        values.prepend(o.value(QLatin1String(one)));
    }
    QList<qint64> ids;
    for (const QJsonValue& v : values) {
        if (const auto id = contentId(v, table, count)) {
            ids << *id;
        } else {
            qCWarning(lcEvents).noquote() << where << "unknown" << one << v.toVariant().toString();
        }
    }
    return ids;
}

QList<List> parse(const QByteArray& json, QString* title, QString* error) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject()) {
        *error = pe.errorString();
        return {};
    }
    const QJsonObject root = doc.object();
    *title = root.value(QLatin1String("title")).toString(*title);
    QList<List> lists;
    for (const QJsonValue& lv : root.value(QLatin1String("event_lists")).toArray()) {
        const QJsonObject lo = lv.toObject();
        List l;
        l.id = lo.value(QLatin1String("id")).toInteger(lists.size() + 1);
        l.name = lo.value(QLatin1String("name")).toString(QStringLiteral("Event %1").arg(l.id));
        l.vehicleClass = lo.value(QLatin1String("vehicle_class")).toString();
        l.everyMinutes = qMax<qint64>(1, lo.value(QLatin1String("every_minutes")).toInteger(2));
        for (const QJsonValue& rv : lo.value(QLatin1String("races")).toArray()) {
            const QJsonObject ro = rv.toObject();
            Race r;
            const QString where = QStringLiteral("%1 race %2:").arg(l.name).arg(l.races.size() + 1);
            r.mode = ro.value(QLatin1String("mode")).toString(r.mode);
            r.tracks = contentIds(ro, "track", "tracks", kTracks, std::size(kTracks), where);
            for (const ContentId& t : kTracks) {
                if (!r.tracks.isEmpty() && t.id == r.tracks.first()) {
                    r.trackName = QLatin1String(t.name);
                }
            }
            r.vehicles = contentIds(ro, "car", "cars", kVehicles, std::size(kVehicles), where);
            r.vehicleClass = ro.value(QLatin1String("vehicle_class")).toString(l.vehicleClass);
            r.laps = ro.value(QLatin1String("laps")).toInteger(r.laps);
            r.duration = ro.value(QLatin1String("duration")).toInteger(r.duration);
            r.maxPlayers = ro.value(QLatin1String("max_players")).toInteger(r.maxPlayers);
            r.weather = ro.value(QLatin1String("weather")).toString(r.weather);
            r.precipitation = ro.value(QLatin1String("precipitation")).toString(r.precipitation);
            r.timeOfDay = ro.value(QLatin1String("time_of_day")).toDouble(r.timeOfDay);
            r.speedOfTime = ro.value(QLatin1String("speed_of_time")).toInteger(r.speedOfTime);
            if (r.tracks.isEmpty()) {
                qCWarning(lcEvents).noquote() << where << "no track, left out";
                continue;
            }
            l.races << r;
        }
        if (l.races.isEmpty()) {
            qCWarning(lcEvents).noquote() << l.name << "has no races, left out";
            continue;
        }
        lists << l;
    }
    return lists;
}

const List* listById(const QList<List>& lists, qint64 id) {
    for (const List& l : lists) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

void fillRace(const Race& r, EventTemplate* t) {
    GameModeHolder* mode = t->mutable_gamemodeholder();
    mode->mutable_object()->set_name(r.mode.toStdString());
    mode->mutable_configuration()->mutable_team()->set_count(
        r.mode.startsWith(QLatin1String("Team")) ? 2 : 0);
    for (const qint64 id : r.tracks) {
        t->add_tracks()->set_id(id);
    }
    for (const qint64 id : r.vehicles) {
        t->add_vehicles()->set_id(id);
    }
    TemplateConfiguration* c = t->mutable_configuration();
    TemplateGeneration* g = c->mutable_generation();
    g->set_loanvehicle(0);
    g->set_timetrialduration(r.duration);
    g->set_driftduration(r.duration);
    g->set_minimumlaps(r.laps);
    c->mutable_playercount()->set_teamsizemaximum(r.maxPlayers);
    c->mutable_playercount()->set_maximum(r.maxPlayers);
    c->mutable_settings()->set_vehiclerequirement(r.vehicleClass.toStdString());
    TemplateEnvironment* e = c->mutable_environment();
    e->set_speedoftime(r.speedOfTime);
    e->set_seed(1);
    e->set_timeofdayfloat(static_cast<float>(r.timeOfDay));
    e->set_weather(r.weather.toStdString());
    e->set_precipitation(r.precipitation.toStdString());
    e->set_precipitationduration(0);
    e->set_fog(QStringLiteral("Clear").toStdString());
}

void fillList(const List& l, EventList* out) {
    EventListObject* o = out->mutable_object();
    o->set_id(l.id);
    o->set_name(l.name.toStdString());
    o->set_status(GENERIC_1);
    o->set_type(static_cast<GenericEnum>(kTypeMultiplayer));
    for (const Race& r : l.races) {
        fillRace(r, out->add_templates());
    }
    EventListConfiguration* c = out->mutable_configuration();
    c->mutable_image()->set_billboard(0);
    c->mutable_image()->set_eventlistimage(0);
    EventListMatching* m = c->mutable_matching();
    m->set_interval(l.everyMinutes);
    m->set_lockouttime(15);
    m->set_repeatperiod("MINUTES");
    m->set_schedule(false);
    m->set_nextslotoffsetseconds(0);
    m->set_postlobbytransitiontime(10);
    m->set_postlobbyeventselecttimer(20);
    m->set_matchedintotimeout(60);
    m->set_readyuptime(20);
    EventListSettings* s = c->mutable_settings();
    s->set_eventname(l.name.toStdString());
    s->set_vehiclerequirement(l.vehicleClass.toStdString());
    s->set_featureoverride("");
    s->set_doublexp(false);
    EventListTimers* t = c->mutable_timers();
    t->set_endofrace(30);
    t->set_eventselect(20);
    t->set_livelobby(60);
    t->set_viewresults(20);
    t->set_vehicleselect(30);
    t->set_mingletime(20);
    t->set_viewracereport(15);
    c->mutable_colour()->set_eventlistcolour("");
}

} // namespace

bool eventListsLoad(const QString& path, QString* error) {
    if (!QFile::exists(path)) {
        QSaveFile out(path);
        if (out.open(QIODevice::WriteOnly) && out.write(kExample) >= 0 && out.commit()) {
            qCInfo(lcEvents).noquote() << "wrote example event lists to" << path;
        }
    }
    QFile file(path);
    QByteArray json = kExample;
    bool ok = true;
    if (file.open(QIODevice::ReadOnly)) {
        json = file.readAll();
    }
    QString title = QStringLiteral("DRIVECLUB Community");
    QList<List> lists = parse(json, &title, error);
    if (lists.isEmpty()) {
        ok = false;
        if (error->isEmpty()) {
            *error = QStringLiteral("no event lists with races");
        }
        lists = parse(kExample, &title, error);
    }
    QMutexLocker lock(&state().mutex);
    state().title = title;
    state().lists = lists;
    QStringList names;
    for (const List& l : lists) {
        names << QStringLiteral("%1 (%2 races, every %3 min)")
                     .arg(l.name)
                     .arg(l.races.size())
                     .arg(l.everyMinutes);
    }
    qCInfo(lcEvents).noquote() << "event lists:" << names.join(QStringLiteral(", "));
    return ok;
}

void eventListsSeason(GetCurrentSeasonResponse& out) {
    QMutexLocker lock(&state().mutex);
    for (const List& l : state().lists) {
        fillList(l, out.add_eventlists());
    }
    SeasonConfiguration* c = out.mutable_configuration();
    c->mutable_background()->set_image("");
    c->mutable_background()->set_logo("");
    c->mutable_description()->set_title(state().title.toStdString());
}

void eventListsUpcoming(const QueryUpcomingMatchSlotsRequest& req, qint64 accountId,
                        QueryUpcomingMatchSlotsResponse& out) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // startDate: ms or s since 1970.
    qint64 start = req.startdate() > 100000000000LL ? req.startdate() : req.startdate() * 1000;
    start = qMax(start, now);
    const int count = qBound(1, req.numberofslots() > 0 ? req.numberofslots() : 3, 10);
    QMutexLocker lock(&state().mutex);
    for (const IdOnly& wanted : req.eventlists()) {
        const List* l = listById(state().lists, wanted.id());
        if (!l) {
            continue;
        }
        const qint64 listNo = (l - state().lists.constData()) + 1;
        const qint64 every = l->everyMinutes * 60000;
        const qint64 races = l->races.size();
        for (qint64 n = start / every + 1; n <= start / every + count; ++n) {
            EventSlot slot;
            slot.id = (listNo * kSlotsPerList + n % kSlotsPerList) * races;
            slot.eventList = l->id;
            slot.matchTime = n * every;
            slot.races = static_cast<int>(races);
            if (const auto it = state().known.constFind(slot.id); it != state().known.constEnd()) {
                slot.registered = it->registered;
            }
            state().known.insert(slot.id, slot);
            UpcomingMatchSlot* u = out.add_slots();
            MatchSlot* s = u->mutable_slot();
            s->set_id(slot.id);
            s->set_eventlist(slot.eventList);
            s->set_matchtime(slot.matchTime);
            s->mutable_event()->mutable_event()->set_id(slot.id);
            const Race& first = l->races.first();
            s->mutable_event()->mutable_gamemodeholder()->mutable_object()->set_name(
                first.mode.toStdString());
            s->mutable_event()->mutable_gamemodeholder()->mutable_configuration()
                ->mutable_team()->set_count(0);
            u->set_players(static_cast<int>(slot.registered.size()));
            u->set_friendsandclubmates(0);
            u->set_registered(slot.registered.contains(accountId));
        }
    }
}

std::optional<EventSlot> eventListsRegister(qint64 slotId, const QList<qint64>& accounts) {
    QMutexLocker lock(&state().mutex);
    for (EventSlot& s : state().known) {
        for (const qint64 a : accounts) {
            s.registered.removeAll(a);
        }
    }
    const auto it = state().known.find(slotId);
    if (it == state().known.end()) {
        return std::nullopt;
    }
    for (const qint64 a : accounts) {
        if (a) {
            it->registered << a;
        }
    }
    return *it;
}

void eventListsUnregister(qint64 accountId) {
    QMutexLocker lock(&state().mutex);
    for (EventSlot& s : state().known) {
        s.registered.removeAll(accountId);
    }
}

QList<EventSlot> eventListsRegisteredSlots(qint64 accountId) {
    QMutexLocker lock(&state().mutex);
    QList<EventSlot> found;
    for (const EventSlot& s : state().known) {
        if (s.registered.contains(accountId)) {
            found << s;
        }
    }
    return found;
}

int eventListsPlayers(qint64 slotId) {
    QMutexLocker lock(&state().mutex);
    return static_cast<int>(state().known.value(slotId).registered.size());
}

int eventListsActivePlayers(qint64 eventList) {
    QMutexLocker lock(&state().mutex);
    int n = 0;
    for (const EventSlot& s : state().known) {
        if (s.eventList == eventList) {
            n += static_cast<int>(s.registered.size());
        }
    }
    return n;
}

QList<EventSlot> eventListsTakeDue(qint64 nowMs) {
    QMutexLocker lock(&state().mutex);
    QList<EventSlot> due;
    for (auto it = state().known.begin(); it != state().known.end();) {
        if (it->matchTime > nowMs) {
            ++it;
            continue;
        }
        if (!it->registered.isEmpty()) {
            due << *it;
        }
        it = state().known.erase(it);
    }
    return due;
}

qint64 eventListsNextRace(qint64 slotId) {
    QMutexLocker lock(&state().mutex);
    for (qsizetype i = 0; i < state().lists.size(); ++i) {
        const qint64 races = state().lists.at(i).races.size();
        if ((slotId / races) / kSlotsPerList != i + 1) {
            continue;
        }
        return slotId % races + 1 < races ? slotId + 1 : 0;
    }
    return 0;
}

// Championships in a private lobby

namespace {

constexpr qsizetype kLobbySettingsSize = 60;
constexpr qsizetype kMaxRaces = 12;

QString raceTitle(const QByteArray& block) {
    const auto* b = reinterpret_cast<const uchar*>(block.constData());
    const int mode = b[0] | b[1] << 8;
    const qint64 track = qint64(b[4]) | qint64(b[5]) << 8 | qint64(b[6]) << 16 | qint64(b[7]) << 24;
    const int laps = b[0xa] | b[0xb] << 8;
    QString title = QString::number(track);
    for (const ContentId& t : kTracks) {
        if (t.id == track) {
            title = QString::fromUtf8(t.title);
        }
    }
    static const char* const modes[] = {"Race", "Drift", "Time Trial"};
    title += QStringLiteral(", %1").arg(QLatin1String(mode >= 0 && mode < 3 ? modes[mode] : "?"));
    if (mode == 0) {
        title += QStringLiteral(", %1 laps").arg(laps);
    }
    return title;
}

QByteArray championshipAnswer(const Championship& c) {
    QByteArray out = "seq=" + QByteArray::number(c.seq) + '\n';
    const qsizetype n = c.races.size();
    if (n && c.race >= n) {
        return out + "active=0\nraces=" + QByteArray::number(n) + "\nnote=Championship finished!\n";
    }
    if (!c.running) {
        const QString note =
            n ? QStringLiteral("Championship: %1 race%2 set").arg(n).arg(n == 1 ? QString() : QStringLiteral("s"))
              : QStringLiteral("Championship: no races yet");
        return out + "active=0\nraces=" + QByteArray::number(n) + "\nnote=" + note.toUtf8() + '\n';
    }
    const QString note = QStringLiteral("Championship race %1/%2: %3")
                             .arg(c.race + 1)
                             .arg(n)
                             .arg(raceTitle(c.races.at(c.race)));
    // From the second race on the host's runtime starts the race by itself once all are back.
    return out + "active=1\nraces=" + QByteArray::number(n) + "\nrace=" + QByteArray::number(c.race + 1) +
           "\nautostart=" + (c.race > 0 || c.startFirst ? "1" : "0") + "\nnote=" + note.toUtf8() +
           "\nblock=" + c.races.at(c.race).toHex() + '\n';
}

// The lobby settings' location (+2) for a track, from its name: the order of tracks.csv's locations.
int trackLocation(const char* name) {
    static const char* const locations[] = {"canada_", "chile_", "india_", "japan_", "norway_", "uk_"};
    for (int i = 0; i < 6; ++i) {
        if (qstrncmp(name, locations[i], qstrlen(locations[i])) == 0) {
            return i;
        }
    }
    return -1;
}

// Races on random tracks and at random times of day, from the host's lobby settings (mode, class,
// laps and the rest stay), no track twice. Add-on tracks too: the launcher's DLC unlock (on by
// default) makes them available; without it the game refuses one and keeps the lobby's track.
QList<QByteArray> randomRaces(const QByteArray& lobby, int count) {
    QList<const ContentId*> tracks;
    for (const ContentId& t : kTracks) {
        if (t.id < 900 && trackLocation(t.name) >= 0) {
            tracks << &t;
        }
    }
    static const quint16 times[] = {6 * 60, 8 * 60, 12 * 60, 15 * 60, 18 * 60, 19 * 60 + 30, 22 * 60};
    QList<QByteArray> races;
    for (int i = 0; i < count && !tracks.isEmpty(); ++i) {
        const ContentId* t = tracks.takeAt(QRandomGenerator::global()->bounded(tracks.size()));
        QByteArray b = lobby;
        qToLittleEndian<quint16>(quint16(trackLocation(t->name)), b.data() + 2);
        qToLittleEndian<quint32>(quint32(t->id), b.data() + 4);
        qToLittleEndian<quint16>(times[QRandomGenerator::global()->bounded(int(std::size(times)))],
                                 b.data() + 0xc);
        // Weather (+0x10): clear, light clouds, overcast, stormy; precipitation (+0x12): dry, light,
        // heavy, dynamic. Half the races are clear; rain or snow (the track's) only under clouds.
        quint16 weather = 0, precipitation = 0;
        if (QRandomGenerator::global()->bounded(2)) {
            weather = quint16(1 + QRandomGenerator::global()->bounded(3));
            precipitation = weather >= 2 ? quint16(QRandomGenerator::global()->bounded(4)) : 0;
        }
        qToLittleEndian<quint16>(weather, b.data() + 0x10);
        qToLittleEndian<quint16>(precipitation, b.data() + 0x12);
        races << b;
    }
    return races;
}

} // namespace

QByteArray eventListsChampionship(const QString& player, const QString& command,
                                  const QByteArray& block) {
    QMutexLocker lock(&state().mutex);
    Championship& c = state().championships[player.toLower()];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (command == QLatin1String("add")) {
        const QByteArray b = QByteArray::fromHex(block);
        if (c.running || c.race >= c.races.size()) {
            // Adding after a run starts a new championship.
            if (c.running || !c.races.isEmpty()) {
                c.races.clear();
            }
            c.running = false;
            c.race = 0;
            c.startFirst = false;
        }
        if (b.size() == kLobbySettingsSize && c.races.size() < kMaxRaces) {
            c.races << b;
            qCInfo(lcEvents).noquote() << player << "added championship race" << c.races.size()
                                       << ":" << raceTitle(b);
            ++c.seq;
            return "seq=" + QByteArray::number(c.seq) + "\nactive=0\nraces=" + QByteArray::number(c.races.size()) +
                   "\nnote=" + QStringLiteral("Race %1 added: %2")
                       .arg(c.races.size())
                       .arg(raceTitle(b))
                       .toUtf8() +
                   '\n';
        }
    } else if (command == QLatin1String("random")) {
        const QByteArray b = QByteArray::fromHex(block);
        if (b.size() == kLobbySettingsSize) {
            c.races = randomRaces(b, 3);
            c.running = true;
            c.race = 0;
            c.changed = now;
            c.startFirst = true;
            QStringList titles;
            for (const QByteArray& r : c.races) {
                titles << raceTitle(r);
            }
            qCInfo(lcEvents).noquote() << player << "started a random championship:" << titles.join(" | ");
            ++c.seq;
        }
    } else if (command == QLatin1String("toggle")) {
        c.startFirst = false;
        if (c.running && c.race < c.races.size()) {
            c.running = false;
            c.race = 0;
            qCInfo(lcEvents).noquote() << player << "stopped the championship";
        } else if (!c.races.isEmpty()) {
            c.running = true;
            c.race = 0;
            c.changed = now;
            qCInfo(lcEvents).noquote() << player << "started a championship of" << c.races.size()
                                       << "races";
        }
        ++c.seq;
    }
    return championshipAnswer(c);
}

void eventListsRaceFinished(const QString& player) {
    QMutexLocker lock(&state().mutex);
    const auto it = state().championships.find(player.toLower());
    if (it == state().championships.end() || !it->running || it->race >= it->races.size()) {
        return;
    }
    // The game sends its stats more than once after an event; a race takes longer than this.
    if (QDateTime::currentMSecsSinceEpoch() - it->changed < 45000) {
        return;
    }
    ++it->race;
    ++it->seq;
    it->changed = QDateTime::currentMSecsSinceEpoch();
    if (it->race >= it->races.size()) {
        it->running = false;
    }
    qCInfo(lcEvents).noquote() << player << "finished championship race" << it->race << "of"
                               << it->races.size();
}