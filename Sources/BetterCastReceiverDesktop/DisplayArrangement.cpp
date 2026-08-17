#include "DisplayArrangement.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QRectF>

DisplayArrangement::DisplayArrangement(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(180);

    // Repaint when the desktop layout changes — attaching a virtual display,
    // unplugging a monitor, or dragging one to a new position in Settings.
    auto repaintSelf = [this]() { update(); };
    connect(qApp, &QGuiApplication::screenAdded, this, repaintSelf);
    connect(qApp, &QGuiApplication::screenRemoved, this, repaintSelf);
    for (QScreen* s : QGuiApplication::screens()) {
        connect(s, &QScreen::geometryChanged, this, repaintSelf);
    }
}

void DisplayArrangement::setActiveDisplays(const QStringList& deviceNames) {
    if (m_active == deviceNames) return;
    m_active = deviceNames;
    update();
}

void DisplayArrangement::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) return;

    // Bounding box of the whole virtual desktop, which can start at negative
    // coordinates when a monitor sits left of or above the primary.
    QRect bounds = screens.first()->geometry();
    for (QScreen* s : screens) bounds = bounds.united(s->geometry());
    if (bounds.width() <= 0 || bounds.height() <= 0) return;

    const qreal margin = 14.0;
    const qreal availW = width() - margin * 2;
    const qreal availH = height() - margin * 2;
    const qreal scale = qMin(availW / bounds.width(), availH / bounds.height());

    // Centre the arrangement in the widget.
    const qreal offsetX = margin + (availW - bounds.width() * scale) / 2.0;
    const qreal offsetY = margin + (availH - bounds.height() * scale) / 2.0;

    const QColor text = palette().color(QPalette::WindowText);
    const QColor dim = palette().color(QPalette::Mid);
    const QColor accent(0x00, 0x78, 0xD4);
    const QColor live(0x4c, 0xaf, 0x50);

    for (QScreen* s : screens) {
        const QRect g = s->geometry();
        const QRectF r(offsetX + (g.x() - bounds.x()) * scale,
                       offsetY + (g.y() - bounds.y()) * scale,
                       g.width() * scale,
                       g.height() * scale);

        const bool isPrimary = (s == QGuiApplication::primaryScreen());
        const bool isStreaming = m_active.contains(s->name(), Qt::CaseInsensitive);

        QColor border = dim;
        QColor fill = dim;
        fill.setAlpha(30);
        if (isStreaming) { border = live; fill = live; fill.setAlpha(45); }
        else if (isPrimary) { border = accent; fill = accent; fill.setAlpha(35); }

        QPainterPath box;
        box.addRoundedRect(r, 6, 6);
        p.fillPath(box, fill);
        p.setPen(QPen(border, isStreaming || isPrimary ? 2.0 : 1.0));
        p.drawPath(box);

        // Label: resolution, plus what the screen is for. Kept inside the rect
        // so a small virtual display does not spill text over its neighbour.
        p.setPen(text);
        QFont f = p.font();
        f.setPointSize(8);
        p.setFont(f);

        const QString title = QString("%1 x %2").arg(g.width()).arg(g.height());
        QString subtitle;
        if (isStreaming)      subtitle = "streaming";
        else if (isPrimary)   subtitle = "main";

        QRectF textRect = r.adjusted(4, 4, -4, -4);
        p.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap,
                   subtitle.isEmpty() ? title : title + "\n" + subtitle);
    }
}
