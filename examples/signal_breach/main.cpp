// Signal Breach: Signal Siege's fight first person, on kin's experimental 3D
// (docs/3d_plan.md, M2). Click to play (the mouse is captured and looks
// around); Esc pauses and lets it go.
#include "breach_view.hpp"
#include "example_common.hpp"
#include "siege_audio.hpp"

#include <kin/assets/content.hpp>
#include <kin/assets/file_watcher.hpp>
#include <kin/core/json.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace examples {
namespace {

struct BreachOptions {
    Options common;
    float sensitivity = 0.08f; // degrees a pixel of mouse motion
    // --latency-probe: mouse motion pushed as the OS would, at times unrelated
    // to the frames, timed until the camera turns and the frame is presented.
    bool latency_probe = false;
    BreachViewOptions view{}; // --no-shadows, --no-bloom, --no-lights
};

BreachOptions parse(int argc, char** argv) {
    BreachOptions o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 == argc) throw std::invalid_argument(std::string{arg} + " needs a value");
            return argv[++i];
        };
        const auto number = [&](float& out, float lo, float hi) {
            const std::string text = value();
            const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
            if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || !std::isfinite(out) || out < lo || out > hi)
                throw std::invalid_argument(std::string{arg} + " value out of range");
        };
        const auto integer = [&](int& out, int hi) {
            float v = 0;
            number(v, 1, float(hi));
            out = static_cast<int>(v);
        };
        if (arg == "--benchmark") o.common.benchmark = true;
        else if (arg == "--enemies") integer(o.common.enemies, Breach::max_enemies);
        else if (arg == "--width") integer(o.common.width, 4096);
        else if (arg == "--height") integer(o.common.height, 4096);
        else if (arg == "--capture-frame") integer(o.common.capture_frame, 1000000);
        else if (arg == "--screenshot") o.common.screenshot = value();
        else if (arg == "--ui-scale") number(o.common.ui_scale, .5f, 4);
        else if (arg == "--sensitivity") number(o.sensitivity, .001f, 2);
        else if (arg == "--vsync") o.common.vsync = true;
        else if (arg == "--latency-probe") o.latency_probe = true;
        else if (arg == "--no-shadows") o.view.shadows = false;
        else if (arg == "--no-bloom") o.view.bloom = false;
        else if (arg == "--no-lights") o.view.lights = false;
        else if (arg == "--mute") o.common.mute = true;
        else if (arg == "--help") o.common.help = true;
    }
    if (o.common.width < 640 || o.common.height < 480) throw std::invalid_argument("minimum size is 640x480");
    return o;
}

class BreachScene final : public Scene {
public:
    BreachScene(BreachOptions options, bool tool_run)
        : _options(std::move(options)), _breach(_options.common.enemies),
          // Signal Siege's sounds; its cues fade over a screen, here over about 27 m.
          _audio(_options.common.mute || _options.common.benchmark || tool_run, 1.0f / 40.0f), _autoplay(tool_run) {
        if (_options.latency_probe) {
            // Motion every 23 ms (no common beat with the frames), stamped as SDL would.
            _probe_thread = std::thread([this] {
                while (!_probe_stop) {
                    SDL_Event event{};
                    event.type = SDL_EVENT_MOUSE_MOTION;
                    event.motion.xrel = 0.5f;
                    event.motion.timestamp = SDL_GetTicksNS();
                    SDL_PushEvent(&event);
                    std::this_thread::sleep_for(std::chrono::milliseconds(23));
                }
            });
        }
    }
    ~BreachScene() override {
        release_mouse();
        _probe_stop = true;
        if (_probe_thread.joinable()) _probe_thread.join();
        if (_options.latency_probe) {
            // Printed too: a windowed run (the one that presents to a display) has no report.
            std::cout << "latency: " << _to_read_ms.size() << " samples, event to camera " << stat(_to_read_ms, .5) << " ms (p95 "
                      << stat(_to_read_ms, .95) << "), event to presented " << stat(_to_present_ms, .5) << " ms (p95 "
                      << stat(_to_present_ms, .95) << "), GPU frame " << stat(_gpu_ms, .5) << " ms\n";
        }
    }

    std::string_view name() const override { return "Signal Breach"; }
    kin::EcsWorld* world() override { return &_breach.world; }

    void on_enter(SceneContext& ctx) override {
        _seed = rng_detail::to_u64(ctx.rng);
        _breach.reset(_seed);
        _yaw = _breach.yaw;
        _pitch = _breach.pitch;
    }

