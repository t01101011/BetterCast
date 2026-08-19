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
                     [](const QString& entry) { g_lastLog = entry.toStdString(); });

    g_discovery = std::make_unique<ServiceDiscovery>();
    QObject::connect(g_discovery.get(), &ServiceDiscovery::serviceFound,
                     [](const DiscoveredService&) { refreshDevices(); });
    QObject::connect(g_discovery.get(), &ServiceDiscovery::serviceLost,
                     [](const QString&) { refreshDevices(); });
    g_discovery->startBrowsing();

    LogManager::instance().log("Glass: BetterCast core running inside the D3D11 loop");
    return true;
}

void pump() {
    if (!g_app) return;

    // Bounded rather than open-ended: an unbounded processEvents can service
    // work faster than it arrives and stall the frame it was called from.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 4 /* ms */);

#ifdef _WIN32
    // Throttle when nobody is looking.
    //
    // GetActiveWindow() is this thread's active window and GetForegroundWindow()
    // is the system's; equal means we are the window in front. Comparing them
    // avoids having to thread the HWND through from a patched main().
    //
    // Sleeping here rather than skipping the frame keeps the loop structure
    // upstream untouched, and Qt has already been serviced above so discovery
    // and sockets stay responsive at any frame rate.
    const HWND active = GetActiveWindow();
    const bool foreground = active != nullptr && active == GetForegroundWindow();

    g_frameCap = foreground ? 60 : 15;
    if (!foreground) {
        Sleep(50);
    }
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
