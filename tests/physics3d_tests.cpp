// kin/physics3d (Jolt Physics behind kin's own types): bodies at rest and in motion
// against hand-computed values, the shapes, layers, handles, the state hash and its
// determinism (the same bits twice, and with 1, 4 and no workers), saved states.
// No GPU. docs/physics_3d.md.

#include <kin/core/jobs.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/physics3d/debug_draw3d.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using namespace kin;

int failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            ++failures;                                                                  \
            std::fprintf(stderr, "physics3d_tests:%d: CHECK failed: %s\n", __LINE__, #cond); \
        }                                                                                \
    } while (0)

constexpr f32 dt = 1.0f / 60.0f;

bool near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }
bool near(Vec3f a, Vec3f b, f32 eps) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

void run(World3D& world, int frames) {
    for (int i = 0; i < frames; ++i) {
        (void)world.step(dt);
    }
}

World3DConfig serial() {
    World3DConfig c;
    c.single_threaded = true;
    c.contact_events = true;
    return c;
}

// A floor whose top face is the plane y = 0.
Body3D add_floor(World3D& world, f32 friction = 0.5f, f32 restitution = 0.0f) {
    return world.create_body({.shape = Shape3D::box({50.0f, 0.5f, 50.0f}), .motion = Body3DMotion::Static,
                              .position = {0.0f, -0.5f, 0.0f}, .friction = friction, .restitution = restitution});
}

Body3D add_box(World3D& world, Vec3f position, f32 half = 0.5f, u64 user_id = 0) {
    return world.create_body({.shape = Shape3D::box({half, half, half}), .position = position, .user_id = user_id});
}

// A pile of boxes and spheres dropped into a walled yard, from a fixed recipe.
struct Lcg {
    u32 s = 12345;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<f32>(s >> 8) / static_cast<f32>(1u << 24);
    }
};

void build_pile(World3D& world, int count, u64 seed = 12345) {
    add_floor(world, 0.5f);
    const f32 w = 8.0f;
    const Shape3D wall_x = Shape3D::box({0.5f, 10.0f, w + 1.0f});
    const Shape3D wall_z = Shape3D::box({w + 1.0f, 10.0f, 0.5f});
    for (const f32 sx : {-1.0f, 1.0f}) {
        world.create_body({.shape = wall_x, .motion = Body3DMotion::Static, .position = {sx * (w + 0.5f), 10.0f, 0.0f}});
        world.create_body({.shape = wall_z, .motion = Body3DMotion::Static, .position = {0.0f, 10.0f, sx * (w + 0.5f)}});
    }
    const Shape3D box = Shape3D::box({0.4f, 0.3f, 0.35f});
    const Shape3D ball = Shape3D::sphere(0.35f);
    Lcg rng;
    rng.s = static_cast<u32>(seed);
    for (int i = 0; i < count; ++i) {
        const f32 x = (rng.next() - 0.5f) * 2.0f * (w - 1.0f);
        const f32 z = (rng.next() - 0.5f) * 2.0f * (w - 1.0f);
        const f32 y = 1.0f + 0.8f * static_cast<f32>(i / 60);
        const Quatf q = Quatf::axis_angle({rng.next() + 0.1f, rng.next(), rng.next()}, rng.next() * 360.0f);
        world.create_body({.shape = i % 3 == 0 ? ball : box, .position = {x, y, z}, .rotation = q,
                           .friction = 0.4f, .restitution = 0.1f, .user_id = static_cast<u64>(i)});
    }
    world.optimize_broad_phase();
}

std::vector<u64> pile_hashes(World3DConfig config, int count, int frames) {
    World3D world(config);
    build_pile(world, count);
    std::vector<u64> hashes;
    for (int i = 0; i < frames; ++i) {
        (void)world.step(dt);
        hashes.push_back(world.state_hash());
    }
    return hashes;
}

void test_rest_and_sleep() {
    World3D world(serial());
    add_floor(world);
    const Body3D box = add_box(world, {0.0f, 3.0f, 0.0f});
    CHECK(world.is_active(box));
    run(world, 300);
    const Vec3f p = world.position(box);
    // Half a metre up, give or take Jolt's 2 cm of allowed penetration.
    CHECK(near(p.y, 0.5f, 0.03f));
    CHECK(near(p.x, 0.0f, 0.01f) && near(p.z, 0.0f, 0.01f));
    CHECK(length(world.linear_velocity(box)) < 0.05f);
    CHECK(!world.is_active(box)); // asleep
    CHECK(world.active_body_count() == 0);
    world.activate(box);
    CHECK(world.is_active(box));
    world.deactivate(box);
    CHECK(!world.is_active(box));
}

void test_free_fall_dt() {
    // v += g dt, then x += v dt: after n steps the drop is g dt^2 n (n + 1) / 2.
    World3D world(serial());
    const Body3D b = world.create_body({.shape = Shape3D::sphere(0.25f), .position = {0.0f, 100.0f, 0.0f}});
    run(world, 60);
    const f32 expected = 9.81f * dt * dt * 60.0f * 61.0f / 2.0f;
    CHECK(near(100.0f - world.position(b).y, expected, 0.01f));
    CHECK(near(world.linear_velocity(b).y, -9.81f, 0.01f));
    // A bigger step covers the same time with the same law.
    World3D big(serial());
    const Body3D c = big.create_body({.shape = Shape3D::sphere(0.25f), .position = {0.0f, 100.0f, 0.0f}});
    (void)big.step(0.5f);
    CHECK(near(big.linear_velocity(c).y, -4.905f, 0.01f));
}

void test_gravity_parameter() {
    World3DConfig cfg = serial();
    cfg.gravity = {5.0f, 0.0f, 0.0f};
    World3D world(cfg);
    CHECK(near(world.gravity(), {5.0f, 0.0f, 0.0f}, 1e-6f));
    const Body3D b = world.create_body({.shape = Shape3D::sphere(0.25f), .position = {0.0f, 10.0f, 0.0f}});
    run(world, 60);
    const f32 expected = 5.0f * dt * dt * 60.0f * 61.0f / 2.0f;
    CHECK(near(world.position(b).x, expected, 0.01f));
    CHECK(near(world.position(b).y, 10.0f, 1e-4f));
    CHECK(near(world.position(b).z, 0.0f, 1e-4f));
    // Changed at run time: now along -Z.
    world.set_gravity({0.0f, 0.0f, -2.0f});
    world.set_linear_velocity(b, {});
    run(world, 30);
    CHECK(world.linear_velocity(b).z < -0.9f && near(world.linear_velocity(b).x, 0.0f, 1e-3f));
    // The default is -Y.
    World3D plain(serial());
    CHECK(near(plain.gravity(), {0.0f, -9.81f, 0.0f}, 1e-6f));
    // A body's own factor scales it.
    World3D scaled(serial());
    const Body3D half = scaled.create_body({.shape = Shape3D::sphere(0.25f), .position = {0.0f, 10.0f, 0.0f},
                                            .gravity_factor = 0.5f});
    run(scaled, 60);
    CHECK(near(scaled.linear_velocity(half).y, -4.905f, 0.01f));
}

void test_sphere_rolls_down_mesh() {
    World3D world(serial());
    // A ramp 20 m long, 10 wide, falling 20 degrees toward +X, as two triangles.
    const f32 a = 20.0f * 3.14159265f / 180.0f;
    const f32 run_x = 20.0f * std::cos(a);
    const f32 drop = 20.0f * std::sin(a);
    const std::array<Vec3f, 4> verts{{{0.0f, drop, -5.0f}, {0.0f, drop, 5.0f}, {run_x, 0.0f, 5.0f}, {run_x, 0.0f, -5.0f}}};
    const std::array<u32, 6> idx{0, 1, 2, 0, 2, 3};
    const Shape3D mesh = Shape3D::triangle_mesh(verts, idx);
    CHECK(mesh.valid() && mesh.kind() == Shape3DKind::TriangleMesh);
    world.create_body({.shape = mesh, .motion = Body3DMotion::Static, .friction = 0.8f});
    const Body3D ball = world.create_body({.shape = Shape3D::sphere(0.5f), .position = {1.0f, drop - 1.0f * std::tan(a) + 0.6f, 0.0f},
                                           .friction = 0.8f});
    run(world, 180);
    const Vec3f p = world.position(ball);
    // Well down the slope, still on it: on the plane y = drop - x tan(a), one radius off it.
    CHECK(p.x > 5.0f);
    const f32 surface = drop - p.x * std::tan(a);
    CHECK(near(p.y - surface, 0.5f / std::cos(a), 0.1f));
    // Rolling without slipping: spin about -Z at speed / radius.
    const Vec3f w = world.angular_velocity(ball);
    const f32 speed = length(world.linear_velocity(ball));
    CHECK(w.z < 0.0f);
    CHECK(near(-w.z, speed / 0.5f, 0.15f * speed / 0.5f + 0.1f));
    // A dynamic body of a mesh is refused.
    CHECK(!world.create_body({.shape = mesh, .motion = Body3DMotion::Dynamic}).valid());
}

void test_restitution() {
    // Dropped from 2 m onto a floor; the combined restitution is the larger of
    // the two, 0.8, so it comes back to 0.8^2 of the height.
    auto bounce_height = [](f32 restitution) {
        World3D world(serial());
        add_floor(world, 0.5f, 0.0f);
        const f32 r = 0.25f;
        const Body3D ball = world.create_body({.shape = Shape3D::sphere(r), .position = {0.0f, 2.0f + r, 0.0f},
                                               .restitution = restitution});
        bool bounced = false;
        f32 peak = 0.0f;
        for (int i = 0; i < 240; ++i) {
            const f32 before = world.linear_velocity(ball).y;
            (void)world.step(dt);
            const f32 after = world.linear_velocity(ball).y;
            if (before < -1.0f && after > 0.0f) {
                bounced = true;
            }
            if (bounced) {
                peak = std::max(peak, world.position(ball).y - r);
                if (after < 0.0f) {
                    break; // past the top
                }
            }
        }
        return bounced ? peak : -1.0f;
    };
    const f32 h = bounce_height(0.8f);
    CHECK(h > 0.0f);
    CHECK(near(h, 0.64f * 2.0f, 0.15f));
    CHECK(h < 2.0f); // never above where it fell from
    const f32 dead = bounce_height(0.0f);
    CHECK(dead < 0.05f);
}

void test_shapes_at_rest() {
    World3D world(serial());
    add_floor(world);
    const Body3D capsule = world.create_body({.shape = Shape3D::capsule(0.5f, 0.25f), .position = {-6.0f, 2.0f, 0.0f}});
    const Body3D cylinder = world.create_body({.shape = Shape3D::cylinder(0.4f, 0.3f), .position = {-3.0f, 2.0f, 0.0f}});
    std::vector<Vec3f> pts;
    for (const f32 x : {-0.5f, 0.5f}) {
        for (const f32 y : {-0.5f, 0.5f}) {
            for (const f32 z : {-0.5f, 0.5f}) {
                pts.push_back({x, y, z});
            }
        }
    }
    const Shape3D hull = Shape3D::convex_hull(pts);
    CHECK(hull.valid() && hull.kind() == Shape3DKind::ConvexHull);
    CHECK(near(hull.volume(), 1.0f, 0.01f));
    const Body3D hull_body = world.create_body({.shape = hull, .position = {0.0f, 2.0f, 0.0f}});

    // A height field: 16 x 16 samples, flat at 1 m, over x and z in [-8, 8) (cells of 1 m).
    std::vector<f32> heights(16 * 16, 1.0f);
    const Shape3D field = Shape3D::height_field(heights, 16, {-8.0f, 0.0f, -8.0f}, {1.0f, 1.0f, 1.0f});
    CHECK(field.valid() && field.kind() == Shape3DKind::HeightField);
    World3D fieldworld(serial());
    fieldworld.create_body({.shape = field, .motion = Body3DMotion::Static});
    const Body3D on_field = fieldworld.create_body({.shape = Shape3D::sphere(0.5f), .position = {1.3f, 4.0f, -2.2f}});
    // A slope: height = 0.5 x; the ball rests nowhere but comes down it.
    std::vector<f32> ramp(16 * 16);
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
            ramp[static_cast<std::size_t>(z * 16 + x)] = 0.5f * static_cast<f32>(x);
        }
    }
    World3D rampworld(serial());
    rampworld.create_body({.shape = Shape3D::height_field(ramp, 16, {-8.0f, 0.0f, -8.0f}), .motion = Body3DMotion::Static,
                           .friction = 0.0f});
    const Body3D slider = rampworld.create_body({.shape = Shape3D::sphere(0.25f), .position = {2.0f, 6.0f, 0.0f},
                                                 .friction = 0.0f});

    run(world, 240);
    run(fieldworld, 240);
    run(rampworld, 120);

    // Capsule upright: centre one half height plus radius up.
    CHECK(near(world.position(capsule).y, 0.75f, 0.04f));
    CHECK(near(world.position(cylinder).y, 0.4f, 0.04f));
    CHECK(near(world.position(hull_body).y, 0.5f, 0.04f));
    CHECK(near(fieldworld.position(on_field).y, 1.5f, 0.04f));
    CHECK(near(fieldworld.position(on_field).x, 1.3f, 0.02f));
    // The sliding ball went down the slope (-X) and stayed above the surface.
    const Vec3f s = rampworld.position(slider);
    CHECK(s.x < 1.0f);
    CHECK(s.y >= 0.5f * s.x - 0.05f);
    // Bad input gives no shape.
    CHECK(!Shape3D::box({0.0f, 1.0f, 1.0f}).valid());
    CHECK(!Shape3D::sphere(-1.0f).valid());
    CHECK(!Shape3D::convex_hull(std::span<const Vec3f>(pts.data(), 3)).valid());
    CHECK(!Shape3D::height_field(heights, 15).valid());
    CHECK(!Shape3D::triangle_mesh(std::span<const Vec3f>(pts.data(), 3), std::array<u32, 3>{0, 1, 5}).valid());
    CHECK(!Shape3D::compound({}).valid());
}

