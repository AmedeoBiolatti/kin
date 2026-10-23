#pragma once
#include <kin/core/types.hpp>

namespace kin::detail {
inline u64 history3d_mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}
// Nonzero identities are scoped by object/part; zero preserves legacy matching.
inline u64 part_history3d(u64 object, u64 part) {
    if (!object)
        return 0;
    const u64 id = history3d_mix(object ^ history3d_mix(part + 1));
    return id ? id : 1;
}
} // namespace kin::detail
