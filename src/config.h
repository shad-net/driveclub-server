// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "egs.h"
#include "otg.h"

#include <QByteArray>
#include <QHostAddress>
#include <QString>

#include <optional>

struct Config {
    QHostAddress address = QHostAddress::Any;
    quint16 port = 8080;
    // Prefix in front of every endpoint, e.g. "/game/v1"; empty = none.
    QString basePath;
    // Log file, empty = console only. Rewritten from the start
    // when it reaches logMaxBytes.
    QString logFile;
    qint64 logMaxBytes = 10 * 1024 * 1024;
    // Trace every request and answer in full (headers, EGS fields, bodies). Off = one line each.
    bool trace = true;
    // Bytes of each body to dump in a trace, 0 = whole body.
    qint64 traceBodyBytes = 64 * 1024;
    EgsConfig egs;
    OtgConfig otg;
};

std::optional<Config> loadConfig(const QByteArray& json, QString* error);
QByteArray defaultConfigJson();
