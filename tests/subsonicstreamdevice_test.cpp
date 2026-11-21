/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * End-to-end check for the whole-track stream cache: a local HTTP server implements byte ranges,
 * and the real SubsonicStreamDevice reads/seeks against it on a decoder-like worker thread.
 */

#include "../src/playback/subsonicremoteplayback.cpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

namespace {
int failures{0};

void check(bool condition, const std::string& message)
{
    if(!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

QByteArray makePayload(int size)
{
    QByteArray payload(size, Qt::Uninitialized);
    for(int i{0}; i < size; ++i) {
        payload[i] = static_cast<char>((i * 31 + i / 251) & 0xff);
    }
    return payload;
}

class RangeServer : public QObject
{
public:
    ~RangeServer() override
    {
        m_shuttingDown = true;
        m_server.close();
        const auto sockets = m_server.findChildren<QTcpSocket*>();
        for(QTcpSocket* socket : sockets) {
            QObject::disconnect(socket, nullptr, this, nullptr);
        }
    }

    explicit RangeServer(QByteArray payload, bool ignoreRanges = false, bool throttle = false,
                         bool chunked = false, qint64 dropFirstAfter = -1)
        : m_payload{std::move(payload)}
        , m_ignoreRanges{ignoreRanges}
        , m_throttle{throttle}
        , m_chunked{chunked}
        , m_dropFirstAfter{dropFirstAfter}
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while(QTcpSocket* socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    if(m_shuttingDown) {
                        return;
                    }
                    m_bodies.remove(socket);
                    m_offsets.remove(socket);
                    socket->deleteLater();
                });
                m_buffers.insert(socket, {});
            }
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    [[nodiscard]] quint16 port() const
    {
        return m_server.serverPort();
    }

    std::atomic<int> requests{0};
    std::atomic<qint64> served{0};

private:
    void onReadyRead(QTcpSocket* socket)
    {
        QByteArray& buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if(headerEnd < 0) {
            return;
        }

        const QByteArray request = buffer.left(headerEnd);
        buffer.clear();
        ++requests;

        qint64 start{0};
        qint64 end{m_payload.size() - 1};
        const QByteArray lower = request.toLower();
        const qsizetype rangePos = lower.indexOf("\r\nrange: bytes=");
        const bool hasRange = !m_chunked && rangePos >= 0 && !m_ignoreRanges;
        if(hasRange) {
            const qsizetype valuePos = rangePos + int(std::strlen("\r\nrange: bytes="));
            const QByteArray value  = request.mid(valuePos, request.indexOf("\r\n", valuePos) - valuePos);
            const qsizetype dash    = value.indexOf('-');
            if(dash >= 0) {
                start = value.left(dash).toLongLong();
                const QByteArray last = value.mid(dash + 1).trimmed();
                if(!last.isEmpty()) {
                    end = last.toLongLong();
                }
            }
            if(start >= m_payload.size()) {
                const QByteArray response = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                            "Content-Range: bytes */" + QByteArray::number(m_payload.size())
                                          + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                socket->write(response);
                socket->disconnectFromHost();
                return;
            }
            start = std::max<qint64>(0, start);
            end   = std::clamp<qint64>(end, start, m_payload.size() - 1);
        }

        const qint64 count = hasRange ? end - start + 1 : m_payload.size();
        QByteArray header;
        if(hasRange) {
            header = "HTTP/1.1 206 Partial Content\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Content-Range: bytes " + QByteArray::number(start) + '-' + QByteArray::number(end) + '/'
                   + QByteArray::number(m_payload.size()) + "\r\n"
                     "Content-Length: " + QByteArray::number(count) + "\r\n"
                     "Connection: close\r\n\r\n";
        }
        else {
            start  = 0;
            const QByteArray length = m_chunked
                ? QByteArray{}
                : "Content-Length: " + QByteArray::number(m_payload.size()) + "\r\n";
            header = "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/octet-stream\r\n" + length
                   + "Connection: close\r\n\r\n";
        }

        socket->write(header);
        if(!m_throttle) {
            qint64 toSend = count;
            if(m_dropFirstAfter >= 0 && !m_dropped) {
                toSend     = std::min(count, m_dropFirstAfter);
                m_dropped  = true;
            }
            socket->write(m_payload.constData() + start, toSend);
            served += toSend;
            socket->disconnectFromHost();
            return;
        }

        m_bodies.insert(socket, m_payload.mid(start, count));
        m_offsets.insert(socket, 0);
        auto* timer = new QTimer{socket};
        timer->setInterval(1);
        connect(timer, &QTimer::timeout, this, [this, socket, timer] {
            if(!m_bodies.contains(socket)) {
                timer->stop();
                return;
            }
            QByteArray& body = m_bodies[socket];
            qint64& offset   = m_offsets[socket];
            const qint64 chunk = std::min<qint64>(64 * 1024, body.size() - offset);
            if(chunk > 0) {
                socket->write(body.constData() + offset, chunk);
                offset += chunk;
                served += chunk;
            }
            if(offset >= body.size()) {
                timer->stop();
                socket->disconnectFromHost();
            }
        });
        timer->start();
    }

    QByteArray m_payload;
    bool m_ignoreRanges;
    bool m_throttle;
    bool m_chunked;
    qint64 m_dropFirstAfter;
    bool m_dropped{false};
    bool m_shuttingDown{false};
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QHash<QTcpSocket*, QByteArray> m_bodies;
    QHash<QTcpSocket*, qint64> m_offsets;
    QTcpServer m_server;
};

bool readExact(QIODevice& device, char* data, qint64 size)
{
    qint64 got{0};
    while(got < size) {
        const qint64 count = device.read(data + got, size - got);
        if(count <= 0) {
            return false;
        }
        got += count;
    }
    return true;
}

bool matches(const char* data, qint64 offset, const QByteArray& payload, qint64 size)
{
    if(offset < 0 || offset + size > payload.size()) {
        return false;
    }
    if(std::memcmp(data, payload.constData() + offset, static_cast<size_t>(size)) == 0) {
        return true;
    }
    qsizetype first{-1};
    for(qsizetype i{0}; i < size; ++i) {
        if(data[i] != payload.at(offset + i)) {
            first = i;
            break;
        }
    }
    std::cerr << "mismatch at " << offset << " first differing byte " << first << " read "
              << QByteArray(data + std::max<qsizetype>(0, first), 16).toHex().constData() << " expected "
              << payload.mid(offset + std::max<qsizetype>(0, first), 16).toHex().constData() << '\n';
    return false;
}

//! Runs the decoder-side scenario on a worker thread while the main thread pumps Qt events.
std::string runScenario(const QByteArray& payload, quint16 port, bool expectRangeSupport,
                        std::shared_ptr<QNetworkAccessManager> network, std::atomic<int>* requests,
                        std::atomic<qint64>* served, std::shared_ptr<Fooyin::Subsonic::SubsonicBufferMonitor> monitor,
                        const std::shared_ptr<Fooyin::Subsonic::SessionCache>& cache)
{
    std::string result;
    std::atomic_bool done{false};

    std::thread worker([&] {
        QUrl url{QStringLiteral("http://127.0.0.1:%1/rest/stream").arg(port)};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("id"), QStringLiteral("test-song"));
        url.setQuery(query);

        auto cacheEntry = cache->entry(QStringLiteral("test-song"));
        auto device = std::make_unique<Fooyin::Subsonic::SubsonicStreamDevice>(network, url, true, monitor,
                                                                                cacheEntry);
        if(!device->open(QIODevice::ReadOnly)) {
            result = "open failed: " + device->errorString().toStdString();
            done = true;
            return;
        }
        if(device->size() != payload.size()) {
            result = "wrong size: " + std::to_string(device->size());
            done = true;
            return;
        }
        if(expectRangeSupport && device->isSequential()) {
            result = "range-capable stream reported as sequential";
            done = true;
            return;
        }

        QByteArray buffer(payload.size(), Qt::Uninitialized);

        // Small reads exercise QIODevice's own 16 KB read-ahead buffer, which calls readData ahead
        // of the user-visible position. The device must keep its own offset across that.
        for(int i{0}; i < 4; ++i) {
            if(!readExact(*device, buffer.data() + i * 512, 512)
               || !matches(buffer.constData() + i * 512, i * 512, payload, 512)) {
                result = "small sequential read mismatch";
                done = true;
                return;
            }
        }
        if(!device->seek(0)) {
            result = "seek back to start after small reads failed";
            done = true;
            return;
        }

        // Sequential start.
        if(!readExact(*device, buffer.data(), 512 * 1024)
           || !matches(buffer.constData(), 0, payload, 512 * 1024)) {
            result = "initial sequential read mismatch";
            done = true;
            return;
        }

        // Jump far forward: the old 64 MB window would have discarded the prefix here.
        const qint64 forward = payload.size() * 3 / 4;
        if(!device->seek(forward) || !readExact(*device, buffer.data(), 256 * 1024)
           || !matches(buffer.constData(), forward, payload, 256 * 1024)) {
            result = "forward seek read mismatch";
            done = true;
            return;
        }

        // Seek back into the prefix that was already downloaded before the forward jump.
        const qint64 backward = payload.size() / 128;
        if(!device->seek(backward) || !readExact(*device, buffer.data(), 128 * 1024)
           || !matches(buffer.constData(), backward, payload, 128 * 1024)) {
            result = "backward seek read mismatch";
            done = true;
            return;
        }

        // Seek into the hole left by the forward jump; this must fetch a new range.
        const qint64 hole = payload.size() / 2;
        if(!device->seek(hole) || !readExact(*device, buffer.data(), 128 * 1024)
           || !matches(buffer.constData(), hole, payload, 128 * 1024)) {
            result = "hole seek read mismatch";
            done = true;
            return;
        }

        // Let the background hole filler finish. Reads at arbitrary positions must then be local.
        int stable{0};
        int lastRequests{-1};
        qint64 lastServed{-1};
        for(int i{0}; i < 400 && stable < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            const int nowRequests = requests->load();
            const qint64 nowServed = served->load();
            if(nowRequests == lastRequests && nowServed == lastServed) {
                ++stable;
            }
            else {
                stable       = 0;
                lastRequests = nowRequests;
                lastServed   = nowServed;
            }
        }
        const int requestsBeforeRandomReads = requests->load();

        const qint64 offsets[] = {0, payload.size() - 1, payload.size() / 3, payload.size() / 7,
                                  payload.size() * 2 / 3, 4096, payload.size() / 2, payload.size() - 65536};
        for(const qint64 offset : offsets) {
            const qint64 size = std::min<qint64>(64 * 1024, payload.size() - offset);
            if(!device->seek(offset) || !readExact(*device, buffer.data(), size)
               || !matches(buffer.constData(), offset, payload, size)) {
                result = "post-cache random seek mismatch at " + std::to_string(offset);
                done = true;
                return;
            }
        }

        if(requests->load() != requestsBeforeRandomReads) {
            result = "random reads after the cache filled still requested data";
            done = true;
            return;
        }

        device->close();
        done = true;
    });

