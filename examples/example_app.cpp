#include "example_app.hpp"
#include "workloads.hpp"
#include <kin/core/json.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace examples {
namespace {
struct Options {
    int enemies = 300, runs = 1000, capture_frame = 120;
    int width = 1280, height = 800;
    float ui_scale = 0; // zero follows the window's current display
    bool benchmark = false, help = false, vsync = false, power_grid = false;
    std::string scenario = "live", screenshot;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&](std::string_view name) -> std::string {
            if (i + 1 == argc) throw std::invalid_argument(std::string{name} + " needs a value");
            return argv[++i];
        };
        const auto integer = [&](int& out, int maximum) {
            const std::string text = value(arg);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || out < 1 || out > maximum)
                throw std::invalid_argument(std::string{arg} + " value out of range");
        };
        if (arg == "--benchmark") o.benchmark = true;
        else if (arg == "--power-grid") o.power_grid = true;
        else if (arg == "--stress") { o.enemies = 1200; o.runs = 10000; }
        else if (arg == "--enemies") integer(o.enemies, 5000);
        else if (arg == "--runs") integer(o.runs, 100000);
        else if (arg == "--capture-frame") integer(o.capture_frame, 1000000);
        else if (arg == "--width") integer(o.width, 4096);
        else if (arg == "--height") integer(o.height, 4096);
        else if (arg == "--ui-scale") {
            const std::string text = value(arg);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), o.ui_scale);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
                !std::isfinite(o.ui_scale) || o.ui_scale < .5f || o.ui_scale > 4)
                throw std::invalid_argument("--ui-scale must be between 0.5 and 4");
        }
        else if (arg == "--scenario") o.scenario = value(arg);
        else if (arg == "--screenshot") o.screenshot = value(arg);
        else if (arg == "--vsync") o.vsync = true;
        else if (arg == "--help") o.help = true;
    }
    if (o.scenario != "idle" && o.scenario != "live" && o.scenario != "scroll" && o.scenario != "churn")
        throw std::invalid_argument("scenario must be idle, live, scroll, or churn");
    if (o.width < 640 || o.height < 480) throw std::invalid_argument("minimum example size is 640x480");
    return o;
}

void capture(SceneContext& ctx, const Options& options, int ticks, bool& captured) {
    if (captured || options.screenshot.empty() || ticks < options.capture_frame) return;
    const std::filesystem::path path{options.screenshot};
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    if (!ctx.renderer.save_png(options.screenshot)) {
        if (ctx.report) ctx.report->fail("Screenshot capture failed");
        throw std::runtime_error("Screenshot capture failed: " + options.screenshot);
    }
    captured = true;
}

float update_ui_scale(Window& window, const Options& options, Vec2i& last_minimum) {
    const float scale = options.ui_scale > 0 ? options.ui_scale : window.display_scale();
    const auto units = window.size(), pixels = window.pixel_size();
    if (units.x > 0 && units.y > 0 && pixels.x > 0 && pixels.y > 0) {
        const Vec2i minimum{int(std::ceil(640 * scale * units.x / pixels.x)),
                            int(std::ceil(480 * scale * units.y / pixels.y))};
        if (minimum != last_minimum) {
            window.set_minimum_size(minimum);
            if (units.x < minimum.x || units.y < minimum.y)
                window.set_size({std::max(units.x, minimum.x), std::max(units.y, minimum.y)});
            last_minimum = minimum;
        }
    }
    return scale;
}

