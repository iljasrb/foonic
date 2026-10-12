/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicclient.h"

#include <core/network/networkaccessmanager.h>

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>

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

QString randomSalt()
{
    static constexpr auto Chars = "abcdefghijklmnopqrstuvwxyz0123456789"_L1;
    QString salt;
    salt.reserve(12);
    for(int i{0}; i < 12; ++i) {
        salt.append(Chars.at(QRandomGenerator::global()->bounded(Chars.size())));
    }
    return salt;
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

void SubsonicClient::ping(std::function<void(const QString& error)> callback)
{
    get(u"ping"_s, {}, [callback = std::move(callback)](const QJsonObject&, const QString& error) {
        callback(error);
    });
}

void SubsonicClient::search(const QString& query, SearchCallback callback)
{
    const auto makeQuery = [query] {
        QUrlQuery q;
        q.addQueryItem(u"query"_s, query);
        q.addQueryItem(u"artistCount"_s, u"30"_s);
        q.addQueryItem(u"albumCount"_s, u"30"_s);
        q.addQueryItem(u"songCount"_s, u"60"_s);
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
    }

    return buildUrl(u"stream"_s, query);
}

QUrl SubsonicClient::coverArtUrl(const QString& id, int size) const
{
    QUrlQuery query;
    query.addQueryItem(u"id"_s, id);
    query.addQueryItem(u"size"_s, QString::number(size));
    return buildUrl(u"getCoverArt"_s, query);
}

void SubsonicClient::get(const QString& method, const QUrlQuery& query, ResponseCallback callback)
{
    if(!m_settings.isValid()) {
        callback({}, tr("Subsonic server is not configured."));
        return;
    }

    QNetworkReply* reply = m_network->get(QNetworkRequest{buildUrl(method, query)});
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback), method]() {
        reply->deleteLater();

        if(reply->error() != QNetworkReply::NoError) {
            qCWarning(SUBSONIC) << "Request failed:" << method << reply->errorString();
            callback({}, reply->errorString());
            return;
        }

        QJsonParseError parseError;
        const QByteArray payload      = reply->readAll();
        const QJsonDocument document  = QJsonDocument::fromJson(payload, &parseError);
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

QUrl SubsonicClient::buildUrl(const QString& method, QUrlQuery query) const
{
    QUrl url{m_settings.server.trimmed()};
    if(url.scheme().isEmpty()) {
        url = QUrl{u"http://"_s + m_settings.server.trimmed()};
    }

    QString path = url.path();
    while(path.endsWith(u'/')) {
        path.chop(1);
    }
    url.setPath(path + u"/rest/"_s + method);

    const QString salt = randomSalt();
    query.addQueryItem(u"u"_s, m_settings.username);
    query.addQueryItem(u"s"_s, salt);
    query.addQueryItem(u"t"_s, QString::fromLatin1(QCryptographicHash::hash((m_settings.password + salt).toUtf8(),
                                                                           QCryptographicHash::Md5)
                                                       .toHex()));
    query.addQueryItem(u"v"_s, u"1.16.1"_s);
    query.addQueryItem(u"c"_s, u"fooyin-subsonic"_s);
    query.addQueryItem(u"f"_s, u"json"_s);
    url.setQuery(query);

    return url;
}
} // namespace Fooyin::Subsonic
