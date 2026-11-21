/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <core/track.h>

#include <QJsonObject>
#include <QString>

namespace Fooyin::Subsonic {
/*! Builds a fooyin Track from a Subsonic song object and the filepath/URL to play it from. */
inline Track trackFromSong(const QJsonObject& song, const QString& filepath, const QString& coverUrl = {})
{
    Track track{filepath};

    track.setTitle(song.value(QStringLiteral("title")).toString());

    const QString artist = song.value(QStringLiteral("artist")).toString();
    if(!artist.isEmpty()) {
        track.setArtists({artist});
    }

    const QString album = song.value(QStringLiteral("album")).toString();
    if(!album.isEmpty()) {
        track.setAlbum(album);
    }

    const QString albumArtist = song.value(QStringLiteral("albumArtist")).toString();
    if(!albumArtist.isEmpty()) {
        track.setAlbumArtists({albumArtist});
    }

    const int trackNumber = song.value(QStringLiteral("track")).toInt();
    if(trackNumber > 0) {
        track.setTrackNumber(QString::number(trackNumber));
    }

    const int discNumber = song.value(QStringLiteral("discNumber")).toInt();
    if(discNumber > 0) {
        track.setDiscNumber(QString::number(discNumber));
    }

    const int year = song.value(QStringLiteral("year")).toInt();
    if(year > 0) {
        track.setYear(year);
    }

    const QString genre = song.value(QStringLiteral("genre")).toString();
    if(!genre.isEmpty()) {
        track.setGenres({genre});
    }

    const int duration = song.value(QStringLiteral("duration")).toInt();
    if(duration > 0) {
        track.setDuration(static_cast<uint64_t>(duration) * 1000);
    }

    const int bitrate = song.value(QStringLiteral("bitRate")).toInt();
    if(bitrate > 0) {
        track.setBitrate(bitrate);
    }

    const QString codec = song.value(QStringLiteral("suffix")).toString();
    if(!codec.isEmpty()) {
        track.setCodec(codec.toUpper());
    }

    if(!coverUrl.isEmpty()) {
        track.replaceExtraTag(QStringLiteral("COVERART"), coverUrl);
    }

    track.setMetadataWasRead(true);
    return track;
}
} // namespace Fooyin::Subsonic
