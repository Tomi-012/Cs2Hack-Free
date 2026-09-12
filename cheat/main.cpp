// =============================================================================
//  main.cpp — CS2 External Cheat entry point with DirectX 11 Overlay GUI
//  DUA THREAD:
//    worker = attach + entity scan + fitur (bhop/noshake/radar/glow/fov/bomb/hit)
//            loop adaptif 2ms fokus / 16ms blur, TANPA render
//    main   = overlay render saja
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#include <thread>
#include <chrono>
#include <array>
#include <cstring>
#include <mutex>

#include "memory.h"
#include "offsets.h"
#include "features.h"
#include "overlay.h"
#include "../math/math_utils.h"
#include "../math/mat4x4.h"

// ---- Single-instance guard via Named Mutex ---------------------------------
static constexpr wchar_t k_mutex_name[] = L"Global\\CS2_External_Cheat_4B7F2A1E";
static HANDLE g_single_instance_mutex   = nullptr;

static bool EnsureSingleInstance() noexcept {
    g_single_instance_mutex = CreateMutexW(nullptr, TRUE, k_mutex_name);
    if (g_single_instance_mutex == nullptr) return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = nullptr;

        HWND existing = FindWindowW(L"NVIDIA_Share_OverlayHost", nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        } else {
            MessageBoxW(nullptr,
                L"Program sudah berjalan!\n\nCek system tray atau Task Manager.",
                L"CS2 Cheat - Already Running",
                MB_OK | MB_ICONINFORMATION);
        }
        return false;
    }
    return true;
}

// ---- Snapshot worker → render (copy kecil, lock sesaat) ---------------------
struct FrameSnapshot {
    std::array<PlayerData, k_max_entities> players{};
    Mat4x4 view_matrix{};
    Vec3  local_eye{};
    float local_yaw = 0.f;
    bool  yaw_valid = false;
    int   local_team = 0;
    bool  attached   = false;
    bool  can_write  = false;
    int   game_build = 0;
    double worker_hz = 0;
    DWORD sample_ms = 0;
    BombInfo bomb{};
    HitState hit{};
};
static FrameSnapshot g_snap;
static std::mutex    g_snap_mtx;

// Logger diagnostik 2 detik (ala H-V2 hv2_diag.txt)
static void LogWorker() noexcept {
    char cfg[MAX_PATH];
    config::ConfigPath(cfg, sizeof cfg);
    std::string path = cfg;
    size_t p = path.find_last_of("\\/");
    path = (p == std::string::npos ? "" : path.substr(0, p + 1)) + "cs2_diag.txt";
    while (Overlay::g_running) {
        for (int i = 0; i < 20 && Overlay::g_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!Overlay::g_running) break;
        FrameSnapshot s;
        { std::lock_guard<std::mutex> lk(g_snap_mtx); s = g_snap; }
        const Overlay::RenderStats& r = Overlay::Stats();
        FILE* f = fopen(path.c_str(), "w");
        if (!f) continue;
        int pc = 0, al = 0, en = 0;
        for (int i = 1; i < k_max_entities; ++i)
            if (s.players[i].pawn_ptr) {
                ++pc;
                if (s.players[i].alive) {
                    ++al;
                    if (s.players[i].team != s.local_team) ++en;
                }
            }
        fprintf(f, "attached=%d build=%d write=%d players=%d alive=%d enemies=%d\n",
            (int)s.attached, s.game_build, (int)s.can_write, pc, al, en);
        fprintf(f, "bomb planted=%d countdown=%.1f defusing=%d defuse=%.1f by=%s\n",
            (int)s.bomb.planted, s.bomb.countdown, (int)s.bomb.defusing,
            s.bomb.defuse_remaining, s.bomb.defuser);
        fprintf(f, "render fps=%.1f p95=%.2f ui=%.2f submit=%.2f present=%.2f wait=%.2f snap_age=%.0f worker_hz=%.1f\n",
            r.loop_fps, r.frame_p95_ms, r.ui_ms, r.submit_ms, r.present_ms,
            r.wait_ms, r.snapshot_age_ms, s.worker_hz);
        fprintf(f, "frames=%u hidden=%u sskip=%u rskip=%u pfail=%u comp=%d warp=%d\n",
            r.frames, r.hidden_frames, r.static_skips, r.render_skips,
            r.present_failures, (int)r.composition, (int)r.software);
        fprintf(f, "rpm=%llu read_fail=%llu write_fail=%llu\n",
            (unsigned long long)g_mem.read_calls,
            (unsigned long long)g_mem.read_failures,
            (unsigned long long)g_mem.write_failures);
        fclose(f);
    }
}

