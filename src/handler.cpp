// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "handler.h"

#include "database.h"
#include "egs.h"
#include "otg.h"

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QRegularExpression>

namespace {
Q_LOGGING_CATEGORY(lcHandler, "handler")
}

// Last gameStatus text each player sent (PUT presence/gameStatus), by lower-case name.
static QMutex& presenceMutex() {
    static QMutex m;
    return m;
}
static QHash<QString, QString>& presenceStatus() {
    static QHash<QString, QString> h;
    return h;
}

// Online = an EGS session of that player was used recently. The game reads the friend list once
// at start and then waits for push events, which the runtime does not deliver yet; a short window
// would leave whoever started first seeing everyone offline. So "recently" is generous.
// shortcut: replace with real presence push events once the runtime fires them.
static bool playerOnline(const QString& name) {
    constexpr qint64 OnlineWindowMs = 6 * 60 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const EgsSessionInfo& s : egsSessions()) {
        if (s.name.compare(name, Qt::CaseInsensitive) == 0 && now - s.lastSeen < OnlineWindowMs) {
            return true;
        }
    }
    return false;
}

static QJsonObject presenceOf(const QString& name) {
    QJsonObject primary{{"onlineStatus", playerOnline(name) ? "online" : "offline"},
                        {"platform", "PS4"}};
    if (playerOnline(name)) {
        // The 1.28 eboot calls sceNpSetNpTitleId with CUSA00003_00, and the game seems to count only
        // friends in that same title. shortcut: one fixed id; per-player ids if a US build differs.
        primary.insert("gameTitleInfo", QJsonObject{{"npTitleId", "CUSA00003_00"},
                                                    {"titleName", "DRIVECLUB"},
                                                    {"launchPlatform", "PS4"}});
        QMutexLocker l(&presenceMutex());
        const QString status = presenceStatus().value(name.toLower());
        if (!status.isEmpty()) {
            primary.insert("gameStatus", status);
        }
    }
    return QJsonObject{{"primaryInfo", primary}};
}

// The Sony web API calls the game makes next to EGS and OTG, answered with the smallest valid reply:
// every other known player is a friend, presence comes from recent EGS sessions, a profile exists
// for every known player.
// shortcut: the store container is empty; upgrade when it matters.
static bool handlePsn(const HttpRequest& request, const QString& endpoint,
                      const HttpServer::Responder& respond) {
    static const QRegularExpression users(
        QStringLiteral("^v1/users/([^/]+)/(friendList|presence/gameStatus)$"));
    const bool get = request.method == "GET";
    HttpResponse r;
    r.contentType = "application/json";
    const auto m = users.match(endpoint);
    if (m.hasMatch() && get && m.captured(2) == QLatin1String("friendList")) {
        // Same as EGS friends: every other known player.
        const bool withPresence = request.query.hasQueryItem(QStringLiteral("presenceType"));
        QJsonArray list;
        for (const db::Player& p : db::players()) {
            if (p.loggedIn && p.name.compare(m.captured(1), Qt::CaseInsensitive) != 0) {
                QJsonObject f{{"onlineId", p.name}, {"personalDetail", QJsonObject{}}};
                if (withPresence) {
                    f.insert("presence", presenceOf(p.name));
                }
                list.append(f);
            }
        }
        r.body = QJsonDocument(QJsonObject{{"friendList", list}, {"start", 0}, {"size", list.size()},
                                           {"totalResults", list.size()}})
                     .toJson(QJsonDocument::Compact);
    } else if (m.hasMatch() && request.method == "PUT" && m.captured(2) == QLatin1String("presence/gameStatus")) {
        const QString status = QJsonDocument::fromJson(request.body).object().value("gameStatus").toString();
        {
            QMutexLocker l(&presenceMutex());
            presenceStatus().insert(m.captured(1).toLower(), status);
        }
        r.status = 204;
    } else if (get && endpoint == QLatin1String("v1/profiles")) {
        // The game reads "profiles"[i] (onlineId, npId, avatarUrl, personalDetail.profilePictureUrl)
        // for the comma-separated onlineIds it asks for. npId: the game parses it
        // (sceNpWebApiUtilityParseNpId) into the SceNpId it opens a P2P connection to; without it
        // the guest never connects to the host of a private lobby. Base64 of "<onlineId>@<opt>",
        // the runtime's parser takes the part before '@'.
        QJsonArray profiles;
        const QStringList ids = request.query.queryItemValue(QStringLiteral("onlineId"))
                                    .split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& id : ids) {
            const auto player = db::playerByName(id);
            if (!player) {
                continue;
            }
            profiles.append(QJsonObject{
                {"onlineId", player->name},
                {"npId", QString::fromLatin1((player->name + "@b7.de.np").toUtf8().toBase64())},
                {"avatarUrl", QString()},
                {"plus", 1},
                {"aboutMe", QString()},
                {"languagesUsed", QJsonArray()},
                {"avatarUrls", QJsonArray()},
                {"isOfficiallyVerified", false},
                {"personalDetail", QJsonObject{{"firstName", QString()},
                                               {"lastName", QString()},
                                               {"profilePictureUrl", QString()},
                                               {"profilePictureUrls", QJsonArray()}}}});
        }
        if (profiles.isEmpty()) {
            r.status = 404;
            r.body = R"({"error":{"code":2105356,"message":"User not found"}})";
        } else {
            r.body = QJsonDocument(QJsonObject{{"profiles", profiles},
                                               {"profile", profiles.first()}})
                         .toJson(QJsonDocument::Compact);
        }
    } else if (get && endpoint.startsWith(QLatin1String("v1/users/me/container/"))) {
        r.body = R"({"skus":[],"links":[]})";
    } else if (get && endpoint.startsWith(QLatin1String("dccoop/p2p/"))) {
        // P2P directory in place of a matchmaking server: each game registers the UDP port of its
        // P2P transport (dccoop/p2p/register/<name>?port=N, address = where the request came from),
        // and signaling looks a peer up with dccoop/p2p/<name>, answered as "<ipv4> <port>".
        // shortcut: in memory only; a peer behind NAT needs its port forwarded.
        static QMutex mutex;
        static QHash<QString, QPair<QString, int>> peers;
        QMutexLocker l(&mutex);
        r.contentType = "text/plain";
        const QString rest = endpoint.mid(11);
        if (rest.startsWith(QLatin1String("register/"))) {
            const QString name = rest.mid(9).toLower();
            const int port = request.query.queryItemValue(QStringLiteral("port")).toInt();
            bool ok = false;
            const QHostAddress ip(request.peer.toIPv4Address(&ok));
            const auto old = peers.value(name);
            peers.insert(name, {ok ? ip.toString() : QStringLiteral("127.0.0.1"), port});
            if (old != peers.value(name)) {
                qCInfo(lcHandler).noquote() << "p2p:" << name << "at" << peers.value(name).first
                                            << peers.value(name).second;
            }
            r.status = 204;
        } else if (peers.contains(rest.toLower())) {
            auto p = peers.value(rest.toLower());
            // A game on the server's own PC registers from 127.0.0.1; a player on another PC
            // reaches it at the address this server has towards them (LAN or VPN such as Hamachi).
            bool fromOther = false;
            const quint32 asker = request.peer.toIPv4Address(&fromOther);
            if (QHostAddress(p.first).isLoopback() && fromOther && !QHostAddress(asker).isLoopback()) {
                bool ok = false;
                const quint32 own = request.local.toIPv4Address(&ok);
                if (ok) {
                    p.first = QHostAddress(own).toString();
                }
            }
            r.body = QStringLiteral("%1 %2").arg(p.first).arg(p.second).toUtf8();
        } else {
            r.status = 404;
        }
    } else {
        return false;
    }
    respond(r);
    return true;
}

