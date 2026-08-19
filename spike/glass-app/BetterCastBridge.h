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

// Frames per second the throttle is currently allowing, for the UI to show.
int currentFrameCap();

void shutdown();

// Receivers seen on the network right now, newest state each call.
const std::vector<Device>& devices();

// Proves an OpenGL widget and a D3D11 swapchain survive in one process, which
// is the one coexistence question the architecture rests on. Returns false if
// the window could not be created.
bool toggleProbeWindow();
bool probeWindowOpen();

// Last line the core logged, so the glass UI can show that real machinery is
// running behind it rather than mock data.
std::string lastLogLine();

} // namespace BetterCastBridge
