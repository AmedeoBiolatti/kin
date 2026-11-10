#pragma once

#include "gpu_texture.hpp"

#include <SDL3/SDL.h>
#include <memory>
#include <mutex>
#include <vector>

namespace kin::gpu {

struct GpuMeshBufferPage {
    struct Range {
        u32 offset, bytes;
    };
    explicit GpuMeshBufferPage(SharedDevice device) : device(std::move(device)) {}
    ~GpuMeshBufferPage();
    SharedDevice device;
    SDL_GPUBuffer* buffer = nullptr;
    SDL_GPUBufferUsageFlags usage = 0;
    u32 capacity = 0, used = 0, high_water = 0;
    std::vector<Range> free;
};

struct GpuMeshBufferSlice {
    std::shared_ptr<GpuMeshBufferPage> page;
    u32 offset = 0, bytes = 0;
};
struct GpuMeshBufferAllocation {
    GpuMeshBufferSlice vertices, indices;
    explicit operator bool() const { return vertices.page && indices.page; }
};

// Small static meshes keep local indices and bind their own offsets into shared pages. Ranges are reusable only after
// a fence covering their last recorded draws and uploads has signalled. The pool can outlive the renderer with its
// meshes.
class GpuMeshBufferPool {
public:
    // `extra_usage` is added to every page's (the GI scene's compute reads meshes:
    // SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ).
    explicit GpuMeshBufferPool(SharedDevice device, SDL_GPUBufferUsageFlags extra_usage = 0)
        : _device(std::move(device)), _extra_usage(extra_usage) {}
    ~GpuMeshBufferPool();
    GpuMeshBufferAllocation allocate(u32 vertex_bytes, u32 index_bytes);
    void retire(GpuMeshBufferAllocation allocation);
    bool pending() const;
    void seal(SDL_GPUFence* fence);
    void collect();
    struct Stats {
        u64 allocated = 0, live = 0, retired = 0, reused = 0;
        u32 pages = 0;
    };
    Stats stats() const;

private:
    struct Batch {
        SDL_GPUFence* fence;
        std::vector<GpuMeshBufferAllocation> allocations;
    };
    GpuMeshBufferSlice allocate_slice(u32 bytes, SDL_GPUBufferUsageFlags usage, u32 capacity);
    void free_slice(const GpuMeshBufferSlice& slice);
    SharedDevice _device;
    SDL_GPUBufferUsageFlags _extra_usage = 0;
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<GpuMeshBufferPage>> _pages;
    std::vector<GpuMeshBufferAllocation> _pending;
    std::vector<Batch> _batches;
    u64 _allocated = 0, _retired = 0, _reused = 0;
};
} // namespace kin::gpu
