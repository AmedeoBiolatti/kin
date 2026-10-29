# Compiled resource graphs

`kin/renderer/resource_graph.hpp` is a backend-independent compiler. `kin/renderer/graph_resources.hpp` binds its resources through public renderer APIs. Both are usable without experimental 3D. The stock native 3D renderer uses the same compiler for its coarse phase schedule; ordinary games still call `Renderer3D::render()`.

Graph compilation is separate from shader compilation, GPU pipeline creation and temporal/asset caches.

A definition contains named logical resources and passes. A resource has one writer; reads depend on that writer regardless of declaration order. Imported values are initialized and read-only. Write another logical version for a result, and import last frame's history separately from this frame's output. Reading and writing the same logical resource in a pass is rejected: feedback needs ping-pong resources.

```cpp
ResourceGraphDesc graph;
graph.resources = {
    {.name = "source", .kind = GraphResourceKind::ColorTarget,
     .size = {640, 360}, .imported = true},
    {.name = "result", .kind = GraphResourceKind::ColorTarget,
     .size = {640, 360}, .exported = true},
};
graph.passes = {{.name = "filter", .reads = {0}, .writes = {1},
                 .required_features = graph_feature::raster | graph_feature::shaders}};

ResourceGraphCompiler compiler; // keep across frames
GraphResources resources;      // keep with this renderer
const auto& plan = compiler.compile(graph, available_graph_features(renderer));
const std::array imports{GraphImport{.resource = 0, .texture = source}};
if (!plan || !resources.prepare(renderer, graph, plan, imports))
    return; // inspect plan.diagnostics or resources.diagnostics()

const std::array<std::function<bool()>, 1> operations{{[&] {
    auto target = renderer.scoped_render_target(resources.target(1));
    renderer.clear(colors::black);
    renderer.draw_shader_surface({0, 0, 640, 360}, shader, params, resources.texture(0));
    return true;
}}};
if (resources.execute(plan, operations))
    renderer.draw_texture(resources.texture(1), {0, 0, 640, 360});
```

Declare every access accurately. Callbacks use the existing public raster/compute/3D operations and supply their own parameters and bindings. Compilation validates the declarations; it cannot inspect arbitrary C++ callback code or insert synchronization inside an opaque operation. A callback failure stops subsequent operations and does not roll back commands already recorded.

The compiler diagnoses duplicate names, invalid IDs, multiple writers, missing initialization, feedback, missing/disabled dependencies, cycles and unavailable required features before callbacks. `after` adds explicit dependencies, including between side effects. Exports and side-effect passes are roots for unused-pass culling. Unsupported work that is unused may be culled. `GraphCompileOptions{.cull_unused=false, .reuse_slots=false}` provides the reference schedule/resource plan.

The optional `max_slot_payload_bytes` compile limit rejects a graph before allocation if its managed slots exceed the requested payload budget. Imports, backend-owned tokens and hidden driver storage are outside this limit; callers choose an explicit fallback.

The plan contains a stable topological order, first/last use intervals, compatible physical slots, planned payload and culling counts. Exports pin their final contents; persistent resources retain dedicated slots. Transient slots can share SDL resource objects after the previous value's last use, or through a validated attachment-version handoff below. This is object reuse, not overlapping native heap allocations. SDL may allocate hidden cycle backings; use the device ledger and external telemetry when assessing memory.

`GraphResources::prepare()` validates the exact definition/plan and imported dimensions/formats, then stages allocations before publishing new bindings. Failure keeps the previous bindings. Keep resources on one renderer and use handles created by that renderer. Imports retain their supplied handles. Exports are current-frame textures/buffers, not immutable snapshots across later executions. Release a pipeline with `clear()` when its retained capacity is no longer useful.

Public colour targets use the renderer's working format. Set `GraphResourceDesc::hdr` to match `renderer.hdr()`; HDR target payload is RGBA16F. A working-colour-space change recreates managed targets. Storage textures use the declared `TextureFormat`; buffers use `bytes`. Tokens describe dependencies between backend-owned operations and have no graph-managed allocation. Explicit float/depth render attachments are described below. MSAA resources and full stock allocation migration remain future extensions; tokens have no allocatable texture storage.

Stable plans update non-overlapping imported bindings without allocating new CPU/GPU storage. Re-importing a managed output as history detaches any matching managed allocation to preserve that input; use explicit ping-pong history allocations/resource sets for stable temporal storage. Exact descriptor/feature/compile-option comparison caches the plan; camera, time, callback parameters and imported contents do not invalidate it. Resize or topology/feature changes do. Use a compiler/resource set per retained pipeline variant to avoid replacing one cached variant with another every frame. `compiler.stats()` reports compilation count, hits and CPU compilation time.

Build and run `kin_ascii3d` for the public-API 3D → ASCII → output example. Its GPU shader samples one scene colour per cell and selects compact glyph strokes. `kin_ascii3d --test` compares optimized/reference pixels, verifies target reuse, tests HDR declarations and checks stable allocations. It currently requires native custom shader support; capability checks reject unsupported backends before execution. The rendering path performs no CPU readback.

The legacy 2D `RenderGraph` caches its dependency order through this compiler. Missing dependencies and cycles now throw an actionable error before any callback, instead of silently ignoring a dependency or recursing indefinitely. Toggle behaviour and pass statistics remain available.

The native 3D migration currently exposes thirteen backend operations as dependency tokens. Their existing internal depth, shadow, GI, temporal and output work remains atomic and owns its resources. `Scene3D::debug.graph_reference` runs the original fixed order for parity testing. `RenderDebugStats3D` reports pass counts, cache reuse and CPU compilation/recording time. Stock transient allocation aliasing, complete stock composition from public operations and a general finite-propagation simulation remain separate work. Standalone geometry, sun-shadow and mesh-surface operations are available below.

