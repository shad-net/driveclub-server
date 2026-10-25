// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

#include <functional>

constexpr char DriveclubClientId[] = "2a347f19-d4c4-492d-aadc-bc07ae7b8c15";

struct AuthConfig {
    QString shadnetUrl;
    QString clientId;
    bool allowUnverified = false;
};

struct AuthResult {
    enum class Outcome {
        Verified,   // shadNet confirmed the code
        Unverified, // not checked (no shadnet.url) or failed but allow_unverified is on
        Denied,     // failed and allow_unverified is off
    };
    Outcome outcome = Outcome::Unverified;
    QString accountId;        // shadNet account id (Verified only)
    QString onlineId;         // shadNet online id (Verified only)
    QString message;          // why it was not verified
    bool unavailable = false; // shadNet could not be reached (Denied: answer "try later")
};

void setupAuth(const AuthConfig& config);
const AuthConfig& authConfig();
void verifyCode(const QString& what, const QString& code,
                std::function<void(const AuthResult&)> done);
