/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "subsonicsettings.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>

class QNetworkReply;

namespace Fooyin {
class NetworkAccessManager;

namespace Subsonic {
class SubsonicClient : public QObject
{
    Q_OBJECT

public:
    using ObjectCallback = std::function<void(const QJsonObject& object, const QString& error)>;
    using ArrayCallback = std::function<void(const QJsonArray& items, const QString& error)>;
    using SearchCallback = std::function<void(const QJsonObject& searchResult, const QString& error)>;
    using ResultCallback = std::function<void(bool ok, const QString& error)>;

    SubsonicClient(std::shared_ptr<NetworkAccessManager> network, SubsonicSettings settings,
                   QObject* parent = nullptr);

    void setSettings(const SubsonicSettings& settings);

    void getArtists(ArrayCallback callback);
    void getArtist(const QString& id, ObjectCallback callback);
    void getAlbum(const QString& id, ObjectCallback callback);
    void getPlaylists(ArrayCallback callback);
    void getPlaylist(const QString& id, ObjectCallback callback);
    void getMusicFolders(ArrayCallback callback);
    /*! Searches artists/albums/songs server-side (search3, falling back to search2). */
    void search(const QString& query, int artistCount, int albumCount, int songCount, SearchCallback callback);
    void ping(ObjectCallback callback);

    //! Reports a now-playing (@p submission false) or completed-play (@p submission true) event.
    void scrobble(const QString& id, bool submission);

    //! Star/unstar a song, album or artist by id. @p idParam is `id`, `albumId` or `artistId`.
    void star(const QString& id, bool star, const QString& idParam = QStringLiteral("id"));
    //! Sets a 1-5 star rating (0 clears it) on a song, album or artist.
    void setRating(const QString& id, int rating);

    //! Starred songs across artists/albums/songs.
    void getStarred(ArrayCallback callback);
    //! The complete `starred2` object (artist/album/song arrays).
    void getStarred2(ObjectCallback callback);
    /*! Album list: type = newest, recent, frequent, starred, highest, random,
     *  alphabeticalByName, alphabeticalByArtist, byYear, byGenre. */
    void getAlbumList2(const QString& type, ArrayCallback callback, int size = 100, int offset = 0);
    void getRandomSongs(ArrayCallback callback, int count = 50);
    void getGenres(ArrayCallback callback);
    void getSongsByGenre(const QString& genre, ArrayCallback callback, int count = 100, int offset = 0);
    void getArtistInfo2(const QString& id, ObjectCallback callback);
    void getAlbumInfo2(const QString& id, ObjectCallback callback);
    void getSimilarSongs2(const QString& id, ArrayCallback callback, int count = 50);
    void getTopSongs(const QString& artist, ArrayCallback callback, int count = 50);
    void getLyrics(const QString& artist, const QString& title, ObjectCallback callback);
    void getLyricsBySongId(const QString& id, ObjectCallback callback);
    //! Queries the server's OpenSubsonic extensions and caches their names.
    void getOpenSubsonicExtensions(ArrayCallback callback);
    [[nodiscard]] bool supportsFormPost() const;

    void createPlaylist(const QString& name, const QStringList& songIds, ResultCallback callback);
    void renamePlaylist(const QString& id, const QString& name, ResultCallback callback);
    void addToPlaylist(const QString& id, const QStringList& songIds, ResultCallback callback);
    //! Remove entries by position (as returned by getPlaylist). Indexes are applied highest-first.
    void removeFromPlaylist(const QString& id, QList<int> indexes, ResultCallback callback);
    void deletePlaylist(const QString& id, ResultCallback callback);

    //! The original file, without transcoding. Used for library downloads.
    [[nodiscard]] QUrl downloadUrl(const QString& id) const;
    //! The original file through the stream endpoint (fallback for servers without `download`).
    [[nodiscard]] QUrl originalStreamUrl(const QString& id) const;
    [[nodiscard]] QUrl streamUrl(const QString& id) const;
    [[nodiscard]] QUrl coverArtUrl(const QString& id, int size = 300) const;

private:
    using ResponseCallback = std::function<void(const QJsonObject& response, const QString& error)>;

    void get(const QString& method, const QUrlQuery& query, ResponseCallback callback, int attempt = 0);
    void post(const QString& method, const QUrlQuery& query, ResponseCallback callback);
    void send(const QString& method, const QUrlQuery& query, const QByteArray& body, ResponseCallback callback,
              int attempt = 0);
    void updatePlaylist(const QUrlQuery& query, ResultCallback callback);

    std::shared_ptr<NetworkAccessManager> m_network;
    SubsonicSettings m_settings;
    QSet<QString> m_extensions;
};
} // namespace Subsonic
} // namespace Fooyin
