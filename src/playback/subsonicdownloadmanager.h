/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

class QFile;
class QNetworkReply;
class QTimer;

namespace Fooyin {
class AudioLoader;
class NetworkAccessManager;
class PlayerController;
class PlaylistHandler;
class SettingsManager;
class Track;

namespace Subsonic {
class SubsonicClient;

/*!
 * Fetches Subsonic tracks into a local cache in the background.
 *
 * Playlist entries start as remote stream URLs (always playable) and are promoted to the cached
 * local file once downloaded so they become seekable. Downloads are limited to playback order
 * (current + a configurable number ahead), hidden, and retried with an HTTP Range header so
 * servers/proxies that cut long-lived streams don't stop them.
 */
class SubsonicDownloadManager : public QObject
{
    Q_OBJECT

public:
    enum class CacheState : uint8_t
    {
        NotCached = 0,
        Downloading,
        Cached,
    };
    Q_ENUM(CacheState)

    SubsonicDownloadManager(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                            PlayerController* player, PlaylistHandler* handler,
                            std::shared_ptr<AudioLoader> audioLoader, QObject* parent = nullptr);
    ~SubsonicDownloadManager() override;

    /*!
     * Registers @p songs for caching. @p urls are the remote stream URLs used for the corresponding
     * playlist entries. Returns the local cache paths.
     */
    QStringList registerSongs(const QList<QJsonObject>& songs, const QStringList& urls, const QString& dir);

    /*! Ensures the track with @p songId is cached, calling @p onDone when it is (or fails). */
    void ensureDownloaded(const QString& songId, std::function<void(bool)> onDone = {});

    /*! Re-applies the current playback mode to the current track (stop caching, or start caching it). */
    void applyPlaybackMode();

    [[nodiscard]] CacheState cacheStateForSong(const QString& songId) const;

    /*! True when @p songId has a permanent local copy downloaded into the music library. */
    [[nodiscard]] bool isDownloadedCopy(const QString& songId) const;

    /*! Path of the permanent local library copy for @p songId, or an empty string. */
    [[nodiscard]] QString downloadedCopyPath(const QString& songId) const;

    /*!
     * Records a music-library download of @p songId at @p path so playback uses the local file and the
     * browser can mark it as a downloaded copy. Supersedes any in-flight cache download of the song.
     */
    void addDownloadedCopy(const QString& songId, const QJsonObject& song, const QString& path,
                           bool notify = true);
    [[nodiscard]] int downloadProgressForSong(const QString& songId) const;
    [[nodiscard]] QString activeDownloadName() const;
    [[nodiscard]] int activeDownloadProgress() const;
    [[nodiscard]] qint64 cacheSizeBytes() const;
    [[nodiscard]] int cachedCount() const;

    /*! Deletes every cached file not currently playing. */
    void clearCache();

Q_SIGNALS:
    void cacheStateChanged(const QString& songId);
    void downloadProgressChanged();

private:
    struct Entry
    {
        QJsonObject song;
        QString url;
        QString path;
        bool done{false};
        bool library{false}; // permanent music-library download; never evicted
    };

    void onCurrentTrackChanged(const Track& track);
    void cancelDownloads();
    void tryApplyPendingSeek();
    void request(int index, bool priority = false);
    void processNext();
    void startRequest();
    void handleReadyRead();
    void handleFinished();
    void finishCurrent(bool ok);
    void embedCoverThenPromote(int index);
    void promoteEntryToLocal(int index);
    void promoteAtQuietPoint(const Track& localTrack, const QString& remoteUrl, uint64_t position,
                             std::function<void()> afterCommit);
    void findQuietPoint(const Track& track, uint64_t startMs, std::function<void(uint64_t)> done);
    void enableCrossfadeForSwitch();
    void evictCacheIfNeeded();
    [[nodiscard]] Track trackForEntry(const Entry& entry) const;
    [[nodiscard]] int indexForPath(const QString& path) const;
    [[nodiscard]] int indexForSongId(const QString& songId) const;
    [[nodiscard]] int registerRemoteTrack(const Track& track);
    [[nodiscard]] QString pathForSong(const QJsonObject& song, const QString& dir) const;

    std::shared_ptr<NetworkAccessManager> m_network;
    std::shared_ptr<AudioLoader> m_audioLoader;
    SettingsManager* m_settings;
    SubsonicClient* m_client;
    PlayerController* m_player;
    PlaylistHandler* m_handler;
    QString m_dir;
    std::vector<Entry> m_entries;
    QHash<QString, int> m_indexById;   // song id -> m_entries index
    QHash<QString, int> m_indexByPath; // cache path -> m_entries index
    std::set<int> m_requests;
    std::map<int, std::function<void(bool)>> m_callbacks;
    int m_active{-1};
    int m_priority{-1};
    int m_gateIndex{-1};
    bool m_cancelActive{false};
    uint64_t m_pendingSeekPosition{0};
    qint64 m_pendingSeekBaseMs{0};
    QString m_pendingSeekPath;

    QFile* m_file{nullptr};
    QNetworkReply* m_reply{nullptr};
    qint64 m_received{0};
    qint64 m_requestStart{0};
    qint64 m_total{-1};
    int m_attempts{0};
    bool m_rangeChecked{false};
    bool m_sentRange{false};
    qint64 m_lastProgressEmit{0};
    int m_promoteToken{0};
    // fooyin's manual-change crossfade is gated on Engine/Crossfading (default off); enable it just
    // around a cache switch and restore the user's value afterwards.
    QTimer* m_crossfadeRestoreTimer{nullptr};
    bool m_crossfadeRestore{false};
    bool m_crossfadeWasEnabled{false};
    QVariant m_crossfadingValuesRestore;
};
} // namespace Subsonic
} // namespace Fooyin
