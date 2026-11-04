/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QWidget>

#include <memory>

class QLabel;
class QPlainTextEdit;

namespace Fooyin {
class NetworkAccessManager;
class SettingsManager;

namespace Subsonic {
class SubsonicClient;

/*!
 * Side panel showing the selected song/album/artist/playlist, including cover art, server metadata
 * and lyrics or artist/album notes when the server provides them.
 */
class SubsonicDetailsPanel : public QWidget
{
    Q_OBJECT

public:
    SubsonicDetailsPanel(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                         SubsonicClient* client, QWidget* parent = nullptr);

    void showSong(const QJsonObject& song);
    void showAlbum(const QJsonObject& album);
    void showArtist(const QJsonObject& artist);
    void showPlaylist(const QJsonObject& playlist);
    void clear();

private:
    void setCover(const QString& coverArtId);
    void setHeader(const QString& title, const QString& subtitle);
    void setMeta(const QString& meta);
    void setBody(const QString& body);
    void requestLyrics(const QJsonObject& song);
    void requestAlbumInfo(const QJsonObject& album);
    void requestArtistInfo(const QJsonObject& artist);
    [[nodiscard]] QString songLyrics(const QJsonObject& object) const;

    std::shared_ptr<NetworkAccessManager> m_network;
    SettingsManager* m_settings;
    SubsonicClient* m_client;

    QLabel* m_cover;
    QLabel* m_title;
    QLabel* m_subtitle;
    QLabel* m_meta;
    QPlainTextEdit* m_body;

    QHash<QString, QString> m_lyricsCache;
    QSet<QString> m_highResRequests;
    QString m_coverId;
    int m_requestId{0};
};
} // namespace Subsonic
} // namespace Fooyin
