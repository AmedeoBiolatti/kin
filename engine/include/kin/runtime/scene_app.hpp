#pragma once

#include <kin/runtime/game_info.hpp>
#include <kin/platform/log.hpp>
#include <kin/scene/scene_manager.hpp>

#include <functional>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace kin {

class AssetServer;
class FileWatcher;
class Localization;

struct HeadlessOptions {
    bool enabled = false;
    bool list_actions = false;
    bool print_game_info = false;
    bool profile_render = false;
    bool profile = false;
    bool profile_lines = false;
    i32 frames = 0;
    // Seed for the run's root RNG key (--seed). Threaded into SceneContext::rng.
    u64 seed = 0;
    // Destination for the JSON run report (--report). "-" means stdout. A
    // non-empty value also forces headless execution.
    std::string report_path;
    std::string profile_json_path;
    std::string profile_text_path;
    // --overdraw-view: start with the overdraw view on (Renderer2D::set_overdraw_view).
    bool overdraw_view = false;
    // --screenshot=PATH: the last frame saved as a PNG when the run ends.
    std::string screenshot_path;
    // Run the game as a long-lived command server (--server) instead of a fixed
    // headless run. server_mode is "driven" (default) or "realtime";
    // server_transport is "stdio" (default) or "http".
    bool server = false;
    std::string server_mode = "driven";
    std::string server_transport = "stdio";
    i32 server_port = 8080;
    std::string log_path;
    std::optional<LogLevel> log_level;
    std::optional<LogFormat> log_format;
    bool invalid_log_option = false;
    // --max-fps N: caps a windowed run's frame rate, over the game's own setting
    // (0: uncapped).
    std::optional<f32> max_fps;
    // Render probe (--probe-render[=PATH]): look for spikes and flicker in every
    // rendered frame and write a kin.render_probe/1 report to PATH ("-" or no
    // PATH: stdout). --probe-fail fails the run when the probe finds anything;
    // --probe-tile=N sets the tile size. Each forces a headless run that renders.
    // Needs a build with KIN_ENABLE_RENDER_PROBE.
    std::string probe_render_path;
    bool probe_fail = false;
    i32 probe_tile_size = 0;
    // Determinism check (--check-determinism[=PATH]): instead of playing, run the
    // game three times in lockstep (twice alike, once with one job worker) and
    // report the first frame and the entities and components where their states
    // differ, as kin.determinism/1 JSON to PATH ("-" or no PATH: stdout). Needs a
    // build with KIN_ENABLE_DETERMINISM_CHECK.
    bool check_determinism = false;
    std::string determinism_path;
    // --state-lockstep: the runs the check starts. After each update, print the
    // state's hash and wait for a command on standard input.
    bool state_lockstep = false;
    // --locale=TAG: the language to show (Localization::set_locale), over the
    // game's choice; --pseudo-locale is --locale=en-XA. Headless and server runs
    // without it show the base language, so they stay the same on every machine.
    std::string locale;
    // --fail-on-missing-text: fail the run if any text was looked up by a key
    // the shown language (and its fallbacks) lacks.
    bool fail_on_missing_text = false;
    // --fail-on-text-overflow: fail the run if any ui2 widget's text did not
    // fit its bounds (Label, Button, Toggle, ...: those that report overflow).
    // The run report lists them under ui_overflow either way. With
    // --pseudo-locale it finds layouts too tight for longer languages.
    bool fail_on_text_overflow = false;
    // The command line as given, program first (the check runs it again).
    std::vector<std::string> args;
};

struct SceneAppConfig {
    WindowedAppConfig window;
    HeadlessOptions headless;
    const GameInfo* game = nullptr;
    std::ostream* game_info_output = nullptr;
    std::ostream* action_output = nullptr;
    // When set, the JSON run report is written here instead of opening the file
    // named by headless.report_path. Mainly for tests.
    std::ostream* report_output = nullptr;
    std::ostream* profile_json_output = nullptr;
    std::ostream* profile_text_output = nullptr;
    // When set, the render probe runs and its report is written here instead of
    // to headless.probe_render_path. Mainly for tests.
    std::ostream* probe_output = nullptr;
    // When set, the determinism check's report is written here instead of to
    // headless.determinism_path. Mainly for tests.
    std::ostream* determinism_output = nullptr;
    bool render_headless = false;
    // Optional factory enabling sim.reset in server mode: repopulates the scene
    // stack for a fresh episode. The same factory should produce the initial
    // scenes the game pushes before calling run_scene_app.
    std::function<void(SceneManager&)> reset_scenes;
    // Optional async asset server. When set, run_scene_app pumps it once per
    // frame before scenes.update — draining to quiescence in headless mode (for
    // deterministic runs) and applying budgeted completions when windowed.
    AssetServer* asset_server = nullptr;
    // Optional watcher of the game's own data files (see FileWatcher). When set,
    // run_scene_app polls it once per frame before scenes.update, so changed files
    // reload between frames; never in headless or server runs, which stay
    // deterministic.
    FileWatcher* file_watcher = nullptr;
    // Optional translations. When set, run_scene_app makes them the active
    // localization (kin::tr, retained ui2 text, dialogue, scripts), applies
    // --locale, keeps ui2's direction (ui2::set_ui_direction) that of the
    // language shown, and lists missing text in the run report.
    Localization* localization = nullptr;
    // Where a windowed run keeps the GPU pipelines it made, to make them while
    // the next run loads (Renderer2D::pipeline_record). Unset: the user data
    // folder, kin/<the window title>/pipelines.txt. Empty: not kept.
    std::optional<std::filesystem::path> pipeline_record_path{};
};

HeadlessOptions parse_headless_options(int argc, char** argv);

void write_available_actions(std::ostream& out, const std::vector<AvailableInputAction>& actions);

// Runs the scene app and returns a process exit code: 0 on success, 1 if a
// scene reported a failure via SceneContext::report.
int run_scene_app(const SceneAppConfig& config, SceneManager& scenes);

} // namespace kin
