#include "Report.h"

#include "Format.h"
#include "XlsxWriter.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QMap>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {

using Color = XlsxWriter::Color;
using Style = XlsxWriter::Style;

QString tr(const char *text, int n = -1)
{
    return QCoreApplication::translate("Report", text, nullptr, n);
}

QString percent(double fraction)
{
    return QStringLiteral("%1 %").arg(qRound(fraction * 100.0));
}

QString signedPercent(double fraction)
{
    const int value = qRound(fraction * 100.0);
    return QStringLiteral("%1%2 %").arg(value > 0 ? QStringLiteral("+") : value < 0 ? QStringLiteral("−") : QString())
        .arg(std::abs(value));
}

QString formatMetric(double value, report::Metric metric)
{
    return metric == report::Metric::Download || metric == report::Metric::Upload ? fmt::mbps(value) : fmt::ms(value);
}

// "https://speed.cloudflare.com · FRA · IPv4"
QString serverLabel(const SpeedRun &run)
{
    QStringList parts{run.server};
    if (!run.result.server.colo.isEmpty())
        parts << run.result.server.colo;
    if (run.ipVersion)
        parts << QStringLiteral("IPv%1").arg(run.ipVersion);
    return parts.join(QStringLiteral(" · "));
}

QString runLabel(const SpeedRun &run, int position)
{
    return tr("Messung %1 (%2)").arg(position).arg(run.time.toString(QStringLiteral("HH:mm:ss")));
}

const QList<report::Metric> kMetrics{report::Metric::Download, report::Metric::Upload, report::Metric::Ping,
                                     report::Metric::Jitter};

// --- Workbook design ----------------------------------------------------------

const Color kInk(0x1f2937);
const Color kMuted(0x6b7280);
const Color kAccent(0x2f80ed);
const Color kUpload(0x9b51e0);
const Color kPing(0xd9821e);
const Color kJitter(0x6b7280);
const Color kHeaderFill(0x2f3b4c);
const Color kZebra(0xf5f7fa);
const Color kLine(0xd9dde3);
const Color kGoodText(0x1e7b3a);
const Color kGoodFill(0xe7f6ec);
const Color kWarnText(0xa15c07);
const Color kWarnFill(0xfff4e5);
const Color kBadText(0xb42318);
const Color kBadFill(0xfde8e8);

// Line colors for the individual runs of a load test.
const QList<Color> kRunColors{kAccent,
                               kUpload,
                               kPing,
                               Color(0x2e9d4f),
                               Color(0xd63b3b),
                               Color(0x0e9fb5),
                               Color(0x8d6e63),
                               kJitter};

// More extra latency under load than this is clearly noticeable.
constexpr double kBufferbloatMs = 100.0;

const QString kMbpsFormat = QStringLiteral("0.0");
const QString kMsFormat = QStringLiteral("0.0");
const QString kPercentFormat = QStringLiteral("0%");
const QString kDeviationFormat = QStringLiteral("+0.0%;-0.0%;0.0%");
const QString kCvFormat = QStringLiteral("0.0%");
const QString kMbFormat = QStringLiteral("#,##0.0");
const QString kDateTimeFormat = QStringLiteral("dd.mm.yyyy hh:mm:ss");

Style titleStyle()
{
    Style s;
    s.bold = true;
    s.fontSize = 18;
    s.fontColor = kInk;
    return s;
}

Style subtitleStyle()
{
    Style s;
    s.italic = true;
    s.fontColor = kMuted;
    return s;
}

Style sectionStyle(const Color &color = kAccent)
{
    Style s;
    s.bold = true;
    s.fontSize = 13;
    s.fontColor = kInk;
    s.borders = Style::BorderBottom;
    s.borderColor = color;
    return s;
}

Style labelStyle()
{
    Style s;
    s.bold = true;
    s.fontColor = kMuted;
    return s;
}

Style valueStyle()
{
    Style s;
    s.fontColor = kInk;
    s.align = Style::Left;
    return s;
}

Style headerStyle(Style::Align align = Style::Center)
{
    Style s;
    s.bold = true;
    s.fontColor = 0xffffff;
    s.fill = kHeaderFill;
    s.align = align;
    s.wrap = true;
    s.borders = Style::BorderAll;
    s.borderColor = kHeaderFill;
    return s;
}

Style cellStyle(bool zebra, const QString &format = {}, Style::Align align = Style::Right)
{
    Style s;
    s.numberFormat = format;
    s.fontColor = kInk;
    s.align = align;
    if (zebra)
        s.fill = kZebra;
    s.borders = Style::BorderBottom;
    s.borderColor = kLine;
    return s;
}

Style rated(Style s, Finding::Level level)
{
    switch (level) {
    case Finding::Good:
        s.fontColor = kGoodText;
        s.fill = kGoodFill;
        s.bold = true;
        break;
    case Finding::Warning:
        s.fontColor = kWarnText;
        s.fill = kWarnFill;
        s.bold = true;
        break;
    case Finding::Bad:
        s.fontColor = kBadText;
        s.fill = kBadFill;
        s.bold = true;
        break;
    case Finding::Info:
        break;
    }
    return s;
}

Finding::Level contractLevel(double fraction)
{
    return fraction >= kContractThreshold ? Finding::Good : fraction >= 0.5 ? Finding::Warning : Finding::Bad;
}

// A trend must be clear and, for latencies, also large enough to matter:
// 14 -> 16 ms is +14 % but nobody notices.
bool significantTrend(const stats::Trend &t, const QList<double> &values, report::Metric metric)
{
    if (!t.significant())
        return false;
    if (report::higherIsBetter(metric))
        return true;
    return std::abs(t.slope * double(values.size() - 1)) >= 5.0;
}

// Spread and rating of the regular runs, i.e. without flukes.
stats::Summary regularSummary(const QList<double> &values)
{
    return stats::summarize(stats::withoutOutliers(values));
}

// Trend of a metric as short text plus its rating.
QPair<QString, Finding::Level> trendCell(const QList<double> &values, report::Metric metric)
{
    if (values.size() < 4)
        return {QStringLiteral("–"), Finding::Info};
    const stats::Trend t = stats::trend(values);
    if (!significantTrend(t, values, metric))
        return {tr("kein Trend"), Finding::Good};
    const bool rising = t.relativeChange > 0;
    const bool good = rising == report::higherIsBetter(metric);
    return {(rising ? tr("steigend %1") : tr("fallend %1")).arg(signedPercent(t.relativeChange)),
            good ? Finding::Info : Finding::Bad};
}

