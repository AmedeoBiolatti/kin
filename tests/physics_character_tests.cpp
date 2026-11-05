// Shapes cast and overlapped, characters moved (PhysicsWorld::move_character)
// and CharacterController2D: landing, sliding, slopes, snapping, one-sided
// platforms (for characters and for dynamic bodies), pushing, being carried.
// In points, 32 to a metre, y down, as games are.

#include <kin/ecs/physics.hpp>
#include <kin/ecs/world.hpp>
#include <kin/physics/physics.hpp>
#include <kin/scene/scene_asset.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <optional>

namespace {

constexpr kin::f32 ppm = 32.0f;
constexpr kin::f32 dt = 1.0f / 60.0f;

bool near(kin::f32 a, kin::f32 b, kin::f32 epsilon = 0.5f) {
    return std::fabs(a - b) <= epsilon;
}

kin::PhysicsWorld points_world(kin::Vec2f gravity = {}) {
    return kin::PhysicsWorld{{.gravity = gravity, .units_per_meter = ppm}};
}

// A static box: its top at `top`, from `left` to `right`, 32 deep.
kin::PhysicsBody ground(kin::PhysicsWorld& world, kin::f32 top, kin::f32 left = -400.0f, kin::f32 right = 400.0f, bool one_sided = false) {
    const kin::PhysicsBody body = world.create_body({.type = kin::PhysicsBodyType::Static, .position = {0.5f * (left + right), top + 16.0f}});
    kin::PhysicsShape shape = kin::PhysicsShape::box({0.5f * (right - left), 16.0f});
    shape.one_sided = one_sided;
    world.add_fixture(body, {.shape = shape});
    return body;
}

kin::PhysicsBody wall(kin::PhysicsWorld& world, kin::f32 left) {
    const kin::PhysicsBody body = world.create_body({.type = kin::PhysicsBodyType::Static, .position = {left + 16.0f, 0.0f}});
    world.add_fixture(body, {.shape = kin::PhysicsShape::box({16.0f, 1000.0f})});
    return body;
}

// A slope rising to the right at `degrees`, from (0, 0) on.
kin::PhysicsBody slope(kin::PhysicsWorld& world, kin::f32 degrees) {
    const kin::f32 run = 300.0f;
    const kin::f32 rise = run * std::tan(degrees * 3.14159265f / 180.0f);
    const kin::PhysicsBody body = world.create_body({.type = kin::PhysicsBodyType::Static});
    world.add_fixture(body, {.shape = kin::PhysicsShape::polygon({{0.0f, 0.0f}, {run, -rise}, {run, 40.0f}, {0.0f, 40.0f}})});
    return body;
}

// A character's shape: 16 wide, 32 tall.
kin::PhysicsCharacterDef character(kin::PhysicsBody self = {}) {
    return {.shape = kin::PhysicsShape::box({8.0f, 16.0f}), .filter = {.ignore = self}};
}

void test_casts() {
    kin::PhysicsWorld world = points_world();
    const kin::PhysicsBody floor = ground(world, 100.0f);
    // Falling: each shape stops on the floor's top (y 100).
    for (const kin::PhysicsShape& shape : {kin::PhysicsShape::circle(10.0f), kin::PhysicsShape::box({10.0f, 10.0f}),
                                           kin::PhysicsShape::capsule({10.0f, 5.0f})}) {
        const kin::f32 bottom = shape.type == kin::PhysicsShapeType::Capsule ? 5.0f : 10.0f;
        const std::optional<kin::PhysicsShapeHit> hit = world.cast_shape(shape, {0.0f, 0.0f}, {0.0f, 200.0f});
        assert(hit && hit->body == floor);
        assert(near(hit->fraction * 200.0f, 100.0f - bottom, 0.3f));
        assert(near(hit->normal.x, 0.0f, 0.01f) && near(hit->normal.y, -1.0f, 0.01f));
        assert(near(hit->point.y, 100.0f, 0.3f));
    }
    // Short of it, or along it from on it: nothing.
    assert(!world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 0.0f}, {0.0f, 50.0f}));
    assert(!world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 90.0f}, {100.0f, 90.0f}));
    // Away from it, from inside it: nothing either; into it from there: at once.
    assert(!world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 95.0f}, {0.0f, 0.0f}));
    const std::optional<kin::PhysicsShapeHit> pressed = world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 90.0f}, {0.0f, 120.0f});
    assert(pressed && pressed->fraction < 0.01f);
    // Its body ignored, or a mask that leaves it out: nothing.
    assert(!world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 0.0f}, {0.0f, 200.0f}, {.ignore = floor}));
    assert(!world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 0.0f}, {0.0f, 200.0f}, {.mask_bits = 0x0002}));
    // A sensor in the way is hit unless sensors are left out.
    const kin::PhysicsBody zone = world.create_body({.position = {0.0f, 50.0f}});
    world.add_fixture(zone, {.shape = kin::PhysicsShape::box({20.0f, 5.0f}), .sensor = true});
    assert(world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 0.0f}, {0.0f, 200.0f})->body == zone);
    assert(world.cast_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 0.0f}, {0.0f, 200.0f}, {.include_sensors = false})->body == floor);

    // Overlaps: in it, touching it, above it.
    assert(world.query_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 95.0f}, {.include_sensors = false}).size() == 1);
    assert(world.query_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 90.0f}, {.include_sensors = false}).empty());
    assert(world.query_shape(kin::PhysicsShape::box({30.0f, 30.0f}), {0.0f, 50.0f}).size() == 1); // the sensor
    assert(world.body_of(world.query_shape(kin::PhysicsShape::box({30.0f, 30.0f}), {0.0f, 50.0f}).front()) == zone);
    assert(world.query_shape(kin::PhysicsShape::circle(10.0f), {0.0f, 0.0f}).empty());
}

