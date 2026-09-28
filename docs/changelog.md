# Changelog

All notable changes to kin are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and kin uses
[Semantic Versioning](https://semver.org/) — while the version is 0.x, minor
releases may change APIs.

## [Unreleased]

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
