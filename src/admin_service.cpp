// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "admin_service.h"

#include "auth.h"
#include "database.h"

#include <QDateTime>
#include "egs.h"
#include "file_log.h"
#include "game_data.h"
#include "otg.h"
#include "request_log.h"
#include "version.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>

namespace {

Q_LOGGING_CATEGORY(lcAdmin, "admin")

HttpResponse json(int status, const QJsonValue& value) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json";
    r.body = value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                             : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    return r;
}

HttpResponse error(int status, const QString& message) {
    return json(status, QJsonObject{{QStringLiteral("error"), message}});
}

bool sameKey(const QByteArray& given, const QString& expected) {
    const QByteArray a = QCryptographicHash::hash(given, QCryptographicHash::Sha256);
    const QByteArray b = QCryptographicHash::hash(expected.toUtf8(), QCryptographicHash::Sha256);
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace

AdminService::AdminService(const Config& config) : config_(config) {
    uptime_.start();
}

void AdminService::handle(const HttpRequest& request, HttpServer::Responder respond) {
    if (config_.adminKey.isEmpty() ||
        !sameKey(request.headers.value("x-admin-key"), config_.adminKey)) {
        qCWarning(lcAdmin).noquote()
            << request.method << request.path << "from" << request.peer.toString() << "-> 401";
        static QSet<QString> warned;
        const bool gamePath = request.path.startsWith(QLatin1String("/egs/")) ||
                              request.path.startsWith(QLatin1String("/titles/")) ||
                              request.path.startsWith(QLatin1String("/login/"));
        if (gamePath && !warned.contains(request.peer.toString())) {
            warned.insert(request.peer.toString());
            qCWarning(lcAdmin).noquote()
                << "a game is connecting to the ADMIN port from" << request.peer.toString()
                << ": its EGS/OTG address must use the game port" << config_.port
                << "(listen.port), not the admin port" << config_.adminPort;
        }
        respond(error(401, QStringLiteral("missing or wrong X-Admin-Key")));
        return;
    }
    const HttpResponse response = route(request);
    if (request.method != "GET" || response.status >= 400) {
        qCInfo(lcAdmin).noquote() << request.method << request.path << "->" << response.status;
    }
    respond(response);
}

HttpResponse AdminService::route(const HttpRequest& request) {
    const QStringList parts = request.path.split('/', Qt::SkipEmptyParts);
    const QByteArray& m = request.method;
    if (parts.size() < 2 || parts[0] != QLatin1String("api")) {
        return error(404, QStringLiteral("not found"));
    }
    const QString& what = parts[1];
    const QString board = request.query.queryItemValue(QStringLiteral("board"), QUrl::FullyDecoded);

    if (what == QLatin1String("status") && parts.size() == 2 && m == "GET") {
        return status();
    }
    if (what == QLatin1String("sessions")) {
        if (parts.size() == 2 && m == "GET") {
            return sessions();
        }
        if (parts.size() == 3 && m == "DELETE") {
            if (!egsDropSession(parts[2])) {
                return error(404, QStringLiteral("no such session"));
            }
            qCInfo(lcAdmin).noquote() << "dropped EGS session" << parts[2];
            return json(200, QJsonObject{{QStringLiteral("dropped"), true}});
        }
    }
    if (what == QLatin1String("otg") && parts.size() >= 3 && parts[2] == QLatin1String("tokens")) {
        if (parts.size() == 3 && m == "GET") {
            return tokens();
        }
        if (parts.size() == 4 && m == "DELETE") {
            if (!otgDropToken(parts[3])) {
                return error(404, QStringLiteral("no such token"));
            }
            return json(200, QJsonObject{{QStringLiteral("dropped"), true}});
        }
    }
    if (what == QLatin1String("leaderboards")) {
        if (parts.size() == 2 && m == "GET") {
            return json(200, otgBoardList());
        }
        if (parts.size() == 2 && m == "DELETE") {
            if (!otgClearBoard(board)) {
                return error(404, QStringLiteral("no such board"));
            }
            qCInfo(lcAdmin).noquote() << "cleared leaderboard" << board;
            return json(200, QJsonObject{{QStringLiteral("cleared"), board}});
        }
        if (parts.size() == 3 && parts[2] == QLatin1String("entries")) {
            if (m == "GET") {
                const auto entries = otgBoardEntries(board);
                if (!entries) {
                    return error(404, QStringLiteral("no such board"));
                }
                return json(200, *entries);
            }
            if (m == "POST") {
                return setEntry(board, request.body);
            }
            if (m == "DELETE") {
                const QString entrant =
                    request.query.queryItemValue(QStringLiteral("entrant"), QUrl::FullyDecoded);
                if (!otgRemoveEntry(board, entrant)) {
                    return error(404, QStringLiteral("no such entry"));
                }
                qCInfo(lcAdmin).noquote() << "removed" << entrant << "from leaderboard" << board;
                return json(200, QJsonObject{{QStringLiteral("removed"), entrant}});
            }
        }
    }
    if (what == QLatin1String("ghosts") && parts.size() == 3 && parts[2] == QLatin1String("data") &&
        m == "GET") {
        QByteArray data;
        const qint64 id = request.query.queryItemValue(QStringLiteral("id")).toLongLong();
        const auto g = db::ghost(id, &data);
        if (!g) {
            return error(404, QStringLiteral("no such ghost"));
        }
        HttpResponse r;
        r.contentType = "application/octet-stream";
        r.body = data;
        return r;
    }
    if (what == QLatin1String("ghosts") && parts.size() == 2) {
        if (m == "GET") {
            static const QRegularExpression ghostRef(QStringLiteral("(?:^|,)ghost:(\\d+)(?:,|$)"));
            QHash<qint64, QJsonArray> usedOn;
            for (const db::Score& sc : db::scoresWithGhosts()) {
                for (const QJsonValue& a : sc.attachment) {
                    const auto match = ghostRef.match(a.toString());
                    if (match.hasMatch()) {
                        usedOn[match.captured(1).toLongLong()].append(QJsonObject{
                            {QStringLiteral("board"), sc.board},
                            {QStringLiteral("entrant"), sc.entrant},
                            {QStringLiteral("score"), QString::number(sc.score)},
                            {QStringLiteral("updated"), double(sc.updated)},
                        });
                        break;
                    }
                }
            }
            QJsonArray out;
            for (const db::Ghost& g : db::ghosts()) {
                out.append(QJsonObject{
                    {QStringLiteral("id"), QString::number(g.id)},
                    {QStringLiteral("account_id"), QString::number(g.accountId)},
                    {QStringLiteral("name"), egsPlayerName(g.accountId)},
                    {QStringLiteral("mime_type"), g.mimeType},
                    {QStringLiteral("checksum"), g.checksum},
                    {QStringLiteral("size"), g.size},
                    {QStringLiteral("created"), double(g.created)},
                    {QStringLiteral("downloads"), g.downloads},
                    {QStringLiteral("last_download"), double(g.lastDownload)},
                    {QStringLiteral("used_on"), usedOn.value(g.id)},
                });
            }
            return json(200, out);
        }
        if (m == "DELETE") {
            const qint64 id = request.query.queryItemValue(QStringLiteral("id")).toLongLong();
            if (!db::removeGhost(id)) {
                return error(404, QStringLiteral("no such ghost"));
            }
            qCInfo(lcAdmin).noquote() << "removed ghost" << id;
            return json(200, QJsonObject{{QStringLiteral("removed"), QString::number(id)}});
        }
    }
    if (what == QLatin1String("clubs") && parts.size() == 2) {
        if (m == "GET") {
            QHash<qint64, QJsonArray> invitesByClub;
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            for (const db::ClubInvite& i : db::clubInvites()) {
                if (i.status == db::InviteClosed || i.expires <= nowMs) {
                    continue;
                }
                invitesByClub[i.clubId].append(QJsonObject{
                    {QStringLiteral("id"), QString::number(i.id)},
                    {QStringLiteral("account_id"), QString::number(i.accountId)},
                    {QStringLiteral("name"), egsPlayerName(i.accountId)},
                    {QStringLiteral("kind"), i.status == db::InviteRequest
                                                 ? QStringLiteral("join request")
                                                 : QStringLiteral("invite")},
                    {QStringLiteral("invited_by"), egsPlayerName(i.invitedBy)},
                    {QStringLiteral("created"), double(i.created)},
                    {QStringLiteral("expires"), double(i.expires)},
                });
            }
            QJsonArray out;
            for (const db::Club& c : db::clubs()) {
                QJsonArray members;
                QString owner;
                for (const db::ClubMember& mem : db::clubMembers(c.id)) {
                    const QString name = egsPlayerName(mem.accountId);
                    if (mem.type == db::ClubOwner) {
                        owner = name;
                    }
                    members.append(QJsonObject{
                        {QStringLiteral("account_id"), QString::number(mem.accountId)},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("role"), mem.type == db::ClubOwner
                                                     ? QStringLiteral("owner")
                                                     : QStringLiteral("member")},
                        {QStringLiteral("joined"), double(mem.joined)},
                    });
                }
                out.append(QJsonObject{
                    {QStringLiteral("id"), QString::number(c.id)},
                    {QStringLiteral("name"), c.name},
                    {QStringLiteral("membership"), c.membership},
                    {QStringLiteral("status"), c.status},
                    {QStringLiteral("fame"), double(c.xp)},
                    {QStringLiteral("level"), clubLevel(c.xp)}, // 0 without game data
                    {QStringLiteral("created"), double(c.created)},
                    {QStringLiteral("owner"), owner},
                    {QStringLiteral("members"), members},
                    {QStringLiteral("invites"), invitesByClub.value(c.id)},
                    {QStringLiteral("customized"), !c.customization.isEmpty()},
                });
            }
            return json(200, out);
        }
        if (m == "DELETE") {
            const qint64 id = request.query.queryItemValue(QStringLiteral("id")).toLongLong();
            const auto club = db::club(id);
            if (!club || !db::removeClub(id)) {
                return error(404, QStringLiteral("no such club"));
            }
            db::removeScore(QStringLiteral("fame/club"), QString::number(id));
            qCInfo(lcAdmin).noquote() << "removed club" << id << club->name;
            return json(200, QJsonObject{{QStringLiteral("removed"), QString::number(id)}});
        }
    }
    if (what == QLatin1String("clubs") && parts.size() == 3 && parts[2] == QLatin1String("owner") &&
        m == "POST") {
        const qint64 club = request.query.queryItemValue(QStringLiteral("club")).toLongLong();
        const qint64 account = request.query.queryItemValue(QStringLiteral("account")).toLongLong();
        const auto target = db::clubMember(account);
        if (!db::club(club) || !target || target->clubId != club) {
            return error(404, QStringLiteral("not a member of that club"));
        }
        for (db::ClubMember mem : db::clubMembers(club)) {
            const int type = mem.accountId == account ? db::ClubOwner : db::ClubMemberType;
            if (mem.type != type) {
                mem.type = type;
                db::putClubMember(mem);
            }
        }
        qCInfo(lcAdmin).noquote() << "made" << egsPlayerName(account) << "owner of club" << club;
        return json(200, QJsonObject{{QStringLiteral("owner"), QString::number(account)}});
    }
    if (what == QLatin1String("clubs") && parts.size() == 3) {
        const qint64 club = request.query.queryItemValue(QStringLiteral("club")).toLongLong();
        QString failed;
        if (parts[2] == QLatin1String("fame") && m == "POST") {
            const QString xp = request.query.queryItemValue(QStringLiteral("xp"));
            bool ok = false;
            const qint64 value = xp.toLongLong(&ok);
            if (!ok || value < 0) {
                return error(400, QStringLiteral("xp (>= 0) is required"));
            }
            failed = egsSetClubFame(club, value);
        } else if (parts[2] == QLatin1String("rename") && m == "POST") {
            failed = egsRenameClub(
                club, request.query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded));
        } else if (parts[2] == QLatin1String("member") && m == "DELETE") {
            failed = egsRemoveClubMember(
                club, request.query.queryItemValue(QStringLiteral("account")).toLongLong());
        } else {
            return error(404, QStringLiteral("not found"));
        }
        if (!failed.isEmpty()) {
            return error(400, failed);
        }
        return json(200, QJsonObject{{QStringLiteral("club"), QString::number(club)}});
    }
    if (what == QLatin1String("parties") && m == "DELETE") {
        QString failed;
        if (parts.size() == 2) {
            failed = egsDisbandParty(request.query.queryItemValue(QStringLiteral("id")));
        } else if (parts.size() == 3 && parts[2] == QLatin1String("member")) {
            failed = egsKickFromParty(
                request.query.queryItemValue(QStringLiteral("party")),
                request.query.queryItemValue(QStringLiteral("account")).toLongLong());
        } else {
            return error(404, QStringLiteral("not found"));
        }
        if (!failed.isEmpty()) {
            return error(400, failed);
        }
        return json(200, QJsonObject{{QStringLiteral("done"), true}});
    }
    if (what == QLatin1String("players") && parts.size() == 3 &&
        parts[2] == QLatin1String("detail") && m == "GET") {
        return playerDetail(request.query.queryItemValue(QStringLiteral("account")).toLongLong());
    }
    if (what == QLatin1String("gamedata") && parts.size() == 2 && m == "GET") {
        const GameData& g = gameData();
        QJsonArray tracks;
        for (const GameTrack& t : g.tracks) {
            tracks.append(QJsonObject{{QStringLiteral("id"), t.id},
                                      {QStringLiteral("code"), t.code},
                                      {QStringLiteral("name"), t.name},
                                      {QStringLiteral("location"), t.location},
                                      {QStringLiteral("route"), t.routeType}});
        }
        QJsonArray vehicles;
        for (const GameVehicle& v : g.vehicles) {
            vehicles.append(QJsonObject{{QStringLiteral("id"), v.id},
                                        {QStringLiteral("code"), v.code},
                                        {QStringLiteral("name"), v.name},
                                        {QStringLiteral("class"), v.vehicleClass},
                                        {QStringLiteral("player_unlock"), v.playerUnlockLevel},
                                        {QStringLiteral("club_unlock"), v.clubUnlockLevel}});
        }
        auto numbers = [](const QList<qint64>& list) {
            QJsonArray a;
            for (const qint64 x : list) {
                a.append(double(x));
            }
            return a;
        };
        QJsonArray caps;
        for (const int c : g.playerCaps) {
            caps.append(c);
        }
        return json(200, QJsonObject{{QStringLiteral("dir"), g.dir},
                                     {QStringLiteral("tracks"), tracks},
                                     {QStringLiteral("vehicles"), vehicles},
                                     {QStringLiteral("player_levels"), numbers(g.playerLevels)},
                                     {QStringLiteral("player_caps"), caps},
                                     {QStringLiteral("club_levels"), numbers(g.clubLevels)},
                                     {QStringLiteral("elite_levels"), numbers(g.eliteLevels)}});
    }
    if (what == QLatin1String("activity") && parts.size() == 2 && m == "GET") {
        return activity();
    }
    // Parties (lobbyService) as they are now.
    if (what == QLatin1String("parties") && parts.size() == 2 && m == "GET") {
        QJsonArray out;
        for (const EgsPartyInfo& p : egsParties()) {
            QJsonArray members;
            for (const EgsPartyMember& mem : p.members) {
                members.append(
                    QJsonObject{{QStringLiteral("account_id"), QString::number(mem.accountId)},
                                {QStringLiteral("name"), mem.name},
                                {QStringLiteral("host"), mem.host}});
            }
            out.append(QJsonObject{{QStringLiteral("id"), p.id},
                                   {QStringLiteral("type"), p.type},
                                   {QStringLiteral("status"), p.status},
                                   {QStringLiteral("members"), members}});
        }
        return json(200, out);
    }
    if (what == QLatin1String("club_reports") && parts.size() == 2) {
        if (m == "GET") {
            QJsonArray out;
            for (const db::ClubReport& r : db::clubReports()) {
                const auto club = db::club(r.clubId);
                out.append(QJsonObject{
                    {QStringLiteral("id"), QString::number(r.id)},
                    {QStringLiteral("club_id"), QString::number(r.clubId)},
                    {QStringLiteral("club_name"), r.clubName},
                    {QStringLiteral("club_now"), club ? club->name : QString()},
                    {QStringLiteral("club_exists"), bool(club)},
                    {QStringLiteral("reporter_id"), QString::number(r.reporter)},
                    {QStringLiteral("reporter"), egsPlayerName(r.reporter)},
                    {QStringLiteral("reason"), double(r.reason)},
                    {QStringLiteral("language"), double(r.language)},
                    {QStringLiteral("created"), double(r.created)},
                });
            }
            return json(200, out);
        }
        if (m == "DELETE") {
            const qint64 id = request.query.queryItemValue(QStringLiteral("id")).toLongLong();
            if (!db::removeClubReport(id)) {
                return error(404, QStringLiteral("no such report"));
            }
            qCInfo(lcAdmin).noquote() << "dismissed club report" << id;
            return json(200, QJsonObject{{QStringLiteral("removed"), QString::number(id)}});
        }
    }
    if (what == QLatin1String("challenges") && parts.size() == 2 && m == "GET") {
        return json(200, otgChallenges());
    }
    if (what == QLatin1String("challenges") && parts.size() == 3 &&
        parts.at(2) == QLatin1String("feature")) {
        if (m == "POST") {
            const QJsonObject o = QJsonDocument::fromJson(request.body).object();
            QString why;
            if (!otgFeatureChallenge(o.value(QStringLiteral("id")).toString(),
                                     o.value(QStringLiteral("list")).toString(),
                                     o.value(QStringLiteral("extend_days")).toInt(), &why)) {
                return error(why == QLatin1String("no such challenge") ? 404 : 400, why);
            }
            return json(200, QJsonObject{{QStringLiteral("featured"),
                                          o.value(QStringLiteral("id")).toString()}});
        }
        if (m == "DELETE") {
            const QString id = request.query.queryItemValue(QStringLiteral("id"));
            const QString list = request.query.queryItemValue(QStringLiteral("list"));
            if (!otgUnfeatureChallenge(id, list)) {
                return error(404, QStringLiteral("not featured"));
            }
            return json(200, QJsonObject{{QStringLiteral("unfeatured"), id}});
        }
    }
    if (what == QLatin1String("gamestates") && parts.size() == 2) {
        if (m == "GET") {
            return json(200, otgGameStates());
        }
        if (m == "DELETE") {
            const QString resource =
                request.query.queryItemValue(QStringLiteral("resource"), QUrl::FullyDecoded);
            if (!otgRemoveGameState(resource)) {
                return error(404, QStringLiteral("no such gamestate"));
            }
            qCInfo(lcAdmin).noquote() << "removed gamestate" << resource;
            return json(200, QJsonObject{{QStringLiteral("removed"), resource}});
        }
    }
    if (what == QLatin1String("stats") && parts.size() == 2 && m == "GET") {
        static const QRegularExpression statsResource(
            QStringLiteral("^stats/V\\d+/player/(\\d+)$"));
        QJsonArray out;
        for (const db::GameState& g : db::gameStates()) {
            const auto match = statsResource.match(g.resource);
            if (!match.hasMatch()) {
                continue;
            }
            const qint64 account = match.captured(1).toLongLong();
            out.append(QJsonObject{{QStringLiteral("account_id"), QString::number(account)},
                                   {QStringLiteral("name"), egsPlayerName(account)},
                                   {QStringLiteral("counters"), g.counts.size()},
                                   {QStringLiteral("version"), g.version},
                                   {QStringLiteral("updated"), double(g.updated)}});
        }
        for (const qint64 account : db::accoladeAccounts()) {
            bool listed = false;
            for (const QJsonValue& v : out) {
                listed |= v.toObject().value(QStringLiteral("account_id")).toString() ==
                          QString::number(account);
            }
            if (!listed) {
                out.append(QJsonObject{{QStringLiteral("account_id"), QString::number(account)},
                                       {QStringLiteral("name"), egsPlayerName(account)},
                                       {QStringLiteral("counters"), 0},
                                       {QStringLiteral("version"), 0},
                                       {QStringLiteral("updated"), 0.0}});
            }
        }
        for (const db::Club& c : db::clubs()) {
            int counters = 0;
            qint64 updated = 0;
            for (const db::ClubMember& mem : db::clubMembers(c.id)) {
                if (const auto g =
                        db::gameState(QStringLiteral("stats/V06/player/%1").arg(mem.accountId))) {
                    counters += int(g->counts.size());
                    updated = std::max(updated, g->updated);
                }
            }
            out.append(QJsonObject{{QStringLiteral("club_id"), QString::number(c.id)},
                                   {QStringLiteral("account_id"), QString()},
                                   {QStringLiteral("name"), QStringLiteral("club %1").arg(c.name)},
                                   {QStringLiteral("counters"), counters},
                                   {QStringLiteral("version"), 0},
                                   {QStringLiteral("updated"), double(updated)}});
        }
        QJsonArray sets;
        for (const QString& s : egsStatSets()) {
            sets.append(s);
        }
        sets.append(QStringLiteral("Accolades"));
        return json(200,
                    QJsonObject{{QStringLiteral("players"), out}, {QStringLiteral("sets"), sets}});
    }
    if (what == QLatin1String("stats") && parts.size() == 3 && parts[2] == QLatin1String("table") &&
        m == "GET") {
        const qint64 account = request.query.queryItemValue(QStringLiteral("account")).toLongLong();
        const qint64 club = request.query.queryItemValue(QStringLiteral("club")).toLongLong();
        const QString set = request.query.queryItemValue(QStringLiteral("set"));
        if (set == QLatin1String("Accolades")) {
            QJsonArray rows;
            const QList<db::Accolade> list =
                club ? db::clubAccolades(club) : db::accolades(account);
            for (const db::Accolade& a : list) {
                rows.append(QJsonObject{{QStringLiteral("name"), a.name},
                                        {QStringLiteral("values"), QJsonArray{a.progress}}});
            }
            return json(200, QJsonObject{{QStringLiteral("columns"),
                                          QJsonArray{QStringLiteral("Progress")}},
                                         {QStringLiteral("rows"), rows},
                                         {QStringLiteral("stored"), !list.isEmpty()}});
        }
        const EgsStatsTable t = club ? egsClubStats(club, set) : egsStats(account, set);
        if (!t.known) {
            return error(404, QStringLiteral("unknown stats set"));
        }
        QJsonArray rows;
        for (const auto& [name, values] : t.rows) {
            QJsonArray v;
            for (const qint64 x : values) {
                v.append(x);
            }
            rows.append(QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("values"), v}});
        }
        return json(200,
                    QJsonObject{{QStringLiteral("columns"), QJsonArray::fromStringList(t.columns)},
                                {QStringLiteral("rows"), rows},
                                {QStringLiteral("stored"), t.stored}});
    }
    if (what == QLatin1String("players") && parts.size() == 2 && m == "GET") {
        QJsonArray out;
        const QSet<qint64> online = egsOnlineAccounts();
        const QHash<qint64, qint64> polls = egsLastPolls();
        QHash<qint64, QPair<QString, QStringList>> partyOf; // account -> (party id, member names)
        for (const EgsPartyInfo& party : egsParties()) {
            QStringList names;
            for (const EgsPartyMember& mem : party.members) {
                names << (mem.host ? mem.name + QStringLiteral(" (leader)") : mem.name);
            }
            for (const EgsPartyMember& mem : party.members) {
                partyOf.insert(mem.accountId, qMakePair(party.id, names));
            }
        }
        for (const db::Player& p : db::players()) {
            QJsonObject o{{QStringLiteral("account_id"), QString::number(p.accountId)},
                          {QStringLiteral("name"), p.name},
                          {QStringLiteral("logged_in"), p.loggedIn},
                          {QStringLiteral("shadnet_account"), p.shadnetAccount},
                          {QStringLiteral("first_seen"), double(p.firstSeen)},
                          {QStringLiteral("last_seen"), double(p.lastSeen)}};
            o.insert(QStringLiteral("friends"),
                     QJsonArray::fromStringList(db::friendNames(p.accountId)));
            o.insert(QStringLiteral("online"), online.contains(p.accountId));
            if (polls.contains(p.accountId)) {
                o.insert(QStringLiteral("last_poll"), double(polls.value(p.accountId)));
            }
            if (partyOf.contains(p.accountId)) {
                o.insert(QStringLiteral("party_id"), partyOf.value(p.accountId).first);
                o.insert(QStringLiteral("party"),
                         QJsonArray::fromStringList(partyOf.value(p.accountId).second));
            }
            if (const auto f = db::fame(p.accountId)) {
                o.insert(QStringLiteral("fame"), f->xp);
                o.insert(QStringLiteral("level"), driverLevel(f->xp));      // 0 without game data
                o.insert(QStringLiteral("elite_level"), eliteLevel(f->xp)); // 0 below elite
                o.insert(QStringLiteral("fame_updated"), double(f->updated));
                if (f->stars) {
                    o.insert(QStringLiteral("stars"), *f->stars);
                }
                if (f->elite) {
                    o.insert(QStringLiteral("elite"), *f->elite);
                }
            }
            out.append(o);
        }
        return json(200, out);
    }
    if (what == QLatin1String("players") && parts.size() == 3 &&
        parts[2] == QLatin1String("fame") && (m == "POST" || m == "PUT")) {
        const QJsonObject o = QJsonDocument::fromJson(request.body).object();
        const qint64 account = o.value(QStringLiteral("account_id")).toVariant().toLongLong();
        auto number = [&o](const char* key) -> std::optional<qint64> {
            const QJsonValue v = o.value(QLatin1String(key));
            if (v.isUndefined() || v.isNull()) {
                return std::nullopt;
            }
            return v.toVariant().toLongLong();
        };
        const auto xp = number("xp");
        const auto stars = number("stars");
        const auto elite = number("elite");
        if (account <= 0 || (!xp && !stars && !elite) || (xp && *xp < 0) || (stars && *stars < 0) ||
            (elite && *elite < 0)) {
            return error(400, QStringLiteral("account_id and at least one of xp, stars, elite "
                                             "(each >= 0) are required"));
        }
        if (xp) {
            egsSetFame(account, *xp);
        }
        if (stars || elite) {
            egsSetStarsElite(account, stars, elite);
        }
        QJsonObject out{{QStringLiteral("account_id"), QString::number(account)}};
        if (const auto f = db::fame(account)) {
            out.insert(QStringLiteral("fame"), f->xp);
            if (f->stars) {
                out.insert(QStringLiteral("stars"), *f->stars);
            }
            if (f->elite) {
                out.insert(QStringLiteral("elite"), *f->elite);
            }
        }
        return json(200, out);
    }
    if (what == QLatin1String("requests") && parts.size() == 2 && m == "GET") {
        return requests(request.query.queryItemValue(QStringLiteral("after")).toULongLong());
    }
    return error(404, QStringLiteral("not found"));
}

