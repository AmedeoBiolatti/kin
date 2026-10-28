# Rendering implementation progress

Work proceeds on `exp/rendering-foundations`, from committed lighting work `77b76c8`. The active GI subvoxel edits and main's planning changes are separate. See the [pipeline plan](rendering_pipeline_plan.md) for scope and acceptance gates.

## Stage 1: correctness and material fidelity

- `50c5e32`: conservative affine bounds across visibility, spatial groups, retained caches and LOD. Native image comparisons cover shear, mirrors, shadows, picking, skinning, retained instances and CPU/GPU clusters.
- `5dcdfed`: seam-aware flat-soup LOD, attribute simplification and versioned bakes, retaining the uniform-attribute memory path.
- Independent linear RGB emission, explicit legacy conversion, and matching native/browser/custom-shader/GI semantics. Strength-only GI changes retain voxel attributes; zero emission changes emitter membership.
- Independent per-map texture samplers and image sharing by colour/data role, including material-file inheritance/clearing, cutout shadows, picking and appearance caches.
- One camera-depth composition order for native transparent mesh copies and sprites. Only consecutive compatible packed records batch together. Additive light behind alpha foregrounds is attenuated correctly. Explicit particle `sort=false` retains its approximate whole-source ordering.
- Conservative cluster storage reservations: draws unable to reserve a complete finest cut render their full level-zero geometry in all dependent passes, with nonblocking CPU fallback counters. This may be substantially more expensive than an actual coarse cut. It is a safety policy, not a performance improvement; precise per-object GPU overflow fallback remains a candidate to measure.
- Flat reconstructed normals preserve a usable UV normal-map frame.

### Validation

Focused native fidelity tests pass on RTX 4080 Laptop Vulkan and software Vulkan. They cover gamma/linear composition, cross-material interleaving, smoke on both sides of glass, additive light behind alpha, RGB emission independent of blue base/vertex/instance colours, HDR strength, GI colour and intensity reuse, zero-factor membership, sampler binding changes on one shared image, 108 imported min/mag/wrap combinations, and tiny/exact-fit cluster budgets. Material asset sharing, variants and reload tests pass. CPU bounds and mesh-cluster suites pass, including the new flat-seam regression.

The browser build uses CI's Emscripten 6.0.13. The WebGL2 3D smoke test passes in Chromium/SwiftShader, including materials, skinning, sky, fog, shadows, resizing and input. It is functional coverage, not a browser performance measurement.

The complete native renderer dispatch now passes in one process after graph integration, following earlier partitioned runs, including the large cut, temporal, GI, animation, particle, cache and compressed-import cases. The final material fixes also pass the focused hardware/software fidelity suites and unlit specialization comparison. Large cut-parity fixtures now explicitly reserve enough room to exercise their cut paths; separate pressure tests verify complete fallback geometry, cascades, two views and occlusion without debug readback. A short existing GPU/CPU timing assertion was context-sensitive: the same case passed alone (~0.30 ms GPU versus ~0.38 ms CPU) and failed after the wider suite (~0.49 ms versus ~0.38 ms). Functional coverage now checks bounded per-copy upload work as well as image/count parity; timings remain reported. Performance conclusions belong in isolated repeated benchmarks under the plan's protocol.

Local native configuration: Release, GCC 15.2, experimental 3D, scene runtime, Jolt, scripting and image I/O; audio, TTF and 2D physics disabled. A build-only `-Wno-error=template-body` on scripting targets handles an existing sol2/GCC 15 diagnostic; repository and dependency sources were not altered for it.

## Stage 2 baseline

Before incremental updates, a CPU-only flat workload with 10,000 and 100,000 entities measured four transform flushes plus collection per frame, with static and 1-percent-edit cases. Five runs used 50 warm-up and 150 measured frames each. At 100,000 entities, p50 was about 10 ms for both cases. Builds were active on the machine, so this is a diagnostic baseline, not a controlled end-to-end speed claim. Retain the original binary for alternating before/after measurements and add hierarchy, tracked-write and raw-write correctness gates.

