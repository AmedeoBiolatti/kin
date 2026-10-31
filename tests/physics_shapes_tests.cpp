// PhysicsWorld's shapes past boxes and circles (capsules, polygons, chains),
// fixtures of several Box2D shapes (contacts and queries count them once), and
// joints.

#include <kin/physics/physics.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ranges>
#include <vector>

using kin::f32;
using kin::PhysicsBody;
using kin::PhysicsBodyType;
using kin::PhysicsFixture;
using kin::PhysicsJointType;
using kin::PhysicsShape;
using kin::PhysicsWorld;
using kin::Vec2f;

namespace {

constexpr f32 dt = 1.0f / 60.0f;

bool near(f32 a, f32 b, f32 epsilon = 0.05f) {
    return std::fabs(a - b) <= epsilon;
}

f32 distance(Vec2f a, Vec2f b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

void run(PhysicsWorld& world, int steps) {
    for (int i = 0; i < steps; ++i) {
        world.step(dt);
    }
}

// A static floor, its top at y = 0 (y is down: gravity is +y).
PhysicsBody floor_of(PhysicsWorld& world) {
    const PhysicsBody floor = world.create_body({.type = PhysicsBodyType::Static, .position = {0.0f, 1.0f}});
    world.add_fixture(floor, {.shape = PhysicsShape::box({20.0f, 1.0f})});
    return floor;
}

int begins_between(const PhysicsWorld& world, PhysicsFixture a, PhysicsFixture b) {
    return static_cast<int>(std::ranges::count_if(world.contacts(), [&](const kin::PhysicsContactEvent& event) {
        return event.type == kin::PhysicsContactEventType::Begin &&
               ((event.fixture_a == a && event.fixture_b == b) || (event.fixture_a == b && event.fixture_b == a));
    }));
}

void test_capsules() {
    PhysicsWorld world({.gravity = {0.0f, 10.0f}});
    floor_of(world);
    // Standing (taller than wide): round at its top and bottom.
    const PhysicsBody body = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {0.0f, -3.0f}, .fixed_rotation = true});
    const PhysicsFixture capsule = world.add_fixture(body, {.shape = PhysicsShape::capsule({0.25f, 0.75f})});
    assert(world.shape_parts(capsule) == 3);
    const std::vector<kin::PhysicsDebugShape> shapes = world.debug_shapes();
    assert(std::ranges::count(shapes, kin::PhysicsDebugShapeType::Circle, &kin::PhysicsDebugShape::type) == 2);
    run(world, 180);
    // It rests on its round bottom: its centre 0.75 above the floor.
    assert(near(world.position(body).y, -0.75f, 0.03f));
    // One fixture: a ray through it hits it once.
    const std::vector<kin::PhysicsRaycastHit> hits = world.raycast({0.0f, -5.0f}, {0.0f, -0.1f});
    assert(hits.size() == 1 && hits[0].fixture == capsule);
    assert(near(hits[0].point.y, -1.5f, 0.02f));
    // As wide as long: a circle.
    const PhysicsBody ball = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {5.0f, -3.0f}});
    assert(world.shape_parts(world.add_fixture(ball, {.shape = PhysicsShape::capsule({0.5f, 0.5f})})) == 1);
}

void test_polygons() {
    PhysicsWorld world;
    const PhysicsBody body = world.create_body({.type = PhysicsBodyType::Static});
    // A triangle: one Box2D polygon.
    assert(world.shape_parts(world.add_fixture(body, {.shape = PhysicsShape::polygon({{0, 0}, {2, 0}, {0, 2}}, {10, 10})})) == 1);
    // An L (concave): cut in triangles, still one fixture.
    const PhysicsFixture l = world.add_fixture(body, {.shape = PhysicsShape::polygon({{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}})});
    assert(world.shape_parts(l) >= 4);
    const auto inside = [&](Vec2f point) { return std::ranges::count(world.query_point(point), l) == 1; };
    assert(inside({0.5f, 0.5f}) && inside({2.5f, 0.5f}) && inside({0.5f, 2.5f}));
    assert(!inside({2.0f, 2.0f})); // the notch
    // On a seam between its triangles: found once.
    for (const Vec2f at : {Vec2f{1.0f, 1.0f}, Vec2f{0.5f, 1.0f}, Vec2f{1.0f, 0.5f}}) {
        const std::vector<PhysicsFixture> found = world.query_point(at);
        assert(std::ranges::count(found, l) <= 1);
    }
    assert(std::ranges::count(world.query_aabb({-1.0f, -1.0f, 5.0f, 5.0f}), l) == 1);
    // Turned with its offset: a box's corners as a polygon, a quarter turn.
    PhysicsShape turned = PhysicsShape::polygon({{0, 0}, {4, 0}, {4, 1}, {0, 1}}, {20, 0});
    turned.angle = 3.14159265f / 2.0f;
    const PhysicsFixture upright = world.add_fixture(body, {.shape = turned});
    assert(std::ranges::count(world.query_point({19.5f, 3.0f}), upright) == 1); // along +y now, to the left of x = 20
    assert(world.query_point({23.0f, 0.5f}).empty());
    // Points with no area: a fixture of nothing.
    const PhysicsFixture flat = world.add_fixture(body, {.shape = PhysicsShape::polygon({{0, 0}, {1, 0}, {2, 0}})});
    assert(world.is_alive(flat) && world.shape_parts(flat) == 0);
    assert(world.shape_parts(world.add_fixture(body, {.shape = PhysicsShape::polygon({{0, 0}, {1, 1}})})) == 0);
    world.destroy_fixture(flat);
    assert(!world.is_alive(flat));
}

