#include "BetterCastBridge.h"

#include "AudioDecoder.h"
#include "AudioPlayer.h"
#include "LogManager.h"
#include "NetworkListener.h"
#include "ServiceDiscovery.h"
#include "VideoDecoder.h"
#include "VideoRenderer.h"
#include "Language.h"
#include "UpdateChecker.h"
#ifdef ENABLE_SENDER
#include "sender/SenderController.h"
#endif

#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QSettings>
#include <QSurfaceFormat>
#include <QSysInfo>
#include <QUrl>
#include <QCoreApplication>
#include <QOpenGLWidget>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace BetterCastBridge {

// Declared up here rather than below the helpers, because the helpers call
// them: anything inside the anonymous namespace that asks for a frame needs
// requestRedraw to already be a name.
void requestRedraw();   // defined below, used by init's connections
int  sessionCount();    // defined below, used by the render throttle

namespace {

std::unique_ptr<QApplication>     g_app;
std::unique_ptr<ServiceDiscovery> g_discovery;
std::unique_ptr<QOpenGLWidget>    g_probe;
std::unique_ptr<NetworkListener>  g_network;
std::unique_ptr<VideoDecoder>     g_decoder;
std::unique_ptr<VideoRenderer>    g_renderer;   // parentless: its own window
std::unique_ptr<AudioDecoder>     g_audioDecoder;
std::unique_ptr<AudioPlayer>      g_audioPlayer;
std::unique_ptr<UpdateChecker>    g_updates;
std::string g_updateStatus;
std::string g_updateUrl;
#ifdef ENABLE_SENDER
std::unique_ptr<SenderController> g_sender;
#endif

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

    const auto& found = g_discovery->discoveredServices();

    // Names Apple advertises twice: a Wi-Fi service and an AWDL "<name> P2P"
    // one. macOS hides the P2P copy when the base device is present; without
    // that both appear and the P2P one is the broken half.
    auto hasBase = [&found](const QString& p2pName) {
        const QString base = p2pName.left(p2pName.length() - 4);   // strip " P2P"
        for (const auto& o : found) {
            if (o.name == base) return true;
        }
        return false;
    };

    for (const auto& s : found) {
        if (s.name.endsWith(" P2P") && hasBase(s.name)) continue;

        // An AWDL target is a .local hostname on a link Windows has no route
        // to, so it never resolves - "Host not found" is the whole story of
        // the iPhone failing to connect. Skip anything that is not an address
        // we can actually dial.
        const QHostAddress addr(s.host);
        if (addr.isNull()) continue;

        Device d;
        d.name = s.name.toStdString();
        d.host = s.host.toStdString();
        // Every BetterCast receiver listens on 51820. mDNS may advertise the
        // P2P listener's dynamic port instead, which is unreachable from here.
        // MainWindow has forced this for the Qt app all along; the bridge was
        // reading the raw SRV record and dialling 57510.
        d.port = 51820;
        g_devices.push_back(std::move(d));
    }
}

// The display device name of the screen the user is looking at, e.g.
// "\\.\DISPLAY1", for mirroring.
//
// Asked of Windows rather than inferred from the monitor list: the primary is
// whichever adapter carries the PRIMARY_DEVICE flag, which is not reliably the
// first one, and picking the wrong screen would mirror the wrong desktop.
QString mainDisplayName() {
#ifdef _WIN32
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        const bool attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        const bool primary  = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        if (attached && primary) return QString::fromWCharArray(dd.DeviceName);
        dd.cb = sizeof(dd);
    }
#endif
    return QString();
}

// ── Android over USB ─────────────────────────────────────────────────────

std::vector<AndroidDevice> g_androids;
std::string                g_androidStatus;
bool                       g_androidWatching = false;
QProcess*                  g_adb       = nullptr;   // one probe at a time
QTimer*                    g_adbTimer  = nullptr;

// scrcpy and adb, bundled in a folder beside the exe. Falls back to PATH, so
// a machine that already has the Android tools installed works too.
QString androidTool(const QString& exe) {
    const QString bundled =
        QCoreApplication::applicationDirPath() + "/scrcpy/" + exe;
    if (QFileInfo::exists(bundled)) return bundled;

    QString base = exe;
    if (base.endsWith(".exe", Qt::CaseInsensitive)) base.chop(4);
    return QStandardPaths::findExecutable(base);
}

