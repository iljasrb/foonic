/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicclient.h"

#include "subsonicnetwork.h"
#include "subsonicurl.h"

#include <core/network/networkaccessmanager.h>

#include <QDateTime>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(SUBSONIC, "fy.subsonic")

namespace {
QJsonArray asArray(const QJsonValue& value)
{
    if(value.isArray()) {
        return value.toArray();
    }
    if(value.isObject()) {
        return {value.toObject()};
    }
    return {};
}

bool isRetryableMethod(const QString& method)
{
    static const QSet<QString> retryable{
        u"ping"_s,
        u"getArtists"_s,
        u"getArtist"_s,
        u"getAlbum"_s,
        u"getPlaylists"_s,
        u"getPlaylist"_s,
        u"getMusicFolders"_s,
        u"search2"_s,
        u"search3"_s,
        u"getStarred2"_s,
        u"getAlbumList2"_s,
        u"getRandomSongs"_s,
        u"getGenres"_s,
        u"getSongsByGenre"_s,
        u"getArtistInfo2"_s,
        u"getAlbumInfo2"_s,
        u"getSimilarSongs2"_s,
        u"getTopSongs"_s,
        u"getLyrics"_s,
        u"getLyricsBySongId"_s,
        u"getOpenSubsonicExtensions"_s,
    };
    return retryable.contains(method);
}
} // namespace

namespace Fooyin::Subsonic {
SubsonicClient::SubsonicClient(std::shared_ptr<NetworkAccessManager> network, SubsonicSettings settings,
                               QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
    , m_settings{std::move(settings)}
{ }

void SubsonicClient::setSettings(const SubsonicSettings& settings)
{
    m_settings = settings;
}

void SubsonicClient::getArtists(ArrayCallback callback)
{
    get(u"getArtists"_s, {}, [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
        if(!error.isEmpty()) {
            callback({}, error);
            return;
        }

        QJsonArray artists;
        for(const QJsonValue& index : asArray(response.value(u"artists"_s).toObject().value(u"index"_s))) {
            for(const QJsonValue& artist : asArray(index.toObject().value(u"artist"_s))) {
                artists.append(artist);
            }
        }
        callback(artists, {});
    });
}

void SubsonicClient::getArtist(const QString& id, ObjectCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    get(u"getArtist"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(response.value(u"artist"_s).toObject(), error);
        });
}

void SubsonicClient::getAlbum(const QString& id, ObjectCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    get(u"getAlbum"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(response.value(u"album"_s).toObject(), error);
        });
}

void SubsonicClient::getPlaylists(ArrayCallback callback)
{
    get(u"getPlaylists"_s, {}, [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
        callback(asArray(response.value(u"playlists"_s).toObject().value(u"playlist"_s)), error);
    });
}

void SubsonicClient::getPlaylist(const QString& id, ObjectCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    get(u"getPlaylist"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(response.value(u"playlist"_s).toObject(), error);
        });
}

void SubsonicClient::getMusicFolders(ArrayCallback callback)
{
    get(u"getMusicFolders"_s, {},
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(asArray(response.value(u"musicFolders"_s).toObject().value(u"musicFolder"_s)), error);
        });
}

void SubsonicClient::ping(std::function<void(const QJsonObject& response, const QString& error)> callback)
{
    get(u"ping"_s, {}, [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
        callback(response, error);
    });
}

void SubsonicClient::scrobble(const QString& id, bool submission)
{
    if(!m_settings.isValid() || id.isEmpty()) {
        return;
    }

    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    query.addQueryItem(u"submission"_s, submission ? u"true"_s : u"false"_s);
    if(submission) {
        query.addQueryItem(u"time"_s, QString::number(QDateTime::currentMSecsSinceEpoch()));
    }

    // Fire and forget; scrobble responses carry no data worth surfacing.
    get(u"scrobble"_s, query, [](const QJsonObject&, const QString&) {});
}

void SubsonicClient::star(const QString& id, bool star, const QString& idParam)
{
    if(!m_settings.isValid() || id.isEmpty()) {
        return;
    }
    QUrlQuery query;
    query.addQueryItem(idParam, id);
    get(star ? u"star"_s : u"unstar"_s, query, [](const QJsonObject&, const QString&) {});
}

