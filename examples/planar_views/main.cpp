// planar_views: mirrors and portals (docs/rendering_3d.md, Mirrors and portals). A courtyard under a procedural sky
// with a mirror on its back wall, a basin of rippled water, and two portal arches facing each other (so each shows the
// other, and itself, levels deep), an orb flying through them; each drawn from a view of its own (Camera3D's
// mirror_view and portal_chain; planar_on_screen skips the views no one sees) that kin's planar surfaces read
// (engine/shaders/planar_reflection.surface.glsl and planar_window.surface.glsl, compiled here at build time).
//
//   planar_views [--shot picture.png] [--camera N] [--frames N] [--scale S] [--depth N] [--bench N] [--planar off]
//
// --camera picks a camera (0: orbit, 1: the mirror, 2: the water, 3: into the west portal, 4: into the east one, 5:
// walking through them, round and round); --depth is how many levels of portals in portals are drawn (3);
// --scale is the pictures' sharpness against the view's (1: as sharp; each covers only where its surface shows). --shot
// renders headless and writes a picture (with KIN_RENDER_BACKEND=gpu). --bench renders N frames in its window and says
// what each view's recording cost the CPU (averaged after the first 30); with --profile the run's profile also has each
// view's GPU time (gpu.mirror, gpu.water, gpu.portal_a, gpu.portal_b, gpu.main). --planar off draws the planar surfaces
// with their stand-ins and renders no planar views (the cost without them). Mouse drag: orbit. Wheel: zoom. 1-5: the
// cameras, 0: orbit. Esc: quit.
#include "planar_surfaces.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/render3d/diagnostics3d.hpp>
#include <kin/render3d/mesh3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/timeofday3d.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/planar_views3d.hpp>
#include <map>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace kin;
namespace fs = std::filesystem;

struct Options {
    fs::path shot;
    int camera = 0;
    int frames = 60; // before the shot
    f32 scale = 1.0f;
    int bench = 0;      // frames to time (0: none)
    bool planar = true; // render the planar views
    bool probes = true; // the reflection probes: the courtyard's, and the chrome ball's own
    bool ssr = true;    // screen-space reflections
    u32 depth = 3;      // a portal's levels: portals seen through portals
};

// --bench's sums: a view's CPU milliseconds over the frames counted.
struct Timing {
    f64 cpu = 0;
    int cpu_n = 0;
};

// Ripples, as a tiling normal map (linear data, green up): a few waves of whole periods across the square.
Texture ripples(Renderer2D& r, int n) {
    std::vector<u8> rgba(std::size_t(n * n) * 4);
    constexpr std::array<std::array<f32, 4>, 7> waves{{// cycles across x, across y, height, phase
                                                       {2, 1, 1.0f, 0.0f},
                                                       {-1, 3, 0.7f, 1.3f},
                                                       {4, -3, 0.35f, 2.1f},
                                                       {3, 7, 0.22f, 0.4f},
                                                       {-9, -4, 0.12f, 3.0f},
                                                       {11, 6, 0.08f, 5.2f},
                                                       {-6, 13, 0.06f, 1.7f}}};
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            f32 dx = 0, dy = 0;
            for (const auto& w : waves) {
                const f32 k = 2 * std::numbers::pi_v<f32> / f32(n);
                const f32 c = std::cos(k * (w[0] * f32(x) + w[1] * f32(y)) + w[3]) * w[2] * 0.06f;
                dx += c * w[0];
                dy += c * w[1];
            }
            const Vec3f normal = normalize(Vec3f{-dx, -dy, 1});
            u8* p = &rgba[std::size_t(y * n + x) * 4];
            p[0] = u8(std::lround((normal.x * 0.5f + 0.5f) * 255));
            p[1] = u8(std::lround((normal.y * 0.5f + 0.5f) * 255));
            p[2] = u8(std::lround((normal.z * 0.5f + 0.5f) * 255));
            p[3] = 255;
        }
    return r.create_texture_from_rgba(rgba.data(), {n, n});
}

