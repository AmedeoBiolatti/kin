// Ropes of kin/physics3d (kin/physics3d/rope3d.hpp), without a GPU: length under load, anchors in the world and on moving
// bodies, a bridge that carries a box, tension against weight, pay-out and reel, breaking, sampling, and a character that grabs
// a rope, hangs from it with its weight on it, climbs, swings and lets go. docs/physics_3d.md ("Ropes").

#include <kin/core/jobs.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/physics3d/character3d.hpp>
#include <kin/physics3d/traversal3d.hpp>

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

#define CHECK(cond)                                                                               \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            ++failures;                                                                           \
            std::fprintf(stderr, "physics3d_rope_tests:%d: CHECK failed: %s\n", __LINE__, #cond); \
        }                                                                                         \
    } while (0)

#define CHECK_NEAR(value, expected, eps)                                                                              \
    do {                                                                                                              \
        const double v_ = static_cast<double>(value), e_ = static_cast<double>(expected);                             \
        if (verbose) std::printf("  %s = %.4f (expected %.4f +- %.4f)\n", #value, v_, e_, static_cast<double>(eps));    \
        if (!(std::fabs(v_ - e_) <= static_cast<double>(eps))) {                                                      \
            ++failures;                                                                                               \
            std::fprintf(stderr, "physics3d_rope_tests:%d: %s = %.5f, expected %.5f +- %.5f\n", __LINE__, #value, v_, \
                         e_, static_cast<double>(eps));                                                               \
        }                                                                                                             \
    } while (0)

constexpr f32 dt = 1.0f / 60.0f;

World3DConfig serial() {
    World3DConfig c;
    c.single_threaded = true;
    return c;
}

Body3D add_floor(World3D& w) {
    return w.create_body({.shape = Shape3D::box({30.0f, 0.5f, 30.0f}), .motion = Body3DMotion::Static,
                          .position = {0.0f, -0.5f, 0.0f}, .friction = 0.6f, .user_id = 1});
}

// A rope hanging from a world point `top` with a free end, `length` long.
RopeDesc3D hanging(Vec3f top, f32 length) {
    RopeDesc3D d;
    d.start = RopeAnchor3D::at(top);
    d.end = RopeAnchor3D::free();
    d.length = length;
    return d;
}

// ---- making and sampling ------------------------------------------------------------------------------------------------

void test_create_and_sample() {
    World3D world(serial());
    add_floor(world);
    const RopeHandle3D rope = world.create_rope(hanging({0.0f, 6.0f, 0.0f}, 4.0f));
    CHECK(rope.valid() && world.contains(rope) && world.rope_count() == 1);
    // 16 segments of 25 cm; the start is fixed (a constraint), the 15 joints between them.
    const RopeState3D st = world.rope_state(rope);
    CHECK(st.segments == 16);
    CHECK_NEAR(st.rest_length, 4.0f, 1e-5f);
    CHECK_NEAR(st.current_length, 4.0f, 1e-3f);
    CHECK(world.body_count() == 17);
    CHECK(world.constraint_count() == 16);
    // Hangs straight down from the point, in order from the top.
    const std::vector<RopePoint3D> pts = world.rope_points(rope);
    CHECK(pts.size() == 17);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        CHECK_NEAR(pts[i].position.y, 6.0f - 0.25f * static_cast<f32>(i), 1e-3f);
        CHECK_NEAR(length(pts[i].tangent), 1.0f, 1e-4f);
        CHECK_NEAR(pts[i].tangent.y, -1.0f, 1e-3f);
    }
    const std::vector<RopeSegment3D> segs = world.rope_segments(rope);
    CHECK(segs.size() == 16 && segs[0].body.valid());
    CHECK(world.rope_segment_of(segs[5].body).has_value() && world.rope_segment_of(segs[5].body)->segment == 5);
    CHECK(!world.rope_segment_of(Body3D{0, 0}).has_value());
    // The closest point to a place: on segment 7 (y from 4.25 to 4.0) at 0.6 of it, 0.5 away.
    const auto c = world.rope_closest_point(rope, {0.5f, 4.1f, 0.0f});
    CHECK(c.has_value() && c->segment == 7);
    CHECK_NEAR(c->t, 0.6f, 0.02f);
    CHECK_NEAR(c->distance, 0.5f, 1e-3f);
    CHECK_NEAR(c->along, (7.0f + c->t) / 16.0f, 1e-5f);
    // Refused: no length for a free end, a body anchor that is not a body, a non-finite point.
    RopeDesc3D bad;
    bad.start = RopeAnchor3D::at({0, 5, 0});
    CHECK(!world.create_rope(bad).valid());
    bad.end = RopeAnchor3D::on(Body3D{99, 0});
    bad.length = 3.0f;
    CHECK(!world.create_rope(bad).valid());
    RopeDesc3D nan = hanging({std::nanf(""), 1.0f, 0.0f}, 2.0f);
    CHECK(!world.create_rope(nan).valid());
    CHECK(world.rope_count() == 1 && world.body_count() == 17);
    // Destroyed: bodies and joints go.
    world.destroy_rope(rope);
    CHECK(!world.contains(rope) && world.body_count() == 1 && world.constraint_count() == 0);
}

void test_hanging_rope_settles() {
    World3D world(serial());
    // Made lying 45 degrees out from the vertical and let go: it swings, keeps its length, and comes to rest below the anchor.
    RopeDesc3D d = hanging({1.0f, 6.0f, 0.0f}, 4.0f);
    d.hang_direction = normalize(Vec3f{1.0f, -1.0f, 0.0f});
    d.angular_damping = 2.0f;
    d.linear_damping = 0.3f;
    const RopeHandle3D rope = world.create_rope(d);
    CHECK_NEAR(world.rope_points(rope).back().position.x, 1.0f + 4.0f * 0.70711f, 0.01f);
    f32 stretch = 0.0f;
    for (int i = 0; i < 1800; ++i) {
        world.step(dt);
        stretch = std::max(stretch, world.rope_state(rope).current_length / 4.0f - 1.0f);
    }
    CHECK(stretch < 0.05f);
    const std::vector<RopePoint3D> pts = world.rope_points(rope);
    CHECK_NEAR(pts.back().position.x, 1.0f, 0.1f);
    CHECK_NEAR(pts.back().position.y, 2.0f, 0.1f);
}

// ---- loads ---------------------------------------------------------------------------------------------------------------