HttpResponse AdminService::setEntry(const QString& board, const QByteArray& body) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (!doc.isObject()) {
        return error(400, QStringLiteral("body must be a JSON object"));
    }
    const QJsonObject o = doc.object();
    const QString entrant = o.value(QStringLiteral("entrant")).toVariant().toString().trimmed();
    if (board.isEmpty() || entrant.isEmpty()) {
        return error(400, QStringLiteral("board and entrant are required"));
    }
    quint64 score = 0;
    bool ok = false;
    if (o.contains(QStringLiteral("time_ms"))) {
        const qint64 ms = o.value(QStringLiteral("time_ms")).toInteger(-1);
        ok = ms >= 0 && quint64(ms) <= OtgTimeMask;
        score = otgTimeScore(quint64(ms));
    } else if (o.contains(QStringLiteral("points"))) {
        const QJsonValue v = o.value(QStringLiteral("points"));
        const qint64 n = v.isString() ? v.toString().trimmed().toLongLong(&ok) : v.toInteger(-1);
        ok = (v.isString() ? ok : true) && n >= 0 && quint64(n) <= OtgTimeMask;
        score = otgPointsScore(quint64(n));
    } else {
        const QJsonValue v = o.value(QStringLiteral("score"));
        if (v.isString()) {
            score = v.toString().trimmed().toULongLong(&ok);
        } else if (v.isDouble()) {
            const qint64 n = v.toInteger(-1);
            ok = n >= 0;
            score = quint64(n);
        }
    }
    if (!ok) {
        return error(400,
                     QStringLiteral("score (unsigned integer), time_ms or points is required"));
    }
    QJsonArray attachment;
    const QJsonValue a = o.value(QStringLiteral("attachment"));
    if (a.isArray()) {
        attachment = a.toArray();
    } else {
        attachment.append(a.isString() ? a.toString() : QString());
    }
    if (!otgSetEntry(board, entrant, score, attachment)) {
        return error(400, QStringLiteral("cannot set entry"));
    }
    const QString name = o.value(QStringLiteral("name")).toString().trimmed();
    if (!name.isEmpty() && entrant.toLongLong() != 0) {
        egsSetPlayerName(entrant.toLongLong(), name);
    }
    qCInfo(lcAdmin).noquote() << "set" << entrant << "on leaderboard" << board << "score" << score;
    return json(200, QJsonObject{{QStringLiteral("board"), board},
                                 {QStringLiteral("entrant"), entrant},
                                 {QStringLiteral("score"), QString::number(score)}});
}

