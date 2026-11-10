#include <kin/scene/scene_asset.hpp>

#include "scene_components_internal.hpp"

#include <kin/ecs/normal_sprite2d.hpp>
#include <kin/ecs/skeleton2d.hpp>
#include <kin/ecs/time.hpp>
#include <kin/pathfinding/navigation2d.hpp>
#if defined(KIN_EXPERIMENTAL_3D) && KIN_ENABLE_PHYSICS3D
#include <kin/ecs/scene3d.hpp>
#endif

namespace kin {

void register_builtin_editor_components(EcsWorld& world) {
    register_navigation2d_components(world);
    register_skeleton2d_components(world);
    register_normal_sprite2d_components(world);
#if defined(KIN_EXPERIMENTAL_3D) && KIN_ENABLE_PHYSICS3D
    register_render3d_components(world);
    register_physics3d_components(world);
#endif
    scene_detail::register_scene_render_components(world);
#if KIN_ENABLE_PHYSICS
    scene_detail::register_scene_physics2d_components(world);
#endif
    scene_detail::register_scene_particles_components(world);
    scene_detail::register_scene_gameplay_components(world);
    scene_detail::register_scene_lighting_components(world);
    scene_detail::register_scene_view_components(world);
    // kin's own (update_timers and update_lifetimes count them), so one a
    // scene or prefab file gives is one they see.
    if (!world.components().find("Timer")) {
        world.components().native<kin::Timer>("Timer")
            .field("duration", &kin::Timer::duration)
            .field("elapsed", &kin::Timer::elapsed)
            .field("repeating", &kin::Timer::repeating)
            .field("finished", &kin::Timer::finished)
            .field("ticks", &kin::Timer::ticks);
    }
    if (!world.components().find("Lifetime")) {
        world.components().native<kin::Lifetime>("Lifetime").field("remaining", &kin::Lifetime::remaining);
    }
}

} // namespace kin