void test_tension_equals_weight_and_length_bound() {
    for (const f32 mass : {10.0f, 50.0f, 150.0f}) {
        World3D world(serial());
        const Body3D box = world.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .motion = Body3DMotion::Dynamic,
                                              .position = {0.0f, 1.7f, 0.0f}, .mass = mass});
        RopeDesc3D d;
        d.start = RopeAnchor3D::at({0.0f, 6.0f, 0.0f});
        d.end = RopeAnchor3D::on(box, {0.0f, 0.3f, 0.0f});
        const RopeHandle3D rope = world.create_rope(d);
        CHECK(rope.valid());
        world.step(dt);
        const f32 length0 = world.rope_state(rope).rest_length;
        CHECK_NEAR(length0, 4.0f, 0.02f);
        f32 stretch = 0.0f;
        for (int i = 0; i < 600; ++i) {
            world.step(dt);
            stretch = std::max(stretch, world.rope_state(rope).current_length / length0 - 1.0f);
        }
        // The rope's own mass: 1 kg a metre.
        const f32 weight = (mass + 4.0f) * 9.81f;
        const f32 tension = world.rope_tension(rope, RopeEnd3D::Start);
        const f32 at_box = world.rope_tension(rope, RopeEnd3D::End);
        if (verbose) std::printf("  load %.0f kg: tension %.1f / %.1f N (weight %.1f), stretch %.4f\n", mass, tension, at_box, weight, stretch);
        CHECK_NEAR(tension, weight, 0.10f * weight);
        CHECK_NEAR(at_box, mass * 9.81f, 0.10f * mass * 9.81f);
        CHECK(stretch < 0.10f);
        // Settled: within 5 % of the rest length.
        CHECK(world.rope_state(rope).current_length / length0 - 1.0f < 0.05f);
        // The anchor holds: the first joint is where it was put.
        CHECK_NEAR(world.rope_points(rope).front().position.y, 6.0f, 0.01f);
    }
}

void test_anchor_on_a_moving_body() {
    // The start is on a kinematic carriage moving along X at 1 m/s, a point of its own space (0.5 up and 0.3 forward); the
    // end hangs a 20 kg box. The joint follows the carriage's point, the box follows the rope.
    World3D world(serial());
    add_floor(world);
    const Body3D carriage = world.create_body({.shape = Shape3D::box({0.5f, 0.2f, 0.5f}), .motion = Body3DMotion::Kinematic,
                                               .position = {0.0f, 6.0f, 0.0f}});
    const Body3D box = world.create_body({.shape = Shape3D::box({0.25f, 0.25f, 0.25f}), .motion = Body3DMotion::Dynamic,
                                          .position = {0.0f, 1.5f, 0.3f}, .mass = 20.0f});
    RopeDesc3D d;
    d.start = RopeAnchor3D::on(carriage, {0.0f, -0.2f, 0.3f});
    d.end = RopeAnchor3D::on(box, {0.0f, 0.25f, 0.0f});
    const RopeHandle3D rope = world.create_rope(d);
    CHECK(rope.valid());
    // A turned carriage too: the offset is in the body's own space.
    for (int i = 1; i <= 240; ++i) {
        const f32 t = static_cast<f32>(i) * dt;
        const Quatf turn = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 20.0f * t);
        world.move_kinematic(carriage, {t, 6.0f, 0.0f}, turn, dt);
        world.step(dt);
    }
    const Vec3f carriage_at = world.position(carriage);
    const Vec3f want = carriage_at + world.rotation(carriage).rotate({0.0f, -0.2f, 0.3f});
    CHECK(length(world.rope_points(rope).front().position - want) < 0.03f);
    // Carried: the box is under it, near the line of the carriage's travel.
    CHECK_NEAR(world.position(box).x, carriage_at.x, 1.2f);
    CHECK(world.position(box).y > 1.0f);
}

void test_bridge_carries_a_box() {
    // Three ropes between two posts 6 m apart, 6.3 m long, over a pit; a 30 kg plank-like box set across them stays on them (one
    // rope alone would roll it off, as in life, and two let it slide off sideways).
    World3D world(serial());
    for (const f32 x : {-3.0f, 3.0f}) {
        world.create_body({.shape = Shape3D::box({0.2f, 2.0f, 1.0f}), .motion = Body3DMotion::Static, .position = {x, 2.0f, 0.0f}});
    }
    // Low walls along the sides (the ropes have no twist stiffness: a box on them rolls them like rollers, and walks off
    // sideways, slowly; the walls are what a real bridge's boards and side ropes are).
    for (const f32 z : {-1.0f, 1.0f}) {
        world.create_body({.shape = Shape3D::box({3.0f, 0.6f, 0.05f}), .motion = Body3DMotion::Static, .position = {0.0f, 3.2f, z},
                           .friction = 0.0f});
    }
    RopeHandle3D rails[3];
    for (int k = 0; k < 3; ++k) {
        const f32 z = 0.5f * static_cast<f32>(k - 1);
        RopeDesc3D d;
        d.start = RopeAnchor3D::at({-3.0f, 3.8f, z});
        d.end = RopeAnchor3D::at({3.0f, 3.8f, z});
        d.length = 6.3f;
        d.radius = 0.05f;
        d.mass_per_metre = 2.0f;
        rails[k] = world.create_rope(d);
        CHECK(rails[k].valid());
    }
    for (int i = 0; i < 240; ++i) {
        world.step(dt);
    }
    const auto mid = [&](int k) {
        const RopeHandle3D r = rails[k];
        return world.rope_points(r)[world.rope_state(r).segments / 2].position.y;
    };
    const f32 sag = 3.8f - mid(1);
    if (verbose) std::printf("  sag before the box %.3f m\n", sag);
    CHECK(sag > 0.15f && sag < 1.2f);
    const Body3D box = world.create_body({.shape = Shape3D::box({0.3f, 0.1f, 0.8f}), .motion = Body3DMotion::Dynamic,
                                          .position = {0.0f, mid(1) + 0.3f, 0.0f}, .mass = 30.0f});
    // Five seconds: the ropes are free to roll under it (no twist stiffness), which lets a box walk off in time; that is
    // documented, and not what is being checked here.
    for (int i = 0; i < 300; ++i) {
        world.step(dt);
        if (verbose && i % 20 == 0) {
            const Quatf q = world.rotation(box);
            std::printf("    %d box %.2f %.2f %.2f rot %.2f %.2f %.2f %.2f mid %.2f %.2f\n", i, world.position(box).x, world.position(box).y,
                        world.position(box).z, q.x, q.y, q.z, q.w, mid(0), mid(1));
        }
    }
    const f32 sag_loaded = 3.8f - mid(1);
    if (verbose) std::printf("  sag with the box %.3f m, box y %.3f\n", sag_loaded, world.position(box).y);
    CHECK(sag_loaded > sag + 0.05f);
    CHECK(world.position(box).y > 3.8f - sag_loaded - 0.05f && world.position(box).y < 3.8f - sag_loaded + 0.4f); // on them
    CHECK_NEAR(world.position(box).x, 0.0f, 0.3f);
    // The posts share its weight (and the ropes'): a force at each of the six ends.
    f32 total = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const f32 a = world.rope_tension(rails[k], RopeEnd3D::Start), b = world.rope_tension(rails[k], RopeEnd3D::End);
        CHECK(a > 50.0f && b > 50.0f);
        total += a + b;
    }
    // Held up by the four ends: their pull is more than the weight (they pull sideways too).
    CHECK(total > 30.0f * 9.81f);
}

// ---- length ---------------------------------------------------------------------------------------------------------------

