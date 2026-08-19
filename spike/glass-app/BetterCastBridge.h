#pragma once

// Lets liquidDX11's D3D11 render loop drive BetterCast's real Qt core.
//
// Two things had to be true before this was possible, and both now are:
//
//   1. The core is widget-free. Capture, encode, the virtual display driver,
//      discovery and the network layer used to include MainWindow.h just to
//      reach LogManager, dragging QMainWindow in with it. Lifting LogManager
//      out means those objects can be linked by any front end.
//
//   2. A full QApplication, not QCoreApplication. Widgets stay available, so
//      the existing VideoWindow that shows received video keeps working
//      unchanged while the main UI is a native D3D11 window. Nothing about the
//      receive path needs porting, which was the objection that looked fatal.
//
// The glass loop owns the frame timing and calls pump() once per frame, which
// is where Qt's timers, sockets and queued signals actually run. No IPC, no
// second process, no serialisation boundary.
//
// Deliberately plain types across this seam: the page fragments are ImGui code
// including headers from a foreign project, and handing them QString or
// QObject would drag Qt's moc and include order into that world for nothing.

#include <cstdint>
#include <string>
#include <vector>

namespace BetterCastBridge {

struct Device {
    std::string name;
    std::string host;
    uint16_t    port = 0;
};

// Creates the QApplication and starts mDNS browsing. Safe to call once.
bool init(int argc, char** argv);

// Run Qt's event loop for this frame, and throttle the frame rate when the
// window is not in front.
//
// Must be called from the render loop or nothing Qt-driven ever fires: no
// discovery, no sockets, no timers.
//
// The throttle is here rather than at another patch site because this is
// already called once per frame. liquidDX11 redraws continuously, which is
// right for a showcase and wrong for a utility that sits open all day - it
// measured around 50% of an integrated GPU doing nothing. A backgrounded
// window has nobody looking at it, so it does not need 60fps.
void pump();

// Whether this frame is worth drawing at all.
//
// The real saving is not a lower frame rate, it is not rendering. A utility
// that sits open all day changes nothing on screen for minutes at a time, and
// every frame it draws in that state is a GPU cycle spent on an identical
// image - the swapchain still shows the last frame presented, so skipping
// costs nothing visually.
//
// Sleeps before returning false, so the caller can `continue` without spinning
// the loop. Returns true whenever the user is interacting, the window has just
// come forward, or the core reports something new.
bool shouldRender();

// Frames per second the throttle is currently allowing, for the UI to show.
int currentFrameCap();

// Forces the next frame to draw. Call when something changes that the render
// loop cannot see for itself - a device appearing, a stream starting.
void requestRedraw();

void shutdown();

// Receivers seen on the network right now, newest state each call.
const std::vector<Device>& devices();

// ── Streaming ────────────────────────────────────────────────────────────
//
// The real SenderController, the same one the shipping app drives: DXGI
// duplication into a hardware H.264 encoder, one virtual display per
// receiver. Nothing here reimplements any of it.

// Begin streaming to a receiver. width/height of 0 means "match the primary".
// Returns false if no display could be claimed - see lastLogLine() for why,
// since the reasons are worth reading rather than reducing to a bool.
bool startSending(const std::string& host, uint16_t port,
                  int fps, int bitrateMbps, int width, int height);

void stopSending(const std::string& host);
bool isSendingTo(const std::string& host);
int  sessionCount();

// Virtual display this receiver's stream is being captured from, empty when
// it is not streaming.
std::string displayForReceiver(const std::string& host);

// Encoder actually in use, e.g. "h264_nvenc". Empty until the first stream
// starts, because the encoder is probed at that point rather than up front.
std::string encoderInfo();

// Proves an OpenGL widget and a D3D11 swapchain survive in one process, which
// is the one coexistence question the architecture rests on. Returns false if
// the window could not be created.
bool toggleProbeWindow();
bool probeWindowOpen();

// Last line the core logged, so the glass UI can show that real machinery is
// running behind it rather than mock data.
std::string lastLogLine();

} // namespace BetterCastBridge
