# 3D API naming and colour migration

Experimental 3D C++ types now end in `3D`. Shared colour and math types (`Color`, `LinearRgb`, `Vec3f`, `Quatf`) remain dimension-independent. ECS `PointLight3D` and `SpotLight3D` describe entity-mounted lights; the renderer takes positioned `PointLightSource3D` and `SpotLightSource3D` records.

All 3D light, sky, fog, time-of-day, particle and weather RGB inputs and outputs use `LinearRgb`: three float channels named `r`, `g`, `b`, with no alpha or implicit vector/sRGB conversion. Values above one retain HDR energy. Material, vertex and instance tints continue to use sRGB `Color`.

```cpp
scene.lights.sun.color = kin::to_linear_rgb(kin::Color::rgb(255, 220, 180));
scene.lights.sun.color = scene.lights.sun.color * 3.0f;
scene.lights.ambient = kin::LinearRgb{0.25f, 0.25f, 0.28f};
```

`to_linear_rgb` uses the existing sRGB transfer function. `linear_rgb(LinearColor)` explicitly drops alpha from an already-linear colour; `rgb.with_alpha(a)` supplies alpha for existing `LinearColor` APIs. Scene and prefab RGB triples keep their linear floating-point representation, including HDR values. Geometry directions and scattering coefficient calculations remain `Vec3f`. Public sky/fog colour functions and `particle_light_tint` return `LinearRgb`; `fog_apply` also accepts a `LinearRgb` surface colour. Use `.r/.g/.b` for colour channels and `mix` to blend them. Scene/prefab RGB fields and particle recipes keep their existing names and linear triples; their shared field codec still uses numeric `{x,y,z}` tables in Lua.

`Lights3D::ground_ambient` now uses `GroundAmbient3D { enabled, color }`, matching the other optional visual features. Replace optional assignment/reset with:

```cpp
scene.lights.ground_ambient = {.enabled=true, .color={0.1f, 0.08f, 0.06f}};
scene.lights.ground_ambient.enabled = false; // retain the configured colour
```

Fallible 3D operations now return `Result3D<T>`; operations with no payload return
`Status3D` (`Result3D<void>`). Check `result.ok()` or `if (result)`, read the
payload with `*result`/`result->`, and report `diagnostics3d_text(result.diagnostics)`.
Use `.ok()` when supplying a bool parameter; the bool conversion is explicit.
`Status3D` is now `[[nodiscard]]` too. `Result3D<bool>` has no bool conversion:
check `.ok()` for success and `*result` (after checking) or `.value_or(false)`
for the payload. Replace `if (file.poll())` with a checked result then `if (*changed)`.
Unchanged failed model sources return cached diagnostics; only source edits or
pack remounts retry. Owner `diagnostics()` accessors remain inspection snapshots.
`Model3D::asset_diagnostics` has been removed. Check the diagnostics on the failed
`load_model()` result; asset loaders reject the whole model and return no drawable
fragments. CPU inspection still exposes `ModelData3D::asset_diagnostics` for tools
that deliberately inspect unsupported content.

Each result owns its diagnostics, so later calls cannot overwrite an earlier
failure. A successful `false` or empty payload is still success. Failed results
have no value. `value_or(fallback)` explicitly chooses a fallback.

```cpp
auto loaded = assets.load_model("character.glb");
if (!loaded) return report(kin::diagnostics3d_text(loaded.diagnostics));
auto character = std::move(*loaded); // AssetHandle<Model3D>
auto rendered = session.render(alpha);
if (!rendered) report(kin::diagnostics3d_text(rendered.diagnostics));
```

