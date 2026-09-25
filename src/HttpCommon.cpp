#include "HttpCommon.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QPointer>
#include <QRandomGenerator>
#include <QUrlQuery>

#include <algorithm>
#include <vector>

namespace http {

namespace {

bool isLocalHost(const QString &host)
{
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
        return true;

    QHostAddress address;
    if (!address.setAddress(host))
        return false;
    if (address.isLoopback() || address.isLinkLocal())
        return true;

    bool isIPv4 = false;
    const quint32 v4 = address.toIPv4Address(&isIPv4);
    if (isIPv4) {
        return (v4 >> 24) == 10                    // 10.0.0.0/8
            || (v4 >> 20) == ((172u << 4) | 1u)    // 172.16.0.0/12
            || (v4 >> 16) == ((192u << 8) | 168u); // 192.168.0.0/16
    }
    return (address.toIPv6Address()[0] & 0xfe) == 0xfc; // fc00::/7
}

QNetworkRequest targetRequest(const Target &target, const QUrl &url)
{
    QNetworkRequest request = makeRequest(url);
    if (!target.hostName.isEmpty()) {
        const int port = target.url.port();
        const bool defaultPort = port < 0 || port == (target.url.scheme() == QLatin1String("https") ? 443 : 80);
        const QString host = defaultPort ? target.hostName : QStringLiteral("%1:%2").arg(target.hostName).arg(port);
        request.setRawHeader("Host", host.toUtf8());
        request.setPeerVerifyName(target.hostName);
    }
    return request;
}

} // namespace

QString ipVersionName(IpVersion version)
{
    switch (version) {
    case IpVersion::V4:
        return QStringLiteral("IPv4");
    case IpVersion::V6:
        return QStringLiteral("IPv6");
    case IpVersion::Any:
        break;
    }
    return QStringLiteral("auto");
}

QUrl endpoint(const QUrl &base, const QString &name, qint64 bytes)
{
    QUrl url = base;
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    url.setPath(path + name);

    if (bytes >= 0) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("bytes"), QString::number(bytes));
        url.setQuery(query);
    } else {
        url.setQuery(QString());
    }
    return url;
}

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    // HTTP/1.1 gives one TCP connection per stream; HTTP/2 would multiplex
    // all streams over a single connection.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    // Ask for the raw bytes: a compressed response would distort the numbers.
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("Cache-Control", "no-store");
    request.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("broadband/" BROADBAND_VERSION));
    request.setTransferTimeout(kTransferTimeoutMs);
    return request;
}

QNetworkRequest Target::request(const QString &name, qint64 bytes) const
{
    return targetRequest(*this, endpoint(url, name, bytes));
}

QNetworkRequest Target::uploadRequest() const
{
    QNetworkRequest request = targetRequest(*this, endpoint(url, QStringLiteral("__up")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/octet-stream"));
    return request;
}

void resolveTarget(const QUrl &base, IpVersion version, QObject *context,
                   std::function<void(const Target &target, const QString &error)> done)
{
    const QString host = base.host();
    if (version == IpVersion::Any || QHostAddress(host).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol) {
        done(Target{base, {}}, {});
        return;
    }

    // Through a proxy the proxy picks the address; forcing one would at best
    // be ignored and is often refused.
    if (proxyFor(base).type() != QNetworkProxy::NoProxy) {
        const QList<QNetworkProxy> proxies = QNetworkProxyFactory::proxyForQuery(QNetworkProxyQuery(base));
        if (!proxies.isEmpty() && proxies.first().type() != QNetworkProxy::NoProxy) {
            done({}, QObject::tr("Die IP-Version lässt sich nicht erzwingen, weil die Verbindung über einen Proxy "
                                 "läuft (%1:%2).")
                         .arg(proxies.first().hostName())
                         .arg(proxies.first().port()));
            return;
        }
    }

    const QPointer<QObject> guard(context);
    QHostInfo::lookupHost(host, context, [base, version, host, guard, done](const QHostInfo &info) {
        if (!guard)
            return;
        const auto wanted = version == IpVersion::V4 ? QAbstractSocket::IPv4Protocol : QAbstractSocket::IPv6Protocol;
        const QList<QHostAddress> addresses = info.addresses();
        const auto it = std::find_if(addresses.begin(), addresses.end(),
                                     [wanted](const QHostAddress &a) { return a.protocol() == wanted; });
        if (it == addresses.end()) {
            const QString reason = info.error() != QHostInfo::NoError
                                       ? info.errorString()
                                       : QObject::tr("keine %1-Adresse gefunden").arg(ipVersionName(version));
            done({}, QObject::tr("%1 nicht auflösbar: %2").arg(host, reason));
            return;
        }
        QUrl url = base;
        url.setHost(it->toString());
        done(Target{url, host}, {});
    });
}

QNetworkProxy proxyFor(const QUrl &url)
{
    return isLocalHost(url.host()) ? QNetworkProxy(QNetworkProxy::NoProxy)
                                   : QNetworkProxy(QNetworkProxy::DefaultProxy);
}

const QByteArray &uploadPayload()
{
    static const QByteArray data = [] {
        std::vector<quint32> noise(kUploadChunk / sizeof(quint32));
        QRandomGenerator::global()->fillRange(noise.data(), qsizetype(noise.size()));
        return QByteArray(reinterpret_cast<const char *>(noise.data()), kUploadChunk);
    }();
    return data;
}

double median(QList<double> values)
{
    if (values.isEmpty())
        return 0.0;
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

bool isClientError(const QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return status >= 400 && status < 500;
}

int ServerInfo::ipVersion() const
{
    switch (QHostAddress(clientIp).protocol()) {
    case QAbstractSocket::IPv4Protocol:
        return 4;
    case QAbstractSocket::IPv6Protocol:
        return 6;
    default:
        return 0;
    }
}

bool updateServerInfo(ServerInfo &info, const QNetworkReply *reply)
{
    const ServerInfo before = info;
    auto take = [reply](QString &field, const char *header) {
        const QByteArray value = reply->rawHeader(header);
        if (!value.isEmpty())
            field = QString::fromUtf8(value).trimmed();
    };
    take(info.colo, "cf-meta-colo");
    take(info.city, "cf-meta-city");
    take(info.country, "cf-meta-country");
    take(info.asn, "cf-meta-asn");
    take(info.clientIp, "cf-meta-ip");

    // CF-RAY: <id>-<colo>
    if (info.colo.isEmpty()) {
        const QByteArray ray = reply->rawHeader("cf-ray");
        const auto dash = ray.lastIndexOf('-');
        if (dash > 0 && dash + 1 < ray.size())
            info.colo = QString::fromLatin1(ray.mid(dash + 1)).trimmed();
    }
    return !(info == before);
}

} // namespace http
