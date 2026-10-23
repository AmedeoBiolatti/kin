#pragma once

// The Person rig's inverse kinematics for Sponza Walk (docs/3d_plan.md, P1b): which constraints it gets and how the
// panel's state drives them each frame. Kept out of main.cpp so that a test can drive it with no window
// (tests/skeleton_tests.cpp).
//
//     for (IKConstraint3D& k : person_constraints(ground)) animator.add_ik(k);   // once: feet, look, hand, tail, in
//     that order
//     ...each frame:  drive_person(animator, panel, base_matrix, camera_position);
//
// The order is the one the pose wants: the feet lower the pelvis, the look turns the spine, the arm then reaches from
// where the shoulder has got to (a constraint moves what hangs below its joints), and the tail, which hangs off the
// pelvis, last.
#include "character.hpp"
#include "settings.hpp"

#include <kin/ecs/render3d.hpp>
#include <vector>

namespace sponza_walk {
using namespace kin;

// What the panel's "Character IK" section says (the subset of Settings the rig reads), in the person's own axes: x to
// its left, y up, z forward, metres, from the foot of the person as it was placed.
struct PersonIk {
    bool on = false;
    float hand_x = 0.45f, hand_y = 0.1f, hand_z = 0.45f;
    int pole = 1;         // where the elbow points: 0 as animated, 1 down, 2 back, 3 up, 4 out
    float weight = 1.0f;  // the hand's goal weight
    float fade = 0.2f;    // seconds a weight takes to change
    float stretch = 1.0f; // the arm may lengthen to this factor
    bool look = false;
    float look_weight = 1.0f;
    float look_smooth = 0.15f;
    bool feet = false;
    bool step = true;
    float step_height = 0.2f;
    bool tail = false; // a FABRIK chain on the tail, to the target below
    float tail_x = 0.5f, tail_y = 0.9f, tail_z = -0.35f;
    bool limits = false; // the knees are hinges with a twist limit at the hips, the elbow folds to 150 degrees
    bool lock = false;   // a planted foot stays where it was put in the world
    bool toes = false;   // the toes lie on the ground under them
    bool walk = false;   // the person drifts from side to side (to see feet lock)
};

// The panel's section of Settings, as the rig reads it.
inline PersonIk person_ik(const Settings& s) {
    PersonIk p;
    p.on = s.ik;
    p.hand_x = s.ik_hand_x;
    p.hand_y = s.ik_hand_y;
    p.hand_z = s.ik_hand_z;
    p.pole = s.ik_pole;
    p.weight = s.ik_weight;
    p.fade = s.ik_fade;
    p.stretch = s.ik_stretch;
    p.look = s.ik_look;
    p.look_weight = s.ik_look_weight;
    p.look_smooth = s.ik_look_smooth;
    p.feet = s.ik_feet;
    p.step = s.ik_step;
    p.step_height = s.ik_step_height;
    p.tail = s.ik_tail;
    p.tail_x = s.ik_tail_x;
    p.tail_y = s.ik_tail_y;
    p.tail_z = s.ik_tail_z;
    p.limits = s.ik_limits;
    p.lock = s.ik_lock;
    p.toes = s.ik_toes;
    p.walk = s.ik_walk;
    return p;
}

// Where the elbow points for the panel's choice (`pole` 1 to 4), in the person's own axes.
inline Vec3f person_elbow_pole(int pole) {
    static const Vec3f poles[] = {{0, 0, 0}, {0, -1, 0}, {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};
    return poles[std::clamp(pole, 0, 4)];
}

// The hand's and the tail's targets, in the person's own space (the hand from the left shoulder's rest place).
inline Vec3f person_hand_target(const PersonIk& s) {
    return {0.2f + s.hand_x, 1.65f + s.hand_y, s.hand_z};
}
inline Vec3f person_tail_target(const PersonIk& s) {
    return {s.tail_x, s.tail_y, s.tail_z};
}

// How far the person has drifted from where it was placed (along its own x), seconds into the walk.
inline float person_walk_offset(float seconds) {
    return 0.7f * std::sin(seconds * 0.9f);
}

// The constraints of the Person, in order. `ground` is in the world (the foot placer's query).
inline std::vector<IKConstraint3D> person_constraints(GroundQuery3D ground) {
    std::vector<IKConstraint3D> out;
    IKConstraint3D feet;
    feet.name = "feet";
    feet.kind = IKKind3D::FootPlacer;
    feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
    feet.toe_joints = {"toe_L", "toe_R"};
    feet.foot_heights = {0.05f, 0.05f};
    feet.toe_heights = {0.03f, 0.03f};
    feet.pole = IKConstraint3D::Pole::ModelDirection;
    feet.pole_vector = {0.0f, 0.0f, 1.0f};
    feet.two_bone.max_stretch = 1.1f;
    feet.foot.smooth_seconds = 0.08f;
    feet.foot.toe_max_tilt_degrees = 0.0f; // set by drive_person
    feet.ground = std::move(ground);
    out.push_back(feet);

    IKConstraint3D look;
    look.name = "look";
    look.kind = IKKind3D::LookAt;
    look.joints = {"spine", "neck", "head"};
    look.look_links = {
        {.weight = 0.2f, .max_yaw_degrees = 15.0f, .max_pitch_up_degrees = 10.0f, .max_pitch_down_degrees = 10.0f},
        {.weight = 0.3f, .max_yaw_degrees = 35.0f, .max_pitch_up_degrees = 20.0f, .max_pitch_down_degrees = 20.0f},
        {.weight = 0.5f, .max_yaw_degrees = 60.0f, .max_pitch_up_degrees = 35.0f, .max_pitch_down_degrees = 35.0f}};
    out.push_back(look);

    IKConstraint3D hand;
    hand.name = "hand";
    hand.kind = IKKind3D::TwoBone;
    hand.joints = {"shoulder_L", "elbow_L", "hand_L"};
    out.push_back(hand);

    IKConstraint3D tail;
    tail.name = "tail";
    tail.kind = IKKind3D::Fabrik;
    tail.joints = {"tail_1", "tail_2", "tail_3", "tail_4", "tail_5"};
    tail.weight = 0.0f;
    out.push_back(tail);
    return out;
}

// One frame's drive of the person's constraints from the panel. `base` is where the person was placed (the targets are
// in its space and stay where they are while it walks); `camera` is the point the head looks at.
inline void drive_person(Animator3D& a, const PersonIk& s, const Mat4f& base, Vec3f camera) {
    IKConstraint3D& hand = a.ik("hand");
    hand.set_target(base.apply(person_hand_target(s))).set_weight(s.weight);
    hand.fade_seconds = s.fade;
    hand.two_bone.max_stretch = s.stretch;
    hand.two_bone.max_bend_degrees = s.limits ? 150.0f : 180.0f;
    if (std::clamp(s.pole, 0, 4) == 0) {
        hand.pole = IKConstraint3D::Pole::None;
    } else {
        hand.set_pole_direction(person_elbow_pole(s.pole));
    }
    IKConstraint3D& look = a.ik("look");
    look.set_target(camera).set_weight(s.look ? s.look_weight : 0.0f);
    look.fade_seconds = s.fade;
    look.look_at.smooth_seconds = s.look_smooth;
    IKConstraint3D& feet = a.ik("feet");
    feet.set_weight(s.feet ? 1.0f : 0.0f);
    feet.fade_seconds = s.fade;
    feet.foot.lock_feet = s.lock;
    feet.foot.toe_max_tilt_degrees = s.toes ? 40.0f : 0.0f;
    // The knees bend one way (about the leg's own X, forward), the hip may turn 35 degrees about the thigh to put the
    // knee where the pole wants it, and the leg folds to 150 degrees and no more.
    feet.two_bone.hinge = s.limits;
    feet.two_bone.hinge_axis = {1.0f, 0.0f, 0.0f};
    feet.two_bone.max_twist_degrees = s.limits ? 35.0f : 180.0f;
    feet.two_bone.max_bend_degrees = s.limits ? 150.0f : 180.0f;
    IKConstraint3D& tail = a.ik("tail");
    tail.set_target(base.apply(person_tail_target(s))).set_weight(s.tail ? 1.0f : 0.0f);
    tail.fade_seconds = s.fade;
}

} // namespace sponza_walk
