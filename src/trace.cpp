// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "trace.h"

#include "egs.h"
#include "proto_dump.h"

#include <QAtomicInteger>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace {

Q_LOGGING_CATEGORY(lcTrace, "trace")

TraceOptions& options() {
    static TraceOptions o;
    return o;
}

QAtomicInteger<quint64> nextId{1};

bool isGameHeader(const QByteArray& name) {
    static const QByteArrayList names = {
        "authorization", "x-sessionkey",    "x-request-signature", "x-response-signature",
        "x-api-version", "x-service-error", "x-service-errorcode", "x-service-errornumber",
    };
    return names.contains(name.toLower());
}

struct Rule {
    QRegularExpression pattern;
    QString label;
};

// Order matters: the first match wins.
const QList<Rule>& rules() {
    static const QList<Rule> list = [] {
        const QList<QPair<const char*, const char*>> raw = {
            // EGS, https://<gmServer>/egs ... ("%s://%s:%d%s/direct%s%s")
            {"^/egs/binary(/|$)", "EGS binary"},
            {"^/egs/messaging(/|$)", "EGS messaging"},
            {"^/egs/direct(/|$)", "EGS direct"},
            {"^/egs/?$", "EGS index"},
            {"^/egs/", "EGS"},
            {"^/messaging(/|$)", "EGS messaging"},
            // OTG hub, login and OTG domain services
            {"^/titles/driveclub/domains/?$", "OTG hub domains"},
            {"^/[^/]+/[^/]+/v\\d+/.+/~leaderboard$", "OTG leaderboard"},
            {"^/[^/]+/[^/]+/v\\d+/.+/~gamestate$", "OTG gamestate"},
            {"^/[^/]+/[^/]+/v\\d+/scores/", "OTG scores"},
            {"^/[^/]+/[^/]+/v\\d+/", "OTG service"},
            {"^/titles/", "OTG hub"},
            {"^/xlogin/driveclub", "OTG login (fake)"},
            {"^/login/driveclub", "OTG login (psn)"},
            {"^/scores/common/dcbikes/", "OTG scores/common bikes"},
            {"^/scores/common/club/", "OTG scores/common club"},
            {"^/scores/common/", "OTG scores/common"},
            {"^/scores/fame/", "OTG scores/fame"},
            {"^/scores/challenges/", "OTG scores/challenges"},
            {"^/scores/faceoffs/", "OTG scores/faceoffs"},
            {"^/scores/tour/", "OTG scores/tour"},
            {"^/scores/dummy", "OTG scores/dummy"},
            {"^/scores(/|$)", "OTG scores"},
            {"^/ghosts(/|$)", "OTG ghosts"},
            {"^/club(/|$)", "OTG club"},
            {"^/player(/|$)", "OTG player"},
            // Telemetry (WWSGA "<url>/games/<id>/telemetry", EvoMessageQueue)
            {"^/games/[^/]+/telemetry", "WWSGA telemetry"},
            {"^/telemetry(/|$)", "telemetry"},
            // PSN web API; normally answered by shadNet, logged here if it lands on this server
            {"^/v1/users/", "PSN users API"},
            {"^/v1/sessions", "PSN sessions API"},
            {"^/v1/profiles", "PSN profiles API"},
            {"^/v1/", "PSN web API"},
        };
        QList<Rule> out;
        for (const auto& [pattern, label] : raw) {
            out.append(
                {QRegularExpression(QString::fromLatin1(pattern)), QString::fromLatin1(label)});
        }
        return out;
    }();
    return list;
}

bool looksLikeJson(const QByteArray& body) {
    const QByteArray t = body.trimmed();
    return t.startsWith('{') || t.startsWith('[');
}

bool isPrintable(const QByteArray& data) {
    return std::all_of(data.begin(), data.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x20 || c == '\n' || c == '\r' || c == '\t';
    });
}

