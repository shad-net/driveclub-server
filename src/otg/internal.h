// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <limits>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUuid>
#include "auth.h"
#include "database.h"
#include "egs/egs.h"
#include "otg/otg.h"

namespace otg {

Q_DECLARE_LOGGING_CATEGORY(lcOtg)

using Entry = db::Score; // a leaderboard row

struct Token {
    QString name;    // shadNet online id when verified, else what the request said
    QString account; // shadNet account id, empty when not verified
    qint64 created = 0;
    qint64 lastUsed = 0;
    int requests = 0;
};

struct Tokens {
    QMutex mutex;
    QHash<QString, Token> byToken;
};

inline Tokens& tokens() {
    static Tokens t;
    return t;
}
inline const QString IAmSpecial = QStringLiteral("i_am_special");
OtgConfig& config();
QString baseUrl(const HttpRequest& request);
HttpResponse json(const QJsonDocument& doc, int status = 200);
HttpResponse login(const HttpRequest& request, const QString& who, const AuthResult& auth);
QString tokenOwner(const HttpRequest& request);
bool isClubBoard(const QString& board);
QString callerEntrant(const HttpRequest& request, const QString& board = {});
QJsonValue scoreJson(quint64 score);
QJsonObject entryJson(const Entry& e, qsizetype position);
QString boardName(QString resource);
HttpResponse readBoard(const HttpRequest& request, const QString& name);
bool isChallengeList(const QString& resource);
HttpResponse writeBoard(const HttpRequest& request, const QString& name, bool latestWins);
HttpResponse gameStateJson(const db::GameState& g);
HttpResponse handleGameState(const HttpRequest& request, const QString& resource);

} // namespace otg
