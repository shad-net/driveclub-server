// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: Config, sessions, online status, players, the endpoint table and request dispatch.

#include "egs/internal.h"

namespace egs {

Q_LOGGING_CATEGORY(lcEgs, "egs")

EgsConfig& config() {
    static EgsConfig c;
    return c;
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
            // A player who stopped polling cannot take part in a party any more.
            leaveAllPartiesLocked(a);
        }
    }
    p.online = online;
}

// ---- Players (account id -> name) -------------------------------------------------------------

void rememberPlayer(qint64 accountId, const QString& name, bool loggedIn,
                    const QString& shadnetAccount) {
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

// ---- systemService.getTimeInfo, transactionService ----------------------------------------------

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
        // Activity feed and notifications: answered empty, so menus waiting for them go on.
        endpoint<GetActivityFeedRequest, GetActivityFeedResponse>(
            "activityService", "getActivityFeed", false, true,
            [](Context&, const GetActivityFeedRequest&, GetActivityFeedResponse&) {}),
        endpoint<GetNotificationsRequest, GetNotificationsResponse>(
            "activityService", "getNotifications", false, true,
            [](Context&, const GetNotificationsRequest&, GetNotificationsResponse&) {}),
        // Multiplayer events.
        endpoint<GetCurrentSeasonRequest, GetCurrentSeasonResponse>(
            "eventService", "getCurrentSeason", false, true, getCurrentSeason),
        endpoint<QueryEventListActivityRequest, QueryEventListActivityResponse>(
            "matchingService", "queryEventListActivity", false, true, queryEventListActivity),
        endpoint<QueryUpcomingMatchSlotsRequest, QueryUpcomingMatchSlotsResponse>(
            "matchingService", "queryUpcomingMatchSlots", false, true, queryUpcomingMatchSlots),
        endpoint<RegisterInterestRequest, InterestResponse>("matchingService", "registerInterest",
                                                            false, true, registerInterest),
        endpoint<RegisterInterestRequest, InterestResponse>("matchingService", "unregisterInterest",
                                                            false, true, unregisterInterest),
        endpoint<QueryInterestRequest, QueryInterestResponse>("matchingService", "queryInterest",
                                                              false, true, queryInterest),
        endpoint<QueryMatchSlotsByIdRequest, QueryMatchSlotsByIdResponse>(
            "matchingService", "queryMatchSlotsById", false, true, queryMatchSlotsById),
        endpoint<MarkReadyRequest, MarkReadyResponse>(
            "matchingService", "markReady", false, true,
            [](Context& c, const MarkReadyRequest& r, MarkReadyResponse& o) {
                markReady(c, r, o, true);
            }),
        endpoint<MarkReadyRequest, MarkReadyResponse>(
            "matchingService", "markNotReady", false,
            true,
            [](Context& c, const MarkReadyRequest& r,
               MarkReadyResponse& o) { markReady(c, r, o, false); }),
        endpoint<LobbyWithMemberRequest, LobbiesResponse>("lobbyService", "joinLobby", false, true,
                                                          joinLobby),
        endpoint<ProgressToNextRaceRequest, LobbiesResponse>("lobbyService", "progressToNextRace",
                                                             false, true, progressToNextRace),
    };
    return list;
}

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

QStringList splitPath(QString path) {
    if (path.startsWith('/')) {
        path.remove(0, 1);
    }
    if (!path.startsWith(QLatin1String("egs/binary/"))) {
        return {};
    }
    return path.mid(11).split('/', Qt::SkipEmptyParts);
}

} // namespace egs
