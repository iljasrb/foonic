/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicbrowser.h"

#include "subsonicclient.h"
#include "subsoniclibrarydownloader.h"
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
#include <QDir>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
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

// Stable identity of an expandable tree item, used to remember open/closed state across refreshes.
QString expansionKey(const QTreeWidgetItem* item)
{
    const int kind = item->data(0, KindRole).toInt();
    if(kind != Artist && kind != Album && kind != PlaylistNode) {
        return {};
    }
    return QString::number(kind) + u":"_s + item->data(0, IdRole).toString();
}

struct FilterTerm
{
    QString field; // empty = match any field
    QString value;
    bool negated{false};
};

// Splits the filter into field-scoped/negated terms. In Contains mode, plain words are joined back
// into one phrase so they must appear together; in the word modes they stay separate.
QList<FilterTerm> parseFilter(const QString& text, Fooyin::Subsonic::MatchMode mode)
{
    static const QSet<QString> knownFields
        = {u"artist"_s, u"album"_s, u"title"_s, u"genre"_s, u"playlist"_s, u"year"_s};

    QStringList words;
    QString current;
    bool inQuotes{false};
    for(const QChar ch : text) {
        if(ch == u'"') {
            inQuotes = !inQuotes;
            continue;
        }
        if(ch.isSpace() && !inQuotes) {
            if(!current.isEmpty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current += ch;
    }
    if(!current.isEmpty()) {
        words.push_back(current);
    }

    QList<FilterTerm> terms;
    QString barePhrase;
    for(QString word : words) {
        FilterTerm term;
        if(word.size() > 1 && word.startsWith(u'-')) {
            term.negated = true;
            word.remove(0, 1);
        }
        const int colon = word.indexOf(u':');
        if(colon > 0) {
            const QString field = word.left(colon).toLower();
            if(knownFields.contains(field)) {
                term.field = field;
                word       = word.mid(colon + 1);
            }
        }
        if(word.isEmpty()) {
            continue;
        }
        if(mode == Fooyin::Subsonic::MatchMode::Contains && !term.negated && term.field.isEmpty()) {
            if(!barePhrase.isEmpty()) {
                barePhrase += u' ';
            }
            barePhrase += word;
            continue;
        }
        term.value = word;
        terms.push_back(term);
    }
    if(!barePhrase.isEmpty()) {
        FilterTerm phrase;
        phrase.value = barePhrase;
        terms.push_front(phrase);
    }
    return terms;
}

QString serverQueryFor(const QList<FilterTerm>& terms)
{
    QStringList parts;
    for(const FilterTerm& term : terms) {
        if(!term.negated && !term.value.isEmpty()) {
            parts.push_back(term.value);
        }
    }
    return parts.join(u' ');
}

bool termMatches(const FilterTerm& term, const QHash<QString, QString>& fields)
{
    if(term.field.isEmpty()) {
        for(const QString& value : fields) {
            if(value.contains(term.value, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }
    return fields.value(term.field).contains(term.value, Qt::CaseInsensitive);
}

bool matchesFilter(const QList<FilterTerm>& terms, Fooyin::Subsonic::MatchMode mode,
                   const QHash<QString, QString>& fields)
{
    if(terms.isEmpty()) {
        return true;
    }
    bool anyPositive{false};
    bool positive{mode != Fooyin::Subsonic::MatchMode::AnyWord}; // AND starts true, OR starts false
    for(const FilterTerm& term : terms) {
        const bool matched = termMatches(term, fields);
        if(term.negated) {
            if(matched) {
                return false;
            }
            continue;
        }
        anyPositive = true;
        positive    = (mode == Fooyin::Subsonic::MatchMode::AnyWord) ? (positive || matched) : (positive && matched);
    }
    return anyPositive ? positive : true;
}

QHash<QString, QString> artistFields(const QJsonObject& artist)
{
    return {{u"artist"_s, artist.value(u"name"_s).toString()}};
}

QHash<QString, QString> albumFields(const QJsonObject& album)
{
    return {{u"album"_s, album.value(u"name"_s).toString()}, {u"artist"_s, album.value(u"artist"_s).toString()}};
}

QHash<QString, QString> songFields(const QJsonObject& song)
{
    QHash<QString, QString> fields;
    fields[u"title"_s]  = song.value(u"title"_s).toString();
    fields[u"artist"_s] = song.value(u"artist"_s).toString();
    fields[u"album"_s]  = song.value(u"album"_s).toString();
    fields[u"genre"_s]  = song.value(u"genre"_s).toString();
    fields[u"year"_s]   = QString::number(song.value(u"year"_s).toInt());
    return fields;
}

QHash<QString, QString> playlistFields(const QJsonObject& playlist, const QJsonArray& entries)
{
    QHash<QString, QString> fields;
    fields[u"playlist"_s] = playlist.value(u"name"_s).toString();

    QString titles;
    QString artists;
    QString albums;
    for(const QJsonValue& value : entries) {
        const QJsonObject entry = value.toObject();
        titles += entry.value(u"title"_s).toString() + u' ';
        artists += entry.value(u"artist"_s).toString() + u' ';
        albums += entry.value(u"album"_s).toString() + u' ';
    }
    if(!titles.isEmpty()) {
        fields[u"title"_s] = titles;
    }
    if(!artists.isEmpty()) {
        fields[u"artist"_s] = artists;
    }
    if(!albums.isEmpty()) {
        fields[u"album"_s] = albums;
    }
    return fields;
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
                [this](const QString&, const QJsonObject&, const QString&) { updateDownloadedColumn(); });
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
    filterMenu->addSection(tr("Search in"));
    const auto addScope = [this, filterMenu](const QString& text, bool& flag) {
        auto* action = filterMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(flag);
        connect(action, &QAction::toggled, this, [this, flagPtr = &flag](bool on) {
            *flagPtr = on;
            applyFilter();
        });
    };
    addScope(tr("Artists"), m_searchArtists);
    addScope(tr("Albums"), m_searchAlbums);
    addScope(tr("Songs"), m_searchSongs);
    addScope(tr("Playlists"), m_searchPlaylists);

    filterMenu->addSection(tr("Match"));
    auto* matchGroup = new QActionGroup{filterMenu};
    matchGroup->setExclusive(true);
    const auto addMatch = [this, filterMenu, matchGroup](const QString& text, MatchMode mode) {
        auto* action = filterMenu->addAction(text);
        action->setCheckable(true);
        action->setChecked(m_matchMode == mode);
        matchGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            m_matchMode = mode;
            applyFilter();
        });
    };
    addMatch(tr("Contains"), MatchMode::Contains);
    addMatch(tr("All words"), MatchMode::AllWords);
    addMatch(tr("Any word"), MatchMode::AnyWord);
    m_filterOptions->setMenu(filterMenu);

    m_filterTimer = new QTimer{this};
    m_filterTimer->setSingleShot(true);
    m_filterTimer->setInterval(150);

    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(10, 8, 10, 0);
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(m_status, 1);
    toolbar->addWidget(m_filterOptions);
    toolbar->addWidget(m_filter);

    m_tree->setHeaderLabels({tr("Name"), tr("Artist"), tr("Album"), tr("Length"), tr("Size"), tr("Downloaded")});
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
    connect(m_tree, &QTreeWidget::itemCollapsed, this, &SubsonicBrowser::onCollapsed);
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
    // Remember where the user was so the rebuilt (and lazily reloaded) tree can restore it.
    m_pendingScroll = m_tree->verticalScrollBar()->value();
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
        m_client->getArtist(id, [this, id, finish, remaining](const QJsonObject& object, const QString& error) {
            const QJsonArray albums = error.isEmpty() ? asArray(object.value(u"album"_s)) : QJsonArray{};
            // tracksForDrag builds an artist from this cache, so store the album list here too; without
            // it the first drag produced no tracks and only the (async) tree load made the next one work.
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

    restoreExpansion(item);
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

    child->setData(0, KindRole, Song);
    child->setData(0, IdRole, song.value(u"id"_s).toString());
    child->setData(0, JsonRole, song);
    child->setData(0, CoverRole, song.value(u"coverArt"_s).toString());
    applyCover(child, song.value(u"coverArt"_s).toString());

    child->setText(5, downloadedText(song));
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

    if(state->action == BrowserAction::Download) {
        downloadToLibrary(state);
        return;
    }

    const SubsonicSettings settings = loadSubsonicSettings(m_settings);
    m_client->setSettings(settings);

    qCInfo(SUBSONIC) << "Adding" << state->songs.size() << "track(s), total" << formatBytes(state->bytes);

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
        case BrowserAction::Download:
            break; // handled in finishGather() before tracks are built
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

    QMenu menu{this};
    QAction* playAction  = menu.addAction(tr("Play"));
    QAction* addAction   = menu.addAction(tr("Add to Playlist"));
    menu.addSeparator();
    QAction* nextAction  = menu.addAction(tr("Play Next"));
    QAction* queueAction = menu.addAction(tr("Add to Queue"));
    menu.addSeparator();
    QAction* downloadAction = menu.addAction(tr("Download to Library"));
    QAction* starAction{nullptr};
    QAction* unstarAction{nullptr};
    if(item->data(0, KindRole).toInt() == Song) {
        menu.addSeparator();
        starAction   = menu.addAction(tr("Star"));
        unstarAction = menu.addAction(tr("Unstar"));
    }

    QStringList songIds;
    for(QTreeWidgetItem* selected : m_tree->selectedItems()) {
        if(selected->data(0, KindRole).toInt() == Song) {
            songIds.push_back(selected->data(0, IdRole).toString());
        }
    }

    QHash<QAction*, QString> playlistActions;
    QAction* newServerPlaylistAction{nullptr};
    if(!songIds.isEmpty()) {
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
    if(item->data(0, KindRole).toInt() == PlaylistNode) {
        menu.addSeparator();
        renamePlaylistAction = menu.addAction(tr("Rename Playlist..."));
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
    else if(chosen == starAction || chosen == unstarAction) {
        const bool star = chosen == starAction;
        m_client->setSettings(loadSubsonicSettings(m_settings));
        QStringList ids;
        for(QTreeWidgetItem* selected : m_tree->selectedItems()) {
            if(selected->data(0, KindRole).toInt() == Song) {
                ids.push_back(selected->data(0, IdRole).toString());
            }
        }
        if(ids.isEmpty()) {
            ids.push_back(item->data(0, IdRole).toString());
        }
        for(const QString& id : ids) {
            if(!id.isEmpty()) {
                m_client->star(id, star);
            }
        }
        setStatus(tr("Starred %1 song(s).").arg(ids.size()));
    }
    else if(playlistActions.contains(chosen)) {
        addSongsToServerPlaylist(playlistActions.value(chosen), songIds);
    }
    else if(newServerPlaylistAction && chosen == newServerPlaylistAction) {
        createServerPlaylist(songIds);
    }
    else if(renamePlaylistAction && chosen == renamePlaylistAction) {
        renameSelectedPlaylist(item);
    }
    else if(deletePlaylistAction && chosen == deletePlaylistAction) {
        deleteSelectedPlaylist(item);
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
        QList<QJsonObject> songs;
        songs.reserve(items.size());
        for(const QJsonValue& value : items) {
            songs.push_back(value.toObject());
        }
        showDiscoverySongs(title, songs);
    };

    if(chosen == randomAction) {
        setStatus(tr("Loading random songs..."));
        m_client->getRandomSongs([show](const QJsonArray& items, const QString& error) {
            show(tr("Random"), items, error);
        });
    }
    else {
        setStatus(tr("Loading starred songs..."));
        m_client->getStarred([show](const QJsonArray& items, const QString& error) {
            show(tr("Starred"), items, error);
        });
    }
}

void SubsonicBrowser::showDiscoverySongs(const QString& title, const QList<QJsonObject>& songs)
{
    if(songs.isEmpty()) {
        setStatus(tr("%1: no songs.").arg(title));
        return;
    }

    auto* header = createResultHeader(tr("%1 (%2)").arg(title).arg(songs.size()));
    m_tree->insertTopLevelItem(0, header);
    for(const QJsonObject& song : songs) {
        createSongItem(header, song);
    }
    header->setExpanded(true);
    m_tree->scrollToItem(header);
    setStatus(tr("%1: %2 song(s)").arg(title).arg(songs.size()));
}

void SubsonicBrowser::createServerPlaylist(const QStringList& songIds)
{
    bool accepted{false};
    const QString name = QInputDialog::getText(this, tr("New playlist"), tr("Playlist name:"), QLineEdit::Normal,
                                               {}, &accepted);
    if(!accepted || name.trimmed().isEmpty()) {
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->createPlaylist(name.trimmed(), songIds, [this](bool ok, const QString& error) {
        setStatus(ok ? tr("Playlist created.") : error);
        if(ok) {
            refresh();
        }
    });
}

void SubsonicBrowser::addSongsToServerPlaylist(const QString& id, const QStringList& songIds)
{
    if(id.isEmpty() || songIds.isEmpty()) {
        return;
    }
    m_client->setSettings(loadSubsonicSettings(m_settings));
    m_client->addToPlaylist(id, songIds, [this](bool ok, const QString& error) {
        setStatus(ok ? tr("Added to playlist.") : error);
        if(ok) {
            refresh();
        }
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
        });
    }

    if(menu.exec(header->viewport()->mapToGlobal(pos)) == showAll) {
        for(int column{0}; column < m_tree->columnCount(); ++column) {
            m_tree->setColumnHidden(column, false);
        }
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
    const QString coverUrl
        = coverArt.isEmpty()
            ? QString{}
            : m_client->coverArtUrl(coverArt, loadSubsonicSettings(m_settings).coverArtSize).toString();
    return trackFromSong(song, filepath, coverUrl);
}

QString SubsonicBrowser::coverCachePath(const QString& coverArtId) const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/foonic/covers");
    QDir{}.mkpath(dir);
    const QByteArray key = QCryptographicHash::hash(coverArtId.toUtf8(), QCryptographicHash::Sha1).toHex();
    return dir + u'/' + QString::fromLatin1(key) + u".jpg"_s;
}

QString SubsonicBrowser::downloadedText(const QJsonObject& song) const
{
    return m_libraryDownloads && !m_libraryDownloads->existingLocalPath(song).isEmpty()
        ? u"\u2713 Downloaded"_s
        : QString{};
}

void SubsonicBrowser::updateDownloadedColumn()
{
    // ponytail: linear scan on every finished download; track just that row if the tree grows huge.
    QTreeWidgetItemIterator it{m_tree};
    while(*it) {
        QTreeWidgetItem* item = *it;
        if(item->data(0, KindRole).toInt() == Song) {
            const QString text = downloadedText(item->data(0, JsonRole).toJsonObject());
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

    // Disk cache first, so a restart doesn't re-fetch every cover.
    QPixmap pixmap;
    if(pixmap.load(coverCachePath(coverArtId))) {
        m_coverCache.insert(coverArtId, pixmap);
        applyCoverToTree(coverArtId);
        return;
    }

    m_coverRequests.insert(coverArtId);

    QNetworkReply* reply = m_network->get(QNetworkRequest{m_client->coverArtUrl(coverArtId, 64)});
    connect(reply, &QNetworkReply::finished, this, [this, reply, coverArtId] {
        reply->deleteLater();

        if(reply->error() == QNetworkReply::NoError) {
            QPixmap downloaded;
            if(downloaded.loadFromData(reply->readAll())) {
                downloaded = downloaded.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                m_coverCache.insert(coverArtId, downloaded);
                downloaded.save(coverCachePath(coverArtId), "JPG");
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

        const QString query = serverQueryFor(parseFilter(m_filterText, m_matchMode));
        if(m_searchPlaylists) {
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
    restoreExpansion(nullptr);
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
    restoreExpansion(header);
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

void SubsonicBrowser::setStatus(const QString& text)
{
    m_status->setText(text);
}
} // namespace Fooyin::Subsonic
