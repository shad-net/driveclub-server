// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "otg.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QUuid>

namespace {

Q_LOGGING_CATEGORY(lcOtg, "otg")

OtgConfig& config() {
    static OtgConfig c;
    return c;
}

struct Tokens {
    QMutex mutex;
    QHash<QString, QString> nameByToken;
};

Tokens& tokens() {
    static Tokens t;
    return t;
}

QString baseUrl(const HttpRequest& request) {
    if (!config().baseUrl.isEmpty()) {
        return config().baseUrl;
    }
    const QByteArray host = request.headers.value("host");
    return QStringLiteral("http://") +
           (host.isEmpty() ? QStringLiteral("127.0.0.1:8080") : QString::fromLatin1(host));
}

HttpResponse json(const QJsonDocument& doc, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json";
    r.body = doc.toJson(QJsonDocument::Compact);
    return r;
}

HttpResponse login(const HttpRequest& request, const QString& who) {
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QMutexLocker lock(&tokens().mutex);
        tokens().nameByToken.insert(token, who);
    }
    const QString base = baseUrl(request);
    qCInfo(lcOtg).noquote() << "OTG login" << who << "-> token" << token << "gateway" << base;
    return json(QJsonDocument(QJsonObject{
        {QStringLiteral("token"), token},
        {QStringLiteral("config"), QJsonObject{{QStringLiteral("gateway"), base}}},
        {QStringLiteral("zone"), config().zone},
    }));
}

} // namespace

void setOtgConfig(const OtgConfig& c) {
    config() = c;
}

bool handleOtg(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond) {
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
        // provider=psn&code=<NpAuth authorization code>. Not checked against shadNet yet.
        const QString code = request.query.queryItemValue(QStringLiteral("code"));
        respond(
            login(request, code.isEmpty() ? QStringLiteral("psn") : QStringLiteral("psn code")));
        return true;
    }
    if (endpoint == QLatin1String("xlogin/driveclub")) {
        respond(login(request, request.query.queryItemValue(QStringLiteral("user"))));
        return true;
    }
    return false;
}
