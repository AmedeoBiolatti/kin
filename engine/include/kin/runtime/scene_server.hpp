#pragma once

#include <kin/core/json_value.hpp>
#include <kin/core/rng.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/windowed_app.hpp>
#include <kin/scene/scene_manager.hpp>

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class AssetServer;

enum class ServerMode {
    // Time only advances when the client calls sim.tick. Single-threaded,
    // deterministic, reproducible from seed plus the command log.
    Driven,
    // The game runs at wall-clock pace on the main thread; commands are drained
    // between frames from a background reader.
    Realtime,
};

enum class ServerTransport {
    // Newline-delimited JSON-RPC over stdin/stdout. Single client.
    Stdio,
    // HTTP server (one request = one command). Path is the method, the request
    // body is the params object. Multi-client and browser/curl friendly.
    Http,
};

// Map the CLI/option strings ("driven"/"realtime", "stdio"/"http") to the enums.
// Unrecognized input falls back to the default (Driven / Stdio). These centralize
// the mapping so callers (e.g. the headless launcher) don't inline the comparison.
ServerMode parse_server_mode(std::string_view text);
ServerTransport parse_server_transport(std::string_view text);

struct ServerResponse {
    bool ok = true;
    std::string error;        // populated when !ok
    std::string result_json;  // serialized result object, valid JSON when ok
};

// Holds the live engine objects the command handlers read and mutate. Exposed
// so the dispatcher can be exercised in tests without any transport.
class ServerContext {
public:
    ServerContext(App& app,
                  Window& window,
                  Renderer2D& renderer,
                  SceneManager& scenes,
                  WindowedAppConfig window_config,
                  RngKey rng,
                  f32 fixed_dt,
                  bool render,
                  AssetServer* asset_server = nullptr);

    App& app() { return _app; }
    Window& window() { return _window; }
    Renderer2D& renderer() { return _renderer; }
    SceneManager& scenes() { return _scenes; }
    const WindowedAppConfig& window_config() const { return _window_config; }
    RngKey rng() const { return _rng; }
    f32 fixed_dt() const { return _fixed_dt; }
    i64 frame() const { return _frame; }
    const RunReport& report() const { return _report; }

    // Advance exactly one fixed-timestep frame. Queued input actions are applied
    // immediately after begin_frame so the scene observes them this frame.
    void step();

    // Apply pending scene-stack commands (initial push, on_enter) without
    // advancing gameplay, so read endpoints work before the first tick.
    void realize_scenes();
    // Pops every scene (on_exit, then destroyed) while the renderer is still
    // alive, so scene-owned textures and shaders are freed before the device
    // is. run_scene_server calls it on the way out, as run_scene_app does.
    void shutdown_scenes();

    // Render the current scene to the (headless) backbuffer without presenting,
    // so view.screenshot can read it back.
    void render_frame();

    // Provide a factory used by sim.reset to rebuild the scene stack.
    void set_reset_factory(std::function<void(SceneManager&)> factory) {
        _reset_scenes = std::move(factory);
    }
    bool can_reset() const { return static_cast<bool>(_reset_scenes); }

    // Rebuild the scene stack from the reset factory and zero the frame counter,
    // report, and queued input. Optionally reseed the root RNG key.
    void reset(std::optional<u64> seed);

    // Queue an action for the next step. mode is "press" (one-frame tap),
    // "hold", or "release".
    void queue_action(std::string name, std::string mode);

    // Queue a pointer move to window-space pos for the next step.
    void queue_mouse_move(Vec2f window_pos);
    // Queue a mouse button for the next step. mode is "press", "hold", "release".
    void queue_mouse_button(std::string button, std::string mode);
    // Queue a mouse wheel delta for the next step.
    void queue_mouse_wheel(f32 delta);
    // Queue text input for the next step.
    void queue_text(std::string text);

private:
    struct PendingInput {
        enum class Kind { Action, MouseMove, MouseButton, Wheel, Text };
        Kind kind;
        std::string text;  // action name / button name / text input
        std::string mode;  // press / hold / release
        Vec2f pos{};       // window-space pointer position
        f32 value = 0.0f;  // wheel delta
    };

    SceneContext make_context();

    App& _app;
    Window& _window;
    Renderer2D& _renderer;
    SceneManager& _scenes;
    WindowedAppConfig _window_config;
    RngKey _rng;
    f32 _fixed_dt;
    bool _render;
    AssetServer* _asset_server = nullptr;
    i64 _frame = 0;
    RunReport _report;
    std::vector<PendingInput> _pending;
    std::function<void(SceneManager&)> _reset_scenes;
};

// Routes one command (method + params) to its handler. Transport-agnostic: the
// stdio and future HTTP loops both funnel through here.
ServerResponse dispatch_server_command(ServerContext& ctx,
                                       const GameInfo* game,
                                       std::string_view method,
                                       const JsonValue& params);

struct ServerConfig {
    WindowedAppConfig window;
    ServerTransport transport = ServerTransport::Stdio;
    ServerMode mode = ServerMode::Driven;
    u64 seed = 0;
    u16 port = 8080;          // HTTP transport only
    bool render = false;
    const GameInfo* game = nullptr;
    // stdio transport streams; default to std::cin / std::cout when null. Tests
    // inject their own streams here.
    std::istream* in = nullptr;
    std::ostream* out = nullptr;
    // Optional factory that repopulates the scene stack for sim.reset. When unset,
    // sim.reset returns an error.
    std::function<void(SceneManager&)> reset_scenes;
    // Optional async assets are pumped before each simulation step. Kept last
    // so existing positional aggregate initialization remains source-compatible.
    AssetServer* asset_server = nullptr;
};

// Runs the game as a long-lived command server. Returns a process exit code.
int run_scene_server(const ServerConfig& config, SceneManager& scenes);

} // namespace kin