// Merged row with one finding, height estimated from the text length since
// Excel does not auto-fit merged cells.
void writeFinding(XlsxWriter::Sheet &sheet, int row, int lastColumn, double widthChars, const Finding &finding)
{
    Style s;
    s.wrap = true;
    s.align = Style::Left;
    s.fontColor = kInk;
    switch (finding.level) {
    case Finding::Good:
        s.fontColor = kGoodText;
        break;
    case Finding::Warning:
        s.fontColor = kWarnText;
        break;
    case Finding::Bad:
        s.fontColor = kBadText;
        s.bold = true;
        break;
    case Finding::Info:
        break;
    }
    static const QString bullets[] = {QStringLiteral("•"), QStringLiteral("✔"), QStringLiteral("⚠"),
                                      QStringLiteral("✘")};
    const QString text = bullets[finding.level] + QLatin1Char(' ') + finding.text;
    sheet.setText(row, 0, text, s);
    for (int c = 1; c <= lastColumn; ++c)
        sheet.setStyle(row, c, s);
    sheet.mergeCells(row, 0, row, lastColumn);
    const int lines = std::max(1, int(std::ceil(double(text.size()) * 1.1 / widthChars)));
    sheet.setRowHeight(row, 15.0 * lines + 3.0);
}

void writeHeader(XlsxWriter::Sheet &sheet, int row, const QStringList &headers)
{
    for (int c = 0; c < headers.size(); ++c)
        sheet.setText(row, c, headers[c], headerStyle(c == 0 ? Style::Left : Style::Center));
    sheet.setRowHeight(row, 32);
}

void writeKeyValue(XlsxWriter::Sheet &sheet, int row, int lastColumn, const QString &key, const QString &value)
{
    sheet.setText(row, 0, key, labelStyle());
    sheet.setText(row, 1, value, valueStyle());
    sheet.mergeCells(row, 1, row, lastColumn);
}

void writeSection(XlsxWriter::Sheet &sheet, int row, int lastColumn, const QString &title)
{
    sheet.setText(row, 0, title, sectionStyle());
    for (int c = 1; c <= lastColumn; ++c)
        sheet.setStyle(row, c, sectionStyle());
    sheet.setRowHeight(row, 22);
}

QString timeSpan(const QDateTime &first, const QDateTime &last)
{
    const QString date = QStringLiteral("dd.MM.yyyy");
    const QString time = QStringLiteral("HH:mm");
    if (first.date() == last.date())
        return QStringLiteral("%1, %2 – %3 Uhr").arg(first.toString(date), first.toString(time), last.toString(time));
    return QStringLiteral("%1 %2 – %3 %4 Uhr")
        .arg(first.toString(date), first.toString(time), last.toString(date), last.toString(time));
}

// Speed test runs grouped by server, in order of first appearance.
QList<QList<SpeedRun>> groupByServer(const QList<SpeedRun> &runs)
{
    QList<QList<SpeedRun>> groups;
    QStringList servers;
    for (const SpeedRun &run : runs) {
        qsizetype i = servers.indexOf(run.server);
        if (i < 0) {
            servers << run.server;
            groups.append(QList<SpeedRun>());
            i = groups.size() - 1;
        }
        groups[i].append(run);
    }
    return groups;
}

QString clientsWord(bool upload)
{
    return upload ? tr("Uploads") : tr("Downloads");
}

// --- Overview sheet -------------------------------------------------------------

constexpr int kOverviewColumns = 10;
constexpr double kOverviewWidths[kOverviewColumns] = {22, 11, 11, 11, 11, 11, 12, 26, 20, 16};

double overviewWidth()
{
    double sum = 0;
    for (double w : kOverviewWidths)
        sum += w;
    return sum;
}

int writeSpeedOverview(XlsxWriter::Sheet &sheet, int row, const QList<SpeedRun> &runs)
{
    const int last = kOverviewColumns - 1;
    writeKeyValue(sheet, row++, last, tr("Server"), runs.first().server);
    writeKeyValue(sheet, row++, last, tr("Messungen"), QString::number(runs.size()));
    writeKeyValue(sheet, row++, last, tr("Zeitraum"), timeSpan(runs.first().time, runs.last().time));
    const bool uniform = std::all_of(runs.begin(), runs.end(), [&](const SpeedRun &r) {
        return r.streams == runs.first().streams && r.durationSec == runs.first().durationSec;
    });
    writeKeyValue(sheet, row++, last, tr("Einstellungen"),
                  uniform ? tr("%1 Streams · %2 s je Richtung").arg(runs.first().streams).arg(runs.first().durationSec)
                          : tr("unterschiedlich, siehe Blatt „Speedtest“"));
    ++row;

    writeHeader(sheet, row++,
                {tr("Kennzahl"), tr("Mittelwert"), tr("Median"), tr("Minimum"), tr("Maximum"), tr("Std.-Abw."),
                 tr("Schwankung"), tr("Bewertung"), tr("Ausreißer"), tr("Trend")});
    for (qsizetype m = 0; m < kMetrics.size(); ++m) {
        const report::Metric metric = kMetrics[m];
        const bool zebra = m % 2;
        const QList<double> values = report::metricValues(runs, metric);
        const stats::Summary s = stats::summarize(values);
        const QString format = metric == report::Metric::Download || metric == report::Metric::Upload ? kMbpsFormat
                                                                                                       : kMsFormat;

        Style name = cellStyle(zebra, {}, Style::Left);
        name.bold = true;
        sheet.setText(row, 0, QStringLiteral("%1 (%2)").arg(report::metricName(metric), report::metricUnit(metric)),
                      name);
        sheet.setNumber(row, 1, s.mean, cellStyle(zebra, format));
        Style median = cellStyle(zebra, format);
        median.bold = true;
        sheet.setNumber(row, 2, s.median, median);
        sheet.setNumber(row, 3, s.min, cellStyle(zebra, format));
        sheet.setNumber(row, 4, s.max, cellStyle(zebra, format));
        if (s.count > 1) {
            sheet.setNumber(row, 5, s.stddev, cellStyle(zebra, format));
            sheet.setNumber(row, 6, s.cv(), cellStyle(zebra, kCvFormat));
        } else {
            sheet.setText(row, 5, QStringLiteral("–"), cellStyle(zebra));
            sheet.setText(row, 6, QStringLiteral("–"), cellStyle(zebra));
        }

        // Ping and jitter are small numbers where a large relative spread is normal.
        const bool rateStability = metric == report::Metric::Download || metric == report::Metric::Upload;
        const stats::Summary regular = regularSummary(values);
        const stats::Stability stability = stats::stability(regular);
        if (rateStability && stability != stats::Stability::Unknown) {
            QString text = report::stabilityText(stability);
            if (regular.count < s.count)
                text += tr(" ohne Ausreißer");
            sheet.setText(row, 7, text,
                          rated(cellStyle(zebra, {}, Style::Center), report::stabilityLevel(stability)));
        } else {
            sheet.setText(row, 7, QStringLiteral("–"), cellStyle(zebra, {}, Style::Center));
        }

        QStringList outliers;
        for (const SpeedRun &run : runs) {
            if (stats::isOutlier(report::metricValue(run, metric), s))
                outliers << run.time.toString(QStringLiteral("HH:mm:ss"));
        }
        if (s.count < 3)
            sheet.setText(row, 8, QStringLiteral("–"), cellStyle(zebra, {}, Style::Center));
        else if (outliers.isEmpty())
            sheet.setText(row, 8, tr("keine"), rated(cellStyle(zebra, {}, Style::Center), Finding::Good));
        else
            sheet.setText(row, 8, QStringLiteral("%1 (%2)").arg(outliers.size()).arg(outliers.join(QStringLiteral(", "))),
                          rated(cellStyle(zebra, {}, Style::Center), Finding::Warning));

        const auto [trendText, trendLevel] = trendCell(values, metric);
        sheet.setText(row, 9, trendText,
                      trendLevel == Finding::Info ? cellStyle(zebra, {}, Style::Center)
                                                  : rated(cellStyle(zebra, {}, Style::Center), trendLevel));
        ++row;
    }

    ++row;
    for (const Finding &finding : report::speedFindings(runs) + report::loadedLatencyFindings(runs))
        writeFinding(sheet, row++, last, overviewWidth(), finding);
    return row;
}

