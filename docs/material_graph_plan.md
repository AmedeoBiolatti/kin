# Material graph: plan

Status (2026-11-05): P0 and most of P1 built on exp/material-graph (see "Progress" at the end). Model: Godot's visual shaders and Unreal's material editor. A graph of
nodes computes a surface's attributes; it compiles into the engine's own shader template, in every pass and variant
the engine draws, so a graph material is lit, shadowed, fogged and GI-lit exactly as a stock one is.

## Why not today's custom shaders

`Material3D::shader` (docs/rendering_3d.md, "Custom shaders") replaces kin's fragment shader with one written against
`kin_material.inc.glsl`. A graph could generate such shaders in a week, but they would look worse than stock materials:

- They have no GI. `KIN_GI` (probes, slopes, coverage, area lights) is only in the stock light variants.
- They have no light grid. They keep the 16 strongest point and spot lights.
- They are native-only. WebGL2 rejects them.
- The shadow and pick passes draw them as if they had no shader. A cutout cast by the graph's alpha is lost.
- They get one compiled shader. There are no fog, light or velocity variants, and no skinned forms.

So the graph must compile into the stock template instead, as Godot's and Unreal's do.

## The model

Two layers, as Godot has both visual shaders and text shaders:

1. **Surface shaders (text).** A GLSL file defines one function, which the stock shaders call where they now read the
   material's maps:

   ```glsl
   // the inputs: uv, uv1, world position, normal, tangent, view, vertex colour, time, the instance's tint,
   // the screen position; the parameters by name (generated accessors)
   void kin_surface(in KinSurfaceIn i, inout KinSurfaceOut o) {
       vec3 n = kin_noise3(i.position * param_scale);
       o.base_color = mix(param_dirt, kin_sample(param_albedo, i.uv).rgb, n.x);
       o.roughness = 0.4 + 0.5 * n.y;
   }
   ```

   `KinSurfaceOut` has `base_color`, `alpha`, `normal` (tangent space, or world with a flag), `metallic`, `roughness`,
   `occlusion`, `emission` and `alpha_cutoff`. Its defaults are the stock material's values, so writing nothing gives
   today's material.

   Later, `kin_vertex(inout KinVertex v)` adds a world-position offset (wind, waves) in the vertex stages, and
   `kin_light()` sets a custom lighting response through the existing `KIN_DIRECT_LIGHT` hook (toon).

2. **The graph.** `kin.material-graph/1` JSON holds the nodes, their links, the parameters, the shading model (lit or
   unlit), the blend mode and the domain (surface first). Its compiler generates a surface shader: one GLSL function
   with typed, ordered statements, unused nodes dropped. Nodes, like Godot's and Unreal's:
   - Inputs: uv, position, normal, view, vertex colour, time, screen position, camera distance, the object's position.
   - Parameters: float, vec2/3/4, colour, texture.
   - Textures: sample, sample with mip bias or level, triplanar, equirectangular (by direction), normal-map decode,
     texture arrays.
   - Maths: arithmetic, mix, clamp, smoothstep, remap, dot, cross, length, sin, cos, pow, frac, step, swizzle, split,
     join.
   - Procedural: value, gradient and cellular noise, fbm, checker, gradients.
   - Effects: fresnel, depth fade (needs the scene depth: later), parallax (later), UV panner and rotator.
   - Utilities: reroute, comment boxes, groups (subgraphs: later).
   - A raw-code node (a GLSL expression).

A material instance is a `.kinmaterials` file naming a graph and the values of its parameters. That fits the existing
layering of base, part and variant patches unchanged:

```json
{"schema": "kin.materials3d/1", "material": {"graph": "materials/rust.kinmatgraph",
  "params": {"dirt": [0.3, 0.2, 0.1], "albedo": "textures/metal.png", "scale": 4.0}}}
```

## Engine work

1. **The template hook.** Stock fragment shaders call `kin_surface()`. The default `kin_surface()` is today's map
   reads, kept byte-identical (proved with the parity matrix and the Bistro). Variants compiled per graph:
   `mesh_material` and `mesh_velocity`, each plain, fog, light and light-fog, plus `shadow_mask` and `pick_mask`.
   That is 10 fragment shaders. The mask variants only need the alpha path, and the optimizer drops the rest. Vertex
   stages need variants only once `kin_vertex` exists; they come in static and skinned forms.
2. **Parameters.** Each graph material extends its record in `KinDrawMaterials` (the per-draw storage buffer) with its
   parameter vec4s, packed by the compiler. That keeps PR #90's merging of compatible draws, since graphs differ by
   shader and not per draw. The per-graph limit is about 16 vec4.
