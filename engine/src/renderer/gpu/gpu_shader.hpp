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
    GpuShader(SDL_GPUDevice* device, SDL_GPUShader* shader);
    ~GpuShader();

    GpuShader(const GpuShader&) = delete;
    GpuShader& operator=(const GpuShader&) = delete;
    GpuShader(GpuShader&& other) noexcept;
    GpuShader& operator=(GpuShader&& other) noexcept;

    static GpuShader from_bytes(GpuDevice& device, SDL_GPUShaderStage stage,
                                SDL_GPUShaderFormat format,
                                std::span<const u8> bytes,
                                u32 uniform_buffers = 0,
                                u32 samplers = 0);
    static GpuShader from_file(GpuDevice& device, SDL_GPUShaderStage stage,
                               SDL_GPUShaderFormat format,
                               const std::filesystem::path& path,
                               u32 uniform_buffers = 0,
                               u32 samplers = 0);

    SDL_GPUShader* handle() const { return _shader; }
    explicit operator bool() const { return _shader != nullptr; }

private:
    void release();

    SDL_GPUDevice* _device = nullptr;
    SDL_GPUShader* _shader = nullptr;
};

std::vector<u8> read_shader_file(const std::filesystem::path& path);

} // namespace kin::gpu
