#pragma once

// Light/dark theming that follows the OS, plus the Windows 11 Mica backdrop.
//
// The stylesheet used to be one hard-coded dark string, so there was no way to
// express a light variant. It is now generated from a Palette, and the two
// palettes are the only thing that differs between themes — which also means a
// colour can't be updated in one theme and forgotten in the other.
//
// Mica is the closest native counterpart to the macOS window vibrancy: the
// desktop wallpaper is sampled and blurred behind the window by the compositor,
// rather than faked with translucency. It needs Windows 11 build 22621+ and
// degrades to a plain background everywhere else.

#include <QColor>
#include <QPalette>
#include <QGuiApplication>
#include <QString>
#include <QStyleHints>
#include <QWidget>

namespace Theme {

struct Palette {
    QString window;       // window + stack background
    QString sidebar;      // sidebar background
    QString surface;      // input/button fill
    QString surfaceHover;
    QString surfaceAlt;   // log viewer, deep recesses
    QString border;
    QString borderStrong;
    QString text;
    QString textDim;
    QString textFaint;
    QString accent;
    QString accentText;
    QString selection;    // rgba string for selected rows
    QString hoverOverlay; // rgba string for hovered rows
    bool isDark = true;

    // Glass mode lets the compositor's blurred backdrop show through, so most
    // fills become rgba() strings. QPalette cannot parse those and needs real
    // colours for the widgets Qt draws itself, hence a solid counterpart for
    // the two that matter. Both default to the opaque values above, so the
    // ordinary themes are unaffected.
    QString windowSolid;
    QString surfaceSolid;
    bool glass = false;

    QString paletteWindow()  const { return windowSolid.isEmpty()  ? window  : windowSolid; }
    QString paletteSurface() const { return surfaceSolid.isEmpty() ? surface : surfaceSolid; }
};

inline Palette darkPalette() {
    Palette p;
    p.window       = "#1a1a1a";
    p.sidebar      = "#1e1e1e";
    p.surface      = "#2a2a2a";
    p.surfaceHover = "#444";
    p.surfaceAlt   = "#111";
    p.border       = "#444";
    p.borderStrong = "#555";
    p.text         = "#e0e0e0";
    p.textDim      = "#888";
    p.textFaint    = "#666";
    p.accent       = "#0078D4";
    p.accentText   = "#4da6ff";
    p.selection    = "rgba(0, 120, 212, 0.18)";
    p.hoverOverlay = "rgba(255, 255, 255, 0.05)";
    p.isDark = true;
    return p;
}

inline Palette lightPalette() {
    Palette p;
    p.window       = "#f3f3f3";
    p.sidebar      = "#eaeaea";
    p.surface      = "#ffffff";
    p.surfaceHover = "#e6e6e6";
    p.surfaceAlt   = "#fafafa";
    p.border       = "#d0d0d0";
    p.borderStrong = "#b8b8b8";
    p.text         = "#1a1a1a";
    p.textDim      = "#5f5f5f";
    p.textFaint    = "#9a9a9a";
    p.accent       = "#0067c0";
    p.accentText   = "#0067c0";
    p.selection    = "rgba(0, 103, 192, 0.14)";
    p.hoverOverlay = "rgba(0, 0, 0, 0.05)";
    p.isDark = false;
    return p;
}

// A dark glass finish: the window itself is transparent and every panel is a
// translucent sheet over the compositor's blurred backdrop, which is as close
// as Windows gets to the frosted look of macOS vibrancy.
//
// Built on the dark palette because translucent white over a bright wallpaper
// washes text out. Secondary text is lifted well above the flat theme's grey —
// #888 is readable on a fixed #1a1a1a but disappears over a photo.
inline Palette glassPalette() {
    Palette p     = darkPalette();
    // Semi-opaque, NOT transparent.
    //
    // A fully transparent window paints nothing, so Qt never clears the
    // backing store and every page of the stack accumulates on top of the
    // last — the whole UI rendered at once, overlapping. A translucent wash
    // covers every pixel each frame (clearing it) while still letting the
    // Acrylic backdrop read through at roughly a quarter strength.
    p.window       = "rgba(24, 24, 27, 0.72)";
    p.sidebar      = "rgba(255, 255, 255, 0.04)";
    p.surface      = "rgba(255, 255, 255, 0.07)";
    p.surfaceHover = "rgba(255, 255, 255, 0.14)";
    p.surfaceAlt   = "rgba(0, 0, 0, 0.28)";
    p.border       = "rgba(255, 255, 255, 0.12)";
    p.borderStrong = "rgba(255, 255, 255, 0.22)";
    p.text         = "#f5f5f5";
    p.textDim      = "#c2c2c2";
    p.textFaint    = "#9a9a9a";
    p.selection    = "rgba(255, 255, 255, 0.16)";
    p.hoverOverlay = "rgba(255, 255, 255, 0.08)";
    p.windowSolid  = "#1a1a1a";
    p.surfaceSolid = "#2f2f2f";
    p.glass        = true;
    p.isDark       = true;
    return p;
}

// User's choice. Order matches the Settings combo box.
enum class Mode { System = 0, Light = 1, Dark = 2, Glass = 3 };

Mode savedMode();
void setSavedMode(Mode mode);

// True when the OS is set to a dark app theme. Qt 6.5+ reports this and emits
// colorSchemeChanged when the user flips it, so the app can follow live.
inline bool systemPrefersDark() {
    if (auto* hints = QGuiApplication::styleHints()) {
        return hints->colorScheme() == Qt::ColorScheme::Dark;
    }
    return true;
}

// The palette to draw with, honouring an explicit override before the OS.
inline Palette activePalette() {
    switch (savedMode()) {
        case Mode::Light: return lightPalette();
        case Mode::Dark:  return darkPalette();
        case Mode::Glass: return glassPalette();
        case Mode::System: break;
    }
    return systemPrefersDark() ? darkPalette() : lightPalette();
}

QString stylesheet(const Palette& p);

// Widget palette matching the theme.
//
// Inline stylesheets across the UI use palette(window-text) / palette(mid)
// instead of hard-coded colours — hard-coded white text was invisible on a
// light background. Those resolve against the widget's QPalette, so it has to
// be set from the theme rather than left on whatever the OS supplies, or
// forcing Light while Windows is Dark would still paint light-on-light.
QPalette qtPalette(const Palette& p);

// Apply the Mica backdrop and match the title bar to the theme.
// No-op below Windows 11 22621.
void applyWindowBackdrop(QWidget* window, const Palette& p);

} // namespace Theme
