// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "egs.h"

#include "auth.h"
#include "database.h"
#include "egs.pb.h"
#include "otg.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QLoggingCategory>
#include <QMessageAuthenticationCode>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QStringList>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
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

// ---- Sessions ---------------------------------------------------------------------------------

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
    QHostAddress peer;
    qint64 lastPoll = 0;
    QList<QPair<QByteArray, std::string>> outbox;
};

struct Sessions {
    QMutex mutex;
    QHash<QByteArray, Session> byKey;
};

Sessions& sessions() {
    static Sessions s;
    return s;
}

// ---- Online status (sessionService.queryStatus, SessionCallbackMessage) ------------------------

constexpr qint64 OnlineTimeoutMs = 30000;

struct Presence {
    QSet<qint64> online;                  // as of the last check
    QHash<qint64, QSet<qint64>> watchers; // account -> accounts that asked about it
    qint64 lastCheck = 0;
};

Presence& presence() {
    static Presence p;
    return p;
}

QSet<qint64> onlineAccountsLocked(qint64 now) {
    QSet<qint64> out;
    for (const Session& s : std::as_const(sessions().byKey)) {
        if (s.accountId != 0 && s.lastPoll != 0 && now - s.lastPoll < OnlineTimeoutMs) {
            out.insert(s.accountId);
        }
    }
    return out;
}

void pushLocked(qint64 account, const QByteArray& type, const std::string& data) {
    Session* newest = nullptr;
    for (Session& s : sessions().byKey) {
        if (s.accountId == account && (!newest || s.lastSeen > newest->lastSeen)) {
            newest = &s;
        }
    }
    if (newest && newest->outbox.size() < 200) {
        newest->outbox.append(qMakePair(type, data));
    }
}

// ---- Parties (lobbyService, lobbyType 1) ------------------------------------------------------

constexpr int LobbyJoined = 4, LobbyLeft = 6, LobbyStatusChanged = 7, LobbyMemberData = 8,
              LobbyHostChanged = 12, LobbyKicked = 14;

struct PartyMember {
    qint64 accountId = 0;
    QString name;
    bool host = false;
    std::string contentRestriction;
};

struct Party {
    QString id;
    int type = 1;
    int status = 1; // LobbyStatus 1 = created; the leader sets its own with updateLobby
    qint64 matchSlotId = 0;
    QList<PartyMember> members;
};

struct Parties {
    QHash<QString, Party> byId;
    QHash<QPair<qint64, int>, QString> ofAccount; // (account, lobby type) -> lobby id
};

Parties& parties() {
    static Parties p;
    return p;
}

void fillLobby(::Lobby* out, const Party& p) {
    out->set_id(p.id.toStdString());
    out->set_lobbytype(static_cast<LobbyValue>(p.type));
    out->set_status(static_cast<LobbyValue>(p.status));
    if (p.matchSlotId != 0) {
        out->set_matchslotid(p.matchSlotId);
    }
    for (const PartyMember& m : p.members) {
        ::LobbyMember* lm = out->add_memberslist();
        lm->mutable_sessionkey()->set_accountid(m.accountId);
        lm->mutable_sessionkey()->set_username(m.name.toStdString());
        lm->set_host(m.host);
        lm->set_team(0);
        if (!m.contentRestriction.empty()) {
            lm->set_contentrestriction(m.contentRestriction);
        }
    }
}

void pushLobbyLocked(const Party& p, int action, const QList<qint64>& updated, QList<qint64> to) {
    LobbyCallbackMessage m;
    m.set_action(static_cast<LobbyValue>(action));
    fillLobby(m.mutable_lobby(), p);
    for (const qint64 u : updated) {
        m.add_updatedmembers()->mutable_sessionkey()->set_accountid(u);
    }
    const std::string data = m.SerializeAsString();
    std::sort(to.begin(), to.end());
    to.erase(std::unique(to.begin(), to.end()), to.end());
    for (const qint64 a : to) {
        if (a != 0) {
            pushLocked(a, QByteArrayLiteral("LobbyCallbackMessage"), data);
        }
    }
}

QList<qint64> partyAccounts(const Party& p) {
    QList<qint64> out;
    for (const PartyMember& m : p.members) {
        out.append(m.accountId);
    }
    return out;
}

QString leavePartyLocked(qint64 account, int type, int action = LobbyLeft,
                         const QList<qint64>& alsoTell = {}) {
    const QString id = parties().ofAccount.take(qMakePair(account, type));
    auto it = parties().byId.find(id);
    if (id.isEmpty() || it == parties().byId.end()) {
        return id;
    }
    Party& p = *it;
    bool wasHost = false;
    for (qsizetype i = 0; i < p.members.size(); ++i) {
        if (p.members.at(i).accountId == account) {
            wasHost = p.members.at(i).host;
            p.members.removeAt(i);
            break;
        }
    }
    if (p.members.isEmpty()) {
        parties().byId.erase(it);
        return id;
    }
    if (wasHost) {
        p.members.first().host = true;
    }
    pushLobbyLocked(p, action, {account}, partyAccounts(p) + alsoTell);
    return id;
}

void leaveAllPartiesLocked(qint64 account) {
    QList<int> types;
    for (auto it = parties().ofAccount.cbegin(); it != parties().ofAccount.cend(); ++it) {
        if (it.key().first == account) {
            types.append(it.key().second);
        }
    }
    for (const int t : types) {
        const QString id = leavePartyLocked(account, t);
        qCInfo(lcEgs).noquote() << "account" << account << "went offline: left lobby" << id;
    }
}

void presenceCheckLocked(qint64 now) {
    Presence& p = presence();
    if (now - p.lastCheck < 2000) {
        return;
    }
    p.lastCheck = now;
    const QSet<qint64> online = onlineAccountsLocked(now);
    auto notify = [&](qint64 account, SessionAction action) {
        const auto it = p.watchers.constFind(account);
        if (it == p.watchers.constEnd()) {
            return;
        }
        SessionCallbackMessage m;
        m.set_accountid(account);
        m.set_sessionaction(action);
        const std::string data = m.SerializeAsString();
        for (const qint64 w : *it) {
            if (w != account && online.contains(w)) {
                pushLocked(w, QByteArrayLiteral("SessionCallbackMessage"), data);
            }
        }
    };
    for (const qint64 a : online) {
        if (!p.online.contains(a)) {
            notify(a, SESSION_ACTION_STARTED);
        }
    }
    for (const qint64 a : std::as_const(p.online)) {
        if (!online.contains(a)) {
            notify(a, SESSION_ACTION_ENDED);
            leaveAllPartiesLocked(a);
        }
    }
    p.online = online;
}

// ---- Players (account id -> name) -------------------------------------------------------------

void rememberPlayer(qint64 accountId, const QString& name, bool loggedIn,
                    const QString& shadnetAccount = {}) {
    db::Player p;
    p.accountId = accountId;
    p.name = name;
    p.loggedIn = loggedIn;
    p.shadnetAccount = shadnetAccount;
    db::putPlayer(p);
}

// Stable positive id per online name, so a player keeps the same accountId across logins.
qint32 accountIdFor(const QString& name) {
    const QByteArray h = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha1);
    const quint32 v =
        (quint8(h[0]) << 24) | (quint8(h[1]) << 16) | (quint8(h[2]) << 8) | quint8(h[3]);
    return static_cast<qint32>(v & 0x7fffffff) | 1;
}

// protobuf returns std::string in older releases and std::string_view in newer ones.
QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// ---- Endpoints --------------------------------------------------------------------------------

struct Context {
    const HttpRequest& request;
    QByteArray sessionKey;    // the session this request was signed with, empty before login
    QByteArray newSessionKey; // set by a handler: sent as X-SessionKey, becomes the signing key
    int status = 200;
    AuthResult auth;         // login endpoints: the shadNet check of the authCode
    QByteArray serviceError; // set by a handler: sent as X-Service-Error
    int serviceErrorCode =
        0; // set by a handler: sent as X-Service-ErrorCode (the game's error number)
};

struct Endpoint {
    QString service;
    QString method;
    const gp::Descriptor* request;
    const gp::Descriptor* response;
    bool secure;
    bool listed; // sent in getSvcEndpointList
    std::function<std::string(Context&, const QByteArray&)> run;
    enum class Discovery { None, FriendLists } discovery = Discovery::None;
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
    // A verified login uses shadNet's online id, not the name the client sent.
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
    s.peer = ctx.request.peer; // for matching /egs/messaging polls (online status)
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
        AccountProgression* pr = p->mutable_accountprogression();
        if (const auto f = db::fame(id)) {
            pr->set_xp(f->xp);
            if (f->stars) {
                pr->set_stars(*f->stars);
            }
            if (f->elite) {
                pr->set_elite(*f->elite);
            }
        } else {
            pr->set_xp(0);
        }
        if (const auto m = db::clubMember(id)) {
            p->set_clubid(m->clubId); // read only when present (0x628F12)
        }
        names << QStringLiteral("%1=%2").arg(id).arg(name);
    }
    qCInfo(lcEgs).noquote() << "profiles for" << req.accounts_size()
                            << "accounts:" << names.join(QStringLiteral(", "));
}

qint64 sessionAccount(const Context& ctx, QString* name = nullptr);

