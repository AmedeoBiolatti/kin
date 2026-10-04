#include <kin/runtime/scene_app.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <kin/assets/file_watcher.hpp>
#include <kin/core/json.hpp>
#include <kin/core/profile.hpp>
#include <kin/core/rng.hpp>
#include <kin/platform/log.hpp>
#include <kin/platform/user_data.hpp>
#include <kin/runtime/debug_overlay.hpp>
#ifdef KIN_ENABLE_RENDER_PROBE
#include <kin/runtime/render_probe.hpp>
#endif
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/scene_server.hpp>
#ifdef KIN_ENABLE_DETERMINISM_CHECK
#include <kin/runtime/state_hash.hpp>
#endif

#include "runtime_internal.hpp"
#include <string_view>

namespace kin {
namespace {

bool parse_i32(std::string_view text, i32& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_u64(std::string_view text, u64& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

void write_run_report(std::ostream& out,
                      const RunReport& report,
                      u64 seed,
                      i32 frames,
                      const SceneManager& scenes) {
    JsonWriter json(out);
    json.begin_object();
    json.field("schema", "kin.run_report/1");
    json.field("status", report.failed ? "failed" : "ok");
    if (report.failed) {
        json.field("failure_reason", report.failure_reason);
    }
    json.field("seed", seed);
    json.field("frames", frames);
    json.key("scenes").begin_array();
    for (i32 i = 0; i < scenes.depth(); ++i) {
        const Scene* scene = scenes.at(i);
        if (!scene) {
            continue;
        }
        json.begin_object();
        json.field("name", scene->name());
        json.key("state").begin_object();
        scene->write_report(json);
        json.end_object();
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out << '\n';
}

void write_bindings(std::ostream& out, const std::vector<InputBinding>& bindings) {
    if (bindings.empty()) {
        out << "unbound";
        return;
    }

    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << binding_name(bindings[i]);
    }
}

void configure_runtime_logging(const HeadlessOptions& options) {
    reset_logger_config_from_environment();
    LoggerConfig config = logger_config();

    if (!options.log_path.empty()) {
        config.file_path = options.log_path;
    }
    if (options.log_level) {
        config.min_level = *options.log_level;
    }
    if (options.log_format) {
        config.format = *options.log_format;
    }
    set_logger_config(std::move(config));

    if (options.invalid_log_option) {
        KIN_LOG_WARN("log", "invalid logging option, using default");
    }
}

} // namespace

HeadlessOptions parse_headless_options(int argc, char** argv) {
    HeadlessOptions options;
    bool invalid_log_option = false;
    options.args.assign(argv, argv + argc);

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--headless") {
            options.enabled = true;
        } else if (arg == "--list-actions") {
            options.enabled = true;
            options.list_actions = true;
        } else if (arg == "--game-info" || arg == "--list-info") {
            options.enabled = true;
            options.print_game_info = true;
        } else if (arg == "--overdraw-view") {
            options.overdraw_view = true;
        } else if (arg.starts_with("--screenshot=")) {
            options.screenshot_path = arg.substr(13);
        } else if (arg == "--profile-render") {
            options.enabled = true;
            options.profile_render = true;
        } else if (arg == "--profile") {
            options.profile = true;
        } else if (arg == "--profile-lines") {
            options.profile = true;
            options.profile_lines = true;
        } else if (arg.starts_with("--profile-json=")) {
            options.profile = true;
            options.profile_json_path = arg.substr(15);
        } else if (arg == "--profile-json" && i + 1 < argc) {
            options.profile = true;
            options.profile_json_path = argv[i + 1];
            ++i;
        } else if (arg.starts_with("--profile-text=")) {
            options.profile = true;
            options.profile_text_path = arg.substr(15);
        } else if (arg == "--profile-text" && i + 1 < argc) {
            options.profile = true;
            options.profile_text_path = argv[i + 1];
            ++i;
        } else if (arg.starts_with("--frames=")) {
            i32 frames = 0;
            if (parse_i32(arg.substr(9), frames)) {
                options.frames = frames;
            }
        } else if (arg == "--frames" && i + 1 < argc) {
            i32 frames = 0;
            if (parse_i32(argv[i + 1], frames)) {
                options.frames = frames;
                ++i;
            }
        } else if (arg.starts_with("--seed=")) {
            u64 seed = 0;
            if (parse_u64(arg.substr(7), seed)) {
                options.seed = seed;
            }
        } else if (arg == "--seed" && i + 1 < argc) {
            u64 seed = 0;
            if (parse_u64(argv[i + 1], seed)) {
                options.seed = seed;
                ++i;
            }
        } else if (arg.starts_with("--report=")) {
            options.enabled = true;
            options.report_path = arg.substr(9);
        } else if (arg == "--report" && i + 1 < argc) {
            options.enabled = true;
            options.report_path = argv[i + 1];
            ++i;
        } else if (arg == "--probe-render") {
            options.enabled = true;
            options.probe_render_path = "-";
        } else if (arg.starts_with("--probe-render=")) {
            options.enabled = true;
            options.probe_render_path = arg.substr(15);
        } else if (arg == "--probe-fail") {
            options.enabled = true;
            options.probe_fail = true;
        } else if (arg.starts_with("--probe-tile=")) {
            i32 tile = 0;
            if (parse_i32(arg.substr(13), tile) && tile > 0) {
                options.enabled = true;
                options.probe_tile_size = tile;
            }
        } else if (arg == "--check-determinism") {
            options.check_determinism = true;
        } else if (arg.starts_with("--check-determinism=")) {
            options.check_determinism = true;
            options.determinism_path = arg.substr(20);
        } else if (arg == "--state-lockstep") {
            options.enabled = true;
            options.state_lockstep = true;
        } else if (arg == "--server") {
            options.server = true;
        } else if (arg.starts_with("--server-mode=")) {
            options.server = true;
            options.server_mode = arg.substr(14);
        } else if (arg.starts_with("--server-transport=")) {
            options.server = true;
            options.server_transport = arg.substr(19);
        } else if (arg.starts_with("--port=")) {
            i32 port = 0;
            if (parse_i32(arg.substr(7), port)) {
                options.server_port = port;
            }
        } else if (arg.starts_with("--log=")) {
            options.log_path = arg.substr(6);
        } else if (arg == "--log" && i + 1 < argc) {
            options.log_path = argv[i + 1];
            ++i;
        } else if (arg.starts_with("--log-level=")) {
            LogLevel level{};
            if (parse_log_level(arg.substr(12), level)) {
                options.log_level = level;
            } else {
                invalid_log_option = true;
            }
        } else if (arg.starts_with("--max-fps=") || (arg == "--max-fps" && i + 1 < argc)) {
            const std::string text = arg == "--max-fps" ? std::string(argv[++i]) : std::string(arg.substr(10));
            char* end = nullptr;
            const f32 fps = std::strtof(text.c_str(), &end);
            if (end != text.c_str() && *end == '\0' && fps >= 0.0f) {
                options.max_fps = fps;
            }
        } else if (arg == "--log-level" && i + 1 < argc) {
            LogLevel level{};
            if (parse_log_level(argv[i + 1], level)) {
                options.log_level = level;
            } else {
                invalid_log_option = true;
            }
            ++i;
        } else if (arg.starts_with("--log-format=")) {
            LogFormat format{};
            if (parse_log_format(arg.substr(13), format)) {
                options.log_format = format;
            } else {
                invalid_log_option = true;
            }
        } else if (arg == "--log-format" && i + 1 < argc) {
            LogFormat format{};
            if (parse_log_format(argv[i + 1], format)) {
                options.log_format = format;
            } else {
                invalid_log_option = true;
            }
            ++i;
        }
    }