HttpResponse AdminService::status() {
    const AuthConfig& auth = authConfig();
    const QList<RequestRecord> recent = recentRequests();
    const QList<db::Ghost> ghosts = db::ghosts();
    qint64 ghostBytes = 0;
    for (const db::Ghost& g : ghosts) {
        ghostBytes += g.size;
    }
    return json(
        200, QJsonObject{
                 {QStringLiteral("version"), Driveclub::Version()},
                 {QStringLiteral("build_date"), Driveclub::BuildDate()},
                 {QStringLiteral("build_timestamp"), Driveclub::BuildTimestamp()},
                 {QStringLiteral("uptime"), uptime_.elapsed() / 1000},
                 {QStringLiteral("listen"),
                  QStringLiteral("%1:%2").arg(config_.address.toString()).arg(config_.port)},
                 {QStringLiteral("server_identifier"), config_.egs.serverIdentifier},
                 {QStringLiteral("otg_zone"), config_.otg.zone},
                 {QStringLiteral("otg_domain_id"), config_.otg.domainId},
                 {QStringLiteral("shadnet_url"), auth.shadnetUrl},
                 {QStringLiteral("shadnet_client_id"), auth.clientId},
                 {QStringLiteral("allow_unverified"), auth.allowUnverified},
                 {QStringLiteral("sessions"), qint64(egsSessions().size())},
                 {QStringLiteral("parties"), qint64(egsParties().size())},
                 {QStringLiteral("game_data"),
                  QJsonObject{
                      {QStringLiteral("dir"), gameData().dir},
                      {QStringLiteral("loaded"), QJsonArray::fromStringList(gameData().loaded)},
                      {QStringLiteral("missing"), QJsonArray::fromStringList(gameData().missing)}}},
                 {QStringLiteral("online"), qint64(egsOnlineAccounts().size())},
                 {QStringLiteral("otg_tokens"), qint64(otgTokens().size())},
                 {QStringLiteral("leaderboards"), qint64(otgBoardList().size())},
                 {QStringLiteral("database"), db::path()},
                 {QStringLiteral("players"), qint64(egsPlayers().size())},
                 {QStringLiteral("ghosts"), qint64(ghosts.size())},
                 {QStringLiteral("ghost_bytes"), double(ghostBytes)},
                 {QStringLiteral("requests"), recent.isEmpty() ? 0.0 : double(recent.last().id)},
             });
}

