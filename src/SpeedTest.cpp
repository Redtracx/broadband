#include "SpeedTest.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr int kSampleIntervalMs = 250;
// Live values are averaged over this window; progress reports (especially for
// uploads) arrive in bursts and would make the chart spiky otherwise.
constexpr qint64 kSmoothingWindowMs = 1000;
// The first part of every transfer phase is ignored for the final value, so
// TCP slow start and connection setup don't drag the result down.
constexpr double kWarmupFraction = 0.2;
// Pause between two pings while downloading or uploading.
constexpr int kLoadedPingIntervalMs = 250;

// Mean absolute difference between consecutive samples (as in RFC 3550).
double jitter(const QList<double> &values)
{
    if (values.size() < 2)
        return 0.0;
    double sum = 0.0;
    for (qsizetype i = 1; i < values.size(); ++i)
        sum += std::abs(values[i] - values[i - 1]);
    return sum / double(values.size() - 1);
}

} // namespace

SpeedTest::SpeedTest(QObject *parent)
    : QObject(parent)
    , m_pingNam(new QNetworkAccessManager(this))
{
    m_sampleTimer.setInterval(kSampleIntervalMs);
    m_sampleTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_sampleTimer, &QTimer::timeout, this, &SpeedTest::onSampleTimer);
}

SpeedTest::~SpeedTest()
{
    abortAll();
}

bool SpeedTest::isRunning() const
{
    return m_phase == Phase::Latency || m_phase == Phase::Download || m_phase == Phase::Upload;
}

void SpeedTest::start(const SpeedTestConfig &config)
{
    stop();

    m_config = config;
    m_config.streams = std::clamp(m_config.streams, 1, kMaxStreams);
    m_config.durationSec = std::max(1, m_config.durationSec);
    m_config.pingCount = std::max(1, m_config.pingCount);
    m_result = {};
    m_pings.clear();
    m_pingsDone = 0;

    setPhase(Phase::Latency);
    emit progressChanged(0.0);

    const int generation = m_generation;
    http::resolveTarget(m_config.baseUrl, m_config.ipVersion, this,
                        [this, generation](const http::Target &target, const QString &error) {
                            if (generation != m_generation || !isRunning())
                                return;
                            if (!error.isEmpty()) {
                                fail(error);
                                return;
                            }
                            m_target = target;
                            releaseStreams();

                            const QNetworkProxy proxy = http::proxyFor(m_config.baseUrl);
                            m_pingNam->clearConnectionCache();
                            m_pingNam->setProxy(proxy);
                            for (int i = 0; i < m_config.streams; ++i) {
                                auto *nam = new QNetworkAccessManager(this);
                                nam->setProxy(proxy);
                                m_streamNams.append(nam);
                            }
                            sendPing();
                        });
}

void SpeedTest::stop()
{
    if (!isRunning())
        return;
    abortAll();
    releaseStreams();
    setPhase(Phase::Idle);
}

void SpeedTest::setPhase(Phase phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    emit phaseChanged(phase);
}

void SpeedTest::fail(const QString &message)
{
    abortAll();
    releaseStreams();
    setPhase(Phase::Idle);
    emit failed(message);
}

void SpeedTest::abortAll()
{
    ++m_generation;
    m_sampleTimer.stop();
    auto replies = std::exchange(m_replies, {});
    if (m_pingReply)
        replies.append(std::exchange(m_pingReply, nullptr));
    for (QNetworkReply *reply : std::as_const(replies)) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_uploadProgress.clear();
}

void SpeedTest::releaseStreams()
{
    // Deferred: this may run inside a signal of one of their replies.
    for (QNetworkAccessManager *nam : std::exchange(m_streamNams, {}))
        nam->deleteLater();
}

void SpeedTest::takeServerInfo(const QNetworkReply *reply)
{
    if (http::updateServerInfo(m_result.server, reply))
        emit serverInfoChanged(m_result.server);
}

// --- Latency ----------------------------------------------------------------

void SpeedTest::sendPing()
{
    m_pingReply = m_pingNam->get(m_target.request(QStringLiteral("__down"), 0));
    m_pingTimer.start();
    connect(m_pingReply, &QNetworkReply::finished, this, [this] {
        const double elapsedMs = double(m_pingTimer.nsecsElapsed()) / 1e6;
        QNetworkReply *reply = std::exchange(m_pingReply, nullptr);
        reply->deleteLater();

        if (m_phase != Phase::Latency) {
            // A lost ping under load is no reason to abort.
            onLoadedPingFinished(reply->error() == QNetworkReply::NoError ? elapsedMs : -1.0);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("Latenzmessung fehlgeschlagen: %1").arg(reply->errorString()));
            return;
        }
        takeServerInfo(reply);
        onPingFinished(elapsedMs);
    });
}

void SpeedTest::onPingFinished(double elapsedMs)
{
    // The first request pays for DNS, TCP and TLS setup, so it isn't counted.
    if (m_pingsDone++ > 0)
        m_pings.append(elapsedMs);

    emit progressChanged(double(m_pingsDone) / double(m_config.pingCount + 1));

    if (m_pingsDone <= m_config.pingCount) {
        sendPing();
        return;
    }

    m_result.pingMs = http::median(m_pings);
    m_result.jitterMs = jitter(m_pings);
    emit latencyMeasured(m_result.pingMs, m_result.jitterMs);

    startTransfer(Phase::Download);
}

