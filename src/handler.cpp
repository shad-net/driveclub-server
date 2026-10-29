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

#include <algorithm>
#include <cmath>

namespace {
Q_LOGGING_CATEGORY(lcHandler, "handler")
}

// Last gameStatus each player sent (PUT presence/gameStatus: the text and its translations), by
// lower-case name.
static QMutex& presenceMutex() {
    static QMutex m;
    return m;
}
static QHash<QString, QJsonObject>& presenceStatus() {
    static QHash<QString, QJsonObject> h;
    return h;
}

// The status text in the first of the asked languages ("en-GB,de") the player sent, else the default.
static QString gameStatusOf(const QString& name, const QStringList& languages = {}) {
    QMutexLocker l(&presenceMutex());
    const QJsonObject sent = presenceStatus().value(name.toLower());
    for (const QString& language : languages) {
        for (const QJsonValue& v : sent.value("localizedGameStatus").toArray()) {
            const QString lang = v.toObject().value("npLanguage").toString();
            if (lang.compare(language, Qt::CaseInsensitive) == 0 ||
                lang.compare(language.section('-', 0, 0), Qt::CaseInsensitive) == 0) {
                return v.toObject().value("gameStatus").toString();
            }
        }
    }
    return sent.value("gameStatus").toString();
}

// Online = an EGS session of that player was used recently. The game reads the friend list once
// at start and then waits for push events, which the runtime does not deliver yet; a short window
// would leave whoever started first seeing everyone offline. So "recently" is generous.
// shortcut: replace with real presence push events once the runtime fires them.
// A game with the presence poll (dccoop/presence/<name>, every 5 s) is online while it keeps asking:
// a closed game shows as offline within seconds. Older runtimes fall back to their EGS sessions.
static QHash<QString, qint64>& presencePolls() {
    static QHash<QString, qint64> h; // lower-case name -> last poll, under presenceMutex
    return h;
}

static bool playerOnline(const QString& name) {
    constexpr qint64 PollWindowMs = 15 * 1000;
    constexpr qint64 OnlineWindowMs = 6 * 60 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker l(&presenceMutex());
        const auto polled = presencePolls().constFind(name.toLower());
        if (polled != presencePolls().constEnd()) {
            return now - *polled < PollWindowMs;
        }
    }
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
        const QString status = gameStatusOf(name);
        if (!status.isEmpty()) {
            primary.insert("gameStatus", status);
        }
    }
    return QJsonObject{{"primaryInfo", primary}};
}

// A player's avatar: a 128x128 PNG made from the name (a head-and-shoulders figure on a colour
// from a hash of the name). Without an avatarUrl the game shows an empty spinning ring where the
// avatar goes (menu bar, profile, friends) and waits for it.
// shortcut: generated pictures only; uploading your own is not supported.
static QByteArray avatarPng(const QString& name) {
    constexpr int Size = 128;
    const uint h = qHash(name.toLower());
    const int hue = int(h % 360);
    // HSV (hue, 0.55, 0.75) to RGB
    const auto channel = [hue](int n) {
        const double k = std::fmod(n + hue / 60.0, 6.0);
        const double v = 0.75, s = 0.55;
        return uchar(255 * (v - v * s * std::max(0.0, std::min({k, 4 - k, 1.0}))));
    };
    const uchar bg[3] = {channel(5), channel(3), channel(1)};
    const uchar fg[3] = {235, 235, 235};
    QByteArray raw;
    raw.reserve((Size * 3 + 1) * Size);
    for (int y = 0; y < Size; ++y) {
        raw.append(char(0)); // filter: none
        for (int x = 0; x < Size; ++x) {
            const double dx = x - Size / 2.0, dyHead = y - Size * 0.40, dyBody = y - Size * 1.02;
            const bool head = dx * dx + dyHead * dyHead < (Size * 0.19) * (Size * 0.19);
            const bool body = dx * dx + dyBody * dyBody < (Size * 0.40) * (Size * 0.40);
            const uchar* c = head || body ? fg : bg;
            raw.append(char(c[0])).append(char(c[1])).append(char(c[2]));
        }
    }
    static quint32 table[256];
    if (table[1] == 0) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) {
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    const auto chunk = [](const char* type, const QByteArray& data) {
        QByteArray out;
        const auto be32 = [&out](quint32 v) {
            out.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v));
        };
        be32(quint32(data.size()));
        const QByteArray body = QByteArray(type, 4) + data;
        quint32 crc = 0xffffffffu;
        for (const char b : body) {
            crc = table[(crc ^ uchar(b)) & 0xff] ^ (crc >> 8);
        }
        out.append(body);
        be32(crc ^ 0xffffffffu);
        return out;
    };
    QByteArray ihdr;
    for (const quint32 v : {quint32(Size), quint32(Size)}) {
        ihdr.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v));
    }
    ihdr.append(char(8)).append(char(2)).append(char(0)).append(char(0)).append(char(0));
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    png += chunk("IHDR", ihdr);
    png += chunk("IDAT", qCompress(raw, 9).mid(4)); // qCompress: 4-byte size, then a zlib stream
    png += chunk("IEND", {});
    return png;
}

