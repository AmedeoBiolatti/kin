#include <kin/physics/physics.hpp>

#include <box2d/box2d.h>

#include <algorithm>
#include <set>

namespace kin {
namespace {

constexpr u32 invalid_index = UINT32_MAX;

b2Vec2 to_b2(Vec2f value) {
    return {value.x, value.y};
}

Vec2f from_b2(const b2Vec2& value) {
    return {value.x, value.y};
}

b2BodyType to_b2(PhysicsBodyType type) {
    switch (type) {
    case PhysicsBodyType::Static: return b2_staticBody;
    case PhysicsBodyType::Dynamic: return b2_dynamicBody;
    case PhysicsBodyType::Kinematic: return b2_kinematicBody;
    }
    return b2_staticBody;
}

PhysicsBodyType from_b2(b2BodyType type) {
    switch (type) {
    case b2_staticBody: return PhysicsBodyType::Static;
    case b2_dynamicBody: return PhysicsBodyType::Dynamic;
    case b2_kinematicBody: return PhysicsBodyType::Kinematic;
    }
    return PhysicsBodyType::Static;
}

bool filter_matches(const b2Fixture& fixture, PhysicsFilter filter) {
    if (!filter.include_sensors && fixture.IsSensor()) {
        return false;
    }

    const b2Filter fixture_filter = fixture.GetFilterData();
    return (fixture_filter.categoryBits & filter.mask_bits) != 0
        && (filter.category_bits & fixture_filter.maskBits) != 0;
}

struct ContactKey {
    PhysicsFixture a{};
    PhysicsFixture b{};

    friend bool operator<(const ContactKey& lhs, const ContactKey& rhs) {
        if (lhs.a.index != rhs.a.index) {
            return lhs.a.index < rhs.a.index;
        }
        if (lhs.a.generation != rhs.a.generation) {
            return lhs.a.generation < rhs.a.generation;
        }
        if (lhs.b.index != rhs.b.index) {
            return lhs.b.index < rhs.b.index;
        }
        return lhs.b.generation < rhs.b.generation;
    }
};

ContactKey make_key(PhysicsFixture a, PhysicsFixture b) {
    if (b.index < a.index || (b.index == a.index && b.generation < a.generation)) {
        return {b, a};
    }
    return {a, b};
}

} // namespace

PhysicsShape PhysicsShape::box(Vec2f half_extents, Vec2f offset) {
    PhysicsShape shape;
    shape.type = PhysicsShapeType::Box;
    shape.half_extents = half_extents;
    shape.offset = offset;
    return shape;
}

PhysicsShape PhysicsShape::circle(f32 radius, Vec2f offset) {
    PhysicsShape shape;
    shape.type = PhysicsShapeType::Circle;
    shape.radius = radius;
    shape.offset = offset;
    return shape;
}

struct PhysicsWorld::Impl : b2ContactListener {
    struct BodySlot {
        b2Body* body = nullptr;
        u32 generation = 1;
        u64 user_id = 0;
        std::vector<u32> fixtures;
    };

    struct FixtureSlot {
        b2Fixture* fixture = nullptr;
        PhysicsBody body{};
        u32 generation = 1;
        u64 tag = 0;
    };

    explicit Impl(PhysicsWorldDef def)
        : world(to_b2(def.gravity)) {
        world.SetContactListener(this);
    }

    b2World world;
    std::vector<BodySlot> bodies;
    std::vector<u32> free_bodies;
    std::vector<FixtureSlot> fixtures;
    std::vector<u32> free_fixtures;
    std::vector<PhysicsContactEvent> events;
    std::set<ContactKey> touching;
    std::vector<PhysicsContact> current;

    PhysicsBody body_handle(u32 index) const {
        if (index >= bodies.size() || !bodies[index].body) {
            return {};
        }
        return {index, bodies[index].generation};
    }

    PhysicsFixture fixture_handle(u32 index) const {
        if (index >= fixtures.size() || !fixtures[index].fixture) {
            return {};
        }
        return {index, fixtures[index].generation};
    }

