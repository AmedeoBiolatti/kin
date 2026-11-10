#include <kin/ecs/render.hpp>
#include <kin/physics/debug_render.hpp>
#include <kin/physics/physics.hpp>
#include <kin/renderer/render_queue.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <ranges>
#include <vector>

namespace {

bool near(kin::f32 a, kin::f32 b, kin::f32 epsilon = 0.05f) {
    return std::fabs(a - b) <= epsilon;
}

bool contains(std::span<const kin::PhysicsContactEvent> events, kin::PhysicsContactEventType type) {
    return std::ranges::any_of(events, [type](const kin::PhysicsContactEvent& event) {
        return event.type == type;
    });
}

bool contains_fixture(const std::vector<kin::PhysicsFixture>& fixtures, kin::PhysicsFixture fixture) {
    return std::ranges::find(fixtures, fixture) != fixtures.end();
}

} // namespace

int main() {
    static_assert(kin::Transform2D{.pos = {1.0f, 2.0f}, .rotation = 0.25f}.rotation == 0.25f);

    {
        kin::PhysicsWorld world;
        kin::PhysicsBody body = world.create_body({.type = kin::PhysicsBodyType::Static, .position = {1.0f, 2.0f}, .rotation = 0.5f, .user_id = 42});
        assert(world.is_alive(body));
        assert(world.user_id(body) == 42);
        assert((world.position(body) == kin::Vec2f{1.0f, 2.0f}));
        assert(near(world.rotation(body), 0.5f));

        kin::PhysicsFixture fixture = world.add_fixture(body, {.shape = kin::PhysicsShape::box({0.5f, 0.5f}), .tag = 7});
        assert(world.is_alive(fixture));
        assert(world.fixture_tag(fixture) == 7);
        assert(world.fixtures(body).size() == 1);
        assert(world.fixtures(body).front() == fixture);

        world.destroy_body(body);
        assert(!world.is_alive(body));
        assert(!world.is_alive(fixture));
    }

    {
        kin::PhysicsWorld world;
        kin::PhysicsBody static_body = world.create_body({.type = kin::PhysicsBodyType::Static, .position = {0.0f, 0.0f}});
        kin::PhysicsFixture box = world.add_fixture(static_body, {.shape = kin::PhysicsShape::box({0.5f, 0.5f}), .category_bits = 0x0002});

        kin::PhysicsBody circle_body = world.create_body({.type = kin::PhysicsBodyType::Kinematic, .position = {3.0f, 0.0f}});
        kin::PhysicsFixture circle = world.add_fixture(circle_body, {.shape = kin::PhysicsShape::circle(0.5f), .category_bits = 0x0004});

        assert(contains_fixture(world.query_point({0.0f, 0.0f}), box));
        assert(!contains_fixture(world.query_point({3.0f, 0.0f}, {.include_sensors = true}), box));

        std::vector<kin::PhysicsFixture> aabb = world.query_aabb({-1.0f, -1.0f, 5.0f, 2.0f});
        assert(contains_fixture(aabb, box));
        assert(contains_fixture(aabb, circle));

        std::vector<kin::PhysicsFixture> filtered = world.query_aabb({-1.0f, -1.0f, 5.0f, 2.0f}, {.category_bits = 0x0002, .mask_bits = 0x0002});
        assert(contains_fixture(filtered, box));
        assert(!contains_fixture(filtered, circle));

        std::vector<kin::PhysicsRaycastHit> hits = world.raycast({-2.0f, 0.0f}, {4.0f, 0.0f});
        assert(hits.size() >= 2);
        assert(hits.front().fixture == box);

        std::vector<kin::PhysicsDebugShape> debug = world.debug_shapes();
        assert(debug.size() == 2);
        assert(std::ranges::any_of(debug, [](const kin::PhysicsDebugShape& shape) {
            return shape.type == kin::PhysicsDebugShapeType::Polygon && shape.points.size() == 4;
        }));
        assert(std::ranges::any_of(debug, [](const kin::PhysicsDebugShape& shape) {
            return shape.type == kin::PhysicsDebugShapeType::Circle && near(shape.radius, 0.5f);
        }));

        kin::RenderQueue queue{kin::RenderSortMode::Submission};
        kin::submit_physics_debug(queue, world, {.circle_segments = 12, .draw_contacts = false});
        assert(queue.size() == 16);
        assert(std::ranges::all_of(queue.commands(), [](const kin::RenderCommand& command) {
            return command.type == kin::RenderCommandType::Line && command.key.layer == kin::layer_value(kin::RenderLayer::Debug);
        }));
    }

    {
        kin::PhysicsWorld world;
        kin::PhysicsBody wall = world.create_body({.type = kin::PhysicsBodyType::Static, .position = {0.0f, 0.0f}});
        world.add_fixture(wall, {.shape = kin::PhysicsShape::box({0.5f, 0.5f})});

        kin::PhysicsBody mover = world.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {-2.0f, 0.0f}, .fixed_rotation = true});
        world.add_fixture(mover, {.shape = kin::PhysicsShape::box({0.5f, 0.5f}), .density = 1.0f});
        world.set_linear_velocity(mover, {4.0f, 0.0f});

        for (int i = 0; i < 90; ++i) {
            world.step(1.0f / 60.0f);
        }

        assert(world.position(mover).x < -0.95f);
        assert(world.current_contacts().size() == 1);
    }

    {
        kin::PhysicsWorld world;
        kin::PhysicsBody trigger = world.create_body({.type = kin::PhysicsBodyType::Static, .position = {0.0f, 0.0f}});
        kin::PhysicsFixture sensor = world.add_fixture(trigger, {.shape = kin::PhysicsShape::box({1.0f, 1.0f}), .sensor = true});

        kin::PhysicsBody actor = world.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {0.0f, 0.0f}});
        kin::PhysicsFixture actor_fixture = world.add_fixture(actor, {.shape = kin::PhysicsShape::circle(0.25f), .density = 1.0f});

        world.step(1.0f / 60.0f);
        assert(contains(world.contacts(), kin::PhysicsContactEventType::Begin));
        assert(world.current_contacts().size() == 1);
        assert(world.current_contacts()[0].sensor);

        world.step(1.0f / 60.0f);
        assert(world.contacts().empty());
        assert(world.current_contacts().size() == 1);

        world.set_transform(actor, {5.0f, 0.0f}, 0.0f);
        world.step(1.0f / 60.0f);
        assert(contains(world.contacts(), kin::PhysicsContactEventType::End));
        assert(world.current_contacts().empty());

        assert(world.is_alive(sensor));
        assert(world.is_alive(actor_fixture));
        world.destroy_fixture(sensor);
        assert(!world.is_alive(sensor));
    }

    {
        kin::PhysicsWorld world;
        kin::PhysicsBody wall = world.create_body({.type = kin::PhysicsBodyType::Static, .position = {0.0f, 0.0f}});
        world.add_fixture(wall, {.shape = kin::PhysicsShape::box({0.5f, 0.5f})});

        kin::PhysicsBody mover = world.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {0.0f, 0.0f}});
        world.add_fixture(mover, {.shape = kin::PhysicsShape::box({0.5f, 0.5f}), .density = 1.0f});

        world.step(1.0f / 60.0f);
        const bool has_contact_point = std::ranges::any_of(world.contacts(), [](const kin::PhysicsContactEvent& event) {
            return event.has_point;
        });
        assert(has_contact_point);

        kin::RenderQueue queue_without_contacts{kin::RenderSortMode::Submission};
        kin::submit_physics_debug(queue_without_contacts, world, {.draw_contacts = false});

        kin::RenderQueue queue_with_contacts{kin::RenderSortMode::Submission};
        kin::submit_physics_debug(queue_with_contacts, world);

        assert(queue_with_contacts.size() >= queue_without_contacts.size() + 3);
        assert(queue_with_contacts.commands().back().color == kin::Color::rgb(238, 92, 92));
    }

    {
        kin::PhysicsWorld world_a;
        kin::PhysicsWorld world_b;
        kin::PhysicsBody body_a = world_a.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {0.0f, 0.0f}, .linear_velocity = {1.0f, 0.0f}});
        kin::PhysicsBody body_b = world_b.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {0.0f, 0.0f}, .linear_velocity = {1.0f, 0.0f}});
        world_a.add_fixture(body_a, {.shape = kin::PhysicsShape::circle(0.5f), .density = 1.0f});
        world_b.add_fixture(body_b, {.shape = kin::PhysicsShape::circle(0.5f), .density = 1.0f});

        for (int i = 0; i < 30; ++i) {
            world_a.step(1.0f / 60.0f);
            world_b.step(1.0f / 60.0f);
        }

        assert(near(world_a.position(body_a).x, world_b.position(body_b).x, 0.001f));
        assert(near(world_a.position(body_a).y, world_b.position(body_b).y, 0.001f));
    }

    {
        // In points, 32 to the metre: Box2D sees a 1 m crate moving at 50 m/s,
        // and everything given and given back is in points.
        constexpr kin::f32 dt = 1.0f / 60.0f;
        kin::PhysicsWorld world({.gravity = {0.0f, 9.8f * 32.0f}, .units_per_meter = 32.0f});
        assert(world.units_per_meter() == 32.0f);
        kin::PhysicsBody crate = world.create_body({
            .type = kin::PhysicsBodyType::Dynamic,
            .position = {64.0f, 32.0f},
            .linear_velocity = {1600.0f, 0.0f},
            .gravity_scale = 0.0f,
        });
        world.add_fixture(crate, {.shape = kin::PhysicsShape::box({16.0f, 16.0f})});
        world.step(dt);
        assert(near(world.position(crate).x, 64.0f + 1600.0f * dt, 0.1f));
        assert(near(world.linear_velocity(crate).x, 1600.0f, 0.5f));

        const std::vector<kin::PhysicsDebugShape> shapes = world.debug_shapes();
        assert(shapes.size() == 1 && shapes[0].points.size() == 4);
        const auto [left, right] = std::ranges::minmax(shapes[0].points | std::views::transform([](kin::Vec2f p) { return p.x; }));
        assert(near(right - left, 32.0f, 0.1f));
        const kin::Vec2f at = world.position(crate);
        assert(!world.query_point(at).empty());
        assert(world.query_point({at.x + 20.0f, at.y}).empty());
        assert(!world.query_aabb({at.x + 10.0f, at.y - 2.0f, 20.0f, 4.0f}).empty());
        const std::vector<kin::PhysicsRaycastHit> hits = world.raycast({0.0f, at.y}, {400.0f, at.y});
        assert(!hits.empty() && near(hits[0].point.x, at.x - 16.0f, 0.1f));

        kin::PhysicsBody ball = world.create_body({.type = kin::PhysicsBodyType::Dynamic, .position = {300.0f, 0.0f}});
        world.add_fixture(ball, {.shape = kin::PhysicsShape::circle(8.0f)});
        for (int i = 0; i < 60; ++i) {
            world.step(dt);
        }
        assert(near(world.linear_velocity(ball).y, 9.8f * 32.0f, 1.0f)); // a second's fall
        const std::vector<kin::PhysicsDebugShape> round = world.debug_shapes();
        assert(std::ranges::any_of(round, [](const kin::PhysicsDebugShape& s) { return s.type == kin::PhysicsDebugShapeType::Circle && near(s.radius, 8.0f, 0.01f); }));

        // A unit a metre (the default): no body moves more than 2 a step.
        kin::PhysicsWorld metres;
        assert(metres.units_per_meter() == 1.0f);
        kin::PhysicsBody fast = metres.create_body({.type = kin::PhysicsBodyType::Dynamic, .linear_velocity = {1600.0f, 0.0f}});
        metres.add_fixture(fast, {.shape = kin::PhysicsShape::box({16.0f, 16.0f})});
        metres.step(dt);
        assert(metres.position(fast).x <= 2.001f);

        assert(kin::PhysicsWorld({.units_per_meter = 0.0f}).units_per_meter() == 1.0f);
        assert(kin::PhysicsWorld({.units_per_meter = -4.0f}).units_per_meter() == 1.0f);
    }

    return 0;
}
