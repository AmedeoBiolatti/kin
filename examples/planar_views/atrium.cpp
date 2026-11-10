// planar_atrium: an authored scene (content/scenes/atrium.kinscene) with mirrors and portals as entities (Mirror3D,
// Portal3D), run by a SceneSession3D, which renders their pictures; its materials are material graphs (marble,
// brushed brass with verdigris, a checkered marble floor, terrazzo, sandstone, glowing portal trim) and its motion a
// Lua script (content/scripts/atrium.lua: an orb flying through the portals by scene3d.portal_step).
//
//   planar_atrium [--camera N] [--shot picture.png] [--frames N] [--content folder] [--bake-probes]
//                 [--autoplay SECONDS [--autoplay-report report.json]]
//
// --camera: 6 (the default) plays: the survivor (an animated character, a physics capsule) is yours, seen from behind
// (WASD or the arrows walk, the mouse turns the camera, Space jumps, Shift runs, P changes the portals' effect: off,
// ripples, vortex, glitch, membrane, each answering what goes through; it walks through the portals, carried
// by physics, and the camera follows it; Tab frees the mouse). 0 orbits the
// atrium, 1 looks at the mirror, 2 at the pool, 3 into the west portal, 4 walks through the portals, 5 watches the
// survivor walk its round into the west portal (the keys 0-6 too). --shot renders
// headless and writes a picture after --frames frames (with KIN_RENDER_BACKEND=gpu). --bake-probes captures the scene's
// reflection probes after a few frames and writes them to the content's probes/ folder (Radiance .hdr panoramas; name
// them in a probe's "baked" to load them instead of capturing at run time), then quits. In a window it opens on a
// loading screen (what loads, how far along, the controls), which fades out over the scene's first frames; its material
// graphs are cooked at build time (else each is compiled with glslc as it starts, about 2 s). --autoplay plays mode 6
// headless for SECONDS through the real input (a bot pressing W, Space and Shift and moving the mouse: laps of the
// atrium, through both portals, a jump through one, into the open south edge) and reports what a player would meet:
// falls, escapes from the room, getting stuck, missed portals, the frame times (CPU and GPU, after a warm-up),
// pipelines made late, the memory every 10 seconds; it fails (exit 2) on any of the first four, and on memory still
// growing over the second half of a run of 4 minutes or more. Esc: quit.
#include "planar_surfaces.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <kin/assets/image.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/render3d.hpp>
#include <kin/platform/window.hpp>
#include <kin/render3d/assets3d.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/planar_views3d.hpp>
#include <kin/scene/reflection_probes3d.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <kin/ui2/text.hpp>
#include <malloc.h>
#include <memory>
#include <sol/sol.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace kin;

struct Options {
    std::filesystem::path content = ATRIUM_CONTENT, shot;
    int camera = 6;
    int frames = 90;
    bool bake = false;
    f32 autoplay = 0; // seconds
    std::filesystem::path autoplay_report;
};

// The memory the process holds (resident), in MiB (Linux; 0 elsewhere).
f64 resident_mib() {
    std::ifstream statm("/proc/self/statm");
    u64 pages = 0, resident = 0;
    if (!(statm >> pages >> resident))
        return 0.0;
    return f64(resident) * 4096.0 / (1024.0 * 1024.0);
}

f64 percentile(std::vector<f64> values, f64 p) {
    if (values.empty())
        return 0.0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(p * f64(values.size() - 1) + 0.5))];
}

// --autoplay: plays the survivor as a player would, by the keys and the mouse (Input's test hooks: what the window's
// events would set), and watches what happens. Its legs: walk to a point; go through a portal (to a point past it:
// done when physics carries the survivor through); walk into the open south edge (done when it stops there, still on
// the floor). Laps alternate walking and running; one portal is taken jumping, and jumps come every few seconds.
class AtriumBot {
public:
    enum class Leg : u8 { Walk, Portal, Edge };
    struct Step {
        Leg leg;
        f32 x, z;
    };
    struct Finding {
        f32 time;
        std::string what;
    };

