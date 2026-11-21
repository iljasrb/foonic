/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicplaybackcontroller.h"

#include "subsonicbuffermonitor.h"
#include "subsonicclient.h"
#include "subsonicsettings.h"
#include "subsonicstreamcache.h"
#include "subsonicstreamutils.h"

#include <core/engine/fadingdefs.h>
#include <core/network/networkaccessmanager.h>
#include <core/network/networkutils.h>
#include <core/player/playercontroller.h>
#include <core/track.h>

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
namespace {
constexpr qint64 PrefetchProbeBytes = 1LL * 1024 * 1024;
}

SubsonicPlaybackController::SubsonicPlaybackController(std::shared_ptr<NetworkAccessManager> network,
                                                       SettingsManager* settings, PlayerController* player,
                                                       std::shared_ptr<SubsonicBufferMonitor> bufferMonitor,
                                                       std::shared_ptr<SessionCache> cache, QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
    , m_settings{settings}
    , m_client{new SubsonicClient{m_network, loadSubsonicSettings(settings), this}}
    , m_player{player}
    , m_bufferMonitor{std::move(bufferMonitor)}
    , m_cache{std::move(cache)}
{
    if(!m_player) {
        return;
    }

    connect(m_player, &PlayerController::currentTrackChanged, this, [this](const Track& track) {
        const QString newId = songIdForTrack(track);
        // If the prefetched track is now playing, keep its partial cache: the stream device picks
        // it up and continues from the first hole instead of starting the download again.
        const bool promoted = !m_prefetchId.isEmpty() && m_prefetchId == newId;
        qCDebug(SUBSONIC) << "current track" << newId << "prefetch" << m_prefetchId << "promoted" << promoted;

        m_currentId = newId;
        m_nextId.clear();
        m_prefetchFailedId.clear();
        updatePinnedTracks();
        cancelPrefetch(!promoted);

        if(m_bufferMonitor) {
            m_bufferMonitor->reset();
            if(m_cache && !m_currentId.isEmpty()) {
                const auto entry = m_cache->find(m_currentId);
                if(entry && entry->complete()) {
                    m_bufferMonitor->setRange(0.0, 1.0);
                }
            }
        }
        updateSubsonicFade(track.isRemote() && isSubsonicStreamUrl(QUrl{track.filepath()}));
        startScrobble(track);
    });
    connect(m_player, &PlayerController::upcomingTrackChanged, this,
            [this](const Player::UpcomingTrack& upcoming) {
                const QUrl url{upcoming.track.track.filepath()};
                const QString nextId
                    = isSubsonicStreamUrl(url) ? QUrlQuery{url}.queryItemValue(u"id"_s) : QString{};
                qCDebug(SUBSONIC) << "upcoming track" << nextId << "previous" << m_nextId;

                if(nextId == m_nextId) {
                    tryPrefetch();
                    return;
                }

                m_nextId = (nextId.isEmpty() || nextId == m_currentId) ? QString{} : nextId;
                m_prefetchFailedId.clear();
                updatePinnedTracks(); // unpin the previous next track before discarding its partial file
                if(!m_prefetchId.isEmpty() && m_prefetchId != m_nextId && m_prefetchId != m_currentId) {
                    cancelPrefetch(true); // the upcoming track changed while it was downloading
                }
                tryPrefetch();
            });
    if(m_bufferMonitor) {
        connect(m_bufferMonitor.get(), &SubsonicBufferMonitor::rangeChanged, this,
                &SubsonicPlaybackController::tryPrefetch);
    }
    connect(m_player, &PlayerController::positionChanged, this, [this](uint64_t ms) {
        maybeScrobble(ms);
        tryPrefetch();
    });
    connect(m_player, &PlayerController::playStateChanged, this,
            [this](Player::PlayState state, Player::PlayState) {
                if(state == Player::PlayState::Stopped) {
                    cancelPrefetch(true);
                    if(m_bufferMonitor) {
                        m_bufferMonitor->reset();
                    }
                }
            });
    // fooyin's seek container refreshes the elapsed label only on positionChanged, which the engine
    // publishes after the new position is prefetched. Republish it immediately so a remote seek shows
    // the target at once instead of snapping back at the end.
    connect(m_player, &PlayerController::positionMoved, this, [this](uint64_t ms) {
        if(m_player && isSubsonicStreamUrl(QUrl{m_player->currentTrack().filepath()})) {
            m_player->restoreCurrentPosition(ms);
        }
    });
}

SubsonicPlaybackController::~SubsonicPlaybackController()
{
    cancelPrefetch(true);

    // A settings override left behind by an active remote track must not outlive the plugin.
    if(m_crossfadeRestore) {
        m_settings->set(QStringLiteral("Engine/Crossfading"), m_crossfadeWasEnabled);
        m_settings->set(QStringLiteral("Engine/CrossfadingValues"), m_crossfadingValuesRestore);
    }
}

void SubsonicPlaybackController::updateSubsonicFade(bool active)
{
    // Remote Subsonic seeking is a hard cut unless fooyin's crossfade is on. Turn it on for remote
    // Subsonic playback (fade out old position, fade in new) and restore the user's settings after.
    // autoChange stays off so normal/gapless track transitions are untouched.
    const QString crossfadeKey = QStringLiteral("Engine/Crossfading");
    const QString valuesKey    = QStringLiteral("Engine/CrossfadingValues");

    if(!m_crossfadeRestoreTimer) {
        m_crossfadeRestoreTimer = new QTimer{this};
        m_crossfadeRestoreTimer->setSingleShot(true);
        connect(m_crossfadeRestoreTimer, &QTimer::timeout, this, [this, crossfadeKey, valuesKey] {
            if(!m_crossfadeRestore) {
                return;
            }
            m_settings->set(crossfadeKey, m_crossfadeWasEnabled);
            m_settings->set(valuesKey, m_crossfadingValuesRestore);
            m_crossfadeRestore = false;
        });
    }

    if(active) {
        m_crossfadeRestoreTimer->stop();
        if(!m_crossfadeRestore) {
            m_crossfadeWasEnabled      = m_settings->value(crossfadeKey).toBool();
            m_crossfadingValuesRestore = m_settings->value(valuesKey);
            m_crossfadeRestore         = true;
        }

        auto values = m_settings->value(valuesKey).value<Engine::CrossfadingValues>();
        const SubsonicSettings settings = loadSubsonicSettings(m_settings);
        const int fadeMs = std::clamp(settings.seekFadeMs, 0, 5000);
        values.seek = {.in = fadeMs, .out = fadeMs, .curve = Engine::FadeCurve::Linear, .enabled = settings.seekFadeEnabled};
        // Do not configure a manual crossfade: fooyin would prepare a remote crossfade target before
        // a user track change, and a non-started remote decoder cannot prime that stream. A normal
        // manual change fade still works and track clicks load immediately.
        values.manualChange.enabled = false;
        values.autoChange.enabled   = false;
        m_settings->set(crossfadeKey, true);
        m_settings->set(valuesKey, QVariant::fromValue(values));
    }
    else if(m_crossfadeRestore) {
        // Let an in-flight manual/seek fade finish before restoring the user's settings.
        m_crossfadeRestoreTimer->start(1000);
    }
}

QString SubsonicPlaybackController::songIdForTrack(const Track& track) const
{
    const QUrl url{track.filepath()};
    return isSubsonicStreamUrl(url) ? QUrlQuery{url}.queryItemValue(u"id"_s) : QString{};
}

void SubsonicPlaybackController::startScrobble(const Track& track)
{
    m_scrobbleId.clear();
    m_scrobbleAt = 0;
    m_scrobbled   = false;

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    if(!settings.scrobblePlays) {
        return;
    }

    const QString id = songIdForTrack(track);
    if(id.isEmpty()) {
        return;
    }

    // Report the play when half the track is done, or after four minutes for long tracks.
    const uint64_t duration = track.duration();
    m_scrobbleAt = duration > 0 ? std::min<uint64_t>(duration / 2, 4ULL * 60 * 1000) : 4ULL * 60 * 1000;
    m_scrobbleId = id;

    m_client->setSettings(settings);
    m_client->scrobble(id, false); // now playing
    qCDebug(SUBSONIC) << "Now playing" << id << "scrobble at" << m_scrobbleAt << "ms";
}

void SubsonicPlaybackController::maybeScrobble(uint64_t positionMs)
{
    if(m_scrobbled || m_scrobbleId.isEmpty() || positionMs < m_scrobbleAt) {
        return;
    }

    m_scrobbled = true;
    m_client->scrobble(m_scrobbleId, true);
    qCDebug(SUBSONIC) << "Scrobbled" << m_scrobbleId;
}

void SubsonicPlaybackController::updatePinnedTracks()
{
    if(!m_cache) {
        return;
    }

    QSet<QString> pins;
    if(!m_currentId.isEmpty()) {
        pins.insert(m_currentId);
    }
    if(!m_nextId.isEmpty()) {
        pins.insert(m_nextId);
    }

    for(const QString& id : std::as_const(m_pinnedIds)) {
        if(!pins.contains(id)) {
            m_cache->unpin(id);
        }
    }
    for(const QString& id : std::as_const(pins)) {
        m_cache->pin(id);
    }
    m_pinnedIds = pins;
    m_cache->evict();
}

bool SubsonicPlaybackController::hasComfortableLead() const
{
    if(!m_player || !m_bufferMonitor) {
        return true;
    }

    const Track track = m_player->currentTrack();
    if(!track.isValid() || !track.isRemote()) {
        return true;
    }
    if(!isSubsonicStreamUrl(QUrl{track.filepath()})) {
        return false; // don't compete with an unrelated remote stream
    }
    if(m_bufferMonitor->isComplete()) {
        return true;
    }

    const QVector<BufferRange> ranges = m_bufferMonitor->ranges();
    if(ranges.isEmpty()) {
        return false;
    }

    const uint64_t duration = track.duration();
    if(duration == 0) {
        return ranges.back().end >= 0.5;
    }

    const double position = std::clamp(static_cast<double>(m_player->currentPosition())
                                           / static_cast<double>(duration),
                                       0.0, 1.0);
    // Start prefetching once roughly half a minute is cached ahead, so the next track has a head
    // start without taking bandwidth away from the current one too early.
    const double lead = std::clamp(30000.0 / static_cast<double>(duration), 0.0, 1.0);
    for(const BufferRange& range : ranges) {
        if(range.start <= position + 0.000001 && range.end >= position) {
            return range.end - position >= lead;
        }
    }
    return false;
}

void SubsonicPlaybackController::tryPrefetch()
{
    if(!m_player || !m_cache || m_nextId.isEmpty() || !m_prefetchId.isEmpty() || m_prefetchReply) {
        return;
    }
    if(m_nextId == m_currentId) {
        return;
    }

    const bool comfortableLead = hasComfortableLead();
    if(m_prefetchFailedId == m_nextId) {
        if(!comfortableLead) {
            return;
        }
        m_prefetchFailedId.clear(); // a probe may have failed; full prefetch can still work
    }

    const auto existing = m_cache->find(m_nextId);
    if(existing && existing->complete()) {
        return;
    }

    if(!comfortableLead) {
        // Fetch just the first megabyte at low priority even while the current track is still
        // buffering. It gives the next track a warm decoder probe and instant first audio.
        if(!existing || existing->rangeList().empty()) {
            startPrefetch(m_nextId, true);
        }
        return;
    }

    startPrefetch(m_nextId, false);
}

void SubsonicPlaybackController::startPrefetch(const QString& songId, bool probe)
{
    if(!m_cache || songId.isEmpty() || !m_prefetchId.isEmpty() || m_prefetchReply) {
        return;
    }

    auto entry = m_cache->entry(songId);
    if(!entry || entry->complete()) {
        return;
    }

    m_prefetchEntry    = std::move(entry);
    m_prefetchId       = songId;
    m_prefetchProbe    = probe;
    m_prefetchAttempts = 0;
    m_prefetchError.clear();
    ++m_prefetchGeneration;

    m_client->setSettings(loadSubsonicSettings(m_settings));
    startPrefetchRequest();
}

void SubsonicPlaybackController::startPrefetchRequest()
{
    if(!m_prefetchEntry || m_prefetchReply || m_prefetchId.isEmpty()) {
        return;
    }

    qint64 start{-1};
    qint64 end{-1};
    if(m_prefetchEntry->total() >= 0) {
        const auto gap = m_prefetchEntry->firstGap();
        if(!gap) {
            qCDebug(SUBSONIC) << "Prefetched" << m_prefetchId;
            resetPrefetchState();
            if(m_cache) {
                m_cache->evict();
            }
            return;
        }
        start = gap->start;
        end   = gap->end;
    }
    else {
        // Unknown total: fill the first hole before the high-water mark, otherwise append.
        const auto ranges = m_prefetchEntry->rangeList();
        start             = 0;
        for(const ByteRange& range : ranges) {
            if(range.start > start) {
                break;
            }
            start = std::max(start, range.end);
        }
    }

    if(m_prefetchProbe) {
        end = end >= 0 ? std::min(end, start + PrefetchProbeBytes) : start + PrefetchProbeBytes;
    }

    m_prefetchRequestedStart = start;
    m_prefetchActiveLimit    = end;
    m_prefetchWritePos       = -1;
    m_prefetchResponseStart  = -1;
    m_prefetchResponseEnd    = -1;
    m_prefetchTotal          = m_prefetchEntry->total();
    m_prefetchHeaders        = false;
    m_prefetchFullResponse   = false;
    m_prefetchOpenEnded      = end < 0 || (m_prefetchTotal >= 0 && end == m_prefetchTotal);
    m_prefetchFatalError     = false;
    m_prefetchEof            = false;
    m_prefetchError.clear();

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);

    const QUrl streamUrl = m_client->streamUrl(m_prefetchId);
    m_prefetchEstimateTotal
        = QUrlQuery{streamUrl}.queryItemValue(u"estimateContentLength"_s) == u"true"_s;

    QNetworkRequest request = makeNetworkRequest(
        streamUrl, NetworkRequestOption::AlwaysNetwork | NetworkRequestOption::AcceptAny);
    request.setTransferTimeout(30000);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setPriority(QNetworkRequest::LowPriority); // never let it compete with the playing track
    QByteArray range = QByteArrayLiteral("bytes=") + QByteArray::number(start) + '-';
    if(!m_prefetchOpenEnded && end >= 0) {
        range += QByteArray::number(end - 1);
    }
    request.setRawHeader("Range", range);

    auto* reply = m_network->get(request);
    reply->setReadBufferSize(256 * 1024);
    if(settings.ignoreSslErrors) {
        connect(reply, &QNetworkReply::sslErrors, reply,
                [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    }
    m_prefetchReply = reply;

    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        if(reply == m_prefetchReply) {
            readPrefetchReply(reply);
        }
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if(reply == m_prefetchReply) {
            updatePrefetchHeaders(reply);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishPrefetch(reply); });
}

void SubsonicPlaybackController::updatePrefetchHeaders(QNetworkReply* reply)
{
    if(!reply || m_prefetchHeaders || !m_prefetchEntry) {
        return;
    }
    m_prefetchHeaders = true;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qint64 responseOffset = m_prefetchRequestedStart >= 0 ? m_prefetchRequestedStart : 0;

    const QByteArray contentType = reply->rawHeader("Content-Type").toLower().trimmed();
    const bool errorDocument = contentType.startsWith("text/xml") || contentType.startsWith("application/xml")
        || contentType.startsWith("application/json") || contentType.startsWith("text/html");
    if(status < 400 && errorDocument) {
        m_prefetchError      = QStringLiteral("The server returned an error instead of audio.");
        m_prefetchFatalError = true;
    }

    bool contentRangeSeen{false};
    const QByteArray contentRange = reply->rawHeader("Content-Range");
    if(!contentRange.isEmpty()) {
        const ContentRange range = parseContentRange(contentRange);
        if(range.total >= 0) {
            if(m_prefetchEstimateTotal) {
                if(m_prefetchTotal < 0 || range.total > m_prefetchTotal) {
                    m_prefetchTotal = range.total;
                }
                m_prefetchEntry->setEstimatedTotal(range.total);
            }
            else {
                m_prefetchTotal = range.total;
                m_prefetchEntry->setTotal(range.total);
            }
        }
        if(range.start >= 0) {
            responseOffset = range.start;
        }
        m_prefetchResponseEnd = range.end;
        contentRangeSeen      = true;
    }

    if(!contentRangeSeen && status == 200) {
        m_prefetchFullResponse = true;
        responseOffset         = 0;
        const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if(contentLength > 0) {
            if(m_prefetchEstimateTotal) {
                if(m_prefetchTotal < 0 || contentLength > m_prefetchTotal) {
                    m_prefetchTotal = contentLength;
                }
                m_prefetchEntry->setEstimatedTotal(contentLength);
            }
            else {
                m_prefetchTotal = contentLength;
                m_prefetchEntry->setTotal(contentLength);
            }
            m_prefetchResponseEnd = contentLength - 1;
        }
        m_prefetchActiveLimit = m_prefetchTotal;
    }
    else {
        m_prefetchFullResponse = false;
    }

    if(status == 416 && m_prefetchRequestedStart > 0) {
        qint64 actualTotal{-1};
        const auto ranges = m_prefetchEntry->rangeList();
        if(!ranges.empty() && ranges.front().start == 0 && m_prefetchRequestedStart >= ranges.back().end) {
            actualTotal = ranges.back().end;
        }
        if(actualTotal < 0 && contentRangeSeen && m_prefetchTotal >= 0) {
            actualTotal = m_prefetchTotal;
        }
        if(actualTotal >= 0) {
            m_prefetchTotal = actualTotal;
            m_prefetchEntry->setTotal(actualTotal);
            m_prefetchEof        = true;
            m_prefetchFatalError = false;
            m_prefetchError.clear();
            m_prefetchResponseStart = -1;
            m_prefetchWritePos      = -1;
            qCDebug(SUBSONIC) << "prefetch range beyond EOF; using actual size" << actualTotal;
            return;
        }
    }

    if(status >= 400) {
        m_prefetchError      = QStringLiteral("HTTP %1").arg(status);
        m_prefetchFatalError = status != 408 && status != 429 && status < 500;
    }
    else if(contentRangeSeen && m_prefetchRequestedStart >= 0 && responseOffset > m_prefetchRequestedStart) {
        m_prefetchError = QStringLiteral("The server returned an unexpected byte range.");
    }

    m_prefetchResponseStart = responseOffset;
    m_prefetchWritePos      = responseOffset;
    qCDebug(SUBSONIC) << "prefetch headers:" << "status" << status << "content-range" << contentRange << "total"
                      << m_prefetchTotal << "start" << m_prefetchWritePos << "limit" << m_prefetchActiveLimit;
}

void SubsonicPlaybackController::readPrefetchReply(QNetworkReply* reply)
{
    if(!reply || reply != m_prefetchReply) {
        return;
    }
    if(!m_prefetchHeaders) {
        updatePrefetchHeaders(reply);
    }
    if(!m_prefetchEntry || m_prefetchWritePos < 0) {
        reply->readAll();
        return;
    }

    QByteArray chunk = reply->readAll();
    if(chunk.isEmpty()) {
        return;
    }

    const qint64 written = m_prefetchEntry->writeAt(m_prefetchWritePos, chunk.constData(), chunk.size());
    if(written <= 0) {
        m_prefetchError = m_prefetchEntry->errorString();
        if(m_prefetchError.isEmpty()) {
            m_prefetchError = QStringLiteral("Could not write the stream cache.");
        }
        return;
    }

    m_prefetchWritePos += written;
    if(m_prefetchTotal > 0 && m_prefetchWritePos > m_prefetchTotal) {
        m_prefetchTotal       = m_prefetchWritePos;
        m_prefetchActiveLimit = m_prefetchTotal;
        m_prefetchEntry->setTotal(m_prefetchTotal);
    }
}

void SubsonicPlaybackController::finishPrefetch(QNetworkReply* reply)
{
    if(!reply || reply != m_prefetchReply) {
        if(reply) {
            reply->deleteLater();
        }
        return;
    }

    if(!m_prefetchHeaders) {
        updatePrefetchHeaders(reply);
    }
    if(m_prefetchError.isEmpty() && m_prefetchHeaders) {
        readPrefetchReply(reply);
    }

    m_prefetchReply = nullptr;

    const int status         = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto replyErrorCode = reply->error();
    const QString replyError  = reply->errorString();
    reply->deleteLater();

    const bool failed = !m_prefetchEof
        && (status >= 400 || replyErrorCode != QNetworkReply::NoError || !m_prefetchError.isEmpty());
    if(failed) {
        if(!m_prefetchFatalError && m_prefetchAttempts < 3) {
            ++m_prefetchAttempts;
            const int generation = m_prefetchGeneration;
            qCDebug(SUBSONIC) << "Prefetch" << m_prefetchId << "failed:" << replyError << "retry"
                              << m_prefetchAttempts;
            QTimer::singleShot(500, this, [this, generation] {
                if(generation == m_prefetchGeneration && !m_prefetchReply) {
                    m_prefetchError.clear();
                    startPrefetchRequest();
                }
            });
            return;
        }

        const QString failedId = m_prefetchId;
        qCWarning(SUBSONIC) << "Prefetch gave up for" << failedId << m_prefetchError;
        m_prefetchFailedId = failedId;
        resetPrefetchState();
        if(m_cache && failedId != m_currentId) {
            m_cache->discardIncomplete(failedId);
            m_cache->evict();
        }
        return;
    }

    if(m_prefetchEstimateTotal && m_prefetchOpenEnded && m_prefetchEntry->total() >= 0
       && m_prefetchWritePos > m_prefetchResponseStart) {
        // estimateContentLength is only a guess; a response that ran to EOF gives the real size.
        m_prefetchTotal = m_prefetchWritePos;
        m_prefetchEntry->setTotal(m_prefetchTotal);
    }

    if(m_prefetchEntry->total() < 0 && m_prefetchFullResponse) {
        const auto ranges = m_prefetchEntry->rangeList();
        const qint64 total = ranges.empty() ? 0 : ranges.back().end;
        m_prefetchEntry->setTotal(total);
    }

    const bool noProgress = !m_prefetchEof && m_prefetchWritePos <= m_prefetchResponseStart;
    if(noProgress) {
        if(m_prefetchAttempts < 3) {
            ++m_prefetchAttempts;
            const int generation = m_prefetchGeneration;
            QTimer::singleShot(500, this, [this, generation] {
                if(generation == m_prefetchGeneration && !m_prefetchReply) {
                    m_prefetchHeaders = false;
                    startPrefetchRequest();
                }
            });
        }
        else {
            m_prefetchFailedId = m_prefetchId;
            qCWarning(SUBSONIC) << "Prefetch gave up for" << m_prefetchId << "after empty ranges";
            const QString failedId = m_prefetchId;
            resetPrefetchState();
            if(m_cache && failedId != m_currentId) {
                m_cache->discardIncomplete(failedId);
                m_cache->evict();
            }
        }
        return;
    }

    if(m_prefetchEntry->complete()) {
        qCDebug(SUBSONIC) << "Prefetched" << m_prefetchId;
        resetPrefetchState();
        if(m_cache) {
            m_cache->evict();
        }
        return;
    }

    if(m_prefetchProbe) {
        qCDebug(SUBSONIC) << "Prefetch probe ready for" << m_prefetchId;
        resetPrefetchState();
        if(m_cache) {
            m_cache->evict();
        }
        tryPrefetch(); // start the full prefetch now if the current track has caught up
        return;
    }

    m_prefetchAttempts = 0; // a complete range was received
    m_prefetchHeaders  = false;
    m_prefetchError.clear();
    startPrefetchRequest();
}

void SubsonicPlaybackController::cancelPrefetch(bool discard)
{
    ++m_prefetchGeneration;

    const QString cancelledId = m_prefetchId;
    auto* reply               = m_prefetchReply;
    m_prefetchReply           = nullptr;
    m_prefetchEntry.reset();
    m_prefetchId.clear();
    m_prefetchProbe    = false;
    m_prefetchAttempts = 0;
    m_prefetchError.clear();

    if(reply) {
        reply->abort(); // finished() deletes it; it no longer matches m_prefetchReply
    }

    if(discard && !cancelledId.isEmpty() && m_cache && cancelledId != m_currentId) {
        m_cache->discardIncomplete(cancelledId);
        m_cache->evict();
    }
}

void SubsonicPlaybackController::resetPrefetchState()
{
    m_prefetchReply = nullptr;
    m_prefetchEntry.reset();
    m_prefetchId.clear();
    m_prefetchProbe    = false;
    m_prefetchAttempts = 0;
    m_prefetchRequestedStart = -1;
    m_prefetchActiveLimit    = -1;
    m_prefetchWritePos       = -1;
    m_prefetchResponseStart  = -1;
    m_prefetchResponseEnd    = -1;
    m_prefetchTotal          = -1;
    m_prefetchHeaders        = false;
    m_prefetchFullResponse   = false;
    m_prefetchOpenEnded      = false;
    m_prefetchEstimateTotal  = false;
    m_prefetchFatalError     = false;
    m_prefetchEof            = false;
    m_prefetchError.clear();
}
} // namespace Fooyin::Subsonic
