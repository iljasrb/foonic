/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <core/track.h>

#include <QJsonObject>
#include <QString>

#include <algorithm>

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

    const QString date = song.value(QStringLiteral("date")).toString();
    if(!date.isEmpty()) {
        track.setDate(date);
    }

    const QString genre = song.value(QStringLiteral("genre")).toString();
    if(!genre.isEmpty()) {
        track.setGenres({genre});
    }

    const QString composer = song.value(QStringLiteral("composer")).toString();
    if(!composer.isEmpty()) {
        track.setComposers({composer});
    }

    const QString comment = song.value(QStringLiteral("comment")).toString();
    if(!comment.isEmpty()) {
        track.setComment(comment);
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

    const qint64 size = static_cast<qint64>(song.value(QStringLiteral("size")).toDouble());
    if(size > 0) {
        track.setFileSize(static_cast<uint64_t>(size));
    }

    const int sampleRate = song.value(QStringLiteral("samplingRate")).toInt();
    if(sampleRate > 0) {
        track.setSampleRate(sampleRate);
    }

    const int channels = song.value(QStringLiteral("channelCount")).toInt();
    if(channels > 0) {
        track.setChannels(channels);
    }

    const int bitDepth = song.value(QStringLiteral("bitDepth")).toInt();
    if(bitDepth > 0) {
        track.setBitDepth(bitDepth);
    }

    // OpenSubsonic exposes the user's server-side annotation on the song object.
    const int userRating = song.value(QStringLiteral("userRating")).toInt();
    if(userRating > 0) {
        track.setRatingStars(std::clamp(userRating, 0, 5) * 2);
    }
    track.setLoved(!song.value(QStringLiteral("starred")).toString().isEmpty());

    if(!coverUrl.isEmpty()) {
        track.replaceExtraTag(QStringLiteral("COVERART"), coverUrl);
    }

    track.setMetadataWasRead(true);
    return track;
}
} // namespace Fooyin::Subsonic
