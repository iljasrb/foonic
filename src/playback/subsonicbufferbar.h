/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QVector>

class QEvent;
class QTimer;
class QWidget;

namespace Fooyin {
class SettingsManager;

namespace Subsonic {
//! Cached part of the current track, as fractions of the total duration.
struct BufferRange
{
    double start{0.0};
    double end{0.0};
};

/*!
 * Downloaded ranges of the current remote stream, as fractions of the track. Updated from the
 * decoder/network threads and read on the GUI thread.
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
    [[nodiscard]] double start() const;
    [[nodiscard]] double end() const;

Q_SIGNALS:
    void rangeChanged();

private:
    QVector<BufferRange> m_ranges;
};

class BufferOverlay;

/*!
 * Paints the cached ranges of the current Subsonic stream onto fooyin's seekbars.
 *
 * fooyin's SeekBar has no public buffer hook, so this follows the layout at runtime, overlays a
 * transparent, mouse-transparent child on each TrackSlider and draws muted bands over its groove.
 */
class SubsonicBufferBar : public QObject
{
    Q_OBJECT

public:
    SubsonicBufferBar(SettingsManager* settings, SubsonicBufferMonitor* monitor, QObject* parent = nullptr);
    ~SubsonicBufferBar() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] bool enabled() const;
    void scan();
    void updateOverlays();

    SettingsManager* m_settings;
    SubsonicBufferMonitor* m_monitor;
    QHash<QWidget*, BufferOverlay*> m_overlays;
    QTimer* m_scanTimer;
};
} // namespace Subsonic
} // namespace Fooyin
