#include "Theme.h"

#ifdef _WIN32
#include <Windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

// Present in newer SDKs; defined here so the build does not depend on the
// runner's SDK version. Unsupported values are ignored by older Windows.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2   // Mica
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3   // Acrylic — heavier blur than Mica
#endif
#endif

#include <QSettings>

namespace Theme {

// Persisted so the choice survives a restart. Defaults to System.
Mode savedMode() {
    QSettings s("BetterCast", "BetterCast");
    const int v = s.value("appearance/theme", static_cast<int>(Mode::System)).toInt();
    if (v < 0 || v > static_cast<int>(Mode::Glass)) return Mode::System;
    return static_cast<Mode>(v);
}

void setSavedMode(Mode mode) {
    QSettings s("BetterCast", "BetterCast");
    s.setValue("appearance/theme", static_cast<int>(mode));
}

QPalette qtPalette(const Palette& p) {
    QPalette q;
    // paletteWindow()/paletteSurface() fall back to the opaque values, because
    // QColor cannot parse the rgba() strings glass mode uses for its fills.
    const QColor text(p.text), dim(p.textDim),
                 window(p.paletteWindow()), surface(p.paletteSurface());

    q.setColor(QPalette::Window, window);
    q.setColor(QPalette::WindowText, text);
    q.setColor(QPalette::Base, surface);
    q.setColor(QPalette::AlternateBase, QColor(p.surfaceAlt));
    q.setColor(QPalette::Text, text);
    q.setColor(QPalette::Button, surface);
    q.setColor(QPalette::ButtonText, text);
    q.setColor(QPalette::Highlight, QColor(p.accent));
    q.setColor(QPalette::HighlightedText, QColor("#ffffff"));
    q.setColor(QPalette::ToolTipBase, surface);
    q.setColor(QPalette::ToolTipText, text);
    q.setColor(QPalette::PlaceholderText, dim);
    // Mid is what the stylesheets use for secondary text, so it must stay
    // readable against the window background in BOTH themes.
    q.setColor(QPalette::Mid, dim);
    q.setColor(QPalette::Dark, QColor(p.border));
    q.setColor(QPalette::Disabled, QPalette::WindowText, QColor(p.textFaint));
    q.setColor(QPalette::Disabled, QPalette::Text, QColor(p.textFaint));
    q.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(p.textFaint));
    return q;
}

