// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "http_server.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <optional>

namespace {

Q_LOGGING_CATEGORY(lcConn, "conn")

constexpr qsizetype MaxHeaderBytes = 16 * 1024;
// Ghost and /egs/binary uploads can be larger than a JSON call.
constexpr qsizetype MaxBodyBytes = 16 * 1024 * 1024;
constexpr int IdleTimeoutMs = 60 * 1000;

QByteArray reasonPhrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "Status";
    }
}

enum class ParseResult { Complete, Incomplete, Error };

struct Parsed {
    HttpRequest request;
    bool keepAlive = true;
    qsizetype consumed = 0;
    int errorStatus = 400;
};

QString peerName(const QTcpSocket* socket) {
    return QStringLiteral("%1:%2").arg(socket->peerAddress().toString()).arg(socket->peerPort());
}

std::optional<qsizetype> decodeChunked(const QByteArray& buf, qsizetype pos, QByteArray* body,
                                       bool* error) {
    for (;;) {
        const qsizetype lineEnd = buf.indexOf("\r\n", pos);
        if (lineEnd < 0) {
            return std::nullopt;
        }
        bool ok = false;
        const QByteArray sizeField = buf.mid(pos, lineEnd - pos).split(';').first().trimmed();
        const qsizetype size = sizeField.toLongLong(&ok, 16);
        if (!ok || size < 0 || body->size() + size > MaxBodyBytes) {
            *error = true;
            return std::nullopt;
        }
        pos = lineEnd + 2;
        if (size == 0) {
            const qsizetype trailerEnd = buf.indexOf("\r\n\r\n", pos - 2);
            if (trailerEnd < 0) {
                return std::nullopt;
            }
            return trailerEnd + 4;
        }
        if (buf.size() < pos + size + 2) {
            return std::nullopt;
        }
        body->append(buf.constData() + pos, size);
        pos += size + 2;
    }
}

ParseResult parse(const QByteArray& buf, Parsed* out) {
    const qsizetype headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (buf.size() > MaxHeaderBytes) {
            out->errorStatus = 431;
            return ParseResult::Error;
        }
        return ParseResult::Incomplete;
    }
    const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() != 3 || !requestLine[2].startsWith("HTTP/1.")) {
        return ParseResult::Error;
    }
    HttpRequest& req = out->request;
    req.method = requestLine[0].toUpper();
    const QByteArray target = requestLine[1];
    const qsizetype q = target.indexOf('?');
    req.path = QUrl::fromPercentEncoding(target.left(q < 0 ? target.size() : q));
    if (q >= 0) {
        req.query = QUrlQuery(QString::fromUtf8(target.mid(q + 1)));
    }
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) {
            continue;
        }
        req.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }

    const QByteArray connection = req.headers.value("connection").toLower();
    out->keepAlive =
        requestLine[2] == "HTTP/1.1" ? connection != "close" : connection == "keep-alive";

    const qsizetype bodyStart = headerEnd + 4;
    if (req.headers.value("transfer-encoding").toLower().contains("chunked")) {
        bool error = false;
        const auto end = decodeChunked(buf, bodyStart, &req.body, &error);
        if (error) {
            out->errorStatus = 413;
            return ParseResult::Error;
        }
        if (!end) {
            req.body.clear();
            return ParseResult::Incomplete;
        }
        out->consumed = *end;
        return ParseResult::Complete;
    }

    qsizetype length = 0;
    if (req.headers.contains("content-length")) {
        bool ok = false;
        length = req.headers.value("content-length").toLongLong(&ok);
        if (!ok || length < 0) {
            return ParseResult::Error;
        }
        if (length > MaxBodyBytes) {
            out->errorStatus = 413;
            return ParseResult::Error;
        }
    }
    if (buf.size() < bodyStart + length) {
        return ParseResult::Incomplete;
    }
    req.body = buf.mid(bodyStart, length);
    out->consumed = bodyStart + length;
    return ParseResult::Complete;
}

} // namespace

HttpServer::HttpServer(Handler handler, QObject* parent)
    : QObject(parent), handler_(std::move(handler)) {
    connect(&server_, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
}

bool HttpServer::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (!server_.listen(address, port)) {
        *error = server_.errorString();
        return false;
    }
    return true;
}

