#pragma once

// =============================================================================
//  features.h — CS2 cheat features (port H-V2 build 14181)
//  ESP · Radar(spotted) · Bhop · NoShake · Glow · FOV · Bomb timer · Hitsound
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "memory.h"
#include "offsets.h"
#include "../math/vec3.h"
#include "../math/vec2.h"
#include "../math/mat4x4.h"
#include "../math/mat3x4.h"
#include "../math/math_utils.h"

// ImGui selalu tersedia karena project ini selalu build dengan imgui
#include "../third_party/imgui/imgui.h"

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif
#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// ---- Konstanta -------------------------------------------------------------
static constexpr int   k_max_entities  = 65;   // slot array, index 1..64 = controller
static constexpr int   k_entity_loop   = 64;
static constexpr float k_head_offset_z = 75.f;

// ---- Struct data player ----------------------------------------------------
struct PlayerData {
    uintptr_t pawn_ptr    = 0;
    uintptr_t ctrl_ptr    = 0;
    int       controller_index = 0;
    Vec3      origin      = {};
    Vec3      head_pos    = {};
    int       health      = 0;
    int       armor       = 0;
    int       team        = 0;
    int       weapon_def  = -1;
    int       clip        = -1;
    int       clip_capacity = -1;
    float     distance    = 0.f;
    bool      dormant     = true;
    bool      alive       = false;
    bool      ghost       = false; // data anti-blink (posisi basi) — render saja
    bool      head_estimated = false;
    float     eye_yaw     = 0.f;
    float     eye_pitch   = 0.f;
    bool      eye_yaw_valid = false;
    char      name[128]   = {};
    Vec3      bones[64]   = {};
};

FORCEINLINE bool IsValidVec3(const Vec3& v) noexcept {
    return !std::isnan(v.x) && !std::isnan(v.y) && !std::isnan(v.z) &&
           !std::isinf(v.x) && !std::isinf(v.y) && !std::isinf(v.z);
}

// ---- Kapasitas clip per senjata (H-V2, data game 2026-09-09) ----------------
[[nodiscard]] inline int WeaponClipCapacity(int id) noexcept {
    switch (id) {
    case 1: return 7;   case 2: return 30;  case 3: return 20;  case 4: return 20;
    case 7: return 30;  case 8: return 30;  case 9: return 5;   case 10: return 25;
    case 11: return 20; case 13: return 35; case 14: return 100; case 16: return 30;
    case 17: return 30; case 19: return 50; case 23: return 30; case 24: return 25;
    case 25: return 7;  case 26: return 64; case 27: return 5;  case 28: return 150;
    case 29: return 7;  case 30: return 18; case 31: return 1;  case 32: return 13;
    case 33: return 30; case 34: return 30; case 35: return 8;  case 36: return 13;
    case 38: return 20; case 39: return 30; case 40: return 10; case 60: return 20;
    case 61: return 12; case 63: return 12; case 64: return 8;
    default: return -1;
    }
}

struct DefName { int def; const char* name; };
static constexpr DefName k_weapon_names[] = {
    {1,"Desert Eagle"},{2,"Dual Berettas"},{3,"Five-SeveN"},{4,"Glock-18"},
    {7,"AK-47"},{8,"AUG"},{9,"AWP"},{10,"FAMAS"},{11,"G3SG1"},{13,"Galil AR"},
    {14,"M249"},{16,"M4A4"},{17,"MAC-10"},{19,"P90"},{23,"MP5-SD"},{24,"UMP-45"},
    {25,"XM1014"},{26,"PP-Bizon"},{27,"MAG-7"},{28,"Negev"},{29,"Sawed-Off"},
    {30,"Tec-9"},{32,"P2000"},{33,"MP7"},{34,"MP9"},{35,"Nova"},{36,"P250"},
    {38,"SCAR-20"},{39,"SG 553"},{40,"SSG 08"},{42,"Knife"},{43,"HE Grenade"},
    {44,"Flashbang"},{45,"Smoke"},{46,"Molotov"},{47,"Decoy"},{48,"Incendiary"},
    {49,"C4"},{59,"Knife"},{60,"M4A1-S"},{61,"USP-S"},{63,"CZ75-Auto"},{64,"R8 Revolver"},
};
[[nodiscard]] inline const char* WeaponName(int def_index) noexcept {
    if (def_index < 1) return "-";
    if (def_index >= 500 && def_index <= 526) return "Knife";
    if (def_index == 31) return "Zeus x27";
    for (const auto& w : k_weapon_names)
        if (w.def == def_index) return w.name;
    return "Unknown";
}

// =============================================================================
//  Entity resolution — pola kanonik build 14181
// =============================================================================
[[nodiscard]] FORCEINLINE uintptr_t GetEntityByIndex(uintptr_t entity_system,
                                                     uint32_t idx) noexcept
{
    if (UNLIKELY(!Memory::IsValidPtr(entity_system) || idx == 0 || idx >= 0x7FF0))
        return 0;
    uintptr_t bulk = g_mem.Read<uintptr_t>(
        entity_system + schemas::entity_list::BULK_HEADER
        + (uintptr_t)(idx >> schemas::entity_list::ENTITY_SHIFT)
          * schemas::entity_list::ENTITY_SIZE);
    if (UNLIKELY(!Memory::IsValidPtr(bulk))) return 0;
    uintptr_t identity = bulk + (uintptr_t)(idx & 0x1FFu)
                               * schemas::entity_list::IDENTITY_SIZE;
    uintptr_t ent = g_mem.Read<uintptr_t>(
        identity + schemas::entity_list::IDENTITY_ENTITY);
    return Memory::IsValidPtr(ent) ? ent : 0;
}