// Game custom data: what the game sends a friend through PSN (private lobby invitations, challenges).
// The sender POSTs a multipart body: a JSON "message" part (recipients in "to") and the binary game
// data. On a console the recipient's system raises a GAME_CUSTOM_DATA event with the item id; here
// the recipient's runtime polls dccoop/pending/<name> and raises it. The game then GETs
// .../items/<id>/gameData and PUTs .../items/<id> when done.
// shortcut: items live in memory only and are never deleted.
struct CustomDataItem {
    quint64 id = 0;
    QString from;
    QStringList to;
    QJsonObject message;
    QByteArray gameData;
    QStringList delivered; // recipients whose runtime has been told
};
static QMutex& customDataMutex() {
    static QMutex m;
    return m;
}
static QList<CustomDataItem>& customData() {
    static QList<CustomDataItem> items;
    return items;
}

// Splits a multipart body into (headers, content) parts.
static QList<QPair<QByteArray, QByteArray>> multipartParts(const HttpRequest& request) {
    QList<QPair<QByteArray, QByteArray>> parts;
    static const QRegularExpression boundaryRe(QStringLiteral("boundary=\"?([^\";]+)\"?"));
    const auto m = boundaryRe.match(QString::fromLatin1(request.headers.value("content-type")));
    if (!m.hasMatch()) {
        return parts;
    }
    const QByteArray delimiter = "--" + m.captured(1).toLatin1();
    qsizetype pos = request.body.indexOf(delimiter);
    while (pos >= 0) {
        const qsizetype start = pos + delimiter.size();
        if (request.body.mid(start, 2) == "--") {
            break;
        }
        const qsizetype next = request.body.indexOf(delimiter, start);
        QByteArray part = request.body.mid(start, next < 0 ? -1 : next - start);
        if (part.startsWith("\r\n")) {
            part.remove(0, 2);
        }
        if (part.endsWith("\r\n")) {
            part.chop(2);
        }
        const qsizetype split = part.indexOf("\r\n\r\n");
        if (split >= 0) {
            parts.append({part.left(split), part.mid(split + 4)});
        }
        pos = next;
    }
    return parts;
}

