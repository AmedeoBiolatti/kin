# Camp content

The 3D starting points (`camp_walk`, `projectile_range`, `lua_range`) and the engine's 3D tests share this content: two
small sets of externally authored CC0 assets, and the scenes, prefabs, materials, colliders and animations made from them.
Builds read these files locally and never download content. Each set includes a license and manifest with source URLs and
checksums. (Summit, in the [kin gallery](https://github.com/AmedeoBiolatti/kin-gallery), started from the same set.)

## Props and collision

`kenney-survival/` contains 14 unmodified glTF models and one 512 × 512 colour atlas from
[Kenney Survival Kit 2.0](https://kenney.nl/assets/survival-kit). The original files and license total 242,947 bytes and
2,337 unique mesh triangles. The view keeps scene-owned `Assets3D` handles; the engine shares the atlas across models without
manual material replacement (approximately 1.33 MiB RGBA8
including mipmaps, excluding meshes and allocation overhead).

`camp_walk` additionally uses the existing bottle and box models for the runtime
`camp-supply.kinprefab` and `camp-crate.kinprefab`: three collectible sensor bottles
and a pushable dynamic crate. Their native C++ gameplay state is initialized by the
session's spawn callback. No additional downloaded art or textures are required.

Reproduce the selection with Python's standard library:

```sh
python3 tools/assets/fetch_summit.py
```

`fetch_summit.py --archive PATH` accepts an existing, checksum-verified download.

## Authored survivor

`kenney-character/survivor.glb` derives from [Kenney Animated Characters Survivors](https://kenney.nl/assets/animated-characters-survivors),
CC0-1.0. The male survivor's mesh and skin texture are used, with the authored idle/run/jump actions. The preparation tool fits
vertices to the canonical 25-joint rig, collapses finger weights into hands, retargets world rotation changes,
recalculates flat normals and embeds the unmodified texture. This preserves hand/foot IK and ragdoll binding.
Walk is a slower playback of the authored run; Fall holds a mid-jump pose.
Kin adds a 0.4-second in-place Pickup reach with `tools/assets/add_summit_pickup.py`
(7.5 KiB of glTF data, no duplicated mesh/texture/skeleton). These are provisional animation choices, not a
complete authored climbing, walking or get-up animation set. The authored clips do not have the procedural landing-squash layer.

The fitted mesh has 1,604 triangles and five clips. Regenerate using an existing Blender installation (no editor download needed):

```sh
python3 tools/assets/fetch_summit_character.py --blender /path/to/blender --scratch /disk/backed/scratch
```

The script also accepts `--archive PATH`. Blender runs headless with one thread; the game uses only the resulting glTF and
needs neither Blender nor the FBX source files. The manifest records source, rig, preparation tool and derived asset checksums.
CPU asset tests check rig compatibility, inverse bind matrices, skin weights, finite sampled animation and baked collider bounds.
The survivor is authored in `prefabs/survivor.kinprefab`, loaded by the normal prefab utilities and bound through
`resolve_render3d_assets` using the same renderer-scoped asset library. The prefab owns its transform, asset reference,
tint and base animator settings. `animations/survivor.kinanimator` defines the locomotion blend and named foot/hand IK;
`animations/survivor.kinanim` uses the existing state-machine format. Gameplay supplies inputs, targets, physics queries
and ragdolls. The imported model has no Squash clip, so its authored layer list stays empty, preserving its previous look.
Cache lookups hold weak references and have bounded metadata; scene handles/batches/draws own the live GPU resources.
See [shared asset and placement API](../../../docs/rendering_3d.md#shared-assets-and-whole-model-placement).

## Diagnostics and packaging

Both authored camps instantiate `prefabs/campfire.kinprefab`: an ember material on the
existing fire pit, procedural flames/smoke/sparks and a point light. Its three ordinary
`ParticleEmitter3D` descendants in `campfire-effect.kinprefab` have capacities 96/64/48 (208 total), with independent
seeded simulation per prefab instance. No new textures or asset downloads are needed.
See [particle authoring](../../../docs/rendering_3d.md#authored-particle-emitters).
The playable scene uses a structural override: remove `effect/smoke` and add a smaller
`low-smoke` child (32 particles), retaining three emitters with total capacity 176.
The static camp inherits the original effect. See [composition and structure](../../../docs/ecs_prefabs.md#composition).

`camp_walk` uses `scenes/camp-play.kinscene` and `prefabs/camp-player.kinprefab`
to make the authored camp playable. `colliders/` contains small explicit recipes:
compound platform, shared convex barrel and hollow mesh tent. Character settings
and animation parameter bindings use the standard component fields. Cooking reads
only CPU geometry and discards it afterward; it never loads collision textures or
duplicates GPU meshes. See [physics authoring](../../../docs/physics_3d.md#authored-collision-and-characters)
and [controls](../../README.md#camp-walk).

`scenes/camp.kinscene` is a complete C++ scene-loading example for
[`SceneSession3D`](../../../docs/rendering_3d.md#owning-a-3d-scene-session): camera, environment,
free camp props, animated survivor prefab and primitive floor collider. It uses the same
content root and loaders.
It instantiates `prefabs/camp-barrel.kinprefab` twice with brass/blue variants from
`materials/camp.kinmaterials`, sharing the barrel model. The fire pit uses its ember
variant. See [material authoring](../../../docs/rendering_3d.md#authored-material-overrides-and-variants).

`projectile_range` uses `scenes/projectile-range.kinscene` and the `range-target` /
`range-projectile` prefabs. Its barrel enemy, crate target, rock projectiles and floor
reuse this same free art. No additional asset download is required.
