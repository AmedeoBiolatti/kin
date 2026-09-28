#include "gpu_shader.hpp"

#include "gpu_device.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kin::gpu {

namespace {

std::runtime_error sdl_error(std::string_view prefix) {
    std::string message{prefix};
    message += ": ";
    message += SDL_GetError();
    return std::runtime_error(message);
}

} // namespace

GpuShader::GpuShader(SDL_GPUDevice* device, SDL_GPUShader* shader, u32 samplers)
    : _device(device), _shader(shader), _samplers(samplers) {}

GpuShader::~GpuShader() {
    release();
}

GpuShader::GpuShader(GpuShader&& other) noexcept
    : _device(std::exchange(other._device, nullptr)),
      _shader(std::exchange(other._shader, nullptr)),
      _samplers(std::exchange(other._samplers, 0u)) {}

GpuShader& GpuShader::operator=(GpuShader&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    _device = std::exchange(other._device, nullptr);
    _shader = std::exchange(other._shader, nullptr);
    _samplers = std::exchange(other._samplers, 0u);
    return *this;
}

GpuShader GpuShader::from_bytes(GpuDevice& device, SDL_GPUShaderStage stage,
                                SDL_GPUShaderFormat format,
                                std::span<const u8> bytes,
                                u32 uniform_buffers,
                                u32 samplers) {
    if (bytes.empty()) {
        throw std::runtime_error("GpuShader::from_bytes failed: bytecode is empty");
    }

    SDL_GPUShaderCreateInfo info{};
    info.code = bytes.data();
    info.code_size = bytes.size();
    info.entrypoint = "main";
    info.format = format;
    info.stage = stage;
    info.num_uniform_buffers = uniform_buffers;
    info.num_samplers = samplers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device.handle(), &info);
    if (!shader) {
        throw sdl_error("SDL_CreateGPUShader failed");
    }
    return GpuShader{device.handle(), shader, samplers};
}

GpuShader GpuShader::from_file(GpuDevice& device, SDL_GPUShaderStage stage,
                               SDL_GPUShaderFormat format,
                               const std::filesystem::path& path,
                               u32 uniform_buffers,
                               u32 samplers) {
    const std::vector<u8> bytes = read_shader_file(path);
    return from_bytes(device, stage, format, bytes, uniform_buffers, samplers);
}

void GpuShader::release() {
    if (_device && _shader) {
        SDL_ReleaseGPUShader(_device, _shader);
    }
    _device = nullptr;
    _shader = nullptr;
    _samplers = 0;
}

std::vector<u8> read_shader_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open shader file: " + path.string());
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Shader file is empty: " + path.string());
    }
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Failed to read shader file: " + path.string());
    }
    return bytes;
}

} // namespace kin::gpu
