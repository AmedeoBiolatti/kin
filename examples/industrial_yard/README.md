# Industrial yard

A rendering workload shared by Kin and Godot Forward+. A camera travels through
an industrial yard, enters a warehouse, exits through its door and finishes with
an elevated view. It targets costs that the instanced Grove scene did not expose:
independent objects, material changes, widely distributed edits, skeleton poses,
transparent sorting/overdraw and overlapping local shadows.

| Tier | Props | PBR variants | Moving props | Skinned robots | Smoke cards | Points / shadowed spots |
|---|---:|---:|---:|---:|---:|---:|
| Small | 2,000 | 16 | 40 | 32 | 128 | 8 / 2 |
| Medium | 8,000 | 64 | 800 | 128 | 512 | 16 / 4 |
| Heavy | 20,000 | 128 | 5,000 | 256 | 2,000 | 16 / 8 |

Each tier also has 215 structure objects, including six glass windows. Props are
independent draws/nodes unless moving; moving props share one instance batch per
material, spread across the yard. Every material group changes each frame. Both
engines may perform their normal batching/culling. Do not replace the independent
static objects with a single premerged mesh: that would change this workload.

The generator writes a small, deterministic `YRD1` recipe containing exact mesh,
texture, environment, camera and animation bytes. Ten shared meshes contain 4,378
unique triangles. Medium has 4,222,448 triangles across all instances, **not** that
many unique mesh triangles or necessarily that many visible triangles. The recipe
is about 1.35 MiB; textures are reused, and no external assets are downloaded.
Camera motion and animation use frame indices so slow and fast engines traverse
identical states. One loop is 1,440 measured frames, after 300 stationary warm-up
frames. Both consumers swap the required winding convention; Godot uses the baked
tangents, while Kin derives the normal-map basis through its usual shader path.

## Build and inspect

Use a Release build with `KIN_EXPERIMENTAL_3D=ON`. Keep builds serial on the
constrained development machine. Use a Linux disk directory for artifacts, not
RAM-backed `/tmp` or the Windows source tree.

```sh
cmake --build build --target industrial_yard -j1
python3 examples/industrial_yard/generate.py /home/bamedeo/kin-bench/industrial-yard/medium.yrd
python3 examples/industrial_yard/test_recipe.py \
  --binary build/bin/industrial_yard --root /home/bamedeo/kin-bench/industrial-yard/tmp

KIN_RENDER_BACKEND=gpu build/bin/industrial_yard \
  --recipe /home/bamedeo/kin-bench/industrial-yard/medium.yrd \
  --metrics /home/bamedeo/kin-bench/industrial-yard/preview.csv --preview
```

Preview follows the same camera route at a 60 FPS cap; Escape exits. For measured
runs, omit `--preview`, specify `--measured 1440` and use `--report=PATH` and
`--screenshot=PATH` if wanted. `--resolution-scale 1|2` selects 1280×720 or 2560×1440
internally; the presentation window stays 1280×720. `--route-offset 540` starts
inside the warehouse. `--validate-recipe` validates the binary without creating a
renderer, window, jobs or GPU resources; it does not require `--metrics`.

Native profiling controls (default rendering is unchanged):

- `--gpu-pass-markers` labels GPU passes for external profilers without readback.

- `--visible` shows an uncapped window and exits after warm-up + measured frames.
  External GPU timestamp capture needs presentation; hidden runs do not provide
  Nsight with the same GPU workload boundaries. This differs from the 60 FPS preview.
- `--warmup 0..300` shortens warm-up for size-bounded GPU captures. Keep the default
  300 for normal timings; very short captures are pass-attribution probes.
- `--rebuild-scene` resubmits the full native command list when actors animate, as
  the earlier sample did. The default updates existing joint ranges. Compare
  with the default to isolate command submission and layout reuse.
- `--point-lights N` limits the recipe's points (0..recipe count, maximum 16).
  Spot lights and their maps remain active. Default: all recipe points.
- `--receive-shadows 0` disables material shadow reception while retaining casters
  and shadow-map generation. Default: 1.

The last two controls change the image and diagnose cost; they are not optimizations
at behavior parity. `--profile-json=PATH` records existing runtime present/backend
and fence-based GPU-frame counters. The latter include CPU recording gaps and are
not equivalent to GPU timestamp queries. On hidden runs `gpu.wait` measures only
swapchain acquisition; the unshown-frame fence throttle is included in
`present.backend`. Preserve its bounded frames-in-flight policy.

## Serial comparison

Use the already installed Godot Linux editor and matching Linux release export
template. The template must be named `linux_release.x86_64` in that directory.
The runner copies the tiny project to scratch, exports a pack, and hardlinks the
template when possible. It never installs Godot or overwrites older benchmarks.

```sh
python3 examples/industrial_yard/run_compare.py \
  --root /home/bamedeo/kin-bench/industrial-yard \
  --godot /home/bamedeo/kin-bench/godot-4.7.2 \
  --build --suite preflight --frames 60 --repeats 1

python3 examples/industrial_yard/run_compare.py \
  --root /home/bamedeo/kin-bench/industrial-yard \
  --godot /home/bamedeo/kin-bench/godot-4.7.2 \
  --suite full --frames 1440 --repeats 3
```

