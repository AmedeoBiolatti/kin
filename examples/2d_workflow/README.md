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
