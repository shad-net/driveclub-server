// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "egs.h"

#include "egs.pb.h"

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
    qint64 lastSeen = 0;
};

struct Sessions {
    QMutex mutex;
    QHash<QByteArray, Session> byKey;
};

Sessions& sessions() {
    static Sessions s;
    return s;
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
    QString name = qs(req.name());
    if (name.isEmpty()) {
        name = QStringLiteral("player");
    }
    Session s;
    s.key = QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    s.name = name;
    s.accountId = accountIdFor(name);
    s.lastSeen = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker lock(&sessions().mutex);
        // One session per player: drop the old key.
        for (auto it = sessions().byKey.begin(); it != sessions().byKey.end();) {
            it = it->name == name ? sessions().byKey.erase(it) : std::next(it);
        }
        sessions().byKey.insert(s.key, s);
    }
    qCInfo(lcEgs).noquote() << QStringLiteral(
                                   "login %1 (client %2, %3) -> accountId %4, session %5")
                                   .arg(name, qs(req.clientversion()),
                                        req.psplus() ? QStringLiteral("PS Plus")
                                                     : QStringLiteral("no PS Plus"))
                                   .arg(s.accountId)
                                   .arg(QString::fromLatin1(s.key));

    SessionKey* key = resp.mutable_sessionkey();
    key->set_id(s.key.toStdString());
    key->set_accountid(s.accountId);
    key->set_username(name.toStdString());
    ctx.newSessionKey = s.key;
}

const QList<Endpoint>& endpoints() {
    static const QList<Endpoint> list = {
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

// "egs/binary/<service>/<method>" or "/egs/binary/..." -> {service, method}
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
            it->lastSeen = QDateTime::currentMSecsSinceEpoch();
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

// Copies only what protobuf 2.4.1 (the game's version) knows.
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
        r.contentType = "application/octet-stream";
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
    Context ctx{request, found.value_or(QByteArray()), {}, 200};

    const Endpoint* e = findEndpoint(service, method);
    if (!e) {
        qCWarning(lcEgs).noquote() << "no EGS method" << service + '.' + method
                                   << "- add its messages to proto/egs.proto and an entry to "
                                      "endpoints() in src/egs.cpp";
        HttpResponse r = egsAnswer(method, ctx.sessionKey, 404, {});
        r.headers.append(
            qMakePair(QByteArray("X-Service-Error"), QByteArray("Service Method unavailable")));
        respond(r);
        return true;
    }

    const std::string out = e->run(ctx, request.body);
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
    return true;
}
