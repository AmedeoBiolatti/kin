// The character controller of kin/physics3d (kin/physics3d/character3d.hpp), without a GPU: behaviours against
// hand-computed values on small scenes and on the shared test level (tests/character_level.hpp), determinism with 0, 1
// and 4 workers, saved states. docs/physics_3d.md ("Character controller").

#include "character_level.hpp"

#include <kin/core/jobs.hpp>
#include <kin/physics3d/world3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace kin;

int failures = 0;
bool verbose = false;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            ++failures;                                                                        \
            std::fprintf(stderr, "physics3d_character_tests:%d: CHECK failed: %s\n", __LINE__, #cond); \
        }                                                                                      \
    } while (0)

// A check that shows the measured number in the log when it fails and, with -v, always.
#define CHECK_NEAR(value, expected, eps)                                                                              \
    do {                                                                                                              \
        const double v_ = static_cast<double>(value), e_ = static_cast<double>(expected);                             \
        if (verbose) std::printf("  %s = %.4f (expected %.4f +- %.4f)\n", #value, v_, e_, static_cast<double>(eps));    \
        if (!(std::fabs(v_ - e_) <= static_cast<double>(eps))) {                                                      \
            ++failures;                                                                                               \
            std::fprintf(stderr, "physics3d_character_tests:%d: %s = %.5f, expected %.5f +- %.5f\n", __LINE__, #value, v_, \
                         e_, static_cast<double>(eps));                                                               \
        }                                                                                                             \
    } while (0)

constexpr f32 dt = 1.0f / 60.0f;

bool near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

World3DConfig serial() {
    World3DConfig c;
    c.single_threaded = true;
    return c;
}

void run(World3D& world, int frames) {
    for (int i = 0; i < frames; ++i) {
        world.step(dt);
    }
}

Body3D add_floor(World3D& world, f32 friction = 0.6f) {
    return world.create_body({.shape = Shape3D::box({100.0f, 0.5f, 100.0f}), .motion = Body3DMotion::Static,
                              .position = {0.0f, -0.5f, 0.0f}, .friction = friction, .user_id = 1});
}

Body3D add_static(World3D& world, Vec3f centre, Vec3f half, Quatf rotation = {}, u64 user_id = 0, u8 layer = layers3d::Auto) {
    return world.create_body({.shape = Shape3D::box(half), .motion = Body3DMotion::Static, .position = centre,
                              .rotation = rotation, .friction = 0.6f, .layer = layer, .user_id = user_id});
}

CharacterInput3D forward_x(f32 amount = 1.0f, bool sprint = false) {
    CharacterInput3D in;
    in.move = {0.0f, amount};
    in.forward = {1.0f, 0.0f, 0.0f};
    in.sprint = sprint;
    return in;
}

Character3D make(World3D& world, Vec3f feet, CharacterDesc3D desc = {}) {
    desc.position = feet;
    const Character3D c = world.create_character(desc);
    CHECK(c.valid());
    return c;
}

// The least and greatest of a value over a run.
struct Range {
    f32 lo = 1e30f, hi = -1e30f;
    void add(f32 v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
};

// --- tests -----------------------------------------------------------------------------------------------------------

void test_stand_and_rest() {
    World3D world(serial());
    add_floor(world);
    const Character3D c = make(world, {0.0f, 0.5f, 0.0f});
    run(world, 120);
    const CharacterState3D s = world.character(c);
    CHECK_NEAR(s.position.y, 0.0, 0.03);
    CHECK(s.ground == CharacterGround3D::OnGround);
    CHECK(s.ground_user_id == 1);
    CHECK_NEAR(s.ground_normal.y, 1.0, 1e-3);
    CHECK_NEAR(length(s.velocity), 0.0, 0.05);
    CHECK_NEAR(s.height, 1.8, 1e-4);
    CHECK(s.body.valid() && world.layer(s.body) == layers3d::Character);
    // The inner body is where the capsule is: its centre half a height above the feet.
    CHECK_NEAR(world.position(s.body).y, s.position.y + 0.02, 0.05);
    // A character that is not asked to move stays put (no drift).
    const Vec3f at = s.position;
    run(world, 300);
    CHECK_NEAR(length(world.character(c).position - at), 0.0, 1e-4);
}

void test_walk_speed_and_stop() {
    World3D world(serial());
    add_floor(world);
    const Character3D c = make(world, {0.0f, 0.0f, 0.0f});
    run(world, 30);
    const f32 x0 = world.character(c).position.x;
    world.set_character_input(c, forward_x());
    run(world, 120); // 2 s: 0.1 s to speed, then 4 m/s
    const CharacterState3D s = world.character(c);
    CHECK_NEAR(s.ground_speed, 4.0, 0.05);
    CHECK_NEAR(s.position.x - x0, 4.0 * 2.0 - 0.5 * 4.0 * 0.1, 0.1);
    CHECK_NEAR(s.position.z, 0.0, 1e-3);
    world.set_character_input(c, forward_x(1.0f, true));
    run(world, 60);
    CHECK_NEAR(world.character(c).ground_speed, 7.0, 0.05);
    // Half input is half speed; no input stops within a few steps.
    world.set_character_input(c, forward_x(0.5f));
    run(world, 60);
    CHECK_NEAR(world.character(c).ground_speed, 2.0, 0.05);
    world.set_character_input(c, {});
    run(world, 10);
    CHECK_NEAR(world.character(c).ground_speed, 0.0, 0.01);
    // Diagonal input is not faster than forward (the move vector is cut to length 1).
    CharacterInput3D diag = forward_x();
    diag.move = {1.0f, 1.0f};
    world.set_character_input(c, diag);
    run(world, 60);
    CHECK_NEAR(world.character(c).ground_speed, 4.0, 0.05);
}

void test_slopes() {
    // A slab tilted `degrees` about Z rising along +X, 0.4 thick, top surface through the origin.
    const auto slope = [](World3D& world, f32 degrees) {
        const f32 a = degrees * 3.14159265f / 180.0f;
        const Vec3f normal{-std::sin(a), std::cos(a), 0.0f};
        add_static(world, normal * -0.2f + Vec3f{0, 0, 0} + Vec3f{10.0f * std::cos(a), 10.0f * std::sin(a), 0.0f},
                   {30.0f, 0.2f, 10.0f}, Quatf::axis_angle({0, 0, 1}, degrees), 5);
    };
    struct Row { f32 degrees; f32 gain; f32 speed; };
    for (const f32 degrees : {20.0f, 35.0f, 44.0f}) {
        World3D world(serial());
        slope(world, degrees);
        const f32 a = degrees * 3.14159265f / 180.0f;
        const Character3D c = make(world, {0.0f, 0.0f, 0.0f});
        run(world, 30);
        const Vec3f p0 = world.character(c).position;
        world.set_character_input(c, forward_x());
        run(world, 120);
        const CharacterState3D s = world.character(c);
        // It climbs: on the ground, at roughly the walk speed over the slope's own length.
        CHECK(s.ground == CharacterGround3D::OnGround);
        const f32 climbed = s.position.y - p0.y;
        // The horizontal speed is held at walk_speed, so the height gained is that times tan(angle).
        if (verbose) std::printf("slope %.0f: gain %.2f in 2 s, ground speed %.2f, x %.2f\n", degrees, climbed, s.ground_speed, s.position.x - p0.x);
        CHECK(climbed > 0.8f * 2.0f * 4.0f * std::sin(a) * 0.9f);
        CHECK(s.ground_normal.y > 0.7f);
        CHECK_NEAR(s.ground_normal.y, std::cos(a), 0.01);
    }
    for (const f32 degrees : {50.0f, 60.0f}) {
        World3D world(serial());
        slope(world, degrees);
        // Put it on the slope part-way up and ask it to walk up: it slides down instead.
        const f32 a = degrees * 3.14159265f / 180.0f;
        const Character3D c = make(world, {5.0f * std::cos(a), 5.0f * std::sin(a) + 0.02f, 0.0f});
        run(world, 5);
        world.set_character_input(c, forward_x());
        const Vec3f p0 = world.character(c).position;
        run(world, 60);
        const CharacterState3D s = world.character(c);
        if (verbose) std::printf("steep %.0f: y %.2f -> %.2f, ground %d\n", degrees, p0.y, s.position.y, int(s.ground));
        CHECK(s.position.y < p0.y - 0.5f);
        CHECK(s.ground == CharacterGround3D::Sliding || s.ground == CharacterGround3D::InAir);
    }
}


void test_camera_frame() {
    World3D world(serial());
    add_floor(world);
    const Character3D c = make(world, {0, 0, 0});
    run(world, 10);
    // forward (0, 0, -1): move.y goes -Z, move.x goes +X (right of forward).
    CharacterInput3D in;
    in.forward = {0.0f, 0.0f, -1.0f};
    in.move = {0.0f, 1.0f};
    world.set_character_input(c, in);
    run(world, 60);
    CharacterState3D s = world.character(c);
    CHECK(s.position.z < -3.0f && std::fabs(s.position.x) < 1e-3f);
    // The forward vector's part along up is ignored, a forward along up moves nothing.
    in.forward = {0.0f, 1.0f, 0.0f};
    world.set_character_input(c, in);
    run(world, 30);
    CHECK_NEAR(world.character(c).ground_speed, 0.0, 0.01);
    in.forward = {1.0f, 0.7f, 0.0f};
    in.move = {0.0f, 1.0f};
    world.set_character_input(c, in);
    run(world, 30);
    CHECK_NEAR(world.character(c).ground_speed, 4.0, 0.05);
    CHECK(near(world.character(c).velocity.y, 0.0f, 0.05f));
    // Strafing: move.x along the right of forward (0,0,-1) is +X.
    in.forward = {0.0f, 0.0f, -1.0f};
    in.move = {1.0f, 0.0f};
    world.set_character_input(c, in);
    const f32 x0 = world.character(c).position.x;
    run(world, 60);
    CHECK(world.character(c).position.x - x0 > 3.0f);
}

void test_stairs_and_ledges() {
    // Six steps of 15 cm and 30 cm deep along +X.
    {
        World3D world(serial());
        add_floor(world);
        for (int i = 0; i < 6; ++i) {
            const f32 h = 0.15f * static_cast<f32>(i + 1);
            add_static(world, {3.15f + 0.3f * static_cast<f32>(i), h * 0.5f, 0}, {0.15f, h * 0.5f, 3.0f});
        }
        add_static(world, {8.9f, 0.45f, 0}, {4.1f, 0.45f, 3.0f});
        const Character3D c = make(world, {0, 0, 0});
        run(world, 10);
        world.set_character_input(c, forward_x());
        Range vy;
        for (int i = 0; i < 180; ++i) {
            world.step(dt);
            vy.add(world.character(c).position.y);
        }
        const CharacterState3D s = world.character(c);
        CHECK_NEAR(s.position.y, 0.9, 0.04);
        CHECK(s.position.x > 6.0f); // on its way, not stuck at the foot
        // Never in the air for long: it stays on the ground on the way up.
        CHECK(s.ground == CharacterGround3D::OnGround);
    }
    // One ledge of a given height: max step 0.4 climbs 0.35 and not 0.45; max step 0.2 does not climb 0.35.
    struct Case { f32 ledge; f32 max_step; bool climbs; };
    for (const Case k : {Case{0.35f, 0.4f, true}, Case{0.6f, 0.4f, false}, Case{0.35f, 0.2f, false}, Case{0.19f, 0.2f, true}}) {
        World3D world(serial());
        add_floor(world);
        add_static(world, {5.0f, k.ledge * 0.5f, 0}, {4.0f, k.ledge * 0.5f, 3.0f});
        CharacterDesc3D d;
        d.max_step_height = k.max_step;
        const Character3D c = make(world, {0, 0, 0}, d);
        run(world, 10);
        world.set_character_input(c, forward_x());
        run(world, 120);
        const CharacterState3D s = world.character(c);
        if (verbose) std::printf("ledge %.2f step %.2f: x %.2f y %.2f\n", k.ledge, k.max_step, s.position.x, s.position.y);
        if (k.climbs) {
            CHECK(s.position.x > 5.0f && near(s.position.y, k.ledge, 0.04f));
        } else {
            CHECK(s.position.x < 1.0f + 0.01f && s.position.x > 0.0f && near(s.position.y, 0.0f, 0.03f));
        }
    }
}

void test_stick_to_ground() {
    // A ramp down at 30 degrees from the edge of a platform at y = 0, walked at sprint speed.
    const auto measure = [&](f32 step_down, int* air_steps, f32* clearance) {
        World3D world(serial());
        add_static(world, {-20.0f, -0.5f, 0}, {20.0f, 0.5f, 5.0f}); // the platform, top y = 0
        const f32 a = 30.0f * 3.14159265f / 180.0f;
        // A slab whose top surface leaves (0, 0) going down at 30 degrees.
        const Vec3f normal{std::sin(a), std::cos(a), 0.0f};
        add_static(world, Vec3f{10.0f * std::cos(a), -10.0f * std::sin(a), 0.0f} - normal * 0.2f, {10.0f, 0.2f, 5.0f},
                   Quatf::axis_angle({0, 0, 1}, -30.0f));
        CharacterDesc3D d;
        d.step_down_distance = step_down;
        d.sprint_speed = 7.0f;
        const Character3D c = make(world, {-6.0f, 0, 0}, d);
        run(world, 10);
        world.set_character_input(c, forward_x(1.0f, true));
        int air = 0;
        f32 worst = 0.0f;
        for (int i = 0; i < 150; ++i) {
            world.step(dt);
            const CharacterState3D s = world.character(c);
            if (s.position.x > 0.2f && s.position.x < 8.0f) {
                if (s.ground != CharacterGround3D::OnGround) {
                    ++air;
                }
                // Distance above the slope's surface under the feet (a ray down).
                if (const auto hit = world.raycast({s.position.x, s.position.y + 1.0f, 0}, {s.position.x, s.position.y - 3.0f, 0},
                                                   {.ignore = s.body})) {
                    worst = std::max(worst, s.position.y - hit->point.y);
                }
            }
        }
        *air_steps = air;
        *clearance = worst;
    };
    int air_on = 0, air_off = 0;
    f32 gap_on = 0.0f, gap_off = 0.0f;
    measure(0.5f, &air_on, &gap_on);
    measure(0.0f, &air_off, &gap_off);
    if (verbose) std::printf("down a 30 degree ramp at 7 m/s: stick on: %d air steps, gap %.3f; off: %d, gap %.3f\n", air_on, gap_on, air_off, gap_off);
    CHECK(air_on == 0);
    CHECK(gap_on < 0.05f);
    CHECK(air_off >= 5);
    CHECK(gap_off > 0.05f);

    // The hump (a sphere of radius 10, 0.3 high) at 7 m/s.
    World3D world(serial());
    add_floor(world);
    world.create_body({.shape = Shape3D::sphere(10.0f), .motion = Body3DMotion::Static, .position = {20.0f, -9.7f, 0.0f}, .friction = 0.6f});
    const Character3D c = make(world, {10.0f, 0, 0});
    run(world, 10);
    world.set_character_input(c, forward_x(1.0f, true));
    int air = 0;
    f32 top = 0.0f;
    for (int i = 0; i < 180; ++i) {
        world.step(dt);
        const CharacterState3D s = world.character(c);
        if (s.position.x > 11.0f && s.position.x < 29.0f && s.ground != CharacterGround3D::OnGround) {
            ++air;
        }
        top = std::max(top, s.position.y);
    }
    CHECK(air == 0);
    CHECK_NEAR(top, 0.3, 0.04);
}

void test_jump() {
    for (const f32 g : {9.81f, 20.0f}) {
        World3DConfig cfg = serial();
        cfg.gravity = {0.0f, -g, 0.0f};
        World3D world(cfg);
        add_floor(world);
        CharacterDesc3D d;
        d.jump_speed = 5.0f;
        const Character3D c = make(world, {0, 0, 0}, d);
        run(world, 30);
        CharacterInput3D in;
        in.jump = true;
        world.set_character_input(c, in);
        Range y;
        int air_steps = 0;
        for (int i = 0; i < 180; ++i) {
            world.step(dt);
            const CharacterState3D s = world.character(c);
            y.add(s.position.y);
            if (s.ground != CharacterGround3D::OnGround) {
                ++air_steps;
            }
            if (i == 0) {
                in.jump = false;
                world.set_character_input(c, in);
            }
        }
        const f32 analytic = 25.0f / (2.0f * g);
        // The discrete step loses about v dt / 2 off the continuous apex.
        if (verbose) std::printf("jump at g %.2f: apex %.3f, analytic %.3f, %d steps in the air\n", g, y.hi, analytic, air_steps);
        CHECK(y.hi < analytic + 0.01f && y.hi > analytic - 5.0f * dt * 0.5f - 0.01f);
        const f32 flight = 2.0f * 5.0f / g;
        CHECK_NEAR(static_cast<f32>(air_steps) * dt, flight, 0.1f);
        CHECK(world.character(c).ground == CharacterGround3D::OnGround);
    }
    // gravity_scale 0.5 doubles the apex; a held jump command does not jump again (an edge makes a jump).
    World3D world(serial());
    add_floor(world);
    CharacterDesc3D d;
    d.gravity_scale = 0.5f;
    const Character3D c = make(world, {0, 0, 0}, d);
    run(world, 30);
    CharacterInput3D in;
    in.jump = true;
    world.set_character_input(c, in);
    int jumps = 0;
    Range y;
    for (int i = 0; i < 300; ++i) {
        world.step(dt);
        y.add(world.character(c).position.y);
        jumps += world.character(c).jumped ? 1 : 0;
    }
    CHECK_NEAR(y.hi, 25.0 / (2.0 * 4.905), 0.1);
    CHECK(jumps == 1);
    // Released and pressed again: another.
    in.jump = false;
    world.set_character_input(c, in);
    run(world, 2);
    in.jump = true;
    world.set_character_input(c, in);
    run(world, 2);
    CHECK(world.character(c).vertical_speed > 0.0f || world.character(c).jumped);
}

void test_tunnelling() {
    World3D world(serial());
    add_floor(world);
    add_static(world, {5.0f, 1.5f, 0}, {0.01f, 1.5f, 4.0f}); // 2 cm thick
    CharacterDesc3D d;
    d.sprint_speed = 60.0f;
    d.acceleration = 100000.0f;
    const Character3D c = make(world, {0, 0, 0}, d);
    run(world, 5);
    world.set_character_input(c, forward_x(1.0f, true));
    f32 furthest = 0.0f;
    for (int i = 0; i < 120; ++i) {
        world.step(dt);
        furthest = std::max(furthest, world.character(c).position.x);
    }
    if (verbose) std::printf("60 m/s at a 2 cm wall at x = 5: furthest x %.3f\n", furthest);
    CHECK(furthest < 5.0f - 0.3f + 0.05f);
    CHECK(furthest > 4.0f);
    // The same speed with no wall goes the distance (the test is the wall, not a speed cap).
    World3D open(serial());
    add_floor(open);
    const Character3D c2 = make(open, {0, 0, 0}, d);
    run(open, 5);
    open.set_character_input(c2, forward_x(1.0f, true));
    run(open, 60);
    CHECK(open.character(c2).position.x > 50.0f);
}

void test_moving_platforms() {
    // A platform going along +X at 2 m/s: a character on it with no input is carried.
    {
        World3D world(serial());
        add_floor(world);
        const Body3D platform = world.create_body({.shape = Shape3D::box({2.0f, 0.25f, 2.0f}), .motion = Body3DMotion::Kinematic,
                                                   .position = {0, 0.25f, 0}, .friction = 0.9f, .user_id = 77});
        const Character3D c = make(world, {0, 0.5f, 0});
        Vec3f centre{0, 0.25f, 0};
        const auto advance = [&](int steps) {
            for (int i = 0; i < steps; ++i) {
                centre.x += 2.0f * dt;
                world.move_kinematic(platform, centre, {}, dt);
                world.step(dt);
            }
        };
        run(world, 30);
        CHECK(world.character(c).ground_user_id == 77);
        advance(180);
        const CharacterState3D s = world.character(c);
        CHECK_NEAR(world.position(platform).x, 6.0, 0.01);
        CHECK_NEAR(s.position.x - world.position(platform).x, 0.0, 0.03);
        CHECK_NEAR(s.position.y, 0.5, 0.03);
        CHECK_NEAR(s.ground_velocity.x, 2.0, 0.05);
        CHECK(s.ground_user_id == 77);
        CHECK_NEAR(s.ground_speed, 0.0, 0.05); // standing still on it
        // Walking on it: ground speed is relative to it (4), the world velocity 4 + 2.
        world.set_character_input(c, forward_x());
        advance(20);
        CHECK_NEAR(world.character(c).ground_speed, 4.0, 0.05);
        CHECK_NEAR(world.character(c).velocity.x, 6.0, 0.1);
        // Jumping off keeps the platform's speed: after the take-off its horizontal speed is 2 (no input).
        world.set_character_input(c, {});
        advance(30);
        CharacterInput3D in;
        in.jump = true;
        world.set_character_input(c, in);
        advance(6);
        const CharacterState3D air = world.character(c);
        CHECK(air.ground == CharacterGround3D::InAir);
        CHECK_NEAR(air.velocity.x, 2.0, 0.15);
    }
    // A platform that turns about Y at 1 rad/s: a character 2 m from the axis goes round with it, its radius kept.
    {
        World3D world(serial());
        add_floor(world);
        const Body3D platform = world.create_body({.shape = Shape3D::box({4.0f, 0.25f, 4.0f}), .motion = Body3DMotion::Kinematic,
                                                   .position = {0, 0.25f, 0}, .friction = 0.9f, .user_id = 78});
        const Character3D c = make(world, {2.0f, 0.5f, 0});
        run(world, 30);
        for (int i = 0; i < 360; ++i) {
            world.move_kinematic(platform, {0, 0.25f, 0}, Quatf::axis_angle({0, 1, 0}, static_cast<f32>(i + 1) * dt * 57.29578f), dt);
            world.step(dt);
        }
        const CharacterState3D s = world.character(c);
        const f32 radius = std::sqrt(s.position.x * s.position.x + s.position.z * s.position.z);
        // After 6 s at 1 rad/s: 6 rad = 343.8 degrees about +Y: x = r cos(6), z = -r sin(6).
        if (verbose) std::printf("spinner: r %.3f angle %.3f rad\n", radius, std::atan2(-s.position.z, s.position.x));
        CHECK_NEAR(radius, 2.0, 0.08);
        CHECK_NEAR(s.position.x, 2.0 * std::cos(6.0), 0.15);
        CHECK_NEAR(s.position.z, -2.0 * std::sin(6.0), 0.15);
        CHECK(s.ground == CharacterGround3D::OnGround);
    }
}

void test_pushing_boxes() {
    const auto push = [&](f32 mass, f32 friction, f32 strength, f32* box_travel, f32* character_travel) {
        World3D world(serial());
        add_floor(world, 0.6f);
        const Body3D box = world.create_body({.shape = Shape3D::box({0.4f, 0.4f, 0.4f}), .position = {1.5f, 0.4f, 0},
                                              .mass = mass, .friction = friction, .user_id = 9});
        CharacterDesc3D d;
        d.push_strength = strength;
        const Character3D c = make(world, {0, 0, 0}, d);
        run(world, 60);
        const f32 b0 = world.position(box).x;
        world.set_character_input(c, forward_x());
        run(world, 120);
        *box_travel = world.position(box).x - b0;
        *character_travel = world.character(c).position.x;
    };
    f32 box_light = 0, char_light = 0, box_heavy = 0, char_heavy = 0, box_strong = 0, char_strong = 0;
    push(10.0f, 0.4f, 150.0f, &box_light, &char_light);
    push(800.0f, 0.9f, 150.0f, &box_heavy, &char_heavy);
    push(800.0f, 0.9f, 20000.0f, &box_strong, &char_strong);
    if (verbose) {
        std::printf("push: light box %.2f m (character %.2f), heavy %.2f m (character %.2f), heavy with 20 kN %.2f (character %.2f)\n",
                    box_light, char_light, box_heavy, char_heavy, box_strong, char_strong);
    }
    CHECK(box_light > 5.0f);
    CHECK(char_light > 6.0f);
    CHECK(box_heavy < 0.1f);
    CHECK(char_heavy < 1.5f);
    CHECK(box_strong > 1.0f);
    CHECK(box_strong > 10.0f * box_heavy);
}

void test_standing_on_dynamic() {
    World3D world(serial());
    add_floor(world, 0.0f);
    const Body3D raft = world.create_body({.shape = Shape3D::box({2.0f, 0.25f, 2.0f}), .position = {0, 0.25f, 0},
                                           .linear_velocity = {1.0f, 0, 0}, .mass = 2000.0f, .friction = 0.0f, .user_id = 55});
    const Character3D c = make(world, {0, 0.5f, 0});
    run(world, 120);
    const CharacterState3D s = world.character(c);
    CHECK(s.ground_user_id == 55);
    CHECK_NEAR(world.position(raft).x, 2.0, 0.1);
    // Carried by it with no input and no friction between them: only the ground velocity does it.
    CHECK_NEAR(s.position.x - world.position(raft).x, 0.0, 0.1);
    CHECK_NEAR(s.ground_velocity.x, 1.0, 0.05);
    // And it weighs on it: the raft's own speed is hardly changed (70 kg on 2000), a 70 kg load pushes it into the floor.
    CHECK_NEAR(world.linear_velocity(raft).x, 1.0, 0.05);
}

void test_ceiling_bump() {
    World3D world(serial());
    add_floor(world);
    add_static(world, {0, 2.5f, 0}, {5.0f, 0.3f, 5.0f}, {}, 66); // underside y = 2.2
    const Character3D c = make(world, {0, 0, 0});
    run(world, 30);
    world.drain_character_events();
    CharacterInput3D in;
    in.jump = true;
    world.set_character_input(c, in);
    f32 top = 0.0f;
    int hit_step = -1;
    f32 hit_speed = 0.0f;
    int turned_step = -1;
    for (int i = 0; i < 90; ++i) {
        world.step(dt);
        in.jump = false;
        world.set_character_input(c, in);
        for (const CharacterEvent3D& e : world.drain_character_events()) {
            if (e.kind == CharacterEventKind3D::HitHead) {
                CHECK(hit_step < 0);
                hit_step = i;
                hit_speed = e.speed;
                CHECK(e.body_user_id == 0 || e.body_user_id == 66);
                CHECK(e.normal.y < -0.9f);
            }
        }
        const CharacterState3D s = world.character(c);
        top = std::max(top, s.position.y);
        if (hit_step >= 0 && turned_step < 0 && s.vertical_speed < -0.01f) {
            turned_step = i;
        }
    }
    if (verbose) std::printf("ceiling: top of feet %.3f (head %.3f), hit at step %d at %.2f m/s, falling from step %d\n", top, top + 1.8f, hit_step, hit_speed, turned_step);
    CHECK(top + 1.8f < 2.2f + 0.04f);
    CHECK(hit_step > 0);
    CHECK(hit_speed > 1.0f);
    // No hanging under the ceiling: it is falling again within a few steps of the hit.
    CHECK(turned_step - hit_step <= 4);
}

void test_sensors() {
    World3D world(serial());
    add_floor(world);
    const Body3D sensor = world.create_body({.shape = Shape3D::box({1.0f, 1.0f, 3.0f}), .motion = Body3DMotion::Static,
                                             .position = {5.0f, 1.0f, 0}, .layer = layers3d::Sensor, .sensor = true, .user_id = 321});
    const Character3D c = make(world, {0, 0, 0});
    run(world, 10);
    world.drain_character_events();
    world.drain_events();
    world.set_character_input(c, forward_x());
    std::vector<CharacterEvent3D> seen;
    for (int i = 0; i < 180; ++i) {
        world.step(dt);
        for (const CharacterEvent3D& e : world.drain_character_events()) {
            if (e.body_user_id == 321) {
                seen.push_back(e);
            }
        }
    }
    // Walked straight through at the full speed (not stopped), with one begin and one end.
    CHECK(world.character(c).position.x > 10.0f);
    CHECK(seen.size() == 2);
    if (seen.size() == 2) {
        CHECK(seen[0].kind == CharacterEventKind3D::ContactBegin && seen[0].sensor);
        CHECK(seen[1].kind == CharacterEventKind3D::ContactEnd && seen[1].sensor);
    }
    // The world's own events see the character's body too (sensor flagged, the character's user id on its side).
    (void)sensor;
}

void test_layers() {
    const auto walk = [&](Layer3DMask mask) {
        World3D world(serial());
        add_floor(world);
        add_static(world, {4.0f, 1.0f, 0}, {0.2f, 1.0f, 4.0f}, {}, 5, layers3d::User0);
        CharacterDesc3D d;
        d.collides_with = mask;
        const Character3D c = make(world, {0, 0, 0}, d);
        run(world, 5);
        world.set_character_input(c, forward_x());
        run(world, 120);
        return world.character(c).position.x;
    };
    const f32 blocked = walk(all_layers3d);
    const f32 passes = walk(all_layers3d & ~layer_bit(layers3d::User0));
    if (verbose) std::printf("layers: blocked at x %.2f, masked out reaches x %.2f\n", blocked, passes);
    CHECK(blocked < 4.0f);
    CHECK(passes > 6.0f);
}

void test_gravity_direction() {
    // Ceiling: gravity turned over, the character falls up and stands on the underside of a slab at y = 6.
    {
        World3D world(serial());
        add_floor(world);
        add_static(world, {0, 7.0f, 0}, {50.0f, 1.0f, 50.0f}, {}, 9);
        const Character3D c = make(world, {0, 0, 0});
        run(world, 30);
        CHECK_NEAR(world.character(c).up.y, 1.0, 1e-5);
        world.set_gravity({0.0f, 9.81f, 0.0f});
        // The up vector turns smoothly: per-step angle small, monotone, within 1 degree of the target soon.
        f32 previous = 3.14159265f;
        f32 largest_turn = 0.0f;
        int settled_step = -1;
        for (int i = 0; i < 180; ++i) {
            world.step(dt);
            const f32 angle = std::acos(std::clamp(-world.character(c).up.y, -1.0f, 1.0f)); // away from the target (0, -1, 0)
            // The angle to the new up (0, -1, 0): pi at first, then 0.
            const f32 to_target = std::acos(std::clamp(-world.character(c).up.y, -1.0f, 1.0f));
            (void)angle;
            CHECK(to_target <= previous + 1e-4f);
            largest_turn = std::max(largest_turn, previous - to_target);
            previous = to_target;
            if (settled_step < 0 && to_target < 0.0175f) {
                settled_step = i;
            }
        }
        if (verbose) std::printf("up turned over: largest turn %.1f degrees a step, within 1 degree after %d steps\n",
                                 largest_turn * 57.29578f, settled_step);
        CHECK(largest_turn < 0.3f);
        CHECK(settled_step > 20 && settled_step < 90);
        const CharacterState3D s = world.character(c);
        CHECK(s.ground == CharacterGround3D::OnGround);
        CHECK(s.ground_user_id == 9);
        CHECK_NEAR(s.position.y, 6.0, 0.05);
        CHECK_NEAR(s.up.y, -1.0, 1e-3);
        // Walks on the ceiling at the walk speed, staying on it.
        world.set_character_input(c, forward_x());
        run(world, 120);
        const CharacterState3D w = world.character(c);
        CHECK_NEAR(w.ground_speed, 4.0, 0.05);
        CHECK_NEAR(w.position.y, 6.0, 0.05);
        CHECK(w.position.x > 6.0f);
    }
    // A wall: gravity along +X, and a wall at x = 10: it stands on the wall, feet at x = 10, up = -X, then walks along it.
    {
        World3D world(serial());
        add_static(world, {11.0f, 0, 0}, {1.0f, 50.0f, 50.0f}, {}, 8);
        World3D& w = world;
        w.set_gravity({9.81f, 0, 0});
        const Character3D c = make(w, {5.0f, 0, 0});
        run(w, 120);
        CharacterState3D s = w.character(c);
        CHECK(s.ground == CharacterGround3D::OnGround);
        CHECK_NEAR(s.position.x, 10.0, 0.05);
        CHECK_NEAR(s.up.x, -1.0, 1e-3);
        CharacterInput3D in;
        in.forward = {0.0f, 1.0f, 0.0f}; // along the wall's "up"
        in.move = {0.0f, 1.0f};
        w.set_character_input(c, in);
        run(w, 60);
        s = w.character(c);
        CHECK_NEAR(s.position.x, 10.0, 0.05);
        CHECK(s.position.y > 3.0f);
        CHECK_NEAR(s.ground_speed, 4.0, 0.05);
        // A jump goes along the wall's normal (away from it) and comes back.
        in.move = {};
        in.jump = true;
        w.set_character_input(c, in);
        f32 away = 0.0f;
        for (int i = 0; i < 60; ++i) {
            w.step(dt);
            in.jump = false;
            w.set_character_input(c, in);
            away = std::max(away, 10.0f - w.character(c).position.x);
        }
        CHECK_NEAR(away, 25.0 / (2.0 * 9.81), 0.08);
    }
    // An explicit up on a world with ordinary gravity: the character is held to its own (a planet's surface): up = +X
    // means gravity for it is toward -X.
    {
        World3D world(serial());
        add_static(world, {-1.0f, 0, 0}, {1.0f, 50.0f, 50.0f}, {}, 7);
        CharacterDesc3D d;
        d.up = {1.0f, 0.0f, 0.0f};
        const Character3D c = make(world, {5.0f, 0, 0}, d);
        run(world, 120);
        const CharacterState3D s = world.character(c);
        CHECK(s.ground == CharacterGround3D::OnGround);
        CHECK_NEAR(s.position.x, 0.0, 0.05);
        CHECK(s.ground_user_id == 7);
    }
}

void test_wall_friction() {
    // Falling pressed against a vertical wall: with no friction it falls as free fall; with friction 1 it is held back.
    const auto fall = [&](f32 friction, f32* tangential_speed) {
        World3D world(serial());
        add_floor(world);
        add_static(world, {1.0f, 20.0f, 0}, {0.5f, 40.0f, 5.0f}, {}, 3); // wall face at x = 0.5
        CharacterDesc3D d;
        d.wall_friction = friction;
        d.max_step_height = 0.0f;
        d.air_acceleration = 300.0f; // pressed hard against the wall: friction needs a normal force
        const Character3D c = make(world, {0.0f, 30.0f, 0}, d);
        world.set_character_input(c, forward_x());
        run(world, 30);
        const CharacterState3D s = world.character(c);
        *tangential_speed = -s.velocity.y;
        // Not into the wall: its x is held at the wall less the radius.
        return s.position.x;
    };
    f32 free_fall = 0.0f, held = 0.0f;
    const f32 x0 = fall(0.0f, &free_fall);
    const f32 x1 = fall(1.0f, &held);
    if (verbose) std::printf("wall: speed after 0.5 s free %.2f m/s (g t = 4.9), friction 1: %.2f\n", free_fall, held);
    CHECK_NEAR(free_fall, 9.81 * 0.5, 0.3);
    CHECK(held < 0.5f * free_fall);
    CHECK(x0 < 0.5f && x1 < 0.5f && x0 > 0.0f);

    // Sliding along a wall on the ground, zero friction: the speed along it is kept and the speed into it is killed.
    World3D world(serial());
    add_floor(world);
    add_static(world, {0.0f, 1.0f, 3.0f}, {50.0f, 1.0f, 0.5f}, {}, 4); // wall face at z = 2.5, along X
    const Character3D c = make(world, {0, 0, 0});
    run(world, 10);
    CharacterInput3D in;
    in.forward = {1.0f, 0.0f, 1.0f}; // diagonal: into the wall and along it
    in.move = {0.0f, 1.0f};
    world.set_character_input(c, in);
    run(world, 120);
    const CharacterState3D s = world.character(c);
    if (verbose) std::printf("slide: velocity %.2f %.2f %.2f, z %.2f\n", s.velocity.x, s.velocity.y, s.velocity.z, s.position.z);
    CHECK_NEAR(s.velocity.z, 0.0, 0.05);
    CHECK_NEAR(s.velocity.x, 4.0 / std::sqrt(2.0), 0.15);
    CHECK_NEAR(s.position.z, 2.5 - 0.3 - 0.02, 0.05);
    // And away from it again at once (no sticking): the step after the input turns, it moves off.
    in.forward = {0.0f, 0.0f, -1.0f};
    world.set_character_input(c, in);
    run(world, 3);
    CHECK(world.character(c).velocity.z < -0.5f);
}

void test_crouch_and_clearance() {
    // A roof slab with its underside at 1.35 m over x in [5, 9]. Standing is 1.8 m, crouching 1.0 m.
    World3D world(serial());
    add_floor(world);
    add_static(world, {7.0f, 1.45f, 0}, {2.0f, 0.1f, 3.0f}, {}, 12);
    CharacterDesc3D d;
    const Character3D c = make(world, {0, 0, 0}, d);
    run(world, 10);
    // Standing it cannot go under (it stops at the edge), crouching it can.
    world.set_character_input(c, forward_x());
    run(world, 120);
    CHECK(world.character(c).position.x < 5.0f - 0.3f + 0.05f);
    CHECK(!world.character(c).crouching);
    CharacterInput3D in = forward_x();
    in.crouch = true;
    world.set_character_input(c, in);
    run(world, 20);
    CHECK(world.character(c).crouching);
    CHECK_NEAR(world.character(c).height, 1.0, 1e-4);
    run(world, 100);
    CHECK(world.character(c).position.x > 6.0f); // under the slab
    CHECK_NEAR(world.character(c).ground_speed, 2.0, 0.1); // the crouch speed
    // Letting go of crouch under it: it stays crouched (blocked), and stands when it is out the far side.
    in.crouch = false;
    world.set_character_input(c, in);
    run(world, 5);
    CharacterState3D s = world.character(c);
    CHECK(s.crouching);
    CHECK(s.stand_blocked);
    CHECK(s.position.x > 5.5f && s.position.x < 9.0f);
    run(world, 150);
    s = world.character(c);
    CHECK(s.position.x > 9.5f);
    CHECK(!s.crouching && !s.stand_blocked);
    CHECK_NEAR(s.height, 1.8, 1e-4);
    // The explicit call: refused under a ceiling, and the capsule shape is the crouched one while it is.
    world.teleport_character(c, {7.0f, 0, 0});
    CHECK(world.set_character_crouch(c, true));
    CHECK(!world.set_character_crouch(c, false));
    CHECK(world.character(c).crouching);
    // The body's shape follows: a ray at 1.2 m above the feet passes under the roof's slab at 1.35 and does not hit the
    // crouched capsule's top (1.0 m).
    const auto top = world.raycast({7.0f, 1.2f, -2.0f}, {7.0f, 1.2f, 2.0f}, {.mask = layer_bit(layers3d::Character)});
    CHECK(!top.has_value());
    const auto side = world.raycast({7.0f, 0.5f, -2.0f}, {7.0f, 0.5f, 2.0f}, {.mask = layer_bit(layers3d::Character)});
    CHECK(side.has_value());
    world.teleport_character(c, {20.0f, 0, 0});
    CHECK(world.set_character_crouch(c, false));
    CHECK(!world.character(c).crouching);
}

void test_events() {
    World3D world(serial());
    add_floor(world);
    add_static(world, {10.0f, 0.5f, 0}, {1.0f, 0.5f, 3.0f}, {}, 44); // a slab 1 m high, x in [9, 11]
    CharacterDesc3D d;
    d.user_id = 4242;
    // Dropped from 3 m onto the slab: it lands at sqrt(2 g h) = sqrt(2 x 9.81 x 2).
    const Character3D c = make(world, {10.0f, 3.0f, 0}, d);
    std::vector<CharacterEvent3D> all;
    const auto gather = [&] {
        for (const CharacterEvent3D& e : world.drain_character_events()) {
            all.push_back(e);
        }
    };
    const auto of = [&](CharacterEventKind3D kind) {
        std::vector<CharacterEvent3D> out;
        for (const CharacterEvent3D& e : all) {
            if (e.kind == kind) out.push_back(e);
        }
        return out;
    };
    run(world, 60);
    gather();
    auto landed = of(CharacterEventKind3D::Landed);
    CHECK(landed.size() == 1);
    if (!landed.empty()) {
        if (verbose) std::printf("landed at %.2f m/s (sqrt(2 g h) = %.2f)\n", landed[0].speed, std::sqrt(2.0f * 9.81f * 2.0f));
        CHECK_NEAR(landed[0].speed, std::sqrt(2.0f * 9.81f * 2.0f), 0.3);
        CHECK(landed[0].user_id == 4242 && landed[0].character == c);
        CHECK(landed[0].body_user_id == 44);
        CHECK(landed[0].normal.y > 0.9f);
    }
    CHECK(of(CharacterEventKind3D::LeftGround).empty());
    // Within the step: Landed comes before ContactBegin (the order of the kinds).
    {
        std::size_t landed_at = 0, begin_at = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].kind == CharacterEventKind3D::Landed) landed_at = i;
            if (all[i].kind == CharacterEventKind3D::ContactBegin) begin_at = i;
        }
        CHECK(of(CharacterEventKind3D::ContactBegin).size() == 1 && landed_at < begin_at);
    }
    // A jump: LeftGround with `jumped` at 5 m/s, then Landed again.
    all.clear();
    CharacterInput3D in;
    in.jump = true;
    world.set_character_input(c, in);
    run(world, 1);
    in.jump = false;
    world.set_character_input(c, in);
    run(world, 80);
    gather();
    const auto left = of(CharacterEventKind3D::LeftGround);
    CHECK(left.size() == 1 && left[0].jumped && near(left[0].speed, 5.0f, 0.3f));
    CHECK(of(CharacterEventKind3D::Landed).size() == 1);
    // Walk off the slab's far side (a drop of 1 m: more than the stick-to-ground distance): LeftGround without `jumped`,
    // ContactEnd with the slab's user id, Landed on the floor (user id 1) with the speed of a 1 m fall.
    all.clear();
    world.set_character_input(c, forward_x());
    run(world, 120);
    gather();
    bool end_44 = false, begin_floor = false;
    for (const CharacterEvent3D& e : all) {
        end_44 = end_44 || (e.kind == CharacterEventKind3D::ContactEnd && e.body_user_id == 44);
        begin_floor = begin_floor || (e.kind == CharacterEventKind3D::ContactBegin && e.body_user_id == 1);
    }
    const auto left_walk = of(CharacterEventKind3D::LeftGround);
    CHECK(left_walk.size() == 1 && !left_walk[0].jumped);
    CHECK(end_44);
    CHECK(begin_floor);
    landed = of(CharacterEventKind3D::Landed);
    CHECK(landed.size() == 1 && landed[0].body_user_id == 1);
    if (!landed.empty()) {
        CHECK_NEAR(landed[0].speed, std::sqrt(2.0f * 9.81f * 1.0f), 0.5);
    }
    CHECK(world.character_events().empty());
}

