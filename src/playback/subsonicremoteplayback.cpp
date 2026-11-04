/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicremoteplayback.h"

#include "subsonicbufferbar.h"
#include "subsonicsettings.h"
#include "subsonicstreamcache.h"
#include "subsonicstreamutils.h"

#include <core/engine/audioinput.h>
#include <core/engine/audioloader.h>
#include <core/network/networkaccessmanager.h>
#include <core/network/networkutils.h>
#include <core/network/remoteioservice.h>
#include <core/network/remotesourceprovider.h>
#include <core/network/remotestreamdevice.h>
#include <core/track.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslError>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

using namespace Qt::StringLiterals;

namespace {
constexpr auto OpenHeaderTimeout = std::chrono::seconds{10};
constexpr int MaxStreamRetries = 3;
constexpr int StreamRetryDelayMs = 500;
// A dropped link does not reset the socket, so QNAM keeps the reply "in progress" and a blocked read
// never returns, freezing pause/seek until the network comes back. Time out on inactivity instead.
constexpr int StreamIdleTimeoutMs = 5000;
// FFmpeg probes a few KB backwards after a seek to find the frame boundary. Fetch this much before a
// backward target so that probe hits the cache.
constexpr qint64 SeekBackMargin = 128LL * 1024;
// A forward seek within this distance keeps the running transfer and waits for it to reach the
// target instead of cancelling and re-requesting.
constexpr qint64 CoverWaitGap = 2LL * 1024 * 1024;
qint64 monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Diagnostics: identifies each device instance and request in the debug log.
int nextDeviceId()
{
    static std::atomic<int> next{0};
    return ++next;
}

//! Removes caches left behind by a previous crash; files still in use are recent and kept.
void removeStaleStreamCaches()
{
    const QDir dir{Fooyin::Subsonic::streamCacheDir()};
    if(!dir.exists()) {
        return;
    }
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("stream-*.cache")}, QDir::Files);
    for(const QFileInfo& file : files) {
        if(file.lastModified().toUTC() < cutoff) {
            QFile::remove(file.absoluteFilePath());
        }
    }
}
} // namespace

namespace Fooyin::Subsonic {
namespace {
struct StreamState
{
    QPointer<QNetworkReply> reply;
    QFile cache;
    QString cachePath;
    RangeSet cached;

    // Total size of the track. -1 until the server tells us (or forever for chunked streams).
    qint64 total{-1};
    // Logical read position. QIODevice::pos() is not updated for sequential devices, and the
    // device can be sequential until the headers reveal a size, so keep our own offset.
    qint64 readPos{0};
    // Range requested for the current transfer and its exclusive end (-1 = open ended).
    qint64 requestedStart{-1};
    qint64 activeLimit{-1};
    // Next file offset to write bytes from the current response to. -1 until headers arrive.
    qint64 writePos{-1};
    // File offset of the first byte of the current response.
    qint64 responseStart{-1};
    // Set by seek(): the next read starts a transfer for this position.
    qint64 pendingStart{-1};
    // Set while a decoder read is blocked on an uncached position.
    qint64 waitingPos{-1};
    bool pendingRequest{false};
    // A request has been posted to the network thread but the reply does not exist yet.
    bool requestPosted{false};
    bool rangeCapable{false};
    bool headersProcessed{false};
    //! True when the current response is a full 200 body from offset zero.
    bool fullResponse{false};
    bool streamEnded{false};
    int status{0};
    int attempts{0};
    int requestSequence{0};
    bool nonBlocking{false};
    bool readWouldBlock{false};
    bool aborted{false};
    QString error;
    std::stop_token token;

    std::mutex mutex;
    std::condition_variable_any ready;

    // Seekbar publish throttle. Guarded by `mutex`; the decoder and network threads both publish.
    qint64 lastPublishMs{-1000};
    // Last seek time (steady clock). Background hole filling stays quiet briefly around seeks so
    // FFmpeg's backward probes can hit the pre-roll instead of cancelling a fill per probe.
    qint64 lastSeekMs{-1000};

    [[nodiscard]] bool canSeek() const
    {
        // A known size is enough: ranges are used when available, otherwise a seek waits for the
        // sequential transfer to reach the target. The disk cache serves everything already read.
        return total >= 0;
    }

    [[nodiscard]] bool complete() const
    {
        return total >= 0 && cached.coversAll(total);
    }

    //! True while the current response is expected to reach @p pos. Caller must hold the mutex.
    [[nodiscard]] bool willCover(qint64 pos) const
    {
        if(!reply || aborted || !error.isEmpty()) {
            return false;
        }
        if(!headersProcessed) {
            return requestedStart >= 0 && requestedStart <= pos && (activeLimit < 0 || pos < activeLimit);
        }
        if(writePos < 0 || writePos > pos) {
            return false;
        }
        return activeLimit < 0 || pos < activeLimit;
    }

