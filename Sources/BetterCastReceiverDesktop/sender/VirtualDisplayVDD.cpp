#include "VirtualDisplayVDD.h"
#include "../MainWindow.h"  // LogManager

#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QSet>
#include <QThread>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

// Log to both qDebug and the in-app LogManager
#define VDD_LOG(msg) do { \
    QString _m = (msg); \
    qDebug().noquote() << _m; \
    LogManager::instance().log(_m); \
} while(0)

#ifdef _WIN32
#include <Windows.h>
#include <dxgi.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <cfgmgr32.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")   // CCD: QueryDisplayConfig / SetDisplayConfig

#ifndef DISPLAYCONFIG_PATH_MODE_IDX_INVALID
#define DISPLAYCONFIG_PATH_MODE_IDX_INVALID 0xffffffff
#endif
#endif

// ─── CCD (Connecting and Configuring Displays) helpers ────────────────────────
#ifdef _WIN32
namespace {

// Keywords shared with the DXGI/GDI enumeration paths below.
bool looksVirtual(const QString& text) {
    static const QStringList kKeys = {"virtual", "indirect", "idd", "vdd", "mtt"};
    QString lower = text.toLower();
    for (const auto& k : kKeys) {
        if (lower.contains(k)) return true;
    }
    return false;
}

bool isVirtualTarget(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = path.targetInfo.adapterId;
    name.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return false;

    if (looksVirtual(QString::fromWCharArray(name.monitorFriendlyDeviceName)) ||
        looksVirtual(QString::fromWCharArray(name.monitorDevicePath))) {
        return true;
    }
    // IddCx targets have no real connector, so they report OTHER.
    return name.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
}

// GetDisplayConfigBufferSizes → QueryDisplayConfig, retrying the race where a
// display is hotplugged between the two calls.
bool queryPaths(QVector<DISPLAYCONFIG_PATH_INFO>& paths,
                QVector<DISPLAYCONFIG_MODE_INFO>& modes,
                UINT32 flags) {
    for (int attempt = 0; attempt < 5; attempt++) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return false;
        }
        if (pathCount == 0 || modeCount == 0) return false;  // no displays to reason about
        paths.resize(static_cast<int>(pathCount));
        modes.resize(static_cast<int>(modeCount));

        LONG r = QueryDisplayConfig(flags, &pathCount, paths.data(),
                                    &modeCount, modes.data(), nullptr);
        if (r == ERROR_SUCCESS) {
            paths.resize(static_cast<int>(pathCount));
            modes.resize(static_cast<int>(modeCount));
            return true;
        }
        if (r != ERROR_INSUFFICIENT_BUFFER) return false;  // only the race is retryable
    }
    return false;
}

QString sourceKey(const DISPLAYCONFIG_PATH_INFO& p) {
    return QString("%1:%2:%3").arg(p.sourceInfo.adapterId.HighPart)
                              .arg(p.sourceInfo.adapterId.LowPart)
                              .arg(p.sourceInfo.id);
}

QString targetKey(const DISPLAYCONFIG_PATH_INFO& p) {
    return QString("%1:%2:%3").arg(p.targetInfo.adapterId.HighPart)
                              .arg(p.targetInfo.adapterId.LowPart)
                              .arg(p.targetInfo.id);
}

} // namespace
#endif

// Known VDD installation paths (static fallbacks)
static const QStringList kVddPathsBase = {
    "C:/VirtualDisplayDriver",
    "C:/Program Files/Virtual Display Driver",
    "C:/Program Files/VirtualDisplayDriver",
    "C:/IddSampleDriver",
};

// Build full path list at runtime (QCoreApplication must exist)
static QStringList getVddPaths() {
    QStringList paths;
    // Check next to the app executable first (NSIS installs VDD here)
    QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        paths.append(appDir + "/VirtualDisplayDriver");
    }
    paths.append(kVddPathsBase);
    return paths;
}

// VDD settings file names (varies by version)
static const QStringList kSettingsFiles = {
    "vdd_settings.xml",
    "settings.xml",
    "config.xml",
    "option.txt",
    "options.xml",
};

// VDD named pipe (modern versions)
static const char* kVddPipeName = "\\\\.\\pipe\\VDDPipe";

// VDD hardware IDs to look for
static const QStringList kVddHardwareIds = {
    "Root\\VirtualDisplayDriver",
    "Root\\IddSampleDriver",
    "Root\\VDD",
    "VDD",
    "IddSampleDriver",
    "MttVDD",
};

VirtualDisplayVDD::VirtualDisplayVDD(QObject* parent)
    : QObject(parent)
{
    m_vddInstalled = detectVddInstall();
    if (m_vddInstalled) {
        VDD_LOG("VDD: Found installation at " + m_vddPath);
    } else {
        VDD_LOG("VDD: Not installed — checked registry, known paths, services, and devices");
    }
}

VirtualDisplayVDD::~VirtualDisplayVDD() {
    // Clean up any virtual displays we created
    if (m_createdDisplayCount > 0) {
        removeAllVirtualDisplays();
    }
}

bool VirtualDisplayVDD::isVddInstalled() const {
    return m_vddInstalled;
}

QString VirtualDisplayVDD::vddInstallPath() const {
    return m_vddPath;
}

