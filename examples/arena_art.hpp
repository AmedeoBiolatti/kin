#pragma once

#include "workloads.hpp"

#include <kin/particles/particle_system.hpp>
#include <kin/renderer/lighting.hpp>
#include <kin/renderer/post_process.hpp>
#include <kin/renderer/sprite_catalog.hpp>

#include <array>
#include <vector>

namespace examples {

// Signal Siege's look, built at startup without asset files: shaded sprites
// rasterized from signed-distance shapes, a lit arena (kin::LightLayer),
// particles driven by the arena's events (kin::ParticleSystem), and a bloom,
// vignette and grade chain on the GPU backend. It only reads the simulation, so
// the benchmark workload, which never builds one, is unaffected.
class ArenaPainter {
public:
    // Creates the textures and hands the enemy ones to the arena's ECS renderer.
    void init(Renderer2D& renderer, Arena& arena);
    bool ready() const { return _ready; }

    // Turns the arena's events into particles and advances them; call with the
    // simulation's dt, so effects freeze with it. Needs no renderer, so effects
    // are already in flight when a run is first drawn (e.g. a server screenshot).
    void update(Arena& arena, float dt);

    // Floor, arena and effects for the view, lit; the HUD goes on top afterwards.
    void draw(Renderer2D& renderer, Arena& arena, const Camera2D& camera);

    // The bloom/vignette/grade chain (GPU backend only; others ignore it).
    void enable_post_process(Renderer2D& renderer);
    void disable_post_process(Renderer2D& renderer);

    std::size_t commands() const { return _world.size() + _glow.size() + _emissive.size(); }
    std::size_t particles() const { return _particles.active_count(); }
    std::size_t lights() const { return _lights.size(); }
    std::size_t visible_enemies() const { return _visible.size(); }
    bool lit() const { return _lit; }

private:
    void draw_floor(Renderer2D& renderer, const Arena& arena, const Camera2D& camera);
    void collect_effects(const Arena& arena, const RenderView& view);
    void collect_lights(const Arena& arena, const Camera2D& camera);
    void draw_shadows(Renderer2D& renderer, const Arena& arena, const Camera2D& camera);
    void gather_visible(const Arena& arena, const Camera2D& camera);

    struct VisibleEnemy {
        Vec2f pos;
        int kind = 0;
        float hp = 0, cooldown = 0;
    };
    std::vector<VisibleEnemy> _visible; // this frame's enemies near the view
    std::vector<SpriteInstance> _shadows;

    bool _ready = false;
    bool _lit = false;
    Texture _player, _core, _shot, _bullet, _ring, _cone, _halo, _shadow, _reactor, _fan, _edge;
    std::array<Texture, 4> _floors;
    Texture _enemies; // all three classes, see make_enemy_atlas()
    SpriteCatalog _sprites;
    LightLayer _lighting;
    ParticleSystem _particles{make_key(0x5167'5ee9)};
    std::vector<Light2D> _lights;
    RenderQueue _world;                                 // lit: enemies, player
    RenderQueue _emissive{RenderSortMode::Submission};  // unlit, alpha-blended: shots, cores
    RenderQueue _glow{RenderSortMode::Submission};      // unlit, additive: halos, particles
    std::vector<PostProcessPass> _post;
    Vec2i _post_size{};
    bool _post_enabled = false;
    float _thrust_timer = 0;
};

} // namespace examples