[[nodiscard]] FORCEINLINE uintptr_t GetEntityByHandle(uintptr_t entity_system,
                                                      uint32_t handle) noexcept
{
    using namespace schemas::entity_list;
    uint32_t idx = handle & 0x7FFFu;
    if (!Memory::IsValidPtr(entity_system) || handle >= 0xFFFFFFFEu ||
        !idx || idx >= 0x7FF0) return 0;
    uintptr_t bulk = g_mem.Read<uintptr_t>(
        entity_system + BULK_HEADER + (idx >> ENTITY_SHIFT) * BULK_STRIDE);
    if (!Memory::IsValidPtr(bulk)) return 0;
    struct Identity { uintptr_t entity; uintptr_t unused; uint32_t handle; uint32_t pad; };
    static_assert(offsetof(Identity, handle) == (size_t)IDENTITY_HANDLE);
    Identity id{};
    if (!g_mem.ReadBlob(bulk + (idx & 0x1FFu) * IDENTITY_SIZE, &id, sizeof id) ||
        id.handle != handle) return 0;
    return Memory::IsValidPtr(id.entity) ? id.entity : 0;
}

// ---- Bone array helpers ----------------------------------------------------
[[nodiscard]] FORCEINLINE uintptr_t GetBoneArrayPtr(uintptr_t pawn) noexcept {
    if (UNLIKELY(!Memory::IsValidPtr(pawn))) return 0;
    uintptr_t scene_node = g_mem.Read<uintptr_t>(
        pawn + schemas::base_entity::m_pGameSceneNode);
    if (UNLIKELY(!Memory::IsValidPtr(scene_node))) return 0;
    uintptr_t bone_array = g_mem.Read<uintptr_t>(
        scene_node + schemas::scene_node::m_modelState
                   + schemas::model_state::m_boneArray);
    return Memory::IsValidPtr(bone_array) ? bone_array : 0;
}

[[nodiscard]] FORCEINLINE bool ValidBone(const Vec3& pos, const Vec3& origin) noexcept {
    if (!IsValidVec3(pos) || !IsValidVec3(origin) || pos.IsZero()) return false;
    Vec3 d = pos - origin;
    return d.x*d.x + d.y*d.y <= 10000.f && d.z >= -30.f && d.z <= 120.f;
}

inline std::ptrdiff_t g_bone_stride = 32;

inline void ResolveBoneStride(uintptr_t bone_array, const Vec3& origin) noexcept {
    static DWORD last = 0;
    if (UNLIKELY(!Memory::IsValidPtr(bone_array))) return;
    DWORD now = GetTickCount();
    if (last && now - last < 500) return;
    last = now;
    auto valid = [&](int stride) noexcept {
        size_t base = stride == 48 ? 32 : 0;
        Vec3 pelvis = g_mem.ReadVec3(bone_array + base);
        Vec3 head   = g_mem.ReadVec3(bone_array + 6u * (uintptr_t)stride + base);
        return ValidBone(pelvis, origin) && ValidBone(head, origin) &&
               (head.z - pelvis.z) > 10.f && (head.z - pelvis.z) < 100.f;
    };
    if (valid((int)g_bone_stride)) return;
    int other = g_bone_stride == 32 ? 48 : 32;
    if (valid(other)) g_bone_stride = other;
}

// Bone yang digambar skeleton ESP — di-cache per frame di ReadPlayerData
static constexpr int k_draw_bones[] = {
    schemas::bones::Pelvis, schemas::bones::Spine1, schemas::bones::Spine2,
    schemas::bones::Spine3, schemas::bones::Neck,   schemas::bones::Head,
    schemas::bones::LeftHand, schemas::bones::RightHand,
    schemas::bones::LeftFoot, schemas::bones::RightFoot,
};

