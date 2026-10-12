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
lights are cleared with the draws. Spot lights (`add_spot_light`, a cone of a point light) and the shadows of lights that are not the sun
are below ("Spot lights and shadows from lights that are not the sun"). Light is added in linear light; the result
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

**Kept while nothing moves.** `lights.cache_shadows` (off by default: a game that pauses or has photo mode turns it on) keeps the map(s) from one render to the next and does not draw them again
when they would come out the same, to the byte: the sun, the area, the resolution, the **camera** (what casts into view, and the cascades' fit,
depend on it), the levels of detail, and every caster's mesh, material and copies must all be as they were (a cut-out's texture too: written
in place, it is seen; a render target as the texture is never cached). It pays for a paused game, a photo mode, a viewer at rest (the shadow pass
is a quarter to a half of such a frame) and costs a moving scene nothing measurable (a key of a few hundred bytes and a sample of the copies; the
copies are compared in full, stopping at the first difference, only once two renders agreed). `stats().shadow_cached` is true when it hit (the
shadow draw counts are then 0). Turn it off for a benchmark that renders one scene many times; `KIN_SHADOW_CACHE=0` turns it off for the whole
renderer. See the plan's "Opt-in speedups, phase 1".

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
With contact shadows also on (and `half_resolution` off) the two blurs are one dispatch a direction (`stats().blur_merged` is 2; the counts above
still count each feature's own); the result is the same to the byte. The blur ends in the first target again (raw, across, raw), so a feature
holds two R8 textures, not three.
In views of 1.5 million pixels or more (about 1440 x 1040; 1920 x 1080 and up) the occlusion reads a **linear depth chain** instead of the depth: one
compute dispatch writes the distance in front of the camera of every pre-pass pixel and four coarser levels (R32F, a weighted average of four texels
that leans to the farthest, after XeGTAO), a tap picks its level by its distance from the pixel (the first level from 20 pixels, the next from 40, and
so on) and the blurs read level 0, so the far taps of a wide search miss the cache far less and no tap divides to find its position. It gives the
pictures of the old path to within 0.2 levels on the reference scenes (`stats().ao_chain_built` is 1; the chain's own dispatch is not in
`ao_dispatches`) and saves 0.2 to 0.5 ms of the 1.3 to 2 of the occlusion at 1080p and 25 to 30% of it at 4K, but costs a little more where the occlusion's
reach is a few pixels (far views, the kiln at 1080p: +0.2 of 8 ms): the plan's "Opt-in speedups, phase 2". Below that size the old path runs and the
picture is the old one to the byte. `KIN_AO_CLASSIC=1` never uses the chain and `KIN_AO_CHAIN=1` always does (read on each render: tests, A/B timing).
Contact shadows blurred on their own (no occlusion in the same view, or the occlusion at half resolution) get one blur pass, not two (-0.03 ms at 1280 x 720,
-0.1 at 1080p; the pictures differ by under 0.3 levels on average): `stats().contact_dispatches` is then 2 (the march and the blur);
`KIN_CONTACT_CLASSIC=1` blurs twice.

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
- **Velocity** is written, from each copy's matrix of the frame before, kept in
  a buffer of its own beside the instances (the 128 B instance is untouched) and
  the view's last view-projection, **by the main pass** as a second colour
  output when temporal anti-aliasing is all that needs a depth pre-pass (no
  ambient occlusion, contact shadows or occlusion culling, and no opaque mesh
  with a custom shader): there is then no second geometry pass for it
  (`stats().taa_main_velocity`, `ao_draws` 0), which was the largest part of
  TAA's cost in big scenes (the kiln's fell by 2.2 ms of 7.2). Opaque and
  cut-out meshes write it; blended, additive and wireframe ones leave it as it
  is. With any of those other features the pre-pass is drawn once and shared,
  writing four floats (the normal and the velocity, instead of two). The
  pictures of the two ways are identical to the byte; `KIN_TAA_PREPASS=1`
  forces the pre-pass. The first-person overlay has its own pass
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
shader with a one-step dither (night skies ring in 8 bits without it). `stats().sky_drawn`.

**The fog** (`Fog3D`) is `density` per unit of distance plus a `height_density` that thins upward with `height_falloff`
from `base_height`, from `start`, up to `max_opacity`; the optical depth along each fragment's **view ray** is exact (the integral of the
height density is closed: `kin::fog_optical_depth` is the same arithmetic on the CPU). The colour is `colour`, or with `colour_from_sky` the
sky's in that direction (so far geometry melts into it at the horizon, whatever the time of day), plus the sun's light times a lobe of
`sun_anisotropy` (`sun_scatter`). Applied to everything the main pass shades through the stock shader (opaque, cut-out, skinned,
instanced, clustered, blended, additive); the overlay (a gun in the hand) is not fogged and custom material shaders are not.
`kin_fog.inc.glsl` holds it for a pass of your own. `stats().fog_applied`.

