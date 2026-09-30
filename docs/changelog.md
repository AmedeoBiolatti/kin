# Changelog

All notable changes to kin are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and kin uses
[Semantic Versioning](https://semver.org/) — while the version is 0.x, minor
releases may change APIs.

## [Unreleased]

### Added

- `Renderer2D::id()`: unique for the life of the process, never reused.
- `kin_bench` cases `render.submit_flush_sprites_{2k,8k,32k}`: submit and
  flush, the everyday path.

### Changed

- `RenderQueue::flush` draws in sorted order without moving the commands, so
  `commands()` keeps submission order after a flush. Call `sort_commands()`
  first if you need the commands themselves sorted.

### Performance

- Render queue sorting is 2–2.5× faster: keys are radix sorted, a queue that
  has not changed since its last sort is not sorted again, and `flush` skips
  reordering the commands. Submitting and flushing 32k sprites takes 4.2 ms
  instead of 9.7 ms.

### Performance

- ECS inspection is 5× faster: `query_entities` and `EcsWorld::snapshot()`
  read each entity's parent and name once instead of on every comparison while
  sorting. A query matching 4.5k of 10k entities takes 1.7 ms instead of 9 ms.

### Fixed

- GPU games no longer crash or hang on exit. Textures that outlive their
  renderer, such as the glyph atlases in the static system-font cache, skip
  their release once the GPU device is destroyed instead of calling into it.
- Font caches key on `Renderer2D::id()` rather than the renderer's address, so
  a new renderer created where an old one lived no longer draws text with the
  old renderer's textures.

## [0.2.0] — 2026-09-30

Features the first game built on kin needed: hex grids, 2D lighting, a job
system shared by the whole engine, and shaders with more textures, larger
uniform blocks and data textures. CI now also runs the GPU backend's tests.

### Upgrading from 0.1

- `ShaderParams::uniforms` is a `std::vector<f32>`: code that indexes it is
  unchanged; code that relied on it being a `std::array` needs updating.
- The ECS scheduler, `AssetServer` and `PathServer` share
  `default_job_system()`. `worker_count` in `AssetServerConfig` and
  `PathServerConfig` now limits concurrent work rather than starting threads.
- Destroying an `AssetServer` or `PathServer` drops work that has not started.
- A scripted `press` is released at the start of the next tick, so games see
  its release edge one tick later than before.

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
- Data textures for shaders: `create_texture(size, format, pixels)` with
  `TextureFormat::R16Uint`, `Rg16Uint` and `R32Float` (on backends with
  `capabilities().data_textures`); `update_texture()` writes in the texture's
  format.
- Shader uniform blocks up to `MaxShaderUniformFloats` (4096 floats, 16 KiB).
- `frame.timing` server command: update and render time over the last 600
  ticks.
- ui2 tooltips warm up: once one has shown, the next shows at once
  (`TooltipOptions::warm_frames`).

### Changed

- `ShaderParams::uniforms` is a `std::vector<f32>` (16 floats by default)
  instead of a `std::array<f32, 16>`. Indexing it is unchanged.
- On the SDL_GPU backend, sampler slots a material shader declares but a draw
  leaves empty are bound to a white texture instead of being left unbound.
- The ECS scheduler, `AssetServer` and `PathServer` run on one shared job
  system, `default_job_system()`, instead of each starting its own threads.
  `set_default_job_system()` replaces it; each also accepts its own pool. The
  `worker_count` settings now limit how much work each runs at once.
- `AssetServer` and `PathServer` drains do queued work on the calling thread
  while they wait. Destroying either drops work that has not started instead
  of finishing it first.
- Every `GameInfo` member has a default, so designated initializers can omit
  any without `-Wmissing-field-initializers` warnings.

### Fixed

- A scripted `press` (`input.mouse`, `input.action`) is released at the start
  of the next tick instead of at the end of its own, so the release is seen and
  ui2 clicks complete. Before, a pressed button never produced a click.

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

[Unreleased]: https://github.com/AmedeoBiolatti/kin/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/AmedeoBiolatti/kin/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/AmedeoBiolatti/kin/releases/tag/v0.1.0
