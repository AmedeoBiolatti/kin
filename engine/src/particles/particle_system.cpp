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
                     bool output_pixel_rect,
                     const Material2D* material) {
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
            .source = resolved.sprite.source,
            .color = color,
            .texture = resolved.sprite.texture,
            .rotation = particle.rotation,
            .material = material,
        });
    } else {
        queue.submit({
            .type = RenderCommandType::FillRect,
            .key = key,
            .rect = rect,
            .output_pixel_rect = output_pixel_rect,
            .color = color,
            .rotation = particle.rotation,
            .material = material,
        });
    }
}

constexpr u8 deepest_sub_effect = 3;
constexpr i32 most_streamed_an_update = 1024;

f32 life_part(const Particle& particle) {
    return particle.lifetime > 0.0f ? std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f) : 1.0f;
}

// `particle` `dt` seconds on.
void advance(Particle& particle, f32 dt) {
    particle.age += dt;
    particle.velocity.x += particle.acceleration.x * dt;
    particle.velocity.y += particle.acceleration.y * dt;
    if (particle.drag > 0.0f) {
        const f32 kept = std::exp(-particle.drag * dt);
        particle.velocity.x *= kept;
        particle.velocity.y *= kept;
    }
    particle.rotation += particle.spin * dt;
    const f32 pace = particle.look ? particle_curve_at(particle.look->speed_curve, life_part(particle), 1.0f) : 1.0f;
    particle.position.x += particle.velocity.x * pace * dt;
    particle.position.y += particle.velocity.y * pace * dt;
}

Vec2f turned(Vec2f v, f32 degrees) {
    if (degrees == 0.0f) {
        return v;
    }
    const f32 a = degrees * (tau / 360.0f);
    const f32 c = std::cos(a);
    const f32 s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

} // namespace

std::shared_ptr<const ParticleLook> particle_look(const ParticleBurst& burst) {
    if (burst.size_curve.empty() && burst.color_curve.empty() && burst.speed_curve.empty() &&
        burst.on_death.empty() && burst.trail.empty()) {
        return nullptr;
    }
    return std::make_shared<const ParticleLook>(ParticleLook{
        .size_curve = burst.size_curve,
        .color_curve = burst.color_curve,
        .speed_curve = burst.speed_curve,
        .on_death = burst.on_death,
        .trail = burst.trail,
        .trail_rate = burst.trail_rate,
    });
}

ParticleSystem::ParticleSystem(RngKey seed)
    : _key(seed) {
}

ParticleSystem::ParticleSystem(u64 seed)
    : _key(make_key(seed)) {
}

ParticleSystem::ParticleSystem(const ParticleSystem& other)
    : _particles(other._particles),
      _emitters(other._emitters),
      _next_emitter(other._next_emitter),
      _limit(other._limit),
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
    _emitters = other._emitters;
    _next_emitter = other._next_emitter;
    _limit = other._limit;
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
    _emitters.clear();
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