    //! Derives size/range support/response offset from headers. Caller must hold the mutex.
    void updateHeaders(QNetworkReply* source)
    {
        if(headersProcessed || !source) {
            return;
        }
        headersProcessed = true;
        status           = source->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        qint64 responseOffset = requestedStart >= 0 ? requestedStart : 0;
        bool contentRangeSeen = false;
        const QByteArray contentRange = source->rawHeader("Content-Range");
        if(!contentRange.isEmpty()) {
            const ContentRange range = parseContentRange(contentRange);
            if(range.total >= 0) {
                total = range.total;
            }
            if(range.start >= 0) {
                responseOffset = range.start;
            }
            contentRangeSeen = true;
            rangeCapable     = true;
        }
        if(status == 206) {
            rangeCapable = true;
        }
        if(source->rawHeader("Accept-Ranges").toLower().contains("bytes")) {
            rangeCapable = true;
        }

        if(!contentRangeSeen && status == 200) {
            // The server ignored the Range request and is sending the whole file from zero. Keep all
            // of it in the cache instead of discarding a prefix; the read will wait for its offset.
            fullResponse               = true;
            responseOffset             = 0;
            const qint64 contentLength = source->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            if(contentLength > 0) {
                // A 200 body is the complete representation, so its Content-Length is exact even
                // when an earlier transcoded response carried an estimate.
                total = contentLength;
            }
            activeLimit = total;
        }
        else {
            fullResponse = false;
        }

        if(status >= 400) {
            error = QStringLiteral("HTTP %1").arg(status);
        }
        else if(contentRangeSeen && requestedStart >= 0 && responseOffset > requestedStart) {
            // A range that starts after the requested byte can never satisfy the read.
            error = QStringLiteral("The server returned an unexpected byte range.");
        }

        responseStart = responseOffset;
        writePos      = responseOffset;
        qCDebug(SUBSONIC) << "stream headers: status" << status << "content-range" << contentRange << "total" << total
                          << "rangeCapable" << rangeCapable << "start" << writePos << "limit" << activeLimit;
    }

    //! Drains whatever the reply has buffered into the disk cache. Caller must hold the mutex.
    void appendFromReply(QNetworkReply* source)
    {
        if(!source || source != reply || writePos < 0 || !cache.isOpen() || !error.isEmpty()) {
            return;
        }

        QByteArray chunk = source->readAll();
        if(chunk.isEmpty()) {
            return;
        }
        if(total > 0 && !fullResponse) {
            // A ranged response must stay inside the representation; anything beyond its reported
            // size is not part of the track.
            if(writePos >= total) {
                return;
            }
            const qint64 room = total - writePos;
            if(chunk.size() > room) {
                chunk.truncate(static_cast<qsizetype>(room));
            }
        }
        if(chunk.isEmpty()) {
            return;
        }

        if(!cache.seek(writePos)) {
            error = QStringLiteral("Could not seek the stream cache: %1").arg(cache.errorString());
            return;
        }

        qint64 written{0};
        while(written < chunk.size()) {
            const qint64 count = cache.write(chunk.constData() + written, chunk.size() - written);
            if(count <= 0) {
                error = QStringLiteral("Could not write the stream cache: %1").arg(cache.errorString());
                return;
            }
            written += count;
        }
        cache.flush();

        cached.add(writePos, writePos + written);
        writePos += written;
        if(fullResponse && total > 0 && writePos > total) {
            // The transcoded stream is longer than the server's estimate. Keep the extra bytes and
            // use the real high-water mark as the size instead of truncating playback.
            total       = writePos;
            activeLimit = total;
        }
        attempts = 0; // data is flowing again, so the next drop gets a fresh retry budget
    }
};

class SubsonicStreamDevice : public QIODevice, public RemoteStreamDevice
{
public:
    SubsonicStreamDevice(std::shared_ptr<QNetworkAccessManager> network, QUrl url, bool ignoreSslErrors,
                         std::shared_ptr<SubsonicBufferMonitor> monitor, QObject* parent = nullptr)
        : QIODevice{parent}
        , m_network{std::move(network)}
        , m_url{std::move(url)}
        , m_ignoreSslErrors{ignoreSslErrors}
        , m_monitor{std::move(monitor)}
        , m_state{std::make_shared<StreamState>()}
    { }

    ~SubsonicStreamDevice() override
    {
        SubsonicStreamDevice::close();
    }

    [[nodiscard]] int id() const
    {
        return m_id;
    }

    [[nodiscard]] bool isSequential() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return !m_state->canSeek();
    }

    [[nodiscard]] qint64 size() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->total;
    }