This replaces handle-plus-owner-error, bool-plus-owner-error, raw diagnostic
vectors, parser-specific result structs and public error-output pointers on the
3D loading/rendering/authoring boundaries. CPU builders return their value:
`read_model_data(bytes,path)`, `read_model_geometry3d(path,limits)` and
`build_navmesh3d(...)`. `Model3D::placement()` now returns `Result3D<Mat4f>`, including invalid placement options as a failed result. Mesh/skeleton validation and asset binding return
`Status3D`. `ModelFile3D` construction now only configures the watcher: call `poll()` for the initial load, so its failure is returned explicitly. `ModelFile3D::poll()` returns `Result3D<bool>`: `false` means unchanged,
an absent payload means failure. `reload_if_changed()` returns
`Result3D<SceneSession3DReload>`; readback methods return `Result3D<bool>` where
a successful false means no GPU cut. Use `InstanceBatch3D::create(...)` and
`ModelBatch3D::create(...)` instead of fallible constructors. Existing owner
`diagnostics()` queries remain inspection snapshots; use the returned result to
retain a particular operation's failure. Scene submissions still latch a refusal
until `clear()`/`clear_diagnostics()`, so rendering also rejects an unchecked bad
submission.

Navmesh builders accept `max_step_height` (metres, default zero). Enable it to
connect matching stair-tread boundaries; paths retain both portal heights for the
movement controller. Radius erosion remains conservative at whole-triangle
resolution and now propagates only from nearby boundaries. Recompile consumers of
`NavMeshBuildOptions3D`/`NavTriangle3D`; their layout now includes step metadata.

Native `World3D` result predicates/mutations and `StepReport3D` now warn when
ignored. `rope_points(handle, output_vector)` provides reusable readback storage.
Physics hashes include temporary mass even when zero; regenerate stored expected
hashes with this build. Summit checks replay hashes and exact simulation step
counts against a bare run from the same toolchain; cross-toolchain checks retain
gameplay outcomes and duration bounds. `IRendererBackend3D::pick` returns a result, preserving
readback errors instead of converting them into successful misses.

## Gameplay and content changes

- `World3DConfig::contact_events` defaults to `false`. Enable it explicitly for
  rigid-body Lua contacts or native contact callbacks. `PhysicsContacts3D` still
  exists as an empty readable singleton when collection is disabled.
- `SceneSession3D::step(dt)` remains one authoritative fixed step. Pass the host's
  accumulator fraction to `collect(alpha)` or `render(alpha)` for interpolated
  body/character draw placement, including descendants and attached props. The
  default `alpha=1` preserves existing callers. Explicit `RenderTransform3D`
  overrides take precedence. ECS and native physics poses are never changed by
  collection. Body-parented cameras, ragdoll poses, ropes and IK targets now use
  the same sampled pose. Linked traversal hands include hold rotation, clearance
  and rope-polyline motion; manually edited targets retain gameplay control.
  Camera rigs retain their own matching alpha. Headless
  `SceneApp` supplies `alpha=1`; remove per-host `headless() ? 1 : alpha` workarounds.
- Lua `scene3d.handle(ecs_entity)` converts a reflected `KinEntity` to a checked
  session handle; `scene3d.resolve(handle)` returns a reflected entity or nil if
  stale. Use these around physics hits, prefab roots and ordinary ECS queries.
  Gameplay calls with stale entity arguments now consistently raise
  `stale or foreign entity handle`, including reflected access, deferred command
  targets, event filters and query/IK options. Use `alive()` or `resolve()` before
  optional work; missing components on a live entity still return false/nil.
  Historical action tokens keep their query/cancellation semantics.
  Malformed supplied options, wrong userdata and wrong-typed optional arguments
  raise a Lua error. Nil/omitted optional arguments retain their defaults. Checks
  are local to the 3D bindings; the public engine-wide `SOL_ALL_SAFETIES_ON`
  definition is removed, preserving existing 2D Lua numeric conversions.
- Authored component names are schema identifiers, independent of C++ spelling:
  write **`SkinnedMeshRenderer`** in scene/prefab JSON and reflected Lua calls;
  the C++ type is `SkinnedMeshRenderer3D`. Existing asset names are unchanged.
- `ParticleEmitter3D::emitting` gates automatic births alongside the native
  `ParticleSystem3D::emitting` flag. The bridge preserves both controls. Bursts
  remain explicit and existing particles finish their lifetimes.
