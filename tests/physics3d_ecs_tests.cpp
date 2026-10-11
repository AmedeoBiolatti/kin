// The ECS side of kin/physics3d (kin/ecs/physics3d.hpp), without a GPU: bodies made for entities in entity id order
// and destroyed with them, kinematic bodies following Transform3D and dynamic ones writing it back (under a
// parent too), events routed to a place game code reads, determinism of a whole scene, debug lines, the ground
// query of foot placement over a physics floor and step, colliders from meshes. docs/physics_3d.md.

#include "skin_fixtures.hpp"

#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/render3d/mesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace kin;

int failures = 0;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            ++failures;                                                                       \
            std::fprintf(stderr, "physics3d_ecs_tests:%d: CHECK failed: %s\n", __LINE__, #cond); \
        }                                                                                     \
    } while (0)

constexpr f32 dt = 1.0f / 60.0f;

bool near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }
bool near(Vec3f a, Vec3f b, f32 eps) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }

World3DConfig serial() {
    World3DConfig c;
    c.single_threaded = true;
    return c;
}

Collider3D box_collider(Vec3f half) {
    Collider3D c;
    c.kind = Collider3D::Kind::Box;
    c.half_extents = half;
    return c;
}

// A static floor whose top is y = 0.
flecs::entity add_floor(flecs::world& world) {
    return world.entity().set(Transform3D{.position = {0, -0.5f, 0}}).set(box_collider({50, 0.5f, 50}));
}

flecs::entity add_crate(flecs::world& world, Vec3f at, f32 half = 0.5f) {
    return world.entity().set(Transform3D{.position = at}).set(box_collider({half, half, half})).set(RigidBody3D{.density = 500.0f});
}

void test_bodies_in_entity_order() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    std::vector<flecs::entity> made;
    made.push_back(add_floor(world));
    for (int i = 0; i < 4; ++i) {
        made.push_back(add_crate(world, {static_cast<f32>(i) * 2.0f, 1.0f, 0}));
    }
    CHECK(physics.body_count() == 0);
    bodies.sync_bodies();
    CHECK(physics.body_count() == 5 && bodies.body_count() == 5);
    // Entity i has body number i: created in ascending entity id order.
    for (std::size_t i = 0; i < made.size(); ++i) {
        CHECK(bodies.body_of(made[i]).index == i);
        CHECK(physics.user_id(bodies.body_of(made[i])) == made[i].id());
        CHECK(made[i].get<PhysicsBody3D>() != nullptr && made[i].get<PhysicsBody3D>()->body == bodies.body_of(made[i]));
    }
    CHECK(physics.motion(bodies.body_of(made[0])) == Body3DMotion::Static);
    CHECK(physics.motion(bodies.body_of(made[1])) == Body3DMotion::Dynamic);

    // Entities that go take their bodies; new ones fill the freed numbers, again in id order (last freed, first used).
    const Body3D gone1 = bodies.body_of(made[2]);
    const Body3D gone2 = bodies.body_of(made[4]);
    made[2].destruct();
    made[4].remove<Collider3D>();
    bodies.sync_bodies();
    CHECK(physics.body_count() == 3 && bodies.body_count() == 3);
    CHECK(!physics.contains(gone1) && !physics.contains(gone2));
    CHECK(bodies.body_of(made[4]) == Body3D{});
    const flecs::entity n1 = add_crate(world, {0, 3, 0});
    const flecs::entity n2 = add_crate(world, {0, 5, 0});
    bodies.sync_bodies();
    // Destroyed in ascending order (slot 2 then 4): the free list holds [2, 4], last in first out: the new entity with
    // the lower id takes 4 and the other 2 (flecs reuses ids, so which of n1 and n2 that is, is up to flecs).
    const flecs::entity lower = n1.id() < n2.id() ? n1 : n2;
    const flecs::entity upper = n1.id() < n2.id() ? n2 : n1;
    CHECK(bodies.body_of(lower).index == 4 && bodies.body_of(upper).index == 2);
    CHECK(physics.body_count() == 5);
}