void test_reel_and_pay_out() {
    World3D world(serial());
    const Body3D box = world.create_body({.shape = Shape3D::box({0.2f, 0.2f, 0.2f}), .motion = Body3DMotion::Dynamic,
                                          .position = {0.0f, 2.0f, 0.0f}, .mass = 10.0f});
    RopeDesc3D d;
    d.start = RopeAnchor3D::at({0.0f, 6.0f, 0.0f});
    d.end = RopeAnchor3D::on(box, {0.0f, 0.2f, 0.0f});
    d.linear_damping = 0.5f;
    const RopeHandle3D rope = world.create_rope(d);
    for (int i = 0; i < 240; ++i) {
        world.step(dt);
    }
    const f32 l0 = world.rope_state(rope).rest_length;
    CHECK_NEAR(l0, 3.8f, 0.05f);
    const f32 top0 = world.position(box).y;
    // Reel in at 1 m/s by 1 m: it takes 1 s (60 steps), the box rises 1 m, its segments are shorter.
    world.reel_rope(rope, l0 - 1.0f, 1.0f);
    for (int i = 0; i < 30; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.rope_state(rope).rest_length, l0 - 0.5f, 0.02f);
    for (int i = 0; i < 40; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.rope_state(rope).rest_length, l0 - 1.0f, 1e-4f);
    for (int i = 0; i < 180; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.position(box).y, top0 + 1.0f, 0.1f);
    const std::vector<RopeSegment3D> segs = world.rope_segments(rope);
    CHECK_NEAR(length(segs[3].b - segs[3].a), (l0 - 1.0f) / static_cast<f32>(world.rope_state(rope).segments), 5e-3f);
    // Pay out 2 m at 2 m/s: the box goes down, and the rope's measured length follows its rest length.
    world.reel_rope(rope, l0 + 1.0f, 2.0f);
    for (int i = 0; i < 70; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.rope_state(rope).rest_length, l0 + 1.0f, 1e-4f);
    for (int i = 0; i < 240; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.rope_state(rope).current_length, l0 + 1.0f, 0.15f);
    CHECK_NEAR(world.position(box).y, top0 - 1.0f, 0.15f);
    // At once.
    world.set_rope_length(rope, l0);
    CHECK_NEAR(world.rope_state(rope).rest_length, l0, 1e-5f);
}

void test_reel_in_a_heavy_load() {
    // A 60 kg crate on the end: the winch carries it (kinematic while it is reeled in), it is at the rope's end at once and dynamic
    // again after; a restore in the middle of it puts the motion type and the shapes back and repeats the trajectory.
    World3D world(serial());
    const Body3D crate = world.create_body({.shape = Shape3D::box({0.25f, 0.25f, 0.25f}), .motion = Body3DMotion::Dynamic,
                                            .position = {0.0f, 2.0f, 0.0f}, .mass = 60.0f});
    RopeDesc3D d;
    d.start = RopeAnchor3D::at({0.0f, 6.0f, 0.0f});
    d.end = RopeAnchor3D::on(crate, {0.0f, 0.25f, 0.0f});
    const RopeHandle3D rope = world.create_rope(d);
    for (int i = 0; i < 240; ++i) {
        world.step(dt);
    }
    const f32 y0 = world.position(crate).y;
    world.reel_rope(rope, world.rope_state(rope).rest_length - 1.5f, 1.0f);
    std::vector<u8> saved;
    std::vector<u64> first, second;
    for (int i = 0; i < 150; ++i) {
        if (i == 40) {
            saved = world.save_state();
            CHECK(world.motion(crate) == Body3DMotion::Kinematic);
        }
        world.step(dt);
        if (i >= 40) {
            first.push_back(world.state_hash());
        }
    }
    // 1.5 m at 1 m/s: 1.5 s (90 steps): carried the whole way, within a centimetre of the rope's end.
    CHECK_NEAR(world.position(crate).y, y0 + 1.5f, 0.12f);
    CHECK(world.motion(crate) == Body3DMotion::Dynamic);
    CHECK_NEAR(world.rope_state(rope).rest_length, 3.75f - 1.5f, 1e-4f);
    CHECK(world.restore_state(saved));
    CHECK(world.motion(crate) == Body3DMotion::Kinematic);
    for (int i = 40; i < 150; ++i) {
        world.step(dt);
        second.push_back(world.state_hash());
    }
    CHECK(first == second);
}

void test_breaks_over_threshold() {
    int broke[2] = {0, 0};
    u32 joint[2] = {0, 0};
    for (int heavy = 0; heavy < 2; ++heavy) {
        World3D world(serial());
        const Body3D box = world.create_body({.shape = Shape3D::box({0.25f, 0.25f, 0.25f}), .motion = Body3DMotion::Dynamic,
                                              .position = {0.0f, 1.5f, 0.0f}, .mass = heavy == 1 ? 90.0f : 20.0f});
        RopeDesc3D d;
        d.start = RopeAnchor3D::at({0.0f, 6.0f, 0.0f});
        d.end = RopeAnchor3D::on(box, {0.0f, 0.25f, 0.0f});
        d.break_tension = 500.0f;
        d.user_id = 77;
        const RopeHandle3D rope = world.create_rope(d);
        for (int i = 0; i < 240; ++i) {
            world.step(dt);
        }
        const std::vector<RopeEvent3D> events = world.drain_rope_events();
        broke[heavy] = static_cast<int>(events.size());
        if (!events.empty()) {
            CHECK(events[0].kind == RopeEventKind3D::Broke && events[0].rope == rope && events[0].user_id == 77);
            CHECK(events[0].force > 500.0f);
            joint[heavy] = events[0].joint;
            CHECK(world.rope_state(rope).broken);
        }
    }
    CHECK(broke[0] == 0);  // 20 kg: about 240 N
    CHECK(broke[1] >= 1);  // 90 kg: about 880 N
    CHECK(joint[1] <= 16);
}

void test_collide_world_off() {
    // A rope that does not collide with the level hangs through a slab; one that does lies on it.
    f32 y[2];
    for (int off = 0; off < 2; ++off) {
        World3D world(serial());
        world.create_body({.shape = Shape3D::box({2.0f, 0.1f, 2.0f}), .motion = Body3DMotion::Static, .position = {0.0f, 3.0f, 0.0f}});
        RopeDesc3D d;
        d.start = RopeAnchor3D::at({-1.5f, 3.25f, 0.0f});
        d.end = RopeAnchor3D::free();
        d.hang_direction = {1.0f, 0.0f, 0.0f};
        d.length = 2.0f;
        d.collide_world = off == 0;
        const RopeHandle3D rope = world.create_rope(d);
        for (int i = 0; i < 300; ++i) {
            world.step(dt);
        }
        y[off] = world.rope_points(rope).back().position.y;
    }
    if (verbose) std::printf("  rope end y: collides %.2f, passes %.2f\n", y[0], y[1]);
    CHECK(y[1] < 2.5f);   // passed through the slab (hangs from the anchor)
    CHECK(y[0] > 3.0f);   // lying on it
}

// ---- determinism ----------------------------------------------------------------------------------------------------------