    [[nodiscard]] qint64 bytesAvailable() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->cached.contiguousBytes(m_state->readPos);
    }

    [[nodiscard]] bool readWouldBlock() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->nonBlocking && m_state->readWouldBlock;
    }

    [[nodiscard]] qsizetype bufferedByteCount() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return static_cast<qsizetype>(m_state->cached.contiguousBytes(m_state->readPos));
    }

    void setNonBlockingReadsEnabled(bool enabled) override
    {
        {
            const std::scoped_lock lock{m_state->mutex};
            m_state->nonBlocking = enabled;
            if(!enabled) {
                m_state->readWouldBlock = false;
            }
        }
        m_state->ready.notify_all();
    }

    void setReadCancellationToken(std::stop_token token) override
    {
        {
            const std::scoped_lock lock{m_state->mutex};
            m_state->token = token;
        }
        m_state->ready.notify_all();
    }

    //! Resumes the transfer from @p from after a dropped connection. Called on the decoder thread.
    void scheduleRetry(qint64 from)
    {
        const auto network = m_network;
        const QPointer<SubsonicStreamDevice> device{this};
        QMetaObject::invokeMethod(network.get(), [network, device, from] {
            QTimer::singleShot(StreamRetryDelayMs, network.get(), [device, from] {
                if(device) {
                    device->startTransfer(from, -1, true);
                }
            });
        });
    }

    //! Fills the first hole left behind by a seek so the whole track ends up cached. Network thread.
    //! @p fromRead suppresses fills for a short time after a seek; read-driven fills otherwise
    //! fight FFmpeg's seek probes. Finished transfers always get to schedule a fill.
    void fillNextGap(bool fromRead = false)
    {
        qint64 start{-1};
        qint64 end{-1};
        {
            const std::scoped_lock lock{m_state->mutex};
            if(m_state->aborted || m_state->reply || m_state->requestPosted || m_state->pendingRequest
               || !m_state->error.isEmpty() || m_state->total < 0 || m_state->waitingPos >= 0
               || (fromRead && monotonicMs() - m_state->lastSeekMs < 500)) {
                return;
            }
            const auto gap = m_state->cached.firstGap(m_state->total);
            if(!gap) {
                m_state->streamEnded = true;
                return;
            }
            start = gap->start;
            end   = gap->end;
        }
        startTransfer(start, end, false);
    }

    bool open(OpenMode mode) override
    {
        if(isOpen()) {
            return true;
        }
        if(!m_network || !m_url.isValid() || !(mode & ReadOnly)) {
            setErrorString(QStringLiteral("Invalid Subsonic stream."));
            return false;
        }

        const QString dir = streamCacheDir();
        QDir{}.mkpath(dir);
        removeStaleStreamCaches();
        const QString cachePath
            = dir + u"/stream-%1.cache"_s.arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

        {
            const std::scoped_lock lock{m_state->mutex};
            m_state->cachePath = cachePath;
            m_state->cache.setFileName(cachePath);
            if(!m_state->cache.open(QIODevice::ReadWrite | QIODevice::Truncate | QIODevice::Unbuffered)) {
                setErrorString(QStringLiteral("Could not create the stream cache: %1")
                                   .arg(m_state->cache.errorString()));
                return false;
            }
            m_state->cached.clear();
            m_state->total            = -1;
            m_state->readPos          = 0;
            m_state->requestedStart   = -1;
            m_state->activeLimit      = -1;
            m_state->writePos         = -1;
            m_state->responseStart    = -1;
            m_state->pendingStart     = -1;
            m_state->pendingRequest   = false;
            m_state->requestPosted    = false;
            m_state->rangeCapable     = false;
            m_state->headersProcessed = false;
            m_state->streamEnded      = false;
            m_state->status           = 0;
            m_state->attempts         = 0;
            m_state->aborted          = false;
            m_state->error.clear();
            m_state->reply.clear();
        }

        m_clock.start();
        qCDebug(SUBSONIC) << "stream" << m_id << "open cache" << cachePath;
        startTransfer(0, -1, false);

        // Never block the thread that owns the network manager: its event loop is what delivers the
        // reply. Fooyin opens remote sources from the GUI thread for resolver probes; waiting here
        // deadlocks the whole application and also blocks the playback stream's network callbacks.
        if(QThread::currentThread() == m_network->thread()) {
            if(!QIODevice::open(mode)) {
                setErrorString(QStringLiteral("Could not open the Subsonic stream."));
                close();
                return false;
            }
            qCDebug(SUBSONIC) << "stream" << m_id << "open deferred on the network thread";
            return true;
        }

        // Wait briefly for headers so size()/canSeek() are valid during decoder init.
        QString error;
        qint64 total{-1};
        int status{0};
        bool seekable{false};
        bool aborted{false};
        {
            std::unique_lock lock{m_state->mutex};
            m_state->ready.wait_for(lock, OpenHeaderTimeout, [this] {
                return m_state->headersProcessed || m_state->streamEnded || !m_state->error.isEmpty();
            });
            error    = m_state->error;
            total    = m_state->total;
            status   = m_state->status;
            seekable = m_state->canSeek();
            aborted  = m_state->aborted;
        }

        if(aborted) {
            close();
            return false;
        }
        if(error.isEmpty() && status < 200) {
            error = QStringLiteral("No response from the server.");
        }
        qCDebug(SUBSONIC) << "stream" << m_id << "open result: total" << total << "status" << status << "seekable"
                          << seekable << "error" << error;
        if(!error.isEmpty()) {
            setErrorString(error);
            close();
            return false;
        }
        // Open after the headers so QIODevice sees the correct sequential/seekable mode.
        if(!QIODevice::open(mode)) {
            setErrorString(QStringLiteral("Could not open the Subsonic stream."));
            close();
            return false;
        }
        publishBuffer(true);
        return true;
    }

    void close() override
    {
        if(!m_state->aborted) {
            qCDebug(SUBSONIC) << "stream" << m_id << "close t" << m_clock.elapsed() << "requests"
                              << m_requests.load();
        }

        QPointer<QNetworkReply> reply;
        QString cachePath;
        {
            const std::scoped_lock lock{m_state->mutex};
            if(m_state->aborted && !m_state->reply && !m_state->cache.isOpen()) {
                QIODevice::close();
                return;
            }
            m_state->aborted = true;
            m_state->streamEnded = true;
            ++m_state->requestSequence;
            m_state->requestPosted  = false;
            m_state->pendingRequest = false;
            reply                   = m_state->reply;
            m_state->reply.clear();
            m_state->cache.close();
            cachePath = m_state->cachePath;
            m_state->cachePath.clear();
        }
        m_state->ready.notify_all();

        if(reply) {
            QMetaObject::invokeMethod(m_network.get(), [reply] {
                if(reply) {
                    reply->abort();
                    reply->deleteLater();
                }
            });
        }
        if(!cachePath.isEmpty()) {
            QFile::remove(cachePath);
        }
        QIODevice::close();
    }

    bool seek(qint64 pos) override
    {
        if(pos < 0 || !isOpen()) {
            return false;
        }

        QPointer<QNetworkReply> oldReply;
        {
            const std::scoped_lock lock{m_state->mutex};
            if(m_state->aborted) {
                return false;
            }
            m_state->lastSeekMs = monotonicMs();
            if(m_state->total >= 0 && pos > m_state->total) {
                return false;
            }
            if(m_state->total < 0) {
                // Unknown length: Qt treats this as a sequential device, so a real seek is not
                // possible. Reading already follows our own offset.
                return false;
            }
            if(pos == m_state->readPos) {
                return true;
            }

            // Already on disk: just move. A background transfer may keep running.
            if(m_state->cached.contains(pos) || pos == m_state->total) {
                m_state->pendingRequest = false; // discard a seek that never got read
                m_state->readPos        = pos;
                QIODevice::seek(pos);
                return true;
            }

            // The running request is about to reach the target: let the read wait for it.
            if(m_state->reply && m_state->willCover(pos) && m_state->writePos >= 0
               && pos - m_state->writePos <= CoverWaitGap) {
                m_state->readPos = pos;
                QIODevice::seek(pos);
                return true;
            }

            // Data is elsewhere in the file: stop the old transfer and fetch the target on the next
            // read. FFmpeg often issues several seeks before reading; only the last one costs a request.
            oldReply                = m_state->reply;
            m_state->reply.clear();
            ++m_state->requestSequence;
            m_state->requestPosted    = false;
            m_state->pendingRequest   = true;
            m_state->pendingStart     = pos;
            m_state->requestedStart   = pos;
            m_state->activeLimit      = -1;
            m_state->headersProcessed = false;
            m_state->writePos         = -1;
            m_state->responseStart    = -1;
            m_state->attempts         = 0;
            m_state->streamEnded      = false;
            m_state->error.clear();
            m_state->readPos          = pos;
        }
        m_state->ready.notify_all();

        if(oldReply) {
            QMetaObject::invokeMethod(m_network.get(), [oldReply] {
                if(oldReply) {
                    oldReply->abort();
                    oldReply->deleteLater();
                }
            });
        }

        qCDebug(SUBSONIC) << "stream" << m_id << "seek" << pos << "will fetch on next read" << "t"
                          << m_clock.elapsed();
        QIODevice::seek(pos);
        return true;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if(maxSize <= 0) {
            return 0;
        }
        if(QThread::currentThread() == m_network->thread()) {
            setErrorString(QStringLiteral("Subsonic stream cannot be read from the network thread."));
            return -1;
        }

        std::unique_lock lock{m_state->mutex};
        if(m_state->aborted) {
            return -1;
        }

        if(m_state->pendingRequest) {
            // Use the same pre-roll as a normal read. FFmpeg probes backwards for the frame
            // boundary after a seek; without this margin every probe became a new HTTP request and
            // cancelled the previous one.
            const qint64 target = m_state->pendingStart;
            m_state->pendingRequest = false;
            const qint64 from = planReadStart(target);
            const qint64 to   = planReadEnd(from);
            lock.unlock();
            startTransfer(from, to, false);
            lock.lock();
        }

        QElapsedTimer stall;
        stall.start();
        m_state->readWouldBlock = false;

        while(true) {
            if(m_state->aborted) {
                return -1;
            }

            const qint64 pos      = m_state->readPos;
            const qint64 rangeEnd = m_state->cached.rangeEnd(pos);
            if(rangeEnd > pos) {
                const qint64 count = std::min<qint64>(maxSize, rangeEnd - pos);
                if(!m_state->cache.seek(pos)) {
                    const QString error = m_state->cache.errorString();
                    lock.unlock();
                    setErrorString(QStringLiteral("Could not read the stream cache: %1").arg(error));
                    return -1;
                }
                const qint64 read = m_state->cache.read(data, count);
                if(read <= 0) {
                    const QString error = m_state->cache.errorString();
                    lock.unlock();
                    setErrorString(QStringLiteral("Could not read the stream cache: %1").arg(error));
                    return -1;
                }
                m_state->readWouldBlock = false;
                m_state->readPos += read;
                logReadRate(read);
                lock.unlock();
                // Keep filling holes behind/ahead while the decoder is reading cached data. Without
                // this, a hole left by a seek would only be retried when another transfer ends.
                fillNextGap(true);
                return read;
            }

            if(m_state->total >= 0 && pos >= m_state->total) {
                return 0;
            }
            if(!m_state->error.isEmpty()) {
                const QString error = m_state->error;
                lock.unlock();
                setErrorString(error);
                return -1;
            }
            if(m_state->streamEnded) {
                if(m_state->total < 0 || pos >= m_state->writePos) {
                    return 0;
                }
                const QString error = QStringLiteral("Stream ended before the requested position.");
                lock.unlock();
                setErrorString(error);
                return -1;
            }

            bool start{false};
            if(!m_state->reply && !m_state->requestPosted) {
                start = true;
            }
            else if(m_state->reply && m_state->headersProcessed && !m_state->willCover(pos)) {
                // A transfer for another part of the file is running. Abandon it and fetch this
                // position; everything already written stays cached.
                start = true;
            }

            if(start) {
                const qint64 from = planReadStart(pos);
                const qint64 to   = planReadEnd(from);
                lock.unlock();
                startTransfer(from, to, false);
                lock.lock();
                continue;
            }

            const auto readable = [this, pos] {
                return m_state->pendingRequest || m_state->cached.contains(pos)
                    || (m_state->total >= 0 && pos >= m_state->total) || !m_state->error.isEmpty()
                    || m_state->aborted || m_state->streamEnded;
            };

            m_state->waitingPos = pos;
            if(m_state->nonBlocking) {
                if(!m_state->ready.wait_for(lock, m_state->token, std::chrono::milliseconds{50}, readable)) {
                    m_state->waitingPos = -1;
                    if(m_state->token.stop_requested()) {
                        return -1;
                    }
                    m_state->readWouldBlock = true;
                    const qint64 now        = m_clock.elapsed();
                    if(now - m_lastWouldBlockLog >= 1000) {
                        m_lastWouldBlockLog = now;
                        qCDebug(SUBSONIC) << "stream" << m_id << "read would-block at" << pos << "cached"
                                          << m_state->cached.contiguousBytes(pos) << "t" << now;
                    }
                    return -1;
                }
                m_state->waitingPos = -1;
            }
            else {
                m_state->ready.wait(lock, m_state->token, readable);
                m_state->waitingPos = -1;
                if(m_state->token.stop_requested()) {
                    qCDebug(SUBSONIC) << "stream" << m_id << "read aborted by stop token at" << pos;
                    return -1;
                }
            }

            if(stall.elapsed() >= 200) {
                qCDebug(SUBSONIC) << "stream" << m_id << "read stalled" << stall.elapsed() << "ms at" << pos
                                  << "cached" << m_state->cached.contiguousBytes(pos) << "t" << m_clock.elapsed();
                stall.restart();
            }
        }
    }

    qint64 writeData(const char* /*data*/, qint64 /*maxSize*/) override
    {
        return -1;
    }

