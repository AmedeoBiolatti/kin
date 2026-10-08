# 3D Rendering — Experimental Plan

> Branch `exp/3d-rendering`, from 0.2.6 (d307186). A proposal: nothing here is
> built yet.

## What this is, and what earns it

The manifest says "2D first, 3D later" and "Kin earns its features". This
branch is the "later", built against one game that needs it: **Signal
Breach**, a first-person version of Signal Siege. Each 3D feature arrives
when that game, or kin-render-studio's 3D view, asks for it.

The renderer adds **3D drawn inside a 2D frame**. A 3D view is an offscreen
colour + depth target that Renderer2D lays into the frame like any render
target, with 2D drawing and ui2 under or over it. A full-screen shooter is
that view at the screen's size with the HUD drawn on top; a model turning in
a panel is the same view at 256².

Kin's targets are turn-based, card and puzzle games. A shooter is the right
stress test for a 3D renderer (speed, transparency, lights, latency), and
the right experiment for this branch; keeping 3D on main because of it is a
separate decision, which would change the manifest's target on purpose.

### The game: Signal Breach

Signal Siege seen from inside. You stand on the steel deck; star, plate and
ring enemies fly and strafe round you in 3D; pink bullets stream at you.
Signal Siege already has most of a shooter:

- a deterministic simulation (seeded, a checksum in its report);
- enemies that are abstract shapes, made in code: no Blender, no skinning;
- an autopilot that plays it (`--benchmark`, headless runs, agents);
- scale: up to 100,000 enemies and 8,192 shots.

**The floor stays flat in the rules, as in Doom.** The level is a grid of
cells, each with a floor height (ramps, platforms, a raised centre) and
walls between some of them; the player moves and collides against that grid
in the game's own code. Enemies fly in full 3D; shots are rays and moving
spheres against enemy spheres. That is a real 3D shooter with no 3D physics
engine, which is the dependency held back longest.

### The second consumer: kin-render-studio

Its 3D view (`app/src/model_view.*`) is a CPU rasterizer on its own thread:
the kiln's 5.15 million triangles, flat shaded in a key and a fill light, a shadow
on the ground, the map's hexes, 2x2 supersampling once still, the part and
the point under the pointer. It needs what the shooter doesn't (picking,
wireframe, a still picture kept), and it is the check that the renderer
draws what Blender's export says: replace `ModelRaster`, get the same
picture, faster.

### Not in scope

3D physics, navigation meshes, terrain, level of detail, a deferred
renderer, skinned meshes, PBR, an editor. Each waits for a game that needs
it.

## Choices

| Concern | Choice | Why |
|---|---|---|
| Graphics API | SDL_GPU (already on Vulkan) | The manifest planned "Vulkan" for 3D; kin already forces SDL_GPU's Vulkan driver. It has depth buffers, MSAA, instancing, storage buffers and compute: enough. Its limits (no timestamp queries, no host-visible vertex buffers) are known and already lived with. The manifest's table should say so. |
| SDL_Renderer backend | No 3D (`capabilities().rendering_3d` stays false) | It has no depth buffer. A view there is cleared to its background and a warning logged once. |
| Math | Kin's own header, `kin/core/math3d.hpp` | `Vec3f`, `Vec4f`, `Quatf`, `Mat4f`, projections: a few hundred lines in `Affine2`'s style. glm is large and would be the only foreign type in the public API. |
| Conventions | Right-handed, +Y up, -Z forward, metres; column-major matrices as GLSL reads them | glTF's, so imported models need no conversion. The studio's Blender Z-up export is turned once at load. Signal Siege's 2D plane (x right, y down, pixels) maps to x, z at 1/64 m a pixel: a 64-pixel cell is a metre. |
| Depth | Reverse Z, D32_FLOAT (D24 where unsupported) | Precision held out to the far plane, which a shooter's long sight lines need. Formats checked with `SDL_GPUTextureSupportsFormat`. |
| Colour | Lighting in linear; HDR views in a linear HDR scene | Glowing shots need light above white for bloom. A view's target has the 2D renderer's render-target format, so it is laid in on the existing premultiplied path; in a Gamma scene the fragment shader encodes instead. |
| Model files | glTF 2.0 (`.glb`, `.gltf`) through cgltf, from M3 | A single MIT header, like stb and dr_libs. Not needed until the gun and props; the enemies are made in code. |
| API style | Immediate: a draw list filled each frame | As Renderer2D and RenderQueue are. Meshes and textures are made once and kept on the GPU; transforms and per-draw data are uploaded each frame, instances in storage buffers. |
| Where | `engine/include/kin/render3d/`; the game in `examples/signal_breach` | Built behind `KIN_EXPERIMENTAL_3D` (on in this branch, off on main until it is decided). The docs say the API may change in any release. |

## The API, as Signal Breach would use it

```cpp
#include <kin/render3d/renderer3d.hpp>

kin::Renderer3D r3d{renderer};               // r3d.available(): false on SDL_Renderer
kin::Mesh star = r3d.create_mesh(kin::meshes::star_prism(5, 0.4f, 0.2f, 0.15f));
kin::Mesh shot = r3d.create_mesh(kin::meshes::sphere(0.08f, 8));
kin::View3D view = r3d.create_view(renderer.output_size(), {.msaa = 4, .hdr = true});

// each frame
kin::Scene3D scene;
scene.camera = kin::Camera3D::perspective(eye, yaw, pitch, /*fov*/ 90.0f, 0.05f, 200.0f);
scene.lights.sun = {.direction = sun_dir, .color = {1.0f, 0.95f, 0.9f}, .shadows = true};
scene.lights.ambient = {0.08f, 0.09f, 0.12f};
scene.add_point_light({.position = muzzle, .color = {4, 3, 2}, .radius = 3});
scene.draw_instanced(star, star_transforms, {.base_color = red, .emissive = 0.6f});
scene.draw_instanced(shot, shot_transforms, {.base_color = pink, .emissive = 4.0f, .unlit = true});
scene.draw_particles(sparks, {.blend = kin::BlendMode::Additive});  // depth tested, not written
scene.overlay.camera = gun_camera;           // the gun: its own projection, depth cleared
scene.overlay.draw(gun, gun_transform);
r3d.render(view, scene);                     // recorded into the frame here, in order

renderer.draw_texture(view.texture(), screen_rect);
const kin::Vec2f bar = scene.camera.project(enemy_top, view, screen_rect); // 2D draw coordinates
```

`r3d.render` works as `dispatch_compute` already does: it flushes the 2D
draws recorded so far, records its passes (shadow, scene, overlay, bloom)
into the view's targets, and later 2D draws sample the result in the same
command buffer. 3D and 2D come out in the order the calls are made, with no
second frame loop. Bloom runs on the view's HDR texture with the existing
`pp_bloom_*` shaders, before the HUD is drawn, so the HUD doesn't bloom.

The seam is one virtual on the backend, `IRenderer2DBackend::renderer_3d()`,
which returns an `IRenderer3DBackend*` (null by default). The SDL_GPU backend
returns a `GpuRenderer3D` that shares its `GpuDevice` (uploads, staging,
texture pool, retention until submit), its pipeline cache (with a key that
grows depth test/write, cull mode and the mesh layouts) and its frame.

## Milestones

Each ends with something measured or shown. The game is built in the stages
of M1-M3; the studio comes after.

### M0 — Groundwork (nothing visible). Done.

What was built: `kin/core/math3d.hpp` and its tests (`kin-math3d`); the
pipeline cache keyed by a `GpuPipelineKey` with depth test, culling, sample
count and wireframe (the 2D calls unchanged); `IRenderer2DBackend::renderer_3d()`
and `kin::Renderer3D`, reporting the depth format and the most MSAA samples;
the `KIN_EXPERIMENTAL_3D` option; `kin-render3d` tests, also on the lavapipe
CI job. Nothing draws with depth until M1's mesh shader.

