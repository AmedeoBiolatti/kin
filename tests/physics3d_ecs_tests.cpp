// The ECS side of kin/physics3d (kin/ecs/physics3d.hpp), without a GPU: bodies made for entities in entity id order
// and destroyed with them, kinematic bodies following Transform3D and dynamic ones writing it back (under a
// parent too), events routed to a place game code reads, determinism of a whole scene, debug lines, the ground
// query of foot placement over a physics floor and step, colliders from meshes. docs/physics_3d.md.

#include "skin_fixtures.hpp"

#include <kin/core/jobs.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/mesh3d.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
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
    c.contact_events = true;
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

void test_disabled_contacts_readable() {
    kin::EcsWorld ecs;
    World3DConfig config;
    config.single_threaded = true;
    CHECK(!config.contact_events);
    World3D physics(config);
    WorldPhysicsState3D bodies{ecs, physics};
    CHECK(ecs.raw().get<PhysicsContacts3D>() != nullptr);
    add_floor(ecs.raw());
    add_crate(ecs.raw(), {0, .5f, 0});
    for (int i = 0; i < 3; ++i)
        bodies.step(dt);
    CHECK(ecs.raw().get<PhysicsContacts3D>()->events.empty());
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
    CHECK(world.get<PhysicsContacts3D>() != nullptr);
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
    // The same compound with an offset of its own (Jolt wraps the placed compound in a decorated shape).
    // Copy before set(), which takes the collider's owned children.
    Collider3D placed = compound;
    placed.offset = {0, 0, 3};
    world.entity().set(Transform3D{.position = {20, 1, 0}}).set(compound);
    world.entity().set(Transform3D{.position = {30, 1, 0}}).set(placed);
    bodies.sync_bodies();
    const auto top = [&](f32 x) { return physics.raycast({x, 10, 0}, {x, -5, 0}); };
    CHECK(top(0.0f) && near(top(0.0f)->point.y, 2.0f, 1e-3f));
    CHECK(top(0.9f) && near(top(0.9f)->point.y, 2.0f, 1e-3f) && !top(1.1f));
    CHECK(!top(10.0f) && top(13.0f) && near(top(13.0f)->point.y, 1.5f, 1e-3f));
    const auto l = top(19.0f);
    const auto r = top(21.0f);
    CHECK(l && l->sub_shape == 0 && near(l->point.y, 1.5f, 1e-3f));
    CHECK(r && r->sub_shape == 1 && near(r->point.y, 1.5f, 1e-3f));
    // The offset compound's children are told apart too, by every query.
    const auto pl = physics.raycast({29, 10, 3}, {29, -5, 3});
    const auto pr = physics.raycast({31, 10, 3}, {31, -5, 3});
    CHECK(pl && pl->sub_shape == 0 && near(pl->point.y, 1.5f, 1e-3f));
    CHECK(pr && pr->sub_shape == 1 && near(pr->point.y, 1.5f, 1e-3f));
    CHECK(!physics.raycast({31, 10, 0}, {31, -5, 0}));
    const auto cast = physics.shape_cast(Shape3D::sphere(0.1f), {31, 10, 3}, {}, {0, -15, 0});
    CHECK(cast && cast->sub_shape == 1);
    const auto over = physics.overlap(Shape3D::sphere(0.1f), {31, 1, 3}, {});
    CHECK(over.size() == 1 && over[0].sub_shape == 1);
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

void test_read_only_render_snapshots() {
    kin::EcsWorld ecs;
    auto& world = ecs.raw();
    auto config = serial();
    config.gravity = {};
    World3D physics(config);
    WorldPhysicsState3D bodies{ecs, physics};
    const auto moving = add_crate(world, {});
    moving.get_mut<RigidBody3D>()->linear_velocity = {6, 0, 0};
    moving.get_mut<RigidBody3D>()->linear_damping = 0;
    const auto platform = world.entity().set(Transform3D{.position = {10, 0, 0}})
        .set(box_collider({1, .2f, 1})).set(RigidBody3D{.motion = Body3DMotion::Kinematic});
    const auto child = world.entity().child_of(moving).set(Transform3D{.position = {2, 0, 0}});
    bodies.sync_bodies();
    platform.get_mut<Transform3D>()->position.x = 12;
    bodies.step(dt);
    const auto hash = physics.state_hash();
    const auto midpoint = bodies.render_transform(moving, .5f);
    const auto child_midpoint = bodies.render_transform(child, .5f);
    const auto platform_midpoint = bodies.render_transform(platform, .5f);
    CHECK(midpoint && near(midpoint->m[12], .05f, .001f));
    CHECK(child_midpoint && near(child_midpoint->m[12], 2.05f, .001f));
    CHECK(platform_midpoint && near(platform_midpoint->m[12], 11, .001f));
    CHECK(near(moving.get<Transform3D>()->position.x, .1f, .001f));
    CHECK(platform.get<Transform3D>()->position.x == 12 && physics.state_hash() == hash);
    // Late edits (including an ancestor edit) snap without moving the body.
    moving.get_mut<Transform3D>()->position.x = 5;
    CHECK(near(bodies.render_transform(child, 0)->m[12], 7, .001f));
    CHECK(physics.state_hash() == hash);
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

// Game code moving a dynamic entity is a teleport: the interpolation starts again from there (no lerp across the jump).
void test_teleported_entity_interpolates_from_there() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity crate = add_crate(world, {0, 10, 0});
    for (int i = 0; i < 6; ++i) {
        bodies.step(dt);
    }
    crate.get_mut<Transform3D>()->position = {8, 7, 0};
    bodies.step(dt);
    const Vec3f after = physics.position(bodies.body_of(crate));
    CHECK(near(after.x, 8.0f, 1e-5f) && after.y < 7.0f && after.y > 6.9f);
    bodies.interpolate(0.0f);
    CHECK(near(crate.get<Transform3D>()->position, {8, 7, 0}, 1e-5f));
    bodies.interpolate(0.5f);
    CHECK(near(crate.get<Transform3D>()->position, {8, 0.5f * (7.0f + after.y), 0}, 1e-5f));
    bodies.interpolate(1.0f);
    CHECK(near(crate.get<Transform3D>()->position, after, 1e-6f));
    // The interpolated pose is not read as a move: the body goes on falling from where it is.
    bodies.step(dt);
    CHECK(physics.position(bodies.body_of(crate)).y < after.y && near(physics.position(bodies.body_of(crate)).x, 8.0f, 1e-5f));
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

    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(skin_fixtures::make_biped());
    const Skeleton3D& sk = model->skeletons[0];
    AnimClip3D idle;
    idle.name = "idle";
    idle.duration = 1.0f;
    idle.channels.resize(sk.joint_count());
    model->clips.push_back(idle);

    kin::EcsWorld ecs;
    WorldRenderState3D state{ecs};
    const flecs::entity e = ecs.raw().entity().set(Transform3D{}).set(SkinnedMeshRenderer3D{.model = model}).set(Animator3D{.clip = "idle"});
    int casts = 0;
    IKConstraint3D feet;
    feet.name = "feet";
    feet.kind = IKKind3D::FootPlacer;
    feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
    feet.foot_heights = {0.05f, 0.05f};
    feet.pole = IKConstraint3D::Pole::ModelDirection;
    feet.pole_vector = {0, 0, 1};
    feet.foot.smooth_seconds = 0.0f;
    feet.foot.ray_down = 0.8f;
    const GroundQuery3D physics_ground = ground_query_from_world(physics, all_layers3d);
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
    const GroundQuery3D with_self = ground_query_from_world(physics, all_layers3d);
    const GroundQuery3D without_self = ground_query_from_world(physics, all_layers3d, self);
    const auto blocked = with_self({0, 2, 0}, {0, -1, 0});
    const auto clear = without_self({0, 2, 0}, {0, -1, 0});
    CHECK(blocked && blocked->position.y > 1.0f);
    CHECK(clear && near(clear->position.y, -0.2f, 1e-3f) && near(clear->normal.y, 1.0f, 1e-3f));
    // A mask: query only layer 7 and nothing is there.
    CHECK(!ground_query_from_world(physics, layer_bit(7))({0, 2, 0}, {0, -1, 0}));
}

// --- colliders from meshes ---

void test_colliders_from_meshes() {
    struct FakeMesh final : IMeshBackend3D {};
    const Bounds3D bounds{{-1, 0, -2}, {1, 3, 2}};
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    const flecs::entity e = world.entity().set(Transform3D{}).set(MeshRenderer3D{.mesh = Mesh3D{std::make_shared<FakeMesh>(), 3, 3, bounds}});
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
    auto data = std::make_shared<MeshData3D>(meshes::box({2, 2, 2}));
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


// --- characters ---------------------------------------------------------------------------------------------------------

flecs::entity add_character(flecs::world& world, Vec3f at, CharacterDesc3D desc = {}) {
    CharacterController3D c;
    c.desc = desc;
    return world.entity().set(Transform3D{.position = at}).set(c);
}

void press(flecs::entity e, Vec2f move, Vec3f forward = {1, 0, 0}, bool jump = false) {
    CharacterController3D* c = e.get_mut<CharacterController3D>();
    c->input.move = move;
    c->input.forward = forward;
    c->input.jump = jump;
}

void test_character_entity() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    const flecs::entity floor = add_floor(world);
    const flecs::entity crate = add_crate(world, {5, 0.5f, 3});
    // A collider on the same entity makes no second body: the controller is its capsule.
    const flecs::entity hero = add_character(world, {0, 0.5f, 0});
    hero.set(box_collider({0.5f, 0.9f, 0.5f}));
    bodies.sync_bodies();
    // Bodies first (entity order), then the character's capsule after them.
    CHECK(bodies.body_count() == 2);
    CHECK(physics.body_count() == 3 && physics.character_count() == 1 && bodies.character_count() == 1);
    const Character3D handle = bodies.character_of(hero);
    CHECK(handle.valid() && hero.get<PhysicsCharacter3D>() != nullptr && hero.get<PhysicsCharacter3D>()->character == handle);
    CHECK(physics.character(handle).body.index == 2);
    CHECK(physics.user_id(physics.character(handle).body) == hero.id());

    // It settles on the floor, written back into the entity.
    for (int i = 0; i < 60; ++i) {
        bodies.step(dt);
    }
    CHECK(near(hero.get<Transform3D>()->position.y, 0.0f, 0.03f));
    CHECK(hero.get<CharacterController3D>()->state.ground == CharacterGround3D::OnGround);
    CHECK(hero.get<CharacterController3D>()->state.ground_user_id == floor.id());
    // Walk: position follows, the facing the game sets goes to the rotation (physics does not turn it).
    press(hero, {0, 1});
    hero.get_mut<CharacterController3D>()->facing = Quatf::axis_angle({0, 1, 0}, 90.0f);
    for (int i = 0; i < 60; ++i) {
        bodies.step(dt);
    }
    CHECK(hero.get<Transform3D>()->position.x > 3.0f);
    CHECK(near(hero.get<CharacterController3D>()->state.ground_speed, 4.0f, 0.05f));
    CHECK(hero.get<Transform3D>()->rotation == Quatf::axis_angle({0, 1, 0}, 90.0f));
    // With apply_facing off the rotation is the game's own.
    hero.get_mut<CharacterController3D>()->apply_facing = false;
    hero.get_mut<Transform3D>()->rotation = Quatf::axis_angle({0, 1, 0}, 30.0f);
    bodies.step(dt);
    CHECK(hero.get<Transform3D>()->rotation == Quatf::axis_angle({0, 1, 0}, 30.0f));

    // Game code moving the entity is a teleport (velocity cleared).
    hero.get_mut<Transform3D>()->position = {-10, 3, 0};
    press(hero, {0, 0});
    bodies.step(dt);
    CHECK(near(hero.get<Transform3D>()->position.x, -10.0f, 0.1f));
    for (int i = 0; i < 90; ++i) {
        bodies.step(dt);
    }
    CHECK(near(hero.get<Transform3D>()->position.x, -10.0f, 0.1f) && near(hero.get<Transform3D>()->position.y, 0.0f, 0.03f));

    // Events with entity ids: a jump and a landing; the singleton carries them too.
    std::vector<CharacterEvent3D> seen;
    press(hero, {0, 0}, {1, 0, 0}, true);
    for (int i = 0; i < 90; ++i) {
        bodies.step(dt);
        if (i == 0) {
            press(hero, {0, 0}, {1, 0, 0}, false);
        }
        for (const CharacterEvent3D& e : bodies.character_events()) {
            seen.push_back(e);
        }
        if (!bodies.character_events().empty()) {
            const CharacterEvents3D* singleton = world.get<CharacterEvents3D>();
            CHECK(singleton != nullptr && singleton->events.size() == bodies.character_events().size());
        }
    }
    bool left = false, landed = false;
    for (const CharacterEvent3D& e : seen) {
        CHECK(e.user_id == hero.id());
        left = left || (e.kind == CharacterEventKind3D::LeftGround && e.jumped);
        landed = landed || (e.kind == CharacterEventKind3D::Landed && e.body_user_id == floor.id());
    }
    CHECK(left && landed);
    // The entity going takes the character and its body.
    const i32 before = physics.body_count();
    hero.destruct();
    bodies.sync_bodies();
    CHECK(physics.character_count() == 0 && bodies.character_count() == 0 && physics.body_count() == before - 1);
    (void)crate;
}

void test_character_carried_by_kinematic_platform() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity platform = world.entity()
                                       .set(Transform3D{.position = {0, 0.25f, 0}})
                                       .set(box_collider({2.0f, 0.25f, 2.0f}))
                                       .set(RigidBody3D{.motion = Body3DMotion::Kinematic, .friction = 0.9f});
    const flecs::entity hero = add_character(world, {0, 0.5f, 0});
    for (int i = 0; i < 30; ++i) {
        bodies.step(dt);
    }
    CHECK(hero.get<CharacterController3D>()->state.ground_user_id == platform.id());
    // The game moves the platform's entity 2 m/s along X; the character on it goes along without any input.
    for (int i = 0; i < 180; ++i) {
        platform.get_mut<Transform3D>()->position.x += 2.0f * dt;
        bodies.step(dt);
    }
    CHECK(near(platform.get<Transform3D>()->position.x, 6.0f, 1e-3f));
    CHECK(near(hero.get<Transform3D>()->position.x, 6.0f, 0.06f));
    CHECK(near(hero.get<Transform3D>()->position.y, 0.5f, 0.03f));
    CHECK(near(hero.get<CharacterController3D>()->state.ground_velocity.x, 2.0f, 0.1f));
}

