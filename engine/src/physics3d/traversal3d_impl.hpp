#pragma once

// What a character keeps for grab and climb (kin/physics3d/traversal3d.hpp), and the ledge search the probe and the
// pull-up share. Internal: no Jolt header; the logic over Jolt's character is in traversal_character3d.cpp.

#include <kin/physics3d/traversal3d.hpp>
#include <kin/physics3d/world3d.hpp>

#include <array>

namespace kin::physics3d_detail {

// One limb's grip, as it is stored (and saved, hashed).
struct GripData {
    GripPhase3D phase = GripPhase3D::Free;
    GripKind3D kind = GripKind3D::Surface;
    Body3D body{};
    Vec3f local_point{};
    Vec3f local_normal{0.0f, 0.0f, 1.0f};
    f32 quality = 1.0f;
    f32 drain = 1.0f;
    f32 slip = 0.0f;
    u32 tag = 0;
    bool fixed = false;
    bool ledge = false;
    // Reaching: 0 to 1.
    f32 reach_t = 0.0f;
    // Slipping: how long, and whether Slipped has been told.
    f32 slip_time = 0.0f;
    bool slipping = false;
    // Exhaustion: how long it has been exhausted and when this grip starts to fail; `fade` goes 1 to 0 over it.
    f32 exhaust_t = 0.0f;
    f32 exhaust_delay = 0.0f;
    f32 fade = 1.0f;
    // The character let go (not the command): the command must be released before this limb grabs again.
    bool latch = false;
};

struct TravData {
    TraversalMode3D mode = TraversalMode3D::Walking;
    std::array<GripData, max_grip_limbs3d> grips{};
    Resource3D resource{};
    TraversalInput3D input{};
    bool prev_pull = false;
    bool prev_jump_off = false;
    // The swing (world offset from the hang place and its velocity), and the place the last step hung from.
    Vec3f swing{};
    Vec3f swing_velocity{};
    Vec3f hang_place{};
    bool has_hang_place = false;
    f32 hang_time = 0.0f;
    // The way the character faces while hanging, across up (unit; zero before the first hang).
    Vec3f facing{};
    // The pull-up: seconds so far, the body the path is in and the path (feet: start, over the edge, standing) in its space.
    f32 mantle_time = 0.0f;
    Body3D mantle_body{};
    std::array<Vec3f, 3> mantle_path{};
    bool mantle_sprint_latch = false;
};

// A top edge found for a hand at `point` of a surface with `normal` (pointing out of the body).
struct LedgeInfo {
    bool found = false;
    // The lip: on the surface's plane at the height of the top.
    Vec3f lip{};
    // Horizontal (across `up`), out of the body: the edge's outward direction.
    Vec3f across{};
    // The top face's point under the ray, and the body it belongs to.
    Vec3f top{};
    Body3D body{};
    // Where the feet stand over it and whether a capsule fits there (set by ledge_room()).
    Vec3f stand{};
    bool clear = false;
};

// Looks for the body's top edge between `search_down` below and `search_up` above `point`: a ray down just inside the
// surface. `body` must be the one the hand is on. The top it meets first may be another body's when the held face runs up
// to within 5 cm of it and ends there (`body` in the result is the one stood on).
LedgeInfo find_ledge(const World3D& world, Body3D body, Vec3f point, Vec3f normal, Vec3f up, f32 search_up,
                     f32 search_down, Layer3DMask mask, Body3D ignore);

// Fills `stand` and `clear`: a capsule of `radius` and `height` standing `inset` beyond the edge fits (no overlap, a floor
// under it). With height 0 it is not checked (clear is true).
void ledge_room(const World3D& world, LedgeInfo& ledge, Vec3f up, f32 radius, f32 height, f32 inset, Layer3DMask mask,
                Body3D ignore);

// The provider of rope grips (rope3d.cpp): a candidate is a point of a rope near the reach, found from the world's ropes
// rather than from the casts' hits.
const GripProvider3D& rope_grip_provider();

// The rotation that takes +Y to the unit vector `up` (a capsule's own axis is +Y).
Quatf capsule_rotation(Vec3f up);

// A capsule of `radius` and `height` (feet to top; centred on its middle), kept per thread: the character's standing
// shape for clearance checks and sweeps.
Shape3D capsule_shape(f32 radius, f32 height);

} // namespace kin::physics3d_detail