int writeLoadOverview(XlsxWriter::Sheet &sheet, int row, const LoadSeries &load)
{
    const int last = kOverviewColumns - 1;
    const QList<LoadStageResult> medians = load.medianStages();
    QStringList stages;
    for (const LoadStageResult &stage : medians)
        stages << QString::number(stage.clients);

    writeKeyValue(sheet, row++, last, tr("Server"), load.server);
    writeKeyValue(sheet, row++, last, tr("Vertrag"),
                  load.contractMbps > 0 ? tr("%1 Mbit/s").arg(load.contractMbps) : tr("nicht angegeben"));
    writeKeyValue(sheet, row++, last, tr("Gleichzeitige Clients"),
                  tr("%1 · je Stufe %2 s").arg(stages.join(QStringLiteral(", "))).arg(load.stageDurationSec));
    writeKeyValue(sheet, row++, last, tr("Durchläufe"),
                  tr("%1 · %2").arg(load.runs.size()).arg(timeSpan(load.runs.first().time, load.runs.last().time)));
    writeKeyValue(sheet, row++, last, tr("Ping ohne Last"), tr("%1 ms").arg(fmt::ms(load.medianIdleLatencyMs())));
    ++row;

    const bool several = load.runs.size() > 1;
    writeHeader(sheet, row++,
                {tr("Gleichzeitige Clients"), several ? tr("Gesamt (Median)") : tr("Gesamt"), tr("Minimum"),
                 tr("Maximum"), tr("vom Vertrag"), tr("Ø je Client"), tr("Schwankung"), tr("Bewertung"),
                 tr("Ping unter Last (ms)"), tr("langsamster Client")});
    for (qsizetype i = 0; i < medians.size(); ++i) {
        const bool zebra = i % 2;
        const LoadStageResult &median = medians[i];
        QList<double> totals;
        for (const LoadRun &run : load.runs) {
            if (i < run.stages.size())
                totals << run.stages[i].totalMbps;
        }
        const stats::Summary s = stats::summarize(totals);

        Style clients = cellStyle(zebra, {}, Style::Left);
        clients.bold = true;
        sheet.setText(row, 0, tr("%1 × %2").arg(median.clients).arg(clientsWord(load.upload)), clients);
        Style total = cellStyle(zebra, kMbpsFormat);
        total.bold = true;
        sheet.setNumber(row, 1, median.totalMbps, total);
        sheet.setNumber(row, 2, s.min, cellStyle(zebra, kMbpsFormat));
        sheet.setNumber(row, 3, s.max, cellStyle(zebra, kMbpsFormat));
        if (load.contractMbps > 0) {
            const double fraction = median.totalMbps / load.contractMbps;
            sheet.setNumber(row, 4, fraction, rated(cellStyle(zebra, kPercentFormat), contractLevel(fraction)));
        } else {
            sheet.setText(row, 4, QStringLiteral("–"), cellStyle(zebra));
        }
        sheet.setNumber(row, 5, median.clientAvgMbps, cellStyle(zebra, kMbpsFormat));
        if (s.count > 1)
            sheet.setNumber(row, 6, s.cv(), cellStyle(zebra, kCvFormat));
        else
            sheet.setText(row, 6, QStringLiteral("–"), cellStyle(zebra));
        sheet.setNumber(row, 9, median.clientMinMbps, cellStyle(zebra, kMbpsFormat));
        if (median.latencyMs >= 0)
            sheet.setNumber(row, 8, median.latencyMs, cellStyle(zebra, kMsFormat));
        else
            sheet.setText(row, 8, QStringLiteral("–"), cellStyle(zebra));
        const stats::Summary regular = regularSummary(totals);
        const stats::Stability stability = stats::stability(regular);
        if (stability != stats::Stability::Unknown)
            sheet.setText(row, 7,
                          report::stabilityText(stability) + (regular.count < s.count ? tr(" ohne Ausreißer") : QString()),
                          rated(cellStyle(zebra, {}, Style::Center), report::stabilityLevel(stability)));
        else
            sheet.setText(row, 7, QStringLiteral("–"), cellStyle(zebra, {}, Style::Center));
        ++row;
    }

    ++row;
    for (const Finding &finding : report::loadFindings(load))
        writeFinding(sheet, row++, last, overviewWidth(), finding);
    return row;
}

void writeOverview(XlsxWriter::Sheet &sheet, const QList<SpeedRun> &speedRuns, const LoadSeries &load)
{
    const int last = kOverviewColumns - 1;
    sheet.setShowGridLines(false);
    sheet.setTabColor(kInk);
    sheet.setLandscape(true);
    for (int c = 0; c < kOverviewColumns; ++c)
        sheet.setColumnWidth(c, kOverviewWidths[c]);

    sheet.setText(0, 0, tr("Broadband – Messbericht"), titleStyle());
    sheet.mergeCells(0, 0, 0, last);
    sheet.setRowHeight(0, 30);
    sheet.setText(1, 0,
                  tr("Erstellt am %1 mit broadband %2")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy, HH:mm")),
                           QStringLiteral(BROADBAND_VERSION)),
                  subtitleStyle());
    sheet.mergeCells(1, 0, 1, last);

    int row = 3;
    const QList<QList<SpeedRun>> groups = groupByServer(speedRuns);
    for (const QList<SpeedRun> &group : groups) {
        writeSection(sheet, row++, last,
                     groups.size() > 1 ? tr("Speedtest – %1").arg(QUrl(group.first().server).host()) : tr("Speedtest"));
        row = writeSpeedOverview(sheet, row + 1, group) + 1;
    }
    if (!load.isEmpty()) {
        writeSection(sheet, row++, last,
                     load.upload ? tr("Bandbreite unter Last – Upload") : tr("Bandbreite unter Last – Download"));
        row = writeLoadOverview(sheet, row + 1, load) + 1;
    }

    writeSection(sheet, row++, last, tr("Erläuterung"));
    ++row;
    const QList<Finding> notes{
        {Finding::Info, tr("Median: mittlerer Wert der Messreihe. Er ist unempfindlich gegen einzelne Ausreißer "
                           "und deshalb aussagekräftiger als der Mittelwert.")},
        {Finding::Info, tr("Schwankung: Standardabweichung im Verhältnis zum Mittelwert (Variationskoeffizient). "
                           "Unter 5 % sehr stabil, unter 15 % stabil, unter 30 % schwankend, darüber instabil.")},
        {Finding::Info, tr("Ausreißer: Messungen, die mehr als drei robuste Standardabweichungen (MAD) und mehr als "
                           "10 % vom Median abweichen. Ab 3 Messungen.")},
        {Finding::Info, tr("Trend: Ausgleichsgerade über die Messreihe; angezeigt ab ±10 % Veränderung und einem "
                           "Bestimmtheitsmaß R² ≥ 0,5. Ab 4 Messungen.")},
        {Finding::Info, tr("Vertrag erreicht ab 90 % der vertraglichen Bandbreite (Vorgabe der Bundesnetzagentur). "
                           "Rechtlich verwertbar ist nur die offizielle Breitbandmessung (breitbandmessung.de).")},
    };
    for (const Finding &note : notes)
        writeFinding(sheet, row++, last, overviewWidth(), note);
}

