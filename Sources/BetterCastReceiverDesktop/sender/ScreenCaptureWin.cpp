#include "ScreenCaptureWin.h"
#include "../MainWindow.h"  // LogManager

#include <d3d11.h>
#include <dxgi1_2.h>
#include <Windows.h>
#include <timeapi.h>   // timeBeginPeriod — not pulled in when WIN32_LEAN_AND_MEAN is set
#include <QDebug>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod

namespace {

qint64 nowNanos() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

ScreenCaptureWin::ScreenCaptureWin(int targetFPS, QObject* parent)
    : ScreenCapture(parent)
    , m_targetFPS(targetFPS > 0 ? targetFPS : 30)
{
    m_minFrameIntervalNs = 1000000000LL / m_targetFPS;
}

void ScreenCaptureWin::setMonitorIndex(int adapterIndex, int outputIndex) {
    m_adapterIndex = adapterIndex;
    m_outputIndex = outputIndex;
}

ScreenCaptureWin::~ScreenCaptureWin() {
    stop();
}

// Resolve the target monitor by device name across every adapter, then create
// the D3D11 device on the adapter that actually owns it.
//
// Index-based lookup is unreliable here: a VDD is root-enumerated (ROOT\DISPLAY)
// and shows up as its own DXGI adapter, so an index taken from a different
// enumeration points at the wrong adapter — DuplicateOutput then fails and we
// silently drop to the much slower GDI path.
bool ScreenCaptureWin::initD3DForOutput() {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (FAILED(hr)) {
        qWarning() << "Sender: CreateDXGIFactory1 failed";
        return false;
    }

    int foundAdapter = -1;
    int foundOutput = -1;

    if (!m_displayName.isEmpty()) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
            IDXGIOutput* output = nullptr;
            for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
                DXGI_OUTPUT_DESC desc;
                output->GetDesc(&desc);
                output->Release();

                if (m_displayName.compare(QString::fromWCharArray(desc.DeviceName),
                                          Qt::CaseInsensitive) == 0) {
                    foundAdapter = static_cast<int>(a);
                    foundOutput = static_cast<int>(o);
                    break;
                }
            }
            adapter->Release();
            if (foundAdapter >= 0) break;
        }
    }

    if (foundAdapter >= 0) {
        if (foundAdapter != m_adapterIndex || foundOutput != m_outputIndex) {
            LogManager::instance().log(
                QString("Sender: Resolved %1 to adapter %2 output %3 (was %4/%5)")
                    .arg(m_displayName).arg(foundAdapter).arg(foundOutput)
                    .arg(m_adapterIndex).arg(m_outputIndex));
        }
        m_adapterIndex = foundAdapter;
        m_outputIndex = foundOutput;
    } else if (!m_displayName.isEmpty()) {
        LogManager::instance().log(
            QString("Sender: %1 not found in DXGI enumeration — is it attached to the "
                    "desktop? Falling back to adapter %2 output %3")
                .arg(m_displayName).arg(m_adapterIndex).arg(m_outputIndex));
    }

    IDXGIAdapter1* selectedAdapter = nullptr;
    hr = factory->EnumAdapters1(m_adapterIndex, &selectedAdapter);
    factory->Release();
    if (FAILED(hr)) {
        qWarning() << "Sender: Adapter" << m_adapterIndex << "not found, using default";
        selectedAdapter = nullptr;
    }

    UINT flags = 0;
#ifdef QT_DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(
        selectedAdapter,
        selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
        &m_device, &featureLevel, &m_context);

    if (selectedAdapter) selectedAdapter->Release();

    if (FAILED(hr)) {
        qWarning() << "Sender: D3D11CreateDevice failed, hr=" << Qt::hex << hr;
        return false;
    }
    qDebug() << "Sender: D3D11 device on adapter" << m_adapterIndex
             << "feature level" << Qt::hex << featureLevel;
    return true;
}

