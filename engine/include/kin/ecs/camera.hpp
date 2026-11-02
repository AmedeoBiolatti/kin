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
//
// Following its entity: a `dead_zone` is a box (centred on what the camera
// looks at) its entity moves about in without the camera following, which
// only moves to keep it inside; `look_ahead` looks ahead the way its entity
// moves (its motion over `look_ahead_time` seconds, as far as `look_ahead`
// along each axis); `zoom_smoothing` makes a change of zoom take a while.
//
// Shaking: shake_camera() adds trauma (0 to 1), which wears off by
// `shake_decay` a second. Shaken, the camera moves by up to `shake_strength`
// points and turns by up to `shake_rotation` degrees, times the trauma
// squared (a small knock barely shows, a big one throws it about), jittering
// `shake_frequency` times a second along smooth noise (the same each run).
//
//     kin::shake_camera(world, 0.5f); // a hard landing

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
    Vec2f dead_zone{};    // the box its entity moves in without the camera following; 0: none
    Vec2f look_ahead{};   // how far ahead it looks the way its entity moves, along each axis; 0: none
    f32 look_ahead_time = 0.4f; // seconds of its entity's motion it looks ahead (no further than look_ahead)
    f32 zoom_smoothing = 0.0f;  // seconds a change of zoom takes (about); 0: at once
    f32 shake_strength = 16.0f; // points it moves by at full trauma
    f32 shake_rotation = 0.0f;  // degrees it turns by at full trauma (turned, rectangles' edges show steps)
    f32 shake_frequency = 18.0f; // jitters a second
    f32 shake_decay = 1.5f;     // trauma worn off a second

    // Kept by update_cameras(): where it looks (smoothed), and the point its
    // dead zone is around; how far ahead it looks now, and where its entity was.
    Vec2f at{};
    bool placed = false;
    Vec2f focus{};
    Vec2f lead{};
    Vec2f last_target{};
    f32 shown_zoom = 0.0f; // the zoom it shows while zoom_smoothing (0: not yet)
    // Its shake: trauma (0 to 1, shake_camera()), and how it is moved and turned now.
    f32 trauma = 0.0f;
    f64 shake_time = 0.0;
    Vec2f shake_offset{};
    f32 shake_angle = 0.0f;
};

// The camera an entity with a CameraComponent is, for a viewport of `viewport`
// (the size the game draws at).
Camera2D camera_of(const EcsEntity& entity, Vec2f viewport);
// The first current camera's; none when there is none.
std::optional<Camera2D> scene_camera(EcsWorld& world, Vec2f viewport);
// The world's cameras moved on by `dt`: following their entities (smoothing,
// dead zone, look-ahead), zooming, shaking. One on an entity made from a
// prefab gets its own copy first.
void update_cameras(EcsWorld& world, f32 dt);

// Trauma added to the world's current cameras (to one camera's), up to 1.
void shake_camera(EcsWorld& world, f32 trauma);
void shake_camera(EcsEntity camera, f32 trauma);

} // namespace kin
