#pragma once

// The grab-and-climb test level (docs/3d_plan.md P2d), built of boxes in a World3D: the same level for the CPU tests
// (tests/physics3d_traversal_tests.cpp), the reference scene `physics_climb` (tools/reference) and the cost bench, so that
// what is measured is what is drawn.
//
// Stations along +X, each a wall whose face is the plane z = 0 looking toward +Z (a character stands at z > 0 facing -Z):
//
//   x =  0  wall: 4 m of plain rock, a ledge 1.5 m deep on top (the climb and the pull-up)
//   x = 10  overhang: a 5 m wall with a roof slab 3 m up that reaches 1.5 m out (hanging from a ceiling)
//   x = 20  holds: a smooth 3.3 m wall that cannot be gripped, four holds on it and a lip of rock standing out on top
//   x = 30  platform: a kinematic slab that moves along X, marked as a hold (grip carried by what it holds)
//   x = 40  crate: a pedestal 2 m high with a 30 kg crate half over its edge (hanging from a loose body)
//   x = 50  ice: a 7 m wall of slippery, tiring rock (a grip slides, then lets go)
//   x = 60  low ceiling: a 3 m ledge with a roof 1.4 m over it (a pull-up that cannot be made)
//
// Every piece's user id is 200 + its index.

#include <kin/core/json_value.hpp>
#include <kin/physics3d/world3d.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace climb_level {

using namespace kin;

enum class Kind : u8 { Floor, Wall, Lip, Roof, SmoothWall, Hold, Platform, Pedestal, Crate, IceWall, LowRoof, Ledge };

struct Piece {
    Kind kind = Kind::Floor;
    Vec3f centre{};
    Vec3f half{};
    Quatf rotation{};
    Body3D body{};
    u64 user_id = 0;
    bool moving = false; // kinematic or dynamic: the drawer reads its pose from the world
};

inline constexpr f32 x_wall = 0.0f, x_overhang = 10.0f, x_holds = 20.0f, x_platform = 30.0f, x_crate = 40.0f,
                     x_ice = 50.0f, x_low = 60.0f;
inline constexpr f32 wall_height = 4.0f;

struct Level {
    std::vector<Piece> pieces;
    Body3D floor{}, wall{}, roof{}, smooth_wall{}, platform{}, crate{}, pedestal{}, ice{}, low_roof{};
    std::vector<Body3D> holds;
    // The frame the level was built in (identity: as drawn above): positions of stations in the world.
    Quatf frame{};
    Vec3f origin{};
    Vec3f platform_centre{};

    // A point of the level's own coordinates in the world.
    Vec3f at(Vec3f local) const { return frame.rotate(local) + origin; }
    Vec3f direction(Vec3f local) const { return frame.rotate(local); }

    // The platform's place at step `n` (60 Hz): a triangle wave 3 m each way along X at 1.5 m/s.
    Vec3f platform_position(int n) const {
        const f32 period = 8.0f;
        const f32 t = std::fmod(static_cast<f32>(n) / 60.0f, period);
        const f32 u = t < period * 0.5f ? t : period - t; // 0 to 4 to 0 seconds
        const Vec3f local = platform_local_start + Vec3f{1.5f * u, 0.0f, 0.0f};
        return frame.rotate(local) + origin;
    }
    Vec3f platform_local_start{x_platform - 3.0f, 2.2f, 0.5f};

    // Moves the platform to where it is at step n+1 (call before World3D::step of step n).
    void drive(World3D& world, int n, f32 dt) const {
        if (platform.valid()) {
            world.move_kinematic(platform, platform_position(n + 1), frame, dt);
        }
    }
};

