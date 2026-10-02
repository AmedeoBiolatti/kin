# Kin

[![CI](https://github.com/AmedeoBiolatti/kin/actions/workflows/ci.yml/badge.svg)](https://github.com/AmedeoBiolatti/kin/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Kin is a C++23 game engine for humans and AI agents alike. It targets small,
incremental **2D** games — turn-based strategy, card, and puzzle games — and is
built to be driven from code, the command line, and automated agents. It starts
small, stays honest about its scope, and grows alongside the games built with it.
[The manifest](docs/manifest.md) explains the design philosophy.

> **Status — v0.2.3.** The engine, its test suite, demos (`games/`) and two
> performance examples (`examples/`). 0.2 adds hex grids, 2D lighting, a shared
> job system and richer shaders; 0.2.1 adds a multi-line text editor, faster
> rendering and tilemaps, and fixes a GPU crash on exit; 0.2.2 draws large
> numbers of sprites far faster (instanced batches, a compact render queue,
> worker threads) and reworks the Signal Siege example; 0.2.3 adds hot reload of
> a game's data files, Lua for code outside the ECS, ui2 themes from data files,
> file drops, dialogs and child processes, a frame rate cap and GPU frame
> timing; see the
> [changelog](docs/changelog.md). APIs are young and may change between minor
> versions.

![Signal Siege: the cyan player ship, marked by a ground ring, fires at red star, orange plate and purple ring enemies while pink enemy bullets stream across a dark steel deck](docs/images/signal_siege.png)

<sub>Signal Siege, an arena survival example: a 1280x800 frame from kin's GPU backend, captured through the scene server
while its autoplay benchmark runs: <code>KIN_RENDER_BACKEND=gpu signal_siege --server --benchmark --seed=7 --mute</code>,
then <code>sim.tick</code> and <code>view.screenshot</code> requests.</sub>

## Features

- **ECS on [flecs](https://github.com/SanderMertens/flecs)** — native and
  data-defined components, prefabs, events, a system scheduler with dependency
  graphs and parallel batches, and live world inspection.
- **2D rendering on SDL3** — sprites, tilemaps (including isometric), particles,
  2D lighting, post-processing, and an animation system with state machines.
- **UI (ui2)** — immediate-style widgets, layout, and themes.
- **Physics** with Box2D, **scripting** in Lua (sol2), **audio**, **pathfinding**
  (square and hex grids), **hex grid** coordinates and layouts, **dialogue**,
  **save data**, hot-reloadable **assets**, and **background jobs** and parallel
  loops on a worker pool.
- **Agent interface** — every game gets deterministic headless runs with a JSON
  report, a line-JSON/HTTP control server, screenshots, and profiling from the
  same command-line flags ([docs/agent_interface.md](docs/agent_interface.md)).

## Platforms

Windows (MSVC 2022) and Linux (GCC 13+ or Clang 18+) are supported and built in
CI. Other platforms SDL3 supports may work but are untested.

## Building

Requirements: CMake 3.22+, a C++23 compiler, Git, and network access on the
first configure — CMake fetches SDL3, SDL3_image, SDL3_ttf, flecs, Box2D, Lua and
sol2 with `FetchContent`.

On Linux, SDL3 also needs the windowing and audio development packages. On
Debian/Ubuntu:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
    libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols \
    libdecor-0-dev libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev \
    libdrm-dev libgbm-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libdbus-1-dev libudev-dev libibus-1.0-dev
```

Configure, build, and test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure -j 8
```

On Windows, run these from a Visual Studio developer prompt. Executables land in
`build/bin/` (`build/bin/Release/` with multi-config generators). To run the
tests without a display, set `SDL_VIDEO_DRIVER=dummy` and
`SDL_AUDIO_DRIVER=dummy`.

| Option | Default | Effect |
|---|---|---|
| `KIN_BUILD_GAMES` | top-level only | Demos in `games/` |
| `KIN_BUILD_EXAMPLES` | top-level only | Performance examples in `examples/` |
| `KIN_BUILD_BENCHMARKS` | top-level only | `kin_bench` and other benchmark runners |
| `KIN_BUILD_TESTS` | top-level only | Engine tests (with `BUILD_TESTING`) |
| `KIN_ENABLE_PROFILING` | `OFF` | Manual profiling macros ([docs/profiling.md](docs/profiling.md)) |
| `KIN_ENABLE_RENDER_PROBE` | `ON` | Headless render probe, `--probe-render` ([docs/testing.md](docs/testing.md#render-probe)); turn off for shipping builds |

## Demos and examples

- `games/ecs_systems_demo`, `games/ecs_graph_demo`, `games/ecs_parallel_bench` —
  the ECS system scheduler, from a simple pipeline to parallel batches.
- `games/isometric_demo` — isometric tilemap rendering and draw ordering.
- `games/hex_demo` — hex coordinates, picking, movement range and A* paths in
  all four hex layouts.
- `games/lighting_demo` — ambient light, point lights and a flashlight at night.
- [examples/](examples/README.md) — **Signal Siege**, an arena survival game, and
  **Run Observatory**, a simulated LLM training tracker. Both support
  interactive use, seeded headless runs, screenshots, and benchmark workloads
  shared with `kin_bench`.

![Run Observatory: a simulated training dashboard with a 10,000-row run list, loss chart and event stream](docs/images/run_observatory.png)

<sub>Run Observatory with 10,000 simulated runs, built with kin's ui2 widgets.</sub>

Try the agent interface on a demo:

```sh
./build/bin/ecs_systems_demo --headless --frames=120 --seed=7 --report=-
```

## Using kin in a game

A game is its own project that adds kin's source tree with `add_subdirectory`.
As a subproject kin builds only its engine: its demos, examples, benchmarks and
tests default to on only when kin is the top-level project.

With `FetchContent`:

```cmake
cmake_minimum_required(VERSION 3.22)
project(my_game LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)

include(FetchContent)
FetchContent_Declare(kin
    GIT_REPOSITORY https://github.com/AmedeoBiolatti/kin.git
    GIT_TAG v0.2.3
)
FetchContent_MakeAvailable(kin)

add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE kin::engine)
kin_configure_game_assets(my_game my_game)   # assets/ beside the CMakeLists
```

Or with a local checkout beside the game:

```cmake
set(KIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../kin" CACHE PATH "Source tree of the kin engine")
add_subdirectory("${KIN_DIR}" kin)
```

Kin has no install rules yet, so `find_package(kin)` is not supported.

## Documentation

| Topic | |
|---|---|
| Agents and automation | [agent_interface](docs/agent_interface.md), [testing](docs/testing.md), [profiling](docs/profiling.md) |
| ECS | [entities](docs/ecs_entities.md), [components](docs/ecs_components.md), [data components](docs/ecs_data_components.md), [systems](docs/ecs_systems.md), [events](docs/ecs_events.md), [prefabs](docs/ecs_prefabs.md), [inspection](docs/ecs_inspection.md), [editor workflow](docs/ecs_editor_workflow.md) |
| Engine | [rendering](docs/rendering.md), [animation](docs/animation_system.md), [UI](docs/ui.md), [audio](docs/audio.md), [assets](docs/assets.md), [scripting](docs/scripting.md), [files and processes](docs/platform.md), [background jobs](docs/jobs.md), [hex grids](docs/hex_grids.md) |
| Project | [manifest](docs/manifest.md), [changelog](docs/changelog.md), [third-party notices](docs/third_party_notices.md) |

## Contributing

Issues and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md)
and the [Code of Conduct](.github/CODE_OF_CONDUCT.md). Changes are recorded in the
[changelog](docs/changelog.md).

## License

Kin is released under the [MIT License](LICENSE). Its dependencies are under
their own permissive licenses; see [docs/third_party_notices.md](docs/third_party_notices.md).
