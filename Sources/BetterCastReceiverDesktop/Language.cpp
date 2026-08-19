#include "Language.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTranslator>

namespace Language {

bool install(QTranslator* translator) {
    if (!translator) return false;

    const QString code = resolvedCode();

    // English is the source language: every tr() already returns it, so there
    // is no catalogue to load and nothing to install.
    if (code == QLatin1String("en")) return false;

    // Built by qt_add_translations into the binary. Loading from a resource
    // rather than disk means a half-copied install cannot silently fall back
    // to English with no explanation.
    const QString path = QStringLiteral(":/i18n/bettercast_%1").arg(code);
    if (!translator->load(path)) {
        qWarning() << "Language: no catalogue for" << code << "at" << path;
        return false;
    }

    QCoreApplication::installTranslator(translator);
    return true;
}

} // namespace Language
