#include <kin/physics/physics.hpp>

#include <box2d/box2d.h>
#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_set>

namespace kin {
namespace {

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

u64 fixture_key(PhysicsFixture fixture) {
    return (static_cast<u64>(fixture.index) << 32) | fixture.generation;
}

f32 cross(b2Vec2 a, b2Vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// Twice the signed area.
f32 doubled_area(const std::vector<b2Vec2>& points) {
    f32 area = 0.0f;
    for (std::size_t i = 0; i < points.size(); ++i) {
        area += cross(points[i], points[(i + 1) % points.size()]);
    }
    return area;
}

bool convex(const std::vector<b2Vec2>& points) {
    const f32 sense = doubled_area(points);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const b2Vec2 a = points[i];
        const b2Vec2 b = points[(i + 1) % points.size()];
        const b2Vec2 c = points[(i + 2) % points.size()];
        if (cross(b - a, c - b) * sense < 0.0f) {
            return false;
        }
    }
    return true;
}

// A Box2D polygon of `points` if they make one: not so small that Box2D's
// welding of near corners leaves it no area (Box2D asserts then).
bool set_polygon(b2PolygonShape& shape, const std::vector<b2Vec2>& points) {
    if (points.size() < 3 || points.size() > b2_maxPolygonVertices) {
        return false;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (std::size_t j = i + 1; j < points.size(); ++j) {
            if (b2DistanceSquared(points[i], points[j]) < b2_linearSlop * b2_linearSlop) {
                return false;
            }
        }
    }
    if (std::abs(doubled_area(points)) < 2.0f * b2_linearSlop * b2_linearSlop) {
        return false;
    }
    shape.Set(points.data(), static_cast<int32>(points.size()));
    return true;
}

// `points` without those too near the one before (and, closed, the last
// when it is the first again).
std::vector<b2Vec2> distinct(const std::vector<b2Vec2>& points, bool closed) {
    std::vector<b2Vec2> kept;
    kept.reserve(points.size());
    for (const b2Vec2& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            continue;
        }
        if (kept.empty() || b2DistanceSquared(kept.back(), point) > b2_linearSlop * b2_linearSlop) {
            kept.push_back(point);
        }
    }
    while (closed && kept.size() > 1 && b2DistanceSquared(kept.front(), kept.back()) <= b2_linearSlop * b2_linearSlop) {
        kept.pop_back();
    }
    return kept;
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

PhysicsShape PhysicsShape::capsule(Vec2f half_extents, Vec2f offset) {
    PhysicsShape shape;
    shape.type = PhysicsShapeType::Capsule;
    shape.half_extents = half_extents;
    shape.offset = offset;
    return shape;
}

PhysicsShape PhysicsShape::polygon(std::vector<Vec2f> points, Vec2f offset) {
    PhysicsShape shape;
    shape.type = PhysicsShapeType::Polygon;
    shape.points = std::move(points);
    shape.offset = offset;
    return shape;
}

PhysicsShape PhysicsShape::chain(std::vector<Vec2f> points, bool loop, Vec2f offset) {
    PhysicsShape shape;
    shape.type = PhysicsShapeType::Chain;
    shape.points = std::move(points);
    shape.loop = loop;
    shape.offset = offset;
    return shape;
}

struct PhysicsWorld::Impl : b2ContactListener, b2DestructionListener {
    struct BodySlot {
        b2Body* body = nullptr;
        u32 generation = 1;
        u64 user_id = 0;
        std::vector<u32> fixtures;
    };

    struct FixtureSlot {
        bool alive = false;
        std::vector<b2Fixture*> parts{}; // the Box2D shapes it is made of
        PhysicsBody body{};
        u32 generation = 1;
        u64 tag = 0;
        bool sensor = false;
    };

    struct JointSlot {
        b2Joint* joint = nullptr;
        PhysicsJointType type = PhysicsJointType::Hinge;
        u32 generation = 1;
        u64 user_id = 0;
    };