void VirtualDisplayVDD::refreshInstallStatus() {
    bool wasInstalled = m_vddInstalled;
    m_vddPath.clear();
    m_vddInstalled = detectVddInstall();
    if (m_vddInstalled && !wasInstalled) {
        VDD_LOG("VDD: Now detected at " + m_vddPath);
        emit statusChanged("Virtual Display Driver detected");
    } else if (!m_vddInstalled) {
        VDD_LOG("VDD: Still not detected after refresh");
    }
}

bool VirtualDisplayVDD::detectVddInstall() {
    VDD_LOG("VDD: Starting detection...");

    // Method 0: Check BetterCast's own bundled VDD path (set by installer)
#ifdef _WIN32
    {
        HKEY hKey;
        LONG result = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE, L"Software\\BetterCast",
            0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS) {
            wchar_t vddPath[MAX_PATH] = {};
            DWORD size = sizeof(vddPath);
            result = RegQueryValueExW(hKey, L"VDDPath", nullptr, nullptr,
                                       reinterpret_cast<LPBYTE>(vddPath), &size);
            RegCloseKey(hKey);
            if (result == ERROR_SUCCESS) {
                QString path = QString::fromWCharArray(vddPath);
                VDD_LOG("VDD [Method 0]: Registry key found, path=" + path);
                if (QDir(path).exists()) {
                    m_vddPath = path;
                    VDD_LOG("VDD [Method 0]: Directory exists — detected via registry");
                    return true;
                } else {
                    VDD_LOG("VDD [Method 0]: Directory does NOT exist");
                }
            } else {
                VDD_LOG("VDD [Method 0]: Registry key exists but VDDPath value not found");
            }
        } else {
            VDD_LOG("VDD [Method 0]: No HKLM\\Software\\BetterCast registry key");
        }
    }
#endif

    // Method 1: Check known installation paths
    for (const auto& basePath : getVddPaths()) {
        QDir dir(basePath);
        if (dir.exists()) {
            VDD_LOG("VDD [Method 1]: Directory exists: " + basePath);
            // Verify there's actually a driver or settings file here
            for (const auto& settingsFile : kSettingsFiles) {
                if (QFileInfo::exists(basePath + "/" + settingsFile)) {
                    m_vddPath = basePath;
                    VDD_LOG("VDD [Method 1]: Found settings file " + settingsFile + " in " + basePath);
                    return true;
                }
            }
            // Check for driver files even without settings
            if (QFileInfo::exists(basePath + "/VirtualDisplayDriver.dll") ||
                QFileInfo::exists(basePath + "/IddSampleDriver.dll") ||
                QFileInfo::exists(basePath + "/MttVDD.dll")) {
                m_vddPath = basePath;
                VDD_LOG("VDD [Method 1]: Found driver DLL in " + basePath);
                return true;
            }
            // Check for driver .inf files
            if (QFileInfo::exists(basePath + "/VirtualDisplayDriver.inf") ||
                QFileInfo::exists(basePath + "/MttVDD.inf")) {
                m_vddPath = basePath;
                VDD_LOG("VDD [Method 1]: Found driver .inf in " + basePath);
                return true;
            }
            // Check for VDD Control exe (newer versions — filename may use space or dot)
            if (QFileInfo::exists(basePath + "/VDD Control.exe") ||
                QFileInfo::exists(basePath + "/VDD.Control.exe")) {
                m_vddPath = basePath;
                VDD_LOG("VDD [Method 1]: Found VDD.Control.exe in " + basePath);
                return true;
            }
            // List what IS in the directory for debugging
            QStringList files = dir.entryList(QDir::Files);
            VDD_LOG("VDD [Method 1]: Files in " + basePath + ": " +
                    (files.isEmpty() ? "(empty)" : files.join(", ")));
        }
    }
    VDD_LOG("VDD [Method 1]: No known paths matched");

#ifdef _WIN32
    // Method 2: Check registry for VDD driver service
    static const wchar_t* serviceKeys[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\VirtualDisplayDriver",
        L"SYSTEM\\CurrentControlSet\\Services\\IddSampleDriver",
        L"SYSTEM\\CurrentControlSet\\Services\\MttVDD",
    };
    for (const auto* svcKey : serviceKeys) {
        HKEY hKey;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, svcKey, 0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS) {
            wchar_t imagePath[MAX_PATH] = {};
            DWORD size = sizeof(imagePath);
            result = RegQueryValueExW(hKey, L"ImagePath", nullptr, nullptr,
                                       reinterpret_cast<LPBYTE>(imagePath), &size);
            RegCloseKey(hKey);
            if (result == ERROR_SUCCESS) {
                QString path = QString::fromWCharArray(imagePath);
                QFileInfo fi(path);
                m_vddPath = fi.absolutePath();
                VDD_LOG("VDD [Method 2]: Found service at " + QString::fromWCharArray(svcKey) +
                        ", imagePath=" + path);
                return true;
            } else {
                VDD_LOG("VDD [Method 2]: Service key " + QString::fromWCharArray(svcKey) +
                        " exists but no ImagePath");
            }
        }
    }
    VDD_LOG("VDD [Method 2]: No VDD service found in registry");

    // Method 3: Check for VDD device via SetupDI
    HDEVINFO devInfo = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devInfo != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA devData = {};
        devData.cbSize = sizeof(devData);
        int deviceCount = 0;
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); i++) {
            wchar_t hwId[512] = {};
            if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devData,
                    SPDRP_HARDWAREID, nullptr,
                    reinterpret_cast<PBYTE>(hwId), sizeof(hwId), nullptr)) {
                QString id = QString::fromWCharArray(hwId).toLower();
                deviceCount++;
                for (const auto& vddId : kVddHardwareIds) {
                    if (id.contains(vddId.toLower())) {
                        SetupDiDestroyDeviceInfoList(devInfo);
                        if (m_vddPath.isEmpty()) {
                            for (const auto& p : getVddPaths()) {
                                if (QDir(p).exists()) { m_vddPath = p; break; }
                            }
                        }
                        VDD_LOG("VDD [Method 3]: Found device with hwId=" + id);
                        return true;
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        VDD_LOG(QString("VDD [Method 3]: Scanned %1 display devices, none matched VDD hardware IDs").arg(deviceCount));
    } else {
        VDD_LOG("VDD [Method 3]: SetupDiGetClassDevs failed");
    }
#endif

    return false;
}

bool VirtualDisplayVDD::isDriverLoaded() const {
#ifdef _WIN32
    // Check if any VDD service is registered
    static const wchar_t* serviceKeys[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\MttVDD",
        L"SYSTEM\\CurrentControlSet\\Services\\VirtualDisplayDriver",
        L"SYSTEM\\CurrentControlSet\\Services\\IddSampleDriver",
    };
    for (const auto* svcKey : serviceKeys) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, svcKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return true;
        }
    }

    // Also check via named pipe — if pipe exists, driver is running
    HANDLE pipe = CreateFileA(kVddPipeName, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        return true;
    }
