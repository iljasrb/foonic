/*
 * Fooyin Subsonic plugin
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subsonicbufferbar.h"

#include <QAbstractSlider>
#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QSet>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QWidget>

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
        // read as one smooth slide and the timer stops on its own once the band has caught up.
        m_animTimer->setInterval(16);
        connect(m_animTimer, &QTimer::timeout, this, [this] {
            const double diff = m_targetEnd - m_displayEnd;
            if(std::abs(diff) < 0.0005) {
                m_displayEnd = m_targetEnd;
                m_animTimer->stop();
            }
            else {
                m_displayEnd += diff * 0.25;
            }
            update();
        });
    }

    //! Feeds a new buffered range. Growth is animated; shrink/seek/reset snaps.
    void setRange(double start, double end)
    {
        m_displayStart = start;
        m_targetEnd    = end;

        if(end <= start || m_displayEnd < 0.0 || end < m_displayEnd) {
            m_displayEnd = end;
            m_animTimer->stop();
            update();
            return;
        }

        if(end > m_displayEnd && !m_animTimer->isActive()) {
            m_animTimer->start();
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        auto* slider = qobject_cast<QSlider*>(parentWidget());
        if(!slider || slider->maximum() <= slider->minimum()) {
            return;
        }

        const double start = m_displayStart;
        const double end   = m_displayEnd;
        if(end <= start) {
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
        const double from = std::max(start, valueFraction);
        if(end <= from) {
            return; // nothing buffered ahead of the playhead
        }

        // Start after the handle when the buffer covers it, so the handle keeps its normal colour.
        const bool forward      = span > 0.0;
        const double handleEdge = horizontal ? (forward ? current.right() : current.left())
                                             : (forward ? current.bottom() : current.top());
        const auto toPixel      = [&](double fraction) { return origin + span * std::clamp(fraction, 0.0, 1.0); };
        const double begin      = start <= valueFraction ? handleEdge : toPixel(from);
        const double finish     = toPixel(end);
        if(std::abs(finish - begin) < 1.0) {
            return;
        }

        QRectF band;
        // The groove rect includes style padding; a hair thinner keeps the band flush with the painted line.
        const double thickness = std::max(1.0, (horizontal ? groove.height() : groove.width()) - 1.0);
        if(horizontal) {
            const double top = groove.top() + (groove.height() - thickness) / 2.0;
            band = QRectF{std::min(begin, finish), top, std::abs(finish - begin), thickness};
        }
        else {
            const double left = groove.left() + (groove.width() - thickness) / 2.0;
            band = QRectF{left, std::min(begin, finish), thickness, std::abs(finish - begin)};
        }

        // Text colour contrasts with the surrounding theme; low alpha keeps it a hint, not a widget.
        QColor colour = slider->palette().color(QPalette::WindowText);
        colour.setAlpha(80);

        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        const double radius = std::min(band.width(), band.height()) / 2.0;
        painter.drawRoundedRect(band, radius, radius);
    }

private:
    QTimer* m_animTimer;
    double m_targetEnd{-1.0};
    double m_displayStart{-1.0};
    double m_displayEnd{-1.0};
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
    if(qFuzzyCompare(m_start, start) && qFuzzyCompare(m_end, end)) {
        return;
    }
    m_start = start;
    m_end   = end;
    Q_EMIT rangeChanged();
}

void SubsonicBufferMonitor::reset()
{
    if(m_end < 0.0) {
        return;
    }
    m_start = -1.0;
    m_end   = -1.0;
    Q_EMIT rangeChanged();
}

double SubsonicBufferMonitor::start() const
{
    return m_start;
}

double SubsonicBufferMonitor::end() const
{
    return m_end;
}

SubsonicBufferBar::SubsonicBufferBar(SubsonicBufferMonitor* monitor, QObject* parent)
    : QObject{parent}
    , m_monitor{monitor}
    , m_scanTimer{new QTimer{this}}
{
    connect(m_scanTimer, &QTimer::timeout, this, &SubsonicBufferBar::scan);
    if(m_monitor) {
        connect(m_monitor, &SubsonicBufferMonitor::rangeChanged, this, &SubsonicBufferBar::updateOverlays);
    }
    scan();
    // fooyin recreates seekbars on layout changes with no signal a plugin can hook; a cheap rescan
    // keeps the overlay attached without touching fooyin.
    m_scanTimer->start(1000);
}

SubsonicBufferBar::~SubsonicBufferBar()
{
    for(BufferOverlay* overlay : std::as_const(m_overlays)) {
        delete overlay;
    }
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
        case QEvent::Move:
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
    for(BufferOverlay* overlay : std::as_const(m_overlays)) {
        overlay->setRange(m_monitor->start(), m_monitor->end());
    }
}

void SubsonicBufferBar::scan()
{
    if(!m_monitor) {
        return;
    }

    QSet<QWidget*> found;

    const QWidgetList widgets = QApplication::allWidgets();
    for(QWidget* widget : widgets) {
        if(!widget || !QString::fromLatin1(widget->metaObject()->className()).contains(u"SeekBar"_s)) {
            continue;
        }

        auto* slider = widget->findChild<QSlider*>();
        if(!slider) {
            continue;
        }
        found.insert(slider);

        if(m_overlays.contains(slider)) {
            continue;
        }

        auto* overlay = new BufferOverlay{slider};
        m_overlays.insert(slider, overlay);
        overlay->setRange(m_monitor->start(), m_monitor->end());
        slider->installEventFilter(this);
        connect(slider, &QAbstractSlider::valueChanged, overlay, qOverload<>(&QWidget::update));
        connect(slider, &QAbstractSlider::sliderMoved, overlay, qOverload<>(&QWidget::update));
        connect(slider, &QObject::destroyed, this, [this, slider] { m_overlays.remove(slider); });

        overlay->setGeometry(slider->rect());
        overlay->show();
        overlay->raise();
    }

    for(auto it = m_overlays.begin(); it != m_overlays.end();) {
        if(!found.contains(it.key())) {
            it.value()->deleteLater();
            it = m_overlays.erase(it);
        }
        else {
            ++it;
        }
    }
}
} // namespace Fooyin::Subsonic