void aggregatedProfiles(Context& ctx, const GetAggregatedProfilesRequest& req,
                        GetAggregatedProfilesResponse& resp) {
    QString me;
    const qint64 account = config().friendLists ? sessionAccount(ctx, &me) : 0;
    QStringList added;
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
        AggregatedProfile* profile = resp.add_profiles();
        AccountRef* a = profile->mutable_account();
        a->set_id(id);
        a->set_name(name.toStdString());
        if (const auto m = db::clubMember(id)) {
            profile->mutable_club()->set_id(m->clubId);
        }
        AggregatedProgression* pr = profile->mutable_progression();
        pr->set_xp(0);
        if (const auto f = db::fame(id)) {
            pr->set_xp(f->xp);
            if (f->stars) {
                pr->set_stars(*f->stars);
            }
            if (f->elite) {
                pr->set_elite(*f->elite);
            }
        }
        found << QStringLiteral("%1=%2%3").arg(name).arg(id).arg(
            known ? QString() : QStringLiteral(" (not played yet)"));
        if (account != 0 && name.compare(me, Qt::CaseInsensitive) != 0 &&
            db::addFriend(account, name)) {
            added << name;
        }
    }
    qCInfo(lcEgs).noquote() << "aggregated profiles:" << found.join(QStringLiteral(", "));
    if (!added.isEmpty()) {
        qCInfo(lcEgs).noquote() << "friends added for" << me
                                << "from the PSN friend list:" << added.join(QStringLiteral(", "))
                                << "->" << db::friendNames(account).size() << "friends";
    }
}

qint64 friendAccountId(const QString& name) {
    const auto known = db::playerByName(name);
    return known ? known->accountId : accountIdFor(name);
}

void fillFriends(GetFriendsResponse& resp, const QStringList& names) {
    for (const QString& n : names) {
        const auto known = db::playerByName(n);
        FriendAccount* a = resp.add_friends()->mutable_account();
        a->set_id(known ? known->accountId : accountIdFor(n));
        a->set_lastseen(known ? std::max<qint64>(known->lastSeen, known->firstSeen) : 0);
        a->set_name((known ? known->name : n).toStdString());
    }
}

void friends(Context& ctx, const GetFriendsRequest&, GetFriendsResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    QStringList names;
    if (config().friendLists) {
        names = account ? db::friendNames(account) : QStringList();
    } else {
        for (const EgsPlayerInfo& p : egsPlayers()) {
            if (p.name != me) {
                names << p.name;
            }
        }
    }
    fillFriends(resp, names);
    int playing = 0;
    for (const Friend& f : resp.friends()) {
        playing += f.account().lastseen() != 0;
    }
    qCInfo(lcEgs).noquote() << "friends of" << (me.isEmpty() ? QStringLiteral("?") : me) << "->"
                            << names.size() << QStringLiteral("(%1 play here)").arg(playing)
                            << (names.isEmpty() ? QString() : names.join(QStringLiteral(", ")))
                            << (config().friendLists
                                    ? QString()
                                    : QStringLiteral("(everyone: egs.friend_lists off)"));
}

void changeFriends(Context& ctx, const FriendsChangeRequest& req, GetFriendsResponse& resp,
                   bool add) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    QStringList changed;
    QStringList unchanged;
    for (const FriendName& f : req.data().accounts()) {
        const QString name = qs(f.name()).trimmed();
        if (name.isEmpty() || account == 0) {
            continue;
        }
        if (add && name.compare(me, Qt::CaseInsensitive) == 0) {
            unchanged << name + QStringLiteral(" (self)");
            continue;
        }
        const bool done = add ? db::addFriend(account, name) : db::removeFriend(account, name);
        (done ? changed : unchanged) << name;
    }
    const QStringList now = account ? db::friendNames(account) : QStringList();
    fillFriends(resp, now);
    qCInfo(lcEgs).noquote()
        << (add ? "friends added for" : "friends removed for")
        << (me.isEmpty() ? QStringLiteral("?") : me) << ":"
        << (changed.isEmpty() ? QStringLiteral("none") : changed.join(QStringLiteral(", ")))
        << (unchanged.isEmpty()
                ? QString()
                : QStringLiteral("(unchanged: %1)").arg(unchanged.join(QStringLiteral(", "))))
        << "->" << now.size() << (now.size() == 1 ? "friend" : "friends");
}

void addFriends(Context& ctx, const FriendsChangeRequest& req, GetFriendsResponse& resp) {
    changeFriends(ctx, req, resp, true);
}

void removeFriends(Context& ctx, const FriendsChangeRequest& req, GetFriendsResponse& resp) {
    changeFriends(ctx, req, resp, false);
}

void queryStatus(Context& ctx, const QueryStatusRequest& req, QueryStatusResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    QStringList on;
    int asked = 0;
    {
        QMutexLocker lock(&sessions().mutex);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const QSet<qint64> online = onlineAccountsLocked(now);
        for (const IdRef& a : req.accounts()) {
            if (a.id() == 0) {
                continue;
            }
            ++asked;
            if (account != 0) {
                presence().watchers[a.id()].insert(account);
            }
            if (online.contains(a.id())) {
                resp.add_sessions()->mutable_sessionkey()->set_accountid(a.id());
                on << QString::number(a.id());
            }
        }
    }
    QStringList names;
    for (const QString& id : on) {
        names << egsPlayerName(id.toLongLong());
    }
    qCInfo(lcEgs).noquote() << "online status for" << (me.isEmpty() ? QStringLiteral("?") : me)
                            << ":" << names.size() << "of" << asked << "online"
                            << (names.isEmpty() ? QString() : names.join(QStringLiteral(", ")));
}

// ---- lobbyService: parties --------------------------------------------------------------------

QString lobbyWho(qint64 account, const QString& name) {
    return name.isEmpty() ? QString::number(account) : name;
}

void createLobby(Context& ctx, const LobbyWithMemberRequest& req, LobbiesResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const int type = req.lobby().lobbytype();
    if (account == 0) {
        ctx.status = 500;
        ctx.serviceError = "no session";
        return;
    }
    Party p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.type = type;
    PartyMember m;
    m.accountId = account;
    m.name = me;
    m.host = true;
    m.contentRestriction = req.member().contentrestriction();
    p.members.append(m);
    QString old;
    {
        QMutexLocker lock(&sessions().mutex);
        old = leavePartyLocked(account, type);
        parties().byId.insert(p.id, p);
        parties().ofAccount.insert(qMakePair(account, type), p.id);
    }
    fillLobby(resp.add_lobbies(), p);
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "created lobby" << p.id << "type" << type
                            << (old.isEmpty() ? QString() : QStringLiteral("(left %1)").arg(old));
}

void getLobby(Context& ctx, const LobbyWithMemberRequest& req, LobbiesResponse& resp) {
    const qint64 account = sessionAccount(ctx);
    QMutexLocker lock(&sessions().mutex);
    const QString id = parties().ofAccount.value(qMakePair(account, int(req.lobby().lobbytype())));
    const auto it = parties().byId.constFind(id);
    if (!id.isEmpty() && it != parties().byId.cend()) {
        fillLobby(resp.add_lobbies(), *it);
    }
}

void joinWithAccount(Context& ctx, const LobbyJoinWithAccountRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const int type = req.has_type() ? int(req.type()) : 1;
    qint64 target = req.accounttojoin().id();
    if (target == 0 && !req.accounttojoin().name().empty()) {
        const auto p = db::playerByName(qs(req.accounttojoin().name()));
        target = p ? p->accountId : 0;
    }
    const QString targetName = target ? egsPlayerName(target) : qs(req.accounttojoin().name());
    QMutexLocker lock(&sessions().mutex);
    const QSet<qint64> online = onlineAccountsLocked(QDateTime::currentMSecsSinceEpoch());
    const QString id = parties().ofAccount.value(qMakePair(target, type));
    auto it = parties().byId.find(id);
    if (account == 0 || target == 0 || id.isEmpty() || it == parties().byId.end() ||
        !online.contains(target)) {
        ctx.status = 500;
        ctx.serviceErrorCode = -18009;
        ctx.serviceError = "member offline";
        qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "could not join" << targetName
                                << "(no party, or offline)";
        return;
    }
    if (parties().ofAccount.value(qMakePair(account, type)) != id) {
        leavePartyLocked(account, type);
        it = parties().byId.find(id); // the hash may have changed
        if (it == parties().byId.end()) {
            ctx.status = 500;
            ctx.serviceErrorCode = -18009;
            return;
        }
        PartyMember m;
        m.accountId = account;
        m.name = me;
        m.contentRestriction = req.extrajoindetails().contentrestriction();
        it->members.append(m);
        parties().ofAccount.insert(qMakePair(account, type), id);
    }
    pushLobbyLocked(*it, LobbyJoined, {account}, partyAccounts(*it));
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "joined"
                            << QStringLiteral("%1's party").arg(targetName) << id << "->"
                            << it->members.size() << "members";
}

void updateLobby(Context& ctx, const LobbyWithMemberRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const int type = req.lobby().lobbytype();
    QMutexLocker lock(&sessions().mutex);
    auto it = parties().byId.find(parties().ofAccount.value(qMakePair(account, type)));
    if (it == parties().byId.end()) {
        return;
    }
    if (req.lobby().has_matchslotid()) {
        it->matchSlotId = req.lobby().matchslotid();
    }
    if (req.lobby().has_status()) {
        it->status = req.lobby().status();
        QList<qint64> others = partyAccounts(*it);
        others.removeAll(account);
        pushLobbyLocked(*it, LobbyStatusChanged, {}, others);
        qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "set party" << it->id << "status"
                                << it->status;
    }
}

void updateMember(Context& ctx, const LobbyWithMemberRequest& req, Empty&) {
    const qint64 account = sessionAccount(ctx);
    const int type = req.lobby().lobbytype();
    QMutexLocker lock(&sessions().mutex);
    auto it = parties().byId.find(parties().ofAccount.value(qMakePair(account, type)));
    if (it == parties().byId.end()) {
        return;
    }
    for (PartyMember& m : it->members) {
        if (m.accountId == account && req.member().has_contentrestriction()) {
            m.contentRestriction = req.member().contentrestriction();
        }
    }
    QList<qint64> others = partyAccounts(*it);
    others.removeAll(account);
    pushLobbyLocked(*it, LobbyMemberData, {account}, others);
}

void leaveCurrentLobby(Context& ctx, const LobbyLeaveCurrentRequest& req,
                       LobbyLeaveCurrentResponse&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    QString id;
    {
        QMutexLocker lock(&sessions().mutex);
        id = leavePartyLocked(account, int(req.lobbytype()));
    }
    if (!id.isEmpty()) {
        qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "left lobby" << id;
    }
}