void test_scale_offset_and_compound() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    // A unit box (half 0.5) on an entity of scale 2 is 2 m on a side: its top at y = 1 + 1 = 2 when centred on y = 1.
    world.entity().set(Transform3D{.position = {0, 1, 0}, .scale = {2, 2, 2}}).set(box_collider({0.5f, 0.5f, 0.5f}));
    // An offset collider: the box is 3 m to the side of the entity's origin.
    Collider3D off = box_collider({0.5f, 0.5f, 0.5f});
    off.offset = {3, 0, 0};
    world.entity().set(Transform3D{.position = {10, 1, 0}}).set(off);
    // A compound of two: a box at -1 and a sphere at +1.
    Collider3D compound;
    compound.kind = Collider3D::Kind::Compound;
    Collider3D left = box_collider({0.5f, 0.5f, 0.5f});
    left.offset = {-1, 0, 0};
    Collider3D right;
    right.kind = Collider3D::Kind::Sphere;
    right.radius = 0.5f;
    right.offset = {1, 0, 0};
    compound.children = {left, right};
    world.entity().set(Transform3D{.position = {20, 1, 0}}).set(compound);
    bodies.sync_bodies();
    const auto top = [&](f32 x) { return physics.raycast({x, 10, 0}, {x, -5, 0}); };
    CHECK(top(0.0f) && near(top(0.0f)->point.y, 2.0f, 1e-3f));
    CHECK(top(0.9f) && near(top(0.9f)->point.y, 2.0f, 1e-3f) && !top(1.1f));
    CHECK(!top(10.0f) && top(13.0f) && near(top(13.0f)->point.y, 1.5f, 1e-3f));
    const auto l = top(19.0f);
    const auto r = top(21.0f);
    CHECK(l && l->sub_shape == 0 && near(l->point.y, 1.5f, 1e-3f));
    CHECK(r && r->sub_shape == 1 && near(r->point.y, 1.5f, 1e-3f));
}

void test_dynamic_writes_back() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity crate = add_crate(world, {0, 3, 0});
    // Scale on the entity is left alone by the write back.
    crate.get_mut<Transform3D>()->scale = {1, 1, 1};
    for (int i = 0; i < 180; ++i) {
        bodies.step(dt);
    }
    const Transform3D* t = crate.get<Transform3D>();
    CHECK(near(t->position.y, 0.5f, 0.03f) && near(t->position.x, 0.0f, 0.02f));
    CHECK(near(t->position, physics.position(bodies.body_of(crate)), 1e-6f));
    // A body that tumbled: the rotation is written.
    const flecs::entity tipped = world.entity()
                                     .set(Transform3D{.position = {5, 3, 0}, .rotation = Quatf::axis_angle({0, 0, 1}, 30.0f)})
                                     .set(box_collider({0.5f, 0.5f, 0.5f}))
                                     .set(RigidBody3D{});
    bodies.step(dt);
    f32 changed = 0.0f;
    for (int i = 0; i < 120; ++i) {
        bodies.step(dt);
        const Quatf q = tipped.get<Transform3D>()->rotation;
        changed = std::max(changed, std::fabs(q.z - Quatf::axis_angle({0, 0, 1}, 30.0f).z));
    }
    CHECK(changed > 0.01f);
    CHECK(near(tipped.get<Transform3D>()->rotation.x, physics.rotation(bodies.body_of(tipped)).x, 1e-6f));
    // Initial velocity and gravity factor from the component.
    const flecs::entity floater = world.entity()
                                      .set(Transform3D{.position = {0, 10, 0}})
                                      .set(Collider3D{.kind = Collider3D::Kind::Sphere, .radius = 0.2f})
                                      .set(RigidBody3D{.gravity_factor = 0.0f, .linear_velocity = {3, 0, 0}});
    for (int i = 0; i < 60; ++i) {
        bodies.step(dt);
    }
    CHECK(near(floater.get<Transform3D>()->position.x, 3.0f, 0.1f) && near(floater.get<Transform3D>()->position.y, 10.0f, 0.01f));
}

