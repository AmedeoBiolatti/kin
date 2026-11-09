# Starting and growing a Kin project

Use the existing project's conventions when extending it. For a new game, start
with [the runnable starter](../games/starter/README.md): one executable, one scene,
and Kin's shared runtime. It builds inside this repository or as a standalone
project with a local Kin checkout. It needs no downloaded game assets.

## Find the closest working path

Before editing, identify the existing implementation to reuse, the smallest
change that meets the request, and the checks that will demonstrate completion.
For a small task, a few sentences in the task update are sufficient. Keep branch
scope and ownership in the existing task or PR; avoid creating parallel tracking
documents. Coordinate overlapping shared-code changes before building two versions.

| Need | Starting point | Current reference |
|---|---|---|
| Small native 2D game | [starter](../games/starter/) | [Scenes and runtime](scene_flow.md), [input](input.md) |
| 2D ECS gameplay | [ECS systems demo](../games/ecs_systems_demo/) | [Entities](ecs_entities.md), [systems](ecs_systems.md) |
| Lua scripts and isolated tests | [Lua testing example](../examples/lua_testing/README.md) | [Lua development](lua_development.md) |
| Lua 2D scene/content workflow | [2D workshop](../examples/2d_workflow/README.md) | [Assets](assets.md), [reload](hot_reload.md) |
| Native 3D game | [Camp Walk](../examples/camp_walk/), [Projectile Range](../examples/projectile_range/) | [3D session lifecycle](rendering_3d.md#owning-a-3d-scene-session) |
| Lua 3D gameplay | [Lua Range](../examples/lua_range/) | [3D scenes](rendering_3d.md), [Lua development](lua_development.md) |
| Packaging or another platform | Existing game and `kin_game` helpers | [Shipping](shipping.md), [Android](android.md), [web](web.md) |

Rendering viewers, benchmark workloads and large demo games are references for
their particular features. Copy only what the new game's requirements need.

## Default layout

Start with:

```text
my_game/
  CMakeLists.txt
  src/
    main.cpp
```

Add `content/` when there are authored assets/scripts, `tests/` when meaningful
regression cases need their own sources, and a README when it has useful setup or
behavior to explain. Keep generated output under `out/` and out of version control;
see [the build conventions](../AGENTS.md#builds-and-output).

Keep `main.cpp` as the entry point. As real responsibilities grow, move them into
files such as `gameplay.cpp` and `world.cpp`, then feature directories such as
`src/gameplay/`, `src/world/` and `src/ui/` when several files belong together.
Keep private headers beside their implementations. An `include/` tree is for an
actual library with external callers. Project size alone does not require more
executables, build systems, plugin layers or manager classes.

## Reuse and completion

Use `kin_game` for content and packaging, `run_scene_app` for the application
lifecycle and headless interface, and `SceneSession3D` for normal 3D scene
ownership/frame stages. Extend these paths when needed rather than duplicating
their infrastructure in a new game. Game-specific rules belong in the game.

Start with direct functions, concrete types and existing dependencies. Introduce
an abstraction when actual callers benefit or a real boundary needs protection;
add configuration for a real variation. Explain a new dependency, executable or
architectural layer by naming the current problem it solves. This is an ordinary
implementation decision, not an additional approval step.

Validate the requested behavior with [focused checks](testing.md), broaden them
when shared contracts or uncertainty require it, and finish when the completion
conditions are met. Record unrelated findings without adding refactors, platforms
or speculative features to the branch.

The current guides linked above and the public APIs describe supported workflows.
The [manifest](manifest.md) explains philosophy; dated measurements and historical
plans describe their recorded context. When they conflict with implemented
behavior, verify the current API and correct the relevant guidance.
