// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: statsService: player and club stats pages.

#include "egs/internal.h"

namespace egs {

const QString StatsResource = QStringLiteral("stats/V06/player/%1");

struct StatColumn {
    const char* name;
    const char* subject;
    const char* counter;
};

const QList<QPair<QString, QString>>& rowSets() {
    static const QList<QPair<QString, QString>> sets = {
        {QStringLiteral("Tracks"), QStringLiteral("track")},
        {QStringLiteral("Vehicles"), QStringLiteral("vehicle")},
        {QStringLiteral("GameModes"), QStringLiteral("event")},
        {QStringLiteral("Challenges"), QStringLiteral("solochallenge")}};
    return sets;
}
const StatColumn RowColumns[] = {
    {"Attempts", "", "attempts"}, {"TotalDistanceDriven", "", "distance"},
    {"TotalFame", "", "fame"},    {"Wins", "", "wins"},
    {"Created", "", "created"},   {"Forwarded", "", "forwarded"}, // no counter seen: 0
};
const StatColumn OverdriveColumns[] = {
    {"PositionGainedTotalFame", "UniquePosition", "fame"},
    {"PositionGainedAttempts", "UniquePosition", "attempts"},
    {"SectorAccuracyTotalFame", "Sector Accuracy", "fame"},
    {"SectorAccuracyAttempts", "Sector Accuracy", "attempts"},
    {"CleanLapTotalFame", "CleanLap", "fame"},
    {"CleanLapAttempts", "CleanLap", "attempts"},
    {"CleanRaceTotalFame", "CleanRace", "fame"},
    {"CleanRaceAttempts", "CleanRace", "attempts"},
    {"CleanSectorTotalFame", "CleanSector", "fame"},
    {"CleanSectorAttempts", "CleanSector", "attempts"},
    {"CrazySpeedTotalFame", "Crazy Speed", "fame"},
    {"CrazySpeedAttempts", "Crazy Speed", "attempts"},
    {"DraftTotalFame", "Drafting", "fame"},
    {"DraftAttempts", "Drafting", "attempts"},
    {"DriftOverdriveTotalFame", "Drift", "fame"},
    {"DriftOverdriveAttempts", "Drift", "attempts"},
    {"VehicleCollisionTotalFame", "VehicleCollision", "fame"},
    {"VehicleCollisionAttempts", "VehicleCollision", "attempts"},
    {"EnvironmentCollisionTotalFame", "EnvironmentCollision", "fame"},
    {"EnvironmentCollisionAttempts", "EnvironmentCollision", "attempts"},
    {"FinishPositionTotalFame", "FinishPosition", "fame"},
    {"FinishPositionAttempts", "FinishPosition", "attempts"},
    {"OffTrackTotalFame", "OffTrack", "fame"},
    {"OffTrackAttempts", "OffTrack", "attempts"},
};

const StatColumn FaceOffColumns[] = {
    {"AverageSpeedTotalFame", "Average Speed Face-Off", "fame"},
    {"AverageSpeedAttempts", "Average Speed Face-Off", "attempts"},
    {"AverageSpeedWins", "Average Speed Face-Off", "wins"},
    {"CorneringTotalFame", "Cornering Face-Off", "fame"},
    {"CorneringAttempts", "Cornering Face-Off", "attempts"},
    {"CorneringWins", "Cornering Face-Off", "wins"},
    {"DriftTotalFame", "Drift Face-Off", "fame"},
    {"DriftAttempts", "Drift Face-Off", "attempts"},
    {"DriftWins", "Drift Face-Off", "wins"},
};

qint64 statValue(const QJsonObject& counts, const QString& subject, const QString& category,
                 const QString& counter) {
    const QString key = subject + QLatin1Char('/') + category + QLatin1Char('/') + counter;
    return counts.value(key).toInteger() - counts.value(key + QStringLiteral("/-")).toInteger();
}

QStringList statSets() {
    QStringList sets;
    for (const auto& s : rowSets()) {
        sets << s.first;
    }
    return sets << QStringLiteral("Overdrive") << QStringLiteral("FaceOff");
}

EgsStatsTable statsTable(qint64 accountId, const QString& set) {
    const auto g = db::gameState(StatsResource.arg(accountId));
    return statsFromCounts(g ? g->counts : QJsonObject(), bool(g), set);
}

EgsStatsTable clubStatsTable(qint64 clubId, const QString& set, int* membersWithStats) {
    if (const auto own = db::gameState(QStringLiteral("stats/V06/club/%1").arg(clubId))) {
        return statsFromCounts(own->counts, true, set);
    }
    QHash<QString, qint64> sums;
    int withStats = 0;
    for (const db::ClubMember& m : db::clubMembers(clubId)) {
        const auto g = db::gameState(StatsResource.arg(m.accountId));
        if (!g) {
            continue;
        }
        ++withStats;
        for (auto it = g->counts.constBegin(); it != g->counts.constEnd(); ++it) {
            sums[it.key()] += it.value().toInteger();
        }
    }
    QJsonObject counts;
    for (auto it = sums.cbegin(); it != sums.cend(); ++it) {
        counts.insert(it.key(), it.value());
    }
    if (membersWithStats) {
        *membersWithStats = withStats;
    }
    return statsFromCounts(counts, withStats > 0, set);
}

EgsStatsTable statsFromCounts(const QJsonObject& counts, bool stored, const QString& set) {
    EgsStatsTable t;
    t.stored = stored;
    const auto fixed = [&](const StatColumn* first, const StatColumn* last,
                           const QString& category) {
        QList<qint64> values;
        for (const StatColumn* c = first; c != last; ++c) {
            t.columns << QString::fromLatin1(c->name);
            values << statValue(counts, QString::fromLatin1(c->subject), category,
                                QString::fromLatin1(c->counter));
        }
        t.rows.append({set, values});
        t.known = true;
    };
    if (set == QLatin1String("Overdrive")) {
        fixed(std::begin(OverdriveColumns), std::end(OverdriveColumns),
              QStringLiteral("overdrive"));
        return t;
    }
    if (set == QLatin1String("FaceOff")) {
        fixed(std::begin(FaceOffColumns), std::end(FaceOffColumns), QStringLiteral("faceoff"));
        return t;
    }
    for (const auto& [name, category] : rowSets()) {
        if (name != set) {
            continue;
        }
        t.known = true;
        for (const StatColumn& c : RowColumns) {
            t.columns << QString::fromLatin1(c.name);
        }
        const QString marker = QLatin1Char('/') + category + QLatin1Char('/');
        QStringList subjects;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            const qsizetype at = it.key().indexOf(marker);
            if (at > 0 && !subjects.contains(it.key().left(at))) {
                subjects << it.key().left(at);
            }
        }
        for (const QString& subject : subjects) {
            QList<qint64> values;
            for (const StatColumn& c : RowColumns) {
                values << statValue(counts, subject, category, QString::fromLatin1(c.counter));
            }
            t.rows.append({subject, values});
        }
    }
    return t;
}