void test_parented_bodies() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    // A parent at (10, 0, 0) turned 90 degrees about y; the child's local (2, 4, 0) is the world's (10, 4, -2).
    const flecs::entity parent = world.entity().set(Transform3D{.position = {10, 0, 0}, .rotation = Quatf::axis_angle({0, 1, 0}, 90.0f)});
    const flecs::entity child = world.entity()
                                    .child_of(parent)
                                    .set(Transform3D{.position = {2, 4, 0}})
                                    .set(box_collider({0.5f, 0.5f, 0.5f}))
                                    .set(RigidBody3D{});
    bodies.sync_bodies();
    CHECK(near(physics.position(bodies.body_of(child)), {10, 4, -2}, 1e-4f));
    for (int i = 0; i < 20; ++i) {
        bodies.step(dt);
    }
    // The world pose of the child is what the body says: local = parent^-1 * body.
    const auto world_of = [&] {
        return parent.get<Transform3D>()->matrix().apply(child.get<Transform3D>()->position);
    };
    CHECK(near(world_of(), physics.position(bodies.body_of(child)), 1e-3f));
    // Move the parent away: the body is left where it is (bodies are world level), the local transform follows.
    const Vec3f before = physics.position(bodies.body_of(child));
    parent.get_mut<Transform3D>()->position = {-30, 0, 0};
    for (int i = 0; i < 5; ++i) {
        bodies.step(dt);
    }
    CHECK(near(physics.position(bodies.body_of(child)).x, before.x, 0.05f));
    CHECK(near(world_of(), physics.position(bodies.body_of(child)), 1e-3f));
    // Land on the floor and rest at 0.5.
    for (int i = 0; i < 120; ++i) {
        bodies.step(dt);
    }
    CHECK(near(physics.position(bodies.body_of(child)).y, 0.5f, 0.03f));
}

void test_kinematic_follows_and_pushes() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity platform = world.entity()
                                       .set(Transform3D{.position = {0, 0.25f, 0}})
                                       .set(box_collider({1, 0.25f, 1}))
                                       .set(RigidBody3D{.motion = Body3DMotion::Kinematic, .friction = 1.0f});
    const flecs::entity crate = add_crate(world, {0, 1.0f, 0}, 0.4f);
    crate.get_mut<RigidBody3D>()->friction = 1.0f;
    // Carried: the platform moves along x at 2 m/s by the game setting its Transform3D.
    for (int i = 0; i < 120; ++i) {
        platform.get_mut<Transform3D>()->position.x = 2.0f * dt * static_cast<f32>(i + 1);
        bodies.step(dt);
    }
    const Body3D pb = bodies.body_of(platform);
    CHECK(near(physics.position(pb).x, 4.0f, 0.02f));
    CHECK(near(platform.get<Transform3D>()->position.x, 4.0f, 1e-5f)); // not written back
    CHECK(crate.get<Transform3D>()->position.x > 2.5f); // carried by friction
    CHECK(crate.get<Transform3D>()->position.y > 0.6f);   // on it
    // A kinematic child follows its parent.
    const flecs::entity holder = world.entity().set(Transform3D{.position = {20, 0, 0}});
    const flecs::entity hand = world.entity()
                                   .child_of(holder)
                                   .set(Transform3D{.position = {0, 3, 0}})
                                   .set(box_collider({0.2f, 0.2f, 0.2f}))
                                   .set(RigidBody3D{.motion = Body3DMotion::Kinematic});
    bodies.step(dt);
    CHECK(near(physics.position(bodies.body_of(hand)), {20, 3, 0}, 1e-3f));
    holder.get_mut<Transform3D>()->position = {25, 0, 0};
    bodies.step(dt);
    CHECK(near(physics.position(bodies.body_of(hand)), {25, 3, 0}, 1e-3f));
}