static bool InitProcess() noexcept {
    if (!g_mem.Attach("cs2.exe")) return false;
    g_mem.client_base = g_mem.GetModuleBase("client.dll");
    g_mem.engine2_base = g_mem.GetModuleBase("engine2.dll");
    if (!g_mem.client_base || !g_mem.engine2_base) { g_mem.Detach(); return false; }
    g_game_build = Memory::IsValidPtr(g_mem.engine2_base)
        ? (int)g_mem.Read<int32_t>(g_mem.engine2_base + offsets::engine2::dwBuildNumber)
        : 0;
    return true;
}

// ============================================================================
//  Worker thread — game loop cheat (scan + fitur)
// ============================================================================
static void GameWorker() noexcept {
    bool  process_attached  = false;
    DWORD last_attach_check = 0;

    std::array<PlayerData, k_max_entities + 1> ent_last{};
    std::array<DWORD,    k_max_entities + 1> ent_ms{};
    DWORD name_ms = 0, weapon_ms = 0;
    std::array<double, 64> loop_times{};
    size_t loop_n = 0, loop_i = 0;
    auto last_start = std::chrono::steady_clock::now();

    while (Overlay::g_running) {
        auto loop_start = std::chrono::steady_clock::now();
        DWORD now = GetTickCount();

        if (!process_attached && (now - last_attach_check > 500)) {
            last_attach_check = now;
            process_attached = InitProcess();
            if (process_attached && !g_mem.CanWrite()) {
                static bool warned_ro = false;
                if (!warned_ro) {
                    warned_ro = true;
                    MessageBoxW(nullptr,
                        L"Proses cs2.exe terbaca READ-ONLY.\n"
                        L"Radar/Glow/FOV TIDAK akan jalan.\n\n"
                        L"Jalankan ulang program ini sebagai Administrator.",
                        L"CS2 Cheat - READ-ONLY",
                        MB_OK | MB_ICONWARNING);
                }
            }
        }

        FrameSnapshot snap{};

        if (process_attached && g_mem.client_base) {
            if (!g_mem.IsAlive()) {
                process_attached = false;
                g_mem.Detach();
                ResetPatches();
                ent_last.fill(PlayerData{});
                ent_ms.fill(0);
                name_ms = weapon_ms = 0;
            } else {
                uintptr_t entity_list = g_mem.Read<uintptr_t>(
                    g_mem.client_base + offsets::client::dwEntityList);
                uintptr_t local_pawn  = g_mem.Read<uintptr_t>(
                    g_mem.client_base + offsets::client::dwLocalPlayerPawn);

                {
                    static uintptr_t sess_pawn = 0, sess_list = 0;
                    if (local_pawn != sess_pawn || entity_list != sess_list) {
                        ResetPatches();
                        ++g_mem.session;
                        ent_last.fill(PlayerData{});
                        ent_ms.fill(0);
                        name_ms = weapon_ms = 0;
                        sess_pawn = local_pawn; sess_list = entity_list;
                    }
                }
                if (!Memory::IsValidPtr(entity_list) || !Memory::IsValidPtr(local_pawn)) {
                    ent_last.fill(PlayerData{});
                    ent_ms.fill(0);
                    g_mem.writes_allowed = false;
                } else {
                    g_mem.writes_allowed = false;
                    uintptr_t local_scene = g_mem.Read<uintptr_t>(
                        local_pawn + schemas::base_entity::m_pGameSceneNode);
                    Vec3 local_feet = Memory::IsValidPtr(local_scene)
                        ? g_mem.ReadVec3(local_scene + schemas::scene_node::m_vecAbsOrigin)
                        : Vec3{};
                    if (!IsValidVec3(local_feet)) local_feet = Vec3{};
                    Vec3 view_off = g_mem.ReadVec3(local_pawn + schemas::player_pawn::m_vecViewOffset);
                    if (!IsValidVec3(view_off) || view_off.z < 20.f || view_off.z > 90.f)
                        view_off = Vec3{ 0.f, 0.f, 64.f };
                    Vec3 local_origin = local_feet + view_off;

                    int local_team = g_mem.Read<uint8_t>(
                        local_pawn + schemas::base_entity::m_iTeamNum);

                    // Yaw lokal utk panah offscreen
                    Vec3 eye = g_mem.ReadVec3(local_pawn + schemas::player_pawn::m_angEyeAngles);
                    if (IsValidVec3(eye)) {
                        snap.local_yaw = eye.y;
                        snap.yaw_valid = true;
                    }

                    DWORD now_ms = GetTickCount();
                    bool want_name = (now_ms - name_ms > 400);
                    if (want_name) name_ms = now_ms;
                    bool want_weapon = (now_ms - weapon_ms > 100);
                    if (want_weapon) weapon_ms = now_ms;

                    bool need_eye = g_esp_cfg.enabled && g_esp_cfg.draw_eyeray;
                    bool need_armor = g_esp_cfg.enabled && g_esp_cfg.draw_armor;

                    for (int i = 1; i <= k_entity_loop; ++i) {
                        PlayerData tmp{};
                        PlayerData& prev = ent_last[i];

                        if (ReadPlayerData(entity_list, i, tmp, local_origin,
                                           want_name, want_weapon, want_weapon && need_armor, need_eye)) {
                            if (!want_name && prev.pawn_ptr == tmp.pawn_ptr)
                                memcpy(tmp.name, prev.name, sizeof(tmp.name));
                            if (!want_weapon && prev.pawn_ptr == tmp.pawn_ptr) {
                                tmp.weapon_def = prev.weapon_def;
                                tmp.clip = prev.clip;
                                tmp.clip_capacity = prev.clip_capacity;
                                tmp.armor = prev.armor;
                            }
                            UpdatePlayerCache(prev, &tmp, now_ms, ent_ms[i]);
                        } else {
                            UpdatePlayerCache(prev, nullptr, now_ms, ent_ms[i]);
                        }
                    }

                    int slot = 0;
                    for (int i = 1; i <= k_entity_loop && slot < k_max_entities - 1; ++i) {
                        PlayerData& src = ent_last[i];
                        if (!src.pawn_ptr) continue;
                        snap.players[++slot] = src;
                    }

                    // ---- Fitur worker ----
                    RunBhop(local_pawn);
                    RunNoShake(local_pawn);
                    RunRadar(snap.players, local_team);
                    RunGlow(snap.players, local_team);
                    RunFovChanger(g_mem.client_base);
                    ReadBomb(g_mem.client_base, entity_list, snap.bomb);
                    DetectHit(g_mem.client_base, local_pawn, snap.hit);

                    snap.view_matrix = g_mem.Read<Mat4x4>(
                        g_mem.client_base + offsets::client::dwViewMatrix);
                    bool matrix_ok = !snap.view_matrix.IsZeroMatrix();
                    g_mem.writes_allowed = matrix_ok;
                    snap.local_team = local_team;
                    snap.local_eye = local_origin;
                    snap.attached   = true;
                    snap.can_write  = g_mem.CanWrite();
                }
            }
        }

        {
            double iv = std::chrono::duration<double, std::milli>(
                loop_start - last_start).count();
            last_start = loop_start;
            loop_times[loop_i++ % loop_times.size()] = iv;
            if (loop_n < loop_times.size()) ++loop_n;
            double sum = 0;
            for (size_t k = 0; k < loop_n; ++k) sum += loop_times[k];
            snap.worker_hz = sum > 0 ? 1000.0 * (double)loop_n / sum : 0;
        }
        snap.game_build = g_game_build;
        snap.sample_ms = GetTickCount();
        { std::lock_guard<std::mutex> lk(g_snap_mtx); g_snap = snap; }

        // Loop adaptif: 2ms saat game fokus, 16ms saat blur
        HWND fg = GetForegroundWindow();
        HWND game = FindWindowA("SDL_app", nullptr);
        int period = (game && (fg == game)) ? 2 : 16;
        std::this_thread::sleep_until(loop_start + std::chrono::milliseconds(period));
    }
    ResetPatches();
}

