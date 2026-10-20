# Kin glTF/export profile 1

This is the tested portable model subset for Kin's experimental 3D asset loaders.
Its profile ID is `kin-gltf/1`.
It describes usable assets, not everything that a glTF parser can inspect. Models
outside a listed limit must be re-exported or split; the engine rejects unsupported
skin/deformation data instead of accepting a partly drawn or partly static model.
The file format remains glTF 2.0; this profile is a Kin contract, not a new extension.

## Export contract

| Area | Supported content | Export requirement |
|---|---|---|
| Container | `.glb`, or `.gltf` with embedded/external buffers and PNG/JPEG images | Prefer GLB for a self-contained delivery. Keep external dependencies under the content root; use relative URIs. |
| Geometry | Indexed/unindexed TRIANGLES; static node transforms; float POSITION/NORMAL/UV attributes; vertex colours | Convert curves to meshes and triangulate. Export normals (missing normals become flat faces). No Draco/meshopt compressed geometry or sparse accessors. |
| Materials | Metallic/roughness PBR, base colour, normal, occlusion and emissive maps; opaque/mask/blend; double-sided; unlit and emissive-strength extensions | Bake procedural shader inputs to images. Use the glTF channel packing and colour spaces. Other material extensions are outside this portable profile. |
| UVs | One selected coordinate set and placement per material, including a shared `KHR_texture_transform` | Every map in that material must use the same set/offset/rotation/scale, and the primitive must provide that set. Bake differing placements into common UVs or split the material/geometry. |
| Skin | One distinct palette, at most 256 joints including intermediate nodes; multiple meshes may share it | All skins in the file must have identical joint order and inverse bind matrices. Remove unused distinct skins or split armatures into separate model files. |
| Influences | `JOINTS_0` u8/u16; `WEIGHTS_0` float/normalized u8/u16; four influences per vertex | Limit to four, normalize and remove invalid/zero-total weights. Extra influence sets and invalid joint references reject the asset. |
| Animation | Translation/rotation/scale; LINEAR, STEP, CUBICSPLINE; skin clips or plain-node clips | Bake constraints/IK to transform keys. Do not mix a skinned character with separately animated rigid nodes needing another pose in one model. Attach rigid props under a joint instead. |
| Deformation | Linear blend skinning and rigid-node transforms | Bake/remove shape keys and morph targets. Oversized or unsupported skins do not fall back to static geometry. |
| Scene content | Mesh parts from the default scene, or first scene when no default is selected | Author Kin cameras/lights/environment in `.kinscene`; model loading does not create them from glTF. |

Quantized attributes and native compressed textures have additional implementation
support, but are not the portable export baseline above. Required extensions outside
the loader's allowlist reject the file. Optional extensions can use their glTF core
fallback; this is not support for the extension's additional appearance or behavior.
For predictable results, omit extensions outside the profile.

The coordinate/unit and packing conventions come from the
[Khronos glTF specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html).
Use metres and +Y up in the exported file. Normal maps follow glTF's tangent-space
convention; metallic/roughness images use the blue/green channels respectively.

## Blender export recipe

