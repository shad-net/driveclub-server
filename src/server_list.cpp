// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "server_list.h"

#include "upnp.h"
#include "version.h"

#include <QDateTime>
#include <QHash>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>
#include <QUrlQuery>

Q_LOGGING_CATEGORY(lcServers, "servers")

namespace {

constexpr qint64 kListedForMs = 3 * 60 * 1000; // a registration counts this long
constexpr int kRegisterEveryMs = 60 * 1000;
const QByteArray kSearch = QByteArrayLiteral("DCCOOP?");

struct Listed {
    QString name;
    QString address;
    int port = 8080;
    int players = 0;
    qint64 seen = 0;
};

struct State {
    QMutex mutex;
    ServerListConfig config;
    std::function<QStringList()> onlinePlayers;
    QHash<QString, Listed> listed; // "address:port"
};

State& state() {
    static State s;
    return s;
}

QString serverName() {
    const QString name = state().config.name.trimmed();
    return name.isEmpty() ? QHostInfo::localHostName() : name;
}

QStringList online() {
    return state().onlinePlayers ? state().onlinePlayers() : QStringList();
}

void registerOn(const QString& target);

// list_on may name several servers, separated by commas or spaces: this one is listed on each.
void registerOnList() {
    const QStringList targets =
        state().config.listOn.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    for (const QString& target : targets) {
        registerOn(target.contains(QLatin1Char(':')) ? target : target + QStringLiteral(":8080"));
    }
}

void registerOn(const QString& target) {
    QUrl url(QStringLiteral("http://%1/dccoop/servers/register").arg(target));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), serverName());
    q.addQueryItem(QStringLiteral("port"), QString::number(state().config.port));
    q.addQueryItem(QStringLiteral("players"), QString::number(online().size()));
    url.setQuery(q);
    static QNetworkAccessManager* net = new QNetworkAccessManager;
    QNetworkRequest request(url);
    request.setTransferTimeout(5000);
    QNetworkReply* reply = net->get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, target] {
        static QHash<QString, bool> failedBefore; // per list server: logged when it changes
        const bool failed = reply->error() != QNetworkReply::NoError;
        if (!failedBefore.contains(target) || failed != failedBefore.value(target)) {
            if (failed) {
                qCWarning(lcServers).noquote() << "cannot register on the server list at" << target << ":"
                                               << reply->errorString();
            } else {
                qCInfo(lcServers).noquote() << "listed on the server list at" << target;
            }
            failedBefore.insert(target, failed);
        }
        reply->deleteLater();
    });
}

} // namespace

QByteArray serverInfoJson() {
    QJsonArray players;
    for (const QString& p : online()) {
        players.append(p);
    }
    QMutexLocker lock(&state().mutex);
    return QJsonDocument(QJsonObject{{QStringLiteral("name"), serverName()},
                                     {QStringLiteral("version"), Driveclub::Version()},
                                     {QStringLiteral("players"), players},
                                     {QStringLiteral("port"), int(state().config.port)},
                                     {QStringLiteral("internet"), upnpPublicAddress()}})
        .toJson(QJsonDocument::Compact);
}

QByteArray serverListJson() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QStringList here = online();
    QJsonArray out;
    QMutexLocker lock(&state().mutex);
    // this server itself: the browser knows its address already (it asked it)
    out.append(QJsonObject{{QStringLiteral("name"), serverName()},
                           {QStringLiteral("address"), QString()},
                           {QStringLiteral("port"), int(state().config.port)},
                           {QStringLiteral("players"), int(here.size())},
                           {QStringLiteral("seen"), 0}});
    for (auto it = state().listed.begin(); it != state().listed.end();) {
        if (now - it->seen > kListedForMs) {
            qCInfo(lcServers).noquote() << "server" << it->name << "at" << it.key() << "dropped from the list";
            it = state().listed.erase(it);
            continue;
        }
        out.append(QJsonObject{{QStringLiteral("name"), it->name},
                               {QStringLiteral("address"), it->address},
                               {QStringLiteral("port"), it->port},
                               {QStringLiteral("players"), it->players},
                               {QStringLiteral("seen"), (now - it->seen) / 1000}});
        ++it;
    }
    return QJsonDocument(out).toJson(QJsonDocument::Compact);
}

QByteArray serverListRegister(const QString& address, const QString& name, int port, int players) {
    if (address.isEmpty() || port < 1 || port > 65535) {
        return QByteArrayLiteral("{\"is_success\":false}");
    }
    QString shown = name.trimmed().left(40);
    if (shown.isEmpty()) {
        shown = address;
    }
    QMutexLocker lock(&state().mutex);
    const QString key = QStringLiteral("%1:%2").arg(address).arg(port);
    const bool known = state().listed.contains(key);
    state().listed.insert(key, Listed{shown, address, port, std::max(0, players), QDateTime::currentMSecsSinceEpoch()});
    if (!known) {
        qCInfo(lcServers).noquote() << "server" << shown << "at" << key << "listed";
    }
    return QByteArrayLiteral("{\"is_success\":true}");
}

void serverListStart(const ServerListConfig& config, std::function<QStringList()> onlinePlayers) {
    {
        QMutexLocker lock(&state().mutex);
        state().config = config;
        state().onlinePlayers = std::move(onlinePlayers);
    }
    // The network search: a datagram "DCCOOP?" to the server's port (UDP), answered with the info.
    auto* socket = new QUdpSocket;
    if (socket->bind(QHostAddress::AnyIPv4, config.port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        QObject::connect(socket, &QUdpSocket::readyRead, socket, [socket] {
            while (socket->hasPendingDatagrams()) {
                QByteArray datagram(int(socket->pendingDatagramSize()), Qt::Uninitialized);
                QHostAddress from;
                quint16 port = 0;
                socket->readDatagram(datagram.data(), datagram.size(), &from, &port);
                if (datagram.startsWith(kSearch)) {
                    socket->writeDatagram("DCCOOP!" + serverInfoJson(), from, port);
                }
            }
        });
        qInfo("server list: answering network searches on UDP port %u as \"%s\"", unsigned(config.port),
              qPrintable(serverName()));
    } else {
        qWarning("server list: cannot answer network searches on UDP port %u: %s", unsigned(config.port),
                 qPrintable(socket->errorString()));
    }
    if (!config.listOn.trimmed().isEmpty()) {
        auto* timer = new QTimer;
        QObject::connect(timer, &QTimer::timeout, [] { registerOnList(); });
        timer->start(kRegisterEveryMs);
        QTimer::singleShot(2000, [] { registerOnList(); });
    }
}
