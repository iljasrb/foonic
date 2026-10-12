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
    using ArtistsCallback = std::function<void(const QJsonArray& artists, const QString& error)>;
    using SearchCallback = std::function<void(const QJsonObject& searchResult, const QString& error)>;

    SubsonicClient(std::shared_ptr<NetworkAccessManager> network, SubsonicSettings settings,
                   QObject* parent = nullptr);

    void setSettings(const SubsonicSettings& settings);

    void getArtists(ArtistsCallback callback);
    void getArtist(const QString& id, ObjectCallback callback);
    void getAlbum(const QString& id, ObjectCallback callback);
    void getPlaylists(ArtistsCallback callback);
    void getPlaylist(const QString& id, ObjectCallback callback);
    /*! Searches artists/albums/songs server-side (search3, falling back to search2). */
    void search(const QString& query, SearchCallback callback);
    void ping(std::function<void(const QString& error)> callback);

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