// A mirror or still water: where it is, the view its picture is drawn in, its material, and its stand-in in other
// pictures (its picture is made for this frame's camera, not for theirs).
struct Mirror {
    Vec3f centre;
    Quatf facing; // its +Z out of its front
    Vec2f size;
    f32 distortion = 0;
    View3D view;
    Material3D material, stand_in;
    PlanarView3D seen;
    bool shown = false; // on screen this frame: its view rendered
};

// A portal: where it is, a view and a material a level of depth (portal_chain: level k's surface reads level k + 1's
// picture), its arch, and the stand-in for the deepest level and for other pictures.
struct Portal {
    PortalPose3D pose;
    std::vector<View3D> views;
    std::vector<Material3D> levels;
    Material3D stand_in, arch;
    std::vector<PlanarView3D> chain;
    u32 shown = 0; // levels rendered this frame
};

// Something moving through the portals: where it is and how fast (world units a second).
struct Mover {
    Vec3f position, velocity;
    Quatf facing{};
};

class Courtyard final : public Scene {
public:
    explicit Courtyard(Options o) : _o(std::move(o)), _camera(_o.camera) {}
    std::string_view name() const override { return "Planar views"; }

    void update(SceneContext& ctx) override {
        if (!_r)
            setup(ctx);
        const auto& in = ctx.input;
        if (in.pressed(Key::Escape))
            ctx.app.quit();
        const std::array keys{Key::Num0, Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5};
        for (int i = 0; i < int(keys.size()); ++i)
            if (in.pressed(keys[size_t(i)]))
                _camera = i;
        if (in.mouse_held(MouseButton::Left)) {
            const Vec2f d{in.mouse_pos().x - _last_mouse.x, in.mouse_pos().y - _last_mouse.y};
            if (_dragging) {
                _yaw -= d.x * 0.006f;
                _pitch = std::clamp(_pitch + d.y * 0.004f, 0.02f, 1.2f);
            }
            _dragging = true;
        } else
            _dragging = false;
        _last_mouse = in.mouse_pos();
        _distance = std::clamp(_distance * std::pow(0.9f, in.frame_mouse_wheel_y()), 4.0f, 30.0f);
        _time += ctx.dt;
        if (!_dragging && _camera == 0)
            _yaw += ctx.dt * 0.08f;
        move(_orb, ctx.dt);
        move(_walker, ctx.dt);
    }