// --- Speed test sheet -----------------------------------------------------------

void writeSpeedSheet(XlsxWriter::Sheet &sheet, const QList<SpeedRun> &runs)
{
    sheet.setTabColor(kAccent);
    sheet.setLandscape(true);
    sheet.freezeRows(1);
    const QStringList headers{tr("Nr."),          tr("Serie"),           tr("Zeitpunkt"),         tr("Server"),
                              tr("Streams"),      tr("Dauer (s)"),       tr("Ping (ms)"),         tr("Jitter (ms)"),
                              tr("Download (Mbit/s)"), tr("Upload (Mbit/s)"), tr("Download Δ Median"),
                              tr("Upload Δ Median"), tr("Daten ↓ (MB)"),  tr("Daten ↑ (MB)"),
                              tr("Ping Last ↓ (ms)"), tr("Ping Last ↑ (ms)"), tr("Neu verbunden"), tr("Hinweis")};
    const double widths[] = {6, 8, 19, 30, 9, 9, 11, 11, 13, 13, 13, 13, 12, 12, 13, 13, 11, 34};
    for (int c = 0; c < headers.size(); ++c)
        sheet.setColumnWidth(c, widths[c]);
    writeHeader(sheet, 0, headers);

    // Medians and outliers per server, so different targets are not mixed.
    struct GroupStats
    {
        QMap<report::Metric, stats::Summary> summaries;
    };
    QHash<QString, GroupStats> groups;
    for (const QList<SpeedRun> &group : groupByServer(runs)) {
        GroupStats &g = groups[group.first().server];
        for (report::Metric metric : kMetrics)
            g.summaries.insert(metric, stats::summarize(report::metricValues(group, metric)));
    }
    const int metricColumn[] = {8, 9, 6, 7}; // Download, Upload, Ping, Jitter

    for (qsizetype i = 0; i < runs.size(); ++i) {
        const SpeedRun &run = runs[i];
        const int row = int(i) + 1;
        const bool zebra = i % 2;
        const GroupStats &g = groups[run.server];

        sheet.setNumber(row, 0, double(i + 1), cellStyle(zebra, QStringLiteral("0"), Style::Center));
        sheet.setText(row, 1, QStringLiteral("%1.%2").arg(run.series).arg(run.index),
                      cellStyle(zebra, {}, Style::Center));
        sheet.setDateTime(row, 2, run.time, cellStyle(zebra, kDateTimeFormat, Style::Left));
        sheet.setText(row, 3, serverLabel(run), cellStyle(zebra, {}, Style::Left));
        sheet.setNumber(row, 4, run.streams, cellStyle(zebra, QStringLiteral("0")));
        sheet.setNumber(row, 5, run.durationSec, cellStyle(zebra, QStringLiteral("0")));

        QStringList notes;
        for (qsizetype m = 0; m < kMetrics.size(); ++m) {
            const report::Metric metric = kMetrics[m];
            const double value = report::metricValue(run, metric);
            const stats::Summary s = g.summaries.value(metric);
            Style style = cellStyle(zebra, metric == report::Metric::Download || metric == report::Metric::Upload
                                               ? kMbpsFormat
                                               : kMsFormat);
            if (stats::isOutlier(value, s)) {
                style = rated(style, Finding::Bad);
                notes << report::metricName(metric);
            }
            sheet.setNumber(row, metricColumn[m], value, style);
        }
        for (const report::Metric metric : {report::Metric::Download, report::Metric::Upload}) {
            const stats::Summary s = g.summaries.value(metric);
            const int column = metric == report::Metric::Download ? 10 : 11;
            if (s.count > 1 && s.median > 0)
                sheet.setNumber(row, column, (report::metricValue(run, metric) - s.median) / s.median,
                                cellStyle(zebra, kDeviationFormat));
            else
                sheet.setText(row, column, QStringLiteral("–"), cellStyle(zebra));
        }
        sheet.setNumber(row, 12, double(run.result.downloadedBytes) / 1e6, cellStyle(zebra, kMbFormat));
        sheet.setNumber(row, 13, double(run.result.uploadedBytes) / 1e6, cellStyle(zebra, kMbFormat));
        for (const auto &[column, latency] : {std::pair{14, run.result.downloadLatencyMs},
                                              std::pair{15, run.result.uploadLatencyMs}}) {
            if (latency >= 0)
                sheet.setNumber(row, column, latency, cellStyle(zebra, kMsFormat));
            else
                sheet.setText(row, column, QStringLiteral("–"), cellStyle(zebra));
        }
        sheet.setNumber(row, 16, run.result.retries,
                        run.result.retries > 0 ? rated(cellStyle(zebra, QStringLiteral("0")), Finding::Warning)
                                               : cellStyle(zebra, QStringLiteral("0")));
        sheet.setText(row, 17, notes.isEmpty() ? QString() : tr("Ausreißer: %1").arg(notes.join(QStringLiteral(", "))),
                      notes.isEmpty() ? cellStyle(zebra, {}, Style::Left)
                                      : rated(cellStyle(zebra, {}, Style::Left), Finding::Bad));
    }

    const int lastDataRow = int(runs.size());
    sheet.setAutoFilter(0, 0, lastDataRow, int(headers.size()) - 1);

    // Live formulas below the data, only if all runs used the same server.
    int row = lastDataRow + 2;
    if (groups.size() == 1) {
        struct Aggregate
        {
            const char *label;
            const char *function;
        };
        const Aggregate aggregates[] = {{"Mittelwert", "AVERAGE"}, {"Median", "MEDIAN"}, {"Minimum", "MIN"},
                                        {"Maximum", "MAX"},        {"Std.-Abw.", "STDEV"}};
        const stats::Summary *summaries[] = {&groups.begin()->summaries[report::Metric::Download],
                                             &groups.begin()->summaries[report::Metric::Upload],
                                             &groups.begin()->summaries[report::Metric::Ping],
                                             &groups.begin()->summaries[report::Metric::Jitter]};
        for (const Aggregate &aggregate : aggregates) {
            const QString function = QLatin1String(aggregate.function);
            if (function == QLatin1String("STDEV") && runs.size() < 2)
                continue;
            Style label = labelStyle();
            label.align = Style::Right;
            sheet.setText(row, 5, tr(aggregate.label), label);
            for (qsizetype m = 0; m < kMetrics.size(); ++m) {
                const stats::Summary &s = *summaries[m];
                const double cached = function == QLatin1String("AVERAGE")  ? s.mean
                                      : function == QLatin1String("MEDIAN") ? s.median
                                      : function == QLatin1String("MIN")    ? s.min
                                      : function == QLatin1String("MAX")    ? s.max
                                                                            : s.stddev;
                const int column = metricColumn[m];
                Style style = cellStyle(false, m < 2 ? kMbpsFormat : kMsFormat);
                style.bold = true;
                style.borders = Style::BorderTop;
                sheet.setFormula(row, column,
                                 QStringLiteral("%1(%2:%3)")
                                     .arg(function, XlsxWriter::cellRef(1, column), XlsxWriter::cellRef(lastDataRow, column)),
                                 cached, style);
            }
            ++row;
        }
    }

    // Charts: the "red thread" through the series at a glance.
    row += 1;
    XlsxWriter::LineChart throughput;
    throughput.title = tr("Download und Upload je Messung");
    throughput.xTitle = tr("Messung");
    throughput.yTitle = tr("Mbit/s");
    throughput.categoryColumn = 0;
    throughput.categoryFirstRow = 1;
    throughput.categoryLastRow = lastDataRow;
    throughput.series = {{tr("Download"), 8, 1, lastDataRow, kAccent}, {tr("Upload"), 9, 1, lastDataRow, kUpload}};
    throughput.fromRow = row;
    throughput.fromColumn = 0;
    throughput.toRow = row + 20;
    throughput.toColumn = 8;
    sheet.addLineChart(throughput);

    XlsxWriter::LineChart latency = throughput;
    latency.title = tr("Ping und Jitter je Messung");
    latency.yTitle = tr("ms");
    latency.series = {{tr("Ping"), 6, 1, lastDataRow, kPing}, {tr("Jitter"), 7, 1, lastDataRow, kJitter}};
    latency.fromColumn = 8;
    latency.toColumn = 15;
    sheet.addLineChart(latency);
}

