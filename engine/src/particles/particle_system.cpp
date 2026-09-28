#include <kin/particles/particle_system.hpp>

#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace kin {
namespace {

constexpr f32 tau = 6.28318530717958647692f;

f32 clamp_grid(f32 grid) {
    return grid > 0.0f ? grid : 1.0f;
}

f32 snap_value(f32 value, f32 grid) {
    return std::round(value / grid) * grid;
}

Color lerp_color(Color a, Color b, f32 t) {
    const auto lerp = [t](u8 x, u8 y) {
        return static_cast<u8>(std::clamp(std::round(static_cast<f32>(x) + (static_cast<f32>(y) - static_cast<f32>(x)) * t),
                                          0.0f,
                                          255.0f));
    };
    return {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)};
}

u64 pass_mask_for_layer(i32 layer) {
    if (layer >= layer_value(RenderLayer::Debug)) {
        return render_pass_mask::debug;
    }
    if (layer >= layer_value(RenderLayer::UI)) {
        return render_pass_mask::ui;
    }
    if (layer >= layer_value(RenderLayer::Effects)) {
        return render_pass_mask::effects;
    }
    return render_pass_mask::world;
}

RenderKey key_for(const Particle& particle) {
    return {
        .layer = particle.render.layer,
        .order = particle.render.order,
        .y = particle.position.y + particle.render.sort_y_offset,
        .use_y = particle.render.y_sort,
        .pass_mask = pass_mask_for_layer(particle.render.layer),
    };
}

void submit_particle(RenderQueue& queue,
                     const Particle& particle,
                     Rectf rect,
                     Color color,
                     const SpriteCatalog* sprites,
                     bool output_pixel_rect) {
    const RenderKey key = key_for(particle);
    ResolvedSprite resolved;
    if (sprites &&
        !particle.render.sprite_id.empty() &&
        sprites->resolve(particle.render.sprite_id, resolved)) {
        queue.submit({
            .type = RenderCommandType::Sprite,
            .key = key,
            .rect = rect,
            .output_pixel_rect = output_pixel_rect,
            .color = color,
            .sprite = resolved.sprite,
        });
    } else {
        queue.submit({
            .type = RenderCommandType::FillRect,
            .key = key,
            .rect = rect,
            .output_pixel_rect = output_pixel_rect,
            .color = color,
        });
    }
}

} // namespace

ParticleSystem::ParticleSystem(RngKey seed)
    : _key(seed) {
}

ParticleSystem::ParticleSystem(u64 seed)
    : _key(make_key(seed)) {
}

ParticleSystem::ParticleSystem(const ParticleSystem& other)
    : _particles(other._particles),
      _key(other._key),
      _catalog_shared(other._catalog_shared),
      _catalog_handle(other._catalog_handle),
      _pixel_policy(other._pixel_policy),
      _pixel_grid(other._pixel_grid) {
    if (other._catalog_copy) {
        _catalog_copy = std::make_unique<ParticleCatalog>(*other._catalog_copy);
    }
}

ParticleSystem& ParticleSystem::operator=(const ParticleSystem& other) {
    if (this == &other) {
        return *this;
    }
    _particles = other._particles;
    _key = other._key;
    _catalog_copy = other._catalog_copy ? std::make_unique<ParticleCatalog>(*other._catalog_copy) : nullptr;
    _catalog_shared = other._catalog_shared;
    _catalog_handle = other._catalog_handle;
    _pixel_policy = other._pixel_policy;
    _pixel_grid = other._pixel_grid;
    return *this;
}

void ParticleSystem::set_seed(RngKey seed) {
    _key = seed;
}

void ParticleSystem::reserve(std::size_t capacity) {
    _particles.reserve(capacity);
    KIN_LOG_DEBUG_F("particle",
                    "particle system reserved",
                    (LogFields{{.name = "capacity", .value = std::to_string(capacity)}}));
}

void ParticleSystem::clear() {
    const std::size_t count = _particles.size();
    _particles.clear();
    KIN_LOG_DEBUG_F("particle",
                    "particle system cleared",
                    (LogFields{{.name = "count", .value = std::to_string(count)}}));
}

void ParticleSystem::set_catalog(ParticleCatalog catalog) {
    _catalog_copy = std::make_unique<ParticleCatalog>(std::move(catalog));
    _catalog_shared.reset();
    _catalog_handle = {};
    KIN_LOG_DEBUG("particle", "particle catalog assigned");
}

void ParticleSystem::set_catalog(std::shared_ptr<const ParticleCatalog> catalog) {
    _catalog_shared = std::move(catalog);
    _catalog_copy.reset();
    _catalog_handle = {};
    KIN_LOG_DEBUG_F("particle",
                    "particle shared catalog assigned",
                    (LogFields{{.name = "status", .value = _catalog_shared ? "assigned" : "empty"}}));
}

void ParticleSystem::set_catalog(AssetHandle<ParticleCatalog> catalog) {
    _catalog_handle = catalog;
    _catalog_copy.reset();
    _catalog_shared.reset();
    KIN_LOG_DEBUG_F("particle",
                    "particle asset catalog assigned",
                    (LogFields{{.name = "status", .value = _catalog_handle ? "assigned" : "empty"}}));
}

void ParticleSystem::clear_catalog() {
    _catalog_copy.reset();
    _catalog_shared.reset();
    _catalog_handle = {};
    KIN_LOG_DEBUG("particle", "particle catalog cleared");
}

void ParticleSystem::set_pixel_grid(f32 grid) {
    _pixel_grid = clamp_grid(grid);
}

