#include <kin/ecs/physics.hpp>

#include <kin/ecs/tilemap.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kin {
namespace {

struct PhysicsHandleKey {
    u32 index = 0;
    u32 generation = 0;

    explicit PhysicsHandleKey(PhysicsBody body)
        : index(body.index),
          generation(body.generation) {
    }

    friend bool operator==(PhysicsHandleKey, PhysicsHandleKey) = default;
};

struct PhysicsHandleKeyHash {
    std::size_t operator()(PhysicsHandleKey key) const {
        return (static_cast<std::size_t>(key.index) << 32) ^ static_cast<std::size_t>(key.generation);
    }
};

using BodyEntityMap = std::unordered_map<PhysicsHandleKey, flecs::entity, PhysicsHandleKeyHash>;

BodyEntityMap build_body_entity_map(flecs::world& world, PhysicsWorld& physics) {
    BodyEntityMap map;
    world.query<PhysicsBodyComponent>().each([&](flecs::entity entity, const PhysicsBodyComponent& body) {
        if (physics.is_alive(body.body)) {
            map.emplace(PhysicsHandleKey{body.body}, entity);
        }
    });
    return map;
}

void push_event_for_body(const BodyEntityMap& map, PhysicsBody body, const PhysicsContactEvent& event) {
    const auto found = map.find(PhysicsHandleKey{body});
    if (found == map.end() || !found->second.is_alive()) {
        return;
    }
    if (PhysicsContactQueue* queue = found->second.get_mut<PhysicsContactQueue>()) {
        queue->events.push_back(event);
    }
}

void push_contact_for_body(const BodyEntityMap& map, PhysicsBody body, const PhysicsContact& contact) {
    const auto found = map.find(PhysicsHandleKey{body});
    if (found == map.end() || !found->second.is_alive()) {
        return;
    }
    if (PhysicsOverlapSet* overlaps = found->second.get_mut<PhysicsOverlapSet>()) {
        overlaps->contacts.push_back(contact);
    }
}

// Transform2D turns in degrees, like the rest of kin; the physics world in
// radians, like Box2D. The same sense on screen (y down): only the unit differs.
constexpr f32 radians_per_degree = 3.14159265358979323846f / 180.0f;

f32 to_physics_angle(f32 degrees) {
    return degrees * radians_per_degree;
}

f32 from_physics_angle(f32 radians) {
    return radians / radians_per_degree;
}

// Physics works in the world; Transform2D is relative to the entity's parent.
// The entity's pose in the world now (not the WorldTransform of the last
// render, which may be a frame old).
WorldTransform world_pose(flecs::entity entity, const Transform2D& transform) {
    if (!entity.parent()) {
        return {transform.pos, transform.rotation, transform.scale};
    }
    return current_world_transform(entity);
}

i32 depth_of(flecs::entity entity) {
    i32 depth = 0;
    for (flecs::entity parent = entity.parent(); parent; parent = parent.parent()) {
        ++depth;
    }
    return depth;
}

// An entity that has T from a prefab (IsA) gets its own copy, as `fresh`
// makes it: a body, its fixtures and its contacts are the entity's alone.
template <typename T, typename Fresh>
void own_inherited(flecs::world& world, Fresh fresh) {
    if (!world.template component<T>().has(flecs::OnInstantiate, flecs::Inherit)) {
        return; // copied onto instances when they are made (flecs will not look up for it)
    }
    std::vector<std::pair<flecs::entity, T>> inherited;
    world.template query_builder<const T>().term_at(0).up(flecs::IsA).build().each([&](flecs::entity entity, const T& value) {
        if (!entity.template owns<T>()) { // matched by its prefab's even once it has its own
            inherited.emplace_back(entity, value);
        }
    });
    for (auto& [entity, value] : inherited) {
        fresh(value);
        entity.set(std::move(value));
    }
}

void own_inherited_physics(flecs::world& world) {
    own_inherited<PhysicsBodyComponent>(world, [](PhysicsBodyComponent& body) { body.body = {}; });
    own_inherited<PhysicsColliderComponent>(world, [](PhysicsColliderComponent& colliders) {
        for (PhysicsCollider& collider : colliders.colliders) {
            collider.fixture = {};
        }
        colliders.body = {};
    });
    own_inherited<PhysicsJointComponent>(world, [](PhysicsJointComponent& joint) { joint.joint = {}; });
    own_inherited<TilemapCollider>(world, [](TilemapCollider& collider) {
        collider.body = {};
        collider.built_from.reset();
    });
    own_inherited<PhysicsContactQueue>(world, [](PhysicsContactQueue& queue) { queue.events.clear(); });
    own_inherited<PhysicsOverlapSet>(world, [](PhysicsOverlapSet& overlaps) { overlaps.contacts.clear(); });
}

// Internal flecs-typed implementations. The public API (further down) takes
// EcsWorld& and forwards via world.raw(), keeping flecs out of the
// kin/ecs/physics.hpp surface.

u64 fixture_key(PhysicsFixture fixture) {
    return (static_cast<u64>(fixture.index) << 32) | fixture.generation;
}

Vec2f rotated(Vec2f v, f32 radians) {
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

// A point of an entity (as its children are placed), in the world.
Vec2f world_point(const WorldTransform& pose, Vec2f point) {
    const Vec2f scaled{point.x * pose.scale.x, point.y * pose.scale.y};
    const Vec2f turned = rotated(scaled, to_physics_angle(pose.rotation));
    return {pose.pos.x + turned.x, pose.pos.y + turned.y};
}

// Where `child` is from `owner` as its body sees it: bodies are not scaled,
// so the child's scale is all its own and its parents'.
WorldTransform pose_on_body(const WorldTransform& owner, const WorldTransform& child) {
    const Vec2f along{child.pos.x - owner.pos.x, child.pos.y - owner.pos.y};
    return {rotated(along, -to_physics_angle(owner.rotation)), child.rotation - owner.rotation, child.scale};
}

bool same_pose(const WorldTransform& a, const WorldTransform& b) {
    constexpr f32 near = 1e-4f;
    return std::abs(a.pos.x - b.pos.x) <= near && std::abs(a.pos.y - b.pos.y) <= near &&
           std::abs(a.rotation - b.rotation) <= near && std::abs(a.scale.x - b.scale.x) <= near &&
           std::abs(a.scale.y - b.scale.y) <= near;
}

// `shape`, of an entity at `at` from its body: on the body.
PhysicsShape placed_shape(PhysicsShape shape, const WorldTransform& at) {
    const f32 turn = to_physics_angle(at.rotation);
    const Vec2f scale = at.scale;
    const f32 most = std::max(std::abs(scale.x), std::abs(scale.y));
    const bool uniform = std::abs(std::abs(scale.x) - std::abs(scale.y)) <= 1e-4f * most;
    const bool mirrored = (scale.x < 0.0f) != (scale.y < 0.0f);
    const auto place = [&](Vec2f point) {
        const Vec2f turned = rotated({point.x * scale.x, point.y * scale.y}, turn);
        return Vec2f{at.pos.x + turned.x, at.pos.y + turned.y};
    };
    // Its points, on the body (as polygons and chains have them).
    const auto bake = [&](const std::vector<Vec2f>& points) {
        std::vector<Vec2f> baked;
        baked.reserve(points.size());
        for (Vec2f point : points) {
            const Vec2f turned = rotated(point, shape.angle);
            baked.push_back(place({shape.offset.x + turned.x, shape.offset.y + turned.y}));
        }
        if (mirrored) {
            std::ranges::reverse(baked); // its sides the same way round
        }
        return baked;
    };
    switch (shape.type) {
    case PhysicsShapeType::Circle:
        shape.offset = place(shape.offset);
        shape.radius *= most;
        break;
    case PhysicsShapeType::Box:
        if (!uniform && shape.angle != 0.0f) {
            // Turned, then stretched: no longer a box.
            const Vec2f h = shape.half_extents;
            shape.points = bake({{-h.x, -h.y}, {h.x, -h.y}, {h.x, h.y}, {-h.x, h.y}});
            shape.type = PhysicsShapeType::Polygon;
            shape.offset = {};
            shape.angle = 0.0f;
            break;
        }
        [[fallthrough]];
    case PhysicsShapeType::Capsule:
        shape.half_extents = uniform || shape.angle == 0.0f
            ? Vec2f{std::abs(shape.half_extents.x * scale.x), std::abs(shape.half_extents.y * scale.y)}
            : Vec2f{std::abs(shape.half_extents.x) * most, std::abs(shape.half_extents.y) * most};
        shape.offset = place(shape.offset);
        shape.angle = turn + (mirrored ? -shape.angle : shape.angle);
        break;
    case PhysicsShapeType::Polygon:
    case PhysicsShapeType::Chain:
        shape.points = bake(shape.points);
        shape.offset = {};
        shape.angle = 0.0f;
        break;
    }
    return shape;
}

// The nearest ancestor with a body.
flecs::entity body_above(flecs::entity entity) {
    for (flecs::entity parent = entity.parent(); parent; parent = parent.parent()) {
        if (parent.has<PhysicsBodyComponent>()) {
            return parent;
        }
    }
    return {};
}

void sync_bodies_and_colliders_raw(flecs::world& world, PhysicsWorld& physics) {
    own_inherited_physics(world);
    std::unordered_set<u64> claimed; // the fixtures colliders have
    world.query<PhysicsBodyComponent>().each([&](flecs::entity entity, PhysicsBodyComponent& body) {
        if (!physics.is_alive(body.body)) {
            PhysicsBodyDef def = body.def;
            if (const Transform2D* transform = entity.get<Transform2D>()) {
                const WorldTransform pose = world_pose(entity, *transform);
                def.position = pose.pos;
                def.rotation = to_physics_angle(pose.rotation);
                if (pose.scale != Vec2f{1.0f, 1.0f}) {
                    static bool warned = false;
                    if (!std::exchange(warned, true)) {
                        KIN_LOG_WARN("physics", "a physics body's entity is scaled; bodies ignore scale (size the colliders instead)");
                    }
                }
            }
            if (def.user_id == 0) {
                def.user_id = static_cast<u64>(entity.id());
            }
            body.body = physics.create_body(def);
        }

        PhysicsColliderComponent* collider_component = entity.get_mut<PhysicsColliderComponent>();
        if (!collider_component) {
            return;
        }
        collider_component->body = {};
        for (PhysicsCollider& collider : collider_component->colliders) {
            if (!physics.is_alive(collider.fixture)) {
                collider.fixture = physics.add_fixture(body.body, collider.def);
            }
            claimed.insert(fixture_key(collider.fixture));
        }
    });

    // Colliders of entities with no body: on the body above them.
    world.query_builder<PhysicsColliderComponent>().without<PhysicsBodyComponent>().build().each(
        [&](flecs::entity entity, PhysicsColliderComponent& colliders) {
            const flecs::entity owner = body_above(entity);
            const PhysicsBodyComponent* body = owner ? owner.get<PhysicsBodyComponent>() : nullptr;
            const PhysicsBody target = body && physics.is_alive(body->body) ? body->body : PhysicsBody{};
            const WorldTransform at = target ? pose_on_body(current_world_transform(owner), current_world_transform(entity)) : WorldTransform{};
            const bool moved = colliders.body != target || !same_pose(colliders.placed, at);
            for (PhysicsCollider& collider : colliders.colliders) {
                if (moved || !target) {
                    physics.destroy_fixture(collider.fixture);
                    collider.fixture = {};
                }
                if (target && !physics.is_alive(collider.fixture)) {
                    PhysicsFixtureDef def = collider.def;
                    def.shape = placed_shape(def.shape, at);
                    collider.fixture = physics.add_fixture(target, def);
                }
                claimed.insert(fixture_key(collider.fixture));
            }
            colliders.body = target;
            colliders.placed = at;
        });

    // A fixture no collider has (one taken out of a component's list) goes.
    world.query<const PhysicsBodyComponent>().each([&](const PhysicsBodyComponent& body) {
        for (PhysicsFixture fixture : physics.fixtures(body.body)) {
            if (!claimed.contains(fixture_key(fixture))) {
                physics.destroy_fixture(fixture);
            }
        }
    });
}

// The joint as the physics world takes it (angles in radians).
PhysicsJointDef joint_def(const PhysicsJointComponent& joint) {
    const bool angular = joint.type == PhysicsJointType::Hinge;
    const bool turning = angular || joint.type == PhysicsJointType::Wheel;
    return {
        .type = joint.type,
        .collide_connected = joint.collide_connected,
        .limit = joint.limit,
        .lower = angular ? to_physics_angle(joint.lower) : joint.lower,
        .upper = angular ? to_physics_angle(joint.upper) : joint.upper,
        .motor = joint.motor,
        .motor_speed = turning ? to_physics_angle(joint.motor_speed) : joint.motor_speed,
        .max_motor_force = joint.max_motor_force,
        .length = joint.length,
        .frequency = joint.frequency,
        .damping_ratio = joint.damping_ratio,
    };
}

bool same_tuning(const PhysicsJointDef& a, const PhysicsJointDef& b) {
    return a.limit == b.limit && a.lower == b.lower && a.upper == b.upper && a.motor == b.motor &&
           a.motor_speed == b.motor_speed && a.max_motor_force == b.max_motor_force && a.length == b.length &&
           a.frequency == b.frequency && a.damping_ratio == b.damping_ratio;
}

void sync_physics_joints_raw(flecs::world& world, PhysicsWorld& physics) {
    world.query<PhysicsJointComponent>().each([&](flecs::entity entity, PhysicsJointComponent& joint) {
        const PhysicsBodyComponent* body = entity.get<PhysicsBodyComponent>();
        if (!body || !physics.is_alive(body->body)) {
            physics.destroy_joint(joint.joint);
            joint.joint = {};
            return;
        }
        // The other end: an entity with a body, or none (the world).
        PhysicsBody other_body{};
        flecs::entity other;
        if (joint.other.id != 0) {
            other = flecs::entity(world, static_cast<flecs::entity_t>(joint.other.id));
            const PhysicsBodyComponent* other_component = other.is_alive() ? other.get<PhysicsBodyComponent>() : nullptr;
            if (!other_component || !physics.is_alive(other_component->body)) {
                physics.destroy_joint(joint.joint);
                joint.joint = {};
                return; // not yet (or not one to join to)
            }
            other_body = other_component->body;
        }
        const PhysicsJointComponent::Made& made = joint.made;
        const bool same = physics.is_alive(joint.joint) && made.def.body_a == body->body && made.def.body_b == other_body &&
                          made.type == joint.type && made.other == joint.other.id && made.anchor == joint.anchor &&
                          made.other_anchor == joint.other_anchor && made.axis == joint.axis &&
                          made.collide_connected == joint.collide_connected;
        if (same) {
            const PhysicsJointDef tuning = joint_def(joint);
            if (!same_tuning(tuning, made.def)) {
                physics.update_joint(joint.joint, tuning);
                joint.made.def = {.type = made.def.type,
                                  .body_a = made.def.body_a,
                                  .body_b = made.def.body_b,
                                  .anchor = made.def.anchor,
                                  .other_anchor = made.def.other_anchor,
                                  .axis = made.def.axis,
                                  .collide_connected = made.def.collide_connected,
                                  .limit = tuning.limit,
                                  .lower = tuning.lower,
                                  .upper = tuning.upper,
                                  .motor = tuning.motor,
                                  .motor_speed = tuning.motor_speed,
                                  .max_motor_force = tuning.max_motor_force,
                                  .length = tuning.length,
                                  .frequency = tuning.frequency,
                                  .damping_ratio = tuning.damping_ratio,
                                  .user_id = made.def.user_id};
            }
            return;
        }
        physics.destroy_joint(joint.joint);
        joint.joint = {};
        if (joint.broken) {
            return;
        }
        const WorldTransform pose = current_world_transform(entity);
        PhysicsJointDef def = joint_def(joint);
        def.body_a = body->body;
        def.body_b = other_body;
        def.anchor = world_point(pose, joint.anchor);
        def.other_anchor = other ? world_point(current_world_transform(other), joint.other_anchor) : world_point(pose, joint.other_anchor);
        def.axis = rotated(joint.axis, to_physics_angle(pose.rotation));
        def.user_id = static_cast<u64>(entity.id());
        joint.joint = physics.create_joint(def);
        joint.made = {.type = joint.type,
                      .other = joint.other.id,
                      .anchor = joint.anchor,
                      .other_anchor = joint.other_anchor,
                      .axis = joint.axis,
                      .collide_connected = joint.collide_connected,
                      .def = def};
    });
}

void sync_tilemap_colliders_raw(flecs::world& world, PhysicsWorld& physics) {
    world.query_builder<TilemapCollider>().without<TilemapComponent>().build().each([&](TilemapCollider& collider) {
        physics.destroy_body(collider.body);
        collider.body = {};
        collider.built_from.reset();
    });
    world.query<const TilemapComponent, TilemapCollider>().each(
        [&](flecs::entity entity, const TilemapComponent& tiles, TilemapCollider& collider) {
            const Vec2f at = current_world_transform(entity).pos; // maps are not turned or scaled
            if (physics.is_alive(collider.body) && collider.built_from == tiles.tilemap) {
                const Vec2f now = physics.position(collider.body);
                if (std::abs(now.x - at.x) > 1e-3f || std::abs(now.y - at.y) > 1e-3f) {
                    physics.set_transform(collider.body, at, 0.0f);
                }
                return;
            }
            physics.destroy_body(collider.body);
            collider.body = {};
            collider.built_from = tiles.tilemap;
            if (!tiles.tilemap) {
                return;
            }
            collider.body = physics.create_body({.type = PhysicsBodyType::Static, .position = at, .user_id = static_cast<u64>(entity.id())});
            const TileCollisionShapes shapes = tiles.tilemap->map.collision_shapes(collider.layer);
            PhysicsFixtureDef def{
                .friction = collider.friction,
                .restitution = collider.restitution,
                .sensor = collider.sensor,
                .category_bits = collider.category_bits,
                .mask_bits = collider.mask_bits,
            };
            for (const std::vector<Vec2f>& loop : shapes.loops) {
                def.shape = PhysicsShape::chain(loop, true);
                def.shape.one_sided = true; // they stop what comes from outside
                physics.add_fixture(collider.body, def);
            }
            for (const std::vector<Vec2f>& polygon : shapes.polygons) {
                def.shape = PhysicsShape::polygon(polygon);
                physics.add_fixture(collider.body, def);
            }
        });
}

// Bodies and their shapes, tilemaps', then joints (between bodies made, and
// weighed: a spring's stiffness is from its bodies' masses).
void sync_physics_bodies_raw(flecs::world& world, PhysicsWorld& physics) {
    sync_bodies_and_colliders_raw(world, physics);
    sync_tilemap_colliders_raw(world, physics);
    sync_physics_joints_raw(world, physics);
}

void break_physics_joints_raw(flecs::world& world, PhysicsWorld& physics) {
    world.query<PhysicsJointComponent>().each([&](PhysicsJointComponent& joint) {
        if (joint.break_force > 0.0f && physics.is_alive(joint.joint)) {
            const Vec2f force = physics.joint_force(joint.joint);
            if (std::hypot(force.x, force.y) > joint.break_force) {
                physics.destroy_joint(joint.joint);
                joint.joint = {};
                joint.broken = true;
            }
        }
    });
}

void sync_transforms_to_physics_raw(flecs::world& world, PhysicsWorld& physics) {
    // The entity leads: a body under a parent follows it. One teleported goes
    // where its entity is, once.
    std::vector<flecs::entity> teleported;
    world.query<PhysicsBodyComponent, const Transform2D>().each(
        [&](flecs::entity entity, PhysicsBodyComponent& body, const Transform2D& transform) {
            const bool teleport = entity.has<PhysicsTeleport>();
            if (teleport) {
                teleported.push_back(entity);
            }
            if ((body.sync_to_physics || teleport) && physics.is_alive(body.body)) {
                const WorldTransform pose = world_pose(entity, transform);
                physics.set_transform(body.body, pose.pos, to_physics_angle(pose.rotation));
            }
        });
    for (flecs::entity entity : teleported) {
        entity.remove<PhysicsTeleport>();
    }
}

void step_physics_raw(flecs::world& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations, i32 position_iterations) {
    world.query<PhysicsBodyComponent, const PhysicsVelocity>().each([&](const PhysicsBodyComponent& body, const PhysicsVelocity& velocity) {
        if (physics.is_alive(body.body)) {
            physics.set_linear_velocity(body.body, velocity.linear);
        }
    });
    physics.step(dt, velocity_iterations, position_iterations);
    break_physics_joints_raw(world, physics);
}

void sync_transforms_from_physics_raw(flecs::world& world, PhysicsWorld& physics) {
    // The simulation leads: a body stays where it is in the world when its
    // parent moves, and its Transform2D says where that is from the parent.
    // Bodies under a parent are written parents first, so each is placed
    // against where its parent has just been put.
    struct Placed {
        i32 depth = 0;
        flecs::entity entity;
        WorldTransform pose;
    };
    std::vector<Placed> parented;
    world.query<PhysicsBodyComponent, Transform2D>().each(
        [&](flecs::entity entity, const PhysicsBodyComponent& body, Transform2D& transform) {
            if (!body.sync_from_physics || !physics.is_alive(body.body)) {
                return;
            }
            const Vec2f pos = physics.position(body.body);
            const f32 rotation = from_physics_angle(physics.rotation(body.body));
            if (!entity.parent()) {
                transform.pos = pos;
                transform.rotation = rotation;
                return;
            }
            parented.push_back({depth_of(entity), entity, {pos, rotation, {1.0f, 1.0f}}});
        });
    std::stable_sort(parented.begin(), parented.end(), [](const Placed& a, const Placed& b) { return a.depth < b.depth; });
    for (const Placed& placed : parented) {
        Transform2D* transform = placed.entity.get_mut<Transform2D>();
        const Transform2D local = to_local(current_world_transform(placed.entity.parent()), placed.pose);
        transform->pos = local.pos;
        transform->rotation = local.rotation; // its scale stays its own
    }
}

void collect_physics_contacts_raw(flecs::world& world, PhysicsWorld& physics) {
    world.query<PhysicsContactQueue>().each([](PhysicsContactQueue& queue) {
        queue.events.clear();
    });
    world.query<PhysicsOverlapSet>().each([](PhysicsOverlapSet& overlaps) {
        overlaps.contacts.clear();
    });

    const BodyEntityMap map = build_body_entity_map(world, physics);

    for (const PhysicsContactEvent& event : physics.contacts()) {
        push_event_for_body(map, event.body_a, event);
        push_event_for_body(map, event.body_b, event);
    }

    for (const PhysicsContact& contact : physics.current_contacts()) {
        push_contact_for_body(map, contact.body_a, contact);
        push_contact_for_body(map, contact.body_b, contact);
    }
}

} // namespace

EcsPhysicsBridge::EcsPhysicsBridge(EcsWorld& ecs, PhysicsWorld& physics)
    : _remove_body_observer(ecs.raw().observer<PhysicsBodyComponent>("destroy_physics_body_on_remove")
                                .event(flecs::OnRemove)
                                .each([&physics](flecs::entity, PhysicsBodyComponent& body) {
                                    if (physics.is_alive(body.body)) {
                                        physics.destroy_body(body.body);
                                    }
                                    body.body = {};
                                })),
      _remove_collider_observer(ecs.raw().observer<PhysicsColliderComponent>("destroy_physics_fixtures_on_remove")
                                    .event(flecs::OnRemove)
                                    .each([&physics](flecs::entity, PhysicsColliderComponent& colliders) {
                                        // Its own body's, or the body above it's.
                                        for (PhysicsCollider& collider : colliders.colliders) {
                                            physics.destroy_fixture(collider.fixture);
                                            collider.fixture = {};
                                        }
                                        colliders.body = {};
                                    })),
      _remove_joint_observer(ecs.raw().observer<PhysicsJointComponent>("destroy_physics_joint_on_remove")
                                 .event(flecs::OnRemove)
                                 .each([&physics](flecs::entity, PhysicsJointComponent& joint) {
                                     physics.destroy_joint(joint.joint);
                                     joint.joint = {};
                                 })),
      _remove_tilemap_observer(ecs.raw().observer<TilemapCollider>("destroy_tilemap_body_on_remove")
                                   .event(flecs::OnRemove)
                                   .each([&physics](flecs::entity, TilemapCollider& collider) {
                                       physics.destroy_body(collider.body);
                                       collider.body = {};
                                       collider.built_from.reset();
                                   })) {
}

EcsPhysicsBridge::~EcsPhysicsBridge() {
    release();
}

EcsPhysicsBridge::EcsPhysicsBridge(EcsPhysicsBridge&& other) noexcept
    : _remove_body_observer(std::exchange(other._remove_body_observer, flecs::observer{})),
      _remove_collider_observer(std::exchange(other._remove_collider_observer, flecs::observer{})),
      _remove_joint_observer(std::exchange(other._remove_joint_observer, flecs::observer{})),
      _remove_tilemap_observer(std::exchange(other._remove_tilemap_observer, flecs::observer{})) {
}

EcsPhysicsBridge& EcsPhysicsBridge::operator=(EcsPhysicsBridge&& other) noexcept {
    if (this != &other) {
        release();
        _remove_body_observer = std::exchange(other._remove_body_observer, flecs::observer{});
        _remove_collider_observer = std::exchange(other._remove_collider_observer, flecs::observer{});
        _remove_joint_observer = std::exchange(other._remove_joint_observer, flecs::observer{});
        _remove_tilemap_observer = std::exchange(other._remove_tilemap_observer, flecs::observer{});
    }
    return *this;
}

void EcsPhysicsBridge::release() {
    for (flecs::observer* observer : {&_remove_body_observer, &_remove_collider_observer, &_remove_joint_observer, &_remove_tilemap_observer}) {
        if (observer->id() != 0 && observer->is_alive()) {
            observer->destruct();
        }
        *observer = flecs::observer{};
    }
}

void register_physics_components(EcsWorld& world) {
    flecs::world& raw = world.raw();
    raw.component<PhysicsBodyComponent>("PhysicsBodyComponent");
    raw.component<PhysicsColliderComponent>("PhysicsColliderComponent");
    raw.component<PhysicsJointComponent>("PhysicsJointComponent");
    raw.component<TilemapCollider>("TilemapCollider");
    raw.component<PhysicsVelocity>("PhysicsVelocity");
    raw.component<PhysicsTeleport>("PhysicsTeleport");
    raw.component<PhysicsContactQueue>("PhysicsContactQueue");
    raw.component<PhysicsOverlapSet>("PhysicsOverlapSet");
}

void sync_physics_bodies(EcsWorld& world, PhysicsWorld& physics) {
    sync_physics_bodies_raw(world.raw(), physics);
}

void sync_transforms_to_physics(EcsWorld& world, PhysicsWorld& physics) {
    sync_transforms_to_physics_raw(world.raw(), physics);
}

void step_physics(EcsWorld& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations, i32 position_iterations) {
    step_physics_raw(world.raw(), physics, dt, velocity_iterations, position_iterations);
}

void sync_transforms_from_physics(EcsWorld& world, PhysicsWorld& physics) {
    sync_transforms_from_physics_raw(world.raw(), physics);
}

void collect_physics_contacts(EcsWorld& world, PhysicsWorld& physics) {
    collect_physics_contacts_raw(world.raw(), physics);
}

void update_physics_world(EcsWorld& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations, i32 position_iterations) {
    flecs::world& raw = world.raw();
    sync_physics_bodies_raw(raw, physics);
    sync_transforms_to_physics_raw(raw, physics);
    step_physics_raw(raw, physics, dt, velocity_iterations, position_iterations);
    sync_transforms_from_physics_raw(raw, physics);
    collect_physics_contacts_raw(raw, physics);
}

} // namespace kin