**Time of day** (`TimeOfDay3D`): `hours` (solar time), `day_of_year`, `latitude`, `axial_tilt`, `moon_phase`, `speed` (game seconds a
second for `advance(day, dt)`). `evaluate()` is a pure function of them (and the sky's haze and sun intensity): sun and moon
directions, the sun's colour, the moonlight, ambient and ground ambient (the sky's cosine-weighted light), a fog colour and density
suggestion; `apply()` writes them into the lights, the sky's moon and the fog's colour (not the haze, the stars or the density, which are
looks); `sun_times()` gives sunrise and sunset. North is -z, east +x. As a component: put a `TimeOfDay3D` on an entity and call
`WorldRenderState3D::advance_time_of_day(dt)` and `apply_time_of_day(scene)`.

**The sky's light** (`Lights3D::sky_lights_scene`): the atmosphere and ground (no disc, stars or moon) are drawn into a 256 x 128 panorama,
mipped and filtered with the environment machinery, and take the place of `environment` and `ambient`. It is made again only when
the sun or the sky moved by more than `Sky3D::light_threshold` and `light_interval` renders (4) have passed, so a day sweep re-filters a few
times a second (`stats().sky_env_regenerated`, `sky_env_generations`, `sky_env_cpu_ms`; about 0.4 ms of GPU each time). It lights the scene
whether or not the sky is drawn. Without it the hemisphere ambient is used as before.

`warm_up()` makes the sky's, the fog variants' and the dither's pipelines too. Harness: `kin_reference` JSON `sky`, `fog`,
`time_of_day`, `lights.sky_lights_scene`, a camera's `hours`, `at_sun`, `at_moon`; `procedural: "landscape"`; scenes `sky_day`, `sky_dusk`,
`sky_night`, `fog_valley`, `tod_sweep`. Sponza Walk's "Sky and fog (P3a)" section and `--preset sky|dusk|fog|night|day-night`.

### Spot lights and shadows from lights that are not the sun

Off by default; off, the picture and the cost are those without them (the old shaders are not touched: the stock fragment shader has four
more variants, used only when a scene has a spot light, a local shadow or a moon shadow). `kin/render3d/scene3d.hpp` (`SpotLight`,
`LightShadow3D`, `DirectionalShadow3D`, `Lights3D::spots`), `kin/render3d/local_lights.hpp` (the maths); docs/3d_plan.md "P3b" has the models
and the numbers.

```cpp
kin::SpotLight torch{.position = camera.position, .direction = camera.forward(), .color = {40, 38, 30}, .radius = 30.0f,
                     .inner_angle = 8.0f, .outer_angle = 18.0f};      // degrees from the axis
torch.shadow = {.enabled = true, .resolution = 1024};                  // a shadow map, if it is among the most important
scene.add_spot_light(torch);

kin::PointLight lamp{.position = {0, 3.5f, 0}, .color = {30, 27, 22}, .radius = 20.0f};
lamp.shadow.enabled = true;                                            // a cube of six maps (the faces the view can use)
scene.add_light(lamp);

scene.lights.max_shadowed_spots = 4;       // up to 8; of the lights that ask, the most important this many a frame get a map
scene.lights.max_shadowed_points = 2;      // up to 2 cubes
scene.lights.fill_shadow = {.enabled = true, .cascades = 2};           // the fill light (the moon) casts: its own cascades, over shadow_area
```

**A spot light** is a point light inside a cone: the same photometry (`color` is what a white surface in the middle of the cone shows
one unit away), `radius` and `size`, and a smooth falloff between `inner_angle` (full light) and `outer_angle` (none), by glTF's punctual
spot curve: `t = clamp((cos a - cos outer) / (cos inner - cos outer), 0, 1)`, light times `t * t`. Exactly zero from the outer angle
on, exactly full inside the inner one. At most 16 reach the view a frame (the cone's bounding sphere in view, the strongest and nearest
first, as the point lights). `kin::spot_attenuation(light, point)` is the same number on the CPU.

