/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicdownloadmanager.h"

#include "subsonicclient.h"
#include "subsonicsettings.h"
#include "subsonicstreamutils.h"
#include "subsonictrack.h"

#include <core/engine/audioloader.h>
#include <core/engine/fadingdefs.h>
#include <core/network/networkaccessmanager.h>
#include <core/player/playercontroller.h>
#include <core/playlist/playlist.h>
#include <core/playlist/playlisthandler.h>
#include <core/track.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace {
constexpr int MaxAttempts = 5;
constexpr int RetryDelayMs = 750;
constexpr qint64 ProgressEmitIntervalMs = 250;
// A live switch close to the end would interrupt the track for no real benefit; let it finish and use
// the local copy from the next play onward.
constexpr uint64_t NearEndSkipMs = 10000;
// The seek command is dispatched to the engine thread, which runs a few ms behind the position we
// captured; nudge the restore point forward by that latency so the seekbar doesn't visibly step back.
constexpr uint64_t SeekDispatchMarginMs = 15;

QString songDisplayName(const QJsonObject& song)
{
    const QString artist = song.value(u"artist"_s).toString();
    const QString title  = song.value(u"title"_s).toString();
    if(artist.isEmpty()) {
        return title;
    }
    if(title.isEmpty()) {
        return artist;
    }
    return artist + u" - "_s + title;
}
} // namespace

