#include <kin/ecs/physics.hpp>

#include <algorithm>
#include <unordered_map>

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

// Internal flecs-typed implementations. The public API (further down) takes
// EcsWorld& and forwards via world.raw(), keeping flecs out of the
// kin/ecs/physics.hpp surface.

void sync_physics_bodies_raw(flecs::world& world, PhysicsWorld& physics) {
    world.query<PhysicsBodyComponent>().each([&](flecs::entity entity, PhysicsBodyComponent& body) {
        if (!physics.is_alive(body.body)) {
            PhysicsBodyDef def = body.def;
            if (const Transform2D* transform = entity.get<Transform2D>()) {
                def.position = transform->pos;
                def.rotation = to_physics_angle(transform->rotation);
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

        for (PhysicsCollider& collider : collider_component->colliders) {
            if (!physics.is_alive(collider.fixture)) {
                collider.fixture = physics.add_fixture(body.body, collider.def);
            }
        }

        const std::vector<PhysicsFixture> body_fixtures = physics.fixtures(body.body);
        for (PhysicsFixture fixture : body_fixtures) {
            const bool still_authored = std::ranges::any_of(collider_component->colliders, [&](const PhysicsCollider& collider) {
                return collider.fixture == fixture;
            });
            if (!still_authored) {
                physics.destroy_fixture(fixture);
            }
        }
    });
}

void sync_transforms_to_physics_raw(flecs::world& world, PhysicsWorld& physics) {
    world.query<PhysicsBodyComponent, const Transform2D>().each([&](PhysicsBodyComponent& body, const Transform2D& transform) {
        if (body.sync_to_physics && physics.is_alive(body.body)) {
            physics.set_transform(body.body, transform.pos, to_physics_angle(transform.rotation));
        }
    });
}

void step_physics_raw(flecs::world& world, PhysicsWorld& physics, f32 dt, i32 velocity_iterations, i32 position_iterations) {
    world.query<PhysicsBodyComponent, const PhysicsVelocity>().each([&](const PhysicsBodyComponent& body, const PhysicsVelocity& velocity) {
        if (physics.is_alive(body.body)) {
            physics.set_linear_velocity(body.body, velocity.linear);
        }
    });
    physics.step(dt, velocity_iterations, position_iterations);
}

void sync_transforms_from_physics_raw(flecs::world& world, PhysicsWorld& physics) {
    world.query<PhysicsBodyComponent, Transform2D>().each([&](const PhysicsBodyComponent& body, Transform2D& transform) {
        if (body.sync_from_physics && physics.is_alive(body.body)) {
            transform.pos = physics.position(body.body);
            transform.rotation = from_physics_angle(physics.rotation(body.body));
        }
    });
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
                                    .each([&physics](flecs::entity entity, PhysicsColliderComponent& colliders) {
                                        const PhysicsBodyComponent* body = entity.get<PhysicsBodyComponent>();
                                        if (!body) {
                                            return;
                                        }
                                        if (!physics.is_alive(body->body)) {
                                            return;
                                        }
                                        for (PhysicsCollider& collider : colliders.colliders) {
                                            if (physics.is_alive(collider.fixture)) {
                                                physics.destroy_fixture(collider.fixture);
                                            }
                                            collider.fixture = {};
                                        }
                                    })) {
}

void register_physics_components(EcsWorld& world) {
    flecs::world& raw = world.raw();
    raw.component<PhysicsBodyComponent>("PhysicsBodyComponent");
    raw.component<PhysicsColliderComponent>("PhysicsColliderComponent");
    raw.component<PhysicsVelocity>("PhysicsVelocity");
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
