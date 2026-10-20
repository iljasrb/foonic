/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "subsonicsettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>

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
    void ping(std::function<void(const QJsonObject& response, const QString& error)> callback);

    //! Reports a now-playing (@p submission false) or completed-play (@p submission true) event.
    void scrobble(const QString& id, bool submission);

    //! Star/unstar a song, album or artist by id.
    void star(const QString& id, bool star);

    //! Starred songs across artists/albums/songs.
    void getStarred(ArrayCallback callback);
    /*! Album list: type = newest, recent, frequent, starred, highest, random,
     *  alphabeticalByName, alphabeticalByArtist, byYear, byGenre. */
    void getAlbumList2(const QString& type, ArrayCallback callback, int size = 100, int offset = 0);
    void getRandomSongs(ArrayCallback callback, int count = 50);

    void createPlaylist(const QString& name, const QStringList& songIds,
                        std::function<void(bool ok, const QString& error)> callback);
    void renamePlaylist(const QString& id, const QString& name,
                        std::function<void(bool ok, const QString& error)> callback);
    void addToPlaylist(const QString& id, const QStringList& songIds,
                       std::function<void(bool ok, const QString& error)> callback);
    void deletePlaylist(const QString& id, std::function<void(bool ok, const QString& error)> callback);

    [[nodiscard]] QUrl streamUrl(const QString& id) const;
    [[nodiscard]] QUrl coverArtUrl(const QString& id, int size = 300) const;

private:
    using ResponseCallback = std::function<void(const QJsonObject& response, const QString& error)>;

    void get(const QString& method, const QUrlQuery& query, ResponseCallback callback);
    [[nodiscard]] QUrl buildUrl(const QString& method, QUrlQuery query) const;

    std::shared_ptr<NetworkAccessManager> m_network;
    SubsonicSettings m_settings;
};
} // namespace Subsonic
} // namespace Fooyin
