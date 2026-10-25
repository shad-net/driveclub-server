// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "request_log.h"

#include <QMutex>
#include <QMutexLocker>

namespace {

constexpr qsizetype MaxRecords = 1000;

struct Log {
    QMutex mutex;
    QList<RequestRecord> records;
};

Log& log() {
    static Log l;
    return l;
}

} // namespace

void recordRequest(const RequestRecord& record) {
    QMutexLocker lock(&log().mutex);
    log().records.append(record);
    if (log().records.size() > MaxRecords) {
        log().records.remove(0, log().records.size() - MaxRecords);
    }
}

QList<RequestRecord> recentRequests(quint64 after) {
    QMutexLocker lock(&log().mutex);
    QList<RequestRecord> out;
    for (const RequestRecord& r : log().records) {
        if (r.id > after) {
            out.append(r);
        }
    }
    return out;
}
