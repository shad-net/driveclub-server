// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "egs.h"

#include "auth.h"
#include "database.h"
// The game reads a field named "slots", which is a Qt macro.
#pragma push_macro("slots")
#undef slots
#include "egs.pb.h"
#pragma pop_macro("slots")
#include "otg.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QList>
#include <QLoggingCategory>
#include <QTimeZone>
#include <QTimer>

#include <chrono>
#include <QMessageAuthenticationCode>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QUuid>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace gp = google::protobuf;

namespace {

Q_LOGGING_CATEGORY(lcEgs, "egs")

EgsConfig& config() {
    static EgsConfig c;
    return c;
}

// Sessions

struct Session {
    QByteArray key;
    qint32 accountId = 0;
    QString name;
    QString shadnetAccount; // empty when the login was not verified with shadNet
    QString clientVersion;
    bool psPlus = false;
    qint64 created = 0;
    qint64 lastSeen = 0;
    int requests = 0;
    QString transaction;
    qint64 transactionCounter = 1000;
};

struct Sessions {
    QMutex mutex;
    QHash<QByteArray, Session> byKey;
};

Sessions& sessions() {
    static Sessions s;
    return s;
}

// Players (account id -> name)

void rememberPlayer(qint64 accountId, const QString& name, bool loggedIn,
                    const QString& shadnetAccount = {}) {
    db::Player p;
    p.accountId = accountId;
    p.name = name;
    p.loggedIn = loggedIn;
    p.shadnetAccount = shadnetAccount;
    db::putPlayer(p);
}

qint32 accountIdFor(const QString& name) {
    const QByteArray h = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha1);
    const quint32 v =
        (quint8(h[0]) << 24) | (quint8(h[1]) << 16) | (quint8(h[2]) << 8) | quint8(h[3]);
    return static_cast<qint32>(v & 0x7fffffff) | 1;
}

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// Endpoints

struct Context {
    const HttpRequest& request;
    QByteArray sessionKey;    // the session this request was signed with, empty before login
    QByteArray newSessionKey; // set by a handler: sent as X-SessionKey, becomes the signing key
    int status = 200;
    AuthResult auth; // login endpoints: the shadNet check of the authCode
};

struct Endpoint {
    QString service;
    QString method;
    const gp::Descriptor* request;
    const gp::Descriptor* response;
    bool secure;
    bool listed;
    std::function<std::string(Context&, const QByteArray&)> run;
};

template <class Req, class Resp>
Endpoint endpoint(const char* service, const char* method, bool secure, bool listed,
                  std::function<void(Context&, const Req&, Resp&)> handler) {
    Endpoint e{QString::fromLatin1(service),
               QString::fromLatin1(method),
               Req::descriptor(),
               Resp::descriptor(),
               secure,
               listed,
               {}};
    e.run = [handler, service, method](Context& ctx, const QByteArray& body) {
        Req req;
        if (!req.ParseFromArray(body.constData(), static_cast<int>(body.size()))) {
            qCWarning(lcEgs).noquote() << service << method << "request does not parse as"
                                       << qs(Req::descriptor()->name());
        }
        Resp resp;
        handler(ctx, req, resp);
        return resp.SerializeAsString();
    };
    return e;
}

std::string endpointList();

void login(Context& ctx, const OAuthLoginRequest& req, OAuthLoginResponse& resp) {
    const bool verified = ctx.auth.outcome == AuthResult::Outcome::Verified;
    QString name = verified ? ctx.auth.onlineId : qs(req.name());
    if (name.isEmpty()) {
        name = QStringLiteral("player");
    }
    Session s;
    s.key = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    s.name = name;
    s.accountId = accountIdFor(name);
    s.shadnetAccount = verified ? ctx.auth.accountId : QString();
    s.clientVersion = qs(req.clientversion());
    s.psPlus = req.psplus();
    s.created = QDateTime::currentMSecsSinceEpoch();
    s.lastSeen = s.created;
    {
        QMutexLocker lock(&sessions().mutex);
        // One session per player: drop the old key.
        for (auto it = sessions().byKey.begin(); it != sessions().byKey.end();) {
            if (it->name == name) {
                db::removeSession(it->key);
                it = sessions().byKey.erase(it);
            } else {
                ++it;
            }
        }
        sessions().byKey.insert(s.key, s);
    }
    qCInfo(lcEgs).noquote()
        << QStringLiteral("login %1 (%2, client %3, %4) -> accountId %5, session %6")
               .arg(name,
                    verified ? QStringLiteral("shadNet verified") : QStringLiteral("not verified"),
                    qs(req.clientversion()),
                    req.psplus() ? QStringLiteral("PS Plus") : QStringLiteral("no PS Plus"))
               .arg(s.accountId)
               .arg(QString::fromLatin1(s.key));

    rememberPlayer(s.accountId, name, true, s.shadnetAccount);
    db::putSession({s.key, s.accountId, s.name, s.shadnetAccount, s.clientVersion, s.psPlus,
                    s.created, s.lastSeen, s.requests});

    SessionKey* key = resp.mutable_sessionkey();
    key->set_id(s.key.toStdString());
    key->set_accountid(s.accountId);
    key->set_username(name.toStdString());
    ctx.newSessionKey = s.key;
}

void minimalProfiles(Context&, const GetMinimalProfilesRequest& req,
                     GetMinimalProfilesResponse& resp) {
    QStringList names;
    for (const qint64 id : req.accounts()) {
        MinimalProfile* p = resp.add_profiles();
        p->set_accountid(id);
        const QString name = egsPlayerName(id);
        p->set_name(name.toStdString());
        if (const auto f = db::fame(id)) {
            p->mutable_accountprogression()->set_xp(f->xp);
        }
        if (const auto m = db::clubMembership(id)) {
            p->set_clubid(m->clubId);
        }
        names << QStringLiteral("%1=%2").arg(id).arg(name);
    }
    qCInfo(lcEgs).noquote() << "profiles for" << req.accounts_size()
                            << "accounts:" << names.join(QStringLiteral(", "));
}

void aggregatedProfiles(Context&, const GetAggregatedProfilesRequest& req,
                        GetAggregatedProfilesResponse& resp) {
    QStringList found;
    for (const std::string& wanted : req.accountnames()) {
        const QString name = qs(wanted).trimmed();
        if (name.isEmpty()) {
            continue;
        }
        const auto knownPlayer = db::playerByName(name);
        qint64 id = knownPlayer ? knownPlayer->accountId : 0;
        const bool known = id != 0;
        if (!known) {
            id = accountIdFor(name);
        }
        AggregatedProfile* p = resp.add_profiles();
        AccountRef* a = p->mutable_account();
        a->set_id(id);
        a->set_name(name.toStdString());
        if (const auto m = db::clubMembership(id)) {
            p->mutable_club()->set_id(m->clubId);
        }
        if (const auto f = db::fame(id)) {
            p->mutable_progression()->set_xp(f->xp);
            p->set_stars(f->stars.value_or(0));
            p->set_elite(f->elite.value_or(0));
        }
        found << QStringLiteral("%1=%2%3").arg(name).arg(id).arg(known ? QString()
                                                                       : QStringLiteral(" (new)"));
    }
    qCInfo(lcEgs).noquote() << "aggregated profiles:" << found.join(QStringLiteral(", "));
}

void friends(Context& ctx, const GetFriendsRequest&, GetFriendsResponse& resp) {
    const QString me = egsSessionName(ctx.sessionKey);
    // Latest use of any session per player; 0 (never) may keep the game from listing a friend as
    // playing. shortcut: ms since epoch is a guess at the unit the game expects.
    QHash<QString, qint64> seen;
    for (const EgsSessionInfo& s : egsSessions()) {
        seen[s.name] = qMax(seen.value(s.name), s.lastSeen);
    }
    QStringList names;
    for (const EgsPlayerInfo& p : egsPlayers()) {
        if (p.name == me) {
            continue;
        }
        FriendAccount* a = resp.add_friends()->mutable_account();
        a->set_id(p.accountId);
        a->set_lastseen(seen.value(p.name));
        a->set_name(p.name.toStdString());
        names << p.name;
    }
    qCInfo(lcEgs).noquote() << "friends of" << (me.isEmpty() ? QStringLiteral("?") : me) << "->"
                            << names.size()
                            << (names.isEmpty() ? QString() : names.join(QStringLiteral(", ")));
}

// systemService.getTimeInfo, transactionService

void timeInfo(Context&, const Empty&, GetTimeInfoResponse& resp) {
    resp.set_timestamp(QDateTime::currentMSecsSinceEpoch());
}

Session* sessionOf(const Context& ctx) {
    auto it = sessions().byKey.find(ctx.sessionKey);
    return it == sessions().byKey.end() ? nullptr : &*it;
}

std::function<void(Context&, const TransactionRequest&, TransactionResponse&)> transactionCall(
    const char* method, bool startNew) {
    return
        [method, startNew](Context& ctx, const TransactionRequest& req, TransactionResponse& resp) {
            QMutexLocker lock(&sessions().mutex);
            Session* s = sessionOf(ctx);
            if (!s) {
                resp.set_currenttransactionid("0");
                return;
            }
            const QString before = s->transaction;
            if (startNew || s->transaction.isEmpty()) {
                s->transaction = QString::number(++s->transactionCounter);
            }
            resp.set_currenttransactionid(s->transaction.toStdString());
            qCInfo(lcEgs).noquote()
                << "transaction" << method << "for" << s->name
                << (req.has_currenttransactionid()
                        ? QStringLiteral("(completes %1)").arg(qs(req.currenttransactionid()))
                        : QString())
                << "->" << s->transaction
                << (before == s->transaction ? QString() : QStringLiteral("(new)"));
        };
}

