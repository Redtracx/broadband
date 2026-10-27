#include "LoadTest.h"

#include "HttpCommon.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>

#include <algorithm>
#include <utility>

namespace {

constexpr int kSampleIntervalMs = 250;
constexpr qint64 kSmoothingWindowMs = 1000;
// Ignore the ramp-up at the start of every stage for the stage result.
constexpr double kWarmupFraction = 0.2;
constexpr int kIdlePings = 5;
constexpr int kPingIntervalMs = 250;
// Give queues in routers and the server time to drain between stages.
constexpr int kStagePauseMs = 1000;
constexpr int kMaxClients = 64;

} // namespace

LoadTest::LoadTest(QObject *parent)
    : QObject(parent)
    , m_pingNam(new QNetworkAccessManager(this))
{
    m_sampleTimer.setInterval(kSampleIntervalMs);
    m_sampleTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_sampleTimer, &QTimer::timeout, this, &LoadTest::onSampleTimer);
}

LoadTest::~LoadTest()
{
    abortPing();
    abortChecks();
    teardownClients();
}

double LoadTest::totalDuration(const LoadTestConfig &config)
{
    return double(config.stages.size()) * std::max(1, config.stageDurationSec);
}

void LoadTest::start(const LoadTestConfig &config)
{
    stop();

    m_config = config;
    m_config.stageDurationSec = std::max(2, m_config.stageDurationSec);
    for (int &clients : m_config.stages)
        clients = std::clamp(clients, 1, kMaxClients);

    ++m_runId;
    m_running = true;
    m_results.clear();
    m_stageIndex = -1;
    m_idlePhase = true;
    m_idlePings.clear();
    m_idlePingsDone = 0;
    m_servers = {m_config.baseUrl};
    m_checkedServers.clear();

    if (m_config.stages.isEmpty()) {
        fail(tr("Keine Laststufen angegeben."));
        return;
    }

    m_pingNam->clearConnectionCache();
    m_pingNam->setProxy(http::proxyFor(m_config.baseUrl));

    emit progressChanged(0.0);
    sendPing();
}

void LoadTest::stop()
{
    if (!m_running)
        return;
    ++m_runId;
    m_running = false;
    m_stageActive = false;
    m_sampleTimer.stop();
    abortPing();
    abortChecks();
    teardownClients();
    emit stopped();
}

void LoadTest::fail(const QString &message)
{
    ++m_runId;
    m_running = false;
    m_stageActive = false;
    m_sampleTimer.stop();
    abortPing();
    abortChecks();
    teardownClients();
    emit failed(message);
}

void LoadTest::abortPing()
{
    if (!m_pingReply)
        return;
    m_pingReply->disconnect(this);
    m_pingReply->abort();
    m_pingReply->deleteLater();
    m_pingReply = nullptr;
}

void LoadTest::abortChecks()
{
    const auto checks = std::exchange(m_checks, {});
    for (QNetworkReply *reply : checks) {
        reply->disconnect(this);
        reply->abort();
        reply->manager()->deleteLater(); // also deletes the reply
    }
}

void LoadTest::teardownClients()
{
    const auto clients = std::exchange(m_clients, {});
    for (Client *client : clients) {
        if (client->reply) {
            client->reply->disconnect(this);
            client->reply->abort();
        }
        client->nam->deleteLater(); // also deletes its replies
        delete client;
    }
}

// --- Latency ----------------------------------------------------------------

void LoadTest::sendPing()
{
    m_pingReply = m_pingNam->get(http::makeRequest(http::pingUrl(m_config.baseUrl)));
    m_pingTimer.start();
    connect(m_pingReply, &QNetworkReply::finished, this, [this] {
        const double elapsedMs = double(m_pingTimer.nsecsElapsed()) / 1e6;
        QNetworkReply *reply = std::exchange(m_pingReply, nullptr);
        reply->deleteLater();

        if (reply->error() == QNetworkReply::NoError) {
            onPingFinished(elapsedMs);
        } else if (m_idlePhase) {
            fail(tr("Server nicht erreichbar: %1").arg(reply->errorString()));
        } else {
            // A lost ping under load is no reason to abort; just try again.
            onPingFinished(-1.0);
        }
    });
}

void LoadTest::onPingFinished(double elapsedMs)
{
    if (m_idlePhase) {
        // The first request pays for connection setup and isn't counted.
        if (m_idlePingsDone++ > 0)
            m_idlePings.append(elapsedMs);
        if (m_idlePingsDone <= kIdlePings) {
            sendPing();
            return;
        }
        m_idlePhase = false;
        m_idleLatencyMs = http::median(m_idlePings);
        emit idleLatencyMeasured(m_idleLatencyMs);
        checkExtraServers();
        return;
    }

    if (elapsedMs >= 0.0 && m_warmedUp)
        m_stagePings.append(elapsedMs);

    const int runId = m_runId;
    QTimer::singleShot(kPingIntervalMs, this, [this, runId] {
        if (runId == m_runId && m_stageActive && !m_pingReply)
            sendPing();
    });
}