// The Sony web API calls the game makes next to EGS and OTG, answered with the smallest valid reply:
// every other known player is a friend, presence comes from recent EGS sessions, a profile exists
// for every known player.
// shortcut: the store container is empty; upgrade when it matters.
static bool handlePsn(const HttpRequest& request, const QString& endpoint,
                      const HttpServer::Responder& respond) {
    static const QRegularExpression users(
        QStringLiteral("^v1/users/([^/]+)/(friendList|presence/gameStatus|presence)$"));
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
        const QJsonObject sent = QJsonDocument::fromJson(request.body).object();
        {
            QMutexLocker l(&presenceMutex());
            presenceStatus().insert(m.captured(1).toLower(), sent);
        }
        r.status = 204;
    } else if (m.hasMatch() && get && m.captured(2) == QLatin1String("presence")) {
        // One friend's presence (the game asks when it shows what they are doing). Answered in both
        // shapes the PSN API used: flat and with a platformInfoList.
        const QString name = m.captured(1);
        const bool online = playerOnline(name);
        const QString status = gameStatusOf(
            name, request.query.queryItemValue(QStringLiteral("npLanguages")).split(',', Qt::SkipEmptyParts));
        QJsonObject info{{"onlineStatus", online ? "online" : "offline"}, {"platform", "PS4"}};
        if (online) {
            info.insert("npTitleId", "CUSA00003_00");
            info.insert("titleName", "DRIVECLUB");
            if (!status.isEmpty()) {
                info.insert("gameStatus", status);
            }
        }
        QJsonObject presence = info;
        presence.insert("platformInfoList", QJsonArray{info});
        presence.insert("hasBroadcastData", false);
        presence.insert("primaryInfo", presenceOf(name).value("primaryInfo"));
        r.body = QJsonDocument(QJsonObject{{"presence", presence}}).toJson(QJsonDocument::Compact);
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
            const QString avatar =
                otgBaseUrl(request) + QStringLiteral("/dccoop/avatar/%1.png").arg(player->name);
            profiles.append(QJsonObject{
                {"onlineId", player->name},
                {"npId", QString::fromLatin1((player->name + "@b7.de.np").toUtf8().toBase64())},
                {"avatarUrl", avatar},
                {"plus", 1},
                {"aboutMe", QString()},
                {"languagesUsed", QJsonArray()},
                {"avatarUrls", QJsonArray{QJsonObject{{"size", "m"}, {"avatarUrl", avatar}}}},
                {"isOfficiallyVerified", false},
                {"personalDetail", QJsonObject{{"firstName", QString()},
                                               {"lastName", QString()},
                                               {"profilePictureUrl", avatar},
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
    } else if (get && endpoint == QLatin1String("v1/users/me/entitlements")) {
        r.body = R"({"entitlements":[],"start":0,"size":0,"totalResults":0})";
    } else if (get && endpoint.startsWith(QLatin1String("dccoop/presence/"))) {
        // Friends' presence for the runtime, which polls it and raises the presence push events the
        // game only learns changes from: one "<name>\t<1 online|0>\t<gameStatus>" line per friend.
        // The poll itself is the asking player's sign of life (playerOnline).
        const QString me = endpoint.mid(16);
        {
            QMutexLocker l(&presenceMutex());
            presencePolls().insert(me.toLower(), QDateTime::currentMSecsSinceEpoch());
        }
        QByteArray body;
        for (const db::Player& p : db::players()) {
            if (!p.loggedIn || p.name.compare(me, Qt::CaseInsensitive) == 0) {
                continue;
            }
            const bool online = playerOnline(p.name);
            body += (p.name + '\t' + (online ? "1" : "0") + '\t' + (online ? gameStatusOf(p.name) : QString()))
                        .toUtf8().replace('\n', ' ') + '\n';
        }
        r.contentType = "text/plain; charset=utf-8";
        r.body = body;
    } else if (get && endpoint.startsWith(QLatin1String("dccoop/avatar/"))) {
        QString name = endpoint.mid(14);
        name.chop(name.endsWith(QLatin1String(".png")) ? 4 : 0);
        r.contentType = "image/png";
        r.body = avatarPng(name);
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