bool ScreenCaptureWin::initDuplication() {
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) { qWarning() << "Sender: QueryInterface IDXGIDevice failed"; return false; }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) { qWarning() << "Sender: GetAdapter failed"; return false; }

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(m_outputIndex, &output);
    adapter->Release();
    if (FAILED(hr)) {
        qWarning() << "Sender: EnumOutputs failed for output" << m_outputIndex;
        return false;
    }

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();
    if (FAILED(hr)) { qWarning() << "Sender: QueryInterface IDXGIOutput1 failed"; return false; }

    hr = output1->DuplicateOutput(m_device, &m_duplication);
    output1->Release();
    if (FAILED(hr)) {
        qWarning() << "Sender: DuplicateOutput failed, hr=" << Qt::hex << hr;
        return false;
    }

    DXGI_OUTDUPL_DESC desc;
    m_duplication->GetDesc(&desc);
    m_resolution = QSize(desc.ModeDesc.Width, desc.ModeDesc.Height);
    qDebug() << "Sender: Desktop duplication ready," << m_resolution;

    // Rotating staging textures for CPU readback. Mapping the texture we just
    // wrote stalls the pipeline waiting on the GPU; alternating lets the copy
    // for frame N+1 proceed while frame N is being read.
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_resolution.width();
    texDesc.Height = m_resolution.height();
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < kStagingCount; i++) {
        hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_stagingTex[i]);
        if (FAILED(hr)) { qWarning() << "Sender: CreateTexture2D staging failed"; return false; }
    }
    m_stagingIndex = 0;
    return true;
}

bool ScreenCaptureWin::start() {
    if (m_running) return true;

    m_useGdiFallback = false;
    m_lastEmitNs = 0;

    if (!initD3DForOutput()) {
        LogManager::instance().log("Sender: D3D11 init failed, trying GDI fallback");
        cleanup();
        if (!initGdiFallback()) {
            emit error("Failed to initialize screen capture (both DXGI and GDI failed)");
            return false;
        }
    } else if (!initDuplication()) {
        LogManager::instance().log("Sender: DXGI Desktop Duplication unavailable, using GDI fallback");
        cleanup();
        if (!initGdiFallback()) {
            emit error("Failed to initialize screen capture (DXGI unsupported, GDI failed)");
            return false;
        }
    }

    m_running = true;
    m_thread = std::thread([this]() { captureLoop(); });

    if (m_useGdiFallback) {
        LogManager::instance().log(
            QString("Sender: Screen capture started (GDI fallback — expect higher latency) "
                    "at %1 FPS, %2x%3")
                .arg(m_targetFPS).arg(m_resolution.width()).arg(m_resolution.height()));
    } else {
        LogManager::instance().log(
            QString("Sender: Screen capture started (DXGI duplication) at %1 FPS, %2x%3")
                .arg(m_targetFPS).arg(m_resolution.width()).arg(m_resolution.height()));
    }
    return true;
}

void ScreenCaptureWin::stop() {
    m_running = false;                       // capture loop exits within its acquire timeout
    if (m_thread.joinable()) m_thread.join();
    cleanup();
}

void ScreenCaptureWin::captureLoop() {
    // Timer resolution affects our sleeps and DXGI's own wait granularity.
    timeBeginPeriod(1);

    while (m_running) {
        if (m_useGdiFallback) {
            const qint64 start = nowNanos();
            captureFrameGdi();
            const qint64 elapsed = nowNanos() - start;
            const qint64 remaining = m_minFrameIntervalNs - elapsed;
            if (remaining > 0 && m_running) {
                Sleep(static_cast<DWORD>(remaining / 1000000));
            }
        } else if (!captureFrameDxgi()) {
            break;
        }
    }

    timeEndPeriod(1);
}

