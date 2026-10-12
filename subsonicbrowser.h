/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <core/track.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QHash>
#include <QPixmap>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

class QLabel;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace Fooyin {
class NetworkAccessManager;
class PlaylistInteractor;
class SettingsManager;

namespace Subsonic {
class SubsonicClient;
class SubsonicDownloadManager;

enum class BrowserAction : uint8_t
{
    Play = 0,
    AddToPlaylist,
    PlayNext,
    AddToQueue,
};

/*!
 * The Subsonic artist/album/song browser, usable in a dialog or embedded in a layout widget.
 */
class SubsonicBrowser : public QWidget
{
    Q_OBJECT

public:
    SubsonicBrowser(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                    SubsonicDownloadManager* downloads, SettingsManager* settings, QWidget* parent = nullptr);

    void refresh();
    void openSettings();

private:
    struct GatherState
    {
        QList<QJsonObject> songs;
        qint64 bytes{0};
        int pending{0};
        bool finished{false};
        BrowserAction action{BrowserAction::Play};
    };

    void onExpanded(QTreeWidgetItem* item);
    void ensureChildrenLoaded(QTreeWidgetItem* item, bool expand = true);
    [[nodiscard]] TrackList tracksForDrag(const QList<QTreeWidgetItem*>& items);
    void showContextMenu(const QPoint& pos);
    void playSelection(BrowserAction action);

    void loadChildren(QTreeWidgetItem* item, int kind, const QString& id, std::function<void()> onDone = {},
                      bool expand = true);
    void populateChildren(QTreeWidgetItem* item, int kind, const QJsonObject& object, bool expand);
    void addPlaceholder(QTreeWidgetItem* item);
    void gatherTracks(const QList<QTreeWidgetItem*>& items, const std::shared_ptr<GatherState>& state);
    void finishGather(const std::shared_ptr<GatherState>& state);
    void downloadAndPlay(const std::shared_ptr<GatherState>& state);
    void applyAction(const TrackList& tracks, BrowserAction action, bool startPlayback);
    [[nodiscard]] Track trackForSong(const QJsonObject& song, const QString& filepath) const;
    [[nodiscard]] QString cacheText(const QString& songId) const;
    void updateDownloadProgress();
    void applyCover(QTreeWidgetItem* item, const QString& coverArtId);
    void requestCover(const QString& coverArtId);
    void applyCoverToTree(const QString& coverArtId);
    void applyFilter();
    void clearTree();
    void rebuildArtistTree();
    void addPlaylistsHeader();
    void buildSearchTree(const QJsonObject& result);
    QTreeWidgetItem* createArtistItem(const QJsonObject& artist, QTreeWidgetItem* parent = nullptr);
    QTreeWidgetItem* createAlbumItem(QTreeWidgetItem* parent, const QJsonObject& album);
    QTreeWidgetItem* createSongItem(QTreeWidgetItem* parent, const QJsonObject& song);
    void setStatus(const QString& text);

    std::shared_ptr<NetworkAccessManager> m_network;
    PlaylistInteractor* m_interactor;
    SubsonicDownloadManager* m_downloads;
    SettingsManager* m_settings;
    SubsonicClient* m_client;
    QLabel* m_status;
    QTreeWidget* m_tree;
    QLineEdit* m_filter{nullptr};
    QTimer* m_filterTimer{nullptr};
    QString m_filterText;
    std::map<QTreeWidgetItem*, std::vector<std::function<void()>>> m_pendingLoads;
    std::set<QTreeWidgetItem*> m_loading;
    QHash<QString, QPixmap> m_coverCache;
    QSet<QString> m_coverRequests;
    QList<QJsonObject> m_artists;
    QList<QJsonObject> m_playlists;
    QHash<QString, QJsonArray> m_albumsByArtist;
    QHash<QString, QJsonArray> m_songsByAlbum;
    int m_searchGeneration{0};
    bool m_filterMode{false};
    bool m_applyingFilter{false};
    int m_generation{0};
    int m_treeGeneration{0};
};
} // namespace Subsonic
} // namespace Fooyin
