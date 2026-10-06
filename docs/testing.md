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
| `--frames=N` / `--frames N` | Run exactly N fixed-timestep frames; a windowed run instead runs in real time (frame rate cap included) and quits after N rendered frames. |
| `--seed=N` / `--seed N` | Seed the run's root RNG key (`SceneContext::rng`, default 0). |
| `--max-fps=N` / `--max-fps N` | Cap a windowed run at N frames a second (0: uncapped), over the game's own setting. Headless runs are never paced. |
| `--report[=PATH]` | Emit a JSON run report to PATH (`-` for stdout). Forces headless. |
| `--probe-render[=PATH]` | Check every rendered frame for spikes and flicker; write a JSON report to PATH (`-` or no PATH for stdout). Forces headless. See [Render probe](#render-probe). |
| `--probe-fail` | Fail the run (exit code `1`) when the render probe finds anything. Forces headless. |
| `--probe-tile=N` | Render probe tile size in pixels (default 16). |
| `--check-determinism[=PATH]` | Run the game three times in lockstep and report where their states differ, as JSON to PATH (`-` or no PATH for stdout). See [Determinism check](#determinism-check). |
| `--list-actions` | Print available input actions and bindings, then quit. |
| `--game-info` / `--list-info` | Print game metadata, then quit. |
| `--profile-render` | Run 600 frames and print a render timing table. |
| `--profile` | Profile the run: windowed, until it quits or `--frames` N; with `--headless`, a 600-frame pass unless `--frames` is set. |
| `--overdraw-view` | Start with the overdraw view on (SDL_GPU): the screen shows how many times each pixel is shaded, as colours. Also a debug overlay toggle. |
| `--screenshot=PATH` | Save the last frame as a PNG when the run ends. |
| `--locale=TAG` / `--locale TAG` | Show the game in this language (see [localization](localization.md)), over its own choice. Headless and server runs without it use the base language. |
| `--pseudo-locale` | Show the pseudo-locale (`en-XA`): accented, longer, bracketed text, to find untranslated strings and layouts that will not fit. |
| `--fail-on-missing-text` | Fail the run if any text was looked up by a key the shown language lacks. The run report always lists them under `localization.missing_text`. |
| `--fail-on-text-overflow` | Fail the run if any ui2 widget's text did not fit its bounds. The run report always lists them under `ui_overflow`; with `--pseudo-locale` this finds layouts too tight for longer languages. |
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

## Determinism check

`--check-determinism` checks the promise above: that the same seed and frame
count always play out the same way. Instead of playing, the game runs itself
three times as headless child processes, in lockstep:

- **baseline**;
- **repeat**, the same again: catches anything that differs between two runs of
  one build, such as uninitialized memory, clocks, or behavior that depends on
  addresses (pointer-keyed maps, sorting by pointer);
- **one worker**, with `KIN_JOB_WORKERS=1`: catches anything that depends on
  how work is split between threads, such as races and order-dependent
  parallel systems.

```sh
./build/bin/ecs_graph_demo --check-determinism --frames=300 --seed=7
./build/bin/ecs_graph_demo --check-determinism=out/determinism.json --frames=600
```

After every update each run hashes its state, and the runs are compared frame
by frame. At the first frame where a run differs from the baseline, both are
asked for their state item by item, and the report names what differs. The
exit code is `1` if any run differs.

```json
{
  "schema": "kin.determinism/1",
  "status": "diverged",
  "frames": 300, "seed": 7,
  "coverage": { "scenes": 1, "entities": 45, "values": 113, "not_compared": ["(Identifier,Name)", "Label"] },
  "runs": [
    { "name": "repeat", "environment": {}, "status": "ok", "frames": 300 },
    { "name": "one worker", "environment": { "KIN_JOB_WORKERS": "1" }, "status": "diverged",
      "frames": 37, "first_frame": 37, "total_differences": 2,
      "differences": [
        { "scene": "Play", "entity": 571, "name": "box_3", "component": "Velocity", "change": "value",
          "baseline": { "hex": "0000a0c20000f042", "as_f32": [-80, 120] },
          "run":      { "hex": "0000a0420000f042", "as_f32": [80, 120] } },
        { "scene": "Play", "component": "(report)", "change": "value",
          "fields": [ { "path": ".bounces", "baseline": 12, "run": 11 } ] }
      ] }
  ]
}
```

What is compared, per scene on the stack:

- the scene's `write_report` state (differences are listed by field path);
- if the scene exposes an ECS world (`Scene::world()`), every entity: its set of
  components, tags and pairs (`"(type)"`), and the bytes of each plain-data
  component (trivially copyable). `as_f32` shows the same bytes read as floats,
  since most game data is.

What is not:

- components that own resources (strings, containers, texture handles): their
  bytes hold addresses that differ between processes. They're listed in
  `coverage.not_compared`. Move state that matters out of them, or into
  `write_report`;
- the ECS's own bookkeeping (component and type definitions, systems, queries,
  observers, the flecs namespace);
- state a scene keeps in its own members, unless its `write_report` includes it.
  A game without an ECS world is checked through its reports alone; `coverage`
  says how much was seen.

Things to know:

- Reports must be deterministic too: a report that includes timings (such as
  a system's `last_duration_ms`) differs on every run and fails the check.
- A plain-data component whose padding bytes are left uninitialized, or that
  holds a raw pointer, can show up as a difference that isn't one. Fields
  initialized in the struct, or `T{}`, keep padding stable.
- Frames are numbered like the run's: on frame 1 the first scene is pushed, so
  its first update happens on frame 2.
- The cost is three headless runs plus hashing each frame's state, which is
  linear in entities. The children inherit the game's other options (seed,
  logging); options that make it do something else (`--report`, `--profile*`,
  `--probe-*`, `--server`) are dropped.
- `KIN_JOB_WORKERS=N` sets the default job system's worker count in any run.
- Builds configured with `-DKIN_ENABLE_DETERMINISM_CHECK=OFF` leave the check out.

`kin::hash_state` and `kin::describe_state` (`kin/runtime/state_hash.hpp`) are
the state hashing on its own, for tests that compare states directly.

## Render probe

`--probe-render` checks a run's rendering for animation and rendering faults
without any game-specific setup or reference images. It renders every headless
frame, reads it back, and compares it with the two frames before it, tile by tile
(16×16 pixels by default):

```sh
./build/bin/ecs_systems_demo --frames=600 --seed=7 --probe-render=out/probe.json
./build/bin/ecs_systems_demo --frames=600 --seed=7 --probe-render --probe-fail   # CI gate
```

It reports two kinds of event:

- **flicker** — a frame that differs from both neighbours while they agree with
  each other: the A-B-A of a sprite jittering in place, a sprite flipping between
  two frames every tick, or a single wrong frame. Something moving through a tile
  is not flicker: the ratio has to hold over the tiles around it too
  (`flicker_radius`), so the change can't simply have moved next door.
- **spike** — a tile changes far more than the run's recent frames did (robust
  deviations over a rolling median of each frame's most-changed tile), after an
  8-frame warm-up. A sudden flash, a texture turning to garbage, a teleport.

Consecutive frames with the same kind of event in overlapping places are one
event, so a sprite that jitters for 200 frames is one event with `frames: 200`.

```json
{
  "schema": "kin.render_probe/1",
  "width": 960, "height": 540, "frames": 600,
  "config": { "tile_size": 16, "pixel_threshold": 0, "baseline_frames": 60, "warmup_frames": 8,
              "spike_sigmas": 8, "spike_sigma_floor": 0.01, "spike_min_delta": 0.05,
              "flicker_min_delta": 0.01, "flicker_ratio": 4, "flicker_radius": 2 },
  "summary": { "events": 1, "spikes": 0, "flickers": 1, "peak_delta": 0.0172, "mean_delta": 0.0093 },
  "events": [
    { "kind": "flicker", "first_frame": 12, "last_frame": 211, "frames": 200,
      "rect": { "x": 480, "y": 256, "w": 32, "h": 16 },
      "peak_frame": 40, "peak_delta": 0.086, "peak_score": 86.4,
      "culprits": [
        { "entity": 557, "name": "hero", "component": "SpriteRenderer", "frames": 200,
          "changes": ["moved"], "max_move": 1, "score": 1 }
      ] }
  ],
  "timeline": { "delta": [], "max_tile_delta": [], "changed": [] }
}
```

Frames are numbered like the run's frames (the first rendered frame is 1); `rect`
is in the captured frame's pixels. Deltas are mean channel change, 0 to 1.
`peak_score` is the number of deviations over the baseline for a spike, and the
ratio of the frame's change to its neighbours' for flicker. `timeline` holds one
value per frame, for plotting. The report is deterministic: the same seed and
frame count give the same file.

### Culprits

While the probe runs, every draw that reaches the screen is traced with its
bounds and with who drew it. Each event lists up to five `culprits`: the draw
sources near it whose own draws changed in the way the event implies. For
flicker that means a change that came back (moved and moved back, a color or
animation frame that flipped and flipped back), or a draw shown or hidden for a
single frame. For a spike it means any change. `changes` says how: `moved`,
`resized`, `frame` (another region of the same texture, i.e. an animation
frame), `color`, `texture`, `rotated`, `appeared`, `disappeared`. Draws that
stayed the same are never culprits, so a still wall next to a jittering sprite
isn't blamed.

A source is one of:

- an ECS entity and render component (`entity`, `name`, `component`): what the
  `SpriteRenderer`, `TextureRenderer`, `RectRenderer` and `LineRenderer` and particle
  components draw, through `render_world`, `collect_*` or `submit_*`;
- a named scope (`scope`) for code that draws directly:

  ```cpp
  #include <kin/renderer/draw_trace.hpp>   // KIN_DRAW_SCOPE
  #include <kin/ecs/render.hpp>            // KIN_DRAW_ENTITY

  {
      KIN_DRAW_SCOPE("hud.minimap");
      draw_minimap(renderer);              // its draws are the minimap's
  }
  world.each([&](flecs::entity entity, const Box& box) {
      KIN_DRAW_ENTITY(entity, "Box");      // these draws are entity's Box
      renderer.fill_rect(box.rect, box.color);
  });
  ```

- otherwise, the scene whose `render` drew it (`scope` is the scene's `name()`).

An empty `culprits` list means no traced draw explains the change. That usually
points at something the trace can't see, such as a shader or a render target
whose contents changed while its draw stayed put. Both macros compile to
nothing when the probe is compiled out.

Things to know:

- The probe sees what the headless renderer draws. That is the SDL_Renderer
  software backend, so custom materials, post-processing and transition shaders
  are not drawn and their faults can't be seen.
- Under full-screen motion such as a scrolling camera every tile changes every
  frame, which hides flicker. Large one-frame glitches still show up there, as a
  spike in and out.
- Legitimate fast change (screen shake, hit flashes, scene cuts) shows up as
  spikes or flicker too. Read the events before gating CI on `--probe-fail`.
- The cost is a framebuffer readback and two frame comparisons a frame, a few
  milliseconds at 960×540. Compiled in but not running, the probe costs a
  load and a branch per queued draw (about 3% of collecting and flushing 125k
  sprites into a backend that draws nothing). Builds configured with
  `-DKIN_ENABLE_RENDER_PROBE=OFF` leave it out entirely; the flags then only
  log a warning.

`kin::RenderProbe` (`kin/runtime/render_probe.hpp`) is the analysis on its own:
feed it RGBA frames (and optionally their `DrawRecord`s from a `kin::DrawTrace`)
with `add_frame`, and read `events()` or `write_json`.

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
| `input.action {name, mode}` | Queue an action `press` (a tap, released at the start of the next tick), `hold`, or `release` for the next tick. |
| `input.mouse {x, y, button?, mode?, wheel?, space?}` | Move the pointer and optionally click/scroll; coordinates default to logical space unless `space` is specified. |
| `input.text {text}` | Deliver a text-input string for the next tick. |
| `server.status` | Frame counter, fixed dt, scene depth, failure flag. |
| `frame.timing {reset?}` | Update and render time over the last 600 ticks (last, mean, max). |
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
`input.action`. A `press` is released at the start of the next tick, so a ui2
click completes then; hover the widget for a tick before pressing.

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


