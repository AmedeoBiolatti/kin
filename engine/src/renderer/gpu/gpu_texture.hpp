#pragma once
// Ported from v0 engine/renderer_gpu. RAII wrapper over an SDL_GPUTexture.
#include <kin/core/types.hpp>

#include <SDL3/SDL.h>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace kin::gpu {

// A device handle shared by the device and every texture it creates. The device
// nulls it when destroyed, so a texture that outlives its device (for example
// one held by a static cache until exit) skips the release instead of calling
// into a destroyed device.
using SharedDevice = std::shared_ptr<SDL_GPUDevice*>;

// Released sampled textures kept for reuse, so a game that creates the same
// kind of texture frame after frame skips the driver's create and destroy. A
// reused texture's first upload replaces it whole and cycles its storage, so a
// frame still on the GPU keeps reading the old texels. Shared by the device and
// its pooled textures, which may be released on any thread.
class GpuTexturePool {
public:
    explicit GpuTexturePool(SharedDevice device) : _device(std::move(device)) {}

    // A pooled texture of this size and format, or null.
    SDL_GPUTexture* take(u32 width, u32 height, SDL_GPUTextureFormat format);
    // Keeps `texture` (of `bytes`) for take(), or releases it when the pool is
    // over its size or closed.
    void give_back(SDL_GPUTexture* texture, u32 width, u32 height, SDL_GPUTextureFormat format, u64 bytes);
    // Once a frame: releases what has waited too long.
    void tick();
    // Releases everything, and later give_backs too (the device is going).
    void close();

    u64 reused() const { return _reused; }

private:
    struct Entry {
        SDL_GPUTexture* texture = nullptr;
        u32 width = 0, height = 0;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        u64 bytes = 0;
        u64 since = 0; // tick() count when given back
    };
    void release_locked(std::size_t index);

    SharedDevice _device;
    std::mutex _mutex;
    std::vector<Entry> _entries; // oldest first
    u64 _bytes = 0;
    u64 _ticks = 0;
    u64 _reused = 0;
    bool _closed = false;
};

class GpuTexture {
public:
    GpuTexture() = default;
    GpuTexture(SharedDevice device, SDL_GPUTexture* texture,
               u32 width, u32 height, SDL_GPUTextureFormat format);
    // A texture that goes back to `pool` (as `bytes`) when released.
    GpuTexture(SharedDevice device, SDL_GPUTexture* texture, u32 width, u32 height, SDL_GPUTextureFormat format,
               std::shared_ptr<GpuTexturePool> pool, u64 bytes);
    ~GpuTexture();

    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;
    GpuTexture(GpuTexture&& other) noexcept;
    GpuTexture& operator=(GpuTexture&& other) noexcept;

    SDL_GPUTexture* handle() const { return _texture; }
    u32 width() const { return _width; }
    u32 height() const { return _height; }
    SDL_GPUTextureFormat format() const { return _format; }

    explicit operator bool() const { return _texture != nullptr; }

private:
    void release();

    SharedDevice _device;
    std::shared_ptr<GpuTexturePool> _pool;
    u64 _bytes = 0; // for the pool
    SDL_GPUTexture* _texture = nullptr;
    u32 _width = 0;
    u32 _height = 0;
    SDL_GPUTextureFormat _format = SDL_GPU_TEXTUREFORMAT_INVALID;
};

} // namespace kin::gpu
