#pragma once

#include "breach.hpp"

#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/post_process.hpp>
#include <kin/ui2/context.hpp>

#include <memory>
#include <vector>

namespace examples {

struct BreachHudState {
    bool paused = false, autoplay = false, captured = false;
    float display_scale = 1;
    float yaw = 0, pitch = 0; // where the player looks now (ahead of the last step's)
};

// Signal Breach drawn: the deck, its walls and ramps, the enemies (from the
// ECS: MeshRenderer, WorldRenderState3D), their shots, cores, sparks and the
// player's tracers, seen from the player's eye; the gun in the view's overlay;
// health bars, damage numbers and the HUD in 2D on top. Reads the simulation
// only, as ArenaPainter does.
class BreachView {
public:
    void init(Renderer2D& renderer, Breach& breach);
    bool ready() const { return _r3d != nullptr; }
    // Turns the simulation's events into effects and advances them (with the
    // simulation's dt, so they freeze with it).
    void update(Breach& breach, float dt);
    void draw(Renderer2D& renderer, Breach& breach, Input& input, const BreachHudState& state, float dt);
    void enable_post_process(Renderer2D& renderer);

    const Render3DStats& stats() const;
    bool available() const { return _r3d && _r3d->available(); }
    // The camera of the last frame drawn, and the playfield it fills (logical).
    const Camera3D& camera() const { return _scene.camera; }
    Rectf playfield() const { return _playfield; }

private:
    void build_level(const BreachLevel& level);
    void fill_scene(Breach& breach, const BreachHudState& state);
    void draw_hud(Renderer2D& renderer, const Breach& breach, Input& input, const BreachHudState& state, float dt);

    std::unique_ptr<Renderer3D> _r3d;
    View3D _view;
    Scene3D _scene;
    Rectf _playfield{0, 0, 1280, 800};
    Mesh _box, _wedge, _shot, _core, _spark, _barrel, _flash;
    std::vector<MeshInstance> _tiles, _raised, _walls, _ramps;

    struct Spark { Vec3f pos{}, velocity{}; float life = 0; Color color{}; };
    struct Tracer { Vec3f from{}, to{}; float life = 0; };
    struct Number { Vec3f pos{}; float life = 0; bool kill = false; };
    std::vector<Spark> _sparks;
    std::vector<Tracer> _tracers;
    std::vector<Number> _numbers;
    float _flash_time = 0, _recoil = 0, _walked = 0;
    RngKey _rng = make_key(0xB4EAC4);

    ui2::Context _ui;
    std::vector<PostProcessPass> _post;
    Vec2i _post_size{};
};

} // namespace examples
