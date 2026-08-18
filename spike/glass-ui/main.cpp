// BetterCast's device page, drawn with liquidDX11's glass kit.
//
// Spike only. Nothing here is wired to BetterCast — the point is to see the
// real layout in the real material, and to measure what it costs on the
// integrated GPU that also drives DXGI capture for streaming.
//
// Frame protocol taken from liquidDX11's own main.cpp:
//   Backdrop::Capture()  (throttled)
//   ImGui NewFrame x3
//   Renderer::BeginFrame(w, h, originX, originY, desktopW, desktopH, cursor)
//   ...widgets...
//   ImGui::Render() -> Renderer::Render(heavySRV, softSRV) -> RenderDrawData

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "glass/backdrop.h"
#include "glass/glass.h"

#include <d3d11.h>
#include <tchar.h>

#include <cstdio>

static ID3D11Device*           g_device      = nullptr;
static ID3D11DeviceContext*    g_ctx         = nullptr;
static IDXGISwapChain*         g_swapchain   = nullptr;
static ID3D11RenderTargetView* g_rtv         = nullptr;
static Glass::Backdrop         g_backdrop;

// Cheap modes, because the whole question is what this costs while idle.
// liquidDX11 already throttles its own backdrop capture, and the renderer
// exposes a resolution scale — both are exactly the levers a "low GPU"
// setting would pull, so they are wired to keys here to be measured.
static int   g_backdropIntervalMs = 33;    // 0 = every frame
static float g_renderScale        = 1.0f;  // >1 renders smaller, upscales
static bool  g_vsync              = true;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &got, &g_ctx);
    }
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_ctx)       { g_ctx->Release();       g_ctx = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (g_device && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_swapchain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                       DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// -- BetterCast's device page, in Glass widgets ------------------------------
//
// Follows liquidDX11's own page idiom: Glass::BeginPanel/EndPanel with
// StatRow, Slider and Button inside. The first attempt used BeginCard nested
// in ImGui::Columns, which is not how the kit is laid out and rendered as a
// broken interface.
static void DrawBetterCastPage(int winW, int winH) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
    ImGui::Begin("##bettercast", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- Sidebar --------------------------------------------------------
    ImGui::BeginChild("sidebar", ImVec2(236.0f, 0.0f), false);
    Glass::BeginPanel("DEVICES");
    static int selected = 2;
    const char* devices[] = { "Overview", "Lenovo Legion Y70",
                              "Stephen's MacBook Air", "Stephen's iPhone" };
    for (int i = 0; i < 4; i++) {
        if (Glass::Chip(devices[i], selected == i)) selected = i;
    }
    Glass::EndPanel();

    Glass::BeginPanel("SEND / RECEIVE");
    Glass::Chip("Send Screen", false);
    Glass::Chip("Receive Screen", false);
    Glass::EndPanel();

    Glass::BeginPanel("SYSTEM");
    Glass::Chip("Wi-Fi Hotspot", false);
    Glass::Chip("Settings", false);
    Glass::Chip("Logs", false);
    Glass::EndPanel();
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Device page ----------------------------------------------------
    ImGui::BeginChild("content", ImVec2(0.0f, 0.0f), false);

    Glass::BeginPanel("STEPHEN'S MACBOOK AIR");
    Glass::StatRow("Address", "192.168.254.111:51820");
    Glass::StatRow("Status", "Available");
    Glass::EndPanel();

    Glass::BeginPanel("STREAM TO THIS DEVICE");
    ImGui::TextWrapped("Gives this device a virtual display of its own, so your "
                       "desktop extends onto it rather than mirroring.");
    ImGui::Dummy(ImVec2(0, 6));
    if (Glass::Button("Send Screen Here", true)) {}
    ImGui::SameLine();
    if (Glass::Button("Configure...", false)) {}
    Glass::EndPanel();

    Glass::BeginPanel("STREAM QUALITY FOR THIS DEVICE");
    static float fps = 60.0f, bitrate = 20.0f;
    ImGui::SetNextItemWidth(240.0f);
    Glass::Slider("Frame rate", &fps, 15.0f, 120.0f, "%.0f FPS");
    static int resIdx = 1;
    const char* resolutions[] = { "Match this PC - 1920 x 1080", "1440 x 900",
                                  "1280 x 800", "1280 x 720" };
    Glass::Dropdown("Resolution", resolutions, 4, &resIdx);
    ImGui::SetNextItemWidth(240.0f);
    Glass::Slider("Bitrate", &bitrate, 2.0f, 100.0f, "%.0f Mbps");
    Glass::EndPanel();

    // ---- Spike instrumentation ------------------------------------------
    //
    // On screen rather than in a log, because the previous round produced
    // "it looks plain" and there was no way to tell whether the backdrop had
    // failed, the SRVs were null, or the material was drawing wrongly.
    // Backdrop::Init ignores the result of CreateDuplication and returns true
    // regardless, so a failed capture looks exactly like success from outside.
    Glass::BeginPanel("SPIKE DIAGNOSTICS");
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.1f FPS  (%.2f ms)",
                  ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
    Glass::StatRow("Frame", buf);
    Glass::StatRow("Backdrop", g_backdrop.ok() ? "capturing" : "FAILED - no glass");
    std::snprintf(buf, sizeof(buf), "%d x %d", g_backdrop.width(), g_backdrop.height());
    Glass::StatRow("Desktop size", buf);
    Glass::StatRow("Heavy SRV", g_backdrop.heavySRV() ? "ok" : "null");
    Glass::StatRow("Soft SRV",  g_backdrop.softSRV()  ? "ok" : "null");
    std::snprintf(buf, sizeof(buf), "%d ms", g_backdropIntervalMs);
    Glass::StatRow("Recapture every", g_backdropIntervalMs == 0 ? "every frame" : buf);
    std::snprintf(buf, sizeof(buf), "%.1fx", g_renderScale);
    Glass::StatRow("Render scale", buf);
    Glass::StatRow("VSync", g_vsync ? "on" : "off");
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("F1 recapture rate   F2 render scale   F3 vsync   Esc quit");
    Glass::EndPanel();

    ImGui::EndChild();
    ImGui::End();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance,
                       nullptr, nullptr, nullptr, nullptr,
                       L"BetterCastGlassSpike", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"BetterCast - Glass Spike",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1180, 760,
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    static Glass::Renderer glass;
    if (!glass.Init(g_device, g_ctx)) {
        ::MessageBoxW(hwnd, L"Glass renderer failed to initialise.", L"Spike", MB_ICONERROR);
        return 1;
    }

    // Every Glass:: free function submits primitives through this global. Not
    // setting it is why the first build exited instantly: BeginCard
    // dereferenced a null renderer before a frame was ever presented.
    Glass::g = &glass;

    g_backdrop.Init(g_device, g_ctx, hwnd);
    Glass::SetAccent(0.34f, 0.64f, 1.0f);   // BetterCast blue

    DWORD lastBlur = 0;
    bool running = true;
    while (running) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        const DWORD now = ::GetTickCount();
        if (g_backdropIntervalMs == 0 || (now - lastBlur) >= (DWORD)g_backdropIntervalMs) {
            lastBlur = now;
            g_backdrop.Capture();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Cost levers, so the idle draw can actually be measured rather than
        // guessed at. These are what a "low GPU" setting would drive.
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
            g_backdropIntervalMs = g_backdropIntervalMs == 0 ? 100
                                 : g_backdropIntervalMs == 100 ? 500 : 0;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            g_renderScale = g_renderScale > 1.5f ? 1.0f : g_renderScale + 0.5f;
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) g_vsync = !g_vsync;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) running = false;

        RECT rc; ::GetClientRect(hwnd, &rc);
        POINT origin = { 0, 0 }; ::ClientToScreen(hwnd, &origin);
        const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

        glass.SetRenderScale(g_renderScale);
        glass.BeginFrame(cw, ch, origin.x, origin.y,
                         g_backdrop.width(), g_backdrop.height(),
                         ImGui::GetIO().MousePos);

        DrawBetterCastPage(cw, ch);

        ImGui::Render();
        const float clear[4] = { 0.02f, 0.03f, 0.05f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);

        glass.Render(g_backdrop.heavySRV(), g_backdrop.softSRV());
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapchain->Present(g_vsync ? 1 : 0, 0);
    }

    Glass::g = nullptr;
    g_backdrop.Shutdown();
    glass.Shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