    b2Body* body_ptr(PhysicsBody handle) const {
        if (handle.index >= bodies.size()) {
            return nullptr;
        }
        const BodySlot& slot = bodies[handle.index];
        return slot.generation == handle.generation ? slot.body : nullptr;
    }

    b2Fixture* fixture_ptr(PhysicsFixture handle) const {
        if (handle.index >= fixtures.size()) {
            return nullptr;
        }
        const FixtureSlot& slot = fixtures[handle.index];
        return slot.generation == handle.generation ? slot.fixture : nullptr;
    }

    PhysicsBody handle_for(const b2Body* body) const {
        if (!body) {
            return {};
        }
        const uintptr_t value = const_cast<b2Body*>(body)->GetUserData().pointer;
        if (value == 0 || value > bodies.size()) {
            return {};
        }
        return body_handle(static_cast<u32>(value - 1));
    }

    PhysicsFixture handle_for(const b2Fixture* fixture) const {
        if (!fixture) {
            return {};
        }
        const uintptr_t value = const_cast<b2Fixture*>(fixture)->GetUserData().pointer;
        if (value == 0 || value > fixtures.size()) {
            return {};
        }
        return fixture_handle(static_cast<u32>(value - 1));
    }

    bool alive(PhysicsBody handle) const {
        return body_ptr(handle) != nullptr;
    }

    bool alive(PhysicsFixture handle) const {
        return fixture_ptr(handle) != nullptr;
    }

    void erase_touching_with(PhysicsFixture fixture) {
        for (auto it = touching.begin(); it != touching.end();) {
            if (it->a == fixture || it->b == fixture) {
                it = touching.erase(it);
            } else {
                ++it;
            }
        }
    }

    void rebuild_current_contacts() {
        current.clear();
        current.reserve(touching.size());
        for (const ContactKey& key : touching) {
            b2Fixture* fixture_a = fixture_ptr(key.a);
            b2Fixture* fixture_b = fixture_ptr(key.b);
            if (!fixture_a || !fixture_b) {
                continue;
            }
            current.push_back({
                .body_a = handle_for(fixture_a->GetBody()),
                .body_b = handle_for(fixture_b->GetBody()),
                .fixture_a = key.a,
                .fixture_b = key.b,
                .sensor = fixture_a->IsSensor() || fixture_b->IsSensor(),
            });
        }
    }

    void push_event(PhysicsContactEventType type, b2Contact* contact) {
        b2Fixture* fixture_a = contact->GetFixtureA();
        b2Fixture* fixture_b = contact->GetFixtureB();
        PhysicsContactEvent event{
            .type = type,
            .body_a = handle_for(fixture_a->GetBody()),
            .body_b = handle_for(fixture_b->GetBody()),
            .fixture_a = handle_for(fixture_a),
            .fixture_b = handle_for(fixture_b),
            .sensor = fixture_a->IsSensor() || fixture_b->IsSensor(),
        };

        b2WorldManifold manifold;
        contact->GetWorldManifold(&manifold);
        if (contact->GetManifold()->pointCount > 0) {
            event.has_point = true;
            event.point = from_b2(manifold.points[0]);
            event.normal = from_b2(manifold.normal);
        }
        events.push_back(event);
    }

    void BeginContact(b2Contact* contact) override {
        PhysicsFixture fixture_a = handle_for(contact->GetFixtureA());
        PhysicsFixture fixture_b = handle_for(contact->GetFixtureB());
        if (fixture_a && fixture_b) {
            touching.insert(make_key(fixture_a, fixture_b));
        }
        push_event(PhysicsContactEventType::Begin, contact);
    }