// ---- Baca data satu player (batch RPM) --------------------------------------
// read_weapon/read_armor/read_eye di-throttle caller (mahal).
FORCEINLINE Vec3 RdVecFrom(const uint8_t* buf, size_t off) noexcept {
    Vec3 v; memcpy(&v, buf + off, 12); return v;
}
FORCEINLINE bool ReadPlayerData(uintptr_t entity_list,
                                 int        index,
                                 PlayerData& out,
                                 const Vec3& local_origin,
                                 bool       read_name   = true,
                                 bool       read_weapon = false,
                                 bool       read_armor  = false,
                                 bool       read_eye    = false) noexcept
{
    if (UNLIKELY(!Memory::IsValidPtr(entity_list))) return false;
    out.controller_index = index;

    uintptr_t ctrl = GetEntityByIndex(entity_list, (uint32_t)index);
    if (UNLIKELY(!Memory::IsValidPtr(ctrl))) return false;
    out.ctrl_ptr = ctrl;

    uint32_t pawn_handle = g_mem.Read<uint32_t>(
        ctrl + schemas::player_controller::m_hPlayerPawn);
    if (UNLIKELY(pawn_handle == 0 || pawn_handle == 0xFFFFFFFF)) return false;

    uintptr_t pawn = GetEntityByHandle(entity_list, pawn_handle);
    if (UNLIKELY(!Memory::IsValidPtr(pawn))) return false;
    out.pawn_ptr = pawn;

    // 1 RPM: blob pawn 0x330..0x404 (scene,health,life,team)
    constexpr uintptr_t k_blob_base = 0x330;
    uint8_t pawn_blob[0xE0];
    if (UNLIKELY(!g_mem.ReadBlob(pawn + k_blob_base, pawn_blob, sizeof(pawn_blob))))
        return false;
    auto rd8 = [&](size_t o) noexcept { return pawn_blob[o]; };
    auto rd32 = [&](size_t o) noexcept {
        int32_t v; memcpy(&v, pawn_blob + o, 4); return v; };
    auto rdptr = [&](size_t o) noexcept {
        uintptr_t v; memcpy(&v, pawn_blob + o, 8); return v; };
    const size_t o_scene  = (size_t)(schemas::base_entity::m_pGameSceneNode - (std::ptrdiff_t)k_blob_base);
    const size_t o_health = (size_t)(schemas::base_entity::m_iHealth      - (std::ptrdiff_t)k_blob_base);
    const size_t o_life   = (size_t)(schemas::base_entity::m_lifeState    - (std::ptrdiff_t)k_blob_base);
    const size_t o_team   = (size_t)(schemas::base_entity::m_iTeamNum     - (std::ptrdiff_t)k_blob_base);

    out.health = rd32(o_health);
    out.team   = rd8(o_team);
    if (out.health < 0 || out.health > 200) return false;
    if (out.team != 2 && out.team != 3) return false;

    out.alive = (rd8(o_life) == 0) && (out.health > 0);

    uintptr_t scene_node = rdptr(o_scene);

    // 1 RPM: blob scene 0xC8..0x108 (origin + dormant)
    if (!Memory::IsValidPtr(scene_node)) {
        out.dormant = true;
        out.origin  = Vec3{};
    } else {
        constexpr uintptr_t k_scene_base = 0xC8;
        uint8_t scene_blob[0x40];
        if (g_mem.ReadBlob(scene_node + k_scene_base, scene_blob, sizeof(scene_blob))) {
            out.origin  = RdVecFrom(scene_blob, 0);
            out.dormant = scene_blob[(size_t)(schemas::scene_node::m_bDormant - (std::ptrdiff_t)k_scene_base)] != 0;
        } else {
            out.dormant = true;
            out.origin  = Vec3{};
        }
        if (!IsValidVec3(out.origin)) return false;
    }

    if (read_name)
        g_mem.ReadString(ctrl + schemas::player_controller::m_iszPlayerName,
                         out.name, sizeof(out.name));

    if (out.alive) {
        if (read_armor) {
            out.armor = g_mem.Read<int32_t>(pawn + schemas::player_pawn::m_ArmorValue);
            if (out.armor < 0 || out.armor > 200) out.armor = 0;
        }

        out.eye_yaw_valid = false;
        if (read_eye && !out.dormant) {
            Vec3 eye = g_mem.ReadVec3(pawn + schemas::player_pawn::m_angEyeAngles);
            if (IsValidVec3(eye)) {
                out.eye_yaw = eye.y;
                out.eye_pitch = eye.x;
                out.eye_yaw_valid = true;
            }
        }

        if (read_weapon) {
            uintptr_t ws = g_mem.Read<uintptr_t>(pawn + schemas::player_pawn::m_pWeaponServices);
            if (Memory::IsValidPtr(ws)) {
                uint32_t wh = g_mem.Read<uint32_t>(ws + schemas::player_pawn::weapon_services::m_hActiveWeapon);
                uintptr_t w = GetEntityByHandle(entity_list, wh);
                if (Memory::IsValidPtr(w)) {
                    out.weapon_def = (int)g_mem.Read<int16_t>(w + schemas::weapon::m_iItemDefinitionIndex);
                    int clip = g_mem.Read<int32_t>(w + schemas::weapon::m_iClip1);
                    out.clip = (clip >= 0 && clip <= 500) ? clip : -1;
                    out.clip_capacity = WeaponClipCapacity(out.weapon_def);
                }
            }
        }

        if (!out.dormant) {
            uintptr_t bone_array = GetBoneArrayPtr(pawn);
            if (Memory::IsValidPtr(bone_array)) {
                ResolveBoneStride(bone_array, out.origin);
                size_t base = (g_bone_stride == 48 ? 32 : 0);
                uint8_t bone_blob[48 * 48];
                size_t bone_bytes = 48u * (size_t)g_bone_stride;
                if (g_mem.ReadBlob(bone_array, bone_blob, bone_bytes)) {
                    for (int b : k_draw_bones) {
                        Vec3 pos;
                        memcpy(&pos, bone_blob + (size_t)b * (size_t)g_bone_stride + base, 12);
                        if (!ValidBone(pos, out.origin))
                            pos = Vec3{};
                        out.bones[b] = pos;
                    }
                }
            }
        }

        out.head_estimated = false;
        out.head_pos = out.bones[schemas::bones::Head];
        if (out.head_pos.IsZero() || !IsValidVec3(out.head_pos)) {
            // Estimasi mengikuti postur asli via viewOffset (berdiri ~64, jongkok ~46)
            out.head_estimated = true;
            float height = k_head_offset_z;
            Vec3 view = g_mem.ReadVec3(pawn + schemas::player_pawn::m_vecViewOffset);
            if (IsValidVec3(view) && view.z >= 20.f && view.z <= 90.f)
                height = view.z + 6.f;
            out.head_pos = { out.origin.x, out.origin.y, out.origin.z + height };
        }

        out.distance = (out.origin - local_origin).Length() * 0.01905f;
        if (std::isnan(out.distance) || std::isinf(out.distance)) out.distance = 0.f;
    }

    return true;
}

// Anti-blink cache: data bertahan sesaat saat read gagal / dormant flip
FORCEINLINE void UpdatePlayerCache(PlayerData& old, const PlayerData* cur, DWORD now,
                                   DWORD& stamp_ms) noexcept {
    if (cur) {
        if (!cur->dormant) { old = *cur; stamp_ms = now; }
        else if (cur->pawn_ptr == old.pawn_ptr && old.pawn_ptr && now - stamp_ms < 250) {
            old.ghost = true; old.dormant = true;
        } else { old = PlayerData{}; stamp_ms = 0; }
    } else if (old.pawn_ptr && now - stamp_ms < 350) {
        old.ghost = true; old.dormant = true;
    } else { old = PlayerData{}; stamp_ms = 0; }
}

