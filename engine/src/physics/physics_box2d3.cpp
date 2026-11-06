// kin's 2D physics (kin/physics/physics.hpp) on Box2D 3.2 (KIN_PHYSICS_BOX2D_V3,
// experimental): the same PhysicsWorld as physics.cpp's Box2D 2.4 one, the
// handles, fixtures of several shapes, contacts, queries, casts and the
// character mover as they are there. A step is solved on Box2D's own worker
// threads (PhysicsWorldDef::workers), and comes out the same however many.
// What differs, Box2D's way:
// - Sensors see what their filter lets them (every shape takes part in
//   sensor events), but not other sensors.
// - A contact a one-sided shape lets through, or a character's with what does
//   not move by itself, never touches: no begin or end is told for it (2.4
//   told the begin before it disabled the contact).
// - A capsule is one shape (2.4 made it of three).
// - Polygons have no skin round them (2.4's were 0.01 m bigger).

#include <kin/physics/physics.hpp>

#include <box2d/box2d.h>
#include <box2d/collision.h>
#include <box2d/math_functions.h>
#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace kin {
namespace {

constexpr f32 linear_slop = 0.005f;     // Box2D's, in metres
constexpr f32 polygon_radius = 0.01f;   // the rounding a mover's box takes at least (2.4's polygon skin)
constexpr f32 max_float = std::numeric_limits<f32>::max();

Vec2f from_b2(b2Vec2 value) {
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
    default: return PhysicsBodyType::Static;
    }
}

struct ContactKey {
    PhysicsFixture a{};
    PhysicsFixture b{};

    friend bool operator<(const ContactKey& lhs, const ContactKey& rhs) {
        if (lhs.a.index != rhs.a.index) return lhs.a.index < rhs.a.index;
        if (lhs.a.generation != rhs.a.generation) return lhs.a.generation < rhs.a.generation;
        if (lhs.b.index != rhs.b.index) return lhs.b.index < rhs.b.index;
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

u64 shape_key(b2ShapeId id) {
    return b2StoreShapeId(id);
}

u64 pair_key(b2ShapeId a, b2ShapeId b) {
    const u64 x = shape_key(a);
    const u64 y = shape_key(b);
    return x < y ? (x * 0x9E3779B97F4A7C15ull) ^ y : (y * 0x9E3779B97F4A7C15ull) ^ x;
}

f32 doubled_area(const std::vector<b2Vec2>& points) {
    f32 area = 0.0f;
    for (std::size_t i = 0; i < points.size(); ++i) {
        area += b2Cross(points[i], points[(i + 1) % points.size()]);
    }
    return area;
}

bool convex(const std::vector<b2Vec2>& points) {
    const f32 sense = doubled_area(points);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const b2Vec2 a = points[i];
        const b2Vec2 b = points[(i + 1) % points.size()];
        const b2Vec2 c = points[(i + 2) % points.size()];
        if (b2Cross(b - a, c - b) * sense < 0.0f) {
            return false;
        }
    }
    return true;
}

// A Box2D polygon of `points` if they make one (its hull: not so small that
// Box2D welds its corners away).
std::optional<b2Polygon> make_polygon(const std::vector<b2Vec2>& points) {
    if (points.size() < 3 || points.size() > B2_MAX_POLYGON_VERTICES) {
        return std::nullopt;
    }
    if (std::abs(doubled_area(points)) < 2.0f * linear_slop * linear_slop) {
        return std::nullopt;
    }
    const b2Hull hull = b2ComputeHull(points.data(), static_cast<int>(points.size()));
    if (hull.count < 3) {
        return std::nullopt;
    }
    return b2MakePolygon(&hull, 0.0f);
}

std::vector<b2Vec2> distinct(const std::vector<b2Vec2>& points, bool closed) {
    std::vector<b2Vec2> kept;
    kept.reserve(points.size());
    for (const b2Vec2& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            continue;
        }
        if (kept.empty() || b2DistanceSquared(kept.back(), point) > linear_slop * linear_slop) {
            kept.push_back(point);
        }
    }
    while (closed && kept.size() > 1 && b2DistanceSquared(kept.front(), kept.back()) <= linear_slop * linear_slop) {
        kept.pop_back();
    }
    return kept;
}

b2Transform transform_at(b2Vec2 at) {
    return {at, b2Rot_identity};
}

// A shape as the distance tests take it (from its body's origin).
b2ShapeProxy proxy_of(b2ShapeId shape) {
    switch (b2Shape_GetType(shape)) {
    case b2_circleShape: {
        const b2Circle circle = b2Shape_GetCircle(shape);
        return b2MakeProxy(&circle.center, 1, circle.radius);
    }
    case b2_capsuleShape: {
        const b2Capsule capsule = b2Shape_GetCapsule(shape);
        const b2Vec2 points[2] = {capsule.center1, capsule.center2};
        return b2MakeProxy(points, 2, capsule.radius);
    }
    case b2_polygonShape: {
        const b2Polygon polygon = b2Shape_GetPolygon(shape);
        return b2MakeProxy(polygon.vertices, polygon.count, polygon.radius);
    }
    case b2_segmentShape: {
        const b2Segment segment = b2Shape_GetSegment(shape);
        const b2Vec2 points[2] = {segment.point1, segment.point2};
        return b2MakeProxy(points, 2, 0.0f);
    }
    case b2_chainSegmentShape: {
        const b2ChainSegment chain = b2Shape_GetChainSegment(shape);
        const b2Vec2 points[2] = {chain.segment.point1, chain.segment.point2};
        return b2MakeProxy(points, 2, 0.0f);
    }
    default: return b2MakeProxy(nullptr, 0, 0.0f);
    }
}

// How far outside its core a shape's outline is (a polygon's rounding).
f32 skin_of(b2ShapeId shape) {
    return b2Shape_GetType(shape) == b2_polygonShape ? b2Shape_GetPolygon(shape).radius : 0.0f;
}

