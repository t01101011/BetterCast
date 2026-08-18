#pragma once

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>

#include <algorithm>

#include "thirdparty/qrcodegen.hpp"

// Render text as a QR code.
//
// Used for the Wi-Fi join code on the hotspot page. The payload follows the
// WIFI: format every phone camera already understands, so nothing has to be
// installed on the other device to scan it.
namespace QrImage {

// Escape a value for the WIFI: URI scheme, where \ ; , : and " are delimiters.
inline QString wifiEscape(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar c : value) {
        if (c == QLatin1Char('\\') || c == QLatin1Char(';') || c == QLatin1Char(',') ||
            c == QLatin1Char(':')  || c == QLatin1Char('"')) {
            out += QLatin1Char('\\');
        }
        out += c;
    }
    return out;
}

// The standard join payload. WPA covers WPA2/WPA3 for scanning purposes.
inline QString wifiPayload(const QString& ssid, const QString& passphrase) {
    return QStringLiteral("WIFI:S:%1;T:WPA;P:%2;;")
        .arg(wifiEscape(ssid), wifiEscape(passphrase));
}

// Returns a null pixmap if the text cannot be encoded — callers should hide the
// QR rather than show an empty box.
inline QPixmap render(const QString& text, int targetPx,
                      const QColor& dark  = QColor(0, 0, 0),
                      const QColor& light = QColor(255, 255, 255)) {
    if (text.isEmpty() || targetPx <= 0) return QPixmap();

    try {
        const QByteArray utf8 = text.toUtf8();
        const auto qr = qrcodegen::QrCode::encodeText(utf8.constData(),
                                                      qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize();

        // Four modules of quiet zone, as the spec requires — scanners are
        // unreliable without it, especially against a dark app background.
        const int quiet = 4;
        const int total = modules + quiet * 2;

        // Integer scale only. A fractional one leaves modules of uneven pixel
        // width, which is exactly what makes a QR fail to decode.
        const int scale = std::max(1, targetPx / total);

        QImage image(total * scale, total * scale, QImage::Format_RGB32);
        image.fill(light);

        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(dark);
        for (int y = 0; y < modules; y++) {
            for (int x = 0; x < modules; x++) {
                if (qr.getModule(x, y)) {
                    painter.drawRect((x + quiet) * scale, (y + quiet) * scale, scale, scale);
                }
            }
        }
        painter.end();

        return QPixmap::fromImage(image);
    } catch (...) {
        return QPixmap();
    }
}

} // namespace QrImage
