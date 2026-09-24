#include "HttpCommon.h"

#include <QHostAddress>
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

} // namespace

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

QNetworkRequest makeUploadRequest(const QUrl &base)
{
    QNetworkRequest request = makeRequest(endpoint(base, QStringLiteral("__up")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/octet-stream"));
    return request;
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

} // namespace http
