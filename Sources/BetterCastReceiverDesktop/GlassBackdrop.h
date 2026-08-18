#pragma once

#include <QImage>
#include <QObject>
#include <QRect>

// Frosted glass for a Qt Widgets window, by capturing what is behind it.
//
// Windows will not hand a Qt widget the composited Mica/Acrylic surface in any
// way Qt can paint over reliably: making the window translucent stops Qt
// clearing its backing store, and every page of the stack then accumulates on
// top of the last. So instead of asking the compositor for a blurred backdrop,
// take one — grab the desktop behind the window, blur it, and paint it as the
// window's own background. It is opaque, so it clears the buffer every frame,
// which is precisely what the translucent route failed to do.
//
// The approach is lifted from StephenLovino/liquidDX11 (MIT), which does this
// with DXGI Desktop Duplication, a Dual Kawase chain and an HLSL refraction
// shader. This is the cheap version of the same idea: heavy blur destroys
// detail anyway, so capturing at a fraction of the size and scaling back up
// with smooth interpolation gets most of the look for none of the GPU work —
// and avoids a second desktop-duplication instance competing with the one the
// sender already runs.
class GlassBackdrop : public QObject {
    Q_OBJECT

public:
    explicit GlassBackdrop(QObject* parent = nullptr);

    // Desktop behind `screenRect`, blurred, at that rect's size.
    // Returns a null image when capture is unavailable.
    QImage capture(const QRect& screenRect);

    // Hide a window from screen capture, so grabbing the desktop behind it does
    // not grab the window itself and feed back into an infinite tunnel.
    //
    // Costs something real for this app in particular: an excluded window is
    // also invisible to OBS, Teams and BetterCast's own streaming, so demoing
    // BetterCast would show a hole where BetterCast is. Callers should expose a
    // way to turn it off — liquidDX11 binds it to F11 for the same reason.
    // Needs Windows 10 2004+; older builds silently keep the window visible,
    // where the tunnel effect is the giveaway.
    static bool setExcludedFromCapture(WId window, bool excluded);

    // How much smaller the capture is than the window. Larger means blurrier
    // and cheaper; 8 is roughly a 40px Gaussian once scaled back up.
    static constexpr int kDownscale = 8;

    // Extra box-blur passes on the small image. Three approximates a Gaussian
    // closely enough that the difference is invisible at this scale.
    static constexpr int kBlurPasses = 3;
};
