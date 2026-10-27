// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace db {

// Opens (creating when needed) the database and brings its schema up to date.
bool open(const QString& path, QString* error);
QString path();
// Deletes sessions and tokens not used for maxAgeDays.
void prune(int maxAgeDays);

// Players
struct Player {
    qint64 accountId = 0;
    QString name;
    bool loggedIn = false;  // false: only known from an admin test entry or a friend lookup
    QString shadnetAccount; // last verified shadNet account id
    qint64 firstSeen = 0;   // ms since epoch
    qint64 lastSeen = 0;
};
void putPlayer(const Player& player);
std::optional<Player> player(qint64 accountId);
std::optional<Player> playerByName(const QString& name); // case-insensitive
QList<Player> players();

// Leaderboards
struct Score {
    QString board;
    QString entrant;
    quint64 score = 0;
    QJsonArray attachment;
    qint64 updated = 0;
};
// Best first.
QList<Score> boardScores(const QString& board);
qsizetype boardSize(const QString& board);
std::optional<Score> score(const QString& board, const QString& entrant);
bool putScore(const Score& score); // insert or replace
bool removeScore(const QString& board, const QString& entrant);
bool clearBoard(const QString& board);

struct BoardInfo {
    QString name;
    qsizetype entries = 0;
    quint64 best = 0;
    qint64 updated = 0;
};
QList<BoardInfo> boards();

// EGS sessions
struct Session {
    QByteArray key;
    qint32 accountId = 0;
    QString name;
    QString shadnetAccount;
    QString clientVersion;
    bool psPlus = false;
    qint64 created = 0;
    qint64 lastSeen = 0;
    int requests = 0;
};
void putSession(const Session& session);
void touchSession(const QByteArray& key, qint64 lastSeen, int requests);
void removeSession(const QByteArray& key);
QList<Session> sessions();

// OTG tokens
struct Token {
    QString token;
    QString name;
    QString account;
    qint64 created = 0;
    qint64 lastUsed = 0;
    int requests = 0;
};
void putToken(const Token& token);
void touchToken(const QString& token, qint64 lastUsed, int requests);
void removeToken(const QString& token);
QList<Token> tokens();

// OTG counter gamestates
struct GameState {
    QString resource;
    qint64 version = 0;
    QJsonObject counts; // name to integer
    qint64 updated = 0;
};
std::optional<GameState> gameState(const QString& resource);
bool putGameState(const GameState& state);
bool removeGameState(const QString& resource);
QList<GameState> gameStates();

} // namespace db