**Shadows.** A light's `shadow.enabled` asks for a map; of the spots in view that ask, the `max_shadowed_spots` most important have one,
and of the point lights the `max_shadowed_points`. A map is a perspective depth map over the cone (a point light's: six 90-degree faces
with a guard band, only those a visible surface can fall in drawn), in one array whose side is the largest `resolution` any shadow
light asks for (default `Lights3D::local_shadow_resolution`, 1024; a light that asks for less draws into a corner of it). Importance is
intensity times reach times the share of the view the light fills, and a light that had a map last frame counts 1.5 times over, so the
choice does not flip between frames; name a light with `id` (else its index in the list) to keep that across frames. Filtering is
3x3 comparisons as the sun's, `bias` (texels, times 1 + the surface's slope to the light) and `normal_offset` (texels) in the light's
own texel at the surface's distance, `softness` the taps' spacing, `near_plane` the map's, and `distance` the reach (the shadow fades
out over its last fifth and the map ends there). Casters are what the cone and the radius reach: opaque and cut-out meshes, instanced
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
or a point light (it is for the moon's cascades). `stats()` has `spots`, `spot_slot[i]` / `point_slot[i]` (the slot of `Lights3D::spots[i]`
and `points[i]`, -1 none), `local_shadow_maps`, `local_shadow_drawn`, `local_shadow_cached`, `local_shadow_draws`, `local_shadow_casters`,
`moon_cascades`, `local_shadow_bytes` and `lights_variant`.

Harness: `kin_reference` JSON `lights.spots` (`attach` carries one with the camera), `lights.points[].shadow`, `lights.fill_shadow`,
`time_of_day.shadows`, `procedural: "stage"`, options `--spots`, `--spot-shadows`, `--point-lights`, `--point-shadows`, `--moon-shadow`, `--local-res`;
scenes `spot_shadows`, `spot_night`, `spot_budget`, `moon_shadow`, `point_shadow`. Sponza Walk's "Spot lights and shadows" section and `--preset
spots|flashlight|moon|lamps`. `warm_up()` makes the light variants' pipelines and the maps the configured lights will want.

Memory: a layer is `side`^2 x 4 bytes (a spot 4 MiB at 1024, 1 MiB at 512), a point light six layers; the array holds the layers asked for
and gives them back after 120 idle renders. Not done: shadows from custom material shaders' own light, contact shadows for spots, a light
cone's in-scattering in fog (a lamp in mist shows no beam), soft shadows by light size (the filter is a fixed width).

### Particles and weather

Off by default; off, the picture and the cost are those without them (the old shaders are not touched: particles have three shaders of
their own, and the depth pre-pass is made only for soft particles). `kin/render3d/particles3d.hpp`; docs/3d_plan.md "P3c" has the models
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
a hash of all of it, and `save()` / `restore()` take the state to bytes and back for a rollback. A ground height (visual only: no
physics) kills or bounces. ECS: `ParticleEmitter3D` holds a system and is stepped at its entity's place by
`WorldRenderState3D::simulate_particles(dt, wind)` and drawn by `collect()`; a `Weather3D` component is the game's weather
(`advance_weather(dt)`, `apply_weather(scene, base_fog)`).

**Drawing** (`ParticleLook3D`, `Scene3D::draw_particles`): one instanced draw a batch of sprites (a system's, or the weather's layers),
tested against the depth of the opaque meshes and never written, after the scene's own blended meshes and before the overlay (the overlay's
and a Scene3D::overlay's particles are not drawn). Facings: `Camera` (turned by `rotation`), `Stretched` (along the velocity, `stretch`
seconds of motion long, head at the particle, for rain and sparks), `Axis` (turns about an axis to face the camera), `Flat` (in the plane
normal to the axis, a ring on the ground). A texture, an atlas of columns by rows (the sprite's `frame`, mixed with the next with
`blend_frames`), or a procedural soft disc (`softness`, `ring`). Blend `Alpha`, `Additive` (adds light; dimmed by fog, not mixed) or
`Premultiplied` (a texture that holds colour times alpha: alpha 0 with colour adds, inside one sprite). `sort`: back to front within the
batch (a radix sort on the view depth, stable: O(n), 24,000 sprites 0.2 ms); batches are drawn in the order of their mean depth, so two
systems overlap correctly as wholes and not particle by particle across systems. Additive never sorts.

**Soft particles** (`soft`, `soft_distance`): the fragment shader reads the view's depth, the opaque meshes' pre-pass depth (one sample a
pixel, a depth texture of its own: under MSAA the particle is shaded once a pixel and the depth read at its centre), and fades the sprite by
a smoothstep of how far it is in front of the surface behind it: 0 at the surface, 1 `soft_distance` or more in front (`kin::soft_particle_fade`
is the same number). It costs the pre-pass (a draw of the opaque meshes' depth and normal) where nothing else asks for one, and under TAA the
velocity then comes from it too; a scene without a soft batch makes none. The surface must be opaque: a blended mesh is not in the pre-pass.

**Light and fog.** The fog is `kin_fog.inc.glsl`'s, evaluated in the vertex shader at each corner and interpolated (the particle is small,
the closed form smooth), so a particle takes the fog a mesh at the same place takes, to a level of 255 (tested); `fogged = false` leaves
it out. `lit` (0 to 1) tints the particle by `kin::particle_light_tint(lights)`: ambient (sky and ground averaged) plus half the sun and half
the fill, which is what a lit white ball shows; 0 shows its own colour (fire, sparks, glows). With `TimeOfDay3D::apply` it follows the
day: snow is not white at night. No shadows fall on particles, and no spot or point light reaches them (the tint is the sun, the fill and the
ambient light only).

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

`stats()` has `particles_submitted`, `particles_drawn` (after the view's cull and the 1,048,576 sprites a frame it takes), `particle_batches`,
`particle_soft`, `weather_particles`, `particles_dropped` and `particle_cpu_ms`. `capabilities().particles` and `soft_particles`. Not done:
particles lit by spot lights, depth-hit splashes (the ground is one height), the velocity of a particle for TAA (a moving particle ghosts under
TAA; use MSAA for rain), collisions with anything, GPU-simulated particles (the CPU writes the instance buffer each frame), a sort across
batches, and rain that stops under a roof (it falls through Sponza's nave).

Harness: `kin_reference` JSON `weather` and `particles`, options `--rain`, `--snow`, `--wind`, `--no-weather`, `--no-particles`, `--soft`,
`--particles N`; scenes `rain_street`, `snow_valley`, `particles_soft`, `particles_blend`, `particles_budget`. Sponza Walk's "Particles and
weather (P3c)" section and `--preset rain|snow|storm|fire`.

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
loaded and `poll()` loads it again when the file changes. Skins and animations
are read too (see Skinned meshes below). Not read yet: morph targets (their
animation channels are ignored, with a line in the log), texture transforms,
second UV sets, `KHR_materials_*` other than unlit and emissive strength.

## Skinned meshes

A character is a mesh whose vertices follow a skeleton. kin keeps the three
things apart: the **skeleton** and its **clips** (plain data and maths on the
CPU, `kin/render3d/skeleton.hpp` and `animation3d.hpp`), the **pose** they make
each frame (one matrix a joint), and the **skinned mesh** that the GPU poses by
those matrices (`Scene3D::draw_skinned`). It is not `kin/anim` (the engine's
animation of registered properties): that one's state machines can choose the
clips and weights fed to this.

```cpp
kin::Model hero = r3d.load_model("models/hero.glb");          // skeletons, clips, skinned parts
kin::AnimationPlayer3D player{&hero.skeletons[0]};
player.play(hero.find_clip("Walk"));
std::vector<kin::Mat4f> joints;

