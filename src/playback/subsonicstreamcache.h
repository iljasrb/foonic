/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Fooyin::Subsonic {
//! Half-open byte interval: [start, end).
struct ByteRange
{
    qint64 start{0};
    qint64 end{0};
};

/*!
 * Sorted, merged set of downloaded byte ranges.
 *
 * The stream device keeps the whole current track in a disk cache. Unlike a single in-memory
 * window, this retains everything that has been downloaded, so seeking backwards (or into any
 * other completed range) never needs another request. Holes are filled in the background.
 */
class RangeSet
{
public:
    void clear()
    {
        m_ranges.clear();
    }

    //! Adds [start, end), merging overlapping and adjacent intervals.
    void add(qint64 start, qint64 end)
    {
        if(end <= start) {
            return;
        }

        auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), start,
                                   [](const ByteRange& range, qint64 value) { return range.end < value; });

        while(it != m_ranges.end() && it->start <= end) {
            start = std::min(start, it->start);
            end   = std::max(end, it->end);
            it    = m_ranges.erase(it);
        }
        m_ranges.insert(it, ByteRange{start, end});
    }

    //! Drops bytes at or beyond @p total, e.g. when an authoritative size replaces an estimate.
    void trim(qint64 total)
    {
        if(total < 0) {
            return;
        }

        while(!m_ranges.empty() && m_ranges.back().start >= total) {
            m_ranges.pop_back();
        }
        if(!m_ranges.empty() && m_ranges.back().end > total) {
            m_ranges.back().end = total;
        }
    }

    [[nodiscard]] bool contains(qint64 pos) const
    {
        return rangeEnd(pos) > pos;
    }

    //! End of the range containing @p pos, or -1 when the position is not cached.
    [[nodiscard]] qint64 rangeEnd(qint64 pos) const
    {
        auto it = std::upper_bound(m_ranges.begin(), m_ranges.end(), pos,
                                   [](qint64 value, const ByteRange& range) { return value < range.start; });
        if(it == m_ranges.begin()) {
            return -1;
        }
        --it;
        return pos >= it->start && pos < it->end ? it->end : -1;
    }

    //! Number of bytes cached from @p pos until the next hole.
    [[nodiscard]] qint64 contiguousBytes(qint64 pos) const
    {
        const qint64 end = rangeEnd(pos);
        return end > pos ? end - pos : 0;
    }

    //! First not-yet-downloaded interval below @p total, or nothing when the file is complete.
    [[nodiscard]] std::optional<ByteRange> firstGap(qint64 total) const
    {
        if(total <= 0) {
            return std::nullopt;
        }

        qint64 pos = 0;
        for(const ByteRange& range : m_ranges) {
            if(range.start > pos) {
                return ByteRange{pos, std::min(range.start, total)};
            }
            pos = std::max(pos, range.end);
            if(pos >= total) {
                return std::nullopt;
            }
        }
        if(pos < total) {
            return ByteRange{pos, total};
        }
        return std::nullopt;
    }

    //! First not-yet-downloaded interval at or after @p from, below @p total.
    [[nodiscard]] std::optional<ByteRange> firstGapFrom(qint64 from, qint64 total) const
    {
        if(total <= 0) {
            return std::nullopt;
        }

        qint64 pos = std::clamp<qint64>(from, 0, total);
        for(const ByteRange& range : m_ranges) {
            if(range.end <= pos) {
                continue;
            }
            if(range.start > pos) {
                return ByteRange{pos, std::min(range.start, total)};
            }
            pos = std::max(pos, range.end);
            if(pos >= total) {
                return std::nullopt;
            }
        }
        if(pos < total) {
            return ByteRange{pos, total};
        }
        return std::nullopt;
    }

    [[nodiscard]] bool coversAll(qint64 total) const
    {
        return total >= 0 && !firstGap(total).has_value();
    }

    [[nodiscard]] const std::vector<ByteRange>& ranges() const
    {
        return m_ranges;
    }

private:
    std::vector<ByteRange> m_ranges;
};

inline qint64 cacheMonotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline QString streamCacheBaseDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if(base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    return base + QStringLiteral("/foonic");
}