Use **File → Export → glTF 2.0**, selecting GLB. The
[Blender glTF manual](https://docs.blender.org/manual/en/5.0/addons/import_export/scene_gltf2.html)
describes the exporter; option labels vary with Blender versions. This recipe targets
the file contract above. The automated tests exercise exported glTF data, not a
particular Blender binary or every exporter version.

1. Select only the intended meshes and, for a character, its one armature. Remove
   unused armatures/skins. Apply intended mesh modifiers before binding/export,
   and check the bind pose after any change to object transforms.
2. Use Principled BSDF inputs that map to core glTF metallic/roughness, or an unlit
   material. Bake procedural textures. Use PNG/JPEG and a common UV placement for
   every map in each material; mark data maps as non-colour data in Blender.
3. Export normals, UVs, materials and skinning. Limit bone influences to **4** and
   disable exporting all influences. Keep the exported joint count, including
   intermediate nodes, within 256. Multiple character mesh objects are fine only
   when they export the same palette.
4. Export the intended actions/clips with baked transform sampling where constraints
   or IK drive them. Turn off mesh compression. Remove shape-key/morph deformation.
   Export a separate model for independently animated rigid scenery.
5. Validate the file with the
   [Khronos glTF Validator](https://github.com/KhronosGroup/glTF-Validator), then
   load it in Kin. A successful generic glTF validation does not imply that it
   fits Kin's palette, UV or deformation limits.

Preview a real export with the same asset loader used by a game:

```sh
cmake --preset 3d
cmake --build build --target kin_asset_preview
KIN_RENDER_BACKEND=gpu build/bin/kin_asset_preview --root path/to/content --model models/hero.glb \
  --clip Walk --time=0.5 --headless --frames=2 --screenshot=hero.png --report=hero.json
```

Omit `--clip` for a static model. On initial rejection the preview exits with an
error; on a failed live reload it retains the last good model and reports
`reload_error`. Check the bind pose and each clip's beginning/middle/end visually.

## Acceptance tests

`kin-model-profile3d` runs without a GPU; `kin-model-profile3d-gpu` exercises the
same structural acceptance rules with the native renderer. They cover GLB/static/PBR
fixtures, external PNG and embedded JPEG images, u8/u16 joint indices, float/u8/u16 weights,
equivalent multi-mesh skins, rejection of distinct palettes/oversized skins/bad
joint indices/morphs/mixed poses, shared versus incompatible UV placements, and
repair after rejection. The GPU case also submits both parts of an equivalent-skin
character and verifies that a rejected direct load has no drawable fragments and
that a failed polling reload retains its last good model.

`kin-skeleton` checks pose/channel values, interpolation, joint ordering and baked
texture coordinates; `kin-model-assets3d` and `kin-render3d` cover textures,
materials and image rendering. All run against local generated/checked-in fixtures;
there are no downloads during tests. The lavapipe CI job includes both profile cases.

```sh
cmake --build build --target kin_model_profile3d_tests kin_skeleton_tests kin_navmesh3d_tests
ctest --test-dir build -R '^kin-(model-profile3d(-gpu)?|skeleton|navmesh3d)$' --output-on-failure
```

`read_model_data` is deliberately useful for inspection of unsupported content.
It can return true and retain multiple skeletons or repaired data. Check its
`asset_diagnostics` before treating that data as a supported asset. `Assets3D`
checks these diagnostics on CPU and GPU; failed assets are never cached.
CPU-only models do not decode images or upload resources. Geometry/skin/UV checks
apply there; texture decoding and device-format/upload checks require a GPU load.

## Navigation geometry

For character navigation, build with the character's radius and upright height
in the same world units as its collision controller. Pass static walls/ceilings
as well as floor triangles. Winding identifies walkable floors; obstruction tests
are two-sided. Configure `up` and slope limits to agree with the character.

Height tests the whole floor footprint against source triangles. Radius erodes
the resulting boundaries, including regions blocked by headroom. Uphill rise
across the radius adds a conservative headroom margin based on the steepest
actual slope in the original region. Both dimensions zero request point-agent
navigation; changing either dimension requires rebuilding.

Clearance removes whole triangles, without clipping, merging or subdivision.
Use a tessellated walk surface: the regression fixtures use **0.1 m cells**. Coarse
triangles can remove excess area or the whole mesh; check `valid()` and preview
the resulting surface, rather than assuming a wide polygon will be inset. Vertices,
nearest points, raycasts, random samples and paths all use the filtered mesh.
The support/obstruction tolerance is 1e-5 world units. Static navigation remains
separate from collision-based movement: use the character controller to follow
the surface, handle slopes/steps and stop on dynamic obstacles.

`kin-navmesh3d` checks small/large agents at a doorway, short/tall agents under a
ceiling, partial sloping ceilings, embedded walls, slopes with headroom, stacked
layers, alternate up axes and repeatable builds. See
[navigation queries and limits](hex_grids.md#navigation-meshes).