    void EndContact(b2Contact* contact) override {
        PhysicsFixture fixture_a = handle_for(contact->GetFixtureA());
        PhysicsFixture fixture_b = handle_for(contact->GetFixtureB());
        if (fixture_a && fixture_b) {
            touching.erase(make_key(fixture_a, fixture_b));
        }
        push_event(PhysicsContactEventType::End, contact);
    }
};

PhysicsWorld::PhysicsWorld(PhysicsWorldDef def)
    : _impl(std::make_unique<Impl>(def)) {
}

PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

PhysicsBody PhysicsWorld::create_body(const PhysicsBodyDef& def) {
    b2BodyDef body_def;
    body_def.type = to_b2(def.type);
    body_def.position = to_b2(def.position);
    body_def.angle = def.rotation;
    body_def.linearVelocity = to_b2(def.linear_velocity);
    body_def.angularVelocity = def.angular_velocity;
    body_def.linearDamping = def.linear_damping;
    body_def.angularDamping = def.angular_damping;
    body_def.gravityScale = def.gravity_scale;
    body_def.fixedRotation = def.fixed_rotation;

    u32 index = 0;
    if (!_impl->free_bodies.empty()) {
        index = _impl->free_bodies.back();
        _impl->free_bodies.pop_back();
    } else {
        index = static_cast<u32>(_impl->bodies.size());
        _impl->bodies.push_back({});
    }

    Impl::BodySlot& slot = _impl->bodies[index];
    slot.body = _impl->world.CreateBody(&body_def);
    slot.user_id = def.user_id;
    slot.fixtures.clear();
    slot.body->GetUserData().pointer = static_cast<uintptr_t>(index) + 1;
    return {index, slot.generation};
}

void PhysicsWorld::destroy_body(PhysicsBody body) {
    if (!is_alive(body)) {
        return;
    }

    Impl::BodySlot& slot = _impl->bodies[body.index];
    for (u32 fixture_index : slot.fixtures) {
        if (fixture_index < _impl->fixtures.size() && _impl->fixtures[fixture_index].fixture) {
            PhysicsFixture fixture{fixture_index, _impl->fixtures[fixture_index].generation};
            _impl->erase_touching_with(fixture);
            _impl->fixtures[fixture_index].fixture = nullptr;
            _impl->fixtures[fixture_index].generation++;
            _impl->free_fixtures.push_back(fixture_index);
        }
    }
    slot.fixtures.clear();
    _impl->world.DestroyBody(slot.body);
    slot.body = nullptr;
    slot.user_id = 0;
    slot.generation++;
    _impl->free_bodies.push_back(body.index);
    _impl->rebuild_current_contacts();
}

bool PhysicsWorld::is_alive(PhysicsBody body) const {
    return _impl->alive(body);
}

PhysicsFixture PhysicsWorld::add_fixture(PhysicsBody body, const PhysicsFixtureDef& def) {
    b2Body* body_ptr = _impl->body_ptr(body);
    if (!body_ptr) {
        return {};
    }

    b2PolygonShape box_shape;
    b2CircleShape circle_shape;
    b2FixtureDef fixture_def;
    switch (def.shape.type) {
    case PhysicsShapeType::Box:
        box_shape.SetAsBox(def.shape.half_extents.x, def.shape.half_extents.y, to_b2(def.shape.offset), 0.0f);
        fixture_def.shape = &box_shape;
        break;
    case PhysicsShapeType::Circle:
        circle_shape.m_radius = def.shape.radius;
        circle_shape.m_p = to_b2(def.shape.offset);
        fixture_def.shape = &circle_shape;
        break;
    }

    fixture_def.density = def.density;
    fixture_def.friction = def.friction;
    fixture_def.restitution = def.restitution;
    fixture_def.isSensor = def.sensor;
    fixture_def.filter.categoryBits = def.category_bits;
    fixture_def.filter.maskBits = def.mask_bits;

    u32 index = 0;
    if (!_impl->free_fixtures.empty()) {
        index = _impl->free_fixtures.back();
        _impl->free_fixtures.pop_back();
    } else {
        index = static_cast<u32>(_impl->fixtures.size());
        _impl->fixtures.push_back({});
    }

    Impl::FixtureSlot& slot = _impl->fixtures[index];
    slot.fixture = body_ptr->CreateFixture(&fixture_def);
    slot.body = body;
    slot.tag = def.tag;
    slot.fixture->GetUserData().pointer = static_cast<uintptr_t>(index) + 1;
    _impl->bodies[body.index].fixtures.push_back(index);
    return {index, slot.generation};
}

void PhysicsWorld::destroy_fixture(PhysicsFixture fixture) {
    b2Fixture* fixture_ptr = _impl->fixture_ptr(fixture);
    if (!fixture_ptr) {
        return;
    }

    Impl::FixtureSlot& slot = _impl->fixtures[fixture.index];
    b2Body* body = fixture_ptr->GetBody();
    if (slot.body.index < _impl->bodies.size()) {
        auto& fixtures = _impl->bodies[slot.body.index].fixtures;
        fixtures.erase(std::remove(fixtures.begin(), fixtures.end(), fixture.index), fixtures.end());
    }

    _impl->erase_touching_with(fixture);
    body->DestroyFixture(fixture_ptr);
    slot.fixture = nullptr;
    slot.body = {};
    slot.tag = 0;
    slot.generation++;
    _impl->free_fixtures.push_back(fixture.index);
    _impl->rebuild_current_contacts();
}

bool PhysicsWorld::is_alive(PhysicsFixture fixture) const {
    return _impl->alive(fixture);
}

std::vector<PhysicsFixture> PhysicsWorld::fixtures(PhysicsBody body) const {
    std::vector<PhysicsFixture> result;
    if (!is_alive(body)) {
        return result;
    }

    const Impl::BodySlot& slot = _impl->bodies[body.index];
    result.reserve(slot.fixtures.size());
    for (u32 fixture_index : slot.fixtures) {
        PhysicsFixture fixture = _impl->fixture_handle(fixture_index);
        if (fixture) {
            result.push_back(fixture);
        }
    }
    return result;
}

u64 PhysicsWorld::user_id(PhysicsBody body) const {
    return is_alive(body) ? _impl->bodies[body.index].user_id : 0;
}

u64 PhysicsWorld::fixture_tag(PhysicsFixture fixture) const {
    return is_alive(fixture) ? _impl->fixtures[fixture.index].tag : 0;
}

void PhysicsWorld::set_transform(PhysicsBody body, Vec2f position, f32 rotation) {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        body_ptr->SetTransform(to_b2(position), rotation);
    }
}

