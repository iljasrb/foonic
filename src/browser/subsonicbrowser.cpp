/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicbrowser.h"

#include "subsonicclient.h"
#include "subsoniccovercache.h"
#include "subsonicdetailspanel.h"
#include "subsoniclibrarydownloader.h"
#include "subsonicnetwork.h"
#include "subsonicsettings.h"
#include "subsonictrack.h"

#include <gui/playlist/playlistinteractor.h>
#include <gui/trackmimedata.h>
#include <core/network/networkaccessmanager.h>
#include <core/player/playercontroller.h>

#include <utils/id.h>
#include <utils/settings/settingsdialogcontroller.h>

#include <QActionGroup>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
constexpr int KindRole   = Qt::UserRole;
constexpr int IdRole     = Qt::UserRole + 1;
constexpr int JsonRole   = Qt::UserRole + 2;
constexpr int LoadedRole = Qt::UserRole + 3;
constexpr int CoverRole  = Qt::UserRole + 4;

constexpr int GridJsonRole  = Qt::UserRole;
constexpr int GridKindRole  = Qt::UserRole + 1;
constexpr int GridCoverRole = Qt::UserRole + 2;

enum ItemKind : uint8_t
{
    Placeholder = 0,
    Artist      = 1,
    Album       = 2,
    Song        = 3,
    ResultHeader = 4,
    PlaylistNode = 5,
    LoadMore     = 6,
    HomeShelf    = 7,
    Genre        = 8,
};

enum Column : uint8_t
{
    NameColumn = 0,
    ArtistColumn,
    AlbumColumn,
    LengthColumn,
    SizeColumn,
    RatingColumn,
    StarredColumn,
    DownloadedColumn,
    ColumnCount,
};

enum GridKind : uint8_t
{
    GridAlbum = 0,
    GridLoadMore,
};

// Cap concurrent playlist-entry requests during a search scan so a large playlist library doesn't
// flood the server.
constexpr int MaxPlaylistFetches = 4;
// Page size for getAlbumList2 album listings (the "Load more" row fetches the next page).
constexpr int AlbumPageSize = 100;
constexpr int GridPageSize  = 200;
constexpr int MaxCoverRequests = 4;

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

qint64 songSize(const QJsonObject& song)
{
    return static_cast<qint64>(song.value(u"size"_s).toDouble());
}

QString ratingText(int rating)
{
    if(rating <= 0) {
        return {};
    }
    return QString(rating, u'★') + QString(5 - rating, u'☆');
}

//! Returns the best id to pass to getCoverArt. Some servers omit `coverArt` but accept the
//! album/song id directly.
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

struct CoverCacheResult
{
    QImage small;
    QImage grid;
    bool hit{false};
};

//! Checks the disk cache and builds the small/grid variants off the GUI thread.
class CoverCacheTask : public QRunnable
{
public:
    CoverCacheTask(QString coverArtId, std::function<void(CoverCacheResult)> done)
        : m_coverArtId{std::move(coverArtId)}
        , m_done{std::move(done)}
    { }

    void run() override
    {
        CoverCacheResult result;
        QImage image;
        if(image.load(Fooyin::Subsonic::coverCachePath(m_coverArtId, Fooyin::Subsonic::GridCoverSize))
           || image.load(Fooyin::Subsonic::coverCachePath(m_coverArtId, Fooyin::Subsonic::CoverCacheSize))) {
            result.small = image.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            result.grid  = image.scaled(Fooyin::Subsonic::GridCoverSize, Fooyin::Subsonic::GridCoverSize,
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
            result.hit   = true;
        }
        m_done(std::move(result));
    }

private:
    QString m_coverArtId;
    std::function<void(CoverCacheResult)> m_done;
};

struct CoverDecodeResult
{
    QImage small;
    QImage grid;
    bool ok{false};
};

//! Decodes and scales a cover off the GUI thread; saving the grid copy also happens here.
class CoverDecodeTask : public QRunnable
{
public:
    CoverDecodeTask(QByteArray data, QString coverArtId, std::function<void(CoverDecodeResult)> done)
        : m_data{std::move(data)}
        , m_coverArtId{std::move(coverArtId)}
        , m_done{std::move(done)}
    { }

    void run() override
    {
        CoverDecodeResult result;
        QImage image;
        if(image.loadFromData(m_data)) {
            Fooyin::Subsonic::saveCachedCover(m_coverArtId, image, Fooyin::Subsonic::GridCoverSize);
            result.small = image.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            result.grid  = image.scaled(Fooyin::Subsonic::GridCoverSize, Fooyin::Subsonic::GridCoverSize,
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
            result.ok    = true;
        }
        m_done(std::move(result));
    }

private:
    QByteArray m_data;
    QString m_coverArtId;
    std::function<void(CoverDecodeResult)> m_done;
};

//! Neutral tile shown until a cover arrives, so rows don't look broken or half-loaded.
QIcon placeholderCoverIcon()
{
    static const QIcon icon = [] {
        QPixmap pixmap{300, 300};
        pixmap.fill(Qt::transparent);

        QPainter painter{&pixmap};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(128, 128, 128, 36));
        painter.drawRoundedRect(pixmap.rect().adjusted(8, 8, -8, -8), 18, 18);

        painter.setPen(QColor(128, 128, 128, 150));
        QFont font = painter.font();
        font.setPointSize(72);
        painter.setFont(font);
        painter.drawText(pixmap.rect(), Qt::AlignCenter, u"♪"_s);
        return QIcon{pixmap};
    }();
    return icon;
}

QString starredText(const QJsonObject& object)
{
    return object.value(u"starred"_s).toString().isEmpty() ? QString{} : u"★"_s;
}

bool isStarred(const QJsonObject& object)
{
    return !object.value(u"starred"_s).toString().isEmpty();
}

int userRating(const QJsonObject& object)
{
    return object.value(u"userRating"_s).toInt();
}

// Stable identity of an expandable tree item, used to remember open/closed state across refreshes.
QString expansionKey(const QTreeWidgetItem* item)
{
    const int kind = item->data(0, KindRole).toInt();
    if(kind != Artist && kind != Album && kind != PlaylistNode && kind != HomeShelf && kind != Genre) {
        return {};
    }
    return QString::number(kind) + u":"_s + item->data(0, IdRole).toString();
}

class SubsonicTreeWidget : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

    std::function<Fooyin::TrackList(const QList<QTreeWidgetItem*>&)> mimeDataProvider;
    // Returns true if @p items are ready to drag; otherwise starts a fetch and returns false. The
    // owner then calls resumeDrag() when it finishes, or cancelDragFetch() if the drag is abandoned.
    std::function<bool(const QList<QTreeWidgetItem*>&)> dragDataReady;
    std::function<void()> cancelDragFetch;

    void resumeDrag()
    {
        if(m_pendingActions == Qt::IgnoreAction) {
            return;
        }
        const Qt::DropActions actions = m_pendingActions;
        m_pendingActions              = Qt::IgnoreAction;
        QTreeWidget::startDrag(actions);
    }

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if(mimeDataProvider) {
            if(auto tracks = mimeDataProvider(items); !tracks.empty()) {
                return new Fooyin::TrackMimeData{std::move(tracks)};
            }
        }
        return QTreeWidget::mimeData(items);
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        if(m_pendingActions != Qt::IgnoreAction) {
            return; // already waiting on a fetch triggered by an earlier move
        }
        if(dragDataReady && !dragDataReady(selectedItems())) {
            m_pendingActions = supportedActions;
            return;
        }
        QTreeWidget::startDrag(supportedActions);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if(m_pendingActions != Qt::IgnoreAction) {
            m_pendingActions = Qt::IgnoreAction; // drag abandoned before its data arrived
            if(cancelDragFetch) {
                cancelDragFetch();
            }
        }
        QTreeWidget::mouseReleaseEvent(event);
    }

private:
    Qt::DropActions m_pendingActions{Qt::IgnoreAction};
};

class SubsonicGridWidget : public QListWidget
{
public:
    using QListWidget::QListWidget;

    std::function<Fooyin::TrackList(const QList<QListWidgetItem*>&)> mimeDataProvider;
    std::function<bool(const QList<QListWidgetItem*>&)> dragDataReady;
    std::function<void()> cancelDragFetch;

    void resumeDrag()
    {
        if(m_pendingActions == Qt::IgnoreAction) {
            return;
        }
        const Qt::DropActions actions = m_pendingActions;
        m_pendingActions              = Qt::IgnoreAction;
        startDrag(actions); // re-check dragDataReady in case the selection changed
    }

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if(mimeDataProvider) {
            if(auto tracks = mimeDataProvider(items); !tracks.empty()) {
                return new Fooyin::TrackMimeData{std::move(tracks)};
            }
        }
        return QListWidget::mimeData(items);
    }

    void startDrag(Qt::DropActions supportedActions) override
    {
        if(m_pendingActions != Qt::IgnoreAction) {
            return;
        }
        if(dragDataReady && !dragDataReady(selectedItems())) {
            m_pendingActions = supportedActions;
            return;
        }
        QListWidget::startDrag(supportedActions);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        // Don't start a rubber-band selection from empty space; it fights with item drags.
        if(event->button() == Qt::LeftButton && !itemAt(event->pos())) {
            clearSelection();
            event->accept();
            return;
        }
        QListWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if(m_pendingActions != Qt::IgnoreAction) {
            m_pendingActions = Qt::IgnoreAction;
            if(cancelDragFetch) {
                cancelDragFetch();
            }
        }
        QListWidget::mouseReleaseEvent(event);
    }

private:
    Qt::DropActions m_pendingActions{Qt::IgnoreAction};
};
} // namespace