    while(!done) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    worker.join();
    QCoreApplication::processEvents();
    return result;
}
std::string runSequentialScenario(const QByteArray& payload, quint16 port,
                                  std::shared_ptr<QNetworkAccessManager> network,
                                  const std::shared_ptr<Fooyin::Subsonic::SessionCache>& cache)
{
    std::string result;
    std::atomic_bool done{false};

    std::thread worker([&] {
        QUrl url{QStringLiteral("http://127.0.0.1:%1/rest/stream").arg(port)};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("id"), QStringLiteral("chunked-song"));
        url.setQuery(query);

        auto cacheEntry = cache->entry(QStringLiteral("chunked-song"));
        auto device = std::make_unique<Fooyin::Subsonic::SubsonicStreamDevice>(network, url, true, nullptr,
                                                                                cacheEntry);
        if(!device->open(QIODevice::ReadOnly)) {
            result = "chunked open failed: " + device->errorString().toStdString();
            done = true;
            return;
        }
        if(device->size() != -1 || !device->isSequential()) {
            result = "chunked stream must be sequential with an unknown size";
            done = true;
            return;
        }

        QByteArray buffer(64 * 1024, Qt::Uninitialized);
        qint64 pos{0};
        while(pos < payload.size()) {
            const qint64 count = std::min<qint64>(buffer.size(), payload.size() - pos);
            if(!readExact(*device, buffer.data(), count)
               || !matches(buffer.constData(), pos, payload, count)) {
                result = "chunked sequential read mismatch at " + std::to_string(pos);
                done = true;
                return;
            }
            pos += count;
        }

        char extra{0};
        if(device->read(&extra, 1) != 0) {
            result = "chunked stream did not report EOF";
            done = true;
            return;
        }
        device->close();
        done = true;
    });

    while(!done) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    worker.join();
    QCoreApplication::processEvents();
    return result;
}

