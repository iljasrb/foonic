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
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Fooyin {
class NetworkAccessManager;
class PlaylistInteractor;
class SettingsManager;

namespace Subsonic {
class SubsonicClient;
class SubsonicLibraryDownloader;

enum class BrowserAction : uint8_t
{
    Play = 0,
    AddToPlaylist,
    PlayNext,
    AddToQueue,
    Download,
};

enum class MatchMode
{
    Contains = 0,
    AllWords,
    AnyWord,
};

/*!
 * The Subsonic artist/album/song browser, usable in a dialog or embedded in a layout widget.
 */
class SubsonicBrowser : public QWidget
{
    Q_OBJECT

public:
    SubsonicBrowser(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                    SubsonicLibraryDownloader* libraryDownloads, SettingsManager* settings,
                    QWidget* parent = nullptr);

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
    void onCollapsed(QTreeWidgetItem* item);
    void restoreExpansion(QTreeWidgetItem* parent);
    void scheduleScrollRestore();
    void ensureChildrenLoaded(QTreeWidgetItem* item, bool expand = true);
    [[nodiscard]] TrackList tracksForDrag(const QList<QTreeWidgetItem*>& items);
    bool ensureDragData(const QList<QTreeWidgetItem*>& items);
    void cancelDragFetch();
    void showContextMenu(const QPoint& pos);
    void showHeaderMenu(const QPoint& pos);
    void showDiscoveryMenu(const QPoint& pos);
    void showDiscoverySongs(const QString& title, const QList<QJsonObject>& songs);
    void createServerPlaylist(const QStringList& songIds);
    void addSongsToServerPlaylist(const QString& id, const QStringList& songIds);
    void renameSelectedPlaylist(QTreeWidgetItem* item);
    void deleteSelectedPlaylist(QTreeWidgetItem* item);
    void playSelection(BrowserAction action);

    void loadChildren(QTreeWidgetItem* item, int kind, const QString& id, std::function<void()> onDone = {},
                      bool expand = true);
    void populateChildren(QTreeWidgetItem* item, int kind, const QJsonObject& object, bool expand);
    void addPlaceholder(QTreeWidgetItem* item, const QString& text = {});
    void gatherTracks(const QList<QTreeWidgetItem*>& items, const std::shared_ptr<GatherState>& state);
    void finishGather(const std::shared_ptr<GatherState>& state);
    void downloadToLibrary(const std::shared_ptr<GatherState>& state);
    void applyAction(const TrackList& tracks, BrowserAction action, bool startPlayback);
    [[nodiscard]] Track trackForSong(const QJsonObject& song, const QString& filepath) const;
    [[nodiscard]] QString coverCachePath(const QString& coverArtId) const;
    [[nodiscard]] QString downloadedText(const QJsonObject& song) const;
    void updateDownloadedColumn();
    void applyCover(QTreeWidgetItem* item, const QString& coverArtId);
    void requestCover(const QString& coverArtId);
    void applyCoverToTree(const QString& coverArtId);
    void applyFilter();
    void clearTree();
    void rebuildArtistTree();
    void addPlaylistsHeader();
    void startPlaylistScan();
    void pumpPlaylistScan();
    void renderSearchView();
    void buildSearchTree(const QJsonObject& result);
    QTreeWidgetItem* createResultHeader(const QString& title);
    QTreeWidgetItem* createPlaylistItem(QTreeWidgetItem* parent, const QJsonObject& playlist);
    QTreeWidgetItem* createArtistItem(const QJsonObject& artist, QTreeWidgetItem* parent = nullptr);
    QTreeWidgetItem* createAlbumItem(QTreeWidgetItem* parent, const QJsonObject& album);
    QTreeWidgetItem* createSongItem(QTreeWidgetItem* parent, const QJsonObject& song);
    void setStatus(const QString& text);

    std::shared_ptr<NetworkAccessManager> m_network;
    PlaylistInteractor* m_interactor;
    SubsonicLibraryDownloader* m_libraryDownloads;
    SettingsManager* m_settings;
    SubsonicClient* m_client;
    QLabel* m_status;
    QTreeWidget* m_tree;
    QLineEdit* m_filter{nullptr};
    QToolButton* m_filterOptions{nullptr};
    QTimer* m_filterTimer{nullptr};
    QString m_filterText;
    bool m_searchArtists{true};
    bool m_searchAlbums{true};
    bool m_searchSongs{true};
    bool m_searchPlaylists{true};
    MatchMode m_matchMode{MatchMode::Contains};
    std::map<QTreeWidgetItem*, std::vector<std::function<void()>>> m_pendingLoads;
    std::set<QTreeWidgetItem*> m_loading;
    QHash<QString, QPixmap> m_coverCache;
    QSet<QString> m_coverRequests;
    QList<QJsonObject> m_artists;
    QList<QJsonObject> m_playlists;
    QHash<QString, QJsonArray> m_playlistEntries;
    QStringList m_playlistScanQueue;
    int m_playlistScanToken{0};
    int m_playlistScanActive{0};
    int m_playlistScanTotal{0};
    int m_playlistScanDone{0};
    QJsonObject m_searchResult;
    bool m_searchResultReady{false};
    QHash<QString, QJsonArray> m_albumsByArtist;
    QHash<QString, QJsonArray> m_songsByAlbum;
    int m_searchGeneration{0};
    bool m_filterMode{false};
    bool m_applyingFilter{false};
    int m_generation{0};
    int m_treeGeneration{0};
    bool m_dragFetching{false};
    int m_dragFetchToken{0};
    // Remembered across refresh: which items were expanded, and the scroll offset to restore.
    QSet<QString> m_expandedKeys;
    int m_pendingScroll{-1};
    QTimer* m_scrollRestoreTimer{nullptr};
};
} // namespace Subsonic
} // namespace Fooyin
