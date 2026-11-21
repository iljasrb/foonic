/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <memory>

namespace Fooyin {
class AudioLoader;
class NetworkAccessManager;
class SettingsManager;

namespace Subsonic {
class SessionCache;
class SubsonicBufferMonitor;

/*!
 * Makes Subsonic HTTP streams seekable/pausable.
 *
 * fooyin forces remote (http/https) tracks to be non-seekable, and its default remote device is
 * sequential. This installs a seekable HTTP-Range device through a custom RemoteSourceProvider and
 * registers a decoder that wraps fooyin's own FFmpeg decoder, handing it that device plus a
 * non-remote track copy so the "remote => not seekable" rule is not applied.
 *
 * Devices and the next-track prefetcher share @p cache, so a track is downloaded once and a
 * prefetched next track starts without buffering. Returns the monitor that devices publish to.
 */
std::shared_ptr<SubsonicBufferMonitor> registerSubsonicRemotePlayback(
    const std::shared_ptr<AudioLoader>& loader, std::shared_ptr<NetworkAccessManager> network,
    SettingsManager* settings, std::shared_ptr<SessionCache> cache);
} // namespace Subsonic
} // namespace Fooyin
