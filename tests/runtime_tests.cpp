#include <kin/core/json.hpp>
#include <kin/core/rng.hpp>
#include <kin/runtime/debug_overlay.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/runtime/windowed_app.hpp>

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool has_log_event(const std::vector<kin::LogEvent>& events,
                   std::string_view category,
                   std::string_view message) {
    return std::ranges::any_of(events, [&](const kin::LogEvent& event) {
        return event.category == category && event.message == message;
    });
}

class RuntimeActionScene final : public kin::Scene {
public:
    void collect_actions(kin::InputActionContext& context) const override {
        context.add("start", "Start");
    }
};

// Records the seed it observed and exposes it through the run report so the test
// can assert that --seed reached the scene deterministically.
class SeededReportScene final : public kin::Scene {
public:
    std::string_view name() const override { return "Seeded"; }

    void update(kin::SceneContext& ctx) override {
        _observed = kin::rng_u32(ctx.rng);
        ++_frames;
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("observed", _observed);
        json.field("frames", _frames);
    }

private:
    kin::u32 _observed = 0;
    kin::i32 _frames = 0;
};

// Fails on its second update to exercise the fail-early + exit-code contract.
class FailingScene final : public kin::Scene {
public:
    std::string_view name() const override { return "Failing"; }

    void update(kin::SceneContext& ctx) override {
        ++_frames;
        if (_frames == 2 && ctx.report) {
            ctx.report->fail("boom");
        }
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("frames", _frames);
    }

private:
    kin::i32 _frames = 0;
};

} // namespace