void test_game_moves_entities() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity crate = add_crate(world, {0, 5, 0});
    for (int i = 0; i < 10; ++i) {
        bodies.step(dt);
    }
    const f32 vy = physics.linear_velocity(bodies.body_of(crate)).y;
    CHECK(vy < -1.0f);
    // Game code teleports it: the body goes there with its velocity kept; and a static entity moved moves its body.
    crate.get_mut<Transform3D>()->position = {8, 7, 0};
    bodies.step(dt);
    CHECK(near(physics.position(bodies.body_of(crate)).x, 8.0f, 1e-3f));
    CHECK(near(physics.position(bodies.body_of(crate)).y, 7.0f + vy * dt - 9.81f * dt * dt, 0.05f));
    const flecs::entity wall = world.entity().set(Transform3D{.position = {0, 1, 5}}).set(box_collider({1, 1, 0.1f}));
    bodies.step(dt);
    CHECK(physics.raycast({0, 1, 10}, {0, 1, 0}) && near(physics.raycast({0, 1, 10}, {0, 1, 0})->point.z, 5.1f, 1e-3f));
    wall.get_mut<Transform3D>()->position = {0, 1, -5};
    bodies.step(dt);
    CHECK(!physics.raycast({0, 1, 10}, {0, 1, 0}) || near(physics.raycast({0, 1, 10}, {0, 1, 0})->point.z, -4.9f, 1e-3f));
    // rebuild() and a changed motion: re-made at the next sync.
    const Body3D old = bodies.body_of(wall);
    wall.set(RigidBody3D{.motion = Body3DMotion::Kinematic});
    bodies.step(dt);
    CHECK(!physics.contains(old) && physics.motion(bodies.body_of(wall)) == Body3DMotion::Kinematic);
    const Body3D before = bodies.body_of(wall);
    bodies.rebuild(wall);
    bodies.step(dt);
    CHECK(bodies.body_of(wall).valid() && !physics.contains(before));
}

void test_events_routed() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3DConfig cfg = serial();
    World3D physics(cfg);
    WorldPhysicsState3D bodies{ecs, physics};
    const flecs::entity floor = add_floor(world);
    const flecs::entity crate = add_crate(world, {0, 1.5f, 0});
    int begins = 0;
    for (int i = 0; i < 60; ++i) {
        bodies.step(dt);
        for (const ContactEvent3D& e : bodies.events()) {
            if (e.kind == ContactKind3D::Begin) {
                ++begins;
                CHECK(e.user_a == floor.id() && e.user_b == crate.id());
                CHECK(bodies.entity_of(e.user_b) == crate);
            }
        }
        // The singleton holds the same.
        const PhysicsContacts3D* s = world.get<PhysicsContacts3D>();
        CHECK(s != nullptr && s->events.size() == bodies.events().size());
    }
    CHECK(begins == 1);
    // A destroyed entity's id names nobody.
    const u64 id = crate.id();
    crate.destruct();
    CHECK(!bodies.entity_of(id).is_valid());
    bodies.step(dt);
    bool ended = false;
    for (const ContactEvent3D& e : bodies.events()) {
        ended = ended || (e.kind == ContactKind3D::End && e.user_b == id);
    }
    CHECK(ended);
    // advance(): whole fixed steps with a remainder, and the longest frame is capped.
    WorldPhysicsState3D stepper{ecs, physics, {.fixed_dt = 0.01f, .max_steps = 4}};
    CHECK(stepper.advance(0.025f) == 2);
    CHECK(near(stepper.alpha(), 0.5f, 1e-3f));
    CHECK(stepper.advance(0.005f) == 1);
    CHECK(stepper.advance(1.0f) == 4);
    CHECK(stepper.alpha() <= 1.0f);
}

std::vector<u64> scene_hashes(const World3DConfig& cfg, int frames) {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(cfg);
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    for (int i = 0; i < 60; ++i) {
        const f32 x = static_cast<f32>(i % 6) * 1.1f - 3.0f;
        const f32 z = static_cast<f32>((i / 6) % 5) * 1.1f - 2.0f;
        const f32 y = 1.0f + 0.9f * static_cast<f32>(i / 30);
        if (i % 3 == 0) {
            world.entity().set(Transform3D{.position = {x, y, z}}).set(Collider3D{.kind = Collider3D::Kind::Sphere, .radius = 0.4f}).set(RigidBody3D{});
        } else {
            world.entity().set(Transform3D{.position = {x, y, z}, .rotation = Quatf::axis_angle({1, 1, 0}, static_cast<f32>(i))})
                .set(box_collider({0.4f, 0.3f, 0.35f}))
                .set(RigidBody3D{});
        }
    }
    std::vector<u64> hashes;
    for (int i = 0; i < frames; ++i) {
        bodies.step(dt);
        hashes.push_back(physics.state_hash());
    }
    return hashes;
}