private:
    //! Start of the range to fetch for a read at @p pos. Caller must hold the mutex.
    [[nodiscard]] qint64 planReadStart(qint64 pos) const
    {
        qint64 start = pos > SeekBackMargin ? pos - SeekBackMargin : 0;
        const qint64 cachedEnd = m_state->cached.rangeEnd(start);
        if(cachedEnd > start) {
            start = cachedEnd;
        }
        return std::min(start, pos);
    }

    //! Exclusive end of the range to fetch for a read starting at @p start, bounded by cached data.
    [[nodiscard]] qint64 planReadEnd(qint64 start) const
    {
        qint64 end = m_state->total;
        for(const ByteRange& range : m_state->cached.ranges()) {
            if(range.start > start) {
                if(end < 0 || range.start < end) {
                    end = range.start;
                }
                break;
            }
        }
        if(end >= 0 && end <= start) {
            end = m_state->total;
        }
        return end;
    }

    //! Publishes all cached ranges to the seekbar overlay. Bytes approximate VBR positions.
    void publishBuffer(bool force = false)
    {
        if(!m_monitor) {
            return;
        }

        QVector<BufferRange> ranges;
        {
            const std::scoped_lock lock{m_state->mutex};
            const qint64 total = m_state->total;
            if(total <= 0) {
                return;
            }

            const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
            if(!force && now - m_state->lastPublishMs < 100) {
                return;
            }
            m_state->lastPublishMs = now;

            for(const ByteRange& range : m_state->cached.ranges()) {
                const double start = std::clamp(static_cast<double>(range.start) / static_cast<double>(total), 0.0, 1.0);
                const double end   = std::clamp(static_cast<double>(range.end) / static_cast<double>(total), 0.0, 1.0);
                if(end > start) {
                    ranges.append(BufferRange{start, end});
                }
            }
        }

        const auto monitor = m_monitor;
        QMetaObject::invokeMethod(
            monitor.get(), [monitor, ranges] { monitor->setRanges(ranges); }, Qt::QueuedConnection);
    }

    //! One line per second while streaming: shows whether decoding continues (e.g. while paused).
    void logReadRate(qint64 count)
    {
        m_bytesRead += count;
        const qint64 now = m_clock.elapsed();
        if(now - m_lastRateLog < 1000) {
            return;
        }
        qCDebug(SUBSONIC) << "stream" << m_id << "read rate" << m_bytesRead << "B/s at" << m_state->readPos
                          << "cached" << m_state->cached.contiguousBytes(m_state->readPos) << "t" << now;
        m_bytesRead   = 0;
        m_lastRateLog = now;
    }

    //! Starts a ranged transfer. @p end is exclusive; -1 means to the end of the file.
    void startTransfer(qint64 start, qint64 end, bool retry)
    {
        const auto state = m_state;
        QPointer<QNetworkReply> oldReply;
        int sequence{0};
        {
            const std::scoped_lock lock{m_state->mutex};
            if(m_state->aborted || !m_state->cache.isOpen()) {
                return;
            }
            if(start < 0) {
                start = 0;
            }

            // Never re-download a cached prefix.
            const qint64 cachedEnd = m_state->cached.rangeEnd(start);
            if(cachedEnd > start) {
                start = cachedEnd;
            }
            if(m_state->total >= 0 && start >= m_state->total) {
                m_state->streamEnded = true;
                return;
            }

            if(end < 0 || (m_state->total >= 0 && end > m_state->total)) {
                end = m_state->total;
            }
            if(end >= 0 && end <= start) {
                end = m_state->total;
            }
            if(end < 0) {
                // Unknown total: bound the transfer at the next cached range so we never fetch the
                // same tail twice after a seek.
                for(const ByteRange& range : m_state->cached.ranges()) {
                    if(range.start > start) {
                        end = range.start;
                        break;
                    }
                }
            }

            m_state->requestedStart   = start;
            m_state->activeLimit      = end;
            m_state->headersProcessed = false;
            m_state->fullResponse     = false;
            m_state->status           = 0;
            m_state->writePos         = -1;
            m_state->responseStart    = -1;
            m_state->streamEnded      = false;
            m_state->pendingRequest   = false;
            m_state->requestPosted    = true;
            sequence                  = ++m_state->requestSequence;
            oldReply                  = m_state->reply;
            m_state->reply.clear();
            if(!retry) {
                m_state->attempts = 0;
            }
            m_state->error.clear();
        }

        if(oldReply) {
            QMetaObject::invokeMethod(m_network.get(), [oldReply] {
                if(oldReply) {
                    oldReply->abort();
                    oldReply->deleteLater();
                }
            });
        }

        const QUrl url               = m_url;
        const bool ignoreSsl         = m_ignoreSslErrors;
        const auto network           = m_network;
        const QPointer<SubsonicStreamDevice> device{this};
        const int id                 = m_id;
        const int requestNo          = m_requests.fetch_add(1) + 1;
        const qint64 openedAt        = m_clock.elapsed();
        qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "range" << start << '-' << end << "t"
                          << openedAt;

        QMetaObject::invokeMethod(network.get(), [network, url, ignoreSsl, state, start, end, sequence, device, id,
                                                  requestNo, openedAt] {
            {
                const std::scoped_lock lock{state->mutex};
                if(state->aborted || state->requestSequence != sequence) {
                    return;
                }
            }

            QNetworkRequest request = makeNetworkRequest(
                url, NetworkRequestOption::AlwaysNetwork | NetworkRequestOption::AcceptAny);
            request.setTransferTimeout(StreamIdleTimeoutMs);
            request.setPriority(QNetworkRequest::HighPriority);
            QByteArray range = QByteArrayLiteral("bytes=") + QByteArray::number(start) + '-';
            if(end >= 0) {
                range += QByteArray::number(end - 1);
            }
            request.setRawHeader("Range", range);

            QNetworkReply* reply = network->get(request);
            reply->setReadBufferSize(0); // drain straight to the cache; no in-memory window

            if(ignoreSsl) {
                QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                                 [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
            }

            QObject::connect(reply, &QIODevice::readyRead, reply, [device, state, reply] {
                {
                    const std::scoped_lock lock{state->mutex};
                    if(reply == state->reply && !state->aborted) {
                        state->updateHeaders(reply);
                        state->appendFromReply(reply);
                    }
                }
                state->ready.notify_all();
                if(device) {
                    device->publishBuffer();
                }
            });

            QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [state, reply] {
                {
                    const std::scoped_lock lock{state->mutex};
                    if(reply == state->reply && !state->aborted) {
                        state->updateHeaders(reply);
                    }
                }
                state->ready.notify_all();
            });

            QObject::connect(reply, &QNetworkReply::finished, reply, [device, state, reply, id, requestNo] {
                qint64 retryFrom{-1};
                bool fillNext{false};
                {
                    const std::scoped_lock lock{state->mutex};
                    if(reply == state->reply) {
                        state->updateHeaders(reply);
                        state->appendFromReply(reply);
                        state->reply.clear();
                        state->requestPosted = false;

                        if(!state->aborted) {
                            const bool httpError = state->status >= 400;
                            const bool hasError
                                = httpError || reply->error() != QNetworkReply::NoError || !state->error.isEmpty();

                            if(hasError && state->attempts < MaxStreamRetries) {
                                ++state->attempts;
                                retryFrom = state->writePos >= 0 ? state->writePos : state->requestedStart;
                                qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "failed:"
                                                  << reply->errorString() << "retry" << state->attempts << "from"
                                                  << retryFrom;
                                state->error.clear(); // the retry replaces the failed transfer
                                state->headersProcessed = false;
                                state->writePos         = -1;
                            }
                            else if(hasError) {
                                if(state->error.isEmpty()) {
                                    state->error = reply->errorString();
                                }
                                qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "gave up:"
                                                  << state->error;
                            }
                            else if(state->total >= 0) {
                                if(state->complete()) {
                                    state->streamEnded = true;
                                }
                                else if(state->writePos <= state->responseStart) {
                                    // A finished response that delivered no bytes would otherwise make
                                    // the hole filler request the same range forever.
                                    state->error = QStringLiteral("The server sent an empty stream range.");
                                }
                                else {
                                    fillNext = true;
                                }
                            }
                            else {
                                // Chunked/transcoded response: EOF is the end of the stream.
                                state->streamEnded = true;
                            }
                        }
                    }
                }
                state->ready.notify_all();
                if(device) {
                    device->publishBuffer(true);
                }
                reply->deleteLater();

                if(retryFrom >= 0 && device) {
                    device->scheduleRetry(retryFrom);
                }
                else if(fillNext && device) {
                    device->fillNextGap();
                }
            });

            QObject::connect(reply, &QNetworkReply::errorOccurred, reply,
                             [state, reply, id, requestNo](QNetworkReply::NetworkError error) {
                                 // Only logged: latching here would race the retry path in finished().
                                 qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "error"
                                                   << static_cast<int>(error) << reply->errorString();
                                 state->ready.notify_all();
                             });

            {
                const std::scoped_lock lock{state->mutex};
                if(state->aborted || state->requestSequence != sequence) {
                    reply->abort();
                    reply->deleteLater();
                    return;
                }
                state->reply = reply;
                state->requestPosted = false;
            }
            state->ready.notify_all();
        });
    }

    std::shared_ptr<QNetworkAccessManager> m_network;
    QUrl m_url;
    bool m_ignoreSslErrors;
    std::shared_ptr<SubsonicBufferMonitor> m_monitor;
    std::shared_ptr<StreamState> m_state;
    int m_id{nextDeviceId()};
    std::atomic<int> m_requests{0}; // touched from both the decoder and the network thread
    QElapsedTimer m_clock;
    qint64 m_bytesRead{0};
    qint64 m_lastRateLog{-1000};
    qint64 m_lastWouldBlockLog{-1000};
};