void fillStats(const EgsStatsTable& t, GetAccountStatsResponse& resp) {
    for (const QString& c : t.columns) {
        resp.add_columnnames(c.toStdString());
    }
    for (const auto& [name, values] : t.rows) {
        AccountStat* row = resp.add_stats();
        row->set_total(1);
        row->mutable_definition()->set_name(name.toStdString());
        StatEntry* entry = row->add_entries();
        for (const qint64 v : values) {
            entry->add_columns(v);
        }
    }
}

void accountStats(Context& ctx, const GetAccountStatsRequest& req, GetAccountStatsResponse& resp) {
    const QString set = qs(req.columndefinitionsetname());
    qint64 club = req.clubs_size() > 0 ? req.clubs(0).id() : 0;
    if (club == 0 && req.currentclub()) {
        const auto m = db::clubMember(sessionAccount(ctx));
        club = m ? m->clubId : 0;
    }
    const qint64 account = req.accounts_size() > 0 ? req.accounts(0).id() : sessionAccount(ctx);
    const EgsStatsTable t = club ? clubStatsTable(club, set) : statsTable(account, set);
    if (!t.known) {
        qCWarning(lcEgs).noquote() << "stats: unknown column set" << set;
    }
    fillStats(t, resp);
    qCInfo(lcEgs).noquote() << "stats" << set << "for"
                            << (club ? QStringLiteral("club %1").arg(club) : egsPlayerName(account))
                            << "->" << resp.stats_size()
                            << (resp.stats_size() == 1 ? "row" : "rows")
                            << (!t.stored ? QStringLiteral("(no stats stored yet)") : QString());
}

void clubStats(Context& ctx, const GetClubStatsRequest& req, GetAccountStatsResponse& resp) {
    const QString set = qs(req.columndefinitionsetname());
    qint64 club = req.clubs_size() > 0 ? req.clubs(0) : 0;
    if (club == 0 && req.currentclub()) {
        const auto m = db::clubMember(sessionAccount(ctx));
        club = m ? m->clubId : 0;
    }
    int members = 0;
    const EgsStatsTable t = clubStatsTable(club, set, &members);
    if (!t.known) {
        qCWarning(lcEgs).noquote() << "club stats: unknown column set" << set;
    }
    fillStats(t, resp);
    const auto c = club ? db::club(club) : std::nullopt;
    qCInfo(lcEgs).noquote() << "club stats" << set << "for club" << club
                            << (c ? QStringLiteral("\"%1\"").arg(c->name)
                                  : QStringLiteral("(unknown)"))
                            << "->" << resp.stats_size()
                            << (resp.stats_size() == 1 ? "row" : "rows")
                            << QStringLiteral("(sum of %1 member(s) with stats)").arg(members);
}

} // namespace egs
