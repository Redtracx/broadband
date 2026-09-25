#pragma once

#include "Report.h"

#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

// Measurements survive restarts: every completed speed test and load test run
// is appended as one JSON object per line (JSON Lines) to a file in the
// user's data directory. The GUI and the command line client share the file,
// so runs started by cron show up in the GUI and its Excel export.
//
// The file contains measured values, times, the server address, the Cloudflare
// data center and the IP version, but not the public IP address.
namespace history {

struct Data
{
    QList<SpeedRun> speedRuns;    // oldest first
    QList<LoadSeries> loadSeries; // oldest first

    int lastSpeedSeries() const;
    int lastLoadSeries() const;
};

// <data dir>/history.jsonl, or $BROADBAND_HISTORY if set.
QString defaultPath();

// Lines that can't be parsed are skipped, so a damaged line does not cost
// the whole history. A missing file is an empty history.
Data load(const QString &path, QString *errorString = nullptr);

bool append(const QString &path, const SpeedRun &run, QString *errorString = nullptr);
bool append(const QString &path, const LoadSeries &series, const LoadRun &run, QString *errorString = nullptr);

// Removes all speed test entries and keeps the load tests.
bool removeSpeedRuns(const QString &path, QString *errorString = nullptr);

// The same objects are used for the history file and for `--format json`.
QJsonObject toJson(const SpeedRun &run);
QJsonObject toJson(const LoadSeries &series, const LoadRun &run);
std::optional<SpeedRun> speedRunFromJson(const QJsonObject &object);

// CSV with a header line, for spreadsheets and scripts (`--format csv`).
// Decimal points, no thousands separators, regardless of the locale.
QString speedCsvHeader();
QString toCsv(const SpeedRun &run);
QString loadCsvHeader();
QStringList toCsv(const LoadSeries &series, const LoadRun &run); // one line per stage

} // namespace history
