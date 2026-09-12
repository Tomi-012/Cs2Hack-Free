#pragma once

// =============================================================================
//  math_utils.h — namespace math (ramping: hanya yang dipakai cheat)
// =============================================================================

#include "vec3.h"
#include "vec2.h"
#include <numbers>

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif
#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

namespace math {

inline constexpr float Pi      = std::numbers::pi_v<float>;
inline constexpr float Deg2Rad = Pi / 180.f;
inline constexpr float Rad2Deg = 180.f / Pi;

[[nodiscard]] FORCEINLINE float ToRadians(float deg) noexcept { return deg * Deg2Rad; }
[[nodiscard]] FORCEINLINE float ToDegrees(float rad) noexcept { return rad * Rad2Deg; }

} // namespace math
