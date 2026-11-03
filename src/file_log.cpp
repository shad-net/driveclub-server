// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "file_log.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>

#include <cstdio>

namespace {

struct FileLog {
    QMutex mutex;
    QFile file;
    qint64 maxBytes = 0;
    QtMessageHandler previous = nullptr;
};

FileLog& state() {
    static FileLog log;
    return log;
}

// Called with the mutex held.
void restart(FileLog& log) {
    log.file.close();
    if (!log.file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        std::fprintf(stderr, "cannot reopen log file %s\n", qPrintable(log.file.fileName()));
    }
}

void handler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    FileLog& log = state();
    if (log.previous) {
        log.previous(type, context, message);
    }
    const QByteArray line =
        (qFormatLogMessage(type, context, message) + QLatin1Char('\n')).toUtf8();
    QMutexLocker lock(&log.mutex);
    if (!log.file.isOpen()) {
        return;
    }
    if (log.file.size() + line.size() > log.maxBytes) {
        restart(log);
        if (!log.file.isOpen()) {
            return;
        }
    }
    log.file.write(line);
    log.file.flush();
}

struct WarningLog {
    QMutex mutex;
    QList<LoggedWarning> lines;
    QtMessageHandler previous = nullptr;
};

WarningLog& warnings() {
    static WarningLog w;
    return w;
}

void warningHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    WarningLog& w = warnings();
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker lock(&w.mutex);
        w.lines.append({QDateTime::currentMSecsSinceEpoch(),
                        QString::fromLatin1(context.category ? context.category : ""), message});
        while (w.lines.size() > 50) {
            w.lines.removeFirst();
        }
    }
    if (w.previous) {
        w.previous(type, context, message);
    }
}

} // namespace

void startWarningCapture() {
    warnings().previous = qInstallMessageHandler(warningHandler);
}

QList<LoggedWarning> recentWarnings() {
    QMutexLocker lock(&warnings().mutex);
    return warnings().lines;
}

bool startFileLog(const QString& path, qint64 maxBytes, QString* error) {
    FileLog& log = state();
    QMutexLocker lock(&log.mutex);
    log.maxBytes = maxBytes;
    log.file.setFileName(path);
    if (!log.file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        *error = log.file.errorString();
        return false;
    }
    log.previous = qInstallMessageHandler(handler);
    return true;
}
