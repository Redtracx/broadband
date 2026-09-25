#include "Cli.h"

#include "Format.h"
#include "History.h"
#include "LoadTest.h"
#include "SpeedTest.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>

namespace {

QString tr(const char *text, int n = -1)
{
    return QCoreApplication::translate("Cli", text, nullptr, n);
}

// Also works if the test ends before the event loop runs, e.g. on bad input.
void quitLater()
{
    QTimer::singleShot(0, qApp, [] { QCoreApplication::quit(); });
}

QString findingPrefix(Finding::Level level)
{
    switch (level) {
    case Finding::Good:
        return QStringLiteral("✔ ");
    case Finding::Warning:
        return QStringLiteral("⚠ ");
    case Finding::Bad:
        return QStringLiteral("✘ ");
    case Finding::Info:
        break;
    }
    return QStringLiteral("• ");
}

void printFindings(QTextStream &out, const QList<Finding> &findings)
{
    for (const Finding &finding : findings)
        out << findingPrefix(finding.level) << finding.text << '\n';
}

QString loadedLatencyText(double ms)
{
    return ms < 0 ? QString() : tr(" (Ping unter Last %1 ms)").arg(fmt::ms(ms));
}

void saveToHistory(const std::function<bool(const QString &, QString *)> &append)
{
    QString error;
    if (!append(history::defaultPath(), &error)) {
        QTextStream(stderr) << tr("Verlauf konnte nicht gespeichert werden: %1\n").arg(error);
    }
}

int runSpeedTest(QCoreApplication &app, const CliOptions &options)
{
    QTextStream out(stdout);
    QTextStream err(stderr);
    const bool text = options.format == CliOptions::Format::Text;

    SpeedTestConfig config;
    config.baseUrl = options.url;
    config.durationSec = options.durationSec;
    config.streams = options.streams;
    config.ipVersion = options.ipVersion;

    SpeedTest test;
    int exitCode = 0;
    if (text) {
        out << tr("Server:    %1\n").arg(options.url.toString());
        out.flush();
        QObject::connect(&test, &SpeedTest::latencyMeasured, &test, [&](double ping, double jitter) {
            const http::ServerInfo &info = test.result().server;
            QStringList details;
            if (!info.colo.isEmpty())
                details << tr("Rechenzentrum %1").arg(info.colo);
            if (info.ipVersion())
                details << QStringLiteral("IPv%1").arg(info.ipVersion());
            if (!details.isEmpty())
                out << tr("Verbindung: %1\n").arg(details.join(QStringLiteral(", ")));
            out << tr("Ping:      %1 ms (Jitter %2 ms)\n").arg(fmt::ms(ping), fmt::ms(jitter));
            out.flush();
        });
        QObject::connect(&test, &SpeedTest::phaseFinished, &test, [&](SpeedTest::Phase phase, double mbps) {
            const bool download = phase == SpeedTest::Phase::Download;
            const SpeedTestResult &r = test.result();
            out << (download ? tr("Download:  %1 Mbit/s%2\n") : tr("Upload:    %1 Mbit/s%2\n"))
                       .arg(fmt::mbps(mbps),
                            loadedLatencyText(download ? r.downloadLatencyMs : r.uploadLatencyMs));
            out.flush();
        });
    }

    QObject::connect(&test, &SpeedTest::finished, &test, [&](const SpeedTestResult &result) {
        const history::Data data = options.save ? history::load(history::defaultPath()) : history::Data();
        SpeedRun run;
        run.time = QDateTime::currentDateTime();
        run.server = options.url.toString();
        run.series = data.lastSpeedSeries() + 1;
        run.index = 1;
        run.streams = config.streams;
        run.durationSec = config.durationSec;
        run.ipVersion = result.server.ipVersion();
        run.result = result;

        switch (options.format) {
        case CliOptions::Format::Text:
            printFindings(out, report::loadedLatencyFindings({run}));
            break;
        case CliOptions::Format::Json:
            out << QJsonDocument(history::toJson(run)).toJson(QJsonDocument::Compact) << '\n';
            break;
        case CliOptions::Format::Csv:
            if (options.header)
                out << history::speedCsvHeader() << '\n';
            out << history::toCsv(run) << '\n';
            break;
        }
        out.flush();
        if (options.save)
            saveToHistory([&](const QString &path, QString *error) { return history::append(path, run, error); });
        quitLater();
    });
    QObject::connect(&test, &SpeedTest::failed, &test, [&](const QString &message) {
        err << tr("Fehler: %1\n").arg(message);
        exitCode = 1;
        quitLater();
    });

    test.start(config);
    app.exec();
    return exitCode;
}

int runLoadTest(QCoreApplication &app, const CliOptions &options)
{
    QTextStream out(stdout);
    QTextStream err(stderr);
    const bool text = options.format == CliOptions::Format::Text;

    LoadTestConfig config;
    config.baseUrl = options.url;
    config.stages = options.loadStages;
    config.stageDurationSec = options.durationSec;
    config.upload = options.upload;
    config.ipVersion = options.ipVersion;

    const history::Data data = options.save ? history::load(history::defaultPath()) : history::Data();
    LoadSeries series;
    series.id = data.lastLoadSeries() + 1;
    series.server = options.url.toString();
    series.upload = options.upload;
    series.contractMbps = options.contractMbps;
    series.stageDurationSec = config.stageDurationSec;
    const QDateTime start = QDateTime::currentDateTime();

    LoadTest test;
    int exitCode = 0;
    if (text) {
        out << tr("Server: %1 · %2\n")
                   .arg(options.url.toString(), options.upload ? tr("Upload") : tr("Download"));
        out.flush();
        QObject::connect(&test, &LoadTest::idleLatencyMeasured, &test, [&](double ms) {
            out << tr("Ping ohne Last: %1 ms\n").arg(fmt::ms(ms));
            out.flush();
        });
        QObject::connect(&test, &LoadTest::stageFinished, &test, [&](int, const LoadStageResult &r) {
            QString line = tr("%1 Clients: %2 Mbit/s gesamt, je Client %3 – %4 Mbit/s")
                               .arg(r.clients, 3)
                               .arg(fmt::mbps(r.totalMbps), fmt::mbps(r.clientMinMbps), fmt::mbps(r.clientMaxMbps));
            if (r.latencyMs >= 0)
                line += tr(", Ping %1 ms").arg(fmt::ms(r.latencyMs));
            if (series.contractMbps > 0)
                line += tr(", %1 % vom Vertrag").arg(qRound(100.0 * r.totalMbps / series.contractMbps));
            out << line << '\n';
            out.flush();
        });
    }

    QObject::connect(&test, &LoadTest::finished, &test, [&] {
        LoadRun run;
        run.time = start;
        run.idleLatencyMs = test.idleLatencyMs();
        run.stages = test.results();
        series.runs = {run};

        switch (options.format) {
        case CliOptions::Format::Text:
            printFindings(out, report::loadFindings(series));
            break;
        case CliOptions::Format::Json:
            out << QJsonDocument(history::toJson(series, run)).toJson(QJsonDocument::Compact) << '\n';
            break;
        case CliOptions::Format::Csv:
            if (options.header)
                out << history::loadCsvHeader() << '\n';
            for (const QString &line : history::toCsv(series, run))
                out << line << '\n';
            break;
        }
        out.flush();
        if (options.save) {
            saveToHistory(
                [&](const QString &path, QString *error) { return history::append(path, series, run, error); });
        }
        quitLater();
    });
    QObject::connect(&test, &LoadTest::failed, &test, [&](const QString &message) {
        err << tr("Fehler: %1\n").arg(message);
        exitCode = 1;
        quitLater();
    });

    test.start(config);
    app.exec();
    return exitCode;
}

} // namespace

int runCli(QCoreApplication &app, const CliOptions &options)
{
    return options.loadStages.isEmpty() ? runSpeedTest(app, options) : runLoadTest(app, options);
}
