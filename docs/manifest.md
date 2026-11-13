# Kin Engine — Manifest

> A familiar built to grow with you.

Kin is a C++ game engine designed for humans and AI agents alike. It starts small, stays honest about its scope, and grows incrementally alongside the games built with it.

This page describes principles. For current project structure, supported runtime
paths and working examples, use the [project workflow](project_workflow.md).

---

## Philosophy

### Composition over inheritance
Systems, components, and behaviors are assembled, not inherited. No deep class hierarchies. Logic lives in systems; data lives in components.

### Independent systems
Subsystems have explicit dependencies and clean interfaces. Optional components
can be omitted; the dependency boundaries below identify the foundational APIs
and replaceable backend seams.

### AI-first
Kin is designed to be worked on and with by AI agents. This means:
- Headless execution mode for automated testing and simulation
- Structured, machine-readable logging and state introspection
- Shared build, asset and runtime CLI tooling, with a runnable project starter
- Clear, consistent APIs that are easy to generate and consume
- Live entity/world inspection via flecs REST API

Headless runs must be deterministic by default: fixed timestep, explicit frame
limit, explicit seed, stable process exit codes, and a JSON report suitable for
agent assertions. Visual output is optional in headless mode; state and events
are the test surface.

### Hot reload
Assets and Lua scripts support hot reload through shared development paths.
Use [development sessions](hot_reload.md) for reload and rollback rather than
implementing another watcher in each game.

### Scripting is a first-class citizen
Logic can be written in C++ or in script. Lua has shared gameplay, checking,
testing and reload workflows; see [Lua development](lua_development.md). Add
script hosts when real games need them, reusing the existing runtime boundaries.

### Localization from day one
No hardcoded strings. All user-facing text routes through a key lookup. Locale switching is a runtime operation. This is not a feature bolted on later.

### Incremental renderer and platform support
Kin supports 2D games and an experimental 3D path. Use the
[current 3D guide](rendering_3d.md) and its session lifecycle for 3D work. Extend
platforms and renderers for actual workloads, with explicit validation of the
supported configuration.

### Incremental complexity
Kin earns its features. Each system is added when a real game needs it, not speculatively. A turn-based strategy game and a card game should run before a particle system exists.

### Deterministic execution
All randomness uses the scene's seeded `RngKey` and the provided utilities. `fold_in(ctx.rng, ctx.step)` gives every update its own key, so a run that is replayed, rolled back or joined draws the same numbers again.

---

## Target

**Games:** Small, incremental 2D games — turn-based strategy, card games, puzzle games, and similar.

**Platforms:** Windows and Linux first. [Android](android.md) and
[browser exports](web.md) are experimental; use their current guides for status
and requirements.

**Developers:** Solo developers and small teams, human or AI.

---

## Technology Choices

| Concern | Choice | Rationale |
|---|---|---|
| Language | C++23 | Performance, control, ecosystem |
| Platform layer | SDL3 | Modern, cross-platform, good 2D renderer |
| Rendering | SDL3 GPU/software paths and experimental 3D | Shared runtime with explicit backend choices; see [rendering](rendering.md) |
| ECS | flecs | Built-in hierarchies, REST API, reflection, relationships |
| Physics | Box2D | Mature, well-understood, sufficient for 2D |
| Scripting | Lua (sol2) | Lightweight, hot-reloadable, proven in games |
| Build system | CMake | Standard, well-supported, IDE-friendly |

### Dependency boundaries

Not every dependency is hidden, and the line is deliberate:

- **Fully encapsulated and swappable.** SDL3 sits behind the renderer backend
  seam (`IRenderer2DBackend`) and platform layer; Box2D sits behind a pimpl in
  `kin::PhysicsWorld`. No `SDL_*` or `b2*` type appears in a public header. These
  could be replaced without touching game code.
- **Foundational and intentionally exposed.** flecs (ECS) and Lua/sol2
  (scripting bindings) are *not* hidden. The ECS wrappers (`EcsWorld`,
  `EcsEntity`, `EcsQuery`) are thin generic layers over flecs and must include
  `<flecs.h>`; the scripting component-binding layer is inherently sol-typed.
  Public Kin signatures use Kin types (`EcsWorld&`, `EcsEntity`, `ComponentId`)
  rather than raw flecs/sol types, but the underlying libraries are a load-bearing
  part of the API by design and are not swappable. `EcsWorld::raw()` /
  `EcsEntity::raw()` are the supported escape hatches into flecs for capabilities
  the wrappers do not yet surface.

---

## Non-Goals

- **Not a visual editor** (yet). Kin is a code-first engine. Editor tooling may come later.
- **No speculative engine features.** Experimental 3D grows with real game workloads and validated runtime paths.
- **Not a framework for AAA games**. Kin optimizes for clarity, iteration speed, and agent-friendliness over raw enterprise scale.
- **Default project conventions.** The starter and project guide provide a consistent foundation; game-specific architecture grows from actual responsibilities.

---

## Naming

The name *Kin* means familiar — a companion that grows with its maker. It reflects the relationship between the engine, the developer, and the AI agents that work alongside them.