void test_character_parented_and_interpolated() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    // Under a parent at x = 10: the character is made at the parent's world place of its local position, and its
    // Transform3D is rewritten as its place seen from the parent.
    const flecs::entity parent = world.entity().set(Transform3D{.position = {10, 0, 0}});
    const flecs::entity hero = add_character(world, {2, 0.5f, 0});
    hero.child_of(parent);
    bodies.sync_bodies();
    const Character3D handle = bodies.character_of(hero);
    CHECK(near(physics.character(handle).position.x, 12.0f, 1e-3f));
    for (int i = 0; i < 60; ++i) {
        bodies.step(dt);
    }
    CHECK(near(physics.character(handle).position.x, 12.0f, 1e-3f));
    CHECK(near(hero.get<Transform3D>()->position.x, 2.0f, 1e-3f) && near(hero.get<Transform3D>()->position.y, 0.0f, 0.03f));
    press(hero, {0, 1});
    for (int i = 0; i < 30; ++i) {
        bodies.step(dt);
    }
    // Interpolation: between the last two steps' feet, alpha 0 the earlier; alpha 1 what step leaves.
    const f32 x_now = hero.get<Transform3D>()->position.x;
    bodies.interpolate(0.0f);
    const f32 x_before = hero.get<Transform3D>()->position.x;
    bodies.interpolate(0.5f);
    const f32 x_half = hero.get<Transform3D>()->position.x;
    bodies.interpolate(1.0f);
    CHECK(near(hero.get<Transform3D>()->position.x, x_now, 1e-4f));
    CHECK(x_before < x_half && x_half < x_now && near(x_now - x_before, 4.0f * dt, 0.02f));
    CHECK(near(x_half, 0.5f * (x_before + x_now), 1e-4f));
    // An interpolated pose is not read as a move by the next step.
    bodies.step(dt);
    CHECK(hero.get<Transform3D>()->position.x > x_now);
}

