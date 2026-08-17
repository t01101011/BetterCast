#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <cstdint>

class ScreenCapture;
class VideoEncoderFF;
class NetworkSender;
class VirtualDisplayVDD;
class InputInjector;

// Orchestrates capture → encode → send, once per connected receiver.
//
// Each receiver gets its own virtual display, capture, encoder and socket —
// matching the macOS sender, where two connected devices mean two extra
// screens rather than the same screen mirrored twice. Sessions are fully
// independent: one receiver disconnecting or failing to encode does not
// disturb the others.
class SenderController : public QObject {
    Q_OBJECT
public:
    explicit SenderController(QObject* parent = nullptr);
    ~SenderController() override;

    // Start streaming to one receiver. `displayName` selects the monitor to
    // capture (e.g. "\\\\.\\DISPLAY21"); when empty the controller claims the
    // next virtual display not already in use by another session.
    bool startSending(const QString& receiverHost, uint16_t port = 51820,
                      int fps = 30, int bitrateMbps = 8,
                      const QString& displayName = QString());

    // Stop one receiver, or every receiver.
    void stopSending(const QString& receiverHost);
    void stopAll();

    bool isSending() const { return !m_sessions.isEmpty(); }
    bool isSendingTo(const QString& receiverHost) const;
    int  sessionCount() const { return m_sessions.size(); }
    QStringList activeReceivers() const;

    // Display used by a given receiver, for the UI to show what goes where.
    QString displayForReceiver(const QString& receiverHost) const;

    // Default monitor for the next session started without an explicit display.
    void setMonitorIndex(int adapterIndex, int outputIndex);
    void setDisplayName(const QString& name) { m_displayName = name; }

    // VDD virtual display management
    VirtualDisplayVDD* vdd() const { return m_vdd; }

    QString encoderInfo() const;

signals:
    void started(const QString& receiverHost);
    void stopped(const QString& receiverHost);
    void connected(const QString& receiverHost);
    void disconnected(const QString& receiverHost);
    void error(const QString& message);
    void statusChanged(const QString& status);
    // Emitted whenever a session starts or ends, so the UI can refresh counts.
    void sessionsChanged();

private:
    // One receiver's pipeline. Owned by the controller; torn down as a unit.
    struct Session {
        QString host;
        uint16_t port = 51820;
        QString displayName;
        int adapterIndex = 0;
        int outputIndex = 0;
        int fps = 30;
        int bitrateMbps = 8;
        bool encoderReady = false;

        ScreenCapture* capture = nullptr;
        VideoEncoderFF* encoder = nullptr;
        NetworkSender* network = nullptr;
        InputInjector* input = nullptr;
    };

    Session* findSession(const QString& host) const;
    void destroySession(Session* s);
    // Choose a virtual display no other session is streaming, creating one if
    // none is free. Returns an empty string when nothing suitable exists.
    QString claimDisplayFor(const QString& host);
    bool displayInUse(const QString& displayName) const;

    void onFrameCaptured(Session* s, const QByteArray& nv12, int width, int height,
                         qint64 ptsNanos);
    void onEncoded(Session* s, const QByteArray& payload);
    void onSessionConnected(Session* s);
    void onSessionDisconnected(Session* s);

    QVector<Session*> m_sessions;
    // Auto-adding a display raises a UAC prompt. If it fails once it will keep
    // failing for the same reason, so stop trying and let the user decide -
    // five prompts in a row was the observed behaviour otherwise.
    bool m_autoAddFailed = false;
    VirtualDisplayVDD* m_vdd = nullptr;

    // Defaults applied to the next session started without explicit values.
    int m_adapterIndex = 0;
    int m_outputIndex = 0;
    QString m_displayName;
};