// =============================================================================
//  ESP Configurations (port H-V2)
// =============================================================================
struct EspConfig {
    bool  enabled        = true;
    bool  draw_box       = true;
    int   box_style      = 0; // 0 = Full Box, 1 = Corner Box
    bool  draw_health    = true;
    bool  draw_armor     = true;
    bool  draw_ammo      = false;
    bool  draw_weapon    = true;
    bool  draw_name      = true;
    bool  draw_distance  = true;
    bool  draw_bones     = true;
    bool  draw_snaplines = false;
    bool  draw_head_dot  = true;
    bool  draw_eyeray    = false;
    bool  offscreen_arrows = true;
    bool  teammates      = false;
    float box_color[4]   = { 0.95f, 0.25f, 0.25f, 1.0f };
    float team_color[4]  = { 0.25f, 0.75f, 0.95f, 1.0f };
    float bone_color[4]  = { 1.00f, 1.00f, 1.00f, 0.9f };
    float snap_color[4]  = { 1.00f, 0.85f, 0.20f, 0.8f };
    float arrow_color[4] = { 1.00f, 0.45f, 0.15f, 0.9f };
};
inline EspConfig g_esp_cfg;

// Input sintetis (bhop) diblokir saat menu terbuka.
inline bool g_input_blocked = false;

// Build number game, dibaca sekali saat attach (nilai di offsets.h).
inline int g_game_build = 0;

// =============================================================================
//  Misc Configurations (port H-V2)
// =============================================================================
struct MiscConfig {
    bool  bhop            = false;
    bool  noshake         = true;
    bool  glow            = false;
    bool  radar_spotted   = true;
    bool  fov_changer     = false;
    float desired_fov     = 110.0f;
    bool  bomb_timer      = true;
    bool  hitsound        = false;
    bool  watermark       = true;
    bool  obs_bypass      = false;
};
inline MiscConfig g_misc_cfg;

// =============================================================================
//  Crosshair Overlay Configurations
// =============================================================================
struct CrosshairConfig {
    bool  enabled   = false;
    float size      = 6.0f;
    float gap       = 3.0f;
    float thickness = 1.5f;
    float color[4]  = { 0.0f, 1.0f, 0.4f, 1.0f };
};
inline CrosshairConfig g_crosshair_cfg;

// Profile-check bomb ala H-V2: signature clock/bomb di client.dll harus cocok,
// jika tidak maka timer disembunyikan (bukan ditebak).
namespace detail_bomb {
    inline constexpr uintptr_t k_bomb_root_rva = 0xCC3F71;
    inline constexpr uint8_t k_bomb_root[] = { 0x48,0x8b,0x1d,0x40,0x34,0x6d,0x01 };
    inline constexpr uintptr_t k_clock_rva = 0xCC3FE4;
    inline constexpr uint8_t k_clock[] = {
        0x48,0x8b,0x05,0xd5,0x17,0x3f,0x01,0x48,0x8d,0x8b,0xd0,0x11,0x00,0x00,0xf3,0x0f,
        0x10,0x70,0x30,
    };
    template<size_t N> bool SigOk(uintptr_t client, uintptr_t rva,
                                  const uint8_t(&want)[N]) noexcept {
        uint8_t got[N]{};
        return g_mem.ReadBlob(client + rva, got, N) && memcmp(got, want, N) == 0;
    }
    inline uint64_t last_session = 0;
    inline DWORD last_ms = 0;
    inline bool cached = false;
} // namespace detail_bomb
inline bool BombProfileOk(uintptr_t client_base) noexcept {
    using namespace detail_bomb;
    DWORD now = GetTickCount();
    if (last_session == g_mem.session && (now - last_ms) < 1000) return cached;
    last_session = g_mem.session; last_ms = now;
    cached = Memory::IsValidPtr(client_base) &&
        SigOk(client_base, k_bomb_root_rva, k_bomb_root) &&
        SigOk(client_base, k_clock_rva, k_clock);
    return cached;
}

// ---- Beep non-blocking (thread terpisah, ala H-V2) ----
namespace detail_beep {
    inline std::atomic<bool> busy{false};
    inline std::thread worker;
    inline void Once(bool kill) noexcept {
        bool expect = false;
        if (!busy.compare_exchange_strong(expect, true)) return;
        if (worker.joinable()) worker.join();
        try {
            worker = std::thread([kill] { Beep(kill ? 300 : 900, 25); busy = false; });
        } catch (...) { busy = false; }
    }
    inline void Join() noexcept {
        if (worker.joinable()) worker.join();
        busy = false;
    }
} // namespace detail_beep
inline void BeepOnce(bool kill) noexcept { detail_beep::Once(kill); }
inline void JoinBeep() noexcept { detail_beep::Join(); }

// =============================================================================
//  Bomb info (port H-V2)
// =============================================================================
struct BombInfo {
    bool  planted  = false;
    bool  defusing = false;
    float countdown = -1.f;
    float defuse_remaining = -1.f;
    float timer_length = 0.f;
    float defuse_length = 0.f;
    char  defuser[128] = {};
};

