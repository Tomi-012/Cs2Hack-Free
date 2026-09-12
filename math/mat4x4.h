#pragma once

// =============================================================================
//  Mat4x4 — 4×4 matrix row-major, C++26, Clang 22, SSE2
//  Digunakan untuk dwViewMatrix CS2 (world-to-screen)
// =============================================================================

#include "vec3.h"
#include "vec2.h"
#include <immintrin.h>
#include <cmath>

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif

struct alignas(16) Mat4x4 {
    float m[4][4];

    // ---- Konstruktor -------------------------------------------------------
    FORCEINLINE Mat4x4() noexcept {
        _mm_storeu_ps(m[0], _mm_setzero_ps());
        _mm_storeu_ps(m[1], _mm_setzero_ps());
        _mm_storeu_ps(m[2], _mm_setzero_ps());
        _mm_storeu_ps(m[3], _mm_setzero_ps());
    }

    // Identity
    [[nodiscard]] static FORCEINLINE Mat4x4 Identity() noexcept {
        Mat4x4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.f;
        return r;
    }

    // ---- Row SSE load/store ------------------------------------------------
    FORCEINLINE __m128 LoadRow(int r) const noexcept {
        return _mm_loadu_ps(m[r]);
    }
    FORCEINLINE void StoreRow(int r, __m128 v) noexcept {
        _mm_storeu_ps(m[r], v);
    }

    // ---- Multiply mat × mat (SSE) ------------------------------------------
    [[nodiscard]] Mat4x4 operator*(const Mat4x4& o) const noexcept;

    // ---- Transform Vec3 homogeneous (w=1) ----------------------------------
    [[nodiscard]] FORCEINLINE Vec3 TransformPoint(const Vec3& p) const noexcept {
        float x = m[0][0]*p.x + m[0][1]*p.y + m[0][2]*p.z + m[0][3];
        float y = m[1][0]*p.x + m[1][1]*p.y + m[1][2]*p.z + m[1][3];
        float z = m[2][0]*p.x + m[2][1]*p.y + m[2][2]*p.z + m[2][3];
        return { x, y, z };
    }

    [[nodiscard]] FORCEINLINE bool IsZeroMatrix() const noexcept {
        return m[0][0]==0.f && m[3][3]==0.f && m[1][1]==0.f;
    }
    // ---- WorldToScreen -----------------------------------------------------
    // Mengembalikan true jika titik ada di depan kamera
    // screen_pos = koordinat piksel, screen_w/h = resolusi layar
    [[nodiscard]] FORCEINLINE bool WorldToScreen(
        const Vec3& world,
        Vec2&       screen_pos,
        float       screen_w,
        float       screen_h) const noexcept
    {
        // Baris 0 = X-clip, baris 1 = Y-clip, baris 3 = W
        float clip_x = m[0][0]*world.x + m[0][1]*world.y + m[0][2]*world.z + m[0][3];
        float clip_y = m[1][0]*world.x + m[1][1]*world.y + m[1][2]*world.z + m[1][3];
        float clip_w = m[3][0]*world.x + m[3][1]*world.y + m[3][2]*world.z + m[3][3];

        if (__builtin_expect(std::isnan(clip_w) || std::isinf(clip_w) || clip_w < 0.001f, 0)) return false;

        float inv_w    = 1.f / clip_w;
        float x        = (screen_w * 0.5f) + (clip_x * inv_w) * (screen_w  * 0.5f);
        float y        = (screen_h * 0.5f) - (clip_y * inv_w) * (screen_h  * 0.5f);

        if (__builtin_expect(std::isnan(x) || std::isinf(x) || std::isnan(y) || std::isinf(y), 0)) return false;

        screen_pos.x = x;
        screen_pos.y = y;
        return true;
    }

    // ---- Transpose (SSE) ---------------------------------------------------
    [[nodiscard]] Mat4x4 Transposed() const noexcept;

    // ---- Inverse (cofactor expansion, per spesifikasi tidak diperlukan AVX-512) ----
    [[nodiscard]] Mat4x4 Inverted() const noexcept;

};

