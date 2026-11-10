#include <kin/ecs/physics.hpp>
#include <kin/scene/scene_asset.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

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

// A collider of an entity with no body, on the body above it: its fixtures
// count as that body's (none destroyed), and one taken out of its list goes.
void test_colliders_below_a_body_keep_their_fixtures() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;
    kin::EcsEntity owner = world.entity("cart")
                               .set(kin::Transform2D{})
                               .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic}})
                               .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(4.0f))}});
    kin::EcsEntity wheel = world.entity("wheel")
                               .set(kin::Transform2D{.pos = {6.0f, 0.0f}})
                               .set(kin::PhysicsColliderComponent{
                                   .colliders = {collider(kin::PhysicsShape::circle(2.0f)), collider(kin::PhysicsShape::circle(1.0f))}});
    wheel.child_of(owner);
    kin::sync_physics_bodies(world, physics);
    const kin::PhysicsBody body = owner.get<kin::PhysicsBodyComponent>()->body;
    assert(physics.fixtures(body).size() == 3 && physics.fixture_count(body) == 3);
    kin::sync_physics_bodies(world, physics); // nothing changed: nothing goes
    assert(physics.fixture_count(body) == 3);
    const kin::PhysicsFixture removed = wheel.get<kin::PhysicsColliderComponent>()->colliders[1].fixture;
    wheel.get_mut<kin::PhysicsColliderComponent>()->colliders.pop_back();
    kin::sync_physics_bodies(world, physics);
    assert(!physics.is_alive(removed) && physics.fixture_count(body) == 2);
}

// Contacts reach the entity whose body it is, found by the body's user id,
// or, for a body the game gave an id of its own, through every body.
void test_contacts_reach_bodies_with_their_own_ids() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;
    kin::EcsEntity trigger = world.entity("trigger")
                                 .set(kin::Transform2D{})
                                 .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static, .user_id = 777}})
                                 .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::box({10.0f, 10.0f}), true)}})
                                 .set(kin::PhysicsContactQueue{});
    kin::EcsEntity actor = world.entity("actor")
                               .set(kin::Transform2D{})
                               .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic}})
                               .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(4.0f))}})
                               .set(kin::PhysicsContactQueue{});
    kin::sync_physics_bodies(world, physics);
    assert(physics.user_id(trigger.get<kin::PhysicsBodyComponent>()->body) == 777);
    kin::step_physics(world, physics, 1.0f / 60.0f);
    kin::collect_physics_contacts(world, physics);
    assert(trigger.get<kin::PhysicsContactQueue>()->events.size() == 1);
    assert(actor.get<kin::PhysicsContactQueue>()->events.size() == 1);
}

// A body the simulation leads goes where its entity is when teleported, once.
void test_teleport_places_the_body_once() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;
    kin::EcsEntity actor = body_entity(world, "actor", {.pos = {0.0f, 0.0f}}, kin::PhysicsBodyType::Dynamic);
    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    actor.set(kin::Transform2D{.pos = {50.0f, 20.0f}});
    actor.raw().add<kin::PhysicsTeleport>();
    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    const kin::PhysicsBody body = actor.get<kin::PhysicsBodyComponent>()->body;
    assert(near(physics.position(body), kin::Vec2f{50.0f, 20.0f}, 1.0f));
    assert(!actor.raw().has<kin::PhysicsTeleport>());
    // Moved again without one: the simulation leads, the entity goes back.
    actor.set(kin::Transform2D{.pos = {0.0f, 0.0f}});
    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    assert(near(physics.position(body), kin::Vec2f{50.0f, 20.0f}, 1.0f));
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

const kin::ComponentFieldSnapshot* snapshot_field(const kin::ComponentSnapshot& snapshot, std::string_view name) {
    const auto found = std::ranges::find(snapshot.fields, name, &kin::ComponentFieldSnapshot::name);
    return found == snapshot.fields.end() ? nullptr : &*found;
}

