// terrain_editor: sculpt and paint a heightmap terrain (kin/render3d/terrain3d.hpp, docs/rendering_3d.md, Terrain).
// A 1025 x 1025 terrain of fractal hills (or a .kinterrain file) with four painted layers (grass, rock, sand, snow),
// drawn as CDLOD under kin's terrain surface (compiled at build time: kin_terrain_surface); Jolt's height field of it
// follows each stroke, and balls dropped on it roll down what was sculpted.
//
//   terrain_editor [--file terrain.kinterrain] [--load] [--shot picture.png] [--frames N]
//
// Right mouse drag: look. W A S D: move, Q / E: down / up, Left Shift: faster. Left mouse: the brush, a stroke while
// held. 1 raise, 2 lower, 3 smooth, 4 flatten (to the height where the stroke began), 5 noise, 6 paint. Z / X: the
// layer painted. , / .: the brush's radius. - / =: its strength. Ctrl+Z / Ctrl+Y: undo / redo. B: drop a ball at the
// brush. F5: save to --file, F9: load it. Esc: quit. --shot sculpts a few strokes, renders headless and writes a
// picture (with KIN_RENDER_BACKEND=gpu).
#include "terrain_surface.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/physics3d/terrain_shape3d.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/terrain3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace kin;
namespace fs = std::filesystem;

struct Options {
    fs::path file = "terrain.kinterrain";
    bool load = false;
    fs::path shot;
    int frames = 30;
};

f32 hash(i32 x, i32 z, i32 seed) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(z) * 668265263u + static_cast<u32>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<f32>((h ^ (h >> 16)) & 0xffffffu) / 16777215.0f;
}
f32 noise(f32 x, f32 z, i32 seed) {
    const i32 ix = static_cast<i32>(std::floor(x)), iz = static_cast<i32>(std::floor(z));
    const f32 fx = x - static_cast<f32>(ix), fz = z - static_cast<f32>(iz);
    const f32 ux = fx * fx * (3 - 2 * fx), uz = fz * fz * (3 - 2 * fz);
    const f32 a = hash(ix, iz, seed), b = hash(ix + 1, iz, seed), c = hash(ix, iz + 1, seed),
              d = hash(ix + 1, iz + 1, seed);
    const f32 top = a + (b - a) * ux, bottom = c + (d - c) * ux;
    return top + (bottom - top) * uz;
}
f32 fractal(f32 x, f32 z, i32 seed, int octaves) {
    f32 sum = 0, amplitude = 0.5f, frequency = 1;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * noise(x * frequency, z * frequency, seed + o);
        amplitude *= 0.5f;
        frequency *= 2;
    }
    return sum;
}

// A noise that repeats every `period` cells (a tiling picture's).
f32 tiled_noise(f32 x, f32 z, i32 seed, i32 period) {
    const i32 ix = static_cast<i32>(std::floor(x)), iz = static_cast<i32>(std::floor(z));
    const f32 fx = x - static_cast<f32>(ix), fz = z - static_cast<f32>(iz);
    const f32 ux = fx * fx * (3 - 2 * fx), uz = fz * fz * (3 - 2 * fz);
    const auto at = [&](i32 cx, i32 cz) {
        return hash(((cx % period) + period) % period, ((cz % period) + period) % period, seed);
    };
    const f32 top = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * ux;
    const f32 bottom = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * ux;
    return top + (bottom - top) * uz;
}

// A layer's picture, which tiles: two colours mixed by a noise of `cells` cells across, its alpha the noise (the
// layer's height for blending).
Image picture(LinearRgb a, LinearRgb b, i32 cells, i32 seed) {
    Image image{{128, 128}, std::vector<u8>(128 * 128 * 4)};
    const auto srgb = [](f32 c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return static_cast<u8>(
            std::lround(255 * (c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f)));
    };
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            f32 v = 0, amplitude = 0.5f;
            for (int o = 0, n = cells; o < 4; ++o, n *= 2, amplitude *= 0.5f)
                v += amplitude * tiled_noise(static_cast<f32>(x) * static_cast<f32>(n) / 128,
                                             static_cast<f32>(y) * static_cast<f32>(n) / 128, seed + o, n);
            v = std::clamp(v, 0.0f, 1.0f);
            u8* p = &image.rgba[static_cast<std::size_t>(y * 128 + x) * 4];
            p[0] = srgb(a.r + (b.r - a.r) * v);
            p[1] = srgb(a.g + (b.g - a.g) * v);
            p[2] = srgb(a.b + (b.b - a.b) * v);
            p[3] = static_cast<u8>(std::lround(v * 255));
        }
    return image;
}