// each frame
player.update(dt);                                             // clips sampled, blended, layered
player.joint_matrices(joints);                                 // world pose times inverse bind, one a joint
scene.draw_skinned(hero, joints, kin::Mat4f::translation(position));
```

**Data.** `Skeleton`: joint `names`, `parents` (always lower than the joint, so a
pose turns to matrices in one pass), the `rest` pose (local translation,
rotation, scale), `inverse_bind` matrices and a `root_transform` (the file's nodes
above the roots). At most `max_skin_joints` (256: a joint index is a byte on the
GPU). `Pose` is the local `JointPose` of every joint. `AnimClip3D` holds, per joint,
`Track3D`s of translation, rotation and scale keyframes with `Step`, `Linear` or
`CubicSpline` interpolation exactly as glTF specifies (rotations slerped; a cubic
spline's tangents scaled by the time between keys). A joint with no track keeps its rest value.

**Maths.** `sample_clip(clip, skeleton, time, loop, pose)`; `blend_poses(a, b, w)`;
`PoseMask::subtree(skeleton, joint)`, `::joints(skeleton, names)` and `blend_masked`
for an upper-body layer over locomotion; `apply_additive(base, additive, reference,
mask, weight)` for additive clips (hit reactions, breathing); `extract_root_motion`
and `remove_root_motion` for clips whose root joint carries the character's travel
(you get the delta between two times, wrapping at a loop's end, and the pose with the root held at rest);
`compute_joint_matrices(skeleton, pose, out)`.

**Player.** `AnimationPlayer3D` plays a clip (`play`), fades to another
(`crossfade_to(clip, seconds)`; a fade begun during a fade fades from the picture of
that moment, held, so nothing jumps), and has layers (`layer(i)`: clip, weight, mask,
additive, loop, speed). `set_root_motion(true)` holds the base layer's root and reports the motion
of the last `update` as `root_motion()`. The pointers to the skeleton and clips are
not owned: a `Model`'s stay put as long as the `Model` is not copied or changed.
Measured: about 2 us for a 24-joint character's `update` and matrices, 4.5 us with a crossfade.

**Loading.** The glTF reader takes skins (joints, `inverseBindMatrices`) and
animations (translation, rotation, scale channels; `weights` channels are ignored), and
`JOINTS_0` (u8 or u16) and `WEIGHTS_0` (float, normalised u8 or u16): four influences a
vertex, the weights renormalised to sum 1 (a vertex with none is bound to joint 0, an
influence naming a joint past the skin is dropped; both are logged). Joints are put parents-first
whatever order the file listed them in, and the vertices' indices follow. A skin with more
than 256 joints is not used (logged): its meshes are drawn as static meshes. A skinned
mesh's node placing is ignored (as glTF says); the joint matrices place it, and the transform you pass
to `draw_skinned` is the model's. A mesh on a node under a joint (a sword in a hand) with no skin of its
own is bound to that joint with all its weight, so it follows. `Model::skeletons`, `Model::clips`
(an animation of two skins is two clips of one name), `ModelPart::skin`, `Model::find_clip`; or read
the same without a GPU with `read_model_data` (for tools and tests).

**Meshes.** `MeshData::skin` is a parallel stream (four joint indices and four
weights a vertex), empty for a mesh that is not skinned; `validate`, `append` and `bounds`
know it. On the GPU it is a second vertex buffer (8 bytes a vertex: four joint bytes and four unorm
weight bytes that sum to exactly 255), so a mesh without a skin has the vertex buffer and pipelines it
always had. A skinned mesh cannot have levels of detail or clusters (`MeshOptions::levels`,
`clusters` are ignored, with a line in the log).

**Drawing.** `scene.draw_skinned(mesh, joint_matrices, transform, material, tint, id)`:
the matrices (at least `Mesh::skin_joint_count()`) are copied into the scene; each call is one copy with its
own pose, and copies of one mesh and material one after another are drawn as one instanced draw. The
renderer puts the frame's matrices in one storage buffer and each copy's first one in a buffer of its
own (the 128-byte instance is unchanged); the vertex shader moves each vertex by the weighted sum
of its joints' matrices and turns its normal by their linear part (right for rigid and uniformly scaled
joints; a non-uniform scale in a joint is approximated). `draw_skinned(model, joints, transform)`
draws all of a model's parts (skinned ones in the pose, the others as `draw` does).
`Renderer3DCapabilities::skinning`; `Render3DStats::skinned_copies`, `skin_matrices`, `skinned_draws`.

**Every pass** (P1a part 2). A skinned copy is drawn by the main pass, casts the sun's shadow (one map or any
cascades, cut-out materials too), is in the depth and normal pre-pass (ambient occlusion, contact shadows, the
occluder depth of occlusion culling, cut-outs included), has its own velocity under temporal anti-aliasing, and is
picked (`View3D::pick` names its `id`). It is culled by its pose: its bounding sphere is made each frame from where its joints
put the spheres each of them pulls on (never smaller than the posed mesh, so an arm that swings into view is not culled).

*Velocity.* The renderer keeps last frame's joint matrices of each view (the whole array, uploaded again as the "previous" joints)
and matches each copy to last frame's the way it matches the model matrix: by `MeshInstance::id` when non-zero, else by its place
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

*Entities.* `SkinnedMeshRenderer` (a shared `Model`, tint, `visible`, `id`) and `Animator3D` (see Entities below).

Limits: a skinned mesh has no levels or clusters; a non-uniform scale in a joint skins normals approximately; the velocity of a
copy matched by order with a changed count is a guess (give it an id); D3D12 and Metal are untested.

**One-dimensional blends.** `player.play_blend(points, parameter)` / `crossfade_to_blend(points, parameter, seconds)` and
`set_blend_parameter(p)`: the clips of `BlendPoint3D{clip, position}` blended by the parameter (the two points either side of it;
held at the ends), each clip at the same phase of its own length. The base's `time()` is then a phase (0 to 1) that advances by
`dt * speed / (the duration blended at the parameter now)`, so a walk and a run step in time and a changing parameter never jumps.
Fades, root motion and layers work as for a clip.

## Inverse kinematics

A hand to a hold, a foot on the ground, a head to a point: solvers that bend a posed skeleton after the clips and before the
matrices (`kin/render3d/ik.hpp`, CPU only, no shader changes; the stage P1b of the plan). They work on the **local** pose and
keep the joints' **model-space** world matrices in step, so they chain (each sees what the one before did) and cost a few
microseconds a character.

```cpp
kin::Pose pose = player.pose();
std::vector<kin::Mat4f> world, joints(skeleton.joint_count());
kin::compute_world_matrices(skeleton, pose, world);