void test_coyote_and_buffer() {
    // An edge at x = 3: the floor ends there (a slab 0..3), nothing below.
    const auto edge_jump = [&](f32 coyote, f32 press_after_s) {
        World3D world(serial());
        add_static(world, {-2.0f, -0.5f, 0}, {5.0f, 0.5f, 5.0f});
        CharacterDesc3D d;
        d.coyote_seconds = coyote;
        const Character3D c = make(world, {0, 0, 0}, d);
        run(world, 10);
        world.set_character_input(c, forward_x());
        // Walk until the feet are off the edge, then count the time.
        int off_at = -1;
        bool pressed = false;
        f32 best_up = -100.0f;
        for (int i = 0; i < 240; ++i) {
            world.step(dt);
            const CharacterState3D s = world.character(c);
            if (off_at < 0 && s.ground != CharacterGround3D::OnGround) {
                off_at = i;
            }
            if (off_at >= 0 && !pressed && static_cast<f32>(i - off_at) * dt >= press_after_s) {
                CharacterInput3D in = forward_x();
                in.jump = true;
                world.set_character_input(c, in);
                pressed = true;
            }
            if (pressed) {
                best_up = std::max(best_up, s.vertical_speed);
            }
        }
        return best_up;
    };
    CHECK(edge_jump(0.0f, 0.1f) < 0.0f);   // too late with no coyote time: no jump
    CHECK(edge_jump(0.2f, 0.1f) > 4.0f);   // inside it: jumps
    CHECK(edge_jump(0.2f, 0.4f) < 0.0f);   // after it: no

    // Jump buffer: pressed 0.1 s before landing.
    const auto buffered = [&](f32 buffer) {
        World3D world(serial());
        add_floor(world);
        const Character3D c = make(world, {0, 0.6f, 0});
        // Falling from 0.6: lands after about 0.35 s. Press at 0.25 s.
        f32 best_up = -100.0f;
        for (int i = 0; i < 90; ++i) {
            if (i == 15) {
                CharacterDesc3D d = world.character(c).height > 0 ? CharacterDesc3D{} : CharacterDesc3D{};
                (void)d;
                CharacterInput3D in;
                in.jump = true;
                world.set_character_input(c, in);
            }
            world.step(dt);
            if (i >= 15) {
                best_up = std::max(best_up, world.character(c).vertical_speed);
            }
        }
        (void)buffer;
        return best_up;
    };
    CHECK(buffered(0.0f) < 1.0f);
    // With the option: made through the descriptor.
    {
        World3D world(serial());
        add_floor(world);
        CharacterDesc3D d;
        d.jump_buffer_seconds = 0.2f;
        const Character3D c = make(world, {0, 0.6f, 0}, d);
        f32 best_up = -100.0f;
        for (int i = 0; i < 90; ++i) {
            if (i == 15) {
                CharacterInput3D in;
                in.jump = true;
                world.set_character_input(c, in);
            }
            world.step(dt);
            if (i >= 15) {
                best_up = std::max(best_up, world.character(c).vertical_speed);
            }
        }
        CHECK(best_up > 4.0f);
    }
}

