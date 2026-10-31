/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicplaybackcontroller.h"

#include "subsonicclient.h"
#include "subsonicbufferbar.h"
#include "subsonicsettings.h"
#include "subsonicstreamutils.h"

#include <core/engine/fadingdefs.h>
#include <core/network/networkaccessmanager.h>
#include <core/player/playercontroller.h>
#include <core/track.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
SubsonicPlaybackController::SubsonicPlaybackController(std::shared_ptr<NetworkAccessManager> network,
                                                       SettingsManager* settings, PlayerController* player,
                                                       std::shared_ptr<SubsonicBufferMonitor> bufferMonitor,
                                                       QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
    , m_settings{settings}
    , m_client{new SubsonicClient{m_network, loadSubsonicSettings(settings), this}}
    , m_player{player}
    , m_bufferMonitor{std::move(bufferMonitor)}
{
    if(!m_player) {
        return;
    }

    connect(m_player, &PlayerController::currentTrackChanged, this, [this](const Track& track) {
        cancelPrefetch();
        m_pendingPrefetchId.clear();
        if(m_bufferMonitor) {
            m_bufferMonitor->reset();
        }
        updateSubsonicFade(track.isRemote() && isSubsonicStreamUrl(QUrl{track.filepath()}));
        startScrobble(track);
    });
    connect(m_player, &PlayerController::upcomingTrackChanged, this,
            [this](const Player::UpcomingTrack& upcoming) {
                const QUrl url{upcoming.track.track.filepath()};
                m_pendingPrefetchId = isSubsonicStreamUrl(url) ? QUrlQuery{url}.queryItemValue(u"id"_s) : QString{};
                if(m_pendingPrefetchId.isEmpty()) {
                    cancelPrefetch();
                }
                else {
                    if(!m_prefetchId.isEmpty() && m_prefetchId != m_pendingPrefetchId) {
                        cancelPrefetch(); // the upcoming track changed while it was downloading
                    }
                    tryPrefetch();
                }
            });
    if(m_bufferMonitor) {
        connect(m_bufferMonitor.get(), &SubsonicBufferMonitor::rangeChanged, this,
                &SubsonicPlaybackController::tryPrefetch);
    }
    connect(m_player, &PlayerController::positionChanged, this,
            [this](uint64_t ms) { maybeScrobble(ms); });
    connect(m_player, &PlayerController::playStateChanged, this,
            [this](Player::PlayState state, Player::PlayState) {
                if(state == Player::PlayState::Stopped && m_bufferMonitor) {
                    m_bufferMonitor->reset();
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
    cancelPrefetch();

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

void SubsonicPlaybackController::tryPrefetch()
{
    if(!m_player || m_pendingPrefetchId.isEmpty() || m_prefetchReply || !m_bufferMonitor) {
        return;
    }
    // Only once the current track is fully downloaded, so the prefetch cannot slow its buffering.
    if(m_bufferMonitor->end() < 0.999) {
        return;
    }
    if(m_pendingPrefetchId == m_prefetchId || m_pendingPrefetchId == songIdForTrack(m_player->currentTrack())
       || QFileInfo::exists(prefetchPath(m_pendingPrefetchId))) {
        return;
    }

    startPrefetch(m_pendingPrefetchId);
}

void SubsonicPlaybackController::startPrefetch(const QString& songId)
{
    QDir{}.mkpath(prefetchDir());

    // Keep at most the current track's file and the one being prefetched.
    const QString currentId = songIdForTrack(m_player->currentTrack());
    const QDir dir{prefetchDir()};
    for(const QString& name : dir.entryList(QDir::Files)) {
        const QString id = name.section(u'.', 0, 0);
        if(id != songId && id != currentId) {
            QFile::remove(dir.filePath(name));
        }
    }

    const QString partPath = prefetchDir() + u'/' + songId + u".part"_s;
    QFile::remove(partPath);

    auto* file = new QFile{partPath, this};
    if(!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file->deleteLater();
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    QNetworkRequest request{m_client->streamUrl(songId)};
    request.setPriority(QNetworkRequest::LowPriority); // never let it compete with the playing track
    auto* reply = m_network->get(request);
    m_prefetchReply = reply;
    m_prefetchId    = songId;

    connect(reply, &QIODevice::readyRead, this, [reply, file] {
        if(file->isOpen()) {
            file->write(reply->readAll());
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, songId, partPath] {
        reply->deleteLater();
        if(file->isOpen()) {
            file->close();
        }
        file->deleteLater();

        const bool ok = reply->error() == QNetworkReply::NoError && QFileInfo{partPath}.size() > 0;
        if(ok) {
            QFile::remove(prefetchPath(songId));
            if(!QFile::rename(partPath, prefetchPath(songId))) {
                QFile::remove(partPath);
            }
            qCDebug(SUBSONIC) << "Prefetched" << songId;
        }
        else {
            QFile::remove(partPath);
            qCDebug(SUBSONIC) << "Prefetch failed for" << songId << reply->errorString();
        }

        if(m_prefetchReply == reply) {
            m_prefetchReply = nullptr;
            m_prefetchId.clear();
        }
    });
}

void SubsonicPlaybackController::cancelPrefetch()
{
    m_prefetchId.clear();
    if(!m_prefetchReply) {
        return;
    }

    auto* reply = m_prefetchReply;
    m_prefetchReply = nullptr;
    reply->abort();
    reply->deleteLater();
}
} // namespace Fooyin::Subsonic
