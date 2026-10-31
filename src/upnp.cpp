// SPDX-FileCopyrightText: Copyright 2026 Driveclub Custom Server Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "upnp.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include <atomic>
#include <optional>
#include <thread>

Q_LOGGING_CATEGORY(lcUpnp, "upnp")

namespace {

constexpr int kLeaseSeconds = 3600;
constexpr int kRenewSeconds = 1800;

QMutex& mutex() {
    static QMutex m;
    return m;
}
QString& publicAddress() {
    static QString s;
    return s;
}

struct Gateway {
    QUrl control;    // the WAN connection service's control URL
    QString service; // its service type
    QHostAddress local; // this PC's address towards the router
};

// An HTTP request in this thread, waiting for the answer (at most timeoutMs).
QByteArray request(QNetworkAccessManager& net, const QUrl& url, const QByteArray& soapAction = {},
                   const QByteArray& body = {}, int* status = nullptr, int timeoutMs = 4000) {
    QNetworkRequest r(url);
    r.setTransferTimeout(timeoutMs);
    QNetworkReply* reply;
    if (soapAction.isEmpty()) {
        reply = net.get(r);
    } else {
        r.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("text/xml; charset=\"utf-8\""));
        r.setRawHeader("SOAPAction", '"' + soapAction + '"');
        reply = net.post(r, body);
    }
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (status) {
        *status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }
    const QByteArray out = reply->readAll();
    reply->deleteLater();
    return out;
}

// Where the routers describe themselves: SSDP answers, else the usual places on each network's .1 (FRITZ!Box
// :49000/igddesc.xml, miniupnpd :5000/rootDesc.xml).
QList<QUrl> descriptionUrls() {
    QList<QUrl> out;
    QUdpSocket socket;
    if (socket.bind(QHostAddress::AnyIPv4, 0)) {
        const QByteArray search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                                  "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
        socket.writeDatagram(search, QHostAddress(QStringLiteral("239.255.255.250")), 1900);
        const qint64 until = QDateTime::currentMSecsSinceEpoch() + 2500;
        while (QDateTime::currentMSecsSinceEpoch() < until) {
            if (!socket.waitForReadyRead(200)) {
                continue;
            }
            while (socket.hasPendingDatagrams()) {
                QByteArray d(int(socket.pendingDatagramSize()), Qt::Uninitialized);
                socket.readDatagram(d.data(), d.size());
                const auto m = QRegularExpression(QStringLiteral("(?im)^location:\\s*(\\S+)")).match(QString::fromLatin1(d));
                if (m.hasMatch() && !out.contains(QUrl(m.captured(1)))) {
                    out << QUrl(m.captured(1));
                }
            }
        }
    }
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        if (!(i.flags() & QNetworkInterface::IsUp) || (i.flags() & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& e : i.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol || e.prefixLength() < 8 || e.prefixLength() > 30) {
                continue;
            }
            const quint32 router = (e.ip().toIPv4Address() & e.netmask().toIPv4Address()) | 1;
            const QString at = QHostAddress(router).toString();
            out << QUrl(QStringLiteral("http://%1:49000/igddesc.xml").arg(at))
                << QUrl(QStringLiteral("http://%1:5000/rootDesc.xml").arg(at));
        }
    }
    return out;
}

// This PC's address on the router's network.
QHostAddress localTowards(const QHostAddress& router) {
    for (const QNetworkInterface& i : QNetworkInterface::allInterfaces()) {
        for (const QNetworkAddressEntry& e : i.addressEntries()) {
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && e.prefixLength() > 0 &&
                e.ip().isInSubnet(router, e.prefixLength())) {
                return e.ip();
            }
        }
    }
    return {};
}

std::optional<Gateway> findGateway(QNetworkAccessManager& net) {
    for (const QUrl& url : descriptionUrls()) {
        int status = 0;
        const QString xml = QString::fromUtf8(request(net, url, {}, {}, &status, 2500));
        if (status != 200 || xml.isEmpty()) {
            continue;
        }
        // the first WAN IP (or PPP) connection service and its control URL
        static const QRegularExpression service(QStringLiteral(
            "<serviceType>(urn:schemas-upnp-org:service:WAN(?:IP|PPP)Connection:\\d)</serviceType>.*?<controlURL>([^<]+)</controlURL>"),
            QRegularExpression::DotMatchesEverythingOption);
        const auto m = service.match(xml);
        if (!m.hasMatch()) {
            continue;
        }
        Gateway g;
        g.service = m.captured(1);
        g.control = url.resolved(QUrl(m.captured(2).trimmed()));
        g.local = localTowards(QHostAddress(url.host()));
        if (g.local.isNull()) {
            continue;
        }
        qCInfo(lcUpnp).noquote() << "router" << url.host() << "(" + g.service + ")";
        return g;
    }
    return std::nullopt;
}

