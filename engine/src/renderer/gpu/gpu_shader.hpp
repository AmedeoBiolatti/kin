#pragma once
// Ported from v0 engine/renderer_gpu. RAII wrapper over an SDL_GPUShader.
#include <kin/core/types.hpp>

#include <SDL3/SDL.h>

#include <filesystem>
#include <span>
#include <vector>

namespace kin::gpu {

class GpuDevice;

class GpuShader {
public:
    GpuShader() = default;
    GpuShader(SDL_GPUDevice* device, SDL_GPUShader* shader, u32 samplers = 0, u32 storage_buffers = 0);
    ~GpuShader();

    GpuShader(const GpuShader&) = delete;
    GpuShader& operator=(const GpuShader&) = delete;
    GpuShader(GpuShader&& other) noexcept;
    GpuShader& operator=(GpuShader&& other) noexcept;

    static GpuShader from_bytes(GpuDevice& device, SDL_GPUShaderStage stage,
                                SDL_GPUShaderFormat format,
                                std::span<const u8> bytes,
                                u32 uniform_buffers = 0,
                                u32 samplers = 0,
                                u32 storage_buffers = 0);
    static GpuShader from_file(GpuDevice& device, SDL_GPUShaderStage stage,
                               SDL_GPUShaderFormat format,
                               const std::filesystem::path& path,
                               u32 uniform_buffers = 0,
                               u32 samplers = 0);

    SDL_GPUShader* handle() const { return _shader; }
    // Fragment sampler slots the shader declares; draws must bind all of them.
    u32 samplers() const { return _samplers; }
    // Fragment storage buffer slots (after the samplers in set 2).
    u32 storage_buffers() const { return _storage_buffers; }
    explicit operator bool() const { return _shader != nullptr; }

private:
    void release();

    SDL_GPUDevice* _device = nullptr;
    SDL_GPUShader* _shader = nullptr;
    u32 _samplers = 0;
    u32 _storage_buffers = 0;
};

std::vector<u8> read_shader_file(const std::filesystem::path& path);

} // namespace kin::gpu