inline QString streamCacheKey(const QString& songId)
{
    return QString::fromLatin1(QCryptographicHash::hash(songId.toUtf8(), QCryptographicHash::Sha1).toHex());
}

/*!
 * One track's bytes on disk plus the ranges that are known to be present. The entry is shared by
 * the live stream device and the next-track prefetcher, so both fill the same file.
 */
class CacheEntry
{
public:
    CacheEntry(QString songId, QString path)
        : m_songId{std::move(songId)}
        , m_path{std::move(path)}
        , m_lastUsedMs{cacheMonotonicMs()}
    { }

    ~CacheEntry()
    {
        removeFile();
    }

    CacheEntry(const CacheEntry&)            = delete;
    CacheEntry& operator=(const CacheEntry&) = delete;

    [[nodiscard]] const QString& songId() const
    {
        return m_songId;
    }

    [[nodiscard]] QString path() const
    {
        return m_path;
    }

    [[nodiscard]] qint64 total() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_total;
    }

    //! Sets an authoritative size and trims anything beyond it.
    void setTotal(qint64 total)
    {
        if(total < 0) {
            return;
        }
        const std::scoped_lock lock{m_mutex};
        m_total = total;
        m_ranges.trim(total);
        touchLocked();
    }

    //! Sets a size from estimateContentLength without shrinking or trimming known data.
    void setEstimatedTotal(qint64 total)
    {
        if(total < 0) {
            return;
        }
        const std::scoped_lock lock{m_mutex};
        if(m_total < 0 || total > m_total) {
            m_total = total;
        }
        touchLocked();
    }

    [[nodiscard]] bool complete() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_total >= 0 && (m_total == 0 || m_ranges.coversAll(m_total));
    }

    [[nodiscard]] bool contains(qint64 pos) const
    {
        const std::scoped_lock lock{m_mutex};
        return m_ranges.contains(pos);
    }

    [[nodiscard]] qint64 rangeEnd(qint64 pos) const
    {
        const std::scoped_lock lock{m_mutex};
        return m_ranges.rangeEnd(pos);
    }

    [[nodiscard]] qint64 contiguousBytes(qint64 pos) const
    {
        const std::scoped_lock lock{m_mutex};
        return m_ranges.contiguousBytes(pos);
    }

    [[nodiscard]] std::optional<ByteRange> firstGap() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_ranges.firstGap(m_total);
    }

    [[nodiscard]] std::optional<ByteRange> firstGapFrom(qint64 from) const
    {
        const std::scoped_lock lock{m_mutex};
        return m_ranges.firstGapFrom(from, m_total);
    }

    [[nodiscard]] std::vector<ByteRange> rangeList() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_ranges.ranges();
    }

    [[nodiscard]] qint64 lastUsedMs() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_lastUsedMs;
    }

    void touch()
    {
        const std::scoped_lock lock{m_mutex};
        touchLocked();
    }

    [[nodiscard]] QString errorString() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_error;
    }

    //! Reads bytes already present in the cache. Returns -1 on an I/O error, 0 when not cached.
    qint64 readAt(char* data, qint64 maxSize, qint64 pos)
    {
        const std::scoped_lock lock{m_mutex};
        if(!ensureOpenLocked()) {
            return -1;
        }
        if(!m_ranges.contains(pos)) {
            return 0;
        }
        if(!m_file.seek(pos)) {
            m_error = QStringLiteral("Could not seek the stream cache: %1").arg(m_file.errorString());
            return -1;
        }
        const qint64 read = m_file.read(data, maxSize);
        if(read < 0) {
            m_error = QStringLiteral("Could not read the stream cache: %1").arg(m_file.errorString());
        }
        else {
            m_error.clear();
        }
        touchLocked();
        return read;
    }

    //! Writes bytes at @p pos and records the covered range. Returns -1 on an I/O error.
    qint64 writeAt(qint64 pos, const char* data, qint64 size)
    {
        if(pos < 0 || size <= 0) {
            return 0;
        }

        const std::scoped_lock lock{m_mutex};
        if(!ensureOpenLocked()) {
            return -1;
        }
        if(!m_file.seek(pos)) {
            m_error = QStringLiteral("Could not seek the stream cache: %1").arg(m_file.errorString());
            return -1;
        }

        qint64 written{0};
        while(written < size) {
            const qint64 count = m_file.write(data + written, size - written);
            if(count <= 0) {
                m_error = QStringLiteral("Could not write the stream cache: %1").arg(m_file.errorString());
                return -1;
            }
            written += count;
        }

        m_ranges.add(pos, pos + written);
        m_error.clear();
        touchLocked();
        return written;
    }

    [[nodiscard]] qint64 sizeOnDisk() const
    {
        const std::scoped_lock lock{m_mutex};
        if(m_file.isOpen()) {
            return m_file.size();
        }
        return QFileInfo{m_path}.size();
    }

    void removeFile()
    {
        const std::scoped_lock lock{m_mutex};
        if(m_file.isOpen()) {
            m_file.close();
        }
        if(!m_path.isEmpty()) {
            QFile::remove(m_path);
        }
        m_ranges.clear();
        m_total = -1;
    }

