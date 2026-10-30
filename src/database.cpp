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

// Players

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

// Leaderboards

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

QList<Score> scoresWithGhosts() {
    QList<Score> out;
    QMutexLocker l(&lock());
    QSqlQuery q(connection());
    // "ghost:-1" is the game's "no ghost"; the caller parses the id.
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

// EGS sessions

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

// OTG tokens

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

// OTG counter gamestates

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

// Fame

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

// Ghosts

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
