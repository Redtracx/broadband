#pragma once

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
    // Any server speaking the Cloudflare or LibreSpeed protocol, see HttpCommon.h
    QUrl baseUrl{QStringLiteral("https://speed.cloudflare.com")};
    int durationSec = 10; // per transfer phase
    int streams = 4;      // parallel HTTP connections (Qt allows up to 6 per host)
    int pingCount = 10;
};

struct SpeedTestResult
{
    double pingMs = 0.0;
    double jitterMs = 0.0;
    double downloadMbps = 0.0;
    double uploadMbps = 0.0;
    qint64 downloadedBytes = 0;
    qint64 uploadedBytes = 0;
};

// Runs latency, download and upload measurements one after another.
// Everything happens asynchronously on the thread the object lives in.
class SpeedTest : public QObject
{
    Q_OBJECT

public:
    enum class Phase { Idle, Latency, Download, Upload, Done };
    Q_ENUM(Phase)

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
    void throughputSample(SpeedTest::Phase phase, double seconds, double mbps);
    void phaseFinished(SpeedTest::Phase phase, double mbps);
    void finished(const SpeedTestResult &result);
    void failed(const QString &message);

private:
    void setPhase(Phase phase);
    void fail(const QString &message);
    void abortAll();

    void sendPing();
    void onPingFinished(double elapsedMs);

    void startTransfer(Phase phase);
    void launchStream();
    void onSampleTimer();
    void finishTransfer();

    QNetworkAccessManager *m_nam = nullptr;
    SpeedTestConfig m_config;
    SpeedTestResult m_result;
    Phase m_phase = Phase::Idle;

    // Latency phase
    QList<double> m_pings;
    int m_pingsDone = 0;
    QElapsedTimer m_pingTimer;

    // Transfer phases
    QList<QNetworkReply *> m_replies;
    QHash<QNetworkReply *, qint64> m_uploadProgress;
    QElapsedTimer m_phaseTimer;
    QTimer m_sampleTimer;
    qint64 m_bytes = 0;
    QList<QPair<qint64, qint64>> m_window; // (ms, bytes) of recent samples
    qint64 m_warmupBytes = -1;
    qint64 m_warmupMs = 0;
};
