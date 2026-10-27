#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

// Helpers shared by SpeedTest and LoadTest for talking to a speed test
// server. Two protocols are understood, chosen by the server address:
//
// Cloudflare (any other address, also `broadband --server`):
//   GET  <base>/__down?bytes=N        -> N bytes of payload
//   POST <base>/__up                  -> body is discarded
// LibreSpeed (address ends in garbage.php or empty.php):
//   GET  <dir>/garbage.php?ckSize=N   -> N MiB of payload
//   GET  <dir>/empty.php              -> empty response (ping)
//   POST <dir>/empty.php              -> body is discarded
namespace http {

constexpr qint64 kDownloadChunk = 25'000'000; // Cloudflare rejects very large requests
constexpr int kUploadChunk = 8'000'000;
constexpr int kTransferTimeoutMs = 15'000;

bool isLibreSpeed(const QUrl &server);
QUrl pingUrl(const QUrl &server);
QUrl downloadUrl(const QUrl &server);
QNetworkRequest makeRequest(const QUrl &url);
QNetworkRequest makeUploadRequest(const QUrl &server);

// Local targets must not go through an HTTP proxy that may be configured
// system-wide, otherwise LAN tests would fail.
QNetworkProxy proxyFor(const QUrl &url);

// Random data, so compressing proxies can't inflate upload results.
const QByteArray &uploadPayload();

double median(QList<double> values);

} // namespace http
