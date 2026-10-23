#pragma once

// Summit's camera (docs/3d_plan.md, P4a): presentation. It reads the game's state and the world's queries and writes
// nothing back; look enters the simulation as commands, and the rig follows the player's own look angles.
//
//     summit::CameraRig rig;
//     rig.observe(game, 0);                         // once per fixed step: the head and the look as the step left them
//     const summit::CameraView v = rig.frame(game.world(), alpha, frame_seconds);   // once per rendered frame
//
// Third person: a spring arm, a sphere cast from the head back along the view; it is pulled in at once when rock is in
// the way and eased out when it clears (so that it does not pop), kept `margin` off the rock (it can come all the way
// to the head). First person: the eye at the head, no arm.

#include "game.hpp"

#include <kin/physics3d/spring_arm3d.hpp>
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
    // Hanging from the rock the view swings round to this side and down a little (degrees), so that the face shows as
    // relief and not as a wall in front of the lens; it eases in and out at `swing_rate` degrees a second.
    f32 hang_yaw = 32.0f;
    f32 hang_pitch = -8.0f;
    f32 swing_rate = 60.0f;
    // Lying (knocked out, dead): the pivot this far above the pelvis, the view this many degrees down, the arm this
    // long, so that the whole body is in the picture.
    f32 lie_height = 0.9f;
    f32 lie_pitch = -26.0f;
    f32 lie_distance = 5.6f;
    f32 tight_fov_boost = 16.0f; // degrees added when the arm is short (rock close behind)
    f32 eye_height = 1.62f;      // first person, above the feet
    f32 z_near = 0.12f;
};

struct CameraView {
    Vec3f position{};
    Quatf rotation{};
    f32 fov = 66.0f;
    f32 arm = 0.0f;          // the arm's length now
    bool obstructed = false; // the arm was shortened by rock this frame
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
    // The camera `alpha` of the way from the last step's state to the one before (0: the previous, 1: the latest), for
    // a frame of `dt` seconds (the arm eases by it). Queries `world` (static rock only).
    CameraView frame(const World3D& world, f32 alpha = 1.0f, f32 dt = fixed_step);
    // The turn of the view (degrees, as PlayerCommand::look: x to the right, y up) the hand has made that no step has
    // taken yet: the mouse's motion not yet read by a step, the sticks' rate for the time since the last step. Set it
    // once a frame, before frame(). It is in no step: the simulation's look is the commands'.
    void set_look_ahead(Vec2f look);
    // Where the person's feet and which way it faces, `alpha` of the way between the last two steps: the same instant
    // as frame()'s camera. A person drawn at the latest step against a camera drawn between two jumps sideways by up to
    // a step's travel every step (60 Hz, plainly seen when the view is side on to the way it goes).
    Vec3f person_position(f32 alpha) const;
    f32 person_facing(f32 alpha) const;
    // Forget the history (after a teleport or a respawn): the next frame puts the arm out at once.
    void reset();

private:
    bool _have = false;
    Vec3f _head{}, _prev_head{};
    f32 _yaw = 0.0f, _prev_yaw = 0.0f, _pitch = 0.0f, _prev_pitch = 0.0f;
    Vec3f _feet{}, _prev_feet{};
    f32 _facing = 0.0f, _prev_facing = 0.0f;
    Vec2f _look_ahead{};
    f32 _pitch_min = -80.0f, _pitch_max = 80.0f;
    Body3D _ignore{};
    SpringArm3D _spring_arm;
    bool _hanging = false;
    bool _down = false;
    bool _lie_pick = true;
    f32 _lie_offset = 0.0f; // degrees the lying view goes round to the clearest side
    f32 _lie = 0.0f;        // 0 to 1: how far the lying view has come round
    f32 _swing = 0.0f;      // 0 to 1: how far the hanging view has come round
};

} // namespace summit