HttpResponse AdminService::playerDetail(qint64 account) {
    const auto player = db::player(account);
    if (account <= 0 || !player) {
        return error(404, QStringLiteral("no such player"));
    }
    const QString id = QString::number(account);
    QJsonObject out{{QStringLiteral("account_id"), id},
                    {QStringLiteral("name"), player->name},
                    {QStringLiteral("logged_in"), player->loggedIn},
                    {QStringLiteral("shadnet_account"), player->shadnetAccount},
                    {QStringLiteral("first_seen"), double(player->firstSeen)},
                    {QStringLiteral("last_seen"), double(player->lastSeen)},
                    {QStringLiteral("online"), egsOnlineAccounts().contains(account)}};
    if (const qint64 poll = egsLastPolls().value(account)) {
        out.insert(QStringLiteral("last_poll"), double(poll));
    }
    if (const auto f = db::fame(account)) {
        QJsonObject fame{{QStringLiteral("xp"), f->xp},
                         {QStringLiteral("updated"), double(f->updated)},
                         {QStringLiteral("level"), driverLevel(f->xp)},
                         {QStringLiteral("elite_level"), eliteLevel(f->xp)}};
        if (f->stars) {
            fame.insert(QStringLiteral("stars"), *f->stars);
        }
        if (f->elite) {
            fame.insert(QStringLiteral("elite"), *f->elite);
        }
        out.insert(QStringLiteral("fame"), fame);
    }
    if (const auto m = db::clubMember(account)) {
        const auto club = db::club(m->clubId);
        out.insert(QStringLiteral("club"),
                   QJsonObject{{QStringLiteral("id"), QString::number(m->clubId)},
                               {QStringLiteral("name"), club ? club->name : QString()},
                               {QStringLiteral("owner"), m->type == db::ClubOwner},
                               {QStringLiteral("fame"), club ? double(club->xp) : 0.0},
                               {QStringLiteral("level"), club ? clubLevel(club->xp) : 0},
                               {QStringLiteral("joined"), double(m->joined)}});
    }
    for (const EgsPartyInfo& p : egsParties()) {
        QStringList names;
        bool in = false;
        for (const EgsPartyMember& mem : p.members) {
            names << (mem.host ? mem.name + QStringLiteral(" (leader)") : mem.name);
            in = in || mem.accountId == account;
        }
        if (in) {
            out.insert(QStringLiteral("party"),
                       QJsonObject{{QStringLiteral("id"), p.id},
                                   {QStringLiteral("members"), QJsonArray::fromStringList(names)}});
        }
    }
    out.insert(QStringLiteral("friends"), QJsonArray::fromStringList(db::friendNames(account)));
    QJsonArray accolades;
    for (const db::Accolade& a : db::accolades(account)) {
        accolades.append(QJsonObject{{QStringLiteral("name"), a.name},
                                     {QStringLiteral("progress"), double(a.progress)},
                                     {QStringLiteral("updated"), double(a.updated)}});
    }
    out.insert(QStringLiteral("accolades"), accolades);
    QJsonArray scores;
    for (const db::EntrantScore& e : db::entrantScores(id)) {
        scores.append(QJsonObject{{QStringLiteral("board"), e.score.board},
                                  {QStringLiteral("score"), QString::number(e.score.score)},
                                  {QStringLiteral("rank"), double(e.rank)},
                                  {QStringLiteral("size"), double(e.boardSize)},
                                  {QStringLiteral("updated"), double(e.score.updated)},
                                  {QStringLiteral("attachment"), e.score.attachment}});
    }
    out.insert(QStringLiteral("scores"), scores);
    QJsonArray challenges;
    for (const db::Score& c : db::boardScores(QStringLiteral("challenges/player/") + id)) {
        challenges.append(QJsonObject{{QStringLiteral("id"), c.entrant},
                                      {QStringLiteral("ends"), double(c.score >> 8)},
                                      {QStringLiteral("created_by_them"), (c.score & 0xff) == 1},
                                      {QStringLiteral("updated"), double(c.updated)}});
    }
    out.insert(QStringLiteral("challenges"), challenges);
    QJsonArray ghosts;
    for (const db::Ghost& g : db::ghosts()) {
        if (g.accountId == account) {
            ghosts.append(QJsonObject{{QStringLiteral("id"), QString::number(g.id)},
                                      {QStringLiteral("size"), double(g.size)},
                                      {QStringLiteral("created"), double(g.created)},
                                      {QStringLiteral("downloads"), g.downloads}});
        }
    }
    out.insert(QStringLiteral("ghosts"), ghosts);
    int sessionCount = 0;
    for (const EgsSessionInfo& si : egsSessions()) {
        sessionCount += si.accountId == account ? 1 : 0;
    }
    out.insert(QStringLiteral("sessions"), sessionCount);
    return json(200, out);
}

