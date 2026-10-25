// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "admin_service.h"

#include "auth.h"
#include "database.h"
#include "egs.h"
#include "otg.h"
#include "request_log.h"
#include "version.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

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
    if (what == QLatin1String("players") && parts.size() == 2 && m == "GET") {
        QJsonArray out;
        for (const EgsPlayerInfo& p : egsPlayers()) {
            out.append(QJsonObject{{QStringLiteral("account_id"), QString::number(p.accountId)},
                                   {QStringLiteral("name"), p.name},
                                   {QStringLiteral("logged_in"), p.loggedIn}});
        }
        return json(200, out);
    }
    if (what == QLatin1String("requests") && parts.size() == 2 && m == "GET") {
        return requests(request.query.queryItemValue(QStringLiteral("after")).toULongLong());
    }
    return error(404, QStringLiteral("not found"));
}

// POST /api/leaderboards/entries?board=<name>
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
    // Optional display name, answered when the game asks profileService for this account.
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
    return json(200,
                QJsonObject{
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
                    {QStringLiteral("otg_tokens"), qint64(otgTokens().size())},
                    {QStringLiteral("leaderboards"), qint64(otgBoardList().size())},
                    {QStringLiteral("database"), db::path()},
                    {QStringLiteral("players"), qint64(egsPlayers().size())},
                    {QStringLiteral("requests"), recent.isEmpty() ? 0.0 : double(recent.last().id)},
                });
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
        });
    }
    return json(200, out);
}
