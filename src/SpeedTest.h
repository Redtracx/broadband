#pragma once

#include "HttpCommon.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

struct SpeedTestConfig
{
    // Any server speaking the Cloudflare speed test protocol:
    //   GET  <base>/__down?bytes=N  -> N bytes of payload
    //   POST <base>/__up            -> body is discarded
    QUrl baseUrl{QStringLiteral("https://speed.cloudflare.com")};
    int durationSec = 10; // per transfer phase
    int streams = 4;      // parallel HTTP connections, 1..kMaxStreams
    int pingCount = 10;
    http::IpVersion ipVersion = http::IpVersion::Any;
};

struct SpeedTestResult
{
    double pingMs = 0.0;
    double jitterMs = 0.0;
    double downloadMbps = 0.0;
    double uploadMbps = 0.0;
    qint64 downloadedBytes = 0;
    qint64 uploadedBytes = 0;
    // Median ping while the line is busy (bufferbloat), -1 if none completed.
    double downloadLatencyMs = -1.0;
    double uploadLatencyMs = -1.0;
    int retries = 0; // failed transfers that were restarted
    http::ServerInfo server;
};

// Runs latency, download and upload measurements one after another.
// Everything happens asynchronously on the thread the object lives in.
class SpeedTest : public QObject
{
    Q_OBJECT

public:
    enum class Phase { Idle, Latency, Download, Upload, Done };
    Q_ENUM(Phase)

    // Every stream has its own access manager, so Qt's limit of six
    // connections per host and manager does not apply.
    static constexpr int kMaxStreams = 32;

    explicit SpeedTest(QObject *parent = nullptr);
    ~SpeedTest() override;

    bool isRunning() const;
    Phase phase() const { return m_phase; }
    const SpeedTestResult &result() const { return m_result; }

public slots:
    void start(const SpeedTestConfig &config);
    void stop();

signals:
    void phaseChanged(SpeedTest::Phase phase);
    void progressChanged(double fraction); // 0..1 within the current phase
    void latencyMeasured(double pingMs, double jitterMs);
    void serverInfoChanged(const http::ServerInfo &info);
    void throughputSample(SpeedTest::Phase phase, double seconds, double mbps);
    void phaseFinished(SpeedTest::Phase phase, double mbps);
    void finished(const SpeedTestResult &result);
    void failed(const QString &message);

private:
    void setPhase(Phase phase);
    void fail(const QString &message);
    void abortAll();
    void releaseStreams();
    void takeServerInfo(const QNetworkReply *reply);

    void sendPing();
    void onPingFinished(double elapsedMs);
    void onLoadedPingFinished(double elapsedMs);

    void startTransfer(Phase phase);
    void launchStream(int slot);
    void onStreamError(int slot, QNetworkReply *reply);
    void onSampleTimer();
    void finishTransfer();

    QNetworkAccessManager *m_pingNam = nullptr;
    QList<QNetworkAccessManager *> m_streamNams;
    SpeedTestConfig m_config;
    http::Target m_target;
    SpeedTestResult m_result;
    Phase m_phase = Phase::Idle;
    int m_generation = 0; // invalidates delayed actions after an abort

    // Latency, idle and while loaded
    QNetworkReply *m_pingReply = nullptr;
    QList<double> m_pings;
    QList<double> m_loadedPings;
    int m_pingsDone = 0;
    QElapsedTimer m_pingTimer;

    // Transfer phases
    QList<QNetworkReply *> m_replies;
    QHash<QNetworkReply *, qint64> m_uploadProgress;
    QElapsedTimer m_phaseTimer;
    QTimer m_sampleTimer;
    qint64 m_bytes = 0;
    int m_phaseErrors = 0;
    QList<QPair<qint64, qint64>> m_window; // (ms, bytes) of recent samples
    qint64 m_warmupBytes = -1;
    qint64 m_warmupMs = 0;
};