// --- Load test sheet ------------------------------------------------------------

void writeLoadSheet(XlsxWriter::Sheet &sheet, const LoadSeries &load)
{
    sheet.setTabColor(kUpload);
    sheet.setLandscape(true);
    sheet.freezeRows(1);
    const QStringList headers{tr("Durchlauf"),         tr("Zeitpunkt"),           tr("Clients"),
                              tr("Gesamt (Mbit/s)"),   tr("vom Vertrag"),         tr("Ø je Client (Mbit/s)"),
                              tr("Min je Client"),     tr("Max je Client"),       tr("Ping unter Last (ms)"),
                              tr("Ping ohne Last (ms)"), tr("Datenmenge (MB)"),   tr("Dauer (s)"),
                              tr("Hinweis")};
    const double widths[] = {10, 19, 9, 13, 11, 13, 12, 12, 13, 13, 13, 9, 30};
    for (int c = 0; c < headers.size(); ++c)
        sheet.setColumnWidth(c, widths[c]);
    writeHeader(sheet, 0, headers);

    // Outliers per stage across runs
    const qsizetype stageCount = load.runs.first().stages.size();
    QList<stats::Summary> stageStats;
    for (qsizetype i = 0; i < stageCount; ++i) {
        QList<double> totals;
        for (const LoadRun &run : load.runs) {
            if (i < run.stages.size())
                totals << run.stages[i].totalMbps;
        }
        stageStats << stats::summarize(totals);
    }

    int row = 1;
    QList<QPair<int, int>> runRows; // first and last row of each run
    for (qsizetype r = 0; r < load.runs.size(); ++r) {
        const LoadRun &run = load.runs[r];
        const bool zebra = r % 2; // shade whole runs, not rows, to make the grouping visible
        const int first = row;
        for (qsizetype i = 0; i < run.stages.size(); ++i) {
            const LoadStageResult &stage = run.stages[i];
            sheet.setNumber(row, 0, double(r + 1), cellStyle(zebra, QStringLiteral("0"), Style::Center));
            sheet.setDateTime(row, 1, run.time, cellStyle(zebra, kDateTimeFormat, Style::Left));
            sheet.setText(row, 2, QStringLiteral("%1×").arg(stage.clients), cellStyle(zebra, {}, Style::Center));

            const bool outlier = i < stageStats.size() && stats::isOutlier(stage.totalMbps, stageStats[i]);
            Style total = cellStyle(zebra, kMbpsFormat);
            total.bold = true;
            sheet.setNumber(row, 3, stage.totalMbps, outlier ? rated(total, Finding::Bad) : total);
            if (load.contractMbps > 0) {
                const double fraction = stage.totalMbps / load.contractMbps;
                sheet.setNumber(row, 4, fraction, rated(cellStyle(zebra, kPercentFormat), contractLevel(fraction)));
            } else {
                sheet.setText(row, 4, QStringLiteral("–"), cellStyle(zebra));
            }
            sheet.setNumber(row, 5, stage.clientAvgMbps, cellStyle(zebra, kMbpsFormat));
            sheet.setNumber(row, 6, stage.clientMinMbps, cellStyle(zebra, kMbpsFormat));
            sheet.setNumber(row, 7, stage.clientMaxMbps, cellStyle(zebra, kMbpsFormat));
            if (stage.latencyMs >= 0)
                sheet.setNumber(row, 8, stage.latencyMs, cellStyle(zebra, kMsFormat));
            else
                sheet.setText(row, 8, QStringLiteral("–"), cellStyle(zebra));
            sheet.setNumber(row, 9, run.idleLatencyMs, cellStyle(zebra, kMsFormat));
            sheet.setNumber(row, 10, double(stage.bytes) / 1e6, cellStyle(zebra, kMbFormat));
            sheet.setNumber(row, 11, stage.seconds, cellStyle(zebra, QStringLiteral("0.0")));
            sheet.setText(row, 12, outlier ? tr("Ausreißer gegenüber den anderen Durchläufen") : QString(),
                          outlier ? rated(cellStyle(zebra, {}, Style::Left), Finding::Bad)
                                  : cellStyle(zebra, {}, Style::Left));
            ++row;
        }
        runRows.append({first, row - 1});
    }
    sheet.setAutoFilter(0, 0, row - 1, int(headers.size()) - 1);

    // One line per run over the stages: do the runs tell the same story?
    XlsxWriter::LineChart chart;
    chart.title = load.runs.size() > 1 ? tr("Gesamtdurchsatz je Stufe und Durchlauf") : tr("Gesamtdurchsatz je Stufe");
    chart.xTitle = tr("Gleichzeitige Clients");
    chart.yTitle = tr("Mbit/s");
    chart.categoryColumn = 2;
    chart.categoryFirstRow = runRows.first().first;
    chart.categoryLastRow = runRows.first().second;
    for (qsizetype r = 0; r < runRows.size(); ++r) {
        chart.series.append({load.runs.size() > 1 ? tr("Durchlauf %1").arg(r + 1) : tr("Gesamt"), 3,
                             runRows[r].first, runRows[r].second, kRunColors[r % kRunColors.size()]});
    }
    chart.fromRow = row + 1;
    chart.fromColumn = 0;
    chart.toRow = row + 22;
    chart.toColumn = 9;
    sheet.addLineChart(chart);
}

} // namespace