    options.invalid_log_option = invalid_log_option;
    return options;
}

void write_available_actions(std::ostream& out, const std::vector<AvailableInputAction>& actions) {
    out << "actions\n";
    for (const AvailableInputAction& action : actions) {
        out << "- " << action.name;
        if (!action.label.empty() && action.label != action.name) {
            out << ": " << action.label;
        }
        out << " [";
        write_bindings(out, action.bindings);
        out << "]";
        if (!action.description.empty()) {
            out << " - " << action.description;
        }
        out << '\n';
    }
}

void write_profile_row(std::ostream& out, const RuntimeDebugOverlay& overlay, std::string_view name) {
    const DebugTimingStats stats = overlay.stats(name);
    if (stats.last_ms == 0.0 && stats.median_ms == 0.0 && stats.p99_ms == 0.0) {
        return;
    }
    out << std::left << std::setw(30) << name
        << " last=" << std::right << std::setw(7) << std::fixed << std::setprecision(3) << stats.last_ms
        << " median=" << std::setw(7) << stats.median_ms
        << " p99=" << std::setw(7) << stats.p99_ms
        << " ms\n";
}

void write_render_profile_report(std::ostream& out, const RuntimeDebugOverlay& overlay, i32 frames) {
    out << "render profile";
    if (frames > 0) {
        out << " (" << frames << " frames)";
    }
    out << '\n';

    constexpr std::array rows = {
        std::string_view{"update"},
        std::string_view{"frame.update_total"},
        std::string_view{"frame.update_steps"},
        std::string_view{"app.raw_frame"},
        std::string_view{"app.clamped_frame"},
        std::string_view{"app.accumulator_before"},
        std::string_view{"app.accumulator_after"},
        std::string_view{"app.hit_max_steps"},
        std::string_view{"app.pacing_wait"},
        std::string_view{"render.scene"},
        std::string_view{"render.scene.camera"},
        std::string_view{"render.scene.view"},
        std::string_view{"render.scene.queue_reset"},
        std::string_view{"render.scene.propagate_transforms"},
        std::string_view{"render.scene.static_rebuild"},
        std::string_view{"render.scene.collect_dynamic"},
        std::string_view{"render.scene.collect_world"},
        std::string_view{"render.scene.count_unculled"},
        std::string_view{"render.scene.sort_dynamic"},
        std::string_view{"render.scene.flush_world"},
        std::string_view{"render.scene.ecs_ui"},
        std::string_view{"present"},
        std::string_view{"present.flush"},
        std::string_view{"present.backend"},
        std::string_view{"gpu.wait"},
        std::string_view{"gpu.frame"},
        std::string_view{"frame.render_total"},
        std::string_view{"frame.unaccounted"},
        std::string_view{"frame"},
    };
    for (std::string_view row : rows) {
        write_profile_row(out, overlay, row);
    }
}

int run_scene_app(const SceneAppConfig& config, SceneManager& scenes) {
    configure_runtime_logging(config.headless);
    KIN_LOG_INFO_F("runtime",
                   "scene app starting",
                   (LogFields{
                       {.name = "title", .value = std::string{config.window.title}},
                       {.name = "headless", .value = config.headless.enabled ? "true" : "false"},
                       {.name = "server", .value = config.headless.server ? "true" : "false"},
                       {.name = "frames", .value = std::to_string(config.headless.frames)},
                       {.name = "seed", .value = std::to_string(config.headless.seed)},
                   }));

    if (config.headless.check_determinism && !config.headless.state_lockstep) {
#ifdef KIN_ENABLE_DETERMINISM_CHECK
        return runtime_detail::run_determinism_check(config);
#else
        KIN_LOG_ERROR("runtime", "determinism check requested, but this build has KIN_ENABLE_DETERMINISM_CHECK off");
        return 1;
#endif
    }

    if (config.headless.server) {
        ServerConfig server{
            .window = config.window,
            .transport = parse_server_transport(config.headless.server_transport),
            .mode = parse_server_mode(config.headless.server_mode),
            .seed = config.headless.seed,
            .port = static_cast<u16>(config.headless.server_port),
            .render = config.render_headless,
            .game = config.game,
            .reset_scenes = config.reset_scenes,
            .asset_server = config.asset_server,
        };
        return run_scene_server(server, scenes);
    }

    if (config.headless.print_game_info) {
        std::ostream& out = config.game_info_output ? *config.game_info_output : std::cout;
        if (config.game) {
            write_game_info(out, *config.game);
        } else {
            GameInfo fallback;
            fallback.title = std::string(config.window.title);
            fallback.window = {
                .width = config.window.width,
                .height = config.window.height,
                .logical_width = config.window.logical_width,
                .logical_height = config.window.logical_height,
                .integer_scale = config.window.integer_scale,
                .resizable = config.window.resizable,
                .borderless = config.window.borderless,
            };
            fallback.input_map = config.window.input_map;
            write_game_info(out, fallback);
        }
        return 0;
    }

    WindowedAppConfig window = config.window;
    if (config.headless.max_fps) {
        window.max_fps = *config.headless.max_fps;
    }
    // --profile measures the run as it is: windowed unless --headless (which
    // then defaults to a 600-frame pass). --profile-render is always a pass.
    if (config.headless.enabled || config.headless.list_actions || config.headless.profile_render) {
        window.mode = AppMode::Headless;
        window.hidden = true;
        window.max_frames = config.headless.frames > 0 ? config.headless.frames :
            ((config.headless.profile_render || config.headless.profile) ? 600 : 1);
    }

    RuntimeDebugOverlay debug_overlay;
    ProfileSession profile_session;
    const bool profile_enabled = config.headless.profile ||
        config.profile_json_output ||
        config.profile_text_output ||
        !config.headless.profile_json_path.empty() ||
        !config.headless.profile_text_path.empty();
    if (profile_enabled) {
        profile_session.start(std::string{config.window.title});
        if (config.headless.profile_lines) {
            set_current_profile_session(&profile_session);
        }
    }
    RunReport run_report;
    const bool probe_requested = config.probe_output != nullptr ||
        !config.headless.probe_render_path.empty() ||
        config.headless.probe_fail ||
        config.headless.probe_tile_size > 0;
#ifdef KIN_ENABLE_RENDER_PROBE
    std::optional<RenderProbe> probe;
    std::vector<u8> probe_pixels;
    // Records who draws what, so probe events can name their culprits.
    DrawTrace draw_trace;
    struct ActiveTraceReset {
        bool armed = false;
        ~ActiveTraceReset() {
            if (armed) {
                set_active_draw_trace(nullptr);
            }
        }
    } active_trace_reset;
    if (probe_requested) {
        RenderProbeConfig probe_config;
        if (config.headless.probe_tile_size > 0) {
            probe_config.tile_size = config.headless.probe_tile_size;
        }
        probe.emplace(probe_config);
        probe->set_draw_trace(&draw_trace);
        set_active_draw_trace(&draw_trace);
        active_trace_reset.armed = true;
    }
#else
    if (probe_requested) {
        KIN_LOG_WARN("runtime", "render probe requested, but this build has KIN_ENABLE_RENDER_PROBE off");
    }
#endif
#ifdef KIN_ENABLE_DETERMINISM_CHECK
    StateCoverage lockstep_coverage;
#endif
    const RngKey root_key = make_key(config.headless.seed);
    const bool want_report = config.report_output != nullptr || !config.headless.report_path.empty();
    std::string report_snapshot;
    i32 frames_run = 0;
    const bool log_frame_stats = [] {
        const char* value = std::getenv("KIN_LOG_FRAME_STATS");
        return value && value[0] == '1';
    }();
    i32 logged_frames = 0;
    const i32 windowed_frame_limit = window.mode == AppMode::Headless ? 0 : config.headless.frames;
    i32 frames_rendered = 0;
    const auto ms_since = [](std::chrono::steady_clock::time_point start) {
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<f64, std::milli>(end - start).count();
    };
    const auto ns_since = [](std::chrono::steady_clock::time_point start) {
        const auto end = std::chrono::steady_clock::now();
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    };

    // Scene-owned textures must be destroyed while the renderer/device is still
    // alive. Capture the report first because the shutdown tears down the scene
    // stack before run_windowed_app destroys its backend.
    // The pipelines an earlier windowed run made, made again while this one
    // loads; this run's, kept for the next.
    std::filesystem::path pipeline_file;
    if (window.mode != AppMode::Headless) {
        if (config.pipeline_record_path) {
            pipeline_file = *config.pipeline_record_path;
        } else {
            std::string folder;
            for (const char c : std::string_view{config.window.title}) {
                folder += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '-';
            }
            pipeline_file = user_data_dir("kin") / (folder.empty() ? std::string{"game"} : folder) / "pipelines.txt";
        }
    }
    bool pipelines_prewarmed = false;
    debug_overlay.options().overdraw_view = config.headless.overdraw_view;
    const auto user_shutdown = window.shutdown;
    window.shutdown = [&, user_shutdown](FrameContext& frame) {
#ifdef KIN_ENABLE_DETERMINISM_CHECK
        if (config.headless.state_lockstep) {
            runtime_detail::state_lockstep_finish(lockstep_coverage);
        }
#endif
#ifdef KIN_ENABLE_RENDER_PROBE
        if (probe && config.headless.probe_fail && !probe->events().empty()) {
            const RenderProbeEvent& first = probe->events().front();
            std::ostringstream reason;
            reason << "render probe found " << probe->events().size() << " event(s); first: "
                   << render_probe_event_kind_name(first.kind) << " at frame " << first.first_frame
                   << " in " << first.rect.w << "x" << first.rect.h << "+" << first.rect.x << "+" << first.rect.y;
            run_report.fail(reason.str());
        }
#endif
        if (!config.headless.screenshot_path.empty() && !frame.renderer.save_png(config.headless.screenshot_path)) {
            KIN_LOG_ERROR_F("runtime", "screenshot not saved",
                            (LogFields{{.name = "path", .value = config.headless.screenshot_path}}));
        }
        if (!pipeline_file.empty()) {
            std::error_code error;
            std::filesystem::create_directories(pipeline_file.parent_path(), error);
            std::ofstream out{pipeline_file};
            out << frame.renderer.pipeline_record();
        }
        if (want_report) {
            std::ostringstream report;
            write_run_report(report, run_report, config.headless.seed, frames_run, scenes);
            report_snapshot = report.str();
        }
        if (user_shutdown) {
            user_shutdown(frame);
        }
        SceneContext shutdown_ctx{
            .app = frame.app,
            .window = frame.window,
            .renderer = frame.renderer,
            .input = frame.input,
            .scenes = scenes,
            .dt = 0.0f,
            .alpha = 0.0f,
            .is_top = true,
            .debug_options = &debug_overlay.options(),
            .rng = root_key,
            .report = &run_report,
        };
        scenes.shutdown(shutdown_ctx);
    };
    const auto record_profile = [&](std::string_view name,
                                    std::string_view category,
                                    std::chrono::steady_clock::time_point start,
                                    std::source_location location = std::source_location::current()) {
        if (profile_enabled) {
            profile_session.record(name, category, ns_since(start), location);
        }
    };
    const auto record_profile_value = [&](std::string_view name,
                                          std::string_view category,
                                          f64 ms,
                                          std::source_location location = std::source_location::current()) {
        if (profile_enabled) {
            profile_session.record(name, category, static_cast<u64>(std::max(0.0, ms) * 1'000'000.0), location);
        }
    };

    auto frame_start = std::chrono::steady_clock::now();
    u64 gpu_frames_sampled = 0; // the backend's count at the last gpu.frame recorded
    bool frame_prepared = false;
    f64 frame_update_total_ms = 0.0;
    i32 frame_update_steps = 0;
    const auto prepare_frame = [&](FrameContext& ctx) {
        if (frame_prepared) {
            return;
        }
        frame_start = std::chrono::steady_clock::now();
        frame_prepared = true;
        frame_update_total_ms = 0.0;
        frame_update_steps = 0;
        debug_overlay.begin_frame();
        ctx.renderer.reset_backend_stats();
        if (ctx.input.frame_pressed(Key::F1)) {
            debug_overlay.toggle();
        }
        if (ctx.input.frame_pressed(Key::F2)) {
            debug_overlay.toggle_capture();
        }
        // Scene-side timing/counter recording is not free (games may walk system
        // snapshots and emit dozens of counters per phase), so only install the
        // sink when something will actually consume it.
        if (debug_overlay.visible() || debug_overlay.capture_active() ||
            config.headless.profile || config.headless.profile_render) {
            debug_overlay.options().record_timing = [&](std::string_view name, f64 ms) {
                debug_overlay.record(name, ms);
            };
            debug_overlay.options().record_status = [&](std::string_view name, std::string value) {
                debug_overlay.set_status(name, std::move(value));
            };
        } else {
            debug_overlay.options().record_timing = {};
            debug_overlay.options().record_status = {};
        }
        if (config.headless.profile_render || config.headless.profile || debug_overlay.capture_active()) {
            debug_overlay.options().detailed_render_timings_enabled = true;
        }
        ctx.renderer.set_texture_batching_enabled(debug_overlay.options().texture_batching_enabled);
        ctx.renderer.set_overdraw_view(debug_overlay.options().overdraw_view);
    };
    const auto make_scene_context = [&](FrameContext& ctx) {
        return kin::SceneContext{
            .app = ctx.app,
            .window = ctx.window,
            .renderer = ctx.renderer,
            .input = ctx.input,
            .scenes = scenes,
            .dt = ctx.dt,
            .alpha = ctx.alpha,
            .is_top = true,
            .debug_options = &debug_overlay.options(),
            .rng = root_key,
            .report = &run_report,
        };
    };

    run_windowed_app(window, [&](FrameContext& ctx) {
        if (!pipelines_prewarmed) {
            pipelines_prewarmed = true;
            if (!pipeline_file.empty()) {
                std::ifstream in{pipeline_file};
                const std::string record{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
                ctx.renderer.prewarm_pipelines(record);
            }
        }
        prepare_frame(ctx);
        ++frames_run;
        if (profile_enabled) {
            profile_session.set_frame(frames_run);
        }
        kin::SceneContext scene_ctx = make_scene_context(ctx);
        runtime_detail::pump_scene_assets(config.asset_server, window.mode != AppMode::Headless);
        if (config.file_watcher && window.mode != AppMode::Headless) {
            config.file_watcher->poll();
        }
        const auto update_start = std::chrono::steady_clock::now();
        scenes.update(scene_ctx);
        const f64 update_ms = ms_since(update_start);
        frame_update_total_ms += update_ms;
        ++frame_update_steps;
        debug_overlay.record("update", update_ms);
        record_profile("update", "runtime", update_start);
#ifdef KIN_ENABLE_DETERMINISM_CHECK
        if (config.headless.state_lockstep && !runtime_detail::state_lockstep_step(scenes, frames_run, lockstep_coverage)) {
            ctx.app.quit();
        }
#endif

        if (run_report.failed) {
            KIN_LOG_ERROR_F("runtime",
                            "run report failed",
                            (LogFields{{.name = "reason", .value = run_report.failure_reason}}));
            ctx.app.quit();
        }

        if (config.headless.list_actions) {
            std::ostream& out = config.action_output ? *config.action_output : std::cout;
            write_available_actions(out, scenes.available_actions(ctx.input.map()));
            ctx.app.quit();
            const f64 frame_ms = ms_since(frame_start);
            debug_overlay.record("frame.update_total", frame_update_total_ms);
            debug_overlay.record("frame.update_steps", static_cast<f64>(frame_update_steps));
            debug_overlay.record("frame.unaccounted", std::max(0.0, frame_ms - frame_update_total_ms));
            debug_overlay.record("frame", frame_ms);
            record_profile_value("frame.update_total", "runtime", frame_update_total_ms);
            record_profile_value("frame.update_steps", "runtime", static_cast<f64>(frame_update_steps));
            record_profile_value("frame.unaccounted", "runtime", std::max(0.0, frame_ms - frame_update_total_ms));
            record_profile("frame", "runtime", frame_start);
            frame_prepared = false;
            return;
        }
    }, [&](FrameContext& ctx) {
        prepare_frame(ctx);
        kin::SceneContext scene_ctx = make_scene_context(ctx);
        f64 render_scene_ms = 0.0;
        f64 overlay_ms = 0.0;
        f64 present_ms = 0.0;
        if (!ctx.app.headless() || config.render_headless || config.headless.profile_render || config.headless.profile ||
            probe_requested) {
#ifdef KIN_ENABLE_RENDER_PROBE
            draw_trace.begin_frame();
#endif
            const auto render_start = std::chrono::steady_clock::now();
            scenes.render(scene_ctx);
            render_scene_ms = ms_since(render_start);
            debug_overlay.record("render.scene", render_scene_ms);
            record_profile("render.scene", "runtime", render_start);
#ifdef KIN_ENABLE_RENDER_PROBE
            // The scene alone: before the debug overlay, which is not the game's.
            if (probe) {
                const Vec2i output = ctx.renderer.output_size();
                const Vec2f a = ctx.renderer.window_to_logical({0.0f, 0.0f});
                const Vec2f b = ctx.renderer.window_to_logical({static_cast<f32>(output.x), static_cast<f32>(output.y)});
                Vec2i size;
                if (ctx.renderer.read_rgba({a.x, a.y, b.x - a.x, b.y - a.y}, probe_pixels, size)) {
                    probe->add_frame(probe_pixels, size, draw_trace.draws());
                }
            }
#endif

            const auto overlay_start = std::chrono::steady_clock::now();
            debug_overlay.render(ctx.input, ctx.renderer);
            overlay_ms = ms_since(overlay_start);
            debug_overlay.record("render.debug_overlay", overlay_ms);
            record_profile("render.debug_overlay", "runtime", overlay_start);

            // GPU frame timing costs a fence and a waiting thread, so only when
            // someone reads it.
            ctx.renderer.set_gpu_timing_enabled(profile_enabled || log_frame_stats ||
                                                config.headless.profile_render || debug_overlay.visible());
            const auto present_start = std::chrono::steady_clock::now();
            ctx.renderer.present();
            present_ms = ms_since(present_start);
            debug_overlay.record("present", present_ms);
            record_profile("present", "runtime", present_start);
            const RendererBackendStats present_stats = ctx.renderer.backend_stats();
            debug_overlay.record("present.flush", present_stats.last_present_flush_ms);
            debug_overlay.record("present.backend", present_stats.last_present_backend_ms);
            if (profile_enabled) {
                profile_session.record("present.flush", "runtime", static_cast<u64>(present_stats.last_present_flush_ms * 1'000'000.0));
                profile_session.record("present.backend", "runtime", static_cast<u64>(present_stats.last_present_backend_ms * 1'000'000.0));
            }
            if (ctx.renderer.backend_name() == "SDL_GPU") {
                debug_overlay.record("gpu.wait", present_stats.last_gpu_wait_ms);
                record_profile_value("gpu.wait", "runtime", present_stats.last_gpu_wait_ms);
                // Not a time: how many times each screen pixel was shaded.
                debug_overlay.record("render.overdraw", present_stats.last_overdraw);
                record_profile_value("render.overdraw", "runtime", present_stats.last_overdraw);
                // Only new samples: a frame without one would repeat the last.
                if (present_stats.gpu_frames_sampled != gpu_frames_sampled) {
                    gpu_frames_sampled = present_stats.gpu_frames_sampled;
                    debug_overlay.record("gpu.frame", present_stats.last_gpu_frame_ms);
                    record_profile_value("gpu.frame", "runtime", present_stats.last_gpu_frame_ms);
                }
                for (const GpuScopeTiming& scope : ctx.renderer.take_gpu_scope_timings()) {
                    const std::string name = "gpu." + scope.name;
                    debug_overlay.record(name, scope.ms);
                    record_profile_value(name, "runtime", scope.ms);
                    // Not a time: the scope's share of render.overdraw.
                    debug_overlay.record("overdraw." + scope.name, scope.overdraw);
                    record_profile_value("overdraw." + scope.name, "runtime", scope.overdraw);
                }
            }
        }

        const f64 frame_ms = ms_since(frame_start);
        const f64 render_total_ms = render_scene_ms + overlay_ms + present_ms;
        const f64 accounted_ms = frame_update_total_ms + render_total_ms;
        const f64 unaccounted_ms = std::max(0.0, frame_ms - accounted_ms);
        const AppFrameStats& app_frame = ctx.app.frame_stats();
        debug_overlay.record("frame.update_total", frame_update_total_ms);
        debug_overlay.record("frame.update_steps", static_cast<f64>(frame_update_steps));
        debug_overlay.record("app.raw_frame", static_cast<f64>(app_frame.raw_frame_time) * 1000.0);
        debug_overlay.record("app.pacing_wait", static_cast<f64>(app_frame.pacing_wait) * 1000.0);
        debug_overlay.record("app.clamped_frame", static_cast<f64>(app_frame.clamped_frame_time) * 1000.0);
        debug_overlay.record("app.accumulator_before", static_cast<f64>(app_frame.accumulator_before_update) * 1000.0);
        debug_overlay.record("app.accumulator_after", static_cast<f64>(app_frame.accumulator_after_update) * 1000.0);
        debug_overlay.record("app.hit_max_steps", app_frame.hit_max_steps ? 1.0 : 0.0);
        debug_overlay.record("frame.render_total", render_total_ms);
        debug_overlay.record("frame.unaccounted", unaccounted_ms);
        debug_overlay.record("frame", frame_ms);
        record_profile_value("frame.update_total", "runtime", frame_update_total_ms);
        record_profile_value("frame.update_steps", "runtime", static_cast<f64>(frame_update_steps));
        record_profile_value("app.raw_frame", "runtime", static_cast<f64>(app_frame.raw_frame_time) * 1000.0);
        record_profile_value("app.clamped_frame", "runtime", static_cast<f64>(app_frame.clamped_frame_time) * 1000.0);
        record_profile_value("app.accumulator_before", "runtime", static_cast<f64>(app_frame.accumulator_before_update) * 1000.0);
        record_profile_value("app.accumulator_after", "runtime", static_cast<f64>(app_frame.accumulator_after_update) * 1000.0);
        record_profile_value("app.hit_max_steps", "runtime", app_frame.hit_max_steps ? 1.0 : 0.0);
        record_profile_value("app.pacing_wait", "runtime", static_cast<f64>(app_frame.pacing_wait) * 1000.0);
        record_profile_value("frame.render_total", "runtime", render_total_ms);
        record_profile_value("frame.unaccounted", "runtime", unaccounted_ms);
        record_profile("frame", "runtime", frame_start);
        if (log_frame_stats && ++logged_frames % 120 == 0) {
            // fps from the time between frames, which includes waiting on the GPU
            // and the frame rate cap; frame_ms is the work alone.
            const DebugTimingStats interval = debug_overlay.stats("app.raw_frame");
            const DebugTimingStats frame = debug_overlay.stats("frame");
            const DebugTimingStats present = debug_overlay.stats("present.backend");
            const DebugTimingStats pacing = debug_overlay.stats("app.pacing_wait");
            const DebugTimingStats gpu_wait = debug_overlay.stats("gpu.wait");
            const DebugTimingStats gpu_frame = debug_overlay.stats("gpu.frame");
            const f64 fps = interval.median_ms > 0.0001 ? 1000.0 / interval.median_ms : 0.0;
            KIN_LOG_INFO_F("runtime",
                           "frame stats",
                           (LogFields{
                               {.name = "fps", .value = std::to_string(fps)},
                               {.name = "frame_interval_ms", .value = std::to_string(interval.median_ms)},
                               {.name = "frame_ms", .value = std::to_string(frame.median_ms)},
                               {.name = "pacing_wait_ms", .value = std::to_string(pacing.median_ms)},
                               {.name = "present_backend_ms", .value = std::to_string(present.median_ms)},
                               {.name = "gpu_wait_ms", .value = std::to_string(gpu_wait.median_ms)},
                               {.name = "gpu_frame_ms", .value = std::to_string(gpu_frame.median_ms)},
                           }));
        }
        // A windowed run with --frames runs the real loop (real time, frame rate
        // cap) and stops after that many rendered frames.
        if (windowed_frame_limit > 0 && ++frames_rendered >= windowed_frame_limit) {
            ctx.app.quit();
        }
        frame_prepared = false;
    });

    if (config.headless.profile_render) {
        write_render_profile_report(std::cout, debug_overlay, window.max_frames);
    }

    if (profile_enabled) {
        profile_session.stop();
        if (config.headless.profile_lines) {
            set_current_profile_session(nullptr);
        }

        const auto ensure_parent = [](const std::string& path) {
            const std::filesystem::path file_path{path};
            const std::filesystem::path parent = file_path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
        };
        const auto write_text = [&](std::ostream& out) {
            write_profile_text(out, profile_session, 48, "runtime profile");
        };
        const auto write_json = [&](std::ostream& out) {
            write_profile_json(out, profile_session, "kin.profile/1");
        };

        if (config.profile_text_output) {
            write_text(*config.profile_text_output);
        } else if (config.headless.profile_text_path == "-") {
            write_text(std::cout);
        } else if (!config.headless.profile_text_path.empty()) {
            ensure_parent(config.headless.profile_text_path);
            std::ofstream file(config.headless.profile_text_path);
            if (!file) {
                KIN_LOG_ERROR_F("runtime",
                                "failed to open profile text path",
                                (LogFields{{.name = "path", .value = config.headless.profile_text_path}}));
                return 1;
            }
            write_text(file);
        } else if (config.headless.profile && config.headless.profile_json_path.empty() && !config.profile_json_output) {
            write_text(std::cout);
        }

        if (config.profile_json_output) {
            write_json(*config.profile_json_output);
        } else if (config.headless.profile_json_path == "-") {
            write_json(std::cout);
        } else if (!config.headless.profile_json_path.empty()) {
            ensure_parent(config.headless.profile_json_path);
            std::ofstream file(config.headless.profile_json_path);
            if (!file) {
                KIN_LOG_ERROR_F("runtime",
                                "failed to open profile json path",
                                (LogFields{{.name = "path", .value = config.headless.profile_json_path}}));
                return 1;
            }
            write_json(file);
        }
    }

#ifdef KIN_ENABLE_RENDER_PROBE
    if (probe) {
        const auto write_probe = [&](std::ostream& out) {
            JsonWriter json(out);
            probe->write_json(json);
            out << '\n';
        };
        KIN_LOG_INFO_F("runtime",
                       "render probe done",
                       (LogFields{
                           {.name = "frames", .value = std::to_string(probe->frame_count())},
                           {.name = "spikes", .value = std::to_string(probe->event_count(RenderProbeEventKind::Spike))},
                           {.name = "flickers", .value = std::to_string(probe->event_count(RenderProbeEventKind::Flicker))},
                       }));
        const std::string& path = config.headless.probe_render_path;
        if (config.probe_output) {
            write_probe(*config.probe_output);
        } else if (path == "-") {
            write_probe(std::cout);
        } else if (!path.empty()) {
            const std::filesystem::path parent = std::filesystem::path{path}.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            std::ofstream file(path);
            if (!file) {
                KIN_LOG_ERROR_F("runtime",
                                "failed to open render probe path",
                                (LogFields{{.name = "path", .value = path}}));
                return 1;
            }
            write_probe(file);
        }
    }
#endif

    if (want_report) {
        const auto emit_report = [&](std::ostream& out) {
            if (!report_snapshot.empty()) {
                out << report_snapshot;
            } else {
                write_run_report(out, run_report, config.headless.seed, frames_run, scenes);
            }
        };
        if (config.report_output) {
            emit_report(*config.report_output);
        } else if (config.headless.report_path == "-") {
            emit_report(std::cout);
        } else {
            std::ofstream file(config.headless.report_path);
            if (file) {
                emit_report(file);
            } else {
                KIN_LOG_ERROR_F("runtime",
                                "failed to open report path",
                                (LogFields{{.name = "path", .value = config.headless.report_path}}));
                return 1;
            }
        }
    }

    KIN_LOG_INFO_F("runtime",
                   "scene app stopped",
                   (LogFields{
                       {.name = "frames", .value = std::to_string(frames_run)},
                       {.name = "failed", .value = run_report.failed ? "true" : "false"},
                       {.name = "reason", .value = run_report.failure_reason},
                   }));
    return run_report.failed ? 1 : 0;
}

} // namespace kin
