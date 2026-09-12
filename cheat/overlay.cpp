// =============================================================================
//  overlay.cpp â€” Win32 + DX11 + ImGui Overlay (render ala H-V2)
//  Layout menu dipertahankan, konten fitur mengikuti H-V2 build 14181.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <array>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>

#include "overlay.h"
#include "memory.h"
#include "../math/math_utils.h"
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_impl_win32.h"
#include "../third_party/imgui/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Overlay {

static IDCompositionDevice*  s_dcomp = nullptr;
static IDCompositionTarget*  s_dcomp_target = nullptr;
static IDCompositionVisual*  s_dcomp_visual = nullptr;
static HANDLE s_frame_ready = nullptr;
static UINT s_swap_flags = 0;
static bool s_backend_ready = false, s_resuming = true, s_occluded = false;
static bool s_allow_render = true;
static DWORD s_probe_ms = 0;
static uint32_t s_degrade = 0;
static RenderStats s_stats{};
static std::array<double, 120> s_frame_times{};
static size_t s_frame_count = 0, s_frame_index = 0;
static double s_frame_sum = 0;
static uint32_t s_frames_sorted = 0;
static std::chrono::steady_clock::time_point s_last_frame = std::chrono::steady_clock::now();
static HWND s_game_window = nullptr;

const RenderStats& Stats() noexcept { return s_stats; }

static double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

static void PumpMsgs() {
    MSG m{};
    while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
}

static bool FrameVisible() noexcept {
    HWND fg = ::GetForegroundWindow();
    return g_menu_open ||
        (s_game_window && ::IsWindow(s_game_window) && !::IsIconic(s_game_window) &&
         fg == s_game_window);
}

void WaitForFrame() {
    auto t0 = std::chrono::steady_clock::now();
    s_allow_render = true;
    if (!FrameVisible() || !g_pSwapChain || s_occluded || FAILED(s_stats.last_result)) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } else if (s_frame_ready) {
        for (;;) {
            DWORD r = MsgWaitForMultipleObjectsEx(
                1, &s_frame_ready, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 1) { PumpMsgs(); continue; }
            DWORD now = GetTickCount();
            s_degrade += 5;
            if (s_probe_ms && now - s_probe_ms < 2000) { s_allow_render = false; ++s_stats.render_skips; }
            else s_probe_ms = now;
            break;
        }
        if (s_allow_render) {
            double remain = 1000.0 / 90.0 - MsSince(s_last_frame);
            while (remain > 0.5) {
                DWORD r = MsgWaitForMultipleObjectsEx(
                    0, nullptr, (DWORD)remain + 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (r == WAIT_OBJECT_0) PumpMsgs();
                remain = 1000.0 / 90.0 - MsSince(s_last_frame);
            }
        }
    } else {
        MsgWaitForMultipleObjectsEx(0, nullptr, 4, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    s_stats.wait_ms = MsSince(t0);
}

static void SyncGameWindow(DWORD pid) {
    if (s_game_window) {
        DWORD p = 0;
        ::GetWindowThreadProcessId(s_game_window, &p);
        if (p != pid || !::IsWindow(s_game_window)) s_game_window = nullptr;
    }
    static DWORD search_ms = 0;
    DWORD now = GetTickCount();
    if (!s_game_window && pid && (!search_ms || now - search_ms >= 500)) {
        search_ms = now;
        s_game_window = ::FindWindowA("SDL_app", nullptr);
        if (!s_game_window) s_game_window = ::FindWindowA(nullptr, "Counter-Strike 2");
    }
    bool visible = FrameVisible();
    if (!visible) {
        if (::IsWindowVisible(g_hwnd)) ::ShowWindow(g_hwnd, SW_HIDE);
        s_resuming = true;
        ++s_stats.hidden_frames;
        return;
    }
    RECT desired{};
    bool have = false;
    if (s_game_window && !::IsIconic(s_game_window)) {
        RECT rc{}; POINT pt{};
        if (::GetClientRect(s_game_window, &rc) && ::ClientToScreen(s_game_window, &pt) &&
            rc.right > 0 && rc.bottom > 0) {
            desired = { pt.x, pt.y, pt.x + rc.right, pt.y + rc.bottom };
            have = true;
        }
    }
    if (!have) ::GetWindowRect(g_hwnd, &desired);
    RECT cur{};
    ::GetWindowRect(g_hwnd, &cur);
    bool moved = memcmp(&desired, &cur, sizeof cur) != 0;
    bool topmost = (::GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (moved || !topmost || !::IsWindowVisible(g_hwnd)) {
        ::SetWindowPos(g_hwnd, HWND_TOPMOST, desired.left, desired.top,
                       desired.right - desired.left, desired.bottom - desired.top,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW | (moved ? 0 : SWP_NOMOVE | SWP_NOSIZE));
    }
}

static void FocusGameWindow() {
    HWND game = ::FindWindowA("SDL_app", nullptr);
    if (!game) game = ::FindWindowA(nullptr, "Counter-Strike 2");
    if (game) {
        if (::IsIconic(game)) ::ShowWindow(game, SW_RESTORE);
        ::SetForegroundWindow(game);
    }
}

void SetMenuOpen(bool open) noexcept {
    g_menu_open = open;
    g_input_blocked = open;
    LONG_PTR ex = ::GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    ex = open ? (ex & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE))
              : (ex | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    ::SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
    if (open) { ::ShowWindow(g_hwnd, SW_SHOW); ::SetForegroundWindow(g_hwnd); }
    else if (s_game_window && !::IsIconic(s_game_window)) {
        ::ShowWindow(g_hwnd, SW_HIDE);
        ::ClipCursor(nullptr);
        ::SetForegroundWindow(s_game_window);
        PumpMsgs();
        ::ShowWindow(g_hwnd, SW_SHOW);
    }
    s_resuming = true;
}

static void EnterEspMode() {
    SetMenuOpen(false);
    FocusGameWindow();
}

static void EnterMenuMode() {
    LONG_PTR ex = ::GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    ::SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE,
        (ex & ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) | WS_EX_TOPMOST);
    if (::IsIconic(g_hwnd)) ::ShowWindow(g_hwnd, SW_RESTORE);
    RECT rc{};
    if (::GetWindowRect(g_hwnd, &rc)) {
        int vw = ::GetSystemMetrics(SM_CXSCREEN);
        int vh = ::GetSystemMetrics(SM_CYSCREEN);
        if (rc.right <= 0 || rc.bottom <= 0 || rc.left >= vw || rc.top >= vh ||
            rc.left <= -20000 || rc.top <= -20000) {
            ::SetWindowPos(g_hwnd, nullptr, 0, 0,
                           g_screen_width, g_screen_height,
                           SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    ::ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    HWND fg = ::GetForegroundWindow();
    if (fg && fg != g_hwnd) {
        DWORD fg_tid  = ::GetWindowThreadProcessId(fg, nullptr);
        DWORD our_tid = ::GetCurrentThreadId();
        if (fg_tid && our_tid && fg_tid != our_tid) {
            ::AttachThreadInput(our_tid, fg_tid, TRUE);
            ::SetForegroundWindow(g_hwnd);
            ::AttachThreadInput(our_tid, fg_tid, FALSE);
        }
    } else {
        ::SetForegroundWindow(g_hwnd);
    }
    ::SetActiveWindow(g_hwnd);
}

// =============================================================================
//  WndProc
// =============================================================================
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            UINT nw = LOWORD(lParam), nh = HIWORD(lParam);
            if (nw == 0 || nh == 0) return 0;
            if (g_mainRenderTargetView) {
                g_mainRenderTargetView->Release();
                g_mainRenderTargetView = nullptr;
            }
            if (g_pSwapChain) {
                g_pSwapChain->ResizeBuffers(
                    0, nw, nh,
                    DXGI_FORMAT_UNKNOWN, 0);
                ID3D11Texture2D* pBB = nullptr;
                if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBB))) && pBB) {
                    g_pd3dDevice->CreateRenderTargetView(pBB, nullptr, &g_mainRenderTargetView);
                    pBB->Release();
                }
            }
        }
        return 0;

    case WM_NCHITTEST: if (!g_menu_open) return HTTRANSPARENT; break;
    case WM_ERASEBKGND: return 1;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        break;

    case WM_CLOSE:
        g_running = false;
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// =============================================================================
//  Theme
// =============================================================================
static void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding  = 10.f; s.ChildRounding  = 8.f;
    s.FrameRounding   = 6.f;  s.PopupRounding  = 8.f;
    s.ScrollbarRounding = 6.f; s.GrabRounding  = 6.f;
    s.TabRounding     = 6.f;
    s.WindowBorderSize = 1.f; s.FrameBorderSize = 0.f;
    s.WindowPadding   = {12.f, 10.f};
    s.ItemSpacing     = {8.f, 6.f};

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = {0.95f,0.96f,0.98f,1.f};
    c[ImGuiCol_TextDisabled]         = {0.50f,0.55f,0.62f,1.f};
    c[ImGuiCol_WindowBg]             = {0.08f,0.09f,0.13f,0.97f};
    c[ImGuiCol_ChildBg]              = {0.11f,0.13f,0.18f,0.70f};
    c[ImGuiCol_PopupBg]              = {0.10f,0.11f,0.15f,0.97f};
    c[ImGuiCol_Border]               = {0.22f,0.25f,0.35f,0.60f};
    c[ImGuiCol_FrameBg]              = {0.15f,0.17f,0.24f,1.f};
    c[ImGuiCol_FrameBgHovered]       = {0.22f,0.26f,0.36f,1.f};
    c[ImGuiCol_FrameBgActive]        = {0.28f,0.33f,0.46f,1.f};
    c[ImGuiCol_TitleBg]              = {0.07f,0.08f,0.11f,1.f};
    c[ImGuiCol_TitleBgActive]        = {0.12f,0.14f,0.20f,1.f};
    c[ImGuiCol_CheckMark]            = {0.00f,0.85f,1.00f,1.f};
    c[ImGuiCol_SliderGrab]           = {0.00f,0.85f,1.00f,1.f};
    c[ImGuiCol_SliderGrabActive]     = {0.35f,0.92f,1.00f,1.f};
    c[ImGuiCol_Button]               = {0.18f,0.22f,0.30f,1.f};
    c[ImGuiCol_ButtonHovered]        = {0.26f,0.32f,0.44f,1.f};
    c[ImGuiCol_ButtonActive]         = {0.00f,0.75f,0.95f,1.f};
    c[ImGuiCol_Header]               = {0.16f,0.20f,0.28f,1.f};
    c[ImGuiCol_HeaderHovered]        = {0.24f,0.30f,0.42f,1.f};
    c[ImGuiCol_HeaderActive]         = {0.00f,0.75f,0.95f,1.f};
    c[ImGuiCol_Tab]                  = {0.12f,0.14f,0.20f,1.f};
    c[ImGuiCol_TabHovered]           = {0.24f,0.30f,0.42f,1.f};
    c[ImGuiCol_TabActive]            = {0.18f,0.22f,0.32f,1.f};
    c[ImGuiCol_Separator]            = {0.22f,0.26f,0.36f,0.60f};
    c[ImGuiCol_ScrollbarBg]          = {0.07f,0.08f,0.11f,0.50f};
    c[ImGuiCol_ScrollbarGrab]        = {0.22f,0.26f,0.36f,1.f};
    c[ImGuiCol_ScrollbarGrabHovered] = {0.32f,0.37f,0.50f,1.f};
    c[ImGuiCol_ScrollbarGrabActive]  = {0.42f,0.48f,0.64f,1.f};
}

