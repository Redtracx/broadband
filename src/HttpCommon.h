#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

// Helpers shared by SpeedTest and LoadTest for talking to a server that
// speaks the Cloudflare speed test protocol:
//   GET  <base>/__down?bytes=N  -> N bytes of payload
//   POST <base>/__up            -> body is discarded
namespace http {

constexpr qint64 kDownloadChunk = 25'000'000; // Cloudflare rejects very large requests
constexpr int kUploadChunk = 8'000'000;
constexpr int kTransferTimeoutMs = 15'000;

QUrl endpoint(const QUrl &base, const QString &name, qint64 bytes = -1);
QNetworkRequest makeRequest(const QUrl &url);
QNetworkRequest makeUploadRequest(const QUrl &base);

// Local targets must not go through an HTTP proxy that may be configured
// system-wide, otherwise LAN tests would fail.
QNetworkProxy proxyFor(const QUrl &url);

// Random data, so compressing proxies can't inflate upload results.
const QByteArray &uploadPayload();

double median(QList<double> values);

} // namespace http
