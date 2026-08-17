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

// User's choice. Order matches the Settings combo box.
enum class Mode { System = 0, Light = 1, Dark = 2 };

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
        case Mode::System: break;
    }
    return systemPrefersDark() ? darkPalette() : lightPalette();
}

QString stylesheet(const Palette& p);

// Apply the Mica backdrop and match the title bar to the theme.
// No-op below Windows 11 22621.
void applyWindowBackdrop(QWidget* window, const Palette& p);

} // namespace Theme