    explicit Impl(PhysicsWorldDef def)
        : scale(std::isfinite(def.units_per_meter) && def.units_per_meter > 0.0f ? def.units_per_meter : 1.0f),
          world(meters(def.gravity)) {
        world.SetContactListener(this);
        world.SetDestructionListener(this);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // The game's units to Box2D's metres, and back (lengths, positions,
    // velocities, forces: each has one length in it).
    b2Vec2 meters(Vec2f value) const {
        return {value.x / scale, value.y / scale};
    }

    f32 meters(f32 length) const {
        return length / scale;
    }

    Vec2f units(const b2Vec2& value) const {
        return {value.x * scale, value.y * scale};
    }

    f32 units(f32 length) const {
        return length * scale;
    }

    // A torque's units (a force's times a length) in Box2D's.
    f32 torque(f32 value) const {
        return value / (scale * scale);
    }

    f32 scale = 1.0f; // units per metre
    b2World world;
    b2Body* ground = nullptr; // what a joint to no body holds to
    std::vector<BodySlot> bodies;
    std::vector<u32> free_bodies;
    std::vector<FixtureSlot> fixtures;
    std::vector<u32> free_fixtures;
    std::vector<JointSlot> joints;
    std::vector<u32> free_joints;
    std::vector<PhysicsContactEvent> events;
    // For each pair of fixtures touching: how many of their shapes' contacts touch.
    std::map<ContactKey, u32> touching;
    std::vector<PhysicsContact> current;
    f32 last_dt = 0.0f;

    PhysicsBody body_handle(u32 index) const {
        if (index >= bodies.size() || !bodies[index].body) {
            return {};
        }
        return {index, bodies[index].generation};
    }

    PhysicsFixture fixture_handle(u32 index) const {
        if (index >= fixtures.size() || !fixtures[index].alive) {
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

    const FixtureSlot* fixture_slot(PhysicsFixture handle) const {
        if (handle.index >= fixtures.size()) {
            return nullptr;
        }
        const FixtureSlot& slot = fixtures[handle.index];
        return slot.alive && slot.generation == handle.generation ? &slot : nullptr;
    }

    b2Joint* joint_ptr(PhysicsJoint handle) const {
        if (handle.index >= joints.size()) {
            return nullptr;
        }
        const JointSlot& slot = joints[handle.index];
        return slot.generation == handle.generation ? slot.joint : nullptr;
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

    b2Body* ground_body() {
        if (!ground) {
            b2BodyDef def;
            def.type = b2_staticBody;
            ground = world.CreateBody(&def);
        }
        return ground;
    }

    void free_fixture(u32 index) {
        FixtureSlot& slot = fixtures[index];
        const u32 generation = slot.generation + 1;
        slot = FixtureSlot{};
        slot.generation = generation;
        free_fixtures.push_back(index);
    }

    void free_joint(u32 index) {
        JointSlot& slot = joints[index];
        slot.joint = nullptr;
        slot.user_id = 0;
        slot.generation++;
        free_joints.push_back(index);
    }

    void erase_touching_with(PhysicsFixture fixture) {
        for (auto it = touching.begin(); it != touching.end();) {
            if (it->first.a == fixture || it->first.b == fixture) {
                it = touching.erase(it);
            } else {
                ++it;
            }
        }
    }

    void rebuild_current_contacts() {
        current.clear();
        current.reserve(touching.size());
        for (const auto& [key, count] : touching) {
            const FixtureSlot* a = fixture_slot(key.a);
            const FixtureSlot* b = fixture_slot(key.b);
            if (!a || !b) {
                continue;
            }
            current.push_back({
                .body_a = a->body,
                .body_b = b->body,
                .fixture_a = key.a,
                .fixture_b = key.b,
                .sensor = a->sensor || b->sensor,
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
            event.point = units(manifold.points[0]);
            event.normal = from_b2(manifold.normal);
        }
        events.push_back(event);
    }

    // Two fixtures' contact begins with the first of their shapes' touching,
    // and ends with the last's.
    void BeginContact(b2Contact* contact) override {
        const PhysicsFixture fixture_a = handle_for(contact->GetFixtureA());
        const PhysicsFixture fixture_b = handle_for(contact->GetFixtureB());
        if (fixture_a && fixture_b && ++touching[make_key(fixture_a, fixture_b)] > 1) {
            return;
        }
        push_event(PhysicsContactEventType::Begin, contact);
    }

    void EndContact(b2Contact* contact) override {
        const PhysicsFixture fixture_a = handle_for(contact->GetFixtureA());
        const PhysicsFixture fixture_b = handle_for(contact->GetFixtureB());
        if (fixture_a && fixture_b) {
            const auto found = touching.find(make_key(fixture_a, fixture_b));
            if (found == touching.end() || --found->second > 0) {
                return;
            }
            touching.erase(found);
        }
        push_event(PhysicsContactEventType::End, contact);
    }

    // A joint goes with its body (Box2D destroys it).
    void SayGoodbye(b2Joint* joint) override {
        const uintptr_t value = joint->GetUserData().pointer;
        if (value != 0 && value <= joints.size() && joints[value - 1].joint == joint) {
            free_joint(static_cast<u32>(value - 1));
        }
    }

    void SayGoodbye(b2Fixture*) override {
        // Fixtures go with their bodies in destroy_body.
    }

    // `p`, a point of a shape, on its body (in metres): from the shape's
    // offset, turned by its angle.
    b2Vec2 placed(const PhysicsShape& shape, Vec2f p) const {
        const f32 c = std::cos(shape.angle);
        const f32 s = std::sin(shape.angle);
        return meters(Vec2f{shape.offset.x + c * p.x - s * p.y, shape.offset.y + s * p.x + c * p.y});
    }

    std::vector<b2Vec2> placed_points(const PhysicsShape& shape) const {
        std::vector<b2Vec2> points;
        points.reserve(shape.points.size());
        for (Vec2f point : shape.points) {
            points.push_back(placed(shape, point));
        }
        return points;
    }

    // Calls make(b2Shape, density scale) for each Box2D shape `shape` takes.
    template <typename Make>
    void each_part(const PhysicsShape& shape, Make&& make) const {
        switch (shape.type) {
        case PhysicsShapeType::Box: {
            const f32 hx = meters(std::abs(shape.half_extents.x));
            const f32 hy = meters(std::abs(shape.half_extents.y));
            if (!(hx > 0.0f) || !(hy > 0.0f)) {
                return;
            }
            b2PolygonShape box;
            box.SetAsBox(hx, hy, meters(shape.offset), shape.angle);
            make(box, 1.0f);
            return;
        }
        case PhysicsShapeType::Circle: {
            if (!(shape.radius > 0.0f)) {
                return;
            }
            b2CircleShape circle;
            circle.m_radius = meters(shape.radius);
            circle.m_p = meters(shape.offset);
            make(circle, 1.0f);
            return;
        }
        case PhysicsShapeType::Capsule: {
            // A box between two circles, the circles half as dense: the box
            // has half of each, so the capsule weighs what it should.
            const f32 hx = std::abs(shape.half_extents.x);
            const f32 hy = std::abs(shape.half_extents.y);
            const bool across = hx >= hy; // round at its left and right
            const f32 radius = across ? hy : hx;
            const f32 reach = (across ? hx : hy) - radius; // from its centre to its circles'
            if (!(radius > 0.0f)) {
                return;
            }
            b2CircleShape cap;
            cap.m_radius = meters(radius);
            if (!(meters(reach) > b2_linearSlop)) {
                cap.m_p = meters(shape.offset); // as long as it is wide: a circle
                make(cap, 1.0f);
                return;
            }
            b2PolygonShape middle;
            middle.SetAsBox(meters(across ? reach : radius), meters(across ? radius : reach), meters(shape.offset), shape.angle);
            make(middle, 1.0f);
            const Vec2f end = across ? Vec2f{reach, 0.0f} : Vec2f{0.0f, reach};
            cap.m_p = placed(shape, end);
            make(cap, 0.5f);
            cap.m_p = placed(shape, {-end.x, -end.y});
            make(cap, 0.5f);
            return;
        }
        case PhysicsShapeType::Polygon: {
            const std::vector<b2Vec2> points = distinct(placed_points(shape), true);
            if (points.size() < 3) {
                return;
            }
            b2PolygonShape polygon;
            if (points.size() <= b2_maxPolygonVertices && convex(points)) {
                if (set_polygon(polygon, points)) {
                    make(polygon, 1.0f);
                }
                return;
            }
            // Cut in triangles, each one Box2D polygon.
            std::vector<std::vector<std::array<f64, 2>>> rings(1);
            rings[0].reserve(points.size());
            for (const b2Vec2& point : points) {
                rings[0].push_back({point.x, point.y});
            }
            const std::vector<u32> corners = mapbox::earcut<u32>(rings);
            for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
                const std::vector<b2Vec2> triangle{points[corners[i]], points[corners[i + 1]], points[corners[i + 2]]};
                if (set_polygon(polygon, triangle)) {
                    make(polygon, 1.0f);
                }
            }
            return;
        }
        case PhysicsShapeType::Chain: {
            const std::vector<b2Vec2> points = distinct(placed_points(shape), shape.loop);
            const bool loop = shape.loop && points.size() >= 3;
            if (points.size() < 2) {
                return;
            }
            if (shape.one_sided) {
                b2ChainShape chain;
                if (loop) {
                    chain.CreateLoop(points.data(), static_cast<int32>(points.size()));
                } else {
                    // Its ends carry on as they go (for what slides off them).
                    const b2Vec2 before = points[0] + (points[0] - points[1]);
                    const b2Vec2 after = points.back() + (points.back() - points[points.size() - 2]);
                    chain.CreateChain(points.data(), static_cast<int32>(points.size()), before, after);
                }
                make(chain, 1.0f);
                return;
            }
            const std::size_t segments = loop ? points.size() : points.size() - 1;
            for (std::size_t i = 0; i < segments; ++i) {
                b2EdgeShape edge;
                edge.SetTwoSided(points[i], points[(i + 1) % points.size()]);
                make(edge, 1.0f);
            }
            return;
        }
        }
    }

    // Box2D's spring for a joint between `a` and `b` (none: frequency 0).
    static std::pair<f32, f32> spring(const PhysicsJointDef& def, const b2Body* a, const b2Body* b, bool angular) {
        f32 stiffness = 0.0f;
        f32 damping = 0.0f;
        if (def.frequency > 0.0f) {
            if (angular) {
                b2AngularStiffness(stiffness, damping, def.frequency, def.damping_ratio, a, b);
            } else {
                b2LinearStiffness(stiffness, damping, def.frequency, def.damping_ratio, a, b);
            }
        }
        return {stiffness, damping};
    }

    void apply(b2Joint* joint, PhysicsJointType type, const PhysicsJointDef& def) const {
        const f32 lower = std::min(def.lower, def.upper);
        const f32 upper = std::max(def.lower, def.upper);
        switch (type) {
        case PhysicsJointType::Hinge: {
            auto* hinge = static_cast<b2RevoluteJoint*>(joint);
            hinge->EnableLimit(def.limit);
            hinge->SetLimits(lower, upper);
            hinge->EnableMotor(def.motor);
            hinge->SetMotorSpeed(def.motor_speed);
            hinge->SetMaxMotorTorque(torque(std::abs(def.max_motor_force)));
            break;
        }
        case PhysicsJointType::Slider: {
            auto* slider = static_cast<b2PrismaticJoint*>(joint);
            slider->EnableLimit(def.limit);
            slider->SetLimits(meters(lower), meters(upper));
            slider->EnableMotor(def.motor);
            slider->SetMotorSpeed(meters(def.motor_speed));
            slider->SetMaxMotorForce(meters(std::abs(def.max_motor_force)));
            break;
        }
        case PhysicsJointType::Distance: {
            auto* distance = static_cast<b2DistanceJoint*>(joint);
            if (def.length > 0.0f) {
                distance->SetLength(meters(def.length));
            }
            if (def.limit) {
                distance->SetMinLength(meters(lower));
                distance->SetMaxLength(meters(upper));
            } else if (def.frequency > 0.0f) {
                distance->SetMinLength(0.0f);
                distance->SetMaxLength(b2_maxFloat);
            } else {
                distance->SetMinLength(distance->GetLength());
                distance->SetMaxLength(distance->GetLength());
            }
            const auto [stiffness, damping] = spring(def, joint->GetBodyA(), joint->GetBodyB(), false);
            distance->SetStiffness(stiffness);
            distance->SetDamping(damping);
            break;
        }
        case PhysicsJointType::Weld: {
            auto* weld = static_cast<b2WeldJoint*>(joint);
            const auto [stiffness, damping] = spring(def, joint->GetBodyA(), joint->GetBodyB(), true);
            weld->SetStiffness(stiffness);
            weld->SetDamping(damping);
            break;
        }
        case PhysicsJointType::Wheel: {
            auto* wheel = static_cast<b2WheelJoint*>(joint);
            wheel->EnableLimit(def.limit);
            wheel->SetLimits(meters(lower), meters(upper));
            wheel->EnableMotor(def.motor);
            wheel->SetMotorSpeed(def.motor_speed);
            wheel->SetMaxMotorTorque(torque(std::abs(def.max_motor_force)));
            const auto [stiffness, damping] = spring(def, joint->GetBodyA(), joint->GetBodyB(), false);
            wheel->SetStiffness(stiffness);
            wheel->SetDamping(damping);
            break;
        }
        }
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
    body_def.position = _impl->meters(def.position);
    body_def.angle = def.rotation;
    body_def.linearVelocity = _impl->meters(def.linear_velocity);
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

    // Box2D ends the body's contacts (EndContact, its fixtures still known)
    // and destroys its joints (SayGoodbye).
    Impl::BodySlot& slot = _impl->bodies[body.index];
    _impl->world.DestroyBody(slot.body);
    for (u32 fixture_index : slot.fixtures) {
        if (const PhysicsFixture fixture = _impl->fixture_handle(fixture_index)) {
            _impl->erase_touching_with(fixture);
            _impl->free_fixture(fixture_index);
        }
    }
    slot.fixtures.clear();
    slot.body = nullptr;
    slot.user_id = 0;
    slot.generation++;
    _impl->free_bodies.push_back(body.index);
    _impl->rebuild_current_contacts();
}

bool PhysicsWorld::is_alive(PhysicsBody body) const {
    return _impl->body_ptr(body) != nullptr;
}

PhysicsFixture PhysicsWorld::add_fixture(PhysicsBody body, const PhysicsFixtureDef& def) {
    b2Body* body_ptr = _impl->body_ptr(body);
    if (!body_ptr) {
        return {};
    }

    u32 index = 0;
    if (!_impl->free_fixtures.empty()) {
        index = _impl->free_fixtures.back();
        _impl->free_fixtures.pop_back();
    } else {
        index = static_cast<u32>(_impl->fixtures.size());
        _impl->fixtures.push_back({});
    }

    {
        Impl::FixtureSlot& slot = _impl->fixtures[index];
        slot.alive = true;
        slot.parts.clear();
        slot.body = body;
        slot.tag = def.tag;
        slot.sensor = def.sensor;
    }
    _impl->each_part(def.shape, [&](const b2Shape& shape, f32 density) {
        b2FixtureDef fixture_def;
        fixture_def.shape = &shape;
        fixture_def.density = def.density * density;
        fixture_def.friction = def.friction;
        fixture_def.restitution = def.restitution;
        fixture_def.isSensor = def.sensor;
        fixture_def.filter.categoryBits = def.category_bits;
        fixture_def.filter.maskBits = def.mask_bits;
        b2Fixture* part = body_ptr->CreateFixture(&fixture_def);
        part->GetUserData().pointer = static_cast<uintptr_t>(index) + 1;
        _impl->fixtures[index].parts.push_back(part);
    });
    _impl->bodies[body.index].fixtures.push_back(index);
    return {index, _impl->fixtures[index].generation};
}

void PhysicsWorld::destroy_fixture(PhysicsFixture fixture) {
    const Impl::FixtureSlot* slot = _impl->fixture_slot(fixture);
    if (!slot) {
        return;
    }

    const PhysicsBody owner = slot->body;
    if (b2Body* body = _impl->body_ptr(owner)) {
        const std::vector<b2Fixture*> parts = slot->parts;
        for (b2Fixture* part : parts) {
            body->DestroyFixture(part); // ends its contacts (EndContact)
        }
        auto& fixtures = _impl->bodies[owner.index].fixtures;
        fixtures.erase(std::remove(fixtures.begin(), fixtures.end(), fixture.index), fixtures.end());
    }

    _impl->erase_touching_with(fixture);
    _impl->free_fixture(fixture.index);
    _impl->rebuild_current_contacts();
}

bool PhysicsWorld::is_alive(PhysicsFixture fixture) const {
    return _impl->fixture_slot(fixture) != nullptr;
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

u32 PhysicsWorld::shape_parts(PhysicsFixture fixture) const {
    const Impl::FixtureSlot* slot = _impl->fixture_slot(fixture);
    return slot ? static_cast<u32>(slot->parts.size()) : 0;
}

PhysicsJoint PhysicsWorld::create_joint(const PhysicsJointDef& def) {
    b2Body* a = _impl->body_ptr(def.body_a);
    if (!a || (def.body_b.valid() && !_impl->body_ptr(def.body_b))) {
        return {};
    }
    b2Body* b = def.body_b.valid() ? _impl->body_ptr(def.body_b) : _impl->ground_body();
    if (a == b) {
        return {};
    }
    // Box2D measures its second body from its first: body_a goes second, so
    // a joint's angle, travel and motor are body_a's from body_b.
    std::swap(a, b);
    const b2Vec2 anchor = _impl->meters(def.anchor);
    b2Vec2 axis(def.axis.x, def.axis.y);
    if (!(axis.Normalize() > b2_epsilon)) {
        axis = b2Vec2(1.0f, 0.0f);
    }

    b2Joint* joint = nullptr;
    const auto create = [&](auto& joint_def) {
        joint_def.collideConnected = def.collide_connected;
        joint = _impl->world.CreateJoint(&joint_def);
    };
    switch (def.type) {
    case PhysicsJointType::Hinge: {
        b2RevoluteJointDef joint_def;
        joint_def.Initialize(a, b, anchor);
        create(joint_def);
        break;
    }
    case PhysicsJointType::Slider: {
        b2PrismaticJointDef joint_def;
        joint_def.Initialize(a, b, anchor, axis);
        create(joint_def);
        break;
    }
    case PhysicsJointType::Distance: {
        b2DistanceJointDef joint_def;
        joint_def.Initialize(a, b, _impl->meters(def.other_anchor), anchor);
        create(joint_def);
        break;
    }
    case PhysicsJointType::Weld: {
        b2WeldJointDef joint_def;
        joint_def.Initialize(a, b, anchor);
        create(joint_def);
        break;
    }
    case PhysicsJointType::Wheel: {
        b2WheelJointDef joint_def;
        joint_def.Initialize(a, b, anchor, axis);
        create(joint_def);
        break;
    }
    }
    if (!joint) {
        return {};
    }
    _impl->apply(joint, def.type, def);

    u32 index = 0;
    if (!_impl->free_joints.empty()) {
        index = _impl->free_joints.back();
        _impl->free_joints.pop_back();
    } else {
        index = static_cast<u32>(_impl->joints.size());
        _impl->joints.push_back({});
    }
    Impl::JointSlot& slot = _impl->joints[index];
    slot.joint = joint;
    slot.type = def.type;
    slot.user_id = def.user_id;
    joint->GetUserData().pointer = static_cast<uintptr_t>(index) + 1;
    return {index, slot.generation};
}

void PhysicsWorld::destroy_joint(PhysicsJoint joint) {
    if (b2Joint* joint_ptr = _impl->joint_ptr(joint)) {
        _impl->world.DestroyJoint(joint_ptr);
        _impl->free_joint(joint.index);
    }
}

bool PhysicsWorld::is_alive(PhysicsJoint joint) const {
    return _impl->joint_ptr(joint) != nullptr;
}

void PhysicsWorld::update_joint(PhysicsJoint joint, const PhysicsJointDef& def) {
    if (b2Joint* joint_ptr = _impl->joint_ptr(joint)) {
        _impl->apply(joint_ptr, _impl->joints[joint.index].type, def);
        joint_ptr->GetBodyA()->SetAwake(true);
        joint_ptr->GetBodyB()->SetAwake(true);
    }
}

Vec2f PhysicsWorld::joint_force(PhysicsJoint joint) const {
    b2Joint* joint_ptr = _impl->joint_ptr(joint);
    if (!joint_ptr || !(_impl->last_dt > 0.0f)) {
        return {};
    }
    return _impl->units(joint_ptr->GetReactionForce(1.0f / _impl->last_dt));
}

f32 PhysicsWorld::joint_value(PhysicsJoint joint) const {
    b2Joint* joint_ptr = _impl->joint_ptr(joint);
    if (!joint_ptr) {
        return 0.0f;
    }
    switch (_impl->joints[joint.index].type) {
    case PhysicsJointType::Hinge: return static_cast<b2RevoluteJoint*>(joint_ptr)->GetJointAngle();
    case PhysicsJointType::Slider: return _impl->units(static_cast<b2PrismaticJoint*>(joint_ptr)->GetJointTranslation());
    case PhysicsJointType::Distance: return _impl->units(static_cast<b2DistanceJoint*>(joint_ptr)->GetCurrentLength());
    case PhysicsJointType::Weld: return 0.0f;
    case PhysicsJointType::Wheel: return _impl->units(static_cast<b2WheelJoint*>(joint_ptr)->GetJointTranslation());
    }
    return 0.0f;
}

u64 PhysicsWorld::joint_user_id(PhysicsJoint joint) const {
    return is_alive(joint) ? _impl->joints[joint.index].user_id : 0;
}

u64 PhysicsWorld::user_id(PhysicsBody body) const {
    return is_alive(body) ? _impl->bodies[body.index].user_id : 0;
}

u64 PhysicsWorld::fixture_tag(PhysicsFixture fixture) const {
    const Impl::FixtureSlot* slot = _impl->fixture_slot(fixture);
    return slot ? slot->tag : 0;
}

void PhysicsWorld::set_transform(PhysicsBody body, Vec2f position, f32 rotation) {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        body_ptr->SetTransform(_impl->meters(position), rotation);
    }
}

Vec2f PhysicsWorld::position(PhysicsBody body) const {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        return _impl->units(body_ptr->GetPosition());
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
        body_ptr->SetLinearVelocity(_impl->meters(velocity));
    }
}

Vec2f PhysicsWorld::linear_velocity(PhysicsBody body) const {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        return _impl->units(body_ptr->GetLinearVelocity());
    }
    return {};
}

void PhysicsWorld::apply_force(PhysicsBody body, Vec2f force) {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        body_ptr->ApplyForceToCenter(_impl->meters(force), true);
    }
}

void PhysicsWorld::apply_impulse(PhysicsBody body, Vec2f impulse) {
    if (b2Body* body_ptr = _impl->body_ptr(body)) {
        body_ptr->ApplyLinearImpulseToCenter(_impl->meters(impulse), true);
    }
}

void PhysicsWorld::step(f32 dt, i32 velocity_iterations, i32 position_iterations) {
    _impl->events.clear();
    _impl->last_dt = dt;
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

// Queries find fixtures once each, however many of their shapes they find.
std::vector<PhysicsFixture> PhysicsWorld::query_point(Vec2f point, PhysicsFilter filter) const {
    struct Callback final : b2QueryCallback {
        const Impl& impl;
        Vec2f point{};
        PhysicsFilter filter{};
        std::vector<PhysicsFixture> results;
        std::unordered_set<u64> seen;

        Callback(const Impl& impl, Vec2f point, PhysicsFilter filter)
            : impl(impl),
              point(point),
              filter(filter) {
        }

        bool ReportFixture(b2Fixture* fixture) override {
            if (filter_matches(*fixture, filter) && fixture->TestPoint(impl.meters(point))) {
                const PhysicsFixture found = impl.handle_for(fixture);
                if (found && seen.insert(fixture_key(found)).second) {
                    results.push_back(found);
                }
            }
            return true;
        }
    };

    Callback callback{*_impl, point, filter};
    const b2Vec2 at = _impl->meters(point);
    const b2AABB aabb{{at.x - 0.001f, at.y - 0.001f}, {at.x + 0.001f, at.y + 0.001f}};
    _impl->world.QueryAABB(&callback, aabb);
    return callback.results;
}

std::vector<PhysicsFixture> PhysicsWorld::query_aabb(Rectf rect, PhysicsFilter filter) const {
    struct Callback final : b2QueryCallback {
        const Impl& impl;
        PhysicsFilter filter{};
        std::vector<PhysicsFixture> results;
        std::unordered_set<u64> seen;

        Callback(const Impl& impl, PhysicsFilter filter)
            : impl(impl),
              filter(filter) {
        }

        bool ReportFixture(b2Fixture* fixture) override {
            if (filter_matches(*fixture, filter)) {
                const PhysicsFixture found = impl.handle_for(fixture);
                if (found && seen.insert(fixture_key(found)).second) {
                    results.push_back(found);
                }
            }
            return true;
        }
    };

    Callback callback{*_impl, filter};
    const b2AABB aabb{_impl->meters(Vec2f{rect.x, rect.y}), _impl->meters(Vec2f{rect.x + rect.w, rect.y + rect.h})};
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
                .point = impl.units(point),
                .normal = from_b2(normal),
                .fraction = fraction,
            });
            return 1.0f;
        }
    };

