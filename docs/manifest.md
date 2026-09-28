# Kin Engine — Manifest

> A familiar built to grow with you.

Kin is a C++ game engine designed for humans and AI agents alike. It starts small, stays honest about its scope, and grows incrementally alongside the games built with it.

---

## Philosophy

### Composition over inheritance
Systems, components, and behaviors are assembled, not inherited. No deep class hierarchies. Logic lives in systems; data lives in components.

### Independent systems
Every subsystem (renderer, audio, physics, scripting, localization) is a self-contained module with a clean interface. Systems can be replaced, mocked, or omitted. Nothing is load-bearing by accident.

### AI-first
Kin is designed to be worked on and with by AI agents. This means:
- Headless execution mode for automated testing and simulation
- Structured, machine-readable logging and state introspection
- CLI tooling for every workflow (project creation, builds, asset pipeline)
- Clear, consistent APIs that are easy to generate and consume
- Live entity/world inspection via flecs REST API

Headless runs must be deterministic by default: fixed timestep, explicit frame
limit, explicit seed, stable process exit codes, and a JSON report suitable for
agent assertions. Visual output is optional in headless mode; state and events
are the test surface.

### Hot reload
Fast iteration is non-negotiable. Asset hot reload ships early. Script hot reload follows. The engine should never force a full restart for a texture swap or a Lua change.

### Scripting is a first-class citizen
Logic can be written in C++ or in script. Lua ships first. The scripting layer is an abstraction — additional languages (TypeScript, Python) can be added without rewiring the engine. Scripts are hot-reloadable.

### Localization from day one
No hardcoded strings. All user-facing text routes through a key lookup. Locale switching is a runtime operation. This is not a feature bolted on later.

### 2D first, 3D later
The engine targets 2D games at launch. Architecture decisions (renderer abstraction, coordinate systems, camera) are made with a future Vulkan/3D path in mind, but that path is not built until it is needed.

### Incremental complexity
Kin earns its features. Each system is added when a real game needs it, not speculatively. A turn-based strategy game and a card game should run before a particle system exists.

### Deterministic execution
All randomness is handled by a unique RngKey provided to a sceen using the provided utilities.

---

## Target

**Games:** Small, incremental 2D games — turn-based strategy, card games, puzzle games, and similar.

**Platforms:** Windows and Linux first. Steam Deck (Linux) follows naturally. Android is out of scope for now.

**Developers:** Solo developers and small teams, human or AI.

---

## Technology Choices

| Concern | Choice | Rationale |
|---|---|---|
| Language | C++23 | Performance, control, ecosystem |
| Platform layer | SDL3 | Modern, cross-platform, good 2D renderer |
| Renderer (now) | SDL3 renderer | Simple, sufficient for 2D |
| Renderer (future) | Vulkan | Power and portability for 3D |
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
- **Not a 3D engine** (yet). Architectural decisions respect the future; implementation does not rush it.
- **Not a framework for AAA games**. Kin optimizes for clarity, iteration speed, and agent-friendliness over raw enterprise scale.
- **Not opinionated about game structure**. Kin provides systems; game architecture is the developer's choice.

---

## Naming

The name *Kin* means familiar — a companion that grows with its maker. It reflects the relationship between the engine, the developer, and the AI agents that work alongside them.