#endif
    return false;
}

bool VirtualDisplayVDD::installDriver() {
#ifdef _WIN32
    if (m_vddPath.isEmpty()) return false;

    // Find devcon.exe (bundled in VDD directory or Dependencies subfolder)
    QString devconExe = m_vddPath + "/devcon.exe";
    if (!QFileInfo::exists(devconExe)) {
        devconExe = m_vddPath + "/Dependencies/devcon.exe";
    }

    // Find the MttVDD .inf file
    QString infPath;
    if (QFileInfo::exists(m_vddPath + "/MttVDD.inf")) {
        infPath = m_vddPath + "/MttVDD.inf";
    } else {
        // Fall back to first .inf found
        QDir vddDir(m_vddPath);
        QStringList infFiles = vddDir.entryList({"*.inf"}, QDir::Files);
        if (!infFiles.isEmpty()) {
            infPath = m_vddPath + "/" + infFiles.first();
        }
    }

    if (infPath.isEmpty()) {
        VDD_LOG("VDD: No .inf files found in " + m_vddPath);
        return false;
    }

    // Method 1: devcon install — creates device node + installs driver (preferred for IDD)
    if (QFileInfo::exists(devconExe)) {
        VDD_LOG("VDD: Installing via devcon: " + devconExe + " install " + infPath + " Root\\MttVDD");
        QProcess proc;
        proc.setProgram(devconExe);
        proc.setArguments({"install", infPath, "Root\\MttVDD"});
        proc.start();
        if (proc.waitForFinished(30000)) {
            QString output = proc.readAllStandardOutput() + proc.readAllStandardError();
            VDD_LOG("VDD: devcon output: " + output.trimmed());
            if (proc.exitCode() == 0) {
                VDD_LOG("VDD: Driver device created via devcon");
                QThread::msleep(2000);
                return true;
            }
        }
    } else {
        VDD_LOG("VDD: devcon.exe not found, trying pnputil...");
    }

    // Method 2: pnputil — adds driver to store (may not create device node for IDD)
    VDD_LOG("VDD: Attempting pnputil /add-driver \"" + infPath + "\" /install");
    QProcess proc;
    proc.setProgram("pnputil");
    proc.setArguments({"/add-driver", infPath, "/install"});
    proc.start();
    if (proc.waitForFinished(30000)) {
        QString output = proc.readAllStandardOutput() + proc.readAllStandardError();
        VDD_LOG("VDD: pnputil output: " + output.trimmed());
        if (proc.exitCode() == 0) {
            VDD_LOG("VDD: Driver added to store via pnputil");
        }
    }

    // After pnputil, try creating the device node explicitly
    if (QFileInfo::exists(devconExe)) {
        VDD_LOG("VDD: Creating device node via devcon...");
        QProcess devProc;
        devProc.setProgram(devconExe);
        devProc.setArguments({"install", infPath, "Root\\MttVDD"});
        devProc.start();
        if (devProc.waitForFinished(30000) && devProc.exitCode() == 0) {
            VDD_LOG("VDD: Device node created");
            QThread::msleep(2000);
            return true;
        }
    }

    VDD_LOG("VDD: All driver install methods failed — devcon.exe may be required");
#endif
    return false;
}

// ─── Virtual Display Management ────────────────────────────────────────────────

bool VirtualDisplayVDD::ensureVddControlRunning() {
#ifdef _WIN32
    // Check if pipe already exists (VDD Control is running)
    HANDLE pipe = CreateFileA(kVddPipeName, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        VDD_LOG("VDD: VDD Control is already running (pipe available)");
        return true;
    }

    // Try to launch VDD Control
    QString controlExe = m_vddPath + "/VDD Control.exe";
    if (!QFileInfo::exists(controlExe)) {
        controlExe = m_vddPath + "/VDD.Control.exe";
    }
    if (!QFileInfo::exists(controlExe)) {
        VDD_LOG("VDD: VDD Control.exe not found in " + m_vddPath);
        return false;
    }

    VDD_LOG("VDD: Starting VDD Control: " + controlExe);
    QProcess::startDetached(controlExe, {});

    // Wait for the pipe to become available (VDD Control needs startup time)
    for (int i = 0; i < 20; i++) {  // Up to 10 seconds
        QThread::msleep(500);
        pipe = CreateFileA(kVddPipeName, GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            VDD_LOG("VDD: VDD Control started, pipe available");
            return true;
        }
    }
    VDD_LOG("VDD: VDD Control started but pipe not available after 10s");
    return false;
#else
    return false;
#endif
}