- Particle TAA matches stable `ParticleSprite3D::id` values within an emitter.
  `draw_particles(..., history_id)` optionally identifies the emitter across
  reordered submissions; zero uses submission order. ECS emitters and simulated
  particles supply these identities automatically. Update binary consumers for
  the appended fields/defaulted arguments. Recompile custom particle vertex
  shaders against the current shared includes.
- Particle shadow casting and reception are independent opt-ins on
  `ParticleLook3D`. Casting uses `shadow_cutoff` and a separate bounded stream;
  each map selects at most 16,384 sprites after light-frustum culling.
- Models must satisfy the tested [kin-gltf/1 profile](gltf_profile3d.md).
  Distinct skin palettes, morph targets and unsupported pose/UV combinations now
  fail whole-asset acceptance instead of producing partially usable models.
  Navmesh agent height/radius now filter triangles during construction. Rebuild
  navmeshes with the controller's dimensions and sufficiently tessellated floors.
- 3D is now opt-in at configure time: use the `3d` preset or explicitly enable
  `KIN_EXPERIMENTAL_3D`. The default and shipping presets exclude it.

For runtime lights in a session, create/patch `PointLight3D` or `SpotLight3D`
entities with `Transform3D`; collection owns their positioned renderer records.
Immediate draws use `Scene3D::add_point_light` / `add_spot_light`. Position and
spot direction come from the entity transform in ECS. All paths now share the
following flat field names, including reflected Lua and scene/prefab JSON:

| Field | Units |
| --- | --- |
| `color` | linear RGB, HDR allowed |
| `radius`, `size` | metres |
| `inner_angle`, `outer_angle` (spot) | half-angle degrees |
| `shadow_enabled` | boolean |
| `shadow_resolution` | texels; zero uses the scene default |
| `shadow_bias`, `shadow_normal_offset`, `shadow_softness` | shadow-map texels |
| `shadow_near_plane`, `shadow_distance` | metres; zero distance uses radius |

Replace C++ `light.shadow.enabled` with `light.shadow_enabled`, and similarly
flatten the other former `LightShadow3D` members. Replace authored `shadows` with
`shadow_enabled` and `shadow_near` with `shadow_near_plane`; old field names are
rejected. Reference-harness light JSON uses the same fields and rejects its old
nested `shadow` object. Directional sun/moon and `TimeOfDay3D::shadows` retain
their distinct settings. Entity light `enabled` controls collection separately
from `shadow_enabled`.

Queries use optional hits for ordinary absence; e.g. session picking returns
`Result3D<std::optional<ScenePick3D>>` to distinguish failure from background.
Native physics handles, predicates and bounded query counts remain lightweight
simulation values, not diagnostic owners. Deferred lifecycle requests retain
their pending/cancelled state; Lua programming errors remain catchable Lua
errors. Invalid C++ constructor setup keeps normal `std::invalid_argument`
semantics. Lengths are metres, times seconds and rig angles degrees; spring-arm
`ease_out` is metres per second and `shoulder_radius_scale` is unitless.

## Custom material shaders

Recompile custom material GLSL against the current `kin_material.inc.glsl` to
receive scene fog through `kin_output` and spot/moon lighting and local shadows
through `kin_lit`. `kin_output` also applies the material's alpha cutoff;
`kin_output_unfogged` explicitly bypasses fog while keeping encoding,
premultiplication and cutoff handling. Writing `FragColor` directly remains a
shader-owned output path.

Fragment samplers 0–11 and uniform slots 0–3 are reserved. Slot 2 retains the
four parameter vec4s as its prefix and appends fog data; slot 3 supplies local
lights. Existing compiled shaders retain their parameter layout and original
shading behavior. No scene/prefab or C++ material layout change is required.

## Type names

Update C++ callers using the following names. The experimental API has no legacy aliases; authored component type identifiers remain unchanged; local-light field changes are listed above.