namespace Fooyin::Subsonic {
SubsonicDownloadManager::SubsonicDownloadManager(std::shared_ptr<NetworkAccessManager> network,
                                                 SettingsManager* settings, PlayerController* player,
                                                 PlaylistHandler* handler, std::shared_ptr<AudioLoader> audioLoader,
                                                 std::shared_ptr<SubsonicCacheRegistry> cacheRegistry,
                                                 QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
    , m_audioLoader{std::move(audioLoader)}
    , m_cacheRegistry{std::move(cacheRegistry)}
    , m_settings{settings}
    , m_client{new SubsonicClient{m_network, loadSubsonicSettings(settings), this}}
    , m_player{player}
    , m_handler{handler}
{
    if(m_player) {
        connect(m_player, &PlayerController::currentTrackChanged, this,
                &SubsonicDownloadManager::onCurrentTrackChanged);
        connect(m_player, &PlayerController::currentTrackSeekableChanged, this,
                [this](bool) { tryApplyPendingSeek(); });
        connect(m_player, &PlayerController::positionChanged, this,
                [this](uint64_t ms) { maybeScrobble(ms); });
        // fooyin's seek container refreshes the elapsed label only on positionChanged, which the
        // engine publishes after the new position is prefetched. Republish it immediately so a
        // remote seek shows the target at once instead of snapping back at the end.
        connect(m_player, &PlayerController::positionMoved, this, [this](uint64_t ms) {
            if(m_player && isSubsonicStreamUrl(QUrl{m_player->currentTrack().filepath()})) {
                m_player->restoreCurrentPosition(ms);
            }
        });
        connect(m_player, &PlayerController::playStateChanged, this, [this](Player::PlayState state) {
            if(state == Player::PlayState::Paused) {
                switchCurrentToCached();
            }
        });
    }
}

SubsonicDownloadManager::~SubsonicDownloadManager()
{
    // Disconnect first so aborting the reply doesn't re-enter this half-destroyed object.
    if(m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if(m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    // A settings override left behind by an active remote track must not outlive the plugin.
    if(m_crossfadeRestore) {
        m_settings->set(QStringLiteral("Engine/Crossfading"), m_crossfadeWasEnabled);
        m_settings->set(QStringLiteral("Engine/CrossfadingValues"), m_crossfadingValuesRestore);
    }
}

QStringList SubsonicDownloadManager::registerSongs(const QList<QJsonObject>& songs, const QStringList& urls,
                                                   const QString& dir)
{
    m_dir = dir.isEmpty() ? defaultDownloadDir() : dir;
    QDir{}.mkpath(m_dir);

    // Forget pending work from previous selections but keep known entries so earlier items can still
    // be resolved and re-fetched later.
    m_requests.clear();
    m_priority  = -1;
    m_gateIndex = -1;

    QStringList paths;
    paths.reserve(songs.size());
    std::vector<int> newIndices;
    newIndices.reserve(songs.size());
    for(int i{0}; i < songs.size(); ++i) {
        const QJsonObject& song = songs.at(i);
        const QString url       = urls.value(i);
        const QString songId    = song.value(u"id"_s).toString();

        // A permanent music-library download acts as the cache: reuse it instead of downloading again.
        if(const int existing = indexForSongId(songId);
           existing >= 0 && m_entries[existing].library && m_entries[existing].done
           && QFileInfo::exists(m_entries[existing].path)) {
            m_entries[existing].song = song;
            m_entries[existing].url  = url;
            paths.push_back(m_entries[existing].path);
            newIndices.push_back(existing);
            continue;
        }

        const QString path = pathForSong(song, m_dir);

        int index = indexForPath(path);
        if(index < 0) {
            Entry entry;
            entry.song = song;
            entry.url  = url;
            entry.path = path;

            const qint64 expected = static_cast<qint64>(song.value(u"size"_s).toDouble());
            const QFileInfo info{entry.path};
            entry.done = info.exists() && info.size() > 0 && (expected <= 0 || info.size() >= expected);

            m_entries.push_back(entry);
            index = static_cast<int>(m_entries.size()) - 1;
            m_indexByPath.insert(path, index);
            if(const QString songId = song.value(u"id"_s).toString(); !songId.isEmpty()) {
                m_indexById.insert(songId, index);
            }
        }
        else {
            m_entries[index].url = url; // the token changes between sessions
        }

        paths.push_back(path);
        newIndices.push_back(index);
    }

    // Queue the first track (plus the prefetch window) so a download starts even if playback is not
    // triggered immediately (e.g. added to a non-active or restored playlist). The rest are queued
    // when they get close to playback.
    const int window = std::max(1, loadSubsonicSettings(m_settings).prefetchCount) + 1;
    for(int k{0}; k < static_cast<int>(newIndices.size()) && k < window; ++k) {
        const int index = newIndices[k];
        if(!m_entries[index].done) {
            m_requests.insert(index);
        }
    }

    processNext();
    Q_EMIT downloadProgressChanged();
    return paths;
}

void SubsonicDownloadManager::ensureDownloaded(const QString& songId, std::function<void(bool)> onDone)
{
    const int index = indexForSongId(songId);
    if(index < 0) {
        if(onDone) {
            onDone(false);
        }
        return;
    }

    if(m_entries[index].done) {
        if(onDone) {
            onDone(true);
        }
        return;
    }

    if(onDone) {
        auto& slot = m_callbacks[index];
        if(slot) {
            auto previous = std::move(slot);
            slot = [previous, next = std::move(onDone)](bool ok) {
                previous(ok);
                next(ok);
            };
        }
        else {
            slot = std::move(onDone);
        }
    }
    request(index, true);
}

void SubsonicDownloadManager::applyPlaybackMode()
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    if(settings.playbackMode == PlaybackMode::Stream) {
        cancelDownloads();
        return;
    }

    // Download-first: start caching whatever is playing now (plus its prefetch window).
    if(m_player) {
        const Track current = m_player->currentTrack();
        if(current.isValid()) {
            onCurrentTrackChanged(current);
        }
    }
}

void SubsonicDownloadManager::cancelDownloads()
{
    m_requests.clear();
    m_priority = -1;
    const bool wasGated = m_gateIndex >= 0;
    m_gateIndex           = -1;
    m_pendingSeekPosition = 0;
    m_pendingSeekPath.clear();

    if(m_active >= 0) {
        m_cancelActive = true;
        if(m_reply) {
            m_reply->abort();
        }
        else {
            m_cancelActive = false;
            finishCurrent(false);
        }
    }

    if(wasGated && m_player) {
        m_player->play();
    }

    Q_EMIT downloadProgressChanged();
}

bool SubsonicDownloadManager::recoverMissingCacheTrack(const Track& track)
{
    if(!m_player || track.isRemote() || track.isVirtual() || track.isInArchive()) {
        return false;
    }

    const QString path = track.filepath();
    if(path.isEmpty() || QFileInfo::exists(path) || !isCachePath(path)
       || songIdFromCachePath(path).isEmpty()) {
        return false;
    }

    qCInfo(SUBSONIC) << "Recovering missing cache file" << path << "as remote stream";
    Q_EMIT activityChanged(tr("Recovering missing cache: %1").arg(QFileInfo{path}.fileName()), true);
    // Repoints the current dead entry and every other dead cache entry in the playlists, so switching
    // to a neighbour doesn't fail with "no decoder available" before onCurrentTrackChanged can run.
    repointDeadCachePaths();
    return true;
}

std::optional<Track> SubsonicDownloadManager::remoteTrackForDeadCachePath(const QString& path,
                                                                          const Track& original) const
{
    if(path.isEmpty() || original.isRemote() || original.isVirtual() || original.isInArchive()) {
        return std::nullopt;
    }
    if(!isCachePath(path) || QFileInfo::exists(path)) {
        return std::nullopt;
    }

    const QString songId = songIdFromCachePath(path);
    if(songId.isEmpty()) {
        return std::nullopt;
    }

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    if(!settings.isValid()) {
        return std::nullopt;
    }

    m_client->setSettings(settings);
    const QUrl url         = m_client->streamUrl(songId);
    const QString coverUrl = m_client->coverArtUrl(songId).toString();

    Track remote = original;
    remote.setFilePath(url.toString());
    if(!coverUrl.isEmpty()) {
        remote.replaceExtraTag(QStringLiteral("COVERART"), coverUrl);
    }
    return remote;
}

void SubsonicDownloadManager::repointDeadCachePaths()
{
    QHash<QString, Track> remotes; // dead cache path -> recovered remote track

    if(m_player) {
        const PlaylistTrack current = m_player->currentPlaylistTrack();
        if(current.isValid()) {
            const QString path = current.track.filepath();
            if(!path.isEmpty() && !remotes.contains(path)) {
                if(const auto remote = remoteTrackForDeadCachePath(path, current.track)) {
                    remotes.insert(path, *remote);
                }
            }
        }
    }

    if(m_handler) {
        for(Playlist* playlist : m_handler->playlists()) {
            for(const PlaylistTrack& playlistTrack : playlist->playlistTracks()) {
                const QString path = playlistTrack.track.filepath();
                if(path.isEmpty() || remotes.contains(path)) {
                    continue;
                }
                if(const auto remote = remoteTrackForDeadCachePath(path, playlistTrack.track)) {
                    remotes.insert(path, *remote);
                }
            }
        }
    }

    if(remotes.isEmpty()) {
        return;
    }

    // Deferred: replacing playlist entries / re-committing the current track re-enters this object.
    QMetaObject::invokeMethod(
        this,
        [this, remotes] {
            if(m_handler) {
                for(Playlist* playlist : m_handler->playlists()) {
                    PlaylistTrackList playlistTracks = playlist->playlistTracks();
                    bool changed{false};
                    for(PlaylistTrack& playlistTrack : playlistTracks) {
                        const auto it = remotes.constFind(playlistTrack.track.filepath());
                        if(it != remotes.cend()) {
                            playlistTrack.track = it.value();
                            changed             = true;
                        }
                    }
                    if(changed) {
                        m_handler->replacePlaylistTracks(playlist->id(), playlistTracks,
                                                         PlaylistTrackChangeSource::External);
                    }
                }
            }

            if(m_player) {
                PlaylistTrack current = m_player->currentPlaylistTrack();
                if(current.isValid()) {
                    const auto it = remotes.constFind(current.track.filepath());
                    if(it != remotes.cend() && !QFileInfo::exists(current.track.filepath())) {
                        m_pendingSeekPosition = 0;
                        m_pendingSeekPath.clear();
                        current.track = it.value();
                        m_player->changeCurrentTrack(current, {Player::AdvanceReason::ManualSelection, true});
                    }
                }
            }
        },
        Qt::QueuedConnection);
}

void SubsonicDownloadManager::onCurrentTrackChanged(const Track& track)
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);

    // A restored playlist can point at a cache file that has since been evicted; play it remotely
    // again instead of failing with "no decoder available".
    if(recoverMissingCacheTrack(track)) {
        return;
    }

    if(!track.isRemote() && isCachePath(track.filepath())) {
        Q_EMIT activityChanged(tr("Playing from cache: %1").arg(track.title()), false);
    }

    updateSubsonicFade(track.isRemote() && isSubsonicStreamUrl(QUrl{track.filepath()}));
    startScrobble(track);

    // Stream mode never caches, so don't register or download anything (including restored playlists).
    if(settings.playbackMode == PlaybackMode::Stream) {
        m_requests.clear();
        m_priority            = -1;
        m_pendingSeekPosition = 0;
        m_pendingSeekPath.clear();
        if(track.isRemote()) {
            Q_EMIT activityChanged(tr("Streaming: %1").arg(track.title()), false);
        }
        return;
    }

    // A promotion re-commits the same track with its local file. Apply the saved position once the
    // local track is current; if a different track starts, drop the pending seek.
    if(m_pendingSeekPosition != 0) {
        if(track.filepath() == m_pendingSeekPath) {
            tryApplyPendingSeek();
        }
        else {
            m_pendingSeekPosition = 0;
            m_pendingSeekBaseMs   = 0;
            m_pendingSeekPath.clear();
        }
    }

    int index = -1;
    for(int i{0}; i < static_cast<int>(m_entries.size()); ++i) {
        if(m_entries[i].url == track.filepath()) {
            index = i;
            break;
        }
    }

    // Handle tracks that weren't registered this session (e.g. restored playlists).
    if(index < 0 && track.isRemote()) {
        index = registerRemoteTrack(track);
    }

    if(index < 0) {
        if(track.isRemote()) {
            qCDebug(SUBSONIC) << "Current remote track is not a Subsonic stream:" << track.filepath();
        }
        return;
    }

    if(m_entries[index].done) {
        // We have a local copy but the current track is still the remote stream (e.g. a restored
        // playlist): switch it to the cached/library file so it plays (and seeks) from disk.
        qCDebug(SUBSONIC) << "Current track already cached" << m_entries[index].path;
        promoteEntryToLocal(index);
        return;
    }

    // When streaming-while-downloading is disabled, hold playback until the file is ready.
    if(!settings.streamWhileDownloading && m_player) {
        m_gateIndex = index;
        m_player->pause();
    }

    qCInfo(SUBSONIC) << "Current track will be cached" << m_entries[index].path;
    Q_EMIT activityChanged(tr("Streaming & caching: %1").arg(songDisplayName(m_entries[index].song)), true);
    request(index, true);
    for(int j{1}; j <= settings.prefetchCount; ++j) {
        request(index + j);
    }
}

int SubsonicDownloadManager::registerRemoteTrack(const Track& track)
{
    const QUrl url{track.filepath()};
    if(url.path().section(QLatin1Char{'/'}, -1) != "stream"_L1) {
        return -1;
    }

    const QString id = QUrlQuery{url}.queryItemValue(u"id"_s);
    if(id.isEmpty()) {
        return -1;
    }

    // A permanent library download is the cache: point playback at the local file instead of streaming.
    if(const int existing = indexForSongId(id);
       existing >= 0 && m_entries[existing].library && m_entries[existing].done
       && QFileInfo::exists(m_entries[existing].path)) {
        m_entries[existing].url = track.filepath();
        return existing;
    }

    if(m_dir.isEmpty()) {
        m_dir = loadSubsonicSettings(m_settings).downloadDir;
        QDir{}.mkpath(m_dir);
    }

    QJsonObject song;
    song[u"id"_s]     = id;
    song[u"title"_s]  = track.title();
    song[u"album"_s]  = track.album();
    song[u"suffix"_s] = track.codec().toLower();
    if(!track.artists().empty()) {
        song[u"artist"_s] = track.artists().front();
    }
    if(!track.albumArtists().empty()) {
        song[u"albumArtist"_s] = track.albumArtists().front();
    }
    if(!track.genres().empty()) {
        song[u"genre"_s] = track.genres().front();
    }
    if(track.trackNumber().toInt() > 0) {
        song[u"track"_s] = track.trackNumber().toInt();
    }
    if(track.discNumber().toInt() > 0) {
        song[u"discNumber"_s] = track.discNumber().toInt();
    }
    if(track.year() > 0) {
        song[u"year"_s] = track.year();
    }
    if(track.bitrate() > 0) {
        song[u"bitRate"_s] = track.bitrate();
    }
    if(track.duration() > 0) {
        song[u"duration"_s] = static_cast<int>(track.duration() / 1000);
    }
    // Recover the Subsonic cover id from the track's artwork tag so the promoted local track keeps it.
    if(const QStringList coverTags = track.extraTag(u"COVERART"_s); !coverTags.isEmpty()) {
        if(const QString coverId = QUrlQuery{QUrl{coverTags.front()}}.queryItemValue(u"id"_s);
           !coverId.isEmpty()) {
            song[u"coverArt"_s] = coverId;
        }
    }

    const QString path = pathForSong(song, m_dir);
    if(const int existing = indexForPath(path); existing >= 0) {
        m_entries[existing].url = track.filepath();
        return existing;
    }

    Entry entry;
    entry.song = song;
    entry.url  = track.filepath();
    entry.path = path;
    // A restored playlist has no song size, so treat an existing non-empty file as cached instead of
    // downloading it again on every restart.
    entry.done = QFileInfo::exists(path) && QFileInfo{path}.size() > 0;
    m_entries.push_back(entry);
    const int index = static_cast<int>(m_entries.size()) - 1;
    m_indexByPath.insert(path, index);
    m_indexById.insert(id, index);
    return index;
}

void SubsonicDownloadManager::tryApplyPendingSeek()
{
    if(m_pendingSeekPosition == 0 || !m_player || !m_player->currentTrackSeekable()) {
        return;
    }

    // The seekable signal can fire while the engine is still loading, before the promoted local
    // track has been committed. Wait until it is actually the current track.
    if(m_player->currentTrack().filepath() != m_pendingSeekPath) {
        return;
    }

    const uint64_t position = std::exchange(m_pendingSeekPosition, 0);
    const qint64 elapsed    = QDateTime::currentMSecsSinceEpoch() - std::exchange(m_pendingSeekBaseMs, 0);
    m_pendingSeekPath.clear();

    // The remote kept playing while the engine prepared and faded, so advance the restore point by
    // that wall-clock time to land on the exact position.
    const uint64_t target = position + (elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0) + SeekDispatchMarginMs;
    qCInfo(SUBSONIC) << "Restoring position after promotion:" << target << "ms (base" << position << "ms, +"
                     << elapsed << "ms)";
    m_player->seek(target);
}

void SubsonicDownloadManager::request(int index, bool priority)
{
    if(index < 0 || index >= static_cast<int>(m_entries.size()) || m_entries[index].done) {
        return;
    }

    if(priority) {
        m_priority = index;
    }
    else {
        m_requests.insert(index);
    }
    processNext();
}

void SubsonicDownloadManager::processNext()
{
    if(m_active >= 0) {
        return;
    }

    int index{-1};
    if(m_priority >= 0 && m_priority < static_cast<int>(m_entries.size()) && !m_entries[m_priority].done) {
        index = m_priority;
    }
    m_priority = -1;

    while(index < 0 && !m_requests.empty()) {
        const int candidate = *m_requests.begin();
        m_requests.erase(m_requests.begin());
        if(candidate >= 0 && candidate < static_cast<int>(m_entries.size()) && !m_entries[candidate].done) {
            index = candidate;
            break;
        }
    }

    if(index < 0) {
        return;
    }

    m_active       = index;
    m_received     = 0;
    m_requestStart = 0;
    m_total        = -1;
    m_attempts     = 0;
    m_lastProgressEmit = 0;

    m_client->setSettings(loadSubsonicSettings(m_settings));

    const Entry& entry    = m_entries[index];
    const qint64 expected = static_cast<qint64>(entry.song.value(u"size"_s).toDouble());
    const QFileInfo info{entry.path};
    const qint64 existing = info.exists() ? info.size() : 0;

    // Resume an interrupted download instead of discarding the partial file.
    const bool resuming = existing > 0 && (expected <= 0 || existing < expected);
    m_received          = resuming ? existing : 0;

    if(m_file) {
        m_file->close();
        delete m_file;
    }
    m_file = new QFile{entry.path};
    const auto openMode
        = resuming ? (QIODevice::WriteOnly | QIODevice::Append) : QIODevice::WriteOnly;
    if(!m_file->open(openMode)) {
        qCWarning(SUBSONIC) << "Could not write to" << entry.path;
        finishCurrent(false);
        return;
    }

    qCInfo(SUBSONIC) << "Downloading" << entry.path << (resuming ? u"(resuming)"_s : QString{});
    {
        const bool isCurrent = m_player && !entry.url.isEmpty()
                               && m_player->currentTrack().filepath() == entry.url;
        Q_EMIT activityChanged(isCurrent ? tr("Caching: %1").arg(songDisplayName(entry.song))
                                         : tr("Caching next: %1").arg(songDisplayName(entry.song)),
                               true);
    }
    Q_EMIT cacheStateChanged(entry.song.value(u"id"_s).toString());
    Q_EMIT downloadProgressChanged();
    startRequest();
}

void SubsonicDownloadManager::startRequest()
{
    if(m_active < 0 || m_cancelActive) {
        return;
    }

    m_requestStart = m_received;
    m_rangeChecked = false;
    m_sentRange    = m_received > 0;

    QNetworkRequest request{m_client->streamUrl(m_entries[m_active].song.value(u"id"_s).toString())};
    if(m_sentRange) {
        request.setRawHeader("Range", "bytes=" + QByteArray::number(m_received) + "-");
    }

    m_reply = m_network->get(request);
    connect(m_reply, &QIODevice::readyRead, this, &SubsonicDownloadManager::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &SubsonicDownloadManager::handleFinished);
}

void SubsonicDownloadManager::handleReadyRead()
{
    if(!m_reply || sender() != m_reply) {
        return;
    }

    // If we asked to resume but the server ignored the Range header, discard the partial file.
    // Only relevant when a Range request was actually sent; m_received alone is >0 during a normal
    // download once the first chunk arrives.
    if(m_sentRange && !m_rangeChecked) {
        m_rangeChecked = true;
        if(m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            if(m_file) {
                m_file->resize(0);
                m_file->seek(0);
            }
            m_received     = 0;
            m_requestStart = 0;
            m_total        = -1;
        }
    }

    if(m_total < 0) {
        const QByteArray contentRange = m_reply->rawHeader("Content-Range");
        const int slash               = contentRange.lastIndexOf('/');
        if(slash >= 0) {
            bool ok{false};
            const qint64 total = contentRange.mid(slash + 1).toLongLong(&ok);
            if(ok) {
                m_total = total;
            }
        }

        if(m_total < 0) {
            bool ok{false};
            const qint64 length = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
            if(ok) {
                m_total = m_requestStart + length;
            }
        }
    }

    const QByteArray data = m_reply->readAll();
    if(data.isEmpty() || !m_file) {
        return;
    }

    if(m_file->write(data) != data.size()) {
        qCWarning(SUBSONIC) << "Failed to write to" << m_file->fileName() << "(disk full?)";
        m_cancelActive = true;
        m_reply->abort();
        return;
    }
    m_received += data.size();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if(now - m_lastProgressEmit >= ProgressEmitIntervalMs) {
        m_lastProgressEmit = now;
        Q_EMIT downloadProgressChanged();
    }
}

void SubsonicDownloadManager::handleFinished()
{
    if(sender() != m_reply) {
        return;
    }

    QNetworkReply* reply = m_reply;
    m_reply              = nullptr;

    if(!reply) {
        return;
    }

    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString               = reply->errorString();
    reply->deleteLater();

    if(m_active < 0) {
        return;
    }

    if(m_cancelActive) {
        m_cancelActive = false;
        finishCurrent(false);
        return;
    }

    const bool complete = (error == QNetworkReply::NoError) && (m_total < 0 ? m_received > 0 : m_received >= m_total);
    if(complete) {
        finishCurrent(true);
        return;
    }

    if(m_attempts < MaxAttempts) {
        ++m_attempts;
        qCWarning(SUBSONIC) << "Download retry" << m_attempts << "at" << m_received << "bytes:"
                            << (error == QNetworkReply::NoError ? u"incomplete"_s : errorString);
        if(m_active >= 0 && m_active < static_cast<int>(m_entries.size())) {
            Q_EMIT activityChanged(tr("Retrying download: %1").arg(songDisplayName(m_entries[m_active].song)), true);
        }
        QTimer::singleShot(RetryDelayMs, this, &SubsonicDownloadManager::startRequest);
        return;
    }

    qCWarning(SUBSONIC) << "Download failed after" << MaxAttempts << "attempts:" << errorString;
    finishCurrent(false);
}

void SubsonicDownloadManager::finishCurrent(bool ok)
{
    if(m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }

    const int index = m_active;
    m_active        = -1;

    if(index >= 0 && index < static_cast<int>(m_entries.size())) {
        Entry& entry = m_entries[index];

        if(ok) {
            entry.done = true;
            qCInfo(SUBSONIC) << "Downloaded" << entry.path << m_received << "bytes";
            Q_EMIT activityChanged(tr("Added to cache: %1").arg(songDisplayName(entry.song)), false);
            embedCover(index);
            evictCacheIfNeeded();
        }
        else {
            if(m_cacheRegistry) {
                m_cacheRegistry->remove(entry.song.value(u"id"_s).toString());
            }
            Q_EMIT activityChanged(tr("Download failed: %1").arg(songDisplayName(entry.song)), false);
            QFile::remove(entry.path);
        }

        Q_EMIT cacheStateChanged(entry.song.value(u"id"_s).toString());

        if(const auto callback = m_callbacks.find(index); callback != m_callbacks.end()) {
            auto function = std::move(callback->second);
            m_callbacks.erase(callback);
            if(function) {
                function(ok);
            }
        }

        if(index == m_gateIndex && m_player) {
            // Resume even on failure so a failed download can't leave playback paused forever.
            m_gateIndex = -1;
            QMetaObject::invokeMethod(
                this, [this] { m_player->play(); }, Qt::QueuedConnection);
        }
    }

    Q_EMIT downloadProgressChanged();
    processNext();
}

void SubsonicDownloadManager::embedCover(int index)
{
    if(index < 0 || index >= static_cast<int>(m_entries.size()) || !m_audioLoader) {
        return;
    }

    const QString songId = m_entries[index].song.value(u"id"_s).toString();
    const QString path   = m_entries[index].path;
    const auto publishToRegistry = [this, songId, path] {
        if(m_cacheRegistry) {
            m_cacheRegistry->set(songId, path);
        }
    };

    const QString coverArt = m_entries[index].song.value(u"coverArt"_s).toString();
    if(coverArt.isEmpty()) {
        publishToRegistry();
        return;
    }

    // fooyin resolves remote artwork (COVERART) only for remote tracks, so once the file is local its
    // cover must be embedded in the file itself; otherwise players fall back to the "no cover" icon.
    m_client->setSettings(loadSubsonicSettings(m_settings));
    QNetworkReply* reply = m_network->get(QNetworkRequest{m_client->coverArtUrl(coverArt, 500)});
    connect(reply, &QNetworkReply::finished, this, [this, index, reply, publishToRegistry] {
        reply->deleteLater();

        if(reply->error() == QNetworkReply::NoError && index >= 0
           && index < static_cast<int>(m_entries.size())) {
            const QByteArray data = reply->readAll();
            const Track track     = trackForEntry(m_entries[index]);
            // Keep any art the file already carries (e.g. a higher-resolution embedded cover).
            if(!data.isEmpty() && m_audioLoader->readTrackCover(track, Track::Cover::Front).isEmpty()) {
                const QString mimeType
                    = imageMimeType(data, reply->header(QNetworkRequest::ContentTypeHeader).toString());
                if(mimeType.isEmpty()) {
                    qCWarning(SUBSONIC) << "Cover art for" << track.filepath() << "is not an image; skipping";
                    publishToRegistry();
                    return;
                }
                TrackCovers covers;
                covers[Track::Cover::Front] = CoverImage{mimeType, data};
                if(!m_audioLoader->writeTrackCover(track, covers, {})) {
                    qCWarning(SUBSONIC) << "Could not embed cover art in" << track.filepath();
                }
            }
        }
        publishToRegistry();
    });
}

void SubsonicDownloadManager::promoteEntryToLocal(int index)
{
    const Entry entry = m_entries[index];
    if(entry.url.isEmpty()) {
        return;
    }

    const Track localTrack = trackForEntry(entry);

    // Run off the network callback: replacing playlist entries or re-committing the current track can
    // trigger signals that re-enter the downloader, so defer to the next event-loop iteration.
    QMetaObject::invokeMethod(
        this,
        [this, entry, localTrack] {
            const bool isCurrent    = m_player && m_player->currentTrack().filepath() == entry.url;
            const uint64_t position = isCurrent ? m_player->currentPosition() : uint64_t{0};
            const uint64_t duration = isCurrent ? m_player->currentTrack().duration() : 0;

            // Repoint every playlist entry at the local file. This is silent, but it must run after the
            // audible commit so fooyin doesn't re-commit the current track on its own (unfaded).
            const auto rewritePlaylists = [this, entry, localTrack] {
                if(!m_handler) {
                    return;
                }
                for(Playlist* playlist : m_handler->playlists()) {
                    PlaylistTrackList playlistTracks = playlist->playlistTracks();
                    bool changed{false};
                    for(PlaylistTrack& playlistTrack : playlistTracks) {
                        if(playlistTrack.track.filepath() == entry.url) {
                            playlistTrack.track = localTrack;
                            changed             = true;
                        }
                    }
                    if(changed) {
                        m_handler->replacePlaylistTracks(playlist->id(), playlistTracks,
                                                         PlaylistTrackChangeSource::External);
                    }
                }
            };

            if(!isCurrent) {
                rewritePlaylists();
                return;
            }

            // Don't cut a track that's about to end; the switch then happens at the natural boundary.
            if(duration > 0 && position + NearEndSkipMs >= duration) {
                qCInfo(SUBSONIC) << "Skipping live cache switch near track end" << entry.path << position
                                 << "/" << duration << "ms";
                rewritePlaylists();
                return;
            }

            PlaylistTrack current = m_player->currentPlaylistTrack();
            if(!current.isValid()) {
                rewritePlaylists();
                return;
            }
            current.track = localTrack;

            // Hand off at the current position; the remote fade (updateSubsonicFade) masks the seam.
            m_pendingSeekPosition = position;
            m_pendingSeekBaseMs   = QDateTime::currentMSecsSinceEpoch();
            m_pendingSeekPath     = localTrack.filepath();
            qCInfo(SUBSONIC) << "Switching current track to cached file" << entry.path << "at" << position << "ms";
            Q_EMIT activityChanged(tr("Switching to cached file: %1").arg(songDisplayName(entry.song)), true);
            m_player->changeCurrentTrack(current, {Player::AdvanceReason::ManualSelection, true});
            rewritePlaylists();
        },
        Qt::QueuedConnection);
}

void SubsonicDownloadManager::switchCurrentToCached()
{
    if(!m_player) {
        return;
    }

    const QString currentPath = m_player->currentTrack().filepath();
    if(!isSubsonicStreamUrl(QUrl{currentPath})) {
        return; // already playing from a local file, or not a Subsonic stream
    }

    for(int i{0}; i < static_cast<int>(m_entries.size()); ++i) {
        if(m_entries[i].url == currentPath && m_entries[i].done) {
            promoteEntryToLocal(i);
            return;
        }
    }
}

void SubsonicDownloadManager::updateSubsonicFade(bool active)
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
        values.manualChange.enabled = true;
        values.manualChange.in      = 150;
        values.manualChange.out     = 150;
        values.autoChange.enabled   = false;
        m_settings->set(crossfadeKey, true);
        m_settings->set(valuesKey, QVariant::fromValue(values));
    }
    else if(m_crossfadeRestore) {
        // Let an in-flight manual/seek fade finish before restoring the user's settings.
        m_crossfadeRestoreTimer->start(1000);
    }
}