void test_scene_determinism() {
    const std::vector<u64> a = scene_hashes(serial(), 90);
    CHECK(a == scene_hashes(serial(), 90));
    CHECK(a.front() != a.back());
    JobSystem four({.workers = 4, .max_background = 4});
    World3DConfig cfg;
    cfg.jobs = &four;
    CHECK(scene_hashes(cfg, 90) == a);
}

void test_interpolation() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity crate = add_crate(world, {0, 10, 0});
    for (int i = 0; i < 6; ++i) {
        bodies.step(dt);
    }
    const f32 last = crate.get<Transform3D>()->position.y;
    bodies.interpolate(1.0f);
    CHECK(near(crate.get<Transform3D>()->position.y, last, 1e-6f));
    bodies.interpolate(0.0f);
    const f32 before = crate.get<Transform3D>()->position.y;
    CHECK(before > last); // the one before the last step is higher: it is falling
    bodies.interpolate(0.5f);
    CHECK(near(crate.get<Transform3D>()->position.y, 0.5f * (before + last), 1e-5f));
    // The interpolated pose is not read as a move by game code: the next step keeps the body going.
    const Vec3f at = physics.position(bodies.body_of(crate));
    bodies.step(dt);
    CHECK(physics.position(bodies.body_of(crate)).y < at.y);
    CHECK(near(physics.position(bodies.body_of(crate)).y, at.y + physics.linear_velocity(bodies.body_of(crate)).y * dt, 0.01f));
}

// --- debug lines ---