3. **Textures: the hard limit is samplers.** The stock light variant binds 15 (maps 0 and 2–5, shadows, environment,
   AO, contact, local and moon shadows, GI 12–14), and Metal allows 16 a stage. A graph therefore gets the 5 map
   slots plus 1 free slot: 6 textures. Texture arrays give more: one binding, many layers of the same size, close to
   Unreal's shared samplers. Recommendation: 6 textures and arrays in v1. Freeing slots later (moving the shadows into
   one array) is renderer work of its own.
4. **GI as an emitter and in the bounces.** The voxels and area lights read a material's constants (colour,
   emission). A graph material gets a GI proxy: its mean base colour and emission, found when it is cooked by
   rendering the graph over its UV square. A graph may also set it by hand. A `gi_color` output can come later.
   Receiving GI comes free from item 1.
5. **Cooking.** One compiler library in the engine, shared by the studio and the cook, writes GLSL, then SPIR-V with
   `glslc` (plus DXIL or MSL where SDL_GPU needs them, as for built-in shaders), into a cooked bundle: the variants,
   the parameter layout and the GI proxy. Development builds compile and reload a changed graph at run time, as
   `compile_material_shader` does now. Shipped games never run `glslc`.
6. **Pipelines.** That is 10 variants a graph, each made when first drawn. To avoid hitches, a scene's graphs are made
   when it loads, as the stock pipelines are.
7. **WebGL2 (later).** The compiler emits the same function in GLSL ES for `webgl2/mesh.frag`'s template. Graph nodes
   are simple enough for two dialects. Until then, WebGL2 draws a graph material as its stock fallback (its proxy
   colour and the parameters' texture as base).
8. **Static switches (later).** Unreal compiles a permutation for each combination of compile-time booleans. They
   multiply the variants, so v1 has none: graph branches on uniforms only. (The Unreal import resolves them when it
   imports: one kin graph per set of switch values.)

## The editor (kin-studio)

The studio already draws node graphs (`graph_layout`: dialogue, state machines; flowing arrows from right-side ports
to left-side ports, and Tidy). A material document adds:

- A node palette with search.
- Typed ports, coloured by type, with implicit splat and swizzle.
- A preview of a sphere, cube or plane under the scene's sun and environment, recompiled live on each edit, with
  errors shown on the node that caused them.
- The parameter panel, which becomes the instance's inspector.

Per-node thumbnails (as both Godot and Unreal show) come later: each needs its own small shader.

## Phases

| | What | Done when |
|---|---|---|
| P0 | The `kin_surface()` hook in the stock template | the parity matrix and Bistro stills are byte-identical |
| P1 | Surface shaders (text): variants, parameters in `KinDrawMaterials`, textures, mask and pick passes | a hand-written surface shader equal to a stock material renders pixel-identically; a cutout shadow follows the graph's alpha |
| P2 | The graph format, compiler and node library | golden GLSL and picture tests per node, plus a few composite materials (rust, wet ground, rim light) |
| P3 | Cooking, `.kinmaterials` with `graph` and `params`, the GI proxy, hot reload | a game ships a graph material without `glslc`; GI bounces use its proxy |
| P4 | The studio editor and live preview | edit, save and reload in the studio; the game picks up the change |
| P5 | Later: `kin_vertex` (world-position offset), custom lighting, WebGL2, subgraphs, scene depth, decals and post materials, 2D | |

## Open questions

- Is kin-studio the editor's home? It bends the "2D first" order.
- V1 scope: lit and unlit surfaces only, or decals and post-processing too?
- Is vertex animation (wind, water) needed early? It is the most invasive part: every vertex stage, shadows included.
- Should 2D share the graph (kin's 2D shader effects), as Godot's visual shaders cover canvas items?

## Progress

**P0 (done).** `KinSurface` and `kin_stock_surface()` in `kin_material.inc.glsl`; kin's mesh fragment shader is two
halves (`kin_mesh_surface.inc.glsl`, `kin_mesh_main.inc.glsl`) with `KIN_SURFACE` between. Stock shaders are
unchanged in what they compute: the Bistro's five bench stills (1600 x 1000, GI on) are byte-identical before and
after.

**P1 (mostly done).** Surface shaders as text:
- `Material3D::surface` (`SurfaceShader3D`). `Renderer3D::compile_surface_shader(path)` (glslc now, every variant)
  and `create_surface_shader(variants)` (SPIR-V); `surface_shader_source()`, `surface_variant_defines()`;
  `compile_glsl` takes defines.
- Eight variants (mesh and mesh_velocity, each plain, fog, lights, lights+fog), each with its stock twin's layout,
  chosen wherever the stock one would be: lit by the GI, the light grid, local shadows, fogged, with velocity.
- Parameters: `Material3D::shader_params` after the stock block in uniform slot 1 (`kin_surface_param(0..3)`).
- `kin_compile_surface_shader()` in `cmake/kin_embed.cmake`: the variants at build time.
- `test_gpu_surface_shader`: an identity surface is byte-identical to the stock material in all six setups
  (plain, fog, lights, lights+fog, velocity, velocity+lights); a tint surface is lit and takes its parameters; the
  build-time variants draw byte-identically to the run-time ones; unlit; a broken surface is refused with glslc's
  message. Full ctest passes (but the NTFS-only release-acceptance test); the Bistro still byte-identical.

**P1 still to do.**
- Cut-outs by the surface's alpha in the shadow, pick, pre- and visibility passes (their shaders get the surface
  and its inputs: new varyings).
- The retained-instance and merged-draw paths (`mesh_material` variants: parameters in `KinDrawMaterials`).
- A `time` input (the frame uniforms), the object's position, and the parameters by name (P2's compiler).

