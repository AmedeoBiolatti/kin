#pragma once

// The character controller's test level (docs/3d_plan.md P2c), built of boxes in a World3D: the same level for the CPU
// tests (tests/physics3d_character_tests.cpp), the reference scene `physics_character` (tools/reference) and the cost
// bench, so that what is measured is what is drawn.
//
// All of it along +X (the main route, z = 0), pieces off the route at other z (reached by a teleport of the script):
//
//   x   0 ..  6   flat floor (start x = 1)
//   x   6 .. 10   ramp 20 degrees up to the plateau (top y = 1.456)
//   x  10 .. 14   plateau
//   x  14 .. 16.1 ramp 35 degrees down, back to the floor
//   x  20 .. 24   hump: a round bump 0.3 m high (a sphere of radius 10 buried in the floor)
//   x  30 .. 31.8 staircase: six steps of 15 cm (top y = 0.9), then a flat top to x = 36
//   z = +6  x 40 .. 44   a 60 cm ledge
//   z = -6  x 40 .. 43   a ramp of 50 degrees (too steep to climb at the default 45)
//   z = +12 x 50 .. 58   a low tunnel (ceiling at y = 1.35: a crouch fits, standing does not)
//   z = -12 x 45         a light box (10 kg) to push; z = -16 x 45 a heavy one (800 kg) that does not move
//   z = +18 x 62         a platform that moves back and forth along X (kinematic, 2 m/s) and, at z = +24, one that turns
//   z = -18 x 62         a sensor volume (a trigger), 2 m wide
//
// Every piece's user id is its index + 100 (floor is 100).

#include <kin/core/json_value.hpp>
#include <kin/physics3d/world3d.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace character_level {

using namespace kin;

enum class Kind : u8 { Floor, Ramp, Plateau, Hump, Step, Ledge, Wall, Ceiling, Box, HeavyBox, Platform, Spinner, Sensor };

struct Piece {
    Kind kind = Kind::Floor;
    Shape3DKind shape = Shape3DKind::Box;
    Vec3f centre{};
    Vec3f half{};       // box half extents, or {r, r, r}
    Quatf rotation{};
    Body3D body{};
    u64 user_id = 0;
    bool moving = false; // kinematic or dynamic: the drawer reads its pose from the world
};

struct Level {
    std::vector<Piece> pieces;
    Body3D linear_platform{}, spinner{}, light_box{}, heavy_box{}, sensor{};
    Vec3f linear_centre{}, spinner_centre{};

    // The kinematic platforms' poses at step `n` (60 Hz): the linear one runs along X between x = 59 and 65 at 2 m/s
    // (a triangle wave), the spinner turns about Y at 1 rad/s.
    static Vec3f linear_position(Vec3f centre, int n) {
        const f32 period = 6.0f; // seconds for 6 m there at 2 m/s ... then back: a full cycle is 6 s
        const f32 t = std::fmod(static_cast<f32>(n) / 60.0f, period);
        const f32 u = t < period * 0.5f ? t : period - t; // 0 to 3 to 0
        return {centre.x - 3.0f + 2.0f * u, centre.y, centre.z};
    }
    static Quatf spin_rotation(int n) { return Quatf::axis_angle({0, 1, 0}, static_cast<f32>(n) / 60.0f * 57.29578f); }

    // Moves the kinematic platforms to where they are at step n+1 (call before World3D::step of step n): their velocity
    // is what carries a character.
    void drive(World3D& world, int n, f32 dt) const {
        if (linear_platform.valid()) {
            world.move_kinematic(linear_platform, linear_position(linear_centre, n + 1), {}, dt);
        }
        if (spinner.valid()) {
            world.move_kinematic(spinner, spinner_centre, spin_rotation(n + 1), dt);
        }
    }
};