## Attachment loads and logical versions

`GraphPassDesc::attachments` distinguishes attachment accesses from sampled shader reads. Each `GraphAttachment` declares a target `resource`, `load` (`Clear`, `Discard`, or `Load`), `store` (`Store` or `Discard`) and `access` (`Write` or `ReadOnlyDepth`). Writable attachments implicitly declare a writer; do not repeat them in `writes`. `Load` requires a distinct, compatible `previous` resource and implicitly depends on its producer. A read-only depth attachment names the existing depth in `resource`, requires `Load`/`Store`, has no `previous`, and declares a reader without a new writer.

```cpp
// Resources: opaque_colour and composed_colour are compatible RGBA16F attachments;
// depth is D32F. Sampled sun/local/soft-depth inputs, if used, remain in reads.
graph.passes = {
    {.name = "opaque", .attachments = {{.resource = opaque_colour}, {.resource = depth}}},
    {.name = "transparent", .attachments = {
        {.resource = composed_colour, .previous = opaque_colour, .load = GraphAttachmentLoad::Load},
        {.resource = depth, .load = GraphAttachmentLoad::Load,
         .access = GraphAttachmentAccess::ReadOnlyDepth}}},
};
// Opaque callback clears/stores its colour/depth using SurfaceStage3D::Opaque.
// Transparent callback uses composed_colour and depth with SurfaceStage3D::Transparent.
// Leave SurfaceOptions3D::colour_input empty: the bound executor initializes colour.
if (!resources.prepare(renderer, graph, plan)) return false;
return resources.execute(plan, operations);
```

Declarations cover the entire single-sample image, including all declared array layers. Callbacks still record the actual raster passes, clear values, blending and depth-write state. They must honour load/store declarations and initialize stored results. The compiler does not inspect C++ callbacks, merge arbitrary raster passes, or turn metadata into a different shader/render pipeline. Clear colours and ordinary callback parameters remain outside structural compilation.

The compiler hands a predecessor's physical texture to the new version when the descriptors match, the predecessor's last use is the loading pass, its slot is still available, and that pass does not sample the predecessor. Imported, exported or persistent predecessor contents must survive; persistent destinations remain dedicated too. An exported *new* version may inherit a transient predecessor's texture and pins the final contents. Other readers later in the retained schedule force separate storage. The compiler does not reschedule readers merely to enable reuse. Culled readers do not extend a retained value's lifetime.

When a handoff is unsafe, the plan allocates compatible separate storage and inserts an exact copy before the loading callback. Two outputs loading one predecessor can inherit at most one copy of its slot; all other initializations happen before the callback changes any attachments. Explicitly sampling the predecessor via `reads` is legal with this copy path. Sampling an attached output or a read-only depth target is rejected: attachment access does not permit shader feedback. Use separate sampled depth for soft particles under the current SDL GPU interface. Reading or exporting discarded attachment contents, malformed operations, duplicate bindings and incompatible versions are rejected before recording.

`GraphResources::execute(plan, operations)` records all planned copies and callbacks for its exact prepared plan and working colour space. An invalid/incomplete callback table performs no copies or callbacks. Failure during execution stops later work without rolling back earlier commands. The backend-independent `execute_resource_graph()` accepts an explicit copy recorder; without one it refuses plans containing copies before invoking callbacks. Existing graphs without attachment declarations retain their behavior.

Copy admission uses `graph_feature::texture_copies` / `RendererBackendCapabilities::texture_copies`. Native SDL_GPU supplies exact GPU copies through `Renderer2D::copy_texture(source, destination)`: same extent/native format/array shape/device, distinct textures, base mip across all layers, no blending or colour conversion. Compressed formats and mipmapped destinations are rejected. Handles and pending reads/writes remain valid until queued recording is submitted; later CPU uploads submit preceding copy accesses first. GPU copy writes publish content revisions. WebGL2/SDL_Renderer currently lack this copy capability; safe handoffs can still run there, while a plan requiring a copy fails compilation explicitly. Browser smoke checks a loaded 2D colour handoff and this refusal.

`CompiledResourceGraph::attachment_loads` reports each load's `in_place` decision and reason. `attachment_reuses`, `attachment_copies` and `attachment_copy_bytes` report handoffs and fallback work; `slot_payload_bytes` estimates graph-owned allocated image/buffer payload. `peak_live_payload_bytes` counts **logical versions**, so it can count two views of the same physical texture at the transition; it is not physical residency. Backend `texture_copies` / `texture_copy_bytes` count actual explicit whole-image copy requests and texel payload, not measured bandwidth. The older `surface_colour_copies` counter applies only to manually requested `SurfaceOptions3D::colour_input` copies.

Texture handles refer to physical storage, not immutable logical snapshots. Only access a transient version during its compiled lifetime; declare a later reader or export the old version when it must survive. Extra material bindings must follow the actual logical reads and be rebuilt if physical allocations change. Re-imported prior outputs are protected from managed-slot recycling, including with a cached plan, but this detach path can allocate a replacement each frame. Bounded ping-pong histories remain application-owned policy. This implementation reuses resource objects; it does not alias native GPU heaps or measure SDL/driver cycle backings. `reuse_slots = false` produces the separate-storage/copy reference path.

The [surface example](../tools/surface3d/main.cpp) now uses a colour handoff and read-only depth for opaque → glass/sprites. Its normal path records zero colour copies and saves one 640 × 448 RGBA16F allocation (2.1875 MiB of texture payload) relative to its separate-version reference. Reference/optimized images, working-space transitions, cached compilation and warmed resource handles are checked automatically.

## Geometry operations and data attachments