void SubsonicClient::setRating(const QString& id, int rating)
{
    if(!m_settings.isValid() || id.isEmpty()) {
        return;
    }
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    query.addQueryItem(u"rating"_s, QString::number(std::clamp(rating, 0, 5)));
    get(u"setRating"_s, query, [](const QJsonObject&, const QString&) {});
}

void SubsonicClient::getStarred(ArrayCallback callback)
{
    getStarred2([callback = std::move(callback)](const QJsonObject& object, const QString& error) {
        callback(error.isEmpty() ? asArray(object.value(u"song"_s)) : QJsonArray{}, error);
    });
}

void SubsonicClient::getStarred2(ObjectCallback callback)
{
    get(u"getStarred2"_s, {},
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(response.value(u"starred2"_s).toObject(), error);
        });
}

void SubsonicClient::getAlbumList2(const QString& type, ArrayCallback callback, int size, int offset)
{
    QUrlQuery query;
    query.addQueryItem(u"type"_s, type);
    query.addQueryItem(u"size"_s, QString::number(std::max(1, size)));
    if(offset > 0) {
        query.addQueryItem(u"offset"_s, QString::number(offset));
    }
    get(u"getAlbumList2"_s, query, [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
        callback(error.isEmpty() ? asArray(response.value(u"albumList2"_s).toObject().value(u"album"_s))
                                 : QJsonArray{},
                 error);
    });
}

void SubsonicClient::getRandomSongs(ArrayCallback callback, int count)
{
    QUrlQuery query;
    query.addQueryItem(u"size"_s, QString::number(std::max(1, count)));
    get(u"getRandomSongs"_s, query, [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
        callback(error.isEmpty() ? asArray(response.value(u"randomSongs"_s).toObject().value(u"song"_s)) : QJsonArray{},
                 error);
    });
}

void SubsonicClient::getGenres(ArrayCallback callback)
{
    get(u"getGenres"_s, {}, [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
        callback(error.isEmpty() ? asArray(response.value(u"genres"_s).toObject().value(u"genre"_s)) : QJsonArray{},
                 error);
    });
}

void SubsonicClient::getSongsByGenre(const QString& genre, ArrayCallback callback, int count, int offset)
{
    QUrlQuery query;
    query.addQueryItem(u"genre"_s, genre);
    query.addQueryItem(u"count"_s, QString::number(std::max(1, count)));
    if(offset > 0) {
        query.addQueryItem(u"offset"_s, QString::number(offset));
    }
    get(u"getSongsByGenre"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(error.isEmpty() ? asArray(response.value(u"songsByGenre"_s).toObject().value(u"song"_s))
                                     : QJsonArray{},
                     error);
        });
}

void SubsonicClient::getArtistInfo2(const QString& id, ObjectCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    get(u"getArtistInfo2"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(response.value(u"artistInfo2"_s).toObject(), error);
        });
}

void SubsonicClient::getAlbumInfo2(const QString& id, ObjectCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    get(u"getAlbumInfo2"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            // Both getAlbumInfo and getAlbumInfo2 return an `albumInfo` element.
            callback(response.value(u"albumInfo"_s).toObject(), error);
        });
}

void SubsonicClient::getSimilarSongs2(const QString& id, ArrayCallback callback, int count)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    query.addQueryItem(u"count"_s, QString::number(std::max(1, count)));
    get(u"getSimilarSongs2"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(error.isEmpty() ? asArray(response.value(u"similarSongs2"_s).toObject().value(u"song"_s))
                                     : QJsonArray{},
                     error);
        });
}

void SubsonicClient::getTopSongs(const QString& artist, ArrayCallback callback, int count)
{
    QUrlQuery query;
    query.addQueryItem(u"artist"_s, artist);
    query.addQueryItem(u"count"_s, QString::number(std::max(1, count)));
    get(u"getTopSongs"_s, query, [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
        callback(error.isEmpty() ? asArray(response.value(u"topSongs"_s).toObject().value(u"song"_s)) : QJsonArray{},
                 error);
    });
}

void SubsonicClient::getLyrics(const QString& artist, const QString& title, ObjectCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"artist"_s, artist);
    query.addQueryItem(u"title"_s, title);
    get(u"getLyrics"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(response.value(u"lyrics"_s).toObject(), error);
        });
}

