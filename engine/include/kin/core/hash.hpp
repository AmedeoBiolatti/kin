#pragma once
#include <cstddef>
#include <kin/core/types.hpp>

namespace kin {
// Stable FNV-1a bytes, also used by snapshots without the determinism tooling.
u64 hash_bytes(const void* data, std::size_t size, u64 seed = 0xcbf29ce484222325ull);
} // namespace kin