namespace Fooyin::Subsonic {

namespace {
// OpenSubsonic sortName drops leading articles ("The Beatles" -> "Beatles").
QString artistSortName(const QJsonObject& artist)
{
    const QString sortName = artist.value(u"sortName"_s).toString();
    return sortName.isEmpty() ? artist.value(u"name"_s).toString() : sortName;
}

void sortArtists(QList<QJsonObject>& artists, ArtistSort mode)
{
    switch(mode) {
        case ArtistSort::ServerOrder:
            return;
        case ArtistSort::Name:
        case ArtistSort::NameDescending:
            std::stable_sort(artists.begin(), artists.end(), [mode](const QJsonObject& a, const QJsonObject& b) {
                const int cmp = artistSortName(a).compare(artistSortName(b), Qt::CaseInsensitive);
                return mode == ArtistSort::Name ? cmp < 0 : cmp > 0;
            });
            return;
        case ArtistSort::AlbumCount:
            std::stable_sort(artists.begin(), artists.end(), [](const QJsonObject& a, const QJsonObject& b) {
                return a.value(u"albumCount"_s).toInt() > b.value(u"albumCount"_s).toInt();
            });
            return;
    }
}

qint64 isoTimestamp(const QJsonObject& object, const QString& field)
{
    return QDateTime::fromString(object.value(field).toString(), Qt::ISODate).toMSecsSinceEpoch();
}

void sortPlaylists(QList<QJsonObject>& playlists, PlaylistSort mode)
{
    switch(mode) {
        case PlaylistSort::ServerOrder:
            return;
        case PlaylistSort::Name:
            std::stable_sort(playlists.begin(), playlists.end(), [](const QJsonObject& a, const QJsonObject& b) {
                return a.value(u"name"_s).toString().compare(b.value(u"name"_s).toString(), Qt::CaseInsensitive) < 0;
            });
            return;
        case PlaylistSort::Created:
        case PlaylistSort::Changed: {
            const QString field = (mode == PlaylistSort::Created) ? u"created"_s : u"changed"_s;
            std::stable_sort(playlists.begin(), playlists.end(), [field](const QJsonObject& a, const QJsonObject& b) {
                return isoTimestamp(a, field) > isoTimestamp(b, field); // newest first
            });
            return;
        }
    }
}

QList<QJsonObject> sortedAlbums(const QJsonArray& array, AlbumSort mode)
{
    QList<QJsonObject> albums;
    albums.reserve(array.size());
    for(const QJsonValue& value : array) {
        albums.push_back(value.toObject());
    }

    switch(mode) {
        case AlbumSort::ServerOrder:
            return albums;
        case AlbumSort::Name:
            std::stable_sort(albums.begin(), albums.end(), [](const QJsonObject& a, const QJsonObject& b) {
                return a.value(u"name"_s).toString().compare(b.value(u"name"_s).toString(), Qt::CaseInsensitive) < 0;
            });
            return albums;
        case AlbumSort::Year:
            std::stable_sort(albums.begin(), albums.end(), [](const QJsonObject& a, const QJsonObject& b) {
                return a.value(u"year"_s).toInt() < b.value(u"year"_s).toInt();
            });
            return albums;
        case AlbumSort::SongCount:
            std::stable_sort(albums.begin(), albums.end(), [](const QJsonObject& a, const QJsonObject& b) {
                return a.value(u"songCount"_s).toInt() > b.value(u"songCount"_s).toInt();
            });
            return albums;
        default:
            return albums; // server-ordered lists are shown as returned
    }
}

// getAlbumList2 type for a server-side album list, empty for the local per-artist sorts.
QString serverAlbumType(AlbumSort mode)
{
    switch(mode) {
        case AlbumSort::Newest:
            return u"newest"_s;
        case AlbumSort::Recent:
            return u"recent"_s;
        case AlbumSort::Frequent:
            return u"frequent"_s;
        case AlbumSort::Highest:
            return u"highest"_s;
        case AlbumSort::Starred:
            return u"starred"_s;
        default:
            return {};
    }
}
} // namespace
SubsonicBrowser::SubsonicBrowser(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                                 SubsonicLibraryDownloader* libraryDownloads, SettingsManager* settings,
                                 QWidget* parent)
    : QWidget{parent}
    , m_network{std::move(network)}
    , m_interactor{interactor}
    , m_libraryDownloads{libraryDownloads}
    , m_settings{settings}
    , m_client{new SubsonicClient{m_network, loadSubsonicSettings(settings), this}}
    , m_status{new QLabel{this}}
    , m_tree{new SubsonicTreeWidget{this}}
{
    // Restore the saved sort choices (values are clamped in case the settings file was edited).
    const auto savedSort = [this](const QString& key, int maxValue) {
        return std::clamp(m_settings->value(key).toInt(), 0, maxValue);
    };
    m_artistSort = static_cast<ArtistSort>(
        savedSort(u"Subsonic/ArtistSort"_s, static_cast<int>(ArtistSort::AlbumCount)));
    m_albumSort = static_cast<AlbumSort>(savedSort(u"Subsonic/AlbumSort"_s, static_cast<int>(AlbumSort::Starred)));
    m_playlistSort = static_cast<PlaylistSort>(
        savedSort(u"Subsonic/PlaylistSort"_s, static_cast<int>(PlaylistSort::Changed)));
    m_matchMode = static_cast<MatchMode>(savedSort(u"Subsonic/MatchMode"_s, static_cast<int>(MatchMode::AnyWord)));
    m_searchArtists        = m_settings->value(u"Subsonic/SearchArtists"_s).toBool();
    m_searchAlbums         = m_settings->value(u"Subsonic/SearchAlbums"_s).toBool();
    m_searchSongs          = m_settings->value(u"Subsonic/SearchSongs"_s).toBool();
    m_searchPlaylists      = m_settings->value(u"Subsonic/SearchPlaylists"_s).toBool();
    m_searchPlaylistTracks = m_settings->value(u"Subsonic/SearchPlaylistTracks"_s).toBool();
    m_albumListTitle       = albumSortLabel(m_albumSort);

    auto* tree = static_cast<SubsonicTreeWidget*>(m_tree);
    tree->mimeDataProvider = [this](const QList<QTreeWidgetItem*>& items) { return tracksForDrag(items); };
    tree->dragDataReady  = [this](const QList<QTreeWidgetItem*>& items) { return ensureDragData(items); };
    tree->cancelDragFetch = [this] { cancelDragFetch(); };

    if(m_libraryDownloads) {
        connect(m_libraryDownloads, &SubsonicLibraryDownloader::progress, this,
                [this](int completed, int total, int percent, const QString& current) {
                    const QString progress = percent >= 0 ? tr(" (%1%)").arg(percent) : QString{};
                    setStatus(tr("Downloading %1/%2%3: %4")
                                  .arg(completed + 1)
                                  .arg(total)
                                  .arg(progress)
                                  .arg(current));
                });
        connect(m_libraryDownloads, &SubsonicLibraryDownloader::finished, this,
                [this](int completed, int failed) {
                    setStatus(failed > 0 ? tr("Downloaded %1 track(s); %2 failed.").arg(completed).arg(failed)
                                        : tr("Downloaded %1 track(s) to the library.").arg(completed));
                });
        connect(m_libraryDownloads, &SubsonicLibraryDownloader::warning, this,
                [this](const QString& message) { setStatus(message); });
        connect(m_libraryDownloads, &SubsonicLibraryDownloader::downloaded, this,
                [this](const QString& songId, const QJsonObject&, const QString&) {
                    m_downloadedCache.remove(songId); // re-probe this one, then refresh its rows
                    updateDownloadedItem(songId);
                });
    }

    auto* refreshButton = new QPushButton{tr("Refresh"), this};

    m_filter = new QLineEdit{this};
    m_filter->setPlaceholderText(tr("Filter..."));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(220);

    m_filterOptions = new QToolButton{this};
    m_filterOptions->setPopupMode(QToolButton::InstantPopup);
    m_filterOptions->setText(tr("Filter options"));
    m_filterOptions->setToolTip(tr("Search scope and matching.\n"
                                   "Fields: artist: album: title: genre: playlist: year:\n"
                                   "Prefix a term with '-' to exclude it."));

    auto* filterMenu = new QMenu{m_filterOptions};

    QMenu* searchInMenu = filterMenu->addMenu(tr("Search in"));
    const auto addScope = [this, searchInMenu](const QString& text, bool& flag, const QString& key) {
        auto* action = searchInMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(flag);
        m_scopeActions.push_back({action, &flag});
        connect(action, &QAction::toggled, this, [this, flagPtr = &flag, key](bool on) {
            *flagPtr = on;
            m_settings->set(key, on);
            applyFilter();
        });
    };
    addScope(tr("Artists"), m_searchArtists, u"Subsonic/SearchArtists"_s);
    addScope(tr("Albums"), m_searchAlbums, u"Subsonic/SearchAlbums"_s);
    addScope(tr("Songs"), m_searchSongs, u"Subsonic/SearchSongs"_s);
    addScope(tr("Playlists"), m_searchPlaylists, u"Subsonic/SearchPlaylists"_s);

    auto* playlistTracksAction = searchInMenu->addAction(tr("Playlist tracks"));
    playlistTracksAction->setCheckable(true);
    playlistTracksAction->setChecked(m_searchPlaylistTracks);
    playlistTracksAction->setToolTip(
        tr("Also search the contents of every playlist. This fetches each playlist from the server, "
           "so it is off by default on large libraries."));
    connect(playlistTracksAction, &QAction::toggled, this, [this](bool on) {
        m_searchPlaylistTracks = on;
        m_settings->set(u"Subsonic/SearchPlaylistTracks"_s, on);
        applyFilter();
    });

    QMenu* matchMenu = filterMenu->addMenu(tr("Matching"));
    auto* matchGroup = new QActionGroup{matchMenu};
    matchGroup->setExclusive(true);
    const auto addMatch = [this, matchMenu, matchGroup](const QString& text, MatchMode mode) {
        auto* action = matchMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_matchMode == mode);
        action->setData(static_cast<int>(mode));
        m_matchActions.push_back(action);
        matchGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            m_matchMode = mode;
            m_settings->set(u"Subsonic/MatchMode"_s, static_cast<int>(mode));
            applyFilter();
        });
    };
    addMatch(tr("Contains"), MatchMode::Contains);
    addMatch(tr("All words"), MatchMode::AllWords);
    addMatch(tr("Any word"), MatchMode::AnyWord);

    filterMenu->addSeparator();
    connect(filterMenu->addAction(tr("Clear filter")), &QAction::triggered, this, &SubsonicBrowser::clearFilter);
    filterMenu->addSeparator();

    QMenu* artistSortMenu = filterMenu->addMenu(tr("Sort artists"));
    auto* sortGroup = new QActionGroup{artistSortMenu};
    sortGroup->setExclusive(true);
    const auto addSort = [this, artistSortMenu, sortGroup](const QString& text, ArtistSort mode) {
        auto* action = artistSortMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_artistSort == mode);
        sortGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            m_artistSort = mode;
            m_settings->set(u"Subsonic/ArtistSort"_s, static_cast<int>(mode));
            if(!m_filterMode) {
                rebuildArtistTree();
            }
        });
    };
    addSort(tr("Server order"), ArtistSort::ServerOrder);
    addSort(tr("Name (A-Z)"), ArtistSort::Name);
    addSort(tr("Name (Z-A)"), ArtistSort::NameDescending);
    addSort(tr("Most albums"), ArtistSort::AlbumCount);

    QMenu* playlistSortMenu = filterMenu->addMenu(tr("Sort playlists"));
    auto* playlistSortGroup = new QActionGroup{playlistSortMenu};
    playlistSortGroup->setExclusive(true);
    const auto addPlaylistSort = [this, playlistSortMenu, playlistSortGroup](const QString& text,
                                                                            PlaylistSort mode) {
        auto* action = playlistSortMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_playlistSort == mode);
        playlistSortGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            m_playlistSort = mode;
            m_settings->set(u"Subsonic/PlaylistSort"_s, static_cast<int>(mode));
            if(!m_filterMode) {
                rebuildArtistTree();
            }
        });
    };
    addPlaylistSort(tr("Server order"), PlaylistSort::ServerOrder);
    addPlaylistSort(tr("Name (A-Z)"), PlaylistSort::Name);
    addPlaylistSort(tr("Recently created"), PlaylistSort::Created);
    addPlaylistSort(tr("Recently changed"), PlaylistSort::Changed);

    QMenu* albumsMenu = filterMenu->addMenu(tr("Sort albums"));
    auto* albumSortGroup = new QActionGroup{albumsMenu};
    albumSortGroup->setExclusive(true);
    const auto addAlbumSort = [this, albumsMenu, albumSortGroup](const QString& text, AlbumSort mode) {
        auto* action = albumsMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_albumSort == mode);
        albumSortGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            m_albumSort      = mode;
            m_albumListTitle = albumSortLabel(mode);
            m_settings->set(u"Subsonic/AlbumSort"_s, static_cast<int>(mode));
            if(!m_filterMode) {
                rebuildArtistTree();
            }
        });
    };
    albumsMenu->addSection(tr("Inside each artist"));
    addAlbumSort(tr("Server order"), AlbumSort::ServerOrder);
    addAlbumSort(tr("Name (A-Z)"), AlbumSort::Name);
    addAlbumSort(tr("Release year"), AlbumSort::Year);
    addAlbumSort(tr("Most songs"), AlbumSort::SongCount);
    albumsMenu->addSection(tr("Whole view from server"));
    for(const AlbumSort mode : {AlbumSort::Newest, AlbumSort::Recent, AlbumSort::Frequent, AlbumSort::Highest,
                                AlbumSort::Starred}) {
        addAlbumSort(albumSortLabel(mode), mode);
    }
    m_filterOptions->setMenu(filterMenu);

    m_filterTimer = new QTimer{this};
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(150);

    m_coverPool = new QThreadPool{this};
    m_coverPool->setMaxThreadCount(2);

    m_gridButton = new QToolButton{this};
    m_gridButton->setText(tr("Grid"));
    m_gridButton->setCheckable(true);
    m_gridButton->setToolTip(tr("Show albums as a cover grid."));

    m_detailsButton = new QToolButton{this};
    m_detailsButton->setText(tr("Details"));
    m_detailsButton->setCheckable(true);
    m_detailsButton->setToolTip(tr("Show the details and lyrics panel."));

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(10, 8, 10, 0);
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(m_status, 1);
    toolbar->addWidget(m_gridButton);
    toolbar->addWidget(m_detailsButton);
    toolbar->addWidget(m_filterOptions);
    toolbar->addWidget(m_filter);

    m_tree->setHeaderLabels({tr("Name"), tr("Artist"), tr("Album"), tr("Length"), tr("Size"), tr("Rating"),
                             tr("Favourite"), tr("Downloaded")});
    m_tree->setUniformRowHeights(true);
    m_tree->setIconSize(QSize{24, 24});
    m_tree->setAlternatingRowColors(true);
    m_tree->setRootIsDecorated(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setDragEnabled(true);
    m_tree->setDragDropMode(QAbstractItemView::DragOnly);
    m_tree->setDefaultDropAction(Qt::CopyAction);
    m_tree->setToolTip(tr("Drag artists, albums or songs onto a playlist."));
    restoreHeaderState();

    m_grid = new SubsonicGridWidget{this};
    m_grid->setViewMode(QListView::IconMode);
    m_grid->setSelectionRectVisible(false);
    m_grid->setResizeMode(QListView::Adjust);
    m_grid->setMovement(QListView::Static);
    m_grid->setWordWrap(true);
    m_grid->setUniformItemSizes(true);
    m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_grid->setSpacing(0);
    m_grid->setIconSize(QSize{140, 140});
    m_grid->setGridSize(QSize{170, 210});
    m_grid->setDragEnabled(true);
    m_grid->setDragDropMode(QAbstractItemView::DragOnly);
    m_grid->setDefaultDropAction(Qt::CopyAction);
    m_grid->setToolTip(tr("Double-click an album to play it. Drag an album onto a playlist."));

    auto* grid = static_cast<SubsonicGridWidget*>(m_grid);
    grid->mimeDataProvider = [this](const QList<QListWidgetItem*>& items) { return tracksForGridDrag(items); };
    grid->dragDataReady  = [this](const QList<QListWidgetItem*>& items) { return ensureGridDragData(items); };
    grid->cancelDragFetch = [this] { cancelGridDragFetch(); };

    m_stack = new QStackedWidget{this};
    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_grid);

    m_details = new SubsonicDetailsPanel{m_network, m_settings, m_client, this};
    m_splitter = new QSplitter{Qt::Horizontal, this};
    m_splitter->addWidget(m_stack);
    m_splitter->addWidget(m_details);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setSizes({700, 300});

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins({});
    layout->addLayout(toolbar);
    layout->addWidget(m_splitter, 1);

    connect(refreshButton, &QPushButton::clicked, this, &SubsonicBrowser::refresh);
    connect(m_filterTimer, &QTimer::timeout, this, &SubsonicBrowser::applyFilter);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_filterText = text.trimmed();
        m_filterTimer->start();
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, &SubsonicBrowser::onExpanded);
    connect(m_tree, &QTreeWidget::itemCollapsed, this, &SubsonicBrowser::onCollapsed);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if(item && item->data(0, KindRole).toInt() == LoadMore) {
            const QString type = serverAlbumType(m_albumSort);
            if(!type.isEmpty() && !m_albumListLoading) {
                showServerAlbumList(type, m_albumListLoaded);
            }
            return;
        }
        ensureChildrenLoaded(item);
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if(!item) {
            return;
        }
        if(item->data(0, KindRole).toInt() == Song) {
            auto state    = std::make_shared<GatherState>();
            state->action = BrowserAction::Play;
            gatherTracks({item}, state);
        } else {
            item->setExpanded(true);
        }
    });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) { updateDetails(current); });

    m_headerSaveTimer = new QTimer{this};
    m_headerSaveTimer->setSingleShot(true);
    m_headerSaveTimer->setInterval(400);
    connect(m_headerSaveTimer, &QTimer::timeout, this, &SubsonicBrowser::saveHeaderState);
    connect(m_tree->header(), &QHeaderView::sectionResized, this, [this] { m_headerSaveTimer->start(); });
    connect(m_tree->header(), &QHeaderView::sectionMoved, this, [this] { m_headerSaveTimer->start(); });

    connect(m_gridButton, &QToolButton::toggled, this, &SubsonicBrowser::setGridMode);
    connect(m_detailsButton, &QToolButton::toggled, this, [this](bool on) {
        m_details->setVisible(on);
        m_settings->set(u"Subsonic/DetailsVisible"_s, on);
    });
    connect(m_grid, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) { updateGridDetails(current); });
    connect(m_grid, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if(item && item->data(GridKindRole).toInt() == GridLoadMore) {
            loadGridAlbums(m_gridLoaded);
            return;
        }
        playGridAlbum(item, BrowserAction::Play);
    });
    connect(m_grid, &QListWidget::customContextMenuRequested, this, &SubsonicBrowser::showGridContextMenu);

    m_gridButton->setChecked(m_settings->value(u"Subsonic/GridView"_s).toBool());
    m_detailsButton->setChecked(m_settings->value(u"Subsonic/DetailsVisible"_s).toBool());
    m_details->setVisible(m_detailsButton->isChecked());

    m_scrollRestoreTimer = new QTimer{this};
    m_scrollRestoreTimer->setSingleShot(true);
    connect(m_scrollRestoreTimer, &QTimer::timeout, this, [this] { m_pendingScroll = -1; });

    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &SubsonicBrowser::showContextMenu);
    m_tree->header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree->header(), &QWidget::customContextMenuRequested, this, &SubsonicBrowser::showHeaderMenu);

    // Refresh once the (modeless) settings dialog is closed so config changes take effect.
    if(auto* controller = m_settings->settingsDialog()) {
        connect(controller, &SettingsDialogController::closing, this, [this] {
            m_client->setSettings(loadSubsonicSettings(m_settings));
            refresh();
        });
    }

    if(loadSubsonicSettings(m_settings).isValid()) {
        refresh();
    } else {
        setStatus(tr("Set up a Subsonic server to get started."));
    }
}