// profileService: fame

qint64 sessionAccount(const Context& ctx, QString* name = nullptr) {
    QMutexLocker lock(&sessions().mutex);
    const Session* s = sessionOf(ctx);
    if (name && s) {
        *name = s->name;
    }
    return s ? s->accountId : 0;
}

// A club's fame is its members' fame summed (shortcut: the game's own club fame, earned while in the
// club, is not reported to the server).
qint64 clubFame(qint64 clubId) {
    qint64 xp = 0;
    for (const db::ClubMember& m : db::clubMembers(clubId)) {
        xp += db::fame(m.accountId).value_or(db::Fame{}).xp;
    }
    return xp;
}

void getFame(Context& ctx, const Empty&, GetFameResponse& resp) {
    QString name;
    const qint64 account = sessionAccount(ctx, &name);
    const std::optional<db::Fame> found = account ? db::fame(account) : std::nullopt;
    const db::Fame f = found.value_or(db::Fame{});
    FameAccountProgression* a = resp.mutable_accountprogression();
    a->set_xp(f.xp);
    a->set_serverxp(0);
    if (f.stars) {
        a->set_stars(*f.stars);
    }
    if (f.elite) {
        a->set_elite(*f.elite);
    }
    resp.set_stars(f.stars.value_or(0));
    resp.set_elite(f.elite.value_or(0));
    const auto membership = account ? db::clubMembership(account) : std::nullopt;
    resp.mutable_clubprogression()->set_xp(membership ? clubFame(membership->clubId) : 0);
    qCInfo(lcEgs).noquote() << "fame of" << (name.isEmpty() ? QStringLiteral("?") : name) << "->"
                            << (found ? QString::number(f.xp) : QStringLiteral("none stored (0)"));
}

void updateFame(Context& ctx, const UpdateFameRequest& req, UpdateFameResponse& resp) {
    QString name;
    const qint64 account = sessionAccount(ctx, &name);
    db::Fame f =
        (account ? db::fame(account) : std::nullopt).value_or(db::Fame{account, 0, {}, {}, 0});
    const qint64 sent = req.playertotalclientfame();
    if (account != 0) {
        f.xp = std::max(f.xp, sent);
        if (req.has_stars()) {
            f.stars = req.stars();
        }
        if (req.has_elite()) {
            f.elite = req.elite();
        }
        f.updated = QDateTime::currentMSecsSinceEpoch();
        db::putFame(f);
        const QString entrant = QString::number(account);
        const quint64 score = (quint64(2) << 60) | quint64(f.xp);
        const auto old = db::score(QStringLiteral("fame/player"), entrant);
        if (!old || old->score < score) {
            db::putScore({QStringLiteral("fame/player"), entrant, score, {}, f.updated});
        }
    }
    FameAfter* after = resp.mutable_accountprogression()->mutable_after();
    after->set_xp(f.xp);
    after->set_serverxp(0);
    resp.mutable_clubprogression()->mutable_after()->set_xp(0);
    qCInfo(lcEgs).noquote()
        << "fame update from" << (name.isEmpty() ? QStringLiteral("?") : name) << "total" << sent
        << "-> stored" << f.xp
        << (req.clubfamedelta() > 0
                ? QStringLiteral("(club delta %1 ignored: no clubs yet)").arg(req.clubfamedelta())
                : QString())
        << "transaction" << qs(req.transactionid());
}

// storageService: ghosts

QString bobChecksum(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toBase64());
}

void createBob(Context& ctx, const CreateBobRequest& req, CreateBobResponse& resp) {
    QString name;
    const qint64 account = sessionAccount(ctx, &name);
    const QByteArray data(req.body().data(), static_cast<qsizetype>(req.body().size()));
    const QString sent = qs(req.checksum());
    const QString actual = bobChecksum(data);
    if (!sent.isEmpty() && sent != actual) {
        // Stored anyway, with the checksum we computed: the game checks downloads against it.
        qCWarning(lcEgs).noquote()
            << "ghost from" << name << "has checksum" << sent << "but the data gives" << actual;
    }
    if (req.has_contentlength() && req.contentlength() != data.size()) {
        qCWarning(lcEgs).noquote() << "ghost from" << name << "says" << req.contentlength()
                                   << "bytes but has" << data.size();
    }
    db::Ghost g;
    g.accountId = account;
    g.mimeType = qs(req.mimetype());
    g.checksum = actual;
    const qint64 id = data.isEmpty() ? 0 : db::putGhost(g, data);
    if (id == 0) {
        ctx.status = 500; // the game marks the upload failed (0x41B6E0) and races on
        qCWarning(lcEgs).noquote()
            << "ghost from" << name << "not stored (" << data.size() << "bytes)";
        return;
    }
    const QString url = otgBaseUrl(ctx.request) + QStringLiteral("/ghosts/%1").arg(id);
    resp.set_id(id);
    BobFileDetails* f = resp.mutable_filedetails();
    f->set_id(id);
    f->set_publicurl(url.toStdString());
    f->set_checksum(actual.toStdString());
    qCInfo(lcEgs).noquote() << "ghost" << id << "uploaded by" << name << "," << data.size()
                            << "bytes, type" << req.objecttype() << "access" << req.accesstype();
}

void getRemoteUrl(Context& ctx, const GetRemoteUrlRequest& req, GetRemoteUrlResponse& resp) {
    const auto g = db::ghost(req.id());
    if (!g) {
        ctx.status = 500;
        qCWarning(lcEgs).noquote() << "getRemoteUrl for unknown ghost" << req.id();
        return;
    }
    const QString url = otgBaseUrl(ctx.request) + QStringLiteral("/ghosts/%1").arg(g->id);
    resp.set_id(g->id);
    resp.set_publicurl(url.toStdString());
    resp.set_checksum(g->checksum.toStdString());
    qCInfo(lcEgs).noquote() << "ghost" << g->id << "of" << egsPlayerName(g->accountId) << "->"
                            << url;
}

// Pushed messages: the game's long poll on /egs/messaging reads an XML stream of
// <msg t="Type" s="Size">base64 protobuf</msg>, decodes the content as the descriptor message named
// by t and hands it to the handler registered under that name (LobbyCallbackMessage, ...).
// The poll carries no session, so players are told apart by the runtime's X-Dccoop-Online-Id.
// The poll is a persistent stream (the game's EvoSliccPersistentAction): it must stay open, ending it
// counts as losing the server. Open streams get messages at once; others are queued until the next.
struct PushQueues {
    QMutex mutex;
    QHash<QString, QByteArrayList> byName; // lower-case online id -> pending <msg> elements
    QHash<QString, std::function<bool(const QByteArray&)>> streams; // lower-case online id -> writer
};

PushQueues& pushQueues() {
    static PushQueues q;
    return q;
}

void push(const QString& name, const gp::Message& message) {
    const QByteArray payload = QByteArray::fromStdString(message.SerializeAsString()).toBase64();
    const QByteArray element = QStringLiteral("<msg t=\"%1\" s=\"%2\">")
                                   .arg(qs(message.GetDescriptor()->name()))
                                   .arg(payload.size())
                                   .toUtf8() +
                               payload + "</msg>";
    qCInfo(lcEgs).noquote() << "push to" << name << qs(message.GetDescriptor()->name()) << "-"
                            << qs(message.ShortDebugString());
    QMutexLocker lock(&pushQueues().mutex);
    const QString key = name.toLower();
    const auto stream = pushQueues().streams.constFind(key);
    if (stream != pushQueues().streams.constEnd() && (*stream)(element)) {
        return;
    }
    pushQueues().streams.remove(key);
    pushQueues().byName[key].append(element);
}

// Starts the persistent messaging stream of one player: replaces an older one, flushes what was
// queued, and keeps the connection busy with a line break every 5 s.
void openStream(const QString& name, const std::function<bool(const QByteArray&)>& write) {
    const QString key = name.toLower();
    {
        QMutexLocker lock(&pushQueues().mutex);
        pushQueues().streams.insert(key, write);
        const QByteArray pending = pushQueues().byName.take(key).join();
        if (!pending.isEmpty()) {
            write(pending);
        }
    }
    qCInfo(lcEgs).noquote() << "messaging stream opened for" << name;
    auto* ping = new QTimer;
    QObject::connect(ping, &QTimer::timeout, [ping, key, write, name] {
        if (write("\n")) {
            return;
        }
        qCInfo(lcEgs).noquote() << "messaging stream closed for" << name;
        {
            QMutexLocker lock(&pushQueues().mutex);
            pushQueues().streams.remove(key);
        }
        ping->stop();
        ping->deleteLater();
    });
    ping->start(5000);
}

// Lobbies, in memory. A player is in at most one lobby of each type: the game creates a type 1
// lobby by itself after login and a type 0 one for a private lobby, and asks for each by type.
// shortcut: no persistence, no limits; action numbers for the pushes are guesses (env overrides).
struct LobbyMemberState {
    qint64 accountId = 0;
    QString username;
    bool host = false;
    QByteArray contentRestriction;
    QByteArray memberData;
};

struct LobbyState {
    QString id;
    int type = 0;
    int status = 0;
    QString name;
    QList<LobbyMemberState> members;
};

struct Lobbies {
    QMutex mutex;
    QList<LobbyState> list;
};

Lobbies& lobbies() {
    static Lobbies l;
    return l;
}

int envInt(const char* name, int fallback) {
    const QByteArray v = qgetenv(name);
    bool ok = false;
    const int n = v.toInt(&ok);
    return ok ? n : fallback;
}