bool VirtualDisplayVDD::activateVirtualDisplay() {
#ifdef _WIN32
    // Find an inactive/disconnected virtual display and extend the desktop to it
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        QString devString = QString::fromWCharArray(dd.DeviceString).toLower();
        bool isVirtual = devString.contains("virtual") || devString.contains("indirect") ||
                         devString.contains("idd") || devString.contains("vdd") ||
                         devString.contains("mtt");

        if (isVirtual && !(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            QString devName = QString::fromWCharArray(dd.DeviceName);
            VDD_LOG("VDD: Found disconnected virtual display: " + devName + " (" +
                    QString::fromWCharArray(dd.DeviceString) + ") — activating...");

            // Get the primary monitor's position to place virtual display to the right
            DEVMODEW primaryDm = {};
            primaryDm.dmSize = sizeof(primaryDm);
            EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &primaryDm);

            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
            dm.dmPosition.x = static_cast<LONG>(primaryDm.dmPelsWidth);  // Place to the right
            dm.dmPosition.y = 0;

            // Try to get the intended resolution from settings
            if (EnumDisplaySettingsW(dd.DeviceName, ENUM_REGISTRY_SETTINGS, &dm)) {
                dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
                dm.dmPosition.x = static_cast<LONG>(primaryDm.dmPelsWidth);
                dm.dmPosition.y = 0;
            } else {
                dm.dmPelsWidth = 1920;
                dm.dmPelsHeight = 1080;
            }

            LONG ret = ChangeDisplaySettingsExW(
                dd.DeviceName, &dm, nullptr,
                CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);

            if (ret == DISP_CHANGE_SUCCESSFUL) {
                // Apply changes globally
                ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
                VDD_LOG(QString("VDD: Virtual display activated at position %1,0 (%2x%3)")
                            .arg(dm.dmPosition.x).arg(dm.dmPelsWidth).arg(dm.dmPelsHeight));
                QThread::msleep(1000);
                return true;
            } else {
                VDD_LOG(QString("VDD: ChangeDisplaySettingsEx failed with code %1").arg(ret));
            }
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
    VDD_LOG("VDD: No disconnected virtual display found to activate");
#endif
    return false;
}

// ─── Display Topology (extend vs. mirror) ──────────────────────────────────────

QString VirtualDisplayVDD::TopologyState::describe() const {
    if (!valid) return QStringLiteral("unknown (query failed)");

    QString s = QString::number(activePaths) + " active path(s), ";
    s += anyCloned ? "MIRRORED" : "extended";
    if (!virtualActive)      s += ", no virtual display attached";
    else if (virtualCloned)  s += ", virtual display is mirrored";
    else                     s += ", virtual display has its own source";
    return s;
}

VirtualDisplayVDD::TopologyState VirtualDisplayVDD::queryTopology() const {
    TopologyState st;
#ifdef _WIN32
    QVector<DISPLAYCONFIG_PATH_INFO> paths;
    QVector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!queryPaths(paths, modes, QDC_ONLY_ACTIVE_PATHS)) return st;

    const int n = static_cast<int>(paths.size());
    st.valid = true;
    st.activePaths = n;

    for (int i = 0; i < n; i++) {
        const bool virt = isVirtualTarget(paths[i]);
        if (virt) st.virtualActive = true;

        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            if (sourceKey(paths[i]) != sourceKey(paths[j])) continue;
            // Same source, different target — that is the definition of clone.
            st.anyCloned = true;
            if (virt) st.virtualCloned = true;
        }
    }
#endif
    return st;
}

bool VirtualDisplayVDD::applyExtendTopology() {
#ifdef _WIN32
    // Exactly what Win+P → Extend does: restore the saved extend topology.
    LONG r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
    if (r == ERROR_SUCCESS) {
        VDD_LOG("VDD: Applied SDC_TOPOLOGY_EXTEND");
        return true;
    }
    VDD_LOG(QString("VDD: SDC_TOPOLOGY_EXTEND failed (code %1)").arg(r));
#endif
    return false;
}