What was measured (RTX 4080 Laptop, Linux, Vulkan, Release; 2026-10-08):

- Devices: the RTX 4080 and lavapipe both give 32-bit float depth and 8
  samples (8-bit and HDR colour).
- 2D unchanged: `kin_draw_bench` against 0.2.6, six rounds each, interleaved,
  and a build with the option off: all record times within about 3%
  (draw_texture 0.52 / 0.48 / 0.51 ms min base / new / option off; fill_rect
  0.39 / 0.40 / 0.40; draw_sprites 0.088 / 0.087 / 0.087; cached shapes
  0.37 / 0.34 / 0.33); pipelines' first frames 0.44 / 0.44 / 0.46 ms.
- **Signal Siege baseline** (`KIN_RENDER_BACKEND=gpu signal_siege --headless
  --benchmark --seed=7 --mute --ui-scale 1 --width 1280 --height 800
  --enemies N --profile --frames=1200`, four runs, the same with 0.2.6):

  | Enemies | frame median / p99 | render.scene median | gpu.frame median | update median | checksum |
  |---|---|---|---|---|---|
  | 300 | 0.17 / 3.2 ms | 0.095 ms | 0.16 ms | 0.036 ms | 8268756481707919801 |
  | 20,000 | 1.48-1.55 / 4.1-4.4 ms | 1.15 ms | 0.72 ms | 0.30 ms | 18442429486914611772 |

- **Studio baseline**: `ModelRaster::draw` (16 threads), min / median of
  five pictures turned 7° apart, after one to warm up:

  | Model | Triangles | 1280x800 | 1280x800, 2x2 | 2560x1600 | 2560x1600, 2x2 |
  |---|---|---|---|---|---|
  | arm_head | 1,452 | 2.5 / 2.8 ms | 9.0 / 11.2 | 8.5 / 9.6 | 30.3 / 31.0 |
  | plant | 244,628 | 13.2 / 15.0 | 26.6 / 27.7 | 31.0 / 32.1 | 69.5 / 77.5 |
  | kiln | 5,150,778 | 68.2 / 70.6 | 96.8 / 106.0 | 98.5 / 107.5 | 178.3 / 185.0 |

  The studio draws without supersampling while the model turns, so turning
  the kiln shows about 14 pictures a second at 1280x800.

The plan as it was:

- `math3d.hpp` with tests: products, inverses, quaternions, `look_at`,
  `perspective` (reverse Z, Vulkan clip space), `project`/`unproject`
  round trips.
- The pipeline cache key grows depth, cull and layout fields; the 2D
  pipelines are unchanged (`kin_draw_bench` before and after, min/median).
- The `renderer_3d()` seam, `rendering_3d` reported by SDL_GPU, the CMake
  option.
- **Baselines**: Signal Siege's `--benchmark --seed=7` frame times (median,
  p99) at its default enemies and at 20,000; the studio's
  `ModelRaster::draw` for the kiln at its usual sizes. Later milestones are
  measured against these.

### M1 — Signal Siege in 3D (stage A). Done.

What was built (the API: [3D rendering](rendering_3d.md)):

- `MeshData` (validated), `Mesh` (vertex and index buffers uploaded once, with
  the texture uploads), `kin::meshes`: box, plane, sphere, cylinder, cone,
  torus, star prism, octahedron; `MeshData::append` to build shapes from
  shapes. No submeshes yet: a mesh is drawn with one material.
- `View3D`: colour, depth (reverse Z), MSAA resolved into the texture 2D
  draws sample, premultiplied; follows the 2D pipeline's colour format (gamma,
  linear, linear HDR); resized in place. `Renderer3D::render` records the pass
  between the 2D draws around it, as `dispatch_compute` does.
- `Camera3D` (perspective, orthographic, `look_at`, `orbit`, `project`,
  `ray`), `hit_plane_y`, `Quatf::look_rotation`.
- `Scene3D`: draws merge into instanced draws when the mesh and material
  repeat; `add_instances` for filling in place. Instances (128 bytes: matrix,
  normals' matrix, linear colour) go straight into the upload memory, on the
  job system's workers above 8,192 (small draws taken together in tasks of
  about 4,096). One pipeline per colour format, sample count and culling.
- `mesh.vert` / `mesh.frag`: Lambert from a sun and ambient light, emissive,
  unlit, double-sided; colours decoded and lit in linear light.
- Without SDL_GPU (SDL_Renderer, headless runs): meshes are empty, a view is a
  render target showing its background, a warning is logged once.
- Signal Siege: `--view 3d` (or `--view=3d`), `examples/arena_3d.*`: the deck
  as 384 tiles and four walls, the three enemy kinds (a star prism, a
  hexagonal plate, a ring), the ship, shots, cores, sparks and impacts; a chase
  camera kept over the deck; the mouse aims through the camera at the plane
  the ships fly in; the 2D post chain (bloom, vignette, grade) and the HUD on
  top. The report gains `view`, `draws_3d`, `instances_3d`, `triangles_3d`.
  The title screen still draws its arena in 2D.

Found on the way: a 2D flush leaves the geometry batch without a target, so
2D drawing must be resumed after a pass recorded in the middle of a frame
(the first version lost every 2D draw after a 3D view); a mirroring transform
turns a mesh inside out (its normals are kept facing out; its faces need
double-sided); GPU buffers, unlike textures, held the raw device, so meshes
now hold the device's shared handle and may outlive the renderer (Signal
Siege's scenes do).

What was measured (RTX 4080 Laptop, Linux, Vulkan, Release; 2026-10-08):

- **Signal Siege, 2D and 3D from the same binary** (as M0's baseline command,
  `--view 2d|3d`, four runs each, interleaved; checksums identical to M0's in
  all of them):

  | Enemies | 2D frame median / p99 | 3D frame median / p99 | 2D GPU | 3D GPU |
  |---|---|---|---|---|
  | 300 | 0.17 / 3.0-3.2 ms | 0.14-0.15 / 3.1-3.4 ms | 0.16 ms | 0.29 ms |
  | 20,000 | 1.39-1.44 / 4.1-4.2 ms | 1.29-1.36 / 4.2-4.3 ms; 1.16 / 4.1 after the fix below (two runs) | 0.71 ms | 1.15 ms |

  At 20,000 enemies the 3D view's CPU time is the game filling its scene
  (0.53 ms; 0.78 before writing the enemies' matrices out instead of
  composing them from quaternions) and `render()` (0.32 ms). Its GPU time is
  4x MSAA over 1280x800 and every enemy drawn: no culling yet (M2).
- **`kin_draw_bench 200 N meshes`**, 1280x720 view, 4x MSAA:

  | N | Case | `render()` CPU, 1 thread / workers | GPU |
  |---|---|---|---|
  | 2,000 | boxes, one draw | 0.031 / 0.028 ms | 0.15 ms |
  | 20,000 | boxes, one draw (240k triangles) | 0.23 / 0.15-0.16 ms | 0.32-0.47 ms |
  | 20,000 | spheres, one draw (10.6M triangles) | 0.26 / 0.19-0.20 ms | 1.6-1.7 ms |
  | 20,000 | separate draws | 1.73-1.76 / 1.74-1.78 ms (about 85 ns a draw) | 1.0 ms |

- 2D unchanged against 0.2.6 (`kin_draw_bench`, four rounds interleaved):
  within about 3% everywhere (draw_texture 0.540 / 0.547 ms min, fill_rect
  0.398 / 0.407, draw_sprites 0.097 / 0.100, shapes immediate 2.16 / 2.15).
