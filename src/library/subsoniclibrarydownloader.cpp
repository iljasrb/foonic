/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsoniclibrarydownloader.h"

#include "subsonicclient.h"
#include "subsonicsettings.h"
#include "subsonicstreamutils.h"
#include "subsonictrack.h"

#include <core/engine/audioloader.h>
#include <core/library/musiclibrary.h>
#include <core/network/networkaccessmanager.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
namespace {
constexpr int MaxAttempts   = 3;
constexpr int RetryDelayMs  = 750;
constexpr int CoverSize     = 500;
constexpr int MaxNameLength = 120;
constexpr int ProgressEmitIntervalMs = 250;

QString sanitise(QString name)
{
    static const QRegularExpression Invalid{QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])")};
    name.replace(Invalid, u"_"_s);
    return name.trimmed().left(MaxNameLength);
}

QString withIdSuffix(const QString& path, const QString& id)
{
    const QFileInfo info{path};
    return info.path() + u"/"_s + info.completeBaseName() + u" ["_s + id + u"]"_s
           + (info.suffix().isEmpty() ? QString{} : u"."_s + info.suffix());
}
} // namespace

SubsonicLibraryDownloader::SubsonicLibraryDownloader(std::shared_ptr<NetworkAccessManager> network,
                                                     SettingsManager* settings,
                                                     std::shared_ptr<AudioLoader> audioLoader,
                                                     MusicLibrary* library, QObject* parent)
    : QObject{parent}
    , m_network{std::move(network)}
    , m_audioLoader{std::move(audioLoader)}
    , m_settings{settings}
    , m_library{library}
    , m_client{new SubsonicClient{m_network, loadSubsonicSettings(settings), this}}
{
}

SubsonicLibraryDownloader::~SubsonicLibraryDownloader()
{
    if(m_file) {
        m_file->close();
        delete m_file;
    }
}

bool SubsonicLibraryDownloader::isActive() const
{
    return m_active || !m_queue.isEmpty();
}

int SubsonicLibraryDownloader::total() const
{
    return m_total;
}

int SubsonicLibraryDownloader::completed() const
{
    return m_processed - m_failed;
}

void SubsonicLibraryDownloader::enqueue(const QList<QJsonObject>& songs)
{
    if(songs.isEmpty()) {
        return;
    }

    if(!m_active) {
        m_total     = 0;
        m_processed = 0;
        m_failed    = 0;
        m_scanned.clear();
    }

    m_dir = loadSubsonicSettings(m_settings).libraryDir;
    QDir{}.mkpath(m_dir);

    if(m_library && !m_library->libraryForPath(m_dir)) {
        const QString message
            = tr("'%1' is not inside a fooyin music library folder, so downloads won't appear in the "
                 "library. Add it under Settings \u2192 Library.")
                  .arg(m_dir);
        qCWarning(SUBSONIC) << message;
        Q_EMIT warning(message);
    }

    QSet<QString> reserved;
    for(const QJsonObject& song : songs) {
        ++m_total;

        QString path = pathForSong(song, m_dir);
        // Different songs can map to the same name (multi-disc, duplicate titles); keep them apart.
        if(reserved.contains(path)
           || std::any_of(m_queue.cbegin(), m_queue.cend(),
                          [&path](const Job& job) { return job.path == path; })) {
            path = withIdSuffix(path, song.value(u"id"_s).toString());
        }

        if(QFileInfo::exists(path)) {
            // Already downloaded; scan it so it still lands in the library.
            ++m_processed;
            m_scanned.push_back(QUrl::fromLocalFile(path));
            Q_EMIT downloaded(song.value(u"id"_s).toString(), song, path);
            continue;
        }

        reserved.insert(path);
        m_queue.push_back({song, path});
    }

    if(!m_active) {
        processNext();
    }
}

void SubsonicLibraryDownloader::cancel()
{
    if(!m_active) {
        return;
    }

    m_cancel = true;
    m_queue.clear();
    if(m_reply) {
        m_reply->abort(); // handled in handleFinished()
    }
    // Otherwise we're between jobs or fetching the cover; the current step will stop in processNext().
}