kin::TwoBoneOptions arm;
arm.pole_mode = kin::PoleMode::Direction;          // the elbow points back
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
`TwoBoneState` and `bend_swing_degrees_per_second` (the ECS constraint keeps the state) the side swings at that bounded speed
instead, whatever the target does: 6 degrees a frame at 360 a second, from 24 degrees a frame stateless in the test sweep. The one
case that stays: a target through the exact centre of a chain folded on itself with equal bones turns the upper bone half a turn in
one step (the joints are in the same places); passing 2 cm off it the same turn takes steps of 2.3 degrees.

**`solve_look_at`**: a head, or a chain (spine, neck, head) whose `LookAtJoint::weight`s share the turn. Each joint has yaw and pitch
limits (an ellipse, seen from the head); past a limit the joint is held there and eased back toward forward over `release_degrees`,
so a target going round behind never flips the head from one side to the other. The turn is the shortest one, so nothing twists.
Smoothing is a `LookAtState` (the followed direction, in the space the target is in) with `smooth_seconds` and `max_speed_degrees`
and a `dt`: the function does not hold state itself.

**`solve_chain_fabrik`**: FABRIK for tails, ropes and spines (any number of joints, a tolerance and an iteration cap, optional
fixed root); bone lengths are kept, each joint but the tip is turned the shortest way onto its new bone. A `pole_mode` / `pole`
turns the solved chain about the line from its first joint to the tip (so neither end nor any length moves) until its bulge
faces the pole; a straight chain, or a pole along the line, fades the turn out rather than flip.