void test_character_animation_link() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity hero = add_character(world, {0, 0, 0});
    Animator3D animator = locomotion_animator();
    IKConstraint3D feet;
    feet.name = "feet";
    feet.kind = IKKind3D::FootPlacer;
    feet.weight = 0.0f;
    animator.add_ik(feet);
    hero.set(static_cast<const Animator3D&>(animator));
    hero.set(CharacterAnimationLink3D{});
    for (int i = 0; i < 30; ++i) {
        bodies.step(dt);
    }
    const auto params = [&]() -> const AnimationParams& { return hero.get<Animator3D>()->params; };
    CHECK(near(params().get_float("speed"), 0.0f, 0.01f));
    CHECK(!params().get_bool("airborne"));
    // Its foot placer is given the physics floor as the ground, weight 1, and the capsule is not part of it.
    const IKConstraint3D* placer = hero.get<Animator3D>()->ik_constraints.empty() ? nullptr : &hero.get<Animator3D>()->ik_constraints[0];
    CHECK(placer != nullptr && placer->ground && near(placer->weight, 1.0f, 1e-6f));
    if (placer != nullptr && placer->ground) {
        const auto hit = placer->ground({0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f});
        CHECK(hit.has_value() && near(hit->position.y, 0.0f, 1e-3f) && near(hit->normal.y, 1.0f, 1e-3f));
        // From inside the capsule (its middle), a downward ray starts in the character's body: ignored, so the floor.
        const auto from_inside = placer->ground({0.0f, 0.9f, 0.0f}, {0.0f, -0.5f, 0.0f});
        CHECK(from_inside.has_value() && near(from_inside->position.y, 0.0f, 1e-3f));
    }
    // Walking: the speed parameter is the ground speed; jumping: airborne, foot weight 0; back down: grounded again.
    press(hero, {0, 1});
    for (int i = 0; i < 40; ++i) {
        bodies.step(dt);
    }
    CHECK(near(params().get_float("speed"), 4.0f, 0.05f));
    press(hero, {0, 1}, {1, 0, 0}, true);
    bodies.step(dt);
    press(hero, {0, 1}, {1, 0, 0}, false);
    for (int i = 0; i < 10; ++i) {
        bodies.step(dt);
    }
    CHECK(params().get_bool("airborne"));
    CHECK(params().get_float("vertical") > 1.0f);
    CHECK(near(hero.get<Animator3D>()->ik_constraints[0].weight, 0.0f, 1e-6f));
    for (int i = 0; i < 90; ++i) {
        bodies.step(dt);
    }
    CHECK(!params().get_bool("airborne"));
    CHECK(near(hero.get<Animator3D>()->ik_constraints[0].weight, 1.0f, 1e-6f));
    // The machine the helper makes: ground state with a blend over speed, an air state, transitions on `airborne`.
    const Animator3D made = locomotion_animator({.idle = "idle", .walk = "walk", .run = "run", .fall = "fall"});
    CHECK(made.machine && made.machine->initial == "ground" && made.machine->transitions.size() == 2);
    CHECK(made.blend_spaces.at("ground").entries.size() == 3 && made.blend_spaces.at("ground").parameter == "speed");
}


