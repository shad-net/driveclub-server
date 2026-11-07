// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "egs/egs.h"

#include "auth.h"
#include "database.h"
#include "otg/otg.h"
#undef slots
#include "egs.pb.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/text_format.h>

#include <QCoreApplication>
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
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace gp = google::protobuf;

namespace egs {

Q_DECLARE_LOGGING_CATEGORY(lcEgs)

// ---- Sessions ---------------------------------------------------------------------------------

struct Session {
    QByteArray key;
    qint32 accountId = 0;
    QString name;
    QString shadnetAccount; // empty when the login was not verified with shadNet
    QString clientVersion;
    std::string
        sessionData; // the login's clientSessionData: the game's peer address (race lobbies)
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

inline Sessions& sessions() {
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

inline Presence& presence() {
    static Presence p;
    return p;
}

// ---- Parties (lobbyService, lobbyType 1) ------------------------------------------------------

constexpr int LobbyJoined = 4, LobbyLeft = 6, LobbyStatusChanged = 7, LobbyMemberData = 8,
              LobbyHostChanged = 12, LobbyKicked = 14;

struct PartyMember {
    qint64 accountId = 0;
    QString name;
    bool host = false;
    std::string contentRestriction;
    // Race lobbies (type 0) only.
    int playerNumber = 0;
    std::string memberData;
    QString clientVersion;
};

struct Party {
    QString id;
    int type = 1;
    int status = 1; // LobbyStatus 1 = created; the leader sets its own with updateLobby
    qint64 matchSlotId = 0;
    QList<PartyMember> members;
    // Race lobbies (type 0) only.
    qint64 eventListId = 0;
    qint64 nextSlot = 0; // chosen by the first progressToNextRace
};

struct Parties {
    QHash<QString, Party> byId;
    QHash<QPair<qint64, int>, QString> ofAccount; // (account, lobby type) -> lobby id
};

inline Parties& parties() {
    static Parties p;
    return p;
}

// protobuf returns std::string in older releases and std::string_view in newer ones.
inline QString qs(std::string_view s) {
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

constexpr int ClubMaxMembers = 6;
EgsConfig& config();
QSet<qint64> onlineAccountsLocked(qint64 now);
void pushLocked(qint64 account, const QByteArray& type, const std::string& data);
void presenceCheckLocked(qint64 now);
void rememberPlayer(qint64 accountId, const QString& name, bool loggedIn,
                    const QString& shadnetAccount = {});
qint32 accountIdFor(const QString& name);
void timeInfo(Context&, const Empty&, GetTimeInfoResponse& resp);
Session* sessionOf(const Context& ctx);
std::function<void(Context&, const TransactionRequest&, TransactionResponse&)> transactionCall(
    const char* method, bool startNew);
qint64 sessionAccount(const Context& ctx, QString* name = nullptr);
Endpoint withGroup(Endpoint e, Endpoint::Discovery group);
const QList<Endpoint>& endpoints();
bool discoveryOn(const Endpoint& e);
const Endpoint* findEndpoint(const QString& service, const QString& method);
std::string endpointList();
HttpResponse egsAnswer(const QString& method, const QByteArray& sessionKey, int status,
                       const QByteArray& body);
QStringList splitPath(QString path);
void login(Context& ctx, const OAuthLoginRequest& req, OAuthLoginResponse& resp);
void minimalProfiles(Context&, const GetMinimalProfilesRequest& req,
                     GetMinimalProfilesResponse& resp);
void aggregatedProfiles(Context& ctx, const GetAggregatedProfilesRequest& req,
                        GetAggregatedProfilesResponse& resp);
qint64 friendAccountId(const QString& name);
void fillFriends(GetFriendsResponse& resp, const QStringList& names);
void friends(Context& ctx, const GetFriendsRequest&, GetFriendsResponse& resp);
void changeFriends(Context& ctx, const FriendsChangeRequest& req, GetFriendsResponse& resp,
                   bool add);
void addFriends(Context& ctx, const FriendsChangeRequest& req, GetFriendsResponse& resp);
void removeFriends(Context& ctx, const FriendsChangeRequest& req, GetFriendsResponse& resp);
void queryStatus(Context& ctx, const QueryStatusRequest& req, QueryStatusResponse& resp);
void quickSearch(Context& ctx, const AccountQuickSearchRequest& req, AccountListResponse& resp);
void fillLobby(::Lobby* out, const Party& p);
void pushLobbyLocked(const Party& p, int action, const QList<qint64>& updated, QList<qint64> to);
QList<qint64> partyAccounts(const Party& p);
QString leavePartyLocked(qint64 account, int type, int action = LobbyLeft,
                         const QList<qint64>& alsoTell = {});
void leaveAllPartiesLocked(qint64 account);
QString lobbyWho(qint64 account, const QString& name);
void createLobby(Context& ctx, const LobbyWithMemberRequest& req, LobbiesResponse& resp);
void getLobby(Context& ctx, const LobbyWithMemberRequest& req, LobbiesResponse& resp);
void joinWithAccount(Context& ctx, const LobbyJoinWithAccountRequest& req, Empty&);
void updateLobby(Context& ctx, const LobbyWithMemberRequest& req, Empty&);
void updateMember(Context& ctx, const LobbyWithMemberRequest& req, Empty&);
void leaveCurrentLobby(Context& ctx, const LobbyLeaveCurrentRequest& req,
                       LobbyLeaveCurrentResponse&);
void leaveLobbyById(Context& ctx, const LobbyLeaveByIdRequest& req, LobbyLeaveByIdResponse&);
void removeFromLobby(Context& ctx, const LobbyRemoveRequest& req, Empty&);
void changeHost(Context& ctx, const LobbyChangeHostRequest& req, Empty&);
qint64 slotIntervalMs();
qint64 floorDiv(qint64 a, qint64 b);
void pushMatchingLocked(qint64 account, int action, qint64 slot, qint64 startsInMs = -1);
QSet<qint64> knownPlayers(qint64 account);
bool isLocalIPv4(quint32 a);
std::string memberDataFor(const Session& s);
const Session* newestSessionLocked(qint64 account);
PartyMember raceMemberLocked(qint64 account, int number);
void getCurrentSeason(Context&, const GetCurrentSeasonRequest&, GetCurrentSeasonResponse& resp);
void queryEventListActivity(Context&, const QueryEventListActivityRequest& req,
                            QueryEventListActivityResponse& resp);
void queryUpcomingMatchSlots(Context& ctx, const QueryUpcomingMatchSlotsRequest& req,
                             QueryUpcomingMatchSlotsResponse& resp);
void registerInterest(Context& ctx, const RegisterInterestRequest& req, InterestResponse& resp);
void unregisterInterest(Context& ctx, const RegisterInterestRequest& req, InterestResponse& resp);
void queryInterest(Context& ctx, const QueryInterestRequest& req, QueryInterestResponse& resp);
void queryMatchSlotsById(Context& ctx, const QueryMatchSlotsByIdRequest& req,
                         QueryMatchSlotsByIdResponse& resp);
void markReady(Context& ctx, const MarkReadyRequest& req, MarkReadyResponse& resp, bool ready);
void joinLobby(Context& ctx, const LobbyWithMemberRequest& req, LobbiesResponse& resp);
void progressToNextRace(Context& ctx, const ProgressToNextRaceRequest& req, LobbiesResponse& resp);
void matchingTickLocked(qint64 now);
void putClubFameScore(qint64 club, qint64 xp);
QString isoTime(qint64 ms);
void egsPush(qint64 account, const gp::Message& m);
void clubFail(Context& ctx, const QString& who, const QString& why);
QList<qint64> memberIds(qint64 clubId);
void pushMemberChange(qint64 club, qint64 account, ActivityVerb verb, QList<qint64> to);
bool isOwner(qint64 club, qint64 account);
void removeFromClub(qint64 club, qint64 account, ActivityVerb verb);
bool addToClub(qint64 club, qint64 account);
void fillCustomization(ClubCustomization* out, const db::Club& c, const QString& base, bool urls);
void fillClub(::Club* out, const db::Club& c, const QueryClubsRequest& req, const QString& base,
              const QSet<qint64>& online);
void clubSanitize(Context& ctx, const ClubNameMessage& req, ClubNameMessage& resp);
void clubCreate(Context& ctx, const ClubCreateRequest& req, ClubReply& resp);
void queryClubs(Context& ctx, const QueryClubsRequest& req, QueryClubsResponse& resp);
void clubQuickSearch(Context& ctx, const ClubQuickSearchRequest& req, ClubListResponse& resp);
void clubJoin(Context& ctx, const ClubIdRequest& req, ClubReply& resp);
void clubRequestJoin(Context& ctx, const ClubIdRequest& req, ClubReply&);
void clubLeave(Context& ctx, const ClubIdRequest&, ClubReply&);
void clubInvite(Context& ctx, const ClubAccountsRequest& req, Empty&);
void clubFindInvites(Context& ctx, const FindClubInvitesRequest& req, ClubInvitesResponse& resp);
void clubReject(Context& ctx, const ClubIdRequest& req, Empty&);
void clubMembership(Context& ctx, const ClubMembershipRequest& req, ClubReply& resp);
void clubCustomize(Context& ctx, const UpdateCustomizationRequest& req,
                   UpdateCustomizationResponse& resp);
std::optional<std::pair<qint64, qint64>> ownerAndTarget(Context& ctx,
                                                        const ClubAccountsRequest& req,
                                                        const QString& me, qint64 account);
void getKeywords(Context& ctx, const GetKeywordsRequest& req, GetKeywordsResponse& resp);
void clubReport(Context& ctx, const ClubReportRequest& req, Empty&);
void clubKick(Context& ctx, const ClubAccountsRequest& req, Empty&);
void clubChangeAdmin(Context& ctx, const ClubAccountsRequest& req, Empty&);
qint64 clubFameForGame(const db::Club& club);
void getFame(Context& ctx, const Empty&, GetFameResponse& resp);
void updateFame(Context& ctx, const UpdateFameRequest& req, UpdateFameResponse& resp);
void fillAccolades(AccoladeProgressResponse& resp, qint64 account, const QString& only);
void playerAccolades(Context& ctx, const GetAccoladeProgressRequest& req,
                     AccoladeProgressResponse& resp);
void clubAccolades(Context& ctx, const GetClubAccoladeProgressRequest& req,
                   AccoladeProgressResponse& resp);
void updateAccolades(Context& ctx, const UpdateAccoladeProgressRequest& req,
                     AccoladeProgressResponse& resp);
QString bobChecksum(const QByteArray& data);
void createBob(Context& ctx, const CreateBobRequest& req, CreateBobResponse& resp);
void getRemoteUrl(Context& ctx, const GetRemoteUrlRequest& req, GetRemoteUrlResponse& resp);
const QList<QPair<QString, QString>>& rowSets();
qint64 statValue(const QJsonObject& counts, const QString& subject, const QString& category,
                 const QString& counter);
QStringList statSets();
EgsStatsTable statsTable(qint64 accountId, const QString& set);
EgsStatsTable clubStatsTable(qint64 clubId, const QString& set, int* membersWithStats = nullptr);
EgsStatsTable statsFromCounts(const QJsonObject& counts, bool stored, const QString& set);
void fillStats(const EgsStatsTable& t, GetAccountStatsResponse& resp);
void accountStats(Context& ctx, const GetAccountStatsRequest& req, GetAccountStatsResponse& resp);
void clubStats(Context& ctx, const GetClubStatsRequest& req, GetAccountStatsResponse& resp);

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

} // namespace egs
