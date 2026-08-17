#pragma once
#include <QObject>
#include <QByteArray>
#include <QSize>

// Abstract screen capture interface.
// Platform implementations: ScreenCaptureWin (DXGI), ScreenCaptureLinux (PipeWire)
class ScreenCapture : public QObject {
    Q_OBJECT
public:
    explicit ScreenCapture(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~ScreenCapture() = default;

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual QSize resolution() const = 0;

signals:
    // Emitted for each captured frame.
    // data: NV12 pixel buffer (Y plane followed by interleaved UV plane)
    // width, height: frame dimensions
    // ptsNanos: capture time from the monotonic clock, not a frame counter —
    //           lets the receiver pace playback on when the frame really existed
    //
    // NOTE: implementations may emit this from a dedicated capture thread.
    // Connect with Qt::DirectConnection to keep encoding off the GUI thread.
    void frameCaptured(const QByteArray& data, int width, int height, qint64 ptsNanos);
    void error(const QString& message);
};
