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

```cpp
#include <kin/render3d/renderer3d.hpp>

kin::Renderer3D r3d{renderer};
kin::Mesh crate = r3d.create_mesh(kin::meshes::box({1, 1, 1}));
kin::View3D view = r3d.create_view({640, 360}, {.samples = 4});

// each frame
kin::Scene3D scene;
scene.camera = kin::Camera3D::orbit({0, 0, 0}, angle, 30.0f, 4.0f);
scene.draw(crate, kin::Mat4f::rotation_y(angle), {.color = kin::Color::rgb(200, 140, 80)});
r3d.render(view, scene);                                 // after the 2D drawn so far
renderer.draw_texture(view.texture(), {0, 0, 640, 360}); // laid in like a render target
```

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

## Meshes

`kin::MeshData` is triangles as data: vertices (position, normal, UV, colour)
and indices, three a triangle, counter-clockwise seen from the front.
`validate()` says whether it can be drawn; `append(other, transform)` builds
a shape from shapes. `Renderer3D::create_mesh` keeps it on the GPU as a
`kin::Mesh` (cheap to copy). Meshes may outlive the renderer.

Shapes made in code (`kin::meshes`): `box`, `plane`, `sphere`, `cylinder`,
`cone`, `torus`, `star_prism`, `octahedron`, `wedge` (a ramp).

## Scenes

A `kin::Scene3D` is filled each frame and holds no GPU state: a camera, the
lights, and draws.

- `draw(mesh, transform, material, tint, id)`: one copy. Copies of the same
  mesh and material one after another are drawn as one instanced draw. `id`
  is what picking says is under a pixel showing it (see Views).
- `draw_instanced(mesh, instances, material)`: many, each a transform, a
  tint and an id.
- `add_instances(mesh, count, material)`: room to fill in place (valid until
  the next draw is added).

`kin::Material3D`: `color` (times the mesh's vertex colours and each
instance's tint), `texture` (the base colour's, by the mesh's UVs),
`emissive` (light of its own, a share of its colour), `unlit`, `double_sided`
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
  UVs on the GPU, so meshes need no tangents. `occlusion` (R, with
  `occlusion_strength`) dims the ambient light only. `emissive_texture`: a
  colour times `emissive` (without it, `emissive` is a share of the base
  colour as before).
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
  all of the material's textures do past 0..1. Models set them from the
  glTF sampler (`REPEAT`, the glTF default, when it names none); samplers are
  made by filter and wrap on first use.

`wireframe` draws only the mesh's triangles' edges, a pixel wide, unlit in
the material's colour, just in front of faces in the same place (a little
depth bias): a model's edges drawn over it, or lines on the ground made of
degenerate triangles (`a, b, b`). Wireframes cast no shadow and are not
picked. They need line fill, which desktop GPUs have; where the pipeline
can't be made they are not drawn.

`blend`: `Opaque` (the default), `Alpha` (over what is behind by its alpha;
drawn after the opaque, furthest first) or `Additive` (adds light: sparks,
tracers, flashes, in any order). Transparent draws are depth-tested but don't
hide what is behind them.

Copies wholly outside the camera's view are skipped (`scene.cull`, on by
default): each is tested as its mesh's bounding sphere, moved and scaled with
it, and only the visible ones are sent to the GPU. `stats().culled` counts
the rest.

`scene.overlay()` is a scene drawn over this one, in front of everything in
it, with its own camera and this scene's lights: a first-person gun or hands,
which then never sink into a wall. It is drawn in the same pass, in its own
slice of the depth range.