// --- LoadSeries -----------------------------------------------------------------

QList<LoadStageResult> LoadSeries::medianStages() const
{
    if (runs.isEmpty())
        return {};
    if (runs.size() == 1)
        return runs.first().stages;

    QList<LoadStageResult> result;
    for (qsizetype i = 0; i < runs.first().stages.size(); ++i) {
        QList<double> total, avg, min, max, latency, seconds;
        qint64 bytes = 0;
        int count = 0;
        for (const LoadRun &run : runs) {
            if (i >= run.stages.size())
                continue;
            const LoadStageResult &s = run.stages[i];
            total << s.totalMbps;
            avg << s.clientAvgMbps;
            min << s.clientMinMbps;
            max << s.clientMaxMbps;
            seconds << s.seconds;
            if (s.latencyMs >= 0)
                latency << s.latencyMs;
            bytes += s.bytes;
            ++count;
        }
        LoadStageResult m;
        m.clients = runs.first().stages[i].clients;
        m.totalMbps = stats::median(total);
        m.clientAvgMbps = stats::median(avg);
        m.clientMinMbps = stats::median(min);
        m.clientMaxMbps = stats::median(max);
        m.latencyMs = latency.isEmpty() ? -1.0 : stats::median(latency);
        m.bytes = count ? bytes / count : 0;
        m.seconds = stats::median(seconds);
        result << m;
    }
    return result;
}

double LoadSeries::medianIdleLatencyMs() const
{
    QList<double> values;
    for (const LoadRun &run : runs)
        values << run.idleLatencyMs;
    return stats::median(values);
}

// --- Texts ----------------------------------------------------------------------

QString report::metricName(Metric metric)
{
    switch (metric) {
    case Metric::Download:
        return tr("Download");
    case Metric::Upload:
        return tr("Upload");
    case Metric::Ping:
        return tr("Ping");
    case Metric::Jitter:
        return tr("Jitter");
    }
    return {};
}

QString report::metricUnit(Metric metric)
{
    return metric == Metric::Download || metric == Metric::Upload ? tr("Mbit/s") : tr("ms");
}

bool report::higherIsBetter(Metric metric)
{
    return metric == Metric::Download || metric == Metric::Upload;
}

double report::metricValue(const SpeedRun &run, Metric metric)
{
    switch (metric) {
    case Metric::Download:
        return run.result.downloadMbps;
    case Metric::Upload:
        return run.result.uploadMbps;
    case Metric::Ping:
        return run.result.pingMs;
    case Metric::Jitter:
        return run.result.jitterMs;
    }
    return 0.0;
}

QList<double> report::metricValues(const QList<SpeedRun> &runs, Metric metric)
{
    QList<double> values;
    values.reserve(runs.size());
    for (const SpeedRun &run : runs)
        values << metricValue(run, metric);
    return values;
}

QString report::stabilityText(stats::Stability stability)
{
    switch (stability) {
    case stats::Stability::VeryStable:
        return tr("sehr stabil");
    case stats::Stability::Stable:
        return tr("stabil");
    case stats::Stability::Fluctuating:
        return tr("schwankend");
    case stats::Stability::Unstable:
        return tr("instabil");
    case stats::Stability::Unknown:
        break;
    }
    return QStringLiteral("–");
}

Finding::Level report::stabilityLevel(stats::Stability stability)
{
    switch (stability) {
    case stats::Stability::VeryStable:
    case stats::Stability::Stable:
        return Finding::Good;
    case stats::Stability::Fluctuating:
        return Finding::Warning;
    case stats::Stability::Unstable:
        return Finding::Bad;
    case stats::Stability::Unknown:
        break;
    }
    return Finding::Info;
}

