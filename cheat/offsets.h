#pragma once

// =============================================================================
//  offsets.h — CS2 Offsets
//  Sumber : sezzyaep/CS2-OFFSETS 14181 + a2x/cs2-dumper schema
//  Build  : 14181 (disamakan H-V2 yang berfungsi)
// =============================================================================

#include <cstddef>

namespace offsets {

    namespace client {
        constexpr std::ptrdiff_t dwEntityList            = 0x2577BE0;
        constexpr std::ptrdiff_t dwGlobalVars            = 0x20B57C0;
        constexpr std::ptrdiff_t dwLocalPlayerController = 0x23A78D0;
        constexpr std::ptrdiff_t dwLocalPlayerPawn       = 0x23CCC08;
        constexpr std::ptrdiff_t dwPlantedC4             = 0x23973B8;
        constexpr std::ptrdiff_t dwViewMatrix            = 0x23D21F0;
    }

    namespace global_vars {
        constexpr std::ptrdiff_t curtime = 0x30;
    }

    namespace engine2 {
        constexpr std::ptrdiff_t dwBuildNumber = 0x6105A4;
    }

} // namespace offsets

namespace schemas {

    // C_BaseEntity
    namespace base_entity {
        constexpr std::ptrdiff_t m_pGameSceneNode    = 0x330;
        constexpr std::ptrdiff_t m_iHealth           = 0x34C;
        constexpr std::ptrdiff_t m_lifeState         = 0x354;  // uint8, 0=alive
        constexpr std::ptrdiff_t m_iTeamNum          = 0x3E7;  // uint8, 2=T, 3=CT
        constexpr std::ptrdiff_t m_fFlags            = 0x3F4;
    }

    // CGameSceneNode
    namespace scene_node {
        constexpr std::ptrdiff_t m_vecAbsOrigin      = 0xC8;
        constexpr std::ptrdiff_t m_bDormant          = 0x103;
        constexpr std::ptrdiff_t m_modelState        = 0x140;
    }

    namespace model_state {
        constexpr std::ptrdiff_t m_boneArray         = 0x80;
    }

    // CCSPlayerController
    namespace player_controller {
        constexpr std::ptrdiff_t m_iszPlayerName     = 0x6F4;  // char[128]
        constexpr std::ptrdiff_t m_hPlayerPawn       = 0x914;
        constexpr std::ptrdiff_t m_iDesiredFOV       = 0x78C;  // int32
        constexpr std::ptrdiff_t m_pActionTrackingServices = 0x820;
    }

    // C_CSPlayerPawnBase + C_CSPlayerPawn
    namespace player_pawn {
        constexpr std::ptrdiff_t m_pWeaponServices        = 0x1208;
        constexpr std::ptrdiff_t m_pCameraServices        = 0x1240;
        constexpr std::ptrdiff_t m_pBulletServices        = 0x1490;
        constexpr std::ptrdiff_t m_bIsScoped              = 0x1C78;
        constexpr std::ptrdiff_t m_entitySpottedState     = 0x1C60;
        constexpr std::ptrdiff_t m_bSpottedByMask_offset  = 0x0C;   // dalam EntitySpottedState_t
        constexpr std::ptrdiff_t m_ArmorValue             = 0x1CA4;
        constexpr std::ptrdiff_t m_angEyeAngles           = 0x3350;
        constexpr std::ptrdiff_t m_vecViewOffset          = 0xE78;
        constexpr std::ptrdiff_t m_Glow                   = 0xDE0;  // CGlowProperty
        namespace weapon_services {
            constexpr std::ptrdiff_t m_hActiveWeapon      = 0x60;
        }
    }

    namespace bullet_services {
        constexpr std::ptrdiff_t m_totalHitsOnServer = 0x48;
    }

    namespace action_tracking_services {
        constexpr std::ptrdiff_t m_iNumRoundKills = 0x128;
    }

    namespace camera_services {
        constexpr std::ptrdiff_t m_vecCsViewPunchAngle = 0x48;
    }

    // CGlowProperty
    namespace glow_property {
        constexpr std::ptrdiff_t m_fGlowColor               = 0x08;  // float[3]
        constexpr std::ptrdiff_t m_iGlowType                = 0x30;  // int
        constexpr std::ptrdiff_t m_nGlowRange               = 0x38;  // int
        constexpr std::ptrdiff_t m_bEligibleForScreenHighlight = 0x50; // bool
        constexpr std::ptrdiff_t m_bGlowing                 = 0x51;  // bool
    }

    // C_BasePlayerWeapon
    namespace weapon {
        constexpr std::ptrdiff_t m_iItemDefinitionIndex = 0x11A8 + 0x50 + 0x1BA; // = 0x13B2
        constexpr std::ptrdiff_t m_iClip1               = 0x1700;
    }

    // C_PlantedC4
    namespace planted_c4 {
        constexpr std::ptrdiff_t m_bHasExploded = 0x11D5;
        constexpr std::ptrdiff_t m_bBombTicking = 0x11A0;
        constexpr std::ptrdiff_t m_flTimerLength = 0x11D8;
        constexpr std::ptrdiff_t m_bBeingDefused = 0x11DC;
        constexpr std::ptrdiff_t m_flC4Blow           = 0x11D0;
        constexpr std::ptrdiff_t m_flDefuseLength     = 0x11EC;
        constexpr std::ptrdiff_t m_flDefuseCountDown  = 0x11F0;
        constexpr std::ptrdiff_t m_bBombDefused       = 0x11F4;
        constexpr std::ptrdiff_t m_hBombDefuser       = 0x11F8;
    }

    // Entity list traversal — build 14181
    //   entity_system = *(client + dwEntityList)
    //   bulk   = *(entity_system + 0x10 + 8 * (idx >> 9))
    //   entity = *(bulk + 0x70 * (idx & 0x1FF))
    namespace entity_list {
        constexpr int ENTITY_SHIFT = 9;
        constexpr int ENTITY_SIZE  = 8;
        constexpr int BULK_STRIDE  = 8;
        constexpr std::ptrdiff_t BULK_HEADER     = 0x10;
        constexpr std::ptrdiff_t IDENTITY_SIZE   = 0x70;
        constexpr std::ptrdiff_t IDENTITY_ENTITY = 0x00;
        constexpr std::ptrdiff_t IDENTITY_HANDLE = 0x10;
    }

    // Bone indices
    namespace bones {
        constexpr int Head      = 6;
        constexpr int Neck      = 5;
        constexpr int Spine3    = 4;
        constexpr int Spine2    = 3;
        constexpr int Spine1    = 2;
        constexpr int Pelvis    = 0;
        constexpr int LeftHand  = 13;
        constexpr int RightHand = 38;
        constexpr int LeftFoot  = 22;
        constexpr int RightFoot = 47;
    }

} // namespace schemas

// Build game yang dipakai generate offset ini
constexpr int k_offset_build = 14181;
