#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkReply;
class QObject;

// Helpers shared by SpeedTest and LoadTest for talking to a server that
// speaks the Cloudflare speed test protocol:
//   GET  <base>/__down?bytes=N  -> N bytes of payload
//   POST <base>/__up            -> body is discarded
namespace http {

constexpr qint64 kDownloadChunk = 25'000'000; // Cloudflare rejects very large requests
constexpr int kUploadChunk = 8'000'000;
constexpr int kTransferTimeoutMs = 15'000;
// A failed transfer is restarted after this delay instead of aborting the test.
constexpr int kRetryDelayMs = 250;

enum class IpVersion { Any, V4, V6 };

QString ipVersionName(IpVersion version);

QUrl endpoint(const QUrl &base, const QString &name, qint64 bytes = -1);
QNetworkRequest makeRequest(const QUrl &url);

// Where the requests of a test go. With a forced IP version the host name is
// resolved up front and requests are sent to one address of that family,
// while the Host header and the TLS certificate check still use the name.
struct Target
{
    QUrl url;         // base URL; its host may have been replaced by an address
    QString hostName; // original host name if it was replaced, else empty

    QNetworkRequest request(const QString &name, qint64 bytes = -1) const;
    QNetworkRequest uploadRequest() const;
};

// Calls done(target, error) once the target is known: right away for
// IpVersion::Any or an IP address, otherwise after a DNS lookup. The callback
// is dropped if context is destroyed first.
void resolveTarget(const QUrl &base, IpVersion version, QObject *context,
                   std::function<void(const Target &target, const QString &error)> done);

// Local targets must not go through an HTTP proxy that may be configured
// system-wide, otherwise LAN tests would fail.
QNetworkProxy proxyFor(const QUrl &url);

// Random data, so compressing proxies can't inflate upload results.
const QByteArray &uploadPayload();

double median(QList<double> values);

// A 4xx response means the server does not support the request; retrying
// would not help.
bool isClientError(const QNetworkReply *reply);

// What the server tells about itself and about us. Cloudflare sends this in
// cf-meta-* headers (and the data center in CF-RAY); the built-in LAN
// server sends cf-meta-ip.
struct ServerInfo
{
    QString colo;     // Cloudflare data center (IATA airport code), e.g. FRA
    QString city;     // of the client, as seen by the server
    QString country;
    QString asn;      // autonomous system of the client's provider
    QString clientIp; // public address the server saw

    bool isEmpty() const { return colo.isEmpty() && clientIp.isEmpty(); }
    // 4 or 6 from clientIp, 0 if unknown.
    int ipVersion() const;

    bool operator==(const ServerInfo &other) const
    {
        return colo == other.colo && city == other.city && country == other.country && asn == other.asn &&
               clientIp == other.clientIp;
    }
};

// Adds the fields present in the reply's headers; returns true if anything changed.
bool updateServerInfo(ServerInfo &info, const QNetworkReply *reply);

} // namespace http