void test_compound() {
    const Shape3D unit = Shape3D::box({0.5f, 0.5f, 0.5f});        // volume 1
    const Shape3D double_box = Shape3D::box({1.0f, 0.5f, 0.5f});  // volume 2
    const std::array<ShapeChild3D, 2> parts{{{unit, {0.0f, 0.0f, 0.0f}, {}}, {double_box, {3.0f, 0.0f, 0.0f}, {}}}};
    const Shape3D compound = Shape3D::compound(parts);
    CHECK(compound.valid() && compound.kind() == Shape3DKind::Compound);
    CHECK(near(compound.volume(), 3.0f, 0.01f));
    // (0 * 1 + 3 * 2) / 3 along X.
    CHECK(near(compound.centre_of_mass(), {2.0f, 0.0f, 0.0f}, 0.01f));
    CHECK(near(compound.bounds_min().x, -0.5f, 0.01f) && near(compound.bounds_max().x, 4.0f, 0.01f));

    const std::array<ShapeChild3D,1> identity_child{{{unit,{},{}}}};
    const auto identity=Shape3D::compound(identity_child);
    std::vector<u8> binary;
    CHECK(identity.save_binary(binary));
    const auto restored=Shape3D::restore_binary(binary,Shape3DKind::Compound);
    CHECK(restored.valid() && restored.kind()==Shape3DKind::Compound && restored.volume()==identity.volume());

    World3DConfig cfg = serial();
    cfg.gravity = {};
    World3D world(cfg);
    const Body3D b = world.create_body({.shape = compound, .position = {10.0f, 0.0f, 0.0f}, .density = 100.0f});
    CHECK(near(world.mass(b), 300.0f, 0.5f));
    CHECK(near(world.centre_of_mass_position(b), {12.0f, 0.0f, 0.0f}, 0.01f));
    // An impulse through the centre of mass does not spin it; one off it does.
    world.add_impulse_at(b, {0.0f, 300.0f, 0.0f}, {12.0f, 0.0f, 0.0f});
    run(world, 1);
    CHECK(near(world.linear_velocity(b), {0.0f, 1.0f, 0.0f}, 0.01f));
    CHECK(length(world.angular_velocity(b)) < 1e-3f);
    World3D spun(cfg);
    const Body3D c = spun.create_body({.shape = compound, .position = {10.0f, 0.0f, 0.0f}, .density = 100.0f});
    spun.add_impulse_at(c, {0.0f, 300.0f, 0.0f}, {10.0f, 0.0f, 0.0f}); // at the unit box, left of the centre
    run(spun, 1);
    CHECK(spun.angular_velocity(c).z < -0.01f);
    // Falls under gravity as a whole body: the rotating centre of mass moves with position.
    World3D falling(serial());
    add_floor(falling);
    const Body3D f = falling.create_body({.shape = compound, .position = {0.0f, 3.0f, 0.0f}, .density = 100.0f});
    run(falling, 400);
    const Vec3f com = falling.centre_of_mass_position(f);
    CHECK(com.y > 0.4f && com.y < 0.7f); // lying on a side, centre of mass about half a box up
}

void test_kinematic_pushes() {
    World3D world(serial());
    add_floor(world, 0.0f);
    const Body3D pusher = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 2.0f}), .motion = Body3DMotion::Kinematic,
                                             .position = {-5.0f, 0.5f, 0.0f}});
    const Body3D crate = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0.0f, 0.5f, 0.0f},
                                            .friction = 0.0f});
    f32 x = -5.0f;
    for (int i = 0; i < 240; ++i) {
        x += 2.0f * dt;
        world.move_kinematic(pusher, {x, 0.5f, 0.0f}, {}, dt);
        (void)world.step(dt);
    }
    CHECK(near(world.position(pusher).x, x, 0.02f));
    // The pusher went from -5 to -1; its face (+0.5) met the crate's (-0.5) at x = -1, so the crate went on.
    CHECK(world.position(crate).x > 0.3f);
    CHECK(world.motion(pusher) == Body3DMotion::Kinematic);
    CHECK(length(world.linear_velocity(pusher)) > 1.5f);
    // A kinematic body is not moved by gravity.
    CHECK(near(world.position(pusher).y, 0.5f, 1e-3f));
}

void test_handles_and_user_ids() {
    World3D world(serial());
    const Body3D a = add_box(world, {0.0f, 5.0f, 0.0f}, 0.5f, 11);
    const Body3D b = add_box(world, {3.0f, 5.0f, 0.0f}, 0.5f, 22);
    const Body3D c = add_box(world, {6.0f, 5.0f, 0.0f}, 0.5f, 33);
    CHECK(a.valid() && b.valid() && c.valid());
    CHECK(a.index == 0 && b.index == 1 && c.index == 2);
    CHECK(world.user_id(a) == 11 && world.user_id(b) == 22 && world.user_id(c) == 33);
    CHECK(world.body_count() == 3);
    world.set_user_id(b, 99);
    CHECK(world.user_id(b) == 99);
    CHECK(world.user_id(a) == 11);

    (void)world.destroy_body(b);
    CHECK(!world.contains(b));
    CHECK(world.body_count() == 2);
    CHECK(world.user_id(b) == 0);
    CHECK(near(world.position(b), {}, 0.0f)); // a dead handle reads as nothing
    world.set_position(b, {1.0f, 1.0f, 1.0f}); // ... and is ignored
    const Body3D d = add_box(world, {9.0f, 5.0f, 0.0f}, 0.5f, 44);
    CHECK(d.index == b.index && d.generation != b.generation); // the number is reused, not the identity
    CHECK(!world.contains(b));
    CHECK(world.contains(d));
    CHECK(world.user_id(d) == 44);
    (void)world.destroy_body(b); // stale: must not destroy d
    CHECK(world.contains(d));
    CHECK(!world.contains(Body3D{}));

    // Refused: invalid shape, non-finite position, a dynamic body in the static layer, a layer out of range.
    CHECK(!world.create_body({}).valid());
    CHECK(!world.create_body({.shape = Shape3D::sphere(1.0f), .position = {std::numeric_limits<f32>::quiet_NaN(), 0, 0}}).valid());
    CHECK(!world.create_body({.shape = Shape3D::sphere(1.0f), .layer = layers3d::Static}).valid());
    CHECK(!world.create_body({.shape = Shape3D::sphere(1.0f), .layer = 40}).valid());
    CHECK(world.body_count() == 3);

    // Transforms and velocities read back what was set.
    const Quatf q = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 90.0f);
    world.set_position(a, {1.0f, 2.0f, 3.0f});
    world.set_rotation(a, q);
    world.set_linear_velocity(a, {1.0f, 0.0f, 0.0f});
    world.set_angular_velocity(a, {0.0f, 2.0f, 0.0f});
    CHECK(near(world.position(a), {1.0f, 2.0f, 3.0f}, 1e-5f));
    CHECK(near(world.rotation(a).y, q.y, 1e-5f) && near(world.rotation(a).w, q.w, 1e-5f));
    CHECK(near(world.linear_velocity(a), {1.0f, 0.0f, 0.0f}, 1e-5f));
    CHECK(near(world.angular_velocity(a), {0.0f, 2.0f, 0.0f}, 1e-5f));
    CHECK(world.shape(a).kind() == Shape3DKind::Box);
}

void test_mass_forces_damping() {
    World3DConfig cfg = serial();
    cfg.gravity = {};
    World3D world(cfg);
    const Shape3D box = Shape3D::box({0.5f, 0.5f, 0.5f});
    const Body3D by_density = world.create_body({.shape = box, .density = 500.0f});
    const Body3D by_mass = world.create_body({.shape = box, .position = {5.0f, 0, 0}, .mass = 7.0f, .density = 1.0f});
    CHECK(near(world.mass(by_density), 500.0f, 0.5f));
    CHECK(near(world.mass(by_mass), 7.0f, 1e-4f));
    CHECK(world.mass(world.create_body({.shape = box, .motion = Body3DMotion::Static, .position = {0, 9, 0}})) == 0.0f);

    // An impulse is momentum: J / m.
    world.add_impulse(by_mass, {14.0f, 0.0f, 0.0f});
    run(world, 1);
    CHECK(near(world.linear_velocity(by_mass).x, 2.0f, 1e-3f));
    // A force acts for the step it is added in: F / m * dt.
    world.add_force(by_density, {0.0f, 0.0f, 1000.0f});
    run(world, 1);
    CHECK(near(world.linear_velocity(by_density).z, 2.0f * dt, 1e-3f));
    run(world, 1); // not added again: no more acceleration
    CHECK(near(world.linear_velocity(by_density).z, 2.0f * dt, 1e-3f));
    // A torque turns it.
    world.add_torque(by_mass, {0.0f, 1.0f, 0.0f});
    run(world, 1);
    CHECK(world.angular_velocity(by_mass).y > 0.0f);

    // Damping: 1 per second takes about a third of the speed off in a second... 1/(1 + c dt) per step.
    World3D damped(cfg);
    const Body3D d = damped.create_body({.shape = box, .linear_velocity = {10.0f, 0, 0}, .linear_damping = 1.0f});
    const Body3D u = damped.create_body({.shape = box, .position = {0, 5, 0}, .linear_velocity = {10.0f, 0, 0}});
    run(damped, 60);
    CHECK(damped.linear_velocity(d).x < 4.0f && damped.linear_velocity(d).x > 3.0f);
    CHECK(near(damped.linear_velocity(u).x, 10.0f, 1e-3f));
}

void test_layers() {
    World3D world(serial());
    // Defaults by motion type.
    const Body3D floor = add_floor(world);
    const Body3D dyn = add_box(world, {0.0f, 4.0f, 0.0f});
    const Body3D kin_body = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .motion = Body3DMotion::Kinematic,
                                               .position = {20.0f, 4.0f, 0.0f}});
    CHECK(world.layer(floor) == layers3d::Static);
    CHECK(world.layer(dyn) == layers3d::Dynamic);
    CHECK(world.layer(kin_body) == layers3d::Kinematic);
    CHECK(world.layers_collide(layers3d::Dynamic, layers3d::Static));
    CHECK(world.layers_collide(layers3d::Static, layers3d::Dynamic));
    CHECK(!world.layers_collide(layers3d::Sensor, layers3d::Sensor));
    CHECK(world.collision_mask(layers3d::Dynamic) == all_layers3d);
    CHECK(layer_bit(5) == 32u && layer_bit(40) == 0u);

    // A layer that does not collide with the floor falls through it; one that does, rests.
    world.set_layers_collide(layers3d::User0, layers3d::Static, false);
    CHECK(!world.layers_collide(layers3d::Static, layers3d::User0));
    CHECK(world.collision_mask(layers3d::User0) == (all_layers3d & ~layer_bit(layers3d::Static)));
    const Shape3D ball = Shape3D::sphere(0.5f);
    const Body3D ghost = world.create_body({.shape = ball, .position = {10.0f, 3.0f, 0.0f}, .layer = layers3d::User0});
    const Body3D solid = world.create_body({.shape = ball, .position = {-10.0f, 3.0f, 0.0f}, .layer = layers3d::User0 + 1});
    run(world, 180);
    CHECK(world.position(ghost).y < -3.0f);
    CHECK(near(world.position(solid).y, 0.5f, 0.04f));
    CHECK(world.layer(ghost) == layers3d::User0);

    // Two dynamic layers that ignore each other pass through; with the pair on they stack.
    auto stacked = [](bool collide) {
        World3D w(serial());
        w.create_body({.shape = Shape3D::box({20.0f, 0.5f, 20.0f}), .motion = Body3DMotion::Static, .position = {0, -0.5f, 0}});
        w.set_layers_collide(layers3d::User0, layers3d::User0 + 1, collide);
        const Shape3D box = Shape3D::box({0.5f, 0.5f, 0.5f});
        w.create_body({.shape = box, .position = {0, 0.5f, 0}, .layer = layers3d::User0});
        const Body3D top = w.create_body({.shape = box, .position = {0, 3.0f, 0}, .layer = layers3d::User0 + 1});
        for (int i = 0; i < 180; ++i) {
            (void)w.step(dt);
        }
        return w.position(top).y;
    };
    CHECK(near(stacked(true), 1.5f, 0.05f));
    CHECK(near(stacked(false), 0.5f, 0.05f));

    // A sensor does not push what falls on it.
    World3D sensors(serial());
    add_floor(sensors);
    sensors.create_body({.shape = Shape3D::box({2.0f, 0.5f, 2.0f}), .motion = Body3DMotion::Static,
                         .position = {0.0f, 1.0f, 0.0f}, .layer = layers3d::Sensor, .sensor = true});
    const Body3D fall = sensors.create_body({.shape = ball, .position = {0.0f, 4.0f, 0.0f}});
    run(sensors, 180);
    CHECK(near(sensors.position(fall).y, 0.5f, 0.05f));
}

void test_destroy_stacked() {
    World3D world(serial());
    add_floor(world);
    std::vector<Body3D> tower;
    for (int i = 0; i < 5; ++i) {
        tower.push_back(add_box(world, {0.0f, 0.5f + 1.001f * static_cast<f32>(i), 0.0f}));
    }
    run(world, 120);
    CHECK(near(world.position(tower[4]).y, 4.5f, 0.15f));
    (void)world.destroy_body(tower[0]);
    CHECK(world.body_count() == 5); // floor and four boxes
    run(world, 120);
    // The rest came down onto the floor: the top box is now 3.5 up.
    CHECK(near(world.position(tower[1]).y, 0.5f, 0.1f));
    CHECK(near(world.position(tower[4]).y, 3.5f, 0.2f));
    for (const Body3D b : tower) {
        (void)world.destroy_body(b);
    }
    CHECK(world.body_count() == 1);
    run(world, 10);
    // Destroying everything in a world and reusing it works.
    const Body3D again = add_box(world, {0.0f, 2.0f, 0.0f});
    run(world, 120);
    CHECK(world.contains(again) && near(world.position(again).y, 0.5f, 0.05f));
}

void test_pile() {
    World3D world(serial());
    build_pile(world, 500);
    run(world, 300);
    CHECK(world.body_count() == 505);
    int escaped = 0;
    int resting = 0;
    // The 500 are the bodies numbered 5 to 504 (floor and four walls come first).
    for (u32 i = 5; i < 505; ++i) {
        const Vec3f p = world.position({i, 0});
        if (!(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) || p.y < -0.1f || std::fabs(p.x) > 8.0f
            || std::fabs(p.z) > 8.0f) {
            ++escaped;
        }
        if (!world.is_active({i, 0})) {
            ++resting;
        }
    }
    CHECK(escaped == 0);
    CHECK(world.active_body_count() + resting == 500);
}