Vec2f PhysicsWorld::position(PhysicsBody body) const {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        return from_b2(body_ptr->GetPosition());
    }
    return {};
}

f32 PhysicsWorld::rotation(PhysicsBody body) const {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        return body_ptr->GetAngle();
    }
    return 0.0f;
}

void PhysicsWorld::set_linear_velocity(PhysicsBody body, Vec2f velocity) {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        body_ptr->SetLinearVelocity(to_b2(velocity));
    }
}

Vec2f PhysicsWorld::linear_velocity(PhysicsBody body) const {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        return from_b2(body_ptr->GetLinearVelocity());
    }
    return {};
}

void PhysicsWorld::apply_force(PhysicsBody body, Vec2f force) {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        body_ptr->ApplyForceToCenter(to_b2(force), true);
    }
}

void PhysicsWorld::apply_impulse(PhysicsBody body, Vec2f impulse) {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        body_ptr->ApplyLinearImpulseToCenter(to_b2(impulse), true);
    }
}

void PhysicsWorld::step(f32 dt, i32 velocity_iterations, i32 position_iterations) {
    _impl->events.clear();
    _impl->world.Step(dt, velocity_iterations, position_iterations);
    _impl->rebuild_current_contacts();
}

std::span<const PhysicsContactEvent> PhysicsWorld::contacts() const {
    return _impl->events;
}

std::span<const PhysicsContact> PhysicsWorld::current_contacts() const {
    return _impl->current;
}

void PhysicsWorld::clear_contacts() {
    _impl->events.clear();
}

std::vector<PhysicsFixture> PhysicsWorld::query_point(Vec2f point, PhysicsFilter filter) const {
    struct Callback final : b2QueryCallback {
        const Impl& impl;
        Vec2f point{};
        PhysicsFilter filter{};
        std::vector<PhysicsFixture> results;

        Callback(const Impl& impl, Vec2f point, PhysicsFilter filter)
            : impl(impl),
              point(point),
              filter(filter) {
        }

        bool ReportFixture(b2Fixture* fixture) override {
            if (filter_matches(*fixture, filter) && fixture->TestPoint(to_b2(point))) {
                results.push_back(impl.handle_for(fixture));
            }
            return true;
        }
    };

    Callback callback{*_impl, point, filter};
    const b2AABB aabb{{point.x - 0.001f, point.y - 0.001f}, {point.x + 0.001f, point.y + 0.001f}};
    _impl->world.QueryAABB(&callback, aabb);
    return callback.results;
}