    void step(Input& input, kin::EcsEntity survivor, kin::EcsEntity camera, f32 dt) {
        _time += dt;
        const auto* controller = survivor ? survivor.get<CharacterController3D>() : nullptr;
        const auto* view = camera ? camera.get<Transform3D>() : nullptr;
        if (!controller || !view)
            return;
        const Vec3f p = controller->state.position;
        // (Not the first second: the capsule starts at the origin and goes to its place.)
        const bool through = _has_last && _time > 1.0f && std::hypot(p.x - _last.x, p.z - _last.z) > 2.0f;
        _last = p, _has_last = true;
        if (through)
            ++crossings;

        // What a player would notice.
        if (p.y < -0.5f && !_fell) {
            _fell = true;
            note("fell off the floor at " + where(p));
        }
        const bool outside = std::fabs(p.x) > 11.5f || p.z < -10.0f || p.z > 11.0f;
        if (outside && !_escaped) {
            _escaped = true;
            note("left the atrium at " + where(p));
        }

        const Step goal = route[_leg];
        const Vec2f to{goal.x - p.x, goal.z - p.z};
        const f32 distance = std::hypot(to.x, to.y);
        bool next = false;
        if (goal.leg == Leg::Portal && through) {
            ++portals;
            next = true;
        } else if (goal.leg == Leg::Walk && distance < 0.6f) {
            next = true;
        } else if (goal.leg == Leg::Portal && through == false && _stalled > 4.0f) {
            note("missed the portal toward " + where({goal.x, 0, goal.z}) + ", stopped at " + where(p));
            next = true;
        } else if (goal.leg == Leg::Edge && _stalled > 1.5f) {
            ++edges;
            next = true;
        } else if (goal.leg == Leg::Walk && _stalled > 4.0f) {
            note("stuck at " + where(p) + " walking to " + where({goal.x, 0, goal.z}));
            next = true;
        }
        // Progress: the closest it came to the goal; stalled while that does not improve.
        if (distance < _closest - 0.25f)
            _closest = distance, _stalled = 0.0f;
        else
            _stalled += dt;
        if (next) {
            _leg = (_leg + 1) % route.size();
            if (_leg == 0)
                ++laps;
            _closest = 1e9f, _stalled = 0.0f;
        }

        // The camera toward the goal (the mouse: the script turns its yaw by 0.15 degrees a pixel, left for +x), and
        // walking while it looks about the right way.
        const Vec3f forward = view->rotation.rotate({0, 0, -1});
        const f32 have = std::atan2(forward.x, forward.z), want = std::atan2(to.x, to.y);
        f32 turn = (want - have) * 57.29578f;
        turn = std::remainder(turn, 360.0f);
        input.set_mouse_motion({std::clamp(-0.6f * turn / 0.15f, -50.0f, 50.0f), 0.0f});
        hold(input, Key::W, std::fabs(turn) < 50.0f);
        hold(input, Key::LeftShift, laps % 2 == 1);
        // A jump every 5 seconds, and over the threshold of the first portal (jumping through it).
        const bool jump = std::fmod(_time, 5.0f) < 0.1f || (_leg == 3 && laps % 2 == 0 && distance < 4.5f);
        hold(input, Key::Space, jump);
    }

    void note(std::string what) { findings.push_back({_time, std::move(what)}); }