// ============================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    timeBeginPeriod(1);

    if (!EnsureSingleInstance()) { timeEndPeriod(1); return 0; }

    if (!Overlay::Init(hInstance)) {
        timeEndPeriod(1);
        return 1;
    }

    char cfg_path[MAX_PATH];
    config::ConfigPath(cfg_path, sizeof cfg_path);
    config::Load(cfg_path);

    std::thread worker(GameWorker);
    std::thread logger(LogWorker);
    bool dirty = false;
    DWORD change_ms = 0;
    uint32_t last_hash = 0;
    bool have_hash = false;

    auto config_hash = []() noexcept -> uint32_t {
        uint32_t h = 2166136261u;
        auto mix = [&](const void* p, size_t n) {
            const uint8_t* b = (const uint8_t*)p;
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
        };
        mix(&g_esp_cfg, sizeof g_esp_cfg);
        mix(&g_misc_cfg, sizeof g_misc_cfg);
        mix(&g_crosshair_cfg, sizeof g_crosshair_cfg);
        return h;
    };

    while (Overlay::g_running) {
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_END) & 0x8000)) {
            break;
        }
        Overlay::ProcessEvents();
        if (!Overlay::g_running) break;
        Overlay::WaitForFrame();
        Overlay::ProcessEvents();
        if (!Overlay::g_running) break;

        g_input_blocked = Overlay::g_menu_open;

        FrameSnapshot snap;
        { std::lock_guard<std::mutex> lk(g_snap_mtx); snap = g_snap; }

        Overlay::RenderFrame(snap.players, snap.local_team, snap.attached,
                             snap.view_matrix, snap.local_eye, snap.local_yaw,
                             snap.yaw_valid, snap.can_write, snap.bomb, snap.hit,
                             snap.game_build, snap.worker_hz, snap.sample_ms);

        // OBS bypass: keluarkan overlay dari capture
        {
            int want = g_misc_cfg.obs_bypass ? 0x00000004 /*WDA_EXCLUDEFROMCAPTURE*/ : 0;
            static int cur = -1;
            if (want != cur && Overlay::g_hwnd) {
                SetWindowDisplayAffinity(Overlay::g_hwnd, (DWORD)want);
                cur = want;
            }
        }

        // Autosave 1 detik setelah perubahan
        uint32_t h = config_hash();
        if (!have_hash) { last_hash = h; have_hash = true; }
        else if (h != last_hash) { last_hash = h; dirty = true; change_ms = GetTickCount(); }
        if (dirty && GetTickCount() - change_ms >= 1000) {
            if (config::Save(cfg_path)) dirty = false;
            else change_ms = GetTickCount();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    Overlay::g_running = false;
    worker.join();
    logger.join();
    JoinBeep();

    if (dirty) config::Save(cfg_path);
    g_mem.Detach();
    Overlay::Shutdown();
    timeEndPeriod(1);

    if (g_single_instance_mutex) {
        ReleaseMutex(g_single_instance_mutex);
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = nullptr;
    }

    return 0;
}

int main() {
    return WinMain(GetModuleHandle(NULL), NULL, NULL, SW_SHOW);
}