    void render(SceneContext& ctx) override {
        if (!_r)
            return;
        const auto size = ctx.renderer.output_size();
        // The frame in the pictures' pixels: as sharp as the view (--scale 1).
        const Vec2i frame{std::max(1, int(f32(size.x) * _o.scale)), std::max(1, int(f32(size.y) * _o.scale))};
        if (!_view.valid() || size != _size)
            make_views(size);
        _size = size;
        const f32 aspect = f32(size.x) / f32(std::max(size.y, 1));

        // Which pictures this frame needs, from where: a mirror or a portal not on screen needs none, and a portal's
        // chain stops at the first level where the portal is out of the view before it.
        // Each picture is of where its surface shows only (planar_picture: crop_planar_view cuts its camera to that),
        // as sharp as the view, its view resized to it (in steps, so not every frame).
        const Camera3D eye = camera();
        const auto fit = [&](View3D& view, Material3D& material, const PlanarPicture3D& picture) {
            if (view.texture().size() != picture.size)
                (void)_r->resize(view, picture.size);
            material.emissive_texture = view.texture();
            return f32(picture.size.x) / f32(picture.size.y);
        };
        for (Mirror* m : {&_mirror, &_water}) {
            const auto bounds = planar_on_screen(eye, aspect, m->centre, m->facing, m->size);
            m->shown = _o.planar && bounds;
            if (m->shown) {
                const PlanarPicture3D picture = planar_picture(*bounds, frame);
                m->seen = mirror_view(eye, plane_through(m->centre, m->facing.rotate({0, 0, 1})), aspect);
                crop_planar_view(m->seen, picture.window, fit(m->view, m->material, picture), true);
                planar_view_params(m->material.shader_params, m->seen.projector, m->distortion);
            }
        }
        for (u32 i = 0; i < 2; ++i) {
            Portal& p = _portals[i];
            const PortalPose3D& exit = _portals[1 - i].pose;
            p.chain = portal_chain(eye, p.pose, exit, aspect, u32(p.views.size()));
            p.shown = 0;
            const Mat4f through = Mat4f::translation(exit.position) * Mat4f::rotation(portal_rotation(p.pose, exit)) *
                                  Mat4f::translation(-p.pose.position);
            // Level by level, each cut to where the portal shows within the level before's picture.
            Camera3D from = eye;
            Rectf within{0, 0, 1, 1};
            while (_o.planar && p.shown < p.chain.size()) {
                const auto bounds = planar_on_screen(from, aspect, p.pose.position, p.pose.rotation, p.pose.size);
                if (!bounds)
                    break;
                const f32 x0 = std::max(bounds->x, within.x), y0 = std::max(bounds->y, within.y);
                const f32 x1 = std::min(bounds->x + bounds->w, within.x + within.w);
                const f32 y1 = std::min(bounds->y + bounds->h, within.y + within.h);
                if (!(x0 < x1 && y0 < y1))
                    break;
                within = {x0, y0, x1 - x0, y1 - y0};
                const PlanarPicture3D picture = planar_picture(within, frame);
                PlanarView3D& level = p.chain[p.shown];
                from = level.camera;
                crop_planar_view(level, picture.window, fit(p.views[p.shown], p.levels[p.shown], picture), false,
                                 through);
                planar_view_params(p.levels[p.shown].shader_params, level.projector);
                ++p.shown;
            }
        }
        _water.material.uv_offset = _water.stand_in.uv_offset = {_time * 0.02f, _time * 0.013f};

        // The pictures first, deepest first, with stand-ins for what they cannot show; then the courtyard.
        const bool counted = _o.bench > 0 && _frame >= 30;
        u32 rendered = 0;
        const auto picture = [&](View3D& view, const Camera3D& camera, const char* name) {
            _scene.camera = camera;
            const auto scope = ctx.renderer.gpu_scope(name);
            const bool ok = bool(_r->render(view, _scene));
            if (counted)
                add_cpu(name, _r->stats().cpu_ms);
            ++rendered;
            return ok;
        };
        const std::array<const Material3D*, 2> stand_in_portals{&_portals[0].stand_in, &_portals[1].stand_in};
        compose(_mirror.stand_in, _water.stand_in, stand_in_portals);
        // The probes, of the scene with the planar surfaces as their stand-ins: the courtyard's once, the ball's every
        // frame (six renders at 64 pixels a face), each seeing the other as last captured.
        if (_o.probes) {
            const auto scope = ctx.renderer.gpu_scope("probes");
            if (!_court_probe)
                _court_probe = _r->create_reflection_capture(128).value_or({});
            if (!_ball_probe)
                _ball_probe = _r->create_reflection_capture(64).value_or({});
            if (_court_probe && !_court_captured)
                _court_captured = bool(_r->capture_reflection(_court_probe, _scene, {0, 1.5f, 0}));
            if (_ball_probe)
                _ball_captured = bool(_r->capture_reflection(_ball_probe, _scene, ball_centre, {.hidden_id = ball_id}));
            compose(_mirror.stand_in, _water.stand_in, stand_in_portals); // (now with the probes placed)
        }
        for (Mirror* m : {&_mirror, &_water})
            if (m->shown && !picture(m->view, m->seen.camera, m == &_mirror ? "mirror" : "water"))
                return fail(ctx, "a mirror's render failed");
        for (u32 i = 0; i < 2; ++i) {
            Portal& p = _portals[i];
            for (u32 k = p.shown; k-- > 0;) {
                auto portals = stand_in_portals;
                portals[i] = k + 1 < p.shown ? &p.levels[k + 1] : &p.stand_in;
                compose(_mirror.stand_in, _water.stand_in, portals);
                if (!picture(p.views[k], p.chain[k].camera, i == 0 ? "portal_a" : "portal_b"))
                    return fail(ctx, "a portal's render failed");
            }
        }
        compose(_mirror.shown ? _mirror.material : _mirror.stand_in, _water.shown ? _water.material : _water.stand_in,
                {_portals[0].shown ? &_portals[0].levels[0] : &_portals[0].stand_in,
                 _portals[1].shown ? &_portals[1].levels[0] : &_portals[1].stand_in});
        _scene.camera = eye;
        {
            const auto scope = ctx.renderer.gpu_scope("main");
            if (!_r->render(_view, _scene))
                return fail(ctx, "the 3D render failed");
        }
        if (counted) {
            add_cpu("main", _r->stats().cpu_ms);
            _views_rendered += rendered;
        }
        ctx.renderer.clear(Color::rgb(0, 0, 0));
        ctx.renderer.draw_texture(_view.texture(), {0, 0, f32(size.x), f32(size.y)});

        if (!_o.shot.empty() && ++_frame == _o.frames) {
            Image image;
            if (!ctx.renderer.read_rgba({0, 0, f32(size.x), f32(size.y)}, image.rgba, image.size) ||
                !save_image(image, _o.shot))
                throw std::runtime_error("cannot write " + _o.shot.string());
            std::cout << "planar_views: wrote " << _o.shot.string() << " (" << rendered << " planar views)\n";
            ctx.app.quit();
        }
        if (_o.bench > 0 && ++_frame == _o.bench) {
            const int n = _o.bench - 30;
            std::cout << "planar_views: " << size.x << " x " << size.y << ", pictures at " << _o.scale
                      << " of its sharpness, " << f64(_views_rendered) / n << " of them a frame"
                      << (_o.planar ? "" : " (off)") << ", CPU ms a render over " << n << " frames (GPU: --profile):\n";
            for (const auto& [name, t] : _timing)
                std::cout << "  " << name << ": " << (t.cpu_n ? t.cpu / t.cpu_n : 0.0) << '\n';
            ctx.app.quit();
        }
    }

private:
    static constexpr f32 water_height = 0.3f;
    static constexpr Vec3f basin{1, 0, 1.5f};

