#pragma once
// Note: this file is only compiled on Windows (gated in CMakeLists.txt).
// Do NOT wrap in #ifdef _WIN32 — AutoMoc cannot resolve preprocessor guards
// and will skip Q_OBJECT, causing linker errors.

#include "ScreenCapture.h"
#include <QString>
#include <atomic>
#include <thread>

// Forward declarations — avoid pulling Windows headers into every TU
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutputDuplication;
struct HDC__;
typedef HDC__* HDC;
struct HBITMAP__;
typedef HBITMAP__* HBITMAP;

// DXGI Desktop Duplication capture.
//
// Runs a dedicated thread that blocks in AcquireNextFrame and wakes the instant
// Windows composites a new frame — the equivalent of ScreenCaptureKit's push
// callback on macOS. Polling on a timer instead costs up to a full frame
// interval of latency and jitters with the ~15.6ms Windows tick.
class ScreenCaptureWin : public ScreenCapture {
    Q_OBJECT
public:
    explicit ScreenCaptureWin(int targetFPS = 30, QObject* parent = nullptr);
    ~ScreenCaptureWin() override;

    // Preferred monitor as DXGI indices. Only used as a fallback when the
    // display name is empty or cannot be matched — indices are fragile because
    // a root-enumerated virtual display lives on its own adapter.
    void setMonitorIndex(int adapterIndex, int outputIndex);

    // Display device name, e.g. "\\\\.\\DISPLAY3". This is the reliable key:
    // it is matched against DXGI_OUTPUT_DESC.DeviceName across every adapter.
    void setDisplayName(const QString& name) { m_displayName = name; }

    bool start() override;
    void stop() override;             // stops and joins the capture thread
    bool isRunning() const override { return m_running; }
    QSize resolution() const override { return m_resolution; }

    // True when DXGI duplication was unavailable and GDI BitBlt is in use.
    // GDI has no dirty-rect info and reads back the full uncompressed surface,
    // so it is markedly slower — worth surfacing rather than failing silently.
    bool usingGdiFallback() const { return m_useGdiFallback; }

private:
    bool initD3DForOutput();          // resolve output by name, device on its adapter
    bool initDuplication();
    bool initGdiFallback();
    void captureLoop();               // runs on m_thread
    bool captureFrameDxgi();          // one blocking acquire + convert + emit
    void captureFrameGdi();
    void convertAndEmit(const uint8_t* bgra, int pitch, qint64 ptsNanos);
    void cleanup();

    // D3D11 objects
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGIOutputDuplication* m_duplication = nullptr;

    // Two staging textures, alternated per frame. Mapping the same texture that
    // was just written forces a CPU/GPU sync every frame; rotating avoids it.
    static constexpr int kStagingCount = 2;
    ID3D11Texture2D* m_stagingTex[kStagingCount] = {};
    int m_stagingIndex = 0;

    // GDI fallback objects
    HDC m_gdiDC = nullptr;
    HDC m_memDC = nullptr;
    HBITMAP m_bitmap = nullptr;
    bool m_useGdiFallback = false;

    std::thread m_thread;
    int m_targetFPS;
    qint64 m_minFrameIntervalNs = 0;  // rate limit; 0 = uncapped
    qint64 m_lastEmitNs = 0;
    int m_adapterIndex = 0;
    int m_outputIndex = 0;
    QString m_displayName;
    QSize m_resolution;
    std::atomic<bool> m_running{false};

    // Reused frame buffer — avoids a fresh allocation every frame
    QByteArray m_nv12;
};
