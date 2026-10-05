# Profiling

Kin has two complementary profiling paths:

- full-game runtime profiling through any executable that uses `run_scene_app`
- focused benchmark profiling through `kin_bench`

Runtime phase profiling works in the normal build. Manual line/block profiling
requires configuring the build with `KIN_ENABLE_PROFILING=ON`.

## Build

Enable manual profiling macros:

```sh
cmake -S . -B build -DKIN_ENABLE_PROFILING=ON
cmake --build build --config Release
```

Return to the default no-op macro build:

```sh
cmake -S . -B build -DKIN_ENABLE_PROFILING=OFF
cmake --build build --config Release
```

Build only the benchmark runner:

```sh
cmake --build build --config Release --target kin_bench
```

## Full-Game Profiles

Any game using `run_scene_app` accepts:

| Flag | Effect |
|---|---|
| `--profile` | Profile the run: windowed, until it quits or `--frames` N; with `--headless`, a 600-frame pass unless `--frames` is set. |
| `--overdraw-view` | Start with the overdraw view on (SDL_GPU): the screen shows how many times each pixel is shaded, as colours. Also a debug overlay toggle. |
| `--screenshot=PATH` | Save the last frame as a PNG when the run ends. |
| `--profile-lines` | Enables manual macro capture for this run. |
| `--profile-json=PATH` | Writes `kin.profile/1` JSON. Use `-` for stdout. |
| `--profile-text=PATH` | Writes a human-readable summary. Use `-` for stdout. |
| `--frames=N` | Runs exactly N fixed-timestep frames. |
| `--seed=N` | Uses a deterministic root RNG seed. |

Example:

```powershell
.\build\bin\ecs_systems_demo.exe --headless --profile --frames=600 --seed=7 --profile-json=out\profile\ecs_systems_demo.json --profile-text=out\profile\ecs_systems_demo.txt
```

Runtime profiles include these phase timings by default:

- `frame`
- `update`
- `render.scene`
- `render.debug_overlay`
- `present`
- `present.flush`
- `present.backend`

If `present.backend` dominates in a headless run, remember that this is the
software/SDL backend path, not necessarily final GPU performance.

On the SDL_GPU backend two more rows show the GPU's side of the frame:

- `gpu.wait`: how long `present()` blocked acquiring a swapchain image, that is
  the CPU waiting for the GPU (or the display, under vsync) to catch up. Near
  zero means the CPU is the bottleneck; a large share of `frame` means the GPU is.
- `gpu.frame`: the GPU time of each frame, from when its first command buffer
  was submitted (or the previous frame finished, if later) to when its fence
  signalled. SDL_GPU has no timestamp queries, so this is measured with fences
  and a thread that polls them; it trails the CPU by a frame or two. With the
  GPU more than a few frames behind, some frames go untimed, and the next
  sample is the average over them.
- `gpu.<name>`: the GPU time of a part of the frame the game marked with
  `Renderer2D::gpu_scope()`:

  ```cpp
  {
      const auto scope = renderer.gpu_scope("shadows");
      draw_shadows(renderer);
  } // the scope ends here
  ```

  The scope's edges split the frame's submission so each side can carry a
  fence: resolution is about 0.1 ms, and each scope adds two submissions.
  Scopes do not nest (an inner one is ignored) and end at `present()`. While
  GPU timing is off, `gpu_scope()` does nothing.

- `render.overdraw` (not a time): how many times each screen pixel was shaded
  that frame, on average: the pixels all draws covered, in each target's own
  pixels (a logical scene at its native size; render targets included,
  overlaps counted each time, clipping not taken off), over the screen's. In
  2D this is usually where GPU time goes. The demos run 0.2 to 3.2; XC-121
  runs 5 to 8.5, about half of it render targets composited over the scene.

- `overdraw.<name>`: a `gpu_scope`'s share of `render.overdraw`, so a frame's
  overdraw can be told apart by part.

To see where the overdraw is, the debug overlay's *Overdraw view* toggle (or
`--overdraw-view`) draws every draw as one layer where it covers and shows the
counts as colours: black none, blue 1, green 2, yellow 4, red 8, white 16 or
more. With `--screenshot=PATH` a run saves its last frame as a PNG. Only the
screen's own draws show: a render target's composite counts once, whatever
was drawn into it.

GPU timing is on while a profile is recorded, `KIN_LOG_FRAME_STATS=1` is set,
or the debug overlay is open; elsewhere `Renderer2D::set_gpu_timing_enabled`
turns it on. `KIN_LOG_FRAME_STATS` logs `gpu_wait_ms` and `gpu_frame_ms` too.

## Renderer Benchmarks

With `KIN_BUILD_BENCHMARKS`, two programs time the SDL_GPU backend directly:

- `kin_draw_bench [frames] [quads]`: a frame of `draw_texture` calls, of
  `fill_rect` calls and one `draw_sprites` batch (min and median CPU time to
  record them, present, GPU time), and a shader's first use. On an RTX 4080
  Laptop GPU, 20,000 quads cost about 1.0 ms as `draw_texture` calls, 0.65 ms
  as `fill_rect` calls and 0.19 ms as one `draw_sprites` batch: many sprites of
  one texture are much cheaper batched (`RenderQueue` does so by itself).
  Its third argument picks other runs: `pipelines`, `data`, `scaled`,
  `compute`, `hotreload`, `surfaces`, `mipmaps`, `layers` and `cached` (see
  `docs/rendering.md`).