    void add_cpu(const std::string& name, f64 ms) {
        _timing[name].cpu += ms;
        ++_timing[name].cpu_n;
    }

    void fail(SceneContext& ctx, const char* why) {
        if (ctx.report)
            ctx.report->fail(why);
        ctx.app.quit();
    }

    // A step of `m`'s flight: through a portal when it crosses one (the rest of the step from the other).
    void move(Mover& m, f32 dt) {
        const Vec3f to = m.position + m.velocity * dt;
        for (u32 i = 0; i < 2; ++i) {
            const PortalPose3D &in = _portals[i].pose, &out = _portals[1 - i].pose;
            if (const auto t = portal_crossing(in, m.position, to)) {
                const Quatf turn = portal_rotation(in, out);
                m.position = portal_point(in, out, m.position + (to - m.position) * *t) +
                             turn.rotate(to - m.position) * (1 - *t);
                m.velocity = turn.rotate(m.velocity);
                m.facing = (turn * m.facing).normalized();
                return;
            }
        }
        m.position = to;
    }

    Camera3D camera() const {
        switch (_camera) {
        case 1:
            return Camera3D::look_at({3, 2.2f, 1.5f}, {-0.5f, 1.6f, -8}, 55);
        case 2:
            return Camera3D::look_at({-3.5f, 1.6f, 4.5f}, {0, 0.2f, -1}, 55);
        case 3:
            return Camera3D::look_at({-2, 1.6f, 3.5f}, _portals[0].pose.position, 55);
        case 4:
            return Camera3D::look_at({5, 1.7f, -1.5f}, _portals[1].pose.position, 55);
        case 5: { // walking through the portals, round and round
            Camera3D c;
            c.position = _walker.position;
            c.rotation = _walker.facing;
            c.fov = 60;
            return c;
        }
        default: {
            const Vec3f target{0, 1, -1};
            return Camera3D::look_at(
                target + Vec3f{std::cos(_pitch) * std::sin(_yaw), std::sin(_pitch), std::cos(_pitch) * std::cos(_yaw)} *
                             _distance,
                target, 55);
        }
        }
    }