HttpResponse AdminService::activity() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto counts = [](const db::Activity& a) {
        return QJsonObject{{QStringLiteral("new_players"), double(a.newPlayers)},
                           {QStringLiteral("logins"), double(a.logins)},
                           {QStringLiteral("races"), double(a.races)},
                           {QStringLiteral("score_posts"), double(a.scorePosts)},
                           {QStringLiteral("challenges"), double(a.challenges)},
                           {QStringLiteral("clubs_created"), double(a.clubsCreated)},
                           {QStringLiteral("ghosts"), double(a.ghosts)}};
    };
    int requests = 0;
    int errors = 0;
    for (const RequestRecord& r : recentRequests()) {
        if (r.time >= now - 3600 * 1000) {
            ++requests;
            errors += (r.status >= 400 && !r.expected) ? 1 : 0;
        }
    }
    QJsonArray warnings;
    for (const LoggedWarning& w : recentWarnings()) {
        warnings.append(QJsonObject{{QStringLiteral("time"), double(w.time)},
                                    {QStringLiteral("category"), w.category},
                                    {QStringLiteral("message"), w.message}});
    }
    return json(
        200, QJsonObject{{QStringLiteral("day"), counts(db::activitySince(now - 24 * 3600 * 1000))},
                         {QStringLiteral("hour"), counts(db::activitySince(now - 3600 * 1000))},
                         {QStringLiteral("requests_hour"), requests},
                         {QStringLiteral("errors_hour"), errors},
                         {QStringLiteral("warnings"), warnings}});
}