class SubsonicRemoteSourceProvider : public RemoteSourceProvider
{
public:
    SubsonicRemoteSourceProvider(std::shared_ptr<NetworkAccessManager> network,
                                 std::shared_ptr<RemoteIoService> fallback, SettingsManager* settings,
                                 std::shared_ptr<SubsonicBufferMonitor> monitor)
        : m_network{std::move(network)}
        , m_fallback{std::move(fallback)}
        , m_settings{settings}
        , m_monitor{std::move(monitor)}
    { }

    [[nodiscard]] RemoteStreamSource createStreamSource(const QUrl& url) const override
    {
        if(!isSubsonicStreamUrl(url) || !m_network) {
            return m_fallback->createStreamSource(url);
        }

        // A fully prefetched next track is served from disk, so it starts without buffering.
        if(const QString songId = QUrlQuery{url}.queryItemValue(u"id"_s); !songId.isEmpty()) {
            const QString localPath = prefetchPath(songId);
            if(QFileInfo::exists(localPath)) {
                auto file = std::make_unique<QFile>(localPath);
                if(file->open(QIODevice::ReadOnly)) {
                    if(m_monitor) {
                        const auto monitor = m_monitor;
                        QMetaObject::invokeMethod(
                            monitor.get(), [monitor] { monitor->setRange(0.0, 1.0); }, Qt::QueuedConnection);
                    }
                    RemoteStreamSource source;
                    source.device      = std::move(file);
                    source.openTimeout = std::chrono::milliseconds{0};
                    return source;
                }
            }
        }

        const auto ignoreSsl
            = m_settings && m_settings->value(QStringLiteral("Subsonic/IgnoreSslErrors")).toBool();

        RemoteStreamSource source;
        auto device = std::make_unique<SubsonicStreamDevice>(m_network, url, ignoreSsl, m_monitor);
        source.remoteDevice = device.get();
        source.device       = std::move(device);
        source.openTimeout  = std::chrono::milliseconds{8000};
        return source;
    }

private:
    std::shared_ptr<NetworkAccessManager> m_network;
    std::shared_ptr<RemoteIoService> m_fallback;
    SettingsManager* m_settings;
    std::shared_ptr<SubsonicBufferMonitor> m_monitor;
};

