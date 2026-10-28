#pragma once

// A camera as an entity: the world drawn as it sees it. It looks at where its
// entity is (a camera that is a child of the player follows the player),
// turned as its entity is, `zoom` times as big. EcsScene draws through the
// first `current` one it finds (unless its config's camera_callback says
// otherwise), its viewport the size the game draws at:
//
//     camera.set(kin::Transform2D{.pos = {640, 360}});
//     camera.set(kin::CameraComponent{.zoom = 2.0f});
//     // or one under the player, catching up with it in a fifth of a second
//     follow.child_of(player).set(kin::CameraComponent{.smoothing = 0.2f});
//     kin::update_cameras(world, dt); // each frame, for its smoothing
//
// With `limited` it never shows the world past limit_min..limit_max (a
// level's edges): it stops there as its entity goes on.

#include <kin/ecs/world.hpp>
#include <kin/renderer/render_view.hpp>

#include <optional>

namespace kin {

struct CameraComponent {
    bool current = true; // the one drawn through: the first current one
    f32 zoom = 1.0f;     // 2: the world twice as big
    bool limited = false;
    Vec2f limit_min{};   // the world's top-left corner it shows no further than
    Vec2f limit_max{};   // and its bottom-right
    f32 smoothing = 0.0f; // seconds it takes to catch its entity up (about); 0: none

    // Where it looks while smoothing (kept by update_cameras()).
    Vec2f at{};
    bool placed = false;
};

// The camera an entity with a CameraComponent is, for a viewport of `viewport`
// (the size the game draws at).
Camera2D camera_of(const EcsEntity& entity, Vec2f viewport);
// The first current camera's; none when there is none.
std::optional<Camera2D> scene_camera(EcsWorld& world, Vec2f viewport);
// The world's cameras that smooth moved on by `dt` towards their entities.
// One on an entity made from a prefab gets its own copy first.
void update_cameras(EcsWorld& world, f32 dt);

} // namespace kin