QString stylesheet(const Palette& p) {
    return QString(R"(
    QMainWindow { background-color: %WINDOW%; }
    QSplitter { background-color: transparent; }
    QSplitter::handle { background-color: %BORDER%; width: 1px; }

    QListWidget {
        background-color: %SIDEBAR%;
        border: none;
        outline: none;
        font-size: 13px;
        padding-top: 8px;
    }
    QListWidget::item {
        color: %TEXT%;
        padding: 7px 14px;
        border-radius: 6px;
        margin: 1px 8px;
    }
    QListWidget::item:selected {
        background-color: %SELECTION%;
        color: %ACCENT_TEXT%;
    }
    QListWidget::item:hover:!selected {
        background-color: %HOVER%;
    }

    QStackedWidget { background-color: transparent; }
    QScrollArea { background-color: transparent; border: none; }
    QScrollArea > QWidget > QWidget { background-color: transparent; }

    QLabel { color: %TEXT%; background: transparent; }

    QLineEdit {
        background-color: %SURFACE%;
        color: %TEXT%;
        border: 1px solid %BORDER%;
        border-radius: %CTRL_RADIUS%;
        padding: 7px 10px;
        font-size: 13px;
        selection-background-color: %ACCENT%;
    }
    QLineEdit:focus { border-color: %ACCENT%; }

    QPushButton {
        background-color: %SURFACE%;
        color: %TEXT%;
        border: 1px solid %BORDER_STRONG%;
        border-radius: %CTRL_RADIUS%;
        padding: 8px 16px;
        font-size: 13px;
    }
    QPushButton:hover { background-color: %SURFACE_HOVER%; }
    QPushButton:pressed { background-color: %BORDER%; }
    QPushButton:disabled { background-color: %SURFACE%; color: %TEXT_FAINT%; border-color: %BORDER%; }

    QGroupBox {
        color: %TEXT_DIM%;
        border: 1px solid %BORDER%;
        border-radius: %CARD_RADIUS%;
        margin-top: 20px;
        padding: 26px 18px 16px 18px;
        font-size: 12px;
        font-weight: bold;
        background-color: %SURFACE_GLASS%;
    }
    QGroupBox::title {
        subcontrol-origin: margin;
        left: 16px;
        padding: 0 6px;
        color: %TEXT_DIM%;
    }

    QTextEdit {
        background-color: %SURFACE_ALT%;
        color: %TEXT_DIM%;
        border: 1px solid %BORDER%;
        border-radius: 8px;
        font-family: "Cascadia Code", "Consolas", monospace;
        font-size: 11px;
    }

    QSpinBox {
        background-color: %SURFACE%;
        color: %TEXT%;
        border: 1px solid %BORDER%;
        border-radius: 6px;
        padding: 5px 8px;
        font-size: 13px;
    }
    QSpinBox:focus { border-color: %ACCENT%; }
    QSpinBox::up-button, QSpinBox::down-button {
        background-color: %SURFACE_HOVER%;
        border: none;
        width: 20px;
    }

    QComboBox {
        background-color: %SURFACE%;
        color: %TEXT%;
        border: 1px solid %BORDER%;
        border-radius: 6px;
        padding: 5px 8px;
        font-size: 13px;
    }
    QComboBox:focus { border-color: %ACCENT%; }
    QComboBox::drop-down { border: none; }
    QComboBox QAbstractItemView {
        background-color: %SURFACE%;
        color: %TEXT%;
        selection-background-color: %ACCENT%;
        selection-color: #ffffff;
        border: 1px solid %BORDER%;
    }

    QCheckBox { color: %TEXT%; font-size: 13px; spacing: 8px; }
    QCheckBox::indicator {
        width: 16px; height: 16px;
        border: 1px solid %BORDER_STRONG%; border-radius: 4px;
        background-color: %SURFACE%;
    }
    QCheckBox::indicator:checked { background-color: %ACCENT%; border-color: %ACCENT%; }
)")
        .replace("%WINDOW%", p.window)
        .replace("%SIDEBAR%", p.sidebar)
        .replace("%SURFACE_HOVER%", p.surfaceHover)
        .replace("%SURFACE_ALT%", p.surfaceAlt)
        // Cards sit slightly above the backdrop so Mica reads as depth rather
        // than a flat wash. Kept subtle: too much and text contrast suffers.
        //
        // Glass mode leans harder on this, because there the card fill is the
        // only thing separating text from whatever wallpaper is behind it.
        .replace("%SURFACE_GLASS%", p.glass   ? "rgba(255, 255, 255, 0.07)"
                                   : p.isDark ? "rgba(255, 255, 255, 0.03)"
                                              : "rgba(255, 255, 255, 0.55)")
        // Bigger, softer corners in glass mode — the rounding is most of what
        // separates a frosted sheet from a flat panel.
        .replace("%CARD_RADIUS%", p.glass ? "16px" : "10px")
        .replace("%CTRL_RADIUS%", p.glass ? "9px" : "6px")
        .replace("%SURFACE%", p.surface)
        .replace("%BORDER_STRONG%", p.borderStrong)
        .replace("%BORDER%", p.border)
        .replace("%TEXT_DIM%", p.textDim)
        .replace("%TEXT_FAINT%", p.textFaint)
        .replace("%TEXT%", p.text)
        .replace("%ACCENT_TEXT%", p.accentText)
        .replace("%ACCENT%", p.accent)
        .replace("%SELECTION%", p.selection)
        .replace("%HOVER%", p.hoverOverlay);
}

void applyWindowBackdrop(QWidget* window, const Palette& p) {
#ifdef _WIN32
    if (!window) return;
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd) return;

    // Title bar follows the theme. Without this a light window keeps a dark
    // caption, which looks broken rather than intentional.
    BOOL dark = p.isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // Acrylic blurs far more aggressively than Mica, which is what makes panels
    // read as frosted glass rather than tinted wallpaper. Fails harmlessly on
    // Windows 10 and early Windows 11 builds.
    int backdrop = p.glass ? DWMSBT_TRANSIENTWINDOW : DWMSBT_MAINWINDOW;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    // The backdrop has always been requested, but nothing ever showed through:
    // the window painted an opaque fill straight over it. Glass mode stops Qt
    // filling the window so the composited backdrop is what you actually see.
    //
    // Qt may only honour this at window creation, so switching into or out of
    // glass can need a restart to take full effect.
    window->setAttribute(Qt::WA_TranslucentBackground, p.glass);
    window->setAttribute(Qt::WA_NoSystemBackground, p.glass);
#else
    Q_UNUSED(window); Q_UNUSED(p);
#endif
}

} // namespace Theme