| Previous | Current |
| --- | --- |
| `Bounds3` | `Bounds3D` |
| `ClusterCutTest` | `ClusterCutTest3D` |
| `ClusterCutView` | `ClusterCutView3D` |
| `ClusterDraw` | `ClusterDraw3D` |
| `ClusterParallelFor` | `ClusterParallelFor3D` |
| `ClusterPick` | `ClusterPick3D` |
| `DirectionalLight` | `DirectionalLight3D` |
| `FabrikOptions` | `FabrikOptions3D` |
| `FabrikResult` | `FabrikResult3D` |
| `FogTable` | `FogTable3D` |
| `FootLeg` | `FootLeg3D` |
| `FootPlacementOptions` | `FootPlacementOptions3D` |
| `FootPlacementState` | `FootPlacementState3D` |
| `FootPlan` | `FootPlan3D` |
| `FootTarget` | `FootTarget3D` |
| `GroundHit` | `GroundHit3D` |
| `GroundQuery` | `GroundQuery3D` |
| `IEnvironmentBackend` | `IEnvironmentBackend3D` |
| `IMaterialShaderBackend` | `IMaterialShaderBackend3D` |
| `IMeshBackend` | `IMeshBackend3D` |
| `IRenderer3DBackend` | `IRendererBackend3D` |
| `IView3DBackend` | `IViewBackend3D` |
| `JointChannels` | `JointChannels3D` |
| `JointPose` | `JointPose3D` |
| `LocalShadowView` | `LocalShadowView3D` |
| `LookAtJoint` | `LookAtJoint3D` |
| `LookAtOptions` | `LookAtOptions3D` |
| `LookAtResult` | `LookAtResult3D` |
| `LookAtState` | `LookAtState3D` |
| `Materials3DAsset` | `MaterialsAsset3D` |
| `Materials3DLoadResult`, `MaterialsLoadResult3D` | `Result3D<MaterialsAsset3D>` |
| `ColliderRecipeLoad3D` | `Result3D<ColliderRecipe3D>` |
| `Mesh` | `Mesh3D` |
| `MeshCluster` | `MeshCluster3D` |
| `MeshClusterBuild` | `MeshClusterBuild3D` |
| `MeshClusterGroup` | `MeshClusterGroup3D` |
| `MeshClusterInfo` | `MeshClusterInfo3D` |
| `MeshClusters` | `MeshClusters3D` |
| `MeshData` | `MeshData3D` |
| `MeshInstance` | `MeshInstance3D` |
| `MeshLevelInfo` | `MeshLevelInfo3D` |
| `MeshLodChain` | `MeshLodChain3D` |
| `MeshLodLevel` | `MeshLodLevel3D` |
| `MeshOptions` | `MeshOptions3D` |
| `MeshRenderer` | `MeshRenderer3D` |
| `MeshSkinVertex` | `MeshSkinVertex3D` |
| `MeshVertex` | `MeshVertex3D` |
| `Model` | `Model3D` |
| `ModelData` | `ModelData3D` |
| `ModelFile` | `ModelFile3D` |
| `ModelPart` | `ModelPart3D` |
| `ModelPartData` | `ModelPartData3D` |
| `NoClusterCull` | `NoClusterCull3D` |
| `ParallelFor` | `ParallelFor3D` |
| `PickResult` | `PickResult3D` |
| `PointLight` | `PointLightSource3D` |
| `PoleMode` | `PoleMode3D` |
| `Pose` | `Pose3D` |
| `PoseMask` | `PoseMask3D` |
| `Render3DDebugOptions` | `RenderDebugOptions3D` |
| `Render3DDebugStats` | `RenderDebugStats3D` |
| `Render3DStats` | `RenderStats3D` |
| `Renderer3DCapabilities` | `RendererCapabilities3D` |
| `RootMotion` | `RootMotion3D` |
| `RootMotionAxes` | `RootMotionAxes3D` |
| `SelectedCluster` | `SelectedCluster3D` |
| `ShadowCascade` | `ShadowCascade3D` |
| `ShadowCascades` | `ShadowCascades3D` |
| `ShadowRequest` | `ShadowRequest3D` |
| `ShadowSlots` | `ShadowSlots3D` |
| `Skeleton` | `Skeleton3D` |
| `SkinBound` | `SkinBound3D` |
| `SphereTest` | `SphereTest3D` |
| `SpotCone` | `SpotCone3D` |
| `SpotLight` | `SpotLightSource3D` |
| `SunBox` | `SunBox3D` |
| `TwoBoneChain` | `TwoBoneChain3D` |
| `TwoBoneOptions` | `TwoBoneOptions3D` |
| `TwoBoneResult` | `TwoBoneResult3D` |
| `TwoBoneState` | `TwoBoneState3D` |
| `View3DOptions` | `ViewOptions3D` |
| `SkinnedMeshRenderer` | `SkinnedMeshRenderer3D` |
| `IKTarget` | `IKTarget3D` |
| `IKKind` | `IKKind3D` |
| `IKConstraint` | `IKConstraint3D` |