QString SubsonicDownloadManager::songIdForTrack(const Track& track) const
{
    const QUrl url{track.filepath()};
    if(isSubsonicStreamUrl(url)) {
        return QUrlQuery{url}.queryItemValue(u"id"_s);
    }
    return isCachePath(track.filepath()) ? songIdFromCachePath(track.filepath()) : QString{};
}

bool SubsonicDownloadManager::isCachePath(const QString& path) const
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    const QString configured = QDir::cleanPath(settings.downloadDir.isEmpty() ? defaultDownloadDir()
                                                                              : settings.downloadDir);
    const QString fallback   = QDir::cleanPath(defaultDownloadDir());
    const QString active     = m_dir.isEmpty() ? QString{} : QDir::cleanPath(m_dir);
    return (!configured.isEmpty() && path.startsWith(configured + u'/'))
        || (!fallback.isEmpty() && path.startsWith(fallback + u'/'))
        || (!active.isEmpty() && path.startsWith(active + u'/'));
}

void SubsonicDownloadManager::startScrobble(const Track& track)
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

void SubsonicDownloadManager::maybeScrobble(uint64_t positionMs)
{
    if(m_scrobbled || m_scrobbleId.isEmpty() || positionMs < m_scrobbleAt) {
        return;
    }

    m_scrobbled = true;
    m_client->scrobble(m_scrobbleId, true);
    qCDebug(SUBSONIC) << "Scrobbled" << m_scrobbleId;
}

