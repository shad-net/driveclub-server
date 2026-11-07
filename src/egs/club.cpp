// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: clubService, keywordService, moderationService.

#include "egs/internal.h"

namespace egs {

// ---- clubService
// ---------------------------------------------------------------------------------

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
    if (rest.size() == 1) {
        // Down to one member: the club fame told to the game changes (clubFameForGame), so its
        // game asks getFame again.
        pushClubRefresh<MsgClubProgressionChange>(club, 0);
    }
}

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
    if (memberIds(club).size() == 2) {
        // No longer a one-member club: the club fame told to the game changes (clubFameForGame).
        pushClubRefresh<MsgClubProgressionChange>(club, 0);
    }
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
            // The owner accepts a join request by inviting the player.
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

} // namespace egs
