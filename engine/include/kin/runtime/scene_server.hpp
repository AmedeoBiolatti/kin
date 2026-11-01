#pragma once

#include <kin/core/json_value.hpp>
#include <kin/core/rng.hpp>
#include <kin/core/script_debugger.hpp>
#include <kin/platform/input.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/run_report.hpp>
#include <kin/runtime/windowed_app.hpp>
#include <kin/scene/scene_manager.hpp>

#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
                  AssetServer* asset_server = nullptr,
                  Renderer3D* renderer3d = nullptr);

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

    // Queue an action for the next step. mode is "press" (a tap: held for that
    // step and released at the start of the one after, which sees the release),
    // "hold", or "release".
    void queue_action(std::string name, std::string mode);

    // Queue a pointer move to window-space pos for the next step.
    void queue_mouse_move(Vec2f window_pos);
    // Queue a mouse button for the next step. mode is "press" (a tap, as for
    // actions: a ui2 click completes on the step after it), "hold", "release".
    void queue_mouse_button(std::string button, std::string mode);
    // Queue a mouse wheel delta for the next step.
    void queue_mouse_wheel(f32 delta);
    void queue_mouse_motion(Vec2f delta); // relative, window pixels
    // Queue text input for the next step.
    void queue_text(std::string text);
    // Queue a key for the next step, with modifiers held alongside it. mode is
    // "press" (a tap), "hold" or "release", as for actions.
    void queue_key(Key key, std::string mode, KeyModifiers modifiers);
    // Queue a file dropped on the window at window-space pos, as from the desktop.
    void queue_drop(std::string path, Vec2f window_pos);

    // Wall time of recent frames, oldest first: a driven server's steps, or the
    // frames a game run with --inspect draws (frame.timing reports them).
    struct FrameTiming {
        f64 update_ms = 0.0;   // the scenes' updates (every fixed step run for it)
        f64 render_ms = 0.0;   // drawing it
        f64 present_ms = 0.0;  // presenting it (with any wait for the display)
        f64 interval_ms = 0.0; // since the frame before began: what a player sees (0: not known)
        i64 frame = 0;         // its number among those recorded
    };
    static constexpr std::size_t frame_timing_window = 600;
    const std::deque<FrameTiming>& frame_timings() const { return _timings; }
    void clear_frame_timings() { _timings.clear(); }
    // Keeps a frame's times, numbered after the last (the last
    // frame_timing_window of them).
    void record_frame_timing(FrameTiming timing);

private:
    struct PendingInput {
        enum class Kind { Action, MouseMove, MouseButton, Wheel, Text, Key, MouseMotion, Drop };
        Kind kind;
        std::string text;  // action name / button name / text input / dropped file
        std::string mode;  // press / hold / release
        Vec2f pos{};       // window-space pointer position
        f32 value = 0.0f;  // wheel delta
        Key key = Key::Unknown;
        KeyModifiers modifiers = KeyModifiers::None;
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
    Renderer3D* _renderer3d = nullptr;
    i64 _frame = 0;
    RunReport _report;
    std::vector<PendingInput> _pending;
    // Taps pressed in the last step, released at the start of the next.
    std::vector<std::string> _tapped_actions;
    std::vector<MouseButton> _tapped_buttons;
    std::vector<std::pair<Key, KeyModifiers>> _tapped_keys;
    std::deque<FrameTiming> _timings;
    i64 _timed_frames = 0; // frames recorded, the forgotten too
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
    bool rendering_3d = false;
};

// Runs the game as a long-lived command server. Returns a process exit code.
int run_scene_server(const ServerConfig& config, SceneManager& scenes);

// A game played in its window that also answers server commands, between
// frames (--inspect): a tool such as an editor watches it while it runs.
// Requests and responses are the stdio server's, one JSON line each; the log
// stays on the error stream.
//
// It answers what reads the game (game.info, scene.stack, scene.current,
// scene.actions, world.snapshot, animation.diagnostics, frame.timing, ping),
// server.status (with "paused"), and holds or advances its updates:
// sim.pause, sim.resume, sim.step {count} (the game keeps drawing meanwhile).
// server.shutdown quits the game. What drives a headless server (sim.tick,
// sim.reset, input.*, view.screenshot) is refused: the player drives this one.
//
// It debugs the game's scripts (script_debugger(), while it lives):
// debug.breakpoints {files: {"scripts/a.lua": [3, 7]}} sets them all,
// debug.pause stops at the next line run, debug.state tells the stop (if a
// script is held at one: its frames, their locals and upvalues), and
// debug.continue or debug.step {how: "into"|"over"|"out"} goes on from it.
// While a script is stopped the game is held inside it (it does not draw):
// only debug.*, server.status (with "stopped"), ping and server.shutdown are
// answered then, the rest when it goes on.
class LiveInspector {
public:
    // Reads requests from `in` on a thread of its own; answers on `out`, from
    // another: a tool slow to read a large answer (a world's snapshot) holds
    // that thread, never the game. On std::cout (in step with stdio, as it is
    // unless a game turns that off) an answer is written under stdout's lock,
    // so the game's own prints do not land inside it.
    LiveInspector(std::istream& in, std::ostream& out);
    ~LiveInspector();
    LiveInspector(const LiveInspector&) = delete;
    LiveInspector& operator=(const LiveInspector&) = delete;

    // Answers the requests that have arrived, on the calling (main) thread:
    // once a frame, outside the scenes' update and render.
    void poll(ServerContext& ctx, const GameInfo* game);
    // Whether the next update runs (false while paused with no step left);
    // counts those that do.
    bool admit_update();

    bool paused() const { return _paused; }
    i64 frame() const { return _frame; }
    // The tool closed its end: nothing more comes, and nothing holds the game.
    bool finished() const { return _finished; }
    // Waits until the answers given so far are written (a test reading `out`).
    void flush();

private:
    struct Shared;
    struct Outbox;

    void answer(ServerContext& ctx, const GameInfo* game, std::string_view line);
    void send(const JsonValue* id, const ServerResponse& response);
    // A debug.* request; while stopped, how the script goes on, when it says.
    ServerResponse answer_debug(std::string_view method, const JsonValue& params, bool stopped,
                                std::optional<ScriptResume>& resume);
    // The debugger's waiter: answers the tool while a script is stopped.
    ScriptResume wait_stopped();
    ServerResponse status(bool stopped) const;

    std::shared_ptr<Shared> _shared;
    std::thread _reader;
    std::shared_ptr<Outbox> _outbox;
    std::thread _writer;
    std::ostream& _out;
    ServerContext* _ctx = nullptr; // the last poll's
    bool _paused = false;
    i64 _steps = 0; // updates to run while paused
    i64 _frame = 0;
    bool _finished = false;
};

} // namespace kin
