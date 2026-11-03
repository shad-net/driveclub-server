// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "database.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

namespace db {
namespace {

Q_LOGGING_CATEGORY(lcDb, "db")

const char* const Migrations[] = {
    // 1: players, scores, sessions, tokens
    R"(
    CREATE TABLE players (
        account_id      INTEGER PRIMARY KEY,
        name            TEXT NOT NULL,
        logged_in       INTEGER NOT NULL DEFAULT 0,
        shadnet_account TEXT NOT NULL DEFAULT '',
        first_seen      INTEGER NOT NULL DEFAULT 0,
        last_seen       INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX players_name ON players (name COLLATE NOCASE);

    CREATE TABLE scores (
        board      TEXT NOT NULL,
        entrant    TEXT NOT NULL,
        score      INTEGER NOT NULL,
        attachment TEXT NOT NULL DEFAULT '[]',
        updated    INTEGER NOT NULL,
        PRIMARY KEY (board, entrant)
    );
    CREATE INDEX scores_rank ON scores (board, score DESC, updated ASC);

    CREATE TABLE sessions (
        key             TEXT PRIMARY KEY,
        account_id      INTEGER NOT NULL,
        name            TEXT NOT NULL,
        shadnet_account TEXT NOT NULL DEFAULT '',
        client_version  TEXT NOT NULL DEFAULT '',
        ps_plus         INTEGER NOT NULL DEFAULT 0,
        created         INTEGER NOT NULL,
        last_seen       INTEGER NOT NULL,
        requests        INTEGER NOT NULL DEFAULT 0
    );

    CREATE TABLE otg_tokens (
        token     TEXT PRIMARY KEY,
        name      TEXT NOT NULL,
        account   TEXT NOT NULL DEFAULT '',
        created   INTEGER NOT NULL,
        last_used INTEGER NOT NULL,
        requests  INTEGER NOT NULL DEFAULT 0
    );
    )",
    // 2: OTG counter gamestates (ChallengeTimers/V01/player/<id>/~gamestate and the like)
    R"(
    CREATE TABLE gamestates (
        resource TEXT PRIMARY KEY,
        version  INTEGER NOT NULL,
        counts   TEXT NOT NULL DEFAULT '{}',
        updated  INTEGER NOT NULL
    );
    )",
    // 3: fame (profileService.getFame / updateFameInTransaction)
    R"(
    CREATE TABLE fame (
        account_id INTEGER PRIMARY KEY,
        xp         INTEGER NOT NULL DEFAULT 0,
        stars      INTEGER,
        elite      INTEGER,
        updated    INTEGER NOT NULL
    );
    )",
    // 4: ghosts uploaded with storageService.createBob, downloaded from /ghosts/<id>
    R"(
    CREATE TABLE ghosts (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        account_id INTEGER NOT NULL,
        mime_type  TEXT NOT NULL DEFAULT '',
        checksum   TEXT NOT NULL DEFAULT '',
        size       INTEGER NOT NULL,
        data       BLOB NOT NULL,
        created    INTEGER NOT NULL,
        downloads  INTEGER NOT NULL DEFAULT 0,
        last_download INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX ghosts_owner ON ghosts (account_id);
    )",
    // 5: accolade progress (objectiveService), the sum of the deltas the game sends per accolade
    R"(
    CREATE TABLE accolades (
        account_id INTEGER NOT NULL,
        name       TEXT NOT NULL,
        progress   INTEGER NOT NULL DEFAULT 0,
        updated    INTEGER NOT NULL,
        PRIMARY KEY (account_id, name)
    );
    )",
    // 6: friend lists (friendsListService), kept in step with each player's PSN friends by the
    // game's addFriends / removeFriends after login. Names are PSN online ids.
    R"(
    CREATE TABLE friends (
        account_id INTEGER NOT NULL,
        name       TEXT NOT NULL COLLATE NOCASE,
        added      INTEGER NOT NULL,
        PRIMARY KEY (account_id, name)
    );
    )",
    // 7: clubs (clubService). Club ids start at 1001, invite ids at 5001. customization is the
    // club's ClubCustomization protobuf as sent by
    // the game. A player is in at most one club. Invites and join requests share one table, as in
    // the game (findInvites): status 1 = join request, 2 = invite sent, 3 = closed.
    R"(
    CREATE TABLE clubs (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        name          TEXT NOT NULL UNIQUE COLLATE NOCASE,
        membership    INTEGER NOT NULL DEFAULT 0,
        status        INTEGER NOT NULL DEFAULT 1,
        xp            INTEGER NOT NULL DEFAULT 0,
        customization BLOB NOT NULL DEFAULT x'',
        created       INTEGER NOT NULL
    );
    CREATE TABLE club_members (
        account_id INTEGER PRIMARY KEY,
        club_id    INTEGER NOT NULL REFERENCES clubs (id) ON DELETE CASCADE,
        type       INTEGER NOT NULL,
        mvp        INTEGER NOT NULL DEFAULT 0,
        joined     INTEGER NOT NULL
    );
    CREATE INDEX club_members_club ON club_members (club_id);
    CREATE TABLE club_invites (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        club_id    INTEGER NOT NULL REFERENCES clubs (id) ON DELETE CASCADE,
        account_id INTEGER NOT NULL,
        invited_by INTEGER NOT NULL,
        status     INTEGER NOT NULL,
        created    INTEGER NOT NULL,
        updated    INTEGER NOT NULL,
        expires    INTEGER NOT NULL
    );
    INSERT INTO sqlite_sequence (name, seq) VALUES ('clubs', 1000);
    INSERT INTO sqlite_sequence (name, seq) VALUES ('club_invites', 5000);
    )",
    // 8: club accolade progress (the members' contributions, updateAccoladeProgressInTransaction
    // entries with currentClub true), summed per club.
    R"(
    CREATE TABLE club_accolades (
        club_id  INTEGER NOT NULL REFERENCES clubs (id) ON DELETE CASCADE,
        name     TEXT NOT NULL,
        progress INTEGER NOT NULL DEFAULT 0,
        updated  INTEGER NOT NULL,
        PRIMARY KEY (club_id, name)
    );
    )",
    // 9: club reports (moderationService.submitClubNameReport). The club's name is kept as it was
    // when reported, and a report outlives the club.
    R"(
    CREATE TABLE club_reports (
        id        INTEGER PRIMARY KEY AUTOINCREMENT,
        club_id   INTEGER NOT NULL,
        club_name TEXT NOT NULL DEFAULT '',
        reporter  INTEGER NOT NULL,
        language  INTEGER NOT NULL DEFAULT 0,
        reason    INTEGER NOT NULL DEFAULT 0,
        created   INTEGER NOT NULL
    );
    )",
};

