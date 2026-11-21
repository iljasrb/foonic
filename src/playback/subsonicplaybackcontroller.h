/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <memory>

class QNetworkReply;
class QTimer;

namespace Fooyin {
class NetworkAccessManager;
class PlayerController;
class SettingsManager;
class Track;

namespace Subsonic {
class CacheEntry;
class SessionCache;
class SubsonicBufferMonitor;
class SubsonicClient;

/*!
 * Playback-side companion for remote Subsonic streams: scrobbling, the seek-fade override, the
 * immediate seekbar refresh and next-track prefetching into the shared session cache.
 */
class SubsonicPlaybackController : public QObject
{
    Q_OBJECT

public:
    SubsonicPlaybackController(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                               PlayerController* player, std::shared_ptr<SubsonicBufferMonitor> bufferMonitor,
                               std::shared_ptr<SessionCache> cache, QObject* parent = nullptr);
    ~SubsonicPlaybackController() override;

private:
    void updateSubsonicFade(bool active);
    void startScrobble(const Track& track);
    void maybeScrobble(uint64_t positionMs);
    [[nodiscard]] QString songIdForTrack(const Track& track) const;

    void updatePinnedTracks();
    [[nodiscard]] bool hasComfortableLead() const;
    void tryPrefetch();
    void startPrefetch(const QString& songId, bool probe);
    void startPrefetchRequest();
    void readPrefetchReply(QNetworkReply* reply);
    void updatePrefetchHeaders(QNetworkReply* reply);
    void finishPrefetch(QNetworkReply* reply);
    void cancelPrefetch(bool discard);
    void resetPrefetchState();

    std::shared_ptr<NetworkAccessManager> m_network;
    SettingsManager* m_settings;
    SubsonicClient* m_client;
    PlayerController* m_player;
    std::shared_ptr<SubsonicBufferMonitor> m_bufferMonitor;
    std::shared_ptr<SessionCache> m_cache;

    // fooyin's seek/manual crossfade is gated on Engine/Crossfading (off by default); enable it for
    // remote Subsonic playback and restore the user's values afterwards.
    QTimer* m_crossfadeRestoreTimer{nullptr};
    bool m_crossfadeRestore{false};
    bool m_crossfadeWasEnabled{false};
    QVariant m_crossfadingValuesRestore;

    QString m_scrobbleId;
    uint64_t m_scrobbleAt{0};
    bool m_scrobbled{false};

    QString m_currentId;
    QString m_nextId;
    QSet<QString> m_pinnedIds;

    // Next track downloading into the same cache entry that playback will use.
    QNetworkReply* m_prefetchReply{nullptr};
    std::shared_ptr<CacheEntry> m_prefetchEntry;
    QString m_prefetchId;
    QString m_prefetchFailedId;
    bool m_prefetchProbe{false};
    int m_prefetchAttempts{0};
    int m_prefetchGeneration{0};
    qint64 m_prefetchRequestedStart{-1};
    qint64 m_prefetchActiveLimit{-1};
    qint64 m_prefetchWritePos{-1};
    qint64 m_prefetchResponseStart{-1};
    qint64 m_prefetchResponseEnd{-1};
    qint64 m_prefetchTotal{-1};
    bool m_prefetchHeaders{false};
    bool m_prefetchFullResponse{false};
    bool m_prefetchOpenEnded{false};
    bool m_prefetchEstimateTotal{false};
    bool m_prefetchFatalError{false};
    bool m_prefetchEof{false};
    QString m_prefetchError;
};
} // namespace Subsonic
} // namespace Fooyin