class ArenaScene final : public Scene {
public:
    explicit ArenaScene(Options o) : _options(std::move(o)), _arena(_options.enemies) {}
    std::string_view name() const override { return "Signal Siege"; }
    EcsWorld* world() override { return &_arena.world; }
    void on_enter(SceneContext& ctx) override {
        _seed = rng_detail::to_u64(ctx.rng); _arena.reset(_seed);
        _autoplay = _options.benchmark || ctx.app.headless();
        _power_grid = _options.power_grid;
    }
    void update(SceneContext& ctx) override {
        if (ctx.input.pressed("quit")) {
            if (_power_grid) { _power_grid = false; return; }
            ctx.app.quit();
        }
        if (ctx.input.pressed("power_grid")) { _power_grid = !_power_grid; return; }
        if (ctx.input.pressed("pause")) _paused = !_paused;
        if (ctx.input.pressed("autoplay")) _autoplay = !_autoplay;
        if (ctx.input.pressed("reset")) { _arena.reset(_seed); _power_grid = false; _paused = false; _power_state = {}; }
        if (_paused || _power_grid) return;
        const Vec2f mouse = ctx.renderer.window_to_logical(ctx.input.mouse_pos());
        const Vec2f aim = _camera.screen_to_world(mouse);
        ArenaInput input{.move = {
            float(ctx.input.held("right")) - float(ctx.input.held("left")),
            float(ctx.input.held("down")) - float(ctx.input.held("up"))},
            .aim = {aim.x - _arena.player.x, aim.y - _arena.player.y},
            .fire = ctx.input.held("fire"), .dash = ctx.input.pressed("dash")};
        _arena.step(ctx.dt, _autoplay ? _arena.autopilot() : input, _autoplay);
    }
    void render(SceneContext& ctx) override {
        _display_scale = update_ui_scale(ctx.window, _options, _minimum_size);
        auto& renderer = ctx.renderer;
        if (_grid_coordinates != _power_grid) {
            renderer.set_logical_size(_power_grid ? 0 : 1280, _power_grid ? 0 : 800);
            _grid_coordinates = _power_grid;
        }
        renderer.clear(Color::rgb(12, 20, 28));
        if (_power_grid) {
            if (render_power_grid(_arena,ctx.input,renderer,_power_state,_display_scale)) _power_grid=false;
            capture(ctx, _options, ++_frames, _captured);
            return;
        }
        _camera.viewport = {1280, 800};
        _camera.offset = {std::clamp(_arena.player.x - 640, 0.0f, 1792.0f), std::clamp(_arena.player.y - 400, 0.0f, 1248.0f)};
        // Camera-local floor panels: decoration never participates in physics.
        const int tile_x=int(_camera.offset.x)/256, tile_y=int(_camera.offset.y)/256;
        for (int y=tile_y;y<=tile_y+4;++y) for (int x=tile_x;x<=tile_x+5;++x) {
            const auto p=_camera.world_to_screen({float(x*256),float(y*256)});
            renderer.fill_rect({p.x+2,p.y+2,252,252},(x+y)%2 ? Color::rgb(15,25,35) : Color::rgb(17,28,38));
            renderer.draw_line({p.x+12,p.y+12},{p.x+34,p.y+12},Color::rgb(34,53,65));
            renderer.draw_line({p.x+12,p.y+12},{p.x+12,p.y+34},Color::rgb(34,53,65));
            // Recessed cable channels and travelling power indicators.
            renderer.fill_rect({p.x+120,p.y+2,16,252},Color::rgb(9,19,28));
            renderer.draw_line({p.x+122,p.y+2},{p.x+122,p.y+254},Color::rgb(28,60,71));
            const float pulse=std::fmod(_arena.elapsed*45+float((x+y)*37),224.0f);
            renderer.fill_rect({p.x+126,p.y+12+pulse,3,14},Color::rgb(44,114,128));
            if ((x+y)%3==0) {
                renderer.fill_rect({p.x+178,p.y+180,48,36},Color::rgb(9,18,26));
                for (int i=0;i<5;++i) renderer.fill_rect({p.x+183,p.y+185+i*6.0f,38,2},Color::rgb(35,49,60));
            }
        }
        for (int x = 0; x <= 3072; x += 64) {
            const float sx = x - _camera.offset.x;
            if (sx >= 0 && sx <= 1280) renderer.draw_line({sx, 0}, {sx, 800}, Color::rgb(21,33,43));
        }
        for (int y = 0; y <= 2048; y += 64) {
            const float sy = y - _camera.offset.y;
            if (sy >= 0 && sy <= 800) renderer.draw_line({0, sy}, {1280, sy}, Color::rgb(21,33,43));
        }
        // Sector landmarks make camera movement and aim direction legible.
        for (int y = 256; y < 2048; y += 512) for (int x = 256; x < 3072; x += 512) {
            auto p = _camera.world_to_screen({float(x), float(y)});
            if (p.x < -60 || p.x > 1340 || p.y < -60 || p.y > 860) continue;
            renderer.fill_rect({p.x-38,p.y-38,76,76},Color::rgb(12,22,31));
            renderer.draw_rect({p.x-38,p.y-38,76,76},Color::rgb(36,58,70));
            renderer.draw_rect({p.x-28,p.y-28,56,56},Color::rgb(27,45,58));
            renderer.fill_rect({p.x-10,p.y-2,20,4},Color::rgb(38,78,83));
            renderer.fill_rect({p.x-2,p.y-10,4,20},Color::rgb(38,78,83));
            renderer.fill_rect({p.x-27,p.y+32,12,2},Color::rgb(89,109,90));
            renderer.fill_rect({p.x+15,p.y-34,12,2},Color::rgb(89,109,90));
            // Rotating reactor geometry is driven by simulation time, so it
            // freezes with pause and the upgrade screen.
            for (int i=0;i<8;++i) {
                const float angle=i*.785398f+_arena.elapsed*.3f;
                const Vec2f a{p.x+std::cos(angle)*15,p.y+std::sin(angle)*15};
                const Vec2f b{p.x+std::cos(angle+.45f)*24,p.y+std::sin(angle+.45f)*24};
                renderer.draw_line(a,b,Color::rgb(48,119,127));
            }
            renderer.fill_rect({p.x-5,p.y-5,10,10},Color::rgb(61,145,143));
            for (int i=0;i<4;++i) {
                const float sx=p.x-36+i*20.0f;
                renderer.draw_line({sx,p.y+43},{sx+7,p.y+50},Color::rgb(129,104,60));
                renderer.draw_line({sx,p.y-50},{sx+7,p.y-43},Color::rgb(129,104,60));
            }
        }
        const RenderView view{.camera = &_camera, .culling_enabled = true};
        _arena.collect(_queue, view); _queue.flush(renderer, view);
        const auto p = _camera.world_to_screen(_arena.player);
        const auto mouse = renderer.window_to_logical(ctx.input.mouse_pos());
        if (!_autoplay) {
            renderer.draw_line(p, mouse, Color::rgba(88,171,169,35));
            constexpr auto reticle=Color::rgb(183,239,228);
            renderer.draw_line({mouse.x-11,mouse.y},{mouse.x-5,mouse.y},reticle);
            renderer.draw_line({mouse.x+5,mouse.y},{mouse.x+11,mouse.y},reticle);
            renderer.draw_line({mouse.x,mouse.y-11},{mouse.x,mouse.y-5},reticle);
            renderer.draw_line({mouse.x,mouse.y+5},{mouse.x,mouse.y+11},reticle);
            renderer.fill_rect({mouse.x-1,mouse.y-1,2,2},reticle);
        }
        render_arena_hud(_arena, renderer, _paused, _autoplay, int(_queue.size()), _display_scale);
        capture(ctx, _options, ++_frames, _captured);
    }
    void write_report(JsonWriter& json) const override {
        json.field("ticks", _arena.ticks).field("enemies", _arena.enemy_count()).field("kills", _arena.kills)
            .field("cores", _arena.collected).field("health", _arena.health).field("shots", u64(_arena.shots.size()))
            .field("commands", u64(_queue.size())).field("checksum", _arena.checksum()).field("autoplay", _autoplay);
        json.field("ui_scale", _display_scale);
        json.field("power_grid", _power_grid).field("available_cores", _arena.available_cores());
    }
private:
    Options _options;
    Arena _arena;
    Camera2D _camera;
    RenderQueue _queue;
    Vec2i _minimum_size{};
    float _display_scale = 1;
    u64 _seed = 7;
    int _frames = 0;
    PowerGridState _power_state;
    bool _power_grid = false;
    bool _grid_coordinates = false;
    bool _paused = false, _autoplay = false, _captured = false;
};

