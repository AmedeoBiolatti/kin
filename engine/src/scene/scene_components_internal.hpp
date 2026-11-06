#pragma once

namespace kin {
class EcsWorld;

// Contiguous groups in the public registration sequence. Keep their call order
// in scene_components.cpp stable: component IDs and editor schemas expose it.
namespace scene_detail {
void register_scene_render_components(EcsWorld& world);
void register_scene_physics2d_components(EcsWorld& world);
void register_scene_particles_components(EcsWorld& world);
void register_scene_gameplay_components(EcsWorld& world);
void register_scene_lighting_components(EcsWorld& world);
void register_scene_view_components(EcsWorld& world);
} // namespace scene_detail
} // namespace kin
