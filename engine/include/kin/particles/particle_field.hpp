#pragma once

#include <kin/particles/particle_system.hpp>

#include <vector>

namespace kin {

struct ParticleFieldParticle {
    Vec2f position{};
    Vec2f velocity{};
    f32 size = 1.0f;
    Color color{255, 255, 255, 255};
};

struct ParticleFieldState {
    ParticleField definition{};
    std::vector<ParticleFieldParticle> particles;
    ParticlePixelPolicy pixel_policy = ParticlePixelPolicy::Free;
    f32 pixel_grid = 1.0f;
};

ParticleFieldState init_particle_field(const ParticleField& field);
Particle to_particle(const ParticleField& definition, const ParticleFieldParticle& field_particle);
void reset_particle_field(ParticleFieldState& state, const ParticleField& field);
void update_particle_field(ParticleFieldState& state, f32 dt);
void render_particle_field(Renderer2D& renderer,
                           const ParticleFieldState& state,
                           const SpriteCatalog* sprites = nullptr);

} // namespace kin