**`plan_foot_placement` / `apply_foot_placement`**: for each leg it casts the game's `GroundQuery` (`std::function<optional<GroundHit>
(Vec3f from, Vec3f to)>`; P2's physics raycast is one) from above the animated ankle, puts the ankle target on the hit lifted by
the foot height (a foot in the air keeps its clearance over the ground it is above), tilts the foot to the normal clamped to
`max_tilt_degrees` (not at all in the air), and finds how far the pelvis must go down for the lowest foot (`max_pelvis_drop`;
`max_pelvis_raise`, 0 by default, for the pelvis to rise on a step). `FootPlacementState` eases the pelvis offset and a foot that
the ground fell away from (it rises at once: no sinking into a step). The plan touches no pose; `apply_foot_placement` moves
the pelvis joint and solves the legs.

*Foot locking* (`FootPlacementOptions::lock_feet`, off by default; needs a state and a `model_to_world`, which the ECS gives it): a foot
whose animated sole is within `lock_height` of the ground is planted, and its ankle target is held at the place it was planted, in
the world, while the body moves over it; it lets go when the animated foot rises past `lock_release_height` or the animated ankle is
`lock_release_distance` from the held place (the clip has moved on), and eases back to the animated ankle over `lock_replant_seconds`
(smoothstep), then plants again. The leg's reach and `max_stretch` are the limit: a held foot the leg cannot reach is as near as it
can get. *Toes* (`FootLeg::toe`, a child of the ankle): a second ground query under the animated toe; the toe turns to that normal,
clamped to the foot's slope and to `toe_max_tilt_degrees` from its animated turn, and not at all while the foot is in the air. The
ground query is called once for each leg (and once more for each toe), however many times the constraints are solved in a frame
(the ECS keeps the answers for the frame).

### In the ECS animator

`Animator3D::ik_constraints` (an `IKConstraint` each: `name`, `kind` TwoBone / LookAt / FootPlacer / Fabrik, `joints` by name, `weight`,
`fade_seconds`, a target, a pole, options) are solved in `WorldRenderState3D::collect()` (or by `apply_ik()`), in list order,
on the pose the clips and layers made, with the entity's `WorldTransform3D` of the frame; then the joint matrices are made from
the result. An animator with none is exactly what it was, to the byte.

```cpp
kin::Animator3D animator{.clip = "Idle"};
animator.add_ik({.name = "feet", .kind = kin::IKKind::FootPlacer, .joints = {"pelvis", "hip_L", "knee_L", "ankle_L", "hip_R", "knee_R", "ankle_R"},
                 .foot_heights = {0.05f, 0.05f}, .ground = [&](kin::Vec3f from, kin::Vec3f to) { return physics.raycast(from, to); }});
animator.add_ik({.name = "right_hand", .kind = kin::IKKind::TwoBone, .joints = {"shoulder_R", "elbow_R", "hand_R"}, .fade_seconds = 0.15f});
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

