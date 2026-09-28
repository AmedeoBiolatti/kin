#include <kin/ecs/particles.hpp>

#include <kin/ecs/render.hpp>
#include <kin/particles/animation_particles.hpp>

namespace kin {
namespace {

// Internal flecs-typed implementations; the public EcsWorld& API forwards here.

void update_particles_raw(flecs::world& world, f32 dt) {
    world.each([dt](ParticleSystemComponent& particles) {
        particles.system.update(dt);
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
