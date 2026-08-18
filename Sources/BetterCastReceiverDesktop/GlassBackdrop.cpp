#include "GlassBackdrop.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif // _WIN32

#include <QPainter>

GlassBackdrop::GlassBackdrop(QObject* parent) : QObject(parent) {}

bool GlassBackdrop::setExcludedFromCapture(WId window, bool excluded) {
#ifdef _WIN32
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd) return false;
    return SetWindowDisplayAffinity(hwnd, excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
#else
    Q_UNUSED(window); Q_UNUSED(excluded);
    return false;
#endif
}

#ifdef _WIN32
namespace {

// Three box passes ≈ a Gaussian. Done on the already-downscaled image, so the
// cost is a few thousand pixels rather than a few million.
void boxBlur(QImage& img, int radius) {
    if (radius < 1 || img.isNull()) return;
    const int w = img.width(), h = img.height();

    for (int pass = 0; pass < GlassBackdrop::kBlurPasses; pass++) {
        // Horizontal
        QImage tmp = img;
        for (int y = 0; y < h; y++) {
            const QRgb* src = reinterpret_cast<const QRgb*>(tmp.constScanLine(y));
            QRgb* dst = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; x++) {
                int r = 0, g = 0, b = 0, n = 0;
                const int from = qMax(0, x - radius), to = qMin(w - 1, x + radius);
                for (int i = from; i <= to; i++) {
                    r += qRed(src[i]); g += qGreen(src[i]); b += qBlue(src[i]); n++;
                }
                dst[x] = qRgb(r / n, g / n, b / n);
            }
        }
        // Vertical
        tmp = img;
        for (int y = 0; y < h; y++) {
            QRgb* dst = reinterpret_cast<QRgb*>(img.scanLine(y));
            const int from = qMax(0, y - radius), to = qMin(h - 1, y + radius);
            for (int x = 0; x < w; x++) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int i = from; i <= to; i++) {
                    const QRgb* row = reinterpret_cast<const QRgb*>(tmp.constScanLine(i));
                    r += qRed(row[x]); g += qGreen(row[x]); b += qBlue(row[x]); n++;
                }
                dst[x] = qRgb(r / n, g / n, b / n);
            }
        }
    }
}

} // namespace
#endif // _WIN32

QImage GlassBackdrop::capture(const QRect& screenRect) {
#ifdef _WIN32
    if (screenRect.width() <= 0 || screenRect.height() <= 0) return QImage();

    const int smallW = qMax(1, screenRect.width()  / kDownscale);
    const int smallH = qMax(1, screenRect.height() / kDownscale);

    HDC screenDC = GetDC(nullptr);
    if (!screenDC) return QImage();

    HDC memDC = CreateCompatibleDC(screenDC);
    if (!memDC) { ReleaseDC(nullptr, screenDC); return QImage(); }

    // Top-down 32bpp so the bits map straight onto QImage without a flip.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = smallW;
    bmi.bmiHeader.biHeight      = -smallH;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return QImage();
    }

    HGDIOBJ oldBmp = SelectObject(memDC, bmp);

    // HALFTONE averages when shrinking rather than dropping pixels, which is
    // most of the blur. COLORONCOLOR would alias badly at this ratio.
    SetStretchBltMode(memDC, HALFTONE);
    SetBrushOrgEx(memDC, 0, 0, nullptr);

    const BOOL ok = StretchBlt(memDC, 0, 0, smallW, smallH,
                               screenDC,
                               screenRect.x(), screenRect.y(),
                               screenRect.width(), screenRect.height(),
                               SRCCOPY);

    QImage small;
    if (ok) {
        // Copy out before the DIB is destroyed; the QImage above only wraps it.
        small = QImage(reinterpret_cast<uchar*>(bits), smallW, smallH,
                       smallW * 4, QImage::Format_RGB32).copy();
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (small.isNull()) return QImage();

    boxBlur(small, 2);

    // Smooth upscale finishes the blur: interpolating an 8x-smaller image back
    // up is itself a wide low-pass, which is why this needs no shader.
    return small.scaled(screenRect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
#else
    Q_UNUSED(screenRect);
    return QImage();
#endif
}
