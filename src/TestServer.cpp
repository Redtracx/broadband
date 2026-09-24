#include "TestServer.h"

#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <vector>

namespace {

constexpr int kMaxHeaderSize = 16 * 1024;
constexpr qint64 kMaxDownload = 1'000'000'000;
constexpr qint64 kMaxUpload = 1'000'000'000;
constexpr qint64 kWriteHighWater = 4 * 1024 * 1024;

// 1 MB of random data that every download response is assembled from.
const QByteArray &payload()
{
    static const QByteArray data = [] {
        std::vector<quint32> noise(256 * 1024);
        QRandomGenerator::global()->fillRange(noise.data(), qsizetype(noise.size()));
        return QByteArray(reinterpret_cast<const char *>(noise.data()),
                          qsizetype(noise.size() * sizeof(quint32)));
    }();
    return data;
}

// Serves one client connection. Requests are handled strictly one after
// another, which is all a speed test client needs.
class HttpConnection : public QObject
{
public:
    HttpConnection(QTcpSocket *socket, TestServer *server)
        : QObject(socket)
        , m_socket(socket)
        , m_server(server)
    {
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(m_socket, &QTcpSocket::readyRead, this, &HttpConnection::process);
        connect(m_socket, &QTcpSocket::bytesWritten, this, &HttpConnection::onBytesWritten);
        connect(m_socket, &QTcpSocket::disconnected, m_socket, &QObject::deleteLater);
    }

private:
    enum class State { Headers, Body, Sending };

    void process()
    {
        for (;;) {
            switch (m_state) {
            case State::Headers: {
                m_buffer += m_socket->readAll();
                const auto end = m_buffer.indexOf("\r\n\r\n");
                if (end < 0) {
                    if (m_buffer.size() > kMaxHeaderSize) {
                        m_keepAlive = false;
                        respond(431, "Request Header Fields Too Large");
                    }
                    return;
                }
                const QByteArray head = m_buffer.left(end);
                m_buffer.remove(0, end + 4);
                handleRequest(head);
                break;
            }
            case State::Body: {
                const qint64 fromBuffer = std::min<qint64>(m_remaining, m_buffer.size());
                m_buffer.remove(0, fromBuffer);
                m_remaining -= fromBuffer;
                qint64 received = fromBuffer;
                while (m_remaining > 0 && m_socket->bytesAvailable() > 0) {
                    const auto chunk = m_socket->read(std::min<qint64>(m_remaining, 1 << 20));
                    m_remaining -= chunk.size();
                    received += chunk.size();
                }
                m_server->addTraffic(0, received);
                if (m_remaining > 0)
                    return;
                m_state = State::Headers;
                respond(200, "OK", "OK\n");
                break;
            }
            case State::Sending:
                // Wait until the response is out before reading the next request.
                return;
            }
            if (m_socket->state() != QAbstractSocket::ConnectedState)
                return;
        }
    }

    void handleRequest(const QByteArray &head)
    {
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        if (requestLine.size() != 3) {
            m_keepAlive = false;
            respond(400, "Bad Request");
            return;
        }
        const QByteArray &method = requestLine[0];
        const QByteArray &target = requestLine[1];
        const QByteArray &version = requestLine[2];

        QByteArray connection;
        QByteArray contentLength;
        bool chunked = false;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray &line = lines[i];
            const auto colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "connection")
                connection = value.toLower();
            else if (name == "content-length")
                contentLength = value;
            else if (name == "transfer-encoding")
                chunked = value.toLower() != "identity";
        }
        m_keepAlive = version == "HTTP/1.1" ? connection != "close" : connection == "keep-alive";

        const QUrl url(QString::fromLatin1(target));
        const QString path = url.path();