- `kin_upload_bench [frames] [workers] [big]`: texture creation and updates.

## Benchmark Profiles

List benchmark cases:

```powershell
.\build\bin\kin_bench.exe --list
```

Run a focused case:

```powershell
.\build\bin\kin_bench.exe --case cached_partial_submit --iterations 20 --warmup 3 --json out\bench\cached_partial.json
```

Run line profiling for benchmark cases:

```powershell
.\build\bin\kin_bench.exe --case cached_partial_submit --iterations 3 --warmup 1 --profile-lines --json out\bench\line_profile_cached_partial.json
```

The `kin.benchmark/1` JSON includes:

- `cases`: benchmark-level min, mean, median, p95, and p99
- `profile_summary`: source-attributed timings when `--profile-lines` is used

Warmup calls are included in manual line profile entries, but not in benchmark
case samples. This is useful for spotting cold-start costs, but it can make line
profile p95/p99 higher than the benchmark p95/p99.

## Input Latency Benchmarks

The `input` suite injects short Space key taps through SDL's event queue into
the real `App::run` loop. A producer thread varies tap timing independently of
the render loop. Rendering is paced with a clock, with no renderer or display.
These are **software latency measurements**, excluding keyboard hardware, OS
delivery before SDL enqueue, GPU work, VSync, and display scanout.

Run all four fixed-rate cases and the configurable case:

```powershell
.\build\bin\kin_bench.exe --suite input --iterations 80 --warmup 5 --json out\bench\input_latency.json
```

The fixed cases are `latency_60_60`, `latency_60_144`, `latency_120_144`, and
`latency_60_240` (simulation Hz, render Hz). Use `--case latency` to configure
rates; it defaults to 60 Hz simulation and 144 Hz rendering:

```powershell
.\build\bin\kin_bench.exe --suite input --case latency --input-sim-hz 120 --input-render-hz 240 --iterations 80 --warmup 5 --json out\bench\input_120_240.json
```

Both rate flags accept integers from 10 to 1000 and apply only to `latency`.
For this suite, `--iterations` is the number of measured taps and `--warmup`
is the number of initial taps discarded in the same loop. Prefer at least 80
measured taps for useful percentiles. Runs take real wall-clock time: taps are
spaced by two periods of the slower consumer plus a varying 0–17 ms offset.

Each case reports four event-to-observation distributions in milliseconds:

- `event_to_poll`: SDL event timestamp to input detection.
- `event_to_update`: first fixed update observing the mapped action's press.
- `event_to_render_read`: first render callback observing the frame press.
- `event_to_render_state`: first render callback observing the updated state.

The normal benchmark row uses `event_to_update`. Detailed text includes median,
p95, p99, maximum, missing observations, duplicate observations, and SDL push
errors. JSON keeps the `kin.benchmark/1` envelope and adds `input_latency` to
each input case, with rate metadata, metric summaries, and per-tap timings.
Missing per-tap timings are `null`, never zero; inspect `input_latency.ok`.
Warmup is excluded from distributions and tap failure counters. Unknown event
timestamps are always flagged.

Input cases do not track allocations or generate line-profile events:
allocation columns display `n/a`, JSON sets `allocation_tracking: false`, and
`--fail-on-allocation` is rejected. Exit code 3 means an input case had missing,
duplicate, unknown, or unsuccessfully queued events; reports are still written.
A long host scheduling stall can merge several taps before the consumer runs,
so investigate such failures before attributing them to the engine. There is
no hard latency threshold: timings depend on the host scheduler and load.

## Manual Markers

The [interactive performance examples](../examples/README.md) provide arena
combat and a virtualized training dashboard. Build with `KIN_BUILD_EXAMPLES=ON`
and run `kin_bench --suite examples` for their shared CPU workloads, or launch
either app with `--headless --profile` to measure full headless rendering.

Include:

```cpp
#include <kin/core/profile.hpp>
```

Use:

```cpp
KIN_PROFILE_FUNCTION();
KIN_PROFILE_SCOPE("pathfinding.rebuild");
KIN_PROFILE_LINE("tilemap.cached_partial_submit");
```

Marker behavior:

- `KIN_PROFILE_FUNCTION()` records the current function name.
- `KIN_PROFILE_SCOPE("name")` records the enclosing lexical scope.
- `KIN_PROFILE_LINE("name")` records the exact source file and line where the macro appears.
- All three compile to no-ops unless `KIN_ENABLE_PROFILING=ON`.
- Runtime executables only collect manual marker events when run with `--profile-lines`.

## Reading Results

Text output is sorted by total time. Useful columns:

- `calls`: number of recorded samples
- `total`: total milliseconds across all calls
- `median`: typical call cost
- `p95` / `p99`: tail cost
- `source`: file and line for the marker or runtime phase

JSON output is the source of truth for agents and tooling:

- `kin.profile/1` for full-game profiles
- `kin.benchmark/1` for benchmark runs

Generated reports should go under `out/profile` or `out/bench`; `out/` is ignored
by Git.
