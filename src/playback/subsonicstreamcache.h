/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QStandardPaths>
#include <QString>

#include <algorithm>
#include <optional>
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

    [[nodiscard]] bool coversAll(qint64 total) const
    {
        return total > 0 && !firstGap(total).has_value();
    }

    [[nodiscard]] const std::vector<ByteRange>& ranges() const
    {
        return m_ranges;
    }

private:
    std::vector<ByteRange> m_ranges;
};

//! Directory for the temporary whole-track stream caches.
inline QString streamCacheDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if(base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    return base + QStringLiteral("/foonic/stream-cache");
}
} // namespace Fooyin::Subsonic
