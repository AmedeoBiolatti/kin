#include "gpu_texture.hpp"

namespace kin::gpu {

GpuTexture::GpuTexture(SharedDevice device, SDL_GPUTexture* texture,
                       u32 width, u32 height, SDL_GPUTextureFormat format)
    : _device(std::move(device)), _texture(texture), _width(width), _height(height), _format(format) {}

GpuTexture::~GpuTexture() {
    release();
}

GpuTexture::GpuTexture(GpuTexture&& other) noexcept
    : _device(std::move(other._device)),
      _texture(std::exchange(other._texture, nullptr)),
      _width(std::exchange(other._width, 0)),
      _height(std::exchange(other._height, 0)),
      _format(std::exchange(other._format, SDL_GPU_TEXTUREFORMAT_INVALID)) {}

GpuTexture& GpuTexture::operator=(GpuTexture&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    _device = std::move(other._device);
    _texture = std::exchange(other._texture, nullptr);
    _width = std::exchange(other._width, 0);
    _height = std::exchange(other._height, 0);
    _format = std::exchange(other._format, SDL_GPU_TEXTUREFORMAT_INVALID);
    return *this;
}

void GpuTexture::release() {
    if (_device && *_device && _texture) {
        SDL_ReleaseGPUTexture(*_device, _texture);
    }
    _device.reset();
    _texture = nullptr;
    _width = 0;
    _height = 0;
    _format = SDL_GPU_TEXTUREFORMAT_INVALID;
}

} // namespace kin::gpu