void test_described_components_make_bodies() {
    // Scenes and tools set a body, and its first collider, by their fields.
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::register_physics_components(world);
    kin::PhysicsWorld physics({.gravity = {0.0f, 9.8f * 32.0f}, .units_per_meter = 32.0f});
    kin::EcsPhysicsBridge bridge{world, physics};
    kin::EcsComponentRegistry& components = world.components();

    const kin::ComponentDescriptor* described = components.find("PhysicsBodyComponent");
    assert(described && !described->fields.empty() && described->fields[0].name == "type");
    assert((described->fields[0].choices == std::vector<std::string>{"static", "dynamic", "kinematic"}));
    assert(components.find("PhysicsColliderComponent") && components.find("PhysicsVelocity"));
    assert(components.find("PhysicsContactQueue") && components.find("PhysicsOverlapSet"));

    kin::EcsEntity crate = world.entity("crate").set(kin::Transform2D{.pos = {100.0f, 50.0f}});
    assert(components.add(crate, "PhysicsBodyComponent"));
    assert(components.patch_field(crate, "PhysicsBodyComponent", "type", std::string("dynamic")));
    assert(!components.patch_field(crate, "PhysicsBodyComponent", "type", std::string("wobbly")));
    assert(components.patch_field(crate, "PhysicsBodyComponent", "angular_damping", 0.5f));
    assert(components.patch_field(crate, "PhysicsBodyComponent", "angular_velocity", 90.0f)); // degrees a second
    const kin::PhysicsBodyComponent* body = crate.get<kin::PhysicsBodyComponent>();
    assert(body->def.type == kin::PhysicsBodyType::Dynamic && body->def.angular_damping == 0.5f);
    assert(near(body->def.angular_velocity, 1.5707964f));

    assert(components.add(crate, "PhysicsColliderComponent"));
    assert(crate.get<kin::PhysicsColliderComponent>()->colliders.empty()); // none until a field is set
    assert(components.patch_field(crate, "PhysicsColliderComponent", "half_extents", kin::Vec2f{16.0f, 8.0f}));
    assert(components.patch_field(crate, "PhysicsColliderComponent", "restitution", 0.5f));
    assert(!components.patch_field(crate, "PhysicsColliderComponent", "mask_bits", kin::u32{0x10000}));
    assert(!components.patch_field(crate, "PhysicsColliderComponent", "shape", std::string("triangle")));
    const kin::PhysicsColliderComponent* colliders = crate.get<kin::PhysicsColliderComponent>();
    assert(colliders->colliders.size() == 1);
    assert((colliders->colliders[0].def.shape.half_extents == kin::Vec2f{16.0f, 8.0f}));
    assert(colliders->colliders[0].def.restitution == 0.5f);

    const std::optional<kin::ComponentSnapshot> read = components.snapshot(crate, "PhysicsBodyComponent");
    assert(read);
    const kin::ComponentFieldSnapshot* type = snapshot_field(*read, "type");
    assert(type && std::get<std::string>(type->value) == "dynamic");
    const kin::ComponentFieldSnapshot* turning = snapshot_field(*read, "angular_velocity");
    assert(turning && near(std::get<kin::f32>(turning->value), 90.0f, 0.001f));

    // Made where the entity is, and it falls.
    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    assert(physics.is_alive(crate.get<kin::PhysicsBodyComponent>()->body));
    std::vector<kin::PhysicsDebugShape> shapes = physics.debug_shapes();
    assert(shapes.size() == 1 && shapes[0].type == kin::PhysicsDebugShapeType::Polygon);
    for (int i = 0; i < 30; ++i) {
        kin::update_physics_world(world, physics, 1.0f / 60.0f);
    }
    assert(crate.get<kin::Transform2D>()->pos.y > 60.0f);

    // A collider changed: its fixture is made again from it, the old one goes.
    assert(components.patch_field(crate, "PhysicsColliderComponent", "shape", std::string("circle")));
    assert(components.patch_field(crate, "PhysicsColliderComponent", "radius", 12.0f));
    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    shapes = physics.debug_shapes();
    assert(shapes.size() == 1 && shapes[0].type == kin::PhysicsDebugShapeType::Circle && near(shapes[0].radius, 12.0f, 0.01f));
    (void)bridge;
}