QMutex& lock() {
    static QMutex m;
    return m;
}

QString& dbPath() {
    static QString p;
    return p;
}

QSqlDatabase connection() {
    const QString name =
        QStringLiteral("driveclub-%1").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name)) {
        return QSqlDatabase::database(name);
    }
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    d.setDatabaseName(dbPath());
    d.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (d.open()) {
        QSqlQuery q(d);
        q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
        q.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    } else {
        qCWarning(lcDb).noquote() << "cannot open" << dbPath() << ":" << d.lastError().text();
    }
    return d;
}

bool run(QSqlQuery& q, const char* what) {
    if (q.exec()) {
        return true;
    }
    qCWarning(lcDb).noquote() << what << "failed:" << q.lastError().text();
    return false;
}

QString attachmentText(const QJsonArray& a) {
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

QJsonArray attachmentArray(const QString& text) {
    return QJsonDocument::fromJson(text.toUtf8()).array();
}

Score scoreFrom(const QSqlQuery& q) {
    Score s;
    s.board = q.value(0).toString();
    s.entrant = q.value(1).toString();
    s.score = static_cast<quint64>(q.value(2).toLongLong());
    s.attachment = attachmentArray(q.value(3).toString());
    s.updated = q.value(4).toLongLong();
    return s;
}

Player playerFrom(const QSqlQuery& q) {
    Player p;
    p.accountId = q.value(0).toLongLong();
    p.name = q.value(1).toString();
    p.loggedIn = q.value(2).toBool();
    p.shadnetAccount = q.value(3).toString();
    p.firstSeen = q.value(4).toLongLong();
    p.lastSeen = q.value(5).toLongLong();
    return p;
}

constexpr char PlayerColumns[] =
    "account_id, name, logged_in, shadnet_account, first_seen, last_seen";
constexpr char ScoreColumns[] = "board, entrant, score, attachment, updated";

// A null QString binds as SQL NULL; every text column is NOT NULL, so bind "" instead.
QString text(const QString& s) {
    return s.isNull() ? QStringLiteral("") : s;
}

qint64 now() {
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

bool open(const QString& path, QString* error) {
    QMutexLocker l(&lock());
    dbPath() = path;
    QSqlDatabase d = connection();
    if (!d.isOpen()) {
        *error = d.lastError().text();
        return false;
    }
    QSqlQuery q(d);
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    q.exec(QStringLiteral("PRAGMA user_version"));
    const int version = q.next() ? q.value(0).toInt() : 0;
    const int latest = int(std::size(Migrations));
    for (int v = version; v < latest; ++v) {
        d.transaction();
        bool ok = true;
        for (const QString& statement :
             QString::fromLatin1(Migrations[v]).split(';', Qt::SkipEmptyParts)) {
            if (statement.trimmed().isEmpty()) {
                continue;
            }
            if (!q.exec(statement)) {
                *error = QStringLiteral("schema %1: %2").arg(v + 1).arg(q.lastError().text());
                ok = false;
                break;
            }
        }
        if (!ok || !q.exec(QStringLiteral("PRAGMA user_version = %1").arg(v + 1))) {
            d.rollback();
            if (error->isEmpty()) {
                *error = q.lastError().text();
            }
            return false;
        }
        d.commit();
        qCInfo(lcDb).noquote() << "database schema updated to version" << v + 1;
    }
    // Before 0.0.5 the alias "i_am_special" (the caller) was stored as a literal entrant, so one
    // player's row could be answered as another's. Such rows cannot be given back to their owner.
    if (q.exec(QStringLiteral("DELETE FROM scores WHERE entrant = 'i_am_special'")) &&
        q.numRowsAffected() > 0) {
        qCInfo(lcDb).noquote() << "removed" << q.numRowsAffected()
                               << "leaderboard entries stored as \"i_am_special\"";
    }
    // Every club has an entry on the club fame board (scores/fame/club, kind 2 = plain points);
    // clubs made before 0.0.9 get theirs here.
    if (q.exec(QStringLiteral(
            "INSERT OR IGNORE INTO scores (board, entrant, score, attachment, updated) "
            "SELECT 'fame/club', CAST(id AS TEXT), (2 << 60) | xp, '[]', created FROM clubs")) &&
        q.numRowsAffected() > 0) {
        qCInfo(lcDb).noquote() << "added" << q.numRowsAffected() << "clubs to the club fame board";
    }
    return true;
}

QString path() {
    return dbPath();
}

void prune(int maxAgeDays) {
    const qint64 cutoff = now() - qint64(maxAgeDays) * 24 * 3600 * 1000;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM sessions WHERE last_seen < ?"));
    q.addBindValue(cutoff);
    run(q, "prune sessions");
    const int sessionsGone = q.numRowsAffected();
    q.prepare(QStringLiteral("DELETE FROM otg_tokens WHERE last_used < ?"));
    q.addBindValue(cutoff);
    run(q, "prune tokens");
    const int tokensGone = q.numRowsAffected();
    if (sessionsGone > 0 || tokensGone > 0) {
        qCInfo(lcDb).noquote() << "dropped" << sessionsGone << "sessions and" << tokensGone
                               << "OTG tokens unused for" << maxAgeDays << "days";
    }
}

// ---- Players ----------------------------------------------------------------------------------

void putPlayer(const Player& p) {
    const qint64 t = now();
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "INSERT INTO players (account_id, name, logged_in, shadnet_account, first_seen, last_seen) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (account_id) DO UPDATE SET name = excluded.name, "
        "logged_in = MAX(players.logged_in, excluded.logged_in), "
        "shadnet_account = CASE WHEN excluded.shadnet_account = '' THEN players.shadnet_account "
        "ELSE excluded.shadnet_account END, "
        "last_seen = CASE WHEN excluded.logged_in = 1 THEN excluded.last_seen "
        "ELSE players.last_seen END"));
    q.addBindValue(p.accountId);
    q.addBindValue(text(p.name));
    q.addBindValue(p.loggedIn ? 1 : 0);
    q.addBindValue(text(p.shadnetAccount));
    q.addBindValue(p.firstSeen ? p.firstSeen : t);
    q.addBindValue(p.loggedIn ? t : p.lastSeen);
    run(q, "save player");
}

