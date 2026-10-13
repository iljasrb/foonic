/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Self-check for the Subsonic URL/Content-Range parsing used to make remote streams seekable.
 */

#include "subsonicstreamutils.h"

#include <cassert>
#include <iostream>

using namespace Fooyin::Subsonic;

int main()
{
    assert(isSubsonicStreamUrl(QUrl{QStringLiteral("https://music.example/rest/stream?id=42")}));
    assert(isSubsonicStreamUrl(QUrl{QStringLiteral("http://host/sub/rest/stream?a=b&id=42&format=raw")}));
    assert(!isSubsonicStreamUrl(QUrl{QStringLiteral("https://music.example/rest/stream")}));
    assert(!isSubsonicStreamUrl(QUrl{QStringLiteral("https://music.example/rest/getCoverArt?id=42")}));
    assert(!isSubsonicStreamUrl(QUrl{QStringLiteral("file:///music/track.flac")}));

    const ContentRange full = parseContentRange(QByteArrayLiteral("bytes 0-999/1000"));
    assert(full.start == 0);
    assert(full.total == 1000);

    const ContentRange mid = parseContentRange(QByteArrayLiteral("bytes 500-999/1000"));
    assert(mid.start == 500);
    assert(mid.total == 1000);

    const ContentRange unknown = parseContentRange(QByteArrayLiteral("bytes 0-99/*"));
    assert(unknown.start == 0);
    assert(unknown.total == -1);

    const ContentRange unsatisfiable = parseContentRange(QByteArrayLiteral("bytes */1000"));
    assert(unsatisfiable.start == -1);
    assert(unsatisfiable.total == 1000);

    const ContentRange malformed = parseContentRange(QByteArrayLiteral("garbage"));
    assert(malformed.start == -1);
    assert(malformed.total == -1);

    assert(songIdFromCachePath(QStringLiteral("/cache/subsonic/Moe Shop - Superstar [tr-4236].flac"))
           == QStringLiteral("tr-4236"));
    assert(songIdFromCachePath(QStringLiteral("C:/cache/DJ Myosuke - GOJO [tr-4200].mp3"))
           == QStringLiteral("tr-4200"));
    assert(songIdFromCachePath(QStringLiteral("/music/plain.flac")).isEmpty());
    assert(songIdFromCachePath(QStringLiteral("/cache/[no-extension]")).isEmpty());

    const QJsonObject song{
        {QStringLiteral("id"), QStringLiteral("tr-4802")},
        {QStringLiteral("artist"), QStringLiteral("Iwakura")},
        {QStringLiteral("title"), QStringLiteral("Hatred")},
        {QStringLiteral("suffix"), QStringLiteral("flac")},
    };
    const QString fileName = subsonicCacheFileName(song);
    assert(fileName == QStringLiteral("Iwakura - Hatred [tr-4802].flac"));
    assert(songIdFromCachePath(QStringLiteral("/home/user/.cache/fooyin/subsonic/") + fileName)
           == QStringLiteral("tr-4802"));

    const QJsonObject gojo{
        {QStringLiteral("id"), QStringLiteral("tr-4200")},
        {QStringLiteral("artist"), QStringLiteral("DJ Myosuke")},
        {QStringLiteral("title"), QStringLiteral("GOJO")},
        {QStringLiteral("suffix"), QStringLiteral("flac")},
    };
    assert(subsonicCacheFileName(gojo) == QStringLiteral("DJ Myosuke - GOJO [tr-4200].flac"));
    assert(songIdFromCachePath(subsonicCacheFileName(gojo)) == QStringLiteral("tr-4200"));

    assert(imageMimeType(QByteArrayLiteral("\xFF\xD8\xFF\x00"), {}) == QStringLiteral("image/jpeg"));
    assert(imageMimeType(QByteArrayLiteral("\x89PNG\r\n\x1a\n"), {}) == QStringLiteral("image/png"));
    assert(imageMimeType(QByteArrayLiteral("{\"error\":1}"), QStringLiteral("application/json")).isEmpty());
    assert(imageMimeType(QByteArrayLiteral("\xFF\xD8\xFF"), QStringLiteral("image/jpeg"))
           == QStringLiteral("image/jpeg"));

    std::cout << "subsonicstreamutils: ok\n";
    return 0;
}