`GraphResourceKind::RenderAttachment` describes a single-sample, sampled render attachment with an explicit data format. `Rg16Float` is four bytes per pixel, `Rgba16Float` eight, and `Depth32Float` four. `Depth24Unorm` describes the native stock fallback depth format; direct allocation depends on the device. These formats preserve the numeric values and ignore working colour space/HDR. They cannot be pushed as 2D colour targets. Use `resources.texture(id)` to supply them to a geometry operation or shader. `renderer.create_render_attachment(size, format)` also allocates them directly. Check `graph_feature::attachments` / `capabilities().render_attachments` for RG16F, RGBA16F and D32F support; a D24 allocation can still fail on a device without that optional format.

With experimental 3D, `Renderer3D::render_geometry(targets, scene)` is an independent operation. Check `Renderer3D::capabilities().geometry_pass`; native SDL_GPU supports it where the required shaders and sampled formats exist. WebGL2 and SDL_Renderer currently return an owned diagnostic before recording. A complete public example is [kin_outline3d](../tools/outline3d/main.cpp), with its own [outline shader](../tools/outline3d/outline.frag.glsl). It creates no stock `View3D`:

```cpp
graph.resources = {
    {.name = "depth", .kind = GraphResourceKind::RenderAttachment,
     .format = TextureFormat::Depth32Float, .size = size},
    {.name = "normals", .kind = GraphResourceKind::RenderAttachment,
     .format = TextureFormat::Rg16Float, .size = size},
    {.name = "output", .kind = GraphResourceKind::ColorTarget,
     .size = size, .exported = true},
};
graph.passes = {
    {.name = "geometry", .writes = {0, 1},
     .required_features = graph_feature::rendering3d | graph_feature::attachments},
    {.name = "outline", .reads = {0, 1}, .writes = {2},
     .required_features = graph_feature::shaders},
};
// After compiling and preparing resources, the geometry callback records:
return bool(r3d.render_geometry({resources.texture(0), resources.texture(1)}, scene));
```

The operation clears both attachments on each execution. Depth uses reverse Z: zero is background/far, one the near plane. Its projection is exactly `scene.camera.view_projection(width / height)`, without temporal jitter. Unproject with that matrix, converting top-left pixel coordinates to clip coordinates; do not interpret reverse-Z values as linear distance. Normals are world-space geometric/interpolated unit normals in signed octahedral RG, not RGB or UV coordinates. Decode them as follows, and use depth to reject background:

```glsl
vec3 decode_normal(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    n.xy += vec2(n.x >= 0.0 ? -1.0 : 1.0, n.y >= 0.0 ? -1.0 : 1.0) * max(-n.z, 0.0);
    return normalize(n);
}
```

Opaque and masked world meshes share the stock prepass recorder, including UV/vertex/instance alpha coverage and sampler overrides, skinning, retained batches, frustum culling, whole-mesh LOD and CPU/GPU cluster cuts. Flat meshes reconstruct normals from position derivatives. Blended meshes, wireframes, overlays, particles, material normal maps and material fragment effects are excluded. The operation performs no lighting, environment filtering, shadow rendering, GI, AO, TAA or postprocessing, and does not advance a stock view's jitter/history. Occlusion history is disabled. The scene overload prepares per call; `render_geometry_prepared()` shares the frame-scoped data described below.

Targets must match in extent and belong to the renderer. They must have been allocated as render attachments: a sampled-only RG16F data texture is insufficient. Validation precedes command recording. A later GPU recording failure can leave partial output, consistent with graph execution's existing no-rollback contract. Retained `Texture` handles keep their allocations alive after graph resize/clear; rendering into the same handles replaces their contents.

The native stock prepass uses the shared geometry recorder within a cached typed geometry subgraph. Its depth and normals are pinned, view-owned allocations; the outer thirteen-phase graph still uses tokens for coarse dependencies. TAA's stock variant has its existing packed normal/motion/flag RGBA16F encoding and jitter, which is separate from the standalone RG16F contract. This migration does not expose stock history buffers or add stock transient reuse. `debug_stats().geometry_draws`, `geometry_instances` and `geometry_graph_reused` report the shared geometry work/cache; the existing stock `ao_*` counters remain compatible.

## Shared draw preparation

On native SDL_GPU, `Renderer3D::prepare(scene)` builds the source's packed instance records, conservative world bounds, affine LOD scales and posed skin bounds once. Both standalone geometry and the complete stock renderer can consume the resulting `PreparedScene3D`:

```cpp
const auto result = r3d.prepare(scene, {.max_cpu_bytes = 32ull * 1024 * 1024});
if (!result)
    return; // result.diagnostics explains refusal
const PreparedScene3D prepared = *result;

// These can be separate graph callbacks. Keep the handle/source alive through
// recording, and finish all consumers before readback or presentation.
if (!r3d.render_geometry_prepared({resources.texture(depth), resources.texture(normals)}, prepared))
    return;
if (!r3d.render_prepared(view, prepared))
    return;
renderer.draw_texture(view.texture(), output_rect);
renderer.present();
```

Check `Renderer3D::capabilities().shared_preparation` before requiring this operation. The existing scene-taking `render` and `render_geometry` APIs retain their streaming paths. WebGL2 and fallback backends return unsupported diagnostics for explicit preparation. Graph compilation and callbacks remain independent: refreshing a frame's prepared handle does not invalidate a structural execution plan.

Preparation shares world data; every consumer still evaluates its own camera/shadow visibility and LOD cut. Main/geometry uploads are reused only when the exact source-row ordering and current GPU write generation agree. This includes transparent sorting, overlay placement and off-camera sun casters. Different layouts stream the required records from the prepared CPU data. Current joint palettes can be shared, while previous poses, velocity and TAA history remain view-specific. Local shadow unions reuse their own layouts and can read the camera buffer directly when its rows match. Writers detach using one recycled spare buffer, so sharing does not allocate a new GPU handle each frame. This reuses ordinary SDL buffers, without native heap aliasing.

