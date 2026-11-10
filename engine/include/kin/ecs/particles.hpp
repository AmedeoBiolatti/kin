#pragma once

#include <kin/ecs/component.hpp>
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

    // As scene files and tools give it: a particle catalog of the game's
    // content (.kinparticles), and an effect of it that update_particles
    // bursts where the entity is (`offset` from there, turned with it): once
    // as it starts, and `rate` times a second while `emitting`. With no effect
    // or rate, the game emits (from code). An effect that streams (its own
    // rate, ParticleEmission) streams from there instead while `emitting`,
    // turned with the entity, its burst at start still made. Assets2D binds
    // the catalog to `system`, and `sprites_asset` (a sprite catalog) to
    // `sprites`.
    ComponentAssetRef asset{};
    std::string effect;
    f32 rate = 0.0f;
    bool emitting = true;
    bool burst_at_start = false;
    Vec2f offset{};
    ComponentAssetRef sprites_asset{};
    BlendMode blend = BlendMode::Alpha; // Additive: sparks, fire, glows
    f32 until_next = 0.0f; // seconds to its next burst
    bool started = false;  // its burst at start made
    ParticleEmitter emitter{}; // its effect streaming (an effect that streams)
    bool streaming = false;    // the emitter started while `emitting`
};

struct ParticleFieldComponent {
    // Field particles are simulated in world space; Transform2D is not applied during rendering.
    std::string name;
    ParticleFieldState state;
    const SpriteCatalog* sprites = nullptr;

    // As scene files and tools give it: a particle catalog of the game's
    // content (.kinparticles), `name` one of its fields. Assets2D makes
    // `state` from it, its area from where the entity is, and again when the
    // catalog changes; and binds `sprites_asset` to `sprites`.
    ComponentAssetRef asset{};
    ComponentAssetRef sprites_asset{};
    BlendMode blend = BlendMode::Alpha;
    const ParticleField* bound = nullptr; // the field `state` was made from
};

void update_particles(EcsWorld& world, f32 dt);
// `entity`'s ParticleSystemComponent's effect (or `effect`, another of its
// catalog) burst where it bursts (`offset` from the entity, turned with it);
// false when it has none, or its catalog no such effect.
bool emit_particles(EcsEntity entity, std::string_view effect = {});
bool submit_particles(RenderQueue& queue, const ParticleSystemComponent& particles);
bool submit_particles(RenderQueue& queue, const ParticleFieldComponent& field);
void render_particles(EcsWorld& world, Renderer2D& renderer);
void dispatch_particle_animation_events(EcsWorld& world, std::string_view event_name = "particle");

} // namespace kin