void test_debug_lines() {
    World3D physics(serial());
    // A sleeping box, an awake sphere, a kinematic capsule, a trigger, and a hinge.
    physics.create_body({.shape = Shape3D::box({10, 0.5f, 10}), .motion = Body3DMotion::Static, .position = {0, -0.5f, 0}});
    const Body3D resting = physics.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {-3, 0.5f, 0}});
    physics.create_body({.shape = Shape3D::sphere(0.5f), .position = {0, 40, 0}});
    physics.create_body({.shape = Shape3D::capsule(0.5f, 0.25f), .motion = Body3DMotion::Kinematic, .position = {3, 1, 0},
                         .linear_velocity = {0, 0.5f, 0}}); // moving: a kinematic body at rest sleeps too
    physics.create_body({.shape = Shape3D::box({1, 1, 1}), .motion = Body3DMotion::Static, .position = {6, 1, 0},
                         .layer = layers3d::Sensor, .sensor = true});
    for (int i = 0; i < 120; ++i) {
        physics.step(dt);
    }
    CHECK(!physics.is_active(resting));
    DebugDraw3D draw;
    draw.world(physics);
    const DebugDraw3DOptions& o = draw.options();
    const auto count = [&](Color c) {
        return std::ranges::count_if(draw.lines(), [&](const DebugLine3D& l) { return l.color == c; });
    };
    CHECK(count(o.static_body) == 12);   // the floor's box
    CHECK(count(o.sleeping_body) == 12); // the resting box
    CHECK(count(o.dynamic_body) > 12);   // the sphere: three circles
    CHECK(count(o.kinematic_body) > 12); // the capsule
    CHECK(count(o.sensor_body) == 12);
    // The resting box's edges: all on its extents, y from 0 to 1 and x from -3.5 to -2.5.
    for (const DebugLine3D& l : draw.lines()) {
        if (l.color == o.sleeping_body) {
            CHECK(l.a.x > -3.55f && l.a.x < -2.45f && l.a.y > -0.05f && l.a.y < 1.05f);
        }
    }
    // The sphere: every vertex 0.5 from its centre (0, y, 0).
    f32 worst = 0.0f;
    Vec3f centre{};
    for (const Body3D b : physics.bodies()) {
        if (physics.shape(b).kind() == Shape3DKind::Sphere) {
            centre = physics.position(b);
        }
    }
    for (const DebugLine3D& l : draw.lines()) {
        if (l.color == o.dynamic_body) {
            worst = std::max(worst, std::fabs(length(l.a - centre) - 0.5f));
        }
    }
    CHECK(worst < 1e-3f);

    // Queries: a ray and its hit.
    draw.clear();
    const Vec3f from{-3, 5, 0}, to{-3, -5, 0};
    const auto hit = physics.raycast(from, to);
    draw.ray(from, to, hit);
    CHECK(hit && draw.lines().size() == 1 + 3 + 1); // the segment, a cross, the normal
    CHECK(draw.lines().front().color == o.ray && draw.lines().front().b == hit->point);
    draw.clear();
    draw.ray(from, to, std::nullopt);
    CHECK(draw.lines().size() == 1 && draw.lines()[0].b == to);

    // Contacts and normals: from events (persist on: resting contacts every step).
    World3DConfig cfg = serial();
    cfg.persist_events = true;
    World3D resting_world(cfg);
    resting_world.create_body({.shape = Shape3D::box({10, 0.5f, 10}), .motion = Body3DMotion::Static, .position = {0, -0.5f, 0}});
    resting_world.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 0.55f, 0}});
    for (int i = 0; i < 20; ++i) {
        resting_world.step(dt);
    }
    draw.clear();
    draw.contacts(resting_world.events());
    CHECK(count(o.contact_point) >= 3 && count(o.contact_normal) >= 1);
    for (const DebugLine3D& l : draw.lines()) {
        if (l.color == o.contact_normal) {
            CHECK(near(l.b.y - l.a.y, o.normal_length, 1e-3f)); // up, off the floor
        }
    }

    // Constraints: a hinge and a broken one in their colours; the lines are capped.
    World3D joints(serial());
    const Body3D a = joints.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {0, 5, 0}});
    const Body3D b = joints.create_body({.shape = Shape3D::box({0.5f, 0.5f, 0.5f}), .position = {1.5f, 5, 0}});
    Constraint3DDesc hinge;
    hinge.kind = Constraint3DKind::Hinge;
    hinge.a = a;
    hinge.b = b;
    hinge.point = {0.75f, 5, 0};
    hinge.axis = {0, 0, 1};
    const Constraint3D h = joints.create_constraint(hinge);
    DebugDraw3D jd;
    jd.options().bodies = false;
    jd.world(joints);
    CHECK(!jd.lines().empty());
    for (const DebugLine3D& l : jd.lines()) {
        CHECK(l.color == jd.options().constraint);
    }
    joints.set_constraint_enabled(h, false);
    joints.destroy_constraint(h);
    Constraint3DDesc weak = hinge;
    weak.kind = Constraint3DKind::Point;
    weak.a = {};
    weak.point = {1.5f, 5.5f, 0};
    weak.break_force = 1.0f;
    weak.collide_connected = true;
    joints.create_constraint(weak);
    joints.step(dt);
    joints.step(dt);
    jd.clear();
    jd.world(joints);
    CHECK(!jd.lines().empty() && jd.lines().front().color == jd.options().broken_constraint);
    DebugDraw3D capped({.max_lines = 10});
    capped.world(physics);
    CHECK(capped.lines().size() == 10 && capped.truncated());
}

void test_debug_mesh_and_height_field_caps() {
    World3D physics(serial());
    // A 200 x 200 grid mesh: 80,000 triangles, drawn with at most the cap.
    std::vector<Vec3f> vertices;
    std::vector<u32> indices;
    constexpr int n = 200;
    for (int z = 0; z <= n; ++z) {
        for (int x = 0; x <= n; ++x) {
            vertices.push_back({static_cast<f32>(x), 0.0f, static_cast<f32>(z)});
        }
    }
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const u32 i = static_cast<u32>(z * (n + 1) + x);
            indices.insert(indices.end(), {i, i + n + 1, i + 1, i + 1, i + n + 1, i + n + 2});
        }
    }
    physics.create_body({.shape = Shape3D::triangle_mesh(vertices, indices), .motion = Body3DMotion::Static});
    std::array<f32, 64 * 64> heights{};
    for (std::size_t i = 0; i < heights.size(); ++i) {
        heights[i] = static_cast<f32>(i % 7) * 0.1f;
    }
    physics.create_body({.shape = Shape3D::height_field(heights, 64), .motion = Body3DMotion::Static, .position = {300, 0, 0}});
    DebugDraw3D draw;
    draw.world(physics);
    // Two shapes, each at most 600 triangles of three edges, a little over for the stride's rounding.
    CHECK(draw.lines().size() > 600 && draw.lines().size() <= 2 * 3 * 640);
    DebugDraw3D tight({.max_triangles_per_shape = 50});
    tight.world(physics);
    CHECK(tight.lines().size() <= 2 * 3 * 55 && tight.lines().size() > 100);
}