    // The view, and the pictures' (sized each frame to what they show; the view's samples).
    void make_views(Vec2i size) {
        if (_view.valid()) {
            (void)_r->resize(_view, size);
            return;
        }
        _view = _r->create_view(size, {.samples = 4}).value_or({});
        for (Mirror* m : {&_mirror, &_water})
            m->view = _r->create_view({64, 64}, {.samples = 4}).value_or({});
        for (Portal& p : _portals)
            for (u32 k = 0; k < _o.depth; ++k)
                p.views.push_back(_r->create_view({64, 64}, {.samples = 4}).value_or({}));
    }

    void setup(SceneContext& ctx) {
        _r = ctx.renderer3d;
        if (!_r) { // (a headless run: no shared 3D renderer)
            _own = std::make_unique<Renderer3D>(ctx.renderer);
            _r = _own.get();
        }
        if (!_r->available())
            throw std::runtime_error("needs the GPU 3D renderer (for --shot: KIN_RENDER_BACKEND=gpu)");
        if (_o.bench > 0)
            ctx.renderer.set_gpu_timing_enabled(true);
        const auto mesh = [&](const MeshData3D& data) {
            auto m = _r->create_mesh(data);
            if (!m)
                throw std::runtime_error("cannot make a mesh");
            return *m;
        };
        _box = mesh(meshes::box({1, 1, 1}));
        _sphere = mesh(meshes::sphere(0.5f, 32, 16));
        _torus = mesh(meshes::torus(0.6f, 0.18f, 48, 16));
        _ground = mesh(meshes::plane({30, 30}, 8));
        _square = mesh(meshes::plane({1, 1}));
        // kin's planar surfaces, built into this program by kin_planar_surfaces (examples/CMakeLists.txt).
        const auto surfaces = create_planar_surfaces(*_r, planar_surfaces);
        if (!surfaces)
            throw std::runtime_error("cannot make the planar surfaces: " + diagnostics3d_text(surfaces.diagnostics));
        const SurfaceShader3D reflection = surfaces->reflection, window = surfaces->window;

        // A silver mirror on the north wall and a basin of dark water (a dielectric: little head on, much at a glance).
        // Their emissive maps are their pictures (`emissive` 0: nothing of their own). In other pictures: kin's own
        // reflections.
        _mirror = {.centre = {0, 2, -7.9f}, .facing = {}, .size = {4, 2.4f}};
        _mirror.material = {.color = Color::rgb(235, 235, 240), .metallic = 1, .roughness = 0.045f};
        _mirror.stand_in = _mirror.material;
        _mirror.material.surface = reflection;
        _water = {.centre = basin + Vec3f{0, water_height, 0},
                  .facing = Quatf::axis_angle({1, 0, 0}, -90), // +Z turned up
                  .size = {4, 4},
                  .distortion = 0.25f};
        _water.material = {.color = Color::rgb(10, 28, 34), .casts_shadow = false, .roughness = 0.06f};
        _water.material.normal = ripples(ctx.renderer, 256);
        _water.material.normal_scale = 0.5f;
        _water.material.wrap_u = _water.material.wrap_v = Wrap3D::Repeat;
        _water.material.uv_scale = {1.5f, 1.5f};
        _water.stand_in = _water.material;
        _water.material.surface = reflection;
        // Two portals facing each other across the courtyard, on the west and east walls: unlit windows, a level of
        // depth each, drawn as the box portal_surface_transform places (double-sided) so a camera can step into them.
        const PortalPose3D west{{-8.9f, 1.4f, 1}, Quatf::axis_angle({0, 1, 0}, 90), {1.8f, 2.8f}}; // faces +X
        const PortalPose3D east{{8.9f, 1.4f, 1}, Quatf::axis_angle({0, 1, 0}, -90), {1.8f, 2.8f}}; // faces -X
        const std::array<Color, 2> arches{Color::rgb(40, 170, 160), Color::rgb(200, 60, 150)};
        // (The stand-in is dim: a portal in a mirror or the water reads as a dark doorway, not a light.)
        const std::array<Color, 2> films{Color::rgb(24, 52, 50), Color::rgb(56, 28, 46)};
        for (u32 i = 0; i < 2; ++i) {
            Portal& p = _portals[i];
            p.pose = i == 0 ? west : east;
            Material3D level{.color = colors::white, .unlit = true, .double_sided = true, .casts_shadow = false};
            level.surface = window;
            p.levels.assign(_o.depth, level);
            p.stand_in = {.color = films[i], .unlit = true, .double_sided = true, .casts_shadow = false};
            p.arch = {.color = arches[i], .emissive = 0.6f, .roughness = 0.4f};
        }
        // An orb flying through them, and the walking camera's path, both west and round.
        _orb = {{3, 1.0f, 0.3f}, {-3, 0, 0}};
        _walker = {{-3, 1.6f, 0.6f}, {-1.4f, 0, 0}, Quatf::look_rotation({-1, 0, 0})};

        _scene.sky = {.enabled = true, .turbidity = 2.5f, .sun_intensity = 1.0f};
        _scene.lights.sky_lights_scene = true;
        apply({.hours = 16.0f, .day_of_year = 200, .latitude = 45}, _scene.lights, _scene.sky, _scene.fog);
        _scene.lights.shadow_area = {{-12, -1, -12}, {12, 6, 12}};
        _scene.lights.shadow_resolution = 2048;
    }

