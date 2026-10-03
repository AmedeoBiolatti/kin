#include "gpu_texture.hpp"

namespace kin::gpu {

namespace {
// Pooled textures waiting longer than this many ticks (about frames) are released.
constexpr u64 PoolTicks = 300;
// And the pool holds at most this much, releasing its oldest past it.
constexpr u64 PoolBytes = 256ull * 1024 * 1024;
} // namespace

SDL_GPUTexture* GpuTexturePool::take(u32 width, u32 height, SDL_GPUTextureFormat format,
                                     SDL_GPUTextureUsageFlags usage) {
    const std::lock_guard lock{_mutex};
    // The newest first: the most likely to be the one released just now.
    for (std::size_t i = _entries.size(); i-- > 0;) {
        const Entry& e = _entries[i];
        if (e.width == width && e.height == height && e.format == format && e.usage == usage) {
            SDL_GPUTexture* texture = e.texture;
            _bytes -= e.bytes;
            _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(i));
            return texture;
        }
    }
    return nullptr;
}

void GpuTexturePool::give_back(SDL_GPUTexture* texture, u32 width, u32 height, SDL_GPUTextureFormat format,
                               SDL_GPUTextureUsageFlags usage, u64 bytes) {
    const std::lock_guard lock{_mutex};
    if (_closed || !*_device || bytes > PoolBytes) {
        if (*_device) {
            SDL_ReleaseGPUTexture(*_device, texture);
        }
        return;
    }
    _entries.push_back(Entry{.texture = texture, .width = width, .height = height, .format = format,
                             .usage = usage, .bytes = bytes, .since = _ticks});
    _bytes += bytes;
    while (_bytes > PoolBytes) {
        release_locked(0);
    }
}

void GpuTexturePool::tick() {
    const std::lock_guard lock{_mutex};
    ++_ticks;
    while (!_entries.empty() && _ticks - _entries.front().since > PoolTicks) {
        release_locked(0);
    }
}

void GpuTexturePool::close() {
    const std::lock_guard lock{_mutex};
    while (!_entries.empty()) {
        release_locked(_entries.size() - 1);
    }
    _closed = true;
}

void GpuTexturePool::release_locked(std::size_t index) {
    if (*_device) {
        SDL_ReleaseGPUTexture(*_device, _entries[index].texture);
    }
    _bytes -= _entries[index].bytes;
    _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(index));
}

GpuTexture::GpuTexture(SharedDevice device, SDL_GPUTexture* texture,
                       u32 width, u32 height, SDL_GPUTextureFormat format)
    : _device(std::move(device)), _texture(texture), _width(width), _height(height), _format(format) {}

GpuTexture::GpuTexture(SharedDevice device, SDL_GPUTexture* texture, u32 width, u32 height,
                       SDL_GPUTextureFormat format, std::shared_ptr<GpuTexturePool> pool, u64 bytes,
                       SDL_GPUTextureUsageFlags usage)
    : _device(std::move(device)), _pool(std::move(pool)), _bytes(bytes), _usage(usage), _texture(texture),
      _width(width),
      _height(height), _format(format) {}

GpuTexture::~GpuTexture() {
    release();
}

GpuTexture::GpuTexture(GpuTexture&& other) noexcept
    : _device(std::move(other._device)),
      _pool(std::move(other._pool)),
      _bytes(std::exchange(other._bytes, 0)),
      _usage(std::exchange(other._usage, 0)),
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
    _pool = std::move(other._pool);
    _bytes = std::exchange(other._bytes, 0);
    _usage = std::exchange(other._usage, 0);
    _texture = std::exchange(other._texture, nullptr);
    _width = std::exchange(other._width, 0);
    _height = std::exchange(other._height, 0);
    _format = std::exchange(other._format, SDL_GPU_TEXTUREFORMAT_INVALID);
    return *this;
}

void GpuTexture::release() {
    if (_device && *_device && _texture) {
        if (_pool) {
            _pool->give_back(_texture, _width, _height, _format, _usage, _bytes);
        } else {
            SDL_ReleaseGPUTexture(*_device, _texture);
        }
    }
    _device.reset();
    _pool.reset();
    _bytes = 0;
    _usage = 0;
    _texture = nullptr;
    _width = 0;
    _height = 0;
    _format = SDL_GPU_TEXTUREFORMAT_INVALID;
}

} // namespace kin::gpu