void SubsonicClient::getLyricsBySongId(const QString& id, ObjectCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    get(u"getLyricsBySongId"_s, query,
        [callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            callback(response.value(u"lyricsList"_s).toObject(), error);
        });
}

void SubsonicClient::getOpenSubsonicExtensions(ArrayCallback callback)
{
    get(u"getOpenSubsonicExtensions"_s, {},
        [this, callback = std::move(callback)](const QJsonObject& response, const QString& error) {
            if(!error.isEmpty()) {
                callback({}, error);
                return;
            }
            const QJsonArray extensions = asArray(response.value(u"openSubsonicExtensions"_s));
            m_extensions.clear();
            for(const QJsonValue& value : extensions) {
                const QString name = value.toObject().value(u"name"_s).toString();
                if(!name.isEmpty()) {
                    m_extensions.insert(name);
                }
            }
            callback(extensions, {});
        });
}

bool SubsonicClient::supportsFormPost() const
{
    return m_extensions.contains(u"formPost"_s);
}

void SubsonicClient::createPlaylist(const QString& name, const QStringList& songIds, ResultCallback callback)
{
    if(name.trimmed().isEmpty()) {
        callback(false, tr("Playlist name is empty."));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(u"name"_s, name);
    for(const QString& id : songIds) {
        query.addQueryItem(u"songId"_s, id);
    }
    updatePlaylist(query, std::move(callback));
}

void SubsonicClient::renamePlaylist(const QString& id, const QString& name, ResultCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"playlistId"_s, id);
    query.addQueryItem(u"name"_s, name);
    updatePlaylist(query, std::move(callback));
}

void SubsonicClient::addToPlaylist(const QString& id, const QStringList& songIds, ResultCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"playlistId"_s, id);
    for(const QString& songId : songIds) {
        query.addQueryItem(u"songIdToAdd"_s, songId);
    }
    updatePlaylist(query, std::move(callback));
}

void SubsonicClient::removeFromPlaylist(const QString& id, QList<int> indexes, ResultCallback callback)
{
    if(id.isEmpty() || indexes.isEmpty()) {
        callback(false, tr("Nothing to remove."));
        return;
    }

    // Highest first: servers that remove one by one then never shift the indexes still to be removed.
    std::sort(indexes.begin(), indexes.end(), std::greater<>());

    QUrlQuery query;
    query.addQueryItem(u"playlistId"_s, id);
    for(const int index : indexes) {
        query.addQueryItem(u"songIndexToRemove"_s, QString::number(index));
    }
    updatePlaylist(query, std::move(callback));
}

void SubsonicClient::deletePlaylist(const QString& id, ResultCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    get(u"deletePlaylist"_s, query, [callback = std::move(callback)](const QJsonObject&, const QString& error) {
        callback(error.isEmpty(), error);
    });
}

void SubsonicClient::search(const QString& query, int artistCount, int albumCount, int songCount,
                            SearchCallback callback)
{
    const auto makeQuery = [query, artistCount, albumCount, songCount] {
        QUrlQuery q;
        q.addQueryItem(u"query"_s, query);
        q.addQueryItem(u"artistCount"_s, QString::number(artistCount));
        q.addQueryItem(u"albumCount"_s, QString::number(albumCount));
        q.addQueryItem(u"songCount"_s, QString::number(songCount));
        return q;
    };

    // search3 is the ID3 version (OpenSubsonic/Navidrome); older servers only have search2.
    get(u"search3"_s, makeQuery(),
        [this, makeQuery, callback = std::move(callback)](const QJsonObject& response, const QString& error) mutable {
            if(error.isEmpty()) {
                callback(response.value(u"searchResult3"_s).toObject(), {});
                return;
            }

            get(u"search2"_s, makeQuery(),
                [callback = std::move(callback)](const QJsonObject& response2, const QString& error2) mutable {
                    callback(response2.value(u"searchResult2"_s).toObject(), error2);
                });
        });
}

QUrl SubsonicClient::downloadUrl(const QString& id) const
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    return buildSubsonicUrl(m_settings, u"download"_s, query);
}

QUrl SubsonicClient::originalStreamUrl(const QString& id) const
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    query.addQueryItem(u"format"_s, u"raw"_s);
    query.addQueryItem(u"maxBitRate"_s, u"0"_s);
    return buildSubsonicUrl(m_settings, u"stream"_s, query);
}

