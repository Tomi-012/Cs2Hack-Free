#pragma once

// =============================================================================
//  overlay.h — Win32 + DirectComposition + ImGui Overlay Renderer (ala H-V2)
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include "features.h"

namespace Overlay {
    inline HINSTANCE                g_hinstance = nullptr;
    inline HWND                     g_hwnd = nullptr;
    inline ID3D11Device*            g_pd3dDevice = nullptr;
    inline ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
    inline IDXGISwapChain*          g_pSwapChain = nullptr;
    inline ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
    inline bool                     g_menu_open = true;
    inline bool                     g_running = true;
    inline int                      g_screen_width = 1920;
    inline int                      g_screen_height = 1080;

    struct RenderStats {
        double loop_fps = 0, frame_p95_ms = 0, render_ms = 0, present_ms = 0;
        double wait_ms = 0, ui_ms = 0, submit_ms = 0, snapshot_age_ms = 0;
        unsigned frames = 0, hidden_frames = 0, render_skips = 0, static_skips = 0;
        unsigned present_failures = 0;
        bool software = false, composition = false;
        HRESULT last_result = S_OK;
    };
    const RenderStats& Stats() noexcept;
    void WaitForFrame();
    void SetMenuOpen(bool open) noexcept;
    inline bool s_docs_mode = false; // true = RenderFrame berhenti setelah ImGui::Render

    bool Init(HINSTANCE hInstance);

    void RenderFrame(const std::array<PlayerData, k_max_entities>& players, int local_team,
                     bool process_attached, const Mat4x4& view_matrix,
                     const Vec3& local_eye, float local_yaw, bool yaw_valid,
                     bool can_write, const BombInfo& bomb, const HitState& hit,
                     int game_build, double worker_hz, DWORD sample_ms);

    void ProcessEvents();

    void Shutdown();
}