void SubsonicBrowser::refresh()
{
    ++m_generation;
    ++m_gridDragFetchToken;
    m_gridDragFetching = false;
    // Remember where the user was so the rebuilt (and lazily reloaded) tree can restore it.
    m_pendingScroll = m_tree->verticalScrollBar()->value();
    clearTree();
    clearGrid();
    m_artists.clear();
    m_playlists.clear();
    m_playlistEntries.clear();
    m_downloadedCache.clear();
    m_playlistScanQueue.clear();
    ++m_playlistScanToken; // cancel any scan still in flight
    m_searchResultReady = false;
    m_albumsByArtist.clear();
    m_songsByAlbum.clear();
    m_filterMode = false;

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    if(!settings.isValid()) {
        setStatus(tr("Set up a Subsonic server to get started."));
        return;
    }

    m_client->setSettings(settings);
    setStatus(tr("Loading artists..."));

    // Cache the server's OpenSubsonic capabilities; formPost lets large playlist edits use POST.
    m_client->getOpenSubsonicExtensions([](const QJsonArray&, const QString&) {});
    if(m_gridMode && m_filterText.isEmpty()) {
        loadGridAlbums(0);
    }

    const int generation = m_generation;
    m_client->getPlaylists([this, generation](const QJsonArray& playlists, const QString& error) {
        if(generation != m_generation) {
            return;
        }
        if(!error.isEmpty()) {
            // Not fatal: some servers don't implement getPlaylists.
            qCWarning(SUBSONIC) << "getPlaylists failed:" << error;
            return;
        }
        for(const QJsonValue& value : playlists) {
            m_playlists.push_back(value.toObject());
        }
        if(!m_filterMode) {
            addPlaylistsHeader();
        }
        else if(m_searchPlaylistTracks) {
            startPlaylistScan();
        }
    });

    m_client->getArtists([this, generation](const QJsonArray& artists, const QString& error) {
        if(generation != m_generation) {
            return;
        }
        if(!error.isEmpty()) {
            setStatus(error);
            return;
        }

        m_artists.reserve(artists.size());
        for(const QJsonValue& value : artists) {
            m_artists.push_back(value.toObject());
        }

        if(!m_filterText.isEmpty()) {
            applyFilter();
        }
        else {
            rebuildArtistTree();
        }
    });
}

void SubsonicBrowser::openSettings()
{
    if(auto* controller = m_settings->settingsDialog()) {
        controller->openAtPage(Id{SettingsPageId});
    }
}

void SubsonicBrowser::onExpanded(QTreeWidgetItem* item)
{
    if(const QString key = expansionKey(item); !key.isEmpty()) {
        m_expandedKeys.insert(key);
    }
    ensureChildrenLoaded(item);
}

void SubsonicBrowser::onCollapsed(QTreeWidgetItem* item)
{
    if(const QString key = expansionKey(item); !key.isEmpty()) {
        m_expandedKeys.remove(key);
    }
}

void SubsonicBrowser::restoreExpansion(QTreeWidgetItem* parent)
{
    const int count = parent ? parent->childCount() : m_tree->topLevelItemCount();
    for(int i{0}; i < count; ++i) {
        QTreeWidgetItem* child = parent ? parent->child(i) : m_tree->topLevelItem(i);
        const QString key      = expansionKey(child);
        if(!key.isEmpty() && m_expandedKeys.contains(key)) {
            child->setExpanded(true); // re-triggers the lazy load, which restores deeper levels
        }
    }
    scheduleScrollRestore();
}

void SubsonicBrowser::scheduleScrollRestore()
{
    if(m_pendingScroll < 0) {
        return;
    }

    // Content keeps growing as children load asynchronously, so keep re-applying the offset until the
    // tree settles, then forget it.
    QTimer::singleShot(0, this, [this] {
        if(m_pendingScroll >= 0) {
            m_tree->verticalScrollBar()->setValue(m_pendingScroll);
        }
    });
    m_scrollRestoreTimer->start(1200);
}

void SubsonicBrowser::ensureChildrenLoaded(QTreeWidgetItem* item, bool expand)
{
    if(!item || item->data(0, LoadedRole).toBool()) {
        return;
    }

    const int kind = item->data(0, KindRole).toInt();
    if(kind != Artist && kind != Album && kind != PlaylistNode && kind != HomeShelf && kind != Genre) {
        return;
    }

    item->setData(0, LoadedRole, false);
    loadChildren(item, kind, item->data(0, IdRole).toString(), {}, expand);
}

bool SubsonicBrowser::ensureDragData(const QList<QTreeWidgetItem*>& items)
{
    m_client->setSettings(loadSubsonicSettings(m_settings));

    QStringList albumIds;
    QStringList playlistIds;
    QStringList artistIds;
    QList<QTreeWidgetItem*> lazyItems;

    std::function<void(const QList<QTreeWidgetItem*>&)> scan = [&](const QList<QTreeWidgetItem*>& list) {
        for(QTreeWidgetItem* item : list) {
            if(!item) {
                continue;
            }
            const QString id = item->data(0, IdRole).toString();
            const int kind   = item->data(0, KindRole).toInt();
            if(kind == Album) {
                if(!m_songsByAlbum.contains(id) && !albumIds.contains(id)) {
                    albumIds.push_back(id);
                }
            } else if(kind == PlaylistNode) {
                if(!m_playlistEntries.contains(id) && !playlistIds.contains(id)) {
                    playlistIds.push_back(id);
                }
            } else if(kind == Artist) {
                if(!m_albumsByArtist.contains(id) && !artistIds.contains(id)) {
                    artistIds.push_back(id);
                }
            } else if(kind == HomeShelf || kind == Genre) {
                if(!item->data(0, LoadedRole).toBool()) {
                    lazyItems.push_back(item);
                }
            } else if(kind == ResultHeader) {
                QList<QTreeWidgetItem*> children;
                for(int i{0}; i < item->childCount(); ++i) {
                    children.push_back(item->child(i));
                }
                scan(children);
            }
        }
    };
    scan(items);

    if(albumIds.isEmpty() && playlistIds.isEmpty() && artistIds.isEmpty()) {
        return true; // everything already cached: start the drag now
    }
    if(m_dragFetching) {
        return false; // a fetch is already in flight from an earlier drag attempt
    }
    m_dragFetching = true;
    const int token = ++m_dragFetchToken;

    auto remaining = std::make_shared<int>(0);
    auto finish    = [this, remaining, token] {
        if(token != m_dragFetchToken) {
            return; // superseded by a newer drag or cancelled
        }
        if(--*remaining == 0) {
            m_dragFetching = false;
            static_cast<SubsonicTreeWidget*>(m_tree)->resumeDrag();
        }
    };

    for(const QString& id : albumIds) {
        ++*remaining;
        m_client->getAlbum(id, [this, id, finish](const QJsonObject& object, const QString& error) {
            // Cache even on error (empty) so a failed album isn't retried on the next drag.
            m_songsByAlbum.insert(id, error.isEmpty() ? asArray(object.value(u"song"_s)) : QJsonArray{});
            finish();
        });
    }
    for(const QString& id : playlistIds) {
        ++*remaining;
        m_client->getPlaylist(id, [this, id, finish](const QJsonObject& object, const QString& error) {
            m_playlistEntries.insert(id, error.isEmpty() ? asArray(object.value(u"entry"_s)) : QJsonArray{});
            finish();
        });
    }
    for(const QString& id : artistIds) {
        ++*remaining;
        m_client->getArtist(id, [this, id, finish, remaining](const QJsonObject& object, const QString& error) {
            const QJsonArray albums = error.isEmpty() ? asArray(object.value(u"album"_s)) : QJsonArray{};
            // tracksForDrag builds an artist from this cache; without it the first drag found no
            // tracks until the async tree load finished.
            m_albumsByArtist.insert(id, albums);
            for(const QJsonValue& album : albums) {
                const QString albumId = album.toObject().value(u"id"_s).toString();
                if(albumId.isEmpty() || m_songsByAlbum.contains(albumId)) {
                    continue;
                }
                ++*remaining;
                m_client->getAlbum(albumId, [this, albumId, finish](const QJsonObject& a, const QString& e) {
                    m_songsByAlbum.insert(albumId, e.isEmpty() ? asArray(a.value(u"song"_s)) : QJsonArray{});
                    finish();
                });
            }
            finish();
        });
    }
    for(QTreeWidgetItem* item : std::as_const(lazyItems)) {
        ++*remaining;
        const int kind = item->data(0, KindRole).toInt();
        loadChildren(item, kind, item->data(0, IdRole).toString(), [finish] { finish(); }, false);
    }

    return false; // drag is deferred until the fetches above finish
}

void SubsonicBrowser::cancelDragFetch()
{
    ++m_dragFetchToken;
    m_dragFetching = false;
}

bool SubsonicBrowser::ensureGridDragData(const QList<QListWidgetItem*>& items)
{
    m_client->setSettings(loadSubsonicSettings(m_settings));

    QStringList albumIds;
    for(QListWidgetItem* item : items) {
        if(!item || item->data(GridKindRole).toInt() != GridAlbum) {
            continue;
        }
        const QJsonObject album = item->data(GridJsonRole).toJsonObject();
        const QString id        = album.value(u"id"_s).toString();
        if(!id.isEmpty() && !m_songsByAlbum.contains(id) && !albumIds.contains(id)) {
            albumIds.push_back(id);
        }
    }

    if(albumIds.isEmpty()) {
        return true;
    }
    if(m_gridDragFetching) {
        return false;
    }

    m_gridDragFetching = true;
    const int token    = ++m_gridDragFetchToken;
    auto remaining     = std::make_shared<int>(albumIds.size());
    for(const QString& id : albumIds) {
        m_client->getAlbum(id, [this, id, token, remaining](const QJsonObject& object, const QString& error) {
            if(token != m_gridDragFetchToken) {
                return;
            }
            m_songsByAlbum.insert(id, error.isEmpty() ? asArray(object.value(u"song"_s)) : QJsonArray{});
            if(!error.isEmpty()) {
                setStatus(error);
            }
            if(--*remaining == 0) {
                m_gridDragFetching = false;
                if(auto* grid = static_cast<SubsonicGridWidget*>(m_grid)) {
                    grid->resumeDrag();
                }
            }
        });
    }
    return false;
}

void SubsonicBrowser::cancelGridDragFetch()
{
    ++m_gridDragFetchToken;
    m_gridDragFetching = false;
}

TrackList SubsonicBrowser::tracksForGridDrag(const QList<QListWidgetItem*>& items)
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    if(!settings.isValid()) {
        return {};
    }
    m_client->setSettings(settings);

    QList<QJsonObject> songs;
    QSet<QString> seenIds;
    for(QListWidgetItem* item : items) {
        if(!item || item->data(GridKindRole).toInt() != GridAlbum) {
            continue;
        }
        const QJsonObject album = item->data(GridJsonRole).toJsonObject();
        const QString albumId   = album.value(u"id"_s).toString();
        for(const QJsonValue& value : m_songsByAlbum.value(albumId)) {
            const QJsonObject song = value.toObject();
            const QString songId   = song.value(u"id"_s).toString();
            if(!songId.isEmpty() && !seenIds.contains(songId)) {
                seenIds.insert(songId);
                songs.push_back(song);
            }
        }
    }

    if(songs.empty()) {
        return {};
    }

    TrackList tracks;
    tracks.reserve(songs.size());
    for(const QJsonObject& song : songs) {
        const QString songId = song.value(u"id"_s).toString();
        const QString local  = m_libraryDownloads ? m_libraryDownloads->existingLocalPath(song) : QString{};
        tracks.push_back(trackForSong(song, local.isEmpty() ? m_client->streamUrl(songId).toString() : local));
    }
    qCInfo(SUBSONIC) << "Dragging" << tracks.size() << "track(s) from grid";
    return tracks;
}

TrackList SubsonicBrowser::tracksForDrag(const QList<QTreeWidgetItem*>& items)
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    if(!settings.isValid()) {
        return {};
    }
    m_client->setSettings(settings);

    QList<QJsonObject> songs;
    QSet<QString> seenIds;
    const auto appendSong = [&](const QJsonObject& song) {
        const QString id = song.value(u"id"_s).toString();
        if(id.isEmpty() || !seenIds.contains(id)) {
            seenIds.insert(id);
            songs.push_back(song);
        }
    };
    const auto appendSongs = [&](const QJsonArray& array) {
        for(const QJsonValue& value : array) {
            appendSong(value.toObject());
        }
    };

    // Read from the load caches rather than the tree children: on a first drag the children may still
    // be placeholders (the fetch is async), which is why dragging an unexpanded item used to fail.
    std::function<void(const QList<QTreeWidgetItem*>&)> collect = [&](const QList<QTreeWidgetItem*>& list) {
        for(QTreeWidgetItem* item : list) {
            if(!item) {
                continue;
            }
            const QString id = item->data(0, IdRole).toString();
            const int kind   = item->data(0, KindRole).toInt();
            if(kind == Song) {
                appendSong(item->data(0, JsonRole).toJsonObject());
            } else if(kind == Album) {
                if(const auto cached = m_songsByAlbum.constFind(id); cached != m_songsByAlbum.constEnd()) {
                    appendSongs(cached.value());
                } else {
                    ensureChildrenLoaded(item, false);
                }
            } else if(kind == PlaylistNode) {
                if(const auto cached = m_playlistEntries.constFind(id); cached != m_playlistEntries.constEnd()) {
                    appendSongs(cached.value());
                } else {
                    ensureChildrenLoaded(item, false);
                }
            } else if(kind == Artist) {
                for(const QJsonValue& album : m_albumsByArtist.value(id)) {
                    appendSongs(m_songsByAlbum.value(album.toObject().value(u"id"_s).toString()));
                }
                if(!m_albumsByArtist.contains(id)) {
                    ensureChildrenLoaded(item, false);
                }
            } else if(kind == HomeShelf || kind == Genre) {
                if(item->data(0, LoadedRole).toBool()) {
                    QList<QTreeWidgetItem*> children;
                    for(int i{0}; i < item->childCount(); ++i) {
                        children.push_back(item->child(i));
                    }
                    collect(children);
                } else {
                    ensureChildrenLoaded(item, false);
                }
            } else if(kind == ResultHeader) {
                QList<QTreeWidgetItem*> children;
                for(int i{0}; i < item->childCount(); ++i) {
                    children.push_back(item->child(i));
                }
                collect(children);
            }
        }
    };
    collect(items);

    if(songs.empty()) {
        return {};
    }

    TrackList tracks;
    tracks.reserve(songs.size());
    for(const QJsonObject& song : songs) {
        const QString songId = song.value(u"id"_s).toString();
        const QString local  = m_libraryDownloads ? m_libraryDownloads->existingLocalPath(song) : QString{};
        tracks.push_back(trackForSong(song, local.isEmpty() ? m_client->streamUrl(songId).toString() : local));
    }
    qCInfo(SUBSONIC) << "Dragging" << tracks.size() << "track(s)";
    return tracks;
}

