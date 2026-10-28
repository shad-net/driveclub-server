// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace db {
bool open(const QString& path, QString* error);
QString path();
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
QList<Score> boardScores(const QString& board);
qsizetype boardSize(const QString& board);
std::optional<Score> score(const QString& board, const QString& entrant);
bool putScore(const Score& score); // insert or replace
bool removeScore(const QString& board, const QString& entrant);
bool clearBoard(const QString& board);
QList<Score> scoresWithGhosts();

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
    QJsonObject counts; // name -> integer
    qint64 updated = 0;
};
std::optional<GameState> gameState(const QString& resource);
bool putGameState(const GameState& state);
bool removeGameState(const QString& resource);
QList<GameState> gameStates();

// Fame

struct Fame {
    qint64 accountId = 0;
    qint64 xp = 0;
    std::optional<qint64> stars;
    std::optional<qint64> elite;
    qint64 updated = 0;
};
std::optional<Fame> fame(qint64 accountId);
bool putFame(const Fame& fame);

// Ghosts

struct Ghost {
    qint64 id = 0;
    qint64 accountId = 0;
    QString mimeType;
    QString checksum;
    qint64 size = 0;
    qint64 created = 0;
    int downloads = 0;
    qint64 lastDownload = 0;
};
qint64 putGhost(const Ghost& ghost, const QByteArray& data);
std::optional<Ghost> ghost(qint64 id, QByteArray* data = nullptr);
void countGhostDownload(qint64 id);
bool removeGhost(qint64 id);
QList<Ghost> ghosts();

} // namespace db