void SubsonicDownloadManager::evictCacheIfNeeded()
{
    const int limitMb = loadSubsonicSettings(m_settings).cacheLimitMb;
    if(limitMb <= 0) {
        return;
    }
    const qint64 limit = static_cast<qint64>(limitMb) * 1024 * 1024;

    // Only evict files this manager downloaded. Listing the whole directory could delete unrelated
    // files when the download folder is a shared/misconfigured path.
    QFileInfoList files;
    qint64 total{0};
    for(const Entry& entry : m_entries) {
        if(!entry.done || entry.library) {
            continue; // library downloads are permanent, not part of the cache budget
        }
        const QFileInfo info{entry.path};
        if(!info.exists()) {
            continue;
        }
        files.push_back(info);
        total += info.size();
    }
    if(total <= limit) {
        return;
    }

    std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() < b.lastModified();
    });

    // Protect the file backing whatever is playing right now. The engine may still be playing the
    // remote URL even though the playlist entry already points at the local file.
    QString currentPath = m_player ? m_player->currentTrack().filepath() : QString{};
    for(const Entry& entry : m_entries) {
        if(!currentPath.isEmpty() && entry.url == currentPath) {
            currentPath = entry.path;
            break;
        }
    }
    const QString activePath
        = (m_active >= 0 && m_active < static_cast<int>(m_entries.size())) ? m_entries[m_active].path : QString{};
    for(const QFileInfo& info : std::as_const(files)) {
        if(total <= limit) {
            break;
        }
        if(info.absoluteFilePath() == currentPath || info.absoluteFilePath() == activePath) {
            continue;
        }

        total -= info.size();
        if(!QFile::remove(info.absoluteFilePath())) {
            continue;
        }
        for(Entry& entry : m_entries) {
            if(entry.path == info.absoluteFilePath()) {
                entry.done = false;
                if(m_cacheRegistry) {
                    m_cacheRegistry->remove(entry.song.value(u"id"_s).toString());
                }
                Q_EMIT cacheStateChanged(entry.song.value(u"id"_s).toString());
            }
        }
        qCInfo(SUBSONIC) << "Evicted cached file" << info.absoluteFilePath();
    }
}

