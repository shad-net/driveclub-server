// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "otg.h"

#include "auth.h"
#include "database.h"
#include "egs.h"
#include "eventlists.h"

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

#include <algorithm>
#include <limits>

namespace {

Q_LOGGING_CATEGORY(lcOtg, "otg")

OtgConfig& config() {
    static OtgConfig c;
    return c;
}

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

Tokens& tokens() {
    static Tokens t;
    return t;
}

QString baseUrl(const HttpRequest& request) {
    if (!config().baseUrl.isEmpty()) {
        return config().baseUrl;
    }
    const QByteArray host = request.headers.value("host");
    if (!host.isEmpty()) {
        return QStringLiteral("http://") + QString::fromLatin1(host);
    }
    return QStringLiteral("http://127.0.0.1:%1").arg(config().listenPort);
}

HttpResponse json(const QJsonDocument& doc, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/json";
    r.body = doc.toJson(QJsonDocument::Compact);
    return r;
}

HttpResponse login(const HttpRequest& request, const QString& who, const AuthResult& auth) {
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool verified = auth.outcome == AuthResult::Outcome::Verified;
    Token t;
    t.name = verified ? auth.onlineId : who;
    t.account = verified ? auth.accountId : QString();
    t.created = QDateTime::currentMSecsSinceEpoch();
    t.lastUsed = t.created;
    {
        QMutexLocker lock(&tokens().mutex);
        tokens().byToken.insert(token, t);
    }
    db::putToken({token, t.name, t.account, t.created, t.lastUsed, 0});
    const QString base = baseUrl(request);
    qCInfo(lcOtg).noquote() << "OTG login" << t.name
                            << (verified ? "(shadNet verified)" : "(not verified)") << "-> token"
                            << token << "gateway" << base;
    return json(QJsonDocument(QJsonObject{
        {QStringLiteral("token"), token},
        {QStringLiteral("config"), QJsonObject{{QStringLiteral("gateway"), base}}},
        {QStringLiteral("zone"), config().zone},
    }));
}

QString tokenOwner(const HttpRequest& request) {
    QByteArray value = request.headers.value("authorization");
    if (!value.startsWith("Bearer ")) {
        return {};
    }
    const QString token = QString::fromLatin1(value.mid(7)).trimmed();
    QMutexLocker lock(&tokens().mutex);
    auto it = tokens().byToken.find(token);
    if (it == tokens().byToken.end()) {
        return {};
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool save = now - it->lastUsed >= 10000;
    it->lastUsed = now;
    ++it->requests;
    if (save) {
        db::touchToken(token, it->lastUsed, it->requests);
    }
    return it->name;
}

// Leaderboards

using Entry = db::Score;

QJsonValue scoreJson(quint64 score) {
    if (score <= quint64(std::numeric_limits<qint64>::max())) {
        return QJsonValue(qint64(score));
    }
    return QJsonValue(double(score));
}

QJsonObject entryJson(const Entry& e, qsizetype position) {
    QJsonObject o{
        {QStringLiteral("position"), qint64(position)},
        {QStringLiteral("score"), scoreJson(e.score)},
        {QStringLiteral("entrant"), QJsonObject{{QStringLiteral("id"), e.entrant}}},
    };
    const bool empty = e.attachment.isEmpty() ||
                       (e.attachment.size() == 1 && e.attachment.first().toString().isEmpty());
    if (!empty) {
        o.insert(QStringLiteral("attachment"), e.attachment);
    }
    return o;
}

QString boardName(QString resource) {
    if (resource.endsWith(QLatin1String("/~leaderboard"))) {
        resource.chop(13);
    }
    if (resource.startsWith(QLatin1String("scores/"))) {
        resource.remove(0, 7);
    }
    return resource;
}

HttpResponse readBoard(const HttpRequest& request, const QString& name) {
    const QString method = request.query.queryItemValue(QStringLiteral("_method"));
    if (method.isEmpty()) {
        const qsizetype size = db::boardSize(name);
        qCInfo(lcOtg).noquote() << "leaderboard" << name << "size ->" << size;
        return json(QJsonDocument(QJsonObject{{QStringLiteral("size"), qint64(size)}}));
    }
    const int count =
        std::clamp(request.query.queryItemValue(QStringLiteral("count")).toInt(), 0, 500);
    const QList<Entry> list = db::boardScores(name);
    QJsonArray out;
    QByteArray nextToken;

    if (method == QLatin1String("select")) {
        const QStringList wanted =
            request.query.queryItemValue(QStringLiteral("entrants")).split(',', Qt::SkipEmptyParts);
        for (qsizetype i = 0; i < list.size(); ++i) {
            if (wanted.contains(list.at(i).entrant)) {
                out.append(entryJson(list.at(i), i + 1));
            }
        }
    } else if (method == QLatin1String("nearby")) {
        const QString who = request.query.queryItemValue(QStringLiteral("entrant"));
        const int n = count > 0 ? count : 10;
        qsizetype at = -1;
        for (qsizetype i = 0; i < list.size(); ++i) {
            if (list.at(i).entrant == who) {
                at = i;
                break;
            }
        }
        if (at >= 0) {
            const qsizetype from = std::max<qsizetype>(0, at - n / 2);
            for (qsizetype i = from; i < list.size() && i < from + n; ++i) {
                out.append(entryJson(list.at(i), i + 1));
            }
        }
    } else {
        // page (default): count entries from offset <from>
        const int n = count > 0 ? count : 50;
        const qsizetype from =
            std::max(0, request.query.queryItemValue(QStringLiteral("from")).toInt());
        for (qsizetype i = from; i < list.size() && i < from + n; ++i) {
            out.append(entryJson(list.at(i), i + 1));
        }
        if (from + n < list.size()) {
            nextToken = QByteArray::number(from + n);
        }
    }

    qCInfo(lcOtg).noquote() << "leaderboard" << name << method << "->" << out.size() << "of"
                            << list.size() << "entries";
    HttpResponse r = json(QJsonDocument(out));
    r.headers.append(qMakePair(QByteArray("X-Count"), QByteArray::number(list.size())));
    if (!nextToken.isEmpty()) {
        r.headers.append(qMakePair(QByteArray("X-Next-Token"), nextToken));
    }
    return r;
}

bool isChallengeList(const QString& resource) {
    return resource.startsWith(QLatin1String("challenges/player/")) ||
           resource.startsWith(QLatin1String("challenges/club/"));
}

HttpResponse writeBoard(const HttpRequest& request, const QString& name, bool latestWins) {
    const QJsonDocument doc = QJsonDocument::fromJson(request.body);
    const QJsonArray items = doc.isArray() ? doc.array() : QJsonArray{doc.object()};
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.board = name;
        e.entrant = o.value(QStringLiteral("entrant")).isObject()
                        ? o.value(QStringLiteral("entrant")).toObject().value("id").toString()
                        : o.value(QStringLiteral("entrant")).toVariant().toString();
        e.score = o.value(QStringLiteral("score")).toVariant().toULongLong();
        e.attachment = o.value(QStringLiteral("attachment")).toArray();
        e.updated = QDateTime::currentMSecsSinceEpoch();
        if (e.entrant.isEmpty()) {
            continue;
        }
        const auto old = db::score(name, e.entrant);
        const bool better = latestWins || !old || e.score > old->score;
        if (better) {
            db::putScore(e);
        }
        if (latestWins) {
            const quint64 ends = e.score >> 8;
            qCInfo(lcOtg).noquote()
                << "challenge" << e.entrant << (old ? "updated on" : "added to") << name
                << QStringLiteral("(ends %1 game time, flags %2)")
                       .arg(QDateTime::fromMSecsSinceEpoch(qint64(ends), QTimeZone::utc())
                                .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")))
                       .arg(e.score & 0xff);
        } else {
            qCInfo(lcOtg).noquote()
                << "score" << name << "entrant" << e.entrant << "score" << e.score
                << (better ? QStringLiteral("(stored)")
                           : QStringLiteral("(kept better %1)").arg(old->score));
        }
    }
    return json(QJsonDocument(QJsonObject{{QStringLiteral("is_success"), true}}));
}

// Counter gamestates

HttpResponse gameStateJson(const db::GameState& g) {
    return json(QJsonDocument(QJsonObject{
        {QStringLiteral("version"), g.version},
        {QStringLiteral("counts"), g.counts},
    }));
}

// The game never reports its fame by itself (profileService.updateFame is not called): a player's
// total is the event fame in their stats gamestate, which the fame leaderboard and levels show.
// XP boost: lower-case player name -> factor for the event fame they upload.
QMutex& xpBoostMutex() {
    static QMutex m;
    return m;
}
QHash<QString, int>& xpBoosts() {
    static QHash<QString, int> h;
    return h;
}

int xpBoostOf(qint64 accountId) {
    QMutexLocker lock(&xpBoostMutex());
    return xpBoosts().value(egsPlayerName(accountId).toLower(), 1);
}

void fameFromStats(const db::GameState& g) {
    static const QRegularExpression player(QStringLiteral("^stats/V\\d+/player/(\\d+)$"));
    const QRegularExpressionMatch m = player.match(g.resource);
    if (!m.hasMatch()) {
        return;
    }
    qint64 fame = 0;
    for (auto it = g.counts.constBegin(); it != g.counts.constEnd(); ++it) {
        if (it.key().endsWith(QLatin1String("/event/fame"))) {
            fame += it.value().toInteger();
        }
    }
    const qint64 account = m.captured(1).toLongLong();
    if (fame > db::fame(account).value_or(db::Fame{}).xp) {
        egsSetFame(account, fame);
    }
}

HttpResponse handleGameState(const HttpRequest& request, const QString& resource) {
    if (request.method == "GET") {
        auto g = db::gameState(resource);
        // A club's stats and accolades the game never uploads: its members' counters summed.
        static const QRegularExpression clubState(QStringLiteral("^(\\w+/V\\d+)/club/(\\d+)$"));
        if (const QRegularExpressionMatch m = clubState.match(resource); !g && m.hasMatch()) {
            db::GameState sum{resource, 1, {}, 0};
            for (const db::ClubMember& member : db::clubMembers(m.captured(2).toLongLong())) {
                const auto own = db::gameState(QStringLiteral("%1/player/%2").arg(m.captured(1)).arg(member.accountId));
                if (!own) {
                    continue;
                }
                for (auto it = own->counts.constBegin(); it != own->counts.constEnd(); ++it) {
                    sum.counts.insert(it.key(), sum.counts.value(it.key()).toInteger() + it.value().toInteger());
                }
            }
            if (!sum.counts.isEmpty()) {
                g = sum;
            }
        }
        if (!g) {
            qCInfo(lcOtg).noquote()
                << "gamestate" << resource << "-> none yet (404, game defaults)";
            HttpResponse r = json(QJsonDocument(QJsonObject{}), 404);
            r.expected = true; // "nothing stored yet": the game uses its defaults
            return r;
        }
        qCInfo(lcOtg).noquote() << "gamestate" << resource << "-> version" << g->version << "with"
                                << g->counts.size() << "counters";
        fameFromStats(*g);
        return gameStateJson(*g);
    }
    if (request.method != "POST" && request.method != "PUT") {
        return json(QJsonDocument(QJsonObject{{QStringLiteral("error"), QStringLiteral("method")}}),
                    405);
    }
    const QJsonObject body = QJsonDocument::fromJson(request.body).object();
    db::GameState g = db::gameState(resource).value_or(db::GameState{resource, 0, {}, 0});
    const qint64 parent = body.value(QStringLiteral("parent")).toInteger(-1);
    if (parent != g.version) {
        qCInfo(lcOtg).noquote() << "gamestate" << resource << "write based on version" << parent
                                << "but stored is" << g.version << "(applied anyway)";
    }
    QStringList changes;
    static const QRegularExpression statsOf(QStringLiteral("^stats/V\\d+/player/(\\d+)$"));
    const QRegularExpressionMatch statsMatch = statsOf.match(resource);
    const int boost = statsMatch.hasMatch() ? xpBoostOf(statsMatch.captured(1).toLongLong()) : 1;
    bool eventDone = false; // Mode_<mode>/event/attempts went up: the player finished an event
    for (const QJsonValue& t : body.value(QStringLiteral("transactions")).toArray()) {
        const QJsonObject deltas = t.toObject().value(QStringLiteral("deltas")).toObject();
        for (auto it = deltas.constBegin(); it != deltas.constEnd(); ++it) {
            qint64 delta = it.value().toInteger();
            if (boost > 1 && delta > 0 && it.key().endsWith(QLatin1String("/event/fame"))) {
                delta *= boost; // XP boost: the event's fame counts more
            }
            eventDone = eventDone || (delta > 0 && it.key().startsWith(QLatin1String("Mode_")) &&
                                      it.key().endsWith(QLatin1String("/event/attempts")));
            const qint64 value = std::max<qint64>(0, g.counts.value(it.key()).toInteger() + delta);
            g.counts.insert(it.key(), value);
            changes << QStringLiteral("%1 %2%3 = %4")
                           .arg(it.key(), delta >= 0 ? QStringLiteral("+") : QString())
                           .arg(delta)
                           .arg(value);
        }
    }
    ++g.version;
    g.updated = QDateTime::currentMSecsSinceEpoch();
    db::putGameState(g);
    fameFromStats(g);
    static const QRegularExpression playerStats(QStringLiteral("^stats/V\\d+/player/(\\d+)$"));
    if (const QRegularExpressionMatch m = playerStats.match(resource); eventDone && m.hasMatch()) {
        eventListsRaceFinished(egsPlayerName(m.captured(1).toLongLong()));
    }
    qCInfo(lcOtg).noquote() << "gamestate" << resource << "-> version" << g.version << ":"
                            << (changes.isEmpty() ? QStringLiteral("no changes")
                                                  : changes.join(QStringLiteral(", ")));
    return gameStateJson(g);
}

} // namespace

void setOtgConfig(const OtgConfig& c) {
    config() = c;
}

QByteArray otgXpBoost(const QString& player, const QString& command) {
    QMutexLocker lock(&xpBoostMutex());
    int& factor = xpBoosts()[player.toLower()];
    if (factor < 1) {
        factor = 1;
    }
    if (command == QLatin1String("toggle")) {
        factor = factor > 1 ? 1 : 2;
        qCInfo(lcOtg).noquote() << "XP boost of" << player << (factor > 1 ? "on (x2)" : "off");
    }
    return factor > 1 ? QByteArrayLiteral("XP boost: on (x2 fame)") : QByteArrayLiteral("XP boost: off");
}

void otgFameFromStats() {
    for (const db::GameState& g : db::gameStates()) {
        fameFromStats(g);
    }
    egsUpdateClubFameBoard();
}

QString otgBaseUrl(const HttpRequest& request) {
    return baseUrl(request);
}

bool handleOtg(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond) {
    static const QRegularExpression ghostPath(QStringLiteral("^ghosts/(\\d+)$"));
    if (const auto g = ghostPath.match(endpoint); g.hasMatch()) {
        QByteArray data;
        const qint64 id = g.captured(1).toLongLong();
        const auto ghost = request.method == "GET" ? db::ghost(id, &data) : std::nullopt;
        HttpResponse r;
        if (!ghost) {
            qCWarning(lcOtg).noquote() << "ghost" << id << "requested but not stored";
            r.status = 404;
            r.contentType = "text/plain";
            r.body = "Not Found";
        } else {
            db::countGhostDownload(id);
            qCInfo(lcOtg).noquote() << "ghost" << id << "of" << egsPlayerName(ghost->accountId)
                                    << "downloaded," << data.size() << "bytes";
            r.contentType = ghost->mimeType.isEmpty() ? QByteArray("application/octet-stream")
                                                      : ghost->mimeType.toLatin1();
            r.body = data;
        }
        respond(r);
        return true;
    }
    if (endpoint == QLatin1String("titles/driveclub/domains")) {
        const QString base = baseUrl(request);
        qCInfo(lcOtg).noquote() << "domain list: id" << config().domainId << "auth" << base;
        respond(json(QJsonDocument(QJsonArray{QJsonObject{
            {QStringLiteral("id"), config().domainId},
            {QStringLiteral("auth"), base},
        }})));
        return true;
    }
    if (endpoint == QLatin1String("login/driveclub")) {
        // provider=psn&code=<NpAuth authorization code>, checked with shadNet (auth.cpp).
        const QString code = request.query.queryItemValue(QStringLiteral("code"));
        verifyCode(QStringLiteral("OTG"), code, [request, respond](const AuthResult& auth) {
            if (auth.outcome == AuthResult::Outcome::Denied) {
                qCWarning(lcOtg).noquote() << "OTG login denied:" << auth.message;
                respond(json(QJsonDocument(QJsonObject{{QStringLiteral("error"), auth.message}}),
                             auth.unavailable ? 503 : 401));
                return;
            }
            respond(login(request, QStringLiteral("psn"), auth));
        });
        return true;
    }
    if (endpoint == QLatin1String("xlogin/driveclub")) {
        // OTG_Login_Type "fake": no code to check. Refused when unverified logins are not allowed.
        if (!authConfig().shadnetUrl.isEmpty() && !authConfig().allowUnverified) {
            qCWarning(lcOtg) << "fake OTG login refused (shadnet.allow_unverified is off)";
            respond(json(QJsonDocument(QJsonObject{
                             {QStringLiteral("error"), QStringLiteral("fake login not allowed")}}),
                         401));
            return true;
        }
        respond(login(request, request.query.queryItemValue(QStringLiteral("user")), {}));
        return true;
    }

    static const QRegularExpression service(QStringLiteral("^([^/]+)/([^/]+)/(v\\d+)/(.+)$"));
    const auto m = service.match(endpoint);
    if (!m.hasMatch()) {
        return false;
    }
    const QString resource = m.captured(4);
    const QString board = boardName(resource);
    if (tokenOwner(request).isEmpty()) {
        qCWarning(lcOtg).noquote()
            << request.method << resource << "with an unknown token (server restarted?)";
    }
    if (resource.endsWith(QLatin1String("/~gamestate"))) {
        respond(handleGameState(request, resource.chopped(11)));
        return true;
    }
    const bool isBoard = resource.endsWith(QLatin1String("/~leaderboard"));
    if (request.method == "GET" && isBoard) {
        respond(readBoard(request, board));
        return true;
    }
    if (request.method == "POST" || request.method == "PUT") {
        const QJsonDocument doc = QJsonDocument::fromJson(request.body);
        const QJsonArray arr = doc.isArray() ? doc.array() : QJsonArray{};
        if (!arr.isEmpty() && arr.first().toObject().contains(QStringLiteral("score"))) {
            respond(writeBoard(request, board, isChallengeList(resource)));
            return true;
        }
    }
    qCWarning(lcOtg).noquote() << "no OTG handler for" << request.method << resource << "(zone"
                               << m.captured(1) << ", server" << m.captured(2) << ")";
    HttpResponse r;
    r.status = 404;
    r.contentType = "application/json";
    r.body = "{}";
    respond(r);
    return true;
}

QList<OtgTokenInfo> otgTokens() {
    QMutexLocker lock(&tokens().mutex);
    QList<OtgTokenInfo> out;
    for (auto it = tokens().byToken.constBegin(); it != tokens().byToken.constEnd(); ++it) {
        out.append({it.key(), it->name, it->account, it->created, it->lastUsed, it->requests});
    }
    return out;
}

bool otgDropToken(const QString& token) {
    db::removeToken(token);
    QMutexLocker lock(&tokens().mutex);
    return tokens().byToken.remove(token) > 0;
}

void otgRestoreTokens() {
    QMutexLocker lock(&tokens().mutex);
    for (const db::Token& t : db::tokens()) {
        tokens().byToken.insert(t.token, {t.name, t.account, t.created, t.lastUsed, t.requests});
    }
    if (!tokens().byToken.isEmpty()) {
        qCInfo(lcOtg).noquote() << "restored" << tokens().byToken.size() << "OTG tokens";
    }
}

QString otgBoardKind(const QString& name) {
    static const QRegularExpression challengeOwner(
        QStringLiteral("^challenges/(player|club)/(\\d+)$"));
    static const QRegularExpression challengeBoard(
        QStringLiteral("^challenges/(player|club)/-?\\d+h\\d+m\\d+s\\d+$"));
    if (name.startsWith(QLatin1String("eventhistory/"))) {
        return QStringLiteral("event_history");
    }
    if (challengeBoard.match(name).hasMatch()) {
        return QStringLiteral("challenge_board");
    }
    if (challengeOwner.match(name).hasMatch() || name.startsWith(QLatin1String("challenges/"))) {
        return QStringLiteral("challenge_list");
    }
    return QStringLiteral("leaderboard");
}

QJsonArray otgChallenges() {
    static const QRegularExpression id(QStringLiteral("^(-?\\d+)h(\\d+)m(\\d+)s(\\d+)$"));
    struct Info {
        QString scope;
        QString creator;
        qint64 ends = 0;
        QJsonArray members;
        QJsonArray attachment;
        qint64 updated = 0;
    };
    QMap<QString, Info> byId;
    for (const db::BoardInfo& b : db::boards()) {
        if (otgBoardKind(b.name) != QLatin1String("challenge_list")) {
            continue;
        }
        const QStringList parts = b.name.split('/');
        const QString owner = parts.value(2);
        for (const Entry& e : db::boardScores(b.name)) {
            const auto m = id.match(e.entrant);
            Info& info = byId[e.entrant];
            info.scope = parts.value(1);
            info.creator = m.hasMatch() ? m.captured(4) : QString();
            info.ends = std::max(info.ends, qint64(e.score >> 8));
            info.updated = std::max(info.updated, e.updated);
            const qint64 flags = qint64(e.score & 0xff);
            info.members.append(QJsonObject{
                {QStringLiteral("account_id"), owner},
                {QStringLiteral("name"), egsPlayerName(owner.toLongLong())},
                {QStringLiteral("flags"), flags},
                {QStringLiteral("updated"), double(e.updated)},
            });
            if (flags & 1 || info.attachment.isEmpty()) {
                info.attachment = e.attachment;
            }
        }
    }
    QJsonArray out;
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
        const QString board = QStringLiteral("challenges/%1/%2").arg(it->scope, it.key());
        const QList<Entry> times = db::boardScores(board);
        QJsonObject o{
            {QStringLiteral("id"), it.key()},
            {QStringLiteral("scope"), it->scope},
            {QStringLiteral("creator_account"), it->creator},
            {QStringLiteral("creator_name"),
             it->creator.isEmpty() ? QString() : egsPlayerName(it->creator.toLongLong())},
            {QStringLiteral("ends"), double(it->ends)}, // game clock, ms
            {QStringLiteral("members"), it->members},
            {QStringLiteral("attachment"), it->attachment},
            {QStringLiteral("board"), board},
            {QStringLiteral("entries"), qint64(times.size())},
            {QStringLiteral("updated"), double(it->updated)},
        };
        if (!times.isEmpty()) {
            o.insert(QStringLiteral("best"), QString::number(times.first().score));
            o.insert(QStringLiteral("best_entrant"), times.first().entrant);
            o.insert(QStringLiteral("best_name"),
                     egsPlayerName(times.first().entrant.toLongLong()));
        }
        out.append(o);
    }
    return out;
}

