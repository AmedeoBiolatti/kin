# Changelog

All notable changes to kin are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and kin uses
[Semantic Versioning](https://semver.org/) — while the version is 0.x, minor
releases may change APIs.

## [Unreleased]

### Added

- `draw_shader_surface()` takes a list of source textures, bound at fragment
  sampler slots 0, 1, 2, ... up to `MaxShaderSamplers` (16); previously at most
  two.
- `JobSystem` (`kin/core/jobs.hpp`): background jobs with `Job<T>` handles,
  results applied in submission order by `pump()` / `drain()`, cancellation
  through `std::stop_token`, and `parallel_for`. Background jobs use at most
  part of the workers, so parallel loops always have threads.
- Hex grids: axial coordinates, directions, distance, rotation, rings, ranges
  and lines (`kin/core/hex.hpp`); `HexLayout` for pixel and offset-cell
  conversion in pointy-top and flat-top, odd and even offset layouts.
- Hex pathfinding: `HexGridNav`, `find_path` (A*) and `reachable_cells`
  (movement range).
- `games/hex_demo`, and a hex grids guide (`docs/hex_grids.md`).
- 2D lighting: `LightLayer` multiplies a drawn scene by an ambient colour plus
  `Light2D` point lights, optionally shaped by a texture (cones, spotlights).
  Works on the software and GPU backends without shaders.
- `games/lighting_demo`: lamps, a campfire, a flashlight and a day/night cycle.

### Changed

- On the SDL_GPU backend, sampler slots a material shader declares but a draw
  leaves empty are bound to a white texture instead of being left unbound.
- The ECS scheduler, `AssetServer` and `PathServer` run on one shared job
  system, `default_job_system()`, instead of each starting its own threads.
  `set_default_job_system()` replaces it; each also accepts its own pool. The
  `worker_count` settings now limit how much work each runs at once.
- `AssetServer` and `PathServer` drains do queued work on the calling thread
  while they wait. Destroying either drops work that has not started instead
  of finishing it first.

## [0.1.0] — 2026-09-28

First public release.

### Added

- ECS on flecs: native and data components, prefabs, events, inspection, and a
  system scheduler with dependency graphs and parallel batches.
- SDL3 2D rendering: sprites, tilemaps (orthogonal and isometric), particles,
  post-processing, and an animation system with state machines.
- `ui2` immediate-mode UI with widgets, layout, and themes.
- Box2D physics, Lua scripting (sol2), audio, pathfinding (including the
  threaded `PathServer`), dialogue, save data, and hot-reloadable assets.
- Agent interface shared by every `run_scene_app` game: deterministic headless
  runs with JSON reports, a line-JSON/HTTP control server, screenshots, and
  profiling.
- Demos (`games/`), the Signal Siege and Run Observatory examples, `kin_bench`,
  and the engine test suite.

[Unreleased]: https://github.com/AmedeoBiolatti/kin/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/AmedeoBiolatti/kin/releases/tag/v0.1.0