std::optional<Player> player(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM players WHERE account_id = ?")
                  .arg(QLatin1String(PlayerColumns)));
    q.addBindValue(accountId);
    if (run(q, "read player") && q.next()) {
        return playerFrom(q);
    }
    return std::nullopt;
}

std::optional<Player> playerByName(const QString& name) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM players WHERE name = ? COLLATE NOCASE "
                             "ORDER BY logged_in DESC LIMIT 1")
                  .arg(QLatin1String(PlayerColumns)));
    q.addBindValue(text(name));
    if (run(q, "find player") && q.next()) {
        return playerFrom(q);
    }
    return std::nullopt;
}

QList<Player> players() {
    QList<Player> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM players ORDER BY name COLLATE NOCASE")
                  .arg(QLatin1String(PlayerColumns)));
    if (run(q, "list players")) {
        while (q.next()) {
            out.append(playerFrom(q));
        }
    }
    return out;
}

// ---- Leaderboards -----------------------------------------------------------------------------

QList<Score> boardScores(const QString& board) {
    QList<Score> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM scores WHERE board = ? "
                             "ORDER BY score DESC, updated ASC")
                  .arg(QLatin1String(ScoreColumns)));
    q.addBindValue(text(board));
    if (run(q, "read board")) {
        while (q.next()) {
            out.append(scoreFrom(q));
        }
    }
    return out;
}

QList<EntrantScore> entrantScores(const QString& entrant) {
    QList<EntrantScore> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(
        QStringLiteral("SELECT s.board, s.entrant, s.score, s.attachment, s.updated, "
                       "(SELECT COUNT(*) FROM scores t WHERE t.board = s.board AND "
                       "(t.score > s.score OR (t.score = s.score AND t.updated < s.updated))) + 1, "
                       "(SELECT COUNT(*) FROM scores t WHERE t.board = s.board) "
                       "FROM scores s WHERE s.entrant = ? ORDER BY s.updated DESC"));
    q.addBindValue(text(entrant));
    if (run(q, "read entrant scores")) {
        while (q.next()) {
            out.append({scoreFrom(q), q.value(5).toLongLong(), q.value(6).toLongLong()});
        }
    }
    return out;
}

