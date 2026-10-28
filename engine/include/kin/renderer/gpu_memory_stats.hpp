#pragma once
#include <kin/core/types.hpp>

namespace kin {
// Descriptor payload for Kin-owned SDL_GPU handles, not physical VRAM. Releases
// leave the ledger when handed to SDL; retirement and cycle backings are opaque.
struct GpuMemoryStats {
    bool available = false;
    u64 texture_bytes = 0, buffer_bytes = 0, transfer_bytes = 0;
    u64 pooled_texture_bytes = 0; // subset of texture_bytes
    u64 peak_payload_bytes = 0, live_handles = 0;
    u64 allocations = 0, releases = 0, allocation_failures = 0;
    u64 payload_bytes() const { return texture_bytes + buffer_bytes + transfer_bytes; }
    u64 active_payload_bytes() const { return payload_bytes() - pooled_texture_bytes; }
};
} // namespace kin
