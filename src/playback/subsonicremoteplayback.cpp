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
// FFmpeg probes backwards after a seek to find the exact frame, and its binary search can span a
// megabyte or more (seektable points, not just frame boundaries). A backward seek target fetches this
// margin behind it so the whole probe burst hits memory instead of issuing one HTTP request per probe.
// Forward seeks start exactly at the target: the bytes before it were never downloaded this session,
// and pre-fetching them would only delay the first decoded frame.
constexpr qint64 SeekBackMarginFloor = 1024LL * 1024;
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
    void appendFromReply(QNetworkReply* reply)
    {
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

        QByteArray chunk = reply->read(space + skip);
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
        }
    }

    //! Derives size/range support/skip offset from response headers. Caller must hold the mutex.
    void updateHeaders(QNetworkReply* reply)
    {
        if(headersProcessed) {
            return;
        }
        headersProcessed = true;

        const QByteArray contentRange = reply->rawHeader("Content-Range");
        if(!contentRange.isEmpty()) {
            const ContentRange range = parseContentRange(contentRange);
            if(range.total >= 0) {
                total = range.total;
            }
            if(range.start >= 0) {
                skip = std::max<qint64>(0, requestedStart - range.start);
            }
            rangeCapable = true;
            qCDebug(SUBSONIC) << "stream headers: content-range" << contentRange << "total" << total << "skip"
                              << skip;
            return;
        }

        const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if(contentLength > 0 && total < 0) {
            total = contentLength;
        }
        if(reply->rawHeader("Accept-Ranges").toLower().contains("bytes")) {
            rangeCapable = true;
        }
        // The server ignored our Range request and is sending from 0 again.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
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
        QTimer::singleShot(StreamRetryDelayMs, this, [this, from] { startRequest(from); });
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
            m_state->pendingRequest   = false;
            m_state->skipTo          = -1;
            m_state->finished         = false;
            m_state->aborted          = false;
            m_state->error.clear();
            m_state->reply.clear();
        }

        QIODevice::open(mode);
        startRequest(0);

        // Wait briefly for headers so size()/canSeek() are valid during decoder init.
        std::unique_lock lock{m_state->mutex};
        m_state->ready.wait_for(lock, OpenHeaderTimeout, [this] {
            return m_state->headersProcessed || m_state->finished || !m_state->error.isEmpty();
        });
        qCDebug(SUBSONIC) << "stream opened: total" << m_state->total << "seekable" << m_state->canSeek()
                          << "error" << m_state->error;
        return true;
    }

    void close() override
    {
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
            else if(pos > end && pos - end <= std::max<qint64>(1024 * 1024, m_bufferBytes / 32) && m_state->reply
                    && !m_state->finished && m_state->error.isEmpty()) {
                // Still downloading forward: wait for it to cover `pos` instead of dropping the buffer
                // (which would also lose everything already downloaded behind us).
                m_state->skipTo = pos;
                handled         = true;
            }
        }
        qCDebug(SUBSONIC) << "seek" << pos << (handled ? "buffered/wait" : "refetch") << "bufferBase"
                          << bufferBase << "bufferBytes" << bufferBytes;
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
            const qint64 backwardMargin = std::max<qint64>(SeekBackMarginFloor, m_bufferBytes / 32);
            const qint64 requestedStart = pos < bufferBase ? std::max<qint64>(0, pos - backwardMargin) : pos;
            const qsizetype readPos    = static_cast<qsizetype>(pos - requestedStart);

            oldReply                  = m_state->reply;
            m_state->reply.clear();
            m_state->buffer.clear();
            m_state->readPos          = readPos;
            m_state->bufferBase       = requestedStart;
            m_state->skip             = 0;
            m_state->requestedStart   = requestedStart;
            m_state->headersProcessed = false;
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
        bool ready{true};
        if(m_state->nonBlocking) {
            ready = m_state->ready.wait_for(lock, token, std::chrono::milliseconds{50}, readable);
        }
        else {
            m_state->ready.wait(lock, token, readable);
        }
        m_state->readWouldBlock = false;

        if(token.stop_requested()) {
            return -1;
        }
        if(!ready) {
            m_state->readWouldBlock = true;
            return -1;
        }
        if(m_state->skipTo >= 0) {
            m_state->readPos = static_cast<qsizetype>(
                std::min<qint64>(m_state->skipTo - m_state->bufferBase, m_state->buffer.size()));
            m_state->skipTo = -1;
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
    void startRequest(qint64 rangeStart)
    {
        const QUrl url         = m_url;
        const bool ignoreSsl   = m_ignoreSslErrors;
        const auto state       = m_state;
        const auto network     = m_network;
        const qsizetype bufferBytes = m_bufferBytes;
        const QPointer<SubsonicStreamDevice> device{this};

        QMetaObject::invokeMethod(network.get(), [network, url, ignoreSsl, state, rangeStart, bufferBytes, device] {
            QNetworkRequest request = makeNetworkRequest(
                url, NetworkRequestOption::AlwaysNetwork | NetworkRequestOption::AcceptAny);
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

            QObject::connect(reply, &QNetworkReply::finished, reply, [device, state, reply] {
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
                                retryFrom              = state->bufferBase + state->buffer.size();
                                state->headersProcessed = false;
                                state->requestedStart   = retryFrom;
                                state->reply.clear();
                            }
                            else {
                                if(reply->error() != QNetworkReply::NoError && state->error.isEmpty()) {
                                    state->error = reply->errorString();
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

            QObject::connect(reply, &QNetworkReply::errorOccurred, reply, [state, reply](QNetworkReply::NetworkError) {
                {
                    const std::scoped_lock lock{state->mutex};
                    if(reply == state->reply && !state->aborted && state->error.isEmpty()) {
                        state->error = reply->errorString();
                    }
                }
                state->ready.notify_all();
            });
        });
    }

    std::shared_ptr<NetworkAccessManager> m_network;
    QUrl m_url;
    bool m_ignoreSslErrors;
    qsizetype m_bufferBytes{DefaultBufferBytes};
    std::shared_ptr<StreamState> m_state;
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
            m_inner.reset();
            return {};
        }

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
        if(m_inner) {
            m_inner->stop();
        }
    }

    void seek(uint64_t pos) override
    {
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