// The foot placer on a character's own capsule: the biped of the P1b fixture stands on a floor (y = 0) with a 10 cm step in a
// layer the character walks through but the foot rays hit, under its left foot. The ground the system hands the placer is the
// physics world with the capsule left out; the ankles land on what it says.
void test_character_feet_on_physics_ground() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity step = world.entity()
                                   .set(Transform3D{.position = {0.55f, 0.05f, 0}})
                                   .set(box_collider({0.5f, 0.05f, 1.0f}))
                                   .set(RigidBody3D{.motion = Body3DMotion::Static, .layer = layers3d::User0});
    (void)step;

    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(skin_fixtures::make_biped());
    const Skeleton3D& sk = model->skeletons[0];
    AnimClip3D idle;
    idle.name = "idle";
    idle.duration = 1.0f;
    idle.channels.resize(sk.joint_count());
    model->clips.push_back(idle);

    CharacterDesc3D desc;
    desc.collides_with = all_layers3d & ~layer_bit(layers3d::User0);
    CharacterController3D controller;
    controller.desc = desc;
    Animator3D animator = locomotion_animator({.idle = "idle", .walk = "", .run = "", .fall = "idle"});
    IKConstraint3D feet;
    feet.name = "feet";
    feet.kind = IKKind3D::FootPlacer;
    feet.joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"};
    feet.foot_heights = {0.05f, 0.05f};
    feet.pole = IKConstraint3D::Pole::ModelDirection;
    feet.pole_vector = {0, 0, 1};
    feet.foot.smooth_seconds = 0.0f;
    feet.foot.ray_down = 0.8f;
    animator.add_ik(feet);
    const flecs::entity hero = world.entity()
                                   .set(Transform3D{.position = {0, 0.02f, 0}})
                                   .set(controller)
                                   .set(SkinnedMeshRenderer3D{.model = model})
                                   .set(static_cast<const Animator3D&>(animator))
                                   .set(CharacterAnimationLink3D{});
    WorldRenderState3D state{ecs};
    for (int frame = 0; frame < 60; ++frame) {
        bodies.step(dt);
        state.animate(dt);
        state.propagate_transforms();
        state.apply_ik();
    }
    CHECK(near(hero.get<Transform3D>()->position.y, 0.0f, 0.03f));
    CHECK(hero.get<CharacterController3D>()->state.ground == CharacterGround3D::OnGround);
    const Animator3D& a = *hero.get<Animator3D>();
    const auto joint = [&](int j) {
        const Mat4f& m = a.ik_world[static_cast<std::size_t>(j)];
        return Vec3f{m.m[12], m.m[13], m.m[14]};
    };
    namespace B = skin_fixtures::biped;
    // Left foot (x = +0.1) on the step (top y = 0.1), right (x = -0.1) on the floor (y = 0): the ankle 0.05 above each sole,
    // and the capsule round the pelvis was not taken for ground.
    CHECK(near(joint(B::ankle_l).y, 0.1f + 0.05f, 4e-3f));
    CHECK(near(joint(B::ankle_r).y, 0.0f + 0.05f, 4e-3f));
    CHECK(near(joint(B::ankle_l).x, 0.1f, 0.02f) && near(joint(B::ankle_r).x, -0.1f, 0.02f));
}

// A character's own state hashed with the bodies', per step: the same across runs and worker counts.
std::vector<u64> character_scene_hashes(World3DConfig cfg, int frames) {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(cfg);
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    for (int i = 0; i < 12; ++i) {
        add_crate(world, {static_cast<f32>(i % 4) * 1.2f + 2.0f, 0.5f + static_cast<f32>(i / 4) * 1.1f, static_cast<f32>(i % 3) - 1.0f});
    }
    std::vector<flecs::entity> heroes;
    for (int i = 0; i < 4; ++i) {
        heroes.push_back(add_character(world, {static_cast<f32>(i) * 2.0f - 3.0f, 0.2f, 4.0f - static_cast<f32>(i)}));
    }
    std::vector<u64> out;
    u32 seed = 77;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < heroes.size(); ++i) {
            seed = seed * 1664525u + 1013904223u;
            const f32 a = static_cast<f32>(seed >> 8) / static_cast<f32>(1u << 24);
            press(heroes[i], {a - 0.5f, 0.8f}, {1, 0, 0}, (f + static_cast<int>(i) * 17) % 70 == 0);
        }
        bodies.step(dt);
        out.push_back(physics.state_hash());
    }
    return out;
}

void test_character_scene_determinism() {
    constexpr int frames = 150;
    const std::vector<u64> a = character_scene_hashes(serial(), frames);
    CHECK(a == character_scene_hashes(serial(), frames));
    JobSystem four({.workers = 4, .max_background = 4});
    World3DConfig cfg;
    cfg.jobs = &four;
    CHECK(character_scene_hashes(cfg, frames) == a);
    CHECK(a.front() != a.back());
}


// --- grab and climb ---------------------------------------------------------------------------------------------------

// A wall (z from -1.5 to 0, 4 m tall) with a hand-sized hold on its face, a crate and a character in front of it.
struct ClimbScene {
    kin::EcsWorld ecs;
    World3D physics{serial()};
    WorldPhysicsState3D bodies{ecs, physics};
    flecs::entity wall, hold, hero;

    explicit ClimbScene(CharacterDesc3D desc = {}, Vec3f at = {0.0f, 1.6f, 0.7f}) {
        flecs::world& world = ecs.raw();
        add_floor(world);
        wall = world.entity()
                   .set(Transform3D{.position = {0, 2.0f, -0.75f}})
                   .set(box_collider({3.0f, 2.0f, 0.75f}))
                   .set(RigidBody3D{.motion = Body3DMotion::Static})
                   .set(SurfaceMaterial3D{.grip = 0.9f, .slip = 0.0f, .drain = 1.5f, .tag = 42});
        hold = world.entity()
                   .set(Transform3D{.position = {2.0f, 1.5f, 0.075f}})
                   .set(box_collider({0.15f, 0.06f, 0.075f}))
                   .set(RigidBody3D{.motion = Body3DMotion::Static})
                   .set(Hold3D{.quality = 0.7f});
        desc.traversal.enabled = true;
        CharacterController3D c;
        c.desc = desc;
        c.input.forward = {0, 0, -1};
        hero = world.entity().set(Transform3D{.position = at}).set(c);
    }
    CharacterController3D& controller() { return *hero.get_mut<CharacterController3D>(); }
};