std::vector<u64> run_ropes(World3DConfig cfg, int steps) {
    World3D world(cfg);
    add_floor(world);
    const Body3D box = world.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .motion = Body3DMotion::Dynamic,
                                          .position = {2.0f, 1.0f, 0.0f}, .mass = 25.0f});
    RopeDesc3D a = hanging({0.0f, 6.0f, 0.0f}, 4.0f);
    RopeDesc3D b;
    b.start = RopeAnchor3D::at({2.0f, 6.0f, 0.0f});
    b.end = RopeAnchor3D::on(box, {0.0f, 0.3f, 0.0f});
    RopeDesc3D c;
    c.start = RopeAnchor3D::at({-3.0f, 3.0f, 0.0f});
    c.end = RopeAnchor3D::at({-1.0f, 3.0f, 0.0f});
    c.length = 2.6f;
    const RopeHandle3D ra = world.create_rope(a);
    const RopeHandle3D rb = world.create_rope(b);
    world.create_rope(c);
    // Something to swing it.
    world.create_body({.shape = Shape3D::sphere(0.2f), .motion = Body3DMotion::Dynamic, .position = {-5.0f, 3.2f, 0.0f},
                       .linear_velocity = {6.0f, 0.0f, 0.0f}, .mass = 8.0f, .gravity_factor = 0.4f});
    std::vector<u64> hashes;
    for (int i = 0; i < steps; ++i) {
        if (i == 30) {
            world.add_impulse(box, {60.0f, 0.0f, 20.0f});
        }
        if (i == 60) {
            world.reel_rope(rb, 2.5f, 0.8f);
        }
        if (i == 120) {
            world.set_rope_length(ra, 3.0f);
        }
        world.step(dt);
        hashes.push_back(world.state_hash());
    }
    return hashes;
}

void test_determinism_and_restore() {
    const std::vector<u64> a = run_ropes(serial(), 300);
    CHECK(a == run_ropes(serial(), 300));
    CHECK(std::adjacent_find(a.begin(), a.end(), std::not_equal_to<>()) != a.end());
    for (const int workers : {1, 4}) {
        JobSystem jobs({.workers = workers, .max_background = workers});
        World3DConfig cfg;
        cfg.jobs = &jobs;
        cfg.single_threaded = false;
        CHECK(run_ropes(cfg, 300) == a);
    }
    // Saved while the rope reels (step 90), run on, restored: the same hashes again, and a rope's shape comes back with its
    // length.
    World3D world(serial());
    const Body3D box = world.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .motion = Body3DMotion::Dynamic,
                                          .position = {0.0f, 1.0f, 0.0f}, .mass = 25.0f});
    RopeDesc3D d;
    d.start = RopeAnchor3D::at({0.0f, 6.0f, 0.0f});
    d.end = RopeAnchor3D::on(box, {0.0f, 0.3f, 0.0f});
    const RopeHandle3D rope = world.create_rope(d);
    for (int i = 0; i < 90; ++i) {
        if (i == 60) {
            world.reel_rope(rope, 2.0f, 1.0f);
        }
        world.step(dt);
    }
    const std::vector<u8> saved = world.save_state();
    const f32 length_at_save = world.rope_state(rope).rest_length;
    std::vector<u64> first, second;
    for (int i = 0; i < 120; ++i) {
        if (i == 50) {
            world.set_rope_length(rope, 4.0f);
        }
        world.step(dt);
        first.push_back(world.state_hash());
    }
    CHECK(world.rope_state(rope).rest_length != length_at_save);
    CHECK(world.restore_state(saved));
    CHECK_NEAR(world.rope_state(rope).rest_length, length_at_save, 1e-6f);
    for (int i = 0; i < 120; ++i) {
        if (i == 50) {
            world.set_rope_length(rope, 4.0f);
        }
        world.step(dt);
        second.push_back(world.state_hash());
    }
    CHECK(first == second);
    // The hash sees the length.
    World3D x(serial()), y(serial());
    const RopeHandle3D rx = x.create_rope(hanging({0.0f, 5.0f, 0.0f}, 3.0f));
    y.create_rope(hanging({0.0f, 5.0f, 0.0f}, 3.0f));
    CHECK(x.state_hash() == y.state_hash());
    x.reel_rope(rx, 2.0f, 0.1f);
    x.step(dt);
    y.step(dt);
    CHECK(x.state_hash() != y.state_hash());
}

void test_redirected_gravity() {
    // Gravity along -X: a rope anchored at a point hangs along it, with its end at the same distance.
    World3D world(serial());
    world.set_gravity({-9.81f, 0.0f, 0.0f});
    RopeDesc3D d;
    d.start = RopeAnchor3D::at({6.0f, 2.0f, 0.0f});
    d.end = RopeAnchor3D::free();
    d.length = 4.0f;
    const RopeHandle3D rope = world.create_rope(d); // the free end hangs along gravity by default
    for (int i = 0; i < 300; ++i) {
        world.step(dt);
    }
    const Vec3f end = world.rope_points(rope).back().position;
    CHECK_NEAR(end.x, 2.0f, 0.05f);
    CHECK_NEAR(end.y, 2.0f, 0.05f);
}

// ---- a character on a rope -------------------------------------------------------------------------------------------------

// A floor, a rope hanging from (0, top, 0) to `bottom`, a hero with traversal on in the air under its end (feet at 1.85, the
// rope's end 1.5 m over its head: it reaches up for it, as a jump would) facing -Z.
struct Rig {
    World3D world;
    RopeHandle3D rope;
    Character3D hero;
    CharacterInput3D input;
    TraversalInput3D tin;
    std::vector<CharacterEvent3D> events;
    int frame = 0;

    // `tilt` degrees: the rope is made out from the vertical (toward +X) and the hero put under its end: it swings.
    explicit Rig(f32 top = 7.0f, f32 bottom = 3.4f, World3DConfig cfg = serial(), Vec3f hero_feet = {0.0f, 1.85f, 0.7f},
                 f32 tilt = 0.0f)
        : world(cfg) {
        add_floor(world);
        RopeDesc3D d = hanging({0.0f, top, 0.0f}, top - bottom);
        if (tilt != 0.0f) {
            const f32 a = tilt * 3.14159265f / 180.0f;
            d.hang_direction = {std::sin(a), -std::cos(a), 0.0f};
            d.length = (top - bottom) / std::cos(a);
        }
        rope = world.create_rope(d);
        CHECK(rope.valid());
        if (tilt != 0.0f) {
            hero_feet = world.rope_points(rope).back().position + Vec3f{0.0f, -1.5f, 0.45f};
        }
        CharacterDesc3D desc;
        desc.traversal.enabled = true;
        desc.traversal.resource.max = 1000.0f;
        desc.position = hero_feet;
        hero = world.create_character(desc);
        input.forward = {0.0f, 0.0f, -1.0f};
        tin.reach = {0.0f, 0.45f, -1.0f};
        // The rope settles first (it is made hanging straight: it only has to take up its slack).
        world.step(dt);
    }
    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            world.set_character_input(hero, input);
            world.set_traversal_input(hero, tin);
            world.step(dt);
            for (const CharacterEvent3D& e : world.drain_character_events()) {
                events.push_back(e);
            }
            ++frame;
        }
    }
    CharacterState3D state() const { return world.character(hero); }
    TraversalState3D trav() const { return world.traversal(hero); }
    int count(CharacterEventKind3D kind) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.kind == kind; }));
    }
    void grab(int steps = 60) {
        tin.grab = {true, true};
        step(steps);
    }
};