QString egsMethod(const QByteArray& body) {
    if (!looksLikeJson(body)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject o = doc.object();
    QString method = o.value(QStringLiteral("serviceMethod")).toString();
    const QString service = o.value(QStringLiteral("service")).toString();
    if (!service.isEmpty() && !method.isEmpty() && !method.contains('.')) {
        method = service + '.' + method;
    }
    return method;
}

void logLines(quint64 id, const QString& prefix, const QString& text) {
    for (const QString& line : text.split('\n')) {
        qCInfo(lcTrace).noquote() << QStringLiteral("#%1 %2%3").arg(id).arg(prefix, line);
    }
}

void traceBody(quint64 id, const QByteArray& body, const QByteArray& contentType,
               const QString& decoded = {}) {
    if (body.isEmpty()) {
        return;
    }
    QString kind;
    if (contentType.contains("json") || looksLikeJson(body)) {
        kind = QStringLiteral("json");
    } else if (contentType.contains("protobuf") || contentType.contains("octet") ||
               !isPrintable(body.left(512))) {
        kind = QStringLiteral("binary");
    } else {
        kind = QStringLiteral("text");
    }
    qCInfo(lcTrace).noquote()
        << QStringLiteral("#%1   body %2 bytes (%3)").arg(id).arg(body.size()).arg(kind);

    const qsizetype limit = options().bodyLimit;
    if (kind == QLatin1String("json")) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error == QJsonParseError::NoError) {
            QByteArray pretty = doc.toJson(QJsonDocument::Indented).trimmed();
            const bool cut = limit > 0 && pretty.size() > limit;
            if (cut) {
                pretty.truncate(limit);
            }
            logLines(id, QStringLiteral("   | "), QString::fromUtf8(pretty));
            if (cut) {
                qCInfo(lcTrace).noquote()
                    << QStringLiteral("#%1   | ... (cut at %2 bytes)").arg(id).arg(limit);
            }
            return;
        }
        qCInfo(lcTrace).noquote()
            << QStringLiteral("#%1   (not valid JSON: %2)").arg(id).arg(err.errorString());
    }
    if (!decoded.isEmpty()) {
        logLines(id, QStringLiteral("   ~ "), decoded);
    } else if (kind == QLatin1String("binary")) {
        // Unknown protobuf (a method not in egs.proto yet): field numbers only.
        QStringList fields;
        if (dumpProto(limit > 0 ? body.left(limit) : body, &fields) && !fields.isEmpty()) {
            qCInfo(lcTrace).noquote() << QStringLiteral("#%1   protobuf:").arg(id);
            logLines(id, QStringLiteral("   ~ "), fields.join('\n'));
        }
    }
    logLines(id, QStringLiteral("   | "), dumpBody(body, limit));
}

QStringList egsPathParts(const QString& path) {
    for (const QLatin1String prefix :
         {QLatin1String("/egs/binary/"), QLatin1String("/egs/direct/")}) {
        if (path.startsWith(prefix)) {
            return path.mid(prefix.size()).split('/', Qt::SkipEmptyParts);
        }
    }
    return {};
}

} // namespace

void setTraceOptions(const TraceOptions& o) {
    options() = o;
}

const TraceOptions& traceOptions() {
    return options();
}

QString endpointLabel(const HttpRequest& request) {
    for (const Rule& rule : rules()) {
        if (!rule.pattern.match(request.path).hasMatch()) {
            continue;
        }
        const QStringList parts = egsPathParts(request.path);
        if (!parts.isEmpty()) {
            return rule.label + ' ' + parts.join('.');
        }
        return rule.label;
    }
    return {};
}

QString dumpBody(const QByteArray& body, qsizetype limit) {
    const QByteArray data = limit > 0 ? body.left(limit) : body;
    QString out;
    if (isPrintable(data)) {
        out = QString::fromUtf8(data);
    } else {
        QStringList lines;
        for (qsizetype off = 0; off < data.size(); off += 16) {
            const QByteArray row = data.mid(off, 16);
            QString hex = QString::fromLatin1(row.toHex(' '));
            hex = hex.leftJustified(16 * 3 - 1, ' ');
            QString ascii;
            for (char c : row) {
                const auto u = static_cast<unsigned char>(c);
                ascii += (u >= 0x20 && u < 0x7f) ? QChar(c) : QChar('.');
            }
            lines << QStringLiteral("%1  %2  %3").arg(off, 6, 16, QChar('0')).arg(hex, ascii);
        }
        out = lines.join('\n');
    }
    if (body.size() > data.size()) {
        out += QStringLiteral("\n... (%1 more bytes)").arg(body.size() - data.size());
    }
    return out;
}