void test_handles_and_bodies() {
    World3D world(serial());
    add_floor(world);
    const i32 bodies_before = world.body_count();
    CharacterDesc3D bad;
    bad.radius = 0.0f;
    CHECK(!world.create_character(bad).valid());
    bad = {};
    bad.height = 0.4f; // under 2 radii
    CHECK(!world.create_character(bad).valid());
    bad = {};
    bad.position = {std::nanf(""), 0, 0};
    CHECK(!world.create_character(bad).valid());
    bad = {};
    bad.layer = layers3d::Static;
    CHECK(!world.create_character(bad).valid());
    CHECK(world.character_count() == 0 && world.body_count() == bodies_before);

    const Character3D a = make(world, {0, 0, 0});
    const Character3D b = make(world, {3, 0, 0});
    CHECK(world.character_count() == 2 && world.body_count() == bodies_before + 2);
    CHECK(a.index == 0 && b.index == 1);
    const Body3D inner = world.character(a).body;
    CHECK(world.contains(inner));
    // Its body is its own: the body calls refuse it.
    world.set_position(inner, {50, 50, 50});
    world.destroy_body(inner);
    CHECK(world.contains(inner));
    run(world, 5);
    CHECK_NEAR(world.character(a).position.y, 0.0, 0.03);
    // Destroying the character takes the body; the handle is dead; the number is reused with a new generation.
    world.destroy_character(a);
    CHECK(!world.contains(a) && !world.contains(inner) && world.character_count() == 1);
    CHECK(world.body_count() == bodies_before + 1);
    CHECK(world.character(a).body == Body3D{});
    const Character3D again = make(world, {6, 0, 0});
    CHECK(again.index == a.index && again.generation != a.generation);
    CHECK(world.characters().size() == 2 && world.characters()[0] == again && world.characters()[1] == b);
    // Dead handles are inert.
    world.set_character_input(a, forward_x());
    world.teleport_character(a, {1, 1, 1});
    CHECK(!world.set_character_crouch(a, true));
    // Destroying the world with characters in it is clean (this scope's end).
}