inline void ReadBomb(uintptr_t client_base, uintptr_t entity_list, BombInfo& out) noexcept {
    out = BombInfo{};
    if (!g_misc_cfg.bomb_timer) return;
    static DWORD bomb_ms = 0;
    static BombInfo cached{};
    DWORD now = GetTickCount();
    if (bomb_ms && now - bomb_ms < 100) { out = cached; return; }
    bomb_ms = now;

    BombInfo b{};
    if (!Memory::IsValidPtr(client_base)) { cached = b; out = b; return; }
    if (!BombProfileOk(client_base)) { cached = b; out = b; return; }
    uintptr_t bomb = 0;
    if (!g_mem.ReadBlob(client_base + offsets::client::dwPlantedC4, &bomb, sizeof(bomb)) ||
        !Memory::IsValidPtr(bomb)) { cached = b; out = b; return; }

    constexpr auto start = schemas::planted_c4::m_bBombTicking;
    uint8_t fields[schemas::planted_c4::m_hBombDefuser + sizeof(uint32_t) - start]{};
    uintptr_t after = 0;
    if (!g_mem.ReadBlob(bomb + start, fields, sizeof(fields)) ||
        !g_mem.ReadBlob(client_base + offsets::client::dwPlantedC4, &after, sizeof(after)) ||
        after != bomb) { cached = b; out = b; return; }
    auto Flag = [&](std::ptrdiff_t o) noexcept { return fields[o - start]; };
    auto Flt = [&](std::ptrdiff_t o) noexcept {
        float f = 0; memcpy(&f, fields + (o - start), sizeof(f)); return f; };
    using namespace schemas;
    uint8_t ticking = Flag(planted_c4::m_bBombTicking),
            defused = Flag(planted_c4::m_bBombDefused),
            exploded = Flag(planted_c4::m_bHasExploded),
            defusing = Flag(planted_c4::m_bBeingDefused);
    if (ticking > 1 || defused > 1 || exploded > 1 || defusing > 1 ||
        (defused && exploded) || defused || exploded || !ticking) { cached = b; out = b; return; }

    float blow = Flt(planted_c4::m_flC4Blow), length = Flt(planted_c4::m_flTimerLength);
    if (!std::isfinite(blow) || blow <= 0 || !std::isfinite(length) || length < 1 || length > 180) {
        cached = b; out = b; return;
    }
    b.planted = true; b.timer_length = length;

    // Jam simulasi game via GlobalVars (validasi ketat)
    float curtime = -1.f;
    uintptr_t gv = g_mem.Read<uintptr_t>(client_base + offsets::client::dwGlobalVars);
    if (Memory::IsValidPtr(gv)) {
        float t = g_mem.Read<float>(gv + offsets::global_vars::curtime);
        if (std::isfinite(t) && t >= 0 && t <= 1e7f) curtime = t;
    }
    if (curtime >= 0 && blow - curtime >= -1 && blow - curtime <= length + 1)
        b.countdown = (blow - curtime < 0.f) ? 0.f : (blow - curtime);

    if (defusing) {
        uint32_t handle = 0;
        memcpy(&handle, fields + (planted_c4::m_hBombDefuser - start), sizeof(handle));
        uintptr_t defuser = GetEntityByHandle(entity_list, handle);
        float end = Flt(planted_c4::m_flDefuseCountDown), len = Flt(planted_c4::m_flDefuseLength);
        if (Memory::IsValidPtr(defuser) && std::isfinite(end) && std::isfinite(len) &&
            len > 0 && len <= 30 && (curtime < 0 || (end - curtime >= -1 && end - curtime <= len + 1))) {
            b.defusing = true; b.defuse_length = len;
            if (curtime >= 0)
                b.defuse_remaining = (end - curtime < 0.f) ? 0.f : (end - curtime);
            for (int i = 1; i <= k_entity_loop; ++i) {
                uintptr_t ctrl = GetEntityByIndex(entity_list, (uint32_t)i);
                if (!Memory::IsValidPtr(ctrl)) continue;
                uint32_t ph = g_mem.Read<uint32_t>(ctrl + schemas::player_controller::m_hPlayerPawn);
                if (GetEntityByHandle(entity_list, ph) == defuser) {
                    g_mem.ReadString(ctrl + schemas::player_controller::m_iszPlayerName,
                                     b.defuser, sizeof(b.defuser));
                    break;
                }
            }
        }
    }
    cached = b; out = b;
}

// =============================================================================
//  Hit detect utk hitsound (counter server, tanpa tebak HP)
// =============================================================================
struct HitState {
    DWORD last_ms = 0;
    bool  kill = false;
    bool  active = false;
};
inline void DetectHit(uintptr_t client_base, uintptr_t local_pawn, HitState& out) noexcept {
    static int last_hits = -1, last_kills = -1;
    static HitState cur{};
    out = HitState{};
    if (!g_misc_cfg.hitsound || !Memory::IsValidPtr(local_pawn) || !Memory::IsValidPtr(client_base)) {
        last_hits = last_kills = -1; cur = HitState{}; return;
    }
    uintptr_t svc = g_mem.Read<uintptr_t>(local_pawn + schemas::player_pawn::m_pBulletServices);
    uintptr_t ctrl = g_mem.Read<uintptr_t>(client_base + offsets::client::dwLocalPlayerController);
    uintptr_t kills = Memory::IsValidPtr(ctrl)
        ? g_mem.Read<uintptr_t>(ctrl + schemas::player_controller::m_pActionTrackingServices) : 0;
    int hits = 0, nkill = 0;
    bool hits_ok = Memory::IsValidPtr(svc) &&
        g_mem.ReadBlob(svc + schemas::bullet_services::m_totalHitsOnServer, &hits, sizeof(hits)) &&
        hits >= 0 && hits < 100000;
    bool kills_ok = Memory::IsValidPtr(kills) &&
        g_mem.ReadBlob(kills + schemas::action_tracking_services::m_iNumRoundKills, &nkill, sizeof(nkill)) &&
        nkill >= 0 && nkill < 1000;
    if (!hits_ok && !kills_ok) { last_hits = last_kills = -1; cur = HitState{}; return; }
    bool hit = hits_ok && last_hits >= 0 && hits > last_hits;
    bool kill = kills_ok && last_kills >= 0 && nkill > last_kills;
    last_hits = hits_ok ? hits : -1;
    last_kills = kills_ok ? nkill : -1;
    if (hit || kill) {
        cur.last_ms = GetTickCount(); cur.kill = kill; cur.active = true;
        BeepOnce(kill);
    }
    if (cur.active && GetTickCount() - cur.last_ms >= 300) cur = HitState{};
    out = cur;
}