void test_hash_sensitivity() {
    auto make = [](u64 user, Vec3f p, Quatf q, Vec3f v, Vec3f w) {
        World3D world(serial());
        world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = p, .rotation = q, .linear_velocity = v,
                           .angular_velocity = w, .user_id = user});
        return world.state_hash();
    };
    const Quatf q = Quatf::axis_angle({1, 0, 0}, 30.0f);
    const u64 base = make(1, {1, 2, 3}, q, {0.5f, 0, 0}, {0, 0.5f, 0});
    CHECK(make(1, {1, 2, 3}, q, {0.5f, 0, 0}, {0, 0.5f, 0}) == base);
    CHECK(make(2, {1, 2, 3}, q, {0.5f, 0, 0}, {0, 0.5f, 0}) != base); // user id
    CHECK(make(1, {1, 2, 3.001f}, q, {0.5f, 0, 0}, {0, 0.5f, 0}) != base); // position
    CHECK(make(1, {1, 2, 3}, Quatf::axis_angle({1, 0, 0}, 30.1f), {0.5f, 0, 0}, {0, 0.5f, 0}) != base); // rotation
    CHECK(make(1, {1, 2, 3}, q, {0.501f, 0, 0}, {0, 0.5f, 0}) != base); // linear velocity
    CHECK(make(1, {1, 2, 3}, q, {0.5f, 0, 0}, {0, 0.501f, 0}) != base); // angular velocity
    // One last-place bit of one component is enough.
    CHECK(make(1, {1, 2, std::nextafter(3.0f, 4.0f)}, q, {0.5f, 0, 0}, {0, 0.5f, 0}) != base);

    // Which body is which counts: the same two states swapped between two bodies differ.
    auto pair = [](Vec3f p0, Vec3f p1) {
        World3D world(serial());
        const Shape3D s = Shape3D::sphere(0.5f);
        world.create_body({.shape = s, .position = p0});
        world.create_body({.shape = s, .position = p1});
        return world.state_hash();
    };
    CHECK(pair({0, 0, 0}, {5, 0, 0}) != pair({5, 0, 0}, {0, 0, 0}));
    // The same bodies, different creation numbers (a destroyed one's place reused) differ from a fresh world.
    {
        World3D a(serial());
        World3D b(serial());
        const Shape3D s = Shape3D::sphere(0.5f);
        a.create_body({.shape = s, .position = {1, 0, 0}});
        const Body3D gone = a.create_body({.shape = s, .position = {9, 0, 0}});
        (void)a.destroy_body(gone);
        const Body3D kept = a.create_body({.shape = s, .position = {2, 0, 0}});
        b.create_body({.shape = s, .position = {1, 0, 0}});
        b.create_body({.shape = s, .position = {2, 0, 0}});
        CHECK(kept.index == 1 && kept.generation == 1);
        CHECK(a.state_hash() != b.state_hash()); // generation differs
    }
    // An empty world has a hash, and an input after some steps changes the run from there on.
    World3D empty(serial());
    CHECK(empty.state_hash() == World3D(serial()).state_hash());

    World3D one(serial());
    World3D two(serial());
    build_pile(one, 60);
    build_pile(two, 60);
    run(one, 60);
    run(two, 60);
    CHECK(one.state_hash() == two.state_hash());
    two.add_impulse({20, 0}, {0.0f, 0.001f, 0.0f});
    run(one, 1);
    run(two, 1);
    CHECK(one.state_hash() != two.state_hash());
}

void test_determinism() {
    constexpr int frames = 120;
    const std::vector<u64> serial_run = pile_hashes(serial(), 300, frames);
    CHECK(serial_run == pile_hashes(serial(), 300, frames)); // twice
    {
        JobSystem one({.workers = 1, .max_background = 1});
        World3DConfig cfg;
        cfg.jobs = &one;
        CHECK(pile_hashes(cfg, 300, frames) == serial_run);
    }
    {
        JobSystem four({.workers = 4, .max_background = 4});
        World3DConfig cfg;
        cfg.jobs = &four;
        const std::vector<u64> a = pile_hashes(cfg, 300, frames);
        CHECK(a == serial_run);
        CHECK(pile_hashes(cfg, 300, frames) == a);
        // Collision steps are part of the input: 2 differs from 1, and is as repeatable.
        cfg.collision_steps = 2;
        const std::vector<u64> two_steps = pile_hashes(cfg, 300, frames);
        CHECK(two_steps != a);
        CHECK(pile_hashes(cfg, 300, frames) == two_steps);
    }
    // Hashes change over time (a scene that settled to nothing would prove little).
    CHECK(serial_run.front() != serial_run[frames / 2] && serial_run[frames / 2] != serial_run.back());
    CHECK(World3D::cross_platform_deterministic());
}

void test_save_restore() {
    World3D world(serial());
    build_pile(world, 200);
    run(world, 90);
    const std::vector<u8> saved = world.save_state();
    CHECK(!saved.empty());
    const u64 at_save = world.state_hash();
    std::vector<u64> first;
    for (int i = 0; i < 90; ++i) {
        (void)world.step(dt);
        first.push_back(world.state_hash());
    }
    CHECK(world.state_hash() != at_save);
    CHECK(world.restore_state(saved));
    CHECK(world.state_hash() == at_save);
    std::vector<u64> second;
    for (int i = 0; i < 90; ++i) {
        (void)world.step(dt);
        second.push_back(world.state_hash());
    }
    CHECK(first == second);

    // Another world with the same bodies can take the state too.
    World3D other(serial());
    build_pile(other, 200);
    CHECK(other.restore_state(saved));
    CHECK(other.state_hash() == at_save);

    // A different set of bodies is refused, and nothing changes.
    world.create_body({.shape = Shape3D::sphere(0.5f), .position = {0, 50, 0}});
    const u64 before = world.state_hash();
    CHECK(!world.restore_state(saved));
    CHECK(world.state_hash() == before);
    CHECK(!world.restore_state(std::span<const u8>{}));
    const std::vector<u8> junk(32, 0xAB);
    CHECK(!world.restore_state(junk));
}


// --- P2b: queries ---

QueryFilter3D any_body() { return {}; }

// A down ray at (x, z) from y = 10 to y = -5 against what is there.
std::optional<RayHit3D> ray_down(const World3D& world, f32 x, f32 z, const QueryFilter3D& f = {}) {
    return world.raycast({x, 10.0f, z}, {x, -5.0f, z}, f);
}

void test_raycast_each_shape() {
    World3D world(serial());
    const auto put = [&](Shape3D shape, Vec3f at, u64 id) {
        return world.create_body({.shape = std::move(shape), .motion = Body3DMotion::Static, .position = at, .user_id = id});
    };
    const Body3D box = put(Shape3D::box({0.5f, 0.5f, 0.5f}), {0, 0.5f, 0}, 11);
    const Body3D sphere = put(Shape3D::sphere(0.5f), {3, 0.5f, 0}, 12);
    const Body3D capsule = put(Shape3D::capsule(0.5f, 0.25f), {6, 1.0f, 0}, 13);
    const Body3D cylinder = put(Shape3D::cylinder(0.5f, 0.5f), {9, 1.0f, 0}, 14);
    const std::array<Vec3f, 8> cube = {Vec3f{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
                                       {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f},  {0.5f, 0.5f, 0.5f}};
    const Body3D hull = put(Shape3D::convex_hull(cube), {12, 0.5f, 0}, 15);
    // A quad at y = 2, wound so that its front faces +Y.
    const std::array<Vec3f, 4> quad = {Vec3f{-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1}};
    const std::array<u32, 6> quad_indices = {0, 1, 2, 0, 2, 3};
    const Body3D mesh = put(Shape3D::triangle_mesh(quad, quad_indices), {15, 2, 0}, 16);
    std::array<f32, 64> flat;
    flat.fill(1.0f);
    const Body3D field = put(Shape3D::height_field(flat, 8, {0, 0, 0}, {1, 1, 1}), {18, 0, 0}, 17);
    const std::array<ShapeChild3D, 2> parts = {ShapeChild3D{Shape3D::box({0.5f, 0.5f, 0.5f}), {-1, 0, 0}, {}},
                                               ShapeChild3D{Shape3D::box({0.5f, 1.0f, 0.5f}), {1, 0, 0}, {}}};
    const Body3D compound = put(Shape3D::compound(parts), {25, 1.0f, 0}, 18);
    world.optimize_broad_phase();

    {
        const auto hit = ray_down(world, 0.1f, 0.1f);
        CHECK(hit && hit->body == box && hit->user_id == 11);
        CHECK(hit && near(hit->point, {0.1f, 1.0f, 0.1f}, 1e-4f));
        CHECK(hit && near(hit->normal, {0, 1, 0}, 1e-4f));
        CHECK(hit && near(hit->fraction, 9.0f / 15.0f, 1e-5f) && near(hit->distance, 9.0f, 1e-4f));
    }
    {
        // A box's side: a ray along +x from the left meets the face x = -0.5, normal -x.
        const auto hit = world.raycast({-4, 0.5f, 0.1f}, {4, 0.5f, 0.1f});
        CHECK(hit && hit->body == box && near(hit->point.x, -0.5f, 1e-4f) && near(hit->normal, {-1, 0, 0}, 1e-4f));
    }
    {
        const auto top = ray_down(world, 3.0f, 0.0f);
        CHECK(top && top->body == sphere && near(top->point.y, 1.0f, 1e-4f) && near(top->normal, {0, 1, 0}, 1e-4f));
        // Off centre: y = 0.5 + sqrt(0.25 - 0.09) = 0.9, the normal (0.6, 0.8, 0).
        const auto side = ray_down(world, 3.3f, 0.0f);
        CHECK(side && near(side->point.y, 0.9f, 1e-3f) && near(side->normal, {0.6f, 0.8f, 0.0f}, 1e-3f));
    }
    {
        const auto hit = ray_down(world, 6.0f, 0.0f);
        CHECK(hit && hit->body == capsule && near(hit->point.y, 1.75f, 1e-4f) && near(hit->normal, {0, 1, 0}, 1e-3f));
    }
    {
        const auto hit = ray_down(world, 9.0f, 0.0f);
        CHECK(hit && hit->body == cylinder && near(hit->point.y, 1.5f, 1e-4f) && near(hit->normal, {0, 1, 0}, 1e-3f));
    }
    {
        const auto hit = ray_down(world, 12.1f, 0.1f);
        CHECK(hit && hit->body == hull && near(hit->point.y, 1.0f, 1e-3f) && near(hit->normal, {0, 1, 0}, 1e-3f));
    }
    {
        // A mesh is hit from above and from below, and its normal is the front's both times.
        const auto above = ray_down(world, 15.2f, 0.1f);
        CHECK(above && above->body == mesh && near(above->point.y, 2.0f, 1e-4f) && near(above->normal, {0, 1, 0}, 1e-4f));
        const auto below = world.raycast({15.2f, -3.0f, 0.1f}, {15.2f, 5.0f, 0.1f});
        CHECK(below && below->body == mesh && near(below->point.y, 2.0f, 1e-4f) && near(below->normal, {0, 1, 0}, 1e-4f));
    }
    {
        const auto hit = ray_down(world, 21.5f, 3.5f);
        CHECK(hit && hit->body == field && near(hit->point.y, 1.0f, 0.02f) && hit->normal.y > 0.99f);
    }
    {
        // The compound reports which child: x = 24 is child 0 (top y = 1.5), x = 26 child 1 (top y = 2.0).
        const auto left = ray_down(world, 24.0f, 0.0f);
        CHECK(left && left->body == compound && left->sub_shape == 0 && near(left->point.y, 1.5f, 1e-4f));
        const auto right = ray_down(world, 26.0f, 0.0f);
        CHECK(right && right->body == compound && right->sub_shape == 1 && near(right->point.y, 2.0f, 1e-4f));
    }
    // Nothing there, and a degenerate ray.
    CHECK(!ray_down(world, 40.0f, 40.0f));
    CHECK(!world.raycast({1, 1, 1}, {1, 1, 1}));
    // A ray that starts inside a convex solid hits it at once.
    const auto inside = world.raycast({0, 0.5f, 0}, {0, -5, 0});
    CHECK(inside && inside->body == box && inside->fraction == 0.0f);
}

void test_query_filters_and_order() {
    World3D world(serial());
    // Three boxes in a column at y = 0.5 (layer Static), 2.5 (layer User0), 4.5 (layer User0+1).
    const Body3D low = world.create_body({.shape = Shape3D::box({1, 0.5f, 1}), .motion = Body3DMotion::Static,
                                          .position = {0, 0.5f, 0}, .user_id = 1});
    const Body3D mid = world.create_body({.shape = Shape3D::box({1, 0.5f, 1}), .motion = Body3DMotion::Static,
                                          .position = {0, 2.5f, 0}, .layer = layers3d::User0, .user_id = 2});
    const Body3D high = world.create_body({.shape = Shape3D::box({1, 0.5f, 1}), .motion = Body3DMotion::Static,
                                           .position = {0, 4.5f, 0}, .layer = layers3d::User0 + 1, .user_id = 3});
    const Body3D trigger = world.create_body({.shape = Shape3D::box({1, 0.5f, 1}), .motion = Body3DMotion::Static,
                                              .position = {0, 6.5f, 0}, .layer = layers3d::Sensor, .sensor = true, .user_id = 4});
    world.optimize_broad_phase();
    const Vec3f from{0, 10, 0}, to{0, -2, 0};

    // The nearest first: with the sensor in, then without (the default).
    CHECK(world.raycast(from, to, {.sensors = true})->body == trigger);
    CHECK(world.raycast(from, to)->body == high);
    // Layer mask: leave out `high`'s layer, then both User layers.
    CHECK(world.raycast(from, to, {.mask = all_layers3d & ~layer_bit(layers3d::User0 + 1)})->body == mid);
    CHECK(world.raycast(from, to, {.mask = layer_bit(layers3d::Static)})->body == low);
    CHECK(!world.raycast(from, to, {.mask = layer_bit(layers3d::Character)}));
    // Ignore body.
    CHECK(world.raycast(from, to, {.ignore = high})->body == mid);
    CHECK(world.raycast(from, to, {.mask = layer_bit(layers3d::Static), .ignore = low}) == std::nullopt);

    // All of them, nearest first, by distance.
    const auto all = world.raycast_all(from, to);
    CHECK(all.size() == 3);
    if (all.size() == 3) {
        CHECK(all[0].body == high && all[1].body == mid && all[2].body == low);
        CHECK(all[0].fraction < all[1].fraction && all[1].fraction < all[2].fraction);
        CHECK(near(all[0].point.y, 5.0f, 1e-4f) && near(all[2].point.y, 1.0f, 1e-4f));
    }
    CHECK(world.raycast_all(from, to, {.sensors = true}).size() == 4);
    // The segment ends before the lowest.
    CHECK(world.raycast_all(from, {0, 4, 0}).size() == 1);
    const auto reversed = world.raycast_all(to, from); // from below: lowest first
    CHECK(reversed.size() == 3 && reversed[0].body == low);

    // Overlap: a sphere over the column's middle, sorted by body number whatever the order they were found in.
    const auto both = world.overlap(Shape3D::sphere(1.2f), {0, 3.5f, 0}, {});
    CHECK(both.size() == 2);
    if (both.size() == 2) {
        CHECK(both[0].body == mid && both[1].body == high);
        CHECK(both[0].body.index < both[1].body.index);
        CHECK(both[0].penetration > 0.0f && both[0].user_id == 2);
    }
    CHECK(world.overlap(Shape3D::sphere(1.2f), {0, 3.5f, 0}, {}, {.ignore = mid}).size() == 1);
    CHECK(world.overlap(Shape3D::sphere(1.2f), {0, 3.5f, 0}, {}, {.mask = layer_bit(layers3d::User0)}).size() == 1);
    CHECK(world.overlap(Shape3D::sphere(0.2f), {5, 5, 5}, {}).empty());
    // Sensors: left out unless asked for.
    CHECK(world.overlap(Shape3D::sphere(0.3f), {0, 6.5f, 0}, {}).empty());
    CHECK(world.overlap(Shape3D::sphere(0.3f), {0, 6.5f, 0}, {}, {.sensors = true}).size() == 1);

    // Points.
    const auto inside = world.point_inside({0, 2.5f, 0});
    CHECK(inside.size() == 1 && inside[0] == mid);
    CHECK(world.point_inside({0, 3.5f, 0}).empty());
    CHECK(world.point_inside({0, 6.5f, 0}).empty() && world.point_inside({0, 6.5f, 0}, {.sensors = true}).size() == 1);
    const auto closest = world.closest_point({0, 3.5f, 0}, 5.0f);
    CHECK(closest && (closest->body == mid || closest->body == high) && near(closest->distance, 0.5f, 1e-3f));
    CHECK(closest && near(closest->point.y, 3.0f, 1e-2f) + near(closest->point.y, 4.0f, 1e-2f) == 1);
    const auto none = world.closest_point({20, 3.5f, 0}, 1.0f);
    CHECK(!none);
    const auto in = world.closest_point({0, 2.5f, 0}, 1.0f);
    CHECK(in && in->body == mid && in->distance == 0.0f);
}

void test_shape_cast() {
    World3D world(serial());
    add_floor(world);
    const Body3D wall = world.create_body({.shape = Shape3D::box({0.5f, 2.0f, 2.0f}), .motion = Body3DMotion::Static,
                                           .position = {3.0f, 2.0f, 0.0f}, .user_id = 7});
    world.optimize_broad_phase();
    // A sphere of 0.5 falls from 5 m: it touches the floor (y = 0) with its centre at 0.5: after 4.5 of 10 m.
    {
        const auto hit = world.shape_cast(Shape3D::sphere(0.5f), {0, 5, 0}, {}, {0, -10, 0});
        CHECK(hit && near(hit->fraction, 0.45f, 2e-3f));
        CHECK(hit && near(hit->normal, {0, 1, 0}, 1e-3f) && near(hit->point.y, 0.0f, 2e-3f));
        CHECK(hit && near(hit->position, {0, 0.5f, 0}, 2e-2f));
        CHECK(hit && hit->penetration == 0.0f);
    }
    // A box of half 0.5 along +x at wall x = 2.5: its centre meets it at x = 2 (7 of 10 m from -5), normal -x.
    {
        const auto hit = world.shape_cast(Shape3D::box({0.5f, 0.5f, 0.5f}), {-5, 2, 0}, {}, {10, 0, 0});
        CHECK(hit && hit->body == wall && hit->user_id == 7);
        CHECK(hit && near(hit->fraction, 0.7f, 2e-3f));
        CHECK(hit && near(hit->normal, {-1, 0, 0}, 1e-3f));
        CHECK(hit && near(hit->point.x, 2.5f, 2e-3f));
    }
    // A rotated box (45 degrees about y) meets the wall corner first: centre at 2.5 - 0.7071.
    {
        const Quatf turned = Quatf::axis_angle({0, 1, 0}, 45.0f);
        const auto hit = world.shape_cast(Shape3D::box({0.5f, 0.5f, 0.5f}), {-5, 2, 0}, turned, {10, 0, 0});
        CHECK(hit && near(hit->position.x, 2.5f - 0.70711f, 0.02f));
    }
    // Ends short: no hit. Ignore the wall: the floor is not on the way.
    CHECK(!world.shape_cast(Shape3D::box({0.5f, 0.5f, 0.5f}), {-5, 2, 0}, {}, {5, 0, 0}));
    CHECK(!world.shape_cast(Shape3D::box({0.5f, 0.5f, 0.5f}), {-5, 2, 0}, {}, {10, 0, 0}, {.ignore = wall}));
    CHECK(!world.shape_cast(Shape3D::box({0.5f, 0.5f, 0.5f}), {-5, 2, 0}, {}, {10, 0, 0}, {.mask = layer_bit(layers3d::Character)}));
    // It starts inside: fraction 0 with a penetration.
    {
        const auto hit = world.shape_cast(Shape3D::sphere(0.5f), {0, 0.2f, 0}, {}, {0, -1, 0});
        CHECK(hit && hit->fraction == 0.0f && hit->penetration > 0.1f);
    }
    // Initial overlap must also be reported while moving out of a solid.
    {
        const auto hit = world.shape_cast(Shape3D::sphere(0.5f), {0, 0.2f, 0}, {}, {0, 2, 0}, {.initial_overlap = true});
        CHECK(hit && hit->fraction == 0.0f && hit->penetration > 0.1f);
    }
    // Meshes cannot be swept.
    const std::array<Vec3f, 3> tri = {Vec3f{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    const std::array<u32, 3> idx = {0, 1, 2};
    CHECK(!world.shape_cast(Shape3D::triangle_mesh(tri, idx), {0, 5, 0}, {}, {0, -10, 0}));
}

// --- P2b: events ---

struct EventLog {
    std::vector<ContactEvent3D> all;
    void take(World3D& world) {
        for (const ContactEvent3D& e : world.drain_events()) {
            all.push_back(e);
        }
    }
    int count(Body3D a, Body3D b, ContactKind3D kind) const {
        int n = 0;
        for (const ContactEvent3D& e : all) {
            if (e.a.index == std::min(a.index, b.index) && e.b.index == std::max(a.index, b.index) && e.kind == kind) {
                ++n;
            }
        }
        return n;
    }
};

// What the order of a run's events is, as one number: every field of every event.
u64 events_hash(std::span<const ContactEvent3D> events, u64 h = 0xcbf29ce484222325ull) {
    const auto mix = [&](u64 v) { h = (h ^ v) * 0x100000001b3ull; };
    for (const ContactEvent3D& e : events) {
        mix(e.a.index);
        mix(e.a.generation);
        mix(e.b.index);
        mix(e.b.generation);
        mix(e.user_a);
        mix(e.user_b);
        mix(static_cast<u64>(e.kind));
        mix(e.sensor ? 1 : 0);
        mix(std::bit_cast<u32>(e.point.x));
        mix(std::bit_cast<u32>(e.point.y));
        mix(std::bit_cast<u32>(e.point.z));
        mix(std::bit_cast<u32>(e.normal.x));
        mix(std::bit_cast<u32>(e.normal.y));
        mix(std::bit_cast<u32>(e.normal.z));
        mix(std::bit_cast<u32>(e.closing_speed));
        mix(e.point_count);
    }
    return h;
}

void test_events_begin_end_once() {
    World3D world(serial());
    const Body3D floor = add_floor(world, 0.5f, 0.0f);
    // A box with no bounce lands once and stays (and sleeps): one Begin, no End, however long we run.
    const Body3D box = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 2, 0}, .user_id = 77});
    EventLog log;
    int sleeping_at = -1;
    for (int i = 0; i < 400; ++i) {
        (void)world.step(dt);
        log.take(world);
        if (sleeping_at < 0 && !world.is_active(box)) {
            sleeping_at = i;
        }
    }
    CHECK(sleeping_at > 0 && sleeping_at < 380);
    CHECK(log.count(floor, box, ContactKind3D::Begin) == 1);
    CHECK(log.count(floor, box, ContactKind3D::End) == 0);
    CHECK(log.all.size() == 1);
    if (!log.all.empty()) {
        const ContactEvent3D& e = log.all[0];
        // floor is body 0, the box body 1: a = floor, the normal from the floor up to the box.
        CHECK(e.a == floor && e.b == box && e.user_b == 77 && e.user_a == 0);
        CHECK(!e.sensor && e.normal.y > 0.99f && near(e.point.y, 0.0f, 0.03f));
        // It arrived at sqrt(2 g h) with h = 1.5 m.
        CHECK(near(e.closing_speed, std::sqrt(2.0f * 9.81f * 1.5f), 0.4f));
        CHECK(e.point_count >= 1);
    }
    // Destroying the resting box ends the contact (once), reported by the next step.
    (void)world.destroy_body(box);
    (void)world.step(dt);
    log.take(world);
    CHECK(log.count(floor, box, ContactKind3D::End) == 1);
    CHECK(log.all.size() == 2);
}