## Stage 2: tracked CPU work and memory accounting

`WorldRenderState3D` supports explicit `SceneMutationMode3D::Tracked`. Use `set<T>()`, `modified<T>()` after mutable writes, or `invalidate()` after external bulk changes. Structural observers include disabled entities and prefab sources. Stable topology/source caches propagate dirty roots and descendants, and static mesh/model extraction reuses retained batches. Sparse placement edits preserve independently collected snapshots. Native physics and root motion publish actual transform changes. `work_stats()` reports visits, updates, fallback collections, validation reuse and estimated metadata capacity.

The default `Conservative` mode preserves raw mutable pointers using the original tight transform loops. An experimental comparison cache regressed that path (~13 ms versus ~10 ms at 100,000 entities), so it was rejected. Animation, lights, particles, attachments and presentation callbacks still use full extraction; structural/material changes rebuild the static cache. This is an initial incremental path, not a claim that all ECS scans are removed.

Particle configuration validation is cached only in tracked mode, including failures and disabled emitter capacity. CPU regression tests cover static zero visits, sparse edits, descendants, raw writes, derived-transform tampering, reparenting, prefab source edits, observer destruction and particle validation invalidation.

`RendererBackendStats::gpu_memory` accounts for every Kin-owned SDL_GPU texture, buffer and transfer handle. It calculates format-block payload, mip chains, layers/depth and samples, and reports idle pooled textures separately, plus peaks and allocation/release/failure counts. These are descriptor payloads; SDL cycle backings, pending GPU retirement, alignment and driver allocations remain opaque. Buffers now share the device lifetime guard used by textures. Hardware and software Vulkan tests pass descriptor calculations, allocation/release accounting, 100 pool reuses, 100 target resizes and destruction after the renderer.

`kin_scene_work_bench` emits CPU timings with actual scan/update counters. Final before/after results are recorded below once validated.

## Stage 3: compiled graph and GPU ASCII

The backend-independent `ResourceGraphCompiler` validates single writers, initialization, feedback, dependencies, cycles and required features before execution. It provides stable scheduling, unused-pass culling, resource lifetimes, compatible allocation slots, export/history pins and exact descriptor caching. Reference options disable culling and reuse. `GraphResources` allocates public colour targets, storage textures and buffers transactionally and validates imports. Reuse concerns SDL objects; it does not imply arbitrary native heap aliasing.

`kin_ascii3d --test` passes on hardware and software Vulkan: optimized/reference pictures match exactly, four logical targets use three compatible physical slots, repeated frames reuse plans and allocate no new resource handles. The live tool is an example using public APIs and a GPU glyph shader with no CPU readback in its rendering path. Native custom shader support is required; WebGL2 capability rejection is explicit.

The existing 2D `RenderGraph` also uses compiled scheduling and diagnoses cycles/missing dependencies before callbacks. CPU graph and existing rendering tests pass.

## Stage 4: stock operations and quality calibration

The native stock renderer now schedules 13 coarse operations through the same public compiler, with cached plans per view and CPU compilation/recording counters. Their backend-owned resources are represented by dependency tokens; internal pre-pass, main-pass, GI/history allocation and synchronization remain within those operations. Fine-grained resource migration and stock transient aliasing remain later work. `debug.graph_reference` executes the original fixed order for comparison. A two-view HDR/TAA/AO/contact-shadow/sun-shadow/picking regression matches the fixed order exactly. The complete native renderer suite passes.

The reference tool accepts explicit filtering, mip-bias and specular-AA candidates and records effective settings, graph counters and memory payload. `quality_matrix.py` records source, binary and content identities, alternates candidate order, preserves logs and optionally compares with Cycles. Candidate version 1 is not an engine preset. Ground truth now supports independent seeds and defaults to no denoising, adaptive sampling or sample clamping. Moving-camera calibration and the active lighting work's final GI snapshot are still required before selecting new defaults.

