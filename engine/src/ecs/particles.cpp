#include <kin/ecs/particles.hpp>

#include <kin/core/affine.hpp>
#include <kin/ecs/render.hpp>
#include <kin/particles/animation_particles.hpp>

#include <utility>

namespace kin {
namespace {

// Where an emitter bursts: where its entity is now, `offset` from there
// (turned and scaled with it).
Vec2f burst_point(flecs::entity entity, const ParticleSystemComponent& particles) {
    if (!entity.has<Transform2D>()) {
        return particles.offset;
    }
    const WorldTransform pose = current_world_transform(entity);
    return Affine2::trs(pose.pos, pose.rotation, pose.scale).apply(particles.offset);
}

// An emitter as authored: its effect burst as it starts, and at its rate.
void emit_authored(flecs::entity entity, ParticleSystemComponent& particles, f32 dt) {
    const ParticleCatalog* catalog = particles.system.catalog();
    if (particles.effect.empty() || !catalog || !catalog->has_effect(particles.effect)) {
        return; // the game's to emit; or its catalog is not bound yet, or has no such effect
    }
    const bool rated = particles.emitting && particles.rate > 0.0f;
    const f32 every = rated ? 1.0f / particles.rate : 0.0f;
    if (!std::exchange(particles.started, true) && particles.burst_at_start) {
        particles.system.emit(particles.effect, burst_point(entity, particles));
        particles.until_next = every; // the next at its rate, from now
    }
    if (!rated) {
        return;
    }
    particles.until_next -= dt;
    // A long update makes a few bursts, not a flood.
    for (i32 bursts = 0; particles.until_next <= 0.0f && bursts < 8; ++bursts) {
        particles.system.emit(particles.effect, burst_point(entity, particles));
        particles.until_next += every;
    }
    if (particles.until_next <= 0.0f) {
        particles.until_next = every;
    }
}

// Internal flecs-typed implementations; the public EcsWorld& API forwards here.

void update_particles_raw(flecs::world& world, f32 dt) {
    world.each([dt](flecs::entity entity, ParticleSystemComponent& particles) {
        particles.system.update(dt);
        emit_authored(entity, particles, dt); // after: a burst shows where it is made
    });
    world.each([dt](ParticleFieldComponent& field) {
        update_particle_field(field.state, dt);
    });
}

void render_particles_raw(flecs::world& world, Renderer2D& renderer) {
    render_world(world, renderer);
}

} // namespace

void update_particles(EcsWorld& world, f32 dt) {
    update_particles_raw(world.raw(), dt);
}

void render_particles(EcsWorld& world, Renderer2D& renderer) {
    render_particles_raw(world.raw(), renderer);
}

void dispatch_particle_animation_events(EcsWorld& world, std::string_view event_name) {
    ParticleSystem* target = nullptr;
    world.each([&](ParticleSystemComponent& particles) {
        if (!target) {
            target = &particles.system;
        }
    });
    if (target) {
        dispatch_particle_animation_events(world, *target, event_name);
    }
}

} // namespace kin
