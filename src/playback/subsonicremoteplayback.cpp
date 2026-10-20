/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicremoteplayback.h"

#include "subsonicsettings.h"
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
#include <QElapsedTimer>
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
constexpr qint64 DefaultBufferBytes = 64LL * 1024 * 1024;
constexpr int MaxStreamRetries = 3;
constexpr int StreamRetryDelayMs = 500;
// A dropped link does not reset the socket, so QNAM keeps the reply "in progress" and a blocked read
// never returns, freezing pause/seek until the network comes back. Time out on inactivity instead.
constexpr int StreamIdleTimeoutMs = 5000;
// FFmpeg probes a few KB backwards after a seek to find the frame boundary. Fetch this much before a
// backward target so that probe hits memory, but keep it small: the pre-roll has to download before the
// target byte is readable, and a megabyte-scale margin made every seek wait for it.
constexpr qint64 SeekBackMargin = 128LL * 1024;
// A forward seek within this distance keeps the buffer and waits for the running download to reach the
// target instead of re-requesting: FFmpeg jumps to the end of the file before bisecting, and discarding
// the buffer there threw away tens of megabytes on every seek.
constexpr qint64 ForwardWaitGap = 32LL * 1024 * 1024;
// Diagnostics: identifies each device instance and request in the debug log.
int nextDeviceId()
{
    static std::atomic<int> next{0};
    return ++next;
}
} // namespace

namespace Fooyin::Subsonic {
namespace {
struct StreamState
{
    QPointer<QNetworkReply> reply;
    QByteArray buffer;
    qsizetype readPos{0};
    qint64 bufferBase{0}; // file offset of buffer[0]
    qint64 maxBytes{DefaultBufferBytes};
    qint64 total{-1};
    qint64 skip{0};
    qint64 requestedStart{0};
    qint64 skipTo{-1}; // file offset to wait for on a forward seek, -1 = none
    bool rangeCapable{false};
    bool headersProcessed{false};
    int status{0}; // HTTP status of the current reply, 0 = none
    bool pendingRequest{false};
    int attempts{0};
    bool nonBlocking{false};
    bool readWouldBlock{false};
    bool finished{false};
    bool aborted{false};
    QString error;
    std::stop_token token;
    std::mutex mutex;
    std::condition_variable_any ready;

    [[nodiscard]] bool canSeek() const
    {
        return total >= 0 && rangeCapable;
    }

    [[nodiscard]] qsizetype buffered() const
    {
        return std::max<qsizetype>(0, buffer.size() - readPos);
    }

    //! Reads whatever the reply has buffered. Caller must hold the mutex.
    void appendFromReply(QNetworkReply* source)
    {
        if(!source->isOpen()) {
            return;
        }

        // Retain consumed history so backward seeks stay in memory; only drop it once the buffer is full.
        if(buffer.size() >= maxBytes && readPos > 0) {
            bufferBase += readPos;
            buffer.remove(0, readPos);
            readPos = 0;
        }

        const qint64 space = maxBytes - buffer.size();
        if(space <= 0 && skip <= 0) {
            return;
        }

        QByteArray chunk = source->read(space + skip);
        if(chunk.isEmpty()) {
            return;
        }

        if(skip > 0) {
            const qsizetype drop = static_cast<qsizetype>(std::min<qint64>(skip, chunk.size()));
            chunk.remove(0, drop);
            skip -= drop;
        }

        if(!chunk.isEmpty() && space > 0) {
            buffer.append(chunk.left(space));
            attempts = 0; // data is flowing again, so the next drop gets a fresh retry budget
        }
    }