## Public headers

Paths are relative to `kin/render3d/`.

| Previous | Current |
| --- | --- |
| `environment.hpp` | `environment3d.hpp` |
| `ik.hpp` | `ik3d.hpp` |
| `local_lights.hpp` | `local_lights3d.hpp` |
| `mesh.hpp` | `mesh3d.hpp` |
| `mesh_clusters.hpp` | `mesh_clusters3d.hpp` |
| `mesh_lod.hpp` | `mesh_lod3d.hpp` |
| `model.hpp` | `model3d.hpp` |
| `shadow_cascades.hpp` | `shadow_cascades3d.hpp` |
| `skeleton.hpp` | `skeleton3d.hpp` |
| `taa.hpp` | `taa3d.hpp` |

## 2D-only build

```sh
cmake --preset 2d
cmake --build --preset 2d
ctest --preset 2d
```

The preset builds Release with `KIN_EXPERIMENTAL_3D=OFF` and uses one build/test job. It keeps both SDL_GPU and software 2D rendering, the 2D samples, scripting and scene/prefab support. It excludes 3D rendering/physics sources, glTF and meshoptimizer dependencies, 3D tools and 3D shader compilation/embedding. Shared CPU math remains available. The test preset selects SDL rendering for normal headless app tests; GPU renderer tests can also be run with `KIN_REQUIRE_GPU_TESTS=1` on a machine with a GPU.

For an existing build folder use `cmake -S . -B build-no3d -DKIN_EXPERIMENTAL_3D=OFF`. Game code that calls the 3D implementation should be guarded by `#ifdef KIN_EXPERIMENTAL_3D`. Runtime 3D opt-in in a 2D-only build returns a diagnostic failure.

## Formatting

The repository `.clang-format` defines C++ formatting. Use clang-format 18, matching the rendering/authoring sources:

```sh
python3 tools/format_3d.py
python3 tools/format_3d.py --check
```

Pass `--clang-format /path/to/clang-format` when it is not on PATH. The check exits nonzero for formatting differences and does not write files. It covers the 3D renderer, authoring/runtime glue, samples, tools and their tests; it excludes third-party and generated code.

Scene reload polling reports `Unchanged` or `Activated` in a successful result;
inspect failed `Result3D` diagnostics instead of the removed `Failed` enum value.
`scene3d.entity(id)` returns nil for an absent authored ID. Resolved Lua `KinEntity`
values retain their prefab-pool lease: `alive()`/`valid()` become false after return
and mutation through an expired lease raises an error even if the slot is reused.

Scene generation IDs are monotonic and may have gaps: a failed activation reserves
a unique candidate ID while leaving the active scene's generation and handles valid.

Resolved entity children and saved `component()` editors retain the same pool
lease. Returning the prefab invalidates their later mutations too. The physics
bridge now drains native constraint break events each step; consume its
`constraint_events()` span or the `ConstraintEvents3D` singleton afterward.