LobbyMemberState memberFor(const QByteArray& sessionKey) {
    QMutexLocker lock(&sessions().mutex);
    const auto it = sessions().byKey.constFind(sessionKey);
    LobbyMemberState m;
    if (it != sessions().byKey.constEnd()) {
        m.accountId = it->accountId;
        m.username = it->name;
    }
    return m;
}

void fillLobby(const LobbyState& state, Lobby* out) {
    out->set_id(state.id.toStdString());
    out->set_lobbytype(static_cast<LobbyType>(state.type));
    out->set_status(static_cast<LobbyStatus>(state.status));
    out->set_name(state.name.toStdString());
    out->set_matchslotid(0);
    out->set_eventlistid(0);
    int playerNumber = 0;
    for (const LobbyMemberState& m : state.members) {
        LobbyMember* lm = out->add_memberslist();
        lm->mutable_sessionkey()->set_accountid(m.accountId);
        lm->mutable_sessionkey()->set_username(m.username.toStdString());
        lm->set_host(m.host);
        lm->set_contentrestriction(m.contentRestriction.toStdString());
        lm->mutable_sessionkey()->set_contentrestriction(m.contentRestriction.toStdString());
        if (!m.memberData.isEmpty()) {
            lm->set_memberdata(m.memberData.toStdString());
        }
        lm->set_team(0);
        lm->set_accountid(m.accountId);
        lm->set_playernumber(playerNumber++);
        lm->set_clientversion("CV_bM_tP_p128_x3");
        lm->set_aicontrolled(false);
    }
}

QString describe(const LobbyState& state) {
    QStringList names;
    for (const LobbyMemberState& m : state.members) {
        names << (m.host ? m.username + QStringLiteral("*") : m.username);
    }
    return QStringLiteral("%1 type %2 [%3]").arg(state.id).arg(state.type).arg(names.join(", "));
}

// Removes the account from the lobbies that match (by id, else by type, -1 = any; empty lobbies go,
// a host leaving hands over); returns them as they are afterwards. Call with lobbies().mutex held.
QList<LobbyState> leaveAll(qint64 accountId, int type = -1, const QString& id = {}) {
    QList<LobbyState> left;
    for (auto it = lobbies().list.begin(); it != lobbies().list.end();) {
        if ((!id.isEmpty() && it->id != id) || (id.isEmpty() && type >= 0 && it->type != type)) {
            ++it;
            continue;
        }
        const qsizetype before = it->members.size();
        bool wasHost = false;
        for (qsizetype i = it->members.size() - 1; i >= 0; --i) {
            if (it->members[i].accountId == accountId) {
                wasHost = it->members[i].host;
                it->members.removeAt(i);
            }
        }
        if (it->members.size() == before) {
            ++it;
            continue;
        }
        if (wasHost && !it->members.isEmpty()) {
            it->members.first().host = true;
        }
        left << *it;
        if (it->members.isEmpty()) {
            it = lobbies().list.erase(it);
        } else {
            ++it;
        }
    }
    return left;
}

// Tells the other members of a lobby what changed.
void pushLobby(const LobbyState& state, int action, qint64 except) {
    LobbyCallbackMessage msg;
    msg.set_action(static_cast<LobbyAction>(action));
    fillLobby(state, msg.mutable_lobby());
    for (const LobbyMemberState& m : state.members) {
        if (m.accountId != except) {
            push(m.username, msg);
        }
    }
}

void createLobby(Context& ctx, const LobbyRequest& req, LobbyList& resp) {
    LobbyMemberState me = memberFor(ctx.sessionKey);
    me.host = true;
    me.contentRestriction = QByteArray::fromStdString(req.member().contentrestriction());
    LobbyState state;
    state.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    state.type = req.lobby().lobbytype();
    state.name = qs(req.lobby().name());
    state.members << me;
    QList<LobbyState> left;
    {
        QMutexLocker lock(&lobbies().mutex);
        left = leaveAll(me.accountId, state.type);
        lobbies().list << state;
    }
    for (const LobbyState& l : left) {
        pushLobby(l, envInt("DCCOOP_ACTION_LEAVE", 6), me.accountId);
    }
    fillLobby(state, resp.add_lobbies());
    qCInfo(lcEgs).noquote() << "lobby created:" << describe(state);
}

void joinLobby(Context& ctx, const LobbyRequest& req, LobbyList& resp) {
    LobbyMemberState me = memberFor(ctx.sessionKey);
    me.contentRestriction = QByteArray::fromStdString(req.member().contentrestriction());
    const qint64 targetId = req.accounttojoin().id();
    const QString targetName = qs(req.accounttojoin().name());
    const QString lobbyId = qs(req.lobby().id());
    qCInfo(lcEgs).noquote() << "join request from" << me.username << "-" << qs(req.ShortDebugString());
    LobbyState joined;
    bool found = false;
    {
        QMutexLocker lock(&lobbies().mutex);
        for (LobbyState& l : lobbies().list) {
            if (req.lobby().has_lobbytype() && l.type != req.lobby().lobbytype()) {
                continue;
            }
            bool match = !lobbyId.isEmpty() && l.id == lobbyId;
            for (const LobbyMemberState& m : l.members) {
                if ((targetId && m.accountId == targetId) ||
                    (!targetName.isEmpty() && m.username.compare(targetName, Qt::CaseInsensitive) == 0)) {
                    match = true;
                }
            }
            if (!match) {
                continue;
            }
            bool already = false;
            for (const LobbyMemberState& m : l.members) {
                already = already || m.accountId == me.accountId;
            }
            if (!already) {
                // Leave my other lobby of this type first (the game made one for me at login).
                const QString target = l.id;
                const int type = l.type;
                for (LobbyState& other : lobbies().list) {
                    if (other.id != target && other.type == type) {
                        for (qsizetype i = other.members.size() - 1; i >= 0; --i) {
                            if (other.members[i].accountId == me.accountId) {
                                other.members.removeAt(i);
                            }
                        }
                    }
                }
                lobbies().list.removeIf([](const LobbyState& s) { return s.members.isEmpty(); });
                for (LobbyState& again : lobbies().list) {
                    if (again.id == target) {
                        again.members << me;
                        joined = again;
                    }
                }
            } else {
                joined = l;
            }
            found = true;
            break;
        }
    }
    if (!found) {
        qCWarning(lcEgs).noquote() << "join: no lobby for" << qs(req.ShortDebugString());
        return;
    }
    fillLobby(joined, resp.add_lobbies());
    qCInfo(lcEgs).noquote() << "lobby joined by" << me.username << ":" << describe(joined);
    pushLobby(joined, envInt("DCCOOP_ACTION_JOIN", 4), me.accountId);
}

void getLobby(Context& ctx, const LobbyRequest& req, LobbyList& resp) {
    const LobbyMemberState me = memberFor(ctx.sessionKey);
    QMutexLocker lock(&lobbies().mutex);
    for (const LobbyState& l : lobbies().list) {
        if (req.lobby().has_lobbytype() && l.type != req.lobby().lobbytype()) {
            continue;
        }
        for (const LobbyMemberState& m : l.members) {
            if (m.accountId == me.accountId) {
                fillLobby(l, resp.add_lobbies());
                return;
            }
        }
    }
}

void leaveLobby(Context& ctx, const LobbyRequest& req, LobbyList& resp) {
    const LobbyMemberState me = memberFor(ctx.sessionKey);
    qCInfo(lcEgs).noquote() << "leave request from" << me.username << "-" << qs(req.ShortDebugString());
    QList<LobbyState> left;
    {
        QMutexLocker lock(&lobbies().mutex);
        left = leaveAll(me.accountId, req.lobby().has_lobbytype() ? req.lobby().lobbytype() : -1,
                        qs(req.lobbyid()));
    }
    for (const LobbyState& l : left) {
        LobbyResult* r = resp.add_results();
        r->set_lobbyid(l.id.toStdString());
        qCInfo(lcEgs).noquote() << "lobby left by" << me.username << ":" << describe(l);
        pushLobby(l, envInt("DCCOOP_ACTION_LEAVE", 6), me.accountId);
    }
}