class Editor final : public Scene {
public:
    explicit Editor(Options o) : _o(std::move(o)) {}
    std::string_view name() const override { return "Terrain editor"; }

    void update(SceneContext& ctx) override {
        if (!_r)
            setup(ctx);
        const Input& in = ctx.input;
        if (in.pressed(Key::Escape))
            ctx.app.quit();
        const bool ctrl = in.modifier_held(KeyModifiers::Ctrl);
        // The camera.
        if (in.mouse_held(MouseButton::Right)) {
            if (_looking) {
                _yaw -= (in.mouse_pos().x - _last_mouse.x) * 0.004f;
                _pitch = std::clamp(_pitch - (in.mouse_pos().y - _last_mouse.y) * 0.004f, -1.5f, 1.5f);
            }
            _looking = true;
        } else {
            _looking = false;
        }
        _last_mouse = in.mouse_pos();
        const Vec3f forward{std::cos(_pitch) * std::sin(_yaw), std::sin(_pitch), std::cos(_pitch) * std::cos(_yaw)};
        const Vec3f right = normalize(cross(forward, Vec3f{0, 1, 0}));
        const f32 speed = (in.held(Key::LeftShift) ? 120.0f : 30.0f) * ctx.dt;
        if (!ctrl) {
            if (in.held(Key::W))
                _eye += forward * speed;
            if (in.held(Key::S))
                _eye -= forward * speed;
            if (in.held(Key::D))
                _eye += right * speed;
            if (in.held(Key::A))
                _eye -= right * speed;
        }
        if (in.held(Key::E))
            _eye.y += speed;
        if (in.held(Key::Q))
            _eye.y -= speed;
        if (const auto ground = _terrain.height({_eye.x, _eye.z}))
            _eye.y = std::max(_eye.y, *ground + 1.5f);

        // The brush's settings.
        const std::array modes{Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6};
        for (std::size_t i = 0; i < modes.size(); ++i)
            if (in.pressed(modes[i])) {
                _brush.mode = static_cast<TerrainBrushMode3D>(i);
                say();
            }
        if (in.pressed(Key::Z) && !ctrl) {
            _brush.layer = (_brush.layer + _terrain.layer_count() - 1) % _terrain.layer_count();
            say();
        }
        if (in.pressed(Key::X)) {
            _brush.layer = (_brush.layer + 1) % _terrain.layer_count();
            say();
        }
        if (in.pressed(Key::Comma) || in.pressed(Key::Period)) {
            _brush.radius = std::clamp(_brush.radius * (in.pressed(Key::Period) ? 1.25f : 0.8f), 1.0f, 200.0f);
            say();
        }
        if (in.pressed(Key::Minus) || in.pressed(Key::Equals)) {
            _rate = std::clamp(_rate * (in.pressed(Key::Equals) ? 1.25f : 0.8f), 0.05f, 40.0f);
            say();
        }
        if (ctrl && in.pressed(Key::Z))
            if (const auto changed = _terrain.undo())
                update_collision(*changed, "undo");
        if (ctrl && in.pressed(Key::Y))
            if (const auto changed = _terrain.redo())
                update_collision(*changed, "redo");
        if (in.pressed(Key::F5)) {
            const auto saved = _terrain.save(_o.file);
            std::cout << (saved ? "saved " : "not saved: ")
                      << (saved ? _o.file.string() : diagnostics3d_text(saved.diagnostics)) << '\n';
        }
        if (in.pressed(Key::F9))
            load();

        // Where the mouse points on the terrain; the brush there while the left button is held.
        const Vec2i size = ctx.renderer.output_size();
        const Ray3 ray = camera().ray(in.mouse_pos(), {0, 0, static_cast<f32>(size.x), static_cast<f32>(size.y)});
        const auto hit = _terrain.raycast(ray.origin, ray.direction, 5000.0f);
        _aim = hit ? std::optional<Vec3f>{hit->position} : std::nullopt;
        if (in.mouse_held(MouseButton::Left) && _aim) {
            if (!_stroking) {
                _terrain.begin_stroke();
                _stroking = true;
                _brush.height = _aim->y; // (flatten: to where the stroke began)
            }
            dab({_aim->x, _aim->z}, ctx.dt);
        } else if (_stroking && !in.mouse_held(MouseButton::Left)) {
            _stroking = false;
            if (const auto changed = _terrain.end_stroke(); !changed.empty())
                update_collision(changed, "stroke");
        }
        if (in.pressed(Key::B) && _aim)
            _balls.push_back(_world.create_body({.shape = Shape3D::sphere(1.5f),
                                                 .position = *_aim + Vec3f{0, 25, 0},
                                                 .friction = 0.6f,
                                                 .restitution = 0.2f}));
        _world.step(std::min(ctx.dt, 1.0f / 30.0f));
    }

