#include "History.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace history {

namespace {

const QString kSpeedType = QStringLiteral("speed");
const QString kLoadType = QStringLiteral("load");

QString timeToString(const QDateTime &time)
{
    // Keep the local offset, so the file reads naturally and stays unambiguous.
    return time.toOffsetFromUtc(time.offsetFromUtc()).toString(Qt::ISODateWithMs);
}

QDateTime timeFromString(const QString &text)
{
    return QDateTime::fromString(text, Qt::ISODateWithMs).toLocalTime();
}

// Plain number with a decimal point; -1 stays -1 ("not measured").
QString number(double value, int decimals = 2)
{
    return QString::number(value, 'f', decimals);
}

QString csvText(const QString &text)
{
    if (!text.contains(QLatin1Char(',')) && !text.contains(QLatin1Char('"')) && !text.contains(QLatin1Char('\n')))
        return text;
    return QLatin1Char('"') + QString(text).replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
}

QJsonObject stageToJson(const LoadStageResult &stage)
{
    return QJsonObject{
        {QStringLiteral("clients"), stage.clients},
        {QStringLiteral("totalMbps"), stage.totalMbps},
        {QStringLiteral("clientAvgMbps"), stage.clientAvgMbps},
        {QStringLiteral("clientMinMbps"), stage.clientMinMbps},
        {QStringLiteral("clientMaxMbps"), stage.clientMaxMbps},
        {QStringLiteral("latencyMs"), stage.latencyMs},
        {QStringLiteral("bytes"), stage.bytes},
        {QStringLiteral("seconds"), stage.seconds},
        {QStringLiteral("retries"), stage.retries},
    };
}

LoadStageResult stageFromJson(const QJsonObject &o)
{
    LoadStageResult stage;
    stage.clients = o.value(QStringLiteral("clients")).toInt();
    stage.totalMbps = o.value(QStringLiteral("totalMbps")).toDouble();
    stage.clientAvgMbps = o.value(QStringLiteral("clientAvgMbps")).toDouble();
    stage.clientMinMbps = o.value(QStringLiteral("clientMinMbps")).toDouble();
    stage.clientMaxMbps = o.value(QStringLiteral("clientMaxMbps")).toDouble();
    stage.latencyMs = o.value(QStringLiteral("latencyMs")).toDouble(-1.0);
    stage.bytes = o.value(QStringLiteral("bytes")).toInteger();
    stage.seconds = o.value(QStringLiteral("seconds")).toDouble();
    stage.retries = o.value(QStringLiteral("retries")).toInt();
    return stage;
}

bool appendLine(const QString &path, const QJsonObject &object, QString *errorString)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (errorString)
            *errorString = QObject::tr("Ordner %1 kann nicht angelegt werden").arg(info.absolutePath());
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (errorString)
            *errorString = file.errorString();
        return false;
    }
    // One write per line: appends from the GUI and a cron job don't interleave.
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (file.write(line) != line.size()) {
        if (errorString)
            *errorString = file.errorString();
        return false;
    }
    return true;
}

} // namespace

int Data::lastSpeedSeries() const
{
    int last = 0;
    for (const SpeedRun &run : speedRuns)
        last = std::max(last, run.series);
    return last;
}

int Data::lastLoadSeries() const
{
    int last = 0;
    for (const LoadSeries &series : loadSeries)
        last = std::max(last, series.id);
    return last;
}

QString defaultPath()
{
    const QString overridden = qEnvironmentVariable("BROADBAND_HISTORY");
    if (!overridden.isEmpty())
        return overridden;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("history.jsonl"));
}