void test_moves() {
    kin::PhysicsWorld world = points_world();
    const kin::PhysicsBody floor = ground(world, 100.0f);
    const kin::PhysicsBody side = wall(world, 200.0f);

    // Falling onto the floor: it stops just above it, on it, its fall gone.
    kin::PhysicsCharacterMove moved = world.move_character(character(), {0.0f, 0.0f}, {0.0f, 9000.0f}, dt);
    assert(moved.on_floor && moved.floor == floor && !moved.on_wall);
    assert(moved.position.y < 84.0f && moved.position.y > 82.5f);
    assert(moved.velocity.y == 0.0f && near(moved.floor_normal.y, -1.0f, 0.01f));
    assert(moved.hits.size() == 1 && moved.hits.front().body == floor);
    const kin::Vec2f standing = moved.position;

    // Standing: still on it; walking: along it.
    moved = world.move_character(character(), standing, {}, dt, true);
    assert(moved.on_floor && near(moved.position.y, standing.y, 0.05f));
    moved = world.move_character(character(), standing, {600.0f, 0.0f}, dt, true);
    assert(moved.on_floor && near(moved.position.x, 10.0f, 0.05f) && near(moved.position.y, standing.y, 0.05f));

    // Into the wall, falling: it slides down it, its push into it gone.
    moved = world.move_character(character(), {150.0f, 0.0f}, {6000.0f, 1200.0f}, dt);
    assert(moved.on_wall && !moved.on_floor && moved.hits.front().body == side);
    assert(moved.position.x < 192.0f && moved.position.x > 190.5f);
    assert(near(moved.position.y, 20.0f, 0.05f));
    assert(near(moved.velocity.x, 0.0f, 0.1f) && near(moved.velocity.y, 1200.0f) && near(moved.wall_normal.x, -1.0f, 0.01f));

    // A ceiling stops its rise.
    kin::PhysicsWorld roofed = points_world();
    ground(roofed, -32.0f, -400.0f, 400.0f);
    moved = roofed.move_character(character(), {0.0f, 40.0f}, {300.0f, -6000.0f}, dt);
    assert(moved.on_ceiling && near(moved.velocity.y, 0.0f, 0.1f) && near(moved.velocity.x, 300.0f));
    assert(moved.position.y > 16.0f && moved.position.y < 17.5f && near(moved.position.x, 5.0f, 0.05f));
}