void leaveLobbyById(Context& ctx, const LobbyLeaveByIdRequest& req, LobbyLeaveByIdResponse&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const QString wanted = qs(req.lobbyid());
    QStringList left;
    {
        QMutexLocker lock(&sessions().mutex);
        QList<int> types;
        for (auto it = parties().ofAccount.cbegin(); it != parties().ofAccount.cend(); ++it) {
            // An empty id (sent by a kicked player, who is already out) leaves nothing.
            if (it.key().first == account && !wanted.isEmpty() && it.value() == wanted) {
                types.append(it.key().second);
            }
        }
        for (const int t : types) {
            left << leavePartyLocked(account, t);
        }
    }
    if (!left.isEmpty()) {
        qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "left lobby"
                                << left.join(QStringLiteral(", "));
    }
}

void removeFromLobby(Context& ctx, const LobbyRemoveRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const qint64 kicked = req.accountid();
    const QString id = qs(req.lobbyid());
    {
        QMutexLocker lock(&sessions().mutex);
        const auto it = parties().byId.constFind(id);
        if (it == parties().byId.cend() || kicked == account) {
            return;
        }
        bool host = false;
        bool member = false;
        for (const PartyMember& m : it->members) {
            host = host || (m.accountId == account && m.host);
            member = member || m.accountId == kicked;
        }
        if (!host || !member) {
            return; // only the party leader kicks
        }
        leavePartyLocked(kicked, it->type, LobbyKicked, {kicked});
    }
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "removed" << egsPlayerName(kicked)
                            << "from party" << id;
}

void changeHost(Context& ctx, const LobbyChangeHostRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const int type = req.lobby().has_lobbytype() ? int(req.lobby().lobbytype()) : 1;
    QString id;
    qint64 newHost = req.newhost().id();
    {
        QMutexLocker lock(&sessions().mutex);
        auto it = parties().byId.find(parties().ofAccount.value(qMakePair(account, type)));
        if (it == parties().byId.end()) {
            return;
        }
        id = it->id;
        const QList<qint64> accounts = partyAccounts(*it);
        if (!accounts.contains(newHost)) {
            newHost = 0;
            for (const qint64 a : accounts) {
                if (a != account) {
                    newHost = a;
                    break;
                }
            }
        }
        if (newHost == 0) {
            return;
        }
        for (PartyMember& m : it->members) {
            m.host = m.accountId == newHost;
        }
        pushLobbyLocked(*it, LobbyHostChanged, {newHost}, accounts);
    }
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "made" << egsPlayerName(newHost)
                            << "host of party" << id;
}