class TrackerScene final : public Scene {
public:
    explicit TrackerScene(Options o) : _options(std::move(o)) {}
    std::string_view name() const override { return "Run Observatory"; }
    void on_enter(SceneContext& ctx) override { _model = std::make_unique<Tracker>(_options.runs, rng_detail::to_u64(ctx.rng)); }
    void update(SceneContext& ctx) override {
        if (ctx.input.pressed("quit")) ctx.app.quit();
        if (_options.benchmark || ctx.app.headless()) _dashboard.scripted_frame(*_model, _model->ticks, _options.scenario);
        _model->step(ctx.dt);
    }
    void render(SceneContext& ctx) override {
        const float scale = update_ui_scale(ctx.window, _options, _minimum_size);
        // Refresh the GPU target after a resize before asking for layout bounds.
        ctx.renderer.clear();
        const auto size = ctx.renderer.output_size();
        _dashboard.render(*_model, ctx.input, ctx.renderer, {float(size.x), float(size.y)}, ctx.dt, scale);
        _display_scale = scale;
        ctx.window.set_text_input_enabled(_dashboard.wants_text_input());
        capture(ctx, _options, ++_render_frames, _captured);
    }
    void on_exit(SceneContext& ctx) override { ctx.window.set_text_input_enabled(false); }
    void write_report(JsonWriter& json) const override {
        json.field("runs", u64(_model->runs.size())).field("ticks", _model->ticks).field("telemetry_revisions", _model->revisions)
            .field("matching_runs", u64(_model->visible.size())).field("drawn_rows", _dashboard.drawn_rows())
            .field("scenario", _options.scenario).field("checksum", _model->checksum()).field("simulated_data", true);
        json.field("ui_scale", _display_scale);
    }
private:
    Options _options;
    std::unique_ptr<Tracker> _model;
    TrackerDashboard _dashboard;
    Vec2i _minimum_size{};
    float _display_scale = 1;
    int _render_frames = 0;
    bool _captured = false;
};
}