bool VirtualDisplayVDD::applyExtendTopologySupplied() {
#ifdef _WIN32
    // SDC_TOPOLOGY_EXTEND only restores a topology Windows has already saved.
    // When there is none (common the first time a VDD monitor appears) we build
    // the path set ourselves: keep exactly the targets that are active now, but
    // give each one a source no other target uses.
    QVector<DISPLAYCONFIG_PATH_INFO> all;
    QVector<DISPLAYCONFIG_MODE_INFO> allModes;
    if (!queryPaths(all, allModes, QDC_ALL_PATHS)) {
        VDD_LOG("VDD: QueryDisplayConfig(QDC_ALL_PATHS) failed");
        return false;
    }

    // The targets we must keep lit — never activate anything new.
    QSet<QString> wantedTargets;
    for (const auto& p : all) {
        if (p.flags & DISPLAYCONFIG_PATH_ACTIVE) wantedTargets.insert(targetKey(p));
    }
    if (wantedTargets.isEmpty()) return false;

    QVector<DISPLAYCONFIG_PATH_INFO> chosen;
    QSet<QString> usedSources;
    QSet<QString> placedTargets;

    auto take = [&](const DISPLAYCONFIG_PATH_INFO& p) {
        DISPLAYCONFIG_PATH_INFO path = p;
        path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        // Dictate topology only — let Windows pick resolutions and refresh rates.
        path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        chosen.append(path);
        usedSources.insert(sourceKey(p));
        placedTargets.insert(targetKey(p));
    };

    // Pass 1 — every active target keeps its current source if nothing else claimed it.
    for (const auto& p : all) {
        if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
        if (placedTargets.contains(targetKey(p))) continue;
        if (usedSources.contains(sourceKey(p))) continue;  // loser of a clone pair
        take(p);
    }

    // Pass 2 — targets that lost the source race get re-homed onto a free one.
    for (const auto& p : all) {
        if (!wantedTargets.contains(targetKey(p))) continue;
        if (placedTargets.contains(targetKey(p))) continue;
        if (usedSources.contains(sourceKey(p))) continue;
        take(p);
    }

    if (placedTargets.size() != wantedTargets.size()) {
        VDD_LOG(QString("VDD: Could only give %1 of %2 displays a unique source")
                    .arg(placedTargets.size()).arg(wantedTargets.size()));
        return false;
    }

    const UINT32 base = SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
    const UINT32 count = static_cast<UINT32>(chosen.size());
    LONG r = SetDisplayConfig(count, chosen.data(), 0, nullptr, base | SDC_SAVE_TO_DATABASE);
    if (r != ERROR_SUCCESS) {
        VDD_LOG(QString("VDD: SetDisplayConfig failed (code %1), retrying without save").arg(r));
        r = SetDisplayConfig(count, chosen.data(), 0, nullptr, base);
    }
    if (r == ERROR_SUCCESS) {
        VDD_LOG(QString("VDD: Applied explicit extend topology across %1 display(s)")
                    .arg(chosen.size()));
        return true;
    }
    VDD_LOG(QString("VDD: Explicit extend topology failed (code %1)").arg(r));
#endif
    return false;
}

bool VirtualDisplayVDD::positionVirtualDisplay() {
#ifdef _WIN32
    // Find the primary so we can park the virtual display just past its right edge.
    DEVMODEW primaryDm = {};
    primaryDm.dmSize = sizeof(primaryDm);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &primaryDm)) {
        VDD_LOG("VDD: Could not read primary display settings");
        return false;
    }

    // The desktop may already extend past the primary (real second monitor);
    // park the virtual display beyond everything so nothing overlaps.
    LONG rightEdge = static_cast<LONG>(primaryDm.dmPelsWidth);
    DISPLAY_DEVICEW scan = {};
    scan.cb = sizeof(scan);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &scan, 0); i++) {
        if (scan.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(scan.DeviceName, ENUM_CURRENT_SETTINGS, &dm) &&
                !looksVirtual(QString::fromWCharArray(scan.DeviceString))) {
                rightEdge = qMax(rightEdge, dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth));
            }
        }
        scan = {};
        scan.cb = sizeof(scan);
    }

    bool moved = false;
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        const QString devName = QString::fromWCharArray(dd.DeviceName);
        const bool isVirtual = looksVirtual(QString::fromWCharArray(dd.DeviceString));

        if (isVirtual && (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                dd = {}; dd.cb = sizeof(dd);
                continue;
            }

            if (dm.dmPosition.x == rightEdge && dm.dmPosition.y == 0) {
                VDD_LOG("VDD: " + devName + " already positioned beside the primary");
                dd = {}; dd.cb = sizeof(dd);
                continue;
            }

            dm.dmFields = DM_POSITION;
            dm.dmPosition.x = rightEdge;
            dm.dmPosition.y = 0;

            LONG ret = ChangeDisplaySettingsExW(dd.DeviceName, &dm, nullptr,
                                                CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
            if (ret == DISP_CHANGE_SUCCESSFUL) {
                moved = true;
                rightEdge += static_cast<LONG>(dm.dmPelsWidth);  // stack extra VDDs sideways
                VDD_LOG(QString("VDD: Positioned %1 at %2,0").arg(devName).arg(dm.dmPosition.x));
            } else {
                VDD_LOG(QString("VDD: Could not position %1 (code %2)").arg(devName).arg(ret));
            }
        }
        dd = {};
        dd.cb = sizeof(dd);
    }

    if (moved) ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);  // commit
    return moved;
#else
    return false;
#endif
}

bool VirtualDisplayVDD::ensureExtendedTopology() {
#ifdef _WIN32
    TopologyState before = queryTopology();
    if (!before.valid) {
        VDD_LOG("VDD: Topology query failed — cannot verify extend mode");
        return false;
    }
    VDD_LOG("VDD: Topology before: " + before.describe());

    if (!before.anyCloned) {
        positionVirtualDisplay();
        return true;
    }

    VDD_LOG("VDD: Desktop is mirrored — switching to extend...");

    // SDC_TOPOLOGY_EXTEND can report success while leaving the desktop cloned
    // (it restores a *saved* topology, which may itself be stale), so verify
    // after each attempt rather than trusting the return code.
    for (int attempt = 0; attempt < 2; attempt++) {
        const bool applied = (attempt == 0) ? applyExtendTopology()
                                            : applyExtendTopologySupplied();
        if (!applied) continue;

        QThread::msleep(500);  // let the mode change settle before re-reading
        TopologyState after = queryTopology();
        VDD_LOG("VDD: Topology after: " + after.describe());
        if (after.valid && !after.anyCloned) {
            positionVirtualDisplay();
            emit statusChanged("Display extended");
            return true;
        }
    }

    emit error("Could not switch the desktop out of mirrored mode. "
               "Set Display Settings → 'Extend these displays' manually.");
    return false;
#else
    return false;
#endif
}