void test_events_bounce() {
    World3DConfig cfg = serial();
    World3D world(cfg);
    const Body3D floor = add_floor(world, 0.5f, 0.8f);
    const Body3D ball = world.create_body({.shape = Shape3D::sphere(0.3f), .position = {0, 3, 0}, .friction = 0.5f,
                                           .restitution = 0.8f});
    EventLog log;
    int begins_seen_from_state = 0;
    bool was_touching = false;
    for (int i = 0; i < 600; ++i) {
        (void)world.step(dt);
        log.take(world);
        const bool touching = world.position(ball).y < 0.3f + 0.03f && std::fabs(world.linear_velocity(ball).y) < 0.5f;
        was_touching = touching;
        (void)begins_seen_from_state;
    }
    (void)was_touching;
    // Alternates Begin, End, Begin, ... for the pair: never two of a kind in a row, and the first is a Begin.
    CHECK(!log.all.empty() && log.all.front().kind == ContactKind3D::Begin);
    for (std::size_t i = 1; i < log.all.size(); ++i) {
        CHECK(log.all[i].kind != log.all[i - 1].kind);
        CHECK(log.all[i].a == floor && log.all[i].b == ball);
    }
    const int begins = log.count(floor, ball, ContactKind3D::Begin);
    const int ends = log.count(floor, ball, ContactKind3D::End);
    // Drops from 3 m at 0.8 restitution: the bounces go 1.92, 1.23, 0.79 ... m: many, then it settles into contact.
    CHECK(begins >= 6);
    CHECK(begins == ends + 1); // ends in contact
    // Each bounce's Begin closes at less speed than the one before.
    f32 last = 1e9f;
    int checked = 0;
    for (const ContactEvent3D& e : log.all) {
        if (e.kind == ContactKind3D::Begin && checked < 4) {
            CHECK(e.closing_speed < last);
            last = e.closing_speed;
            ++checked;
        }
    }
}

void test_events_sensor() {
    World3D world(serial());
    add_floor(world);
    // A trigger box over y 1 to 2 (static sensor: detects awake bodies passing through it, pushes nothing).
    const Body3D trigger = world.create_body({.shape = Shape3D::box({2, 0.5f, 2}), .motion = Body3DMotion::Static,
                                              .position = {0, 1.5f, 0}, .layer = layers3d::Sensor, .sensor = true,
                                              .user_id = 900});
    const Body3D ball = world.create_body({.shape = Shape3D::sphere(0.25f), .position = {0, 4, 0}, .user_id = 5});
    EventLog log;
    f32 lowest = 10.0f;
    for (int i = 0; i < 120; ++i) {
        (void)world.step(dt);
        log.take(world);
        lowest = std::min(lowest, world.position(ball).y);
    }
    CHECK(lowest < 0.3f); // it fell through the trigger to the floor (nothing was pushed)
    int enter = 0, leave = 0;
    for (const ContactEvent3D& e : log.all) {
        if (e.sensor) {
            CHECK((e.a == trigger || e.b == trigger));
            enter += e.kind == ContactKind3D::Begin;
            leave += e.kind == ContactKind3D::End;
        }
    }
    CHECK(enter == 1 && leave == 1);
    // The events are in order: the enter before the leave.
    int enter_at = -1, leave_at = -1;
    for (std::size_t i = 0; i < log.all.size(); ++i) {
        if (log.all[i].sensor) {
            (log.all[i].kind == ContactKind3D::Begin ? enter_at : leave_at) = static_cast<int>(i);
        }
    }
    CHECK(enter_at >= 0 && leave_at > enter_at);
    // A sensor is excluded from queries by default but its body exists and is reported by is_sensor.
    CHECK(world.is_sensor(trigger) && !world.is_sensor(ball));
}

// A pile with events: every step's batch hashed in order, to compare runs.
std::vector<u64> pile_event_hashes(World3DConfig config, int count, int frames, bool persist, std::size_t* total = nullptr) {
    config.persist_events = persist;
    config.contact_events = true;
    World3D world(config);
    build_pile(world, count);
    std::vector<u64> hashes;
    std::size_t n = 0;
    for (int i = 0; i < frames; ++i) {
        (void)world.step(dt);
        const std::span<const ContactEvent3D> events = world.events();
        // Each batch is sorted by (a, b, kind).
        for (std::size_t k = 1; k < events.size(); ++k) {
            const ContactEvent3D& x = events[k - 1];
            const ContactEvent3D& y = events[k];
            CHECK(std::tie(x.a.index, x.b.index, x.kind) <= std::tie(y.a.index, y.b.index, y.kind));
        }
        n += events.size();
        hashes.push_back(events_hash(events, world.state_hash()));
        world.drain_events();
    }
    if (total != nullptr) {
        *total = n;
    }
    return hashes;
}

void test_events_deterministic() {
    constexpr int frames = 100;
    std::size_t total = 0;
    const std::vector<u64> base = pile_event_hashes(serial(), 200, frames, false, &total);
    CHECK(total > 200); // a pile makes many begins
    CHECK(base == pile_event_hashes(serial(), 200, frames, false)); // twice
    {
        JobSystem one({.workers = 1, .max_background = 1});
        World3DConfig cfg;
        cfg.jobs = &one;
        CHECK(pile_event_hashes(cfg, 200, frames, false) == base);
    }
    {
        JobSystem four({.workers = 4, .max_background = 4});
        World3DConfig cfg;
        cfg.jobs = &four;
        CHECK(pile_event_hashes(cfg, 200, frames, false) == base);
        CHECK(pile_event_hashes(cfg, 200, frames, false) == base);
    }
    // With persist events too, on workers and not.
    std::size_t with_persist = 0;
    const std::vector<u64> persist = pile_event_hashes(serial(), 120, 60, true, &with_persist);
    {
        JobSystem four({.workers = 4, .max_background = 4});
        World3DConfig cfg;
        cfg.jobs = &four;
        CHECK(pile_event_hashes(cfg, 120, 60, true) == persist);
    }
    std::size_t without_persist = 0;
    pile_event_hashes(serial(), 120, 60, false, &without_persist);
    CHECK(with_persist > without_persist);
    // Two collision steps: Jolt reports a pair once a collision step, so a step holds two records of one pair and kind with
    // different points and speeds, and the one the event carries must not depend on which thread's record came first.
    {
        World3DConfig cfg = serial();
        cfg.collision_steps = 2;
        const std::vector<u64> two_steps = pile_event_hashes(cfg, 120, 60, true);
        CHECK(two_steps == pile_event_hashes(cfg, 120, 60, true));
        JobSystem one({.workers = 1, .max_background = 1});
        World3DConfig cfg_one;
        cfg_one.jobs = &one;
        cfg_one.collision_steps = 2;
        CHECK(pile_event_hashes(cfg_one, 120, 60, true) == two_steps);
        JobSystem four({.workers = 4, .max_background = 4});
        World3DConfig cfg_four;
        cfg_four.jobs = &four;
        cfg_four.collision_steps = 2;
        for (int run = 0; run < 3; ++run) {
            CHECK(pile_event_hashes(cfg_four, 120, 60, true) == two_steps);
        }
    }
}


