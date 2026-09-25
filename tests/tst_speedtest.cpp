#include "LoadTest.h"
#include "SpeedTest.h"
#include "TestServer.h"

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

namespace {

// Speaks just enough HTTP for the speed test, but breaks off the first
// `failures` large downloads midway, like a flaky connection.
class FlakyServer : public QTcpServer
{
public:
    explicit FlakyServer(int failures)
        : m_failures(failures)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { serve(socket); });
        });
    }

    int broken() const { return m_broken; }

private:
    void serve(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        for (;;) {
            qint64 &pendingBody = m_pendingBody[socket];
            if (pendingBody > 0) {
                const qint64 n = std::min<qint64>(pendingBody, buffer.size());
                buffer.remove(0, n);
                pendingBody -= n;
                if (pendingBody > 0)
                    return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nOK\n");
            }
            const auto end = buffer.indexOf("\r\n\r\n");
            if (end < 0)
                return;
            const QByteArray head = buffer.left(end);
            buffer.remove(0, end + 4);

            if (head.startsWith("POST")) {
                const auto at = head.toLower().indexOf("content-length:");
                pendingBody = head.mid(at + 15, head.indexOf('\r', at) - at - 15).trimmed().toLongLong();
                continue;
            }
            const qint64 bytes = head.mid(head.indexOf("bytes=") + 6, head.indexOf(' ', 4) - head.indexOf("bytes=") - 6)
                                     .toLongLong();
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(bytes) + "\r\n\r\n");
            if (bytes > 1'000'000 && m_broken < m_failures) {
                ++m_broken;
                socket->write(QByteArray(100'000, 'x'));
                // Close cleanly rather than abort(): a reset can make the
                // kernel drop the data already sent, and Qt then silently
                // resends the request instead of reporting an error.
                socket->disconnectFromHost();
                return;
            }
            socket->write(QByteArray(bytes, 'x'));
        }
    }

    int m_failures;
    int m_broken = 0;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, qint64> m_pendingBody;
};

} // namespace

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
    void serverIdleTimeout();
    void serverConnectionLimit();

    void fullRun();
    void stopWhileRunning();
    void connectionRefused();
    void moreThanSixStreams();
    void survivesDroppedConnections();
    void failsWhenTransfersKeepBreaking();
    void forcedIpVersion();

    void loadTestStages_data();
    void loadTestStages();
    void loadTestStop();
    void loadTestSurvivesDroppedConnections();

private:
    QNetworkReply *get(const QString &path);
    QUrl baseUrl() const;
    int peakConnections(const QSignalSpy &spy) const;

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

int TestSpeedTest::peakConnections(const QSignalSpy &spy) const
{
    int peak = 0;
    for (const QList<QVariant> &args : spy)
        peak = std::max(peak, args.first().toInt());
    return peak;
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
}

void TestSpeedTest::serverRejectsBadRequests()
{
    QFETCH(QString, path);
    QFETCH(int, status);
    QScopedPointer<QNetworkReply> reply(get(path));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), status);
}

void TestSpeedTest::serverIdleTimeout()
{
    m_server->setIdleTimeoutMs(300);
    QTcpSocket socket;
    QSignalSpy disconnected(&socket, &QTcpSocket::disconnected);
    socket.connectToHost(QHostAddress::LocalHost, m_server->port());
    QVERIFY(socket.waitForConnected(5000));
    socket.write("GET /__down?by"); // never finishes the request
    QVERIFY(disconnected.wait(5000));
    QTRY_COMPARE_WITH_TIMEOUT(m_server->connectionCount(), 0, 5000);
}

void TestSpeedTest::serverConnectionLimit()
{
    m_server->setMaxConnections(1);
    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, m_server->port());
    QVERIFY(first.waitForConnected(5000));
    QTRY_COMPARE_WITH_TIMEOUT(m_server->connectionCount(), 1, 5000);

    QTcpSocket second;
    QSignalSpy disconnected(&second, &QTcpSocket::disconnected);
    second.connectToHost(QHostAddress::LocalHost, m_server->port());
    QVERIFY(second.waitForConnected(5000));
    QVERIFY(disconnected.wait(5000));
    QCOMPARE(m_server->connectionCount(), 1);

    // The first connection still works.
    QSignalSpy readyRead(&first, &QTcpSocket::readyRead);
    first.write("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    QVERIFY(readyRead.wait(5000));
    QVERIFY(first.readAll().startsWith("HTTP/1.1 200"));
}