private:
    //! Caller must hold @ref m_mutex.
    bool ensureOpenLocked()
    {
        if(m_file.isOpen()) {
            return true;
        }
        if(m_path.isEmpty()) {
            m_error = QStringLiteral("The stream cache has no file path.");
            return false;
        }

        QDir{}.mkpath(QFileInfo{m_path}.path());
        m_file.setFileName(m_path);
        if(!m_file.open(QIODevice::ReadWrite)) {
            m_error = QStringLiteral("Could not open the stream cache: %1").arg(m_file.errorString());
            return false;
        }
        return true;
    }

    //! Caller must hold @ref m_mutex.
    void touchLocked()
    {
        m_lastUsedMs = cacheMonotonicMs();
    }

    QString m_songId;
    QString m_path;
    mutable std::mutex m_mutex;
    QFile m_file;
    RangeSet m_ranges;
    qint64 m_total{-1};
    qint64 m_lastUsedMs{0};
    QString m_error;
};

/*!
 * Session-only cache for remote track bytes.
 *
 * Files live in a unique directory and are removed when the plugin shuts down. The store can
 * evict completed/unused entries when the configured size limit is exceeded; entries held by a
 * stream device or the prefetcher are never evicted, and pinned current/next tracks are kept.
 */
class SessionCache
{
public:
    SessionCache(QString baseDir, qint64 limitBytes)
        : m_baseDir{std::move(baseDir)}
        , m_limitBytes{std::max<qint64>(0, limitBytes)}
    {
        removeStaleCaches(m_baseDir);
        QDir{}.mkpath(m_baseDir);
        m_dir = m_baseDir + QStringLiteral("/session-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir{}.mkpath(m_dir);
    }

    ~SessionCache()
    {
        {
            const std::scoped_lock lock{m_mutex};
            for(const auto& entry : std::as_const(m_entries)) {
                entry->removeFile();
            }
            m_entries.clear();
        }
        if(!m_dir.isEmpty()) {
            QDir{m_dir}.removeRecursively();
        }
    }

    SessionCache(const SessionCache&)            = delete;
    SessionCache& operator=(const SessionCache&) = delete;

    [[nodiscard]] QString directory() const
    {
        return m_dir;
    }

    //! Returns the entry for @p songId, creating an empty one if needed.
    [[nodiscard]] std::shared_ptr<CacheEntry> entry(const QString& songId)
    {
        if(songId.isEmpty()) {
            return {};
        }

        const std::scoped_lock lock{m_mutex};
        if(const auto it = m_entries.constFind(songId); it != m_entries.constEnd()) {
            it.value()->touch();
            return it.value();
        }

        const QString path = m_dir + u'/' + streamCacheKey(songId) + QStringLiteral(".cache");
        auto cacheEntry    = std::make_shared<CacheEntry>(songId, path);
        m_entries.insert(songId, cacheEntry);
        return cacheEntry;
    }

    [[nodiscard]] std::shared_ptr<CacheEntry> find(const QString& songId) const
    {
        const std::scoped_lock lock{m_mutex};
        return m_entries.value(songId);
    }

    void pin(const QString& songId)
    {
        if(songId.isEmpty()) {
            return;
        }
        const std::scoped_lock lock{m_mutex};
        m_pinned.insert(songId);
    }

    void unpin(const QString& songId)
    {
        if(songId.isEmpty()) {
            return;
        }
        const std::scoped_lock lock{m_mutex};
        m_pinned.remove(songId);
    }

    void setLimitBytes(qint64 limitBytes)
    {
        {
            const std::scoped_lock lock{m_mutex};
            m_limitBytes = std::max<qint64>(0, limitBytes);
        }
        evict();
    }

    [[nodiscard]] qint64 limitBytes() const
    {
        const std::scoped_lock lock{m_mutex};
        return m_limitBytes;
    }

    //! Removes an entry when nothing else is using it. Returns true when the entry was removed.
    bool discard(const QString& songId)
    {
        const std::scoped_lock lock{m_mutex};
        const auto it = m_entries.find(songId);
        if(it == m_entries.end() || it.value().use_count() > 1 || m_pinned.contains(songId)) {
            return false;
        }
        it.value()->removeFile();
        m_entries.erase(it);
        return true;
    }

    //! Drops an incomplete, unused entry (e.g. a cancelled prefetch). Returns true when removed.
    bool discardIncomplete(const QString& songId)
    {
        const std::scoped_lock lock{m_mutex};
        const auto it = m_entries.find(songId);
        if(it == m_entries.end() || it.value().use_count() > 1 || m_pinned.contains(songId)
           || it.value()->complete()) {
            return false;
        }
        it.value()->removeFile();
        m_entries.erase(it);
        return true;
    }

    void evict()
    {
        const std::scoped_lock lock{m_mutex};
        if(m_limitBytes <= 0) {
            return;
        }

        while(true) {
            qint64 totalBytes{0};
            for(const auto& entry : std::as_const(m_entries)) {
                totalBytes += entry->sizeOnDisk();
            }
            if(totalBytes <= m_limitBytes) {
                return;
            }

            QString candidate;
            qint64 candidateUsed{cacheMonotonicMs()};
            for(auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
                auto& entry = it.value();
                if(m_pinned.contains(it.key()) || entry.use_count() > 1) {
                    continue;
                }
                const qint64 used = entry->lastUsedMs();
                if(candidate.isEmpty() || used < candidateUsed) {
                    candidate     = it.key();
                    candidateUsed = used;
                }
            }
            if(candidate.isEmpty()) {
                return;
            }

            const auto it = m_entries.find(candidate);
            if(it == m_entries.end()) {
                return;
            }
            it.value()->removeFile();
            m_entries.erase(it);
        }
    }

    [[nodiscard]] qint64 totalSizeBytes() const
    {
        const std::scoped_lock lock{m_mutex};
        qint64 totalBytes{0};
        for(const auto& entry : std::as_const(m_entries)) {
            totalBytes += entry->sizeOnDisk();
        }
        return totalBytes;
    }

    [[nodiscard]] int entryCount() const
    {
        const std::scoped_lock lock{m_mutex};
        return static_cast<int>(m_entries.size());
    }

private:
    //! Removes session directories left behind by an earlier crash.
    static void removeStaleCaches(const QString& baseDir)
    {
        const QDir dir{baseDir};
        if(!dir.exists()) {
            return;
        }

        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
        const QFileInfoList entries
            = dir.entryInfoList({QStringLiteral("session-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        for(const QFileInfo& entry : entries) {
            if(entry.lastModified().toUTC() < cutoff) {
                QDir{entry.absoluteFilePath()}.removeRecursively();
            }
        }
    }

    QString m_baseDir;
    QString m_dir;
    mutable std::mutex m_mutex;
    QHash<QString, std::shared_ptr<CacheEntry>> m_entries;
    QSet<QString> m_pinned;
    qint64 m_limitBytes{0};
};
} // namespace Fooyin::Subsonic