void test_teleport() {
    World3D world(serial());
    add_floor(world);
    const Character3D c = make(world, {0, 0, 0});
    run(world, 10);
    world.set_character_input(c, forward_x());
    run(world, 30);
    world.teleport_character(c, {20.0f, 3.0f, 5.0f});
    CharacterState3D s = world.character(c);
    CHECK_NEAR(s.position.x, 20.0, 1e-4);
    CHECK_NEAR(length(s.velocity), 0.0, 1e-4);
    CHECK_NEAR(world.position(s.body).x, 20.0, 0.01);
    world.set_character_input(c, {});
    run(world, 60);
    s = world.character(c);
    CHECK(s.ground == CharacterGround3D::OnGround);
    CHECK_NEAR(s.position.y, 0.0, 0.03);
    CHECK_NEAR(s.position.x, 20.0, 0.01);
}

void test_character_order() {
    // Two characters walking toward each other: the one with the lower number moves first, so it gets the room; swap
    // the creation order and the other wins. (A step in the other order would flip each case.)
    const auto meet = [&](bool swap) {
        World3D world(serial());
        add_floor(world);
        CharacterInput3D left_in = forward_x();
        CharacterInput3D right_in = forward_x();
        right_in.forward = {-1.0f, 0.0f, 0.0f};
        Character3D left, right;
        if (!swap) {
            left = make(world, {-3.0f, 0, 0});
            right = make(world, {3.0f, 0, 0});
        } else {
            right = make(world, {3.0f, 0, 0});
            left = make(world, {-3.0f, 0, 0});
        }
        run(world, 5);
        world.set_character_input(left, left_in);
        world.set_character_input(right, right_in);
        run(world, 120);
        return std::pair{world.character(left).position.x, world.character(right).position.x};
    };
    const auto [lx, rx] = meet(false);
    const auto [lx2, rx2] = meet(true);
    if (verbose) std::printf("meeting: lower number first: left %.3f right %.3f; swapped: left %.3f right %.3f\n", lx, rx, lx2, rx2);
    // They stop each other; the first mover ends up the nearer to the middle by a hair, deterministically.
    CHECK(lx < rx && lx2 < rx2);
    CHECK(std::fabs(lx) < std::fabs(rx));    // the lower number is nearer the middle
    CHECK(std::fabs(rx2) < std::fabs(lx2));  // and when the right one was made first, it is
}

