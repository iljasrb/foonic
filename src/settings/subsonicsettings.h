/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <utils/settings/settingsmanager.h>

#include <QLoggingCategory>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(SUBSONIC)

namespace Fooyin::Subsonic {
constexpr auto SettingsPageId = "Fooyin.Subsonic.Settings";

inline QString formatBytes(qint64 bytes)
{
    if(bytes <= 0) {
        return {};
    }

    static const QStringList Units{QStringLiteral("B"), QStringLiteral("kB"), QStringLiteral("MB"),
                                   QStringLiteral("GB"), QStringLiteral("TB")};
    double value = static_cast<double>(bytes);
    int unit{0};
    while(value >= 1024.0 && unit < Units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }

    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 2).arg(Units.at(unit));
}

enum class PlaybackMode : uint8_t
{
    Stream = 0,
    DownloadFirst,
};

struct SubsonicSettings
{
    QString server;
    QString username;
    QString password;
    PlaybackMode playbackMode{PlaybackMode::DownloadFirst};
    QString format{QStringLiteral("raw")}; // "raw", "mp3", "opus", "aac"
    int maxBitRate{0};                     // kbps, 0 = unlimited (only used when transcoding)
    QString downloadDir;
    bool streamWhileDownloading{true}; // play immediately and switch to the cache at the same position
    int prefetchCount{1};              // upcoming tracks to fetch ahead of playback
    int cacheLimitMb{0};               // maximum cache size, 0 = unlimited
    bool ignoreSslErrors{false};       // accept self-signed/invalid TLS certificates
    QString musicFolderId;             // limit browsing/search to one server folder; empty = all

    [[nodiscard]] bool isValid() const
    {
        return !server.trimmed().isEmpty() && !username.trimmed().isEmpty();
    }
};

inline QString defaultDownloadDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/subsonic");
}

inline void registerSubsonicSettings(SettingsManager* settings)
{
    // ponytail: password stored in plain text in fooyin's config; move to a keyring if that ever matters.
    settings->createSetting(QStringLiteral("Subsonic/Server"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/Username"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/Password"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/PlaybackMode"), static_cast<int>(PlaybackMode::DownloadFirst));
    settings->createSetting(QStringLiteral("Subsonic/Format"), QStringLiteral("raw"));
    settings->createSetting(QStringLiteral("Subsonic/MaxBitRate"), 0);
    settings->createSetting(QStringLiteral("Subsonic/DownloadDir"), defaultDownloadDir());
    settings->createSetting(QStringLiteral("Subsonic/StreamWhileDownloading"), true);
    settings->createSetting(QStringLiteral("Subsonic/PrefetchCount"), 1);
    settings->createSetting(QStringLiteral("Subsonic/CacheLimitMb"), 0);
    settings->createSetting(QStringLiteral("Subsonic/IgnoreSslErrors"), false);
    settings->createSetting(QStringLiteral("Subsonic/MusicFolderId"), QString{});
}

inline SubsonicSettings loadSubsonicSettings(SettingsManager* settings)
{
    SubsonicSettings result;
    result.server       = settings->value(QStringLiteral("Subsonic/Server")).toString();
    result.username     = settings->value(QStringLiteral("Subsonic/Username")).toString();
    result.password     = settings->value(QStringLiteral("Subsonic/Password")).toString();
    result.playbackMode
        = static_cast<PlaybackMode>(settings->value(QStringLiteral("Subsonic/PlaybackMode")).toInt());
    result.format     = settings->value(QStringLiteral("Subsonic/Format")).toString();
    result.maxBitRate = settings->value(QStringLiteral("Subsonic/MaxBitRate")).toInt();
    result.downloadDir = settings->value(QStringLiteral("Subsonic/DownloadDir")).toString();
    if(result.downloadDir.isEmpty()) {
        result.downloadDir = defaultDownloadDir();
    }
    if(result.format.isEmpty()) {
        result.format = QStringLiteral("raw");
    }
    result.streamWhileDownloading = settings->value(QStringLiteral("Subsonic/StreamWhileDownloading")).toBool();
    result.prefetchCount = std::max(0, settings->value(QStringLiteral("Subsonic/PrefetchCount")).toInt());
    result.cacheLimitMb  = std::max(0, settings->value(QStringLiteral("Subsonic/CacheLimitMb")).toInt());
    result.ignoreSslErrors = settings->value(QStringLiteral("Subsonic/IgnoreSslErrors")).toBool();
    result.musicFolderId   = settings->value(QStringLiteral("Subsonic/MusicFolderId")).toString();
    return result;
}

inline void saveSubsonicSettings(SettingsManager* settings, const SubsonicSettings& subsonic)
{
    settings->set(QStringLiteral("Subsonic/Server"), subsonic.server);
    settings->set(QStringLiteral("Subsonic/Username"), subsonic.username);
    settings->set(QStringLiteral("Subsonic/Password"), subsonic.password);
    settings->set(QStringLiteral("Subsonic/PlaybackMode"), static_cast<int>(subsonic.playbackMode));
    settings->set(QStringLiteral("Subsonic/Format"), subsonic.format);
    settings->set(QStringLiteral("Subsonic/MaxBitRate"), subsonic.maxBitRate);
    settings->set(QStringLiteral("Subsonic/DownloadDir"), subsonic.downloadDir);
    settings->set(QStringLiteral("Subsonic/StreamWhileDownloading"), subsonic.streamWhileDownloading);
    settings->set(QStringLiteral("Subsonic/PrefetchCount"), subsonic.prefetchCount);
    settings->set(QStringLiteral("Subsonic/CacheLimitMb"), subsonic.cacheLimitMb);
    settings->set(QStringLiteral("Subsonic/IgnoreSslErrors"), subsonic.ignoreSslErrors);
    settings->set(QStringLiteral("Subsonic/MusicFolderId"), subsonic.musicFolderId);
}
} // namespace Fooyin::Subsonic
