/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
inline constexpr int MaxNameLength = 120;

//! Makes a server-provided name safe to use as a single path component.
inline QString sanitisePathComponent(QString name)
{
    static const QRegularExpression Invalid{QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])")};
    name.replace(Invalid, u"_"_s);
    name = name.trimmed();
    // Windows rejects components ending in a dot or space; trim them on every platform for portability.
    while(name.endsWith(u'.') || name.endsWith(u' ')) {
        name.chop(1);
    }
    return name.left(MaxNameLength);
}

inline QString withIdSuffix(const QString& path, const QString& id)
{
    const QFileInfo info{path};
    return info.path() + u"/"_s + info.completeBaseName() + u" ["_s + id + u"]"_s
           + (info.suffix().isEmpty() ? QString{} : u"."_s + info.suffix());
}

//! AlbumArtist/Album[/CDn]/NN - Title.ext, matching the downloader's layout.
inline QString subsonicSongPath(const QJsonObject& song, const QString& dir, bool includeDisc = true)
{
    QString albumArtist = song.value(u"albumArtist"_s).toString();
    if(albumArtist.isEmpty()) {
        albumArtist = song.value(u"artist"_s).toString();
    }
    if(albumArtist.isEmpty()) {
        albumArtist = u"Unknown Artist"_s;
    }

    QString album = song.value(u"album"_s).toString();
    if(album.isEmpty()) {
        album = u"Unknown Album"_s;
    }

    QString title = song.value(u"title"_s).toString();
    if(title.isEmpty()) {
        title = song.value(u"id"_s).toString();
    }

    const int discNumber = song.value(u"discNumber"_s).toInt();
    const int discTotal  = song.value(u"discTotal"_s).toInt();
    const bool multiDisc = discTotal > 1 || discNumber > 1;

    const int trackNumber = song.value(u"track"_s).toInt();
    if(trackNumber > 0) {
        title = QStringLiteral("%1 - %2").arg(trackNumber, 2, 10, QLatin1Char('0')).arg(title);
    }

    QString suffix = song.value(u"suffix"_s).toString();
    if(suffix.isEmpty()) {
        suffix = u"bin"_s;
    }

    QString path = dir + u'/' + sanitisePathComponent(albumArtist) + u'/' + sanitisePathComponent(album);
    if(multiDisc && includeDisc) {
        path += u"/CD"_s + QString::number(std::max(1, discNumber));
    }
    return path + u'/' + sanitisePathComponent(title) + u'.' + suffix;
}
} // namespace Fooyin::Subsonic