        if (method == "GET" && path == QLatin1String("/__down")) {
            bool ok = false;
            const qint64 bytes = QUrlQuery(url).queryItemValue(QStringLiteral("bytes")).toLongLong(&ok);
            if (!ok || bytes < 0 || bytes > kMaxDownload) {
                respond(400, "Bad Request", "invalid 'bytes' parameter\n");
                return;
            }
            writeHead(200, "OK", bytes, "application/octet-stream");
            m_remaining = bytes;
            m_state = State::Sending;
            pump();
        } else if (method == "POST" && path == QLatin1String("/__up")) {
            bool ok = false;
            const qint64 length = contentLength.toLongLong(&ok);
            if (chunked || !ok || length < 0) {
                m_keepAlive = false;
                respond(411, "Length Required");
                return;
            }
            if (length > kMaxUpload) {
                m_keepAlive = false;
                respond(413, "Payload Too Large");
                return;
            }
            m_remaining = length;
            m_state = State::Body;
        } else if (method == "GET" && path == QLatin1String("/")) {
            respond(200, "OK", "broadband test server " BROADBAND_VERSION "\n");
        } else {
            respond(404, "Not Found");
        }
    }

    void writeHead(int status, const char *reason, qint64 length, const char *type)
    {
        QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n";
        head += "Content-Type: " + QByteArray(type) + "\r\n";
        head += "Content-Length: " + QByteArray::number(length) + "\r\n";
        head += "Cache-Control: no-store\r\n";
        head += "Access-Control-Allow-Origin: *\r\n";
        head += m_keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
        m_socket->write(head);
    }

    void respond(int status, const char *reason, const QByteArray &body = {})
    {
        const QByteArray text = body.isEmpty() ? QByteArray(reason) + '\n' : body;
        writeHead(status, reason, text.size(), "text/plain; charset=utf-8");
        m_socket->write(text);
        finishResponse();
    }

    void pump()
    {
        const QByteArray &data = payload();
        while (m_remaining > 0 && m_socket->bytesToWrite() < kWriteHighWater) {
            const qint64 n = std::min<qint64>(m_remaining, data.size());
            m_socket->write(data.constData(), n);
            m_remaining -= n;
        }
        if (m_remaining == 0)
            finishResponse();
    }

    void onBytesWritten(qint64 bytes)
    {
        m_server->addTraffic(bytes, 0);
        if (m_state == State::Sending)
            pump();
    }

    void finishResponse()
    {
        m_state = State::Headers;
        if (!m_keepAlive) {
            m_socket->disconnectFromHost();
            return;
        }
        // A pipelined request may already be waiting in the buffer.
        if (!m_buffer.isEmpty() || m_socket->bytesAvailable() > 0)
            QMetaObject::invokeMethod(this, [this] { process(); }, Qt::QueuedConnection);
    }

    QTcpSocket *m_socket;
    TestServer *m_server;
    State m_state = State::Headers;
    QByteArray m_buffer;
    qint64 m_remaining = 0;
    bool m_keepAlive = true;
};

} // namespace

TestServer::TestServer(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &TestServer::onNewConnection);
}

TestServer::~TestServer()
{
    // Tear down without emitting signals: receivers may already be gone.
    const auto sockets = m_server.findChildren<QTcpSocket *>();
    for (QTcpSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
    }
    m_server.close();
}

bool TestServer::start(quint16 port, const QHostAddress &address)
{
    stop();
    return m_server.listen(address, port);
}

void TestServer::stop()
{
    m_server.close();
    const auto sockets = m_server.findChildren<QTcpSocket *>();
    for (QTcpSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    if (m_connections != 0) {
        m_connections = 0;
        emit connectionCountChanged(0);
    }
}

void TestServer::addTraffic(qint64 sent, qint64 received)
{
    m_bytesSent += sent;
    m_bytesReceived += received;
}

void TestServer::onNewConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        new HttpConnection(socket, this);
        ++m_connections;
        emit connectionCountChanged(m_connections);
        connect(socket, &QObject::destroyed, this, [this] {
            --m_connections;
            emit connectionCountChanged(m_connections);
        });
    }
}
