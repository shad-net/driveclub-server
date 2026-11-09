// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: lobbyService: parties (lobbyType 1).

#include "egs/internal.h"

namespace egs {

void fillLobby(::Lobby* out, const Party& p, qint64 viewer) {
    out->set_id(p.id.toStdString());
    out->set_lobbytype(static_cast<LobbyValue>(p.type));
    out->set_status(static_cast<LobbyValue>(p.status));
    if (p.matchSlotId != 0) {
        out->set_matchslotid(p.matchSlotId);
    }
    if (p.eventListId != 0) {
        out->set_eventlistid(p.eventListId);
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
        if (p.type == 0) {
            lm->set_id(std::to_string(m.accountId));
            lm->set_playernumber(m.playerNumber);
            const bool own = viewer != 0 && m.accountId == viewer && !m.ownMemberData.empty();
            lm->set_memberdata(own ? m.ownMemberData : m.memberData);
            lm->set_clientversion(m.clientVersion.toStdString());
            lm->set_aicontrolled(false);
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
    std::string data = m.SerializeAsString();
    std::sort(to.begin(), to.end());
    to.erase(std::unique(to.begin(), to.end()), to.end());
    for (const qint64 a : to) {
        if (a == 0) {
            continue;
        }
        if (p.type == 0) { // race lobby: each member gets its own entry as its game sent it
            m.mutable_lobby()->Clear();
            fillLobby(m.mutable_lobby(), p, a);
            data = m.SerializeAsString();
        }
        pushLocked(a, QByteArrayLiteral("LobbyCallbackMessage"), data);
    }
}

QList<qint64> partyAccounts(const Party& p) {
    QList<qint64> out;
    for (const PartyMember& m : p.members) {
        out.append(m.accountId);
    }
    return out;
}

QString leavePartyLocked(qint64 account, int type, int action, const QList<qint64>& alsoTell) {
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
    if (p.type == 0) {
        return id;
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
        fillLobby(resp.add_lobbies(), *it, account);
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

} // namespace egs
