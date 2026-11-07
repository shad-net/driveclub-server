// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// OTG: the board, challenge and gamestate tools of otg/otg.h (admin app).

#include "otg/internal.h"

using namespace otg;

QString otgBoardKind(const QString& name) {
    static const QRegularExpression challengeOwner(
        QStringLiteral("^challenges/(player|club)/(\\d+)$"));
    static const QRegularExpression challengeBoard(
        QStringLiteral("^challenges/(player|club)/-?\\d+h\\d+m\\d+s\\d+$"));
    if (name.startsWith(QLatin1String("eventhistory/"))) {
        return QStringLiteral("event_history");
    }
    if (challengeBoard.match(name).hasMatch()) {
        return QStringLiteral("challenge_board");
    }
    if (challengeOwner.match(name).hasMatch() || name.startsWith(QLatin1String("challenges/"))) {
        return QStringLiteral("challenge_list");
    }
    return QStringLiteral("leaderboard");
}

static QString featuredListOf(const QString& board) {
    static const QRegularExpression featured(
        QStringLiteral("^challenges/(community|evo)/(player|club)$"));
    const auto m = featured.match(board);
    return m.hasMatch() ? m.captured(1) : QString();
}

QJsonArray otgChallenges() {
    static const QRegularExpression id(QStringLiteral("^(-?\\d+)h(\\d+)m(\\d+)s(\\d+)$"));
    struct Info {
        QString scope;
        QString creator;
        qint64 ends = 0;
        QJsonArray members;
        QJsonArray attachment;
        qint64 updated = 0;
        QJsonArray featured;
        QString creatorClub;
    };
    QMap<QString, Info> byId;
    for (const db::BoardInfo& b : db::boards()) {
        if (otgBoardKind(b.name) != QLatin1String("challenge_list")) {
            continue;
        }
        const QStringList parts = b.name.split('/');
        const QString owner = parts.value(2);
        // challenges/{community,evo}/{player,club}: featured copies, not a player's list.
        const QString featuredList = featuredListOf(b.name);
        if (!featuredList.isEmpty()) {
            for (const Entry& e : db::boardScores(b.name)) {
                Info& info = byId[e.entrant];
                info.featured.append(QJsonObject{{QStringLiteral("list"), featuredList},
                                                 {QStringLiteral("ends"), double(e.score >> 8)}});
                if (info.attachment.isEmpty()) {
                    info.attachment = e.attachment;
                }
            }
            continue;
        }
        for (const Entry& e : db::boardScores(b.name)) {
            const auto m = id.match(e.entrant);
            Info& info = byId[e.entrant];
            info.scope = parts.value(1);
            info.creator = m.hasMatch() ? m.captured(4) : QString();
            info.ends = std::max(info.ends, qint64(e.score >> 8));
            info.updated = std::max(info.updated, e.updated);
            const qint64 flags = qint64(e.score & 0xff);
            const auto ownerClub = parts.value(1) == QLatin1String("club")
                                       ? db::club(owner.toLongLong())
                                       : std::nullopt;
            info.members.append(QJsonObject{
                {QStringLiteral("account_id"), owner},
                {QStringLiteral("name"), ownerClub ? QStringLiteral("club %1").arg(ownerClub->name)
                                                   : egsPlayerName(owner.toLongLong())},
                {QStringLiteral("flags"), flags},
                {QStringLiteral("updated"), double(e.updated)},
            });
            if (flags & 1 || info.attachment.isEmpty()) {
                info.attachment = e.attachment;
            }
            if ((flags & 1) && ownerClub) {
                info.creatorClub = ownerClub->name;
            }
        }
    }
    QJsonArray out;
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
        if (it->scope.isEmpty()) {
            continue;
        }
        const QString board = QStringLiteral("challenges/%1/%2").arg(it->scope, it.key());
        const QList<Entry> times = db::boardScores(board);
        QJsonObject o{
            {QStringLiteral("id"), it.key()},
            {QStringLiteral("scope"), it->scope},
            {QStringLiteral("creator_account"), it->creator},
            {QStringLiteral("creator_name"),
             it->creator.isEmpty() ? QString() : egsPlayerName(it->creator.toLongLong())},
            {QStringLiteral("kind"), it->scope}, // "player" or "club"
            {QStringLiteral("creator_club"), it->creatorClub},
            {QStringLiteral("ends"), double(it->ends)}, // game clock, ms
            {QStringLiteral("members"), it->members},
            {QStringLiteral("attachment"), it->attachment},
            {QStringLiteral("board"), board},
            {QStringLiteral("entries"), qint64(times.size())},
            {QStringLiteral("updated"), double(it->updated)},
            {QStringLiteral("featured"), it->featured},
        };
        if (!times.isEmpty()) {
            o.insert(QStringLiteral("best"), QString::number(times.first().score));
            o.insert(QStringLiteral("best_entrant"), times.first().entrant);
            const auto bestClub = it->scope == QLatin1String("club")
                                      ? db::club(times.first().entrant.toLongLong())
                                      : std::nullopt;
            o.insert(QStringLiteral("best_name"),
                     bestClub ? QStringLiteral("club %1").arg(bestClub->name)
                              : egsPlayerName(times.first().entrant.toLongLong()));
        }
        out.append(o);
    }
    return out;
}