    // The courtyard: a stone ground, three walls, the mirror in its frame, the basin and its water, the portals'
    // arches and surfaces, and things to see in them; the planar surfaces drawn with the materials given.
    void compose(const Material3D& mirror, const Material3D& water, std::array<const Material3D*, 2> portals) {
        _scene.clear();
        _scene.ssr.enabled = _o.ssr;
        const auto draw = [&](const Mesh3D& m, const Mat4f& at, const Material3D& material) {
            (void)_scene.draw(m, at, material);
        };
        const auto box = [&](Vec3f centre, Vec3f size, const Material3D& material) {
            draw(_box, Mat4f::trs(centre, {}, size), material);
        };
        const Material3D stone{.color = Color::rgb(150, 142, 128), .roughness = 0.85f};
        const Material3D wall{.color = Color::rgb(186, 170, 150), .roughness = 0.9f};
        const Material3D frame{.color = Color::rgb(120, 86, 40), .metallic = 0.8f, .roughness = 0.35f};
        draw(_ground, Mat4f::identity(), stone);
        box({0, 2, -8.25f}, {19, 4, 0.5f}, wall);
        box({-9.5f, 2, 0}, {0.5f, 4, 17}, wall);
        box({9.5f, 2, 0}, {0.5f, 4, 17}, wall);
        // The mirror: a thin silver sheet just in front of its frame (a square turned to face its +Z).
        box({_mirror.centre.x, _mirror.centre.y, -8.0f + 0.01f}, {4.6f, 3.0f, 0.1f}, frame);
        draw(_square, Mat4f::trs(_mirror.centre, Quatf::axis_angle({1, 0, 0}, 90), {_mirror.size.x, 1, _mirror.size.y}),
             mirror);
        // The basin: a rim round a sunken pool, the water just under its top.
        const Material3D rim{.color = Color::rgb(200, 196, 188), .roughness = 0.6f};
        box(basin + Vec3f{0, 0.2f, -2.15f}, {4.6f, 0.4f, 0.3f}, rim);
        box(basin + Vec3f{0, 0.2f, 2.15f}, {4.6f, 0.4f, 0.3f}, rim);
        box(basin + Vec3f{-2.15f, 0.2f, 0}, {0.3f, 0.4f, 4.0f}, rim);
        box(basin + Vec3f{2.15f, 0.2f, 0}, {0.3f, 0.4f, 4.0f}, rim);
        draw(_square, Mat4f::trs(_water.centre, {}, {_water.size.x, 1, _water.size.y}), water);
        // The arches stand wholly behind their surface, so that the view out of a portal (clipped at its surface)
        // does not see its own.
        for (u32 i = 0; i < 2; ++i) {
            const Portal& p = _portals[i];
            const Mat4f place = Mat4f::trs(p.pose.position, p.pose.rotation);
            draw(_box, place * Mat4f::trs({-1.0f, 0, -0.16f}, {}, {0.2f, 2.8f, 0.3f}), p.arch);
            draw(_box, place * Mat4f::trs({1.0f, 0, -0.16f}, {}, {0.2f, 2.8f, 0.3f}), p.arch);
            draw(_box, place * Mat4f::trs({0, 1.5f, -0.16f}, {}, {2.2f, 0.2f, 0.3f}), p.arch);
            draw(_box, portal_surface_transform(p.pose, 0.3f), *portals[i]);
        }
        // Things to see.
        const std::array<Color, 5> colours{Color::rgb(220, 60, 40), Color::rgb(240, 190, 40), Color::rgb(60, 160, 90),
                                           Color::rgb(50, 110, 220), Color::rgb(170, 70, 200)};
        for (int i = 0; i < 5; ++i) {
            const f32 a = f32(i) * 1.2566f + 0.4f;
            const Vec3f at{4.5f + 2.2f * std::cos(a), 0.5f, 4.5f + 2.2f * std::sin(a)};
            box(at, {0.8f, 1.0f + f32(i) * 0.4f, 0.8f}, {.color = colours[size_t(i)], .roughness = 0.5f});
        }
        // The chrome ball: with its own probe, a curved mirror (it is left out of its own picture by its id).
        (void)_scene.draw(_sphere, Mat4f::trs(ball_centre, {}, {1, 1, 1}),
                          {.color = Color::rgb(230, 230, 235), .metallic = 1, .roughness = 0.05f}, colors::white,
                          ball_id);
        draw(_torus,
             Mat4f::trs(basin + Vec3f{0, 1.6f, 0},
                        Quatf::axis_angle({0, 1, 0}, _time * 30) * Quatf::axis_angle({1, 0, 0}, 70)),
             {.color = Color::rgb(240, 170, 40), .metallic = 1, .roughness = 0.25f});
        box({6, 0.75f, -4}, {1, 1.5f, 1}, {.color = Color::rgb(230, 230, 60), .roughness = 0.4f});
        box({-5.5f, 0.4f, -2}, {0.8f, 0.8f, 0.8f}, {.color = Color::rgb(70, 200, 230), .roughness = 0.4f});
        // The orb, glowing; half-way through a portal, at the exit too (portal_straddle: what has gone through shows
        // beyond it, what has not still shows before it), and neither cut by the pictures' clip planes (DrawClip3D:
        // the image whole; the orb again, whole, in the pictures only), or the halves would show a rim where they meet.
        const Mat4f orb = Mat4f::trs(_orb.position, {}, {0.4f, 0.4f, 0.4f});
        const Material3D glow{.color = Color::rgb(255, 120, 40), .emissive = 1.5f};
        draw(_sphere, orb, glow);
        for (u32 i = 0; i < 2; ++i)
            if (const auto through = portal_straddle(_portals[i].pose, _portals[1 - i].pose, _orb.position, 0.2f)) {
                const MeshInstance3D image{.transform = *through * orb}, again{.transform = orb};
                (void)_scene.draw_instanced(_sphere, std::span(&image, 1), glow, DrawClip3D::whole);
                (void)_scene.draw_instanced(_sphere, std::span(&again, 1), glow, DrawClip3D::whole_if_cut);
            }
        place_probes();
    }