Only one application/build runs at once. The existing guard requires 2 GiB free
before starting, stops below 1 GiB available, and limits process-tree RSS to 512
MiB Kin / 1,024 MiB Godot / 2,048 MiB build. Each command has a 300-second timeout.
RSS includes loading but excludes dedicated VRAM. These are safety ceilings, not
promised maximum memory requirements on every platform. Output includes commands,
logs, process peaks, per-frame CSV, run reports and checked timing summaries.

`--suite baseline` runs 720p/1440p; `--suite ablations` runs 720p controls removing
actors, freezing skeleton poses, stopping rigid motion, disabling shadows and
removing transparency separately, plus AO+TAA enabled.
`full` includes both. `--tier small|medium|heavy` chooses the recipe. Repeated
pairs alternate engine order; they do not lock clocks or stop other desktop jobs.
The project defaults to Compatibility only for inexpensive editor/export startup;
the comparison explicitly selects Vulkan Forward+ and fails on another renderer.

## Behavior and measurement limits

- Both engines receive the same independent objects, material parameters, source
  maps, linear light colors, radius/cones, frame camera and float32 transforms.
  The runner checks matching populations/features and moving position sums at
  every measured frame. It verifies native uploaded joint counts, route progress,
  actual shadow work and nonempty geometry, rather than trusting requested flags.
  Godot also reads back the uploaded MultiMesh transforms twice. These and the
  skin checks can add an outlier to frame 302; validation remains in the CSV.
- Robots use actual six-joint GPU skinning, not separate rigid limb draws. Kin
  consumes the baked palette; Godot uses `Skeleton3D`, `Skin` and inverse binds.
  Godot validates baked skeleton vertices against the native palette at startup
  **and after animation advances**, with a 1e-4 tolerance. Pose3D notification updates
  complete before the manually forced render. See [Skin](https://docs.godotengine.org/en/4.7/classes/class_skin.html)
  and [Skeleton3D](https://docs.godotengine.org/en/4.7/classes/class_skeleton3d.html).
  These are simple 200-triangle robots: six independent root joints and one full
  joint weight per vertex. They exercise the skinning/submission path, without
  testing complex blended rigs, animation graphs or IK. Godot sets the baked poses
  from GDScript; Kin does so from C++. Script/native call overhead is consequently
  part of the application comparison, not evidence of a pure renderer difference.
- Kin's `Scene3D::draw_skinned` initially copies poses into the scene. The default
  now keeps commands submitted and edits those joint ranges with
  `update_joint_matrices`, so animation no longer rebuilds static object commands.
  Skinned draws can reuse prepared layout metadata; posed bounds, shadow selection,
  current/previous joints and uploads still refresh. The sample keeps one 32-bit
  joint offset per actor (512 bytes for medium), without another pose mirror.
  `--rebuild-scene` restores animated command resubmission for comparison.
  `--animation 0` keeps robot geometry and frozen poses; `--motion 0` retains the
  prop batch layout and stops its buffer updates. Godot retains its ordinary scene
  nodes. Earlier comparison numbers predate the native pose-update path.
- Godot initializes and edits MultiMeshes through the same bulk-buffer path,
  preserving the sample's CPU buffer. Initializing with per-instance setters first
  creates an internal CPU cache and changes dirty-resource/AABB notification order;
  that produced alternating cached/redrawn shadows in the first no-actor probe.
  Those provisional timings were archived separately and excluded. See the
  [Godot mesh storage implementation](https://github.com/godotengine/godot/blob/4.7-stable/servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp).
- Spot shadows use nominal 90° perspective cones, range 60, near .025, 1024² D32
  maps. Godot fixes the near plane internally. Its 2048 atlas holds four 1024 slots;
  Heavy uses 4096 with sixteen 1024 slots for eight requested spots. Kin retains
  its two-texel guard band (about 90.223° actual projection). No sun or point-light
  shadows; moving casters keep local maps active. Culling, batching, shadow cache
  policies, bias and filter kernels are each engine's normal implementation.
- The feature comparison is **not pixel parity**. BRDF details, normal-map bases,
  environment convolution, glass highlights, emission/color handling and shadow
  filtering differ visibly. AO/TAA are separate quality experiments with different
  implementations, disabled in the baseline. One sample, HDR/ACES, no occlusion
  culling. Point/spot selection can change with the camera in Kin.
- Full-frame interval includes sample code, scene traversal/submission, GPU pacing
  and presentation. Native submission/thread/process CPU and Godot scene-update,
  viewport CPU and GPU timestamp columns measure different scopes; do not compare
  those columns as if they were identical engine render times. Godot GPU timestamps
  may lag the current frame. Kin has no matching GPU timestamp in this harness.
- This is a procedural rendering stress test: no physics, AI, asset streaming,
  gameplay, high-resolution texture residency or sophisticated character rigs.
  Increasing instances does not test millions of unique mesh triangles. Results
  cannot establish a general engine ranking.

CPU-only tests check mesh winding/tangents, all tier budgets/determinism, spatially
distributed edits, nonidentity skin transforms and native rejection of nine
malformed inputs (including oversized counts, NaN, invalid indices/weights,
nonaffine matrices and truncation). GPU preflights also need visual inspection of
both outside and warehouse screenshots.

Measured results and practical conclusions are recorded in `docs/3d_plan.md`.
