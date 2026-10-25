// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QList>
#include <QString>

// The most recent game requests, for the admin API's traffic view.
struct RequestRecord {
    quint64 id = 0;
    qint64 time = 0; // ms since epoch
    QString method;
    QString path;
    QString label;
    QString peer;
    int status = 0;
    qint64 elapsedMs = 0;
    qint64 bytesIn = 0;
    qint64 bytesOut = 0;
};

void recordRequest(const RequestRecord& record);
QList<RequestRecord> recentRequests(quint64 after = 0);
