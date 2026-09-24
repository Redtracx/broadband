#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPair>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

struct LoadTestConfig
{
    QUrl baseUrl{QStringLiteral("https://speed.cloudflare.com")};
    QList<int> stages{1, 2, 4, 8, 16}; // simultaneous clients per stage
    int stageDurationSec = 10;
    bool upload = false;
};

struct LoadStageResult
{
    int clients = 0;
    double totalMbps = 0.0;     // all clients together
    double clientAvgMbps = 0.0; // per client
    double clientMinMbps = 0.0;
    double clientMaxMbps = 0.0;
    double latencyMs = -1.0;    // median ping while loaded, -1 if no ping completed
    qint64 bytes = 0;           // transferred during the whole stage
    double seconds = 0.0;
};

// Bandwidth under concurrent load: runs one stage per entry in
// LoadTestConfig::stages, each with that many independent clients (own
// connection each) transferring at the same time, while measuring latency.
// Shows whether the line delivers its bandwidth when several devices load
// at once, how it is shared, and how much latency grows under load.
class LoadTest : public QObject
{
    Q_OBJECT

public:
    explicit LoadTest(QObject *parent = nullptr);
    ~LoadTest() override;

    bool isRunning() const { return m_running; }
    double idleLatencyMs() const { return m_idleLatencyMs; }
    const QList<LoadStageResult> &results() const { return m_results; }

    // Total run time in seconds for a config, used to scale charts.
    static double totalDuration(const LoadTestConfig &config);

public slots:
    void start(const LoadTestConfig &config);
    void stop();

signals:
    void idleLatencyMeasured(double ms);
    void stageStarted(int index, int clients);
    void sample(double seconds, double totalMbps); // seconds since the first stage started
    void progressChanged(double fraction);         // 0..1 over the whole run
    void stageFinished(int index, const LoadStageResult &result);
    void finished();
    void failed(const QString &message);
    void stopped();

private:
    struct Client
    {
        QNetworkAccessManager *nam = nullptr;
        QNetworkReply *reply = nullptr;
        qint64 bytes = 0;
        qint64 warmupBytes = 0;
        qint64 uploadSent = 0; // progress of the current upload request
    };

    void fail(const QString &message);
    void abortPing();
    void teardownClients();

    void sendPing();
    void onPingFinished(double elapsedMs);

    void startStage(int index);
    void launch(Client *client);
    void onSampleTimer();
    void finishStage();

    LoadTestConfig m_config;
    int m_runId = 0; // invalidates pending timers of a previous run
    bool m_running = false;
    bool m_idlePhase = false;
    bool m_stageActive = false;

    QNetworkAccessManager *m_pingNam = nullptr;
    QNetworkReply *m_pingReply = nullptr;
    QElapsedTimer m_pingTimer;
    QList<double> m_idlePings;
    QList<double> m_stagePings;
    int m_idlePingsDone = 0;
    double m_idleLatencyMs = 0.0;

    int m_stageIndex = -1;
    QList<Client *> m_clients;
    QElapsedTimer m_stageTimer;
    QTimer m_sampleTimer;
    qint64 m_warmupMs = 0;
    bool m_warmedUp = false;
    double m_stageOffset = 0.0; // seconds of the run before this stage
    QList<QPair<qint64, qint64>> m_window;
    QList<LoadStageResult> m_results;
};