void quickSearch(Context& ctx, const AccountQuickSearchRequest& req, AccountListResponse& resp) {
    QString me;
    sessionAccount(ctx, &me);
    const QString prefix = qs(req.startswith()).trimmed();
    const int limit = req.maximumentries() > 0 ? req.maximumentries() : 100;
    QList<EgsPlayerInfo> found;
    for (const EgsPlayerInfo& p : egsPlayers()) {
        if (p.name.compare(me, Qt::CaseInsensitive) != 0 &&
            p.name.startsWith(prefix, Qt::CaseInsensitive)) {
            found.append(p);
        }
    }
    std::sort(found.begin(), found.end(), [](const EgsPlayerInfo& a, const EgsPlayerInfo& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    QStringList names;
    for (const EgsPlayerInfo& p : found.mid(0, limit)) {
        resp.add_accounts()->set_id(p.accountId);
        names << p.name;
    }
    qCInfo(lcEgs).noquote() << "player search" << QStringLiteral("\"%1\"").arg(prefix) << "from"
                            << (me.isEmpty() ? QStringLiteral("?") : me) << "->" << names.size()
                            << (names.isEmpty() ? QString() : names.join(QStringLiteral(", ")))
                            << (found.size() > limit
                                    ? QStringLiteral("(%1 more not sent)").arg(found.size() - limit)
                                    : QString());
}

// ---- systemService.getTimeInfo, transactionService ----------------------------------------------

void timeInfo(Context&, const Empty&, GetTimeInfoResponse& resp) {
    resp.set_timestamp(QDateTime::currentMSecsSinceEpoch());
}

// The session a request was signed with, or nullptr (call with sessions().mutex held).
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

// ---- clubService
// ---------------------------------------------------------------------------------

constexpr int ClubMaxMembers = 6;

void putClubFameScore(qint64 club, qint64 xp) {
    const QString entrant = QString::number(club);
    db::putScore({QStringLiteral("fame/club"),
                  entrant,
                  (quint64(2) << 60) | quint64(std::max<qint64>(0, xp)),
                  {},
                  QDateTime::currentMSecsSinceEpoch()});
}
constexpr qint64 ClubInviteTtlMs = 7LL * 24 * 3600 * 1000; // the game's own 7-day reminder

QString isoTime(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc())
        .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz+00:00"));
}

// Queues a push for an account's newest session (delivered on its next /egs/messaging poll).
void egsPush(qint64 account, const gp::Message& m) {
    if (account == 0) {
        return;
    }
    QMutexLocker lock(&sessions().mutex);
    pushLocked(account, QByteArray::fromStdString(std::string(m.GetDescriptor()->name())),
               m.SerializeAsString());
}

void clubFail(Context& ctx, const QString& who, const QString& why) {
    ctx.status = 500;
    ctx.serviceError = why.toUtf8();
    qCWarning(lcEgs).noquote() << "club request from" << (who.isEmpty() ? QStringLiteral("?") : who)
                               << "refused:" << why;
}

QList<qint64> memberIds(qint64 clubId) {
    QList<qint64> out;
    for (const db::ClubMember& m : db::clubMembers(clubId)) {
        out.append(m.accountId);
    }
    return out;
}

void pushMemberChange(qint64 club, qint64 account, ActivityVerb verb, QList<qint64> to) {
    MsgClubMemberChange m;
    m.set_club(club);
    m.set_account(account);
    m.set_verb(verb);
    to.removeAll(0);
    std::sort(to.begin(), to.end());
    to.erase(std::unique(to.begin(), to.end()), to.end());
    for (const qint64 a : to) {
        egsPush(a, m);
    }
}

template <class Msg>
void pushClubRefresh(qint64 club, qint64 editor) {
    Msg m;
    m.set_club(club);
    m.set_account(editor);
    for (const qint64 a : memberIds(club)) {
        if (a != editor) {
            egsPush(a, m);
        }
    }
}

bool isOwner(qint64 club, qint64 account) {
    const auto m = db::clubMember(account);
    return m && m->clubId == club && m->type == db::ClubOwner;
}

void removeFromClub(qint64 club, qint64 account, ActivityVerb verb) {
    const QList<qint64> before = memberIds(club);
    db::removeClubMember(account);
    const QList<db::ClubMember> rest = db::clubMembers(club);
    if (rest.isEmpty()) {
        db::removeClub(club);
        db::removeScore(QStringLiteral("fame/club"), QString::number(club));
        qCInfo(lcEgs).noquote() << "club" << club << "is empty and was removed";
    } else if (std::none_of(rest.begin(), rest.end(),
                            [](const db::ClubMember& m) { return m.type == db::ClubOwner; })) {
        db::ClubMember next = rest.first();
        next.type = db::ClubOwner;
        db::putClubMember(next);
        qCInfo(lcEgs).noquote() << egsPlayerName(next.accountId) << "is now owner of club" << club;
    }
    pushMemberChange(club, account, verb, before);
}

// Puts a player in a club (leaving their old one). False when the club is full or gone.
bool addToClub(qint64 club, qint64 account) {
    if (!db::club(club)) {
        return false;
    }
    const auto old = db::clubMember(account);
    if (old && old->clubId == club) {
        return true;
    }
    if (memberIds(club).size() >= ClubMaxMembers) {
        return false;
    }
    if (old) {
        removeFromClub(old->clubId, account, ACTIVITY_VERB_LEFT);
    }
    db::putClubMember({account, club, db::ClubMemberType, false, 0});
    db::closeClubInvites(account);
    pushMemberChange(club, account, ACTIVITY_VERB_JOINED, memberIds(club));
    return true;
}

void fillCustomization(ClubCustomization* out, const db::Club& c, const QString& base, bool urls) {
    if (!c.customization.isEmpty()) {
        out->ParseFromArray(c.customization.constData(), static_cast<int>(c.customization.size()));
    }
    out->set_id(c.id);
    if (urls && out->badgebobid() > 0) {
        out->set_badgeboburl(
            (base + QStringLiteral("/ghosts/%1").arg(out->badgebobid())).toStdString());
    }
    if (urls && out->smallbadgebobid() > 0) {
        out->set_smallbadgeboburl(
            (base + QStringLiteral("/ghosts/%1").arg(out->smallbadgebobid())).toStdString());
    }
}

void fillClub(::Club* out, const db::Club& c, const QueryClubsRequest& req, const QString& base,
              const QSet<qint64>& online) {
    const QList<db::ClubMember> members = db::clubMembers(c.id);
    out->set_id(c.id);
    out->set_name(c.name.toStdString());
    out->set_membership(static_cast<ClubValue>(c.membership));
    out->set_clubstatus(static_cast<ClubValue>(c.status));
    out->set_size(static_cast<int>(members.size()));
    out->set_createdon(isoTime(c.created).toStdString());
    if (req.withmembers()) {
        for (const db::ClubMember& m : members) {
            ::ClubMember* cm = out->add_clubmembers();
            cm->set_account(m.accountId);
            cm->set_type(static_cast<ClubValue>(m.type));
            cm->set_createdon(isoTime(m.joined).toStdString());
            cm->set_mvp(m.mvp);
            cm->set_online(online.contains(m.accountId));
        }
    }
    if (req.withprogression()) {
        out->mutable_progression()->set_xp(c.xp);
    }
    if (req.withcustomization() && !c.customization.isEmpty()) {
        fillCustomization(out->mutable_customization(), c, base, req.lookupremoteurl());
    }
}

void clubSanitize(Context& ctx, const ClubNameMessage& req, ClubNameMessage& resp) {
    const QString name = qs(req.name()).trimmed();
    resp.set_name(name.toStdString());
    qCInfo(lcEgs).noquote() << "club name check" << QStringLiteral("\"%1\"").arg(name) << "from"
                            << egsSessionName(ctx.sessionKey)
                            << (db::clubByName(name) ? QStringLiteral("(taken)")
                                                     : QStringLiteral("(free)"));
}

void clubCreate(Context& ctx, const ClubCreateRequest& req, ClubReply& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const QString name = qs(req.name()).trimmed();
    if (account == 0 || name.isEmpty()) {
        clubFail(ctx, me, QStringLiteral("no session or no name"));
        return;
    }
    if (db::clubByName(name)) {
        clubFail(ctx, me, QStringLiteral("club name \"%1\" is taken").arg(name));
        return;
    }
    if (const auto old = db::clubMember(account)) {
        removeFromClub(old->clubId, account, ACTIVITY_VERB_LEFT);
    }
    const qint64 id = db::createClub(name, int(req.membership()), account);
    if (id == 0) {
        clubFail(ctx, me, QStringLiteral("club \"%1\" could not be created").arg(name));
        return;
    }
    db::closeClubInvites(account);
    putClubFameScore(id, 0);
    resp.set_id(id);
    resp.set_membership(req.membership());
    qCInfo(lcEgs).noquote() << me << "created club" << id << QStringLiteral("\"%1\"").arg(name)
                            << "membership" << int(req.membership());
}

void queryClubs(Context& ctx, const QueryClubsRequest& req, QueryClubsResponse& resp) {
    QSet<qint64> ids(req.clubids().begin(), req.clubids().end());
    QSet<qint64> accounts(req.accountids().begin(), req.accountids().end());
    QSet<QString> names;
    for (const std::string& n : req.clubnames()) {
        names.insert(qs(n).trimmed().toLower());
    }
    QSet<qint64> withAccount;
    for (const qint64 a : accounts) {
        if (const auto m = db::clubMember(a)) {
            withAccount.insert(m->clubId);
        }
    }
    QList<db::Club> list = db::clubs();
    std::reverse(list.begin(), list.end()); // newest first
    QSet<qint64> online;
    {
        QMutexLocker lock(&sessions().mutex);
        online = onlineAccountsLocked(QDateTime::currentMSecsSinceEpoch());
    }
    const int limit = req.range().numberofresults() > 0 ? req.range().numberofresults() : 100;
    const QString base = otgBaseUrl(ctx.request);
    QStringList found;
    for (const db::Club& c : std::as_const(list)) {
        if ((!ids.isEmpty() && !ids.contains(c.id)) ||
            (!accounts.isEmpty() && !withAccount.contains(c.id)) ||
            (!names.isEmpty() && !names.contains(c.name.toLower()))) {
            continue;
        }
        if (resp.clubs_size() >= limit) {
            break;
        }
        fillClub(resp.add_clubs(), c, req, base, online);
        found << QStringLiteral("%1 \"%2\"").arg(c.id).arg(c.name);
    }
    qCInfo(lcEgs).noquote() << "clubs for" << egsSessionName(ctx.sessionKey) << "-"
                            << qs(req.ShortDebugString()) << "->" << found.size()
                            << (found.isEmpty() ? QString() : found.join(QStringLiteral(", ")));
}

void clubQuickSearch(Context& ctx, const ClubQuickSearchRequest& req, ClubListResponse& resp) {
    const QString prefix = qs(req.startswith()).trimmed();
    const int limit = req.maximumentries() > 0 ? req.maximumentries() : 100;
    QList<db::Club> list;
    for (const db::Club& c : db::clubs()) {
        if (c.name.startsWith(prefix, Qt::CaseInsensitive)) {
            list.append(c);
        }
    }
    std::sort(list.begin(), list.end(), [](const db::Club& a, const db::Club& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    QStringList names;
    for (const db::Club& c : list.mid(0, limit)) {
        resp.add_clubs()->set_id(c.id);
        names << c.name;
    }
    qCInfo(lcEgs).noquote() << "club search" << QStringLiteral("\"%1\"").arg(prefix) << "from"
                            << egsSessionName(ctx.sessionKey) << "->" << names.size()
                            << (names.isEmpty() ? QString() : names.join(QStringLiteral(", ")));
}

void clubJoin(Context& ctx, const ClubIdRequest& req, ClubReply& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    // join {id} (builder 0x62DBA0): an invite id when accepting an invite, else a club id.
    qint64 club = req.id();
    if (const auto inv = db::clubInvite(req.id()); inv && inv->accountId == account) {
        club = inv->clubId;
    }
    if (const auto m = account ? db::clubMember(account) : std::nullopt; m && m->clubId == club) {
        db::closeClubInvites(account, club);
        resp.set_id(club);
        qCInfo(lcEgs).noquote() << me << "is already in club" << club << "(join ignored)";
        return;
    }
    bool invited = false;
    for (const db::ClubInvite& i : db::clubInvites()) {
        invited |= i.accountId == account && i.clubId == club && i.status == db::InviteSent &&
                   i.expires > QDateTime::currentMSecsSinceEpoch();
    }
    if (account == 0 || !invited) {
        clubFail(ctx, me, QStringLiteral("no open invite to club %1").arg(club));
        return;
    }
    if (!addToClub(club, account)) {
        clubFail(ctx, me, QStringLiteral("club %1 is full or gone").arg(club));
        return;
    }
    resp.set_id(club);
    qCInfo(lcEgs).noquote() << me << "joined club" << club;
}

void clubRequestJoin(Context& ctx, const ClubIdRequest& req, ClubReply&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const auto club = db::club(req.id());
    if (account == 0 || !club) {
        clubFail(ctx, me, QStringLiteral("no club %1").arg(req.id()));
        return;
    }
    for (const db::ClubInvite& i : db::clubInvites()) {
        if (i.accountId == account && i.clubId == club->id && i.status == db::InviteRequest) {
            qCInfo(lcEgs).noquote() << me << "asked again to join club" << club->id;
            return;
        }
    }
    const qint64 id =
        db::addClubInvite(club->id, account, account, db::InviteRequest, ClubInviteTtlMs);
    MsgNotification n;
    n.set_authortype(ACTIVITY_AUTHOR_0);
    n.set_club(club->id);
    n.set_createdby(account);
    n.set_createdbyclub(-1);
    n.set_id(id);
    n.set_subjectid(club->id);
    n.set_subjecttype(ACTIVITY_SUBJECT_CLUB);
    n.set_verb(ACTIVITY_VERB_REQUESTED);
    n.set_version(1);
    for (const db::ClubMember& m : db::clubMembers(club->id)) {
        if (m.type == db::ClubOwner) {
            egsPush(m.accountId, n);
        }
    }
    qCInfo(lcEgs).noquote() << me << "asked to join club" << club->id
                            << QStringLiteral("\"%1\" (request %2)").arg(club->name).arg(id);
}

void clubLeave(Context& ctx, const ClubIdRequest&, ClubReply&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    if (const auto m = account ? db::clubMember(account) : std::nullopt) {
        removeFromClub(m->clubId, account, ACTIVITY_VERB_LEFT);
        qCInfo(lcEgs).noquote() << me << "left club" << m->clubId;
    }
}

void clubInvite(Context& ctx, const ClubAccountsRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const auto mine = account ? db::clubMember(account) : std::nullopt;
    const qint64 club = req.club().id() ? req.club().id() : (mine ? mine->clubId : 0);
    if (!mine || mine->clubId != club) {
        clubFail(ctx, me, QStringLiteral("can only invite to your own club"));
        return;
    }
    QStringList done;
    for (const IdRef& a : req.accounts()) {
        const qint64 who = a.id();
        if (who == 0 || who == account) {
            continue;
        }
        if (const auto them = db::clubMember(who); them && them->clubId == club) {
            done << egsPlayerName(who) + QStringLiteral(" (already a member)");
            continue;
        }
        bool asked = false;
        bool pending = false;
        for (const db::ClubInvite& i : db::clubInvites()) {
            asked |= i.accountId == who && i.clubId == club && i.status == db::InviteRequest;
            pending |= i.accountId == who && i.clubId == club && i.status == db::InviteSent;
        }
        if (asked) {
            done << egsPlayerName(who) + (addToClub(club, who)
                                              ? QStringLiteral(" (request accepted)")
                                              : QStringLiteral(" (club full)"));
        } else if (!pending) {
            db::addClubInvite(club, who, account, db::InviteSent, ClubInviteTtlMs);
            MsgInviteCallback cb;
            cb.set_club(club);
            cb.set_account(who);
            egsPush(who, cb);
            done << egsPlayerName(who);
        } else {
            done << egsPlayerName(who) + QStringLiteral(" (already invited)");
        }
    }
    qCInfo(lcEgs).noquote() << me << "invited to club" << club << ":"
                            << done.join(QStringLiteral(", "));
}

void clubFindInvites(Context& ctx, const FindClubInvitesRequest& req, ClubInvitesResponse& resp) {
    QSet<qint64> accounts(req.accountids().begin(), req.accountids().end());
    QSet<qint64> clubs(req.clubids().begin(), req.clubids().end());
    QSet<int> status;
    for (const int st : req.status()) {
        status.insert(st);
    }
    if (status.isEmpty()) {
        status.insert(db::InviteSent);
    }
    const int limit = req.maximumentries() > 0 ? req.maximumentries() : 20;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const db::ClubInvite& i : db::clubInvites()) {
        if (i.status != db::InviteClosed) {
            // An open invite or request for a club the player is already in is stale: close it.
            if (const auto m = db::clubMember(i.accountId); m && m->clubId == i.clubId) {
                db::closeClubInvites(0, 0, i.id);
                continue;
            }
        }
        if (!status.contains(i.status) || (!req.includeexpired() && i.expires <= now) ||
            (!accounts.isEmpty() && !accounts.contains(i.accountId)) ||
            (!clubs.isEmpty() && !clubs.contains(i.clubId)) || !db::club(i.clubId)) {
            continue;
        }
        if (resp.clubinvites_size() >= limit) {
            break;
        }
        ::ClubInvite* out = resp.add_clubinvites();
        out->set_status(static_cast<ClubValue>(i.status));
        out->set_id(i.id);
        out->set_club(i.clubId);
        out->set_account(i.accountId);
        out->set_invitedby(i.invitedBy);
        out->set_createdon(isoTime(i.created).toStdString());
        out->set_updatedon(isoTime(i.updated).toStdString());
        out->set_expireson(isoTime(i.expires).toStdString());
    }
    qCInfo(lcEgs).noquote() << "club invites for" << egsSessionName(ctx.sessionKey) << "-"
                            << qs(req.ShortDebugString()) << "->" << resp.clubinvites_size();
}

void clubReject(Context& ctx, const ClubIdRequest& req, Empty&) {
    // rejectInvite / rejectJoinRequest {id = invite id}.
    const int closed = db::closeClubInvites(0, 0, req.id());
    qCInfo(lcEgs).noquote() << egsSessionName(ctx.sessionKey) << "rejected invite/request"
                            << req.id() << (closed ? QString() : QStringLiteral("(not open)"));
}

void clubMembership(Context& ctx, const ClubMembershipRequest& req, ClubReply& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    auto club = db::club(req.id());
    if (!club || !isOwner(club->id, account)) {
        clubFail(ctx, me, QStringLiteral("only the owner can change club %1").arg(req.id()));
        return;
    }
    club->membership = int(req.membership());
    db::updateClub(*club);
    pushClubRefresh<MsgClubDetailsChange>(club->id, account);
    resp.set_id(club->id);
    resp.set_membership(req.membership());
    qCInfo(lcEgs).noquote() << me << "set club" << club->id << "membership to" << club->membership;
}

void clubCustomize(Context& ctx, const UpdateCustomizationRequest& req,
                   UpdateCustomizationResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const auto mine = account ? db::clubMember(account) : std::nullopt;
    auto club = mine ? db::club(mine->clubId) : std::nullopt;
    if (!club) {
        clubFail(ctx, me, QStringLiteral("not in a club"));
        return;
    }
    ClubCustomization cu = req.customization();
    cu.set_id(club->id);
    cu.clear_badgeboburl(); // worked out from the bob ids when asked (lookupRemoteUrl)
    cu.clear_smallbadgeboburl();
    const std::string data = cu.SerializeAsString();
    club->customization = QByteArray(data.data(), static_cast<qsizetype>(data.size()));
    db::updateClub(*club);
    pushClubRefresh<MsgClubProfileChange>(club->id, account);
    fillCustomization(resp.add_customizations(), *club, otgBaseUrl(ctx.request),
                      req.lookupremoteurl());
    qCInfo(lcEgs).noquote() << me << "customized club" << club->id << "-"
                            << qs(cu.ShortDebugString());
}

std::optional<std::pair<qint64, qint64>> ownerAndTarget(Context& ctx,
                                                        const ClubAccountsRequest& req,
                                                        const QString& me, qint64 account) {
    const qint64 club = req.club().id();
    const qint64 target = req.accounts_size() ? req.accounts(0).id() : 0;
    const auto t = target ? db::clubMember(target) : std::nullopt;
    if (!isOwner(club, account) || target == account || !t || t->clubId != club) {
        clubFail(ctx, me, QStringLiteral("not allowed (owner only, target must be a member)"));
        return std::nullopt;
    }
    return std::make_pair(club, target);
}

void getKeywords(Context& ctx, const GetKeywordsRequest& req, GetKeywordsResponse& resp) {
    QStringList asked;
    for (const std::string& g : req.groupnames()) {
        const QString group = qs(g);
        const QStringList words = config().clubKeywords.value(group);
        for (const QString& w : words) {
            resp.add_keywords()->set_name(w.toStdString());
        }
        asked << QStringLiteral("%1 (%2)").arg(group).arg(words.size());
    }
    qCInfo(lcEgs).noquote() << "club keywords for" << egsSessionName(ctx.sessionKey) << ":"
                            << asked.join(QStringLiteral(", ")) << "->" << resp.keywords_size();
}

void clubReport(Context& ctx, const ClubReportRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const auto club = db::club(req.entityid());
    db::ClubReport r;
    r.clubId = req.entityid();
    r.clubName = club ? club->name : QString();
    r.reporter = account ? account : req.reporteraccountid();
    r.language = req.reportlanguageid();
    r.reason = req.reportreasonid();
    const qint64 id = db::addClubReport(r);
    qCWarning(lcEgs).noquote()
        << "club report" << id << ":" << (me.isEmpty() ? egsPlayerName(r.reporter) : me)
        << "reported club" << r.clubId
        << (club ? QStringLiteral("\"%1\"").arg(club->name) : QStringLiteral("(no such club)"))
        << QStringLiteral("reason %1, language %2").arg(r.reason).arg(r.language)
        << (req.reporteraccountid() && req.reporteraccountid() != r.reporter
                ? QStringLiteral("(request said reporter %1)").arg(req.reporteraccountid())
                : QString());
}

void clubKick(Context& ctx, const ClubAccountsRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const auto ct = ownerAndTarget(ctx, req, me, account);
    if (!ct) {
        return;
    }
    removeFromClub(ct->first, ct->second, ACTIVITY_VERB_KICKED);
    qCInfo(lcEgs).noquote() << me << "kicked" << egsPlayerName(ct->second) << "from club"
                            << ct->first;
}

void clubChangeAdmin(Context& ctx, const ClubAccountsRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    const auto ct = ownerAndTarget(ctx, req, me, account);
    if (!ct) {
        return;
    }
    for (db::ClubMember m : db::clubMembers(ct->first)) {
        if (m.accountId == ct->second || m.accountId == account) {
            m.type = m.accountId == ct->second ? db::ClubOwner : db::ClubMemberType;
            db::putClubMember(m);
        }
    }
    pushClubRefresh<MsgClubDetailsChange>(ct->first, account);
    qCInfo(lcEgs).noquote() << me << "made" << egsPlayerName(ct->second) << "owner of club"
                            << ct->first;
}

// ---- profileService: fame
// -------------------------------------------------------------------------

qint64 sessionAccount(const Context& ctx, QString* name) {
    QMutexLocker lock(&sessions().mutex);
    const Session* s = sessionOf(ctx);
    if (name && s) {
        *name = s->name;
    }
    return s ? s->accountId : 0;
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
    // Club fame: the sum of the members' clubFameDelta (0 without a club).
    const auto member = account ? db::clubMember(account) : std::nullopt;
    const auto club = member ? db::club(member->clubId) : std::nullopt;
    resp.mutable_clubprogression()->set_xp(club ? club->xp : 0);
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
    qint64 clubXp = 0;
    QString clubNote;
    if (const auto member = account ? db::clubMember(account) : std::nullopt) {
        if (req.clubfamedelta() > 0) {
            clubXp = db::addClubXp(member->clubId, req.clubfamedelta());
            putClubFameScore(member->clubId, clubXp);
            pushClubRefresh<MsgClubProgressionChange>(member->clubId, account);
            clubNote = QStringLiteral("club %1 +%2 = %3")
                           .arg(member->clubId)
                           .arg(req.clubfamedelta())
                           .arg(clubXp);
        } else if (const auto club = db::club(member->clubId)) {
            clubXp = club->xp;
        }
    } else if (req.clubfamedelta() > 0) {
        clubNote =
            QStringLiteral("(club delta %1 ignored: not in a club)").arg(req.clubfamedelta());
    }
    resp.mutable_clubprogression()->mutable_after()->set_xp(clubXp);
    qCInfo(lcEgs).noquote() << "fame update from" << (name.isEmpty() ? QStringLiteral("?") : name)
                            << "total" << sent << "-> stored" << f.xp << clubNote << "transaction"
                            << qs(req.transactionid());
}

// ---- objectiveService: accolades
// -----------------------------------------------------------------

void fillAccolades(AccoladeProgressResponse& resp, qint64 account, const QString& only) {
    for (const db::Accolade& a : db::accolades(account)) {
        if (!only.isEmpty() && a.name != only) {
            continue;
        }
        AccoladeProgress* p = resp.add_progresslist();
        p->set_accoladename(a.name.toStdString());
        p->set_type(ACCOLADE_TYPE_6);
        p->set_progress(a.progress);
    }
}

void playerAccolades(Context& ctx, const GetAccoladeProgressRequest& req,
                     AccoladeProgressResponse& resp) {
    QString name;
    const qint64 session = sessionAccount(ctx, &name);
    const qint64 account = req.currentaccount() || !req.has_accountid() ? session : req.accountid();
    const QString only = qs(req.accoladename());
    if (account != 0) {
        fillAccolades(resp, account, only);
    }
    qCInfo(lcEgs).noquote() << "accolades of" << egsPlayerName(account) << "for"
                            << (name.isEmpty() ? QStringLiteral("?") : name)
                            << (only.isEmpty() ? QString() : QStringLiteral("(%1)").arg(only))
                            << (req.has_accoladeid()
                                    ? QStringLiteral("(id %1)").arg(req.accoladeid())
                                    : QString())
                            << "->" << resp.progresslist_size()
                            << (resp.progresslist_size() == 1 ? "entry" : "entries");
}

void clubAccolades(Context& ctx, const GetClubAccoladeProgressRequest& req,
                   AccoladeProgressResponse& resp) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    qint64 club = req.clubid();
    if (req.currentclub() || club == 0) {
        const auto m = account ? db::clubMember(account) : std::nullopt;
        club = m ? m->clubId : 0;
    }
    const QString only = qs(req.accoladename());
    for (const db::Accolade& a : club ? db::clubAccolades(club) : QList<db::Accolade>()) {
        if (!only.isEmpty() && a.name != only) {
            continue;
        }
        AccoladeProgress* p = resp.add_progresslist();
        p->set_accoladename(a.name.toStdString());
        p->set_type(ACCOLADE_TYPE_2);
        p->set_progress(a.progress);
    }
    qCInfo(lcEgs).noquote() << "club accolades of"
                            << (club ? QStringLiteral("club %1").arg(club)
                                     : QStringLiteral("no club"))
                            << "for" << (me.isEmpty() ? QStringLiteral("?") : me) << "->"
                            << resp.progresslist_size()
                            << (resp.progresslist_size() == 1 ? "entry" : "entries");
}

void updateAccolades(Context& ctx, const UpdateAccoladeProgressRequest& req,
                     AccoladeProgressResponse& resp) {
    QString name;
    const qint64 account = sessionAccount(ctx, &name);
    static QMutex mutex;
    static QSet<QString> applied;
    QStringList changes;
    QStringList skipped;
    for (const AccoladeProgressUpdate& u : req.updates()) {
        const QString accolade = qs(u.accoladename());
        if (accolade.isEmpty()) {
            continue;
        }
        if (u.currentclub()) {
            // A contribution to the player's club (second update list, 0x5B3FA0), tracked apart
            // from the player's own progress (which comes with currentClub false): added to the
            // club's totals and answered as type 2.
            const auto member = account ? db::clubMember(account) : std::nullopt;
            if (!member) {
                skipped << QStringLiteral("%1 %2%3 (club contribution, not in a club)")
                               .arg(accolade, u.progress() >= 0 ? "+" : "")
                               .arg(u.progress());
                continue;
            }
            const QString clubKey =
                QStringLiteral("%1|%2|club|%3")
                    .arg(QString::fromLatin1(ctx.sessionKey), qs(req.transactionid()), accolade);
            bool fresh = true;
            {
                QMutexLocker l(&mutex);
                fresh = req.transactionid().empty() || !applied.contains(clubKey);
                applied.insert(clubKey);
            }
            qint64 total = 0;
            if (fresh) {
                total = db::addClubAccoladeProgress(member->clubId, accolade, u.progress());
                changes << QStringLiteral("%1 %2%3 = %4 (club %5)")
                               .arg(accolade, u.progress() >= 0 ? "+" : "")
                               .arg(u.progress())
                               .arg(total)
                               .arg(member->clubId);
            } else {
                for (const db::Accolade& a : db::clubAccolades(member->clubId)) {
                    if (a.name == accolade) {
                        total = a.progress;
                    }
                }
                skipped << QStringLiteral("%1 (club, already applied)").arg(accolade);
            }
            AccoladeProgress* p = resp.add_progresslist();
            p->set_accoladename(accolade.toStdString());
            p->set_type(ACCOLADE_TYPE_2);
            p->set_progress(total);
            continue;
        }
        if (account == 0) {
            skipped << accolade;
            continue;
        }
        const QString key =
            QStringLiteral("%1|%2|%3")
                .arg(QString::fromLatin1(ctx.sessionKey), qs(req.transactionid()), accolade);
        bool fresh = true;
        {
            QMutexLocker l(&mutex);
            if (applied.size() > 20000) {
                applied.clear();
            }
            fresh = req.transactionid().empty() || !applied.contains(key);
            applied.insert(key);
        }
        qint64 total = 0;
        if (fresh) {
            total = db::addAccoladeProgress(account, accolade, u.progress());
            changes << QStringLiteral("%1 %2%3 = %4")
                           .arg(accolade, u.progress() >= 0 ? "+" : "")
                           .arg(u.progress())
                           .arg(total);
        } else {
            for (const db::Accolade& a : db::accolades(account)) {
                if (a.name == accolade) {
                    total = a.progress;
                }
            }
            skipped << QStringLiteral("%1 (already applied)").arg(accolade);
        }
        AccoladeProgress* p = resp.add_progresslist();
        p->set_accoladename(accolade.toStdString());
        p->set_type(ACCOLADE_TYPE_6);
        p->set_progress(total);
    }
    qCInfo(lcEgs).noquote() << "accolade update from"
                            << (name.isEmpty() ? QStringLiteral("?") : name) << "transaction"
                            << qs(req.transactionid()) << ":"
                            << (changes.isEmpty() ? QStringLiteral("nothing stored")
                                                  : changes.join(", "))
                            << (skipped.isEmpty()
                                    ? QString()
                                    : QStringLiteral("| not stored: %1").arg(skipped.join(", ")));
}

// ---- storageService: ghosts
// ----------------------------------------------------------------------

QString bobChecksum(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toBase64());
}