// =============================================================================
//  Patch tracker — tulis dengan restore otomatis (port H-V2)
// =============================================================================
namespace detail {
    enum class PatchKind { Glow, Radar, Fov };
    struct Patch {
        uintptr_t address = 0, owner = 0, controller = 0;
        PatchKind kind = PatchKind::Glow;
        size_t size = 0;
        uint8_t original[16]{}, written[16]{};
        bool applied = false;
    };
    inline std::vector<Patch> g_patches;

    inline bool SameOwner(const Patch& p) noexcept {
        if (p.kind == PatchKind::Fov)
            return g_mem.Read<uintptr_t>(
                g_mem.client_base + offsets::client::dwLocalPlayerController) == p.owner;
        uintptr_t pawn = g_mem.Read<uintptr_t>(
            g_mem.client_base + offsets::client::dwLocalPlayerPawn);
        if (!p.controller) return pawn == p.owner;
        uintptr_t list = g_mem.Read<uintptr_t>(
            g_mem.client_base + offsets::client::dwEntityList);
        if (!Memory::IsValidPtr(list) || !Memory::IsValidPtr(p.controller)) return false;
        uint32_t h = g_mem.Read<uint32_t>(
            p.controller + schemas::player_controller::m_hPlayerPawn);
        return GetEntityByHandle(list, h) == p.owner;
    }

    inline void RestoreKind(PatchKind kind,
                            const std::array<PlayerData, k_max_entities>* active = nullptr,
                            int team = 0) noexcept {
        for (auto it = g_patches.begin(); it != g_patches.end();) {
            if (it->kind != kind) { ++it; continue; }
            if (active) {
                bool keep = false;
                for (const auto& p : *active)
                    if (p.pawn_ptr == it->owner && p.alive && !p.ghost && !p.dormant &&
                        p.team != team) { keep = true; break; }
                if (keep) { ++it; continue; }
            }
            uint8_t cur[16]{};
            if (it->applied && g_mem.CanModify() && SameOwner(*it) &&
                g_mem.ReadBlob(it->address, cur, it->size) &&
                memcmp(cur, it->written, it->size) == 0) {
                SIZE_T w = 0;
                if (!WriteProcessMemory(g_mem.h_write, (LPVOID)it->address,
                                        it->original, it->size, &w) || w != it->size)
                    ++g_mem.write_failures;
                else { ++it; it = g_patches.erase(it); continue; }
                ++it; continue;
            }
            it = g_patches.erase(it);
        }
    }

    template<class T> void ApplyPatch(PatchKind kind, uintptr_t addr,
                                      const T& value, uintptr_t owner,
                                      uintptr_t controller = 0) noexcept {
        static_assert(sizeof(T) <= 16);
        if (!g_mem.CanModify() ||
            (kind != PatchKind::Radar && g_input_blocked)) return;
        auto it = g_patches.begin();
        for (; it != g_patches.end(); ++it)
            if (it->address == addr && it->owner == owner && it->kind == kind) break;
        if (it == g_patches.end()) {
            if (g_patches.size() >= 512) return;
            Patch p{}; p.address = addr; p.owner = owner; p.controller = controller;
            p.kind = kind; p.size = sizeof(T);
            if (!g_mem.ReadBlob(addr, p.original, sizeof(T))) return;
            g_patches.push_back(p);
            it = g_patches.end() - 1;
        }
        if (g_mem.Write(addr, value)) {
            memcpy(it->written, &value, sizeof(T));
            it->applied = true;
        } else if (!it->applied) {
            g_patches.erase(it);
        }
    }
} // namespace detail

// ---- Bhop -------------------------------------------------------------------
inline void RunBhop(uintptr_t local_pawn) noexcept {
    if (!g_misc_cfg.bhop || !Memory::IsValidPtr(local_pawn)) return;
    if (g_input_blocked) return;
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) return;

    uint32_t flags = g_mem.Read<uint32_t>(local_pawn + schemas::base_entity::m_fFlags);
    bool on_ground = (flags & 1) != 0;

    static bool was_on_ground = false;
    if (g_mem.Read<int32_t>(local_pawn + schemas::base_entity::m_iHealth) <= 0) {
        was_on_ground = false;
        return;
    }
    bool do_jump = on_ground && !was_on_ground;
    was_on_ground = on_ground;
    if (!do_jump) return;

    INPUT inp[2]{};
    inp[0].type = INPUT_KEYBOARD; inp[0].ki.wVk = VK_SPACE;
    inp[0].ki.dwFlags = KEYEVENTF_KEYUP;
    inp[1].type = INPUT_KEYBOARD; inp[1].ki.wVk = VK_SPACE;
    inp[1].ki.dwFlags = 0;
    SendInput(2, inp, sizeof(INPUT));
}

// ---- NoShake: redam getaran kamera visual ------------------------------------
inline void RunNoShake(uintptr_t local_pawn) noexcept {
    if (!g_misc_cfg.noshake || !Memory::IsValidPtr(local_pawn)) return;
    if (g_input_blocked || !g_mem.CanModify()) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 50) return;
    last = now;
    uintptr_t cam = g_mem.Read<uintptr_t>(local_pawn + schemas::player_pawn::m_pCameraServices);
    if (!Memory::IsValidPtr(cam)) return;
    uintptr_t addr = cam + schemas::camera_services::m_vecCsViewPunchAngle;
    Vec3 v = g_mem.ReadVec3(addr);
    if (!v.IsZero()) g_mem.WriteVec3(addr, Vec3{});
}