    // A lap: west lane, the west portal (out of the east one), the north lane, round, the east portal (out of the
    // west one), the south lane, the open edge.
    static constexpr std::array route{
        Step{Leg::Walk, 8.0f, 8.5f},     Step{Leg::Walk, -8.0f, 8.5f}, Step{Leg::Walk, -8.0f, 2.0f},
        Step{Leg::Portal, -13.0f, 2.0f}, Step{Leg::Walk, 8.0f, 2.0f},  Step{Leg::Walk, 8.0f, -7.0f},
        Step{Leg::Walk, -8.0f, -7.0f},   Step{Leg::Walk, -8.0f, 2.0f}, Step{Leg::Walk, -8.0f, 8.5f},
        Step{Leg::Walk, 8.0f, 8.5f},     Step{Leg::Walk, 8.0f, 2.0f},  Step{Leg::Portal, 13.0f, 2.0f},
        Step{Leg::Walk, -8.0f, 2.0f},    Step{Leg::Walk, -8.0f, 8.5f}, Step{Leg::Walk, 0.0f, 8.5f},
        Step{Leg::Edge, 0.0f, 13.0f}};
    int laps = 0, crossings = 0, portals = 0, edges = 0;
    std::vector<Finding> findings;

private:
    static std::string where(Vec3f p) {
        char text[64];
        std::snprintf(text, sizeof text, "(%.2f, %.2f, %.2f)", p.x, p.y, p.z);
        return text;
    }
    void hold(Input& input, Key key, bool down) {
        const size_t i = key == Key::W ? 0 : key == Key::LeftShift ? 1 : 2;
        if (down != _held[i])
            down ? input.set_key_pressed(key) : input.set_key_released(key);
        _held[i] = down;
    }

    size_t _leg = 0;
    f32 _time = 0, _closest = 1e9f, _stalled = 0;
    Vec3f _last{};
    bool _has_last = false, _fell = false, _escaped = false;
    std::array<bool, 3> _held{};
};

class Atrium final : public Scene {
public:
    explicit Atrium(Options o) : _o(std::move(o)), _camera(_o.camera) {}
    std::string_view name() const override { return "Atrium"; }

