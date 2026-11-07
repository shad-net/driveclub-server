// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: profileService: fame.

#include "egs/internal.h"

namespace egs {

qint64 clubFameForGame(const db::Club& club) {
    const QList<db::ClubMember> members = db::clubMembers(club.id);
    if (members.size() >= 2) {
        return club.xp;
    }
    qint64 xp = std::max(club.xp, config().soloClubFame);
    if (members.size() == 1) {
        if (const auto f = db::fame(members.first().accountId)) {
            xp = std::max(xp, f->xp);
        }
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
    a->set_stars(f.stars.value_or(0));
    a->set_elite(f.elite.value_or(0));
    // Club fame: the sum of the members' clubFameDelta (0 without a club).
    const auto member = account ? db::clubMember(account) : std::nullopt;
    const auto club = member ? db::club(member->clubId) : std::nullopt;
    const qint64 clubXp = club ? clubFameForGame(*club) : 0;
    resp.mutable_clubprogression()->set_xp(clubXp);
    qCInfo(lcEgs).noquote() << "fame of" << (name.isEmpty() ? QStringLiteral("?") : name) << "->"
                            << (found ? QString::number(f.xp) : QStringLiteral("none stored (0)"))
                            << (club && clubXp != club->xp
                                    ? QStringLiteral(
                                          "; club %1 fame %2 reported as %3 (one member: "
                                          "driver fame or egs.solo_club_fame)")
                                          .arg(club->id)
                                          .arg(club->xp)
                                          .arg(clubXp)
                                    : QString());
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
        }
        if (const auto club = db::club(member->clubId)) {
            clubXp = clubFameForGame(*club);
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

} // namespace egs