void SubsonicLibraryDownloader::processNext()
{
    if(m_cancel) {
        m_cancel = false;
        m_active = false;
        if(m_file) {
            // Interrupted mid-download (e.g. during a retry delay); drop the partial file.
            m_file->close();
            delete m_file;
            m_file = nullptr;
            QFile::remove(m_current.path);
        }
        Q_EMIT finished(completed(), m_failed);
        return;
    }

    if(m_queue.isEmpty()) {
        m_active = false;
        if(m_library && !m_scanned.isEmpty()) {
            m_library->scanFiles(m_scanned);
        }
        Q_EMIT finished(completed(), m_failed);
        m_scanned.clear();
        return;
    }

    m_active = true;
    m_current = m_queue.takeFirst();
    m_attempts     = 0;
    m_received     = 0;
    m_requestStart = 0;
    m_totalBytes   = -1;
    m_rangeChecked = false;
    m_sentRange    = false;

    QDir{}.mkpath(QFileInfo{m_current.path}.path());

    if(m_file) {
        m_file->close();
        delete m_file;
    }
    m_file = new QFile{m_current.path};
    if(!m_file->open(QIODevice::WriteOnly)) {
        qCWarning(SUBSONIC) << "Could not write library file" << m_current.path;
        advance(false);
        return;
    }

    qCInfo(SUBSONIC) << "Downloading library track" << m_current.path;
    m_lastProgressEmit = 0;
    emitProgress();
    startRequest();
}

void SubsonicLibraryDownloader::startRequest()
{
    if(m_cancel) {
        processNext(); // emits finished() and resets state
        return;
    }
    if(!m_file) {
        return;
    }

    m_requestStart = m_received;
    m_rangeChecked = false;
    m_sentRange    = m_received > 0;

    m_client->setSettings(loadSubsonicSettings(m_settings));

    QNetworkRequest request{m_client->streamUrl(m_current.song.value(u"id"_s).toString())};
    if(m_sentRange) {
        request.setRawHeader("Range", "bytes=" + QByteArray::number(m_received) + "-");
    }

    m_reply = m_network->get(request);
    connect(m_reply, &QIODevice::readyRead, this, &SubsonicLibraryDownloader::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &SubsonicLibraryDownloader::handleFinished);
}

void SubsonicLibraryDownloader::handleReadyRead()
{
    if(!m_reply || sender() != m_reply) {
        return;
    }

    // If we asked to resume but the server ignored the Range header, restart the file from scratch.
    if(m_sentRange && !m_rangeChecked) {
        m_rangeChecked = true;
        if(m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            if(m_file) {
                m_file->resize(0);
                m_file->seek(0);
            }
            m_received     = 0;
            m_requestStart = 0;
            m_totalBytes   = -1;
        }
    }

    if(m_totalBytes < 0) {
        const QByteArray contentRange = m_reply->rawHeader("Content-Range");
        const int slash               = contentRange.lastIndexOf('/');
        if(slash >= 0) {
            bool ok{false};
            const qint64 total = contentRange.mid(slash + 1).toLongLong(&ok);
            if(ok) {
                m_totalBytes = total;
            }
        }
        if(m_totalBytes < 0) {
            bool ok{false};
            const qint64 length = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
            if(ok) {
                m_totalBytes = m_requestStart + length;
            }
        }
    }

    const QByteArray data = m_reply->readAll();
    if(data.isEmpty() || !m_file) {
        return;
    }

    if(m_file->write(data) != data.size()) {
        qCWarning(SUBSONIC) << "Failed to write to" << m_file->fileName() << "(disk full?)";
        m_cancel = true;
        m_reply->abort();
        return;
    }
    m_received += data.size();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if(now - m_lastProgressEmit >= ProgressEmitIntervalMs) {
        emitProgress();
    }
}