    Callback callback{*_impl, filter};
    _impl->world.RayCast(&callback, _impl->meters(from), _impl->meters(to));
    std::ranges::sort(callback.hits, [](const PhysicsRaycastHit& a, const PhysicsRaycastHit& b) {
        return a.fraction < b.fraction;
    });
    // Each fixture's nearest hit (its shapes may each be hit).
    std::vector<PhysicsRaycastHit> nearest;
    std::unordered_set<u64> seen;
    nearest.reserve(callback.hits.size());
    for (const PhysicsRaycastHit& hit : callback.hits) {
        if (seen.insert(fixture_key(hit.fixture)).second) {
            nearest.push_back(hit);
        }
    }
    return nearest;
}

std::vector<PhysicsDebugShape> PhysicsWorld::debug_shapes(PhysicsFilter filter) const {
    std::vector<PhysicsDebugShape> shapes;
    for (const Impl::FixtureSlot& slot : _impl->fixtures) {
        if (!slot.alive) {
            continue;
        }
        for (b2Fixture* fixture : slot.parts) {
            if (!filter_matches(*fixture, filter)) {
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
                    debug.points.push_back(_impl->units(body->GetWorldPoint(polygon->m_vertices[i])));
                }
                break;
            }
            case b2Shape::e_circle: {
                debug.type = PhysicsDebugShapeType::Circle;
                const auto* circle = static_cast<const b2CircleShape*>(shape);
                debug.center = _impl->units(body->GetWorldPoint(circle->m_p));
                debug.radius = _impl->units(circle->m_radius);
                break;
            }
            case b2Shape::e_edge: {
                debug.type = PhysicsDebugShapeType::Chain;
                const auto* edge = static_cast<const b2EdgeShape*>(shape);
                debug.points = {_impl->units(body->GetWorldPoint(edge->m_vertex1)), _impl->units(body->GetWorldPoint(edge->m_vertex2))};
                break;
            }
            case b2Shape::e_chain: {
                debug.type = PhysicsDebugShapeType::Chain; // a loop's last point is its first again
                const auto* chain = static_cast<const b2ChainShape*>(shape);
                debug.points.reserve(static_cast<std::size_t>(chain->m_count));
                for (int32 i = 0; i < chain->m_count; ++i) {
                    debug.points.push_back(_impl->units(body->GetWorldPoint(chain->m_vertices[i])));
                }
                break;
            }
            default:
                continue;
            }

            shapes.push_back(std::move(debug));
        }
    }
    return shapes;
}

std::vector<PhysicsDebugJoint> PhysicsWorld::debug_joints() const {
    std::vector<PhysicsDebugJoint> joints;
    for (u32 i = 0; i < _impl->joints.size(); ++i) {
        const Impl::JointSlot& slot = _impl->joints[i];
        if (slot.joint) {
            joints.push_back({
                .joint = {i, slot.generation},
                .type = slot.type,
                .anchor_a = _impl->units(slot.joint->GetAnchorB()), // body_a is Box2D's second
                .anchor_b = _impl->units(slot.joint->GetAnchorA()),
            });
        }
    }
    return joints;
}

f32 PhysicsWorld::units_per_meter() const {
    return _impl->scale;
}

} // namespace kin
