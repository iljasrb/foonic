/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class QFile;

namespace Fooyin {
class AudioLoader;
class MusicLibrary;
class NetworkAccessManager;
class SettingsManager;

namespace Subsonic {
class SubsonicClient;

/*!
 * Downloads Subsonic songs into a local music library folder as normal audio files
 * (Artist/Album/NN - Title.ext), writing their tags and embedding the cover art, then
 * asks the music library to scan them so they show up like locally added tracks.
 *
 * This is separate from the playback cache: library files are never evicted and the
 * downloads don't compete with playback prefetch.
 */
class SubsonicLibraryDownloader : public QObject
{
    Q_OBJECT

public:
    SubsonicLibraryDownloader(std::shared_ptr<NetworkAccessManager> network, SettingsManager* settings,
                              std::shared_ptr<AudioLoader> audioLoader, MusicLibrary* library,
                              QObject* parent = nullptr);
    ~SubsonicLibraryDownloader() override;

    /*! Queues @p songs to be saved into the configured library folder. */
    void enqueue(const QList<QJsonObject>& songs);
    void cancel();

    [[nodiscard]] bool isActive() const;
    [[nodiscard]] int total() const;
    [[nodiscard]] int completed() const;

Q_SIGNALS:
    void progress(int completed, int total, const QString& current);
    void finished(int completed, int failed);
    void warning(const QString& message);

private:
    struct Job
    {
        QJsonObject song;
        QString path;
    };

    void processNext();
    void startRequest();
    void handleReadyRead();
    void handleFinished();
    void finalizeJob();
    void embedCover();
    void advance(bool ok);
    void writeTags(const Job& job) const;
    [[nodiscard]] QString pathForSong(const QJsonObject& song) const;

    std::shared_ptr<NetworkAccessManager> m_network;
    std::shared_ptr<AudioLoader> m_audioLoader;
    SettingsManager* m_settings;
    MusicLibrary* m_library;
    SubsonicClient* m_client;

    QString m_dir;
    QList<Job> m_queue;
    QList<QUrl> m_scanned;
    Job m_current;
    int m_total{0};
    int m_processed{0};
    int m_failed{0};
    int m_attempts{0};
    bool m_active{false};
    bool m_cancel{false};

    QFile* m_file{nullptr};
    QNetworkReply* m_reply{nullptr};
    qint64 m_received{0};
    qint64 m_requestStart{0};
    qint64 m_totalBytes{-1};
    bool m_rangeChecked{false};
    bool m_sentRange{false};
};
} // namespace Subsonic
} // namespace Fooyin
