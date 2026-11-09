// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTcpServer>

class QTcpSocket;

// Collector for the game's own telemetry (eboot "[MQ] Stomp::Connection"): with
// otg_telemetry_enabled and the telem_* keys in the TSS, the game opens a plain STOMP 1.x
// connection to telem_messageServer:telem_port and SENDs every telemetry message (JSON). We only
// log them: they are the game's view of what happened, e.g. the "NozomiRoom" messages with the
// race room join result (room_type RoomJoin2, dcrj_result) and peer mesh results (mesh_result),
// which the server cannot see otherwise. See docs/TSS.md, "Telemetry".
class TelemetryCollector : public QObject {
public:
    explicit TelemetryCollector(QObject* parent = nullptr);
    bool listen(const QHostAddress& address, quint16 port, QString* error);

private:
    void onConnection();
    void onReadyRead(QTcpSocket* socket);
    // Handles one complete frame; false closes the connection.
    bool handleFrame(QTcpSocket* socket, const QByteArray& command,
                     const QHash<QByteArray, QByteArray>& headers, const QByteArray& body);

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
};
