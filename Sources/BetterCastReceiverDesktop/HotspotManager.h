#pragma once

#include <QObject>
#include <QString>

// Windows Mobile Hotspot control.
//
// Screen streaming only needs the devices on one local network — it never
// touches the internet. On a public network with client isolation (hotel,
// mall, campus) that local path does not exist and BetterCast cannot work at
// all, so hosting an access point from the sending machine is the fix: the
// receiver joins it and the two are on one segment by construction.
//
// Windows exposes this through WinRT's NetworkOperatorTetheringManager, which
// both creates the AP and reports its SSID and passphrase — the exact payload a
// pairing QR needs, so one API covers the whole feature.
class HotspotManager : public QObject {
    Q_OBJECT

public:
    explicit HotspotManager(QObject* parent = nullptr);
    ~HotspotManager() override;

    struct Info {
        bool    supported   = false;  // the API answered at all
        bool    on          = false;
        QString ssid;
        QString passphrase;           // needed for the join QR
        int     clientCount = 0;
        int     maxClients  = 0;
        QString error;                // set when supported is false
    };

    // Cheap enough to call from the UI thread.
    Info query() const;

    // Both run off-thread; the outcome arrives on stateChanged() or failed().
    void start();
    void stop();

    // Windows turns the hotspot off by itself after roughly five minutes with
    // no client connected ("automatically turn off mobile hotspot", on by
    // default). A pairing screen that starts the AP and then waits for someone
    // to walk to their phone will therefore go dead underneath the QR it is
    // showing. Callers displaying a QR should keep this armed rather than
    // start once and trust it to stay up. The alternative — clearing
    // PeerlessTimeoutEnabled under HKLM icssvc — needs elevation and changes
    // behaviour for every other app on the machine, so it is not done here.
    static constexpr int kIdleShutoffSeconds = 300;

signals:
    void stateChanged(const HotspotManager::Info& info);
    void failed(const QString& message);
};