void SpeedTest::onLoadedPingFinished(double elapsedMs)
{
    // Pings during the warm-up are not representative yet.
    if (elapsedMs >= 0.0 && m_warmupBytes >= 0)
        m_loadedPings.append(elapsedMs);

    const int generation = m_generation;
    QTimer::singleShot(kLoadedPingIntervalMs, this, [this, generation] {
        if (generation == m_generation && !m_pingReply &&
            (m_phase == Phase::Download || m_phase == Phase::Upload))
            sendPing();
    });
}

// --- Download / upload ------------------------------------------------------

void SpeedTest::startTransfer(Phase phase)
{
    setPhase(phase);
    emit progressChanged(0.0);

    m_bytes = 0;
    m_phaseErrors = 0;
    m_window = {{0, 0}};
    m_warmupBytes = -1;
    m_warmupMs = qint64(m_config.durationSec * 1000 * kWarmupFraction);
    m_loadedPings.clear();

    m_phaseTimer.start();
    for (int i = 0; i < m_config.streams; ++i)
        launchStream(i);
    m_sampleTimer.start();
    if (!m_pingReply)
        sendPing();
}

void SpeedTest::launchStream(int slot)
{
    QNetworkAccessManager *nam = m_streamNams.value(slot);
    if (!nam)
        return;

    QNetworkReply *reply = nullptr;
    if (m_phase == Phase::Download) {
        reply = nam->get(m_target.request(QStringLiteral("__down"), http::kDownloadChunk));
        connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
            m_bytes += reply->readAll().size();
        });
    } else {
        reply = nam->post(m_target.uploadRequest(), http::uploadPayload());
        m_uploadProgress.insert(reply, 0);
        connect(reply, &QNetworkReply::uploadProgress, this, [this, reply](qint64 sent, qint64) {
            qint64 &last = m_uploadProgress[reply];
            if (sent > last) {
                m_bytes += sent - last;
                last = sent;
            }
        });
    }

    m_replies.append(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, slot] {
        m_replies.removeOne(reply);
        m_uploadProgress.remove(reply);
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            onStreamError(slot, reply);
            return;
        }
        takeServerInfo(reply);
        // Keep the pipe full until the phase is over.
        launchStream(slot);
    });
}

void SpeedTest::onStreamError(int slot, QNetworkReply *reply)
{
    const QString what = m_phase == Phase::Download ? tr("Download") : tr("Upload");
    // A single dropped connection should not cost the whole measurement, but
    // a server that rejects the request or keeps failing ends the test.
    const int maxErrors = std::max(3, 2 * m_config.streams);
    if (http::isClientError(reply) || ++m_phaseErrors > maxErrors) {
        fail(tr("%1 fehlgeschlagen: %2").arg(what, reply->errorString()));
        return;
    }

    ++m_result.retries;
    const int generation = m_generation;
    const Phase phase = m_phase;
    QTimer::singleShot(http::kRetryDelayMs, this, [this, generation, phase, slot] {
        if (generation == m_generation && m_phase == phase)
            launchStream(slot);
    });
}

void SpeedTest::onSampleTimer()
{
    const qint64 now = m_phaseTimer.elapsed();
    while (m_window.size() > 1 && now - m_window[1].first >= kSmoothingWindowMs)
        m_window.removeFirst();
    const auto [startMs, startBytes] = m_window.first();
    if (now > startMs) {
        const double mbps = double(m_bytes - startBytes) * 8.0 / (double(now - startMs) * 1000.0);
        emit throughputSample(m_phase, double(now) / 1000.0, mbps);
    }
    m_window.append({now, m_bytes});

    if (m_warmupBytes < 0 && now >= m_warmupMs) {
        m_warmupBytes = m_bytes;
        m_warmupMs = now;
    }

    const qint64 totalMs = qint64(m_config.durationSec) * 1000;
    emit progressChanged(std::min(1.0, double(now) / double(totalMs)));

    if (now >= totalMs)
        finishTransfer();
}

void SpeedTest::finishTransfer()
{
    const qint64 now = m_phaseTimer.elapsed();
    const qint64 totalBytes = m_bytes;
    abortAll();

    const qint64 startBytes = m_warmupBytes >= 0 ? m_warmupBytes : 0;
    const qint64 startMs = m_warmupBytes >= 0 ? m_warmupMs : 0;
    const qint64 ms = now - startMs;
    const double mbps = ms > 0 ? double(totalBytes - startBytes) * 8.0 / (double(ms) * 1000.0) : 0.0;
    const double loadedLatency = m_loadedPings.isEmpty() ? -1.0 : http::median(m_loadedPings);

    const Phase phase = m_phase;
    if (phase == Phase::Download) {
        m_result.downloadMbps = mbps;
        m_result.downloadedBytes = totalBytes;
        m_result.downloadLatencyMs = loadedLatency;
    } else {
        m_result.uploadMbps = mbps;
        m_result.uploadedBytes = totalBytes;
        m_result.uploadLatencyMs = loadedLatency;
    }
    emit phaseFinished(phase, mbps);

    if (phase == Phase::Download) {
        startTransfer(Phase::Upload);
    } else {
        releaseStreams();
        setPhase(Phase::Done);
        emit finished(m_result);
    }
}