// joinLobby: the private lobby game joins the room of a lobby it already knows (by id, after the
// invitation push). Adds the caller, answers with the lobby and tells the other members.
void joinLobbyById(Context& ctx, const LobbyRequest& req, JoinLobbyResponse& resp) {
    LobbyMemberState me = memberFor(ctx.sessionKey);
    const QString id = qs(req.lobby().id());
    qCInfo(lcEgs).noquote() << "joinLobby from" << me.username << "-" << qs(req.ShortDebugString());
    LobbyState joined;
    bool found = false;
    {
        QMutexLocker lock(&lobbies().mutex);
        for (LobbyState& l : lobbies().list) {
            bool mine = false;
            for (const LobbyMemberState& m : l.members) {
                mine = mine || m.accountId == me.accountId;
            }
            if ((!id.isEmpty() && l.id == id) || (id.isEmpty() && mine)) {
                if (!mine) {
                    l.members << me;
                }
                joined = l;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        qCWarning(lcEgs).noquote() << "joinLobby: no lobby" << id;
        ctx.status = 404;
        return;
    }
    fillLobby(joined, resp.mutable_lobby());
    fillLobby(joined, resp.add_lobbies());
    qCInfo(lcEgs).noquote() << "joinLobby ok:" << describe(joined);
    pushLobby(joined, envInt("DCCOOP_ACTION_JOIN", 4), me.accountId);
}

// updateMember: stores what the member reports (its content restriction, the cars and tracks it
// owns, which decides whether it is eligible for an event) and tells the others when it changed.
// Pushing only on a change matters: every member answers a push with another updateMember.
void updateMember(Context& ctx, const LobbyRequest& req, LobbyList& resp) {
    const LobbyMemberState me = memberFor(ctx.sessionKey);
    const QString id = qs(req.lobby().id());
    const qint64 who = req.member().sessionkey().accountid() ? req.member().sessionkey().accountid()
                                                             : me.accountId;
    LobbyState changed;
    bool anyChange = false;
    {
        QMutexLocker lock(&lobbies().mutex);
        for (LobbyState& l : lobbies().list) {
            if (!id.isEmpty() && l.id != id) {
                continue;
            }
            for (LobbyMemberState& m : l.members) {
                if (m.accountId != who) {
                    continue;
                }
                if (req.member().has_contentrestriction()) {
                    const QByteArray cr = QByteArray::fromStdString(req.member().contentrestriction());
                    anyChange = anyChange || cr != m.contentRestriction;
                    m.contentRestriction = cr;
                }
                if (req.member().has_memberdata()) {
                    const QByteArray md = QByteArray::fromStdString(req.member().memberdata());
                    anyChange = anyChange || md != m.memberData;
                    m.memberData = md;
                }
                changed = l;
            }
            if (!changed.id.isEmpty()) {
                break;
            }
        }
    }
    if (changed.id.isEmpty()) {
        qCWarning(lcEgs).noquote() << "updateMember: no lobby" << id << "for" << who;
        getLobby(ctx, req, resp);
        return;
    }
    fillLobby(changed, resp.add_lobbies());
    qCInfo(lcEgs).noquote() << "member" << who << "updated in" << describe(changed)
                            << (anyChange ? "(changed, pushing)" : "(unchanged)");
    if (anyChange) {
        pushLobby(changed, envInt("DCCOOP_ACTION_UPDATE", 4), who);
    }
}

// updateLobby, changeHost, progressToNextRace: answer with the lobby as it is.
// shortcut: their changes are not applied yet; the log shows what the game sends.
void echoLobby(Context& ctx, const LobbyRequest& req, LobbyList& resp) {
    qCInfo(lcEgs).noquote() << "lobby request (not applied):" << qs(req.ShortDebugString());
    getLobby(ctx, req, resp);
}

// Known players whose name starts with the typed text.
void quickSearch(Context&, const QuickSearchRequest& req, QuickSearchResponse& resp) {
    const QString prefix = qs(req.startswith()).trimmed();
    const int max = req.maximumentries() > 0 ? req.maximumentries() : 20;
    for (const EgsPlayerInfo& p : egsPlayers()) {
        if (resp.accounts_size() >= max) {
            break;
        }
        if (p.name.startsWith(prefix, Qt::CaseInsensitive)) {
            resp.add_accounts()->set_id(p.accountId);
        }
    }
    qCInfo(lcEgs).noquote() << "quick search" << prefix << "->" << resp.accounts_size();
}

// statsService.getAccountStats: one set (Tracks, Vehicles, GameModes, Challenges, Overdrive, FaceOff)
// for one account or club. The game maps each name in columnNames onto one of its stat columns and
// fails the request (the profile pages keep loading) unless they are there. The values come from the
// counters the game itself uploads after each event (OTG gamestate stats/V06/player/<id>, keys
// "<subject>/<category>/<counter>"); a club's are its members' summed.
//
// Tracks, Vehicles, GameModes and Challenges are rows named by their subject (track or car id, mode,
// solo challenge) with one entry: attempts, distance, fame, wins, created, forwarded. Overdrive and
// FaceOff are one row whose entry lists fixed subjects in the game's own order (each column picks its
// subject by position).
namespace {
QJsonObject statCounts(qint64 accountId) {
    const auto g = db::gameState(QStringLiteral("stats/V06/player/%1").arg(accountId));
    return g ? g->counts : QJsonObject{};
}

QJsonObject clubStatCounts(qint64 clubId) {
    QJsonObject sum;
    for (const db::ClubMember& m : db::clubMembers(clubId)) {
        const QJsonObject c = statCounts(m.accountId);
        for (auto it = c.begin(); it != c.end(); ++it) {
            sum[it.key()] = sum.value(it.key()).toInteger() + it.value().toInteger();
        }
    }
    return sum;
}

// A counter; a penalty's fame ("<subject>/overdrive/fame/-") counts as negative.
qint64 statCounter(const QJsonObject& counts, const QString& subject, const QString& category,
                   const QString& counter) {
    const QString key = subject + '/' + category + '/' + counter;
    if (counts.contains(key)) {
        return counts.value(key).toInteger();
    }
    return -counts.value(key + QStringLiteral("/-")).toInteger();
}
} // namespace

void accountStats(Context& ctx, const GetAccountStatsRequest& req, GetAccountStatsResponse& resp) {
    const QString set = qs(req.columndefinitionsetname());
    QJsonObject counts;
    QString owner;
    if (req.clubs_size() > 0 || req.currentclub()) {
        const qint64 club = req.clubs_size() > 0 ? req.clubs(0).id()
                                                 : db::clubMembership(sessionAccount(ctx)).value_or(db::ClubMember{}).clubId;
        counts = clubStatCounts(club);
        owner = QStringLiteral("club %1").arg(club);
    } else {
        const qint64 account = req.accounts_size() > 0 ? req.accounts(0).id() : sessionAccount(ctx);
        counts = statCounts(account);
        owner = QStringLiteral("account %1").arg(account);
    }

    struct Fixed { const char* column; const char* subject; const char* counter; };
    static const Fixed overdrive[] = {
        {"PositionGainedTotalFame", "UniquePosition", "fame"}, {"PositionGainedAttempts", "UniquePosition", "attempts"},
        {"SectorAccuracyTotalFame", "Sector Accuracy", "fame"}, {"SectorAccuracyAttempts", "Sector Accuracy", "attempts"},
        {"CleanLapTotalFame", "CleanLap", "fame"}, {"CleanLapAttempts", "CleanLap", "attempts"},
        {"CleanRaceTotalFame", "CleanRace", "fame"}, {"CleanRaceAttempts", "CleanRace", "attempts"},
        {"CleanSectorTotalFame", "CleanSector", "fame"}, {"CleanSectorAttempts", "CleanSector", "attempts"},
        {"CrazySpeedTotalFame", "Crazy Speed", "fame"}, {"CrazySpeedAttempts", "Crazy Speed", "attempts"},
        {"DraftTotalFame", "Drafting", "fame"}, {"DraftAttempts", "Drafting", "attempts"},
        {"DriftOverdriveTotalFame", "Drift", "fame"}, {"DriftOverdriveAttempts", "Drift", "attempts"},
        {"VehicleCollisionTotalFame", "VehicleCollision", "fame"}, {"VehicleCollisionAttempts", "VehicleCollision", "attempts"},
        {"EnvironmentCollisionTotalFame", "EnvironmentCollision", "fame"},
        {"EnvironmentCollisionAttempts", "EnvironmentCollision", "attempts"},
        {"FinishPositionTotalFame", "FinishPosition", "fame"}, {"FinishPositionAttempts", "FinishPosition", "attempts"},
        {"OffTrackTotalFame", "OffTrack", "fame"}, {"OffTrackAttempts", "OffTrack", "attempts"}};
    static const Fixed faceoff[] = {
        {"AverageSpeedTotalFame", "Average Speed Face-Off", "fame"}, {"AverageSpeedAttempts", "Average Speed Face-Off", "attempts"},
        {"AverageSpeedWins", "Average Speed Face-Off", "wins"},
        {"CorneringTotalFame", "Cornering Face-Off", "fame"}, {"CorneringAttempts", "Cornering Face-Off", "attempts"},
        {"CorneringWins", "Cornering Face-Off", "wins"},
        {"DriftTotalFame", "Drift Face-Off", "fame"}, {"DriftAttempts", "Drift Face-Off", "attempts"},
        {"DriftWins", "Drift Face-Off", "wins"}};

    if (set == "Overdrive" || set == "FaceOff") {
        const bool od = set == "Overdrive";
        const Fixed* first = od ? std::begin(overdrive) : std::begin(faceoff);
        const Fixed* last = od ? std::end(overdrive) : std::end(faceoff);
        AccountStat* row = resp.add_stats();
        row->set_total(1);
        row->mutable_definition()->set_name(set.toStdString());
        StatEntry* entry = row->add_entries();
        for (const Fixed* f = first; f != last; ++f) {
            resp.add_columnnames(f->column);
            entry->add_columns(statCounter(counts, f->subject, od ? "overdrive" : "faceoff", f->counter));
        }
    } else {
        static const char* const columns[] = {"Attempts", "TotalDistanceDriven", "TotalFame", "Wins", "Created", "Forwarded"};
        static const char* const counters[] = {"attempts", "distance", "fame", "wins", "created", "forwarded"};
        const QString category = set == "Tracks" ? "track" : set == "Vehicles" ? "vehicle"
                               : set == "GameModes" ? "event" : "solochallenge";
        for (const char* c : columns) {
            resp.add_columnnames(c);
        }
        const QString suffix = '/' + category + '/';
        QStringList subjects;
        for (auto it = counts.begin(); it != counts.end(); ++it) {
            const qsizetype at = it.key().indexOf(suffix);
            if (at > 0 && !subjects.contains(it.key().left(at))) {
                subjects << it.key().left(at);
            }
        }
        for (const QString& subject : subjects) {
            AccountStat* row = resp.add_stats();
            row->set_total(1);
            row->mutable_definition()->set_name(subject.toStdString());
            StatEntry* entry = row->add_entries();
            for (const char* counter : counters) {
                entry->add_columns(statCounter(counts, subject, category, counter));
            }
        }
    }
    qCInfo(lcEgs).noquote() << "stats" << set << "for" << owner << "(" << egsSessionName(ctx.sessionKey)
                            << ") ->" << resp.stats_size() << "rows";
}

// clubService. Clubs live in the database; a player is in at most one club. The creator is the
// owner (member type 2), others join as members (type 1). shortcut: join requests are granted at
// once, invitations are stored but not pushed, club fame (xp) is not counted yet.

constexpr int ClubOwner = 2;
constexpr int ClubMemberType = 1;
constexpr int ClubActive = 1;
constexpr qsizetype ClubMaxMembers = 6;

QString rfc3339(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC)
        .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
}

bool accountOnline(qint64 accountId) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const EgsSessionInfo& s : egsSessions()) {
        if (s.accountId == accountId && now - s.lastSeen < 2 * 60 * 1000) {
            return true;
        }
    }
    return false;
}