Track SubsonicDownloadManager::trackForEntry(const Entry& entry) const
{
    const QString coverArt = entry.song.value(u"coverArt"_s).toString();
    const QString coverUrl
        = coverArt.isEmpty() ? QString{} : m_client->coverArtUrl(coverArt).toString();
    return trackFromSong(entry.song, entry.path, coverUrl);
}

SubsonicDownloadManager::CacheState SubsonicDownloadManager::cacheStateForSong(const QString& songId) const
{
    const int index = indexForSongId(songId);
    if(index < 0) {
        return CacheState::NotCached;
    }
    if(m_entries[index].done) {
        return CacheState::Cached;
    }
    if(index == m_active || index == m_priority || m_requests.contains(index)) {
        return CacheState::Downloading;
    }
    return CacheState::NotCached;
}

bool SubsonicDownloadManager::isDownloadedCopy(const QString& songId) const
{
    const int index = indexForSongId(songId);
    return index >= 0 && m_entries[index].library && m_entries[index].done;
}

QString SubsonicDownloadManager::downloadedCopyPath(const QString& songId) const
{
    const int index = indexForSongId(songId);
    if(index < 0 || !m_entries[index].library || !m_entries[index].done) {
        return {};
    }
    return m_entries[index].path;
}

QString SubsonicDownloadManager::localPathForSong(const QJsonObject& song) const
{
    const QString dir = cacheDir();
    if(dir.isEmpty()) {
        return {};
    }
    const QString path = pathForSong(song, dir);
    const QFileInfo info{path};
    // Cover embedding grows the file, so "at least the expected size" means complete.
    const qint64 expected = static_cast<qint64>(song.value(u"size"_s).toDouble());
    return info.exists() && info.size() > 0 && (expected <= 0 || info.size() >= expected) ? path : QString{};
}