Activity activitySince(qint64 since) {
    Activity a;
    QMutexLocker l(&lock());
    auto count = [since](const char* sql) -> qint64 {
        QSqlQuery q(connection());
        q.prepare(QString::fromLatin1(sql));
        q.addBindValue(since);
        return run(q, "read activity") && q.next() ? q.value(0).toLongLong() : 0;
    };
    a.newPlayers = count("SELECT COUNT(*) FROM players WHERE first_seen >= ?");
    a.logins = count("SELECT COUNT(*) FROM players WHERE logged_in = 1 AND last_seen >= ?");
    a.races =
        count("SELECT COUNT(*) FROM scores WHERE board LIKE 'eventhistory/%' AND updated >= ?");
    a.scorePosts = count("SELECT COUNT(*) FROM scores WHERE updated >= ? AND board NOT LIKE "
                         "'eventhistory/%' AND NOT (board LIKE 'challenges/%' AND board NOT LIKE "
                         "'%h%m%s%') AND board NOT LIKE 'fame/%'");
    a.challenges = count("SELECT COUNT(*) FROM scores WHERE updated >= ? AND (board LIKE "
                         "'challenges/player/%' OR board LIKE 'challenges/club/%') AND board NOT "
                         "LIKE '%h%m%s%' AND (score & 255) = 1");
    a.clubsCreated = count("SELECT COUNT(*) FROM clubs WHERE created >= ?");
    a.ghosts = count("SELECT COUNT(*) FROM ghosts WHERE created >= ?");
    return a;
}

QList<Score> scoresWithGhosts() {
    QList<Score> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM scores WHERE attachment LIKE '%ghost:%' "
                             "AND attachment NOT LIKE '%ghost:-%'")
                  .arg(QLatin1String(ScoreColumns)));
    if (run(q, "read ghost scores")) {
        while (q.next()) {
            out.append(scoreFrom(q));
        }
    }
    return out;
}

qsizetype boardSize(const QString& board) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM scores WHERE board = ?"));
    q.addBindValue(text(board));
    return run(q, "count board") && q.next() ? q.value(0).toLongLong() : 0;
}

std::optional<Score> score(const QString& board, const QString& entrant) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM scores WHERE board = ? AND entrant = ?")
                  .arg(QLatin1String(ScoreColumns)));
    q.addBindValue(text(board));
    q.addBindValue(text(entrant));
    if (run(q, "read score") && q.next()) {
        return scoreFrom(q);
    }
    return std::nullopt;
}

bool putScore(const Score& s) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO scores (%1) VALUES (?, ?, ?, ?, ?)")
                  .arg(QLatin1String(ScoreColumns)));
    q.addBindValue(text(s.board));
    q.addBindValue(text(s.entrant));
    q.addBindValue(static_cast<qint64>(s.score));
    q.addBindValue(attachmentText(s.attachment));
    q.addBindValue(s.updated ? s.updated : now());
    return run(q, "save score");
}

bool removeScore(const QString& board, const QString& entrant) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM scores WHERE board = ? AND entrant = ?"));
    q.addBindValue(text(board));
    q.addBindValue(text(entrant));
    return run(q, "remove score") && q.numRowsAffected() > 0;
}

bool clearBoard(const QString& board) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM scores WHERE board = ?"));
    q.addBindValue(text(board));
    return run(q, "clear board") && q.numRowsAffected() > 0;
}

QList<BoardInfo> boards() {
    QList<BoardInfo> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT board, COUNT(*), MAX(score), MAX(updated) FROM scores "
                             "GROUP BY board ORDER BY board"));
    if (run(q, "list boards")) {
        while (q.next()) {
            out.append({q.value(0).toString(), q.value(1).toLongLong(),
                        static_cast<quint64>(q.value(2).toLongLong()), q.value(3).toLongLong()});
        }
    }
    return out;
}

// ---- EGS sessions -----------------------------------------------------------------------------

void putSession(const Session& s) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sessions (key, account_id, name, shadnet_account, client_version, "
        "ps_plus, created, last_seen, requests) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(QString::fromLatin1(s.key));
    q.addBindValue(s.accountId);
    q.addBindValue(text(s.name));
    q.addBindValue(text(s.shadnetAccount));
    q.addBindValue(text(s.clientVersion));
    q.addBindValue(s.psPlus ? 1 : 0);
    q.addBindValue(s.created);
    q.addBindValue(s.lastSeen);
    q.addBindValue(s.requests);
    run(q, "save session");
}

void touchSession(const QByteArray& key, qint64 lastSeen, int requests) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("UPDATE sessions SET last_seen = ?, requests = ? WHERE key = ?"));
    q.addBindValue(lastSeen);
    q.addBindValue(requests);
    q.addBindValue(QString::fromLatin1(key));
    run(q, "update session");
}

void removeSession(const QByteArray& key) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM sessions WHERE key = ?"));
    q.addBindValue(QString::fromLatin1(key));
    run(q, "remove session");
}

QList<Session> sessions() {
    QList<Session> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT key, account_id, name, shadnet_account, client_version, "
                             "ps_plus, created, last_seen, requests FROM sessions"));
    if (run(q, "list sessions")) {
        while (q.next()) {
            Session s;
            s.key = q.value(0).toString().toLatin1();
            s.accountId = q.value(1).toInt();
            s.name = q.value(2).toString();
            s.shadnetAccount = q.value(3).toString();
            s.clientVersion = q.value(4).toString();
            s.psPlus = q.value(5).toBool();
            s.created = q.value(6).toLongLong();
            s.lastSeen = q.value(7).toLongLong();
            s.requests = q.value(8).toInt();
            out.append(s);
        }
    }
    return out;
}

