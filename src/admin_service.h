// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "config.h"
#include "http_server.h"

#include <QElapsedTimer>
//   GET    /api/status
//   GET    /api/sessions                         EGS sessions (signed-in players)
//   DELETE /api/sessions/<key>                   forget a session (the game must log in again)
//   GET    /api/otg/tokens                       OTG login tokens
//   DELETE /api/otg/tokens/<token>
//   GET    /api/leaderboards                     [{name, entries, best, updated}]
//   GET    /api/leaderboards/entries?board=<b>   ranked entries
//   DELETE /api/leaderboards?board=<b>           clear a board
//   POST   /api/leaderboards/entries?board=<b>   add/replace {entrant, score | time_ms | points,
//                                                attachment,
//                                                name (optional display name)}
//   GET    /api/players                          [{account_id, name, logged_in}] known names
//   DELETE /api/leaderboards/entries?board=<b>&entrant=<e>
//   GET    /api/requests[?after=<id>]            recent game requests (newest last)
class AdminService {
public:
    explicit AdminService(const Config& config);
    void handle(const HttpRequest& request, HttpServer::Responder respond);

private:
    HttpResponse route(const HttpRequest& request);
    HttpResponse status();
    HttpResponse sessions();
    HttpResponse tokens();
    HttpResponse requests(quint64 after);
    HttpResponse setEntry(const QString& board, const QByteArray& body);

    Config config_;
    QElapsedTimer uptime_;
};
