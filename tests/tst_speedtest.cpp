#include "LoadTest.h"
#include "SpeedTest.h"
#include "TestServer.h"

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTest>

class TestSpeedTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void serverDownload_data();
    void serverDownload();
    void serverUpload();
    void serverRejectsBadRequests_data();
    void serverRejectsBadRequests();
    void serverLibreSpeed();

    void fullRun_data();
    void fullRun();
    void stopWhileRunning();
    void connectionRefused();

    void loadTestStages_data();
    void loadTestStages();
    void loadTestStop();
    void loadTestMultipleServers_data();
    void loadTestMultipleServers();

private:
    QNetworkReply *get(const QString &path);
    QUrl baseUrl() const;

    TestServer *m_server = nullptr;
    QNetworkAccessManager *m_nam = nullptr;
};

void TestSpeedTest::init()
{
    m_server = new TestServer(this);
    QVERIFY2(m_server->start(0, QHostAddress::LocalHost), qPrintable(m_server->errorString()));
    m_nam = new QNetworkAccessManager(this);
    m_nam->setProxy(QNetworkProxy::NoProxy);
}

void TestSpeedTest::cleanup()
{
    delete m_nam;
    delete m_server;
}

QUrl TestSpeedTest::baseUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server->port()));
}

QNetworkReply *TestSpeedTest::get(const QString &path)
{
    QNetworkReply *reply = m_nam->get(QNetworkRequest(baseUrl().resolved(QUrl(path))));
    QSignalSpy finished(reply, &QNetworkReply::finished);
    finished.wait(10'000);
    return reply;
}

void TestSpeedTest::serverDownload_data()
{
    QTest::addColumn<qint64>("bytes");
    QTest::newRow("empty") << qint64(0);
    QTest::newRow("small") << qint64(1);
    QTest::newRow("odd size") << qint64(1'234'567);
    QTest::newRow("larger than write buffer") << qint64(20'000'000);
}

void TestSpeedTest::serverDownload()
{
    QFETCH(qint64, bytes);
    QScopedPointer<QNetworkReply> reply(get(QStringLiteral("/__down?bytes=%1").arg(bytes)));
    QCOMPARE(reply->error(), QNetworkReply::NoError);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(reply->readAll().size(), bytes);
}

void TestSpeedTest::serverUpload()
{
    const QByteArray body(5'000'000, 'x');
    QNetworkRequest request(baseUrl().resolved(QUrl(QStringLiteral("/__up"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/octet-stream"));
    QScopedPointer<QNetworkReply> reply(m_nam->post(request, body));
    QSignalSpy finished(reply.data(), &QNetworkReply::finished);
    QVERIFY(finished.wait(10'000));
    QCOMPARE(reply->error(), QNetworkReply::NoError);
    QCOMPARE(reply->readAll(), QByteArray("OK\n"));
    QCOMPARE(m_server->bytesReceived(), qint64(body.size()));
}

void TestSpeedTest::serverRejectsBadRequests_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("status");
    QTest::newRow("missing bytes") << QStringLiteral("/__down") << 400;
    QTest::newRow("invalid bytes") << QStringLiteral("/__down?bytes=abc") << 400;
    QTest::newRow("negative bytes") << QStringLiteral("/__down?bytes=-5") << 400;
    QTest::newRow("unknown path") << QStringLiteral("/nope") << 404;
    QTest::newRow("invalid ckSize") << QStringLiteral("/garbage.php?ckSize=abc") << 400;
    QTest::newRow("negative ckSize") << QStringLiteral("/garbage.php?ckSize=-1") << 400;
}

void TestSpeedTest::serverRejectsBadRequests()
{
    QFETCH(QString, path);
    QFETCH(int, status);
    QScopedPointer<QNetworkReply> reply(get(path));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), status);
}

void TestSpeedTest::serverLibreSpeed()
{
    QScopedPointer<QNetworkReply> download(get(QStringLiteral("/garbage.php?ckSize=3")));
    QCOMPARE(download->error(), QNetworkReply::NoError);
    QCOMPARE(download->readAll().size(), qint64(3 * 1024 * 1024));

    QScopedPointer<QNetworkReply> ping(get(QStringLiteral("/empty.php")));
    QCOMPARE(ping->error(), QNetworkReply::NoError);

    const QByteArray body(1'000'000, 'x');
    QNetworkRequest request(baseUrl().resolved(QUrl(QStringLiteral("/empty.php"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/octet-stream"));
    QScopedPointer<QNetworkReply> upload(m_nam->post(request, body));
    QSignalSpy finished(upload.data(), &QNetworkReply::finished);
    QVERIFY(finished.wait(10'000));
    QCOMPARE(upload->error(), QNetworkReply::NoError);
    QCOMPARE(m_server->bytesReceived(), qint64(body.size()));
}

void TestSpeedTest::fullRun_data()
{
    QTest::addColumn<QString>("path");
    QTest::newRow("cloudflare protocol") << QString();
    QTest::newRow("librespeed protocol") << QStringLiteral("/garbage.php");
}

void TestSpeedTest::fullRun()
{
    QFETCH(QString, path);

    SpeedTest test;
    QSignalSpy phases(&test, &SpeedTest::phaseChanged);
    QSignalSpy samples(&test, &SpeedTest::throughputSample);
    QSignalSpy finished(&test, &SpeedTest::finished);
    QSignalSpy failed(&test, &SpeedTest::failed);

    SpeedTestConfig config;
    config.baseUrl = baseUrl().resolved(QUrl(path));
    config.durationSec = 1;
    config.streams = 2;
    config.pingCount = 3;
    test.start(config);
    QVERIFY(test.isRunning());

    QVERIFY(finished.wait(20'000));
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QVERIFY(!test.isRunning());

    QList<SpeedTest::Phase> sequence;
    for (const QList<QVariant> &args : phases)
        sequence << args.first().value<SpeedTest::Phase>();
    QCOMPARE(sequence, (QList<SpeedTest::Phase>{SpeedTest::Phase::Latency, SpeedTest::Phase::Download,
                                                SpeedTest::Phase::Upload, SpeedTest::Phase::Done}));

    const SpeedTestResult &result = test.result();
    QVERIFY(result.pingMs > 0.0);
    QVERIFY(result.jitterMs >= 0.0);
    QVERIFY(result.downloadMbps > 0.0);
    QVERIFY(result.uploadMbps > 0.0);
    QVERIFY(result.downloadedBytes > 0);
    QVERIFY(result.uploadedBytes > 0);
    QVERIFY(samples.size() >= 4);
}

void TestSpeedTest::stopWhileRunning()
{
    SpeedTest test;
    QSignalSpy phases(&test, &SpeedTest::phaseChanged);
    QSignalSpy finished(&test, &SpeedTest::finished);

    SpeedTestConfig config;
    config.baseUrl = baseUrl();
    config.durationSec = 5;
    test.start(config);

    QTRY_COMPARE_WITH_TIMEOUT(test.phase(), SpeedTest::Phase::Download, 10'000);
    test.stop();
    QCOMPARE(test.phase(), SpeedTest::Phase::Idle);
    QVERIFY(!test.isRunning());

    QTest::qWait(500);
    QVERIFY(finished.isEmpty());
    QCOMPARE(test.phase(), SpeedTest::Phase::Idle);
}

void TestSpeedTest::connectionRefused()
{
    const quint16 port = m_server->port();
    m_server->stop();

    SpeedTest test;
    QSignalSpy failed(&test, &SpeedTest::failed);

    SpeedTestConfig config;
    config.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port));
    test.start(config);

    QVERIFY(failed.wait(10'000));
    QCOMPARE(test.phase(), SpeedTest::Phase::Idle);
}

void TestSpeedTest::loadTestStages_data()
{
    QTest::addColumn<bool>("upload");
    QTest::newRow("download") << false;
    QTest::newRow("upload") << true;
}

void TestSpeedTest::loadTestStages()
{
    QFETCH(bool, upload);

    LoadTest test;
    QSignalSpy connections(m_server, &TestServer::connectionCountChanged);
    QSignalSpy stages(&test, &LoadTest::stageStarted);
    QSignalSpy samples(&test, &LoadTest::sample);
    QSignalSpy finished(&test, &LoadTest::finished);
    QSignalSpy failed(&test, &LoadTest::failed);

    LoadTestConfig config;
    config.baseUrl = baseUrl();
    config.stages = {1, 3, 8}; // 8 > Qt's 6 connections per manager
    config.stageDurationSec = 2;
    config.upload = upload;
    test.start(config);
    QVERIFY(test.isRunning());

    QVERIFY(finished.wait(30'000));
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QVERIFY(!test.isRunning());
    QCOMPARE(stages.size(), 3);
    QVERIFY(samples.size() >= 12);
    QVERIFY(test.idleLatencyMs() > 0.0);

    const QList<LoadStageResult> &results = test.results();
    QCOMPARE(results.size(), 3);
    const QList<int> expectedClients{1, 3, 8};
    for (qsizetype i = 0; i < results.size(); ++i) {
        const LoadStageResult &r = results[i];
        QCOMPARE(r.clients, expectedClients[i]);
        QVERIFY(r.totalMbps > 0.0);
        QVERIFY(r.clientMinMbps > 0.0); // every client got data
        QVERIFY(r.clientMinMbps <= r.clientAvgMbps + 1e-9);
        QVERIFY(r.clientAvgMbps <= r.clientMaxMbps + 1e-9);
        QVERIFY(qAbs(r.clientAvgMbps * r.clients - r.totalMbps) < 1e-6 * r.totalMbps + 1e-9);
        QVERIFY(r.bytes > 0);
    }
    // The 8 clients of the last stage must have run in parallel, each on its
    // own connection (plus the ping connection).
    int peak = 0;
    for (const QList<QVariant> &args : std::as_const(connections))
        peak = std::max(peak, args.first().toInt());
    QVERIFY2(peak >= 9, qPrintable(QStringLiteral("peak connections: %1").arg(peak)));
}

void TestSpeedTest::loadTestStop()
{
    LoadTest test;
    QSignalSpy stopped(&test, &LoadTest::stopped);
    QSignalSpy stageFinished(&test, &LoadTest::stageFinished);
    QSignalSpy finished(&test, &LoadTest::finished);

    LoadTestConfig config;
    config.baseUrl = baseUrl();
    config.stages = {2, 4};
    config.stageDurationSec = 5;
    test.start(config);

    QVERIFY(QTest::qWaitFor([&] { return m_server->connectionCount() >= 2; }, 10'000));
    test.stop();
    QCOMPARE(stopped.size(), 1);
    QVERIFY(!test.isRunning());

    QTest::qWait(1500);
    QVERIFY(stageFinished.isEmpty());
    QVERIFY(finished.isEmpty());
}

void TestSpeedTest::loadTestMultipleServers_data()
{
    QTest::addColumn<bool>("upload");
    QTest::newRow("download") << false;
    QTest::newRow("upload") << true;
}

// Clients are spread round-robin over all reachable servers; an unreachable
// extra server is skipped instead of failing the test.
void TestSpeedTest::loadTestMultipleServers()
{
    QFETCH(bool, upload);

    TestServer second;
    QVERIFY2(second.start(0, QHostAddress::LocalHost), qPrintable(second.errorString()));
    TestServer gone;
    QVERIFY(gone.start(0, QHostAddress::LocalHost));
    const quint16 gonePort = gone.port();
    gone.stop();

    const QUrl secondUrl(QStringLiteral("http://127.0.0.1:%1/garbage.php").arg(second.port()));
    const QUrl goneUrl(QStringLiteral("http://127.0.0.1:%1").arg(gonePort));

    LoadTest test;
    QSignalSpy unavailable(&test, &LoadTest::serverUnavailable);
    QSignalSpy ready(&test, &LoadTest::serversReady);
    QSignalSpy finished(&test, &LoadTest::finished);
    QSignalSpy failed(&test, &LoadTest::failed);

    LoadTestConfig config;
    config.baseUrl = baseUrl();
    config.extraServers = {goneUrl, secondUrl, baseUrl()}; // the main server again is ignored
    config.stages = {4};
    config.stageDurationSec = 2;
    config.upload = upload;
    test.start(config);

    QVERIFY(finished.wait(30'000));
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(unavailable.size(), 1);
    QCOMPARE(unavailable.first().first().toUrl(), goneUrl);
    QCOMPARE(ready.size(), 1);
    QCOMPARE(test.servers(), (QList<QUrl>{baseUrl(), secondUrl}));

    const QList<LoadStageResult> &results = test.results();
    QCOMPARE(results.size(), 1);
    QCOMPARE(results.first().clients, 4);
    QVERIFY(results.first().clientMinMbps > 0.0); // every client got data

    // Both servers carried transfer traffic, not just a ping.
    for (const TestServer *server : std::initializer_list<const TestServer *>{m_server, &second}) {
        const qint64 traffic = upload ? server->bytesReceived() : server->bytesSent();
        QVERIFY2(traffic > 1'000'000, qPrintable(QStringLiteral("traffic: %1").arg(traffic)));
    }
}

QTEST_GUILESS_MAIN(TestSpeedTest)
#include "tst_speedtest.moc"