void parseAdbDevices(const QString& out) {
    std::vector<AndroidDevice> found;
    const QStringList lines = out.split(QChar('\n'), Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        // The header, and the noise adb prints while starting its daemon.
        if (line.isEmpty()) continue;
        if (line.startsWith("List of devices")) continue;
        if (line.startsWith('*')) continue;

        const QStringList parts = line.split(QRegularExpression("\\s+"),
                                             Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;

        AndroidDevice d;
        d.serial = parts[0].toStdString();
        d.state  = parts[1].toStdString();
        // "model:Pixel_7" when adb was asked for the long listing. Underscores
        // are adb's, not the owner's, so they read better as spaces.
        for (int i = 2; i < parts.size(); i++) {
            if (!parts[i].startsWith("model:")) continue;
            QString m = parts[i].mid(6);
            m.replace(QChar('_'), QChar(' '));
            d.model = m.toStdString();
            break;
        }
        if (d.model.empty()) d.model = d.serial;
        found.push_back(std::move(d));
    }
    g_androids.swap(found);
}

void probeAndroid() {
    if (g_adb) return;   // a probe is already out

    const QString adb = androidTool("adb.exe");
    if (adb.isEmpty()) {
        g_androidStatus = "adb was not found beside BetterCast or on this PC";
        return;
    }

    // The lambdas hold the process they belong to rather than reading the
    // global. errorOccurred and finished can both fire, and by the time the
    // second arrives the global may already point at the next probe - acting
    // on that one would clear a result that has not happened yet.
    QProcess* p = new QProcess();
    g_adb = p;

    QObject::connect(p, &QProcess::finished,
                     [p](int code, QProcess::ExitStatus) {
                         const QString out =
                             QString::fromUtf8(p->readAllStandardOutput());
                         if (code == 0) {
                             parseAdbDevices(out);
                             g_androidStatus = g_androids.empty()
                                 ? std::string("no Android device on the cable yet")
                                 : "adb sees " + std::to_string(g_androids.size()) +
                                   (g_androids.size() == 1 ? " device" : " devices");
                         } else {
                             g_androidStatus = "adb exited with " + std::to_string(code);
                         }
                         if (g_adb == p) g_adb = nullptr;
                         p->deleteLater();
                         requestRedraw();
                     });
    QObject::connect(p, &QProcess::errorOccurred,
                     [p](QProcess::ProcessError) {
                         g_androidStatus = "adb could not be started";
                         if (g_adb == p) g_adb = nullptr;
                         p->deleteLater();
                         requestRedraw();
                     });
    p->start(adb, QStringList() << "devices" << "-l");
}

// Settings for one device, under a key of its own.
QString deviceKey(const std::string& name) {
    // A device name is free text - it can contain anything the owner typed,
    // including the slashes QSettings reads as group separators. Percent
    // encoding keeps one device's settings from landing inside another's.
    return QString::fromUtf8(
        QUrl::toPercentEncoding(QString::fromStdString(name)));
}

} // namespace

bool init(int argc, char** argv) {
    if (g_app) return true;

    g_argc = argc;
    g_argv = argv;

    // Before QApplication, and the reason received video came through black.
    //
    // VideoRenderer uploads NV12 through fixed-function paths that a Core
    // profile context does not have; main.cpp of the Qt app sets this for
    // exactly that reason and says so. Without it the renderer gets whatever
    // Qt defaults to, the texture upload quietly fails and every frame paints
    // black - decoded fine, displayed as nothing.
    QSurfaceFormat fmt;
    fmt.setVersion(2, 1);
    fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);

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
    // Receive side. Without this Windows browses but never announces itself,
    // so it stops appearing on other devices as a screen they can extend to -
    // which is what the shipping app does at startup and the first version of
    // this bridge quietly dropped.
    g_decoder  = std::make_unique<VideoDecoder>();
    g_renderer = std::make_unique<VideoRenderer>();
    g_renderer->setWindowTitle("BetterCast - Receiving");
    g_renderer->resize(1280, 720);

    // Sound as well as picture. The macOS sender encodes system audio as AAC
    // and sends it down the same socket tagged 0x02; without a decoder wired
    // in, NetworkListener drops those packets and the stream arrives silent.
    // The Qt app has always done this - the bridge passed only the video half.
    g_audioDecoder = std::make_unique<AudioDecoder>();
    g_audioPlayer  = std::make_unique<AudioPlayer>();
    QObject::connect(g_audioDecoder.get(), &AudioDecoder::pcmDecoded,
                     g_audioPlayer.get(), &AudioPlayer::onPcmDecoded);

    g_network = std::make_unique<NetworkListener>();
    g_network->setup(g_decoder.get(), g_renderer.get(), g_audioDecoder.get());
    g_network->start();

    const uint16_t port = g_network->actualTcpPort();
    g_discovery->startAdvertising(port);
    g_discovery->startBrowsing();

    // Show the video window only while something is actually sending, the way
    // the Qt app opens and closes it on connect.
    QObject::connect(g_network.get(), &NetworkListener::connectionEstablished,
                     []() { if (g_renderer) g_renderer->show(); requestRedraw(); });
    QObject::connect(g_network.get(), &NetworkListener::connectionLost,
                     []() { if (g_renderer) g_renderer->hide(); requestRedraw(); });
    QObject::connect(g_network.get(), &NetworkListener::statusChanged,
                     [](const QString& m) { LogManager::instance().log(m); });

    LogManager::instance().log(
        QString("Glass: listening on port %1 and advertising to the network").arg(port));