void SubsonicDownloadManager::addDownloadedCopy(const QString& songId, const QJsonObject& song, const QString& path,
                                                bool notify)
{
    if(songId.isEmpty() || path.isEmpty()) {
        return;
    }

    int index = indexForSongId(songId);
    if(index < 0) {
        Entry entry;
        entry.song    = song;
        entry.path    = path;
        entry.done    = true;
        entry.library = true;
        m_entries.push_back(entry);
        index = static_cast<int>(m_entries.size()) - 1;
        m_indexByPath.insert(path, index);
        m_indexById.insert(songId, index);
    }
    else {
        Entry& entry = m_entries[index];
        if(entry.library && entry.path == path) {
            entry.done = true;
            if(notify) {
                Q_EMIT cacheStateChanged(songId);
            }
            return;
        }

        // The local copy supersedes any in-flight cache download of the same song. Abort it first so
        // it cleans up its own cache file before we repoint the entry at the library file.
        if(index == m_active) {
            if(m_reply) {
                m_cancelActive = true;
                m_reply->abort();
            }
            else {
                m_cancelActive = false;
                finishCurrent(false);
            }
        }
        m_requests.erase(index);
        if(m_priority == index) {
            m_priority = -1;
        }
        if(m_gateIndex == index) {
            m_gateIndex = -1;
        }

        m_indexByPath.remove(entry.path);
        entry.song    = song;
        entry.path    = path;
        entry.url.clear();
        entry.done    = true;
        entry.library = true;
        m_indexByPath.insert(path, index);
    }

    if(notify) {
        Q_EMIT cacheStateChanged(songId);
        Q_EMIT downloadProgressChanged();
        if(m_player && songId == songIdForTrack(m_player->currentTrack())) {
            Q_EMIT activityChanged(tr("Playing downloaded copy: %1").arg(songDisplayName(song)), false);
        }
    }
}

