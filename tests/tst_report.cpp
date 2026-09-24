#include "Report.h"
#include "Statistics.h"
#include "XlsxWriter.h"

#include <QTest>
#include <QXmlStreamReader>

#include <array>

class TestReport : public QObject
{
    Q_OBJECT

private slots:
    void median();
    void summary();
    void outliers_data();
    void outliers();
    void trend();

    void speedFindingsConsistent();
    void speedFindingsFluke();
    void speedFindingsFallingTrend();
    void loadMedianAndFindings();

    void cellReferences();
    void workbookParts();
    void zipContainer();
};

namespace {

QList<SpeedRun> speedRuns(const QList<double> &downloads, const QString &server = QStringLiteral("http://lan:8080"))
{
    QList<SpeedRun> runs;
    const QDateTime start(QDate(2026, 9, 23), QTime(18, 0));
    for (qsizetype i = 0; i < downloads.size(); ++i) {
        SpeedRun run;
        run.time = start.addSecs(i * 60);
        run.server = server;
        run.index = int(i + 1);
        run.streams = 4;
        run.durationSec = 10;
        run.result.downloadMbps = downloads[i];
        run.result.uploadMbps = 40.0 + double(i % 2);
        run.result.pingMs = 12.0;
        run.result.jitterMs = 1.0;
        run.result.downloadedBytes = 100'000'000;
        run.result.uploadedBytes = 20'000'000;
        runs.append(run);
    }
    return runs;
}

bool hasFinding(const QList<Finding> &findings, Finding::Level level, const QString &text)
{
    return std::any_of(findings.begin(), findings.end(),
                       [&](const Finding &f) { return f.level == level && f.text.contains(text); });
}

QString describe(const QList<Finding> &findings)
{
    QStringList lines;
    for (const Finding &f : findings)
        lines << QStringLiteral("[%1] %2").arg(f.level).arg(f.text);
    return lines.join(QLatin1Char('\n'));
}

LoadSeries loadSeries()
{
    LoadSeries series;
    series.server = QStringLiteral("http://lan:8080");
    series.contractMbps = 250;
    series.stageDurationSec = 10;
    const QList<QList<double>> totals{{100, 200, 240}, {104, 120, 236}, {98, 204, 242}};
    for (qsizetype r = 0; r < totals.size(); ++r) {
        LoadRun run;
        run.time = QDateTime(QDate(2026, 9, 23), QTime(20, int(r)));
        run.idleLatencyMs = 10.0 + double(r);
        const QList<int> clients{1, 2, 4};
        for (qsizetype i = 0; i < clients.size(); ++i) {
            LoadStageResult stage;
            stage.clients = clients[i];
            stage.totalMbps = totals[r][i];
            stage.clientAvgMbps = stage.totalMbps / stage.clients;
            stage.clientMinMbps = stage.clientAvgMbps * 0.9;
            stage.clientMaxMbps = stage.clientAvgMbps * 1.1;
            stage.latencyMs = 20.0 * stage.clients;
            stage.bytes = qint64(stage.totalMbps * 1e6 / 8 * 10);
            stage.seconds = 10;
            run.stages.append(stage);
        }
        series.runs.append(run);
    }
    return series;
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char byte : data) {
        crc ^= quint8(byte);
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

quint32 read32(const QByteArray &data, qsizetype offset)
{
    return quint32(quint8(data[offset])) | quint32(quint8(data[offset + 1])) << 8 |
           quint32(quint8(data[offset + 2])) << 16 | quint32(quint8(data[offset + 3])) << 24;
}

quint16 read16(const QByteArray &data, qsizetype offset)
{
    return quint16(quint8(data[offset]) | quint8(data[offset + 1]) << 8);
}

} // namespace

void TestReport::median()
{
    QCOMPARE(stats::median({}), 0.0);
    QCOMPARE(stats::median({3.0}), 3.0);
    QCOMPARE(stats::median({5.0, 1.0, 3.0}), 3.0);
    QCOMPARE(stats::median({4.0, 1.0, 3.0, 2.0}), 2.5);
}

void TestReport::summary()
{
    const stats::Summary s = stats::summarize({2, 4, 4, 4, 5, 5, 7, 9});
    QCOMPARE(s.count, 8);
    QCOMPARE(s.mean, 5.0);
    QCOMPARE(s.median, 4.5);
    QCOMPARE(s.min, 2.0);
    QCOMPARE(s.max, 9.0);
    QVERIFY(qAbs(s.stddev - 2.13809) < 1e-4); // sample standard deviation
    QVERIFY(qAbs(s.cv() - s.stddev / 5.0) < 1e-12);

    const stats::Summary single = stats::summarize({42});
    QCOMPARE(single.stddev, 0.0);
    QCOMPARE(stats::stability(single), stats::Stability::Unknown);
}

void TestReport::outliers_data()
{
    QTest::addColumn<QList<double>>("values");
    QTest::addColumn<double>("value");
    QTest::addColumn<bool>("outlier");

    const QList<double> steady{240, 238, 245, 241, 236, 243};
    QTest::newRow("fluke") << (steady + QList<double>{96}) << 96.0 << true;
    QTest::newRow("regular") << (steady + QList<double>{96}) << 238.0 << false;
    // All identical except a small wobble: MAD is 0, but 2 % is no outlier.
    QTest::newRow("tiny wobble") << QList<double>{100, 100, 100, 100, 102} << 102.0 << false;
    QTest::newRow("too few runs") << QList<double>{240, 96} << 96.0 << false;
}

void TestReport::outliers()
{
    QFETCH(QList<double>, values);
    QFETCH(double, value);
    QFETCH(bool, outlier);
    QCOMPARE(stats::isOutlier(value, stats::summarize(values)), outlier);
}

void TestReport::trend()
{
    const stats::Trend falling = stats::trend({250, 230, 210, 190, 170});
    QVERIFY(falling.significant());
    QVERIFY(falling.relativeChange < -0.3);
    QVERIFY(qAbs(falling.r2 - 1.0) < 1e-9);

    QVERIFY(!stats::trend({240, 238, 245, 241, 236, 243}).significant());
    QVERIFY(!stats::trend({250, 200, 170}).significant()); // needs 4 runs
}

void TestReport::speedFindingsConsistent()
{
    const QList<Finding> findings = report::speedFindings(speedRuns({240, 238, 245, 241, 236}));
    QVERIFY2(hasFinding(findings, Finding::Good, QStringLiteral("konsistent")), qPrintable(describe(findings)));
    QVERIFY2(hasFinding(findings, Finding::Good, QStringLiteral("Kein Trend")), qPrintable(describe(findings)));
    for (const Finding &f : findings)
        QVERIFY2(f.level != Finding::Warning && f.level != Finding::Bad, qPrintable(describe(findings)));
}

void TestReport::speedFindingsFluke()
{
    const QList<Finding> findings = report::speedFindings(speedRuns({240, 238, 245, 96, 241, 236}));
    // The fluke is reported, but the remaining runs still count as consistent.
    QVERIFY2(hasFinding(findings, Finding::Warning, QStringLiteral("Messung 4")), qPrintable(describe(findings)));
    QVERIFY2(hasFinding(findings, Finding::Good, QStringLiteral("Abgesehen von den Ausreißern")),
             qPrintable(describe(findings)));
}

void TestReport::speedFindingsFallingTrend()
{
    const QList<Finding> findings = report::speedFindings(speedRuns({250, 235, 215, 190, 170, 150}));
    QVERIFY2(hasFinding(findings, Finding::Bad, QStringLiteral("Download fällt")), qPrintable(describe(findings)));

    const QList<Finding> single = report::speedFindings(speedRuns({250}));
    QVERIFY2(hasFinding(single, Finding::Info, QStringLiteral("mindestens 3")), qPrintable(describe(single)));
}

void TestReport::loadMedianAndFindings()
{
    const LoadSeries series = loadSeries();
    const QList<LoadStageResult> medians = series.medianStages();
    QCOMPARE(medians.size(), 3);
    QCOMPARE(medians[0].clients, 1);
    QCOMPARE(medians[0].totalMbps, 100.0);
    QCOMPARE(medians[1].totalMbps, 200.0); // the fluke of 120 does not count
    QCOMPARE(medians[2].totalMbps, 240.0);
    QCOMPARE(series.medianIdleLatencyMs(), 11.0);

    const QList<Finding> findings = report::loadFindings(series);
    QVERIFY2(hasFinding(findings, Finding::Good, QStringLiteral("Vertragliche Bandbreite erreicht")),
             qPrintable(describe(findings)));
    QVERIFY2(hasFinding(findings, Finding::Warning, QStringLiteral("Durchlauf 2 bei 2 Clients")),
             qPrintable(describe(findings)));
    QVERIFY2(hasFinding(findings, Finding::Good, QStringLiteral("stimmen gut überein")), qPrintable(describe(findings)));
}

void TestReport::cellReferences()
{
    QCOMPARE(XlsxWriter::columnName(0), QStringLiteral("A"));
    QCOMPARE(XlsxWriter::columnName(25), QStringLiteral("Z"));
    QCOMPARE(XlsxWriter::columnName(26), QStringLiteral("AA"));
    QCOMPARE(XlsxWriter::columnName(701), QStringLiteral("ZZ"));
    QCOMPARE(XlsxWriter::columnName(702), QStringLiteral("AAA"));
    QCOMPARE(XlsxWriter::cellRef(0, 0), QStringLiteral("A1"));
    QCOMPARE(XlsxWriter::cellRef(9, 27, true), QStringLiteral("$AB$10"));
}

void TestReport::workbookParts()
{
    XlsxWriter writer;
    writer.setCreator(QStringLiteral("test"));
    XlsxWriter::Sheet &sheet = writer.addSheet(QStringLiteral("Daten [1/2]: <&>"));
    QCOMPARE(sheet.name(), QStringLiteral("Daten _1_2__ <&>"));
    XlsxWriter::Style bold;
    bold.bold = true;
    bold.fill = 0x2f80ed;
    sheet.setText(0, 0, QStringLiteral("Wert <\"&\"> \x01"), bold);
    sheet.setNumber(1, 0, 1.5);
    sheet.setNumber(2, 0, 2.5);
    sheet.setFormula(3, 0, QStringLiteral("SUM(A2:A3)"), 4.0);
    sheet.setAutoFilter(0, 0, 2, 0);
    sheet.freezeRows(1);
    XlsxWriter::LineChart chart;
    chart.categoryFirstRow = 1;
    chart.categoryLastRow = 2;
    chart.series = {{QStringLiteral("A & B"), 0, 1, 2, 0x9b51e0}};
    sheet.addLineChart(chart);

    QStringList names;
    for (const auto &[name, data] : writer.files()) {
        names << name;
        QXmlStreamReader xml(data);
        while (!xml.atEnd())
            xml.readNext();
        QVERIFY2(!xml.hasError(), qPrintable(name + QStringLiteral(": ") + xml.errorString()));
    }
    for (const char *expected : {"[Content_Types].xml", "_rels/.rels", "xl/workbook.xml", "xl/_rels/workbook.xml.rels",
                                 "xl/styles.xml", "xl/worksheets/sheet1.xml", "xl/worksheets/_rels/sheet1.xml.rels",
                                 "xl/drawings/drawing1.xml", "xl/charts/chart1.xml"}) {
        QVERIFY2(names.contains(QLatin1String(expected)), expected);
    }

    // The report itself: overview plus one sheet per test type
    const QByteArray xlsx = report::xlsx(speedRuns({240, 238, 96, 241}), loadSeries());
    QVERIFY(xlsx.startsWith("PK\x03\x04"));
    QVERIFY(xlsx.contains("xl/worksheets/sheet3.xml"));
    QVERIFY(!xlsx.contains("xl/worksheets/sheet4.xml"));
}

void TestReport::zipContainer()
{
    XlsxWriter writer;
    writer.addSheet(QStringLiteral("A")).setText(0, 0, QStringLiteral("x"));
    const QList<QPair<QString, QByteArray>> files = writer.files();
    const QByteArray zip = writer.toByteArray();

    // End of central directory: entry count must match
    const qsizetype eocd = zip.size() - 22;
    QCOMPARE(read32(zip, eocd), quint32(0x06054b50));
    QCOMPARE(qsizetype(read16(zip, eocd + 10)), files.size());

    // Walk the local file headers: names, sizes and checksums
    qsizetype offset = 0;
    QStringList names;
    for (qsizetype i = 0; i < files.size(); ++i) {
        QCOMPARE(read32(zip, offset), quint32(0x04034b50));
        QCOMPARE(read16(zip, offset + 8), quint16(8)); // deflate
        const quint32 crc = read32(zip, offset + 14);
        const quint32 compressed = read32(zip, offset + 18);
        const quint32 size = read32(zip, offset + 22);
        const quint16 nameLength = read16(zip, offset + 26);
        const QString name = QString::fromUtf8(zip.mid(offset + 30, nameLength));
        names << name;

        const auto file = std::find_if(files.begin(), files.end(), [&](const auto &f) { return f.first == name; });
        QVERIFY2(file != files.end(), qPrintable(name));
        QCOMPARE(qsizetype(size), file->second.size());
        QCOMPARE(crc, crc32(file->second));
        offset += 30 + nameLength + compressed;
    }
    QCOMPARE(read32(zip, offset), quint32(0x02014b50)); // central directory follows
    QCOMPARE(names.first(), QStringLiteral("[Content_Types].xml"));
}

QTEST_GUILESS_MAIN(TestReport)
#include "tst_report.moc"
