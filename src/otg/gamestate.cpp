// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// OTG: Counter gamestates (stats, challenge timers).

#include "otg/internal.h"

namespace otg {

HttpResponse gameStateJson(const db::GameState& g) {
    return json(QJsonDocument(QJsonObject{
        {QStringLiteral("version"), g.version},
        {QStringLiteral("counts"), g.counts},
    }));
}

HttpResponse handleGameState(const HttpRequest& request, const QString& resource) {
    if (request.method == "GET") {
        const auto g = db::gameState(resource);
        if (!g) {
            qCInfo(lcOtg).noquote()
                << "gamestate" << resource << "-> none yet (404, game defaults)";
            HttpResponse r = json(QJsonDocument(QJsonObject{}), 404);
            r.expected = true; // "nothing stored yet": the game uses its defaults
            return r;
        }
        qCInfo(lcOtg).noquote() << "gamestate" << resource << "-> version" << g->version << "with"
                                << g->counts.size() << "counters";
        return gameStateJson(*g);
    }
    if (request.method != "POST" && request.method != "PUT") {
        return json(QJsonDocument(QJsonObject{{QStringLiteral("error"), QStringLiteral("method")}}),
                    405);
    }
    const QJsonObject body = QJsonDocument::fromJson(request.body).object();
    db::GameState g = db::gameState(resource).value_or(db::GameState{resource, 0, {}, 0});
    const qint64 parent = body.value(QStringLiteral("parent")).toInteger(-1);
    if (parent != g.version) {
        qCInfo(lcOtg).noquote() << "gamestate" << resource << "write based on version" << parent
                                << "but stored is" << g.version << "(applied anyway)";
    }
    QStringList changes;
    for (const QJsonValue& t : body.value(QStringLiteral("transactions")).toArray()) {
        const QJsonObject deltas = t.toObject().value(QStringLiteral("deltas")).toObject();
        for (auto it = deltas.constBegin(); it != deltas.constEnd(); ++it) {
            const qint64 delta = it.value().toInteger();
            const qint64 value = std::max<qint64>(0, g.counts.value(it.key()).toInteger() + delta);
            g.counts.insert(it.key(), value);
            changes << QStringLiteral("%1 %2%3 = %4")
                           .arg(it.key(), delta >= 0 ? QStringLiteral("+") : QString())
                           .arg(delta)
                           .arg(value);
        }
    }
    ++g.version;
    g.updated = QDateTime::currentMSecsSinceEpoch();
    db::putGameState(g);
    qCInfo(lcOtg).noquote() << "gamestate" << resource << "-> version" << g.version << ":"
                            << (changes.isEmpty() ? QStringLiteral("no changes")
                                                  : changes.join(QStringLiteral(", ")));
    return gameStateJson(g);
}

} // namespace otg