void HttpServer::onNewConnection() {
    while (QTcpSocket* socket = server_.nextPendingConnection()) {
        auto* idle = new QTimer(socket);
        idle->setSingleShot(true);
        idle->setInterval(IdleTimeoutMs);
        connect(idle, &QTimer::timeout, socket, [socket] {
            qCInfo(lcConn).noquote() << "closing idle connection" << peerName(socket) << "after"
                                     << IdleTimeoutMs / 1000 << "s without data";
            socket->disconnectFromHost();
        });
        idle->start();
        const QString peer = peerName(socket);
        socket->setProperty("idleTimer", QVariant::fromValue(static_cast<QObject*>(idle)));
        socket->setProperty("openedAt", QDateTime::currentMSecsSinceEpoch());
        socket->setProperty("requests", 0);
        socket->setProperty("keepalives", 0);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, [socket, peer] {
            const qint64 ms =
                QDateTime::currentMSecsSinceEpoch() - socket->property("openedAt").toLongLong();
            const int requests = socket->property("requests").toInt();
            const int keepalives = socket->property("keepalives").toInt();
            const bool unfinished = !socket->property("buffer").toByteArray().isEmpty();
            if (requests > keepalives || requests == 0 || unfinished) {
                QString line = QStringLiteral("connection from %1 closed after %2 ms, %3 requests")
                                   .arg(peer)
                                   .arg(ms)
                                   .arg(requests);
                if (keepalives > 0) {
                    line += QStringLiteral(" (%1 keepalives)").arg(keepalives);
                }
                if (unfinished) {
                    line += QStringLiteral(" (unfinished request discarded)");
                }
                qCInfo(lcConn).noquote() << line;
            }
            socket->deleteLater();
        });
    }
}

void HttpServer::onReadyRead(QTcpSocket* socket) {
    if (auto* idle = qobject_cast<QTimer*>(socket->property("idleTimer").value<QObject*>())) {
        idle->start();
    }
    socket->setProperty("buffer", socket->property("buffer").toByteArray() + socket->readAll());
    processNext(socket);
}

void HttpServer::processNext(QTcpSocket* socket) {
    if (socket->property("busy").toBool() || socket->property("closing").toBool()) {
        return;
    }
    QByteArray buffer = socket->property("buffer").toByteArray();
    // A TLS ClientHello (0x16 0x03 ..) never completes an HTTP header, so catch it here. The game
    // talks https to EGS (gmServer) and to the OTG hub.
    if (buffer.size() >= 2 && static_cast<unsigned char>(buffer[0]) == 0x16 &&
        static_cast<unsigned char>(buffer[1]) == 0x03) {
        qCWarning(lcConn).noquote()
            << "TLS handshake from" << peerName(socket)
            << "on the plain HTTP port: the client is using https. Put a TLS proxy in front of"
               " this server, or make the client use http.";
        socket->setProperty("buffer", QByteArray());
        socket->setProperty("closing", true);
        socket->disconnectFromHost();
        return;
    }
    Parsed parsed;
    const ParseResult result = parse(buffer, &parsed);
    if (result == ParseResult::Incomplete) {
        return;
    }
    if (result == ParseResult::Error) {
        qCWarning(lcConn).noquote()
            << "bad request from" << peerName(socket) << "->" << parsed.errorStatus
            << reasonPhrase(parsed.errorStatus) << "first bytes:" << buffer.left(64).toHex(' ');
        HttpResponse response;
        response.status = parsed.errorStatus;
        response.contentType = "text/plain";
        response.body = reasonPhrase(parsed.errorStatus);
        socket->setProperty("buffer", QByteArray());
        send(socket, response, false);
        return;
    }
    buffer.remove(0, parsed.consumed);
    socket->setProperty("buffer", buffer);
    socket->setProperty("busy", true);
    socket->setProperty("requests", socket->property("requests").toInt() + 1);
    parsed.request.peer = socket->peerAddress();

    QPointer<QTcpSocket> guard(socket);
    const bool keepAlive = parsed.keepAlive;
    handler_(parsed.request, [this, guard, keepAlive](HttpResponse response) {
        if (!guard) {
            return;
        }
        guard->setProperty("busy", false);
        if (response.keepalive) {
            guard->setProperty("keepalives", guard->property("keepalives").toInt() + 1);
        }
        send(guard, response, keepAlive);
        if (keepAlive) {
            QMetaObject::invokeMethod(
                this,
                [this, guard] {
                    if (guard)
                        processNext(guard);
                },
                Qt::QueuedConnection);
        }
    });
}

void HttpServer::send(QTcpSocket* socket, const HttpResponse& response, bool keepAlive) {
    QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' +
                      reasonPhrase(response.status) + "\r\n";
    head += "Content-Type: " + response.contentType + "\r\n";
    head += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    for (const auto& [key, value] : response.headers) {
        head += key + ": " + value + "\r\n";
    }
    head += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    head += "\r\n";
    socket->write(head + response.body);
    if (!keepAlive) {
        socket->setProperty("closing", true);
        socket->disconnectFromHost();
    }
}
