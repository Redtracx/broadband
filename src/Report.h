#pragma once

#include "LoadTest.h"
#include "SpeedTest.h"
#include "Statistics.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

// The Bundesnetzagentur considers the contracted maximum reached when at
// least 90 % of it is measured.
constexpr double kContractThreshold = 0.9;

// One completed speed test. Runs started together (repetitions) share a series.
struct SpeedRun
{
    QDateTime time;
    QString server;
    int series = 1;
    int index = 1;      // 1-based position within the series
    int streams = 0;
    int durationSec = 0;
    int ipVersion = 0; // 4 or 6 as seen by the server, 0 if unknown
    SpeedTestResult result;
};

// One completed pass over all stages of a load test.
struct LoadRun
{
    QDateTime time;
    double idleLatencyMs = 0.0;
    QList<LoadStageResult> stages;
};

// Repeated load test runs with identical settings.
struct LoadSeries
{
    int id = 0; // groups the runs in the stored history
    QString server;
    bool upload = false;
    int contractMbps = 0; // 0 = unknown
    int stageDurationSec = 0;
    QList<LoadRun> runs;

    bool isEmpty() const { return runs.isEmpty(); }
    // Median over all runs, per stage; a single run is returned unchanged.
    QList<LoadStageResult> medianStages() const;
    double medianIdleLatencyMs() const;
};

// A plain-language statement about a series of measurements.
struct Finding
{
    enum Level { Info, Good, Warning, Bad };
    Level level = Info;
    QString text;
};

namespace report {

// Which metric of a speed test, for statistics and texts.
enum class Metric { Download, Upload, Ping, Jitter };

QString metricName(Metric metric);
QString metricUnit(Metric metric);
bool higherIsBetter(Metric metric);
double metricValue(const SpeedRun &run, Metric metric);
QList<double> metricValues(const QList<SpeedRun> &runs, Metric metric);

QString stabilityText(stats::Stability stability);
Finding::Level stabilityLevel(stats::Stability stability);

// Is the series consistent, which runs are outliers, is there a trend?
// All runs should be against the same server.
QList<Finding> speedFindings(const QList<SpeedRun> &runs);

// Bufferbloat (ping while downloading/uploading) and restarted transfers.
QList<Finding> loadedLatencyFindings(const QList<SpeedRun> &runs);

// Verdict of a load test: contract reached, saturation, bufferbloat and,
// with several runs, how well they agree.
QList<Finding> loadFindings(const LoadSeries &series);

// Excel workbook with an overview sheet and one sheet per test type.
QByteArray xlsx(const QList<SpeedRun> &speedRuns, const LoadSeries &load);
bool saveXlsx(const QString &path, const QList<SpeedRun> &speedRuns, const LoadSeries &load,
              QString *errorString = nullptr);

} // namespace report