// `origin` moves the whole level (its floor included) to somewhere else in the world.
inline Level build(World3D& world, bool with_movers = true, Vec3f origin = {}) {
    Level level;
    const auto add = [&](Kind kind, Vec3f centre, Vec3f half, Quatf rotation, f32 friction = 0.6f) -> Piece& {
        centre += origin;
        Piece p;
        p.kind = kind;
        p.centre = centre;
        p.half = half;
        p.rotation = rotation;
        p.user_id = 100 + level.pieces.size();
        p.body = world.create_body({.shape = Shape3D::box(half), .motion = Body3DMotion::Static, .position = centre,
                                    .rotation = rotation, .friction = friction, .user_id = p.user_id});
        level.pieces.push_back(p);
        return level.pieces.back();
    };
    const auto deg = [](f32 d) { return d * 3.14159265f / 180.0f; };
    const auto tilt = [](f32 degrees) { return Quatf::axis_angle({0, 0, 1}, degrees); };

    add(Kind::Floor, {32.5f, -0.5f, 0}, {38.0f, 0.5f, 31.0f}, {});

    // The ramp of `degrees` rising along +X from (x0, y0): its top surface (a slab 0.4 thick) runs from x0 to x0 + run.
    const auto ramp = [&](Kind kind, f32 x0, f32 y0, f32 z, f32 degrees, f32 run, f32 half_width) {
        const f32 a = deg(std::fabs(degrees));
        const f32 sign = degrees < 0 ? -1.0f : 1.0f;
        const f32 rise = run * std::tan(a);
        const f32 length = std::sqrt(run * run + rise * rise);
        // Top surface's middle, then down along the normal by half the slab's thickness.
        const Vec3f mid{x0 + run * 0.5f, y0 + sign * rise * 0.5f, z};
        const Vec3f normal{-sign * std::sin(a), std::cos(a), 0.0f};
        add(kind, mid - normal * 0.2f, {length * 0.5f, 0.2f, half_width}, tilt(degrees));
    };

    ramp(Kind::Ramp, 6.0f, 0.0f, 0.0f, 20.0f, 4.0f, 2.0f);            // up to y = 1.456 at x = 10
    add(Kind::Plateau, {12.0f, 0.728f, 0.0f}, {2.0f, 0.728f, 2.0f}, {});
    ramp(Kind::Ramp, 14.0f, 1.456f, 0.0f, -35.0f, 1.456f / std::tan(deg(35.0f)), 2.0f); // down to the floor

    // The hump: a sphere of radius 10 whose top is 0.3 above the floor, at x = 22.
    {
        Piece p;
        p.kind = Kind::Hump;
        p.shape = Shape3DKind::Sphere;
        p.centre = Vec3f{22.0f, -9.7f, 0.0f} + origin;
        p.half = {10.0f, 10.0f, 10.0f};
        p.user_id = 100 + level.pieces.size();
        p.body = world.create_body({.shape = Shape3D::sphere(10.0f), .motion = Body3DMotion::Static, .position = p.centre,
                                    .friction = 0.6f, .user_id = p.user_id});
        level.pieces.push_back(p);
    }

    // The staircase: six steps, 0.3 deep, 0.15 high each, then a flat top.
    for (int i = 0; i < 6; ++i) {
        const f32 h = 0.15f * static_cast<f32>(i + 1);
        add(Kind::Step, {30.15f + 0.3f * static_cast<f32>(i), h * 0.5f, 0.0f}, {0.15f, h * 0.5f, 2.0f}, {});
    }
    add(Kind::Plateau, {33.9f, 0.45f, 0.0f}, {2.1f, 0.45f, 2.0f}, {});

    add(Kind::Ledge, {42.0f, 0.3f, 6.0f}, {2.0f, 0.3f, 2.0f}, {});
    ramp(Kind::Ramp, 40.0f, 0.0f, -6.0f, 50.0f, 1.26f, 2.0f);
    add(Kind::Plateau, {42.26f, 0.75f, -6.0f}, {1.0f, 0.75f, 2.0f}, {});

    // The tunnel: a roof slab at y = 1.35 on two walls (x 50 to 58, z = 12 +- 1).
    add(Kind::Ceiling, {54.0f, 1.45f, 12.0f}, {4.0f, 0.1f, 1.2f}, {});
    add(Kind::Wall, {54.0f, 0.7f, 10.7f}, {4.0f, 0.7f, 0.1f}, {});
    add(Kind::Wall, {54.0f, 0.7f, 13.3f}, {4.0f, 0.7f, 0.1f}, {});

    // Things that move.
    const auto dynamic_box = [&](Kind kind, Vec3f centre, f32 half, f32 mass, f32 friction) {
        centre += origin;
        Piece p;
        p.kind = kind;
        p.centre = centre;
        p.half = {half, half, half};
        p.user_id = 100 + level.pieces.size();
        p.moving = true;
        p.body = world.create_body({.shape = Shape3D::box(p.half), .motion = Body3DMotion::Dynamic, .position = centre,
                                    .mass = mass, .friction = friction, .user_id = p.user_id});
        level.pieces.push_back(p);
        return p.body;
    };
    level.light_box = dynamic_box(Kind::Box, {45.0f, 0.4f, -12.0f}, 0.4f, 10.0f, 0.4f);
    level.heavy_box = dynamic_box(Kind::HeavyBox, {45.0f, 0.4f, -16.0f}, 0.4f, 800.0f, 0.9f);

    if (with_movers) {
        const auto kinematic = [&](Kind kind, Vec3f centre, Vec3f half) {
            centre += origin;
            Piece p;
            p.kind = kind;
            p.centre = centre;
            p.half = half;
            p.user_id = 100 + level.pieces.size();
            p.moving = true;
            p.body = world.create_body({.shape = Shape3D::box(half), .motion = Body3DMotion::Kinematic, .position = centre,
                                        .friction = 0.9f, .user_id = p.user_id});
            level.pieces.push_back(p);
            return p.body;
        };
        level.linear_centre = Vec3f{62.0f, 0.25f, 18.0f} + origin;
        level.linear_platform = kinematic(Kind::Platform, {62.0f, 0.25f, 18.0f}, {1.5f, 0.25f, 1.5f});
        level.spinner_centre = Vec3f{62.0f, 0.25f, 24.0f} + origin;
        level.spinner = kinematic(Kind::Spinner, {62.0f, 0.25f, 24.0f}, {3.0f, 0.25f, 3.0f});

        Piece s;
        s.kind = Kind::Sensor;
        s.centre = Vec3f{62.0f, 1.0f, -18.0f} + origin;
        s.half = {1.0f, 1.0f, 1.0f};
        s.user_id = 100 + level.pieces.size();
        s.body = world.create_body({.shape = Shape3D::box(s.half), .motion = Body3DMotion::Static, .position = s.centre,
                                    .layer = layers3d::Sensor, .sensor = true, .user_id = s.user_id});
        level.pieces.push_back(s);
        level.sensor = s.body;
    }
    return level;
}

