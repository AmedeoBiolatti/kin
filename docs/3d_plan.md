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

Four steps, each judged on the G0 scenes against the one before (`g1b_final`
first, then `g2a`, `g2b`, ...), for how it looks and what it costs. Every
feature is off by default and the picture is then the same to the bit.
The renderer is forward with MSAA and one sun map, with no depth or normal
pre-pass: AO, contact shadows and TAA each need one.

#### G2a — Cascaded shadow maps. Done.

`Lights3D::shadow_cascades` (1: today's single map, unchanged) and
`shadow_distance`. 3 to 4 orthographic cascades, split by a log/linear
blend of near and far, each fitted to its slice of the view frustum and
snapped to texels so edges don't shimmer. One depth array (a layer a
cascade), one shadow pass a layer. `mesh.frag` picks the cascade by view
depth, blends over the last tenth of each range, and filters 3 x 3; the bias
scales with the cascade. `shadow_area` stays the limit of what casts and
receives. Caster skipping and back-face culling stay, run per cascade.
Checks: `shadows` and `sponza` against `truth`; GPU cost per cascade (the
risk is casters drawn again for each).

#### G2a — results

Built: `Lights3D::shadow_cascades` (1 to 4; 1 is the single map and the
default) and `shadow_distance` (100 by default, clamped to the camera's far
plane); `kin/render3d/shadow_cascades.hpp`, the splits and the fit as pure
functions (`cascade_splits`, `fit_shadow_cascades`); a depth array of
`shadow_cascades` layers (`GpuDevice::create_target_array`), one shadow pass
a layer; `mesh.frag` picks, blends and filters.

- **Splits.** 0.6 of the logarithmic split and 0.4 of the linear one. With a
  near of 0.05 and 100 units that is 10, 21, 39, 100: the log split alone gave
  the first cascade 0.3 units, which wastes a whole map on the gun.
- **Fit.** Each cascade's box is the bounding sphere of its slice (on the
  view's axis, equally far from the near and far corners; the radius is
  rounded up to 1/16), so its size does not change as the camera turns, with
  its centre snapped to whole texels in the sun's space (a space fixed by the
  area, so a grid in it stays put in the world). The unit test moves and turns
  the camera and checks every probe point's texel position shifts by a whole
  number and the scale is the same. Where the area is smaller than the
  sphere on an axis the box is the area itself (also fixed): small levels get
  the single map's resolution in every cascade, as they should. The depth
  range is the whole area's, so whatever is between the slice and the sun casts.
- **Blend.** Each cascade fades into the next over the last tenth of its
  range, so the next cascade's slice starts there (the slices overlap by
  that). Where one's map doesn't hold the point (its box stops at the area's
  edge) the other's answer is used; with neither, lit.
- **Bias.** The depth bias and the normal offset are per cascade, as the
  single map's were (a texel's worth of depth, one and a half off the
  surface), from that cascade's texel.