// ---- OTG tokens -------------------------------------------------------------------------------

void putToken(const Token& t) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO otg_tokens (token, name, account, created, "
                             "last_used, requests) VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(text(t.token));
    q.addBindValue(text(t.name));
    q.addBindValue(text(t.account));
    q.addBindValue(t.created);
    q.addBindValue(t.lastUsed);
    q.addBindValue(t.requests);
    run(q, "save token");
}

void touchToken(const QString& token, qint64 lastUsed, int requests) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("UPDATE otg_tokens SET last_used = ?, requests = ? WHERE token = ?"));
    q.addBindValue(lastUsed);
    q.addBindValue(requests);
    q.addBindValue(text(token));
    run(q, "update token");
}

void removeToken(const QString& token) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM otg_tokens WHERE token = ?"));
    q.addBindValue(text(token));
    run(q, "remove token");
}

QList<Token> tokens() {
    QList<Token> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "SELECT token, name, account, created, last_used, requests FROM otg_tokens"));
    if (run(q, "list tokens")) {
        while (q.next()) {
            out.append({q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                        q.value(3).toLongLong(), q.value(4).toLongLong(), q.value(5).toInt()});
        }
    }
    return out;
}

// ---- OTG counter gamestates -------------------------------------------------------------------

std::optional<GameState> gameState(const QString& resource) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "SELECT resource, version, counts, updated FROM gamestates WHERE resource = ?"));
    q.addBindValue(text(resource));
    if (run(q, "read gamestate") && q.next()) {
        GameState g;
        g.resource = q.value(0).toString();
        g.version = q.value(1).toLongLong();
        g.counts = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object();
        g.updated = q.value(3).toLongLong();
        return g;
    }
    return std::nullopt;
}

bool putGameState(const GameState& g) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO gamestates (resource, version, counts, "
                             "updated) VALUES (?, ?, ?, ?)"));
    q.addBindValue(text(g.resource));
    q.addBindValue(g.version);
    q.addBindValue(QString::fromUtf8(QJsonDocument(g.counts).toJson(QJsonDocument::Compact)));
    q.addBindValue(g.updated ? g.updated : now());
    return run(q, "save gamestate");
}

bool removeGameState(const QString& resource) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM gamestates WHERE resource = ?"));
    q.addBindValue(text(resource));
    return run(q, "remove gamestate") && q.numRowsAffected() > 0;
}

QList<GameState> gameStates() {
    QList<GameState> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "SELECT resource, version, counts, updated FROM gamestates ORDER BY resource"));
    if (run(q, "list gamestates")) {
        while (q.next()) {
            GameState g;
            g.resource = q.value(0).toString();
            g.version = q.value(1).toLongLong();
            g.counts = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object();
            g.updated = q.value(3).toLongLong();
            out.append(g);
        }
    }
    return out;
}

// ---- Fame -------------------------------------------------------------------------------------

std::optional<Fame> fame(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "SELECT account_id, xp, stars, elite, updated FROM fame WHERE account_id = ?"));
    q.addBindValue(accountId);
    if (run(q, "read fame") && q.next()) {
        Fame f;
        f.accountId = q.value(0).toLongLong();
        f.xp = q.value(1).toLongLong();
        if (!q.value(2).isNull()) {
            f.stars = q.value(2).toLongLong();
        }
        if (!q.value(3).isNull()) {
            f.elite = q.value(3).toLongLong();
        }
        f.updated = q.value(4).toLongLong();
        return f;
    }
    return std::nullopt;
}

bool putFame(const Fame& f) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO fame (account_id, xp, stars, elite, updated) "
                             "VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(f.accountId);
    q.addBindValue(f.xp);
    q.addBindValue(f.stars ? QVariant(*f.stars) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(f.elite ? QVariant(*f.elite) : QVariant(QMetaType(QMetaType::LongLong)));
    q.addBindValue(f.updated ? f.updated : now());
    return run(q, "save fame");
}

// ---- Accolades --------------------------------------------------------------------------------

QList<Accolade> accolades(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT account_id, name, progress, updated FROM accolades "
                             "WHERE account_id = ? ORDER BY name"));
    q.addBindValue(accountId);
    QList<Accolade> out;
    if (run(q, "read accolades")) {
        while (q.next()) {
            out.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong(),
                        q.value(3).toLongLong()});
        }
    }
    return out;
}

qint64 addAccoladeProgress(qint64 accountId, const QString& name, qint64 delta) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "INSERT INTO accolades (account_id, name, progress, updated) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (account_id, name) DO UPDATE SET progress = progress + excluded.progress, "
        "updated = excluded.updated"));
    q.addBindValue(accountId);
    q.addBindValue(text(name));
    q.addBindValue(delta);
    q.addBindValue(now());
    run(q, "save accolade");
    QSqlQuery r(connection());
    r.prepare(QStringLiteral("SELECT progress FROM accolades WHERE account_id = ? AND name = ?"));
    r.addBindValue(accountId);
    r.addBindValue(text(name));
    return run(r, "read accolade") && r.next() ? r.value(0).toLongLong() : 0;
}

