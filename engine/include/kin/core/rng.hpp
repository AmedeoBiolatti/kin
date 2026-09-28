#pragma once

#include <kin/core/types.hpp>

#include <utility>

namespace kin {

// Functional, splittable PRNG with the same usage pattern as JAX keys.
//
// Rules:
//   1. Never reuse a key for two unrelated values.
//   2. Split before generating: auto [key, sub] = split(key);
//   3. The same seed and split tree always produce the same values.
struct RngKey {
    u32 lo = 0;
    u32 hi = 0;

    friend constexpr bool operator==(RngKey, RngKey) = default;
};

namespace rng_detail {

inline u64 mix(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

inline u64 to_u64(RngKey key) {
    return (static_cast<u64>(key.hi) << 32) | static_cast<u64>(key.lo);
}

inline RngKey from_u64(u64 value) {
    return {
        static_cast<u32>(value & 0xffffffffULL),
        static_cast<u32>(value >> 32),
    };
}

} // namespace rng_detail

inline RngKey make_key(u64 seed) {
    return rng_detail::from_u64(rng_detail::mix(seed));
}

inline std::pair<RngKey, RngKey> split(RngKey key) {
    const u64 value = rng_detail::to_u64(key);
    return {
        rng_detail::from_u64(rng_detail::mix(value ^ 0x9e3779b97f4a7c15ULL)),
        rng_detail::from_u64(rng_detail::mix(value ^ 0x6c62272e07bb0142ULL)),
    };
}

inline u32 rng_u32(RngKey key) {
    return static_cast<u32>(
        rng_detail::mix(rng_detail::to_u64(key) ^ 0xdeadbeefcafe0000ULL) >> 32
    );
}

inline f32 rng_f32(RngKey key, f32 lo, f32 hi) {
    constexpr f32 scale = 1.0f / static_cast<f32>(0xffffffffU);
    return lo + static_cast<f32>(rng_u32(key)) * scale * (hi - lo);
}

inline i32 rng_i32(RngKey key, i32 lo, i32 hi) {
    // Guard against an inverted or empty range: without this, hi < lo underflows
    // the unsigned width and produces an out-of-range result. Compute the width
    // in 64-bit so a full i32 span (e.g. lo=INT_MIN, hi=INT_MAX) cannot overflow
    // the signed subtraction.
    if (hi <= lo) {
        return lo;
    }
    const u64 width = static_cast<u64>(static_cast<i64>(hi) - static_cast<i64>(lo) + 1);
    const i64 offset = static_cast<i64>(rng_u32(key) % width);
    return static_cast<i32>(static_cast<i64>(lo) + offset);
}

inline bool rng_bool(RngKey key, f32 probability = 0.5f) {
    return rng_f32(key, 0.0f, 1.0f) < probability;
}

} // namespace kin