#ifdef ENABLE_SENDER
    g_sender = std::make_unique<SenderController>();
    // Every one of these changes something the render loop cannot see, so each
    // has to ask for a frame or the UI sits on a stale picture until the user
    // happens to move the mouse.
    QObject::connect(g_sender.get(), &SenderController::sessionsChanged,
                     []() { requestRedraw(); });
    QObject::connect(g_sender.get(), &SenderController::error,
                     [](const QString& m) { LogManager::instance().log("Sender error: " + m); });
    QObject::connect(g_sender.get(), &SenderController::statusChanged,
                     [](const QString& m) { LogManager::instance().log(m); });
#endif

    LogManager::instance().log("Glass: BetterCast core running inside the D3D11 loop");
    return true;
}

void pump() {
    if (!g_app) return;

    // Drain the queue, do not ration it.
    //
    // This used to cap at 4ms, on the theory that an unbounded processEvents
    // could stall the frame. That was wrong twice over: processEvents returns
    // as soon as the queue is empty, so there is nothing to run away with, and
    // the cap was actively harmful. Captured frames reach the encoder and the
    // socket through queued signals delivered on this thread, so 4ms out of
    // every 16 gave the whole streaming pipeline a 25% duty cycle. Three
    // receivers at 60fps is 180 frames a second through that gate, and the
    // backlog showed up as cursor lag that grew the longer a stream ran while
    // the picture itself stayed clean - a queue draining slower than it fills,
    // not a dropped-frame problem.
    QCoreApplication::processEvents(QEventLoop::AllEvents);
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

    // Sleep so the caller can skip without spinning.
    //
    // Much shorter while streaming: the sleep sets how often pump() runs, and
    // pump() is what moves captured frames to the encoder and the socket. At
    // 16ms an idle-looking window still throttles a live stream to 60 drains a
    // second; at 4ms it drains 250 times, which is comfortably ahead of three
    // receivers at 60fps. Idle with nothing streaming keeps the long sleep,
    // since that is where the GPU saving comes from.
    const bool streaming = sessionCount() > 0;
    Sleep(streaming ? 4 : 16);
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

    // Before Qt goes: a timer that fires during teardown would start an adb
    // process nobody is left to collect. g_adb is only non-null while no
    // deleteLater is pending for it, so deleting it here cannot double up.
    if (g_adbTimer) {
        g_adbTimer->stop();
        delete g_adbTimer;
        g_adbTimer = nullptr;
    }
    if (g_adb) {
        g_adb->kill();
        g_adb->waitForFinished(1000);
        delete g_adb;
        g_adb = nullptr;
    }

#ifdef ENABLE_SENDER
    if (g_sender) {
        g_sender->stopAll();       // joins the capture threads before Qt goes
        g_sender.reset();
    }
#endif
    if (g_discovery) {
        g_discovery->stopBrowsing();
        g_discovery->stopAdvertising();
        g_discovery.reset();
    }
    g_network.reset();          // before the decoders it feeds
    g_renderer.reset();
    g_decoder.reset();
    g_audioPlayer.reset();
    g_audioDecoder.reset();
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

// -- Identity -------------------------------------------------------------

std::string userName() {
    QSettings s("BetterCast", "BetterCast");
    // Falls back to the machine name, which is what mDNS advertises anyway, so
    // an untouched install shows something true rather than a placeholder.
    const QString fallback = QString("%1 (Windows)").arg(QSysInfo::machineHostName());
    return s.value("identity/name", fallback).toString().toStdString();
}

void setUserName(const std::string& name) {
    QSettings s("BetterCast", "BetterCast");
    s.setValue("identity/name", QString::fromStdString(name));
    requestRedraw();
}

std::string userHandle() {
    // The address other devices reach this machine on is more use under the
    // name than an invented handle.
    for (const QHostAddress& a : QNetworkInterface::allAddresses()) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) {
            return a.toString().toStdString();
        }
    }
    return "no network";
}