// Builds the level in `frame` (a rotation of the whole level about `origin`: a wall treated as a floor) at `origin`.
inline Level build(World3D& world, Quatf frame = {}, Vec3f origin = {}) {
    Level level;
    level.frame = frame;
    level.origin = origin;
    const auto add = [&](Kind kind, Vec3f centre, Vec3f half, Body3DMotion motion = Body3DMotion::Static, f32 mass = 0.0f,
                         f32 friction = 0.6f) -> Piece& {
        Piece p;
        p.kind = kind;
        p.centre = level.at(centre);
        p.half = half;
        p.rotation = frame;
        p.user_id = 200 + level.pieces.size();
        p.moving = motion != Body3DMotion::Static;
        p.body = world.create_body({.shape = Shape3D::box(half), .motion = motion, .position = p.centre, .rotation = frame,
                                    .mass = mass, .friction = friction, .user_id = p.user_id});
        level.pieces.push_back(p);
        return level.pieces.back();
    };

    level.floor = add(Kind::Floor, {30.0f, -0.5f, 0.0f}, {45.0f, 0.5f, 25.0f}).body;

    // x = 0: a 4 m wall (z from -1.5 to 0), its top a ledge.
    level.wall = add(Kind::Wall, {x_wall, 2.0f, -0.75f}, {3.0f, 2.0f, 0.75f}).body;

    // x = 10: a 5 m wall and a roof slab reaching 1.5 m out from it at 3 m.
    add(Kind::Wall, {x_overhang, 2.5f, -0.75f}, {3.0f, 2.5f, 0.75f});
    level.roof = add(Kind::Roof, {x_overhang, 3.15f, 0.0f}, {3.0f, 0.15f, 1.5f}).body;

    // x = 20: a smooth wall that no hand can take hold of, holds on it, and a lip of rock on top.
    level.smooth_wall = add(Kind::SmoothWall, {x_holds, 1.65f, -0.75f}, {3.0f, 1.65f, 0.75f}).body;
    world.set_surface_material(level.smooth_wall, {.grippable = false});
    // (The lip stands 0.2 m out of the wall: a grippable thing exactly flush behind a smooth one cannot be seen by a cast
    // that stops at the first thing it meets.)
    add(Kind::Lip, {x_holds, 3.4f, -0.65f}, {3.0f, 0.1f, 0.85f});
    const Vec3f hold_places[] = {{-0.25f, 1.7f, 0.0f}, {0.25f, 2.15f, 0.0f}, {-0.25f, 2.6f, 0.0f}, {0.25f, 3.05f, 0.0f}};
    for (const Vec3f& h : hold_places) {
        // A box standing 0.15 m out of the wall: a jug of 0.3 x 0.12 m.
        Piece& p = add(Kind::Hold, {x_holds + h.x, h.y, 0.075f}, {0.15f, 0.06f, 0.075f});
        (void)world.set_hold(p.body, {.quality = 1.0f});
        level.holds.push_back(p.body);
    }

    // x = 30: a slab that moves along X, a hold itself.
    {
        const Vec3f start = level.platform_local_start;
        Piece& p = add(Kind::Platform, start, {1.0f, 0.4f, 0.3f}, Body3DMotion::Kinematic, 0.0f, 0.9f);
        level.platform = p.body;
        level.platform_centre = p.centre;
        (void)world.set_hold(p.body, {.quality = 1.0f, .slide = true});
    }

    // x = 40: a pedestal and a crate with a third of it over the edge.
    level.pedestal = add(Kind::Pedestal, {x_crate, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}).body;
    level.crate = add(Kind::Crate, {x_crate, 2.4f, 0.9f}, {0.4f, 0.4f, 0.4f}, Body3DMotion::Dynamic, 30.0f, 0.5f).body;

    // x = 50: a tall wall of ice.
    level.ice = add(Kind::IceWall, {x_ice, 3.5f, -0.75f}, {3.0f, 3.5f, 0.75f}).body;
    world.set_surface_material(level.ice, {.grip = 1.0f, .slip = 1.0f, .drain = 2.0f, .tag = 1});

    // x = 60: a ledge at 3 m, and a roof 1.4 m over it.
    add(Kind::Ledge, {x_low, 1.5f, -1.0f}, {3.0f, 1.5f, 1.0f});
    level.low_roof = add(Kind::LowRoof, {x_low, 4.5f, -1.0f}, {3.0f, 0.1f, 1.0f}).body;

    world.optimize_broad_phase();
    return level;
}

// The character of the scripted route and of the picture: a person of the biped fixture's size (1.95 m, the shoulders at
// 1.65 m), traversal on.
inline CharacterDesc3D climber_desc() {
    CharacterDesc3D d;
    d.height = 1.95f;
    d.radius = 0.3f;
    d.traversal.enabled = true;
    d.traversal.reach = 0.95f; // the arm of the biped is 0.95 m
    d.traversal.limbs[0].shoulder = {-0.2f, 1.65f, 0.0f};
    d.traversal.limbs[1].shoulder = {0.2f, 1.65f, 0.0f};
    return d;
}

// --- the scripted timeline ------------------------------------------------------------------------------------------------
// Commands (held until the next key) and teleports, and checkpoints (boxes the feet must be in at a frame). It lives in the
// scene file (tests/reference/scenes/physics_climb.json) so that the picture and the CPU test run the same thing.

struct Key {
    int frame = 0;
    bool has_move = false, has_forward = false, has_jump = false, has_teleport = false, has_reach = false;
    bool has_grab = false, has_pull_up = false, has_jump_off = false, has_drop = false, has_stamina = false;
    f32 stamina = 0.0f;
    Vec2f move{};
    Vec3f forward{0, 0, -1};
    bool jump = false;
    bool grab[2] = {false, false};
    Vec3f reach{};
    bool pull_up = false, jump_off = false, drop = false;
    Vec3f teleport{};
};

