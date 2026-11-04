# Kin 2D Workshop

A playable integration example: move with WASD/left stick, fire with Space/A,
and keep drones away as they navigate through the wall gap. Start joins a controller.
The control room supports pointer, keyboard/controller focus and semantic actions.
It includes rebinding, cancelable room preloads, reduced motion and cache budgets.

The character uses a triangulated movement blend space, an additive arm layer
and a bone attachment. Rooms use normal/specular lighting and polygon shadows,
with SDL_GPU evaluation or the portable CPU fallback. Lua owns projectile
tweens, pooled leases, contexts, rebinding, preloads and rumble.

Requires scripting, image I/O, Python and Pillow (`tools/requirements-imports.txt`).
It can be built independently of the other examples/audio:

```sh
cmake -S . -B build -DKIN_BUILD_EXAMPLES=OFF -DKIN_BUILD_2D_WORKFLOW=ON
cmake --build build --target kin_2d_workflow
build/bin/kin_2d_workflow
```

`art/imports.json` packs the source sprites into a native catalog/atlas. Build
`kin_2d_workflow_watch_2d` to watch imports; the running actor cache reloads changes.
Room loads keep immutable snapshots. `art/generate.py` regenerates the small
procedural source images, rig and room definitions using standard Python.

The memory panel shows the actor cache and room cache separately. Applying
1 KiB budgets retains their live allocations and reports the overrun. Native
residency rows explain component/catalog pins; catalogs deliberately retain their
textures until the scene cache closes. This is payload accounting, excluding
driver allocations, mip chains and other independent caches.

```sh
KIN_RENDER_BACKEND=sdl SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  build/bin/kin_2d_workflow --self-test
cmake --build build --target kin_2d_workflow_content
build/bin/kin_2d_workflow --self-test --pack build/bin/kin_2d_workflow.kinpak
```

The 480-frame playthrough checks projectile reuse, stale leases, two prepared-room
commits, navigation through the gap, six semantic controls and retained assets
under budget pressure. It runs as `kin-workflow-demo` in CTest. Native SDL_GPU
runs also assert that the lighting shader executes. Add `--screenshot output.png`
to save its final frame, or `--content path` to select another content root.

The Workshop also exposes the standard driven scene server and read-only
windowed inspector. [Gameplay scenarios](../../docs/automation.md) exercise
scoped UI actions, mapped input, world queries, loading, pause and reset:

```sh
cmake --build build --target kin_scenario
python3 tools/automation/kin_test.py run examples/2d_workflow/tests/workshop.json \
  --out /tmp/workshop-scenario --trace-world --game build/bin/kin_2d_workflow
python3 tools/automation/kin_test.py replay /tmp/workshop-scenario/commands.jsonl \
  --out /tmp/workshop-replay --game build/bin/kin_2d_workflow
ctest --test-dir build -R kin-workflow-automation --output-on-failure
```

`tests/workshop.lua` uses the same runner with `--lua-compiler build/bin/kin_scenario`.
`--automation-record session.jsonl --seed=7` writes a native driven RPC recording.
Server sessions keep rebinding profiles under the session's `user-data` directory.

For [safe live development](../../docs/hot_reload.md), run
`cmake --build build --target kin_2d_workflow_dev`. It starts the game and import
watcher together. Lua state, live projectile leases and room progress survive
accepted edits; invalid Lua/assets retain the running generation. The
`kin-workflow-development` CTest exercises the complete loop and replay.
