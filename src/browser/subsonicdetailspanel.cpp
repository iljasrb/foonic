/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicdetailspanel.h"

#include "subsonicclient.h"
#include "subsoniccovercache.h"
#include "subsonicnetwork.h"
#include "subsonicsettings.h"

#include <core/network/networkaccessmanager.h>

#include <QFont>
#include <QFrame>
#include <QJsonArray>
#include <QLabel>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPixmapCache>
#include <QStringList>
#include <QTextOption>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
namespace {
QString durationText(int seconds)
{
    if(seconds <= 0) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString ratingText(int rating)
{
    if(rating <= 0) {
        return {};
    }
    return QString(rating, u'★') + QString(5 - rating, u'☆');
}

QString starredText(const QJsonObject& object)
{
    return object.value(u"starred"_s).toString().isEmpty() ? QString{} : u"★"_s;
}

QString joinMeta(const QStringList& parts)
{
    QStringList nonEmpty;
    for(const QString& part : parts) {
        if(!part.isEmpty()) {
            nonEmpty.push_back(part);
        }
    }
    return nonEmpty.join(u"  ·  "_s);
}

QString coverIdFor(const QJsonObject& object)
{
    const QString coverArt = object.value(u"coverArt"_s).toString();
    if(!coverArt.isEmpty()) {
        return coverArt;
    }
    const QString albumId = object.value(u"albumId"_s).toString();
    if(!albumId.isEmpty()) {
        return albumId;
    }
    return object.value(u"id"_s).toString();
}
} // namespace

SubsonicDetailsPanel::SubsonicDetailsPanel(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                                           SubsonicClient* client, QWidget* parent)
    : QWidget{parent}
    , m_network{std::move(network)}
    , m_settings{settings}
    , m_client{client}
    , m_cover{new QLabel{this}}
    , m_title{new QLabel{this}}
    , m_subtitle{new QLabel{this}}
    , m_meta{new QLabel{this}}
    , m_body{new QPlainTextEdit{this}}
{
    setMinimumWidth(260);
    setMaximumWidth(460);

    m_cover->setFixedSize(180, 180);
    m_cover->setAlignment(Qt::AlignCenter);
    m_cover->setScaledContents(false);

    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() + 2.0);
    m_title->setFont(titleFont);
    m_title->setWordWrap(true);

    m_subtitle->setWordWrap(true);
    m_meta->setWordWrap(true);
    m_meta->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_body->setReadOnly(true);
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(6);
    layout->addWidget(m_cover, 0, Qt::AlignHCenter);
    layout->addWidget(m_title);
    layout->addWidget(m_subtitle);
    layout->addWidget(m_meta);
    layout->addSpacing(4);
    layout->addWidget(m_body, 1);

    clear();
}

void SubsonicDetailsPanel::clear()
{
    ++m_requestId;
    m_coverId.clear();
    m_cover->clear();
    m_title->clear();
    m_subtitle->clear();
    m_meta->clear();
    m_body->clear();
}

void SubsonicDetailsPanel::setHeader(const QString& title, const QString& subtitle)
{
    m_title->setText(title);
    m_subtitle->setText(subtitle);
    m_subtitle->setVisible(!subtitle.isEmpty());
}

void SubsonicDetailsPanel::setMeta(const QString& meta)
{
    m_meta->setText(meta);
    m_meta->setVisible(!meta.isEmpty());
}

void SubsonicDetailsPanel::setBody(const QString& body)
{
    m_body->setPlainText(body);
}

