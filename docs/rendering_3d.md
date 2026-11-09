# 3D Rendering (Experimental)

> Experimental: built with `-DKIN_EXPERIMENTAL_3D=ON`, and the API may change
> in any release. Why it exists and where it is going: [the plan](3d_plan.md).

Kin draws 3D inside its 2D frame. A **view** is a picture (colour and depth)
that a 3D **scene** is rendered into; Renderer2D lays it into the frame like
any render target, with 2D drawing and ui2 under or over it. A full-screen 3D
game is a view the size of the screen with its HUD drawn on top; a model
turning in a panel is a small view.

3D needs the SDL_GPU backend (`renderer.capabilities().rendering_3d`). Under
SDL_Renderer (and in headless runs without `KIN_RENDER_BACKEND=gpu`) meshes
come back empty and a view shows only its background, so a game still runs
and its state can still be tested.

For a game, use `SceneSession3D` to own the world, scene lifecycle, frame stages and view. The
[camp walkthrough](#owning-a-3d-scene-session) shows loading, authored components, physics and error reporting;
`examples/camp_walk` and `examples/projectile_range` are the starting points.
For Lua gameplay, use the [3D Lua workflow](#lua-gameplay-in-a-3d-scene) and `examples/lua_range`.

```cpp
#include <kin/scene/scene_session3d.hpp>

kin::Renderer3D renderer3d{renderer2d};
kin::Assets3D assets{renderer3d, content_root};
kin::SceneSession3DOptions options;
options.viewport = {960, 540};
kin::SceneSession3D session{renderer3d, assets, options};
auto loaded = session.load("scenes/camp.kinscene");
if (!loaded) {
    // Display diagnostics3d_text(loaded.diagnostics).
}
// Each fixed step: check session.step(dt).
// Each rendered frame, composite after a successful render:
if (session.render())
    renderer2d.draw_texture(session.view()->texture(), {0, 0, 960, 540});
```

`SceneFrame3D` integrates the same stages with a game-owned ECS world. The direct `Scene3D::draw` and
`Renderer3D::render` APIs support embedded views, renderer tools and the benchmark examples below.
`Renderer3D::render_geometry` also records standalone opaque/cutout depth and world normals for custom pipelines;
see the [geometry graph contract and outline example](render_graph.md#geometry-operations-and-data-attachments).
For explicit native shadow → material shading → custom postprocessing, use
[`render_sun_shadow()` and `render_surface()`](render_graph.md#explicit-sun-shadows-and-mesh-surfaces).
The [surface graph example](../tools/surface3d/main.cpp) composes opaque HDR shading, ordered glass/sprites, bloom/output and GPU ASCII without a stock view. See [surface stages and soft-depth inputs](render_graph.md#opaque-and-transparent-surface-stages) for explicit composition. [Attachment versions](render_graph.md#attachment-loads-and-logical-versions) let compiled graphs load colour in place when safe and preserve it through a copy when older readers remain.
For several consumers of the same scene, [explicit shared preparation](render_graph.md#shared-draw-preparation)
reuses world data and compatible GPU uploads while keeping visibility and temporal history per view.
Configure with `cmake --preset 3d` to include the experimental engine; ordinary and shipping builds leave it out.
The [build and validation guide](rendering_validation.md) includes runnable viewer/graph commands, multiple-camera
visibility, required-GPU tests, CI coverage and evaluation limits.

Fallible native 3D operations return `Result3D<T>` or `Status3D`. Check the result
before reading `*result`; `result.diagnostics` owns the failure messages. Empty
query payloads are ordinary absence, independent of result success. The
[migration guide](3d_api_migration.md) lists the breaking signatures and fields.


## Conventions

- Right-handed, +Y up, -Z forward (glTF's); metres are a good unit. A camera
  looks down its own -Z.
- Angles in degrees, as in 2D; a positive angle turns anticlockwise looking
  down the axis toward the origin.
- `kin::Mat4f` is column-major and maps column vectors: `a * b` applies `b`
  first. `Mat4f::trs(position, rotation, scale)` places a thing.
- Depth is reverse Z: 1 at the near plane, 0 at the far one (or infinity).

The maths is in `kin/core/math3d.hpp`: `Vec3f`, `Vec4f`, `Quatf`
(`axis_angle`, `yaw_pitch`, `look_rotation`, `slerp`), `Mat4f` (`translation`,
`scaling`, `rotation`, `look_at`, `perspective`, `orthographic`, `inverse`),
and `project`, `unproject` and `screen_ray` between the world and 2D draw
coordinates.

## Colour types and authored quality

All native 3D RGB settings and colour-function results use `LinearRgb` (`r,g,b`):
lights, sky ground/moon/night colours, fog, time of day, particles and weather.
HDR values above one remain linear. Directions, positions and velocities use
`Vec3f`; surface/texture byte tints still use sRGB `Color`. No implicit conversion
connects these types. See [the API migration](3d_api_migration.md).

`SceneEnvironment3D` includes `ground_ambient` and `ground_ambient_enabled`, every
public LOD setting, and the full procedural sky controls. Additional sky fields
are `sky_mode` (`procedural`/`image`), `sky_sun_size`, `sky_sun_disc`,
`sky_night_color`, `sky_moon`, `sky_moon_direction`, `sky_moon_size`,
`sky_moon_light`, `sky_moon_color`, `sky_light_threshold` and `sky_light_interval`.
An image sky still needs a runtime environment-map handle, set on the active
`SceneEnvironment3D::lights.environment` during host initialization. `PointLight3D` and
`SpotLight3D` expose `shadow_resolution`, `shadow_bias`, `shadow_normal_offset`,
`shadow_near_plane`, `shadow_distance` and `shadow_softness` alongside `shadow_enabled`.
These values survive save/load, inspection, patches and collection. RGB file
fields stay linear triples; HDR colours are never encoded as byte tints.

## Automatic rendering paths

Renderer implementation A/B environment variables have been retired. The supported optimizations are automatic;
shader/device support, scene features, workload thresholds and bounded resource budgets select the necessary fallbacks.
GPU retention and cached bounds keep their existing limits. Feature targets expire after 120 unused renders, and upload
staging adapts to completed batch occupancy. There is no environment checklist to enable these paths.

Use authored quality settings for visual features, `scene.lights.cache_shadows` for shadow caching, and explicit
`scene.debug` controls for CPU cluster selection, LOD overrides, jitter freezing, GPU counter readback and pass markers.
The disabled point-light tile-mask and cutout main-depth experiments, their shaders and their counters have been removed.
Historical measurements in the plan/changelog describe the switches that existed when those measurements were taken.

## Meshes

`kin::MeshData3D` is triangles as data: vertices (position, normal, UV, colour)
and indices, three a triangle, counter-clockwise seen from the front.
`validate()` says whether it can be drawn; `append(other, transform)` builds
a shape from shapes. `Renderer3D::create_mesh` keeps it on the GPU as a
`kin::Mesh3D` (cheap to copy). Meshes may outlive the renderer.

Small static meshes share bounded vertex/index buffer pages. The renderer binds a page at zero and selects each mesh with
indexed-draw offsets, including its LOD ranges; consecutive meshes on the same pages avoid redundant buffer bindings in the
main, shadow, depth, velocity and picking passes. Skinned and clustered meshes keep dedicated vertices. Retirement fences
protect shared ranges before reuse, and empty pages are released. Local-shadow preparation packs accepted caster copies into flat lists instead of allocating
per-mesh/per-view vectors; the stable ordering preserves shadow-cache keys and clustered cuts.

Shapes made in code (`kin::meshes`): `box`, `plane`, `sphere`, `cylinder`,
`cone`, `torus`, `star_prism`, `octahedron`, `wedge` (a ramp).

### Baked cache

Levels of detail and cluster hierarchies are built by `create_mesh` at every launch (the kiln's hierarchy: 5 s and 1.5 GB of the
process). A `kin::BakeCache` (`kin/assets/bake_cache.hpp`) keeps each build in a folder and gives it back on the next run:

```cpp
#include <kin/assets/bake_cache.hpp>

kin::BakeCache bake{kin::user_data_dir("my-game") / "kin" / "bake"}; // the game's folder; 1 GiB at most by default
r3d.set_bake_cache(&bake);        // levels and clusters, for create_mesh, load_model and Assets3D
collisions.set_bake_cache(&bake); // cooked collision shapes (physics_3d.md)
```

With a cache set, `create_mesh` with `clusters` or `levels > 1` looks the build up under its key and restores it, else builds it and
writes it. The key is a hash of the mesh's vertex and index bytes, the `MeshOptions3D` fields the build reads (for clusters
`cluster_triangles` and the three weights; for levels every level option), the format's version and meshoptimizer's
(`mesh_clusters_bake_key`, `mesh_levels_bake_key`). **The invalidation rule is the key**: a changed mesh or option is another key, so an
entry is never stale and there is nothing to clear after editing a model; entries no longer asked for age out. A restored build is the
build to the byte (every field of `MeshClusterBuild3D`, or the `MeshLodChain3D`), so the mesh and the pictures are those without the cache:
the reference scenes `hidden` and `hidden_dense`, and the helmet and Sponza with `--lod-levels 4` and with `--lod-clusters`, give the same
PNGs off, cold and warm (`kin_reference --bake-cache DIR`). Meshes with nothing worth keeping (one cluster's worth of triangles, a chain
that stops at level 0 at once) are built as before. `hidden` loads in 30 ms warm against 109 ms without (its 100,352-triangle torus);
Sponza with clusters in 1.8 s against 2.1 s (the rest is its textures). The key reads the mesh at 2.2 GB/s (the kiln's: a quarter of a second).

The folder holds a `<key>.bake` file a build: the key, the length and a checksum before the bytes. A truncated, corrupt or misplaced
file is a miss (and removed), never a crash; the restore checks every offset and count besides. Writes go to a temporary file renamed
into place, so a crash or a second process never leaves half a file under a key. Past `max_bytes` (the constructor's second argument)
the entries used longest ago go first. `bake.stats()` counts hits, misses, refused files, writes, evictions and bytes. Opt-in: the engine
never picks a folder (nor creates `<user data>/kin/bake` by itself); the folder the game passes is made by the first write, and a
default-constructed cache (or none) reads and writes nothing. The bytes are for this machine's build: keep them out of shipped content.

## Scenes

The normal `WorldRenderState3D::collect(scene)` / `SceneFrame3D::collect(scene)` loop automatically retains substantial
opaque/masked mesh and static-model buckets. Keep using ordinary ECS components and clear/collect/render each frame;
no hand-built `InstanceBatch3D`, static marker or renderer flags are required. Buckets of 256 or more copies use fixed
records, with conservative bounds for consecutive 256-copy ranges from 512 copies upward. Source order is preserved;
this is not a spatial sort. Unchanged records keep their revision; changed runs update only those records and bounds.
A retained bucket stays eligible down to 128 copies, avoiding oscillation at 256. Two reusable batches preserve
independent collected scenes and alternate counts without recreating their records on every collection. Other count
changes create a new batch. Small buckets, blended/custom/wireframe/skinned materials and budget overflow use ordinary
streamed submissions.

The collector keeps at most 128 automatic batches and **16 MiB of instance-record payload** per render state, in addition
to its existing collection scratch. Group/edit metadata and allocator overhead are extra. Separate scenes keep snapshot
semantics: an edit reuses an unshared spare batch, or creates one if older collected scenes still own both record sets. Such scene-owned snapshots
are outside the collector's live-record budget. Clear old scenes to release them; `clear_collection_cache()` drops the
collector's handles. Collection still visits the current ECS records each frame, so writes through ordinary component
pointers remain visible. GPU tables retain their separate 32 MiB cap and feature/capability fallbacks below; enabling
TAA or unsupported material features does not silently lower quality to retain records.

A `kin::Scene3D` is filled each frame and holds no GPU state: a camera, the
lights, and draws.

- `draw(mesh, transform, material, tint, id)`: one copy. Copies of the same
  mesh and material one after another are drawn as one instanced draw. `id`
  is what picking says is under a pixel showing it (see Views).
- `draw_instanced(mesh, instances, material)`: many, each a transform, a
  tint and an id.
- `add_instances(mesh, count, material)`: room to fill in place (valid until
  the next draw is added).

The GPU renderer reuses the mesh draw/piece/task layout for persistent commands and structurally identical resubmissions.
`draw_identity()` and `draw_revision()` identify that command set: mesh submissions and `clear()` change its revision;
moving a scene gives the destination a fresh identity and invalidates the source. After clear/resubmission into the same
scene/storage, exact weak-owner/mesh/batch/range/group checks can reuse the layout despite a changed revision. Changed
mesh identity, ranges, skin layout, group layout or scene identity rebuild it. Material and instance content are read
afresh; no content hash is used to establish correctness. Camera, light and particle changes do
not invalidate the layout. Batch layout revisions are checked separately, including edits through another shared handle.
Fixed-size batch content edits keep draw/piece/task metadata valid. Group bounds and all visibility proofs still refresh;
a group gaining/losing valid bounds changes the batch layout revision and rebuilds the layout, even if the total number
of valid groups stays the same. GPU tables and CPU spheres continue to use the full content revision independently.

This reuses the renderer's existing arrays, not a copy of scene instances or visibility results. Camera frustums, shadow
membership, group plane proofs, retained-table slots/bounds and uploads are refreshed each render. Writes through a still
valid `add_instances()` span remain visible. A single layout is kept per GPU backend (shared by its `Renderer3D` facades);
alternating unrelated scenes replaces it. It is bounded to 8,192 draw references,
16,384 pieces, 4,096 submitted spatial groups, 128 batch stamps and 16 MiB of existing vector payload capacity.
Exact structural descriptors add at most 1 MiB of bounded vector payload (plus weak-owner control-block overhead).
They keep no owning mesh/batch handles and no second instance array. LOD-enabled
scenes use the original construction path; oversized layouts and structural command edits rebuild it too. Skinned commands
can reuse the layout: joint pointers/offsets, posed bounds, visibility, shadows and GPU uploads refresh each render.

Stats `prepared_reused` (0/1) and
`prepared_draws` report reuse and submitted valid draw references. `prepared_cache_bytes` counts keys, stamps and exact structural descriptors;
`prepared_layout_bytes` counts the existing layout vector capacities, not additional allocations. Structurally unchanged
resubmissions can hit the cache. No shader, render target or per-instance cache is added.
Batch `layout_revision()` adds eight bytes per shared batch; no new renderer stamp array is needed.

`draw_instanced_group(mesh, instances, world_bounds, material)` preserves a spatial cell within an instanced draw. It accepts
either transforms or `MeshInstance3D` records. Equal mesh/material groups still merge into one draw, retaining global alpha
sorting and instance order. Camera, sun and local/moon shadow views classify each cell independently; rejected cells skip
per-copy tests, fully contained cells skip their plane tests, and intersecting cells refine individual spheres as usual.
Camera rejection alone never removes an offscreen shadow caster. Ungrouped ranges interspersed with cells keep per-copy culling.

Calculate `world_bounds` with `instance_bounds(mesh, instances)` when the mesh/transforms change, then cache it. It encloses
the renderer's transformed culling spheres (including shear from a scaled parent and rotated child), rather than only the geometry's
tighter box. Bounds use the coordinate space of their own scene, including an overlay's. Supplying stale or undersized valid
bounds can incorrectly cull copies; invalid/nonfinite boxes fall back to per-copy tests. These calls submit plain instances;
`draw_skinned` continues to use its posed bounds. `clear()` also clears group metadata while retaining its capacity.

`RenderDebugStats3D::spatial_groups`
counts active cells; `camera_instance_tests`, `bounds_instance_tests` and `local_instance_tests` count refined copies, not
planes. `camera_group_rejected` counts copies rejected for the camera; `local_group_rejected` counts rejected candidates
across wanted shadow views. Group metadata introduces no additional GPU draws or instance buffers. Blockfield's grove caches
64 foliage-cell bounds and eight mist-cell bounds when loading its shared recipe.

`InstanceBatch3D::create(mesh, std::vector<MeshInstance3D>, std::vector<InstanceGroup3D>)` owns a fixed-size collection and optional
ordered, disjoint cell ranges. Move the vectors in to avoid another CPU copy. Cell bounds are computed from the records.
`scene.draw_instanced(batch, material)` retains a cheap shared handle; these records are in `Draw::batch.instances()`,
rather than `Scene3D::instances()`. A batch stays a separate draw even beside equal materials. `clear()` releases the scene's
handle; the caller can reuse its own handle next frame. Meshes, materials, transforms, tints and picking IDs behave as usual.

`batch.update(first, records)` edits a fixed range, increments the shared revision, and refreshes affected cell bounds at the cost
of the edit's size, not the cell's: a copy strictly inside its cell's bounds that moves only widens them, and a cell an edit may
have shrunk (a copy on one of its faces moved in from it) is measured again once, when `groups()` is next read (2,000 one-copy edits
of a 100,000-copy batch: 0.2 ms, not 350 ms; `kin_render3d_bench batch`). Out-of-range edits return a failed `Status3D` with diagnostics; changing the count requires a new batch. Copies share edits, including handles already submitted
into a scene. Edit between renders, never concurrently with rendering. The latest records are read when rendering starts.
`changed_since(revision)` returns the enclosing changed range for an independent reader, or an empty range at the current
revision. Each batch keeps the last 16 edit ranges (exact) and, from its first edit, the revision of the last edit of every
16-copy block (half a byte a copy): a reader further behind than 16 edits gets the span of the blocks edited since, not the whole
batch; zero or future revisions conservatively return the whole batch. `changed_ranges_since(revision, out)` gives the same
edits as ascending, disjoint ranges (touching ones merged) for a reader that copies what changed and can skip the gaps between
far edits. Queries do not consume history, and empty edits do not advance the revision.
`layout_revision()` is separate from the content revision: fixed storage/group ranges and mesh identity do not change on
updates. Only a group's validity changing affects the renderer's piece ranges. Empty handles report zero for both revisions.

The camera classifier reuses current cached spheres for single-level, non-skinned/non-clustered draws that need no sun
membership bits. It fills camera-inside/outside ranges and tests partial ranges four copies at a time, using the existing
local-shadow SIMD helper (or its scalar fallback), in source order with the original boundary comparisons. Local shadow
selection remains independent of camera rejection. No sphere cache or GPU resource is added.
Classification and prefix reduction also clear only the LOD/cascade counter prefixes consumed by the current render;
enabling more levels/cascades clears those newly active slots before use.

For a compiler-only experiment, configure `-DKIN_RENDERER3D_O3=ON`. In GCC/Clang Release builds this appends `-O3`
only to `gpu_renderer3d.cpp`, after the engine target's `-O2`; other engine sources, floating-point flags and GPU
shaders stay unchanged. The default is OFF: the Grove O2/O3 comparison showed small, mixed performance changes and
larger renderer code. Set it OFF and rebuild to restore O2; this is a CMake option, not a runtime environment variable.

The GPU renderer retains up to **32 MiB of live instance tables / 128 batches**. Built-in opaque/masked, non-skinned copies
with one selected mesh level use stable 128-byte records and per-view 32-bit index lists for main/prepass/pick and shadows.
An unchanged batch uploads no records. An explicit edit sends only the copies edited since (`changed_ranges_since`), written
in place; the entire table, cycled, goes instead for a new table, a colour or layout change, over half the copies changed, or a
table an earlier view of the same GPU frame draws from (that frame's uploads land before all its draws). At most one upload
per table per GPU frame: changed data in a later view of that frame uses the immediate path. Each material colour a batch is drawn in has a table of
its own, so two views (or draws) of one batch in different colours upload nothing once made; a colour without a table takes
over the batch's table unused for a whole frame (an animated colour uploads once a frame), else a new one. TAA, blended
materials, custom shaders, selected multi-level/cluster LOD, occlusion culling,
unavailable shaders and capacity overflow keep the ordinary upload path. CPU batch handles work in these cases too.
The budget covers live tables, not total GPU memory: streamed buffers, staging and in-flight buffer versions are additional.
Expired batches' tables are released between GPU frames without waiting for the GPU (SDL frees a buffer once the work
submitted with it is done). When the 128 tables or 32 MiB are full, tables not drawn for a whole frame are evicted, least
recently drawn first; the next frame, the largest batch refused may also evict tables drawn last frame that together are
smaller than it, its room kept for it, so retention settles on the larger batches without equal ones trading places. Stats:
`retained_evictions`, `retained_refused` (eligible draws streamed for lack of room) and `device_waits` (device-wide waits
inside `render()`, 0 normally).
Tables with at least 8,192 records pack directly into upload staging on the attached renderer workers, in 2,048-record
slices when renderer workers are attached. This changes neither GPU upload
size nor the cycling/multiple-view safety rules, and adds no CPU instance mirror or GPU buffer.
Consecutive retained batches can share a graphics pipeline while using different instance storage. The draw binding cache
compares storage-buffer handles/counts independently of pipeline identity, including depth, shadows and picking.

Stats expose live `retained_gpu_bytes`, submitted
`retained_instances`, per-render `retained_upload_bytes`, and main-view `retained_index_bytes`. `instance_upload_bytes` counts
streamed main/local-shadow 128-byte records, excluding retained records and all index lists. The same fields appear in the
Blockfield CSV/report. Grove uses retained masked foliage. `--rebuild-scene` measures per-frame command submission.
Mist keeps its existing alpha-sorted path.

Ordinary direct scene draws can share a retained table too. A scene with at least
32 eligible draw records and 256 eligible copies is admitted after its commands
survive a render without resubmission. Built-in opaque/masked, unskinned draws
share storage while keeping their original material, draw order and picking IDs.
Packed records and world bounds are reused across camera changes and pose-only
edits. A command revision refreshes the scene table and its eligible bounds.
Camera/sun visibility still runs for each consumer; fixed local lights can reuse
static caster selection as described below. Colour draws keep their source order.

Scene tables share the existing 32 MiB/128-table GPU budget and 4 MiB bounds
budget with explicit batches. Their rows follow `Scene3D::instances()`, including
unused rows for ineligible draws. Tables retain weak scene identities and release
expired/moved sources between GPU frames. Unsupported features, memory pressure,
and later edits after an upload in the same GPU frame use the streamed path.
Shared prepared consumers, TAA, enabled LOD/occlusion, custom shaders, wireframes,
skinned and blended draws retain their existing paths. A public `add_instances()`
span disables scene-table retention until `clear()`: unannounced writes through
that span remain visible even after `modified_instances()`.

`Scene3D::debug.retain_scene_instances = false` selects the streamed reference
for this optimization. The Yard example exposes `--retain-scene-instances 0|1`.
`direct_retained_instances`, `direct_cache_hits`, `direct_upload_bytes` and
`direct_bounds_rebuilt` expose useful work/reuse; the instance/upload/bounds
counts are subsets of the existing retained counters. For alternating full-route
controls and exact captures, use `tools/benchmark_render3d_direct_cache.py`.

The native stock local-shadow path also caches static direct-source caster
selection for unchanged spot and point projections. After the same source and
projection have been observed twice, a bounded CPU plan stores relative copy
indices in source order. Moving instance batches, skinned casters and other
ineligible draws are gathered afresh and interleaved with those rows each render.
The plan keys the weak scene identity, command revision, ref count, exact light
projection/frustum and reach. Camera-dependent point-face selection remains live;
sun/moon/GI maps, enabled LOD and explicit prepared consumers keep their existing
paths. A changed or expired source/projection invalidates its plan, including
scene moves and edits between views of the same GPU frame.

Depth batching retains its 128-entry locality windows. Exact current comparator
keys validate a saved sort permutation per window; a changed key sequence is
sorted again with the original stable tie order. Current records, textures and
samplers are still bound each render. This cache reuses CPU preparation, including
when `lights.cache_shadows` is false; it does not retain GPU shadow images.

CPU vector payload is limited to 4 MiB per renderer across 64 source/light-slot
probes, plus fixed metadata. Unstable projections are observed without allocating
payload, and pressure falls back to fresh gathering/sorting.
`debug.cache_local_shadow_gather = false` selects that reference path;
`debug.local_shadow_gather_budget` can reduce the cap for diagnostics.
`local_gather_cache_hits/misses/builds`, `local_gather_cached_tests/copies`,
`local_gather_cache_bytes` and `local_sort_cache_hits/misses/cached_entries`
report reuse independently of whole-map hits. Existing local tests/candidates
count fresh work only. The Yard CLI exposes `--cache-local-shadow-gather 0|1`;
benchmark `--mode local-gather` keeps direct retention enabled in both controls.

The native main pass retains draw decisions and source ordering after an unchanged
command revision has been observed twice. A plan holds up to 8,192 draw refs and
4 MiB of CPU vector payload, keyed by both scenes' identities, revisions and
storage. It caches stock material uniforms, texture/sampler compatibility classes
and pipeline pointers. Target format, samples, TAA, velocity, fog, local-light and
indexed-storage changes select current pipeline variants; pipeline-cache
forget/destroy events invalidate saved pointers. Failed pipeline allocations
remain retryable. Custom shaders keep their live selection/binding path.

The source's complete opaque order is sorted once, then filtered by current
visibility. Transparent mesh copies and particles still sort by current depth.
The world/overlay opaque and transparent blocks are assembled in the original
stable order, without repeatedly sorting the opaque items. Sources resubmitted
every render retain the immediate path rather than repeatedly build this plan.

For eligible static direct draws, a stock shader can read its material constants
from a retained GPU table: 64 bytes per source row, at most 4 MiB across eight
source tables, independently of the CPU cap and existing instance-table budget.
The vertex shader passes the original retained row to the fragment shader.
Consecutive commands can then merge when scene, mesh, pipeline, instance table
and texture/sampler bindings match, even with different scalar PBR values,
emission or mask cutoffs. Instance colour/tint and picking IDs keep their existing
records. Neither source draws nor copies are reordered. The ordinary picking,
depth and shadow passes keep their original refs and lists.

The main pass uploads a compact camera-index list for these runs; its GPU buffer
is separate from the material cap (at most 256 KiB with the current material-row
limit). Material tables upload only when source commands change. Moving batches,
skinning, unlit specialization, custom shaders, wireframes, TAA/LOD, exposed
instance spans and budget/feature fallbacks use their existing main shaders and
draws. Camera, light, pose and texture-content edits remain live. Legacy texture
scale/mip changes and global sampling settings are resolved when binding each
current texture class. No new public shader ABI is required. `warm_up()` prepares
the applicable indexed and material-table variants as well as ordinary draws.

`debug.cache_draw_commands = false` selects ordinary main draw preparation and
submission; `debug.draw_command_budget` reduces the CPU and GPU-material caps,
each clamped to 4 MiB. The immutable batch-kind flag reused by culling is common
to both controls. `draw_plan_hits/builds/bytes`, `draw_pipeline_hits/misses`,
`main_batched_draws`, `main_merged_draws`, `draw_material_gpu_bytes/upload_bytes`
and `draw_index_gpu_bytes/upload_bytes` expose the work and logical payload.
Pipeline hits count decisions reused before merging as well as recorded commands.
These GPU bytes exclude staging and additional driver backings in flight.
The Yard CLI exposes `--cache-draw-commands 0|1`; benchmark `--mode draw-commands`
keeps instance and local-gather retention on in both controls and verifies that
merged commands account exactly for the reduced draw count.

Resident tables also cache transformed culling spheres on the CPU, using 16 bytes/copy and at most **4 MiB per renderer**
(plus small container overhead). Spheres use the same calculations as the immediate path. A batch revision refreshes only
the union of edited ranges since that table's own bounds revision; new tables and expired history refresh the full array.
Material-colour changes alone
do not refresh spheres. Main and sun/local/moon culling still perform
their normal visibility tests, reading these spheres rather than retransforming each copy. CPU cache refresh is independent
of GPU record uploads, including edits between views in one GPU frame.

Only batches already admitted to the GPU cache get CPU sphere storage. Non-skinned copies with one selected mesh level can
reuse it during GPU streaming fallbacks too (for example, a later view with TAA, alpha or a conflicting material colour).
Selected multi-level LOD keeps its original scale calculation to preserve threshold rounding; posed bounds remain dynamic.
Uncached local casters use a compact transient sphere array addressed separately from visibility flags. When a batch becomes
cached, oversized old transient storage is released, avoiding two full sphere arrays. Expired tables release both caches.

Stats/CSV/report
expose live allocated `bounds_cache_bytes`, allocated transient `bounds_scratch_bytes`, submitted `bounds_cache_instances`
(counting repeated draws separately), and actual `bounds_cache_rebuilt` copies this render. `bounds_instance_tests` counts
actual sphere calculations, including cache rebuilds; it therefore includes a cold or edited batch once and skips unchanged
cached copies on following renders. These memory counters cover sphere payload capacity, not total process/GPU memory.

Local shadow gathering also retains the unresolved frustum-plane mask for each spatial cell/view. A range proves full
acceptance only when its whole sphere-enclosing box is inside the view and, for bounded lights, strictly inside the light
radius. Single-level non-clustered ranges then fill consecutive accepted indices directly, without reading spheres or
allocating classification bytes. Partial ranges test only the unresolved planes, copied once into a small worker-stack
frustum in their original order. Proven light-radius containment skips per-copy distance tests independently of plane masks.
Moon views retain their camera/slice shadow-reach tests when camera culling is enabled; camera rejection never removes
local offscreen casters. Selected LOD/cluster ranges keep per-copy classification. No per-instance persistent cache is added.

`local_candidates` counts copy/view pairs after cell rejection;
`local_instance_tests` counts those receiving per-copy gather work; `local_group_accepted` counts the rest. `local_radius_tests`
counts actual per-copy radius checks. `local_plane_budget` is the **potential** plane-test count before radius rejection and
per-copy early exits, not executed dot products. `local_bucket_bytes` counts temporary byte-classification payload. These
fields appear in stats/Blockfield CSV/report. Extra metadata is one byte per possible local view per cell (24 bytes/cell
with current limits); scratch may shrink, while accepted-copy lists and existing instance/bounds caches retain their budgets.

Local shadow preparation uses one counter per task for single-level and clustered meshes. Only tasks that select among
multiple mesh levels allocate per-level counters, sized once to their exact combined requirement. At eight levels the
32-byte task plus its 32-byte counters equals the original 64-byte record, avoiding a memory regression in LOD-heavy scenes. Accepted indices reuse the local upload list's backing storage;
cache decisions and cluster cuts finish before that list is compacted/remapped in source order. Retained draws keep raw
batch indices, while streamed draws are remapped into the caster union. Slot lookup storage covers only streamed copies;
oversized prior slot allocations are released when they exceed twice the required size (with a 4,096-slot floor).

Stats/CSV/report expose `local_task_bytes` (task and
rare multi-level counter vector capacities), `local_slot_bytes` (allocated slot-vector capacity), and `local_copy_bytes`
(combined simultaneous accepted/upload-list capacities). These measure specific CPU payloads, excluding allocator overhead,
other scratch, GPU buffers and process RSS. The reused copy list retains its high-water capacity, as the upload list did.

Directional sun/moon reach uses a prepared ray through `shadow_area` and computes each sphere's swept endpoint only
when a camera/slice plane rejects its starting position. Sun cascades and CPU cluster selection reuse that endpoint.
Spatial cells carry pending camera/slice plane masks: planes proven to contain every enclosed sphere can be skipped;
a cell outside the camera is never rejected from that proof alone, since its shadows may enter view. Original plane
coefficients, order and float divisions are retained. Cluster spheres use full plane sets rather than instance-cell proofs.

Spatial groups and multiple sun cascades use the
prepared form; ordinary single-map draws and scenes without sun shadows use the original form, avoiding helper overhead
for small draws. Masks add eight bytes per allocated spatial cell; no per-instance cache is added and compact local
tasks remain 32 bytes. Existing culling/payload counters retain their meaning: `local_plane_budget` covers light
frustums, not directional camera/slice reach. Camera-culling-off behavior and GPU cluster selection are unchanged.

Camera-inside spatial cells can classify single-level, non-skinned, non-clustered copies as a range, filling the existing
visibility bytes and recording one count. The shortcut requires that no per-copy sun-cascade membership or uncached
local-shadow sphere is needed. Partial cells, offscreen casters and those consumers retain refinement. Fully visible
single-level pieces also fill retained indices as consecutive source indices, or pack streamed records without reading
visibility per copy. Source order, draw counts and alpha sorting are preserved, with no new per-instance storage.
`RenderDebugStats3D::camera_range_accepted` counts copies classified by the cell shortcut; `bounds_instance_tests` continues
to count actual transformed bounds, so it may decrease for streamed cells. The counter does not include packing alone.

Large local-shadow candidate ranges also test four spheres at once directly from the shared 16-byte cached/scratch
bounds records. Prepared plane/radius constants live on the worker stack; no second bounds layout or per-instance cache
is allocated. Ranges below 16 candidates retain the scalar loop, fully accepted cells retain their existing range path,
and final one-to-three candidates use the scalar tail. Surviving lanes stay in source order through moon reach tests,
LOD/cluster buckets and cache decisions. Single-bucket ranges classify four accepted copies together.

The native path uses SSE2 with scalar SSE math on GCC/Clang builds without FMA/fast-math flags; MSVC and other targets
retain scalar gathering. Ordered rejection comparisons, arithmetic order, touching and unordered inputs match the scalar
predicates on the tested build. Compact tasks remain 32 bytes and existing diagnostics retain their meaning;
`local_plane_budget` still describes potential scalar plane tests rather than executed SIMD operations. The native test
object is 464 bytes on the tested x86-64 build, temporary per active worker task, without per-renderer override state.

The GPU device's shared texture/storage upload staging starts at 4 MiB, growing immediately for larger
writes. This limits startup cycle backings before the first observation window. Every 120 completed
upload batches, it can shrink to the next power of two of that window's maximum aggregate batch occupancy, with a 1 MiB
minimum and a factor-of-two size threshold. It grows immediately when an individual upload needs more space, and when a
batch of smaller writes overflowed it (sent as several submissions) it grows to the next power of two of that batch's total,
up to 64 MiB, so the next such batch is one submission. Bursts can
therefore take up to two windows of subsequent quiet batches to reach the minimum. Idle frames without upload submissions
do not advance the window. SDL retires old transfer buffers after their submitted copies finish; this adds no GPU wait.

The policy is device-wide, covering 2D
textures and 3D storage uploads; the separate vertex/index upload ring is unchanged. `RendererBackendStats` exposes
`upload_staging_bytes` (current single-handle capacity, zero between retirement and reallocation) and cumulative
`upload_staging_allocations`. These exclude SDL's cycle backings, retired resources and driver pools, so are not total RAM.
Blockfield's CSV also records `upload_count` and `upload_batches` from the shared upload path. Large active workloads,
including the tested dense TAA case, retain 16 MiB; smaller staging is not a universal process-memory guarantee.

For external GPU profilers, `scene.debug.gpu_pass_markers = true` adds SDL debug groups around clusters,
sun/local shadows, prepass/occluders, overlay velocity, main rendering, temporal resolve, picking and view output. These
are labels in the existing command buffer: they do not create timestamp pools, readbacks, fence scopes or submission
splits. Disabled by default, with one flag per renderer and stack-only guards. SDL may ignore labels on unsupported
backends. Vulkan/NVIDIA Nsight Systems can collect hardware timestamps for these ranges externally.

Treat marker durations as pipeline spans, not isolated shader costs. A dependent output/copy can wait for fragments
launched in the main pass. In the 100k Grove capture, disabling HDR moved ~0.6 ms from the output marker into the final
copy without reducing the enclosing GPU submission. Compare enclosing submissions and controlled variants before blaming
an output shader. Kin's ordinary fence timer remains a different measurement from these external hardware timestamps.

`kin::Material3D`: `color` (times the mesh's vertex colours and each
instance's tint), `texture` (the base colour's, by the mesh's UVs),
`emissive` (emission strength), `unlit`, `double_sided`
(back faces are culled otherwise), `blend`, `casts_shadow` and
`receives_shadow`. A transform that mirrors turns a mesh inside out: draw it
double-sided.

Surfaces are glTF's metallic-roughness model, lit in linear light with a
Lambert diffuse and a Cook-Torrance specular (GGX distribution, Smith
visibility, Schlick Fresnel) for the sun, the fill light and point lights:

- `metallic` (0 a dielectric, 1 a metal; default 0) and `roughness` (0 a
  mirror, 1 chalk; default 1: rough plastic, as before). The colour is the
  dielectric's diffuse colour, or the metal's reflection. Roughness is
  floored at 0.045 (a mirror would be a pixel-wide highlight that shimmers).
- `metallic_roughness`: a texture with roughness in G and metallic in B,
  times the two factors. `normal` (with `normal_scale`): a tangent-space
  normal map (green up, as glTF stores it); the tangent frame comes from the
  UVs on the GPU, so meshes need no tangents. A map of two channels holds x
  and y only, and z is rebuilt: BC5 (`TextureFormat::Bc5RgUnorm`), the format
  normal maps are best kept in (each channel compressed on its own, where BC1
  bends fine relief by tens of degrees), or any whose blue reads 0. No normal
  of a map points into the surface, so a texel whose z would is read so too;
  the others are read as they are. `occlusion` (R, with
  `occlusion_strength`) dims the ambient light only. `emissive_texture`: a
  colour decoded to linear light. `material.set_emission({r,g,b}, strength)` selects independent emission:
  linear `emission_color` × `emissive` × the map (white when absent), unaffected by base colour, vertex colour or
  instance tint. HDR factors/strength retain their energy. The default `emission_mode = BaseRelative` preserves
  native legacy glow: the map alone, or the base colour when absent, times `emissive`. `Independent` is what glTF
  imports use; glTF's RGB factor and `KHR_materials_emissive_strength` remain separate. Imported `KHR_materials_unlit`
  uses base colour only, as that extension specifies; native unlit materials can still add emission. `emits_light()` excludes
  independent zero RGB and zero strength from GI emitter membership. Colour/mode/map edits invalidate GI attributes;
  nonzero strength-only edits continue to reuse them.
- Maps of data (`metallic_roughness`, `normal`, `occlusion`) are read as the
  numbers they hold; colours (`texture`, `emissive_texture`) are sRGB, decoded
  as they are sampled. A texture you make yourself for a data map: 
  `renderer.create_texture(size, TextureFormat::Rgba8, pixels)` (linear);
  `create_texture_from_rgba` makes colour.
- Ambient light is the hemisphere's (`ambient` above to `ground_ambient`
  below), seen in two ways: along the normal for the diffuse, and along the
  reflection (turned toward the normal as the surface roughens) times the
  surface's specular albedo, Karis' fit of the split-sum integral, for the
  specular: a metal reflects the sky. What the specular takes the diffuse
  does not. An environment map will replace the hemisphere (G1b of the plan).
- `blend = Blend3D::Mask` with `alpha_cutoff` (0.5): opaque where the base
  colour's alpha (times the mesh's, the material's and the copy's) reaches
  the cutoff, not drawn below it: leaves, fences, grates. Drawn with the
  opaque (it writes depth), picked and shadowed as cut out (the shadow pass
  samples the base texture for these). A cut-out is opaque where it stays.

- `wrap_u`, `wrap_v` (`Wrap3D::Clamp` by default, `Repeat`, `Mirror`): what
  maps without an explicit sampler do past 0..1. `texture_samplers` has optional entries in `MaterialMap3D` order:
  base colour, metallic-roughness, normal, occlusion, emission. A `TextureSampler3D` declares independent wrap U/V,
  minification and magnification (`Nearest` or `Linear`), and mip filtering (`None`, `Nearest`, `Linear`). Images share
  independently of these descriptors. glTF preserves all six min filters, both mag filters and all wrap combinations;
  omitted filters default to linear/trilinear and omitted wraps to repeat. Nearest magnification does not discard
  separately requested mips. Reset an imported entry to `std::nullopt` to use the legacy material/texture settings.
  Missing mip storage resolves to no-mip sampling on both native and browser backends. Quality anisotropy and bias
  affect only fully linear mipmapped samplers, preserving explicit nearest and no-mip intent.
- `Scene3D::anisotropy` (1, off, by default; up to 16): anisotropic filtering
  of the materials' mipmapped textures. Where a surface is seen at a slant (a
  street, a floor, a wall along the view) a pixel covers many texels along the
  slant and few across it; trilinear reads it from the mip that fits the long
  side, which blurs the texture to its mean colour a few metres away, while
  anisotropic filtering reads up to `anisotropy` samples along the slant from
  the mip that fits the short side, so it stays sharp into the distance.
  Facing the camera it changes next to nothing (the hardware may choose the
  mip its own way with the filtering on: under 1 of 255 on average in kin's
  test, on NVIDIA, Intel and lavapipe). Nearest and unmipmapped textures are
  read as before, and every pass reads a material's textures alike (the
  cut-outs in the shadow maps, the pre-pass and picking; particles), so a
  cut-out's edge agrees in all of them; the overlay is drawn with the scene's.
  It is clamped to `RendererCapabilities3D::max_anisotropy`: 16 on SDL_GPU
  (SDL makes its devices with the feature), and on WebGL2 what
  `EXT_texture_filter_anisotropic` allows (1 without it). `SceneEnvironment3D`
  authors it as `anisotropy`.
- `Scene3D::specular_aa` (0 by default, off; 0 to 1; Godot's is 0.25):
  specular anti-aliasing (Tokuyoshi and Kaplanyan's, Godot's "roughness
  limiter"). Where a surface's normal turns within a pixel (a normal map's
  bevels, a curved edge, small things far away), a glossy highlight can be
  thinner than the pixel and very bright: one frame's samples hit it and the
  next miss it, so it glints and crawls as the view moves, and temporal
  anti-aliasing, which keeps little history in motion, shows each frame's
  glints as bright lines along the bevels. The lighting raises the roughness by
  how much the normal turns across the pixel (alpha squared plus twice this
  times the normal's change, at most 0.18 more), spreading the highlight over
  the pixel. In kin's test a glossy sphere 15 pixels across, moved by quarter
  pixels, swings 13 times its mean light without it and 1.9 times with 0.25; a
  near satin one changes in a few rim pixels only. In the Bistro, walking at a
  panelled door: bright-line pixels down from 170 to 61 a frame. Both backends;
  the scene's for the overlay too. `SceneEnvironment3D` authors it as
  `specular_aa`.
- `Scene3D::specular_multiscatter` (on by default): the light a rough
  specular surface reflects more than once among its microfacets, which single
  scattering leaves out, put back (Fdez-Aguera's fit, in the lights' highlights
  and the ambient reflectance alike): a rough white metal returns all of the
  light, as a furnace test wants. Off, a rough metal is darker (a white one at
  roughness 0.8 returns some 45% less of the sun's light; a rough dielectric
  nearly the same), as engines that leave it out draw it (Unreal's legacy
  shading, Substrate off). Both backends; `kin_material_lineup
  --single-scatter` compares with such an engine.
- `Scene3D::texture_lod_bias` (0 by default; -2 to 2): the mip level the
  materials' mipmapped textures are read from, moved by that many levels;
  negative is finer and sharper. Temporal anti-aliasing averages the shimmer
  of a finer level, and its resolve softens textures a little: a bias of -0.5
  (Godot's with its TAA) to -1 (FSR 2's at native resolution) gives that back,
  as `TemporalAA3D::sharpness` does within surfaces; the two together sharpen
  textures most. Read as `anisotropy` is, in every pass. Where
  `RendererCapabilities3D::texture_lod_bias`: SDL_GPU on Vulkan and Direct3D
  12 (Metal's samplers have no bias; WebGL2 none either). `SceneEnvironment3D`
  authors it as `texture_lod_bias`. Unlit materials use dedicated colour-only
  or base-textured main-pass shaders, with separate cut-out variants. They
  preserve vertex/material/tint colours, sRGB conversion, emission,
  alpha/additive blending, HDR and MSAA, while omitting lighting resources and
  unused normal/world-position outputs. Fog, skinned meshes, temporal AA and
  custom materials use the general shaders. Shadow, picking and depth/normal
  passes keep their existing shaders. `warmup()` prepares the selected unlit
  pipeline too. `RenderDebugStats3D::unlit_draws` counts specialized main-pass
  commands; `unlit_textured_draws` is its textured subset. Specialization is
  selected automatically when the material and view support it. `wireframe`
  draws only the mesh's triangles' edges, a pixel wide, unlit in the
  material's colour, just in front of faces in the same place (a little depth
  bias): a model's edges drawn over it, or lines on the ground made of
  degenerate triangles (`a, b, b`). Wireframes cast no shadow and are not
  picked. They need line fill, which desktop GPUs have; where the pipeline
  can't be made they are not drawn. `blend`: `Opaque` (the default), `Alpha`
  (over what is behind by its alpha; drawn after the opaque, furthest first)
  or `Additive` (adds light: sparks, tracers, flashes, in any order).
  Transparent draws are depth-tested but don't hide what is behind them.
  Copies wholly outside the camera's view are skipped (`scene.cull`, on by
  default): each is tested as its mesh's bounding sphere, moved and scaled
  with it, and only the visible ones are sent to the GPU. `stats().culled`
  counts the rest. `scene.overlay()` is a scene drawn over this one, in front
  of everything in it, with its own camera and this scene's lights: a
  first-person gun or hands, which then never sink into a wall. It is drawn in
  the same pass, in its own slice of the depth range. Lights:
  `scene.lights.sun` (`direction`, toward the light; `color`, linear, above 1
  is brighter: what a white surface facing it shows, so a Lambert surface of
  albedo a shows a times `color`; a glossy surface adds its highlight),
  `scene.lights.fill` (a second sun that casts no shadow, black by default),
  `scene.lights.ambient` (with `ground_ambient.enabled`, using
  `ground_ambient.color` for light from below: faces between take a mix by how
  far they face up), and point lights (`scene.add_point_light({position,
  color, radius, size})`: muzzle flashes, explosions, lamps). A point light
  falls off with the square of the distance, as light does: its `color` is
  what a white surface facing it shows one unit away (twice as far, a
  quarter), and the light is windowed to nothing at `radius` (`(1 -
  (d/r)^4)^2`, as glTF's punctual lights). `size` is the light's own size (1
  cm by default): nearer than that it is no brighter, as the light spreads
  over a body that big. Give flashes and fireballs theirs, or what stands in
  them blazes white. Before G1a the fall-off was `(1 - d^2/r^2)^2` without the
  inverse square: colours of those days are about the old colour times the
  square of the distance they were used at. The point lights whose reach is in
  view light a frame, the strongest and nearest first: up to 256 through the
  light grid (below), else 16 (`RendererCapabilities3D::max_point_lights`);
  point lights are cleared with the draws. Spot lights (`add_spot_light`, a cone of
  a point light) and the shadows of lights that are not the sun are below
  ("Spot lights and shadows from lights that are not the sun"). Light is added
  in linear light; the result is stored as the 2D pipeline stores colour, or
  kept HDR (see Views). The material shader skips a point light's shadow and
  BRDF evaluation when its windowed attenuation is zero. Positive
  contributions, however small, retain the same shading; this adds no light
  lists, caches or GPU buffers. Image-based light: `scene.lights.environment`
  is an `Environment3D`, an equirectangular HDR picture of the sky or a room
  (`r3d.load_environment( "sky.hdr")` reads a Radiance `.hdr` through the
  content folder or pack; `r3d.create_environment(size, rgb_floats)` takes
  linear RGB floats, rows top to bottom, 2:1). With one set, surfaces take
  their ambient light from it instead of `ambient`/`ground_ambient` (which are
  then not used; the sun, fill and point lights go on): the diffuse from its
  irradiance by the normal, the specular from a mip a roughness (GGX-filtered;
  a NaN or infinite texel of the picture is filtered as black) along the
  reflection, times the same split-sum fit as before. `environment_intensity`
  scales it, `environment_rotation` (degrees) turns it about +y. The picture's
  centre is +x, its top +y, and going right from the centre turns toward +z
  (Blender's world texture in kin's axes). The filtered copies (a 7-level
  equirect chain up to 1024 x 512 for the specular, 64 x 32 for the
  irradiance, half floats) are made by compute shaders in the first render
  that uses the environment, once (`debug_stats().environment_prefilters`);
  that render needs `compute` and a float format that storage textures can
  write, else the hemisphere stays. An environment is not drawn as a
  background (the view's `background` colour is), and in the overlay it is
  carried into the overlay's space. The sun casts shadows over
  `scene.lights.shadow_area`, a box round what casts and receives them (the
  level): one shadow map (`shadow_resolution`, 2048 texels by default) over
  that box, filtered over 3x3 taps. Opaque meshes cast (`casts_shadow`);
  copies culled from the camera still cast when their shadow may fall in view
  (the copy swept down the light, through the area, reaches the camera's
  view), and are left out otherwise. A smaller box is sharper. The overlay
  gets no shadows. Sun receiver offsets are configurable in shadow texels:
  `shadow_bias` (depth, 1 by default) and `shadow_normal_offset` (1.5 by
  default). Sun receiver offsets use the mesh normal facing the viewer, rather
  than normal-map relief: changing a texture must not move an unchanged
  caster's shadow. Flat-shaded clusters with zero vertex normals retain their
  reconstructed face normal. Opting into `shadow_receiver_plane_bias` corrects
  each PCF tap for the receiver's geometric depth gradient, including the
  comparison sampler's subtexel-dependent bilinear footprint. On planar
  receivers it allows small offsets on thin contacts without coplanar
  self-shadowing. Coarse curved meshes can produce streaks or triangular
  light/shadow patches where the filter extrapolates across different facets;
  use conventional PCF for these. XC121 uses conventional PCF with depth 1 and
  normal offset 1.5, supported by finer maps for thin cables. Derivatives for
  the optional correction are taken before the sun-facing branch and cascade
  selection/blending; the correction applies independently in each cascade.
  These are receiver uniforms: changing them updates shading while a valid
  cached depth map remains reusable. No additional texture or shadow pass is
  allocated. Local-light and moon bias settings are unchanged. A single-sided
  mesh casts with the faces it shows, toward the sun: for a closed mesh that
  is the same shadow as both sides for half the work, but a lone quad facing
  away from the sun (a ceiling seen from below) casts nothing. Make it
  `double_sided` to cast from both sides. `stats()` counts the shadow map's
  draw calls and the meshes drawn into it. **Cascades.** One map over a big
  area is coarse: its texels are the area's size over `shadow_resolution`, the
  same far and near. `lights.shadow_cascades` (1 by default: the single map
  above, the picture unchanged; 2 to 4, more is 4) splits the camera's view by
  depth instead, nearer cascades covering less, and gives each its own
  `shadow_resolution` square map (a layer of one depth array, a shadow pass
  each). `lights.shadow_distance` (100 units by default, never past the
  camera's `z_far`) is how far from the camera they reach; the splits are a
  blend of logarithmic and linear (0.6 of the log one), so the first cascade
  covers a few metres and the last the rest. Each cascade's box is the sphere
  round its slice of the view, so it keeps its size as the camera turns,
  snapped to whole texels in the sun's space so edges don't shimmer as it
  moves; `shadow_area` still limits it, and is what casts and receives.
  `mesh.frag` picks the cascade by the depth of the pixel along the camera's
  forward, and fades into the next over the last tenth of each range (no
  visible seam), with 3x3 taps and a depth bias and normal offset that scale
  with the cascade's texel. A copy casts into each cascade whose slice its
  shadow may fall in (the same test as above, a cascade at a time), so a
  far-off thing isn't drawn into the near maps. Without a depth array to
  render into the renderer makes fewer layers (down to one: the single map),
  says so once in the log, and never fails. Costs more than one map: a pass
  and the casters of each cascade, a few more taps; use it where the area is
  much bigger than what the camera sees close up. `stats().shadow_cascades`
  counts the maps; `debug_stats().cascade_casters` counts copies drawn into
  each. **Kept while nothing moves.** `lights.cache_shadows` (off by default:
  a game that pauses or has photo mode turns it on) keeps the map(s) from one
  render to the next and does not draw them again when they would come out the
  same, to the byte: the sun, the area, the resolution, the **camera** (what
  casts into view, and the cascades' fit, depend on it), the levels of detail,
  and every caster's mesh, material and copies must all be as they were (a
  cut-out's texture too: written in place, it is seen; a render target as the
  texture is never cached). It pays for a paused game, a photo mode, a viewer
  at rest (the shadow pass is a quarter to a half of such a frame) and costs a
  moving scene nothing measurable (a key of a few hundred bytes and a sample
  of the copies; the copies are compared in full, stopping at the first
  difference, only once two renders agreed). `stats().shadow_cached` is true
  when it hit (the shadow draw counts are then 0). Set
  `scene.lights.cache_shadows = false` for a benchmark that renders one scene
  many times. See the plan's "Opt-in speedups, phase 1". ### Ambient occlusion
  `scene.ambient_occlusion = {.enabled = true, .radius = 0.5f, .intensity =
  1.0f, .half_resolution = false}` (off by default; off, the picture and the
  cost are those of a renderer without it). The opaque and cut-out meshes the
  camera sees are drawn first into a depth and world-normal pre-pass (one
  sample, whatever the view's MSAA; where the main pass has one sample too, as
  with temporal anti-aliasing, it starts from a copy of the pre-pass's depth,
  below); GTAO (2 slices, 8 steps a side, a thickness heuristic)
  runs in compute from it, then a depth-aware blur (R8 result). `mesh.frag`
  multiplies the ambient light and the environment's diffuse by it (Jimenez et
  al.'s multi-bounce fit, from the surface's colour) and the environment's
  reflections by a specular occlusion term; the sun, fill and point lights are
  not touched. `radius` is how far (units) something can still shut light off:
  a few tenths of the size of what you want creased (0.5 for props, several
  units for rooms); `intensity` is the power the result is raised to. Blended,
  additive and wireframe meshes read no occlusion (they would read what is
  behind them), nor does the first-person overlay, which is not in the
  pre-pass either: its own lighting stays as it was. Where the device lacks a
  sampled depth, a two-float colour target or an R8 storage texture, occlusion
  is off with one line in the log. `half_resolution` computes and blurs at half
  the view's size, then reconstructs against full-size depth and normals to
  avoid borrowing occlusion from a different surface. Reconstruction adds one
  compute dispatch and a full-size R8 target; `ao_dispatches` includes it. The
  original linear upscale is the fallback when that optional shader is unavailable.
  Half size remains a speed/detail tradeoff; full resolution is the default.
  `debug_stats()` has `ao_draws`, `ao_instances` and `ao_dispatches` (0 when
  off). Costs a second draw of every opaque copy and about 0.3 ms or more of
  compute at 1280 x 720: see the plan's G2b results. With contact shadows also
  on (and `half_resolution` off) the two blurs are one dispatch a direction
  (`debug_stats().blur_merged` is 2; the counts above still count each
  feature's own); the result is the same to the byte. The blur ends in the
  first target again (raw, across, raw), so a feature holds two R8 textures,
  not three. In views of 1.5 million pixels or more (about 1440 x 1040; 1920 x
  1080 and up) the occlusion reads a **linear depth chain** instead of the
  depth: one compute dispatch writes the distance in front of the camera of
  every pre-pass pixel and four coarser levels (R32F, a weighted average of
  four texels that leans to the farthest, after XeGTAO), a tap picks its level
  by its distance from the pixel (the first level from 20 pixels, the next
  from 40, and so on) and the blurs read level 0, so the far taps of a wide
  search miss the cache far less and no tap divides to find its position. It
  gives the pictures of the old path to within 0.2 levels on the reference
  scenes (`debug_stats().ao_chain_built` is 1; the chain's own dispatch is not
  in `ao_dispatches`) and saves 0.2 to 0.5 ms of the 1.3 to 2 of the occlusion
  at 1080p and 25 to 30% of it at 4K, but costs a little more where the
  occlusion's reach is a few pixels (far views, the kiln at 1080p: +0.2 of 8
  ms): the plan's "Opt-in speedups, phase 2". Below that size the old path
  runs and the picture is the old one to the byte. Contact shadows blurred on
  their own (no occlusion in the same view, or the occlusion at half
  resolution) get one blur pass, not two (-0.03 ms at 1280 x 720, -0.1 at
  1080p; the pictures differ by under 0.3 levels on average):
  `debug_stats().contact_dispatches` is then 2 (the march and the blur). ###
  Contact shadows `scene.contact_shadows = {.enabled = true, .distance = 0.5f,
  .thickness = 0.1f, .intensity = 1.0f, .steps = 20}` (off by default; off,
  the picture and the cost are those of a renderer without it). For the small
  shadows a shadow map is too coarse for: the foot of a pole, a plate on the
  ground. A compute pass marches from each pixel toward the sun through the
  pre-pass's depth (the same pre-pass and targets as ambient occlusion, drawn
  once when both are on, so it works with occlusion off), starting a different
  amount along the first step at each pixel; a sample counts as blocking when
  the depth there is nearer the camera than the ray, but by no more than
  `thickness` (a thing that is further in front is taken to be a thin occluder
  the ray passes behind, so a distant plate in front casts nothing). The
  result fades over the last 40% of `distance` and near the screen's edge,
  goes through the occlusion's depth-aware blur, and is read by `mesh.frag`,
  which multiplies **only the sun's term** by it, after the shadow map's and
  faded with `Lights3D::sun_shadow_strength` as the map's shadow is (at 0 the
  march does not run): the fill, ambient light, environment and point lights
  are untouched. `distance` (units) is how far toward the sun to look;
  `thickness` (units) how deep an occluder is assumed to be (smaller: fewer
  false shadows from near things, thin shadows may thin out; larger: the
  reverse); `intensity` 0..1; `steps` 4..64. Surfaces turned from the sun,
  blended, additive and wireframe meshes, the first-person overlay and meshes
  that do not receive shadows read none. Marched in compute rather than in
  `mesh.frag`: the main pass shades every fragment of the overdraw and the
  march would run in each, where compute runs once a pixel and `mesh.frag`
  stays one texture read. Without a sampled depth, an RG16F colour target or
  R8 storage it is off with one line in the log; with no sun it does nothing.
  `debug_stats()` has `contact_dispatches` (0 when off); the pre-pass shows in
  `ao_draws` and `ao_instances`. Custom shaders see a tenth sampler
  (`uContactShadow`) in kin's include; ones compiled before still work. Full
  size only; about 0.15 to 0.3 ms at 1280 x 720 (see the plan's G2c results).
  ### Temporal anti-aliasing `scene.taa = {.enabled = true, .blend = 0.9f,
  .still_blend = 0.96f, .sharpness = 0.0f, .jitter_scale = 1.0f, .jitter_samples = 8}` (off by
  default; off, the picture is bit-identical and nothing extra exists: no
  pass, no buffer). Each frame is drawn a Halton (2, 3) fraction of a pixel
  off (`kin::taa_jitter`, `kin/render3d/taa3d.hpp`), and a compute pass blends
  it into the picture of the frames before. **It replaces MSAA while on**: the
  view draws one sample a pixel whatever `ViewOptions3D::samples` asks
  (`stats().samples` says 1).
- **Where the settings live.** On the scene, like occlusion: it can be switched
  live without making the view again. What it keeps between frames, the
  history, belongs to the **view** (the scene is rebuilt every frame): made by
  the first frame that asks, dropped by the first that doesn't. `View3D::reset_history()`
  drops it by hand (a camera cut, a scene swapped); a resize, the first frame
  and a change from a perspective to an orthographic camera do it by themselves
  (`debug_stats().taa_reset`).
- **Jitter** moves the projection of the passes that draw the picture (the main
  pass, the pre-pass, the occlusion and contact-shadow maths that unproject its
  depth, the overlay). The frustum culling, the shadow cascades' fit, the pick
  pass, `Camera3D::project` and `ray` use the exact matrices.
- **Velocity** is written, from each copy's matrix of the frame before, kept in
  a buffer of its own beside the instances (the 128 B instance is untouched) and
  the view's last view-projection, **by the main pass** as a second colour
  output when temporal anti-aliasing is all that needs a depth pre-pass (no
  ambient occlusion, contact shadows or occlusion culling, and no particles or
  custom material shaders): there is then no second geometry pass for it
  (`debug_stats().taa_main_velocity`, `ao_draws` 0), which was the largest part of
  TAA's cost in big scenes (the kiln's fell by 2.2 ms of 7.2). Opaque and
  cut-out meshes write it; blended, additive and wireframe ones leave it as it
  is. Colour-only particle/custom shaders use the pre-pass to preserve the motion
  attachment on every backend. With any of those other features the pre-pass is drawn once and shared,
  writing four floats (the normal and the velocity, instead of two). The
  pictures of the two ways are identical to the byte. The first-person overlay has its own pass
  and its own previous camera, so a gun is neither smeared by the world behind
  it nor trailed.
- **The main pass from the pre-pass's depth:** wherever a pre-pass ran (any of the features above) and the main pass
  has one sample a pixel at the view's size (temporal anti-aliasing, or a view without MSAA) and no overlay (whose
  depths the main pass keeps in a range of their own), the main pass begins from a copy of the pre-pass's depth
  instead of clearing its own. The fragments the scene's nearest surfaces hide then fail the depth test before they
  are shaded: the light variants' shading runs about once a pixel instead of once a fragment of the overdraw. The
  pre-pass's copies are drawn again over themselves at the same depths: every mesh vertex shader's position is
  `invariant`, so the two passes' depths agree to the bit; what the pre-pass leaves out is drawn as before. The
  picture is the one clearing the depth gives, to the byte. `debug_stats().main_depth_from_prepass` says when;
  `scene.debug.prepass_depth = false` clears it as before. Bistro at 2404 x 1352 (GI, AO, TAA): about 1.4 ms of GPU
  a frame less at night, 0.75 by day.
- **Matching copies from frame to frame** (the scene is rebuilt, so nothing
  says which copy is which): by `MeshInstance3D::id` when it isn't 0 (give moving
  things stable ids to get the best); else by place in the mesh's copies in
  draw order, if there are as many as last frame and the copy hasn't moved more
  than eight times its size; else by where it was (the nearest copy of the mesh
  within twice its size). A copy none of these find, or a mesh not drawn last frame,
  has no past: its velocity points off the screen, and the resolve uses no history there.
  Limits: copies of one mesh that cross within twice their size while the list
  also changes can swap pasts (a wrong velocity of at most that distance; the
  clip below bounds the damage); the key is the mesh, not the material, so a
  material that changes between frames does not break matching; a copy that
  moves more than twice its size in a frame has no history (bullets: give them
  ids). Costs CPU: about 0.3 ms a render for 22,000 copies.
- **Resolve** (`taa.comp`): reproject by the velocity of the nearest surface round
  the pixel (its 3 x 3, by the main pass's depth; a surface within a fiftieth of
  the pixel's own depth is the same one): each frame's jitter puts a pixel at an
  edge on one surface one frame and on the other the next, and read by its own
  flickering velocity a moving edge (a character's) would keep little of its
  anti-aliasing. The world round the overlay keeps its own, as the gun is fixed to
  the screen while the world turns behind it. Read the history with a Catmull-Rom
  filter (five taps), clip it to a 3 x 3 box of this frame's colours in YCoCg
  (mean and spread, inside the neighbours' range), mix with weight `blend`, less
  where no surface within a pixel of the spot read from moved as this one did last
  frame (the velocities the last frame had there), a little where the history is
  read between pixels (at most 30%), with speed and with how far the clip moved
  it; none off the screen, for a new copy or the first frame. An edge moving a
  tenth to a whole pixel a frame is within a quarter of a still one's error
  against a 16-sample picture (it was 2.3 to 8 times it). A **still pixel**
  (moving under half a pixel a frame, in between by degrees) reads its history
  exactly, so it keeps more of it (`still_blend`, never below `blend`; at `blend`
  or below, still pixels are treated as moving ones) and its clip box goes by the
  spread alone, 2.5 of it, past this frame's range: detail finer than a pixel (a
  menu's print, a railing's bars), which one frame's nine samples can all miss
  where the jitter puts them, was pulled to the background and back once a jitter
  period, a shimmer with the jitter's rhythm. A flat neighbourhood still clips as
  tightly (a change of light there is followed at once); the tests that drop
  history (another surface, a change of light) apply as before. Not solved: a bar
  thinner than a pixel against a perfectly flat background (the sky) loses most
  of its contrast, as the frames that miss it clip it away. This frame's colour is
  read from its 3 x 3 neighbours by how near the pixel centre each sample fell (a
  Gaussian), not as the pixel's own sample, which would pull the picture to
  wherever the last few jitters were (with the jitter at 0 it is the pixel's own).
  Light is compressed (`c / (1 + max c)`) for the clip and the mix. `sharpness` (0
  to 1) adds a local-contrast boost kept inside the neighbours' range, one more
  dispatch, within surfaces: not where the depth bent across the pixel's cross in
  the last eight frames (a silhouette, the sky beside a roof, a crease; the depth
  runs straight across any plane, however slanted), whose blended pixels the boost
  would push back into steps. Eight frames, not this one alone: the jitter moves a
  thing thinner than a pixel in and out of one frame's depth, so its pixels were
  sharpened in some of the jitter's places and not in the others. The resolve
  keeps how many frames ago the depth bent in the two lowest bits of each half of
  the velocities it keeps for the next frame (a relative 0.4% of the velocity,
  far under the half pixel it is judged by), followed as the picture moves; no
  memory is added. An outline's pixels keep the resolve's blend; the cells of a
  checkerboard on one plane come out 29% steeper.
- **Before the post chain**: bloom, exposure and the tonemap work on the
  resolved picture. An 8-bit view is drawn in linear float into a texture of its
  own, resolved, then clipped at white and encoded (`view_output.frag` mode 3);
  what it loses is MSAA's resolve in encoded values (edges are averaged in linear light,
  as an HDR view's are), and the extra RGBA16F targets.
- **Particles and transparency:** particles/weather and blended/additive meshes
  write their own reprojection motion and local coverage after AO/contact shading.
  Opaque custom materials retain opaque velocities. New sprites, discontinuous
  appearance changes and unversioned render-target textures reject their affected
  footprints; motion elsewhere does not reject the entire transparent scene.
  Receiving particles record shadowed irradiance, so a redrawn moving shadow
  lowers history confidence where illumination changed. Redrawing an identical
  shadow map with caching disabled does not change its content revision.
  Stationary coverage uses the normal history weight; moving composites cap it
  at 0.6 because independently moving overlapping layers have several velocities.
  Coverage respects depth, texture alpha, procedural edges, alpha-zero emission,
  fog and soft intersections. Blended custom materials use their triangle
  footprint because arbitrary fragment code owns its coverage. The extra pass
  reuses the normals target after AO/contact shading; it adds no texture. Particle
  quad history uses a separate 64-byte GPU record per visible sprite (up to
  64 MiB at the colour cap), plus at most 65,536 CPU history records per view.
  Its GPU buffer is allocated only with particle TAA and released after 120 idle
  renders. `debug_stats().taa_reactive_draws` counts coverage draws separately.
- **Not covered:** a camera cut needs `reset_history()`; a pan of exactly half pixels a
  frame lines the jitter up with the motion and leaves part of each pixel
  unvisited (rates that don't repeat are fine); thin detail in a pan
  is softer than MSAA's. The occlusion's and contact shadows' per-pixel noise
  is fixed, not varied by frame: left.
- Without RGBA16F as target and storage, or without what the pre-pass needs, it is off with
  one line in the log. `debug_stats()` has `taa_dispatches` (0 when off, 2 with
  sharpening), `taa_reset`, `taa_jitter`, `taa_reactive_draws` and `samples`.
  See the plan's G2d results for numbers.

### Global illumination

`scene.global_illumination = {.enabled = true, .area = level_box, .cell = 0.15f}` (off by default; off, the picture
and the cost are as without it). Light that reaches a surface off other surfaces, and the sky's light kept out of where
walls hide it, built in layers that several strategies share (the plan's G5): the **GI scene**, the scene in voxels; the
**lit scene**, the light leaving each of its surface voxels; and the first strategy, **probes** traced through them
(DDGI), which light the picture. With it on, a surface's diffuse light from the sky or the environment becomes the
probes': the sky's light where they see the sky, the light of every surface they see, none through a wall. Sponza against
Cycles goes from 50 levels off on average to 7, the Cornell box (lit by its panel through the probes) from 10 to 7.6: see
the plan's G5b results.

- **The GI scene:** voxels of `cell` units over `area`, in `levels` levels (3, up to `max_gi_levels`, 4), each a
  window of `side` x `height` x `side` voxels (128 x 64 x 128, rounded up to whole blocks of 8; at most `max_gi_side`,
  256, a side and `max_gi_voxels`, 4,194,304, in all) and each level's voxels twice the last's: windows round the
  camera, near it fine and further out coarse, moved a block at a time as it moves. The first level whose window holds
  the whole area stays over it, and is the last (the first level: a window just big enough). Every voxel holds its
  distance to the nearest voxel a surface is in (up to 8 voxels); each voxel a surface touches holds that surface's
  colour (the base colour, a metal's too: the material's, the copy's tint, the vertices' and the texture's at the
  voxel's nearest point of the surface, read at the mip whose texels are the voxel's size), its normal (the face's, on
  the side its vertices' normals are; a voxel with faces looking both ways, a wall thinner than a voxel, counts as
  double-sided) and its emission (as `mesh.frag` makes it). It takes in every opaque and cut-out copy that reaches
  into `area` (a copy's box; a skinned copy's from its joints' spheres in its pose), skinned copies in their pose (none
  with `skinned` off: an animated character's pose makes the blocks round it again every few renders; off, it is lit
  by the GI but neither shades nor lights it, as Godot's SDFGI holds static geometry only), a cut-out only where its
  texture's alpha at the voxel's size passes its cutoff; not the overlay, nor blended, additive, wireframe or unlit
  draws. Beyond the last window there is nothing (rays that leave it see the sky).
- **Surface materials** (a surface shader's or a material graph's: `Material3D::surface`) are seen through a stand-in:
  what the surface makes of the material over its UV square (64 texels a side, a square 4 units across seen from
  above), its base colour and alpha in the base map's place and, for a material that emits, its emission in the
  emissive map's, made before the first render that needs it (the material drawn unlit, and again lit by nothing,
  which leaves its emission alone) and again when the material changes (its surface, parameters, maps or colour).
  Its maps themselves are not read: a graph's base map may hold a mask or a normal map. What the stand-in can't see
  in the surface's SPIR-V is in its `SurfaceTraits3D` (`SurfaceShader3D::traits`), which a material graph's compiler
  fills in: `emission`, the surface makes its own (a graph with an emission output), so the stand-in's emission is
  light in the GI even with the material's emission fields off (else only a material whose fields emit,
  `emits_light`, glows there); `vertex_color` and `vertex_alpha`, its colour and alpha read the vertex colour (the
  stand-in is drawn with a white one, and the voxels take the copy's vertex colour and tint only where they do; a
  surface shader of your own has the stock surface's defaults: no emission of its own, both read; make it with
  others as `SurfaceShader3D{shader.backend(), traits}`). Its limits: a surface that depends on where it is or which
  way it faces (world-aligned maps, a slope's rock over a flat ground's sand) is seen as on flat ground facing up,
  and one that uses the vertex colour as a mask, not a tint, is seen through a white one times it; an animated one
  at the time of its stand-in; its emission is the voxels' (an emissive map's, not an area light); a World Position
  Offset does not move its voxels.
- **How it is built:** in compute, before the passes that draw. Every voxel a triangle touches (Schwarz and Seidel's
  conservative surface voxelization), so a wall of no thickness has no gap a ray can pass. A voxel's owner, which
  writes its colour, normal and emission, is the touching triangle drawn first, an emissive one before any other (a
  lamp's panel a millimetre under a ceiling keeps its light). A jump flood (Rong and Tan) gives the distances. Made
  again only where something changed, by blocks of 8 x 8 x 8 voxels: those a copy that came, went, moved or changed (a
  pose, a mesh, a material's colour, textures, or whether it emits, a tint) covers or covered, a voxel more each way,
  and the slabs a window gains when it moves (all of a window that jumps past where it was; all of every level when the
  settings change). A block made again is emptied, voxelized from the copies that reach into it, and its distances
  flooded again with a margin; the rest keep theirs. A voxel holds the same whether it was made with the rest or
  alone, and wherever its window stood: the same scene gives the same voxels to the byte however it got there. What
  changed is made in the first level at once after a render in which nothing changed there, else (something that
  keeps moving: an animated character, a falling prop) every fourth render; in level k every 4 x 2^k renders, the
  levels in different renders (the light it bounces is low and slow, the probes' kept over many renders).
  `debug_stats()`: `gi_built` (anything made this render), `gi_blocks`.
- **Emitters:** how strongly a material emits is not part of its voxels. An emissive draw's voxels hold its emission at
  a strength of one and the number of its emitter (the draw's material, its strength left out: up to
  `max_gi_emitters`, 2048, numbered afresh with each whole build), and the GI's light multiplies by the emitter's
  strength of the render. A strength that changes from render to render (a lamp dimmed, a game that scales its
  emissives by its exposure) makes nothing again; the light follows within the eight renders the lit scene's blocks
  take in turn. A draw that differs from one already numbered only by its strength (in the same render), or that finds
  no number left, keeps its strength in its voxels, and a change of it is a change of what they are made of.
- **Emitters as area lights** (`global_illumination.emitters`: `GiEmitters3D::Exact`, the default; `Probes` leaves
  them to the probes, as on main before, cheaper and softer, without their contact shadows or reflections): each
  numbered emitter whose material has neither an emissive nor a base-colour map is an area light (two triangles that
  make a parallelogram as one; at most `area_lights.max_emitters`, 64, whole GI emitters in their numbering, those
  past it left with the probes; so are those whose largest piece is under `area_lights.min_size` across, by default
  half the first level's voxel: a candle's flame, an eye, a firefly). Where the camera sees, on gi_resolve's half-size
  texels, its light is the polygon's exact form factor (clipped to the surface's horizon) times the share of it 4
  lines across it find unshadowed: each line's shadowed stretches exact (an occluder triangle blocks one interval of a
  line: Moller-Trumbore's numerators are affine in the place along the emitter), its light along them by 2 Gauss
  points a stretch, whose error cancels in the share (a large emitter close by, which the lines alone sample poorly,
  is right where nothing blocks it). The voxels get their light from 2 such lines directly (each voxel's front kept
  while the area lights' scene and strengths stay), for the bounces: coarse next to a large emitter. The probes no
  longer carry those emitters' own light (it would count twice); the light on the view fades out with the probes'
  toward their windows' edges, and is none past them. The occluders are every copy's triangles but skinned ones, in
  two levels of bounding volume hierarchy (each mesh's in its own space, made once, under the copies'; copies
  unchanged for 8 renders join one world-space hierarchy), walked with the shaft from the surface to the emitter; a
  closed single-sided mesh's faces with the surface behind them are skipped (its faces toward it block the same). Past
  `area_lights.max_triangles` (131,072) occluder triangles in all, each mesh but the emitters' shades with a coarser
  shape of its own (meshoptimizer's simplifier, halved as many times as it takes). With the temporal anti-aliasing a
  render finds one texel in `area_lights.interleave` (4; 2 or 1) and takes the others from where their surface was in
  the render before (by the motion between the two views without the jitter); those it cannot take are found in the
  same dispatch, and a light or shadow that moves lags up to interleave - 1 renders there. Without it every texel is
  found. Reflections: each emitter's GGX lobe by linearly transformed cosines (Heitz et al.; three.js's tables, MIT),
  times the share of the diffuse light found unshadowed (one visibility for every emitter). A scene without such an
  emitter costs what it does with `Probes`. Error against Cycles (mean levels over the room): the Cornell box 4.4 with
  the probes alone, 1.8 with area lights; a room lit by three panels of three shapes 18 and 4.8. Cost over the probes
  alone at 1280 x 720 (RTX 4080 laptop, at 2 GHz): 0.13 ms the Cornell box, 0.63 ms the three-panel room; dense
  occluders cost more (64 balls of 6,016 triangles round three emitters: 200 ms, 65 within the default budget).
  Memory: 8 bytes a voxel, 16 bytes a texel at half size and 8 more for the texels to find, 64 bytes an occluder
  triangle.
- **The lit scene:** each surface voxel's light: its colour times the sun's and the fill light's, each point and spot
  light's within reach (kin_lit's fall-off and cones) through a ray walked toward the light through the voxels (a
  surface in the way: none), and the probes' light (the bounce of the render before), plus its emission. A double-sided
  voxel has each side's own light, so a wall between a lit room and a dark one is dark on the dark side. Each render
  lights the blocks it made and an eighth of all the others in turn (a light that changes is in all of it within eight
  renders).
- **The GI's sun map:** the sun's light reaches a voxel through the sun's own view of the meshes, as the main pass's
  shadows, not through the voxels: an orthographic map (`gi_sun_map_side`, 4096, a side) along the sun over the whole
  of `area` (whatever the camera sees), drawn as the local lights' maps are, before the GI, and kept while nothing in
  it changes (always, whatever `Lights3D::cache_shadows` says: what is kept is what would be drawn). A voxel's side
  reads it at four points a third of a voxel from its middle, a voxel out along its normal, so a voxel half in the sun
  is half lit. The voxels' own march, whose cover is conservative (every voxel a surface touches full) and coarser in
  each window further out, closes the narrow gaps the sun comes through: in Bistro's afternoon the sunlit voxels gave
  a median 0.49 of the light the main pass shows there (a quarter of them near none), and the shade got 0.54 of the
  sun's bounce Cycles gives it; with the map 0.76 and 0.68. The march stays for the fill light, the point and spot
  lights, and a voxel outside the map. `debug_stats().gi_sun_map`; `Scene3D::debug.gi_sun_map = false` marches the sun
  too. Memory: 64 MB (D32).
- **The probes** (`global_illumination.probes`, a `GiProbes3D`): on a grid every `spacing` voxels (2, 4 or 8: 4; further
  apart where a level would have more than `max_gi_probes`, 65,536), a window of them in each level, moving with the
  voxels'. A probe more than 1.8 spacings from every surface sleeps (at most 7.9 voxels: the distances are kept to 8),
  and one within 1.8 voxels of a surface moves to the place with the most room nearby; they are placed again where the
  scene changes (one stays where it stands while that still has room, and one its rays found inside something stays
  so till they say otherwise: moved or woken, a probe would light with what it saw from elsewhere till its next
  update), and a probe new to its window starts empty. Each render a batch of them (`per_render`, 2,048; 0, all;
  at most a million rays) each casts `rays` rays (128, a spherical Fibonacci set turned each render) through the
  voxels, from the finest window that holds it on to the coarser ones: a hit reads the lit scene, a miss the sky (the
  environment's specular chain, or the ambient and ground ambient). The probes new to their windows go first, the
  others in turn, each level its share of the rest: 4 of 7 to the first (round the camera, most of the picture), 2 to
  the next and 1 to the third. Each keeps its irradiance (octahedral, 6 x 6 texels) and the mean and mean square of its
  rays' distances (14 x 14), keeping `hysteresis` (0.97) of what it had, or less while the probe is young: the mean of
  its updates so far (none at its first, half at its second...) until that weighs as much (the irradiance's alpha
  counts them). With three windows' 49,152 probes and 2,048 updated a render, a probe is updated only every 14 to 56
  renders, and at 0.97 an update a new probe, or the bounced light building up, took most of a minute to settle (in
  `canyon`, a street against Cycles, the shade was 21% short after 480 renders; now 9%, 7% when settled; in the
  Bistro, the shade 5 to 7% short for ten seconds after a walk, now settled within one). One whose rays meet more
  than a quarter backs of one-sided surfaces is inside something and gives nothing.
- **What a surface reads:** the probes of the finest level whose window holds it: the eight round it, each weighted by
  distance, by whether it is in front of the surface, and by whether it can see it (Chebyshev's inequality on its
  distances that way), the look-up moved out three tenths of a spacing along the normal and a tenth toward the eye;
  times `intensity`. Over a window's last three cells of probes it smoothly fades into the next level's; the outer
  scrolling level fades to the actual ambient/environment lighting, for diffuse and specular together. A last fixed
  level that already contains the whole requested area retains its lighting; voxel bounce feedback also retains its
  outer light rather than treating the display fallback as lost energy. A scrolling window's
  blend follows the camera continuously, inset by half a voxel block so storage snapping cannot jump the ramp.
  Small windows shorten the ramp to retain an interior. Untraced probes contribute missing support rather than
  making the first ready neighbour supply full GI. Sleeping/inside probes are still deliberately excluded.
  Keep important surfaces inside the outer fade band; include the floors and roofs in the requested GI area.
  They are read once a texel at half the view's size, after the depth and normal
  pre-pass (which global illumination brings with it), on the surface each texel stands for (the nearest of its 2 x
  2 pixels): the light facing its normal, and how its brightness changes with the way faced (a linear fit from three
  more ways, 60 degrees round the normal). With temporal anti-aliasing, each texel keeps 85% of what the same place
  held the render before (found where the last view put it, if it held the same surface, give or take its own pixels'
  spread), so that the jitter, which moves what a texel's pixels show at the edges of things, does not make the light
  flicker; a change of light shows within a few renders. The main pass takes the four texels round each pixel,
  with the current GI coverage in a separate half-resolution R16F target (0.44 MiB at 1280 x 720, 1.76 MiB at
  2560 x 1440; no coverage history), filtered with the same surface/depth choices as the light. Coverage follows the
  current window immediately; old lighting history cannot keep GI alive outside it. The light and slope are
  filtered: those at its depth (all four, most often), else the nearest in depth (a thin thing between texels); for
  what the pre-pass did not draw (glass, in front of what it shows) all four; and turns that light to the normal map's
  normal for the diffuse and to the reflection: a rough surface reflects
  that, a smooth one the environment dimmed by the share of the sky's light that is. (Read by each fragment instead,
  the probes cost the Bistro 13 ms at 2844 x 1600, the overdraw's fragments too; read so, about 1.4.) GTAO and the
  occlusion maps still multiply both (keep GTAO's radius under the probes' spacing, or creases are darkened twice).
  Only the stock shaders' light variants read them: with them on, those draw (`debug_stats().lights_variant`); a
  custom material shader keeps the sky's light, and the overlay its own.
- **Where:** compute that writes 3D textures of half floats, 8-bit and four-half-float texels and reads and writes
  them for the probes (`RendererCapabilities3D::global_illumination`), the pre-pass's compute (as ambient
  occlusion's), and the light variants (the shadow format); else off, with one line in the log the first time it is
  asked for. Where the device has it, the meshes' buffers are made readable by compute, and each mesh keeps the box of
  each run of 64 of its triangles (the voxelizer takes only the runs that reach what changed), which costs nothing
  unused. Not on WebGL2.
- **Memory:** 42 bytes a voxel (the scene's 18, the lit scene's two sides' 16, the build's 8) and 1.5 KB a probe, for
  every level (the default's three levels of 1 million voxels and 16,384 probes: about 200 MB), and the light on the
  view (16 bytes a pixel at half the view's size), counted in `debug_stats().feature_bytes` and given back after 120
  renders without it, as the other features'.
- **Authored:** `SceneEnvironment3D`'s `gi_enabled`, `gi_min`, `gi_max`, `gi_cell`, `gi_levels`, `gi_side`,
  `gi_height`, `gi_skinned`, `gi_probes`, `gi_probe_spacing`, `gi_probe_rays`, `gi_probes_per_render`,
  `gi_hysteresis`, `gi_intensity`, `gi_emitters` (`"exact"` or `"probes"`), `gi_area_max_emitters`,
  `gi_area_max_triangles` and `gi_area_interleave`.
- **Seeing it:** `scene.debug.gi_view` (`GiView3D::Albedo`, `Normal`, `Emission`, `Steps`, `Radiance`, `Irradiance`)
  shows the GI scene traced from the camera instead of the picture: each pixel walks the voxels its ray passes, in
  jumps the distances allow, to the first one a surface is in; `Radiance` is the light leaving it, `Irradiance` the
  probes' light there. `r3d.read_gi_scene(view, out)` reads the first level's voxels back (a `GiScene3D`, in its
  window's order), waiting for the GPU. `debug_stats()` has `gi_voxels` (all the levels', 0 without), `gi_levels`,
  `gi_built`, `gi_blocks`, `gi_copies`, `gi_triangles`, `gi_dispatches`, `gi_probes`, `gi_probes_updated`, `gi_rays`,
  `gi_lit` and `gi_resolved`. With `debug.gpu_pass_markers` each of its passes is a debug group (`kin3d.gi_clear`,
  `_voxelize`, `_attributes`, `_flood`, `_place`, `_light`, `_trace`, `_update`, `_resolve`, `_area`).
- **Cost:** see the plan's G5a to G5c results: the Bistro at 2844 x 1600 on an RTX 4080 laptop, its mannequin
  posed anew each render, about 1.6 ms of GPU a render at full clock and 2.35 throttled (a fifth of the frame without
  it: the GI scene about 15%, the probes 28%, their light on the view 21%, reading it in the main pass 36%).

### Levels of detail

`r3d.create_mesh(data, {.levels = 4})` (or `load_model(path, {.levels = 4})`) builds, at load, up to
`levels - 1` simpler versions of a mesh with meshoptimizer's simplifier (`MeshOptions3D`,
`kin/render3d/mesh_lod3d.hpp`): each about half the triangles of the one before (`target_ratio`),
normals, UVs and vertex colours weighed in, open borders locked, and the chain stopped where a level
would gain under 15%, have under 16 triangles, or lie further than `max_error` (a tenth of the mesh's
size) from the one before. **Level 0 is the mesh as given**, to the bit; the levels are only more index
lists over the same vertices (a mesh costs a few more indices, not more vertices). `Mesh3D::level_count()`,
`level_triangles(i)` and `level_error(i)` (an upper bound, in the mesh's units, on how far level i
lies from level 0: meshoptimizer's error for each step, added up) read them back. A mesh without
indices (a glTF with no normals is read as flat-shaded triangles) is welded by position, and its levels share
vertices of their own (kept after the mesh's; level 0 is untouched): one for each welded position they use, with no
normal, so that the shader makes each face's from derivatives as for flat clusters (a normal map has no effect on
those levels). A vertex for each corner of each level, as before, took the kiln's 4 levels 12.8 M vertices (1.1 GB
more at load, all told); for a 2 M-triangle flat torus with 4 levels it is 0.5 M vertices (17 MB) against 5.25 M (180
MB), on the CPU and the GPU, and the `kin_reference` run peaks at 0.85 GB against 1.17.

`scene.lod = {.enabled = true, .error_pixels = 1.0f, .bias = 1.0f}` (off by default;
off, the picture, the statistics and the cost are those without it) draws each copy at the coarsest level
whose error, times the copy's largest scale, spans no more than `error_pixels / bias` pixels at the
nearest point of its bounding sphere (`distance` along the view, `0.5 * view height / tan(fov / 2)` pixels
at a unit; orthographic cameras use the view's pixels a unit). A copy has one level a frame, in the
camera's pass, the shadow passes (every cascade), the depth pre-pass, the velocity pass and picking, so
a shadow is that of the surface drawn. Copies of a mesh and material at one level are one instanced
draw, so a draw is up to `levels` draws; `debug_stats().lod_copies_by_level` counts the copies the camera
sees at each level and `stats().triangles` the triangles drawn. Limits: whole meshes only (a big mesh
is drawn at one level whole: see the plan's G3b for clusters); no hysteresis (copies are not
matched from frame to frame except for TAA, so a copy at the edge of a threshold can change level as
the camera moves; what changes is under the error bound); alpha-blended meshes (drawn furthest first,
which levels would break) and meshes without levels are drawn as before; the overlay has a setting of
its own (`scene.overlay().lod`, off). The temporal history survives a copy changing level: copies are
matched by mesh and place, not by level.

### Sky, fog and time of day

Off by default; off, the picture and the cost are those without them (the old shaders are not touched: the fog is a variant of
the stock fragment shader, the sky a pass of its own). `kin/render3d/sky3d.hpp` and `timeofday3d.hpp`; docs/3d_plan.md "P3a" has
the numbers.

```cpp
scene.sky = {.enabled = true, .turbidity = 3.0f, .stars = true};       // a procedural atmosphere behind the opaque meshes
scene.fog = {.enabled = true, .density = 0.0004f, .height_density = 0.004f, .height_falloff = 0.05f, .base_height = 20.0f,
             .colour_from_sky = true, .sun_scatter = 0.25f};
kin::TimeOfDay3D day{.hours = 19.4f, .day_of_year = 172, .latitude = 45.0f};
kin::apply(day, scene.lights, scene.sky, scene.fog);                   // sun, moonlight (the fill), ambient, the sky's moon, fog colour
scene.lights.sky_lights_scene = true;                                  // ambient light and reflections from the sky
```

**The sky** (`Sky3D`) is a single-scattering atmosphere (Rayleigh, Mie, an ozone term) lit by `Lights3D::sun.direction`: `turbidity` (haze),
`sun_intensity` (the sun's light above the air; the sun's light on the meshes is that times the air's transmittance:
`kin::sky_sun_light`, which `apply()` puts in `lights.sun.color`), `intensity` (times the sky), `ground_color` (the ground below the horizon,
lit as a horizontal surface; the sky's colour reaches it over 9 degrees, no seam), a sun disc (`sun_size` degrees, `sun_disc` times the sun's
light: HDR, bloom has it), optional `stars` and a `moon` (a disc, and a glow). `SkyMode3D::Image` shows `Lights3D::environment` instead
(its intensity and rotation). It is drawn in the main pass after the opaque meshes and before the blended ones and the overlay, with the depth
test of "nothing was drawn here", so it costs a triangle's pixels where nothing else is, is correct under MSAA and TAA (the jittered
matrix, and the camera's turn as the velocity), and is HDR (bloom, exposure, tonemap) like the rest. A view with a sky is output through a
shader with a one-step dither (night skies ring in 8 bits without it). `debug_stats().sky_drawn`.

**The fog** (`Fog3D`) is `density` per unit of distance plus a `height_density` that thins upward with `height_falloff`
from `base_height`, from `start`, up to `max_opacity`; the optical depth along each fragment's **view ray** is exact (the integral of the
height density is closed: `kin::fog_optical_depth` is the same arithmetic on the CPU). The colour is `colour`, or with `colour_from_sky` the
sky's in that direction (so far geometry melts into it at the horizon, whatever the time of day), plus the sun's light times a lobe of
`sun_anisotropy` (`sun_scatter`). The meshes read the sky's colour from a table made on the CPU when the sky or the sun's height
changes (`sky_model::FogTable3D`: the scattering split by its phase functions, 48 elevations crowded at the horizon, within a third of a
level of `sky_radiance` and the same at and under the horizon), not the integral per fragment; the sky pass reuses its own, and particles
read the table per vertex. Applied to everything the main pass shades through the stock shader (opaque, cut-out, skinned,
instanced, clustered, blended, additive) and custom shaders using `kin_output`; the overlay (a gun in the hand) is not fogged.
The sky gets the height fog from `start` to infinity (the limit of a mesh's as it recedes), not the distance term, which an infinite
ray would make total. With `SkyMode3D::Image`, `colour_from_sky` takes `colour` everywhere (the panorama is not read for the fog):
set it to the panorama's horizon. `kin_fog.inc.glsl` holds the fog for a pass of your own. `debug_stats().fog_applied`.

**The fog's sun through the shadows** (`Fog3D::sun_shadows`, off by default): the sun's in-scattering only where the fog's
air is in sunlight. Lit everywhere, the fog in a street's shade (the air between the camera and a wall, all in the
buildings' shadow) shone with the sun as in the open and veiled the shade: in the Bistro, the fog alone took 18% of the
fine detail away against 9% for Godot's, whose volumetric fog is shadowed. After the depth pre-pass (which it turns on),
`fog_sun.comp` walks each ray at half the view's size in 16 stretches (closer together near the camera, each moved along
itself per pixel and per frame for temporal anti-aliasing to average), weighing each stretch's share of the fog's opacity
(the closed integral) by one filtered comparison against the sun's shadow map (the cascade whose slice holds it; lit
outside its map, past the cascades' reach or the one map's `shadow_distance`, by `sun_shadow_strength`), to the surface
or, for the sky, to the end of the height fog. Then the main pass, the sky pass and the particles leave the sun's
in-scattering out of their fog, and `fog_sun_apply.frag` adds it once a pixel after the main pass: the lobe toward the
pixel's ray times the opacity to what the pre-pass drew times the share, read from the four texels round it where their
rays' lengths agree with the pixel's (else weighed by how near they are). Read in the main pass instead, the share cost
every fragment drawn, the hidden ones too (0.35 ms more in the Bistro). With no shadows to go by, the picture is as without
it. A blended surface takes the light of the ray to what is behind it; a custom shader's fog leaves the sun's out too.
Needs a float picture (an HDR view or temporal anti-aliasing), a perspective camera and SDL_GPU; elsewhere the
in-scattering stays unshadowed. About 0.45 ms at 2404 x 1352 on the RTX 4080 Laptop (the walk 0.25 to 0.3, the
adding 0.15). Light shafts where shadows cross the air come with it. `debug_stats().fog_sun_shadowed`; authored as
`SceneEnvironment3D`'s `fog_sun_shadows`.

**Lights in the fog** (`Fog3D::volumetric`, `VolumetricFog3D`, off by default): the light of the point and spot lights
that the fog's air scatters toward the camera: a lamp's glow in the mist, a torch's beam, cut by the lights' shadow maps
where they have them (a beam through a doorway, the bars of a window). The view's near `distance` (40 m) is cut into a
volume of cells, tiles of `tile` pixels by `slices` (64) of its depth, the slice edges at `distance * (i / slices)^2`
(thin near the camera, where a cell spans the most of the picture). `tile` 0 (the default) is the view's height over 90,
at least 8: cells of one angle at any size (8 pixels at 720p, 16 at 1440p, 24 at 2160p), about 0.9 million of them at
16:9, so the cost and the memory (three volumes of 8 bytes a cell, 22 MB) stay those of 720p. At 2560 x 1440 in A Lovely
Night of Terror its pictures differ from 8-pixel tiles' by a level or two of 255 (at most 4 for 99.9% of the pixels, near
a torch held at the camera). `fog_volume_depth.comp` first finds each column's farthest surface (the pre-pass's depth
over its tile, then the farthest of the 3 x 3 tiles round it, which the adding pass's bilinear reads reach), and
`fog_volume.comp` leaves the cells behind it unlit (a whole block at once where none of its cells is in front): no ray
reaches them; a mark keeps the next frame from taking them for history. The picture is the same as lighting them all
(`RenderDebugOptions3D::fog_volume_cull = false`); the integration stops there too. `fog_volume.comp` lights each cell at one point in it,
moved within the cell per cell and per frame and blended with the last frame's volume where the cell was then (`history`,
0.9 of it kept; without a history, the cells' middles): every light the frame chose whose sphere meets the cell's block
(the block's threads gather them into a list of up to 128 first), by the stock shader's falloff (the windowed inverse
square, no brighter than at the light's `size`, a spot's cone), through its shadow map (one filtered comparison; the
frames average the rest), times a Henyey-Greenstein lobe of `anisotropy` (0.3) over 4 pi. The air is the fog's own
(`density` from `start`, `height_density`), and scatters all it takes out, times `intensity` and each light's own
`volumetric` (`PointLightSource3D`, `SpotLightSource3D` and their ECS components; 0 leaves a light out of the fog).
`fog_volume_integrate.comp` sums each column front to back (each slice's light over its length in closed form, times the
share that gets through the air before it, held at no less than `1 - max_opacity`), and after the main pass
`fog_volume_apply.frag` adds the sum at the depth the pre-pass (which it turns on) drew at each pixel (the whole
volume's for the sky). The main pass's fog dims what is behind the air by the same density, so the two agree. A light without a shadow map
lights the air on both sides of a wall, as it lights surfaces: give the lights that matter shadows (up to 8 point lights' cubes
and 8 spots' maps a frame), or `volumetric = 0`. With `sun_and_moon` (on) the sun and the fill light (the moon) scatter in the
volume too, through their shadows (the sun's cascades, the fill's `DirectionalShadow3D`), at the main pass's fog's strength
(`sun_scatter` times a lobe of `sun_anisotropy`: none at a `sun_scatter` of 0): shafts through windows and between trees. The main
pass, the sky and the particles then leave the sun out of their fog; the fog past the volume takes it unshadowed in the adding
pass (its share: the ray's opacity less the volume's), and `sun_shadows`' walk is not made. The fill light, which the main pass's
fog has not, scatters within the volume only. The light is added inside the main pass, after the opaque meshes and the sky and
before the blended ones, the particles and the overlay: a blended surface dims the air's light behind it as it dims the surface
behind it (dark glass at 90% leaves a tenth of a lamp's glow), and adds the air's in front of it for its own share: the stock
fog variants read the volume's sums (`uFogVolume`, their last sampler, after a surface's graph maps) at the fragment's depth
once the light is in (the fog block's `view.w`), so a greenhouse in a lamp-lit mist is as misted as the lawn before it.
Particles, custom material shaders and a surface shader's light variants do not (SDL_GPU's 16 samplers a shader are the
stock light layout's and the graph maps there); a surface shader made before this has no such sampler, and adds none. Needs a float picture (an HDR view or temporal
anti-aliasing), a perspective camera and SDL_GPU. `debug_stats().fog_volume`, `fog_volume_lights`, `fog_volume_cells`;
authored as `SceneEnvironment3D`'s `fog_volumetric`, `fog_volumetric_intensity`, `_anisotropy`, `_distance`, `_tile`,
`_slices`, `_history`, `_sun_and_moon`.

**Time of day** (`TimeOfDay3D`): `hours` (solar time), `day_of_year`, `latitude`, `axial_tilt`, `moon_phase`, `speed` (game seconds a
second for `advance(day, dt)`; the clock is summed exactly behind the f32 `hours`, so an hour of frames is an hour at any frame rate,
and setting `hours` yourself is noticed and continued from). `evaluate()` is a pure function of them (and the sky's haze and sun intensity): sun and moon
directions, the sun's colour, the moonlight, ambient and ground ambient (the sky's cosine-weighted light), a fog colour and density
suggestion; `apply()` writes them into the lights, the sky's moon and the fog's colour (not the haze, the stars or the density, which are
looks); `sun_times()` gives sunrise and sunset. North is -z, east +x. As a component: put a `TimeOfDay3D` on an entity and call
`WorldRenderState3D::advance_time_of_day(dt)` and `apply_time_of_day(scene)`.

Set `TimeOfDay3D::compute_ambient=false` when the renderer supplies sky lighting
and the CPU hemisphere integrals are unused. Sun/moon motion, direct light,
daylight and fog are still evaluated every call. The returned ambient values
are zero and `apply()` retains the caller's ambient lights. The default is true,
preserving existing callers. Browser Summit uses this fast path.

**The sky's light** (`Lights3D::sky_lights_scene`): the atmosphere and ground (no disc, stars or moon) are drawn into a 256 x 128 panorama,
mipped and filtered with the environment machinery, and take the place of `environment` and `ambient`. It is made again only when
the sun or the sky moved by more than `Sky3D::light_threshold` and `light_interval` renders (4) have passed, so a day sweep re-filters a few
times a second (`debug_stats().sky_env_regenerated`, `sky_env_generations`, `sky_env_cpu_ms`; about 0.4 ms of GPU each time). It lights the scene
whether or not the sky is drawn. Without it the hemisphere ambient is used as before.

`warm_up()` makes the sky's, the fog variants' and the dither's pipelines too. Harness: `kin_reference` JSON `sky`, `fog`,
`time_of_day`, `lights.sky_lights_scene`, a camera's `hours`, `at_sun`, `at_moon`; `procedural: "landscape"`; scenes `sky_day`, `sky_dusk`,
`sky_night`, `fog_valley`, `tod_sweep`. Sponza Walk's "Sky and fog (P3a)" section and `--preset sky|dusk|fog|night|day-night`.

### Spot lights and shadows from lights that are not the sun

Off by default; off, the picture and the cost are those without them (the old shaders are not touched: the stock fragment shader has four
more variants, used only when a scene has a spot light, a local shadow or a moon shadow). `kin/render3d/scene3d.hpp` (`SpotLightSource3D`,
`DirectionalShadow3D`, `Lights3D::spots`), `kin/render3d/local_lights3d.hpp` (the maths); docs/3d_plan.md "P3b" has the models
and the numbers.

```cpp
kin::SpotLightSource3D torch{.position = camera.position, .direction = camera.forward(), .color = {40, 38, 30}, .radius = 30.0f,
                     .inner_angle = 8.0f, .outer_angle = 18.0f};      // degrees from the axis
torch.shadow_enabled = true; torch.shadow_resolution = 1024;                  // a shadow map, if it is among the most important
scene.add_spot_light(torch);

kin::PointLightSource3D lamp{.position = {0, 3.5f, 0}, .color = {30, 27, 22}, .radius = 20.0f};
lamp.shadow_enabled = true;                                            // a cube of six maps (the faces the view can use)
scene.add_point_light(lamp);

scene.lights.max_shadowed_spots = 4;       // up to 8; of the lights that ask, the most important this many a frame get a map
scene.lights.max_shadowed_points = 2;      // up to 8 cubes
scene.lights.fill_shadow = {.enabled = true, .cascades = 2};           // the fill light (the moon) casts: its own cascades, over shadow_area
```

**A spot light** is a point light inside a cone: the same photometry (`color` is what a white surface in the middle of the cone shows
one unit away), `radius` and `size`, and a smooth falloff between `inner_angle` (full light) and `outer_angle` (none), by glTF's punctual
spot curve: `t = clamp((cos a - cos outer) / (cos inner - cos outer), 0, 1)`, light times `t * t`. Exactly zero from the outer angle
on, exactly full inside the inner one. Up to 256 that reach the view light a frame through the light grid, else 16 (the cone's
bounding sphere in view, the strongest and nearest first, as the point lights; `RendererCapabilities3D::max_spot_lights`).
`kin::spot_attenuation(light, point)` is the same number on the CPU.

**The light grid.** Where the light shaders are made (`capabilities().local_shadows`), the main pass's light variants shade each
fragment with the point and spot lights whose reach touches its cell of the view only, not with every light: the view is cut into
tiles of 64 pixels by 32 slices of the depth along the camera's forward (each slice farther than the last by the same ratio, from 0.1
to where the farthest light's reach ends), and each frame the CPU lists, for each cell, the lights whose bounding sphere's box covers
it, in the order the lights were chosen (so the light adds up as a loop over them all would). The lights and the lists go to two
storage buffers the light variants read. With more than 16 point lights in view the light variant draws (the plain shaders keep their
loop over 16); the overlay, particles, custom material shaders and WebGL2 keep the fixed blocks of the 16 strongest of each kind.
`Scene3D::debug.light_grid = false` turns it off (every fragment looping over the 16 strongest of each, as before it): the same lights
give the same picture either way. In Bistro's night (2404 x 1352, 40 to 60 lights in view) the grid took 1.6 ms off the GPU's frame
at Normal and 3.4 ms at High, drawing every light where the loops drew 16 of each.

**Shadows.** A light's `shadow.enabled` asks for a map; of the spots in view that ask, the `max_shadowed_spots` most important have one,
and of the point lights the `max_shadowed_points`. A map is a perspective depth map over the cone (a point light's: six 90-degree faces
with a guard band, only those a visible surface can fall in drawn), in one array whose side is the largest `resolution` of the lights
given a map this render (default `Lights3D::local_shadow_resolution`, 1024); it grows at once and shrinks only after a smaller side has
done for 120 renders in a row. Each map is rounded up to a power of two and packed: the largest first, each layer filled in Z order by
squares of the map's side, so a lamp's 256-texel cube takes six sixteenths of a 1024 layer (six lamps and a torch: four layers, 16 MB,
not 37). A packed map is cleared alone (a depth-only triangle over its square) and kept by the shadow cache like any other. The shader
block stays under SDL_GPU's 4 KiB a uniform block: a spot's map keeps its matrix, a cube's six faces are derived from the light's
position (`kin_local_clip`), and each map has its place in the array (`local_c`). Importance is
intensity times reach times the share of the view the light fills, and a light that had a map last frame counts 1.5 times over, so the
choice does not flip between frames; name a light with `id` (else its index in the list) to keep that across frames. Filtering is
3x3 comparisons as the sun's, `bias` (texels, times 1 + the surface's slope to the light) and `normal_offset` (texels) in the light's
own texel at the surface's distance, `softness` the taps' spacing, `near_plane` the map's, and `distance` the reach (the shadow fades
out over its last fifth and the map ends there). The offset and the slope are the mesh's own normal's, as the sun's (and the moon's
offset): a normal map changes the light, not where a shadow falls.
Casters are what the cone and the radius reach: opaque and cut-out meshes, instanced
and posed (skinned) copies, levels of detail by the camera's choice, and clustered meshes cut from the light's own point of view (the
error is judged in the map's texels). `Material3D::receives_shadow` and `casts_shadow` govern these shadows too; custom material shaders
get neither the spots nor the shadows; the overlay gets the lights without shadows.

**The moon.** `Lights3D::fill_shadow` makes the fill light cast through its own cascades (1 to 4, `Lights3D::shadow_area` and
`shadow_distance` as the sun's, `resolution` of its own), drawn from the same casters. `strength` (0 to 1) lightens it. With
`TimeOfDay3D::shadows = true`, `apply()` sets `fill_shadow.enabled` and `strength` from the moon's height (fading in over its first 8
degrees, with its light) and `Lights3D::sun_shadow_strength` from the sun's (0 under 2 degrees below the horizon, 1 above 1 degree above
it): a strength under 1 uses the light shaders, 0 draws no sun map. At dusk both cast: up to the sun's cascades plus the moon's.

**Kept while nothing moves.** `lights.cache_shadows` keeps each local map as it is cached: the exact bytes the draw is a function of (the
view's matrix and size, each caster's mesh, material, copies and pose, the cut's settings) are compared, so a static spot over static
casters costs nothing after its first render, a moved light or caster redraws only its own map, and the camera is not part of it for a spot
or a point light (it is for the moon's cascades). `debug_stats()` has `spots`, `spot_slot[i]` / `point_slot[i]` (the slot of `Lights3D::spots[i]`
and `points[i]`, -1 none), `local_shadow_maps`, `local_shadow_drawn`, `local_shadow_cached`, `local_shadow_draws`, `local_shadow_casters`,
`moon_cascades`, `local_shadow_bytes` and `lights_variant`.

Harness: `kin_reference` JSON `lights.spots` (`attach` carries one with the camera), `lights.points[].shadow_enabled`, `lights.fill_shadow`,
`time_of_day.shadows`, `procedural: "stage"`, options `--spots`, `--spot-shadows`, `--point-lights`, `--point-shadows`, `--moon-shadow`, `--local-res`;
scenes `spot_shadows`, `spot_night`, `spot_budget`, `moon_shadow`, `point_shadow`. Sponza Walk's "Spot lights and shadows" section and `--preset
spots|flashlight|moon|lamps`. `warm_up()` makes the light variants' pipelines and the maps the configured lights will want.

Memory: a layer is `side`^2 x 4 bytes (a spot 4 MiB at 1024, 1 MiB at 512), a point light six layers; the array holds the layers asked for
and gives them back after 120 idle renders. Not done: shadows from custom material shaders' own light, contact shadows for spots, a light
cone's in-scattering in fog (a lamp in mist shows no beam), soft shadows by light size (the filter is a fixed width).

### Decals

`Scene3D::draw_decal(Decal3D)` projects a picture onto the opaque surfaces inside a box: scorch marks, paint,
dirt, footprints, posters, bullet holes (in the ECS: `DecalRenderer3D`, below). The box is the unit cube `[-0.5, 0.5]^3` placed by `transform`; the picture
lies on its X/Z sides (u along +X, v along +Z, the picture's top at -Z) and is projected along -Y, from the box's top
face down through its depth. Decals are drawn after the scene's opaque meshes and before the sky and the blended
meshes, in the order of their `order` (lower first, so higher lies over lower where they overlap; 0 by default), and
lit as a custom material is: the sun and its shadows, the fill light, ambient or
environment light with the ambient occlusion, point and spot lights (the first 16, as custom shaders have) and their
shadows, and fog. `clear()` takes them away.

```cpp
const kin::Texture scorch = renderer.create_texture_from_rgba(pixels, 256, 256); // colour and coverage (alpha)
scene.draw_decal({.transform = kin::Mat4f::trs({x, 0, z}, kin::Quatf::axis_angle({0, 1, 0}, 30), {2, 1, 2}),
                  .texture = scorch});
// Onto a wall facing +Z: the box's +Y turned toward +Z; a normal map (tangent space, green up) for relief.
scene.draw_decal({.transform = kin::Mat4f::trs({x, y, wall_z}, kin::Quatf::axis_angle({1, 0, 0}, 90), {0.2f, 0.3f, 0.2f}),
                  .texture = hole, .normal = hole_normals});
```

`color` multiplies the texture (alpha too); `roughness`, `metallic` and `emission` (the colour as light of its own)
describe the paint; `normal_strength` scales the normal map. Surfaces turned away from the projection fade: fully
drawn where the cosine between their normal and the box's +Y is above `normal_fade` (0.3), to nothing at 0, never
onto a surface facing away, so a floor decal does not smear down the walls. `depth_fade` (0.25) fades it toward the
box's top and bottom faces. `wrap` is the picture's sampler (clamped by default).

`Material3D::receives_decals` (true; `"receives_decals"` in `.kinmaterials`) turned off keeps decals off a mesh: such
meshes are drawn after the decals, over them, so a character walking through a scorch mark's box is not painted
with it (the decal's work under its pixels is simply covered).

In the ECS a `DecalRenderer3D` places one at its entity: the box is the entity's space scaled by `size` (the picture's
width along X, the depth it reaches along Y, its height along Z), projected along the entity's -Y. Scene files and
prefabs author it, its images by path (`resolve_render3d_assets` binds them: the texture as colour, the normal map as
data; a failed load keeps the pictures bound before, and the session reloads them when the files change):

```json
{"id": "scorch", "components": {
    "Transform3D": {"position": [2, 0, -3], "rotation": [0, 0.26, 0, 0.97]},
    "DecalRenderer3D": {"texture": "decals/scorch.png", "size": [2, 1, 2], "normal_fade": 0.4, "order": 0}}}
```

A decal reads the pre-pass's depth and normals (the mesh's normal, before its own normal map), so a scene with decals
always runs the pre-pass (with TAA, the main pass then no longer writes the velocity itself). It lands on what the
pre-pass draws: opaque and cut-out meshes with stock or surface shaders, not custom-shader, blended or unlit meshes,
particles or the overlay; opaque meshes whose material has `receives_decals` off are drawn over them. Seen from outside its box, a decal's front faces are depth-tested against the scene, so
what stands in front of the box hides it; from inside the box (or within twice the near plane of it) its back faces
are drawn untested. Decals whose box is out of the view are skipped (`debug_stats().decals_culled`).

The others are batched: their placings and settings go to the GPU in one buffer a frame, and the decals of one
`order` that share their pictures (texture, normal map, wrap) are drawn together, as one instanced draw of 36
vertices a box (`debug_stats().decals_drawn`, `decal_draws`), whatever order they were added in. Pictures are drawn in
the order each first appears among its `order`'s decals, so when two decals of different pictures overlap and one
must lie over the other, give it a higher `order` (a decal of a picture already drawn earlier in its order would
otherwise join that picture's draw, under the other). 256 decals of one picture are one draw (`render()` 0.18 ms of
CPU against 0.26 drawing them one by one; `kin_render3d_surface_bench`), 256 of four pictures four draws. A pixel
costs four extra depth reads (the texture's gradients come from the neighbours on the same surface, so edges of what stands in
front do not pick the smallest mip level). `capabilities().decals` says whether the device can; where not (or on
WebGL2), decals are not drawn, said once. Not supported: `render_surface`/`prepare` graphs, and clustered
(many-decal) culling.

### Particles and weather

Off by default; off, the picture and the cost are those without them. Particles have their own shaders;
the pre-pass supplies depth for soft particles and velocity under TAA. `kin/render3d/particles3d.hpp`; docs/3d_plan.md "P3c" has the models
and the numbers.

```cpp
kin::ParticleSystem3D sparks{7};                          // seeded: the same seed gives the same sparks on every machine
sparks.desc.shape = kin::ParticleShape3D::Cone;
sparks.desc.rate = 800.0f;  sparks.desc.speed = {3.0f, 7.0f};  sparks.desc.lifetime = {0.6f, 1.6f};
sparks.desc.gravity = {0, -9.8f, 0};  sparks.desc.drag = 0.4f;
sparks.desc.look = {.blend = kin::ParticleBlend3D::Additive, .facing = kin::ParticleFacing3D::Stretched, .stretch = 0.05f};
sparks.transform = kin::Mat4f::translation({0, 0.2f, -4});
sparks.update(dt, &scene.weather.wind);                  // fixed steps of 1/120 s inside: frame-rate independent, savable
scene.draw_particles(sparks);                            // each frame, like a mesh

scene.weather.rain.intensity = 0.7f;                     // a volume of rain round the camera: a function of (time, camera)
scene.weather.wind = {.direction = {1, 0, 0.3f}, .speed = 5.0f, .gust_strength = 0.5f, .turbulence = 0.6f};
scene.weather.has_ground = true;  scene.weather.ground_height = 0.0f;
scene.weather.time += dt;
scene.weather.apply(scene.lights, scene.fog, base_fog);  // dims the sun and ambient light, thickens the fog (after TimeOfDay's apply)
```

**Systems** (`ParticleSystem3D`, `ParticleDesc3D`): emitters of a point, box, sphere, cone or disc (in the emitter's own space: its
`transform` places and turns it; the axis is +Y), a `rate` and `burst()`s, lifetimes, speeds along the emission direction and extra
velocity vectors drawn between two, gravity, drag toward the air's velocity (the wind times `wind_influence`: the step is the exact
solution for constant air, so any drag is stable), size, colour and alpha over life, rotation and spin, a per-particle random and a
flipbook over the life. The simulation runs in fixed steps of 1/120 s taken out of the time `update(dt)` is given, with every random a hash of
(seed, birth number, purpose): the same seed and the same calls make the same particles whatever the frame lengths were, `state_hash()` is
a hash of all of it. For rollback/resimulation, `save()` and `restore(saved, ParticleRestore3D::Rollback)` preserve particle identity and presentation
continuity. Plain `restore(saved)` treats a changed state as a new presentation epoch. A ground height (visual only: no
physics) kills or bounces. The wind is read at every particle's own place each step, 32 at a time: the gust and turbulence sines run in f32
turns by a polynomial (arithmetic only, so the same bits on every machine; within 1e-5 m/s of `Wind3D::velocity` near the origin, the
error growing with the distance from it as the f32 turn counts coarsen: about 1e-3 of the turbulence speed at 10 km; the CPU test
asserts 2e-3 m/s over a 36 m box 100 m out) in loops the compiler
makes vector code of, the drag's `exp` is taken once a step, and a system without drag, influence or wind never looks for the air (20,000
particles in gusts and turbulence: 0.6 ms an `update(1/60)`, from 16 ms; `kin_render3d_bench wind`). ECS: `ParticleEmitter3D` holds a system and is stepped at its entity's place by
`WorldRenderState3D::simulate_particles(dt, wind)` and drawn by `collect()`; a `Weather3D` component is the game's weather
(`advance_weather(dt)`, `apply_weather(scene, base_fog)`).

**Drawing** (`ParticleLook3D`, `Scene3D::draw_particles`): one instanced draw a batch of sprites (a system's, or the weather's layers),
tested against the depth of the opaque meshes and never written, interleaved with transparent meshes before the overlay (the overlay's
and a Scene3D::overlay's particles are not drawn). Facings: `Camera` (turned by `rotation`), `Stretched` (along the velocity, `stretch`
seconds of motion long, head at the particle, for rain and sparks), `Axis` (turns about an axis to face the camera), `Flat` (in the plane
normal to the axis, a ring on the ground). A texture, an atlas of columns by rows (the sprite's `frame`, mixed with the next with
`blend_frames`), or a procedural soft disc (`softness`, `ring`). Blend `Alpha`, `Additive` (adds light; dimmed by fog, not mixed) or
`Premultiplied` (a texture that holds colour times alpha: alpha 0 with colour adds, inside one sprite). `sort` (default true): sprites and alpha/additive mesh copies compose together by camera-space centre depth, with
stable ties. Only consecutive copies of one compatible source and contiguous packed slots share a draw. This preserves
cross-batch interleaving and dims additive light behind alpha foregrounds. `sort=false` explicitly keeps that particle
source in one run at its mean depth. Centre sorting is approximate for intersecting geometry; it does not split polygons.
`debug_stats().transparent_draw_runs`, `transparent_sort_ms` (CPU scheduling, not GPU time) and `composition_order_bytes`
show the cost. Overlay meshes use their own scene/camera order. Native sprites are supported; browser particle support
remains unavailable.

**Soft particles** (`soft`, `soft_distance`): the fragment shader reads the view's depth, the opaque meshes' pre-pass depth (one sample a
pixel, a depth texture of its own: under MSAA the particle is shaded once a pixel and the depth read at its centre), and fades the sprite by
a smoothstep of how far it is in front of the surface behind it: 0 at the surface, 1 `soft_distance` or more in front (`kin::soft_particle_fade`
is the same number). It costs the pre-pass (a draw of the opaque meshes' depth and normal) where nothing else asks for one, and under TAA the
velocity then comes from it too; a scene without a soft batch makes none. The surface must be opaque: a blended mesh is not in the pre-pass.

**Light and fog.** The fog is `kin_fog.inc.glsl`'s, evaluated in the vertex shader at each corner and interpolated (the particle is small,
the closed form smooth), so a particle takes the fog a mesh at the same place takes, to a level of 255 (tested); `fogged = false` leaves
it out. `lit` (0 to 1) tints the particle by ambient (sky and ground averaged), half the sun and fill
(`kin::particle_light_tint(lights)`), plus half the point/spot irradiance at the sprite centre.
Local lights use the same radius, source size, spot cone and selection budgets as meshes;
the isotropic approximation has no surface normal. The renderer selects a separate shader
only for batches with `lit > 0` and selected local lights. Zero shows the particle's own
colour (fire, sparks, glows). With `TimeOfDay3D::apply`, snow is not white at night.
Set `look.receives_shadow = true` on a lit batch to receive sun/cascade, moon,
point and spot shadow maps. The default is false; `lit = 0` ignores the option.
Shadow reception attenuates only direct illumination: ambient light, the
unlit share and fog retain their existing behavior. Irradiance remains the
isotropic centre-lighting approximation, while visibility is sampled at each
fragment's world position, so a shadow can cross a large sprite. Bias, cascade
transitions, directional strengths and local shadow distance fading use the
same sampling helpers and settings as mesh materials. Configure the light's
normal shadow settings as well (`shadow_area` for sun/moon;
`light.shadow_enabled` for point/spot); a request without active maps uses the
ordinary particle path.

```cpp
sparks.desc.look.lit = 1.0f;
sparks.desc.look.receives_shadow = true;
scene.weather.snow.look.receives_shadow = true;
```

Set `look.casts_shadow = true` independently to cast into those maps, even
with `lit = 0`. The caster uses the same camera-facing, axis, flat or stretched
quad, rotation, texture alpha, procedural disc/ring and interpolated flipbook
frame as the visible sprite. `look.shadow_cutoff` (default `0.5`, range `[0,1]`)
alpha-tests intrinsic opacity: texture/procedural alpha times particle alpha.
Zero opacity never casts. Fog and soft-particle intersection fading affect the
visible sprite only. Shadows are binary silhouettes filtered by the light's
normal PCF; translucent or coloured volume transmission is not simulated.

```cpp
sparks.desc.look.casts_shadow = true;
sparks.desc.look.shadow_cutoff = 0.3f;
scene.weather.snow.look.casts_shadow = true;
```

`capabilities().particle_shadows` reports casting and reception availability.
Reception adds fragment shadow reads without extra targets or caster draws;
casting adds draws to the configured maps. Disabled/unlit reception keeps its
existing shaders. Warm-up includes both options. The shadow caches track
moving/fading particles, texture revisions, look edits and removal, including
casters outside the colour frustum. TAA invalidation includes shadow casters
when shading particle receivers; still shadow edges keep temporal smoothing.
Soft particles, flipbooks, fog, HDR, MSAA and all particle blend modes share
the same visible output and coverage paths.

Casting has a separate stream capped at 1,048,576 sprites per render, independent
of the colour stream's culling and limit. A batch outside a light's frustum costs
no caster draw. Its GPU buffer is allocated on demand and released after 120
idle renders. `debug_stats()` exposes `particle_shadow_batches` (batches receiving
maps), `particle_shadow_submitted` / `particle_shadow_dropped` (unique uploaded
casters), `particle_shadow_draws` / `particle_shadow_instances` (including repeats
per light/cascade), and `particle_shadow_bytes` (allocated caster GPU buffer).

**Weather** (`Weather3D`, four `WeatherLayer3D`: rain, snow, dust, mist). A layer is `max_particles` in a box `box` round the camera of
which the first `intensity` share are shown (one at a time: smooth); each one's place is a formula of its number and `weather.time`
(fall speed, the wind's drift, a flutter), wrapped into the box in **world space**, so that it does not move when the camera does and any
number costs no state. Rain is stretched streaks with the wind's slant and splashes (flat rings, `splash` share of the drops) where it
meets `ground_height`; snow soft flakes lit by the scene; mist large soft wisps. A particle's fade at the box's sides and top keeps a
wrapping one from popping, and one fades in over its first two metres from the eye (a flake at the lens is a blob across the picture).
The renderer makes only the particles in the view (the sprites are kept while the weather, the camera and the frustum are unchanged: a view
drawn twice, a photo mode). `Weather3D::apply` dims the sun, fill and ambient light by `wetness()` and sets the fog to your base fog
thickened and greyed (rain at 1: the sun at 40%, ambient 70%, 0.004 more density); call it after the time of day, each frame, from the
base fog (the fog is overwritten, so it does not compound), and scale your sun disc by `sun_disc_scale()`. `Wind3D` (a direction, a speed,
three gust sines, three turbulence sines per component) has `velocity()` for simulated particles and a closed-form `displacement()` for the
weather: the same wind.

`debug_stats()` has `particles_submitted`, `particles_drawn` (after the view's cull and the 1,048,576 sprites a frame it takes), `particle_batches`,
`particle_soft`, `weather_particles`, `particles_dropped` and `particle_cpu_ms`. `capabilities().particles` and `soft_particles`. Not done:
depth-hit splashes (the ground is one height), collisions with anything,
GPU-simulated particles (the CPU writes the instance buffer each frame), a sort across
batches, and rain that stops under a roof (it falls through Sponza's nave).
Moving particles and weather use the temporal reprojection described above.
Native emitters supply stable birth IDs; immediate callers can supply sprite IDs
and an emitter history ID, or retain stable submission order.

Harness: `kin_reference` JSON `weather` and `particles`, options `--rain`, `--snow`, `--wind`, `--no-weather`, `--no-particles`, `--soft`,
`--particles N`; scenes `rain_street`, `snow_valley`, `particles_soft`, `particles_blend`, `particles_budget`. Sponza Walk's "Particles and
weather (P3c)" section and `--preset rain|snow|storm|fire`.

## Custom shaders

A material can have a fragment shader of its own (`Material3D::shader`,
`shader_params`: its first four vec4s), in place of kin's, for a hologram, a force
field, water. It is GLSL for Vulkan, compiled against kin's include, which
declares the inputs and uniforms and has the lighting in it:

```glsl
#version 450 core
#include "kin_material.inc.glsl"   // from engine/shaders (glslc -I)

void main() {
    vec4 base = kin_base_color();                 // linear colour, alpha
    float rim = pow(1.0 - abs(dot(kin_normal(), kin_view())), 3.0);
    FragColor = kin_output(base.rgb * rim * kin_param(0).x, rim * base.a);
}
```

What it provides: `kin_base_color()` (vertex, instance and material colours
times the base texture), `kin_normal()` (unit, on the viewer's side, bent by
the normal map), `kin_view()`, `kin_position()`, `kin_uv()`,
`kin_metallic_roughness()`, `kin_occlusion()`, `kin_emissive(base)`,
`kin_lit(base, normal, metallic, roughness, occlusion)` (kin's lighting: sun
with its shadow, fill/moon, point and spot lights with local shadows, ambient or environment; call it for a
lit surface, skip it for an unlit one), `kin_output(colour, alpha)` (encoded
for the target, premultiplied, with scene fog and material alpha cutoff: write `FragColor` with it),
`kin_output_unfogged(colour, alpha)` (the same output contract with fog bypassed),
`kin_param(i)`, and the raw `frame`, `material` uniforms and samplers for anything else.
Samplers 0 to 11 and fragment uniform buffers 0 to 3 are kin's. Slot 2 starts
with the four parameter vec4s and then holds fog data; slot 3 holds local lights.
Each block stays below SDL_GPU's 4 KiB limit. Recompile custom shaders against
the current include to get fog, spot/moon lighting and local shadows. Existing
compiled shaders retain the parameter prefix and their original shading behavior.
The blend mode, depth and culling are the material's, as ever; the shadow and
pick passes draw the material as if it had no shader.

`r3d.create_material_shader(spirv)` makes one from SPIR-V (build it with
`kin_compile_glsl`, which adds the include path; `cmake/kin_spirv.cmake` does
too, and skips `*.inc.glsl`; see `examples/CMakeLists.txt`);
`r3d.compile_material_shader(path)` compiles GLSL at run time with glslc, for
development. Resource layouts are reflected and validated; automatic 3D shader hot reload is not provided.
Signal Breach's hit shields (`examples/shaders/shield.frag.glsl`: Fresnel rim
and climbing lines, additive) are one. For a custom HDR pipeline, use explicit surface attachments and the public bloom/output operations below.

### Surface shaders

A custom shader replaces kin's whole fragment shader, and with it what kin's own shaders have and it does not: the
global illumination, the light grid (it keeps the 16 strongest point and spot lights), the fog and light variants
and the velocity for temporal anti-aliasing. A surface shader (`Material3D::surface`) keeps all of that: it is one
function, compiled into each of kin's mesh fragment shaders, that changes the surface kin shades. The material
graph (docs/material_graph_plan.md) is built on it.

```glsl
// rust.surface.glsl: no #version, no main
void kin_surface(inout KinSurface s) {
    const float rust = smoothstep(0.4, 0.6, fract(sin(dot(floor(kin_position().xz * 8.0), vec2(12.9, 78.2))) * 43758.5));
    s.base_color = mix(s.base_color, kin_surface_param(0).rgb, rust);
    s.roughness = mix(s.roughness, 0.9, rust);
    s.metallic *= 1.0 - rust;
}
```

`s` arrives as kin's material makes it (`kin_stock_surface()`: the colours times the base map, the normal bent by
the normal map, metallic and roughness times their map, the occlusion map, the emission), so a function that
changes nothing draws exactly what the material draws without one. Its fields: `base_color` (linear), `alpha`
(straight: a `Blend3D::Mask` discards below the cutoff), `normal` (unit, world space, on the viewer's side),
`metallic`, `roughness` (keep it within [0.045, 1]), `occlusion`, `emission` (linear light of its own) and `backlight`
(0 as kin's material makes it: the colour a thin surface, a leaf, lets light through in from behind, as Unreal's
two-sided foliage, Godot's backlight: each light on its back side, wrapped by half, scattered forward by a GGX lobe
of alpha 0.6, and the ambient light from behind; a backlit surface is lit, and its shadows read, from either side),
`clearcoat` and `clearcoat_roughness` (0 and 0.045 as kin's material makes them: a clear coat over the surface, as
glTF's KHR_materials_clearcoat: a dielectric of IOR 1.5 with a GGX highlight of its own roughness, its reflection of the
environment too, the layer below dimmed by the coat's Fresnel), `specular` (0.5: a dielectric's reflectance head on is
0.08 times it, 4% by default, as Unreal's Specular; 0: no highlight, no reflection) and `diffuse_wrap` (0: Lambert's
diffuse; 0.5: the light's first two spherical harmonic bands, 1/4 + n.l / 2, lighting the side facing away a little, as
Unreal's directional volume lighting of translucency; 1: the first alone, 1/4 from every side, the ambient light the mean
of both sides'), and `reflection` (0 as kin's material makes it: rgb a reflection of its own in linear light, a how much
of the surface's reflection it is, in place of the environment's or the probes': a mirror's picture, Mirrors and
portals). After the function, kin lights it
(`kin_lit_surface(s)`; `kin_lit(base, n, metallic, roughness, occlusion)` for a surface without either) and adds the emission, or, for an `unlit` material, shows its colour plus
its emission; then fog and output as ever. The function may call every `kin_*` function of the custom shaders
(`kin_uv()`, `kin_position()`, `kin_view()`, `kin_normal()`, the material's maps through `uBase`, `uMetalRough`,
`uNormalMap`, `uOcclusion`, `uEmissive`), and reads `Material3D::shader_params` (`surface_param_count3d`: 64 floats) as `kin_surface_param(0..15)`.

`r3d.compile_surface_shader(path)` compiles the file now with glslc (development); a game ships its variants
compiled at build time with `kin_compile_surface_shader(<var> OUTPUT_DIR <dir> SOURCE <name>.surface.glsl)`
(`cmake/kin_embed.cmake`), which lists the twelve `.spv` files in `SurfaceVariant3D`'s order for
`r3d.create_surface_shader(variants)`. `surface_shader_source()` and `surface_variant_defines()` say what each
variant is. A `shader` takes the surface's place. WebGL2 has no surface shaders.

**Cut-outs.** A `Blend3D::Mask` surface cuts its shadow, its occlusion (the pre-passes), its picks and the visibility
queries by its own alpha where it defines `float kin_surface_alpha(vec2 uv, vec3 position, float vertex_alpha)` under
`#ifdef KIN_SURFACE_MASK` (the UV as the maps read it, the world position, the copy's and vertex's alpha; it may
sample the material's five maps, `kin_surface_param` and the noise of `kin_graph_noise.inc.glsl`;
`material.cutoff.z` is `Scene3D::time`): five more variants (`SurfaceVariant3D::ShadowMask` ...
`VisibilityMask`, made when the text has it; `CUTOUTS` for `kin_compile_surface_shader`). Without it those passes cut
by the base map's alpha, as kin's own materials do. Material graphs write it for their `alpha` output when it uses
only what those passes have (see Material graphs).

Surface materials take kin's batched paths as its own do: a static batch's copies are retained, draws are planned and
cached, and separate draws of one mesh and surface merge into one (the merged draws' four variants, mesh_material's,
read each copy's `shader_params` from a table of their own beside the draws' material records, made only for a scene
with surface draws). Twelve variants in all; the last four are optional (a surface without them draws unmerged).

### Material graphs

A material graph (`kin.material-graph/1`, JSON; `kin/render3d/material_graph3d.hpp`) wires nodes into what a surface
is, as Godot's visual shaders and Unreal's material editor do, and compiles into a surface shader (above): lit,
shadowed, fogged and GI-lit as kin's own materials are.

```json
{"schema": "kin.material-graph/1", "name": "Rust",
 "parameters": [{"name": "tint", "type": "color", "default": [0.45, 0.2, 0.08, 1]},
                {"name": "scale", "type": "float", "default": 4},
                {"name": "albedo", "type": "texture", "slot": "base"}],
 "nodes": [{"id": "map", "type": "texture", "texture": "albedo"},
           {"id": "scaled", "type": "multiply", "inputs": {"a": "position", "b": "scale"}},
           {"id": "rust", "type": "fbm", "inputs": {"p": "scaled"}},
           {"id": "amount", "type": "smoothstep", "inputs": {"edge0": 0.45, "edge1": 0.6, "x": "rust"}},
           {"id": "color", "type": "mix", "inputs": {"a": "map.rgb", "b": "tint.rgb", "t": "amount"}},
           {"id": "rough", "type": "mix", "inputs": {"a": "surface.roughness", "b": 0.95, "t": "amount"}}],
 "outputs": {"base_color": "color", "roughness": "rough"}}
```

- **Inputs** are references or values. A reference names a node (`"map"`), a component of one (`"map.rgb"`,
  `"map.a"`: swizzles of xyzw or rgba), a parameter, or a built-in input: `uv` (with the material's UV transform),
  `position` (world), `normal` (the mesh's, before maps), `view` (toward the camera), `vertex_color`, `time`
  (`Scene3D::time`), `screen` (the pixel, 0 to 1), `camera_distance`, and `surface.<field>`: what kin's material makes
  of the surface before the graph (`base_color`, `alpha`, `normal`, `metallic`, `roughness`, `occlusion`,
  `emission`). A value is a number or an array of 2 to 4. Types are float, vec2, vec3, vec4: a float widens to any
  vector; a vector narrows to its first components, or widens with 0 (and 1 for the fourth), as Unreal's do.
- **Outputs** (`base_color`, `alpha`, `normal` in world space, `metallic`, `roughness`, `occlusion`, `emission`)
  replace the surface's; those not given keep what the material makes. Roughness is kept within [0.045, 1], the
  normal unit, colours non-negative. `offset` (Unreal's World Position Offset) moves each vertex in the world, in
  metres: compiled again for the surface's vertex variants (`SurfaceVariant3D::VertexMesh` ...: mesh.vert,
  mesh_velocity.vert, mesh_indexed.vert, the pre-passes' and the shadows', each with `kin_surface_vertex.inc.glsl`
  before it), so the main pass, the pre-passes, the sun's and lights' shadows, pick and visibility all place the
  vertices alike, and the velocity's previous position takes the view's time of the frame before. At a vertex it reads
  `uv`, `position`, `normal` (the mesh's), `view`, `vertex_color`, `time`, `camera_distance`, the parameters and the
  maps (their top level: a vertex has no derivatives; no tangent frame, nor the environment), and the object's space:
  `object_position` (the copy's origin; a fragment has it too, flat from the mesh's vertex shader) and, which only a
  vertex has, the `object_to_world` / `world_to_object` nodes (a
  point, `w` 1, or a direction, `w` 0, through the copy's model matrix: wind swaying a plant by its height, a vertex
  animation's local offsets; the frame before's matrix for the velocity); `surface.*`, `screen`,
  `normal_map`, `tangent_to_world` and `fresnel` it does not have: such an offset is not drawn, warned
  (`CompiledMaterialGraph3D::offset` false). A surface that moves its vertices is not merged with others, and its
  shadow caches redraw as its time, parameters or maps change. Not moved: a skinned mesh's vertices, the GI scene's
  voxels; a mesh is culled by its unmoved bounds.
- **Parameters** (`float`, `vec2`, `vec3`, `vec4`, `color`: linear RGBA) take `Material3D::shader_params`' 64 floats
  (`surface_param_count3d`; enough for Unreal's master materials: 102 of 107 free Fab materials before unused ones
  are left out), vec3 and vec4 each a vec4 of their own, packed in the first place free in the order declared (a float
  in a vec3's fourth); `material_graph_params3d(graph, {{"name", value}})` fills them from the defaults and the values
  given. Textures take one of the material's map slots (`"slot"`: `base`, `metallic_roughness`, `normal`, `occlusion`,
  `emissive`: the material's `texture`, `metallic_roughness`, ... fields bind them), sampled as sRGB (`base`,
  `emissive`) or linear, or as `"color_space"` says. A texture that isn't the slot's map (`"role": "data"`: a noise in
  the occlusion slot) undoes kin's own reading of the slot (the occlusion then 1). Past the five, `"slot": "layers"`
  with `"layer"` (0 to 15, `max_graph_map_layers3d`): a layer of `Material3D::graph_maps`, one 2D array of the
  material's further maps (`Renderer3D::create_map_array`; a materials file's params build it with
  `Assets3D::load_map_array`: each map resampled to the largest size among them), read repeated, filtered and
  mipmapped, decoded in the shader when sRGB. A layer the material lacks reads white. Not in the cut-out passes: an
  alpha that reads one cuts by the base map's alpha (warned).
- **Nodes** (`material_graph_nodes3d()` lists them): `constant`; `add`, `subtract`, `multiply`, `divide`, `min`,
  `max`, `power`, `mod`, `step`, `mix` (Unreal's Lerp), `clamp`, `smoothstep`, `remap`; `abs`, `floor`, `ceil`,
  `fract`, `sqrt`, `sin`, `cos`, `tan`, `exp`, `log`, `sign`, `negate`, `one_minus`, `saturate`, `normalize`,
  `length`, `dot`, `distance`, `cross`, `reflect`, `combine`; `texture`, `normal_map` (a normal map's texel as the
  world normal, with a `strength`), `tangent_to_world`, `triplanar`, `texture_equirect` (an equirectangular map's texel
  in a `direction`: the environment's layout, seamless where u wraps); `noise_value`, `noise_gradient`,
  `noise_cellular`, `fbm`, `checker` (deterministic: hash-based, the same picture on every GPU); `fresnel`,
  `uv_panner` (with time), `uv_rotate`; and `glsl`, an expression of its own over named inputs.
- **Errors** name their node: an unknown type or input, a missing input, a cycle, a bad swizzle, a reference to
  nothing, too many parameters, two textures in a slot, nodes chained more than 1024 deep (a node's inputs, theirs,
  and so on: the compiler follows them on a stack of its own, 256 MB reserved, as a 1 MB main thread's, Windows',
  overflowed in a few hundred; 128 where no thread can be made, as in a browser). Fab's deepest graphs chain 56.
- **Cut-outs**: a graph's `alpha` is compiled again for the shadow, occluder, pick, pre- and visibility passes when
  it uses only what they have: `uv`, `position`, `time`, `vertex_color`'s alpha, `surface.alpha` (the base map's),
  parameters, textures, noise and maths (`CompiledMaterialGraph3D::cutout`). One that uses more (the view, a normal,
  `fresnel`, `triplanar`, `normal_map`, a `glsl` node) compiles with a warning naming the node
  (`CompiledMaterialGraph3D::warnings`), and its cut-outs in those passes follow the base map's alpha.

`compile_material_graph3d(text)` writes the surface shader's GLSL (`CompiledMaterialGraph3D::surface`) and the
parameters' layout; `r3d.compile_material_graph(path)` compiles it now (development). Nodes not wired to an output are
left out.

**As assets.** A `.kinmaterials` file names a graph and its parameters' values, which merge one by one across the
base, part and variant layers (a variant can change only the tint):

```json
{"schema": "kin.materials3d/1",
 "material": {"graph": "materials/rust.kingraph",
              "params": {"tint": [0.45, 0.2, 0.08, 1], "scale": 4, "albedo": "textures/iron.png"}},
 "variants": {"wet": {"material": {"params": {"scale": 8}}}, "plain": {"material": {"graph": null}}}}
```

Numbers go into `shader_params` (a graph's parameters take its place; unnamed ones keep their defaults); a texture
(a path, or `{"asset", "sampler"}`) is bound in its parameter's slot, sRGB or linear as the graph says. Unknown names,
a number for a texture, too many numbers and parameters without a graph are errors; `"graph": null` clears the
surface (and what its parameters did). `Assets3D::load_material_graph(path)` loads graphs, cached by path:

- **cooked**, `<graph>.spv` beside the source: the layout and every variant's SPIR-V (optimised: glslc -O, as
  surface shaders compiled at run time are too), deflated, with a hash of what it holds, in one file (about 330 KB:
  twelve shaders of kin's lighting), made by
  `kin_material_graph cook <graph>` or at build time by `kin_cook_material_graph(<var> SOURCE <graph> [OUTPUT
  <file>])` (`cmake/kin_embed.cmake`), when it is at least as new as the source, or the source is gone: what a
  shipped game, which has no glslc, loads. `kin_game` cooks every graph in a game's content so, beside it
  (`kin_cook_material_graphs(<var> CONTENT <dir>)`), and its package and content pack wait for them; a build that
  can't cook (no glslc, or Android's cross-compile, whose tools don't run on the build machine) needs them cooked
  already, by a desktop build, and kept with the content: a shipping build does not configure without them;
- else the source, compiled now with glslc (development); without glslc and no cooked file, an error saying so.

Invalidating the graph or its cooked file (`SceneSession3D` does, for watched material files' dependencies) rebinds
the materials that use it: editing a graph reloads it. `kin_material_graph check <graph>` prints its errors (each
naming its node) or its parameters; `kin_material_graph glsl <graph> <out>` its GLSL; `kin_material_graph nodes` the
node types.

### Custom lighting responses

A custom native material can replace the direct-light response while retaining stock material sampling, ambient/IBL, emission and output handling. Define `KIN_DIRECT_LIGHT` before including `kin_material.inc.glsl`, then implement the named function with signature `vec3 response(Surface surface, KinLight light)`. No new sampler or uniform slots are required. Existing custom shaders and stock shaders keep their original path when the macro is absent.

```glsl
#version 450 core
#define KIN_DIRECT_LIGHT toon_response
#include "kin_material.inc.glsl"
vec3 toon_response(Surface surface, KinLight light) {
    float bands = 4.0;
    float cosine = max(dot(surface.n, light.direction), 0.0);
    float stepped = floor(cosine * bands + 0.5) / bands;
    return surface.diffuse * light.irradiance * (stepped * light.visibility);
}
void main() {
    vec4 base = kin_base_color();
    vec2 mr = kin_metallic_roughness();
    vec3 lit = kin_lit(base.rgb, kin_normal(), mr.x, mr.y, kin_occlusion());
    FragColor = kin_output(lit + kin_emissive(base.rgb), base.a);
}
```

`KinLight` supplies a unit direction toward the light, attenuated linear irradiance, separate shadow/contact visibility, the light kind (`KIN_LIGHT_SUN`, `KIN_LIGHT_FILL`, `KIN_LIGHT_POINT`, `KIN_LIGHT_SPOT`), and a position/distance for local lights in the same world space as `kin_position()` (the overlay's own space when drawing it). Directionals use zero position/distance. Irradiance already includes the point radius window, inverse-square/size limit and spot-cone attenuation, but excludes the BRDF and visibility. Return outgoing linear radiance; the stock response is `shade(surface, light.direction, light.irradiance * light.visibility)`. A callback can use visibility differently or replace it analytically. Direct lights with zero range/cone attenuation are skipped. Directional callbacks can shade the back-facing hemisphere too, allowing wrapped responses.

The callback runs from `kin_lit()`. Ambient/IBL and emission remain separate; it does not change geometry, mask coverage, shadow casters, alpha or depth. Custom materials retain their existing fixed light blocks (up to 16 selected points and 16 spots); this does not add light-grid storage bindings to custom shaders. Stock grid rendering consumes explicit local maps normally. Extra material inputs below allow custom shadow representations and other graph-produced lighting data. A complete finite-propagation solver still requires bounded scene/light history and explicit simulation policy. The position/distance fields alone do not implement physical light travel.

Build/run `kin_surface3d` for explicit sun/local graph shadows, this [toon shader](../tools/surface3d/toon.frag.glsl), and GPU ASCII postprocessing. See the [local-shadow graph contract](render_graph.md#explicit-local-shadows) for map ownership, light-index/version rules and capabilities.


### Extra material textures and buffers

Native `RendererCapabilities3D::material_inputs` supports immutable `MaterialInputs3D` sets created with `r3d.create_material_inputs(shader, textures, buffers)` and assigned to `Material3D::inputs`. The factory returns owned diagnostics. Create sets outside hot draw loops and share them across draws; the handle uses shared ownership, with resource vectors owned once per set. Stock materials carry an empty handle. Sets belong to one exact shader and device. Replacement shaders or different graph allocations require a new set; uploads into existing handles do not.

The twelve existing sampled slots remain reserved. Extra textures use consecutive SPIR-V set 2 bindings 12–15, with no gaps. Read-only storage buffers follow **all** sampled slots in that set: binding `12 + extra_texture_count` for the first, then consecutive bindings, up to eight buffers. SDL fragment storage slots start at zero; GLSL descriptor bindings include the sampler offset. Uniform buffers remain set 3 bindings 0–3, with their existing layouts.

```glsl
#version 450 core
#include "kin_material.inc.glsl"
layout(set=2, binding=12) uniform sampler2D lighting_field;
// One extra sampled texture means the first storage binding is thirteen.
layout(std430, set=2, binding=13) readonly buffer Palette { vec4 tint; } palette;
void main() {
    vec4 base = kin_base_color();
    vec3 field = texture(lighting_field, kin_uv()).rgb;
    FragColor = kin_output(base.rgb * field * palette.tint.rgb, base.a);
}
```

Use `std::array<Texture, N>` / `std::array<DataBuffer, N>` or spans at the factory. Reflection requires exact resource counts, supported dense bindings, a fragment `main`, read-only buffers, and sufficient buffer bytes for the fixed block plus one runtime-array element. A runtime array's complete indexing range remains the shader author's responsibility. Layout checks do not prove arbitrary shader memory accesses safe.

Sampled inputs support float or unsigned 2D and explicit 2D-array images, including raw depth and depth-comparison samplers. An explicit one-layer array still needs an array sampler. RGBA8/compressed colour textures use their texture scale mode; float data, integer and raw-depth inputs use nearest filtering. Depth-comparison inputs use the native reverse-Z comparison sampler. Separate sampler-state overrides for these extra inputs are not exposed. Descriptor arrays, cube/3D/MSAA/signed-integer images, storage images and writable fragment buffers are outside this profile. Custom light blocks still select up to sixteen points and sixteen spots.

A set owns its allocations and shader, while queued work retains bound resources through submission. Observing/releasing handles after renderer destruction is safe; shader/input `valid()` becomes false. Recording and mutation remain ordered on the renderer thread. Surface/view feedback is rejected. For graph dependencies, ping-pong history, CPU-upload frame boundaries and TAA revision handling, see [graph resources in materials](render_graph.md#graph-resources-in-materials). Materials files and ECS reflection do not serialize runtime binding sets; the application assigns them after resolving graph resources.

### Bloom and output in custom graphs

Native `render_bloom()` and `render_output()` expose the same bloom, exposure/ACES, alpha handling and output encoding used by stock views. Their render targets and intermediate storage belong to the caller/graph; no view or private history is required. `render_surface()` can select an explicit linear RGBA16F `linear_colour` attachment, independently of the 2D working format, so a custom graph can shade in HDR and finish into an SDR target without changing global renderer state.

Check `capabilities().bloom_pass` / `output_pass`. Bloom uses a threshold extraction plus two separable blur rounds and leaves its result in the first of two supplied RGBA16F attachments. Output combines optional bloom and encodes to the current working target; exposure/ACES apply to SDR, while HDR stays linear for the 2D frame's final tone mapping. Covered and additive alpha follow the stock contract. See [the complete contract and example](render_graph.md#bloom-and-view-output) for graph accesses, typed targets, validation, lifetime and counters.

## Models

For an isolated asset turntable, material/normal/UV inspection, explicit reload
and reusable runtime diagnostics, see [the asset preview tool](asset_preview.md).

### Shared assets and whole-model placement

For scene-owned art, `kin/render3d/assets3d.hpp` provides a renderer-scoped `Assets3D` library. Load once while creating the
scene and keep the returned `AssetHandle<Model3D>` for as long as the scene needs it. The handle uses the existing asset
ownership interface; `.shared()` also supplies the `shared_ptr<const Model3D>` expected by `SkinnedMeshRenderer3D`.

```cpp
kin::Assets3D art{r3d, kin::content_root()}; // r3d/Renderer2D outlive the art
auto loaded = art.load_model("props/tree.glb");
if (!loaded) { /* report loaded.diagnostics and choose a fallback */ }
else {
    auto tree = std::move(*loaded); // AssetHandle<Model3D>
    auto transform = tree->placement({
        .position = {4, 0, -8},
        .rotation = kin::Quatf::axis_angle({0, 1, 0}, 45),
        .pivot = kin::ModelPivot3D::BottomCenter,
        .size = 6, // longest model-bounds dimension in metres
    });
    if (transform) scene.draw(*tree, *transform, kin::colors::white, 42);
}
```

`Model3D::placement()` defaults to the authored origin and units. `Center` or `BottomCenter` positions that bounds-based pivot
at `position`; positive `size` fits the whole model before the optional nonuniform/mirrored `scale`. Node transforms remain
part of the model. Invalid sizes, unusable fitting bounds and non-finite resulting transforms return failed results.

Repeated models with the same normalized path and `MeshOptions3D` reuse the same handle while it is alive. External texture
dependencies are shared across models, keyed by resolved file, sRGB/data interpretation and nearest/mipmapped filtering.
Embedded images additionally use their owning model and image index. This prevents a normal/ORM map from aliasing an sRGB
colour map, or a nearest texture from changing another material's filtering. URI escapes and lexical `.`/`..` aliases are
resolved; symlink aliases are not deduplicated. Paths work with content folders and mounted `.kinpak` files.

The library holds **weak references only**. It owns no persistent decoded image copies and pins no model/texture resources.
Release the scene's handles, batches and draws to unload their resources; the renderer may still retain in-flight GPU work
under its usual lifetime rules. `trim()` removes expired lookup entries. `clear()` forgets all lookups so the next load reads
fresh content, without invalidating existing consumers. File changes and pack remounts require this explicit invalidation.
Invalid model loads return an empty handle and an error, and are retried on the next request.

`Assets3D::Limits{.models = 128, .textures = 512}` bounds weak lookup metadata; oldest entries are forgotten when full, while
live handles remain valid. These limits do **not** cap the assets a game chooses to keep alive or total GPU memory. Zero
disables the corresponding lookup cache. `stats()` reports entry counts, load attempts and cache hits; expired entries can
remain in the counts until `trim()` or the next load. Loading and cache operations run on the renderer thread. Do not
register these GPU loaders with `AssetServer` background workers; asynchronous decode/upload scheduling is separate work.

For many static copies, build a complete model batch once rather than expanding its mesh parts in game code:

```cpp
std::vector<kin::MeshInstance3D> trees;
// Fill trees with model-to-world matrices, tints and stable object IDs.
auto forest = kin::ModelBatch3D::create(*tree, trees, {.cell_size = 16}).value_or({});
// Each frame, after scene.clear():
scene.draw_instanced(forest);
```

`ModelBatch3D` preserves each glTF part's transform/material and every copy's tint/ID. It builds shared `InstanceBatch3D`
records and conservative camera/shadow bounds, grouping by model-transform origins into world-space cells (default 16 units;
zero disables grouping). `ModelBatchOptions3D::material` optionally replaces all parts' materials. The source model and input
span need not outlive construction; scene submission retains handles rather than copying records every frame. `parts()`
exposes read-only part batches for tools, and `instance_count()`, `triangle_count()` and `record_bytes()` report construction
costs. Skinned parts are rejected; characters keep their animation/IK through `draw_skinned` or the ECS animation bridge.

Summit uses these APIs for its shared prop atlas, survivor handle, bottom-aligned prop placement and dense scenery batches.
Its game code retains the content-specific placements, collider checks and art tints.

### Direct loading and reload

`r3d.load_model("models/crate.glb")` reads a glTF 2.0 file (`.glb`, or
`.gltf` with its buffers and images beside it) through the content folder or
pack: each primitive a mesh, placed by its node, with its material's base
colour and texture (shared mip storage, independent per-map samplers), RGB emissive
light, `BLEND` as alpha, `MASK` as a cut-out (`alphaCutoff`), double-sided and
`KHR_materials_unlit`, and its metallic and roughness factors and map, normal
map (and its scale), occlusion map (and its strength) and emissive map.
`scene.draw(model, transform)` draws its parts. A `kin::ModelFile3D` keeps one
loaded after the first successful `poll()`, then reloads on source edits.
Construction does no I/O; check each returned result. Skins and animations
are read too (see Skinned meshes below), and morph targets with their weight animations (see Morph targets).
The [tested glTF/export profile](gltf_profile3d.md) defines supported content and the export recipe. Multiple
distinct skin palettes and mixed skin/plain-node animation reject the whole asset.
Texture transforms and a selected UV set are supported when all of a material's
maps agree on that placement; incompatible per-map placements reject the asset.
Other `KHR_materials_*` extensions are outside the profile; an unsupported required
extension rejects the file. Models do not import glTF cameras or lights into the
Kin scene.

Images embedded as base64 `data:` URIs (Blender's "glTF Embedded" export) load
as a `.glb`'s do. An image that is a KTX2 or DDS file (a `.ktx2` file,
`image/ktx2`, or `KHR_texture_basisu`'s image where the texture has no other)
is uploaded compressed as it is, with its own mips, and `Assets3D::load_texture`
reads `.ktx2` and `.dds` paths the same way; Basis Universal ones need a
transcoder and are a missing texture (see
[compressed textures](assets.md#compressed-textures)). Geometry that cannot
be read as it is (Draco or meshopt compression, sparse accessors, a `POSITION` or `NORMAL` that is not `VEC3`, a
`TEXCOORD_0` that is not `VEC2`) rejects the complete asset with diagnostics in
the failed `Result3D<Model3D>`. Unsupported geometry, skins and animation are
validated before meshes or textures are uploaded. A file
named by an absolute path is never read, and a model under the content root or
in a mounted pack cannot leave it with `..` (`../textures/` from `models/`
still reads; a loose model opened from elsewhere keeps `..`). A file not read
fails the model, a buffer and an image alike: an image that cannot be read or
decoded fails `Renderer3D::load_model` with the asset's specific diagnostic;
a failed result has no model payload. Nodes nest
at most 512 deep. A plain node between two of a skin's joints is kept as a joint
of the skeleton that no vertex is bound to, so the joints below it rest where
their inverse bind matrices say (the asset is rejected if that would take
the skin past 256 joints).

### Animated nodes

An animation that moves plain nodes (a door on its hinge, a fan, a crane's arm, a
prop's root) plays as a skin's does: the moved nodes and every node above them are
a skeleton of the model, its **node skeleton** (`Model3D::node_skeleton`, an index
into `skeletons` after the skins; -1 for none). Its joints are those nodes, parents
first, with their names and rest placings; its inverse binds are identities, so its
joint matrices are the nodes' world matrices in the model's space. The animation's
channels on them are a clip of that skeleton, of the animation's name (after the
skins' clips, so theirs keep their places). Each part that is not skinned hangs from
its nearest node in it (`ModelPart3D::node`, -1 for none) with its placing below that
node (`ModelPart3D::node_offset`); `ModelPart3D::transform` stays its place at rest, so
everything that draws a model at rest is unchanged. A part is drawn at its node's
matrix times its offset (`Model3D::part_transform(part, node_matrices)`).

```cpp
auto loaded = r3d.load_model("models/door.glb");
if (!loaded) return report(kin::diagnostics3d_text(loaded.diagnostics));
kin::Model3D door = std::move(*loaded);
if (door.node_skeleton < 0 || std::size_t(door.node_skeleton) >= door.skeletons.size())
    return report("door has no node animation");
const kin::u32 nodes = static_cast<kin::u32>(door.node_skeleton);
const auto* open = door.find_clip("Open", nodes);
if (!open) return report("door has no Open clip");
kin::AnimationPlayer3D player{&door.skeletons[nodes]};
player.play(open, /*loop=*/false);
std::vector<kin::Mat4f> matrices;

// each frame
player.update(dt);                                      // crossfades, layers and events as for a character
player.joint_matrices(matrices);                        // the nodes' world matrices
scene.draw(door, kin::Mat4f::translation(position), matrices);
```

`scene.draw(model, transform, node_matrices)` places the parts under an animated
node by the pose and the rest as `draw(model, transform)` does; the rest pose's
matrices draw the same picture (to rounding). A part whose node has no matrix in the
span, or one that is not finite, is drawn at rest (logged once). A model with a skin
and animated props as well: pose each skeleton with its own player (the props' clip
is `find_clip(name, model.node_skeleton)`) and draw both with
`scene.draw_skinned(model, joints, transform, tint, id, {}, node_matrices)`.
In the ECS a `ModelRenderer3D` with an `Animator3D` plays the animator's clips on
its model's node skeleton (state machines, blends, layers, actions, events and IK
on the nodes' names); without an animator it draws exactly as before.

The node skeleton is made only when a part that is not skinned hangs from one of its
nodes: an animation of nodes that carry no mesh of the model (BrainStem's root above
its skin's joints, a node of another scene) is ignored as before, with a line in the
log. At most 256 nodes (the moved ones and those above them); more are not animated
(logged). Not animated yet: a skin under an animated node (its joints' root does not
follow it), a node under a skin's joint (the part follows the joint only), and morph
targets.

## Skinned meshes

A character is a mesh whose vertices follow a skeleton. kin keeps the three
things apart: the **skeleton** and its **clips** (plain data and maths on the
CPU, `kin/render3d/skeleton3d.hpp` and `animation3d.hpp`), the **pose** they make
each frame (one matrix a joint), and the **skinned mesh** that the GPU poses by
those matrices (`Scene3D::draw_skinned`). It is not `kin/anim` (the engine's
animation of registered properties): that one's state machines can choose the
clips and weights fed to this.

```cpp
auto loaded = r3d.load_model("models/hero.glb");
if (!loaded) return report(kin::diagnostics3d_text(loaded.diagnostics));
kin::Model3D hero = std::move(*loaded);
if (hero.skeletons.empty()) return report("hero has no skeleton");
const auto* walk = hero.find_clip("Walk");
if (!walk) return report("hero has no Walk clip");
kin::AnimationPlayer3D player{&hero.skeletons[0]};
player.play(walk);
std::vector<kin::Mat4f> joints;

// each frame
player.update(dt);                                             // clips sampled, blended, layered
player.joint_matrices(joints);                                 // world pose times inverse bind, one a joint
scene.draw_skinned(hero, joints, kin::Mat4f::translation(position));
```

**Data.** `Skeleton3D`: joint `names`, `parents` (always lower than the joint, so a
pose turns to matrices in one pass), the `rest` pose (local translation,
rotation, scale), `inverse_bind` matrices and a `root_transform` (the file's nodes
above the roots). At most `max_skin_joints` (256: a joint index is a byte on the
GPU). `Pose3D` is the local `JointPose3D` of every joint. `AnimClip3D` holds, per joint,
`Track3D`s of translation, rotation and scale keyframes with `Step`, `Linear` or
`CubicSpline` interpolation exactly as glTF specifies (rotations slerped; a cubic
spline's tangents scaled by the time between keys). A joint with no track keeps its rest value.

**Maths.** `sample_clip(clip, skeleton, time, loop, pose)`; `blend_poses(a, b, w)` (or `blend_poses(a, b, w, out)`, which
may write over `a` or `b` and allocates nothing once `out` has room: the player blends this way, into scratch it keeps, so a
1D blend, a crossfade or a layer allocates nothing after its first frame, and the ECS animator compares its state and layer
keys without building the strings);
`PoseMask3D::subtree(skeleton, joint)`, `::joints(skeleton, names)` and `blend_masked`
for an upper-body layer over locomotion; `apply_additive(base, additive, reference,
mask, weight)` for additive clips (hit reactions, breathing); `extract_root_motion`
and `remove_root_motion` for clips whose root joint carries the character's travel
(you get the delta between two times, wrapping at a loop's end, or its start at a negative speed, and the pose with the root held at rest);
`compute_joint_matrices(skeleton, pose, out)`.

**Player.** `AnimationPlayer3D` plays a clip (`play`), fades to another
(`crossfade_to(clip, seconds)`; a fade begun during a fade fades from the picture of
that moment, held, so nothing jumps), and has layers (`layer(i)`: clip, weight, mask,
additive, loop, speed). `set_root_motion(true)` holds the base layer's root and reports the motion
of the last `update` as `root_motion()`. The pointers to the skeleton and clips are
not owned: a `Model3D`'s stay put as long as the `Model3D` is not copied or changed (`ModelFile3D::poll()` replaces its model in
place, so rebind a player after it returns true).
Measured: about 2 us for a 24-joint character's `update` and matrices, 4.5 us with a crossfade.

**Loading.** Asset loaders accept one distinct skin palette per file, with at
most 256 joints including intermediate nodes. Equivalent skins with identical
joint order and inverse bind matrices share that palette. Distinct palettes,
including reordered or different bind matrices and unused distinct skins, reject
the whole model before it becomes a usable asset; split the export into models.
`Animator3D` and `draw_skinned(model, ...)` pose `skeletons[0]`.

`JOINTS_0` (u8/u16) and `WEIGHTS_0` (float or normalized u8/u16) supply at most
four influences per vertex. Finite positive sums are normalized; missing usable
weights, out-of-range joints, extra influence sets and oversized skins are asset
errors, rather than a partly static character. Translation/rotation/scale clips
support Linear, Step and CubicSpline, as do morph weight channels. Models needing
both a skin pose and a separate visible plain-node pose are rejected. Plain-node
animation by itself remains supported. A rigid mesh under a joint is attached to
that palette with full weight and follows it normally.

The low-level `read_model_data` keeps parsing results for inspection/tools,
including multiple skeletons and recoverable partial data. A true parse result is
not an asset acceptance result: check `ModelData3D::asset_diagnostics` too. Both
CPU-only `Assets3D` and GPU loaders reject those diagnostics, failed loads are not
cached, and `ModelFile3D` keeps its last good model after a rejected reload. The
[profile and acceptance tests](gltf_profile3d.md) cover both paths.

**Meshes.** `MeshData3D::skin` is a parallel stream (four joint indices and four
weights a vertex), empty for a mesh that is not skinned; `validate`, `append` and `bounds`
know it. On the GPU it is a second vertex buffer (8 bytes a vertex: four joint bytes and four unorm
weight bytes that sum to exactly 255), so a mesh without a skin has the vertex buffer and pipelines it
always had. A skinned mesh cannot have levels of detail or clusters (`MeshOptions3D::levels`,
`clusters` are ignored, with a line in the log).

**Drawing.** `scene.draw_skinned(mesh, joint_matrices, transform, material, tint, id)`:
the matrices (at least `Mesh3D::skin_joint_count()`) are copied into the scene; each call is one copy with its
own pose, and copies of one mesh and material one after another are drawn as one instanced draw. The
renderer puts the frame's matrices in one storage buffer and each copy's first one in a buffer of its
own (the 128-byte instance is unchanged); the vertex shader moves each vertex by the weighted sum
of its joints' matrices and turns its normal by their linear part (right for rigid and uniformly scaled
joints; a non-uniform scale in a joint is approximated). `draw_skinned(model, joints, transform)`
draws all of a model's parts (skinned ones in the pose, the others as `draw` does).
`RendererCapabilities3D::skinning`; `stats().skinned_copies` and `stats().skinned_draws`
report submitted copies/draws; `debug_stats().skin_matrices` counts uploaded matrices.

To animate submitted commands, call `scene.update_joint_matrices(first, matrices)` between renders. `first` is an index
in the scene's existing `joint_matrices()` array; record its size before `draw_skinned`, or use the copy's entry in
`skin_offsets()`. The replacement range must fit or the call returns a failed `Status3D`; it never resizes the palette or
changes `draw_revision()`. Empty ranges at the end are allowed and overlapping source spans are supported. Indices are
local to the scene's current submissions: obtain them again after `clear()`. Parts sharing a model pose see one update.
This edits existing storage without another palette or per-character cache. Transforms/materials still require ordinary
scene submission.

**Every pass** (P1a part 2). A skinned copy is drawn by the main pass, casts the sun's shadow (one map or any
cascades, cut-out materials too), is in the depth and normal pre-pass (ambient occlusion, contact shadows, the
occluder depth of occlusion culling, cut-outs included), has its own velocity under temporal anti-aliasing, and is
picked (`View3D::pick` names its `id`). It is culled by its pose: its bounding sphere is made each frame from where its joints
put the spheres each of them pulls on (never smaller than the posed mesh, so an arm that swings into view is not culled).

*Velocity.* The renderer keeps last frame's joint matrices of each view (the whole array, uploaded again as the "previous" joints)
and matches each copy to last frame's the way it matches the model matrix: by `MeshInstance3D::id` when non-zero, else by its place
among the copies of its mesh when the count agrees, else the nearest, else it is new (no history: velocity far off screen, so TAA
gives it no history weight). The skinned vertex shaders place the vertex with this frame's joints and this frame's model matrix,
and again with last frame's of both, so a moving pose has its own velocity (main-pass velocity when TAA is alone, the pre-pass's
when ambient occlusion or contact shadows are on, and the overlay's). Give moving characters an `id`: by order a copy that
leaves the list shifts its neighbours' histories (`draw_skinned(model, ..., id)` and the ECS renderer do; the ECS id is the entity's).

*Shadow cache.* Each posed copy's matrices are in the cache's comparison (a sample of them in the cheap key, all of them in the
full comparison against the snapshot), and the mesh (its skeleton) in the key: a character standing still keeps the map, one that
moves redraws it, and the cache never serves a stale pose.

*Shared pose.* `draw_skinned(model, ...)` copies the pose into the scene once and every skinned part points at that range (BrainStem:
18 matrices, not 59 times 18).

*Entities.* `SkinnedMeshRenderer3D` (a shared `Model3D`, tint, `visible`, `id`) and `Animator3D` (see Entities below).

Limits: a skinned mesh has no levels or clusters; a non-uniform scale in a joint skins normals approximately; the velocity of a
copy matched by order with a changed count is a guess (give it an id); D3D12 and Metal are untested.

**One-dimensional blends.** `player.play_blend(points, parameter)` / `crossfade_to_blend(points, parameter, seconds)` and
`set_blend_parameter(p)`: the clips of `BlendPoint3D{clip, position}` blended by the parameter (the two points either side of it;
held at the ends), each clip at the same phase of its own length. The base's `time()` is then a phase (0 to 1) that advances by
`dt * speed / (the duration blended at the parameter now)`, so a walk and a run step in time and a changing parameter never jumps.
Fades, root motion and layers work as for a clip.

## Morph targets

Shapes a mesh blends toward (glTF's morph targets, Blender's shape keys): a face's expressions, a creature's squash
and stretch, a door's dent. `MeshData3D::morph_targets` holds, for each target, a position offset for every vertex
(and optionally a normal offset); `morph_weights` the weights the mesh starts at (none: all 0). The mesh then shows
each vertex at `position + sum(w[t] * positions[t])`, its normal `normalize(normal + sum(w[t] * normals[t]))`.

```cpp
kin::MeshData3D blob = kin::meshes::sphere(0.5f, 48, 24);
kin::MorphTarget3D squash{"squash"};
for (const auto& v : blob.vertices)
    squash.positions.push_back({v.position.x * 0.3f, -(v.position.y + 0.5f) * 0.5f, v.position.z * 0.3f});
blob.morph_targets = {std::move(squash)};
kin::Mesh3D mesh = r3d.create_mesh(blob).value_or({});
const float weights[] = {0.8f};
r3d.set_morph_weights(mesh, weights);   // from the next render on; every copy drawn shows it
kin::Mesh3D other = r3d.copy_mesh(mesh).value_or({}); // a second one with weights of its own
```

`Renderer3D::set_morph_weights(mesh, weights)` takes one weight for each target (`Mesh3D::morph_target_count`,
`morph_target_name`, `find_morph_target`), each within [-1, 1]; weights equal to the current ones cost nothing. On
native devices the blend runs on the GPU (`morph_blend.comp`, recorded into the frame's commands: the call itself
takes about 5 microseconds whatever the size, against 0.12 to 1.3 ms for an 8,000-vertex mesh blended on the CPU
with 8 to 52 targets changing; `kin_render3d_surface_bench`): the mesh keeps its vertices at rest and every target's
offsets on the GPU (24 bytes a vertex a target), shared by its copies. WebGL2 blends on the CPU (skipping targets of
weight 0) and uploads the vertices. Either way the vertices are cycled: a frame still drawing the old ones keeps
them. All passes see the new shape: shadows (the shadow caches notice it), the pre-pass and
its occlusion, decals, picking. A skinned mesh is blended first, then skinned. `Mesh3D::bounds()` of such a mesh
holds it at any weights within [-1, 1], so culling never drops a blended copy. Since the weights belong to the mesh,
copies drawn of it share them: `copy_mesh` gives another mesh (its own GPU vertices, the same targets) for another
character. A mesh with targets has no levels of detail or clusters (asked for, it is made without them, with a warning in the log),
keeps its vertices and targets on the CPU, and has a vertex buffer of its own. Where the GPU blends, temporal anti-aliasing sees a changing shape's own motion: the first blend of a frame keeps the
shape the frame before drew, and the velocity pre-pass reads its positions from there (the pre-pass always runs with
TAA when such a mesh is drawn; `debug_stats().morph_velocity_draws`). Set weights before rendering the frame that
shows them: a shape changed after a frame's render moves without velocity in the next. Meshes with a moving surface
shader (a world-position offset), and WebGL2, keep the copy's own motion only. Global illumination is not told that the shape
changed (its scene follows when something else makes it voxelize again; area-light shadows keep the first shape).
`capabilities().morph_targets` is true on every native device and on WebGL2.

`blend_morph_targets(data, weights, out)` and `morph_bounds(data)` are the same on the CPU, for tools.
`Scene3D::draw_skinned(model, ..., node_matrices, history_id, meshes)` draws a model with some parts' meshes replaced
(an entity's own copies).

**Animation.** `AnimationPlayer3D::morph_weights()` (by model part; an empty entry is a part no playing clip moves)
blends the playing clips' weight channels as the pose is blended: crossfades (a fade in the middle of another fades out
the held weights), 1D blends, layers by their weight (additive ones add their change from their first frame) and the
action. A model needs a skeleton (a skin or animated nodes) for the player; weights-only animations of a model with
neither are applied with `apply_clip_morphs`.

**Entities.** A `MorphWeights3D` on an entity with a `SkinnedMeshRenderer3D` or `ModelRenderer3D` gives it weights of
its own: `WorldRenderState3D::apply_morphs(renderer)` (SceneFrame3D calls it before collecting; call it after
`animate()` in a hand-made loop) makes the entity copies of the model's parts with targets (once a model, and again for
an entity the component was copied to) and sets them from the model's weights, then the clips its `Animator3D` plays
(unless `animated` is off), then the targets named in `weights`. `collect()` draws the copies; entities without the
component draw the shared meshes. Authored as `{"MorphWeights3D": {"weights": "smile=0.8, blink=1"}}`; from C++
`get_mut<MorphWeights3D>()->set("smile", 0.8f)` / `erase("smile")`; from Lua `scene3d.set_morph_weight(entity, "smile",
0.8)` (adds the component when the entity has none), `scene3d.morph_weight(entity, "smile")` (nil when not set) and
`scene3d.clear_morph_weight(entity, "smile")`.
`MeshData3D::append` meets targets by name (a target only one side has moves the other side's vertices by nothing).

**glTF.** A primitive's `targets` (POSITION and NORMAL offsets; sparse accessors are read; TANGENT and other
attributes are ignored), the mesh's `extras.targetNames` (else `target0`, `target1`, ...) and the node's, else the
mesh's, `weights` are read into each part's mesh. An animation channel of a node's `weights` becomes a
`MorphChannel3D` (`parts`: the parts made from that node's mesh, `targets` weights a key; step, linear and cubic
spline) in `AnimClip3D::morphs`, on every clip of that animation; an animation of weights alone is a clip of its own
with no joint channels. `apply_clip_morphs(r3d, model, clip, time, loop)` sets them all for a moment of the clip,
`sample_morph_channel` samples one:

```cpp
player.update(dt);
kin::apply_clip_morphs(r3d, model, *model.find_clip("Talk"), talk_time);
```

## Inverse kinematics

A hand to a hold, a foot on the ground, a head to a point: solvers that bend a posed skeleton after the clips and before the
matrices (`kin/render3d/ik3d.hpp`, CPU only, no shader changes; the stage P1b of the plan). They work on the **local** pose and
keep the joints' **model-space** world matrices in step, so they chain (each sees what the one before did) and cost a few
microseconds a character.

```cpp
kin::Pose3D pose = player.pose();
std::vector<kin::Mat4f> world, joints(skeleton.joint_count());
kin::compute_world_matrices(skeleton, pose, world);

kin::TwoBoneOptions3D arm;
arm.pole_mode = kin::PoleMode3D::Direction;          // the elbow points back
arm.pole = {0, 0, -1};
arm.max_stretch = 1.1f;                            // may lengthen 10%, softly
arm.match_rotation = true;                         // the hand to the hold's surface
arm.end_rotation = kin::Quatf::look_rotation(-hold_normal);
kin::solve_two_bone(skeleton, pose, world, {shoulder, elbow, hand}, hold_in_model_space, arm);

kin::world_to_joint_matrices(skeleton, world, joints);   // what draw_skinned wants
```

**`solve_two_bone`**: an arm or a leg (root, mid, end joints; twist joints between them are carried along). It rotates the root and
the mid joint (local rotations; the pose's local translations and scales are kept) so that the end reaches the target, or stands
as near as the chain can, on the line to it. The elbow goes to the pole's side (a direction or a point), the weight blends from
the given pose (0 returns it bit for bit), `max_stretch` lets the bones lengthen with a soft clamp (measured against the skeleton's
rest lengths, so solving twice never stretches twice), `min_bend_degrees` keeps a knee from locking, and `match_rotation` turns the
end joint to a model-space orientation. Details worth knowing: it is solved in the space of the root joint's parent, so a scaled or
turned model and a non-uniform scale above the chain are exact (and so is one on the root joint itself, or between it and the
mid joint, where the forearm's drawn length depends on how it is turned: the mid joint is turned until the end is at the right
distance, then the rigid solve and up to 8 passes from its own result; a hand lands within 1e-6 of any reachable target);
the upper bone's turn carries the bend plane with it as far as the pose is bent, so the elbow hinges and the twist is taken at the
shoulder (a straight pose has no plane: the shortest turn); a pole on the target's axis falls back smoothly on the pose's own bend
and, for a chain that is straight with no pole, on the way the **rest pose** is bent (what the rig's author built, turned as the
root has turned), else the hinge's side, else toward -Y of the root's parent (give a pole). A target nearer the root than the
folded chain can come, with unequal bones, turns the chain steeply but without a jump.

*Joint limits* (all off by default; the pose is then what it always was):

```cpp
arm.max_bend_degrees = 150.0f;            // the mid joint folds to 150 and no further (a target nearer is aimed at as nearly)
leg.hinge = true; leg.hinge_axis = {1, 0, 0};  // the knee bends about ITS OWN X, the positive way only
leg.max_twist_degrees = 30.0f;            // the hip turns at most 30 degrees about the thigh from what the clips did
```

With a hinge the mid joint's local rotation is a pure turn about the axis (a knee never takes a sideways twist, and a clip's
hyperextension is brought back to the forward side); a pole that asks the knee to go behind makes the root twist to carry the hinge
round to it, and `max_twist_degrees` limits that twist: the knee's side falls short of the pole's (the foot still reaches), and
as the pole goes round to straight behind the limit itself eases to nothing at a half turn, so the knee comes back to the clips'
side and never flips (swept round the leg in 0.5 degree steps: 0.5 to 0.7 degrees a step). The twist is measured from a plane: the
hinge, or the pose's bend (a nearly straight pose with no hinge fades the limit with how bent it is).

*The side of the bend over time.* A pole going through the target's axis (the hand over the head with the elbow pole up) must
swing the elbow to the other side: for any function of the target alone that is inevitable (no field of directions across the
axis of every target is without a zero: the hairy-ball theorem), and it happens over a few degrees of the target's travel. With a
`TwoBoneState3D` and `bend_swing_degrees_per_second` (the ECS constraint keeps the state) the side swings at that bounded speed
instead, whatever the target does: 6 degrees a frame at 360 a second, from 24 degrees a frame stateless in the test sweep. The one
case that stays: a target through the exact centre of a chain folded on itself with equal bones turns the upper bone half a turn in
one step (the joints are in the same places); passing 2 cm off it the same turn takes steps of 2.3 degrees.

**`solve_look_at`**: a head, or a chain (spine, neck, head) whose `LookAtJoint3D::weight`s share the turn. Each joint has yaw and pitch
limits (an ellipse, seen from the head); past a limit the joint is held there and eased back toward forward over `release_degrees`,
so a target going round behind never flips the head from one side to the other. The turn is the shortest one, so nothing twists.
Smoothing is a `LookAtState3D` (the followed direction, in the space the target is in) with `smooth_seconds` and `max_speed_degrees`
and a `dt`: the function does not hold state itself.

**`solve_chain_fabrik`**: FABRIK for tails, ropes and spines (any number of joints, a tolerance and an iteration cap, optional
fixed root); bone lengths are kept, each joint but the tip is turned the shortest way onto its new bone. A `pole_mode` / `pole`
turns the solved chain about the line from its first joint to the tip (so neither end nor any length moves) until its bulge
faces the pole; a straight chain, or a pole along the line, fades the turn out rather than flip.

**`plan_foot_placement` / `apply_foot_placement`**: for each leg it casts the game's `GroundQuery3D` (`std::function<optional<GroundHit3D>
(Vec3f from, Vec3f to)>`; P2's physics raycast is one) from above the animated ankle, puts the ankle target on the hit lifted by
the foot height (a foot in the air keeps its clearance over the ground it is above), tilts the foot to the normal clamped to
`max_tilt_degrees` (not at all in the air), and finds how far the pelvis must go down for the lowest foot (`max_pelvis_drop`;
`max_pelvis_raise`, 0 by default, for the pelvis to rise on a step). `FootPlacementState3D` eases the pelvis offset and a foot that
the ground fell away from (it rises at once: no sinking into a step). The plan touches no pose; `apply_foot_placement` moves
the pelvis joint and solves the legs.

*Foot locking* (`FootPlacementOptions3D::lock_feet`, off by default; needs a state and a `model_to_world`, which the ECS gives it): a foot
whose animated sole is within `lock_height` of the ground is planted, and its ankle target is held at the place it was planted, in
the world, while the body moves over it; it lets go when the animated foot rises past `lock_release_height` or the animated ankle is
`lock_release_distance` from the held place (the clip has moved on), and eases back to the animated ankle over `lock_replant_seconds`
(smoothstep), then plants again. The leg's reach and `max_stretch` are the limit: a held foot the leg cannot reach is as near as it
can get. *Toes* (`FootLeg3D::toe`, a child of the ankle): a second ground query under the animated toe; the toe turns to that normal,
clamped to the foot's slope and to `toe_max_tilt_degrees` from its animated turn, and not at all while the foot is in the air. The
ground query is called once for each leg (and once more for each toe), however many times the constraints are solved in a frame
(the ECS keeps the answers for the frame).

### In the ECS animator

`Animator3D::ik_constraints` (an `IKConstraint3D` each: `name`, `kind` TwoBone / LookAt / FootPlacer / Fabrik, `joints` by name, `weight`,
`fade_seconds`, a target, a pole, options) are solved in `WorldRenderState3D::collect()` (or by `apply_ik()`), in list order,
on the pose the clips and layers made, with the entity's `WorldTransform3D` of the frame; then the joint matrices are made from
the result. An animator with none is exactly what it was, to the byte.

```cpp
kin::Animator3D animator{.clip = "Idle"};
animator.add_ik({.name = "feet", .kind = kin::IKKind3D::FootPlacer, .joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"},
                 .foot_heights = {0.05f, 0.05f}, .ground = [&](kin::Vec3f from, kin::Vec3f to) { return physics.raycast(from, to); }});
animator.add_ik({.name = "right_hand", .kind = kin::IKKind3D::TwoBone, .joints = {"shoulder_R", "elbow_R", "hand_R"}, .fade_seconds = 0.15f});
entity.set(animator);

// each frame, from game code (a set from a non-const lvalue moves it: take the pointer)
auto* a = entity.get_mut<kin::Animator3D>();
a->ik("right_hand").set_target(hold_entity, {0, 0.05f, 0}).set_pole_direction({0, 0, -1}).set_weight(gripping ? 1.0f : 0.0f);
```

- **Targets.** A world-space point (`set_target(Vec3f)`), a point carried by another entity (`set_target(entity, local_offset)`:
  its `WorldTransform3D` of the frame, so it follows it), or none. A target entity that is destroyed, or has no `WorldTransform3D`,
  counts as no target: the constraint lets go over its fade, from the place it was last at. Poles are a world point
  (`set_pole_position`) or a model-space direction (`set_pole_direction`). The foot placer's `ground` is in world space.
- **Weights** are a goal (`set_weight`) the weight in force eases to linearly in `fade_seconds` and shapes with a smoothstep: a hand
  lifts to a hold and lets go with no jerk. 0 in force means the constraint does nothing and costs nothing.
- **Order matters.** A constraint moves everything below its joints, so list them in the order the pose wants them: feet (they lower
  the pelvis), then the look (it turns the spine), then the arms, so that a hand lands on its target. A second constraint on the
  same chain wins.
- **When.** Solving is in `collect()`, not `animate()`, because the entity's matrix of the frame is known only after
  `propagate_transforms()`. Smoothing uses the `dt` of the `animate()` steps since the last solve, once: two views, or an early
  `apply_ik()` to read a joint, change nothing. The solved pose and its model-space world matrices stay on the animator
  (`ik_pose`, `ik_world`), for gameplay that wants to know where a hand is.
- **Culling, velocity, shadows.** All take the matrices as they come: the bound is made from the posed joints (a hand reaching
  into view is not culled), the previous frame's matrices (TAA velocity) are the previous frame's IK'd ones, and the shadow cache
  sees the pose change.

- **FABRIK** (`IKKind3D::Fabrik`): `joints` root to tip, a target, an optional pole (`set_pole_position` / `set_pole_direction`), `weight`
  and fade like the others, and `fabrik` options (tolerance, iterations, `fix_root`): a tail, a rope held by a hand, a spine.
- **Limits and state.** `two_bone` carries `max_bend_degrees`, `hinge` / `hinge_axis`, `max_twist_degrees` and
  `bend_swing_degrees_per_second` (the elbow's side swings at that speed; the constraint keeps the state, a weight of 0 forgets it); a
  foot placer's legs use them too. `foot.lock_feet` (and its thresholds) lock planted feet in the world; `toe_joints` / `toe_heights`
  / `foot.toe_max_tilt_degrees` give the legs toes.

Cost (an 18-joint person, a hand, a three-joint look-at and two feet, with the matrices): about 6 microseconds on one core; 500
such characters in the ECS (`animate` + `propagate` + `collect`, 3 constraints each) 3.2 ms against 0.65 ms for the same
500 with none. The solvers know nothing of collisions (the ground query is the game's) or of a limit's reach beyond what is
written above; look-at has its limits, the two-bone and FABRIK (a pole only) have the ones above; D3D12 and Metal are untested
(nothing here is GPU).

## Entities

`kin/ecs/render3d.hpp` puts meshes on ECS entities: `Transform3D` (position,
rotation, scale, relative to the flecs parent), `WorldTransform3D` (added with
it, composed through the hierarchy) and `MeshRenderer3D` (mesh, material,
tint, and an `id` that pick reports and TAA matches frames by: the entity's own when 0). `WorldRenderState3D::propagate_transforms()`
then `collect(scene)` turn them into one instanced draw a mesh and material.

```cpp
kin::WorldRenderState3D state{world};
world.entity().set(kin::Transform3D{.position = {2, 0, 0}}).set(kin::MeshRenderer3D{.mesh = crate});
// each frame
state.propagate_transforms();
state.collect(scene);
```

**Lights.** `PointLight3D` and `SpotLight3D` (colour, radius, size, for a spot the cone's angles, the `shadow_*` fields, `enabled`) on an entity
with a `Transform3D` put a light at its place in `collect(scene)` (a spot shines down the entity's own -Z, the way a camera looks; parent it to a
camera or a hand to carry it); the entity's id names the light for the choice of which has a shadow map.

**Characters.** `SkinnedMeshRenderer3D{.model}` (a `std::shared_ptr<const Model3D>`: its parts, skeleton and clips) draws the model at the
entity's `WorldTransform3D`, in the pose of the entity's `Animator3D`. It is its own component, not `MeshRenderer3D`: a mesh renderer's
entities merge into one instanced draw of one mesh, and a character is several parts with a skeleton and a pose of its own.
`WorldRenderState3D::animate(dt)` (once per fixed step or frame) steps every animator; `collect` draws them. An animator rebinds to
its entity's model whenever that `shared_ptr` is replaced by another, even a model that lands at the old one's address. An
`Animator3D` beside a `ModelRenderer3D` poses that model's animated nodes (its node skeleton: see Models, Animated nodes), so a
door or a fan placed as a static model plays its clips; a `SkinnedMeshRenderer3D` keeps posing its model's first skin.

```cpp
auto hero = std::make_shared<const kin::Model3D>(r3d.load_model("models/hero.glb").value_or({}));
auto machine = std::make_shared<kin::AnimationStateMachine>();              // kin/anim's, unchanged
machine->initial = "idle";
machine->states = {{"idle", "Idle"}, {"move", ""}, {"jump", "Jump"}};      // state -> clip name
machine->transitions = {{"", "jump", {{.param = "jump", .op = kin::CondOp::TriggerSet}}},
                        {"idle", "move", {{.param = "speed", .op = kin::CondOp::Greater, .threshold = 0.1f}}},
                        {"move", "idle", {{.param = "speed", .op = kin::CondOp::Less, .threshold = 0.05f}}}};
kin::Animator3D a;
a.machine = machine;
a.blend_spaces["move"] = {"speed", {{"Walk", 1.5f}, {"Run", 4.5f}}};         // 1D blend by the float `speed`
a.fades = {{"idle", "move", 0.25f}, {"", "jump", 0.1f}};  a.fade_seconds = 0.3f;
a.layers = {{.clip = "Wave", .mask_joint = "UpperArm.R", .weight = 1.0f}};   // data: clip and mask by name
world.entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer3D{.model = hero})
              .set(static_cast<const kin::Animator3D&>(a));   // a const lvalue: flecs's set() moves from a non-const one
// each fixed step: set parameters from game code, then
animator->params.set_float("speed", velocity);  animator->params.set_trigger("jump");
state.animate(dt);
```

Without a machine, `Animator3D::clip` names the base clip (changing it crossfades over `fade_seconds`). The state machine is
`kin/anim`'s `AnimationStateMachine` and `AnimationParams`, evaluated with its own `evaluate()` (one transition a step, triggers
consumed); no change to `kin/anim`. What is not bridged: `kin/anim` events and registered-property tracks (a skeletal clip is not
one). `apply_root_motion` moves the entity's `Transform3D` by the base layer's root motion; `remove_root_motion` only holds the root
(in place). The base animator settings and model asset reference can be authored
through the shared scene/prefab registry below. `AnimatorAsset3D` adds authored
state machines, blend spaces, layers, fades and IK descriptions.

### Authored scenes and prefabs

`kin/ecs/scene3d.hpp` extends the existing ECS registry, `.kinscene` (`kin.scene/1`),
`.kinprefab` (`kin.prefab/1`), overrides, editor play clones, inspection and Lua.
There is one parser, field codec and instantiation pipeline for 2D and 3D.
The standard `register_builtin_editor_components(world)` includes these registrations
in experimental 3D builds; `register_render3d_components` and
`register_physics3d_components` can also be called separately. Registration is idempotent.

A static prop prefab can contain a root transform and complete model, plus children
with local transforms, lights and primitive colliders:

```json
{
  "schema": "kin.prefab/1", "id": "crate", "root": "root",
  "entities": [{"id": "root", "components": {
    "Transform3D": {"position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
    "ModelRenderer3D": {"asset": "props/crate.glb", "visible": true},
    "Collider3D": {"kind": "box", "half_extents": [0.5, 0.5, 0.5]}
  }}]
}
```

```cpp
#include <kin/ecs/scene3d.hpp>
#include <kin/scene/scene_asset.hpp>

kin::register_builtin_editor_components(world);
auto loaded = kin::load_prefab_asset(content_root / "prefabs/crate.kinprefab", world.components());
if (!loaded.ok()) { /* display loaded.diagnostics; do not instantiate */ }
else {
    kin::PrefabOverrideSet overrides;
    overrides.entities["root"] = {{"Transform3D", {{"position", kin::Vec3f{3, 0, -2}}}}};
    auto instance = kin::instantiate_prefab(world, *loaded.prefab, {}, world.components(), overrides);
    // Check instance.ok()/diagnostics, then bind authored assets on the render thread:
    auto bound = kin::resolve_render3d_assets(world, assets); // Assets3D{renderer3d, content_root}
    // Check bound; report bound.diagnostics. Failed replacements preserve their last good model.
}
// WorldRenderState3D: animate(dt), propagate_transforms(), collect(scene) as usual.
```

The existing `load_scene_asset` / `instantiate_scene_asset` and `SceneAssetRuntime`
use the same path. Scene prefab placement is an ordinary override:
`"overrides":{"root":{"Transform3D":{"position":[3,0,-2]}}}`. Asset paths are
relative to `Assets3D`'s content root, including models referenced by nested prefab files.
Folders and mounted `.kinpak` archives work through the existing content utilities.
Summit's `content/prefabs/survivor.kinprefab` exercises this pipeline for its skinned
survivor; its animation graph and IK configuration are also authored. Gameplay supplies
parameters, targets, raycasts and ragdoll poses.

Initial registered authored fields:

| Component | Fields |
|---|---|
| `Transform3D` | position, quaternion rotation, scale |
| `ModelRenderer3D`, `SkinnedMeshRenderer` (C++ `SkinnedMeshRenderer3D`) | asset, tint, visible, object id |
| `ModelMaterials3D` | material override asset, named variant (static/skinned) |
| `Animator3D` | clip, speed, loop, playing, fade seconds, apply/remove root motion |
| `AnimatorAsset3D` | referenced animation/IK configuration |
| `SceneCamera3D` | enabled/priority, projection, fov/height, near/far; pose from Transform3D |
| `SceneEnvironment3D` | enabled/priority, sun/fill/ambient, shadow budgets/area, sky, fog, AO/contact/TAA, MSAA/HDR/exposure/bloom |
| `TimeOfDay3D` | hours, day, latitude, axial tilt, moon phase, speed, shadows |
| `PointLight3D`, `SpotLight3D` | linear colour, radius, size, enabled, shadows enabled; spot cone angles |
| `Collider3D` | primitive dimensions or referenced compound/hull/mesh recipe; local offset/rotation/scale |
| `RigidBody3D` | static/kinematic/dynamic motion, mass/density, friction/restitution/damping, gravity, sensor/sleep/CCD, initial velocities, layer |
| `CharacterController3D` | capsule dimensions, movement/jump/grounding settings, layer/mask, up/facing |
| `CharacterAnimationLink3D` | animation parameter names, foot constraint name, ground-query mask |
| `ParticleEmitter3D` | seed/emission/capacity, shape/ranges/forces/lifetime curves, texture/atlas/blend/facing, enabled/emitting |
| `DecalRenderer3D` | texture and normal images, box size, colour, roughness/metallic, normal strength, emission, normal/depth fade, drawing order, visible |
| `MorphWeights3D` | morph target weights by name (`"smile=0.8, blink=1"`), whether the animator's clips set the others |

Vectors accept arrays or `x/y/z` objects; rotations use `x/y/z/w` order and must be
finite unit quaternions. Enum adapters reject unknown names through normal patches.
`ModelRenderer3D` preserves imported part transforms/materials and batches compatible
meshes/materials with other static entities; all parts use the entity ID unless an
explicit nonzero `id` is authored. Skinned assets must contain a skin and skeleton;
static model components reject them. Runtime `Model3D` pointers, mesh/texture handles,
animation players and physics bodies are never serialized.

Binding is explicit after instantiation, hot reload or an asset-reference edit.
It performs synchronous loading on the render thread, with no per-frame filesystem
polling or extra owning cache. It binds enabled runtime entities, copies inherited
components before mutation, and leaves prefab definitions untouched. Empty references
leave programmatically supplied models alone. Missing or incompatible assets produce
entity/component/path diagnostics; fix the content and call again to retry.
For changed content at the same path, call `assets.clear()` before resolving. Skinned
model replacement resets animation/IK runtime state and retains authored settings;
bind before animation/physics setup, and rebuild live ragdolls when skeletons change.
Unload scenes through the existing lifecycle; clear submitted `Scene3D` draws and
collect again to release render buckets. `Assets3D` itself keeps only weak resources.

Per-part material variants use `ModelMaterials3D`; see
[material authoring](#authored-material-overrides-and-variants).
Ropes and ragdoll
descriptions remain C++ configuration.
Referenced collider recipes round-trip as asset paths; raw runtime mesh/shape handles
remain C++ state. See [physics authoring](physics_3d.md#authored-collision-and-characters).
Edits to live collider parameters use the existing physics `rebuild(entity)` contract.
GPU resolution is an explicit host step after the existing scene hot-reload utilities.
`build_scene_asset` now preserves scene-loaded prefab references and minimal overrides;
see [linked instance saves](ecs_prefabs.md#saving-linked-instances). Explicit unpacking
is available for unsupported edits. Composed references and structural overrides now
support child/component additions/removals, renames and internal reparenting; see
[prefab composition](ecs_prefabs.md#composition).

### Authored camera and environment settings

Add a `SceneCamera3D` and `Transform3D` to a camera entity. It looks down local -Z,
including parent rotation and scale. Projection is `perspective` or `orthographic`;
`fov` is vertical degrees, `height` is the orthographic world height, and `far: 0`
represents infinity without writing nonfinite JSON. Add `SceneEnvironment3D` to an
entity for scene-wide settings; it requires no transform. Fields are flat inspector
properties, for example:

```json
{"SceneEnvironment3D": {
  "sky_enabled": true, "sky_lights_scene": true, "sky_turbidity": 3,
  "fog_enabled": true, "fog_density": 0.002, "fog_color_from_sky": true,
  "shadow_min": [-100,-20,-100], "shadow_max": [100,100,100],
  "shadow_cascades": 4, "shadow_distance": 100,
  "hdr": true, "exposure": 1, "bloom": 0.1
}}
```

After transform propagation, call `apply_scene3d_settings(world, scene, &view_options)`
before collecting local lights/draws. Use the returned view options when creating
the view. View options are fixed at creation in the current API: recreate the view
after editing them. Camera/environment selection independently
chooses the enabled entity with highest priority, then lowest ID. No matching
component leaves host settings alone. Invalid projection ranges or environment
budgets return diagnostics and leave that component's previous settings intact.
Existing local light lists are retained. `WorldRenderState3D::apply_time_of_day`
can then update sun/sky/fog from an authored `TimeOfDay3D`, after its clock is advanced.
This subset authors procedural sky settings; HDR panorama asset binding remains C++.

### Authored animation configuration

`AnimatorAsset3D{"asset": "animations/hero.kinanimator"}` accompanies the usual
`Animator3D` and authored `SkinnedMeshRenderer` (C++ `SkinnedMeshRenderer3D`). Its `kin.animator3d/1` JSON resource references
the existing `.kinanim` machine format instead of introducing a second graph parser:

```json
{
  "schema": "kin.animator3d/1",
  "machine": {"asset": "animations/hero.kinanim", "name": "locomotion"},
  "blend_spaces": {"ground": {"parameter": "speed", "entries": [
    {"clip": "Idle", "position": 0}, {"clip": "Walk", "position": 1.6},
    {"clip": "Run", "position": 4.2}
  ]}},
  "layers": [{"clip": "Wave", "mask_joint": "spine", "weight": 0.5, "additive": true}],
  "fades": [{"from": "ground", "to": "air", "seconds": 0.15}],
  "ik": [{"name": "hand", "kind": "two_bone",
          "joints": ["shoulder_L", "elbow_L", "hand_L"], "weight": 0,
          "fade_seconds": 0.12, "pole": [0.5,-0.7,-0.5], "max_stretch": 1.12}]
}
```

Machine paths, like model paths, are relative to the common content root. A machine
name is optional only when the file has exactly one machine. Skeletal machines use
`replace` transitions; `push` is rejected because the 3D animator has no override stack.
Blend entries are sorted and duplicate positions rejected. Layers also accept speed,
loop and include_root; fades support empty-name wildcards. IK kinds are `two_bone`,
`foot_placer`, `look_at`, `fabrik`; the initial schema covers named chains, weight/fade,
model-direction poles, stretch, foot/toe heights, toe joints and foot smoothing/tilt.
The complete working locomotion and foot/hand rig is in Summit's
`content/animations/survivor.kinanimator` and `survivor.kinanim`.

`resolve_render3d_assets` binds these after model loading. Tools can separately call
`resolve_animator3d_assets(world, content_root)` with no renderer. Configuration and
machine source reads are explicit, with a 1 MiB cap per source and bounded array sizes;
there is no per-frame filesystem work or persistent owning lookup cache. Consumers
share parsed configurations within a bind, including unchanged existing consumers.
Unchanged content retains playback progress. Invalid JSON/fields, graph references,
clips or joint names return contextual diagnostics and retain the previous configuration.
Clip/joint compatibility is checked once a runtime model is available.

Changed configuration resets playback/IK runtime while retaining base component
settings and gameplay parameters. Game code supplies parameters, targets and physics
queries; existing ground callbacks are retained by matching IK names during rebinding.
Specialized solver tuning, root-motion axis settings and gameplay callbacks remain
C++ hooks. Summit uses the authored machine, speed blend and three IK constraints;
its procedural fallback keeps the previous C++ setup.

## First person

`Window::set_relative_mouse(true)` captures the mouse: the pointer is hidden
and held, and `Input::frame_mouse_motion()` reports how far it moved since
the last rendered frame (window pixels). Turn the camera with it in
`render()`, so the view answers within the frame; the simulation reads the
look at its next step. `Input::mouse_motion()` is the same for `update()`.
Agents send motion with the server's `input.mouse {dx, dy}`.
[Signal Breach](../examples/signal_breach/main.cpp) does all of this.

## Sponza Walk

[examples/sponza_walk](../examples/sponza_walk) flies a camera round Sponza
(fetched by `tools/reference/fetch.py`) with every feature of the renderer on
a switch: lights, shadows (cascades), occlusion, contact shadows, temporal anti-aliasing, levels of detail, MSAA, HDR and bloom, and each
material feature (done to copies of the model's materials, so the model is
kept). Its settings are one struct applied to a `Scene3D` each frame, so a new
feature is a field and a row in its table of switches; `--set`, `--preset` and
`--headless --screenshot` use the same table. It warms the renderer up once at start (`Renderer3D::warm_up`, below) and has a switch for the shadow cache. See the
[examples README](../examples/README.md#sponza-walk). Its "Character IK" section drives a person of 25 joints (toes and a tail
among them): a hand to a target, the head to the camera, feet on the floor and a step, FABRIK on the tail to a green ball, joint
limits, foot locking, toes, and a person that drifts from side to side to see feet lock (`--preset ik`, `ik-reach`, `ik-tail`,
`ik-walk`, `ik-all`). What the panel does to the constraints is `examples/sponza_walk/ik_rig.hpp`, which a unit test drives. Its "Sky and fog"
section (P3a) has the sky, fog, time of day and the sky's light on switches (`--preset sky`, `dusk`, `fog`, `night`, `day-night`); its "Spot
lights and shadows" section (P3b) drops spot lights at the camera (J), carries a flashlight, hangs rings of spot and point lamps over the
model and switches their shadow maps, the moon's shadow and the budget (`--preset spots`, `flashlight`, `moon`, `lamps`).
Its "Global illumination" section and `--preset gi` enable probe GI, with voxel size, clipmap levels, rays,
probes per frame and intensity controls. GI remains opt-in in the feature viewer. The model bounds have an outer
probe guard, and the stats report whether GI is actually lighting the view. Allow a few seconds to settle.

## Cameras

`kin::Camera3D` is a position, a rotation and a projection
(`Projection3D::Perspective` with `fov`, or `Orthographic` with `height`, in
world units), `z_near` and `z_far` (infinite by default).
`Camera3D::look_at(eye, target)` and `Camera3D::orbit(target, yaw, pitch,
distance)` make one.

Between the view and 2D, in the coordinates of the rectangle the view is laid
into: `camera.project(world, rect)` (where a point shows, for labels and
health bars) and `camera.ray(point, rect)` (what a click points at;
`kin::hit_plane_y` meets it with the ground).

`camera.clip_plane` (a `Vec4f`: a point p is drawn where `dot(xyz, p) + w >= 0`; the default `(0, 0, 0, 1)` keeps
everything) cuts the view at a plane: what a mirror's or a portal's view needs (below). It is clipped in the vertex
shaders (`gl_ClipDistance`, which SDL_GPU enables on every backend), so the depth stays the camera's own and ambient
occlusion, fog, contact shadows and temporal anti-aliasing work as ever; the copies wholly behind it are culled on the
CPU as those out of view are (`stats().culled`; the GPU cut culls by the six frustum planes and leaves the seventh to
the vertex shaders). Shadows are not clipped (a wall behind the mirror still shades what is in front of it), and the
sky shows where nothing is drawn. `kin::plane_through(point, normal)` makes one. WebGL2 does not clip; it says so once.
`camera.hidden_id` (a `MeshInstance3D::id`; 0: none) leaves the copies with that id out of the camera's picture, their
shadows kept: a shiny thing in its own reflection probe's capture.
A draw can be left whole by it: `scene.draw_instanced(mesh, copies, material, clip)` with `DrawClip3D::whole` (never
cut nor culled by it) or `DrawClip3D::whole_if_cut` (the same, and drawn only by cameras that have a clip plane: the
copy appears in the pictures alone); draws merge only with draws that say the same (`Scene3D::Draw::clip`). WebGL2
leaves out the `whole_if_cut` draws, as it cuts nothing.

## Mirrors and portals

A mirror, still water or a portal is a surface that shows a picture of the scene from somewhere else: a second view
(`View3D`), rendered from a camera of its own each frame before the view that sees the surface, whose texture the
surface's material reads where each of its points shows in that picture. Kin gives the cameras and the surfaces; the
game owns the views (`examples/planar_views`: a mirror, a basin of rippled water and two portals; `planar_atrium`: the
same as an authored scene, `examples/planar_views/content/scenes/atrium.kinscene`, with material graphs and a Lua
script flying an orb through the portals).

```cpp
// Once: a view for the reflection (half size is plenty), and the mirror's material.
kin::View3D reflection = r3d.create_view(size / 2, {.samples = 1}).value_or({});
kin::Material3D mirror{.color = kin::colors::white, .metallic = 1, .roughness = 0.045f};
mirror.surface = planar_reflection;            // engine/shaders/planar_reflection.surface.glsl, built (below)
mirror.emissive_texture = reflection.texture(); // its picture (`emissive` stays 0)

// Each frame, from the camera that sees the mirror.
const kin::PlanarView3D seen = kin::mirror_view(scene.camera, kin::plane_through(at, normal), aspect);
kin::planar_view_params(mirror.shader_params, seen.projector);
// ... draw the scene, the mirror with `mirror` ...
const kin::Camera3D eye = scene.camera;
scene.camera = seen.camera;
r3d.render(reflection, scene); // first: the picture
scene.camera = eye;
r3d.render(view, scene);       // then the view that sees the mirror
```

- **`mirror_view(camera, plane, aspect, offset = 0.005)`**: the camera's reflection in the plane (behind the mirror,
  looking out through it; the camera's side of the plane is the front, whichever way the plane faces), with its
  `clip_plane` at the mirror, `offset` (world units) in front of it so the mirror's own surface is not in its picture.
  `projector` takes a point of the mirror to that view's clip space. A camera cannot turn a picture inside out as a
  reflection does: this one sees the reflection flipped left to right, and the projector reads it the right way.
- **`portal_view(camera, entry, exit, aspect, offset = 0.005)`**: what the camera sees through the portal `entry`
  (`PortalPose3D`: a position and a rotation; the surface in its XY plane, its front its +Z): the camera taken through
  the portals (in at the entry's front, out of the exit's front, turned half round), clipped at the exit, and the
  projector for the entry's surface (the same as the eye's own projection, for a portal). `portal_point` and
  `portal_rotation` do the same to anything else that goes through (a body, a bullet, the player's camera when it
  crosses).
- **The surfaces.** `engine/shaders/planar_reflection.surface.glsl` (a mirror, water, a polished floor: the picture is
  the surface's reflection, in place of the environment's or the probes', weighed by its Fresnel, roughness and
  specular occlusion as theirs would be; its highlights stay. Water reflects little head on and much at a glance; a
  metal all of it; past a roughness of 0.4 it gives way, by 0.7, to the environment's, blurred on the way) and
  `planar_window.surface.glsl` (a portal or a screen: the picture is the colour of an `unlit` material). Each reads its
  picture from the material's emissive map and its parameters from `shader_params` (`planar_view_params(params,
  projector, distortion, strength)`: the projector's 16 floats, then `distortion`, world units the read moves with the
  normal map's bend, 0.25 for rippled water, and `strength`). They are not in the engine (a surface's variants are about
  a megabyte): a game builds the ones it uses, `kin_compile_surface_shader(spv OUTPUT_DIR <dir> SOURCE
  ${KIN_SHADER_INCLUDE_DIR}/planar_reflection.surface.glsl)`, embeds them and makes them with
  `create_surface_shader`, as the example does, or compiles them at run time with `compile_surface_shader`
  (development). A surface of its own can call the same functions (`kin_planar_reflect`, `kin_planar_show`,
  `kin_planar_uv`, `kin_planar_read`) with a picture and projector of its choosing, and `KinSurface::reflection` takes
  any reflection a surface computes.
- **A view's own picture.** A material whose map is the picture its view is drawing into reads white there (it may not
  read the texture it writes); with the clip plane's offset the mirror is not drawn in its own view anyway.
- **Planar surfaces in each other's pictures.** A mirror's picture is made for the camera that sees the mirror; seen
  in another planar view (a portal in the mirror, the mirror in the water), it would show the picture made for the
  main camera, which is wrong there. Draw planar surfaces with stand-ins in the planar views (the example: kin's own
  reflections for the mirror and the water, a dim film for the portals), or give such a pair a view of its own.
- **Portals in portals.** `portal_chain(camera, entry, exit, aspect, depth)` gives `depth` views, each through the
  entry again from the one before. View k draws the entry's surface with view k + 1's picture and projector (the last
  with a stand-in); render the last first. Two portals facing each other show their corridor to that depth. Stop a
  chain where `planar_on_screen(chain[k].camera, ...)` says the entry is out of view (the example does).
- **Going through.** `PortalPose3D::size` is the opening. `portal_crossing(entry, from, to)` says whether a step
  (a player's, a body's, a frame's) goes in through the entry's front within the opening, and how much of the step
  was taken before it: move the rest from the exit, `portal_point` for the place, `portal_rotation` for the facing
  and the velocity (the example's orb and its walking camera, `--camera 5`). `ray_through_portal(ray, entry, exit)`
  continues an aim, a bullet or a line of sight out of the exit (again for each portal it meets).
  `portal_straddle(entry, exit, centre, radius)`: while a thing is half-way through (its bounds closer to the entry's
  plane than its radius and the views' clip offset, its centre within the opening and its radius no larger than the
  opening's diagonal: not the wall or the floor round it), the transform to its image at the exit; draw
  it there too, before it is moved through and after. The part that has gone through then shows beyond the exit, and
  the part that has not still shows before the entry. Draw neither cut by the pictures' clip planes, or a closed mesh
  cut open shows the rim of its inside where the halves meet (a crescent, a thin ring, a small hole as it leaves): the
  image with `DrawClip3D::whole` and the thing again, where it is, with `DrawClip3D::whole_if_cut`
  (`scene.draw_instanced(mesh, copies, material, clip)`; see Cameras). Physics is the
  game's: a body half-way through is the game's to mirror at the exit (a second, kinematic copy) and to let through
  the wall round the opening.
- **Close up.** Kin's views clamp depth at the near plane rather than clip there, so a flat portal stays whole however
  close the camera comes. A camera that passes the plane before the game moves it through (an eye ahead of the body
  whose crossing moves it) has the flat surface behind it; draw the surface as the box `portal_surface_transform(entry,
  depth)` places (`meshes::box({1, 1, 1})`, its material `double_sided`): inside it the camera sees its faces, which
  read their own pixels of the picture (a portal's projector is the camera's own projection).
- **Which views to render.** `planar_on_screen(camera, aspect, centre, rotation, size)` gives where a mirror or a
  portal shows on screen (0..1), or nothing when it is out of view or seen from behind: its view need not be rendered
  then. **Only where it shows:** `Camera3D::window` makes a view of part of a camera's frame (an off-centre
  projection: what a camera of the whole frame would show there, its pixels as dense as the frame's at the view's size
  over the window's), and `crop_planar_view(view, window, aspect, mirrored, through)` cuts a planar view to it (a
  mirror's window turned left to right: its camera sees the frame flipped). A mirror covering a fifth of the screen
  then costs about a fifth of a view, as sharp as the view: `PlanarViews3D` does so, with the view's samples. Depth,
  the light grid, fog, ambient occlusion and culling follow the projection; the sun's cascades are fitted to the whole
  frame, and levels of detail to the window's denser pixels.
- **Cost.** Each view is a whole render: its shadows, ambient occlusion and the rest, as the main view's. A small view
  (`examples/planar_views --scale`) and the clip plane's culling (half the scene is behind a mirror on a wall) keep it
  down; a view not seen this frame (planar_on_screen) need not be rendered. `examples/planar_views`
  (a 1280 x 720 view, four planar views at 640 x 360, about 40 copies; RTX 4080 Laptop, `--bench 330 --profile`): each
  planar view about 0.16 ms of GPU time (median; the scopes' resolution is about 0.1 ms) and 0.03 to 0.15 ms of CPU
  recording, the main view 0.2 to 0.5 ms. Orbiting with portals three deep, 5.2 planar views a frame on average (3.2 one
  deep; those not on screen skipped): the whole frame's GPU time 2.4 ms (median), 2.1 one deep. Temporal
  anti-aliasing in a planar view keeps its own history; on the main view the picture moves as the surface does, so a
  reflection's parallax can smear for a frame when the camera moves fast.
- **Colours.** The picture is read as the material's maps are (decoded where the 2D pipeline keeps textures encoded).
  Give the planar views the main view's colour handling: both LDR, or both HDR in an HDR 2D pipeline (an LDR main view
  over an `hdr` planar view would tonemap the reflection twice). The portal's picture is fogged in its view; the
  window's unlit surface is fogged again at its own distance.


- **Water you see into.** `refraction_view(camera, plane, aspect)` is what is below a surface, seen by the camera itself
  (clipped just under the plane, so the surface is not in it), and `planar_refraction_params(params, projector,
  distortion, strength)` puts its projector into a planar surface's parameters; the reflection surface shows that
  picture (in the material's base colour map) as the light that comes up through the water: what is not reflected (1
  minus its Fresnel, water's 2% head on), bent by the ripples. Render the picture with a fog that thickens under the
  surface (a height fog, its base height the surface's) and what is below fades into the water's murk as it deepens.
  `Mirror3D` does all of it: `refraction` (0: a mirror), `refraction_distortion`, `murk` and `murk_depth` (about two
  thirds gone that deep, looking straight down); the atrium's pool shows its floor.

### Mirrors and portals on entities

`Mirror3D` and `Portal3D` (`kin/ecs/render3d.hpp`; authored in scenes and prefabs, shown by the inspector like any
reflected component) put them on entities: each is a surface in the entity's XY plane, centred on it, its front its +Z,
`size` wide and high times the entity's scale. `SceneSession3D::render` draws them and renders their pictures after
`compose`, given kin's planar surfaces (`SceneSession3DOptions::planar_surfaces`); a game that collects its own world
runs a `PlanarViews3D` (`kin/scene/planar_views3d.hpp`) before rendering the view: `planar.render(world, scene,
view_size, view_options)` draws the surfaces into the collected scene and renders the pictures they need.

```json
{"id": "pond", "components": {"Transform3D": {"position": [0, 0.3, 0], "rotation": [-0.7071068, 0, 0, 0.7071068]},
  "Mirror3D": {"size": [4, 4], "color": [10, 28, 34, 255], "metallic": 0, "roughness": 0.06,
               "normal": "textures/ripples.png", "uv_scale": [1.5, 1.5], "flow": [0.02, 0.013], "distortion": 0.25}}},
{"id": "west", "components": {"Transform3D": {"position": [-9, 1.4, 1], "rotation": [0, 0.7071068, 0, 0.7071068]},
  "Portal3D": {"size": [1.8, 2.8], "exit": "east", "depth": 3}}},
{"id": "east", "components": {"Transform3D": {"position": [9, 1.4, 1], "rotation": [0, -0.7071068, 0, 0.7071068]},
  "Portal3D": {"size": [1.8, 2.8], "exit": "west", "depth": 3}}}
```

- **`Mirror3D`**: `color`, `metallic`, `roughness` as a material's (a silver mirror by default: white, metallic 1,
  roughness 0.045; water: a dark colour, metallic 0); `normal` (an image of the content, read as linear data: ripples)
  repeated `uv_scale` times and moving by `flow` a second (Scene3D::time); `distortion`, `strength`, and `resolution`
  (its pictures' sharpness, a share of the view's: 1, as sharp, each covering only where it shows).
- **`Portal3D`**: `exit` (an entity: the portal it opens onto, or any entity, whose placing is the exit's), `depth`
  (levels of portals in portals), `resolution`, and `stand_in` (an unlit colour: its deepest level, and what it shows in
  other planar views' pictures, and everywhere without the planar surfaces).
- **What renders.** A mirror's picture when it is on screen, of where it shows (two pixels round it, in steps of 64 so
  that its view keeps its size while it moves a little); a portal's levels while the level before sees it, each of
  where it shows within the level before, deepest first; each with the other planar surfaces as stand-ins (a mirror as kin's own metal) and without the scene's overlay
  (`Scene3D::show_overlay`: a first-person scene's hands are not in its mirrors). Views are kept from frame to frame and
  dropped with their entities. `PlanarViews3D::views_rendered()` and `views_skipped()` count them. Whatever is
  half-way through a portal is drawn at its exit too (`portal_straddle`, `Scene3D::draw_again`): every copy of the
  scene's draws whose bounds straddle an opening, plain, skinned (a character walking through: its pose as it is) or an
  instance batch's, with its id, whole, and an opaque one again where it is, whole, in the pictures only (`DrawClip3D`).
  The atrium's survivor (Kenney's, CC0) walks through its portals (`planar_atrium --camera 5`).
- **Going through.** `portal_step(world, from, to)` (Lua `scene3d.portal_step(from, to)`) tests a step against every
  enabled portal: where it ends, out of the exit, and the turn to give its facing and velocity (`turn`, a quaternion),
  or nothing (nil). `portal_poses(entity)` gives an entity's portal and its exit as the camera3d.hpp helpers take them.
- **The surfaces.** `kin_planar_surfaces(<target>)` (`cmake/kin_embed.cmake`) builds both into a target, and
  `create_planar_surfaces(r3d, planar_surfaces)` makes them; `compile_planar_surfaces(r3d)` compiles them now with
  glslc (development builds). Without them a session still draws the surfaces: mirrors as kin's metal, portals as their
  stand-in.

## Reflection probes

A reflection probe is the scene seen all round from a point, captured into a picture filtered as the environment is
(a level a roughness), which the glossy surfaces inside the probe's box reflect in place of the environment: a polished
floor shows the room's walls and windows, not the sky. The reflection is parallax-corrected: the reflected ray meets
the probe's parallax box (the room's walls) and the picture is read toward that point from where it was captured, so
the far wall shows where it is on the floor instead of as if it were infinitely far away.

```cpp
// Once: a capture (64 to 256 pixels a face is plenty), filled from the scene.
kin::ReflectionCapture3D hall = r3d.create_reflection_capture(128).value_or({});
r3d.capture_reflection(hall, scene, {0, 1.5f, 0}); // again when the room changes
// Each frame, with the lights:
scene.lights.reflection_probes.push_back({.position = {0, 1.5f, 0}, // where it was captured
                                          .extent = {8, 2, 6},       // the room's half extents
                                          .capture = hall});
```

- **Capturing.** `capture_reflection(capture, scene, position, {z_near, z_far, hidden_id, global_illumination})` renders
  six square 90-degree views (as `render` draws them: sky, lights, shadows, fog, the probes already there; without
  temporal anti-aliasing or the overlay) into a panorama, in world space. Global illumination is a view's own and
  settles over frames, so a capture has its own none: `global_illumination` (a `View3D*`) lends it that view's, its
  probes as they stand (read, not moved; round that view's camera, so a probe within their reach is lit as the view
  sees the scene lit; not its area lights). `global_illumination_lit(view)` says whether that view's GI has light yet.
  It costs six renders at the capture's side; `debug_stats().reflection_captures` counts them
  (`reflection_gi_captures` those lit by a view's GI). The renders that use a capture filter it into a layer of the
  renderer's reflection array once a capture (`reflection_filters`).
- **The boxes.** `extent` is the influence box's half extents, round `position + box_offset`, turned by `rotation`; a
  surface inside it reflects the probe, fading out over `blend` (world units) inside its faces toward the probes it
  overlaps or the environment. `parallax_extent` (zero: the influence box's) is the box the picture is taken to lie on;
  `parallax = false` reads it as if infinitely far, as the environment is. Where boxes overlap, the higher `priority`
  first, then the smaller box: each takes its weight of what the ones before it left. `reflection_probe_weight` and
  `reflection_probe_direction` do the shader's arithmetic on the CPU.
- **What reads them.** The stock lit shaders (the light variants: a scene with probes draws with them, as with global
  illumination), material graphs and surface shaders; custom material shaders keep the environment. A mirror's or a
  portal's picture still wins where it shows (`KinSurface::reflection`), and with global illumination the probes' light
  replaces the GI's specular where their boxes reach.
- **Diffuse light.** A probe lights what is inside its box too (`diffuse`, on by default): each surface by what the
  probe saw all round, facing its way (its picture's irradiance, a level more in its layer, as an environment's), in
  place of the environment's or the hemisphere's ambient light. Global illumination's probes still win where they have
  light. `diffuse = false` keeps a probe to reflections (a shiny thing's own probe, say, where the room's light should
  stay the room's).
- **Baked.** `save_reflection_capture(capture, file)` writes a capture's panorama as a Radiance `.hdr` picture (2:1,
  linear, the layout environments use) and `load_reflection_capture(path)` makes a capture of one, through the content
  folder or pack: a scene ships its probes' pictures instead of capturing them at load (six renders each). Any 2:1
  panorama will do (a room rendered elsewhere). A loaded capture can be captured again.
- **Curved mirrors.** A shiny ball, a vase, a helmet: a probe at its centre captured every frame, without parallax (the
  centre is the right point to see the world from), with the thing left out of its own picture (`{.hidden_id = id}`,
  its copies' `MeshInstance3D::id`: `Camera3D::hidden_id`), a small box round it and a higher priority than the room's.
  Two shiny things see each other a frame late. No self-reflection (a concave shape does not see itself); close to a
  large curved surface the reflection's parallax is off. `examples/planar_views` (its chrome ball, `--probes off` to
  compare) and the atrium's polished statue do it.
- **Costs** (RTX 4080 Laptop): a capture of six 64-pixel faces, about 0.4 ms of the GPU; filtering it into its layer
  (the next render that reads it) about 0.3 ms more at the sky's 256 x 128. A probe captured once costs that once; a
  live one every frame.
- **Limits.** At most `max_reflection_probes3d` (16) a render: those whose boxes reach the view, by priority, then the
  nearest. Their layers are the environment's specular size (256 x 128 without one; a 1024-wide environment makes each
  probe's layer about 5.6 MB with its levels). WebGL2 has none.

### Reflection probes on entities

A `ReflectionProbe3D` component (`kin/ecs/render3d.hpp`; scene files, Lua's component access) makes the entity a probe,
captured from its place, its boxes round it (plus `box_offset`), turned and scaled as it is. `SceneSession3D` captures
and places them (`ReflectionProbes3D`, `kin/scene/reflection_probes3d.hpp`, for a game that renders `scene()` itself),
before the mirrors' and portals' pictures, which reflect them too. A probe is captured when it first shows, when it
moves, when `recapture` changes, every `interval` seconds (of `Scene3D::time`) or `every_frame`; `hide_self` leaves the
entity's own copies out of its picture (a shiny statue's own probe). With global illumination on, the session's view
lends its GI to the captures, and a probe captured before that GI settled (`ReflectionProbes3D::gi_settle` updates of
light, 60) is captured once more when it has. With `baked` (a picture in the content) it is loaded
from there instead and captured only when `recapture` changes; `ReflectionProbes3D::bake(world, scene, content)` captures
every probe, writes `probes/<authored id>.hdr` and sets their `baked` (save the scene to keep it; `planar_atrium
--bake-probes` does it for the atrium):

```json
{"id": "hall_probe", "components": {"Transform3D": {"position": [0, 2, 0]},
  "ReflectionProbe3D": {"extent": [11.5, 3.5, 10.5], "box_offset": [0, 1, 0.5], "parallax_extent": [11.5, 3, 10.5]}}},
{"id": "chrome_statue", "components": {"Transform3D": {"position": [6.5, 1.75, 5.5]},
  "ModelRenderer3D": {"asset": "models/sphere.glb"}, "ModelMaterials3D": {"asset": "materials/chrome.kinmaterials"},
  "ReflectionProbe3D": {"extent": [0.6, 0.6, 0.6], "parallax": false, "every_frame": true, "hide_self": true,
                        "resolution": 64, "priority": 1}}}
```

## Screen-space reflections

`scene.ssr.enabled` traces each pixel's mirror reflection through the depth pre-pass (on a grid half the view's size)
and reads what it meets from the previous frame's picture: a polished floor shows the columns standing on it, a wet
road the cars, where they are on the screen. They take over from the reflection probes and the environment where the
trace finds something; a mirror's or a portal's picture still wins where it shows.

```cpp
scene.ssr.enabled = true;       // authored: SceneEnvironment3D's "ssr_enabled" (and ssr_max_roughness, ...)
scene.ssr.max_roughness = 0.8f; // blurred more as surfaces roughen; rougher ones keep the probes' or the environment's
scene.ssr.thickness = 0.3f;     // how deep (world units) what the pre-pass sees is taken to be
scene.ssr.max_distance = 40.0f; // how far a ray goes
scene.ssr.steps = 48;           // marching steps a ray (8 to 128), then a refinement
```

- **How.** A compute pass after the pre-pass (which runs for it) marches each grid texel's reflected ray, its steps
  growing with the distance, through the pre-pass's depth; where the ray passes behind what it holds by less than the
  thickness it has hit, and five halvings find where. It keeps how far the hit is, not its colour. After each frame's
  temporal resolve the frame's picture is kept as a pyramid (half size, then each level half the one before; linear,
  or decoded where the view's output is sRGB) in the same buffer. The main pass's stock lit shaders take the hit
  distance of the grid texels round each pixel that traced its own surface (by their depth: glass and the overlay, not
  in the pre-pass, get none), follow the fragment's own reflection (its normal map's) that far, and read the previous
  picture there at the pyramid level as wide as the reflection's lobe is at that distance: a rough surface reflects a
  blurred picture. With temporal anti-aliasing each pixel starts its march at a different point (averaged over
  frames); without, all start alike (no noise, some banding).
- **Where they fade.** Toward the screen's edges, toward the end of the rays, for rays that come back toward the camera
  (what they meet is behind it), as the surface roughens to `max_roughness`, where the surface's own shape turns fast
  across the screen (a small curved thing: its neighbours' hits are not its own), where the normal map turns the
  reflection more than about 20 degrees from the face's own (the hit's distance was found along that one), and wherever
  a thing's own reflection probe (one without parallax, captured from its centre: a chrome ball) reflects it.
- **Costs.** About 0.2 to 0.25 ms of the GPU at 1280 x 720 (RTX 4080 Laptop), the pre-pass it needs included; a buffer
  a view of 12 bytes a grid texel plus about 11 for the picture's pyramid.
- **Limits.** Only what is on screen, as last frame showed it (what just appeared reflects a frame late; what is off
  screen is the probes'). A normal map's detail is followed with the face's hit distance: close for bumps, off for steep
  ones (which fade to the probes). The stock lit shaders, material graphs and surfaces (the light variants: a scene
  with them draws with those); custom material shaders and WebGL2 have none.

## Views

`Renderer3D::create_view(size, {.samples, .background, .hdr, .exposure, .auto_exposure,
.bloom, .bloom_threshold, .pick})`: `samples` is MSAA (1, 2, 4 or 8, fewer where the
device has fewer); `background` shows where nothing is drawn (transparent lets
the 2D under the view show).

A view made with `pick` also keeps, a pixel each, the world point and the id
of the scene's opaque or alpha mesh drawn there (not wireframes, additive
glows or the overlay). `Renderer3D::pick(view, pixel)` reads one back: `hit`,
`id`, `position`. It waits for the GPU: what the frame has recorded so far is
submitted, and 2D drawn after it goes on in a new command buffer, as after a
read-back. Ask once per pixel per picture (a studio keeps the answer under a
still pointer), not every pixel every frame. A view that has not been rendered
since it was made or resized has nothing to ask: `pick` says no `hit` until the
next `render`.

`hdr` draws into a float picture that keeps light past white, then exposes
and tonemaps it (ACES: brights roll off, the darkest tones darken a little)
on its way to the 2D frame; an HDR 2D pipeline gets it linear and tonemaps
it with everything else. Over a transparent `background` a partly covered
pixel (an MSAA edge, a faint glow) is exposed and tonemapped as its own colour,
then given its coverage again, so an edge has the colour of what it belongs to.
An additive glow (a `Blend3D::Additive` mesh, an additive particle) over the
transparent background adds its colour and leaves the coverage at 0; it is
encoded like the same glow over geometry, so it is as bright.
`bloom` (its strength; it turns `hdr` on) glows
round what is brighter than `bloom_threshold`: emissive materials past 1,
lit by strong lights. The glow belongs to the view, so 2D drawn over it
(a HUD) stays crisp.

`auto_exposure` (`AutoExposure3D`, off by default) makes the exposure follow the picture, as eyes and cameras do:
walk from a dark room into the sun and the view is too bright for a moment, then settles. Each render meters the
view's HDR picture on the GPU before the bloom is added: a histogram of the log2 luminance of every 2 x 2 block
(128 bins over 28 stops), the darkest `low` (0.1) and the brightest past `high` (0.9) shares of the pixels left out,
so a lamp or a patch of sky does not darken everything. The exposure that brings the average of the rest to `key`
(0.18, mid-grey) plus `compensation_ev`, clamped to `2^min_ev .. 2^max_ev` (-4 to 6 stops), is approached
exponentially: `speed_up` (1 per second) while it rises (after going into the dark), `speed_down` (3) while it falls,
as eyes adapt to light faster than to dark. `centre_weight` (0 to 1) makes the middle of the view count more. The
result multiplies `exposure`, which stays as a manual factor. Nothing goes through the CPU: the adapted exposure
stays in a small storage buffer of the view's that the output shader reads, so there is no read-back stall, and three
small compute dispatches (two after the first render) are all it costs (`debug_stats().exposure_dispatches`). The
time between renders is `Scene3D::time`'s step when it moves forward (by at most a second), else the wall clock's
(at most a quarter of a second); a new view, and one after `reset_history()`, starts at its target. It works where
the view tonemaps (`hdr` or `bloom`, out to an 8-bit 2D pipeline; an HDR 2D pipeline tonemaps the frame itself) and
`capabilities().auto_exposure` is true (not on WebGL2, whose views have no HDR yet).
`Renderer3D::exposure(view)` gives the game the adapted exposure (the multiplier, 2^ev) without stalling: each render
copies it out, and it arrives once that frame's GPU work is done (a frame or two behind the picture; nullopt until the
first copy arrives, and for views without auto exposure). Use it for a HUD, or for gameplay that reacts to how
dazzled the player is.

`render(view, scene)` draws the scene into it, recorded between the 2D drawn before and
after; a view keeps its picture until rendered again, so a still model needs
no rendering each frame. `resize(view, size)` gives it a new size and texture.
`view.reset_history()` drops the history temporal anti-aliasing keeps.

`Renderer3D::capabilities()` says what the device has (depth bits, most
samples, which passes; `software` for a software rasteriser such as lavapipe,
whose pictures are right and whose milliseconds mean nothing: tests compare
timings only where it is false); `stats()` summarizes the last render (draw calls, instances, culled
copies, shadow draws/casters, lights, triangles and CPU recording time).
`debug_stats()` adds per-cascade counts and renderer implementation counters.

## Memory and warm-up

**Load time.** Levels of detail and cluster hierarchies are built at load, on the CPU, which for a huge mesh is the longest part of
loading (the kiln: 5 s, and the peak of the process's memory). A bake cache (`r3d.set_bake_cache`, see [Baked cache](#baked-cache)) reads
them back on later runs instead: the same meshes, without the simplifier's time or memory.

**Targets of the opt-in features are given back.** Ambient occlusion's and contact shadows' textures, the pre-pass's depth and normals,
temporal anti-aliasing's history and velocity, and occlusion culling's pyramid, history and occluder depth (with the renderer's second gathered
buffer of cluster indices) belong to a view and are made by the first render that asks. A feature that has not been used for 120 renders of the
view gives its targets back (they go to SDL_GPU's allocator, so they are reusable at once, though a driver may keep its blocks); a feature that is
on again makes them again, and the pictures are those of a fresh view (the temporal history starts afresh, as after `reset_history()`). A
feature toggled at least every 119 renders never lets go, so toggling does not thrash. `stats().feature_bytes` is the video memory those targets
hold now (1280 x 720, everything on: Sponza 45.7 MiB, the kiln with occlusion culling 93.2 MiB; nothing when off) and `debug_stats().targets_released`
the groups given back this render. Nothing else is released (the shadow map, MSAA and HDR targets stay).

**Warm-up.** Pipelines are made on first use, which is tens of milliseconds a pipeline on a driver with a cold shader cache (turning on temporal
anti-aliasing for the first time took 390 ms, ambient occlusion 40 ms; a warm cache, 3 to 6 ms and 1.4 ms). `r3d.warm_up(view, scene)` makes, without
drawing, the pipelines the scene's draws and its settings (shadow maps and cascades, ambient occlusion, contact shadows, temporal anti-aliasing,
occlusion culling, custom shaders, the overlay, picking) would make on the first render, and the targets of those features for the view, so
that the first frame with a feature is a steady one (`stats().pipelines_made` is 0 for it). Call it behind a loading screen with a scene that has
the features on, or when a game turns one on. Nothing is warmed at start-up by itself. It does not make cluster cut buffers or occlusion culling's
history: the first frame with levels or clusters on still takes about 2 ms, and with occlusion culling about 6 ms, for those buffers.
Sponza Walk warms everything up once when it starts and again when a switch that needs it is turned.

## Cost

From `kin_draw_bench 200 N meshes` (RTX 4080 Laptop, 1280x720, 4x MSAA): an
instanced draw of 20,000 boxes takes about 0.2 ms of CPU in `render()` and
0.3-0.5 ms of GPU; 20,000 separate draws about 85 ns each. Instances are
written straight into the upload memory, on the renderer's job system above
8,192 of them (`renderer.set_job_system`). Instance what repeats.

A mesh's vertices are written straight into the upload memory as they are
converted, in 8 MB pieces (a 5-million-triangle model goes up without a
staging buffer its size). kin-render-studio's kiln (5.15 million triangles)
draws in 6-8 ms at 1280x800 with 4x MSAA, against 70 ms or more on the CPU
(see the plan's M4).

Morph targets are supported (see Morph targets below); the plan's later stages list what else is not yet.


### Clusters

`r3d.create_mesh(data, {.clusters = true})` (or `load_model(path, {.clusters = true})`) builds for one huge mesh a hierarchy of
clusters of at most 128 triangles (`kin/render3d/mesh_clusters3d.hpp`; `levels` is then ignored): leaves cut from the mesh, then
rounds of grouping about 4 neighbouring clusters, simplifying the group to half with its outer border locked, and splitting the
result, up to one cluster (open meshes stop at a few roots). The mesh is welded to share its vertices (by every attribute; a
flat-shaded one, as a model with no normals, by position, UV and colour, and its normals are then made in the shader from the screen-space
derivatives of the position: a normal map has no effect on it). `Mesh3D::cluster_count()`, `cluster_levels()`, `cluster_roots()`,
`leaf_triangles()`, `cluster_triangles()` read it back. The kiln (5.15 M triangles) takes 5 s at load and 1.5 GB for the process, less than
with no hierarchy, since the welded vertices replace the unindexed ones on the GPU. The mesh keeps on the CPU only the
hierarchy (`MeshClusters3D`, what the cut reads): the welded vertices are uploaded and let go (`MeshClusterBuild3D`).

With `scene.lod.enabled` each copy is cut through its hierarchy every frame (in compute on the GPU where the device can, else on the CPU: see below): a cluster is drawn when its own error spans at
most `error_pixels / bias` pixels at the nearest point of its bounding sphere and its parent's does not, so near parts stay fine
and far parts are coarse inside one mesh, with no cracks (the borders between groups were locked in every simplification above them) and every
surface covered once. `scene.debug.force_level = N` draws the cut at hierarchy depth N (0: the leaves). The same triangles are drawn by every pass for a copy
(colour, pre-pass, velocity, pick, each shadow map; casters outside the view that cast into it are cut for the shadow only). The indices
of what is drawn are compacted into one buffer a frame; one draw per copy per pass. `debug_stats()` has `cluster_copies`, `clusters_tested`,
`clusters_drawn`, `cluster_upload_bytes`, `cluster_ms`. Off (the default) a mesh with clusters is drawn whole and the picture is that of the mesh
without; alpha-blended copies are never cut. Clusters pay for one huge mesh seen near and far (the kiln close up: 0.4 against 2.3 ms of GPU for whole-mesh levels, 4.4 at
full detail); not for many copies of small meshes, nor mid-size ones.

**The cut on the GPU** (`scene.debug.gpu_culling`, default on; `RendererCapabilities3D::gpu_driven_clusters`). A cluster's test needs no neighbour, so
four compute passes test every cluster of every copy at once: the cut and the frustum tests of each pass (the camera's, each cascade's or the one
shadow map's: the same expressions as the CPU's, in the same order), a prefix sum of the chosen clusters' triangle counts, and a gather of their
indices into one index buffer in the clusters' own order (so a picture is the same every run), which indirect draws read: one multi-draw a draw and pass,
the copy's slot in the instance buffer as the first instance. The CPU only lists the copies (0.03 to 0.08 ms of `render()` on the kiln,
against 0.4 to 0.9 for the CPU cut). The picture is the CPU cut's, byte for byte (tested with 1, 3 and 4 cascades, occlusion, contact shadows, temporal
anti-aliasing, picking). It costs video memory: the table of clusters (80 bytes each) and every level's indices (the kiln: 62 MB more than the leaves
alone, 7 MB of table), and the gathered indices (`scene.lod.max_cluster_triangles`, default 4 million triangles, 12 bytes each, shared by all
passes and copies). Before selecting clusters, each submitted draw reserves the upper bound of its finest complete
cut for all camera/sun views. If it cannot fit, that draw uses its complete level-zero mesh across colour, depth, motion,
picking and shadows. This first conservative policy can use more geometry than the requested detail budget, even when
an actual coarser cut would have fit. It preserves coverage without synchronous GPU readback; `cluster_budget_fallbacks`
counts submitted fallback copies and `cluster_budget_reserved_triangles` reports the reservation. Both CPU and GPU cuts
use this policy. Budget zero selects complete meshes. A future per-object GPU overflow path can reduce conservative
fallback costs. Allocation failure remains an operation failure, not permission to omit geometry. The counts of what it drew are on the GPU:
`scene.debug.stats_readback` reads them back at the end of `render()` (waiting for the GPU, as `pick` does: for tools and tests) into `debug_stats()`
(`clusters_drawn`, `triangles`, `cluster_overflow`, `cluster_triangles_all`); without it the GPU-only counters are 0, `triangles` excludes GPU-selected geometry (`triangles_complete` is false), and `draws`, `shadow_draws` count the multi-draws.
`Renderer3D::read_cluster_picks` lists the clusters it chose. `debug_stats().cluster_gpu` says which cut a render made.
`scene.debug.gpu_culling = false` selects the CPU cut. Alpha and additive copies are never cut. The cut runs on the Vulkan and Metal drivers
only: its indirect draws carry the copy's slot as the argument's first instance and the vertex shader reads `instances[first + gl_InstanceIndex]`,
which Vulkan's `gl_InstanceIndex` and Metal's `instance_id` include and Direct3D 12's `SV_InstanceID` does not (SDL_GPU promises nothing), so on
any other driver `gpu_driven_clusters` (and `occlusion_culling`) is false and the CPU cuts, with one line in the log.

**Occlusion culling** (`scene.lod.occlusion = {.enabled = true}`; off by default; `RendererCapabilities3D::occlusion_culling`). Clusters hidden
behind other geometry are not drawn. It needs the selection, the GPU cut and clustered meshes (else it is ignored, with one line in the log), and the picture
is the one without it, to the byte in every test (a slit narrower than a pixel between two pieces of wall can be seen through by a multisampled picture, and
not by the depth the test reads: 78 bytes of a 160 x 120 picture in the one case measured). It is the standard two-pass method: the clusters
that showed last frame (a history of one word a cluster, per view) are drawn first, with the frame's opaque meshes, into a depth; a pyramid of its farthest
depths (three compute passes) is built; every other cluster is tested against it, this frame's own depth, so that what comes into view
(a turn, a door, a moving copy) is never culled; the rest is drawn. A wrong history only costs speed: `View3D::reset_history()`, a resize and
a change of the scene's clustered draws start it empty. With a pre-pass (ambient occlusion, contact shadows or temporal anti-aliasing on) the pre-pass is made in
two halves and nothing is drawn twice; without one a depth-only pass of the first set is the price, because the multisampled main depth cannot be
sampled. The shadow maps are not culled (a caster hidden from the camera still shadows what shows), nor the overlay, and meshes with a shader of their own are no occluders.
`depth_margin` (relative, 0.001) and `pixel_margin` (1) widen the test a little. `debug_stats()` has `occlusion`, `occlusion_levels`, `occlusion_dispatches`
and, with `stats_readback`, `clusters_first_pass`, `clusters_occluded`, `triangles_occluded`, `clusters_wasted` (drawn first, found hidden after all);
`clusters_drawn + clusters_occluded` is the cut's clusters in view. It adds fixed work (the pyramid, a second cut, a second multi-draw a draw) and wins only
where much more is hidden than that costs: see the plan's G3d for the numbers (a wall hiding 256 copies of a 100 thousand triangle clustered mesh:
12 to 19% of the GPU time saved; the kiln and Sponza: nothing to save, 0.03 to 0.12 ms spent). Copies drawn from CPU instance lists
(`draw_instanced` of meshes without clusters, the crowd) are not tested.

### Owning a 3D scene session

`<kin/scene/scene_session3d.hpp>` adds `SceneSession3D` over the existing
`SceneAssetRuntime`, component registry, `Assets3D`, rendering and physics bridges.
It owns one isolated ECS world, render state and view, plus an optional physics world.
The renderer and its `Assets3D` must outlive the session and any retained resources.
All operations run synchronously on the renderer thread.

```cpp
kin::Assets3D assets{renderer3d, content_root};
kin::SceneSession3DOptions options;
options.viewport = {960, 540};
options.view.samples = 1;
// Optional: no physics world or scratch pool exists by default.
// Set contact_events = true in the configuration when gameplay consumes PhysicsContacts3D.
options.physics = kin::World3DConfig{
    .max_bodies = 256, .max_body_pairs = 1024,
    .max_contact_constraints = 1024, .temp_memory_bytes = 2 * 1024 * 1024};
options.register_components = [](kin::EcsWorld& world) {
    // Register the game's native/data components and systems here.
};
options.initialize = [](kin::SceneSession3DContext& context) {
    // Assets are bound; add game state/controllers before physics bodies exist.
    // context.instance maps authored IDs. context.dependencies can include custom files.
};
kin::SceneSession3D session{renderer3d, assets, options};
auto loaded = session.load("scenes/camp.kinscene");
if (!loaded) return report(kin::diagnostics3d_text(loaded.diagnostics));
// A failed replacement leaves the previous scene usable.
// In the host's fixed-step loop: check session.step(fixed_dt).
// In its render loop, composite only after a successful render:
auto rendered = session.render(alpha);
if (rendered) renderer2d.draw_texture(session.view()->texture(), {0, 0, 960, 540});
else report(kin::diagnostics3d_text(rendered.diagnostics));
```

The complete camp scene lives in `examples/summit/content/scenes/camp.kinscene`.
It combines existing free Kenney props, the animated survivor prefab, a camera,
procedural environment and a primitive floor collider. It is a standalone loading
example; Summit's expedition retains its game-specific presentation loop.

`load` is `prepare` followed by `activate`. For a frame-boundary handoff, call them
separately: preparation validates a candidate world, binds assets/configurations,
runs initialization, validates settings and creates physics bodies before activation.
Parse, dependency, texture/upload, initialization, settings and body-creation failures
discard the candidate and retain the active world's simulation state and view.
`cancel` releases the candidate; `unload` releases both worlds. At most one candidate
is retained. A new preparation replaces any previous candidate.

`generation()` changes on activation/unload. Entity handles and world/render/physics
pointers from the old scene become invalid after the swap: reacquire entities with
`entity(authored_id)`. Reload starts from authored state. Carrying player/save state
between worlds is an explicit game policy implemented through initialization.
Initialization must mutate the supplied candidate; external side effects are not
rolled back. Throwing rejects preparation. `compose(context, scene)` adds game draws
or settings after stock collection and also runs during preparation; keep it free of
external side effects and create/bind entities in initialization instead.

`step(dt)` interleaves one ECS frame with native simulation at phase boundaries:
physics after Physics, fresh contacts in PostPhysics, then animation, time/weather
clocks, transforms and particles after Animation. Each native simulation advances
once. Lifecycle requests commit at the boundaries described below. `collect()` resets internal
draws/settings, propagates transforms, applies authored camera/environment and clocks,
collects local lights/models/ropes, then calls `compose`. `render()` collects and submits.
Custom gameplay ordering can use `world()`, `bodies()` and `rendering()` directly instead
of `step`. The host remains responsible for input, fixed-step scheduling, compositing,
UI and scene-stack enter/suspend/resume policies. A host `Scene::world()` can
return `session.world()` for the existing inspection tools. Scenes with a `script`
use the Lua workflow below through the same session and frame stages.

Audio is opt-in: give `options.audio` and `options.audio_catalog` (both outlive the
session) and each `step` runs the ECS audio bridge after the Audio phase's systems, so
the one-shots and emitters they add sound that step. Emitters and one-shots sound from
their `WorldTransform3D` (`offset_3d` in the entity's own space), and the listener is the
`AudioListener` entity's transform or, with none, the scene's active `SceneCamera3D`
facing its -Z ([audio](audio.md#runtime)). The host still calls `audio.update(dt)` once a
frame.

```cpp
options.audio = &audio; options.audio_catalog = &catalog;
options.initialize = [](kin::SceneSession3DContext& context) {
    context.world.entities().find_by_authored_id("campfire").set(kin::AudioEmitter{.cue = "crackle"});
};
```

A session also runs headless, over a renderer without 3D (`Renderer3D::available()` false:
the `SDL_Renderer` backend, a server under `AppMode::Headless`, a test job without a GPU).
Everything but drawing is the same: preparation and activation, spawning, preloading and
pools, edits, commands, physics and queries, animation, particles, time and weather, and
`collect()`, which builds the `Scene3D` on the CPU. Models load CPU-only through
`Assets3D::load_model`: their parts' names, transforms and skins, with empty mesh handles
that keep their counts, bounds and joints, and the skeletons and clips, so animation, joint
sockets and placement behave as with a GPU; parts keep default materials and no image is
decoded. There is no view (`view()` is null), ropes are not meshed, and `render()` and
`pick()` return false/empty with the diagnostic "no renderer". A session over a GPU
renderer takes exactly the path it took before.

Hot reload is explicit: call `changed_sources()` or `reload_if_changed()` on a development
timer, never implicitly in every render. It watches the loaded scene/prefabs, model
sources, decoded external image/buffer paths, animator/machine files and initialization's
custom dependencies. Deletions and mounted pack ownership changes are detected. Disk
polling uses modification times and sizes, so manual `reload(true)` is available for
edits that preserve both. Content packs must be reopened/remounted with the existing
content utilities; the session does not alter global mounts. Failed reloads keep their
old watch baseline and can be retried. Live field edits through `session.edit` or the
active authored members' registry patches extend dependency watching automatically.
Direct C++ mutations/structural changes still require explicit binding or a fresh reload.

Only affected model/texture lookups are invalidated, through `Assets3D::invalidate`;
existing consumers retain valid handles. Unchanged art and matching view targets are
shared during preparation. New targets alone are warmed up; activation resets temporal
history. Changing view size/options creates a replacement target, with temporary overlap.
Scene-specific physics scratch/capacity allocations also overlap during preparation.
To minimize peak memory when continuity is unnecessary, `unload` before `load`.
Retaining model/view handles outside the session deliberately prolongs their lifetime.

Defaults limit each scene/prefab source to 8 MiB, expanded authored entities to 10,000
and watched files to 4,096; configure `limits`/`max_dependencies` for the game. Animator
and machine sources are checked before reading against their separate 1 MiB caps.
These are authoring budgets, not a total GPU/model-memory allocator. There are no worker
uploads, resident scene caches, automatic reload jobs or hidden per-frame content reads.
The lower-level shared `SceneAssetRuntime::load` accepts the same optional scene/prefab
byte and expanded entity limits; its existing callers remain unlimited by default.

Authored collision recipes and character settings use the same component registry;
see [physics authoring](physics_3d.md#authored-collision-and-characters). Physics-enabled
sessions cook these CPU assets before body creation, watch their sources, and retain
the active world on failed reload. `camp_walk` hosts the playable authored camp.

### Lua gameplay in a 3D scene

`SceneSession3D` loads the optional `script` in a `.kinscene`, relative to the
content root, including scripts and `require` modules in mounted `.kinpak` files.
It reuses the Lua ECS API (`scene:authored`, `get`, `patch`, queries and
`register_system`) and the session's native stages. Do not also construct a
`ScriptScene` or run another ECS/physics frame loop for that world.

```json
{"schema":"kin.scene/1", "script":"scripts/game.lua", "entities":[]}
```

```cpp
options.scripts.input = &context.input;
options.scripts.app = &context.app;
// Optional scripts.bind adds host APIs to every fresh VM.
// Optional scripts.components supplies legacy typed component glue.
// Ordinary registered native/data components use the shared registry already.
```

These services and custom binding captures outlive the session. Missing input
reads false; `ctx.app:quit()` without an app reports a Lua error. Forward
`session.collect_actions(actions)` and `session.write_report(json)` from the host,
checking their boolean results. They run between frames. The host still owns
fixed stepping and view compositing.

```lua
local crate
function on_load(scene)
    crate = scene:authored("crate")
    -- Configure asset/collider/body fields before native bodies bind.
end
function update(ctx)
    if ctx.input:pressed("launch") then
        scene3d.impulse(scene3d.entity("crate"), {x=0,y=6,z=0})
    end
end
function collect_actions(actions) actions:add("launch") end
function write_report() return {height=crate:get("Transform3D").position.y} end
```

Lifecycle and failures:

- `on_load(scene)` runs in preparation after initial asset binding and the C++
  initializer, before body synchronization. Configure entities and register
  systems here. Native `scene3d` gameplay operations need activation.
- `on_enter(ctx)` runs against the candidate's bound physics during activation.
  Enter and validation must succeed before replacing the old world. Existing
  bindings are configured in load; enter can query physics, apply impulses and
  queue prefabs. Cancelled preparations never call enter.
- `update(ctx)` runs once per `step(dt)`, before ECS Input. Lua systems run at
  their declared phases; physics, animation, particles and audio advance through
  the same native stage boundaries as C++.
- `on_exit(ctx)` runs once on successful replacement/unload while old services
  live. Cleanup errors are logged and cannot veto teardown. Exit, reports and
  action collection cannot submit native prefab commands or impulses.
- Failed load/enter preserves the current world and VM. Failed update/system
  execution returns false with Lua diagnostics and cancels its uncommitted
  commands. Earlier gameplay/native effects are not rolled back. Custom bindings
  must avoid external side effects in preparation/enter when those effects need
  transactional replacement.

Runtime position/rotation and game-component writes use the reflected API.
Resource, collider/body setup, scale and particle capacity changes belong in
load-time configuration or explicit between-frame edits/reloads. Use the native
3D lifecycle for managed prefabs rather than deleting their members through the
raw ECS API.

The global `scene3d` table exposes checked gameplay operations:

| Function | Result |
|---|---|
| `available()` / `physics_available()` | Active scene / usable physics |
| `entity(authored_id)` | Checked scene-generation handle with `id` and `alive()` |
| `raycast(from, to[, {mask, sensors, ignore}])` | `nil` for a miss, otherwise `{entity, position, normal, fraction, sub_shape}`; entity is absent for unmapped native obstacles |
| `impulse(entity, vector)` | `true` for a live ordinary dynamic body, `false` for a stale/unavailable/non-dynamic target |
| `spawn(path_or_prepared_or_pool[, {position, rotation, scale, parent}])` | Deferred prefab request |
| `preload(path)` | Deferred preparation request; successful result is `request.prepared` |
| `create_pool(prepared[, {capacity=16}])` | Deferred fixed-capacity pool request; successful result is `request.pool` |
| `pool_stats(pool)` | Snapshot `{capacity, active, available, acquired, reused, exhausted}`, or `nil` for a stale pool |
| `release(prepared_or_pool)` | Deferred release request; release pools before their prepared prefab |
| `despawn(prefab_or_spawn_request)` | Deferred despawn request |
| `character_input(entity, command)` | Replace the controller command; `false` for a stale entity or absent native character |
| `character_state(entity)` | Last synchronized controller state, or `nil` |
| `contact_events([entity])` | Snapshot of the last native rigid-body contact batch, optionally filtered to either side |
| `character_events([entity])` | Snapshot of the last native character event batch, optionally filtered to that character |
| `set_animation(entity, options)` | Set base playback options; `false` for a stale/missing animator or unknown clip |
| `set_animation_parameter(entity, name, value)` | Set a boolean or finite numeric state-machine/blend parameter |
| `trigger_animation(entity, name)` | Set a trigger consumed by the authored state machine |
| `animation_state(entity)` | Playback configuration, current machine state, base time and `finished`, or `nil` |
| `play_action(entity, name[, mode])` | Checked playback handle, or `nil` for missing/invalid/busy actions; mode is `if_idle` (default) or `replace` |
| `cancel_action(action[, fade_seconds])` | Cancel that playback only; default `-1` uses its configured fade-out |
| `action_status(action)` | `playing`, `finishing`, `completed`, `cancelled` or `superseded` |
| `action_event_valid(action)` | Whether a marker from that playback still belongs to the actor's current action |
| `animation_events(entity)` | Marker snapshot array and dropped-marker count, returned as two Lua values |
| `shape({kind, ...})` | Reusable immutable convex query shape |
| `raycast_all(from, to[, options])` | Bounded hit array and overflow flag |
| `shape_cast(shape, position, displacement[, options])` | Nearest sweep hit or `nil`; also includes `shape_position` and `penetration` |
| `overlap(shape, position[, options])` | Bounded overlap array and overflow flag |
| `set_ik(entity, name, options)` / `ik_state(entity, name)` | Control an existing constraint / read its weight and target state |
| `traversal_input(entity, command)` / `traversal_state(entity)` | Replace traversal commands / read native grips, mode and resource |
| `set_traversal_resource(entity, value)` | Set the character's native resource (clamped to its configured maximum) |
| `start_ragdoll(entity[, options])` / `hand_over_to_ragdoll(entity[, options])` | Start body simulation / hand over a character controller |
| `blend_ragdoll(entity, alpha, seconds)` / `ragdoll_get_up(entity, seconds)` | Blend body influence / return to animation without clearance testing |
| `animate_ragdoll(entity)` / `set_ragdoll_subtree(entity, joint, weight)` | Restore animation / set a named subtree's body influence |
| `recover_from_ragdoll(entity[, options])` | Clearance-tested recovery result `{recovered, blocked, place}`, or `nil` |
| `ragdoll_state(entity)` / `ragdoll_events([entity])` | Native mode/body snapshot / non-consuming event batch |
| `rope_state(entity)` / `rope_points(entity)` | Native length/tension state / copied polyline |
| `rope_closest_point(entity, position)` / `rope_point_at(entity, segment, fraction)` | Closest sample / sample a one-based segment |
| `set_rope_length(entity, length)` / `reel_rope(entity, length, speed)` | Set the next Physics stage's target length, instantly / at metres per second |
| `rope_events([entity])` | Non-consuming break-event snapshot |

Vectors are finite `{x,y,z}` tables; optional rotations are unit `{x,y,z,w}`
quaternions. Prefab paths stay relative to the content root. Queries read the
last synchronized physics state and never step or synchronize it. Unavailable
physics queries, invalid arguments and stale ignore filters report Lua errors.
Handles expose no mutable body IDs and retain no world.

`character_input` replaces the whole `CharacterInput3D`, with defaults for omitted
fields; it does not patch the previous command. `move` is `{x,y}` (right/forward),
`forward` is a world `{x,y,z}` direction (default -Z), and `jump`, `crouch`, `sprint`
are booleans. The native controller clamps movement length to one. Jump is a held
level with native edge detection. Submit each fixed step, including a neutral command
when movement stops. Commands persist until replaced. The call does not step physics:
`update`/Input/PrePhysics commands affect that frame's native Physics stage; commands
submitted after Physics affect the next one.

```lua
local player, action
function on_enter(ctx) player = scene3d.entity("player") end
function update(ctx)
    local right = ctx.input:held("right") and 1 or 0
    local left = ctx.input:held("left") and 1 or 0
    assert(scene3d.character_input(player, {
        move={x=right-left,y=0}, jump=ctx.input:held("jump")
    }))
    if ctx.input:pressed("interact") then
        action = scene3d.play_action(player, "pickup") or action
    end
end
```

Character state includes `position`, `velocity`, `up`, `ground`, `ground_entity`,
`ground_normal`, `ground_velocity`, `ground_speed`, `vertical_speed`, `crouching`,
`stand_blocked`, `air_time`, `height` and `jumped`. Ground is `on_ground`,
`on_steep_slope`, `sliding` or `in_air`; `ground_entity` is absent for an unmapped
native obstacle. No physics service gives `false` for character input and `nil`
for character state, while animation remains available.

Read contacts once in a PostPhysics system to react to the current step.
`contact_events` requires the host to enable `World3DConfig::contact_events`;
otherwise its batch is empty. Persist records additionally require `persist_events`.
Character events do not require this option. Both event functions report a Lua
error without an active physics service or with a stale filter handle. They never
drain native events or retain a history. Rows and their nested vectors are copies;
editing a Lua snapshot cannot edit the simulation.

Contact rows contain `a`, `b`, `kind` (`begin`, `persist`, `end`), `sensor`,
`position`, `normal`, `closing_speed` and `point_count`. The normal points from
native side a to b; filtering does not swap sides. Character rows contain
`character`, `other`, `kind`, `sensor`, `position`, `normal`, `speed`, `jumped`,
`hand` and `reason`. Kinds use snake_case native event names, including `landed`,
`left_ground`, `hit_head`, `contact_begin` and `contact_end`; traversal reason codes
are the native numeric values. Entity fields are checked scene handles, absent
when their original native binding is gone or unmapped. A reused user ID does not
retarget an old event to a different body or pooled lease.

`set_animation` accepts `clip`, `speed`, `loop`, `playing`, `fade_seconds`, retaining
unspecified fields. It checks all options before applying them; invalid/nonfinite
values and unknown options raise a Lua error. Explicit clip selection requires a
compatible model clip and no authored state machine. For a machine, set parameters
and triggers instead. Changing `loop` updates the current base playback without
restarting its clock or its action. `CharacterAnimationLink3D` still writes its configured
locomotion parameters during Physics; use other parameter names or an appropriately
ordered system when providing an override. Animation mutation methods require an
active enter/update callback or system, like impulses. Declare writes to `Animator3D`
or `CharacterController3D` in systems that mutate them, and reads for their snapshots.
Successful immediate gameplay writes are not rolled back if a later callback fails.

Actions use the existing authored `.kinanimator` definitions and one native action
slot. Base locomotion continues underneath. Handles contain a checked actor and one
playback token, retain no actor/model/world, and cannot cancel a different actor's
action. Replacing playback supersedes its old handle; destroying/replacing the actor
also makes old handles superseded. Cancellation returns `false` when it cannot affect
that playback. A new action or reload does not reuse an old playback identity.

Read animation markers once in an Audio system, after native Animation. Register
one owner for each actor's gameplay marker handling; a native dispatcher may also
drain the batch. `animation_events` is a non-consuming snapshot of the current batch,
not a queue. Rows contain `channel`, `name`, `value` and, for action markers, `action`.
The native batch is bounded to 16 entries and reports overflow through the second
return value. Markers from superseded/cancelled actions are omitted. If an earlier
handler cancels/replaces an action, check each copied marker's ownership before use:

```lua
local events, dropped = scene3d.animation_events(player)
for _, event in ipairs(events) do
    if not event.action or scene3d.action_event_valid(event.action) then
        -- Handle the marker. This handler may cancel/replace an action.
    end
end
```

Requests have `status` (`pending`, `succeeded`, `failed`, `cancelled`, `invalid`),
`error`, `prefab`, `prepared` and `pool`. The last two are `nil` until their
respective preparation operation succeeds. Read `prefab.root` after spawn success; its checked handle becomes
stale after despawn/replacement. Lua systems submit through their scheduler
command buffer, including failure/discard rules. Enter/update use the host's
buffer. Commands execute at the normal Physics/PostPhysics/Animation/final
boundaries; submission does not imply that binding/capacity checks succeeded.

Script/module sources join explicit `changed_sources()` / `reload_if_changed()`.
Reload creates a fresh world and VM, resetting script locals; failed reloads keep
the last good version. Poll between frames; no background watcher is added.
Changes after preparation remain detectable after activation.

Build `lua_range`: WASD moves, Space jumps, Shift sprints, C crouches, E launches,
F plays the pickup action, R reloads and Escape exits. The C++ host in
`examples/lua_range/main.cpp` owns services/presentation; gameplay is
`scripts/lua-range.lua` and `scenes/lua-range.kinscene` under Summit content,
sharing its existing CC0 crate and survivor art. Headless runs automatically walk,
jump, launch crates and play an action; the report includes contacts and markers.
The script prepares and warms an eight-slot crate pool, then acquires/returns
leases; the report includes reuse and exhaustion counts.

Prepare explicitly, wait for successful request completion, then acquire instances:

```lua
local loading, warming, pool
function on_enter(ctx) loading = scene3d.preload("prefabs/crate.kinprefab") end
function update(ctx)
    if loading.status == "failed" then error(loading.error) end
    if loading.status == "succeeded" and not warming then
        warming = scene3d.create_pool(loading.prepared, {capacity=8})
    end
    if warming and warming.status == "failed" then error(warming.error) end
    if warming and warming.status == "succeeded" then pool = warming.pool end
    if pool and ctx.input:pressed("launch") then
        local request = scene3d.spawn(pool, {position={x=0,y=2,z=0}})
        -- Retain the request; scene3d.despawn(request) returns its lease.
    end
end
```

Preparation and warm-up may load assets and allocate; ordinary pooled acquisition
uses the native fixed hierarchy and already bound assets. Capacity defaults to 16;
Lua accepts integer capacities 1..10000, further constrained by the host's
prefab/entity/pool budgets. Exhaustion fails the request instead of growing the
pool. Pool instances support ordinary rigid bodies, fixed scale and no external
parent; controllers, ragdolls and ropes use ordinary spawning. Release destroys
active leases too. Reload/unload invalidates all resources; failed callbacks and
prepared scene replacement cancel uncommitted resource commands exactly like
spawn/despawn. Do not release a prepared prefab until its pools are released. Lua pools use
the fixed prefab recipe; custom native reset/release callbacks use the host API.

Query shapes accept `box` with positive `half_extents`, `sphere` with positive
`radius`, `capsule`/`cylinder` with `half_height` and `radius`, or `convex_hull`
with 4..256 nondegenerate `points`. Construct once and reuse; shapes need no active
physics service. Query options share `mask`, `sensors`, and a checked `ignore`
entity. Sweeps additionally accept `rotation` and `initial_overlap`; overlaps
accept `rotation`. `raycast_all` and `overlap` accept integer `capacity` 1..256
(default 32) and return two values: hit array, overflow flag. Hits retain native
ordering and expose checked entity handles only for mapped bodies. Snapshots do
not remain live query results. Invalid input raises a Lua error; a miss gives
`nil` or an empty array. No query advances simulation.

`set_ik` patches an existing named constraint; unknown constraints return `false`.
Options are `target` (world vector, checked scene entity, or `false` to clear),
`offset` (entity-local), `weight` (0..1), `fade_seconds`, `pole_position` or
`pole_direction` (model space), `match_rotation`, `target_rotation` (world unit
quaternion), and `rotation_weight` (0..1). Unspecified fields stay unchanged;
all options validate before mutation. Scene targets check scene generation and
pool lease on every native animation/solve: recycling a target fades the old grip
out and never follows its replacement. `ik_state` includes `weight`,
`current_weight`, `target_valid`, `has_last_target` and optional `last_target`.
Target validity updates during native Animation; the solved pose and last target
update during IK/collection, as in C++.

Traversal commands replace `grab` (boolean limb entries 1..4), `reach` (world
vector), `pull_up`, `jump_off` and `drop`, with neutral defaults for omitted
fields. They persist until replaced; native edge rules and phase ordering are
unchanged. State includes `enabled`, `mode` (`walking`, `hanging`, `mantling`),
`limb_count`, `held`, copied `grips`, mantle/swing/anchor data and `resource`.
Grips include phase, kind, checked obstacle `entity`, position/normal/surface
frame, strength, reach/slip progress, surface tag, drain, fixed/ledge and quality.
Traversal events arrive through `character_events`; resource writes affect the
next physics step.

Ragdoll start options are `blend_seconds`, optional `velocity`, `impulse_bone`
(native zero-based bone index, default -1), `impulse`, and optional world
`impulse_point`. An impulse requires a valid bone index; invalid bone indices
raise an error before changing mode. Hand-over accepts the same impulse fields plus `inherit_velocity`
(default true), with a default 0.1-second blend, and uses the character's velocity.
Recovery accepts `get_up_seconds`, `max_ground_distance`, `mask`, and
`allowed_penetration`. Its `place` includes `found`, position/rotation, `face_down`,
`lying` and `height`. Use clearance-tested recovery for a controller; the low-level
get-up blend deliberately performs no clearance test. State includes mode
(`animated`, `ragdoll`, `blend`, `get_up`), alpha/target alpha, dynamic/awake body
counts, maximum speed, settled and frozen. Events are `started`, `settled`,
`recovered` or `blocked`, with checked `entity` when still mapped.

Rope state includes segment count, rest/target/current length, broken state, both
anchor force vectors and tensions. Points contain position/tangent. Closest-point
results additionally contain a one-based segment, segment fraction `t`, whole-rope
`along` and distance. `rope_point_at` uses a segment and fraction 0..1. Native
minimum-length clamping still applies. Break rows contain `kind="broke"`, checked
`entity`, native joint index (0 start anchor; segment count end anchor), and force.
Ragdoll/rope event calls are snapshots of the current native batch, not drains;
read once in PostPhysics. A helper-generated `blocked` event is available immediately
after recovery and until the next native Physics stage. Missing native components/services return `false` for
controls and `nil` for state; event/query calls without physics raise Lua errors.

Authored `.kinanimator` files define IK chains; native host initialization still
supplies traversal descriptors and ragdoll/rope construction where there is no
authored schema. These bindings control existing native gameplay components;
they do not expose arbitrary Lua callbacks inside physics solvers or duplicate
native simulations. Declare component reads/writes for Lua systems as above.

### C++ runtime prefab gameplay

`SceneSession3D` can own runtime objects independently of the authored level. It
uses the existing `.kinprefab` loader, native components, asset handles and physics
bridge; gameplay components and systems do not require inspector field metadata.

```cpp
struct Pickup { int amount = 1; };

// Register native systems through the existing options.register_components hook.
// Their normal C++ component queries need no components().native<T>() schema.
auto spawned = session.spawn("prefabs/camp-supply.kinprefab", {
    .position = kin::Vec3f{1, 0, 2},
    .configure = [](kin::SceneSpawnContext3D& context) {
        context.root.set(Pickup{2});
        // context.entity("local-child-id") resolves a member of this instance.
    }
});
if (!spawned) return report(kin::diagnostics3d_text(spawned.diagnostics));
auto supply = *spawned;

// Retain value handles; resolve when needed. Native runtime state stays native.
if (auto entity = session.resolve(supply.root)) {
    const auto* pickup = entity.get<Pickup>();
    // Use pickup->amount for game rules, then release the complete object:
    if (pickup) {
        auto removed = session.despawn(supply);
        if (!removed) report(kin::diagnostics3d_text(removed.diagnostics));
    }
}
```

Spawning returns only after new model/material/animation/particle bindings and native
physics objects are ready. A missing asset, invalid shape, failed constructor or
capacity limit removes partial new entities and physics bindings. The constructor
runs once, before binding, and can add arbitrary native gameplay components. Keep
it confined to the supplied entities: do not create/delete/reparent entities, mutate
the existing level, register systems or perform external side effects. Those effects
cannot be rolled back. Optional position/rotation/scale values replace only the given
root fields, preserving other prefab defaults. `parent` is another `SceneEntity3D`;
placement is local to it. Spawn a dependent object separately with that parent handle.
Dynamic bodies retain world-level simulation; use native constraints for physical attachment.

`SceneEntity3D` contains session identity, scene generation, the existing ECS handle
and a runtime lease identity (zero for authored entities).
`session.handle(session.entity("player"))` obtains one for an authored entity.
`resolve` returns empty after destruction, reload/unload, or use with another session;
it never dereferences a previous world's pointer. `entity(prefab, local_id)` returns a
member handle. A handle's boolean conversion means it was initialized; use `resolve`
or `alive(prefab)` for current liveness. Raw entity/component/body handles retain their
existing lifetime rules. Inside a gameplay system, `handle.resolve(*context.world)`
performs the same checks without capturing the session. Returning a pooled lease
invalidates its root and child handles even when the ECS entity is reused later.

`despawn` removes all members, including reparented members and dependent runtime
prefabs below them, and reconciles physics immediately. Authored/unmanaged children
cause a refusal before removal; detach them explicitly first. Use `despawn` for these
owned objects. Direct native destruction invalidates handles but leaves bounded
ownership bookkeeping until `despawn` or scene replacement. `runtime_prefabs()` and
`runtime_entities()` report that ownership. Runtime entities have no authored IDs and
stay out of ordinary level saves; explicit runtime snapshots remain an application
policy. Reload/unload releases the complete runtime population.

Reflected configuration also has a typed C++ entry point:

```cpp
session.reconfigure<kin::Collider3D>(object.root, [](auto& collider) {
    collider.scale = {1.5f, 1, 1};
});
session.reconfigure<kin::ModelMaterials3D>(object.root, [](auto& material) {
    material.variant = "blue";
});
```

This mutates a temporary component and sends changed reflected fields through the
same validated live-binding transaction described below. No-op changes do not rebuild
bodies. Only reflected fields are applied; configure custom reflected definitions in
the existing registration hook when needed. Per-frame input, velocities, game state,
animation parameters and impulses use existing native APIs directly. This avoids a
staging/binding pass for ordinary gameplay updates.

Direct lifecycle/configuration calls are synchronous on the renderer thread, between
steps/collection, outside deferred/readonly iteration and with no prepared scene
replacement. Systems use the existing ECS command buffer to request runtime lifecycle
work instead:

```cpp
options.initialize = [](kin::SceneSession3DContext& scene) {
    const auto objects = scene.commands; // weak endpoint for this scene generation
    scene.world.systems().register_task({.id = "game.fire"},
        [objects](kin::SystemContext& context) {
            auto shot = objects.spawn(*context.commands, "prefabs/range-projectile.kinprefab", {
                .position = kin::Vec3f{-2.2f, .5f, 0},
                .configure = [](kin::SceneSpawnContext3D& created) {
                    // Add native game state before asset binding and physics.
                }
            });
            // Store shot in bounded game-owned state to poll next frame.
            // objects.despawn(*context.commands, shot) also accepts this ticket.
        });
};
```

Include `<kin/ecs/system.hpp>` for system/command-buffer definitions. `session.commands()`
also returns the active endpoint between frames. Each system uses its own supplied
buffer; parallel task systems are supported. Partitioned native Flecs worker callbacks
retain their existing `context.commands == nullptr` contract: use a main-thread native
system or a task system for lifecycle requests. The scheduler's normal flush
order transfers requests to a bounded scene queue. `step()` runs one full-frame graph
with host boundaries on the renderer thread:

1. Input through Physics systems run, then queued births/removals commit. Native
   physics advances once and propagates current transforms.
2. PostPhysics systems read fresh `PhysicsContacts3D` and body poses from this step.
   Their lifecycle requests commit before Animation, so contact-driven removals do
   not reach animation or collection. Bodies born here first simulate next step.
3. Animation systems run, their requests commit, and native animation, environment
   clocks, transforms and particles advance once.
4. Audio through Editor systems run. Remaining requests commit and transforms
   propagate before collection; these late births first simulate on the next step. When no system of
   these phases was scheduled this frame and no request waits, nothing can have changed since the Animation
   boundary, and the particle budgets are not checked and the transforms not propagated a second time
   (`collect()` does both for whatever the host changed since).

Empty phases still execute their host boundaries. Frame rates/intervals update once,
and the complete graph is validated before any system or native simulation runs.
`projectile_range` separates Update firing from PostPhysics contact damage/removal.
Manually flushing a buffer or running a system only transfers requests; `step()` is
required to execute them. No lifecycle loading/binding runs inside ECS iteration.
Callers supply the timestep; `step()` does not introduce a second fixed-step loop.

`SceneRequest3D::status()` is `Invalid` for a default ticket, then `Pending`, and finally
`Succeeded`, `Failed` or `Cancelled`. `prefab()` returns a checked value handle only for
a successful spawn; it may later become stale like any runtime handle. `error()` gives
the failure/cancellation reason. A ticket never retains a world, assets or constructor
captures. Despawn accepts an existing `ScenePrefab3D` or a spawn ticket; a ticket must
have succeeded earlier in execution order. Parent placement currently requires an
already resolved entity handle. Sequential requests are independent, not a transaction:
a failed request reports through its ticket and the rest of the frame continues.
Earlier successful requests remain committed. A failed system or host boundary stops
later phases and cancels requests that have not reached a lifecycle boundary. Earlier
commits and native physics cannot be rolled back; retrying `step()` advances a new
step. Dropped/unflushed command buffers cancel their own requests.
Reload, unload and destruction cancel all old requests, including unflushed ones.
A prepared replacement cancels requests when a step attempts to execute them (`error()` says
that they are cancelled, not held): spawns and despawns queued between `prepare` and
`activate`/`cancel` are lost, as are those still queued at activation, because the request
belongs to the old scene. Submit them again after `cancel`, or through the new scene's
`commands()` after `activate`. Direct recursive calls remain rejected.

`max_pending_commands` defaults to 64 and counts accepted requests in both unflushed
buffers and the scene queue. Capacity exhaustion fails immediately without appending
another ECS command. Do not rely on which worker wins the last slot when parallel
submission saturates the budget. Constructor captures are application-owned C++ memory:
capture small values/handles, keep result collections bounded, and avoid retaining a
whole level in a callback. Path-based source loading remains synchronous; use explicit
preloading/pools below for repeated objects. A per-step spawn-time budget is not implemented.
The target/projectile example keeps a fixed
16-slot projectile array, consumes contact Begin events once, expires missed shots,
and handles damage, destruction and respawn entirely in a C++ system.

Default budgets are 64 entities per spawn, 1,024 runtime entities and
128 runtime prefabs (`max_spawn_entities`, `max_runtime_entities`,
`max_runtime_prefabs`), in addition to existing source, scene, particle, dependency and
native physics capacities. Path-based source loading/binding happens at the explicit
spawn call; there is no background upload or new implicit resident GPU cache. Hold deliberate
`Assets3D` handles if a game needs assets kept ready across an empty population.
Scene camera/environment settings belong to the level and are rejected in object
prefabs. Dependency watches extend at spawn and are pruned by scene reload, preserving
their bounded metadata and explicit polling contract.

Binding functions accept an optional entity-ID span for construction of selected
objects; an empty span retains their whole-world behavior. Particle validation for a
selected span covers that subset, so the session also checks the full scene's particle
budget. `WorldPhysicsState3D::sync_entities(ids)` reconciles only those IDs (including
destroyed IDs), leaving unrelated bindings and rebuild requests intact; an empty span
does no work. Mesh3D/material collection buckets retain only current draws and are cleared
on owned despawn, so removed objects cannot become a hidden owning asset cache.

`camp_walk` demonstrates the complete loop: spawn three supplies and a dynamic crate,
rotate supplies with a native ECS system, collect with E, push with F, and reset with R.
It uses the existing free bottle/box models and native character input/physics APIs.

The example's `input.hpp` maps the existing `InputMap`/`GamepadState` to a fixed-size
native `PlayerCommand`. Gameplay never reads a device: submit the value once per step
as ordinary C++ state, then let the existing scheduler consume it in Input, set
controller input in PrePhysics and perform interactions with current poses in
PostPhysics. The command mailbox clears after consumption so missing input is neutral.
Successful deferred removal is acknowledged before pickup animation; failed requests
never grant collection credit. The same path accepts scripted test commands.

```cpp
// Level loading, once per scene generation:
options.register_components = camp_walk::register_components;
options.initialize = camp_walk::install;
// After session.load(...), camp_walk::prepare(session) spawns four owned objects.

// Each fixed step, from a device mapper, recording or test:
camp_walk::submit(*session.world(), camp_walk::PlayerCommand{.move = {0, 1}});
auto stepped = session.step(dt);
if (!stepped) report(kin::diagnostics3d_text(stepped.diagnostics));
```

This is example game code in `examples/camp_walk/gameplay.hpp`, built on existing
session/controller/ECS APIs. The host owns level replacement and explicitly carries
chosen progress counters after successful reload. Initialization modifies only the
candidate world; it never changes active gameplay state or retains devices.

### Scene-aware physics queries

`SceneSession3D::queries()` returns a copyable weak `SceneQueries3D` endpoint; scene
initialization receives it as `context.queries`. Capture it in main-thread ECS systems
alongside `context.commands`. Queries become available on activation when the scene
has physics. They read the last synchronized/stepped native world, with no implicit
sync, simulation, asset loading or second world. Use PostPhysics for this step's poses.
Calls from worker threads, inactive candidates, replaced/unloaded scenes or scenes
without physics are unavailable. `bool(queries)` checks availability on the caller thread.

```cpp
const auto queries = session.queries();
const auto player = queries.handle(session.entity("player"));
if (auto hit = queries.raycast(from, to, {.ignore = player, .sensors = true})) {
    auto entity = queries.resolve(*hit);
    // hit->physics contains the existing body, point, normal, distance, etc.
    // entity is empty for a native obstacle without a current scene binding.
}

std::array<kin::SceneOverlapHit3D, 8> hits;
auto found = queries.overlap(reusable_sphere, center, {}, hits,
    {.mask = kin::layer_bit(kin::layers3d::Sensor), .ignore = player, .sensors = true});
if (found.valid && !found.overflow) {
    for (std::size_t i = 0; i < found.count; ++i) {
        auto entity = queries.resolve(hits[i]);
        // Select/filter game-owned components here.
    }
}
```

The endpoint provides closest `raycast`, closest `shape_cast`, bounded `raycast_all`
and bounded `overlap`. `SceneQueryFilter3D` exposes a layer mask, sensor opt-in and a
checked entity to ignore. Ignoring an entity excludes all native bodies carrying its
id, including character/ragdoll/rope bodies; the existing primary-body collision-group
exclusion also applies. Stale, malformed and foreign ignore handles fail closed.
Single-hit queries return no hit on unavailable/invalid input; bounded queries return
`valid=false`, zero count and no overflow. See the [bounded physics contract](physics_3d.md#bounded-query-results)
for deterministic ordering, truncation and zero-capacity probes. Scene wrappers write
into the same caller slots as their raw physics records, without a hidden temporary list.

`SceneRayHit3D`, `SceneShapeHit3D` and `SceneOverlapHit3D` hold a raw `physics` value
plus a checked `entity` identity. `queries.resolve(hit)` verifies both the current
scene/lease and native body binding. It rejects despawned/reloaded/reused leases and
body rebuilds even if the ECS entity remains alive. The captured point/normal remain
snapshots; resolve does not refresh geometry. Direct `hit.entity.resolve(world)` checks
only entity identity. Hits and endpoints retain no world, models or GPU resources.
A raw native obstacle remains a hit even when it cannot map to an entity, so it still
blocks targeting. Native bodies with merely copied/forged entity user ids do not map.

Camp uses an eight-slot sensor overlap for a 1.25 m reach volume, ranks owned supplies
by distance, checks rays from the player and camera, and highlights only a visible
candidate with a gold tint/light. Collection and crate pushing cannot pass through
obstacles. Saturation clears selection and increments `query_overflows`; the example
keeps three supplies and one crate. The third bottle was moved out from behind the
campfire so the demo remains playable under the new visibility policy.

### Preloaded prefabs and bounded pools

Do loading explicitly on the renderer thread after loading the level, before starting
its gameplay steps:

```cpp
struct Projectile { float remaining = 2; int damage = 1; };
auto prepared = session.preload("prefabs/range-projectile.kinprefab");
if (!prepared) return report(kin::diagnostics3d_text(prepared.diagnostics));
auto projectile = *prepared;
auto created = session.create_pool(projectile, {
    .capacity = 16,
    .reset = [](kin::SceneSpawnContext3D& context) { context.root.set(Projectile{}); },
    .release = [](kin::SceneSpawnContext3D& context) { context.root.set(Projectile{}); }
});
if (!created) return report(kin::diagnostics3d_text(created.diagnostics));
auto pool = *created;
auto spawned = session.spawn(pool, {.position = kin::Vec3f{-2.2f, .5f, 0}});
if (!spawned) return report(kin::diagnostics3d_text(spawned.diagnostics));
auto returned = session.despawn(*spawned); // return the lease to its pool
if (!returned) return report(kin::diagnostics3d_text(returned.diagnostics));
auto stats = session.pool_stats(pool);
auto released_pool = session.release(pool);
if (!released_pool) return report(kin::diagnostics3d_text(released_pool.diagnostics));
auto released_preload = session.release(projectile);
if (!released_preload) report(kin::diagnostics3d_text(released_preload.diagnostics));
```

`preload` uses the shared prefab loader, binders and asset ownership. It retains a
frozen CPU recipe plus one inactive bound reference instance, not a second world or
new GPU cache. Geometry/materials/textures/collision recipes are shared through the
existing handles. The reference does not render, simulate, produce contacts or enter
ordinary gameplay queries. `spawn(projectile, options)` instantiates from this snapshot
without reading prefab/model/material/animation/texture/collider files. Changes on disk
require an explicit new preload or scene reload. Existing dependency watches still
report changes when polled. Preloading validates native physics using a temporary body,
so loading needs a spare native slot; it is loading work, not a background operation.

`create_pool` allocates its complete capacity up front. It keeps disabled ECS members
and bound resources for reuse, with no overflow allocation or automatic expansion.
Acquisition restores all authored components from the pristine bound reference, then
calls `reset` and the optional per-spawn `configure`. Internal reflected entity refs
are remapped to the current slot, rather than the reference instance. Transform position
and rotation can vary; scale, hierarchy, models/material bindings/animation assets,
particle textures and collider shapes stay prepared. Engine state such as animator
parameters/player/IK state, particle ages/time/seed, render tint and body descriptors
returns to its authored baseline. Arbitrary native game components need an explicit
reset callback; the engine cannot infer which health, timers, inventory or external
handles belong to a new life. Remove/reset game components that should not persist.

`despawn(lease)` runs optional release cleanup, disables the members, removes ordinary
native bodies and clears collected references. It resets stock state and renews lease
identity before reuse. Old value handles and completed request results cannot resolve
to the next occupant. Raw ECS/component/body handles retain their normal lifetime rules;
use checked value handles across leases. Idle entities retain component storage and
asset handles deliberately. On acquisition the physics bridge recreates native bodies
from reset descriptors: this is **ECS/asset pooling**, not persistent native-body pooling
or a guarantee of zero allocations. Controllers, ragdolls and ropes currently use
ordinary spawning; pool creation rejects them. Pool parenting and nested runtime children
are unsupported; detach foreign children before returning or destroying a pool.
Choose native physics capacities for the desired concurrent population: warm-up is
sequential, and a later acquisition can still fail if other bodies fill the world.

Pool callbacks configure only the supplied game-owned state and must preserve topology
and prepared engine resources. Keep them nonthrowing where possible; a failed reset
reports an error, removes partial native objects, restores stock configuration and leaves
the slot inactive. Release cleanup also runs after a failed acquisition. A release callback
failure leaves the lease active and reports failure. Explicit pool destruction refuses
foreign children and can clean remaining owned members after direct native destruction;
it does not silently replenish damaged slots. Reload/unload destroys every slot and reference.

The existing `SceneCommands3D::spawn` overloads accept prepared and pool handles and return
the same checked tickets. Deferred `despawn` returns a pooled lease. Execution uses
the phase boundaries described above. A released/reloaded resource handle makes queued requests
fail safely; outstanding handles/tickets do not pin resources. Both resource handles are
session/generation scoped. Releasing a prepared prefab refuses while any pool references it;
ordinary spawned instances retain their own art after its release.

`max_preloaded_prefabs` and `max_prefab_pools` default to 16 each. Reference instances and
all pool slots count against the existing entity/prefab/scene budgets, including idle slots.
Disabled retained particle emitters count toward configured particle budgets too.
`runtime_prefabs()` / `runtime_entities()` count active runtime ownership;
`reserved_entities()` includes inactive slots and preload references. `pool_stats()` exposes
fixed capacity and reuse/exhaustion counters. Keep pool capacities small and deliberate;
release a pool/reference to release retained storage/assets, or reload the scene.

`projectile_range` explicitly prepares target/projectile art during loading, preallocates
16 projectile slots, and resets native Projectile state on every use. Gameplay compares
native body handles in contact events, avoiding old contacts referring to a reused ECS ID.
The host performs this loading setup again after R reloads the scene. The regression's
optional `kin_prefab_pool3d_tests --benchmark` compares warm path/preloaded/pooled lifecycle
latencies over 10,000 cycles each; `KIN_POOL_PROFILE` selects a CSV output path.

### Live field editing

An activated session routes its loaded authored members' ordinary
`world.components().patch_field(...)` calls through transactional live editing. Existing
inspectors use the same component descriptors, values, diagnostics and change events.
For a batch with cross-field invariants, use:

```cpp
auto barrel = session.entity("brass-barrel");
const kin::SceneFieldEdit3D changes[] = {
    {barrel, "Transform3D", "scale", kin::Vec3f{3.5f, 3.5f, 3.5f}},
    {barrel, "ModelMaterials3D", "variant", std::string("blue")}
};
auto edited = session.edit(changes);
if (!edited) report(kin::diagnostics3d_text(edited.diagnostics));
```

Run edits on the renderer thread between steps/collection, outside deferred/readonly
iteration and with no prepared scene replacement. The isolated staging world contains
only affected entities, transform descendants and transform-only ancestors. It copies
native runtime state through the shared registry, binds models/materials/animation/
particle textures, validates settings and scene-wide particle capacity, cooks collision
assets and prepares replacement bodies before committing. Failed validation, missing
assets, invalid native shapes and exhausted body capacity leave active fields, bindings,
body handles and field events unchanged. Successful batches publish the normal field
events and derived binding changes. Scene/world identity and generation remain stable;
the next stock collection reflects the edits. Existing checked scene saves retain the
new values as ordinary prefab overrides.

Unchanged model handles/geometry remain shared. Particle arrays retain living particles,
emission progress and ages; reducing a cap below the live count is rejected. Compatible
animation state is retained by the existing binder. Ordinary rigid-body edits preserve
pose and velocity unless pose/velocity fields were explicitly changed; parent edits
rebuild affected descendant colliders. New physics handles replace old ones, so reacquire
them through `bodies()->body_of(...)`. Replacement requires spare capacity while both
shapes/bodies exist; it does not allocate another physics world or scratch pool. View
option changes prepare a replacement target before commit and briefly overlap targets.

Defaults cap a batch at 256 fields and 64 affected entities including ancestors
(`max_edit_fields`, `max_edit_entities`), plus 64 KiB total names/string/reference text.
Native components must support staging by copy; noncopyable types return a diagnostic.
Existing particle/source/dependency budgets
still apply. New watches extend the existing bounded watch map without acknowledging
unrelated source changes; old dependencies are pruned on scene reload. The registration
callback also runs for staging worlds, so keep it free of external side effects.
Register all editable custom component definitions there as well.

This path edits existing fields. Structural changes, controller/ragdoll/rope physics
configuration and constrained rigid bodies still need explicit integration or scene
reload, as do edits to disabled entities. Body replacement invalidates external physics handles/contacts; it does not
transfer custom constraints or grip ownership. New runtime entities can call `edit`
explicitly after binding/physics setup; their construction and prefab compilation keep
the normal registry behavior. Direct `get_mut`, raw Flecs mutations, and component
add/remove/reparent calls bypass the policy. Standalone ECS worlds retain their existing
explicit binding/rebuild contract. No per-frame polling or binding pass is added;
the synchronous edit work runs only when one of these field-edit paths is requested.

`camp_walk`: **F6** applies typed size/material/particle configuration while movement,
animation and collisions continue. The headless demo performs these changes and reports
immediate collision replacement and preservation of living particles.

### Authored material overrides and variants

`ModelMaterials3D` patches a static or skinned renderer's imported materials. It uses
`Material3D`, the existing shared texture cache and the ordinary ECS component codecs:

```json
"ModelMaterials3D": {"asset": "materials/camp.kinmaterials", "variant": "blue"}
```

Add this component to a prefab once, then override its `asset` or `variant` using the
usual scene/prefab field overrides, inspector or Lua patches. Only these two fields
are serialized. Runtime bindings, textures and GPU resources are excluded from saves.
`resolve_render3d_assets` binds materials after models; tools can explicitly call
`resolve_materials3d_assets(world, assets)` after an edit. Empty `asset` clears the
binding. SceneSession3D coordinates loading and transactional reload automatically
when its explicit reload methods are called.

A `.kinmaterials` file has schema `kin.materials3d/1`. Its base `material` patches all
parts, `parts` patches selected parts, and a selected named variant adds another layer:

```json
{
  "schema": "kin.materials3d/1",
  "material": {"roughness": 0.8},
  "parts": [{"part": "Canvas", "material": {"double_sided": true}}],
  "variants": {
    "blue": {"material": {"color": [35,100,200,255], "roughness": 0.65}},
    "wet": {"parts": [{"part": 0, "material": {"roughness": 0.2}}]}
  }
}
```

The application order is imported material, base global patch, base part patch,
variant global patch, variant part patch. Omitted fields preserve earlier values;
`false` and zero are real overrides. An empty variant name selects only the base layer.
An unknown variant, unknown part or ambiguous part name is an error. Names must be
unique across imported model parts; otherwise use a zero-based index in `model.parts`.
Two rules in the same layer cannot select the same part, including a name/index alias.
Prefer names where possible because indices follow the imported part order.

Fields correspond directly to `Material3D`: sRGB byte `color` (RGB/RGBA array or
`{r,g,b,a}` object), metallic/roughness, emissive intensity, normal_scale,
occlusion_strength, alpha_cutoff, unlit, double_sided, casts_shadow, receives_shadow,
wireframe, blend and wrap_u/wrap_v. Blend values are `opaque`, `alpha`, `additive`,
`mask`; wrapping is `clamp`, `repeat`, `mirror`. Metallic, roughness, occlusion strength
and alpha cutoff are in [0,1], emissive in [0,1e6], normal scale in [0,100]. Unknown
fields, non-finite values, invalid enums and out-of-range values return diagnostics.
`shader` names a compiled material fragment shader relative to the content root;
`null` explicitly clears an imported or inherited shader. `shader_params` supplies
exactly 16 finite numbers (four vec4s, read through `kin_param`). For example:

```json
"shader": "shaders/thin_glass.frag.spv",
"shader_params": [1.45,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0]
```

Compile shaders with `kin_compile_glsl` and the material shader API described above.
The loader checks SPIR-V size/header before creating the shader; files are capped at
4 MiB. It does not compile GLSL or implement a shader graph. `load_material_shader`
shares live handles in a weak cache with 64 metadata entries by default
(`Assets3D::Limits::shaders`, zero disables caching). Bindings own their shader handles,
and invalidating a shader source refreshes affected bindings without invalidating
retained handles. Shader dependencies use the same content-pack and reload paths as
textures. The GPU renderer retains compiled custom shaders for its pipeline lifetime;
this cache limit bounds lookup metadata, not total shader/pipeline GPU memory.

Texture fields are `texture`, `normal`, `metallic_roughness`, `occlusion` and
`emissive_texture`. A string names a path relative to the common content root, a
`null` explicitly clears the imported map, and an object selects sampling:

```json
"texture": {"asset": "textures/canvas.png", "filter": "mipmapped"},
"normal": "textures/canvas-normal.png",
"metallic_roughness": null
```

Filters are `mipmapped` (default) and `nearest`. Base color/emissive maps load as sRGB;
normal, metallic-roughness (G roughness, B metallic) and occlusion (R) maps load as
linear data. Identical paths and roles share images with imported glTF maps and other variants; different samplers
retain their own descriptors. The legacy `filter` field converts to a per-map descriptor using the material's wrap
settings. An explicit descriptor can instead preserve different min/mag/mip choices:

```json
"emission_color": [1.0, 0.1, 0.02],
"emissive": 5,
"texture": {"asset": "textures/canvas.png", "sampler": {
  "wrap_u": "repeat", "wrap_v": "clamp", "min": "linear", "mag": "nearest", "mip": "linear"
}}
```

`sampler: "inherit"` preserves the imported/previous map sampler; `sampler: null` clears that override and converts the
replacement map to legacy sampling. A replacement without `sampler` also converts to legacy sampling. Do not combine
`filter` and `sampler`. Descriptor defaults are repeat, linear min/mag and linear mip filtering. Clearing the map also
clears its override. `emission_color` is a three-number linear RGB factor (each in [0,1e6]) and implies
`emission_mode: "independent"`; an explicit `"base_relative"` with that colour is rejected. An old `emissive`-only patch
retains legacy base-relative appearance. To edit only independent strength, also specify `emission_mode: "independent"`. Texture paths are file paths, not percent-encoded glTF URIs. Model3D UVs and
tangents must suit the maps; the importer supplies the existing tangent generation.

`Assets3D::load_materials(path, model, variant)` returns an existing-style
`AssetHandle<MaterialBinding3D>`. The binding owns one material value per part and
shared texture handles, with a weak model identity. It never clones or pins geometry.
Bindings for the same file/model/variant share a weak cache entry; its default metadata
budget is 128 (`Assets3D::Limits::materials`). `trim`, `clear` and `invalidate` cover
these entries too. Source files are capped at 1 MiB before reading, with at most 128
variants, 256 part rules per layer and 4,096 bound model parts. These limits do not cap
total decoded texture or GPU memory; retained consumer handles own their resources.

Direct drawing can pass `binding->materials` to the per-part overloads of `Scene3D::draw`
and `draw_skinned` (after tint and object ID). A nonempty array must exactly match the
model's part count. ECS collection uses the binding only for its original model identity;
if a model was replaced and rebinding failed, it falls back to the new model's imported
materials rather than applying stale part indices.

Invalid material/configuration/texture replacements retain the last good binding.
Use `assets.invalidate(changed_source)` before manual rebinding; `SceneSession3D` does
this for watched material files and their selected texture/shader dependencies. A material-only
edit keeps the same model/mesh; an image edit refreshes affected material bindings and
textures. An image also used by the imported model can require that model to be rebound
to update its original material handles. Existing consumers stay valid throughout.
Folders and mounted content packs use the same loaders and relative paths.

Summit's authored camp uses `prefabs/camp-barrel.kinprefab` twice: the default brass
variant and a blue variant selected by a standard prefab override. Both share one
barrel model. The fire pit selects the ember variant from the same material asset.
The expedition's existing imported appearance remains unchanged.

### Tiling (a material's UV transform)

`Material3D::uv_scale` and `uv_offset` move where every map of the material is read: the mesh's UV times the scale,
plus the offset (Godot's `uv1_scale` / `uv1_offset`, Unity's tiling and offset), in every pass: the lit and unlit
colour, the normal map's frame, a cut-out's coverage in the main, shadow, pick, pre- and visibility passes and the
temporal anti-aliasing's coverage. `.kinmaterials` take them as `"uv_scale": [x, y]` and `"uv_offset": [x, y]`. A
glTF model's `KHR_texture_transform` is applied to its UVs when it loads, as before; these apply on top. Not yet: the
GI's voxels read the base map untransformed (a tiled map's mean colour is much the same), and WebGL2 ignores them.
Identity (the default) reads exactly what it read before.

### Importing other engines' materials

`kin_import_material godot <material.tres | folder> --project <godot project> --content <kin content root> --out
<folder>` turns Godot 4 `StandardMaterial3D` and `ORMMaterial3D` resources into `.kinmaterials` files (one each, a
folder's in its layout), and `import_godot_material()` (`kin/render3d/material_import3d.hpp`) does it for tools such
as the studio. Every field kin has is written, so the file replaces a model's imported material as a whole:

- albedo colour and map; metallic and roughness, their maps' selected channels packed into kin's one map (G
  roughness, B metallic; ORM maps as they are); normal map and scale (only with `normal_enabled`); ambient
  occlusion (its channel moved to R); emission: its colour and map by its operator (an added colour is put into a
  map with the map, scaled by the brightest texel), its energy times, with the project's physical light units, its
  intensity in nits;
- transparency (alpha; scissor as a cut-out at its threshold; hash as a cut-out at a half; depth pre-pass as alpha),
  the add blend mode, cull mode (disabled is double-sided), unshaded, receiving shadows, `uv1_scale` and `uv1_offset`
  (`Material3D::uv_scale`, `uv_offset`), and the texture filter and repeat as every map's sampler.

Maps inside the content root are referenced where they are; others are copied into `<out>/textures`, beside the
maps made. What kin does not show as Godot does is printed as a note for each material (backlight, subsurface
scattering, proximity and distance fade, refraction, detail maps, front-face culling,
subtract and multiply blending, ...), not dropped silently. Maps a material needs repacked but kin cannot decode (DDS,
KTX) are left out with their factor kept, and said. Every file written is read back through
`parse_materials3d_asset` first.

`kin_import_material unity <material.mat | folder> --project <the folder holding Assets/> ...` (and
`import_unity_material()`) does the same for Unity's materials, read by their shader's properties: URP's Lit (Simple
Lit and Baked Lit as dielectrics of their smoothness, Unlit unlit), the built-in pipeline's Standard, HDRP's Lit; a
custom shader's or Shader Graph's material by the usual property names, with a note. Textures are found by GUID
through the project's `.meta` files. Mapped:

- base colour and map, and the base map's tiling and offset as the material's UV transform (Unity reads every map
  with it; a map tiled differently is noted);
- metallic and smoothness: kin's roughness is 1 - smoothness. With the metallic map (R metallic, A smoothness, or the
  albedo's alpha by `_SmoothnessTextureChannel`) the import makes kin's map from it, the smoothness factor (Standard's
  gloss map scale) in it; HDRP's mask map is remapped by its `_MetallicRemap`, `_SmoothnessRemap` and `_AORemap`
  ranges;
- normal map and scale; occlusion (Unity's G, moved to R) and its strength; emission with the `_EMISSION` keyword
  (the colour in linear light, times its map; HDRP's colour is linear already, in its physical units: noted);
- opaque, cut-out (`_AlphaClip`, Standard's Cutout), transparent (alpha; additive; premultiplied and multiply noted),
  `_Cull` (off: double-sided; HDRP's `_DoubleSidedEnable`), `_ReceiveShadows`.

TIFF and PSD maps (frequent in Unity projects; kin's images don't read them) are decoded by the import's own reader
(baseline TIFF: strips or tiles, uncompressed, LZW, PackBits, Deflate, the horizontal predictor, 8/16-bit and float;
PSD's composited picture) and written as PNG. Unity's Shader Graphs themselves (see `--graphs` below), its HLSL
shaders, detail, parallax and clear-coat maps are noted.

Unreal's materials are read in two steps. Inside Unreal's editor, `tools/import_material/unreal_export.py` (its
Python, headless: `UnrealEditor-Cmd <Project.uproject> -run=pythonscript -EnablePlugins=PythonScriptPlugin
-script="unreal_export.py --content /Game/Path --out <folder>" -nullrhi`) writes each Material and Material Instance
as a `kin.unreal-material/1` file: its blend mode, shading model, two-sidedness and clip value, its parameters' values
(an instance's own over its parents'), and the expressions feeding each output a few deep, with the textures they
name exported as PNG (with their sRGB and green-flip settings). Then `kin_import_material unreal <export.json |
folder> --project <the export folder> ...` (`import_unreal_material()`) reads each output as bias + gain x one
texture channel:

- constants, scalar and vector parameters, texture samples (and their TextureCoordinate tiling), Multiply, Add,
  Subtract, One Minus, Component Mask, and a Lerp whose alpha is exactly 0 or 1; anything else (a varying Lerp, two
  textures multiplied, a parameter collection, custom HLSL) is noted and that output left at kin's default;
- base colour (Unreal's colours are linear), metallic and roughness (an ORM map laid out as kin's, R occlusion, G
  roughness, B metallic, is used as it is; others are packed, bias and gain baked in), occlusion, normal (Unreal's are
  DirectX's, green down: flipped, unless the texture flips green on import, whose source is then as kin's),
  emission, opacity from the base map's alpha (masked: a cut-out at the clip value), blend mode (opaque, masked,
  translucent, additive), two-sided, unlit (its emissive as its colour);
- one-colour maps (Unreal's white, black and flat-normal placeholders) are folded into their factor; a uniform normal
  map that isn't flat is kept.

`tools/import_material/unreal_probe.py` makes the test materials (`tests/fixtures/unreal`) in a scratch project.

#### Node graphs (`--graphs`)

With `--graphs` (`import_unreal_graph()`, `import_unity_graph()`, `import_godot_graph()`) a material's node graph is
imported too, as a [material graph](#material-graphs) beside it: `<graph>.kingraph`, named by the material's
`"graph"`, its parameters' values (an Unreal instance's, a Unity material's properties, a Godot material's
`shader_parameter/`s) in its `"params"`. A common subset of nodes is read (the math, texture samples and their UVs,
parameters, time, vertex colour, position, normals and view, fresnel, panning, lerps, clamps, steps, remaps, splits
and combines; the headers list them); an output fed by a node kin has no counterpart for keeps its Tier 1 value
(Unreal, Unity: Godot's ShaderMaterials have none) and is noted, naming the node. A graph that doesn't compile in
kin, or samples more than 21 textures (the five slots and 16 layers), falls back to Tier 1, said, as does an output
whose nodes chain more than 1024 deep (the import, like the compiler, runs on a stack of its own). One whose
parameters fill more than the 64 floats gets their values (the material's) as constants in a graph of its own
(`<graph>__<material>.kingraph`), said: they are not parameters then.

- Unreal: `unreal_export.py` writes the whole graph, and the graphs of the Material Functions it calls (inlined by
  the import: a function input left unconnected takes its preview value); World Position is centimetres (scaled from
  kin's metres) in Unreal's axes, Z up: kin's (x, z, y), as Unreal's glTF exporter maps them (so are the camera's
  position and vector, the reflection vector and the normals); normal maps are DirectX's (flipped); Power clamps its
  base at 0, as Unreal's. A Static Switch takes the material's or instance's value, else its default: an instance
  that changes one gets a graph of its own, named for the switches on and off (`M_Rock__Moss_on.kingraph`). Named
  Reroutes are followed to their declarations. A material with Tangent Space Normal off gives its normal in the world
  (Unreal's axes). A Custom node of one HLSL expression (or `return` one) of its inputs, numbers and common intrinsics
  (`lerp`, `frac`, `saturate`, `Mod`, `float3`, integer casts as truncation, ...) is read as GLSL, said; anything
  more (statements, loops, textures, engine globals) is refused. Read as Unreal computes them: Bump Offset (the UVs moved along the
  view in the frame the UVs make), Transform between tangent, world and view space (the view's frame from the
  camera's forward, as if it did not roll), If, a parameter collection's value (its default: a game may change it).
  World Position Offset is the graph's `offset` (Unreal's axes and centimetres as kin's). A function call's output is
  taken by its index (the export lists each call's outputs from Unreal's T3D: the editor's pin names are not always
  the outputs'); an OpenEXR map (HDR, displacements: vertex animation textures) is decoded by the import into 8 bits,
  each channel fitted to its range, and read back by it in the graph. A mask (or a pin) of a scalar is the scalar, as
  Unreal masks one (its G, or a vector of it). Approximated, each said in the notes: local or
  an instance's space as world space (Transform, TransformPosition) and the object's
  position as the origin (kin's graphs do not see the object's transform); Depth Fade as its opacity, Scene Depth as
  10 km (kin's graphs see no scene depth: nothing behind); a cube map as the scene's environment; Landscape Layer
  Coords from the world position; Parallax Occlusion Mapping as its UVs (no parallax); a second UV set as the
  first; a particle's colour as the vertex colour, a dynamic parameter as its default. A graph that gives the base
  colour (or alpha) makes the material's colour white (or opaque): a graph's vertex colour includes it.
  Where the graph cannot be used, the Tier 1 import reads approximately too, said: a map behind UVs it cannot follow
  at the mesh's UVs, a Lerp by a varying alpha as its side with a map, an expression it does not know as its first
  input (a Lerp by a constant is read exactly).
- Unity: Shader Graph 10 and later (`.shadergraph`, sub-graphs inlined; older files fall back to Tier 1); Unity's
  vector widths are kept (an unconnected default is cut to what it is added to); a Keyword node follows the
  material's keywords (the graph is written per set of keywords on: `Lit__GLOW.kingraph`); a texture's colour space
  is its default texture's import setting. Colours are taken as gamma, as Unity's inspector shows them (unchecked
  against Unity).
- Godot 4: a `ShaderMaterial` whose shader is a `VisualShader` (its own `.tres` or inline; text `.gdshader`s are
  refused); its fragment stage; Normal Map is read as Godot reads it (the texel, its depth the strength); Godot's
  vertex, normal and view are in view space, kin's in the world's (noted: the same where only angles count, as in a
  fresnel); an Expression node of one assignment becomes a `glsl` node.

`tools/import_material/godot_probe.gd` (headless Godot) and `make_unity_fixture.py` make the Tier 2 test fixtures.

#### Comparing with the source engines (the material lineup)

`tools/import_material/lineup` renders one small scene in kin, Godot 4 and Unreal 5, so imported materials can be
compared with the pictures their own engines make. `lineup.json` lays it out: four unlit quads (graph math, textures,
colour spaces: these should match to the pixel) over four lit spheres (one sun, no ambient), with eight materials
described in words and made as graphs in both engines.

1. `python3 make_assets.py <folder>`: the textures (PNG) and meshes (`.glb`) all three engines load.
2. Godot: in a project holding the assets and `godot_lineup.gd`, `godot --headless --path <project> --import`, then
   `godot --path <project> --script res://godot_lineup.gd -- --out godot.png` (windowed: it renders). It saves the
   materials as VisualShaders.
3. Unreal: `unreal_lineup.py` in the editor of a project with AA, GI, reflections, AO and grain off
   (`LINEUP_DIR=<folder> LINEUP_OUT=unreal.png`; see its header). It saves the materials under `/Game/Lineup`;
   export them with `unreal_export.py`.
4. Import each engine's materials with `kin_import_material godot|unreal ... --graphs`, then
   `kin_material_lineup --scene lineup.json --content <root> --materials <folder> --out kin.png`.
5. `python3 compare.py lineup.json <reference.png> <candidate.png> <sheet.png>`: each object's mean and largest
   difference in 8-bit levels, the pixels off by more than 2, PSNR (and FLIP with `flip_evaluator`), and a sheet of
   the two pictures and their difference.

All three draw linear light encoded to sRGB, with no tone curve. Measured (2026-11-05): kin with Godot's materials
against Godot, unlit row: at most 1 level apart; kin with Unreal's materials is byte-identical to kin with Godot's;
the lit spheres differ by about 3-4 levels, from the engines' BRDFs (Godot's Burley diffuse, kin's multiscatter
boost on metals).

Snapshots do the same for materials that already exist in an Unreal project (a Fab pack, say), a picture a material:
`snapshot.json` puts the material on a quad and a sphere, lit as the lineup is, placed in Unreal by its glTF axes (as
the import maps World Position). List the materials a line each, by
their imported path (`m/<pack>/.../<name>`, a note after it if wanted), then:

1. Unreal: `unreal_snapshots.py` in that project's editor, with the lineup project's render settings and
   `Content/Python/init_unreal.py`, and texture streaming off (`r.TextureStreaming=0`: full-size maps, as kin's).
   `LINEUP_SCRIPT=unreal_snapshots.py SNAP_SCENE=snapshot.json SNAP_ASSETS=<quad.glb, sphere.glb> SNAP_LIST=<list>
   SNAP_OUT=<folder>`.
2. kin, each material: `kin_material_lineup --scene snapshot.json --content <import root> --meshes <quad.glb,
   sphere.glb> --material <path> --out <folder>/<path, / as __>.png`, with `--time <seconds>` the clock Unreal's log
   gives the picture (`lineup: time`) for a material that moves with time.
3. `python3 snapshot_report.py snapshot.json <list> <unreal folder> <kin folder> <out>`: a sheet (a row a material:
   Unreal, kin, their difference) and a table of `compare.py`'s measures for each quad and sphere.
### Authored particle emitters

`ParticleEmitter3D` uses the same scene/prefab registry, field overrides, inspection,
Lua patches and checked saves as other 3D components. It exposes the existing
`ParticleSystem3D` descriptor. A prefab can compose emitter, model and light children.
`prefabs/campfire.kinprefab` does this for flames, smoke, sparks and a steady light;
both authored camp scenes instantiate it.

```json
{
  "id": "sparks", "parent": "campfire",
  "components": {
    "Transform3D": {"position": [0, 0.2, 0]},
    "ParticleEmitter3D": {
      "seed": 23, "shape": "cone", "extent": [0.12, 0, 0],
      "cone_angle": 25, "rate": 14, "max_particles": 48,
      "lifetime": [0.6, 1.1], "speed": [1.1, 2], "size": [0.018, 0.035],
      "gravity": [0, -1.5, 0], "colour_start": [5, 1.2, 0.1],
      "colour_end": [1, 0.15, 0.02], "blend": "additive",
      "facing": "stretched", "stretch": 0.035, "sort": false
    }
  }
}
```

Ranges `lifetime`, `speed`, `size`, `rotation` and `spin` use `[min,max]` (the Vec2
codec also accepts `{x,y}`). Colours are linear RGB, permitting HDR values.
Shapes: `point`, `box`, `sphere`, `cone`, `disc`; blends: `alpha`, `additive`,
`premultiplied`; facings: `camera`, `stretched`, `axis`, `flat`.
Other field names follow the native descriptor: `surface_only`, `spread`,
`velocity_min/max`, `drag`, `wind_influence`, `size_start/mid/end`, `alpha_start/end`,
`fade_in`, `colour_variation`, `frames_per_life`, `frame_random`, `ground`,
`ground_height`, `bounce`, `axis`, `stretch_min`, `atlas_columns/rows/frames`,
`blend_frames`, `softness`, `ring`, `soft`, `soft_distance`, `lit`,
`receives_shadow`, `casts_shadow`, `shadow_cutoff`, `fogged`, `sort`.
The component also exposes `seed`, `rate_scale`, `enabled` and `emitting`.

Optional `texture` is an asset reference relative to the common content root;
`texture_nearest` selects nearest sampling instead of the default mipmapped filter.
Textures load as sRGB through `Assets3D::load_texture`, sharing its role/filter cache
with glTF and authored materials. Empty references restore procedural particles
after an authored binding; untouched C++ textures remain usable. Failed texture
replacement keeps the previous texture and returns diagnostics. Rebinding preserves
particle ages, birth progress and time. Invalidate a changed source before rebinding;
SceneSession3D watches these textures and handles transactional reloads automatically.
Folders and mounted packs use identical references.

Live state is owned per instance, materializing inherited emitters before simulation.
Prefab definitions are never stepped. The seed, descriptor and flags serialize;
live particles, clocks, birth progress, bound paths and GPU handles do not. Equal
seeds/calls give equal effects placed by their own transforms; override seeds to vary
instances. A transactional scene reload starts fresh simulations.

Set `emitting=false` to stop automatic births while existing particles finish visibly;
`enabled=false` also hides the batch (particles continue aging). Bursts/clears use
`entity.get_mut<ParticleEmitter3D>()->system.burst(count)` / `.clear()`. Bursts respect
the native `max_particles` cap, including while automatic emission is stopped.

`ParticleEmitter3D` exposes `receives_shadow` and `casts_shadow` (both default
false), and `shadow_cutoff` (default `0.5`, range `[0,1]`), through scene/prefab
fields, save/load, inspector edits and Lua field access. Reception needs `lit > 0`;
casting is independent of lighting, just like the native `ParticleLook3D` settings.

`ParticleLimits3D` defaults to 128 emitters, 20,000 configured particles per emitter
and 65,536 across the scene. The sum includes disabled/stopped emitters. Configure
`SceneSession3DOptions::particle_limits` for the game; the campfire reserves only 208
particles in the base prefab; the playable camp's smaller-smoke override uses 176.
These limits bound configured simulation capacity, not decoded texture
bytes, weather, custom C++ draws or total retained CPU/GPU allocator memory.

`validate_particles3d(world, limits)` is CPU-only: it checks ranges, finite values,
enum/atlas/axis consistency, live count versus edited capacity and aggregate budgets.
`resolve_particles3d_assets(world, assets, limits)` validates before loading textures;
`resolve_render3d_assets` calls it too (optional third budget argument). Direct hosts
validate after edits and before simulation/collection. SceneSession3D checks at load,
after game systems before stock simulation, and before collection, including runtime
edits. Rates are bounded to representable per-step birth counts. Source limits remain
those of the shared scene/prefab loader; simulation does no content I/O.


## Following a player without an editor

`kin/scene/camera_rig3d.hpp` provides a native `CameraRig3D`, using the existing
`Camera3D`, checked scene handles and physics queries. Keep it in game state;
configure it during loading. It owns two pose snapshots and two small spring-arm
states, sharing retained sphere shapes. No input device, asset cache or world is owned.

```cpp
CameraRig3D rig{CameraRig3DSettings{
    .target_offset={0,1.2f,0}, .arm={.distance=5,.shoulder=.35f},
    .pitch_min=-70, .pitch_max=25, .initial_pitch=-15}};
rig.follow(session.handle(session.entity("player"))); // after scene activation

// Input: right/up motion, in degrees for this fixed step.
rig.orbit(command.look);
// PostPhysics: after native physics has published current WorldTransform3D.
rig.observe(*context.world);
auto gameplay_view=rig.fixed_frame(queries,context.dt); // optional; once per step

// Render: alpha=0 previous step, alpha=1 latest; real frame seconds for recovery.
auto view=rig.frame(session.queries(),alpha,frame_seconds);
if(view) {
    player.get_mut<RenderTransform3D>()->matrix=view->target_transform;
    player.get_mut<RenderTransform3D>()->enabled=true;
    // Apply view->camera in SceneSession3DOptions::compose after collection.
}
```

Add the native `RenderTransform3D` component once during setup. Its `enabled` flag
is false by default; disable it when no frame is available. It overrides world-space
placement during collection for meshes, static/skinned model roots and point/spot
lights. It never writes local/world simulation transforms, changes a controller,
propagates to children, or resamples animation, IK, ropes or world-space particles.
For attachments, set their own presentation placements. Pool reuse clears the override.

The rig interpolates target translation/rotation and the shortest yaw arc, with
clamped pitch; target offsets are world-space. Positive pitch looks up. It accepts
positive, nondegenerate world TRS transforms; shear, reflection and invalid values
return no frame. Scale changes beyond floating-point tolerance snap. `reset()` preserves the target and angles but
makes the next observation snap; movement beyond `teleport_distance` does this
automatically. Small teleports need an explicit reset. `follow()` resets on target
changes. Destroyed targets, released pool leases and replaced scenes return no
frame; explicitly follow the new checked handle after respawning. Old endpoints
retain no world. Calls belong on the scene owner thread and require scene physics.

The fixed-step and render views have independent arm history. Repeated rendering
cannot alter gameplay visibility. Both sphere-sweep against the latest synchronized
physics world, immediately compress on obstruction and recover at `ease_out` metres
per second. Sensor/layer filters are configurable and the checked target is ignored.
An initial overlap is reported in `frame.arm.initial_overlap`; the arm collapses,
but this solver does not depenetrate a focus point already inside solid geometry.
Invalid configuration retains settings/history; invalid frame times return no frame.
`initial_yaw`/`initial_pitch` are constructor defaults; reconfiguration preserves
current angles, clamped to the new limits.

Camp demonstrates this flow in `examples/camp_walk/gameplay.hpp`: the camera follows
the controller, the player is drawn at the matching interpolated pose, and item LOS
uses the fixed-step view. Its host only supplies alpha/frame time and composes the
camera. Headless captures use alpha=1. Summit shares the lower-level `SpringArm3D`
collision solver while retaining its climbing, death camera and look-ahead policies.


## Joint sockets and prefab attachments

`kin/scene/joint_attachment3d.hpp` adds native `JointAttachment3D` state on an
actual prefab instance's root. The scene session supplies checked target identity;
the renderer uses the existing skeleton, animator, IK and render placement.
No editor schema or second animation/attachment graph is introduced.

```cpp
const auto player=session.handle(session.entity("player"));
auto lantern=session.spawn("prefabs/camp-lantern.kinprefab",{
    .configure=[player](SceneSpawnContext3D& spawned) {
        spawned.root.set(JointAttachment3D{player,"hand_R",
            Mat4f::translation({0,0.02f,0})});
    }}).value_or({});
// The next session.collect()/render() updates placement and status.
```

Placement is `target render root * final model-space joint * offset * prefab root
Transform3D`. Child meshes, static/skinned models and point/spot lights keep their
local transforms under that root. An attachment takes precedence over
`RenderTransform3D` on its members. The target's render override is respected, so a
camera-interpolated character and its held item stay together. Targets use the first
skeleton and their final animation/layer/IK/pose-override result, or rest pose before
an animator binds. Targets with IK are solved once for attachment preparation and
that same pose is used for drawing, without advancing animation again.

This is **presentation only**. It never reparents, writes `Transform3D` or
`WorldTransform3D`, moves a collider/body, or assumes ownership of either entity.
Use cosmetic prefabs; physics/character/ragdoll/rope ownership and gameplay motion
remain explicit. World-space particle simulations do not follow a render override;
place/burst those explicitly from a game pose. Invalid attachment subtrees' particle
batches are hidden along with their geometry/lights. Camera/environment components
are not driven by attachments.

Missing, hidden/disabled or expired targets and missing joints hide the attached
subtree rather than leaving it at a stale position. `status()` explains the last
collection result; `placement()` returns its successful root matrix snapshot.
Removing the component detaches; setting `enabled=false` restores ordinary world
placement. Despawning the target does not despawn its accessories: the game owns
those prefab handles. Scene replacement and renewed pool leases require explicit
rebinding. Returning an attachment to a prefab pool clears its binding/cache.

`configure(target,joint,offset)` validates a nonempty joint name of at most 95 bytes
and a finite affine offset; failure retains the previous binding. Bind only actual
instances, not prefab definitions. Joint indices are resolved once per immutable
model identity; a weak model cache detects replacement without retaining old art.
Changing a skeleton in place requires rebinding. The same bounded ancestor-chain
math is used without copying a complete pose per attachment. Warm collection reuses
existing scene/animator storage.

At most `max_joint_attachments3d` (64) enabled roots are prepared per collection;
excess roots fail closed with `LimitExceeded`. This bounds preparation state and
unique-target scratch, not the world's total entity count or draw submissions.
Hierarchy traversal is capped at 64 ancestors. Binding to a target that is itself
attached (or inside an attached subtree) is unsupported, preventing recursive socket
cycles. Nearest attachment roots control their subtrees. A root must have a
`Transform3D`; targets need an owned runtime animator when one is present and a
visible `SkinnedMeshRenderer3D`. Use `WorldRenderState3D(EcsWorld&)`, as SceneSession3D
does, so checked scene targets resolve through the owning registry. The raw Flecs
constructor has no checked scene registry and returns `MissingTarget` for bindings.

Camp carries a lantern assembled from the existing bottle/box assets, with a small
material override and warm point light. It adds one cosmetic prefab/four entities,
uses `hand_R`, shares the loaded meshes/textures, and creates no collision bodies.

## Timed 3D animation events

Author markers alongside the existing machine, blends and IK in `.kinanimator`:

```json
"events": [
  {"clip": "Walk", "markers": [
    {"phase": 0.25, "channel": "game", "name": "footstep", "value": "right"},
    {"phase": 0.75, "channel": "game", "name": "footstep", "value": "left"}
  ]}
]
```

`phase` is normalized clip time in `[0,1)`, independent of clip duration. Markers
use the same `AnimationEvent` channel/name/value payload and `EventInterpreter`
handlers as `kin/anim`. Imported models stay immutable; each animator shares its
configuration's marker table. Native code can assign
`std::shared_ptr<const AnimationEventTracks3D>` to `Animator3D::event_tracks`.
There is no hidden asset I/O during playback.

`SceneSession3D::step` emits at its Animation boundary. Consume in an Audio-phase
system, which runs immediately afterwards even with no rendering:

```cpp
AnimationEventDispatch events;
events.on("game", [](EcsEntity actor, const AnimationEvent& event) {
    if (event.name == "footstep") { /* use this step's controller/world pose */ }
});
// Inside an Audio-phase task, after inspecting overflow counters:
const u32 unhandled = dispatch_animation_events3d(world, events);
```

`dispatch_animation_events3d` drains each animator once, passes the animated
entity to the existing handler, and discards unhandled channels. Calling it again
before the next animation step emits nothing. It does not touch the general
`AnimationEventQueue`, whose existing retention contract is unchanged. The 2D
`AnimationEvent::target/offset` fields are not resolved by this bridge; authorable
3D markers accept channel/name/value only. Direct dispatch to an already resolved
entity is also available as `AnimationEventDispatch::dispatch`.

The incoming base owns events immediately when a crossfade starts, including an
interrupted fade. In a 1D blend, the dominant clip owns the entire step (the upper
clip wins an exact 50/50 tie), at the shared phase and blended duration. Changing
the blend parameter does not replay earlier markers. Outgoing clips and ordinary pose
layers are silent; the dedicated action slot below has explicit marker ownership. Align contact phases across walk/run clips for smooth
locomotion. One-shot action events use the dedicated action slot described below.

Forward intervals are `(before,after]`; reverse-speed playback uses
`[after,before)`. Loops cross phase zero normally. Play/restart and `set_time` do
not emit markers at the initial time or replay skipped intervals. Non-looping
clips stop emitting at their endpoint. Zero dt emits nothing but still samples
the pose; negative/nonfinite dt clears output without advancing. Playback uses
float clocks, as pose sampling does.

Each `Animator3D::events` holds the earliest **16** crossings of its latest step
in playback order. A larger step counts additional crossings analytically,
without iterating over its loop count; `dropped` saturates at `UINT32_MAX`.
Inspect `events.dropped` before dispatch, which clears the batch. Undrained output
is replaced on the next step, including when paused or missing a model. Asset
reload clears old references; invalid replacements retain the last good table
and player. Assets allow at most 64 distinct clip tracks and 256 markers total,
with bounded strings and the existing 1 MiB configuration ceiling.

Standalone players use `update(dt, tracks, batch)`; the batch borrows pointers
into `tracks`, which must remain alive and unchanged until consumed. ECS batches
retain their actual source table until drained or replaced,
including when native code replaces `event_tracks` before dispatch. The bridge
keeps it alive through callbacks. The batch now occupies 280 bytes on this build (including action ownership tokens),
plus a 16-byte shared marker table reference per animator; marker
strings live once per configuration. Crossing collection itself needs no heap
allocation; the surrounding pose/blend and callback code has its own costs.

Camp demonstrates grounded movement events with three dust particles per footstep,
a 24-particle emitter, one 0.1-second synthesized mono sound, and eight sound
voices. Dust/sound positions use the fixed-step controller's feet and facing,
not render-only sockets or interpolated placement. They approximate foot contact;
the sample phases 0.25/0.75 were chosen from the bundled clips' lowest foot
heights. Markers can be retimed in content without changing game code. Camp reports
event,
overflow, unhandled-channel and audio-request counts. Idle and airborne movement
produce no footstep effects.

## One-shot character actions

`Animator3D` has one bounded action slot over its normal locomotion and pose
layers, before IK. Definitions live in the same `.kinanimator` configuration:

```json
"actions": [
  {"name": "pickup", "clip": "Pickup", "mask_joint": "spine",
   "fade_in": 0.06, "fade_out": 0.08, "base_events": true}
],
"events": [
  {"clip": "Pickup", "source": "action", "markers": [
    {"phase": 0.4, "channel": "game", "name": "pickup_contact"}
  ]}
]
```

Start during a fixed-step game system, after assets resolve (including before the
first animation update):

```cpp
const auto token = play_action3d(hero, "pickup"); // zero if busy/unknown/invalid
// Explicit restart or interruption by another action:
const auto next = play_action3d(hero, "pickup", ActionStart3D::Replace);
// A matching current token is required; the default uses its authored fade-out.
cancel_action3d(hero, next);
const auto status = action_status3d(hero, next);
```

`IfIdle` rejects while an action plays or fades out; no requests accumulate.
`Replace` cuts the previous action and fades the new one from the underlying
locomotion/layers. Replacement does not retain a frozen pose of the prior action.
The base and its root motion continue. Actions are in-place: no action root motion
is moved onto the physics/controller entity. Use an empty mask name for all joints,
or a subtree with optional `include_root: false`. Action weight is `(0,1]`, speed
is positive, fades are finite nonnegative seconds; assets bound speed to
`[0.001,1000]` and fade lengths to 3600 seconds. `additive: true` adds the change
from the action's first frame using the existing additive-pose implementation.

`Playing` advances the clip once; at its end `Finishing` holds its last pose and
fades back to the underlying pose, then becomes `Completed`. Large steps consume
the remaining clip time and fade time in one update. Cancellation holds the current
action pose while fading out and revokes markers immediately; status remains
`Cancelled` afterwards. A second cancel does not restart the fade. Passing zero
fade time stops immediately. Cancelling a completed current token can still revoke
undelivered markers from that update; already handled game effects are not undone.
Pausing `Animator3D::playing` pauses all animation; changing its base `speed` does
not change the action's independently authored speed.

Tokens are opaque process-local playback identities, not entity handles, save data
or network IDs. Retain the actor's checked `SceneEntity3D` separately. Only the most
recent action result is retained: a replaced, reloaded or restored player's older
tokens report `Superseded`; an invalid token reports `None`. Rebinding/removing a
model cancels the current action, including an already cancelling fade. Tokens
are not recycled by new play requests, including across actors and pooled reuse.
Native snapshots/copies still require the owning entity handle to identify the
actor. Define native actions with a shared `CharacterActions3D` table assigned to
`Animator3D::actions`; standalone players expose equivalent `play_action`,
`cancel_action`, `action_status`, `action_busy`, `action_time` and `action_weight`.

Marker tracks accept `source: "base"` (the backward-compatible default),
`"action"`, or `"both"`. Use action-only tracks for contact/gameplay effects.
Normal pose layers remain silent. Masked actions can keep locomotion footsteps
with `base_events: true`; false suppresses base events for every step in which the
action slot is busy, including its exit fade. Action and base crossings share the
same **16-event** cap, ordered by time within the step; base wins exact ties.
Overflow still counts all dropped crossings. Native consumers can inspect
`AnimationEventBatch3D::actions[i]`: zero means base, otherwise the owning token.
Before dispatching a copied action occurrence, check `action_event_valid(token)`.
`dispatch_animation_events3d` does this automatically before each callback, so a
cancel/restart inside a handler suppresses later events from the old playback in
that same batch. Final contact markers remain valid when a large step naturally
completes the action before dispatch.

There are at most 32 authored definitions; names/clip/joint strings are capped at
95 bytes. Definitions and marker strings are shared. There is one reusable subtree
mask per player's action, bounded by its validated skeleton size; `PoseMask3D::set_subtree`
reuses its storage. Playback creates no private model, clip, pose history, action
queue or completion queue. The current 64-bit build uses an 88-byte inline action
state in the player and 128 extra bytes per event batch for token ownership, plus
one shared definition reference per animator. The first use of a mask may allocate
its joint weights; warmed play/restart/update needs no extra C++ allocation for
this state. Ordinary blend/IK/handlers retain their own costs.

Camp demonstrates an authored left-hand reach over locomotion. The small glTF
Pickup clip is added by `tools/assets/add_summit_pickup.py`; its original mesh,
texture, rig and prior animations are preserved. The button selects and locks a
checked supply handle. The contact marker rechecks that same handle, grounded
state, reach, visibility from player and fixed camera, and query saturation. A
valid contact submits a checked scene despawn; the game awards supplies only once
that command succeeds at the late boundary. Movement out of reach, new obstacles,
despawn, jump, action replacement and scene restart cannot award a stale pickup.
The award runs in the following Animation phase; extra rendering never advances
an action or completes an interaction.

## Native 3D interactions

`<kin/scene/interaction3d.hpp>` shares target discovery and contact validation over
`SceneQueries3D`. Games supply a read-only callback returning an optional
`InteractionOffer3D`: a current world-space contact point and a nonzero native
verb ID. Return no offer for objects the actor cannot currently use. Game code
maps IDs to prompts, input, animations and effects; no inspector registration or
central ability registry is required.

```cpp
constexpr InteractionVerb3D Collect = 1;
// Construct once; retains one sphere, never a candidate vector.
InteractionQuery3D interactions{{.reach=1.25f,
    .candidates=layer_bit(layers3d::Sensor),
    .candidate_sensors=true,.sight_sensors=true}};
std::array<SceneOverlapHit3D,8> candidates;
InteractionObserver3D observer{queries.handle(player), hand_position, camera_position};
auto describe = [](EcsEntity entity) -> std::optional<InteractionOffer3D> {
    if (!entity.has<MySupply>() || !entity.has<WorldTransform3D>()) return {};
    return InteractionOffer3D{
        entity.get<WorldTransform3D>()->matrix.apply({}) + Vec3f{0,.3f,0}, Collect};
};
auto selected = interactions.select(queries, observer, candidates, describe);
if (selected) {
    // Store alongside the actor's existing ActionToken3D when input starts use.
    InteractionFocus3D locked = selected.focus;
    // At contact: rebuild observer from the current fixed-step actor/camera pose,
    // check the owning action token, and rerun current game eligibility.
    auto contact = interactions.revalidate(queries, observer, locked, describe);
    if (contact) { /* game callback submits its checked scene command */ }
}
```

`select` checks native body identity, applies game eligibility, limits the contact
point by reach and requires the first ray hit to be the candidate. An optional
view adds a second independent sight ray; all actor bodies are ignored. Sensors
and masks are separate for discovery and sight. Unknown native obstacles block
sight. Exact distance ties choose the lower entity ID. Offers use world-space
points: adding an offset after transforming the origin avoids scaling a hand
height by the model's asset scale.

Results distinguish `Ready`, `NoTarget`, `Overflow`, `Unavailable` and
`InvalidObserver`. Saturated caller storage (including zero capacity) returns
`Overflow` with no focus, before invoking eligibility callbacks. Callback work
must be read-only and synchronous; the utility retains neither it nor its offers.
Discovery requires the body's geometry to overlap the reach sphere as well as
the contact point to be in range. Candidate masks apply only during discovery;
`revalidate` reruns game policy directly on the original checked target and verb,
so policy must enforce current eligibility. A changed verb, destroyed/recycled
lease, old scene, invalid actor/point or failed sight/range check rejects contact.
Revalidation never substitutes a newly nearest object. It does not require the
old native body to persist: a current rebuilt body on the same live entity can
still be usable if the current policy and sight checks pass.

Settings are immutable after construction; invalid nonpositive/nonfinite reach
throws. Set `require_sight=false` for pure proximity interactions. Otherwise a
zero-length sight line fails closed because it cannot prove a first target hit.
Run on the scene owner thread after physics synchronization. As with scene
queries, no implicit synchronization, simulation or world ownership occurs.
The utility does not own action tokens or effects. Check the actor's action token
before contact, apply game conditions such as grounded state, and award only
successful deferred scene commands. Keep overflow policy explicit when a game's
ongoing interaction also depends on that step's discovery results.

Camp uses this API for bounded supply discovery, marker-time checked pickup
contact and direct known-target crate pushing. Its native verbs map to keyboard
and gamepad prompts. Game inventory, impulses, action cancellation and command
acknowledgement remain ordinary C++ systems. The utility itself performs no
per-call C++ allocations after warm-up; game callbacks can impose their own costs.

### Pointing at an entity

Choosing with the pointer is a question about pixels rather than reach. Give the session
`options.view.pick = true` (one more pass over the opaque meshes every render; off by
default, and a `SceneEnvironment3D`'s view settings never turn it off), and
`pick(pixel)` reads the last rendered frame, drawing nothing: the entity under the pixel
and the world point there. `MeshRenderer3D`, `ModelRenderer3D` and `SkinnedMeshRenderer3D`
draw with their entity's low 32 bits, or with an `id` of their own; the session maps
either back to the live entity, the child that carries the renderer (walk its parents for
a prefab root). It names no entity for a `compose` draw or for an entity hidden or gone
since the render. The result holds an empty optional over the background; disabled
picking returns a failed result with diagnostics. Like `Renderer3D::pick` it waits for the GPU: ask on a click
or when the pointer moves, not every frame.

```cpp
if (input.pressed("select")) {
    auto picked = session.pick(pointer_in_view);
    if (!picked) report(kin::diagnostics3d_text(picked.diagnostics));
    else if (const auto& hit = *picked; hit && hit->entity) select(hit->entity);
}
```

## Using a game-owned world and physics

`SceneSession3D` is the owning convenience layer. A game that already owns its
ECS/physics world can use the same loader, bindings and frame stages separately:

```cpp
#include <kin/scene/scene_loader3d.hpp>
#include <kin/scene/scene_frame3d.hpp>

kin::EcsWorld world;
kin::register_builtin_editor_components(world);
kin::WorldRenderState3D rendering{world};
kin::SceneAssetRuntime level;
// assets is the game's renderer-scoped Assets3D.
if (!level.load(assets.root() / "scenes/level.kinscene", assets.root(), world.components())) {
    // Inspect level.diagnostics().
}
auto bound = kin::instantiate_scene3d(level, world, assets);
if (!bound) return report(kin::diagnostics3d_text(bound.diagnostics));
// Binding failure removes only new entities.

kin::SceneFrame3D frame{world, rendering};
// Each fixed update:
auto stepped = frame.step(dt);
if (!stepped) report(kin::diagnostics3d_text(stepped.diagnostics));
kin::Scene3D scene;
kin::ViewOptions3D options{.pick = true}; // pick is the game's; the rest the scene's
// Each render: frame.collect(scene, &options), then custom draws and the game's
// own Renderer3D::render(view, scene). The game owns/reuses/resizes its View3D.
```

`collect` fills `options` from the scene's `SceneEnvironment3D` (samples,
background, HDR, exposure, bloom) and keeps `options.pick` as the game set it:
a pick pass is the game's choice, not an authored one, on this path as in the
session (`scene3d_view_options` is the one rule). Make the view from the
collected options, and again when they change; `Renderer3D::pick(view, pixel)`
then names the mesh under a pixel by its `MeshInstance3D::id` (an entity's low
32 bits, or the renderer's own `id`).

`SceneAssetRuntime` still loads `.kinscene`/`.kinprefab` through the shared
registry. `instantiate_scene3d` binds only the new instance's models, materials,
animators and particles. Its `SceneBindingOptions3D::physics_assets` optionally
binds collider assets too. It neither creates nor steps native physics. Empty
instances do not bind unrelated entities. Additive loads preserve the world's
scene header by default; `update_scene_header` explicitly opts into replacing it.
Authored IDs must be unique in the destination world. Register native components
before loading. Scripted 3D scenes use `SceneSession3D`, which owns Lua execution;
the additive helper refuses them rather than omitting their lifecycle.

Call `level.clear(world)` to remove the instance while the world is alive. Its
entity descendants are owned with it: detach game-owned children first. A failed
binding leaves pre-existing entities intact; successful asset-cache entries may
remain shared. This path does not promise transactional replacement of an entire
live world: explicitly unload/reload at a game boundary, or use the owning
session for isolated candidate-world replacement, checked lifecycle commands,
preloaded prefab pools and live-edit transactions.

`SceneFrame3D` borrows the ECS world and its `WorldRenderState3D`. Give it
`SceneFrameOptions3D::bodies = &existing_bridge` to reuse an existing
`WorldPhysicsState3D` and its `World3D`, including unrelated native obstacles.
For a raw simulation, instead supply `physics_step = [&](float dt) {
    game.step(dt); }`. The callback runs once at the Physics boundary; the game
owns synchronization with its ECS representation. Supplying both step owners is
rejected. A game that already steps physics can supply neither.

The driver runs ECS phases with native physics after Physics, animation/clocks/
particles after Animation, optional ECS audio after Audio, and a final validation
and transform propagation after Editor. Custom `SystemPhaseHooks::after` run
before the native stage, after ECS command buffers flush. A failure stops later
phases without undoing completed gameplay. Hosts with an existing phase loop can
call `after_phase(phase, dt)` instead of `step(dt)`; never call both for one step.
The owning session uses these same stages and collection code.

No world, scene, view, assets or GPU resources are owned by the frame driver.
Collection fills caller-owned scene/view options and does no view creation.
Its optional renderer is used only for rope meshing; without one it remains a
CPU-only path. Owner-thread calls only; borrowed objects outlive the driver.

### One scene runtime for 2D and 3D

Set `SceneAppConfig::rendering_3d = true` to expose the runtime's shared renderer
as `SceneContext::renderer3d` through enter/update/render/exit in windowed,
headless and command-server runs. `ServerConfig` has the same opt-in. Camp,
Projectile Range and Summit use it; each retains ownership of its own gameplay
and assets. It is released after scenes shut down, before the 2D renderer dies.

The default is false: existing 2D games allocate no `Renderer3D`. The pointer is
null when not requested. A requested renderer can still have `available() ==
false` on a backend without 3D: CPU loading and simulation remain usable. This
option does not select a GPU backend; use the normal backend configuration.
A 2D-only build rejects the opt-in with a diagnostic rather than silently
ignoring it. No 3D definitions or dependencies are needed by its scene context.

## Runtime failure reporting

Fallible 3D loading, rendering, binding and authoring calls return `Result3D<T>`
or `Status3D` for operations without a payload. Both are `[[nodiscard]]`. Each
result owns its diagnostics; later calls cannot overwrite a saved failure.
Inspect the result before dereferencing it. Successful calls have no diagnostics,
and failed results have no payload. `diagnostics3d_text()` joins messages for a log
or HUD; successful draw submissions allocate no diagnostic history.

```cpp
auto loaded = assets.load_model("models/prop.glb");
if (!loaded) return show_error(kin::diagnostics3d_text(loaded.diagnostics));
auto submitted = scene.draw(**loaded, transform);
if (!submitted) return show_error(kin::diagnostics3d_text(submitted.diagnostics));
auto rendered = r3d.render(view, scene);
if (!rendered) show_error(kin::diagnostics3d_text(rendered.diagnostics));
```

`Result3D<bool>` deliberately has no bool conversion: success and the bool payload
answer different questions. A model watcher reports a successful `false` when the
source has not changed. An unchanged failed
source returns its cached failure without retrying; modifying/remounting it retries.
A failed reload preserves the last good model.

```cpp
auto changed = file.poll();
if (!changed.ok()) show_error(kin::diagnostics3d_text(changed.diagnostics));
else if (*changed) rebind(file.model());
```

Owner `diagnostics()` accessors are inspection snapshots, valid until the next
operation. Use returned diagnostics to retain a particular failure. Resource and
session operations replace these snapshots; observing state/counters does not
clear them. Calls belong on the owner thread. Legacy error-output parameters are
removed. C++ query misses (for example no pick hit), predicates and checked stale
handles keep their documented optional/bool semantics; they are not operational
load failures. Lua gameplay entity arguments instead raise the common stale-handle
error; only explicit liveness queries return false/nil. CPU builders and
validators use returned results too.

`Scene3D` accumulates commands and latches the first rejected submission until
`clear()`. A failed draw leaves existing commands and poses intact. Rendering and
warm-up reject an invalid scene, including its overlay, before GPU work. This
allows a host that checks the final render result to deliberately discard individual
submission results with `(void)`. `clear_diagnostics()` explicitly acknowledges a
refusal while retaining valid commands; normal frame construction uses `clear()`.
A failed retained-batch update preserves its records, bounds and revision. Batch
factories return a failed result, with no batch payload, on invalid input.

`instantiate_scene3d` returns `Status3D`; failed binding removes its new instance
and retains existing entities. Deferred lifecycle requests expose completion status
and an immutable failure message. SDL_Renderer fallback rendering succeeds after
drawing the view background; `available()` and capabilities still distinguish it
from the full GPU 3D implementation.

Recoverable runtime validation and submission do not throw. Invalid fixed
constructor configuration (session capacities, frame ownership/audio pairs) is a
programming-contract exception. Allocation failure is not converted into a promise
of recoverable operation; these APIs are not `noexcept`.

Headless `SceneApp` rendering supplies `alpha=1`: its fixed step is already
complete. Windowed hosts pass the accumulator fraction to `collect(alpha)` or
`render(alpha)`. The sampled presentation includes body-parented cameras, ragdoll
bones, joint attachments, IK targets and rope geometry while authoritative ECS and
physics state stay unchanged.


## Public statistics and debug controls

`Renderer3D::stats()` returns the compact `RenderStats3D` summary from
`kin/render3d/stats3d.hpp`: 20 counters/flags for submitted geometry, lights,
shadows, skinning, particles, CPU recording time, first-use pipelines and the
reported feature/local-shadow allocations. Those allocations are not total
VRAM. CPU recording time is not GPU or complete-frame time. Reading the summary
never waits for the GPU.

Detailed implementation instrumentation is available explicitly through
`Renderer3D::debug_stats()` (`RenderDebugStats3D`,
`kin/render3d/debug_stats3d.hpp`). It includes retained tables, buffer uploads,
cache payloads, per-pass dispatches, light slots, LOD histograms and cluster
counters. These fields may change with the renderer implementation. Both APIs
observe the same last render, including overlays; the renderer keeps one
counter store and does not copy a detailed report into every summary.

GPU cluster selection is asynchronous. With debug readback off, `triangles`
counts only the main-pass triangles known to the CPU and `triangles_complete`
is false when GPU-selected geometry contributes. It is a partial count, not an
estimated total. Explicit readback makes the count complete when the GPU reads
succeed; CPU cuts and scenes without GPU-selected geometry need no readback.
Neither stats getter initiates a readback, changes errors, or retains history.
A new render resets both reports, including after a rejected render.

Visual quality stays in `scene.taa` and `scene.lod`. Diagnostic overrides live
in `scene.debug` (`RenderDebugOptions3D`, `kin/render3d/debug3d.hpp`):

```cpp
scene.lod.enabled = true;
scene.lod.error_pixels = 1.0f;
// Only when inspecting a rendering problem:
scene.debug.force_level = 2;       // -1 restores normal LOD selection
scene.debug.gpu_culling = false;   // compare against the CPU cluster cut
scene.debug.freeze_jitter = true;  // hold the first TAA jitter sample
scene.debug.stats_readback = true; // blocking GPU cluster readback for profiling
scene.debug.gpu_pass_markers = true; // external GPU profiler labels, no readback
if (!r3d.render(view, scene)) return;
const auto& summary = r3d.stats();
const auto& details = r3d.debug_stats();
```

The `kin3d.prepass` GPU group contains nested `kin3d.prepass_geometry`,
`kin3d.ao`, contact-shadow and GI-resolve ranges. Inside `kin3d.ao`, the depth
chain, horizon search, horizontal/vertical blur and optional upsample have
separate labels. These groups stay in the existing command buffer and introduce
no timing readback or submission splits. Parent and child durations overlap;
do not add them when attributing total GPU time.

When storing a detailed snapshot, use `RenderDebugStats3D`; copying it into
`RenderStats3D` intentionally keeps only the compact summary.

Migration from the combined API:

| Previous access | Explicit access |
|---|---|
| `r3d.stats().retained_gpu_bytes` (and other implementation counters) | `r3d.debug_stats().retained_gpu_bytes` |
| `scene.lod.force_level` | `scene.debug.force_level` |
| `scene.lod.gpu_culling` | `scene.debug.gpu_culling` |
| `scene.lod.stats_readback` | `scene.debug.stats_readback` |
| `scene.taa.freeze_jitter` | `scene.debug.freeze_jitter` |

Defaults select normal rendering and no blocking readback. Debug overrides are
not serialized scene quality settings. They belong to each scene; the overlay
can have its own forced LOD. GPU/CPU cluster selection and cluster readback use
the main scene's options for the whole view; TAA jitter also belongs to the view.
`Scene3D::clear()` preserves quality and debug options while clearing commands.
`SceneFrame3D::collect()` rebuilds the main scene's quality and debug options
from defaults; set overrides after collection or in the session's compose callback.

The existing `record_render3d_diagnostics()` publishes only the public summary
to F1/capture/profiling sinks. Tools can explicitly add implementation rows with
`record_render3d_debug_diagnostics()` from
`kin/render3d/debug_diagnostics3d.hpp`. Neither sink performs GPU reads or owns
renderer resources. Benchmarks and reference tools consume `debug_stats()` so
supported CSV/report counters remain available; retired experiment columns have been removed.

## API names, colour inputs and 2D builds

See [the 3D API migration guide](3d_api_migration.md) for `LinearRgb` light inputs, the suffixed type/header names, the 2D-only preset and the formatting check.


### Fixed-step presentation and temporal effects

Call `session.step(fixed_dt)` from the fixed update and
`session.render(accumulator / fixed_dt)` once per displayed frame. The session
interpolates dynamic/kinematic bodies, character positions and rotations,
particle sprites and child draw placements without
writing gameplay transforms or advancing physics. Joint attachments use the
same root placement. Linked traversal hands sample moving holds and rope
polylines at that alpha, including hand rotation and surface clearance.
Collection preserves gameplay-authored targets and native traversal state.
Explicit `RenderTransform3D` remains the host's override;
pass the same alpha to a game-owned `CameraRig3D`.

Transparent TAA uses reprojected particle quads and blended-mesh velocities,
with bounded history confidence for the composite. New/discontinuous coverage
is rejected locally; unrelated instance motion does not reject every effect.
Opaque custom materials use the opaque velocity path. Simulated particles have
stable birth identities; immediate callers may set `ParticleSprite3D::id` and
an emitter `history_id` on `draw_particles`. Zero particle/emitter IDs use submission order.
`clear()` and a changed-state `restore()` start a new presentation identity epoch.
Restoring identical bytes preserves the existing presentation too. For rollback,
use `restore(bytes, ParticleRestore3D::Rollback)` and resimulate with motion capture to preserve birth
IDs and rebuild the corrected interpolation segment. Random birth indices,
state hashes and rollback bytes keep their original
simulation meaning. ECS interpolation retains compact motion/life samples,
not complete rendered sprites on every fixed step.
History retains the nearest 65,536 visible sprites per view; additional sprites use
spatial reconstruction. Overlapping independently moving translucent layers
share the foremost contributing velocity, so history confidence is capped.
Blended mesh and particle coverage reject history where opaque geometry behind them moves
independently of the camera beyond a fraction of the projected pixel footprint,
including the following frame's old footprint. The threshold follows projected pixel size,
not world units.
Static geometry at different depths retains history during camera translation
and rotation, including triangles that extend behind the camera. Independent
motion is measured at visible fragments, so a vertex behind the camera does not
turn sub-pixel motion into a full-face rejection. Derivative footprints are
evaluated before cutout discards. The existing motion/normal target carries the marker; there is
no extra opaque-motion target or fullscreen copy. Lit particles record local
irradiance as shadow receivers do. Small lighting changes retain temporal coverage
samples; larger changes lower history weight locally over the full reconstruction
footprint. Frozen-jitter inspection uses current illumination wherever it changed.
These changes do not reset particle geometry identities. Transparent mesh appearance is recorded per matched
instance alongside its velocity, with material partitions for anonymous draws.
Anonymous transparent copies first try the same collection index within twice
the copy's transformed bounding radius, then fall back to spatial matching.
If that search misses, a final same-index fallback accepts motion up to eight
radii. Separated copies can be added, removed or reordered without changing another
copy's appearance history, including shared materials. Use explicit instance IDs
for coincident copies, nearby reorderings or large jumps whose identity cannot
be inferred reliably from order and position. Appearance records
are allocated only for transparent copies; opaque copies keep the ordinary
velocity history.

Particle casters are culled individually against each light map and capped at
16,384 per map in stable submission order, including each point-light face.
`particle_shadow_map_dropped` counts per-map budget omissions across the frame.
The global caster upload cap remains 1,048,576. Local shadow caches depend only
on their selected particle casters. Only maps selected for redraw pack and upload
casters; `particle_shadow_uploaded_bytes` is zero when all caster maps are cached.
Flat sprites do not depend on the camera;
billboard maps update when their actual camera-dependent quads change.

Lua bridges reflected ECS entities and session handles with
`scene3d.handle(entity)` and `scene3d.resolve(handle)` (nil for stale handles).
Every gameplay function taking an entity argument, including event filters, IK
and query ignore options, raises `stale or foreign entity handle` when the
entity was destroyed, belongs to an old scene generation or has an expired pool
lease. Reflected entity access and deferred command targets use the same error.
`alive()`/`valid()` remain false-returning liveness predicates; `id()` remains
available for diagnostics. Action-status tokens retain their historical status
and cancellation returns false after their actor expires. Missing components on
a live entity retain each function's documented false/nil result.
The authored skinned-renderer name is `SkinnedMeshRenderer`, while C++ uses
`SkinnedMeshRenderer3D`. Camera rig angles are degrees and distances are metres;
spring-arm `ease_out` is metres per second and `shoulder_radius_scale` is
unitless.

Lua `scene3d.teleport(entity, position, rotation?)` instantly moves a bound rigid
body in world space, clears velocity and resets interpolation. `position` is a
finite `{x,y,z}` table; optional `rotation` is a unit `{x,y,z,w}` quaternion and
omission preserves the native rotation. The call requires a physics scene and
mutation callback/system, returns false for a live entity without a bound rigid
body, and raises on malformed arguments or stale handles. This is the explicit
way to teleport a kinematic body; ordinary Transform3D animation continues to
sweep.


### Stable object identities

`MeshInstance3D::id` remains the 32-bit picking value. Its separate 64-bit
`history_id` identifies a live copy for TAA, independently of picking IDs and
submission order. Supply a unique nonzero value for each copy and keep it until
that object is replaced. Mesh/model `draw` and `draw_skinned` accept the same
identity as their final argument; models derive distinct identities per part.
Zero retains the direct API's legacy ID/order/spatial matching. ECS collection
automatically includes entity generation, collection scope and prefab lease,
so changing picking IDs does not reset history and a recycled lease starts fresh.
See [the migration guide](3d_api_migration.md#final-presentation-api-adjustments)
for the named particle modes, read-only interpolation and physics debug settings.

### Tracked scene extraction

`WorldRenderState3D` defaults to `SceneMutationMode3D::Conservative`, which sees raw mutable component edits at every transform barrier and collection. An owner that publishes its edits can opt into `state.mutation_mode(SceneMutationMode3D::Tracked)` (or use `session.rendering()` after loading). Use `set<T>()`, or call `modified<T>()` after editing a mutable component. Call `state.invalidate()` after external bulk changes that cannot publish notifications. World mutations and rendering must follow the world's owner-thread/deferred-merge contract.

Tracked mode caches hierarchy topology and inherited transform sources, then updates dirty entities and descendants. Static mesh/model collection reuses retained instance batches; transform edits update their affected slots. Previously collected scenes retain their snapshots. Skinned animation, node animation, lights, particles, joint attachments and presentation callbacks currently use the full collection path. Structural/material changes conservatively rebuild extraction. This mode is an explicit mutation contract: an escaped pointer edited without notification can remain stale until invalidation.

`state.work_stats()` exposes transform visits/recomputations, mesh/model collection visits, sparse record updates, collection fallback/reuse, particle validation runs/reuse and estimated tracking capacity. `reset_work_stats()` clears counters. `SceneFrame3D` reuses particle validation only in tracked mode and invalidates it on configuration/structural edits, including disabled retained emitters. Native physics and root motion publish actual transform edits.

The `kin_scene_work_bench` target emits CSV for 10,000/100,000 entities, four transform barriers and static/1-percent-edit cases. Its defaults are exploratory (50 warm-up/150 measured frames, five repeats); use `--warmup`, `--frames` and `--runs` for the longer comparison protocol.

### GPU resource payload

`renderer.backend_stats().gpu_memory` reports native SDL_GPU texture, buffer and transfer payload, pooled idle texture payload, peaks, handle counts and allocation/release/failure events. Format block dimensions, mip levels, layers/depth and samples are included. Pooled texture bytes are a subset of texture bytes. These numbers describe Kin-owned handles; they exclude hidden SDL cycle backings, driver alignment, GPU retirement and physical residency. Existing per-view feature and mesh-pool counters provide additional attribution.

Static model parts use the same tracked path as mesh renderers. Each changed model placement updates its part records using the model's immutable local transforms; asset/material resolver changes publish notifications automatically. Node/skinned animation and arbitrary placement callbacks retain the conservative collection fallback.

See [compiled resource graphs](render_graph.md) for public pipeline composition and `kin_ascii3d`. The current material shader interface is version 2 (`kin/render3d/material_interface3d.hpp`, GLSL `KIN_MATERIAL_INTERFACE_VERSION`). Its material block keeps the previous 48-byte prefix and appends independent RGB emission at byte 48, for 64 bytes total. Recompile custom shaders to use the new emission helper/fields.
