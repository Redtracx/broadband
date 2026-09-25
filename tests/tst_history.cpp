#include "ContractCheck.h"
#include "History.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TestHistory : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip();
    void skipsDamagedLines();
    void removeSpeedRunsKeepsLoadTests();
    void csv();

    void contractComplete();
    void contractDeviations();
    void contractProtocolIssues();
    void contractIncomplete();

private:
    QTemporaryDir m_dir;
};

namespace {

SpeedRun speedRun(const QDateTime &time, double down, double up, int series = 1)
{
    SpeedRun run;
    run.time = time;
    run.server = QStringLiteral("https://speed.cloudflare.com");
    run.series = series;
    run.streams = 4;
    run.durationSec = 10;
    run.ipVersion = 6;
    run.result.pingMs = 12.5;
    run.result.jitterMs = 1.25;
    run.result.downloadMbps = down;
    run.result.uploadMbps = up;
    run.result.downloadLatencyMs = 30.0;
    run.result.uploadLatencyMs = -1.0;
    run.result.downloadedBytes = 123'456'789;
    run.result.uploadedBytes = 9'876'543;
    run.result.retries = 2;
    run.result.server.colo = QStringLiteral("FRA");
    run.result.server.clientIp = QStringLiteral("2001:db8::1");
    return run;
}

LoadSeries loadSeries(int id)
{
    LoadSeries series;
    series.id = id;
    series.server = QStringLiteral("http://lan:8080");
    series.upload = true;
    series.contractMbps = 50;
    series.stageDurationSec = 5;
    return series;
}

LoadRun loadRun(const QDateTime &time)
{
    LoadRun run;
    run.time = time;
    run.idleLatencyMs = 8.0;
    LoadStageResult stage;
    stage.clients = 4;
    stage.totalMbps = 48.0;
    stage.clientAvgMbps = 12.0;
    stage.clientMinMbps = 11.0;
    stage.clientMaxMbps = 13.0;
    stage.latencyMs = 90.0;
    stage.bytes = 30'000'000;
    stage.seconds = 5.0;
    stage.retries = 1;
    run.stages = {stage, stage};
    return run;
}

// 10 runs per day at 9:00, 9:10, … and 18:00, 18:10, …
QList<SpeedRun> measurementDays(const QList<QDate> &dates, const std::function<double(int, int)> &download)
{
    QList<SpeedRun> runs;
    for (int d = 0; d < dates.size(); ++d) {
        for (int i = 0; i < contract::kPerDay; ++i) {
            const QTime time = i < 5 ? QTime(9, i * 10) : QTime(18, (i - 5) * 10);
            runs << speedRun(QDateTime(dates[d], time), download(d, i), 40.0);
        }
    }
    return runs;
}

QString describe(const QList<Finding> &findings)
{
    QStringList lines;
    for (const Finding &f : findings)
        lines << QStringLiteral("[%1] %2").arg(f.level).arg(f.text);
    return lines.join(QLatin1Char('\n'));
}

bool hasFinding(const QList<Finding> &findings, Finding::Level level, const QString &text)
{
    return std::any_of(findings.begin(), findings.end(),
                       [&](const Finding &f) { return f.level == level && f.text.contains(text); });
}

const QList<QDate> kDates{QDate(2026, 9, 1), QDate(2026, 9, 3), QDate(2026, 9, 5)};
const contract::Contract kContract{{250, 200, 100}, {40, 30, 10}};

} // namespace

