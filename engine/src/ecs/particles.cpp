#include <kin/ecs/particles.hpp>

#include <kin/core/affine.hpp>
#include <kin/ecs/render.hpp>
#include <kin/particles/animation_particles.hpp>

#include <utility>

namespace kin {
namespace {

// Where an emitter bursts: where its entity is now, `offset` from there
// (turned and scaled with it); and how it is turned.
struct BurstPoint {
    Vec2f at{};
    f32 angle = 0.0f;
};
BurstPoint burst_point(flecs::entity entity, const ParticleSystemComponent& particles) {
    if (!entity.has<Transform2D>()) {
        return {particles.offset, 0.0f};
    }
    const WorldTransform pose = current_world_transform(entity);
    return {Affine2::trs(pose.pos, pose.rotation, pose.scale).apply(particles.offset), pose.rotation};
}

// An effect that streams: its emitter started as the entity starts emitting,
// stopped as it stops, and where the entity is (before the update, which
// streams along the way it went).
void follow(flecs::entity entity, ParticleSystemComponent& particles) {
    const BurstPoint point = burst_point(entity, particles);
    if (particles.emitting && !particles.streaming) {
        particles.emitter = particles.system.start(particles.effect, point.at, point.angle);
        particles.streaming = true;
    } else if (!particles.emitting && particles.streaming) {
        particles.system.stop(particles.emitter);
        particles.emitter = {};
        particles.streaming = false;
    }
    if (particles.emitter) {
        particles.system.move(particles.emitter, point.at, point.angle);
    }
}

// The effect authored, when the catalog bound has it.
const ParticleEffect* authored(const ParticleSystemComponent& particles) {
    const ParticleCatalog* catalog = particles.system.catalog();
    return particles.effect.empty() || !catalog ? nullptr : catalog->effect(particles.effect);
}

// An emitter as authored: its effect burst as it starts, and at its rate.
void emit_authored(flecs::entity entity, ParticleSystemComponent& particles, f32 dt) {
    const ParticleEffect* effect = authored(particles);
    if (!effect) {
        return; // the game's to emit; or its catalog is not bound yet, or has no such effect
    }
    const bool rated = particles.emitting && particles.rate > 0.0f && !effect->streams();
    const f32 every = rated ? 1.0f / particles.rate : 0.0f;
    if (!std::exchange(particles.started, true) && particles.burst_at_start) {
        particles.system.emit(particles.effect, burst_point(entity, particles).at);
        particles.until_next = every; // the next at its rate, from now
    }
    if (!rated) {
        return;
    }
    particles.until_next -= dt;
    // A long update makes a few bursts, not a flood.
    for (i32 bursts = 0; particles.until_next <= 0.0f && bursts < 8; ++bursts) {
        particles.system.emit(particles.effect, burst_point(entity, particles).at);
        particles.until_next += every;
    }
    if (particles.until_next <= 0.0f) {
        particles.until_next = every;
    }
}

// Internal flecs-typed implementations; the public EcsWorld& API forwards here.

void update_particles_raw(flecs::world& world, f32 dt) {
    world.each([dt](flecs::entity entity, ParticleSystemComponent& particles) {
        if (const ParticleEffect* effect = authored(particles); effect && effect->streams()) {
            follow(entity, particles);
        } else if (particles.streaming) { // its effect streams no more (the catalog changed)
            particles.system.stop(particles.emitter);
            particles.emitter = {};
            particles.streaming = false;
        }
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

bool emit_particles(EcsEntity entity, std::string_view effect) {
    if (!entity) {
        return false;
    }
    flecs::entity live = entity.raw();
    ParticleSystemComponent* particles = live.get_mut<ParticleSystemComponent>();
    if (!particles) {
        return false;
    }
    const std::string name{effect.empty() ? std::string_view{particles->effect} : effect};
    const ParticleCatalog* catalog = particles->system.catalog();
    if (name.empty() || !catalog || !catalog->has_effect(name)) {
        return false;
    }
    return particles->system.emit(name, burst_point(live, *particles).at);
}

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
