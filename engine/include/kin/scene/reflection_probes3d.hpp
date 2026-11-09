#pragma once
// A world's reflection probes (the ReflectionProbe3D component, kin/ecs/render3d.hpp): captured when they need it and
// placed in the scene's lights (Lights3D::reflection_probes), each frame, after the scene is composed and before the
// pictures that read it (SceneSession3D does it; a game that renders scene() itself runs one).
#include <kin/ecs/world.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <memory>

namespace kin {

class ReflectionProbes3D {
public:
    explicit ReflectionProbes3D(Renderer3D& renderer);
    ~ReflectionProbes3D();
    ReflectionProbes3D(ReflectionProbes3D&&) noexcept;
    ReflectionProbes3D& operator=(ReflectionProbes3D&&) noexcept;

    // Lists `world`'s enabled probes in `scene.lights.reflection_probes` and captures those that need it (first shown,
    // moved, resized, `recapture` changed, their `interval` past by Scene3D::time, or `every_frame`). The captures are
    // six renders each of `scene` as it is (its camera put back after).
    Status3D update(EcsWorld& world, Scene3D& scene);
    // The probes the last update placed and the captures it made.
    u32 probes() const;
    u32 captures() const;

private:
    struct Data;
    std::unique_ptr<Data> _data;
};

} // namespace kin
