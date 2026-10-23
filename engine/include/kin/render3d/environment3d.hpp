#pragma once

// Light from all round (image-based light) for kin's experimental 3D
// (docs/3d_plan.md, G1b): an equirectangular HDR picture of the sky or a
// room, which lights surfaces with its colours and shows in glossy ones.
// Made by Renderer3D::create_environment / load_environment; set in
// Lights3D::environment.
//
// Orientation (the same as Blender's world texture, so a scene set up in
// Blender looks alike; kin is +y up, right-handed): the picture's top row is
// +y (up), its centre is the +x direction, and going right from the centre
// turns toward +z (a quarter of the way: +z, half way: -x, then -z).
// Lights3D::environment_rotation turns it about +y.

#include <kin/core/types.hpp>
#include <memory>

namespace kin {

// The backend's copy of an environment, with its filtered versions.
class IEnvironmentBackend3D {
public:
    virtual ~IEnvironmentBackend3D() = default;
};

// An environment on the GPU; cheap to copy (copies share it). Empty where 3D
// isn't available.
class Environment3D {
public:
    Environment3D() = default;
    Environment3D(std::shared_ptr<IEnvironmentBackend3D> backend, Vec2i size)
        : _backend(std::move(backend)), _size(size) {}

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    // The picture's size in pixels.
    Vec2i size() const { return _size; }
    const std::shared_ptr<IEnvironmentBackend3D>& backend() const { return _backend; }

    friend bool operator==(const Environment3D& a, const Environment3D& b) { return a._backend == b._backend; }

private:
    std::shared_ptr<IEnvironmentBackend3D> _backend;
    Vec2i _size{};
};

} // namespace kin
