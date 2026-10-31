/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
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
class SubsonicBufferMonitor;
class SubsonicClient;

/*!
 * Playback-side companion for remote Subsonic streams: scrobbling, the seek-fade override and the
 * immediate seekbar refresh. Seeking/pausing the stream itself lives in SubsonicStreamDevice.
 */
class SubsonicPlaybackController : public QObject
{
    Q_OBJECT

public:
    SubsonicPlaybackController(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                               PlayerController* player, std::shared_ptr<SubsonicBufferMonitor> bufferMonitor,
                               QObject* parent = nullptr);
    ~SubsonicPlaybackController() override;

private:
    void updateSubsonicFade(bool active);
    void startScrobble(const Track& track);
    void maybeScrobble(uint64_t positionMs);
    [[nodiscard]] QString songIdForTrack(const Track& track) const;
    void tryPrefetch();
    void startPrefetch(const QString& songId);
    void cancelPrefetch();

    std::shared_ptr<NetworkAccessManager> m_network;
    SettingsManager* m_settings;
    SubsonicClient* m_client;
    PlayerController* m_player;
    std::shared_ptr<SubsonicBufferMonitor> m_bufferMonitor;

    // fooyin's seek/manual crossfade is gated on Engine/Crossfading (off by default); enable it for
    // remote Subsonic playback and restore the user's values afterwards.
    QTimer* m_crossfadeRestoreTimer{nullptr};
    bool m_crossfadeRestore{false};
    bool m_crossfadeWasEnabled{false};
    QVariant m_crossfadingValuesRestore;

    QString m_scrobbleId;
    uint64_t m_scrobbleAt{0};
    bool m_scrobbled{false};

    // Next track pre-downloaded to disk once the current one is fully buffered.
    QNetworkReply* m_prefetchReply{nullptr};
    QString m_pendingPrefetchId;
    QString m_prefetchId;
};
} // namespace Subsonic
} // namespace Fooyin
