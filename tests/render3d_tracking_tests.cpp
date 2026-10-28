#include <cassert>
#include <iostream>
#include <kin/ecs/render3d.hpp>
#include <kin/scene/scene_frame3d.hpp>
using namespace kin;

int main() {
    struct Fake final : IMeshBackend3D {};
    const Mesh3D mesh{std::make_shared<Fake>(), 8, 36, {{-1, -1, -1}, {1, 1, 1}}};
    kin::EcsWorld world;
    WorldRenderState3D state{world};
    state.mutation_mode(SceneMutationMode3D::Tracked);
    std::vector<flecs::entity> entities;
    for (u32 i = 0; i < 10000; ++i)
        entities.push_back(
            world.raw().entity().set(Transform3D{.position = {f32(i), 0, 0}}).set(MeshRenderer3D{.mesh = mesh}));
    Scene3D scene;
    state.propagate_transforms();
    state.collect(scene);
    state.reset_work_stats();
    for (int i = 0; i < 4; ++i)
        state.propagate_transforms();
    scene.clear();
    state.collect(scene);
    assert(state.work_stats().transform_visits == 0);
    assert(state.work_stats().collection_visits == 0);
    assert(state.work_stats().collection_reuses == 1);
    Scene3D old;
    state.collect(old);
    const auto old_records = old.draws()[0].batch.instances();
    for (u32 i = 0; i < 100; ++i)
        entities[i].set(Transform3D{.position = {f32(i), 2, 0}});
    state.propagate_transforms();
    assert(state.work_stats().transform_visits == 100);
    assert(state.work_stats().transforms_recomputed == 100);
    scene.clear();
    state.collect(scene);
    assert(state.work_stats().collection_visits == 0 && state.work_stats().collection_updates == 100);
    assert(old_records[0].transform.m[13] == 0);
    assert(scene.draws()[0].batch.instances()[0].transform.m[13] == 2);
    old.clear();
    // Hierarchy changes rebuild topology; an ancestor edit reaches every descendant once.
    auto parent = world.raw().entity().set(Transform3D{.position = {0, 3, 0}});
    entities[0].child_of(parent);
    auto child = world.raw().entity().child_of(entities[0]).set(Transform3D{.position = {0, 4, 0}});
    state.propagate_transforms();
    assert(child.get<WorldTransform3D>()->position().y == 9);
    state.reset_work_stats();
    parent.set(Transform3D{.position = {0, 5, 0}});
    state.propagate_transforms();
    state.propagate_transforms();
    assert(child.get<WorldTransform3D>()->position().y == 11);
    assert(state.work_stats().transform_visits == 3);
    assert(state.work_stats().topology_rebuilds == 0);
    entities[0].remove(flecs::ChildOf, flecs::Wildcard);
    state.propagate_transforms();
    assert(child.get<WorldTransform3D>()->position().y == 6);
    // A raw edit remains supported by the default compatibility path, including derived tampering.
    state.mutation_mode(SceneMutationMode3D::Conservative);
    entities[0].get_mut<Transform3D>()->position.y = 7;
    state.propagate_transforms();
    assert(child.get<WorldTransform3D>()->position().y == 11);
    entities[0].get_mut<WorldTransform3D>()->matrix = Mat4f::translation({0, 500, 0});
    state.propagate_transforms();
    assert(child.get<WorldTransform3D>()->position().y == 11);
    // Independent states have independent queues and callback lifetimes.
    {
        WorldRenderState3D second{world};
        second.mutation_mode(SceneMutationMode3D::Tracked);
        second.propagate_transforms();
    }
    entities[1].set(Transform3D{.position = {0, 20, 0}});
    state.propagate_transforms();
    assert(entities[1].get<WorldTransform3D>()->position().y == 20);
    kin::EcsWorld inherited;
    inherited.raw().component<Transform3D>().add(flecs::OnInstantiate, flecs::Inherit);
    WorldRenderState3D inherited_state{inherited};
    inherited_state.mutation_mode(SceneMutationMode3D::Tracked);
    auto prefab = inherited.raw().prefab().set(Transform3D{.position = {0, 1, 0}});
    auto instance = inherited.raw().entity().is_a(prefab);
    inherited_state.propagate_transforms();
    assert(instance.get<WorldTransform3D>()->position().y == 1);
    prefab.set(Transform3D{.position = {0, 6, 0}});
    inherited_state.propagate_transforms();
    assert(instance.get<WorldTransform3D>()->position().y == 6);
    instance.disable();
    inherited_state.propagate_transforms();
    instance.enable();
    inherited_state.propagate_transforms();
    assert(instance.get<WorldTransform3D>()->position().y == 6);
    // Validation reuses both success and failure, including disabled retained emitters.
    kin::EcsWorld particles;
    WorldRenderState3D particle_state{particles};
    particle_state.mutation_mode(SceneMutationMode3D::Tracked);
    auto emitter = particles.raw().entity().set(Transform3D{});
    ParticleEmitter3D config;
    config.system.desc.max_particles = 8;
    emitter.set(config);
    emitter.disable();
    SceneFrame3D frame{
        particles, particle_state, {.particles = {.emitters = 2, .per_emitter = 16, .total_particles = 32}}};
    assert(frame.after_phase(SystemPhase::Editor, 1.f / 60));
    assert(frame.after_phase(SystemPhase::Editor, 1.f / 60));
    assert(particle_state.work_stats().particle_validation_runs == 1);
    assert(particle_state.work_stats().particle_validation_reuses == 1);
    emitter.get_mut<ParticleEmitter3D>()->system.desc.max_particles = 64;
    emitter.modified<ParticleEmitter3D>();
    assert(!frame.after_phase(SystemPhase::Editor, 1.f / 60));
    assert(!frame.after_phase(SystemPhase::Editor, 1.f / 60));
    assert(particle_state.work_stats().particle_validation_runs == 2);
    emitter.remove<ParticleEmitter3D>();
    assert(frame.after_phase(SystemPhase::Editor, 1.f / 60));
    std::cout << "tracked transforms: static zero visits, sparse 1%, descendants, snapshots, reparenting and raw "
                 "compatibility\n";
}
