# Testing

Kin tests are CTest-driven C++ executables under `tests/`, built when kin is the
top-level project (`KIN_BUILD_TESTS`).

## Quick Start

Configure, build, and run the full test suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure -j 8
```

Keep CTest parallelism at 8 or below: many tests start SDL, and higher counts
cause backend startup contention.

Pass normal CTest arguments for focused runs:

```sh
ctest --test-dir build -C Release --output-on-failure -R kin-scene
ctest --test-dir build -C Release --output-on-failure -R kin-ui2
ctest --test-dir build -C Release --output-on-failure --rerun-failed
```

Use `-j 1` when diagnosing order or resource issues.

## Building Tests

Build one test executable:

```sh
cmake --build build --config Release --target kin_scene_tests
```

On Windows, run these from a Visual Studio developer prompt (or any shell where
MSVC's environment is set up) so the compiler and its include/library paths
are found.

## Test Layout

Each test source usually builds one executable:

```cmake
add_executable(kin_scene_tests
    scene_tests.cpp
)

target_link_libraries(kin_scene_tests PRIVATE
    kin::engine
)

add_test(NAME kin-scene COMMAND kin_scene_tests)
```

CTest names use kebab case, such as `kin-scene`. Executable targets use snake case,
such as `kin_scene_tests`.

## Keeping Tests Fast

Prefer pure engine tests when possible. ECS, asset parsing, pathfinding, UI layout,
and serialization tests should avoid SDL windows unless the behavior specifically
depends on platform or renderer integration.

When a test does need `App`, `Window`, or `Renderer2D`, create those fixtures sparingly.
SDL setup and teardown are expensive compared with normal engine assertions. Reuse a
fixture inside one test executable and reset the state under test between cases:

```cpp
struct Fixture {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window;
    kin::Renderer2D renderer;
    kin::SceneManager scenes;
    kin::SceneContext ctx;

