#pragma once

#include <kin/assets/asset_handle.hpp>
#include <kin/core/rng.hpp>
#include <kin/particles/particle_catalog.hpp>
#include <kin/renderer/render_queue.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <memory>
#include <vector>

namespace kin {

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
};

struct ParticleRenderOptions {
    ParticlePixelPolicy pixel_policy = ParticlePixelPolicy::Free;
    f32 pixel_grid = 1.0f;
    const SpriteCatalog* sprites = nullptr;
    bool sort = true;
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

    void update(f32 dt);
    void render(Renderer2D& renderer, const SpriteCatalog* sprites = nullptr) const;
    void render(Renderer2D& renderer, ParticleRenderOptions options) const;

    std::size_t active_count() const { return _particles.size(); }
    bool empty() const { return _particles.empty(); }
    const std::vector<Particle>& particles() const { return _particles; }

private:
    RngKey split_key();
    f32 random_range(ParticleRange range);
    const ParticleCatalog* catalog() const;

    std::vector<Particle> _particles;
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