void test_slopes() {
    // Walking up a gentle slope: as fast along it as on the flat; on it.
    {
        kin::PhysicsWorld world = points_world();
        slope(world, 30.0f);
        ground(world, 0.0f, -400.0f, 0.0f);
        kin::Vec2f at = world.move_character(character(), {-40.0f, -100.0f}, {0.0f, 9000.0f}, dt).position;
        bool on_floor = true;
        for (int i = 0; i < 60; ++i) {
            const kin::PhysicsCharacterMove moved = world.move_character(character(), at, {120.0f, 0.0f}, dt, on_floor);
            on_floor = moved.on_floor;
            at = moved.position;
            assert(on_floor);
        }
        // 2 points a step, 120 in all: 32 to the slope's foot, 88 along it (76 across, 44 up).
        assert(at.x > 62.0f && at.x < 72.0f);
        assert(at.y < -56.0f && at.y > -66.0f);
    }
    // Too steep: a wall; walking into it climbs nothing.
    {
        kin::PhysicsWorld world = points_world();
        slope(world, 60.0f);
        ground(world, 0.0f, -400.0f, 0.0f);
        kin::Vec2f at = world.move_character(character(), {-12.0f, -100.0f}, {0.0f, 9000.0f}, dt).position;
        const kin::f32 level = at.y;
        bool on_floor = true;
        for (int i = 0; i < 30; ++i) {
            const kin::PhysicsCharacterMove moved = world.move_character(character(), at, {300.0f, 0.0f}, dt, on_floor);
            on_floor = moved.on_floor;
            at = moved.position;
        }
        assert(on_floor && near(at.y, level, 0.1f) && at.x < 0.0f);
    }
    // Standing on a slope it doesn't slide; with stop_on_slope off, falling onto it, it slides down.
    {
        kin::PhysicsWorld world = points_world();
        slope(world, 30.0f);
        const kin::PhysicsCharacterMove landed = world.move_character(character(), {150.0f, -150.0f}, {0.0f, 9000.0f}, dt);
        assert(landed.on_floor);
        const kin::PhysicsCharacterMove stood = world.move_character(character(), landed.position, {0.0f, 600.0f}, dt, true);
        assert(stood.on_floor && near(stood.position.x, landed.position.x, 0.01f) && near(stood.position.y, landed.position.y, 0.05f));
        kin::PhysicsCharacterDef sliding = character();
        sliding.stop_on_slope = false;
        const kin::PhysicsCharacterMove slid = world.move_character(sliding, landed.position, {0.0f, 600.0f}, dt, true);
        assert(slid.position.x < landed.position.x - 1.0f && slid.position.y > landed.position.y);
    }
}

void test_snap() {
    // Walking off a step 10 lower: with snap it stays on the floor, without it falls.
    kin::PhysicsWorld world = points_world();
    ground(world, 0.0f, -400.0f, 0.0f);
    ground(world, 10.0f, 0.0f, 400.0f);
    const kin::Vec2f start = world.move_character(character(), {-12.0f, -50.0f}, {0.0f, 9000.0f}, dt).position;
    kin::PhysicsCharacterDef snapping = character();
    snapping.snap = 16.0f;
    const kin::PhysicsCharacterMove kept = world.move_character(snapping, start, {1800.0f, 0.0f}, dt, true);
    assert(kept.on_floor && near(kept.position.y, start.y + 10.0f, 0.1f));
    const kin::PhysicsCharacterMove off = world.move_character(character(), start, {1800.0f, 0.0f}, dt, true);
    assert(!off.on_floor && near(off.position.y, start.y, 0.05f));
    // Going up, it is not snapped down.
    const kin::PhysicsCharacterMove jumping = world.move_character(snapping, start, {1800.0f, -300.0f}, dt, true);
    assert(!jumping.on_floor);
}

