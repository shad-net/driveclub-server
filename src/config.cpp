// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {

bool readInt(const QJsonValue& v, qint64 min, qint64 max, qint64* out) {
    if (!v.isDouble()) {
        return false;
    }
    const double d = v.toDouble();
    const qint64 i = static_cast<qint64>(d);
    if (d != static_cast<double>(i) || i < min || i > max) {
        return false;
    }
    *out = i;
    return true;
}

std::optional<Config> failed(QString* error, const QString& message) {
    *error = message;
    return std::nullopt;
}

} // namespace

std::optional<Config> loadConfig(const QByteArray& json, QString* error) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return failed(
            error, QStringLiteral("config is not a JSON object: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = doc.object();
    Config c;

    const QJsonObject listen = root.value("listen").toObject();
    if (listen.contains("address") && !c.address.setAddress(listen.value("address").toString())) {
        return failed(error, QStringLiteral("listen.address is not an IP address"));
    }
    if (listen.contains("port")) {
        qint64 port = 0;
        if (!readInt(listen.value("port"), 1, 65535, &port)) {
            return failed(error, QStringLiteral("listen.port must be 1..65535"));
        }
        c.port = static_cast<quint16>(port);
    }

    c.basePath = root.value("base_path").toString();
    while (c.basePath.endsWith('/')) {
        c.basePath.chop(1);
    }
    if (!c.basePath.isEmpty() && !c.basePath.startsWith('/')) {
        return failed(error, QStringLiteral("base_path must start with /"));
    }

    const QJsonObject log = root.value("log").toObject();
    c.logFile = log.value("file").toString();
    if (log.contains("max_size_mb")) {
        qint64 mb = 0;
        if (!readInt(log.value("max_size_mb"), 1, 4096, &mb)) {
            return failed(error, QStringLiteral("log.max_size_mb must be 1..4096"));
        }
        c.logMaxBytes = mb * 1024 * 1024;
    }
    if (log.contains("trace")) {
        if (!log.value("trace").isBool()) {
            return failed(error, QStringLiteral("log.trace must be true or false"));
        }
        c.trace = log.value("trace").toBool();
    }
    if (log.contains("trace_body_bytes")) {
        qint64 bytes = 0;
        if (!readInt(log.value("trace_body_bytes"), 0, 16 * 1024 * 1024, &bytes)) {
            return failed(error, QStringLiteral("log.trace_body_bytes must be 0..16777216"));
        }
        c.traceBodyBytes = bytes;
    }

    const QJsonObject egs = root.value("egs").toObject();
    if (egs.contains("server_identifier")) {
        c.egs.serverIdentifier = egs.value("server_identifier").toString();
        if (c.egs.serverIdentifier.isEmpty()) {
            return failed(error, QStringLiteral("egs.server_identifier must not be empty"));
        }
    }
    if (egs.contains("version")) {
        c.egs.version = egs.value("version").toString();
    }

    const QJsonObject otg = root.value("otg").toObject();
    c.otg.baseUrl = otg.value("base_url").toString();
    while (c.otg.baseUrl.endsWith('/')) {
        c.otg.baseUrl.chop(1);
    }
    if (otg.contains("domain_id")) {
        c.otg.domainId = otg.value("domain_id").toString();
    }
    if (otg.contains("zone")) {
        c.otg.zone = otg.value("zone").toString();
    }
    return c;
}

QByteArray defaultConfigJson() {
    return QByteArrayLiteral(R"({
    "listen": { "address": "0.0.0.0", "port": 8080 },
    "base_path": "",
    "log": { "file": "server.log", "max_size_mb": 50, "trace": true, "trace_body_bytes": 65536 },
    "egs": { "server_identifier": "dc-community", "version": "1" },
    "otg": { "base_url": "", "domain_id": "driveclub", "zone": "eu" }
}
)");
}