// One iteration of the DXGI path. Returns false to terminate the loop.
bool ScreenCaptureWin::captureFrameDxgi() {
    if (!m_duplication) return false;

    bool havePending = false;   // a frame sits in staging, not yet converted

    while (m_running) {
        // Wait only as long as the pacing budget allows, so a pending frame
        // still gets emitted when the desktop goes idle mid-interval.
        DWORD timeoutMs = 100;
        if (havePending) {
            const qint64 due = m_lastEmitNs + m_minFrameIntervalNs;
            const qint64 waitNs = due - nowNanos();
            timeoutMs = waitNs <= 0 ? 0 : static_cast<DWORD>(waitNs / 1000000);
        }

        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (havePending) break;   // nothing newer coming — emit what we hold
            continue;                 // desktop genuinely idle
        }

        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                qDebug() << "Sender: Duplication access lost, reinitializing...";
                if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
                for (int i = 0; i < kStagingCount; i++) {
                    if (m_stagingTex[i]) { m_stagingTex[i]->Release(); m_stagingTex[i] = nullptr; }
                }
                if (!initDuplication()) {
                    emit error("Failed to reinitialize desktop duplication");
                    return false;
                }
                return true;
            }
            return false;
        }

        // LastPresentTime == 0 means only the pointer moved — the desktop image
        // is unchanged, so re-encoding it would burn bitrate for nothing.
        if (frameInfo.LastPresentTime.QuadPart == 0) {
            desktopResource->Release();
            m_duplication->ReleaseFrame();
            continue;
        }

        ID3D11Texture2D* desktopTex = nullptr;
        hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTex);
        desktopResource->Release();
        if (FAILED(hr)) {
            m_duplication->ReleaseFrame();
            continue;
        }

        // Copy out and release promptly — holding the frame blocks compositing.
        m_stagingIndex = (m_stagingIndex + 1) % kStagingCount;
        m_context->CopyResource(m_stagingTex[m_stagingIndex], desktopTex);
        desktopTex->Release();
        m_duplication->ReleaseFrame();
        havePending = true;

        // Under the frame budget? Keep draining so we encode only the newest
        // state instead of every intermediate frame of a fast animation.
        if (nowNanos() < m_lastEmitNs + m_minFrameIntervalNs) continue;
        break;
    }

    if (!havePending || !m_running) return m_running;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_context->Map(m_stagingTex[m_stagingIndex], 0, D3D11_MAP_READ, 0, &mapped))) {
        return true;
    }
    convertAndEmit(static_cast<const uint8_t*>(mapped.pData),
                   static_cast<int>(mapped.RowPitch), nowNanos());
    m_context->Unmap(m_stagingTex[m_stagingIndex], 0);
    return true;
}

bool ScreenCaptureWin::initGdiFallback() {
    if (m_displayName.isEmpty()) {
        m_gdiDC = CreateDCA("DISPLAY", nullptr, nullptr, nullptr);
    } else {
        m_gdiDC = CreateDCA(nullptr, m_displayName.toLocal8Bit().constData(), nullptr, nullptr);
    }
    if (!m_gdiDC) m_gdiDC = GetDC(nullptr);  // whole virtual desktop
    if (!m_gdiDC) {
        LogManager::instance().log("Sender: GDI CreateDC failed");
        return false;
    }

    int w = GetDeviceCaps(m_gdiDC, HORZRES);
    int h = GetDeviceCaps(m_gdiDC, VERTRES);
    if (w <= 0 || h <= 0) {
        LogManager::instance().log(QString("Sender: GDI invalid resolution: %1x%2").arg(w).arg(h));
        DeleteDC(m_gdiDC);
        m_gdiDC = nullptr;
        return false;
    }

    m_memDC = CreateCompatibleDC(m_gdiDC);
    m_bitmap = CreateCompatibleBitmap(m_gdiDC, w, h);
    SelectObject(m_memDC, m_bitmap);

    m_resolution = QSize(w, h);
    m_useGdiFallback = true;

    LogManager::instance().log(QString("Sender: GDI capture initialized for %1 (%2x%3)")
                                   .arg(m_displayName.isEmpty() ? "primary" : m_displayName)
                                   .arg(w).arg(h));
    return true;
}