struct Checkpoint {
    int frame = 0;
    Vec3f lo{}, hi{};
    std::string what;
    // Optional: the mode the character is in ("walking", "hanging", "mantling") and how many hands hold.
    std::string mode;
    int held = -1;
};

struct Route {
    Vec3f start{0.0f, 0.0f, 0.7f};
    int frames = 0;
    std::vector<Key> keys;
    std::vector<Checkpoint> checkpoints;
};

inline Vec3f json_vec(const JsonValue* v, Vec3f fallback = {}) {
    if (v == nullptr || v->items().size() < 3) {
        return fallback;
    }
    return {static_cast<f32>(v->items()[0].as_number()), static_cast<f32>(v->items()[1].as_number()),
            static_cast<f32>(v->items()[2].as_number())};
}

inline Route load_route(const JsonValue& doc) {
    Route r;
    r.start = json_vec(doc.find("start"), r.start);
    r.frames = static_cast<int>(doc.number_at("frames", 0));
    if (const JsonValue* script = doc.find("script")) {
        for (const JsonValue& k : script->items()) {
            Key key;
            key.frame = static_cast<int>(k.number_at("frame"));
            if (const JsonValue* m = k.find("move"); m != nullptr && m->items().size() >= 2) {
                key.has_move = true;
                key.move = {static_cast<f32>(m->items()[0].as_number()), static_cast<f32>(m->items()[1].as_number())};
            }
            if (const JsonValue* f = k.find("forward")) {
                key.has_forward = true;
                key.forward = json_vec(f, key.forward);
            }
            if (const JsonValue* g = k.find("grab"); g != nullptr && g->items().size() >= 2) {
                key.has_grab = true;
                key.grab[0] = g->items()[0].as_bool();
                key.grab[1] = g->items()[1].as_bool();
            }
            if (const JsonValue* rr = k.find("reach")) {
                key.has_reach = true;
                key.reach = json_vec(rr);
            }
            key.has_jump = k.find("jump") != nullptr;
            key.jump = k.bool_at("jump");
            key.has_pull_up = k.find("pull_up") != nullptr;
            key.pull_up = k.bool_at("pull_up");
            key.has_jump_off = k.find("jump_off") != nullptr;
            key.jump_off = k.bool_at("jump_off");
            key.has_drop = k.find("drop") != nullptr;
            key.drop = k.bool_at("drop");
            if (const JsonValue* t = k.find("teleport")) {
                key.has_teleport = true;
                key.teleport = json_vec(t);
            }
            if (k.find("stamina") != nullptr) {
                key.has_stamina = true;
                key.stamina = static_cast<f32>(k.number_at("stamina"));
            }
            r.keys.push_back(key);
        }
    }
    if (const JsonValue* cps = doc.find("checkpoints")) {
        for (const JsonValue& c : cps->items()) {
            Checkpoint cp;
            cp.frame = static_cast<int>(c.number_at("frame"));
            cp.lo = json_vec(c.find("min"));
            cp.hi = json_vec(c.find("max"));
            cp.what = c.string_at("what");
            cp.mode = c.string_at("mode");
            cp.held = c.find("held") != nullptr ? static_cast<int>(c.number_at("held")) : -1;
            r.checkpoints.push_back(cp);
        }
    }
    return r;
}

// Applies the keys of step `frame` to a character's commands (held), returning a teleport when a key has one.
struct Applied {
    CharacterInput3D input;
    TraversalInput3D traversal;
    bool teleport = false;
    Vec3f teleport_to{};
    // A key that sets the resource (a rest, a pick-up) between two parts of the route.
    bool stamina = false;
    f32 stamina_value = 0.0f;
};

inline Applied apply_keys(Applied now, const Route& route, int frame) {
    now.teleport = false;
    now.stamina = false;
    for (const Key& k : route.keys) {
        if (k.frame != frame) {
            continue;
        }
        if (k.has_move) now.input.move = k.move;
        if (k.has_forward) now.input.forward = k.forward;
        if (k.has_jump) now.input.jump = k.jump;
        if (k.has_grab) {
            now.traversal.grab[0] = k.grab[0];
            now.traversal.grab[1] = k.grab[1];
        }
        if (k.has_reach) now.traversal.reach = k.reach;
        if (k.has_pull_up) now.traversal.pull_up = k.pull_up;
        if (k.has_jump_off) now.traversal.jump_off = k.jump_off;
        if (k.has_drop) now.traversal.drop = k.drop;
        if (k.has_teleport) {
            now.teleport = true;
            now.teleport_to = k.teleport;
        }
        if (k.has_stamina) {
            now.stamina = true;
            now.stamina_value = k.stamina;
        }
    }
    return now;
}

} // namespace climb_level
