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
   - Textures: sample, sample with mip bias or level, triplanar, normal-map decode, texture arrays.
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
   multiply the variants, so v1 has none: graph branches on uniforms only.

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