// --- IK floor ---

void test_foot_placement_over_physics() {
    // A physics floor with its top at y = -0.2 and a step (top at y = 0) under the left foot: the planted feet land on
    // what the raycast says, and the pelvis comes down by the lower foot's drop.
    World3D physics(serial());
    const Body3D floor = physics.create_body({.shape = Shape3D::box({10, 0.5f, 10}), .motion = Body3DMotion::Static,
                                              .position = {0, -0.7f, 0}});
    const Body3D step = physics.create_body({.shape = Shape3D::box({0.5f, 0.1f, 1.0f}), .motion = Body3DMotion::Static,
                                             .position = {0.55f, -0.1f, 0}});
    physics.optimize_broad_phase();

    auto model = std::make_shared<Model>();
    model->skeletons.push_back(skin_fixtures::make_biped());
    const Skeleton& sk = model->skeletons[0];
    AnimClip3D idle;
    idle.name = "idle";
    idle.duration = 1.0f;
    idle.channels.resize(sk.joint_count());
    model->clips.push_back(idle);

    kin::EcsWorld ecs;
    WorldRenderState3D state{ecs};
    const flecs::entity e = ecs.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer{.model = model}).set(Animator3D{.clip = "idle"});
    int casts = 0;
    IKConstraint feet;
    feet.name = "feet";
    feet.kind = IKKind::FootPlacer;
    feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
    feet.foot_heights = {0.05f, 0.05f};
    feet.pole = IKConstraint::Pole::ModelDirection;
    feet.pole_vector = {0, 0, 1};
    feet.foot.smooth_seconds = 0.0f;
    feet.foot.ray_down = 0.8f;
    const GroundQuery physics_ground = ground_query_from_world(physics, all_layers3d);
    feet.ground = [&](Vec3f from, Vec3f to) {
        ++casts;
        return physics_ground(from, to);
    };
    e.get_mut<Animator3D>()->add_ik(feet);
    for (int frame = 0; frame < 3; ++frame) {
        state.animate(dt);
        state.propagate_transforms();
        state.apply_ik();
    }
    const Animator3D& a = *e.get<Animator3D>();
    const auto joint = [&](int j) {
        const Mat4f& m = a.ik_world[static_cast<std::size_t>(j)];
        return Vec3f{m.m[12], m.m[13], m.m[14]};
    };
    namespace B = skin_fixtures::biped;
    CHECK(casts >= 6);
    // Left foot (x = +0.1) on the step (y = 0), right (x = -0.1) on the floor (y = -0.2); an ankle 0.05 above its sole.
    CHECK(near(joint(B::ankle_l).y, 0.0f + 0.05f, 2e-3f));
    CHECK(near(joint(B::ankle_r).y, -0.2f + 0.05f, 2e-3f));
    // The sole is on the surface the query reports there.
    const auto under_left = physics.raycast({0.1f, 1, 0}, {0.1f, -2, 0});
    const auto under_right = physics.raycast({-0.1f, 1, 0}, {-0.1f, -2, 0});
    CHECK(under_left && under_left->body == step && under_right && under_right->body == floor);
    CHECK(near(joint(B::ankle_l).y - 0.05f, under_left->point.y, 2e-3f));
    CHECK(near(joint(B::ankle_r).y - 0.05f, under_right->point.y, 2e-3f));
    // The pelvis (rest at y = 1) came down by the lower foot's drop.
    CHECK(near(joint(B::pelvis).y, 0.8f, 5e-3f));
    // Feet stay under their hips (x, z).
    CHECK(near(joint(B::ankle_l).x, 0.1f, 0.02f) && near(joint(B::ankle_r).x, -0.1f, 0.02f));

    // The character's own body is ignored by the query: a capsule round the pelvis would otherwise be "ground".
    const Body3D self = physics.create_body({.shape = Shape3D::capsule(0.6f, 0.3f), .motion = Body3DMotion::Kinematic,
                                             .position = {0, 0.5f, 0}});
    const GroundQuery with_self = ground_query_from_world(physics, all_layers3d);
    const GroundQuery without_self = ground_query_from_world(physics, all_layers3d, self);
    const auto blocked = with_self({0, 2, 0}, {0, -1, 0});
    const auto clear = without_self({0, 2, 0}, {0, -1, 0});
    CHECK(blocked && blocked->position.y > 1.0f);
    CHECK(clear && near(clear->position.y, -0.2f, 1e-3f) && near(clear->normal.y, 1.0f, 1e-3f));
    // A mask: query only layer 7 and nothing is there.
    CHECK(!ground_query_from_world(physics, layer_bit(7))({0, 2, 0}, {0, -1, 0}));
}