std::vector<PhysicsFixture> PhysicsWorld::query_aabb(Rectf rect, PhysicsFilter filter) const {
    struct Callback final : b2QueryCallback {
        const Impl& impl;
        PhysicsFilter filter{};
        std::vector<PhysicsFixture> results;

        Callback(const Impl& impl, PhysicsFilter filter)
            : impl(impl),
              filter(filter) {
        }

        bool ReportFixture(b2Fixture* fixture) override {
            if (filter_matches(*fixture, filter)) {
                results.push_back(impl.handle_for(fixture));
            }
            return true;
        }
    };

    Callback callback{*_impl, filter};
    const b2AABB aabb{{rect.x, rect.y}, {rect.x + rect.w, rect.y + rect.h}};
    _impl->world.QueryAABB(&callback, aabb);
    return callback.results;
}

std::vector<PhysicsRaycastHit> PhysicsWorld::raycast(Vec2f from, Vec2f to, PhysicsFilter filter) const {
    struct Callback final : b2RayCastCallback {
        const Impl& impl;
        PhysicsFilter filter{};
        std::vector<PhysicsRaycastHit> hits;

        Callback(const Impl& impl, PhysicsFilter filter)
            : impl(impl),
              filter(filter) {
        }

        float ReportFixture(b2Fixture* fixture, const b2Vec2& point, const b2Vec2& normal, float fraction) override {
            if (!filter_matches(*fixture, filter)) {
                return -1.0f;
            }
            hits.push_back({
                .body = impl.handle_for(fixture->GetBody()),
                .fixture = impl.handle_for(fixture),
                .point = from_b2(point),
                .normal = from_b2(normal),
                .fraction = fraction,
            });
            return 1.0f;
        }
    };

    Callback callback{*_impl, filter};
    _impl->world.RayCast(&callback, to_b2(from), to_b2(to));
    std::ranges::sort(callback.hits, [](const PhysicsRaycastHit& a, const PhysicsRaycastHit& b) {
        return a.fraction < b.fraction;
    });
    return callback.hits;
}

std::vector<PhysicsDebugShape> PhysicsWorld::debug_shapes(PhysicsFilter filter) const {
    std::vector<PhysicsDebugShape> shapes;
    for (const Impl::FixtureSlot& slot : _impl->fixtures) {
        b2Fixture* fixture = slot.fixture;
        if (!fixture || !filter_matches(*fixture, filter)) {
            continue;
        }

        b2Body* body = fixture->GetBody();
        PhysicsDebugShape debug{
            .body = _impl->handle_for(body),
            .fixture = _impl->handle_for(fixture),
            .body_type = from_b2(body->GetType()),
            .sensor = fixture->IsSensor(),
            .awake = body->IsAwake(),
        };

        const b2Shape* shape = fixture->GetShape();
        switch (shape->GetType()) {
        case b2Shape::e_polygon: {
            debug.type = PhysicsDebugShapeType::Polygon;
            const auto* polygon = static_cast<const b2PolygonShape*>(shape);
            debug.points.reserve(static_cast<std::size_t>(polygon->m_count));
            for (int32 i = 0; i < polygon->m_count; ++i) {
                debug.points.push_back(from_b2(body->GetWorldPoint(polygon->m_vertices[i])));
            }
            break;
        }
        case b2Shape::e_circle: {
            debug.type = PhysicsDebugShapeType::Circle;
            const auto* circle = static_cast<const b2CircleShape*>(shape);
            debug.center = from_b2(body->GetWorldPoint(circle->m_p));
            debug.radius = circle->m_radius;
            break;
        }
        default:
            continue;
        }

        shapes.push_back(std::move(debug));
    }
    return shapes;
}

} // namespace kin