    void update(SceneContext& ctx) override {
        probe_frame_began();
        Input& input = ctx.input;
        if (input.pressed("quit")) {
            // Esc lets the mouse go and pauses; again, it quits.
            if (_paused || (!_captured && !_autoplay)) {
                ctx.app.quit();
                return;
            }
            _paused = true;
            release_mouse();
            return;
        }
        if (input.pressed("autoplay")) _autoplay = !_autoplay;
        if (input.pressed("reset")) {
            _breach.reset(_seed);
            _yaw = _breach.yaw;
            _pitch = _breach.pitch;
            _paused = false;
        }
        // A click captures the mouse (and is not a shot).
        const bool can_capture = !ctx.app.headless() && !_autoplay;
        if (can_capture && !_captured && input.mouse_pressed(MouseButton::Left)) {
            ctx.window.set_relative_mouse(true);
            _window = &ctx.window;
            _captured = true;
            _paused = false;
            return;
        }
        if (_paused) return;
        BreachInput in;
        if (_autoplay) {
            in = _breach.autopilot();
        } else {
            in.move = {float(input.held("right")) - float(input.held("left")), float(input.held("forward")) - float(input.held("back"))};
            in.yaw = _yaw;
            in.pitch = _pitch;
            in.fire = _captured && input.held("fire");
            in.jump = input.pressed("jump");
            in.dash = input.pressed("dash");
        }
        _breach.step(ctx.dt, in, _autoplay);
        if (_autoplay) {
            _yaw = _breach.yaw;
            _pitch = _breach.pitch;
        }
        play_sounds();
        _view.update(_breach, ctx.dt);
    }

    void render(SceneContext& ctx) override {
        probe_frame_began();
        _display_scale = update_ui_scale(ctx.window, _options.common, _minimum_size);
        Renderer2D& renderer = ctx.renderer;
        if (!_logical) {
            renderer.set_logical_size(1280, 800);
            _logical = true;
        }
        renderer.clear(Color::rgb(12, 20, 28));
        if (!_view.ready()) _view.init(renderer, _breach, _options.view);
        // Mouse look, from this frame's motion: the camera turns now, the
        // simulation follows at its next step.
        const Vec2f motion = ctx.input.frame_mouse_motion();
        if ((_captured && !_paused && !_autoplay) || _options.latency_probe) {
            _yaw = std::remainder(_yaw - motion.x * _options.sensitivity, 360.0f);
            _pitch = std::clamp(_pitch - motion.y * _options.sensitivity, -85.0f, 85.0f);
        }
        if (_options.latency_probe) {
            if (_frames == 0) renderer.set_gpu_timing_enabled(true);
            const f64 gpu = renderer.backend_stats().last_gpu_frame_ms;
            if (gpu > 0) _gpu_ms.push_back(gpu);
            if (motion.x != 0 && !_probe_waiting) {
                // This frame turns the camera by that motion: when it began, and now.
                _probe_event_ns = ctx.input.last_mouse_motion_event_time_ns();
                _probe_read_ns = SDL_GetTicksNS();
                _probe_waiting = true;
                _probe_frame = _frames + 1;
            }
        }
        _view.draw(renderer, _breach, ctx.input,
                   {.paused = _paused, .autoplay = _autoplay, .captured = _captured, .display_scale = _display_scale,
                    .yaw = _yaw, .pitch = _pitch},
                   ctx.dt);
        capture(ctx, _options.common, ++_frames, _captured_shot);
    }

    void write_report(JsonWriter& json) const override {
        json.field("ticks", _breach.ticks).field("enemies", _breach.enemy_count()).field("kills", _breach.kills)
            .field("cores", _breach.collected).field("health", _breach.health).field("shots", u64(_breach.shots.size()))
            .field("checksum", _breach.checksum()).field("autoplay", _autoplay)
            .field("player_x", f64(_breach.player.x)).field("player_y", f64(_breach.player.y)).field("player_z", f64(_breach.player.z))
            .field("yaw", f64(_breach.yaw)).field("pitch", f64(_breach.pitch));
        const Render3DStats& s = _view.stats();
        json.field("draws_3d", u64(s.draws)).field("instances_3d", u64(s.instances)).field("culled_3d", u64(s.culled))
            .field("triangles_3d", s.triangles).field("drawn_3d", _view.available());
        if (_options.latency_probe) {
            json.key("latency").begin_object()
                .field("samples", u64(_to_read_ms.size()))
                .field("event_to_camera_ms_median", stat(_to_read_ms, .5)).field("event_to_camera_ms_p95", stat(_to_read_ms, .95))
                .field("event_to_present_ms_median", stat(_to_present_ms, .5)).field("event_to_present_ms_p95", stat(_to_present_ms, .95))
                .field("gpu_frame_ms_median", stat(_gpu_ms, .5))
                .end_object();
        }
    }

private:
    // The step's events as sounds, heard from the player's eye.
    void play_sounds() {
        _audio.set_listener_3d({.position = _breach.eye_position(), .forward = _breach.view_direction()});
        for (const BreachEvent& e : _breach.events) {
            switch (e.kind) {
            case BreachEvent::Kind::Fire: _audio.play_at("shot", e.pos); break;
            case BreachEvent::Kind::EnemyFire: _audio.play_at("enemy_shot", e.pos); break;
            case BreachEvent::Kind::Hit: _audio.play_at("hit", e.pos); break;
            case BreachEvent::Kind::Kill: _audio.play_at("kill", e.pos); break;
            case BreachEvent::Kind::Pickup: _audio.play_at("pickup", e.pos); break;
            case BreachEvent::Kind::Hurt: _audio.play_at("hurt", e.pos); break;
            case BreachEvent::Kind::Jump: break;
            }
        }
        _audio.update(1.0f / 120.0f);
    }
    static f64 stat(std::vector<f64> v, f64 share) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<std::size_t>(share * static_cast<f64>(v.size())))];
    }
    // A probed motion's frame is presented when the next frame begins.
    void probe_frame_began() {
        if (_probe_waiting && _frames >= _probe_frame) {
            const u64 now = SDL_GetTicksNS();
            _to_read_ms.push_back(static_cast<f64>(_probe_read_ns - _probe_event_ns) / 1e6);
            _to_present_ms.push_back(static_cast<f64>(now - _probe_event_ns) / 1e6);
            _probe_waiting = false;
        }
    }
    void release_mouse() {
        if (_window && _captured) _window->set_relative_mouse(false);
        _captured = false;
    }

    BreachOptions _options;
    Breach _breach;
    SiegeAudio _audio;
    BreachView _view;
    Window* _window = nullptr;
    float _yaw = 0, _pitch = 0; // the player's look, turned each rendered frame
    float _display_scale = 1;
    Vec2i _minimum_size{};
    u64 _seed = 7;
    int _frames = 0;
    bool _autoplay = false, _paused = false, _captured = false, _logical = false, _captured_shot = false;
    std::thread _probe_thread;
    std::atomic<bool> _probe_stop{false};
    bool _probe_waiting = false;
    int _probe_frame = 0;
    u64 _probe_event_ns = 0, _probe_read_ns = 0;
    std::vector<f64> _to_read_ms, _to_present_ms, _gpu_ms;
};