Imported `KHR_materials_unlit` now ignores PBR emission fallback data. Native unlit emission retains legacy glow and supports independent RGB; both generic and specialized paths are checked. In-place emissive texture updates invalidate GI attributes without changing the texture handle.

The static incremental path also covers model parts, including multiple parts in a shared material bucket and shared prefab renderers. Bindings retain part indices rather than duplicate part matrices. Asset/model/material/particle-texture resolvers publish changed runtime bindings, so native reload helpers invalidate extraction and validation caches. Regression coverage includes these resolver changes.

ASCII checks use explicit runtime validation in Release builds, including optimized/reference equality, actual target handle reuse and HDR working-format transitions. The example uses stack operation/import tables and cached binding storage to avoid steady preparation allocations.

The graph and memory examples pass with Release checks enabled on hardware/software Vulkan. WebGL2 has also been rebuilt and its smoke fixture passes after graph integration. Quality results and calibration limits are recorded in [rendering measurements](rendering_measurements.md).

## Stage 5: standalone geometry in public graphs

`Renderer3D::render_geometry` records opaque/cutout world geometry into caller-owned sampled D32F depth and signed octahedral RG16F world normals. The new `RenderAttachment` graph resource kind allocates explicit float/depth textures with exact payload accounting, compatible lifetime slots and colour-space-independent reuse. Extreme RGBA16F sizes are rejected before payload arithmetic can wrap. The compiler remains usable without experimental 3D.

The native stock prepass now shares its geometry recorder with this operation and uses a cached typed geometry subgraph for its pinned view-owned outputs. The outer thirteen-phase schedule and stock TAA normal/motion packing remain intact. The operation has an exact unjittered camera, independent clearing and validation for formats, extents, attachment usage, device ownership and attachment feedback. It retains ordinary culling, skinning, retained batches and CPU/GPU LOD cuts while leaving stock view history untouched.