void SubsonicDetailsPanel::setCover(const QString& coverArtId)
{
    if(coverArtId.isEmpty()) {
        m_coverId.clear();
        m_cover->clear();
        return;
    }
    m_coverId = coverArtId;
    const QString cacheKey = u"foonic-details:"_s + coverArtId;
    QPixmap cached;
    if(QPixmapCache::find(cacheKey, &cached)) {
        m_cover->setPixmap(cached);
        return;
    }
    const QPixmap highRes = loadCachedCover(coverArtId, CoverCacheSize);
    if(!highRes.isNull()) {
        cached = highRes.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmapCache::insert(cacheKey, cached);
        m_cover->setPixmap(cached);
        return;
    }

    // Show the grid copy immediately, then upgrade to the high-resolution copy in the background.
    const QPixmap grid = loadCachedCover(coverArtId, GridCoverSize);
    if(!grid.isNull()) {
        cached = grid.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmapCache::insert(cacheKey, cached);
        m_cover->setPixmap(cached);
    }
    else {
        m_cover->clear();
    }

    if(!m_client || m_highResRequests.contains(coverArtId)) {
        return;
    }

    m_highResRequests.insert(coverArtId);
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);
    const int coverSize = std::max(settings.coverArtSize, CoverCacheSize);
    QNetworkReply* reply = subsonicGet(m_network, m_client->coverArtUrl(coverArtId, coverSize), settings);
    if(!reply) {
        m_highResRequests.remove(coverArtId);
        return;
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, coverArtId, cacheKey] {
        reply->deleteLater();
        m_highResRequests.remove(coverArtId);
        if(m_coverId != coverArtId || reply->error() != QNetworkReply::NoError) {
            return;
        }

        QPixmap pixmap;
        if(!pixmap.loadFromData(reply->readAll())) {
            return;
        }
        saveCachedCover(coverArtId, pixmap, CoverCacheSize);
        pixmap = pixmap.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmapCache::insert(cacheKey, pixmap);
        m_cover->setPixmap(pixmap);
    });
}

void SubsonicDetailsPanel::showSong(const QJsonObject& song)
{
    ++m_requestId;

    const QString title    = song.value(u"title"_s).toString();
    const QString artist   = song.value(u"artist"_s).toString();
    const QString album    = song.value(u"album"_s).toString();
    const QString subtitle = album.isEmpty() ? artist : artist + u" — "_s + album;

    setHeader(title, subtitle);
    setMeta(joinMeta({song.value(u"year"_s).toInt() > 0 ? QString::number(song.value(u"year"_s).toInt()) : QString{},
                      song.value(u"genre"_s).toString(), durationText(song.value(u"duration"_s).toInt()),
                      song.value(u"bitRate"_s).toInt() > 0
                          ? tr("%1 kbps").arg(song.value(u"bitRate"_s).toInt())
                          : QString{},
                      song.value(u"suffix"_s).toString().toUpper(),
                      ratingText(song.value(u"userRating"_s).toInt()), starredText(song)}));
    setCover(coverIdFor(song));

    const QString cacheKey = song.value(u"id"_s).toString();
    if(const auto cached = m_lyricsCache.constFind(cacheKey); cached != m_lyricsCache.constEnd()) {
        setBody(cached.value());
        return;
    }

    setBody(tr("Loading lyrics…"));
    requestLyrics(song);
}

void SubsonicDetailsPanel::showAlbum(const QJsonObject& album)
{
    ++m_requestId;

    setHeader(album.value(u"name"_s).toString(), album.value(u"artist"_s).toString());
    setMeta(joinMeta({album.value(u"year"_s).toInt() > 0 ? QString::number(album.value(u"year"_s).toInt()) : QString{},
                      tr("%1 songs").arg(album.value(u"songCount"_s).toInt()),
                      durationText(album.value(u"duration"_s).toInt()),
                      ratingText(album.value(u"userRating"_s).toInt()), starredText(album)}));
    setCover(coverIdFor(album));
    setBody(tr("Loading album notes…"));
    requestAlbumInfo(album);
}

void SubsonicDetailsPanel::showArtist(const QJsonObject& artist)
{
    ++m_requestId;

    setHeader(artist.value(u"name"_s).toString(), tr("Artist"));
    setMeta(joinMeta({tr("%1 albums").arg(artist.value(u"albumCount"_s).toInt()), starredText(artist)}));
    setCover(coverIdFor(artist));
    setBody(tr("Loading biography…"));
    requestArtistInfo(artist);
}