// Reads a file whole (the route dump below prints where the character is, every N frames: `-r <scene.json> N`, to write
// checkpoints from).
std::string read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::string text;
    if (f == nullptr) {
        return text;
    }
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, f)) > 0) {
        text.append(buffer, n);
    }
    std::fclose(f);
    return text;
}

void dump_route(const std::string& path, int every) {
    const JsonParseResult parsed = parse_json(read_file(path));
    if (!parsed.ok()) {
        std::printf("no route: %s\n", parsed.error.c_str());
        return;
    }
    const JsonValue* c = parsed.value->find("character");
    const character_level::Route route = character_level::load_route(*c);
    World3D world(serial());
    const character_level::Level level = character_level::build(world);
    world.optimize_broad_phase();
    const Character3D hero = make(world, route.start);
    for (int n = 0; n < route.frames; ++n) {
        character_level::apply_keys(world, hero, route, n);
        level.drive(world, n, dt);
        world.step(dt);
        if (n % every == 0) {
            const CharacterState3D s = world.character(hero);
            std::printf("%4d  pos (%7.3f %6.3f %7.3f)  v (%5.2f %5.2f %5.2f) ground %d speed %.2f crouch %d  ground body %llu\n", n, s.position.x,
                        s.position.y, s.position.z, s.velocity.x, s.velocity.y, s.velocity.z, int(s.ground), s.ground_speed, s.crouching,
                        static_cast<unsigned long long>(s.ground_user_id));
        }
    }
}