std::string runCacheReuseScenario(const QByteArray& payload, quint16 port,
                                  std::shared_ptr<QNetworkAccessManager> network,
                                  std::atomic<int>* requests,
                                  const std::shared_ptr<Fooyin::Subsonic::SessionCache>& cache)
{
    std::string result;
    std::atomic_bool done{false};

    std::thread worker([&] {
        QUrl url{QStringLiteral("http://127.0.0.1:%1/rest/stream").arg(port)};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("id"), QStringLiteral("reuse-song"));
        url.setQuery(query);

        {
            auto entry  = cache->entry(QStringLiteral("reuse-song"));
            auto device = std::make_unique<Fooyin::Subsonic::SubsonicStreamDevice>(network, url, true, nullptr,
                                                                                    entry);
            if(!device->open(QIODevice::ReadOnly)) {
                result = "reuse first open failed: " + device->errorString().toStdString();
                done = true;
                return;
            }
            QByteArray buffer(payload.size(), Qt::Uninitialized);
            if(!readExact(*device, buffer.data(), payload.size())
               || !matches(buffer.constData(), 0, payload, payload.size())) {
                result = "reuse first read mismatch";
                done = true;
                return;
            }
            device->close();
        }

        const int requestsAfterFill = requests->load();
        {
            auto entry = cache->entry(QStringLiteral("reuse-song"));
            if(!entry->complete()) {
                result = "reuse entry was not complete";
                done = true;
                return;
            }
            auto device = std::make_unique<Fooyin::Subsonic::SubsonicStreamDevice>(network, url, true, nullptr,
                                                                                    entry);
            if(!device->open(QIODevice::ReadOnly)) {
                result = "reuse second open failed: " + device->errorString().toStdString();
                done = true;
                return;
            }
            if(device->isSequential() || device->size() != payload.size()) {
                result = "complete cache entry did not report a seekable known-size source";
                done = true;
                return;
            }
            const qint64 offset = payload.size() / 3;
            QByteArray buffer(128 * 1024, Qt::Uninitialized);
            if(!device->seek(offset) || !readExact(*device, buffer.data(), buffer.size())
               || !matches(buffer.constData(), offset, payload, buffer.size())) {
                result = "reuse second read mismatch";
                done = true;
                return;
            }
            device->close();
        }

        if(requests->load() != requestsAfterFill) {
            result = "reading a complete cache entry still requested data";
        }
        done = true;
    });

    while(!done) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    worker.join();
    QCoreApplication::processEvents();
    return result;
}

