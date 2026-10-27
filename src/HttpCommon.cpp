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

constexpr qint64 kLibreSpeedChunk = 1024 * 1024; // garbage.php counts in MiB

// <base>/<name>, or for LibreSpeed <directory of the script>/<name>
QUrl endpoint(const QUrl &server, const QString &name, const QString &key = {}, qint64 value = 0)
{
    QUrl url = server;
    QString path = url.path();
    if (isLibreSpeed(server))
        path.truncate(path.lastIndexOf(QLatin1Char('/')) + 1);
    else if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    url.setPath(path + name);

    if (!key.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem(key, QString::number(value));
        url.setQuery(query);
    } else {
        url.setQuery(QString());
    }
    return url;
}

} // namespace

bool isLibreSpeed(const QUrl &server)
{
    const QString path = server.path();
    return path.endsWith(QLatin1String("/garbage.php"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String("/empty.php"), Qt::CaseInsensitive);
}

QUrl pingUrl(const QUrl &server)
{
    return isLibreSpeed(server) ? endpoint(server, QStringLiteral("empty.php"))
                                : endpoint(server, QStringLiteral("__down"), QStringLiteral("bytes"), 0);
}

QUrl downloadUrl(const QUrl &server)
{
    if (isLibreSpeed(server)) {
        return endpoint(server, QStringLiteral("garbage.php"), QStringLiteral("ckSize"),
                        kDownloadChunk / kLibreSpeedChunk);
    }
    return endpoint(server, QStringLiteral("__down"), QStringLiteral("bytes"), kDownloadChunk);
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

QNetworkRequest makeUploadRequest(const QUrl &server)
{
    const QString name = isLibreSpeed(server) ? QStringLiteral("empty.php") : QStringLiteral("__up");
    QNetworkRequest request = makeRequest(endpoint(server, name));
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