- Tests: `kin-render3d` (meshes face out, validation, scenes, cameras, the
  SDL fallback; on SDL_GPU: depth order with and without MSAA, lighting and
  emissive light, instances and tints, instances filled on workers, culling,
  2D before and after a view and a view rendered twice in a frame, linear HDR,
  meshes outliving the renderer), passing on the RTX 4080 and lavapipe.
  Signal Siege's own check (`kin-examples`: the ship where the camera puts it,
  aiming round trips, the checksum untouched) runs where SDL_GPU is and skips
  elsewhere: not on the lavapipe CI job, which builds without the examples
  (adding them there is left for after the `ci/faster` branch reshapes the
  jobs).

The plan as it was:

Signal Siege's simulation, unchanged, drawn in 3D from a chase camera behind
and above the player: `signal_siege --view=3d`. No game design, a ready-made
benchmark, and a screenshot test from the first week.

- `MeshData` (positions, normals, UVs, colours, indices, submeshes)
  validated; `Mesh`, static vertex and index buffers uploaded once.
  `kin::meshes`: box, sphere, cylinder, torus, star prism, plane, made in
  code.
- `View3D`: colour + depth, MSAA 1/2/4 resolved into the sampled texture,
  resized with the window; `render()` interleaved with 2D.
- `Camera3D`: perspective and orthographic, `look_at`, `orbit`, `project`
  to 2D coordinates, `ray` from a 2D point.
- Materials: **unlit** and **lit** (Lambert from a sun and ambient, plus an
  emissive term). Opaque only, sorted by pipeline then front to back.
- **Instancing**: `draw_instanced(mesh, transforms)`, per-instance
  transforms and colours in a storage buffer, filled on the job system for
  large batches (as `draw_sprites` is).