    Fixture()
        : window(app.create_window({.title = "test", .width = 64, .height = 64, .hidden = true})),
          renderer(window),
          ctx{app, window, renderer, app.input(), scenes, 0.1f, true} {
    }
};
```

For renderer-facing logic that does not need the SDL backend, use a small fake
`IRenderer2DBackend`. This keeps tests deterministic and avoids graphics backend
startup cost.

Use temporary directories for generated assets and scripts:

```cpp
const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-my-test";
std::filesystem::remove_all(dir);
std::filesystem::create_directories(dir);
```

Clean the directory at the start of tests that assert discovery counts or exact file
sets. For hot-reload tests, advance the file timestamp explicitly after rewriting.

## Failure Workflow

Run the last failed tests:

```sh
ctest --test-dir build -C Release --output-on-failure --rerun-failed
```

Run one executable directly when you do not need CTest filtering:

```powershell
.\build\bin\kin_scene_tests.exe
```

For assertion-heavy tests on MSVC, route assertion text to stderr:

```cpp
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
```

This keeps CTest failure output useful in non-interactive shells.

## Headless Expectations

Headless tests should be deterministic:

- fixed frame counts,
- fixed timestep assumptions,
- no sleeps except when explicitly testing timestamp or hot-reload behavior,
- no dependency on a real display or audio device,
- stable process exit codes.

If a test needs backend integration, keep it as a smoke test and put most assertions
around the pure engine layer or a fake backend.

## Headless CLI

Any game built on `run_scene_app` (see `games/*/src/main.cpp`) accepts a shared set
of headless flags parsed by `parse_headless_options`:

| Flag | Effect |
|---|---|
| `--headless` | Run headless (dummy video driver, hidden window). Defaults to 1 frame. |
| `--frames=N` / `--frames N` | Run exactly N fixed-timestep frames. |
| `--seed=N` / `--seed N` | Seed the run's root RNG key (`SceneContext::rng`, default 0). |
| `--report[=PATH]` | Emit a JSON run report to PATH (`-` for stdout). Forces headless. |
| `--list-actions` | Print available input actions and bindings, then quit. |
| `--game-info` / `--list-info` | Print game metadata, then quit. |
| `--profile-render` | Run 600 frames and print a render timing table. |
| `--profile` | Run headless profiling. Defaults to 600 frames unless `--frames` is set. |
| `--profile-lines` | Enable line/block profile capture for compiled-in Kin profile macros. |
| `--profile-json=PATH` | Write `kin.profile/1` JSON to PATH (`-` for stdout). Implies `--profile`. |
| `--profile-text=PATH` | Write a text profile summary to PATH (`-` for stdout). Implies `--profile`. |

### Determinism and exit codes

Runs are deterministic by construction: fixed timestep, explicit frame count, and an
explicit seed. The process exit code is the contract for agents — `0` on success,
`1` when a scene calls `ctx.report->fail("reason")`. A failure also stops the run
early so the remaining frames do not execute.

### JSON run report

`--report` writes a structured, assertable snapshot of the run:

```json
{
  "schema": "kin.run_report/1",
  "status": "ok",
  "seed": 7,
  "frames": 5,
  "scenes": [
    { "name": "Play", "state": { "score": 3, "game_over": false } }
  ]
}
```

`status` is `"failed"` (with a `failure_reason` field) when a scene fails. Each scene
on the stack contributes its assertable state by overriding `Scene::write_report`,
which receives a `JsonWriter` positioned inside that scene's `state` object:

```cpp
void write_report(kin::JsonWriter& json) const override {
    json.field("score", _score);
    json.field("game_over", !_alive);
}
```

This is the headless test surface: drive the game for N frames with a fixed seed,
then assert on the exit code and the JSON state.

## Benchmarking And Profiling

Build the benchmark runner:

```sh
cmake --build build --config Release --target kin_bench
```

List available benchmark cases:

```powershell
.\build\bin\kin_bench.exe --list
```

Run the tilemap suite with text output and a machine-readable report:

```powershell
.\build\bin\kin_bench.exe --suite tilemap --iterations 20 --warmup 3 --json out\bench\tilemap.json
```

For full-game profiling, use any game executable built on `run_scene_app`:

```powershell
.\build\bin\ecs_systems_demo.exe --profile --frames=600 --seed=7 --profile-json=out\profile\ecs_systems_demo.json
```

The profile JSON schema is `kin.profile/1`. Runtime profiles include frame,
update, scene render, debug overlay, present, and backend present timings by
default. Add `KIN_PROFILE_SCOPE`, `KIN_PROFILE_FUNCTION`, or `KIN_PROFILE_LINE`
in engine/game code and configure with `-DKIN_ENABLE_PROFILING=ON` to collect
manual source-location entries.

## Server Mode

> Full reference: [agent_interface.md](agent_interface.md). The summary below
> covers the testing-relevant essentials.

`--server` runs the game as a long-lived JSON-RPC command server instead of a
fixed headless run, letting an agent spawn the process once and drive it
interactively. The transport is selectable (`--server-transport=stdio|http`) and
so is the time model (`--server-mode=driven|realtime`, driven default).

```powershell
$cmds = @(
  '{"id":1,"method":"game.info"}',
  '{"id":2,"method":"input.action","params":{"name":"spawn","mode":"press"}}',
  '{"id":3,"method":"sim.tick","params":{"count":40}}',
  '{"id":4,"method":"scene.current"}',
  '{"id":5,"method":"server.shutdown"}'
) -join "`n"
$cmds | .\build\bin\ecs_systems_demo.exe --server --seed=7
```

Each request is one line: `{"id": <any>, "method": "...", "params": {...}}`. Each
response is one line: `{"id": <echoed>, "result": {...}}` or
`{"id": <echoed>, "error": {"message": "..."}}`. A leading UTF-8 BOM is tolerated.

| Method | Effect |
|---|---|
| `game.info` | Game metadata (id, title, window, tags, fields). |
| `scene.stack` | All scenes with index, name, flags, and `has_world`. |
| `scene.current` | Top scene name plus its `write_report` state. |
| `scene.actions` | Available input actions and their bindings. |
| `world.snapshot {scene?, animations?}` | Flecs world JSON for an ECS scene (see below). |
| `animation.diagnostics` | Unresolved-reference diagnostics from the scene's animation library. |
| `ui.snapshot {scene?}` | Unavailable after legacy `kin/ui` removal; use `world.snapshot` or scene reports. |
| `view.screenshot {path?}` | Render the current scene and save a PNG; returns path + dimensions. |
| `sim.tick {count}` | Advance N fixed-timestep frames; returns the frame counter. |
| `sim.reset {seed?}` | Rebuild the scene stack from the game's factory; zero frame/report; optionally reseed. |
| `input.action {name, mode}` | Queue an action `press` (one-frame tap), `hold`, or `release` for the next tick. |
| `input.mouse {x, y, button?, mode?, wheel?, space?}` | Move the pointer and optionally click/scroll; coordinates default to logical space unless `space` is specified. |
| `input.text {text}` | Deliver a text-input string for the next tick. |
| `server.status` | Frame counter, fixed dt, scene depth, failure flag. |
| `ping` / `server.shutdown` | Liveness check / stop the server. |

In **driven** mode time only advances on `sim.tick`, so a run is fully
reproducible from the seed plus the command log — the log itself is a replayable
scenario. The dispatcher (`dispatch_server_command` over a `ServerContext`) is
transport-agnostic and unit-tested directly in `server_tests.cpp`.

### Reading and driving UI

The retained UI snapshot path has been removed. For automation, expose assertable
state through `write_report`, observe ECS state through `world.snapshot`, and
drive UI with `input.mouse`/keyboard actions using known coordinates for the
scene under test.

`input.mouse` coordinates default to game logical space. Pass `"space":"window"`
when driving native window-pixel overlays such as the F1 debug panel.

Pointer and text injection follow the same after-`begin_frame` timing as
`input.action`, so a one-frame `press` is auto-released and does not linger.

### HTTP transport

`--server-transport=http --port=N` exposes the same methods over HTTP, built on
the flecs HTTP server that ships compiled into the engine. The request **path is
the method** and the request **body is the params object**; the response body is
the result JSON directly (no `id` envelope — HTTP correlates request and
response). Success is `200`, a bad request/params is `400`, an unknown method is
`404`. Replies carry `Access-Control-Allow-Origin: *` so the endpoints work from
a browser console, and `OPTIONS` preflight is handled.

```bash
ecs_systems_demo --server --server-transport=http --port=8137 &

curl -s localhost:8137/game.info
curl -s localhost:8137/scene.stack
curl -s -X POST localhost:8137/sim.tick    -d '{"count":3}'
curl -s -X POST localhost:8137/input.action -d '{"name":"spawn","mode":"press"}'
curl -s localhost:8137/world.snapshot
curl -s localhost:8137/server.shutdown
```

Request handlers run on the main thread while the loop pumps
`ecs_http_server_dequeue`, so game state needs no locking. Driven mode advances
only on `sim.tick`; realtime mode also steps a fixed frame per loop iteration.

## Scene Server Snapshots

Driven scene-server tests can request `world.snapshot` for ECS scenes. The result
keeps the raw Flecs `world` JSON and also includes animation-player snapshots by
default:

```json
{
  "scene": 0,
  "world": {},
  "animations": [
    {
      "entity": 512,
      "name": "actor",
      "state": "idle",
      "playing": true,
      "bindings": { "X": "hero" },
      "triggers": ["hit"],
      "layers": [
        { "kind": "base", "animation": "hero.idle", "time": 0.016, "speed": 1, "weight": 1, "done": false }
      ]
    }
  ]
}
```

Pass `"params":{"animations":false}` to omit the `animations` section when a test
only needs the raw ECS world.

Scenes that expose an animation library through `Scene::animation_library()` also
support:

```json
{ "id": 1, "method": "animation.diagnostics" }
```

The response is `{ "diagnostics": [...] }` with `severity`, `path`, and `message`
fields. Scenes without an animation library return a clear error.


