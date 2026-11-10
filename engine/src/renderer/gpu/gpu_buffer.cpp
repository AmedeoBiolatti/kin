#include "gpu_buffer.hpp"

#include "gpu_memory.hpp"

namespace kin::gpu {

GpuBuffer::GpuBuffer(SharedDevice device, SDL_GPUBuffer* buffer, u32 size)
    : _device(std::move(device)), _buffer(buffer), _size(size) {}

GpuBuffer::~GpuBuffer() {
    release();
}

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept
    : _device(std::exchange(other._device, nullptr)), _buffer(std::exchange(other._buffer, nullptr)),
      _size(std::exchange(other._size, 0)) {}

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    _device = std::exchange(other._device, nullptr);
    _buffer = std::exchange(other._buffer, nullptr);
    _size = std::exchange(other._size, 0);
    return *this;
}

void GpuBuffer::release() {
    if (_device && *_device && _buffer) {
        tracked_release_buffer(*_device, _buffer);
    }
    _device = nullptr;
    _buffer = nullptr;
    _size = 0;
}

} // namespace kin::gpu
