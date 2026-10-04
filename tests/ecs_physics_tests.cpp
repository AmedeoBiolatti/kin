#include <kin/ecs/physics.hpp>

#include <cassert>
#include <cmath>

namespace {

bool near(kin::f32 a, kin::f32 b, kin::f32 epsilon = 0.05f) {
    return std::fabs(a - b) <= epsilon;
}

kin::PhysicsCollider collider(kin::PhysicsShape shape, bool sensor = false, kin::u64 tag = 0) {
    return {
        .def = {
            .shape = shape,
            .sensor = sensor,
            .tag = tag,
        },
    };
}

void test_body_creation_multiple_colliders_and_transform_sync() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");

    kin::PhysicsWorld physics;
    kin::EcsEntity entity = world.entity("actor")
                                .set(kin::Transform2D{.pos = {10.0f, 20.0f}, .rotation = 90.0f})
                                .set(kin::PhysicsBodyComponent{
                                    .def = {
                                        .type = kin::PhysicsBodyType::Dynamic,
                                        .fixed_rotation = true,
                                    },
                                })
                                .set(kin::PhysicsColliderComponent{
                                    .colliders = {
                                        collider(kin::PhysicsShape::circle(8.0f), false, 1),
                                        collider(kin::PhysicsShape::box({14.0f, 6.0f}, {20.0f, 0.0f}), true, 2),
                                    },
                                });

    kin::sync_physics_bodies(world, physics);

    auto* body = entity.get<kin::PhysicsBodyComponent>();
    auto* colliders = entity.get<kin::PhysicsColliderComponent>();
    assert(body != nullptr);
    assert(colliders != nullptr);
    assert(physics.is_alive(body->body));
    assert(physics.user_id(body->body) == entity.id());
    assert(physics.is_alive(colliders->colliders[0].fixture));
    assert(physics.is_alive(colliders->colliders[1].fixture));
    assert(physics.fixture_tag(colliders->colliders[1].fixture) == 2);
    assert((physics.position(body->body) == kin::Vec2f{10.0f, 20.0f}));
    // Transform2D turns in degrees, the physics world in radians.
    assert(near(physics.rotation(body->body), 1.5707964f));
}

void test_velocity_step_and_transform_sync_from_physics() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");

    kin::PhysicsWorld physics;
    kin::EcsEntity entity = world.entity("mover")
                                .set(kin::Transform2D{.pos = {0.0f, 0.0f}})
                                .set(kin::PhysicsBodyComponent{
                                    .def = {
                                        .type = kin::PhysicsBodyType::Kinematic,
                                    },
                                })
                                .set(kin::PhysicsColliderComponent{
                                    .colliders = {collider(kin::PhysicsShape::circle(4.0f))},
                                })
                                .set(kin::PhysicsVelocity{.linear = {60.0f, 0.0f}});

    kin::update_physics_world(world, physics, 1.0f / 60.0f);

    const kin::Transform2D* transform = entity.get<kin::Transform2D>();
    assert(transform != nullptr);
    assert(near(transform->pos.x, 1.0f, 0.01f));
    assert(near(transform->pos.y, 0.0f, 0.01f));

    // A quarter turn in the physics world (radians) is 90 degrees on the entity.
    physics.set_transform(entity.get<kin::PhysicsBodyComponent>()->body, {1.0f, 0.0f}, 1.5707964f);
    kin::sync_transforms_from_physics(world, physics);
    assert(near(entity.get<kin::Transform2D>()->rotation, 90.0f, 0.001f));
}

bool near(kin::Vec2f a, kin::Vec2f b, kin::f32 epsilon = 0.01f) {
    return near(a.x, b.x, epsilon) && near(a.y, b.y, epsilon);
}

kin::EcsEntity body_entity(kin::EcsWorld& world, const char* name, kin::Transform2D transform, kin::PhysicsBodyType type,
                           bool to_physics = false, bool from_physics = true) {
    return world.entity(name)
        .set(transform)
        .set(kin::PhysicsBodyComponent{.def = {.type = type}, .sync_to_physics = to_physics, .sync_from_physics = from_physics})
        .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(1.0f))}});
}