Particle ParticleSystem::make(const ParticleBurst& burst, Vec2f at, f32 angle) {
    constexpr f32 radians = tau / 360.0f;
    const f32 spread = std::clamp(burst.spread, 0.0f, 360.0f) * radians;
    // All round: any angle (as kin always did); else within the spread.
    const f32 heading = spread >= tau ? rng_f32(split_key(), 0.0f, tau)
                                      : (burst.direction + angle) * radians + rng_f32(split_key(), -0.5f, 0.5f) * spread;
    const f32 speed = random_range(burst.speed);
    Vec2f off{};
    if (burst.shape == ParticleShape::Circle) {
        // Even over the ellipse: the square root of an even fraction out.
        const f32 out = std::sqrt(rng_f32(split_key(), 0.0f, 1.0f));
        const f32 around = rng_f32(split_key(), 0.0f, tau);
        off = {std::cos(around) * out * burst.shape_size.x * 0.5f, std::sin(around) * out * burst.shape_size.y * 0.5f};
    } else if (burst.shape == ParticleShape::Box) {
        off.x = rng_f32(split_key(), -0.5f, 0.5f) * burst.shape_size.x;
        off.y = rng_f32(split_key(), -0.5f, 0.5f) * burst.shape_size.y;
    }
    off = turned(off, angle);
    return Particle{
        .position = {at.x + off.x, at.y + off.y},
        .velocity = {std::cos(heading) * speed, std::sin(heading) * speed},
        .acceleration = burst.acceleration,
        .lifetime = std::max(0.001f, random_range(burst.lifetime)),
        .start_size = burst.start_size,
        .end_size = burst.end_size,
        .start_color = burst.start_color,
        .end_color = burst.end_color,
        .render = burst.render,
        .drag = std::max(0.0f, burst.drag),
        .rotation = random_range(burst.rotation),
        .spin = random_range(burst.spin),
        .use_mid = burst.use_mid,
        .mid_at = std::clamp(burst.mid_at, 0.001f, 0.999f),
        .mid_size = burst.mid_size,
        .mid_color = burst.mid_color,
        .until_trail = burst.trail_rate > 0.0f ? 1.0f / burst.trail_rate : 0.0f,
    };
}

void ParticleSystem::burst(const ParticleBurst& burst) {
    const i32 count = std::max(0, burst.count);
    const std::shared_ptr<const ParticleLook> look = particle_look(burst);
    for (i32 i = 0; i < count; ++i) {
        Particle particle = make(burst, burst.position, 0.0f);
        particle.look = look;
        emit(particle);
    }
}

