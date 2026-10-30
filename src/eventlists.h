// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Online events (the game's season of event lists). Each event list is a championship: its races
// run one after the other in the same lobby. They come from eventlists.json next to the server,
// which is written with a few examples when missing.

#pragma once

#include <QList>
#include <QString>

#include <optional>

class GetCurrentSeasonResponse;
class QueryUpcomingMatchSlotsRequest;
class QueryUpcomingMatchSlotsResponse;

// Reads eventlists.json (writes the examples first when it is missing); false and an error when it
// does not parse, the examples are used then.
bool eventListsLoad(const QString& path, QString* error);

void eventListsSeason(GetCurrentSeasonResponse& out);
void eventListsUpcoming(const QueryUpcomingMatchSlotsRequest& req, qint64 accountId,
                        QueryUpcomingMatchSlotsResponse& out);

struct EventSlot {
    qint64 id = 0;
    qint64 eventList = 0;
    qint64 matchTime = 0; // ms since epoch
    int races = 1;        // templates of the event list
    QList<qint64> registered;
};

// Registers accounts for a slot handed out by eventListsUpcoming; nullopt for an unknown slot.
std::optional<EventSlot> eventListsRegister(qint64 slotId, const QList<qint64>& accounts);
void eventListsUnregister(qint64 accountId);
QList<EventSlot> eventListsRegisteredSlots(qint64 accountId);
int eventListsPlayers(qint64 slotId);
int eventListsActivePlayers(qint64 eventList);
// Slots whose start has come, with the accounts registered for them; they are forgotten.
QList<EventSlot> eventListsTakeDue(qint64 nowMs);
// The slot that runs the next race of the slot's event list (same list, next template).
qint64 eventListsNextRace(qint64 slotId);

// Championships in a private lobby: the host sets up each race in the lobby and adds it (its
// settings block, hex); while it runs, the host's runtime writes each race's block back into the
// lobby, and a race counts as done when the host's stats arrive after it.
// command: "" (state only), "add" (block = the lobby's settings), "toggle" (start or stop).
// Answer: key=value lines (seq, active, note, block while a race is on).
QByteArray eventListsChampionship(const QString& player, const QString& command,
                                  const QByteArray& block);
void eventListsRaceFinished(const QString& player);
