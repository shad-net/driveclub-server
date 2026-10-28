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
#include <QList>
#include <QLoggingCategory>
#include <QMessageAuthenticationCode>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QUuid>

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

// Sessions

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
};

struct Sessions {
    QMutex mutex;
    QHash<QByteArray, Session> byKey;
};

Sessions& sessions() {
    static Sessions s;
    return s;
}

// Players (account id -> name)

void rememberPlayer(qint64 accountId, const QString& name, bool loggedIn,
                    const QString& shadnetAccount = {}) {
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

QString qs(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

// Endpoints

struct Context {
    const HttpRequest& request;
    QByteArray sessionKey;    // the session this request was signed with, empty before login
    QByteArray newSessionKey; // set by a handler: sent as X-SessionKey, becomes the signing key
    int status = 200;
    AuthResult auth; // login endpoints: the shadNet check of the authCode
};

struct Endpoint {
    QString service;
    QString method;
    const gp::Descriptor* request;
    const gp::Descriptor* response;
    bool secure;
    bool listed;
    std::function<std::string(Context&, const QByteArray&)> run;
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
        if (const auto f = db::fame(id)) {
            p->mutable_accountprogression()->set_xp(f->xp);
        }
        names << QStringLiteral("%1=%2").arg(id).arg(name);
    }
    qCInfo(lcEgs).noquote() << "profiles for" << req.accounts_size()
                            << "accounts:" << names.join(QStringLiteral(", "));
}

void aggregatedProfiles(Context&, const GetAggregatedProfilesRequest& req,
                        GetAggregatedProfilesResponse& resp) {
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
        AccountRef* a = resp.add_profiles()->mutable_account();
        a->set_id(id);
        a->set_name(name.toStdString());
        found << QStringLiteral("%1=%2%3").arg(name).arg(id).arg(known ? QString()
                                                                       : QStringLiteral(" (new)"));
    }
    qCInfo(lcEgs).noquote() << "aggregated profiles:" << found.join(QStringLiteral(", "));
}

void friends(Context& ctx, const GetFriendsRequest&, GetFriendsResponse& resp) {
    const QString me = egsSessionName(ctx.sessionKey);
    QStringList names;
    for (const EgsPlayerInfo& p : egsPlayers()) {
        if (p.name == me) {
            continue;
        }
        FriendAccount* a = resp.add_friends()->mutable_account();
        a->set_id(p.accountId);
        a->set_lastseen(0); // unit unknown (seconds or ms); 0 = not seen
        a->set_name(p.name.toStdString());
        names << p.name;
    }
    qCInfo(lcEgs).noquote() << "friends of" << (me.isEmpty() ? QStringLiteral("?") : me) << "->"
                            << names.size()
                            << (names.isEmpty() ? QString() : names.join(QStringLiteral(", ")));
}

// systemService.getTimeInfo, transactionService

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

// profileService: fame

qint64 sessionAccount(const Context& ctx, QString* name = nullptr) {
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
    resp.mutable_clubprogression()->set_xp(0); // clubs are not implemented yet
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
    resp.mutable_clubprogression()->mutable_after()->set_xp(0);
    qCInfo(lcEgs).noquote()
        << "fame update from" << (name.isEmpty() ? QStringLiteral("?") : name) << "total" << sent
        << "-> stored" << f.xp
        << (req.clubfamedelta() > 0
                ? QStringLiteral("(club delta %1 ignored: no clubs yet)").arg(req.clubfamedelta())
                : QString())
        << "transaction" << qs(req.transactionid());
}

// storageService: ghosts

QString bobChecksum(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Md5).toBase64());
}

void createBob(Context& ctx, const CreateBobRequest& req, CreateBobResponse& resp) {
    QString name;
    const qint64 account = sessionAccount(ctx, &name);
    const QByteArray data(req.body().data(), static_cast<qsizetype>(req.body().size()));
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
    const qint64 id = data.isEmpty() ? 0 : db::putGhost(g, data);
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
    qCInfo(lcEgs).noquote() << "ghost" << id << "uploaded by" << name << "," << data.size()
                            << "bytes, type" << req.objecttype() << "access" << req.accesstype();
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
        endpoint<CreateBobRequest, CreateBobResponse>("storageService", "createBob", false, true,
                                                      createBob),
        endpoint<GetRemoteUrlRequest, GetRemoteUrlResponse>("storageService", "getRemoteUrl", false,
                                                            true, getRemoteUrl),
    };
    return list;
}

const Endpoint* findEndpoint(const QString& service, const QString& method) {
    for (const Endpoint& e : endpoints()) {
        if (e.service == service && e.method == method) {
            return &e;
        }
    }
    return nullptr;
}

std::string endpointList() {
    SvcEndpointDescriptorList list;
    for (const Endpoint& e : endpoints()) {
        if (!e.listed) {
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
            result.set_retryhint(auth.unavailable ? 30 : 300);
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