- **FABRIK** (`IKKind::Fabrik`): `joints` root to tip, a target, an optional pole (`set_pole_position` / `set_pole_direction`), `weight`
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

**Lights.** `PointLight3D` and `SpotLight3D` (colour, radius, size, for a spot the cone's angles, a `LightShadow3D`, `enabled`) on an entity
with a `Transform3D` put a light at its place in `collect(scene)` (a spot shines down the entity's own -Z, the way a camera looks; parent it to a
camera or a hand to carry it); the entity's id names the light for the choice of which has a shadow map.

**Characters.** `SkinnedMeshRenderer{.model}` (a `std::shared_ptr<const Model>`: its parts, skeleton and clips) draws the model at the
entity's `WorldTransform3D`, in the pose of the entity's `Animator3D`. It is its own component, not `MeshRenderer`: a mesh renderer's
entities merge into one instanced draw of one mesh, and a character is several parts with a skeleton and a pose of its own.
`WorldRenderState3D::animate(dt)` (once per fixed step or frame) steps every animator; `collect` draws them.

```cpp
auto hero = std::make_shared<const kin::Model>(r3d.load_model("models/hero.glb"));
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
world.entity().set(kin::Transform3D{}).set(kin::SkinnedMeshRenderer{.model = hero})
              .set(static_cast<const kin::Animator3D&>(a));   // a const lvalue: flecs's set() moves from a non-const one
// each fixed step: set parameters from game code, then
animator->params.set_float("speed", velocity);  animator->params.set_trigger("jump");
state.animate(dt);
```

Without a machine, `Animator3D::clip` names the base clip (changing it crossfades over `fade_seconds`). The state machine is
`kin/anim`'s `AnimationStateMachine` and `AnimationParams`, evaluated with its own `evaluate()` (one transition a step, triggers
consumed); no change to `kin/anim`. What is not bridged: `kin/anim` events and registered-property tracks (a skeletal clip is not
one). `apply_root_motion` moves the entity's `Transform3D` by the base layer's root motion; `remove_root_motion` only holds the root
(in place). Not yet serialised: the 3D components have no component schema or prefab support (neither does `Transform3D`).

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

## Memory and warm-up

**Targets of the opt-in features are given back.** Ambient occlusion's and contact shadows' textures, the pre-pass's depth and normals,
temporal anti-aliasing's history and velocity, and occlusion culling's pyramid, history and occluder depth (with the renderer's second gathered
buffer of cluster indices) belong to a view and are made by the first render that asks. A feature that has not been used for 120 renders of the
view gives its targets back (they go to SDL_GPU's allocator, so they are reusable at once, though a driver may keep its blocks); a feature that is
on again makes them again, and the pictures are those of a fresh view (the temporal history starts afresh, as after `reset_history()`). A
feature toggled at least every 119 renders never lets go, so toggling does not thrash. `stats().feature_bytes` is the video memory those targets
hold now (1280 x 720, everything on: Sponza 45.7 MiB, the kiln with occlusion culling 93.2 MiB; nothing when off) and `stats().targets_released`
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

Not yet: morph targets (the plan's later stages list what else).


### Clusters

`r3d.create_mesh(data, {.clusters = true})` (or `load_model(path, {.clusters = true})`) builds for one huge mesh a hierarchy of
clusters of at most 128 triangles (`kin/render3d/mesh_clusters.hpp`; `levels` is then ignored): leaves cut from the mesh, then
rounds of grouping about 4 neighbouring clusters, simplifying the group to half with its outer border locked, and splitting the
result, up to one cluster (open meshes stop at a few roots). The mesh is welded to share its vertices (by every attribute; a
flat-shaded one, as a model with no normals, by position, UV and colour, and its normals are then made in the shader from the screen-space
derivatives of the position: a normal map has no effect on it). `Mesh::cluster_count()`, `cluster_levels()`, `cluster_roots()`,
`leaf_triangles()`, `cluster_triangles()` read it back. The kiln (5.15 M triangles) takes 5 s at load and 1.5 GB for the process, less than
with no hierarchy, since the welded vertices replace the unindexed ones on the GPU.

With `scene.lod.enabled` each copy is cut through its hierarchy every frame (in compute on the GPU where the device can, else on the CPU: see below): a cluster is drawn when its own error spans at
most `error_pixels / bias` pixels at the nearest point of its bounding sphere and its parent's does not, so near parts stay fine
and far parts are coarse inside one mesh, with no cracks (the borders between groups were locked in every simplification above them) and every
surface covered once. `force_level = N` draws the cut at hierarchy depth N (0: the leaves). The same triangles are drawn by every pass for a copy
(colour, pre-pass, velocity, pick, each shadow map; casters outside the view that cast into it are cut for the shadow only). The indices
of what is drawn are compacted into one buffer a frame; one draw per copy per pass. `stats()` has `cluster_copies`, `clusters_tested`,
`clusters_drawn`, `cluster_upload_bytes`, `cluster_ms`. Off (the default) a mesh with clusters is drawn whole and the picture is that of the mesh
without; alpha-blended copies are never cut. Clusters pay for one huge mesh seen near and far (the kiln close up: 0.4 against 2.3 ms of GPU for whole-mesh levels, 4.4 at
full detail); not for many copies of small meshes, nor mid-size ones.

**The cut on the GPU** (`scene.lod.gpu_culling`, default on; `Renderer3DCapabilities::gpu_driven_clusters`). A cluster's test needs no neighbour, so
four compute passes test every cluster of every copy at once: the cut and the frustum tests of each pass (the camera's, each cascade's or the one
shadow map's: the same expressions as the CPU's, in the same order), a prefix sum of the chosen clusters' triangle counts, and a gather of their
indices into one index buffer in the clusters' own order (so a picture is the same every run), which indirect draws read: one multi-draw a draw and pass,
the copy's slot in the instance buffer as the first instance. The CPU only lists the copies (0.03 to 0.08 ms of `render()` on the kiln,
against 0.4 to 0.9 for the CPU cut). The picture is the CPU cut's, byte for byte (tested with 1, 3 and 4 cascades, occlusion, contact shadows, temporal
anti-aliasing, picking). It costs video memory: the table of clusters (80 bytes each) and every level's indices (the kiln: 62 MB more than the leaves
alone, 7 MB of table), and the gathered indices (`scene.lod.max_cluster_triangles`, default 4 million triangles, 12 bytes each, shared by all
passes and copies; a cut that passes it is cut short, never written out of bounds). The counts of what it drew are on the GPU:
`scene.lod.stats_readback` reads them back at the end of `render()` (waiting for the GPU, as `pick` does: for tools and tests) into `stats()`
(`clusters_drawn`, `triangles`, `cluster_overflow`, `cluster_triangles_all`); without it those are 0 and `draws`, `shadow_draws` count the multi-draws.
`Renderer3D::read_cluster_picks` lists the clusters it chose. `stats().cluster_gpu` says which cut a render made.
`KIN_GPU_CLUSTERS=0` forces the CPU cut. Alpha-blended copies are never cut.

