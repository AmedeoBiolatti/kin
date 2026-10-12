#pragma once

// Summit's camera (docs/3d_plan.md, P4a): presentation. It reads the game's state and the world's queries and writes nothing
// back; look enters the simulation as commands, and the rig follows the player's own look angles.
//
//     summit::CameraRig rig;
//     rig.observe(game, 0);                         // once per fixed step: the head and the look as the step left them
//     const summit::CameraView v = rig.frame(game.world(), alpha, frame_seconds);   // once per rendered frame
//
// Third person: a spring arm, a sphere cast from the head back along the view; it is pulled in at once when rock is in the way
// and eased out when it clears (so that it does not pop), kept `margin` off the rock (it can come all the way to the head).
// First person: the eye at the head, no arm.

#include "game.hpp"

#include <kin/render3d/camera3d.hpp>

namespace summit {
using namespace kin;

struct CameraSettings {
    bool first_person = false;
    f32 distance = 4.2f;      // the arm's length in the open (m)
    f32 shoulder = 0.35f;     // the pivot is this far to the right of the head (m)
    f32 probe_radius = 0.22f; // the sphere cast along the arm
    f32 margin = 0.12f;       // kept off the rock
    f32 ease_out = 5.0f;      // m/s the arm lengthens at when the way clears
    f32 fov = 66.0f;
    // Hanging from the rock the view swings round to this side and down a little (degrees), so that the face shows as relief
    // and not as a wall in front of the lens; it eases in and out at `swing_rate` degrees a second.
    f32 hang_yaw = 32.0f;
    f32 hang_pitch = -8.0f;
    f32 swing_rate = 60.0f;
    f32 eye_height = 1.62f;   // first person, above the feet
    f32 z_near = 0.12f;
};

struct CameraView {
    Vec3f position{};
    Quatf rotation{};
    f32 fov = 66.0f;
    f32 arm = 0.0f;           // the arm's length now
    bool obstructed = false;  // the arm was shortened by rock this frame
    Camera3D camera() const {
        Camera3D c;
        c.position = position;
        c.rotation = rotation;
        c.fov = fov;
        return c;
    }
};

class CameraRig {
public:
    CameraSettings settings;

    // Takes the player's head and look as the step left them; the one before is kept for interpolation.
    void observe(const Game& game, i32 player = 0);
    // The camera `alpha` of the way from the last step's state to the one before (0: the previous, 1: the latest), for a frame of
    // `dt` seconds (the arm eases by it). Queries `world` (static rock only).
    CameraView frame(const World3D& world, f32 alpha = 1.0f, f32 dt = fixed_step);
    // Forget the history (after a teleport or a respawn): the next frame puts the arm out at once.
    void reset();

private:
    bool _have = false;
    Vec3f _head{}, _prev_head{};
    f32 _yaw = 0.0f, _prev_yaw = 0.0f, _pitch = 0.0f, _prev_pitch = 0.0f;
    Vec3f _feet{}, _prev_feet{};
    Body3D _ignore{};
    f32 _arm = -1.0f;
    bool _hanging = false;
    f32 _swing = 0.0f; // 0 to 1: how far the hanging view has come round
};

} // namespace summit