std::string userInitials() {
    const QString n = QString::fromStdString(userName()).trimmed();
    QString out;
    const QStringList parts = n.split(QChar(' '), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (!part.isEmpty() && part.at(0).isLetterOrNumber()) out += part.at(0).toUpper();
        if (out.size() == 2) break;
    }
    return out.isEmpty() ? std::string("BC") : out.toStdString();
}

// -- Settings -------------------------------------------------------------

std::string appVersion() {
    return UpdateChecker::currentVersion().toStdString();
}

std::string logFilePath() {
    return LogManager::instance().logFilePath().toStdString();
}

std::vector<std::pair<std::string, std::string>> languages() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : Language::available()) {
        out.emplace_back(e.code.toStdString(), e.endonym.toStdString());
    }
    return out;
}

std::string savedLanguage() {
    return Language::savedCode().toStdString();
}

void setSavedLanguage(const std::string& code) {
    Language::setSavedCode(QString::fromStdString(code));
    LogManager::instance().log("Settings: language applies on next launch");
}

void checkForUpdates() {
    if (!g_updates) {
        g_updates = std::make_unique<UpdateChecker>();
        QObject::connect(g_updates.get(), &UpdateChecker::finished,
                         [](bool available, const QString& tag, const QString& url, const QString&) {
            g_updateUrl = url.toStdString();
            g_updateStatus = available
                ? QString("Update available: %1").arg(tag).toStdString()
                : QString("Up to date (%1)").arg(UpdateChecker::currentTag()).toStdString();
            requestRedraw();
        });
        QObject::connect(g_updates.get(), &UpdateChecker::failed, [](const QString& why) {
            // Quietly: BetterCast is often used on an isolated hotspot with no
            // internet, where a failed check is expected rather than wrong.
            g_updateStatus = QString("Could not check: %1").arg(why).toStdString();
            requestRedraw();
        });
    }
    g_updateStatus = "Checking...";
    g_updates->check();
}

std::string updateStatus() { return g_updateStatus; }
std::string updateUrl()    { return g_updateUrl; }

void openUrl(const std::string& url) {
    QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
}

void* appIconHandle() {
#ifdef _WIN32
    static HICON icon = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // Beside the executable, as the workflow ships it. LR_LOADFROMFILE
        // avoids needing a resource script in a project whose build is patched
        // together rather than owned.
        const QString path =
            QFileInfo(QCoreApplication::applicationDirPath() + "/appicon.ico").absoluteFilePath();
        icon = (HICON)LoadImageW(nullptr, path.toStdWString().c_str(), IMAGE_ICON,
                                 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (!icon) LogManager::instance().log("Glass: appicon.ico not found beside the exe");
    }
    return icon;
#else
    return nullptr;
#endif
}

// ── Streaming ────────────────────────────────────────────────────────────

bool startExtending(const std::string& host, uint16_t port,
                    int fps, int bitrateMbps, int width, int height) {
#ifdef ENABLE_SENDER
    if (!g_sender) return false;
    // Empty display name: the controller claims a virtual display for this
    // receiver, which is what extending means.
    const bool ok = g_sender->startSending(QString::fromStdString(host), port,
                                           fps, bitrateMbps, QString(), width, height);
    requestRedraw();
    return ok;
#else
    (void)host; (void)port; (void)fps; (void)bitrateMbps; (void)width; (void)height;
    return false;
#endif
}

bool startMirroring(const std::string& host, uint16_t port,
                    int fps, int bitrateMbps) {
#ifdef ENABLE_SENDER
    if (!g_sender) return false;
    const QString screen = mainDisplayName();
    if (screen.isEmpty()) {
        LogManager::instance().log("Glass: could not work out which display is the main "
                                   "one, so there is nothing to mirror");
        return false;
    }
    // Naming a real monitor is how the controller is told to mirror: it
    // captures that screen as it is, rather than claiming a virtual display
    // and resizing it. Size is deliberately not passed - the screen is already
    // whatever size it is, and changing that would resize the desktop.
    const bool ok = g_sender->startSending(QString::fromStdString(host), port,
                                           fps, bitrateMbps, screen, 0, 0);
    requestRedraw();
    return ok;
#else
    (void)host; (void)port; (void)fps; (void)bitrateMbps;
    return false;
#endif
}

