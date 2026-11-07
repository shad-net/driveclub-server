// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: objectiveService: accolades.

#include "egs/internal.h"

namespace egs {

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

} // namespace egs
