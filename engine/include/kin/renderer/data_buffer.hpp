#pragma once

// A buffer of plain data for shaders (a storage buffer): an array of structs a
// material shader reads by index, for per-object parameters without packing
// them into a data texture. GLSL, after the shader's textures in set 2:
//
//     layout(std430, set = 2, binding = SAMPLERS + i) readonly buffer Casters {
//         vec4 casters[];
//     };
//
// Made by Renderer2D::create_data_buffer (capabilities().data_buffers).

#include <kin/core/types.hpp>

#include <cstddef>
#include <memory>

namespace kin {

class IDataBufferBackend {
public:
    virtual ~IDataBufferBackend() = default;
    virtual std::size_t size() const = 0;
};

class DataBuffer {
public:
    DataBuffer() = default;
    explicit DataBuffer(std::shared_ptr<IDataBufferBackend> backend) : _backend(std::move(backend)) {}

    bool valid() const { return _backend != nullptr; }
    std::size_t size() const { return _backend ? _backend->size() : 0; }
    const std::shared_ptr<IDataBufferBackend>& backend() const { return _backend; }

private:
    std::shared_ptr<IDataBufferBackend> _backend;
};

} // namespace kin