QJsonObject toJson(const SpeedRun &run)
{
    const SpeedTestResult &r = run.result;
    QJsonObject o{
        {QStringLiteral("type"), kSpeedType},
        {QStringLiteral("time"), timeToString(run.time)},
        {QStringLiteral("server"), run.server},
        {QStringLiteral("series"), run.series},
        {QStringLiteral("index"), run.index},
        {QStringLiteral("streams"), run.streams},
        {QStringLiteral("durationSec"), run.durationSec},
        {QStringLiteral("pingMs"), r.pingMs},
        {QStringLiteral("jitterMs"), r.jitterMs},
        {QStringLiteral("downloadMbps"), r.downloadMbps},
        {QStringLiteral("uploadMbps"), r.uploadMbps},
        {QStringLiteral("downloadLatencyMs"), r.downloadLatencyMs},
        {QStringLiteral("uploadLatencyMs"), r.uploadLatencyMs},
        {QStringLiteral("downloadedBytes"), r.downloadedBytes},
        {QStringLiteral("uploadedBytes"), r.uploadedBytes},
        {QStringLiteral("retries"), r.retries},
    };
    if (!r.server.colo.isEmpty())
        o.insert(QStringLiteral("colo"), r.server.colo);
    if (run.ipVersion)
        o.insert(QStringLiteral("ipVersion"), run.ipVersion);
    return o;
}

std::optional<SpeedRun> speedRunFromJson(const QJsonObject &o)
{
    if (o.value(QStringLiteral("type")).toString() != kSpeedType)
        return std::nullopt;
    SpeedRun run;
    run.time = timeFromString(o.value(QStringLiteral("time")).toString());
    run.server = o.value(QStringLiteral("server")).toString();
    if (!run.time.isValid() || run.server.isEmpty())
        return std::nullopt;
    run.series = o.value(QStringLiteral("series")).toInt(1);
    run.index = o.value(QStringLiteral("index")).toInt(1);
    run.streams = o.value(QStringLiteral("streams")).toInt();
    run.durationSec = o.value(QStringLiteral("durationSec")).toInt();
    run.ipVersion = o.value(QStringLiteral("ipVersion")).toInt();

    SpeedTestResult &r = run.result;
    r.pingMs = o.value(QStringLiteral("pingMs")).toDouble();
    r.jitterMs = o.value(QStringLiteral("jitterMs")).toDouble();
    r.downloadMbps = o.value(QStringLiteral("downloadMbps")).toDouble();
    r.uploadMbps = o.value(QStringLiteral("uploadMbps")).toDouble();
    r.downloadLatencyMs = o.value(QStringLiteral("downloadLatencyMs")).toDouble(-1.0);
    r.uploadLatencyMs = o.value(QStringLiteral("uploadLatencyMs")).toDouble(-1.0);
    r.downloadedBytes = o.value(QStringLiteral("downloadedBytes")).toInteger();
    r.uploadedBytes = o.value(QStringLiteral("uploadedBytes")).toInteger();
    r.retries = o.value(QStringLiteral("retries")).toInt();
    r.server.colo = o.value(QStringLiteral("colo")).toString();
    return run;
}

QJsonObject toJson(const LoadSeries &series, const LoadRun &run)
{
    QJsonArray stages;
    for (const LoadStageResult &stage : run.stages)
        stages.append(stageToJson(stage));
    return QJsonObject{
        {QStringLiteral("type"), kLoadType},
        {QStringLiteral("time"), timeToString(run.time)},
        {QStringLiteral("server"), series.server},
        {QStringLiteral("series"), series.id},
        {QStringLiteral("upload"), series.upload},
        {QStringLiteral("contractMbps"), series.contractMbps},
        {QStringLiteral("stageDurationSec"), series.stageDurationSec},
        {QStringLiteral("idleLatencyMs"), run.idleLatencyMs},
        {QStringLiteral("stages"), stages},
    };
}

