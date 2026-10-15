#include "gpu_mesh_pool.hpp"
#include <algorithm>

namespace kin::gpu {
GpuMeshBufferPage::~GpuMeshBufferPage() {
    if (buffer && device && *device) SDL_ReleaseGPUBuffer(*device, buffer);
}
GpuMeshBufferPool::~GpuMeshBufferPool() {
    if (_device && *_device) {
        for (const auto& batch : _batches) {
            SDL_WaitForGPUFences(*_device, true, &batch.fence, 1);
            SDL_ReleaseGPUFence(*_device, batch.fence);
        }
    }
}

GpuMeshBufferSlice GpuMeshBufferPool::allocate_slice(u32 bytes, SDL_GPUBufferUsageFlags usage, u32 capacity) {
    for (const auto& page : _pages) {
        if (page->usage != usage) continue;
        for (auto it = page->free.begin(); it != page->free.end(); ++it) {
            if (it->bytes < bytes) continue;
            const u32 offset = it->offset;
            it->offset += bytes; it->bytes -= bytes;
            if (it->bytes == 0) page->free.erase(it);
            if (offset < page->high_water) ++_reused;
            page->high_water = std::max(page->high_water, offset + bytes);
            page->used += bytes;
            return {page, offset, bytes};
        }
    }
    // Keep pooling bounded; an unusually large or fragmented scene can use the existing dedicated-buffer path.
    constexpr u64 max_reservation = 512ull * 1024 * 1024;
    if (_allocated + capacity > max_reservation) return {};
    auto page = std::make_shared<GpuMeshBufferPage>(_device);
    const SDL_GPUBufferCreateInfo info{.usage = usage, .size = capacity, .props = 0};
    page->buffer = SDL_CreateGPUBuffer(*_device, &info);
    if (!page->buffer) return {};
    page->usage = usage; page->capacity = capacity; page->used = page->high_water = bytes;
    page->free.push_back({bytes, capacity - bytes});
    _allocated += capacity; _pages.push_back(page);
    return {page, 0, bytes};
}

GpuMeshBufferAllocation GpuMeshBufferPool::allocate(u32 vertex_bytes, u32 index_bytes) {
    std::lock_guard lock(_mutex);
    constexpr u32 vertex_page = 1024 * 1024, index_page = 256 * 1024;
    if (!vertex_bytes || !index_bytes || vertex_bytes > vertex_page / 2 || index_bytes > index_page / 2) return {};
    GpuMeshBufferAllocation result;
    result.vertices = allocate_slice(vertex_bytes, SDL_GPU_BUFFERUSAGE_VERTEX, vertex_page);
    if (!result.vertices.page) return {};
    result.indices = allocate_slice(index_bytes, SDL_GPU_BUFFERUSAGE_INDEX, index_page);
    if (!result.indices.page) { free_slice(result.vertices); return {}; }
    return result;
}

void GpuMeshBufferPool::free_slice(const GpuMeshBufferSlice& slice) {
    auto& page = *slice.page;
    page.used -= slice.bytes;
    auto it = std::lower_bound(page.free.begin(), page.free.end(), slice.offset,
                              [](const auto& range, u32 offset) { return range.offset < offset; });
    it = page.free.insert(it, {slice.offset, slice.bytes});
    if (it != page.free.begin() && (it - 1)->offset + (it - 1)->bytes == it->offset) {
        (it - 1)->bytes += it->bytes; it = page.free.erase(it) - 1;
    }
    if (it + 1 != page.free.end() && it->offset + it->bytes == (it + 1)->offset) {
        it->bytes += (it + 1)->bytes; page.free.erase(it + 1);
    }
}

void GpuMeshBufferPool::retire(GpuMeshBufferAllocation allocation) {
    std::lock_guard lock(_mutex);
    _retired += allocation.vertices.bytes + allocation.indices.bytes;
    _pending.push_back(std::move(allocation));
}
bool GpuMeshBufferPool::pending() const {
    std::lock_guard lock(_mutex);
    return !_pending.empty();
}
void GpuMeshBufferPool::seal(SDL_GPUFence* fence) {
    std::lock_guard lock(_mutex);
    _batches.push_back({fence, std::move(_pending)}); _pending.clear();
}
void GpuMeshBufferPool::collect() {
    std::lock_guard lock(_mutex);
    for (auto it = _batches.begin(); it != _batches.end();) {
        // Backpressure for applications that retire geometry faster than the GPU consumes it, rather than unbounded pages.
        if (_retired > 16ull * 1024 * 1024) SDL_WaitForGPUFences(*_device, true, &it->fence, 1);
        if (!SDL_QueryGPUFence(*_device, it->fence)) { ++it; continue; }
        SDL_ReleaseGPUFence(*_device, it->fence);
        for (const auto& allocation : it->allocations) {
            free_slice(allocation.vertices); free_slice(allocation.indices);
            _retired -= allocation.vertices.bytes + allocation.indices.bytes;
        }
        it = _batches.erase(it);
    }
    // No persistent empty-page cache: unloading the last mesh gives its pages back.
    std::erase_if(_pages, [&](const auto& page) {
        if (page->used) return false;
        _allocated -= page->capacity; return true;
    });
}
GpuMeshBufferPool::Stats GpuMeshBufferPool::stats() const {
    std::lock_guard lock(_mutex);
    u64 used = 0;
    for (const auto& page : _pages) used += page->used;
    return {_allocated, used - _retired, _retired, _reused, static_cast<u32>(_pages.size())};
}
} // namespace kin::gpu