void test_chains() {
    PhysicsWorld world({.gravity = {0.0f, 10.0f}});
    const PhysicsBody ground = world.create_body({.type = PhysicsBodyType::Static});
    // A two-sided line, three segments: what falls on it rests on it.
    const PhysicsFixture line = world.add_fixture(ground, {.shape = PhysicsShape::chain({{-6, 0}, {-2, 0}, {2, 0}, {6, 0}})});
    assert(world.shape_parts(line) == 3);
    const PhysicsBody box = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {-2.0f, -2.0f}, .fixed_rotation = true});
    const PhysicsFixture crate = world.add_fixture(box, {.shape = PhysicsShape::box({1.0f, 0.5f})});
    int begins = 0;
    for (int i = 0; i < 120; ++i) {
        world.step(dt);
        begins += begins_between(world, line, crate);
    }
    assert(near(world.position(box).y, -0.5f, 0.03f));
    // The crate lies across two segments: one contact between the two.
    assert(begins == 1);
    assert(std::ranges::count_if(world.current_contacts(), [&](const kin::PhysicsContact& contact) {
               return contact.fixture_a == line || contact.fixture_b == line;
           }) == 1);
    // From below it stops what rises too.
    const PhysicsBody rising = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {3.0f, 2.0f}, .gravity_scale = 0.0f});
    world.add_fixture(rising, {.shape = PhysicsShape::circle(0.25f)});
    world.set_linear_velocity(rising, {0.0f, -5.0f});
    run(world, 60);
    assert(world.position(rising).y > 0.2f);

    // One-sided, drawn left to right: stops what falls from above, lets
    // through what comes up from below.
    PhysicsWorld platforms({.gravity = {0.0f, 10.0f}});
    const PhysicsBody still = platforms.create_body({.type = PhysicsBodyType::Static});
    PhysicsShape platform = PhysicsShape::chain({{-3, 0}, {3, 0}});
    platform.one_sided = true;
    assert(platforms.shape_parts(platforms.add_fixture(still, {.shape = platform})) == 1);
    const PhysicsBody faller = platforms.create_body({.type = PhysicsBodyType::Dynamic, .position = {-1.0f, -2.0f}});
    platforms.add_fixture(faller, {.shape = PhysicsShape::circle(0.25f)});
    const PhysicsBody jumper = platforms.create_body({.type = PhysicsBodyType::Dynamic, .position = {1.0f, 2.0f}, .gravity_scale = 0.0f});
    platforms.add_fixture(jumper, {.shape = PhysicsShape::circle(0.25f)});
    platforms.set_linear_velocity(jumper, {0.0f, -5.0f});
    run(platforms, 90);
    assert(near(platforms.position(faller).y, -0.25f, 0.05f));
    assert(platforms.position(jumper).y < -2.0f);

    // A loop, wound clockwise (on screen), one-sided: stops what comes from outside.
    PhysicsShape ring = PhysicsShape::chain({{-2, -1}, {2, -1}, {2, 1}, {-2, 1}}, true);
    ring.one_sided = true;
    const PhysicsBody pit = platforms.create_body({.type = PhysicsBodyType::Static, .position = {20.0f, 0.0f}});
    assert(platforms.shape_parts(platforms.add_fixture(pit, {.shape = ring})) == 1);
    const PhysicsBody dropped = platforms.create_body({.type = PhysicsBodyType::Dynamic, .position = {20.0f, -4.0f}});
    platforms.add_fixture(dropped, {.shape = PhysicsShape::circle(0.25f)});
    run(platforms, 120);
    assert(near(platforms.position(dropped).y, -1.25f, 0.05f));
    // Drawn as debug lines (a loop's last point its first again).
    const std::vector<kin::PhysicsDebugShape> shapes = platforms.debug_shapes();
    assert(std::ranges::any_of(shapes, [](const kin::PhysicsDebugShape& shape) {
        return shape.type == kin::PhysicsDebugShapeType::Chain && shape.points.size() == 5;
    }));
}

