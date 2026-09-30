#pragma once
// Ported from v0 engine/renderer_gpu. RAII wrapper over an SDL_GPUTexture.
#include <kin/core/types.hpp>

#include <SDL3/SDL.h>

#include <memory>
#include <utility>

namespace kin::gpu {

// A device handle shared by the device and every texture it creates. The device
// nulls it when destroyed, so a texture that outlives its device (for example
// one held by a static cache until exit) skips the release instead of calling
// into a destroyed device.
using SharedDevice = std::shared_ptr<SDL_GPUDevice*>;

class GpuTexture {
public:
    GpuTexture() = default;
    GpuTexture(SharedDevice device, SDL_GPUTexture* texture,
               u32 width, u32 height, SDL_GPUTextureFormat format);
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
    SDL_GPUTexture* _texture = nullptr;
    u32 _width = 0;
    u32 _height = 0;
    SDL_GPUTextureFormat _format = SDL_GPU_TEXTUREFORMAT_INVALID;
};

} // namespace kin::gpu