void test_prefab_instances_get_their_own_bodies() {
    // Described components are inherited by a prefab's instances; each
    // instance's body, fixtures and contacts are made its own.
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::register_physics_components(world);
    kin::PhysicsWorld physics({.gravity = {0.0f, 10.0f}});
    kin::EcsPhysicsBridge bridge{world, physics};

    flecs::entity prefab = world.raw().prefab("ball");
    prefab.set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic}});
    prefab.set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(1.0f))}});
    prefab.add<kin::PhysicsContactQueue>();
    flecs::entity a = world.raw().entity().is_a(prefab).set(kin::Transform2D{.pos = {0.0f, 0.0f}});
    flecs::entity b = world.raw().entity().is_a(prefab).set(kin::Transform2D{.pos = {10.0f, 0.0f}});
    assert(!a.owns<kin::PhysicsBodyComponent>()); // inherited, as described components are

    for (int i = 0; i < 30; ++i) {
        kin::update_physics_world(world, physics, 1.0f / 60.0f);
    }
    for (flecs::entity instance : {a, b}) {
        assert(instance.owns<kin::PhysicsBodyComponent>() && instance.owns<kin::PhysicsColliderComponent>());
        assert(instance.owns<kin::PhysicsContactQueue>());
        assert(physics.is_alive(instance.get<kin::PhysicsBodyComponent>()->body));
        assert(instance.get<kin::Transform2D>()->pos.y > 0.5f);
    }
    assert(!(a.get<kin::PhysicsBodyComponent>()->body == b.get<kin::PhysicsBodyComponent>()->body));
    assert(near(a.get<kin::Transform2D>()->pos.x, 0.0f) && near(b.get<kin::Transform2D>()->pos.x, 10.0f));
    assert(physics.debug_shapes().size() == 2);
    assert(!physics.is_alive(prefab.get<kin::PhysicsBodyComponent>()->body)); // the prefab stays a template

    b.destruct();
    assert(physics.debug_shapes().size() == 1);
    (void)bridge;
}

void test_bridge_takes_its_observers_with_it() {
    // A bridge moved works on; one gone leaves nothing behind to reach the
    // physics world (which may go before the ECS world does).
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics;
    const auto body_entity = [&](const char* name) {
        return world.entity(name)
            .set(kin::Transform2D{})
            .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic}})
            .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(1.0f))}});
    };
    kin::EcsEntity kept = body_entity("kept");
    {
        kin::EcsPhysicsBridge first{world, physics};
        kin::EcsPhysicsBridge bridge = std::move(first);
        kin::EcsEntity gone = body_entity("gone");
        kin::sync_physics_bodies(world, physics);
        const kin::PhysicsBody gone_body = gone.get<kin::PhysicsBodyComponent>()->body;
        assert(physics.is_alive(gone_body));
        gone.destroy();
        assert(!physics.is_alive(gone_body));
    }
    const kin::PhysicsBody body = kept.get<kin::PhysicsBodyComponent>()->body;
    assert(physics.is_alive(body));
    kept.remove<kin::PhysicsBodyComponent>();
    assert(physics.is_alive(body)); // no bridge, so nothing destroyed it
}

} // namespace