bool isMirroring() {
#ifdef ENABLE_SENDER
    if (!g_sender) return false;
    const QString screen = mainDisplayName();
    if (screen.isEmpty()) return false;
    for (const QString& host : g_sender->activeReceivers()) {
        if (g_sender->displayForReceiver(host).compare(screen, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
#else
    return false;
#endif
}

void stopSending(const std::string& host) {
#ifdef ENABLE_SENDER
    if (g_sender) g_sender->stopSending(QString::fromStdString(host));
    requestRedraw();
#else
    (void)host;
#endif
}

bool isSendingTo(const std::string& host) {
#ifdef ENABLE_SENDER
    return g_sender && g_sender->isSendingTo(QString::fromStdString(host));
#else
    (void)host; return false;
#endif
}

int sessionCount() {
#ifdef ENABLE_SENDER
    return g_sender ? g_sender->sessionCount() : 0;
#else
    return 0;
#endif
}

std::string displayForReceiver(const std::string& host) {
#ifdef ENABLE_SENDER
    if (!g_sender) return {};
    return g_sender->displayForReceiver(QString::fromStdString(host)).toStdString();
#else
    (void)host; return {};
#endif
}

std::string encoderInfo() {
#ifdef ENABLE_SENDER
    return g_sender ? g_sender->encoderInfo().toStdString() : std::string();
#else
    return {};
#endif
}

// ── Per-device stream settings ───────────────────────────────────────────

StreamSettings settingsFor(const std::string& deviceName) {
    StreamSettings out;
    if (deviceName.empty()) return out;
    QSettings s("BetterCast", "BetterCast");
    const QString k = "devices/" + deviceKey(deviceName) + "/";
    out.fps         = s.value(k + "fps",     out.fps).toInt();
    out.bitrateMbps = s.value(k + "bitrate", out.bitrateMbps).toInt();
    out.width       = s.value(k + "width",   out.width).toInt();
    out.height      = s.value(k + "height",  out.height).toInt();
    return out;
}

void setSettingsFor(const std::string& deviceName, const StreamSettings& v) {
    if (deviceName.empty()) return;
    QSettings s("BetterCast", "BetterCast");
    const QString k = "devices/" + deviceKey(deviceName) + "/";
    s.setValue(k + "fps",     v.fps);
    s.setValue(k + "bitrate", v.bitrateMbps);
    s.setValue(k + "width",   v.width);
    s.setValue(k + "height",  v.height);
}

// ── Android over USB ─────────────────────────────────────────────────────

bool androidToolsPresent() {
    return !androidTool("scrcpy.exe").isEmpty() && !androidTool("adb.exe").isEmpty();
}

void watchAndroid(bool on) {
    g_androidWatching = on;
    if (!on) {
        if (g_adbTimer) g_adbTimer->stop();
        g_androids.clear();
        g_androidStatus.clear();
        requestRedraw();
        return;
    }

    if (!g_adbTimer) {
        g_adbTimer = new QTimer();
        QObject::connect(g_adbTimer, &QTimer::timeout, []() { probeAndroid(); });
    }
    // Often enough that plugging a cable in feels immediate, rarely enough
    // that this is not spawning a process every frame.
    g_adbTimer->start(3000);
    probeAndroid();
    requestRedraw();
}

bool androidWatching() {
    return g_androidWatching;
}

const std::vector<AndroidDevice>& androidDevices() {
    return g_androids;
}

bool mirrorAndroid(const std::string& serial) {
    const QString exe = androidTool("scrcpy.exe");
    if (exe.isEmpty()) {
        g_androidStatus = "scrcpy was not found beside BetterCast or on this PC";
        return false;
    }

    QStringList args;
    if (!serial.empty()) args << "-s" << QString::fromStdString(serial);
    args << "--window-title" << "BetterCast - Android";

    // Its own process and its own window, not a widget inside this one.
    // scrcpy pushes a server to the phone and decodes the stream itself;
    // wrapping that would mean reimplementing it, and it already works.
    //
    // Started in its own directory because that is where it looks for
    // scrcpy-server, which it has to push to the phone before anything
    // appears.
    const QString dir = QFileInfo(exe).absolutePath();
    const bool ok = QProcess::startDetached(exe, args, dir);
    g_androidStatus = ok ? "scrcpy started" : "scrcpy could not be started";
    LogManager::instance().log("Glass: " + QString::fromStdString(g_androidStatus) +
                               (serial.empty() ? QString()
                                               : " for " + QString::fromStdString(serial)));
    requestRedraw();
    return ok;
}

std::string androidStatus() {
    return g_androidStatus;
}

} // namespace BetterCastBridge