void TestHistory::roundTrip()
{
    const QString path = m_dir.filePath(QStringLiteral("roundtrip/history.jsonl"));
    const QDateTime time(QDate(2026, 9, 24), QTime(20, 15, 30, 250));
    QVERIFY(history::append(path, speedRun(time, 245.5, 41.25, 3)));
    QVERIFY(history::append(path, loadSeries(2), loadRun(time.addSecs(60))));
    QVERIFY(history::append(path, loadSeries(2), loadRun(time.addSecs(120))));
    QVERIFY(history::append(path, speedRun(time.addSecs(180), 250.0, 40.0, 4)));

    QString error;
    const history::Data data = history::load(path, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(data.speedRuns.size(), 2);
    QCOMPARE(data.lastSpeedSeries(), 4);

    const SpeedRun &run = data.speedRuns.first();
    QCOMPARE(run.time, time);
    QCOMPARE(run.server, QStringLiteral("https://speed.cloudflare.com"));
    QCOMPARE(run.series, 3);
    QCOMPARE(run.ipVersion, 6);
    QCOMPARE(run.result.downloadMbps, 245.5);
    QCOMPARE(run.result.uploadMbps, 41.25);
    QCOMPARE(run.result.downloadLatencyMs, 30.0);
    QCOMPARE(run.result.uploadLatencyMs, -1.0);
    QCOMPARE(run.result.downloadedBytes, qint64(123'456'789));
    QCOMPARE(run.result.retries, 2);
    QCOMPARE(run.result.server.colo, QStringLiteral("FRA"));
    QVERIFY(run.result.server.clientIp.isEmpty()); // the public address is not stored

    QCOMPARE(data.loadSeries.size(), 1);
    const LoadSeries &series = data.loadSeries.first();
    QCOMPARE(series.id, 2);
    QCOMPARE(data.lastLoadSeries(), 2);
    QVERIFY(series.upload);
    QCOMPARE(series.contractMbps, 50);
    QCOMPARE(series.runs.size(), 2);
    QCOMPARE(series.runs[1].time, time.addSecs(120));
    QCOMPARE(series.runs[0].stages.size(), 2);
    QCOMPARE(series.runs[0].stages[0].totalMbps, 48.0);
    QCOMPARE(series.runs[0].stages[0].latencyMs, 90.0);
    QCOMPARE(series.runs[0].stages[0].retries, 1);
}

void TestHistory::skipsDamagedLines()
{
    const QString path = m_dir.filePath(QStringLiteral("damaged.jsonl"));
    QVERIFY(history::append(path, speedRun(QDateTime::currentDateTime(), 100, 10)));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::Append));
        file.write("{\"type\":\"speed\",\"time\":\"kaputt\"}\n{not json\n\n{\"type\":\"future\"}\n");
    }
    QVERIFY(history::append(path, speedRun(QDateTime::currentDateTime(), 200, 20)));

    const history::Data data = history::load(path);
    QCOMPARE(data.speedRuns.size(), 2);
    QCOMPARE(data.speedRuns[1].result.downloadMbps, 200.0);

    QVERIFY(history::load(m_dir.filePath(QStringLiteral("missing.jsonl"))).speedRuns.isEmpty());
}

void TestHistory::removeSpeedRunsKeepsLoadTests()
{
    const QString path = m_dir.filePath(QStringLiteral("remove.jsonl"));
    const QDateTime time = QDateTime::currentDateTime();
    QVERIFY(history::append(path, speedRun(time, 100, 10)));
    QVERIFY(history::append(path, loadSeries(1), loadRun(time)));
    QVERIFY(history::append(path, speedRun(time, 100, 10)));

    QVERIFY(history::removeSpeedRuns(path));
    const history::Data data = history::load(path);
    QVERIFY(data.speedRuns.isEmpty());
    QCOMPARE(data.loadSeries.size(), 1);
}

void TestHistory::csv()
{
    SpeedRun run = speedRun(QDateTime(QDate(2026, 9, 24), QTime(8, 0)), 245.5, 41.25);
    run.server = QStringLiteral("http://host/a,b"); // needs quoting
    const QString line = history::toCsv(run);
    QVERIFY(line.contains(QStringLiteral(",\"http://host/a,b\",FRA,6,4,10,12.50,1.25,245.50,41.25,30.00,-1.00,")));
    // One field per header column; the quoted comma doesn't count.
    QCOMPARE(QString(line).remove(QStringLiteral("a,b")).count(QLatin1Char(',')),
             history::speedCsvHeader().count(QLatin1Char(',')));

    const QStringList lines = history::toCsv(loadSeries(1), loadRun(run.time));
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.first().count(QLatin1Char(',')), history::loadCsvHeader().count(QLatin1Char(',')));
    QVERIFY(lines.first().contains(QStringLiteral(",upload,50,4,48.00,")));
}

void TestHistory::contractComplete()
{
    const QList<SpeedRun> runs = measurementDays(kDates, [](int, int i) { return i == 3 ? 230.0 : 210.0; });
    const contract::Result result = contract::evaluate(runs, kContract);
    QVERIFY(result.complete);
    QVERIFY2(!result.deviation, qPrintable(describe(result.findings)));
    QCOMPARE(result.days.size(), 3);
    for (const contract::Day &day : result.days)
        QVERIFY2(day.protocolIssues.isEmpty(), qPrintable(day.protocolIssues.join(QLatin1Char(';'))));
    QVERIFY(hasFinding(result.findings, Finding::Good, QStringLiteral("an 3 von 3 Messtagen erreicht")));
    QVERIFY(hasFinding(result.findings, Finding::Good, QStringLiteral("Keine erhebliche Abweichung")));
}