// Pairs with many sub shapes (compound bodies on a triangle mesh floor): one Begin a pair, its data from the lowest sub
// shape pair, so the same on every worker count and run.
std::vector<u64> compound_event_hashes(World3DConfig config, int frames, std::size_t* begins = nullptr) {
    config.contact_events = true;
    World3D world(config);
    std::vector<Vec3f> v;
    std::vector<u32> idx;
    constexpr int n = 12;
    for (int z = 0; z <= n; ++z) {
        for (int x = 0; x <= n; ++x) {
            v.push_back({static_cast<f32>(x) - 6.0f, 0.15f * std::sin(static_cast<f32>(x) * 0.9f) * std::cos(static_cast<f32>(z) * 0.7f),
                         static_cast<f32>(z) - 6.0f});
        }
    }
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const u32 i = static_cast<u32>(z * (n + 1) + x);
            idx.insert(idx.end(), {i, i + n + 1, i + 1, i + 1, i + n + 1, i + n + 2});
        }
    }
    world.create_body({.shape = Shape3D::triangle_mesh(v, idx), .motion = Body3DMotion::Static, .friction = 0.5f});
    const std::array<ShapeChild3D, 3> parts = {ShapeChild3D{Shape3D::box({0.3f, 0.15f, 0.15f}), {-0.3f, 0, 0}, {}},
                                               ShapeChild3D{Shape3D::box({0.3f, 0.15f, 0.15f}), {0.3f, 0, 0}, {}},
                                               ShapeChild3D{Shape3D::sphere(0.2f), {0, 0.25f, 0}, {}}};
    const Shape3D shape = Shape3D::compound(parts);
    Lcg rng;
    for (int i = 0; i < 60; ++i) {
        world.create_body({.shape = shape,
                           .position = {(rng.next() - 0.5f) * 8.0f, 0.8f + 0.5f * static_cast<f32>(i / 12), (rng.next() - 0.5f) * 8.0f},
                           .rotation = Quatf::axis_angle({rng.next(), rng.next() + 0.1f, rng.next()}, rng.next() * 360.0f),
                           .friction = 0.5f, .user_id = static_cast<u64>(i)});
    }
    world.optimize_broad_phase();
    std::vector<u64> hashes;
    std::size_t b = 0;
    for (int i = 0; i < frames; ++i) {
        (void)world.step(dt);
        for (const ContactEvent3D& e : world.events()) {
            b += e.kind == ContactKind3D::Begin ? 1 : 0;
        }
        hashes.push_back(events_hash(world.events(), world.state_hash()));
        world.drain_events();
    }
    if (begins != nullptr) {
        *begins = b;
    }
    return hashes;
}

void test_events_deterministic_sub_shapes() {
    std::size_t begins = 0;
    const std::vector<u64> base = compound_event_hashes(serial(), 150, &begins);
    CHECK(begins > 60);
    CHECK(base == compound_event_hashes(serial(), 150));
    JobSystem four({.workers = 4, .max_background = 4});
    World3DConfig cfg;
    cfg.jobs = &four;
    for (int run = 0; run < 3; ++run) {
        CHECK(compound_event_hashes(cfg, 150) == base);
    }
}

// Events made by destroying bodies join the batch of the next step in its order, not the order of the calls.
void test_events_destroy_order() {
    World3D world(serial());
    const Body3D floor = add_floor(world);
    std::vector<Body3D> boxes;
    for (int i = 0; i < 4; ++i) {
        boxes.push_back(world.create_body({.shape = Shape3D::box({0.4f, 0.4f, 0.4f}),
                                           .position = {2.0f * static_cast<f32>(i), 0.4f, 0}}));
    }
    run(world, 150);
    world.drain_events();
    // A new body lands in the same step in which two others are destroyed, the higher one first.
    (void)world.destroy_body(boxes[3]);
    (void)world.destroy_body(boxes[1]);
    const Body3D fresh = world.create_body({.shape = Shape3D::box({0.4f, 0.4f, 0.4f}), .position = {1.0f, 0.41f, 3.0f}});
    (void)world.step(dt);
    const std::span<const ContactEvent3D> events = world.events();
    CHECK(events.size() >= 3);
    for (std::size_t k = 1; k < events.size(); ++k) {
        CHECK(std::tie(events[k - 1].a.index, events[k - 1].b.index, events[k - 1].kind)
              <= std::tie(events[k].a.index, events[k].b.index, events[k].kind));
    }
    int ends = 0;
    for (const ContactEvent3D& e : events) {
        ends += e.kind == ContactKind3D::End && e.a == floor ? 1 : 0;
    }
    CHECK(ends == 2);
    (void)fresh;
}

void test_events_persist_and_drain() {
    World3DConfig cfg = serial();
    cfg.persist_events = true;
    World3D world(cfg);
    const Body3D floor = add_floor(world);
    const Body3D box = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 0.6f, 0}});
    int persists = 0;
    EventLog log;
    for (int i = 0; i < 20; ++i) {
        (void)world.step(dt);
        // events() is what is waiting, drain_events() takes it.
        const std::size_t waiting = world.events().size();
        const std::vector<ContactEvent3D> got = world.drain_events();
        CHECK(got.size() == waiting && world.events().empty());
        for (const ContactEvent3D& e : got) {
            persists += e.kind == ContactKind3D::Persist;
            log.all.push_back(e);
        }
    }
    CHECK(persists >= 5);
    CHECK(log.count(floor, box, ContactKind3D::Begin) == 1);
    // Off by config: none, and no listener cost.
    World3DConfig off = serial();
    off.contact_events = false;
    World3D quiet(off);
    add_floor(quiet);
    quiet.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 0.6f, 0}});
    run(quiet, 30);
    CHECK(quiet.events().empty());
}

// --- P2b: constraints ---

void test_hinge_limits_and_motor() {
    World3DConfig cfg = serial();
    cfg.gravity = {0, 0, 0};
    World3D world(cfg);
    // A door: a static post and a plank hinged at its edge, turning about Y.
    // (The post is a body of its own, joined to the door: connected bodies do not collide by default.)
    const Body3D post = world.create_body({.shape = Shape3D::box({0.05f, 1, 0.05f}), .motion = Body3DMotion::Static,
                                           .position = {0, 1, 0}});
    const Body3D door = world.create_body({.shape = Shape3D::box({0.5f, 0.9f, 0.03f}), .position = {0.55f, 1, 0}, .mass = 10.0f,
                                           .angular_damping = 0.1f});
    Constraint3DDesc d;
    d.kind = Constraint3DKind::Hinge;
    d.a = post;
    d.b = door;
    d.point = {0.05f, 1, 0};
    d.axis = {0, 1, 0};
    d.limits = true;
    d.min_angle = -30.0f;
    d.max_angle = 45.0f;
    const Constraint3D hinge = world.create_constraint(d);
    CHECK(hinge.valid() && world.contains(hinge) && world.constraint_count() == 1);
    // Torque pushes it one way for 3 seconds: it stops at the limit (45), the hinge point stays put.
    f32 peak = 0.0f;
    for (int i = 0; i < 180; ++i) {
        world.add_torque(door, {0, 20.0f, 0});
        (void)world.step(dt);
        peak = std::max(peak, world.constraint_info(hinge).value);
    }
    f32 angle = world.constraint_info(hinge).value;
    CHECK(near(std::fabs(angle), 45.0f, 1.5f) && peak < 46.5f);
    CHECK(std::fabs(angle) > 40.0f);
    // The hinge point does not drift: the door's edge is where the post is.
    const Vec3f edge = world.position(door) + world.rotation(door).rotate({-0.5f, 0, 0});
    CHECK(near(edge, {0.05f, 1, 0}, 0.02f));
    // The other way: -30.
    f32 trough = 0.0f;
    for (int i = 0; i < 240; ++i) {
        world.add_torque(door, {0, -20.0f, 0});
        (void)world.step(dt);
        trough = std::min(trough, world.constraint_info(hinge).value);
    }
    CHECK(near(world.constraint_info(hinge).value, -30.0f, 1.5f) && trough > -31.5f);

    // The motor: turns at 90 degrees a second without limits.
    World3D free_world(cfg);
    const Body3D p2 = free_world.create_body({.shape = Shape3D::box({0.05f, 1, 0.05f}), .motion = Body3DMotion::Static,
                                              .position = {0, 1, 0}});
    const Body3D d2 = free_world.create_body({.shape = Shape3D::box({0.5f, 0.9f, 0.03f}), .position = {0.55f, 1, 0}, .mass = 10.0f});
    Constraint3DDesc m = d;
    m.a = p2;
    m.b = d2;
    m.limits = false;
    const Constraint3D motor = free_world.create_constraint(m);
    free_world.set_constraint_motor(motor, Motor3DMode::Velocity, 90.0f, 500.0f);
    run(free_world, 60);
    CHECK(near(std::fabs(free_world.constraint_info(motor).value), 90.0f, 8.0f));
    CHECK(near(std::fabs(free_world.angular_velocity(d2).y), 3.14159f / 2.0f, 0.1f));
    // Motor off: it keeps coasting (no gravity, no damping) and a position motor goes to its angle.
    free_world.set_constraint_motor(motor, Motor3DMode::Position, 20.0f, 500.0f);
    run(free_world, 180);
    CHECK(near(free_world.constraint_info(motor).value, 20.0f, 2.0f));
}

void test_distance_point_slider_cone() {
    World3DConfig cfg = serial();
    World3D world(cfg);
    // A pendulum on a 2 m rod from the world (a invalid): the distance holds while it swings.
    const Body3D bob = world.create_body({.shape = Shape3D::sphere(0.2f), .position = {2, 5, 0}, .mass = 2.0f});
    Constraint3DDesc rod;
    rod.kind = Constraint3DKind::Distance;
    rod.b = bob;
    rod.point = {0, 5, 0};
    rod.point_b = {2, 5, 0};
    const Constraint3D rope = world.create_constraint(rod);
    CHECK(rope.valid());
    f32 lo = 1e9f, hi = 0.0f;
    f32 lowest_y = 1e9f;
    for (int i = 0; i < 240; ++i) {
        (void)world.step(dt);
        const f32 length = world.position(bob).x * 0 + std::sqrt(world.position(bob).x * world.position(bob).x
                                                                 + (world.position(bob).y - 5.0f) * (world.position(bob).y - 5.0f));
        lo = std::min(lo, length);
        hi = std::max(hi, length);
        lowest_y = std::min(lowest_y, world.position(bob).y);
    }
    CHECK(lo > 1.97f && hi < 2.03f);
    CHECK(lowest_y < 3.2f); // it swung down to about 3 m
    CHECK(near(world.constraint_info(rope).value, 2.0f, 0.03f));

    // A range: min 1, max 3 with the bob dropped from the top: it falls to the max and stops there.
    World3D range(cfg);
    const Body3D b2 = range.create_body({.shape = Shape3D::sphere(0.2f), .position = {0, 4, 0}, .mass = 1.0f});
    Constraint3DDesc r2;
    r2.kind = Constraint3DKind::Distance;
    r2.b = b2;
    r2.point = {0, 5, 0};
    r2.point_b = {0, 4, 0};
    r2.min_distance = 1.0f;
    r2.max_distance = 3.0f;
    range.create_constraint(r2);
    run(range, 240);
    CHECK(near(range.position(b2).y, 2.0f, 0.1f)); // 5 - 3

    // A spring (3 Hz): it stretches past the rigid length under load and oscillates, so it ends near but not at max.
    World3D springy(cfg);
    const Body3D b3 = springy.create_body({.shape = Shape3D::sphere(0.2f), .position = {0, 4, 0}, .mass = 1.0f});
    Constraint3DDesc r3 = r2;
    r3.b = b3;
    r3.min_distance = 1.0f;
    r3.max_distance = 1.0f;
    r3.spring_frequency = 0.6f; // k = m (2 pi f)^2 = 14 N/m: it hangs about 0.7 m below its length
    r3.spring_damping = 0.3f;
    springy.create_constraint(r3);
    f32 deepest = 5.0f;
    for (int i = 0; i < 120; ++i) {
        (void)springy.step(dt);
        deepest = std::min(deepest, springy.position(b3).y);
    }
    CHECK(deepest < 3.6f && deepest > 1.0f); // a rigid rod of 1 m holds it at y = 4: this one gives

    // A point constraint: the bob hangs from a point and stays within its arm's length of it.
    World3D pt(cfg);
    const Body3D b4 = pt.create_body({.shape = Shape3D::box({0.2f, 0.2f, 0.2f}), .position = {1, 5, 0}, .mass = 1.0f});
    Constraint3DDesc p;
    p.kind = Constraint3DKind::Point;
    p.b = b4;
    p.point = {0, 5, 0};
    pt.create_constraint(p);
    f32 far = 0.0f;
    for (int i = 0; i < 180; ++i) {
        (void)pt.step(dt);
        // The anchor on the body is (-1, 0, 0) in its own frame: it stays at the world point.
        const Vec3f anchor = pt.position(b4) + pt.rotation(b4).rotate({-1, 0, 0});
        far = std::max(far, length(anchor - Vec3f{0, 5, 0}));
    }
    CHECK(far < 0.03f);

    // A slider: along x, limits -0.5 to +1.0, pushed both ways with gravity off.
    World3DConfig zero = cfg;
    zero.gravity = {};
    World3D sl(zero);
    const Body3D rail = sl.create_body({.shape = Shape3D::box({2, 0.05f, 0.05f}), .motion = Body3DMotion::Static, .position = {0, 1, 0}});
    const Body3D cart = sl.create_body({.shape = Shape3D::box({0.2f, 0.2f, 0.2f}), .position = {0, 1, 0}, .mass = 5.0f});
    Constraint3DDesc s;
    s.kind = Constraint3DKind::Slider;
    s.a = rail;
    s.b = cart;
    s.point = {0, 1, 0};
    s.axis = {1, 0, 0};
    s.limits = true;
    s.min_position = -0.5f;
    s.max_position = 1.0f;
    const Constraint3D slide = sl.create_constraint(s);
    for (int i = 0; i < 120; ++i) {
        sl.add_force(cart, {200, 30, 0}); // the sideways part is taken by the rail
        (void)sl.step(dt);
    }
    CHECK(near(sl.constraint_info(slide).value, 1.0f, 0.03f));
    CHECK(near(sl.position(cart).y, 1.0f, 0.02f));
    for (int i = 0; i < 240; ++i) {
        sl.add_force(cart, {-200, 0, 0});
        (void)sl.step(dt);
    }
    CHECK(near(sl.constraint_info(slide).value, -0.5f, 0.03f));

    // A cone: a capsule hanging along -y from a point may swing 25 degrees and no more, pushed hard sideways.
    World3DConfig no_g = cfg;
    no_g.gravity = {};
    World3D cone(no_g);
    const Body3D arm = cone.create_body({.shape = Shape3D::capsule(0.4f, 0.1f), .position = {0, 4, 0}, .mass = 2.0f});
    Constraint3DDesc c;
    c.kind = Constraint3DKind::Cone;
    c.b = arm;
    c.point = {0, 4.5f, 0}; // the capsule's top end
    c.axis = {0, -1, 0};
    c.swing_angle = 25.0f;
    c.limits = true;
    c.min_angle = -10.0f;
    c.max_angle = 10.0f;
    cone.create_constraint(c);
    f32 widest = 0.0f;
    for (int i = 0; i < 180; ++i) {
        cone.add_force_at(arm, {60, 0, 0}, cone.position(arm) + Vec3f{0, -0.4f, 0});
        (void)cone.step(dt);
        const Vec3f along = cone.rotation(arm).rotate({0, -1, 0});
        widest = std::max(widest, std::acos(std::clamp(along.y * -1.0f * -1.0f, -1.0f, 1.0f)) * 180.0f / 3.14159f);
    }
    // `along` is the capsule's -y; hanging it is (0, -1, 0): the angle from straight down.
    const Vec3f final_axis = cone.rotation(arm).rotate({0, -1, 0});
    const f32 swing = std::acos(std::clamp(-final_axis.y, -1.0f, 1.0f)) * 180.0f / 3.14159f;
    CHECK(near(swing, 25.0f, 3.0f));
    (void)widest;
}

