#pragma once

#include <cmath>
#include <cstdint>

namespace umbral
{
// Deterministic, platform-independent integer hash (splitmix-style finaliser).
inline uint32_t hash32 (uint32_t x) noexcept
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

inline uint32_t hash2 (int32_t a, int32_t b, uint32_t seed) noexcept
{
    return hash32 ((uint32_t) a * 0x9e3779b1u ^ hash32 ((uint32_t) b + seed * 0x85ebca6bu + 0x165667b1u));
}

// Lattice value in [-1, 1].
inline float latticeValue (int32_t a, int32_t b, uint32_t seed) noexcept
{
    return (float) (hash2 (a, b, seed) >> 8) * (2.0f / 16777215.0f) - 1.0f;
}

inline float quintic (float t) noexcept { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

// Smooth 2-D value noise, C2-continuous, in [-1, 1]. x = position along the spectrum (lattice cells),
// y = position along the drift clock (lattice cells). A pure function of (x, y, seed): reproducible.
inline float valueNoise2 (float x, float y, uint32_t seed) noexcept
{
    const float fx0 = std::floor (x), fy0 = std::floor (y);
    const int32_t ix = (int32_t) fx0, iy = (int32_t) fy0;
    const float tx = quintic (x - fx0), ty = quintic (y - fy0);
    const float v00 = latticeValue (ix, iy, seed), v10 = latticeValue (ix + 1, iy, seed);
    const float v01 = latticeValue (ix, iy + 1, seed), v11 = latticeValue (ix + 1, iy + 1, seed);
    const float a = v00 + (v10 - v00) * tx, b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

// Tiny xorshift generator for the per-frame noise phases (state is reset with the engine -> deterministic).
struct XorShift32
{
    uint32_t s = 2463534242u;
    uint32_t next() noexcept
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
};
} // namespace umbral