// =============================================================================
//  Init
// =============================================================================
bool Init(HINSTANCE hInstance) {
    g_hinstance = hInstance;
    g_screen_width  = GetSystemMetrics(SM_CXSCREEN);
    g_screen_height = GetSystemMetrics(SM_CYSCREEN);
    if (g_screen_width  <= 0) g_screen_width  = 1920;
    if (g_screen_height <= 0) g_screen_height = 1080;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"NVIDIA_Share_OverlayHost";
    ::RegisterClassExW(&wc);

    g_hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW,
        L"NVIDIA_Share_OverlayHost",
        L"CS2 Overlay",
        WS_POPUP,
        0, 0, g_screen_width, g_screen_height,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return false;

    ::SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL feat_arr[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, feat_arr, 2, D3D11_SDK_VERSION,
            &g_pd3dDevice, nullptr, &g_pd3dDeviceContext);
        s_stats.software = FAILED(hr);
        if (FAILED(hr))
            hr = ::D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                flags, feat_arr, 2, D3D11_SDK_VERSION,
                &g_pd3dDevice, nullptr, &g_pd3dDeviceContext);
        if (FAILED(hr)) { s_stats.last_result = hr; return false; }

        IDXGIDevice* device = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        IDXGISwapChain1* swap1 = nullptr;
        hr = g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) hr = device->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = (UINT)g_screen_width; d.Height = (UINT)g_screen_height;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (SUCCEEDED(hr)) {
            hr = factory->CreateSwapChainForComposition(g_pd3dDevice, &d, nullptr, &swap1);
            if (FAILED(hr)) {
                d.Flags = 0;
                hr = factory->CreateSwapChainForComposition(g_pd3dDevice, &d, nullptr, &swap1);
            }
        }
        s_swap_flags = d.Flags;
        if (SUCCEEDED(hr)) hr = swap1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain));
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(device, IID_PPV_ARGS(&s_dcomp));
        if (SUCCEEDED(hr)) hr = s_dcomp->CreateTargetForHwnd(g_hwnd, TRUE, &s_dcomp_target);
        if (SUCCEEDED(hr)) hr = s_dcomp->CreateVisual(&s_dcomp_visual);
        if (SUCCEEDED(hr)) hr = s_dcomp_visual->SetContent(swap1);
        if (SUCCEEDED(hr)) hr = s_dcomp_target->SetRoot(s_dcomp_visual);
        if (SUCCEEDED(hr)) hr = s_dcomp->Commit();
        if (SUCCEEDED(hr) && s_swap_flags) {
            IDXGISwapChain2* s2 = nullptr;
            if (SUCCEEDED(swap1->QueryInterface(IID_PPV_ARGS(&s2)))) {
                s2->SetMaximumFrameLatency(2);
                s_frame_ready = s2->GetFrameLatencyWaitableObject();
                s2->Release();
            }
        }
        if (swap1) swap1->Release();
        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (device) device->Release();
        s_stats.last_result = hr;
        if (FAILED(hr)) return false;

        ID3D11Texture2D* pBB = nullptr;
        if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBB))) && pBB) {
            g_pd3dDevice->CreateRenderTargetView(pBB, nullptr, &g_mainRenderTargetView);
            pBB->Release();
        }
        if (!g_mainRenderTargetView) return false;
        s_stats.composition = true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();

    ApplyTheme();
    ImGui_ImplWin32_Init(g_hwnd);
    if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) return false;
    s_backend_ready = true;

    ::ShowWindow(g_hwnd, SW_SHOW);
    g_running = true;
    SetMenuOpen(true);
    return true;
}