[kin_outline3d](../tools/outline3d/main.cpp) renders a live depth/normal outline pipeline entirely through public APIs, with a shader owned by the example. Its `--test` path checks nonempty surfaces/edges, exact optimized/reference pixels, stable handles, cached plans, stable GPU handle allocation and zero device-idle waits. [The geometry contract](render_graph.md#geometry-operations-and-data-attachments) documents normal decoding, reverse-Z depth, ownership and supported coverage.

This is a first reusable geometry operation. The standalone path still prepares/culls its submission on each call. It excludes blended/wireframe/overlay/particle geometry and normal-map/material-fragment effects; it has no occlusion history or MSAA. Shared prepared submissions, replaceable shadow/main operations and full stock transient allocation migration remain subsequent work. WebGL2/SDL_Renderer reject this operation explicitly.

Validation: the complete native renderer dispatch passes; ten focused hardware suites pass, with the separate live-foreign-device lifecycle probe skipped as documented; all six selected software Vulkan suites pass, including that ownership probe. The WebGL2 smoke fixture passes with explicit unsupported-geometry checks. The generic graph compiler builds and passes without experimental 3D, and the 190-file 3D formatting check passes. Four saved stock captures match decoded RGBA bytes exactly. The [measurement notes](rendering_measurements.md#separate-device-lifecycle-finding) record the unresolved two-device window-release crash and its 2D-only reproduction.

## Stage 6: shared preparation for geometry and stock consumers

`Renderer3D::prepare` now builds frame-scoped world data for multiple consumers. `render_geometry_prepared` and `render_prepared` share transformed bounds, affine scales, packed instances and skin bounds. Visibility, LOD, camera/overlay ordering, shadow membership and temporal history remain per consumer. Exact row maps and GPU write generations prevent stale upload reuse after camera changes or intervening ordinary rendering. Current palettes and compatible local-shadow unions also reuse data. Identical camera/local unions can share their buffer, with a recycled spare for writers.

Weak source identity guards and frame/source/batch/pose revisions reject expired inputs before drawing. A pose revision preserves the existing command-layout revision contract. `modified_instances()` explicitly invalidates preparation after edits through old mutable spans. Handles borrow scene/batch data and can safely be observed/released after source or renderer destruction. CPU payload admission defaults to 32 MiB; preparation, scratch, upload and reuse counters make its cost visible. The ordinary streaming API stays available. WebGL2/fallback preparation is explicitly unsupported.

The [API contract and example](render_graph.md#shared-draw-preparation) describe lifetime, ownership, raw-span notifications and the CPU/memory tradeoff. Public shadow/main extraction remains subsequent work.

Validation: the complete native renderer dispatch passes. Eleven focused hardware checks pass, with the inherited two-live-device lifecycle probe skipped; seven software Vulkan checks pass, including cross-device rejection and renderer-lifetime observation. The new cases verify 10,000-copy upload reuse, no repeated bound transformations, off-camera sun casters, local unions, stable GPU allocations across frames, LOD/cluster/composition parity, eight frames of independent skinned/TAA history, and edit/move/frame invalidation. Four stock captures remain byte-identical to `35518ab`. WebGL2 smoke passes with explicit unsupported-preparation checks. The 2D-only graph compiler and 190-file formatting check also pass. See the [measurement notes](rendering_measurements.md#shared-draw-preparation) for costs, scope and artifact paths.


## Stage 7: public sun-shadow and mesh-surface composition

Native `render_sun_shadow()` writes caller-owned D32F arrays and returns matching cascade metadata. `render_surface()` consumes an optional explicit map and writes working-colour plus stored reverse-Z depth from the same prepared scene. They share stock mesh/material, skinning, retained-instance and LOD recording. No private map swapping, stock temporal history advance or implicit local-shadow/effect pipeline is involved. Array shape is part of graph validation, import compatibility, allocation reuse and payload budgeting.

The new [surface graph example](../tools/surface3d/main.cpp) prepares once, records sun shadows → mesh shading → GPU ASCII → presentation, and checks reference pixels, stable allocation/plan reuse and HDR transitions. The implementation is deliberately the directional-shadow/material-surface slice: local/fill shadow inputs, public sky/fog/particles/screen-space/temporal operations and complete stock composition remain follow-up work. Custom map/projection bindings are supported; arbitrary shadow-lighting algorithms still need material interfaces. Finite-speed lighting also needs a separate bounded history/solver.

Acceptance covers one/four cascades, one-layer array types, unused/inactive layer clears, fractional/disabled shadow strength, externally changed projections, stored depth, transparent/overlay/local-light parity, flat/masked/skinned/retained meshes, CPU/GPU cluster cuts, feedback/format/extent/ownership/lifetime rejection, import shape and overflow, queued resource retention, and stock cache/TAA interleaving. A regression seeded stock particle-caster scratch before a mesh-only public shadow pass, failed before its explicit exclusion guard and passes with the guard. Native/software focused suites, full native stock dispatch, browser smoke, saved stock captures, 2D-only compilation and formatting are recorded in the [measurement notes](rendering_measurements.md#public-shadow-and-surface-operations).

## Stage 8: explicit local-shadow inputs and material light responses

`render_local_shadows()` renders explicit spot/point requests into caller-owned D32F layers; `render_surface()` consumes their projection/filter metadata through `SurfaceOptions3D::local_shadows`. Requests bypass stock slot allocation and private caches, render complete point cubes, clear unused layers and preserve the prepared-scene/frame ownership contract. Per-light settings remain editable; invalid or overlapping layouts, malformed metadata and incompatible inputs fail before recording. Stock cache revisions, arrays and view histories retain their ownership.

Custom material shaders can define `KIN_DIRECT_LIGHT` and return each direct light's response from a `Surface` and `KinLight`. Irradiance and visibility are separate; kind, direction and local position/distance are available. Ambient/IBL, emission and output remain separate. Existing stock/custom shader paths stay intact without the macro. This is a fragment-lighting hook, with the existing fixed custom light blocks; extra material graph-resource bindings, other shadow representations, geometry deformation and a complete propagation/history solver remain follow-ups.

The [surface graph example](../tools/surface3d/main.cpp) now composes sun + explicit spot/point shadows → [toon material](../tools/surface3d/toon.frag.glsl) → GPU ASCII. Tests cover exact stock pixels, arbitrary layer placement, mixed viewport sizes, all cube faces, off-camera casters, skinning/masking/retained/cluster paths, light-grid bindings, private cache hits and eight frames of independent TAA/AO history. Hook tests cover stock response parity, visibility bypass, kind, world position/distance and wrapped directional response. Historical/queued lifetimes and the full twenty-layer profile have independent checks.

The final failure-reporting review caught a no-op edge introduced while extracting the recorder: camera-pruned stock cube faces can all require no draw before any caster buffer exists. The regression fails before the guard and succeeds after it; no work is a successful operation. Thirteen focused hardware tests and nine software tests pass, with the inherited hardware live-device lifecycle probe skipped. Full native stock dispatch, browser smoke, four unchanged stock captures, 2D-only graph compilation and formatting are recorded in the [measurement notes](rendering_measurements.md#local-shadow-graphs-and-material-light-responses).


## Stage 9: graph textures and buffers in custom materials

Native `create_material_inputs()` creates immutable, shader-specific shared binding sets for four extra sampled textures and eight read-only buffers. Reflected resource shapes, dense descriptor bindings, minimum buffer sizes and device identity are validated before use. Stock rendering and independent surfaces consume the same sets; stock material slots and custom light blocks retain their ABI. Raw/comparison depth arrays, unsigned inputs and GPU-produced data are supported within the bounded profile. Other fragment outputs, descriptor arrays and geometry deformation remain separate work.

CPU uploads after recorded material reads submit the earlier work before updating, without an explicit idle wait; frame-scoped preparation expires at that boundary. Compute writes stay in-frame and preserve shared world preparation. Content revisions cover buffers and rendered inputs, conservatively invalidating stock TAA when extra inputs change. Shared shader/device guards and target-stack ownership make late handle release safe.

The [surface graph](../tools/surface3d/main.cpp) now composes compute-produced cloud lighting/palette data with explicit sun/local maps, a toon material and ASCII output. It caches material bindings until physical resources change. [The graph contract](render_graph.md#graph-resources-in-materials) and [shader ABI](rendering_3d.md#extra-material-textures-and-buffers) describe dependencies, allocation lifetime, temporal reset policy and unsupported features. This supplies the consumption path for alternative shadows or lighting history; it does not implement a propagation solver or complete stock public composition. The finite-speed lighting follow-up is an example-only prototype: its solver, history and simulation policy stay outside the engine and stock renderer.

Validation: the final full native 3D dispatch passes, as do fifteen focused hardware tests (two inherited live-device lifecycle skips), all twelve software Vulkan tests, the browser smoke fixture, standalone 2D-only graph compilation and the 193-file formatting gate. Four stock captures match predecessor `c96504f` byte for byte. The unmodified full 2D suite retains a pre-existing DPI mapping failure reproduced in the October 6 binary; a temporary diagnostic copy omitting only that test passes all remaining cases, including every engine shader's reflection. Details and artifact paths are in [material-input measurements](rendering_measurements.md#material-graph-inputs).

## Standalone slow-light prototype

`kin_slowlight3d` demonstrates finite-speed direct illumination using the existing public graph/material APIs. All propagation/history logic and shaders are in `tools/slowlight3d`; engine sources, public APIs and stock defaults are unchanged. A fixed receiver plane, moving pulsed source and moving spherical occluder show instantaneous versus delayed lighting, including both source-to-surface and surface-to-observer travel time. A four-second, 121-sample history has explicit startup/exhaustion behavior and bounded reconstruction after time jumps. Interactive speed, pause/reset and delay display controls plus deterministic captures make the approximation inspectable.

See [the model and run instructions](render_graph.md#standalone-slow-light-prototype). It is an example-level direct-light model, not a general delayed-geometry renderer or relativity simulator.


Validation: analytic CPU checks and GPU/CPU transport probes pass on hardware and software Vulkan, together with the existing surface/material-input regressions. Optimized/reference images match exactly, bounded history and warmed GPU resource reuse pass, the 90-frame live loop exits, CPU sanitizers pass and all 196 formatted 3D files pass. Illumination, delay and exhausted-history captures have been inspected; artifact paths and scope are recorded in [prototype measurements](rendering_measurements.md#standalone-slow-light-prototype).


## Stage 10: public bloom and view output

Native `render_bloom()` and `render_output()` share the stock five-draw bloom recorder and view finishing recorder. Graphs own the linear colour, bloom/scratch and working-format output targets; optional explicit `SurfaceTargets3D::linear_colour` supports HDR shading independently of global 2D colour space. Existing surface/view calls retain their behavior. Both public operations preserve premultiplied/additive coverage, SDR ACES/exposure, HDR linear passthrough and stable gamma dithering, with explicit validation, resource retention and content revisions. Stock views now report these fullscreen draws separately and propagate recording failures.

The surface graph example records HDR toon → bloom/output → ASCII, with cached plans, stable allocations and exact-format attachments retained through working-space changes. Public sky/fog/particles, screen-space effects and temporal resolve remain subsequent work. See [the API contract](render_graph.md#bloom-and-view-output).


Validation: complete native 3D dispatch, eighteen focused hardware checks (three inherited live-device lifecycle skips), all sixteen software Vulkan checks, browser smoke, four unchanged stock captures, 2D-only graph compilation and the 197-file formatting gate pass. New coverage includes exact stock/public output across working spaces, independent alpha/exposure/HDR/dither checks, transparent bloom coverage, queued CPU read/write ordering, resource ownership/lifetime, TAA interleaving/revisions and warmed allocation reuse. Artifact paths and the temporary-workspace recovery are recorded in [the measurements](rendering_measurements.md#public-bloom-and-view-output).


## Stage 11: opaque/transparent surfaces and particles

Native public surfaces now expose an opaque stage and a transparent stage with optional world sprites/weather sharing stock mesh/sprite composition. The existing combined mesh-only API remains the default. Transparent consumers load initialized colour/depth without writing depth; optional distinct `colour_input` supplies a versioned graph path with one explicit full colour copy. Soft intersections consume a separate unjittered world-depth input. Public sprite lighting consumes supplied sun/local maps and never borrows private maps or advances stock particle history/caster state. Public shadow producers remain mesh-only.

The surface graph now records opaque HDR toon → glass/sprites → bloom/output → ASCII, using distinct colour versions under the existing compiler rules. This is not yet copy-free graph attachment versioning. Prepared world bounds and compatible uploads remain shared; per-consumer visibility, LOD and composition work remain. The [API contract](render_graph.md#opaque-and-transparent-surface-stages) describes depth, ownership, live particle inputs and feature limits.

Regression checks cover exact stock/combined/in-place/versioned pixels across working spaces, glass/sprite blend modes, overlays/weather, masked skinning, retained transparency, wireframes, orthographic cameras, CPU/GPU cluster paths, explicit sun/spot/point sprite reception, soft fades, resource lifetime, CPU upload ordering, stock TAA isolation and warmed resource reuse. Independent linear HDR clears are corrected after a coloured-background comparison exposed gamma-encoded clear values.

A stronger history check also exposed a stock TAA bug: globally ordered colour runs can split a particle source around glass, while coverage replays that source once. Comparing the two draw counts falsely rejected coverage and reset history. The check now validates against source batches; the regression requires real accumulation in both stock views across interleaved public particle calls.

Validation: complete native stock dispatch, nineteen focused hardware checks (four inherited live-device lifecycle skips), all eighteen software Vulkan checks, browser smoke, four unchanged stock captures, standalone 2D-only graph compilation and the 198-file formatting gate pass. See [the measurements and build limitations](rendering_measurements.md#opaquetransparent-surfaces-and-particles).


## Stage 12: compiled attachment versions

Graph attachment declarations now express clear/discard/load, store/discard and read-only depth testing separately from sampled reads. The compiler hands compatible transient storage to a loaded output version at the predecessor's last use. Later readers, sampling during the same pass and imported/exported/persistent contents receive separate storage with a planned pre-callback copy. Per-load reasons, copy counts/bytes and slot payload are inspectable. Existing single-writer graphs remain compatible; shader feedback is still rejected.

The bound executor records exact native texture copies, including depth arrays, with resource retention, CPU upload ordering and content revisions. Cached resource preparation protects a re-imported previous output from managed-slot recycling. Explicit ping-pong histories remain the stable-allocation path. Safe handoffs also work for browser 2D targets; unsupported copy plans fail capability admission. This is resource-object reuse, not native heap aliasing or automatic compilation of callback internals.

The [surface graph](../tools/surface3d/main.cpp) now declares opaque colour/depth, loaded composed colour and read-only depth. Its normal path eliminates the colour copy and one full-size RGBA16F texture, saving 2.1875 MiB payload at 640 × 448. It still compares exact pixels with the separate-copy reference and checks cached plans, warmed handles and HDR transitions. The earlier stage's manual `colour_input` path remains available. See [the contract](render_graph.md#attachment-loads-and-logical-versions).

Validation: full native stock 3D dispatch, twenty focused hardware checks (five inherited live-device lifecycle skips), all twenty-one software Vulkan checks, browser 2D handoff/stock 3D smoke, 300 compiler reference graphs, compiler sanitizers, standalone 2D-only compilation, four unchanged stock captures and the 199-file formatting gate pass. See [the measurements](rendering_measurements.md#compiled-attachment-versions) for scope and artifact paths.

## Scene-independent rendering evaluation

`tools/eval/eval.py` adds manifest-driven benchmark, capture, Cycles truth, scoring,
stress and offline report stages. Sponza, deterministic Industrial Yard and the
external Bistro game are registered adapters; a JSON command protocol supports
other applications without engine-specific scene conversion. See the
[evaluator guide](../tools/eval/README.md) and [adapter protocol](../tools/eval/protocol.md).

The runtime exposes opt-in raw CPU/frame, descriptor-memory, copy and fence-estimate
records through `KIN_EVAL_FRAME_LOG`. Throughput processes avoid capture and
synchronized diagnostics. Repeats alternate build order; reports distinguish CPU
percentiles from unassigned GPU fence observations, owned payload from device-wide
residency observations, and baseline image regression from reviewed Cycles accuracy.
Frozen provenance, complete artifact inventories, dependency-aware resume and a
shared truth cache prevent stale/missing frames from silently passing.

Scoring reproduces the previous Bistro display transforms, FLIP, image/video JOD,
bias/detail and temporal definitions, adds Kin SSIM, and reports two-seed Cycles
noise. Yard remains baseline regression only until a reviewed procedural exporter
exists. Lifecycle stress currently covers persistent reference-view resize/TAA/AO;
asset/shader reload, scene load/unload and multiple views are explicitly unexercised.
The stress baseline is established after feature prewarming and upload/pool expiry,
so startup slack cannot serve as a hidden allowance for later retained growth.