The handle borrows the source scene and expires at a GPU frame boundary: presentation, readback, submission or another operation that ends the frame. Source destruction/movement, command changes, batch edits, pose edits and overlay changes invalidate it before recording. `valid()` remains safe after scene or renderer destruction. Camera, lighting and quality settings can change between consumers; they do not change the prepared world data. View resizing gets fresh per-view culling. Recording and source mutation must remain ordered on the renderer thread.

Fill `add_instances()` spans before preparing. If an already-returned mutable span is edited afterward, call `scene.modified_instances()` before using a prepared handle again. Raw memory writes cannot be detected without scanning. Ordinary scene-taking rendering continues to read current values. Use `update_joint_matrices()` and `InstanceBatch3D::update()` for poses and batches; their revisions invalidate preparation automatically. Source diagnostics and incompatible renderer inputs also prevent consumption.

The default admission limit is 32 MiB of preparation payload per handle. Packed records/bounds/scales cost 148 bytes per submitted copy, plus metadata and compact row maps. Admission reserves room for maps and checks the GPU record-count limit; it precedes large allocations. `preparation_cpu_bytes` reports the handle's vector payload and `preparation_scratch_bytes` reports renderer mapping capacity. Allocator overhead and existing scene/assets are excluded. One spare instance buffer can remain resident and is included in the device GPU ledger. Release stale handles when finished; their CPU data remains owned until release even though they have expired.

Preparation has an up-front CPU/memory cost and includes copies that may later be culled. It is useful when several consumers share a submission. A retained-only scene may already benefit from incremental caches and need no extra preparation. Measure total time including `prepare()`, separately from consumer recording. `preparation_records`/`preparation_skin_bounds` count build work; `shared_records`, `shared_uploads`, `shared_instance_bytes`, `shared_joint_uploads` and `shared_joint_bytes` describe reuse. `instance_upload_bytes`, `joint_upload_bytes` and `bounds_instance_tests` expose the work still performed. `shared_records` counts source records bound for culling/drawing, including subsequently culled copies; byte counters measure actual skipped uploads. Reading these counters does not trigger GPU readback.

## Multiple cameras and visibility masks

A scene can feed independent camera graphs in the same GPU frame. Prepare its world data once, change
`scene.camera` between consumers, and restore it before recording the main view. Culling, projection and LOD are
per operation; changing a camera does not recompile a structural graph or edit the prepared geometry. Separate
stock views own their GI/TAA history. Sequential sensor graphs can reuse a small depth/mask pair after reducing
each camera's result; simultaneously retained images need separate resources.

Native `Renderer3D::render_visibility(targets, prepared, options)` exposes a lightweight target-ID pass. Check
`capabilities().visibility_pass`. It writes a single-sample **D32F depth + linear RGBA16F mask**, of matching size
on the renderer's device. Mask RGB is one for the nearest target fragment and zero for occluders/background.
It uses the prepared source's current, exact unjittered camera; the caller's camera range/FOV apply normally.

```cpp
// target_id is MeshInstance3D::id (also the id argument to Scene3D::draw).
const VisibilityOptions3D query{.target_id = player_id, .ignored_id = enemy_body_id};
scene.camera = enemy_camera;
const auto observed = r3d.render_visibility({depth, mask}, prepared, query);
scene.camera = main_camera;
if (!observed) return;
if (!r3d.render_prepared(main_view, prepared)) return;
// Submit/read results only after every consumer of this frame's preparation.
```

Assign a distinct target ID; all of a model's parts may share it. Unassigned world objects can keep ID zero.
`UINT32_MAX` is reserved, and an ignored ID must differ from the target. Ignored fragments write neither mask nor
depth, allowing a sensor to exclude its own body. Opaque and alpha-masked world meshes preserve skinning, UV/alpha
cutouts, imported samplers, material-sidedness, retained copies and LOD/cluster selection. Blended/additive meshes,
wireframes, overlays and particles do not occlude this pass. Material fragment effects and custom displacement
are not evaluated. Use suitable proxy geometry when gameplay visibility needs a different policy.

There is no lighting, GI, AO, shadows, TAA or postprocessing, no main-view history advance and no readback inside
the operation. Invalid/expired/foreign inputs and recording failures return diagnostics rather than an occluded
observation. `debug_stats().visibility_draws` and `visibility_instances` describe its geometry work. Unsupported
backends, including WebGL2, report unavailable; stock rendering keeps its existing behaviour.

The optional [vision example](../tools/vision3d) combines a camera graph (mask → GPU pixel count → tiny byte atlas)
with the normal main-view graph:

```sh
<native-build>/bin/kin_vision3d
<native-build>/bin/kin_vision3d --resolution 128 --budget 2
<native-build>/bin/kin_vision3d --frames 120 --capture vision.png
<native-build>/bin/kin_vision3d --test
```

SPACE toggles cover; ESC closes the example. The top image is the normal camera, the bottom three images are
front/flank/rear enemy masks, with unknown/occluded/visible indicators and pixel-count bars. The optional scheduler
in `tools/vision3d/vision.hpp` belongs to the example, not the engine's AI/runtime systems. It admits at most
`views_per_batch` observers (default four, rotating fairly) and `max_pending` asynchronous batches (default three).
Its maximum mask extent is 256 × 256. No room in the pending queue means no new observation, not invisibility.

`Graph::record(scene, prepared, requests, frame)` requires the **same source scene passed to prepare** and records
only; `submit()` runs once after main/observer consumers. It requests one small readback, four alpha bytes per
observer, without downloading the mask. Using alpha preserves exact integer counts through gamma/linear/HDR
colour targets. `Renderer2D::request_read_rgba(rect, false)` explicitly refuses a synchronous fallback, returning
zero on unsupported/read-setup failure. The original one-argument API retains its existing fallback behaviour.
`poll()` never waits and returns the original token/generation, source frame, camera, target ID, count and validity.
Failed reads are unknown. Completion can take multiple frames; callers discard stale identities and choose their
own age limits, thresholds and hysteresis. Destruction drains at most the configured pending reads and can wait;
the renderers must outlive the scheduler. Call `prepare()` before scene preparation after a working-space change.

