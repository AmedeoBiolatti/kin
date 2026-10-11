// kin/physics3d (Jolt Physics behind kin's own types): bodies at rest and in motion
// against hand-computed values, the shapes, layers, handles, the state hash and its
// determinism (the same bits twice, and with 1, 4 and no workers), saved states.
// No GPU. docs/physics_3d.md.

#include <kin/core/jobs.hpp>
#include <kin/physics3d/world3d.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
        world.step(dt);
    }
}

World3DConfig serial() {
    World3DConfig c;
    c.single_threaded = true;
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
        world.step(dt);
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
    big.step(0.5f);
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
            world.step(dt);
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
        world.step(dt);
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

    world.destroy_body(b);
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
    world.destroy_body(b); // stale: must not destroy d
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
            w.step(dt);
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
    world.destroy_body(tower[0]);
    CHECK(world.body_count() == 5); // floor and four boxes
    run(world, 120);
    // The rest came down onto the floor: the top box is now 3.5 up.
    CHECK(near(world.position(tower[1]).y, 0.5f, 0.1f));
    CHECK(near(world.position(tower[4]).y, 3.5f, 0.2f));
    for (const Body3D b : tower) {
        world.destroy_body(b);
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
        a.destroy_body(gone);
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
        world.step(dt);
        first.push_back(world.state_hash());
    }
    CHECK(world.state_hash() != at_save);
    CHECK(world.restore_state(saved));
    CHECK(world.state_hash() == at_save);
    std::vector<u64> second;
    for (int i = 0; i < 90; ++i) {
        world.step(dt);
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

} // namespace

int main() {
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
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_tests: all passed\n");
    return 0;
}
