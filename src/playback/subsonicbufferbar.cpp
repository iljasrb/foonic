/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicbufferbar.h"

#include "subsonicsettings.h"

#include <utils/settings/settingsdialogcontroller.h>

#include <QAbstractSlider>
#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QWidget>

#include <QtAlgorithms>

#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace Fooyin::Subsonic {
namespace {
// QSlider::initStyleOption is protected; replicate what the style needs to place the groove/handle.
QStyleOptionSlider sliderOption(QSlider* slider)
{
    QStyleOptionSlider opt;
    opt.initFrom(slider);
    opt.subControls       = QStyle::SC_None;
    opt.activeSubControls = QStyle::SC_None;
    opt.orientation       = slider->orientation();
    opt.minimum           = slider->minimum();
    opt.maximum           = slider->maximum();
    opt.tickPosition      = slider->tickPosition();
    opt.tickInterval      = slider->tickInterval();
    opt.sliderPosition    = slider->sliderPosition();
    opt.sliderValue       = slider->value();
    opt.singleStep        = slider->singleStep();
    opt.pageStep          = slider->pageStep();
    opt.upsideDown
        = slider->orientation() == Qt::Horizontal ? slider->invertedAppearance() : !slider->invertedAppearance();
    return opt;
}

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

bool sameStarts(const QVector<BufferRange>& lhs, const QVector<BufferRange>& rhs)
{
    if(lhs.size() != rhs.size()) {
        return false;
    }
    for(qsizetype i{0}; i < lhs.size(); ++i) {
        if(std::abs(lhs.at(i).start - rhs.at(i).start) > 0.000001) {
            return false;
        }
    }
    return true;
}
} // namespace

class BufferOverlay : public QWidget
{
public:
    explicit BufferOverlay(QWidget* slider)
        : QWidget{slider}
        , m_animTimer{new QTimer{this}}
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);

        // Exponential catch-up: each frame closes a quarter of the gap, so chunked download updates
        // read as one smooth slide and the timer stops on its own once the bands have caught up.
        m_animTimer->setInterval(16);
        connect(m_animTimer, &QTimer::timeout, this, [this] {
            bool caughtUp{true};
            for(qsizetype i{0}; i < m_displayRanges.size() && i < m_targetRanges.size(); ++i) {
                const double diff = m_targetRanges.at(i).end - m_displayRanges.at(i).end;
                if(std::abs(diff) < 0.0005) {
                    m_displayRanges[i].end = m_targetRanges.at(i).end;
                }
                else {
                    m_displayRanges[i].end += diff * 0.25;
                    caughtUp = false;
                }
            }
            if(caughtUp) {
                m_animTimer->stop();
            }
            update();
        });
    }

    //! Feeds the cached ranges. Growth is animated; a changed range layout snaps.
    void setRanges(const QVector<BufferRange>& ranges)
    {
        const QVector<BufferRange> clean = sanitiseRanges(ranges);

        // A new hole or a merged range changes the layout; animating that would look like the band
        // jumping around, so draw the exact state instead.
        if(!sameStarts(m_displayRanges, clean) || m_displayRanges.size() != clean.size()) {
            m_targetRanges  = clean;
            m_displayRanges = clean;
            m_animTimer->stop();
            update();
            return;
        }

        m_targetRanges = clean;
        for(qsizetype i{0}; i < m_targetRanges.size(); ++i) {
            if(m_targetRanges.at(i).end < m_displayRanges.at(i).end) {
                m_displayRanges[i].end = m_targetRanges.at(i).end;
            }
        }
        if(!m_animTimer->isActive()) {
            for(qsizetype i{0}; i < m_targetRanges.size(); ++i) {
                if(m_targetRanges.at(i).end > m_displayRanges.at(i).end) {
                    m_animTimer->start();
                    break;
                }
            }
        }
        update();
    }

    //! Hides the bands without detaching the overlay (used by the settings toggle).
    void setBandEnabled(bool enabled)
    {
        if(m_bandEnabled == enabled) {
            return;
        }
        m_bandEnabled = enabled;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if(!m_bandEnabled || m_displayRanges.isEmpty()) {
            return;
        }

        auto* slider = qobject_cast<QSlider*>(parentWidget());
        if(!slider || slider->maximum() <= slider->minimum()) {
            return;
        }

        QStyleOptionSlider opt = sliderOption(slider);
        QStyle* style          = slider->style();
        const QRect groove = style->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, slider);
        if(groove.isEmpty()) {
            return;
        }

        const bool horizontal = opt.orientation == Qt::Horizontal;
        const auto handleRect = [&](int value) {
            opt.sliderPosition = value;
            opt.sliderValue    = value;
            return style->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, slider);
        };

        const QRect first   = handleRect(opt.minimum);
        const QRect last    = handleRect(opt.maximum);
        const QRect current = handleRect(slider->sliderPosition());
        const double origin = horizontal ? first.center().x() : first.center().y();
        const double span   = horizontal ? last.center().x() - first.center().x()
                                         : last.center().y() - first.center().y();
        if(span == 0.0) {
            return;
        }

        const double valueFraction
            = (slider->sliderPosition() - opt.minimum) / double(opt.maximum - opt.minimum);

        // Start after the handle when a band covers it, so the handle keeps its normal colour.
        const bool forward      = span > 0.0;
        const double handleEdge = horizontal ? (forward ? current.right() : current.left())
                                             : (forward ? current.bottom() : current.top());
        const auto toPixel      = [&](double fraction) { return origin + span * std::clamp(fraction, 0.0, 1.0); };

        // Text colour contrasts with the surrounding theme; low alpha keeps it a hint, not a widget.
        QColor colour = slider->palette().color(QPalette::WindowText);
        colour.setAlpha(80);

        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);

        // The groove rect includes style padding; a hair thinner keeps the bands flush with the line.
        const double thickness = std::max(1.0, (horizontal ? groove.height() : groove.width()) - 1.0);

        for(const BufferRange& range : std::as_const(m_displayRanges)) {
            if(range.end <= valueFraction) {
                continue; // behind the playhead; the normal progress fill already covers it
            }
            const double from = std::max(range.start, valueFraction);
            if(range.end <= from) {
                continue;
            }

            const double begin  = range.start <= valueFraction ? handleEdge : toPixel(from);
            const double finish = toPixel(range.end);
            if(std::abs(finish - begin) < 1.0) {
                continue;
            }

            QRectF band;
            if(horizontal) {
                const double top = groove.top() + (groove.height() - thickness) / 2.0;
                band = QRectF{std::min(begin, finish), top, std::abs(finish - begin), thickness};
            }
            else {
                const double left = groove.left() + (groove.width() - thickness) / 2.0;
                band = QRectF{left, std::min(begin, finish), thickness, std::abs(finish - begin)};
            }

            const double radius = std::min(band.width(), band.height()) / 2.0;
            painter.drawRoundedRect(band, radius, radius);
        }
    }

