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