QList<Accolade> clubAccolades(qint64 clubId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT club_id, name, progress, updated FROM club_accolades "
                             "WHERE club_id = ? ORDER BY name"));
    q.addBindValue(clubId);
    QList<Accolade> out;
    if (run(q, "read club accolades")) {
        while (q.next()) {
            out.append({q.value(0).toLongLong(), q.value(1).toString(), q.value(2).toLongLong(),
                        q.value(3).toLongLong()});
        }
    }
    return out;
}

qint64 addClubAccoladeProgress(qint64 clubId, const QString& name, qint64 delta) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "INSERT INTO club_accolades (club_id, name, progress, updated) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (club_id, name) DO UPDATE SET progress = progress + excluded.progress, "
        "updated = excluded.updated"));
    q.addBindValue(clubId);
    q.addBindValue(text(name));
    q.addBindValue(delta);
    q.addBindValue(now());
    run(q, "save club accolade");
    QSqlQuery r(connection());
    r.prepare(QStringLiteral("SELECT progress FROM club_accolades WHERE club_id = ? AND name = ?"));
    r.addBindValue(clubId);
    r.addBindValue(text(name));
    return run(r, "read club accolade") && r.next() ? r.value(0).toLongLong() : 0;
}

QList<qint64> accoladeAccounts() {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT DISTINCT account_id FROM accolades ORDER BY account_id"));
    QList<qint64> out;
    if (run(q, "list accolade accounts")) {
        while (q.next()) {
            out.append(q.value(0).toLongLong());
        }
    }
    return out;
}

bool removeAccolades(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM accolades WHERE account_id = ?"));
    q.addBindValue(accountId);
    return run(q, "remove accolades") && q.numRowsAffected() > 0;
}

// ---- Friends ----------------------------------------------------------------------------------

QStringList friendNames(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT name FROM friends WHERE account_id = ? ORDER BY name"));
    q.addBindValue(accountId);
    QStringList out;
    if (run(q, "read friends")) {
        while (q.next()) {
            out.append(q.value(0).toString());
        }
    }
    return out;
}

bool addFriend(qint64 accountId, const QString& name) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(
        QStringLiteral("INSERT OR IGNORE INTO friends (account_id, name, added) VALUES (?, ?, ?)"));
    q.addBindValue(accountId);
    q.addBindValue(text(name));
    q.addBindValue(now());
    return run(q, "add friend") && q.numRowsAffected() > 0;
}

bool removeFriend(qint64 accountId, const QString& name) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM friends WHERE account_id = ? AND name = ?"));
    q.addBindValue(accountId);
    q.addBindValue(text(name));
    return run(q, "remove friend") && q.numRowsAffected() > 0;
}

QStringList allFriendNames() {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT DISTINCT name FROM friends"));
    QStringList out;
    if (run(q, "list friend names")) {
        while (q.next()) {
            out.append(q.value(0).toString());
        }
    }
    return out;
}

bool hasFriendList(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT 1 FROM friends WHERE account_id = ? LIMIT 1"));
    q.addBindValue(accountId);
    return run(q, "check friends") && q.next();
}

// ---- Clubs ------------------------------------------------------------------------------------

namespace {
Club clubFrom(const QSqlQuery& q) {
    Club c;
    c.id = q.value(0).toLongLong();
    c.name = q.value(1).toString();
    c.membership = q.value(2).toInt();
    c.status = q.value(3).toInt();
    c.xp = q.value(4).toLongLong();
    c.customization = q.value(5).toByteArray();
    c.created = q.value(6).toLongLong();
    return c;
}
constexpr char ClubColumns[] = "id, name, membership, status, xp, customization, created";

ClubInvite inviteFrom(const QSqlQuery& q) {
    ClubInvite i;
    i.id = q.value(0).toLongLong();
    i.clubId = q.value(1).toLongLong();
    i.accountId = q.value(2).toLongLong();
    i.invitedBy = q.value(3).toLongLong();
    i.status = q.value(4).toInt();
    i.created = q.value(5).toLongLong();
    i.updated = q.value(6).toLongLong();
    i.expires = q.value(7).toLongLong();
    return i;
}
constexpr char InviteColumns[] =
    "id, club_id, account_id, invited_by, status, created, updated, expires";
} // namespace

QList<Club> clubs() {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM clubs ORDER BY id").arg(QLatin1String(ClubColumns)));
    QList<Club> out;
    if (run(q, "list clubs")) {
        while (q.next()) {
            out.append(clubFrom(q));
        }
    }
    return out;
}

std::optional<Club> club(qint64 id) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM clubs WHERE id = ?").arg(QLatin1String(ClubColumns)));
    q.addBindValue(id);
    if (run(q, "read club") && q.next()) {
        return clubFrom(q);
    }
    return std::nullopt;
}