void test_traversal_entities() {
    ClimbScene scene;
    scene.bodies.sync_bodies();
    // What a hand finds is read from the components when the body is made.
    const Body3D wall_body = scene.bodies.body_of(scene.wall);
    CHECK(scene.physics.surface_material(wall_body).tag == 42);
    CHECK(near(scene.physics.surface_material(wall_body).grip, 0.9f, 1e-6f) && near(scene.physics.surface_material(wall_body).drain, 1.5f, 1e-6f));
    const Body3D hold_body = scene.bodies.body_of(scene.hold);
    CHECK(scene.physics.is_hold(hold_body) && near(scene.physics.hold(hold_body).quality, 0.7f, 1e-6f));
    CHECK(!scene.physics.is_hold(wall_body));
    // A component changed on a live body is read at the next rebuild.
    scene.wall.set(SurfaceMaterial3D{.tag = 43});
    CHECK(scene.physics.surface_material(wall_body).tag == 42);
    scene.bodies.rebuild(scene.wall);
    scene.bodies.sync_bodies();
    CHECK(scene.physics.surface_material(scene.bodies.body_of(scene.wall)).tag == 43);
    scene.wall.set(SurfaceMaterial3D{.grip = 0.9f, .slip = 0.0f, .drain = 1.5f, .tag = 42});
    scene.bodies.rebuild(scene.wall);

    // The command is sent, the state comes back: the character takes hold of the wall and hangs.
    scene.bodies.sync_bodies(); // (adds the character's handle component: take references to components after it)
    CharacterController3D* c = &scene.controller();
    c->traversal.grab = {true, true};
    c->traversal.reach = {0.0f, 0.0f, -1.0f};
    for (int i = 0; i < 60; ++i) {
        scene.bodies.step(dt);
    }
    const CharacterController3D& after = *scene.hero.get<CharacterController3D>();
    CHECK(after.traversal_state.enabled && after.traversal_state.mode == TraversalMode3D::Hanging);
    CHECK(after.traversal_state.held == 2);
    CHECK(after.state.traversal_mode == TraversalMode3D::Hanging);
    CHECK(scene.bodies.traversal_of(scene.hero).held == 2);
    CHECK(after.traversal_state.grips[0].body == scene.bodies.body_of(scene.wall));
    CHECK(after.traversal_state.grips[0].surface_tag == 42);
    // Its events come with the entity ids: two Gripped, naming the character and the wall.
    int gripped = 0;
    for (const CharacterEvent3D& e : scene.bodies.character_events()) {
        gripped += e.kind == CharacterEventKind3D::Gripped ? 1 : 0;
    }
    int gripped_total = 0;
    // (events are per step: look in the singleton of a step that had them by running the grab again below)
    (void)gripped;
    (void)gripped_total;
    // It faces the wall: the model's +Z is the way it looks (-Z, into the wall).
    const Vec3f looks = scene.hero.get<Transform3D>()->rotation.rotate({0, 0, 1});
    CHECK(near(looks.z, -1.0f, 0.05f));
    // And its feet are where the hang puts them: a capsule out from the wall, the hands 1.7 above.
    CHECK(near(scene.hero.get<Transform3D>()->position.z, 0.34f, 0.04f));
    // The resource is in the state.
    CHECK(after.traversal_state.resource.max == 100.0f && after.traversal_state.resource.value < 100.0f);
    // Letting go: it falls, the events of that step name the entity.
    c = &scene.controller();
    c->traversal.grab = {false, false};
    bool fell = false;
    for (int i = 0; i < 10 && !fell; ++i) {
        scene.bodies.step(dt);
        for (const CharacterEvent3D& e : scene.bodies.character_events()) {
            if (e.kind == CharacterEventKind3D::GripReleased) {
                CHECK(e.user_id == scene.hero.id() && e.body_user_id == scene.wall.id());
            }
            if (e.kind == CharacterEventKind3D::Fell) {
                fell = true;
                CHECK(e.user_id == scene.hero.id());
            }
        }
    }
    CHECK(fell);
    CHECK(scene.hero.get<CharacterController3D>()->state.traversal_mode == TraversalMode3D::Walking);
    // The authored hold: reaching for it takes the hold's quality.
    c = &scene.controller();
    c->traversal.grab = {false, true};
    c->traversal.reach = {0.0f, 0.0f, -1.0f};
    scene.physics.teleport_character(scene.bodies.character_of(scene.hero), {1.8f, 0.0f, 0.7f});
    for (int i = 0; i < 40; ++i) {
        scene.bodies.step(dt);
    }
    const TraversalState3D t = scene.bodies.traversal_of(scene.hero);
    CHECK(t.grips[1].phase == GripPhase3D::Held && t.grips[1].kind == GripKind3D::Hold);
    CHECK(near(t.grips[1].strength, 0.7f, 1e-4f));
    CHECK(t.grips[1].body_user_id == scene.hold.id());
}

void test_traversal_entities_events() {
    // The events of the step, in the singleton too, with entity ids (a step with Gripped events).
    ClimbScene scene({}, {0.0f, 1.6f, 0.7f});
    scene.bodies.sync_bodies();
    CharacterController3D& c = scene.controller();
    c.traversal.grab = {true, true};
    c.traversal.reach = {0.0f, 0.0f, -1.0f};
    int gripped = 0;
    for (int i = 0; i < 40; ++i) {
        scene.bodies.step(dt);
        for (const CharacterEvent3D& e : scene.bodies.character_events()) {
            if (e.kind == CharacterEventKind3D::Gripped) {
                ++gripped;
                CHECK(e.user_id == scene.hero.id() && e.body_user_id == scene.wall.id() && e.hand == static_cast<u8>(gripped - 1));
                CHECK(near(e.point.z, 0.0f, 1e-3f));
            }
        }
        const CharacterEvents3D* singleton = scene.ecs.raw().get<CharacterEvents3D>();
        if (singleton != nullptr) {
            CHECK(singleton->events.size() == scene.bodies.character_events().size());
        }
    }
    CHECK(gripped == 2);
}

void test_traversal_hands_ik() {
    // The biped's two hands go to the two anchors by IK (limb 0 the left hand), from the animation link, while it hangs; and
    // the hands go free (weight 0) when it lets go.
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(skin_fixtures::make_biped());
    const Skeleton3D& sk = model->skeletons[0];
    AnimClip3D idle;
    idle.name = "idle";
    idle.duration = 1.0f;
    idle.channels.resize(sk.joint_count());
    model->clips.push_back(idle);
    CharacterDesc3D desc;
    desc.height = 1.95f;
    desc.traversal.limbs[0].shoulder = {-0.2f, 1.65f, 0.0f};
    desc.traversal.limbs[1].shoulder = {0.2f, 1.65f, 0.0f};
    ClimbScene scene(desc);
    Animator3D animator = locomotion_animator({.idle = "idle", .walk = "", .run = "", .fall = "idle", .hang = "idle"});
    for (const char* side : {"L", "R"}) {
        IKConstraint3D hand;
        hand.name = std::string("hand_") + side;
        hand.kind = IKKind3D::TwoBone;
        hand.joints = {std::string("shoulder_") + side, std::string("elbow_") + side, std::string("hand_") + side};
        hand.weight = 0.0f;
        hand.fade_seconds = 0.0f;
        hand.pole = IKConstraint3D::Pole::ModelDirection;
        hand.pole_vector = {0.0f, -0.3f, -1.0f};
        animator.add_ik(hand);
    }
    CHECK(animator.machine && animator.machine->transitions.size() == 6 && animator.machine->states.count("hang") == 1);
    scene.hero.set(SkinnedMeshRenderer3D{.model = model});
    scene.hero.set(static_cast<const Animator3D&>(animator));
    CharacterAnimationLink3D link;
    link.hands = {"hand_L", "hand_R"};
    scene.hero.set(link);
    WorldRenderState3D state{scene.ecs};
    scene.bodies.sync_bodies();
    scene.controller().traversal.grab = {true, true};
    scene.controller().traversal.reach = {0.0f, 0.0f, -1.0f};
    const auto step = [&] {
        scene.bodies.step(dt);
        state.animate(dt);
        state.propagate_transforms();
        state.apply_ik();
    };
    for (int i = 0; i < 70; ++i) {
        step();
    }
    CHECK(scene.bodies.traversal_of(scene.hero).held == 2);
    const Animator3D& a = *scene.hero.get<Animator3D>();
    CHECK(a.params.get_bool("hanging"));
    CHECK(near(a.ik_constraints[0].weight, 1.0f, 1e-6f) && near(a.ik_constraints[1].weight, 1.0f, 1e-6f));
    const Mat4f world = scene.hero.get<Transform3D>()->matrix();
    const auto hand_world = [&](int joint) { return world.apply(Vec3f{a.ik_world[static_cast<std::size_t>(joint)].m[12],
                                                                       a.ik_world[static_cast<std::size_t>(joint)].m[13],
                                                                       a.ik_world[static_cast<std::size_t>(joint)].m[14]}); };
    namespace B = skin_fixtures::biped;
    const TraversalState3D t = scene.bodies.traversal_of(scene.hero);
    // Each hand within 2 cm of its anchor (the left model hand is the character's left: limb 0).
    CHECK(near(hand_world(B::hand_l), t.grips[0].point, 0.02f));
    CHECK(near(hand_world(B::hand_r), t.grips[1].point, 0.02f));
    // Let go: the weights fade to 0 (at once here: fade_seconds 0), the machine is not hanging.
    scene.controller().traversal.grab = {false, false};
    for (int i = 0; i < 6; ++i) {
        step();
    }
    CHECK(near(a.ik_constraints[0].weight, 0.0f, 1e-6f) && near(a.ik_constraints[1].weight, 0.0f, 1e-6f));
    CHECK(!a.params.get_bool("hanging"));
}

