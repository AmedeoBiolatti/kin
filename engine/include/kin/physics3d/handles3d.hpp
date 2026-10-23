#pragma once

// Handles and layer numbers shared by kin/physics3d's headers (world3d.hpp, character3d.hpp). Experimental:
// the API may change in any release (docs/3d_plan.md, docs/physics_3d.md).

#include <kin/core/types.hpp>

namespace kin {

enum class Body3DMotion : u8 {
    Static,    // never moves (moved by set_position it stays where put; costs nothing)
    Kinematic, // moved by the caller (velocity or move_kinematic), pushes dynamic bodies
    Dynamic,   // moved by forces, contacts and gravity
};

// A body in a World3D: an index and a generation, so that a handle to a
// destroyed body never names the body that took its place.
struct Body3D {
    u32 index = UINT32_MAX;
    u32 generation = 0;

    bool valid() const { return index != UINT32_MAX; }
    explicit operator bool() const { return valid(); }
    friend constexpr bool operator==(Body3D, Body3D) = default;
};

// Collision layers: a body is in one of 32, and a world holds a 32 x 32 table of
// which pairs collide. A set of layers is a 32-bit mask, bit n for layer n, also
// what queries filter by (a ray that ignores the character's layer, ...).
using Layer3DMask = u32;

namespace layers3d {
// Layer numbers. 0 is for static bodies only (it is its own broad phase tree);
// the rest are free for any body but are named for what they are usually used for.
inline constexpr u8 Static = 0;
inline constexpr u8 Dynamic = 1;
inline constexpr u8 Kinematic = 2;
inline constexpr u8 Character = 3;
// Detects but does not collide: sensors pair with nothing else's layer by
// default (a trigger volume sits here). Events come with P2b's queries.
inline constexpr u8 Sensor = 4;
inline constexpr u8 User0 = 5; // 5 to 31 are the game's
inline constexpr u8 Count = 32;
// "The layer that goes with the motion type": Static, Kinematic or Dynamic.
inline constexpr u8 Auto = 255;
} // namespace layers3d

constexpr Layer3DMask layer_bit(u8 layer) { return layer < 32 ? (Layer3DMask{1} << layer) : 0; }
inline constexpr Layer3DMask all_layers3d = 0xFFFFFFFFu;

} // namespace kin