void test_grab_and_hang() {
    Rig r;
    if (verbose) {
        r.tin.grab = {true, true};
        for (int i = 0; i < 90; ++i) {
            r.step();
            if (i % 5 == 0) {
                const std::vector<RopeSegment3D> sg = r.world.rope_segments(r.rope);
                std::printf("    %d mode %d held %u feet %.2f %.2f %.2f end %.2f %.2f %.2f\n", i, static_cast<int>(r.trav().mode), r.trav().held,
                            r.state().position.x, r.state().position.y, r.state().position.z, sg.back().b.x, sg.back().b.y, sg.back().b.z);
            }
        }
    }
    if (!verbose) r.grab(90);
    const TraversalState3D t = r.trav();
    CHECK(t.mode == TraversalMode3D::Hanging && t.held == 2);
    CHECK(t.grips[0].kind == GripKind3D::Rope && t.grips[1].kind == GripKind3D::Rope);
    CHECK(r.count(CharacterEventKind3D::Gripped) == 2);
    // Two hands, not on the same point: at least 0.2 m of rope between them.
    CHECK(length(t.grips[0].point - t.grips[1].point) >= 0.2f);
    // Both on the rope's line (x = z = 0, within its radius and a hair).
    for (int h = 0; h < 2; ++h) {
        CHECK_NEAR(t.grips[h].point.x, 0.0f, 0.1f);
        CHECK_NEAR(t.grips[h].point.z, 0.0f, 0.1f);
    }
    // The character hangs beside the rope facing it, its weight on the rope: the pull at the top is its weight and the rope's.
    const f32 expected = (70.0f + 3.6f) * 9.81f;
    // The mean of a second (the rope bobs a little on its stretch).
    f32 tension = 0.0f;
    for (int i = 0; i < 60; ++i) {
        r.step();
        tension += r.world.rope_tension(r.rope, RopeEnd3D::Start) / 60.0f;
    }
    if (verbose) std::printf("  hanging: tension %.1f N (expected %.1f), feet %.2f %.2f %.2f\n", tension, expected, r.state().position.x, r.state().position.y, r.state().position.z);
    CHECK_NEAR(tension, expected, 0.1f * expected);
    CHECK(std::fabs(r.state().position.x) < 0.5f && r.state().position.z > 0.0f && r.state().position.z < 0.7f);
    // Hanging below the hands.
    CHECK(r.state().position.y < t.anchor_centre.y - 1.0f);
    // The rope took its weight: it is stretched by a few per cent at most.
    CHECK(r.world.rope_state(r.rope).current_length < 3.6f * 1.06f);
    // Let go: it falls and the rope comes back up (the load is off).
    r.tin.grab = {false, false};
    r.step(90);
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    f32 free = 0.0f;
    if (verbose) std::printf("  after release: mode %d held %u hero %.2f %.2f %.2f tension %.1f\n", static_cast<int>(r.trav().mode), r.trav().held,
                             r.state().position.x, r.state().position.y, r.state().position.z, r.world.rope_tension(r.rope, RopeEnd3D::Start));
    if (verbose) {
        const auto sg = r.world.rope_segments(r.rope);
        f32 added = 0;
        for (const auto& q : sg) added += r.world.added_mass(q.body);
        std::printf("  rope end %.2f %.2f %.2f added %.1f bodies %d active %d\n", sg.back().b.x, sg.back().b.y, sg.back().b.z, added,
                    r.world.body_count(), r.world.active_body_count());
        for (const Body3D b : r.world.bodies()) {
            std::printf("    body %u at %.2f %.2f %.2f motion %d active %d\n", b.index, r.world.position(b).x, r.world.position(b).y, r.world.position(b).z,
                        static_cast<int>(r.world.motion(b)), r.world.is_active(b) ? 1 : 0);
        }
    }
    r.step(180); // the rope's stretch rings out
    for (int i = 0; i < 120; ++i) {
        r.step();
        free += r.world.rope_tension(r.rope, RopeEnd3D::Start) / 120.0f;
    }
    CHECK_NEAR(free, 3.6f * 9.81f, 0.15f * 3.6f * 9.81f);
}

void test_climb_up_and_down() {
    Rig r;
    r.grab(60);
    CHECK(r.trav().held == 2);
    const f32 y0 = r.state().position.y;
    // Up the rope: 0.6 m/s along it.
    r.input.move = {0.0f, 1.0f};
    r.step(120);
    const f32 y1 = r.state().position.y;
    if (verbose) std::printf("  climbed %.2f m in 2 s\n", y1 - y0);
    CHECK_NEAR(y1 - y0, 1.2f, 0.2f);
    CHECK(r.trav().mode == TraversalMode3D::Hanging && r.trav().held == 2);
    // Down: the other way.
    r.input.move = {0.0f, -1.0f};
    r.step(60);
    const f32 y2 = r.state().position.y;
    CHECK_NEAR(y1 - y2, 0.6f, 0.15f);
    // Stops at the end of the rope: climbing on for 12 s does not leave it, the hands stop at the top (anchor 6 m).
    r.input.move = {0.0f, 1.0f};
    r.step(380);
    const TraversalState3D t = r.trav();
    CHECK(t.mode == TraversalMode3D::Hanging);
    CHECK(t.anchor_centre.y <= 7.0f + 1e-3f && t.anchor_centre.y > 6.0f);
    // Climbing costs more than hanging (the stamina went down; 100 units: exhausted well before 15 s of it).
    CHECK(t.resource.value < 1000.0f);
}

void test_drain_one_hand_and_two() {
    f32 rate[2];
    for (int hands = 1; hands <= 2; ++hands) {
        Rig r;
        r.tin.grab = {true, hands == 2};
        r.step(60);
        CHECK(r.trav().held == static_cast<u32>(hands));
        const f32 before = r.trav().resource.value;
        r.step(60);
        rate[hands - 1] = before - r.trav().resource.value;
    }
    if (verbose) std::printf("  drain: one hand %.2f, two hands %.2f a second\n", rate[0], rate[1]);
    CHECK_NEAR(rate[0], 12.0f, 0.5f);
    CHECK_NEAR(rate[1], 7.0f, 0.5f);
    CHECK(rate[0] > rate[1] * 1.5f);
}

