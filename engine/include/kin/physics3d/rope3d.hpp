#pragma once

// Ropes for kin/physics3d (experimental, docs/physics_3d.md "Ropes"): a chain of capsule bodies joined by ball joints
// (point constraints), between two anchors or hanging from one. A rope is made of ordinary bodies and constraints, so that
// grips, loads, queries and events already understand it; the extras here are its length, its tension, its points and a grip
// provider that lets a character hang from it and climb.
//
//     kin::RopeDesc3D desc;
//     desc.start = kin::RopeAnchor3D::at({0, 5, 0});          // fixed in the world
//     desc.end = kin::RopeAnchor3D::free();                    // hangs
//     desc.length = 4.0f;
//     const kin::RopeHandle3D rope = world.create_rope(desc);
//     ...
//     for (const kin::RopePoint3D& p : world.rope_points(rope)) { /* a polyline, with the tangent at each point */ }
//     world.reel_rope(rope, 2.0f, 0.5f);                       // shorten to 2 m at 0.5 m/s
//     const f32 pull = world.rope_tension(rope, kin::RopeEnd3D::Start);
//
// Why ball joints: a chain of capsules with point constraints is the most stable arrangement Jolt offers at the mass ratios
// of a climber on a rope (80 kg on 0.25 kg segments); cone or hinge limits would add bend stiffness but cost iterations and
// cannot be re-anchored in place, which pay-out and reel need. The rope's joints ask the solver for many iterations
// (RopeDesc3D::velocity_steps: Jolt's own 10 let 80 kg stretch a 4 m rope by more than a quarter); a segment count of 8
// or sub-stepping the world (World3DConfig::collision_steps) help the same way.
//
// Length. A rope has a fixed number of segments; its rest length is changed by stretching them all alike (their capsules and
// the joints' anchors; the chain is re-laid along its own polyline, so nothing jumps). `reel_rope` moves it toward a target at
// a speed, a step at a time. Mass per metre is for the length it was made at.
//
// Determinism: made in segment order; its rest length and target are in World3D::state_hash(), save_state() and
// restore_state(); nothing depends on the number of workers.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/physics3d/handles3d.hpp>

#include <vector>

namespace kin {

struct RopeHandle3D {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(RopeHandle3D, RopeHandle3D) = default;
};

enum class RopeEnd3D : u8 { Start, End };

// What one end of a rope is fixed to.
struct RopeAnchor3D {
    enum class Kind : u8 {
        Free,  // nothing: the end hangs (a rope with both ends free is a falling rope)
        World, // a point of the world
        Body,  // a point of a body, given in that body's own space (it moves with it)
    };
    Kind kind = Kind::Free;
    // World: the point. Body: the offset in the body's space. Free: where the end starts (used for the first end only).
    Vec3f point{};
    Body3D body{};

    static RopeAnchor3D free(Vec3f start = {}) { return {Kind::Free, start, {}}; }
    static RopeAnchor3D at(Vec3f world_point) { return {Kind::World, world_point, {}}; }
    static RopeAnchor3D on(Body3D body, Vec3f offset = {}) { return {Kind::Body, offset, body}; }
};

struct RopeDesc3D {
    RopeAnchor3D start;
    RopeAnchor3D end;
    // Rest length (m). 0: the distance between two anchored ends (taut). A rope longer than the distance sags. A free end
    // needs a length.
    f32 length = 0.0f;
    // Segments: `segments` when above 0, else length / segment_length rounded (at least 1).
    u32 segments = 0;
    f32 segment_length = 0.25f;
    f32 radius = 0.04f;
    f32 mass_per_metre = 1.0f;
    f32 friction = 0.6f;
    f32 linear_damping = 0.02f;
    f32 angular_damping = 0.6f;
    // Bends how the chain lies at the start when it is longer than the gap: the direction it sags (default: with gravity).
    Vec3f sag_direction{};
    // Where a free end hangs from the start: along this (default: with gravity).
    Vec3f hang_direction{};
    u8 layer = layers3d::Dynamic;
    // Collides with static bodies of the world (false: the rope passes through level geometry but still meets bodies
    // that are not static: a bridge's box).
    bool collide_world = true;
    // Segments that are not neighbours collide with each other (a coil). Off: a rope's segments never meet each other.
    bool self_collision = false;
    bool continuous = false;
    // Solver iterations for the rope's island (0: velocity 128 and position 32). An 80 kg climber on 16 light segments stretches the
    // rope by about 2.5 % at that (4.7 % at 64 and 16, a sixth at 24 and 4, a quarter at the world's 10 and 2); a rope that carries
    // nothing is fine with 16 and 4, which costs a fifth of the default (0.04 against 0.22 ms a step for 20 segments).
    u32 velocity_steps = 0;
    u32 position_steps = 0;
    // A joint or anchor that takes more than this force (N) breaks (0: never): a RopeEvent3D is made.
    f32 break_tension = 0.0f;
    u64 user_id = 0;
};

// A point of the rope's polyline: a joint between segments (segments + 1 of them), with the direction the rope runs in there
// (unit, from the start toward the end).
struct RopePoint3D {
    Vec3f position{};
    Vec3f tangent{0.0f, -1.0f, 0.0f};
};

// A segment's frame: its end points and its body.
struct RopeSegment3D {
    Vec3f a{}, b{};
    Quatf rotation{};
    Body3D body{};
};

// The rope's closest point to a place.
struct RopeClosest3D {
    u32 segment = 0;
    // 0 to 1 along that segment, and 0 to 1 along the whole rope by length.
    f32 t = 0.0f;
    f32 along = 0.0f;
    Vec3f point{};
    Vec3f tangent{};
    f32 distance = 0.0f;
};

struct RopeState3D {
    u32 segments = 0;
    f32 rest_length = 0.0f;
    f32 target_length = 0.0f;
    // The polyline's own length as it is now (more than the rest length under load: the stretch).
    f32 current_length = 0.0f;
    bool broken = false;
};

// Which rope and segment a body is, if it is one (World3D::rope_segment_of).
struct RopeSegmentRef3D {
    RopeHandle3D rope{};
    u32 segment = 0;
    u32 segments = 0;
    f32 segment_length = 0.0f;
};

enum class RopeEventKind3D : u8 { Broke };

struct RopeEvent3D {
    RopeHandle3D rope{};
    u64 user_id = 0;
    RopeEventKind3D kind = RopeEventKind3D::Broke;
    // The joint that broke: 0 is the start anchor, 1 .. segments - 1 the joints between segments, `segments` the end anchor.
    u32 joint = 0;
    f32 force = 0.0f;
};

} // namespace kin