int run_example(int argc, char** argv, bool tracker) {
    try {
        const Options options = parse(argc, argv);
        if (options.help) {
            std::cout << "--benchmark (autoplay/scripted UI) --stress --enemies 1..5000 --runs 1..100000\n"
                "--scenario idle|live|scroll|churn --vsync --screenshot PATH --capture-frame N\n"
                "--width 640..4096 --height 480..4096 --ui-scale 0.5..4 (default: automatic)\n"
                "--power-grid (Signal Siege: start on the paused upgrade screen)\n"
                "Standard Kin flags: --headless --frames N --seed N --report PATH --profile --profile-json PATH\n";
            return 0;
        }
        InputMap input;
        input.bind("quit", Key::Escape); input.bind("pause", Key::P); input.bind("reset", Key::R);
        input.bind("autoplay", Key::B); input.bind("dash", Key::Space);
        input.bind("power_grid", Key::U); input.bind("grid_next", Key::Right);
        input.bind("grid_previous", Key::Left); input.bind("grid_buy", {Key::Enter,Key::KeypadEnter});
        input.bind("grid_up",Key::Up); input.bind("grid_down",Key::Down);
        input.bind("up", Key::W); input.bind("down", Key::S); input.bind("left", Key::A); input.bind("right", Key::D);
        input.bind("fire", MouseButton::Left);
        GameInfo game{.id = tracker ? "run_observatory" : "signal_siege", .title = tracker ? "Run Observatory" : "Signal Siege",
            .version = "0.1", .description = tracker ? "Simulated LLM training tracker and UI benchmark" : "Arena survival and ECS performance example",
            .window = {.width = options.width, .height = options.height, .logical_width = tracker ? 0 : 1280, .logical_height = tracker ? 0 : 800, .resizable = true},
            .tags = {"example", "benchmark"}, .input_map = input};
        auto window = window_config(game); window.fixed_dt = 1.0f / 120; window.vsync = options.vsync;
        window.high_pixel_density = tracker;
        auto headless = parse_headless_options(argc, argv);
        if (!options.screenshot.empty() && headless.enabled && headless.frames == 0)
            headless.frames = options.capture_frame;
        if (!options.screenshot.empty() && headless.enabled && headless.frames > 0 && headless.frames < options.capture_frame)
            throw std::invalid_argument("--frames must reach --capture-frame for screenshot capture");
        SceneManager scenes;
        const auto build = [options, tracker](SceneManager& manager) {
            if (tracker) manager.push(std::make_unique<TrackerScene>(options));
            else manager.push(std::make_unique<ArenaScene>(options));
        };
        build(scenes);
        return run_scene_app({.window = window, .headless = headless, .game = &game,
            .render_headless = !options.screenshot.empty(), .reset_scenes = build}, scenes);
    } catch (const std::exception& e) {
        std::cerr << "Example error: " << e.what() << '\n'; return 1;
    }
}
}