// The period of the rope's swing against the pendulum's analytic one, with the climber at the end.
void test_swing_period() {
    // A rope of 6 m from y = 9 to y = 3, made 12 degrees out from the vertical (toward +X): the hero is put at its lower end.
    World3D world(serial());
    add_floor(world);
    RopeDesc3D d = hanging({0.0f, 9.0f, 0.0f}, 6.0f);
    const f32 angle = 12.0f * 3.14159265f / 180.0f;
    d.hang_direction = {std::sin(angle), -std::cos(angle), 0.0f};
    d.linear_damping = 0.0f;
    d.angular_damping = 0.05f;
    const RopeHandle3D rope = world.create_rope(d);
    const std::vector<RopePoint3D> pts = world.rope_points(rope);
    const Vec3f end = pts.back().position;
    CharacterDesc3D cd;
    cd.traversal.enabled = true;
    cd.traversal.resource.max = 1000.0f; // it hangs for half a minute
    // Feet below the end by the hang drop (1.7), 0.35 in front of it.
    cd.position = end + Vec3f{0.0f, -1.5f, 0.45f};
    const Character3D hero = world.create_character(cd);
    CharacterInput3D in;
    in.forward = {0.0f, 0.0f, -1.0f};
    TraversalInput3D tin;
    tin.grab = {true, true};
    tin.reach = {0.0f, 0.55f, -1.0f};
    std::vector<f32> xs;
    std::vector<Vec3f> hand;
    for (int i = 0; i < 60 * 30; ++i) {
        world.set_character_input(hero, in);
        world.set_traversal_input(hero, tin);
        world.step(dt);
        const TraversalState3D t = world.traversal(hero);
        if (t.held >= 1) {
            xs.push_back(t.anchor_centre.x);
            hand.push_back(t.anchor_centre);
        } else {
            xs.push_back(0.0f);
        }
    }
    CHECK(world.traversal(hero).held == 2);
    if (verbose) {
        for (std::size_t i = 0; i < xs.size(); i += 240) {
            std::printf("    t=%zu s x %.3f\n", i / 60, xs[i]);
        }
    }
    // The period from upward zero crossings of the hands' x about the mean, after the first seconds.
    const f32 mean = [&] {
        f32 lo = 1e9f, hi = -1e9f;
        for (std::size_t i = 120; i < xs.size(); ++i) {
            lo = std::min(lo, xs[i]);
            hi = std::max(hi, xs[i]);
        }
        return 0.5f * (lo + hi);
    }();
    std::vector<f32> crossings;
    for (std::size_t i = 121; i < xs.size(); ++i) {
        if (xs[i - 1] < mean && xs[i] >= mean) {
            const f32 u = (mean - xs[i - 1]) / (xs[i] - xs[i - 1]);
            crossings.push_back((static_cast<f32>(i - 1) + u) * dt);
        }
    }
    CHECK(crossings.size() >= 4);
    if (crossings.size() >= 4) {
        const f32 period = (crossings.back() - crossings.front()) / static_cast<f32>(crossings.size() - 1);
        // The pendulum: from the anchor to the hands' mean.
        const f32 length_to_hands = 9.0f - hand.back().y;
        const f32 analytic = 2.0f * 3.14159265f * std::sqrt(length_to_hands / 9.81f);
        // A rod and a point mass: small angle, the mass of the rope itself (5 kg) next to 70 shortens it a little.
        if (verbose) std::printf("  swing period %.3f s, analytic %.3f s (length %.2f m)\n", period, analytic, length_to_hands);
        CHECK_NEAR(period, analytic, 0.06f * analytic);
    }
}

void test_jump_off_a_swing() {
    // On a rope that swings (made 25 degrees out): let go at the bottom of the swing, where it is fastest.
    Rig r(9.0f, 3.4f, serial(), {}, 25.0f);
    r.grab(30);
    CHECK(r.trav().held == 2);
    // Run to the lowest point of the swing: the hands' height at its least (the first quarter period is about 1.3 s).
    f32 lowest = 1e9f;
    int at = 0;
    for (int i = 0; i < 120; ++i) {
        r.step();
        if (r.trav().anchor_centre.y < lowest) {
            lowest = r.trav().anchor_centre.y;
            at = i;
        }
    }
    // Back to near the bottom on the way up the other side... simply: step until the hands' height is within 2 cm of the
    // lowest seen, while moving fast.
    for (int i = 0; i < 400; ++i) {
        if (std::fabs(r.trav().anchor_centre.y - lowest) < 0.03f && length(r.world.linear_velocity(r.trav().grips[0].body)) > 1.5f) {
            break;
        }
        r.step();
    }
    (void)at;
    const Vec3f v_rope = r.world.linear_velocity(r.trav().grips[0].body);
    CHECK(length(v_rope) > 1.5f);
    r.tin.jump_off = true; // the hands still ask to hold: the jump off lets go for them
    r.step(1);
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    CHECK(r.count(CharacterEventKind3D::GripReleased) >= 2);
    const CharacterState3D st = r.state();
    // The speed of the swing carried (the character's own, which follows the hands), and the jump off's kick up (4 m/s).
    if (verbose) std::printf("  jump off: v %.2f %.2f %.2f, rope %.2f %.2f %.2f\n", st.velocity.x, st.velocity.y, st.velocity.z, v_rope.x, v_rope.y, v_rope.z);
    CHECK_NEAR(st.velocity.x, v_rope.x, 1.0f);
    CHECK(st.velocity.y > v_rope.y + 3.0f);
}

void test_pay_out_carries_the_climber() {
    Rig r;
    r.grab(60);
    const f32 y0 = r.state().position.y;
    const f32 l0 = r.world.rope_state(r.rope).rest_length;
    r.world.reel_rope(r.rope, l0 + 1.0f, 1.0f);
    r.step(150);
    if (verbose) std::printf("  pay out: mode %d held %u feet y %.2f (from %.2f), rope end y %.2f\n", static_cast<int>(r.trav().mode), r.trav().held,
                             r.state().position.y, y0, r.world.rope_segments(r.rope).back().b.y);
    CHECK(r.trav().held == 2);
    CHECK_NEAR(r.world.rope_state(r.rope).rest_length, l0 + 1.0f, 1e-4f);
    // It went down with the rope: about a metre.
    CHECK_NEAR(r.state().position.y, y0 - 1.0f, 0.25f);
}

void test_end_on_a_moving_body_carries_the_climber() {
    World3D world(serial());
    add_floor(world);
    const Body3D carriage = world.create_body({.shape = Shape3D::box({0.5f, 0.2f, 0.5f}), .motion = Body3DMotion::Kinematic,
                                               .position = {0.0f, 6.2f, 0.0f}});
    RopeDesc3D d;
    d.start = RopeAnchor3D::on(carriage, {0.0f, -0.2f, 0.0f});
    d.end = RopeAnchor3D::free();
    d.length = 4.8f;
    const RopeHandle3D rope = world.create_rope(d);
    CharacterDesc3D cd;
    cd.traversal.enabled = true;
    cd.position = {0.0f, 0.0f, 0.7f};
    const Character3D hero = world.create_character(cd);
    CharacterInput3D in;
    in.forward = {0.0f, 0.0f, -1.0f};
    TraversalInput3D tin;
    tin.grab = {true, true};
    tin.reach = {0.0f, 0.45f, -1.0f};
    for (int i = 0; i < 90; ++i) {
        world.set_character_input(hero, in);
        world.set_traversal_input(hero, tin);
        world.step(dt);
    }
    CHECK(world.traversal(hero).held == 2);
    const f32 x0 = world.character(hero).position.x;
    // The carriage goes 3 m along +X at 1 m/s: the climber with it, swinging a little behind.
    for (int i = 1; i <= 180; ++i) {
        world.move_kinematic(carriage, {static_cast<f32>(i) * dt, 6.2f, 0.0f}, {}, dt);
        world.set_character_input(hero, in);
        world.set_traversal_input(hero, tin);
        world.step(dt);
    }
    CHECK(world.traversal(hero).held == 2);
    const f32 dx = world.character(hero).position.x - x0;
    if (verbose) std::printf("  carried %.2f m of 3.0\n", dx);
    CHECK(dx > 2.0f && dx < 4.0f);
    (void)rope;
}