void fillCustomization(const db::Club& c, ClubCustomization* out) {
    const QJsonObject& j = c.customization;
    out->set_id(c.id);
    if (j.contains(QStringLiteral("liveryDescriptor"))) {
        out->set_liverydescriptor(j.value(QStringLiteral("liveryDescriptor")).toString().toStdString());
    }
    for (const QJsonValue& v : j.value(QStringLiteral("layers")).toArray()) {
        const QJsonObject l = v.toObject();
        ClubLayer* layer = out->add_layers();
        layer->set_scale(float(l.value(QStringLiteral("scale")).toDouble(1.0)));
        layer->set_image(l.value(QStringLiteral("image")).toString().toStdString());
        for (const QJsonValue& color : l.value(QStringLiteral("colors")).toArray()) {
            layer->add_colors(color.toString().toStdString());
        }
    }
    for (const QJsonValue& t : j.value(QStringLiteral("tags")).toArray()) {
        out->add_tags(t.toString().toStdString());
    }
}

void fillClub(const db::Club& c, ::Club* out) {
    const QList<db::ClubMember> members = db::clubMembers(c.id);
    out->set_id(c.id);
    out->set_name(c.name.toStdString());
    out->set_historicalname(c.name.toStdString());
    out->set_membership(static_cast<GenericEnum>(c.membership));
    out->set_clubstatus(static_cast<GenericEnum>(ClubActive));
    out->set_size(int(members.size()));
    out->set_createdon(rfc3339(c.created).toStdString());
    for (const db::ClubMember& m : members) {
        ClubMember* cm = out->add_clubmembers();
        cm->set_account(m.accountId);
        cm->set_online(accountOnline(m.accountId));
        cm->set_type(static_cast<GenericEnum>(m.type));
        cm->set_createdon(rfc3339(m.joined).toStdString());
        cm->set_mvp(false);
    }
    fillCustomization(c, out->mutable_customization());
    out->mutable_progression()->set_xp(clubFame(c.id));
}

// Takes a player out of their club; an owner hands the club to the longest member, the last one
// out closes it.
void leaveClub(qint64 accountId) {
    const auto membership = db::clubMembership(accountId);
    if (!membership) {
        return;
    }
    db::removeClubMember(accountId);
    const QList<db::ClubMember> rest = db::clubMembers(membership->clubId);
    if (rest.isEmpty()) {
        db::removeClub(membership->clubId);
        qCInfo(lcEgs).noquote() << "club" << membership->clubId << "closed (last member left)";
    } else if (membership->type == ClubOwner) {
        db::ClubMember next = rest.first();
        next.type = ClubOwner;
        db::putClubMember(next);
    }
}

bool joinClub(qint64 accountId, qint64 clubId, int type) {
    const auto c = db::club(clubId);
    if (!c || db::clubMembers(clubId).size() >= ClubMaxMembers) {
        return false;
    }
    if (const auto current = db::clubMembership(accountId); current && current->clubId == clubId) {
        return true;
    }
    leaveClub(accountId);
    return db::putClubMember({accountId, clubId, type, 0});
}

void createClub(Context& ctx, const ClubRequest& req, ClubReply& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const QString name = qs(req.name()).trimmed();
    if (account == 0 || name.isEmpty()) {
        ctx.status = 400;
        return;
    }
    db::Club c;
    c.name = name;
    c.membership = req.membership();
    c.created = QDateTime::currentMSecsSinceEpoch();
    const qint64 id = db::createClub(c);
    if (id == 0) {
        ctx.status = 500;
        return;
    }
    leaveClub(account);
    db::putClubMember({account, id, ClubOwner, c.created});
    resp.set_id(id);
    qCInfo(lcEgs).noquote() << "club" << id << name << "created by" << me;
}

void sanitizeClubName(Context&, const ClubNameMessage& req, ClubNameMessage& resp) {
    resp.set_name(qs(req.name()).simplified().left(32).toStdString());
}

void queryClubs(Context&, const QueryClubsRequest& req, QueryClubsResponse& resp) {
    QList<qint64> ids;
    for (const qint64 id : req.clubids()) {
        ids << id;
    }
    for (const qint64 account : req.accountids()) {
        if (const auto m = db::clubMembership(account)) {
            ids << m->clubId;
        }
    }
    const QList<db::Club> all = db::clubs();
    for (const std::string& n : req.clubnames()) {
        for (const db::Club& c : all) {
            if (c.name.compare(qs(n), Qt::CaseInsensitive) == 0) {
                ids << c.id;
            }
        }
    }
    const bool browse = req.clubids_size() == 0 && req.accountids_size() == 0 &&
                        req.clubnames_size() == 0;
    const int limit = req.range().numberofresults() > 0 ? req.range().numberofresults() : 50;
    int added = 0;
    for (const db::Club& c : all) {
        if (added >= limit || (!browse && !ids.contains(c.id))) {
            continue;
        }
        fillClub(c, resp.add_clubs());
        ++added;
    }
    qCInfo(lcEgs).noquote() << "clubs query" << qs(req.ShortDebugString()) << "->" << added;
}

void quickSearchClubs(Context&, const ClubQuickSearchRequest& req, ClubQuickSearchResponse& resp) {
    const QString prefix = qs(req.startswith());
    const int limit = req.maximumentries() > 0 ? req.maximumentries() : 20;
    for (const db::Club& c : db::clubs()) {
        if (resp.clubs_size() < limit && c.name.startsWith(prefix, Qt::CaseInsensitive)) {
            resp.add_clubs()->set_id(c.id);
        }
    }
}

void joinClubCall(Context& ctx, const ClubRequest& req, ClubReply& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    if (account == 0 || !joinClub(account, req.id(), ClubMemberType)) {
        ctx.status = 400;
        return;
    }
    resp.set_id(req.id());
    qCInfo(lcEgs).noquote() << me << "joined club" << req.id();
}

void leaveClubCall(Context& ctx, const ClubRequest&, ClubReply& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const auto m = db::clubMembership(account);
    leaveClub(account);
    if (m) {
        resp.set_id(m->clubId);
        qCInfo(lcEgs).noquote() << me << "left club" << m->clubId;
    }
}

void inviteToClub(Context& ctx, const ClubAccountsRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    for (const IdOnly& a : req.accounts()) {
        db::ClubInvite i;
        i.clubId = req.club().id();
        i.accountId = a.id();
        i.invitedBy = account;
        db::putClubInvite(i);
        qCInfo(lcEgs).noquote() << me << "invited" << egsPlayerName(a.id()) << "to club"
                                << req.club().id();
    }
}

void findClubInvites(Context& ctx, const FindClubInvitesRequest& req, FindClubInvitesResponse& resp) {
    QList<qint64> accounts(req.accountids().begin(), req.accountids().end());
    QList<qint64> clubs(req.clubids().begin(), req.clubids().end());
    if (accounts.isEmpty() && clubs.isEmpty()) {
        accounts << sessionAccount(ctx);
    }
    for (const db::ClubInvite& i : db::clubInvites()) {
        if (i.status != 0 || !(accounts.contains(i.accountId) || clubs.contains(i.clubId)) ||
            !db::club(i.clubId)) {
            continue;
        }
        ClubInvite* out = resp.add_clubinvites();
        out->set_status(static_cast<GenericEnum>(i.status));
        out->set_id(i.id);
        out->set_club(i.clubId);
        out->set_account(i.accountId);
        out->set_invitedby(i.invitedBy);
        out->set_createdon(rfc3339(i.created).toStdString());
        out->set_updatedon(rfc3339(i.updated).toStdString());
        out->set_expireson(rfc3339(i.created + qint64(7) * 24 * 3600 * 1000).toStdString());
    }
}

void rejectClubInvite(Context& ctx, const ClubRequest& req, Empty&) {
    const qint64 account = sessionAccount(ctx);
    for (db::ClubInvite i : db::clubInvites()) {
        if (i.status == 0 && (i.id == req.id() || (i.clubId == req.id() && i.accountId == account))) {
            i.status = 2;
            db::putClubInvite(i);
        }
    }
}

void updateClubMembership(Context& ctx, const ClubRequest& req, ClubReply& resp) {
    const auto m = db::clubMembership(sessionAccount(ctx));
    const qint64 clubId = req.id() ? req.id() : (m ? m->clubId : 0);
    auto c = db::club(clubId);
    if (!c) {
        ctx.status = 400;
        return;
    }
    c->membership = req.membership();
    db::putClub(*c);
    resp.set_id(c->id);
    resp.set_membership(req.membership());
}