HttpResponse AdminService::sessions() {
    QJsonArray out;
    for (const EgsSessionInfo& s : egsSessions()) {
        out.append(QJsonObject{
            {QStringLiteral("key"), s.key},
            {QStringLiteral("name"), s.name},
            {QStringLiteral("account_id"), s.accountId},
            {QStringLiteral("shadnet_account"), s.shadnetAccount},
            {QStringLiteral("verified"), !s.shadnetAccount.isEmpty()},
            {QStringLiteral("client_version"), s.clientVersion},
            {QStringLiteral("ps_plus"), s.psPlus},
            {QStringLiteral("created"), double(s.created)},
            {QStringLiteral("last_seen"), double(s.lastSeen)},
            {QStringLiteral("requests"), s.requests},
            {QStringLiteral("transaction"), s.transaction},
        });
    }
    return json(200, out);
}

HttpResponse AdminService::tokens() {
    QJsonArray out;
    for (const OtgTokenInfo& t : otgTokens()) {
        out.append(QJsonObject{
            {QStringLiteral("token"), t.token},
            {QStringLiteral("name"), t.name},
            {QStringLiteral("shadnet_account"), t.account},
            {QStringLiteral("verified"), !t.account.isEmpty()},
            {QStringLiteral("created"), double(t.created)},
            {QStringLiteral("last_used"), double(t.lastUsed)},
            {QStringLiteral("requests"), t.requests},
        });
    }
    return json(200, out);
}

HttpResponse AdminService::requests(quint64 after) {
    QJsonArray out;
    for (const RequestRecord& r : recentRequests(after)) {
        out.append(QJsonObject{
            {QStringLiteral("id"), double(r.id)},
            {QStringLiteral("time"), double(r.time)},
            {QStringLiteral("method"), r.method},
            {QStringLiteral("path"), r.path},
            {QStringLiteral("label"), r.label},
            {QStringLiteral("peer"), r.peer},
            {QStringLiteral("status"), r.status},
            {QStringLiteral("ms"), double(r.elapsedMs)},
            {QStringLiteral("bytes_in"), double(r.bytesIn)},
            {QStringLiteral("bytes_out"), double(r.bytesOut)},
            {QStringLiteral("expected"), r.expected},
        });
    }
    return json(200, out);
}