void SubsonicBrowser::loadChildren(QTreeWidgetItem* item, int kind, const QString& id, std::function<void()> onDone,
                                   bool expand)
{
    if(onDone) {
        m_pendingLoads[item].push_back(std::move(onDone));
    }

    // A fetch for this item is already in flight: the callback registered above will run when it finishes.
    if(m_loading.contains(item)) {
        return;
    }
    m_loading.insert(item);

    const int treeGeneration = m_treeGeneration;
    auto callback = [this, item, kind, treeGeneration, expand](const QJsonObject& object, const QString& error) {
        m_loading.erase(item);

        if(treeGeneration == m_treeGeneration) {
            if(error.isEmpty()) {
                populateChildren(item, kind, object, expand);
            } else {
                while(item->childCount() > 0) {
                    QTreeWidgetItem* child = item->takeChild(0);
                    unregisterItem(child);
                    delete child;
                }
                item->setData(0, LoadedRole, false);
                setStatus(error);
            }
        }

        if(const auto pending = m_pendingLoads.find(item); pending != m_pendingLoads.end()) {
            auto callbacks = std::move(pending->second);
            m_pendingLoads.erase(pending);
            for(const auto& callbackFn : callbacks) {
                if(callbackFn) {
                    callbackFn();
                }
            }
        }
    };

    if(kind == Artist) {
        if(const auto cached = m_albumsByArtist.constFind(id); cached != m_albumsByArtist.constEnd()) {
            QJsonObject object;
            object[u"album"_s] = cached.value();
            callback(object, {});
            return;
        }
        m_client->getArtist(id, callback);
    }
    else if(kind == PlaylistNode) {
        if(const auto cached = m_playlistEntries.constFind(id); cached != m_playlistEntries.constEnd()) {
            QJsonObject object;
            object[u"entry"_s] = cached.value();
            callback(object, {});
            return;
        }
        m_client->getPlaylist(id, callback);
    }
    else if(kind == HomeShelf) {
        if(id == u"genres"_s) {
            m_client->getGenres([callback](const QJsonArray& genres, const QString& error) {
                QJsonObject object;
                object[u"genre"_s] = genres;
                callback(object, error);
            });
        }
        else if(id == u"random"_s) {
            m_client->getRandomSongs([callback](const QJsonArray& songs, const QString& error) {
                QJsonObject object;
                object[u"song"_s] = songs;
                callback(object, error);
            }, 50);
        }
        else if(id == u"starred"_s) {
            m_client->getStarred2(callback);
        }
        else {
            m_client->getAlbumList2(id, [callback](const QJsonArray& albums, const QString& error) {
                QJsonObject object;
                object[u"album"_s] = albums;
                callback(object, error);
            }, 25);
        }
    }
    else if(kind == Genre) {
        m_client->getSongsByGenre(id, [callback](const QJsonArray& songs, const QString& error) {
            QJsonObject object;
            object[u"song"_s] = songs;
            callback(object, error);
        }, 100);
    }
    else {
        if(const auto cached = m_songsByAlbum.constFind(id); cached != m_songsByAlbum.constEnd()) {
            QJsonObject object;
            object[u"song"_s] = cached.value();
            callback(object, {});
            return;
        }
        m_client->getAlbum(id, callback);
    }
}

void SubsonicBrowser::populateChildren(QTreeWidgetItem* item, int kind, const QJsonObject& object, bool expand)
{
    while(item->childCount() > 0) {
        QTreeWidgetItem* child = item->takeChild(0);
        unregisterItem(child);
        delete child;
    }

    item->setData(0, LoadedRole, true);

    if(kind == HomeShelf) {
        const QString shelf = item->data(0, IdRole).toString();
        if(shelf == u"genres"_s) {
            const QJsonArray genres = asArray(object.value(u"genre"_s));
            for(const QJsonValue& value : genres) {
                const QJsonObject genre = value.toObject();
                auto* genreItem         = new QTreeWidgetItem{item};
                genreItem->setText(0, genre.value(u"value"_s).toString());
                genreItem->setText(2, tr("%1 songs").arg(genre.value(u"songCount"_s).toInt()));
                genreItem->setData(0, KindRole, Genre);
                genreItem->setData(0, IdRole, genre.value(u"value"_s).toString());
                genreItem->setData(0, JsonRole, genre);
                addPlaceholder(genreItem);
            }
            if(genres.isEmpty()) {
                addPlaceholder(item, tr("No genres"));
            }
        }
        else if(shelf == u"starred"_s) {
            const QJsonArray artists = asArray(object.value(u"artist"_s));
            const QJsonArray albums  = asArray(object.value(u"album"_s));
            const QJsonArray songs   = asArray(object.value(u"song"_s));

            if(!artists.isEmpty()) {
                auto* header = createResultHeader(tr("Artists"), item);
                for(const QJsonValue& value : artists) {
                    addPlaceholder(createArtistItem(value.toObject(), header));
                }
            }
            if(!albums.isEmpty()) {
                auto* header = createResultHeader(tr("Albums"), item);
                for(const QJsonValue& value : albums) {
                    addPlaceholder(createAlbumItem(header, value.toObject()));
                }
            }
            if(!songs.isEmpty()) {
                auto* header = createResultHeader(tr("Songs"), item);
                for(const QJsonValue& value : songs) {
                    createSongItem(header, value.toObject());
                }
            }
            if(artists.isEmpty() && albums.isEmpty() && songs.isEmpty()) {
                addPlaceholder(item, tr("Nothing starred yet"));
            }
        }
        else if(shelf == u"random"_s) {
            const QJsonArray songs = asArray(object.value(u"song"_s));
            for(const QJsonValue& value : songs) {
                createSongItem(item, value.toObject());
            }
            if(songs.isEmpty()) {
                addPlaceholder(item, tr("No songs"));
            }
        }
        else {
            const QJsonArray albums = asArray(object.value(u"album"_s));
            for(const QJsonObject& album : sortedAlbums(albums, AlbumSort::ServerOrder)) {
                addPlaceholder(createAlbumItem(item, album));
            }
            if(albums.isEmpty()) {
                addPlaceholder(item, tr("No albums"));
            }
        }
    }
    else if(kind == Genre) {
        const QJsonArray songs = asArray(object.value(u"song"_s));
        for(const QJsonValue& value : songs) {
            createSongItem(item, value.toObject());
        }
        if(songs.isEmpty()) {
            addPlaceholder(item, tr("No songs"));
        }
    }
    else if(kind == Artist) {
        const QJsonArray albums = asArray(object.value(u"album"_s));
        m_albumsByArtist.insert(item->data(0, IdRole).toString(), albums);
        for(const QJsonObject& album : sortedAlbums(albums, m_albumSort)) {
            addPlaceholder(createAlbumItem(item, album));
        }
    }
    else if(kind == Album) {
        const QJsonArray songs = asArray(object.value(u"song"_s));
        m_songsByAlbum.insert(item->data(0, IdRole).toString(), songs);
        for(const QJsonValue& value : songs) {
            createSongItem(item, value.toObject());
        }
    }
    else if(kind == PlaylistNode) {
        const QJsonArray entries = asArray(object.value(u"entry"_s));
        m_playlistEntries.insert(item->data(0, IdRole).toString(), entries);
        for(const QJsonValue& value : entries) {
            createSongItem(item, value.toObject());
        }
        // Gonic (and possibly others) silently drop playlist items whose paths no longer resolve,
        // returning an empty entry list. Surface that instead of expanding to nothing.
        if(entries.isEmpty()) {
            addPlaceholder(item, tr("No tracks (server returned none)"));
        }
    }

    if(expand) {
        item->setExpanded(true);
    }

    restoreExpansion(item);
}

QTreeWidgetItem* SubsonicBrowser::createArtistItem(const QJsonObject& artist, QTreeWidgetItem* parent)
{
    auto* item = parent ? new QTreeWidgetItem{parent} : new QTreeWidgetItem{m_tree};
    item->setText(NameColumn, artist.value(u"name"_s).toString());
    item->setText(RatingColumn, ratingText(userRating(artist)));
    item->setText(StarredColumn, starredText(artist));
    item->setData(0, KindRole, Artist);
    item->setData(0, IdRole, artist.value(u"id"_s).toString());
    item->setData(0, JsonRole, artist);
    item->setIcon(NameColumn, placeholderCoverIcon());
    const QString coverId = coverIdFor(artist);
    item->setData(0, CoverRole, coverId);
    applyCover(item, coverId);
    return item;
}

QTreeWidgetItem* SubsonicBrowser::createAlbumItem(QTreeWidgetItem* parent, const QJsonObject& album)
{
    auto* child = new QTreeWidgetItem{parent};
    child->setText(NameColumn, album.value(u"name"_s).toString());
    child->setText(ArtistColumn, album.value(u"artist"_s).toString());
    child->setText(AlbumColumn, QString::number(album.value(u"year"_s).toInt()));
    child->setText(RatingColumn, ratingText(userRating(album)));
    child->setText(StarredColumn, starredText(album));
    child->setData(0, KindRole, Album);
    child->setData(0, IdRole, album.value(u"id"_s).toString());
    child->setData(0, JsonRole, album);
    child->setIcon(NameColumn, placeholderCoverIcon());
    const QString coverId = coverIdFor(album);
    child->setData(0, CoverRole, coverId);
    applyCover(child, coverId);
    return child;
}

QTreeWidgetItem* SubsonicBrowser::createSongItem(QTreeWidgetItem* parent, const QJsonObject& song)
{
    auto* child = new QTreeWidgetItem{parent};

    const int trackNumber = song.value(u"track"_s).toInt();
    const QString title   = song.value(u"title"_s).toString();
    child->setText(NameColumn,
                   trackNumber > 0 ? u"%1. %2"_s.arg(trackNumber, 2, 10, QLatin1Char{'0'}).arg(title) : title);
    child->setText(ArtistColumn, song.value(u"artist"_s).toString());
    child->setText(AlbumColumn, song.value(u"album"_s).toString());

    const int seconds = song.value(u"duration"_s).toInt();
    if(seconds > 0) {
        child->setText(LengthColumn,
                       u"%1:%2"_s.arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char{'0'}));
    }

    const qint64 size = songSize(song);
    if(size > 0) {
        child->setText(SizeColumn, QLocale{}.formattedDataSize(size));
    }

    child->setText(RatingColumn, ratingText(userRating(song)));
    child->setText(StarredColumn, starredText(song));

    child->setData(0, KindRole, Song);
    child->setData(0, IdRole, song.value(u"id"_s).toString());
    child->setData(0, JsonRole, song);
    child->setIcon(NameColumn, placeholderCoverIcon());
    const QString coverId = coverIdFor(song);
    child->setData(0, CoverRole, coverId);
    applyCover(child, coverId);

    child->setText(DownloadedColumn, downloadedText(song));
    if(const QString id = song.value(u"id"_s).toString(); !id.isEmpty()) {
        m_songItemsById[id].append(child);
    }
    return child;
}

void SubsonicBrowser::addPlaceholder(QTreeWidgetItem* item, const QString& text)
{
    auto* placeholder = new QTreeWidgetItem{item};
    placeholder->setText(0, text.isEmpty() ? tr("Loading...") : text);
    placeholder->setData(0, KindRole, Placeholder);
    placeholder->setDisabled(true);
}

void SubsonicBrowser::gatherTracks(const QList<QTreeWidgetItem*>& items, const std::shared_ptr<GatherState>& state)
{
    for(QTreeWidgetItem* item : items) {
        if(!item) {
            continue;
        }

        const int kind = item->data(0, KindRole).toInt();
        if(kind == Song) {
            const QJsonObject song = item->data(0, JsonRole).toJsonObject();
            state->bytes += songSize(song);
            state->songs.push_back(song);
        } else if(kind == ResultHeader) {
            QList<QTreeWidgetItem*> children;
            for(int i{0}; i < item->childCount(); ++i) {
                children.push_back(item->child(i));
            }
            gatherTracks(children, state);
        } else if(kind == Artist || kind == Album || kind == PlaylistNode || kind == HomeShelf || kind == Genre) {
            if(item->data(0, LoadedRole).toBool()) {
                QList<QTreeWidgetItem*> children;
                for(int i{0}; i < item->childCount(); ++i) {
                    children.push_back(item->child(i));
                }
                gatherTracks(children, state);
            } else {
                ++state->pending;
                const int generation = m_generation;
                loadChildren(item, kind, item->data(0, IdRole).toString(), [this, item, state, generation] {
                    if(generation == m_generation) {
                        QList<QTreeWidgetItem*> children;
                        for(int i{0}; i < item->childCount(); ++i) {
                            children.push_back(item->child(i));
                        }
                        gatherTracks(children, state);
                    }
                    --state->pending;
                    finishGather(state);
                });
            }
        }
    }

    finishGather(state);
}

void SubsonicBrowser::finishGather(const std::shared_ptr<GatherState>& state)
{
    if(state->finished || state->pending > 0) {
        return;
    }
    state->finished = true;

    if(state->songs.empty()) {
        setStatus(tr("No tracks to add."));
        return;
    }

    if(state->action == BrowserAction::Download) {
        downloadToLibrary(state);
        return;
    }

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);

    if(state->action == BrowserAction::AddToServerPlaylist) {
        QStringList ids;
        ids.reserve(state->songs.size());
        for(const QJsonObject& song : state->songs) {
            ids.push_back(song.value(u"id"_s).toString());
        }
        m_client->addToPlaylist(state->playlistId, ids,
                                [this, count = ids.size()](bool ok, const QString& error) {
                                    setStatus(ok ? tr("Added %1 track(s) to the server playlist.").arg(count) : error);
                                    if(ok) {
                                        refresh();
                                    }
                                });
        return;
    }

    if(state->action == BrowserAction::CreateServerPlaylist) {
        QStringList ids;
        ids.reserve(state->songs.size());
        for(const QJsonObject& song : state->songs) {
            ids.push_back(song.value(u"id"_s).toString());
        }
        m_client->createPlaylist(state->playlistName, ids, [this](bool ok, const QString& error) {
            setStatus(ok ? tr("Playlist created.") : error);
            if(ok) {
                refresh();
            }
        });
        return;
    }

    qCInfo(SUBSONIC) << "Adding" << state->songs.size() << "track(s), total"
                     << QLocale{}.formattedDataSize(state->bytes);

    TrackList tracks;
    tracks.reserve(state->songs.size());
    for(const QJsonObject& song : state->songs) {
        const QString songId = song.value(u"id"_s).toString();
        const QString local  = m_libraryDownloads ? m_libraryDownloads->existingLocalPath(song) : QString{};
        tracks.push_back(trackForSong(song, local.isEmpty() ? m_client->streamUrl(songId).toString() : local));
    }
    applyAction(tracks, state->action, true);
}