class SubsonicDecoder : public AudioDecoder
{
public:
    explicit SubsonicDecoder(DecoderCreator innerCreator)
        : m_innerCreator{std::move(innerCreator)}
    { }

    [[nodiscard]] QStringList extensions() const override
    {
        return {};
    }

    [[nodiscard]] QStringList supportedSchemes() const override
    {
        return {};
    }

    [[nodiscard]] bool supportsRemoteSources() const override
    {
        return true;
    }

    [[nodiscard]] bool isSeekable() const override
    {
        return m_seekable;
    }

    std::optional<AudioFormat> init(const AudioSource& source, const Track& track, DecoderOptions options) override
    {
        m_inner.reset();
        m_seekable = false;

        if(!source.device || !isSubsonicStreamUrl(QUrl{source.filepath}) || !m_innerCreator) {
            return {};
        }

        m_inner = m_innerCreator();
        if(!m_inner) {
            return {};
        }

        m_inner->setPlaybackHints(playbackHints());

        auto* streamDevice = dynamic_cast<SubsonicStreamDevice*>(source.device);
        if(streamDevice) {
            streamDevice->setReadCancellationToken(abortToken());
        }

        AudioSource innerSource = source;
        innerSource.filepath.clear(); // let FFmpeg probe the content, never re-open the URL

        Track innerTrack = track;
        innerTrack.setFilePath(QStringLiteral("/foonic/subsonic-stream"));

        const auto format = m_inner->init(innerSource, innerTrack, options);
        if(!format) {
            qCWarning(SUBSONIC) << "decoder init failed on stream" << (streamDevice ? streamDevice->id() : -1);
            m_inner.reset();
            return {};
        }
        qCDebug(SUBSONIC) << "decoder init: stream" << (streamDevice ? streamDevice->id() : -1) << "innerSeekable"
                          << m_inner->isSeekable() << "concurrent" << m_inner->allowsConcurrentDecoding()
                          << "prebufferMs" << m_inner->playbackPrebufferMs();

        m_seekable = m_inner->isSeekable() && source.device && !source.device->isSequential();
        return format;
    }