Pixel counts measure rasterized geometric visibility at the chosen resolution/LOD, not lighting-based perception
or an exact visible-area fraction. Small targets/thin gaps can be missed. Filter candidate observers with distance
and FOV first, bound the update rate, and compare the total submission cost against physics raycasts. Gameplay
decisions and confidence policies stay outside the renderer.

## Explicit sun shadows and mesh surfaces

Native SDL_GPU exposes `render_sun_shadow()` and `render_surface()` as independent graph callbacks. Check `RendererCapabilities3D::sun_shadow_pass` / `surface_pass`. WebGL2 and SDL_Renderer reject these operations with owned diagnostics; they retain their existing stock renderer.

`GraphResourceDesc::array_layers` defaults to zero (a 2D texture). A positive count declares an explicit 2D array, including a one-layer array. Arrays require `GraphResourceKind::RenderAttachment` and `graph_feature::attachment_arrays` / `Renderer2D::capabilities().render_attachment_arrays`. Direct allocation uses `create_render_attachment(size, format, layers)`. Payload budgets multiply by the layer count; slot compatibility and imports distinguish 2D textures from one-layer arrays. Custom shaders must declare the corresponding sampler type. Ordinary 2D uploads and the twelve reserved 3D material texture slots do not accept arrays; extra material inputs below accept sampled 2D arrays.

A complete runnable example is [kin_surface3d](../tools/surface3d/main.cpp): graph-owned sun/local depth and compute lighting data → opaque HDR mesh shading → ordered glass/sprites → stock bloom/view output → GPU ASCII → presentation, with no stock `View3D`. Prepare once after updating the scene and before executing the graph:

```cpp
// Resources: sun = D32F array, colour = working-colour target, depth = D32F 2D.
// Declare sun's writer, surface's read of sun and writes of colour/depth,
// and postprocessing's reads/writes in the graph.
auto prepared = r3d.prepare(scene).value_or({});
SunShadow3D shadow;
// sun callback:
auto result = r3d.render_sun_shadow(resources.texture(sun), prepared, receiver_size);
if (!result) return false;
shadow = *result;
// surface callback:
return bool(r3d.render_surface(
    {resources.target(colour), resources.texture(depth)}, prepared,
    {.background = colors::black, .sun_shadow = &shadow}));
```

`render_sun_shadow()` clears and writes a square, sampled D32F array with 1–4 layers. It fits the scene's sun against `shadow_area`; `receiver_size` supplies the camera aspect for cascades. The requested cascade count is limited by the supplied array capacity. The target's extent sets shadow resolution. Opaque/masked world casters, off-camera casters, skinning, retained instances and CPU/GPU LOD cuts use stock recording. Overlays and particle casters are excluded. Unused layers, including every layer of an inactive sun, clear to reverse-Z zero. This operation deliberately redraws rather than consulting the stock view's private shadow cache.

The returned `SunShadow3D` holds the texture plus `ShadowCascades3D` projection/bias-scale metadata. It is a binding to mutable texture contents. Keep projection and depth versions together; use distinct persistent or ping-pong resources for history. A developer may supply a custom projection/map by constructing the same value. Finite matrices, positive texel/depth scales, valid cascade ranges, sampler format and ownership are checked before recording. Stock shading uses the comparison/PCF material interface; custom shaders can implement other algorithms through the material inputs below.

By default, `render_surface()` clears single-sample colour/depth and shades scene meshes with the exact unjittered camera projection. It includes stock materials/custom material shaders, opaque and masked draws, ordered transparency, wireframes, overlays, explicit environment lighting and point/spot lights with optional supplied local shadows. Colour uses the current Renderer2D working format, or an explicit linear RGBA16F `linear_colour` attachment as described below; surface recording performs no view tonemapping. Depth is stored for later graph consumers. Overlay depth uses the stock split range; blended materials test depth without writing it.

Pass `sun_shadow = nullptr` to shade without sun shadows. No shadow map is generated implicitly. Scene switches for shadow generation, sky, fog, AO, GI, TAA and postprocessing do not add work to this operation. Particle/weather rendering requires the explicit opt-in described below. Procedural sky lighting is also omitted; an explicitly supplied environment may be filtered on first use. Fill-shadow inputs and these effects remain future public operations. Geometry/surface culling disables temporal occlusion, and neither operation advances a stock view's history or modifies its private shadow maps.

Both operations consume the prepared handle's frame and source-lifetime contract. Targets/maps must belong to the renderer; surface attachment feedback through any material map is rejected before recording. Allocation or recording failure follows the graph's no-rollback contract. Keep all declared graph reads live through their consumers; projection metadata is ordinary callback data, while the graph tracks the matching depth texture. A finite-propagation light simulation needs explicit scene/light history and a bounded solver in addition to these rendering operations.


## Opaque and transparent surface stages

Check native `RendererCapabilities3D::surface_stages`, `surface_particles` and `surface_soft_particles` for the requested features. `SurfaceOptions3D::stage` selects `All` (the default combined mesh-only operation), `Opaque` (opaque/masked meshes), or `Transparent` (blended meshes and wireframes). `All` and `Opaque` clear colour and depth. `Transparent` loads initialized colour and depth, blends over them, and preserves depth. Supply the same camera, attachment size/format and overlay layout as the opaque producer. Validation checks texture compatibility; it cannot infer which camera or scene produced existing pixels.

