#pragma once

// The heightmap terrains of an ECS world (TerrainRenderer3D, kin/ecs/render3d.hpp; docs/rendering_3d.md, Terrain):
// each entity's .kinterrain loaded once (again when its file changes), placed at the entity's world position, drawn
// into a collected Scene3D for its camera, and given a static height field in a physics world. SceneSession3D does
// this for its scene; a game that collects its own scene calls update after SceneFrame3D::collect.

#include <filesystem>
#include <kin/ecs/world.hpp>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/terrain3d.hpp>
#include <memory>

namespace kin {

class World3D;

class Terrains3D {
public:
    // `content` the folder TerrainRenderer3D::terrain is relative to; `surface` kin's terrain surface
    // (create_terrain_surface), else compiled by glslc when first needed (compile_terrain_surface: development).
    Terrains3D(Renderer3D& renderer, std::filesystem::path content, SurfaceShader3D surface = {});
    ~Terrains3D();
    Terrains3D(const Terrains3D&) = delete;
    Terrains3D& operator=(const Terrains3D&) = delete;

    // Each enabled TerrainRenderer3D drawn into `scene` (for scene.camera); its collision body kept in `physics`
    // (null: none). Terrains of entities gone or disabled are dropped, their bodies destroyed.
    Status3D update(EcsWorld& world, Scene3D& scene, World3D* physics);
    // An entity's terrain as loaded (null: none yet), to sculpt from game code; after changing its heights, call
    // heights_changed so its collision follows at the next update.
    Terrain3D* terrain(EcsEntity entity);
    void heights_changed(EcsEntity entity);
    // The same for a rectangle of samples (Terrain3D::brush's, end_stroke's, undo's): its collision is updated in
    // place there, not made again.
    void heights_changed(EcsEntity entity, Terrain3D::Changed changed);
    // How many terrains are loaded.
    std::size_t size() const;

private:
    struct State;
    std::unique_ptr<State> _s;
};

} // namespace kin
