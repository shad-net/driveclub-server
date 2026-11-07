// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: storageService: ghosts.

#include "egs/internal.h"

namespace egs {

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

} // namespace egs