void TestSpeedTest::fullRun()
{
    SpeedTest test;
    QSignalSpy phases(&test, &SpeedTest::phaseChanged);
    QSignalSpy samples(&test, &SpeedTest::throughputSample);
    QSignalSpy finished(&test, &SpeedTest::finished);
    QSignalSpy failed(&test, &SpeedTest::failed);

    SpeedTestConfig config;
    config.baseUrl = baseUrl();
    config.durationSec = 2;
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
    QVERIFY(result.downloadLatencyMs > 0.0); // pinged while loaded
    QVERIFY(result.uploadLatencyMs > 0.0);
    QCOMPARE(result.retries, 0);
    QCOMPARE(result.server.clientIp, QStringLiteral("127.0.0.1"));
    QCOMPARE(result.server.ipVersion(), 4);
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

void TestSpeedTest::moreThanSixStreams()
{
    SpeedTest test;
    QSignalSpy connections(m_server, &TestServer::connectionCountChanged);
    QSignalSpy finished(&test, &SpeedTest::finished);
    QSignalSpy failed(&test, &SpeedTest::failed);

    SpeedTestConfig config;
    config.baseUrl = baseUrl();
    config.durationSec = 1;
    config.streams = 10;
    config.pingCount = 2;
    test.start(config);

    QVERIFY(finished.wait(20'000));
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    // 10 transfer connections plus the ping connection
    const int peak = peakConnections(connections);
    QVERIFY2(peak >= 11, qPrintable(QStringLiteral("peak connections: %1").arg(peak)));
}

void TestSpeedTest::survivesDroppedConnections()
{
    FlakyServer flaky(3);
    QVERIFY(flaky.listen(QHostAddress::LocalHost));

    SpeedTest test;
    QSignalSpy finished(&test, &SpeedTest::finished);
    QSignalSpy failed(&test, &SpeedTest::failed);

    SpeedTestConfig config;
    config.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(flaky.serverPort()));
    config.durationSec = 2;
    config.streams = 2;
    config.pingCount = 2;
    test.start(config);

    QVERIFY(finished.wait(20'000));
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(flaky.broken(), 3);
    // Qt may resend a broken request on its own before reporting an error.
    QVERIFY2(test.result().retries >= 1 && test.result().retries <= 3,
             qPrintable(QString::number(test.result().retries)));
    QVERIFY(test.result().downloadMbps > 0.0);
    QVERIFY(test.result().uploadMbps > 0.0);
}

void TestSpeedTest::failsWhenTransfersKeepBreaking()
{
    FlakyServer flaky(1'000'000);
    QVERIFY(flaky.listen(QHostAddress::LocalHost));

    SpeedTest test;
    QSignalSpy failed(&test, &SpeedTest::failed);

    SpeedTestConfig config;
    config.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(flaky.serverPort()));
    config.durationSec = 10;
    config.streams = 2;
    config.pingCount = 2;
    test.start(config);

    // Gives up after a handful of attempts instead of retrying forever.
    QVERIFY(failed.wait(10'000));
    QVERIFY(failed.first().first().toString().startsWith(QStringLiteral("Download")));
    // Up to max(3, 2 * streams) = 4 errors are retried, the fifth ends the
    // test. The server may count more breaks: the other stream breaks in the
    // meantime, and Qt resends some requests on its own before reporting.
    QVERIFY2(flaky.broken() >= 5, qPrintable(QString::number(flaky.broken())));
    QCOMPARE(test.phase(), SpeedTest::Phase::Idle);
}

void TestSpeedTest::forcedIpVersion()
{
    SpeedTest test;
    QSignalSpy finished(&test, &SpeedTest::finished);
    QSignalSpy failed(&test, &SpeedTest::failed);

    // "localhost" is resolved up front; requests go to 127.0.0.1 with the
    // name in the Host header.
    SpeedTestConfig config;
    config.baseUrl = QUrl(QStringLiteral("http://localhost:%1").arg(m_server->port()));
    config.durationSec = 1;
    config.pingCount = 2;
    config.ipVersion = http::IpVersion::V4;
    test.start(config);

    QVERIFY(finished.wait(20'000));
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(test.result().server.ipVersion(), 4);

    const http::Target target{QUrl(QStringLiteral("https://example.org:8443/base")), QStringLiteral("example.org")};
    const QNetworkRequest request = target.request(QStringLiteral("__down"), 0);
    QCOMPARE(request.rawHeader("Host"), QByteArray("example.org:8443"));
    QCOMPARE(request.peerVerifyName(), QStringLiteral("example.org"));
    QCOMPARE(request.url().path(), QStringLiteral("/base/__down"));
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
    const int peak = peakConnections(connections);
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

void TestSpeedTest::loadTestSurvivesDroppedConnections()
{
    FlakyServer flaky(4);
    QVERIFY(flaky.listen(QHostAddress::LocalHost));

    LoadTest test;
    QSignalSpy finished(&test, &LoadTest::finished);
    QSignalSpy failed(&test, &LoadTest::failed);

    LoadTestConfig config;
    config.baseUrl = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(flaky.serverPort()));
    config.stages = {4};
    config.stageDurationSec = 2;
    test.start(config);

    QVERIFY(finished.wait(20'000));
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(test.results().size(), 1);
    // Qt may resend a broken request on its own before reporting an error.
    QVERIFY2(test.results().first().retries >= 1 && test.results().first().retries <= 4,
             qPrintable(QString::number(test.results().first().retries)));
    QVERIFY(test.results().first().totalMbps > 0.0);
}

QTEST_GUILESS_MAIN(TestSpeedTest)
#include "tst_speedtest.moc"
