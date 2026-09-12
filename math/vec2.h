#pragma once

// =============================================================================
//  Vec2 — 2D vector, C++26, Clang 22, SSE2
//  Konvensi: PascalCase fungsi/kelas, snake_case member/lokal
// =============================================================================

#include <immintrin.h>   // SSE / AVX intrinsics
#include <cmath>
#include <cstdint>

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif

// --------------------------------------------------------------------------
// Forward declare print helper (implementasi di print.cpp)
// --------------------------------------------------------------------------
namespace Print { void Log(const char* msg); }

// --------------------------------------------------------------------------
struct alignas(8) Vec2 {
    float x, y;

    // ---- Konstruktor -------------------------------------------------------
    FORCEINLINE Vec2() noexcept : x(0.f), y(0.f) {}
    FORCEINLINE Vec2(float x_, float y_) noexcept : x(x_), y(y_) {}
    FORCEINLINE explicit Vec2(float s) noexcept : x(s), y(s) {}

    // ---- Operator aritmetik -----------------------------------------------
    [[nodiscard]] FORCEINLINE Vec2 operator+(const Vec2& o) const noexcept {
        return { x + o.x, y + o.y };
    }
    [[nodiscard]] FORCEINLINE Vec2 operator-(const Vec2& o) const noexcept {
        return { x - o.x, y - o.y };
    }
    [[nodiscard]] FORCEINLINE Vec2 operator*(float s) const noexcept {
        return { x * s, y * s };
    }
    [[nodiscard]] FORCEINLINE Vec2 operator/(float s) const noexcept {
        float inv = 1.f / s;
        return { x * inv, y * inv };
    }
    FORCEINLINE Vec2& operator+=(const Vec2& o) noexcept { x += o.x; y += o.y; return *this; }
    FORCEINLINE Vec2& operator-=(const Vec2& o) noexcept { x -= o.x; y -= o.y; return *this; }
    FORCEINLINE Vec2& operator*=(float s)        noexcept { x *= s;   y *= s;   return *this; }
    FORCEINLINE bool  operator==(const Vec2& o) const noexcept { return x == o.x && y == o.y; }
    FORCEINLINE bool  operator!=(const Vec2& o) const noexcept { return !(*this == o); }

    // ---- Fungsi matematika ------------------------------------------------
    [[nodiscard]] FORCEINLINE float Dot(const Vec2& o) const noexcept {
        return x * o.x + y * o.y;
    }

    // Length via SSE: load dua float, multiply, hadd, sqrt
    [[nodiscard]] FORCEINLINE float Length() const noexcept {
        __m128 v = _mm_set_ps(0.f, 0.f, y, x);
        __m128 sq = _mm_mul_ps(v, v);
        // hadd: sq[0]+sq[1]
        __m128 hadd = _mm_hadd_ps(sq, sq);
        return _mm_cvtss_f32(_mm_sqrt_ss(hadd));
    }

    [[nodiscard]] FORCEINLINE float LengthSq() const noexcept {
        return x * x + y * y;
    }

    [[nodiscard]] FORCEINLINE Vec2 Normalized() const noexcept {
        float len = Length();
        if (__builtin_expect(len < 1e-6f, 0)) return {};
        float inv = 1.f / len;
        return { x * inv, y * inv };
    }

    [[nodiscard]] FORCEINLINE bool IsZero() const noexcept {
        return x == 0.f && y == 0.f;
    }

};

// Skalar * Vec2
FORCEINLINE Vec2 operator*(float s, const Vec2& v) noexcept {
    return v * s;
}