void ParticleSystem::sub_burst(std::string_view effect_name, Vec2f position, u8 depth) {
    const ParticleCatalog* active = catalog();
    const ParticleEffect* effect = active ? active->effect(effect_name) : nullptr;
    if (!effect || depth > deepest_sub_effect || _particles.size() >= _limit) {
        return; // a name no effect has is left be: it would be said each time a particle ends
    }
    const std::size_t room = _limit - _particles.size();
    const i32 count = static_cast<i32>(std::min<std::size_t>(static_cast<std::size_t>(std::max(0, effect->burst.count)), room));
    const std::shared_ptr<const ParticleLook> look = particle_look(effect->burst);
    for (i32 i = 0; i < count; ++i) {
        Particle particle = make(effect->burst, position, 0.0f);
        particle.look = look;
        particle.depth = depth;
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

ParticleEmitter ParticleSystem::start(std::string_view effect_name, Vec2f position, f32 angle) {
    const ParticleCatalog* active = catalog();
    const ParticleEffect* effect = active ? active->effect(effect_name) : nullptr;
    if (!effect) {
        KIN_LOG_WARN_F("particle",
                       "particle effect missing",
                       (LogFields{{.name = "effect", .value = std::string{effect_name}},
                                  {.name = "reason", .value = active ? "missing_effect" : "missing_catalog"}}));
        return {};
    }
    if (!effect->streams()) {
        const std::shared_ptr<const ParticleLook> look = particle_look(effect->burst);
        for (i32 i = 0; i < effect->burst.count; ++i) {
            Particle particle = make(effect->burst, position, angle);
            particle.look = look;
            emit(particle);
        }
        return {};
    }
    if (_next_emitter == 0) {
        _next_emitter = 1;
    }
    _emitters.push_back({
        .id = _next_emitter++,
        .effect = std::string{effect_name},
        .position = position,
        .was = position,
        .shifted = position,
        .angle = angle,
    });
    Emitter& emitter = _emitters.back();
    // Prewarmed: as if it had been streaming already (its own particles only,
    // and no sub-effects of theirs).
    const std::size_t first = _particles.size();
    constexpr f32 step = 1.0f / 30.0f;
    for (f32 done = 0.0f; done < effect->emission.prewarm;) {
        const f32 dt = std::min(step, effect->emission.prewarm - done);
        for (std::size_t i = first; i < _particles.size(); ++i) {
            advance(_particles[i], dt);
        }
        const auto dead = std::remove_if(_particles.begin() + static_cast<std::ptrdiff_t>(first), _particles.end(),
                                         [](const Particle& p) { return p.age >= p.lifetime; });
        _particles.erase(dead, _particles.end());
        stream(emitter, *effect, dt);
        done += dt;
    }
    KIN_LOG_DEBUG_F("particle",
                    "particle emitter started",
                    (LogFields{{.name = "effect", .value = std::string{effect_name}},
                               {.name = "id", .value = std::to_string(emitter.id)}}));
    return {emitter.id};
}

ParticleSystem::Emitter* ParticleSystem::find(ParticleEmitter emitter) {
    for (Emitter& e : _emitters) {
        if (e.id == emitter.id) {
            return &e;
        }
    }
    return nullptr;
}

const ParticleSystem::Emitter* ParticleSystem::find(ParticleEmitter emitter) const {
    return const_cast<ParticleSystem*>(this)->find(emitter);
}

bool ParticleSystem::move(ParticleEmitter emitter, Vec2f position) {
    Emitter* e = find(emitter);
    if (e) {
        e->position = position;
    }
    return e != nullptr;
}

bool ParticleSystem::move(ParticleEmitter emitter, Vec2f position, f32 angle) {
    Emitter* e = find(emitter);
    if (e) {
        e->position = position;
        e->angle = angle;
    }
    return e != nullptr;
}

bool ParticleSystem::jump(ParticleEmitter emitter, Vec2f position) {
    Emitter* e = find(emitter);
    if (e) {
        e->position = e->was = e->shifted = position;
    }
    return e != nullptr;
}

void ParticleSystem::stop(ParticleEmitter emitter) {
    if (Emitter* e = find(emitter)) {
        e->on = false;
    }
}

bool ParticleSystem::emitting(ParticleEmitter emitter) const {
    const Emitter* e = find(emitter);
    return e && e->on;
}

// The particles `emitter` makes over `dt` as it went from where it was to
// where it is: each where it was made on the way, and as old as made that long ago.
void ParticleSystem::stream(Emitter& emitter, const ParticleEffect& effect, f32 dt) {
    const Vec2f from = emitter.was;
    const Vec2f to = emitter.position;
    emitter.was = to;
    if (!emitter.on) {
        return;
    }
    const ParticleEmission& emission = effect.emission;
    const f32 moved = std::hypot(to.x - from.x, to.y - from.y);
    emitter.owed += emission.rate * dt + emission.per_distance * moved;
    const i32 due = static_cast<i32>(std::floor(emitter.owed));
    emitter.owed -= static_cast<f32>(due);
    const std::size_t room = _particles.size() < _limit ? _limit - _particles.size() : 0;
    const i32 count = static_cast<i32>(std::min<std::size_t>(static_cast<std::size_t>(std::min(due, most_streamed_an_update)), room));
    if (count <= 0) {
        return;
    }
    const Vec2f velocity = dt > 0.0f ? Vec2f{(to.x - from.x) / dt, (to.y - from.y) / dt} : Vec2f{};
    const std::shared_ptr<const ParticleLook> look = particle_look(effect.burst);
    for (i32 i = 0; i < count; ++i) {
        const f32 along = static_cast<f32>(i + 1) / static_cast<f32>(count);
        Particle particle = make(effect.burst, {from.x + (to.x - from.x) * along, from.y + (to.y - from.y) * along}, emitter.angle);
        particle.velocity.x += velocity.x * emission.inherit;
        particle.velocity.y += velocity.y * emission.inherit;
        particle.look = look;
        particle.emitter = emitter.id;
        advance(particle, (1.0f - along) * dt);
        if (particle.age < particle.lifetime) {
            emit(particle);
        }
    }
}

void ParticleSystem::update(f32 dt) {
    const ParticleCatalog* active = catalog();
    const auto effect_of = [active](const Emitter& e) { return active ? active->effect(e.effect) : nullptr; };

    // Local emitters' particles go where they went.
    for (Emitter& e : _emitters) {
        const ParticleEffect* effect = effect_of(e);
        const Vec2f by{e.position.x - e.shifted.x, e.position.y - e.shifted.y};
        e.shifted = e.position;
        if (!effect || !effect->emission.local || (by.x == 0.0f && by.y == 0.0f)) {
            continue;
        }
        for (Particle& particle : _particles) {
            if (particle.emitter == e.id) {
                particle.position.x += by.x;
                particle.position.y += by.y;
            }
        }
    }

    // Sub-effects, made once the particles have moved (and the dead are gone).
    struct Sub {
        std::shared_ptr<const ParticleLook> look;
        bool death = false;
        Vec2f at{};
        u8 depth = 0;
    };
    std::vector<Sub> subs;
    for (Particle& particle : _particles) {
        advance(particle, dt);
        if (!particle.look || particle.depth >= deepest_sub_effect) {
            continue;
        }
        const ParticleLook& look = *particle.look;
        if (!look.trail.empty() && look.trail_rate > 0.0f) {
            particle.until_trail -= dt;
            for (i32 made = 0; particle.until_trail <= 0.0f && made < 4; ++made) {
                subs.push_back({particle.look, false, particle.position, particle.depth});
                particle.until_trail += 1.0f / look.trail_rate;
            }
            particle.until_trail = std::max(particle.until_trail, 0.0f);
        }
        if (!look.on_death.empty() && particle.age >= particle.lifetime) {
            subs.push_back({particle.look, true, particle.position, particle.depth});
        }
    }
    std::erase_if(_particles, [](const Particle& particle) {
        return particle.age >= particle.lifetime;
    });
    for (const Sub& sub : subs) {
        sub_burst(sub.death ? sub.look->on_death : sub.look->trail, sub.at, static_cast<u8>(sub.depth + 1));
    }

    for (Emitter& e : _emitters) {
        const ParticleEffect* effect = effect_of(e);
        if (!effect) {
            e.was = e.position; // its catalog not bound yet, or without it now
            continue;
        }
        e.age += dt;
        stream(e, *effect, dt);
        if (effect->emission.duration > 0.0f && e.age >= effect->emission.duration) {
            e.on = false;
        }
    }
    // A stopped emitter goes, once no particle of its moves with it.
    std::erase_if(_emitters, [&](const Emitter& e) {
        if (e.on) {
            return false;
        }
        const ParticleEffect* effect = effect_of(e);
        if (!effect || !effect->emission.local) {
            return true;
        }
        return std::ranges::none_of(_particles, [&](const Particle& p) { return p.emitter == e.id; });
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
    const f32 t = life_part(particle);
    if (particle.look && !particle.look->color_curve.empty()) {
        return particle_gradient_at(particle.look->color_curve, t, particle.start_color);
    }
    if (particle.use_mid) {
        return t < particle.mid_at ? lerp_color(particle.start_color, particle.mid_color, t / particle.mid_at)
                                   : lerp_color(particle.mid_color, particle.end_color, (t - particle.mid_at) / (1.0f - particle.mid_at));
    }
    return lerp_color(particle.start_color, particle.end_color, t);
}

f32 particle_size_at(const Particle& particle) {
    const f32 t = life_part(particle);
    if (particle.look && !particle.look->size_curve.empty()) {
        return particle_curve_at(particle.look->size_curve, t, particle.start_size);
    }
    if (particle.use_mid) {
        return t < particle.mid_at
                   ? particle.start_size + (particle.mid_size - particle.start_size) * (t / particle.mid_at)
                   : particle.mid_size + (particle.end_size - particle.mid_size) * ((t - particle.mid_at) / (1.0f - particle.mid_at));
    }
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
                        options.pixel_policy == ParticlePixelPolicy::Output,
                        blend_material(options.blend));
        submitted = true;
    }
    return submitted;
}

} // namespace kin