void updateClubCustomization(Context& ctx, const UpdateCustomizationRequest& req,
                             UpdateCustomizationResponse& resp) {
    const auto m = db::clubMembership(sessionAccount(ctx));
    const qint64 clubId = req.customization().id() ? req.customization().id()
                                                   : (m ? m->clubId : 0);
    auto c = db::club(clubId);
    if (!c) {
        ctx.status = 400;
        return;
    }
    const ClubCustomization& in = req.customization();
    QJsonObject& j = c->customization;
    if (in.has_liverydescriptor()) {
        j.insert(QStringLiteral("liveryDescriptor"), qs(in.liverydescriptor()));
    }
    if (in.layers_size() > 0) {
        QJsonArray layers;
        for (const ClubLayer& l : in.layers()) {
            QJsonArray colors;
            for (const std::string& color : l.colors()) {
                colors.append(qs(color));
            }
            layers.append(QJsonObject{{QStringLiteral("scale"), double(l.scale())},
                                      {QStringLiteral("image"), qs(l.image())},
                                      {QStringLiteral("colors"), colors}});
        }
        j.insert(QStringLiteral("layers"), layers);
    }
    if (in.tags_size() > 0) {
        QJsonArray tags;
        for (const std::string& t : in.tags()) {
            tags.append(qs(t));
        }
        j.insert(QStringLiteral("tags"), tags);
    }
    db::putClub(*c);
    fillCustomization(*c, resp.add_customizations());
}

// sessionService.queryStatus: the asked accounts that have a session in use are online.
void queryStatus(Context&, const QueryStatusRequest& req, QueryStatusResponse& resp) {
    constexpr qint64 OnlineWindowMs = 2 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QHash<qint64, QString> online;
    for (const EgsSessionInfo& s : egsSessions()) {
        if (now - s.lastSeen < OnlineWindowMs) {
            online.insert(s.accountId, s.name);
        }
    }
    for (const IdOnly& a : req.accounts()) {
        if (!online.contains(a.id())) {
            continue;
        }
        LobbySessionKey* k = resp.add_sessions()->mutable_sessionkey();
        k->set_accountid(a.id());
        k->set_username(online.value(a.id()).toStdString());
        resp.add_accounts()->set_id(a.id());
    }
}

const QList<Endpoint>& endpoints() {
    static const QList<Endpoint> list = {
        // Registered by the game itself before it has a schema (0xA29400), so not listed.
        endpoint<Empty, gp::FileDescriptorProto>(
            "indexService", "getMessageDescriptors", false, false,
            [](Context&, const Empty&, gp::FileDescriptorProto& out) {
                out.ParseFromString(egsDescriptor().toStdString());
            }),
        endpoint<Empty, SvcEndpointDescriptorList>(
            "indexService", "getSvcEndpointList", false, false,
            [](Context&, const Empty&, SvcEndpointDescriptorList& out) {
                out.ParseFromString(endpointList());
            }),
        endpoint<Empty, GetVersionResponse>("systemService", "getVersion", false, true,
                                            [](Context&, const Empty&, GetVersionResponse& out) {
                                                out.set_version(config().version.toStdString());
                                                out.set_serveridentifier(
                                                    config().serverIdentifier.toStdString());
                                            }),
        endpoint<OAuthLoginRequest, OAuthLoginResponse>("sessionService", "oauthLogin", false, true,
                                                        login),
        endpoint<OAuthLoginRequest, OAuthLoginResponse>("sessionService", "oauthRefresh", false,
                                                        true, login),
        endpoint<GetMinimalProfilesRequest, GetMinimalProfilesResponse>(
            "profileService", "getMinimalProfiles", false, true, minimalProfiles),
        endpoint<GetAggregatedProfilesRequest, GetAggregatedProfilesResponse>(
            "profileService", "getAggregatedProfiles", false, true, aggregatedProfiles),
        endpoint<GetFriendsRequest, GetFriendsResponse>("friendsListService", "getFriends", false,
                                                        true, friends),
        endpoint<Empty, GetTimeInfoResponse>("systemService", "getTimeInfo", false, true, timeInfo),
        endpoint<TransactionRequest, TransactionResponse>("transactionService", "newTransaction",
                                                          false, true,
                                                          transactionCall("newTransaction", true)),
        endpoint<TransactionRequest, TransactionResponse>(
            "transactionService", "getCurrentTransaction", false, true,
            transactionCall("getCurrentTransaction", false)),
        endpoint<TransactionRequest, TransactionResponse>(
            "transactionService", "completeTransaction", false, true,
            transactionCall("completeTransaction", false)),
        endpoint<TransactionRequest, TransactionResponse>(
            "transactionService", "completeAndGetNewTransaction", false, true,
            transactionCall("completeAndGetNewTransaction", true)),
        endpoint<TransactionRequest, TransactionResponse>(
            "transactionService", "cancelCurrentTransaction", false, true,
            transactionCall("cancelCurrentTransaction", true)),
        endpoint<Empty, GetFameResponse>("profileService", "getFame", false, true, getFame),
        endpoint<UpdateFameRequest, UpdateFameResponse>("profileService", "updateFameInTransaction",
                                                        false, true, updateFame),
        endpoint<CreateBobRequest, CreateBobResponse>("storageService", "createBob", false, true,
                                                      createBob),
        endpoint<GetRemoteUrlRequest, GetRemoteUrlResponse>("storageService", "getRemoteUrl", false,
                                                            true, getRemoteUrl),
        endpoint<Empty, GlobalGameConfiguration>(
            "configurationService", "getGlobalGameConfiguration", false, true,
            [](Context&, const Empty&, GlobalGameConfiguration& out) {
                UiConfig* ui = out.mutable_configuration()->mutable_uiconfig();
                ui->set_enablebuttonchallenge(true);
                ui->set_enablebuttonchallengecreate(true);
                ui->set_enablebuttondrive(true);
                ui->set_enablebuttonmultiplayer(true);
                ui->set_enablebuttonsingleevent(true);
                ui->set_enablebuttontour(true);
                ui->set_enablebuttonfacebook(false);
                ui->set_betaisactive(false);
            }),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "createLobby", false, true, createLobby),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "joinWithAccount", false, true,
                                          joinLobby),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "getLobby", false, true, getLobby),
        endpoint<LobbyRequest, JoinLobbyResponse>("lobbyService", "joinLobby", false, true,
                                                  joinLobbyById),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "leaveLobbyById", false, true,
                                          leaveLobby),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "leaveCurrentLobby", false, true,
                                          leaveLobby),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "removeFromLobby", false, true,
                                          leaveLobby),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "changeHost", false, true, echoLobby),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "progressToNextRace", false, true,
                                          echoLobby),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "updateMember", false, true,
                                          updateMember),
        endpoint<LobbyRequest, LobbyList>("lobbyService", "updateLobby", false, true, echoLobby),
        // Services the game retries without end while they are missing from the list. The game
        // reads every field its parser names, so the messages must have them all; they are answered
        // with defaults. shortcut: real stats, search, feed, notifications and accolades.
        // matchingService: logged so its use can be followed. shortcut: one slot per event id, no
        // real matchmaking; players counted from everyone registered for the same id.
        endpoint<MatchingRequest, RegisterInterestResponse>(
            "matchingService", "registerInterest", false, true,
            [](Context& ctx, const MatchingRequest& req, RegisterInterestResponse& out) {
                qCInfo(lcEgs).noquote() << "matching: registerInterest from"
                                        << egsSessionName(ctx.sessionKey) << "-"
                                        << qs(req.ShortDebugString());
                out.set_registered(true);
                out.mutable_slot()->set_id(req.id() ? req.id() : 1);
            }),
        endpoint<MatchingRequest, RegisterInterestResponse>(
            "matchingService", "unregisterInterest", false, true,
            [](Context& ctx, const MatchingRequest& req, RegisterInterestResponse& out) {
                qCInfo(lcEgs).noquote() << "matching: unregisterInterest from"
                                        << egsSessionName(ctx.sessionKey) << "-"
                                        << qs(req.ShortDebugString());
                out.set_registered(false);
            }),
        endpoint<MatchingRequest, MarkReadyResponse>(
            "matchingService", "markReady", false, true,
            [](Context& ctx, const MatchingRequest& req, MarkReadyResponse& out) {
                qCInfo(lcEgs).noquote() << "matching: markReady from"
                                        << egsSessionName(ctx.sessionKey) << "-"
                                        << qs(req.ShortDebugString());
                out.set_id(req.matchslotid());
                out.set_ready(true);
            }),
        endpoint<MatchingRequest, MarkReadyResponse>(
            "matchingService", "markNotReady", false, true,
            [](Context&, const MatchingRequest& req, MarkReadyResponse& out) {
                out.set_id(req.matchslotid());
                out.set_ready(false);
            }),
        endpoint<MatchingRequest, QueryMatchSlotsResponse>(
            "matchingService", "queryMatchSlotsById", false, true,
            [](Context&, const MatchingRequest& req, QueryMatchSlotsResponse& out) {
                for (const qint64 id : req.slotids()) {
                    MatchSlotInfo* s = out.add_slots();
                    s->set_slotid(id);
                    s->set_players(0);
                    s->set_friendsandclubmates(0);
                }
            }),
        endpoint<MatchingRequest, QueryEventListActivityResponse>(
            "matchingService", "queryEventListActivity", false, true,
            [](Context&, const MatchingRequest& req, QueryEventListActivityResponse& out) {
                for (const qint64 id : req.eventlists()) {
                    EventListActivity* a = out.add_activity();
                    a->set_eventlist(id);
                    a->set_players(0);
                }
            }),
        endpoint<QuickSearchRequest, QuickSearchResponse>("accountService", "quickSearch", false,
                                                          true, quickSearch),
        endpoint<GetActivityFeedRequest, GetActivityFeedResponse>(
            "activityService", "getActivityFeed", false, true,
            [](Context&, const GetActivityFeedRequest&, GetActivityFeedResponse&) {}),
        // Nothing to report yet: answered so the social hub and menu stop waiting.
        endpoint<GetNotificationsRequest, GetNotificationsResponse>(
            "activityService", "getNotifications", false, true,
            [](Context&, const GetNotificationsRequest&, GetNotificationsResponse&) {}),
        endpoint<GetPlayerAccoladeProgressRequest, GetPlayerAccoladeProgressResponse>(
            "objectiveService", "getPlayerAccoladeProgress", false, true,
            [](Context&, const GetPlayerAccoladeProgressRequest&,
               GetPlayerAccoladeProgressResponse&) {}),
        endpoint<GetAccountStatsRequest, GetAccountStatsResponse>(
            "statsService", "getAccountStats", false, true, accountStats),
        endpoint<QueryStatusRequest, QueryStatusResponse>("sessionService", "queryStatus", false,
                                                          true, queryStatus),
        endpoint<ClubRequest, ClubReply>("clubService", "createAndJoin", false, true, createClub),
        endpoint<ClubNameMessage, ClubNameMessage>("clubService", "sanitize", false, true,
                                                   sanitizeClubName),
        endpoint<QueryClubsRequest, QueryClubsResponse>("clubService", "queryClubs", false, true,
                                                        queryClubs),
        endpoint<ClubQuickSearchRequest, ClubQuickSearchResponse>("clubService", "quickSearch",
                                                                  false, true, quickSearchClubs),
        endpoint<ClubRequest, ClubReply>("clubService", "join", false, true, joinClubCall),
        endpoint<ClubRequest, ClubReply>("clubService", "requestJoin", false, true, joinClubCall),
        endpoint<ClubRequest, ClubReply>("clubService", "leave", false, true, leaveClubCall),
        endpoint<ClubAccountsRequest, Empty>("clubService", "invite", false, true, inviteToClub),
        endpoint<FindClubInvitesRequest, FindClubInvitesResponse>("clubService", "findInvites",
                                                                  false, true, findClubInvites),
        endpoint<ClubRequest, Empty>("clubService", "rejectInvite", false, true, rejectClubInvite),
        endpoint<ClubRequest, Empty>("clubService", "rejectJoinRequest", false, true,
                                     [](Context&, const ClubRequest&, Empty&) {}),
        endpoint<ClubRequest, ClubReply>("clubService", "updateMembership", false, true,
                                         updateClubMembership),
        endpoint<UpdateCustomizationRequest, UpdateCustomizationResponse>(
            "clubService", "updateCustomization", false, true, updateClubCustomization),
        endpoint<GetKeywordsRequest, GetKeywordsResponse>(
            "keywordService", "getKeywords", false, true,
            [](Context&, const GetKeywordsRequest&, GetKeywordsResponse&) {}),
    };
    return list;
}