void test_one_way() {
    for (const bool chain : {false, true}) {
        kin::PhysicsWorld world = points_world();
        kin::PhysicsBody platform;
        if (chain) {
            platform = world.create_body({.type = kin::PhysicsBodyType::Static});
            kin::PhysicsShape line = kin::PhysicsShape::chain({{-100.0f, 0.0f}, {100.0f, 0.0f}});
            line.one_sided = true;
            world.add_fixture(platform, {.shape = line});
        } else {
            platform = ground(world, 0.0f, -100.0f, 100.0f, true);
        }
        // From below, going up: through it.
        kin::PhysicsCharacterMove moved = world.move_character(character(), {0.0f, 60.0f}, {0.0f, -6000.0f}, dt);
        assert(moved.hits.empty() && near(moved.position.y, -40.0f, 0.05f));
        // From the side, inside it: through it, and not pushed out of it.
        moved = world.move_character(character(), {-130.0f, 10.0f}, {3000.0f, 0.0f}, dt);
        assert(moved.hits.empty() && near(moved.position.x, -80.0f, 0.05f) && near(moved.position.y, 10.0f, 0.05f));
        // From above: onto it.
        moved = world.move_character(character(), {0.0f, -60.0f}, {0.0f, 6000.0f}, dt);
        assert(moved.on_floor && moved.floor == platform && moved.position.y < -16.0f && moved.position.y > -17.5f);
        const kin::Vec2f on_it = moved.position;
        // Standing on it, it is a floor; walking along it too.
        moved = world.move_character(character(), on_it, {300.0f, 0.0f}, dt, true);
        assert(moved.on_floor && near(moved.position.y, on_it.y, 0.05f));
        // Dropping through: one_way off.
        kin::PhysicsCharacterDef dropping = character();
        dropping.one_way = false;
        moved = world.move_character(dropping, on_it, {0.0f, 3000.0f}, dt, true);
        assert(!moved.on_floor && moved.position.y > on_it.y + 40.0f);
        // Halfway through it, falling: through it still (it came from below).
        moved = world.move_character(character(), {0.0f, 4.0f}, {0.0f, 600.0f}, dt);
        assert(moved.hits.empty() && !moved.on_floor);
        // Casts: down onto it, a hit; up through it, none.
        assert(world.cast_shape(kin::PhysicsShape::circle(5.0f), {0.0f, -50.0f}, {0.0f, 50.0f}));
        assert(!world.cast_shape(kin::PhysicsShape::circle(5.0f), {0.0f, 50.0f}, {0.0f, -50.0f}));
    }

    // Dynamic bodies: a ball thrown up through a one-sided box comes down onto it.
    kin::PhysicsWorld world = points_world({0.0f, 9.8f * ppm});
    ground(world, 0.0f, -100.0f, 100.0f, true);
    const kin::PhysicsBody ball = world.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {0.0f, 60.0f}, .linear_velocity = {0.0f, -500.0f}});
    world.add_fixture(ball, {.shape = kin::PhysicsShape::circle(8.0f)});
    kin::f32 highest = 60.0f;
    for (int i = 0; i < 240; ++i) {
        world.step(dt);
        highest = std::min(highest, world.position(ball).y);
    }
    assert(highest < -30.0f); // it went up through
    assert(near(world.position(ball).y, -8.0f, 1.0f) && near(world.linear_velocity(ball).y, 0.0f, 1.0f)); // and rests on top
    // From the side, it passes through.
    const kin::PhysicsBody bullet = world.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {-140.0f, 16.0f},
                                                       .linear_velocity = {600.0f, 0.0f}, .gravity_scale = 0.0f});
    world.add_fixture(bullet, {.shape = kin::PhysicsShape::circle(4.0f)});
    for (int i = 0; i < 60; ++i) {
        world.step(dt);
    }
    assert(world.position(bullet).x > 300.0f);
}

void test_push() {
    for (const kin::f32 push : {1.0f, 0.0f}) {
        kin::PhysicsWorld world = points_world({0.0f, 9.8f * ppm});
        ground(world, 100.0f);
        const kin::PhysicsBody crate = world.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {30.0f, 84.0f}});
        world.add_fixture(crate, {.shape = kin::PhysicsShape::box({16.0f, 16.0f}), .friction = 0.0f});
        for (int i = 0; i < 30; ++i) {
            world.step(dt); // settled
        }
        kin::PhysicsCharacterDef def = character();
        def.push = push;
        const kin::Vec2f start = world.move_character(def, {0.0f, 50.0f}, {0.0f, 9000.0f}, dt).position;
        kin::PhysicsCharacterMove moved = world.move_character(def, start, {600.0f, 0.0f}, dt, true);
        assert(moved.on_wall && moved.hits.back().body == crate);
        if (push > 0.0f) {
            assert(near(world.linear_velocity(crate).x, 600.0f, 1.0f));
        } else {
            assert(near(world.linear_velocity(crate).x, 0.0f, 0.5f));
        }
    }
}