quint64 traceRequest(const HttpRequest& request, const QString& label) {
    const quint64 id = nextId.fetchAndAddRelaxed(1);
    const QString query = request.query.isEmpty() ? QString() : '?' + request.query.toString();
    const QString name = label.isEmpty() ? QStringLiteral("UNKNOWN endpoint") : label;
    qCInfo(lcTrace).noquote() << QStringLiteral("#%1 >>> %2 %3%4  [%5]  from %6")
                                     .arg(id)
                                     .arg(QString::fromLatin1(request.method), request.path, query,
                                          name, request.peer.toString());
    if (!options().enabled) {
        return id;
    }

    QList<QByteArray> keys = request.headers.keys();
    std::sort(keys.begin(), keys.end());
    for (const QByteArray& key : keys) {
        qCInfo(lcTrace).noquote() << QStringLiteral("#%1   %2 %3: %4")
                                         .arg(id)
                                         .arg(isGameHeader(key) ? '*' : ' ')
                                         .arg(QString::fromLatin1(key),
                                              QString::fromLatin1(request.headers.value(key)));
    }
    for (const auto& [k, v] : request.query.queryItems(QUrl::FullyDecoded)) {
        qCInfo(lcTrace).noquote() << QStringLiteral("#%1   query %2 = %3").arg(id).arg(k, v);
    }
    const QString method = egsMethod(request.body);
    if (!method.isEmpty()) {
        qCInfo(lcTrace).noquote()
            << QStringLiteral("#%1   EGS serviceMethod: %2").arg(id).arg(method);
    }
    const QByteArray signature = request.headers.value("x-request-signature");
    const QStringList egs = egsPathParts(request.path);
    if (!signature.isEmpty() && egs.size() >= 2) {
        const auto key = egsFindSessionKey(egs.at(1), request.body, signature);
        if (!key) {
            qCWarning(lcTrace).noquote()
                << QStringLiteral("#%1   signature MISMATCH for method \"%2\": no session key "
                                  "issued by this server fits (server restarted?)")
                       .arg(id)
                       .arg(egs.at(1));
        } else if (key->isEmpty()) {
            qCInfo(lcTrace).noquote()
                << QStringLiteral("#%1   signature ok (method \"%2\", no session)")
                       .arg(id)
                       .arg(egs.at(1));
        } else {
            qCInfo(lcTrace).noquote()
                << QStringLiteral("#%1   signature ok (method \"%2\", session of %3)")
                       .arg(id)
                       .arg(egs.at(1), egsSessionName(*key));
        }
    }
    traceBody(id, request.body, request.headers.value("content-type").toLower(),
              request.body.isEmpty() ? QString()
                                     : egsDescribeBody(request.path, false, 0, request.body));
    return id;
}

void traceResponse(quint64 id, const QString& label, const HttpRequest& request,
                   const HttpResponse& response, qint64 elapsedMs) {
    const QString name = label.isEmpty() ? QStringLiteral("UNKNOWN endpoint") : label;
    const auto line = QStringLiteral("#%1 <<< %2  [%3]  %4 bytes  %5 ms")
                          .arg(id)
                          .arg(response.status)
                          .arg(name)
                          .arg(response.body.size())
                          .arg(elapsedMs);
    if (response.status >= 400) {
        qCWarning(lcTrace).noquote() << line;
    } else {
        qCInfo(lcTrace).noquote() << line;
    }
    if (!options().enabled) {
        return;
    }
    qCInfo(lcTrace).noquote() << QStringLiteral("#%1     content-type: %2")
                                     .arg(id)
                                     .arg(QString::fromLatin1(response.contentType));
    for (const auto& [key, value] : response.headers) {
        qCInfo(lcTrace).noquote() << QStringLiteral("#%1   %2 %3: %4")
                                         .arg(id)
                                         .arg(isGameHeader(key) ? '*' : ' ')
                                         .arg(QString::fromLatin1(key), QString::fromLatin1(value));
    }
    traceBody(id, response.body, response.contentType.toLower(),
              response.body.isEmpty()
                  ? QString()
                  : egsDescribeBody(request.path, true, response.status, response.body));
}