// --- refusals and rollback ---------------------------------------------------------------------------------------------------

// The warnings that the bridge logs (category physics3d) while it lives, counted by a piece of their text.
struct Warnings {
    std::vector<LogEvent> events;
    LoggerConfig saved = logger_config();
    Warnings() { set_logger_config({.min_level = LogLevel::Warn, .sdl_sink = false, .memory_events = &events}); }
    ~Warnings() { set_logger_config(saved); }
    std::ptrdiff_t count(std::string_view text) const {
        return std::ranges::count_if(events, [&](const LogEvent& e) {
            return e.level == LogLevel::Warn && e.category == "physics3d" && e.message.find(text) != std::string::npos;
        });
    }
};

void test_refused_bodies_are_tried_once() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    Warnings warnings;

    // A dynamic triangle mesh is refused by the world. Ten steps ask it once, not ten times (each ask cooks the mesh and warns).
    const Collider3D triangles = collider_triangles_from_mesh_data(std::make_shared<MeshData3D>(meshes::box({2, 2, 2})));
    flecs::entity wall = world.entity().set(Transform3D{.position = {0, 3, 0}}).set(triangles).set(RigidBody3D{});
    for (int i = 0; i < 10; ++i) {
        bodies.step(dt);
    }
    CHECK(bodies.body_count() == 0 && physics.body_count() == 0);
    CHECK(warnings.count("no valid body") == 1);
    // Changing what it was refused for asks again, and it works as a static body.
    wall.set(RigidBody3D{.motion = Body3DMotion::Static});
    bodies.step(dt);
    CHECK(bodies.body_count() == 1 && physics.body_count() == 1 && warnings.count("no valid body") == 1);
    // Refused again as dynamic: one more warning, however many steps follow.
    wall.set(RigidBody3D{});
    for (int i = 0; i < 5; ++i) {
        bodies.step(dt);
    }
    CHECK(bodies.body_count() == 0 && warnings.count("no valid body") == 2);
    // Naming the entity asks again, and so does rebuild.
    const std::array<kin::EcsId, 1> named{wall.id()};
    bodies.sync_entities(named);
    CHECK(warnings.count("no valid body") == 3);
    bodies.rebuild(wall);
    bodies.sync_bodies();
    bodies.sync_bodies();
    CHECK(warnings.count("no valid body") == 4);
    // Losing the collider and getting it back is a new attempt.
    wall.remove<Collider3D>();
    bodies.sync_bodies();
    wall.set(triangles);
    bodies.sync_bodies();
    bodies.sync_bodies();
    CHECK(warnings.count("no valid body") == 5);

    // A character shorter than its width: once, then made when the descriptor is mended.
    CharacterDesc3D squat;
    squat.radius = 0.5f;
    squat.height = 0.6f;
    flecs::entity dwarf = add_character(world, {20, 1, 0}, squat);
    for (int i = 0; i < 10; ++i) {
        bodies.step(dt);
    }
    CHECK(bodies.character_count() == 0 && warnings.count("no valid character") == 1);
    CharacterController3D mended = *dwarf.get<CharacterController3D>();
    mended.desc.height = 1.8f;
    dwarf.set(mended);
    bodies.step(dt);
    CHECK(bodies.character_count() == 1 && warnings.count("no valid character") == 1);

    // A rope with no thickness likewise.
    Rope3D thin;
    thin.length = 2.0f;
    thin.radius = 0.0f;
    flecs::entity rope = world.entity().set(Transform3D{.position = {30, 5, 0}}).set(thin);
    for (int i = 0; i < 10; ++i) {
        bodies.step(dt);
    }
    CHECK(!bodies.rope_of(rope).valid() && warnings.count("no valid rope") == 1);
    thin.radius = 0.04f;
    rope.set(thin);
    bodies.step(dt);
    CHECK(bodies.rope_of(rope).valid() && warnings.count("no valid rope") == 1);

    // A world with no room refuses for that reason, not for the components: it is asked again when there is room.
    World3DConfig tiny = serial();
    tiny.max_bodies = 1;
    World3D small(tiny);
    kin::EcsWorld ecs2;
    WorldPhysicsState3D crowd{ecs2, small};
    const flecs::entity first = add_crate(ecs2.raw(), {0, 1, 0});
    const flecs::entity second = add_crate(ecs2.raw(), {5, 1, 0});
    for (int i = 0; i < 3; ++i) {
        crowd.step(dt);
    }
    CHECK(crowd.body_count() == 1 && crowd.body_of(first).valid() && !crowd.body_of(second).valid());
    first.destruct();
    crowd.step(dt);
    CHECK(crowd.body_count() == 1 && crowd.body_of(second).valid());
}

void test_rotated_collider_under_skewed_scale() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    Warnings warnings;

    // A beam 2 m long along X, turned to Z, on an entity stretched along Z: the scale would have to be applied after the turn
    // (6 m), which a shape scaled in its own axes and then turned cannot be. It is refused (as the asset cooker refuses it).
    Collider3D beam = box_collider({1.0f, 0.1f, 0.1f});
    beam.rotation = Quatf::axis_angle({0, 1, 0}, 90.0f);
    world.entity().set(Transform3D{.position = {0, 10, 0}, .scale = {1, 1, 3}}).set(beam);
    Collider3D compound;
    compound.kind = Collider3D::Kind::Compound;
    compound.children = {beam};
    world.entity().set(Transform3D{.position = {0, 20, 0}, .scale = {1, 1, 3}}).set(compound);
    bodies.sync_bodies();
    bodies.sync_bodies();
    CHECK(bodies.body_count() == 0 && physics.body_count() == 0);
    CHECK(warnings.count("uniform parent scale") == 2);

    // Under a scale that is the same on every axis it is made: 2 m times 2 is 4 m long, along Z.
    world.entity().set(Transform3D{.position = {0, 10, 0}, .scale = {2, 2, 2}}).set(beam);
    // Not turned, a stretched box is as it was: 0.1 times 3 along Z.
    world.entity().set(Transform3D{.position = {5, 10, 0}, .scale = {1, 1, 3}}).set(box_collider({1.0f, 0.1f, 0.1f}));
    bodies.sync_bodies();
    CHECK(bodies.body_count() == 2 && physics.body_count() == 2);
    const auto turned = physics.raycast({0, 10, -5}, {0, 10, 5});
    CHECK(turned && near(turned->point.z, -2.0f, 0.05f));
    const auto stretched = physics.raycast({5, 10, -5}, {5, 10, 5});
    CHECK(stretched && near(stretched->point.z, -0.3f, 0.05f));
    CHECK(warnings.count("uniform parent scale") == 2);
}