int main() {
    char arg0[] = "game";
    char arg1[] = "--headless";
    char arg2[] = "--list-actions";
    char arg3[] = "--frames=3";
    char* argv[] = {arg0, arg1, arg2, arg3};
    const kin::HeadlessOptions options = kin::parse_headless_options(4, argv);
    assert(options.enabled);
    assert(options.list_actions);
    assert(options.frames == 3);

    char profile_arg[] = "--profile-render";
    char* profile_argv[] = {arg0, profile_arg, arg3};
    const kin::HeadlessOptions profile_options = kin::parse_headless_options(3, profile_argv);
    assert(profile_options.enabled);
    assert(profile_options.profile_render);
    assert(profile_options.frames == 3);

    char runtime_profile_arg[] = "--profile";
    char profile_lines_arg[] = "--profile-lines";
    char profile_json_arg[] = "--profile-json=profile.json";
    char profile_text_arg[] = "--profile-text";
    char profile_text_path_arg[] = "profile.txt";
    char* runtime_profile_argv[] = {
        arg0,
        runtime_profile_arg,
        profile_lines_arg,
        profile_json_arg,
        profile_text_arg,
        profile_text_path_arg,
        arg3,
    };
    const kin::HeadlessOptions runtime_profile_options = kin::parse_headless_options(7, runtime_profile_argv);
    assert(runtime_profile_options.enabled);
    assert(runtime_profile_options.profile);
    assert(runtime_profile_options.profile_lines);
    assert(runtime_profile_options.profile_json_path == "profile.json");
    assert(runtime_profile_options.profile_text_path == "profile.txt");
    assert(runtime_profile_options.frames == 3);

    char info_arg[] = "--game-info";
    char* info_argv[] = {arg0, info_arg};
    const kin::HeadlessOptions info_options = kin::parse_headless_options(2, info_argv);
    assert(info_options.enabled);
    assert(info_options.print_game_info);

    char seed_arg[] = "--seed=99";
    char report_arg[] = "--report";
    char report_path_arg[] = "out.json";
    char* seed_argv[] = {arg0, seed_arg, report_arg, report_path_arg};
    const kin::HeadlessOptions seed_options = kin::parse_headless_options(4, seed_argv);
    assert(seed_options.seed == 99);
    assert(seed_options.enabled);
    assert(seed_options.report_path == "out.json");

    char log_arg[] = "--log";
    char log_path_arg[] = "kin.jsonl";
    char log_level_arg[] = "--log-level=debug";
    char log_format_arg[] = "--log-format";
    char log_format_value[] = "jsonl";
    char bad_log_level_arg[] = "--log-level=nope";
    char* log_argv[] = {arg0, log_arg, log_path_arg, log_level_arg, log_format_arg, log_format_value, bad_log_level_arg};
    const kin::HeadlessOptions log_options = kin::parse_headless_options(7, log_argv);
    assert(log_options.log_path == "kin.jsonl");
    assert(log_options.log_level && *log_options.log_level == kin::LogLevel::Debug);
    assert(log_options.log_format && *log_options.log_format == kin::LogFormat::JsonLines);
    assert(log_options.invalid_log_option);

    kin::GameInfo info{
        .id = "runtime-test",
        .title = "Runtime Test",
        .version = "0.1",
        .description = "Runtime metadata smoke test",
        .window = {
            .width = 320,
            .height = 180,
            .logical_width = 160,
            .logical_height = 90,
            .integer_scale = true,
            .resizable = true,
            .borderless = true,
        },
        .tags = {"test", "headless"},
    };
    kin::set_field(info, "mode", "metadata");
    kin::set_field(info, "mode", "runtime");
    assert(kin::field(info, "mode") != nullptr);
    assert(*kin::field(info, "mode") == "runtime");

    kin::WindowedAppConfig metadata_window = kin::window_config(info);
    assert(metadata_window.title == "Runtime Test");
    assert(metadata_window.width == 320);
    assert(metadata_window.logical_height == 90);
    assert(metadata_window.integer_scale);
    assert(metadata_window.resizable);
    assert(metadata_window.borderless);

    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);

    std::vector<kin::LogEvent> runtime_log_events;
    kin::set_logger_config({
        .min_level = kin::LogLevel::Debug,
        .format = kin::LogFormat::Text,
        .sdl_sink = false,
        .memory_events = &runtime_log_events,
    });

    int frames = 0;
    kin::run_windowed_app({
        .title = "runtime-test",
        .width = 64,
        .height = 64,
        .logical_width = 64,
        .logical_height = 64,
        .hidden = true,
        .borderless = true,
        .input_map = input,
    }, [&](kin::FrameContext& ctx) {
        assert(&ctx.input == &ctx.app.input());
        assert(ctx.window.borderless());
        assert(ctx.dt >= 0.0f);

        ++frames;
        if (frames == 2) {
            ctx.app.quit();
        }
    }, [](kin::FrameContext& ctx) {
        ctx.renderer.clear(0, 0, 0);
        ctx.renderer.present();
    });

    assert(frames == 2);
    assert(has_log_event(runtime_log_events, "runtime", "app initialized"));
    assert(has_log_event(runtime_log_events, "runtime", "window created"));
    assert(has_log_event(runtime_log_events, "runtime", "app loop stopped"));

    kin::InputMap scene_input;
    scene_input.bind("start", kin::Key::Enter);

    kin::SceneManager scenes;
    scenes.push(std::make_unique<RuntimeActionScene>());

    std::ostringstream actions;
    kin::run_scene_app({
        .window = {
            .title = "runtime-scene-test",
            .width = 64,
            .height = 64,
            .logical_width = 64,
            .logical_height = 64,
            .input_map = scene_input,
        },
        .headless = {
            .enabled = true,
            .list_actions = true,
        },
        .action_output = &actions,
    }, scenes);

    const std::string report = actions.str();
    assert(report.find("start: Start [Enter]") != std::string::npos);

    std::ostringstream metadata;
    kin::run_scene_app({
        .window = kin::window_config(info),
        .headless = {
            .enabled = true,
            .print_game_info = true,
        },
        .game = &info,
        .game_info_output = &metadata,
    }, scenes);

    const std::string metadata_report = metadata.str();
    assert(metadata_report.find("id: runtime-test") != std::string::npos);
    assert(metadata_report.find("mode: runtime") != std::string::npos);
    assert(metadata_report.find("borderless") != std::string::npos);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-runtime-tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path info_path = dir / "game.kininfo";
    assert(kin::save_game_info(info, info_path));
    const kin::GameInfo loaded_info = kin::load_game_info(info_path);
    assert(loaded_info.id == "runtime-test");
    assert(loaded_info.title == "Runtime Test");
    assert(loaded_info.version == "0.1");
    assert(loaded_info.description == "Runtime metadata smoke test");
    assert(loaded_info.window.width == 320);
    assert(loaded_info.window.height == 180);
    assert(loaded_info.window.logical_width == 160);
    assert(loaded_info.window.logical_height == 90);
    assert(loaded_info.window.integer_scale);
    assert(loaded_info.window.resizable);
    assert(loaded_info.window.borderless);
    assert(loaded_info.tags.size() == 2);
    assert(loaded_info.tags[0] == "test");
    assert(loaded_info.tags[1] == "headless");
    assert(kin::field(loaded_info, "mode") != nullptr);
    assert(*kin::field(loaded_info, "mode") == "runtime");

    kin::RuntimeDebugOverlay overlay;
    overlay.record("update", 3.0);
    overlay.record("update", 1.0);
    overlay.record("update", 2.0);
    const kin::DebugTimingStats stats = overlay.stats("update");
    assert(stats.last_ms == 2.0);
    assert(stats.median_ms == 2.0);
    assert(stats.p99_ms == 3.0);
    assert(overlay.stats("missing").last_ms == 0.0);
    assert(kin::key_name(kin::Key::F2) == std::string_view{"F2"});
    assert(kin::key_name(kin::Key::F12) == std::string_view{"F12"});
    overlay.start_capture();
    assert(overlay.capture_active());
    overlay.record("render.scene", 0.5);
    overlay.stop_capture();
    assert(!overlay.capture_active());
    assert(overlay.capture_available());

    const kin::u64 default_pass_mask = kin::render_pass_mask::world |
        kin::render_pass_mask::effects |
        kin::render_pass_mask::ui |
        kin::render_pass_mask::debug;
    assert(overlay.options().render_pass_mask() == default_pass_mask);
    assert(overlay.options().texture_batching_enabled);
    overlay.options().pass_world_enabled = false;
    assert((overlay.options().render_pass_mask() & kin::render_pass_mask::world) == 0);
    assert((overlay.options().render_pass_mask() & kin::render_pass_mask::effects) != 0);

    // JsonWriter: nesting, escaping, and number formatting.
    {
        std::ostringstream json_out;
        kin::JsonWriter json(json_out, false);
        json.begin_object();
        json.field("name", "a\"b\\c\n");
        json.field("count", 3);
        json.field("flag", true);
        json.key("items").begin_array();
        json.value(1);
        json.value(2);
        json.end_array();
        json.end_object();
        const std::string json_text = json_out.str();
        const std::string expected = R"({"name":"a\"b\\c\n","count":3,"flag":true,"items":[1,2]})";
        assert(json_text == expected);
    }

    // Seed threading + report emission: a fixed seed produces a stable observed
    // value and a well-formed "ok" report.
    {
        kin::SceneManager seeded;
        seeded.push(std::make_unique<SeededReportScene>());
        std::ostringstream report;
        const int code = kin::run_scene_app({
            .window = {.title = "seeded", .width = 64, .height = 64},
            .headless = {.enabled = true, .frames = 3, .seed = 99},
            .report_output = &report,
        }, seeded);
        assert(code == 0);
        const std::string text = report.str();
        assert(text.find("\"status\": \"ok\"") != std::string::npos);
        assert(text.find("\"seed\": 99") != std::string::npos);
        assert(text.find("\"frames\": 3") != std::string::npos);
        assert(text.find("\"name\": \"Seeded\"") != std::string::npos);

        const kin::u32 expected = kin::rng_u32(kin::make_key(99));
        assert(text.find("\"observed\": " + std::to_string(expected)) != std::string::npos);
    }

    // Runtime profile output is available for every game that uses
    // run_scene_app; no game-specific wiring is required.
    {
        kin::SceneManager profiled;
        profiled.push(std::make_unique<SeededReportScene>());
        std::ostringstream profile_json;
        std::ostringstream profile_text;
        const int code = kin::run_scene_app({
            .window = {.title = "profiled", .width = 64, .height = 64},
            .headless = {.profile = true, .frames = 3, .seed = 1},
            .profile_json_output = &profile_json,
            .profile_text_output = &profile_text,
        }, profiled);
        assert(code == 0);
        const std::string json_text = profile_json.str();
        const std::string text = profile_text.str();
        assert(json_text.find("\"schema\": \"kin.profile/1\"") != std::string::npos);
        assert(json_text.find("\"name\": \"frame\"") != std::string::npos);
        assert(json_text.find("\"name\": \"update\"") != std::string::npos);
        assert(text.find("runtime profile profiled") != std::string::npos);
        assert(text.find("render.scene") != std::string::npos);
    }

    // Failure contract: a scene that fails stops the run early, reports the
    // reason, and yields a non-zero exit code.
    {
        kin::SceneManager failing;
        failing.push(std::make_unique<FailingScene>());
        std::ostringstream report;
        const int code = kin::run_scene_app({
            .window = {.title = "failing", .width = 64, .height = 64},
            .headless = {.enabled = true, .frames = 10},
            .report_output = &report,
        }, failing);
        assert(code == 1);
        const std::string text = report.str();
        assert(text.find("\"status\": \"failed\"") != std::string::npos);
        assert(text.find("\"failure_reason\": \"boom\"") != std::string::npos);
        // Failed on frame 2, so the run stopped early rather than running all 10.
        assert(text.find("\"frames\": 2") != std::string::npos);
    }

    return 0;
}