// A SOAP call; the answer's body, the UPnP error code in *error (0: none).
QString soap(QNetworkAccessManager& net, const Gateway& g, const QString& action, const QString& args, int* error) {
    const QString body = QStringLiteral(
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%1 xmlns:u=\"%2\">%3</u:%1>"
        "</s:Body></s:Envelope>").arg(action, g.service, args);
    int status = 0;
    const QString answer = QString::fromUtf8(
        request(net, g.control, (g.service + '#' + action).toUtf8(), body.toUtf8(), &status));
    const auto code = QRegularExpression(QStringLiteral("<errorCode>(\\d+)</errorCode>")).match(answer);
    *error = status == 200 ? 0 : code.hasMatch() ? code.captured(1).toInt() : -1;
    return answer;
}

bool mapPort(QNetworkAccessManager& net, const Gateway& g, quint16 port, const char* protocol) {
    const auto args = [&](int lease) {
        return QStringLiteral(
                   "<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort><NewProtocol>%2</NewProtocol>"
                   "<NewInternalPort>%1</NewInternalPort><NewInternalClient>%3</NewInternalClient><NewEnabled>1</NewEnabled>"
                   "<NewPortMappingDescription>DriveClub server</NewPortMappingDescription><NewLeaseDuration>%4</NewLeaseDuration>")
            .arg(port)
            .arg(QLatin1String(protocol), g.local.toString())
            .arg(lease);
    };
    int error = 0;
    soap(net, g, QStringLiteral("AddPortMapping"), args(kLeaseSeconds), &error);
    if (error == 725) { // OnlyPermanentLeasesSupported
        soap(net, g, QStringLiteral("AddPortMapping"), args(0), &error);
    }
    if (error) {
        qCWarning(lcUpnp).noquote()
            << "the router did not forward" << protocol << port << "( error" << error << ")"
            << (error == 606 || error == 718 || error == 401
                    ? "- allow it in the router: FRITZ!Box: Internet, Permit Access, this PC, \"Permit independent port sharing\""
                    : "");
        return false;
    }
    return true;
}

void unmapPort(QNetworkAccessManager& net, const Gateway& g, quint16 port, const char* protocol) {
    int error = 0;
    soap(net, g, QStringLiteral("DeletePortMapping"),
         QStringLiteral("<NewRemoteHost></NewRemoteHost><NewExternalPort>%1</NewExternalPort><NewProtocol>%2</NewProtocol>")
             .arg(port)
             .arg(QLatin1String(protocol)),
         &error);
}

std::atomic<bool> stopping{false};

} // namespace

bool isInternetAddress(quint32 a) {
    const auto in = [a](quint32 net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };
    return !(in(0x0A000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) || in(0x64400000, 10) ||
             in(0x19000000, 8) || in(0x1A000000, 8) || in(0x7F000000, 8) || in(0xA9FE0000, 16) || a == 0);
}

QString upnpPublicAddress() {
    QMutexLocker lock(&mutex());
    return publicAddress();
}

void upnpStart(quint16 port, const QString& statusFile) {
    std::thread([port, statusFile] {
        QNetworkAccessManager net;
        const auto write = [&statusFile](const QString& text) {
            QSaveFile f(statusFile);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(text.toUtf8());
                f.commit();
            }
        };
        std::optional<Gateway> gateway;
        while (!stopping) {
            if (!gateway) {
                gateway = findGateway(net);
            }
            QString status;
            if (!gateway) {
                qCWarning(lcUpnp) << "no router answered UPnP: the server is not reachable from the internet";
                status = QStringLiteral("error=no router with UPnP found\n");
            } else {
                const bool tcp = mapPort(net, *gateway, port, "TCP");
                const bool udp = mapPort(net, *gateway, port, "UDP");
                int error = 0;
                const QString answer = soap(net, *gateway, QStringLiteral("GetExternalIPAddress"), {}, &error);
                const auto ip = QRegularExpression(QStringLiteral("<NewExternalIPAddress>([^<]*)<")).match(answer);
                const QHostAddress external(ip.hasMatch() ? ip.captured(1) : QString());
                bool v4 = false;
                const quint32 raw = external.toIPv4Address(&v4);
                if (tcp && v4 && isInternetAddress(raw)) {
                    {
                        QMutexLocker lock(&mutex());
                        if (publicAddress() != external.toString()) {
                            qCInfo(lcUpnp).noquote() << "reachable from the internet at" << external.toString() << "port"
                                                     << port << (udp ? "(TCP and UDP)" : "(TCP)");
                        }
                        publicAddress() = external.toString();
                    }
                    status = QStringLiteral("address=%1\nport=%2\n").arg(external.toString()).arg(port);
                } else if (tcp) {
                    qCWarning(lcUpnp).noquote() << "the router's own address" << external.toString()
                                                << "is not on the internet (DS-Lite or carrier NAT?): not reachable";
                    status = QStringLiteral("error=the router has no public IPv4 address\n");
                } else {
                    status = QStringLiteral("error=the router did not forward the port (allow port sharing for this PC)\n");
                    gateway.reset(); // look again next time
                }
            }
            write(status);
            for (int s = 0; s < (gateway ? kRenewSeconds : 120) && !stopping; ++s) {
                QThread::sleep(1);
            }
        }
    }).detach();
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [port] {
        stopping = true;
        QNetworkAccessManager net;
        if (const auto g = findGateway(net)) {
            unmapPort(net, *g, port, "TCP");
            unmapPort(net, *g, port, "UDP");
        }
    });
}
