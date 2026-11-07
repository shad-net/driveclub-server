// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

struct GameTrack {
    QString id;       // UniqueID, e.g. "74"
    QString code;     // e.g. "canada_road_circuit_02_r"
    QString name;     // e.g. "Fraser Valley - Reversed"
    QString location; // e.g. "Canada"
    QString routeType;
};

struct GameVehicle {
    QString id;           // UniqueID, e.g. "52" (the "veh:<id>" of score attachments)
    QString code;         // e.g. "ren_alp"
    QString name;         // e.g. "Renault Alpine A110-50"
    QString vehicleClass; // Hot Hatch, Sports, Performance, Super, Hyper, Bonus, Superbike
    int playerUnlockLevel = 0;
    int clubUnlockLevel = 0; // 0 = not unlocked by club level
};

struct GameData {
    QString dir;         // the folder read, empty = none
    QStringList loaded;  // files read, e.g. "tracks.csv (120)"
    QStringList missing; // files not found or not readable
    QList<GameTrack> tracks;
    QList<GameVehicle> vehicles;
    QList<qint64> playerLevels; // fame for level i+1 (first is 0)
    QList<int> playerCaps;      // levels marked [capN], e.g. 50, 55, 60
    QList<qint64> clubLevels;   // fame for club level i+1
    QList<qint64> eliteLevels;  // fame for elite level i+1
};

void loadGameData(const QString& dir);
const GameData& gameData();
int driverLevel(qint64 fame);
int eliteLevel(qint64 fame);
int clubLevel(qint64 fame);