// Enough bodies that their sync is done in pieces on the job system's
// workers: what is missing is still made, what no collider claims still
// goes, and every entity is placed where its body is.
void test_many_bodies_sync_in_pieces() {
    kin::EcsWorld world;
    kin::register_physics_components(world);
    world.component<kin::Transform2D>("Transform2D");
    kin::PhysicsWorld physics{kin::PhysicsWorldDef{.gravity = {0.0f, 0.0f}}};
    std::vector<kin::EcsEntity> crowd;
    for (int i = 0; i < 6000; ++i) {
        crowd.push_back(world.entity()
                            .set(kin::Transform2D{.pos = {static_cast<kin::f32>(i % 100) * 20.0f, static_cast<kin::f32>(i / 100) * 20.0f}})
                            .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic, .gravity_scale = 0.0f}})
                            .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(4.0f))}})
                            .set(kin::PhysicsVelocity{.linear = {static_cast<kin::f32>(i % 7) - 3.0f, 2.0f}}));
    }
    kin::EcsEntity cart = world.entity("cart")
                              .set(kin::Transform2D{.pos = {-100.0f, -100.0f}})
                              .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic}})
                              .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(4.0f))}});
    kin::EcsEntity wheel = world.entity("wheel")
                               .set(kin::Transform2D{.pos = {6.0f, 0.0f}})
                               .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(2.0f))}});
    wheel.child_of(cart);

    kin::sync_physics_bodies(world, physics);
    for (const kin::EcsEntity& one : crowd) {
        assert(physics.is_alive(one.get<kin::PhysicsBodyComponent>()->body));
        assert(physics.fixture_count(one.get<kin::PhysicsBodyComponent>()->body) == 1);
    }
    const kin::PhysicsBody cart_body = cart.get<kin::PhysicsBodyComponent>()->body;
    assert(physics.fixture_count(cart_body) == 2);
    kin::sync_physics_bodies(world, physics); // nothing changed: nothing goes
    assert(physics.fixture_count(cart_body) == 2);

    for (int step = 0; step < 3; ++step) {
        kin::update_physics_world(world, physics, 1.0f / 60.0f);
    }
    for (const kin::EcsEntity& one : crowd) {
        const kin::Vec2f at = physics.position(one.get<kin::PhysicsBodyComponent>()->body);
        assert(one.get<kin::Transform2D>()->pos.x == at.x && one.get<kin::Transform2D>()->pos.y == at.y);
    }
    assert(crowd[5].get<kin::Transform2D>()->pos.y > 0.0f); // they moved

    // A few more among many: made, in the step they came.
    std::vector<kin::EcsEntity> late;
    for (int i = 0; i < 3; ++i) {
        late.push_back(world.entity()
                           .set(kin::Transform2D{.pos = {-300.0f - static_cast<kin::f32>(i) * 20.0f, 0.0f}})
                           .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Dynamic}})
                           .set(kin::PhysicsColliderComponent{.colliders = {collider(kin::PhysicsShape::circle(4.0f))}}));
    }
    // A fixture no collider has, one taken out of a collider list, one of a collider below a body.
    const kin::PhysicsBody stray_body = crowd[4000].get<kin::PhysicsBodyComponent>()->body;
    const kin::PhysicsFixture stray = physics.add_fixture(stray_body, {.shape = kin::PhysicsShape::circle(1.0f)});
    const kin::PhysicsFixture dropped = crowd[2500].get<kin::PhysicsColliderComponent>()->colliders[0].fixture;
    crowd[2500].get_mut<kin::PhysicsColliderComponent>()->colliders.clear();
    const kin::PhysicsFixture wheel_fixture = wheel.get<kin::PhysicsColliderComponent>()->colliders[0].fixture;
    wheel.get_mut<kin::PhysicsColliderComponent>()->colliders.clear();
    kin::update_physics_world(world, physics, 1.0f / 60.0f);
    for (const kin::EcsEntity& one : late) {
        assert(physics.is_alive(one.get<kin::PhysicsBodyComponent>()->body));
        assert(physics.fixture_count(one.get<kin::PhysicsBodyComponent>()->body) == 1);
    }
    assert(!physics.is_alive(stray) && physics.fixture_count(stray_body) == 1);
    assert(!physics.is_alive(dropped) && physics.fixture_count(crowd[2500].get<kin::PhysicsBodyComponent>()->body) == 0);
    assert(!physics.is_alive(wheel_fixture) && physics.fixture_count(cart_body) == 1);
}

int main() {
    test_body_creation_multiple_colliders_and_transform_sync();
    test_velocity_step_and_transform_sync_from_physics();
    test_bodies_under_a_parent();
    test_nested_bodies_place_parents_first();
    test_removed_collider_destroys_fixture();
    test_contact_and_overlap_collection();
    test_colliders_below_a_body_keep_their_fixtures();
    test_contacts_reach_bodies_with_their_own_ids();
    test_teleport_places_the_body_once();
    test_entity_removal_destroys_physics_body();
    test_component_removal_cleans_up_physics_objects();
    test_described_components_make_bodies();
    test_prefab_instances_get_their_own_bodies();
    test_bridge_takes_its_observers_with_it();
    test_many_bodies_sync_in_pieces();
    return 0;
}
