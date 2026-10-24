// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "http_server.h"

#include <QString>

// OTG (leaderboards, challenges, stats, accolades), all JSON over HTTP.
//
// 1. GET https://hub.services.otg.scee.com/titles/driveclub/domains
// 2. GET <auth>/login/driveclub?provider=psn&code=<NpAuth code>
struct OtgConfig {
    QString baseUrl;
    QString domainId = QStringLiteral("driveclub"); // must contain the TSS OTG_Domain value
    QString zone = QStringLiteral("eu");
};

void setOtgConfig(const OtgConfig& config);

bool handleOtg(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond);
