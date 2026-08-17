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

namespace Icons {

// Font stack: Fluent first, MDL2 for Windows 10, then a generic fallback.
inline QFont font(int pointSize = 12) {
    QFont f;
    f.setPointSize(pointSize);
    f.setStyleStrategy(QFont::PreferAntialias);
    f.setFamilies({"Segoe Fluent Icons", "Segoe MDL2 Assets", "Segoe UI Symbol"});
    return f;
}

inline QString glyph(char16_t code) { return QString(QChar(code)); }

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
inline QString arrange()    { return glyph(0xE80A); } // Move

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