// --- colliders from meshes ---

void test_colliders_from_meshes() {
    struct FakeMesh final : IMeshBackend {};
    const Bounds3 bounds{{-1, 0, -2}, {1, 3, 2}};
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    const flecs::entity e = world.entity().set(Transform3D{}).set(MeshRenderer{.mesh = Mesh{std::make_shared<FakeMesh>(), 3, 3, bounds}});
    const auto c = collider_from_mesh(e);
    CHECK(c && c->kind == Collider3D::Kind::Box && near(c->half_extents, {1, 1.5f, 2}, 1e-6f) && near(c->offset, {0, 1.5f, 0}, 1e-6f));
    CHECK(!collider_from_mesh(world.entity()));
    // Through the system: the box covers the mesh's bounds.
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    e.set(*c);
    bodies.sync_bodies();
    const auto top = physics.raycast({0, 10, 0}, {0, -5, 0});
    CHECK(top && near(top->point.y, 3.0f, 1e-3f));
    const auto side = physics.raycast({-10, 1.5f, 0}, {10, 1.5f, 0});
    CHECK(side && near(side->point.x, -1.0f, 1e-3f));

    // From the CPU triangles: a convex hull of a box mesh, and its triangles as a static mesh.
    auto data = std::make_shared<MeshData>(meshes::box({2, 2, 2}));
    const Collider3D hull = collider_hull_from_mesh_data(data);
    const Collider3D tris = collider_triangles_from_mesh_data(data);
    World3D other(serial());
    kin::EcsWorld ecs2;
    WorldPhysicsState3D bodies2{ecs2, other};
    ecs2.raw().entity().set(Transform3D{.position = {0, 1, 0}}).set(hull).set(RigidBody3D{.motion = Body3DMotion::Static});
    ecs2.raw().entity().set(Transform3D{.position = {10, 1, 0}}).set(tris).set(RigidBody3D{.motion = Body3DMotion::Static});
    bodies2.sync_bodies();
    CHECK(other.body_count() == 2);
    const auto h = other.raycast({0.2f, 10, 0.2f}, {0.2f, -5, 0.2f});
    const auto t = other.raycast({10.2f, 10, 0.2f}, {10.2f, -5, 0.2f});
    CHECK(h && near(h->point.y, 2.0f, 1e-3f));
    CHECK(t && near(t->point.y, 2.0f, 1e-3f));
    // A dynamic body with a triangle mesh is refused (and does not stop the others).
    ecs2.raw().entity().set(Transform3D{.position = {20, 1, 0}}).set(tris).set(RigidBody3D{});
    bodies2.sync_bodies();
    CHECK(other.body_count() == 2 && bodies2.body_count() == 2);
}

} // namespace

int main() {
    test_bodies_in_entity_order();
    test_scale_offset_and_compound();
    test_dynamic_writes_back();
    test_parented_bodies();
    test_kinematic_follows_and_pushes();
    test_game_moves_entities();
    test_events_routed();
    test_scene_determinism();
    test_interpolation();
    test_debug_lines();
    test_debug_mesh_and_height_field_caps();
    test_foot_placement_over_physics();
    test_colliders_from_meshes();
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_ecs_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_ecs_tests: all passed\n");
    return 0;
}
