/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <mutex>

namespace Fooyin::Subsonic {
//! True for a http(s) URL pointing at the Subsonic `stream` endpoint with an id.
inline bool isSubsonicStreamUrl(const QUrl& url)
{
    if(url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) {
        return false;
    }
    return url.path().endsWith(QStringLiteral("/rest/stream"), Qt::CaseInsensitive)
        && !QUrlQuery{url}.queryItemValue(QStringLiteral("id")).isEmpty();
}

struct ContentRange
{
    qint64 start{-1};
    qint64 total{-1};
};

//! Parses a `Content-Range: bytes 0-123/456` header (`*` total yields -1).
inline ContentRange parseContentRange(const QByteArray& header)
{
    ContentRange range;
    const qsizetype slash = header.indexOf('/');
    if(slash < 0) {
        return range;
    }

    const QByteArray total = header.mid(slash + 1).trimmed();
    if(total != "*") {
        bool totalOk{false};
        range.total = total.toLongLong(&totalOk);
        if(!totalOk) {
            range.total = -1;
        }
    }

    const qsizetype space = header.indexOf(' ');
    const qsizetype dash  = space >= 0 ? header.indexOf('-', space + 1) : -1;
    if(dash > space) {
        bool startOk{false};
        range.start = header.mid(space + 1, dash - space - 1).trimmed().toLongLong(&startOk);
        if(!startOk) {
            range.start = -1;
        }
    }

    return range;
}

//! Extracts the Subsonic song id from a cache filename like `Artist - Title [tr-4802].flac`.
inline QString songIdFromCachePath(const QString& path)
{
    static const QRegularExpression CacheId{QStringLiteral(R"(\[([^\[\]]+)\]\.[^./\\]+$)")};
    const QRegularExpressionMatch match = CacheId.match(path);
    return match.hasMatch() ? match.captured(1) : QString{};
}

//! Builds the cache file name (`Artist - Title [id].suffix`) for a Subsonic song object.
inline QString subsonicCacheFileName(const QJsonObject& song)
{
    const QString id = song.value(QStringLiteral("id")).toString();

    QString name = song.value(QStringLiteral("artist")).toString() + QStringLiteral(" - ")
        + song.value(QStringLiteral("title")).toString();
    name = name.trimmed();
    if(name.isEmpty()) {
        name = id;
    }

    static const QRegularExpression Invalid{QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])")};
    name.replace(Invalid, QStringLiteral("_"));
    name = name.left(180);

    QString suffix = song.value(QStringLiteral("suffix")).toString();
    if(suffix.isEmpty()) {
        suffix = QStringLiteral("bin");
    }

    return name + QStringLiteral(" [") + id + QStringLiteral("].") + suffix;
}

//! Returns the image mime type for @p data, or an empty string if it isn't an image. Some servers
//! answer a cover request with an error envelope (JSON/HTML); embedding that as "art" corrupts files.
inline QString imageMimeType(const QByteArray& data, const QString& reported)
{
    if(reported.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive)) {
        return reported;
    }
    if(data.startsWith(QByteArrayLiteral("\xFF\xD8\xFF"))) {
        return QStringLiteral("image/jpeg");
    }
    if(data.startsWith(QByteArrayLiteral("\x89PNG\r\n\x1a\n"))) {
        return QStringLiteral("image/png");
    }
    if(data.startsWith(QByteArrayLiteral("GIF8"))) {
        return QStringLiteral("image/gif");
    }
    if(data.startsWith(QByteArrayLiteral("RIFF")) && data.mid(8, 4) == QByteArrayLiteral("WEBP")) {
        return QStringLiteral("image/webp");
    }
    return {};
}

//! Thread-safe map from Subsonic song id to a completed cache file path. The download manager
//! publishes entries here; the stream device consults it so a cached track can be served from disk.
class SubsonicCacheRegistry
{
public:
    void set(const QString& songId, const QString& path)
    {
        const std::scoped_lock lock{m_mutex};
        m_paths.insert(songId, path);
    }

    void remove(const QString& songId)
    {
        const std::scoped_lock lock{m_mutex};
        m_paths.remove(songId);
    }

    [[nodiscard]] QString pathFor(const QString& songId) const
    {
        const std::scoped_lock lock{m_mutex};
        return m_paths.value(songId);
    }

private:
    mutable std::mutex m_mutex;
    QHash<QString, QString> m_paths;
};
} // namespace Fooyin::Subsonic
