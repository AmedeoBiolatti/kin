#pragma once

#include "breach.hpp"

#include <kin/render3d/model3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/ui2/context.hpp>
#include <memory>
#include <vector>

namespace examples {

// What the view draws (each can be left off, to measure what it costs).
struct BreachViewOptions {
    bool shadows = true; // the sun's, over the deck
    bool bloom = true;   // an HDR view with glow round what is past white
    bool lights = true;  // muzzle flashes and explosions as point lights
};

struct BreachHudState {
    bool paused = false, autoplay = false, captured = false;
    float display_scale = 1;
    float yaw = 0, pitch = 0; // where the player looks now (ahead of the last step's)
};

// Signal Breach drawn: the deck, its walls and ramps, the enemies (from the
// ECS: MeshRenderer3D, WorldRenderState3D), their shots, cores, sparks and the
// player's tracers, seen from the player's eye; the gun in the view's overlay;
// health bars, damage numbers and the HUD in 2D on top. Reads the simulation
// only, as ArenaPainter does.
class BreachView {
public:
    void init(Renderer2D& renderer, Breach& breach, const BreachViewOptions& options = {});
    bool ready() const { return _r3d != nullptr; }
    // Turns the simulation's events into effects and advances them (with the
    // simulation's dt, so they freeze with it).
    void update(Breach& breach, float dt);
    void draw(Renderer2D& renderer, Breach& breach, Input& input, const BreachHudState& state, float dt);

    const RenderStats3D& stats() const;
    bool available() const { return _r3d && _r3d->available(); }
    // The camera of the last frame drawn, and the playfield it fills (logical).
    const Camera3D& camera() const { return _scene.camera; }
    Rectf playfield() const { return _playfield; }

private:
    void build_level(const BreachLevel& level);
    void fill_scene(Breach& breach, const BreachHudState& state);
    void draw_hud(Renderer2D& renderer, const Breach& breach, Input& input, const BreachHudState& state, float dt);

    std::unique_ptr<Renderer3D> _r3d;
    BreachViewOptions _options;
    View3D _view;
    Scene3D _scene;
    Rectf _playfield{0, 0, 1280, 800};
    Mesh3D _box, _wedge, _shot, _core, _spark, _barrel, _flash;
    // glTF models from the content folder, loaded again when they change
    // (tools/models/make_breach_models.py); the shapes above stand in without them.
    std::unique_ptr<ModelFile3D> _gun, _crate;
    std::vector<MeshInstance3D> _tiles, _raised, _walls, _ramps;
    std::vector<Vec3f> _crates; // where crates stand

    // Shields: shells that flash round an enemy where a shot hit it (shield.frag.glsl).
    struct Shell {
        Vec3f pos{};
        float life = 0;
        Color color{};
    };
    std::vector<Shell> _shells;
    MaterialShader3D _shield;
    Mesh3D _shell;
    struct Spark {
        Vec3f pos{}, velocity{};
        float life = 0;
        Color color{};
    };
    struct Flash {
        Vec3f pos{};
        LinearRgb color{};
        float radius = 0, life = 0, length = 0, size = 0.3f;
    };
    std::vector<Flash> _flashes;
    struct Tracer {
        Vec3f from{}, to{};
        float life = 0;
    };
    struct Number {
        Vec3f pos{};
        float life = 0;
        bool kill = false;
    };
    std::vector<Spark> _sparks;
    std::vector<Tracer> _tracers;
    std::vector<Number> _numbers;
    float _flash_time = 0, _recoil = 0, _walked = 0;
    RngKey _rng = make_key(0xB4EAC4);

    ui2::Context _ui;
};

} // namespace examples
