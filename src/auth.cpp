// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "auth.h"

#include "shadnet_verifier.h"

#include <QLoggingCategory>

#include <memory>

namespace {

Q_LOGGING_CATEGORY(lcAuth, "auth")

struct State {
    AuthConfig config;
    std::unique_ptr<ShadNetVerifier> verifier;
};

State& state() {
    static State s;
    return s;
}

AuthResult notVerified(const QString& message, bool unavailable) {
    AuthResult r;
    r.outcome = state().config.allowUnverified ? AuthResult::Outcome::Unverified
                                               : AuthResult::Outcome::Denied;
    r.message = message;
    r.unavailable = unavailable;
    return r;
}

} // namespace

void setupAuth(const AuthConfig& config) {
    State& s = state();
    s.config = config;
    if (s.config.clientId.isEmpty()) {
        s.config.clientId = QString::fromLatin1(DriveclubClientId);
    }
    if (config.shadnetUrl.isEmpty()) {
        s.verifier.reset();
        return;
    }
    if (!s.verifier) {
        s.verifier = std::make_unique<ShadNetVerifier>();
    }
    s.verifier->setEndpoint(config.shadnetUrl);
    s.verifier->setClientId(s.config.clientId);
}

const AuthConfig& authConfig() {
    return state().config;
}

void verifyCode(const QString& what, const QString& code,
                std::function<void(const AuthResult&)> done) {
    State& s = state();
    if (!s.verifier) {
        done(AuthResult{}); // checks off: Unverified, accepted
        return;
    }
    if (code.isEmpty() || code == QLatin1String("AUTHEN")) {
        const AuthResult r = notVerified(QStringLiteral("no authorization code"), false);
        qCWarning(lcAuth).noquote()
            << what << "login without an authorization code"
            << (r.outcome == AuthResult::Outcome::Denied ? "-> denied" : "-> allowed");
        done(r);
        return;
    }
    s.verifier->verify(code, [what, done](const ShadNetVerifier::Result& v) {
        State& s = state();
        switch (v.status) {
        case ShadNetVerifier::Status::Verified:
            if (!v.clientId.isEmpty() && v.clientId != s.config.clientId) {
                qCWarning(lcAuth).noquote() << what << "code is for client" << v.clientId
                                            << "instead of" << s.config.clientId;
                done(notVerified(QStringLiteral("code is for another client"), false));
                return;
            }
            {
                AuthResult r;
                r.outcome = AuthResult::Outcome::Verified;
                r.accountId = v.accountId;
                r.onlineId = v.onlineId;
                qCInfo(lcAuth).noquote()
                    << what << "login verified by shadNet:"
                    << QStringLiteral("%1 (account %2)").arg(v.onlineId, v.accountId);
                done(r);
            }
            return;
        case ShadNetVerifier::Status::Rejected: {
            const AuthResult r = notVerified(v.message, false);
            qCWarning(lcAuth).noquote()
                << what << "code rejected by shadNet:" << v.message
                << (r.outcome == AuthResult::Outcome::Denied ? "-> denied"
                                                             : "-> allowed (allow_unverified)");
            done(r);
            return;
        }
        case ShadNetVerifier::Status::Unavailable: {
            const AuthResult r = notVerified(v.message, true);
            qCWarning(lcAuth).noquote()
                << what << "shadNet unavailable:" << v.message
                << (r.outcome == AuthResult::Outcome::Denied ? "-> denied"
                                                             : "-> allowed (allow_unverified)");
            done(r);
            return;
        }
        }
    });
}