Raw ECS query values track physical entities. For a pooled actor, cache a token
from `scene3d.handle(e)` and resolve that token when accessing the current lease.

Finite presentation alpha now clamps to [0,1]; non-finite alpha still fails.
Character rotation and simulated particle sprites follow the same fixed-step
fraction as meshes. Collection leaves their authoritative state unchanged.
For an instantaneous rigid-body move use
`WorldPhysicsState3D::teleport(entity, world_position, world_rotation)` and check
its status. This clears velocity and interpolation history, including for
kinematic bodies. Ordinary kinematic transform animation continues to sweep.

Lua can use `scene3d.teleport(entity, world_position, world_rotation?)` for the
same rigid-body operation. Omit the quaternion to retain native rotation.
Destroyed reflected entities now reject access and repeated `destroy()` in 2D
as well as 3D; check `alive()`/`valid()` for optional work. Native exceptions
reported by either Lua host include the closest Lua caller position.
`EcsDeferredEntity` now carries its creating command batch's scope. Only values
returned by `create_entity()` are valid; do not retain placeholders across
flushes or transfer them to another buffer. Lua enforces the same rule.
Particle sprite IDs include a presentation epoch changed by `clear()` or a
changed-state discontinuous `restore()`. Identical restores leave it unchanged;
`ParticleRestore3D::Rollback` preserves it for rollback/resimulation. This does not change deterministic birth randomness,
state hashes or saved bytes. Rebuild binary consumers for the appended deferred
scope and the particle presentation API/layout changes; authored schemas are
unchanged.


## Final presentation API adjustments

`MeshInstance3D::history_id` is a stable 64-bit temporal identity, separate from
its 32-bit picking `id`. Direct `draw` and `draw_skinned` overloads also accept
it as their final argument. Keep it stable while an object lives, unique among
copies of the same mesh/material, and change it on replacement. Explicit history
IDs take precedence over picking IDs; changing a picking ID preserves temporal
identity. Model submissions derive a distinct history identity for each part.
Zero preserves the existing picking-ID/order/spatial fallback for direct callers.
ECS collection supplies independent identities containing the entity generation,
collection scope and pooled-prefab lease, even when a custom picking ID is set.
Recompile consumers: instance records now include an additional 64-bit field.

`WorldPhysicsState3D::interpolate(alpha)` is deprecated because it writes
presentation into gameplay transforms. Use `SceneSession3D::render(alpha)`,
`SceneFrame3D::collect(scene, view, alpha)`, or `render_transform(entity, alpha)`
for read-only presentation. The deprecated implementation remains available for
migration and retains its existing behavior.

Particle presentation choices use named modes, with unchanged defaults:

```cpp
particles.update(dt, wind, kin::ParticleMotion3D::Capture);
particles.sprites(sprites, alpha);
auto snapshot = particles.save();
// After rewinding, resimulate with Capture to rebuild corrected motion samples.
bool restored = particles.restore(snapshot, kin::ParticleRestore3D::Rollback);
```

Replace the former capture boolean with `ParticleMotion3D::None` or `Capture`,
and the restore boolean with `ParticleRestore3D::Discontinuous` or `Rollback`.
Saving, simulation hashes and rollback bytes are unchanged.

`SceneSession3DOptions::bodies` is replaced by `physics_debug`, of type
`PhysicsDebug3DSettings { enabled, options, contacts, grips }`. Sessions take their
fixed timestep and catch-up policy from the host. The unused session settings
`bodies.fixed_dt` and `bodies.max_steps` are removed. Standalone bridges retain
`WorldPhysicsState3DConfig::{fixed_dt,max_steps}` for `advance()`; their drawing
settings now live in `config.debug`. For example, replace
`bodies.config().debug_draw = true` with `bodies.config().debug.enabled = true`,
and `bodies.config().debug` (the old line-drawing options) with
`bodies.config().debug.options`.