- The HUD and the reticle drawn over the view unchanged.
- Acceptance: the same checksum with `--view=3d` as without (drawing doesn't
  touch the simulation); frame times against the M0 baseline at both enemy
  counts; on the lavapipe CI job, a shot test at a small enemy count (the
  player where the camera puts it, the deck lit from the sun's side).
- Bench: `kin_draw_bench ... meshes` (N instances of a mesh in a view).

### M2 — First person (stage B). Done.

What was built:

- **Input**: `Input::mouse_motion()` (for `update()`, used up by a frame's
  first step) and `frame_mouse_motion()` (for `render()`), as the wheel has;
  `set_mouse_motion()`; `last_mouse_motion_event_time_ns()`;
  `Window::set_relative_mouse()` (SDL lets the mouse go while the window is
  out of focus). `Key::LeftShift`. The agent server's `input.mouse` takes
  `dx`/`dy`.
- **3D**: `Scene3D::overlay()`, drawn in the same pass with its own camera and
  the slice [0.9, 1] of the depth range (reverse Z: in front of the scene's
  [0, 0.9], depth-tested against itself) instead of a second pass;
  frustum culling of every copy (its mesh's bounding sphere, moved and scaled
  with it) in the pass that fills the instances, on the job system for big
  scenes, then only the visible copies written (`Scene3D::cull`,
  `Render3DStats::culled`); `meshes::wedge` for ramps; `kin/ecs/render3d.hpp`:
  `Transform3D`, `WorldTransform3D` (composed through the flecs hierarchy as
  2D's is), `MeshRenderer`, and `WorldRenderState3D` gathering them into
  instanced draws.
- **Signal Breach** (`examples/signal_breach`, `breach.*`, `breach_view.*`):
  its own simulation, with Signal Siege's three enemy kinds, colours, cores
  and 90-second run, and its HUD strings (a `breach` section added to the four
  language files). A 48 x 32 m deck of one-metre cells: floors at 0, 1 and 1.5
  m, walls 3 m, ramps between; the player walks, steps up to 0.45 m, jumps,
  dashes (Shift), falls off ledges, and fires along the view (rays against
  enemy spheres and the deck). Enemies are ECS entities (`Transform3D`,
  `BreachEnemy`, a `MeshRenderer` given by the view); they fly in 3D over the
  walls, not round them; plates and rings fire moving spheres when they see
  the player. Movement splits across threads above 16,384 enemies, firing and
  damage stay serial. The gun is the overlay (recoil, a bob, a muzzle flash);
  health bars over damaged plates and damage numbers are placed with
  `Camera3D::project`. Click captures the mouse; Esc pauses and lets it go,
  again quits; R restarts, B autoplay. The autopilot aims at the nearest
  enemy in sight (turning at most 360 degrees a second), walks a loop of
  waypoints, jumps when stuck; `--benchmark`, headless runs and the server
  use it.

What was measured (RTX 4080 Laptop, Linux, Vulkan, Release, 1280x800 at 4x
MSAA; 2026-10-08, with other sessions loading the machine, load average
about 4):

- **Determinism**: `--check-determinism` (baseline, repeat, one job worker)
  agrees on every frame: 1,200 frames at 300 enemies (314 entities, 900
  values a frame) and 600 frames at 20,000 (20,014 entities, the parallel
  movement path).
- **Frame times** (`signal_breach --headless --benchmark --seed=7 --enemies N
  --profile --profile-lines --frames=1200`, three runs, medians):

  | Enemies | frame / p99 | step | ECS gather | `render()` (culling incl.) | GPU | drawn / culled copies |
  |---|---|---|---|---|---|---|
  | 2,000 | 0.25 / 1.7-2.1 ms | 0.074 ms | 0.043 ms | 0.066 ms | 0.27-0.29 ms | 2,364 / 1,436 |
  | 20,000 | 1.61-1.73 / 2.2-2.5 ms | 0.44-0.46 ms | 0.50-0.59 ms | 0.41-0.43 ms | 0.39-0.45 ms | 14,492 / 14,622 |

  The ECS gather (propagating 20,000 transforms and copying them into the
  scene twice: buckets, then the draw) is the largest part at 20,000 and the
  first to look at.
- **Mouse to screen** (`signal_breach --benchmark --latency-probe --enemies
  2000`, windowed on a 240 Hz display; motion pushed as SDL events every 23
  ms from another thread, its event timestamp to the frame that turns the
  camera, and to the next frame's start, after `present`; the OS's input path
  and the display's scanout are not included):

  | | event to camera | event to presented | GPU frame |
  |---|---|---|---|
  | vsync off (frame 0.26 ms) | 0.3-1.2 ms (p95 1.3-1.6) | 1.6-2.6 ms (p95 2.6-3.1) | 0.30-0.40 ms |
  | vsync on (240 Hz) | 2.1-3.6 ms (p95 4-19) | 6.3-7.3 ms (p95 8-22) | — |

  Two of four vsync runs (and part of one without) stalled for seconds: with
  the window not on screen the compositor holds its frames. The medians are
  the useful numbers; the tails depend on the desktop.
- Tests: `kin-platform` (motion reaches one step, frame motion one frame),
  `kin-server` (`dx`/`dy`), `kin-render3d` (culling with and without workers,
  the overlay in front of a nearer wall, ECS transforms and gathering, the
  wedge faces out), `kin-examples` (Breach's deck heights, ramps, ledges,
  pillars, jumping, firing, determinism by seed), all passing on the RTX
  4080 and lavapipe.

Not done, or done differently:

- Signal Breach shares Signal Siege's design (kinds, colours, cores, run
  length, HUD strings), not its code: the two simulations are separate.
- Enemies fly through walls; only the player and shots collide with the deck.
- No title or results screen (a run ends with a banner, R starts another),
  no sound, no settings screen (`--sensitivity` sets the mouse).
- Gamepad aiming waits for kin's gamepad input.

The plan as it was:

`examples/signal_breach`: a new simulation beside Signal Siege's (which
stays as it is), sharing its enemies, spawning and scoring.

- **Engine input**: relative mouse mode (the cursor captured, raw motion
  read each frame, released on pause and focus loss), with a sensitivity
  setting. Kin's input has only absolute mouse positions today.
- The level: a cell grid with floor heights and walls, drawn as instanced
  boxes and ramps; the player walks, jumps and collides against it.
- Enemies fly in 3D (Signal Siege's patterns lifted off the floor); shots
  are rays (the player's) and moving spheres (theirs).
- **The gun**: an overlay pass in the same view, its own camera and field
  of view, depth cleared, so it never clips into walls.
- **Frustum culling** of instances and draws, on the job system.
- Health bars and damage numbers placed with `Camera3D::project`.
- **ECS**: `Transform3D`, `WorldTransform3D` (the flecs hierarchy, as 2D's)
  and `MeshRenderer`, since enemies are entities already; shots and sparks
  stay plain arrays, as in Signal Siege.
- **Autopilot** aims in 3D, so headless runs, the benchmark and agents still
  play whole matches.
- Acceptance: a seeded headless match gives the same checksum run to run
  and with one job worker; mouse to screen latency measured (input sampled
  late, the frame cap, GPU wait) and written down; frame times at 2,000 and
  20,000 enemies.

### M3 — What makes it a shooter (stage C). Done.

What was built:

- **Lighting**: `mesh.frag` lights in linear light with a sun (through its
  shadow map), ambient light and up to 16 point lights, plus emissive light;
  frame-wide uniforms pushed once a pass, a small block a draw. Point lights
  (`Scene3D::add_light`) are chosen each frame: those whose reach is in view,
  by strength over distance.
- **Shadows**: `Lights3D::shadow_area` (a box) and `shadow_resolution`: an
  orthographic depth map from the sun over the box (reverse Z), drawn by
  opaque casters (`Material3D::casts_shadow`); copies the camera culls but the
  sun sees are written after the visible ones and drawn only into the map.
  Receivers take 3x3 bilinear comparisons from a point pushed off the surface
  along its normal (no acne without a depth-bias pass). A 1 x 1 "all lit" map
  stands in when there are none.
- **Transparency**: `Blend3D::Alpha` (premultiplied over, drawn after the
  opaque, draws and copies furthest first) and `Additive` (premultiplied
  add, any order); both depth-tested, not writing depth.
- **HDR and bloom**: `View3DOptions::hdr`, `exposure`, `bloom`,
  `bloom_threshold`: the scene into a float target, a bright pass at half
  size, a 9-tap blur twice across and down (the second with taps twice as
  far), then `view_output.frag` adds the glow, exposes, tonemaps (ACES) and
  encodes into the 2D pipeline's format (linear for an HDR one). The glow is
  the view's, so the HUD over it stays crisp; an HDR view clears in linear.
- **Textures**: `Material3D::texture` (the base colour's), decoded in the
  shader where textures are stored encoded.
- **glTF**: cgltf 1.14 (MIT, a single header, fetched with
  `KIN_EXPERIMENTAL_3D`; in the third-party notices and the shipped
  licences). `Renderer3D::load_model` reads `.glb`/`.gltf` through the content
  folder or pack: primitives as meshes placed by their nodes, base colour and
  texture (mipmapped, or nearest when the sampler asks), emissive (and its
  strength), `BLEND`, double-sided, unlit; flat normals when a primitive has
  none. `ModelFile` reloads on change. `decode_image()` reads images from
  memory. `tools/models/glb.py` writes small .glb files with the standard
  library (the test model and Signal Breach's gun and crates are made by it).
- **3D sound**: `AudioEngine::set_listener_3d()`, `set_position_3d()`,
  `AudioPlayRequest::position_3d`, `calculate_spatial_audio_3d()`: pan by the
  direction from where the listener faces, less pan close up, up to a fifth
  quieter behind, rolloff by 3D distance. (New names rather than overloads:
  overloads made `set_listener({x, y})` ambiguous.)
- **Signal Breach**: sun shadows over the deck, muzzle-flash, explosion and
  enemy-shot lights, bloom on the view (the 2D post chain is gone), additive
  sparks and tracers, a glTF gun (brushed metal) and eight glTF crates (a
  metre high, solid, climbable by jumping), sound in 3D with Signal Siege's
  synthesized cues. `--no-shadows`, `--no-bloom`, `--no-lights`, `--mute`.
  The crates change the level, so its checksums differ from M2's.

What was measured (as M2; three runs each, medians; `signal_breach
--headless --benchmark --seed=7 --enemies N --profile --profile-lines
--frames=1200`, with each feature left off in turn):

| Enemies | Features | frame | GPU | `render()` CPU |
|---|---|---|---|---|
| 2,000 | all | 0.41-0.42 ms | 0.45-0.46 ms | 0.116-0.118 ms |
| 2,000 | no shadows | 0.36-0.37 | 0.34 | 0.071-0.078 |
| 2,000 | no bloom | 0.39-0.40 | 0.35 | 0.106-0.108 |
| 2,000 | no lights | 0.41-0.42 | 0.43 | 0.114-0.118 |
| 2,000 | none (as M2) | 0.31-0.34 | 0.19-0.20 | 0.061-0.063 |
| 20,000 | all | 1.82-2.03 | 1.13-1.28 | 0.74-0.83 |
| 20,000 | no shadows | 1.48-1.60 | 0.44-0.47 | 0.41-0.44 |
| 20,000 | no bloom | 1.84-1.94 | 1.04-1.13 | 0.74-0.78 |
| 20,000 | no lights | 1.82-1.93 | 1.00-1.03 | 0.74-0.78 |
| 20,000 | none (as M2) | 1.49-1.59 | 0.29 | 0.40-0.44 |

What each costs: at 2,000 enemies shadows about 0.11 ms of GPU and 0.04 ms
of CPU, bloom about 0.10 ms of GPU, point lights 0.02-0.03 ms; at 20,000,
shadows 0.7-0.8 ms of GPU and 0.33 ms of CPU (every enemy is in the sun's view
of the deck, so every one casts, and the extra copies are tested and
written), bloom 0.1-0.15 ms, lights 0.1-0.25 ms (more fragments to light).
Shadows at many casters were the thing to look at next (done after M4,
below).
GPU scopes per pass were not used (they split submissions and change what
they time); the cost of each pass is the difference with it off.

- Determinism, again: `--check-determinism` agrees at 300 enemies (1,200
  frames) and 20,000 (600 frames), repeat and one worker.
- Tests: `kin-render3d` (a point light on a wall and nothing past its
  radius, 16 of 21 lights chosen; a box's shadow on the floor where the sun
  puts it, none without an area; additive light adding up, alpha over a
  wall, transparent copies behind a wall hidden; bloom round an emissive
  cube and none without it, the HDR background not lifted, glow over a
  transparent background; a texture's texels; the glTF test model's parts,
  materials, node transforms, bounds, flat normals and texels, hot reload,
  a missing file), `kin-audio` (3D pan by facing, behind quieter, height as
  distance, close-up pan, the engine's channels, back to 2D). All pass on the
  RTX 4080 and lavapipe; with 3D off, 65 tests pass.

The plan as it was:

- **Bloom** on the view's HDR texture: glowing shots and enemies.
- **Point lights**: muzzle flashes and explosions, at most 16 a frame,
  forward shaded, chosen by distance and strength; no clustering.
- **Transparency**: sparks and explosion particles drawn additively, tested
  against depth but not writing it; alpha-blended draws sorted back to
  front. `render.overdraw` watched, since this is where it grows.
- **Shadows**: one directional shadow map fitted to the level, PCF filtered.
- **Positional audio**: the listener gets a facing direction and a height;
  emitters pan and roll off in 3D.
- **glTF** through cgltf, for the gun and the level's props, with
  base-colour textures (mipmapped) and hot reload through the asset system.
- Acceptance: frame times at both enemy counts against M2 with everything
  on, and each feature's cost alone (a GPU scope per pass); shot tests for a
  muzzle light on a wall and a shadow on the deck.

### M4 — Studio parity. Done.

What was built, in kin:

- **Picking**: `View3DOptions::pick` gives a view a second, single-sampled
  target (RGBA32F: the world point, and the id as the alpha's bits) drawn
  after the main pass by the scene's opaque and alpha meshes (`pick.frag`;
  not wireframes, additive glows or the overlay). The id is the draw's
  (`Scene3D::draw(..., id)`, `MeshInstance::id`), carried in a spare float
  of each copy's normal matrix. `Renderer3D::pick(view, pixel)` submits what
  the frame has recorded and reads one texel back
  (`GpuDevice::read_texture_region`); 2D goes on in a new command buffer.
  0.2-0.6 ms a pick on the RTX 4080, with the frame's work before it.
- **Wireframe**: `Material3D::wireframe`: line fill, unlit, depth-tested with
  a small depth bias (a pipeline key field), after the opaque draws; no
  shadow, no pick. A degenerate triangle (a, b, b) is a line: the studio's
  grid is drawn that way.
- **Light the studio needs**: `Lights3D::fill` (a second sun, no shadow),
  `ground_ambient` (hemisphere ambient: faces mix by how far they face up)
  and `Material3D::receives_shadow`.
- **Large meshes**: `create_mesh` writes vertices and indices straight into
  the upload memory as it converts them, on the job system when there are
  many, in 8 MB pieces (a 5-million-triangle mesh needs no 512 MB staging
  buffer).
- No highlight tint was needed in the engine: the studio's glow mixes the
  changed parts' material colours toward the highlight.

In kin-render-studio (branch `exp/3d-view`, built against this branch):
`GpuModelView` (app/src/model_gpu_view.*) draws `ModelRaster`'s picture with
kin's 3D: a mesh per part per material (Blender's z up turned to kin's y
up, each triangle wound to face its normal), the rig's key as the sun
(1.15 x its colour, casting onto the ground only), its fill (0.22), the sky's
light from above and below (0.42, 0.18), emitting materials unlit, the
ground lit in the colour that comes out as the raster's unlit ground, the
hex grid as wireframe lines, the model's edges (the Wire button) as a wireframe, the
glow as a colour mix. It answers `part_at`, `point_at` and `screen_of` like
the raster, the last pixel's pick kept until the picture changes. The view
draws again only when what is asked changes, always at the screen's pixels
with 4x MSAA (the CPU drew at half size while turning). Without
`KIN_EXPERIMENTAL_3D`, or on SDL_Renderer, or with `KRS_CPU_MODEL_VIEW=1`,
the CPU raster draws as before. `krs_model_view_check` draws a model both
ways and compares pictures, times and picks.

What was measured (`krs_model_view_check`, 1280x800, the raster at 2x2
against 4x MSAA, the first of six orbits 7 degrees apart; RTX 4080 Laptop;
the machine was busier than at M0, so the raster is timed again beside it):

| Model | Triangles | CPU raster now, 1x / 2x2 (min / median ms) | M0 baseline 1x / 2x2 | GPU view, drawn and waited for | `render()` CPU |
|---|---|---|---|---|---|
| arm_head | 1,452 | 6.6 / 7.4, 20.7 / 21.9 | 2.5 / 2.8, 9.0 / 11.2 | 0.76-0.83 / 0.88-0.91 | 0.20 / 0.22 |
| plant | 244,628 | 35.2 / 36.7, 70.1 / 72.1 | 13.2 / 15.0, 26.6 / 27.7 | 1.30 / 1.43 | 0.65 / 0.82 |
| kiln | 5,150,778 | 156 / 188, 222 / 263 | 68.2 / 70.6, 96.8 / 106.0 | 6.0-7.0 / 7.4-8.3 | 0.6 / 0.8-1.1 |

- **The pictures**: per colour channel, the mean difference is 0.21 levels
  (kiln), 0.23 (plant), 0.27 (arm_head); median 0, p95 0-1, p99 6. Pixels
  more than 8 levels apart: 0.90%, 0.85%, 0.53%; more than 24: 0.28%,
  0.37%, 0.16%. What differs: edges (MSAA's samples are not the raster's
  2x2), the grid's one-pixel lines a fraction of a pixel apart, the shadow
  a few levels lighter (the GPU lights the ground without the sun; the
  raster darkens its colour by 0.58), soft shadow edges (3x3 PCF against the
  raster's hard ones), and on the kiln a plank whose coplanar faces the two
  break the other way (the raster keeps the first drawn, the GPU the last).
  Wire mode: 0.54 levels mean (the GPU's edges a pixel wide, the raster's
  half that).
- **Picking**: of 4,000 pixels across the picture, the same part, ground or
  nothing at 99.97-100% against the raster drawn without supersampling (it
  answers for a pixel's middle, as the GPU does); on the model 99.81% (kiln,
  one pixel of 515), 100% (plant, arm_head). Against the 2x2 raster, which
  answers for the sample right and below the middle, 97.9-99.5% on the
  model. Points: 0.8-6 mm apart on models 2-4 m across. `screen_of` agrees
  to a hundredth of a pixel.
- **Loading**: the kiln's 15.5 million vertices (556 MB; page faults on
  new memory are most of the cost) are made on a thread of their own and
  uploaded in chunks of 65,536 triangles, 4 ms of them a frame: 0.7-0.9 s
  to the first picture over about 220 frames, none of them longer than
  13 ms except the one that first draws it (17-31 ms; 43-142 ms the first
  time a process draws in 3D, its pipelines and targets made). Meanwhile
  the model there before is drawn (a new export of the same asset), or a
  busy state (another asset). The chunks add about 80 draws: the kiln
  draws in 8.3 ms where it took 7.4.
- lavapipe draws the same picture (arm_head: 0.29 levels mean, the same
  picks) at 35 ms.

- **The studio itself**, driven through kin's agent server
  (`KIN_RENDER_BACKEND=gpu kin_render_studio --server --render`) on a copy
  of XC-121's project with its exports cached: the 3D tab draws arm_head
  and the kiln on the GPU, a hover names the part under the pointer
  (picked), Wire draws the edges. Selecting the kiln, fresh each time, gave
  no frame over 100 ms in three runs (two on the GPU, one on the CPU
  raster); a frame of the studio's UI takes about 13 ms in that mode,
  whichever tab, the kept 3D picture adding nothing.

### After M4: shadow cost. Done.

Measured first, with `kin_draw_bench 200 20000 shadows` (Signal Breach's
deck, tiles and enemy meshes; first person from one end, from mid-deck, and
from above; `KIN_BENCH_REPEAT=8` renders the view eight times a frame so the
3D work outweighs the timer's noise). The map pass cost about as much as
half the main pass: every caster again, both faces, through `mesh.vert`.
Sampling it was not the cost (`KIN_BENCH_NO_RECEIVE`: the same), nor mostly
its size (1024: 0.5 ms, 2048: 0.65) or the triangles (a torus a quarter as
fine: 0.53): it is per copy.

- **Back faces culled** in the shadow pass for single-sided materials: a
  closed mesh's back faces are behind its front ones from the sun too, so
  the map is the same for half the triangles. 0.84-1.0 ms became
  0.57-0.70 (per render, from above). A lone single-sided quad facing away
  from the sun no longer casts (`double_sided` casts both ways; a test
  checks both).
- **shadow.vert**: only the placing (no normals, colours or varyings). Within
  the noise (0.03-0.05 ms): kept, as it costs nothing.
- **Casters whose shadow can't reach the view are skipped**: a copy the
  camera culls is drawn into the map only if its bounding sphere, swept
  down the light to where it leaves the shadow area, reaches the camera's
  frustum. From mid-deck 21,536 casters became 8,778 and the shadows'
  GPU cost 0.85 ms became 0.25; fewer copies are written too.
  `Render3DStats::shadow_casters` counts them.
- **The map is not cycled**: a frame that renders several views was handed
  a new map for each (7.3 ms a render at 4096 repeated, 2.2 without); at
  one render a frame the same either way (an interleaved A/B, five runs).

Signal Breach (`--headless --benchmark --seed=7 --frames=1200`, three runs
each, the M3 binary and this one interleaved; the machine was slower than
at M3, so each is compared with its own `--no-shadows`):

| Enemies | Shadows' GPU cost, M3 / now | Shadows' render() CPU cost, M3 / now |
|---|---|---|
| 2,000 | 0.17-0.22 / 0.02-0.06 ms | 0.09-0.26 / 0.03-0.06 ms |
| 20,000 | 0.78-0.86 / 0.07-0.26 ms | 0.58-0.88 / 0.28-0.48 ms |

The pictures are the same to the bit (frames 600 and 1800 of the
benchmark, both binaries).

## Advanced graphics (stage D)

Past a shooter that looks like one: materials and custom shaders, sharper
and steadier pictures, meshes that adapt to the screen, and ray tracing.
SDL_GPU (3.4) has vertex, fragment and compute shaders, indirect draws and
dispatches; no ray-tracing pipelines, no tessellation, geometry or mesh
shaders. Ray tracing and adaptive meshes are built on compute, which runs on
every SDL_GPU backend.

Every change in this stage is judged on the same scenes, before and after,
for how it looks and how fast it is: G0 first.

### G0 — Reference scenes. Done.

Scenes that each stress one thing, rendered from fixed cameras at a fixed
size, timed, and compared: against the picture before a change (what it
changed), and against a ground truth, Blender's Cycles rendering the same
scene (how far kin is from it).

The scenes (`tests/reference/scenes/*.json`: models, cameras, lights, the
sky, the view's size and samples, what each is for):

| Scene | From | For |
|---|---|---|
| `cornell` | made here: the Cornell box's published geometry | light bouncing, an area light, soft shadows (G1 lights, G4) |
| `spheres` | made here: a 5 x 5 grid of spheres, metal by roughness, on a floor | materials (G1); a white-furnace variant |
| `shadows` | made here: poles, thin plates, steps, objects touching the ground, a low sun | acne, peter-panning, aliasing, contact (G2) |
| `helmet` | Khronos glTF samples: FlightHelmet (CC0) | textured materials, normal maps (G1) |
| `sponza` | Khronos glTF samples: Sponza (Crytek's licence: fetched, never committed) | an interior: shadows, many materials, bounced light (G2, G4) |
| `crowd` | made in code: Signal Breach's deck and 20,000 instanced enemies | instancing, shadows, GPU culling (G3) |
| `kiln` | local only: kin-render-studio's export, 5.15 million triangles | dense meshes and LOD (G3); traced against Cycles (G4) |

The tools (`tools/reference/`):

- `fetch.py`: the downloaded scenes, pinned to a commit of
  KhronosGroup/glTF-Sample-Assets and checked by SHA-256, into a cache
  (`KIN_REFERENCE_CACHE`, by default `~/.cache/kin/reference`); the made
  ones written there too (`make_scenes.py`, with `tools/models/glb.py`).
- `kin_reference` (C++): renders scenes' cameras to PNGs and a JSON of
  times (`render()` on the CPU, the GPU's time with the view rendered N
  times a frame so it outweighs the timer's noise, the first frame, the
  load) and what was drawn.
- `blender_truth.py`: the same scenes and cameras in Cycles (lights mapped
  to Blender's units by a calibration render), Standard view transform, kept
  by the scene's hash and Blender's version.
- `compare.py`: a pair of pictures: mean difference a channel, PSNR, SSIM,
  p99, pixels more than 8 levels apart, a difference image; times side by
  side; an HTML report.
- `run.py`: renders with a build into `out/<label>`, compares two labels,
  or a label against the truth.

Acceptance: every scene renders on the RTX 4080 and on lavapipe; the same
binary twice gives the same pictures; the timing noise of each scene is
measured (what a later change must beat to count); the truth exists for
the scenes Cycles can render; the report for kin as it is now is the
baseline the rest of the stage is measured against.

What was built, as above, plus `studio_mesh_to_glb.py` (the kiln from the
studio's export, numpy). The downloads are pinned to glTF-Sample-Assets
edc7c9e and checked against `tools/reference/assets.lock.json`. Cycles'
lights are kin's mapped: a white plane under a sun of kin colour 0.5 shows
0.7333 in Cycles and 0.7354 in kin (the same level, stored). Sponza's light
was chosen so that its galleries are not black in Cycles (the sun down the
courtyard's length, a brighter sky); its ground-floor aisle stays near
black there at any light kin's other scenes use, so it has no camera.

**Found on the way, fixed**: a hidden or minimized window gets no swapchain
texture, so SDL never held the CPU back, and frames (with the render
targets each cycles) piled up without bound when the GPU was the slower:
8-12 GB of GPU memory after a few seconds of the crowd rendered 8 times a
frame, then `VK_ERROR_DEVICE_LOST` loading the kiln. A frame that shows
nothing now queues an empty command buffer with a fence after it, and the
CPU waits for the oldest when more than two are pending (not the frame's
own fence, which the GPU timer may own; fences released only once
signalled). Peak GPU memory 1.1-1.75 GB on the same runs. This is the 2D
backend's, not 3D's: it belongs on main too.

The baseline (`run.py render g0 --frames 120`, RTX 4080, 1280 x 720, 4x
MSAA, the view rendered 8 times a frame for the GPU's time):

| Scene / camera | render() CPU ms | GPU ms | against Cycles: mean levels, PSNR dB, SSIM, pixels over 8 |
|---|---|---|---|
| cornell/front | 0.013 | 0.058 | 10.2, 22.6, 0.879, 45% |
| furnace/front | 0.010 | 0.039 | 0.2, 48.6, 0.998, 0.2% |
| spheres/front | 0.017 | 0.115 | 5.7, 24.9, 0.967, 24% |
| spheres/grazing | 0.017 | 0.108 | 5.8, 24.7, 0.964, 23% |
| shadows/overview | 0.026 | 0.075 | 2.5, 34.6, 0.990, 8% |
| shadows/contact | 0.015 | 0.077 | 5.2, 29.1, 0.971, 23% |
| shadows/fence | 0.023 | 0.090 | 4.1, 31.4, 0.975, 24% |
| shadows/grazing | 0.014 | 0.095 | 6.7, 28.9, 0.990, 50% |
| helmet/front | 0.011 | 0.146 | 2.1, 27.3, 0.973, 9% |
| helmet/side | 0.012 | 0.153 | 2.0, 29.8, 0.968, 12% |
| sponza/nave | 0.065 | 0.250 | 43.4, 14.5, 0.492, 98% |
| sponza/roof | 0.049 | 0.231 | 28.9, 16.9, 0.652, 91% |
| kiln/game | 0.026 | 5.37 | 0.8, 38.0, 0.994, 4% |
| kiln/close | 0.026 | 6.09 | 2.5, 33.1, 0.981, 16% |
| crowd/first_person | 0.48-1.07 | 1.24 | (speed only) |
| crowd/mid_deck | 0.40-0.47 | 0.62-0.66 | |
| crowd/above | 0.47-0.70 | 1.31-1.34 | |

What the distances are: the Cornell box lacks bounced light and the area
light's soft shadows; the spheres lack reflections and metal (kin's
materials are matte); the shadows differ in their edges (Cycles' sun is
sharp, kin filters 3 x 3) and in the sky light kin does not let the scene
hide; the helmet in its normal and roughness maps; Sponza most of all: kin's
ambient reaches everywhere, Cycles' sky only through the roof, and its
alpha-cut plants are black in kin. The furnace passes only because kin's
materials are matte (Cycles' own let a little through at the extremes).

The noise: the same binary twice gives the same pictures to the bit but
for a few pixels of kiln/close (a level apart: coplanar faces); the GPU's
times agree within about 5% on scenes over 0.2 ms and up to 30% under
0.1 ms; render() on the CPU varies twice over on the crowd (its workers).
A change counts where it moves a median by more than either run's p10-p90
spread (`compare.py` marks it `*`). On lavapipe every scene renders (the
kiln at 4.4-7.7 s a frame) and its pictures are within 0.5 levels of the
4080's (p99 at most 3).

### G1 — Materials and custom shaders. G1a and G1b done.

glTF's metallic-roughness materials with normal, occlusion and emissive
maps; physical light falloff; image-based light (an HDR environment,
prefiltered in compute: diffuse and specular); custom 3D material shaders
(a fragment shader with kin's lighting as an include) and passes of one's
own on an HDR view. Checks: the white furnace, the sphere grid and the
helmet against Cycles.

#### G1a — results

Physically based materials. Built: `Material3D` gains glTF's
metallic-roughness (`metallic`, `roughness`, `metallic_roughness`, `normal`
and `normal_scale`, `occlusion` and `occlusion_strength`, `emissive_texture`)
and `Blend3D::Mask` with `alpha_cutoff`; the loader reads all of it. The
data maps are made as plain RGBA8 (`Renderer2D::create_texture`), colours
as before (sRGB, decoded in the sampler or the shader), so a data map is
never gamma-decoded. `mesh.frag` is Lambert plus Cook-Torrance (GGX, Smith
height-correlated visibility, Schlick), for the sun with its shadow, the fill
and point lights; F0 = mix(0.04, base, metallic), the diffuse of a
dielectric taking what its Fresnel leaves; roughness floored at 0.045.
Ambient light (still the hemisphere: the environment map is G1b) is seen
along the normal for the diffuse and along the reflection, turned toward the
normal by the roughness, times Karis' fit of the split-sum integral for the
specular, with Fdez-Aguera's multiple-scattering term (without it a rough
white metal lost a third of its light in the furnace) and the same
compensation on the direct specular. Normal maps take their tangent frame
from the derivatives of world position and UV (no tangents in the vertex
format); the frame turns with the face for back faces. Point lights are
inverse-square, windowed `(1 - (d/r)^4)^2`, floored at 1 cm: `color` is the
light at one unit. Masks discard in `mesh.frag`, and have their own pick
and shadow fragment shaders (`pick_mask`, `shadow_mask` with a vertex shader
that carries UV and alpha): other materials keep the depth-only shadow pass.
Masks are drawn with the opaque.

Layouts: `FrameUniforms` gains `eye` (the view vector); `MaterialUniforms`
is three vec4: (emissive, unlit, receives shadow, cutoff), (metallic,
roughness, normal scale, occlusion strength), (has normal map, has emissive
map). The fragment shader has six samplers: base, shadow, metallic-roughness,
normal, occlusion, emissive; a material without a map binds the 1 x 1 white
texture (the factors are then the values; the two flags switch the normal
and emissive maps off) and the five are bound again only when a draw's
textures differ from the last's. Behaviour changed: every surface now has the
specular of rough plastic (`roughness` 1: about 4% of the light, spread), so
existing scenes got a little brighter and shinier; the existing tests'
expectations held unchanged. Point-light colours are not what they were
(Signal Breach's flashes were scaled up by 6.4, 4 and 1.4 times for
their usual distances, the Cornell lamp down to 0.14 times; the flashes
look as bright at about half their radius).

Measured (RTX 4080, 1280 x 720, 4x MSAA, 120 frames, g0 the baseline above):

| Scene / camera | GPU ms g0 | GPU ms g1a | against Cycles, mean levels, g0 | g1a | SSIM g0 | g1a |
|---|---|---|---|---|---|---|
| cornell/front | 0.059 | 0.078 | 10.2 | 10.2 | 0.879 | 0.887 |
| furnace/front | 0.039 | 0.040 | 0.23 | 0.23 | 0.998 | 0.998 |
| spheres/front | 0.074 | 0.116 | 5.7 | 3.6 | 0.967 | 0.980 |
| spheres/grazing | 0.080 | 0.096 | 5.8 | 4.1 | 0.964 | 0.971 |
| helmet/front | 0.113 | 0.15-0.31 | 2.1 | 0.66 | 0.973 | 0.989 |
| helmet/side | 0.100 | 0.153 | 2.0 | 0.68 | 0.968 | 0.989 |
| shadows/overview | 0.060 | 0.059 | 2.5 | 2.4 | 0.990 | 0.990 |
| shadows/contact | 0.072 | 0.059-0.079 | 5.2 | 4.8 | 0.971 | 0.971 |
| shadows/fence | 0.075 | 0.094 | 4.1 | 3.5 | 0.975 | 0.976 |
| shadows/grazing | 0.059 | 0.092 | 6.7 | 6.9 | 0.990 | 0.990 |
| sponza/nave | 0.196 | 0.325 | 43.4 | 49.4 | 0.492 | 0.472 |
| sponza/roof | 0.172 | 0.275 | 28.9 | 33.1 | 0.652 | 0.631 |
| kiln/game | 4.17 | 4.31 | 0.82 | 0.67 | 0.994 | 0.995 |
| kiln/close | 4.65 | 4.65 | 2.5 | 1.55 | 0.981 | 0.987 |
| crowd/first_person | 1.00 | 1.12 | | | | |
| crowd/mid_deck | 0.52 | 0.56-0.59 | | | | |
| crowd/above | 1.05 | 1.33 | | | | |

The small scenes' GPU times scatter by 20-30% between identical runs (the
same binary's repeat: helmet/front 0.31 then 0.42, sponza/nave 0.33 then
0.75), so the table says: the shading costs about 0.03-0.1 ms on the
scenes that fill the screen with materials (helmet, spheres, Sponza), 10-25%
on the crowd, and nothing measurable on the kiln (vertex bound). The pictures
of the same binary twice are identical but for a level on the kiln.

Quality: the helmet moves from 2.0 to 0.7 mean levels from Cycles (SSIM .97
to .99: its metal, leather and normal maps are now read), the spheres from
5.7 to 3.6-4.1 (a metal reflects the sky, the highlight sharpens with
roughness; what is left is the reflections of the other spheres and the
floor, and the sun's disc), and the furnace stays within 0.23 level (with
the multiple-scattering term: without it the rough metals lost 3.3 levels).
Sponza got further from Cycles (43 to 49): its plants are now cut out
rather than black, which is right, but more of its light is added to
a scene whose truth is dark (kin's ambient reaches every room, Cycles' sky
only the roof). Its textures still look washed: **found, not fixed**, glTF's
sampler wrap modes (repeat, mirrored) are not read, so the floor, the walls
and the ceiling, whose UVs run past 1, clamp; that is for the next change in
this stage. The Cornell lamp (an inverse-square point under the ceiling)
lights the ceiling above it brightly, as a point does.

#### G1b — results

Built: glTF sampler wrap modes; image-based light; custom 3D material shaders.

- **Wrap modes.** `Material3D::wrap_u/wrap_v` (Clamp by default for
  hand-made materials, as before; models set them from the glTF sampler, Repeat
  when none). Samplers are made on first use by filter x wrap and cached; a
  clamped material still uses the 2D backend's, so nothing else moved. This was
  Sponza's washed floor, walls and ceiling: they tile now. Against Cycles
  Sponza's mean distance did not improve (49.4 to 50.1 levels on `nave`, 33.1
  to 34.0 on `roof`; SSIM .472 to .501 and .631 to .641): what is left is its
  light (kin's ambient reaches every room), not its textures.
- **Image-based light.** `Lights3D::environment` (an `Environment3D` from
  `Renderer3D::load_environment` for Radiance `.hdr`, decoded by stb_image's HDR
  reader built as C with only that format, or `create_environment` from floats),
  `environment_intensity`, `environment_rotation`. On the first render that uses
  it, compute (`env_filter.comp`) makes: a specular equirect chain (up to 1024 x
  512, 7 levels a roughness, 256 GGX importance samples each, read from the
  picture's own mips by the sample's solid angle, so no speckle), and a 64 x 32
  cosine-weighted irradiance map (512 samples); half floats. `mesh.frag` reads
  them in place of the hemisphere (`ambient` is then unused), with the existing
  analytic split-sum fit and multiple-scattering term. Equirect, not a cube:
  seams are bilinear across u (repeat) and v clamps at the poles (a cube would
  be exact there); SH9 was not used because the 64 x 32 irradiance map is as
  cheap and keeps coloured sunsets. No background drawing (not done: the view's
  colour shows). Orientation: the picture's centre is +x, the top +y, right turns
  toward +z, the same as Blender's world texture in kin's axes (checked by a 32
  sample Cycles render of `ibl_spheres` into `reference_out/truth_check`: the
  highlights and the cloud reflections sit in the same places). Reference scenes
  `ibl_spheres` and `ibl_helmet` (Poly Haven's
  `kloofendal_48d_partly_cloudy_puresky_1k.hdr`, CC0, pinned by URL and SHA-256
  in `fetch.py`), and `blender_truth.py` takes `lights.environment` as the world.
  Against that quick truth_check, `ibl_spheres` is 9.5 and 10.8 mean levels
  away (SSIM .94, p99 120-135): Cycles has shadows from the sky (kin's
  environment light is unoccluded) and reflects the other spheres; the real
  truth is pending a render at 512 samples.
- **Custom shaders.** `Material3D::shader` (a `MaterialShader3D`, from
  `create_material_shader(spirv)` or `compile_material_shader(glsl)`) and
  `shader_params` (four vec4s as `kin_param(i)`). The 2D contract is followed
  where it fits (SPIR-V blobs, a uniform block, the pipeline cache keyed on the
  fragment shader); the difference is that a 3D shader brings the whole
  fragment stage and `mesh.frag`'s code is now `kin_material.inc.glsl`, which
  both it and custom shaders `#include` (glslc `-I`, added by `kin_compile_glsl`,
  `kin_spirv.cmake` and the runtime compiler; `.inc.glsl` files are not shaders).
  The built-in shader is the same code through the same functions: the 120-frame
  reference renders without an environment are bit-identical to `g1a_final`
  (helmet: 80-90 dB, from the wrap fix). Passes of one's own on an HDR view
  were left out: nothing there is cheap (a view's targets are the backend's;
  2D shaders can already process `view.texture()`). Example: Signal Breach's
  hit shield (`examples/shaders/shield.frag.glsl`).

Measured (RTX 4080, 1280 x 720, 4x MSAA, 120 frames, best of three runs of
`g1b` against `g1a_final`, a busy machine: other sessions compiled while it
ran, so the runs scatter 30-100%): cornell 0.078 to 0.077 ms; crowd 1.16, 0.59,
1.35 to 1.10, 0.61, 1.37; helmet side 0.157 to 0.153; kiln 4.30, 4.69 to 4.20,
4.58; spheres front 0.079 to 0.116, grazing 0.114 to 0.093; shadows within
0.84-1.03; sponza 0.314, 0.289 to 0.367, 0.308 (+7 to 17%: tiling textures
are read at more of their area, mips and all). Pictures of scenes without
an environment: identical to `g1a_final`. The environment scenes cost a
filter the first frame (not timed separately: a few compute dispatches of 256 to
512 samples a texel over 1024 x 512 and smaller) and nothing after.

### G2 — Sharper, steadier pictures

Cascaded shadow maps, ambient occlusion (GTAO), temporal anti-aliasing
(motion vectors), contact shadows. Checks: the shadow scene and Sponza
against before, and their costs.

### G3 — Adaptive meshes, GPU-driven drawing

Culling in compute (frustum, and occlusion against the last frame's depth)
into indirect draws; meshes split into clusters of about 128 triangles with
simpler versions of each (meshoptimizer), chosen each frame by their error
on screen, crack-free. Checks: the kiln and the crowd, against full detail
and against before.

### G4 — Ray tracing in compute

A bounding-volume hierarchy (built on the CPU, then on the GPU for what
moves) and ray queries in compute; a progressive path tracer (the studio's
Trace view, converging toward Cycles); ray-traced shadows, occlusion and
reflections for games, at reduced rate and accumulated. Checks: Cornell,
Sponza and the kiln against Cycles by samples; rays a second.

### Later, each on request

Gamepad aiming (unless kin's gamepad input lands first, then in M2);
skinned meshes and glTF animation; metallic-roughness materials and an
environment light; a 3D physics engine (Jolt), only if the cell grid stops
being enough; agent interface additions (`render.3d` stats, `view.pick`
over the server) once an agent needs them.

## How it is tested

- **State**: the simulation's checksum is the test surface, as Signal
  Siege's is; seeded headless matches, run twice alike and once with one job
  worker.
- **Math and data**: ordinary unit tests, everywhere.
- **Pixels**: GPU rasterization is not bit-identical across drivers. Shot
  tests run on the lavapipe CI job at small enemy counts and 256² to 640x360
  views, compare with a tolerance, and test what matters (where things are,
  which side is lit, that a light reaches a wall), not exact images.
  Locally, repeat a doubtful shot several times before blaming the code.
- **Speed**: `signal_siege --benchmark --view=3d` and the Signal Breach
  benchmark, A/B with separately built binaries, min/median. Check
  `nvidia-smi` for other jobs on the GPU before trusting GPU timings.
- **Headless runs**: 3D needs `KIN_RENDER_BACKEND=gpu` (which keeps a real
  video driver headless). Without it the game still runs and its state is
  still checked; views stay empty.

## Risks

- **Scope.** A shooter pulls hard toward a full 3D engine. The milestones
  stop where the game is playable and looks like a shooter; anything past
  M4 waits for a request, and nothing in 2D may get slower (bench before
  merging anything back).
- **Latency and pacing.** A shooter shows a dropped frame or a frame of
  input lag that a card game hides. Measured in M2, not assumed.
- **Many small draws.** The 2D work found per-call cost, not per-byte cost,
  dominates. Everything repeated is instanced from the start; draws are
  counted per frame and kept in the dozens, not thousands.
- **Overdraw.** Particles, bloom and the gun overlay add full-screen work;
  `render.overdraw` and GPU scopes per pass show where.
- **Two coordinate systems.** 2D is Y-down in draw units, 3D Y-up in metres.
  The bridge is explicit: `Camera3D::project`/`ray` take and return 2D draw
  coordinates of the rectangle the view is laid into. The 2D transform stack
  does not reach inside a view; it moves the laid-in texture.
- **Device differences.** Depth formats, MSAA counts and line fill mode
  vary; each is checked at start and degrades (D24, fewer samples, no
  wireframe) with a log line, never a failure.
- **Lavapipe speed.** CI shot tests stay small.

## Decisions wanted

1. **Stage A as a flag on Signal Siege** (`--view=3d`, recommended: same
   binary, same benchmark, same checksum) or as the first build of Signal
   Breach.
2. **Gamepad**: wait for kin's own gamepad input, or add aiming with one in
   M2.
3. **Whether the studio's parity (M4) stays in this branch** or follows it.