void createBob(Context& ctx, const CreateBobRequest& req, CreateBobResponse& resp) {
    QString name;
    const qint64 account = sessionAccount(ctx, &name);
    const QByteArray data(req.body().data(), static_cast<qsizetype>(req.body().size()));
    if (account == 0) {
        ctx.status = 500;
        qCWarning(lcEgs).noquote()
            << "ghost upload without a session refused (" << data.size() << "bytes)";
        return;
    }
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
    qint64 id = 0;
    bool reused = false;
    if (!data.isEmpty()) {
        if (const auto same = db::findGhost(account, g.mimeType, actual, data.size())) {
            QByteArray stored;
            if (db::ghost(*same, &stored) && stored == data) {
                id = *same;
                reused = true;
                qCInfo(lcEgs).noquote() << "ghost from" << name << "is identical to ghost" << id
                                        << ", reusing it (" << data.size() << "bytes)";
            }
        }
        if (id == 0) {
            id = db::putGhost(g, data);
        }
    }
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
    if (!reused) {
        qCInfo(lcEgs).noquote() << "ghost" << id << "uploaded by" << name << "," << data.size()
                                << "bytes, type" << req.objecttype() << "access"
                                << req.accesstype();
    }
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

// ---- statsService.getAccountStats
// -----------------------------------------------------------------

const QString StatsResource = QStringLiteral("stats/V06/player/%1");

struct StatColumn {
    const char* name;
    const char* subject; // fixed subject (Overdrive, FaceOff), else empty: the row's subject
    const char* counter;
};

// Per-subject sets: the category their subjects come from; all have the same six columns.
const QList<QPair<QString, QString>>& rowSets() {
    static const QList<QPair<QString, QString>> sets = {
        {QStringLiteral("Tracks"), QStringLiteral("track")},
        {QStringLiteral("Vehicles"), QStringLiteral("vehicle")},
        {QStringLiteral("GameModes"), QStringLiteral("event")},
        {QStringLiteral("Challenges"), QStringLiteral("solochallenge")}};
    return sets;
}
const StatColumn RowColumns[] = {
    {"Attempts", "", "attempts"}, {"TotalDistanceDriven", "", "distance"},
    {"TotalFame", "", "fame"},    {"Wins", "", "wins"},
    {"Created", "", "created"},   {"Forwarded", "", "forwarded"}, // no counter seen: 0
};
const StatColumn OverdriveColumns[] = {
    {"PositionGainedTotalFame", "UniquePosition", "fame"},
    {"PositionGainedAttempts", "UniquePosition", "attempts"},
    {"SectorAccuracyTotalFame", "Sector Accuracy", "fame"},
    {"SectorAccuracyAttempts", "Sector Accuracy", "attempts"},
    {"CleanLapTotalFame", "CleanLap", "fame"},
    {"CleanLapAttempts", "CleanLap", "attempts"},
    {"CleanRaceTotalFame", "CleanRace", "fame"},
    {"CleanRaceAttempts", "CleanRace", "attempts"},
    {"CleanSectorTotalFame", "CleanSector", "fame"},
    {"CleanSectorAttempts", "CleanSector", "attempts"},
    {"CrazySpeedTotalFame", "Crazy Speed", "fame"},
    {"CrazySpeedAttempts", "Crazy Speed", "attempts"},
    {"DraftTotalFame", "Drafting", "fame"},
    {"DraftAttempts", "Drafting", "attempts"},
    {"DriftOverdriveTotalFame", "Drift", "fame"},
    {"DriftOverdriveAttempts", "Drift", "attempts"},
    {"VehicleCollisionTotalFame", "VehicleCollision", "fame"},
    {"VehicleCollisionAttempts", "VehicleCollision", "attempts"},
    {"EnvironmentCollisionTotalFame", "EnvironmentCollision", "fame"},
    {"EnvironmentCollisionAttempts", "EnvironmentCollision", "attempts"},
    {"FinishPositionTotalFame", "FinishPosition", "fame"},
    {"FinishPositionAttempts", "FinishPosition", "attempts"},
    {"OffTrackTotalFame", "OffTrack", "fame"},
    {"OffTrackAttempts", "OffTrack", "attempts"},
};
// Max Speed Face-Off has no fame, attempts or wins columns in the game.
const StatColumn FaceOffColumns[] = {
    {"AverageSpeedTotalFame", "Average Speed Face-Off", "fame"},
    {"AverageSpeedAttempts", "Average Speed Face-Off", "attempts"},
    {"AverageSpeedWins", "Average Speed Face-Off", "wins"},
    {"CorneringTotalFame", "Cornering Face-Off", "fame"},
    {"CorneringAttempts", "Cornering Face-Off", "attempts"},
    {"CorneringWins", "Cornering Face-Off", "wins"},
    {"DriftTotalFame", "Drift Face-Off", "fame"},
    {"DriftAttempts", "Drift Face-Off", "attempts"},
    {"DriftWins", "Drift Face-Off", "wins"},
};

qint64 statValue(const QJsonObject& counts, const QString& subject, const QString& category,
                 const QString& counter) {
    const QString key = subject + QLatin1Char('/') + category + QLatin1Char('/') + counter;
    return counts.value(key).toInteger() - counts.value(key + QStringLiteral("/-")).toInteger();
}

QStringList statSets() {
    QStringList sets;
    for (const auto& s : rowSets()) {
        sets << s.first;
    }
    return sets << QStringLiteral("Overdrive") << QStringLiteral("FaceOff");
}

EgsStatsTable statsFromCounts(const QJsonObject& counts, bool stored, const QString& set);

EgsStatsTable statsTable(qint64 accountId, const QString& set) {
    const auto g = db::gameState(StatsResource.arg(accountId));
    return statsFromCounts(g ? g->counts : QJsonObject(), bool(g), set);
}

EgsStatsTable clubStatsTable(qint64 clubId, const QString& set, int* membersWithStats = nullptr) {
    if (const auto own = db::gameState(QStringLiteral("stats/V06/club/%1").arg(clubId))) {
        return statsFromCounts(own->counts, true, set);
    }
    QHash<QString, qint64> sums;
    int withStats = 0;
    for (const db::ClubMember& m : db::clubMembers(clubId)) {
        const auto g = db::gameState(StatsResource.arg(m.accountId));
        if (!g) {
            continue;
        }
        ++withStats;
        for (auto it = g->counts.constBegin(); it != g->counts.constEnd(); ++it) {
            sums[it.key()] += it.value().toInteger();
        }
    }
    QJsonObject counts;
    for (auto it = sums.cbegin(); it != sums.cend(); ++it) {
        counts.insert(it.key(), it.value());
    }
    if (membersWithStats) {
        *membersWithStats = withStats;
    }
    return statsFromCounts(counts, withStats > 0, set);
}

EgsStatsTable statsFromCounts(const QJsonObject& counts, bool stored, const QString& set) {
    EgsStatsTable t;
    t.stored = stored;
    const auto fixed = [&](const StatColumn* first, const StatColumn* last,
                           const QString& category) {
        QList<qint64> values;
        for (const StatColumn* c = first; c != last; ++c) {
            t.columns << QString::fromLatin1(c->name);
            values << statValue(counts, QString::fromLatin1(c->subject), category,
                                QString::fromLatin1(c->counter));
        }
        t.rows.append({set, values});
        t.known = true;
    };
    if (set == QLatin1String("Overdrive")) {
        fixed(std::begin(OverdriveColumns), std::end(OverdriveColumns),
              QStringLiteral("overdrive"));
        return t;
    }
    if (set == QLatin1String("FaceOff")) {
        fixed(std::begin(FaceOffColumns), std::end(FaceOffColumns), QStringLiteral("faceoff"));
        return t;
    }
    for (const auto& [name, category] : rowSets()) {
        if (name != set) {
            continue;
        }
        t.known = true;
        for (const StatColumn& c : RowColumns) {
            t.columns << QString::fromLatin1(c.name);
        }
        const QString marker = QLatin1Char('/') + category + QLatin1Char('/');
        QStringList subjects;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            const qsizetype at = it.key().indexOf(marker);
            if (at > 0 && !subjects.contains(it.key().left(at))) {
                subjects << it.key().left(at);
            }
        }
        for (const QString& subject : subjects) {
            QList<qint64> values;
            for (const StatColumn& c : RowColumns) {
                values << statValue(counts, subject, category, QString::fromLatin1(c.counter));
            }
            t.rows.append({subject, values});
        }
    }
    return t;
}

