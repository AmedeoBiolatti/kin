# Interactive performance examples

Two asset-free native applications share workload code with `kin_bench`.
They are examples, playable/interactive demos, and reproducible performance
fixtures. Neither requires downloaded assets, an account, or a network service.
Run Observatory uses system fonts with the engine's bitmap fallback.

## Build and run

From the repository root (Release recommended):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DKIN_BUILD_EXAMPLES=ON -DKIN_BUILD_BENCHMARKS=ON
cmake --build build --config Release --target signal_siege run_observatory kin_bench
```

Run `build/bin/signal_siege` or `build/bin/run_observatory`. On Windows use
`.exe`; multi-configuration generators may place them in `build/bin/Release/`.
`KIN_BUILD_EXAMPLES` defaults to ON when kin is the top-level project and is
independent of `KIN_BUILD_GAMES`.
Set it OFF to omit both apps and their optional `kin_bench` cases.

## Signal Siege

A top-down arena survival game. Survive 90 seconds against three enemy classes:
fast chasers, slow armored units, and ranged orbiting units. Enemy speed grows
over six wave phases. Killed enemies respawn to sustain the configured load.
Collect green cores to repair the player and improve weapon fire rate; ten
cores unlock a second projectile. Dash grants brief immunity.

It opens on a title screen over an autoplaying arena: **Play**, **Watch
autoplay** or **Quit** (arrows or W/S and Enter, or the mouse). A run that ends
fades to a results screen (time survived, kills, cores, upgrades) whose counters
roll up; Enter retries, Escape returns to the title. The best run is kept between
sessions with kin's `SaveStore`. Screens change with kin's scene transitions
(iris, dissolve, wipe, crossfade; a plain fade on the software backend).

- WASD: move; mouse: aim; hold left mouse: fire.
- Space: dash (1.5-second cooldown); P: pause; R: restart with the same seed.
- B: toggle invincible autoplay; Escape: back to the title (quits when the run was
  started from the command line); F1: engine timing overlay.
- U: open/close the Power Grid (simulation pauses); Escape returns from the grid.
- On the grid, click a square or use Left/Right to select, then click Install or
  press Enter. Connected prerequisites must be installed first. Three starting
  cores let you choose one branch immediately; collected cores fund more nodes.
  There are **36 purchasable upgrades**, six per connected branch: Mobility,
  Weapons, Salvage, Ballistics, Defense, and Recovery. Effects include movement,
  dash duration/speed/cooldown, damage, fire cadence, spread volleys, projectile
  speed/lifetime, attraction, armor, maximum health, regeneration and core repairs.
  Tier costs are 3, 5, 6, 8, 10 and 12 cores. All upgrades reset with R.
  Six always-visible tabs select a branch. Its six connected nodes fit in two
  rows on small windows and one row on wide windows; no panning is needed.
  Left/Right chooses an upgrade; Up/Down or the wheel switches branch. Main Enter
  and keypad Enter install, with immediate success or failure feedback.
  No exclusive branches or permanent purchases.
- `--power-grid`: start on the upgrade screen, also usable for screenshot runs.
- `--mute`: no sound.

Headless, `--benchmark`, `--screenshot` and `--power-grid` runs skip the title and
start an autoplaying arena, as before. They and scene-server runs are silent and
never read or write the saved best run.
- Default: 300 enemies. `--stress`: 1,200. `--enemies N`: up to 5,000.

Uses ECS transforms/renderers, a cached movement query, a spatial grid for
projectile collision, bounded projectile/effect/pickup storage, camera culling,
render-queue sorting, and a HUD. The playfield is 3,072 x 2,048 world units.
Both apps run simulation at 120 Hz; UI interactions run during rendering.

Its look is built at startup with no asset files (`arena_art.cpp`), and shows off
several kin systems:

- **Sprites** rasterized from signed-distance shapes: the player ship, a star, an
  armored plate and a ring for the three enemy classes, cores, shots and steel deck
  plates. Enemies are drawn by the ECS render system through `TextureRenderer`.
- **2D lighting** (`LightLayer`) of the environment: a dim ambient, the player's
  light and a flashlight cone that follows the aim, amber reactor lamps, impacts,
  and orbiters about to fire.
- **Particles** (`ParticleSystem`) from the arena's events: hit sparks, kill
  bursts in each enemy's colour, core pickups, dashes, damage, and twin thruster
  plumes, drawn additively.
- **Post-processing** on the GPU backend: bloom, a vignette and a light grade.
  The Power Grid screen turns it off to keep its text crisp; the software backend
  presents without it.

Colour keeps the screen readable at 300 enemies: cyan and white are only the
player (the ship, drawn over everything with a ground ring, and its streaking
shots); enemies are warm, solid shapes drawn at full colour after the lighting
(red stars, orange plates, purple rings) with no glow; enemy bullets are the one
round, white-cored hot pink-red thing; cores are small green gems that blink
before they expire; an orbiter about to fire shows a closing ring. The floor stays
neutral steel, and only shots, cores and effects glow.

The art only reads the simulation: collisions, randomness and checksums are the
same with or without it, and the `kin_bench` arena cases, which never attach it,
measure the plain render path. Effects advance with simulation time, so pausing
freezes them, and there is no camera shake or full-screen flash to disturb aim.
On the GPU the look costs well under a millisecond a frame; SDL's software
renderer, used headless, takes about 25 ms a frame at the default load.

Sound is synthesized at startup (no audio files) and played with kin's
`AudioEngine`: shots, hits, kills, pickups, dashes and damage, positioned around
the player so distant fights are quieter, plus wave, win, lose and menu cues and
a looping ambient drone. The clips are registered with `AudioEngine::add_clip`.

The HUD is built from ui2 widgets: glass panels, hull and dash bars (the hull bar
shifts colour and flashes when hit), a countdown with wave progress, rolling kill
and core counters, an "upgrades ready" notice, a `TargetReticle`, and a wave
banner animated with kin animation property tracks. The controls fade out after
the opening seconds and return while paused.

The arena keeps a fixed 1,280 x 800 logical view, letterboxed to fit the window.
Its HUD uses native-resolution system text and automatic display scaling;
smaller windows wrap controls instead of shrinking the font. Pause/end panels
scale with the HUD, while movement, aiming, and the visible playfield remain
independent of UI scale. `--ui-scale 1.5` optionally overrides automatic scaling
for this app too.

## Run Observatory

A simulated LLM training tracker. Search experiments by name, model, or status;
click a row to inspect it; sort by loss; pause/resume the selected run or the
global telemetry stream. Scroll the run table with the mouse wheel. Space
toggles telemetry when the search field is not active.

The dashboard shows four summary cards, a virtualized table, three loss curves,
and bounded event logs. Only visible rows are drawn, while the complete run
dataset remains available for filtering and sorting. Telemetry updates at 4 Hz.
Default: 1,000 runs; `--stress`: 10,000; `--runs N`: up to 100,000.
This is simulated data, not a connection to real training infrastructure.

Display scaling is detected automatically and refreshed when resizing or moving
between monitors. Text is rasterized at the current pixel density, controls and
hit areas scale together, and charts share the available height with the logs.
Resize the window, or use `--width 960 --height 640`. The minimum content area is
640 x 480 density-independent units, so the pixel minimum grows with scaling.
An optional `--ui-scale 1.5` override is available for screenshots and testing;
normal use needs no scale or resolution configuration.
`--benchmark --scenario idle|live|scroll|churn` enables scripted workloads:

- `idle`: freeze telemetry and display the existing dataset.
- `live`: update active runs, histories, and logs.
- `scroll`: live telemetry plus continuous scripted table scrolling.
- `churn`: scrolling, search changes, sorting, and selection changes.

## Reproducible benchmarks

Run from the repo root. Reports and screenshots belong in ignored `out/`.
Headless mode automatically uses arena autoplay and tracker scripted scenarios.
`--benchmark` enables the same behavior in an ordinary visible window.

```sh
build/bin/signal_siege --benchmark --stress --profile --frames 1200 --seed 7 --report out/bench/arena-state.json --profile-json out/bench/arena-profile.json
build/bin/run_observatory --benchmark --stress --scenario churn --profile --frames 1200 --seed 7 --report out/bench/tracker-state.json --profile-json out/bench/tracker-profile.json
build/bin/kin_bench --suite examples --iterations 120 --warmup 5 --json out/bench/examples.json
```

Runtime `--profile` includes update, rendering, and presentation timing using
SDL's headless software renderer. A visible run is necessary to evaluate the
GPU backend and display pacing. `--vsync` requests synchronized presentation;
default presentation is unsynchronized. A profile run does not measure physical
key-to-screen latency; use the separate `input` suite for software input timing.

`kin_bench` includes:

| Case | One measured sample |
| --- | --- |
| `arena_1200_encounter` | Reset seed 7, 120 autoplay steps, collect and sort one view |
| `arena_5000_encounter` | Same encounter with 5,000 enemies |
| `tracker_idle_10k` | Draw an idle 10,000-run dashboard through the null backend |
| `tracker_scroll_10k` | Advance telemetry and scrolling, then draw the dashboard |
| `tracker_churn_10k` | Change search/sort/selection, advance telemetry, then draw |

Arena numbers are **milliseconds per encounter**, not per frame. Tracker
numbers are CPU preparation/render-dispatch costs, excluding GPU presentation.
Live tracker cases evolve across samples; keep warmup and iteration counts
identical for comparisons, and use at least 120 iterations to cover telemetry
updates. Churn deliberately changes filters every benchmark sample, more often
than the interactive scripted scenario. Seeded run reports include checksums
for repeatability on the same build/platform. Floating-point simulation is not
promised to be bit-identical across architectures.

## Capture and verify

```sh
build/bin/signal_siege --headless --stress --frames 360 --seed 7 --screenshot out/bench/arena.png --capture-frame 360
build/bin/run_observatory --headless --stress --frames 120 --seed 7 --screenshot out/bench/tracker.png --capture-frame 120
build/bin/run_observatory --headless --width 3840 --height 2160 --ui-scale 2 --frames 120 --screenshot out/bench/tracker-4k.png
cmake --build build --config Release --target kin_example_tests
ctest --test-dir build -C Release -R kin-examples --output-on-failure
```

Capture frame numbers count rendered frames. The runtime's first frame enters
the scene without updating it, so a run of N frames performs N-1 simulation
steps. Screenshots are captured before presentation. The tests cover seeded
replay, combat, bounds/capacity, filtering/sorting, pause behavior, virtualized
row counts, and mouse/text actions in a real headless UI without simulation.
Scale-change tests cover 100%, 125%, 150%, and 200%, including clicking and
scrolling. On a desktop session, `kin_example_tests --native-display` checks
automatic scaling and pointer mapping using the selected native renderer.
Use `kin_example_tests --native-game` for the game's native HUD and restored
aim-coordinate checks; it also accepts an optional screenshot path.
On X11, prefix that command with `SDL_VIDEO_X11_SCALING_FACTOR=2` to emulate a
200% display content scale; append a PNG path to capture the result.
When capturing headlessly without `--frames`, the app runs through the requested
capture frame automatically.