// --- the scripted route -------------------------------------------------------------------------------------------------
// A timeline of commands (held until the next key), teleports between the pieces off the main route, and checkpoints:
// boxes the character's feet must be in at a frame. It lives in the scene file (tests/reference/scenes/physics_character.json)
// so that the picture and the CPU test run the same thing.

struct Key {
    int frame = 0;
    bool has_move = false, has_forward = false, has_jump = false, has_crouch = false, has_sprint = false, has_teleport = false;
    Vec2f move{};
    Vec3f forward{1, 0, 0};
    bool jump = false, crouch = false, sprint = false;
    Vec3f teleport{};
};

struct Checkpoint {
    int frame = 0;
    Vec3f lo{}, hi{};
    bool has_crouching = false;
    bool crouching = false;
    std::string what;
};

struct Route {
    Vec3f start{1, 0, 0};
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

inline Route load_route(const JsonValue& character) {
    Route r;
    r.start = json_vec(character.find("start"), r.start);
    r.frames = static_cast<int>(character.number_at("frames", 0));
    if (const JsonValue* script = character.find("script")) {
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
            key.has_jump = k.find("jump") != nullptr;
            key.jump = k.bool_at("jump");
            key.has_crouch = k.find("crouch") != nullptr;
            key.crouch = k.bool_at("crouch");
            key.has_sprint = k.find("sprint") != nullptr;
            key.sprint = k.bool_at("sprint");
            if (const JsonValue* t = k.find("teleport")) {
                key.has_teleport = true;
                key.teleport = json_vec(t);
            }
            r.keys.push_back(key);
        }
    }
    if (const JsonValue* cps = character.find("checkpoints")) {
        for (const JsonValue& c : cps->items()) {
            Checkpoint cp;
            cp.frame = static_cast<int>(c.number_at("frame"));
            cp.lo = json_vec(c.find("min"));
            cp.hi = json_vec(c.find("max"));
            cp.has_crouching = c.find("crouching") != nullptr;
            cp.crouching = c.bool_at("crouching");
            cp.what = c.string_at("what");
            r.checkpoints.push_back(cp);
        }
    }
    return r;
}

// Applies the keys of step `frame` to the character: its command (held), and a teleport.
inline void apply_keys(World3D& world, Character3D c, const Route& route, int frame) {
    CharacterInput3D in = world.character_input(c);
    bool changed = false;
    for (const Key& k : route.keys) {
        if (k.frame != frame) {
            continue;
        }
        changed = true;
        if (k.has_move) in.move = k.move;
        if (k.has_forward) in.forward = k.forward;
        if (k.has_jump) in.jump = k.jump;
        if (k.has_crouch) in.crouch = k.crouch;
        if (k.has_sprint) in.sprint = k.sprint;
        if (k.has_teleport) world.teleport_character(c, k.teleport);
    }
    if (changed) {
        world.set_character_input(c, in);
    }
}

} // namespace character_level