// The answer both stats calls share (parser 0x642310).
void fillStats(const EgsStatsTable& t, GetAccountStatsResponse& resp) {
    for (const QString& c : t.columns) {
        resp.add_columnnames(c.toStdString());
    }
    for (const auto& [name, values] : t.rows) {
        AccountStat* row = resp.add_stats();
        row->set_total(1);
        row->mutable_definition()->set_name(name.toStdString());
        StatEntry* entry = row->add_entries();
        for (const qint64 v : values) {
            entry->add_columns(v);
        }
    }
}

void accountStats(Context& ctx, const GetAccountStatsRequest& req, GetAccountStatsResponse& resp) {
    const QString set = qs(req.columndefinitionsetname());
    qint64 club = req.clubs_size() > 0 ? req.clubs(0).id() : 0;
    if (club == 0 && req.currentclub()) {
        const auto m = db::clubMember(sessionAccount(ctx));
        club = m ? m->clubId : 0;
    }
    const qint64 account = req.accounts_size() > 0 ? req.accounts(0).id() : sessionAccount(ctx);
    const EgsStatsTable t = club ? clubStatsTable(club, set) : statsTable(account, set);
    if (!t.known) {
        qCWarning(lcEgs).noquote() << "stats: unknown column set" << set;
    }
    fillStats(t, resp);
    qCInfo(lcEgs).noquote() << "stats" << set << "for"
                            << (club ? QStringLiteral("club %1").arg(club) : egsPlayerName(account))
                            << "->" << resp.stats_size()
                            << (resp.stats_size() == 1 ? "row" : "rows")
                            << (!t.stored ? QStringLiteral("(no stats stored yet)") : QString());
}