bool VirtualDisplayVDD::createVirtualDisplay(int width, int height, int refreshRate) {
    if (!m_vddInstalled) {
        emit error("VDD is not installed. Download from github.com/itsmikethetech/Virtual-Display-Driver");
        return false;
    }

    emit statusChanged(QString("Creating virtual display %1x%2 @ %3Hz...")
                            .arg(width).arg(height).arg(refreshRate));

    // First check if the driver is actually loaded in Windows
    // (files on disk ≠ driver installed)
    if (!isDriverLoaded()) {
        VDD_LOG("VDD: Driver files found but driver not loaded in Windows — attempting install...");
        if (!installDriver()) {
            emit error("VDD driver files exist but the driver isn't installed in Windows. "
                       "Try running 'VDD Control.exe' from the VirtualDisplayDriver folder, "
                       "or run as admin: pnputil /add-driver MttVDD.inf /install");
            return false;
        }
        VDD_LOG("VDD: Driver installed successfully");
    }

    // Ensure VDD Control is running (provides the named pipe interface)
    ensureVddControlRunning();

    // Method 1: Try VDD named pipe (modern versions)
    VDD_LOG("VDD: Trying named pipe to create display...");
    QString pipeCmd = QString("{\"command\":\"add\",\"width\":%1,\"height\":%2,\"refreshRate\":%3}")
                          .arg(width).arg(height).arg(refreshRate);
    if (tryNamedPipe(pipeCmd)) {
        VDD_LOG("VDD: Named pipe succeeded");
        m_createdDisplayCount++;
        emit statusChanged(QString("Virtual display created: %1x%2 @ %3Hz")
                               .arg(width).arg(height).arg(refreshRate));
        QThread::msleep(1500);
        int outputIdx = findVirtualDisplayOutput();
        if (outputIdx < 0) {
            VDD_LOG("VDD: Display not in monitor list — trying to activate/extend...");
            activateVirtualDisplay();
            QThread::msleep(1000);
            outputIdx = findVirtualDisplayOutput();
        }
        // Windows re-applies the last projection mode to newly attached monitors,
        // so a fresh VDD often comes up mirrored. Force extend before capture.
        ensureExtendedTopology();
        emit virtualDisplayCreated(outputIdx);
        return true;
    }

    // Method 2: Modify settings file + notify driver
    VDD_LOG("VDD: Named pipe unavailable, trying settings file method...");
    auto displays = readVddSettings();
    displays.append({width, height, refreshRate});

    if (!writeVddSettings(displays)) {
        emit error("Failed to write VDD settings file");
        return false;
    }
    VDD_LOG("VDD: Settings file written, notifying driver...");

    if (!notifyDriverRefresh()) {
        emit error("Failed to notify VDD driver — try restarting the driver manually");
        return false;
    }

    m_createdDisplayCount++;
    emit statusChanged(QString("Virtual display created: %1x%2 @ %3Hz")
                           .arg(width).arg(height).arg(refreshRate));

    QThread::msleep(2000);
    int outputIdx = findVirtualDisplayOutput();
    if (outputIdx < 0) {
        VDD_LOG("VDD: Display not in monitor list — trying to activate/extend...");
        activateVirtualDisplay();
        QThread::msleep(1000);
        outputIdx = findVirtualDisplayOutput();
    }
    if (outputIdx < 0) {
        VDD_LOG("VDD: Virtual display still not found — it may need manual 'Extend' in Display Settings");
    }
    ensureExtendedTopology();
    emit virtualDisplayCreated(outputIdx);
    return true;
}

bool VirtualDisplayVDD::removeVirtualDisplay(int index) {
    if (!m_vddInstalled) return false;

    emit statusChanged("Removing virtual display...");

    // Method 1: Try named pipe
    QString pipeCmd;
    if (index >= 0) {
        pipeCmd = QString("{\"command\":\"remove\",\"index\":%1}").arg(index);
    } else {
        pipeCmd = "{\"command\":\"remove\",\"index\":-1}";
    }

    if (tryNamedPipe(pipeCmd)) {
        if (m_createdDisplayCount > 0) m_createdDisplayCount--;
        emit virtualDisplayRemoved();
        emit statusChanged("Virtual display removed");
        return true;
    }

    // Method 2: Modify settings
    auto displays = readVddSettings();
    if (displays.isEmpty()) return false;

    if (index >= 0 && index < displays.size()) {
        displays.remove(index);
    } else {
        displays.removeLast();
    }

    if (!writeVddSettings(displays)) {
        emit error("Failed to update VDD settings");
        return false;
    }

    notifyDriverRefresh();
    if (m_createdDisplayCount > 0) m_createdDisplayCount--;
    emit virtualDisplayRemoved();
    emit statusChanged("Virtual display removed");
    return true;
}

