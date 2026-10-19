# Inspecting a 3D asset

`kin_asset_preview` opens one glTF/GLB using the same `Assets3D`, material
bindings and renderer as a game. It needs a build with `KIN_EXPERIMENTAL_3D=ON`
and the GPU backend. It downloads no content and creates no physics world.

```sh
cmake --build build --target kin_asset_preview -j1
KIN_RENDER_BACKEND=gpu build/bin/kin_asset_preview \
  --model examples/content/models/crate.glb --hud

# A game's root makes sidecar texture/shader paths resolve as they do in-game.
KIN_RENDER_BACKEND=gpu build/bin/kin_asset_preview \
  --root /path/to/xc121/assets --model 3d/models/lander.glb --hud
```

The matching `.kinmaterials` sidecar is loaded automatically when present.
Use `--materials FILE --variant NAME` to select another binding or variant.
Without `--root`, the model's parent is the asset root; supply the game root
when sidecars refer to paths such as `3d/materials/...`.

| Control | Action |
|---|---|
| Right drag / middle drag / wheel | Orbit / pan / zoom |
| Home | Frame the complete asset and reset the camera/clock |
| Tab | Cycle inspection mode |
| Period | Cycle individual parts, then return to all parts |
| B / G | Bounds / ground plane |
| S / A | Sun shadows / ambient occlusion |
| O | Perspective / orthographic |
| Space | Freeze camera input, turntable and animation clock |
| R | Reload model, textures, shader and material binding |
| H | Preview information panel |
| F1 / F2 | Existing runtime diagnostic overlay / timing capture |

The modes are `lit`, `base`, `normals`, `uv`, `metallic`, `roughness`, `alpha`
and `wireframe`. All except wireframe reuse the decoded inputs of the stock
material shader. Normals include the normal map in world space; UVs show a
ten-cell checker and coordinate colours; metallic and roughness show the
effective PBR factors times their maps (including Kin's roughness floor).
Non-base diagnostic modes show opaque surfaces while retaining cutout masks.
They intentionally replace a custom game shader; lit mode preserves it.
Custom game shaders that need game-supplied data/time still need that game
context, and the preview is not a full simulation of such effects.
The data modes need the inspection shader compiled by `glslc` or supplied through
`KIN_SPIRV_DIR`; lit and wireframe remain usable without that optional shader.

The floor and camera frame the model in its authored units. The viewer translates
the bottom centre onto the floor without rescaling it; the HUD prints its original
dimensions. Skinned parts use their rest poses, or `--clip NAME --time SECONDS`
samples the chosen animation using the existing sampler. A turntable rotates the
camera with `--turntable DEGREES_PER_SECOND` on the fixed application clock.

## Repeatable captures

```sh
KIN_RENDER_BACKEND=gpu build/bin/kin_asset_preview \
  --root /path/to/game/assets --model 3d/models/lander.glb \
  --mode normals --no-ground --width 1920 --height 1080 \
  --headless --frames=2 --screenshot=/scratch/lander-normals.png \
  --report=/scratch/lander-normals.json
```

Use `--yaw`, `--pitch`, `--ortho`, `--part N`, `--ao`, `--bounds`, `--no-shadows`
and `--no-ground` for a controlled comparison. `--inspect` also prints imported
part names, material factors/maps and importer diagnostics at startup. The run
report includes those fields, selected mode, actual render dimensions and counts.
Standard Kin `--server` actions use the `preview.*` names exposed by
`--list-actions`, so scripts can inspect, reload and capture the same session.
F1/F2 belong to the ordinary application runtime; the driven server has no runtime
overlay. Its `preview.hud` action provides the compact information panel instead.

Reload prepares a replacement before changing the active asset. Missing/invalid
models, bindings or clips retain the previous working model and report an error;
the HUD shows it even when otherwise hidden. Reload is explicit and does not poll
files or rebake Blender materials.

The viewer retains one asset, material table, bounded pose data and one view.
It rejects model sources over 32 MiB, more than 256 parts or 16 skeletons; these
are preview limits, not engine limits. A 2048px sun depth map costs 16 MiB before
driver overhead. MSAA is 2x; AO is opt-in at half resolution. Source/lookup limits
do not cap texture residency, and successful reload briefly retains the old and
candidate GPU resources. Use the project's external RSS guard for large assets.
The viewer does not retain CPU mesh copies for inspection or frame history.

## Adding 3D diagnostics to a game

```cpp
#include <kin/render3d/diagnostics3d.hpp>

if (renderer3d.render(view, scene)) {
    kin::record_render3d_diagnostics(ctx.debug_options, renderer3d.stats(), view.size());
}
```

This feeds the existing F1/F2 overlay and profiling sinks. It reports CPU render
recording time, draws/triangles/instances, culling, shadow passes, compute
dispatches, upload bytes, pipeline creation and separately named allocation
counters. It adds no resource ownership, GPU readbacks or history; absent sinks
do no formatting. For multiple views these fixed labels show the last published
view; aggregate or use separate custom status labels if all views are needed.

The runtime's existing `gpu.frame` measurement is asynchronous, covers the whole
frame and can average several frames when the GPU falls behind. It is not a
per-pass timestamp. `render3d.cpu` measures CPU submission, not GPU execution.
Feature-target, local-shadow and shared-mesh counters are individual reported
allocations, not total VRAM or process RSS. XC121's native view now publishes
these diagnostics through this helper; its normal hidden-overlay rendering is
unchanged.
