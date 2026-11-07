// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: Multiplayer events: eventService.getCurrentSeason, matchingService, race lobbies (lobbyType
// 0).

#include "egs/internal.h"

namespace egs {

constexpr int MatchRegistered = 1, MatchUnregistered = 2, LobbyMatched = 10;
constexpr int ErrNoNextRace = -18025;

struct RaceTemplate {
    QString mode; // Race, Time Trial, Drift
    QList<qint64> tracks;
    QList<qint64> vehicles;
    qint64 laps = 1;
    double timeOfDay = 12.0;
    QString weather = QStringLiteral("Clear");
    QString requirement;
    qint64 timeTrialSeconds = 0;
    qint64 driftSeconds = 0;
};

struct RaceEventList {
    qint64 id = 0;
    QString name;
    QString colour;
    QList<RaceTemplate> templates;
};

const QList<RaceEventList>& allRaceEventLists() {
    static const QList<RaceEventList> lists = [] {
        const QList<qint64> raceTracks{1,  8,  15, 22, 29, 37, 2,  9,  16,
                                       23, 30, 38, 3,  10, 17, 24, 31, 39};
        const QList<qint64> roadCircuits{6, 13, 20, 25, 32, 40, 73, 113, 120, 125, 132, 140,
                                         7, 14, 21, 26, 33, 41, 74, 114, 121, 126, 133, 42};
        const QList<qint64> pointToPoint{4, 11, 18, 27, 34, 43, 71, 111, 118, 127, 134, 143,
                                         5, 12, 19, 28, 35, 44, 72, 112, 119, 128, 135, 144};
        const QList<qint64> hotHatch{58, 51, 84, 8};
        const QList<qint64> sports{13, 16, 9, 46, 11, 38, 15};
        const QList<qint64> performance{40, 17, 6, 12, 2, 25, 5, 20, 37, 109, 45, 57, 27, 54, 47};
        const QList<qint64> performanceRwd{40, 17, 6, 2, 25, 5, 20, 37, 45, 57, 54, 47};
        const QList<qint64> super{10, 14, 39, 82, 7, 56, 23, 55, 3, 48, 4, 83, 30, 42, 26};
        auto t = [](const QString& mode, const QList<qint64>& tracks, const QList<qint64>& cars,
                    qint64 laps, double tod, const QString& weather, const QString& req,
                    qint64 tt = 0, qint64 drift = 0) {
            return RaceTemplate{mode, tracks, cars, laps, tod, weather, req, tt, drift};
        };
        const QString race = QStringLiteral("Race"), trial = QStringLiteral("Time Trial"),
                      drift = QStringLiteral("Drift"), clear = QStringLiteral("Clear"),
                      cloudy = QStringLiteral("Cloudy");
        return QList<RaceEventList>{
            {1,
             QStringLiteral("Hot Hatch Race"),
             QStringLiteral("#E8481C"),
             {t(race, raceTracks, hotHatch, 3, 14.0, clear, QStringLiteral("Hot Hatch Class Only")),
              t(race, roadCircuits, hotHatch, 2, 18.5, cloudy,
                QStringLiteral("Hot Hatch Class Only")),
              t(race, pointToPoint, hotHatch, 1, 10.0, clear,
                QStringLiteral("Hot Hatch Class Only"))}},
            {2,
             QStringLiteral("Performance Race"),
             QStringLiteral("#2A7FD4"),
             {t(race, raceTracks, performance, 3, 13.0, clear,
                QStringLiteral("Performance Class Only")),
              t(race, roadCircuits, performance, 2, 19.0, cloudy,
                QStringLiteral("Performance Class Only")),
              t(race, pointToPoint, performance, 1, 9.0, clear,
                QStringLiteral("Performance Class Only"))}},
            {3,
             QStringLiteral("Super Race"),
             QStringLiteral("#C0392B"),
             {t(race, raceTracks, super, 3, 15.0, clear, QStringLiteral("Super Class Only")),
              t(race, roadCircuits, super, 2, 21.0, clear, QStringLiteral("Super Class Only")),
              t(race, pointToPoint, super, 1, 7.5, QStringLiteral("Overcast"),
                QStringLiteral("Super Class Only"))}},
            {4,
             QStringLiteral("Sports Time Trial"),
             QStringLiteral("#27AE60"),
             {t(trial, raceTracks, sports, 1, 12.0, clear, QStringLiteral("Sports Class Only"),
                300),
              t(trial, roadCircuits, sports, 1, 17.0, clear, QStringLiteral("Sports Class Only"),
                300)}},
            {5,
             QStringLiteral("Performance Drift"),
             QStringLiteral("#8E44AD"),
             {t(drift, roadCircuits, performanceRwd, 1, 20.5, clear,
                QStringLiteral("Performance Class Only"), 0, 300),
              t(drift, pointToPoint, performanceRwd, 1, 16.0, clear,
                QStringLiteral("Performance Class Only"), 0, 300)}},
        };
    }();
    return lists;
}

QList<const RaceEventList*> servedRaceLists() {
    QList<const RaceEventList*> out;
    for (const RaceEventList& e : allRaceEventLists()) {
        if (config().raceEventLists.contains(e.id)) {
            out.append(&e);
        }
    }
    return out;
}

const RaceEventList* raceList(qint64 id) {
    for (const RaceEventList* e : servedRaceLists()) {
        if (e->id == id) {
            return e;
        }
    }
    return nullptr;
}

qint64 slotIntervalMs() {
    return qMax<qint64>(60, config().raceSlotSeconds) * 1000;
}

constexpr qint64 LockoutSeconds = 30, PostLobbyEventSelectSeconds = 30;

struct RaceSlot {
    qint64 id = 0;
    qint64 eventList = 0;
    qint64 matchTime = 0; // Unix ms
    qint64 track = 0;
    QList<qint64> registered;
    QHash<qint64, qint64> registeredBy; // party member -> the leader who signed them up
    QList<qint64> ready;
    bool done = false;
};

struct Matching {
    QMap<qint64, RaceSlot> slots;
    QHash<QString, QPair<qint64, qint64>> lobbyLists;
    QHash<QString, qint64> lobbyCreated;
};

Matching& matching() {
    static Matching m;
    return m;
}

qint64 floorDiv(qint64 a, qint64 b) {
    const qint64 q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

RaceSlot& slotAtLocked(qint64 list, qint64 t) {
    const qint64 id = list * 1000000000LL + floorDiv(t, slotIntervalMs()) % 1000000000LL;
    auto it = matching().slots.find(id);
    if (it != matching().slots.end()) {
        return *it;
    }
    RaceSlot slot;
    slot.id = id;
    slot.eventList = list;
    slot.matchTime = t;
    if (const RaceEventList* e = raceList(list); e && !e->templates.isEmpty()) {
        const qint64 n = e->templates.size();
        const RaceTemplate& tpl = e->templates.at(int(id % n));
        if (!tpl.tracks.isEmpty()) {
            slot.track = tpl.tracks.at(int((id / n) % tpl.tracks.size()));
        }
    }
    return *matching().slots.insert(id, slot);
}

RaceSlot* slotByIdLocked(qint64 id) {
    auto it = matching().slots.find(id);
    return it == matching().slots.end() ? nullptr : &*it;
}

RaceSlot* slotForIdLocked(qint64 id) {
    if (RaceSlot* s = slotByIdLocked(id)) {
        return s;
    }
    const qint64 list = id / 1000000000LL, index = id % 1000000000LL;
    const qint64 t = index * slotIntervalMs();
    if (id <= 0 || !raceList(list) || t <= QDateTime::currentMSecsSinceEpoch()) {
        return nullptr;
    }
    RaceSlot& s = slotAtLocked(list, t);
    return s.id == id ? &s : nullptr;
}

void pushMatchingLocked(qint64 account, int action, qint64 slot, qint64 startsInMs) {
    MatchingCallbackMessage m;
    m.set_accountid(account);
    m.set_action(static_cast<EventValue>(action));
    m.set_matchslotid(slot);
    if (startsInMs >= 0) {
        m.set_startsinmilliseconds(startsInMs);
    }
    pushLocked(account, QByteArrayLiteral("MatchingCallbackMessage"), m.SerializeAsString());
}

// Takes `account` off every slot but `keep` (one slot at a time per player).
void unregisterElsewhereLocked(qint64 account, const RaceSlot* keep) {
    for (RaceSlot& other : matching().slots) {
        if (&other != keep && other.registered.removeAll(account) > 0) {
            other.ready.removeAll(account);
            other.registeredBy.remove(account);
            pushMatchingLocked(account, MatchUnregistered, other.id);
        }
    }
}

QSet<qint64> knownPlayers(qint64 account) {
    QSet<qint64> out;
    for (const QString& n : db::friendNames(account)) {
        if (const auto p = db::playerByName(n)) {
            out.insert(p->accountId);
        }
    }
    if (const auto m = db::clubMember(account)) {
        for (const qint64 a : memberIds(m->clubId)) {
            out.insert(a);
        }
    }
    out.remove(account);
    return out;
}

bool isLocalIPv4(quint32 a) {
    return (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 24) == 127 ||
           (a >> 16) == 0xA9FE || (a >> 22) == (0x6440 >> 6);
}

std::string memberDataFor(const Session& s) {
    std::string md = s.sessionData;
    md.resize(22, '\0');
    if (!config().racePublicAddress) {
        return md;
    }
    bool okLocal = false, okSeen = false;
    const quint32 local = QHostAddress(QString::fromLatin1(md.c_str())).toIPv4Address(&okLocal);
    const quint32 seen = s.peer.toIPv4Address(&okSeen);
    if (!okLocal || !okSeen) {
        return md;
    }
    if (isLocalIPv4(local) && !isLocalIPv4(seen)) {
        const QByteArray text = QHostAddress(seen).toString().toLatin1().left(15);
        std::fill(md.begin(), md.begin() + 16, '\0');
        std::copy(text.begin(), text.end(), md.begin());
    }
    return md;
}

const Session* newestSessionLocked(qint64 account) {
    const Session* newest = nullptr;
    for (const Session& s : std::as_const(sessions().byKey)) {
        if (s.accountId == account && (!newest || s.lastSeen > newest->lastSeen)) {
            newest = &s;
        }
    }
    return newest;
}

PartyMember raceMemberLocked(qint64 account, int number) {
    PartyMember m;
    m.accountId = account;
    m.playerNumber = number;
    m.host = number == 0;
    if (const Session* s = newestSessionLocked(account)) {
        m.name = s->name;
        m.memberData = memberDataFor(*s);
        m.clientVersion = s->clientVersion;
    } else {
        m.memberData.assign(22, '\0');
    }
    const QString partyId = parties().ofAccount.value(qMakePair(account, 1));
    if (const auto it = parties().byId.constFind(partyId); it != parties().byId.cend()) {
        for (const PartyMember& pm : it->members) {
            if (pm.accountId == account) {
                m.contentRestriction = pm.contentRestriction;
            }
        }
    }
    return m;
}

void fillEventList(EventList* out, const RaceEventList& e) {
    EventListObject* o = out->mutable_object();
    o->set_id(e.id);
    o->set_name(e.name.toStdString());
    o->set_status(EVENT_VALUE_2); // active
    o->set_type(EVENT_VALUE_5);   // multiplayer
    for (const RaceTemplate& t : e.templates) {
        EventTemplate* et = out->add_templates();
        et->mutable_gamemodeholder()->mutable_object()->set_name(t.mode.toStdString());
        et->mutable_gamemodeholder()->mutable_configuration()->mutable_team()->set_count(1);
        for (const qint64 id : t.tracks) {
            et->add_tracks()->set_id(id);
        }
        for (const qint64 id : t.vehicles) {
            et->add_vehicles()->set_id(id);
        }
        TemplateConfiguration* c = et->mutable_configuration();
        c->mutable_generation()->set_loanvehicle(t.vehicles.isEmpty() ? 0 : t.vehicles.first());
        c->mutable_generation()->set_timetrialduration(t.timeTrialSeconds);
        c->mutable_generation()->set_driftduration(t.driftSeconds);
        c->mutable_generation()->set_minimumlaps(t.laps);
        c->mutable_playercount()->set_teamsizemaximum(12);
        c->mutable_settings()->set_vehiclerequirement(t.requirement.toStdString());
        TemplateEnvironment* env = c->mutable_environment();
        env->set_seed(1); // ignored: the game seeds with the slot id
        env->set_speedoftime(1);
        env->set_timeofdayfloat(float(t.timeOfDay));
        env->set_weather(t.weather.toStdString());
        env->set_precipitation("Dry");
    }
    EventListConfiguration* c = out->mutable_configuration();
    c->mutable_colour()->set_eventlistcolour(e.colour.toStdString());
    c->mutable_image()->set_billboard(0);
    c->mutable_image()->set_eventlistimage(0);
    EventListMatching* m = c->mutable_matching();
    m->set_interval(slotIntervalMs() / 1000);
    m->set_lockouttime(LockoutSeconds);
    m->set_repeatperiod("SECONDS");
    m->set_schedule(true);
    m->set_nextslotoffsetseconds(60);
    m->set_postlobbytransitiontime(10);
    m->set_postlobbyeventselecttimer(PostLobbyEventSelectSeconds);
    m->set_matchedintotimeout(60);
    m->set_readyuptime(30);
    c->mutable_settings()->set_eventname(e.name.toStdString());
    c->mutable_settings()->set_vehiclerequirement("");
    c->mutable_settings()->set_featureoverride("");
    c->mutable_settings()->set_doublexp(false);
    EventListTimers* t = c->mutable_timers();
    t->set_endofrace(30);
    t->set_eventselect(30);
    t->set_livelobby(60);
    t->set_mingletime(30);
    t->set_vehicleselect(30);
    t->set_viewracereport(30);
    t->set_viewresults(30);
}

void getCurrentSeason(Context&, const GetCurrentSeasonRequest&, GetCurrentSeasonResponse& resp) {
    for (const RaceEventList* e : servedRaceLists()) {
        fillEventList(resp.add_eventlists(), *e);
    }
    resp.mutable_configuration()->mutable_description()->set_title("Community Season");
}

void queryEventListActivity(Context&, const QueryEventListActivityRequest& req,
                            QueryEventListActivityResponse& resp) {
    QList<qint64> want(req.eventlists().begin(), req.eventlists().end());
    if (want.isEmpty()) {
        for (const RaceEventList* e : servedRaceLists()) {
            want.append(e->id);
        }
    }
    QMutexLocker lock(&sessions().mutex);
    for (const qint64 list : want) {
        int players = 0;
        for (const RaceSlot& s : std::as_const(matching().slots)) {
            if (s.eventList == list && !s.done) {
                players += int(s.registered.size());
            }
        }
        EventListActivity* a = resp.add_activity();
        a->set_eventlist(list);
        a->set_players(players);
    }
}

void queryUpcomingMatchSlots(Context& ctx, const QueryUpcomingMatchSlotsRequest& req,
                             QueryUpcomingMatchSlotsResponse& resp) {
    const qint64 account = sessionAccount(ctx);
    QList<qint64> lists;
    for (const IdRef& e : req.eventlists()) {
        lists.append(e.id());
    }
    if (lists.isEmpty()) {
        for (const RaceEventList* e : servedRaceLists()) {
            lists.append(e->id);
        }
    }
    const int n = qBound(1, req.numberofslots() > 0 ? req.numberofslots() : 3, 10);
    const qint64 start =
        req.startdate() > 0 ? req.startdate() : QDateTime::currentMSecsSinceEpoch();
    const qint64 interval = slotIntervalMs();
    const qint64 first = (floorDiv(start, interval) + 1) * interval;
    const QSet<qint64> known = knownPlayers(account);
    QMutexLocker lock(&sessions().mutex);
    for (const qint64 list : lists) {
        if (!raceList(list)) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            const RaceSlot& slot = slotAtLocked(list, first + i * interval);
            UpcomingMatchSlot* u = resp.add_slots();
            MatchSlot* ms = u->mutable_slot();
            ms->set_id(slot.id);
            ms->set_eventlist(list);
            ms->set_matchtime(slot.matchTime);
            ms->mutable_event()->mutable_event()->set_id(slot.track);
            ms->mutable_event()
                ->mutable_gamemodeholder()
                ->mutable_configuration()
                ->mutable_team()
                ->set_count(1);
            int both = 0;
            for (const qint64 a : slot.registered) {
                both += known.contains(a) ? 1 : 0;
            }
            u->set_players(int(slot.registered.size()));
            u->set_friendsandclubmates(both);
            u->set_registered(slot.registered.contains(account));
        }
    }
}

void registerInterest(Context& ctx, const RegisterInterestRequest& req, InterestResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    resp.mutable_slot()->set_id(req.id());
    QMutexLocker lock(&sessions().mutex);
    RaceSlot* slot = slotForIdLocked(req.id());
    if (account == 0 || !slot || slot->done) {
        resp.set_registered(false);
        qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "cannot register for race slot"
                                << req.id() << (slot ? "(already matched)" : "(no such slot)");
        return;
    }
    QList<qint64> who{account};
    for (const qint64 m : req.eligiblepartymembers()) {
        if (m != 0 && !who.contains(m) && newestSessionLocked(m)) {
            who.append(m);
        }
    }
    const qint64 left = qMax<qint64>(0, slot->matchTime - QDateTime::currentMSecsSinceEpoch());
    QStringList names;
    for (const qint64 a : who) {
        unregisterElsewhereLocked(a, slot);
        if (!slot->registered.contains(a)) {
            slot->registered.append(a);
        }
        if (a != account) {
            slot->registeredBy.insert(a, account);
        }
        pushMatchingLocked(a, MatchRegistered, slot->id, left);
        if (const Session* s = newestSessionLocked(a)) {
            names.append(s->name);
        }
    }
    resp.set_registered(true);
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "registered for race slot" << slot->id
                            << (who.size() > 1 ? "with party " + names.join(", ") : QString())
                            << "(list" << slot->eventList << ", track" << slot->track << ", in"
                            << left / 1000 << "s," << slot->registered.size() << "registered)";
}

void unregisterInterest(Context& ctx, const RegisterInterestRequest& req, InterestResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    resp.set_registered(false);
    resp.mutable_slot()->set_id(req.id());
    QMutexLocker lock(&sessions().mutex);
    if (RaceSlot* slot = slotByIdLocked(req.id());
        slot && slot->registered.removeAll(account) > 0) {
        slot->ready.removeAll(account);
        slot->registeredBy.remove(account);
        pushMatchingLocked(account, MatchUnregistered, slot->id);
        // A leader taking the party off takes off the members it signed up.
        int members = 0;
        for (auto it = slot->registeredBy.begin(); it != slot->registeredBy.end();) {
            if (it.value() == account) {
                slot->registered.removeAll(it.key());
                slot->ready.removeAll(it.key());
                pushMatchingLocked(it.key(), MatchUnregistered, slot->id);
                ++members;
                it = slot->registeredBy.erase(it);
            } else {
                ++it;
            }
        }
        qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "unregistered from race slot"
                                << slot->id
                                << (members > 0
                                        ? QStringLiteral("with %1 party member(s)").arg(members)
                                        : QString());
    }
}

void queryInterest(Context& ctx, const QueryInterestRequest& req, QueryInterestResponse& resp) {
    QList<qint64> want;
    for (const IdRef& a : req.accounts()) {
        want.append(a.id());
    }
    if (want.isEmpty()) {
        want.append(sessionAccount(ctx));
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker lock(&sessions().mutex);
    for (const RaceSlot& s : std::as_const(matching().slots)) {
        if (s.done) {
            continue;
        }
        bool hit = false;
        for (const qint64 a : want) {
            hit = hit || s.registered.contains(a);
        }
        if (hit) {
            const qint64 left = qMax<qint64>(0, s.matchTime - now);
            InterestSlot* i = resp.add_slots();
            i->mutable_slot()->set_id(s.id);
            i->set_startsinmilliseconds(std::to_string(left));
            i->set_startsinseconds(std::to_string(left / 1000));
        }
    }
}

void queryMatchSlotsById(Context& ctx, const QueryMatchSlotsByIdRequest& req,
                         QueryMatchSlotsByIdResponse& resp) {
    const QSet<qint64> known = knownPlayers(sessionAccount(ctx));
    QMutexLocker lock(&sessions().mutex);
    for (const qint64 id : req.slotids()) {
        MatchSlotCount* c = resp.add_slots();
        c->set_slotid(id);
        int players = 0, both = 0;
        if (const RaceSlot* s = slotByIdLocked(id)) {
            players = int(s->registered.size());
            for (const qint64 a : s->registered) {
                both += known.contains(a) ? 1 : 0;
            }
        }
        c->set_players(players);
        c->set_friendsandclubmates(both);
    }
}

void markReady(Context& ctx, const MarkReadyRequest& req, MarkReadyResponse& resp, bool ready) {
    const qint64 account = sessionAccount(ctx);
    resp.set_id(req.matchslotid());
    resp.set_ready(ready);
    QMutexLocker lock(&sessions().mutex);
    if (RaceSlot* s = slotForIdLocked(req.matchslotid())) {
        s->ready.removeAll(account);
        if (ready) {
            s->ready.append(account);
        }
    }
}

void joinLobby(Context& ctx, const LobbyWithMemberRequest& req, LobbiesResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const QString id = qs(req.lobby().id());
    QMutexLocker lock(&sessions().mutex);
    auto it = parties().byId.find(id);
    if (account == 0 || it == parties().byId.end() || it->type != 0) {
        ctx.status = 500;
        ctx.serviceError = "no race lobby " + id.toUtf8();
        return;
    }
    bool in = false;
    for (const PartyMember& m : std::as_const(it->members)) {
        in = in || m.accountId == account;
    }
    if (!in) {
        it->members.append(raceMemberLocked(account, int(it->members.size())));
    }
    parties().ofAccount.insert(qMakePair(account, 0), it->id);
    fillLobby(resp.add_lobbies(), *it);
}

void progressToNextRace(Context& ctx, const ProgressToNextRaceRequest& req, LobbiesResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const QString id = qs(req.id());
    QMutexLocker lock(&sessions().mutex);
    auto info = matching().lobbyLists.find(id);
    const RaceEventList* list =
        info != matching().lobbyLists.end() ? raceList(info->first) : nullptr;
    if (!list) {
        ctx.status = 500;
        ctx.serviceError = "no next race for lobby " + id.toUtf8();
        ctx.serviceErrorCode = ErrNoNextRace;
        return;
    }
    if (info->second == 0) {
        const qint64 interval = slotIntervalMs();
        const qint64 earliest = QDateTime::currentMSecsSinceEpoch() +
                                (LockoutSeconds + PostLobbyEventSelectSeconds) * 1000;
        qint64 t = (floorDiv(earliest, interval) + 1) * interval;
        while (slotAtLocked(list->id, t).done) {
            t += interval;
        }
        info->second = slotAtLocked(list->id, t).id;
    }
    ::Lobby* l = resp.add_lobbies();
    const auto it = parties().byId.constFind(id);
    if (it != parties().byId.cend()) {
        fillLobby(l, *it);
    } else {
        l->set_id(id.toStdString());
        l->set_lobbytype(LOBBY_VALUE_0);
        l->set_eventlistid(list->id);
    }
    l->set_matchslotid(info->second);
    l->set_status(LOBBY_VALUE_12);
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "next race of lobby" << id << "-> slot"
                            << info->second;
}

void matchingTickLocked(qint64 now) {
    for (RaceSlot& slot : matching().slots) {
        if (slot.done || now < slot.matchTime) {
            continue;
        }
        slot.done = true;
        if (slot.registered.isEmpty()) {
            continue;
        }
        Party p;
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.type = 0;
        p.status = 6;
        p.matchSlotId = slot.id;
        p.eventListId = slot.eventList;
        for (const qint64 a : std::as_const(slot.registered)) {
            leavePartyLocked(a, 0);
            p.members.append(raceMemberLocked(a, int(p.members.size())));
        }
        parties().byId.insert(p.id, p);
        for (const qint64 a : std::as_const(slot.registered)) {
            parties().ofAccount.insert(qMakePair(a, 0), p.id);
        }
        matching().lobbyLists.insert(p.id, qMakePair(slot.eventList, qint64(0)));
        matching().lobbyCreated.insert(p.id, now);
        pushLobbyLocked(p, LobbyMatched, {}, slot.registered);
        qCInfo(lcEgs).noquote() << "race slot" << slot.id << "(list" << slot.eventList << ", track"
                                << slot.track << "):" << slot.registered.size()
                                << "players into race lobby" << p.id;
    }
    for (auto it = matching().slots.begin(); it != matching().slots.end();) {
        it = it->done && now - it->matchTime > 3600 * 1000 ? matching().slots.erase(it)
                                                           : std::next(it);
    }
    for (auto it = matching().lobbyCreated.begin(); it != matching().lobbyCreated.end();) {
        if (now - it.value() > 6 * 3600 * 1000) {
            matching().lobbyLists.remove(it.key());
            if (const auto p = parties().byId.constFind(it.key());
                p != parties().byId.cend() && p->members.isEmpty()) {
                parties().byId.erase(p);
            }
            it = matching().lobbyCreated.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace egs