**Occlusion culling** (`scene.lod.occlusion = {.enabled = true}`; off by default; `Renderer3DCapabilities::occlusion_culling`). Clusters hidden
behind other geometry are not drawn. It needs the selection, the GPU cut and clustered meshes (else it is ignored, with one line in the log), and the picture
is the one without it, to the byte in every test (a slit narrower than a pixel between two pieces of wall can be seen through by a multisampled picture, and
not by the depth the test reads: 78 bytes of a 160 x 120 picture in the one case measured). It is the standard two-pass method: the clusters
that showed last frame (a history of one word a cluster, per view) are drawn first, with the frame's opaque meshes, into a depth; a pyramid of its farthest
depths (three compute passes) is built; every other cluster is tested against it, this frame's own depth, so that what comes into view
(a turn, a door, a moving copy) is never culled; the rest is drawn. A wrong history only costs speed: `View3D::reset_history()`, a resize and
a change of the scene's clustered draws start it empty. With a pre-pass (ambient occlusion, contact shadows or temporal anti-aliasing on) the pre-pass is made in
two halves and nothing is drawn twice; without one a depth-only pass of the first set is the price, because the multisampled main depth cannot be
sampled. The shadow maps are not culled (a caster hidden from the camera still shadows what shows), nor the overlay, and meshes with a shader of their own are no occluders.
`depth_margin` (relative, 0.001) and `pixel_margin` (1) widen the test a little. `stats()` has `occlusion`, `occlusion_levels`, `occlusion_dispatches`
and, with `stats_readback`, `clusters_first_pass`, `clusters_occluded`, `triangles_occluded`, `clusters_wasted` (drawn first, found hidden after all);
`clusters_drawn + clusters_occluded` is the cut's clusters in view. It adds fixed work (the pyramid, a second cut, a second multi-draw a draw) and wins only
where much more is hidden than that costs: see the plan's G3d for the numbers (a wall hiding 256 copies of a 100 thousand triangle clustered mesh:
12 to 19% of the GPU time saved; the kiln and Sponza: nothing to save, 0.03 to 0.12 ms spent). Copies drawn from CPU instance lists
(`draw_instanced` of meshes without clusters, the crowd) are not tested.