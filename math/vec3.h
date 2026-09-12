#pragma once

// =============================================================================
//  Vec3 — 3D vector, C++26, Clang 22, SSE2/SSE4.1
//  16-byte aligned agar aman di-load ke __m128
// =============================================================================

#include <immintrin.h>
#include <cmath>
#include <cstdint>

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif

struct alignas(16) Vec3 {
    float x, y, z;
    float _pad = 0.f;   // padding agar 16-byte — jangan disentuh

    // ---- Konstruktor -------------------------------------------------------
    FORCEINLINE Vec3() noexcept : x(0.f), y(0.f), z(0.f), _pad(0.f) {}
    FORCEINLINE Vec3(float x_, float y_, float z_) noexcept
        : x(x_), y(y_), z(z_), _pad(0.f) {}
    FORCEINLINE explicit Vec3(float s) noexcept : x(s), y(s), z(s), _pad(0.f) {}

    // Load/store helper SSE
    FORCEINLINE __m128 ToM128() const noexcept {
        return _mm_load_ps(reinterpret_cast<const float*>(this));
    }
    FORCEINLINE void FromM128(__m128 v) noexcept {
        _mm_store_ps(reinterpret_cast<float*>(this), v);
    }

    // ---- Operator aritmetik (SSE) -----------------------------------------
    [[nodiscard]] FORCEINLINE Vec3 operator+(const Vec3& o) const noexcept {
        Vec3 r;
        r.FromM128(_mm_add_ps(ToM128(), o.ToM128()));
        return r;
    }
    [[nodiscard]] FORCEINLINE Vec3 operator-(const Vec3& o) const noexcept {
        Vec3 r;
        r.FromM128(_mm_sub_ps(ToM128(), o.ToM128()));
        return r;
    }
    [[nodiscard]] FORCEINLINE Vec3 operator*(float s) const noexcept {
        Vec3 r;
        r.FromM128(_mm_mul_ps(ToM128(), _mm_set1_ps(s)));
        return r;
    }
    [[nodiscard]] FORCEINLINE Vec3 operator/(float s) const noexcept {
        return *this * (1.f / s);
    }
    [[nodiscard]] FORCEINLINE Vec3 operator-() const noexcept {
        return { -x, -y, -z };
    }

    FORCEINLINE Vec3& operator+=(const Vec3& o) noexcept { FromM128(_mm_add_ps(ToM128(), o.ToM128())); return *this; }
    FORCEINLINE Vec3& operator-=(const Vec3& o) noexcept { FromM128(_mm_sub_ps(ToM128(), o.ToM128())); return *this; }
    FORCEINLINE Vec3& operator*=(float s)        noexcept { FromM128(_mm_mul_ps(ToM128(), _mm_set1_ps(s))); return *this; }

    FORCEINLINE bool operator==(const Vec3& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
    FORCEINLINE bool operator!=(const Vec3& o) const noexcept { return !(*this == o); }

    // ---- Matematika -------------------------------------------------------

    // Dot product via SSE4.1 dpps
    [[nodiscard]] FORCEINLINE float Dot(const Vec3& o) const noexcept {
        // mask 0x71: kalikan x,y,z (bit 4-6), taruh hasil di bit 0
        return _mm_cvtss_f32(_mm_dp_ps(ToM128(), o.ToM128(), 0x71));
    }

    [[nodiscard]] FORCEINLINE float LengthSq() const noexcept {
        return Dot(*this);
    }

    [[nodiscard]] FORCEINLINE float Length() const noexcept {
        return _mm_cvtss_f32(_mm_sqrt_ss(_mm_dp_ps(ToM128(), ToM128(), 0x71)));
    }

    [[nodiscard]] FORCEINLINE float Length2D() const noexcept {
        __m128 v = _mm_set_ps(0.f, 0.f, y, x);
        __m128 sq = _mm_mul_ps(v, v);
        __m128 sum = _mm_hadd_ps(sq, sq);
        return _mm_cvtss_f32(_mm_sqrt_ss(sum));
    }

    // Cross product — tidak bisa full SSE3 tanpa shuffle trik, tapi masih cepat
    [[nodiscard]] FORCEINLINE Vec3 Cross(const Vec3& o) const noexcept {
        // a.yzx * b.zxy - a.zxy * b.yzx
        __m128 a  = ToM128();
        __m128 b  = o.ToM128();
        __m128 a_yzx = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1));
        __m128 b_yzx = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
        __m128 a_zxy = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 1, 0, 2));
        __m128 b_zxy = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2));
        Vec3 r;
        r.FromM128(_mm_sub_ps(_mm_mul_ps(a_yzx, b_zxy),
                              _mm_mul_ps(a_zxy, b_yzx)));
        return r;
    }

    [[nodiscard]] FORCEINLINE Vec3 Normalized() const noexcept {
        float len = Length();
        if (__builtin_expect(len < 1e-6f, 0)) return {};
        return *this * (1.f / len);
    }

    // Fast reciprocal sqrt (Newton–Raphson via rsqrtps, ~22-bit precision)
    [[nodiscard]] FORCEINLINE Vec3 FastNormalized() const noexcept {
        __m128 v   = ToM128();
        __m128 dot = _mm_dp_ps(v, v, 0x7F);
        __m128 rsq = _mm_rsqrt_ps(dot);
        // satu iterasi Newton: rsq = rsq * (1.5 - 0.5 * dot * rsq * rsq)
        __m128 half = _mm_set1_ps(0.5f);
        __m128 three_halves = _mm_set1_ps(1.5f);
        rsq = _mm_mul_ps(rsq,
              _mm_sub_ps(three_halves,
              _mm_mul_ps(half,
              _mm_mul_ps(dot, _mm_mul_ps(rsq, rsq)))));
        Vec3 r;
        r.FromM128(_mm_mul_ps(v, rsq));
        return r;
    }

    [[nodiscard]] FORCEINLINE bool IsZero() const noexcept {
        return x == 0.f && y == 0.f && z == 0.f;
    }

    // Floor tiap komponen — inline asm Intel syntax (lebih cepat dari std::floor)
    [[nodiscard]] FORCEINLINE Vec3 Floor() const noexcept {
        Vec3 r;
        r.FromM128(_mm_floor_ps(ToM128()));
        return r;
    }

    [[nodiscard]] FORCEINLINE Vec3 Ceil() const noexcept {
        Vec3 r;
        r.FromM128(_mm_ceil_ps(ToM128()));
        return r;
    }

};

FORCEINLINE Vec3 operator*(float s, const Vec3& v) noexcept { return v * s; }
