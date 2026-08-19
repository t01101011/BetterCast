#pragma once

// UI language selection.
//
// Mirrors how Theme handles appearance: a persisted choice that defaults to
// following the system, resolved at startup and changeable from Settings.
//
// Translations are compiled into the binary as Qt resources rather than
// shipped as loose .qm files, so the installer stays a single executable and
// a partial install cannot leave the app half-translated.

#include <QCoreApplication>
#include <QLocale>
#include <QSettings>
#include <QString>
#include <QVector>

class QTranslator;

namespace Language {

struct Entry {
    QString code;        // "en", "fil", "es" — matches the .ts suffix
    QString endonym;     // the language's name in itself, which is what a
                         // speaker looking for it will recognise
};

// English first as the source language, then alphabetical by code.
inline QVector<Entry> available() {
    return {
        { QStringLiteral("en"),  QStringLiteral("English") },
        { QStringLiteral("es"),  QStringLiteral("Español") },
        { QStringLiteral("fil"), QStringLiteral("Filipino") },
    };
}

// Empty means "follow the system".
inline QString savedCode() {
    QSettings s("BetterCast", "BetterCast");
    return s.value("appearance/language", QString()).toString();
}

inline void setSavedCode(const QString& code) {
    QSettings s("BetterCast", "BetterCast");
    s.setValue("appearance/language", code);
}

// The language actually used: an explicit choice, else the closest match for
// the system locale, else English.
inline QString resolvedCode() {
    const QString saved = savedCode();
    const auto entries = available();

    if (!saved.isEmpty()) {
        for (const auto& e : entries) {
            if (e.code == saved) return saved;
        }
        // A saved code we no longer ship: fall through to the system rather
        // than showing a language the build cannot load.
    }

    const QString sys = QLocale::system().name();           // e.g. "es_ES"
    const QString lang = sys.section(QLatin1Char('_'), 0, 0); // e.g. "es"
    for (const auto& e : entries) {
        if (e.code == lang) return e.code;
    }
    return QStringLiteral("en");
}

// Load the resolved language into `translator` and install it. English is the
// source language and has no catalogue, so it succeeds by doing nothing.
// Returns true when a catalogue was actually installed.
bool install(QTranslator* translator);

} // namespace Language