// Physics works in the world, Transform2D from the parent. A simulated body
// stays put when its parent moves; one the entity leads follows it.
void test_bodies_under_a_parent() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;

    kin::EcsEntity parent = world.entity("parent").set(kin::Transform2D{.pos = {100.0f, 0.0f}, .rotation = 90.0f});
    kin::EcsEntity simulated = body_entity(world, "simulated", {.pos = {10.0f, 0.0f}}, kin::PhysicsBodyType::Dynamic);
    kin::EcsEntity led = body_entity(world, "led", {.pos = {0.0f, 20.0f}}, kin::PhysicsBodyType::Kinematic, true, false);
    simulated.child_of(parent);
    led.child_of(parent);

    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    const kin::PhysicsBody simulated_body = simulated.get<kin::PhysicsBodyComponent>()->body;
    const kin::PhysicsBody led_body = led.get<kin::PhysicsBodyComponent>()->body;
    // Made where the entity is in the world, turned with its parent.
    assert(near(physics.position(simulated_body), {100.0f, 10.0f}));
    assert(near(physics.rotation(simulated_body), 1.5707964f, 0.001f));
    assert(near(physics.position(led_body), {80.0f, 0.0f}));

    parent.set(kin::Transform2D{.pos = {200.0f, 0.0f}, .rotation = 90.0f});
    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    assert(near(physics.position(simulated_body), {100.0f, 10.0f}));
    assert(near(kin::current_world_transform(simulated).pos, {100.0f, 10.0f}));
    assert(near(simulated.get<kin::Transform2D>()->pos, {10.0f, 100.0f}));
    assert(near(simulated.get<kin::Transform2D>()->rotation, 0.0f, 0.001f));
    assert(near(physics.position(led_body), {180.0f, 0.0f}));
    assert(near(led.get<kin::Transform2D>()->pos, {0.0f, 20.0f})); // the entity leads: untouched
}

// A body under another body is placed against where its parent has just been
// put, whichever order the query visits them in.
void test_nested_bodies_place_parents_first() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;

    kin::EcsEntity child = body_entity(world, "child", {.pos = {0.0f, 10.0f}}, kin::PhysicsBodyType::Dynamic);
    kin::EcsEntity mover = body_entity(world, "mover", {.pos = {0.0f, 0.0f}}, kin::PhysicsBodyType::Kinematic);
    mover.set(kin::PhysicsVelocity{.linear = {60.0f, 0.0f}});
    child.child_of(mover);

    for (int i = 0; i < 3; ++i) {
        kin::update_physics_world(world, physics, 1.0f / 60.0f);
    }
    assert(near(mover.get<kin::Transform2D>()->pos, {3.0f, 0.0f}));
    assert(near(kin::current_world_transform(child).pos, {0.0f, 10.0f}));
    assert(near(child.get<kin::Transform2D>()->pos, {-3.0f, 10.0f}));
}

void test_removed_collider_destroys_fixture() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");

    kin::PhysicsWorld physics;
    kin::EcsEntity entity = world.entity("actor")
                                .set(kin::Transform2D{.pos = {0.0f, 0.0f}})
                                .set(kin::PhysicsBodyComponent{
                                    .def = {
                                        .type = kin::PhysicsBodyType::Dynamic,
                                    },
                                })
                                .set(kin::PhysicsColliderComponent{
                                    .colliders = {
                                        collider(kin::PhysicsShape::circle(4.0f), false, 1),
                                        collider(kin::PhysicsShape::box({3.0f, 3.0f}, {8.0f, 0.0f}), true, 2),
                                    },
                                });

    kin::sync_physics_bodies(world, physics);
    kin::PhysicsBody body = entity.get<kin::PhysicsBodyComponent>()->body;
    kin::PhysicsFixture removed = entity.get<kin::PhysicsColliderComponent>()->colliders[1].fixture;
    assert(physics.fixtures(body).size() == 2);

    kin::PhysicsColliderComponent* colliders = entity.get_mut<kin::PhysicsColliderComponent>();
    colliders->colliders.pop_back();
    kin::sync_physics_bodies(world, physics);

    assert(!physics.is_alive(removed));
    assert(physics.fixtures(body).size() == 1);
}