    //! Derives size/range support/skip offset from response headers. Caller must hold the mutex.
    void updateHeaders(QNetworkReply* source)
    {
        if(headersProcessed) {
            return;
        }
        headersProcessed = true;
        status           = source->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        const QByteArray contentRange = source->rawHeader("Content-Range");
        if(!contentRange.isEmpty()) {
            const ContentRange range = parseContentRange(contentRange);
            if(range.total >= 0) {
                total = range.total;
            }
            if(range.start >= 0) {
                skip = std::max<qint64>(0, requestedStart - range.start);
            }
            rangeCapable = true;
            qCDebug(SUBSONIC) << "stream headers: status" << status << "content-range" << contentRange << "total"
                              << total << "skip" << skip;
            return;
        }

        const qint64 contentLength = source->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if(contentLength > 0 && total < 0) {
            total = contentLength;
        }
        if(source->rawHeader("Accept-Ranges").toLower().contains("bytes")) {
            rangeCapable = true;
        }
        // The server ignored our Range request and is sending from 0 again.
        if(status == 200 && requestedStart > 0) {
            skip = requestedStart;
        }
        qCDebug(SUBSONIC) << "stream headers: status" << status << "total" << total << "rangeCapable" << rangeCapable
                          << "skip" << skip;
    }
};

class SubsonicStreamDevice : public QIODevice, public RemoteStreamDevice
{
public:
    SubsonicStreamDevice(std::shared_ptr<NetworkAccessManager> network, QUrl url, bool ignoreSslErrors,
                         qsizetype bufferBytes, QObject* parent = nullptr)
        : QIODevice{parent}
        , m_network{std::move(network)}
        , m_url{std::move(url)}
        , m_ignoreSslErrors{ignoreSslErrors}
        , m_bufferBytes{bufferBytes}
        , m_state{std::make_shared<StreamState>()}
    {
        m_state->maxBytes = bufferBytes;
    }

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
        return !canSeek();
    }

    [[nodiscard]] qint64 size() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->total;
    }