void test_fixed_and_breakable() {
    World3DConfig cfg = serial();
    cfg.gravity = {};
    World3D world(cfg);
    const Body3D a = world.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .position = {0, 1, 0},
                                        .linear_velocity = {2, 0, 0}, .angular_velocity = {0, 0, 0.5f}, .mass = 5.0f});
    const Body3D b = world.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .position = {1, 1, 0}, .mass = 5.0f});
    Constraint3DDesc d;
    d.kind = Constraint3DKind::Fixed;
    d.a = a;
    d.b = b;
    d.point = {0.5f, 1, 0};
    const Constraint3D weld = world.create_constraint(d);
    CHECK(weld.valid());
    const Vec3f offset0 = world.position(b) - world.position(a);
    run(world, 120);
    // b came along: the offset in a's frame is what it was, both moved on.
    const Vec3f rel = world.rotation(a).conjugate().rotate(world.position(b) - world.position(a));
    CHECK(near(rel, offset0, 0.03f));
    // The weld shares a's momentum with b: both go at about 1 m/s now.
    CHECK(world.position(a).x > 1.5f && world.position(b).x > 2.0f);
    CHECK(length(world.linear_velocity(a) - world.linear_velocity(b)) < 0.7f);
    CHECK(world.is_active(b));
    (void)weld;
    // Destroy the constraint: b is left to go its own way (and stays put, having no velocity of its own: it had a's).
    world.destroy_constraint(weld);
    CHECK(!world.contains(weld) && world.constraint_count() == 0);

    // Breakable: 100 kg hung from the world by a fixed constraint (981 N): breaks at 400 N? no: holds at 2000, breaks at 400.
    World3D hang(serial());
    const Body3D holds = hang.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .position = {0, 5, 0}, .mass = 100.0f,
                                           .user_id = 1});
    const Body3D snaps = hang.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .position = {5, 5, 0}, .mass = 100.0f,
                                           .user_id = 2});
    Constraint3DDesc h;
    h.kind = Constraint3DKind::Point;
    h.b = holds;
    h.point = {0, 5.3f, 0};
    h.break_force = 2000.0f;
    h.user_id = 11;
    const Constraint3D strong = hang.create_constraint(h);
    h.b = snaps;
    h.point = {5, 5.3f, 0};
    h.break_force = 400.0f;
    h.user_id = 22;
    const Constraint3D weak = hang.create_constraint(h);
    std::vector<ConstraintEvent3D> broke;
    for (int i = 0; i < 120; ++i) {
        (void)hang.step(dt);
        for (const ConstraintEvent3D& e : hang.drain_constraint_events()) {
            broke.push_back(e);
        }
    }
    CHECK(broke.size() == 1);
    if (broke.size() == 1) {
        CHECK(broke[0].constraint == weak && broke[0].b == snaps && broke[0].user_id == 22);
        CHECK(broke[0].force > 400.0f && broke[0].force < 2000.0f);
    }
    CHECK(hang.constraint_info(weak).broken && !hang.constraint_info(weak).enabled);
    CHECK(!hang.constraint_info(strong).broken);
    CHECK(hang.position(snaps).y < 0.0f);          // fell (nothing under it)
    CHECK(near(hang.position(holds).y, 5.0f, 0.1f)); // hangs
    // Mending: enabling a broken constraint puts it back (the body is far away, so it pulls hard).
    hang.set_constraint_enabled(weak, true);
    CHECK(!hang.constraint_info(weak).broken);
}

void test_constraints_save_restore_hash() {
    const auto make = [](World3D& world) {
        add_floor(world);
        std::vector<Body3D> chain;
        Body3D previous;
        for (int i = 0; i < 6; ++i) {
            const Body3D link = world.create_body({.shape = Shape3D::box({0.2f, 0.1f, 0.1f}),
                                                   .position = {0.4f * static_cast<f32>(i) + 0.2f, 4.0f, 0.0f}, .mass = 1.0f});
            Constraint3DDesc d;
            d.kind = Constraint3DKind::Hinge;
            d.a = previous;
            d.b = link;
            d.point = {0.4f * static_cast<f32>(i), 4.0f, 0.0f};
            d.axis = {0, 0, 1};
            d.break_force = i == 3 ? 12.0f : 0.0f; // the fourth link's joint cannot hold the weight of what hangs
            world.create_constraint(d);
            chain.push_back(link);
            previous = link;
        }
    };
    World3D world(serial());
    make(world);
    CHECK(world.constraint_count() == 6);
    const u64 with = world.state_hash();
    World3D bare(serial());
    add_floor(bare);
    CHECK(with != bare.state_hash());
    const std::vector<u8> saved = world.save_state();
    const u64 at_save = world.state_hash();
    std::vector<u64> first;
    for (int i = 0; i < 90; ++i) {
        (void)world.step(dt);
        first.push_back(world.state_hash());
    }
    // The weak joint broke somewhere in there.
    bool any_broken = false;
    for (const Constraint3D c : world.constraints()) {
        any_broken = any_broken || world.constraint_info(c).broken;
    }
    CHECK(any_broken);
    CHECK(world.restore_state(saved));
    CHECK(world.state_hash() == at_save);
    for (const Constraint3D c : world.constraints()) {
        CHECK(!world.constraint_info(c).broken); // mended by the restore
    }
    std::vector<u64> second;
    for (int i = 0; i < 90; ++i) {
        (void)world.step(dt);
        second.push_back(world.state_hash());
    }
    CHECK(first == second);
    // Another world with the same constraints takes it too; one with a constraint more does not.
    World3D other(serial());
    make(other);
    CHECK(other.restore_state(saved) && other.state_hash() == at_save);
    other.create_constraint({.kind = Constraint3DKind::Point, .b = other.bodies().back(), .point = {1, 1, 1}});
    CHECK(!other.restore_state(saved));

    // Workers do not change it.
    JobSystem four({.workers = 4, .max_background = 4});
    World3DConfig cfg;
    cfg.jobs = &four;
    World3D threaded(cfg);
    make(threaded);
    std::vector<u64> third;
    for (int i = 0; i < 90; ++i) {
        (void)threaded.step(dt);
        third.push_back(threaded.state_hash());
    }
    CHECK(third == first);
}


void test_events_sleep_and_wake() {
    World3D world(serial());
    const Body3D floor = add_floor(world);
    const Body3D box = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 0.55f, 0}});
    EventLog log;
    for (int i = 0; i < 200; ++i) {
        (void)world.step(dt);
        log.take(world);
    }
    CHECK(!world.is_active(box));
    CHECK(log.count(floor, box, ContactKind3D::Begin) == 1 && log.count(floor, box, ContactKind3D::End) == 0);
    // Woken in place: still touching, so neither an End nor a second Begin.
    world.activate(box);
    for (int i = 0; i < 60; ++i) {
        (void)world.step(dt);
        log.take(world);
    }
    CHECK(log.count(floor, box, ContactKind3D::Begin) == 1 && log.count(floor, box, ContactKind3D::End) == 0);
    // Thrown up: an End once it leaves, a Begin again when it lands, and the End does not repeat.
    world.add_impulse(box, {0, 5000.0f, 0}); // 1000 kg: 5 m/s up
    for (int i = 0; i < 240; ++i) {
        (void)world.step(dt);
        log.take(world);
    }
    CHECK(log.count(floor, box, ContactKind3D::End) == 1);
    CHECK(log.count(floor, box, ContactKind3D::Begin) == 2);
    // A sleeper woken by a body that landed on it stays in contact with the floor.
    World3D stack(serial());
    const Body3D ground = add_floor(stack);
    const Body3D low = stack.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 0.5f, 0}});
    run(stack, 120);
    CHECK(!stack.is_active(low));
    EventLog second;
    second.take(stack);
    const Body3D top = stack.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .position = {0, 3, 0}});
    for (int i = 0; i < 180; ++i) {
        (void)stack.step(dt);
        second.take(stack);
    }
    CHECK(second.count(ground, low, ContactKind3D::End) == 0);
    CHECK(second.count(ground, low, ContactKind3D::Begin) == 1); // from the first drain: low's own landing
    CHECK(second.count(low, top, ContactKind3D::Begin) == 1);
    (void)top;
}

void test_constraint_collide_connected() {
    // Two boxes that overlap, joined by a point constraint: they do not collide by default, so nothing pushes them
    // apart and no event is made between them; with collide_connected they do.
    for (const bool collide : {false, true}) {
        World3DConfig cfg = serial();
        cfg.gravity = {};
        World3D world(cfg);
        const Body3D a = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 0, 0}});
        const Body3D b = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0.3f, 0, 0}});
        Constraint3DDesc d;
        d.kind = Constraint3DKind::Point;
        d.a = a;
        d.b = b;
        d.point = {0.15f, 0, 0};
        d.collide_connected = collide;
        world.create_constraint(d);
        EventLog log;
        run(world, 30);
        log.take(world);
        const f32 gap = length(world.position(b) - world.position(a));
        if (collide) {
            CHECK(gap > 0.5f); // pushed apart as far as the joint allows
            CHECK(log.count(a, b, ContactKind3D::Begin) == 1);
        } else {
            CHECK(near(gap, 0.3f, 0.02f));
            CHECK(log.all.empty());
        }
    }
}

void test_body_helpers() {
    World3D world(serial());
    add_floor(world);
    // teleport: velocity gone, position and rotation set.
    const Body3D ball = world.create_body({.shape = Shape3D::sphere(0.3f), .position = {0, 5, 0},
                                           .linear_velocity = {3, 2, 1}, .angular_velocity = {1, 1, 1}});
    world.teleport(ball, {10, 7, -2}, Quatf::axis_angle({0, 1, 0}, 90.0f));
    CHECK(near(world.position(ball), {10, 7, -2}, 1e-5f));
    CHECK(near(world.linear_velocity(ball), {}, 1e-6f) && near(world.angular_velocity(ball), {}, 1e-6f));
    // set_position keeps velocity (as before).
    world.set_linear_velocity(ball, {1, 0, 0});
    world.set_position(ball, {0, 7, 0});
    CHECK(near(world.linear_velocity(ball), {1, 0, 0}, 1e-6f));

    // follow_transform: a kinematic body arrives at the target at the end of the step, a dynamic one nearly.
    World3DConfig cfg = serial();
    cfg.gravity = {};
    World3D free_world(cfg);
    const Body3D kin_body = free_world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .motion = Body3DMotion::Kinematic,
                                                    .position = {0, 0, 0}});
    const Body3D dyn_body = free_world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 3, 0}});
    const Quatf target_rot = Quatf::axis_angle({0, 1, 0}, 30.0f);
    free_world.follow_transform(kin_body, {1, 0, 0}, target_rot, dt);
    free_world.follow_transform(dyn_body, {1, 3, 0}, target_rot, dt);
    (void)free_world.step(dt);
    CHECK(near(free_world.position(kin_body), {1, 0, 0}, 1e-3f));
    CHECK(near(free_world.position(dyn_body), {1, 3, 0}, 2e-3f));
    const Quatf q = free_world.rotation(dyn_body);
    CHECK(std::fabs(q.x * target_rot.x + q.y * target_rot.y + q.z * target_rot.z + q.w * target_rot.w) > 0.9999f);

    // set_layer and set_sensor after creation.
    World3D layered(serial());
    add_floor(layered);
    const Body3D fall = layered.create_body({.shape = Shape3D::sphere(0.3f), .position = {0, 2, 0}});
    CHECK(layered.layer(fall) == layers3d::Dynamic);
    layered.set_layers_collide(layers3d::User0, layers3d::Static, false);
    CHECK(layered.set_layer(fall, layers3d::User0));
    CHECK(layered.layer(fall) == layers3d::User0);
    CHECK(!layered.set_layer(fall, layers3d::Static)); // only static bodies are in layer 0
    run(layered, 120);
    CHECK(layered.position(fall).y < -1.0f); // fell through the floor
    layered.teleport(fall, {0, 2, 0}, {});
    CHECK(layered.set_layer(fall, layers3d::Dynamic));
    run(layered, 120);
    CHECK(near(layered.position(fall).y, 0.3f, 0.03f));
    // As a sensor it falls through too; and back.
    layered.set_sensor(fall, true);
    CHECK(layered.is_sensor(fall));
    layered.teleport(fall, {0, 2, 0}, {});
    run(layered, 90);
    CHECK(layered.position(fall).y < -0.5f);
    layered.set_sensor(fall, false);
    CHECK(!layered.is_sensor(fall));
}

