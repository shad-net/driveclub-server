// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "http_server.h"

#include <QJsonArray>
#include <QList>
#include <QString>

#include <optional>

// OTG (leaderboards, challenges, stats, accolades), all JSON over HTTP.
//
// 1. GET https://hub.services.otg.scee.com/titles/driveclub/domains
// 2. GET <auth>/login/driveclub?provider=psn&code=<NpAuth code>
// 3. Services under <gateway>/<zone>/<serverIdentifier>/v01/<resource>. Leaderboards
//    (<board>/~leaderboard reads, score posts) nd counter gamestates (<...>/~gamestate) are
//    handled, other resources get 404 and a log line.
struct OtgConfig {
    QString baseUrl;
    quint16 listenPort = 8080; // listen.port, for requests without a Host header
    QString domainId = QStringLiteral("driveclub"); // must contain the TSS OTG_Domain value
    QString zone = QStringLiteral("eu");
};

void setOtgConfig(const OtgConfig& config);

//  For the admin API
struct OtgTokenInfo {
    QString token;
    QString name;    // shadNet online id when verified
    QString account; // shadNet account id, empty when not verified
    qint64 created = 0;
    qint64 lastUsed = 0;
    int requests = 0;
};
QList<OtgTokenInfo> otgTokens();
bool otgDropToken(const QString& token);
void otgRestoreTokens();
QJsonArray otgBoardList();
std::optional<QJsonArray> otgBoardEntries(const QString& name);
bool otgRemoveEntry(const QString& board, const QString& entrant);
bool otgClearBoard(const QString& board);
QJsonArray otgGameStates();
bool otgRemoveGameState(const QString& resource);
bool otgSetEntry(const QString& board, const QString& entrant, quint64 score,
                 const QJsonArray& attachment);

constexpr quint64 OtgTimeMask = 0x0FFFFFFFFFFFFFFFull;
constexpr quint64 otgTimeScore(quint64 ms) {
    return ms > OtgTimeMask ? 0 : OtgTimeMask - ms;
}
constexpr quint64 otgPointsScore(quint64 points) {
    return (quint64(2) << 60) | (points & OtgTimeMask);
}

bool handleOtg(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond);