```cpp
// Manual, in-place composition: both calls use the same prepared scene and attachments.
auto prepared = r3d.prepare(scene).value_or({});
if (!r3d.render_surface({.depth = depth, .linear_colour = colour}, prepared,
                       {.background = colors::black, .stage = SurfaceStage3D::Opaque}))
    return false;
return bool(r3d.render_surface({.depth = depth, .linear_colour = colour}, prepared,
    {.stage = SurfaceStage3D::Transparent, .particles = true}));
```

`particles = true` enables world `Scene3D::draw_particles()` sources and procedural weather in `All` or `Transparent`; it is rejected with `Opaque`. Sprites share the existing per-copy mesh/sprite camera-depth order, blending, atlas/facing, culling and budget policy. `ParticleLook3D::sort = false` keeps a source as one run at its mean depth. This retains stock approximate transparency; intersecting geometry does not become order independent. Overlays retain the stock camera/depth-range rules. Particles/weather are live inputs at each surface call, independent of prepared mesh revisions. Submit/update them before the transparent consumer; the mesh source, pose, retained-batch and frame lifetime rules still apply.

For a compiled resource graph, use the [attachment version contract](#attachment-loads-and-logical-versions): opaque writes colour/depth attachments; transparent loads a new colour version from opaque colour and tests existing depth through `ReadOnlyDepth`. Declare supplied shadow maps and sampled soft depth as `reads`, then feed the composed version to bloom/output. `GraphResources::execute()` selects an in-place handoff or a preservation copy automatically; leave `colour_input` empty. For manually initialized pipelines, optional `colour_input` still copies a distinct, exact-format same-size source before transparent blending and reports `surface_colour_copies`. Both paths preserve single-writer and shader-feedback rules without a hidden view or prepass.

Soft intersections need a separate sampled, two-dimensional D32F `soft_depth`, matching size and the unjittered world camera, without overlay depth splitting. For example, declare a geometry callback writing `GeometryTargets3D::depth` and normals, then pass that depth to the transparent consumer. It cannot be the active surface depth attachment: the current SDL GPU interface cannot sample that texture while it is attached for depth testing. If an enabled source requests soft intersections without compatible depth, the call fails before recording. There is no silent soft-particle fallback or implicit prepass; omit this extra producer for hard particles. A soft depth input without `particles = true` is rejected.

Lit sprites use the scene's lights and the explicitly supplied sun/local maps. Null bindings remain unshadowed even after stock rendering has populated private maps. Public sun/local producers still rasterize mesh casters only; sprite `casts_shadow` does not add public caster work. Fill shadows, public sky/fog, AO/GI/TAA, MSAA and temporal history remain outside this operation. Sprite texture handles are retained through queued recording; later CPU uploads submit preceding reads before replacement and expire frame-scoped prepared handles. GPU producers remain ordered within the frame.

Prepared bounds and compatible instance/joint uploads are shared across these consumers. Visibility, LOD selection, particle selection and composition sorting remain per operation. The [surface graph example](../tools/surface3d/main.cpp) demonstrates the explicit versioned colour path with glass, sprites, sun/local maps, HDR toon, bloom/output and ASCII. The [stage tests](../tests/render3d_surface_stages_tests.cpp) compare combined, in-place and versioned pictures against stock rendering and check depth/source preservation, ownership, soft intersections, history isolation and warmed GPU resource reuse.


## Explicit local shadows

`Renderer3D::render_local_shadows(depth_array, prepared, requests, receiver_size)` renders explicitly requested point/spot mesh shadows on native SDL_GPU. Check `capabilities().local_shadow_pass`. The target is a square sampled D32F array with at most `max_local_shadow_layers` (20) layers. At most eight spots and two points may be requested. Each request specifies a light's index in the prepared source scene, a first layer, and a viewport resolution (zero uses the full array side). A spot occupies one layer; a point occupies six consecutive layers in +x, −x, +y, −y, +z, −z order. Occupied ranges must not overlap, and each light can appear only once.

```cpp
// Submit local lights after scene.clear(), which clears those lists too.
scene.lights.spots.push_back(spot);
scene.lights.points.push_back(point);
const std::array requests{
    LocalShadowRequest3D{.kind = LocalShadowKind3D::Spot, .light = 0, .first_layer = 0},
    LocalShadowRequest3D{.kind = LocalShadowKind3D::Point, .light = 0, .first_layer = 1},
};
auto prepared = r3d.prepare(scene).value_or({});
// Local-shadow graph callback (declares a write of the seven-layer array):
auto result = r3d.render_local_shadows(resources.texture(local), prepared, requests, size);
if (!result) return false;
LocalShadows3D shadows = std::move(*result);
// Surface callback declares a read of that array, alongside any sun depth:
return bool(r3d.render_surface({resources.target(colour), resources.texture(depth)}, prepared,
    {.sun_shadow = &sun_shadow, .local_shadows = &shadows}));
```

Requests select their lights independently of `shadow_enabled`, stock importance/hysteresis, shadow-slot budgets and private caches. The supplied target and request resolutions determine storage/viewport size; per-light bias, normal offset, softness and distance settings determine fitting/filter metadata. All six requested point faces are rendered, including faces outside the current camera. This makes the output complete and reusable for another camera; it can cost more than the stock camera-pruned cube path. Unused layers and unused texels of smaller viewports are cleared to reverse-Z far. Empty requests clear the entire declared array.

The result stores the depth texture plus `LocalShadowMap3D` values: normalized source requests, projection matrices/near/far/scales, bias/filter settings and fade distance. `SurfaceOptions3D::local_shadows` explicitly binds them to the selected lights, including the stock light grid. Null supplies unshadowed local lighting. A developer may edit projection/filter metadata before consumption; keep the corresponding map contents and scene light indices aligned. Lighting settings can change without invalidating prepared geometry, so moved/removed/reordered lights require an intentional metadata update. Historical maps need distinct persistent targets and their matching projection/light metadata. The values are mutable bindings, not snapshots of a texture's old contents.

Casters use stock mask samplers, skinning, retained instances, conservative bounds and light-specific LOD/cluster cuts. Overlays and particles are excluded. The operation redraws its requested maps without updating private stock arrays, slot ownership, cache revisions or per-view TAA/AO history. CPU world/posed bounds come from the prepared handle; camera/caster membership and local cut selection still run per consumer. No local-shadow array is silently generated by `render_surface()`.

Before recording, the backend rejects wrong devices/formats/usages, expired preparation, invalid light indices/kinds/settings, overlapping/out-of-range layers, excessive requests, oversized viewports, invalid projection/scaling values and invalid filter ranges. Errors own their diagnostics. Unsupported backends fail explicitly. Recording failure can leave partial contents, as with other graph operations.

The [surface example](../tools/surface3d/main.cpp) includes both producers and a [toon response shader](../tools/surface3d/toon.frag.glsl). See [custom lighting responses](rendering_3d.md#custom-lighting-responses) for replacing the direct BRDF/visibility response. Materials can consume additional graph textures/buffers through the interface below. A stock fill-shadow operation remains separate work.


## Graph resources in materials

Check `RendererCapabilities3D::material_inputs` on native SDL_GPU. `create_material_inputs(shader, textures, buffers)` creates an immutable, shader-specific `MaterialInputs3D` binding set. Assign it to `Material3D::inputs`. Both stock rendering and `render_surface()` bind these resources for custom fragment shaders. Geometry, shadow, picking and GI voxelization keep their existing material/coverage contracts; custom fragment inputs do not deform geometry or alter those passes.

```cpp
// After resources.prepare(), rebuild only when a physical binding or shader changes.
const std::array textures{resources.texture(light_field)};
const std::array buffers{resources.buffer(palette)};
auto inputs = r3d.create_material_inputs(toon_shader, textures, buffers);
if (!inputs) return; // owned diagnostics describe layout/device/type errors
Material3D material{.shader = toon_shader, .inputs = *inputs};
// Submit with this material, then prepare the scene once for all consumers.
```

Declare `light_field` and `palette` in the surface pass's `reads`, and give their compute producer the corresponding `writes`. The compiler cannot discover hidden reads inside a material or callback. Declared lifetimes must cover every draw using the bindings, including overlays. Allocate persistent/imported history explicitly and ping-pong distinct logical resources for updates. A binding set retains its physical handles, but does not preserve old contents after an aliased slot is reused. Rebuild it after resize, a graph allocation change or shader replacement. Updating contents on the same handles does not require rebuilding.

The [surface example](../tools/surface3d/main.cpp) caches bindings by physical identity and adds a [compute producer](../tools/surface3d/field.comp.glsl) for an RGBA16F cloud-lighting field plus a palette buffer. Its [toon fragment shader](../tools/surface3d/toon.frag.glsl) consumes both. Shadow producers and the material-data producer can share one prepared scene's subsequent surface consumer without repeating world-bound transformations. Optimized/reference execution and warmed GPU allocation reuse are checked by `kin_surface3d --test`.

The bounded native profile allows four extra sampled textures and eight read-only storage buffers. [The shader ABI](rendering_3d.md#extra-material-textures-and-buffers) specifies bindings, types and filtering. Validation rejects wrong counts, devices, shader identity, numeric/array types, short buffers and sampling the active surface/view output. Shader reflection rejects writable buffers and unsupported layouts before pipeline creation. WebGL2 and fallback renderers return unsupported diagnostics.

GPU compute writes remain ordered in the current frame. CPU texture/buffer uploads after a recorded material read submit earlier work before uploading, without an explicit device-idle wait. This is a frame boundary: create fresh prepared handles for subsequent consumers. Prefer GPU producers or upload before preparation when sharing world data. Queued draws retain inputs until submission; a temporary 2D target on the render-target stack also remains owned across such boundaries.

Native content revisions include CPU uploads, compute outputs, 2D target writes and stock/public 3D outputs. Changing extra input contents or sampler state conservatively resets a stock view's TAA history. An identical write can still reset it; there is no shader-level proof of whether an update affects appearance. Standalone surfaces have no implicit temporal history. The finite-speed lighting prototype owns its bounded light/scene history, solver, units and occluder policy in a standalone example, outside the engine and stock renderer; these public bindings provide its material-consumption path.

### Standalone slow-light prototype

`kin_slowlight3d` is an example under [tools/slowlight3d](../tools/slowlight3d/main.cpp). Its solver, sampled history, simulation settings, shaders and diagnostics live entirely there. It uses the existing public graph/material APIs and adds no engine mode or stock-renderer behavior.

```sh
cmake --build <native-build> --target kin_slowlight3d kin_slowlight3d_tests
<native-build>/bin/kin_slowlight3d
<native-build>/bin/kin_slowlight3d --time 1.5 --speed 100 --capture slow-light.png
<native-build>/bin/kin_slowlight3d --delay --time 8 --capture delay.png
ctest --test-dir <native-build> --output-on-failure -R '^kin-slowlight3d-'
```

Build with experimental 3D, tools and glslc; native compute, sampled data attachments and material inputs are required. WebGL2/fallback support is outside the prototype. Capture requires image I/O; `--help` lists options and `--frames N` runs a bounded live-loop smoke check. SPACE pauses, UP/DOWN select 25/50/100/200/400 km/h, D toggles the delay map, R restarts with empty past history, and ESC exits. The fixed camera compares delayed lighting with an instantaneous control at the same observer time. The one-metre grid and current-position rings are diagram guides. Pink stripes mean missing history; delay mode maps zero to two seconds from blue to orange and darkens incoming occlusion.

The model is direct illumination of a static plane by one moving, pulsed point emitter and one moving opaque spherical occluder. Coordinates are metres and time is seconds. At 100 km/h, ten metres takes 0.36 seconds. For observer time `t`, receiver `P` and eye `E`, the surface event is `t_surface = t - |P-E|/c`. A bounded bisection solves `t_emit + |L(t_emit)-P|/c = t_surface` for a subluminal sampled emitter path. Each overlapping history interval tests the incoming photon's continuous worldline against the sphere's linearly moving centre. Occlusion therefore uses the sphere at the photon's time, rather than its current or emission-time position. The observer leg contributes delay but is unoccluded.

The example keeps 121 fixed-size snapshots at 30 Hz, spanning at most four seconds. Positions interpolate linearly; emitter power and sphere presence/radius hold from the left sample until the next tick. Pausing preserves samples; reset/rewind discards future history; large jumps reconstruct at most 121 samples from the deterministic example trajectory. Evaluation uses time relative to the latest tick to preserve GPU float precision over long runs. Missing emission/surface history returns unknown, rather than clamping to the oldest state or silently substituting instantaneous lighting. At 25 km/h the fixed camera can require more than four seconds even after warm-up; increasing light speed restores coverage within the same memory budget.

A compute pass writes both transport fields into one 384 x 192 RGBA16F texture. Custom materials read that field and the imported history buffer to draw two ground-plane views. Every access is declared in the graph. The history buffer is 3,872 bytes and is uploaded before material preparation; bindings are reused while physical handles stay unchanged. There is no CPU readback in the live rendering path. Explicit test/capture modes use readback.

This is a classical, restricted transport demonstration. It does not render delayed images of arbitrary moving meshes, occlude the eye leg, or model bounces, scattering, special relativity, Doppler, aberration or radiometric ground truth. Energy uses a simple cosine factor and softened inverse-square falloff in arbitrary units; a constant display fill keeps the diagram readable. The instantaneous comparison uses the same appearance model. These approximation choices belong to the example.

## Bloom and view output

Native `Renderer3D::render_bloom()` and `render_output()` expose the stock finishing operations independently of a `View3D`. Check `RendererCapabilities3D::bloom_pass` / `output_pass`; unsupported backends return owned diagnostics. Both operations use the same recording code as stock views. They do not render geometry, advance temporal history, generate implicit effects or allocate their own render targets.

`SurfaceTargets3D::linear_colour` optionally selects a sampled RGBA16F render attachment instead of the working-format `colour` target. Set exactly one of them, with matching D32F depth. This keeps scene radiance linear and premultiplied, including values above one, regardless of the 2D renderer's working format. Existing `colour` calls keep their working-format behavior. The explicit HDR target participates in the same ownership, feedback, revision, culling and prepared-scene rules.

```cpp
// HDR colour/result/scratch: GraphResourceKind::RenderAttachment, RGBA16F.
// Output: GraphResourceKind::ColorTarget, matching renderer.hdr().
// surface writes {hdr, depth}; bloom reads {hdr}, writes {glow, scratch};
// finish reads {hdr, glow}, writes {output}.
if (!r3d.render_surface({.depth = resources.texture(depth),
                         .linear_colour = resources.texture(hdr)}, prepared)) return false;
if (!r3d.render_bloom({resources.texture(glow), resources.texture(scratch)},
                      resources.texture(hdr), {.threshold = 1.f})) return false;
return bool(r3d.render_output(resources.target(output), resources.texture(hdr),
                              {.bloom = resources.texture(glow),
                               .bloom_strength = .3f, .exposure = 1.1f}));
```

Bloom takes linear premultiplied RGBA16F input plus two distinct, matching RGBA16F render attachments. HDR working render targets are also accepted. The result and scratch may have a different extent from the source; stock parity uses `max(1, source_extent / 2)` per axis. The operation extracts luminance above a finite nonnegative threshold, then records two separable nine-tap Gaussian blur rounds at increasing tap spacing. Five draws leave the final bloom in `targets.result`. Extraction precedes exposure. Both intermediates are internal reads/writes of this atomic operation; declare them as writes of its graph pass rather than graph-level feedback reads. The graph must keep both live through the operation.

Output reads linear premultiplied RGBA16F colour and optional bloom, replacing the entire supplied working-format `RenderTarget` without blending. Bloom strength and exposure must be finite and nonnegative; positive strength requires a bloom input. Sources can differ in size from the output and are sampled with the stock linear sampler. Gamma SDR output applies ACES/exposure and encodes to sRGB; linear SDR output applies ACES/exposure and uses the target's automatic sRGB storage. `tone_map_sdr=false` selects encoding without tone mapping or exposure. HDR working output remains linear and unexposed for the 2D frame's finishing stage, so it is not tone-mapped twice. `dither=true` requests the stock stable one-step noise when explicitly encoding gamma SDR, with the same available-shader fallback as stock skies.

The stock alpha contract is preserved: covered colour is unpremultiplied for ACES/exposure and premultiplied again, additive colour can survive zero coverage, and bloom can extend coverage over a transparent background. Input values must follow that contract; the operation does not scan GPU pixels to infer their colour encoding or alpha convention.

Validation rejects incompatible formats/usages, arrays, foreign devices, mismatched bloom intermediates and any active output feedback before recording. Queued resources are retained; CPU uploads after a recorded public post-process read/write or surface write submit earlier work before updating, expiring frame-scoped preparation without an explicit device-idle wait. GPU producers remain in-frame. Written texture revisions invalidate material-input history as usual. A recording failure can leave partial output; stop subsequent graph operations. `debug_stats().bloom_draws` / `output_draws` report these fullscreen draws for both stock and public paths, separate from mesh counts.

The [surface graph example](../tools/surface3d/main.cpp) now records explicit HDR toon shading → stock bloom/output → GPU ASCII. Its reference/optimized pictures, cached plans, warmed GPU allocations and working-space transitions are checked by `kin_surface3d --test`. Public sky/fog, screen-space effects and temporal resolve remain separate migration work.