const Endpoint* findEndpoint(const QString& service, const QString& method) {
    for (const Endpoint& e : endpoints()) {
        if (e.service == service && e.method == method) {
            return &e;
        }
    }
    return nullptr;
}

std::string endpointList() {
    SvcEndpointDescriptorList list;
    for (const Endpoint& e : endpoints()) {
        if (!e.listed) {
            continue;
        }
        SvcEndpointDescriptor* d = list.add_endpoints();
        d->set_service(e.service.toStdString());
        d->set_servicemethod(e.method.toStdString());
        d->set_requestmessage(std::string(e.request->name()));
        d->set_responsemessage(std::string(e.response->name()));
        d->set_httpmethod("POST");
        d->set_secure(e.secure);
        d->set_initialtokens(100);
        d->set_maxtokens(100);
        d->set_tokensperminute(600);
        d->set_available(true);
    }
    return list.SerializeAsString();
}

HttpResponse egsAnswer(const QString& method, const QByteArray& sessionKey, int status,
                       const QByteArray& body) {
    HttpResponse r;
    r.status = status;
    r.contentType = "application/octet-stream";
    r.body = body;
    r.headers.append(
        qMakePair(QByteArray("X-Response-Signature"), egsSignature(method, sessionKey, body)));
    return r;
}

QStringList splitPath(QString path) {
    if (path.startsWith('/')) {
        path.remove(0, 1);
    }
    if (!path.startsWith(QLatin1String("egs/binary/"))) {
        return {};
    }
    return path.mid(11).split('/', Qt::SkipEmptyParts);
}

} // namespace

// The invitee of a private lobby waits ("Joining lobby") without asking for anything: it waits for the
// lobby to be pushed. Push the inviter's lobby with the invitee added to both, 2 s after the invite.
// Tried out by pushing each lobby type and action: lobby type 1 with action 4 makes both games take the lobby as
// theirs (each answers with updateMember); type 0, and the other actions 0-16, get no reaction.
// DCCOOP_INVITE_LOBBYTYPE and DCCOOP_ACTION_INVITE override type and action.
void egsLobbyInvitation(const QString& from, const QString& to) {
    LobbyState pushed;
    bool found = false;
    {
        QMutexLocker lock(&lobbies().mutex);
        for (LobbyState& l : lobbies().list) {
            bool hasFrom = false, hasTo = false;
            for (const LobbyMemberState& m : l.members) {
                hasFrom = hasFrom || m.username.compare(from, Qt::CaseInsensitive) == 0;
                hasTo = hasTo || m.username.compare(to, Qt::CaseInsensitive) == 0;
            }
            if (!hasFrom) {
                continue;
            }
            if (!hasTo) {
                LobbyMemberState invitee;
                invitee.username = to;
                invitee.accountId = accountIdFor(to);
                l.members << invitee;
            }
            pushed = l;
            found = true;
            break;
        }
    }
    if (!found) {
        // The inviter has no lobby here (the server restarted since its login): make one.
        LobbyMemberState host, invitee;
        host.username = from;
        host.accountId = accountIdFor(from);
        host.host = true;
        invitee.username = to;
        invitee.accountId = accountIdFor(to);
        pushed.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pushed.type = 1;
        pushed.members << host << invitee;
        QMutexLocker lock(&lobbies().mutex);
        lobbies().list << pushed;
    }
    pushed.type = envInt("DCCOOP_INVITE_LOBBYTYPE", 1);
    const int action = envInt("DCCOOP_ACTION_INVITE", 4);
    qCInfo(lcEgs).noquote() << "invitation" << from << "->" << to << ": pushing" << describe(pushed)
                            << "action" << action;
    QTimer::singleShot(std::chrono::seconds(2), [pushed, action] {
        LobbyCallbackMessage msg;
        msg.set_action(static_cast<LobbyAction>(action));
        fillLobby(pushed, msg.mutable_lobby());
        for (const LobbyMemberState& m : pushed.members) {
            push(m.username, msg);
        }
    });
}

void setEgsConfig(const EgsConfig& c) {
    config() = c;
}

QByteArray egsSignature(const QString& method, const QByteArray& sessionKey,
                        const QByteArray& body) {
    QMessageAuthenticationCode mac(QCryptographicHash::Sha1, QByteArray(EgsKey));
    mac.addData(method.toUtf8());
    mac.addData(sessionKey);
    mac.addData(body);
    return mac.result().toBase64();
}

std::optional<QByteArray> egsFindSessionKey(const QString& method, const QByteArray& body,
                                            const QByteArray& signature) {
    if (egsSignature(method, {}, body) == signature) {
        return QByteArray();
    }
    QMutexLocker lock(&sessions().mutex);
    for (auto it = sessions().byKey.begin(); it != sessions().byKey.end(); ++it) {
        if (egsSignature(method, it.key(), body) == signature) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const bool save = now - it->lastSeen >= 10000; // at most every 10 s per session
            it->lastSeen = now;
            ++it->requests;
            if (save) {
                db::touchSession(it.key(), it->lastSeen, it->requests);
            }
            return it.key();
        }
    }
    return std::nullopt;
}

QString egsSessionName(const QByteArray& sessionKey) {
    QMutexLocker lock(&sessions().mutex);
    const auto it = sessions().byKey.constFind(sessionKey);
    return it == sessions().byKey.constEnd() ? QString() : it->name;
}

namespace {

void copyEnum24(const gp::EnumDescriptorProto& in, gp::EnumDescriptorProto* out) {
    out->set_name(in.name());
    for (const auto& v : in.value()) {
        auto* o = out->add_value();
        o->set_name(v.name());
        o->set_number(v.number());
    }
}

void copyMessage24(const gp::DescriptorProto& in, gp::DescriptorProto* out) {
    out->set_name(in.name());
    for (const auto& f : in.field()) {
        auto* o = out->add_field();
        o->set_name(f.name());
        o->set_number(f.number());
        o->set_label(f.label());
        o->set_type(f.type());
        if (f.has_type_name()) {
            o->set_type_name(f.type_name());
        }
        if (f.has_default_value()) {
            o->set_default_value(f.default_value());
        }
    }
    for (const auto& n : in.nested_type()) {
        copyMessage24(n, out->add_nested_type());
    }
    for (const auto& e : in.enum_type()) {
        copyEnum24(e, out->add_enum_type());
    }
}

} // namespace

QByteArray egsDescriptor() {
    static const QByteArray d = [] {
        gp::FileDescriptorProto full;
        SvcEndpointDescriptorList::descriptor()->file()->CopyTo(&full);
        gp::FileDescriptorProto file;
        file.set_name(full.name());
        for (const auto& m : full.message_type()) {
            copyMessage24(m, file.add_message_type());
        }
        for (const auto& e : full.enum_type()) {
            copyEnum24(e, file.add_enum_type());
        }
        const std::string s = file.SerializeAsString();
        return QByteArray(s.data(), static_cast<qsizetype>(s.size()));
    }();
    return d;
}