void test_joints() {
    // A pendulum: a bob on a hinge to the world swings, its string as long.
    PhysicsWorld world({.gravity = {0.0f, 10.0f}});
    const PhysicsBody bob = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {2.0f, 0.0f}});
    world.add_fixture(bob, {.shape = PhysicsShape::circle(0.2f)});
    // Joined to no body: to the world, at (0, 0).
    const kin::PhysicsJoint hinge = world.create_joint({.type = PhysicsJointType::Hinge, .body_a = bob, .anchor = {0.0f, 0.0f}});
    assert(world.is_alive(hinge));
    run(world, 30);
    assert(near(distance(world.position(bob), {0.0f, 0.0f}), 2.0f, 0.02f));
    assert(world.position(bob).y > 0.5f); // it swung down
    assert(world.joint_value(hinge) > 0.2f); // and turned (clockwise on screen: y is down)
    assert(world.debug_joints().size() == 1);

    // A motor turns it at its speed (up to its torque).
    PhysicsWorld still;
    const PhysicsBody wheel = still.create_body({.type = PhysicsBodyType::Dynamic});
    still.add_fixture(wheel, {.shape = PhysicsShape::circle(1.0f)});
    const kin::PhysicsJoint axle = still.create_joint({.type = PhysicsJointType::Hinge,
                                                       .body_a = wheel,
                                                       .motor = true,
                                                       .motor_speed = 2.0f,
                                                       .max_motor_force = 1000.0f});
    run(still, 60);
    assert(near(still.joint_value(axle), 2.0f, 0.1f));
    still.update_joint(axle, {.motor = true, .motor_speed = -2.0f, .max_motor_force = 1000.0f});
    run(still, 60);
    assert(near(still.joint_value(axle), 0.0f, 0.15f));

    // A rope: a weight hangs no further than its length, and pulls on it.
    const PhysicsBody weight = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {10.0f, 1.0f}});
    world.add_fixture(weight, {.shape = PhysicsShape::circle(0.2f), .density = 1.0f});
    const kin::PhysicsJoint rope = world.create_joint({.type = PhysicsJointType::Distance,
                                                       .body_a = weight,
                                                       .anchor = {10.0f, 1.0f},
                                                       .other_anchor = {10.0f, -1.0f},
                                                       .limit = true,
                                                       .lower = 0.0f,
                                                       .upper = 3.0f});
    run(world, 120);
    assert(near(world.position(weight).y, 2.0f, 0.05f)); // three below (10, -1)
    const f32 weight_mass = 3.14159265f * 0.2f * 0.2f;
    assert(near(std::hypot(world.joint_force(rope).x, world.joint_force(rope).y), weight_mass * 10.0f, 0.1f));
    assert(near(world.joint_value(rope), 3.0f, 0.05f));

    // A slider: along its axis only, within its limits.
    const PhysicsBody slide = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {20.0f, 0.0f}});
    world.add_fixture(slide, {.shape = PhysicsShape::box({0.5f, 0.5f})});
    world.create_joint({.type = PhysicsJointType::Slider,
                        .body_a = slide,
                        .anchor = {20.0f, 0.0f},
                        .axis = {1.0f, 1.0f},
                        .limit = true,
                        .lower = -1.0f,
                        .upper = 1.0f});
    run(world, 120);
    const Vec2f slid = world.position(slide);
    assert(near(slid.x - 20.0f, slid.y, 0.02f));                          // on the diagonal
    assert(near(distance(slid, {20.0f, 0.0f}), 1.0f, 0.05f) && slid.y > 0.0f); // at its end, down

    // Joints go with their bodies.
    world.destroy_body(bob);
    assert(!world.is_alive(hinge) && world.is_alive(rope));
    // None to make: no body, or both ends one.
    assert(!world.create_joint({.body_a = bob}));
    assert(!world.create_joint({.body_a = weight, .body_b = weight}));
}

void test_joints_in_units() {
    // In points (32 a metre), a spring's and a motor's numbers are still the game's.
    PhysicsWorld world({.gravity = {0.0f, 320.0f}, .units_per_meter = 32.0f});
    const PhysicsBody load = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {0.0f, 64.0f}});
    world.add_fixture(load, {.shape = PhysicsShape::box({8.0f, 8.0f})});
    world.create_joint({.type = PhysicsJointType::Distance,
                        .body_a = load,
                        .anchor = {0.0f, 64.0f},
                        .other_anchor = {0.0f, 0.0f},
                        .frequency = 2.0f,
                        .damping_ratio = 1.0f});
    run(world, 240);
    // It hangs lower than its rest length, by its weight over the stiffness:
    // g / (2 pi f)^2 = 320 / 157.9 = 2.03 points.
    assert(near(world.position(load).y, 66.03f, 0.3f));

    const PhysicsBody fixed = world.create_body({.type = PhysicsBodyType::Static, .position = {200.0f, 0.0f}});
    const PhysicsBody arm = world.create_body({.type = PhysicsBodyType::Dynamic, .position = {200.0f, 0.0f}, .gravity_scale = 0.0f});
    world.add_fixture(arm, {.shape = PhysicsShape::box({32.0f, 4.0f})});
    const kin::PhysicsJoint weld = world.create_joint({.type = PhysicsJointType::Weld, .body_a = arm, .body_b = fixed, .anchor = {200.0f, 0.0f}});
    world.apply_impulse(arm, {0.0f, 100.0f});
    run(world, 60);
    assert(world.is_alive(weld) && near(world.position(arm).y, 0.0f, 0.5f) && near(world.rotation(arm), 0.0f, 0.02f));
}

} // namespace

int main() {
    test_capsules();
    test_polygons();
    test_chains();
    test_joints();
    test_joints_in_units();
    return 0;
}
