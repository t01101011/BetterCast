#pragma once
// Note: this file is only compiled on Windows (gated by ENABLE_SENDER + WIN32).

#include <QObject>
#include <QString>
#include <QSize>
#include <QVector>

// Manages Virtual Display Driver (VDD) integration.
// Detects VDD installation, creates/removes virtual monitors,
// and enumerates available DXGI outputs for capture selection.
class VirtualDisplayVDD : public QObject {
    Q_OBJECT
public:
    explicit VirtualDisplayVDD(QObject* parent = nullptr);
    ~VirtualDisplayVDD() override;

    struct MonitorInfo {
        int adapterIndex;
        int outputIndex;
        QString name;        // e.g. "\\.\DISPLAY1"
        QString adapterName; // e.g. "NVIDIA GeForce RTX 3080"
        int width;
        int height;
        bool isVirtual;      // true if this is a VDD virtual display
    };

    struct VddResolution {
        int width;
        int height;
        int refreshRate;
    };

    // Snapshot of the desktop's display topology.
    //
    // In CCD terms a "clone" (what Display Settings calls "Duplicate these
    // displays") is two active paths that share one source — one framebuffer
    // driving several monitors. Extend gives every target its own source.
    struct TopologyState {
        bool valid = false;         // the CCD query succeeded
        bool anyCloned = false;     // some source drives more than one target
        bool virtualCloned = false; // a virtual display is one of the clones
        bool virtualActive = false; // a virtual target is attached to the desktop
        int activePaths = 0;
        QString describe() const;
    };

    // VDD detection
    bool isVddInstalled() const;
    QString vddInstallPath() const;
    void refreshInstallStatus();  // re-run detection (e.g. after user installs VDD)

    // Virtual display management
    bool createVirtualDisplay(int width = 1920, int height = 1080, int refreshRate = 60);
    bool removeVirtualDisplay(int index = -1); // -1 = remove last
    bool removeAllVirtualDisplays();
    int virtualDisplayCount() const;

    // Monitor enumeration (all monitors — real + virtual)
    QVector<MonitorInfo> enumerateMonitors() const;

    // Find the output index for a virtual display
    int findVirtualDisplayOutput() const;

    // The four modes Win+P offers. Windows applies these through one call, so
    // exposing them in-app saves a trip to Display Settings.
    enum class Topology { Extend, Duplicate, InternalOnly, ExternalOnly };
    bool applyTopology(Topology mode);
    static QString topologyName(Topology mode);

    // A root-enumerated VDD device node. Each node contributes one virtual
    // monitor — they are NOT described by the settings XML, which is why
    // rewriting that file cannot remove them.
    struct VddDevice {
        QString instanceId;    // e.g. "ROOT\\DISPLAY\\0001"
        QString friendlyName;
    };
    QVector<VddDevice> enumerateVddDevices() const;

    // Remove VDD device nodes, keeping the first `keep` of them.
    // Requires administrator rights, so this triggers a single UAC prompt
    // rather than elevating the whole app.
    bool removeVddDevices(int keep = 0);

    // Attach a specific virtual display to the desktop and place it beside the
    // others. A VDD device node can exist while its monitor is detached — it
    // then reports 0x0 and has no framebuffer, so capturing it yields nothing.
    bool attachVirtualDisplay(const QString& deviceName,
                              int width = 1920, int height = 1080, int refreshRate = 60);

    // Display topology
    TopologyState queryTopology() const;

    // Force the desktop out of mirrored mode and place the virtual display
    // beside the primary. No-op (but still repositions) when already extended.
    // This is what Display Settings → "Extend these displays" does.
    bool ensureExtendedTopology();

signals:
    void virtualDisplayCreated(int outputIndex);
    void virtualDisplayRemoved();
    void error(const QString& message);
    void statusChanged(const QString& status);

private:
    bool detectVddInstall();
    bool isDriverLoaded() const;
    bool installDriver();
    bool ensureVddControlRunning();
    bool activateVirtualDisplay();
    bool applyExtendTopology();          // SDC_TOPOLOGY_EXTEND — the Win+P route
    bool applyExtendTopologySupplied();  // explicit one-source-per-target path set
    bool positionVirtualDisplay();       // place it to the right of the primary
    bool writeVddSettings(const QVector<VddResolution>& displays);
    QVector<VddResolution> readVddSettings() const;
    bool notifyDriverRefresh();
    bool tryNamedPipe(const QString& command);

    QString m_vddPath;
    bool m_vddInstalled = false;
    int m_createdDisplayCount = 0;
};