void clubStats(Context& ctx, const GetClubStatsRequest& req, GetAccountStatsResponse& resp) {
    const QString set = qs(req.columndefinitionsetname());
    qint64 club = req.clubs_size() > 0 ? req.clubs(0) : 0;
    if (club == 0 && req.currentclub()) {
        const auto m = db::clubMember(sessionAccount(ctx));
        club = m ? m->clubId : 0;
    }
    int members = 0;
    const EgsStatsTable t = clubStatsTable(club, set, &members);
    if (!t.known) {
        qCWarning(lcEgs).noquote() << "club stats: unknown column set" << set;
    }
    fillStats(t, resp);
    const auto c = club ? db::club(club) : std::nullopt;
    qCInfo(lcEgs).noquote() << "club stats" << set << "for club" << club
                            << (c ? QStringLiteral("\"%1\"").arg(c->name)
                                  : QStringLiteral("(unknown)"))
                            << "->" << resp.stats_size()
                            << (resp.stats_size() == 1 ? "row" : "rows")
                            << QStringLiteral("(sum of %1 member(s) with stats)").arg(members);
}

Endpoint withGroup(Endpoint e, Endpoint::Discovery group) {
    e.discovery = group;
    return e;
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
        endpoint<FindGlobalObjectivesRequest, FindGlobalObjectivesResponse>(
            "objectiveService", "findGlobalObjectives", false, true,
            [](Context& ctx, const FindGlobalObjectivesRequest& req,
               FindGlobalObjectivesResponse&) {
                qCInfo(lcEgs).noquote() << "global objectives for" << egsSessionName(ctx.sessionKey)
                                        << "type" << int(req.type()) << "-> none";
            }),
        endpoint<GetAccoladeProgressRequest, AccoladeProgressResponse>(
            "objectiveService", "getPlayerAccoladeProgress", false, true, playerAccolades),
        endpoint<GetClubAccoladeProgressRequest, AccoladeProgressResponse>(
            "objectiveService", "getClubAccoladeProgress", false, true, clubAccolades),
        endpoint<UpdateAccoladeProgressRequest, AccoladeProgressResponse>(
            "objectiveService", "updateAccoladeProgressInTransaction", false, true,
            updateAccolades),
        endpoint<CreateBobRequest, CreateBobResponse>("storageService", "createBob", false, true,
                                                      createBob),
        endpoint<GetRemoteUrlRequest, GetRemoteUrlResponse>("storageService", "getRemoteUrl", false,
                                                            true, getRemoteUrl),
        endpoint<GetClubStatsRequest, GetAccountStatsResponse>("statsService", "getClubStats",
                                                               false, true, clubStats),
        endpoint<GetAccountStatsRequest, GetAccountStatsResponse>("statsService", "getAccountStats",
                                                                  false, true, accountStats),
        // Clubs.
        endpoint<ClubCreateRequest, ClubReply>("clubService", "createAndJoin", false, true,
                                               clubCreate),
        endpoint<ClubNameMessage, ClubNameMessage>("clubService", "sanitize", false, true,
                                                   clubSanitize),
        endpoint<QueryClubsRequest, QueryClubsResponse>("clubService", "queryClubs", false, true,
                                                        queryClubs),
        endpoint<ClubQuickSearchRequest, ClubListResponse>("clubService", "quickSearch", false,
                                                           true, clubQuickSearch),
        endpoint<ClubIdRequest, ClubReply>("clubService", "join", false, true, clubJoin),
        endpoint<ClubIdRequest, ClubReply>("clubService", "requestJoin", false, true,
                                           clubRequestJoin),
        endpoint<ClubIdRequest, ClubReply>("clubService", "leave", false, true, clubLeave),
        endpoint<ClubAccountsRequest, Empty>("clubService", "invite", false, true, clubInvite),
        endpoint<FindClubInvitesRequest, ClubInvitesResponse>("clubService", "findInvites", false,
                                                              true, clubFindInvites),
        endpoint<ClubIdRequest, Empty>("clubService", "rejectInvite", false, true, clubReject),
        endpoint<ClubIdRequest, Empty>("clubService", "rejectJoinRequest", false, true, clubReject),
        endpoint<ClubMembershipRequest, ClubReply>("clubService", "updateMembership", false, true,
                                                   clubMembership),
        endpoint<UpdateCustomizationRequest, UpdateCustomizationResponse>(
            "clubService", "updateCustomization", false, true, clubCustomize),
        endpoint<GetKeywordsRequest, GetKeywordsResponse>("keywordService", "getKeywords", false,
                                                          true, getKeywords),
        endpoint<ClubReportRequest, Empty>("moderationService", "submitClubNameReport", false, true,
                                           clubReport),
        endpoint<ClubAccountsRequest, Empty>("clubService", "kick", false, true, clubKick),
        endpoint<ClubAccountsRequest, Empty>("clubService", "changeAdmin", false, true,
                                             clubChangeAdmin),
        // Real friend lists (config.json egs.friend_lists, on by default).
        withGroup(endpoint<FriendsChangeRequest, GetFriendsResponse>(
                      "friendsListService", "addFriends", false, true, addFriends),
                  Endpoint::Discovery::FriendLists),
        withGroup(endpoint<FriendsChangeRequest, GetFriendsResponse>(
                      "friendsListService", "removeFriends", false, true, removeFriends),
                  Endpoint::Discovery::FriendLists),
        endpoint<AccountQuickSearchRequest, AccountListResponse>("accountService", "quickSearch",
                                                                 false, true, quickSearch),
        endpoint<QueryStatusRequest, QueryStatusResponse>("sessionService", "queryStatus", false,
                                                          true, queryStatus),
        endpoint<LobbyWithMemberRequest, LobbiesResponse>("lobbyService", "createLobby", false,
                                                          true, createLobby),
        endpoint<LobbyWithMemberRequest, LobbiesResponse>("lobbyService", "getLobby", false, true,
                                                          getLobby),
        endpoint<LobbyJoinWithAccountRequest, Empty>("lobbyService", "joinWithAccount", false, true,
                                                     joinWithAccount),
        endpoint<LobbyWithMemberRequest, Empty>("lobbyService", "updateLobby", false, true,
                                                updateLobby),
        endpoint<LobbyWithMemberRequest, Empty>("lobbyService", "updateMember", false, true,
                                                updateMember),
        endpoint<LobbyLeaveCurrentRequest, LobbyLeaveCurrentResponse>(
            "lobbyService", "leaveCurrentLobby", false, true, leaveCurrentLobby),
        endpoint<LobbyLeaveByIdRequest, LobbyLeaveByIdResponse>("lobbyService", "leaveLobbyById",
                                                                false, true, leaveLobbyById),
        endpoint<LobbyRemoveRequest, Empty>("lobbyService", "removeFromLobby", false, true,
                                            removeFromLobby),
        endpoint<LobbyChangeHostRequest, Empty>("lobbyService", "changeHost", false, true,
                                                changeHost),
    };
    return list;
}