// --- the level, the route, determinism ---------------------------------------------------------------------------------

struct RouteRun {
    World3D world;
    character_level::Level level;
    character_level::Route route;
    Character3D hero;
    int frame = 0;

    RouteRun(const World3DConfig& cfg, const character_level::Route& r)
        : world(cfg)
        , route(r) {
        level = character_level::build(world);
        world.optimize_broad_phase();
        CharacterDesc3D d;
        d.position = route.start;
        d.height = 1.75f;
        hero = world.create_character(d);
    }
    void step() {
        character_level::apply_keys(world, hero, route, frame);
        level.drive(world, frame, dt);
        world.step(dt);
        ++frame;
    }
};

character_level::Route load_scene_route() {
    const std::string text = read_file(std::string(KIN_TEST_REFERENCE_SCENES) + "/physics_character.json");
    const JsonParseResult parsed = parse_json(text);
    CHECK(parsed.ok());
    if (!parsed.ok()) {
        return {};
    }
    const JsonValue* c = parsed.value->find("character");
    CHECK(c != nullptr);
    return c != nullptr ? character_level::load_route(*c) : character_level::Route{};
}

void test_scripted_route() {
    const character_level::Route route = load_scene_route();
    CHECK(route.frames > 1000 && route.keys.size() > 10 && route.checkpoints.size() > 15);
    RouteRun run_(serial(), route);
    CHECK(run_.world.body_count() > 20);
    std::size_t next = 0;
    std::vector<character_level::Checkpoint> by_frame = route.checkpoints;
    std::ranges::sort(by_frame, {}, &character_level::Checkpoint::frame);
    int sensor_begin = 0, sensor_end = 0, landed = 0, head_hits = 0;
    const f32 box_start = run_.world.position(run_.level.light_box).x;
    const f32 heavy_start = run_.world.position(run_.level.heavy_box).x;
    for (int n = 0; n < route.frames; ++n) {
        run_.step();
        for (const CharacterEvent3D& e : run_.world.drain_character_events()) {
            sensor_begin += e.kind == CharacterEventKind3D::ContactBegin && e.sensor && e.body_user_id == run_.world.user_id(run_.level.sensor);
            sensor_end += e.kind == CharacterEventKind3D::ContactEnd && e.sensor;
            landed += e.kind == CharacterEventKind3D::Landed;
            head_hits += e.kind == CharacterEventKind3D::HitHead;
        }
        // The checkpoint of this frame: the feet in its box (frame f is checked after step f has run).
        while (next < by_frame.size() && by_frame[next].frame == n) {
            const character_level::Checkpoint& cp = by_frame[next++];
            const CharacterState3D s = run_.world.character(run_.hero);
            const bool in_box = s.position.x >= cp.lo.x && s.position.x <= cp.hi.x && s.position.y >= cp.lo.y &&
                                s.position.y <= cp.hi.y && s.position.z >= cp.lo.z && s.position.z <= cp.hi.z;
            const bool stance = !cp.has_crouching || s.crouching == cp.crouching;
            if (!in_box || !stance) {
                ++failures;
                std::fprintf(stderr, "physics3d_character_tests: checkpoint at frame %d (%s): feet (%.3f %.3f %.3f), box (%.3f %.3f %.3f)-(%.3f %.3f %.3f)%s\n",
                             cp.frame, cp.what.c_str(), static_cast<double>(s.position.x), static_cast<double>(s.position.y),
                             static_cast<double>(s.position.z), static_cast<double>(cp.lo.x), static_cast<double>(cp.lo.y),
                             static_cast<double>(cp.lo.z), static_cast<double>(cp.hi.x), static_cast<double>(cp.hi.y),
                             static_cast<double>(cp.hi.z), stance ? "" : " (stance differs)");
            } else if (verbose) {
                std::printf("  checkpoint %4d ok: %s\n", cp.frame, cp.what.c_str());
            }
        }
    }
    CHECK(next == by_frame.size());
    // Beyond positions: the light box was pushed far, the heavy one stayed, the trigger was entered and left once, it
    // landed several times (the jump, the teleports onto platforms) and never hit its head (it crouched under the roof).
    CHECK(run_.world.position(run_.level.light_box).x - box_start > 5.0f);
    CHECK(std::fabs(run_.world.position(run_.level.heavy_box).x - heavy_start) < 0.05f);
    CHECK(sensor_begin == 1 && sensor_end == 1);
    CHECK(landed >= 4);
    CHECK(head_hits == 0);
}

