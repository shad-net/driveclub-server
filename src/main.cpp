// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "admin_service.h"
#include "auth.h"
#include "config.h"
#include "database.h"
#include "egs.h"
#include "file_log.h"
#include "handler.h"
#include "http_server.h"
#include "otg.h"
#include "request_log.h"
#include "trace.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSaveFile>

#include <cstdio>

namespace {

int fatal(const QString& message) {
    std::fprintf(stderr, "error: %s\n", qPrintable(message));
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("driveclub-server"));
    qSetMessagePattern(QStringLiteral(
        "%{time yyyy-MM-dd hh:mm:ss.zzz} [%{type}] %{if-category}%{category}: %{endif}%{message}"));

    const QString configPath = QStringLiteral("config.json");
    QStringList startupNotes; // repeated into the log file once it is open
    if (!QFileInfo::exists(configPath)) {
        QFile file(configPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(defaultConfigJson()) < 0) {
            return fatal(
                QStringLiteral("cannot create %1: %2").arg(configPath, file.errorString()));
        }
        qInfo("wrote default configuration to %s", qPrintable(configPath));
        startupNotes << QStringLiteral("wrote default configuration to %1").arg(configPath);
    }
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fatal(QStringLiteral("cannot read %1: %2").arg(configPath, file.errorString()));
    }
    QByteArray configJson = file.readAll();
    file.close();
    // Settings added in newer versions are written into an existing config.json.
    QStringList added;
    if (const auto merged = mergeDefaultConfig(configJson, &added)) {
        QSaveFile out(configPath);
        if (out.open(QIODevice::WriteOnly) && out.write(*merged) >= 0 && out.commit()) {
            startupNotes << QStringLiteral("added new settings to %1: %2")
                                .arg(configPath, added.join(QStringLiteral(", ")));
            qInfo("%s", qPrintable(startupNotes.last()));
        } else {
            qWarning("cannot update %s (%s); using the new settings' defaults for this run",
                     qPrintable(configPath), qPrintable(out.errorString()));
        }
        configJson = *merged;
    }
    QString error;
    auto config = loadConfig(configJson, &error);
    if (!config) {
        return fatal(QStringLiteral("%1: %2").arg(configPath, error));
    }

    if (!config->logFile.isEmpty()) {
        const QString logPath =
            QDir(QFileInfo(configPath).absolutePath()).absoluteFilePath(config->logFile);
        if (!startFileLog(logPath, config->logMaxBytes, &error)) {
            return fatal(QStringLiteral("cannot open log file %1: %2").arg(logPath, error));
        }
        qInfo("logging to %s (rewritten at %lld MB)", qPrintable(logPath),
              static_cast<long long>(config->logMaxBytes / (1024 * 1024)));
        for (const QString& note : startupNotes) {
            qInfo("%s", qPrintable(note));
        }
    }

    const QDir configDir(QFileInfo(configPath).absolutePath());
    const QString dbPath = configDir.absoluteFilePath(config->databaseFile);
    if (!db::open(dbPath, &error)) {
        return fatal(QStringLiteral("cannot open database %1: %2").arg(dbPath, error));
    }
    qInfo("database %s", qPrintable(dbPath));
    db::prune(config->sessionDays);

    setTraceOptions({config->trace, static_cast<qsizetype>(config->traceBodyBytes)});
    setEgsConfig(config->egs);
    setOtgConfig(config->otg);
    qInfo("OTG base URL (auth, gateway): %s", config->otg.baseUrl.isEmpty()
                                                  ? "http:// + the Host header of each request"
                                                  : qPrintable(config->otg.baseUrl));
    egsRestoreSessions();
    otgRestoreTokens();
    setupAuth(config->auth);
    if (config->auth.shadnetUrl.isEmpty()) {
        qInfo("shadNet checks off (shadnet.url empty): logins are accepted as sent");
    } else {
        qInfo("verifying authorization codes with shadNet at %s as client %s%s",
              qPrintable(config->auth.shadnetUrl), qPrintable(authConfig().clientId),
              config->auth.allowUnverified ? " (unverified logins allowed)" : "");
    }
    qInfo("EGS serverIdentifier \"%s\" (each OTG_Service_* TSS value must contain it)",
          qPrintable(config->egs.serverIdentifier));

    const QString basePath = config->basePath;
    HttpServer server([basePath](const HttpRequest& request, HttpServer::Responder reply) {
        // Every answer goes through here, so each request is traced together with its answer.
        const QString label = endpointLabel(request);
        const quint64 id = traceRequest(request, label);
        QElapsedTimer timer;
        timer.start();
        auto respond = [reply, id, label, timer, request](HttpResponse response) {
            traceResponse(id, label, request, response, timer.elapsed());
            RequestRecord record;
            record.id = id;
            record.time = QDateTime::currentMSecsSinceEpoch();
            record.method = QString::fromLatin1(request.method);
            record.path = request.path;
            if (!request.query.isEmpty()) {
                record.path += '?' + request.query.toString();
            }
            record.label = label;
            record.peer = request.peer.toString();
            record.status = response.status;
            record.elapsedMs = timer.elapsed();
            record.bytesIn = request.body.size();
            record.bytesOut = response.body.size();
            recordRequest(record);
            reply(std::move(response));
        };
        QString path = request.path;
        if (!basePath.isEmpty()) {
            if (!path.startsWith(basePath + '/')) {
                qWarning("outside base path: %s", qPrintable(path));
                HttpResponse notFound;
                notFound.status = 404;
                notFound.contentType = "text/plain";
                notFound.body = "Not Found";
                respond(notFound);
                return;
            }
            path = path.mid(basePath.size());
        }
        handleRequest(request, path.mid(1), respond);
    });
    if (!server.listen(config->address, config->port, &error)) {
        return fatal(QStringLiteral("cannot listen on %1:%2: %3")
                         .arg(config->address.toString())
                         .arg(config->port)
                         .arg(error));
    }
    qInfo("listening on %s:%u, endpoints under %s, trace %s",
          qPrintable(config->address.toString()), config->port,
          qPrintable(basePath.isEmpty() ? QStringLiteral("/") : basePath),
          config->trace ? "on" : "off");

    AdminService admin(*config);
    HttpServer adminServer([&admin](const HttpRequest& request, HttpServer::Responder respond) {
        admin.handle(request, std::move(respond));
    });
    if (config->adminKey.isEmpty()) {
        qInfo("admin API disabled: set admin.key in config.json to use driveclub-admin");
    } else if (!adminServer.listen(config->adminAddress, config->adminPort, &error)) {
        return fatal(QStringLiteral("cannot listen on %1:%2 for the admin API: %3")
                         .arg(config->adminAddress.toString())
                         .arg(config->adminPort)
                         .arg(error));
    } else {
        qInfo("admin API on %s:%u (driveclub-admin, key: admin.key in config.json)",
              qPrintable(config->adminAddress.toString()), config->adminPort);
    }
    return app.exec();
}
