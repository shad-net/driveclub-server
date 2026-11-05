// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "game_data.h"

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <algorithm>
#include <functional>

Q_LOGGING_CATEGORY(lcGameData, "gamedata")

namespace {

GameData& state() {
    static GameData d;
    return d;
}

QString fromCp1252(const QByteArray& bytes) {
    static const char16_t high[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    QString out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out.append(b >= 0x80 && b < 0xA0 ? QChar(high[b - 0x80]) : QChar(b));
    }
    return out;
}

QStringList csvFields(const QString& line) {
    QStringList fields;
    QString field;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    field.append(c);
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.append(c);
            }
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == QLatin1Char(',')) {
            fields.append(field);
            field.clear();
        } else {
            field.append(c);
        }
    }
    fields.append(field);
    return fields;
}

QStringList readLines(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QStringList lines = fromCp1252(f.readAll()).split(QLatin1Char('\n'));
    for (QString& l : lines) {
        if (l.endsWith(QLatin1Char('\r'))) {
            l.chop(1);
        }
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

QString find(const QString& dir, const QStringList& subs, const QString& name,
             const std::function<bool(const QString&)>& accept = {}) {
    for (const QString& sub : subs) {
        const QString path =
            QDir(dir).filePath(sub.isEmpty() ? name : sub + QLatin1Char('/') + name);
        if (!QFile::exists(path)) {
            continue;
        }
        if (accept) {
            const QStringList lines = readLines(path);
            if (lines.isEmpty() || !accept(lines.first())) {
                continue;
            }
        }
        return path;
    }
    return {};
}

QList<QHash<QString, QString>> table(const QString& path) {
    QList<QHash<QString, QString>> rows;
    const QStringList lines = readLines(path);
    if (lines.isEmpty()) {
        return rows;
    }
    const QStringList header = csvFields(lines.first());
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QStringList fields = csvFields(lines.at(i));
        QHash<QString, QString> row;
        for (qsizetype c = 0; c < header.size() && c < fields.size(); ++c) {
            row.insert(header.at(c), fields.at(c));
        }
        rows.append(row);
    }
    return rows;
}

QList<qint64> levels(const QString& path, QList<int>* caps = nullptr) {
    QList<qint64> out;
    static const QRegularExpression number(QStringLiteral("^\\s*(\\d+)"));
    for (const QString& line : readLines(path)) {
        const auto m = number.match(line);
        if (!m.hasMatch()) {
            continue;
        }
        out.append(m.captured(1).toLongLong());
        if (caps && line.contains(QLatin1String("[cap"))) {
            caps->append(int(out.size()));
        }
    }
    return out;
}

int countAtOrBelow(const QList<qint64>& thresholds, qint64 fame) {
    return int(std::upper_bound(thresholds.begin(), thresholds.end(), fame) - thresholds.begin());
}

} // namespace

void loadGameData(const QString& dir) {
    GameData d;
    d.dir = dir;
    if (dir.isEmpty()) {
        state() = d;
        return;
    }
    const QStringList dataDirs{QString(), QStringLiteral("newdata")};
    const QStringList rankDirs{QStringLiteral("rank"), QStringLiteral("newdata/rank"), QString(),
                               QStringLiteral("newdata")};

    const QString tracksPath = find(dir, dataDirs, QStringLiteral("tracks.csv"));
    for (const auto& r : table(tracksPath)) {
        if (r.value(QStringLiteral("UniqueID")).isEmpty()) {
            continue;
        }
        d.tracks.append({r.value(QStringLiteral("UniqueID")), r.value(QStringLiteral("Track Name")),
                         r.value(QStringLiteral("PRODUCT_NAME_1")),
                         r.value(QStringLiteral("Location")),
                         r.value(QStringLiteral("RouteType"))});
    }

    const QString vehiclesPath =
        find(dir, dataDirs, QStringLiteral("vehicles.csv"),
             [](const QString& header) { return header.contains(QLatin1String("UniqueID")); });
    for (const auto& r : table(vehiclesPath)) {
        if (r.value(QStringLiteral("UniqueID")).isEmpty()) {
            continue;
        }
        d.vehicles.append(
            {r.value(QStringLiteral("UniqueID")), r.value(QStringLiteral("Vehicle Name")),
             r.value(QStringLiteral("PRODUCT_NAME_1")), r.value(QStringLiteral("Class")),
             r.value(QStringLiteral("PlayerUnlockLevel")).toInt(),
             r.value(QStringLiteral("ClubUnlockLevel")).toInt()});
    }

    const QString playerPath = find(dir, rankDirs, QStringLiteral("player.csv"));
    d.playerLevels = levels(playerPath, &d.playerCaps);
    const QString clubPath = find(dir, rankDirs, QStringLiteral("club.csv"));
    d.clubLevels = levels(clubPath);
    const QString elitePath = find(dir, rankDirs, QStringLiteral("elite.csv"));
    d.eliteLevels = levels(elitePath);

    auto note = [&d](const QString& name, const QString& path, qsizetype count) {
        if (path.isEmpty() || count == 0) {
            d.missing << name;
        } else {
            d.loaded << QStringLiteral("%1 (%2)").arg(name).arg(count);
        }
    };
    note(QStringLiteral("tracks.csv"), tracksPath, d.tracks.size());
    note(QStringLiteral("vehicles.csv"), vehiclesPath, d.vehicles.size());
    note(QStringLiteral("rank/player.csv"), playerPath, d.playerLevels.size());
    note(QStringLiteral("rank/club.csv"), clubPath, d.clubLevels.size());
    note(QStringLiteral("rank/elite.csv"), elitePath, d.eliteLevels.size());
    qCInfo(lcGameData).noquote()
        << "game data from" << dir << ":"
        << (d.loaded.isEmpty() ? QStringLiteral("nothing") : d.loaded.join(QStringLiteral(", ")))
        << (d.missing.isEmpty()
                ? QString()
                : QStringLiteral("; not found: %1").arg(d.missing.join(QStringLiteral(", "))));
    state() = d;
}

const GameData& gameData() {
    return state();
}

int driverLevel(qint64 fame) {
    return countAtOrBelow(state().playerLevels, fame);
}

int eliteLevel(qint64 fame) {
    return countAtOrBelow(state().eliteLevels, fame);
}

int clubLevel(qint64 fame) {
    return countAtOrBelow(state().clubLevels, fame);
}
