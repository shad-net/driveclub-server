// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "telemetry.h"

#include <QLoggingCategory>
#include <QTcpSocket>

Q_LOGGING_CATEGORY(lcTelemetry, "telemetry")

namespace {

// A message body longer than this is cut in the log.
constexpr qsizetype kMaxLoggedBody = 8192;
// A connection that buffers more than this without completing a frame is dropped.
constexpr qsizetype kMaxFrame = 1024 * 1024;

QByteArray unescapeHeader(const QByteArray& v) {
    // STOMP 1.2 header escapes: \r \n \c (colon) \\ .
    if (!v.contains('\\')) {
        return v;
    }
    QByteArray out;
    out.reserve(v.size());
    for (qsizetype i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            const char c = v[++i];
            out.append(c == 'n' ? '\n' : c == 'r' ? '\r' : c == 'c' ? ':' : c);
        } else {
            out.append(v[i]);
        }
    }
    return out;
}

QString who(const QTcpSocket* s) {
    return QStringLiteral("%1:%2").arg(s->peerAddress().toString()).arg(s->peerPort());
}

} // namespace

TelemetryCollector::TelemetryCollector(QObject* parent) : QObject(parent) {
    connect(&server_, &QTcpServer::newConnection, this, &TelemetryCollector::onConnection);
}

bool TelemetryCollector::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (!server_.listen(address, port)) {
        *error = server_.errorString();
        return false;
    }
    return true;
}

void TelemetryCollector::onConnection() {
    while (QTcpSocket* s = server_.nextPendingConnection()) {
        qCInfo(lcTelemetry).noquote() << "connection from" << who(s);
        buffers_.insert(s, {});
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            qCInfo(lcTelemetry).noquote() << "connection from" << who(s) << "closed";
            buffers_.remove(s);
            s->deleteLater();
        });
    }
}

void TelemetryCollector::onReadyRead(QTcpSocket* socket) {
    QByteArray& buf = buffers_[socket];
    buf.append(socket->readAll());
    if (buf.startsWith('\x16')) {
        qCWarning(lcTelemetry).noquote()
            << who(socket) << "sends TLS; the collector speaks plain STOMP only";
        socket->abort();
        return;
    }
    for (;;) {
        // Heart-beats (and frame separators) are bare end-of-lines.
        qsizetype start = 0;
        while (start < buf.size() && (buf[start] == '\n' || buf[start] == '\r')) {
            ++start;
        }
        buf.remove(0, start);
        const qsizetype headerEnd = buf.indexOf("\n\n");
        const qsizetype headerEndCr = buf.indexOf("\r\n\r\n");
        qsizetype sepPos = -1;
        qsizetype sepLen = 0;
        if (headerEndCr >= 0 && (headerEnd < 0 || headerEndCr < headerEnd)) {
            sepPos = headerEndCr;
            sepLen = 4;
        } else if (headerEnd >= 0) {
            sepPos = headerEnd;
            sepLen = 2;
        }
        if (sepPos < 0) {
            break;
        }
        QList<QByteArray> lines = buf.left(sepPos).split('\n');
        for (QByteArray& l : lines) {
            if (l.endsWith('\r')) {
                l.chop(1);
            }
        }
        const QByteArray command = lines.value(0).trimmed();
        QHash<QByteArray, QByteArray> headers;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0 && !headers.contains(lines[i].left(colon))) {
                headers.insert(lines[i].left(colon), unescapeHeader(lines[i].mid(colon + 1)));
            }
        }
        const qsizetype bodyStart = sepPos + sepLen;
        qsizetype bodyEnd = -1;
        bool ok = false;
        const qsizetype length = headers.value("content-length").toLongLong(&ok);
        if (ok && length >= 0) {
            if (buf.size() < bodyStart + length + 1) {
                break;
            }
            bodyEnd = bodyStart + length;
        } else {
            bodyEnd = buf.indexOf('\0', bodyStart);
            if (bodyEnd < 0) {
                break;
            }
        }
        const QByteArray body = buf.mid(bodyStart, bodyEnd - bodyStart);
        buf.remove(0, bodyEnd + 1);
        if (!handleFrame(socket, command, headers, body)) {
            socket->disconnectFromHost();
            return;
        }
    }
    if (buf.size() > kMaxFrame) {
        qCWarning(lcTelemetry).noquote() << who(socket) << "frame too large, dropping connection";
        socket->abort();
    }
}

bool TelemetryCollector::handleFrame(QTcpSocket* socket, const QByteArray& command,
                                     const QHash<QByteArray, QByteArray>& headers,
                                     const QByteArray& body) {
    const auto receipt = [&] {
        if (headers.contains("receipt")) {
            socket->write("RECEIPT\nreceipt-id:" + headers.value("receipt") + "\n\n" +
                          QByteArray(1, '\0'));
        }
    };
    if (command == "CONNECT" || command == "STOMP") {
        qCInfo(lcTelemetry).noquote()
            << who(socket) << "STOMP connect: host" << headers.value("host") << ", login"
            << headers.value("login") << ", versions" << headers.value("accept-version")
            << ", heart-beat" << headers.value("heart-beat");
        // heart-beat 0,0: no heart-beats either way, so idle connections are never dropped.
        socket->write("CONNECTED\nversion:1.2\nheart-beat:0,0\nserver:driveclub-server\n\n" +
                      QByteArray(1, '\0'));
        return true;
    }
    if (command == "SEND") {
        QByteArray shown = body;
        if (shown.size() > kMaxLoggedBody) {
            shown =
                shown.left(kMaxLoggedBody) + "... (" + QByteArray::number(body.size()) + " bytes)";
        }
        qCInfo(lcTelemetry).noquote()
            << who(socket) << headers.value("destination") << ":" << QString::fromUtf8(shown);
        receipt();
        return true;
    }
    if (command == "DISCONNECT") {
        receipt();
        socket->flush();
        return false;
    }
    if (command == "SUBSCRIBE" || command == "UNSUBSCRIBE" || command == "ACK" ||
        command == "NACK" || command == "BEGIN" || command == "COMMIT" || command == "ABORT") {
        receipt();
        return true;
    }
    qCWarning(lcTelemetry).noquote() << who(socket) << "unknown STOMP frame" << command;
    socket->write("ERROR\nmessage:unknown frame\n\n" + QByteArray(1, '\0'));
    return false;
}