// Whether a discovery-mode endpoint's group is switched on (always true for normal endpoints).
bool discoveryOn(const Endpoint& e) {
    switch (e.discovery) {
    case Endpoint::Discovery::FriendLists:
        return config().friendLists;
    case Endpoint::Discovery::None:
        break;
    }
    return true;
}

const Endpoint* findEndpoint(const QString& service, const QString& method) {
    for (const Endpoint& e : endpoints()) {
        if (!discoveryOn(e)) {
            continue;
        }
        if (e.service == service && e.method == method) {
            return &e;
        }
    }
    return nullptr;
}

std::string endpointList() {
    SvcEndpointDescriptorList list;
    for (const Endpoint& e : endpoints()) {
        if (!e.listed || !discoveryOn(e)) {
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

// "egs/binary/<service>/<method>" or "/egs/binary/..." -> {service, method}
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
        // Built from the compiled egs.proto, reduced to protobuf 2.4.1 descriptor fields.
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
    // Large bytes fields (a ghost in createBob is up to ~150 KB) are shown by size only.
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
        HttpResponse r;
        r.contentType = "text/xml";
        r.body = "<msg t=\"Empty\" s=\"0\"></msg>";
        r.keepalive = true; // counted in the trace, not logged one by one
        {
            QMutexLocker lock(&sessions().mutex);
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            Session* match = nullptr;
            for (Session& s : sessions().byKey) {
                if (!s.peer.isNull() && s.peer.isEqual(request.peer) &&
                    (!match || s.lastSeen > match->lastSeen)) {
                    match = &s;
                }
            }
            if (match) {
                match->lastPoll = now;
            }
            presenceCheckLocked(now);
            if (match && !match->outbox.isEmpty()) {
                const auto [type, data] = match->outbox.takeFirst();
                const QByteArray b64 =
                    QByteArray(data.data(), static_cast<qsizetype>(data.size())).toBase64();
                r.body = "<msg t=\"" + type + "\" s=\"" + QByteArray::number(b64.size()) + "\">" +
                         b64 + "</msg>";
                r.keepalive = false; // a real message: traced like any request
                qCInfo(lcEgs).noquote() << "pushed" << type << "to" << match->name;
            }
        }
        respond(r);
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
    if (!sessionKey.isEmpty()) {
        QMutexLocker lock(&sessions().mutex);
        const auto it = sessions().byKey.find(sessionKey);
        if (it != sessions().byKey.end()) {
            it->peer = request.peer;
        }
    }

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
        if (!ctx.serviceError.isEmpty()) {
            r.headers.append(qMakePair(QByteArray("X-Service-Error"), ctx.serviceError));
        }
        if (ctx.serviceErrorCode != 0) {
            r.headers.append(qMakePair(QByteArray("X-Service-ErrorCode"),
                                       QByteArray::number(ctx.serviceErrorCode)));
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
            result.set_retryhint(RETRY_HINT_1); // enum, see egs.proto
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
    for (const QString& name : db::allFriendNames()) {
        if (accountIdFor(name) == accountId) {
            return name;
        }
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
    qCInfo(lcEgs).noquote() << "fame of" << egsPlayerName(accountId) << "set to" << f.xp
                            << "(admin)";
}

void egsSetStarsElite(qint64 accountId, std::optional<qint64> stars, std::optional<qint64> elite) {
    db::Fame f = db::fame(accountId).value_or(db::Fame{accountId, 0, {}, {}, 0});
    f.accountId = accountId;
    if (stars) {
        f.stars = std::max<qint64>(0, *stars);
    }
    if (elite) {
        f.elite = std::max<qint64>(0, *elite);
    }
    f.updated = QDateTime::currentMSecsSinceEpoch();
    db::putFame(f);
    qCInfo(lcEgs).noquote() << "stars/elite of" << egsPlayerName(accountId) << "set to"
                            << (f.stars ? QString::number(*f.stars) : QStringLiteral("-")) << "/"
                            << (f.elite ? QString::number(*f.elite) : QStringLiteral("-"))
                            << "(admin)";
}

QString egsSetClubFame(qint64 clubId, qint64 xp) {
    auto c = db::club(clubId);
    if (!c) {
        return QStringLiteral("no such club");
    }
    c->xp = std::max<qint64>(0, xp);
    if (!db::updateClub(*c)) {
        return QStringLiteral("could not save the club");
    }
    putClubFameScore(clubId, c->xp);
    pushClubRefresh<MsgClubProgressionChange>(clubId, 0);
    qCInfo(lcEgs).noquote() << "fame of club" << clubId << c->name << "set to" << c->xp
                            << "(admin)";
    return {};
}

QString egsRenameClub(qint64 clubId, const QString& name) {
    auto c = db::club(clubId);
    const QString wanted = name.trimmed();
    if (!c) {
        return QStringLiteral("no such club");
    }
    if (wanted.isEmpty()) {
        return QStringLiteral("the name is empty");
    }
    if (const auto other = db::clubByName(wanted); other && other->id != clubId) {
        return QStringLiteral("club %1 already has that name").arg(other->id);
    }
    const QString old = c->name;
    c->name = wanted;
    if (!db::updateClub(*c)) {
        return QStringLiteral("could not save the club");
    }
    pushClubRefresh<MsgClubDetailsChange>(clubId, 0);
    qCInfo(lcEgs).noquote() << "club" << clubId << "renamed from" << old << "to" << wanted
                            << "(admin)";
    return {};
}

QString egsRemoveClubMember(qint64 clubId, qint64 accountId) {
    const auto m = db::clubMember(accountId);
    if (!db::club(clubId) || !m || m->clubId != clubId) {
        return QStringLiteral("not a member of that club");
    }
    removeFromClub(clubId, accountId, ACTIVITY_VERB_KICKED);
    qCInfo(lcEgs).noquote() << egsPlayerName(accountId) << "removed from club" << clubId
                            << "(admin)";
    return {};
}

QString egsKickFromParty(const QString& partyId, qint64 accountId) {
    {
        QMutexLocker lock(&sessions().mutex);
        const auto it = parties().byId.constFind(partyId);
        if (it == parties().byId.cend() || !partyAccounts(*it).contains(accountId)) {
            return QStringLiteral("not a member of that party");
        }
        leavePartyLocked(accountId, it->type, LobbyKicked, {accountId});
    }
    qCInfo(lcEgs).noquote() << egsPlayerName(accountId) << "removed from party" << partyId
                            << "(admin)";
    return {};
}

QString egsDisbandParty(const QString& partyId) {
    {
        QMutexLocker lock(&sessions().mutex);
        const auto it = parties().byId.constFind(partyId);
        if (it == parties().byId.cend()) {
            return QStringLiteral("no such party");
        }
        Party empty = *it;
        const QList<qint64> members = partyAccounts(empty);
        empty.members.clear();
        pushLobbyLocked(empty, LobbyKicked, members, members);
        for (const qint64 a : members) {
            parties().ofAccount.remove(qMakePair(a, empty.type));
        }
        parties().byId.remove(partyId);
    }
    qCInfo(lcEgs).noquote() << "party" << partyId << "disbanded (admin)";
    return {};
}

QSet<qint64> egsOnlineAccounts() {
    QMutexLocker lock(&sessions().mutex);
    return onlineAccountsLocked(QDateTime::currentMSecsSinceEpoch());
}

QHash<qint64, qint64> egsLastPolls() {
    QHash<qint64, qint64> out;
    QMutexLocker lock(&sessions().mutex);
    for (const Session& s : std::as_const(sessions().byKey)) {
        if (s.accountId != 0 && s.lastPoll > out.value(s.accountId)) {
            out.insert(s.accountId, s.lastPoll);
        }
    }
    return out;
}

QList<EgsPartyInfo> egsParties() {
    QList<EgsPartyInfo> out;
    QMutexLocker lock(&sessions().mutex);
    for (const Party& p : std::as_const(parties().byId)) {
        EgsPartyInfo info{p.id, p.type, p.status, {}};
        for (const PartyMember& m : p.members) {
            info.members.append({m.accountId, m.name, m.host});
        }
        out.append(info);
    }
    return out;
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

QStringList egsStatSets() {
    return statSets();
}

EgsStatsTable egsStats(qint64 accountId, const QString& set) {
    return statsTable(accountId, set);
}

EgsStatsTable egsClubStats(qint64 clubId, const QString& set) {
    return clubStatsTable(clubId, set);
}
