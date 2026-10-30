// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The server list (the launcher's server browser). Every server tells about itself (dccoop/info) and answers a
// search on the local network and VPN (a UDP datagram "DCCOOP?" to its port, answered with the same info). Every
// server can also keep a list of others (dccoop/servers): a server with server.list_on set to another server's
// address registers there every minute and drops out of its list a few minutes after it stops.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

struct ServerListConfig {
    QString name;   // server.name: what the browser shows; empty = this computer's name
    QString listOn; // server.list_on: "host[:port]" of a server to be listed on, empty = none
    quint16 port = 8080;
};

// Starts the network search answer and the registration; onlinePlayers names who is playing now.
void serverListStart(const ServerListConfig& config, std::function<QStringList()> onlinePlayers);
// dccoop/info: {"name", "version", "players": [...], "port"}.
QByteArray serverInfoJson();
// dccoop/servers: the servers listed here, this one first: [{"name", "address", "port", "players", "seen"}].
QByteArray serverListJson();
// dccoop/servers/register?name=&port=&players=: a server lists itself here; its address is where it asks from.
QByteArray serverListRegister(const QString& address, const QString& name, int port, int players);
