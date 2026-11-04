/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "subsonicfilter.h"

#include <core/track.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QHash>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

class QImage;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QStackedWidget;
class QThreadPool;
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
class SubsonicDetailsPanel;
class SubsonicLibraryDownloader;

enum class BrowserAction : uint8_t
{
    Play = 0,
    AddToPlaylist,
    PlayNext,
    AddToQueue,
    Download,
    AddToServerPlaylist,
    CreateServerPlaylist,
};

enum class ArtistSort : uint8_t
{
    ServerOrder = 0,
    Name,
    NameDescending,
    AlbumCount,
};

enum class PlaylistSort : uint8_t
{
    ServerOrder = 0,
    Name,
    Created,
    Changed,
};

enum class AlbumSort : uint8_t
{
    // Local: sorts each artist's album children.
    ServerOrder = 0,
    Name,
    Year,
    SongCount,
    // Server: replaces the whole view with a getAlbumList2 album listing.
    Newest,
    Recent,
    Frequent,
    Highest,
    Starred,
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
        QString playlistId;
        QString playlistName;
    };

    void onExpanded(QTreeWidgetItem* item);
    void onCollapsed(QTreeWidgetItem* item);
    void restoreExpansion(QTreeWidgetItem* parent);
    void scheduleScrollRestore();
    void ensureChildrenLoaded(QTreeWidgetItem* item, bool expand = true);
    [[nodiscard]] TrackList tracksForDrag(const QList<QTreeWidgetItem*>& items);
    bool ensureDragData(const QList<QTreeWidgetItem*>& items);
    void cancelDragFetch();
    [[nodiscard]] TrackList tracksForGridDrag(const QList<QListWidgetItem*>& items);
    bool ensureGridDragData(const QList<QListWidgetItem*>& items);
    void cancelGridDragFetch();
    void showContextMenu(const QPoint& pos);
    void showHeaderMenu(const QPoint& pos);
    void showDiscoveryMenu(const QPoint& pos);
    void showDiscoverySongs(const QString& title, const QJsonArray& songs);
    void showServerAlbumList(const QString& type, int offset = 0);
    void createServerPlaylistFromSelection();
    void addSongsToServerPlaylistFromSelection(const QString& id);
    void renameSelectedPlaylist(QTreeWidgetItem* item);
    void deleteSelectedPlaylist(QTreeWidgetItem* item);
    void removeSongsFromPlaylist(QTreeWidgetItem* item);
    void clearServerPlaylist(QTreeWidgetItem* item);
    void playSelection(BrowserAction action);
    void startRadio(QTreeWidgetItem* item);
    void starSelection(bool star);
    void rateSelection(int rating);

    void loadChildren(QTreeWidgetItem* item, int kind, const QString& id, std::function<void()> onDone = {},
                      bool expand = true);
    void populateChildren(QTreeWidgetItem* item, int kind, const QJsonObject& object, bool expand);
    void addPlaceholder(QTreeWidgetItem* item, const QString& text = {});
    void gatherTracks(const QList<QTreeWidgetItem*>& items, const std::shared_ptr<GatherState>& state);
    void finishGather(const std::shared_ptr<GatherState>& state);
    void downloadToLibrary(const std::shared_ptr<GatherState>& state);
    void applyAction(const TrackList& tracks, BrowserAction action, bool startPlayback);
    [[nodiscard]] Track trackForSong(const QJsonObject& song, const QString& filepath) const;
    [[nodiscard]] QString downloadedText(const QJsonObject& song) const;
    [[nodiscard]] QString albumSortLabel(AlbumSort mode) const;
    void clearFilter();
    void updateDownloadedItem(const QString& songId);
    void applyCover(QTreeWidgetItem* item, const QString& coverArtId);
    void requestCover(const QString& coverArtId);
    void pumpCoverRequests();
    void startCoverDownload(const QString& coverArtId);
    void applyDecodedCover(const QString& coverArtId, const QImage& small, const QImage& grid);
    void finishCover(const QString& coverArtId);
    void applyCoverToTree(const QString& coverArtId);
    void applyCoverToGrid(const QString& coverArtId);
    void unregisterItem(QTreeWidgetItem* item);
    void applyFilter();
    void clearTree();
    void rebuildArtistTree();
    void addHomeHeader();
    void addPlaylistsHeader();
    void startPlaylistScan();
    void pumpPlaylistScan();
    void renderSearchView();
    void buildSearchTree(const QJsonObject& result);
    QTreeWidgetItem* createResultHeader(const QString& title, QTreeWidgetItem* parent = nullptr);
    QTreeWidgetItem* createPlaylistItem(QTreeWidgetItem* parent, const QJsonObject& playlist);
    QTreeWidgetItem* createArtistItem(const QJsonObject& artist, QTreeWidgetItem* parent = nullptr);
    QTreeWidgetItem* createAlbumItem(QTreeWidgetItem* parent, const QJsonObject& album);
    QTreeWidgetItem* createSongItem(QTreeWidgetItem* parent, const QJsonObject& song);
    void setStatus(const QString& text);

    void updateDetails(QTreeWidgetItem* item);
    void updateItemStar(QTreeWidgetItem* item, bool star);
    void updateItemRating(QTreeWidgetItem* item, int rating);
    void updateTreeItemJson(const QString& id, const QJsonObject& object);
    void updateCachedJson(const QString& id, const QJsonObject& object);
    void saveHeaderState();
    void restoreHeaderState();

    void setGridMode(bool enabled);
    void loadGridAlbums(int offset = 0);
    void populateGrid(const QJsonArray& albums, int offset);
    void clearGrid();
    void queueGridCovers();
    void showGridContextMenu(const QPoint& pos);
    void playGridAlbum(QListWidgetItem* item, BrowserAction action);
    void addGridAlbumToServerPlaylist(QListWidgetItem* item, const QString& playlistId);
    void createServerPlaylistFromGridAlbum(QListWidgetItem* item);
    void updateGridDetails(QListWidgetItem* item);
    void updateGridAlbumStar(QListWidgetItem* item, bool star);
    void updateGridAlbumRating(QListWidgetItem* item, int rating);

    std::shared_ptr<NetworkAccessManager> m_network;
    PlaylistInteractor* m_interactor;
    SubsonicLibraryDownloader* m_libraryDownloads;
    SettingsManager* m_settings;
    SubsonicClient* m_client;
    QLabel* m_status;
    QTreeWidget* m_tree;
    QStackedWidget* m_stack;
    QListWidget* m_grid;
    QSplitter* m_splitter;
    SubsonicDetailsPanel* m_details;
    QToolButton* m_gridButton;
    QToolButton* m_detailsButton;
    QLineEdit* m_filter{nullptr};
    QToolButton* m_filterOptions{nullptr};
    QTimer* m_filterTimer{nullptr};
    QTimer* m_headerSaveTimer{nullptr};
    QString m_filterText;
    bool m_searchArtists{true};
    bool m_searchAlbums{true};
    bool m_searchSongs{true};
    bool m_searchPlaylists{true};
    bool m_searchPlaylistTracks{false};
    MatchMode m_matchMode{MatchMode::Contains};
    ArtistSort m_artistSort{ArtistSort::ServerOrder};
    PlaylistSort m_playlistSort{PlaylistSort::ServerOrder};
    AlbumSort m_albumSort{AlbumSort::ServerOrder};
    QString m_albumListTitle; // menu label of the active server album list
    QTreeWidgetItem* m_albumListHeader{nullptr};
    QTreeWidgetItem* m_albumListMore{nullptr};
    int m_albumListLoaded{0};
    bool m_albumListLoading{false};
    QTreeWidgetItem* m_homeHeader{nullptr};
    QTreeWidgetItem* m_playlistsHeader{nullptr};
    QTreeWidgetItem* m_discoveryHeader{nullptr};
    std::map<QTreeWidgetItem*, std::vector<std::function<void()>>> m_pendingLoads;
    std::set<QTreeWidgetItem*> m_loading;
    mutable QHash<QString, QString> m_downloadedCache; // song id -> Downloaded column text
    QHash<QString, QList<QTreeWidgetItem*>> m_itemsByCover;
    QHash<QString, QList<QTreeWidgetItem*>> m_songItemsById;
    QHash<QString, QList<QListWidgetItem*>> m_gridItemsByCover;
    QList<QPair<QAction*, bool*>> m_scopeActions; // search-scope actions and the flag they toggle
    QList<QAction*> m_matchActions;
    QSet<QString> m_coverPending;
    std::deque<QString> m_coverQueue;
    QThreadPool* m_coverPool{nullptr};
    int m_coverActive{0};
    int m_coverCacheActive{0};
    int m_coverDecoding{0};
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
    bool m_gridDragFetching{false};
    int m_gridDragFetchToken{0};
    // Remembered across refresh: which items were expanded, and the scroll offset to restore.
    QSet<QString> m_expandedKeys;
    int m_pendingScroll{-1};
    QTimer* m_scrollRestoreTimer{nullptr};
    // Album grid.
    int m_gridLoaded{0};
    bool m_gridLoading{false};
    bool m_gridLoadedOnce{false};
    bool m_gridMode{false};
};
} // namespace Subsonic
} // namespace Fooyin