void ParticleSystem::emit(const Particle& particle) {
    if (particle.lifetime > 0.0f) {
        _particles.push_back(particle);
    }
}

RngKey ParticleSystem::split_key() {
    auto [next, sub] = split(_key);
    _key = next;
    return sub;
}

f32 ParticleSystem::random_range(ParticleRange range) {
    return rng_f32(split_key(), range.min, range.max);
}

const ParticleCatalog* ParticleSystem::catalog() const {
    if (_catalog_handle) {
        return &*_catalog_handle;
    }
    if (_catalog_shared) {
        return _catalog_shared.get();
    }
    return _catalog_copy.get();
}

void ParticleSystem::burst(const ParticleBurst& burst) {
    const i32 count = std::max(0, burst.count);
    for (i32 i = 0; i < count; ++i) {
        const f32 angle = rng_f32(split_key(), 0.0f, tau);
        const f32 speed = random_range(burst.speed);
        Particle particle{
            .position = burst.position,
            .velocity = {std::cos(angle) * speed, std::sin(angle) * speed},
            .acceleration = burst.acceleration,
            .lifetime = std::max(0.001f, random_range(burst.lifetime)),
            .start_size = burst.start_size,
            .end_size = burst.end_size,
            .start_color = burst.start_color,
            .end_color = burst.end_color,
            .render = burst.render,
        };
        emit(particle);
    }
}

bool ParticleSystem::emit(std::string_view effect_name, Vec2f position) {
    return emit(effect_name, position, {});
}

bool ParticleSystem::emit(std::string_view effect_name, Vec2f position, Vec2f offset) {
    const ParticleCatalog* active = catalog();
    const ParticleEffect* effect = active ? active->effect(effect_name) : nullptr;
    if (!effect) {
        KIN_LOG_WARN_F("particle",
                       "particle effect missing",
                       (LogFields{{.name = "effect", .value = std::string{effect_name}},
                                  {.name = "reason", .value = active ? "missing_effect" : "missing_catalog"}}));
        return false;
    }
    ParticleBurst copy = effect->burst;
    copy.position = {position.x + offset.x, position.y + offset.y};
    burst(copy);
    KIN_LOG_DEBUG_F("particle",
                    "particle effect emitted",
                    (LogFields{{.name = "effect", .value = std::string{effect_name}},
                               {.name = "count", .value = std::to_string(std::max(0, copy.count))}}));
    return true;
}

void ParticleSystem::update(f32 dt) {
    for (Particle& particle : _particles) {
        particle.age += dt;
        particle.velocity.x += particle.acceleration.x * dt;
        particle.velocity.y += particle.acceleration.y * dt;
        particle.position.x += particle.velocity.x * dt;
        particle.position.y += particle.velocity.y * dt;
    }
    std::erase_if(_particles, [](const Particle& particle) {
        return particle.age >= particle.lifetime;
    });
}

void ParticleSystem::render(Renderer2D& renderer, const SpriteCatalog* sprites) const {
    render(renderer, {.pixel_policy = _pixel_policy, .pixel_grid = _pixel_grid, .sprites = sprites});
}

void ParticleSystem::render(Renderer2D& renderer, ParticleRenderOptions options) const {
    options.pixel_grid = clamp_grid(options.pixel_grid);
    render_particles(renderer, _particles, options);
}

Color particle_color_at(const Particle& particle) {
    const f32 t = particle.lifetime > 0.0f ? std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f) : 1.0f;
    return lerp_color(particle.start_color, particle.end_color, t);
}

f32 particle_size_at(const Particle& particle) {
    const f32 t = particle.lifetime > 0.0f ? std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f) : 1.0f;
    return particle.start_size + (particle.end_size - particle.start_size) * t;
}

Rectf particle_rect(const Particle& particle, ParticlePixelPolicy policy, f32 pixel_grid) {
    const f32 size = std::max(0.0f, particle_size_at(particle));
    Rectf rect{particle.position.x - size * 0.5f, particle.position.y - size * 0.5f, size, size};
    if (policy == ParticlePixelPolicy::LogicalPixel) {
        pixel_grid = 1.0f;
    }
    if (policy == ParticlePixelPolicy::LogicalPixel || policy == ParticlePixelPolicy::Subpixel) {
        const f32 grid = clamp_grid(pixel_grid);
        rect.x = snap_value(rect.x, grid);
        rect.y = snap_value(rect.y, grid);
        rect.w = snap_value(rect.w, grid);
        rect.h = snap_value(rect.h, grid);
    }
    return rect;
}

void render_particles(Renderer2D& renderer,
                      const std::vector<Particle>& particles,
                      ParticleRenderOptions options) {
    options.pixel_grid = clamp_grid(options.pixel_grid);
    thread_local RenderQueue queue{RenderSortMode::LayerThenY};
    queue.clear();
    submit_particles(queue, particles, options);
    queue.flush(renderer);
}

bool submit_particles(RenderQueue& queue,
                      const std::vector<Particle>& particles,
                      ParticleRenderOptions options) {
    options.pixel_grid = clamp_grid(options.pixel_grid);
    queue.set_sort(options.sort ? RenderSortMode::LayerThenY : RenderSortMode::Submission);

    bool submitted = false;
    for (const Particle& particle : particles) {
        Rectf rect = particle_rect(particle, options.pixel_policy, options.pixel_grid);
        if (rect.w <= 0.0f || rect.h <= 0.0f) {
            continue;
        }
        submit_particle(queue,
                        particle,
                        rect,
                        particle_color_at(particle),
                        options.sprites,
                        options.pixel_policy == ParticlePixelPolicy::Output);
        submitted = true;
    }
    return submitted;
}

} // namespace kin