// ---- Radar spotted (in-game minimap) -----------------------------------------
// ponytail: tulis langsung tiap 200ms SELALU (pola H-V2 RunRadarSpotted).
// ApplyPatch direstore tiap siklus agar mask refresh mengikuti game.
inline void RunRadar(const std::array<PlayerData, k_max_entities>& players,
                     int local_team) noexcept {
    using namespace detail;
    if (!g_misc_cfg.radar_spotted) { RestoreKind(PatchKind::Radar); return; }
    if (!g_mem.CanModify()) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (last && now - last < 200) return;
    last = now;
    RestoreKind(PatchKind::Radar, &players, local_team);
    for (int i = 1; i <= k_entity_loop; ++i) {
        const PlayerData& p = players[i];
        if (!Memory::IsValidPtr(p.pawn_ptr) || !p.alive || p.ghost || p.dormant || p.team == local_team)
            continue;
        uintptr_t addr = p.pawn_ptr
            + schemas::player_pawn::m_entitySpottedState
            + schemas::player_pawn::m_bSpottedByMask_offset;
        ApplyPatch(PatchKind::Radar, addr, ~uint64_t(0), p.pawn_ptr, p.ctrl_ptr);
    }
}

// ---- Glow --------------------------------------------------------------------
inline void RunGlow(const std::array<PlayerData, k_max_entities>& players,
                    int local_team) noexcept {
    using namespace detail;
    if (!g_misc_cfg.glow || g_input_blocked) { RestoreKind(PatchKind::Glow); return; }
    if (!g_mem.CanModify()) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 200) return;
    last = now;
    RestoreKind(PatchKind::Glow, &players, local_team);
    for (int i = 1; i <= k_entity_loop; ++i) {
        const PlayerData& p = players[i];
        if (!Memory::IsValidPtr(p.pawn_ptr) || !p.alive || p.ghost || p.dormant || p.team == local_team)
            continue;
        uintptr_t g = p.pawn_ptr + schemas::player_pawn::m_Glow;
        ApplyPatch(PatchKind::Glow, g + schemas::glow_property::m_bEligibleForScreenHighlight, true, p.pawn_ptr, p.ctrl_ptr);
        ApplyPatch(PatchKind::Glow, g + schemas::glow_property::m_bGlowing, true, p.pawn_ptr, p.ctrl_ptr);
        ApplyPatch(PatchKind::Glow, g + schemas::glow_property::m_iGlowType, 1, p.pawn_ptr, p.ctrl_ptr);
        ApplyPatch(PatchKind::Glow, g + schemas::glow_property::m_nGlowRange, 5000, p.pawn_ptr, p.ctrl_ptr);
        float col[3] = { g_esp_cfg.box_color[0], g_esp_cfg.box_color[1], g_esp_cfg.box_color[2] };
        ApplyPatch(PatchKind::Glow, g + schemas::glow_property::m_fGlowColor, col, p.pawn_ptr, p.ctrl_ptr);
    }
}

// ---- FOV changer ---------------------------------------------------------------
inline void RunFovChanger(uintptr_t client_base) noexcept {
    using namespace detail;
    if (!Memory::IsValidPtr(client_base)) return;
    if (!g_misc_cfg.fov_changer) { RestoreKind(PatchKind::Fov); return; }
    if (g_input_blocked) return;
    uintptr_t pawn = g_mem.Read<uintptr_t>(client_base + offsets::client::dwLocalPlayerPawn);
    uint8_t scoped = 0;
    if (!Memory::IsValidPtr(pawn) ||
        !g_mem.ReadBlob(pawn + schemas::player_pawn::m_bIsScoped, &scoped, sizeof(scoped)))
        return;
    if (scoped) { RestoreKind(PatchKind::Fov); return; }
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 250) return;
    last = now;
    uintptr_t ctrl = g_mem.Read<uintptr_t>(client_base + offsets::client::dwLocalPlayerController);
    if (Memory::IsValidPtr(ctrl))
        ApplyPatch(PatchKind::Fov, ctrl + schemas::player_controller::m_iDesiredFOV,
                   int(g_misc_cfg.desired_fov), ctrl);
}

// ---- Reset patch saat ganti sesi -------------------------------------------------
inline void ResetPatches() noexcept {
    detail::RestoreKind(detail::PatchKind::Glow);
    detail::RestoreKind(detail::PatchKind::Radar);
    detail::RestoreKind(detail::PatchKind::Fov);
    detail::g_patches.clear();
}

// =============================================================================
//  Config persist — teks key=value di samping exe (ala H-V2)
// =============================================================================
namespace config {
    constexpr const char* k_header = "CS2_CONFIG 4";

