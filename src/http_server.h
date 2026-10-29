// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTcpServer>
#include <QUrlQuery>

#include <functional>

class QTcpSocket;

struct HttpRequest {
    QByteArray method;
    QString path;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
    QHostAddress peer;
    QHostAddress local; // this server's address on the connection (LAN or VPN address)
};

struct HttpResponse {
    int status = 200;
    QByteArray contentType = QByteArrayLiteral("application/octet-stream");
    // Extra headers, e.g. EGS's X-Response-Signature or X-Service-Error.
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    // An error status that is a normal part of the protocol
    bool expected = false;
    // An empty answer to a push-channel poll (/egs/messaging keepalive).
    bool keepalive = false;
    // Set for a response that stays open (EGS messaging): the server sends the head with chunked
    // transfer encoding and `body` as the first chunk, then calls this with a writer for later
    // chunks. The writer returns false once the connection is gone; the connection is not reused.
    std::function<void(std::function<bool(const QByteArray&)>)> stream;
};

// Small HTTP/1.1 server: keep-alive, Content-Length and chunked request bodies.
class HttpServer : public QObject {
    Q_OBJECT
public:
    using Responder = std::function<void(HttpResponse)>;
    using Handler = std::function<void(const HttpRequest&, Responder)>;

    explicit HttpServer(Handler handler, QObject* parent = nullptr);

    bool listen(const QHostAddress& address, quint16 port, QString* error);

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* socket);
    void processNext(QTcpSocket* socket);
    void send(QTcpSocket* socket, const HttpResponse& response, bool keepAlive);

    Handler handler_;
    QTcpServer server_;
};
