#pragma once

#include "LoadTest.h"
#include "Report.h"
#include "SpeedTest.h"

#include <QMainWindow>
#include <QTimer>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class TestServer;
class ThroughputChart;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

    void setServerUrl(const QString &url);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct Tile
    {
        QLabel *value = nullptr;
        QLabel *detail = nullptr;
    };

    QWidget *createSpeedTestTab();
    QWidget *createLoadTestTab();
    QWidget *createServerTab();
    QWidget *createTile(const QString &title, const QString &unit, Tile &tile);

    void loadSettings();
    void saveSettings() const;
    bool serverUrl(QUrl &url);
    bool extraServers(QList<QUrl> &servers);
    void updateRunningState();
    // Waits between two runs of a series, with a countdown in the status label.
    void startPause(int seconds, QLabel *status, std::function<void()> next);
    void pauseTick();
    void exportReport();

    // Speed test
    void toggleTest();
    void startNextSpeedRun();
    QString seriesPrefix() const;
    void onPhaseChanged(SpeedTest::Phase phase);
    void onProgress(double fraction);
    void onSample(SpeedTest::Phase phase, double seconds, double mbps);
    void onPhaseFinished(SpeedTest::Phase phase, double mbps);
    void onFinished(const SpeedTestResult &result);
    void onFailed(const QString &message);
    QList<SpeedRun> currentSeries() const;
    void updateSeriesAnalysis();
    void clearHistory();

    // Load test
    void toggleLoadTest();
    void startNextLoadRun();
    void onLoadStageFinished(int index, const LoadStageResult &result);
    void onLoadFinished();
    void updateContractLine();
    QString skippedServersHtml() const;

    // LAN server
    void toggleServer();
    void updateServerStatus();

    SpeedTest *m_test = nullptr;
    LoadTest *m_load = nullptr;
    TestServer *m_server = nullptr;
    QTabWidget *m_tabs = nullptr;
    QComboBox *m_serverUrl = nullptr;
    QTimer m_pauseTimer;
    int m_pauseRemaining = 0;
    QLabel *m_pauseStatus = nullptr;
    std::function<void()> m_pauseNext;

    // Speed test tab
    Tile m_pingTile;
    Tile m_downloadTile;
    Tile m_uploadTile;
    ThroughputChart *m_chart = nullptr;
    int m_downloadSeries = -1;
    int m_uploadSeries = -1;
    QSpinBox *m_duration = nullptr;
    QSpinBox *m_streams = nullptr;
    QSpinBox *m_repeats = nullptr;
    QSpinBox *m_pause = nullptr;
    QPushButton *m_startButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QTableWidget *m_history = nullptr;
    QLabel *m_seriesAnalysis = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    // All runs of this session; runs started with one click form a series.
    QList<SpeedRun> m_speedRuns;
    SpeedTestConfig m_speedConfig;
    bool m_speedSeriesActive = false;
    int m_series = 0;
    int m_seriesRun = 0;
    int m_seriesTotal = 0;

    // Load test tab
    QSpinBox *m_contract = nullptr;
    QComboBox *m_loadDirection = nullptr;
    QLineEdit *m_loadStages = nullptr;
    QPlainTextEdit *m_loadServers = nullptr;
    QSpinBox *m_loadDuration = nullptr;
    QSpinBox *m_loadRuns = nullptr;
    QSpinBox *m_loadPause = nullptr;
    QPushButton *m_loadButton = nullptr;
    QPushButton *m_loadExportButton = nullptr;
    ThroughputChart *m_loadChart = nullptr;
    int m_loadSeriesIndex = -1;
    QProgressBar *m_loadProgress = nullptr;
    QLabel *m_loadStatus = nullptr;
    QTableWidget *m_loadTable = nullptr;
    QLabel *m_loadSummary = nullptr;
    LoadSeries m_loadSeries;
    LoadTestConfig m_loadConfig;
    bool m_loadSeriesActive = false;
    int m_loadRun = 0;
    int m_loadRunTotal = 0;
    QDateTime m_loadRunStart;
    QStringList m_loadSkipped; // extra servers that didn't answer in this run

    // Server tab
    QSpinBox *m_port = nullptr;
    QPushButton *m_serverButton = nullptr;
    QPushButton *m_useLocalButton = nullptr;
    QLabel *m_serverState = nullptr;
    QLabel *m_serverAddresses = nullptr;
    QLabel *m_serverTraffic = nullptr;
    QTimer m_serverStatsTimer;
};