static bool handleCustomData(const HttpRequest& request, const QString& endpoint,
                             const HttpServer::Responder& respond) {
    static const QRegularExpression itemRe(
        QStringLiteral("^v1/users/([^/]*)/gameCustomData/items/(\\d+)(/gameData)?$"));
    static const QRegularExpression pendingRe(QStringLiteral("^dccoop/pending/([^/]+)$"));
    HttpResponse r;
    r.contentType = "application/json";

    if (request.method == "POST" && endpoint == QLatin1String("v1/gameCustomData/items")) {
        CustomDataItem item;
        item.from = QString::fromUtf8(request.headers.value("x-dccoop-online-id"));
        for (const auto& [head, content] : multipartParts(request)) {
            if (head.contains("Content-Description:message")) {
                item.message = QJsonDocument::fromJson(content).object();
                for (const QJsonValue& v : item.message.value("to").toArray()) {
                    item.to << v.toString();
                }
            } else if (head.contains("Content-Description:game-data-binary")) {
                item.gameData = content;
            }
        }
        if (item.to.isEmpty()) {
            r.status = 400;
            r.body = R"({"error":{"code":2097153,"message":"Invalid request"}})";
            respond(r);
            return true;
        }
        {
            QMutexLocker l(&customDataMutex());
            item.id = 1000 + quint64(customData().size());
            customData().append(item);
        }
        qCInfo(lcHandler).noquote() << "game custom data" << item.id << "from"
                                    << (item.from.isEmpty() ? QStringLiteral("?") : item.from)
                                    << "to" << item.to.join(QStringLiteral(", ")) << "-"
                                    << item.message.value("item").toObject().value("gameDataDetail")
                                           .toObject().value("dataName").toString()
                                    << item.gameData.size() << "bytes";
        r.body = QJsonDocument(QJsonObject{{"itemId", qint64(item.id)}}).toJson(QJsonDocument::Compact);
        respond(r);
        // Game data type 6 (at byte 8) is a private lobby invitation.
        if (item.gameData.size() >= 12 && item.gameData[8] == 6 && !item.from.isEmpty()) {
            for (const QString& to : item.to) {
                egsLobbyInvitation(item.from, to);
            }
        }
        return true;
    }

    if (const auto m = pendingRe.match(endpoint); m.hasMatch() && request.method == "GET") {
        const QString name = m.captured(1);
        QJsonArray list;
        QMutexLocker l(&customDataMutex());
        for (CustomDataItem& item : customData()) {
            if (item.to.contains(name, Qt::CaseInsensitive) &&
                !item.delivered.contains(name, Qt::CaseInsensitive)) {
                item.delivered << name;
                list.append(QJsonObject{{"itemId", qint64(item.id)}, {"from", item.from}});
            }
        }
        r.body = QJsonDocument(list).toJson(QJsonDocument::Compact);
        respond(r);
        return true;
    }

    if (const auto m = itemRe.match(endpoint); m.hasMatch()) {
        const quint64 id = m.captured(2).toULongLong();
        if (request.method == "PUT") {
            // Marking an item used ({"dataUsedFlag":true}). After accepting a lobby invitation the
            // game sends this for item 0 of user "" (its copy of the ids is already cleared), and a
            // failure stops the join, so every mark succeeds.
            r.status = 204;
            respond(r);
            return true;
        }
        QMutexLocker l(&customDataMutex());
        const CustomDataItem* found = nullptr;
        for (const CustomDataItem& item : customData()) {
            if (item.id == id) {
                found = &item;
            }
        }
        if (!found) {
            r.status = 404;
            r.body = R"({"error":{"code":2105858,"message":"Item not found"}})";
        } else if (request.method == "GET" && !m.captured(3).isEmpty()) {
            r.contentType = "application/octet-stream";
            r.body = found->gameData;
        } else if (request.method == "GET") {
            QJsonObject out = found->message.value("item").toObject();
            out.insert("itemId", qint64(found->id));
            out.insert("from", QJsonObject{{"onlineId", found->from}});
            r.body = QJsonDocument(out).toJson(QJsonDocument::Compact);
        } else {
            r.status = 204; // PUT: marked as used
        }
        respond(r);
        return true;
    }
    return false;
}

void handleRequest(const HttpRequest& request, const QString& endpoint,
                   const HttpServer::Responder& respond) {
    if (handleEgs(request, endpoint, respond) || handleOtg(request, endpoint, respond) ||
        handlePsn(request, endpoint, respond) || handleCustomData(request, endpoint, respond)) {
        return;
    }

    // Add endpoints above. Everything else is answered 404 and marked in the log
    qCWarning(lcHandler).noquote() << "unhandled:" << request.method << endpoint;
    HttpResponse response;
    response.status = 404;
    response.contentType = "text/plain";
    response.body = "Not Found";
    respond(response);
}