    inline void ConfigPath(char* out, size_t n) noexcept {
        char exe[MAX_PATH]{};
        DWORD len = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            const char* slash = strrchr(exe, '\\');
            if (slash) {
                snprintf(out, n, "%.*s\\cs2cheat.cfg", (int)(slash - exe), exe);
                return;
            }
        }
        snprintf(out, n, "cs2cheat.cfg");
    }

    enum class Result { Defaults, Loaded, Saved, Missing, Invalid, IoError };
    inline Result last_result = Result::Defaults;
    inline const char* StatusText() noexcept {
        switch (last_result) {
        case Result::Loaded: return "Pengaturan dimuat.";
        case Result::Saved: return "Pengaturan tersimpan.";
        case Result::Missing: return "Config belum ada; memakai pengaturan awal.";
        case Result::Invalid: return "Config tidak valid; default dipakai.";
        case Result::IoError: return "Gagal baca/tulis config.";
        default: return "Pengaturan awal.";
        }
    }

    namespace detail_validate {
        inline void Check(bool&) noexcept {}
    } // namespace detail_validate

    inline bool Validate() noexcept {
        bool ok = true;
        detail_validate::Check(ok);
        if (g_misc_cfg.desired_fov < 60.f || g_misc_cfg.desired_fov > 130.f) ok = false;
        if (g_crosshair_cfg.size < 2.f || g_crosshair_cfg.size > 20.f) ok = false;
        if (g_crosshair_cfg.gap < 0.f || g_crosshair_cfg.gap > 10.f) ok = false;
        if (g_crosshair_cfg.thickness < 1.f || g_crosshair_cfg.thickness > 5.f) ok = false;
        for (int i = 0; i < 4; ++i) {
            auto fin = [&](float v) { if (!(v >= 0.f && v <= 1.f)) ok = false; };
            fin(g_esp_cfg.box_color[i]); fin(g_esp_cfg.team_color[i]);
            fin(g_esp_cfg.bone_color[i]); fin(g_esp_cfg.snap_color[i]);
            fin(g_esp_cfg.arrow_color[i]); fin(g_crosshair_cfg.color[i]);
        }
        if (g_esp_cfg.box_style < 0 || g_esp_cfg.box_style > 1) ok = false;
        return ok;
    }

    namespace detail_cfg {
        template<class F> void Visit(F&& v) {
            v("esp.enabled", g_esp_cfg.enabled); v("esp.draw_box", g_esp_cfg.draw_box);
            v("esp.box_style", g_esp_cfg.box_style, 0, 1); v("esp.draw_health", g_esp_cfg.draw_health);
            v("esp.draw_armor", g_esp_cfg.draw_armor); v("esp.draw_ammo", g_esp_cfg.draw_ammo);
            v("esp.draw_weapon", g_esp_cfg.draw_weapon); v("esp.draw_name", g_esp_cfg.draw_name);
            v("esp.draw_distance", g_esp_cfg.draw_distance); v("esp.draw_bones", g_esp_cfg.draw_bones);
            v("esp.draw_snaplines", g_esp_cfg.draw_snaplines); v("esp.draw_head_dot", g_esp_cfg.draw_head_dot);
            v("esp.draw_eyeray", g_esp_cfg.draw_eyeray); v("esp.offscreen_arrows", g_esp_cfg.offscreen_arrows);
            v("esp.teammates", g_esp_cfg.teammates);
            for (int i = 0; i < 4; ++i) {
                char k[32];
                snprintf(k, sizeof k, "esp.box_color%d", i); v(k, g_esp_cfg.box_color[i], 0., 1.);
                snprintf(k, sizeof k, "esp.team_color%d", i); v(k, g_esp_cfg.team_color[i], 0., 1.);
                snprintf(k, sizeof k, "esp.bone_color%d", i); v(k, g_esp_cfg.bone_color[i], 0., 1.);
                snprintf(k, sizeof k, "esp.snap_color%d", i); v(k, g_esp_cfg.snap_color[i], 0., 1.);
                snprintf(k, sizeof k, "esp.arrow_color%d", i); v(k, g_esp_cfg.arrow_color[i], 0., 1.);
            }
            v("misc.bhop", g_misc_cfg.bhop); v("misc.noshake", g_misc_cfg.noshake);
            v("misc.glow", g_misc_cfg.glow); v("misc.radar_spotted", g_misc_cfg.radar_spotted);
            v("misc.fov_changer", g_misc_cfg.fov_changer);
            v("misc.desired_fov", g_misc_cfg.desired_fov, 60., 130.);
            v("misc.bomb_timer", g_misc_cfg.bomb_timer); v("misc.hitsound", g_misc_cfg.hitsound);
            v("misc.watermark", g_misc_cfg.watermark); v("misc.obs_bypass", g_misc_cfg.obs_bypass);
            v("crosshair.enabled", g_crosshair_cfg.enabled);
            v("crosshair.size", g_crosshair_cfg.size, 2., 20.);
            v("crosshair.gap", g_crosshair_cfg.gap, 0., 10.);
            v("crosshair.thickness", g_crosshair_cfg.thickness, 1., 5.);
            for (int i = 0; i < 4; ++i) {
                char k[32]; snprintf(k, sizeof k, "crosshair.color%d", i);
                v(k, g_crosshair_cfg.color[i], 0., 1.);
            }
        }
    } // namespace detail_cfg

    inline bool Save(const char* path) noexcept {
        if (!Validate()) { last_result = Result::Invalid; return false; }
        FILE* f = fopen(path, "w");
        if (!f) { last_result = Result::IoError; return false; }
        bool ok = fprintf(f, "%s\n", k_header) > 0;
        detail_cfg::Visit([&](const char* key, auto& val, double lo = 0., double hi = 1.) {
            (void)lo; (void)hi;
            if (fprintf(f, "%s=%.6g\n", key, double(val)) < 0) ok = false;
        });
        fclose(f);
        last_result = ok ? Result::Saved : Result::IoError;
        return ok;
    }

    inline bool Load(const char* path) noexcept {
        FILE* f = fopen(path, "r");
        if (!f) { last_result = Result::Missing; return false; }
        char line[128]{};
        if (!fgets(line, sizeof line, f) || strncmp(line, k_header, strlen(k_header)) != 0) {
            fclose(f); last_result = Result::Invalid; return false;
        }
        bool ok = true;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (!eq) { ok = false; break; }
            *eq++ = 0;
            char* end = nullptr;
            double d = strtod(eq, &end);
            if (end == eq || !std::isfinite(d)) { ok = false; break; }
            detail_cfg::Visit([&](const char* key, auto& val, double lo = 0., double hi = 1.) {
                if (strcmp(key, line) != 0) return;
                using T = std::remove_reference_t<decltype(val)>;
                if (d < lo || d > hi) { ok = false; return; }
                if constexpr (std::is_same_v<T, bool>) {
                    if (d != 0 && d != 1) { ok = false; return; }
                    val = (d == 1);
                } else if constexpr (std::is_integral_v<T>) {
                    if (std::trunc(d) != d) { ok = false; return; }
                    val = (T)d;
                } else {
                    val = (T)d;
                }
            });
        }
        fclose(f);
        if (ok) ok = Validate();
        last_result = ok ? Result::Loaded : Result::Invalid;
        return ok;
    }
}
