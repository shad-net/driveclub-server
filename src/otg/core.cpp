// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// OTG: Config, tokens (login), base URL, the caller's entrant id.

#include "otg/internal.h"

namespace otg {

Q_LOGGING_CATEGORY(lcOtg, "otg")

OtgConfig& config() {
    static OtgConfig c;
    return c;
}

QString baseUrl(const HttpRequest& request) {
    if (!config().baseUrl.isEmpty()) {
        return config().baseUrl;
    }
    const QByteArray host = request.headers.value("host");
    if (!host.isEmpty()) {
        return QStringLiteral("http://") + QString::fromLatin1(host);
    }
    return QStringLiteral("http://127.0.0.1:%1").arg(config().listenPort);
}

HttpResponse json(const QJsonDocument& doc, int status) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json";
    r.body = doc.toJson(QJsonDocument::Compact);
    return r;
}

HttpResponse login(const HttpRequest& request, const QString& who, const AuthResult& auth) {
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool verified = auth.outcome == AuthResult::Outcome::Verified;
    Token t;
    t.name = verified ? auth.onlineId : who;
    t.account = verified ? auth.accountId : QString();
    t.created = QDateTime::currentMSecsSinceEpoch();
    t.lastUsed = t.created;
    {
        QMutexLocker lock(&tokens().mutex);
        tokens().byToken.insert(token, t);
    }
    db::putToken({token, t.name, t.account, t.created, t.lastUsed, 0});
    const QString base = baseUrl(request);
    qCInfo(lcOtg).noquote() << "OTG login" << t.name
                            << (verified ? "(shadNet verified)" : "(not verified)") << "-> token"
                            << token << "gateway" << base;
    return json(QJsonDocument(QJsonObject{
        {QStringLiteral("token"), token},
        {QStringLiteral("config"), QJsonObject{{QStringLiteral("gateway"), base}}},
        {QStringLiteral("zone"), config().zone},
    }));
}

QString tokenOwner(const HttpRequest& request) {
    QByteArray value = request.headers.value("authorization");
    if (!value.startsWith("Bearer ")) {
        return {};
    }
    const QString token = QString::fromLatin1(value.mid(7)).trimmed();
    QMutexLocker lock(&tokens().mutex);
    auto it = tokens().byToken.find(token);
    if (it == tokens().byToken.end()) {
        return {};
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Saved at most every 10 s per token; the in-memory copy is always current.
    const bool save = now - it->lastUsed >= 10000;
    it->lastUsed = now;
    ++it->requests;
    if (save) {
        db::touchToken(token, it->lastUsed, it->requests);
    }
    return it->name;
}

bool isClubBoard(const QString& board) {
    return board.startsWith(QLatin1String("challenges/club/")) ||
           board == QLatin1String("fame/club");
}

QString callerEntrant(const HttpRequest& request, const QString& board) {
    const QByteArray value = request.headers.value("authorization");
    if (!value.startsWith("Bearer ")) {
        return {};
    }
    QString name;
    {
        QMutexLocker lock(&tokens().mutex);
        const auto it = tokens().byToken.constFind(QString::fromLatin1(value.mid(7)).trimmed());
        if (it == tokens().byToken.constEnd()) {
            return {};
        }
        name = it->name;
    }
    const auto player = db::playerByName(name);
    if (!player) {
        return {};
    }
    if (isClubBoard(board)) {
        const auto member = db::clubMember(player->accountId);
        return member ? QString::number(member->clubId) : QString();
    }
    return QString::number(player->accountId);
}

} // namespace otg