// One request to every extra server, all at once. Servers that don't answer
// are left out instead of failing the whole test.
void LoadTest::checkExtraServers()
{
    for (const QUrl &server : std::as_const(m_config.extraServers)) {
        if (m_servers.contains(server) || m_checkedServers.contains(server))
            continue;
        auto *nam = new QNetworkAccessManager(this);
        nam->setProxy(http::proxyFor(server));
        QNetworkReply *reply = nam->get(http::makeRequest(http::pingUrl(server)));
        m_checks.append(reply);
        m_checkedServers.append(server); // reachable ones stay, see below
        connect(reply, &QNetworkReply::finished, this, [this, reply, nam, server] {
            m_checks.removeOne(reply);
            nam->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                m_checkedServers.removeOne(server);
                emit serverUnavailable(server, reply->errorString());
            }
            if (m_checks.isEmpty()) {
                // Keep the configured order.
                for (const QUrl &extra : std::as_const(m_config.extraServers)) {
                    if (m_checkedServers.contains(extra) && !m_servers.contains(extra))
                        m_servers.append(extra);
                }
                emit serversReady(m_servers);
                startStage(0);
            }
        });
    }
    if (m_checks.isEmpty()) {
        emit serversReady(m_servers);
        startStage(0);
    }
}

// --- Stages -----------------------------------------------------------------

void LoadTest::startStage(int index)
{
    m_stageIndex = index;
    const int clients = m_config.stages[index];
    emit stageStarted(index, clients);

    m_stageOffset = double(index) * m_config.stageDurationSec;
    m_warmupMs = qint64(m_config.stageDurationSec * 1000 * kWarmupFraction);
    m_warmedUp = false;
    m_stagePings.clear();
    m_window = {{0, 0}};

    m_stageTimer.start();
    for (int i = 0; i < clients; ++i) {
        // A separate access manager per client: each one behaves like its own
        // device with its own connection (and Qt's limit of six connections
        // per host applies per manager).
        auto *client = new Client;
        client->server = m_servers[i % m_servers.size()];
        client->nam = new QNetworkAccessManager(this);
        client->nam->setProxy(http::proxyFor(client->server));
        m_clients.append(client);
        launch(client);
    }
    m_stageActive = true;
    m_sampleTimer.start();
    if (!m_pingReply)
        sendPing();
}

void LoadTest::launch(Client *client)
{
    QNetworkReply *reply = nullptr;
    if (m_config.upload) {
        client->uploadSent = 0;
        reply = client->nam->post(http::makeUploadRequest(client->server), http::uploadPayload());
        connect(reply, &QNetworkReply::uploadProgress, this, [client](qint64 sent, qint64) {
            if (sent > client->uploadSent) {
                client->bytes += sent - client->uploadSent;
                client->uploadSent = sent;
            }
        });
    } else {
        reply = client->nam->get(http::makeRequest(http::downloadUrl(client->server)));
        connect(reply, &QNetworkReply::readyRead, this, [client, reply] {
            client->bytes += reply->readAll().size();
        });
    }

    client->reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, client, reply] {
        client->reply = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            fail(tr("Übertragung fehlgeschlagen (%1 gleichzeitige Clients, Server %2): %3")
                     .arg(m_config.stages.value(m_stageIndex))
                     .arg(client->server.host(), reply->errorString()));
            return;
        }
        launch(client);
    });
}

void LoadTest::onSampleTimer()
{
    const qint64 now = m_stageTimer.elapsed();
    qint64 total = 0;
    for (const Client *client : std::as_const(m_clients))
        total += client->bytes;

    while (m_window.size() > 1 && now - m_window[1].first >= kSmoothingWindowMs)
        m_window.removeFirst();
    const auto [startMs, startBytes] = m_window.first();
    if (now > startMs) {
        const double mbps = double(total - startBytes) * 8.0 / (double(now - startMs) * 1000.0);
        emit sample(m_stageOffset + double(now) / 1000.0, mbps);
    }
    m_window.append({now, total});

    if (!m_warmedUp && now >= m_warmupMs) {
        for (Client *client : std::as_const(m_clients))
            client->warmupBytes = client->bytes;
        m_warmupMs = now;
        m_warmedUp = true;
    }

    const qint64 durationMs = qint64(m_config.stageDurationSec) * 1000;
    const double stageFraction = std::min(1.0, double(now) / double(durationMs));
    emit progressChanged((m_stageIndex + stageFraction) / double(m_config.stages.size()));

    if (now >= durationMs)
        finishStage();
}

void LoadTest::finishStage()
{
    m_sampleTimer.stop();
    m_stageActive = false;
    const qint64 now = m_stageTimer.elapsed();
    const qint64 measuredMs = std::max<qint64>(1, now - m_warmupMs);

    LoadStageResult result;
    result.clients = int(m_clients.size());
    result.seconds = double(now) / 1000.0;
    result.clientMinMbps = -1.0;
    for (const Client *client : std::as_const(m_clients)) {
        const double mbps = double(client->bytes - client->warmupBytes) * 8.0 / (double(measuredMs) * 1000.0);
        result.totalMbps += mbps;
        result.clientMaxMbps = std::max(result.clientMaxMbps, mbps);
        result.clientMinMbps = result.clientMinMbps < 0.0 ? mbps : std::min(result.clientMinMbps, mbps);
        result.bytes += client->bytes;
    }
    result.clientMinMbps = std::max(0.0, result.clientMinMbps);
    result.clientAvgMbps = result.clients > 0 ? result.totalMbps / result.clients : 0.0;
    result.latencyMs = m_stagePings.isEmpty() ? -1.0 : http::median(m_stagePings);

    abortPing();
    teardownClients();
    m_results.append(result);
    const int index = m_stageIndex;
    emit stageFinished(index, result);

    if (index + 1 < m_config.stages.size()) {
        const int runId = m_runId;
        QTimer::singleShot(kStagePauseMs, this, [this, runId, index] {
            if (runId == m_runId && m_running)
                startStage(index + 1);
        });
    } else {
        m_running = false;
        emit progressChanged(1.0);
        emit finished();
    }
}
