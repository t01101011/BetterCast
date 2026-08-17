#pragma once

// Icon glyphs for the sidebar and detail panels.
//
// The macOS app uses SF Symbols. The native Windows counterpart is Segoe Fluent
// Icons (Windows 11), which falls back to Segoe MDL2 Assets (Windows 10) — the
// two share codepoints across the E7xx-E9xx range used here. Both ship with the
// OS, so there are no icon assets to bundle.
//
// This replaces the emoji the sidebar used to draw. Emoji render through the
// colour font at whatever size and baseline that font dictates, so they sat
// inconsistently against the text labels and looked nothing like the rest of
// Windows.
//
// Codepoints are written as hex rather than literal characters: these live in
// the Unicode private use area, where a stray editor or encoding change can
// silently mangle them.

#include <QString>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QColor>

namespace Icons {

// The icon font must be applied to whatever draws the glyph. Concatenating the
// glyph into a label's text does NOT work: the label renders in the UI font
// (Segoe UI), which has nothing at these private-use codepoints, so every icon
// came out as an empty box.
inline QString availableFamily() {
    static const QString family = []() -> QString {
        const QStringList candidates = {"Segoe Fluent Icons", "Segoe MDL2 Assets"};
        const QStringList installed = QFontDatabase::families();
        for (const auto& c : candidates) {
            if (installed.contains(c)) return c;
        }
        return QString();   // no icon font — callers fall back to text only
    }();
    return family;
}

inline QFont font(int pixelSize = 16) {
    QFont f(availableFamily());
    f.setPixelSize(pixelSize);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

inline QString glyph(char16_t code) { return QString(QChar(code)); }

// Render a glyph into a QIcon so it can be attached with setIcon(), keeping the
// label text in the normal UI font. Returns a null icon when no icon font is
// installed, which leaves a clean text-only row rather than a row of tofu.
inline QIcon icon(const QString& glyphStr, const QColor& color = QColor("#c8c8c8"),
                  int sizePx = 16) {
    if (availableFamily().isEmpty()) return QIcon();

    const qreal dpr = 2.0;   // render at 2x so it stays sharp on HiDPI
    QPixmap pm(static_cast<int>(sizePx * dpr), static_cast<int>(sizePx * dpr));
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(font(static_cast<int>(sizePx * dpr)));
    p.setPen(color);
    p.drawText(pm.rect(), Qt::AlignCenter, glyphStr);
    p.end();

    pm.setDevicePixelRatio(dpr);
    return QIcon(pm);
}

// Navigation — mapped from the macOS sidebar's SF Symbols
inline QString overview()   { return glyph(0xE7F4); } // TVMonitor   <- rectangle.on.rectangle
inline QString display()    { return glyph(0xE7F4); } // TVMonitor   <- display
inline QString receive()    { return glyph(0xE896); } // Download    <- display.and.arrow.down
inline QString send()       { return glyph(0xE898); } // Upload
inline QString settings()   { return glyph(0xE713); } // Setting     <- gearshape
inline QString logs()       { return glyph(0xE7C3); } // Page        <- text.alignleft

// Device kinds
inline QString phone()      { return glyph(0xE8EA); } // CellPhone   <- apps.iphone
inline QString pc()         { return glyph(0xE977); } // PC          <- pc
inline QString desktop()    { return glyph(0xE7F8); } // Devices     <- desktopcomputer

// Actions and status
inline QString link()       { return glyph(0xE71B); } // Link
inline QString network()    { return glyph(0xE968); } // NetworkTower
inline QString wifi()       { return glyph(0xE701); } // WiFi
inline QString usb()        { return glyph(0xECF0); } // USB
inline QString refresh()    { return glyph(0xE72C); } // Refresh
inline QString add()        { return glyph(0xE710); } // Add
inline QString remove()     { return glyph(0xE74D); } // Delete
inline QString check()      { return glyph(0xE73E); } // CheckMark
inline QString warning()    { return glyph(0xE7BA); } // Warning
inline QString info()       { return glyph(0xE946); } // Info
inline QString power()      { return glyph(0xE7E8); } // PowerButton
inline QString heart()      { return glyph(0xEB51); } // HeartFill
inline QString arrange()    { return glyph(0xE80A); } // Move

// Mask a square logo to rounded corners.
//
// macOS masks app icons to a superellipse, so a square logo looks foreign
// beside one. Qt has no squircle primitive, but a generous corner radius reads
// the same at icon sizes. Rendered at `dpr` times the requested size and
// downsampled, which keeps the curve smooth even at 16px.
inline QPixmap rounded(const QPixmap& src, int size, qreal dpr = 2.0) {
    if (src.isNull() || size <= 0) return src;

    const int px = static_cast<int>(size * dpr);
    const qreal radius = px * 0.22;   // ~22% of the edge approximates the macOS mask

    QPixmap scaled = src.scaled(px, px, Qt::KeepAspectRatioByExpanding,
                                Qt::SmoothTransformation);
    QPixmap out(px, px);
    out.fill(Qt::transparent);

    QPainter painter(&out);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, px, px, radius, radius);
    painter.setClipPath(clip);
    // Centre the expanded image so a non-square source is cropped, not squashed.
    painter.drawPixmap((px - scaled.width()) / 2, (px - scaled.height()) / 2, scaled);
    painter.end();

    if (dpr != 1.0) out.setDevicePixelRatio(dpr);
    return out;
}

// The application icon, rounded at every size Windows asks for.
//
// setWindowIcon drives the title bar, the taskbar button and Alt-Tab, and it
// used to be handed the raw square PNG — so rounding the in-app logo and the
// generated .ico still left those three square. Supplying real pixmaps per size
// beats letting Qt rescale one, which softens the corners at 16px.
inline QIcon appIcon(const QString& resourcePath = QStringLiteral(":/appicon.png")) {
    const QPixmap src(resourcePath);
    if (src.isNull()) return QIcon();

    QIcon icon;
    for (int s : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        icon.addPixmap(rounded(src, s, 1.0));   // dpr 1: QIcon wants true pixel sizes
    }
    return icon;
}

// Pick a device glyph from its advertised name, mirroring the macOS
// SidebarDeviceRow.deviceIcon logic.
inline QString forDeviceName(const QString& name) {
    const QString n = name.toLower();
    if (n.contains("android") || n.contains("iphone") || n.contains("ipad")) return phone();
    if (n.contains("windows")) return pc();
    if (n.contains("linux")) return desktop();
    return display();  // Macs and anything unrecognised
}

} // namespace Icons
