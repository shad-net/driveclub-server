// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "auth.h"
#include "egs.h"
#include "otg.h"

#include <QByteArray>
#include <QHostAddress>
#include <QString>
#include <QStringList>

#include <optional>

struct Config {
    QHostAddress address = QHostAddress::Any;
    quint16 port = 8080;
    QString basePath;
    QString logFile;
    qint64 logMaxBytes = 10 * 1024 * 1024;
    bool trace = true;
    qint64 traceBodyBytes = 64 * 1024;
    QString databaseFile = QStringLiteral("driveclub.db");
    int sessionDays = 30;
    EgsConfig egs;
    OtgConfig otg;
    AuthConfig auth;
    QHostAddress adminAddress = QHostAddress::LocalHost;
    quint16 adminPort = 8081;
    QString adminKey;
    QString serverName;   // server.name: in the launcher's server list, empty = the computer's name
    QString serverListOn; // server.list_on: "host[:port]" of a server to be listed on, empty = none
    bool internetUpnp = false; // internet.upnp: the router forwards the port to this PC (upnp.h)
};

std::optional<Config> loadConfig(const QByteArray& json, QString* error);
QByteArray defaultConfigJson();
std::optional<QByteArray> mergeDefaultConfig(const QByteArray& json, QStringList* added);
