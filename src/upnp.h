// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Reachable from the internet (internet.upnp): the router is asked over UPnP (IGD) to forward the server's port
// (TCP and UDP) to this PC, renewed every half hour and taken back when the server stops; its public address is
// learned at the same time. Most home routers do it when allowed (FRITZ!Box: the device's "Permit independent port
// sharing"). A router without a public IPv4 address (DS-Lite, carrier-grade NAT) cannot be reached this way.

#pragma once

#include <QString>

void upnpStart(quint16 port, const QString& statusFile);
// The router's public IPv4 address once the forwarding is in place, empty before or without it.
QString upnpPublicAddress();
// An address on the internet, not a home network, VPN (Tailscale, Hamachi, Radmin) or loopback one.
bool isInternetAddress(quint32 ipv4);