int run(int argc, char** argv) {
    try {
        const BreachOptions options = parse(argc, argv);
        if (options.common.help) {
            std::cout << "--benchmark (autoplay) --enemies 1..100000 --sensitivity DEG_PER_PIXEL (0.08)\n"
                         "--latency-probe (time pushed mouse motion until the camera turns and the frame is presented)\n"
                         "--no-shadows --no-bloom --no-lights (leave a feature off) --mute\n"
                         "--width 640..4096 --height 480..4096 --ui-scale 0.5..4 --vsync --screenshot PATH --capture-frame N\n"
                         "Standard Kin flags: --headless --frames N --seed N --report PATH --profile --server\n";
            return 0;
        }
        InputMap input;
        input.bind("quit", Key::Escape);
        input.bind("reset", Key::R);
        input.bind("autoplay", Key::B);
        input.bind("forward", Key::W);
        input.bind("back", Key::S);
        input.bind("left", Key::A);
        input.bind("right", Key::D);
        input.bind("jump", Key::Space);
        input.bind("dash", Key::LeftShift);
        input.bind("fire", MouseButton::Left);
        GameInfo game{.id = "signal_breach", .title = "Signal Breach", .version = "0.1",
                      .description = "First-person arena survival on kin's experimental 3D",
                      .window = {.width = options.common.width, .height = options.common.height, .logical_width = 1280,
                                 .logical_height = 800, .resizable = true},
                      .tags = {"example", "benchmark", "3d"}, .input_map = input};
        auto window = window_config(game);
        window.fixed_dt = 1.0f / 120;
        window.vsync = options.common.vsync;
        auto headless = parse_headless_options(argc, argv);
        if (!options.common.screenshot.empty() && headless.enabled && headless.frames == 0)
            headless.frames = options.common.capture_frame;
        Localization l10n;
        FileWatcher files;
        if (find_content_root(KIN_GAME_CONTENT, argc, argv).empty()) {
            std::cerr << "Signal Breach: its content was not found\n";
            return 1;
        }
        std::vector<std::string> errors;
        l10n.load_directory(content_root() / "lang", errors, &files);
        for (const std::string& error : errors) std::cerr << "Language file: " << error << '\n';
        l10n.set_locale(system_locales());
        set_active_localization(&l10n);
        const bool tool_run = headless.enabled || headless.server || options.common.benchmark || !options.common.screenshot.empty();
        SceneManager scenes;
        const auto build = [options, tool_run](SceneManager& manager) {
            manager.push(std::make_unique<BreachScene>(options, tool_run));
        };
        build(scenes);
        const int code = run_scene_app({.window = window, .headless = headless, .game = &game,
                                        .render_headless = !options.common.screenshot.empty() || headless.server,
                                        .reset_scenes = build, .file_watcher = &files, .localization = &l10n},
                                       scenes);
        set_active_localization(nullptr);
        return code;
    } catch (const std::exception& e) {
        std::cerr << "Signal Breach: " << e.what() << '\n';
        return 1;
    }
}

} // namespace
} // namespace examples

int main(int argc, char** argv) { return examples::run(argc, argv); }