    [[nodiscard]] qint64 bytesAvailable() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->buffered();
    }

    [[nodiscard]] bool canSeek() const
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->canSeek();
    }

    [[nodiscard]] bool readWouldBlock() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->nonBlocking && m_state->readWouldBlock;
    }

    [[nodiscard]] qsizetype bufferedByteCount() const override
    {
        const std::scoped_lock lock{m_state->mutex};
        return m_state->buffered();
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
        // The device lives on the decoder thread, which has no event loop, so a timer bound to it never
        // fires. Run the delay on the network thread (which does) and keep a QPointer to the device.
        const auto network = m_network;
        const QPointer<SubsonicStreamDevice> device{this};
        QMetaObject::invokeMethod(network.get(), [network, device, from] {
            QTimer::singleShot(StreamRetryDelayMs, network.get(), [device, from] {
                if(device) {
                    device->startRequest(from);
                }
            });
        });
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

        {
            const std::scoped_lock lock{m_state->mutex};
            m_state->buffer.clear();
            m_state->readPos          = 0;
            m_state->bufferBase       = 0;
            m_state->total            = -1;
            m_state->skip             = 0;
            m_state->requestedStart   = 0;
            m_state->rangeCapable     = false;
            m_state->headersProcessed = false;
            m_state->status           = 0;
            m_state->pendingRequest   = false;
            m_state->skipTo          = -1;
            m_state->finished         = false;
            m_state->aborted          = false;
            m_state->error.clear();
            m_state->reply.clear();
        }

        QIODevice::open(mode);
        m_clock.start();
        qCDebug(SUBSONIC) << "stream" << m_id << "open";
        startRequest(0);

        // Wait briefly for headers so size()/canSeek() are valid during decoder init.
        QString error;
        qint64 total{-1};
        int status{0};
        bool seekable{false};
        {
            std::unique_lock lock{m_state->mutex};
            m_state->ready.wait_for(lock, OpenHeaderTimeout, [this] {
                return m_state->headersProcessed || m_state->finished || !m_state->error.isEmpty();
            });
            error    = m_state->error;
            total    = m_state->total;
            status   = m_state->status;
            seekable = m_state->canSeek();
        }

        if(error.isEmpty() && status < 200) {
            error = QStringLiteral("No response from the server.");
        }
        qCDebug(SUBSONIC) << "stream" << m_id << "open result: total" << total << "status" << status << "seekable"
                          << seekable << "error" << error;
        if(!error.isEmpty()) {
            // Reporting an empty stream as "opened" made the player treat the track as a live stream and
            // grey out the seekbar instead of failing it.
            setErrorString(error);
            close();
            return false;
        }
        return true;
    }

    void close() override
    {
        if(!m_state->aborted) {
            qCDebug(SUBSONIC) << "stream" << m_id << "close t" << m_clock.elapsed() << "requests"
                              << m_requests.load();
        }
        QPointer<QNetworkReply> reply;
        {
            const std::scoped_lock lock{m_state->mutex};
            if(m_state->aborted && !m_state->reply) {
                QIODevice::close();
                return;
            }
            m_state->aborted  = true;
            m_state->finished = true;
            reply             = m_state->reply;
            m_state->reply.clear();
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
        QIODevice::close();
    }

    bool seek(qint64 pos) override
    {
        if(pos < 0 || !isOpen()) {
            return false;
        }
        if(pos == QIODevice::pos()) {
            return true;
        }

        bool handled{false};
        qint64 bufferBase{0};
        qsizetype bufferBytes{0};
        qint64 total{-1};
        qint64 skipTo{-1};
        qsizetype readPos{0};
        bool finished{false};
        bool pendingRequest{false};
        bool nonBlocking{false};
        {
            const std::scoped_lock lock{m_state->mutex};
            if(m_state->aborted) {
                return false;
            }
            bufferBase       = m_state->bufferBase;
            bufferBytes      = m_state->buffer.size();
            const qint64 end = bufferBase + bufferBytes;

            if(pos >= bufferBase && pos <= end) {
                // Already downloaded: just move the cursor.
                m_state->readPos = static_cast<qsizetype>(pos - bufferBase);
                m_state->skipTo  = -1;
                handled          = true;
            }
            else if(pos > end && pos - end <= ForwardWaitGap && m_state->reply && !m_state->finished
                    && m_state->error.isEmpty()) {
                // Still downloading forward: wait for it to cover `pos` instead of dropping the buffer
                // (which would also lose everything already downloaded behind us).
                m_state->skipTo = pos;
                handled         = true;
            }
            total          = m_state->total;
            skipTo         = m_state->skipTo;
            readPos        = m_state->readPos;
            finished       = m_state->finished;
            pendingRequest = m_state->pendingRequest;
            nonBlocking    = m_state->nonBlocking;
        }
        qCDebug(SUBSONIC) << "stream" << m_id << "seek" << pos << (handled ? "buffered/wait" : "refetch")
                          << "bufferBase" << bufferBase << "bufferBytes" << bufferBytes << "readPos" << readPos
                          << "total" << total << "skipTo" << skipTo << "finished" << finished << "pending"
                          << pendingRequest << "nonBlocking" << nonBlocking << "t" << m_clock.elapsed();
        if(handled) {
            return QIODevice::seek(pos);
        }

        QPointer<QNetworkReply> oldReply;
        {
            const std::scoped_lock lock{m_state->mutex};
            if(m_state->aborted) {
                return false;
            }

            // Backward seek (target below what we've downloaded): fetch a margin behind the target so
            // FFmpeg's backwards probe seeks are served from memory. Forward seek: start exactly at the
            // target so the first decoded frame arrives without downloading a useless prefix.
            const qint64 requestedStart = pos < bufferBase ? std::max<qint64>(0, pos - SeekBackMargin) : pos;
            const qsizetype targetPos  = static_cast<qsizetype>(pos - requestedStart);

            oldReply                  = m_state->reply;
            m_state->reply.clear();
            m_state->buffer.clear();
            m_state->readPos          = targetPos;
            m_state->bufferBase       = requestedStart;
            m_state->skip             = 0;
            m_state->requestedStart   = requestedStart;
            m_state->headersProcessed = false;
            m_state->status           = 0;
            m_state->pendingRequest   = true;
            m_state->skipTo           = -1;
            m_state->finished         = false;
            m_state->error.clear();
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

        // Don't start the request here: FFmpeg may issue several seeks before reading, and the backward
        // margin above collapses its backwards probes into memory. The request starts on the next read.
        return QIODevice::seek(pos);
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
        if(m_state->pendingRequest) {
            m_state->pendingRequest = false;
            const qint64 start      = m_state->requestedStart;
            lock.unlock();
            startRequest(start);
            lock.lock();
        }

        const std::stop_token token = m_state->token;
        const auto readable         = [this] {
            if(m_state->skipTo >= 0) {
                return m_state->bufferBase + m_state->buffer.size() >= m_state->skipTo || m_state->finished
                    || m_state->aborted || !m_state->error.isEmpty();
            }
            return m_state->buffered() > 0 || m_state->finished || m_state->aborted || !m_state->error.isEmpty();
        };
        QElapsedTimer stall;
        stall.start();
        bool ready{true};
        if(m_state->nonBlocking) {
            ready = m_state->ready.wait_for(lock, token, std::chrono::milliseconds{50}, readable);
        }
        else {
            m_state->ready.wait(lock, token, readable);
        }
        m_state->readWouldBlock = false;
        if(stall.elapsed() >= 200) {
            qCDebug(SUBSONIC) << "stream" << m_id << "read stalled" << stall.elapsed() << "ms at" << QIODevice::pos()
                              << "buffered" << m_state->buffered() << "base" << m_state->bufferBase << "skipTo"
                              << m_state->skipTo << "finished" << m_state->finished << "t" << m_clock.elapsed();
        }

        if(token.stop_requested()) {
            qCDebug(SUBSONIC) << "stream" << m_id << "read aborted by stop token at" << QIODevice::pos();
            return -1;
        }
        if(!ready) {
            m_state->readWouldBlock = true;
            const qint64 now         = m_clock.elapsed();
            if(now - m_lastWouldBlockLog >= 1000) {
                m_lastWouldBlockLog = now;
                qCDebug(SUBSONIC) << "stream" << m_id << "read would-block at" << QIODevice::pos() << "buffered"
                                  << m_state->buffered() << "t" << now;
            }
            return -1;
        }
        if(m_state->skipTo >= 0) {
            const qint64 end = m_state->bufferBase + m_state->buffer.size();
            if(end < m_state->skipTo) {
                // The transfer ended before reaching the seek target: fail rather than serve the wrong
                // bytes from the old position.
                const qint64 target = m_state->skipTo;
                m_state->skipTo     = -1;
                lock.unlock();
                qCWarning(SUBSONIC) << "stream" << m_id << "ended" << target - end << "bytes short of seek target"
                                    << target;
                setErrorString(QStringLiteral("Stream ended before the seek target."));
                return -1;
            }
            m_state->readPos = static_cast<qsizetype>(m_state->skipTo - m_state->bufferBase);
            m_state->skipTo  = -1;
        }
        if(!m_state->error.isEmpty()) {
            const QString error = m_state->error;
            lock.unlock();
            setErrorString(error);
            return -1;
        }
        if(m_state->buffered() <= 0) {
            return 0;
        }

        const qsizetype count = std::min<qsizetype>(m_state->buffered(), maxSize);
        std::memcpy(data, m_state->buffer.constData() + m_state->readPos, count);
        m_state->readPos += count;
        logReadRate(count);

        const QPointer<QNetworkReply> reply = m_state->reply;
        lock.unlock();

        // Pull data QNAM buffered but hasn't signalled yet.
        if(reply) {
            QMetaObject::invokeMethod(
                m_network.get(),
                [state = m_state, reply] {
                    if(!reply) {
                        return;
                    }
                    {
                        const std::scoped_lock appendLock{state->mutex};
                        if(reply == state->reply && !state->aborted) {
                            state->appendFromReply(reply);
                        }
                    }
                    state->ready.notify_all();
                },
                Qt::QueuedConnection);
        }

        return count;
    }

    qint64 writeData(const char* /*data*/, qint64 /*maxSize*/) override
    {
        return -1;
    }

private:
    //! One line per second while streaming: shows whether decoding continues (e.g. while paused).
    void logReadRate(qsizetype count)
    {
        m_bytesRead += count;
        const qint64 now = m_clock.elapsed();
        if(now - m_lastRateLog < 1000) {
            return;
        }
        qCDebug(SUBSONIC) << "stream" << m_id << "read rate" << m_bytesRead << "B/s at" << QIODevice::pos()
                          << "buffered" << m_state->buffered() << "t" << now;
        m_bytesRead   = 0;
        m_lastRateLog = now;
    }

    void startRequest(qint64 rangeStart)
    {
        const QUrl url         = m_url;
        const bool ignoreSsl   = m_ignoreSslErrors;
        const auto state       = m_state;
        const auto network     = m_network;
        const qsizetype bufferBytes = m_bufferBytes;
        const QPointer<SubsonicStreamDevice> device{this};

        const int id          = m_id;
        const int requestNo   = m_requests.fetch_add(1) + 1;
        const qint64 openedAt = m_clock.elapsed();
        qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "range" << rangeStart << "from readPos"
                          << QIODevice::pos() << "t" << openedAt;

        QMetaObject::invokeMethod(network.get(), [network, url, ignoreSsl, state, rangeStart, bufferBytes, device, id,
                                                  requestNo, openedAt] {
            qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "dispatched on network thread t"
                              << openedAt;
            QNetworkRequest request = makeNetworkRequest(
                url, NetworkRequestOption::AlwaysNetwork | NetworkRequestOption::AcceptAny);
            request.setTransferTimeout(StreamIdleTimeoutMs);
            if(rangeStart > 0) {
                request.setRawHeader("Range", QByteArrayLiteral("bytes=") + QByteArray::number(rangeStart) + '-');
            }

            QNetworkReply* reply = network->get(request);
            reply->setReadBufferSize(bufferBytes);

            if(ignoreSsl) {
                QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                                 [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
            }

            {
                const std::scoped_lock lock{state->mutex};
                if(state->aborted) {
                    reply->abort();
                    reply->deleteLater();
                    return;
                }
                if(state->reply && state->reply != reply) {
                    qCDebug(SUBSONIC) << "stream" << id << "reply superseded by request" << requestNo;
                }
                state->reply = reply;
            }

            QObject::connect(reply, &QIODevice::readyRead, reply, [state, reply] {
                {
                    const std::scoped_lock lock{state->mutex};
                    if(reply == state->reply && !state->aborted) {
                        state->updateHeaders(reply);
                        state->appendFromReply(reply);
                    }
                }
                state->ready.notify_all();
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
                {
                    const std::scoped_lock lock{state->mutex};
                    if(reply == state->reply) {
                        state->updateHeaders(reply);
                        state->appendFromReply(reply);
                        if(!state->aborted) {
                            if(reply->error() != QNetworkReply::NoError && state->attempts < MaxStreamRetries) {
                                // Resume from where we got to instead of failing the whole track.
                                ++state->attempts;
                                qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "failed:"
                                                  << reply->errorString() << "retry" << state->attempts;
                                retryFrom               = state->bufferBase + state->buffer.size();
                                state->error.clear(); // the retry replaces the failed transfer
                                state->headersProcessed = false;
                                state->requestedStart   = retryFrom;
                                state->reply.clear();
                            }
                            else {
                                if(reply->error() != QNetworkReply::NoError && state->error.isEmpty()) {
                                    state->error = reply->errorString();
                                    qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "gave up:"
                                                      << state->error;
                                }
                                state->finished = true;
                                state->reply.clear();
                            }
                        }
                    }
                }
                state->ready.notify_all();
                reply->deleteLater();
                if(retryFrom >= 0 && device) {
                    device->scheduleRetry(retryFrom);
                }
            });

            QObject::connect(reply, &QNetworkReply::errorOccurred, reply, [state, reply, id, requestNo](
                                                                               QNetworkReply::NetworkError error) {
                // Only logged: latching the error here made the retry path below unreachable, because the
                // decoder thread woke on the non-empty error and failed the track before `finished` ran.
                qCDebug(SUBSONIC) << "stream" << id << "request" << requestNo << "error" << static_cast<int>(error)
                                  << reply->errorString();
                state->ready.notify_all();
            });
        });
    }

    std::shared_ptr<NetworkAccessManager> m_network;
    QUrl m_url;
    bool m_ignoreSslErrors;
    qsizetype m_bufferBytes{DefaultBufferBytes};
    std::shared_ptr<StreamState> m_state;
    int m_id{nextDeviceId()};
    std::atomic<int> m_requests{0}; // touched from both the decoder and the network thread
    QElapsedTimer m_clock;
    qsizetype m_bytesRead{0};
    qint64 m_lastRateLog{-1000};
    qint64 m_lastWouldBlockLog{-1000};
};

class SubsonicRemoteSourceProvider : public RemoteSourceProvider
{
public:
    SubsonicRemoteSourceProvider(std::shared_ptr<NetworkAccessManager> network,
                                 std::shared_ptr<RemoteIoService> fallback, SettingsManager* settings)
        : m_network{std::move(network)}
        , m_fallback{std::move(fallback)}
        , m_settings{settings}
    { }

    [[nodiscard]] RemoteStreamSource createStreamSource(const QUrl& url) const override
    {
        if(!isSubsonicStreamUrl(url) || !m_network) {
            return m_fallback->createStreamSource(url);
        }

        const bool ignoreSsl
            = m_settings && m_settings->value(QStringLiteral("Subsonic/IgnoreSslErrors")).toBool();
        const SubsonicSettings settings = loadSubsonicSettings(m_settings);
        const qsizetype bufferBytes = static_cast<qsizetype>(settings.streamBufferMb) * 1024 * 1024;

        RemoteStreamSource source;
        auto device = std::make_unique<SubsonicStreamDevice>(m_network, url, ignoreSsl, bufferBytes);
        source.remoteDevice = device.get();
        source.device       = std::move(device);
        source.openTimeout  = std::chrono::milliseconds{8000};
        return source;
    }

private:
    std::shared_ptr<NetworkAccessManager> m_network;
    std::shared_ptr<RemoteIoService> m_fallback;
    SettingsManager* m_settings;
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

        const auto* device = dynamic_cast<SubsonicStreamDevice*>(source.device);
        m_seekable         = m_inner->isSeekable() && device && device->canSeek();
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

void registerSubsonicRemotePlayback(const std::shared_ptr<AudioLoader>& loader,
                                    std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings)
{
    if(!loader || !network) {
        return;
    }

    auto fallback = std::make_shared<RemoteIoService>(network, settings);
    loader->setRemoteSourceProvider(
        std::make_shared<SubsonicRemoteSourceProvider>(network, std::move(fallback), settings));

    DecoderCreator ffmpeg;
    for(const auto& entry : loader->decoders()) {
        if(entry.name.compare(u"FFmpeg"_s, Qt::CaseInsensitive) == 0) {
            ffmpeg = entry.creator;
            break;
        }
    }
    if(!ffmpeg) {
        qCWarning(SUBSONIC) << "FFmpeg decoder not found; Subsonic remote seeking is unavailable";
        return;
    }

    loader->addDecoder(u"Foonic Remote"_s, [ffmpeg] { return std::make_unique<SubsonicDecoder>(ffmpeg); }, 0);

    // fooyin restores the decoder order from saved settings after plugins initialise, which can
    // shuffle us behind FFmpeg. Put ourselves back in front once the event loop is running.
    QTimer::singleShot(0, [loader] { loader->changeDecoderIndex(u"Foonic Remote"_s, 0); });
}
} // namespace Fooyin::Subsonic
