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

// ---- Players ----------------------------------------------------------------------------------
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

// ---- Leaderboards -----------------------------------------------------------------------------
struct Score {
    QString board;
    QString entrant;
    quint64 score = 0; // the game's 64-bit score (kind in the top 4 bits, always < 2^63)
    QJsonArray attachment;
    qint64 updated = 0; // ms since epoch; earlier wins a tie
};

QList<Score> boardScores(const QString& board);
qsizetype boardSize(const QString& board);
std::optional<Score> score(const QString& board, const QString& entrant);
bool putScore(const Score& score); // insert or replace
bool removeScore(const QString& board, const QString& entrant);
bool clearBoard(const QString& board);
QList<Score> scoresWithGhosts();
struct EntrantScore {
    Score score;
    qint64 rank = 0;
    qint64 boardSize = 0;
};
QList<EntrantScore> entrantScores(const QString& entrant);

// What happened since a time (ms since epoch), for the admin overview.
struct Activity {
    qint64 newPlayers = 0; // first seen since
    qint64 logins = 0;     // players whose last login is since
    qint64 races = 0;      // eventhistory entries (one per race the game reported)
    qint64 scorePosts = 0; // leaderboard entries set or improved (not event history or lists)
    qint64 challenges = 0; // challenges created (creator's list entry, flags 1)
    qint64 clubsCreated = 0;
    qint64 ghosts = 0; // ghosts uploaded
};
Activity activitySince(qint64 since);

struct BoardInfo {
    QString name;
    qsizetype entries = 0;
    quint64 best = 0;
    qint64 updated = 0;
};
QList<BoardInfo> boards();

// ---- EGS sessions -----------------------------------------------------------------------------
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

// ---- OTG tokens -------------------------------------------------------------------------------
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

// ---- OTG counter gamestates -------------------------------------------------------------------

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

// ---- Fame -------------------------------------------------------------------------------------

struct Fame {
    qint64 accountId = 0;
    qint64 xp = 0;
    std::optional<qint64> stars;
    std::optional<qint64> elite;
    qint64 updated = 0;
};
std::optional<Fame> fame(qint64 accountId);
bool putFame(const Fame& fame);

// ---- Accolades --------------------------------------------------------------------------------

struct Accolade {
    qint64 accountId = 0;
    QString name;
    qint64 progress = 0;
    qint64 updated = 0;
};
QList<Accolade> accolades(qint64 accountId);
qint64 addAccoladeProgress(qint64 accountId, const QString& name, qint64 delta); // new total
QList<qint64> accoladeAccounts();
QList<Accolade> clubAccolades(qint64 clubId);
qint64 addClubAccoladeProgress(qint64 clubId, const QString& name, qint64 delta); // new total
bool removeAccolades(qint64 accountId);

// ---- Friends ----------------------------------------------------------------------------------

QStringList friendNames(qint64 accountId);
bool addFriend(qint64 accountId, const QString& name);    // false when already there
bool removeFriend(qint64 accountId, const QString& name); // false when not there
bool hasFriendList(qint64 accountId);
QStringList allFriendNames(); // every name on any list, once

// ---- Clubs ------------------------------------------------------------------------------------

constexpr int ClubOwner = 2;
constexpr int ClubMemberType = 1;
constexpr int InviteRequest = 1;
constexpr int InviteSent = 2;
constexpr int InviteClosed = 3;

struct Club {
    qint64 id = 0;
    QString name;
    int membership = 0;
    int status = 1;
    qint64 xp = 0;            // club fame, the sum of the members' clubFameDelta
    QByteArray customization; // ClubCustomization protobuf, empty when not set
    qint64 created = 0;
};
struct ClubMember {
    qint64 accountId = 0;
    qint64 clubId = 0;
    int type = ClubMemberType;
    bool mvp = false;
    qint64 joined = 0;
};
struct ClubInvite {
    qint64 id = 0;
    qint64 clubId = 0;
    qint64 accountId = 0;
    qint64 invitedBy = 0;
    int status = InviteSent;
    qint64 created = 0;
    qint64 updated = 0;
    qint64 expires = 0;
};
QList<Club> clubs();
std::optional<Club> club(qint64 id);
std::optional<Club> clubByName(const QString& name);                  // case-insensitive
qint64 createClub(const QString& name, int membership, qint64 owner); // 0 on failure (name taken)
bool updateClub(const Club& club);
qint64 addClubXp(qint64 clubId, qint64 delta); // new total
bool removeClub(qint64 id);                    // with its members and invites
QList<ClubMember> clubMembers(qint64 clubId);
std::optional<ClubMember> clubMember(qint64 accountId); // the player's membership
bool putClubMember(const ClubMember& member);
bool removeClubMember(qint64 accountId);
QList<ClubInvite> clubInvites(); // newest first
std::optional<ClubInvite> clubInvite(qint64 id);
qint64 addClubInvite(qint64 clubId, qint64 accountId, qint64 invitedBy, int status, qint64 ttlMs);
int closeClubInvites(qint64 accountId, qint64 clubId = 0, qint64 inviteId = 0);
struct ClubReport {
    qint64 id = 0;
    qint64 clubId = 0;
    QString clubName; // as it was when reported
    qint64 reporter = 0;
    qint64 language = 0;
    qint64 reason = 0;
    qint64 created = 0;
};
qint64 addClubReport(const ClubReport& report);
QList<ClubReport> clubReports(); // newest first
bool removeClubReport(qint64 id);

// ---- Ghosts -----------------------------------------------------------------------------------

struct Ghost {
    qint64 id = 0;
    qint64 accountId = 0; // uploader
    QString mimeType;
    QString checksum; // base64(MD5(data)), as the game sent it
    qint64 size = 0;
    qint64 created = 0;
    int downloads = 0;
    qint64 lastDownload = 0;
};
qint64 putGhost(const Ghost& ghost, const QByteArray& data);
std::optional<qint64> findGhost(qint64 accountId, const QString& mimeType, const QString& checksum,
                                qint64 size);
std::optional<Ghost> ghost(qint64 id, QByteArray* data = nullptr);
void countGhostDownload(qint64 id);
bool removeGhost(qint64 id);
QList<Ghost> ghosts(); // newest first, without the data

} // namespace db