static void DestroyDComp() {
    s_stats.composition = false;
    s_occluded = false;
    if (s_backend_ready) { ImGui_ImplDX11_Shutdown(); s_backend_ready = false; }
    if (s_dcomp_target) s_dcomp_target->SetRoot(nullptr);
    if (s_dcomp) s_dcomp->Commit();
    if (s_dcomp_visual) { s_dcomp_visual->Release(); s_dcomp_visual = nullptr; }
    if (s_dcomp_target) { s_dcomp_target->Release(); s_dcomp_target = nullptr; }
    if (s_dcomp) { s_dcomp->Release(); s_dcomp = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->ClearState(); g_pd3dDeviceContext->Flush(); }
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (s_frame_ready) { CloseHandle(s_frame_ready); s_frame_ready = nullptr; }
}

// =============================================================================
//  ProcessEvents
// =============================================================================
void ProcessEvents() {
    MSG msg{};
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    static bool ins_last = false;
    bool ins_now = (::GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ins_now && !ins_last) {
        if (g_menu_open) EnterEspMode();
        else { SetMenuOpen(true); EnterMenuMode(); }
    }
    ins_last = ins_now;

    if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
        (::GetAsyncKeyState(VK_END)   & 0x8000))
        g_running = false;
}

// =============================================================================
//  Helpers
// =============================================================================
static void DrawCornerBox(ImDrawList* dl, float x, float y,
                           float w, float h, ImU32 col, float t) {
    float lw = w * 0.25f, lh = h * 0.25f;
    dl->AddLine({x,     y    }, {x+lw,  y    }, col, t);
    dl->AddLine({x,     y    }, {x,     y+lh }, col, t);
    dl->AddLine({x+w,   y    }, {x+w-lw,y    }, col, t);
    dl->AddLine({x+w,   y    }, {x+w,   y+lh }, col, t);
    dl->AddLine({x,     y+h  }, {x+lw,  y+h  }, col, t);
    dl->AddLine({x,     y+h  }, {x,     y+h-lh}, col, t);
    dl->AddLine({x+w,   y+h  }, {x+w-lw,y+h  }, col, t);
    dl->AddLine({x+w,   y+h  }, {x+w,   y+h-lh}, col, t);
}

static ImVec4 Rgba(const float* c) { return {c[0], c[1], c[2], c[3]}; }

// Panah musuh di luar layar (port H-V2)
static void DrawOffscreenArrow(ImDrawList* dl, const Vec3& local_eye, float local_yaw,
                               bool yaw_valid, const PlayerData& p,
                               const Mat4x4& vm, float sw, float sh) {
    if (!g_esp_cfg.offscreen_arrows || !yaw_valid) return;
    Vec2 projected{};
    float rad = 0;
    if (vm.WorldToScreen(p.origin, projected, sw, sh)) {
        rad = atan2f((projected.x - sw * 0.5f) / sw, -(projected.y - sh * 0.5f) / sh);
    } else {
        Vec3 dir = p.origin - local_eye;
        const auto& m = vm.m;
        float n = sqrtf(m[0][0]*m[0][0] + m[0][1]*m[0][1]);
        if (n < 0.001f) return;
        float side = (dir.x * m[0][0] + dir.y * m[0][1]) / n;
        float yaw = math::ToRadians(local_yaw);
        float fwd = dir.x * cosf(yaw) + dir.y * sinf(yaw);
        rad = atan2f(side, fwd);
    }
    float cx = sw * 0.5f, cy = sh * 0.5f;
    float r  = (sw < sh ? sw : sh) * 0.42f;
    float px = cx + sinf(rad) * r;
    float py = cy - cosf(rad) * r;
    ImU32 col = ImGui::ColorConvertFloat4ToU32(Rgba(g_esp_cfg.arrow_color));
    ImVec2 tip{px + sinf(rad) * 10.f, py - cosf(rad) * 10.f};
    ImVec2 l  {px + sinf(rad + 2.5f) * 10.f, py - cosf(rad + 2.5f) * 10.f};
    ImVec2 rr {px + sinf(rad - 2.5f) * 10.f, py - cosf(rad - 2.5f) * 10.f};
    dl->AddTriangleFilled(tip, l, rr, col);
}

// Bar bom (port H-V2)
static void DrawBombBar(ImDrawList* dl, const BombInfo& bomb, float sw) {
    if (!g_misc_cfg.bomb_timer || !bomb.planted) return;
    float w = 220.f, x = (sw - w) * 0.5f, y = 26.f;
    char txt[96];
    if (bomb.defusing && bomb.defuse_remaining >= 0.f)
        snprintf(txt, sizeof(txt), "DEFUSING %.1fs", bomb.defuse_remaining);
    else if (bomb.countdown >= 0.f)
        snprintf(txt, sizeof(txt), "C4 %.1fs", bomb.countdown);
    else
        snprintf(txt, sizeof(txt), "C4 PLANTED");
    float t01 = 0.f;
    if (bomb.defusing && bomb.defuse_remaining >= 0.f && bomb.defuse_length > 0.01f)
        t01 = bomb.defuse_remaining / bomb.defuse_length;
    else if (bomb.countdown >= 0.f && bomb.timer_length > 0.01f)
        t01 = bomb.countdown / bomb.timer_length;
    if (t01 < 0.f) t01 = 0.f; if (t01 > 1.f) t01 = 1.f;
    ImU32 bar = bomb.defusing ? IM_COL32(0, 220, 120, 230) : IM_COL32(255, 80, 60, 230);
    dl->AddRectFilled({x, y}, {x + w, y + 16.f}, IM_COL32(0, 0, 0, 160), 3.f);
    dl->AddRectFilled({x + 2.f, y + 2.f}, {x + 2.f + (w - 4.f) * t01, y + 14.f}, bar, 2.f);
    ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText({x + (w - ts.x) * 0.5f, y + 18.f}, IM_COL32(255, 255, 255, 235), txt);
}