void test_contact_and_overlap_collection() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");

    kin::PhysicsWorld physics;
    kin::EcsEntity trigger = world.entity("trigger")
                                 .set(kin::Transform2D{.pos = {0.0f, 0.0f}})
                                 .set(kin::PhysicsBodyComponent{
                                     .def = {
                                         .type = kin::PhysicsBodyType::Static,
                                     },
                                 })
                                 .set(kin::PhysicsColliderComponent{
                                     .colliders = {collider(kin::PhysicsShape::box({10.0f, 10.0f}), true, 10)},
                                 })
                                 .set(kin::PhysicsContactQueue{})
                                 .set(kin::PhysicsOverlapSet{});

    kin::EcsEntity actor = world.entity("actor")
                               .set(kin::Transform2D{.pos = {0.0f, 0.0f}})
                               .set(kin::PhysicsBodyComponent{
                                   .def = {
                                       .type = kin::PhysicsBodyType::Dynamic,
                                   },
                               })
                               .set(kin::PhysicsColliderComponent{
                                   .colliders = {collider(kin::PhysicsShape::circle(4.0f), false, 20)},
                               })
                               .set(kin::PhysicsContactQueue{})
                               .set(kin::PhysicsOverlapSet{});

    kin::sync_physics_bodies(world, physics);
    kin::step_physics(world, physics, 1.0f / 60.0f);
    kin::collect_physics_contacts(world, physics);

    assert(trigger.get<kin::PhysicsContactQueue>()->events.size() == 1);
    assert(actor.get<kin::PhysicsContactQueue>()->events.size() == 1);
    assert(trigger.get<kin::PhysicsOverlapSet>()->contacts.size() == 1);
    assert(actor.get<kin::PhysicsOverlapSet>()->contacts.size() == 1);

    kin::step_physics(world, physics, 1.0f / 60.0f);
    kin::collect_physics_contacts(world, physics);

    assert(trigger.get<kin::PhysicsContactQueue>()->events.empty());
    assert(actor.get<kin::PhysicsContactQueue>()->events.empty());
    assert(trigger.get<kin::PhysicsOverlapSet>()->contacts.size() == 1);
    assert(actor.get<kin::PhysicsOverlapSet>()->contacts.size() == 1);
}

void test_entity_removal_destroys_physics_body() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");

    kin::PhysicsWorld physics;
    kin::EcsPhysicsBridge bridge{world, physics};

    kin::EcsEntity entity = world.entity("owned")
                                .set(kin::Transform2D{.pos = {0.0f, 0.0f}})
                                .set(kin::PhysicsBodyComponent{
                                    .def = {
                                        .type = kin::PhysicsBodyType::Static,
                                    },
                                })
                                .set(kin::PhysicsColliderComponent{
                                    .colliders = {collider(kin::PhysicsShape::box({4.0f, 4.0f}))},
                                });

    kin::sync_physics_bodies(world, physics);
    const kin::PhysicsBody body = entity.get<kin::PhysicsBodyComponent>()->body;
    assert(physics.is_alive(body));

    entity.destroy();
    world.progress(0.0f);

    assert(!physics.is_alive(body));
    (void)bridge;
}

void test_component_removal_cleans_up_physics_objects() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");

    kin::PhysicsWorld physics;
    kin::EcsPhysicsBridge bridge{world, physics};

    kin::EcsEntity entity = world.entity("components")
                                .set(kin::Transform2D{.pos = {0.0f, 0.0f}})
                                .set(kin::PhysicsBodyComponent{
                                    .def = {
                                        .type = kin::PhysicsBodyType::Static,
                                    },
                                })
                                .set(kin::PhysicsColliderComponent{
                                    .colliders = {
                                        collider(kin::PhysicsShape::box({4.0f, 4.0f}), false, 1),
                                        collider(kin::PhysicsShape::circle(2.0f), true, 2),
                                    },
                                });

    kin::sync_physics_bodies(world, physics);
    kin::PhysicsBody body = entity.get<kin::PhysicsBodyComponent>()->body;
    kin::PhysicsFixture first = entity.get<kin::PhysicsColliderComponent>()->colliders[0].fixture;
    kin::PhysicsFixture second = entity.get<kin::PhysicsColliderComponent>()->colliders[1].fixture;
    assert(physics.is_alive(body));
    assert(physics.is_alive(first));
    assert(physics.is_alive(second));

    entity.remove<kin::PhysicsColliderComponent>();
    world.progress(0.0f);
    assert(physics.is_alive(body));
    assert(!physics.is_alive(first));
    assert(!physics.is_alive(second));

    entity.set(kin::PhysicsColliderComponent{
        .colliders = {collider(kin::PhysicsShape::box({3.0f, 3.0f}), false, 3)},
    });
    kin::sync_physics_bodies(world, physics);
    kin::PhysicsFixture recreated = entity.get<kin::PhysicsColliderComponent>()->colliders[0].fixture;
    assert(physics.is_alive(recreated));

    entity.remove<kin::PhysicsBodyComponent>();
    world.progress(0.0f);
    assert(!physics.is_alive(body));
    assert(!physics.is_alive(recreated));
    (void)bridge;
}

} // namespace

int main() {
    test_body_creation_multiple_colliders_and_transform_sync();
    test_velocity_step_and_transform_sync_from_physics();
    test_bodies_under_a_parent();
    test_nested_bodies_place_parents_first();
    test_removed_collider_destroys_fixture();
    test_contact_and_overlap_collection();
    test_entity_removal_destroys_physics_body();
    test_component_removal_cleans_up_physics_objects();
    return 0;
}
