// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "http_server.h"

#include <QString>

struct TraceOptions {
    bool enabled = true;
    // Bytes of each body to dump, 0 = the whole body.
    qsizetype bodyLimit = 64 * 1024;
};

void setTraceOptions(const TraceOptions& options);
const TraceOptions& traceOptions();
QString endpointLabel(const HttpRequest& request);
quint64 traceRequest(const HttpRequest& request, const QString& label, quint64 id = 0);
quint64 traceNextId();
bool isPushPoll(const HttpRequest& request);
void traceKeepalive(const HttpRequest& request);
void startKeepaliveSummaries();
void traceResponse(quint64 id, const QString& label, const HttpRequest& request,
                   const HttpResponse& response, qint64 elapsedMs);
QString dumpBody(const QByteArray& body, qsizetype limit);
