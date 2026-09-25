#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTcpServer>

// Minimal HTTP/1.1 server implementing the speed test endpoints, so the app
// can measure the bandwidth between two machines in a local network:
//
//   GET  /__down?bytes=N   responds with N bytes of random data
//   POST /__up             reads and discards the request body
//   GET  /                 health check
class TestServer : public QObject
{
    Q_OBJECT

public:
    explicit TestServer(QObject *parent = nullptr);
    ~TestServer() override;

    // Idle connections are closed after this time, and connections beyond
    // the limit are refused, so forgotten or hostile clients can't pile up.
    static constexpr int kDefaultIdleTimeoutMs = 30'000;
    static constexpr int kDefaultMaxConnections = 256;

    bool start(quint16 port, const QHostAddress &address = QHostAddress::Any);
    void stop();

    bool isListening() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    QString errorString() const { return m_server.errorString(); }
    int connectionCount() const { return m_connections; }
    qint64 bytesSent() const { return m_bytesSent; }
    qint64 bytesReceived() const { return m_bytesReceived; }

    int idleTimeoutMs() const { return m_idleTimeoutMs; }
    void setIdleTimeoutMs(int ms) { m_idleTimeoutMs = ms; }
    int maxConnections() const { return m_maxConnections; }
    void setMaxConnections(int count) { m_maxConnections = count; }

    // Called by the connection handlers.
    void addTraffic(qint64 sent, qint64 received);

signals:
    void connectionCountChanged(int count);

private:
    void onNewConnection();

    QTcpServer m_server;
    int m_connections = 0;
    int m_idleTimeoutMs = kDefaultIdleTimeoutMs;
    int m_maxConnections = kDefaultMaxConnections;
    qint64 m_bytesSent = 0;
    qint64 m_bytesReceived = 0;
};
