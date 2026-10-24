#include <kin/core/hash.hpp>

namespace kin {
u64 hash_bytes(const void* data, std::size_t size, u64 seed) {
    const auto* bytes = static_cast<const u8*>(data);
    u64 hash = seed;
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}
} // namespace kin