void test_character_on_a_rope_redirected_gravity() {
    // Gravity along -X: the rope hangs along -X from a point on the wall at x = 6; the hero stands on the "floor" at x = 0 (a
    // plane facing +X), grabs, hangs, and the pull is its weight along X.
    World3DConfig cfg = serial();
    cfg.gravity = {-9.81f, 0.0f, 0.0f};
    World3D world(cfg);
    world.create_body({.shape = Shape3D::box({0.5f, 20.0f, 20.0f}), .motion = Body3DMotion::Static, .position = {-0.5f, 0.0f, 0.0f}});
    RopeDesc3D d;
    d.start = RopeAnchor3D::at({6.0f, 0.0f, 0.0f});
    d.end = RopeAnchor3D::free();
    d.length = 4.8f;
    const RopeHandle3D rope = world.create_rope(d);
    CharacterDesc3D cd;
    cd.traversal.enabled = true;
    cd.position = {0.0f, 0.0f, 0.7f};
    const Character3D hero = world.create_character(cd);
    world.set_character_up(hero, {1.0f, 0.0f, 0.0f}, true);
    CharacterInput3D in;
    in.forward = {0.0f, 0.0f, -1.0f};
    TraversalInput3D tin;
    tin.grab = {true, true};
    // Up is +X: the hands reach "up" along +X and forward along -Z.
    tin.reach = {0.45f, 0.0f, -1.0f};
    for (int i = 0; i < 200; ++i) {
        world.set_character_input(hero, in);
        world.set_traversal_input(hero, tin);
        world.step(dt);
    }
    const TraversalState3D t = world.traversal(hero);
    CHECK(t.mode == TraversalMode3D::Hanging && t.held >= 1);
    const f32 expected = (70.0f + 4.8f) * 9.81f;
    CHECK_NEAR(world.rope_tension(rope, RopeEnd3D::Start), expected, 0.12f * expected);
}

std::vector<u64> run_climber(World3DConfig cfg, int steps, std::vector<u8>* saved = nullptr, int save_at = -1) {
    Rig r(7.0f, 3.4f, cfg);
    std::vector<u64> hashes;
    for (int i = 0; i < steps; ++i) {
        if (i == 5) r.tin.grab = {true, true};
        if (i == 80) r.input.move = {0.0f, 1.0f};
        if (i == 200) r.input.move = {0.4f, -1.0f};
        if (i == 260) r.world.reel_rope(r.rope, 3.2f, 0.7f);
        if (i == 330) r.tin.jump_off = true;
        if (i == save_at && saved != nullptr) {
            *saved = r.world.save_state();
        }
        r.step();
        hashes.push_back(r.world.state_hash());
    }
    return hashes;
}

void test_climber_determinism_and_restore() {
    const std::vector<u64> a = run_climber(serial(), 420);
    CHECK(a == run_climber(serial(), 420));
    for (const int workers : {1, 4}) {
        JobSystem jobs({.workers = workers, .max_background = workers});
        World3DConfig cfg;
        cfg.jobs = &jobs;
        cfg.single_threaded = false;
        CHECK(run_climber(cfg, 420) == a);
    }
    // Save at step 150 (climbing), run to the end, restore: the same hashes from there.
    Rig r;
    std::vector<u64> first;
    std::vector<u8> saved;
    auto script = [&](int i) {
        if (i == 5) r.tin.grab = {true, true};
        if (i == 80) r.input.move = {0.0f, 1.0f};
        if (i == 200) r.input.move = {0.4f, -1.0f};
        if (i == 260) r.world.reel_rope(r.rope, 3.2f, 0.7f);
        if (i == 330) r.tin.jump_off = true;
    };
    for (int i = 0; i < 150; ++i) {
        script(i);
        r.step();
    }
    saved = r.world.save_state();
    const u64 hash_at_save = r.world.state_hash();
    // The input state is part of the saved state; the script's own locals (the move command) are what the caller keeps.
    const CharacterInput3D input_at_save = r.input;
    const TraversalInput3D tin_at_save = r.tin;
    for (int i = 150; i < 420; ++i) {
        script(i);
        r.step();
        first.push_back(r.world.state_hash());
    }
    CHECK(r.world.restore_state(saved));
    CHECK(r.world.state_hash() == hash_at_save);
    r.input = input_at_save;
    r.tin = tin_at_save;
    std::vector<u64> second;
    for (int i = 150; i < 420; ++i) {
        script(i);
        r.step();
        second.push_back(r.world.state_hash());
    }
    if (verbose) {
        for (std::size_t i = 0; i < first.size(); ++i) {
            if (first[i] != second[i]) {
                std::printf("  restore differs at step %zu\n", i + 150);
                break;
            }
        }
    }
    CHECK(first == second);
    CHECK(std::equal(first.begin(), first.end(), a.begin() + 150));
}

// ---- the ECS side --------------------------------------------------------------------------------------------------------

void test_ecs_rope() {
    kin::EcsWorld ecs;
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    // A crate (dynamic) and a rope from a point of the world to a point of its top, made at the first sync, after the bodies.
    const flecs::entity crate = ecs.raw().entity().set(Transform3D{.position = {0.0f, 2.0f, 0.0f}})
                                    .set(Collider3D{.kind = Collider3D::Kind::Box, .half_extents = {0.25f, 0.25f, 0.25f}})
                                    .set(RigidBody3D{.mass = 15.0f});
    Rope3D rope;
    rope.start = RopeAttach3D::at({0.0f, 6.0f, 0.0f});
    rope.end = RopeAttach3D::on(crate, {0.0f, 0.25f, 0.0f});
    rope.break_tension = 0.0f;
    rope.linear_damping = 0.5f; // the load's bounce on the rope's stretch dies down
    const flecs::entity holder = ecs.raw().entity().set(Transform3D{}).set(rope);
    CHECK(!bodies.rope_of(holder).valid());
    bodies.step(dt);
    CHECK(bodies.rope_of(holder).valid() && holder.get<PhysicsRope3D>() != nullptr);
    CHECK(physics.rope_state(bodies.rope_of(holder)).segments == 15);
    CHECK(physics.user_id(physics.rope_segments(bodies.rope_of(holder))[0].body) == holder.id());
    for (int i = 0; i < 480; ++i) {
        bodies.step(dt);
    }
    const Rope3D& r = *holder.get<Rope3D>();
    if (verbose) std::printf("  ecs rope: crate y %.3f, rest %.3f current %.3f, tension %.1f / %.1f\n", physics.position(bodies.body_of(crate)).y,
                             r.state.rest_length, r.state.current_length, r.tension(RopeEnd3D::Start), r.tension(RopeEnd3D::End));
    CHECK(r.points().size() == 16 && r.state.segments == 15);
    CHECK_NEAR(r.points().front().position.y, 6.0f, 0.02f);
    // The pull at the top: the crate's weight and the rope's (3.75 kg), to what the solver's residual lets a reading be.
    const f32 expected = (15.0f + 3.75f) * 9.81f;
    CHECK_NEAR(r.tension(RopeEnd3D::Start), expected, 0.15f * expected);
    CHECK_NEAR(r.tension(RopeEnd3D::End), 15.0f * 9.81f, 0.15f * 15.0f * 9.81f);
    const auto c = r.closest_point({0.6f, 4.0f, 0.0f});
    CHECK(c.has_value() && c->distance > 0.55f && c->distance < 0.65f && c->along > 0.4f && c->along < 0.6f);
    // The crate hangs from it: it is below the start by the rope's length.
    CHECK_NEAR(physics.position(bodies.body_of(crate)).y, 6.0f - 3.75f - 0.25f, 0.1f);
    // Reeled from the component: a target length and a speed.
    holder.get_mut<Rope3D>()->target_length = 2.75f;
    holder.get_mut<Rope3D>()->reel_speed = 1.0f;
    for (int i = 0; i < 420; ++i) {
        bodies.step(dt);
        if (verbose && i % 30 == 0) {
            std::printf("    reel %d: rest %.3f crate y %.3f active %d tension %.0f\n", i, holder.get<Rope3D>()->state.rest_length,
                        physics.position(bodies.body_of(crate)).y, physics.is_active(bodies.body_of(crate)) ? 1 : 0,
                        holder.get<Rope3D>()->tension(RopeEnd3D::Start));
        }
    }
    CHECK_NEAR(holder.get<Rope3D>()->state.rest_length, 2.75f, 1e-3f);
    CHECK_NEAR(physics.position(bodies.body_of(crate)).y, 6.0f - 2.75f - 0.25f, 0.25f);
    // Destroyed with the component; made again with it.
    holder.remove<Rope3D>();
    bodies.sync_bodies();
    CHECK(physics.rope_count() == 0 && physics.body_count() == 1);
}

