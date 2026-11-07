// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

// OTG: Leaderboards: reading and posting scores, challenge lists.

#include "otg/internal.h"

namespace otg {

QJsonValue scoreJson(quint64 score) {
    if (score <= quint64(std::numeric_limits<qint64>::max())) {
        return QJsonValue(qint64(score));
    }
    return QJsonValue(double(score));
}

QJsonObject entryJson(const Entry& e, qsizetype position) {
    QJsonObject o{
        {QStringLiteral("position"), qint64(position)},
        {QStringLiteral("score"), scoreJson(e.score)},
        {QStringLiteral("entrant"), QJsonObject{{QStringLiteral("id"), e.entrant}}},
    };
    const bool empty = e.attachment.isEmpty() ||
                       (e.attachment.size() == 1 && e.attachment.first().toString().isEmpty());
    if (!empty) {
        o.insert(QStringLiteral("attachment"), e.attachment);
    }
    return o;
}

QString boardName(QString resource) {
    if (resource.endsWith(QLatin1String("/~leaderboard"))) {
        resource.chop(13);
    }
    if (resource.startsWith(QLatin1String("scores/"))) {
        resource.remove(0, 7);
    }
    return resource;
}

HttpResponse readBoard(const HttpRequest& request, const QString& name) {
    const QString method = request.query.queryItemValue(QStringLiteral("_method"));
    if (method.isEmpty()) {
        const qsizetype size = db::boardSize(name);
        qCInfo(lcOtg).noquote() << "leaderboard" << name << "size ->" << size;
        return json(QJsonDocument(QJsonObject{{QStringLiteral("size"), qint64(size)}}));
    }
    const int count =
        std::clamp(request.query.queryItemValue(QStringLiteral("count")).toInt(), 0, 500);
    const QList<Entry> list = db::boardScores(name);
    QJsonArray out;
    QByteArray nextToken;

    if (method == QLatin1String("select")) {
        QStringList wanted =
            request.query.queryItemValue(QStringLiteral("entrants")).split(',', Qt::SkipEmptyParts);
        const bool special = wanted.removeAll(IAmSpecial) > 0;
        const QString me = special ? callerEntrant(request, name) : QString();
        for (qsizetype i = 0; i < list.size(); ++i) {
            if ((!me.isEmpty() && list.at(i).entrant == me) ||
                wanted.contains(list.at(i).entrant)) {
                out.append(entryJson(list.at(i), i + 1));
            }
        }
    } else if (method == QLatin1String("nearby")) {
        QString who = request.query.queryItemValue(QStringLiteral("entrant"));
        if (who == IAmSpecial) {
            who = callerEntrant(request, name);
        }
        const int n = count > 0 ? count : 10;
        qsizetype at = -1;
        for (qsizetype i = 0; i < list.size(); ++i) {
            if (list.at(i).entrant == who) {
                at = i;
                break;
            }
        }
        if (at >= 0) {
            const qsizetype from = std::max<qsizetype>(0, at - n / 2);
            for (qsizetype i = from; i < list.size() && i < from + n; ++i) {
                out.append(entryJson(list.at(i), i + 1));
            }
        }
    } else {
        // page (default): count entries from offset <from>
        const int n = count > 0 ? count : 50;
        const qsizetype from =
            std::max(0, request.query.queryItemValue(QStringLiteral("from")).toInt());
        for (qsizetype i = from; i < list.size() && i < from + n; ++i) {
            out.append(entryJson(list.at(i), i + 1));
        }
        if (from + n < list.size()) {
            nextToken = QByteArray::number(from + n);
        }
    }

    qCInfo(lcOtg).noquote() << "leaderboard" << name << method << "->" << out.size() << "of"
                            << list.size() << "entries";
    HttpResponse r = json(QJsonDocument(out));
    r.headers.append(qMakePair(QByteArray("X-Count"), QByteArray::number(list.size())));
    if (!nextToken.isEmpty()) {
        r.headers.append(qMakePair(QByteArray("X-Next-Token"), nextToken));
    }
    return r;
}

bool isChallengeList(const QString& resource) {
    return resource.startsWith(QLatin1String("challenges/player/")) ||
           resource.startsWith(QLatin1String("challenges/club/"));
}

HttpResponse writeBoard(const HttpRequest& request, const QString& name, bool latestWins) {
    const QJsonDocument doc = QJsonDocument::fromJson(request.body);
    const QJsonArray items = doc.isArray() ? doc.array() : QJsonArray{doc.object()};
    for (const QJsonValue& v : items) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.board = name;
        e.entrant = o.value(QStringLiteral("entrant")).isObject()
                        ? o.value(QStringLiteral("entrant")).toObject().value("id").toString()
                        : o.value(QStringLiteral("entrant")).toVariant().toString();
        e.score = o.value(QStringLiteral("score")).toVariant().toULongLong();
        e.attachment = o.value(QStringLiteral("attachment")).toArray();
        e.updated = QDateTime::currentMSecsSinceEpoch();
        if (e.entrant == IAmSpecial) {
            e.entrant = callerEntrant(request, name);
            if (e.entrant.isEmpty()) {
                qCWarning(lcOtg).noquote() << "score" << name
                                           << "for i_am_special from an unknown "
                                              "token: not stored";
                continue;
            }
        }
        if (e.entrant.isEmpty()) {
            continue;
        }
        const auto old = db::score(name, e.entrant);
        const bool better = latestWins || !old || e.score > old->score;
        if (better) {
            db::putScore(e);
        }
        if (latestWins) {
            const quint64 ends = e.score >> 8;
            qCInfo(lcOtg).noquote()
                << "challenge" << e.entrant << (old ? "updated on" : "added to") << name
                << QStringLiteral("(ends %1 game time, flags %2)")
                       .arg(QDateTime::fromMSecsSinceEpoch(qint64(ends), QTimeZone::utc())
                                .toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")))
                       .arg(e.score & 0xff);
        } else {
            qCInfo(lcOtg).noquote()
                << "score" << name << "entrant" << e.entrant << "score" << e.score
                << (better                  ? QStringLiteral("(stored)")
                    : old->score == e.score ? QStringLiteral("(same as stored)")
                                            : QStringLiteral("(kept better %1)").arg(old->score));
        }
    }
    return json(QJsonDocument(QJsonObject{{QStringLiteral("is_success"), true}}));
}

} // namespace otg