std::optional<Club> clubByName(const QString& name) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(
        QStringLiteral("SELECT %1 FROM clubs WHERE name = ?").arg(QLatin1String(ClubColumns)));
    q.addBindValue(text(name));
    if (run(q, "read club by name") && q.next()) {
        return clubFrom(q);
    }
    return std::nullopt;
}

qint64 createClub(const QString& name, int membership, qint64 owner) {
    QMutexLocker l(&lock());
    QSqlDatabase d = connection();
    d.transaction();
    QSqlQuery q(d);
    q.prepare(QStringLiteral("INSERT INTO clubs (name, membership, created) VALUES (?, ?, ?)"));
    q.addBindValue(text(name));
    q.addBindValue(membership);
    q.addBindValue(now());
    if (!run(q, "create club")) {
        d.rollback();
        return 0;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    QSqlQuery m(d);
    m.prepare(
        QStringLiteral("INSERT OR REPLACE INTO club_members (account_id, club_id, type, joined) "
                       "VALUES (?, ?, ?, ?)"));
    m.addBindValue(owner);
    m.addBindValue(id);
    m.addBindValue(ClubOwner);
    m.addBindValue(now());
    if (!run(m, "add club owner")) {
        d.rollback();
        return 0;
    }
    d.commit();
    return id;
}

bool updateClub(const Club& c) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("UPDATE clubs SET name = ?, membership = ?, status = ?, xp = ?, "
                             "customization = ? WHERE id = ?"));
    q.addBindValue(text(c.name));
    q.addBindValue(c.membership);
    q.addBindValue(c.status);
    q.addBindValue(c.xp);
    q.addBindValue(c.customization.isNull() ? QByteArray("") : c.customization);
    q.addBindValue(c.id);
    return run(q, "save club");
}

qint64 addClubXp(qint64 clubId, qint64 delta) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("UPDATE clubs SET xp = xp + ? WHERE id = ?"));
    q.addBindValue(delta);
    q.addBindValue(clubId);
    run(q, "add club fame");
    QSqlQuery r(connection());
    r.prepare(QStringLiteral("SELECT xp FROM clubs WHERE id = ?"));
    r.addBindValue(clubId);
    return run(r, "read club fame") && r.next() ? r.value(0).toLongLong() : 0;
}

bool removeClub(qint64 id) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM clubs WHERE id = ?"));
    q.addBindValue(id);
    return run(q, "remove club") && q.numRowsAffected() > 0;
}

QList<ClubMember> clubMembers(qint64 clubId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT account_id, club_id, type, mvp, joined FROM club_members "
                             "WHERE club_id = ? ORDER BY joined, account_id"));
    q.addBindValue(clubId);
    QList<ClubMember> out;
    if (run(q, "list club members")) {
        while (q.next()) {
            out.append({q.value(0).toLongLong(), q.value(1).toLongLong(), q.value(2).toInt(),
                        q.value(3).toBool(), q.value(4).toLongLong()});
        }
    }
    return out;
}

std::optional<ClubMember> clubMember(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT account_id, club_id, type, mvp, joined FROM club_members "
                             "WHERE account_id = ?"));
    q.addBindValue(accountId);
    if (run(q, "read club member") && q.next()) {
        return ClubMember{q.value(0).toLongLong(), q.value(1).toLongLong(), q.value(2).toInt(),
                          q.value(3).toBool(), q.value(4).toLongLong()};
    }
    return std::nullopt;
}

bool putClubMember(const ClubMember& m) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO club_members (account_id, club_id, type, mvp, "
                             "joined) VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(m.accountId);
    q.addBindValue(m.clubId);
    q.addBindValue(m.type);
    q.addBindValue(m.mvp ? 1 : 0);
    q.addBindValue(m.joined ? m.joined : now());
    return run(q, "save club member");
}

bool removeClubMember(qint64 accountId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM club_members WHERE account_id = ?"));
    q.addBindValue(accountId);
    return run(q, "remove club member") && q.numRowsAffected() > 0;
}

QList<ClubInvite> clubInvites() {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM club_invites ORDER BY created DESC, id DESC")
                  .arg(QLatin1String(InviteColumns)));
    QList<ClubInvite> out;
    if (run(q, "list club invites")) {
        while (q.next()) {
            out.append(inviteFrom(q));
        }
    }
    return out;
}

std::optional<ClubInvite> clubInvite(qint64 id) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1 FROM club_invites WHERE id = ?")
                  .arg(QLatin1String(InviteColumns)));
    q.addBindValue(id);
    if (run(q, "read club invite") && q.next()) {
        return inviteFrom(q);
    }
    return std::nullopt;
}

qint64 addClubInvite(qint64 clubId, qint64 accountId, qint64 invitedBy, int status, qint64 ttlMs) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT INTO club_invites (club_id, account_id, invited_by, status, "
                             "created, updated, expires) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    const qint64 t = now();
    q.addBindValue(clubId);
    q.addBindValue(accountId);
    q.addBindValue(invitedBy);
    q.addBindValue(status);
    q.addBindValue(t);
    q.addBindValue(t);
    q.addBindValue(t + ttlMs);
    return run(q, "add club invite") ? q.lastInsertId().toLongLong() : 0;
}

