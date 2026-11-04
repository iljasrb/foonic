/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QPixmap>
#include <QStandardPaths>
#include <QString>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
//! High-resolution copy used by the details panel.
inline constexpr int CoverCacheSize = 1000;
//! Small copy used by the browser tree and album grid.
inline constexpr int GridCoverSize = 300;

inline QString coverCachePath(const QString& coverArtId, int size = CoverCacheSize)
{
    // v3 stores PNGs at the requested size. Grid and details covers are separate files, so a
    // 300px grid cover never replaces a 1000px details cover.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/foonic/covers-v3");
    QDir{}.mkpath(dir);
    const QByteArray key = QCryptographicHash::hash(coverArtId.toUtf8(), QCryptographicHash::Sha1).toHex();
    return dir + u'/' + QString::fromLatin1(key) + u'-' + QString::number(size) + u".png"_s;
}

inline QPixmap loadCachedCover(const QString& coverArtId, int size = CoverCacheSize)
{
    QPixmap cover;
    if(!coverArtId.isEmpty()) {
        cover.load(coverCachePath(coverArtId, size));
    }
    return cover;
}

inline void saveCachedCover(const QString& coverArtId, const QImage& cover, int size = CoverCacheSize)
{
    if(!coverArtId.isEmpty() && !cover.isNull()) {
        cover.save(coverCachePath(coverArtId, size), "PNG");
    }
}

inline void saveCachedCover(const QString& coverArtId, const QPixmap& cover, int size = CoverCacheSize)
{
    saveCachedCover(coverArtId, cover.toImage(), size);
}
} // namespace Fooyin::Subsonic
