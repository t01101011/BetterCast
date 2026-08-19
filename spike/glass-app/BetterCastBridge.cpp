#include "BetterCastBridge.h"

#include "LogManager.h"
#include "ServiceDiscovery.h"

#include <QApplication>
#include <QCoreApplication>
#include <QOpenGLWidget>
#include <QTimer>

#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace BetterCastBridge {
namespace {

std::unique_ptr<QApplication>     g_app;
std::unique_ptr<ServiceDiscovery> g_discovery;
std::unique_ptr<QOpenGLWidget>    g_probe;

std::vector<Device> g_devices;
std::string         g_lastLog;
int                 g_frameCap = 60;

#ifdef _WIN32
POINT    g_lastCursor    = {};
bool     g_wasForeground = false;
uint64_t g_lastActivity  = 0;
uint64_t g_lastDraw      = 0;
uint64_t g_redrawUntil   = 0;

uint64_t nowMs() {
    return static_cast<uint64_t>(GetTickCount64());
}
#else
uint64_t g_redrawUntil = 0;
uint64_t nowMs() { return 0; }
#endif

// argv has to outlive QApplication: it keeps the pointer, and Qt reads it
// again during shutdown. A local array in init() would be long gone by then.
int    g_argc = 0;
char** g_argv = nullptr;

void refreshDevices() {
    g_devices.clear();
    if (!g_discovery) return;
    for (const auto& s : g_discovery->discoveredServices()) {
        Device d;
        d.name = s.name.toStdString();
        d.host = s.host.toStdString();
        d.port = s.port;
        g_devices.push_back(std::move(d));
    }
}

} // namespace

void requestRedraw();   // defined below, used by init's connections

bool init(int argc, char** argv) {
    if (g_app) return true;

    g_argc = argc;
    g_argv = argv;

    // QApplication rather than QCoreApplication, so widgets remain available
    // and the existing receive window keeps working untouched.
    g_app = std::make_unique<QApplication>(g_argc, g_argv);
    QCoreApplication::setApplicationName("BetterCast");
    QCoreApplication::setOrganizationName("BetterCast");

    QObject::connect(&LogManager::instance(), &LogManager::logAdded,
                     [](const QString& entry) {
                         g_lastLog = entry.toStdString();
                         requestRedraw();
                     });

    g_discovery = std::make_unique<ServiceDiscovery>();
    QObject::connect(g_discovery.get(), &ServiceDiscovery::serviceFound,
                     [](const DiscoveredService&) { refreshDevices(); requestRedraw(); });
    QObject::connect(g_discovery.get(), &ServiceDiscovery::serviceLost,
                     [](const QString&) { refreshDevices(); requestRedraw(); });
    g_discovery->startBrowsing();

    LogManager::instance().log("Glass: BetterCast core running inside the D3D11 loop");
    return true;
}

void pump() {
    if (!g_app) return;

    // Bounded rather than open-ended: an unbounded processEvents can service
    // work faster than it arrives and stall the frame it was called from.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 4 /* ms */);
}

void requestRedraw() {
    g_redrawUntil = nowMs() + 400;   // a short burst, so an update animates in
}

bool shouldRender() {
#ifdef _WIN32
    const HWND active = GetActiveWindow();
    const bool foreground = active != nullptr && active == GetForegroundWindow();

    // Any cursor movement or button held counts as interaction. Polling this
    // avoids patching the upstream message loop to observe input.
    POINT cursor = {};
    GetCursorPos(&cursor);
    const bool moved = cursor.x != g_lastCursor.x || cursor.y != g_lastCursor.y;
    g_lastCursor = cursor;

    const bool pressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                         (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    const uint64_t now = nowMs();
    if (moved || pressed || foreground != g_wasForeground) {
        g_lastActivity = now;
    }
    g_wasForeground = foreground;

    const bool interacting = foreground && (now - g_lastActivity) < 700;
    const bool wanted      = now < g_redrawUntil;

    if (interacting || wanted) {
        g_frameCap = 60;
        return true;
    }

    // Idle. Draw occasionally rather than never: a clock, a device appearing
    // or a stream ending should still show up without the user touching
    // anything, and a window that never repaints looks hung after a restore.
    const int intervalMs = foreground ? 200 : 1000;   // 5fps vs 1fps
    g_frameCap = 1000 / intervalMs;

    if (now - g_lastDraw >= (uint64_t)intervalMs) {
        g_lastDraw = now;
        return true;
    }

    // Sleep so the caller can skip without spinning. Short enough that input
    // still feels immediate: the next poll is at most 16ms away.
    Sleep(16);
    return false;
#else
    return true;
#endif
}

int currentFrameCap() {
    return g_frameCap;
}

void shutdown() {
    g_probe.reset();
    if (g_discovery) {
        g_discovery->stopBrowsing();
        g_discovery.reset();
    }
    g_app.reset();
}

const std::vector<Device>& devices() {
    return g_devices;
}

bool toggleProbeWindow() {
    if (!g_app) return false;

    if (g_probe) {
        g_probe.reset();
        LogManager::instance().log("Glass: closed the OpenGL probe window");
        return true;
    }

    // A bare QOpenGLWidget is enough to answer the question: the receive path
    // renders through one of these, and it has to survive alongside a D3D11
    // swapchain in the same process - on this hardware that means an Intel
    // panel driver and an NVIDIA card in the same address space.
    g_probe = std::make_unique<QOpenGLWidget>();
    g_probe->setWindowTitle("BetterCast - OpenGL coexistence probe");
    g_probe->resize(480, 270);
    g_probe->show();
    LogManager::instance().log("Glass: opened an OpenGL window beside the D3D11 one");
    return true;
}

bool probeWindowOpen() {
    return g_probe != nullptr;
}

std::string lastLogLine() {
    return g_lastLog;
}

} // namespace BetterCastBridge