int SubsonicDownloadManager::downloadProgressForSong(const QString& songId) const
{
    const int index = indexForSongId(songId);
    if(index < 0) {
        return 0;
    }
    if(m_entries[index].done) {
        return 100;
    }
    if(index != m_active || m_total <= 0) {
        return 0;
    }
    return static_cast<int>(std::clamp((m_received * 100) / m_total, qint64{0}, qint64{100}));
}

QString SubsonicDownloadManager::activeDownloadName() const
{
    if(m_active < 0 || m_active >= static_cast<int>(m_entries.size())) {
        return {};
    }
    const QJsonObject& song = m_entries[m_active].song;
    const QString artist    = song.value(u"artist"_s).toString();
    const QString title     = song.value(u"title"_s).toString();
    return artist.isEmpty() ? title : u"%1 - %2"_s.arg(artist, title);
}

int SubsonicDownloadManager::activeDownloadProgress() const
{
    if(m_active < 0 || m_total <= 0) {
        return 0;
    }
    return static_cast<int>(std::clamp((m_received * 100) / m_total, qint64{0}, qint64{100}));
}

qint64 SubsonicDownloadManager::cacheSizeBytes() const
{
    qint64 total{0};
    const QDir dir{cacheDir()};
    for(const QFileInfo& info : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if(!songIdFromCachePath(info.fileName()).isEmpty()) {
            total += info.size();
        }
    }
    return total;
}

