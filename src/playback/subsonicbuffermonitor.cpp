/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicbuffermonitor.h"

#include <algorithm>
#include <cmath>

namespace Fooyin::Subsonic {
namespace {
QVector<BufferRange> sanitiseRanges(const QVector<BufferRange>& ranges)
{
    QVector<BufferRange> result;
    result.reserve(ranges.size());
    for(const BufferRange& range : ranges) {
        const double start = std::clamp(range.start, 0.0, 1.0);
        const double end   = std::clamp(range.end, 0.0, 1.0);
        if(end > start) {
            result.append(BufferRange{start, end});
        }
    }
    return result;
}
} // namespace

SubsonicBufferMonitor::SubsonicBufferMonitor(QObject* parent)
    : QObject{parent}
{ }

void SubsonicBufferMonitor::setRange(double start, double end)
{
    if(end <= start) {
        reset();
        return;
    }
    setRanges(QVector<BufferRange>{BufferRange{start, end}});
}

void SubsonicBufferMonitor::setRanges(const QVector<BufferRange>& ranges)
{
    const QVector<BufferRange> clean = sanitiseRanges(ranges);
    if(clean.size() == m_ranges.size()) {
        bool same{true};
        for(qsizetype i{0}; i < clean.size(); ++i) {
            if(std::abs(clean.at(i).start - m_ranges.at(i).start) > 0.000001
               || std::abs(clean.at(i).end - m_ranges.at(i).end) > 0.000001) {
                same = false;
                break;
            }
        }
        if(same) {
            return;
        }
    }
    if(clean.isEmpty() && m_ranges.isEmpty()) {
        return;
    }

    m_ranges = clean;
    Q_EMIT rangeChanged();
}

void SubsonicBufferMonitor::reset()
{
    if(m_ranges.isEmpty()) {
        return;
    }
    m_ranges.clear();
    Q_EMIT rangeChanged();
}

QVector<BufferRange> SubsonicBufferMonitor::ranges() const
{
    return m_ranges;
}

bool SubsonicBufferMonitor::isComplete() const
{
    return m_ranges.size() == 1 && m_ranges.first().start <= 0.000001
        && m_ranges.first().end >= 0.999999;
}
} // namespace Fooyin::Subsonic