- **Casters per cascade.** The caster test of the single map (the sphere in
  the map's volume, its shadow swept down the light into the view) is run
  per cascade, against the cascade's own volume and its slice of the view. A
  seen copy that isn't in a cascade's volume isn't drawn into it. Each
  cascade has a list of the instance indices it takes (a 4-byte entry,
  not another copy of the 128 B instance); `shadow.vert` and
  `shadow_mask.vert` read through it when `Draw::first.y` is not 0, and run as
  before (one run from `first.x`) when it is. Back-face culling and cut-out
  casters work as before. `Render3DStats::shadow_cascades` and
  `cascade_casters[4]` count them.
- **Always an array.** `uShadow` is a `sampler2DArrayShadow` for either path
  (one layer is the single map; it samples the same). If the device can't make
  a 2D array of the depth format there are no shadows at all, as when it could
  not make a sampled depth (none known; Vulkan, D3D12 and Metal all can). If
  it can make fewer layers than asked it makes fewer, with one line in the
  log, and doesn't ask again.
- **Reference scenes.** `lights.shadow_cascades` and `lights.shadow_distance`
  in a scene's JSON, and `kin_reference --cascades N --shadow-distance D` to set them for
  a whole run (a label of its own: the default scenes are unchanged, so older
  labels stay comparable): `run.py render g2a_c4 --scenes shadows,sponza --cascades 4`.

Checks (RTX 4080, 1280 x 720, 4x MSAA, 120 frames, `kin_reference`):

*Picture with one cascade against `g1b_final`:* identical in every scene
(mean 0.00, PSNR "identical") but `kiln` (72 to 80 dB, and the same between two
runs of this build: its noise, not the change).

*Distance to Cycles, mean levels (lower is better):*

| Picture | `g1b_final` / `g2a` (1 cascade) | `g2a_c4` (4 cascades) |
|---|---|---|
| shadows/overview | 2.38 | 2.38 |
| shadows/contact | 4.75 | 4.70 |
| shadows/fence | 3.53 | 3.44 |
| shadows/grazing | 6.86 | 6.86 |
| sponza/nave | 50.10 | 50.07 |
| sponza/roof | 33.98 | 33.89 |

That is the expected answer: both scenes have small areas (16 m, 30 m), where
2048 texels are already 1 cm, and the cascades fall back to the area's box. The
remaining distance to Cycles is not shadow resolution (the truth's sun is a
point too; what is left on `shadows` was not looked into, on Sponza it is the
ambient light). Cascades
pay where the area is much bigger than what is near. A scratch scene (not
committed: `shadows` with the area at 300 x 300 m) shows it: with one map the
shadows near the camera are a smear of about 15 cm texels, with four they are
as sharp as in the 16 m scene.

*Sharpness far from the camera (unit test `test_gpu_cascades`, 300 m area, a
block 60 m away seen by a 12 degree lens):* the shadow edge is 3 pixels wide
with one map, 1 with four (lavapipe and the RTX 4080 alike).

*Casters drawn (`Render3DStats`; copies counted once for each cascade they are
drawn into):*

| Picture | one map | 4 cascades, per cascade | 4 cascades, total |
|---|---|---|---|
| shadows/overview | 45 | 28 / 45 / 2 / 0 | 75 |
| shadows/contact | 20 | 20 / 2 / 1 / 0 | 23 |
| sponza/nave | 103 | 82 / 78 / 16 / 0 | 176 |
| sponza/roof | 68 | 68 / 36 / 2 / 0 | 106 |
| crowd/first_person | 18,671 | 2,889 / 6,203 / 9,258 / 4,575 | 22,925 |
| crowd/mid_deck | 8,778 | 2,807 / 6,216 / 2,337 / 0 | 11,360 |
| crowd/above | 21,536 | 0 / 0 / 5,792 / 20,369 | 26,161 |

(the crowd's last cascade covers the rest of the view; a cascade with no
casters costs its clear and nothing else). Copies are drawn again where a
cascade's volume overlaps the next one's, 1.2 to 1.7 times in all: the risk
the plan named, and less than it feared because each cascade only takes what
reaches its slice.

*GPU time, ms per render* (`compare.py`'s `*` only where the medians move by
more than the p10-p90 spread; the machine was busy, load 4 to 11, other sessions
compiling, so the tiny scenes scatter 30%):

| Picture | `g1b_final`, 1 cascade (`g2a`) | 4 cascades |
|---|---|---|
| shadows/overview | 0.075, 0.077 | 0.122 `*` |
| shadows/contact | 0.077, 0.093 | 0.117 |
| sponza/nave | 0.317, 0.328 | 0.463 `*` |
| sponza/roof | 0.300, 0.288 | 0.442 `*` |
| crowd/first_person | 1.07, 1.09 | 1.32 `*` |
| crowd/mid_deck | 0.58, 0.67 | 0.72 |
| crowd/above | 1.29, 1.33 | 1.89 `*` |

Four cascades cost 1.4 to 1.6 times a render of Sponza (+0.14 ms) and 1.2 to
1.4 times of the crowd (+0.2 to 0.5 ms): a pass a cascade (clear, state,
the casters) and nine taps for each of the two maps a pixel in a blend
reads. Three would cost a little less; that is the knob.

*The single-map path's own cost,* the commit before against this one,
interleaved, three runs each (the machine busy, load 11): crowd, Sponza and
`spheres` the same within the noise (minimum of three: 1.076/1.078, 0.674/0.668,
1.327/1.332, 0.326/0.327, 0.285/0.290 ms); the small `shadows` pictures
0.083-0.107 to 0.110-0.133 ms (+0.02 to 0.03 ms, one more storage buffer in
the shadow vertex shaders, an array texture and a branch on `first.y`;
`compare.py` does not mark it, and it is within the scatter of those runs;
not confirmed as real).

*Pictures.* `shadows` and `sponza` with four cascades look the same as with
one: no seam at the cascade borders, no acne, in the pictures looked at
(`overview`, `nave`, and crops of the 300 m scratch scene's `contact` and
`fence`). Not tested: shimmer in a moving sequence (the snapping is covered by
the unit test, not by a moving capture), and cascade borders on thin
geometry seen at a grazing angle. One artefact: with a fine cascade the
3 x 3 filter covers less ground, so the texel stair-steps of an edge that
isn't along the map's axes show where a picture magnifies the shadow more than
about 4 times (a close-up `contact` of a 1 cm texel map at 2.6 mm a pixel
has steps; the single map, with the same texel, shows softer ones). Smoothing
that wants a wider or rotated kernel for the nearest cascade, or a PCSS-like
one: left.

Signal Breach (`--headless --benchmark --seed=7 --ui-scale 1 --enemies 2000
--frames=1800 --screenshot ... --capture-frame 1800`): the frame is the same
kind as before (deck, enemies' shadows, gun, HUD); its settings were not
changed (`shadow_cascades` stays 1), and the `crowd` reference scene, which is
the same deck and enemies, is identical to `g1b_final`.

#### G2b — Pre-pass and ambient occlusion (GTAO). Done.

A depth and view-normal pre-pass (vertex-only, normals from the mesh;
resolved to one sample under MSAA). GTAO in compute at half or full
resolution (2 slices, 8 to 12 steps, a thickness heuristic), a depth-aware
blur, an R8 texture. `mesh.frag` multiplies ambient and environment diffuse
by it (with the multi-bounce approximation) and environment reflections by a
specular-occlusion term; the sun and point lights are not touched.
`Scene3D::ambient_occlusion {enabled, radius, intensity}`. Checks:
`ibl_spheres`, `sponza`, `cornell`; `furnace` must not darken with it off.
If the pre-pass costs too much in Signal Breach, AO becomes opt-in per scene.

#### G2b — results

Built: `Scene3D::ambient_occlusion` (`AmbientOcclusion3D {enabled = false,
radius = 0.5, intensity = 1, half_resolution = false}`), a depth and normal
pre-pass, GTAO and a blur in compute, and the lighting changes in
`kin_material.inc.glsl`.

- **Pre-pass.** `prepass.vert` (mesh.vert's placing and the world normal, from
  the same instance buffer and the same culled copies as the main pass: no
  extra upload), `prepass.frag` (octahedral normal in RG16F, the side the
  viewer sees) and `prepass_mask.frag` (the cut-out's alpha test). One sample
  into its own D32 depth and normal targets whatever the view's MSAA, so
  "resolve" costs nothing. The main pass is **not** changed to depth-equal:
  its depth is multisampled and the pre-pass's is not, so it cannot reuse it,
  and the off path stays untouched.
- **GTAO.** `gtao.comp`: 2 slices, 8 steps a side with squared spacing, noise
  per pixel for the angle and the offsets, distance falloff over the outer
  0.615 of the radius, a thickness heuristic (a sample under the horizon pulls
  it down 15% of the way), reach capped at 12% of the view's height in pixels;
  `ao_blur.comp` twice (across, down), 7 taps, weighted by depth difference.
  Three R8 targets per view, made by the first render that asks (never when
  off), full size or half. Needs a sampled D32 (or D24), RG16F as a colour
  target and R8 as a storage texture; else one INFO line and no occlusion.
  Checked on lavapipe too.
- **Lighting.** Ambient and environment diffuse times the multi-bounce term,
  environment reflections times Lagarde's specular occlusion; sun, fill and
  point lights untouched (a test: with the ambient light at zero a floor at
  the foot of a wall is the same with and without). The glTF occlusion map is
  separate and still multiplies both. The AO branch returns early in
  `kin_lit`, so the arithmetic without it is the old arithmetic.
- **Overlay.** Not occluded and not in the pre-pass (its frame's occlusion flag
  is 0): the world's occlusion at a pixel says nothing about a gun in front
  of it; the gun's own lighting is as before. Blended, additive, wireframe
  meshes are not occluded either (`maps.z`).
- **Reference scenes.** `ambient_occlusion {enabled, radius, intensity,
  half_resolution}` in the scene JSON; `kin_reference --ao [--ao-radius R
  --ao-intensity I --ao-half]` for a whole run (labels of their own).
- **Counters.** `Render3DStats::ao_draws`, `ao_instances`, `ao_dispatches`. No
  GPU scope inside `render()` (a scope splits the frame's command buffer);
  cost is read as the difference between runs with it on and off.

*AO off against `g1b_final`:* every picture identical but `kiln`
(72.7 and 80.2 dB, its own jitter). (An earlier shader draft that applied the
terms in one expression was not identical in 7 pictures at 106-112 dB, from
fused multiply-adds: hence the early return.) Signal Breach frame 1800 with it
off is byte-identical to `g2a_breach_1800.png`.

*Distance to Cycles, mean levels (lower is better), defaults (radius 0.5):*

| Picture | off (`g2b`) | `g2b_ao` | `g2b_ao_half` |
|---|---|---|---|
| ibl_spheres front / grazing | 9.49 / 10.81 | 9.80 / 10.89 | 9.80 / 10.88 |
| sponza nave / roof | 50.10 / 33.98 | 48.67 / 32.13 | 48.68 / 32.15 |
| cornell | 10.20 | 10.20 | 10.20 |
| spheres front / grazing | 3.60 / 4.05 | 3.62 / 4.01 | 3.61 / 4.01 |
| helmet front / side | 0.66 / 0.68 | 0.67 / 0.70 | 0.67 / 0.70 |
| ibl_helmet front / side | 1.38 / 1.48 | 1.45 / 1.62 | 1.46 / 1.62 |
| kiln close / game | 1.55 / 0.67 | 1.06 / 0.49 | 1.08 / 0.50 |
| shadows overview / contact / fence / grazing | 2.38 / 4.75 / 3.53 / 6.86 | 2.51 / 5.10 / 4.02 / 7.05 | 2.52 / 5.11 / 3.98 / 7.05 |
| furnace | 0.23 | 0.22 | 0.21 |

Honest reading: occlusion alone does not bring kin nearer Cycles in most
scenes. It helps Sponza and the kiln; it makes `ibl_spheres`, `shadows` and
`ibl_helmet` a little worse, because Cycles also has what kin lacks and AO
only takes away: bounced light (the spheres' undersides are lit by the white
floor in Cycles, dark with AO), the sky's sun shadows and a lit floor. Cornell
has no ambient light (a point light), so nothing changes. Furnace did not get
darker (0.23 to 0.22/0.21): a lone sphere on its plane has almost no occlusion
at radius 0.5; at radius 2 it reached 0.85 (the sphere's own ground), so
a large radius in a furnace does darken, as the geometry really occludes.
Radius is the knob: Sponza with `--ao-radius 4 --ao-intensity 1` is 46.99 /
28.85, `4 / 2` is 44.42 / 25.18, `8 / 2` is 38.80 / 20.63 (and the picture then
reads like Cycles: recesses and arches dark; no label kept for these, they are
`ao_s_*` in `reference_out`).

*GPU cost, ms per render* (RTX 4080 Laptop, 1280 x 720, 4x MSAA, 120 frames,
machine busy so tiny scenes scatter 20-30%; all `*` in compare.py):

| Picture | off | AO full | AO half |
|---|---|---|---|
| cornell | 0.077 | 0.32 | 0.17 |
| spheres front | 0.098 | 0.48 | 0.29 |
| ibl_spheres front | 0.077 | 0.41 | 0.16 |
| shadows overview / contact | 0.075 / 0.077 | 0.35 / 0.61 | 0.22 / 0.31 |
| helmet front | 0.152 | 0.27 | 0.22 |
| sponza nave / roof | 0.317 / 0.300 | 0.87 / 0.80 | 0.60 / 0.52 |
| kiln close / game | 4.78 / 4.27 | 7.08 / 6.19 | 6.54 / 6.10 |
| crowd first_person / mid_deck / above (three runs) | 1.11-1.36 / 0.70-0.79 / 1.49-1.61 | 2.18-2.30 / 1.52-1.64 / 2.28-2.29 | 1.90-1.97 / 1.19 / 2.21 |

Where it goes (skipping stages, same machine): the pre-pass is cheap in small
scenes (+0.01 to 0.05 ms) and +0.33 ms in the crowd (18,671 copies drawn a
second time); the blurs are about 0.1 ms; GTAO itself is 0.2-0.45 ms and
grows when a surface is near the camera (a larger reach in pixels; the cap
at 12% of the height keeps it bounded). It is bound by depth taps (about 32 a
pixel), not arithmetic: unprojecting by the inverse's few terms instead of a
matrix, `cycle = false` on the storage writes and a smaller reach cap each
bought 10-30%; fewer steps (4 against 10) bought 0.08 ms.

**Too expensive for the crowd as a default:** Signal Breach's deck costs about
+0.9 to 1.0 ms with full AO, which roughly doubles its 3D GPU time; half
resolution still adds +0.5 to 0.8 (the pre-pass of every enemy is a third of
it). The plan's rule applies: AO is opt-in per scene, off by default, and
Signal Breach does not turn it on (the game has no setting; its frame was
captured with a temporary override, reverted). A depth mip chain for far taps
and drawing only large occluders into the pre-pass are the next savings; not
done.

*Pictures looked at* (`reference_out/g2b*`, `tmp_ao/*`): ibl_spheres, sponza
nave and roof (default, and radius 8 / intensity 2), cornell, the Signal
Breach frame off and on (forced, radius 1). Off: unchanged. On: soft
darkening under the spheres, in Sponza's arch and column creases (radius 8
gives deep recesses like Cycles'); in Breach only the wall's foot darkens a
little and the pink shots and enemies are unaffected. No light leaking, no
banding or visible noise after the blur at 1280 x 720, and no halo round the
thin rods on Sponza's roof. Known limits, seen or by construction: at a large
radius a near column darkens the far wall right up to its silhouette (a hard
edge, as every screen-space AO has); a multisampled edge pixel reads the AO of
the pixel's centre, so a silhouette can carry the neighbour's occlusion
(not seen as a visible fringe); half resolution is bilinear at depth edges
(slight bleed, not measured); anything off screen or outside the depth
buffer occludes nothing; double-sided thin geometry uses the side seen. Not
tested: moving captures (the noise is fixed per pixel, so it does not shimmer
in a still view; not checked in motion), a first-person scene with AO on in
Breach, orthographic cameras (the shader has the branch; not looked at).

Tests: `test_gpu_ambient_occlusion` (RTX 4080 and lavapipe): off equals
default and runs no passes; the wall's foot, 0.2 away, is darker with it and
open floor and the high wall are not; it fades away from the crease;
intensity darkens more; half resolution and 4x MSAA also darken; a sunlit
floor with no ambient light is unchanged and with some never gets less than
the sun's; blended meshes and the overlay are not in the pre-pass and the
overlay's pixels are unchanged. No unit test of pure maths: the maths is in
GLSL (the GPU test covers it); C++ only builds uniforms.

#### G2c — Contact shadows. Done.

A screen-space march toward the sun through the pre-pass depth (16 to 24
steps, about half a metre, a thickness test, per-pixel noise), multiplying
the sun term only. Fixes the contacts the cascades can't resolve. Check:
`shadows` (poles, plates, objects on the ground).

#### G2c — results

Built: `Scene3D::contact_shadows` (`ContactShadows3D {enabled = false,
distance = 0.5, thickness = 0.1, intensity = 1, steps = 20}`), `contact.comp`,
a few lines in `kin_material.inc.glsl` and the renderer.

- **Where it marches: compute into an R8 texture, not in `mesh.frag`.** The
  main pass shades every fragment of the overdraw (Sponza, the crowd), and a
  20-tap march in each would multiply by it; compute runs once a pixel, and
  `mesh.frag` stays one texture read behind a flag. The cost is three
  full-size R8 targets a view and three dispatches, made only when asked for.
- **Shared pre-pass.** It is drawn when AO or contact shadows (or both) are on
  and never twice; the depth and normal targets are the same. Contact shadows
  work with AO off (a test: 2 pre-pass draws, 0 AO dispatches, 3 contact
  dispatches). `Render3DStats::contact_dispatches` is new; `ao_draws` and
  `ao_instances` are now the pre-pass's, whoever asked for it.
- **The march.** From the pixel's view-space position, pushed off the surface
  by a pixel or more (more where seen at a slant), toward the sun in view
  space, `steps` samples over `distance`, the first one a different fraction
  of a step in every pixel (interleaved gradient noise). A sample counts when the
  depth there is nearer the camera than the ray by more than a tolerance of
  about a pixel's worth of depth, and less than `thickness`, with the weight
  falling off over the top quarter of the thickness, over the last 40% of the
  march and over the outer 5% of the screen. Off the screen ends the march. The
  result goes through `ao_blur.comp` twice (depth-aware, reused as it is). The
  tolerance is in pixels, not units, so a test at 160 x 120 and a game at 4K
  see the same march.
- **Lighting.** `kin_lit` multiplies the sun's shadow-map term by it (only
  there: fill, ambient, environment and point lights are as before); a surface
  with `receives_shadow` off, a blended, additive or wireframe mesh and the
  overlay read 1. The off path does not read the texture (it is white).
  Custom shaders and `mesh.frag` have a tenth sampler; shaders compiled
  against the old include (nine) still work.
- **Degrading.** The same formats as AO (sampled depth, RG16F target, R8
  storage); without them one WARN line when asked for and no shadows. With no
  sun colour it does nothing. Lavapipe is checked.
- **Reference tool.** `contact_shadows {enabled, distance, thickness,
  intensity, steps}` in a scene file; `kin_reference --contact-shadows
  [--contact-distance D --contact-steps N --contact-thickness T]`. Sponza Walk
  has the section (enabled, distance, thickness, intensity, steps), the preset
  `contact`, and the stats line.

*Off against `g1b_final`* (`g2c`): every picture identical but `kiln/game`
(90.5 dB, its own jitter, as in G2b). Signal Breach frame 1800 with it off is
byte-identical to `g2b_breach_off.png`.

*Distance to Cycles, mean levels (lower is better):*

| Picture | off (`g2c`) | on (`g2c_cs`) | AO on | AO + contact | 4 cascades | 4 cascades + contact |
|---|---|---|---|---|---|---|
| shadows overview | 2.38 | 2.41 | 2.51 | 2.54 | 2.38 | 2.41 |
| shadows contact | 4.75 | 4.75 | 5.10 | 5.12 | 4.70 | 4.71 |
| shadows fence | 3.53 | 3.60 | 4.02 | 4.12 | 3.44 | 3.52 |
| shadows grazing | 6.86 | 6.86 | 7.05 | 7.05 | 6.86 | 6.86 |
| spheres front / grazing | 3.60 / 4.05 | 3.63 / 4.06 | | | | |
| sponza nave / roof | 50.10 / 33.98 | 50.09 / 33.96 | 48.67 / 32.13 | 48.66 / 32.12 | 50.07 / 33.89 | 50.06 / 33.88 |
| helmet front / side | 0.66 / 0.68 | 0.67 / 0.70 | | | | |
| kiln close / game | 1.54 / 0.67 | 1.57 / 0.68 | | | | |
| cornell | 10.20 | 10.20 | | | | |
| ibl_spheres front / grazing | 9.49 / 10.81 | 9.49 / 10.81 | | | | |
| furnace | 0.23 | 0.23 | | | | |

Honest reading: **contact shadows do not bring kin nearer Cycles in any scene
here.** The `shadows` scene's 2048 map (and 4 cascades even more) already
resolves its poles and plates at 1280 x 720; what the screen-space march adds is
a little darkening that Cycles does not have (below), so the numbers move by
0.0 to 0.1 the wrong way. Cornell, `ibl_spheres` and furnace have no sun (a point
light, or an environment only) and are bit-identical: the feature does nothing
there, by design. The feature earns its place where a map is coarse (a big
`shadow_area`, one cascade over a large scene), which these scenes are not;
not measured on such a scene.

*GPU cost, ms per render* (RTX 4080 Laptop, 1280 x 720, 4x MSAA, 120 frames;
machine busy, tiny scenes scatter 20-30%; `*` where compare.py marks it):

| Picture | off | contact | contact + AO against AO alone |
|---|---|---|---|
| shadows overview / contact | 0.058 / 0.095 | 0.28 * / 0.35 * | +0.24 / +0.29 (0.33 -> 0.57, 0.56 -> 0.85) * |
| shadows fence / grazing | 0.098 / 0.114 | 0.38 * / 0.34 * | +0.27 / +0.21 * |
| spheres front / grazing | 0.129 / 0.115 | 0.36 * / 0.33 * | |
| helmet front / side | 0.154 / 0.172 | 0.20 / 0.25 * | |
| sponza nave / roof | 0.327 / 0.324 | 0.60 * / 0.53 | +0.25 / +0.26 * |
| kiln close / game | 4.63 / 4.20 | 6.73 * / 6.17 * | |
| cornell / ibl_spheres / furnace | | no sun: no cost | |
| crowd first_person / mid_deck / above (best of three, GPU) | 1.35 / 0.80 / 1.60 | 1.95 / 1.20 / 2.25 | with AO already on: 2.31 -> 2.67, 2.04 -> 2.20, 2.31 -> 2.62 |

The cost is mostly fixed, not the march: on `shadows` overview and Sponza,
8 / 16 / 24 steps were +0.15 / +0.17 / +0.21 and +0.15 / +0.20 / +0.22 ms
over off (the pre-pass, the three dispatches' barriers and the two full-size
blurs are the rest). With AO already on the pre-pass is shared and contact
shadows add 0.2 to 0.3 ms in small scenes and 0.2 to 0.4 in the crowd; alone in
the crowd they add 0.4 to 0.65 ms (the pre-pass of 18,671 copies is a third of
it). Like AO, **not cheap enough for Signal Breach by default**: it stays off
there (the game has no setting). Not done: a half-resolution march, one blur
instead of two, writing the march into the AO's scratch targets.

*Pictures looked at* (`reference_out/g2c*`, the Breach frames
`g2c_breach_off.png`, `g2c_breach_cs_0.5.png`, `g2c_breach_cs_1.5.png`; the
last two were made with a temporary environment override in the renderer,
reverted and grepped for): shadows contact and fence (before, after, Cycles,
the difference), Sponza nave, spheres front, a Breach frame. What it does: a
darker, sharper line where a plate meets the floor and at the foot of the
spheres (a thin arc at the contact), the foot of a wall in Breach a little
darker; no change away from contacts (a test: the far floor is equal within 3
levels). Pixel by pixel in Breach it only ever darkens (3,352 pixels at
distance 0.5; none lighter). Seen and not good: (1) **false shadows from
thin things beside the receiver**: on the fence the narrow lit side face of
each post is darkened (a post's neighbour is within the thickness in screen
space though not in the way in 3D; thickness 0.05 keeps most of the stripe; 0.08 and
0.15 darken it as well, and 0.1, the default, was not looked at at that crop); (2) a plate floating above the floor
extends its shadow strip a few pixels past where the map ends (the march passes
under the plate in screen space, within the thickness); (3) at the sphere's
foot the darkening is a thin arc of a pixel or two. Not seen: banding (the
blur hides the jitter at 1280 x 720), visible noise, light leaking at
contacts, and screen-edge artefacts (the fade stops them; a thin shadow's
blocker just off screen is missed by construction, not looked at). Not
tested: moving captures (the noise is fixed a pixel, so a still view does not
shimmer; motion not checked), orthographic cameras (the shader has the
branch), MSAA edges beyond the one test (a multisampled edge pixel reads its
centre's factor), half-resolution views, the Sponza Walk panel's click path
(headless only: `--preset contact`, `--set`, `--list-settings` and
`--print-settings` were run).

Tests: `test_gpu_contact_shadows` (RTX 4080 and lavapipe): off equals
default with other settings changed and runs no pre-pass; a 0.2 m post's foot
patch is darker (537 -> 372 summed levels, at 160 x 120) while the far floor,
the post's top and a face turned from the sun are equal within 3; the patch
fades with distance from the post and with intensity; a march too short to
reach the post changes nothing; a plate a metre and a half up that covers the
march's samples on the screen blocks nothing (checked by making the thickness
50, which then darkens the floor); with AO on the pre-pass is drawn once and
both run (3 and 3 dispatches); 4x MSAA; the overlay is unchanged; with no sun,
no passes.

#### G2d — Temporal anti-aliasing. Done.

Halton (2,3) jitter of the projection, 8 samples; a velocity buffer from
the previous frame's transforms, kept in a separate per-instance buffer
(not in the 128 B `GpuInstance3D`, so the path without TAA is untouched;
instances matched by draw and index, the unmatched moving by the camera
alone); a compute resolve with an RGBA16F history, reprojection, YCoCg
neighbourhood clamp with variance clipping, and a lower blend on
disocclusion. TAA replaces MSAA when on. Bloom and post effects run after the
resolve; the 2D overlay is not jittered; `pick` uses the unjittered
matrices; the first-person overlay has its own velocity or is left out;
camera cuts reset the history. Checks: a still camera converges to the
no-TAA picture within noise; no trails behind moving enemies in a Signal
Breach capture; a new reference scene with a moving object. Of the four, the
likeliest to be cut if the cost or the fiddliness is too much.

#### G2d — results

Built: `Scene3D::taa` (`TemporalAA3D {enabled = false, blend = 0.9, sharpness = 0,
jitter_scale = 1, jitter_samples = 8, freeze_jitter = false}`), `View3D::reset_history()`,
`kin/render3d/taa.hpp` (`taa_jitter`, `jitter_projection`: pure), the velocity
pre-pass (`prepass_velocity.*`), `taa.comp`, `Render3DStats::{taa_dispatches,
taa_reset, taa_jitter, samples}`; docs/rendering_3d.md has the whole design. The
decisions, short:

- **Setting on the scene, history on the view.** Like occlusion, so it switches live
  without making the view again; the history (two RGBA16F, a one-sample depth, the
  last frame's velocity, for an 8-bit view a float picture before the resolve) is the
  view's, made by the first frame that asks and dropped by the first that doesn't.
- **Velocity in the pre-pass, not an extra target of the main pass.** The pre-pass
  exists already for occlusion and contact shadows (shared, one draw), custom
  material shaders have one colour output (a second would be garbage), and the main
  pass's pipelines stay as they were. The normal and the velocity share one RGBA16F
  target, so no multiple targets in the pipeline cache. Cost: a second draw of the
  opaque meshes when nothing else asked for the pre-pass (kiln: +1.3 ms).
- **Previous transforms in a buffer of their own** (64 B a copy, same slots as the
  instances), with a "no past" flag in a zero of the matrix: the 128 B instance and
  the shadow and pre-pass instance data are untouched.
- **Matching** by id, else draw order when the count agrees, else the nearest copy last
  frame, else new (limits in rendering_3d.md). Tests lose and gain a copy of a crowd
  every frame and still keep their history (sabotaged: halves it).
- **Overlay jittered with the rest** and given its own velocity pass and previous camera
  (sabotaged: the gun's edge error is five times higher).
- **MSAA is replaced** (one sample a pixel; a 4x view with TAA equals a 1x one).
- **Off** is the old renderer: every picture of `g2d` equals `g1b_final` (kiln's own
  noise aside), Signal Breach frame 1800 is byte-identical to `g2c_breach_off.png`, and
  a same-session A/B of the off path against the previous commit's build shows the
  same GPU times within noise.

What went wrong on the way, because it shaped the shader: with the history read at
the pixel centre and each frame's raw sample taken as the pixel's, the picture drifted
a fraction of a pixel toward the last few jitters whenever the history was short
(any motion): a camera pan was worse than no anti-aliasing. Fixes that stayed:
Karis' Gaussian reconstruction of this frame at the pixel centre, less history where
the history is read between pixels, and rejecting history by the velocity the
last frame had at the spot it is read from (the first version let the ghost of a
moving block through on high-frequency content).

*Off against `g1b_final`:* all 21 pictures identical (kiln 72 to 79 dB, its own jitter).

*Distance to Cycles, mean levels / SSIM against `truth` (1280 x 720, 120 frames, TAA
pictures after 130 frames):*

| Picture | 1 sample | MSAA 4x (`g2d`) | TAA (`g2d_taa`) |
|---|---|---|---|
| cornell | 10.24 / .8849 | 10.20 / .8868 | 10.20 / .8872 |
| helmet front / side | 0.69 / .9880, 0.72 / .9868 | 0.66 / .9894, 0.68 / .9886 | 0.58 / .9916, 0.62 / .9902 |
| ibl_helmet front / side | 1.39, 1.49 | 1.38, 1.48 | 1.35, 1.45 |
| ibl_spheres front / grazing | 10.91 / .9324, 15.16 / .9209 | 9.49 / .9386, 10.81 / .9342 | 9.53 / .9392, 10.83 / .9351 |
| kiln close / game | 1.66, 0.77 | 1.55, 0.67 | 1.51, 0.65 |
| shadows overview / contact / fence / grazing | 2.43, 4.80, 3.73, 6.89 | 2.38, 4.75, 3.53, 6.86 | 2.41, 4.79, 3.52, 6.89 |
| spheres front / grazing | 3.64, 4.09 | 3.60, 4.05 | 3.60, 4.07 |
| sponza nave / roof | 50.13, 34.05 | 50.10, 33.98 | 49.97, 33.89 |
| furnace | 0.23 | 0.23 | 0.23 |

Honest reading: the mean distance is dominated by what is not edges, so TAA
is within 0.05 of MSAA 4x everywhere and slightly better on the helmet; its SSIM is
higher than MSAA 4x's in every picture (a still camera: edges settle to what many
samples give). Where one sample is clearly worse (`ibl_spheres` grazing 15.2 against
10.8) TAA is level with MSAA 4x. With AO, contact shadows and 4 cascades together
(sponza, shadows): `g2d_all` (no TAA) / `g2d_taa_all`: sponza nave 48.63 / 48.50, roof
32.04 / 31.95, shadows overview 2.54 / 2.59, contact 5.08 / 5.13, fence 4.04 / 4.00,
grazing 7.05 / 7.08 (SSIM higher with TAA in all six); stable (frame-to-frame change
0.06 levels in the unit test).

*GPU cost, ms a render (RTX 4080 Laptop; `*` where compare.py marks it; TAA draws
one sample, so a scene that was cheap under MSAA 4x pays the fixed passes):*

| Picture | off (MSAA 4x) | TAA |
|---|---|---|
| cornell / furnace | 0.076 / 0.039 | 0.149 / 0.140 * |
| spheres front / grazing | 0.130 / 0.078 | 0.246 * / 0.246 * |
| shadows overview / contact / fence / grazing | 0.092 / 0.096 / 0.095 / 0.116 | 0.172 * / 0.224 * / 0.230 * / 0.251 * |
| helmet front / side | 0.152 / 0.168 | 0.243 * / 0.247 * |
| sponza nave / roof | 0.347 / 0.287 | 0.528 * / 0.422 * |
| kiln close / game | 4.609 / 4.277 | 6.156 * / 5.972 * |
| crowd first_person / mid_deck / above | 1.230 / 0.732 / 1.336 (1.07 / 0.58 / 1.29 earlier) | 1.618 * / 0.923 * / 1.649 * |
| sponza + AO + contact + 4 cascades, nave / roof | 1.076 / 1.000 | 1.193 * / 1.086 * |
| shadows + the same, overview / fence | 0.520 / 0.756 | 0.673 * / 0.849 * |

(Small scenes' absolute numbers scatter 20 to 30%.) The fixed cost is about 0.07 to
0.15 ms (pre-pass, resolve, output pass); big scenes add the second geometry pass (kiln
+1.4 ms, 5 M triangles; crowd +0.4 to 0.5 ms); with occlusion and contact shadows on, the
pre-pass is shared and TAA adds 0.09 to 0.15 ms. CPU: `render()` +0.3 ms for the crowd's 22,000
copies (matching), +0.005 to 0.01 ms for small scenes. Sharpening: one more dispatch,
small. **Signal Breach**: not turned on in the game; with a temporary override (reverted
and grepped for) frame 1800 draws correctly (gun in front, enemies, particles), no ghosts,
enemy edges about as aliased as the MSAA frame's.

*The `motion` scene* (new, `procedural: "motion"`, no Cycles truth; judged against a 4 x 4
supersample with 4x MSAA of the same moment, `kin_reference --supersample 4`, error
in mean square levels; the fence region in brackets):

| Camera | 1 sample | MSAA 4x | TAA |
|---|---|---|---|
| still (objects move, camera still) | 69.7 (490) | 13.4 (98) | 16.2 (98) |
| pan (camera moves 1.8 px a frame at the fence) | 69.7 (490) | 13.4 (98) | 43.8 (299) |
| close (fence near) | 111.6 (216) | 17.5 (36) | 23.1 (25) |

(The pan picture's error depends on the frame's jitter phase: 40 to 70 over the settings tried.)
Looked at (`reference_out/g2d_taa_ss`): the turning cross, the orbiting bar, the block, the
ball and the sliding pole show no trails or ghosts; a faint streak on the fence next to the ball
for a frame or two; thin white posts in a pan break up a little (thin-feature TAA's usual
fault, about 3x MSAA's error); no flicker still; static detail as sharp as MSAA's. Jitter crawl was not
visible in stills; a live walk was not seen. Moving-camera evidence: `sponza_walk --spin`
headless frames, the `pan` camera, and the unit tests' pans; not live mouse movement or the
panel's click path (not testable here; `--preset taa` and `--print-settings` were run).

Tests (`kin_render3d_tests`, RTX 4080 and lavapipe): `test_taa_jitter` (values, period,
range, distinct, centred, clamp, the projection shift at any depth); `test_gpu_taa` (off is
the old picture and runs nothing; a still scene settles, frame to frame 0.05 levels; edge
levels 2 against 45, error against a 16 sample reference 99.6 against 11.9; no jitter is
no anti-aliasing; frozen and short jitters; sharpening steepens edges 61%; reset and resize
and `reset_history`; a camera cut; a 4x MSAA view draws one sample and equals a 1x one; a
block moving by place and by id leaves no trail and stays white; a new mesh or copy is white
at once; one that goes leaves nothing); `test_gpu_taa_over_detail` (a block over a 1-pixel
checker leaves no ghost, panning over a textured surface at four rates is
0.1 to 0.6 of the plain picture's error); `test_gpu_taa_crowd` (losing and gaining copies);
`test_gpu_taa_overlay` (the gun's edge, in and out, against a supersample); `test_gpu_taa_with_occlusion`
(pre-pass drawn once, 3 + 3 + 1 dispatches, crease dark, stable); `test_gpu_taa_pick` (pick is the
same every frame with the jitter); `test_gpu_taa_hdr` (bloom on the resolved picture).
Two checks were made by breaking the code (overlay velocity off, place matching off) and
seeing the tests fail.

Not done: varying the occlusion's noise by frame; velocity for blended and additive
meshes; a depth-based disocclusion test (the velocity one is used); the pan's jitter-phase
dependence; Signal Breach has no TAA switch.

Every new or changed scene (moving object, thin geometry for AO halos) gets
a `truth` render. Unit tests for the cascade fit and the Halton sequence;
pixel tests with tolerance on the RTX 4080 and lavapipe (compute on
lavapipe checked); the 3D-off build keeps building.

#### G2 summary

| Step | Gains | Costs (RTX 4080 Laptop, 1280 x 720) | Default |
|---|---|---|---|
| G2a cascades | sharp shadows near the camera in a large `shadow_area` (3 px edge to 1 px at 60 m, test) | casters drawn again for each cascade; nothing in the small reference scenes (identical pictures) | 1 cascade (off) |
| G2b pre-pass and GTAO | ambient and environment light shut off in creases: Sponza 50.1 to 48.7 (radius 0.5), further with a larger radius | +0.25 to 0.55 ms in small scenes, +0.9 to 1.0 ms in the crowd (not turned on in Signal Breach); measured again in phase 2: +0.3 to +0.6 ms at 1280 x 720, +1.0 to 1.6 at 1080p (-0.2 to -0.65 since phase 2's depth chain), several ms at 4K (frames of 7 to 20 ms with it; -25 to -35% since phase 2) | off |
| G2c contact shadows | small contact shadows a coarse map misses | +0.15 to 0.3 ms (shared pre-pass with AO: +0.2 to 0.3 more, less 0.03 to 0.15 since phase 1 shares one blur between them); alone, 0.03 to 0.07 ms less since phase 2 (one blur pass: 0.16 of which the march 0.08, at 720p; 0.39 at 1080p); no gain on the reference scenes' distances | off |
| G2d TAA | edges and fine detail as good as or better than MSAA 4x (SSIM) from one sample, steadier; works with the rest | +0.07 to 0.15 ms fixed; the second geometry pass of the first version (kiln +1.4, crowd +0.4) is gone since phase 1 (the main pass writes the velocity: the kiln's TAA cost fell by 2.2 ms of 7.2, Sponza's by 0.18 of 0.7, the crowd's by 0.5 of 2.4); +0.09 to 0.15 over AO + contact; CPU +0.3 ms for 22,000 copies | off |

Every feature is off by default and the picture and cost are then those of
`g1b_final` (checked per step, byte for byte in Signal Breach). None of the four
brought kin measurably nearer Cycles by mean distance; they are quality
and stability features (SSIM, edges, creases), and AO and cascades matter most in bigger levels.

### G3 — Adaptive meshes, GPU-driven drawing

Starting point (G2 done; the harness at 1280 x 720, 4x MSAA): the kiln is one
5.15 M triangle model in 13 draws and costs 4.0-4.3 ms of GPU; the crowd (the
Signal Breach deck and 20,000 enemies) is 1.0-2.6 M triangles in 4 instanced
draws, 0.6-1.3 ms of GPU and 0.2 ms of CPU `render()` (after the G2 review the
CPU culling and filling are parallel and cheap). So two different cases:
one huge mesh whose triangles are mostly smaller than a pixel, and many
copies of small meshes whose far copies need fewer triangles.

SDL_GPU has what this needs, and has limits to design round: compute writing
to storage buffers (used since G1b), indexed indirect draws with a count fixed
at record time (no draw count read from the GPU, so culled draws are written
with an instance count of 0, or the work is compacted into one draw),
vertex pulling from storage buffers (instances already do). Nothing here uses
mesh shaders, which SDL_GPU does not have.

Every step is opt-in (a mesh built with levels, a scene setting), and without
it the pictures are the same to the bit. A picture with levels is judged
against the same scene at full detail (what the error costs) and against
before (what it saves): the error is bounded in pixels (default 1).

#### G3a — Levels of detail for meshes. Done.

Adds meshoptimizer (MIT; FetchContent like cgltf; see Decisions wanted).
`Renderer3D::create_mesh(data, {.levels = n})` builds, at load, a chain of
simpler versions (`meshopt_simplify`, attribute-aware: normals and uvs keep
their weight; borders locked where a material or a seam would show), each
with the object-space error it introduces (`meshopt_simplifyScale`). Per copy,
the renderer picks the level whose error projects to under the bound on
screen (`error * scale / distance * focal`), with hysteresis to avoid
popping, and with the copy's own scale; the shadow and pre-pass use the same
level as the main pass (a coarser one for shadows is a later option). Copies
of one mesh at one level are one instanced draw, so a draw becomes up to
`levels` draws. First on the CPU in the parallel cull (the copies are already
visited there), which makes the quality and the cost measurable before any
GPU work. Checks: crowd (triangles and GPU ms against before; the picture
against full detail, which should differ by under a pixel's worth of edge),
the kiln as one mesh (it will show the limit of whole-mesh levels: the near
part cannot be coarse and the far part fine), build time at load.

#### G3a — results

Built: meshoptimizer 0.25 (MIT; `simplifier`, `vcacheoptimizer`, `indexgenerator`, `allocator` compiled into the
engine under `KIN_EXPERIMENTAL_3D`; license in the third-party notices), `MeshOptions`
(`Renderer3D::create_mesh(data, {.levels = n})`, `load_model(path, options)`), `Mesh::level_count()`,
`level_triangles()`, `level_error()`, `Scene3D::lod {enabled, error_pixels, bias, force_level}`,
`Render3DStats::lod_copies_by_level`, `kin_reference --lod --lod-error --lod-bias --lod-force --lod-levels` (and
`lod` / `models[].lod_levels` / `lod_levels` in scene files), Sponza Walk's "Levels of detail" section and
`--preset lod`, `--lod-levels N` (default 4). Selection is on the CPU in the parallel cull; every pass uses the
copy's one level; a draw becomes up to `levels` draws over one index buffer (levels are runs of it, over the one
vertex buffer); the instance buffer layout (`GpuInstance3D`) is unchanged, copies grouped by level then seen before
sun-only. No hysteresis: copies are not matched across frames except for TAA, so there is nothing to keep a level
in; popping is measured below instead. Alpha-blended draws are not levelled.

Checks (RTX 4080 Laptop, 1280 x 720, 4x MSAA; "full" is the same meshes made with 4 levels and the selection
off; the picture of "full" is byte-identical to meshes made without levels):

- **Off path.** `g3a_before_def` / `_all` (crowd, shadows, sponza, helmet, motion, spheres, cornell; default, and
  `--cascades 4 --ao --contact-shadows --taa`) rendered from the pre-change binary, again after: every PNG
  byte-identical and every draws/triangles/casters line equal, except `crowd/above` in the `_all` run, which was
  rendered while another test was running: the pre-change binary alone gives 07ea6cc4 under that load and 6ce1cac0
  quiet, the new one 6ce1cac0 quiet, 3 runs each (a load-dependent TAA flip with 20,000 copies, as in the shot
  nondeterminism notes; not this change). A Signal Breach frame (seed 7, 2,000 enemies, frame 1800) is byte-identical
  between the pre-change and the new binary.
- **Tests.** `kin_render3d_tests`: the builder (sphere, torus, a wavy open patch whose border vertices all stay,
  without the lock some are lost, an unindexed mesh, a flat-shaded unindexed mesh, tiny meshes give one level, the
  recorded errors bound the measured deviation to within x1.5 and are not looser than x8), selection against a formula
  written in the test (distance, scale, bias, force, orthographic, mesh without levels, blended), off equals old to
  the bit, a level-drawn copy equals a mesh made of that level to the bit with one map, cascades, occlusion and
  contact shadows on (so colour, shadow and pre-pass agree), pick, workers equal serial with everything on, TAA with
  a copy changing level. All pass on the RTX 4080 and on lavapipe; ctest 67/67 (3D) and 65/65 (3D off).
  Mutations that each made a new test fail: scale ignored, finest instead of coarsest level, shadow draws level 0,
  pre-pass draws level 0, pick draws level 0, TAA previous matrix by output place (this one only after the TAA
  test was given a textured copy and a mean-square-error bound: the first version, a trail check, passed it),
  cascade list ignoring the level, border lock off.

**Full detail against levels (1 px, 4 levels), median of 3 interleaved pairs**; the picture columns are against full
detail (mean in levels of 255, PSNR, SSIM, p99):

| Scene | Triangles full / levels | GPU ms full / levels | CPU render() ms | Picture (mean, PSNR dB, SSIM, p99) |
|---|---|---|---|---|
| crowd first_person | 2.19 M / 1.59 M | 1.12 / 1.05 to 1.20 | 0.15-0.18 / 0.17-0.18 | 0.00, identical |
| crowd mid_deck | 0.99 M / 0.88 M | 0.70 / 0.68 to 0.78 | 0.13-0.15 / 0.14-0.15 | 0.00, identical |
| crowd above | 2.58 M / 1.51 M | 1.39 / 1.20 to 1.39 (`*` in 2 of 3) | 0.15 / 0.15 | 0.50, 38.2, 0.993, 14 |
| kiln game | 5.15 M / 0.65 M | 4.3 / 0.79 `*` (x0.18) | 0.016 / 0.017-0.025 (no `*` in 2 of 3) | 0.09, 48.6, 0.998, 2 |
| kiln close | 5.15 M / 0.66 M | 4.4-4.7 / 0.87 `*` (x0.19) | 0.016 / 0.018-0.032 (no `*`) | 0.18, 49.0, 0.995, 3 |
| sponza nave | 260 k / 173 k | 0.32 / 0.29-0.31 (not `*`) | 0.10 / 0.10-0.14 | 0.05, 50.8, 0.999, 1 |
| sponza roof | 185 k / 169 k | 0.28 / 0.27-0.31 (not `*`) | 0.07 / 0.07-0.09 | 0.02, 55.6, 1.000, 1 |
| helmet | 94.7 k / 57.6 k | 0.13-0.17 / 0.12-0.18 (not `*`) | 0.014 / 0.015 | 0.05, 47.6, 0.999, 1 |
| shadows (4 cameras) | 0.3-2.9 k / 0.3-1.7 k | 0.06-0.11 / same (not `*`) | +0 to +0.002 | 0.00-0.01, identical or 58 dB |

The CPU cost of the selection is under the noise (a few hundredths of a ms at 22,000 copies); with the setting on
`render()` is not measurably slower except where `*` is not shown at all, the kiln's apparent x1.5-1.9 is 0.01 ms noise
on 13 copies. **Error bound** (levels 4; GPU ms; triangles; picture difference from full detail, mean levels / PSNR dB /
p99):

| Bound | Crowd above: triangles, GPU ms | Crowd above picture | Kiln game: triangles, GPU ms | Kiln game picture | Kiln close picture |
|---|---|---|---|---|---|
| full | 2.58 M, 1.48 | | 5.15 M, 4.32 | | |
| 0.5 px | 2.34 M, 1.57 | 0.05 / 49.1 / 0 | 651 k, 0.73 | 0.06 / 51.6 / 2 | 0.15 / 50.9 / 3 |
| 1 px | 1.51 M, 1.35 | 0.50 / 38.2 / 14 | 648 k, 0.75 | 0.09 / 48.6 / 2 | 0.18 / 49.0 / 3 |
| 2 px | 1.10 M, 1.20 | 0.88 / 34.8 / 23 | 646 k, 0.71 | 0.15 / 43.2 / 3 | 0.29 / 45.4 / 5 |
| 4 px | 1.10 M, 1.18 | 0.88 / 34.8 / 23 | 645 k, 0.70 | 0.22 / 39.9 / 4 | 0.50 / 37.3 / 8 |

(GPU ms between pairs drift by about 5%.) Four levels end at an eighth of the kiln's triangles whatever the bound, so
its floor is 0.65 M; with 6 levels (`--lod --lod-levels 6`, 1 px) the kiln is **167 k triangles, 0.31 ms (game) and
192 k, 0.44 ms (close), against 4.3 and 4.4**, picture 0.11 / 47.7 dB and 0.18 / 49.2 dB, build 8.5 s. The crowd's
enemy meshes are small (a 5-point star, a 6-sided cylinder, a 320-triangle torus): only the tori have levels worth
taking, so the crowd saves 0.4 to 1.1 M of its 1.5 to 2.6 M triangles above and at the first-person view hardly
anything is simplified at 1 px. **Recommended default: 1 px with `bias` 1** (below it nothing more is saved; above 2 the
crowd's picture falls under 35 dB); the setting stays off by default and meshes keep one level unless asked
(`--lod-levels`), 4 for Sponza Walk, 6 for a mesh as dense as the kiln.

**With everything on** (`--cascades 4 --ao --contact-shadows --taa`, levels 4, 1 px): the kiln falls from 6.2-6.9 ms
GPU to 1.18-1.52 (x0.19-0.22, pictures 0.09-0.17 levels, 50 dB). **The crowd gets slower**: GPU 2.07 / 2.33 / 1.79 ms
to 2.24 / 2.52 / 1.99 (+8 to 11%, `*` on one) and CPU `render()` 0.39-0.47 to 0.53-0.59 (+25 to 33%; this scene's CPU
time swings twofold, so treat as indicative): its tori are 320 triangles, so the saving is small and the pre-pass, the
velocity pass, the shadow pass and the main pass each draw a run more per level in use (draws 4 to 5-6). A scene of
small meshes should keep the setting off, or give levels only to meshes of a few thousand triangles.

**Build time and memory.** The kiln (13 meshes, one of 4.96 M triangles in 14.9 M unindexed vertices, flat-shaded because
the file has no normals): 8.0 to 8.2 s at load for 4 levels (the whole model load, levels included; the no-level load was not timed separately), 8.5 s for 6; the whole
`kin_reference` run peaks at 2.74 GB (4 levels) and 2.85 GB (6) against **1.62 GB with no levels** (+1.1 GB, of which the
levels' own vertices, 36 bytes a vertex of 12.8 M, are 0.45 GB and are also kept on the GPU: 9.7 M triangles with all levels
against 5.15 M). A first version that simplified the 14.9 M vertices with all nine attributes peaked at 6.6 GB and was
killed by the OOM killer: the simplifier's per-vertex state with attributes and `Permissive` was the cause. A flat
unindexed mesh is now welded by position, simplified on positions alone (no attribute state), and each level gets vertices
of its own with the triangle's flat normal and the UV and colour of the corner it replaces (`MeshLodChain::extra_vertices`);
level 0's vertices and indices are untouched. Sponza (103 meshes, 79 with levels): 1.3 to 1.8 s load, peak 562 MB; helmet 1.3 s
(load with levels), the crowd's four meshes 2 ms.

**What the pictures show.** Kiln, game and close at 1 px and at 6 levels: no cracks or holes (the glTF's parts are
separate meshes at their own levels, and the borders of a part are locked; I did not examine where the kiln's parts meet, nor
build a model whose parts share an open border, so seams between parts at different levels are unproven, and one level for all parts of a model,
chosen from the model's bounds, is the mitigation I would add). The difference image (x8) is a faint pitting on the
dome and fine edges of the door: the flat shading's normals are regenerated per level, and the geometry differs by under a
pixel. Crowd above at 1 and 4 px at 3x: indistinguishable to the eye in the dense heap. No silhouette shrink beyond the
error (a simplified surface lies inside a convex one by about the error); textures: Sponza at 1 px shows no swimming in
its pictures (mean 0.05 levels). **Popping**, from a 60-step dolly (kiln, margin 1.0 to 0.35; crowd, the `above` camera
sinking), the part of each step's change that full detail does not have, mean over pixels: kiln 0.33 levels at 1 px and
0.82 at 4 px (the picture itself differs from full by 0.16 and 0.41); crowd 1.33 at 1 px and 4.4 at 4 px (picture 0.49
and 1.67): the crowd's changes are about 2.7 times its static error, small coloured edges changing as 20,000 tori swap
levels; I looked at no frame sequence by eye (the numbers are all I have), and without hysteresis a still camera is steady while a
moving one changes at the scale of the bound; 1 px is under what is visible in the two scenes, 4 px is where I would expect to see it.

**Honest limits.** Whole-mesh levels: the kiln is one 5 M triangle mesh, so a close-up of it takes the finest level
whole when any of its bounding sphere is near, and the 0.65 M floor of four levels shows that levels, not culling, set
the cost; its win comes from the camera being far enough (a fit of its whole size) that even the finest level is under a
pixel a triangle. A close-up of one corner is G3b's case (clusters). The recorded error is meshoptimizer's per-step
error added up: against a measured deviation it was 0.72x the measured for the first level of a sphere (a bound within
x1.5 in the tests, x1.0 to x1.4 measured, looser at coarse levels), so the 1 px bound is about 1 to 1.4 px.
Not tested: the Sponza Walk panel click path (the settings and `--preset lod` are, through a headless run); shadows from a
coarser level than the camera's (the plan's later option); LOD for the overlay and for blended meshes; hysteresis. A
one-line enablement for Signal Breach (not made): `scene.lod.enabled = true;` in its view's scene fill, with the enemy
meshes made with levels (`create_mesh(data, {.levels = 3})`); only the tori would gain and the all-features crowd
numbers above say it would cost more than it saves there.

#### G3b — Clusters. Done.

For one big mesh the unit is a cluster: about 128 triangles
(`meshopt_buildMeshlets`), with a bounding sphere and cone for culling.
Clusters are grouped (4 or so), each group simplified with its border locked
to the neighbours' (`meshopt_SimplifyLockBorder`), then re-split, up the
levels into a hierarchy (a DAG, as Nanite's), so that every cut through it is
crack-free: a cluster is drawn when its own error is under the bound on
screen and its parent's is not, which needs no communication between
neighbours. Stored as storage buffers (cluster table, a vertex and an index
pool); built at load (the kiln's 5 M triangles take seconds: measure, and say
whether a cache is needed). Checks: the hierarchy's invariants (every cut
covers the surface once, errors grow up the levels, no cracks: a test draws a
cut and counts open edges), and the build's time and memory.

#### G3b — results

Done, with the limits below. One huge mesh is cut per frame through a cluster hierarchy, on the CPU; G3c moves the cut to the GPU.

**Built.** `engine/include/kin/render3d/mesh_clusters.hpp`, `engine/src/render3d/mesh_clusters.cpp`: `build_mesh_clusters` (pure CPU;
meshoptimizer 0.25's `meshopt_buildMeshletsSpatial` for leaves of at most 128 triangles, `meshopt_partitionClusters` for groups of
about 4, `meshopt_simplify` with `LockBorder | ErrorAbsolute` on each group, run on local vertex numbers so that it works on a few
hundred vertices and not the mesh's millions; groups run on the job system; the sources `clusterizer` and `partition` were added),
`select_clusters` (the cut: a template over a cull object, so the tests and the renderer run the same code), `ClusterCutTest`,
`MeshOptions::clusters` (`levels` is then ignored for that mesh), `Mesh::cluster_count()` / `cluster_levels()` / `cluster_roots()` /
`leaf_triangles()` / `cluster_triangles()` / `flat_shaded()`. The hierarchy is a DAG as Nanite's: a group of clusters is merged,
simplified to half with its outer border (and the mesh's open borders) locked, split into new clusters; a group's error is the largest
input error plus the simplification's, its sphere holds its inputs' spheres, and all its outputs take its (error, sphere) as their own.
A cluster is drawn when its own error projects to at most `error_pixels / bias` pixels at the nearest point of its sphere and its
parent's does not (roots: always); distance as in G3a (along the view, minimum the near plane; orthographic: the view's pixels a unit),
scaled by the copy's largest scale. Error and sphere only grow up a chain, so the projected error is monotonic and exactly one cluster of
each chain from a leaf to a root is drawn. `force_level N` is the cut at hierarchy depth N (0: the leaves). A group that cannot be
simplified by 15% waits and is grouped again next round; if a round gets nowhere the remaining clusters are roots (open meshes and
tori end with 2 or 3 roots, closed spheres with 1; the kiln: see below). Flaps (two triangles on the same vertices, one turned over) that
the simplifier leaves are removed per group; across two groups a few remain (a sliver on a shared border from each side, nothing showing).

**Normals.** A mesh whose every corner normal is its face's (within 0.25 degrees; 99% of triangles) is flat: it is welded by position,
UV and colour (not normal), its vertices carry a zero normal, and the shader makes the normal from `dFdx/dFdy` of the world position
(`kin_material.inc.glsl`, `kin_prepass.inc.glsl`; the pre-pass shaders got a `vWorld` varying), turned to face the viewer (SDL_GPU's rows run down:
the sign was calibrated on Vulkan, on NVIDIA and lavapipe; D3D12 and Metal untested, the same convention by SDL_GPU's design). Cost
in picture: forced to the leaves the kiln equals the unclustered mesh at **89.7 dB (mean 0.00)**; a flat plane and a smooth torus are equal to the
bit. A normal map or tangents have no effect on a flat mesh (out of scope); smooth meshes keep their normals, welded by every attribute
(indexed ones as given). A cheaper way would be quantised per-cluster normals, which are wrong for any non-planar cluster; derivatives were exact here.

**Drawing the cut (CPU, `GpuRenderer3D::cut_clusters`).** After the copies are placed, each clustered copy is cut: one top-down traversal
per copy (serial below 24 copies: on the kiln's 13, one of them 96% of the clusters, workers took 1.1 ms against 0.8 serial; from 24 copies,
`parallel_for`, results by copy so the order is fixed), with the sphere tests of its clusters against the camera frustum and the
shadow views (each cascade's frustum and slice, as the copy-level tests; the one map's light frustum and what can shadow the view): a subtree no view reaches is
skipped. The cut itself uses the camera's distance for every pass, as G3a. One compacted `u32` index buffer is built per frame (a
transient INDEX buffer, grown geometrically, cycled, uploaded as the instance buffer is, written on the workers) holding for each copy one
run for the camera's passes (main, pre-pass, velocity, pick: the same clusters) and one run per shadow view, a run equal to an earlier one
being that run. I chose per-view runs over reusing the main cut for all passes because casters outside the camera's frustum that
shadow the view need clusters the camera run lacks, and because a cascade needs only its own; the price is upload bytes with shadows on
(the kiln's cut 1.9 MB at game, 3.5 at close with one shadow map). Draws: one indexed draw per copy per pass (`first[0]` the instance slot, the shadow
lists as before); `GpuInstance3D` and the TAA matching (by mesh) are untouched. Statistics: `cluster_copies`, `clusters_tested`,
`clusters_drawn`, `cluster_upload_bytes`, `cluster_ms`, `cluster_upload_ms`; `triangles` counts the camera's cut. Not cut: alpha-blended
copies, meshes without clusters, and anything with `lod` off (drawn whole from a persistent index buffer of the leaves, 62 MB of VRAM
for the kiln against 536 MB of vertices before: the welded array is 2.5 M vertices).

**Tests.** New `kin_mesh_cluster_tests` (no GPU): leaves cover each triangle exactly once; at most 128 triangles and 255 vertices a cluster;
error and sphere monotonic, groups consistent; 150 random cuts of each of an icosphere, a soup of the same, a torus, an open tube, a wavy
patch, the patch as a flat soup, a plane and a half-flat half-rough patch (random copy scale, turn, eye near, far or inside,
orthographic sometimes, error 0.25 to 6 px) and forced depths 0 to past the last: the traversal equals the definition, every chain
from a leaf to a root has exactly one chosen cluster, and the open edges of the chosen triangles (welded by position) equal the mesh's (0 for the closed ones, 128 for the
tube); the formula written out in the test with scales; culled cuts; tiny meshes (one cluster); the parallel builder equals the serial. In
`kin_render3d_tests`: off equals the old picture and counts (to the bit; flat plane too), forced leaves equal full detail, the cut's triangles equal the CPU's for 4 bounds
and 4 forced depths and the scale counts, near clusters finer than far in one mesh, picture within a bound, **the picture equals a plain mesh made of the cut, to the bit, with 1 and 3
cascades, occlusion and contact shadows on or off, copy culling on or off** (so colour, pre-pass and every shadow map draw the cut), pick, workers equal serial
(picture byte-identical, same statistics), TAA (still frames equal the stand-in's, moving camera against 8 samples). 68/68 ctest (3D), 65/65 (3D off), the 3D tests pass on the RTX 4080
and lavapipe, `kin_example_tests` pass. Mutations that made a test fail: unlocked borders (open edges), wrong parent in the cut, scale ignored (CPU formula, and in the renderer the transform
dropped), a parent error that is not the largest child's (only after adding the half-flat half-rough patch: on smooth meshes the simplification errors grow by themselves, so the first version of the test passed it), a union sphere
that does not contain its children, a cascade drawing another's run, pick binding the mesh's own indices. Not caught: a shadow pass using the camera's run (the runs are equal in those scenes, so it
is equivalent there).

**Off path.** `g3b_before_def` / `_all` (crowd, shadows, sponza, helmet, motion, spheres, cornell; default and `--cascades 4 --ao --contact-shadows --taa`) from the G3a binary, again after: every PNG
byte-identical and every draws/triangles/casters line equal, except `crowd/above` in the `_all` run, rendered under machine load: quiet, the G3a and the new binary give the same
picture (6ce1cac0) 3 runs each (the known load-dependent TAA flip). The shaders changed (a branch on a zero normal), results did not.

**Clusters against full detail and G3a's 6 levels** (1 px; 1280 x 720, 4x MSAA; medians of 3 interleaved runs; picture against full detail: mean levels / PSNR dB / SSIM):

| Scene | Triangles full / levels 6 / clusters | GPU ms full / levels / clusters | CPU render() ms full / levels / clusters | Picture levels | Picture clusters |
|---|---|---|---|---|---|
| kiln_near dome_close | 5.15 M / 2.67 M / **134 k** | 4.37 / 2.32 / **0.39** | 0.02 / 0.04 / 0.70 | identical (no whole level fits) | 0.25 / 50.5 / 0.996 |
| kiln_near grazing | 5.15 M / 2.60 M / **87 k** | 4.20 / 2.12 / **0.29** | 0.04 / 0.04 / 0.51 | 0.21 / 49.9 / 0.996 | 0.17 / 51.6 / 0.997 |
| kiln_near far | 5.15 M / 166 k / 134 k | 4.29 / 0.27 / 0.31 | 0.05 / 0.02 / 0.53 | 0.09 / 46.0 / 0.998 | 0.08 / 47.0 / 0.998 |
| kiln game | 5.15 M / 168 k / 169 k | 4.27 / 0.31 / 0.38 | 0.02 / 0.04 / 0.75 | 0.11 / 47.7 / 0.997 | 0.10 / 48.3 / 0.998 |
| kiln close | 5.15 M / 192 k / 308 k | 4.68 / 0.52 / 1.16 | 0.03 / 0.02 / 1.47 | 0.18 / 49.2 / 0.996 | 0.18 / 48.5 / 0.995 |
| sponza nave | 260 k / 173 k / 146 k | 0.35 / 0.33 / 0.46 | 0.14 / 0.14 / 0.33 | 0.05 / 50.8 / 0.999 | 0.09 / 49.8 / 0.998 |
| sponza roof | 185 k / 169 k / 63 k | 0.33 / 0.31 / 0.33 | 0.09 / 0.15 / 0.24 | 0.02 / 55.6 / 1.000 | 0.26 / 41.3 / 0.994 |
| helmet | 94.7 k / 57.6 k / 67 k | 0.17 / 0.15 / 0.22 | 0.014 / 0.02 / 0.14 | 0.05 / 48 / 0.999 | 0.02 / 53 / 1.000 |

**With everything on** (`--cascades 4 --ao --contact-shadows --taa`), GPU ms full / levels 6 / clusters: kiln_near dome_close 6.45 / 3.85 / **1.06**, grazing 6.13 / 3.46 / **0.82**, far 6.08 / 0.51 / 0.55;
kiln game 6.09 / 0.62 / 0.63, close 6.78 / 0.98 / 1.14; sponza 1.20 / 1.18 / 1.31 and 1.12 / 1.13 / 1.14; helmet 0.41 / 0.40 / 0.46. Pictures against full: 0.00 to 0.24 mean, 47 to 56 dB (Sponza roof 42 dB).

**Error bound** (kiln_near, clusters; triangles, GPU ms, picture mean / PSNR dB / SSIM against full detail):

| Bound | dome_close | grazing | far |
|---|---|---|---|
| 0.5 px | 191 k, 0.45, 0.21 / 51.9 / 0.997 | 119 k, 0.29, 0.12 / 54.0 / 0.998 | 241 k, 0.48, 0.04 / 52.7 / 0.999 |
| 1 px | 134 k, 0.49, 0.25 / 50.5 / 0.996 | 87 k, 0.31, 0.17 / 51.6 / 0.997 | 134 k, 0.31, 0.08 / 47.0 / 0.998 |
| 2 px | 91 k, 0.38, 0.44 / 47.0 / 0.992 | 60 k, 0.25, 0.26 / 49.0 / 0.996 | 67 k, 0.21, 0.11 / 45.0 / 0.997 |
| 4 px | 61 k, 0.33, 0.53 / 45.0 / 0.990 | 43 k, 0.23, 0.39 / 44.7 / 0.993 | 32 k, 0.17, 0.17 / 41.9 / 0.995 |

(Full detail 4.1 to 5.1 ms. The GPU time has a floor near 0.2 ms: the shadow pass and fixed costs.)

**The CPU cut.** Kiln, 1 px, 13 copies: cut 0.35 to 0.7 ms at kiln_near, 0.45 (game) and 0.8 to 1.1 ms (close) a frame, of which compacting and handing over the upload 0.08 to 0.45 ms
(1.0 to 3.5 MB: 0.2 ms a MB incl. staging); the rest is the traversal, 3 to 7 thousand clusters tested of 91 thousand, about 60 ns a cluster (cache misses: a cluster is 96 bytes in creation order).
That is 30 to 70 times `render()`'s 0.02 ms without clusters: it is the price of this step, and what G3c removes. Forced to the leaves (an unrealistic cut) it uploads 59 MB and costs 11 ms.

**Build time and memory.** Kiln (13 meshes, one of 4.96 M triangles in 14.9 M unindexed vertices; flat): **whole model load 4.7 to 5.2 s with workers (the glTF read included), 91,307 clusters,
17 levels deep, 10.3 M triangles in all levels (2.0x the leaves)**; the `kin_reference` run peaks at **1.52 to 1.56 GB, against 1.62 GB with no hierarchy and 2.74 GB with G3a's 4 levels**
(the extra for the cluster build over the same run without it is negative: the GPU path no longer holds the 14.9 M-vertex soup, only the 2.5 M welded vertices; the CPU hierarchy is about 0.4 GB resident: indices
as local bytes and vertex lists, 96-byte cluster records). The 4-scene runs peak at 1.67 GB (full), 1.91 (clusters) and 3.10 GB (6 levels). Sponza: 1.4 s, 103 meshes, 4,742 clusters, peak 625 MB (G3a: 562 MB);
helmet 1.1 s, 1,780 clusters. No cache is needed for the kiln (5 s); it would help a bigger asset.

**What the pictures show.** kiln_near at 1 px and at 4 px, crops at 3x with a x8 difference image: no cracks or holes at any boundary I could find, including the ring at the dome's base and the
chimney collar; at 4 px the difference is faint facet shading (flat triangles merged into larger ones) and a dotted line of single pixels along a silhouette and a seam, which is the edge moving by a pixel, not a hole.
No shading step at cluster borders that I could see (the normals are per face, so a merged facet is what changes). Popping, 60-step dolly toward the dome (margin 1.0 to 0.12), the part of each step's change that full detail does not have, mean
over pixels: clusters **0.29 levels (worst step 0.46)** at 1 px and 0.70 (1.06) at 4 px, G3a's 6 levels 0.27 (0.54) and 0.65 (0.93); the static error 0.18 / 0.43 against 0.17 / 0.40; full detail itself
changes by 3.7 per step. So clusters pop as much as levels, no more; I have not looked at a frame sequence by eye.

**Where clusters beat levels, and where not.** They win by 6 to 7 times in GPU time (and 20 to 30 times in triangles) where one mesh spans near and far, as kiln_near dome_close and grazing:
whole-mesh levels must keep level 0 because part of the mesh is close. They tie when the whole mesh is far (kiln_near far, kiln game). They lose where the cut is not much finer than a whole level would be: kiln close
(308 k against 192 k triangles, 1.16 against 0.52 ms: locked group borders and a sphere-based distance keep more triangles at a given error), Sponza (small textured meshes: 0.46 against 0.33 ms, the CPU cut 0.2 ms, a draw
a copy a pass; and thin features move by the error: Sponza roof 42 dB) and the helmet. **Recommendation:** clusters for single meshes of hundreds of thousands of triangles or more that are seen from near and far
(terrain, a kiln, a scanned statue); levels for many copies and mid-size meshes; neither for the crowd. Default stays off; 1 px with `bias` 1, as for levels. The crowd with `--lod --lod-clusters`: its meshes are procedural and
unclustered, and pictures, triangles and draws are identical to without (GPU 1.15 / 0.73 / 1.42 ms against 1.15 / 0.71 / 1.48; CPU the same).

**Honest limits and not tested.** The cut is on the CPU (0.4 to 1.5 ms on the kiln; G3c). No hysteresis. Open meshes and tori do not reach one root (the border stays locked: 2 or 3 roots; the kiln's
parts have a few), so their coarsest cut is a few clusters. Error is meshoptimizer's quadric error added up the hierarchy (a bound on position, not on attributes: UV stretch and normal change on smooth textured meshes
are not bounded, only weighed). Alpha-blended copies are not cut; normal maps do nothing on flat meshes. Seams between clusters of different levels are crack-free by construction and by the open-edge test on closed and open test meshes and by eye on the kiln; I built no model whose parts share an open border (seams between *parts*
at different levels, as in G3a, are unproven). D3D12 and Metal untested for the derivative sign. Sponza Walk's panel click path is not tested (the `--lod-clusters` option, `--preset clusters`, the stats line and the section
text were run headless). Peak memory of the kiln run is the harness's, not a game's.

#### G3c — Culling and drawing on the GPU. Done.

Compute selects, per frame, what is drawn: a pass over the copies (frustum
and the level), then over the clusters of the surviving copies (the cut by
error, the frustum, the cone), writing a list of visible clusters, then a
second pass that compacts their triangles' indices into one buffer and
writes the indirect arguments (one indirect draw a mesh and material, as
a copy's own `GpuInstance3D` is already read from a storage buffer, vertices
read through the compacted indices). The shadow passes (one a cascade) and
the pre-pass select the same way against their own frusta, from the same
cluster tables. The CPU culling stays for scenes that don't opt in, and for
GPUs without storage-buffer compute. Checks: the kiln (the aim: GPU ms and
triangles drawn against before, with the picture within the bound of full
detail), the crowd (CPU `render()` ms should fall further, GPU stay level),
`stats` (clusters tested, drawn, triangles).

#### G3c — results

Done, with the limits below. Clustered meshes are cut, culled and compacted in compute and drawn with indirect draws; the picture is the CPU cut's.

**Design, as built.** `engine/shaders/cluster_{cull,scan,args,gather}.comp.glsl`, `GpuRenderer3D::cut_clusters_gpu` / `cluster_pass_gpu`.
`Scene3D::lod` gained `gpu_culling` (default on; the CPU cut of G3b is the fallback and the A/B), `max_cluster_triangles` (default 4 M triangles,
48 MB of video memory) and `stats_readback`; `Renderer3DCapabilities::gpu_driven_clusters`.

- *No traversal.* The cut test of a cluster is local (own error within the bound, parent's not), so one thread tests one (copy, cluster):
  91 k clusters of the kiln in a dispatch a mesh. The test is the CPU's `ClusterCutTest` / `ClusterViews` written again in GLSL, same
  expressions in the same order with `precise` (no fused multiply-add), the copy's scale taken from the CPU as a float so that it is the
  same bits. The frustum tests are the renderer's own (camera, each cascade and its slice, the single map; the cut's distance is the camera's).
- *Mesh data in video memory, in shared chunks*: a table of 80 bytes a cluster (spheres, errors, depth, first index) and every cluster's indices as `u32`
  after the leaves' in the mesh's index range; the index buffer is also a storage buffer (so the leaves are not stored twice): kiln
  **+62 MB of indices for the upper levels and 7.3 MB of table** (the hierarchy takes 134 MB of video memory in all). Meshes are put in
  *chunks* (`GpuClusterChunk`: 16 MB of indices and a table of 16 thousand clusters, or a chunk of the size a bigger mesh needs; the mesh's draws bind the chunk's buffer
  and use `first_index`), and the cut is one dispatch a chunk, not a mesh. The first version bound each mesh's own buffers: on the kiln
  (13 meshes) it cost 0.06 ms, on Sponza (103 meshes) **1.05 ms**, because SDL_GPU puts a pipeline barrier round every storage buffer it
  binds and unbinds (`VULKAN_BindComputeStorageBuffers` moves each to compute usage and back). With chunks Sponza's cut costs less than the CPU's.
  A mesh's range in a chunk is not given back when the mesh goes (the chunk is, with its last mesh).
- *Compaction without atomics in the order*: the cull writes each (view, cluster)'s triangle count into one array (a row a view, all
  copies' clusters in a row), a three-dispatch exclusive scan (block sums, scan of the blocks, scan in place) turns it into positions, and a
  gather copies the chosen clusters' indices to `scan * 3`. Chosen clusters are queued for the gather with an atomic counter, but the
  order of the queue shows nowhere: positions are the scan's, so the gathered indices are the clusters' own order every run (tested byte
  for byte). One indexed *indirect* draw per (view, copy), and one `SDL_DrawGPUIndexedPrimitivesIndirect` multi-draw per draw of the
  scene and pass; its `first_instance` is the copy's slot in `GpuInstance3D`'s buffer, which stays as it was and is read by the compute
  from where it is (no re-upload). An empty run is a count of 0.
- *Capacity: one global buffer, not a region a (pass, copy)*. Per-region worst cases would be (views x copies x the mesh's triangles):
  13 copies x 5 views x 5 M would be 3 GB. The scan already gives every run its place in one array, so the room is one number
  (`max_cluster_triangles`, clamped to at most the sum of the leaf triangles of the copies times the views, and to 100 M); a run
  that passes it is cut short at the room exactly (the draw's count is clamped; the gather writes no more than the room),
  the clusters cut short or dropped are counted (`Render3DStats::cluster_overflow`, with `stats_readback`) and said once in the log.
  I did not take vertex pulling with a packed copy id: it would need new vertex shader variants for four passes and custom material
  shaders, and gives up the hardware vertex cache's index reuse for nothing the scan does not already give.
- *Which passes*: main, pre-pass, velocity and pick share view 0 (the camera's); each cascade (or the single map) is a view of its own with its frustum
  and slice tests as the CPU's (`ClusterViews::views`); a copy only a shadow map sees is cut for that map alone.
- *Read-back*: `Lod3D::stats_readback` and `Renderer3D::read_cluster_picks` submit the frame so far (as `pick` does) and read the counters / the scan
  with a separate command buffer and a fence of its own, released after it signals (`GpuDevice::read_buffer`). The frame fences and the
  GPU timer are not touched. It is synchronous: there is no read-back one frame late (it would need a fence for the frame's command buffer, which the 2D
  backend owns), so it is for tools and tests. Normal frames have none.
- *Fallback*: without compute storage buffers and index / indirect buffers that compute writes (checked by creating them; `KIN_GPU_CLUSTERS=0` turns it off), or when the
  scene has more than 60 M cluster x view elements, or a buffer cannot be made, the CPU cut of G3b runs (one line in the log).


**Tests** (`kin_render3d_tests`, NVIDIA RTX 4080 and lavapipe; 68/68 ctest, 65/65 without 3D, `kin_example_tests`). The G3b tests run on both cuts
(GPU with the read-back). New: `test_gpu_cut_equals_cpu_selection` (240 copies' cuts over a smooth hill, a flat sphere, a torus and a flat plane, perspective, orthographic,
near, far, inside, scaled and turned, forced depths: **8,304 clusters compared, 0 differ**, and the open edges of every chosen set equal the mesh's own; the rounding of a `sqrt`
or a divide could flip a borderline cluster, none did in these); `test_gpu_cut_picture_equals_cpu` (byte-identical pictures with 1, 3 and 4 cascades, occlusion and contact shadows
on or off, copy culling on or off; pick on a grid; 14 frames of a walking camera with temporal anti-aliasing; 62 copies, serial and on workers); `_is_deterministic` (6 renders in
two renderers byte-identical); `_culls_by_pass` (a copy off to the left draws nothing for the camera, its shadow still falls into view; the indirect arguments of empty runs are 0);
`_overflow_clamps` (rooms of half the cut, 600, 100, 1 triangles: clusters dropped and counted, nothing past the room, the picture degrades, with room of one triangle it equals the scene without the mesh, and
with room again equals the first picture); `_falls_back` (`KIN_GPU_CLUSTERS=0`: capability false, the CPU cut, the same picture); `_many_copies` (64 copies of 99,904 triangles: the CPU cut's counts and picture,
`render()` 0.07 ms); `_chunks` (12 meshes in two chunks, copies drawn A, B, A: CPU cut's picture, 8 dispatches).
**Mutations**, each made, seen to fail and reverted: parent tested at the cluster's own sphere (cut test), the run start one triangle off (cascade frame equality), indirect count not written for empty runs
(`_culls_by_pass`, only after adding the read of the arguments: the stale draw lies outside the view, so no picture shows it), a cascade tested against the camera's frustum (walking camera with cascades), scale ignored (two-scale test), run end not held to the room (room of one triangle),
camera frustum not tested (counts), indices placed in queue order instead of the scan's (forced leaves), wrong view bit for the single map (shadow equality), forced depth ignoring the parent, a mesh's range in its chunk ignored,
the gather's copy found by the wrong base. All 12 caught. Not caught by design: nothing writes out of bounds in the tests I can see (the room clamp is checked by counters and pictures, not by a GPU validation layer).

**Off path.** `g3c_before_def` / `_all` / `_cl` from the G3b binary, again after: every PNG byte-identical and every draws/triangles/casters line equal, except `crowd/above` under `_all`
(quiet, three runs of each binary: the old one flipped once in three, the new one never; the known TAA flip). The CPU cut (`--lod-cpu`) and the GPU cut give the same kiln_near pictures as before G3c (one pixel of kiln jitter at `grazing`).

**Kiln, 1 px, 1280 x 720, medians of 3 runs** (GPU ms / CPU `render()` ms; triangles; picture against full detail is as in G3b, identical for the CPU and GPU cut: dome_close 0.25 mean levels / 50.5 dB):

| Scene | Full | Levels 6 | CPU cut (G3b) | **GPU cut** |
|---|---|---|---|---|
| kiln_near dome_close | 5.15 M, 4.37 / 0.016 | 2.67 M, 2.42 / 0.015 | 134 k, 0.38 / 0.540 | **134 k, 0.42 / 0.031** |
| kiln_near grazing | 5.15 M, 4.30 / 0.014 | 2.60 M, 2.17 / 0.012 | 87 k, 0.25 / 0.424 | **87 k, 0.29 / 0.032** |
| kiln_near far | 5.15 M, 4.33 / 0.029 | 166 k, 0.27 / 0.011 | 134 k, 0.29 / 0.395 | **134 k, 0.27 / 0.032** |
| kiln game | 5.15 M, 4.28 / 0.016 | 168 k, 0.31 / 0.014 | 169 k, 0.37 / 0.473 | **169 k, 0.35 / 0.030** |
| kiln close | 5.15 M, 4.42 / 0.027 | 192 k, 0.48 / 0.016 | 308 k, 0.88 / 0.865 | **308 k, 0.58 / 0.032** |

Everything on (`--cascades 4 --ao --contact-shadows --taa`): dome_close 6.80 / 4.07 / 1.28 / **1.02** GPU ms (full / levels / CPU cut / GPU cut), grazing 6.53 / 3.64 / 0.89 / **0.78**, far 6.23 / 0.65 / 0.65 / **0.50**,
game 6.21 / 0.69 / 0.69 / **0.60**, close 6.83 / 1.08 / 1.19 / **1.03**; CPU `render()` 0.044 to 0.048 ms against 0.42 to 0.95 for the CPU cut. With one shadow map the GPU cut costs 0 to 0.05 ms of GPU more than the
CPU cut (the compute: cull, three scan dispatches, arguments, gather; measured first by skipping passes: cull + scan about 0.07 ms, the gather's first version 0.14 ms before I queued the chosen clusters instead
of launching a workgroup for every (view, cluster)); with cascades it is cheaper than the CPU cut (4 cascades with a draw a copy there, one indirect multi-draw here). Frame GPU time is that of the whole frame, so these are on/off deltas, not scopes.
**Sponza and others** (GPU ms / CPU ms; cut CPU / GPU): nave 0.52 / 0.456 against **0.38 / 0.125** (no clusters: 0.36 / 0.112); roof 0.39 / 0.332 against **0.33 / 0.085** (0.29 / 0.073); helmet front 0.24 / 0.238 against
**0.19 / 0.031** (0.17 / 0.017); everything on, nave 1.94 / 0.52 against **1.43 / 0.26**. Crowd unchanged (its meshes have no clusters: 0.19 to 0.27 CPU, 1.24 / 0.72 / 2.0 GPU with or without). Before chunking Sponza's GPU cut cost
1.57 ms (see Design).
**Memory.** Kiln hierarchy in video memory **124.8 MB** (117.9 MB of indices for all levels, 7.0 MB of table) against 58.9 MB of leaf indices before; the gathered indices 48 MB at the default room (4 M triangles, sized down to the scene when the scene's own bound is smaller;
the kiln's worst, 5 views of 300 k, uses a tenth). Peak RSS of `kin_reference` on kiln_near: **1,559 MB with the GPU cut, 1,559 MB with the CPU cut** (1,557 before G3c): no host cost (the upper levels' indices are written into the upload
memory in 8 MB pieces). Sponza 607 MB (CPU cut 600, full 583). Load is the same as G3b's (5 to 7 s; the table and indices are written during the same pass).

**What I looked at.** kiln_near dome_close, the GPU cut against full detail and the x8 difference: the dome's curvature and shading read the same, the difference is a faint speckle of facet shading over the dome (max 65 levels in a few hundred pixels,
679 pixels over 8) and a lighter line along the ring and the dome's silhouette, no cracks or holes at any cluster border; the GPU cut's picture is the CPU cut's byte for byte, so what G3b saw of seams applies. I did not re-run a 60-step dolly (popping is a property of the cut, which is
the same), look at a camera inside the model, or look at a Sponza-with-clusters frame.

**Where it loses, and defaults.** The GPU cut is the default where the device can; it costs video memory (the hierarchy is twice the leaves' indices, plus the gathered buffer) and one more
thing to go wrong on a driver (lavapipe passes). It loses on nothing I measured once meshes share chunks; a scene with a handful of clustered copies of one tiny mesh pays 6 compute passes (about 0.05 ms) for what the CPU would do in microseconds, and the cluster
cut itself still loses where it did in G3b (Sponza, helmet: small meshes) -- clusters stay off by default, the recommendation of G3b stands: single meshes of hundreds of thousands of triangles seen near and far.

**Honest limits and not tested.** No read-back one frame late (synchronous only, for tools and tests); `draws` and `shadow_draws` count multi-draws, and `clusters_drawn`, `triangles`, `cluster_overflow` are 0 without `stats_readback`; overflow is only logged with it. The default room
(4 M triangles) is not clamped to the device's memory; a scene whose passes together choose more is cut short. A mesh's range in a chunk is never reused. No normal-cone test, no per-pass level-of-detail distance. Without the read-back an overflow is silent. D3D12 and Metal untested (indirect first-instance, storage buffers that are also index buffers);
Vulkan on NVIDIA and lavapipe only. Sponza Walk's panel and `--preset clusters-gpu` compile and the help lists them, but I did not see them run (a headless run printed nothing I could check), so the click path and the stats lines are untested.

#### G3d — Occlusion culling. Done (opt-in, off by default: it pays only where much is hidden).

A depth pyramid (Hi-Z) from the last frame's depth (reprojected by the
camera's motion; a conservative bound, or two passes as the standard
technique: draw what was visible, test the rest against the new depth,
draw those that pass), tested in the same compute passes as G3c.
Worth it only where much is hidden: the kiln's inside, Sponza's galleries,
the deck seen from the side. Checks: those, with the picture the same to
within a pixel of edge and no pop-in when the camera turns; the cost of the
pyramid against what it culls. May be cut if it saves nothing measurable.

#### G3d — results

Built, tested on the RTX 4080 and on lavapipe, and **measured not to pay except where a great deal is hidden**: kept as an opt-in (`Lod3D::occlusion`, off by default), not cut,
because it does pay where the content is dense and hidden (a wall hiding 256 heavy copies: 12 to 19% of the frame) and never changes a picture; on the kiln and Sponza,
the scenes the plan named, it only costs. The verdict and where it pays are at the end of this section.

**Design, as built.** `Lod3D::occlusion {enabled, depth_margin, pixel_margin}` (off by default; needs `lod.enabled`, `gpu_culling`, a clustered mesh and
`Renderer3DCapabilities::occlusion_culling`, else ignored with one line in the log). The standard two-pass method, with the first pass's depth made for it:

- *Which depth, and why.* Last frame's depth reprojected (a) was rejected before any code: a cluster that was hidden last frame and shows now (a door opening, the
  camera coming round a wall, a moving copy) would be culled for a frame, and an occluder that moved away would hide what it no longer hides. A coarse
  occluder-only pass (c) has the same flaw as (a) for the occluders left out. The two-pass method (b) tests every cluster against **this frame's own**
  depth, so it never culls what shows, whatever the history holds. The history (one word a cluster of each copy of each clustered draw, kept in two buffers
  swapped each render, per view) only decides which clusters are drawn *before* the pyramid is built: the ones that showed last frame. A history that is wrong
  (a camera cut without `reset_history()`, a changed scene) costs speed, never a picture. A new layout of the scene's clustered draws, `reset_history()` and a
  resize start it empty: the first pass then draws nothing and the second tests everything against the frame's plain meshes.
- *The frame.* `cluster_cull_occ.comp` (the same source as `cluster_cull.comp`, built twice: `cluster_cull.inc.glsl` with and without `KIN_OCCLUSION`) cuts
  and frustum-tests as before. **Pass 1** (all views; the camera's view only for clusters in the history) gathers into set 0 of the cut's buffers, which the
  shadow passes read as before (shadows are **not** occlusion culled: a caster hidden from the camera still shadows what shows). Then the depth: with a
  pre-pass (ambient occlusion, contact shadows or temporal anti-aliasing on) the pre-pass is made in two halves, the opaque meshes and the first pass's clusters,
  then (the pyramid built, pass 2 recorded) the second pass's clusters, loading the first half's depth and normals: no geometry is drawn twice. Without a pre-pass a
  depth-only pass of the same (the shadow shaders, a cut-out's discard included, into a single-sample depth) is the price, because the multisampled main depth
  cannot be sampled. **The pyramid**: `hiz_depth.comp` (4 x 4 pixels a texel, the farthest depth, which in reverse Z is the smallest; R32F) and
  `hiz_reduce.comp` (four levels a pass through shared memory) down to 2 x 2: three compute passes at 1280 x 720 (9 levels). **Pass 2** (set 1, the camera's view
  alone) re-cuts every cluster, and for those in the cut and the frustum projects the cube round its bounding sphere (the 8 corners; a corner at or behind the camera
  plane says "not hidden"), takes the nearest corner depth against the farthest depth of the pyramid's texels under the rectangle (the rectangle widened by
  `pixel_margin`, the level chosen so that it covers at most 2 x 2 texels), and keeps the cluster unless it is behind by `depth_margin`; the kept ones not
  drawn in pass 1 are gathered; the history for the next frame is written (kept or not, for all). The main pass, the pre-pass's second half, the velocity
  pre-pass and the pick pass draw both sets (so a copy hidden by the wall is no pick result); the overlay is whole in pass 1 and never tested.
  With the jittered projection of temporal anti-aliasing both the depth and the test use it. A mesh with a shader of its own may discard where the
  pyramid would count it solid: it is no occluder (drawn after the pyramid).
- *Cost model.* Fixed: 3 pyramid passes, a second cut (cull, 3 scan, arguments, gather: 6 compute passes), the occluder render pass without a pre-pass (which
  draws what showed last frame again), a second multi-draw a draw. Saved: the raster of what the wall hides. Not done: a test of whole copies (the second pass tests
  clusters; a copy-level test would save the cut's work for hidden copies, not their raster), occlusion culling of the shadow maps (they would need the light's
  own pyramid), per-copy GPU culling of copies drawn from CPU instance lists (the crowd: each copy would need its own test, a different design).

**Scenes.** New in `tests/reference/scenes`: `hidden.json` (`procedural: "hidden"`: a floor, a wall 20 m wide and 8 high, 8 tori in front and 64 behind it, 8 x 8, each torus 100,352 triangles in a
clustered mesh; cameras `occluded` (all 64 behind the wall: the best case), `peek` (from beside the wall's end: some show), `open` (from behind the grid: nothing hidden by the wall, the cost of the
test alone)), `hidden_dense.json` (the same with 16 x 16 = 256 copies), `kiln_inside.json` (local: the kiln with cameras inside the dome and behind it); `kin_reference --lod-occlusion`
(and `lod: {occlusion: true}` in a scene file; `--readback` prints the clusters of the first pass, occluded, and drawn for nothing), Sponza Walk's "Occlusion culling" switch, stats lines and
`--preset occlusion`. The existing scenes are untouched.

**Clusters and triangles** (the camera's cut, read back; clusters / triangles, off then on; 1280 x 720):

| Scene / camera | Off | On | Occluded |
|---|---|---|---|
| hidden occluded | 3,135 / 300,033 | 257 / 24,691 | 2,878 clusters, 275,342 triangles (92%) |
| hidden peek | 2,976 / 285,337 | 423 / 41,561 | 2,553 (85%) |
| hidden open | 3,126 / 299,119 | 2,773 / 265,352 | 353 (11%: the tori's own far sides) |
| hidden_dense occluded | 10,921 / 1,044,291 | 257 / 24,691 | 10,664 (98%) |
| hidden_dense peek | 9,314 / 890,968 | 68 / 6,672 | 9,246 (99%) |
| hidden_dense open | 7,096 / 679,654 | 6,435 / 619,669 | 661 (9%) |
| kiln_near dome_close | 1,312 / 134,453 | 363 / 37,955 | 949 (72%) |
| kiln_near grazing | 850 / 87,334 | 275 / 29,085 | 575 (68%) |
| kiln_near far | 1,395 / 134,382 | 805 / 77,238 | 590 (42%) |
| kiln_inside inside_low | 628 / 63,068 | 447 / 45,886 | 181 (29%) |
| kiln_inside inside_up | 207 / 22,197 | 199 / 21,357 | 8 (4%) |
| kiln_inside behind | 2,671 / 262,654 | 780 / 76,872 | 1,891 (71%) |
| sponza nave (clusters) | 1,365 / 146,213 | 1,192 / 127,234 | 173 (13%) |
| sponza roof (clusters) | 581 / 62,924 | 572 / 62,106 | 9 (2%) |

"Drawn for nothing" (drawn in the first pass, hidden this frame) is 0 in every static view. The identity `clusters_drawn + clusters_occluded` = the cut's clusters in view holds exactly.

**GPU ms of the frame, off / on** (medians of 4 interleaved pairs of runs of 120 frames; `*`: the ranges of the two do not overlap; the other GPU users on this machine drifted the
runs by up to 10%, so differences under about 0.05 ms are noise, and the `best` column of the log, minimum against minimum, agrees with these to 0.03 ms; CPU `render()` ms in brackets):

| Scene / camera | Plain (MSAA only) | AO on (a pre-pass exists) | Everything on (4 cascades, AO, contact, TAA) |
|---|---|---|---|
| hidden occluded | 0.29 / 0.31 (+0.02) [0.08 / 0.14] | 0.75 / 0.74 (-0.01 `*`) | 1.19 / 1.22 (+0.03) |
| hidden peek | 0.27 / 0.35 (+0.09 `*`; +0.03 in an earlier run) | 0.77 / 0.75 (-0.02) | 1.26 / 1.23 (-0.03) |
| hidden open | 0.39 / 0.53 (**+0.14 `*`**) | 0.92 / 0.88 (-0.04, noisy; `best` +0.05) | 1.18 / 1.25 (+0.07) |
| hidden_dense occluded | 0.67 / 0.56 (**-0.11 `*`**) [0.12 / 0.17] | 1.19 / 0.97 (**-0.22 `*`**) | 1.80 / 1.58 (-0.23) |
| hidden_dense peek | 0.59 / 0.50 (-0.09 `*`) | 1.14 / 0.98 (-0.15 `*`) | 1.72 / 1.51 (-0.21 `*`) |
| hidden_dense open | 0.69 / 0.87 (**+0.18 `*`**) | 1.39 / 1.40 (+0.00) | 1.68 / 1.75 (+0.07) |
| hidden_dense at 1920 x 1080, occluded / peek / open | 0.84 / 0.74 (-0.10 `*`), 0.75 / 0.64 (-0.11 `*`), 1.05 / 1.21 (+0.16 `*`) | | |
| sponza nave, clusters | 0.38 / 0.47 (**+0.08 `*`**) [0.11 / 0.16] | | 1.22 / 1.18 (-0.03) |
| sponza roof, clusters | 0.29 / 0.39 (+0.10 `*`) | | 1.04 / 1.07 (+0.03 `*`) |
| kiln_near dome_close / grazing / far | 0.40 / 0.44, 0.28 / 0.31 `*`, 0.27 / 0.31 `*` (+0.04, +0.04, +0.04) [0.03 / 0.06] | | 1.00 / 1.12, 0.74 / 0.88, 0.50 / 0.59 (+0.12, +0.14, +0.09) |
| kiln game / close | 0.35 / 0.39 `*` (+0.04), 0.55 / 0.52 (-0.04) | | 0.61 / 0.62, 1.13 / 1.10 |
| kiln_inside inside_low / inside_up / behind | 0.27 / 0.35 `*`, 0.17 / 0.25 `*`, 0.42 / 0.43 (+0.08, +0.08, +0.01) | | 1.20 / 1.29, 1.01 / 1.10, 0.90 / 0.92 |

**The cost, taken apart** (hidden_dense `open`, nothing to cull, 1280 x 720, no pre-pass, +0.11 in that run; by skipping a stage at a time, which makes the picture wrong, the pyramid's share from the smaller scene): the extra render pass of the first pass's clusters and the frame's
opaque meshes **0.05 ms** (the main pass's own raster, drawn a second time, because the multisampled main depth cannot be sampled), the second cut (cull, three scan passes, arguments, gather) and the
second multi-draw **about 0.04**, the three pyramid passes **0.015**. CPU `render()` +0.03 to 0.06 ms for the extra recording. With a pre-pass the first item is gone (the pre-pass is the
depth), and the worst case falls to +0.00 to +0.07. The saving, taken apart (hidden `occluded`, 275 k triangles of the camera's cut hidden): the camera's raster of them is about 0.07 ms of a frame that is 0.26 ms; the shadow pass still draws
every caster the sun sees, hidden from the camera or not (the 64 tori's shadows fall into view), so what is left of `hidden_dense occluded` after culling 1 M triangles of the camera's cut is
the shadow map's million.

**Memory.** Video memory: the second pass's gathered indices (12 bytes a triangle of `max_cluster_triangles`: 48 MB at the default 4 M triangles, as the first set's), the pyramid 0.4 MB at 1280 x 720, the history
8 bytes a cluster of every copy (kiln 0.7 MB, `hidden_dense` 3.6 MB), the occluder depth without a pre-pass 3.7 MB; none of it is made until a render asks for occlusion. Peak resident set of `kin_reference`
over `kiln_inside` and `kiln_near` with clusters: 1,861 MB off, 1,866 MB on (the history and the second set are on the GPU).

**Off path.** `g3d_before_def` / `_all` / `_cl` from the G3c binary, again after (the new binary, `Lod3D::occlusion` off): every PNG byte-identical and every draws / triangles / casters / cut line equal, except
`crowd/above` under `_all`, which flipped under the other sessions' load in both directions (the old binary's 07ea6cc4, the new one's 1d993af4 in that batch) and which is 6ce1cac0 in all six quiet runs, three of each binary: the known TAA flip with 20,000 copies, not this
change. Nothing of the occlusion path is made or run with the setting off (no pyramid, history, pipelines' use, second buffers or passes); `cluster_cull.comp` now includes `cluster_cull.inc.glsl` and
compiles to the same code.

**Tests** (`kin_render3d_tests`: 8 new, NVIDIA RTX 4080 and lavapipe; ctest 68/68 and 65/65 without 3D). Occlusion on against off, picture and picks byte for byte: 1 and 4 samples, 3 cascades, AO and contact shadows,
temporal anti-aliasing, all of them together (6 configurations x 3 to 8 frames, pick grid of 234 pixels); a camera strafing past the wall's end in 60 steps (the balls behind it come into view from step
29: **0 bytes differ in every frame**), another turning on the spot in 60 steps (0 bytes), a ball moving out from behind the wall and back (0 bytes); a slit in the wall of 0.9, 0.45, 0.25, 0.12 m
(8 to 1 pixels: 0 bytes, with 1 and 4 samples) and of 0.06 and 0.02 m (0.5 and 0.17 pixels: 0 bytes with 1 sample; **78 bytes of 57,600 with 4 samples at 0.5 pixel**, a sliver the multisampled picture
sees through and the single-sample depth does not, which is why it is measured and not asserted); balls at the wall's edge, at the screen's edge, half through the floor and the wall, the overlay;
the shadows of balls hidden from the camera still fall into view (1 and 3 cascades); the history empty after `reset_history()`, after a resize and after a change of the scene's draws, and a camera cut without a reset giving the
right picture; 20 frames twice from fresh views identical; ignored (with its one line) with `KIN_GPU_CLUSTERS=0`, with the selection off, with `gpu_culling` off, and with no clustered mesh;
a room too small for the gathered indices degrades as G3c's does, counted. Counts: clusters occluded > 40 behind the wall, **0 on a gently wavy sheet seen from above** (a closed ball hides its own far
side: 6 of 145 in the open), `clusters_drawn + clusters_occluded` equal to the clusters the cut puts in view without occlusion, in every configuration. Mutations, each made, seen to fail and reverted:
the pyramid built with the nearest depth instead of the farthest (strafing: 32 bytes), only 2 x 2 texels read at a level too fine (66 bytes), the rectangle's rows flipped (the first scene), pass 2 drawing nothing (9,309 bytes),
the shadow views following the camera's history (5,713 bytes), the history kept across a resize (the history test), the pick pass drawing only the first set (40 picks), the nearest depth taken from one corner of the cube
(only after adding the balls through the floor and the wall: 520 bytes; the first set of tests passed it, so the tests were not good enough), a level too coarse (safe, so no picture changes; caught only by the counts,
49 to 30 clusters occluded). **Not caught: the margins** (`depth_margin` and `pixel_margin` set to 0): they guard rounding that none of these scenes produce; I could not construct a case, because any geometry inside
the cube's bounding box is behind its nearest corner by at least the cube's size.

**What I looked at.** `hidden` occluded, peek and open with and without occlusion at 1280 x 720: identical (the difference is zero); the three views read as a floor with shadows and tori in front of the wall, the wall's
edge with the hidden tori's tips showing past it from the side, and the grid of tori from behind. The strafe's frames around the reveal (frames 24 to 33 of the test at 160 x 120, off, on and the difference x8): a blue ball appears at the wall's
edge in the same frame in both, the difference is black in every frame; likewise 5 frames of the turn. A Sponza Walk headless run (`--preset occlusion`, nave, 20 frames) draws its stats lines (9 pyramid levels,
3 dispatches, 1,207 clusters in the first pass, 202 occluded) and the same picture byte for byte as `--preset clusters-gpu`. I did not look at frames of the kiln's inside by eye (the numbers say 29% of its camera clusters are hidden
there and the frame gets 0.08 ms slower).

**Verdict.**
- *Where it pays:* a scene where a large share of the cut's triangles are hidden and the cut itself is big: `hidden_dense` (a wall hiding 256 copies of a 100 thousand triangle mesh; 1 M triangles in the camera's cut, 25 thousand left): **-0.09 to -0.11 ms of 0.6 to 0.8 (12 to 16%)**
  at 1280 x 720 and 1920 x 1080, **-0.15 to -0.23 ms (12 to 19%)** with a pre-pass. Even there the shadow maps still draw it all, which caps the saving.
- *Where it costs:* where little is hidden: `hidden` open **+0.14**, `hidden_dense` open **+0.18** (with a pre-pass: -0.04 to +0.07); and where much *is* hidden but the cut is already small: the **kiln** (-42% to -72% of the camera's triangles,
  yet +0.04 to +0.08 ms, with everything on +0.09 to +0.14) and **Sponza with clusters** (+0.08 to +0.10; 13% of the camera's clusters hidden in the nave: its galleries hide too little). The crowd has no clustered meshes: unchanged.
- *Why:* the clusters' cut already holds the raster to about a triangle a pixel, so what a wall can hide is bounded by the screen (a few hundred thousand triangles, 0.05 to 0.1 ms); the method's fixed work is 0.05 to 0.1 ms
  (a second cut, a pyramid, the depth of the first set drawn again where no pre-pass makes it). It wins only where the hidden content is many times the screen's worth of triangles in the *cut* (hundreds of copies of heavy meshes, a city or
  a mine behind its walls), which the clusters' cut makes rare.
- *Keep or cut:* kept, **off by default**, for scenes of that kind and views with a pre-pass; it is correct everywhere it was tried (no picture ever differs, no popping), and costs nothing when off. If the plan wants fewer moving
  parts, it is the first thing to cut: the three things that would change its balance are a pyramid for the shadow maps (they are what remain), a test of whole copies before their clusters (the second cut's cost grows with the copies' clusters),
  and, for views with one sample, drawing the first set into the main target instead of a depth of its own.

**Honest limits and not tested.** Not done: occlusion of the shadow maps (hidden casters still draw and shadow), a test of whole copies, occlusion of copies drawn from CPU instance lists (`draw_instanced` of meshes
without clusters, the crowd: each copy would need its own test on the GPU, a different design), a first pass without the duplicate depth for views with one sample. The pictures are not exact for a slit narrower than a
pixel seen with MSAA. A mesh with a shader of its own is never an occluder (its clusters are drawn after the pyramid). Depth formats: the pyramid reads the pre-pass's D32F (or D24, quantised; the margin
covers it at ordinary distances, untested). D3D12 and Metal untested; Vulkan on NVIDIA and lavapipe only. Sponza Walk's panel click path cannot be tested here; its `--preset occlusion` and stats lines were run headless. A history is per view and kept across frames; two views of one scene each have their own.

Checks across G3: the kiln and the crowd against full detail and against
before (the plan's own check), on the G0 harness with a new label a step;
`truth` is not re-rendered (the pictures are judged against our own full
detail, as the error is ours to bound).

#### G3 summary

What each step costs, what it gains, its default, and when to use which (RTX 4080 Laptop, 1280 x 720, 4x MSAA; the kiln is one 5.15 M triangle mesh, the crowd 20,000 small meshes,
Sponza 260 k triangles in 103 meshes; numbers from each step's results):

| Step | Costs | Gains | Default | Use it for |
|---|---|---|---|---|
| **G3a** levels of detail (`MeshOptions::levels`, `Lod3D`) | build time (kiln 8 s for 4 levels) and memory (+1.1 GB resident, all levels' triangles on the GPU); a draw a level; the crowd with everything on is 8 to 11% slower | kiln 4.3 to 0.3 (6 levels) or 0.8 ms (4); the crowd 0.4 to 1.1 M of 1.5 to 2.6 M triangles, GPU level; picture within 0.1 to 0.2 mean levels | off; meshes keep one level unless asked | a big mesh seen from far (whole-mesh levels), many copies of meshes of thousands of triangles; not small meshes |
| **G3b** clusters, CPU cut (`MeshOptions::clusters`) | 5 s and 1.5 GB at load for the kiln; the cut on the CPU 0.4 to 0.9 ms of `render()` | kiln 4.3 to 0.25 to 0.9 ms GPU: near parts fine, far parts coarse inside one mesh; no cracks | off | one huge mesh (hundreds of thousands to millions of triangles) seen near and far; not Sponza or a helmet |
| **G3c** the cut in compute (`Lod3D::gpu_culling`) | 62 MB of indices and 7 MB of table for the kiln, the gathered buffer (48 MB), about 0.05 ms of GPU for six compute passes | `render()` 0.4 to 0.9 ms down to 0.03 to 0.08; GPU level with the CPU cut, cheaper with cascades; the picture is the CPU cut's byte for byte | on, wherever clusters and the selection are on and the device can | always with G3b |
| **G3d** occlusion culling (`Lod3D::occlusion`) | 0.05 to 0.18 ms of GPU where little is hidden (0 to 0.07 with a pre-pass), 0.03 to 0.06 ms of CPU, +48 MB (the second gathered buffer) | 0.1 to 0.23 ms (12 to 19%) where a wall hides hundreds of heavy copies; the camera's triangles down 70 to 98% on the kiln and the dense scene; the picture is the one without it | off | hundreds of heavy clustered copies behind walls, with ambient occlusion or temporal anti-aliasing on; not the kiln, not Sponza |

### Opt-in speedups, phase 1 — results

Phase 1 of a speed-up pass over the opt-in features (TAA, AO, contact shadows, cascades, occlusion, clusters): changes that keep every picture **byte for byte**. Labels
`sp1_before_*` (the tree at `b2b7359`: `def`, `all` = `--cascades 4 --ao --contact-shadows --taa`, `taa`, `ao`, `cs`, `c4` over crowd, shadows, sponza, helmet, motion, spheres, cornell;
`cl` and `occ` = kiln_near and hidden_dense with `--lod --lod-clusters` (and `--lod-occlusion`); `cltaa` = the same with `--taa`), each picture compared with `cmp` after every item. Timing:
interleaved pairs of runs of the old and the new binary (120 frames each, medians of the cameras' medians over 3 to 5 runs; a run waits for an idle CPU first, because other projects
on this machine load it to a load average of 20 to 35 at times and that moves CPU numbers by 2x and GPU numbers by 10 to 20%); `render()` CPU ms in the second column group. Differences under
about 0.05 ms GPU, or 0.01 ms CPU on the small scenes, are noise.

**Superseded: these flakes were found and fixed afterwards (a draw sorted by heap address; an environment mip level nothing wrote), see "The intermittent green pixels — investigation"; every other mention of the "known flake", "the kiln jitter" or "the TAA flip" in this plan is the same pair.** Known flakes, seen in the **old** binary too and not caused by these changes: `crowd/above` under TAA flips between two pictures (1 of 2 full old-binary runs, 1 of 3 new-binary runs; crowd alone, 5 runs
of each: never), and `kiln_near/grazing` flips between two pictures (old binary 2 of 6 runs, new 3 of 8; under `--taa` too, together with `kiln_near/far`). Every other picture of every configuration was identical in every run. (A first version of the
shader split changed `mesh.vert`'s SPIR-V by hoisting a `const uint at` out of an expression, and `kiln_near/grazing` then differed twice in a row; with hindsight that may have been this flake
or the rounding. The shaders were arranged so that `mesh.vert` and `mesh.frag` compile to the **same SPIR-V as before**, checked with `glslc` and `cmp`, and the velocity variants are separate files: keep it so.)

#### 1. TAA velocity from the main pass

With TAA on and nothing else that needs a pre-pass (no AO, no contact shadows, no occlusion culling, no opaque mesh with a custom shader), the main pass writes the velocity itself into a second
colour target (`mesh_velocity.vert/.frag` = `mesh.vert/.frag` through `kin_mesh_vertex.inc.glsl` / `kin_mesh_fragment.inc.glsl` with `KIN_VELOCITY`), and the separate velocity pre-pass (a whole
second geometry pass, with its own depth) is gone. `Render3DStats::ao_draws` is then 0 and `taa_main_velocity` is true.

- *Same bits.* The velocity expression is `kin_velocity()` moved to `kin_velocity.inc.glsl`, which both the pre-pass and the main pass include; the vertex shader copies `prepass_velocity.vert`'s
  four lines for `vCurrent`/`vPrevious`/`vFresh`. The target is the same RGBA16F texture the pre-pass wrote (`_ao_normals`; velocity in B and A, R and G are 0, nothing reads them in TAA-only), cleared
  to 0 as before, so `taa.comp` is unchanged. Result: **every TAA picture is byte-identical** to the pre-pass's (below).
- *Rules kept.* Opaque and cut-out (`Blend3D::Mask` discards in `mesh.frag` as before) write it; alpha, additive and wireframe pipelines have a colour write mask of 0 on the second target (a new
  `extra_format`/`extra_write` in `GpuPipelineKey`), so they leave the velocity of the opaque mesh behind them, as before. The overlay draws in the same pass with its own cameras (`current`/`prior`
  per scene in the draw block) and overwrites the world's velocity where it covers, as its separate pass did. Clustered draws (CPU and GPU cut), LOD levels and instanced runs go through the same
  `draw_cluster_indirect`/`draw_cluster_copies` code with a longer uniform block (`VelocityUniforms` now starts with `DrawUniforms`' 80 bytes, so a pipeline with the plain `mesh.vert` reads it too).
- *Custom shaders.* A mesh with a shader of its own (`Material3D::shader`) has no velocity output (its fragment shader comes from `kin_material.inc.glsl`, which declares `FragColor` only). A frame with an
  **opaque** custom-shader mesh keeps the old pre-pass (decided per frame in `prepass()`); a **blended** custom shader draws in the main pass with the second target masked. Tested: both, against the
  pre-pass, and a mutation that skips the fallback crashes the test (a pipeline with the wrong storage buffers). `KIN_TAA_PREPASS=1` forces the old path (read on each render: tests, A/B timing).
- *Targets.* The pre-pass depth (D32, 3.7 MB at 1280 x 720) is no longer made for TAA alone; `_ao_normals` (RGBA16F, 7.4 MB) is.

Byte comparison, new tree against `sp1_before_*`: see the table at the end of this section (all pictures of all eight configurations).

GPU ms / CPU `render()` ms, TAA on, old binary then new (medians of interleaved pairs):

| Scene | Old GPU | New GPU | Change | Old CPU | New CPU |
|---|---|---|---|---|---|
| crowd (20,000 copies; first_person / mid_deck / above) | 2.36 | 1.86 | **-0.50** (-0.62 / -0.36 / -0.51) | 0.385 | 0.376 |
| sponza (nave / roof) | 0.72 | 0.54 | **-0.18** (-0.24 / -0.11) | 0.181 | 0.155 |
| kiln, 5.15 M triangles, no LOD | 7.17 | 4.92 | **-2.25** | 0.039 | 0.033 |
| kiln, clusters | 0.64 | 0.54 | -0.10 | 0.065 | 0.059 |
| kiln_near, clusters | 0.50 | 0.42 | -0.08 | 0.064 | 0.058 |
| hidden_dense, clusters | 0.84 | 0.71 | -0.13 | 0.090 | 0.083 |
| small scenes (cornell, helmet, shadows, spheres; two earlier batches) | 0.17 to 0.30 | -0.01 to -0.02 | | 0.02 to 0.06 | 0.01 to 0.04 (-0.005 to -0.03) |

Nothing got slower in either kind of scene; the gain is the whole second geometry pass (TAA's cost on the kiln was +1.4 ms in G2d with a single sample at that time, and here is more than 2 ms of 7).
The CPU numbers on the crowd are dominated by the instance matching and the culling workers (unchanged); recording one geometry pass fewer saves 0.01 to 0.03 ms on Sponza.

#### 2. One blur for ambient occlusion and contact shadows

With both on and the occlusion not at half resolution, the two results have the same size and the same depth weights, so `ao_blur_both.comp` filters both inputs and writes both outputs in one dispatch a
direction (2 dispatches where there were 4; `Render3DStats::blur_merged` = 2, and `ao_dispatches` and `contact_dispatches` still count the dispatches each feature is in, 3 each). The GTAO and
the contact march run first, then the two blurs. Everything else keeps the separate path (half-resolution occlusion, one feature alone; `KIN_BLUR_SEPARATE=1`, read on each render, forces it, for the test and
for timing). The weights are worked out once and applied to both sums, with the same operations in the same order as `ao_blur.comp`, and the results are **byte-identical** (every picture of
`all`, `ao`, `cs` and the rest; and the new test compares the merged picture with `KIN_BLUR_SEPARATE=1`'s to the byte).

Ping-pong: the blur goes raw, across, raw (the second dispatch writes over the raw result, which nothing reads again), so the third R8 texture of each feature is gone: `_ao` and `_contact` no longer
exist and `_ao_raw` / `_contact_raw` hold the results (the main pass samples them). 0.9 MB less a feature at 1280 x 720, 8.3 MB less at 4K. Identical pictures.

Timing: the same binary, merged against `KIN_BLUR_SEPARATE=1`, AO + contact shadows on, interleaved pairs (7 runs of the small scenes, 5 of the big ones; GPU ms, median of the cameras' medians, and in
brackets the median of the runs' p10, which the GPU's clock drift disturbs less); CPU `render()` ms:

| Scene | Separate | Merged | Change | CPU change |
|---|---|---|---|---|
| crowd | 3.07 | 2.93 | **-0.14** (p10 -0.30) | -0.002 |
| sponza | 1.79 | 1.74 | -0.05 (p10 -0.10) | -0.001 |
| shadows | 1.23 | 1.07 | -0.16 (p10 -0.15) | -0.001 |
| helmet | 0.43 | 0.40 | -0.03 | -0.001 |
| kiln_near, clusters | 0.68 | 0.65 | -0.02 (p10 -0.04) | -0.001 |
| hidden_dense, clusters | 1.34 | 1.25 | -0.08 (p10 -0.10) | -0.001 |
| kiln, clusters | 0.79 | 0.74 | -0.05 (p10 -0.03) | -0.002 |

A change in every scene and every camera but one noisy cell (`sponza/roof` +0.065 in one batch, -0.13 in its p10): the saving is the two dispatches' fixed cost and one read of the depth, 0.03 to 0.15 ms.
(The first timing against the old binary, with a machine loaded by another project, gave the same signs but with swings of +/-0.2 ms between cameras; the same-binary pairs above are the record.)

#### 3. Adaptive gating of occlusion culling: measured, not built

*Question.* G3d costs +0.08 to +0.18 ms where little is hidden (the extra depth pass, the second cut and multi-draw, the pyramid). Could a GPU-side signal (a counter of what was occluded last
frame, and indirect dispatch/draw arguments written by a tiny compute pass, zero-sized when skipping) turn the second pass and the pyramid off, with no read-back (the fence rules forbid waiting; the 2D
backend owns the frame fence)?

*What can be skipped on the GPU.* Only compute work, through zero-sized indirect arguments: the **pyramid** (3 dispatches) and the **test** in the second cut. The expensive parts are not skippable that way:
the depth pass of the opaque meshes and the first pass's clusters is a render pass of CPU-recorded draws (a gate that cannot reach the CPU cannot remove them; only their clusters' indirect draws could be
zeroed); and if the second cut is skipped, the clusters that were not in last frame's history must still be drawn (every cluster the first pass did not draw is drawn by the second, untested), so the
second cut and its multi-draw have to run anyway, or the first pass must be told to draw everything (history all ones, written by the same tiny pass), after which the second cut finds nothing new but still runs
its six compute passes unless they too are indirect.

*Measured* (a throwaway build with two switches, since removed: no pyramid and no test, which is what a perfect GPU-side gate recovers at most while keeping the picture right; occlusion culling off as the
baseline; `Lod3D::occlusion` on with `--lod --lod-clusters`; interleaved pairs of runs of one binary, 5 runs; GPU ms, medians of the cameras' medians, CPU `render()` ms):

| Scene / camera | On, GPU over off | Without pyramid and test, GPU over off | What a gate on the pyramid recovers |
|---|---|---|---|
| hidden open (nothing hidden by the wall) | +0.078 | +0.097 | none (noise) |
| hidden_dense open | +0.145 | +0.116 | 0.03 of 0.145 (20%) |
| hidden_dense occluded / peek (much hidden) | -0.137 / -0.134 | +0.228 / +0.200 (nothing culled, so no saving) | it would lose the whole saving |
| sponza nave / roof, clusters | +0.124 / +0.100 | +0.077 / +0.040 | 0.05 to 0.06 (40 to 60%) |
| kiln_near (3 cameras) | +0.036 | +0.036 | none |

CPU `render()` is +0.02 to +0.06 ms with occlusion on in all of them (the second recording), which no GPU gate removes.

*Verdict: dropped.* The part a GPU-side gate can reach (the pyramid and the test) is 0 to 0.05 ms of a 0.08 to 0.15 ms cost, 0 to 40% and not "most", and a gate must not hurt the best case: `hidden_dense`
occluded and peek (-0.13 ms) need the pyramid, so the gate's decision would have to be right every frame, and the only signal for it is last frame's occluded count in a buffer the CPU cannot read without a
fence. A CPU-side gate (skip the whole second pass and the occluder depth pass) would recover the rest, but needs that count a few frames late through a transfer buffer and a frame fence the 2D backend
does not expose to 3D; building one means changing the 2D backend's fence ownership for 0.05 to 0.1 ms in a case the user asked for explicitly with `Lod3D::occlusion`. It stays an opt-in for scenes
where much is hidden (the plan's verdict stands).

#### 4. The shadow map kept when nothing changed

`Lights3D::cache_shadows` (off by default, as every opt-in; the agent first built it on, and the default was turned off at commit; `Render3DStats::shadow_cached` says a render hit; `KIN_SHADOW_CACHE=0` turns it off for the whole renderer). The shadow pass (a single map or the cascades) is skipped when everything it
is a function of is as it was when the map was last drawn: the map itself (a generation counter, so a remade map never matches), its size and layers, the cascades' views and the camera's and the
sun's frusta (what casts into view, and the cascades' fit, follow the **camera**), the levels of detail (the settings and the view they were chosen for), and for every caster its mesh (a serial number, not a pointer),
`double_sided`, blend, and for a cut-out its cutoff, wrap modes and texture (identity and a version bumped by `update_texture`, `write_texture`, a new mip chain and a compute dispatch that writes it; a
texture that is a render target is drawn into without a trace, so a cut-out caster with one is never cached), and its copies' `MeshInstance`s. All of it except the copies is a `key` of bytes compared exactly. The
copies are compared in full (`memcmp`, stopping at the first difference) against a snapshot taken only after two renders in a row agreed on the key **and** on a cheap sample of the copies (up to eight a draw, spread, and the last),
so a scene that moves never copies anything, and one that moves a little (a sample agreeing, the copies not) makes the cache wait eight renders before it copies again. It cannot give a wrong picture short of a
change it does not see: a texture written by a route that does not pass through the 2D backend (none exists in kin), or an `MeshInstance` that changes and changes back between two renders.

*How often can it hit.* Only when the camera **and** everything in the shadow's reach are still: a paused game, a photo mode, a model viewer at rest, a level walked to a stop. Signal Breach (the crowd always moves) never hits;
the Sponza Walk viewer hits whenever the user stops. It is not a general win, and nothing is lost where it misses (below).

*Byte-identical.* Every picture of the eight configurations rendered with the cache on (`--shadow-cache`, 8 renders of a static scene a frame, so all but the first of a camera hit) equals the baseline's (0 differ except
`kiln_near/grazing`, the known flake), and `test_gpu_shadow_cache` compares each hit with an uncached render to the byte.

Cost, same binary with the cache on and off (`KIN_SHADOW_CACHE=0`), interleaved pairs; GPU ms, medians of the cameras' medians, in brackets the p10; CPU `render()` ms:

| Scene | Stationary: GPU off / on | Stationary: CPU off / on | Moving: GPU off / on, CPU off / on |
|---|---|---|---|
| crowd, 22,000 copies | 1.37 / 0.90 (**-0.47**; p10 -0.44) | 0.155 / 0.219 (**+0.06**: the copies compared) | 1.084 / 1.085, 0.195 / 0.193 (**no change**, 9 runs, the crowd drifting each frame) |
| crowd, 4 cascades | 1.44 / 0.87 (-0.57) | 0.37 / 0.55 (+0.19; noisy: +0.06 to +0.36 by camera) | |
| sponza | 0.335 / 0.235 (-0.10; p10 -0.13) | 0.085 / 0.065 (-0.02) | |
| sponza, 4 cascades | 0.52 / 0.30 (-0.22) | 0.127 / 0.163 (+0.04) | |
| helmet | 0.165 / 0.104 (-0.06) | 0.015 / 0.012 | |
| shadows | 0.120 / 0.099 (-0.02) | 0.028 / 0.022 | |
| kiln (clusters) | 0.51 / 0.29 (-0.23) | 0.031 / 0.028 | |
| kiln_near / hidden_dense (clusters) | 0.305 / 0.230 (-0.07), 0.58 / 0.49 (-0.09) | -0.003 / -0.001 | |

A stationary hit saves the whole shadow pass (a quarter to a half of these frames) and costs a comparison of the casters' copies on the CPU, which is a small gain on a small scene (the recording of the pass is skipped) and a loss of 0.06 to 0.2 ms on
22,000 copies (1.6 MB at memory speed): the GPU gain is five to ten times larger. A moving scene pays the key and the sample, which is within noise even for 22,000 copies. The harness and `kin_draw_bench` turn the cache off (`--shadow-cache` brings
it back), because they render one scene many times and mean to time the pass.

Tests (`test_gpu_shadow_cache`, 160 x 120, forty copies in a draw so that the sample cannot see them all): a still scene hits from the third render with 0 draws and the same picture; off (the scene's switch or `KIN_SHADOW_CACHE=0`) draws every time; and each of the sun,
camera, resolution, area, cascades, a caster moved, **a copy the sample does not look at**, a caster added, a material changed, a mesh swapped, a caster that stops casting, levels of detail on and a **forced level**, the **cascades' reach**, the camera under cascades, and a cut-out's
texture written in place, draws again and gives the picture of an uncached render, after which the cache hits again; a render target as a cut-out's texture is never cached; a resize misses; two views alternating each get the right picture. Mutations, each made, seen to fail and reverted:
the full comparison removed (stale: the unsampled copy), the camera left out of the key, the texture version left out, the map's generation, size and layers left out, the double-sided flag, the mesh, the render-target rule (all failed), the levels of detail's
settings and the cascades' plan left out of the key (these two survived the first version of the test, which only turned LOD and the cascades on and off, and a camera move that the key's other entries cover; the forced level and the cascades' reach were added to catch them, and then caught them). Two mutations that survive by design: leaving the sun's direction or the shadow area out of the
key alone (the light's view-projection, which is in it, covers them).

#### 5. Memory: the opt-in targets given back

Each feature's targets belong to a view; a group is released when it has not been used for 120 renders of that view (`KIN_RELEASE_FRAMES=n` for tests, 0: never): ambient occlusion's two R8 textures, contact
shadows', the pre-pass's depth, its normals (also the main pass's velocity), temporal anti-aliasing's (history, depth, float picture of an 8-bit view, sharpened picture, last velocity), occlusion culling's (pyramid, occluder depth, history)
and, per renderer, its second gathered buffer of indices and job history (`GpuView3D::_idle_*`, `age_targets` at the end of `render()`). A group is "used" when its pass ran this render, so a feature toggled at
least every 119 renders never lets go, and the targets are not made again. Temporal anti-aliasing used to release at once when it went off; it now drops its history at once (`suspend_taa`: valid = false, frame 0, the copy
records cleared, so the next frames start afresh exactly as after a release or `reset_history()`) and releases the textures with the rest. The shadow map and the MSAA/HDR targets are untouched. `Render3DStats::feature_bytes` is what the groups hold
and `targets_released` the groups given back that render.

*Video memory, 1280 x 720, everything on, then 130 renders with it all off* (`kin_reference --vram-cycle`): by the renderer's own count (`feature_bytes`), **Sponza** (4 cascades, AO, contact shadows, TAA, 8-bit view) **45.7 MiB to 0**, **kiln_near** (the same with clusters and occlusion culling,
second gathered buffer included) **93.2 MiB to 0** (5 and 7 groups released). By `nvidia-smi --query-gpu=memory.used` (the device is nearly alone on it): kiln_near 876 MiB with the features on to 796 off (-80), 860 to 796 (-64), 810 to 794 (-16 on
the first camera, whose `on` figure was taken while the scene was still growing); **Sponza no visible change** (980 MiB both ways; the first camera 932 on, 980 off, which is an unrelated allocation). SDL_GPU's Vulkan allocator keeps its memory blocks and sub-allocates, so
a release makes the memory reusable by this renderer at once but not always returned to the driver: the count above is exact, the `nvidia-smi` deltas are not. The numbers fall at 4K in proportion (the R8 textures of the blur's third copy alone were 8.3 MB each there).

*Pictures.* After a release, a feature on again draws what a fresh view draws, to the byte: tested for AO + contact + TAA together, for TAA alone (the velocity-in-the-main-pass path), for features on their own clocks, and for occlusion culling.

Tests: `test_gpu_feature_release` (160 x 120, `KIN_RELEASE_FRAMES=5`: held for four renders, gone at the fifth, `targets_released`; on again equals a fresh view over 8 TAA frames, also after a shorter pause;
each feature on its own clock (AO alone holds exactly what a fresh AO-only view holds); toggled every render for 40, every four for 40: never released; every six: released each time; `KIN_RELEASE_FRAMES=0` keeps) and
`test_gpu_occlusion_release` (the pyramid, depth, history and the renderer's second buffer). Mutations, each made, seen to fail and reverted: TAA's history not dropped when it goes off (caught by the old TAA test), releasing after one
render (the thrash test), the second gathered buffer not released, the contact group watching the AO's flag, the velocity target counted as unused while TAA runs (caught only after a `targets_released == 0` check was added to the TAA loop). **Not caught:**
clearing the occlusion layout and `_occ_reset` on release (a stale history can only cost speed, never a picture).

#### 6. Pre-warming pipelines: `Renderer3D::warm_up(view, scene)`

*The hitch, measured first* (`kin_reference --hitch <feature>`, a fresh process per feature: 40 renders of the scene with the feature off, then on; each of the first renders timed; Sponza for AO, contact shadows, TAA, TAA + AO, 4 cascades and all of them,
`hidden_dense` with clusters for LOD, the CPU cut and occlusion; the first render's `render()` ms and CPU wall ms of render + present; steady with it on in brackets). Compute pipelines are all made when the renderer starts (the cost of start-up, not of first use); what
is made on first use is graphics pipelines (4 for AO, 4 for contact shadows, 13 for TAA, 11 for TAA + AO, 11 for all, 0 for the cluster features) and targets.

| Feature on at run time | Cold driver cache: first render, no warm-up | with `warm_up` | Warm cache: first render, no warm-up | with `warm_up` | Steady |
|---|---|---|---|---|---|
| ambient occlusion | 40.7 ms (wall 41.8) | 0.30 (1.07) | 1.4 (2.5) | 0.46 (1.46) | 0.3 (1.3) |
| contact shadows | 47.4 (48.6) | 0.30 (1.09) | 1.4 (2.3) | 0.62 (1.67) | 0.3 (1.3) |
| TAA | **387** (390) | 0.36 (1.18) | 3.6 (5.9) | 0.53 (1.57) | 0.3 (1.4) |
| TAA + AO | **275** (283) | 0.7 (3.3, loaded machine) | 4.8 (7.9) | 0.40 (1.31) | 0.4 (1.4) |
| 4 cascades | 4.1 (7.6) | 0.28 (3.4, loaded machine; 1.2 in the warm run) | 3.8 (7.2) | 0.31 (1.22) | 0.3 (1.4) |
| AO + contact + TAA + 4 cascades | **208** (212) | 0.55 (1.43) | 18.6 (25.8, loaded) | 0.88 (2.0) | 0.5 (1.5) |
| levels / clusters (GPU cut) | 1.9 (5.8) | 2.3 (8.7): **no change** | 1.7 (2.7) | 2.5 (3.7): no change | 0.1 to 0.2 |
| clusters, CPU cut | 1.1 (2.6) | no change | 1.2 (2.9) | no change | 0.8 |
| occlusion culling | 3.5 (5.5) | 3.9 (5.4): **no change** | 6.0 (8.3) | 6.0 (7.8): no change | 0.2 |

("Cold" is `__GL_SHADER_DISK_CACHE=0`, a player's first run; "warm" the second run of the same process, the cache filled. The 4 cascades' 4 ms is the 4-layer shadow map (64 MB) being made, which `warm_up` also does.)

*`warm_up`.* `Renderer3D::warm_up(view, scene)` (`IRenderer3DBackend::warm_up`) makes what `setup_frame`, `prepass` and the passes would ask for on a first render of the scene: the targets of AO, contact shadows and TAA (the velocity target alone, or the pre-pass's, as
the frame will need), the shadow map at the scene's size and cascades, the pyramid and occluder depth of occlusion culling, and pipelines (`pipeline()` makes the missing and keeps them) for each of the scene's draws (the pass its blend and wireframe put it in, culling from `double_sided`, its custom shader,
the velocity variant when the main pass will write it) and, by the scene's settings, the shadow, pick, occluder, and cut-out variants. It records nothing and draws nothing. Nothing is warmed at start-up by default; Sponza Walk warms up with every
feature on once the model is loaded, and again when a switch that needs it is turned (the shadow map's size, MSAA or HDR, the features).
After it the first render makes **0 pipelines** (`Render3DStats::pipelines_made`) and its `render()` is 0.3 to 0.9 ms against 1.4 to 4.8 (warm cache) or 40 to 390 (cold) without; the CPU wall time of the first frame is within about 0.3 ms of
steady in the runs above (the rest is machine noise: the table's loaded-machine cells).

*Not warmed* (and said so): the cluster cut's buffers and occlusion culling's second gathered buffer and history (sized by the scene's clusters, made by the first cut: the 2 ms and 6 ms above), environment filtering, a mesh's first upload, pipelines of draws the scene gets
later. A scene with a different view format or sample count needs its own warm-up (the pipelines are for the view's).

Tests: `test_gpu_warm_up`, each of eight configurations on a **fresh device** (so that nothing earlier made a pipeline): first without `warm_up`, where the first render makes pipelines (5, 6, 23, 18, 11, 8, 1 and 12: the control, so the test is not vacuous), then with it, where it makes **0**
and the second render makes 0 (plain 4x MSAA with picking; cascades in an HDR view; TAA with custom shaders and the overlay (the pre-pass fallback); TAA without them (the main pass writes the velocity); TAA + AO + contact + cascades; AO + contact; occlusion culling; occlusion + AO + TAA; every kind of draw in each: opaque, double-sided,
cut-out, alpha, additive, wireframe, custom shader opaque and blended, overlay). Mutations, each made, seen to fail and reverted: no shadow pipelines, no custom-shader pipelines, no pick pipelines, no velocity variant (only after the TAA configuration without custom shaders was added), occluder pipelines
not made (**not caught**: with this device's formats the occluder pipeline is the shadow pipeline: the same key).

#### Phase 1 in one table

GPU ms / CPU `render()` ms, before to after (old binary against new, or the same binary with the switch off, interleaved pairs), the two kinds of scene:

| Item | Big levels | The crowd (20,000 small meshes) | Pictures |
|---|---|---|---|
| 1 TAA velocity from the main pass (TAA on) | sponza **-0.18** / -0.03; kiln (no LOD) **-2.25** / -0.006; kiln_near -0.08, hidden_dense -0.13, kiln clusters -0.10 (CPU -0.006 to -0.007) | **-0.50** / -0.008 | identical (all TAA configurations) |
| 2 one blur for AO and contact shadows | sponza -0.05 (p10 -0.10), kiln -0.05, hidden_dense -0.08, kiln_near -0.02 / CPU -0.001 to -0.002 | -0.14 (p10 -0.30) / -0.002 | identical |
| 3 occlusion gating | dropped (see above) | | |
| 4 shadow map kept (a still scene) | sponza -0.10 / -0.02; kiln -0.23; kiln_near -0.07, hidden_dense -0.09 / about 0 | -0.47 / **+0.06** (the copies compared; **moving crowd: 0 / 0**) | identical (and tested against uncached) |
| 5 targets given back | 45.7 MiB (sponza) and 93.2 MiB (kiln_near) to 0 after 120 renders off | | identical after re-enabling |
| 6 warm-up | first-use hitch of AO / contact / TAA / all: 40 / 47 / 387 / 208 ms (cold cache) to under 1.1 ms | | no picture involved |

With every feature off (the default, the new binary against the old, interleaved pairs, `render()` CPU ms and GPU ms): crowd GPU +0.083, +0.036, +0.008, -0.029 over four batches of 7 to 9 runs (the same old binary against a copy of itself, which is the noise: +0.071, -0.006, -0.041; the
per-camera swings of that scene are 0.1 to 0.15), CPU -0.001 to -0.007; Sponza GPU +0.009, CPU -0.008; helmet, shadows and cornell GPU within +/-0.006 and CPU within +/-0.001; kiln_near and hidden_dense with clusters GPU +0.012 and +0.014, CPU -0.001.
Nothing slower than this machine's noise can show.

**Picture identity at the end** (the final binary against `sp1_before_*`, every picture of the nine configurations `def`, `all` = 4 cascades + AO + contact shadows + TAA, `taa`, `ao`, `cs`, `c4`, `cl`, `occ` and `cltaa`, once with the shadow cache off as the harness runs it and once with it on, `--shadow-cache`):
**0 pictures differ, except** `crowd/above` under `--taa` (the old binary's own flake) and `kiln_near/far` under `--lod --lod-clusters --taa` (it flips between two pictures in the old binary too: 1 of 3 runs of the old binary, 2 of 3 of the new; both runs of the final binary agree with each other)
and `kiln_near/grazing` under `--lod --lod-clusters` in the shadow-cache run (the known kiln flake: old 2 of 6 runs, new 3 of 8).

**Not done or not tested.** Sponza Walk's panel click path (the new "Keep the map while nothing moves" row, the warm-up on a toggle) cannot be clicked here; its `--headless --preset taa` run and the `--set shadow_cache=off` run were made and give the same picture byte for byte. Device loss and a changed colour space:
the shadow cache lives in the renderer, which a lost device replaces, and a changed 2D target format remakes the view's targets (the cache's key holds the map's generation, which a remake of the map changes, and the view's size), but neither was provoked. D3D12 and Metal untested (Vulkan on NVIDIA and lavapipe only). Occlusion gating (item 3) was measured and dropped. The first
frame with levels, clusters or occlusion culling still allocates its buffers (2 and 6 ms), which `warm_up` does not make. `nvidia-smi` does not show a release reliably (item 5).

**Tools added** to `kin_reference` (nothing else of the harness changed): `--shadow-cache` (the harness turns `Lights3D::cache_shadows` off by default: it renders a static scene many times a frame), `--crowd-move` (the crowd drifts every frame, posed per frame: 22,000 moving copies),
`--hitch <feature>` (+ `--warm-up`), `--vram-cycle`; `kin_draw_bench` turns the cache off. Switches read on every render for tests and A/B timing: `KIN_TAA_PREPASS=1`, `KIN_BLUR_SEPARATE=1`, `KIN_SHADOW_CACHE=0`, `KIN_RELEASE_FRAMES=n`.

### Opt-in speedups, phase 2 — results

Phase 2 asks for cheaper ambient occlusion and contact shadows at the price of **small picture changes**, accepted only inside a contract measured against the code as it was at `a5c4ea8`:
(a) new against current, for every scene with the feature on, mean <= 0.3 levels, p99 <= 6, SSIM >= 0.998 and nothing visible in 3x crops and x8 difference images; (b) distance to the Cycles `truth` not worse by more than 0.05 levels on any
picture and not worse on average; (c) a change that costs more than it saves anywhere, or fails (a) or (b), is dropped or gated; (d) no new shimmer under TAA. Baselines (old binary, kept in `reference_out/tmp_sp2/old/`, removed at the end): labels
`sp2_before_{def,ao,ao_half,cs,both,all,taa,c4}` over the 11 scenes (cornell, crowd, furnace, helmet, ibl_helmet, ibl_spheres, kiln, shadows, spheres, sponza, motion: 24 pictures) and `sp2_before_{cl,big,big_ao,big_cs,big_taa}`
over kiln_near and hidden_dense with `--lod --lod-clusters`; 1080p labels `sp2_b1080_*` for the chain.

**Method for timing.** Interleaved pairs of whole runs of two binaries proved too noisy on this laptop (GPU clocks drift 10 to 40% inside a session; the same pair gave +0.04 ms and -0.25 ms in two batches). The decisive tool is new: **`kin_reference --ab NAME[:A:B]`** sets an environment variable that
the renderer reads on each render to A in one block of 8 frames and to B in the next, throughout one run, and times the two apart (the first three frames of a block, whose GPU time arrives late, are not counted). One process, one clock state, A and B alternating every 8 frames:
the figures below are `A - B` from it, GPU ms, medians over 80 to 400 frames per camera, means over the cameras of the scene group named. (The CPU `render()` ms are in the columns of the tables only where they changed.) A second new tool, **`--flicker N`**, reads back N consecutive frames after a camera's picture and
prints the mean and the largest frame-to-frame change (item (d)). Both are in `kin_reference`; the library has no code for either.

Switches read on each render: `KIN_AO_CLASSIC=1` (the occlusion as before, never over the chain), `KIN_AO_CHAIN=1` (always over the chain), `KIN_CONTACT_CLASSIC=1` (contact shadows blurred twice, as before). With none of them, pictures **under 1.5 million pixels** (every reference picture at 1280 x 720) are the old ones to the byte
except the contact shadows' (below).

**Where the occlusion's time goes (1280 x 720, Sponza and the shadows scene, a throwaway build that skipped one stage at a time).** On/off of the whole occlusion: Sponza nave 0.40 to 0.98 ms, roof 0.33 to 0.85, shadows 0.09 to 0.39 (overview) and 0.10 to 0.70 (contact), the crowd 1.4 to 2.2 / 0.8 to 1.6 / 1.7 to 2.7.
Of that, the pre-pass (the geometry again) is 0.04 on the small scenes and most of the crowd's +0.8; the two blur dispatches about 0.10; the new chain 0.03; the GTAO shader the rest, about 0.4 ms, of which setup and integration (no taps) 0.07, taps 0.35 (their reads about half, their arithmetic the other half: a variant with no arithmetic in the taps was 0.18 ms cheaper, one with every tap at level 0 of the chain 0.09 dearer than with levels).
The depth chain only addresses the reads, and at 720p they hit the cache well enough that the chain's own cost cancels what it saves.

#### 1. The linear depth chain (XeGTAO's mip chain), used from 1.5 million pixels up

*Built.* `ao_depth_chain.comp`: one dispatch of 16 x 16 tiles reads the pre-pass depth and writes the distance in front of the camera (R32F; 0 for the sky) into level 0 and, through groupshared memory, four coarser levels of one mip-mapped texture (`GpuDevice::create_target` has a `levels`
argument now), each texel a weighted average of its four children that leans to the farthest (XeGTAO's filter: a child more than the falloff nearer than the farthest counts for less, so a thin near thing does not drag a far wall toward the camera and a silhouette grows no halo). `gtao_chain.comp` is `gtao.comp` with the same slices, steps, thickness
heuristic, falloff, noise and integration, reading the chain: a tap's level follows its distance from the pixel (level 1 from 20 pixels, then 40, 80, 160), the view-space position is rebuilt from the distance without a divide (`kin_chain.inc.glsl`; for a perspective projection, whose inverse's z is f / w, x and y over w follow from the distance; an orthographic one needs none), and the tap
position is the centre of the texel it fell in. `ao_blur_chain.comp` and `ao_blur_both_chain.comp` read level 0 (no view-space divide a tap, one reciprocal a pixel instead of one a tap). `stats().ao_chain_built` says it ran; the chain's own dispatch is not counted in `ao_dispatches` (it would break every count). The chain goes with the pre-pass's depth
(made on first use, remade when the view's size changes, released with the depth after 120 idle renders, counted in `feature_bytes`).

*What it costs and saves.* The chain's build alone (the dispatch, A/B against none): **+0.027 ms at 1280 x 720, +0.057 at 1920 x 1080, +0.20 at 3840 x 2160**. The occlusion over the chain against the old shader (A/B in one process; GPU ms, means over the cameras, new minus old; negative is faster):

| View | Group of scenes | New minus old | Notes |
|---|---|---|---|
| 1280 x 720 (forced) | Sponza, shadows, helmet, crowd | **+0.037** (Sponza +0.08, crowd first_person/mid_deck -0.03, crowd above +0.07) | does not pay: dropped below 1.5 M pixels |
| 1280 x 720, half resolution | the same | +0.019 | |
| 1920 x 1080 | Sponza nave / roof | **-0.28 / -0.24** of 2.2 / 2.0 | |
| | shadows contact / fence / grazing | -0.42 / -0.44 / -0.32 of 1.7 / 1.9 / 1.7 | |
| | crowd first_person / mid_deck | **-0.51 / -0.64** of 3.6 / 3.4 | |
| | kiln_near dome_close / grazing, hidden_dense occluded / peek / open (clusters) | -0.34 / -0.14, -0.21 / -0.46 / -0.11 | |
| | **loses**: shadows overview (+0.22), helmet (+0.03), crowd above (+0.12), kiln/game (+0.46 of 7.0), kiln_near far (+0.04) | | few pixels with a reach of 20 pixels or more |
| 1920 x 1080 with contact shadows (merged blur) | Sponza, shadows, crowd | -0.55 to -0.97 (the blurs read the chain too) | same losers |
| 1920 x 1080, half resolution | | -0.04 mean | smaller: half the pixels |
| 3840 x 2160 | Sponza nave / roof | **-5.15 / -4.17** of 19.7 / 15.3 | |
| | shadows contact / fence / grazing, crowd first_person / mid_deck | -4.1 / -4.2 / -4.3, -4.8 / -4.3 | -25 to -35% of the occlusion |
| | kiln_near, hidden_dense (clusters) | -2.0 to -3.6 | |
| | **loses**: crowd above +0.49, kiln_near far +0.08 | | |
| 3840 x 2160, half resolution | | -0.57 mean | |

*Verdict: gated, not dropped.* The chain wins where the view is big and the search wide (the cache misses it removes grow with the pixels a tap is away: its saving per frame grows faster than its cost) and loses a little where almost every pixel's reach is a few pixels (a far view, a small object, the kiln at 1080p: 0.03 to 0.5 ms on frames of 1 to 7 ms). The only gate the CPU has without a GPU read-back
is the size of the view, so the chain is used when the depth has **1.5 million pixels or more** (1440 x 1040: 1280 x 720 stays on the old path to the byte, 1920 x 1080 and up take the chain). Where it loses it loses by under 0.5 ms (2% of the kiln frame, up to 15% of a frame with little occlusion work, the crowd's 'above' camera); where it wins it wins 10 to 35%. An honest summary: **a clear gain from 1080p up on typical scenes, 0 at 720p**.
What did *not* help (all throwaway builds, same A/B): arranging all of a slice's depth reads before their arithmetic so the GPU has them in flight together (no change: -0.00 at 1080p within noise, so the taps are not latency-bound in the way assumed), one division less a tap by `inversesqrt` (inside noise), a lower first-level distance (XeGTAO's 2^3.3 = 9.85 pixels instead of 2^4.3 = 19.7: 0.03 ms faster at 1080p, but **the fence in `motion` darkens differently, mean 0.41 and p99 8 at 1080p: fails (a)**; so 19.7), a higher one (39 pixels: 0.13 ms slower than 9.85 at 1080p).

*Pictures against the old occlusion* (worst camera of each scene; mean levels / p99 / SSIM; the chain forced at 1280 x 720, which is where every label of the reference harness is made, and by default at 1920 x 1080; contact shadows alone, and the old half resolution against the chain's):

| Scene | AO 720p (forced) | AO 1080p (default) | AO half 720p (forced) | Contact shadows alone, 720p (one blur) |
|---|---|---|---|---|
| cornell | 0.00 / 0 / 1.0000 | 0.00 / 0 / 1.0000 | 0.00 / 0 / 1.0000 | 0.00 / 0 / 1.0000 |
| crowd | 0.09 / 1 / 0.9995 | 0.10 / 2 / 0.9995 | 0.09 / 1 / 0.9996 | **0.29 / 6 / 0.9987** |
| furnace | 0.01 / 0 / 0.9999 | 0.01 / 0 / 0.9999 | 0.00 / 0 / 0.9999 | 0.00 / 0 / 1.0000 |
| helmet | 0.01 / 0 / 0.9999 | 0.01 / 0 / 0.9999 | 0.01 / 0 / 1.0000 | 0.01 / 0 / 0.9999 |
| ibl_helmet | 0.02 / 1 / 0.9999 | 0.02 / 1 / 0.9999 | 0.01 / 1 / 0.9999 | 0.00 / 0 / 1.0000 |
| ibl_spheres | 0.03 / 1 / 0.9998 | 0.04 / 1 / 0.9998 | 0.03 / 1 / 0.9999 | 0.00 / 0 / 1.0000 |
| kiln | 0.01 / 1 / 0.9999 | 0.02 / 1 / 0.9999 | 0.01 / 1 / 0.9999 | 0.01 / 0 / 0.9997 |
| motion (TAA, thin fence) | 0.15 / 3 / 0.9999 | 0.19 / 4 / 0.9997 | 0.09 / 2 / 0.9999 | 0.02 / 0 / 0.9999 |
| shadows | 0.04 / 1 / 0.9999 | 0.05 / 1 / 0.9999 | 0.04 / 1 / 0.9999 | 0.07 / 2 / 0.9993 |
| spheres | 0.01 / 1 / 0.9999 | 0.01 / 1 / 0.9999 | 0.01 / 1 / 0.9999 | 0.03 / 0 / 0.9997 |
| sponza | 0.02 / 1 / 0.9999 | 0.03 / 1 / 0.9999 | 0.02 / 1 / 0.9999 | 0.01 / 0 / 0.9999 |
| kiln_near, hidden_dense (clusters, `--ao --contact-shadows`, 720p forced) | 0.01 / 0 / 1.0000 | | | 0.01 / 0 / 0.9999 |

All inside (a) (the AO columns with a factor of 2 or more to spare; contact shadows' crowd inside it by 0.01). With AO **and** contact shadows (`both`, chain forced) the worst is 0.15 / 3 / 0.9995 (motion, crowd), with the full set of `--cascades 4 --ao --contact-shadows --taa` 0.16 / 3 / 0.9995. **Distance to Cycles** (`truth`, 18 pictures of 9 scenes, mean levels, new minus old): the chain's AO
**worst +0.014** (sponza roof), mean -0.0008, the largest gain -0.035 (shadows fence); the half-resolution chain worst +0.013; contact shadows alone worst +0.007, mean -0.0007; both together worst +0.014. (b) holds with a factor of 3 to spare. Looked at, 3x crops with the x8 difference beside them: Sponza nave (a lion's head, the banners and the floor: the difference image is black, 1 level at most) and roof (sky edge, lattice, 1 to 2 levels),
ibl_spheres front (the spheres' contact with the floor: nothing), the shadows' fence (the fence's feet and the bars: faint vertical threads at the edges of two bars, 1 to 2 levels) and `motion/close` (the pickets, the worst picture: faint vertical threads along the edges of some pickets, 1 to 3 levels, no halo, no banding, no leak), the kiln's close view (nothing),
and contact shadows' shadows/contact (the end of one contact shadow differs by a pixel or two at its tip, up to 30 levels over 0.5% of the pixels) and the crowd (scattered speckles at x8 that cannot be seen at 1x). I did not look at a Signal Breach frame: the game has no switch to force the occlusion on (the crowd scene is its enemy deck).

*(d) Shimmer under TAA* (`--flicker 30`, 30 consecutive frames after the 32 that settle the history): 1280 x 720 with the chain forced and 1920 x 1080 by default, still camera (sponza, shadows, helmet) and the `motion` scene's moving things and 30-frame pan, `--taa --ao [--contact-shadows]`, classic against chain: the mean frame-to-frame change is the same to 0.001 level everywhere (sponza nave 0.1025 against 0.1024, shadows fence
0.0727 against 0.0723, motion/pan 7.868 against 7.867) and the largest change within 1 level of the old (sponza nave 81 against 81: TAA's own edge jitter; shadows contact 14 and 14). `test_gpu_occlusion_speedups` repeats it on a still scene under TAA.

#### 2. Contact shadows

*Cost.* Contact shadows alone, 1280 x 720: the march 0.07 to 0.09 ms, its two blurs 0.07 to 0.09, together +0.16 beyond the pre-pass (A/B with each stage skipped, Sponza, shadows, helmet, crowd); at 1920 x 1080 0.39 (march 0.20, blurs 0.20), at 3840 x 2160 2.4 (1.0 to 1.4 each).
**Built: one blur pass instead of two when the contact shadows are blurred on their own** (the second pass, down, is gone; the result of the first stays in the second texture and the two change places). With the occlusion also on and at full size the two features' blurs are already one dispatch a direction (phase 1) with the weights shared, and dropping the contact half of the second pass there saved nothing measurable
(+/-0.003 at 720p, -0.02 at 1080p, with a noise of 0.03), so that path is unchanged (and its contact shadows are blurred twice: with occlusion on or off the contact shadows differ by a hair, by design; the test checks the separate path against `KIN_CONTACT_CLASSIC=1` to the byte). GPU, A = one blur, B = two (`KIN_CONTACT_CLASSIC=1`), means of the group:
1280 x 720: Sponza -0.067 / -0.059 (nave / roof), shadows -0.034 to -0.071, helmet -0.018, crowd -0.02 to -0.05, kiln_near / hidden_dense (clusters) -0.02 to -0.05; 1920 x 1080: Sponza -0.14 / -0.15, shadows -0.11 to -0.14, crowd -0.05 to -0.13; 3840 x 2160: Sponza -0.65 / -0.27, shadows -0.34 to -0.42, crowd -0.17 to -0.59. That is 15 to 20% of the contact shadows' own cost (4 to 7% of a Sponza
frame at 720p). CPU `render()`: unchanged (-0.006 to +0.004, noise).
Contract: the table above (contact shadows alone, all 11 scenes + clusters): worst mean 0.29 (crowd above), p99 6, SSIM 0.9987: **inside, with a margin of 0.01 on the mean and none on the p99**: it is the tightest change of the phase and the crowd's noise-heavy small meshes are why; distance to truth worst +0.007, mean -0.0007; flicker under TAA unchanged to 0.001 mean (sponza 0.1035 / 0.1037, shadows/fence 0.0703 / 0.0712, motion 7.858 / 7.857); looked at: the end of a contact shadow differs by a pixel (above) and the crowd shows scattered speckles at x8.
It is kept because it passes every check, saves 0.03 to 0.15 ms where contact shadows run alone and costs nothing elsewhere; flagged as the change to remove first if the crowd's margin matters.

*Not built, with what bounds it.* **Half-resolution march with a depth-aware upsample**: the march is at most 0.09 ms at 720p (0.20 at 1080p); a half-resolution march saves at most three quarters of that and the upsample (a second dispatch, 4 depth-weighted taps) costs part of it back, so under 0.05 ms at 720p, and it breaks the merged blur (different sizes) that saves 0.03 to 0.15 ms by itself. Not worth building. **A cheaper march by arithmetic**: the 4 x 4
multiplication a step replaced by interpolating the clip-space ends of the ray (exact: a projection is linear), built and measured, A/B in one process: **-0.004 ms** (noise; Sponza -0.008 to -0.003, shadows -0.014 to -0.006): the march is bound by its reads, not its arithmetic; removed. **Fewer steps**: `steps` is the user's setting; not changed.

#### 3. Sharing the chain between the two, and fixed costs

With the occlusion on at 1080p and up the contact shadows' blur reads the chain already (0.4 to 0.97 ms saved together in the table above). The contact **march** could read level 0 of the chain (the linear depth, no divide), but its arithmetic was shown above not to matter (a march with its per-step multiplication gone was 0.004 ms faster) and its thickness test is in pixels of a surface: a coarse level would change what a thin
pole shades; not done. The pre-pass's fixed costs (the clears, the transitions, the R8 formats): a skipped-stage build puts the pre-pass at +0.04 ms on the small scenes at 720p and the rest of the crowd's cost is the second draw of 20,000 copies; the R8 targets are already two a feature since phase 1; the chain (R32F, 5 levels, 4/3 of 3.7 MB at 720p, 11 MB at 1080p, 44 MB at 4K) is the only new memory and goes with the depth.
Nothing else was found that can go without changing a picture.

#### 4. What was tried and dropped (each against the contract)

| Idea | Result | Verdict |
|---|---|---|
| **One slice a pixel** instead of two (half the taps; the angle noise spread over the whole half circle) | -0.09 ms (of 0.56) but pictures **fail (a)**: mean 3.7 levels (ibl_spheres), p99 13, SSIM 0.992 | dropped |
| **Checkerboard** GTAO: a thread does one pixel of two (half the dispatch), the first blur pass fills the others from their valid neighbours | -0.08 ms mean (-0.1 to -0.17 on the shadows, none on the helmet); pictures: crowd mean 0.31 and SSIM 0.996, ibl_spheres grazing SSIM 0.9974, motion/close mean 0.33 / p99 6 (**fails (a)**), and a **diagonal hatch** visible in the crowd's floor at 1x (the two kinds of pixel are blurred with different kernels) | dropped |
| **Fewer steps** (8 to 6 / 5 / 4), the chain path | -0.00 / -0.06 / -0.09 ms of 0.6 (noise at 6); not run against the contract: a quarter of the taps for a sixth of the cost | dropped (not worth the look change) |
| Old **half resolution** as the default (existing `half_resolution`) | -45 to -60% of the occlusion at 720p; against full: crowd first_person SSIM 0.9962, mid_deck 0.9972, motion/close mean 0.44 p99 9: **fails (a) on three pictures**, ok on the rest (sponza roof 0.21 / 2 / 0.9982) | stays an explicit option; no default change, no new API (a depth-aware upsample might pass; not tried) |
| Reads before arithmetic in the taps; `inversesqrt` | no change | dropped |
| Contact march by interpolated clip positions | -0.004 ms | dropped |
| Contact half resolution | bound under 0.05 ms | not built |

#### Tests

`test_gpu_occlusion_speedups` (new, 640 x 480 views read back in 160 x 120 tiles, and one 1600 x 1000; NVIDIA and lavapipe give the same numbers to 3 digits): which path ran (`ao_chain_built` 0 for classic, 1 for forced, 0 by default at 640 x 480 with the picture the classic one's to the byte, classic wins over a forced chain, 1 by default at 1600 x 1000 with the picture the forced chain's);
agreement of the chain with the classic path inside the contract on the room (mean 0.022 against a bound of 0.3), thin blocks at several distances in perspective (0.052) and orthographic (0.033), half resolution (0.022), 1600 x 1000 (0.004), and under TAA of a still camera (last frame mean 0.157 / p99 2 against 0.3 / 6; frame to frame as steady as the classic: mean 0.0233 against 0.0241);
**a plane** (a depth ramp: no occluder, so a wrong level, texel or position shows at once) in perspective and orthographic: the two paths agree to 0.002 / 0.000 mean and the chain is never darker than the classic's own noise (+3 levels); resize up then down (each picture the fresh view's, to the byte); release after a pause and made again; and contact shadows' one blur against two (2 against 3 dispatches, mean 0.020 / p99 0 / worst 25, the shadow still there).
`test_gpu_contact_shadows`: the count of contact dispatches for contact shadows alone is **2, not 3** (the march and one blur); the merged-blur comparison "merged equals separate to the byte" now runs the separate path with `KIN_CONTACT_CLASSIC=1` (which still has two passes, as the merged blur does) and adds the one-blur separate path (2 contact dispatches, a different picture); nothing else of the test moved (the crease, the open floor, the sun side, the far plate, intensity, MSAA, the overlay, no sun: all as before).
`test_gpu_ambient_occlusion` is unchanged and passes (small view: the classic path). **Mutations**, each made, seen to fail and reverted: first-level distance 2.5 instead of 19.7 (**caught** by the plane: mean 0.089 against 0.03; an earlier bound of 0.3 would not have: tightened), a texel too far (+1.5) (caught: room mean 0.73), the wrong level fetched (+1) (caught: room mean 2.6), the chain not remade after a resize (**not caught** by shrinking, which a stale bigger chain survives; **caught** after the test grew a view from 512 to 640), the chain built only once (caught: plane mean 4.6), orthographic treated as perspective (caught: plane mean 3.0), the chain not released with the depth (caught: `feature_bytes`), no gating by size (caught), contact blurred twice again (caught: dispatch count).

#### Verification at the end

`ninja -C build -j8`, `ninja -C build-no3d -j8` (after `cmake .` for the new shaders); `ctest` in `build` 68 of 68, in `build-no3d` 65 of 65; `kin_render3d_tests` with `KIN_REQUIRE_GPU_TESTS=1` on the NVIDIA RTX 4080 Laptop (device 0) and on lavapipe (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`: the new shaders compile and give the numbers above); `kin_example_tests`; `KIN_RENDER_BACKEND=gpu sponza_walk --headless --screenshot out.png --preset ao` runs (1280 x 800: the old path; with `KIN_AO_CHAIN=1` the chain's picture differs by mean 0.015, at most 4 levels).
**Byte identity with everything off** (the new binary against `sp2_before_*`, 11 scenes): `def` 24 of 24, `cl` (clusters) 6 of 6; `taa` and `c4` (4 cascades): all but `kiln/close` and `kiln/game` (first run), and in a second `--taa` run `ibl_spheres/front` and `grazing` and the kiln's two. Those pictures are **not stable in the old binary either**, and that is a finding of its own:
four more 11-scene `--taa` runs of each binary (no occlusion, no contact shadows, 1280 x 720) gave `ibl_spheres` with its **spheres drawn green** (the green channel saturated: 9.5% of the picture's pixels at 255 against 0.1%) in 3 of 4 runs of the old binary and 4 of 4 of the new, and the kiln's two pictures differing in 1 and 3 of 4 (old) and 3 of 4 (new): the same process rendered the scene alone, three times each, gave the
baseline's pictures every time. The old binary also drew the green spheres in the 1920 x 1080 `--ao` baseline (after the other scenes of the process; alone it did not), which is why that scene's 1080p baseline was re-rendered alone. Something in the environment lighting (the filtered environment of `ibl_spheres`, which the kiln shares?) reads memory that was not written, depending on what ran before it in the process: it is
in neither the occlusion nor this work (the pictures of the first baseline were green-free by luck of the allocation). The new binary's rate is not distinguishable from the old one's at four runs each. **Not investigated: worth a ticket**; and any A/B of these two scenes should be taken alone, or repeated.

#### Phase 2 in one table

| Item | 1280 x 720 | 1920 x 1080 | 3840 x 2160 | Pictures |
|---|---|---|---|---|
| 1 depth chain for the occlusion (from 1.5 M pixels) | not used (0 change, byte identical) | **-0.2 to -0.65 ms** of 1.2 to 3.6 (Sponza -0.28, crowd -0.5 to -0.64, kiln_near -0.14 to -0.34); loses +0.03 to +0.46 in far views | **-2 to -5 ms** of 8 to 20 (25 to 35%); crowd above +0.49 | mean <= 0.19, p99 <= 4, SSIM >= 0.9995 |
| 2 one blur for contact shadows alone | **-0.02 to -0.07** | -0.05 to -0.15 | -0.17 to -0.65 | mean <= 0.29, p99 <= 6, SSIM >= 0.9987 |
| 3 sharing the chain with the contact march | not worth it (the march is bound by reads, its arithmetic does not matter) | | | |
| 4 quality presets | none added: the cheaper options all fail the contract or save under 0.1 ms | | | |

**Not done or not tested.** Sponza Walk's panel (the click path, and the stats line's new "(depth chain)" note on screen) cannot be exercised here; the Signal Breach game itself (no switch forces the occlusion on); D3D12 and Metal (Vulkan on NVIDIA and lavapipe only); a display scaled by the desktop; devices without R32F storage (the chain pipelines are then not made and the old path runs: not provoked); timing on any GPU but this laptop's RTX 4080 (the gate's 1.5 M pixels is
this one's break-even: a device with a bigger cache or fewer cores would put it elsewhere).
**Tools added** to `kin_reference`: `--ab NAME[:A:B]`, `--flicker N`. The harness otherwise unchanged.

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

### The intermittent green pixels — investigation

(Written as the work went; the last lines are the conclusion.)

Symptom: `run.py render <label> --frames 120 --taa` (all reference scenes in one process) sometimes draws
`ibl_spheres` with a share of pure-green pixels (g >= 250, r and b < 120): 0.30% of `ibl_spheres/front` and 3.42% of
`grazing` in one run of three; the other runs clean. Without `--taa` never. Check script: share of such pixels (Pillow).

- Log (head 8890300, `--frames 120 --taa`, one process at a time, 1280x720):
  - 7 full runs (all 15 scenes): `ibl_spheres` clean in every one. A smaller set did not show it either at first.
  - **Reproduced**: the scene list `cornell,crowd,furnace,helmet,hidden,hidden_dense,ibl_helmet,ibl_spheres,sponza`
    (the full list minus the kiln scenes, motion, shadows, spheres; `ibl_spheres` is not even the last) gives the green
    in about 3 runs of 8 (the rate moves with time: 0 of 6, then 1 of 8 in later batches). The green pictures are
    **byte-identical to each other** (0.30% of `front`, 3.42% of `grazing`, the coordinator's numbers to the digit), so the
    wrong state is a deterministic wrong picture, reached at random, not random garbage.
  - What the green is: against a clean picture of the same run configuration, R and B are *exactly* equal in every
    pixel and only G is raised (by up to 200 levels), on the metallic spheres, growing toward the rim: a G-only error in
    the specular term.
  - **It is not TAA's history**: with `taa.blend = 0` (no history weight) the same frame is just as green
    (`KIN_REF_TAA_PROBE`, a temporary probe in kin_reference, removed again), so the wrong state is in the shaded frame and
    persists from one frame to the next, i.e. in a texture or buffer the shading reads (environment textures first).
- **Root cause found** (environment dumps, `source_L10` and `specular_L1` read back from a green run and from clean runs):
  1. The green run's *specular level 1* (the 512 x 256 level, roughness 1/6) is the clean one plus a constant 17.115 in G
     only, for every texel, bit-identical between green runs; levels 0 and 2 to 6, the irradiance and the source's levels 0 to 9
     are identical to a clean run.
  2. The environment's source picture (1024 x 512, 11 levels by `max(w, h)`) has a **last level (1 x 1) that nothing wrote**:
     green runs read (-0.00017, NaN, -0.00011, NaN) there, a clean run (12.86, 0.95, 11.44, 0.95), i.e. whatever the
     memory held (the two states are the old pictures' "A" and "B": the +-2 level differences between runs of
     `ibl_spheres` that every earlier agent saw were the same bug at a mild value).
  3. Why nothing wrote it: `SDL_GenerateMipmapsForGPUTexture` (Vulkan: `VULKAN_GenerateMipmaps`) blits each level into the
     next with `dstOffsets[1] = {width >> level, height >> level}`, no `max(1, ..)`: on a 2:1 picture the last level is
     1 x 0 and the blit is empty (so is every level after the shorter side reaches 1). kin counted levels by the *longer* side.
  4. Why it shows only in G, and only as one constant: the GGX filter of level 1 (256 importance samples, each read from
     the mip whose texels match the sample's solid angle) reads the 1 x 1 mip for exactly one sample (the grazing-most one,
     n.l = 0.434, 0.434 / 253.7 of the weight); `radiance()` clamps with `min(texel, 1e4)`, and GLSL's `min(NaN, x)` is `x`, so
     the NaN G becomes 1e4 and adds 1e4 * 0.434 / 253.7 = 17.1 to G (R and B, small and negative, add nothing). Every texel
     has the same sample set rotated to its normal, so the offset is the same for every texel and every green run is the same picture.
     With stale values (12.86, ., 11.44) the same sample adds +0.022 to R and B instead: the mild +-2 flip.
  5. The same applies to any non-square mipmapped texture made by `GpuDevice::make_mipmapped` (model textures, 2D textures set to
     `ScaleMode::Mipmapped`): the levels after the shorter side reaches 1 are never written; they are read only when a
     texture is minified very far.
  Why TAA and the other scenes: TAA was a coincidence of memory layout (it allocates targets before the environment). The
  poison switch did not catch it at first because `update_texture(..., whole = true)` cycles the texture (fresh,
  unpoisoned memory) after the poison upload.
- **A second, separate cause: the kiln flake** (`kiln/close`, `kiln/game`, `kiln_near/grazing`: a few pixels, up to 5 to
  18 levels, e.g. one pixel row of six, an MSAA edge). It is not the environment (no environment, no textures involved) and not
  memory: `kiln` alone is identical in 4 of 4 runs, also with `KIN_POISON_TEXTURES=2 KIN_POISON_BUFFERS=1`; in a process
  that rendered other scenes first it differed in 3 of 3 (after the mip fix too). `order_draws` ended its sort with
  `return x.mesh < y.mesh;`: a **heap address**. Two draws whose surfaces tie in depth (the kiln has overlapping geometry)
  are drawn in the order of their meshes' addresses, which changes with whatever the process allocated before. Sorting by
  `GpuMesh3D::serial` (the order the meshes were made, which an address usually follows when nothing was freed) instead:
  `crowd,ibl_helmet,ibl_spheres,kiln,kiln_near` identical in 4 of 4 runs, kiln included (3 of 3 differed before).
- Tool added while looking: `KIN_POISON_TEXTURES=1|2` and `KIN_POISON_BUFFERS=1` (see "Poison switch" below). With it on
  the clean-run pictures did not turn into poison: no texture the 3D renderer makes is read *before it is written* in the
  frames of these runs (so the cause is not a plain read of never-written memory).

**The fix** (two changes, the second a separate bug):

1. `GpuDevice::complete_mip_levels(w, h)` (levels down to where the *shorter* side is 1; a 1024 x 512 picture: 10, the last 2 x 1)
   is the level count of every mip chain SDL generates: `make_mipmapped` (model textures and 2D textures set to
   `ScaleMode::Mipmapped`) and the environment's source picture. Levels 0 to 9 are the bytes they always were. The 1 x 1 level
   an environment used to have (never written) is gone: the sampler clamps to the 2 x 1 level. A mipmapped texture 1 pixel wide or
   tall now has no mips (SDL's chain was empty for it anyway). `make_mipmapped` no longer calls the generator for a one-level texture.
2. `order_draws` breaks ties by `GpuMesh3D::serial` (the order the meshes were made) instead of the mesh's pointer.

**Poison switch** (kept; no cost when unset beyond one `getenv` per texture or buffer creation, none per frame):
`KIN_POISON_TEXTURES=1` fills every texture made by `GpuDevice::create_target`, `create_target_array`, `create_depth_stencil`, the environment's three
textures and the levels `make_mipmapped` is to generate, before its first use, through the upload command buffer: NaN in float texels, white
(0xFF) in 8-bit ones, depth 0.37 and stencil 0x5A; `=2` uses 3e4 instead of NaN. `KIN_POISON_BUFFERS=1` fills buffers made by `create_buffer` with no
data and the cluster chunks with 0xFF bytes. Read at each creation, so a test can set it around one call. Limits: a texture that is cycled afterwards
(an upload or a pass with `cycle = true` while it is still in use) gets fresh memory and loses the poison, which is why the environment source and
`make_mipmapped` poison their levels again after level 0's upload, and why the poison alone did not show this bug at first.

**Tests** (both fail with the fix reverted, to the same message each time, and pass with it, on NVIDIA and lavapipe):
`test_gpu_environment_levels_written` (render3d_tests: a flat 0.25 environment of 512 x 256 made with `KIN_POISON_TEXTURES=1`; a white mirror-ish sphere must
not exceed 0.30 linear (with the bug: 1.0), must not be black in the middle, and must be byte-identical to the same render with no poison, at roughness 1/6, 0.5 and 1) and
`test_gpu_mipmaps_not_square` (renderer_tests: red textures of 256 x 64, 64 x 256, 256 x 16 and 100 x 36 made mipmapped with and without the poison, drawn into a single
pixel: red, not white; with the bug the 256 x 64 one draws (255, 255, 255)).

**Measured after the fix** (the old binary is this tree with `complete_mip_levels` counting by the longer side and the sort by pointer again; both
binaries built from one tree; one process at a time, interleaved old/new, 1280 x 720, `--frames 120`, all 15 scenes, the kiln scenes included; "differ" is
byte comparison against the commonest picture of that binary's runs):

| 10 runs each | old binary | new binary |
|---|---|---|
| default: green `ibl_spheres` | 1 of 10 | 0 |
| default: pictures that differ between runs | `ibl_spheres/front` and `grazing` (3 versions, 4 of 10 runs), `kiln/close` (4 versions, 7 of 10), `kiln/game` (6 versions, 6 of 10), `kiln_inside/behind` (5 of 10), `kiln_near/far` (5 of 10), `kiln_near/grazing` (5 of 10) | **none: all 36 pictures identical in 10 of 10 runs** |
| `--taa`: green `ibl_spheres` | 1 of 10 | 0 |
| `--taa`: pictures that differ | `crowd/above` (2 of 10), `ibl_spheres` (3 versions, 5 of 10), `kiln/close` (4 versions, 5 of 10), `kiln/game` (2 of 10), `kiln_near/far` (5 of 10), `kiln_near/grazing` (4 of 10) | **none: identical in 10 of 10** |
| `kiln_near --lod --lod-clusters --taa` | `far` (3 versions, 6 of 10), `grazing` (4 of 10) | **none: identical in 10 of 10** |

Every "known flake" of the earlier notes (the kiln between 72 and 90 dB, `kiln_near/grazing`, `kiln_near/far`, `crowd/above` under TAA) is gone
and none was load-dependent: they were the pointer sort (all but `ibl_spheres`) and the unwritten mip level (`ibl_spheres`, and `ibl_helmet` at
a lower amplitude). Ten runs of each is evidence, not proof: the old green appeared in 1 run of 10 in these batches and in 3 of 8 or 1 of 3 in others, so a
cause that fired once in 20 runs would not necessarily show; the two causes found are each reproduced, switched off and re-tested, and nothing remains unexplained.
`crowd/above` under all-on settled in the picture the notes called the quiet one (6ce1cac0); the kiln's pictures settled in one of their old states.

**What the fix changes in a picture**: the ibl pictures only, and by one level or less in a few thousand pixels (the 1 x 1 level they read was stale memory; now it
is the 2 x 1 level): `ibl_spheres` default 3,162 and 10,045 pixels of `front` and `grazing`, `ibl_helmet` 1,852 and 399, max 1 level (all-on settings: max 2 to 4);
every other picture of the 11 scenes of `sp2_before_def` and `sp2_before_all` is byte-identical (24 of 24 except those four and `kiln/close`, `kiln/game`, which were flaky
before: they now come out the same in every run).

**Not found, not tested**: D3D12 and Metal (the trimmed chains are right for any backend; whether SDL's other backends have the same empty blit was not looked at); the
Sponza Walk panel click path; a NaN scrub in the TAA resolve was *not* added: `taa.comp`'s `history_at` ends with `max(sum / total, vec4(0))`, which turns a NaN in the
history into 0 on NVIDIA before the shader's own `isnan(history)` test can see it (so that test is dead), and `env_filter.comp`'s `min(texel, 1e4)` turns a NaN
texel into 1e4. Neither caused the green (the NaN came from the unwritten level, not from TAA), and neither is changed here.

## A full 3D game: a Peak-like game (stage P)

The user's direction (2026-10-10): stop at what G2 and G3 gave the renderer, park ray
tracing (G4), and build what a whole 3D game needs, aimed first at a game like *Peak*
(a co-op climbing game: a stylised animated character that grabs and climbs mountain
segments, stamina and status effects, weather, ropes and items). An Astroneer-like game
(voxel planets the player digs, base building, vehicles) is a separate, later track that
needs a terrain system on top of everything here (see the end of the section).

Decisions taken: **Peak-like first**; **one player first, netcode-ready** (the simulation
kept apart from rendering and input, fixed step, input as commands, state hashed as
`kin/runtime/state_hash.hpp` already does, so that co-op can be added by a later step
without a rewrite); **Jolt Physics approved** as a dependency (MIT, fetched like
meshoptimizer, built only with `KIN_EXPERIMENTAL_3D`).

What exists (so what the steps build on): the renderer (materials, shadows, lights, detail
reduction for big scenes, pick, custom shaders), the ECS with `Transform3D` and
`MeshRenderer` (`kin/ecs/render3d.hpp`), a deterministic fixed-timestep core, `kin/anim`
(clips and state machines over registered properties: not skeletal), `kin/audio` with
spatial sound, `kin/particles` (2D), ui2, scripting, saves, prefabs. What does not: skinned
meshes (the glTF loader reads nodes and meshes, not skins or animations; `MeshVertex` has
no joints), 3D physics (`kin/physics` is Box2D), gamepad input in `platform`, a sky,
fog, spot lights, 3D particles and decals, a baked asset cache, and any networking
(the only server in the engine is the agent interface).

### P1 — Skeletal animation (the biggest gap)

*P1a, skinned meshes and clips.* The loader reads skins (joints, inverse bind matrices) and
animations (translation, rotation, scale channels; step, linear, cubic spline) and the
`JOINTS_0` / `WEIGHTS_0` attributes; a `Model` gains a `Skeleton` and its clips. Skin
data is a separate vertex stream (four joint indices, four weights), so a mesh without it
is untouched and every existing picture stays byte for byte. At run time: a `Pose` (local
translation, rotation, scale per joint), a player that samples a clip and blends two, with
layers and masks (an upper-body layer over locomotion), additive clips, and root motion
taken out; the joint matrices (world pose times inverse bind) are written per frame into a
storage buffer and a copy's offset into it is a small per-copy value in a buffer of its own
(like TAA's previous transforms: `GpuInstance3D` stays 128 B). GPU skinning in every pass
that draws meshes (main, shadow cascades, pre-pass, TAA velocity from the previous
frame's joint matrices, pick); bounds for culling from the skeleton or a per-clip bound;
skinned meshes are excluded from levels and clusters (G3), and say so.
API: `Scene3D::draw_skinned(...)`, ECS `SkinnedMeshRenderer` and `Animator3D`; the existing
state machines choose clips and blend weights (a 1D blend by speed first).
Checks: the glTF sample models (CesiumMan, Fox, BrainStem: fetched, not committed) load; the
skinned picture equals a CPU skinning reference to a tolerance; blends, masks, additive
and root motion by unit tests on poses; shadows, velocity (no ghosting on a running
character under TAA), pick and culling by pixel tests; 200 skinned characters of a few
thousand triangles cost under 1 ms of GPU (measure and record); mutation checks on each.

*P1b, inverse kinematics.* A two-bone solver (an arm reaching a hold, a leg to the ground),
look-at, and a foot-placement helper, as pure maths with unit tests, applied to a pose
after the clip; the hand-to-hold use is what climbing needs.

*P1c, a viewer.* An example (or a Sponza Walk section) with a skinned character, a clip
list, blend sliders and the IK target, as the means to look at it and to test headless.

#### P1a — skinned meshes and clips, part 1 — results

Done 2026-10-10 on `exp/3d-rendering` (head 1576a73 plus the working tree). Part 2 (velocity for TAA, the pre-pass and
ambient occlusion, pick, the ECS `Animator3D`) is below, "P1a part 2 - results".

**What was built.**

| Piece | Where | Notes |
|---|---|---|
| `Skeleton`, `Pose`, `JointPose`, `PoseMask`, `compute_joint_matrices`, `skin_mesh` (CPU skinning reference), per-joint bounds (`compute_skin_bounds`, `skin_sphere`) | `kin/render3d/skeleton.hpp`, `src/render3d/skeleton.cpp` | CPU only. Parents come first (validated); at most 256 joints. |
| `AnimClip3D`, `Track3D`, `sample_clip`, `blend_poses`, `blend_masked`, `apply_additive`, `extract_root_motion`, `remove_root_motion`, `AnimationPlayer3D` | `kin/render3d/animation3d.hpp`, `src/render3d/animation3d.cpp` | glTF's step / linear / cubic-spline exactly; slerp for rotations; the player has a base layer with crossfade, layers with masks and weights, additive layers and root motion. Does not touch `kin/anim`. |
| glTF skins, `JOINTS_0` / `WEIGHTS_0`, animations | `src/render3d/model.cpp` (`GltfReader`), `Model::skeletons`, `Model::clips`, `ModelPart::skin`, `read_model_data` | Joints reordered parents-first with the vertices' indices remapped; weights renormalised; a mesh on a node under a joint becomes a rigid skinned part; morph channels and animations of non-joint nodes are logged and ignored; skins over 256 joints are rejected and their meshes drawn static. |
| `MeshData::skin`, `Mesh::skinned()`, `Mesh::skin_joint_count()` | `mesh.hpp` | A parallel stream of 4 joints and 4 weights; `validate`/`append` know it; levels and clusters refused for a skinned mesh (logged). |
| `Scene3D::draw_skinned` (mesh and model), `joint_matrices()`, `skin_offsets()` | `scene3d.hpp` | Matrices copied into the scene; copies of one mesh and material one after another are one draw. |
| GPU skinning, main and shadow passes | `gpu_renderer3d.cpp`; `mesh_skinned.vert`, `mesh_skinned_velocity.vert`, `shadow_skinned.vert`, `shadow_mask_skinned.vert`, `kin_skin.inc.glsl`; `GpuVertexLayout::SkinnedMeshVertices` | A second vertex buffer (8 B a vertex: 4 joint bytes, 4 unorm weight bytes summing to 255); the frame's matrices in one storage buffer, each copy's first matrix a `u32` in a buffer of its own, in the places the instances are (`GpuInstance3D` is still 128 B). |
| Harness | `kin_reference`: `models[].animation {clip, time, loop, speed, root_motion}`, `grid {count, columns, spacing, time_step}`; `make_scenes.py`: `skinned_bend.glb`, `skinned_dense.glb`, `floor20.glb`; `fetch.py`: `cesium_man`, `fox`, `brainstem` (single `.glb`, pinned, locked; `--lock NAME` locks only the new ones) | Scenes `skinned_bend`, `skinned_cesium`, `skinned_fox` (these two `optional`), `skinned_crowd` (speed). |
| Tests | `tests/skeleton_tests.cpp` (new target `kin-skeleton`, CPU only, 16 tests), `tests/skin_fixtures.hpp` (a tube on three joints; a glTF made in code), 10 GPU tests in `tests/render3d_tests.cpp` (`kin_render3d_tests skinned` runs just them) | None needs a download. |

**Where the vertex shader differs.** `skin = sum_i w_i M[joint_i]` (a weighted sum of the matrices); position `model * (skin * p)`; normal
`normalize(mat3(skin) * n)`, then the copy's normal matrix as before. Valid for rigid and uniformly scaled joints; a non-uniform scale in a joint is
approximated (the exact matrix is the inverse transpose of the sum): said in the shader and the docs.

**Culling.** Each joint has a sphere round the vertices that pull on it at all (weight above 0), centred on their box, made at `create_mesh`. A copy's
bound is the sphere round those spheres moved by its joint matrices (each radius times the matrix's stretch, bounded by Gershgorin on `A^T A`:
exact for rotations, conservative for anything else). A posed vertex is a convex mix of the places its joints would put it, and each of those lies in
its joint's moved sphere, so no vertex leaves the bound; it is the one used for the camera frustum test, the sun-only casters and the cascades. The
unit test throws 500 random poses (some with non-uniform scale) at a tube and asserts every posed vertex is inside; the GPU culling test has a copy
whose rest bound is above the view and whose pose swings it in.

**Results.**

| Check | Result |
|---|---|
| `ninja -C build -j8`, `ninja -C build-no3d -j8` | build; 3D-off still builds (new sources are only under the option) |
| `ctest` in `build` with `KIN_REQUIRE_GPU_TESTS=1` | 69 of 69 (68 + `kin-skeleton`), NVIDIA RTX 4080 |
| `ctest` in `build-no3d` | 65 of 65 |
| `kin_render3d_tests` full | passes on NVIDIA (VK_ICD_FILENAMES unset) and on lavapipe |
| Compiled SPIR-V of every existing shader | 64 of 64 byte-identical (`cmp` before and after; the four skinned shaders are new files) |
| Off-path pictures (default, and `--cascades 4 --ao --contact-shadows --taa` over crowd, shadows, sponza, helmet, motion, spheres, cornell; kiln_near with `--lod --lod-clusters`) | 37 of 37 byte-identical to the labels rendered from the tree before the change; a second render of the baseline matched itself |
| Skinned copy vs the same mesh skinned on the CPU and drawn plain (perspective and orthographic, 1 and 3 cascades; lit, normals skinned, shadowed) | 0 of 19,200 pixels differ by more than 4 levels in each of the four pictures |
| Shadow of a posed copy the camera does not see vs the CPU-skinned mesh's (1 and 3 cascades), cut-out shadow, five poses of one mesh in a frame with a plain mesh between, scene and overlay | 0 pixels differ in each |
| Workers vs serial, 2,600 / 40 / 3,000 / 1 copies in different poses (joint buffer growing and shrinking) | byte-identical |

**Mutation checks** (each: break one thing, see a test fail, revert):

| Break | Caught by |
|---|---|
| wrong joint order in the shader (joint 0 and 1 swapped) | `test_gpu_skinned_matches_cpu` (395 pixels differ) |
| inverse bind matrix not applied in `compute_joint_matrices` | `test_joint_matrices` (rest matrices not the identity) |
| weights not renormalised in the loader | `test_loader_skins_and_clips` |
| wrong offset in the side buffer (every copy reads copy 0's matrices) | `test_gpu_skinned_poses_and_mix` (934 pixels) |
| normals not skinned | `test_gpu_skinned_matches_cpu` (331 pixels) |
| cull bound ignoring the pose | `test_gpu_skinned_culling` (the arm that swings in is culled) |
| shadow pass using the rest pose | `test_gpu_skinned_shadow_out_of_view` (72 pixels) |
| cut-out shadow variant using the rest pose | `test_gpu_skinned_cutout_shadow` (138 pixels) |
| joint bound ignoring a matrix's stretch | `test_skin_bounds_hold_the_posed_vertices` |
| bound from only each vertex's strongest joint | `test_skin_bounds_count_every_influence` (added after this one slipped through the first version of the tests: the sphere's slack hid it) |

All ten were caught. One real bug was found by the tests while writing them (the draw's first side-buffer entry was off by one; the
GPU picture was empty), which is why `test_gpu_skinned_off_path` also asserts the scene's own `skin_offsets()`.

**Cost.**

| Measure | Value |
|---|---|
| Existing paths, crowd scene, before (tree before the change, built beside) and after, 6 rounds alternating order, median | first_person CPU 0.300 / 0.291 ms, GPU 1.647 / 1.645; mid_deck 0.232 / 0.233, 1.179 / 1.156; above 0.239 / 0.239, 2.338 / 2.132: unchanged within noise (the GPU clock drifts 30% over a session; pairs alternate for it) |
| 200 skinned copies of a 4,096-triangle mesh (3 joints, 819,212 triangles drawn, 1280 x 720, 4x MSAA), GPU, no shadows | 0.25 to 0.27 ms (the same triangles as plain instances: 0.21 ms, so skinning adds 0.04 to 0.06 ms) |
| ... with the sun's shadow | 0.39 to 0.42 ms (plain instances: 0.35 ms) |
| `render()` CPU, harness, with workers | 0.08 to 0.15 ms (plain instances 0.04 to 0.05 ms); 0.036 to 0.040 ms for a 2,832-triangle mesh in the test, serial (0.18 to 0.20 on lavapipe) |
| CPU animation a character a frame, 24 joints, 3 tracks of 30 keys: `sample_clip` | 1.6 us; joint matrices 0.86 us; `AnimationPlayer3D::update` + matrices 2.4 us; with a crossfade 4.4 us |
| ... the real Fox (24 joints) | 1.8 us; Walk-to-Run crossfade 4.6 us |
| 1,000 such characters a frame | about 2.4 to 4.6 ms of one core; per character it is far below a frame's budget |

200 skinned characters of a few thousand triangles cost under 1 ms of GPU (0.4 ms with the shadow): the spec's goal is met.

**What I looked at** (`kin_reference` scenes; no Cycles truth, which would show only that skinning is skinning: they are judged against the CPU skinning
reference above and by eye).
- *skinned_bend*: four tubes at four moments of the `bend` clip, a fifth with `wave`. Smooth normals follow the bend; shadows follow the pose. At the
  elbow of a hard bend (70 and -55 degrees) the tube pinches: the volume loss of linear blend skinning ("candy wrapper" in the extreme, here
  mild). It is the technique, not a bug; dual quaternions or corrective joints are the answer if a game needs them.
- *skinned_cesium*: CesiumMan walking, three moments: legs and arms swing, no stretched triangles, no popping. The disc on its head is the model's own
  texture (a placeholder image icon), not a fault.
- *skinned_fox*: Survey, Walk and Run, 24 joints: a quadruped with a tail; clean in all three, no cracks at the joints; the flat-shaded low poly look
  is the model's.
- *BrainStem* (59 skinned parts, 18 joints; not committed as a scene: it loads, assembles into the robot and animates; its animation also targets
  nodes that are not joints, which are ignored and logged).
- *skinned_crowd*: 200 copies in different poses, as measured above.

**Honest limits (part 2 must do these).**
1. **Velocity.** A skinned copy under TAA has the camera's velocity only (`mesh_skinned_velocity.vert` takes the copy as not having moved), so a
   moving pose ghosts slightly and a still pose under a moving camera is right. Part 2: keep last frame's joint matrices (a second buffer, matched by
   `MeshInstance::id` or draw order as the copies' previous transforms are) and read them in a skinned `prepass_velocity.vert` and in the main-pass
   velocity variant. When ambient occlusion or contact shadows are on (the velocity is then written by the pre-pass, which has no skinned
   variant) a skinned pixel keeps the pre-pass's clear value, no motion at all, so it ghosts under camera motion as well.
2. **Pre-pass, AO, contact shadows, occlusion culling's occluder depth.** Skinned copies are skipped there (they are not in AO, they do not
   shade or occlude it). Part 2: `prepass_skinned.vert` and `prepass_velocity_skinned.vert` from the same include, and the shadow variants for the occluder pass.
3. **Pick.** `pick.frag`'s vertex stage is `mesh.vert`; a skinned variant is the same include. Until then a skinned copy is invisible to `pick`.
4. **The shadow cache** (`cache_shadows`) is switched off for any frame with a skinned caster: a pose is not in the copies it compares. Part 2: hash the
   copy's matrices into the key.
5. **Non-uniform scale** in a joint skins normals approximately.
6. **A warn-once line** says all of the above when a frame has a skinned copy and one of those features.
7. **Joint matrices of a copy are copied per call** (`draw_skinned` copies `skin_joint_count()` matrices into the scene): a model of 59 parts (BrainStem)
   copies the pose 59 times, 18 matrices each; a per-model pose shared by its parts would save it.
8. **Not tested:** D3D12 and Metal (Vulkan on NVIDIA and lavapipe only; a second vertex buffer and four storage buffers in a vertex shader are
   ordinary there); skins of more than 4 influences (the strongest four of `JOINTS_0` are used, `JOINTS_1` is ignored with a log line); sparse accessors in
   skinned primitives; morph targets; several skeletons in one model drawn with one call (`draw_skinned(model, ...)` poses skeleton 0 only;
   parts bound to another skeleton are skipped, draw those with the mesh overload).
9. **Fit and bounds.** `Model::bounds` is the rest pose's; a character far from it needs its own bound for a fitted camera.
10. **Where this slots in part 2.** `AnimationPlayer3D` is a plain object; an ECS `Animator3D` component owns one (or a clip index and time) and a
    system calls `update`, `joint_matrices(span)` into the frame's scratch, and `draw_skinned`. `kin/anim` state machines choose the clips
    and weights: a parameter-driven 1D blend by speed can be a `crossfade_to` or two layers with weights.

#### P1a — skinned meshes and clips, part 2 — results

Done 2026-10-10/11 on `exp/3d-rendering` (head a4599e5 plus the working tree; not committed). Everything is opt-in: scenes with no skinned copy
are byte for byte as before, in pictures and in the compiled SPIR-V of every earlier shader. (The first session ended in a reboot; the tree was intact
and the checks below were re-run after it.)

**What was built (the spec's items).**

| # | Piece | Where | Notes |
|---|---|---|---|
| 1 | Previous-frame joint matrices and velocity | `gpu_renderer3d.cpp` (`match_copies`, `cull_and_upload`); `kin_skin.inc.glsl` (`KIN_SKIN_PREVIOUS`), `kin_mesh_vertex.inc.glsl`, `mesh_skinned_velocity.vert`, new `prepass_velocity_skinned.vert` | The view's record keeps the frame's whole joint array and each copy's range in it; `match_copies` now also returns each copy's place in last frame's record (by id, by order, by nearest) and the skinned shaders read last frame's array and the per-copy start in it. No past pose (history unusable): posed as now, camera-only velocity; no past copy: the `fresh` flag, velocity far off screen as for a plain copy. Both routes (main-pass velocity, and the pre-pass's with AO/contact/occlusion) and the overlay. |
| 2 | Pre-pass, AO, contact shadows, occluder depth, cut-outs | new `prepass_skinned.vert`; `Pass::Prepass`/`Occluder`/`velocity_pipeline` take `skinned`; the occluder pass uses the existing `shadow_skinned.vert` / `shadow_mask_skinned.vert` | The part-1 warn-once is gone (nothing is left that it said). |
| 3 | Pick | `mesh_skinned.vert` as the pick vertex stage (it writes the `vWorld` and `vId` that `pick.frag` / `pick_mask.frag` read) | No new shader file was needed. |
| 4 | Shadow cache | `shadow_unchanged` / `shadow_drawn` | A copy's matrices are in the sample (cheap key) and in the snapshot compared byte for byte; the mesh's serial (its skeleton) was already in the key. |
| 5 | Shared joint range | `Scene3D::draw_skinned(model, ..., id)` | One range for all parts; BrainStem: 18 matrices uploaded (was 59 x 18 = 1,062). |
| 6 | ECS | `kin/ecs/render3d.hpp`, `src/ecs/render3d.cpp` | `SkinnedMeshRenderer` (its own component, not an extension of `MeshRenderer`: mesh renderers merge into one instanced draw of one mesh, a character is several parts with its own pose and skeleton), `Animator3D`, `WorldRenderState3D::animate(dt)`. The state machine is `kin/anim`'s `AnimationStateMachine` and `AnimationParams`, run with its own `evaluate()`: no change to `kin/anim`, no adapter beyond the component. 1D blends and crossfades: `AnimationPlayer3D::play_blend`, `crossfade_to_blend`, `set_blend_parameter` (a blend's time is a phase 0 to 1 advanced by the blended duration; root motion of a blend is the mix of its clips'). |
| 7 | Harness and viewer | `kin_reference`: `models[].animation.blend {parameter, parameter_to, parameter_seconds, points[{clip, position}]}` and `animation.ecs`; scenes `skinned_blend` and `skinned_ecs`; Sponza Walk "Character (skinned)" section (`examples/sponza_walk/character.hpp`, a procedural 3-joint character with clips Sway, Bend, Wave and the blend), presets `character`, `character-all`, a skinned line in the stats | The panel's click path is untested (no display). |

**Results.**

| Check | Result |
|---|---|
| `ninja -C build -j8`, `ninja -C build-no3d -j8` | build |
| `ctest` in `build`, `KIN_REQUIRE_GPU_TESTS=1` | 69 of 69 |
| `ctest` in `build-no3d` | 65 of 65 |
| `kin_render3d_tests` | passes on NVIDIA (VK_ICD_FILENAMES unset) and lavapipe; `kin_example_tests`, `kin_skeleton_tests` pass |
| SPIR-V of the 62 earlier non-skinned shaders, built from HEAD's sources with glslc and compared with the build's | 62 of 62 byte-identical; of the part-1 skinned ones `mesh_skinned_velocity.vert.spv` changed (it now has the previous joints), two new files |
| Off-path pictures: default and `--cascades 4 --ao --contact-shadows --taa` over crowd, shadows, sponza, helmet, motion, spheres, cornell; kiln_near with `--lod --lod-clusters` | 37 of 37 byte-identical to `p1a_base_*` |
| Velocity: skinned vs the CPU-skinned plain mesh moved the same rigid way (translation in the pose, half in pose and half in placing, rotation, camera moving over a still pose; by order and by id; main-pass and pre-pass routes; overlay; cut-out), 14 frames under TAA, a checker texture so that a wrong history shows | 0 of 19,200 pixels differ by more than 8 levels in every frame of all 11 cases |
| A copy that appears; two characters that swap draw order, matched by id | 0 pixels differ |
| AO + contact shadows, skinned vs CPU-skinned (opaque and cut-out, 1 and 3 cascades) | 0 pixels differ; the features change about 910 pixels, so they are on the skinned copy |
| Occlusion culling with a skinned wall (opaque, cut-out, with AO + contact, with TAA) | 69 clusters occluded of 69, as a plain wall; picture with and without culling byte-identical |
| Pick: 540 pixels, three posed copies, opaque and a cut-out with a hole | identical ids to the plain meshes; 74 of 75 posed-axis points name their copy; empty space none |
| Shadow cache: still pose hits; one copy bends, copy 7 of forty bends, another mesh on the same skeleton and pose, another skeleton, cascades | each misses, the picture is the uncached render's, and it arms again |
| Shared range vs a call a part (also with AO + TAA) | 3 matrices against 9; 0 pixels differ |
| ECS: 30 characters, 40 fixed steps, two runs, one worker; 2,200 characters serial vs workers; destruction | same pictures; copies of destroyed entities gone |
| Everything (cascades 4, AO, contact, TAA, pick) at 400 and 2,600 skinned copies, serial vs workers | byte-identical |
| CPU tests (`kin-skeleton`): blend maths, shared pose, animator clip, state machine + fades + blend space + trigger, layers, root motion, determinism | pass |

**Mutation checks** (each: break one thing, see a test fail, revert; the tree was compared with copies afterwards).

| Break | Caught by |
|---|---|
| main-pass velocity uses the current pose as the past | `test_gpu_skinned_velocity` (424 pixels) |
| pre-pass velocity uses the current pose | same (426) |
| the velocity pre-pass skips skinned copies | same (426) |
| the plain pre-pass skips skinned copies | `test_gpu_skinned_prepass_features` |
| the occluder pass skips skinned copies | `test_gpu_skinned_occluder` (0 occluded of 69) |
| the pick pass skips skinned copies | `test_gpu_skinned_pick` (38 pixels, ids 0) |
| shadow cache ignores poses (key and compare) / only the full compare | `test_gpu_skinned_shadow_cache` (the second step; the "copy 7" step) |
| the mesh serial left out of the key | same (after a step was added: the first version changed the pose too and hid it) |
| past joints matched by place while the model matrix is matched by id | `test_gpu_skinned_velocity_new_and_reordered` (172 pixels) |
| a new skinned copy loses its fresh flag | same (153 pixels) |
| a part reads another matrix of the shared range | `test_gpu_skinned_shared_range` |
| the animator ignores the fade table / the blend parameter | `test_ecs_animator_state_machine` |
| a blend's phase advances by the first clip's duration | `test_player_blend_1d` |

Two test-design lessons: `flecs::entity::set(lvalue)` moves from a non-const lvalue (the animator was emptied; the tests and the harness
set from a const reference now: said in the docs), and a replay test that reused such an animator passed vacuously until that was found.

**Cost** (NVIDIA RTX 4080 Laptop, 1280 x 720, medians of two runs, GPU clock drifts, so deltas are indicative).

| Measure | Value |
|---|---|
| crowd (existing path), CPU / GPU first_person, mid_deck, above | 0.23 / 1.22, 0.21 / 0.67, 0.22 / 1.36 ms (three runs). Part 1 recorded 0.29 / 1.65, 0.23 / 1.16, 0.24 / 2.1 on another day: no "before" binary was built beside this time (the disk), so this is not an interleaved A/B; the pictures and SPIR-V are identical, which is the stronger evidence |
| 200 skinned copies, 819,212 triangles, 4x MSAA, sun shadow | GPU 0.31 ms, CPU render() 0.09 ms |
| ... + TAA (main-pass velocity) | 0.40 ms (+0.09), CPU 0.11 |
| ... + ambient occlusion | 0.65 ms (+0.34), CPU 0.10 |
| ... + contact shadows | 0.56 ms (+0.25), CPU 0.10 |
| ... all (4 cascades, AO, contact, TAA) | 0.94 ms, CPU 0.15 |
| BrainStem (59 parts, 18 joints) | 18 matrices uploaded a frame; render() 0.10 ms |
| Joint history | the frame's array is copied into the view's record and uploaded again: 2 x 64 B per joint per frame (a thousand 24-joint characters: 1.5 MB twice) |

**What I looked at.** `skinned_blend` and `skinned_ecs` (four tubes at blend 0, 0.33, 0.66 and a ramp; the ECS picture equals the direct one on the
three constant ones to sub-pixel float accumulation of 250 steps, and the ramped one differs on purpose: the entity integrates the phase while the
parameter moves, the direct sampler applies the frame's value to the whole history); the velocity test pictures (a rotating checkered tube
under TAA: skinned and plain frames indistinguishable); AO creases and the contact shadow at the foot of a skinned tube by a wall; Sponza Walk
with five characters, AO, contact shadows, TAA and four cascades (the stats line shows the skinned copies).

**Honest limits.**
1. The velocity tests compare rigid moves of the pose (translation, rotation): they exercise the whole previous-joint plumbing and matching, but a
   bending pose's velocity is not compared with an independent reference (there is none: a CPU-skinned plain mesh has no per-vertex past). The shader
   uses the same expressions for both.
2. No interleaved before/after of the crowd (see Cost); the off path is argued by identical pictures and SPIR-V.
3. A copy matched by order when the count of its mesh changes is a guess (the nearest within twice its size, else new): give moving characters an
   id. The ECS gives the entity's id.
4. A custom material shader on a skinned copy works (the stock skinned vertex shader) but with a custom opaque shader TAA keeps the pre-pass, as for plain meshes.
5. No prefab or component-schema support: Transform3D and MeshRenderer have none either (the 3D components are not registered anywhere), so "prefab round trip" is not applicable.
6. `kin/anim` events and registered-property tracks are not bridged to skeletal clips; `Animator3D` copies in a Sponza Walk row play in step (no time offset field).
7. Non-uniform scale in a joint skins normals approximately; D3D12 and Metal untested; a skin of more than 4 influences is cut to 4.
8. Sponza Walk's panel clicks were not exercised (no display); the section was run headless through `--preset`.

**What P1b and P1c still need.** P1b (IK) works on a `Pose` after the player: `Animator3D` exposes `player.pose()` but has no hook between the clips and the
matrices; it needs one (a callback or a list of IK targets by joint name, applied before `compute_joint_matrices`). P1c wants a time offset and a start
phase on the animator, event keys on skeletal clips (footsteps) and the real model path in Sponza Walk (the viewer's character is procedural).

#### P1b — inverse kinematics — results

Done 2026-10-11 on `exp/3d-rendering` (head 5b66837 plus the working tree; not committed). All of it is CPU maths plus the ECS hook: no shader
or renderer change. Opt-in: an `Animator3D` with no constraints is exactly what it was, and every earlier picture is byte for byte.

**What was built (the spec's items).**

| # | Piece | Where | Notes |
|---|---|---|---|
| 1a | `solve_two_bone` | `kin/render3d/ik.hpp`, `src/render3d/ik.cpp` | pole by direction or point, weight, soft stretch, minimum bend, end-orientation target, intermediate (twist) joints carried; solved in the root joint's parent space |
| 1b | `solve_look_at` | same | chain with shared turn, per-joint yaw / pitch limits (ellipse), release toward forward past a limit, smoothing in a `LookAtState` (the function takes `dt`; the Animator keeps the state, in world space) |
| 1c | `solve_chain_fabrik` | same | done: tolerance, iteration cap, fixed or free root; bone lengths kept |
| 1d | `plan_foot_placement`, `apply_foot_placement` | same | `GroundQuery` = `std::function<optional<GroundHit>(Vec3f, Vec3f)>`; targets, orientation, pelvis offset, `FootPlacementState` (pelvis eased both ways, a foot eased down only) |
| 1 | partial matrix updates | `update_world_below`, `world_to_joint_matrices` | only the joints below a changed one are recomputed; the shader's matrices are one multiplication a joint from the world matrices |
| 2 | ECS hook | `kin/ecs/render3d.hpp`, `src/ecs/render3d.cpp` | `IKConstraint`, `Animator3D::{ik_constraints, add_ik, ik(name), find_ik, ik_pose, ik_world}`, `WorldRenderState3D::apply_ik()`; targets: world point, entity (+ local offset), none; pole: world point or model direction; weight goal + fade (linear, smoothstep-shaped) |
| 3 | bounds | none needed | the bound is made from the posed joint matrices, which include the IK: `test_gpu_ik_not_culled` |
| 4 | harness, viewer | `tools/reference/kin_reference.cpp` (`animation.ik`), `make_scenes.py` (`skinned_biped.glb`, `ik_marker.glb`, `ik_step.glb`), scenes `skinned_ik`, `skinned_ik_look`; Sponza Walk "Character IK" section, presets `ik`, `ik-reach`, `ik-all` | procedural person (CesiumMan's joint names are not limbs a rig can name); the viewer's person is `make_person` in `character.hpp` |
| 5 | tests | `tests/skeleton_tests.cpp` (16 CPU tests and a bench, `kin-skeleton`), `tests/render3d_tests.cpp` (5 GPU tests, `kin_render3d_tests test_gpu_ik`), `tests/skin_fixtures.hpp` (`make_biped`, `make_biped_body`, `make_joint_cube`) | see below |

**Design decisions** (the reasons are in the code comments):
- *Solved in the space of the root joint's parent*, not the model's: a rotation of the root is then exactly a rotation whatever scale a model
  or an ancestor carries (non-uniform scale tested), and the bones are the lengths they are drawn at.
- *The upper bone's turn carries the bend plane* (so the elbow only hinges and the twist is taken at the shoulder, continuously with the
  pole), mixed with the shortest turn by how bent the input pose is (a straight arm has no plane). Rejected, in this order: shortest turn
  of each bone (the forearm lands out of plane and the elbow twists: 10 degrees a step in the sweeps); choosing the plane's normal by the
  shorter twist (a hinge reversed, an elbow bending backwards, whenever a pole is more than 90 degrees from the animated bend).
- *The side of the bend falls back smoothly*: a pole on the target's axis gives way to the pose's own bend, and a straight chain with no
  pole to "toward -Y", each by a small pull that acts only near the degenerate case (a switch made 42 jumps in the straight-chain sweep).
- *Stretch is measured against the skeleton's rest bone lengths*, so a second solve of the same chain (or two constraints on it) never
  stretches twice: idempotent. Found by the idempotence test (the first version stretched again each pass).
- *Solved in `collect()`*, not `animate()`: the entity's WorldTransform3D of the frame is known only after `propagate_transforms()`. The
  smoothing state advances by the `dt` of the animate() steps since the last solve, once; a second solve in the frame (two views, an early
  `apply_ik()`) changes nothing.
- *Look-at limits are an ellipse in the deflection's azimuth, eased to forward over `release_degrees` past the limit*, so a target going
  behind the head does not flip it from one limit to the other (worst step of the aim 1.6 degrees for a target moving 0.5).
- *Constraint order is the list order and it matters*: a constraint moves what hangs below its joints. In `skinned_ik` the first
  version listed the arm before the foot placer and the look: the hand missed its target by 11 cm (the pelvis lowered, the spine turned
  after it). Put feet first, then the look, then the arms. The docs say so.
- *A target that goes away lets go*: an entity that is destroyed counts as no target, the goal weight falls to 0 and the fade runs from
  the last place the target was. Without a fade it is at once.

**Results.**

| Check | Result |
|---|---|
| `ninja -C build -j8`, `ninja -C build-no3d -j8` | build; the 3D-off build is untouched (the new files are under the option) |
| `ctest` in `build`, `KIN_REQUIRE_GPU_TESTS=1` | 69 of 69 |
| `ctest` in `build-no3d` | 65 of 65 |
| `kin_render3d_tests` | passes on NVIDIA RTX 4080 (VK_ICD_FILENAMES unset, 91 s) and on lavapipe (151 s) |
| `kin_skeleton_tests` | passes (16 new tests and the bench) |
| Off-path pictures: default and `--cascades 4 --ao --contact-shadows --taa` over crowd, shadows, sponza, helmet, motion, spheres, cornell; kiln_near with `--lod --lod-clusters` | 37 of 37 byte-identical to `p1a_base_def` / `p1a_base_all` / `p1a_base_kiln` |
| Skinned scenes (skinned_bend, skinned_ecs, skinned_blend, skinned_cesium, skinned_fox), default and all-on, before the edit vs after | 20 of 20 byte-identical; a second render of the same build matched itself |
| `skinned_ik`, `skinned_ik_look` rendered twice | identical |

**What the tests assert** (CPU unless said):

| Test | What |
|---|---|
| two-bone reach | 800 random reachable targets (arm and leg, random poles): end within 2e-4 of the target, bone lengths kept to 1e-5, only the chain's rotations changed (translations and scales bit equal), the kept matrices equal a full recompute |
| out of reach | the end on the line to the target at full extension, the elbow on the line (needed Heron's factors: `l1^2 - a^2` lost it to rounding, 3e-4 off); soft stretch monotonic and continuous in the distance, never above the limit; minimum bend 10 degrees gives exactly 10 |
| pole | the elbow on the pole's side for +-Y and +-Z; opposite poles mirror across the axis; a point equals the direction to it |
| weight | 0: pose and matrices equal bit for bit; 0.5: each rotation half way; 1e-4: hardly moved |
| idempotence | solving a solved pose again: matrices within 2e-5, with and without a pole, in and out of reach, with stretch and minimum bend |
| continuity | sweeps of 1,999 to 4,000 steps; table below |
| scaled and turned | a model with a non-uniform scale and a turn at its root and a scaled chest above the shoulder: 200 targets, hand within 1e-3 of the target, bones kept to 2e-5 in the parent's space |
| orientation | the end joint within 0.05 degrees of the target orientation; half the weight half way; without it the local rotation is untouched |
| look-at | arrives; shares 20/30/50%; limits hold exactly (5/15/25 degrees, aim 45 off); asymmetric pitch; weight 0 bit for bit, 0.5 half; target circling the head: worst step of the aim 1.6 degrees for 0.5 of target, at the back 0 from forward; smoothing monotonic and e^-1 left after one time constant; the speed cap holds |
| foot placement | flat ground: nothing moves; a step down under one foot: pelvis -0.2, that foot on it, the other bent (knee forward), bones kept; a step up: pelvis stays, with a raise allowed it takes the lowest rise; slopes of 20 and 50 degrees: the foot's up follows the normal to 0.1 degrees, 30 degrees at most; easing: snapped on the first call, e^-t/tau afterwards; a foot falling away eases down and rising is at once; no ground: left as animated |
| FABRIK | 100 targets reached to 1e-3, lengths kept to 1e-4, root fixed; out of reach: straight at it; free root follows; weight 0 untouched |
| order, bad input | the later constraint on a chain wins; a pelvis lowered then an arm: the shoulder moved, the hand arrives; reversed chains, equal joints, indices out of range, a zero-length bone: nothing happens (`solved` false) |
| ECS | a hand reaches a world target for an entity moved, turned and scaled (1.7); the elbow on the pole's side; an entity target followed; a destroyed one is safe and fades; fade-in at 0.3 s partway, full at 0.5 s; `set_weight(0)` lets go; order; no constraints: matrices bit equal to the player's; unresolved joint names do nothing; two runs equal |
| GPU: hand at target | the red marker's centre of mass within 0.04 to 0.2 pixels of the target's projection (5 cases: three positions, an entity moved and turned, scaled 1.3), and not there at weight 0 |
| GPU: not culled | a person below the view (culled at rest) with the arm raised into view is drawn, the marker at the target |
| GPU: TAA | a hand moved by IK under TAA for 16 frames: the pictures equal those of the same poses played from a baked clip, 0 of 19,200 pixels over 4 levels (the marker moves 10.8 px) |
| GPU: shadow cache | cached and still: hit; only the IK target moves: miss, picture equal to the uncached one, arms again |
| GPU: workers | 40 and 2,200 IK characters: two runs and the job system give the same picture |

Continuity sweeps (worst step of either rotation, or of the elbow's direction, between consecutive targets; the targets move 0.0004 to 0.001 of the reach a step):

| Sweep | Worst step | Note |
|---|---|---|
| through the root, equal bones | 0.16 degrees | and one 180-degree twist at the fold's exact centre, the joints in the same places (see limits) |
| through the root, unequal bones (5 cm hole) | 8.1 degrees, elbow/hand 0.07 a step | the chain turns end over end within the hole, steeply, without a jump |
| along the axis, through full extension | 3.9 degrees | the bend's angle goes as the square root of the distance short of full reach; a minimum bend flattens it |
| straight chain, no pole | 0.86 degrees | |
| round the pole (cones of 40, 8, 2 degrees) | 0.10 to 0.16 | |
| through the pole's axis (three directions) | 0.19, 0.39, 3.2 degrees | the elbow swings to the other side over a few degrees of the target's travel |

**Mutation checks** (each: break one thing, see a test fail, revert; the sources were compared with copies afterwards). 20 mutations, all caught:

| Break | Caught by |
|---|---|
| pole side flipped | `test_ik_two_bone_pole` |
| stretch leaves the forearm at its own length | `test_ik_two_bone_idempotent` (the out-of-reach test passes this break, also with the check on the hand's place that I added there: the pose's translations are stretched right whatever the solve assumed; idempotence is what sees it) |
| weight ignored | `test_ik_two_bone_weight` |
| stretch against the current lengths | `test_ik_two_bone_idempotent` |
| model space instead of the root's parent's | `test_ik_two_bone_scaled_and_turned` |
| look-at limits ignored / a hard clamp with no release | `test_ik_look_at` (both) |
| look-at weight ignored | `test_ik_look_at` |
| look-at smoothing ignores `dt` | `test_ik_look_at` (only after a time-constant assertion was added: it first SURVIVED) |
| pelvis takes the highest foot | `test_ik_foot_placement` |
| foot tilt not clamped | `test_ik_foot_placement` |
| foot ground offsets not eased | `test_ik_foot_placement` |
| FABRIK without the forward pass | `test_ik_fabrik` |
| two-bone without the mid joint's turn | `test_ik_two_bone_reach` (and the GPU tests) |
| weights do not fade | `test_ecs_ik_entity_target_and_fades` |
| destroyed target keeps its weight | `test_ecs_ik_entity_target_and_fades` |
| constraints in reverse order | `test_ecs_ik_order_and_off_path` |
| entity matrix ignored | `test_ecs_ik_reaches_world_target` (and the GPU hand test) |
| `collect()` ignores the IK (matrices of the clips) | the ECS test, and all four GPU tests: hand at target, not culled, TAA vs clip, shadow cache (workers passes: it compares one build with itself) |
| matrices one frame stale | the same |

**Cost** (one core, Release; the machine's other load was light).

| Measure | Value |
|---|---|
| An 18-joint person: world matrices, a two-bone arm, a three-joint look-at, a foot placer for two legs, the matrices (`bench_ik`, 20,000 runs) | 5.6 to 5.7 us |
| 500 such persons in the ECS with three constraints each, `animate` + `propagate_transforms` + `collect` | 3.27 to 3.32 ms a frame (6.5 to 6.6 us each) against 0.66 ms for the same 500 with no constraints |
| The part spent in constraints, then, per person | about 5.4 us: single-digit microseconds, as asked (it includes a heap-allocated `std::function` per foot placer per frame and the IK pose copy) |
| GPU | nothing changes (the matrices are the same array of the same size); not timed separately |

**What I looked at** (`kin_reference` `skinned_ik` and `skinned_ik_look`, Sponza Walk `--preset ik`, `ik-reach`, `ik-all`; the test pictures with `KIN_TEST_DUMP`).
- The hand reaching the target: on it at every position tried (the marker ball sits under the hand's ball). The elbow goes to the pole's side
  (back for the arm to the front-right, down for the arm across the body). No collapse and no visible candy-wrapper at the elbow of this
  procedural person; its limbs are tubes with a ramp of weights over the last 40% of each bone, which is gentle.
- Over-reach: the right arm of the second person straight at a target 2.2 m away, in line with it; stretch off.
- A foot on a step with the pelvis lowered: the person stands at the edge of a 0.2 m step with the left foot on it and the right on the
  floor: the pelvis is 0.2 lower than the step's level (the right foot's drop), the left leg bent with the knee forward and the foot flat on
  the step. The foot sits near the step's back edge (the animated foot is there), nothing slides in a still picture.
- The head: 40 and 80 degrees turn it to face the target (the nose shows it), 125 degrees stops short of the target at the limits (the
  head is turned to the side), straight behind eases back to facing front, 60 degrees up tilts the head back to the limits. No flips.
- Two things on the person's mesh that are the fixture's, not the solver's: the open ends of the tubes at the shoulders and the crease
  where the clavicle tubes cross the chest.

**Honest limits (as first written, at 8eb72bb; the follow-up below says which are fixed and what remains).**
1. *A target through the exact centre of a folded chain with equal bones* turns the upper arm 180 degrees in one step about its own axis with
   every joint in the same place (the fold's plane flips). No continuous rule has no such place; the pose is right, the skin twists. Real
   bones are unequal (a hole of `|l1 - l2|` round the root, through which the chain turns steeply but continuously), and a hand targeted at its
   own shoulder is a bug in the game.
2. *A pole exactly behind the target* (opposite the pose's own bend, near the axis) is the one place the elbow's side must flip: it does so
   over a few degrees of the target's travel (3.2 degrees a step at 0.0003 of the reach a step), not at once.
3. *A straight chain with no pole* bends toward -Y: a knee, or an arm that is not to hang, needs a pole. The ECS foot placer's knees
   keep the clips' bend with `Pole::None`, so give it a model direction when the legs are straight.
4. *A non-uniform scale on the root joint itself or between the mid and end joints* is approximate (the mid and end rotations are applied
   through the rotation of the parent's matrix); one above the chain is exact.
5. *Foot placement is a plan, not physics*: no foot locking (a planted foot slides if the character's root does between frames: the ankle
   target comes from the animated ankle's place), no toe, no slope-aware pelvis tilt, no leg-length limit besides `max_stretch` and
   `max_pelvis_drop`. A foot in the air keeps its clearance above the ground it is over, so a swing across a ledge lifts and drops with it
   (eased down). The ground query is called twice a frame per foot planner (once per leg).
6. *Joint limits*: look-at has them; the two-bone solver has none (an elbow can be driven backwards by a pole behind it).
7. *Look-at smoothing in the ECS is in world space and starts from the head's own aim* when its weight comes up from 0.
8. *Not done or not tested*: D3D12 and Metal (nothing here is GPU); Sponza Walk's panel click path (no display): the section ran only
   through `--preset` and `--set`; CesiumMan's IK (its joint names are not a rig's); a FABRIK use with an ECS constraint kind (the maths
   is in `ik.hpp`; the ECS hook has two-bone, look-at and the foot placer); prefab / component-schema support (the 3D components have none);
   kin/anim events or state-machine parameters driving weights (the game sets them each frame: `set_weight`).

#### P1b follow-up — the known limits, fixed (2026-10-11, `exp/3d-rendering`, working tree on 8eb72bb; not committed)

All CPU maths, the ECS hook, the harness and the viewer: no shader or renderer change. Every new option is off by default and every
picture and test of before is byte for byte (the identity check below).

| # | Limit | What was done | Where |
|---|---|---|---|
| 1 | no joint limits | `TwoBoneOptions::max_bend_degrees` (the mid joint folds to that and no further), `hinge` / `hinge_axis` (the mid joint's own axis: a pure turn about it, the positive way, even from a straight pose or a hyperextended clip) and `max_twist_degrees` (the root's swing-twist twist about the bone, from what the clips did). A pole that asks a knee backward carries the hinge round by twisting the hip; the limit holds the twist, and eases to nothing at a half turn so the knee comes back to the clips' side without a flip. The twist limit turns the elbow's side about the target's axis (the end stays on target) by a bracketed bisection on the twist as a function of the side. | `ik.hpp`, `ik.cpp` `two_bone_pass` |
| 2 | FABRIK not an ECS kind | `IKKind::Fabrik` (joints root to tip, target, pole, weight and fade like the others, `IKConstraint::fabrik`), and a pole in `FabrikOptions`: the solved chain is turned about the root-to-tip line (no end, no length moves) so its bulge faces the pole, fading out for a straight chain or a pole on the line. | `render3d.hpp`/`.cpp` (ECS), `ik.cpp` |
| 3 | foot placement | the ground query once per leg per frame: it was once per leg per solve, and a second solve in a frame (`apply_ik()` then `collect()`, two views) asked again; the ECS now keeps the answers for the frame (an epoch advanced by `animate()` and `propagate_transforms()`, matched by segment). Foot locking (`lock_feet`: planted when the animated sole is within `lock_height`, held in the WORLD while the body moves, released at `lock_release_height` / `lock_release_distance`, eased back over `lock_replant_seconds`). Toes (`FootLeg::toe`: a second query under the animated toe, the toe turns to its normal, clamped to the foot's slope and `toe_max_tilt_degrees`, not in the air). | `ik.cpp` `plan_foot_placement`, `apply_foot_placement`; ECS `solve_ik` |
| 4 | non-uniform scale on the root joint or between mid and end | exact now (hand within 4e-7 of 360 reachable targets, three scalings). A turn of the mid joint changes the forearm's drawn length when its parent's matrix is a non-uniform scale, so the rigid triangle is wrong however it is rotated. The mid joint is first turned about its hinge (the pose's plane, or the hinge) until the end is at the distance the target asks for (a scalar equation in the turn: 72 samples, then bisection), the rigid solve then sees the true lengths, and up to 8 passes from its own result (same side, same end orientation) finish to rounding and to the end orientation. A weight below 1 blends the solve with the pose. | `ik.cpp` `solve_two_bone` |
| 5 | straight chain, no pole, goes to -Y | the rest pose's bend (the rest chain's bend direction, turned by the root's change from rest) when it has one (more than 1 degree), else the hinge's side, else -Y of the root's parent as before. Rigs with a straight rest (the biped fixtures) are unchanged. | `ik.cpp` |
| 6 | the two singular cases | investigated: see "Singularities" below. A stateful treatment exists for the pole case (`bend_swing_degrees_per_second` with a `TwoBoneState`), implemented; the fold's centre is topologically inherent. | `ik.cpp`, ECS `bend_state` |
| 7 | not tested | CesiumMan: arms and legs reach targets made by turning their joints (400 targets, 4 limbs, hand within 3e-7, bones 8e-7 relative); Sponza Walk's panel logic factored into `examples/sponza_walk/ik_rig.hpp` (`person_rig`, `person_constraints`, `drive_person`, `person_ik(Settings)`) and driven by a unit test. | `tests/skeleton_tests.cpp` |

**Singularities (item 6).** *A pole exactly behind the target* (the side across the target's axis is undefined where the pole is on it): for
any function of the target direction alone, the elbow's side is a tangent vector across the sphere of directions, and a continuous
nowhere-zero tangent field on a sphere does not exist (the hairy-ball theorem), so some target direction has an undefined side, and near
it the side turns over a few degrees of the target's travel; moving the zero (as the pull toward the pose's own bend does) cannot remove it.
Stateless it stays: 8.3 degrees a step in a 120-frame sweep through the axis (0.005 of the reach a frame), 24.4 degrees in a 40-frame one.
With memory it is a different problem, and it has a solution: the side may swing toward the one wanted at a bounded speed, whatever the
target does (the end stays on the target the whole time: only the elbow lags). `TwoBoneState` + `bend_swing_degrees_per_second`: the same
sweeps give 6.0 and 6.1 degrees a step at 360 degrees a second (6 a frame), in the ECS 6.1 against 45.0 for the stateless 40-frame sweep.
It is off by default (a state is something a caller must keep), a weight of 0 forgets it, and a second solve in a frame (dt 0) holds the side.
*A target through the exact centre of a folded chain of equal bones*: every joint's place is continuous through it, but the upper bone's twist
about its own axis is not: the plane of the fold is the thing that flips, and no choice of it can be continuous, because the target comes
out of the centre on the opposite side of it and the elbow, which has not moved, would have to be on the other
side of the shoulder (a jump in position instead of one in twist). The worst step is
therefore path-dependent: a pass exactly through it is one half turn (136 degrees at the nearest of 2,000 samples), 2 mm off it 21.8
degrees a step, 20 mm off 2.3 degrees. A real arm has unequal bones, which makes a hole of `|l1 - l2|` where the chain turns steeply but
continuously (8.1 degrees a step, as before). Documented, not fixable.

**Results.**

| Check | Result |
|---|---|
| `ninja -C build -j8`, `ninja -C build-no3d -j8` | build; no warnings from the new code |
| `ctest` in `build`, `KIN_REQUIRE_GPU_TESTS=1` | 69 of 69 (twice: before and after the last tidy) |
| `ctest` in `build-no3d` | 65 of 65 |
| `kin_render3d_tests` | passes on NVIDIA RTX 4080 (VK_ICD_FILENAMES unset, 78 s) and on lavapipe (118 s); the new GPU test's numbers are the same on both |
| Identity: default and `--cascades 4 --ao --contact-shadows --taa`, 120 frames, over crowd, shadows, sponza, helmet, motion, spheres, cornell; kiln_near with `--lod --lod-clusters` | 37 of 37 byte-identical to `p1a_base_def` / `p1a_base_all` / `p1a_base_kiln` |
| Skinned scenes (skinned_bend, skinned_ecs, skinned_blend, skinned_cesium, skinned_fox, skinned_ik, skinned_ik_look), default and all-on, against pictures rendered from the 8eb72bb binary first | 32 of 32 byte-identical |
| `skinned_ik_limits` rendered twice (and all-on once) | identical |
| `kin_skeleton_tests` | passes; new tests: `test_ik_joint_limits`, `test_ik_rest_bend_fallback`, `test_ik_two_bone_nonuniform_scale`, `test_ik_bend_swing_and_singularities`, `test_ik_fabrik_pole`, `test_ik_foot_locking_and_toes`, `test_ecs_ik_fabrik_locking_and_limits`, `test_sponza_walk_ik_panel`, `test_ik_cesium_man` |
| hinge | 300 of 300 random knees are pure hinges (about X, the positive way) with it, 0 of 300 without |
| twist limit 30 / 90 / 180, pole all the way round the leg in 0.5 degree steps | worst step of the knee 0.49 / 0.73 / 0.56 degrees; twist held to 30.05 |
| FABRIK pole round the axis, 0.5 degree steps | worst step of the bulge 0.67 degrees; the bulge faces the pole to 0.99 |
| foot lock (CPU, body 1 cm a frame) | held to 0.2 mm for 14 frames, then released at 15 cm and eased back: worst step 3.9 cm (the smoothstep's 2.5 and the body's 1); planted again 25 frames in. In the ECS: 0.07 mm drift over 11 cm of walking against 0.11 m unlocked |
| toes | 20 / 50 / 20-with-clamp-8 degree slopes give 20 / 30 (the foot's clamp) / 8 degrees of toe tilt to 0.2; none for a foot in the air |
| ground query counts | 2 a frame for two legs through `apply_ik()` + `collect()` + a second `collect()`, 4 with toes |
| scaled chains (root (1.5, 0.6, 2), mid (0.7, 1.4, 1.1), both) | hand within 4.0e-7 / 2.5e-7 / 2.9e-7 of 120 reachable targets each; idempotent to 5e-5; end orientation 0 degrees from the goal |
| rest-bend fallback, straight pose, no pole, sweep | worst step 0.69 degrees |
| CesiumMan (arms, legs; 400 targets) | hand within 2.7e-7; bones kept to 8e-7 relative |
| panel (Sponza Walk) | hand at the panel's target for each of the 4 poles (the elbow on the pole's side); weight 0 and fades; stretch to 1.2; look on/off; the step lifts the left ankle to 0.25; tail to its target (2 mm); locked foot drifts 0.2 mm vs 0.11 m over 11 cm of drift |
| GPU (`test_gpu_ik_limits_fabrik_locking_toes`) | FABRIK head marker 0.26 / 0.38 px from the target (two poles; the pictures differ); knee marker 0.5 / 0.04 px from the CPU's knee, the knee behind the hip-ankle line by 0.29 m without limits and in front by 0.29 with them; toe-tip marker 0.11 / 0.40 px from the CPU's, tilted 25 / 0 degrees; ankle marker moves 0 px locked, 10.3 px unlocked over 28 cm of walking |

**Mutation checks** (a new break each, a test fails, restored; the sources compared with copies afterwards). 18 mutations against the CPU
tests: 16 caught at first, 2 survived and were caught after a new case was added (marked *); 4 of them also against the GPU test (3 caught;
the toe clamp is under the GPU slope):

| Break | Caught by |
|---|---|
| hinge ignored | `test_ik_joint_limits` (pure hinge); GPU: knee in front |
| twist clamp off / limit not eased at a half turn | `test_ik_joint_limits` (twist held; the sweep's step) |
| max bend ignored | `test_ik_joint_limits` |
| rest-bend fallback ignored | `test_ik_rest_bend_fallback` |
| mid-joint pre-step off / refinement passes off | `test_ik_two_bone_nonuniform_scale` (idempotence; the end orientation) |
| bend swing unbounded | `test_ik_bend_swing_and_singularities` |
| FABRIK pole ignored (maths / dropped by the ECS) | `test_ik_fabrik_pole`; `test_ecs_ik_fabrik_locking_and_limits`; GPU: the two pictures equal |
| lock not in the world (model matrix ignored) / never released / no easing | the foot lock test (held, still, the step); the ECS lock test; GPU: marker moves |
| toe clamp off | `test_ik_foot_locking_and_toes` (not the GPU test, whose slope is under the clamp) |
| toe follows the ground in the air * | survived until the airborne-foot case was added |
| ground answers not kept for the frame | `test_ecs_ik_fabrik_locking_and_limits` (counts) |
| ECS drops the bend state * | survived until the ECS bend swing case was added |

**What I looked at.** `kin_reference` scene `skinned_ik_limits` (four people, `skinned_biped_tail.glb`, `ik_ramp.glb`): the two people with the left foot
on a 0.2 m step and the knees' pole pointing back: the left one's knee is behind and its hip turned half way round, the right one's
(hinge, 25 degrees of twist, fold to 150) is in front with the foot on the step; a tail of five joints reaching a red marker with its
bulge up (the tip on the marker); and the feet at the foot of a 25 degree ramp, the toes tilted up onto it, the foot flat. Viewer:
`--preset ik-tail`, `ik-walk`, `ik-all` run headless without error (the tail hidden behind the person from the front camera: judged on
the scene's `tail` camera, not the viewer's).

**What remains.**
1. *The fold's centre with equal bones*: a half turn of the upper bone in one step for a target through it exactly (inherent, above); a
   hand targeted at its own shoulder is a bug in the game.
2. *A pole exactly behind the target stays a fast turn without a state*; with a `TwoBoneState` it is bounded, but the elbow then lags
   the pole by up to a few frames (it is the elbow's side only; the hand is always on target).
3. *The twist limit needs a plane*: a hinge, or a pose that is bent; with a nearly straight pose and no hinge the limit fades with how bent
   it is, so a straight leg with no hinge can still be turned fully by a pole.
4. *Foot locking is a plan too*: it holds the ankle target, not the sole (a locked foot that the leg cannot reach is as near as `max_stretch`
   lets it get, and floats off the ground), the thresholds are in model units, and it needs the animation to put the foot down (a clip that
   never brings the sole within `lock_height` of `ground_level` never plants). A planted foot is not re-planted while it is still easing.
5. *Toes*: the toe follows the normal at the ball, not the height (a ball over a ledge is not lifted); one tilt amount (the foot's
   `tilt_blend_height`) for ankle and toe; the toe's query is one more call.
6. *Scaled chains* are exact for reachable targets; for a target outside what a scaled forearm can reach (the reach is an ellipsoid's, not a
   sphere's) the end stands as near as the mid joint's turn allows, `reached` false. `min_bend_degrees` and `max_bend_degrees` are laws of
   cosines in the rigid triangle, so under a non-uniform scale on the root they are measured in the parent's space, not the drawn one.
7. *Look-at smoothing in the ECS is in world space and starts from the head's own aim* (as before).
8. *Not done or not tested*: D3D12 and Metal (nothing here is GPU); the Sponza Walk panel's widgets and click path, the marker, step and
   tail-target meshes and the keyboard (no display: the state-to-constraint mapping is tested, and the presets run headless); the new `toe` /
   `lock` / `ik: fabrik` keys of `kin_reference` have scenes but no unit test of their parser; prefab / component-schema support;
   kin/anim events driving weights. P2's physics raycast is the one ground query still to plug in.

**What P2 needs from this.**
- *The ground query* is a physics raycast: `GroundQuery` is `(Vec3f from, Vec3f to) -> optional<GroundHit{position, normal}>` in world
  space, from the foot's place down by `ray_up + ray_down` metres; P2's `raycast(from, to)` returns that. One call per leg per frame (one more
  for each toe joint), however many times the constraints are solved in the frame; give it a layer mask for the level (not the character's own body).
- *Grab*: a hold is an entity (or a point) with `WorldTransform3D`: `ik("right_hand").set_target(hold, grip_offset).set_target_rotation(
  surface_rotation).set_pole_direction(away_from_wall)`; `set_weight(1)` with a fade of 0.1 to 0.2 s to reach and `set_weight(0)` to
  release. A hold that is destroyed is let go of by itself. To drive the character's body toward the hold (hanging), move the entity (the physics
  body) and the feet planner's pelvis follows.
- *Reading where a hand is* before drawing (grip checks, stamina, rope attachment): `state.apply_ik()` after `propagate_transforms()`,
  then `animator.ik_world[hand_joint]` in the model's space (times the entity's `WorldTransform3D`).
- *Order of the list* for a climber: feet, look, arms (and the spine's lean as a clip layer before them).

### P2 — 3D physics with Jolt, and a character

Jolt behind `kin/physics3d` (the Box2D module stays as it is): bodies (static, kinematic,
dynamic), shapes (box, sphere, capsule, convex hull, static mesh, height field), raycasts,
shape casts and overlaps, contact events, constraints (fixed, distance, hinge, cone),
stepped by the engine's fixed step and built for cross-platform determinism
(`JPH_CROSS_PLATFORM_DETERMINISTIC`, so a netcode step can rely on it), debug drawing as
wireframe lines in the 3D view, ECS components (`RigidBody3D`, `Collider3D`) and a
system that writes `Transform3D`. On it: a capsule **character controller** (slopes,
steps, moving platforms, gravity direction as a parameter so a planet can come later),
the **grab and climb** mechanic for the slice (raycast for a hold, attach a hand with a
constraint or a kinematic move, stamina drain; the IK of P1b places the hand), a
**ragdoll** built from a skeleton (capsules per bone from the bind pose) blended with a
clip, and **ropes** (chains of capsules with constraints). Checks: determinism (the engine's
`--check-determinism`: three lockstep runs agree, with physics on), unit tests per feature,
a scripted-input run of the climbing test level, the physics step's cost with 500 bodies.
Risk to watch: Jolt is large to compile; this machine has 14 GB of RAM and a nearly full
disk (see HANDOFF.md): build with `-j8`, check the disk first.

Done so far: P2a (the core), P2b (queries, events, constraints, debug draw, ECS), P2c (the character controller) and P2d (grab and
climb), each with its results below; the ragdoll and the ropes remain.

#### P2a — Jolt core — results

Done 2026-10-11 on `exp/3d-rendering` (head 4d3ae05 plus the working tree). The rest of P2 (queries, events, debug draw, ECS
components, character controller, grab and climb, ragdoll, ropes) is not started; the API is shaped for them (see the end).

**What was built.**

| Piece | Where | Notes |
|---|---|---|
| Dependency | `cmake/dependencies.cmake`, `engine/CMakeLists.txt` | Jolt v5.5.0 by FetchContent, only with `KIN_EXPERIMENTAL_3D` (the option is now declared before `include(dependencies)`); `FETCHCONTENT_SOURCE_DIR_JOLTPHYSICS` reuses a clone (`~/.cache/kin/deps/JoltPhysics` here); licence in the package list |
| Build settings | same | static, `CROSS_PLATFORM_DETERMINISTIC` on, single precision, no samples / tests / viewer / debug renderer / profiler / object stream / asserts / install / debug symbols / LTO / `-Werror` |
| Shapes | `kin/physics3d/shape3d.hpp` | box, sphere, capsule, cylinder, convex hull, triangle mesh, height field, compound; shareable handles; volume, centre of mass, bounds; invalid input gives an invalid handle |
| World | `kin/physics3d/world3d.hpp` | `World3D`: gravity as a vector, `step(dt)` with `collision_steps`, bodies by descriptor, the setters, forces and impulses, getters, activation, 32 layers with a pair table and a 32-bit mask type for P2b |
| Determinism | same | `state_hash()` (FNV-1a 64 over fixed-width little-endian fields, bit-exact floats, creation-number order), `save_state` / `restore_state` (Jolt's StateRecorder: cheap, done) |
| Jobs | `engine/src/physics3d/world3d.cpp` | a Jolt `JobSystemWithBarrier` over `kin::JobSystem` (`World3DConfig::jobs`, default `default_job_system()`), or all on the calling thread |
| Tests, bench | `tests/physics3d_tests.cpp` (ctest `kin-physics3d`), `tests/physics3d_bench.cpp` (`kin_physics3d_bench`) | 16 test functions, 152 check expressions, 1.7 s |
| Docs | `docs/physics_3d.md`, changelog, third-party notices, README | |

**Decisions where the plan was open.**
1. *Jolt is compiled into `kin_engine` through an object library of its own* (`kin_physics3d_objects`, two glue sources), not as a PRIVATE
   dependency of the engine: Jolt's PUBLIC compile options (`-mavx2 -mbmi -mpopcnt -mlzcnt -mf16c`) would otherwise reach every engine source
   and could change what the renderer's CPU code compiles to. The glue gets `-fno-rtti` (Jolt's own setting: kin classes derive from Jolt's)
   and `-ffp-contract=off`. The engine links `$<LINK_ONLY:Jolt>`. The identity check below confirms nothing else moved.
2. *Instruction sets*: Jolt's defaults (SSE4.1/4.2, AVX, AVX2, LZCNT, TZCNT, F16C; FMA is off by Jolt itself under cross-platform
   determinism). Results are the same bits with or without them by Jolt's design; a machine without AVX2 would need those options off
   (`USE_AVX2` etc. in the dependency block), not tested here.
3. *Own handles*: `Body3D {index, generation}` over a slot table with a last-in-first-out free list, not Jolt's `BodyID` (whose reuse and
   sequence numbers are Jolt's business); the slot index goes in the hash, which is why the hash order is stable.
4. *Layers*: 32 layers (layer 0 = static, its own broad phase tree; 1 dynamic, 2 kinematic, 3 character, 4 sensor, 5 to 31 free), a
   symmetric 32 x 32 table (`set_layers_collide`), `u32` masks (`layer_bit`, `collision_mask`) for queries; the object layer is the layer
   number, so Jolt's `ObjectLayer` stays 16 bits.
5. *Mass*: a body's `mass` (kg) if above zero, else `density * shape.volume()`; inertia always from the shape, scaled to the mass.
6. *Damping defaults 0* (as the Box2D module; Jolt's own default is 0.05).
7. *`destroy_body` wakes what overlaps the body's box grown by 10 cm*, in body-number order: Jolt does not, and a sleeping tower whose
   bottom box is removed stayed in the air (found by the test "destroy while stacked").
8. *One `step(dt)`* of `collision_steps` collision steps (default 1); the caller owns the fixed step. Not thread safe: one thread at a time.
9. *Temp memory*: 16 MB block with a heap fallback (Jolt's own allocator aborts when it runs out).

**Numbers.**

*Build cost* (this machine: 32 hardware threads, GCC 15, Release, `-j8`):

| What | Cost |
|---|---|
| Jolt, 128 translation units, from nothing | 17.6 s wall, 85 s of CPU |
| `libJolt.a` | 3.8 MB (no debug symbols) |
| Jolt's objects in the build directory | 98 MB |
| The two glue sources | a few seconds each |
| `kin_engine` library | 22.8 MB (all of it, with the glue) |
| Test and bench executables | 2.1 MB each |
| The worktree's `build` directory | 803 MB before, 911 MB after the first full build with Jolt (includes the new test, bench and rebuilt objects) |
| Peak RSS while compiling | not measured exactly; the machine's available memory never fell under 4 GB at `-j8` |
| Source clone | 52 MB (`~/.cache/kin/deps/JoltPhysics`, on the home drive) |
| `/mnt/windows` free before and after | 1.5 GB and 1.4 GB |

*Step cost*, `kin_physics3d_bench`, 60 Hz steps, boxes of about 0.8 x 0.6 x 0.7 m in a walled yard (the machine had other load: the
numbers are indicative). Milliseconds a step, mean over the first 120 steps (falling, landing) and over steps 120 to 360 (settling):

| Scene | Workers | Steps 0-120 mean (worst) | Steps 120-360 mean (worst) | Awake at 360 |
|---|---|---|---|---|
| pile 500 | none (calling thread) | 4.13 (5.33) | 1.63 (4.02) | 0 |
| pile 500 | 1 | 3.06 (3.91) | 1.51 (4.14) | 0 |
| pile 500 | 4 | 2.11 (2.71) | 1.04 (2.64) | 0 |
| pile 500 | 8 | 2.05 (2.51) | 1.11 (2.69) | 0 |
| pile 2000 | none | 10.96 (26.6) | 20.46 (25.5) | 1985 |
| pile 2000 | 1 | 7.31 (17.1) | 15.03 (17.1) | 1985 |
| pile 2000 | 4 | 4.26 (10.6) | 9.33 (11.0) | 1985 |
| pile 2000 | 8 | 3.64 (8.42) | 8.62 (10.5) | 1985 |
| stacks 500 (columns of 10) | none | 1.77 (2.24) | 1.24 (1.64) | 280 |
| stacks 500 | 4 | 0.90 (1.31) | 0.61 (1.22) | 280 |
| stacks 2000 | none | 7.35 (9.27) | 5.45 (6.41) | 1190 |
| stacks 2000 | 1 | 5.05 (8.31) | 3.78 (4.50) | 1190 |
| stacks 2000 | 4 | 2.87 (4.03) | 2.12 (2.67) | 1190 |
| stacks 2000 | 8 | 2.57 (3.62) | 1.97 (2.70) | 1190 |

So 500 bodies cost 1 to 4 ms (the plan's question: well inside 16.7 ms even with no workers), and 2000 stay under 11 ms with 4 workers.
A 2000 box pile in a narrow yard stays awake and jittery for a long time (1985 of 2000 awake after 6 s): that is a scene property, not
a cost per body (the 500 pile sleeps by step 360). "1 worker" beats "none" because the worker and the stepping thread work together.

**Tests** (`kin-physics3d`, no GPU): rest height and sleep; free fall against the discrete law g dt^2 n(n+1)/2 and a 0.5 s step;
sideways gravity, a gravity change at run time, gravity factor; a sphere rolling down a triangle mesh (stays on the plane, spin = speed /
radius); a restitution bounce to 0.64 of the drop height within 0.15 m, and restitution 0 not bouncing; capsule, cylinder, convex hull
and height field at rest, a slope of a height field; compound volume, centre of mass, bounds, impulse through and off the centre; a
kinematic body pushing a crate; handles, generations, user ids, refused descriptors; mass by density and by value, impulse, force,
torque, damping; layers (defaults, a layer that falls through the floor, two dynamic layers that pass or stack, a sensor); a tower
with its bottom removed; a 500 body pile for 300 steps (none escape); hash sensitivity per field and to last-place bits, to swapped
bodies and reused numbers; determinism: the same pile twice, with no workers, 1 and 4 (identical hash every one of 120 steps),
`collision_steps` 2 repeatable and different from 1; save, run, restore, run again, identical hashes; restore refused after a body was
created.

**Mutation checks** (each: break the source on purpose, build, run, restore; the sources compared with copies afterwards, identical):

| Mutation | Result |
|---|---|
| step uses 2 x dt | caught (12 checks) |
| `set_gravity` negates the vector | caught (the run-time gravity check) |
| hash ignores rotation | caught (hash sensitivity) |
| hash ignores velocities | caught (3 checks) |
| hash drops the body number and generation (order-insensitive) | caught (reused-number check) |
| layer pair table ignored | caught (stacked / pass-through test) |
| user id stored off by one | caught (3 checks) |
| restitution ignored | caught (bounce test) |
| density ignored in mass | caught (5 checks) |
| generation not bumped on destroy | caught (6 checks) |
| `restore_state` does not restore | caught (3 checks) |
| kinematic move with a wrong dt | caught (position check) |
| destroy does not wake neighbours | caught (stacked test) |
| capsule half height halved | caught (rest height) |
| compound child offsets dropped | caught (5 checks) |
| Jolt built without `CROSS_PLATFORM_DETERMINISTIC` | caught (`World3D::cross_platform_deterministic()`; Jolt rebuilt, then restored) |
| `GetMaxConcurrency()` returns 64 instead of workers + 1 | **survived, as it should**: an equivalent mutant (the number of jobs Jolt cuts a step into does not change the bits); it is the evidence for "the result does not depend on the thread count" |

**Checks run.** `ninja -C build -j8` and `ninja -C build-no3d -j8`: no warnings from the new code. `build`: `KIN_REQUIRE_GPU_TESTS=1 ctest
-j4 --timeout 600`: 70 of 70 (69 before and the new one). `build-no3d`: 65 of 65, and its `build.ninja` has no Jolt in it. Identity: `render
def` and `render all` (--cascades 4 --ao --contact-shadows --taa) of crowd, sponza, helmet and cornell at 120 frames against
`reference_out/p1a_base_def` and `p1a_base_all`: all 16 pictures byte for byte equal.

**Limits and not done.**
- No queries, shape casts, overlaps, contact events or sensors reporting (a sensor flag and a layer exist; P2b), no constraints, no debug
  draw (Jolt's debug renderer is compiled out; the draw can come from shape triangle data without it, or the option be turned on), no ECS
  components, no character controller, no ragdoll, no ropes, no soft bodies, no scaled or mutable shapes, no per-contact material
  callbacks.
- Cross-platform determinism is built in but only one platform was run: the claim "the same bits on Windows, macOS and ARM" is Jolt's,
  not tested here. Hash NaN-canonicalisation is in the code but no API path can make a NaN (non-finite input is refused), so it has no test.
- `save_state` / `restore_state` need the same set of bodies (Jolt's rule); a rollback that spawns bodies needs a body list of its own
  on top, which is netcode's step.
- The `World3D` is not thread safe, and the `JobSystem` it was given must outlive it.
- Jolt's default allocator and Factory are registered once and never released (shapes may outlive worlds).
- A height field is stored in 8 bits a sample relative to its block, so its surface is within about a 256th of a block's height range.
- Windows (MSVC) and the other compilers: the CMake follows how the other dependencies are handled (static runtime on, no `-Werror`)
  but was not built here; the glue's `-fno-rtti` is written for GCC and Clang only (MSVC's Jolt build turns RTTI off through `/GR-`
  in Jolt's own directory, so the glue may need the same there).

**What P2b needs from this.** Queries take a `Layer3DMask`; the body slot's user data is already the way back from a Jolt body to a
`Body3D` (Jolt body user data is unused: set it to `(generation << 32) | index` when the contact listener is added); `World3D::Impl` holds
the `PhysicsSystem`, so a `ContactListener` and `BroadPhaseQuery` / `NarrowPhaseQuery` calls are additions inside `world3d.cpp`; the
ground query of P1b is `raycast(from, to, mask)`.

#### P2b — queries, events, constraints, debug draw, ECS — results

Done 2026-10-11 on `exp/3d-rendering` (head b337278 plus the working tree). The character controller, grab and climb, ragdoll and
ropes are the next steps (P2c onward); the constraints they need are here.

**What was built.**

| Piece | Where | Notes |
|---|---|---|
| Queries | `World3D::raycast`, `raycast_all`, `shape_cast`, `overlap`, `point_inside`, `closest_point` | `QueryFilter3D {mask, ignore, sensors}`; values, no callbacks; results sorted (`overlap` and `point_inside` by body number, `raycast_all` by distance then body then sub shape) |
| Contact events | `World3D::events()`, `drain_events()`, `ContactEvent3D` | Begin / Persist (config, off) / End per body pair; collected under a lock in Jolt's callbacks and turned into events after the step in (a, b, kind) order |
| Constraints | `create_constraint(Constraint3DDesc)`, `constraint_info`, `set_constraint_motor`, `drain_constraint_events` | fixed, distance (range, spring), point, hinge (limits, friction, motor), cone (Jolt's swing-twist), slider (limits, friction, motor); breakable by force or torque; in hash and save state |
| Kinematic and body helpers | `teleport`, `follow_transform`, `set_layer`, `set_sensor`, `set_shape`, `bodies` | `user_id` was the per-body user data already |
| Debug draw | `kin/physics3d/debug_draw3d.hpp` (`DebugDraw3D`), `draw_debug_lines` | plain line lists; drawn into a view as one unlit wireframe mesh |
| ECS | `kin/ecs/physics3d.hpp`: `Collider3D`, `RigidBody3D`, `PhysicsBody3D`, `PhysicsContacts3D`, `WorldPhysicsState3D`, `ground_query_from_world`, `collider_from_mesh*` | |
| Harness | `kin_reference` scene `physics_pile`; Sponza Walk "Physics" section, `--preset physics`, key B | |
| Tests, bench | `kin-physics3d` (now 34 test functions, 2.4 s), `kin-physics3d-ecs` (13, CPU only, 0.2 s), `test_gpu_physics_debug_lines` in `kin-render3d`; `kin_physics3d_bench queries` | |

**Decisions where the plan was open.**
1. *Debug draw is CPU lines from kin's own view of the shapes*, not Jolt's debug renderer (kept compiled out: it would be a second
   renderer to feed and a build-time and size cost for something that is a list of segments). Into a 3D view it is a mesh of
   sliver triangles drawn with the existing `wireframe` + `unlit` material: no renderer, shader or scene change, so
   the engine has nothing new to keep byte-identical; the cost is one `create_mesh` a frame while it is on. Two device findings
   shaped it: the (a, b, b) "line" of the wireframe material's comment is dropped by lavapipe (a triangle of no area is culled before
   its edges are drawn; the old test passed through an `||`), and the material's depth bias does not lift coplanar edges above their
   face there. So each line is a thin triangle (a, b, c) and its ends are moved a hair toward the eye along their view rays; the two
   devices then draw the same pixels (106 awake and 426 static ones in the test). Opt-in through
   `WorldPhysicsState3DConfig::debug_draw` (default off: nothing is made or drawn). A CPU projection through the 2D renderer was the
   alternative; this is cheaper to write and depth-tests against the scene.
2. *Sensors are left out of queries unless `QueryFilter3D::sensors`*; they report overlaps as `sensor` events. A trigger volume should
   not stop a ray meant for floors.
3. *Rays start-inside*: a ray that starts inside a convex solid hits it at fraction 0 (Jolt's `mTreatConvexAsSolid`); a triangle mesh
   is hit from both sides (the shape was already documented as double sided) with its front normal.
4. *Events are per body pair, not per sub shape*: a compound or a mesh touching counts once (a per-pair count of sub contacts; Begin when
   it goes from 0, End when it returns to 0). The data (point, normal, closing speed) is the lowest sub shape pair's of that step.
5. *Sleeping is not ending.* Jolt reports a contact as removed when its bodies fall asleep (found by the first test: a box that landed
   once made an End at the step it slept). The world keeps such pairs as dormant (still touching, in the saved state) and reports
   neither End nor a second Begin when they wake and still touch; a woken body that is not seen touching for two steps ends it. A
   destroyed body ends its pairs at the next step.
6. *Closing speed, not impulse.* The solver has not run when a contact is added; the event carries the closing speed along the normal
   from the bodies' velocities (a sound's volume is made from it), which is exact for a first impact.
7. *No callbacks into game code from Jolt's threads*: the listener records into a vector under a mutex; everything else happens after
   `Update` on the calling thread. The mutex was not a cost (see numbers).
8. *Connected bodies do not collide by default* (`collide_connected`), by rejecting the pair in `OnContactValidate` from a pair table
   read-only during the step; that needs the listener installed (it always is; `contact_events` only turns recording off).
9. *Broken constraints are disabled, not removed*, so a restore can mend them (Jolt saves the enabled flag); the world's state hash and
   save state carry which broke. The force is the last collision step's total impulse over its length.
10. *Cone is Jolt's swing-twist constraint* (a swing half angle and twist limits), which is what a shoulder or neck wants; the plain cone
    (swing only) is a special case with twist unlimited.
11. *ECS rules.* A body exists when an entity has `Transform3D` and `Collider3D` (static without `RigidBody3D`); made in ascending
    entity id order, with the entity's world scale applied to the collider's sizes; bodies are world level (a parent does not carry a
    dynamic child; its Transform3D is rewritten as the place from the parent as it is now); a kinematic body follows the entity with
    parents; a dynamic entity moved by game code is taken there. flecs reuses entity ids, so "ascending id" means numeric order of the
    ids as flecs gives them (still a function of the order the world was built in, which is what matters).
12. *Colliders from meshes.* The GPU `Mesh` keeps no CPU triangles (checked: `Mesh` holds a backend, counts and bounds; `Model` keeps
    parts of `Mesh`), so `collider_from_mesh(entity)` is a box about the bounds; hulls and triangle meshes are made from the
    `MeshData` the game still holds (`collider_hull_from_mesh_data`, `collider_triangles_from_mesh_data`) or a prebuilt `Shape3D`.
    Not done: retaining triangles in `Mesh` (a memory cost on every mesh for a feature few use).
13. *Interpolation is an opt-in call* (`interpolate(alpha)`), not a mode: `step()` always leaves Transform3D at the last state.
14. *Scale is not a shape operation*: shapes are built at their size (the ECS builds them at the entity's world scale);
    `set_shape` swaps a body's shape. No scaled or mutable shape is exposed.

**Numbers** (this machine, one thread, a loaded desktop: indicative; `kin_physics3d_bench queries`).

| What | Cost |
|---|---|
| 1000 raycasts into a pile of 2000 bodies (1993 awake), straight down | 1.1 ms (1.1 us each) |
| 1000 `raycast_all` through the same pile (3.6 hits each) | 1.9 ms (1.9 us each) |
| 100 shape casts (sphere and box, 15 m) into it | 0.42 ms (4.2 us each) |
| 100 overlaps (sphere, 2.9 bodies each) | 0.82 ms (8.2 us each) |
| 1000 raycasts into one mesh of 100,352 triangles | 0.97 ms (1.0 us each) |
| 100 sphere casts (20 m) against that mesh | 0.69 ms (6.9 us each) |
| step of a 500 body pile, steps 0-120, events off / begin-end / persist too | 4.152 / 4.181 / 4.414 ms |
| steps 120-360, the same | 1.655 / 1.670 / 1.817 ms |
| events made over 360 steps (begin and end / with persist) | 6,542 / 236,283 |
| ECS: `World3D::step` against `WorldPhysicsState3D::step`, 500 dynamic bodies, steps 0-120 / 120-360 | 4.186 / 1.673 ms against 4.370 / 1.976 ms: overhead 0.18 / 0.30 ms |

So events cost under 1 % (begin and end) and 6 % (persist, a record per touching pair per step); the entity system costs about 0.2 to 0.3
ms a step for 500 bodies (a world matrix up the parents and a transform write for each awake dynamic body, and a compare for the rest),
which is under 2 % of a 60 Hz frame. Queries are an order of magnitude below what a character controller needs (a dozen a frame).
Build: Jolt's debug draw was not enabled; the new sources compile in a few seconds each.

**Tests.**
- `kin-physics3d` (CPU, 2.4 s): rays at known distances and normals against box, sphere (centre and off centre: y = 0.9 and the
  (0.6, 0.8, 0) normal), capsule, cylinder, hull, mesh (from above and below), height field and compound (the child index); a ray from
  inside; layer mask, ignore body, sensors; `raycast_all` sorted nearest first and reversed; shape cast against analytic times (sphere
  on a floor 0.45, box on a wall 0.7, a turned box at its corner, none when short, fraction 0 with penetration when overlapping);
  overlap sorted by body number with mask, ignore and sensors; `point_inside`, `closest_point`; events: exactly one Begin and no End for a
  box that lands and sleeps (400 steps), alternate Begin / End and one more Begin than End for a bouncing ball with closing speeds
  decreasing, sensor enter and exit once each in order, user ids and normals, persist events and draining, off by config, sleeping
  and waking (no new Begin, End once when thrown, Begin on landing), destroying a body ends its pair at the next step and the batch is
  sorted; event hashes identical across two runs and 0, 1 and 4 workers for a 200 body pile (all fields of every event), for
  compound bodies on a mesh floor (many sub shapes a pair; 3 runs on 4 workers), with persist events; constraints: hinge limits
  (+45 / -30 held within 1.5 degrees, the hinge point does not drift), hinge velocity motor (90 degrees a second) and position motor,
  distance rod (2 m within 0.03 over a swing), range (falls to the max), spring (gives under load), point, slider limits both ways
  under force, cone swing limit (25 degrees), a weld carrying a body (relative placing kept), breakable (400 N breaks, 2000 N holds,
  one event with the right handle and user id; mending); connected bodies do not collide / do with `collide_connected`; constraints in
  the hash, in save and restore (a break after the save is undone by the restore, the trajectory repeats, another world takes the
  state, one with a constraint more refuses it), 4 workers equal; teleport, follow_transform (kinematic and dynamic), set_layer
  (falls through a layer that does not collide, only static in layer 0), set_sensor, set_shape (rest height changes, mass kept, mesh
  refused for dynamic).
- `kin-physics3d-ecs` (CPU): bodies in entity order, destroyed with entities (and with `Collider3D`), slots reused in id order;
  scale, offset and compound colliders by raycast (sub shape index); dynamic write back (position, rotation, scale left alone,
  initial velocity, gravity factor); parented bodies (made at the parent's world pose, rewritten from the parent as it is, left behind
  when the parent moves, rest at 0.5); kinematic follows Transform3D (carries a crate by friction; a kinematic child follows its
  parent); game code moving a dynamic or static entity; motion change and `rebuild`; events routed with entity ids and as the
  singleton, a destroyed entity names nobody, `advance()` step counts and the cap; a 60 body scene hashed per step equal run to run and
  on 4 workers; interpolation (alpha 0, 0.5, 1; an interpolated pose is not read as a move); debug lines (counts per colour:
  12 / 12 / 48 ...; sleeping boxes in their colour; a sphere's lines all 0.5 from its centre; a ray and its hit; contacts and
  normals; constraints and a broken one in its colour; the line cap; mesh and height field triangle caps); foot placement over a
  physics floor and a step (below); colliders from mesh bounds and from `MeshData` (hull and triangles), a dynamic mesh refused.
- `test_gpu_physics_debug_lines` (`kin-render3d`, GPU): with the switch off the picture is the one without the physics state, byte
  for byte; on, 106 px of the awake colour and 426 of the static one, 0 without; after the box sleeps, 96 px of the sleeping
  colour and none of the awake one; a pixel of its colour at the projection of the box's top front edge; the ray and its hit show.
  Run on NVIDIA and lavapipe.

**IK on a physics floor** (`test_foot_placement_over_physics`, in `kin-physics3d-ecs`): a biped (the P1b fixture) over a floor
whose top is at y = -0.2 and a step (top y = 0) under its left foot, the ground query `ground_query_from_world`. The ankles settle at
y = 0.05 (left, on the step) and y = -0.15 (right, on the floor) to 2 mm, each 0.05 above the surface the raycast reports; the pelvis
at y = 0.8, the lower foot's drop; the query was called at least twice a leg a frame; the character's own kinematic capsule is
ignored through `ignore` (without it the hit is the capsule's top at y > 1).

**Pictures.** `physics_pile` (`tests/reference/scenes/physics_pile.json`; 60 boxes and spheres in a walled yard, frame 0 150 steps
in, `above` and `side`): two runs byte for byte equal. Looked at: the pile of boxes and red spheres in a stone yard, every body
outlined in green (awake: at 2.5 s the pile has not all settled), contact crosses in red and their normals in orange, the yard's
static boxes in grey; the sphere circles show dotted where the facets of the lit mesh lie outside them (the outline is exact, the
mesh is 24 segments). Sponza Walk `--preset physics`: 30 boxes thrown along the nave land on the floor, outlined green while awake
and blue once asleep.

**Mutation checks** (each: break the source, build, run `kin-physics3d` and `kin-physics3d-ecs`, restore; the four sources compared
with copies afterwards, identical):

| Mutation | Result |
|---|---|
| ray normal negated | caught (11 checks) |
| layer mask ignored by queries | caught (7) |
| ignore body ignored | caught (5) |
| sensors not excluded from queries | caught (8) |
| shape cast reports the start (fraction 0, position the start) | caught (4) |
| event batch not sorted | caught only after `test_events_destroy_order` was added (2): the first version of the tests survived it because the raw records' sort already orders everything but the events a destroy queues |
| neither the raw records nor the batch sorted | caught (5,610 checks: events differ run to run on 4 workers) |
| only the raw records not sorted | **survived**: an equivalent mutant: Jolt serialises the callbacks of one pair and the batch sort carries the order, so the first sort is a guard (it keeps the data of a pair's lowest sub shape if the callbacks ever interleave) |
| End events dropped | caught (14) |
| sleeping pair reported as ended | caught (10) |
| hinge limits ignored | caught (3) |
| distance range ignored (rod becomes slack) | caught (4) |
| break force never exceeded | caught (5) |
| restore forgets broken flags | caught (3) |
| connected bodies still collide | caught (5) |
| teleport keeps the velocity | caught (1) |
| debug draw ignores sleeping | caught (2) |
| ECS write back skipped | caught (10) |
| ECS bodies made in reverse entity order | caught (8) |
| kinematic bodies not moved from the entity | caught (3) |
| ECS write back ignores the parent | caught (2) |
| ground query ignores `ignore` | caught (1) |

**Checks run.** `ninja -C build -j8` and `ninja -C build-no3d -j8`: no warnings from the new code (the build's existing ones are Lua's
`tmpnam` and `skeleton_tests` / `kin_reference` explicit-constructor notes). `build`: `KIN_REQUIRE_GPU_TESTS=1 ctest -j4 --timeout 600`:
71 of 71 (70 before and `kin-physics3d-ecs`), `kin-render3d` on the NVIDIA device included. `build-no3d`: 65 of 65, no Jolt or
physics3d in its `build.ninja`. `kin_render3d_tests` on lavapipe (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`): exit 0,
and the new debug-line test gives the same pixel counts as on NVIDIA. Identity: `render def` and `render all` (--cascades 4 --ao
--contact-shadows --taa), 120 frames, of crowd, shadows, sponza, helmet, motion, spheres, cornell and the eight skinned scenes
(skinned_ik, skinned_ik_look and skinned_ik_limits among them, rendered by the committed HEAD binary before the first edit): 37
pictures each, all byte for byte equal to the HEAD binary's, and the 17 of the seven static scenes also to
`reference_out/p1a_base_def` and `p1a_base_all`. `physics_pile` twice: equal byte for byte.

**Limits and not done.**
- No character controller (next: P2c, on `shape_cast`, `overlap`, `follow_transform` and the constraints), grab or climb, ragdoll or
  ropes (they use the constraints and `collide_connected` here); no soft bodies, no per-contact material callbacks, no
  contact-modification hooks, no collision groups beyond the layer table.
- Contact events carry closing speed, not the solver's impulse (the solver has not run when a contact is added).
- A static sensor does not see sleeping bodies (Jolt); a pair that both sleep is held as dormant by the world, not Jolt.
  A woken body that leaves while its other half sleeps is ended two steps after it was last seen (a grace for contacts
  that are re-found a step late).
- `shape_cast` is for convex shapes and compounds (Jolt's rule: not meshes or height fields as the swept shape); ties between
  equal times go to whichever Jolt found first (deterministic, not by body number).
- Constraint force estimates for breaking are the last collision step's impulse over its length: with `collision_steps` > 1 and many
  solver iterations they are approximate. Hinge, slider and cone limits are measured from the pose at creation.
- The debug mesh is made again each frame (one `create_mesh`; no cost when off). A curved shape's outline is exact and the lit mesh
  of it a polygon, so a sphere's circles show dotted where its facets lie outside them. The lines are moved 0.2 % of their distance
  toward the eye: in a view with a very near plane or an orthographic one the shift is fixed (4 mm). A body with a mesh of 100,000 triangles is drawn with at most `max_triangles_per_shape` (600).
- ECS: no collider cache (identical shapes are made per entity unless a ready `Shape3D` is given); no scale tracking after
  creation (use `rebuild`); a kinematic child of a moving parent is placed by its world pose with `follow_transform`, so its
  velocity is the parent's included, once a step.
- Sponza Walk's floor is a plane at y = 0, not Sponza's own collision (the viewer has no triangles of the model).
- Cross-platform determinism of events and constraints is Jolt's claim; only this machine was run (as P2a).
- Not run: the "all-on" identity render on the Windows or MSVC build; the panel's click path of the new section (no display).


#### P2c — character controller — results

Done 2026-10-11 on `exp/3d-rendering` (head 2e665ad plus the working tree, not committed). Grab and climb, ragdoll and ropes are the
next steps (P2d onward); the constraints and the character they use are here.

**What was built.**

| Piece | Where | Notes |
|---|---|---|
| Controller | `kin/physics3d/character3d.hpp` (data), `World3D::create_character`, `set_character_input`, `character`, `character_contacts`, `teleport_character`, `set_character_up`, `set_character_crouch`, `set_character_desc`, `character_events`; `engine/src/physics3d/character3d.cpp` | Jolt's `CharacterVirtual` behind kin types, no Jolt header in a public header; handles are `{index, generation}` |
| ECS | `CharacterController3D`, `PhysicsCharacter3D`, `CharacterEvents3D`, `CharacterAnimationLink3D`, `locomotion_animator`, `drive_character_animator` in `kin/ecs/physics3d.hpp` | made in ascending entity id order after the bodies; written back to `Transform3D`; events carry entity ids |
| Level, route | `tests/character_level.hpp`, `tests/reference/scenes/physics_character.json` | one level and one scripted route (commands, teleports, checkpoints) for the CPU test, the picture and the bench |
| Tests, bench | `kin-physics3d-character` (CPU), 6 more functions in `kin-physics3d-ecs`, `kin_physics3d_bench characters` | |
| Harness, viewer | `kin_reference` scene `physics_character`; Sponza Walk section "Character controller", `--preset controller` | |

**Decisions where the plan was open.**
1. *Characters update BEFORE the world's step* (Jolt's samples do the same: `PrePhysicsUpdate`): a character is moved by swept tests against
   the world as the last step left it, a kinematic platform already holds the velocity this step will give it (so a character on it is
   carried exactly), and what it pushes gets its impulse before the solver runs. Within the call, characters go in ascending handle
   number (documented in `character3d.hpp`, tested by two characters meeting: the lower number moves first and gets the room; made in
   the other order, the other one does).
2. *The controller has a body in the world* (Jolt's inner rigid body, a kinematic capsule in layer `Character`, never asleep): other bodies
   are stopped by it, sensors report it, queries hit it (`ignore` it with `CharacterState3D::body`), the debug draw shows it. It is a slot
   of the world's body table (counted in `body_count()`, in `bodies()` and in the hash); the body calls refuse it (`destroy_body`,
   `set_position`, ...).
3. *Velocity is the controller's own, not Jolt's.* The character keeps (a) the velocity it achieved (displacement over dt, the ground's
   included; a step up a stair shows as a spike), (b) a "control" velocity for the next step's acceleration, which leaves out the
   height corrections that stair walking and stick-to-ground add (a snap down a ramp is not a speed: with it in, a ramp sped the character
   up 5 x in 8 steps), (c) its speed relative to the ground it stands on. Acceleration, deceleration and air acceleration are limits on
   how fast that relative velocity approaches the wish, which is why a platform that has just got going does not drag it backwards.
4. *Along the ground, not across the plan.* The wish speed is held along a slope; what is sent to Jolt is a horizontal vector whose slide
   along the surface is that speed when going uphill (a velocity with an upward part makes Jolt think the character is leaving the
   ground), and the tangent itself downhill (a horizontal one runs off the surface). A ground normal steeper than it may stand on (stair
   walking says it is on the ground while on a step's corner) is not walked along.
5. *ExtendedUpdate is written out* (slide, stick to the floor, walk stairs: the same calls in the same order) so that the slide's own move
   can be told from the snaps after it.
6. *Wall friction is Coulomb, applied before the move* from the contacts of the last step (Jolt's `OnContactSolve` hook is not called for
   a contact the character already rests on, which was found by a test that did nothing). Ground friction scales acceleration and
   braking (ice 0.05). Zero friction slides along a wall at full speed; the speed into it is killed by the solver.
7. *Up is a vector, per character.* Default: against the world's gravity (it follows `set_gravity`); `set_character_up` makes it explicit.
   It turns toward its target by normalised blending (a fraction `up_blend_rate x dt` of the way a step, square roots only: no
   trigonometry, so the same bits everywhere), around a fixed side when it must turn exactly upside down; the capsule's rotation is the
   shortest arc from +Y. Gravity for a character is `|world gravity| x gravity_scale` along -up.
8. *Input is a command*: `move` (x right, y forward, cut to length 1), `forward` (any world vector; its part across up is used), `jump`
   (a level: a jump on a rising edge), `crouch`, `sprint`. Coyote time and jump buffering are options, off.
9. *State in the hash and saved state.* The hash gets each character's own state (command, up vector and target, stance, timers, achieved
   and relative velocities, what it stands on, which bodies it touches) after the bodies and constraints (a world with none hashes as
   before); `save_state` writes it, and Jolt's own character state after the system's. Restore puts each character in the stance saved
   BEFORE the system restores (found by a test: a body's saved position is its centre of mass, and a capsule has another centre
   crouched, so restoring first and reshaping after moved it 0.4 m).

**Numbers** (this machine; `kin-physics3d-character`, one thread, the measured values the tests print with `-v`).

| Behaviour | Measured |
|---|---|
| Walk, sprint, crouch speed on the flat | 4.00, 7.00, 2.00 m/s (acceleration 40 m/s^2: 0.1 s to 4) |
| Speed along a slope of 20, 35, 44 degrees | 4.00 m/s along it (the horizontal command is 4 / cos angle going up) |
| 50 and 60 degree slopes | slides: loses 5.3 m and 6.2 m of height in 1 s, state `Sliding` |
| Ledge of 0.35 / 0.6 m, max step 0.4 | climbed / blocked at the foot; with max step 0.2: 0.35 blocked, 0.19 climbed; the round capsule climbs about 0.1 m past `max_step_height` (0.45 yes, 0.5 no) |
| Six stairs of 15 cm at 4 m/s | top at y = 0.900, on the ground |
| 30 degree ramp down at 7 m/s | 0 steps in the air, 5 cm above the surface (its padding); stick-to-ground off: 48 steps in the air, up to 0.85 m above it |
| Hump 0.3 m high (radius 10) at 7 m/s | 0 steps in the air, top y = 0.30 |
| Jump at 5 m/s | apex 1.233 m (analytic 1.274: the discrete step loses v dt / 2 = 0.042), 0.583 against 0.625 at g = 20; 60 steps in the air (analytic 61) |
| 2 cm wall at 60 m/s | stopped at x = 4.67 (the wall's face less radius and padding) |
| Platform at 2 m/s for 3 s | 0.00 m off it; ground speed 4.00 walking on it (world 6.00); 2.0 m/s as it jumps off |
| Platform turning at 1 rad/s, 2 m out, 6 s | radius 2.000, angle exact to 0.02 rad |
| Light box (10 kg) / heavy (800 kg, mu 0.9) / heavy with 20 kN | box moves 6.24 m (character 6.96) / 0.00 m (character stopped at 0.78) / 6.39 m |
| Ceiling 0.4 m over its head | hit at 4.18 m/s, falling again 2 steps later (no hanging); head stops at 2.16 under a slab at 2.2 |
| Gravity flipped | up turns at most 8.8 degrees a step, within 1 degree after 49 steps (0.8 s), then stands on the ceiling at y = 6.00 and walks at 4.00 m/s; a wall at x = 10 the same, a jump from it rises 1.27 m along its normal |
| Wall: free fall 0.5 s / pressed hard with friction 1 | 4.91 m/s (g t = 4.90) / 0.91 m/s |
| Sliding along a wall at 45 degrees, friction 0 | (2.83, 0, 0): the full speed along it, 0.00 into it |
| Dropped from 2 m | `Landed` at 6.21 m/s (sqrt(2 g h) = 6.26) |
| Two characters meeting | the lower number reaches x = -0.300, the other 0.320 (made the other way round it flips) |

*Cost*, `kin_physics3d_bench characters` (60 Hz steps, the test level; walkers on a grid that walk +X and back, one jump each 97 steps,
mean of steps 240 to 480; 500 boxes are a heap the walkers walk into; a loaded desktop: indicative):

| Workers | Boxes | Characters | Step mean / worst ms | Per character |
|---|---|---|---|---|
| none | 0 | 0 / 1 / 10 / 100 | 0.005 / 0.035 / 0.029 / 0.182 | 31 us (one awake body), 2 us |
| none | 500 | 0 / 1 / 10 / 100 | 0.178 / 0.186 / 0.179 / 0.726 (worst 1.03) | 8 us, 0 us, 5 us (they wake up to 197 bodies) |
| 4 | 0 | 1 / 10 / 100 | 0.132 / 0.157 / 0.293 | 129 us (the job system's wake-up), 15 us, 3 us |
| 4 | 500 | 1 / 10 / 100 | 0.230 / 0.265 / 0.686 (worst 3.1) | 4 us |

So a character costs about 2 to 5 microseconds a step (a capsule's swept tests against a level of boxes), 100 of them 0.2 to 0.7 ms,
well under 5 % of a 60 Hz frame; the fixed cost of one awake body (about 30 us) and of waking the job system with workers (about
0.1 ms) is what the first character pays. The controller runs on the calling thread whatever the workers.

**Tests.**
- `kin-physics3d-character` (CPU, 0.3 s, 26 functions): stand and rest (feet y within 3 cm, ground body and user id, no drift in 300
  steps); walk, sprint, half input, braking, a diagonal not faster; the camera frame (forward and strafe, a forward along up moves
  nothing); slopes 20 to 44 climbed at 4 m/s along them (ground normal checked), 50 and 60 slid; stairs of 15 cm (top and still on the
  ground), ledges against the step height both ways; stick to ground (a 30 degree ramp at 7 m/s with it on: 0 steps in the air, gap
  < 5 cm; off: more than 5 steps, gap > 5 cm; a hump); jump apex and flight time against the analytic law at two gravities,
  `gravity_scale`, a held command not jumping again; tunnelling at 60 m/s through 2 cm; a platform moving (carried, ground speed, leaving
  speed) and one turning (radius, angle); pushing a light box, a heavy one (blocked) and a heavy one with strength; standing on a
  dynamic raft; a ceiling bump (one `HitHead`, no hanging); a sensor (walked through at speed, one begin and one end); layers (masked
  out and not); gravity turned over (smooth turn: monotone, largest step, settled; stands and walks on the ceiling), a wall (stands,
  walks, jumps along its normal), an explicit up on a world of ordinary gravity; wall friction (pressed fall, slide along a wall, off
  the wall at once); crouch (cannot go under standing, crouched speed, `stand_blocked` under it, stands outside, the explicit call
  refused under a roof, the body's shape follows by ray); events (`Landed` speed, `LeftGround` with `jumped`, ContactBegin / End with
  user ids, the order of the kinds); coyote time and jump buffering (on and off); handles (refused descriptors, the body is its own,
  generations, slot reuse); teleport; the order of two characters, both ways; the scripted route of the scene file: 21 checkpoints
  (positions at frames, the stance), the light box moved more than 5 m, the heavy one not at all, the trigger entered and left once,
  four landings, no head bump; determinism (hash every step of 240 steps, a level with 20 boxes and 5 characters on seeded commands,
  equal run to run and with 1 and 4 workers); save, run 120 steps, restore, run the same 120 again (equal hashes), a second world made
  the same way taking the state, a world with another set of characters refusing it; the hash seeing the command alone, the up target
  and one more character.
- `kin-physics3d-ecs` (6 more functions): the entity's character made after the bodies in entity order (a collider on it makes none),
  settles, walks, facing written (and not when `apply_facing` is off), game code moving the entity is a teleport, events with entity ids
  and the singleton, the entity going takes the character; carried by a kinematic platform moved from game code (6 m, within 6 cm);
  under a parent (made at the parent's world place, written back in its space), interpolation; the animation link (speed 4.00 walking,
  airborne in the jump, the foot placer's ground is the floor and not the capsule, weight 1 / 0); the foot placer on the capsule's own
  physics ground (ankles at 0.15 on a step and 0.05 on the floor to 4 mm); a scene of crates and four characters hashed per step, equal
  on 4 workers.
- `kin_reference` `physics_character`: the picture of each camera twice, byte for byte equal (also with `--cascades 4 --ao
  --contact-shadows --taa`, which has no stored baseline).

**Pictures** (`reference_out/p2c_physics_character/`, 1280x720). Looked at: `stairs`, the person (with a spread, mid-stride walk pose)
on the third of six steps, its capsule outlined in cyan, a green axis and a red cross at its feet (the contact and its normal), the
stairs, a plateau and the 20 degree ramp (orange) behind; `tunnel`, the person crouched under the roof, squashed to the capsule's height
(there is no crouch clip), arms out against the walls, the capsule's rings and the roof's dark underside; `platform`, the person
standing on the blue platform that moves, shadow on it; `spinner`, standing on the turning teal platform, the blue one behind;
`overview`, the whole level small from above (ramps, the hump as a green disc, stairs with the person a speck on them, the tunnel, the
platforms, the trigger as a yellow wire box). Sponza Walk `--preset controller` (`--eye -14,2,0 --target 0,1,0`): the person in the
nave with its capsule, the level's boxes in front.

**Mutation checks** (each: break the source on purpose, build, run `kin-physics3d-character`, `kin-physics3d-ecs` and `kin-physics3d`,
restore; the five sources compared with copies afterwards, identical):

| Mutation | Result |
|---|---|
| max slope ignored (always 89 degrees) | caught (10 checks) |
| step height ignored (stairs off) | caught (5) |
| stick to ground off | caught (6) |
| gravity ignores the up vector (always -Y) | caught (12) |
| up vector does not follow the world's gravity | caught (5) |
| jump 20 % short, from the ground | caught (11) |
| jump 20 % short, from coyote time | caught (1) |
| platform (ground) velocity not added | caught (14, and 3 in the ECS test) |
| layer mask ignored | caught (1, and 3 in the ECS test) |
| crouch clearance check removed | caught (3) |
| characters updated in reverse number order | caught (2: the meeting test) |
| characters left out of `state_hash` | caught (2) |
| restore does not restore the velocities | caught (4) |
| stance restored after the physics system, not before | caught (3) |
| ECS write-back of the feet skipped | caught (10) |
| ECS: input not sent to the character | caught (9) |
| ECS: game moving the entity is not a teleport | caught (2) |
| wall friction ignored | caught (1) |
| up turns at once (blend rate ignored) | caught (5) |
| push strength unlimited (live) | caught (5) |
| coyote time ignored | caught (1) |
| `HitHead` events dropped | caught (2) |
| sprint speed ignored | caught (6) |
| push strength unlimited, at creation only | **survived, as it should**: an equivalent mutant (`apply_desc` sets it again right after creation); the row above is the version that matters |

Bugs the tests found while writing them (each is now a check): the up vector would not turn from exactly upside down (a normalised blend
stays on its line: it now aims at a side first); a restore moved a crouched capsule 0.4 m (the stance has to be set before the system
restores); the 15 cm stairs were a hop at best (a rolled-over edge's upward speed was carried into the air); a character on a platform
that had just got going was braked as if it ran backwards on it; a ramp down sped the character up 5 x (a stick-to-ground snap counted
as speed); the "step height" case of 0.45 m launched the character 7 m up (the same carry); `OnContactSolve` was never called for a wall
the character rested on, so wall friction did nothing.

**Checks run.** `ninja -C build -j8` and `ninja -C build-no3d -j8`: no warnings from the new code (the existing ones are Lua's `tmpnam`
and the explicit-constructor notes of `skeleton_tests`, `physics3d_ecs_tests` and `kin_reference`). `build`: `KIN_REQUIRE_GPU_TESTS=1
ctest -j4 --timeout 600`: 72 of 72 (71 and `kin-physics3d-character`), `kin-render3d` on the NVIDIA device included. `build-no3d`: 65 of 65, and its
`build.ninja` has no Jolt, physics3d or character3d in it. `kin_render3d_tests` on NVIDIA (`VK_ICD_FILENAMES` unset) and on lavapipe
(`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`): exit 0 each, the debug-line pixel counts the same on both (106 / 426 / 96).
Identity: `render def` and `render all` (--cascades 4 --ao --contact-shadows --taa), 120 frames, of crowd, shadows, sponza, helmet,
motion, spheres, cornell, skinned_bend, skinned_ecs, skinned_blend, skinned_cesium, skinned_fox, skinned_ik, skinned_ik_look,
skinned_ik_limits and physics_pile (the skinned_ik*, skinned_ik_limits and physics_pile ones rendered by the head binary before the first
edit): 39 pictures each, all byte for byte equal to the head binary's, and the 17 of the seven static scenes also to
`reference_out/p1a_base_def` and `p1a_base_all`. `physics_character` twice, default and all-on: equal byte for byte.

**Limits and not done.**
- Jolt's capsule rolls over a corner, so the highest step is about 0.1 m past `max_step_height`; stair walking at 4 m/s puts it on a
  step's corner for a few steps before it rolls up (the achieved velocity shows a vertical spike there). A very steep or very fast
  approach to a stair can still be refused.
- The visual crouch is a squash of the model (no crouch clip); the locomotion clips of the test person are synthetic (swing, fold).
- Character against character only blocks (no pushing); a dynamic body that falls on a character is held off by its kinematic body but
  does not press it down (the character's own mass acts on what it stands on).
- Jolt's friction does not exist for a character: `ground_friction` scales how quickly it changes speed, nothing more; no per-material
  ground (ice patches) yet, though the field is per character and can be set from the ground's user id.
- The viewer: the level runs through Sponza's walls (the physics world has only the floor and the level); the keyboard path is not
  exercised by any test (no display here): the scripted `ctrl_demo` goes through the same command struct and is what the headless run
  shows.
- Cross-platform determinism is Jolt's claim; only this machine was run (as P2a). The platforms of the level use `std::fmod` and
  `std::sin` (game code, not the engine) for their poses, so the level itself is not claimed to be cross-platform.
- Not done: the character as a dynamic-body-aware "stand on a ragdoll"; grab and climb, ragdoll, ropes (next); a networked rollback
  test (the command struct and the saved state are there; no netcode exists yet).
- `tools/reference/make_scenes.py` (run by `fetch.py`) writes `skinned_biped_walk.glb` into the cache's `made/`; on this machine it was
  written by calling that function alone (the other files are byte for byte what it makes: checked); `kin_reference` says so when the
  file is missing and the scene does not render.

#### P2d — grab and climb — results

Done 2026-10-11 on `exp/3d-rendering` (head bca1c18 plus the working tree, not committed). A GENERAL traversal capability on the
character controller, two hands by default, hands only; nothing in it knows about a mountain. Ragdoll and ropes are next (the grip
providers' interface is ready for a rope and none is built).

**What was built.**

| Piece | Where | Notes |
|---|---|---|
| Data, providers, resource | `kin/physics3d/traversal3d.hpp`, `engine/src/physics3d/traversal3d.cpp` | `SurfaceMaterial3D`, `HoldDesc3D`, `GripProbeDesc3D`, `GripCandidate3D`, `Resource3D`, `TraversalDesc3D` (in `CharacterDesc3D::traversal`), `TraversalInput3D`, `GripState3D`, `TraversalState3D`, the `GripProvider3D` interface (probe, attach, move, detach) with the hold, ledge and surface providers; no Jolt header |
| Probe, material, hold, state | `World3D::probe_grips`, `set_surface_material`, `set_hold`, `set_traversal_input`, `traversal`, `set_traversal_resource` | the probe is written over the public queries (`shape_cast`, `raycast`, `overlap`) |
| The body response | `engine/src/physics3d/traversal_character3d.cpp`, hooks in `character3d.cpp` | grips, hanging, shimmy and climb, slip, load, mantle, resource, exhaustion; Jolt's swept `CharacterVirtual::Update` makes every move |
| Events | `CharacterEventKind3D::Gripped, Slipped, GripReleased, Exhausted, MantleStarted, MantleFinished, MantleBlocked, Fell` (+ `hand`, `reason`) | with the walking events, sorted by (kind, limb) |
| ECS | `Hold3D`, `SurfaceMaterial3D` (the physics struct is the component), `CharacterController3D::traversal` / `traversal_state`, `CharacterAnimationLink3D::hands` / `hanging` / `hand_clearance` / `align_hands` / `hand_offset`, `LocomotionClips3D::hang`, `drive_traversal_animator`, `WorldPhysicsState3D::traversal_of` | facing turned to the surface, the hands' IK on the anchors with the weight of the phase |
| Debug draw | `DebugDraw3D::grips`, `candidates`, `WorldPhysicsState3DConfig::debug_grips` | anchors by phase, normals and frames, line to the body, hang place, ledge top, a probe's candidates by provider |
| Level, timeline | `tests/climb_level.hpp`, `tests/reference/scenes/physics_climb.json` | one level and one scripted timeline (commands, teleports, stamina, checkpoints) for the CPU test, the picture and the bench |
| Tests, bench | `kin-physics3d-traversal` (CPU), 3 functions in `kin-physics3d-ecs`, `kin_physics3d_bench climbing` | |
| Harness, viewer | `kin_reference` scene `physics_climb`; Sponza Walk section "Climb (kin/physics3d, P2d)", `--preset climb` | |

**Decisions where the plan was open.**
1. *Traversal is a mode of the character, inside the controller's step* (not a second object): each step runs the limbs first (grips,
   edges of the commands, the resource), then either the hanging or mantling move or the walking controller of P2c. With
   `traversal.enabled` false nothing of it runs and no byte of it is in the header the hash reads, so a P2c scene hashes as it did (the
   five P2c test programs pass unchanged and the pictures of `physics_character` and `physics_pile` are the head binary's, byte for
   byte). `enabled` is fixed at creation (it is in the saved state's layout); the other numbers are live.
2. *The move is kinematic and swept*: the hanging character's feet are driven to the hang place with Jolt's `CharacterVirtual::Update`
   at a velocity that arrives in one step, so it cannot pass through a wall or the floor (it stands on tiptoe when the hands are low), and
   the same call carries the pull-up. Nothing is a constraint: the plan floated "a constraint or a kinematic move"; a constraint to a
   body would have made the character a dynamic body that can be pulled through things.
3. *Anchors live in the grabbed body's own space* (position and normal), and the move uses where they will be at the end of the step
   (`position + (v + w x r) dt`), so that a kinematic platform carries the hands and the character together with no lag (measured:
   the carried offset stays within 4 cm over a there-and-back, the swing is only the character's reaction to acceleration). A dynamic
   body that is held gets the character's weight as a force at each anchor, shared by the hands that hold it, added before the
   world's step: the crate goes over its pedestal's edge and the character follows its anchor down.
4. *The swing is a spring about the hang place, measured from the anchors as they are and driven to where they will be*: 0.9 Hz and a
   damping ratio of 0.4 across `up` (a pendulum), 3 Hz and 1 along it (the arms); its velocity is read back from what the move
   achieved, so a floor in the way does not wind it up. A grab in the air keeps 0.4 of the speed as the start of the swing.
5. *Hands are independent*; each has its own grip, reaches for 0.15 s (the IK fades in on it), and is not put within 0.2 m of another
   hand's anchor (two hands on one point is not a grip). A limb the character let go of (exhausted, slipped, dropped, jumped off)
   is latched until its command is given up: a held button does not re-grab at once. The shoulders of the held limbs go under their
   hands: a body hangs from one hand with that shoulder under it, which is where the other hand can reach.
6. *Move is along the surface, all hands or none*: x shimmies, y climbs (a surface only; a ledge's lip is a line, and a hold with
   `snap` is a point); the anchors are re-projected on the surface by a ray, so an edge, a corner or a gap stops them, and one hand at
   the end of a wall stops the other (they do not tear apart). A hand going up a face finds the lip within 12 cm and becomes a ledge
   grip.
7. *The ledge is a ray down just inside the surface* (10 cm in, `ledge_search_height` up): a hit on the body's top within range is the
   lip; for a hit on the underside of a ledge the edge is on the hand's side. `clear` is a standing capsule's overlap (and a floor
   ray) at the place over the edge; a pull-up also sweeps the capsule over the edge. The ledge must be a top of the body that was hit.
8. *The mantle is a path in the ledge body's own space* (start, straight up the wall, then over the edge to a spot a capsule's radius
   and 10 cm in), smoothstep over `mantle_seconds` (0.7 s) with the swept move, so a moving platform under the ledge carries it.
   Failure is `MantleBlocked` (`NoLedge`, `NoRoom`, `Tired`) and it stays hanging with nothing spent.
9. *The resource is generic* (`Resource3D` and a config): drain is a rate the caller passes, so the same type is the stamina and the
   sprint; `step` returns the rising edge of exhaustion; it comes back after a delay since it was last spent and is exhausted
   with hysteresis (15 % of max back). Exhausted while hanging: grips fail hand by hand, the weakest first (strength fades over 0.3 s,
   the next hand 0.4 s later), then `Fell`.
10. *Holding with the feet on walkable ground is rest*: the drain is scaled by `ground_drain_factor` (default 0), so the resource comes
    back after its delay while a hand is on the wall of a character that stands.
11. *A hold is taken from the front*: a small hold's face is found by a ray along the character's facing through the place the sweep
    went, moved 5 cm toward the hold's middle (the sweep itself grazes its side or underside).
12. *Events are collected per character and flushed after the walking events*, sorted by (kind, limb), so that the order is not the
    order the limbs were worked in (a mutant that leaves them unsorted is caught).
13. *Gravity direction is the character's `up`* in every frame, force and path above: the whole level turned 90 degrees about Z (a wall
    that is a floor, gravity along +X) or 35 degrees about X and 20 about Y gives the same climb and the same place on the ledge (the
    two agree to better than 0.1 mm, printed 0.0000, against a 6 and 8 cm bound in the test).
14. *The probe is written over the public queries* (a fan of seven sphere sweeps and a ray each: the sweep gives the body and the
    reach, the ray the face and its exact normal): a game can do the same from outside, and the providers need nothing of Jolt.
    The first thing a cast meets is the only thing it sees (documented limit).

**Numbers** (this machine; `kin-physics3d-traversal` with `-v`, one thread; the level is `tests/climb_level.hpp`).

| Behaviour | Measured |
|---|---|
| Reach for a hold | the hand arrives in 0.15 s (9 steps), `Gripped` then; the body swings in to the wall over about 0.5 s |
| Hanging place | feet 1.70 m under the hands (shoulder 1.45 + 0.25), 0.34 m (radius + 4 cm) out from the face: z = 0.338 |
| Climb of the 4 m wall from the floor | 0.6 m/s along the face; the lip is found 12 cm below it and both hands become ledge grips at y = 4.000; 2.17 m in the 217 steps counted from the first hold (the first hold is at 1.73 m) |
| Pull-up | 0.7 s (42 steps): mantle progress 0.762 at step 32; ends standing at y = 4.000, z = -0.400 (the radius and 10 cm inside the lip), on the ground, no landing event |
| Drain, two hands / one hand | 7.00 / 12.00 units a second (ratio 1.714); wet rock (x 2): 14.00; moving +4; ice (x 2) with two grabs: 25.0 in 1.5 s of sliding |
| Resource | 5.000 left of 10 after 1 s at 5 a second; no regen inside the 0.5 s delay (5.000), then 4 a second (7.0 after 0.5 s more); exhausted once (the edge) and until half is back |
| Sprint (20 units, 20 a second) | 7.0 m/s while it lasts, 4.0 m/s from step 62 (exhausted), sprint again after the rest (delay 1 s, 10 a second) |
| Low stamina climb (20 units) | exhausted at step 104 (worked out from the rates: 104), first hand lets go 17 steps later, the second 25 after that, `Fell` the same step |
| Exhaustion with 6 units (after the grabs 2 left) | exhausted 17 steps after the second grab (analytic 17.1), weakest hand 17 steps later (0.3 s fade less the step it began on), the other 25 later (0.4 s) |
| Shimmy | 1.00 m in 60 steps (1.0 m/s); stops 0.4 m apart at the wall's end (x = 2.6 and 3.0), both still holding |
| Slip on ice | slides at 1.200 m/s for 1.500 s (1.78 m), `GripReleased` (Slipped) and `Fell` at the end; the `Slipped` event's speed 1.2 |
| Jump off | 3.00 m/s away from the face and 3.84 m/s up one step on (4.0 less g dt); off the platform that moves at 1.5 m/s: x velocity 1.50 |
| Platform carrying | the anchor's place in the platform's own space unchanged to the bit over 8 s there and back; the character within 4 cm of its offset; the anchor on its face (z = 0.800) |
| Crate (30 kg, a third over the edge) | alone: stays at (y 2.40, z 0.90) for 2.5 s; held: over the edge within half a second (turned by more than 20 degrees), 1.0 m lower or more at 2.5 s, the anchor 0.000 m from where its own-space point is on the crate; a 10 t crate does not move |
| Probe | 0.70 m to a wall face (point z = 0.000, normal +Z, frame right = +X, up = +Y); a hold 0.15 out of a smooth wall, the ledge's lip y = 4.000 with the stand 0.35 in and clear (not clear under the roof); the same list bit for bit twice |
| Redirected gravity | climb on the level turned 90 degrees about Z and 35/20 about Y/X: ends where the upright one does to 0.0000 m |
| Determinism | hash every step of 600 steps, three climbers, a platform, a crate and ice: equal run to run and on 1 and 4 workers; save at step 150 (hanging), run 400, restore: equal hashes; the same mid-pull-up; a second world made the same way taking the state |

*Cost*, `kin_physics3d_bench climbing` (60 Hz, no workers, a loaded desktop: indicative; 500 boxes are a heap in front of the wall):

| What | Cost |
|---|---|
| One probe (`probe_grips`: seven sweeps, up to seven rays, the ledge ray and the capsule overlap) | nothing in reach 1.4 us; a wall face 14.3 us; a ledge with its clearance 15.9 us; holds on a smooth wall 14.6 us; 13.6 to 15.6 us with the 500 boxes round it |
| A hanging character, 1 / 10 / 100, no boxes | 0.015 / 0.045 / 0.357 ms a step: 12 us for the first (one awake body), 4 us each after |
| The same shimmying | 0.016 / 0.061 / 0.537 ms: 5 to 6 us each (a re-projection ray and a ledge ray a hand) |
| Hands asked to grab with nothing in reach (two probes a step), 1 / 10 / 100 | 0.017 / 0.072 / 0.750 ms: 7 to 9 us each |
| With 500 dynamic boxes, hanging / shimmying / asking, 100 characters | 0.440 / 0.650 / 0.901 ms (the heap alone 0.042) |

So a hanging character costs 4 to 6 microseconds a step and a free hand's probe about 15 when it finds something and 1.4 when it does
not; 100 characters hanging and shimmying cost 0.5 ms, under 4 % of a 60 Hz frame, all of it on the calling thread.

**Tests.**
- `kin-physics3d-traversal` (CPU, 0.5 s, 26 functions): the probe (a wall face and its frame, the same list twice, out of reach, the
  layer mask both ways, the ground not held, a smooth wall with and without holds and with holds only, tag/slip/drain in the candidate, a
  ledge with its clearance and not under a roof, no ledges wanted, a fan, `max_candidates`, invalid input); a climb to the top and a
  pull-up (the reach for 0.15 s, held, the hang's place, the speed of the climb, the lip found, the pull-up's phases and end, the events
  and their reasons, walking on); phases and latch (reach cancelled, one hand, two hands not on one place, a hand released for one step
  and free, the level held, drop, no regrab until the command is given up); two hands against one (7 and 12 a second) and wet rock
  (14); `Resource3D` on its own; the sprint spending it and stopping, resting, a character without traversal sprinting for ever; an
  exhaustion with 6 units (the times worked out) and with none; a low-stamina climb whose exhaustion step is worked out from the
  rates; events sorted (a step with exhaustion and both hands' release); the mantle blocked under a roof (and made once the roof
  is gone, `Tired`, `NoLedge`; the press an edge); the shimmy (both ways, to the end of the wall); jump off (velocity, the platform's
  speed carried) and drop; the platform carrying a grip over its whole cycle; the crate's load (alone and held, the anchor following
  in its own space, a heavy crate); ice (the slip, the release time, the distance, the tag, the twice drain); the body lost; handles
  and descriptors (nonsense numbers refused, `enabled` fixed, a character without traversal, a slot reused by another body);
  an authored hold (snap, strength, fixed, the second hand not on the same point, then on another hold); three limbs; standing on
  the ground is rest (and not with a factor); walking unchanged (traversal on and unused is the same feet bit for bit as off); the
  turned level; determinism (three climbers on commands, 600 steps, 0, 1 and 4 workers, the events counted); save and restore
  mid-hang and mid-pull-up and into another world; the hash seeing the command, the resource and a let-go grip; the debug lines;
  the scene file's timeline: 19 checkpoints (feet box, mode, hands holding), two pull-ups, one refusal, a slide, falls, no exhaustion,
  and the same hash every step twice.
- `kin-physics3d-ecs` (3 more functions): the components' data read when the body is made and again at `rebuild`; the command sent
  and the state written back (held, the tag of the surface, the facing turned to the wall within 3 degrees); the events with entity ids
  per step and in the singleton; the biped's hands within 2 cm of the two anchors by IK, weight 1 while they hold and 0 after,
  the `hanging` parameter, the machine's `hang` state.
- `kin_reference` `physics_climb`: the picture of each of 8 cameras twice, byte for byte equal (also with `--cascades 4 --ao
  --contact-shadows --taa`).

**Pictures** (`reference_out/p2d_physics_climb/`, 1280x720). Looked at all eight: `wall`, the person hanging by both hands near the
top of the 4 m wall, the capsule in cyan, green crosses and short blue frame lines at the two anchors, arms bent up to them, legs
apart in the fall pose; `mantle`, the person half way through the pull-up over the holds wall's lip (frame 1122: feet level with the top, about to go
over), the green lines from the grips running down to the lip and the last hold, an orange hold and the smooth wall below; `holds`, hanging from two of the orange holds of the grey smooth wall, the
lip of rock above, the other holds below; `platform`, hanging from the blue slab's face with the feet just off the floor, the
pedestal and crate small to the right; `crate`, the crate tipped over its edge and the person standing under it holding its
underside, the ice wall to the right; `ice`, hanging on the ice wall with orange (slipping) crosses at the hands; `low_roof`, hanging
by the lip of the red ledge with the dark roof over it (the pull-up refused); `overview`, the level small from above with the
person on the lip. The arms are the fixture's tubes, two-bone IK with the elbows pushed out and down: legible but not graceful.
Sponza Walk `--preset climb --frames 420 --hud`: the person on the wall's top after the scripted climb, the stamina bar at 51 of 100
under the picture, the debug capsule, Sponza behind.

**Mutation checks** (each: break the source on purpose, build, run `kin-physics3d-traversal`, `kin-physics3d-ecs`,
`kin-physics3d-character` and `kin-physics3d`, restore; the five sources compared with copies afterwards, identical; the P2c and P2b
programs failed nothing in any of them):

| Mutation | Result |
|---|---|
| anchor stored in world space, not body-local | caught (65 in `kin-physics3d-traversal`) |
| probe ignores the layer mask | caught (1 in `kin-physics3d-traversal`) |
| grippable flag (surface tag) ignored | caught (9 in `kin-physics3d-traversal`) |
| drain multiplier of the surface ignored | caught (2 in `kin-physics3d-traversal`) |
| two-hand discount removed | caught (10 in `kin-physics3d-traversal`) |
| mantle clearance check removed | caught (8 in `kin-physics3d-traversal`) |
| resource regen ignores its delay | caught (7 in `kin-physics3d-traversal`) |
| load not applied to the dynamic body | caught (3 in `kin-physics3d-traversal`) |
| traversal state not in the hash and saved state | caught (11 in `kin-physics3d-traversal`) |
| restore does not restore the grips | caught (11 in `kin-physics3d-traversal`) |
| events not sorted | caught (2 in `kin-physics3d-traversal`) |
| up vector ignored while hanging | caught (6 in `kin-physics3d-traversal`) |
| slip never lets go | caught (3 in `kin-physics3d-traversal`) |
| jump-off has no upward kick | caught (3 in `kin-physics3d-traversal`) |
| exhaustion never lets go | caught (7 in `kin-physics3d-traversal`) |
| sprint not stopped by exhaustion | caught (1 in `kin-physics3d-traversal`) |
| a limb let go by the character grabs again at once (no latch) | caught (5 in `kin-physics3d-traversal`) |
| platform motion not predicted over the step | caught (4 in `kin-physics3d-traversal`) |
| hand does not find the ledge when climbing | caught (7 in `kin-physics3d-traversal`) |
| hands may take the same place | caught (4 in `kin-physics3d-traversal`) |
| grab cost not taken | caught (8 in `kin-physics3d-traversal`) |
| standing on the ground drains as hanging does | caught (1 in `kin-physics3d-traversal`) |
| ECS: traversal command not sent | caught (19 in `kin-physics3d-ecs`) |
| ECS: hands not driven (weight stays) | caught (3 in `kin-physics3d-ecs`) |
| ECS: facing not turned to the surface | caught (1 in `kin-physics3d-ecs`) |
| ECS: surface material component not applied | caught (5 in `kin-physics3d-ecs`) |
| ledge found from the wrong side (inset the other way) | caught (52 in `kin-physics3d-traversal`) |
| shimmy moves hands independently (a blocked hand does not stop the other) | caught (3 in `kin-physics3d-traversal`) |

All 28 were run twice, the second time against the final sources (a late change to where the pull-up ends, 5 mm lower, came after
the first). "hands may take the same place" survived the first run (0 failures: no test had sent both hands at one point); the check
"both hands for the one snapped hold take one grip" was added to `test_authored_hold_snaps` and it is caught (4). No mutant survives
and none is known to be equivalent (the table is the second run). Several rows count many failures because the mutated function is everywhere (the
anchors in world space moves every grip on a body that is not at the origin; the ledge searched from the wrong side fails every
climb).

Bugs the tests and the pictures found while writing them (each is now a check or a decision above): a hold was taken by whichever
face the sweep grazed (a side or the underside), turning the character sideways to it or giving no ledge; a body hung from one
hand with its middle under that hand, out of reach of the other; a platform carried the hands with a lag of 0.2 m (the swing was
measured from where the anchors will be, not where they are); a grippable lip exactly flush behind a smooth wall was never
seen (the first-hit rule); the ray along the top plane of a ledge missed it, so the shimmy never moved; both hands aimed at a
big hold took its middle, the second refused; a character set to 0 stamina regenerated at once (a set restarts the delay now);
`Landed` fired after a pull-up for the few millimetres the stand is above the floor.

**Checks run.** `ninja -C build -j8` and `ninja -C build-no3d -j8`: no warnings from the new code (the existing ones are Lua's `tmpnam`
and the explicit-constructor notes of `skeleton_tests`, `physics3d_ecs_tests` and `kin_reference`). `build`: `KIN_REQUIRE_GPU_TESTS=1
ctest -j4 --timeout 600`: 73 of 73 (72 and `kin-physics3d-traversal`), `kin-render3d` on the NVIDIA device included. `build-no3d`: 65 of
65, and its `build.ninja` has no Jolt, physics3d, character3d or traversal in it. `kin_render3d_tests` on NVIDIA (`VK_ICD_FILENAMES`
unset) and on lavapipe (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`): exit 0 each, the debug-line pixel counts the same
on both (106 / 426 / 96). Identity: `render def` and `render all` (--cascades 4 --ao --contact-shadows --taa), 120 frames, of crowd,
shadows, sponza, helmet, motion, spheres, cornell, skinned_bend, skinned_ecs, skinned_blend, skinned_cesium, skinned_fox,
skinned_ik, skinned_ik_look, skinned_ik_limits, physics_pile and physics_character (the head binary, copied before the first edit,
rendered all of them): 44 pictures each, all byte for byte equal to the head binary's, the 17 of the seven static scenes also to
`reference_out/p1a_base_def` and `p1a_base_all`, and the 5 of `physics_character` to `reference_out/p2c_physics_character`.
`physics_climb` twice, default and all-on: equal byte for byte.

**Limits and not done.**
- The casts stop at the first thing they meet: a grippable body exactly flush behind a non-grippable one is invisible to a hand
  (the level's lip stands out 20 cm), and a hold behind another body is not found. A ledge must be a top of the body that was hit.
- A hand goes where it is aimed (the camera's forward for all hands): choosing the best hold for a limb is the game's. The probe is
  15 us, so a game can ask several.
- Hands only: the legs hang (the clip's pose; no sway, no foot placement while hanging, no foot grips). The hang clip is the `fall`
  pose in the fixture (a `hang` clip is a name on `LocomotionClips3D`, not made); the arm IK is the fixture's T-pose tubes, with
  `hand_offset` untried on any other rig.
- The pull-up cannot be interrupted (no drop or jump off while it runs) and does not look at what moved into the way after it
  started (the swept move stops at it and the path ends where it can).
- Ropes and ladders as a mode are not built (a ladder is a column of `snap` holds; the provider interface is ready for a rope).
- A very fast platform is carried with the prediction of one step only; one that turns carries the anchor but not the normal's turn
  over the step.
- Two characters on the same crate each add their weight (it is correct, and untested); a character gripping a character's body
  needs the character layer in the grip mask (it is out by default).
- Cross-platform determinism is Jolt's claim and kin's arithmetic uses square roots and no trigonometry in the step; only this
  machine was run. The level's platform pose in the test uses `std::fmod` (game code).
- The viewer's keyboard and mouse path (hands on the buttons, F, X, the reach by the view) is not exercised by any test: there is no
  display here; `--preset climb` plays the same command structs from a script, which is what the headless run shows.
- Not done: ragdoll, ropes (next), a networked rollback test (the structs and the saved state are there).

### P3 — A whole scene: sky, fog, spot lights, weather

A sky as the background (a procedural gradient and sun disc first, a cubemap later), height
and distance fog that agree with it, spot lights (cone, falloff, a shadow for a few), more
than one shadow-casting light, a day-night parameter, and 3D particles (billboards, soft
particles, wind) for snow and rain; transparency ordering for them. Opt-in like the rest,
byte-identical when off. Checks: the harness scenes plus new ones (a mountain with fog, a
night scene with spot lights), cost per feature.

### P4 — The vertical slice, "Summit" (a one-player Peak-like)

A new example (`examples/summit`): a small mountain of hand-assembled rock segments with
collision, the animated character, climbing with stamina, falling and respawn, a few status
effects, rope and food items, a weather change, a ui2 HUD, audio (footsteps, wind), all from
a seed and a scripted input run that can be checked headless. It is where the missing things
show up; each finding becomes a step. Gamepad input (aiming and moving; kin has none in
`platform`) lands here if the slice needs it, along with camera controllers and rebinding.

### P5 — Netcode-ready, then co-op

*P5a (inside P4):* keep the rules honest: a fixed-step simulation reading input commands, no
rendering or wall-clock in it, its state hashed every frame, a replay of recorded inputs
reproducing it. *P5b (a plan of its own after the slice):* host-and-join over a transport
(a library to be chosen), replication of characters and objects, prediction for the
local character, then voice and a store backend.

### P6 — Content pipeline and platforms

A baked cache for levels of detail and clusters (the kiln takes 5 to 8 s and 1.1 to 1.5 GB at
load), compressed textures, glTF in the asset manifest and `.kinpak`, a level format with 3D
ECS components in prefabs; Direct3D 12 and Metal checks (everything so far ran on Vulkan on one
NVIDIA GPU and on lavapipe) and a weaker-GPU budget; a lavapipe CI job that builds the
examples; the merge to main.

### The Astroneer-like track (after the slice)

Voxel terrain (a signed distance field in chunks, surface nets or marching cubes meshed in
compute with the G3 machinery, chunk streaming, levels of detail with seams handled), editing
with a tool, collision against changing meshes, gravity toward a planet's centre (the
character controller's gravity parameter), base building by attach points, vehicles on Jolt
bodies, tethers and resources, replication of terrain edits. Its own plan, after P4.

Order: P1, P2, P3 (independent of P2: may run beside it), P4, P5a throughout, then P5b and P6.
Each step is planned with the user before it starts, implemented by a Sonnet 5.5 subagent
with the usual checks (opt-in, byte-identical when off, mutation-checked tests, the memory
rules in HANDOFF.md), and verified by the coordinator.

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
4. **meshoptimizer as a dependency** (G3): MIT, small, C++, fetched like
   cgltf, used only by the experimental 3D option (levels, clusters). The
   alternative is writing the simplifier and meshlet builder here, which is
   weeks of work to match it.
   *Decided: yes (2026-10-08).*
5. **The game to aim at** (stage P): a Peak-like first, an Astroneer-like track after
   it. *Decided 2026-10-10.*
6. **Multiplayer in the first slice**: one player, netcode-ready. *Decided 2026-10-10.*
7. **Jolt Physics as a dependency** (stage P, P2): MIT, fetched like meshoptimizer, only
   with the experimental 3D option. *Decided: yes (2026-10-10).*