void test_ecs_rope_breaks_with_an_event() {
    kin::EcsWorld ecs;
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    const flecs::entity crate = ecs.raw().entity().set(Transform3D{.position = {0.0f, 2.0f, 0.0f}})
                                    .set(Collider3D{.kind = Collider3D::Kind::Box, .half_extents = {0.25f, 0.25f, 0.25f}})
                                    .set(RigidBody3D{.mass = 90.0f});
    Rope3D rope;
    rope.start = RopeAttach3D::at({0.0f, 6.0f, 0.0f});
    rope.end = RopeAttach3D::on(crate, {0.0f, 0.25f, 0.0f});
    rope.break_tension = 500.0f;
    const flecs::entity holder = ecs.raw().entity().set(Transform3D{}).set(rope);
    int broke = 0;
    for (int i = 0; i < 200; ++i) {
        bodies.step(dt);
        for (const RopeEvent3D& e : bodies.rope_events()) {
            CHECK(e.user_id == holder.id() && e.kind == RopeEventKind3D::Broke);
            ++broke;
        }
    }
    CHECK(broke >= 1);
    CHECK(holder.get<Rope3D>()->state.broken);
    // The singleton too.
    CHECK(ecs.raw().has<RopeEvents3D>());
}

void test_ecs_rope_waits_for_its_anchor() {
    // An anchor entity whose body does not exist yet: the rope is made at the sync after it does.
    kin::EcsWorld ecs;
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    const flecs::entity post = ecs.raw().entity().set(Transform3D{.position = {0.0f, 5.0f, 0.0f}});
    Rope3D rope;
    rope.start = RopeAttach3D::on(post, {0.0f, 0.0f, 0.0f});
    rope.end = RopeAttach3D::free();
    rope.length = 2.0f;
    const flecs::entity holder = ecs.raw().entity().set(Transform3D{}).set(rope);
    bodies.step(dt);
    CHECK(!bodies.rope_of(holder).valid() && physics.rope_count() == 0);
    post.set(Collider3D{.kind = Collider3D::Kind::Box, .half_extents = {0.2f, 0.2f, 0.2f}});
    bodies.step(dt);
    CHECK(bodies.rope_of(holder).valid());
}

// A segment is the rope's: destroying, reshaping or switching it from outside is refused (destroyed, it left rope_points
// reading a dead body at the origin), while rays, pushes and the rope's own calls still reach it.
void test_segments_belong_to_the_rope() {
    World3D world(serial());
    const Body3D floor = add_floor(world);
    const RopeHandle3D rope = world.create_rope(hanging({0.0f, 6.0f, 0.0f}, 4.0f));
    for (int i = 0; i < 10; ++i) {
        world.step(dt);
    }
    const Body3D seg = world.rope_segment_body(rope, 5);
    CHECK(world.owner_of(seg) == BodyOwner3D::Rope);
    CHECK(world.owner_of(floor) == BodyOwner3D::None);
    const std::vector<RopePoint3D> before = world.rope_points(rope);
    CHECK(!world.destroy_body(seg));
    CHECK(world.contains(seg) && world.body_count() == 17);
    CHECK(!world.set_shape(seg, Shape3D::sphere(0.1f)));
    CHECK(!world.set_motion(seg, Body3DMotion::Kinematic));
    CHECK(world.motion(seg) == Body3DMotion::Dynamic);
    const std::vector<RopePoint3D> after = world.rope_points(rope);
    CHECK(after.size() == before.size());
    for (std::size_t i = 0; i < after.size() && i < before.size(); ++i) {
        CHECK(length(after[i].position - before[i].position) == 0.0f);
    }
    // A ray across segment 5's middle names it, and an impulse moves it.
    const auto hit = world.raycast({1.0f, 4.625f, 0.0f}, {-1.0f, 4.625f, 0.0f});
    CHECK(hit.has_value() && hit->body == seg && world.owner_of(hit->body) == BodyOwner3D::Rope);
    world.add_impulse(seg, {0.5f, 0.0f, 0.0f});
    world.step(dt);
    CHECK(world.linear_velocity(seg).x > 0.0f);
    // The rope's own calls are not refused: a new length reshapes every segment, and destroy_rope takes them.
    world.set_rope_length(rope, 3.0f);
    CHECK_NEAR(world.rope_state(rope).rest_length, 3.0f, 1e-5f);
    world.destroy_rope(rope);
    CHECK(!world.contains(seg) && world.body_count() == 1);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-v") verbose = true;
    }
    test_create_and_sample();
    test_hanging_rope_settles();
    test_tension_equals_weight_and_length_bound();
    test_anchor_on_a_moving_body();
    test_bridge_carries_a_box();
    test_reel_and_pay_out();
    test_reel_in_a_heavy_load();
    test_breaks_over_threshold();
    test_collide_world_off();
    test_determinism_and_restore();
    test_redirected_gravity();
    test_grab_and_hang();
    test_climb_up_and_down();
    test_drain_one_hand_and_two();
    test_swing_period();
    test_jump_off_a_swing();
    test_pay_out_carries_the_climber();
    test_end_on_a_moving_body_carries_the_climber();
    test_character_on_a_rope_redirected_gravity();
    test_climber_determinism_and_restore();
    test_ecs_rope();
    test_ecs_rope_breaks_with_an_event();
    test_ecs_rope_waits_for_its_anchor();
    test_segments_belong_to_the_rope();
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_rope_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_rope_tests: all passed\n");
    return 0;
}