void SubsonicBrowser::downloadToLibrary(const std::shared_ptr<GatherState>& state)
{
    if(!m_libraryDownloads) {
        setStatus(tr("Library downloads are unavailable."));
        return;
    }

    qCInfo(SUBSONIC) << "Queuing" << state->songs.size() << "track(s) for library download";
    m_libraryDownloads->enqueue(state->songs);
    setStatus(tr("Queued %1 track(s) for download.").arg(state->songs.size()));
}

void SubsonicBrowser::applyAction(const TrackList& tracks, BrowserAction action, bool startPlayback)
{
    if(tracks.empty()) {
        setStatus(tr("No tracks to add."));
        return;
    }

    qCInfo(SUBSONIC) << "Action" << static_cast<int>(action) << tracks.size() << "track(s), first="
                     << tracks.front().filepath() << "remote=" << tracks.front().isRemote();

    switch(action) {
        case BrowserAction::Play:
            m_interactor->tracksToCurrentPlaylistReplace(tracks, startPlayback);
            setStatus(tr("Playing %1 track(s).").arg(tracks.size()));
            break;
        case BrowserAction::AddToPlaylist:
            m_interactor->tracksToCurrentPlaylist(tracks);
            setStatus(tr("Added %1 track(s).").arg(tracks.size()));
            break;
        case BrowserAction::PlayNext:
            if(auto* player = m_interactor->playerController()) {
                player->queueTracksNext(tracks);
            }
            setStatus(tr("Queued %1 track(s) to play next.").arg(tracks.size()));
            break;
        case BrowserAction::AddToQueue:
            if(auto* player = m_interactor->playerController()) {
                player->queueTracks(tracks);
            }
            setStatus(tr("Queued %1 track(s).").arg(tracks.size()));
            break;
        default:
            break; // Download is handled in finishGather() before tracks are built
    }
}

void SubsonicBrowser::showContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if(!item) {
        showDiscoveryMenu(pos);
        return;
    }

    if(!item->isSelected()) {
        m_tree->clearSelection();
        item->setSelected(true);
    }

    const int kind = item->data(0, KindRole).toInt();
    const auto isStarrable = [](int k) { return k == Song || k == Album || k == Artist; };
    const auto isGatherable = [](int k) {
        return k == Song || k == Album || k == Artist || k == HomeShelf || k == Genre || k == ResultHeader;
    };

    QMenu menu{this};
    QAction* playAction  = menu.addAction(tr("Play"));
    QAction* addAction   = menu.addAction(tr("Add to Playlist"));
    menu.addSeparator();
    QAction* nextAction  = menu.addAction(tr("Play Next"));
    QAction* queueAction = menu.addAction(tr("Add to Queue"));
    menu.addSeparator();
    QAction* downloadAction = menu.addAction(tr("Download to Library"));

    QAction* starAction{nullptr};
    QAction* radioAction{nullptr};
    QAction* removeFromPlaylistAction{nullptr};
    QHash<QAction*, int> ratingActions;
    if(kind == Song || kind == Artist) {
        menu.addSeparator();
        radioAction = menu.addAction(tr("Start Radio"));
    }
    if(isStarrable(kind)) {
        bool allStarred{true};
        int starrableCount{0};
        for(QTreeWidgetItem* selected : m_tree->selectedItems()) {
            if(!isStarrable(selected->data(0, KindRole).toInt())) {
                continue;
            }
            ++starrableCount;
            allStarred = allStarred && isStarred(selected->data(0, JsonRole).toJsonObject());
        }
        menu.addSeparator();
        starAction = menu.addAction(allStarred && starrableCount > 0 ? tr("Unstar") : tr("Star"));

        QMenu* ratingMenu = menu.addMenu(tr("Rating"));
        ratingActions.insert(ratingMenu->addAction(tr("No rating")), 0);
        for(int rating{1}; rating <= 5; ++rating) {
            ratingActions.insert(ratingMenu->addAction(tr("%n star(s)", "", rating)), rating);
        }
    }

    // Only a song shown directly under a server playlist can be removed from it.
    if(kind == Song) {
        if(auto* parent = item->parent(); parent && parent->data(0, KindRole).toInt() == PlaylistNode) {
            removeFromPlaylistAction = menu.addAction(tr("Remove from Server Playlist"));
        }
    }

    QHash<QAction*, QString> playlistActions;
    QAction* newServerPlaylistAction{nullptr};
    if(isGatherable(kind)) {
        menu.addSeparator();
        QMenu* addToServer = menu.addMenu(tr("Add to Server Playlist"));
        for(const QJsonObject& playlist : m_playlists) {
            playlistActions.insert(addToServer->addAction(playlist.value(u"name"_s).toString()),
                                   playlist.value(u"id"_s).toString());
        }
        addToServer->addSeparator();
        newServerPlaylistAction = addToServer->addAction(tr("New Playlist..."));
    }

    QAction* renamePlaylistAction{nullptr};
    QAction* deletePlaylistAction{nullptr};
    QAction* clearPlaylistAction{nullptr};
    if(kind == PlaylistNode) {
        menu.addSeparator();
        renamePlaylistAction = menu.addAction(tr("Rename Playlist..."));
        clearPlaylistAction  = menu.addAction(tr("Clear Playlist..."));
        deletePlaylistAction = menu.addAction(tr("Delete Playlist..."));
    }

    QAction* cancelAction{nullptr};
    if(m_libraryDownloads && m_libraryDownloads->isActive()) {
        cancelAction = menu.addAction(tr("Cancel Download"));
    }

    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if(!chosen) {
        return;
    }

    if(chosen == playAction) {
        playSelection(BrowserAction::Play);
    }
    else if(chosen == addAction) {
        playSelection(BrowserAction::AddToPlaylist);
    }
    else if(chosen == nextAction) {
        playSelection(BrowserAction::PlayNext);
    }
    else if(chosen == queueAction) {
        playSelection(BrowserAction::AddToQueue);
    }
    else if(starAction && chosen == starAction) {
        bool allStarred{true};
        for(QTreeWidgetItem* selected : m_tree->selectedItems()) {
            if(isStarrable(selected->data(0, KindRole).toInt())) {
                allStarred = allStarred && isStarred(selected->data(0, JsonRole).toJsonObject());
            }
        }
        starSelection(!allStarred);
    }
    else if(ratingActions.contains(chosen)) {
        rateSelection(ratingActions.value(chosen));
    }
    else if(radioAction && chosen == radioAction) {
        startRadio(item);
    }
    else if(playlistActions.contains(chosen)) {
        addSongsToServerPlaylistFromSelection(playlistActions.value(chosen));
    }
    else if(newServerPlaylistAction && chosen == newServerPlaylistAction) {
        createServerPlaylistFromSelection();
    }
    else if(renamePlaylistAction && chosen == renamePlaylistAction) {
        renameSelectedPlaylist(item);
    }
    else if(clearPlaylistAction && chosen == clearPlaylistAction) {
        clearServerPlaylist(item);
    }
    else if(deletePlaylistAction && chosen == deletePlaylistAction) {
        deleteSelectedPlaylist(item);
    }
    else if(removeFromPlaylistAction && chosen == removeFromPlaylistAction) {
        removeSongsFromPlaylist(item);
    }
    else if(chosen == downloadAction) {
        playSelection(BrowserAction::Download);
    }
    else if(cancelAction && chosen == cancelAction) {
        m_libraryDownloads->cancel();
        setStatus(tr("Download cancelled."));
    }
}

void SubsonicBrowser::showDiscoveryMenu(const QPoint& pos)
{
    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    if(!settings.isValid()) {
        setStatus(tr("Configure the server first."));
        return;
    }
    m_client->setSettings(settings);

    QMenu menu{this};
    QAction* randomAction  = menu.addAction(tr("Random songs"));
    QAction* starredAction = menu.addAction(tr("Starred songs"));

    QAction* chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if(!chosen) {
        return;
    }

    const auto show = [this](const QString& title, const QJsonArray& items, const QString& error) {
        if(!error.isEmpty()) {
            setStatus(error);
            return;
        }
        showDiscoverySongs(title, items);
    };

    if(chosen == randomAction) {
        setStatus(tr("Loading random songs..."));
        m_client->getRandomSongs([show](const QJsonArray& items, const QString& error) {
            show(tr("Random"), items, error);
        });
    }
    else if(chosen == starredAction) {
        setStatus(tr("Loading starred songs..."));
        m_client->getStarred([show](const QJsonArray& items, const QString& error) {
            show(tr("Starred"), items, error);
        });
    }
}

void SubsonicBrowser::showServerAlbumList(const QString& type, int offset)
{
    setStatus(offset > 0 ? tr("Loading more albums...") : tr("Loading %1...").arg(m_albumListTitle.toLower()));
    m_albumListLoading = true;

    const int generation = m_generation;
    m_client->getAlbumList2(
        type,
        [this, generation, type, offset](const QJsonArray& items, const QString& error) {
            m_albumListLoading = false;
            // Ignore a late reply if the user switched back to a per-artist sort meanwhile.
            if(generation != m_generation || serverAlbumType(m_albumSort) != type) {
                return;
            }
            if(!error.isEmpty()) {
                setStatus(error);
                return;
            }

            if(offset == 0) {
                clearTree();
                addHomeHeader();
                addPlaylistsHeader(); // playlists stay pinned at the top
                m_albumListHeader = createResultHeader(QString{});
                m_tree->addTopLevelItem(m_albumListHeader);
                m_albumListHeader->setExpanded(true);
            }
            else if(!m_albumListHeader) {
                return; // the listing was cleared while this page was in flight
            }

            // Keep the viewport where the user left it when appending another page.
            const int scroll = m_tree->verticalScrollBar()->value();

            // Drop the old "Load more" row; a fresh one is appended below if there is more.
            delete m_albumListMore;
            m_albumListMore = nullptr;

            for(const QJsonObject& album : sortedAlbums(items, AlbumSort::ServerOrder)) {
                addPlaceholder(createAlbumItem(m_albumListHeader, album));
            }
            m_albumListLoaded += static_cast<int>(items.size());

            if(items.size() >= AlbumPageSize) {
                m_albumListMore = new QTreeWidgetItem{m_albumListHeader};
                m_albumListMore->setText(0, tr("Load more..."));
                m_albumListMore->setData(0, KindRole, LoadMore);
            }

            m_albumListHeader->setText(0, tr("%1 (%2%3)")
                                              .arg(m_albumListTitle)
                                              .arg(m_albumListLoaded)
                                              .arg(m_albumListMore ? u"+"_s : u""_s));
            if(offset > 0) {
                // Only the rows below the viewport changed, so restore the scroll offset instead of
                // jumping to the new "Load more" row.
                m_tree->verticalScrollBar()->setValue(scroll);
            }
            else {
                m_tree->scrollToItem(m_albumListHeader);
            }
            setStatus(m_albumListLoaded == 0
                          ? tr("%1: no albums (the server may not support this list).").arg(m_albumListTitle)
                          : tr("%1: %2 album(s)").arg(m_albumListTitle).arg(m_albumListLoaded));
        },
        AlbumPageSize, offset);
}

void SubsonicBrowser::showDiscoverySongs(const QString& title, const QJsonArray& songs)
{
    if(songs.isEmpty()) {
        setStatus(tr("%1: no songs.").arg(title));
        return;
    }

    if(m_discoveryHeader) {
        unregisterItem(m_discoveryHeader);
        delete m_discoveryHeader;
        m_discoveryHeader = nullptr;
    }

    m_discoveryHeader = createResultHeader(tr("%1 (%2)").arg(title).arg(songs.size()));
    m_tree->insertTopLevelItem(0, m_discoveryHeader);
    for(const QJsonValue& song : songs) {
        createSongItem(m_discoveryHeader, song.toObject());
    }
    m_discoveryHeader->setExpanded(true);
    m_tree->scrollToItem(m_discoveryHeader);
    setStatus(tr("%1: %2 song(s)").arg(title).arg(songs.size()));
}

void SubsonicBrowser::createServerPlaylistFromSelection()
{
    bool accepted{false};
    const QString name = QInputDialog::getText(this, tr("New playlist"), tr("Playlist name:"), QLineEdit::Normal,
                                               {}, &accepted);
    if(!accepted || name.trimmed().isEmpty()) {
        return;
    }

    auto state        = std::make_shared<GatherState>();
    state->action     = BrowserAction::CreateServerPlaylist;
    state->playlistName = name.trimmed();
    gatherTracks(m_tree->selectedItems(), state);
}

void SubsonicBrowser::addSongsToServerPlaylistFromSelection(const QString& id)
{
    if(id.isEmpty()) {
        return;
    }

    auto state        = std::make_shared<GatherState>();
    state->action     = BrowserAction::AddToServerPlaylist;
    state->playlistId = id;
    gatherTracks(m_tree->selectedItems(), state);
}

