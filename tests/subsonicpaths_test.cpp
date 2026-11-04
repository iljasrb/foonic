/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Keep assertions active in Release builds; CI configures the tests with -DNDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "subsonicpaths.h"

#include <QJsonObject>

#include <cassert>
#include <iostream>

using namespace Fooyin::Subsonic;

int main()
{
    assert(sanitisePathComponent(QStringLiteral("AC/DC: Live?")) == QStringLiteral("AC_DC_ Live_"));
    assert(sanitisePathComponent(QStringLiteral("Name. ")) == QStringLiteral("Name"));
    assert(sanitisePathComponent(QStringLiteral("  spaced  ")) == QStringLiteral("spaced"));

    {
        QJsonObject song;
        song[QStringLiteral("id")]        = QStringLiteral("42");
        song[QStringLiteral("title")]     = QStringLiteral("Song");
        song[QStringLiteral("artist")]    = QStringLiteral("Artist");
        song[QStringLiteral("album")]     = QStringLiteral("Album");
        song[QStringLiteral("track")]     = 3;
        song[QStringLiteral("suffix")]    = QStringLiteral("flac");

        assert(subsonicSongPath(song, QStringLiteral("/music"))
               == QStringLiteral("/music/Artist/Album/03 - Song.flac"));
    }

    {
        QJsonObject song;
        song[QStringLiteral("id")]         = QStringLiteral("43");
        song[QStringLiteral("title")]      = QStringLiteral("Song");
        song[QStringLiteral("albumArtist")] = QStringLiteral("Album Artist");
        song[QStringLiteral("album")]      = QStringLiteral("Album");
        song[QStringLiteral("track")]      = 1;
        song[QStringLiteral("discNumber")] = 2;
        song[QStringLiteral("suffix")]     = QStringLiteral("mp3");

        assert(subsonicSongPath(song, QStringLiteral("/music"))
               == QStringLiteral("/music/Album Artist/Album/CD2/01 - Song.mp3"));
    }

    {
        QJsonObject song;
        song[QStringLiteral("id")]     = QStringLiteral("44");
        song[QStringLiteral("title")]  = QStringLiteral("Song");
        song[QStringLiteral("suffix")] = QStringLiteral("opus");

        assert(subsonicSongPath(song, QStringLiteral("/music"))
               == QStringLiteral("/music/Unknown Artist/Unknown Album/Song.opus"));
    }

    assert(withIdSuffix(QStringLiteral("/music/Artist/Album/01 - Song.flac"), QStringLiteral("42"))
           == QStringLiteral("/music/Artist/Album/01 - Song [42].flac"));

    std::cout << "subsonicpaths: ok\n";
    return 0;
}
