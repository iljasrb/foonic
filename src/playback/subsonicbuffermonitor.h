/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QVector>

namespace Fooyin::Subsonic {
//! Cached part of the current track, as fractions of the total duration.
struct BufferRange
{
    double start{0.0};
    double end{0.0};
};

/*!
 * Downloaded ranges of the current remote stream, as fractions of the track. Updated from the
 * decoder/network threads and read on the GUI thread. Also tells the prefetcher when the current
 * track has enough buffered ahead.
 */
class SubsonicBufferMonitor : public QObject
{
    Q_OBJECT

public:
    explicit SubsonicBufferMonitor(QObject* parent = nullptr);

    void setRange(double start, double end);
    void setRanges(const QVector<BufferRange>& ranges);
    void reset();

    [[nodiscard]] QVector<BufferRange> ranges() const;
    [[nodiscard]] bool isComplete() const;

Q_SIGNALS:
    void rangeChanged();

private:
    QVector<BufferRange> m_ranges;
};
} // namespace Fooyin::Subsonic
