// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: sessionService (login), profileService profiles, friends, online status queries,
// accountService.

#include "egs/internal.h"

namespace egs {

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
    s.sessionData = req.clientsessiondata();
    s.psPlus = req.psplus();
    s.created = QDateTime::currentMSecsSinceEpoch();
    s.lastSeen = s.created;
    s.peer = ctx.request.peer;
    {
        QMutexLocker lock(&sessions().mutex);
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
                    s.created, s.lastSeen, s.requests,
                    QByteArray(s.sessionData.data(), qsizetype(s.sessionData.size()))});

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

void updateStreamingStatus(Context& ctx, const UpdateStreamingStatusRequest& req, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    qCInfo(lcEgs).noquote() << lobbyWho(account, me)
                            << (req.streaming() ? "started streaming" : "stopped streaming")
                            << QString::fromStdString(req.streamingurl());
}

void clearMyAuthToken(Context& ctx, const Empty&, Empty&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "clearMyAuthToken (nothing kept)";
}

void getGames(Context& ctx, const GetGamesRequest& req, GetGamesResponse&) {
    QString me;
    const qint64 account = sessionAccount(ctx, &me);
    qCInfo(lcEgs).noquote() << lobbyWho(account, me) << "getGames from" << req.range().start()
                            << "(" << req.eventlisttypes_size() << "event list types): 0 games";
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

} // namespace egs