void test_resync_after_restore() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity crate = add_crate(world, {10, 20, 0});
    const flecs::entity hero = add_character(world, {0, 0.5f, 0});
    for (int i = 0; i < 60; ++i) {
        bodies.step(dt);
    }
    const Body3D body = bodies.body_of(crate);
    const Character3D handle = bodies.character_of(hero);
    const std::vector<u8> saved = physics.save_state();
    const Vec3f at = physics.position(body);
    const Vec3f stood = physics.character(handle).position;
    CHECK(near(crate.get<Transform3D>()->position, at, 1e-6f) && near(hero.get<Transform3D>()->position, stood, 1e-6f));

    // On: the crate falls further and the hero walks away.
    press(hero, {0, 1});
    for (int i = 0; i < 30; ++i) {
        bodies.step(dt);
    }
    CHECK(crate.get<Transform3D>()->position.y < at.y - 0.3f && hero.get<Transform3D>()->position.x > stood.x + 1.0f);

    // Rolled back: the world is where it was, the entities say where it had got to.
    CHECK(physics.restore_state(saved));
    CHECK(near(physics.position(body), at, 1e-6f) && near(physics.character(handle).position, stood, 1e-6f));
    CHECK(crate.get<Transform3D>()->position.y < at.y - 0.3f && hero.get<Transform3D>()->position.x > stood.x + 1.0f);
    bodies.resync_from_world();
    CHECK(near(crate.get<Transform3D>()->position, at, 1e-6f));
    CHECK(near(hero.get<Transform3D>()->position, stood, 1e-6f));
    CHECK(hero.get<CharacterController3D>()->state.ground == CharacterGround3D::OnGround);
    // Interpolation has no history from before the rollback.
    bodies.interpolate(0.0f);
    CHECK(near(crate.get<Transform3D>()->position, at, 1e-6f) && near(hero.get<Transform3D>()->position, stood, 1e-6f));
    bodies.interpolate(0.5f);
    CHECK(near(crate.get<Transform3D>()->position, at, 1e-6f) && near(hero.get<Transform3D>()->position, stood, 1e-6f));
    // And the step goes on from there: the entities are not read as a move by game code.
    bodies.step(dt);
    CHECK(physics.position(body).y < at.y && near(physics.position(body).y, at.y + physics.linear_velocity(body).y * dt, 0.01f));
    CHECK(near(crate.get<Transform3D>()->position, physics.position(body), 1e-6f));
}

void test_resync_of_a_sleeping_body() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    const flecs::entity box = world.entity()
                                  .set(Transform3D{.position = {0, 3, 0}})
                                  .set(box_collider({0.5f, 0.5f, 0.5f}))
                                  .set(RigidBody3D{.gravity_factor = 0.0f});
    bodies.step(dt);
    const Body3D body = bodies.body_of(box);
    physics.deactivate(body);
    CHECK(!physics.is_active(body));
    const std::vector<u8> asleep = physics.save_state();
    const Vec3f rest = physics.position(body);

    // Kicked awake and away, then rolled back to sleep: step() writes only the awake bodies to the entities.
    physics.add_impulse(body, {0, 5000.0f, 0});
    for (int i = 0; i < 20; ++i) {
        bodies.step(dt);
    }
    CHECK(physics.is_active(body) && box.get<Transform3D>()->position.y > rest.y + 0.5f);
    CHECK(physics.restore_state(asleep));
    CHECK(!physics.is_active(body) && near(physics.position(body), rest, 1e-6f));
    CHECK(box.get<Transform3D>()->position.y > rest.y + 0.5f);
    bodies.resync_from_world();
    CHECK(near(box.get<Transform3D>()->position, rest, 1e-6f));
    bodies.step(dt);
    CHECK(!physics.is_active(body) && near(box.get<Transform3D>()->position, rest, 1e-6f));
}

// Which entities have bodies is read again only when it can have changed, so every way it can change is checked after a run of quiet
// steps (the frames that read nothing again): a new entity, a removed component, a destroyed entity, a raw write to a motion (no
// modified()), disabling, a character component, a body the world lost, rebuild(), and a static entity moved or given a parent long
// after its pose was last taken.
void test_bound_set_follows_changes_after_quiet_steps() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    const flecs::entity crate = add_crate(world, {0, 40, 0});
    const flecs::entity wall = world.entity().set(Transform3D{.position = {5, 1, 0}}).set(box_collider({0.5f, 1, 0.5f}));
    const auto quiet = [&](int n) {
        for (int i = 0; i < n; ++i) {
            bodies.step(dt);
        }
    };
    quiet(5);
    CHECK(bodies.body_count() == 3 && physics.body_count() == 3);
    const Body3D crate_body = bodies.body_of(crate);

    // A new entity.
    const flecs::entity late = add_crate(world, {2, 30, 0});
    quiet(1);
    CHECK(bodies.body_of(late).valid() && bodies.body_count() == 4 && bodies.body_of(crate) == crate_body);
    quiet(5);

    // A removed component, and it coming back.
    const Body3D wall_body = bodies.body_of(wall);
    wall.remove<Collider3D>();
    quiet(1);
    CHECK(!physics.contains(wall_body) && !bodies.body_of(wall).valid() && bodies.body_count() == 3);
    wall.set(box_collider({0.5f, 1, 0.5f}));
    quiet(1);
    CHECK(bodies.body_of(wall).valid() && bodies.body_count() == 4);
    quiet(5);

    // A motion changed by a plain write, without modified(): rebuilt at the next step.
    CHECK(physics.motion(bodies.body_of(crate)) == Body3DMotion::Dynamic);
    crate.get_mut<RigidBody3D>()->motion = Body3DMotion::Kinematic;
    quiet(1);
    CHECK(physics.motion(bodies.body_of(crate)) == Body3DMotion::Kinematic && !physics.contains(crate_body));
    quiet(5);

    // Disabled and enabled again.
    late.disable();
    quiet(1);
    CHECK(!bodies.body_of(late).valid() && bodies.body_count() == 3);
    late.enable();
    quiet(1);
    CHECK(bodies.body_of(late).valid() && bodies.body_count() == 4);
    quiet(5);

    // A body the world lost: made again.
    const Body3D lost = bodies.body_of(late);
    CHECK(physics.destroy_body(lost));
    quiet(1);
    CHECK(bodies.body_of(late).valid() && physics.contains(bodies.body_of(late)) && bodies.body_count() == 4);
    quiet(5);

    // rebuild() after a quiet run.
    const Body3D before = bodies.body_of(late);
    bodies.rebuild(late);
    quiet(1);
    CHECK(bodies.body_of(late).valid() && !physics.contains(before));
    quiet(5);

    // A character component on an entity with a body: the capsule is the body now.
    late.set(CharacterController3D{});
    quiet(1);
    CHECK(bodies.character_count() == 1 && bodies.body_count() == 3);
    late.remove<CharacterController3D>();
    quiet(1);
    CHECK(bodies.character_count() == 0 && bodies.body_of(late).valid() && bodies.body_count() == 4);
    quiet(5);

    // A static entity moved by a plain write, long after its pose was last taken; then given a parent.
    CHECK(near(physics.position(bodies.body_of(wall)), {5, 1, 0}, 1e-4f));
    wall.get_mut<Transform3D>()->position = {9, 1, 0};
    quiet(1);
    CHECK(near(physics.position(bodies.body_of(wall)), {9, 1, 0}, 1e-4f));
    quiet(5);
    const flecs::entity mount = world.entity().set(Transform3D{.position = {0, 0, 10}});
    wall.child_of(mount);
    quiet(1);
    CHECK(near(physics.position(bodies.body_of(wall)), {9, 1, 10}, 1e-4f));
    quiet(5);
    mount.get_mut<Transform3D>()->position = {0, 0, 20};
    quiet(1);
    CHECK(near(physics.position(bodies.body_of(wall)), {9, 1, 20}, 1e-4f));
    wall.remove(flecs::ChildOf, mount);
    quiet(1);
    CHECK(near(physics.position(bodies.body_of(wall)), {9, 1, 0}, 1e-4f));
    wall.get_mut<Transform3D>()->position = {3, 1, 0};
    quiet(1);
    CHECK(near(physics.position(bodies.body_of(wall)), {3, 1, 0}, 1e-4f));
}

