#include <kin/particles/particle_field.hpp>

#include <algorithm>

namespace kin {
namespace {

RngKey split_key(RngKey& key) {
    auto [next, sub] = split(key);
    key = next;
    return sub;
}

f32 random_range(RngKey& key, ParticleRange range) {
    return rng_f32(split_key(key), range.min, range.max);
}

u8 brightness_to_u8(f32 value) {
    return static_cast<u8>(std::clamp(value, 0.0f, 255.0f));
}

} // namespace

Particle to_particle(const ParticleField& definition, const ParticleFieldParticle& field_particle) {
    return {
        .position = field_particle.position,
        .lifetime = 1.0f,
        .start_size = field_particle.size,
        .end_size = field_particle.size,
        .start_color = field_particle.color,
        .end_color = field_particle.color,
        .render = definition.render,
    };
}

ParticleFieldState init_particle_field(const ParticleField& field) {
    ParticleFieldState state;
    reset_particle_field(state, field);
    return state;
}

void reset_particle_field(ParticleFieldState& state, const ParticleField& field) {
    state.definition = field;
    state.particles.clear();
    state.particles.reserve(static_cast<std::size_t>(std::max(0, field.count)));

    RngKey key = make_key(field.seed);
    for (i32 i = 0; i < field.count; ++i) {
        const f32 brightness = random_range(key, field.brightness);
        const Color color = field.use_brightness
            ? Color{brightness_to_u8(brightness), brightness_to_u8(brightness), brightness_to_u8(brightness), 255}
            : field.color;
        state.particles.push_back({
            .position = {
                rng_f32(split_key(key), field.area.x, field.area.x + field.area.w),
                rng_f32(split_key(key), field.area.y, field.area.y + field.area.h),
            },
            .velocity = {
                random_range(key, field.velocity_x),
                random_range(key, field.velocity_y),
            },
            .size = random_range(key, field.size),
            .color = color,
        });
    }
}

void update_particle_field(ParticleFieldState& state, f32 dt) {
    const Rectf area = state.definition.area;
    for (ParticleFieldParticle& particle : state.particles) {
        particle.position.x += particle.velocity.x * dt;
        particle.position.y += particle.velocity.y * dt;
        if (!state.definition.wrap || dt <= 0.0f || area.w <= 0.0f || area.h <= 0.0f) {
            continue;
        }
        while (particle.position.x < area.x) {
            particle.position.x += area.w;
        }
        while (particle.position.x > area.x + area.w) {
            particle.position.x -= area.w;
        }
        while (particle.position.y < area.y) {
            particle.position.y += area.h;
        }
        while (particle.position.y > area.y + area.h) {
            particle.position.y -= area.h;
        }
    }
}

void render_particle_field(Renderer2D& renderer,
                           const ParticleFieldState& state,
                           const SpriteCatalog* sprites) {
    std::vector<Particle> particles;
    particles.reserve(state.particles.size());
    for (const ParticleFieldParticle& particle : state.particles) {
        particles.push_back(to_particle(state.definition, particle));
    }
    render_particles(renderer,
                     particles,
                     {
                         .pixel_policy = state.pixel_policy,
                         .pixel_grid = state.pixel_grid,
                         .sprites = sprites,
                     });
}

} // namespace kin
