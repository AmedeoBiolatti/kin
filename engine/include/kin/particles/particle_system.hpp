#pragma once

#include <kin/assets/asset_handle.hpp>
#include <kin/core/rng.hpp>
#include <kin/particles/particle_catalog.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <memory>
#include <string>
#include <vector>

namespace kin {

// What a burst's particles share past their start: their curves over life and
// their sub-effects (ParticleBurst's, which see).
struct ParticleLook {
    std::vector<ParticleKey> size_curve;
    std::vector<ParticleColorKey> color_curve;
    std::vector<ParticleKey> speed_curve;
    std::string on_death;
    std::string trail;
    f32 trail_rate = 10.0f;
};

// The look of `burst`'s particles; null when they need none.
std::shared_ptr<const ParticleLook> particle_look(const ParticleBurst& burst);

struct Particle {
    Vec2f position{};
    Vec2f velocity{};
    Vec2f acceleration{};
    f32 age = 0.0f;
    f32 lifetime = 1.0f;
    f32 start_size = 1.0f;
    f32 end_size = 1.0f;
    Color start_color{255, 255, 255, 255};
    Color end_color{255, 255, 255, 0};
    ParticleRenderStyle render{};
    f32 drag = 0.0f;
    f32 rotation = 0.0f; // degrees
    f32 spin = 0.0f;     // degrees a second
    bool use_mid = false;
    f32 mid_at = 0.5f;
    f32 mid_size = 1.0f;
    Color mid_color{255, 255, 255, 255};
    std::shared_ptr<const ParticleLook> look{};
    u32 emitter = 0;      // the emitter's id it came from (0: none)
    u8 depth = 0;         // sub-effects deep
    f32 until_trail = 0.0f;
};

// An effect streaming particles from where it is (ParticleSystem::start).
struct ParticleEmitter {
    u32 id = 0;
    explicit operator bool() const { return id != 0; }
    bool operator==(const ParticleEmitter&) const = default;
};

struct ParticleRenderOptions {
    ParticlePixelPolicy pixel_policy = ParticlePixelPolicy::Free;
    f32 pixel_grid = 1.0f;
    const SpriteCatalog* sprites = nullptr;
    bool sort = true;
    BlendMode blend = BlendMode::Alpha; // Additive: sparks, fire, glows
};

class ParticleSystem {
public:
    explicit ParticleSystem(RngKey seed = make_key(1));
    explicit ParticleSystem(u64 seed);
    ParticleSystem(const ParticleSystem& other);
    ParticleSystem& operator=(const ParticleSystem& other);
    ParticleSystem(ParticleSystem&&) noexcept = default;
    ParticleSystem& operator=(ParticleSystem&&) noexcept = default;

    void set_seed(RngKey seed);
    RngKey seed() const { return _key; }
    void reserve(std::size_t capacity);
    void clear();

    void set_catalog(ParticleCatalog catalog);
    void set_catalog(std::shared_ptr<const ParticleCatalog> catalog);
    void set_catalog(AssetHandle<ParticleCatalog> catalog);
    void clear_catalog();

    void set_pixel_policy(ParticlePixelPolicy policy) { _pixel_policy = policy; }
    ParticlePixelPolicy pixel_policy() const { return _pixel_policy; }
    void set_pixel_grid(f32 grid);
    f32 pixel_grid() const { return _pixel_grid; }

    void emit(const Particle& particle);
    void burst(const ParticleBurst& burst);
    bool emit(std::string_view effect_name, Vec2f position);
    bool emit(std::string_view effect_name, Vec2f position, Vec2f offset);

    // An effect that streams (ParticleEffect::streams) started at `position`,
    // turned `angle` degrees (its direction and shape with it); one that does
    // not is burst there once instead, and none returned.
    ParticleEmitter start(std::string_view effect_name, Vec2f position, f32 angle = 0.0f);
    // Where it is now: an update emits along the way from where it was.
    bool move(ParticleEmitter emitter, Vec2f position);
    bool move(ParticleEmitter emitter, Vec2f position, f32 angle);
    // Where it is now, emitting nothing on the way (and its local particles not moved).
    bool jump(ParticleEmitter emitter, Vec2f position);
    // It emits no more; its particles live on (and still move with it, local).
    void stop(ParticleEmitter emitter);
    bool emitting(ParticleEmitter emitter) const;
    std::size_t emitter_count() const { return _emitters.size(); }
    // The most particles emitters and sub-effects make (bursts and emit are not held to it).
    void set_limit(std::size_t limit) { _limit = limit; }
    std::size_t limit() const { return _limit; }

    void update(f32 dt);
    void render(Renderer2D& renderer, const SpriteCatalog* sprites = nullptr) const;
    void render(Renderer2D& renderer, ParticleRenderOptions options) const;

    std::size_t active_count() const { return _particles.size(); }
    // The catalog its effects are from (set_catalog's); null when none is.
    const ParticleCatalog* catalog() const;
    bool empty() const { return _particles.empty(); }
    const std::vector<Particle>& particles() const { return _particles; }

private:
    struct Emitter {
        u32 id = 0;
        std::string effect;
        Vec2f position{};
        Vec2f was{};      // where it was at the last update
        Vec2f shifted{};  // where its local particles were last moved to
        f32 angle = 0.0f;
        f32 age = 0.0f;
        f32 owed = 0.0f;  // particles part made
        bool on = true;
    };

    RngKey split_key();
    f32 random_range(ParticleRange range);
    Particle make(const ParticleBurst& burst, Vec2f at, f32 angle);
    void sub_burst(std::string_view effect_name, Vec2f position, u8 depth);
    void stream(Emitter& emitter, const ParticleEffect& effect, f32 dt);
    Emitter* find(ParticleEmitter emitter);
    const Emitter* find(ParticleEmitter emitter) const;

    std::vector<Particle> _particles;
    std::vector<Emitter> _emitters;
    u32 _next_emitter = 1;
    std::size_t _limit = 16384;
    RngKey _key = make_key(1);
    std::unique_ptr<ParticleCatalog> _catalog_copy;
    std::shared_ptr<const ParticleCatalog> _catalog_shared;
    AssetHandle<ParticleCatalog> _catalog_handle;
    ParticlePixelPolicy _pixel_policy = ParticlePixelPolicy::Free;
    f32 _pixel_grid = 1.0f;
};

Color particle_color_at(const Particle& particle);
f32 particle_size_at(const Particle& particle);
Rectf particle_rect(const Particle& particle, ParticlePixelPolicy policy, f32 pixel_grid);
void render_particles(Renderer2D& renderer,
                      const std::vector<Particle>& particles,
                      ParticleRenderOptions options = {});
bool submit_particles(RenderQueue& queue,
                      const std::vector<Particle>& particles,
                      ParticleRenderOptions options = {});

} // namespace kin