void test_inherited_body_settings_follow_source_changes() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    world.component<RigidBody3D>().add(flecs::OnInstantiate, flecs::Inherit);
    const auto prefab = world.prefab().set(RigidBody3D{.motion = Body3DMotion::Kinematic});
    const auto child = world.entity().is_a(prefab).set(Transform3D{.position = {0, 5, 0}})
                           .set(box_collider({0.5f, 0.5f, 0.5f}));
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    for (int i = 0; i < 3; ++i)
        bodies.step(dt);
    CHECK(!child.owns<RigidBody3D>() && physics.motion(bodies.body_of(child)) == Body3DMotion::Kinematic);

    // This source isn't part of the body's structural query: its move must not leave a borrowed pointer behind.
    struct Moved {};
    prefab.add<Moved>();
    prefab.get_mut<RigidBody3D>()->motion = Body3DMotion::Dynamic;
    bodies.step(dt);
    CHECK(physics.motion(bodies.body_of(child)) == Body3DMotion::Dynamic);

    prefab.remove<RigidBody3D>();
    for (int i = 0; i < 3; ++i)
        bodies.step(dt);
    CHECK(physics.motion(bodies.body_of(child)) == Body3DMotion::Static);
    prefab.set(RigidBody3D{.motion = Body3DMotion::Kinematic});
    bodies.step(dt);
    CHECK(physics.motion(bodies.body_of(child)) == Body3DMotion::Kinematic);
}

// Events reach the singletons, in the order of events(), in step and in advance, the same bits as the vector game code reads.
void test_event_singletons_match_events() {
    kin::EcsWorld ecs;
    flecs::world& world = ecs.raw();
    World3D physics(serial());
    WorldPhysicsState3D bodies{ecs, physics};
    add_floor(world);
    for (int i = 0; i < 6; ++i) {
        add_crate(world, {static_cast<f32>(i) * 1.5f, 0.6f + 0.1f * static_cast<f32>(i), 0});
    }
    std::size_t seen = 0;
    for (int i = 0; i < 90; ++i) {
        bodies.step(dt);
        const PhysicsContacts3D* singleton = world.get<PhysicsContacts3D>();
        CHECK(singleton != nullptr && singleton->events.size() == bodies.events().size());
        if (singleton != nullptr && singleton->events.size() == bodies.events().size()) {
            for (std::size_t k = 0; k < singleton->events.size(); ++k) {
                CHECK(singleton->events[k].a == bodies.events()[k].a && singleton->events[k].b == bodies.events()[k].b &&
                      singleton->events[k].kind == bodies.events()[k].kind);
            }
        }
        seen += bodies.events().size();
    }
    CHECK(seen > 0);
    // advance(): one step, several, none.
    std::size_t total = 0;
    for (const f32 frame : {dt, 2.5f * dt, 0.3f * dt, 3.0f * dt}) {
        const i32 steps = bodies.advance(frame);
        const PhysicsContacts3D* singleton = world.get<PhysicsContacts3D>();
        if (steps == 0) {
            CHECK(bodies.events().empty());
        } else {
            CHECK(singleton != nullptr && singleton->events.size() == bodies.events().size());
        }
        total += bodies.events().size();
    }
    (void)total;
}

// World3D::shape(body) is the handle the body was made with, or last given by set_shape, and is equal to it.
void test_shape_of_a_body_is_its_handle() {
    World3D physics(serial());
    const Shape3D box = Shape3D::box({1, 2, 3});
    const Shape3D ball = Shape3D::sphere(0.5f);
    const Body3D body = physics.create_body({.shape = box, .position = {0, 5, 0}});
    CHECK(physics.shape(body) == box && physics.shape(body).kind() == Shape3DKind::Box);
    CHECK(physics.set_shape(body, ball));
    CHECK(physics.shape(body) == ball && physics.shape(body).kind() == Shape3DKind::Sphere);
    CharacterDesc3D desc;
    desc.position = {10, 1, 0};
    const Character3D hero = physics.create_character(desc);
    // A character's own body has no handle of the game's: Jolt's shape, wrapped.
    CHECK(physics.shape(physics.character(hero).body).valid());
    physics.destroy_body(body);
    CHECK(!physics.shape(body).valid());
    // The slot is used again: it has the new body's shape, not the old one's.
    const Body3D again = physics.create_body({.shape = box, .position = {0, 5, 0}});
    CHECK(physics.shape(again) == box && !physics.shape(body).valid());
}

} // namespace

int main() {
    test_disabled_contacts_readable();
    test_bodies_in_entity_order();
    test_scale_offset_and_compound();
    test_dynamic_writes_back();
    test_read_only_render_snapshots();
    test_parented_bodies();
    test_kinematic_follows_and_pushes();
    test_game_moves_entities();
    test_events_routed();
    test_scene_determinism();
    test_interpolation();
    test_teleported_entity_interpolates_from_there();
    test_debug_lines();
    test_debug_mesh_and_height_field_caps();
    test_foot_placement_over_physics();
    test_colliders_from_meshes();
    test_character_entity();
    test_character_carried_by_kinematic_platform();
    test_character_parented_and_interpolated();
    test_character_animation_link();
    test_character_feet_on_physics_ground();
    test_character_scene_determinism();
    test_traversal_entities();
    test_traversal_entities_events();
    test_traversal_hands_ik();
    test_refused_bodies_are_tried_once();
    test_rotated_collider_under_skewed_scale();
    test_resync_after_restore();
    test_resync_of_a_sleeping_body();
    test_bound_set_follows_changes_after_quiet_steps();
    test_inherited_body_settings_follow_source_changes();
    test_event_singletons_match_events();
    test_shape_of_a_body_is_its_handle();
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_ecs_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_ecs_tests: all passed\n");
    return 0;
}