Lights: `scene.lights.sun` (`direction`, toward the light; `color`, linear,
above 1 is brighter: what a white surface facing it shows, so a Lambert
surface of albedo a shows a times `color`; a glossy surface adds its highlight), `scene.lights.fill` (a second sun that casts no
shadow, black by default), `scene.lights.ambient` (with
`ground_ambient`, the light from below where it differs: faces between take
a mix by how far they face up), and point lights
(`scene.add_light({position, color, radius, size})`: muzzle flashes, explosions,
lamps). A point light falls off with the square of the distance, as light
does: its `color` is what a white surface facing it shows one unit away
(twice as far, a quarter), and the light is windowed to nothing at `radius`
(`(1 - (d/r)^4)^2`, as glTF's punctual lights). `size` is the light's own
size (1 cm by default): nearer than that it is no brighter, as the light
spreads over a body that big. Give flashes and fireballs theirs, or what
stands in them blazes white. Before G1a the fall-off was `(1 - d^2/r^2)^2` without the inverse
square: colours of those days are about the old colour times the square of the
distance they were used at. At most 16 point lights light a
frame: those whose reach is in view, the strongest and nearest first; point
lights are cleared with the draws. Light is added in linear light; the result
is stored as the 2D pipeline stores colour, or kept HDR (see Views).

Image-based light: `scene.lights.environment` is an `Environment3D`, an
equirectangular HDR picture of the sky or a room (`r3d.load_environment(
"sky.hdr")` reads a Radiance `.hdr` through the content folder or pack;
`r3d.create_environment(size, rgb_floats)` takes linear RGB floats, rows top to
bottom, 2:1). With one set, surfaces take their ambient light from it instead
of `ambient`/`ground_ambient` (which are then not used; the sun, fill and point
lights go on): the diffuse from its irradiance by the normal, the specular
from a mip a roughness (GGX-filtered) along the reflection, times the same
split-sum fit as before. `environment_intensity` scales it,
`environment_rotation` (degrees) turns it about +y. The picture's centre is
+x, its top +y, and going right from the centre turns toward +z (Blender's
world texture in kin's axes). The filtered copies (a 7-level equirect chain up
to 1024 x 512 for the specular, 64 x 32 for the irradiance, half floats) are
made by compute shaders in the first render that uses the environment, once
(`stats().environment_prefilters`); that render needs `compute` and a float
format that storage textures can write, else the hemisphere stays. An
environment is not drawn as a background (the view's `background` colour is),
and in the overlay it is carried into the overlay's space.

The sun casts shadows over `scene.lights.shadow_area`, a box round what
casts and receives them (the level): one shadow map
(`shadow_resolution`, 2048 texels by default) over that box, filtered over
3x3 taps. Opaque meshes cast (`casts_shadow`); copies culled from the
camera still cast when their shadow may fall in view (the copy swept down
the light, through the area, reaches the camera's view), and are left out
otherwise. A smaller box is sharper. The overlay gets no shadows.

A single-sided mesh casts with the faces it shows, toward the sun: for a
closed mesh that is the same shadow as both sides for half the work, but a
lone quad facing away from the sun (a ceiling seen from below) casts
nothing. Make it `double_sided` to cast from both sides. `stats()` counts
the shadow map's draw calls and the meshes drawn into it.

**Cascades.** One map over a big area is coarse: its texels are the area's
size over `shadow_resolution`, the same far and near. `lights.shadow_cascades`
(1 by default: the single map above, the picture unchanged; 2 to 4, more is
4) splits the camera's view by depth instead, nearer cascades covering less,
and gives each its own `shadow_resolution` square map (a layer of one depth
array, a shadow pass each). `lights.shadow_distance` (100 units by default,
never past the camera's `z_far`) is how far from the camera they reach;
the splits are a blend of logarithmic and linear (0.6 of the log one), so
the first cascade covers a few metres and the last the rest. Each cascade's
box is the sphere round its slice of the view, so it keeps its size as the
camera turns, snapped to whole texels in the sun's space so edges don't
shimmer as it moves; `shadow_area` still limits it, and is what casts and
receives. `mesh.frag` picks the cascade by the depth of the pixel along the
camera's forward, and fades into the next over the last tenth of each range
(no visible seam), with 3x3 taps and a depth bias and normal offset that
scale with the cascade's texel. A copy casts into each cascade whose slice its
shadow may fall in (the same test as above, a cascade at a time), so a
far-off thing isn't drawn into the near maps. Without a depth array to render
into the renderer makes fewer layers (down to one: the single map), says so
once in the log, and never fails. Costs more than one map: a pass and the
casters of each cascade, a few more taps; use it where the area is
much bigger than what the camera sees close up. `stats()` has
`shadow_cascades` and the copies drawn into each (`cascade_casters`).

### Ambient occlusion

`scene.ambient_occlusion = {.enabled = true, .radius = 0.5f, .intensity = 1.0f,
.half_resolution = false}` (off by default; off, the picture and the cost are
those of a renderer without it). The opaque and cut-out meshes the camera
sees are drawn first into a depth and world-normal pre-pass (one sample,
whatever the view's MSAA; the main pass is not changed and does not reuse its
depth); GTAO (2 slices, 8 steps a side, a thickness heuristic) runs in compute
from it, then a depth-aware blur (R8 result). `mesh.frag` multiplies the
ambient light and the environment's diffuse by it (Jimenez et al.'s
multi-bounce fit, from the surface's colour) and the environment's reflections
by a specular occlusion term; the sun, fill and point lights are not touched.
`radius` is how far (units) something can still shut light off: a few tenths
of the size of what you want creased (0.5 for props, several units for rooms);
`intensity` is the power the result is raised to. Blended, additive and
wireframe meshes read no occlusion (they would read what is behind them),
nor does the first-person overlay, which is not in the pre-pass either: its
own lighting stays as it was. Where the device lacks a sampled depth, a
two-float colour target or an R8 storage texture, occlusion is off with one
line in the log. `half_resolution` computes at half the view's size (about a
third of the cost; a little softer at depth edges). `stats()` has
`ao_draws`, `ao_instances` and `ao_dispatches` (0 when off). Costs a second
draw of every opaque copy and about 0.3 ms or more of compute at 1280 x 720:
see the plan's G2b results.

### Contact shadows

`scene.contact_shadows = {.enabled = true, .distance = 0.5f, .thickness = 0.1f,
.intensity = 1.0f, .steps = 20}` (off by default; off, the picture and the cost
are those of a renderer without it). For the small shadows a shadow map is too
coarse for: the foot of a pole, a plate on the ground. A compute pass marches
from each pixel toward the sun through the pre-pass's depth (the same pre-pass
and targets as ambient occlusion, drawn once when both are on, so it works with
occlusion off), starting a different amount along the first step at each pixel;
a sample counts as blocking when the depth there is nearer the camera than the
ray, but by no more than `thickness` (a thing that is further in front is taken
to be a thin occluder the ray passes behind, so a distant plate in front casts
nothing). The result fades over the last 40% of `distance` and near the
screen's edge, goes through the occlusion's depth-aware blur, and is read by
`mesh.frag`, which multiplies **only the sun's term** by it, after the shadow
map's: the fill, ambient light, environment and point lights are untouched.
`distance` (units) is how far toward the sun to look; `thickness` (units) how
deep an occluder is assumed to be (smaller: fewer false shadows from near
things, thin shadows may thin out; larger: the reverse); `intensity` 0..1;
`steps` 4..64. Surfaces turned from the sun, blended, additive and wireframe
meshes, the first-person overlay and meshes that do not receive shadows read
none. Marched in compute rather than in `mesh.frag`: the main pass shades
every fragment of the overdraw and the march would run in each, where compute
runs once a pixel and `mesh.frag` stays one texture read. Without a sampled
depth, an RG16F colour target or R8 storage it is off with one line in the log;
with no sun it does nothing. `stats()` has `contact_dispatches` (0 when off);
the pre-pass shows in `ao_draws` and `ao_instances`. Custom shaders see a tenth
sampler (`uContactShadow`) in kin's include; ones compiled before still work.
Full size only; about 0.15 to 0.3 ms at 1280 x 720 (see the plan's G2c results).

### Temporal anti-aliasing

`scene.taa = {.enabled = true, .blend = 0.9f, .sharpness = 0.0f,
.jitter_scale = 1.0f, .jitter_samples = 8, .freeze_jitter = false}` (off by
default; off, the picture is bit-identical and nothing extra exists: no pass,
no buffer). Each frame is drawn a Halton (2, 3) fraction of a pixel off
(`kin::taa_jitter`, `kin/render3d/taa.hpp`), and a compute pass blends it into
the picture of the frames before. **It replaces MSAA while on**: the view
draws one sample a pixel whatever `View3DOptions::samples` asks
(`stats().samples` says 1).

- **Where the settings live.** On the scene, like occlusion: it can be switched
  live without making the view again. What it keeps between frames, the
  history, belongs to the **view** (the scene is rebuilt every frame): made by
  the first frame that asks, dropped by the first that doesn't. `View3D::reset_history()`
  drops it by hand (a camera cut, a scene swapped); a resize, the first frame
  and a change from a perspective to an orthographic camera do it by themselves
  (`stats().taa_reset`).
- **Jitter** moves the projection of the passes that draw the picture (the main
  pass, the pre-pass, the occlusion and contact-shadow maths that unproject its
  depth, the overlay). The frustum culling, the shadow cascades' fit, the pick
  pass, `Camera3D::project` and `ray` use the exact matrices.
- **Velocity** is written by the pre-pass (shared with ambient occlusion and
  contact shadows, drawn once; it writes four floats, the normal and the
  velocity, instead of two) from each copy's matrix of the frame before, kept in
  a buffer of its own beside the instances (the 128 B instance is untouched) and
  the view's last view-projection. The first-person overlay has its own pass
  and its own previous camera, so a gun is neither smeared by the world behind
  it nor trailed.
- **Matching copies from frame to frame** (the scene is rebuilt, so nothing
  says which copy is which): by `MeshInstance::id` when it isn't 0 (give moving
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
- **Resolve** (`taa.comp`): reproject by velocity, read the history with a
  Catmull-Rom filter (five taps), clip it to a 3 x 3 box of this frame's colours in
  YCoCg (mean and spread, inside the neighbours' range), mix with weight `blend`,
  less where the surface read from moved differently from this one (the
  velocity the last frame had there), where the history is read between pixels,
  with speed and with how far the clip moved it; none off the screen, for a new
  copy or the first frame. This frame's colour is read from its 3 x 3 neighbours
  by how near the pixel centre each sample fell (a Gaussian), not as the pixel's
  own sample, which would pull the picture to wherever the last few jitters were
  (with the jitter at 0 it is the pixel's own). Light is compressed
  (`c / (1 + max c)`) for the clip and the mix. `sharpness` (0 to 1) adds a
  local-contrast boost kept inside the neighbours' range, one more dispatch.
- **Before the post chain**: bloom, exposure and the tonemap work on the
  resolved picture. An 8-bit view is drawn in linear float into a texture of its
  own, resolved, then clipped at white and encoded (`view_output.frag` mode 3);
  what it loses is MSAA's resolve in encoded values (edges are averaged in linear light,
  as an HDR view's are), and the extra RGBA16F targets.
- **Not covered:** blended, additive and wireframe meshes have no velocity
  (they take the opaque mesh's behind them: sparks and glass trail a little, the clip keeps it
  short); a camera cut needs `reset_history()`; a pan of exactly half pixels a
  frame lines the jitter up with the motion and leaves part of each pixel
  unvisited (rates that don't repeat are fine); thin detail in a pan
  is softer than MSAA's. The occlusion's and contact shadows' per-pixel noise
  is fixed, not varied by frame: left.
- Without RGBA16F as target and storage, or without what the pre-pass needs, it is off with
  one line in the log. `stats()` has `taa_dispatches` (0 when off, 2 with
  sharpening), `taa_reset`, `taa_jitter` and `samples`.
  See the plan's G2d results for numbers.

### Levels of detail

`r3d.create_mesh(data, {.levels = 4})` (or `load_model(path, {.levels = 4})`) builds, at load, up to
`levels - 1` simpler versions of a mesh with meshoptimizer's simplifier (`MeshOptions`,
`kin/render3d/mesh_lod.hpp`): each about half the triangles of the one before (`target_ratio`),
normals, UVs and vertex colours weighed in, open borders locked, and the chain stopped where a level
would gain under 15%, have under 16 triangles, or lie further than `max_error` (a tenth of the mesh's
size) from the one before. **Level 0 is the mesh as given**, to the bit; the levels are only more index
lists over the same vertices (a mesh costs a few more indices, not more vertices). `Mesh::level_count()`,
`level_triangles(i)` and `level_error(i)` (an upper bound, in the mesh's units, on how far level i
lies from level 0: meshoptimizer's error for each step, added up) read them back. A mesh without
indices (a glTF with no normals is read as flat-shaded triangles) is welded by position, and each level gets
vertices of its own with flat normals (kept after the mesh's; level 0 is untouched), which costs memory: the kiln's
5 M triangles take 8 s and about 1.1 GB more at load for 4 levels.

`scene.lod = {.enabled = true, .error_pixels = 1.0f, .bias = 1.0f, .force_level = -1}` (off by default;
off, the picture, the statistics and the cost are those without it) draws each copy at the coarsest level
whose error, times the copy's largest scale, spans no more than `error_pixels / bias` pixels at the
nearest point of its bounding sphere (`distance` along the view, `0.5 * view height / tan(fov / 2)` pixels
at a unit; orthographic cameras use the view's pixels a unit). A copy has one level a frame, in the
camera's pass, the shadow passes (every cascade), the depth pre-pass, the velocity pass and picking, so
a shadow is that of the surface drawn. Copies of a mesh and material at one level are one instanced
draw, so a draw is up to `levels` draws; `stats().lod_copies_by_level` counts the copies the camera
sees at each level and `stats().triangles` the triangles drawn. Limits: whole meshes only (a big mesh
is drawn at one level whole: see the plan's G3b for clusters); no hysteresis (copies are not
matched from frame to frame except for TAA, so a copy at the edge of a threshold can change level as
the camera moves; what changes is under the error bound); alpha-blended meshes (drawn furthest first,
which levels would break) and meshes without levels are drawn as before; the overlay has a setting of
its own (`scene.overlay().lod`, off). The temporal history survives a copy changing level: copies are
matched by mesh and place, not by level.

## Custom shaders

A material can have a fragment shader of its own (`Material3D::shader`,
`shader_params`: four vec4s), in place of kin's, for a hologram, a force
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
with its shadow, fill, point lights, ambient or environment; call it for a
lit surface, skip it for an unlit one), `kin_output(colour, alpha)` (encoded
for the target, premultiplied: write `FragColor` with it), `kin_param(i)`, and
the raw `frame`, `material` uniforms and samplers for anything else. It
declares nothing else: samplers 0 to 7 and uniform buffers 0 to 2 are kin's.
The blend mode, depth and culling are the material's, as ever; the shadow and
pick passes draw the material as if it had no shader.

`r3d.create_material_shader(spirv)` makes one from SPIR-V (build it with
`kin_compile_glsl`, which adds the include path; `cmake/kin_spirv.cmake` does
too, and skips `*.inc.glsl`; see `examples/CMakeLists.txt`);
`r3d.compile_material_shader(path)` compiles GLSL at run time with glslc, for
development. Unlike 2D shaders there is no hot reload or reflection yet.
Signal Breach's hit shields (`examples/shaders/shield.frag.glsl`: Fresnel rim
and climbing lines, additive) are one. Passes of your own on an HDR view are
not offered: draw into the view's texture with the 2D renderer's shaders.

## Models

`r3d.load_model("models/crate.glb")` reads a glTF 2.0 file (`.glb`, or
`.gltf` with its buffers and images beside it) through the content folder or
pack: each primitive a mesh, placed by its node, with its material's base
colour and texture (mipmapped; nearest when the file's sampler asks), emissive
light, `BLEND` as alpha, `MASK` as a cut-out (`alphaCutoff`), double-sided and
`KHR_materials_unlit`, and its metallic and roughness factors and map, normal
map (and its scale), occlusion map (and its strength) and emissive map.
`scene.draw(model, transform)` draws its parts. A `kin::ModelFile` keeps one
loaded and `poll()` loads it again when the file changes. Not read yet:
skins, animations, morph targets, texture transforms, second UV sets,
`KHR_materials_*` other than unlit and emissive strength.

## Entities

`kin/ecs/render3d.hpp` puts meshes on ECS entities: `Transform3D` (position,
rotation, scale, relative to the flecs parent), `WorldTransform3D` (added with
it, composed through the hierarchy) and `MeshRenderer` (mesh, material,
tint). `WorldRenderState3D::propagate_transforms()` then `collect(scene)` turn
them into one instanced draw a mesh and material.

```cpp
kin::WorldRenderState3D state{world};
world.entity().set(kin::Transform3D{.position = {2, 0, 0}}).set(kin::MeshRenderer{.mesh = crate});
// each frame
state.propagate_transforms();
state.collect(scene);
```

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
`--headless --screenshot` use the same table. See the
[examples README](../examples/README.md#sponza-walk).

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

## Views

`Renderer3D::create_view(size, {.samples, .background, .hdr, .exposure,
.bloom, .bloom_threshold, .pick})`: `samples` is MSAA (1, 2, 4 or 8, fewer where the
device has fewer); `background` shows where nothing is drawn (transparent lets
the 2D under the view show).

A view made with `pick` also keeps, a pixel each, the world point and the id
of the scene's opaque or alpha mesh drawn there (not wireframes, additive
glows or the overlay). `Renderer3D::pick(view, pixel)` reads one back: `hit`,
`id`, `position`. It waits for the GPU: what the frame has recorded so far is
submitted, and 2D drawn after it goes on in a new command buffer, as after a
read-back. Ask once per pixel per picture (a studio keeps the answer under a
still pointer), not every pixel every frame.

`hdr` draws into a float picture that keeps light past white, then exposes
and tonemaps it (ACES: brights roll off, the darkest tones darken a little)
on its way to the 2D frame; an HDR 2D pipeline gets it linear and tonemaps
it with everything else. `bloom` (its strength; it turns `hdr` on) glows
round what is brighter than `bloom_threshold`: emissive materials past 1,
lit by strong lights. The glow belongs to the view, so 2D drawn over it
(a HUD) stays crisp. `render(view,
scene)` draws the scene into it, recorded between the 2D drawn before and
after; a view keeps its picture until rendered again, so a still model needs
no rendering each frame. `resize(view, size)` gives it a new size and texture.
`view.reset_history()` drops the history temporal anti-aliasing keeps.

`Renderer3D::capabilities()` says what the device has (depth bits, most
samples); `stats()` what the last render drew (draw calls, instances, copies
culled, shadow draws and casters (and per cascade), point lights, triangles, CPU time).

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

Not yet: skinned meshes, PBR materials (see [the plan](3d_plan.md)).


### Clusters

`r3d.create_mesh(data, {.clusters = true})` (or `load_model(path, {.clusters = true})`) builds for one huge mesh a hierarchy of
clusters of at most 128 triangles (`kin/render3d/mesh_clusters.hpp`; `levels` is then ignored): leaves cut from the mesh, then
rounds of grouping about 4 neighbouring clusters, simplifying the group to half with its outer border locked, and splitting the
result, up to one cluster (open meshes stop at a few roots). The mesh is welded to share its vertices (by every attribute; a
flat-shaded one, as a model with no normals, by position, UV and colour, and its normals are then made in the shader from the screen-space
derivatives of the position: a normal map has no effect on it). `Mesh::cluster_count()`, `cluster_levels()`, `cluster_roots()`,
`leaf_triangles()`, `cluster_triangles()` read it back. The kiln (5.15 M triangles) takes 5 s at load and 1.5 GB for the process, less than
with no hierarchy, since the welded vertices replace the unindexed ones on the GPU.

With `scene.lod.enabled` each copy is cut through its hierarchy every frame (on the CPU): a cluster is drawn when its own error spans at
most `error_pixels / bias` pixels at the nearest point of its bounding sphere and its parent's does not, so near parts stay fine
and far parts are coarse inside one mesh, with no cracks (the borders between groups were locked in every simplification above them) and every
surface covered once. `force_level = N` draws the cut at hierarchy depth N (0: the leaves). The same triangles are drawn by every pass for a copy
(colour, pre-pass, velocity, pick, each shadow map; casters outside the view that cast into it are cut for the shadow only). The indices
of what is drawn are compacted into one buffer a frame; one draw per copy per pass. `stats()` has `cluster_copies`, `clusters_tested`,
`clusters_drawn`, `cluster_upload_bytes`, `cluster_ms`. Off (the default) a mesh with clusters is drawn whole and the picture is that of the mesh
without; alpha-blended copies are never cut. Clusters pay for one huge mesh seen near and far (the kiln close up: 0.4 against 2.3 ms of GPU for whole-mesh levels, 4.4 at
full detail); not for many copies of small meshes, nor mid-size ones (the CPU cut costs 0.4 to 1.5 ms on the kiln until G3c moves it to the GPU).