QString egsDescribeBody(const QString& path, bool response, int status, const QByteArray& body) {
    const QStringList parts = splitPath(path);
    if (parts.size() < 2) {
        return {};
    }
    const Endpoint* e = findEndpoint(parts.at(0), parts.at(1));
    if (!e) {
        return {};
    }
    const gp::Descriptor* type =
        !response ? e->request : (status >= 500 ? StatusResult::descriptor() : e->response);
    std::unique_ptr<gp::Message> msg(
        gp::MessageFactory::generated_factory()->GetPrototype(type)->New());
    if (!msg->ParseFromArray(body.constData(), static_cast<int>(body.size()))) {
        return QStringLiteral("(does not parse as %1)").arg(qs(type->name()));
    }
    const gp::Reflection* reflection = msg->GetReflection();
    for (int i = 0; i < type->field_count(); ++i) {
        const gp::FieldDescriptor* field = type->field(i);
        if (field->type() == gp::FieldDescriptor::TYPE_BYTES && !field->is_repeated() &&
            reflection->HasField(*msg, field)) {
            const std::string value = reflection->GetString(*msg, field);
            if (value.size() > 64) {
                reflection->SetString(msg.get(), field,
                                      "<" + std::to_string(value.size()) + " bytes, not shown>");
            }
        }
    }
    std::string text;
    gp::TextFormat::Printer printer;
    printer.SetUseUtf8StringEscaping(true);
    printer.PrintToString(*msg, &text);
    QStringList lines = qs(text).trimmed().split('\n');
    for (QString& line : lines) {
        line.prepend(QStringLiteral("  "));
    }
    return qs(type->name()) + QStringLiteral(" {\n") + lines.join('\n') + QStringLiteral("\n}");
}

bool handleEgs(const HttpRequest& request, const QString& endpoint,
               const HttpServer::Responder& respond) {
    if (endpoint == QLatin1String("egs/messaging")) {
        // The game polls this for pushed messages. Answering at once makes it poll several times a second
        // and, 30 s later, treat the server as lost. Hold the request instead (long poll), shorter than the
        // game's 15 s receive timeout, and answer as soon as something is queued for this player.
        const QString name = QString::fromUtf8(request.headers.value("x-dccoop-online-id"));
        if (!name.isEmpty() && request.headers.contains("x-dccoop-stream")) {
            HttpResponse r;
            r.contentType = "application/octet-stream";
            r.stream = [name](std::function<bool(const QByteArray&)> write) { openStream(name, write); };
            respond(r);
            return true;
        }
        // Long poll: answer with whatever is queued for this player, or after 10 s with nothing.
        auto timer = std::make_shared<QTimer>();
        auto waited = std::make_shared<int>(0);
        QObject::connect(timer.get(), &QTimer::timeout, [timer, waited, name, respond] {
            QByteArray pending;
            if (!name.isEmpty()) {
                QMutexLocker lock(&pushQueues().mutex);
                pending = pushQueues().byName.take(name.toLower()).join();
            }
            *waited += 250;
            if (pending.isEmpty() && *waited < 10000) {
                return;
            }
            timer->stop();
            // Bare <msg> elements as text/xml: the game parses the body as XML only with an XML
            // content type (otherwise the request counts as failed, 0xc0220002), and then hands
            // each <msg> to its handler.
            HttpResponse r;
            r.contentType = "text/xml";
            if (pending.isEmpty()) {
                r.body = "<msg t=\"Empty\" s=\"0\"></msg>";
                r.keepalive = true; // counted in the trace, not logged one by one
            } else {
                r.body = pending;
                qCInfo(lcEgs).noquote() << "messaging: delivered to" << name << pending.size()
                                        << "bytes";
            }
            respond(r);
            // Drop the connection (and with it the slot's own reference) once this slot has returned.
            QTimer::singleShot(0, [timer] { timer->disconnect(); });
        });
        timer->start(250);
        return true;
    }
    const QStringList parts = splitPath(endpoint);
    if (parts.isEmpty()) {
        return false;
    }
    const QString service = parts.value(0);
    const QString method = parts.value(1);

    const auto found =
        egsFindSessionKey(method, request.body, request.headers.value("x-request-signature"));
    const QByteArray sessionKey = found.value_or(QByteArray());

    const Endpoint* e = findEndpoint(service, method);
    if (!e) {
        qCWarning(lcEgs).noquote() << "no EGS method" << service + '.' + method
                                   << "- add its messages to proto/egs.proto and an entry to "
                                      "endpoints() in src/egs.cpp";
        HttpResponse r = egsAnswer(method, sessionKey, 404, {});
        r.headers.append(
            qMakePair(QByteArray("X-Service-Error"), QByteArray("Service Method unavailable")));
        respond(r);
        return true;
    }

    auto run = [e, service, method, sessionKey, respond](const HttpRequest& req,
                                                         const AuthResult& auth) {
        Context ctx{req, sessionKey, {}, 200, auth};
        const std::string out = e->run(ctx, req.body);
        const QByteArray body(out.data(), static_cast<qsizetype>(out.size()));
        const QByteArray signKey = ctx.newSessionKey.isEmpty() ? ctx.sessionKey : ctx.newSessionKey;
        HttpResponse r = egsAnswer(method, signKey, ctx.status, body);
        if (!ctx.newSessionKey.isEmpty()) {
            r.headers.append(qMakePair(QByteArray("X-SessionKey"), ctx.newSessionKey));
        }
        QString who;
        if (!ctx.sessionKey.isEmpty()) {
            who = QStringLiteral(" (session of %1)").arg(egsSessionName(ctx.sessionKey));
        }
        qCInfo(lcEgs).noquote() << QStringLiteral("%1.%2 -> %3, %4 bytes%5")
                                       .arg(service, method, qs(e->response->name()))
                                       .arg(body.size())
                                       .arg(who);
        respond(r);
    };

    if (e->request != OAuthLoginRequest::descriptor()) {
        run(request, AuthResult{});
        return true;
    }

    OAuthLoginRequest login;
    login.ParseFromArray(request.body.constData(), static_cast<int>(request.body.size()));
    verifyCode(
        QStringLiteral("EGS"), qs(login.authcode()),
        [run, request, method, sessionKey, respond](const AuthResult& auth) {
            if (auth.outcome != AuthResult::Outcome::Denied) {
                run(request, auth);
                return;
            }
            StatusResult result;
            result.set_retryhint(auth.unavailable ? 30 : 300);
            const std::string out = result.SerializeAsString();
            HttpResponse r = egsAnswer(method, sessionKey, auth.unavailable ? 503 : 500,
                                       QByteArray(out.data(), qsizetype(out.size())));
            r.headers.append(qMakePair(
                QByteArray("X-Service-Error"),
                QByteArray(auth.unavailable ? "Service unavailable" : "INVALID Oauth Token")));
            qCWarning(lcEgs).noquote() << method << "denied:" << auth.message;
            respond(r);
        });
    return true;
}

QList<EgsPlayerInfo> egsPlayers() {
    QList<EgsPlayerInfo> out;
    for (const db::Player& p : db::players()) {
        out.append({p.accountId, p.name, p.loggedIn});
    }
    return out;
}

void egsSetPlayerName(qint64 accountId, const QString& name) {
    if (accountId != 0 && !name.trimmed().isEmpty()) {
        rememberPlayer(accountId, name.trimmed(), false);
    }
}

QString egsPlayerName(qint64 accountId) {
    if (const auto p = db::player(accountId)) {
        return p->name;
    }
    return QStringLiteral("Driver %1").arg(accountId);
}

void egsRestoreSessions() {
    QMutexLocker lock(&sessions().mutex);
    for (const db::Session& d : db::sessions()) {
        Session s;
        s.key = d.key;
        s.accountId = d.accountId;
        s.name = d.name;
        s.shadnetAccount = d.shadnetAccount;
        s.clientVersion = d.clientVersion;
        s.psPlus = d.psPlus;
        s.created = d.created;
        s.lastSeen = d.lastSeen;
        s.requests = d.requests;
        sessions().byKey.insert(s.key, s);
    }
    if (!sessions().byKey.isEmpty()) {
        qCInfo(lcEgs).noquote() << "restored" << sessions().byKey.size() << "EGS sessions";
    }
}

void egsSetFame(qint64 accountId, qint64 xp) {
    db::Fame f = db::fame(accountId).value_or(db::Fame{accountId, 0, {}, {}, 0});
    f.accountId = accountId;
    f.xp = std::max<qint64>(0, xp);
    f.updated = QDateTime::currentMSecsSinceEpoch();
    db::putFame(f);
    db::putScore({QStringLiteral("fame/player"),
                  QString::number(accountId),
                  (quint64(2) << 60) | quint64(f.xp),
                  {},
                  f.updated});
    qCInfo(lcEgs).noquote() << "fame of" << egsPlayerName(accountId) << "set to" << f.xp;
    egsUpdateClubFameBoard();
}

void egsUpdateClubFameBoard() {
    for (const db::Club& c : db::clubs()) {
        const qint64 xp = clubFame(c.id);
        if (xp > 0) {
            db::putScore({QStringLiteral("fame/club"), QString::number(c.id), (quint64(2) << 60) | quint64(xp), {},
                          QDateTime::currentMSecsSinceEpoch()});
        }
    }
}

QList<EgsSessionInfo> egsSessions() {
    QMutexLocker lock(&sessions().mutex);
    QList<EgsSessionInfo> out;
    for (const Session& s : sessions().byKey) {
        out.append({QString::fromLatin1(s.key), s.name, s.accountId, s.shadnetAccount,
                    s.clientVersion, s.psPlus, s.created, s.lastSeen, s.requests, s.transaction});
    }
    return out;
}

bool egsDropSession(const QString& key) {
    db::removeSession(key.toLatin1());
    QMutexLocker lock(&sessions().mutex);
    return sessions().byKey.remove(key.toLatin1()) > 0;
}