// Hashes of every step of a scene: the level, a few boxes thrown about, five characters with seeded commands.
std::vector<u64> scene_hashes(const World3DConfig& cfg, int frames) {
    World3D world(cfg);
    const character_level::Level level = character_level::build(world);
    for (int i = 0; i < 20; ++i) {
        world.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}),
                           .position = {20.0f + static_cast<f32>(i % 5), 2.0f + static_cast<f32>(i / 5), 1.0f},
                           .density = 300.0f, .user_id = 900u + static_cast<u64>(i)});
    }
    world.optimize_broad_phase();
    std::vector<Character3D> heroes;
    const Vec3f starts[] = {{1, 0, 0}, {8, 0, 1}, {12, 3, 0}, {30, 0, 0}, {20, 0.5f, 0}};
    for (const Vec3f& at : starts) {
        CharacterDesc3D d;
        d.position = at;
        d.coyote_seconds = 0.1f;
        heroes.push_back(world.create_character(d));
    }
    std::vector<u64> out;
    u32 seed = 4242;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < heroes.size(); ++i) {
            seed = seed * 1664525u + 1013904223u;
            const f32 a = static_cast<f32>(seed >> 8) / static_cast<f32>(1u << 24);
            seed = seed * 1664525u + 1013904223u;
            const f32 b = static_cast<f32>(seed >> 8) / static_cast<f32>(1u << 24);
            CharacterInput3D in;
            in.move = {a * 2.0f - 1.0f, 0.4f + 0.6f * b};
            in.forward = {1.0f, 0.0f, 0.0f};
            in.jump = (f / 40 + static_cast<int>(i)) % 3 == 0 && f % 40 < 3;
            in.sprint = ((f / 90) % 2 == 0);
            in.crouch = ((f / 70 + static_cast<int>(i)) % 4 == 0);
            world.set_character_input(heroes[i], in);
        }
        level.drive(world, f, dt);
        world.step(dt);
        out.push_back(world.state_hash());
    }
    return out;
}

