// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"
#include "file_log.h"
#include "handler.h"
#include "http_server.h"
#include "trace.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

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
    if (!QFileInfo::exists(configPath)) {
        QFile file(configPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(defaultConfigJson()) < 0) {
            return fatal(
                QStringLiteral("cannot create %1: %2").arg(configPath, file.errorString()));
        }
        qInfo("wrote default configuration to %s", qPrintable(configPath));
    }
    QFile file(configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fatal(QStringLiteral("cannot read %1: %2").arg(configPath, file.errorString()));
    }
    QString error;
    auto config = loadConfig(file.readAll(), &error);
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
    }

    setTraceOptions({config->trace, static_cast<qsizetype>(config->traceBodyBytes)});

    const QString basePath = config->basePath;
    HttpServer server([basePath](const HttpRequest& request, HttpServer::Responder reply) {
        // Every answer goes through here, so each request is traced together with its answer.
        const QString label = endpointLabel(request);
        const quint64 id = traceRequest(request, label);
        QElapsedTimer timer;
        timer.start();
        auto respond = [reply, id, label, timer](HttpResponse response) {
            traceResponse(id, label, response, timer.elapsed());
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
    return app.exec();
}