QList<Finding> report::speedFindings(const QList<SpeedRun> &runs)
{
    QList<Finding> findings;
    if (runs.isEmpty())
        return findings;

    const int n = int(runs.size());
    const stats::Summary down = stats::summarize(metricValues(runs, Metric::Download));
    const stats::Summary up = stats::summarize(metricValues(runs, Metric::Upload));
    const stats::Summary ping = stats::summarize(metricValues(runs, Metric::Ping));

    if (n == 1) {
        findings << Finding{Finding::Info, tr("Einzelmessung: %1 Mbit/s Download, %2 Mbit/s Upload, %3 ms Ping.")
                                               .arg(fmt::mbps(down.median), fmt::mbps(up.median), fmt::ms(ping.median))};
    } else {
        findings << Finding{Finding::Info, tr("Median aus %n Messungen: %1 Mbit/s Download, %2 Mbit/s Upload, "
                                              "%3 ms Ping.",
                                              n)
                                               .arg(fmt::mbps(down.median), fmt::mbps(up.median), fmt::ms(ping.median))};
    }
    if (n < 3) {
        findings << Finding{Finding::Info, tr("Für eine belastbare Aussage mindestens 3, besser 5 Messungen "
                                              "nacheinander durchführen. Erst dann lassen sich Ausreißer erkennen.")};
        if (n < 2)
            return findings;
    }

    // Outliers: single runs far away from the rest
    int outlierCount = 0;
    for (qsizetype i = 0; i < runs.size(); ++i) {
        QStringList parts;
        for (const Metric metric : {Metric::Download, Metric::Upload, Metric::Ping}) {
            const stats::Summary s = stats::summarize(metricValues(runs, metric));
            const double value = metricValue(runs[i], metric);
            if (stats::isOutlier(value, s)) {
                parts << tr("%1 %2 statt ~%3 %4")
                             .arg(metricName(metric), formatMetric(value, metric), formatMetric(s.median, metric),
                                  metricUnit(metric));
            }
        }
        if (!parts.isEmpty()) {
            ++outlierCount;
            findings << Finding{Finding::Warning, tr("%1 weicht deutlich ab (%2). Vermutlich ein einmaliger "
                                                     "Ausreißer, der Median ist davon kaum betroffen.")
                                                      .arg(runLabel(runs[i], int(i + 1)),
                                                           parts.join(QStringLiteral(", ")))};
        }
    }

    // Consistency of the throughput
    const stats::Summary regularDown = regularSummary(metricValues(runs, Metric::Download));
    const stats::Summary regularUp = regularSummary(metricValues(runs, Metric::Upload));
    const stats::Stability downStability = stats::stability(regularDown);
    const stats::Stability upStability = stats::stability(regularUp);
    const QString spread =
        tr("Schwankung Download %1, Upload %2").arg(percent(regularDown.cv()), percent(regularUp.cv()));
    const bool consistent = stabilityLevel(downStability) == Finding::Good &&
                            stabilityLevel(upStability) == Finding::Good;
    if (consistent && outlierCount == 0) {
        findings << Finding{Finding::Good, tr("Die Messungen sind konsistent (%1). Ein Zufallsergebnis ist "
                                              "unwahrscheinlich, die Werte sind belastbar.")
                                               .arg(spread)};
    } else if (consistent) {
        findings << Finding{Finding::Good, tr("Abgesehen von den Ausreißern sind die Messungen konsistent (%1).")
                                               .arg(spread)};
    } else {
        const Finding::Level level = std::max(stabilityLevel(downStability), stabilityLevel(upStability));
        findings << Finding{level, tr("Die Ergebnisse schwanken deutlich (%1). Die Leitung liefert nicht "
                                      "gleichmäßig; weitere Messungen zu verschiedenen Tageszeiten helfen, "
                                      "das Muster zu erkennen.")
                                       .arg(spread)};
    }

    // Trends over the series
    bool anyTrend = false;
    for (const Metric metric : {Metric::Download, Metric::Upload, Metric::Ping}) {
        const QList<double> values = metricValues(runs, metric);
        const stats::Trend t = stats::trend(values);
        if (!significantTrend(t, values, metric))
            continue;
        anyTrend = true;
        const bool rising = t.relativeChange > 0;
        const QString change = percent(std::abs(t.relativeChange));
        if (higherIsBetter(metric) && !rising) {
            findings << Finding{Finding::Bad, tr("%1 fällt im Verlauf der Messreihe um %2 (von ~%3 auf ~%4 %5). "
                                                 "Möglich sind zunehmende Auslastung, z. B. abends, oder eine "
                                                 "Drosselung.")
                                                  .arg(metricName(metric), change,
                                                       formatMetric(values.first(), metric),
                                                       formatMetric(values.last(), metric), metricUnit(metric))};
        } else if (!higherIsBetter(metric) && rising) {
            findings << Finding{Finding::Warning,
                                tr("%1 steigt im Verlauf der Messreihe um %2. Das Netz wird zunehmend ausgelastet.")
                                    .arg(metricName(metric), change)};
        } else {
            findings << Finding{Finding::Info, tr("%1 verändert sich im Verlauf der Messreihe um %2.")
                                                   .arg(metricName(metric), change)};
        }
    }
    if (!anyTrend && n >= 4) {
        findings << Finding{Finding::Good,
                            tr("Kein Trend über die Messreihe: Die Leitung liefert gleichbleibend.")};
    }
    return findings;
}

QList<Finding> report::loadedLatencyFindings(const QList<SpeedRun> &runs)
{
    QList<double> down, up;
    int retries = 0;
    for (const SpeedRun &run : runs) {
        if (run.result.downloadLatencyMs >= 0)
            down << run.result.downloadLatencyMs;
        if (run.result.uploadLatencyMs >= 0)
            up << run.result.uploadLatencyMs;
        retries += run.result.retries;
    }

    QList<Finding> findings;
    if (!down.isEmpty() || !up.isEmpty()) {
        const double idle = stats::median(metricValues(runs, Metric::Ping));
        const double loadedDown = down.isEmpty() ? -1.0 : stats::median(down);
        const double loadedUp = up.isEmpty() ? -1.0 : stats::median(up);
        const double worst = std::max(loadedDown, loadedUp);
        QStringList parts;
        if (loadedDown >= 0)
            parts << tr("%1 ms beim Download").arg(fmt::ms(loadedDown));
        if (loadedUp >= 0)
            parts << tr("%1 ms beim Upload").arg(fmt::ms(loadedUp));
        const QString text = tr("Ping unter Last: %1 (ohne Last %2 ms).")
                                 .arg(parts.join(QStringLiteral(", ")), fmt::ms(idle));
        if (worst - idle > kBufferbloatMs) {
            findings << Finding{Finding::Warning, text + tr(" Das ist deutlich spürbar, z. B. bei Videocalls oder "
                                                            "Spielen, während jemand lädt (Bufferbloat). Abhilfe "
                                                            "schafft oft SQM/Smart Queue im Router.")};
        } else {
            findings << Finding{Finding::Good, text + tr(" Der Router puffert kaum, die Leitung bleibt auch bei "
                                                         "voller Auslastung reaktionsschnell.")};
        }
    }
    if (retries > 0) {
        findings << Finding{Finding::Warning,
                            tr("%n Übertragung(en) brachen ab und wurden neu gestartet. Gelegentlich ist das "
                               "normal; häufen sich die Abbrüche, deutet das auf eine instabile Verbindung hin.",
                               retries)};
    }
    return findings;
}

