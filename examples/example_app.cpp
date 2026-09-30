#include "example_app.hpp"
#include "example_common.hpp"
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
        else if (arg == "--enemies") integer(o.enemies, Arena::max_enemies);
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
        else if (arg == "--mute") o.mute = true;
        else if (arg == "--help") o.help = true;
    }
    if (o.scenario != "idle" && o.scenario != "live" && o.scenario != "scroll" && o.scenario != "churn")
        throw std::invalid_argument("scenario must be idle, live, scroll, or churn");
    if (o.width < 640 || o.height < 480) throw std::invalid_argument("minimum example size is 640x480");
    return o;
}

} // namespace

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

namespace {

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
            std::cout << "--benchmark (autoplay/scripted UI) --stress --enemies 1..100000 --runs 1..100000\n"
                "--scenario idle|live|scroll|churn --vsync --screenshot PATH --capture-frame N\n"
                "--width 640..4096 --height 480..4096 --ui-scale 0.5..4 (default: automatic)\n"
                "--power-grid (Signal Siege: start on the paused upgrade screen) --mute (Signal Siege: no sound)\n"
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
        input.bind("menu_up", {Key::Up, Key::W}); input.bind("menu_down", {Key::Down, Key::S});
        input.bind("accept", {Key::Enter, Key::KeypadEnter, Key::Space});
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
        const bool start_in_arena = headless.enabled || options.benchmark || options.power_grid || !options.screenshot.empty();
        const bool tool_run = start_in_arena || headless.server;
        const auto build = [options, tracker, start_in_arena, tool_run](SceneManager& manager) {
            if (tracker) manager.push(std::make_unique<TrackerScene>(options));
            else manager.push(make_signal_siege(options, start_in_arena, tool_run));
        };
        build(scenes);
        return run_scene_app({.window = window, .headless = headless, .game = &game,
            // The server renders every step: menus and HUD run in render().
            .render_headless = !options.screenshot.empty() || headless.server, .reset_scenes = build}, scenes);
    } catch (const std::exception& e) {
        std::cerr << "Example error: " << e.what() << '\n'; return 1;
    }
}
}