    void render(SceneContext& ctx) override {
        if (!_r)
            return;
        const Vec2i size = ctx.renderer.output_size();
        if (!_view.valid())
            _view = _r->create_view(size, {.samples = 4, .hdr = true}).value_or({});
        else if (_view.texture().size() != size)
            (void)_r->resize(_view, size);
        if (!_o.shot.empty() && _frame == 0)
            sculpt_for_the_shot();

        _scene.clear();
        _scene.camera = camera();
        _scene.camera.z_far = 6000;
        _scene.sky = {.enabled = true, .turbidity = 2.5f, .sun_intensity = 1.0f};
        _scene.lights.sky_lights_scene = true;
        apply({.hours = 15.5f, .day_of_year = 200, .latitude = 45}, _scene.lights, _scene.sky, _scene.fog);
        _scene.lights.shadow_cascades = 4;
        _scene.lights.shadow_distance = 1200;
        _scene.lights.shadow_resolution = 2048;
        _scene.lights.shadow_area = {{-520, -10, -520}, {520, 260, 520}};
        _scene.taa.enabled = true;
        (void)_terrain.draw(_scene, 1);
        // The brush: a ring of small beads on the ground round where the mouse points.
        if (_aim) {
            _ring.clear();
            for (int i = 0; i < 48; ++i) {
                const f32 a = static_cast<f32>(i) * 6.2831853f / 48;
                const Vec2f p{_aim->x + std::cos(a) * _brush.radius, _aim->z + std::sin(a) * _brush.radius};
                const f32 y = _terrain.height(p).value_or(_aim->y) + 0.3f;
                const f32 bead = std::max(0.25f, _brush.radius * 0.025f);
                _ring.push_back({.transform = Mat4f::translation({p.x, y, p.y}) * Mat4f::scaling({bead, bead, bead})});
            }
            (void)_scene.draw_instanced(
                _bead, _ring,
                {.color = Color::rgb(255, 210, 60), .emissive = 2.0f, .unlit = true, .casts_shadow = false});
        }
        for (const Body3D ball : _balls)
            (void)_scene.draw(_bead, Mat4f::translation(_world.position(ball)) * Mat4f::scaling({3, 3, 3}),
                              {.color = Color::rgb(220, 70, 50), .roughness = 0.4f});
        if (!_r->render(_view, _scene)) {
            if (ctx.report)
                ctx.report->fail("the 3D render failed");
            ctx.app.quit();
            return;
        }
        ctx.renderer.clear(Color::rgb(0, 0, 0));
        ctx.renderer.draw_texture(_view.texture(), {0, 0, static_cast<f32>(size.x), static_cast<f32>(size.y)});
        if (!_o.shot.empty() && ++_frame == _o.frames) {
            Image image;
            if (!ctx.renderer.read_rgba({0, 0, static_cast<f32>(size.x), static_cast<f32>(size.y)}, image.rgba,
                                        image.size) ||
                !save_image(image, _o.shot))
                throw std::runtime_error("cannot write " + _o.shot.string());
            std::cout << "terrain_editor: wrote " << _o.shot.string() << '\n';
            ctx.app.quit();
        }
    }

private:
    void setup(SceneContext& ctx) {
        _r = ctx.renderer3d;
        if (!_r) {
            _own = std::make_unique<Renderer3D>(ctx.renderer);
            _r = _own.get();
        }
        if (!_r->available())
            throw std::runtime_error("needs the GPU 3D renderer (for --shot: KIN_RENDER_BACKEND=gpu)");
        auto surface = create_terrain_surface(*_r, terrain_surface);
        if (!surface)
            throw std::runtime_error("cannot make the terrain surface: " + diagnostics3d_text(surface.diagnostics));
        _surface = *surface;
        _bead = _r->create_mesh(meshes::sphere(0.5f, 12, 6)).value_or({});
        if (_o.load && fs::exists(_o.file))
            load();
        if (!_terrain.valid())
            make();
        std::cout
            << "terrain_editor: right drag look, WASD/QE move (Shift faster), left mouse brush; 1 raise, 2 lower, "
               "3 smooth, 4 flatten, 5 noise, 6 paint; Z/X layer; ,/. radius; -/= strength; Ctrl+Z/Y undo/redo; "
               "B ball; F5 save, F9 load\n";
        say();
    }