private:
    QTimer* m_animTimer;
    bool m_bandEnabled{true};
    QVector<BufferRange> m_targetRanges;
    QVector<BufferRange> m_displayRanges;
};

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

double SubsonicBufferMonitor::start() const
{
    return m_ranges.isEmpty() ? -1.0 : m_ranges.first().start;
}

double SubsonicBufferMonitor::end() const
{
    return m_ranges.isEmpty() ? -1.0 : m_ranges.last().end;
}

SubsonicBufferBar::SubsonicBufferBar(SettingsManager* settings, SubsonicBufferMonitor* monitor, QObject* parent)
    : QObject{parent}
    , m_settings{settings}
    , m_monitor{monitor}
    , m_scanTimer{new QTimer{this}}
{
    connect(m_scanTimer, &QTimer::timeout, this, &SubsonicBufferBar::scan);
    if(m_monitor) {
        connect(m_monitor, &SubsonicBufferMonitor::rangeChanged, this, &SubsonicBufferBar::updateOverlays);
    }
    // The settings dialog is the only way this toggle changes; re-read it when it closes.
    if(m_settings) {
        if(auto* dialog = m_settings->settingsDialog()) {
            connect(dialog, &SettingsDialogController::closing, this, &SubsonicBufferBar::updateOverlays);
        }
    }
    scan();
    // fooyin recreates seekbars on layout changes with no signal a plugin can hook; a cheap rescan
    // keeps the overlay attached without touching fooyin.
    m_scanTimer->start(1000);
}

SubsonicBufferBar::~SubsonicBufferBar()
{
    qDeleteAll(m_overlays);
}

bool SubsonicBufferBar::eventFilter(QObject* watched, QEvent* event)
{
    auto* slider = qobject_cast<QWidget*>(watched);
    if(!slider) {
        return false;
    }

    const auto it = m_overlays.constFind(slider);
    if(it == m_overlays.constEnd()) {
        return false;
    }

    switch(event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::StyleChange:
        case QEvent::PaletteChange:
        case QEvent::LayoutDirectionChange:
            it.value()->setGeometry(slider->rect());
            it.value()->raise();
            it.value()->update();
            break;
        default:
            break;
    }
    return false;
}

void SubsonicBufferBar::updateOverlays()
{
    const bool on = enabled();
    for(BufferOverlay* overlay : std::as_const(m_overlays)) {
        overlay->setBandEnabled(on);
        overlay->setRanges(m_monitor->ranges());
    }
}

bool SubsonicBufferBar::enabled() const
{
    return !m_settings || loadSubsonicSettings(m_settings).bufferBarEnabled;
}

void SubsonicBufferBar::scan()
{
    if(!m_monitor) {
        return;
    }

    const QWidgetList widgets = QApplication::allWidgets();
    for(QWidget* widget : widgets) {
        if(!widget || !QString::fromLatin1(widget->metaObject()->className()).contains(u"SeekBar"_s)) {
            continue;
        }

        auto* slider = widget->findChild<QSlider*>();
        if(!slider) {
            continue;
        }

        if(m_overlays.contains(slider)) {
            continue;
        }

        auto* overlay = new BufferOverlay{slider};
        m_overlays.insert(slider, overlay);
        overlay->setBandEnabled(enabled());
        overlay->setRanges(m_monitor->ranges());
        slider->installEventFilter(this);
        connect(slider, &QAbstractSlider::valueChanged, overlay, qOverload<>(&QWidget::update));
        connect(slider, &QObject::destroyed, this, [this, slider] { m_overlays.remove(slider); });

        overlay->setGeometry(slider->rect());
        overlay->show();
        overlay->raise();
    }
}
} // namespace Fooyin::Subsonic