Data load(const QString &path, QString *errorString)
{
    Data data;
    QFile file(path);
    if (!file.exists())
        return data;
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorString)
            *errorString = file.errorString();
        return data;
    }

    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        const QString type = o.value(QStringLiteral("type")).toString();

        if (type == kSpeedType) {
            if (const auto run = speedRunFromJson(o))
                data.speedRuns.append(*run);
        } else if (type == kLoadType) {
            LoadRun run;
            run.time = timeFromString(o.value(QStringLiteral("time")).toString());
            run.idleLatencyMs = o.value(QStringLiteral("idleLatencyMs")).toDouble();
            const QJsonArray stages = o.value(QStringLiteral("stages")).toArray();
            for (const QJsonValue &stage : stages)
                run.stages.append(stageFromJson(stage.toObject()));
            if (!run.time.isValid() || run.stages.isEmpty())
                continue;

            const int id = o.value(QStringLiteral("series")).toInt();
            if (data.loadSeries.isEmpty() || data.loadSeries.last().id != id) {
                LoadSeries series;
                series.id = id;
                series.server = o.value(QStringLiteral("server")).toString();
                series.upload = o.value(QStringLiteral("upload")).toBool();
                series.contractMbps = o.value(QStringLiteral("contractMbps")).toInt();
                series.stageDurationSec = o.value(QStringLiteral("stageDurationSec")).toInt();
                data.loadSeries.append(series);
            }
            data.loadSeries.last().runs.append(run);
        }
    }
    return data;
}

bool append(const QString &path, const SpeedRun &run, QString *errorString)
{
    return appendLine(path, toJson(run), errorString);
}

bool append(const QString &path, const LoadSeries &series, const LoadRun &run, QString *errorString)
{
    return appendLine(path, toJson(series, run), errorString);
}

bool removeSpeedRuns(const QString &path, QString *errorString)
{
    QFile in(path);
    if (!in.exists())
        return true;
    if (!in.open(QIODevice::ReadOnly)) {
        if (errorString)
            *errorString = in.errorString();
        return false;
    }
    QByteArray kept;
    while (!in.atEnd()) {
        const QByteArray line = in.readLine();
        const QJsonObject o = QJsonDocument::fromJson(line.trimmed()).object();
        if (o.value(QStringLiteral("type")).toString() != kSpeedType && !line.trimmed().isEmpty())
            kept += line.endsWith('\n') ? line : line + '\n';
    }
    in.close();

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(kept) != kept.size() || !out.commit()) {
        if (errorString)
            *errorString = out.errorString();
        return false;
    }
    return true;
}

QString speedCsvHeader()
{
    return QStringLiteral("time,server,colo,ip_version,streams,duration_s,ping_ms,jitter_ms,download_mbps,"
                          "upload_mbps,download_latency_ms,upload_latency_ms,downloaded_bytes,uploaded_bytes,"
                          "retries");
}

QString toCsv(const SpeedRun &run)
{
    const SpeedTestResult &r = run.result;
    return QStringList{timeToString(run.time),
                       csvText(run.server),
                       csvText(r.server.colo),
                       run.ipVersion ? QString::number(run.ipVersion) : QString(),
                       QString::number(run.streams),
                       QString::number(run.durationSec),
                       number(r.pingMs),
                       number(r.jitterMs),
                       number(r.downloadMbps),
                       number(r.uploadMbps),
                       number(r.downloadLatencyMs),
                       number(r.uploadLatencyMs),
                       QString::number(r.downloadedBytes),
                       QString::number(r.uploadedBytes),
                       QString::number(r.retries)}
        .join(QLatin1Char(','));
}

QString loadCsvHeader()
{
    return QStringLiteral("time,server,direction,contract_mbps,clients,total_mbps,client_avg_mbps,client_min_mbps,"
                          "client_max_mbps,latency_ms,idle_latency_ms,bytes,retries");
}

QStringList toCsv(const LoadSeries &series, const LoadRun &run)
{
    QStringList lines;
    for (const LoadStageResult &s : run.stages) {
        lines << QStringList{timeToString(run.time),
                             csvText(series.server),
                             series.upload ? QStringLiteral("upload") : QStringLiteral("download"),
                             QString::number(series.contractMbps),
                             QString::number(s.clients),
                             number(s.totalMbps),
                             number(s.clientAvgMbps),
                             number(s.clientMinMbps),
                             number(s.clientMaxMbps),
                             number(s.latencyMs),
                             number(run.idleLatencyMs),
                             QString::number(s.bytes),
                             QString::number(s.retries)}
                     .join(QLatin1Char(','));
    }
    return lines;
}

} // namespace history