void ScreenCaptureWin::captureFrameGdi() {
    if (!m_running || !m_gdiDC || !m_memDC) return;

    const int w = m_resolution.width();
    const int h = m_resolution.height();

    BitBlt(m_memDC, 0, 0, w, h, m_gdiDC, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;  // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    QByteArray bgraData(w * h * 4, Qt::Uninitialized);
    GetDIBits(m_memDC, m_bitmap, 0, h, bgraData.data(),
              reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    convertAndEmit(reinterpret_cast<const uint8_t*>(bgraData.constData()), w * 4, nowNanos());
}

// BGRA → NV12 (BT.601). Runs on the capture thread, never the GUI thread.
void ScreenCaptureWin::convertAndEmit(const uint8_t* bgra, int pitch, qint64 ptsNanos) {
    const int w = m_resolution.width();
    const int h = m_resolution.height();
    if (w <= 0 || h <= 0 || !bgra) return;

    const int ySize = w * h;
    const int uvSize = w * (h / 2);
    if (m_nv12.size() != ySize + uvSize) {
        m_nv12.resize(ySize + uvSize);
    }
    uint8_t* yPlane = reinterpret_cast<uint8_t*>(m_nv12.data());
    uint8_t* uvPlane = yPlane + ySize;

    // Fused loop: Y for every pixel, UV from each 2x2 block.
    for (int y = 0; y < h; y += 2) {
        const uint8_t* row0 = bgra + y * pitch;
        const uint8_t* row1 = bgra + (y + 1) * pitch;
        uint8_t* yRow0 = yPlane + y * w;
        uint8_t* yRow1 = yPlane + (y + 1) * w;
        uint8_t* uvRow = uvPlane + (y / 2) * w;

        for (int x = 0; x < w; x += 2) {
            const int b00 = row0[x*4+0],     g00 = row0[x*4+1],     r00 = row0[x*4+2];
            const int b01 = row0[(x+1)*4+0], g01 = row0[(x+1)*4+1], r01 = row0[(x+1)*4+2];
            const int b10 = row1[x*4+0],     g10 = row1[x*4+1],     r10 = row1[x*4+2];
            const int b11 = row1[(x+1)*4+0], g11 = row1[(x+1)*4+1], r11 = row1[(x+1)*4+2];

            yRow0[x]   = static_cast<uint8_t>(((66*r00 + 129*g00 + 25*b00 + 128) >> 8) + 16);
            yRow0[x+1] = static_cast<uint8_t>(((66*r01 + 129*g01 + 25*b01 + 128) >> 8) + 16);
            yRow1[x]   = static_cast<uint8_t>(((66*r10 + 129*g10 + 25*b10 + 128) >> 8) + 16);
            yRow1[x+1] = static_cast<uint8_t>(((66*r11 + 129*g11 + 25*b11 + 128) >> 8) + 16);

            const int rAvg = (r00 + r01 + r10 + r11) >> 2;
            const int gAvg = (g00 + g01 + g10 + g11) >> 2;
            const int bAvg = (b00 + b01 + b10 + b11) >> 2;
            uvRow[x]   = static_cast<uint8_t>(((-38*rAvg - 74*gAvg + 112*bAvg + 128) >> 8) + 128);
            uvRow[x+1] = static_cast<uint8_t>(((112*rAvg - 94*gAvg - 18*bAvg + 128) >> 8) + 128);
        }
    }

    m_lastEmitNs = ptsNanos;
    emit frameCaptured(m_nv12, w, h, ptsNanos);
}

void ScreenCaptureWin::cleanup() {
    if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
    for (int i = 0; i < kStagingCount; i++) {
        if (m_stagingTex[i]) { m_stagingTex[i]->Release(); m_stagingTex[i] = nullptr; }
    }
    if (m_context)     { m_context->Release();     m_context = nullptr; }
    if (m_device)      { m_device->Release();      m_device = nullptr; }
    if (m_bitmap)      { DeleteObject(m_bitmap);   m_bitmap = nullptr; }
    if (m_memDC)       { DeleteDC(m_memDC);        m_memDC = nullptr; }
    if (m_gdiDC)       { DeleteDC(m_gdiDC);        m_gdiDC = nullptr; }
    m_useGdiFallback = false;
    m_resolution = QSize();
}
