// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// EGS: the functions of egs/egs.h (HTTP entry point, descriptors, admin tools).

#include "egs/internal.h"

using namespace egs;

void setEgsConfig(const EgsConfig& c) {
    config() = c;
    static QTimer* presenceTimer = [] {
        auto* t = new QTimer(QCoreApplication::instance());
        t->setInterval(5000);
        QObject::connect(t, &QTimer::timeout, [] {
            QMutexLocker lock(&sessions().mutex);
            presenceCheckLocked(QDateTime::currentMSecsSinceEpoch());
        });
        t->start();
        return t;
    }();
    Q_UNUSED(presenceTimer);
    static QTimer* matchingTimer = [] {
        auto* t = new QTimer(QCoreApplication::instance());
        t->setInterval(1000);
        QObject::connect(t, &QTimer::timeout, [] {
            QMutexLocker lock(&sessions().mutex);
            matchingTickLocked(QDateTime::currentMSecsSinceEpoch());
        });
        t->start();
        return t;
    }();
    Q_UNUSED(matchingTimer);
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
                                      "endpoints() in src/egs/core.cpp";
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
        // As the leader's kick: action 14 to the rest and to the kicked player.
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
