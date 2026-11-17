#pragma once

// Baked lighting on the GPU (Scene3D::baked_lighting): what Renderer3D::create_baked_lighting makes of a bake
// (kin/render3d/baked_lighting3d.hpp): the lightmaps' pages, the copies' placements on them and the probes.

#include <kin/core/types.hpp>
#include <memory>

namespace kin {

// The backend's copy.
class IBakedLightingBackend3D {
public:
    virtual ~IBakedLightingBackend3D() = default;
};

// Cheap to copy (copies share it). Empty where 3D isn't available. A copy reads its lightmap by
// MeshInstance3D::lightmap: its placement in the bake (BakedLightingData3D::placements) plus one.
class BakedLighting3D {
public:
    BakedLighting3D() = default;
    BakedLighting3D(std::shared_ptr<IBakedLightingBackend3D> backend, u32 placements, bool probes)
        : _backend(std::move(backend)), _placements(placements), _probes(probes) {}

    bool valid() const { return _backend != nullptr; }
    explicit operator bool() const { return valid(); }
    u32 placements() const { return _placements; }
    bool has_probes() const { return _probes; }
    const std::shared_ptr<IBakedLightingBackend3D>& backend() const { return _backend; }

    friend bool operator==(const BakedLighting3D& a, const BakedLighting3D& b) { return a._backend == b._backend; }

private:
    std::shared_ptr<IBakedLightingBackend3D> _backend;
    u32 _placements = 0;
    bool _probes = false;
};

} // namespace kin