// =============================================================================
//  RenderFrame
// =============================================================================
void RenderFrame(const std::array<PlayerData, k_max_entities>& players,
                 int local_team, bool process_attached,
                 const Mat4x4& view_matrix,
                 const Vec3& local_eye, float local_yaw, bool yaw_valid,
                 bool can_write, const BombInfo& bomb, const HitState& hit,
                 int game_build, double worker_hz, DWORD sample_ms)
{
    SyncGameWindow(process_attached ? (DWORD)0 : (DWORD)0);
    if (!FrameVisible()) { ++s_stats.hidden_frames; return; }
    if (s_occluded && g_pSwapChain) {
        HRESULT hr = g_pSwapChain->Present(0, DXGI_PRESENT_TEST);
        s_stats.last_result = hr;
        if (hr == DXGI_STATUS_OCCLUDED) return;
        s_occluded = false;
        s_resuming = true;
    }
    if (!g_pd3dDevice || FAILED(s_stats.last_result)) {
        static DWORD retry = 0;
        DWORD now = GetTickCount();
        if (retry && now - retry < 1000) return;
        retry = now;
        DestroyDComp();
        return;
    }
    if (!s_allow_render) return;

    // Skip frame statis saat menu tertutup: hash FNV scene, min refresh 4Hz
    static uint64_t last_sig = 0;
    if (!g_menu_open) {
        if (s_resuming) last_sig = 0;
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const unsigned char* b = (const unsigned char*)p;
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        };
        DWORD quarter = GetTickCount() >> 8;
        mix(&quarter, sizeof quarter);
        mix(&view_matrix, sizeof view_matrix);
        mix(&local_eye, sizeof local_eye);
        mix(&players, sizeof players);
        mix(&bomb, sizeof bomb);
        mix(&hit, sizeof hit);
        mix(&g_esp_cfg, sizeof g_esp_cfg);
        mix(&g_misc_cfg, sizeof g_misc_cfg);
        mix(&g_crosshair_cfg, sizeof g_crosshair_cfg);
        if (h == last_sig) { ++s_stats.static_skips; Sleep(15); return; }
        last_sig = h;
    } else last_sig = 0;

    if (sample_ms) s_stats.snapshot_age_ms = (double)(GetTickCount() - sample_ms);
    auto frame_start = std::chrono::steady_clock::now();
    if (!s_resuming) {
        double dt = MsSince(s_last_frame);
        size_t slot = s_frame_index % s_frame_times.size();
        if (s_frame_count == s_frame_times.size()) s_frame_sum -= s_frame_times[slot];
        s_frame_times[slot] = dt; s_frame_sum += dt;
        s_frame_index++;
        s_frame_count = s_frame_count + 1 < s_frame_times.size() ? s_frame_count + 1 : s_frame_times.size();
        if (s_degrade && dt < 20.0 && s_stats.present_ms < 8.0) --s_degrade;
    }
    s_last_frame = frame_start;
    ++s_stats.frames;
    if (s_frame_count >= 32 && ++s_frames_sorted >= 32) {
        s_frames_sorted = 0;
        auto ordered = s_frame_times;
        std::sort(ordered.begin(), ordered.begin() + s_frame_count);
        s_stats.frame_p95_ms = ordered[(s_frame_count - 1) * 95 / 100];
        if (s_frame_sum > 0) s_stats.loop_fps = 1000.0 * (double)s_frame_count / s_frame_sum;
    }
    auto ui_start = std::chrono::steady_clock::now();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::PushStyleColor(ImGuiCol_WindowBg,   ImVec4(0.08f, 0.09f, 0.13f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,    ImVec4(0.11f, 0.13f, 0.18f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,    ImVec4(0.10f, 0.11f, 0.15f, 1.00f));

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float cx = g_screen_width  * 0.5f;
    float cy = g_screen_height * 0.5f;
    float sw = (float)g_screen_width;
    float sh = (float)g_screen_height;

    // ---- Custom Crosshair ----
    if (g_crosshair_cfg.enabled) {
        ImU32 cc = ImGui::ColorConvertFloat4ToU32(Rgba(g_crosshair_cfg.color));
        float sz = g_crosshair_cfg.size;
        float gp = g_crosshair_cfg.gap;
        float tk = g_crosshair_cfg.thickness;
        dl->AddLine({cx, cy-gp-sz}, {cx, cy-gp}, cc, tk);
        dl->AddLine({cx, cy+gp},    {cx, cy+gp+sz}, cc, tk);
        dl->AddLine({cx-gp-sz, cy}, {cx-gp, cy}, cc, tk);
        dl->AddLine({cx+gp, cy},    {cx+gp+sz, cy}, cc, tk);
    }

    // ---- ESP (port H-V2) ----
    if (process_attached && g_esp_cfg.enabled && !view_matrix.IsZeroMatrix()) {
        for (int i = 1; i < k_max_entities; ++i) {
            const PlayerData& p = players[i];
            if (!p.pawn_ptr) continue;
            if (!p.alive) continue;
            if (p.dormant && p.distance > 50.f) continue;
            if (!g_esp_cfg.teammates && p.team == local_team) continue;

            Vec3 head3 = !p.bones[schemas::bones::Head].IsZero()
                ? p.bones[schemas::bones::Head] : p.head_pos;
            if (head3.IsZero()) head3 = Vec3{p.origin.x, p.origin.y, p.origin.z + 75.f};
            Vec2 head;
            if (!view_matrix.WorldToScreen(head3, head, sw, sh)) {
                if (!p.dormant) DrawOffscreenArrow(dl, local_eye, local_yaw, yaw_valid, p, view_matrix, sw, sh);
                continue;
            }
            if (head.x < 0.f || head.x > sw || head.y < 0.f || head.y > sh) {
                if (!p.dormant) DrawOffscreenArrow(dl, local_eye, local_yaw, yaw_valid, p, view_matrix, sw, sh);
                continue;
            }

            Vec2 f1, f2, feet;
            bool have_feet = false;
            float feet_y = 0.f, feet_x = 0.f;
            const Vec3& bl = p.bones[schemas::bones::LeftFoot];
            const Vec3& br = p.bones[schemas::bones::RightFoot];
            if (!bl.IsZero() && view_matrix.WorldToScreen(bl, f1, sw, sh)) {
                feet_y = f1.y; feet_x = f1.x; have_feet = true;
            }
            if (!br.IsZero() && view_matrix.WorldToScreen(br, f2, sw, sh)) {
                if (!have_feet || f2.y > feet_y) { feet_y = f2.y; feet_x = f2.x; }
                have_feet = true;
            }
            if (have_feet) { feet.x = feet_x; feet.y = feet_y; }
            else if (!view_matrix.WorldToScreen(p.origin, feet, sw, sh)) continue;

            float h = fabsf(feet.y - head.y);
            if (h < 4.f) continue;
            float w   = h * 0.48f;
            float lft = head.x - w * 0.5f;
            float top = head.y;

            float alpha = (p.dormant || p.ghost) ? 0.45f : 1.0f;
            bool  is_enemy = (p.team != local_team);

            ImVec4 base_col = is_enemy
                ? Rgba(g_esp_cfg.box_color)
                : Rgba(g_esp_cfg.team_color);
            base_col.w *= alpha;
            ImU32 box_col = ImGui::ColorConvertFloat4ToU32(base_col);
            int first_vertex = dl->VtxBuffer.Size;

            if (g_esp_cfg.draw_box) {
                if (g_esp_cfg.box_style == 0)
                    dl->AddRect({lft, top}, {lft+w, top+h}, box_col, 0.f, 0, 1.5f);
                else
                    DrawCornerBox(dl, lft, top, w, h, box_col, 1.5f);
            }

            // Health bar + ghost damage putih
            if (g_esp_cfg.draw_health) {
                float pct = (float)p.health / 100.f;
                if (pct < 0.f) pct = 0.f;
                if (pct > 1.f) pct = 1.f;
                static float ghost_hp[66];
                static DWORD ghost_ms[66]{};
                static uintptr_t ghost_pawn[66]{};
                static bool ghost_init = false;
                if (!ghost_init) { for (int gi = 0; gi < 66; ++gi) ghost_hp[gi] = -1.f; ghost_init = true; }
                int ci = p.controller_index;
                if (ci >= 1 && ci <= 64 && !p.ghost && !p.dormant) {
                    DWORD tnow = GetTickCount();
                    if (ghost_pawn[ci] != p.pawn_ptr || ghost_hp[ci] < 0.f) {
                        ghost_hp[ci] = (float)p.health; ghost_pawn[ci] = p.pawn_ptr; ghost_ms[ci] = tnow;
                    } else if ((float)p.health >= ghost_hp[ci] - 0.5f) {
                        ghost_hp[ci] = (float)p.health; ghost_ms[ci] = tnow;
                    } else {
                        float dec = (float)(tnow - ghost_ms[ci]) * (100.f / 500.f);
                        if (dec < 0.f) dec = 0.f; if (dec > 100.f) dec = 100.f;
                        ghost_hp[ci] = (float)p.health > ghost_hp[ci] - dec ? (float)p.health : ghost_hp[ci] - dec;
                        ghost_ms[ci] = tnow;
                        float gpct = ghost_hp[ci] / 100.f;
                        if (gpct < 0.f) gpct = 0.f; if (gpct > 1.f) gpct = 1.f;
                        if (gpct > pct)
                            dl->AddRectFilled({lft - 6.f, top + h - h * gpct}, {lft - 2.f, top + h - h * pct},
                                              IM_COL32(255, 255, 255, 200));
                    }
                }
                ImU32 hp_col = IM_COL32((int)((1.f-pct)*255), (int)(pct*255), 40, 255);
                dl->AddRectFilled({lft-6.f, top+h-h*pct}, {lft-2.f, top+h}, hp_col);
                dl->AddRect({lft-7.f, top-1.f}, {lft-1.f, top+h+1.f}, IM_COL32(0,0,0,180));
            }

            // Armor bar (kanan)
            if (g_esp_cfg.draw_armor) {
                float pct = (float)p.armor / 100.f;
                if (pct < 0.f) pct = 0.f; if (pct > 1.f) pct = 1.f;
                dl->AddRectFilled({lft + w + 2.f, top + h - h * pct}, {lft + w + 6.f, top + h},
                                  IM_COL32(90, 160, 255, 255));
                dl->AddRect({lft + w + 1.f, top - 1.f}, {lft + w + 7.f, top + h + 1.f}, IM_COL32(0,0,0,180));
            }

            // Ammo bar (bawah)
            if (g_esp_cfg.draw_ammo && p.clip >= 0 && p.clip_capacity > 0) {
                float pct = (float)p.clip / (float)p.clip_capacity;
                if (pct > 1.f) pct = 1.f; if (pct < 0.f) pct = 0.f;
                dl->AddRectFilled({lft, top + h + 2.f}, {lft + w * pct, top + h + 5.f},
                                  IM_COL32(255, 220, 90, 230));
            } else if (g_esp_cfg.draw_ammo && p.clip >= 0 && p.clip_capacity <= 0) {
                char ammo[24]; snprintf(ammo, sizeof ammo, "%d", p.clip);
                dl->AddText({lft + w + 10.f, top + h}, IM_COL32(255, 220, 90, 230), ammo);
            }

            // Nama + jarak
            if (g_esp_cfg.draw_name || g_esp_cfg.draw_distance) {
                char buf[192];
                if (g_esp_cfg.draw_name && p.name[0] && g_esp_cfg.draw_distance)
                    snprintf(buf, sizeof(buf), "%s [%.0fm]", p.name, p.distance);
                else if (g_esp_cfg.draw_name && p.name[0])
                    snprintf(buf, sizeof(buf), "%s", p.name);
                else if (g_esp_cfg.draw_distance)
                    snprintf(buf, sizeof(buf), "%.0fm", p.distance);
                else buf[0] = 0;
                if (buf[0]) {
                    ImVec2 ts = ImGui::CalcTextSize(buf);
                    dl->AddText({head.x - ts.x*0.5f, top - 16.f}, IM_COL32(255,255,255,255), buf);
                }
            }

            // Nama senjata
            if (g_esp_cfg.draw_weapon && p.weapon_def > 0) {
                const char* wn = WeaponName(p.weapon_def);
                ImVec2 ts = ImGui::CalcTextSize(wn);
                dl->AddText({head.x - ts.x*0.5f, top + h + 7.f}, IM_COL32(200, 220, 255, 235), wn);
            }

            // Head dot (lingkaran kosong = estimasi)
            if (g_esp_cfg.draw_head_dot) {
                if (p.head_estimated)
                    dl->AddCircle({head.x, head.y}, 4.f, IM_COL32(255, 220, 120, 220));
                else
                    dl->AddCircleFilled({head.x, head.y}, 3.f, IM_COL32(255, 255, 255, 220));
            }

            // Snapline
            if (g_esp_cfg.draw_snaplines) {
                ImU32 sc = ImGui::ColorConvertFloat4ToU32(Rgba(g_esp_cfg.snap_color));
                dl->AddLine({cx, sh}, {feet.x, feet.y}, sc, 1.2f);
            }

            // Bones
            if (g_esp_cfg.draw_bones) {
                ImU32 bc = ImGui::ColorConvertFloat4ToU32(Rgba(g_esp_cfg.bone_color));
                static const std::pair<int,int> bpairs[] = {
                    {schemas::bones::Head,   schemas::bones::Neck},
                    {schemas::bones::Neck,   schemas::bones::Spine3},
                    {schemas::bones::Spine3, schemas::bones::Spine2},
                    {schemas::bones::Spine2, schemas::bones::Spine1},
                    {schemas::bones::Spine1, schemas::bones::Pelvis},
                    {schemas::bones::Neck,   schemas::bones::LeftHand},
                    {schemas::bones::Neck,   schemas::bones::RightHand},
                    {schemas::bones::Pelvis, schemas::bones::LeftFoot},
                    {schemas::bones::Pelvis, schemas::bones::RightFoot},
                };
                for (auto [b1, b2] : bpairs) {
                    const Vec3& p1 = p.bones[b1];
                    const Vec3& p2 = p.bones[b2];
                    Vec2 s1, s2;
                    if (!p1.IsZero() && !p2.IsZero() &&
                        view_matrix.WorldToScreen(p1, s1, sw, sh) &&
                        view_matrix.WorldToScreen(p2, s2, sw, sh))
                        dl->AddLine({s1.x,s1.y},{s2.x,s2.y}, bc, 1.2f);
                }
            }

            // EyeRay: garis arah pandang musuh
            if (g_esp_cfg.draw_eyeray && p.eye_yaw_valid && !p.ghost && !p.dormant) {
                float pr = math::ToRadians(p.eye_pitch), yw = math::ToRadians(p.eye_yaw);
                float planar = 50.f * cosf(pr);
                Vec3 tip{head3.x + cosf(yw) * planar, head3.y + sinf(yw) * planar,
                         head3.z - sinf(pr) * 50.f};
                Vec2 s2;
                if (view_matrix.WorldToScreen(tip, s2, sw, sh)) {
                    ImU32 ec = ImGui::ColorConvertFloat4ToU32(Rgba(g_esp_cfg.bone_color));
                    dl->AddLine({head.x, head.y}, {s2.x, s2.y}, ec, 1.3f);
                }
            }

            // Terapkan alpha ghost ke semua vertex player ini
            if (alpha < 1.f) {
                for (int vi = first_vertex; vi < dl->VtxBuffer.Size; ++vi) {
                    ImU32& c = dl->VtxBuffer[vi].col;
                    ImU32 a = (c >> IM_COL32_A_SHIFT) & 255;
                    c = (c & ~IM_COL32_A_MASK) | (ImU32(a * alpha) << IM_COL32_A_SHIFT);
                }
            }
        }
    }

    // ---- Hitsound indikator: kilat kecil dekat crosshair ----
    if (hit.active && GetTickCount() - hit.last_ms < 300) {
        ImU32 hc = hit.kill ? IM_COL32(255, 60, 60, 240) : IM_COL32(255, 255, 255, 240);
        float g = 5.f, l = 8.f;
        dl->AddLine({cx-g, cy-g}, {cx-g-l, cy-g-l}, hc, 2.f);
        dl->AddLine({cx+g, cy-g}, {cx+g+l, cy-g-l}, hc, 2.f);
        dl->AddLine({cx-g, cy+g}, {cx-g-l, cy+g+l}, hc, 2.f);
        dl->AddLine({cx+g, cy+g}, {cx+g+l, cy+g+l}, hc, 2.f);
        if (hit.kill)
            dl->AddText({cx - 30.f, cy + 14.f}, hc, "KILL");
    }

    // ---- Bom bar ----
    DrawBombBar(dl, bomb, sw);

    // ---- Watermark (ritme render nyata, bukan rata-rata ImGui) ----
    if (g_misc_cfg.watermark) {
        char wm[96];
        double fps = s_stats.loop_fps > 0.0 ? s_stats.loop_fps : ImGui::GetIO().Framerate;
        snprintf(wm, sizeof(wm), "CS2 | %s | %.0f fps",
                 can_write ? "RW" : "READ-ONLY", fps);
        dl->AddText({(sw - ImGui::CalcTextSize(wm).x - 10.f) > 8.f ? sw - ImGui::CalcTextSize(wm).x - 10.f : 8.f, 8.f},
                    IM_COL32(0, 220, 200, 200), wm);
    }

    // ---- Indikator saat menu disembunyikan ----
    if (!g_menu_open) {
        ImU32 ic = process_attached ? IM_COL32(0, 255, 200, 210) : IM_COL32(255, 90, 70, 210);
        dl->AddText({10.f, 8.f}, ic, "cs2 [INSERT]");
    }

    // ---- Menu ImGui (layout dipertahankan) ----
    if (g_menu_open) {
        ImGui::SetNextWindowSize({650.f, 510.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({50.f, 50.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.97f);

        ImGui::Begin("CS2 External Cheat  |  version 1.0", nullptr,
                     ImGuiWindowFlags_NoCollapse);

        // ---- Status bar ----
        if (process_attached) {
            ImGui::TextColored({0.f,0.9f,0.4f,1.f}, "[+] ATTACHED  cs2.exe");
            ImGui::SameLine();
            if (can_write)
                ImGui::TextColored({0.f,0.85f,1.f,1.f}, "| RW");
            else
                ImGui::TextColored({1.f,0.75f,0.f,1.f}, "| READ-ONLY");
            ImGui::SameLine();
            if (g_game_build && g_game_build != k_offset_build)
                ImGui::TextColored({1.f,0.3f,0.2f,1.f},
                    "| BUILD %d != %d (offset stale!)", g_game_build, k_offset_build);
            else
                ImGui::TextDisabled("| build %d", g_game_build);
        } else {
            ImGui::TextColored({0.95f,0.3f,0.25f,1.f},
                               "[-] Menunggu cs2.exe ...");
        }
        if (process_attached && !can_write) {
            ImGui::TextColored({1.f, 0.3f, 0.2f, 1.f},
                "  !! READ-ONLY: Radar/Glow/FOV mati - jalankan program as Administrator");
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 195.f);
        ImGui::TextDisabled("FPS %.0f", ImGui::GetIO().Framerate);
        ImGui::SameLine();
        if (ImGui::Button("Hide [INS]", {90.f, 20.f})) {
            g_menu_open = false;
            EnterEspMode();
        }
        ImGui::SameLine();
        if (ImGui::Button("Exit", {42.f, 20.f}))
            g_running = false;

        ImGui::Separator();

        if (ImGui::BeginTabBar("Tabs")) {

            // Tab: ESP (port H-V2)
            if (ImGui::BeginTabItem(" ESP ")) {
                ImGui::Spacing();
                ImGui::Checkbox("Master Switch ESP", &g_esp_cfg.enabled);
                ImGui::Separator();
                ImGui::Checkbox("Box",      &g_esp_cfg.draw_box);
                ImGui::SameLine(180);
                const char* bstyle[] = {"Full Box","Corner Box"};
                ImGui::SetNextItemWidth(130);
                ImGui::Combo("##bs", &g_esp_cfg.box_style, bstyle, 2);
                ImGui::Checkbox("Health Bar",   &g_esp_cfg.draw_health);
                ImGui::SameLine(180);
                ImGui::Checkbox("Armor Bar",    &g_esp_cfg.draw_armor);
                ImGui::Checkbox("Ammo Bar",     &g_esp_cfg.draw_ammo);
                ImGui::SameLine(180);
                ImGui::Checkbox("Nama Senjata", &g_esp_cfg.draw_weapon);
                ImGui::Checkbox("Nama Player",  &g_esp_cfg.draw_name);
                ImGui::SameLine(180);
                ImGui::Checkbox("Jarak (m)",    &g_esp_cfg.draw_distance);
                ImGui::Checkbox("Skeleton",     &g_esp_cfg.draw_bones);
                ImGui::SameLine(180);
                ImGui::Checkbox("Snaplines",    &g_esp_cfg.draw_snaplines);
                ImGui::Checkbox("Head Dot",     &g_esp_cfg.draw_head_dot);
                ImGui::SameLine(180);
                ImGui::Checkbox("Garis Arah Pandang", &g_esp_cfg.draw_eyeray);
                ImGui::Checkbox("Panah Luar Layar", &g_esp_cfg.offscreen_arrows);
                ImGui::SameLine(180);
                ImGui::Checkbox("Tampilkan Teman (Teammates)", &g_esp_cfg.teammates);
                ImGui::TextDisabled("Lingkaran kepala kosong = perkiraan postur, bukan bone.");
                ImGui::Separator();
                ImGui::ColorEdit4("Box Musuh",  g_esp_cfg.box_color,  ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine(220);
                ImGui::ColorEdit4("Box Tim",    g_esp_cfg.team_color, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Skeleton",   g_esp_cfg.bone_color, ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine(220);
                ImGui::ColorEdit4("Snaplines",  g_esp_cfg.snap_color, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Panah",      g_esp_cfg.arrow_color, ImGuiColorEditFlags_NoInputs);
                ImGui::EndTabItem();
            }

            // Tab: Radar
            if (ImGui::BeginTabItem(" Radar ")) {
                ImGui::Spacing();
                ImGui::Checkbox("Radar Minimap (spotted)", &g_misc_cfg.radar_spotted);
                ImGui::TextDisabled("Menulis mask terlihat ke minimap game. Butuh akses write (Admin).");
                if (!can_write && g_misc_cfg.radar_spotted)
                    ImGui::TextColored({1.f,0.75f,0.f,1.f}, "READ-ONLY: radar tidak aktif.");
                ImGui::EndTabItem();
            }

            // Tab: Misc
            if (ImGui::BeginTabItem(" Misc ")) {
                ImGui::Spacing();
                ImGui::Checkbox("Bunnyhop (tahan SPACE)", &g_misc_cfg.bhop);
                ImGui::Checkbox("Redam Getaran Kamera", &g_misc_cfg.noshake);
                ImGui::Checkbox("Glow (enemy)", &g_misc_cfg.glow);
                ImGui::Checkbox("FOV Changer", &g_misc_cfg.fov_changer);
                if (g_misc_cfg.fov_changer) {
                    ImGui::SetNextItemWidth(180);
                    ImGui::SliderFloat("Desired FOV", &g_misc_cfg.desired_fov, 60.f, 130.f, "%.0f");
                }
                ImGui::Separator();
                ImGui::Checkbox("Timer Bom", &g_misc_cfg.bomb_timer);
                ImGui::Checkbox("Hitsound", &g_misc_cfg.hitsound);
                ImGui::Checkbox("Watermark", &g_misc_cfg.watermark);
                ImGui::Checkbox("Sembunyikan dari Rekaman (OBS)", &g_misc_cfg.obs_bypass);
                ImGui::Separator();
                ImGui::Text("Crosshair Overlay:");
                ImGui::Checkbox("Aktifkan Crosshair", &g_crosshair_cfg.enabled);
                if (g_crosshair_cfg.enabled) {
                    ImGui::SetNextItemWidth(180);
                    ImGui::SliderFloat("Size", &g_crosshair_cfg.size, 2.f, 20.f, "%.0fpx");
                    ImGui::SetNextItemWidth(180);
                    ImGui::SliderFloat("Gap",  &g_crosshair_cfg.gap,  0.f, 10.f, "%.0fpx");
                    ImGui::SetNextItemWidth(180);
                    ImGui::SliderFloat("Thickness", &g_crosshair_cfg.thickness, 1.f, 5.f, "%.1fpx");
                    ImGui::ColorEdit4("Warna##ch", g_crosshair_cfg.color, ImGuiColorEditFlags_NoInputs);
                }
                ImGui::EndTabItem();
            }

            // Tab: Players (port H-V2: ID/Nama/HP/Armor/Tim/Jarak/Senjata)
            if (ImGui::BeginTabItem(" Players ")) {
                ImGui::Spacing();
                int alive_count = 0, total_count = 0;
                for (int i = 1; i < k_max_entities; ++i) {
                    if (players[i].pawn_ptr) {
                        ++total_count;
                        if (players[i].alive) ++alive_count;
                    }
                }
                ImGui::TextDisabled("Player terdeteksi: %d  |  Hidup: %d", total_count, alive_count);
                ImGui::Separator();

                if (ImGui::BeginTable("PT", 7,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY, {0.f, 300.f})) {
                    ImGui::TableSetupColumn("ID",     ImGuiTableColumnFlags_WidthFixed, 28.f);
                    ImGui::TableSetupColumn("Nama",   ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("HP",     ImGuiTableColumnFlags_WidthFixed, 40.f);
                    ImGui::TableSetupColumn("Armor",  ImGuiTableColumnFlags_WidthFixed, 45.f);
                    ImGui::TableSetupColumn("Tim",    ImGuiTableColumnFlags_WidthFixed, 38.f);
                    ImGui::TableSetupColumn("Jarak",  ImGuiTableColumnFlags_WidthFixed, 52.f);
                    ImGui::TableSetupColumn("Senjata",ImGuiTableColumnFlags_WidthFixed, 100.f);
                    ImGui::TableHeadersRow();
                    int cnt = 0;
                    for (int i = 1; i < k_max_entities; ++i) {
                        const PlayerData& p = players[i];
                        if (!p.pawn_ptr) continue;
                        ++cnt;
                        ImGui::TableNextRow();
                        if (p.alive && !p.dormant)
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                IM_COL32(30, 80, 30, 120));
                        else if (!p.alive)
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                IM_COL32(80, 20, 20, 80));

                        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p.controller_index ? p.controller_index : i);
                        ImGui::TableSetColumnIndex(1);
                        if (p.team == 2)
                            ImGui::TextColored({0.95f,0.5f,0.2f,1.f}, "%s", p.name[0] ? p.name : "?");
                        else if (p.team == 3)
                            ImGui::TextColored({0.2f,0.6f,0.95f,1.f}, "%s", p.name[0] ? p.name : "?");
                        else
                            ImGui::Text("%s", p.name[0] ? p.name : "?");
                        ImGui::TableSetColumnIndex(2);
                        if (p.health > 70)
                            ImGui::TextColored({0.2f,0.9f,0.2f,1.f}, "%d", p.health);
                        else if (p.health > 30)
                            ImGui::TextColored({0.9f,0.9f,0.2f,1.f}, "%d", p.health);
                        else
                            ImGui::TextColored({0.9f,0.2f,0.2f,1.f}, "%d", p.health);
                        ImGui::TableSetColumnIndex(3); ImGui::Text("%d", p.armor);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%s", p.team==2?"T":(p.team==3?"CT":"?"));
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Text("%.0fm", p.distance);
                        ImGui::TableSetColumnIndex(6);
                        ImGui::Text("%s", WeaponName(p.weapon_def));
                    }
                    if (cnt == 0) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        if (process_attached)
                            ImGui::TextDisabled("Menunggu match dimulai...");
                        else
                            ImGui::TextDisabled("Menunggu cs2.exe...");
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // Tab: Status (diagnostik live ala H-V2)
            if (ImGui::BeginTabItem(" Status ")) {
                ImGui::Spacing();
                const RenderStats& rs = Stats();
                ImGui::Text("Proses: %s | Build: %d %s",
                    process_attached ? "terhubung" : "menunggu",
                    game_build,
                    (game_build && game_build != k_offset_build) ? "(STALE!)" : "");
                ImGui::Text("Write: %s | Matrix: %s",
                    can_write ? "RW" : "READ-ONLY",
                    view_matrix.IsZeroMatrix() ? "invalid" : "valid");
                int pc = 0, al = 0, en = 0;
                for (int i = 1; i < k_max_entities; ++i) {
                    if (!players[i].pawn_ptr) continue;
                    ++pc;
                    if (players[i].alive) {
                        ++al;
                        if (players[i].team != local_team) ++en;
                    }
                }
                ImGui::Text("Pemain %d | hidup %d | musuh %d", pc, al, en);
                if (bomb.planted) {
                    if (bomb.defusing && bomb.defuse_remaining >= 0.f)
                        ImGui::Text("Bom: DEFUSING %.1fs%s", bomb.defuse_remaining,
                            bomb.defuser[0] ? bomb.defuser : "");
                    else if (bomb.countdown >= 0.f)
                        ImGui::Text("Bom: C4 %.1fs", bomb.countdown);
                    else ImGui::TextDisabled("Bom: planted (jam belum valid)");
                } else {
                    ImGui::TextDisabled("Bom: -");
                }
                ImGui::Text("Hit counter: %s", hit.active ? "HIT" : "-");
                ImGui::Separator();
                ImGui::Text("Render: %.0f fps | p95 %.2f ms", rs.loop_fps, rs.frame_p95_ms);
                ImGui::Text("UI %.2f ms | submit %.2f ms | present %.2f ms | tunggu %.2f ms",
                    rs.ui_ms, rs.submit_ms, rs.present_ms, rs.wait_ms);
                ImGui::Text("Usia snapshot: %.0f ms | worker %.1f Hz",
                    rs.snapshot_age_ms, worker_hz);
                ImGui::Text("Frame %u | hidden %u | skip statis %u | skip render %u | present gagal %u",
                    rs.frames, rs.hidden_frames, rs.static_skips, rs.render_skips,
                    rs.present_failures);
                ImGui::Text("Read %llu | gagal %llu | write gagal %llu",
                    (unsigned long long)g_mem.read_calls,
                    (unsigned long long)g_mem.read_failures,
                    (unsigned long long)g_mem.write_failures);
                ImGui::Text("Renderer: %s | %s",
                    rs.composition ? "DirectComposition" : "belum siap",
                    rs.software ? "WARP" : "GPU");
                ImGui::TextDisabled("%s", config::StatusText());
                ImGui::EndTabItem();
            }

            
            // Tab: Presets (disesuaikan fitur)
            if (ImGui::BeginTabItem(" Presets ")) {
                ImGui::Spacing();
                ImGui::Text("Quick Presets:");
                ImGui::Spacing();
                if (ImGui::Button("Legit", {100.f, 32.f})) {
                    g_esp_cfg.enabled=true; g_esp_cfg.draw_box=true;
                    g_esp_cfg.box_style=1;  g_esp_cfg.draw_bones=false;
                    g_esp_cfg.draw_weapon=true; g_esp_cfg.draw_armor=true;
                    g_esp_cfg.offscreen_arrows=false;
                    g_misc_cfg.bhop=true; g_misc_cfg.glow=false;
                    g_misc_cfg.radar_spotted=false; g_misc_cfg.noshake=true;
                    g_misc_cfg.bomb_timer=true; g_misc_cfg.hitsound=false;
                }
                ImGui::SameLine();
                if (ImGui::Button("Rage", {100.f, 32.f})) {
                    g_esp_cfg.enabled=true; g_esp_cfg.draw_box=true;
                    g_esp_cfg.box_style=0;  g_esp_cfg.draw_bones=true;
                    g_esp_cfg.draw_snaplines=true; g_esp_cfg.draw_weapon=true;
                    g_esp_cfg.draw_armor=true; g_esp_cfg.draw_ammo=true;
                    g_esp_cfg.draw_eyeray=true; g_esp_cfg.offscreen_arrows=true;
                    g_misc_cfg.bhop=true; g_misc_cfg.glow=true;
                    g_misc_cfg.radar_spotted=true; g_misc_cfg.noshake=true;
                    g_misc_cfg.bomb_timer=true; g_misc_cfg.hitsound=true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset", {100.f, 32.f})) {
                    g_esp_cfg={};
                    g_misc_cfg={}; g_crosshair_cfg={};
                }
                ImGui::Spacing();
                ImGui::TextDisabled("Legit: ESP rapi + bhop, tanpa radar/glow.");
                ImGui::TextDisabled("Rage: semua visual + radar + glow + hitsound.");
                ImGui::TextDisabled("INSERT = tampil/sembunyikan menu");
                ImGui::TextDisabled("SHIFT+END = keluar program");
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    ImGui::PopStyleColor(3);

    ImGui::Render();
    if (s_docs_mode) return; // harness docs: present dilakukan caller
    if (!g_mainRenderTargetView || !g_pd3dDeviceContext || !g_pSwapChain) return;

    s_stats.ui_ms = MsSince(ui_start);
    auto submit_start = std::chrono::steady_clock::now();
    D3D11_VIEWPORT vp{};
    vp.Width = (float)g_screen_width; vp.Height = (float)g_screen_height;
    vp.MaxDepth = 1.f;
    g_pd3dDeviceContext->RSSetViewports(1, &vp);
    static const float cc[4] = {0.f, 0.f, 0.f, 0.f};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    s_stats.submit_ms = MsSince(submit_start);
    auto present_start = std::chrono::steady_clock::now();
    s_stats.render_ms = MsSince(frame_start);
    UINT sync_now = s_degrade ? 1u : 0u;
    s_stats.last_result = g_pSwapChain->Present(sync_now, 0);
    if (FAILED(s_stats.last_result)) ++s_stats.present_failures;
    s_occluded = s_stats.last_result == DXGI_STATUS_OCCLUDED;
    s_stats.present_ms = MsSince(present_start);
    if (s_stats.present_ms > 50.0) s_degrade += 5;
}

// =============================================================================
//  Shutdown
// =============================================================================
void Shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain)           { g_pSwapChain->Release();           g_pSwapChain           = nullptr; }
    if (g_pd3dDeviceContext)    { g_pd3dDeviceContext->Release();     g_pd3dDeviceContext     = nullptr; }
    if (g_pd3dDevice)           { g_pd3dDevice->Release();            g_pd3dDevice            = nullptr; }
    if (g_hwnd)                 { ::DestroyWindow(g_hwnd);            g_hwnd                  = nullptr; }
    ::UnregisterClassW(L"NVIDIA_Share_OverlayHost", g_hinstance);
}

} // namespace Overlay