void SubsonicBrowser::clearServerPlaylist(QTreeWidgetItem* item)
{
    if(!item) {
        return;
    }
    const QString id = item->data(0, IdRole).toString();
    if(id.isEmpty()) {
        return;
    }

    const auto answer = QMessageBox::question(this, tr("Foonic"),
                                              tr("Remove all songs from playlist \"%1\"?").arg(item->text(0)),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if(answer != QMessageBox::Yes) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    const auto clearEntries = [this, id](const QJsonArray& entries) {
        if(entries.isEmpty()) {
            setStatus(tr("Playlist is already empty."));
            return;
        }
        QList<int> indexes;
        indexes.reserve(entries.size());
        for(int i{0}; i < entries.size(); ++i) {
            indexes.push_back(i);
        }
        m_client->removeFromPlaylist(id, indexes, [this](bool ok, const QString& error) {
            setStatus(ok ? tr("Playlist cleared.") : error);
            if(ok) {
                refresh();
            }
        });
    };

    if(const auto cached = m_playlistEntries.constFind(id); cached != m_playlistEntries.constEnd()) {
        clearEntries(cached.value());
        return;
    }
    m_client->getPlaylist(id, [this, clearEntries](const QJsonObject& object, const QString& error) {
        if(!error.isEmpty()) {
            setStatus(error);
            return;
        }
        clearEntries(asArray(object.value(u"entry"_s)));
    });
}

void SubsonicBrowser::renameSelectedPlaylist(QTreeWidgetItem* item)
{
    const QString id = item->data(0, IdRole).toString();
    bool accepted{false};
    const QString name = QInputDialog::getText(this, tr("Rename playlist"), tr("Playlist name:"), QLineEdit::Normal,
                                               item->text(0), &accepted);
    if(!accepted || name.trimmed().isEmpty()) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->renamePlaylist(id, name.trimmed(), [this](bool ok, const QString& error) {
        setStatus(ok ? tr("Playlist renamed.") : error);
        if(ok) {
            refresh();
        }
    });
}

void SubsonicBrowser::deleteSelectedPlaylist(QTreeWidgetItem* item)
{
    const QString id = item->data(0, IdRole).toString();
    if(id.isEmpty()) {
        return;
    }
    const auto answer = QMessageBox::question(this, tr("Foonic"),
                                              tr("Delete playlist \"%1\" from the server?").arg(item->text(0)),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if(answer != QMessageBox::Yes) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->deletePlaylist(id, [this](bool ok, const QString& error) {
        setStatus(ok ? tr("Playlist deleted.") : error);
        if(ok) {
            refresh();
        }
    });
}

void SubsonicBrowser::removeSongsFromPlaylist(QTreeWidgetItem* item)
{
    auto* playlist = item ? item->parent() : nullptr;
    if(!playlist || playlist->data(0, KindRole).toInt() != PlaylistNode) {
        return;
    }

    QList<int> indexes;
    for(QTreeWidgetItem* selected : m_tree->selectedItems()) {
        if(selected->parent() == playlist) {
            indexes.push_back(playlist->indexOfChild(selected));
        }
    }
    if(indexes.isEmpty()) {
        indexes.push_back(playlist->indexOfChild(item));
    }

    const auto answer = QMessageBox::question(
        this, tr("Foonic"),
        tr("Remove %n song(s) from playlist \"%1\"?", "", indexes.size()).arg(playlist->text(0)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if(answer != QMessageBox::Yes) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->removeFromPlaylist(playlist->data(0, IdRole).toString(), indexes,
                                 [this](bool ok, const QString& error) {
                                     setStatus(ok ? tr("Removed from playlist.") : error);
                                     if(ok) {
                                         refresh();
                                     }
                                 });
}

void SubsonicBrowser::showHeaderMenu(const QPoint& pos)
{
    auto* header = m_tree->header();

    QMenu menu{this};
    auto* showAll = menu.addAction(tr("Show all columns"));
    menu.addSeparator();

    for(int column{0}; column < m_tree->columnCount(); ++column) {
        auto* action = menu.addAction(m_tree->headerItem()->text(column));
        action->setCheckable(true);
        action->setChecked(!m_tree->isColumnHidden(column));
        connect(action, &QAction::toggled, this, [this, column, action](bool visible) {
            if(!visible) {
                int visibleCount{0};
                for(int i{0}; i < m_tree->columnCount(); ++i) {
                    if(!m_tree->isColumnHidden(i)) {
                        ++visibleCount;
                    }
                }
                if(visibleCount <= 1) {
                    action->setChecked(true); // never hide the last visible column
                    return;
                }
            }
            m_tree->setColumnHidden(column, !visible);
            saveHeaderState();
        });
    }

    if(menu.exec(header->viewport()->mapToGlobal(pos)) == showAll) {
        for(int column{0}; column < m_tree->columnCount(); ++column) {
            m_tree->setColumnHidden(column, false);
        }
        saveHeaderState();
    }
}

void SubsonicBrowser::saveHeaderState()
{
    if(m_settings) {
        m_settings->set(u"Subsonic/HeaderState"_s, m_tree->header()->saveState());
    }
}

void SubsonicBrowser::restoreHeaderState()
{
    const QByteArray state = m_settings->value(u"Subsonic/HeaderState"_s).toByteArray();
    if(!state.isEmpty()) {
        m_tree->header()->restoreState(state);
    }
}

void SubsonicBrowser::playSelection(BrowserAction action)
{
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    if(selected.empty()) {
        setStatus(tr("Select an artist, album or song first."));
        return;
    }

    auto state    = std::make_shared<GatherState>();
    state->action = action;
    gatherTracks(selected, state);
}

void SubsonicBrowser::startRadio(QTreeWidgetItem* item)
{
    if(!item) {
        return;
    }
    const QJsonObject object = item->data(0, JsonRole).toJsonObject();
    const int kind           = item->data(0, KindRole).toInt();
    const QString id         = object.value(u"id"_s).toString();
    const QString artist     = object.value(u"artist"_s).toString();
    if(id.isEmpty()) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    const int generation = m_generation;
    const auto playSongs = [this, generation](const QJsonArray& songs, const QString& error) {
        if(generation != m_generation) {
            return;
        }
        if(!error.isEmpty()) {
            setStatus(error);
            return;
        }
        if(songs.isEmpty()) {
            setStatus(tr("No similar songs found."));
            return;
        }

        TrackList tracks;
        tracks.reserve(songs.size());
        for(const QJsonValue& value : songs) {
            const QJsonObject song = value.toObject();
            const QString songId   = song.value(u"id"_s).toString();
            const QString local    = m_libraryDownloads ? m_libraryDownloads->existingLocalPath(song) : QString{};
            tracks.push_back(trackForSong(song, local.isEmpty() ? m_client->streamUrl(songId).toString() : local));
        }
        applyAction(tracks, BrowserAction::Play, true);
        setStatus(tr("Radio: %1 track(s).").arg(tracks.size()));
    };

    setStatus(tr("Starting radio…"));
    if(kind == Artist) {
        m_client->getSimilarSongs2(id, playSongs, 50);
    }
    else if(!artist.isEmpty()) {
        m_client->getTopSongs(artist, playSongs, 50);
    }
    else {
        m_client->getSimilarSongs2(id, playSongs, 50);
    }
}

void SubsonicBrowser::starSelection(bool star)
{
    m_client->setSettings(loadSubsonicSettings(m_settings));

    int count{0};
    for(QTreeWidgetItem* item : m_tree->selectedItems()) {
        const int kind = item->data(0, KindRole).toInt();
        if(kind != Song && kind != Album && kind != Artist) {
            continue;
        }
        const QString id = item->data(0, IdRole).toString();
        if(id.isEmpty()) {
            continue;
        }
        const QString idParam = kind == Album ? u"albumId"_s : (kind == Artist ? u"artistId"_s : u"id"_s);
        m_client->star(id, star, idParam);
        updateItemStar(item, star);
        ++count;
    }

    setStatus(star ? tr("Starred %1 item(s).").arg(count) : tr("Unstarred %1 item(s).").arg(count));
}

void SubsonicBrowser::rateSelection(int rating)
{
    m_client->setSettings(loadSubsonicSettings(m_settings));

    int count{0};
    for(QTreeWidgetItem* item : m_tree->selectedItems()) {
        const int kind = item->data(0, KindRole).toInt();
        if(kind != Song && kind != Album && kind != Artist) {
            continue;
        }
        const QString id = item->data(0, IdRole).toString();
        if(id.isEmpty()) {
            continue;
        }
        m_client->setRating(id, rating);
        updateItemRating(item, rating);
        ++count;
    }

    setStatus(rating > 0 ? tr("Rated %1 item(s).").arg(count) : tr("Cleared rating on %1 item(s).").arg(count));
}

void SubsonicBrowser::updateItemStar(QTreeWidgetItem* item, bool star)
{
    if(!item) {
        return;
    }
    QJsonObject object = item->data(0, JsonRole).toJsonObject();
    object[u"starred"_s] = star ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : QString{};
    const QString id     = object.value(u"id"_s).toString();
    updateCachedJson(id, object);
    updateTreeItemJson(id, object);
}

void SubsonicBrowser::updateItemRating(QTreeWidgetItem* item, int rating)
{
    if(!item) {
        return;
    }
    QJsonObject object = item->data(0, JsonRole).toJsonObject();
    object[u"userRating"_s] = rating;
    const QString id     = object.value(u"id"_s).toString();
    updateCachedJson(id, object);
    updateTreeItemJson(id, object);
}

void SubsonicBrowser::updateTreeItemJson(const QString& id, const QJsonObject& object)
{
    if(id.isEmpty()) {
        return;
    }

    QTreeWidgetItemIterator it{m_tree};
    while(*it) {
        QTreeWidgetItem* item = *it;
        const int kind        = item->data(0, KindRole).toInt();
        if(item->data(0, IdRole).toString() == id && (kind == Song || kind == Album || kind == Artist)) {
            item->setData(0, JsonRole, object);
            item->setText(RatingColumn, ratingText(userRating(object)));
            item->setText(StarredColumn, starredText(object));
            if(m_tree->currentItem() == item) {
                updateDetails(item);
            }
        }
        ++it;
    }
}

void SubsonicBrowser::updateCachedJson(const QString& id, const QJsonObject& object)
{
    if(id.isEmpty()) {
        return;
    }

    const auto updateArray = [&id, &object](QJsonArray& array) {
        for(int i{0}; i < array.size(); ++i) {
            if(array.at(i).toObject().value(u"id"_s).toString() == id) {
                array.replace(i, object);
            }
        }
    };

    for(QJsonObject& artist : m_artists) {
        if(artist.value(u"id"_s).toString() == id) {
            artist = object;
        }
    }
    for(auto it = m_albumsByArtist.begin(); it != m_albumsByArtist.end(); ++it) {
        updateArray(it.value());
    }
    for(auto it = m_songsByAlbum.begin(); it != m_songsByAlbum.end(); ++it) {
        updateArray(it.value());
    }
    for(auto it = m_playlistEntries.begin(); it != m_playlistEntries.end(); ++it) {
        updateArray(it.value());
    }
    for(QJsonObject& playlist : m_playlists) {
        if(playlist.value(u"id"_s).toString() == id) {
            playlist = object;
        }
    }

    for(const QString& key : {u"artist"_s, u"album"_s, u"song"_s}) {
        QJsonArray array = m_searchResult.value(key).toArray();
        updateArray(array);
        m_searchResult[key] = array;
    }
}

void SubsonicBrowser::updateDetails(QTreeWidgetItem* item)
{
    if(!m_details) {
        return;
    }
    if(!item) {
        m_details->clear();
        return;
    }

    const QJsonObject object = item->data(0, JsonRole).toJsonObject();
    switch(item->data(0, KindRole).toInt()) {
        case Song:
            m_details->showSong(object);
            break;
        case Album:
            m_details->showAlbum(object);
            break;
        case Artist:
            m_details->showArtist(object);
            break;
        case PlaylistNode:
            m_details->showPlaylist(object);
            break;
        default:
            m_details->clear();
            break;
    }
}

Track SubsonicBrowser::trackForSong(const QJsonObject& song, const QString& filepath) const
{
    const QString coverArt = coverIdFor(song);
    const int coverSize    = std::max(loadSubsonicSettings(m_settings).coverArtSize, CoverCacheSize);
    const QString coverUrl
        = coverArt.isEmpty() ? QString{} : m_client->coverArtUrl(coverArt, coverSize).toString();
    return trackFromSong(song, filepath, coverUrl);
}

QString SubsonicBrowser::downloadedText(const QJsonObject& song) const
{
    // Cached: the probe hits the filesystem, and this runs for every song row that gets built.
    const QString id = song.value(u"id"_s).toString();
    if(const auto cached = m_downloadedCache.constFind(id); cached != m_downloadedCache.constEnd()) {
        return cached.value();
    }

    const bool downloaded = m_libraryDownloads && !m_libraryDownloads->existingLocalPath(song).isEmpty();
    const QString text   = downloaded ? u"\u2713 Downloaded"_s : QString{};
    m_downloadedCache.insert(id, text);
    return text;
}

QString SubsonicBrowser::albumSortLabel(AlbumSort mode) const
{
    switch(mode) {
        case AlbumSort::Newest:
            return tr("Recently added");
        case AlbumSort::Recent:
            return tr("Recently played");
        case AlbumSort::Frequent:
            return tr("Most played");
        case AlbumSort::Highest:
            return tr("Top rated");
        case AlbumSort::Starred:
            return tr("Favourite");
        default:
            return {};
    }
}

void SubsonicBrowser::clearFilter()
{
    m_filter->clear();
    m_filterText.clear();
    m_matchMode = MatchMode::Contains;
    m_settings->set(u"Subsonic/MatchMode"_s, static_cast<int>(m_matchMode));

    // Signals are blocked: the flags are set here, and one applyFilter() covers the rest.
    for(const auto& [action, flag] : std::as_const(m_scopeActions)) {
        const QSignalBlocker blocker{action};
        action->setChecked(true);
        *flag = true;
    }
    m_settings->set(u"Subsonic/SearchArtists"_s, true);
    m_settings->set(u"Subsonic/SearchAlbums"_s, true);
    m_settings->set(u"Subsonic/SearchSongs"_s, true);
    m_settings->set(u"Subsonic/SearchPlaylists"_s, true);
    for(QAction* action : std::as_const(m_matchActions)) {
        const QSignalBlocker blocker{action};
        action->setChecked(action->data().toInt() == static_cast<int>(MatchMode::Contains));
    }

    applyFilter();
}

void SubsonicBrowser::updateDownloadedItem(const QString& songId)
{
    const auto items = m_songItemsById.value(songId);
    for(QTreeWidgetItem* item : items) {
        if(!item) {
            continue;
        }
        const QString text = downloadedText(item->data(0, JsonRole).toJsonObject());
        if(item->text(DownloadedColumn) != text) {
            item->setText(DownloadedColumn, text);
        }
    }
}

void SubsonicBrowser::applyCover(QTreeWidgetItem* item, const QString& coverArtId)
{
    if(!item || coverArtId.isEmpty()) {
        return;
    }

    m_itemsByCover[coverArtId].append(item);

    QPixmap cached;
    if(QPixmapCache::find(u"foonic-cover:"_s + coverArtId, &cached)) {
        item->setIcon(NameColumn, QIcon{cached});
        return;
    }

    requestCover(coverArtId);
}

void SubsonicBrowser::requestCover(const QString& coverArtId)
{
    if(coverArtId.isEmpty()) {
        return;
    }

    // Fast path: the grid copy is already in memory. Reapply it instead of checking the disk again.
    QPixmap cached;
    if(QPixmapCache::find(u"foonic-cover-grid:"_s + coverArtId, &cached)) {
        QPixmapCache::insert(u"foonic-cover:"_s + coverArtId,
                             cached.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        applyCoverToTree(coverArtId);
        applyCoverToGrid(coverArtId);
        return;
    }

    if(m_coverPending.contains(coverArtId)) {
        return;
    }
    m_coverPending.insert(coverArtId);
    m_coverQueue.push_back(coverArtId);
    pumpCoverRequests();
}

void SubsonicBrowser::pumpCoverRequests()
{
    // Cache checks and decodes share a two-thread pool. Keep at most four such jobs outstanding so
    // a large grid never floods the event loop or the CPU.
    if(m_coverCacheActive + m_coverDecoding >= MaxCoverRequests) {
        return;
    }

    while(m_coverCacheActive < 2 && !m_coverQueue.empty()) {
        const QString coverArtId = m_coverQueue.front();
        m_coverQueue.pop_front();
        ++m_coverCacheActive;

        const QPointer<SubsonicBrowser> self{this};
        auto* task = new CoverCacheTask{coverArtId, [self, coverArtId](CoverCacheResult result) {
            if(!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self,
                [self, coverArtId, result = std::move(result)]() mutable {
                    if(!self) {
                        return;
                    }
                    --self->m_coverCacheActive;
                    if(result.hit) {
                        self->applyDecodedCover(coverArtId, result.small, result.grid);
                        self->finishCover(coverArtId);
                    }
                    else {
                        self->startCoverDownload(coverArtId);
                    }
                    self->pumpCoverRequests();
                },
                Qt::QueuedConnection);
        }};
        m_coverPool->start(task);
    }
}

void SubsonicBrowser::startCoverDownload(const QString& coverArtId)
{
    ++m_coverActive;

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);
    QNetworkRequest request = subsonicRequest(m_client->coverArtUrl(coverArtId, GridCoverSize));
    request.setPriority(QNetworkRequest::LowPriority);
    QNetworkReply* reply = m_network->get(request);
    applySslIgnore(reply, settings);
    if(!reply) {
        --m_coverActive;
        finishCover(coverArtId);
        return;
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, coverArtId] {
        reply->deleteLater();
        --m_coverActive;

        if(reply->error() != QNetworkReply::NoError) {
            finishCover(coverArtId);
            return;
        }

        const QByteArray data = reply->readAll();
        if(data.isEmpty()) {
            finishCover(coverArtId);
            return;
        }

        ++m_coverDecoding;
        const QPointer<SubsonicBrowser> self{this};
        auto* task = new CoverDecodeTask{data, coverArtId, [self, coverArtId](CoverDecodeResult result) {
            if(!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self,
                [self, coverArtId, result = std::move(result)]() mutable {
                    if(!self) {
                        return;
                    }
                    --self->m_coverDecoding;
                    if(result.ok) {
                        self->applyDecodedCover(coverArtId, result.small, result.grid);
                    }
                    self->finishCover(coverArtId);
                    self->pumpCoverRequests();
                },
                Qt::QueuedConnection);
        }};
        m_coverPool->start(task);
    });
}

void SubsonicBrowser::applyDecodedCover(const QString& coverArtId, const QImage& small, const QImage& grid)
{
    QPixmapCache::insert(u"foonic-cover:"_s + coverArtId, QPixmap::fromImage(small));
    QPixmapCache::insert(u"foonic-cover-grid:"_s + coverArtId, QPixmap::fromImage(grid));
    applyCoverToTree(coverArtId);
    applyCoverToGrid(coverArtId);
}

void SubsonicBrowser::finishCover(const QString& coverArtId)
{
    m_coverPending.remove(coverArtId);
    pumpCoverRequests();
}

void SubsonicBrowser::applyCoverToTree(const QString& coverArtId)
{
    QPixmap cached;
    if(!QPixmapCache::find(u"foonic-cover:"_s + coverArtId, &cached)) {
        return;
    }

    const QIcon icon{cached};
    for(QTreeWidgetItem* item : std::as_const(m_itemsByCover[coverArtId])) {
        if(item) {
            item->setIcon(NameColumn, icon);
        }
    }
}

void SubsonicBrowser::applyCoverToGrid(const QString& coverArtId)
{
    QPixmap cached;
    if(!QPixmapCache::find(u"foonic-cover-grid:"_s + coverArtId, &cached)) {
        if(!QPixmapCache::find(u"foonic-cover:"_s + coverArtId, &cached)) {
            return;
        }
    }

    // Keep the 300px pixmap in the QIcon; Qt scales it to the current responsive icon size.
    const QIcon icon{cached};
    for(QListWidgetItem* item : std::as_const(m_gridItemsByCover[coverArtId])) {
        if(item) {
            item->setIcon(icon);
        }
    }
}

void SubsonicBrowser::unregisterItem(QTreeWidgetItem* item)
{
    if(!item) {
        return;
    }
    for(int i{0}; i < item->childCount(); ++i) {
        unregisterItem(item->child(i));
    }

    const QString coverArtId = item->data(0, CoverRole).toString();
    if(!coverArtId.isEmpty()) {
        auto it = m_itemsByCover.find(coverArtId);
        if(it != m_itemsByCover.end()) {
            it.value().removeAll(item);
            if(it.value().isEmpty()) {
                m_itemsByCover.erase(it);
            }
        }
    }

    if(item->data(0, KindRole).toInt() == Song) {
        const QString id = item->data(0, IdRole).toString();
        auto it          = m_songItemsById.find(id);
        if(it != m_songItemsById.end()) {
            it.value().removeAll(item);
            if(it.value().isEmpty()) {
                m_songItemsById.erase(it);
            }
        }
    }
}

void SubsonicBrowser::applyFilter()
{
    if(m_applyingFilter) {
        return;
    }

    m_applyingFilter = true;

    if(m_filterText.isEmpty()) {
        ++m_searchGeneration; // drop any in-flight search results
        ++m_playlistScanToken;
        m_playlistScanQueue.clear();
        if(m_filterMode) {
            m_filterMode = false;
            rebuildArtistTree();
        }
    }
    else {
        if(m_gridMode && m_gridButton) {
            m_gridButton->setChecked(false); // search results are a tree view
        }
        m_filterMode         = true;
        const int generation = ++m_searchGeneration;
        m_searchResultReady  = false;
        setStatus(tr("Searching..."));

        const QString query = serverQueryFor(parseFilter(m_filterText, m_matchMode));
        if(m_searchPlaylists && m_searchPlaylistTracks) {
            startPlaylistScan();
        }

        if(query.isEmpty()) {
            // Only exclusions / nothing searchable server-side: render from local data only.
            m_searchResult      = {};
            m_searchResultReady = true;
            renderSearchView();
        }
        else {
            m_client->search(query, m_searchArtists ? 30 : 0, m_searchAlbums ? 30 : 0, m_searchSongs ? 60 : 0,
                             [this, generation](const QJsonObject& result, const QString& error) {
                                 if(generation != m_searchGeneration) {
                                     return;
                                 }
                                 if(!error.isEmpty()) {
                                     setStatus(error);
                                     return;
                                 }
                                 m_searchResult      = result;
                                 m_searchResultReady = true;
                                 renderSearchView();
                             });
        }
    }

    m_applyingFilter = false;
}

void SubsonicBrowser::clearTree()
{
    // Invalidate any in-flight lazy loads so their callbacks don't touch deleted items.
    ++m_treeGeneration;
    m_pendingLoads.clear();
    m_loading.clear();
    m_albumListHeader  = nullptr;
    m_albumListMore    = nullptr;
    m_albumListLoaded  = 0;
    m_albumListLoading = false;
    m_homeHeader       = nullptr;
    m_playlistsHeader = nullptr;
    m_discoveryHeader = nullptr;
    m_itemsByCover.clear();
    m_songItemsById.clear();
    m_tree->clear();
}

void SubsonicBrowser::rebuildArtistTree()
{
    if(const QString type = serverAlbumType(m_albumSort); !type.isEmpty()) {
        showServerAlbumList(type);
        return;
    }

    clearTree();
    // Home shelves and playlists stay pinned at the top, above the (sortable) artist list.
    addHomeHeader();
    addPlaylistsHeader();

    QList<QJsonObject> artists = m_artists;
    sortArtists(artists, m_artistSort);
    for(const QJsonObject& artist : artists) {
        auto* item = createArtistItem(artist);
        addPlaceholder(item);
    }
    restoreExpansion(nullptr);
    setStatus(tr("%1 artists").arg(m_artists.size()));
}

QTreeWidgetItem* SubsonicBrowser::createResultHeader(const QString& title, QTreeWidgetItem* parent)
{
    auto* header = parent ? new QTreeWidgetItem{parent} : new QTreeWidgetItem;
    header->setText(0, title);
    header->setData(0, KindRole, ResultHeader);
    header->setFirstColumnSpanned(true);
    QFont font = header->font(0);
    font.setBold(true);
    header->setFont(0, font);
    header->setExpanded(true);
    return header;
}

QTreeWidgetItem* SubsonicBrowser::createPlaylistItem(QTreeWidgetItem* parent, const QJsonObject& playlist)
{
    auto* item = new QTreeWidgetItem{parent};
    item->setText(NameColumn, playlist.value(u"name"_s).toString());
    item->setText(ArtistColumn, playlist.value(u"owner"_s).toString());
    item->setText(AlbumColumn, tr("%1 songs").arg(playlist.value(u"songCount"_s).toInt()));
    item->setData(0, KindRole, PlaylistNode);
    item->setData(0, IdRole, playlist.value(u"id"_s).toString());
    item->setData(0, JsonRole, playlist);
    item->setIcon(NameColumn, placeholderCoverIcon());
    const QString coverId = coverIdFor(playlist);
    item->setData(0, CoverRole, coverId);
    applyCover(item, coverId);
    addPlaceholder(item);
    return item;
}

void SubsonicBrowser::addHomeHeader()
{
    if(m_homeHeader) {
        return;
    }

    m_homeHeader = createResultHeader(tr("Home"));
    m_tree->insertTopLevelItem(0, m_homeHeader);

    const auto addShelf = [this](const QString& title, const QString& key) {
        auto* item = new QTreeWidgetItem{m_homeHeader};
        item->setText(NameColumn, title);
        item->setData(0, KindRole, HomeShelf);
        item->setData(0, IdRole, key);
        addPlaceholder(item);
    };

    addShelf(tr("Recently Added"), u"newest"_s);
    addShelf(tr("Recently Played"), u"recent"_s);
    addShelf(tr("Most Played"), u"frequent"_s);
    addShelf(tr("Top Rated"), u"highest"_s);
    addShelf(tr("Favourite Albums"), u"starred"_s);
    addShelf(tr("Random Songs"), u"random"_s);
    addShelf(tr("Genres"), u"genres"_s);
    m_homeHeader->setExpanded(true);
    restoreExpansion(m_homeHeader);
}

void SubsonicBrowser::addPlaylistsHeader()
{
    // Playlists and artists load in parallel: never add a second header if the other one won.
    if(m_playlists.isEmpty() || m_playlistsHeader) {
        return;
    }

    m_playlistsHeader = createResultHeader(tr("Playlists (%1)").arg(m_playlists.size()));
    m_tree->insertTopLevelItem(m_homeHeader ? 1 : 0, m_playlistsHeader);
    QList<QJsonObject> playlists = m_playlists;
    sortPlaylists(playlists, m_playlistSort);
    for(const QJsonObject& playlist : playlists) {
        createPlaylistItem(m_playlistsHeader, playlist);
    }
    restoreExpansion(m_playlistsHeader);
}

void SubsonicBrowser::startPlaylistScan()
{
    ++m_playlistScanToken; // cancel any in-flight scan; its callbacks will be ignored
    m_playlistScanQueue.clear();
    m_playlistScanDone = 0;
    for(const QJsonObject& playlist : m_playlists) {
        const QString id = playlist.value(u"id"_s).toString();
        if(!m_playlistEntries.contains(id)) {
            m_playlistScanQueue.push_back(id);
        }
    }
    m_playlistScanTotal = m_playlistScanQueue.size();
    pumpPlaylistScan();
}

void SubsonicBrowser::pumpPlaylistScan()
{
    while(m_playlistScanActive < MaxPlaylistFetches && !m_playlistScanQueue.isEmpty()) {
        const int token  = m_playlistScanToken;
        const QString id = m_playlistScanQueue.takeFirst();
        ++m_playlistScanActive;
        m_client->getPlaylist(id, [this, token, id](const QJsonObject& object, const QString& error) {
            --m_playlistScanActive;
            if(token == m_playlistScanToken) {
                m_playlistEntries.insert(id, error.isEmpty() ? asArray(object.value(u"entry"_s)) : QJsonArray{});
                ++m_playlistScanDone;
                if(m_filterMode) {
                    setStatus(tr("Searching playlists (%1/%2)...").arg(m_playlistScanDone).arg(m_playlistScanTotal));
                }
            }
            pumpPlaylistScan();
        });
    }

    if(m_playlistScanActive == 0 && m_playlistScanQueue.isEmpty()) {
        renderSearchView(); // scan finished: show any track matches
    }
}

void SubsonicBrowser::renderSearchView()
{
    if(m_searchResultReady && m_filterMode) {
        buildSearchTree(m_searchResult);
    }
}

void SubsonicBrowser::buildSearchTree(const QJsonObject& result)
{
    clearTree();

    const QList<FilterTerm> terms = parseFilter(m_filterText, m_matchMode);

    // Plain "contains" searches keep the server's own matching; once the user picks a word mode or
    // uses field:/negation syntax we re-filter the server results ourselves.
    bool precise = m_matchMode != MatchMode::Contains;
    for(const FilterTerm& term : terms) {
        if(term.negated || !term.field.isEmpty()) {
            precise = true;
            break;
        }
    }

    const auto filterArray = [&terms, this](
                                 const QJsonArray& array,
                                 const std::function<QHash<QString, QString>(const QJsonObject&)>& fieldsFn) {
        QJsonArray out;
        for(const QJsonValue& value : array) {
            const QJsonObject object = value.toObject();
            if(matchesFilter(terms, m_matchMode, fieldsFn(object))) {
                out.append(object);
            }
        }
        return out;
    };

    // search3 does its own (server-specific) matching, so re-filter its results so the chosen match
    // mode and any field:/negation syntax are actually honoured.
    QJsonArray artists = asArray(result.value(u"artist"_s));
    QJsonArray albums  = asArray(result.value(u"album"_s));
    QJsonArray songs   = asArray(result.value(u"song"_s));
    if(precise) {
        artists = filterArray(artists, artistFields);
        albums  = filterArray(albums, albumFields);
        songs   = filterArray(songs, songFields);
    }

    // Playlists aren't part of search3, so match them locally against their name and cached tracks.
    QList<QJsonObject> playlistMatches;
    for(const QJsonObject& playlist : m_playlists) {
        const QString id = playlist.value(u"id"_s).toString();
        if(matchesFilter(terms, m_matchMode, playlistFields(playlist, m_playlistEntries.value(id)))) {
            playlistMatches.push_back(playlist);
        }
    }

    int total{0};

    if(m_searchPlaylists && !playlistMatches.isEmpty()) {
        auto* header = createResultHeader(tr("Playlists (%1)").arg(playlistMatches.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonObject& playlist : playlistMatches) {
            createPlaylistItem(header, playlist);
        }
        total += playlistMatches.size();
    }

    if(m_searchArtists && !artists.isEmpty()) {
        auto* header = createResultHeader(tr("Artists (%1)").arg(artists.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonValue& value : artists) {
            addPlaceholder(createArtistItem(value.toObject(), header));
        }
        total += artists.size();
    }

    if(m_searchAlbums && !albums.isEmpty()) {
        auto* header = createResultHeader(tr("Albums (%1)").arg(albums.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonValue& value : albums) {
            addPlaceholder(createAlbumItem(header, value.toObject()));
        }
        total += albums.size();
    }

    if(m_searchSongs && !songs.isEmpty()) {
        auto* header = createResultHeader(tr("Songs (%1)").arg(songs.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonValue& value : songs) {
            createSongItem(header, value.toObject());
        }
        total += songs.size();
    }

    if(total == 0) {
        setStatus(tr("No matches."));
        return;
    }

    setStatus(tr("%1 result(s)").arg(total));
}

void SubsonicBrowser::setGridMode(bool enabled)
{
    m_gridMode = enabled;
    m_settings->set(u"Subsonic/GridView"_s, enabled);

    if(m_stack) {
        m_stack->setCurrentWidget(enabled ? static_cast<QWidget*>(m_grid) : static_cast<QWidget*>(m_tree));
    }
    if(enabled) {
        if(!m_gridLoadedOnce) {
            loadGridAlbums(0);
        }
        else {
            QTimer::singleShot(0, this, &SubsonicBrowser::queueGridCovers);
        }
    }
}

void SubsonicBrowser::clearGrid()
{
    if(!m_grid) {
        return;
    }
    m_grid->clear();
    m_gridItemsByCover.clear();
    ++m_gridDragFetchToken;
    m_gridDragFetching = false;
    m_gridLoaded       = 0;
    m_gridLoading      = false;
    m_gridLoadedOnce   = false;
}

void SubsonicBrowser::loadGridAlbums(int offset)
{
    if(!m_grid || m_gridLoading) {
        return;
    }

    m_gridLoading = true;
    if(offset == 0) {
        clearGrid();
        m_gridLoading = true;
        setStatus(tr("Loading albums…"));
    }

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);

    const int generation = m_generation;
    m_client->getAlbumList2(
        u"alphabeticalByName"_s,
        [this, generation, offset](const QJsonArray& albums, const QString& error) {
            m_gridLoading = false;
            if(generation != m_generation) {
                return;
            }
            if(!error.isEmpty()) {
                setStatus(error);
                return;
            }
            populateGrid(albums, offset);
        },
        GridPageSize, offset);
}

void SubsonicBrowser::populateGrid(const QJsonArray& albums, int offset)
{
    if(offset == 0) {
        clearGrid();
    }

    for(const QJsonValue& value : albums) {
        const QJsonObject album = value.toObject();
        const QString name      = album.value(u"name"_s).toString();
        const QString artist    = album.value(u"artist"_s).toString();
        const QString coverArt  = album.value(u"coverArt"_s).toString();

        auto* item = new QListWidgetItem{m_grid};
        item->setText(artist.isEmpty() ? name : name + u"\n"_s + artist);
        item->setIcon(placeholderCoverIcon());
        item->setData(GridJsonRole, QVariant::fromValue(album));
        item->setData(GridKindRole, GridAlbum);
        item->setData(GridCoverRole, coverArt);
        item->setToolTip(tr("%1 — %2").arg(name, artist));
        if(!coverArt.isEmpty()) {
            m_gridItemsByCover[coverArt].append(item);
        }
    }

    m_gridLoaded += static_cast<int>(albums.size());
    m_gridLoadedOnce = true;

    if(albums.size() >= GridPageSize) {
        auto* more = new QListWidgetItem{tr("Load more…"), m_grid};
        more->setData(GridKindRole, GridLoadMore);
        more->setTextAlignment(Qt::AlignCenter);
    }

    setStatus(tr("%1 album(s)").arg(m_gridLoaded));
    QTimer::singleShot(0, this, &SubsonicBrowser::queueGridCovers);
}

void SubsonicBrowser::queueGridCovers()
{
    if(!m_grid) {
        return;
    }

    // Queue the whole loaded page. Relying on visualItemRect() during the first layout pass only
    // caught the top rows, leaving the rest of the grid without covers until it was scrolled.
    for(int i{0}; i < m_grid->count(); ++i) {
        QListWidgetItem* item = m_grid->item(i);
        if(item && item->data(GridKindRole).toInt() == GridAlbum) {
            requestCover(item->data(GridCoverRole).toString());
        }
    }
}

void SubsonicBrowser::updateGridDetails(QListWidgetItem* item)
{
    if(!m_details) {
        return;
    }
    if(!item || item->data(GridKindRole).toInt() != GridAlbum) {
        m_details->clear();
        return;
    }
    m_details->showAlbum(item->data(GridJsonRole).toJsonObject());
}

void SubsonicBrowser::updateGridAlbumStar(QListWidgetItem* item, bool star)
{
    if(!item) {
        return;
    }
    QJsonObject album = item->data(GridJsonRole).toJsonObject();
    album[u"starred"_s] = star ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate) : QString{};
    item->setData(GridJsonRole, QVariant::fromValue(album));
    const QString id = album.value(u"id"_s).toString();
    updateCachedJson(id, album);
    updateTreeItemJson(id, album);

    const QString name   = album.value(u"name"_s).toString();
    const QString artist = album.value(u"artist"_s).toString();
    item->setText((artist.isEmpty() ? name : name + u"\n"_s + artist) + (star ? u"  ★"_s : QString{}));
    if(m_grid->currentItem() == item) {
        updateGridDetails(item);
    }
}

void SubsonicBrowser::updateGridAlbumRating(QListWidgetItem* item, int rating)
{
    if(!item) {
        return;
    }
    QJsonObject album = item->data(GridJsonRole).toJsonObject();
    album[u"userRating"_s] = rating;
    item->setData(GridJsonRole, QVariant::fromValue(album));
    const QString id = album.value(u"id"_s).toString();
    updateCachedJson(id, album);
    updateTreeItemJson(id, album);
    item->setToolTip(tr("%1 — %2\nRating: %3")
                         .arg(album.value(u"name"_s).toString(), album.value(u"artist"_s).toString(),
                              ratingText(rating)));
    if(m_grid->currentItem() == item) {
        updateGridDetails(item);
    }
}

void SubsonicBrowser::showGridContextMenu(const QPoint& pos)
{
    QListWidgetItem* item = m_grid->itemAt(pos);
    if(!item) {
        return;
    }
    if(item->data(GridKindRole).toInt() == GridLoadMore) {
        loadGridAlbums(m_gridLoaded);
        return;
    }

    const QJsonObject album = item->data(GridJsonRole).toJsonObject();
    const QString id        = album.value(u"id"_s).toString();
    if(id.isEmpty()) {
        return;
    }

    QMenu menu{this};
    QAction* playAction  = menu.addAction(tr("Play"));
    QAction* addAction   = menu.addAction(tr("Add to Playlist"));
    menu.addSeparator();
    QAction* nextAction  = menu.addAction(tr("Play Next"));
    QAction* queueAction = menu.addAction(tr("Add to Queue"));
    menu.addSeparator();
    QAction* downloadAction = menu.addAction(tr("Download to Library"));
    menu.addSeparator();
    QAction* starAction = menu.addAction(isStarred(album) ? tr("Unstar") : tr("Star"));

    QMenu* ratingMenu = menu.addMenu(tr("Rating"));
    QHash<QAction*, int> ratingActions;
    ratingActions.insert(ratingMenu->addAction(tr("No rating")), 0);
    for(int rating{1}; rating <= 5; ++rating) {
        ratingActions.insert(ratingMenu->addAction(tr("%n star(s)", "", rating)), rating);
    }

    menu.addSeparator();
    QMenu* addToServer = menu.addMenu(tr("Add to Server Playlist"));
    QHash<QAction*, QString> playlistActions;
    for(const QJsonObject& playlist : m_playlists) {
        playlistActions.insert(addToServer->addAction(playlist.value(u"name"_s).toString()),
                               playlist.value(u"id"_s).toString());
    }
    addToServer->addSeparator();
    QAction* newPlaylistAction = addToServer->addAction(tr("New Playlist..."));

    QAction* chosen = menu.exec(m_grid->viewport()->mapToGlobal(pos));
    if(!chosen) {
        return;
    }

    if(chosen == playAction) {
        playGridAlbum(item, BrowserAction::Play);
    }
    else if(chosen == addAction) {
        playGridAlbum(item, BrowserAction::AddToPlaylist);
    }
    else if(chosen == nextAction) {
        playGridAlbum(item, BrowserAction::PlayNext);
    }
    else if(chosen == queueAction) {
        playGridAlbum(item, BrowserAction::AddToQueue);
    }
    else if(chosen == downloadAction) {
        playGridAlbum(item, BrowserAction::Download);
    }
    else if(chosen == starAction) {
        const bool star = !isStarred(album);
        m_client->setSettings(loadSubsonicSettings(m_settings));
        m_client->star(id, star, u"albumId"_s);
        updateGridAlbumStar(item, star);
        setStatus(star ? tr("Starred album.") : tr("Unstarred album."));
    }
    else if(ratingActions.contains(chosen)) {
        const int rating = ratingActions.value(chosen);
        m_client->setSettings(loadSubsonicSettings(m_settings));
        m_client->setRating(id, rating);
        updateGridAlbumRating(item, rating);
        setStatus(tr("Rated album."));
    }
    else if(playlistActions.contains(chosen)) {
        addGridAlbumToServerPlaylist(item, playlistActions.value(chosen));
    }
    else if(chosen == newPlaylistAction) {
        createServerPlaylistFromGridAlbum(item);
    }
}

void SubsonicBrowser::playGridAlbum(QListWidgetItem* item, BrowserAction action)
{
    if(!item) {
        return;
    }
    const QJsonObject album = item->data(GridJsonRole).toJsonObject();
    const QString id        = album.value(u"id"_s).toString();
    if(id.isEmpty()) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    const int generation = m_generation;
    m_client->getAlbum(id, [this, generation, action](const QJsonObject& object, const QString& error) {
        if(generation != m_generation) {
            return;
        }
        if(!error.isEmpty()) {
            setStatus(error);
            return;
        }

        const QJsonArray songs = asArray(object.value(u"song"_s));
        if(songs.isEmpty()) {
            setStatus(tr("No tracks in this album."));
            return;
        }

        if(action == BrowserAction::Download) {
            if(!m_libraryDownloads) {
                setStatus(tr("Library downloads are unavailable."));
                return;
            }
            QList<QJsonObject> list;
            list.reserve(songs.size());
            for(const QJsonValue& value : songs) {
                list.push_back(value.toObject());
            }
            m_libraryDownloads->enqueue(list);
            setStatus(tr("Queued %1 track(s) for download.").arg(list.size()));
            return;
        }

        TrackList tracks;
        tracks.reserve(songs.size());
        for(const QJsonValue& value : songs) {
            const QJsonObject song = value.toObject();
            const QString songId   = song.value(u"id"_s).toString();
            const QString local    = m_libraryDownloads ? m_libraryDownloads->existingLocalPath(song) : QString{};
            tracks.push_back(trackForSong(song, local.isEmpty() ? m_client->streamUrl(songId).toString() : local));
        }
        applyAction(tracks, action, true);
    });
}

void SubsonicBrowser::addGridAlbumToServerPlaylist(QListWidgetItem* item, const QString& playlistId)
{
    if(!item || playlistId.isEmpty()) {
        return;
    }
    const QJsonObject album = item->data(GridJsonRole).toJsonObject();
    const QString id        = album.value(u"id"_s).toString();
    if(id.isEmpty()) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->getAlbum(id, [this, playlistId](const QJsonObject& object, const QString& error) {
        if(!error.isEmpty()) {
            setStatus(error);
            return;
        }
        QStringList ids;
        for(const QJsonValue& value : asArray(object.value(u"song"_s))) {
            const QString songId = value.toObject().value(u"id"_s).toString();
            if(!songId.isEmpty()) {
                ids.push_back(songId);
            }
        }
        if(ids.isEmpty()) {
            setStatus(tr("No tracks in this album."));
            return;
        }
        m_client->addToPlaylist(playlistId, ids, [this, count = ids.size()](bool ok, const QString& error) {
            setStatus(ok ? tr("Added %1 track(s) to the server playlist.").arg(count) : error);
            if(ok) {
                refresh();
            }
        });
    });
}

void SubsonicBrowser::createServerPlaylistFromGridAlbum(QListWidgetItem* item)
{
    if(!item) {
        return;
    }
    const QJsonObject album = item->data(GridJsonRole).toJsonObject();
    const QString id        = album.value(u"id"_s).toString();
    if(id.isEmpty()) {
        return;
    }

    bool accepted{false};
    const QString name = QInputDialog::getText(this, tr("New playlist"), tr("Playlist name:"), QLineEdit::Normal,
                                               {}, &accepted);
    if(!accepted || name.trimmed().isEmpty()) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->getAlbum(id, [this, playlistName = name.trimmed()](const QJsonObject& object, const QString& error) {
        if(!error.isEmpty()) {
            setStatus(error);
            return;
        }
        QStringList ids;
        for(const QJsonValue& value : asArray(object.value(u"song"_s))) {
            const QString songId = value.toObject().value(u"id"_s).toString();
            if(!songId.isEmpty()) {
                ids.push_back(songId);
            }
        }
        if(ids.isEmpty()) {
            setStatus(tr("No tracks in this album."));
            return;
        }
        m_client->createPlaylist(playlistName, ids, [this](bool ok, const QString& error) {
            setStatus(ok ? tr("Playlist created.") : error);
            if(ok) {
                refresh();
            }
        });
    });
}

void SubsonicBrowser::setStatus(const QString& text)
{
    m_status->setText(text);
}
} // namespace Fooyin::Subsonic