// How many threads step the world (with the one calling step): one in a
// build with no threads (the web).
int workers_wanted(i32 asked) {
#if KIN_ENABLE_THREADS
    if (asked > 0) return std::min(asked, B2_MAX_WORKERS);
    if (const char* text = std::getenv("KIN_PHYSICS_WORKERS")) {
        if (const int n = std::atoi(text); n > 0) return std::min(n, B2_MAX_WORKERS);
    }
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
#else
    (void)asked;
    return 1;
#endif
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

struct PhysicsWorld::Impl {
    struct BodySlot {
        b2BodyId body = b2_nullBodyId;
        u32 generation = 1;
        u64 user_id = 0;
        std::vector<u32> fixtures;
        bool character = false;
    };

    struct FixtureSlot {
        bool alive = false;
        std::vector<b2ShapeId> parts{}; // the Box2D shapes it is made of (a chain's segments too)
        std::vector<b2ChainId> chains{};
        PhysicsBody body{};
        u32 generation = 1;
        u64 tag = 0;
        bool sensor = false;
        bool one_sided = false; // and not a chain: which way its top faces, on its body
        b2Vec2 up{0.0f, -1.0f};
    };

    struct JointSlot {
        b2JointId joint = b2_nullJointId;
        PhysicsJointType type = PhysicsJointType::Hinge;
        u32 generation = 1;
        u64 user_id = 0;
    };

    explicit Impl(PhysicsWorldDef def)
        : scale(std::isfinite(def.units_per_meter) && def.units_per_meter > 0.0f ? def.units_per_meter : 1.0f) {
        b2WorldDef world_def = b2DefaultWorldDef();
        world_def.gravity = meters(def.gravity);
        // No body more than 2 metres a step at 60 steps a second, as 2.4 kept them.
        world_def.maximumLinearSpeed = 120.0f;
        // Box2D's own threads (none past the one stepping it for one worker).
        world_def.workerCount = workers_wanted(def.workers);
        world = b2CreateWorld(&world_def);
        b2World_SetPreSolveCallback(world, &Impl::pre_solve, &Impl::pre_continuous, this);
    }

    ~Impl() {
        if (b2World_IsValid(world)) {
            b2DestroyWorld(world);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    b2Vec2 meters(Vec2f value) const { return {value.x / scale, value.y / scale}; }
    f32 meters(f32 length) const { return length / scale; }
    Vec2f units(b2Vec2 value) const { return {value.x * scale, value.y * scale}; }
    f32 units(f32 length) const { return length * scale; }
    f32 torque(f32 value) const { return value / (scale * scale); }

    f32 scale = 1.0f;
    b2WorldId world = b2_nullWorldId;
    b2BodyId ground = b2_nullBodyId;
    std::vector<BodySlot> bodies;
    std::vector<u32> free_bodies;
    std::vector<FixtureSlot> fixtures;
    std::vector<u32> free_fixtures;
    std::vector<JointSlot> joints;
    std::vector<u32> free_joints;
    std::vector<PhysicsContactEvent> events;
    std::map<ContactKey, u32> touching; // for each pair of fixtures touching: how many of their shapes' contacts touch
    // Every pair touching now, made when first asked for after it changed
    // (a crowd has many, and most steps nobody asks).
    std::vector<PhysicsContact> current;
    std::atomic<bool> current_stale{false};
    std::mutex current_mutex;
    f32 last_dt = 0.0f;
    // Contacts of one-sided shapes, judged when they began: let through, and
    // the step they were last seen in (judged again once they part).
    struct Judged {
        bool passing = false;
        u64 seen = 0;
    };
    std::mutex judged_mutex; // pre-solve runs on Box2D's workers
    std::unordered_map<u64, Judged> judged;
    u64 steps = 0;
    bool any_one_sided = false;

    PhysicsBody body_handle(u32 index) const {
        if (index >= bodies.size() || B2_IS_NULL(bodies[index].body)) {
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

    b2BodyId body_id(PhysicsBody handle) const {
        if (handle.index >= bodies.size()) {
            return b2_nullBodyId;
        }
        const BodySlot& slot = bodies[handle.index];
        return slot.generation == handle.generation ? slot.body : b2_nullBodyId;
    }

    const FixtureSlot* fixture_slot(PhysicsFixture handle) const {
        if (handle.index >= fixtures.size()) {
            return nullptr;
        }
        const FixtureSlot& slot = fixtures[handle.index];
        return slot.alive && slot.generation == handle.generation ? &slot : nullptr;
    }

    b2JointId joint_id(PhysicsJoint handle) const {
        if (handle.index >= joints.size()) {
            return b2_nullJointId;
        }
        const JointSlot& slot = joints[handle.index];
        return slot.generation == handle.generation && !B2_IS_NULL(slot.joint) && b2Joint_IsValid(slot.joint) ? slot.joint
                                                                                                              : b2_nullJointId;
    }

    PhysicsBody handle_for(b2BodyId body) const {
        if (B2_IS_NULL(body) || !b2Body_IsValid(body)) {
            return {};
        }
        const auto value = reinterpret_cast<uintptr_t>(b2Body_GetUserData(body));
        if (value == 0 || value > bodies.size()) {
            return {};
        }
        return body_handle(static_cast<u32>(value - 1));
    }

    PhysicsFixture handle_for(b2ShapeId shape) const {
        if (B2_IS_NULL(shape) || !b2Shape_IsValid(shape)) {
            return {};
        }
        const auto value = reinterpret_cast<uintptr_t>(b2Shape_GetUserData(shape));
        if (value == 0 || value > fixtures.size()) {
            return {};
        }
        return fixture_handle(static_cast<u32>(value - 1));
    }

    const FixtureSlot* slot_of(b2ShapeId shape) const {
        return fixture_slot(handle_for(shape));
    }

    bool matches(b2ShapeId shape, const PhysicsFilter& filter) const {
        if (!filter.include_sensors && b2Shape_IsSensor(shape)) {
            return false;
        }
        const b2Filter f = b2Shape_GetFilter(shape);
        if ((f.categoryBits & filter.mask_bits) == 0 || (filter.category_bits & f.maskBits) == 0) {
            return false;
        }
        return !filter.ignore || handle_for(b2Shape_GetBody(shape)) != filter.ignore;
    }

    static b2QueryFilter query_filter(const PhysicsFilter& filter) {
        // Box2D: (shape.category & query.mask) && (shape.mask & query.category).
        return {.categoryBits = filter.category_bits, .maskBits = filter.mask_bits};
    }

    b2BodyId ground_body() {
        if (B2_IS_NULL(ground)) {
            b2BodyDef def = b2DefaultBodyDef();
            def.type = b2_staticBody;
            ground = b2CreateBody(world, &def);
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
        slot.joint = b2_nullJointId;
        slot.user_id = 0;
        slot.generation++;
        free_joints.push_back(index);
    }

    // The joints Box2D destroyed with a body: their slots free.
    void free_gone_joints() {
        for (u32 i = 0; i < joints.size(); ++i) {
            if (!B2_IS_NULL(joints[i].joint) && !b2Joint_IsValid(joints[i].joint)) {
                free_joint(i);
            }
        }
    }

    // A fixture going: its contacts end (told, as 2.4 told them), and are forgotten.
    void end_touching_with(PhysicsFixture fixture) {
        for (auto it = touching.begin(); it != touching.end();) {
            if (it->first.a == fixture || it->first.b == fixture) {
                const FixtureSlot* a = fixture_slot(it->first.a);
                const FixtureSlot* b = fixture_slot(it->first.b);
                if (a && b) {
                    events.push_back({
                        .type = PhysicsContactEventType::End,
                        .body_a = a->body,
                        .body_b = b->body,
                        .fixture_a = it->first.a,
                        .fixture_b = it->first.b,
                        .sensor = a->sensor || b->sensor,
                    });
                }
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
            current.push_back({.body_a = a->body, .body_b = b->body, .fixture_a = key.a, .fixture_b = key.b, .sensor = a->sensor || b->sensor});
        }
    }

    // Where two shapes touch (world, metres), and the normal from the first.
    struct TouchPoint {
        b2Vec2 point{};
        b2Vec2 normal{};
    };

    // A pair of shapes begins (or ends) touching: their fixtures' contact
    // begins with the first of their shapes' and ends with the last's.
    void touch(b2ShapeId shape_a, b2ShapeId shape_b, bool begin, bool sensor, const TouchPoint* at) {
        const PhysicsFixture fixture_a = handle_for(shape_a);
        const PhysicsFixture fixture_b = handle_for(shape_b);
        if (!fixture_a || !fixture_b) {
            return; // one is gone: its end was told when it went
        }
        const ContactKey key = make_key(fixture_a, fixture_b);
        if (begin) {
            if (++touching[key] > 1) {
                return;
            }
        } else {
            const auto found = touching.find(key);
            if (found == touching.end() || --found->second > 0) {
                return;
            }
            touching.erase(found);
        }
        const FixtureSlot* a = fixture_slot(fixture_a);
        const FixtureSlot* b = fixture_slot(fixture_b);
        PhysicsContactEvent event{
            .type = begin ? PhysicsContactEventType::Begin : PhysicsContactEventType::End,
            .body_a = a->body,
            .body_b = b->body,
            .fixture_a = fixture_a,
            .fixture_b = fixture_b,
            .sensor = sensor || a->sensor || b->sensor,
        };
        if (at) {
            event.has_point = true;
            event.point = units(at->point);
            event.normal = from_b2(at->normal);
        }
        events.push_back(event);
    }

    void collect_events() {
        const b2ContactEvents contacts = b2World_GetContactEvents(world);
        for (int i = 0; i < contacts.beginCount; ++i) {
            const b2ContactBeginTouchEvent& e = contacts.beginEvents[i];
            // Where they first touch: a contact's points are kept from its first shape's body centre.
            const b2ContactData data = b2Contact_GetData(e.contactId);
            if (data.manifold.pointCount > 0) {
                const bool same = B2_ID_EQUALS(data.shapeIdA, e.shapeIdA);
                const b2Vec2 point = b2OffsetPos(b2Body_GetWorldCenter(b2Shape_GetBody(data.shapeIdA)), data.manifold.points[0].anchorA);
                const TouchPoint at{.point = point, .normal = same ? data.manifold.normal : -data.manifold.normal};
                touch(e.shapeIdA, e.shapeIdB, true, false, &at);
            } else {
                touch(e.shapeIdA, e.shapeIdB, true, false, nullptr);
            }
        }
        for (int i = 0; i < contacts.endCount; ++i) {
            const b2ContactEndTouchEvent& e = contacts.endEvents[i];
            touch(e.shapeIdA, e.shapeIdB, false, false, nullptr);
        }
        const b2SensorEvents sensors = b2World_GetSensorEvents(world);
        for (int i = 0; i < sensors.beginCount; ++i) {
            touch(sensors.beginEvents[i].sensorShapeId, sensors.beginEvents[i].visitorShapeId, true, true, nullptr);
        }
        for (int i = 0; i < sensors.endCount; ++i) {
            touch(sensors.endEvents[i].sensorShapeId, sensors.endEvents[i].visitorShapeId, false, true, nullptr);
        }
    }

    bool character(b2BodyId body) const {
        const PhysicsBody handle = handle_for(body);
        return handle && bodies[handle.index].character;
    }

    // Box2D shows a contact before it is solved (on its workers; refused with
    // no points, it never touches): a one-sided
    // shape lets through what does not come down onto its top (judged when
    // they meet, until they part); a character is pushed by nothing that
    // does not move by itself.
    static void pre_solve(b2ShapeId shape_a, b2ShapeId shape_b, b2Manifold* manifold, void* context) {
        if (!static_cast<Impl*>(context)->accepts(shape_a, shape_b, *manifold)) {
            manifold->pointCount = 0;
        }
    }

    // The same for a fast shape swept onto another (continuous collision):
    // false and it goes on through.
    static bool pre_continuous(b2ShapeId shape_a, b2ShapeId shape_b, b2Vec2 point, b2Vec2 normal, void* context) {
        b2Manifold contact{};
        contact.normal = normal;
        contact.pointCount = 1;
        contact.points[0].anchorA = point - b2Body_GetWorldCenter(b2Shape_GetBody(shape_a));
        return static_cast<Impl*>(context)->accepts(shape_a, shape_b, contact);
    }

    // Whether a contact goes to the solver.
    bool accepts(b2ShapeId shape_a, b2ShapeId shape_b, const b2Manifold& contact) {
        const b2BodyId body_a = b2Shape_GetBody(shape_a);
        const b2BodyId body_b = b2Shape_GetBody(shape_b);
        if ((character(body_a) && b2Body_GetType(body_b) != b2_dynamicBody) ||
            (character(body_b) && b2Body_GetType(body_a) != b2_dynamicBody)) {
            return false;
        }
        const FixtureSlot* slot_a = slot_of(shape_a);
        const FixtureSlot* slot_b = slot_of(shape_b);
        const bool one_a = slot_a && slot_a->one_sided;
        const bool one_b = slot_b && slot_b->one_sided;
        if (!one_a && !one_b) {
            return true;
        }
        const u64 key = pair_key(shape_a, shape_b);
        {
            const std::scoped_lock lock(judged_mutex);
            if (const auto found = judged.find(key); found != judged.end()) {
                found->second.seen = steps;
                return !found->second.passing;
            }
        }
        bool passing = true; // two platforms: neither stops the other
        if (!(one_a && one_b)) {
            const b2BodyId platform = one_a ? body_a : body_b;
            const b2BodyId other = one_a ? body_b : body_a;
            const b2Vec2 out = one_a ? contact.normal : -contact.normal; // from the platform to the other
            const b2Vec2 up = b2RotateVector(b2Body_GetRotation(platform), (one_a ? slot_a : slot_b)->up);
            const b2Vec2 at = contact.pointCount > 0
                                  ? b2OffsetPos(b2Body_GetWorldCenter(body_a), contact.points[0].anchorA)
                                  : b2Body_GetWorldCenter(other);
            const b2Vec2 rising = b2Body_GetWorldPointVelocity(other, at) - b2Body_GetWorldPointVelocity(platform, at);
            passing = b2Dot(out, up) < 0.7f || b2Dot(rising, up) > 0.05f;
        }
        const std::scoped_lock lock(judged_mutex);
        judged[key] = {.passing = passing, .seen = steps};
        return !passing;
    }

    // `p`, a point of a shape, on its body (in metres).
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

    // Each Box2D shape `shape` takes, made on `body`; a chain's segments too.
    template <typename Done>
    void make_parts(b2BodyId body, const PhysicsShape& shape, const b2ShapeDef& def, FixtureSlot& slot, Done&& done) {
        const auto made = [&](b2ShapeId id) {
            slot.parts.push_back(id);
            done(id);
        };
        switch (shape.type) {
        case PhysicsShapeType::Box: {
            const f32 hx = meters(std::abs(shape.half_extents.x));
            const f32 hy = meters(std::abs(shape.half_extents.y));
            if (!(hx > 0.0f) || !(hy > 0.0f)) return;
            const b2Polygon box = b2MakeOffsetBox(hx, hy, meters(shape.offset), b2MakeRot(shape.angle));
            made(b2CreatePolygonShape(body, &def, &box));
            return;
        }
        case PhysicsShapeType::Circle: {
            if (!(shape.radius > 0.0f)) return;
            const b2Circle circle{.center = meters(shape.offset), .radius = meters(shape.radius)};
            made(b2CreateCircleShape(body, &def, &circle));
            return;
        }
        case PhysicsShapeType::Capsule: {
            const f32 hx = std::abs(shape.half_extents.x);
            const f32 hy = std::abs(shape.half_extents.y);
            const bool across = hx >= hy;
            const f32 radius = across ? hy : hx;
            const f32 reach = (across ? hx : hy) - radius;
            if (!(radius > 0.0f)) return;
            if (!(meters(reach) > linear_slop)) {
                const b2Circle circle{.center = meters(shape.offset), .radius = meters(radius)};
                made(b2CreateCircleShape(body, &def, &circle));
                return;
            }
            const Vec2f end = across ? Vec2f{reach, 0.0f} : Vec2f{0.0f, reach};
            const b2Capsule capsule{.center1 = placed(shape, {-end.x, -end.y}), .center2 = placed(shape, end), .radius = meters(radius)};
            made(b2CreateCapsuleShape(body, &def, &capsule));
            return;
        }
        case PhysicsShapeType::Polygon: {
            const std::vector<b2Vec2> points = distinct(placed_points(shape), true);
            if (points.size() < 3) return;
            if (points.size() <= B2_MAX_POLYGON_VERTICES && convex(points)) {
                if (const auto polygon = make_polygon(points)) {
                    made(b2CreatePolygonShape(body, &def, &*polygon));
                }
                return;
            }
            std::vector<std::vector<std::array<f64, 2>>> rings(1);
            rings[0].reserve(points.size());
            for (const b2Vec2& point : points) rings[0].push_back({point.x, point.y});
            const std::vector<u32> corners = mapbox::earcut<u32>(rings);
            for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
                if (const auto triangle = make_polygon({points[corners[i]], points[corners[i + 1]], points[corners[i + 2]]})) {
                    made(b2CreatePolygonShape(body, &def, &*triangle));
                }
            }
            return;
        }
        case PhysicsShapeType::Chain: {
            std::vector<b2Vec2> points = distinct(placed_points(shape), shape.loop);
            const bool loop = shape.loop && points.size() >= 3;
            if (points.size() < 2) return;
            if (shape.one_sided) {
                // An open chain's ghosts (past its ends, along them) only
                // guide what slides off them.
                b2ChainDef chain = b2DefaultChainDef();
                chain.points = points.data();
                chain.pointCount = static_cast<int>(points.size());
                chain.isLoop = loop;
                if (!loop) {
                    chain.ghost1 = points[0] + (points[0] - points[1]);
                    chain.ghost2 = points.back() + (points.back() - points[points.size() - 2]);
                }
                chain.filter = def.filter;
                chain.userData = def.userData;
                chain.enableSensorEvents = true;
                b2SurfaceMaterial material = def.material;
                chain.materials = &material;
                chain.materialCount = 1;
                const b2ChainId id = b2CreateChain(body, &chain);
                slot.chains.push_back(id);
                std::vector<b2ShapeId> segments(static_cast<std::size_t>(b2Chain_GetSegmentCount(id)));
                b2Chain_GetSegments(id, segments.data(), static_cast<int>(segments.size()));
                for (b2ShapeId segment : segments) made(segment);
                return;
            }
            const std::size_t segments = loop ? points.size() : points.size() - 1;
            for (std::size_t i = 0; i < segments; ++i) {
                const b2Segment segment{.point1 = points[i], .point2 = points[(i + 1) % points.size()]};
                made(b2CreateSegmentShape(body, &def, &segment));
            }
            return;
        }
        }
    }

    // ---- Shapes that move: casts and characters ----

    struct Mover {
        std::vector<b2Vec2> points;
        f32 radius = 0.0f;

        b2ShapeProxy proxy() const { return b2MakeProxy(points.data(), static_cast<int>(points.size()), radius); }

        b2AABB aabb(b2Vec2 at) const {
            b2AABB box{at + points[0], at + points[0]};
            for (const b2Vec2& point : points) {
                box.lowerBound = b2Min(box.lowerBound, at + point);
                box.upperBound = b2Max(box.upperBound, at + point);
            }
            box.lowerBound = box.lowerBound - b2Vec2{radius, radius};
            box.upperBound = box.upperBound + b2Vec2{radius, radius};
            return box;
        }

        f32 lowest(b2Vec2 at, b2Vec2 up) const {
            f32 low = max_float;
            for (const b2Vec2& point : points) low = std::min(low, b2Dot(at + point, up));
            return low - radius;
        }
    };

    std::optional<Mover> mover_of(const PhysicsShape& shape, bool rounded) const {
        Mover mover;
        switch (shape.type) {
        case PhysicsShapeType::Box: {
            const f32 hx = std::abs(shape.half_extents.x);
            const f32 hy = std::abs(shape.half_extents.y);
            const f32 least = meters(std::min(hx, hy));
            if (!(least > 0.0f)) return std::nullopt;
            const f32 round = std::min(std::max(polygon_radius, rounded ? 0.15f * least : 0.0f), 0.5f * least);
            mover.radius = round;
            const f32 cx = hx - units(round);
            const f32 cy = hy - units(round);
            for (const Vec2f corner : {Vec2f{-cx, -cy}, Vec2f{cx, -cy}, Vec2f{cx, cy}, Vec2f{-cx, cy}}) {
                mover.points.push_back(placed(shape, corner));
            }
            return mover;
        }
        case PhysicsShapeType::Circle:
            if (!(shape.radius > 0.0f)) return std::nullopt;
            mover.points.push_back(meters(shape.offset));
            mover.radius = meters(shape.radius);
            return mover;
        case PhysicsShapeType::Capsule: {
            const f32 hx = std::abs(shape.half_extents.x);
            const f32 hy = std::abs(shape.half_extents.y);
            const bool across = hx >= hy;
            const f32 radius = across ? hy : hx;
            const f32 reach = (across ? hx : hy) - radius;
            if (!(radius > 0.0f)) return std::nullopt;
            mover.radius = meters(radius);
            const Vec2f end = across ? Vec2f{reach, 0.0f} : Vec2f{0.0f, reach};
            mover.points.push_back(placed(shape, end));
            if (meters(reach) > linear_slop) {
                mover.points.push_back(placed(shape, {-end.x, -end.y}));
            } else {
                mover.points.back() = meters(shape.offset);
            }
            return mover;
        }
        case PhysicsShapeType::Polygon:
            mover.points = distinct(placed_points(shape), true);
            if (mover.points.size() < 3 || mover.points.size() > B2_MAX_POLYGON_VERTICES ||
                std::abs(doubled_area(mover.points)) < 2.0f * linear_slop * linear_slop) {
                return std::nullopt;
            }
            mover.radius = polygon_radius;
            return mover;
        case PhysicsShapeType::Chain: return std::nullopt;
        }
        return std::nullopt;
    }

    // A convex piece of something in the world near a mover: one Box2D shape.
    struct Part {
        b2ShapeId shape = b2_nullShapeId;
        b2Transform xf{};
        b2Vec2 shift{0.0f, 0.0f};
        bool dynamic = false;
        bool one_sided = false;
        b2Vec2 up{0.0f, 0.0f};
        f32 top = 0.0f;
        b2ShapeProxy proxy{};
        f32 skin = 0.0f;
    };

    std::vector<Part> parts_near(const b2AABB& box, const PhysicsFilter& filter, f32 dt, b2Vec2 around) const {
        struct Found {
            const Impl& impl;
            const PhysicsFilter& filter;
            std::vector<b2ShapeId> shapes;
        } found{*this, filter, {}};
        b2World_OverlapAABB(world, b2Pos_zero, box, query_filter(filter), [](b2ShapeId shape, void* context) {
            auto& f = *static_cast<Found*>(context);
            if (f.impl.matches(shape, f.filter)) f.shapes.push_back(shape);
            return true;
        }, &found);

        std::vector<Part> parts;
        parts.reserve(found.shapes.size());
        for (b2ShapeId shape : found.shapes) {
            const b2BodyId body = b2Shape_GetBody(shape);
            const b2BodyType type = b2Body_GetType(body);
            Part part{.shape = shape, .xf = b2Body_GetTransform(body), .dynamic = type == b2_dynamicBody};
            if (type != b2_staticBody && dt > 0.0f) {
                part.shift = dt * b2Body_GetWorldPointVelocity(body, around);
            }
            part.proxy = proxy_of(shape);
            part.skin = skin_of(shape);
            const FixtureSlot* slot = slot_of(shape);
            if (b2Shape_GetType(shape) == b2_chainSegmentShape) {
                // A chain's front: to the right going from its first point.
                const b2ChainSegment chain = b2Shape_GetChainSegment(shape);
                const b2Vec2 along = b2Normalize(chain.segment.point2 - chain.segment.point1);
                part.one_sided = true;
                part.up = b2RotateVector(part.xf.q, b2Vec2{along.y, -along.x});
                part.top = b2Dot(b2TransformPoint(part.xf, chain.segment.point1), part.up);
            } else if (slot && slot->one_sided) {
                part.one_sided = true;
                part.up = b2RotateVector(part.xf.q, slot->up);
                part.top = -max_float;
                for (int i = 0; i < part.proxy.count; ++i) {
                    part.top = std::max(part.top, b2Dot(b2TransformPoint(part.xf, part.proxy.points[i]), part.up));
                }
                part.top += part.proxy.radius;
            }
            parts.push_back(part);
        }
        return parts;
    }

    struct Gap {
        f32 distance = 0.0f;
        b2Vec2 normal{0.0f, 0.0f};
        b2Vec2 point{0.0f, 0.0f};
        bool inside = false;
    };

    static Gap gap(const b2ShapeProxy& mover, b2Vec2 at, const b2ShapeProxy& other, const b2Transform& other_xf) {
        b2DistanceInput input{};
        input.proxyA = mover;
        input.proxyB = other;
        const b2Transform mover_xf = transform_at(at);
        input.transform = b2InvMulTransforms(mover_xf, other_xf); // the other in the mover's frame
        input.useRadii = false;
        b2SimplexCache cache{};
        b2DistanceOutput output = b2ComputeShapeDistance(&input, &cache, nullptr, 0);
        output.pointA = b2TransformPoint(mover_xf, output.pointA);
        output.pointB = b2TransformPoint(mover_xf, output.pointB);
        Gap found;
        const f32 radii = mover.radius + other.radius;
        if (output.distance < 1e-6f) {
            found.inside = true;
            found.distance = -radii;
            return found;
        }
        found.normal = (1.0f / output.distance) * (output.pointA - output.pointB);
        found.point = output.pointB + other.radius * found.normal;
        found.distance = output.distance - radii;
        return found;
    }

    static std::optional<std::pair<f32, Gap>> sweep(const b2ShapeProxy& mover, b2Vec2 at, b2Vec2 motion, const b2ShapeProxy& other,
                                                     const b2Transform& other_xf, f32 target) {
        constexpr f32 tolerance = 0.25f * linear_slop;
        f32 t = 0.0f;
        Gap found;
        for (i32 i = 0; i < 32; ++i) {
            found = gap(mover, at + t * motion, other, other_xf);
            if (found.inside) return std::nullopt;
            const f32 closing = -b2Dot(motion, found.normal);
            if (closing <= 1e-7f) return std::nullopt;
            if (found.distance <= target + tolerance) return std::pair{t, found};
            t += (found.distance - target) / closing;
            if (t > 1.0f) return std::nullopt;
        }
        return std::pair{t, found};
    }

    static bool stops(const Part& part, const Mover& mover, b2Vec2 at, b2Vec2 motion, bool one_way, b2Vec2 lift = b2Vec2{0.0f, 0.0f}) {
        if (!part.one_sided) return true;
        if (!one_way || b2Dot(motion, part.up) >= 0.0f) return false;
        return mover.lowest(at, part.up) >= part.top + b2Dot(lift, part.up) - 2.0f * linear_slop;
    }

    void recover(const Mover& mover, b2Vec2& at, const std::vector<Part>& parts, f32 target, bool one_way) const {
        const b2ShapeProxy proxy = mover.proxy();
        for (i32 round = 0; round < 4; ++round) {
            f32 deepest = 0.0f;
            b2Vec2 push{0.0f, 0.0f};
            for (const Part& part : parts) {
                if (part.one_sided && !(one_way && mover.lowest(at, part.up) >= part.top - 2.0f * linear_slop)) continue;
                const Gap found = gap(proxy, at, part.proxy, part.xf);
                f32 depth = 0.0f;
                b2Vec2 way{0.0f, 0.0f};
                if (found.inside) {
                    const b2AABB mine = mover.aabb(at);
                    const b2AABB theirs = b2Shape_GetBounds(part.shape);
                    const f32 left = mine.upperBound.x - theirs.lowerBound.x;
                    const f32 right = theirs.upperBound.x - mine.lowerBound.x;
                    const f32 above = mine.upperBound.y - theirs.lowerBound.y;
                    const f32 below = theirs.upperBound.y - mine.lowerBound.y;
                    const f32 across = std::min(left, right);
                    const f32 along = std::min(above, below);
                    if (across < along) {
                        depth = across + target;
                        way = {left < right ? -1.0f : 1.0f, 0.0f};
                    } else {
                        depth = along + target;
                        way = {0.0f, above < below ? -1.0f : 1.0f};
                    }
                } else if (found.distance < -0.25f * linear_slop) {
                    depth = target - found.distance;
                    way = found.normal;
                }
                if (depth > deepest) {
                    deepest = depth;
                    push = depth * way;
                }
            }
            if (deepest <= 0.0f) return;
            at += push;
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
    b2BodyDef body_def = b2DefaultBodyDef();
    body_def.type = to_b2(def.type);
    body_def.position = _impl->meters(def.position);
    body_def.rotation = b2MakeRot(def.rotation);
    body_def.linearVelocity = _impl->meters(def.linear_velocity);
    body_def.angularVelocity = def.angular_velocity;
    body_def.linearDamping = def.linear_damping;
    body_def.angularDamping = def.angular_damping;
    body_def.gravityScale = def.gravity_scale;
    body_def.motionLocks.angularZ = def.fixed_rotation;
    body_def.enableSleep = def.allow_sleep;

    u32 index = 0;
    if (!_impl->free_bodies.empty()) {
        index = _impl->free_bodies.back();
        _impl->free_bodies.pop_back();
    } else {
        index = static_cast<u32>(_impl->bodies.size());
        _impl->bodies.push_back({});
    }
    body_def.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(index) + 1);
    Impl::BodySlot& slot = _impl->bodies[index];
    slot.body = b2CreateBody(_impl->world, &body_def);
    slot.user_id = def.user_id;
    slot.character = def.character;
    slot.fixtures.clear();
    return {index, slot.generation};
}

void PhysicsWorld::destroy_body(PhysicsBody body) {
    if (!is_alive(body)) {
        return;
    }
    Impl::BodySlot& slot = _impl->bodies[body.index];
    for (u32 fixture_index : slot.fixtures) {
        if (const PhysicsFixture fixture = _impl->fixture_handle(fixture_index)) {
            _impl->end_touching_with(fixture);
        }
    }
    b2DestroyBody(slot.body); // its shapes and joints with it
    for (u32 fixture_index : slot.fixtures) {
        if (_impl->fixture_handle(fixture_index)) {
            _impl->free_fixture(fixture_index);
        }
    }
    slot.fixtures.clear();
    slot.body = b2_nullBodyId;
    slot.user_id = 0;
    slot.character = false;
    slot.generation++;
    _impl->free_bodies.push_back(body.index);
    _impl->free_gone_joints();
    _impl->current_stale.store(true, std::memory_order_release);
}

bool PhysicsWorld::is_alive(PhysicsBody body) const {
    return !B2_IS_NULL(_impl->body_id(body));
}

PhysicsFixture PhysicsWorld::add_fixture(PhysicsBody body, const PhysicsFixtureDef& def) {
    const b2BodyId body_id = _impl->body_id(body);
    if (B2_IS_NULL(body_id)) {
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
    Impl::FixtureSlot& slot = _impl->fixtures[index];
    slot.alive = true;
    slot.parts.clear();
    slot.chains.clear();
    slot.body = body;
    slot.tag = def.tag;
    slot.sensor = def.sensor;
    slot.one_sided = def.shape.one_sided && def.shape.type != PhysicsShapeType::Chain;
    slot.up = b2Vec2{std::sin(def.shape.angle), -std::cos(def.shape.angle)};
    if (slot.one_sided && !_impl->any_one_sided) {
        _impl->any_one_sided = true;
    }

    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(index) + 1);
    shape_def.density = def.density;
    shape_def.material.friction = def.friction;
    shape_def.material.restitution = def.restitution;
    shape_def.isSensor = def.sensor;
    shape_def.enableSensorEvents = true; // sensors see it (and, a sensor, it sees)
    shape_def.enableContactEvents = true;
    shape_def.filter.categoryBits = def.category_bits;
    shape_def.filter.maskBits = def.mask_bits;
    // Box2D asks first for a one-sided shape's contacts and a character's.
    shape_def.enablePreSolveEvents = slot.one_sided || _impl->bodies[body.index].character;
    _impl->make_parts(body_id, def.shape, shape_def, slot, [](b2ShapeId) {});
    _impl->bodies[body.index].fixtures.push_back(index);
    return {index, _impl->fixtures[index].generation};
}

void PhysicsWorld::destroy_fixture(PhysicsFixture fixture) {
    const Impl::FixtureSlot* slot = _impl->fixture_slot(fixture);
    if (!slot) {
        return;
    }
    _impl->end_touching_with(fixture);
    const PhysicsBody owner = slot->body;
    if (!B2_IS_NULL(_impl->body_id(owner))) {
        for (b2ChainId chain : slot->chains) {
            if (b2Chain_IsValid(chain)) b2DestroyChain(chain);
        }
        for (b2ShapeId part : slot->parts) {
            if (b2Shape_IsValid(part) && B2_IS_NULL(b2Shape_GetParentChain(part))) b2DestroyShape(part, true);
        }
        auto& owned = _impl->bodies[owner.index].fixtures;
        owned.erase(std::remove(owned.begin(), owned.end(), fixture.index), owned.end());
    }
    _impl->free_fixture(fixture.index);
    _impl->current_stale.store(true, std::memory_order_release);
}

bool PhysicsWorld::is_alive(PhysicsFixture fixture) const {
    return _impl->fixture_slot(fixture) != nullptr;
}

std::vector<PhysicsFixture> PhysicsWorld::fixtures(PhysicsBody body) const {
    std::vector<PhysicsFixture> result;
    if (!is_alive(body)) return result;
    const Impl::BodySlot& slot = _impl->bodies[body.index];
    result.reserve(slot.fixtures.size());
    for (u32 fixture_index : slot.fixtures) {
        if (PhysicsFixture fixture = _impl->fixture_handle(fixture_index)) result.push_back(fixture);
    }
    return result;
}

std::size_t PhysicsWorld::fixture_count(PhysicsBody body) const {
    if (!is_alive(body)) return 0;
    std::size_t count = 0;
    for (u32 fixture_index : _impl->bodies[body.index].fixtures) {
        count += _impl->fixture_handle(fixture_index) ? 1 : 0;
    }
    return count;
}

PhysicsBody PhysicsWorld::body_of(PhysicsFixture fixture) const {
    const Impl::FixtureSlot* slot = _impl->fixture_slot(fixture);
    return slot ? slot->body : PhysicsBody{};
}

u32 PhysicsWorld::shape_parts(PhysicsFixture fixture) const {
    const Impl::FixtureSlot* slot = _impl->fixture_slot(fixture);
    if (!slot) return 0;
    // A chain is one Box2D shape (as in 2.4), however many segments it has.
    u32 parts = static_cast<u32>(slot->chains.size());
    for (b2ShapeId part : slot->parts) {
        parts += b2Shape_IsValid(part) && B2_IS_NULL(b2Shape_GetParentChain(part)) ? 1 : 0;
    }
    return parts;
}

PhysicsJoint PhysicsWorld::create_joint(const PhysicsJointDef& def) {
    const b2BodyId kin_a = _impl->body_id(def.body_a);
    if (B2_IS_NULL(kin_a) || (def.body_b.valid() && B2_IS_NULL(_impl->body_id(def.body_b)))) {
        return {};
    }
    const b2BodyId kin_b = def.body_b.valid() ? _impl->body_id(def.body_b) : _impl->ground_body();
    if (B2_ID_EQUALS(kin_a, kin_b)) {
        return {};
    }
    // Box2D measures its second body from its first: body_a goes second, so
    // a joint's angle, travel and motor are body_a's from body_b.
    const b2BodyId a = kin_b;
    const b2BodyId b = kin_a;
    const b2Vec2 anchor = _impl->meters(def.anchor);
    b2Vec2 axis{def.axis.x, def.axis.y};
    if (!(b2Length(axis) > 1e-6f)) axis = {1.0f, 0.0f};
    axis = b2Normalize(axis);
    const f32 lower = std::min(def.lower, def.upper);
    const f32 upper = std::max(def.lower, def.upper);
    // A joint holds a frame on each body: at its anchor there, turned so its
    // x-axis is the joint's axis (along which a slider or wheel travels), the
    // same in the world on both, so it starts at no angle.
    const b2Rot turned = b2MakeRotFromUnitVector(axis);
    const auto place = [&](b2JointDef& base, b2Vec2 on_a, b2Vec2 on_b) {
        base.bodyIdA = a;
        base.bodyIdB = b;
        base.localFrameA = {b2Body_GetLocalPoint(a, on_a), b2InvMulRot(b2Body_GetRotation(a), turned)};
        base.localFrameB = {b2Body_GetLocalPoint(b, on_b), b2InvMulRot(b2Body_GetRotation(b), turned)};
        base.collideConnected = def.collide_connected;
    };

    b2JointId joint = b2_nullJointId;
    switch (def.type) {
    case PhysicsJointType::Hinge: {
        b2RevoluteJointDef j = b2DefaultRevoluteJointDef();
        place(j.base, anchor, anchor);
        j.enableLimit = def.limit;
        j.lowerAngle = lower;
        j.upperAngle = upper;
        j.enableMotor = def.motor;
        j.motorSpeed = def.motor_speed;
        j.maxMotorTorque = _impl->torque(std::abs(def.max_motor_force));
        joint = b2CreateRevoluteJoint(_impl->world, &j);
        break;
    }
    case PhysicsJointType::Slider: {
        b2PrismaticJointDef j = b2DefaultPrismaticJointDef();
        place(j.base, anchor, anchor);
        j.enableLimit = def.limit;
        j.lowerTranslation = _impl->meters(lower);
        j.upperTranslation = _impl->meters(upper);
        j.enableMotor = def.motor;
        j.motorSpeed = _impl->meters(def.motor_speed);
        j.maxMotorForce = _impl->meters(std::abs(def.max_motor_force));
        joint = b2CreatePrismaticJoint(_impl->world, &j);
        break;
    }
    case PhysicsJointType::Distance: {
        b2DistanceJointDef j = b2DefaultDistanceJointDef();
        const b2Vec2 on_a = _impl->meters(def.other_anchor);
        place(j.base, on_a, anchor);
        const f32 apart = b2Distance(on_a, anchor);
        j.length = def.length > 0.0f ? _impl->meters(def.length) : std::max(apart, linear_slop);
        j.enableLimit = def.limit;
        j.minLength = def.limit ? _impl->meters(lower) : 0.0f;
        j.maxLength = def.limit ? _impl->meters(upper) : 100000.0f;
        j.enableSpring = def.limit || def.frequency > 0.0f;
        j.hertz = def.frequency;
        j.dampingRatio = def.damping_ratio;
        joint = b2CreateDistanceJoint(_impl->world, &j);
        break;
    }
    case PhysicsJointType::Weld: {
        b2WeldJointDef j = b2DefaultWeldJointDef();
        place(j.base, anchor, anchor);
        j.linearHertz = def.frequency;
        j.angularHertz = def.frequency;
        j.linearDampingRatio = def.damping_ratio;
        j.angularDampingRatio = def.damping_ratio;
        joint = b2CreateWeldJoint(_impl->world, &j);
        break;
    }
    case PhysicsJointType::Wheel: {
        b2WheelJointDef j = b2DefaultWheelJointDef();
        place(j.base, anchor, anchor);
        j.enableLimit = def.limit;
        j.lowerTranslation = _impl->meters(lower);
        j.upperTranslation = _impl->meters(upper);
        j.enableMotor = def.motor;
        j.motorSpeed = def.motor_speed;
        j.maxMotorTorque = _impl->torque(std::abs(def.max_motor_force));
        j.enableSpring = def.frequency > 0.0f;
        j.hertz = def.frequency;
        j.dampingRatio = def.damping_ratio;
        joint = b2CreateWheelJoint(_impl->world, &j);
        break;
    }
    }
    if (B2_IS_NULL(joint)) {
        return {};
    }
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
    return {index, slot.generation};
}

void PhysicsWorld::destroy_joint(PhysicsJoint joint) {
    const b2JointId id = _impl->joint_id(joint);
    if (!B2_IS_NULL(id)) {
        b2DestroyJoint(id);
        _impl->free_joint(joint.index);
    }
}

bool PhysicsWorld::is_alive(PhysicsJoint joint) const {
    return !B2_IS_NULL(_impl->joint_id(joint));
}

void PhysicsWorld::update_joint(PhysicsJoint joint, const PhysicsJointDef& def) {
    const b2JointId id = _impl->joint_id(joint);
    if (B2_IS_NULL(id)) return;
    const f32 lower = std::min(def.lower, def.upper);
    const f32 upper = std::max(def.lower, def.upper);
    switch (_impl->joints[joint.index].type) {
    case PhysicsJointType::Hinge:
        b2RevoluteJoint_EnableLimit(id, def.limit);
        b2RevoluteJoint_SetLimits(id, lower, upper);
        b2RevoluteJoint_EnableMotor(id, def.motor);
        b2RevoluteJoint_SetTargetMotorSpeed(id, def.motor_speed);
        b2RevoluteJoint_SetMaxMotorTorque(id, _impl->torque(std::abs(def.max_motor_force)));
        break;
    case PhysicsJointType::Slider:
        b2PrismaticJoint_EnableLimit(id, def.limit);
        b2PrismaticJoint_SetLimits(id, _impl->meters(lower), _impl->meters(upper));
        b2PrismaticJoint_EnableMotor(id, def.motor);
        b2PrismaticJoint_SetTargetMotorSpeed(id, _impl->meters(def.motor_speed));
        b2PrismaticJoint_SetMaxMotorForce(id, _impl->meters(std::abs(def.max_motor_force)));
        break;
    case PhysicsJointType::Distance:
        if (def.length > 0.0f) b2DistanceJoint_SetRestLength(id, _impl->meters(def.length));
        b2DistanceJoint_EnableLimit(id, def.limit);
        if (def.limit) b2DistanceJoint_SetLimits(id, _impl->meters(lower), _impl->meters(upper));
        b2DistanceJoint_EnableSpring(id, def.limit || def.frequency > 0.0f);
        b2DistanceJoint_SetSpringHertz(id, def.frequency);
        b2DistanceJoint_SetSpringDampingRatio(id, def.damping_ratio);
        break;
    case PhysicsJointType::Weld:
        b2WeldJoint_SetLinearHertz(id, def.frequency);
        b2WeldJoint_SetAngularHertz(id, def.frequency);
        b2WeldJoint_SetLinearDampingRatio(id, def.damping_ratio);
        b2WeldJoint_SetAngularDampingRatio(id, def.damping_ratio);
        break;
    case PhysicsJointType::Wheel:
        b2WheelJoint_EnableLimit(id, def.limit);
        b2WheelJoint_SetLimits(id, _impl->meters(lower), _impl->meters(upper));
        b2WheelJoint_EnableMotor(id, def.motor);
        b2WheelJoint_SetTargetMotorSpeed(id, def.motor_speed);
        b2WheelJoint_SetMaxMotorTorque(id, _impl->torque(std::abs(def.max_motor_force)));
        b2WheelJoint_EnableSpring(id, def.frequency > 0.0f);
        b2WheelJoint_SetSpringHertz(id, def.frequency);
        b2WheelJoint_SetSpringDampingRatio(id, def.damping_ratio);
        break;
    }
    b2Joint_WakeBodies(id);
}

Vec2f PhysicsWorld::joint_force(PhysicsJoint joint) const {
    const b2JointId id = _impl->joint_id(joint);
    if (B2_IS_NULL(id)) return {};
    return _impl->units(b2Joint_GetConstraintForce(id));
}

f32 PhysicsWorld::joint_value(PhysicsJoint joint) const {
    const b2JointId id = _impl->joint_id(joint);
    if (B2_IS_NULL(id)) return 0.0f;
    switch (_impl->joints[joint.index].type) {
    case PhysicsJointType::Hinge: return b2RevoluteJoint_GetCurrentAngle(id);
    case PhysicsJointType::Slider: return _impl->units(b2PrismaticJoint_GetCurrentTranslation(id));
    case PhysicsJointType::Distance: return _impl->units(b2DistanceJoint_GetCurrentLength(id));
    case PhysicsJointType::Weld: return 0.0f;
    case PhysicsJointType::Wheel: {
        const b2BodyId a = b2Joint_GetBodyA(id);
        const b2BodyId b = b2Joint_GetBodyB(id);
        const b2Transform frame_a = b2Joint_GetLocalFrameA(id);
        const b2Vec2 pa = b2Body_GetWorldPoint(a, frame_a.p);
        const b2Vec2 pb = b2Body_GetWorldPoint(b, b2Joint_GetLocalFrameB(id).p);
        const b2Vec2 axis = b2RotateVector(b2MulRot(b2Body_GetRotation(a), frame_a.q), {1.0f, 0.0f}); // its frame's x-axis
        return _impl->units(b2Dot(pb - pa, axis));
    }
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
    const b2BodyId id = _impl->body_id(body);
    if (!B2_IS_NULL(id)) b2Body_SetTransform(id, _impl->meters(position), b2MakeRot(rotation));
}

Vec2f PhysicsWorld::position(PhysicsBody body) const {
    const b2BodyId id = _impl->body_id(body);
    return B2_IS_NULL(id) ? Vec2f{} : _impl->units(b2Body_GetPosition(id));
}

f32 PhysicsWorld::rotation(PhysicsBody body) const {
    const b2BodyId id = _impl->body_id(body);
    return B2_IS_NULL(id) ? 0.0f : b2Rot_GetAngle(b2Body_GetRotation(id));
}

void PhysicsWorld::set_linear_velocity(PhysicsBody body, Vec2f velocity) {
    const b2BodyId id = _impl->body_id(body);
    if (!B2_IS_NULL(id)) b2Body_SetLinearVelocity(id, _impl->meters(velocity));
}

Vec2f PhysicsWorld::linear_velocity(PhysicsBody body) const {
    const b2BodyId id = _impl->body_id(body);
    return B2_IS_NULL(id) ? Vec2f{} : _impl->units(b2Body_GetLinearVelocity(id));
}

void PhysicsWorld::apply_force(PhysicsBody body, Vec2f force) {
    const b2BodyId id = _impl->body_id(body);
    if (!B2_IS_NULL(id)) b2Body_ApplyForceToCenter(id, _impl->meters(force), true);
}

void PhysicsWorld::apply_impulse(PhysicsBody body, Vec2f impulse) {
    const b2BodyId id = _impl->body_id(body);
    if (!B2_IS_NULL(id)) b2Body_ApplyLinearImpulseToCenter(id, _impl->meters(impulse), true);
}

void PhysicsWorld::step(f32 dt, i32 velocity_iterations, i32) {
    _impl->events.clear();
    _impl->last_dt = dt;
    ++_impl->steps;
    // Box2D 3 solves in sub-steps (soft constraints): 4 is its default; 2.4's
    // velocity iterations, halved, say about as much.
    const int substeps = std::clamp(velocity_iterations / 2, 1, 8);
    if (dt > 0.0f) {
        b2World_Step(_impl->world, dt, substeps);
        _impl->collect_events();
    }
    // One-sided contacts not seen this step have parted: judged again when they meet.
    if (!_impl->judged.empty()) {
        std::erase_if(_impl->judged, [&](const auto& item) { return item.second.seen + 1 < _impl->steps; });
    }
    _impl->current_stale.store(true, std::memory_order_release);
}

std::span<const PhysicsContactEvent> PhysicsWorld::contacts() const {
    return _impl->events;
}

std::span<const PhysicsContact> PhysicsWorld::current_contacts() const {
    if (_impl->current_stale.load(std::memory_order_acquire)) {
        const std::scoped_lock lock(_impl->current_mutex);
        if (_impl->current_stale.load(std::memory_order_relaxed)) {
            _impl->rebuild_current_contacts();
            _impl->current_stale.store(false, std::memory_order_release);
        }
    }
    return _impl->current;
}

void PhysicsWorld::clear_contacts() {
    _impl->events.clear();
}

std::vector<PhysicsFixture> PhysicsWorld::query_point(Vec2f point, PhysicsFilter filter) const {
    struct Context {
        const Impl& impl;
        b2Vec2 at;
        PhysicsFilter filter;
        std::vector<PhysicsFixture> results;
        std::unordered_set<u64> seen;
    } context{*_impl, _impl->meters(point), filter, {}, {}};
    const b2AABB box{{context.at.x - 0.001f, context.at.y - 0.001f}, {context.at.x + 0.001f, context.at.y + 0.001f}};
    b2World_OverlapAABB(_impl->world, b2Pos_zero, box, Impl::query_filter(filter), [](b2ShapeId shape, void* raw) {
        auto& c = *static_cast<Context*>(raw);
        if (c.impl.matches(shape, c.filter) && b2Shape_TestPoint(shape, c.at)) {
            const PhysicsFixture found = c.impl.handle_for(shape);
            if (found && c.seen.insert(fixture_key(found)).second) c.results.push_back(found);
        }
        return true;
    }, &context);
    return context.results;
}

std::vector<PhysicsFixture> PhysicsWorld::query_aabb(Rectf rect, PhysicsFilter filter) const {
    struct Context {
        const Impl& impl;
        PhysicsFilter filter;
        std::vector<PhysicsFixture> results;
        std::unordered_set<u64> seen;
    } context{*_impl, filter, {}, {}};
    const b2AABB box{_impl->meters(Vec2f{rect.x, rect.y}), _impl->meters(Vec2f{rect.x + rect.w, rect.y + rect.h})};
    b2World_OverlapAABB(_impl->world, b2Pos_zero, box, Impl::query_filter(filter), [](b2ShapeId shape, void* raw) {
        auto& c = *static_cast<Context*>(raw);
        if (c.impl.matches(shape, c.filter)) {
            const PhysicsFixture found = c.impl.handle_for(shape);
            if (found && c.seen.insert(fixture_key(found)).second) c.results.push_back(found);
        }
        return true;
    }, &context);
    return context.results;
}

std::vector<PhysicsRaycastHit> PhysicsWorld::raycast(Vec2f from, Vec2f to, PhysicsFilter filter) const {
    struct Context {
        const Impl& impl;
        PhysicsFilter filter;
        std::vector<PhysicsRaycastHit> hits;
    } context{*_impl, filter, {}};
    const b2Vec2 origin = _impl->meters(from);
    b2World_CastRay(_impl->world, origin, _impl->meters(to) - origin, Impl::query_filter(filter),
                    [](b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* raw) -> float {
                        auto& c = *static_cast<Context*>(raw);
                        if (!c.impl.matches(shape, c.filter)) return -1.0f;
                        c.hits.push_back({
                            .body = c.impl.handle_for(b2Shape_GetBody(shape)),
                            .fixture = c.impl.handle_for(shape),
                            .point = c.impl.units(point),
                            .normal = from_b2(normal),
                            .fraction = fraction,
                        });
                        return 1.0f;
                    },
                    &context);
    std::ranges::sort(context.hits, [](const PhysicsRaycastHit& a, const PhysicsRaycastHit& b) { return a.fraction < b.fraction; });
    std::vector<PhysicsRaycastHit> nearest;
    std::unordered_set<u64> seen;
    for (const PhysicsRaycastHit& hit : context.hits) {
        if (seen.insert(fixture_key(hit.fixture)).second) nearest.push_back(hit);
    }
    return nearest;
}

std::vector<PhysicsDebugShape> PhysicsWorld::debug_shapes(PhysicsFilter filter) const {
    std::vector<PhysicsDebugShape> shapes;
    for (const Impl::FixtureSlot& slot : _impl->fixtures) {
        if (!slot.alive) continue;
        // A chain: one line through its points (a loop's last its first again), as 2.4 drew it.
        for (b2ChainId chain : slot.chains) {
            if (!b2Chain_IsValid(chain)) continue;
            std::vector<b2ShapeId> segments(static_cast<std::size_t>(b2Chain_GetSegmentCount(chain)));
            b2Chain_GetSegments(chain, segments.data(), static_cast<int>(segments.size()));
            if (segments.empty() || !_impl->matches(segments[0], filter)) continue;
            const b2BodyId body = b2Shape_GetBody(segments[0]);
            const b2Transform xf = b2Body_GetTransform(body);
            PhysicsDebugShape debug{
                .type = PhysicsDebugShapeType::Chain,
                .body = _impl->handle_for(body),
                .fixture = _impl->handle_for(segments[0]),
                .body_type = from_b2(b2Body_GetType(body)),
                .sensor = false,
                .awake = b2Body_IsAwake(body),
            };
            debug.points.push_back(_impl->units(b2TransformPoint(xf, b2Shape_GetChainSegment(segments[0]).segment.point1)));
            for (b2ShapeId segment : segments) {
                debug.points.push_back(_impl->units(b2TransformPoint(xf, b2Shape_GetChainSegment(segment).segment.point2)));
            }
            shapes.push_back(std::move(debug));
        }
        for (b2ShapeId shape : slot.parts) {
            if (!b2Shape_IsValid(shape) || !B2_IS_NULL(b2Shape_GetParentChain(shape)) || !_impl->matches(shape, filter)) continue;
            const b2BodyId body = b2Shape_GetBody(shape);
            const b2Transform xf = b2Body_GetTransform(body);
            PhysicsDebugShape debug{
                .body = _impl->handle_for(body),
                .fixture = _impl->handle_for(shape),
                .body_type = from_b2(b2Body_GetType(body)),
                .sensor = b2Shape_IsSensor(shape),
                .awake = b2Body_IsAwake(body),
            };
            const auto world_point = [&](b2Vec2 p) { return _impl->units(b2TransformPoint(xf, p)); };
            switch (b2Shape_GetType(shape)) {
            case b2_polygonShape: {
                debug.type = PhysicsDebugShapeType::Polygon;
                const b2Polygon polygon = b2Shape_GetPolygon(shape);
                for (int i = 0; i < polygon.count; ++i) debug.points.push_back(world_point(polygon.vertices[i]));
                break;
            }
            case b2_circleShape: {
                debug.type = PhysicsDebugShapeType::Circle;
                const b2Circle circle = b2Shape_GetCircle(shape);
                debug.center = world_point(circle.center);
                debug.radius = _impl->units(circle.radius);
                break;
            }
            case b2_capsuleShape: { // its outline: two half circles and their sides
                debug.type = PhysicsDebugShapeType::Polygon;
                const b2Capsule capsule = b2Shape_GetCapsule(shape);
                const b2Vec2 along = b2Normalize(capsule.center2 - capsule.center1);
                const f32 base = std::atan2(along.y, along.x);
                for (int end = 0; end < 2; ++end) {
                    const b2Vec2 c = end == 0 ? capsule.center2 : capsule.center1;
                    for (int i = 0; i <= 6; ++i) {
                        const f32 a = base - 1.5707964f + static_cast<f32>(i) * 0.5235988f + (end == 0 ? 0.0f : 3.1415927f);
                        debug.points.push_back(world_point(c + capsule.radius * b2Vec2{std::cos(a), std::sin(a)}));
                    }
                }
                break;
            }
            case b2_segmentShape: {
                debug.type = PhysicsDebugShapeType::Chain;
                const b2Segment segment = b2Shape_GetSegment(shape);
                debug.points = {world_point(segment.point1), world_point(segment.point2)};
                break;
            }
            case b2_chainSegmentShape: {
                debug.type = PhysicsDebugShapeType::Chain;
                const b2ChainSegment segment = b2Shape_GetChainSegment(shape);
                debug.points = {world_point(segment.segment.point1), world_point(segment.segment.point2)};
                break;
            }
            default: continue;
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
        if (B2_IS_NULL(slot.joint) || !b2Joint_IsValid(slot.joint)) continue;
        const b2Vec2 a = b2Body_GetWorldPoint(b2Joint_GetBodyA(slot.joint), b2Joint_GetLocalFrameA(slot.joint).p);
        const b2Vec2 b = b2Body_GetWorldPoint(b2Joint_GetBodyB(slot.joint), b2Joint_GetLocalFrameB(slot.joint).p);
        joints.push_back({.joint = {i, slot.generation}, .type = slot.type, .anchor_a = _impl->units(b), .anchor_b = _impl->units(a)});
    }
    return joints;
}

Vec2f PhysicsWorld::point_velocity(PhysicsBody body, Vec2f point) const {
    const b2BodyId id = _impl->body_id(body);
    return B2_IS_NULL(id) ? Vec2f{} : _impl->units(b2Body_GetWorldPointVelocity(id, _impl->meters(point)));
}

std::vector<PhysicsFixture> PhysicsWorld::query_shape(const PhysicsShape& shape, Vec2f at, PhysicsFilter filter) const {
    std::vector<PhysicsFixture> found;
    const std::optional<Impl::Mover> mover = _impl->mover_of(shape, false);
    if (!mover) return found;
    const b2Vec2 where = _impl->meters(at);
    const b2ShapeProxy proxy = mover->proxy();
    std::unordered_set<u64> seen;
    for (const Impl::Part& part : _impl->parts_near(mover->aabb(where), filter, 0.0f, where)) {
        const Impl::Gap between = Impl::gap(proxy, where, part.proxy, part.xf);
        // Overlapping, not touching: past the shape's own outline.
        if (between.inside || between.distance + part.skin < -0.25f * linear_slop) {
            const PhysicsFixture fixture = _impl->handle_for(part.shape);
            if (fixture && seen.insert(fixture_key(fixture)).second) found.push_back(fixture);
        }
    }
    return found;
}

std::optional<PhysicsShapeHit> PhysicsWorld::cast_shape(const PhysicsShape& shape, Vec2f from, Vec2f to, PhysicsFilter filter) const {
    const std::optional<Impl::Mover> mover = _impl->mover_of(shape, false);
    if (!mover) return std::nullopt;
    const b2Vec2 start = _impl->meters(from);
    const b2Vec2 motion = _impl->meters(to) - start;
    b2AABB box = mover->aabb(start);
    const b2AABB end = mover->aabb(start + motion);
    box.lowerBound = b2Min(box.lowerBound, end.lowerBound);
    box.upperBound = b2Max(box.upperBound, end.upperBound);
    const b2ShapeProxy proxy = mover->proxy();
    std::optional<PhysicsShapeHit> nearest;
    for (const Impl::Part& part : _impl->parts_near(box, filter, 0.0f, start)) {
        if (!Impl::stops(part, *mover, start, motion, true)) continue;
        // Up to the shape's own outline.
        const auto hit = Impl::sweep(proxy, start, motion, part.proxy, part.xf, -part.skin);
        if (hit && (!nearest || hit->first < nearest->fraction)) {
            nearest = PhysicsShapeHit{
                .body = _impl->handle_for(b2Shape_GetBody(part.shape)),
                .fixture = _impl->handle_for(part.shape),
                .point = _impl->units(hit->second.point - part.skin * hit->second.normal),
                .normal = from_b2(hit->second.normal),
                .fraction = hit->first,
            };
        }
    }
    return nearest;
}

PhysicsCharacterMove PhysicsWorld::move_character(const PhysicsCharacterDef& def, Vec2f position, Vec2f velocity, f32 dt, bool on_floor,
                                                  Vec2f carry) {
    Impl& impl = *_impl;
    PhysicsCharacterMove result{.position = position, .velocity = velocity};
    const std::optional<Impl::Mover> made = impl.mover_of(def.shape, true);
    if (!made || !std::isfinite(dt) || dt < 0.0f) return result;
    const Impl::Mover& mover = *made;
    const b2ShapeProxy proxy = mover.proxy();
    b2Vec2 up{def.up.x, def.up.y};
    if (!(b2Length(up) > 0.0f)) up = {0.0f, -1.0f};
    up = b2Normalize(up);
    const f32 floor_cos = std::cos(std::clamp(def.max_slope, 0.0f, 1.5f));
    const f32 target = def.skin > 0.0f ? impl.meters(def.skin) : 0.02f;
    const f32 snap = std::max(impl.meters(def.snap), 0.0f);
    PhysicsFilter filter = def.filter;
    filter.include_sensors = false;

    b2Vec2 at = impl.meters(position);
    b2Vec2 speed = impl.meters(velocity);
    const b2Vec2 motion = dt * speed + impl.meters(carry);
    const bool rising = b2Dot(speed, up) > 0.0f;

    const f32 reach = b2Length(motion) + snap + 4.0f * target;
    b2AABB box = mover.aabb(at);
    box.lowerBound = box.lowerBound - b2Vec2{reach, reach};
    box.upperBound = box.upperBound + b2Vec2{reach, reach};
    const std::vector<Impl::Part> parts = impl.parts_near(box, filter, dt, at);

    impl.recover(mover, at, parts, target, def.one_way);

    const auto record = [&](const Impl::Part& part, const Impl::Gap& touch, f32 fraction) {
        result.hits.push_back({
            .body = impl.handle_for(b2Shape_GetBody(part.shape)),
            .fixture = impl.handle_for(part.shape),
            .point = impl.units(touch.point - part.skin * touch.normal),
            .normal = from_b2(touch.normal),
            .fraction = fraction,
        });
    };
    const auto along_floor = [](b2Vec2 across, b2Vec2 normal) {
        const f32 length = b2Length(across);
        b2Vec2 tangent{-normal.y, normal.x};
        if (b2Dot(tangent, across) < 0.0f) tangent = -tangent;
        return length * tangent;
    };

    b2Vec2 remaining = motion;
    f32 elapsed = 0.0f;
    bool floor_met = false;
    for (i32 slide = 0; slide < std::max(def.max_slides, 1) && b2LengthSquared(remaining) > 1e-12f; ++slide) {
        const f32 time_left = 1.0f - elapsed;
        f32 nearest = 1.0f;
        const Impl::Part* hit_part = nullptr;
        Impl::Gap hit_gap;
        for (const Impl::Part& part : parts) {
            const b2Vec2 relative = remaining - time_left * part.shift;
            const b2Vec2 lift = elapsed * part.shift;
            if (!Impl::stops(part, mover, at, relative, def.one_way, lift)) continue;
            b2Transform xf = part.xf;
            xf.p += lift;
            const auto hit = Impl::sweep(proxy, at, relative, part.proxy, xf, target);
            if (hit && hit->first < nearest) {
                nearest = hit->first;
                hit_part = &part;
                hit_gap = hit->second;
            }
        }
        if (!hit_part) {
            at += remaining;
            elapsed = 1.0f;
            break;
        }
        at += nearest * remaining;
        elapsed += time_left * nearest;
        record(*hit_part, hit_gap, nearest);

        const b2Vec2 normal = hit_gap.normal;
        const b2Vec2 part_left = (1.0f - elapsed) * hit_part->shift;
        b2Vec2 relative = (1.0f - nearest) * (remaining - time_left * hit_part->shift);
        const f32 rise = b2Dot(normal, up);

        if (def.push > 0.0f && hit_part->dynamic) {
            const b2BodyId body = b2Shape_GetBody(hit_part->shape);
            const b2Vec2 into = -normal;
            const f32 wanted = def.push * b2Dot(speed, into);
            const f32 has = b2Dot(b2Body_GetWorldPointVelocity(body, hit_gap.point), into);
            if (wanted > has) b2Body_ApplyLinearImpulse(body, b2Body_GetMass(body) * (wanted - has) * into, hit_gap.point, true);
        }

        if (rise >= floor_cos) {
            floor_met = true;
            result.on_floor = true;
            result.floor_normal = from_b2(normal);
            result.floor = impl.handle_for(b2Shape_GetBody(hit_part->shape));
            if (b2Dot(speed, up) < 0.0f) speed = speed - b2Dot(speed, up) * up;
            const b2Vec2 across = relative - b2Dot(relative, up) * up;
            if (def.stop_on_slope) {
                relative = b2LengthSquared(across) > 1e-12f ? along_floor(across, normal) : b2Vec2{0.0f, 0.0f};
            } else if (const f32 into = b2Dot(relative, normal); into < 0.0f) {
                relative = relative - into * normal;
            }
        } else {
            if (-rise >= floor_cos) {
                result.on_ceiling = true;
            } else {
                result.on_wall = true;
                result.wall_normal = from_b2(normal);
            }
            if (const f32 into = b2Dot(relative, normal); into < 0.0f) relative = relative - into * normal;
            if (const f32 into = b2Dot(speed, normal); into < 0.0f) speed = speed - into * normal;
            if (!result.on_ceiling && (floor_met || on_floor) && !rising) {
                if (const f32 lift = b2Dot(relative, up); lift > 0.0f) relative = relative - lift * up;
                if (const f32 lift = b2Dot(speed, up); lift > 0.0f) speed = speed - lift * up;
            }
        }
        remaining = relative + part_left;
    }

    if (!floor_met) {
        const bool snapping = on_floor && snap > 0.0f && b2Dot(speed, up) <= 0.0f;
        const b2Vec2 probe = -(target + (snapping ? snap : 0.0f)) * up;
        f32 nearest = 1.0f;
        const Impl::Part* hit_part = nullptr;
        Impl::Gap hit_gap;
        for (const Impl::Part& part : parts) {
            const b2Vec2 lift = part.shift;
            if (!Impl::stops(part, mover, at, probe, def.one_way, lift)) continue;
            b2Transform xf = part.xf;
            xf.p += lift;
            const auto hit = Impl::sweep(proxy, at, probe, part.proxy, xf, target);
            if (hit && hit->first < nearest && b2Dot(hit->second.normal, up) >= floor_cos) {
                nearest = hit->first;
                hit_part = &part;
                hit_gap = hit->second;
            }
        }
        if (hit_part) {
            at += nearest * probe;
            result.on_floor = true;
            result.floor_normal = from_b2(hit_gap.normal);
            result.floor = impl.handle_for(b2Shape_GetBody(hit_part->shape));
            if (b2Dot(speed, up) < 0.0f) speed = speed - b2Dot(speed, up) * up;
        }
    }

    result.position = impl.units(at);
    result.velocity = impl.units(speed);
    return result;
}

f32 PhysicsWorld::units_per_meter() const {
    return _impl->scale;
}

} // namespace kin