void test_set_shape() {
    World3D world(serial());
    add_floor(world);
    const Body3D box = world.create_body({.shape = Shape3D::box({0.5f, 0.25f, 0.5f}), .position = {0, 2, 0}, .mass = 3.0f});
    run(world, 120);
    CHECK(near(world.position(box).y, 0.25f, 0.03f));
    // A taller shape (a crouch undone): it rests higher, the mass is as it was.
    CHECK(world.set_shape(box, Shape3D::box({0.5f, 0.75f, 0.5f})));
    CHECK(world.is_active(box));
    run(world, 120);
    CHECK(near(world.position(box).y, 0.75f, 0.04f));
    CHECK(near(world.mass(box), 3.0f, 1e-5f));
    // A mesh cannot go to a dynamic body; an invalid shape is refused.
    const std::array<Vec3f, 3> tri = {Vec3f{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    const std::array<u32, 3> idx = {0, 1, 2};
    CHECK(!world.set_shape(box, Shape3D::triangle_mesh(tri, idx)));
    CHECK(!world.set_shape(box, Shape3D{}));
    // The body's reported shape is the new one.
    CHECK(world.shape(box).bounds_max().y > 0.7f);
}

// Past a capacity Jolt drops contacts for the step and says so only in what PhysicsSystem::Update returns: the step reports
// it, last_step() keeps it, and the world counts the step.
void test_step_report_capacity() {
    // Room for 4 contact constraints (the fewest that keep Jolt's manifold table at its 4 buckets) and 8 balls resting apart
    // on the floor: each ball and the floor are one manifold, one contact constraint.
    World3DConfig cfg = serial();
    cfg.max_contact_constraints = 4;
    World3D world(cfg);
    add_floor(world);
    const auto balls = [](World3D& w) {
        for (int i = 0; i < 8; ++i) {
            w.create_body({.shape = Shape3D::sphere(0.5f), .position = {2.0f * static_cast<f32>(i) - 7.0f, 0.5f, 0.0f}});
        }
    };
    balls(world);
    CHECK(!world.last_step().any() && world.overflowed_steps() == 0);
    const StepReport3D report = world.step(dt);
    CHECK(report.contact_constraints_full);
    CHECK(!report.body_pair_cache_full);
    CHECK(world.last_step().contact_constraints_full);
    CHECK(world.overflowed_steps() == 1);
    // The default capacities hold the same scene.
    World3D roomy(serial());
    add_floor(roomy);
    balls(roomy);
    CHECK(!roomy.step(dt).any());
    CHECK(!roomy.last_step().any() && roomy.overflowed_steps() == 0);
}

// A kinematic body made dynamic falls with the mass it was made with (it was refused: only a dynamic body kept a mass).
void test_kinematic_becomes_dynamic() {
    World3D world(serial());
    const Body3D box = world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .motion = Body3DMotion::Kinematic,
                                          .position = {0.0f, 10.0f, 0.0f}, .density = 500.0f});
    CHECK(near(world.mass(box), 500.0f, 0.5f));
    run(world, 5);
    CHECK(near(world.position(box).y, 10.0f, 1e-6f)); // gravity does nothing to a kinematic body
    CHECK(world.set_motion(box, Body3DMotion::Dynamic));
    CHECK(world.motion(box) == Body3DMotion::Dynamic);
    run(world, 30);
    // Half a second of free fall: about 1.27 m down, at 4.9 m/s.
    CHECK(world.position(box).y < 9.0f);
    CHECK(world.linear_velocity(box).y < -4.0f);
    CHECK(near(world.mass(box), 500.0f, 0.5f));
    CHECK(world.set_motion(box, Body3DMotion::Kinematic));
    // A kinematic mesh has no mass to be dynamic with; a static body cannot be switched at all.
    const std::array<Vec3f, 3> tri = {Vec3f{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    const std::array<u32, 3> idx = {0, 1, 2};
    const Body3D mesh = world.create_body({.shape = Shape3D::triangle_mesh(tri, idx), .motion = Body3DMotion::Kinematic,
                                           .position = {5.0f, 0.0f, 0.0f}});
    CHECK(mesh.valid() && world.mass(mesh) == 0.0f);
    CHECK(!world.set_motion(mesh, Body3DMotion::Dynamic));
    CHECK(world.motion(mesh) == Body3DMotion::Kinematic);
    const Body3D wall = world.create_body({.shape = Shape3D::box({1, 1, 1}), .motion = Body3DMotion::Static, .position = {-5, 0, 0}});
    CHECK(!world.set_motion(wall, Body3DMotion::Dynamic));
}

// Live changes of what a body was made with: mass, gravity factor, dampings, friction, restitution.
void test_live_body_settings() {
    const Shape3D box = Shape3D::box({0.5f, 0.5f, 0.5f});
    // A raised gravity factor falls faster: twice the speed after the same time (the speed is exact, g dt a step).
    {
        World3D world(serial());
        const Body3D plain = world.create_body({.shape = box, .position = {0, 50, 0}});
        const Body3D heavy = world.create_body({.shape = box, .position = {5, 50, 0}});
        CHECK(near(world.gravity_factor(heavy), 1.0f, 0.0f));
        CHECK(world.set_gravity_factor(heavy, 2.0f));
        CHECK(near(world.gravity_factor(heavy), 2.0f, 0.0f));
        run(world, 30);
        CHECK(near(world.linear_velocity(plain).y, -9.81f * 30.0f * dt, 1e-3f));
        CHECK(near(world.linear_velocity(heavy).y, 2.0f * world.linear_velocity(plain).y, 1e-3f));
        CHECK(world.position(heavy).y < world.position(plain).y - 1.0f);
        // 0 floats: a body asleep on the floor whose gravity is taken away is woken and stays where it is, or rises (< 0).
        World3D rest(serial());
        add_floor(rest);
        const Body3D b = rest.create_body({.shape = box, .position = {0, 0.5f, 0}});
        run(rest, 180);
        CHECK(!rest.is_active(b));
        CHECK(rest.set_gravity_factor(b, -0.5f));
        CHECK(rest.is_active(b));
        run(rest, 60);
        CHECK(rest.position(b).y > 1.5f);
    }
    // Damping slows: the same speed, damped from the first step on, is (1 - c dt)^60 of itself after a second; spin likewise.
    {
        World3DConfig cfg = serial();
        cfg.gravity = {};
        World3D world(cfg);
        const Body3D d = world.create_body({.shape = box, .linear_velocity = {10, 0, 0}, .angular_velocity = {0, 5, 0}});
        const Body3D u = world.create_body({.shape = box, .position = {0, 5, 0}, .linear_velocity = {10, 0, 0},
                                            .angular_velocity = {0, 5, 0}});
        CHECK(world.set_linear_damping(d, 1.0f) && world.set_angular_damping(d, 2.0f));
        CHECK(near(world.linear_damping(d), 1.0f, 0.0f) && near(world.angular_damping(d), 2.0f, 0.0f));
        run(world, 60);
        CHECK(world.linear_velocity(d).x < 4.0f && world.linear_velocity(d).x > 3.0f);
        CHECK(world.angular_velocity(d).y < 1.0f);
        CHECK(near(world.linear_velocity(u).x, 10.0f, 1e-3f) && near(world.angular_velocity(u).y, 5.0f, 1e-3f));
        // Back to none: the speed it has is kept from then on.
        CHECK(world.set_linear_damping(d, 0.0f));
        const f32 kept = world.linear_velocity(d).x;
        run(world, 30);
        CHECK(near(world.linear_velocity(d).x, kept, 1e-4f));
    }
    // Mass: an equal impulse moves a body of twice the mass at half the speed, and an equal angular impulse turns it at half
    // the rate (the inertia scales with it). A mass added for a step stays on top and goes with the step.
    {
        World3DConfig cfg = serial();
        cfg.gravity = {};
        World3D world(cfg);
        const Body3D a = world.create_body({.shape = box, .mass = 10.0f});
        const Body3D b = world.create_body({.shape = box, .position = {5, 0, 0}, .mass = 10.0f});
        CHECK(world.set_mass(b, 20.0f));
        CHECK(near(world.mass(b), 20.0f, 0.0f));
        world.add_impulse(a, {10, 0, 0});
        world.add_impulse(b, {10, 0, 0});
        world.add_angular_impulse(a, {0, 1, 0});
        world.add_angular_impulse(b, {0, 1, 0});
        run(world, 1);
        CHECK(near(world.linear_velocity(a).x, 1.0f, 1e-4f));
        CHECK(near(world.linear_velocity(b).x, 0.5f, 1e-4f));
        CHECK(near(world.angular_velocity(b).y, 0.5f * world.angular_velocity(a).y, 1e-4f));
        world.set_added_mass(b, 20.0f);
        CHECK(world.set_mass(b, 30.0f));
        world.add_impulse(b, {50, 0, 0}); // over 30 + 20 this step
        run(world, 1);
        CHECK(near(world.linear_velocity(b).x, 0.5f + 1.0f, 1e-4f));
        world.add_impulse(b, {30, 0, 0}); // over 30 alone: the added mass went with the step
        run(world, 1);
        CHECK(near(world.linear_velocity(b).x, 2.5f, 1e-4f));
        // A kinematic body keeps the mass for when it becomes dynamic; a kinematic mesh has none to set.
        const Body3D k = world.create_body({.shape = box, .motion = Body3DMotion::Kinematic, .position = {0, 9, 0}, .mass = 1.0f});
        CHECK(world.set_mass(k, 4.0f) && world.set_motion(k, Body3DMotion::Dynamic));
        world.add_impulse(k, {0, 0, 8});
        run(world, 1);
        CHECK(near(world.linear_velocity(k).z, 2.0f, 1e-4f));
        const std::array<Vec3f, 3> tri = {Vec3f{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
        const std::array<u32, 3> idx = {0, 1, 2};
        const Body3D mesh = world.create_body({.shape = Shape3D::triangle_mesh(tri, idx), .motion = Body3DMotion::Kinematic,
                                               .position = {9, 9, 9}});
        CHECK(!world.set_mass(mesh, 5.0f) && world.mass(mesh) == 0.0f);
    }
    // Restitution changes a bounce: set on the ball after it was made, or on the (static) floor (a contact takes the larger).
    {
        const auto bounce = [](bool on_floor) {
            World3D world(serial());
            const Body3D floor = add_floor(world, 0.5f, 0.0f);
            const f32 r = 0.25f;
            const Body3D ball = world.create_body({.shape = Shape3D::sphere(r), .position = {0.0f, 2.0f + r, 0.0f}});
            CHECK(world.set_restitution(on_floor ? floor : ball, 0.8f));
            CHECK(near(world.restitution(on_floor ? floor : ball), 0.8f, 0.0f));
            f32 peak = 0.0f;
            bool bounced = false;
            for (int i = 0; i < 240; ++i) {
                const f32 before = world.linear_velocity(ball).y;
                (void)world.step(dt);
                const f32 after = world.linear_velocity(ball).y;
                bounced = bounced || (before < -1.0f && after > 0.0f);
                if (bounced) {
                    peak = std::max(peak, world.position(ball).y - r);
                    if (after < 0.0f) {
                        break;
                    }
                }
            }
            return peak;
        };
        CHECK(near(bounce(false), 0.64f * 2.0f, 0.15f));
        CHECK(near(bounce(true), 0.64f * 2.0f, 0.15f));
    }
    // Friction of a static slope set to 0 under a crate asleep on it: the crate is woken and slides (sqrt(1 x 0) = 0).
    {
        World3D world(serial());
        const Quatf tilt = Quatf::axis_angle({0, 0, 1}, 20.0f);
        const Body3D slope = world.create_body({.shape = Shape3D::box({10.0f, 0.5f, 3.0f}), .motion = Body3DMotion::Static,
                                                .rotation = tilt, .friction = 1.0f});
        const Vec3f n = tilt.rotate({0, 1, 0});
        const Body3D crate = world.create_body({.shape = box, .position = n * 1.01f, .rotation = tilt, .friction = 1.0f});
        run(world, 240);
        const Vec3f at = world.position(crate);
        CHECK(!world.is_active(crate));
        CHECK(world.set_friction(slope, 0.0f));
        CHECK(near(world.friction(slope), 0.0f, 0.0f) && near(world.friction(crate), 1.0f, 0.0f));
        CHECK(world.is_active(crate));
        run(world, 60);
        CHECK(length(world.position(crate) - at) > 1.0f);
    }
    // Refused: a static body for what needs motion (friction and restitution do apply to it), a character's own body, a
    // dead handle, numbers that are not finite or out of range. Nothing changes.
    {
        World3D world(serial());
        const Body3D floor = add_floor(world);
        CHECK(!world.set_mass(floor, 10.0f) && !world.set_gravity_factor(floor, 2.0f));
        CHECK(!world.set_linear_damping(floor, 1.0f) && !world.set_angular_damping(floor, 1.0f));
        CHECK(world.gravity_factor(floor) == 0.0f && world.linear_damping(floor) == 0.0f && world.mass(floor) == 0.0f);
        CHECK(world.set_friction(floor, 0.9f) && world.set_restitution(floor, 0.1f));
        const Body3D ball = world.create_body({.shape = Shape3D::sphere(0.3f), .position = {0, 3, 0}, .mass = 2.0f,
                                               .friction = 0.4f, .restitution = 0.2f, .linear_damping = 0.1f});
        for (const f32 bad : {std::nanf(""), INFINITY, -1.0f}) {
            CHECK(!world.set_mass(ball, bad) && !world.set_linear_damping(ball, bad) && !world.set_angular_damping(ball, bad));
            CHECK(!world.set_friction(ball, bad) && !world.set_restitution(ball, bad));
        }
        CHECK(!world.set_mass(ball, 0.0f));
        CHECK(!world.set_gravity_factor(ball, std::nanf("")) && world.set_gravity_factor(ball, -1.0f));
        CHECK(world.set_gravity_factor(ball, 1.0f));
        CHECK(world.mass(ball) == 2.0f && world.friction(ball) == 0.4f && world.restitution(ball) == 0.2f);
        CHECK(near(world.linear_damping(ball), 0.1f, 0.0f));
        const Body3D capsule = world.character(world.create_character({})).body;
        CHECK(world.owner_of(capsule) == BodyOwner3D::Character);
        CHECK(!world.set_mass(capsule, 5.0f) && !world.set_gravity_factor(capsule, 0.0f) && !world.set_friction(capsule, 0.0f));
        CHECK(!world.set_restitution(capsule, 1.0f) && !world.set_linear_damping(capsule, 1.0f));
        CHECK(!world.set_angular_damping(capsule, 1.0f));
        const Body3D gone = world.create_body({.shape = box, .position = {9, 9, 9}});
        CHECK(world.destroy_body(gone));
        CHECK(!world.set_mass(gone, 1.0f) && !world.set_friction(gone, 1.0f) && world.friction(gone) == 0.0f);
        // Not in the hash (the body's setup, as its shape is): an awake body's damping changes nothing the hash reads until a
        // step has used it.
        const Body3D moving = world.create_body({.shape = box, .position = {20, 3, 0}, .linear_velocity = {1, 0, 0}});
        (void)world.step(dt);
        const u64 before = world.state_hash();
        CHECK(world.set_linear_damping(moving, 5.0f) && world.state_hash() == before);
    }
}

// Bodies waking and falling asleep (World3DConfig::activation_events).
void test_activation_events() {
    const Shape3D box = Shape3D::box({0.5f, 0.5f, 0.5f});
    World3DConfig cfg = serial();
    cfg.activation_events = true;
    World3D world(cfg);
    add_floor(world);
    // Made awake: it woke. Then it settles and falls asleep, once; a static body is never reported.
    const Body3D b = world.create_body({.shape = box, .position = {0, 0.55f, 0}, .user_id = 5});
    std::vector<ActivationEvent3D> seen = world.drain_activation_events();
    CHECK(seen.size() == 1 && seen[0].body == b && seen[0].active && seen[0].user_id == 5);
    run(world, 240);
    CHECK(!world.is_active(b));
    seen = world.drain_activation_events();
    CHECK(seen.size() == 1 && seen[0].body == b && !seen[0].active);
    // A push wakes it at the call (in call order with the other calls); deactivate() puts it to sleep likewise.
    const Body3D asleep = world.create_body({.shape = box, .position = {3, 0.5f, 0}, .start_active = false});
    CHECK(world.activation_events().empty());
    world.add_impulse(asleep, {0, 100, 0});
    world.activate(b);
    world.deactivate(b);
    CHECK(world.activation_events().size() == 3);
    seen = world.drain_activation_events();
    CHECK(seen[0].body == asleep && seen[0].active && seen[1].body == b && seen[1].active && seen[2].body == b && !seen[2].active);
    // Woken by the simulation (something lands on it): reported by the step.
    run(world, 240);
    world.drain_activation_events();
    const Body3D drop = world.create_body({.shape = Shape3D::box({0.3f, 0.3f, 0.3f}), .position = {0, 3, 0}});
    world.drain_activation_events();
    bool woke = false;
    for (int i = 0; i < 120 && !woke; ++i) {
        (void)world.step(dt);
        for (const ActivationEvent3D& e : world.drain_activation_events()) {
            woke = woke || (e.body == b && e.active);
        }
    }
    CHECK(woke);
    (void)drop;
    // Off (the default): none, ever.
    World3D quiet(serial());
    add_floor(quiet);
    quiet.create_body({.shape = box, .position = {0, 0.55f, 0}});
    run(quiet, 240);
    CHECK(quiet.activation_events().empty() && quiet.drain_activation_events().empty());
    // The same with no workers and with four: the same events in the same order (a pile settling).
    const auto settle = [](World3DConfig c) {
        c.activation_events = true;
        World3D w(c);
        build_pile(w, 120);
        std::vector<std::pair<u32, bool>> out;
        for (int i = 0; i < 400; ++i) {
            (void)w.step(dt);
            for (const ActivationEvent3D& e : w.drain_activation_events()) {
                out.push_back({e.body.index, e.active});
            }
        }
        return out;
    };
    const auto alone = settle(serial());
    CHECK(std::count_if(alone.begin(), alone.end(), [](const auto& e) { return !e.second; }) > 20);
    {
        JobSystem four({.workers = 4, .max_background = 4});
        World3DConfig c;
        c.jobs = &four;
        CHECK(settle(c) == alone);
    }
    // A restore drops what waits, as it does the other events.
    const std::vector<u8> saved = world.save_state();
    world.activate(b);
    CHECK(!world.activation_events().empty() || world.is_active(b));
    CHECK(world.restore_state(saved));
    CHECK(world.activation_events().empty());
}

// A stream cut short is refused with the world as it was. Jolt's own restore puts the bodies back as it reads and stops where
// the bytes end, and the rope's length and the character's stance were put back before it: the check comes first.
// A static body moved, reshaped or made a sensor wakes what rests on it (Jolt does not): a crate asleep on a slab that goes
// away falls. The same for a kinematic slab reshaped.
void test_changed_static_wakes() {
    // The slab spans x in [-0.25, 3.75]; the crate (x in [-0.5, 0.5]) rests on its edge, asleep.
    const auto asleep_on_slab = [](World3D& world, Body3DMotion motion, Body3D& slab, Body3D& crate) {
        add_floor(world);
        slab = world.create_body({.shape = Shape3D::box({2.0f, 0.25f, 2.0f}), .motion = motion, .position = {1.75f, 2.0f, 0.0f}});
        crate = add_box(world, {0.0f, 2.75f, 0.0f});
        run(world, 180);
        CHECK(!world.is_active(crate) && near(world.position(crate).y, 2.75f, 0.02f));
    };
    const auto fallen = [](World3D& world, Body3D crate) {
        run(world, 60);
        return world.position(crate).y < 2.5f;
    };
    {
        auto config=serial();config.gravity={};World3D world(config);
        const auto support=world.create_body({.shape=Shape3D::box({2,.25f,2}),.position={1.75f,0,0}});
        const auto crate=world.create_body({.shape=Shape3D::box({.5f,.5f,.5f}),.position={0,.75f,0}});
        (void)world.step(dt);world.deactivate(support);world.deactivate(crate);
        CHECK(!world.is_active(crate));
        CHECK(world.set_shape(support,Shape3D::box({.5f,.25f,2})));
        CHECK(world.is_active(crate)); // shrinking a sleeping dynamic support wakes its load
    }
    {
        auto config=serial();config.gravity={};World3D world(config);
        const auto unit=Shape3D::box({1,1,1});
        const std::array<ShapeChild3D,2> walls{{{unit,{-10,0,0},{}},{unit,{10,0,0},{}}}};
        const auto level=world.create_body({.shape=Shape3D::compound(walls),.motion=Body3DMotion::Static});
        const auto free=world.create_body({.shape=Shape3D::sphere(.25f)});
        world.deactivate(free);world.set_sensor(level,true);
        CHECK(!world.is_active(free)); // inside the level AABB, touching no geometry
    }
    const Quatf upright = Quatf::axis_angle({0, 0, 1}, 90.0f); // the slab on its side: x in [1.5, 2], under nothing
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        world.set_position(slab, {10.0f, 2.0f, 0.0f});
        CHECK(fallen(world, crate));
    }
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        world.set_rotation(slab, upright);
        CHECK(fallen(world, crate));
    }
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        world.teleport(slab, {10.0f, 2.0f, 0.0f}, {});
        CHECK(fallen(world, crate));
    }
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        world.follow_transform(slab, {10.0f, 2.0f, 0.0f}, {}, dt);
        CHECK(fallen(world, crate));
    }
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        CHECK(world.set_shape(slab, Shape3D::box({0.5f, 0.25f, 2.0f}))); // x in [1.25, 2.25]
        CHECK(fallen(world, crate));
    }
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        world.set_sensor(slab, true);
        CHECK(fallen(world, crate));
    }
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Kinematic, slab, crate);
        CHECK(world.set_shape(slab, Shape3D::box({0.5f, 0.25f, 2.0f})));
        CHECK(fallen(world, crate));
    }
    for (int change = 0; change < 4; ++change) {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        if (change == 0) {
            world.set_layers_collide(layers3d::Kinematic, layers3d::Dynamic, false);
            CHECK(world.set_layer(slab, layers3d::Kinematic));
        } else if (change == 1)
            world.set_no_collisions(slab, true);
        else if (change == 2)
            world.set_layers_collide(layers3d::Static, layers3d::Dynamic, false);
        else {
            world.set_group(crate, 42);
            run(world, 180);
            CHECK(!world.is_active(crate));
            world.set_group(slab, 42);
        }
        CHECK(world.is_active(crate));
        CHECK(fallen(world, crate));
    }
    // A static body nothing rests on is moved for the cost of a broad phase query: the hash is as without the move elsewhere.
    {
        World3D world(serial());
        Body3D slab, crate;
        asleep_on_slab(world, Body3DMotion::Static, slab, crate);
        const u64 before = world.state_hash();
        const Body3D far = world.create_body({.shape = Shape3D::box({1, 1, 1}), .motion = Body3DMotion::Static, .position = {30, 1, 0}});
        world.set_position(far, {40, 1, 0});
        (void)world.destroy_body(far);
        run(world, 1);
        CHECK(!world.is_active(crate));
        CHECK(world.state_hash() == before);
    }
}