    // The probes into the scene's lights (compose's last step; a capture not yet made is left out).
    void place_probes() {
        if (_court_captured)
            _scene.lights.reflection_probes.push_back({.position = {0, 1.5f, 0},
                                                       .box_offset = {0, 0.5f, 0},
                                                       .extent = {9.25f, 2.5f, 8.5f},
                                                       .blend = 1.0f,
                                                       .parallax_extent = {9.25f, 2.0f, 8.0f},
                                                       .capture = _court_probe});
        if (_ball_captured)
            _scene.lights.reflection_probes.push_back({.position = ball_centre,
                                                       .extent = {0.55f, 0.55f, 0.55f},
                                                       .blend = 0.0f,
                                                       .parallax = false,
                                                       .priority = 1,
                                                       .capture = _ball_probe});
    }

    Options _o;
    Renderer3D* _r = nullptr;
    std::unique_ptr<Renderer3D> _own;
    View3D _view;
    Vec2i _size{};
    Scene3D _scene;
    Mesh3D _box, _sphere, _torus, _ground, _square;
    Mirror _mirror, _water;
    std::array<Portal, 2> _portals;
    Mover _orb, _walker;
    // The reflection probes (--probes): the courtyard's, captured once (its parallax box its walls, floor and the top
    // of the walls), and the chrome ball's own, captured every frame from its centre without the ball, read as if
    // infinitely far: a curved mirror.
    static constexpr Vec3f ball_centre{-2.5f, 0.5f, -3};
    static constexpr u32 ball_id = 4242;
    ReflectionCapture3D _court_probe, _ball_probe;
    bool _court_captured = false, _ball_captured = false;
    int _camera = 0, _frame = 0;
    u64 _views_rendered = 0;
    std::map<std::string, Timing> _timing;
    bool _dragging = false;
    f32 _time = 0, _yaw = 0.6f, _pitch = 0.35f, _distance = 14;
    Vec2f _last_mouse{};
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
            if (a == "--shot")
                o.shot = value();
            else if (a == "--camera")
                o.camera = std::stoi(value());
            else if (a == "--frames")
                o.frames = std::stoi(value());
            else if (a == "--scale")
                o.scale = std::clamp(std::stof(value()), 0.1f, 1.0f);
            else if (a == "--bench")
                o.bench = std::max(31, std::stoi(value()));
            else if (a == "--planar")
                o.planar = value() != "off";
            else if (a == "--probes")
                o.probes = value() != "off";
            else if (a == "--ssr")
                o.ssr = value() != "off";
            else if (a == "--depth")
                o.depth = u32(std::clamp(std::stoi(value()), 1, 8));
            else
                runtime.push_back(argv[i]);
        }
        auto headless = parse_headless_options(int(runtime.size()), runtime.data());
        // (--bench runs in a window: GPU timing needs frames that are presented.)
        if (!o.shot.empty()) {
            headless.enabled = true;
            headless.render = true; // on the GPU
            headless.frames = o.frames + 1;
        }
        SceneManager scenes;
        scenes.push(std::make_unique<Courtyard>(o));
        return run_scene_app({.window = {.title = "Planar views", .width = 1280, .height = 720, .max_fps = 60},
                              .headless = headless,
                              .render_headless = true,
                              .rendering_3d = true},
                             scenes);
    } catch (const std::exception& e) {
        std::cerr << "planar_views: " << e.what() << '\n';
        return 1;
    }
}