## Importing (user, 2026-11-05: Tier 1 for Godot, Unity and Unreal, plus a partial Tier 2 grown as needed)

- Tier 1, the standard PBR materials: Godot `StandardMaterial3D` / `ORMMaterial3D` (.tres, text); Unity `.mat`
  (Standard, URP Lit, HDRP Lit: YAML, textures by GUID through `.meta`; roughness = 1 - smoothness, the
  metallic/smoothness and mask maps repacked, tiling and offset); Unreal through its editor's Python (material
  instance parameters and textures; DirectX normal maps' green flipped; centimetres), its glTF exporter's bake as a
  fallback for anything else.
- Tier 2, graphs, for a common subset of nodes, unknown nodes falling back to Tier 1 or a bake: Godot VisualShader
  (and the Godot shading language's fragment()), Unity Shader Graph, Unreal material graphs. kin's node set (P2) is
  chosen to cover what the three share.
- Not: Unity HLSL / ShaderLab, Unreal Custom HLSL nodes (baked instead).

**Done: Godot Tier 1.** `import_godot_material()` (`kin/render3d/material_import3d.hpp`), `kin_import_material`
(tools/import_material), `write_materials3d_asset()` (the .kinmaterials writer the import and the studio share);
`kin_material_import3d_tests` (fixtures; with KIN_TEST_GODOT_PROJECT a whole project). The Bistro's 116 materials
all import; against the Bistro port's own converter (games/gallery/bistro/tools/bake.py, validated against Godot by
picture), 113 by name, every field agrees but: 12 lights whose emission is off in the resource (the game's script
switches them on: noted), 3 glass of the add blend mode (bake.py made them opaque; the import is Godot's), 2 that
need a DDS map repacked.

**Engine gaps the import found.** A material-level UV transform: done (`Material3D::uv_scale`, `uv_offset`, every
pass; the Bistro byte-identical; `test_gpu_material_uv_transform`: offset and scale equal to the shifted or repeated
texture, lit, unlit, cut-out and its shadow, to the pixel). Decoding BC-compressed DDS to repack; backlight,
subsurface, clearcoat, refraction, detail maps (graph territory).

**Done: Unity Tier 1.** `import_unity_material()` (`kin_import_material unity`): URP Lit / Simple Lit / Baked Lit /
Unlit, Standard, HDRP Lit, by properties; GUIDs through `.meta`; packed metallic/smoothness (factor baked in),
HDRP's remapped mask map, occlusion from G, emission (keyword; gamma colours to linear, HDRP's linear), surfaces;
tiling through the material UV transform. TIFF (LZW, Deflate, PackBits, predictor) and PSD decoded
(image_decode3d.cpp, import_decode.c: stb_image's PSD and zlib, static). Corpus: Unity's open BoatAttack (URP; its
`.mat` and `.meta` only, fetched sparse into kin-wt/material-graph/out/corpus): 36 of 36 import; one material's three
4096 x 4096 LZW TIFFs fetched and imported end to end (maps checked by eye). Not verifiable here (no Unity): that
colour properties are stored in gamma as the inspector shows them and become linear by the sRGB curve (assumed,
not checked against Unity), HDR emission colours the same: to check on a Unity install before relying on emission.

**Done: Unreal Tier 1.** `tools/import_material/unreal_export.py` (inside Unreal 5.8's editor, headless; nothing
saved) + `import_unreal_material()` (`kin_import_material unreal`): outputs as affine terms of one texture channel
(constants, parameters with the instance's values, samples and their tiling, Multiply/Add/Subtract/OneMinus/Mask,
a Lerp by a constant 0 or 1); ORM maps used as they are; DirectX normals flipped unless the texture flips them;
one-colour maps folded. BistroUE (engines/unreal: 121 materials, made from kin's own Bistro content) exported in 1 min
42 s and imported: 121 of 121; against kin's original glTF materials every difference is explained (one-colour maps
folded into the colour, exactly; the night lights' emission behind the game's parameter collection, noted; additive
glass as BistroUE has it). `unreal_probe.py` builds 7 materials of other shapes in a scratch project
(engines/unreal/out/MaterialProbe); their export is `tests/fixtures/unreal`.

Tier 1 is done for the three engines. Next: P2 (the graph and its compiler), with the node set the three share, so
that Tier 2 (Godot VisualShader, Unity Shader Graph, Unreal material graphs beyond affine terms) maps onto it.

**P2 (done, first cut).** `kin.material-graph/1` and its compiler (`material_graph3d.hpp/.cpp`): 48 node types
(Godot / Unity / Unreal's common math, texture, procedural and effect nodes, a raw GLSL node), references with
swizzles, built-in inputs (uv, position, normal, view, vertex colour, time, screen, camera distance, the stock
surface's fields), Unreal-like implicit casts, parameters packed into shader_params, textures in the five map slots
(a "data" role undoing the slot's stock reading), dead nodes left out, errors naming their node. Shader helpers in
`kin_graph.inc.glsl` (deterministic hash noises, triplanar, the UV tangent frame for normal maps). `Scene3D::time`
(frame.time.x). `Renderer3D::compile_material_graph`. Tests: `kin_material_graph3d_tests` (layout, GLSL, errors;
every node type compiled by glslc in all 8 variants); `test_gpu_material_graph` (a graph of no outputs and one
remaking the base colour byte-identical to the material; a checker, its parameter and time; the normal map node
within a few pixels of kin's own; errors).

Next: P3 (`.kinmaterials` naming a graph and its parameters' values, cooking graphs at build time, hot reload) and the
Tier 2 graph imports onto these nodes; P4 the studio's editor.

**P3 (done).** `.kinmaterials` `"graph"` and `"params"` (numbers into shader_params, textures into their slots,
merged per name across layers; `"graph": null` clears); `Assets3D::load_material_graph` (cooked `<graph>.spv` when at
least as new as the source or without one; else glslc now; cached; invalidation by source or cooked file reloads);
the cooked format (`pack/unpack_cooked_material_graph3d`, `cook_material_graph3d`, the layout as
kin.material-graph-layout/1); `kin_material_graph` (check, glsl, cook, nodes) and cmake's `kin_cook_material_graph`.
Tests: material_assets3d (bind, merge, clear, errors, draw, edit-and-reload, cooked without glslc, without its
source, a stale cooked file refused), material_graph3d (layout and pack round trips, damage refused, cook now, cooked
at build time). Not yet: cut-outs by the
graph's alpha in shadow/pick passes, the retained draw paths.

**GI proxy (2026-11-07).** Not the planned mean colour found at cook time: a stand-in made by the renderer, per
material (its parameters and maps are the instance's), the first time a render's GI scene takes it in
(`GpuRenderer3D::bake_gi_proxies`, gpu_gi3d.cpp): the material drawn over its UV square (64 x 64, a square 4 units
across seen from above) unlit and again lit by nothing, `gi_proxy.frag` taking the second (its emission) from the
first (base colour plus emission); the voxelizer reads the base colour (and alpha) in the base map's place, and the
emission in the emissive map's for a material whose own fields emit. Before, the GI read the material's own maps: an
imported Unreal graph's base slot holds whatever the graph samples (59 graph textures there, 41 of them normal maps,
masks or noise), so the landscapes bounced their normal map's lavender and the stone tiles yellow. Checked: the
voxels of a surface painting (0.6, 0.2, 0.1) over a blue base map and glowing (0, 3, 0) over a red emissive map hold
the surface's, blue and red without the stand-ins (`test_gpu_gi_proxy`); `kin_material_lineup --gi-view albedo` on
eight Fab imports (the landscapes sand, earth and snow; the dev grids, stone tiles and zombie skin their own colours).
Limits: as on flat ground facing up (a slope's rock is not seen), an animated surface at its bake's time. The
compiler's `SurfaceTraits3D` (2026-11-07, same day): a graph with an emission output glows in the GI whatever the
material's emission fields say (8 Fab materials had lost theirs: lakes, rivers, pond scum, riverbank, sand, whose
Tier 1 reading found no plain emissive colour), and the voxels take the copy's vertex colour and tint only where the
graph's colour or alpha reads them (they multiplied a graph's own colour by them). Checked by `traits` in
kin_material_graph3d_tests and `test_gpu_gi_proxy` (the glow with the fields off; a tinted copy). Left (a follow-up):
re-baking animated stand-ins, and the surface run in the voxelizer itself for what depends on position, normal or a
vertex-colour mask (in the 65 Fab graphs, time reaches the colour or glow of 35, position 24, the normal 23).

**Deep graphs and shipping (2026-11-08).** The Windows CI crashed in kin_material_import3d_tests: the importers and
the compiler follow a node's inputs recursively, and MSVC's frames (about 15 KB a level in the Unreal importer's
translate) overflowed Windows' 1 MB main thread at a chain of 70 (Fab's deepest graphs chain 56 kin nodes, more in
Unreal's with reroutes and functions). The compiler crashed too, past about 500 at 1 MB on Linux, and at 5000 at 8 MB.
Now the compile and the three graph imports run on a thread with a stack of its own (`detail::run_on_large_stack`,
256 MB reserved; on the caller's where there are no threads, a browser) and chains past 1024 (128 on a caller's
stack) are an error naming the node (the compiler) or a note keeping the output's Tier 1 value (the importers); the
Godot reader also stops at calls nested 64 deep. Checked: chains of 1024 (compiler) and 1000 (Unreal) compile and
import, past them said, by kin_material_graph3d_tests and kin_material_import3d_tests (also passing with a 256 KB
main stack); Godot and Unity chains of 1000 import and 1100 are said, by hand, on a 1 MB stack. Shipping: `kin_game`
cooks its content's graphs beside them (`kin_cook_material_graphs`, one target for each content folder) and its pack
waits for them; where it can't cook (Android's cross-compile, no glslc) a shipping build stops at configure naming
the graphs not cooked. Checked: a test folder cooked by the function; a cooked graph loaded from a content pack with
glslc gone (kin_material_assets3d_tests); a graph dropped into the Summit content cooked, and all four games on that
content (and their packs) depending on it. Not fixed: kin's JSON reader recurses into nested arrays and objects with
no limit (a pre-existing core matter: a file nested a million deep would still overflow).

**Stock rendering re-checked (2026-11-08).** After the lighting additions (backlight, clear coat, specular, wrap, the
multiple-scattering switch) and the rebase: the Bistro game built against main (66972da) and against 4d2a9e1 renders
eight 1600 x 1000 stills (GI, TAA) byte-identically: the bench's five views, one with fog and bloom, two at night
with 38 and 48 lamps lit; each twice per build, the same (`out/bistro_shots.sh`, `out/bistro_night.sh` in the
worktree).

**Stock rendering's cost (2026-11-08).** The same two Bistro builds, the bench's perf settings (3408 x 2130, 300
frames' warm-up, 600 measured), main and the branch run in pairs, alternating which goes first (on this laptop the
second of a pair runs hotter: about 1% slower in daylight, 13% at night, whichever build it is), the GPU cooled to
78 C before each pair and its clocks logged (`out/perf/gated.py`, `analyse.py`):
- GPU: the branch's frame about 1% longer in daylight (+0.5 to +1.1% per view, order-balanced; +1.2 to +1.5% in GPU
  cycles), about 2% at night with 38 lamps. The new lighting terms are constants in a stock material (zero backlight,
  coat and wrap; specular 0.5), so likely the UV transform and the draw's material record (64 to 80 bytes): not
  pinned down.
- CPU: recording a frame's draws took 0.2 to 0.4 ms more (about 20%), in every view and round. Two causes, found by
  perf: `GiKey::add` (the GI scene's key of each draw and copy, every render) stopped being inlined once
  `gi_proxy_key` gave it more callers (1 call site on main, 34 on the branch): every field of every copy became a call
  and a copy of a size known only at run time; and the TAA key hashed all 64 `shader_params` of each draw (16 on
  main), though a stock material reads none. Fixed: `GiKey::add<T>` mixes a value's words with its size known (2 call
  sites left, both of sizes known only at run time); the TAA key takes the parameters something reads (a surface's
  64, a custom shader's 16, a stock material's none). After, at 1600 x 1000, rounds rotating main / before / after:
  the start view +0.29 ms before, +0.07 ms after (median of pairs); night -0.05 ms after (noise). The eight stills
  stay byte-identical to main's. Left: copying the larger `Material3D` (64 floats, the surface and graph maps) into
  the scene each frame, about 0.1 M cycles a frame for Bistro's 831 draws.

**Cooked size.** 669 KB to about 220 KB: the variants optimised (glslc -O, which also drops the debug names; run-time
surface compiles too, so development draws what ships: an optimised cooked graph of no outputs is byte-identical to
the stock material) and the payload deflated (stb_image_write's zlib; inflated by stb_image's), with an FNV-1a hash
of it in the header (a cut or a changed byte is refused); format version 2, version 1 still read. What stays is eight
shaders' worth of kin's lighting (about 30 KB each, deflated); zlib's 32 KB window can't share them.

**Batching.** Surface materials in the retained-instance path (a static batch's copies: nothing to change but the
exclusion), the draw plan (cached commands; the surface's 144-byte uniform push reads the material itself, so cached
commands can't serve old parameters) and the merged draws: four more variants (`SurfaceVariant3D::Material...`,
mesh_material.frag's defines) that read each copy's parameters from a second table (16 floats a copy, beside the
draws' material records, made only for scenes with surface draws). Optional: cooked versions 1 and 2 (8 variants)
still load, unmerged. Cooked format version 3 (the variant count). A cooked graph is now about 330 KB.
`test_gpu_surface_batching`: 384 draws merge (over 200 merged) plain, fogged, lit and both; a merged identity
surface is byte-identical to merged stock materials; merged tinted draws to the same unmerged (each copy's
parameters); a retained 256-copy batch to the same copies streamed.

**Cut-outs by the graph's alpha.** The stock cut-out shaders (shadow, pick, pre-pass, velocity pre-pass, visibility)
are now bodies (`kin_<pass>.inc.glsl`) calling `kin_cutout_alpha(uv, position, vertex alpha)`: the base map's alpha,
or, in a surface's cut-out variant (`KIN_SURFACE_ALPHA`), its `kin_surface_alpha` (declared before main, defined
after it: GLSL prototypes). The shadow-mask vertex shaders also give the world position. A surface's cut-out variants
bind its five maps and a longer cut-out block (parameters, the sRGB decoding, `Scene3D::time`). The graph compiler
compiles the alpha again in a mask mode (uv, position, time, vertex alpha, the base map's alpha, parameters, maps,
noise; the view, normals, fresnel, triplanar, normal maps and glsl nodes refused, as a warning naming the node).
Five optional variants (17 in all; cooked only for graphs with a cut-out alpha). `test_gpu_material_graph_cutout`: a
plane cut by a checker of nodes draws, shadows, occludes (AO pre-pass, TAA velocity pre-pass) and picks byte-identically
to the same plane cut by a checker texture, and differs from it uncut by ~1,230 pixels (its holes and their shadow).

**Tier 2: the three engines' graphs (done, partial node coverage).** `import_unreal_graph`, `import_unity_graph`,
`import_godot_graph` (`kin_import_material ... --graphs`) write a `.kingraph` beside the `.kinmaterials` and name it
there, its parameters' values the material's (an instance's, a material's properties); an output the graph can't
take keeps its Tier 1 value (Unreal, Unity) and is noted, naming the node; a graph that does not compile, or samples
more than five textures (kin's map slots), falls back to Tier 1. Shared in `material_import3d_impl.hpp`'s
`GraphBuilder`: the nodes and their widths (as kin's compiler makes them), cutting a value to a port's width, swizzles
composed, the five slots. Two node types added for them: `append` (Unreal's AppendVector) and `unpack_normal`.
- Unreal: `unreal_export.py` exports the whole expression graph (each node's class, properties, pins); constants,
  parameters, samples (normal maps unpacked, DirectX green flipped unless the texture flips it), coordinates and
  their tiling, the arithmetic and unary nodes, Lerp, Clamp, Power, Dot, Cross, Distance, masks, AppendVector, Time,
  World Position (cm, Unreal's axes), Vertex Color, Camera Vector, normals, Fresnel, Panner, Desaturation, Reroute, Static Switch
  (see below for functions and switch values), Bump Offset, Transform (tangent, world, view), If, collection
  parameters' defaults; approximated and said (2026-11-06): local space, the object's position, Depth Fade, Scene
  Depth, cube maps, landscape coordinates, Parallax Occlusion Mapping, a second UV set, particle inputs. Fab corpus
  (107 materials): full graphs 39 -> 75, partial 50 -> 14, Tier 1 only 18 (more than five textures, or 64 floats).
  Then: maps past the five slots in `Material3D::graph_maps` (a 2D array, `"slot": "layers"`; the sampler after the
  stock ones in the surface variants, their storage bindings one later; older SPIR-V detected and bound as before),
  parameters packed first-fit, and folded into a material's own graph past 64 floats; Custom nodes of one expression;
  Tangent Space Normal off. Later the same day: World Position Offset (the graph's `offset`, eight vertex variants
  of each surface: kin_surface_vertex.inc.glsl, the stock vertex shaders' hook; every pass that places vertices; a
  view's previous time for the velocity), function calls' outputs by index (157 of 1231 references in the Fab corpus
  had the wrong name: the island landscape's normals were its base colour), OpenEXR maps in the import. BistroUE: 121 of 121 with graphs; the probe's M_GraphMix checked node by node.
  Then, from the 107-material comparison with Unreal's renders: cube maps as their export (run in the editor,
  `unreal_export_editor.py`: the commandlet asserts unwrapping a cube) unwraps them, a Radiance HDR in longitude and
  latitude, sampled by the `texture_equirect` node (kin's environment layout, seamless derivatives) in the direction
  (-y, z, x) of Unreal's; Rotate About Axis; View Property (view size, camera, the temporal sample's index by the
  time); Screen Position's PixelPosition; a domain not a surface's said. Textures' colour adjustments (brightness,
  saturation, curves, hue: Unreal applies them building a texture, its exported source has none) and legacy gamma
  baked into the imported maps as Unreal applies them (linear, HSV): the landscapes' rock and snow were 50% too
  bright and coloured. Unreal's snapshots cached (`unreal_snapshot_cache.py`: the editor only for pictures out of
  date). Now close 51, minor 18, major 30, 8 not surfaces. Then an unconnected Base Colour black (WaterSplash), and
  the object's space in offsets (`object_position`, `object_to_world`, `world_to_object`: the copy's model matrix,
  passed to kin_vertex_offset): Unreal's Object Position, local Transform and Transform Position read as they are in
  World Position Offset (the plants' wind: Megascan's quad 19 -> 3.6), approximated beyond it (a fragment does not have
  the placing: every mesh vertex shader would have to pass it on). The snapshots' kin side filters textures 8x anisotropically
  (`kin_material_lineup --anisotropy 8`), as Unreal's default (r.MaxAnisotropy): a landscape's world-aligned UVs on
  an upright quad change along one screen axis only, which trilinear filtering blurs (Mountain 13.9 -> 4.2). Now close
  52, minor 21, major 26, 8 not surfaces. Two-sided foliage: the surface's `backlight` (a graph output; Unreal's
  Subsurface Color, exported now, white where not connected), lit as Unreal's foliage is (each light on the back,
  wrapped by half, through a GGX lobe of alpha 0.6; the ambient light from behind): the Megascan plant's sphere, its
  inside lit from behind, 29.8 -> 3.4. Now close 53, minor 21, major 25 (PlantsGrass's thin blades: their edges).
  Clear coat: the surface's `clearcoat` and `clearcoat_roughness` (graph outputs; glTF's KHR_materials_clearcoat: a
  GGX highlight of the coat's roughness, the layer below dimmed by the coat's Fresnel once, Unreal's twice), Unreal's
  Clear Coat and Clear Coat Roughness (1 and 0.1 where not connected). unreal.MaterialProperty has no CustomData0/1:
  the export reads those two inputs from the material's T3D. The 15 FancyDev materials: Fancy Markup minor -> close,
  Metallic Red major -> minor; now close 54, minor 21, major 24. The majors left of them are rough full metals 12-20%
  brighter in kin: its multiple-scattering compensation, which Unreal's legacy shading (Substrate off:
  USE_ENERGY_CONSERVATION 0) does not have.
  Specular and translucency lighting: the surface's `specular` (0.08 times it a dielectric's reflectance, Unreal's
  Specular, now imported) and `diffuse_wrap` (0 Lambert's; 0.5 and 1 the light's first two spherical harmonic bands or
  the first alone). The export writes a material's translucency_lighting_mode: a translucency lit from Unreal's light
  volume (TLM_VOLUMETRIC_*: the Cheaper water, the ocean, the splash) is diffuse alone (Specular 0) and wrapped (0.5
  directional, 1 not); per pixel, not per vertex. Waterfall Cheaper's quad 27.5 -> 3.1, Waterfall Arc Cheaper's 25.6 ->
  12.1, Pond Scum Cheaper (both) and WaterSplash close, River Cheaper minor; now close 57, minor 19, major 23. Left
  as test artefacts: the lake/river spheres (their offset folds the sphere into overlapping translucent petals,
  blended in index order, which Unreal's mesh build reorders: reversed in kin, they look like Unreal's) and the
  Desert's quad (its world-aligned UVs on an upright quad read one texel row at v = 0, a wrap edge: 3 mm of depth
  changes it more than the gap to Unreal).
  Time and placing: Unreal's snapshots logged the clock of the frame before the one that drew the screenshot (a tick
  late for kin: the ocean's quad 45.8 -> 11.5 once logged right); now the world's clock is stopped (its least time
  dilation) while a material is pictured, so the time logged is the time drawn. A fragment now has the copy's origin
  (`object_position`: a flat value from the mesh's vertex shader), so Unreal's Object Position is read as it is beyond
  the offset too: the waterfalls offset their clock by it (time + position / 4096 s, a quad and a sphere 2.2 m apart
  ran 0.05 s apart), Waterfall Cheaper 3.1/23.0 -> 2.5/2.6, Waterfall Arc Cheaper 12.1/39.5 -> 3.0/3.2. Rough metals:
  Scene3D::specular_multiscatter (on) left out with kin_material_lineup --single-scatter, as Unreal's legacy shading
  (Substrate off: no energy conservation) does: Test R10 M1 23.1 -> 0.2, R05 14.7 -> 2.8, the FancyDev metals minor.
  Now close 59, minor 22, major 18 (with multiple scattering, kin's default); close 61, minor 25, major 13 like for
  like with Unreal's single scattering. The majors left: foam and moving ocean (Rapids' high contrast; the ocean
  spheres' lower halves), the mirrors' sun glint (kin's roughness floor, 0.045), the pond scum's darker patches, and
  the lake and river spheres (a vertex animation texture read by UV sets 2 and 3, which neither sphere has, on
  translucent petals blended in index order).
- Unity: Shader Graph 10+ (the multi-JSON format), sub-graphs inlined (their properties bound to the node's slots),
  the dynamic slots' widths as Unity concretizes them (cut to the narrowest connected vector), fixed slots cut;
  Keyword nodes by the material's keywords (the graph file named for the keywords on); colour spaces from the
  default textures' `.meta` (`sRGBTexture`); the URP target's surface, alpha clip, two sides, Unlit. BoatAttack: 36
  of 36 import, 7 graphs, all cooked by glslc; what stops the rest: Custom Function, Object and Camera nodes, a
  pre-10 sub-graph, the vertex stage, fog. `tools/import_material/make_unity_fixture.py` writes
  `tests/fixtures/unity`.
- Godot 4: VisualShader resources (their own file or inline); port numbering, functions, operators and defaults
  read off Godot 4.7.2's generated code (the output's ports are stable through 4.x: Bent Normal Map was appended
  last); the fragment stage; Normal Map unpacked as Godot does (its depth the strength); one-assignment Expression
  nodes as glsl; view-space inputs as kin's world ones (noted); cull, unshaded, alpha scissor.
  `tools/import_material/godot_probe.gd` makes `tests/fixtures/godot_visual` in a scratch project.

Not verified: any of the three against its engine's render of the same material (structure and glslc only).

**Parameters: 64 floats (2026-11-05).** Measured on 107 free Fab materials: 39 fell back to Tier 1 because their
parameters filled more than 16 floats (master materials: stone, wood, water, up to 110 floats counted). Now
`surface_param_count3d` = 64 (`Material3D::shader_params`; custom `shader`s still read the first 16): the surface
block, the cut-out block and the merged draws' table (64 floats a copy) carry all of them. Cooked format version 4;
version 3's merged-draw variants (the old table stride) are left out, so those draws go unmerged. Fab after it: 28
full graphs, 73 partial, 6 Tier 1 (all > 5 textures); 41 graphs cooked by glslc. Tests: the merged and unmerged
tint at floats 56-63 draw what the tint at 0-7 does; a cut-out scale at float 63; 16 vec4s compile, a 17th is refused.

**Unreal functions and static switches (2026-11-06).** `unreal_export.py` writes each called Material Function's graph
(`"functions"`: its nodes, inputs and outputs by name) and the static switch values (`parameters.switch`: an
instance's, else the material's default). Unreal's Python can't read two things, so they come from the asset's T3D
text (`ObjectExporterT3D`): a Named Reroute usage's declaration, and which output and mask each input takes (the API
names the first connection's output for every input from the same node: R and G of one parameter both came out as the
whole vector). The import inlines each call as a child graph (its pins feed the function's inputs; an unconnected
input takes its preview value, cut to its type), follows reroutes, resolves switches (from parameters, constants,
reroutes) and names the graph file for the switches it read (`M_Function__Green_on.kingraph`), so an instance never
overwrites its parent's graph. Nodes added: Round, Rotator, CameraPositionWS, PixelDepth, Feature Level and Quality
switches (SM6/Epic), ReflectionVectorWS, ScreenPosition, InverseLinearInterpolate, ConstantBiasScale, Sign, Tangent;
DotProduct and CrossProduct were misnamed. Fab (107 materials, 52 distinct functions): 40 full graphs, 49 partial, 17
Tier 1 (16 sample more than five textures, which the inlined functions reach; one fills more than 64 floats). What
makes the partial ones partial, counted by output: Custom HLSL (35), BumpOffset (24), DepthFade (19), SceneDepth (16),
LandscapeLayerCoords and Material Parameter Collections (13 each), Transform (11), ObjectPositionWS (9); outputs kin's
graph lacks (World Position Offset, Specular, Refraction); clear coat. Tests: the probe's M_Function (MF_Tint inlined,
its Amount the preview 0.5) and MI_Function_On (the switch on: green, its own graph; the parent's unchanged).
