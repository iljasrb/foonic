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

struct SubsonicSettings
{
    QString server;
    QString username;
    QString password;
    QString authMode{QStringLiteral("token")}; // "token", "password", "apiKey"
    QString apiKey;
    QString format{QStringLiteral("raw")}; // "raw", "mp3", "opus", "aac"
    int maxBitRate{0};                     // kbps, 0 = unlimited (only used when transcoding)
    bool ignoreSslErrors{false};           // accept self-signed/invalid TLS certificates
    QString musicFolderId;             // limit browsing/search to one server folder; empty = all
    QString libraryDir;                // albums/songs downloaded here are added to the fooyin library
    int streamBufferMb{64};            // in-memory read buffer for remote streams (whole track if size fits)
    int remotePrebufferMs{400};        // decoded audio the engine wants buffered before resuming after a seek
    bool seekFadeEnabled{true};        // fade out/in around remote seeks
    int seekFadeMs{200};               // seek fade length
    int coverArtSize{300};             // cover art requested from the server, in px
    bool scrobblePlays{true};          // report now-playing + play counts back to the server

    [[nodiscard]] bool isValid() const
    {
        if(server.trimmed().isEmpty()) {
            return false;
        }
        if(authMode == QStringLiteral("apiKey")) {
            return !apiKey.trimmed().isEmpty();
        }
        return !username.trimmed().isEmpty();
    }
};

inline QString defaultLibraryDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if(base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + QStringLiteral("/Music");
    }
    return base + QStringLiteral("/Subsonic");
}

inline void registerSubsonicSettings(SettingsManager* settings)
{
    // ponytail: password stored in plain text in fooyin's config; move to a keyring if that ever matters.
    settings->createSetting(QStringLiteral("Subsonic/Server"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/Username"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/Password"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/AuthMode"), QStringLiteral("token"));
    settings->createSetting(QStringLiteral("Subsonic/ApiKey"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/Format"), QStringLiteral("raw"));
    settings->createSetting(QStringLiteral("Subsonic/MaxBitRate"), 0);
    settings->createSetting(QStringLiteral("Subsonic/IgnoreSslErrors"), false);
    settings->createSetting(QStringLiteral("Subsonic/MusicFolderId"), QString{});
    settings->createSetting(QStringLiteral("Subsonic/LibraryDir"), defaultLibraryDir());
    settings->createSetting(QStringLiteral("Subsonic/StreamBufferMb"), 64);
    settings->createSetting(QStringLiteral("Subsonic/RemotePrebufferMs"), 400);
    settings->createSetting(QStringLiteral("Subsonic/SeekFadeEnabled"), true);
    settings->createSetting(QStringLiteral("Subsonic/SeekFadeMs"), 200);
    settings->createSetting(QStringLiteral("Subsonic/CoverArtSize"), 300);
    settings->createSetting(QStringLiteral("Subsonic/ScrobblePlays"), true);
    settings->createSetting(QStringLiteral("Subsonic/ArtistSort"), 0);
    settings->createSetting(QStringLiteral("Subsonic/AlbumSort"), 0);
    settings->createSetting(QStringLiteral("Subsonic/PlaylistSort"), 0);
}

inline SubsonicSettings loadSubsonicSettings(SettingsManager* settings)
{
    SubsonicSettings result;
    result.server       = settings->value(QStringLiteral("Subsonic/Server")).toString();
    result.username     = settings->value(QStringLiteral("Subsonic/Username")).toString();
    result.password     = settings->value(QStringLiteral("Subsonic/Password")).toString();
    result.authMode     = settings->value(QStringLiteral("Subsonic/AuthMode")).toString();
    if(result.authMode.isEmpty()) {
        result.authMode = QStringLiteral("token");
    }
    result.apiKey       = settings->value(QStringLiteral("Subsonic/ApiKey")).toString();
    result.format       = settings->value(QStringLiteral("Subsonic/Format")).toString();
    result.maxBitRate   = settings->value(QStringLiteral("Subsonic/MaxBitRate")).toInt();
    if(result.format.isEmpty()) {
        result.format = QStringLiteral("raw");
    }
    result.ignoreSslErrors = settings->value(QStringLiteral("Subsonic/IgnoreSslErrors")).toBool();
    result.musicFolderId   = settings->value(QStringLiteral("Subsonic/MusicFolderId")).toString();
    result.libraryDir      = settings->value(QStringLiteral("Subsonic/LibraryDir")).toString();
    if(result.libraryDir.isEmpty()) {
        result.libraryDir = defaultLibraryDir();
    }
    result.streamBufferMb    = std::clamp(settings->value(QStringLiteral("Subsonic/StreamBufferMb")).toInt(), 8, 1024);
    result.remotePrebufferMs = std::clamp(settings->value(QStringLiteral("Subsonic/RemotePrebufferMs")).toInt(), 100, 10000);
    result.seekFadeEnabled   = settings->value(QStringLiteral("Subsonic/SeekFadeEnabled")).toBool();
    result.seekFadeMs        = std::clamp(settings->value(QStringLiteral("Subsonic/SeekFadeMs")).toInt(), 0, 5000);
    result.coverArtSize      = std::clamp(settings->value(QStringLiteral("Subsonic/CoverArtSize")).toInt(), 64, 2000);
    result.scrobblePlays     = settings->value(QStringLiteral("Subsonic/ScrobblePlays")).toBool();
    return result;
}

inline void saveSubsonicSettings(SettingsManager* settings, const SubsonicSettings& subsonic)
{
    settings->set(QStringLiteral("Subsonic/Server"), subsonic.server);
    settings->set(QStringLiteral("Subsonic/Username"), subsonic.username);
    settings->set(QStringLiteral("Subsonic/Password"), subsonic.password);
    settings->set(QStringLiteral("Subsonic/AuthMode"), subsonic.authMode);
    settings->set(QStringLiteral("Subsonic/ApiKey"), subsonic.apiKey);
    settings->set(QStringLiteral("Subsonic/Format"), subsonic.format);
    settings->set(QStringLiteral("Subsonic/MaxBitRate"), subsonic.maxBitRate);
    settings->set(QStringLiteral("Subsonic/IgnoreSslErrors"), subsonic.ignoreSslErrors);
    settings->set(QStringLiteral("Subsonic/MusicFolderId"), subsonic.musicFolderId);
    settings->set(QStringLiteral("Subsonic/LibraryDir"), subsonic.libraryDir);
    settings->set(QStringLiteral("Subsonic/StreamBufferMb"), subsonic.streamBufferMb);
    settings->set(QStringLiteral("Subsonic/RemotePrebufferMs"), subsonic.remotePrebufferMs);
    settings->set(QStringLiteral("Subsonic/SeekFadeEnabled"), subsonic.seekFadeEnabled);
    settings->set(QStringLiteral("Subsonic/SeekFadeMs"), subsonic.seekFadeMs);
    settings->set(QStringLiteral("Subsonic/CoverArtSize"), subsonic.coverArtSize);
    settings->set(QStringLiteral("Subsonic/ScrobblePlays"), subsonic.scrobblePlays);
}
} // namespace Fooyin::Subsonic