void TestHistory::contractDeviations()
{
    // 90 % of the maximum (225) only reached on the first day
    {
        const auto runs = measurementDays(kDates, [](int d, int i) { return d == 0 && i == 0 ? 240.0 : 210.0; });
        const contract::Result result = contract::evaluate(runs, kContract);
        QVERIFY(result.deviation);
        QVERIFY2(hasFinding(result.findings, Finding::Bad, QStringLiteral("an 1 von 3 Messtagen erreicht")),
                 qPrintable(describe(result.findings)));
    }
    // Normal speed (200) reached in only 27 of 30 measurements: 90 % needed, 27/30 = 90 % is fine ...
    {
        const auto runs = measurementDays(kDates, [](int, int i) { return i == 0 ? 190.0 : 230.0; });
        const contract::Result result = contract::evaluate(runs, kContract);
        QVERIFY2(!result.deviation, qPrintable(describe(result.findings)));
    }
    // ... 26 of 30 is not
    {
        const auto runs = measurementDays(kDates, [](int d, int i) { return i == 0 || (d == 0 && i == 1) ? 190.0 : 230.0; });
        const contract::Result result = contract::evaluate(runs, kContract);
        QVERIFY(result.deviation);
        QVERIFY(hasFinding(result.findings, Finding::Bad, QStringLiteral("in 26 von 30 Messungen")));
    }
    // Minimum (100) undercut on two days
    {
        const auto runs = measurementDays(kDates, [](int d, int i) { return d > 0 && i == 7 ? 80.0 : 230.0; });
        const contract::Result result = contract::evaluate(runs, kContract);
        QVERIFY(result.deviation);
        QVERIFY(hasFinding(result.findings, Finding::Bad, QStringLiteral("an 2 Messtagen unterschritten")));
    }
}

void TestHistory::contractProtocolIssues()
{
    // Sept 2 directly precedes Sept 3 and is skipped; runs too close together.
    QList<SpeedRun> runs = measurementDays({QDate(2026, 9, 1), QDate(2026, 9, 3), QDate(2026, 9, 5)},
                                           [](int, int) { return 230.0; });
    runs << speedRun(QDateTime(QDate(2026, 9, 2), QTime(12, 0)), 50.0, 5.0);
    runs[1].time = runs[0].time.addSecs(120);  // 2 min after the first
    runs[15].time = runs[14].time.addSecs(3600); // only 1 h between 5th and 6th

    const contract::Result result = contract::evaluate(runs, kContract);
    QCOMPARE(result.days.size(), 3);
    QCOMPARE(result.days.first().date, QDate(2026, 9, 1));
    QVERIFY(result.protocolIssues.join(QString()).contains(QStringLiteral("Nicht gewertet")));
    QVERIFY(result.days[0].protocolIssues.join(QString()).contains(QStringLiteral("weniger als 5 Minuten")));
    QVERIFY(result.days[1].protocolIssues.join(QString()).contains(QStringLiteral("weniger als 3 Stunden")));
    QVERIFY(!result.deviation); // the skipped slow day doesn't count
}

void TestHistory::contractIncomplete()
{
    const QList<SpeedRun> runs = measurementDays({QDate(2026, 9, 1)}, [](int, int) { return 230.0; }).mid(0, 4);
    const contract::Result result = contract::evaluate(runs, kContract);
    QVERIFY(!result.complete);
    QCOMPARE(result.days.size(), 1);
    QVERIFY(hasFinding(result.findings, Finding::Info, QStringLiteral("4 von 30 Messungen an 1 von 3 Messtagen")));
    QVERIFY(hasFinding(result.findings, Finding::Good, QStringLiteral("Vorläufig")));

    // Without contract values only the progress is reported.
    const contract::Result empty = contract::evaluate(runs, {});
    QVERIFY(hasFinding(empty.findings, Finding::Info, QStringLiteral("Vertragswerte eintragen")));
}

QTEST_GUILESS_MAIN(TestHistory)
#include "tst_history.moc"