    void on_enter(SceneContext& ctx) override {
        _renderer = ctx.renderer3d;
        if (!_renderer) {
            _own = std::make_unique<Renderer3D>(ctx.renderer);
            _renderer = _own.get();
        }
        if (!_renderer->available())
            throw std::runtime_error("needs the GPU 3D renderer (for --shot: KIN_RENDER_BACKEND=gpu)");
        _assets = std::make_unique<Assets3D>(*_renderer, _o.content);
        // The survivor's controls (atrium.lua reads the actions).
        const std::vector<std::pair<const char*, std::vector<Key>>> controls{
            {"forward", {Key::W, Key::Up}},  {"back", {Key::S, Key::Down}}, {"left", {Key::A, Key::Left}},
            {"right", {Key::D, Key::Right}}, {"jump", {Key::Space}},        {"sprint", {Key::LeftShift}}};
        for (const auto& [action, keys] : controls)
            for (const Key key : keys)
                ctx.input.bind(action, key);
        // What loads, a step a frame behind the loading screen (in a window): the planar surfaces, each material graph
        // (cooked at build time: a moment; else compiled with glslc, about 2 s each), each model, then the scene (which
        // finds them all loaded). Headless (a shot, a bake, autoplay) it all loads now.
        _steps.push_back({"Mirrors and portals", [this] {
                              const auto surfaces = create_planar_surfaces(*_renderer, planar_surfaces);
                              require(surfaces.diagnostics);
                              _surfaces = *surfaces;
                          }});
        const auto files = [&](const char* folder, const char* extension) {
            std::vector<std::filesystem::path> found;
            std::error_code error;
            for (auto it = std::filesystem::recursive_directory_iterator(_o.content / folder, error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
                if (it->path().extension() == extension)
                    found.push_back(std::filesystem::relative(it->path(), _o.content));
            std::sort(found.begin(), found.end());
            return found;
        };
        for (const auto& graph : files("materials", ".kingraph"))
            _steps.push_back({"Material: " + graph.stem().string(), [this, graph] {
                                  // (Held until the scene has loaded: the cache keeps only what is held.)
                                  if (auto made = _assets->load_material_graph(graph))
                                      _graphs.push_back(std::move(*made));
                              }});
        for (const auto& model : files("models", ".glb"))
            _steps.push_back({"Model: " + model.stem().string(), [this, model] {
                                  if (auto made = _assets->load_model(model))
                                      _models.push_back(std::move(*made));
                              }});
        _steps.push_back({"The atrium", [this, &ctx] { open_scene(ctx); }});
        _windowed = _o.shot.empty() && !_o.bake && _o.autoplay <= 0;
        if (!_windowed)
            while (_step < _steps.size())
                _steps[_step++].second();
    }

    // The scene, its physics and its script, once what it uses is loaded.
    void open_scene(SceneContext& ctx) {
        SceneSession3DOptions options;
        options.viewport = ctx.renderer.output_size();
        options.planar_surfaces = _surfaces;
        options.physics = World3DConfig{}; // (the survivor's capsule, the atrium's colliders)
        options.scripts.app = &ctx.app;
        options.scripts.input = &ctx.input;
        options.scripts.bind = [this](sol::state& lua) {
            _lua = &lua;
            lua["atrium_camera"] = _camera;
            lua["atrium_look_x"] = 0.0f;
            lua["atrium_look_y"] = 0.0f;
        };
        _session = std::make_unique<SceneSession3D>(*_renderer, *_assets, options);
        if (!_session->load("scenes/atrium.kinscene"))
            require(_session->diagnostics());
        _graphs.clear();
        _models.clear();
        if (_o.autoplay > 0)
            _bot = std::make_unique<AtriumBot>();
    }

    // P: the portals' next effect (Portal3D::effect), on both: none, ripples, vortex, glitch, membrane.
    void next_effect() {
        _effect = (_effect + 1) % 5;
        _session->world()->each([&](flecs::entity, Portal3D& portal) { portal.effect = Portal3D::Effect(_effect); });
        _effect_shown = 2.0f;
    }

    // The loading screen: the atrium's name, what loads, how far along, the controls; `opacity` as it fades out over
    // the scene's first frames.
    void loading_screen(SceneContext& ctx, Vec2i size, f32 opacity) {
        auto& r = ctx.renderer;
        const f32 w = f32(size.x), h = f32(size.y);
        const auto a = [&](u8 alpha) { return u8(std::clamp(opacity, 0.0f, 1.0f) * f32(alpha)); };
        r.fill_rect({0, 0, w, h}, Color::rgba(12, 18, 20, a(255)));
        // A portal's glow round the name.
        const f32 bw = std::min(w * 0.42f, 520.0f), bh = 150, bx = w / 2 - bw / 2, by = h / 2 - 150;
        for (int i = 0; i < 6; ++i) {
            const f32 g = f32(6 - i) * 3;
            r.fill_rect({bx - g, by - g, bw + 2 * g, bh + 2 * g}, Color::rgba(60, 220, 210, a(u8(10 + 4 * i))));
        }
        r.fill_rect({bx, by, bw, bh}, Color::rgba(14, 26, 28, a(255)));
        ui2::draw_text_centered(r, "ATRIUM", {w / 2, by + bh / 2 - 12}, 5, Color::rgba(225, 245, 242, a(255)));
        ui2::draw_text_centered(r, "mirrors   water   portals", {w / 2, by + bh / 2 + 34}, 2,
                                Color::rgba(120, 200, 195, a(255)));
        // How far along: the steps done, the one under way named.
        const std::size_t total = _steps.size() + 1; // (and the first frames)
        const std::size_t done = std::min(_step, _steps.size()) + (_session && _shown >= warm_frames ? 1 : 0);
        const std::string what = _step < _steps.size() ? _steps[_step].first : "Light and reflections";
        const f32 pw = std::min(w * 0.5f, 600.0f), px = w / 2 - pw / 2, py = by + bh + 70;
        ui2::draw_text_centered(r, what, {w / 2, py - 22}, 2, Color::rgba(200, 210, 212, a(255)));
        r.fill_rect({px, py, pw, 8}, Color::rgba(36, 48, 52, a(255)));
        r.fill_rect({px, py, pw * f32(done) / f32(total), 8}, Color::rgba(60, 220, 210, a(255)));
        ui2::draw_text_centered(
            r, "WASD walk   mouse look   Space jump   Shift run   P portal effect   Tab free mouse   Esc quit",
            {w / 2, h - 40}, 2, Color::rgba(130, 150, 152, a(255)));
    }

    void update(SceneContext& ctx) override {
        if (ctx.input.pressed(Key::Escape))
            ctx.app.quit();
        if (_step < _steps.size()) {
            // Loading: steps for up to 30 ms a frame, so that the window keeps answering and the screen moves.
            const auto start = std::chrono::steady_clock::now();
            do
                _steps[_step++].second();
            while (_step < _steps.size() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(30));
            return;
        }
        const std::array keys{Key::Num0, Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6};
        for (int i = 0; i < int(keys.size()); ++i)
            if (ctx.input.pressed(keys[size_t(i)]) && _lua) {
                _camera = i;
                (*_lua)["atrium_camera"] = i;
            }
        if (ctx.input.pressed(Key::P))
            next_effect();
        _effect_shown = std::max(0.0f, _effect_shown - ctx.dt);
        if (_bot)
            _bot->step(ctx.input, _session->entity("survivor"), _session->entity("camera"), ctx.dt);
        // Playing in a window: the mouse turns the camera (held in the window; Tab lets it go and takes it back).
        if (ctx.input.pressed(Key::Tab))
            _free_mouse = !_free_mouse;
        const bool windowed = _o.shot.empty() && !_o.bake && !_bot;
        const bool captured = _camera == 6 && windowed && !_free_mouse;
        if (windowed && captured != ctx.window.relative_mouse())
            ctx.window.set_relative_mouse(captured);
        if (_lua && (captured || _bot)) {
            const Vec2f motion = ctx.input.mouse_motion();
            (*_lua)["atrium_look_x"] = (*_lua)["atrium_look_x"].get_or(0.0f) + motion.x;
            (*_lua)["atrium_look_y"] = (*_lua)["atrium_look_y"].get_or(0.0f) + motion.y;
        }
        if (!_session->step(ctx.dt))
            require(_session->diagnostics());
    }

    void render(SceneContext& ctx) override {
        const auto size = ctx.renderer.output_size();
        if (!_session) {
            ctx.renderer.clear();
            loading_screen(ctx, size, 1);
            return;
        }
        if (_bot)
            measure(ctx);
        if (size != _size && !_session->resize(size))
            require(_session->diagnostics());
        _size = size;
        if (!_session->render(ctx.alpha))
            require(_session->diagnostics());
        ctx.renderer.clear();
        ctx.renderer.draw_texture(_session->view()->texture(), {0, 0, f32(size.x), f32(size.y)});
        if (_effect_shown > 0.0f) {
            // The portals' effect, named for a moment as P changes it.
            static constexpr std::array names{"off", "ripples", "vortex", "glitch", "membrane"};
            const u8 a = u8(255.0f * std::min(1.0f, _effect_shown));
            ctx.renderer.fill_rect({f32(size.x) / 2 - 190, 18, 380, 40}, Color::rgba(10, 16, 18, u8(a * 0.7f)));
            ui2::draw_text_centered(ctx.renderer, std::string("Portal effect: ") + names[_effect],
                                    {f32(size.x) / 2, 38}, 2, Color::rgba(150, 240, 230, a));
        }
        if (_windowed && _shown < warm_frames + fade_frames) {
            // The scene's first frames behind the screen (its reflection probes captured, the reflections with a frame
            // before to read), then the screen fading away.
            ++_shown;
            loading_screen(ctx, size,
                           _shown <= warm_frames ? 1.0f : 1.0f - f32(_shown - warm_frames) / f32(fade_frames));
        }
        if (_o.bake && ++_baked == 3) { // (a few frames in: the scene and its probes as they stand)
            ReflectionProbes3D baker{*_renderer, _o.content};
            if (!baker.bake(*_session->world(), *_session->scene(), _o.content, "probes", _session->view()))
                require(_renderer->diagnostics());
            std::cout << "planar_atrium: baked the reflection probes into " << (_o.content / "probes").string() << '\n';
            ctx.app.quit();
            return;
        }
        if (!_o.shot.empty() && ++_frame == _o.frames) {
            Image image;
            if (!ctx.renderer.read_rgba({0, 0, f32(size.x), f32(size.y)}, image.rgba, image.size) ||
                !save_image(image, _o.shot))
                throw std::runtime_error("cannot write " + _o.shot.string());
            std::cout << "planar_atrium: wrote " << _o.shot.string() << '\n';
            ctx.app.quit();
        }
    }

    kin::EcsWorld* world() override { return _session ? _session->world() : nullptr; }
    void on_exit(SceneContext&) override {
        _session.reset();
        _assets.reset();
        _own.reset();
    }

private:
    // --autoplay: each frame's time (the whole frame, from the last render to this one: headless, as fast as it goes),
    // the 3D renderer's CPU recording time, the GPU time, the pipelines made; after 10 seconds of warm-up (pipelines,
    // probes, GI settling). At the end, the report.
    void measure(SceneContext& ctx) {
        using clock = std::chrono::steady_clock;
        const auto now = clock::now();
        const f32 warm = 10.0f;
        ++_frame;
        const f32 time = f32(_frame) / 60.0f;
        if (time > warm) {
            if (_resident_start == 0.0)
                _resident_start = resident_mib(), _pipelines_start = _pipelines;
            _frame_ms.push_back(std::chrono::duration<f64, std::milli>(now - _last_frame).count());
            _cpu_ms.push_back(_renderer->stats().cpu_ms);
            const auto stats = ctx.renderer.backend_stats();
            if (stats.gpu_frames_sampled != _gpu_sampled) {
                _gpu_ms.push_back(stats.last_gpu_frame_ms);
                // The slowest few, and where: the survivor's place and the camera's (a frame or two late: the GPU's).
                const auto survivor = _session->entity("survivor"), camera = _session->entity("camera");
                const auto* controller = survivor ? survivor.get<CharacterController3D>() : nullptr;
                const auto* eye = camera ? camera.get<Transform3D>() : nullptr;
                _slowest.push_back({stats.last_gpu_frame_ms, time, controller ? controller->state.position : Vec3f{},
                                    eye ? eye->position : Vec3f{}});
                std::ranges::sort(_slowest, std::greater{}, &Slow::gpu_ms);
                if (_slowest.size() > 5)
                    _slowest.pop_back();
                _gpu_sampled = stats.gpu_frames_sampled;
            }
        }
        _pipelines += _renderer->stats().pipelines_made;
        if (_frame % 600 == 0)
            _resident.push_back(resident_mib());
        _last_frame = now;
        if (time >= _o.autoplay) {
            report();
            ctx.app.quit();
        }
    }

    void report() {
        const AtriumBot& bot = *_bot;
        const f64 resident = resident_mib();
        // Memory still growing: over the second half of a run of four minutes or more, more than 32 MiB (the caches
        // fill in the first minutes; a run that keeps going leaks).
        std::string memory_finding;
        if (_resident.size() >= 24) {
            const f64 half = _resident[_resident.size() / 2], grown = resident - half;
            if (grown > 32.0) {
                _memory_growing = true;
                char line[160];
                std::snprintf(line, sizeof line,
                              "memory still growing: %.0f MiB more over the second half (%.0f to %.0f)", grown, half,
                              resident);
                memory_finding = line;
            }
        }
        std::ostringstream text;
        text << "{\n  \"seconds\": " << _o.autoplay << ",\n  \"laps\": " << bot.laps
             << ",\n  \"portal_crossings\": " << bot.crossings << ",\n  \"portals_taken\": " << bot.portals
             << ",\n  \"edge_stops\": " << bot.edges << ",\n  \"frame_ms\": {\"p50\": " << percentile(_frame_ms, 0.5)
             << ", \"p95\": " << percentile(_frame_ms, 0.95) << ", \"p99\": " << percentile(_frame_ms, 0.99)
             << ", \"max\": " << percentile(_frame_ms, 1.0)
             << "},\n  \"render3d_cpu_ms\": {\"p50\": " << percentile(_cpu_ms, 0.5)
             << ", \"p95\": " << percentile(_cpu_ms, 0.95) << ", \"p99\": " << percentile(_cpu_ms, 0.99)
             << ", \"max\": " << percentile(_cpu_ms, 1.0)
             << "},\n  \"gpu_frame_ms\": {\"p50\": " << percentile(_gpu_ms, 0.5)
             << ", \"p95\": " << percentile(_gpu_ms, 0.95) << ", \"p99\": " << percentile(_gpu_ms, 0.99)
             << ", \"max\": " << percentile(_gpu_ms, 1.0)
             << "},\n  \"pipelines_after_warm_up\": " << _pipelines - _pipelines_start
             << ",\n  \"resident_mib\": {\"after_warm_up\": " << _resident_start << ", \"end\": " << resident
             << ", \"every_10_s\": [";
        for (size_t i = 0; i < _resident.size(); ++i)
            text << (i ? ", " : "") << int(_resident[i]);
        text << "]},\n  \"slowest_gpu_frames\": [";
        for (size_t i = 0; i < _slowest.size(); ++i) {
            const Slow& f = _slowest[i];
            char line[200];
            std::snprintf(line, sizeof line,
                          "%s\n    {\"gpu_ms\": %.2f, \"time\": %.1f, \"survivor\": [%.2f, %.2f, %.2f], \"camera\": "
                          "[%.2f, %.2f, %.2f]}",
                          i ? "," : "", f.gpu_ms, f.time, f.survivor.x, f.survivor.y, f.survivor.z, f.camera.x,
                          f.camera.y, f.camera.z);
            text << line;
        }
        text << (_slowest.empty() ? "" : "\n  ") << "],\n  \"findings\": [";
        for (size_t i = 0; i < bot.findings.size(); ++i)
            text << (i ? "," : "") << "\n    {\"time\": " << bot.findings[i].time << ", \"what\": \""
                 << bot.findings[i].what << "\"}";
        if (!memory_finding.empty())
            text << (bot.findings.empty() ? "" : ",") << "\n    {\"time\": " << _o.autoplay << ", \"what\": \""
                 << memory_finding << "\"}";
        text << (bot.findings.empty() && memory_finding.empty() ? "" : "\n  ") << "]\n}\n";
        std::cout << "planar_atrium autoplay:\n" << text.str();
        if (!_o.autoplay_report.empty()) {
            if (_o.autoplay_report.has_parent_path())
                std::filesystem::create_directories(_o.autoplay_report.parent_path());
            std::ofstream(_o.autoplay_report) << text.str();
        }
        failed = !bot.findings.empty() || bot.portals == 0 || _memory_growing;
    }

public:
    inline static bool failed = false;

private:
    static void require(const std::vector<std::string>& diagnostics) {
        if (diagnostics.empty())
            return;
        std::string text;
        for (const auto& line : diagnostics)
            text += line + '\n';
        throw std::runtime_error(text);
    }

    Options _o;
    Renderer3D* _renderer = nullptr;
    std::unique_ptr<Renderer3D> _own;
    std::unique_ptr<Assets3D> _assets;
    std::unique_ptr<SceneSession3D> _session;
    sol::state* _lua = nullptr;
    Vec2i _size{};
    int _camera = 0, _frame = 0, _baked = 0;
    bool _free_mouse = false;
    // Loading (loading_screen): the steps, the next one, what they hold until the scene has loaded, the frames shown.
    static constexpr int warm_frames = 4, fade_frames = 24;
    std::vector<std::pair<std::string, std::function<void()>>> _steps;
    std::size_t _step = 0;
    PlanarSurfaces3D _surfaces;
    std::vector<MaterialGraphShader3D> _graphs;
    std::vector<AssetHandle<Model3D>> _models;
    bool _windowed = false;
    int _shown = 0;
    u32 _effect = 0;          // the portals' (next_effect)
    f32 _effect_shown = 0.0f; // seconds its name still shows
    std::unique_ptr<AtriumBot> _bot;
    std::chrono::steady_clock::time_point _last_frame = std::chrono::steady_clock::now();
    std::vector<f64> _frame_ms, _cpu_ms, _gpu_ms;
    u64 _gpu_sampled = 0, _pipelines = 0, _pipelines_start = 0;
    f64 _resident_start = 0.0;
    std::vector<f64> _resident;
    bool _memory_growing = false;
    struct Slow {
        f64 gpu_ms;
        f32 time;
        Vec3f survivor, camera;
    };
    std::vector<Slow> _slowest;
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
            else if (a == "--content")
                o.content = value();
            else if (a == "--camera")
                o.camera = std::clamp(std::stoi(value()), 0, 6);
            else if (a == "--frames")
                o.frames = std::max(1, std::stoi(value()));
            else if (a == "--bake-probes")
                o.bake = true;
            else if (a == "--autoplay")
                o.autoplay = std::max(15.0f, std::stof(value()));
            else if (a == "--autoplay-report")
                o.autoplay_report = value();
            else
                runtime.push_back(argv[i]);
        }
        auto headless = parse_headless_options(int(runtime.size()), runtime.data());
        if (o.autoplay > 0) {
            o.camera = 6;
            o.shot.clear();
        }
        if (!o.shot.empty() || o.bake || o.autoplay > 0) {
            headless.enabled = true;
            headless.render = true; // on the GPU
            headless.frames = o.bake ? 4 : o.autoplay > 0 ? int(o.autoplay * 60.0f) + 10 : o.frames + 1;
        }
        // GPU timing on: the app's frame log (KIN_EVAL_FRAME_LOG, each frame's times, written as it goes; the profile
        // would keep every frame's in memory, and the memory measured would grow with it), beside the report.
        if (o.autoplay > 0 && !std::getenv("KIN_EVAL_FRAME_LOG")) {
#ifdef _WIN32
            const std::string log =
                o.autoplay_report.empty() ? "NUL" : (o.autoplay_report.parent_path() / "frames.log").string();
            _putenv_s("KIN_EVAL_FRAME_LOG", log.c_str());
#else
            const std::string log =
                o.autoplay_report.empty() ? "/dev/null" : (o.autoplay_report.parent_path() / "frames.log").string();
            setenv("KIN_EVAL_FRAME_LOG", log.c_str(), 1);
#endif
            if (o.autoplay_report.has_parent_path())
                std::filesystem::create_directories(o.autoplay_report.parent_path());
        }
        SceneManager scenes;
        scenes.push(std::make_unique<Atrium>(o));
        return run_scene_app({.window = {.title = "Atrium", .width = 1280, .height = 720, .max_fps = 60},
                              .headless = headless,
                              .render_headless = true,
                              .rendering_3d = true},
                             scenes) == 0
                   ? (Atrium::failed ? 2 : 0)
                   : 1;
    } catch (const std::exception& e) {
        std::cerr << "planar_atrium: " << e.what() << '\n';
        return 1;
    }
}