    // Fractal hills, 1025 x 1025 samples a metre apart (heights to 240 m), and four layers by slope and height.
    void make() {
        constexpr i32 n = 1025;
        std::vector<u16> heights(static_cast<std::size_t>(n) * n);
        for (i32 z = 0; z < n; ++z)
            for (i32 x = 0; x < n; ++x) {
                const f32 v = fractal(static_cast<f32>(x) / 320.0f, static_cast<f32>(z) / 320.0f, 5, 8);
                heights[static_cast<std::size_t>(z) * n + static_cast<std::size_t>(x)] =
                    static_cast<u16>(std::lround(std::pow(std::clamp(v, 0.0f, 1.0f), 1.5f) * 65535));
            }
        auto made = Terrain3D::create(
            *_r, _surface, {.samples = {n, n}, .spacing = 1.0f, .origin = {-512, 0, -512}, .max_height = 240}, heights);
        if (!made)
            throw std::runtime_error("cannot make the terrain: " + diagnostics3d_text(made.diagnostics));
        _terrain = std::move(*made);
        std::array<TerrainLayer3D, 4> layers{};
        layers[0] = {
            .albedo = picture({0.10f, 0.20f, 0.05f}, {0.22f, 0.32f, 0.08f}, 8, 11), .roughness = 0.9f, .tile = 6};
        layers[1] = {
            .albedo = picture({0.16f, 0.15f, 0.13f}, {0.40f, 0.37f, 0.33f}, 16, 21), .roughness = 0.75f, .tile = 9};
        layers[2] = {
            .albedo = picture({0.55f, 0.45f, 0.30f}, {0.70f, 0.60f, 0.42f}, 32, 31), .roughness = 0.95f, .tile = 3};
        layers[3] = {
            .albedo = picture({0.80f, 0.82f, 0.86f}, {0.95f, 0.96f, 0.98f}, 4, 41), .roughness = 0.5f, .tile = 12};
        (void)_terrain.set_layers(layers, {128, 128});
        std::array<std::vector<u8>, 4> w;
        for (auto& v : w)
            v.resize(heights.size());
        for (i32 z = 0; z < n; ++z)
            for (i32 x = 0; x < n; ++x) {
                const Vec2f p{-512.0f + static_cast<f32>(x), -512.0f + static_cast<f32>(z)};
                const f32 slope = 1 - _terrain.normal(p).value_or(Vec3f{0, 1, 0}).y;
                const f32 h = _terrain.height(p).value_or(0) / 240.0f;
                const f32 rock = std::clamp((slope - 0.15f) * 7, 0.0f, 1.0f);
                const f32 snow = std::clamp((h - 0.55f) * 8, 0.0f, 1.0f) * (1 - rock);
                const f32 sand = std::clamp((0.06f - h) * 25, 0.0f, 1.0f) * (1 - rock);
                const std::size_t i = static_cast<std::size_t>(z) * n + static_cast<std::size_t>(x);
                w[1][i] = static_cast<u8>(std::lround(rock * 255));
                w[2][i] = static_cast<u8>(std::lround(sand * 255));
                w[3][i] = static_cast<u8>(std::lround(snow * 255));
                w[0][i] = static_cast<u8>(std::lround((1 - rock) * (1 - snow) * (1 - sand) * 255));
            }
        for (u32 l = 0; l < 4; ++l)
            (void)_terrain.set_weights(l, {0, 0}, {n, n}, w[l]);
        _eye = {-420, 300, -420};
        rebuild_collision("made");
    }

    void load() {
        auto loaded = Terrain3D::load(*_r, _surface, _o.file);
        if (!loaded) {
            std::cout << "not loaded: " << diagnostics3d_text(loaded.diagnostics) << '\n';
            return;
        }
        _terrain = std::move(*loaded);
        std::cout << "loaded " << _o.file.string() << '\n';
        rebuild_collision("loaded");
    }

    // The ground's collision again, after the heights changed (one height field of it all).
    void rebuild_collision(const char* why) {
        const auto start = std::chrono::steady_clock::now();
        const Shape3D shape = terrain_shape3d(_terrain);
        if (_ground)
            (void)_world.set_shape(_ground, shape);
        else
            _ground = _world.create_body({.shape = shape, .motion = Body3DMotion::Static});
        const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "collision (" << why << "): " << ms << " ms\n";
    }

    // The ground's collision over what a stroke (or an undo) changed, in place.
    void update_collision(Terrain3D::Changed changed, const char* why) {
        const auto start = std::chrono::steady_clock::now();
        if (!update_terrain_collision(_world, _ground, _terrain, changed)) {
            rebuild_collision(why);
            return;
        }
        const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "collision (" << why << ", " << changed.size.x << " x " << changed.size.y
                  << " samples in place): " << ms << " ms\n";
    }