bool VirtualDisplayVDD::removeAllVirtualDisplays() {
    if (!m_vddInstalled) return false;

    // Try named pipe
    if (tryNamedPipe("{\"command\":\"removeAll\"}")) {
        m_createdDisplayCount = 0;
        emit virtualDisplayRemoved();
        return true;
    }

    // Fallback: write empty display list
    if (writeVddSettings({})) {
        notifyDriverRefresh();
        m_createdDisplayCount = 0;
        emit virtualDisplayRemoved();
        return true;
    }

    return false;
}

int VirtualDisplayVDD::virtualDisplayCount() const {
    return m_createdDisplayCount;
}

// ─── Monitor Enumeration ───────────────────────────────────────────────────────

QVector<VirtualDisplayVDD::MonitorInfo> VirtualDisplayVDD::enumerateMonitors() const {
    QVector<MonitorInfo> result;

#ifdef _WIN32
    // Method 1: DXGI enumeration (only sees attached/active displays)
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr)) {
        VDD_LOG("VDD: CreateDXGIFactory1 failed");
    } else {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT adapterIdx = 0;
             factory->EnumAdapters1(adapterIdx, &adapter) != DXGI_ERROR_NOT_FOUND;
             adapterIdx++) {

            DXGI_ADAPTER_DESC1 adapterDesc;
            adapter->GetDesc1(&adapterDesc);
            QString adapterName = QString::fromWCharArray(adapterDesc.Description);

            IDXGIOutput* output = nullptr;
            for (UINT outputIdx = 0;
                 adapter->EnumOutputs(outputIdx, &output) != DXGI_ERROR_NOT_FOUND;
                 outputIdx++) {

                DXGI_OUTPUT_DESC outputDesc;
                output->GetDesc(&outputDesc);

                MonitorInfo info;
                info.adapterIndex = static_cast<int>(adapterIdx);
                info.outputIndex = static_cast<int>(outputIdx);
                info.name = QString::fromWCharArray(outputDesc.DeviceName);
                info.adapterName = adapterName;
                info.width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
                info.height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

                // Detect virtual displays by adapter name patterns
                QString lowerAdapter = adapterName.toLower();
                info.isVirtual = lowerAdapter.contains("virtual") ||
                                 lowerAdapter.contains("indirect") ||
                                 lowerAdapter.contains("idd") ||
                                 lowerAdapter.contains("vdd") ||
                                 lowerAdapter.contains("mttvdd") ||
                                 lowerAdapter.contains("mtt");

                result.append(info);
                output->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }

    // Method 2: EnumDisplayDevices — finds ALL displays including disconnected virtual ones
    // that DXGI misses. This is needed because virtual displays may not be "attached"
    // to the desktop yet.
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        // Skip inactive devices unless they look virtual
        QString devName = QString::fromWCharArray(dd.DeviceName);
        QString devString = QString::fromWCharArray(dd.DeviceString);
        QString lowerString = devString.toLower();

        bool isVirtual = lowerString.contains("virtual") ||
                         lowerString.contains("indirect") ||
                         lowerString.contains("idd") ||
                         lowerString.contains("vdd") ||
                         lowerString.contains("mtt");

        // Check if this device is already in our DXGI results
        bool alreadyFound = false;
        for (const auto& existing : result) {
            if (existing.name == devName) {
                alreadyFound = true;
                break;
            }
        }

        if (!alreadyFound && isVirtual) {
            MonitorInfo info;
            // These indices are NOT DXGI indices — this branch only runs for
            // displays DXGI could not enumerate (typically a VDD that is not
            // attached to the desktop yet). Publishing the EnumDisplayDevices
            // ordinal as an adapter index sent the capture path to the wrong
            // adapter, which is what forced the slow GDI fallback. The device
            // name below is the authoritative key; ScreenCaptureWin resolves
            // the real adapter/output from it.
            info.adapterIndex = 0;
            info.outputIndex = 0;
            info.name = devName;
            info.adapterName = devString;
            info.isVirtual = true;

            // Get resolution from display settings
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                info.width = static_cast<int>(dm.dmPelsWidth);
                info.height = static_cast<int>(dm.dmPelsHeight);
            } else if (EnumDisplaySettingsW(dd.DeviceName, ENUM_REGISTRY_SETTINGS, &dm)) {
                info.width = static_cast<int>(dm.dmPelsWidth);
                info.height = static_cast<int>(dm.dmPelsHeight);
            } else {
                // Use requested resolution from settings
                info.width = 1920;
                info.height = 1080;
            }

            VDD_LOG(QString("VDD: Found virtual display via EnumDisplayDevices: %1 (%2) %3x%4 flags=0x%5")
                        .arg(devName, devString).arg(info.width).arg(info.height)
                        .arg(dd.StateFlags, 0, 16));
            result.append(info);
        }
        dd = {};
        dd.cb = sizeof(dd);
    }
#endif

    return result;
}

int VirtualDisplayVDD::findVirtualDisplayOutput() const {
    auto monitors = enumerateMonitors();
    // Return the last virtual display found (most recently created)
    for (int i = monitors.size() - 1; i >= 0; i--) {
        if (monitors[i].isVirtual) {
            return i;
        }
    }
    return -1;
}

// ─── VDD Settings File ────────────────────────────────────────────────────────