// A restore puts each body's motion type and added mass back (both change the step; the motion type is in the hash).
void test_restore_motion_and_added_mass() {
    World3D world(serial());
    add_floor(world);
    const Body3D box = add_box(world, {0.0f, 5.0f, 0.0f});
    const Body3D other = add_box(world, {3.0f, 5.0f, 0.0f});
    run(world, 10);
    const std::vector<u8> saved = world.save_state();
    // A sideways push on `other` (in the air for all of this) every step: how far it gets depends on its mass.
    const auto hashes = [&](int frames) {
        std::vector<u64> out;
        for (int i = 0; i < frames; ++i) {
            world.add_force(other, {400.0f, 0.0f, 0.0f});
            (void)world.step(dt);
            out.push_back(world.state_hash());
        }
        return out;
    };
    const std::vector<u64> plain = hashes(30);
    // The motion type changed after the save: the restore changes it back.
    CHECK(world.restore_state(saved));
    CHECK(world.set_motion(box, Body3DMotion::Kinematic));
    CHECK(world.motion(box) == Body3DMotion::Kinematic);
    const u64 at_save = [&] {
        World3D same(serial());
        add_floor(same);
        add_box(same, {0.0f, 5.0f, 0.0f});
        add_box(same, {3.0f, 5.0f, 0.0f});
        run(same, 10);
        return same.state_hash();
    }();
    CHECK(world.state_hash() != at_save);
    CHECK(world.restore_state(saved));
    CHECK(world.motion(box) == Body3DMotion::Dynamic);
    CHECK(world.state_hash() == at_save);
    CHECK(hashes(30) == plain);
    // An added mass given before the save (the step takes it off): the restore puts it back for the step after it.
    CHECK(world.restore_state(saved));
    world.set_added_mass(other, 80.0f);
    const std::vector<u8> heavy = world.save_state();
    const std::vector<u64> weighted = hashes(30);
    CHECK(weighted != plain);
    CHECK(world.added_mass(other) == 0.0f);
    CHECK(world.restore_state(heavy));
    CHECK(world.added_mass(other) == 80.0f);
    CHECK(hashes(30) == weighted);
    // And one given after the save is taken off by the restore.
    world.set_added_mass(other, 80.0f);
    CHECK(world.restore_state(saved));
    CHECK(world.added_mass(other) == 0.0f);
    CHECK(hashes(30) == plain);
}

void test_debug_large_hull_face() {
    constexpr int sides = 80;
    std::vector<Vec3f> vertices;
    for (const float y : {-1.0f, 1.0f})
        for (int i = 0; i < sides; ++i) {
            const float angle = 6.28318530718f * static_cast<float>(i) / sides;
            vertices.push_back({10.0f * std::cos(angle), y, 10.0f * std::sin(angle)});
        }
    const auto hull = Shape3D::convex_hull(vertices);
    CHECK(hull.valid());
    DebugDraw3D debug;
    debug.shape(hull, {}, {}, colors::white);
    u32 top = 0, bottom = 0;
    for (const auto& edge : debug.lines()) {
        CHECK(std::isfinite(edge.a.x) && std::isfinite(edge.a.y) && std::isfinite(edge.a.z));
        CHECK(std::isfinite(edge.b.x) && std::isfinite(edge.b.y) && std::isfinite(edge.b.z));
        top += near(edge.a.y, 1.0f, .001f) && near(edge.b.y, 1.0f, .001f);
        bottom += near(edge.a.y, -1.0f, .001f) && near(edge.b.y, -1.0f, .001f);
    }
    CHECK(!debug.truncated() && top >= sides && bottom >= sides);
}

void test_restore_cut_short_changes_nothing() {
    World3D world(serial());
    build_pile(world, 60);
    RopeDesc3D rope;
    rope.start = RopeAnchor3D::at({14.0f, 6.0f, 0.0f});
    rope.end = RopeAnchor3D::free();
    rope.length = 3.0f;
    const RopeHandle3D r = world.create_rope(rope);
    CharacterDesc3D cd;
    cd.position = {14.0f, 0.0f, 6.0f};
    const Character3D c = world.create_character(cd);
    CHECK(r.valid() && c.valid());
    run(world, 30);
    const std::vector<u8> saved = world.save_state();
    // The world goes on: the pile falls further, the rope is longer, the character crouches (and keeps wanting to).
    world.set_rope_length(r, 4.0f);
    CharacterInput3D crouch;
    crouch.crouch = true;
    world.set_character_input(c, crouch);
    CHECK(world.set_character_crouch(c, true));
    run(world, 30);
    const u64 before = world.state_hash();
    CHECK(near(world.rope_state(r).rest_length, 4.0f, 1e-5f) && world.character(c).crouching);
    const std::vector<u8> cut(saved.begin(), saved.end() - 64);
    CHECK(!world.restore_state(cut));
    CHECK(world.state_hash() == before);
    CHECK(near(world.rope_state(r).rest_length, 4.0f, 1e-5f));
    CHECK(world.character(c).crouching);
    // The whole stream restores: it was the cut that was refused.
    CHECK(world.restore_state(saved));
    CHECK(near(world.rope_state(r).rest_length, 3.0f, 1e-5f));
    CHECK(!world.character(c).crouching);
}

} // namespace

int main(int argc, char** argv) {
    // Separate CTest processes: no World3D or primitive shape may initialise Jolt before these containers allocate.
    if (argc == 2) {
        const std::array<Vec3f, 4> points{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        Shape3D shape;
        if (std::string_view(argv[1]) == "first-hull") {
            shape = Shape3D::convex_hull(points);
            CHECK(shape.valid() && shape.kind() == Shape3DKind::ConvexHull);
        } else if (std::string_view(argv[1]) == "first-mesh") {
            const std::array<u32, 3> indices{0, 1, 2};
            shape = Shape3D::triangle_mesh(points, indices);
            CHECK(shape.valid() && shape.kind() == Shape3DKind::TriangleMesh);
        } else return 2;
        return failures ? 1 : 0;
    }
    test_rest_and_sleep();
    test_free_fall_dt();
    test_gravity_parameter();
    test_sphere_rolls_down_mesh();
    test_restitution();
    test_shapes_at_rest();
    test_compound();
    test_kinematic_pushes();
    test_handles_and_user_ids();
    test_mass_forces_damping();
    test_layers();
    test_destroy_stacked();
    test_pile();
    test_hash_sensitivity();
    test_determinism();
    test_save_restore();
    test_raycast_each_shape();
    test_query_filters_and_order();
    test_shape_cast();
    test_events_begin_end_once();
    test_events_bounce();
    test_events_sensor();
    test_events_deterministic();
    test_events_deterministic_sub_shapes();
    test_events_destroy_order();
    test_events_persist_and_drain();
    test_hinge_limits_and_motor();
    test_distance_point_slider_cone();
    test_fixed_and_breakable();
    test_constraints_save_restore_hash();
    test_events_sleep_and_wake();
    test_constraint_collide_connected();
    test_body_helpers();
    test_set_shape();
    test_step_report_capacity();
    test_kinematic_becomes_dynamic();
    test_live_body_settings();
    test_activation_events();
    test_changed_static_wakes();
    test_restore_motion_and_added_mass();
    test_restore_cut_short_changes_nothing();
    test_debug_large_hull_face();
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_tests: all passed\n");
    return 0;
}
