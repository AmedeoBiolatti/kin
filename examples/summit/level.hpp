#pragma once

// Summit's mountain (docs/3d_plan.md, P4): a procedural level made from a seed, as plain data. One list of `Piece`s (oriented
// boxes with what a hand makes of their surface) is the whole geometry: `Game` makes a static body of each (the collider), and
// `build_render_meshes` makes the triangles you see from the same list, so what is drawn is what is hit. Nothing here needs a
// window, a GPU or a physics world: the level is generated, hashed and checked on the CPU.
//
//     const summit::SummitLevel level = summit::generate_level(seed);
//     level.hash();                                  // the same seed gives the same bytes
//     const summit::RoutePlan plan = summit::plan_route(level, summit::PlayerRules{});   // is it climbable?
//
// The shape: a stack of cliffs and terraces rising along a wandering heading from base camp to the summit (about 150 m). A stage
// is an approach (a ramp that can be walked) and an obstacle (a pitch to climb, a chimney, a roof to traverse under, a rope).
// `route` lists the legs a bot (script.hpp) follows and the planner costs; the player is free to leave it.

#include <kin/core/math3d.hpp>
#include <kin/core/types.hpp>
#include <kin/render3d/mesh.hpp>
#include <kin/render3d/scene3d.hpp>

#include <string>
#include <vector>

namespace summit {
using namespace kin;

inline constexpr f32 pi = 3.14159265358979323846f;

enum class PieceKind : u8 {
    Ground,  // the field at base camp, terraces
    Ramp,    // a walkable approach (a tilted box)
    Cliff,   // a pitch's face and mass: climbed
    Roof,    // an overhang slab: nothing can be climbed through it
    Smooth,  // a face no hand can take hold of (holds on it are pieces of their own)
    Hold,    // an authored hold
    Lip,     // the grippable edge on top of a smooth wall
    Boulder, // decoration with a collider
    Post,    // a rope's anchor beam
};

// What a hand finds. `grippable` false: no surface grip. `hold`: an authored hold (HoldDesc3D) of `hold_quality`.
struct Piece {
    PieceKind kind = PieceKind::Ground;
    Vec3f centre{};
    Vec3f half{1.0f, 1.0f, 1.0f};
    Quatf rotation{};
    bool grippable = true;
    f32 grip = 1.0f;
    f32 slip = 0.0f;
    f32 drain = 1.0f;
    u32 tag = 0;
    bool hold = false;
    f32 hold_quality = 1.0f;
    f32 friction = 0.8f;
};

struct Camp {
    i32 index = 0;
    Vec3f spawn{};   // where a respawn puts the feet
    f32 radius = 4.0f;
    f32 altitude = 0.0f;
    Vec3f marker{}; // where the tent and flag stand (beside the spawn, out of the way)
};

struct RopeSpec {
    Vec3f anchor{}; // fixed in the world, at the top
    f32 length = 6.0f;
    // The way the wall faces (out of the rock), for the bot's hand-over at the top.
    Vec3f wall_normal{0.0f, 0.0f, 1.0f};
};

enum class LegKind : u8 {
    Walk,     // walk to `to` (a continuous run of ground)
    Climb,    // stand at `from`, face `-normal` (the wall), climb `height`, pull up, arrive at `to`
    Overhang, // as Climb, but a roof stops the climb at `roof_height`: shimmy `shimmy` m (along `right`) then climb on
    Rope,     // climb rope `rope` from `from`, then hand over to the lip and pull up to `to`
    Rest,     // recover stamina here until `height` (fraction) is back
    Summit,   // the end: stand in the summit box
};

struct Leg {
    LegKind kind = LegKind::Walk;
    Vec3f from{};
    Vec3f to{};
    Vec3f normal{0.0f, 0.0f, 1.0f}; // out of the wall, horizontal
    f32 height = 0.0f;              // the pitch (m), or for Rest the share of stamina wanted
    f32 roof_height = 0.0f;         // Overhang: how far up the roof is (m above `from`)
    f32 shimmy = 0.0f;              // Overhang: metres along +right (negative: left)
    i32 rope = -1;
    i32 camp = -1;                  // Walk: the camp this leg ends at (-1 none)
    std::string label;
};

struct SummitLevel {
    u64 seed = 0;
    std::vector<Piece> pieces;
    std::vector<Camp> camps;
    std::vector<RopeSpec> ropes;
    std::vector<Leg> route;
    Vec3f start{};          // base camp's spawn (camp 0)
    Vec3f summit{};         // the top (feet)
    f32 summit_radius = 3.0f;
    f32 kill_y = -40.0f;    // below it a body is lost
    Bounds3 bounds{};

    // The level as bytes in a fixed order (floats bit for bit), its FNV-1a hash, and the same of the render meshes.
    std::vector<u8> bytes() const;
    u64 hash() const;
    // Altitude gained from base camp to the summit.
    f32 height() const { return summit.y - start.y; }
};

SummitLevel generate_level(u64 seed);

// What the planner counts with: the numbers of the player model, in one place (game.hpp's PlayerRules is these).
struct ClimbCosts {
    f32 max_stamina = 150.0f;
    f32 climb_speed = 0.8f;
    f32 shimmy_speed = 1.0f;
    f32 drain_two_hands = 7.0f;
    f32 drain_moving = 4.0f;
    f32 grab_cost = 2.0f;
    f32 pull_up_cost = 8.0f;
    f32 pull_up_min = 8.0f;
    f32 regen_rate = 25.0f;
    f32 regen_delay = 1.0f;
    f32 rope_speed = 0.6f;
    f32 walk_speed = 4.0f;
    // Where the hands first take hold (m above the feet: the head's height), and the seconds of settling a hang takes.
    f32 first_hold_height = 2.0f;
    f32 settle_seconds = 0.2f;
};

struct LegPlan {
    LegKind kind = LegKind::Walk;
    f32 seconds = 0.0f;       // game time the leg takes (a walk's, a climb's, a rest's)
    f32 cost = 0.0f;          // stamina units spent
    f32 start_stamina = 0.0f; // wanted when it begins (before a rest)
    f32 lowest = 0.0f;        // the lowest stamina along it
    bool feasible = true;     // false: it cannot be done from a full bar
};

struct RoutePlan {
    std::vector<LegPlan> legs;
    f32 total_seconds = 0.0f;
    f32 worst_cost = 0.0f;       // the dearest leg
    f32 least_margin = 0.0f;     // the smallest (max_stamina - cost) of a climb leg
    bool climbable = true;
};

// The route costed with the stamina model, resting to full before every climb leg and on every camp: each leg must be possible
// from a full bar, so that a player who rests where the route says never runs dry.
RoutePlan plan_route(const SummitLevel& level, const ClimbCosts& costs = {});

// Triangles from the pieces: one mesh a material class, vertices coloured by kind and a hash of the piece (no randomness).
struct RenderMeshData {
    std::string name;
    MeshData mesh;
    Material3D material;
};
std::vector<RenderMeshData> build_render_meshes(const SummitLevel& level);

} // namespace summit