int closeClubInvites(qint64 accountId, qint64 clubId, qint64 inviteId) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    QString where = QStringLiteral("status <> ?");
    if (inviteId) {
        where += QStringLiteral(" AND id = ?");
    } else {
        where += QStringLiteral(" AND account_id = ?");
        if (clubId) {
            where += QStringLiteral(" AND club_id = ?");
        }
    }
    q.prepare(QStringLiteral("UPDATE club_invites SET status = ?, updated = ? WHERE ") + where);
    q.addBindValue(InviteClosed);
    q.addBindValue(now());
    q.addBindValue(InviteClosed);
    if (inviteId) {
        q.addBindValue(inviteId);
    } else {
        q.addBindValue(accountId);
        if (clubId) {
            q.addBindValue(clubId);
        }
    }
    return run(q, "close club invites") ? q.numRowsAffected() : 0;
}

qint64 addClubReport(const ClubReport& r) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT INTO club_reports (club_id, club_name, reporter, language, "
                             "reason, created) VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(r.clubId);
    q.addBindValue(text(r.clubName));
    q.addBindValue(r.reporter);
    q.addBindValue(r.language);
    q.addBindValue(r.reason);
    q.addBindValue(r.created ? r.created : now());
    return run(q, "add club report") ? q.lastInsertId().toLongLong() : 0;
}

QList<ClubReport> clubReports() {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT id, club_id, club_name, reporter, language, reason, created "
                             "FROM club_reports ORDER BY created DESC, id DESC"));
    QList<ClubReport> out;
    if (run(q, "list club reports")) {
        while (q.next()) {
            out.append({q.value(0).toLongLong(), q.value(1).toLongLong(), q.value(2).toString(),
                        q.value(3).toLongLong(), q.value(4).toLongLong(), q.value(5).toLongLong(),
                        q.value(6).toLongLong()});
        }
    }
    return out;
}

bool removeClubReport(qint64 id) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM club_reports WHERE id = ?"));
    q.addBindValue(id);
    return run(q, "remove club report") && q.numRowsAffected() > 0;
}

// ---- Ghosts -----------------------------------------------------------------------------------

qint64 putGhost(const Ghost& g, const QByteArray& data) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("INSERT INTO ghosts (account_id, mime_type, checksum, size, data, "
                             "created) VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(g.accountId);
    q.addBindValue(text(g.mimeType));
    q.addBindValue(text(g.checksum));
    q.addBindValue(qint64(data.size()));
    q.addBindValue(data);
    q.addBindValue(g.created ? g.created : now());
    if (!run(q, "save ghost")) {
        return 0;
    }
    return q.lastInsertId().toLongLong();
}

std::optional<qint64> findGhost(qint64 accountId, const QString& mimeType, const QString& checksum,
                                qint64 size) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT id FROM ghosts WHERE account_id = ? AND mime_type = ? AND "
                             "checksum = ? AND size = ? ORDER BY id LIMIT 1"));
    q.addBindValue(accountId);
    q.addBindValue(text(mimeType));
    q.addBindValue(text(checksum));
    q.addBindValue(size);
    if (run(q, "find ghost") && q.next()) {
        return q.value(0).toLongLong();
    }
    return std::nullopt;
}

namespace {
Ghost ghostFrom(const QSqlQuery& q) {
    Ghost g;
    g.id = q.value(0).toLongLong();
    g.accountId = q.value(1).toLongLong();
    g.mimeType = q.value(2).toString();
    g.checksum = q.value(3).toString();
    g.size = q.value(4).toLongLong();
    g.created = q.value(5).toLongLong();
    g.downloads = q.value(6).toInt();
    g.lastDownload = q.value(7).toLongLong();
    return g;
}
constexpr char GhostColumns[] =
    "id, account_id, mime_type, checksum, size, created, downloads, last_download";
} // namespace

std::optional<Ghost> ghost(qint64 id, QByteArray* data) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("SELECT %1%2 FROM ghosts WHERE id = ?")
                  .arg(QLatin1String(GhostColumns), data ? QStringLiteral(", data") : QString()));
    q.addBindValue(id);
    if (run(q, "read ghost") && q.next()) {
        if (data) {
            *data = q.value(8).toByteArray();
        }
        return ghostFrom(q);
    }
    return std::nullopt;
}

void countGhostDownload(qint64 id) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral(
        "UPDATE ghosts SET downloads = downloads + 1, last_download = ? WHERE id = ?"));
    q.addBindValue(now());
    q.addBindValue(id);
    run(q, "count ghost download");
}

bool removeGhost(qint64 id) {
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(QStringLiteral("DELETE FROM ghosts WHERE id = ?"));
    q.addBindValue(id);
    return run(q, "remove ghost") && q.numRowsAffected() > 0;
}

QList<Ghost> ghosts() {
    QList<Ghost> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    q.prepare(
        QStringLiteral("SELECT %1 FROM ghosts ORDER BY id DESC").arg(QLatin1String(GhostColumns)));
    if (run(q, "list ghosts")) {
        while (q.next()) {
            out.append(ghostFrom(q));
        }
    }
    return out;
}

} // namespace db
