/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicdownloadmanager.h"

#include "subsonicclient.h"
#include "subsonicsettings.h"
#include "subsonictrack.h"

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
// Restarting this close to the start of a track is effectively inaudible, so we can skip the seek.
constexpr uint64_t ResumeSeekThresholdMs = 1000;
} // namespace

namespace Fooyin::Subsonic {
SubsonicDownloadManager::SubsonicDownloadManager(std::shared_ptr<NetworkAccessManager> network,
                                                 SettingsManager* settings, PlayerController* player,
                                                 PlaylistHandler* handler, QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
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
        const QString path      = pathForSong(song, m_dir);

        int index = indexForPath(path);
        if(index < 0) {
            Entry entry;
            entry.song = song;
            entry.url  = url;
            entry.path = path;

            const qint64 expected = static_cast<qint64>(song.value(u"size"_s).toDouble());
            const QFileInfo info{entry.path};
            entry.done = expected > 0 && info.exists() && info.size() == expected;

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

void SubsonicDownloadManager::onCurrentTrackChanged(const Track& track)
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);

    // Stream mode never caches, so don't register or download anything (including restored playlists).
    if(settings.playbackMode == PlaybackMode::Stream) {
        m_requests.clear();
        m_priority            = -1;
        m_pendingSeekPosition = 0;
        m_pendingSeekPath.clear();
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
        qCDebug(SUBSONIC) << "Current track already cached" << m_entries[index].path;
        return;
    }

    // When streaming-while-downloading is disabled, hold playback until the file is ready.
    if(!settings.streamWhileDownloading && m_player) {
        m_gateIndex = index;
        m_player->pause();
    }

    qCInfo(SUBSONIC) << "Current track will be cached" << m_entries[index].path;
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
    if(track.duration() > 0) {
        song[u"duration"_s] = static_cast<int>(track.duration() / 1000);
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
    m_pendingSeekPath.clear();
    qCInfo(SUBSONIC) << "Restoring position after promotion:" << position << "ms";
    m_player->seek(position);
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
            promoteEntryToLocal(index);
            evictCacheIfNeeded();
        }
        else {
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
            // Commit the current track before the (potentially slow) playlist rewrite so the engine
            // starts loading the local file as early as possible. A restart near the start of a track
            // is inaudible, so skip the position restore there.
            if(m_player && m_player->currentTrack().filepath() == entry.url) {
                PlaylistTrack current = m_player->currentPlaylistTrack();
                if(current.isValid()) {
                    current.track           = localTrack;
                    const uint64_t position = m_player->currentPosition();

                    if(position > ResumeSeekThresholdMs) {
                        m_pendingSeekPosition = position;
                        m_pendingSeekPath     = localTrack.filepath();
                        qCInfo(SUBSONIC) << "Promoting current track to cached file" << entry.path << "at"
                                         << position << "ms";
                    }
                    else {
                        m_pendingSeekPosition = 0;
                        m_pendingSeekPath.clear();
                        qCInfo(SUBSONIC) << "Promoting current track to cached file" << entry.path
                                         << "near start; restarting without seek";
                    }

                    m_player->changeCurrentTrack(current);
                }
            }

            if(m_handler) {
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
            }
        },
        Qt::QueuedConnection);
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
        if(!entry.done) {
            continue;
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
    for(const Entry& entry : m_entries) {
        if(entry.done) {
            total += QFileInfo{entry.path}.size();
        }
    }
    return total;
}

int SubsonicDownloadManager::cachedCount() const
{
    return static_cast<int>(std::count_if(m_entries.cbegin(), m_entries.cend(),
                                          [](const Entry& entry) { return entry.done; }));
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
    const QString id = song.value(u"id"_s).toString();
    QString name     = song.value(u"artist"_s).toString() + u" - "_s + song.value(u"title"_s).toString();
    name             = name.trimmed();
    if(name.isEmpty()) {
        name = id;
    }

    static const QRegularExpression Invalid{QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])")};
    name.replace(Invalid, u"_"_s);
    name = name.left(180);

    QString suffix = song.value(u"suffix"_s).toString();
    if(suffix.isEmpty()) {
        suffix = u"bin"_s;
    }

    return dir + u"/"_s + name + u" ["_s + id + u"]."_s + suffix;
}
} // namespace Fooyin::Subsonic