QUrl SubsonicClient::streamUrl(const QString& id) const
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);

    if(m_settings.format.isEmpty() || m_settings.format == "raw"_L1) {
        // Direct play of the original file; also avoids the transcoding path entirely.
        query.addQueryItem(u"format"_s, u"raw"_s);
        query.addQueryItem(u"maxBitRate"_s, u"0"_s);
    }
    else {
        query.addQueryItem(u"format"_s, m_settings.format);
        if(m_settings.maxBitRate > 0) {
            query.addQueryItem(u"maxBitRate"_s, QString::number(m_settings.maxBitRate));
        }
        // Gives the buffer bar a size estimate for a transcoded stream.
        query.addQueryItem(u"estimateContentLength"_s, u"true"_s);
    }

    return buildSubsonicUrl(m_settings, u"stream"_s, query);
}

QUrl SubsonicClient::coverArtUrl(const QString& id, int size) const
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    query.addQueryItem(u"size"_s, QString::number(size));
    return buildSubsonicUrl(m_settings, u"getCoverArt"_s, query);
}

void SubsonicClient::get(const QString& method, const QUrlQuery& query, ResponseCallback callback, int attempt)
{
    send(method, query, {}, std::move(callback), attempt);
}

void SubsonicClient::post(const QString& method, const QUrlQuery& query, ResponseCallback callback)
{
    send(method, {}, query.toString(QUrl::FullyEncoded).toUtf8(), std::move(callback), 0);
}

void SubsonicClient::send(const QString& method, const QUrlQuery& query, const QByteArray& body,
                          ResponseCallback callback, int attempt)
{
    if(!m_settings.isValid()) {
        callback({}, tr("Subsonic server is not configured."));
        return;
    }

    const QUrl url = buildSubsonicUrl(m_settings, method, query);
    QNetworkReply* reply = body.isEmpty() ? subsonicGet(m_network, url, m_settings)
                                          : subsonicPost(m_network, url, body, m_settings);
    if(!reply) {
        callback({}, tr("The network is unavailable."));
        return;
    }

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, method, query, body, callback = std::move(callback), attempt]() mutable {
                reply->deleteLater();

                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if(reply->error() != QNetworkReply::NoError) {
                    const bool transient = status == 0 || status >= 500;
                    if(transient && attempt < 1 && isRetryableMethod(method)) {
                        qCWarning(SUBSONIC) << "Request failed, retrying:" << method << reply->errorString();
                        QTimer::singleShot(500, this,
                                           [this, method, query, body, callback = std::move(callback), attempt]() mutable {
                                               send(method, query, body, std::move(callback), attempt + 1);
                                           });
                        return;
                    }

                    QString error = reply->errorString();
                    if(status == 401 || status == 403) {
                        error = tr("Authentication failed (HTTP %1).").arg(status);
                    }
                    qCWarning(SUBSONIC) << "Request failed:" << method << error;
                    callback({}, error);
                    return;
                }

                QJsonParseError parseError;
                const QByteArray payload     = reply->readAll();
                const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
                qCDebug(SUBSONIC) << "Request ok:" << method << "response=" << payload.size() << "bytes";
                if(parseError.error != QJsonParseError::NoError || !document.isObject()) {
                    callback({}, SubsonicClient::tr("Invalid response from the Subsonic server."));
                    return;
                }

                const QJsonObject response = document.object().value(u"subsonic-response"_s).toObject();
                if(response.value(u"status"_s).toString() != "ok"_L1) {
                    const QJsonObject error = response.value(u"error"_s).toObject();
                    callback({}, error.value(u"message"_s).toString(tr("Subsonic request failed.")));
                    return;
                }

                callback(response, {});
            });
}

void SubsonicClient::updatePlaylist(const QUrlQuery& query, ResultCallback callback)
{
    const bool usePost = supportsFormPost() && query.queryItems().size() > 40;
    const auto done    = [callback = std::move(callback)](const QJsonObject&, const QString& error) {
        callback(error.isEmpty(), error);
    };

    if(usePost) {
        post(u"updatePlaylist"_s, query, done);
    }
    else {
        get(u"updatePlaylist"_s, query, done);
    }
}
} // namespace Fooyin::Subsonic