    void dab(Vec2f at, f32 dt) {
        TerrainBrush3D b = _brush;
        // (Rates a second: metres for raise, lower and noise; a share for the others.)
        const bool metres = b.mode == TerrainBrushMode3D::Raise || b.mode == TerrainBrushMode3D::Lower ||
                            b.mode == TerrainBrushMode3D::Noise;
        b.strength = metres ? _rate * 4 : std::min(_rate * 0.5f, 1.0f);
        b.scale = std::max(2.0f, b.radius * 0.3f);
        (void)_terrain.brush(b, at, dt);
    }

    // --shot: a ridge raised, a crater lowered and flattened, a sandy path painted.
    void sculpt_for_the_shot() {
        const f32 dt = 1.0f / 30;
        _brush.radius = 40;
        _brush.mode = TerrainBrushMode3D::Raise;
        _terrain.begin_stroke();
        for (int i = 0; i < 60; ++i)
            dab({-200.0f + static_cast<f32>(i) * 4, -120}, dt);
        update_collision(_terrain.end_stroke(), "shot ridge");
        _brush.mode = TerrainBrushMode3D::Lower;
        _terrain.begin_stroke();
        for (int i = 0; i < 40; ++i)
            dab({-60, -260}, dt);
        (void)_terrain.end_stroke();
        _brush.mode = TerrainBrushMode3D::Paint;
        _brush.layer = 2;
        _brush.radius = 12;
        _terrain.begin_stroke();
        for (int i = 0; i < 80; ++i)
            dab({-260.0f + static_cast<f32>(i) * 3, -200.0f + std::sin(static_cast<f32>(i) * 0.1f) * 30}, 0.2f);
        (void)_terrain.end_stroke();
        rebuild_collision("shot");
        _aim = Vec3f{-120, _terrain.height({-120, -200}).value_or(0), -200};
    }

    Camera3D camera() const {
        const Vec3f forward{std::cos(_pitch) * std::sin(_yaw), std::sin(_pitch), std::cos(_pitch) * std::cos(_yaw)};
        return Camera3D::look_at(_eye, _eye + forward, 60);
    }

    void say() const {
        static constexpr const char* names[] = {"raise", "lower", "smooth", "flatten", "noise", "paint"};
        std::cout << "brush: " << names[static_cast<int>(_brush.mode)] << ", radius " << _brush.radius << " m, rate "
                  << _rate << ", layer " << _brush.layer << '\n';
    }

    Options _o;
    Renderer3D* _r = nullptr;
    std::unique_ptr<Renderer3D> _own;
    View3D _view;
    Scene3D _scene;
    SurfaceShader3D _surface;
    Terrain3D _terrain;
    Mesh3D _bead;
    std::vector<MeshInstance3D> _ring;
    World3D _world;
    Body3D _ground{};
    std::vector<Body3D> _balls;
    TerrainBrush3D _brush{.radius = 20};
    f32 _rate = 1.0f;
    bool _stroking = false, _looking = false;
    std::optional<Vec3f> _aim;
    Vec3f _eye{-420, 300, -420};
    f32 _yaw = 0.785f, _pitch = -0.35f;
    Vec2f _last_mouse{};
    int _frame = 0;
};

} // namespace

int main(int argc, char** argv) {
    try {
        Options o;
        std::vector<char*> runtime{argv[0]};
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc)
                    throw std::runtime_error(a + " needs a value");
                return argv[++i];
            };
            if (a == "--file")
                o.file = value();
            else if (a == "--load")
                o.load = true;
            else if (a == "--shot")
                o.shot = value();
            else if (a == "--frames")
                o.frames = std::max(2, std::stoi(value()));
            else
                runtime.push_back(argv[i]);
        }
        auto headless = parse_headless_options(static_cast<int>(runtime.size()), runtime.data());
        if (!o.shot.empty()) {
            headless.enabled = true;
            headless.render = true;
            headless.frames = o.frames + 1;
        }
        SceneManager scenes;
        scenes.push(std::make_unique<Editor>(o));
        return run_scene_app({.window = {.title = "Terrain editor", .width = 1280, .height = 720, .max_fps = 60},
                              .headless = headless,
                              .render_headless = true,
                              .rendering_3d = true},
                             scenes);
    } catch (const std::exception& e) {
        std::cerr << "terrain_editor: " << e.what() << '\n';
        return 1;
    }
}