    void start() override
    {
        if(m_inner) {
            m_inner->start();
        }
    }

    void stop() override
    {
        qCDebug(SUBSONIC) << "decoder stop";
        if(m_inner) {
            m_inner->stop();
        }
    }

    void seek(uint64_t pos) override
    {
        qCDebug(SUBSONIC) << "decoder seek" << pos;
        if(m_inner) {
            m_inner->seek(pos);
        }
    }

    [[nodiscard]] AudioBuffer readBuffer(size_t bytes) override
    {
        return m_inner ? m_inner->readBuffer(bytes) : AudioBuffer{};
    }

    [[nodiscard]] ReadResult readAudio(size_t bytes) override
    {
        return m_inner ? m_inner->readAudio(bytes) : ReadResult::endOfStream();
    }

    [[nodiscard]] bool allowsConcurrentDecoding() const override
    {
        return m_inner ? m_inner->allowsConcurrentDecoding() : false;
    }

    [[nodiscard]] int playbackPrebufferMs() const override
    {
        return m_inner ? m_inner->playbackPrebufferMs() : 0;
    }

    [[nodiscard]] QStringList takeWarnings() override
    {
        return m_inner ? m_inner->takeWarnings() : QStringList{};
    }

    [[nodiscard]] bool trackHasChanged() const override
    {
        return m_inner && m_inner->trackHasChanged();
    }

