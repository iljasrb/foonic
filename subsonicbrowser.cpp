/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicbrowser.h"

#include "subsonicclient.h"
#include "subsonicdownloadmanager.h"
#include "subsonicsettings.h"
#include "subsonictrack.h"

#include <gui/playlist/playlistinteractor.h>
#include <gui/trackmimedata.h>
#include <core/network/networkaccessmanager.h>
#include <core/player/playercontroller.h>

#include <utils/id.h>
#include <utils/settings/settingsdialogcontroller.h>

#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {
constexpr int KindRole   = Qt::UserRole;
constexpr int IdRole     = Qt::UserRole + 1;
constexpr int JsonRole   = Qt::UserRole + 2;
constexpr int LoadedRole = Qt::UserRole + 3;
constexpr int CoverRole  = Qt::UserRole + 4;

enum ItemKind : uint8_t
{
    Placeholder = 0,
    Artist      = 1,
    Album       = 2,
    Song        = 3,
    ResultHeader = 4,
    PlaylistNode = 5,
};

// Cap concurrent playlist-entry requests during a search scan so a large playlist library doesn't
// flood the server.
constexpr int MaxPlaylistFetches = 4;

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
} // namespace

namespace Fooyin::Subsonic {
SubsonicBrowser::SubsonicBrowser(std::shared_ptr<NetworkAccessManager> network, PlaylistInteractor* interactor,
                                 SubsonicDownloadManager* downloads, SettingsManager* settings, QWidget* parent)
    : QWidget{parent}
    , m_network{std::move(network)}
    , m_interactor{interactor}
    , m_downloads{downloads}
    , m_settings{settings}
    , m_client{new SubsonicClient{m_network, loadSubsonicSettings(settings), this}}
    , m_status{new QLabel{this}}
    , m_tree{new SubsonicTreeWidget{this}}
{
    auto* tree = static_cast<SubsonicTreeWidget*>(m_tree);
    tree->mimeDataProvider = [this](const QList<QTreeWidgetItem*>& items) { return tracksForDrag(items); };
    tree->dragDataReady  = [this](const QList<QTreeWidgetItem*>& items) { return ensureDragData(items); };
    tree->cancelDragFetch = [this] { cancelDragFetch(); };

    auto* refreshButton = new QPushButton{tr("Refresh"), this};

    m_filter = new QLineEdit{this};
    m_filter->setPlaceholderText(tr("Filter..."));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(220);

    m_filterTimer = new QTimer{this};
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(150);

    auto* toolbar = new QHBoxLayout;
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(m_status, 1);
    toolbar->addWidget(m_filter);

    m_tree->setHeaderLabels({tr("Name"), tr("Artist"), tr("Album"), tr("Length"), tr("Size"), tr("Cache")});
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

    auto* layout = new QVBoxLayout{this};
    layout->setContentsMargins({});
    layout->addLayout(toolbar);
    layout->addWidget(m_tree, 1);

    connect(refreshButton, &QPushButton::clicked, this, &SubsonicBrowser::refresh);
    connect(m_filterTimer, &QTimer::timeout, this, &SubsonicBrowser::applyFilter);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_filterText = text.trimmed();
        m_filterTimer->start();
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, &SubsonicBrowser::onExpanded);
    connect(m_tree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) { ensureChildrenLoaded(item); });
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

    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &SubsonicBrowser::showContextMenu);

    if(m_downloads) {
        connect(m_downloads, &SubsonicDownloadManager::downloadProgressChanged, this,
                &SubsonicBrowser::updateDownloadProgress);
    }

    // Refresh once the (modeless) settings dialog is closed so config changes take effect.
    if(auto* controller = m_settings->settingsDialog()) {
        connect(controller, &SettingsDialogController::closing, this, [this] {
            m_client->setSettings(loadSubsonicSettings(m_settings));
            m_coverCache.clear();
            m_coverRequests.clear();
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
    clearTree();
    m_artists.clear();
    m_playlists.clear();
    m_playlistEntries.clear();
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
        else {
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
            const QJsonObject artist = value.toObject();
            m_artists.push_back(artist);
            auto* item = createArtistItem(artist);
            addPlaceholder(item);
        }

        if(!m_filterText.isEmpty()) {
            applyFilter();
        }
        else {
            setStatus(tr("%1 artists").arg(artists.size()));
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
    ensureChildrenLoaded(item);
}

void SubsonicBrowser::ensureChildrenLoaded(QTreeWidgetItem* item, bool expand)
{
    if(!item || item->data(0, LoadedRole).toBool()) {
        return;
    }

    const int kind = item->data(0, KindRole).toInt();
    if(kind != Artist && kind != Album && kind != PlaylistNode) {
        return;
    }

    item->setData(0, LoadedRole, false);
    loadChildren(item, kind, item->data(0, IdRole).toString(), {}, expand);
}

bool SubsonicBrowser::ensureDragData(const QList<QTreeWidgetItem*>& items)
{
    QStringList albumIds;
    QStringList playlistIds;
    QStringList artistIds;

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
        m_client->getArtist(id, [this, finish, remaining](const QJsonObject& object, const QString& error) {
            const QJsonArray albums = error.isEmpty() ? asArray(object.value(u"album"_s)) : QJsonArray{};
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

    return false; // drag is deferred until the fetches above finish
}

void SubsonicBrowser::cancelDragFetch()
{
    ++m_dragFetchToken;
    m_dragFetching = false;
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

    if(settings.playbackMode == PlaybackMode::DownloadFirst && m_downloads) {
        QStringList urls;
        urls.reserve(songs.size());
        for(const QJsonObject& song : songs) {
            urls.push_back(m_client->streamUrl(song.value(u"id"_s).toString()).toString());
        }

        const QStringList paths = m_downloads->registerSongs(songs, urls, settings.downloadDir);

        TrackList tracks;
        tracks.reserve(songs.size());
        for(int i{0}; i < songs.size(); ++i) {
            const QString songId = songs.at(i).value(u"id"_s).toString();
            const bool cached
                = m_downloads->cacheStateForSong(songId) == SubsonicDownloadManager::CacheState::Cached;
            tracks.push_back(trackForSong(songs.at(i), cached ? paths.at(i) : urls.at(i)));
        }

        qCInfo(SUBSONIC) << "Dragging" << tracks.size() << "track(s) (download-first)";
        return tracks;
    }

    TrackList tracks;
    tracks.reserve(songs.size());
    for(const QJsonObject& song : songs) {
        tracks.push_back(trackForSong(song, m_client->streamUrl(song.value(u"id"_s).toString()).toString()));
    }
    qCInfo(SUBSONIC) << "Dragging" << tracks.size() << "track(s) (stream)";
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
                    delete item->takeChild(0);
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
        delete item->takeChild(0);
    }

    item->setData(0, LoadedRole, true);

    if(kind == Artist) {
        const QJsonArray albums = asArray(object.value(u"album"_s));
        m_albumsByArtist.insert(item->data(0, IdRole).toString(), albums);
        for(const QJsonValue& value : albums) {
            auto* child = createAlbumItem(item, value.toObject());
            addPlaceholder(child);

            // Pre-load each album's songs so the artist (and album) can be dragged as a whole
            // without expanding every album first.
            ensureChildrenLoaded(child, false);
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
}

QTreeWidgetItem* SubsonicBrowser::createArtistItem(const QJsonObject& artist, QTreeWidgetItem* parent)
{
    auto* item = parent ? new QTreeWidgetItem{parent} : new QTreeWidgetItem{m_tree};
    item->setText(0, artist.value(u"name"_s).toString());
    item->setData(0, KindRole, Artist);
    item->setData(0, IdRole, artist.value(u"id"_s).toString());
    return item;
}

QTreeWidgetItem* SubsonicBrowser::createAlbumItem(QTreeWidgetItem* parent, const QJsonObject& album)
{
    auto* child = new QTreeWidgetItem{parent};
    child->setText(0, album.value(u"name"_s).toString());
    child->setText(1, album.value(u"artist"_s).toString());
    child->setText(2, QString::number(album.value(u"year"_s).toInt()));
    child->setData(0, KindRole, Album);
    child->setData(0, IdRole, album.value(u"id"_s).toString());
    child->setData(0, CoverRole, album.value(u"coverArt"_s).toString());
    applyCover(child, album.value(u"coverArt"_s).toString());
    return child;
}

QTreeWidgetItem* SubsonicBrowser::createSongItem(QTreeWidgetItem* parent, const QJsonObject& song)
{
    auto* child = new QTreeWidgetItem{parent};

    const int trackNumber = song.value(u"track"_s).toInt();
    const QString title   = song.value(u"title"_s).toString();
    child->setText(0, trackNumber > 0 ? u"%1. %2"_s.arg(trackNumber, 2, 10, QLatin1Char{'0'}).arg(title)
                                      : title);
    child->setText(1, song.value(u"artist"_s).toString());
    child->setText(2, song.value(u"album"_s).toString());

    const int seconds = song.value(u"duration"_s).toInt();
    if(seconds > 0) {
        child->setText(3, u"%1:%2"_s.arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char{'0'}));
    }

    const qint64 size = songSize(song);
    if(size > 0) {
        child->setText(4, formatBytes(size));
    }

    qCInfo(SUBSONIC) << "Track:" << title << "artist=" << song.value(u"artist"_s).toString()
                     << "size=" << size << "bytes" << "duration=" << seconds << "s"
                     << "bitrate=" << song.value(u"bitRate"_s).toInt() << "kbps"
                     << "codec=" << song.value(u"suffix"_s).toString();

    child->setData(0, KindRole, Song);
    child->setData(0, IdRole, song.value(u"id"_s).toString());
    child->setData(0, JsonRole, song);
    child->setData(0, CoverRole, song.value(u"coverArt"_s).toString());
    applyCover(child, song.value(u"coverArt"_s).toString());
    child->setText(5, cacheText(song.value(u"id"_s).toString()));
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
        } else if(kind == Artist || kind == Album || kind == PlaylistNode) {
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

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);

    qCInfo(SUBSONIC) << "Adding" << state->songs.size() << "track(s), total" << formatBytes(state->bytes)
                     << "(" << state->bytes << "bytes ), mode="
                     << (settings.playbackMode == PlaybackMode::DownloadFirst ? "download-first" : "stream")
                     << "read-ahead=" << m_settings->value(QStringLiteral("Engine/RemoteReadAheadKb")).toInt() << "kB";

    if(settings.playbackMode == PlaybackMode::DownloadFirst) {
        downloadAndPlay(state);
        return;
    }

    TrackList tracks;
    tracks.reserve(state->songs.size());
    for(const QJsonObject& song : state->songs) {
        tracks.push_back(trackForSong(song, m_client->streamUrl(song.value(u"id"_s).toString()).toString()));
    }
    applyAction(tracks, state->action, true);
}

void SubsonicBrowser::downloadAndPlay(const std::shared_ptr<GatherState>& state)
{
    if(!m_downloads) {
        qCWarning(SUBSONIC) << "No download manager available; using streaming only";
        TrackList tracks;
        tracks.reserve(state->songs.size());
        for(const QJsonObject& song : state->songs) {
            tracks.push_back(trackForSong(song, m_client->streamUrl(song.value(u"id"_s).toString()).toString()));
        }
        applyAction(tracks, state->action, true);
        return;
    }

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);

    // Use a unique remote stream URL per song; the same URL is registered so the download manager
    // can promote the playlist entry to the cached file once it has been fetched.
    QStringList urls;
    urls.reserve(state->songs.size());
    for(const QJsonObject& song : state->songs) {
        urls.push_back(m_client->streamUrl(song.value(u"id"_s).toString()).toString());
    }

    const QStringList paths = m_downloads->registerSongs(state->songs, urls, settings.downloadDir);

    qCInfo(SUBSONIC) << "download-first:" << state->songs.size() << "track(s), already cached="
                     << m_downloads->cachedCount();

    // Entries already cached play (and seek) as local files; the rest stream immediately and are
    // swapped to the cache in place once downloaded.
    TrackList tracks;
    tracks.reserve(state->songs.size());
    for(int i{0}; i < state->songs.size(); ++i) {
        const QString songId = state->songs.at(i).value(u"id"_s).toString();
        const bool cached    = m_downloads->cacheStateForSong(songId) == SubsonicDownloadManager::CacheState::Cached;
        tracks.push_back(trackForSong(state->songs.at(i), cached ? paths.at(i) : urls.at(i)));
    }

    applyAction(tracks, state->action, true);
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
    }
}

void SubsonicBrowser::showContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if(!item) {
        return;
    }

    if(!item->isSelected()) {
        m_tree->clearSelection();
        item->setSelected(true);
    }

    QMenu menu{this};
    QAction* playAction  = menu.addAction(tr("Play"));
    QAction* addAction   = menu.addAction(tr("Add to Playlist"));
    menu.addSeparator();
    QAction* nextAction  = menu.addAction(tr("Play Next"));
    QAction* queueAction = menu.addAction(tr("Add to Queue"));

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

Track SubsonicBrowser::trackForSong(const QJsonObject& song, const QString& filepath) const
{
    const QString coverArt = song.value(u"coverArt"_s).toString();
    const QString coverUrl = coverArt.isEmpty() ? QString{} : m_client->coverArtUrl(coverArt).toString();
    return trackFromSong(song, filepath, coverUrl);
}

QString SubsonicBrowser::cacheText(const QString& songId) const
{
    if(!m_downloads) {
        return {};
    }

    switch(m_downloads->cacheStateForSong(songId)) {
        case SubsonicDownloadManager::CacheState::Cached:
            return u"\u2713"_s;
        case SubsonicDownloadManager::CacheState::Downloading: {
            const int progress = m_downloads->downloadProgressForSong(songId);
            return progress > 0 ? u"\u2193 %1%"_s.arg(progress) : u"\u2193"_s;
        }
        case SubsonicDownloadManager::CacheState::NotCached:
            break;
    }
    return {};
}

void SubsonicBrowser::updateDownloadProgress()
{
    // ponytail: linear scan on every progress tick; switch to tracking just the active row if the tree
    // ever holds thousands of songs.
    QTreeWidgetItemIterator it{m_tree};
    while(*it) {
        QTreeWidgetItem* item = *it;
        if(item->data(0, KindRole).toInt() == Song) {
            const QString text = cacheText(item->data(0, IdRole).toString());
            if(item->text(5) != text) {
                item->setText(5, text);
            }
        }
        ++it;
    }
}

void SubsonicBrowser::applyCover(QTreeWidgetItem* item, const QString& coverArtId)
{
    if(!item || coverArtId.isEmpty()) {
        return;
    }

    if(const auto cached = m_coverCache.constFind(coverArtId); cached != m_coverCache.constEnd()) {
        item->setIcon(0, QIcon{cached.value()});
        return;
    }

    requestCover(coverArtId);
}

void SubsonicBrowser::requestCover(const QString& coverArtId)
{
    if(m_coverRequests.contains(coverArtId)) {
        return;
    }
    m_coverRequests.insert(coverArtId);

    QNetworkReply* reply = m_network->get(QNetworkRequest{m_client->coverArtUrl(coverArtId, 64)});
    connect(reply, &QNetworkReply::finished, this, [this, reply, coverArtId] {
        reply->deleteLater();

        if(reply->error() == QNetworkReply::NoError) {
            QPixmap pixmap;
            if(pixmap.loadFromData(reply->readAll())) {
                m_coverCache.insert(coverArtId,
                                    pixmap.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                applyCoverToTree(coverArtId);
                // Bound memory; visible rows keep their already-set icons.
                if(m_coverCache.size() > 1024) {
                    m_coverCache.clear();
                }
                return;
            }
        }

        // Allow a later refresh to retry a failed download.
        m_coverRequests.remove(coverArtId);
    });
}

void SubsonicBrowser::applyCoverToTree(const QString& coverArtId)
{
    const auto cached = m_coverCache.constFind(coverArtId);
    if(cached == m_coverCache.constEnd()) {
        return;
    }

    const QIcon icon{cached.value()};
    QTreeWidgetItemIterator it{m_tree};
    while(*it) {
        QTreeWidgetItem* item = *it;
        if(item->data(0, CoverRole).toString() == coverArtId) {
            item->setIcon(0, icon);
        }
        ++it;
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
        m_filterMode         = true;
        const int generation = ++m_searchGeneration;
        m_searchResultReady  = false;
        setStatus(tr("Searching..."));
        startPlaylistScan();
        m_client->search(m_filterText, [this, generation](const QJsonObject& result, const QString& error) {
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

    m_applyingFilter = false;
}

void SubsonicBrowser::clearTree()
{
    // Invalidate any in-flight lazy loads so their callbacks don't touch deleted items.
    ++m_treeGeneration;
    m_pendingLoads.clear();
    m_loading.clear();
    m_tree->clear();
}

void SubsonicBrowser::rebuildArtistTree()
{
    clearTree();
    addPlaylistsHeader();
    for(const QJsonObject& artist : m_artists) {
        auto* item = createArtistItem(artist);
        addPlaceholder(item);
    }
    setStatus(tr("%1 artists").arg(m_artists.size()));
}

QTreeWidgetItem* SubsonicBrowser::createResultHeader(const QString& title)
{
    auto* header = new QTreeWidgetItem;
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
    item->setText(0, playlist.value(u"name"_s).toString());
    item->setText(1, playlist.value(u"owner"_s).toString());
    item->setText(2, tr("%1 songs").arg(playlist.value(u"songCount"_s).toInt()));
    item->setData(0, KindRole, PlaylistNode);
    item->setData(0, IdRole, playlist.value(u"id"_s).toString());
    item->setData(0, CoverRole, playlist.value(u"coverArt"_s).toString());
    applyCover(item, playlist.value(u"coverArt"_s).toString());
    addPlaceholder(item);
    return item;
}

void SubsonicBrowser::addPlaylistsHeader()
{
    if(m_playlists.isEmpty()) {
        return;
    }

    auto* header = createResultHeader(tr("Playlists (%1)").arg(m_playlists.size()));
    m_tree->insertTopLevelItem(0, header);
    for(const QJsonObject& playlist : m_playlists) {
        createPlaylistItem(header, playlist);
    }
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

    const QJsonArray artists = asArray(result.value(u"artist"_s));
    const QJsonArray albums  = asArray(result.value(u"album"_s));
    const QJsonArray songs   = asArray(result.value(u"song"_s));

    // Playlists aren't part of search3, so match them locally by name or by any track they hold.
    QList<QJsonObject> playlistMatches;
    for(const QJsonObject& playlist : m_playlists) {
        const QString id = playlist.value(u"id"_s).toString();
        bool matches     = playlist.value(u"name"_s).toString().contains(m_filterText, Qt::CaseInsensitive);
        if(!matches) {
            for(const QJsonValue& entry : m_playlistEntries.value(id)) {
                const QJsonObject song = entry.toObject();
                if(song.value(u"title"_s).toString().contains(m_filterText, Qt::CaseInsensitive)
                   || song.value(u"artist"_s).toString().contains(m_filterText, Qt::CaseInsensitive)
                   || song.value(u"album"_s).toString().contains(m_filterText, Qt::CaseInsensitive)) {
                    matches = true;
                    break;
                }
            }
        }
        if(matches) {
            playlistMatches.push_back(playlist);
        }
    }

    if(!playlistMatches.isEmpty()) {
        auto* header = createResultHeader(tr("Playlists (%1)").arg(playlistMatches.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonObject& playlist : playlistMatches) {
            createPlaylistItem(header, playlist);
        }
    }

    if(!artists.isEmpty()) {
        auto* header = createResultHeader(tr("Artists (%1)").arg(artists.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonValue& value : artists) {
            addPlaceholder(createArtistItem(value.toObject(), header));
        }
    }

    if(!albums.isEmpty()) {
        auto* header = createResultHeader(tr("Albums (%1)").arg(albums.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonValue& value : albums) {
            addPlaceholder(createAlbumItem(header, value.toObject()));
        }
    }

    if(!songs.isEmpty()) {
        auto* header = createResultHeader(tr("Songs (%1)").arg(songs.size()));
        m_tree->addTopLevelItem(header);
        for(const QJsonValue& value : songs) {
            createSongItem(header, value.toObject());
        }
    }

    const int total = artists.size() + albums.size() + songs.size() + playlistMatches.size();
    if(total == 0) {
        setStatus(tr("No matches."));
        return;
    }

    setStatus(tr("%1 result(s)").arg(total));
}

void SubsonicBrowser::setStatus(const QString& text)
{
    m_status->setText(text);
}
} // namespace Fooyin::Subsonic
