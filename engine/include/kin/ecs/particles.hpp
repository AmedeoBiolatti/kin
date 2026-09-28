#pragma once

#include <kin/ecs/world.hpp>
#include <kin/particles/particle_field.hpp>
#include <kin/particles/particle_system.hpp>
#include <kin/renderer/render_queue.hpp>

#include <string_view>

namespace kin {

struct ParticleSystemComponent {
    // Particle positions are simulated in world space; Transform2D is not applied during rendering.
    ParticleSystem system;
    const SpriteCatalog* sprites = nullptr;
};

struct ParticleFieldComponent {
    // Field particles are simulated in world space; Transform2D is not applied during rendering.
    std::string name;
    ParticleFieldState state;
    const SpriteCatalog* sprites = nullptr;
};

void update_particles(EcsWorld& world, f32 dt);
bool submit_particles(RenderQueue& queue, const ParticleSystemComponent& particles);
bool submit_particles(RenderQueue& queue, const ParticleFieldComponent& field);
void render_particles(EcsWorld& world, Renderer2D& renderer);
void dispatch_particle_animation_events(EcsWorld& world, std::string_view event_name = "particle");

} // namespace kin