    [[nodiscard]] Track changedTrack() const override
    {
        return m_inner ? m_inner->changedTrack() : Track{};
    }

    [[nodiscard]] std::optional<TimedTrackChange> takeTimedTrackChange() override
    {
        return m_inner ? m_inner->takeTimedTrackChange() : std::nullopt;
    }

    [[nodiscard]] int bitrate() const override
    {
        return m_inner ? m_inner->bitrate() : 0;
    }

    [[nodiscard]] RepeatHandling repeatHandling() const override
    {
        return m_inner ? m_inner->repeatHandling() : RepeatHandling::EngineTransition;
    }

protected:
    void interruptRead() override
    {
        qCDebug(SUBSONIC) << "decoder interrupt read";
        if(m_inner) {
            m_inner->requestAbort();
        }
    }

    void playbackHintsChanged(PlaybackHints hints) override
    {
        if(m_inner) {
            m_inner->setPlaybackHints(hints);
        }
    }

private:
    DecoderCreator m_innerCreator;
    std::unique_ptr<AudioDecoder> m_inner;
    bool m_seekable{false};
};
} // namespace

std::shared_ptr<SubsonicBufferMonitor> registerSubsonicRemotePlayback(
    const std::shared_ptr<AudioLoader>& loader, std::shared_ptr<NetworkAccessManager> network,
    SettingsManager* settings)
{
    auto monitor = std::make_shared<SubsonicBufferMonitor>();
    if(!loader || !network) {
        return monitor;
    }

    auto fallback = std::make_shared<RemoteIoService>(network, settings);
    loader->setRemoteSourceProvider(std::make_shared<SubsonicRemoteSourceProvider>(network, std::move(fallback),
                                                                                   settings, monitor));

    DecoderCreator ffmpeg;
    for(const auto& entry : loader->decoders()) {
        if(entry.name.compare(u"FFmpeg"_s, Qt::CaseInsensitive) == 0) {
            ffmpeg = entry.creator;
            break;
        }
    }
    if(!ffmpeg) {
        qCWarning(SUBSONIC) << "FFmpeg decoder not found; Subsonic remote seeking is unavailable";
        return monitor;
    }

    loader->addDecoder(u"Foonic Remote"_s, [ffmpeg] { return std::make_unique<SubsonicDecoder>(ffmpeg); }, 0);

    // fooyin restores the decoder order from saved settings after plugins initialise, which can
    // shuffle us behind FFmpeg. Put ourselves back in front once the event loop is running.
    QTimer::singleShot(0, [loader] { loader->changeDecoderIndex(u"Foonic Remote"_s, 0); });
    return monitor;
}
} // namespace Fooyin::Subsonic
