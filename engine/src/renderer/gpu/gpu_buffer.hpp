#pragma once
// Ported from v0 engine/renderer_gpu. RAII wrapper over an SDL_GPUBuffer.
#include <kin/core/types.hpp>

#include <SDL3/SDL.h>

#include <utility>

namespace kin::gpu {

class GpuBuffer {
public:
    GpuBuffer() = default;
    GpuBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, u32 size);
    ~GpuBuffer();

    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;

    SDL_GPUBuffer* handle() const { return _buffer; }
    u32 size() const { return _size; }

    explicit operator bool() const { return _buffer != nullptr; }

private:
    void release();

    SDL_GPUDevice* _device = nullptr;
    SDL_GPUBuffer* _buffer = nullptr;
    u32 _size = 0;
};

} // namespace kin::gpu
