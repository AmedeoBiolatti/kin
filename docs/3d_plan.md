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
| G2b pre-pass and GTAO | ambient and environment light shut off in creases: Sponza 50.1 to 48.7 (radius 0.5), further with a larger radius | +0.25 to 0.55 ms in small scenes, +0.9 to 1.0 ms in the crowd (not turned on in Signal Breach) | off |
| G2c contact shadows | small contact shadows a coarse map misses | +0.15 to 0.3 ms (shared pre-pass with AO: +0.2 to 0.3 more); no gain on the reference scenes' distances | off |
| G2d TAA | edges and fine detail as good as or better than MSAA 4x (SSIM) from one sample, steadier; works with the rest | +0.07 to 0.15 ms fixed, second geometry pass in big scenes (kiln +1.4, crowd +0.4); +0.09 to 0.15 over AO + contact; CPU +0.3 ms for 22,000 copies | off |

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

#### G3b — Clusters

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

#### G3c — Culling and drawing on the GPU

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

#### G3d — Occlusion culling

A depth pyramid (Hi-Z) from the last frame's depth (reprojected by the
camera's motion; a conservative bound, or two passes as the standard
technique: draw what was visible, test the rest against the new depth,
draw those that pass), tested in the same compute passes as G3c.
Worth it only where much is hidden: the kiln's inside, Sponza's galleries,
the deck seen from the side. Checks: those, with the picture the same to
within a pixel of edge and no pop-in when the camera turns; the cost of the
pyramid against what it culls. May be cut if it saves nothing measurable.

Checks across G3: the kiln and the crowd against full detail and against
before (the plan's own check), on the G0 harness with a new label a step;
`truth` is not re-rendered (the pictures are judged against our own full
detail, as the error is ours to bound).

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
4. **meshoptimizer as a dependency** (G3): MIT, small, C++, fetched like
   cgltf, used only by the experimental 3D option (levels, clusters). The
   alternative is writing the simplifier and meshlet builder here, which is
   weeks of work to match it.