QVector<VirtualDisplayVDD::VddResolution> VirtualDisplayVDD::readVddSettings() const {
    QVector<VddResolution> displays;
    if (m_vddPath.isEmpty()) return displays;

    // Try each known settings file
    for (const auto& filename : kSettingsFiles) {
        QString path = m_vddPath + "/" + filename;
        QFile file(path);
        if (!file.exists() || !file.open(QIODevice::ReadOnly)) continue;

        QXmlStreamReader xml(&file);
        VddResolution current = {0, 0, 0};

        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                QString name = xml.name().toString();
                if (name == "Width" || name == "width") {
                    current.width = xml.readElementText().toInt();
                } else if (name == "Height" || name == "height") {
                    current.height = xml.readElementText().toInt();
                } else if (name == "RefreshRate" || name == "refreshRate" ||
                           name == "Refresh" || name == "refresh") {
                    current.refreshRate = xml.readElementText().toInt();
                }
            } else if (xml.isEndElement()) {
                QString name = xml.name().toString();
                if ((name == "Display" || name == "display" || name == "Monitor" || name == "monitor")
                    && current.width > 0 && current.height > 0) {
                    if (current.refreshRate == 0) current.refreshRate = 60;
                    displays.append(current);
                    current = {0, 0, 0};
                }
            }
        }

        file.close();
        if (!displays.isEmpty()) break;
    }

    return displays;
}

bool VirtualDisplayVDD::writeVddSettings(const QVector<VddResolution>& displays) {
    if (m_vddPath.isEmpty()) return false;

    // Find existing settings file, or create the first known one
    QString settingsPath;
    for (const auto& filename : kSettingsFiles) {
        QString path = m_vddPath + "/" + filename;
        if (QFileInfo::exists(path)) {
            settingsPath = path;
            break;
        }
    }
    if (settingsPath.isEmpty()) {
        settingsPath = m_vddPath + "/" + kSettingsFiles.first();
    }

    QFile file(settingsPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "VDD: Cannot write settings to" << settingsPath;
        return false;
    }

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("VirtualDisplaySettings");
    xml.writeStartElement("Displays");

    for (const auto& disp : displays) {
        xml.writeStartElement("Display");
        xml.writeTextElement("Width", QString::number(disp.width));
        xml.writeTextElement("Height", QString::number(disp.height));
        xml.writeTextElement("RefreshRate", QString::number(disp.refreshRate));
        xml.writeEndElement(); // Display
    }

    xml.writeEndElement(); // Displays
    xml.writeEndElement(); // VirtualDisplaySettings
    xml.writeEndDocument();

    file.close();
    VDD_LOG(QString("VDD: Wrote %1 display(s) to %2").arg(displays.size()).arg(settingsPath));
    return true;
}

// ─── Driver Communication ──────────────────────────────────────────────────────

bool VirtualDisplayVDD::tryNamedPipe(const QString& command) {
#ifdef _WIN32
    HANDLE pipe = CreateFileA(
        kVddPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr,
        OPEN_EXISTING,
        0, nullptr);

    if (pipe == INVALID_HANDLE_VALUE) {
        VDD_LOG("VDD: Named pipe not available (error " + QString::number(GetLastError()) + ")");
        return false;
    }

    QByteArray data = command.toUtf8();
    DWORD bytesWritten = 0;
    BOOL success = WriteFile(pipe, data.data(), data.size(), &bytesWritten, nullptr);

    if (success) {
        // Read response
        char buffer[1024] = {};
        DWORD bytesRead = 0;
        ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
        if (bytesRead > 0) {
            QString response = QString::fromUtf8(buffer, bytesRead);
            VDD_LOG("VDD: Pipe response: " + response);
        }
    }

    CloseHandle(pipe);
    return success && bytesWritten > 0;
#else
    Q_UNUSED(command);
    return false;
#endif
}

bool VirtualDisplayVDD::notifyDriverRefresh() {
#ifdef _WIN32
    // Method 1: Try named pipe with refresh command
    if (tryNamedPipe("{\"command\":\"refresh\"}")) {
        return true;
    }

    // Method 2: Try pnputil restart with various device IDs
    static const char* deviceIds[] = {
        "Root\\MttVDD\\0000",
        "Root\\VirtualDisplayDriver\\0000",
        "Root\\IddSampleDriver\\0000",
    };
    for (const auto* devId : deviceIds) {
        QProcess proc;
        proc.setProgram("pnputil");
        proc.setArguments({"/restart-device", devId});
        proc.start();
        if (proc.waitForFinished(10000) && proc.exitCode() == 0) {
            VDD_LOG(QString("VDD: Driver restarted via pnputil (%1)").arg(devId));
            return true;
        }
    }

    // Method 3: Try devcon (bundled in VDD directory or system PATH)
    QString devconPath = m_vddPath + "/devcon.exe";
    if (!QFileInfo::exists(devconPath)) devconPath = "devcon";

    static const char* hwIds[] = {"Root\\MttVDD", "Root\\VirtualDisplayDriver"};
    for (const auto* hwId : hwIds) {
        QProcess proc;
        proc.setProgram(devconPath);
        proc.setArguments({"restart", hwId});
        proc.start();
        if (proc.waitForFinished(10000) && proc.exitCode() == 0) {
            VDD_LOG(QString("VDD: Driver restarted via devcon (%1)").arg(hwId));
            return true;
        }
    }

    VDD_LOG("VDD: Could not notify driver — all restart methods failed");
    return false;
#else
    return false;
#endif
}