// CharacterController2D: falls by its own gravity, lands, walks; carried by
// a moving platform; its body made for it; described to scenes.
void test_controller() {
    kin::EcsWorld ecs;
    kin::register_physics_components(ecs);
    kin::PhysicsWorld physics = points_world({0.0f, 9.8f * ppm});
    kin::EcsPhysicsBridge bridge{ecs, physics};

    ecs.entity("Floor")
        .set(kin::Transform2D{.pos = {0.0f, 116.0f}})
        .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
        .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({400.0f, 16.0f})}}}});
    // A coin to touch: a sensor on a static body (sensors see the character's body).
    kin::EcsEntity coin = ecs.entity("Coin")
                              .set(kin::Transform2D{.pos = {60.0f, 84.0f}})
                              .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
                              .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::circle(6.0f), .sensor = true}}}});
    kin::EcsEntity hero = ecs.entity("Hero")
                              .set(kin::Transform2D{.pos = {0.0f, 0.0f}})
                              .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Kinematic}})
                              .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({8.0f, 16.0f})}}}})
                              .set(kin::PhysicsContactQueue{})
                              .set(kin::CharacterController2D{.gravity = 2000.0f});
    for (int i = 0; i < 60; ++i) {
        kin::update_physics_world(ecs, physics, dt);
    }
    const kin::CharacterController2D* state = hero.get<kin::CharacterController2D>();
    assert(state->on_floor && state->velocity.y == 0.0f);
    assert(hero.get<kin::Transform2D>()->pos.y > 82.5f && hero.get<kin::Transform2D>()->pos.y < 84.0f);
    const kin::PhysicsBody body = hero.get<kin::PhysicsBodyComponent>()->body;
    assert(physics.fixtures(body).size() == 1);

    // Walking right into the coin: it is touched.
    bool touched = false;
    hero.get_mut<kin::CharacterController2D>()->velocity = {300.0f, 0.0f};
    for (int i = 0; i < 20; ++i) {
        kin::update_physics_world(ecs, physics, dt);
        for (const kin::PhysicsContactEvent& event : hero.get<kin::PhysicsContactQueue>()->events) {
            touched = touched || (event.type == kin::PhysicsContactEventType::Begin && event.sensor);
        }
    }
    assert(touched && hero.get<kin::CharacterController2D>()->on_floor);
    assert(near(hero.get<kin::Transform2D>()->pos.x, 100.0f, 1.0f));
    (void)coin;

    // A jump: off the floor, up, and down again.
    hero.get_mut<kin::CharacterController2D>()->velocity = {0.0f, -600.0f};
    kin::update_physics_world(ecs, physics, dt);
    assert(!hero.get<kin::CharacterController2D>()->on_floor && hero.get<kin::Transform2D>()->pos.y < 80.0f);
    for (int i = 0; i < 90; ++i) {
        kin::update_physics_world(ecs, physics, dt);
    }
    assert(hero.get<kin::CharacterController2D>()->on_floor);

    // A platform moving right and one moving down carry it.
    for (const kin::Vec2f moving : {kin::Vec2f{120.0f, 0.0f}, kin::Vec2f{0.0f, 60.0f}, kin::Vec2f{0.0f, -60.0f}}) {
        kin::EcsWorld world;
        kin::register_physics_components(world);
        kin::PhysicsWorld sim = points_world({0.0f, 9.8f * ppm});
        kin::EcsPhysicsBridge links{world, sim};
        kin::EcsEntity lift = world.entity("Lift")
                                  .set(kin::Transform2D{.pos = {0.0f, 16.0f}})
                                  .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Kinematic}})
                                  .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({60.0f, 16.0f})}}}});
        kin::EcsEntity rider = world.entity("Rider")
                                   .set(kin::Transform2D{.pos = {0.0f, -40.0f}})
                                   .set(kin::PhysicsBodyComponent{})
                                   .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({8.0f, 16.0f})}}}})
                                   .set(kin::CharacterController2D{.gravity = 2000.0f});
        for (int i = 0; i < 40; ++i) {
            kin::update_physics_world(world, sim, dt); // landed
        }
        assert(rider.get<kin::CharacterController2D>()->on_floor);
        const kin::f32 above = lift.get<kin::Transform2D>()->pos.y - rider.get<kin::Transform2D>()->pos.y;
        lift.set(kin::PhysicsVelocity{.linear = moving});
        for (int i = 0; i < 60; ++i) {
            kin::update_physics_world(world, sim, dt);
            assert(rider.get<kin::CharacterController2D>()->on_floor);
        }
        const kin::Transform2D& lifted = *lift.get<kin::Transform2D>();
        const kin::Transform2D& ridden = *rider.get<kin::Transform2D>();
        assert(near(lifted.pos.x, moving.x, 1.0f) && near(lifted.pos.y, 16.0f + moving.y, 1.0f));
        assert(near(ridden.pos.x, lifted.pos.x, 0.5f) && near(lifted.pos.y - ridden.pos.y, above, 0.5f));
    }

    // Standing on a one-sided platform (a box; a chain, which Box2D makes
    // one-sided itself), drop_through lets it fall through: the simulation's
    // contact with what it stood on doesn't hold it up.
    for (const bool chain : {false, true}) {
        kin::EcsWorld world;
        kin::register_physics_components(world);
        kin::PhysicsWorld sim = points_world({0.0f, 9.8f * ppm});
        kin::EcsPhysicsBridge links{world, sim};
        kin::PhysicsShape top = chain ? kin::PhysicsShape::chain({{-60.0f, 0.0f}, {60.0f, 0.0f}}) : kin::PhysicsShape::box({60.0f, 12.0f}, {0.0f, 12.0f});
        top.one_sided = true;
        world.entity("Ledge")
            .set(kin::Transform2D{})
            .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
            .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = top}}}});
        world.entity("Floor")
            .set(kin::Transform2D{.pos = {0.0f, 216.0f}})
            .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
            .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({200.0f, 16.0f})}}}});
        kin::EcsEntity dropper = world.entity("Dropper")
                                     .set(kin::Transform2D{.pos = {0.0f, -40.0f}})
                                     .set(kin::PhysicsBodyComponent{})
                                     .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({8.0f, 16.0f})}}}})
                                     .set(kin::CharacterController2D{.gravity = 2000.0f});
        for (int i = 0; i < 40; ++i) {
            kin::update_physics_world(world, sim, dt);
        }
        assert(dropper.get<kin::CharacterController2D>()->on_floor && near(dropper.get<kin::Transform2D>()->pos.y, -17.0f, 1.0f));
        dropper.get_mut<kin::CharacterController2D>()->drop_through = 0.25f;
        for (int i = 0; i < 60; ++i) {
            kin::update_physics_world(world, sim, dt);
        }
        assert(dropper.get<kin::CharacterController2D>()->on_floor && near(dropper.get<kin::Transform2D>()->pos.y, 183.0f, 1.0f));
    }

    // Described to scenes (and tools): its settings, and what it found (read only).
    kin::EcsWorld described;
    kin::register_builtin_editor_components(described);
    const kin::ComponentDescriptor* descriptor = described.components().find("CharacterController2D");
    assert(descriptor);
    const auto field = [&](std::string_view name) {
        return std::ranges::find(descriptor->fields, name, &kin::ComponentFieldDescriptor::name);
    };
    for (const char* name : {"velocity", "gravity", "up", "max_slope", "snap", "stop_on_slope", "push", "drop_through"}) {
        assert(field(name) != descriptor->fields.end() && !field(name)->readonly);
    }
    for (const char* name : {"on_floor", "on_wall", "on_ceiling", "floor_normal", "wall_normal"}) {
        assert(field(name) != descriptor->fields.end() && field(name)->readonly);
    }
}

} // namespace

int main() {
    test_casts();
    test_moves();
    test_slopes();
    test_snap();
    test_one_way();
    test_push();
    test_controller();
    std::puts("physics character tests passed");
    return 0;
}
