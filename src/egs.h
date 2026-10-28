// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "http_server.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

constexpr char EgsKey[] = "ca91f51f-a7f5-4b95-814a-5796cfff586c";

struct EgsConfig {
    QString serverIdentifier = QStringLiteral("dc-community");
    QString version = QStringLiteral("1");
};

void setEgsConfig(const EgsConfig& config);
QByteArray egsSignature(const QString& method, const QByteArray& sessionKey,
                        const QByteArray& body);
std::optional<QByteArray> egsFindSessionKey(const QString& method, const QByteArray& body,
                                            const QByteArray& signature);
QString egsSessionName(const QByteArray& sessionKey);
QByteArray egsDescriptor();
QString egsDescribeBody(const QString& path, bool response, int status, const QByteArray& body);

// Signed-in players, for the admin API.
struct EgsSessionInfo {
    QString key;
    QString name;
    qint32 accountId = 0;
    QString shadnetAccount; // empty = login not verified with shadNet
    QString clientVersion;
    bool psPlus = false;
    qint64 created = 0;  // ms since epoch
    qint64 lastSeen = 0; // ms since epoch
    int requests = 0;
    QString transaction; // current transactionService id, empty = none yet
};
QList<EgsSessionInfo> egsSessions();
bool egsDropSession(const QString& key);
void egsRestoreSessions();
struct EgsPlayerInfo {
    qint64 accountId = 0;
    QString name;
    bool loggedIn = false; // false: added from the admin side
};
QList<EgsPlayerInfo> egsPlayers();
void egsSetPlayerName(qint64 accountId, const QString& name);
QString egsPlayerName(qint64 accountId);
void egsSetFame(qint64 accountId, qint64 xp);
bool handleEgs(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond);