void SubsonicDetailsPanel::showPlaylist(const QJsonObject& playlist)
{
    ++m_requestId;

    setHeader(playlist.value(u"name"_s).toString(), playlist.value(u"owner"_s).toString());
    setMeta(joinMeta({tr("%1 songs").arg(playlist.value(u"songCount"_s).toInt()),
                      durationText(playlist.value(u"duration"_s).toInt())}));
    setCover(coverIdFor(playlist));

    const QString comment = playlist.value(u"comment"_s).toString();
    setBody(comment.isEmpty() ? tr("Server playlist") : comment);
}

QString SubsonicDetailsPanel::songLyrics(const QJsonObject& object) const
{
    QStringList lines;
    for(const QJsonValue& value : object.value(u"structuredLyrics"_s).toArray()) {
        for(const QJsonValue& line : value.toObject().value(u"line"_s).toArray()) {
            lines.push_back(line.toObject().value(u"value"_s).toString());
        }
    }
    return lines.join(u'\n');
}

void SubsonicDetailsPanel::requestLyrics(const QJsonObject& song)
{
    if(!m_client) {
        setBody(tr("No lyrics available."));
        return;
    }

    const int requestId = m_requestId;
    const QString key   = song.value(u"id"_s).toString();
    const QString artist = song.value(u"artist"_s).toString();
    const QString title  = song.value(u"title"_s).toString();

    const auto finish = [this, requestId, key](const QString& text) {
        if(requestId != m_requestId) {
            return;
        }
        if(text.trimmed().isEmpty()) {
            setBody(tr("No lyrics available."));
            return;
        }
        m_lyricsCache.insert(key, text);
        setBody(text);
    };

    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->getLyricsBySongId(
        key, [this, requestId, artist, title, finish](const QJsonObject& object, const QString& error) {
            if(requestId != m_requestId) {
                return;
            }
            const QString text = songLyrics(object);
            if(error.isEmpty() && !text.trimmed().isEmpty()) {
                finish(text);
                return;
            }

            // Fall back to the legacy artist/title endpoint used by older servers.
            m_client->getLyrics(artist, title, [finish](const QJsonObject& legacy, const QString& legacyError) {
                finish(legacyError.isEmpty() ? legacy.value(u"value"_s).toString() : QString{});
            });
        });
}

void SubsonicDetailsPanel::requestAlbumInfo(const QJsonObject& album)
{
    if(!m_client) {
        setBody(tr("No album notes available."));
        return;
    }

    const int requestId = m_requestId;
    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->getAlbumInfo2(album.value(u"id"_s).toString(),
                            [this, requestId](const QJsonObject& object, const QString& error) {
                                if(requestId != m_requestId) {
                                    return;
                                }
                                const QString notes = object.value(u"notes"_s).toString();
                                setBody(error.isEmpty() && !notes.isEmpty() ? notes : tr("No album notes available."));
                            });
}

void SubsonicDetailsPanel::requestArtistInfo(const QJsonObject& artist)
{
    if(!m_client) {
        setBody(tr("No biography available."));
        return;
    }

    const int requestId = m_requestId;
    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->getArtistInfo2(artist.value(u"id"_s).toString(),
                             [this, requestId](const QJsonObject& object, const QString& error) {
                                 if(requestId != m_requestId) {
                                     return;
                                 }

                                 QString text = object.value(u"biography"_s).toString();
                                 QStringList similar;
                                 for(const QJsonValue& value : object.value(u"similarArtist"_s).toArray()) {
                                     const QString name = value.toObject().value(u"name"_s).toString();
                                     if(!name.isEmpty()) {
                                         similar.push_back(name);
                                     }
                                 }
                                 if(!similar.isEmpty()) {
                                     if(!text.isEmpty()) {
                                         text += u"\n\n"_s;
                                     }
                                     text += tr("Similar artists: %1").arg(similar.join(u", "_s));
                                 }
                                 setBody(error.isEmpty() && !text.isEmpty() ? text : tr("No biography available."));
                             });
}
} // namespace Fooyin::Subsonic
