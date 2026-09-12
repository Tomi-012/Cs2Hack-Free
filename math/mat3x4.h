#pragma once

// =============================================================================
//  Mat3x4 — 3-baris × 4-kolom (row-major), C++26, Clang 22, SSE2
//  Digunakan untuk bone transforms di CS2 (m_boneArray)
// =============================================================================

#include "vec3.h"
#include <immintrin.h>
#include <cstdint>

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif

struct alignas(16) Mat3x4 {
    // 3 baris, masing-masing 4 float
    float m[3][4];

    // ---- Konstruktor -------------------------------------------------------
    FORCEINLINE Mat3x4() noexcept {
        _mm_storeu_ps(m[0], _mm_setzero_ps());
        _mm_storeu_ps(m[1], _mm_setzero_ps());
        _mm_storeu_ps(m[2], _mm_setzero_ps());
    }

    FORCEINLINE Mat3x4(
        float m00, float m01, float m02, float m03,
        float m10, float m11, float m12, float m13,
        float m20, float m21, float m22, float m23) noexcept
    {
        m[0][0]=m00; m[0][1]=m01; m[0][2]=m02; m[0][3]=m03;
        m[1][0]=m10; m[1][1]=m11; m[1][2]=m12; m[1][3]=m13;
        m[2][0]=m20; m[2][1]=m21; m[2][2]=m22; m[2][3]=m23;
    }

    // ---- Akses baris sebagai Vec3 (hanya xyz) ------------------------------
    [[nodiscard]] FORCEINLINE Vec3 Row(int r) const noexcept {
        return { m[r][0], m[r][1], m[r][2] };
    }

    // ---- Transformasi titik (multiply point): hasil = M * p + translation --
    [[nodiscard]] FORCEINLINE Vec3 TransformPoint(const Vec3& p) const noexcept {
        // Setiap baris: dot(row_xyz, p) + translation
        __m128 px = _mm_set1_ps(p.x);
        __m128 py = _mm_set1_ps(p.y);
        __m128 pz = _mm_set1_ps(p.z);

        __m128 row0 = _mm_load_ps(m[0]);
        __m128 row1 = _mm_load_ps(m[1]);
        __m128 row2 = _mm_load_ps(m[2]);

        // dot tiap baris dengan (px, py, pz) + translation (kolom 3)
        // Kita load kolom 3 sebagai translation
        __m128 tx = _mm_set1_ps(m[0][3]);
        __m128 ty = _mm_set1_ps(m[1][3]);
        __m128 tz = _mm_set1_ps(m[2][3]);

        float rx = m[0][0]*p.x + m[0][1]*p.y + m[0][2]*p.z + m[0][3];
        float ry = m[1][0]*p.x + m[1][1]*p.y + m[1][2]*p.z + m[1][3];
        float rz = m[2][0]*p.x + m[2][1]*p.y + m[2][2]*p.z + m[2][3];

        (void)px; (void)py; (void)pz;
        (void)row0; (void)row1; (void)row2;
        (void)tx; (void)ty; (void)tz;

        return { rx, ry, rz };
    }

    // ---- Transformasi vektor (tanpa translasi) -----------------------------
    [[nodiscard]] FORCEINLINE Vec3 TransformVector(const Vec3& v) const noexcept {
        return {
            m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z,
            m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z,
            m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z
        };
    }

    // ---- Ambil posisi (kolom translasi = kolom 3) --------------------------
    [[nodiscard]] FORCEINLINE Vec3 Origin() const noexcept {
        return { m[0][3], m[1][3], m[2][3] };
    }

    // ---- Transpose 3x3 (tanpa translasi) -----------------------------------
    [[nodiscard]] FORCEINLINE Mat3x4 Transposed3x3() const noexcept {
        return {
            m[0][0], m[1][0], m[2][0], 0.f,
            m[0][1], m[1][1], m[2][1], 0.f,
            m[0][2], m[1][2], m[2][2], 0.f
        };
    }

    // ---- Matrix concat (M_out = this * other) --------------------------------
    [[nodiscard]] Mat3x4 Concat(const Mat3x4& o) const noexcept;

};