QJsonArray otgBoardList() {
    QJsonArray out;
    for (const db::BoardInfo& b : db::boards()) {
        out.append(QJsonObject{
            {QStringLiteral("name"), b.name},
            {QStringLiteral("kind"), otgBoardKind(b.name)},
            {QStringLiteral("entries"), qint64(b.entries)},
            {QStringLiteral("best"), QString::number(b.best)},
            {QStringLiteral("updated"), double(b.updated)},
        });
    }
    return out;
}

std::optional<QJsonArray> otgBoardEntries(const QString& name) {
    const QList<Entry> list = db::boardScores(name);
    if (list.isEmpty()) {
        return std::nullopt;
    }
    QJsonArray out;
    int rank = 0;
    for (const Entry& e : list) {
        out.append(QJsonObject{
            {QStringLiteral("rank"), ++rank},
            {QStringLiteral("entrant"), e.entrant},
            {QStringLiteral("score"), QString::number(e.score)},
            {QStringLiteral("attachment"), e.attachment},
            {QStringLiteral("updated"), double(e.updated)},
        });
    }
    return out;
}

bool otgRemoveEntry(const QString& board, const QString& entrant) {
    return db::removeScore(board, entrant);
}

bool otgSetEntry(const QString& board, const QString& entrant, quint64 score,
                 const QJsonArray& attachment) {
    if (board.isEmpty() || entrant.isEmpty()) {
        return false;
    }
    return db::putScore({board, entrant, score, attachment, QDateTime::currentMSecsSinceEpoch()});
}

QJsonArray otgGameStates() {
    QJsonArray out;
    for (const db::GameState& g : db::gameStates()) {
        out.append(QJsonObject{
            {QStringLiteral("resource"), g.resource},
            {QStringLiteral("version"), g.version},
            {QStringLiteral("counts"), g.counts},
            {QStringLiteral("updated"), double(g.updated)},
        });
    }
    return out;
}

bool otgRemoveGameState(const QString& resource) {
    return db::removeGameState(resource);
}

bool otgClearBoard(const QString& board) {
    return db::clearBoard(board);
}