int SubsonicDownloadManager::cachedCount() const
{
    int count{0};
    const QDir dir{cacheDir()};
    for(const QFileInfo& info : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if(!songIdFromCachePath(info.fileName()).isEmpty()) {
            ++count;
        }
    }
    return count;
}

QString SubsonicDownloadManager::cacheDir() const
{
    const QString dir = m_dir.isEmpty() ? loadSubsonicSettings(m_settings).downloadDir : m_dir;
    return dir.isEmpty() ? defaultDownloadDir() : dir;
}

void SubsonicDownloadManager::clearCache()
{
    const QString currentPath = m_player ? m_player->currentTrack().filepath() : QString{};
    const QString activePath
        = (m_active >= 0 && m_active < static_cast<int>(m_entries.size())) ? m_entries[m_active].path : QString{};

    if(m_dir.isEmpty()) {
        m_dir = loadSubsonicSettings(m_settings).downloadDir;
    }

    // Delete every file in the download dir, including files fetched in earlier sessions that are no
    // longer tracked in m_entries. Keep the playing/active files so playback isn't interrupted.
    const QDir dir{m_dir};
    for(const QFileInfo& info : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        const QString path = info.absoluteFilePath();
        if(path == currentPath || path == activePath) {
            continue;
        }
        QFile::remove(path);
        if(m_cacheRegistry) {
            if(const QString id = songIdFromCachePath(info.fileName()); !id.isEmpty()) {
                m_cacheRegistry->remove(id);
            }
        }
    }

    for(Entry& entry : m_entries) {
        if(entry.done && !QFile::exists(entry.path)) {
            entry.done = false;
            Q_EMIT cacheStateChanged(entry.song.value(u"id"_s).toString());
        }
    }
    Q_EMIT downloadProgressChanged();
}

int SubsonicDownloadManager::indexForPath(const QString& path) const
{
    return m_indexByPath.value(path, -1);
}

int SubsonicDownloadManager::indexForSongId(const QString& songId) const
{
    return m_indexById.value(songId, -1);
}

QString SubsonicDownloadManager::pathForSong(const QJsonObject& song, const QString& dir) const
{
    return dir + u"/"_s + subsonicCacheFileName(song);
}
} // namespace Fooyin::Subsonic