void SubsonicLibraryDownloader::handleFinished()
{
    if(sender() != m_reply) {
        return;
    }

    QNetworkReply* reply = m_reply;
    m_reply              = nullptr;
    if(!reply) {
        return;
    }

    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString               = reply->errorString();
    reply->deleteLater();

    if(m_cancel) {
        m_cancel = false;
        m_active = false;
        if(m_file) {
            m_file->close();
            delete m_file;
            m_file = nullptr;
            QFile::remove(m_current.path);
        }
        Q_EMIT finished(completed(), m_failed);
        return;
    }

    const bool complete
        = (error == QNetworkReply::NoError) && (m_totalBytes < 0 ? m_received > 0 : m_received >= m_totalBytes);
    if(complete) {
        finalizeJob();
        return;
    }

    if(m_attempts < MaxAttempts) {
        ++m_attempts;
        qCWarning(SUBSONIC) << "Library download retry" << m_attempts << "at" << m_received << "bytes:"
                            << (error == QNetworkReply::NoError ? u"incomplete"_s : errorString);
        QTimer::singleShot(RetryDelayMs, this, &SubsonicLibraryDownloader::startRequest);
        return;
    }

    qCWarning(SUBSONIC) << "Library download failed:" << errorString;
    if(m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    QFile::remove(m_current.path);
    advance(false);
}

void SubsonicLibraryDownloader::finalizeJob()
{
    if(m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }

    qCInfo(SUBSONIC) << "Saved library track" << m_current.path << m_received << "bytes";
    writeTags(m_current);
    embedCover();
}

void SubsonicLibraryDownloader::writeTags(const Job& job) const
{
    if(!m_audioLoader) {
        return;
    }

    // A raw/lossless original already carries the server's tags; leave them untouched so nothing is
    // lost. Only tag-less files (e.g. a server-side transcode) are written from the Subsonic metadata.
    Track existing{job.path};
    if(m_audioLoader->readTrackMetadata(existing) && !existing.title().isEmpty()) {
        return;
    }

    const Track track = trackFromSong(job.song, job.path);
    if(!m_audioLoader->writeTrackMetadata(track, AudioReader::Metadata)) {
        qCWarning(SUBSONIC) << "Could not write tags to" << job.path;
    }
}

void SubsonicLibraryDownloader::embedCover()
{
    const QString coverArt = m_current.song.value(u"coverArt"_s).toString();
    if(coverArt.isEmpty() || !m_audioLoader) {
        advance(true);
        return;
    }

    m_client->setSettings(loadSubsonicSettings(m_settings));
    QNetworkReply* reply = m_network->get(QNetworkRequest{m_client->coverArtUrl(coverArt, CoverSize)});
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();

        if(!m_cancel && reply->error() == QNetworkReply::NoError) {
            const QByteArray data = reply->readAll();
            if(!data.isEmpty()) {
                const QString mimeType
                    = imageMimeType(data, reply->header(QNetworkRequest::ContentTypeHeader).toString());
                if(mimeType.isEmpty()) {
                    qCWarning(SUBSONIC) << "Cover art for" << m_current.path << "is not an image; skipping";
                }
                else {
                    const Track track = trackFromSong(m_current.song, m_current.path);
                    // Keep any cover the file already has.
                    if(m_audioLoader->readTrackCover(track, Track::Cover::Front).isEmpty()) {
                        TrackCovers covers;
                        covers[Track::Cover::Front] = CoverImage{mimeType, data};
                        if(!m_audioLoader->writeTrackCover(track, covers, AudioReader::Metadata)) {
                            qCWarning(SUBSONIC) << "Could not embed cover in" << m_current.path;
                        }
                    }
                }
            }
        }

        advance(true);
    });
}

void SubsonicLibraryDownloader::advance(bool ok)
{
    ++m_processed;
    if(ok) {
        m_scanned.push_back(QUrl::fromLocalFile(m_current.path));
        Q_EMIT downloaded(m_current.song.value(u"id"_s).toString(), m_current.song, m_current.path);
    }
    else {
        ++m_failed;
    }
    processNext();
}

void SubsonicLibraryDownloader::emitProgress()
{
    const int percent
        = m_totalBytes > 0
              ? static_cast<int>(std::clamp((m_received * 100) / m_totalBytes, qint64{0}, qint64{100}))
              : -1;
    m_lastProgressEmit = QDateTime::currentMSecsSinceEpoch();
    Q_EMIT progress(completed(), m_total, percent, m_current.song.value(u"title"_s).toString());
}

QString SubsonicLibraryDownloader::existingLocalPath(const QJsonObject& song) const
{
    const QString base = pathForSong(song, loadSubsonicSettings(m_settings).libraryDir);
    if(QFileInfo::exists(base)) {
        return base;
    }
    const QString withId = withIdSuffix(base, song.value(u"id"_s).toString());
    return QFileInfo::exists(withId) ? withId : QString{};
}

QString SubsonicLibraryDownloader::pathForSong(const QJsonObject& song, const QString& dir) const
{
    QString albumArtist = song.value(u"albumArtist"_s).toString();
    if(albumArtist.isEmpty()) {
        albumArtist = song.value(u"artist"_s).toString();
    }
    if(albumArtist.isEmpty()) {
        albumArtist = u"Unknown Artist"_s;
    }

    QString album = song.value(u"album"_s).toString();
    if(album.isEmpty()) {
        album = u"Unknown Album"_s;
    }

    QString title = song.value(u"title"_s).toString();
    if(title.isEmpty()) {
        title = song.value(u"id"_s).toString();
    }

    const int trackNumber = song.value(u"track"_s).toInt();
    if(trackNumber > 0) {
        title = QStringLiteral("%1 - %2").arg(trackNumber, 2, 10, QLatin1Char('0')).arg(title);
    }

    QString suffix = song.value(u"suffix"_s).toString();
    if(suffix.isEmpty()) {
        suffix = u"bin"_s;
    }

    return dir + u"/"_s + sanitise(albumArtist) + u"/"_s + sanitise(album) + u"/"_s + sanitise(title) + u"."_s
           + suffix;
}
} // namespace Fooyin::Subsonic