std::string runRetryScenario(const QByteArray& payload, quint16 port,
                             std::shared_ptr<QNetworkAccessManager> network, std::atomic<int>* requests,
                             const std::shared_ptr<Fooyin::Subsonic::SessionCache>& cache)
{
    std::string result;
    std::atomic_bool done{false};

    std::thread worker([&] {
        QUrl url{QStringLiteral("http://127.0.0.1:%1/rest/stream").arg(port)};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("id"), QStringLiteral("retry-song"));
        url.setQuery(query);

        auto entry  = cache->entry(QStringLiteral("retry-song"));
        auto device = std::make_unique<Fooyin::Subsonic::SubsonicStreamDevice>(network, url, true, nullptr, entry);
        if(!device->open(QIODevice::ReadOnly)) {
            result = "retry open failed: " + device->errorString().toStdString();
            done = true;
            return;
        }

        QByteArray buffer(payload.size(), Qt::Uninitialized);
        if(!readExact(*device, buffer.data(), payload.size())
           || !matches(buffer.constData(), 0, payload, payload.size())) {
            result = "retry read mismatch";
            done = true;
            return;
        }
        if(requests->load() < 2) {
            result = "dropped connection was not retried";
            done = true;
            return;
        }
        if(!entry->complete()) {
            result = "retry did not complete the cache entry";
            done = true;
            return;
        }
        device->close();
        done = true;
    });

    while(!done) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    worker.join();
    QCoreApplication::processEvents();
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app{argc, argv};
    QLoggingCategory::setFilterRules(QStringLiteral("fy.subsonic.debug=true"));

    {
        Fooyin::Subsonic::SubsonicBufferMonitor monitor;
        monitor.setRanges({Fooyin::Subsonic::BufferRange{0.0, 0.5}, Fooyin::Subsonic::BufferRange{0.75, 1.0}});
        check(!monitor.isComplete(), "a broken cache range must not be complete");
        monitor.setRange(0.0, 1.0);
        check(monitor.isComplete(), "a full range must be complete");
        monitor.reset();
        check(!monitor.isComplete(), "reset clears completion");
    }

    // Large enough to make prefix-trimming bugs obvious, but quick over loopback.
    const QByteArray payload = makePayload(48 * 1024 * 1024);
    QTemporaryDir cacheRoot;
    if(!cacheRoot.isValid()) {
        std::cerr << "FAIL: could not create a temporary cache directory\n";
        return 1;
    }

    {
        RangeServer server{payload, false, true};
        if(!server.listen()) {
            std::cerr << "FAIL: could not start range server\n";
            return 1;
        }
        auto network = std::shared_ptr<QNetworkAccessManager>(new QNetworkAccessManager);
        auto monitor = std::make_shared<Fooyin::Subsonic::SubsonicBufferMonitor>();
        auto cache = std::make_shared<Fooyin::Subsonic::SessionCache>(cacheRoot.path(), 512LL * 1024 * 1024);
        const std::string result = runScenario(payload, server.port(), true, network, &server.requests,
                                               &server.served, monitor, cache);
        check(result.empty(), result.empty() ? "range scenario" : result);
        check(monitor->isComplete(), "range scenario must report the whole track as cached");
        check(cache->entry(QStringLiteral("test-song"))->complete(), "range scenario must complete the cache entry");
        std::cout << "range server requests: " << server.requests.load()
                  << ", bytes served: " << server.served.load() << '\n';
    }

    {
        // A server that ignores Range and always sends the whole file from zero. The device must
        // still play and seek by waiting for the sequential transfer.
        RangeServer server{payload.left(2 * 1024 * 1024), true, true};
        if(!server.listen()) {
            std::cerr << "FAIL: could not start fallback server\n";
            return 1;
        }
        auto network = std::shared_ptr<QNetworkAccessManager>(new QNetworkAccessManager);
        auto monitor = std::make_shared<Fooyin::Subsonic::SubsonicBufferMonitor>();
        auto cache = std::make_shared<Fooyin::Subsonic::SessionCache>(cacheRoot.path(), 512LL * 1024 * 1024);
        const std::string result = runScenario(payload.left(2 * 1024 * 1024), server.port(), false, network,
                                               &server.requests, &server.served, monitor, cache);
        check(result.empty(), result.empty() ? "fallback scenario" : result);
        check(monitor->isComplete(), "fallback scenario must report the whole track as cached");
        check(cache->entry(QStringLiteral("test-song"))->complete(), "fallback scenario must complete the cache entry");
        std::cout << "fallback server requests: " << server.requests.load()
                  << ", bytes served: " << server.served.load() << '\n';
    }

    {
        // A complete cache entry must be served without touching the network again.
        const QByteArray reusePayload = payload.left(4 * 1024 * 1024);
        RangeServer server{reusePayload};
        if(!server.listen()) {
            std::cerr << "FAIL: could not start reuse server\n";
            return 1;
        }
        auto network = std::shared_ptr<QNetworkAccessManager>(new QNetworkAccessManager);
        auto cache = std::make_shared<Fooyin::Subsonic::SessionCache>(cacheRoot.path(), 512LL * 1024 * 1024);
        const std::string result
            = runCacheReuseScenario(reusePayload, server.port(), network, &server.requests, cache);
        check(result.empty(), result.empty() ? "cache reuse scenario" : result);
        std::cout << "reuse server requests: " << server.requests.load() << '\n';
    }

    {
        // A connection drop after the headers must resume from the last cached byte.
        const QByteArray retryPayload = payload.left(2 * 1024 * 1024);
        RangeServer server{retryPayload, false, false, false, 256 * 1024};
        if(!server.listen()) {
            std::cerr << "FAIL: could not start retry server\n";
            return 1;
        }
        auto network = std::shared_ptr<QNetworkAccessManager>(new QNetworkAccessManager);
        auto cache = std::make_shared<Fooyin::Subsonic::SessionCache>(cacheRoot.path(), 512LL * 1024 * 1024);
        const std::string result = runRetryScenario(retryPayload, server.port(), network, &server.requests, cache);
        check(result.empty(), result.empty() ? "retry scenario" : result);
        std::cout << "retry server requests: " << server.requests.load() << '\n';
    }

    {
        // No Content-Length and no ranges: a strict sequential stream. The device must still serve
        // every byte in order and stop at EOF.
        const QByteArray chunkedPayload = payload.left(1024 * 1024);
        RangeServer server{chunkedPayload, false, true, true};
        if(!server.listen()) {
            std::cerr << "FAIL: could not start chunked server\n";
            return 1;
        }
        auto network = std::shared_ptr<QNetworkAccessManager>(new QNetworkAccessManager);
        auto cache = std::make_shared<Fooyin::Subsonic::SessionCache>(cacheRoot.path(), 512LL * 1024 * 1024);
        const std::string result = runSequentialScenario(chunkedPayload, server.port(), network, cache);
        check(result.empty(), result.empty() ? "chunked scenario" : result);
        check(cache->entry(QStringLiteral("chunked-song"))->complete(), "chunked scenario must complete the cache entry");
        std::cout << "chunked server requests: " << server.requests.load()
                  << ", bytes served: " << server.served.load() << '\n';
    }

    if(failures == 0) {
        std::cout << "subsonic_streamdevice: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}