QList<Finding> report::loadFindings(const LoadSeries &series)
{
    QList<Finding> findings;
    const QList<LoadStageResult> results = series.medianStages();
    if (results.isEmpty())
        return findings;

    const QString clients = clientsWord(series.upload);
    const bool several = series.runs.size() > 1;
    const auto best = std::max_element(results.begin(), results.end(), [](const auto &a, const auto &b) {
        return a.totalMbps < b.totalMbps;
    });

    findings << Finding{Finding::Info, (several ? tr("Höchster Gesamtdurchsatz: %1 Mbit/s bei %2 gleichzeitigen %3 "
                                                     "(Median aus %4 Durchläufen).")
                                                .arg(fmt::mbps(best->totalMbps))
                                                .arg(best->clients)
                                                .arg(clients)
                                                .arg(series.runs.size())
                                          : tr("Höchster Gesamtdurchsatz: %1 Mbit/s bei %2 gleichzeitigen %3.")
                                                .arg(fmt::mbps(best->totalMbps))
                                                .arg(best->clients)
                                                .arg(clients))};

    if (series.contractMbps > 0) {
        const double fraction = best->totalMbps / series.contractMbps;
        if (fraction >= kContractThreshold) {
            findings << Finding{Finding::Good, tr("Vertragliche Bandbreite erreicht: %1 von %2 Mbit/s.")
                                                   .arg(percent(fraction))
                                                   .arg(series.contractMbps)};
        } else {
            findings << Finding{Finding::Bad, tr("Vertragliche Bandbreite nicht erreicht: höchstens %1 von %2 Mbit/s "
                                                 "(Schwelle: 90 %).")
                                                  .arg(percent(fraction))
                                                  .arg(series.contractMbps)};
        }
    }

    if (results.size() > 1) {
        const auto saturated = std::find_if(results.begin(), results.end(), [&](const auto &r) {
            return r.totalMbps >= 0.9 * best->totalMbps;
        });
        if (saturated->clients == 1) {
            findings << Finding{Finding::Info, tr("Schon ein einzelner Client lastet die Leitung aus. Jeder weitere "
                                                  "teilt sich die Bandbreite mit den anderen.")};
        } else {
            findings << Finding{Finding::Info, tr("Die Leitung ist ab %1 gleichzeitigen %2 ausgelastet. Jeder "
                                                  "weitere teilt sich die Bandbreite mit den anderen.")
                                                   .arg(saturated->clients)
                                                   .arg(clients)};
        }

        const LoadStageResult &last = results.last();
        if (last.totalMbps < 0.8 * best->totalMbps) {
            findings << Finding{Finding::Bad, tr("Bei %1 gleichzeitigen %2 bricht der Gesamtdurchsatz auf %3 Mbit/s "
                                                 "ein (−%4 %).")
                                                  .arg(last.clients)
                                                  .arg(clients)
                                                  .arg(fmt::mbps(last.totalMbps))
                                                  .arg(qRound((1.0 - last.totalMbps / best->totalMbps) * 100))};
        }
        findings << Finding{Finding::Info, tr("Bei %1 gleichzeitigen %2 bekommt jeder im Schnitt %3 Mbit/s "
                                              "(langsamster: %4 Mbit/s).")
                                               .arg(last.clients)
                                               .arg(clients)
                                               .arg(fmt::mbps(last.clientAvgMbps))
                                               .arg(fmt::mbps(last.clientMinMbps))};
    }

    double maxLatency = -1.0;
    for (const LoadStageResult &r : results)
        maxLatency = std::max(maxLatency, r.latencyMs);
    if (maxLatency >= 0.0) {
        const double idle = series.medianIdleLatencyMs();
        const QString text = tr("Ping steigt unter Last von %1 ms auf bis zu %2 ms.").arg(fmt::ms(idle), fmt::ms(maxLatency));
        if (maxLatency - idle > kBufferbloatMs) {
            findings << Finding{Finding::Warning, text + tr(" Das ist deutlich spürbar, z. B. bei Videocalls oder "
                                                            "Spielen (Bufferbloat).")};
        } else {
            findings << Finding{Finding::Info, text};
        }
    }

    if (several) {
        // Agreement between the runs, per stage
        double worstCv = 0.0;
        QStringList outliers;
        QList<double> bestStageTotals;
        const qsizetype bestIndex = best - results.begin();
        for (qsizetype i = 0; i < results.size(); ++i) {
            QList<double> totals;
            for (const LoadRun &run : series.runs) {
                if (i < run.stages.size())
                    totals << run.stages[i].totalMbps;
            }
            const stats::Summary s = stats::summarize(totals);
            worstCv = std::max(worstCv, regularSummary(totals).cv());
            for (qsizetype r = 0; r < series.runs.size(); ++r) {
                if (i < series.runs[r].stages.size() && stats::isOutlier(series.runs[r].stages[i].totalMbps, s))
                    outliers << tr("Durchlauf %1 bei %2 Clients").arg(r + 1).arg(results[i].clients);
            }
            if (i == bestIndex)
                bestStageTotals = totals;
        }
        const stats::Stability stability = stats::stability(worstCv);
        if (stabilityLevel(stability) == Finding::Good) {
            findings << Finding{Finding::Good, tr("Die %1 Durchläufe stimmen gut überein (Schwankung je Stufe "
                                                  "höchstens %2%3). %4")
                                                   .arg(series.runs.size())
                                                   .arg(percent(worstCv))
                                                   .arg(outliers.isEmpty() ? QString() : tr(" ohne Ausreißer"))
                                                   .arg(series.runs.size() < 3
                                                            ? tr("Ab 3 Durchläufen lassen sich auch einzelne "
                                                                 "Ausreißer erkennen.")
                                                            : tr("Ein Zufallsergebnis ist unwahrscheinlich."))};
        } else {
            findings << Finding{stabilityLevel(stability), tr("Die Durchläufe weichen voneinander ab (Schwankung je "
                                                              "Stufe bis %1%2). Die Leitung liefert nicht gleichmäßig.")
                                                               .arg(percent(worstCv))
                                                               .arg(outliers.isEmpty() ? QString()
                                                                                       : tr(" ohne Ausreißer"))};
        }
        if (!outliers.isEmpty()) {
            findings << Finding{Finding::Warning, tr("Ausreißer: %1. Diese Werte sind vermutlich einmalig und "
                                                     "fließen über den Median kaum ein.")
                                                      .arg(outliers.join(QStringLiteral(", ")))};
        }
        const stats::Trend t = stats::trend(bestStageTotals);
        if (t.significant()) {
            findings << Finding{t.relativeChange < 0 ? Finding::Bad : Finding::Info,
                                tr("Der Gesamtdurchsatz bei %1 Clients %2 über die Durchläufe um %3.")
                                    .arg(best->clients)
                                    .arg(t.relativeChange < 0 ? tr("fällt") : tr("steigt"))
                                    .arg(percent(std::abs(t.relativeChange)))};
        }
    }

    qint64 totalBytes = 0;
    for (const LoadRun &run : series.runs) {
        for (const LoadStageResult &r : run.stages)
            totalBytes += r.bytes;
    }
    // Mbit/s -> GB/h: x * 3600 s / 8 bit / 1000 MB
    findings << Finding{Finding::Info, tr("Das entspricht bis zu %1 GB pro Stunde; in diesem Test übertragen: %2.")
                                           .arg(QLocale().toString(best->totalMbps * 3600.0 / 8.0 / 1000.0, 'f', 0),
                                                fmt::bytes(totalBytes))};
    return findings;
}

// --- Workbook -------------------------------------------------------------------

QByteArray report::xlsx(const QList<SpeedRun> &speedRuns, const LoadSeries &load)
{
    XlsxWriter writer;
    writer.setCreator(QStringLiteral("broadband " BROADBAND_VERSION));
    writer.setTitle(tr("Broadband – Messbericht"));

    writeOverview(writer.addSheet(tr("Übersicht")), speedRuns, load);
    if (!speedRuns.isEmpty())
        writeSpeedSheet(writer.addSheet(tr("Speedtest")), speedRuns);
    if (!load.isEmpty())
        writeLoadSheet(writer.addSheet(tr("Bandbreite unter Last")), load);
    return writer.toByteArray();
}

bool report::saveXlsx(const QString &path, const QList<SpeedRun> &speedRuns, const LoadSeries &load,
                      QString *errorString)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(xlsx(speedRuns, load)) < 0 || !file.commit()) {
        if (errorString)
            *errorString = file.errorString();
        return false;
    }
    return true;
}
