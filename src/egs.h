// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "http_server.h"

#include <QByteArray>
#include <QString>

#include <optional>

// EGS ("Evolution Game Services") over https://<gmServer>/egs/binary/<service>/<method>.

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
bool handleEgs(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond);
