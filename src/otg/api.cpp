// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// OTG: the request handler and token functions of otg/otg.h.

#include "otg/internal.h"

using namespace otg;

void setOtgConfig(const OtgConfig& c) {
    config() = c;
}

QString otgBaseUrl(const HttpRequest& request) {
    return baseUrl(request);
}

bool handleOtg(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond) {
    static const QRegularExpression ghostPath(QStringLiteral("^ghosts/(\\d+)$"));
    if (const auto g = ghostPath.match(endpoint); g.hasMatch()) {
        QByteArray data;
        const qint64 id = g.captured(1).toLongLong();
        const auto ghost = request.method == "GET" ? db::ghost(id, &data) : std::nullopt;
        HttpResponse r;
        if (!ghost) {
            qCWarning(lcOtg).noquote() << "ghost" << id << "requested but not stored";
            r.status = 404;
            r.contentType = "text/plain";
            r.body = "Not Found";
        } else {
            db::countGhostDownload(id);
            qCInfo(lcOtg).noquote() << "ghost" << id << "of" << egsPlayerName(ghost->accountId)
                                    << "downloaded," << data.size() << "bytes";
            r.contentType = ghost->mimeType.isEmpty() ? QByteArray("application/octet-stream")
                                                      : ghost->mimeType.toLatin1();
            r.body = data;
        }
        respond(r);
        return true;
    }
    if (endpoint == QLatin1String("titles/driveclub/domains")) {
        const QString base = baseUrl(request);
        qCInfo(lcOtg).noquote() << "domain list: id" << config().domainId << "auth" << base;
        respond(json(QJsonDocument(QJsonArray{QJsonObject{
            {QStringLiteral("id"), config().domainId},
            {QStringLiteral("auth"), base},
        }})));
        return true;
    }
    if (endpoint == QLatin1String("login/driveclub")) {
        // provider=psn&code=<NpAuth authorization code>, checked with shadNet (auth.cpp).
        const QString code = request.query.queryItemValue(QStringLiteral("code"));
        verifyCode(QStringLiteral("OTG"), code, [request, respond](const AuthResult& auth) {
            if (auth.outcome == AuthResult::Outcome::Denied) {
                qCWarning(lcOtg).noquote() << "OTG login denied:" << auth.message;
                respond(json(QJsonDocument(QJsonObject{{QStringLiteral("error"), auth.message}}),
                             auth.unavailable ? 503 : 401));
                return;
            }
            respond(login(request, QStringLiteral("psn"), auth));
        });
        return true;
    }
    if (endpoint == QLatin1String("xlogin/driveclub")) {
        // OTG_Login_Type "fake": no code to check. Refused when unverified logins are not allowed.
        if (!authConfig().shadnetUrl.isEmpty() && !authConfig().allowUnverified) {
            qCWarning(lcOtg) << "fake OTG login refused (shadnet.allow_unverified is off)";
            respond(json(QJsonDocument(QJsonObject{
                             {QStringLiteral("error"), QStringLiteral("fake login not allowed")}}),
                         401));
            return true;
        }
        respond(login(request, request.query.queryItemValue(QStringLiteral("user")), {}));
        return true;
    }

    static const QRegularExpression service(QStringLiteral("^([^/]+)/([^/]+)/(v\\d+)/(.+)$"));
    const auto m = service.match(endpoint);
    if (!m.hasMatch()) {
        return false;
    }
    const QString resource = m.captured(4);
    const QString board = boardName(resource);
    if (tokenOwner(request).isEmpty()) {
        qCWarning(lcOtg).noquote()
            << request.method << resource << "with an unknown token (server restarted?)";
    }
    if (resource.endsWith(QLatin1String("/~gamestate"))) {
        respond(handleGameState(request, resource.chopped(11)));
        return true;
    }
    const bool isBoard = resource.endsWith(QLatin1String("/~leaderboard"));
    if (request.method == "GET" && isBoard) {
        respond(readBoard(request, board));
        return true;
    }
    if (request.method == "POST" || request.method == "PUT") {
        const QJsonDocument doc = QJsonDocument::fromJson(request.body);
        const QJsonArray arr = doc.isArray() ? doc.array() : QJsonArray{};
        if (!arr.isEmpty() && arr.first().toObject().contains(QStringLiteral("score"))) {
            respond(writeBoard(request, board, isChallengeList(resource)));
            return true;
        }
    }
    qCWarning(lcOtg).noquote() << "no OTG handler for" << request.method << resource << "(zone"
                               << m.captured(1) << ", server" << m.captured(2) << ")";
    HttpResponse r;
    r.status = 404;
    r.contentType = "application/json";
    r.body = "{}";
    respond(r);
    return true;
}

QList<OtgTokenInfo> otgTokens() {
    QMutexLocker lock(&tokens().mutex);
    QList<OtgTokenInfo> out;
    for (auto it = tokens().byToken.constBegin(); it != tokens().byToken.constEnd(); ++it) {
        out.append({it.key(), it->name, it->account, it->created, it->lastUsed, it->requests});
    }
    return out;
}

bool otgDropToken(const QString& token) {
    db::removeToken(token);
    QMutexLocker lock(&tokens().mutex);
    return tokens().byToken.remove(token) > 0;
}

void otgRestoreTokens() {
    QMutexLocker lock(&tokens().mutex);
    for (const db::Token& t : db::tokens()) {
        tokens().byToken.insert(t.token, {t.name, t.account, t.created, t.lastUsed, t.requests});
    }
    if (!tokens().byToken.isEmpty()) {
        qCInfo(lcOtg).noquote() << "restored" << tokens().byToken.size() << "OTG tokens";
    }
}