bool otgFeatureChallenge(const QString& challenge, const QString& list, int extendDays,
                         QString* error) {
    if (list != QLatin1String("community") && list != QLatin1String("evo")) {
        *error = QStringLiteral("list must be community or evo");
        return false;
    }
    std::optional<Entry> source;
    QString scope;
    for (const db::BoardInfo& b : db::boards()) {
        static const QRegularExpression owner(QStringLiteral("^challenges/(player|club)/-?\\d+$"));
        const auto m = owner.match(b.name);
        if (!m.hasMatch()) {
            continue;
        }
        const auto e = db::score(b.name, challenge);
        if (e && (!source || (e->score & 1))) {
            source = e;
            scope = m.captured(1);
        }
    }
    if (!source) {
        *error = QStringLiteral("no such challenge");
        return false;
    }
    const quint64 day = 24ull * 3600 * 1000;
    const quint64 ends = (source->score >> 8) + quint64(std::max(0, extendDays)) * day;
    Entry e = *source;
    e.board = QStringLiteral("challenges/%1/%2").arg(list, scope);
    e.score = ends << 8; // flags 0
    e.updated = QDateTime::currentMSecsSinceEpoch();
    db::putScore(e);
    qCInfo(lcOtg).noquote() << "challenge" << challenge << "featured on" << e.board
                            << QStringLiteral("(ends %1 game time)")
                                   .arg(QDateTime::fromMSecsSinceEpoch(qint64(ends),
                                                                       QTimeZone::utc())
                                            .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));
    return true;
}

bool otgUnfeatureChallenge(const QString& challenge, const QString& list) {
    bool removed = false;
    for (const QString& scope : {QStringLiteral("player"), QStringLiteral("club")}) {
        removed |= db::removeScore(QStringLiteral("challenges/%1/%2").arg(list, scope), challenge);
    }
    if (removed) {
        qCInfo(lcOtg).noquote() << "challenge" << challenge << "no longer featured on" << list;
    }
    return removed;
}

QJsonArray otgBoardList() {
    QJsonArray out;
    for (const db::BoardInfo& b : db::boards()) {
        out.append(QJsonObject{
            {QStringLiteral("name"), b.name},
            {QStringLiteral("kind"), otgBoardKind(b.name)},
            {QStringLiteral("entries"), qint64(b.entries)},
            {QStringLiteral("best"), QString::number(b.best)},
            {QStringLiteral("updated"), double(b.updated)},
        });
    }
    return out;
}

std::optional<QJsonArray> otgBoardEntries(const QString& name) {
    const QList<Entry> list = db::boardScores(name);
    if (list.isEmpty()) {
        return std::nullopt;
    }
    QJsonArray out;
    int rank = 0;
    for (const Entry& e : list) {
        QString who;
        if (isClubBoard(name)) {
            const auto club = db::club(e.entrant.toLongLong());
            who = club ? QStringLiteral("club %1").arg(club->name)
                       : QStringLiteral("club %1").arg(e.entrant);
        } else if (!e.entrant.contains(QLatin1Char('h'))) {
            who = egsPlayerName(e.entrant.toLongLong());
        }
        out.append(QJsonObject{
            {QStringLiteral("rank"), ++rank},
            {QStringLiteral("entrant"), e.entrant},
            {QStringLiteral("name"), who},
            {QStringLiteral("score"), QString::number(e.score)},
            {QStringLiteral("attachment"), e.attachment},
            {QStringLiteral("updated"), double(e.updated)},
        });
    }
    return out;
}

bool otgRemoveEntry(const QString& board, const QString& entrant) {
    return db::removeScore(board, entrant);
}

bool otgSetEntry(const QString& board, const QString& entrant, quint64 score,
                 const QJsonArray& attachment) {
    if (board.isEmpty() || entrant.isEmpty()) {
        return false;
    }
    return db::putScore({board, entrant, score, attachment, QDateTime::currentMSecsSinceEpoch()});
}

QJsonArray otgGameStates() {
    QJsonArray out;
    for (const db::GameState& g : db::gameStates()) {
        out.append(QJsonObject{
            {QStringLiteral("resource"), g.resource},
            {QStringLiteral("version"), g.version},
            {QStringLiteral("counts"), g.counts},
            {QStringLiteral("updated"), double(g.updated)},
        });
    }
    return out;
}

bool otgRemoveGameState(const QString& resource) {
    return db::removeGameState(resource);
}

bool otgClearBoard(const QString& board) {
    return db::clearBoard(board);
}
