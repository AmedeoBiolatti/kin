#include "gpu_memory.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>

namespace kin::gpu {
namespace {
enum class Kind { Texture, Buffer, Transfer };
struct Allocation {
    u64 bytes;
    Kind kind;
    bool pooled = false;
};
struct Ledger {
    GpuMemoryStats stats{.available = true};
    std::unordered_map<void*, Allocation> handles;
};
std::mutex mutex;
std::unordered_map<SDL_GPUDevice*, Ledger> devices;
u64& category(GpuMemoryStats& s, Kind kind) {
    if (kind == Kind::Texture)
        return s.texture_bytes;
    if (kind == Kind::Buffer)
        return s.buffer_bytes;
    return s.transfer_bytes;
}
void created(SDL_GPUDevice* device, void* handle, u64 bytes, Kind kind) {
    const std::lock_guard lock{mutex};
    auto found = devices.find(device);
    if (found == devices.end())
        return;
    auto& ledger = found->second;
    if (!handle) {
        ++ledger.stats.allocation_failures;
        return;
    }
    ledger.handles.emplace(handle, Allocation{bytes, kind});
    category(ledger.stats, kind) += bytes;
    ++ledger.stats.allocations;
    ++ledger.stats.live_handles;
    ledger.stats.peak_payload_bytes = std::max(ledger.stats.peak_payload_bytes, ledger.stats.payload_bytes());
}
void released(SDL_GPUDevice* device, void* handle) {
    const std::lock_guard lock{mutex};
    auto found = devices.find(device);
    if (found == devices.end())
        return;
    auto& ledger = found->second;
    auto allocation = ledger.handles.find(handle);
    if (allocation == ledger.handles.end())
        return;
    category(ledger.stats, allocation->second.kind) -= allocation->second.bytes;
    if (allocation->second.pooled)
        ledger.stats.pooled_texture_bytes -= allocation->second.bytes;
    ledger.handles.erase(allocation);
    ++ledger.stats.releases;
    --ledger.stats.live_handles;
}
} // namespace
void register_memory_device(SDL_GPUDevice* device) {
    const std::lock_guard lock{mutex};
    devices.try_emplace(device);
}
void unregister_memory_device(SDL_GPUDevice* device) {
    const std::lock_guard lock{mutex};
    devices.erase(device);
}
GpuMemoryStats gpu_memory_stats(SDL_GPUDevice* device) {
    const std::lock_guard lock{mutex};
    auto found = devices.find(device);
    return found == devices.end() ? GpuMemoryStats{} : found->second.stats;
}
u64 texture_payload_bytes(const SDL_GPUTextureCreateInfo& info) {
    u32 bx = 1, by = 1;
    const auto format = info.format;
    if ((format >= SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM && format <= SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT) ||
        (format >= SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB && format <= SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB))
        bx = by = 4;
    constexpr std::array<std::array<u32, 2>, 14> astc{{{4, 4},
                                                       {5, 4},
                                                       {5, 5},
                                                       {6, 5},
                                                       {6, 6},
                                                       {8, 5},
                                                       {8, 6},
                                                       {8, 8},
                                                       {10, 5},
                                                       {10, 6},
                                                       {10, 8},
                                                       {10, 10},
                                                       {12, 10},
                                                       {12, 12}}};
    if (format >= SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM && format <= SDL_GPU_TEXTUREFORMAT_ASTC_12x12_FLOAT) {
        const auto block = astc[(format - SDL_GPU_TEXTUREFORMAT_ASTC_4x4_UNORM) % astc.size()];
        bx = block[0];
        by = block[1];
    }
    const u64 block_bytes = SDL_GPUTextureFormatTexelBlockSize(format);
    u64 bytes = 0;
    for (u32 level = 0; level < info.num_levels; ++level) {
        const u64 w = std::max(1u, info.width >> level), h = std::max(1u, info.height >> level);
        const u64 layers = info.type == SDL_GPU_TEXTURETYPE_3D ? std::max(1u, info.layer_count_or_depth >> level)
                                                               : info.layer_count_or_depth;
        bytes += ((w + bx - 1) / bx) * ((h + by - 1) / by) * layers * block_bytes;
    }
    return bytes * (u64{1} << info.sample_count);
}
void pooled_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture, bool pooled) {
    const std::lock_guard lock{mutex};
    auto d = devices.find(device);
    if (d == devices.end())
        return;
    auto a = d->second.handles.find(texture);
    if (a == d->second.handles.end() || a->second.pooled == pooled)
        return;
    if (pooled)
        d->second.stats.pooled_texture_bytes += a->second.bytes;
    else
        d->second.stats.pooled_texture_bytes -= a->second.bytes;
    a->second.pooled = pooled;
}
SDL_GPUTexture* tracked_create_texture(SDL_GPUDevice* d, const SDL_GPUTextureCreateInfo* i) {
    auto* handle = SDL_CreateGPUTexture(d, i);
    try {
        created(d, handle, texture_payload_bytes(*i), Kind::Texture);
    } catch (...) {
        if (handle)
            SDL_ReleaseGPUTexture(d, handle);
        throw;
    }
    return handle;
}
SDL_GPUBuffer* tracked_create_buffer(SDL_GPUDevice* d, const SDL_GPUBufferCreateInfo* i) {
    auto* handle = SDL_CreateGPUBuffer(d, i);
    try {
        created(d, handle, i->size, Kind::Buffer);
    } catch (...) {
        if (handle)
            SDL_ReleaseGPUBuffer(d, handle);
        throw;
    }
    return handle;
}
SDL_GPUTransferBuffer* tracked_create_transfer(SDL_GPUDevice* d, const SDL_GPUTransferBufferCreateInfo* i) {
    auto* handle = SDL_CreateGPUTransferBuffer(d, i);
    try {
        created(d, handle, i->size, Kind::Transfer);
    } catch (...) {
        if (handle)
            SDL_ReleaseGPUTransferBuffer(d, handle);
        throw;
    }
    return handle;
}
void tracked_release_texture(SDL_GPUDevice* d, SDL_GPUTexture* h) {
    released(d, h);
    SDL_ReleaseGPUTexture(d, h);
}
void tracked_release_buffer(SDL_GPUDevice* d, SDL_GPUBuffer* h) {
    released(d, h);
    SDL_ReleaseGPUBuffer(d, h);
}
void tracked_release_transfer(SDL_GPUDevice* d, SDL_GPUTransferBuffer* h) {
    released(d, h);
    SDL_ReleaseGPUTransferBuffer(d, h);
}
} // namespace kin::gpu
