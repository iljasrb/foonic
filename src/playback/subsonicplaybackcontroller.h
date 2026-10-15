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

class QTimer;

namespace Fooyin {
class NetworkAccessManager;
class PlayerController;
class SettingsManager;
class Track;

namespace Subsonic {
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
                               PlayerController* player, QObject* parent = nullptr);
    ~SubsonicPlaybackController() override;

private:
    void updateSubsonicFade(bool active);
    void startScrobble(const Track& track);
    void maybeScrobble(uint64_t positionMs);
    [[nodiscard]] QString songIdForTrack(const Track& track) const;

    std::shared_ptr<NetworkAccessManager> m_network;
    SettingsManager* m_settings;
    SubsonicClient* m_client;
    PlayerController* m_player;

    // fooyin's seek/manual crossfade is gated on Engine/Crossfading (off by default); enable it for
    // remote Subsonic playback and restore the user's values afterwards.
    QTimer* m_crossfadeRestoreTimer{nullptr};
    bool m_crossfadeRestore{false};
    bool m_crossfadeWasEnabled{false};
    QVariant m_crossfadingValuesRestore;

    QString m_scrobbleId;
    uint64_t m_scrobbleAt{0};
    bool m_scrobbled{false};
};
} // namespace Subsonic
} // namespace Fooyin
