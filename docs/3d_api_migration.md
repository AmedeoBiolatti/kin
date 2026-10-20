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

Resource loads on `Renderer3D`, `Assets3D` and `PhysicsAssets3D` no longer take an error-output string. Check the returned handle, then copy `diagnostics3d_text(owner.diagnostics())` when needed. Successful calls, including cache hits, clear earlier diagnostics; observation does not. `ModelFile3D::error()` has also been removed in favour of `diagnostics()`. Constructor/precondition errors and stateless CPU validators retain their documented contracts.

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

Update C++ callers using the following names. The experimental API has no legacy aliases; authored scene/prefab component names and schemas stay unchanged.

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
| `Materials3DLoadResult` | `MaterialsLoadResult3D` |
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