void test_determinism() {
    constexpr int frames = 240;
    const std::vector<u64> serial_run = scene_hashes(serial(), frames);
    CHECK(serial_run == scene_hashes(serial(), frames));
    CHECK(serial_run.front() != serial_run[frames / 2] && serial_run[frames / 2] != serial_run.back());
    {
        JobSystem one({.workers = 1, .max_background = 1});
        World3DConfig cfg;
        cfg.jobs = &one;
        CHECK(scene_hashes(cfg, frames) == serial_run);
    }
    {
        JobSystem four({.workers = 4, .max_background = 4});
        World3DConfig cfg;
        cfg.jobs = &four;
        CHECK(scene_hashes(cfg, frames) == serial_run);
        CHECK(scene_hashes(cfg, frames) == serial_run);
    }
}

// A state saved mid-run and restored (in the same world and in another one made the same way) repeats the trajectory.
void test_save_restore_characters() {
    const auto build = [](World3D& world, std::vector<Character3D>& heroes) {
        add_floor(world);
        add_static(world, {6.0f, 0.5f, 0}, {1.0f, 0.5f, 3.0f});
        world.create_body({.shape = Shape3D::box({0.4f, 0.4f, 0.4f}), .position = {4.0f, 0.4f, 1.0f}, .mass = 30.0f, .user_id = 7});
        for (int i = 0; i < 2; ++i) {
            CharacterDesc3D d;
            d.position = {static_cast<f32>(i) * 2.0f, 0, static_cast<f32>(i) - 0.5f};
            heroes.push_back(world.create_character(d));
        }
        world.optimize_broad_phase();
    };
    const auto command = [](World3D& world, const std::vector<Character3D>& heroes, int f) {
        for (std::size_t i = 0; i < heroes.size(); ++i) {
            CharacterInput3D in;
            in.move = {0.2f * static_cast<f32>(i), 1.0f};
            in.forward = {1.0f, 0.0f, 0.0f};
            in.jump = f % 50 == 10 * static_cast<int>(i + 1);
            in.crouch = f > 60 && f < 90 && i == 1;
            in.sprint = f > 20 && f < 40;
            world.set_character_input(heroes[i], in);
        }
    };
    World3D world(serial());
    std::vector<Character3D> heroes;
    build(world, heroes);
    for (int f = 0; f < 80; ++f) {
        command(world, heroes, f);
        world.step(dt);
    }
    const std::vector<u8> saved = world.save_state();
    const u64 at_save = world.state_hash();
    // The crouch is on at the save (hero 1, frames 60 to 90): a restore has to put the shape back.
    CHECK(world.character(heroes[1]).crouching);
    std::vector<u64> first;
    for (int f = 80; f < 200; ++f) {
        command(world, heroes, f);
        world.step(dt);
        first.push_back(world.state_hash());
    }
    CHECK(!world.character(heroes[1]).crouching);
    CHECK(world.restore_state(saved));
    CHECK(world.state_hash() == at_save);
    CHECK(world.character(heroes[1]).crouching);
    std::vector<u64> second;
    for (int f = 80; f < 200; ++f) {
        command(world, heroes, f);
        world.step(dt);
        second.push_back(world.state_hash());
    }
    CHECK(first == second);
    // Another world made the same way takes the state and repeats it.
    World3D other(serial());
    std::vector<Character3D> other_heroes;
    build(other, other_heroes);
    for (int f = 0; f < 5; ++f) {
        other.step(dt); // somewhere else entirely
    }
    CHECK(other.restore_state(saved));
    CHECK(other.state_hash() == at_save);
    std::vector<u64> third;
    for (int f = 80; f < 200; ++f) {
        command(other, other_heroes, f);
        other.step(dt);
        third.push_back(other.state_hash());
    }
    CHECK(third == first);
    // A world with another set of characters refuses it and is left as it was.
    World3D fewer(serial());
    {
        std::vector<Character3D> h;
        build(fewer, h);
        fewer.destroy_character(h[1]);
    }
    const u64 before = fewer.state_hash();
    CHECK(!fewer.restore_state(saved));
    CHECK(fewer.state_hash() == before);
}

// The hash sees what the bodies' places do not: the command, the up vector's target, the held jump, one more character.
void test_hash_sees_characters() {
    const auto make_world = [](World3D& world) {
        add_floor(world);
        CharacterDesc3D d;
        d.coyote_seconds = 0.2f;
        const Character3D c = world.create_character(d);
        for (int i = 0; i < 30; ++i) {
            world.step(dt);
        }
        return c;
    };
    World3D a(serial()), b(serial());
    const Character3D ca = make_world(a);
    const Character3D cb = make_world(b);
    CHECK(a.state_hash() == b.state_hash());
    // The command alone (no step between): the places are the same, the hash differs.
    CharacterInput3D in;
    in.move = {0.0f, 1.0f};
    b.set_character_input(cb, in);
    CHECK(a.state_hash() != b.state_hash());
    a.set_character_input(ca, in);
    CHECK(a.state_hash() == b.state_hash());
    // The up target (explicit, not yet turned to).
    b.set_character_up(cb, {0.0f, 1.0f, 0.001f});
    CHECK(a.state_hash() != b.state_hash());
    a.set_character_up(ca, {0.0f, 1.0f, 0.001f});
    CHECK(a.state_hash() == b.state_hash());
    // A character that exists is in the hash: one more, anywhere, changes it.
    const u64 before = a.state_hash();
    CharacterDesc3D extra;
    extra.position = {10, 0, 10};
    a.create_character(extra);
    CHECK(a.state_hash() != before);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "-r") { dump_route(argv[2], argc > 3 ? std::atoi(argv[3]) : 15); return 0; }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-v") verbose = true;
    }
    test_stand_and_rest();
    test_walk_speed_and_stop();
    test_slopes();
    test_camera_frame();
    test_stairs_and_ledges();
    test_stick_to_ground();
    test_jump();
    test_tunnelling();
    test_moving_platforms();
    test_pushing_boxes();
    test_standing_on_dynamic();
    test_ceiling_bump();
    test_sensors();
    test_layers();
    test_gravity_direction();
    test_wall_friction();
    test_crouch_and_clearance();
    test_events();
    test_coyote_and_buffer();
    test_handles_and_bodies();
    test_teleport();
    test_character_order();
    test_hash_sees_characters();
    test_determinism();
    test_save_restore_characters();
    test_scripted_route();
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_character_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_character_tests: all passed\n");
    return 0;
}
