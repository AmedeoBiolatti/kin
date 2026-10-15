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

P2 done: P2a (the core), P2b (queries, events, constraints, debug draw, ECS), P2c (the character controller), P2d (grab and
climb) and P2e (ragdolls and ropes), each with its results below.

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

#### P2e — ragdolls and ropes — results

Done 2026-10-11 on `exp/3d-rendering` (head c7afd9d plus the working tree, not committed). GENERAL ragdolls and ropes on the Jolt
module; nothing in them knows about a game.

**What was built.**

| Piece | Where | Notes |
|---|---|---|
| Ragdoll | `kin/physics3d/ragdoll3d.hpp`, `engine/src/physics3d/ragdoll3d.cpp` (`RagdollSet`, `auto_ragdoll_desc`), `World3D::create_ragdoll` ... | passive; states Animated, Ragdoll, Blend, GetUp; per-bone weights, partial subtree; events; in hash and saved state |
| Rope | `kin/physics3d/rope3d.hpp`, `engine/src/physics3d/rope3d.cpp` (`RopeSet`, the grip provider) | capsules and ball joints; length, tension, sampling, break; `GripKind3D::Rope` |
| World additions | `world3d.hpp` | `Body3DDesc::group`, `group_self_collision`, `ignore_static`, `ignore_all`, `velocity_steps`, `position_steps`; `set_motion`, `set_added_mass`, `set_group`, `set_no_collisions`, `set_constraint_anchor`, `constraint_impulse`, `Constraint3DDesc::velocity_steps`, `set_character_enabled` |
| Pose side | `kin/ecs/render3d.hpp`, `engine/src/ecs/render3d.cpp` | `PoseOverride3D` on `Animator3D`; `WorldRenderState3D::solve_pose` applies it after the clips and IK and before the joint matrices |
| ECS | `kin/ecs/physics3d.hpp`, `engine/src/ecs/physics3d.cpp` | `Ragdoll3D`, `Rope3D`, events, `hand_over_to_ragdoll`, `recover_from_ragdoll`, `collect_ropes`, interpolation |
| Harness, viewer | `kin_reference` scenes `physics_ragdoll`, `physics_rope`; Sponza Walk "Ragdoll and rope" section, `--preset ragdoll`, `--preset rope` | |
| Tests, bench | `kin-physics3d-ragdoll`, `kin-physics3d-rope` (CPU), 3 GPU tests in `kin-render3d`; `kin_physics3d_bench ragdolls`, `ropes` | |

**Decisions where the plan was open.**
1. *Ragdoll bodies are ordinary bodies and constraints made through the world's public calls*, owned by a `RagdollSet` inside the world
   so that their state (mode, blend, timers, per-bone weights, targets, the get-up snapshot) is in `state_hash()`, `save_state()` and
   `restore_state()`; a world with no ragdoll and no rope hashes as before (the block is written only when one exists). A restore
   puts the bodies' motion types and a rope's shapes and anchors back BEFORE Jolt restores the places (Jolt saves neither).
2. *Animated = kinematic bodies following the clip's pose* (so a bullet or a character can hit them; `animated_bodies` false parks
   them). The bodies are made dynamic and switched with `set_motion`, which keeps the mass; a body made kinematic has none.
3. *Get-up blends a SNAPSHOT.* The bodies go back to kinematic on the clip at once and the pose blends from where they lay to the clip.
   Blending live dynamic bodies out would have them fight the character's own capsule and the floor while it stands (found on paper:
   the capsule's sweep is blocked by the bones it overlaps).
4. *The pose is written as rotations, with the translation of the bind pose except at the top of the ragdolled part.* Bone lengths and
   scale are the bind pose's exactly; the physics joints open under a hard hit (up to 6 cm in a 2 m fall here) and a rigid pose does not
   show that (it makes the joint places differ from the bodies' by up to 8 cm down a chain at the worst moment, 0 at rest). Where: after
   the clips, layers and IK and before the joint matrices (see the section in physics_3d.md): the IK of the P1b rules is untouched
   and physics wins on the bones it owns. An animator with no ragdoll share is cleared of the override: not one byte differs.
5. *One group id for a ragdoll's (and a rope's) bodies*, `Body3DDesc::group`: same non-zero group = no collision between them unless
   both set `group_self_collision`. Queries that ignore a body ignore its group, and a character's own movement does too (its body
   takes the ragdoll's group): a ragdoll on a character does not block its own grips, foot rays or capsule.
6. *Hand-over disables the capsule and parks it at the pelvis's ground point each step* (`set_character_enabled`): the entity's
   `Transform3D` then follows the ragdoll for free, the controller's state is cleared, nothing collides with the parked body.
   Recovery checks the standing capsule at the place against everything that is not the ragdoll or the capsule itself.
7. *Ropes are capsule chains with ball joints, not soft bodies.* Jolt soft bodies were not tried: a rope of them is not bodies that
   grips, loads and events understand, and its determinism here is unmeasured. Cone or hinge joints would give bend stiffness and
   twist; they cannot be re-anchored in place (a reel would have to destroy and make constraints, which breaks "same set of
   constraints" for a restore), so a rope has no twist stiffness (a box on one rope rolls it; documented).
8. *The solver iterations are the rope's stiffness* (below): per constraint (`Constraint3DDesc::velocity_steps`, which the island
   runs the maximum of): 128 and 32 by default.
9. *Length by re-laying the chain.* A fixed segment count; a change of rest length makes every capsule and joint anchor shorter or
   longer (`set_shape` and `set_constraint_anchor`: no constraint is made or destroyed) and puts the bodies along the chain's own
   polyline at the new spacing. The anchors Jolt works out from world points differ from the exact ones in the last place, so a rope
   is made then set through the same function a restore uses (found by a restore test that differed at its first step).
10. *The climber's weight is added MASS, not a force.* P2d loads a dynamic body with a force; on a rope of 0.25 kg segments that makes a
    pendulum of the wrong period (the force accelerates a segment, not 70 kg). `set_added_mass` (for one step, like a force) puts the
    character's mass on the two segments nearest the hand; the rope then swings with the pendulum's period (5.111 s against 5.096 s
    for a 6.45 m pendulum, 0.3 %). A rope with someone on it is kept awake; a grab in the air gives it the momentum.
11. *Rope hand positions.* The anchor is a segment's body and a share of its length from its middle, so that it keeps its place when the
    length changes (pay-out and reel carry the climber). A rope offers three candidates (the closest, a hand's width up, one down),
    so that the second hand's "not on the first one's anchor" rule finds a place; the rope's own provider needs no change to the
    traversal's loop but three small things in it (the segment body changes as a hand climbs, the weight goes on as mass, a rope grip
    hangs with stiff arms and a radius further out).
12. *Tension is smoothed* (a quarter of a second): a single step's constraint impulse wanders by a tenth.
13. *Reeling in carries the load.* Shortening a rope under a load is a velocity the rigid chain cannot have: a driven velocity is
    answered by impulses of thousands of newtons (the first attempt), a teleport of the chain's end by position correction that moves
    the light segments (the second), and the load rose a third as fast as it was reeled. The body anchored at the moving end is made
    kinematic and put at the chain's end every step while the reel is on, then dynamic again with no velocity (decision 2's
    `set_motion`); a game that wants the load free through the reel pays out instead. The cost: a hoisted load does not feel
    contacts for that time.

**Numbers** (this machine, one thread unless said, a loaded desktop: indicative).

*Joints and falls* (`kin-physics3d-ragdoll` with `-v`): a standing biped (17 bodies, 16 joints, 70 kg) dropped 2 m onto a floor: the
widest a joint opened 4.4 to 5.8 cm at the impact (2 to 3 cm at rest), the deepest a capsule's skin in the floor 4 cm, settled by
3.5 s, asleep by 6 s; from 1 m settled in 2 s; the knee hinge (0 to 140 degrees) stayed inside 3 degrees of its range except in
the first 8 steps of a violent start (200 N s at the chest: -19 degrees for 3 steps). A 40 m/s throw at a 2 cm floor and wall: no
bone through. Hand-over at 4 m/s: the bodies' mean speed 3.99. The ragdoll's own bones do not push each other (a ragdoll at rest in
no gravity moved under 1 cm in 2 s); with `self_collision` two thighs pushed through each other part by 0.22 m in half a second.

*Ropes* (`kin-physics3d-rope`): a 4 m rope (16 segments) under 10, 50 and 150 kg: stretch (worst) 0.5 %, 2.8 %, 8.8 %; tension
at the top 128, 576, 1607 N against weights of 137, 530, 1511 N (rope included; readings are within about 10 %). A rope of the same
length, 80 kg, by solver steps: see physics_3d.md. Reeling in 1 m at 1 m/s, a body on the end by position correction alone (a first
version) rose 0.72, 0.53, 0.34 and 0.21 m in 1 s for 10, 20, 40 and 80 kg, 0.94, 0.78, 0.55 and 0.36 m in 5 s, and a crate on
the floor not at all; carried by the winch (kinematic while reeled in, decision 13 below) it is at the chain's end every step:
rise 1.01, 1.04, 1.09 and 1.21 m for 10, 40, 80 and 150 kg (the rest is the load's own stretch given back). A climber (70 kg) up a rope: 1.14 m in 2 s (0.6 m/s with the ramp), top of a 7 m rope
reached and stopped there, 12 and 7 units a second one hand and two, hanging tension 718 N for 70 kg + 3.6 kg of rope (mean of a
second); swing period 5.111 s against the pendulum's 5.096 s; jump off at the bottom of a swing: 2.94 m/s along the swing as the
rope had (2.93) and 3.5 m/s up; the same hands on a rope redirected gravity: the weight on it within 12 %. A rope pulled by a
carriage that moves 3 m at 1 m/s carries the climber 3.56 m (it swings on past).

*Cost*, `kin_physics3d_bench ragdolls` and `ropes` (60 Hz; "boxes" is a heap of 500 dynamic boxes in the same world; a ragdoll is
17 bodies):

| What | none | 4 workers |
|---|---|---|
| 1 / 10 / 50 ragdolls animated (kinematic, targets set each step), no boxes | 0.04 / 0.10 / 0.49 ms | 0.14 / 0.21 / 0.50 ms |
| the same beside 500 boxes (500 alone: 3.1 / 1.4 ms) | 3.18 / 3.26 / 3.75 ms | 1.44 / 1.38 / 1.82 ms |
| 1 / 10 / 50 ragdolls falling and landing (steps 0 to 120), no boxes | 0.20 / 0.98 / 5.05 ms | 0.23 / 0.69 / 1.70 ms |
| the same beside 500 boxes | 2.93 / 3.73 / 7.85 ms | 1.18 / 1.55 / 2.86 ms |
| 1 / 10 / 50 ragdolls settled (asleep, steps 240 to 360), no boxes | 0.02 / 0.05 / 0.21 ms | 0.04 / 0.08 / 0.14 ms |
| per ragdoll: animated / falling / asleep | 0.01 / 0.10 / 0.004 ms | |
| 1 / 10 / 50 ropes of 20 segments, each with a 20 kg crate, swinging, no boxes (default 128 and 32 steps) | 0.31 / 2.2 / 11.1 ms | 0.44 / 0.75 / 2.8 ms |
| the same at rest (still swinging gently: not asleep) | 0.23 / 2.2 / 11.1 ms | 0.37 / 0.65 / 2.6 ms |
| per rope by solver steps: 128 and 32 / 64 and 16 / 32 and 8 / 16 and 4 | 0.22 / 0.12 / 0.065 / 0.04 ms | |
| one character (70 kg) hanging from, and climbing, an 8 m rope of 32 segments | 0.254 ms | |

So a ragdoll costs a tenth of a millisecond while it is falling (50 of them 5 ms on the calling thread, 1.7 with 4 workers) and
nothing worth saying asleep or animated (0.01 ms); a rope is the expensive thing: the default iterations make 50 ropes of 20 segments
11 ms, which is why the number is a field: a rope that carries nothing is fine with 16 and 4 (0.04 ms), a swinging climber wants
128 and 32.

**Tests.**
- `kin-physics3d-ragdoll` (CPU, 0.7 s, 33 functions): the auto desc (17 bones from the pelvis, parents, radius limits, thigh length
  and offset, hinges exactly at the knees and elbows, 70 kg, a foot laid forward, options skip and ratio, empty skeleton); creation
  and refusals (no bones, wrong matrix count, a parent not lower, NaN, stale handle, slot reuse); animated bodies follow a
  moving clip (2 m at 1 m/s within 3 cm) and are hit (a ball stopped, the chest where it was), parked bodies are not (and start
  un-parks them); a 2 m fall (joint opening < 6 cm, nothing through the floor, settled, started then settled events, asleep
  after); freeze on settle (kinematic, asleep, still hittable, thaws on start); knee limits; no tunnelling at 40 m/s through
  2 cm floor and wall; connected bones do not collide; the self-collision flag both ways; blend in monotone to 1 and the pose
  weights are the bodies'; get up monotone to 0 and one Recovered, Blend 0.5 held, per-bone weights multiply, blend back to 0;
  get-up starts from the lying pose (the snapshot does not drift) and turned round in the middle puts the bodies where the pose shows them; partial ragdoll (3 dynamic bodies, the rest kinematic on the
  clip, weights only on the arm); a projectile starts a ragdoll with the impulse at the bone and it moves; events sorted by ragdoll
  both ways of calling; the get-up place (ground, pelvis x and z, height, a horizontal facing; nothing under it: not found; lying on
  the back and on the belly reported, faces by the head); determinism (a three-ragdoll scene with a ball: per-step hashes equal run
  to run and on 1 and 4 workers, 240 steps); save mid-fall, run (with a get-up), restore, same hashes, the hash and the saved
  state see mode and blend, a world with another set of ragdolls refuses the state; redirected gravity (against a wall at +X);
  ECS: made in entity order after the bodies (indices, user ids, destroyed with the component), the pose reaches the animator
  (bone lengths exact to 2e-5, rotations to 1e-3, the top of the ragdolled part exactly the body's), weights 0 give the clip's pose
  and matrices bit for bit and 1 the bodies' rotations, a partial ragdoll leaves every joint outside the arm's subtree
  bit for bit the clip's, IK on the other arm still reaches its target to 2 cm, hand-over (the capsule disabled, the bodies at the
  character's speed, the entity follows the pelvis to 5 cm), recovery with room (enabled at the ground point, a get-up, one
  Recovered, it walks again) and without (a ceiling: refused, Blocked, still a ragdoll; no ground: refused), determinism of a scene
  with a character, hand-over and recovery on 4 workers, interpolation of the pose between steps.
- `kin-physics3d-rope` (CPU, 2.7 s, 23 functions): creation and sampling (16 segments of 25 cm hanging straight, tangents unit and
  down, `rope_segment_of`, the closest point and its segment, t and distance, refusals, destroy); a rope let go from 45 degrees
  keeps its length and comes to rest under the anchor; tension and stretch under 10, 50 and 150 kg; an anchor on a turned moving
  body (3 cm after 4 s); a bridge of three ropes carries a 30 kg box between walls (sag 1.06 m, 1.17 loaded, the box on it) for 5 s;
  reel in and pay out (rest length at the rate, the box follows within 0.1 and 0.15, segment length, an instant length), a 60 kg crate reeled in by the winch (carried: kinematic during, at the end of the rope within 12 cm, dynamic after, a restore in the middle puts the motion type back); breaking at
  500 N (20 kg holds, 90 kg breaks, event with the rope, user id and joint); `collide_world` both ways; determinism (three ropes,
  a crate, a ball, a reel and a set length: hashes equal on 1 and 4 workers) and restore mid-reel (its length and shape come back,
  same hashes), the hash sees the length; redirected gravity; character: grab and hang (two hands, 0.2 m apart on the rope's line,
  tension the weight within 10 %, falls and the rope relaxes when it lets go), climb 1.14 m in 2 s, down, to the end and stops,
  stamina 12 and 7 a second, swing period against the pendulum (0.3 %), jump off a swing, pay-out carries the climber, a moving
  anchor carries it, redirected gravity, determinism of a scripted climber on 0, 1 and 4 workers and a restore mid-climb with
  the same hashes; ECS: made after the bodies, tension at both ends, closest point, reel from the component, breaks with an event
  (the singleton too), waits for an anchor entity's body.
- `test_gpu_ragdoll_pose_reaches_render`, `test_gpu_ragdoll_taa_matches_clip`, `test_gpu_rope_tube` (`kin-render3d`, GPU): the
  marker's centre in the picture is where the CPU's pose puts the hand joint within 0.55 and 0.88 px (two entity placements, 3 and 4
  moments of a fall); with the ragdoll's share at 0 the picture is the clip's byte for byte, and after the ragdoll is animated again
  too; 16 steps of a fall under TAA against the same poses baked as a clip: 0 of 19200 pixels differ by more than 4 levels;
  the rope: nothing drawn by default (byte for byte the empty scene), 154 pixels of its colour, the tube at the projection of all 15
  joints, and 112 pixels differ with its shadow on or off.
- `kin_reference`: `physics_ragdoll` and `physics_rope` rendered twice, byte for byte equal (default and all-on).

**Mutation checks** (each: break the source on purpose, build, run `kin-physics3d-ragdoll`, `kin-physics3d-rope`, `kin-physics3d-ecs`,
`kin-physics3d-character`, `kin-physics3d-traversal` and `kin-physics3d` (and `kin_render3d_tests test_gpu_ragdoll` for the one that
needs a picture), restore from a copy made just before; the sources were then rebuilt and every suite passed again. The rope ones were
run again after the winch carried its load; the numbers of failed checks are from the last run of each):

| Mutation | Result |
|---|---|
| connected bones collide (`collide_connected` true on the joints) | **survived the first version of the test** (the ragdoll's group alone keeps its bones apart); caught (1) once a test with `self_collision` on looked for contacts between a bone and its parent |
| hinge limits ignored | caught (3) |
| local-from-world conversion wrong (the parent's inverse left out) | caught (18) |
| blend alpha ignored | caught (19) |
| partial ragdoll affects all bones | caught (61) |
| previous pose not updated (interpolation) | caught (17) after the interpolation test compared `interpolate(0)` with the pose of the step before; its first version (a lerp between whatever was read) survived |
| pose written from the clips' matrices, not after the override (matrices before the override) | caught by `test_gpu_ragdoll_pose_reaches_render` (the marker is not where the CPU pose is; an abort); no CPU test sees the joint matrices |
| hand-over keeps the capsule | caught (3) |
| recovery clearance check removed | caught (4) |
| ragdoll not in the state hash | caught (1) |
| restore does not restore ragdolls | caught (3) |
| disabled character still updated | **survived** until a test held a move command on a disabled character and dropped a ball through its body; caught (1) |
| kinematic bodies do not follow the clip | caught (19) |
| get-up blends from the live bodies, not the snapshot | caught (34) |
| group rule ignored (a ragdoll's own bones collide) | caught (2) |
| blend time never advances | caught (44) |
| ragdoll events not sorted | caught (1) |
| rope anchor offset taken as a world point | caught (20) |
| rope stretch unbounded (solver iterations removed) | caught (29) |
| tension sign flipped | caught (15) |
| rope grip load (added mass) not applied | caught (3) |
| climb direction flipped | caught (3) |
| reel does not change the length | caught (14) |
| break threshold ignored | caught (3) |
| restore does not refit a rope's length | caught (5) |
| winch does not carry the load (the body is not put at the chain's end) | caught (3) |
| hoisting's motion type not restored | caught (3) |

27 mutations, 3 of them survived a first version of the tests and were caught by what was added (a counted failure of the tests,
not of the code); none survives now and none is known to be equivalent. The failure counts of the "caught" rows are many where
the mutated function is on every path (the ragdoll's group, the weights). The bugs the tests and the pictures found
while writing them: a ragdoll bound for a character blocked the character's own sweeps (found on paper, now a group); the rope's
probe took the normal from where the reach crossed the rope (sideways for two hands: they cancelled, and the character hung
facing away); a climbing hand stayed on its first segment's body (the grip's body was not updated on a move); a loaded rope went to
sleep with its weight on it and stayed stretched after the character let go; a restore differed at its first step (anchors Jolt
works out from world points differ in the last place from the ones a restore sets); a rope of the bridge scene with its anchors
inside the posts kicked the box off.

**Checks run.** `ninja -C build -j8` and `ninja -C build-no3d -j8`: no warnings from the new code (the build's existing ones are
Lua's `tmpnam` and the `AnimationPlayer3D` explicit-constructor notes of the tests and `kin_reference`). `build`: `KIN_REQUIRE_GPU_TESTS=1
ctest -j4 --timeout 600`: 75 of 75 (73 and `kin-physics3d-ragdoll`, `kin-physics3d-rope`), `kin-render3d` on the NVIDIA device
included. `build-no3d`: 65 of 65, and its `build.ninja` has no Jolt, physics3d, ragdoll or rope in it. `kin_render3d_tests` on NVIDIA
(`VK_ICD_FILENAMES` unset) and on lavapipe (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`): exit 0 each, the new
tests' numbers the same on both (marker error 0.55 and 0.88 px, 0 differing pixels under TAA, 154 tube pixels, 112 shadow pixels;
the physics is on the CPU). Identity: `render def` and `render all` (--cascades 4 --ao --contact-shadows --taa), 120 frames, of
crowd, shadows, sponza, helmet, motion, spheres, cornell, skinned_bend, skinned_ecs, skinned_blend, skinned_cesium, skinned_fox,
skinned_ik, skinned_ik_look, skinned_ik_limits, physics_pile, physics_character and physics_climb (the nine pictures of
the first seven not stored rendered by the head binary before the first edit): 52 pictures each mode, 104 of 104 byte for byte equal,
the static scenes' 34 also to `reference_out/p1a_base_def` and `p1a_base_all`, `physics_character` to `reference_out/p2c_physics_character`
(5 of 5) and `physics_climb` to `reference_out/p2d_physics_climb` (8 of 8). `physics_ragdoll` (8 cameras) and `physics_rope`
(6) twice, default and all-on: equal byte for byte (14 of 14 each); the default pictures are in `reference_out/p2e_physics_ragdoll`
and `p2e_physics_rope`.

**Pictures** (1280x720; looked at all fourteen). `physics_ragdoll`: `start`, five people in the T pose on a floor with an orange
ramp and a pink wall, one 4 m up, the capsules of their bones in cyan and magenta (hinge axes, joint crosses); `fall`, the dropped one
standing on the ramp (awake: green), a neighbour with its left arm folded up from a knock (the arm alone is a ragdoll), two still
animated (blue: asleep kinematic); `arm`, the knocked arm swinging down from the shoulder while the body stands; `getup_down`
(frame 300), one lying face down on the floor, one standing with an arm hanging; `getup_up` (375), the one that was lying standing
up again, tilted, between the lying pose and the clip's, the capsules following the clip; `ball`, a red ball at the chest of the
person it was fired at, the person bent over it, another lying on the ramp; `wall`, the thrown one slumped against the wall, an
upright one beside; `end`, four people lying (the ramp, the wall, the floor, the ball's) and one standing. `physics_rope`: `overview`,
the bridge with its low walls, a rope with a crate, the swing and a person under it, small; `bridge`, three ropes sagging from
the right post to a yellow box on them between the walls (the left post's anchors are hidden by the wall); `swing`, the rope out at 25
degrees from its beam with the person at its end, one hand up at the rope behind the body, the other arm out; `climb`, the person
higher on the rope at frame 400, the rope vertical, a hand at it; `winch_start`, a taut rope from the gantry to a crate on the floor;
`winch_end`, the crate hanging 3 m up on a rope of 2.2 m. Sponza Walk `--preset ragdoll --frames 270`: people lying in the nave, green
capsules awake, a red ball; `--preset rope --frames 420`: the person on the rope with a thin tube above it, in front of the curtains
(the camera is the start one, which is not a good one for either).

**Limits and not done.**
- Passive ragdolls only: no motors, no active ragdoll (balance, muscles); pose matching for a get-up (which clip, which place) is the
  game's; the facing is the pelvis's, which is a guess when the pelvis is on its side.
- The animated bodies follow the clip's pose, not the IK's. The pose is rigid (rotations; bone lengths of the bind pose), so the
  joints' opening in a hard fall (up to 6 cm here) is not shown, and a joint place differs from its body's by up to 8 cm down a
  chain at that moment. A violent start (200 N s at a chest) overshoots a hinge by 19 degrees for 3 steps.
- The auto desc is tested on one biped (the fixture's, and the person glb has the same joint names); the override table is for a
  character facing +Z in a T pose; a rig with no joint named `pelvis` needs `root_name`; capsules per bone have no belly or
  shoulders.
- A rope has no twist or bend stiffness (a box across one rope rolls it and walks off in a few seconds; a bridge needs side walls
  or boards); the stretch under a load is the solver's (table: physics_3d.md), 2.5 % at 80 kg at the defaults, a quarter at Jolt's own;
  the default iterations make a rope the dearest thing in this step (0.22 ms awake for 20 segments; a game with many ropes sets 16 and 4).
  Reeling in carries the anchored body as a kinematic one (no contacts for its own for that time).
- A rope's tension is smoothed and within about 10 % of the weight at rest; a sleeping rope shows its last reading.
- A character can hang from a rope with a ragdoll or another character on it (each adds its mass); the rope's segments are in the
  dynamic layer, so a capsule pushed against them pushes them (no special case). A hand takes the closest point of the reach: it
  has no "best hold" choice (as at a wall). Ladders and rope-as-a-mode (a rope that is climbed by climbing input only) are the
  same provider; nothing else was built.
- Soft bodies (Jolt's) were not tried; sleeping ropes under a climber are kept awake (no cost saving while someone hangs).
- Cross-platform determinism is Jolt's claim; the step code added here uses square roots and no trigonometry (the get-up
  turned round is a normalised lerp; the render side's pose writer does use `slerp`, which is not part of the physics state); only
  this machine was run.
- Not run: the Windows or MSVC build; the Sponza Walk panel's click path and the keys G, H, Q, E (no display: `--preset ragdoll`
  and `--preset rope` play the same calls on a clock, which is what a headless run shows); a networked rollback test (the saved
  state is there: a ragdoll, a rope and a grip all repeat their trajectory after a restore in the tests); a ragdoll on a skinned
  model of more than one skeleton or with scaled joints (the pose writer ignores scale: it keeps the pose's).
- The pictures' people are the fixture's tubes with a ball for a head: legible, not graceful.

### P3 — A whole scene: sky, fog, spot lights, weather

A sky as the background (a procedural gradient and sun disc first, a cubemap later), height
and distance fog that agree with it, spot lights (cone, falloff, a shadow for a few), more
than one shadow-casting light, a day-night parameter, and 3D particles (billboards, soft
particles, wind) for snow and rain; transparency ordering for them. Opt-in like the rest,
byte-identical when off. Checks: the harness scenes plus new ones (a mountain with fog, a
night scene with spot lights), cost per feature.

Split in three: **P3a** (sky, fog, day-night, the sky's image-based light: done, below), **P3b**
(spot lights, shadows of spots and point lights, a second shadow-casting directional light, the moon: done, below, after P3a's results) and **P3c**
(3D particles and weather: done, below, after P3b's results: a CPU-simulated deterministic particle system and rain, snow, dust and mist as
camera-following volumes, one instanced draw a batch, soft particles, fogged by `kin_fog.inc.glsl` and tinted by the scene's light).

#### P3a — sky, fog, time of day — results

(2026-10-12, `exp/3d-rendering`, working tree on `d42443b`; not committed.)

**What exists.**
- `kin/render3d/sky3d.hpp`: `Sky3D` (`Scene3D::sky`) and `Fog3D` (`Scene3D::fog`), plain data, off by default, and the same maths on the
  CPU (`sky_radiance`, `sky_color`, `sky_sun_light`, `sky_ground_light`, `sky_irradiance`, `fog_optical_depth`, `fog_opacity`, `fog_color`,
  `fog_apply`; `sky3d.cpp`). The tests compare the pictures with them.
- `kin/render3d/timeofday3d.hpp`: `TimeOfDay3D` {hours, day_of_year, latitude, axial_tilt, moon_phase, speed}, `evaluate()` (pure:
  sun and moon direction/elevation/azimuth, sun colour, moonlight, ambient and ground ambient, fog colour and density suggestion,
  daylight), `apply()` (into `Lights3D`, `Sky3D`, `Fog3D`), `sun_times()`, `advance()`. ECS: a `TimeOfDay3D` component on an entity,
  `WorldRenderState3D::advance_time_of_day(dt)` and `apply_time_of_day(scene)`.
- `Lights3D::sky_lights_scene`; `Render3DStats::{sky_drawn, fog_applied, sky_env_regenerated, sky_env_generations, sky_env_cpu_ms}`.
- Shaders: `kin_sky.inc.glsl` (the atmosphere), `kin_fog.inc.glsl` (fog, one include a particle pass will use), `kin_sky_pass.inc.glsl`
  with `sky.frag`, `sky_velocity.frag`, `sky_velocity_only.frag`, `sky_env.frag`; `mesh_fog.frag` and `mesh_velocity_fog.frag`
  (`#define KIN_FOG` before the one shared mesh fragment include); `view_output_dither.frag`. `kin_material.inc.glsl` gained
  `#ifdef KIN_FOG` code only.
- Harness (`kin_reference`): JSON keys `sky`, `fog`, `time_of_day`, `lights.sky_lights_scene`, a camera's `hours`, `at_sun`, `at_moon`;
  `procedural: "landscape"` (a 6 km height-field valley, 2500 cone trees, six red pillars at 50 m to 4 km); options `--sky`, `--no-sky`,
  `--fog D`, `--fog-fixed`, `--no-fog`, `--sky-light`, `--sky-light-every`, `--time H`; scenes `sky_day`, `sky_dusk`, `sky_night`,
  `fog_valley`, `tod_sweep` (default pictures in `reference_out/p3a_<scene>/`, 18 PNGs, two runs byte for byte equal).
- Sponza Walk: a "Sky and fog (P3a)" section (sky, haze, sun disc, stars, moon, sky-lights-the-scene; fog distance, height, falloff,
  base, start, cap, colour from the sky, sun lobe; time of day with hour, day, latitude, play and speed), presets `sky`, `dusk`, `fog`,
  `night`, `day-night`.

**Models and why.**
- *Sky*: a single-scattering atmosphere, Rayleigh and Mie (Henyey-Greenstein, g = 0.8) and an ozone absorber for the twilight blue, an
  exponential atmosphere seen over a flat ground, **eight midpoint steps** along the ray in the variable `w = 1 - e^(-h/H_R)` (squared, so that
  the ground's air has the finer steps), flat-earth air mass by Kasten and Young rounded off over the last 1.4 degrees of the horizon
  (so the colour has no kink where the ground begins). Chosen over a Preetham or Hosek-Wilkie fit because (a) it is closed enough to run
  in the **mesh fragment shader**: the fog's colour "from the sky" is that same function evaluated along each fragment's view ray, so far
  geometry converges to the sky by construction, not by a second set of constants; (b) it handles low and sub-horizon suns (sunsets,
  twilight, the horizon's red) which the fits do not; (c) the same function fills the environment panorama (the IBL link). Cost: about
  8 exps and pows per pixel. What it lacks: multiple scattering (a fixed x2 `brightness` stands in for it and the ground's bounce, a look
  choice: single scattering alone gives a zenith a fifth as bright as photographs), earth shadow and the Belt of Venus, clouds,
  aerial perspective inside a pixel's ray (the fog is separate).
- *Where it is drawn*: a full-screen triangle at depth 0 in the **main pass**, after the scene's opaque meshes and before its blended
  and additive ones and the overlay, with the depth test of "greater or equal" (reverse Z: it passes only where nothing was drawn, the
  early-z skipping the rest). It binds its own pipeline in that pass (the draw binder and the texture cache are forgotten after it).
  Under MSAA each pixel is shaded once and the depth test is per sample; under TAA the matrix it unprojects with is the jittered one
  (its samples are where the meshes' are) and the camera's turn is written as the velocity (in the second target of the main pass, or,
  with the pre-pass, as a velocity-only triangle in the pre-pass over its depth): without it a bright sub-pixel sun keeps 6% of its light
  in a pan (tested).
- *Sun disc*: coverage by the angle to its centre, a one-pixel linear edge (`atan(|d x s|, d . s)`, exact for small angles), the horizon's
  edge likewise, limb darkening, the radiance `sun_disc` (30) times the sun's light through the air: HDR, so bloom has it.
- *Stars*: a grid of cells over the sphere, a hash on integers (PCG3D: the same stars on every GPU; a float hash differed between
  NVIDIA and lavapipe by 2%), 1.25 pixels wide at least (more than a sixth of a cell: the neighbours are looked in), faded in as the sun
  goes from -3 to -12 degrees. *Moon*: a disc and a second light in the scattering (a glow round it).
- *Fog*: exponential distance density plus an exponential height density, **integrated in closed form** along the ray (`tau = d
  (s - start) + h e^{-k (y0 + dy start - base)} (1 - e^{-k dy L}) / (k dy)`, a series where `k dy L` is small, a finite limit to the sky),
  opacity `1 - e^-tau` capped at `max_opacity`; the colour is the fixed one or `kin_sky_scatter(view direction)` (the horizon's for rays
  that look down), plus the sun's light times a Henyey-Greenstein lobe. Applied **in `kin_output`**, before the sRGB encode, to the lit colour
  (a blended mesh mixes its colour, then its alpha is applied; an additive one is dimmed by `1 - opacity`). It reaches the sky pass
  too: the height fog's integral to infinity (no distance term), so a mist hides the horizon's low sun.
- *Variants*: `pipeline(..., fog)` picks `mesh_fog.frag` / `mesh_velocity_fog.frag` (a fragment shader with one more uniform block in
  slot 2, where custom parameters sit elsewhere, so it is pushed again after a custom draw); custom material shaders are not fogged.
  The old shaders are untouched: **70 of 70 .spv of the build before the stage are byte for byte the ones after**; the stage adds seven.
- *IBL link*: with `Lights3D::sky_lights_scene` the sky (atmosphere and ground, no disc, moon or stars) is drawn into a 256 x 128
  equirectangular panorama (`sky_env.frag`), mipped, and filtered by the same `env_filter.comp` machinery into the specular levels and the
  irradiance; `Lights3D::environment` and `ambient` are not used then. Made again only when a sky parameter has moved by more than
  `Sky3D::light_threshold` (0.004, relative to a light of 1) **and** `light_interval` (4) renders have passed since the last time; it
  is world space, so `environment_rotation` does not turn it. It works whether or not the sky is drawn.
- *Time of day*: Cooper's declination, the hour angle, solar time (no equation of time), north is -z, east +x; sunrise with 0.833
  degrees of refraction; the sun's light is `sun_intensity` times its transmittance through the same atmosphere (so the lights and the
  sky agree by construction); the moon's hour angle follows the phase and its declination swings from the sun's to its opposite; its
  light is 0.1 at full (an art scale: the real ratio is 1 in 400000) and is the fill light.
- *Dither*: a view with a sky is output through `view_output_dither.frag` (one 8-bit step, triangular, from interleaved gradient noise:
  a function of the pixel, so it holds still under TAA): a night sky is a few levels of 8 bits and came out in rings without it. It
  touches every pixel of such a view by one level (an opaque pixel under it differs by <= 1 from the same view without a sky).

**Checks run.**
- *Off is off*: default and all-on (`--cascades 4 --ao --contact-shadows --taa`) renders of crowd, shadows, sponza, helmet, motion,
  spheres, cornell: **34 of 34** pictures byte for byte those of `reference_out/p1a_base_def` and `p1a_base_all`; kiln_near
  (`--lod --lod-clusters`) 3 of 3 against `p1a_base_kiln`; the 13 skinned and physics scenes (rendered from the unmodified binary
  into the scratchpad before the first edit) default 49 of 49 and all-on 49 of 49, and `physics_character`, `physics_climb`,
  `physics_ragdoll`, `physics_rope` 27 of 27 against `p2c`, `p2d`, `p2e` stored pictures. Plus the unit test: a sky or fog "configured but
  off" is the plain picture to the byte, in 8-bit and HDR.
- `ctest` 75 of 75 (`KIN_REQUIRE_GPU_TESTS=1`; the new tests are in `kin-render3d`), `build-no3d` 65 of 65, `kin_render3d_tests` whole
  on NVIDIA and on lavapipe (2 min 20 s), re-run on the final sources after the last edit (star hash, tests, docs).
- New tests (18; the CPU ones and the GPU ones against the CPU's maths): `test_time_of_day_formulae` (equinox noon elevation = 90 - |lat|
  at five latitudes to 0.05 degrees, azimuth sweep, June solstice, sunrise and sunset against the standard formula to 0.03 h and
  to -0.833 degrees, London's 16 h 38, polar day and night, the moon opposite at full and with the sun at new, reddening, apply,
  the clock), `test_sky_model_cpu`, `test_fog_math_cpu` (the closed form against a Simpson integral of the density along
  the ray: 60 rays, relative 2e-4), `test_gpu_sky_matches_cpu` (1187 pixels, worst difference **1** level of 255, four views including
  the ground below the horizon, with haze and intensity changed), `test_gpu_sky_horizon_is_smooth`, `test_gpu_sky_sun_position` (six
  orientations and fields of view from 30 to 100 degrees: the disc's centroid off the projection by at most **0.05 px**),
  `test_gpu_sky_stays_behind_geometry` (a box at 20 m and one at 3 km, 1 and 4 samples, glass in front), `test_gpu_sky_and_fog_off_are_unchanged`,
  `test_gpu_fog_matches_formula` (8 cases x 54 pixels: the ray's length, start and cap, height fog from near its base and above
  it, the sky's colour with the sun's lobe: worst **1** level), `test_gpu_fog_blended_and_custom` (glass, additive, cut-out, custom shader),
  `test_gpu_fog_melts_into_the_sky` (the ground's last rows are the sky's colour, 160/162/143 against 161/161/140), `test_gpu_sky_under_taa`
  (still: consecutive frames differ by <= 1 level away from the disc; turning: within 2 levels of the same frame without TAA),
  `test_gpu_sky_sun_under_taa` (a sub-pixel disc, still and at 2.6 px a frame, both velocity paths: centroid off the projection by
  0.03 px still, 0.25 px worst turning; its light 0.74 to 1.08 of the same frame without TAA), `test_gpu_sky_lights_the_scene` (a white
  sphere lit only by the sky: the picture against the CPU's cosine-weighted estimate, noon 0.596/0.633/0.686 against 0.596/0.634/0.688,
  dusk and night within 3%; warmer at dusk, 3% of noon at night; the light's rotation does not turn it),
  `test_gpu_sky_environment_regeneration`, `test_gpu_sky_image_mode` (the panorama's colours, the intensity, the rotation: a test that
  found the rotation was left out of the first version), `test_gpu_sky_stars_and_moon`, `test_gpu_sky_overlay_and_warm_up`.
- NVIDIA and lavapipe: every new test passes on both. The five scenes' pictures (18) rendered on lavapipe against NVIDIA: mean
  difference 0.02 to 0.04 of 255, nothing over 27 apart from 0.003 to 0.017% of the night pictures' pixels (stars: before the integer
  hash 0.02%; the numbers above are the first run; the stored pictures are from the final build).

**Pictures looked at** (1280x720). `sky_day`: `valley` a clear blue sky paling to a hazy horizon, snow ridges and the pillars fading into
it (the far pillars pink), a green plain with cone trees casting short shadows; `sun` the disc, small and white, a broad
white-blue aureole (the Mie lobe), no ring or banding; `high` plain blue. `sky_dusk`: `sun` the sun on the ridge, the whole low sky
orange to yellow, a purple-grey zenith (the ozone), long dark shadows, trees as silhouettes with a rim of orange; `away` the
anti-solar side, orange horizon band under a grey-violet sky, the terrain black. `sky_night`: `stars` a dark navy sky with 40 or so
faint stars, no rings; `moon` the disc with a wide brownish glow (the moon is low and its light passes much air; the glow is
bigger and browner than a camera's: left), the horizon a dim amber band; `valley` a night plain, a few stars, the red pillar dark red.
`fog_valley`: `valley` a mist in the plain, trees fading, the ridges' snow dissolving into a bright warm haze; `above` from a ridge the mist
lying in the floor with the pillars poking through, a clear green slope on the left; `sun` a washed-out bright sun through the fog.
`tod_sweep` (contact sheet): 5:30 a bright white glare on the east, 7:00 pale, 12:30 flat and pale (fog from a bright sky: low contrast),
17:30 a glare on the west, 20:00 an orange sunset and black ground, 22:30 a navy night. **Ugly or weak**: noon is washed out (the fog's
colour is the whole bright sky; a lower density or `colour_from_sky = false` is the look, not a fault); the glare at 5:30 and 17:30
saturates a large area (HDR: bloom and a lower `sun_disc` or a higher turbidity tame it); the moon's glow is brown and large; the
dusk's sun is hidden by the ridge (it is below it); the night's stars are few single-pixel specks (about 40 in a 90 degree view);
no clouds, so the sky is a smooth gradient (rings are gone with the dither; a faint darker patch round the night's zenith remains: the
gradient's own).
Seams fixed on the way: the terrain's winding (the harness's, culled: the ground showed as the sky's), the horizon's band
(a hard edge of the airmass at y = 0: rounded), banding (dither), stars at low resolution (sub-pixel: sized by the pixel).

**Cost** (NVIDIA RTX 4080 Laptop, GPU ms of a frame with the view rendered 8 times in it, divided by 8, median of 300 frames; the noise is
+-0.03 ms, so these are one significant figure at best):

| What | 1280x720 | 1920x1080 | Notes |
|---|---|---|---|
| Sky pass (`sky_day`, sky only against neither) | +0.0 to +0.08 | +0.06 to +0.15 | the sky is 40 to 100% of the picture |
| Sky pass at night (stars, moon) | +0.07 | +0.11 | the star cell(s) only where the sun is under -3 degrees |
| Fog in the main pass, colour from the sky (sponza nave, roof) | +0.09, +0.12 | +0.26, +0.28 | the eight-step integral per fragment |
| the same on kiln_near (`--lod --lod-clusters`, three cameras) | +0.04 to +0.12 | +0.10 to +0.23 | |
| Fog, a fixed colour (sponza; kiln) | +0.03, +0.02; +0.01 to +0.03 | +0.015, +0.05; +0.03 to +0.05 | |
| Sky + fog + TAA's sky velocity (`sky_day`, `--taa`) | +0.04 to +0.1 | +0.2 to +0.3 | against TAA alone (one run, 100 frames) |
| The sky's panorama made and filtered (`--sky-light-every`: every render) | +0.35 to +0.4 | +0.35 to +0.6 | independent of the view's size (the 1080p spread is noise); once, then a few a second |
| `sky_lights_scene` with nothing changing | 0 | 0 | the same texture reads |

**Limits and not done.**
- Custom material shaders (`Material3D::shader`) are not fogged (their pipelines have no fog block); a game's own shader can include
  `kin_fog.inc.glsl` itself.
- No spot lights, no second shadow-casting light (done in P3b, below): at night the moon is the fill light and casts no shadow. No clouds, no 3D
  particles, no weather (P3c). Fog is not a volume: no light shafts, no shadowed in-scattering, the sun's lobe ignores occlusion.
- Orthographic cameras: the fog uses the depth along the view and one direction for the sky and the fog's colour (a flat sky colour);
  tested only for not failing (no picture test).
- The sky's panorama is 256 x 128: a sharp sun glow is blurred in the light (the sun is a light of its own), the filter's noise at
  the roughest levels is the environment filter's.
- The image sky (`SkyMode3D::Image`) shows the specular's first level (the panorama at its own size, up to 1024 wide), bilinear, no
  sun disc or stars; with no environment it draws nothing.
- Sub-pixel stars twinkle by the camera's turn at low resolution (they are 1.25 px wide: a star between two pixels is dimmer in each).
- `Sky3D::intensity` scales the sky and the panorama but not the sun's light: set `sun_intensity` and the lights together (apply() does).
- One survivor of the mutation run (below): the sky's unjittered matrix under TAA is not observable on this analytic sky.
- Not run: Windows or MSVC; a TAA scene with a *moving sun* (the camera turning is tested, the sun moving is the same arithmetic);
  the ECS clock's component in a headless run beyond the unit-level call (`advance_time_of_day`/`apply_time_of_day` are
  exercised by Sponza Walk's `tod_play` through `kin::advance`, not through flecs); Sponza Walk's panel click path.

**Mutation run** (25 deliberate breaks; each source restored from a saved copy and compared afterwards: all seven files byte for byte
equal; the test suite is `kin_render3d_tests` filtered on "time_of_day", "sky" and "fog").

| # | Break | Caught by |
|---|---|---|
| 0 | sun direction flipped in the sky pass | `test_gpu_sky_matches_cpu` |
| 1 | fog uses the depth z, not the ray's length | `test_gpu_fog_matches_formula` ("wide view") |
| 2 | height fog's sign | `test_gpu_fog_matches_formula` |
| 3 | fog colour not from the sky | `test_gpu_fog_melts_into_the_sky` |
| 4 | the sun's in-scattering ignored | `test_gpu_fog_matches_formula` |
| 5 | the sky drawn over geometry (no depth test) | `test_gpu_sky_stays_behind_geometry` |
| 6 | the sky unprojected with the unjittered matrix under TAA | **survived** (see below) |
| 7 | time-of-day azimuth (the east sign) | `test_time_of_day_formulae` |
| 8 | sunrise offset (no refraction) | `test_time_of_day_formulae` |
| 9 | the IBL regeneration threshold ignoring changes | `test_gpu_sky_lights_the_scene` |
| 10 | the horizon's ground colour ignored | `test_gpu_sky_matches_cpu` |
| 11 | the sky's intensity applied twice | `test_gpu_sky_matches_cpu` (needed a view with intensity 0.55: the first version of the test missed it) |
| 12 | the cut-out's discard skipped under fog | `test_gpu_fog_blended_and_custom` (a test added for it) |
| 13 | the star fade ignoring the sun's elevation | `test_gpu_sky_stars_and_moon` |
| 14 | the moon on the wrong side (phase not applied) | `test_time_of_day_formulae` |
| 15 | the sky's velocity zero (no reprojection under TAA) | `test_gpu_sky_sun_under_taa` (first version **survived**: the smooth sky and a 1.2 degree pan hide it; a sub-pixel disc on black does not: its light falls to 6%) |
| 16 | the fog's max opacity ignored | `test_gpu_fog_matches_formula` |
| 17 | the fog's start distance ignored | `test_gpu_fog_matches_formula` |
| 18 | the overlay fogged | `test_gpu_sky_overlay_and_warm_up` (first version of the test used a thin fog and passed it) |
| 19 | the sun's twilight fade ignored | `test_gpu_sky_horizon_is_smooth` |
| 20 | the panorama turned by `environment_rotation` | `test_gpu_sky_lights_the_scene` |
| 21 | an additive mesh fogged as an opaque one | `test_gpu_fog_blended_and_custom` |
| 22 | blended meshes not fogged | `test_gpu_fog_blended_and_custom` |
| 23 | the sky's velocity sign flipped | `test_gpu_sky_sun_under_taa` |
| 24 | the air mass without the horizon's rounding | `test_gpu_sky_matches_cpu` |

**The survivor, plainly.** Mutant 6 is not caught by anything, and I could not make a test that catches it: the sky is analytic and
smooth, its sun disc's edge is already filtered a pixel wide in the shader, so sampling the sky at the pixel's centre instead of
the jittered position changes the resolved picture by under one level and the disc's centroid by under 0.1 px (measured: the
unjittered build has the *smaller* wobble, 0.008 px against 0.03). It is an equivalent mutant for this content, kept right because
a sky texture or a hard-edged feature (a future cloud layer) would crawl against the meshes' edges with it.

#### P3b — spot lights and more shadows — results

(2026-10-12, `exp/3d-rendering`, working tree on `bb03d7e`; not committed.)

**What exists.**
- `kin/render3d/scene3d.hpp`: `SpotLight` (position, direction, colour, radius, size, `inner_angle`, `outer_angle`, `shadow`, `id`), `LightShadow3D`
  (`enabled`, `resolution`, `bias`, `normal_offset`, `near_plane`, `distance`, `softness`; also a member of `PointLight`, with an `id`),
  `DirectionalShadow3D` (the fill light's: `enabled`, `strength`, `cascades`, `resolution`, `distance`), `Lights3D::{spots, max_shadowed_spots (4),
  max_shadowed_points (2), local_shadow_resolution (1024), fill_shadow, sun_shadow_strength (1)}`, `Scene3D::add_spot_light`.
  `TimeOfDay3D::shadows` (opt-in; `apply()` then fades the sun's and the moon's strength by height). ECS `PointLight3D`, `SpotLight3D`
  (`collect()` adds them: the entity's place, a spot down its -Z, the entity's id as the light's name).
- `kin/render3d/local_lights.hpp` / `local_lights.cpp`: the CPU's side, testable without a GPU: `spot_cone`, `spot_cone_factor`,
  `spot_attenuation`, `point_attenuation`, `spot_bounding_sphere`, `spot_shadow_view`, `cube_face_view`, `cube_face_of`, `shadow_importance`,
  `assign_shadow_slots` (`ShadowSlots`, `ShadowRequest`, `shadow_hysteresis` 1.5).
- `Renderer3DCapabilities::local_shadows`; `Render3DStats::{spots, spot_slot[], point_slot[], local_shadow_maps, local_shadow_drawn,
  local_shadow_cached, local_shadow_draws, local_shadow_casters, local_shadow_triangles, moon_cascades, local_shadow_bytes, lights_variant}`.
- Shaders: four new .spv, the stock fragment shader with `#define KIN_LIGHTS`: `mesh_light.frag`, `mesh_light_fog.frag`,
  `mesh_velocity_light.frag`, `mesh_velocity_light_fog.frag` (with and without fog, with and without the TAA velocity: the same matrix as
  P3a's fog variants). `kin_material.inc.glsl` gained `#ifdef KIN_LIGHTS` code only. The depth passes use the existing shadow vertex and fragment
  shaders unchanged (a perspective matrix in the same uniform; the vertex shaders' list mode for the copies). **All 88 .spv of the build before the
  stage are byte for byte the ones after** (88 same, 0 different, 4 new). Compile cost: four more glslc runs.
- Harness (`kin_reference`): JSON `lights.spots` (`position`, `direction` or `target`, `color`, `radius`, `size`, `inner`, `outer`, `shadow`,
  `attach` for a light carried with the camera), `lights.points[].shadow`, `lights.max_shadowed_spots`, `max_shadowed_points`,
  `local_shadow_resolution`, `fill_shadow`, `sun_shadow_strength`, `time_of_day.shadows`; `procedural: "stage"` (a floor, walls or a closed
  room, pillars, poles, lamp posts, cut-out trees, benches); options `--spots N`, `--spot-shadows M`, `--point-lights N`, `--point-shadows M`,
  `--moon-shadow`, `--moon-cascades N`, `--local-res N`; scenes `spot_shadows`, `spot_night`, `spot_budget`, `moon_shadow`, `point_shadow`
  (default pictures, 13 PNGs, two runs byte for byte equal, in `reference_out/p3b_<scene>/`).
- Sponza Walk: "Spot lights and shadows (P3b)" (J drops a spot at the camera; a flashlight; a ring of spot lamps; a ring of point lamps with cube
  shadows; map size, how many get one, bias, normal offset, filter width; the moon's shadow and its cascades), stats line, presets `spots`,
  `flashlight`, `moon`, `lamps`.

**Models and why.**
- *The cone*: glTF's punctual spot, `t = clamp((cos a - cos outer) / (cos inner - cos outer), 0, 1)`, light times `t^2`, times the point
  light's inverse square and window. Chosen over a smoothstep on the cosine because it is the format's own (an imported glTF light is the same
  light), cheap (a dot and a multiply), exactly 0 at the outer angle and exactly 1 inside the inner one, and its tail is smooth (`t^2`).
- *One array for all local maps.* Spot maps are layers 0..S-1, a point light's cube six layers after them, all `local_shadow_resolution` (or the
  largest any light asks for) square; a light that asks for less draws into the corner of its layer (the viewport) and the shader scales
  the coordinates and clamps the filter into the corner. One sampler, one array, one uniform block, whatever the lights. Layers are made for
  the slots in use and only ever added to (until 120 idle renders give them back): the array's layout is part of what the caches key on.
  The fill light's cascades are an array of their own (a different side, and cascade layers).
- *Depth and bias.* Reverse Z perspective (`depth(d) = near (far/d - 1) / (far - near)`), compared with the same hardware comparison sampler
  as the sun's. The bias is a distance toward the light of `bias` texels (at the surface's distance: `2 d tan_half / resolution`) times
  `1 + slope` (the tangent of the angle to the light, capped at 12), moved into depth exactly by the formula above; the normal offset is
  `normal_offset` texels along the normal. Texel-sized units mean the same default (1 and 1.5) suits a 256 or a 2048 map, near or far.
  The slope cap was 6 at first: a sphere with the normal offset off showed 4% acne near its terminator; at 12 it is 0.15%.
- *Cube.* Six perspective faces of 90 degrees plus a guard of two texels each side (so that the 3x3 taps at a face's edge read the map and
  not the clamp); the shader picks the face by the longest axis of the vector from the light (no cube sampler: one array, one code path).
  A face is drawn only if the camera's frustum reaches the face's pyramid (corners against the camera's planes, conservative).
- *Importance and hysteresis.* `importance = intensity (brightest component) x reach (radius) x coverage^2`, coverage the bounding sphere's angular
  radius over a right angle (1 from inside it). The choice is the top `max` by importance; a light that held a slot last render counts
  1.5 times over; slots are released when the light is not chosen, and the lowest free slot is given to a newcomer. A light's name is its
  `id`, else its index. 1.5 is a stability margin, not a measured optimum: a rival 1.4 times as important does not take a slot, 2 times does.
- *The casters' path.* Not the sun's. The sun's culling writes a byte of view bits a copy (camera, sun, four cascades) and the cluster cut has
  five view slots, the GPU cut eleven frustums: another eight spots and twelve faces and the moon's cascades would have been a rewrite of the
  part that must stay byte for byte. Instead the local maps have a caster path of their own: each caster copy's bounding sphere (the posed
  bound for a skinned one) once a frame; for each view the spheres inside the light's reach and the view's frustum (a moon cascade: also the
  sun's own swept-shadow test against the camera's slice); the union of those copies written once as instances, a list of them a view
  and level, drawn with the shadow vertex shaders' list mode (the cascades' own). Cost: O(views x casters) of sphere tests on the
  CPU (a 22,000-copy crowd in four views: see the cost table). A clustered copy is cut on the CPU from the light (`select_clusters` with the
  light as the eye, the map's texels as the pixels, as the error's judge) and drawn from the compacted indices; levels of detail use the camera's
  choice (as every pass does: a shadow matches the surface).
- *The cache.* Per map, exact by construction: the bytes of everything the draw is a function of (the array's generation, the layer and size, the
  view's matrix, and for each (draw, level) the mesh's serial, material, a cut-out's texture and version, each copy's matrix and a posed copy's
  matrices, and a clustered mesh's cut settings) are built and compared with those of the render the map was drawn from. A render target as a
  cut-out's texture is never kept. A map kept costs the comparison; no snapshot, no sample (the sun's cache copies a scene's casters because the
  cascades' fit depends on the camera; a local map's inputs are the casters in the cone). Another map's draw into a shared layer (an uncached
  render) invalidates it. `KIN_SHADOW_CACHE=0` turns it off here too.
- *The moon.* `fill_shadow`: its cascades fitted as the sun's (`fit_shadow_cascades` for the fill's direction; one map is `fit_sun_box`) over
  the same `shadow_area`, from the same casters; the shader picks the cascade by view depth and blends as the sun's does. At dusk both cast:
  the sun's `shadow_cascades` (1 to 4) plus the moon's (1 to 4, 2 by default). `strength` mixes the shadow toward lit.
- *Time of day.* `apply()` with `shadows`: the moon's strength is `smoothstep(0, 8, elevation) x smoothstep(0.004, 0.02, moon_light)`, the sun's
  `smoothstep(-2, 1, elevation)`: both move at most 0.09 in 0.02 of an hour. The sun's strength below 1 is a uniform of the light shaders
  (the old shaders have no place for it), 0 draws no sun map. It is opt-in because the default pictures of P3a's sky scenes must stay as they were.
- *Fog and the rest.* Fog is applied to the lit colour in `kin_output`, so a spot-lit pixel is fogged as every other (tested to 1 level of the
  formula). Ambient occlusion dims only the ambient light as before. Contact shadows stay the sun's. TAA: the maps are not jittered, the
  receiving pixel is; the overlay is lit by the scene's spots and point lights without their shadows.

**Checks run.**
- *Off is off*: default and all-on (`--cascades 4 --ao --contact-shadows --taa`, 120 frames) renders of crowd, shadows, sponza, helmet, motion,
  spheres, cornell, the skinned and physics scenes (skinned_bend, skinned_ecs, skinned_blend, skinned_cesium, skinned_fox, skinned_ik,
  skinned_ik_look, skinned_ik_limits, physics_pile, physics_character, physics_climb, physics_ragdoll, physics_rope) and the P3a scenes (sky_day,
  sky_dusk, sky_night, fog_valley, tod_sweep): **84 of 84** pictures default and **84 of 84** all-on byte for byte those the binary before the
  stage made (rendered from a copy of it before the first edit), kiln_near (`--lod --lod-clusters`) 3 of 3 and 3 of 3 likewise; against the
  stored pictures: `p1a_base_def` 17 of 17, `p1a_base_all` 17 of 17, `p1a_base_kiln` 3 of 3, `p2c_physics_character` 5, `p2d_physics_climb` 8,
  `p2e_physics_ragdoll` 8, `p2e_physics_rope` 6, `p3a_*` 18 (all equal). Plus `test_gpu_spot_lights_off_is_unchanged`: every new setting configured
  and off, a point light that may cast but does not, a black spot, a spot out of reach: the picture is the plain one to the byte and the light
  shaders are not used.
- `ctest` (`KIN_REQUIRE_GPU_TESTS=1`): **75 of 75** (the new tests are in `kin-render3d`; 1 min 46 s), `build-no3d` **65 of 65**, `kin_render3d_tests`
  whole on NVIDIA (RTX 4080 Laptop) and on lavapipe (2 min 26 s), both exit 0. Old shaders: 88 of 88 .spv byte for byte, 4 new.
- New tests (21; the CPU ones and the GPU ones against the CPU's maths; limits are the ones in the asserts):
  `test_spot_light_maths` (inner, outer, the glTF step's middle, monotone over 0.25-degree steps, clamped and zero angles, the window, behind the
  light, the bounding sphere against 2000 points of 5 cones), `test_local_shadow_views` (the cone's edge at 512/516 of the map, near 1 and far 0,
  the texel's tangent, every one of 2000 random directions inside the face `cube_face_of` names, 0.3 degrees past 45 inside by the guard),
  `test_shadow_slots`, `test_time_of_day_shadows` (strengths 0 below, 1 above, the sun's falling through the evening, the largest step
  0.087 in 0.02 h, a partial strength while the moon is low and while the sun is at the horizon, apply() leaves the settings alone when off),
  `test_ecs_lights`, `test_gpu_spot_cone_matches_cpu` (a white wall at three distances, ten angles, three directions: the pixel against
  0.966 x `spot_attenuation` x the cosine, **worst 1 of 255**; nothing past the outer angle; the direction's sign), `test_gpu_spot_cone_edges`
  (three cones at 640 x 480: the pixel where the falloff is a half within **0.5 px** of the CPU's, black 0.3 degrees past the outer angle),
  `test_gpu_spot_radius_fade` (worst 0 of 255 against the formula, black past the radius), `test_gpu_spot_shadow_position` (a 40 cm pole's
  shadow on a floor against the segment test: centroid off **0.006 m at 512, 0.0024 m at 1024** (bound: two texels, 0.08 m and 0.04 m), area
  within 17%; a box on the floor: the shadow begins **0.05 m** from its foot (bound 0.06) and ends 0.07 m short of the projection
  (bound 0.1, the bias at a grazing 38 degrees); a light that asks for a 2048 map makes the array and the 512 light draws into its corner),
  `test_gpu_spot_shadow_casters` (a cut-out quad: its opaque half's shadow is there (96 of 294) and the clear half's is not (357 of 357);
  a skinned tube: **0 pixels** differ from the same mesh skinned on the CPU, 150 from the straight tube; five instanced copies: five shadows;
  a box behind the light and one past its radius are not drawn), `test_gpu_spot_shadow_acne` (floor overhead, at 45 degrees and
  at a grazing angle, a sphere with the normal offset alone and with the bias alone, a wall: **0, 0, 0, 0.15, 0.15, 0.04%** of the surface's lit
  pixels wrongly darkened; the limit 0.2 to 0.5%), `test_gpu_spot_shadow_distance` (632 shadow pixels to the radius, none with the distance
  at 6 m; with it at 7.5 m the light let through is 0.16, 0.74, 0.98 at 6.15, 6.8, 7.3 m), `test_gpu_spot_receives_shadow_flag` (spots and
  point lights), `test_gpu_spot_shadow_clustered_caster` (a clustered torus: the cut from the light's own view, **9228 / 7628 / 4620 / 2802 / 2310 / 1158
  triangles** at 0.05 to 3.2 px, the same as the CPU's `select_clusters` from the light and not from the camera, at eight errors, on both
  cuts of the camera's; a forced coarse cut shows; the cache keeps it and a change of the error redraws), `test_gpu_spot_shadow_cache`
  (a still map is kept and equals the redrawn one to the byte; each of a moved caster, a second caster, a moved light, the resolution, the cone's
  angle, an added caster redraws and equals the uncached picture; the camera does not), `test_gpu_spot_shadow_budget` (12 spots, 4
  maps: the four the importance rule names, the same slots in ten renders; a rival 1.2 x and 1.4 x as important does not take the map, 2 x
  does; two lights swapping 5% each render never change holder; a second renderer gives the same slots), `test_gpu_point_shadow_cube` (six casters
  and six walls: the shadow point is **0** against 726 to 732 for its mirror, on every face), `test_gpu_moon_shadow` (night: 812 pixels
  darkened, on the side away from the moon, with one and two cascades, no map of the sun's; noon: none of the moon's, the sun's there;
  strength 1, 0.4, 0 give 60, 492, 615), `test_gpu_spot_light_fogged` (worst 1 of 255 against `fog_apply` of the unfogged lit pixel),
  `test_gpu_spot_shadow_under_taa` (still: **1 level** between frames on the floor, **3** at the shadow's own edge, which is sharper than a
  pixel; 7499 and 7505 dark pixels).
- NVIDIA and lavapipe: all of them pass on both; the 13 pictures of the five scenes on lavapipe against NVIDIA: mean difference 0.03 to 0.14 of 255,
  at most 0.005% of the pixels over 8 levels, over 27 only 0.001% of the moon pictures (a few isolated pixels; not looked into further).

**Pictures looked at** (1280x720, `reference_out/p3b_<scene>/`). `spot_shadows`: a dark stage lit by one warm cone from the front and above; 'overview'
shows the pillars' shadows on the wall, the pole's long hairline across the floor, the bench's, the tree's blocky leaf holes and the man's
(skinned, walking) shadow, all where the light says and edged about three map texels wide; the cone's edge is a smooth fade, no ring, no banding;
'floor' from above shows the lit disc and each shadow on it; no acne anywhere I could find, no light leaking at the pillars' feet. The leaf texture is a 64 x 64
nearest-filtered test pattern, so the tree's own look and its shadows are blocky. `spot_night`: two warm lamp pools and a cool torch beam on a
foggy night with stars; the lamps' pools have a round shadow of the post's cap right under them (the post stops 25 cm under the bulb: physical, and
visible: a real lamp has a housing); the torch's beam shows on the floor and the bench but, with no in-scattering, not in the mist; bloom on the bulbs.
`spot_budget`: twelve coloured pools overlapping; the four with maps show the pillars' and bench's shadows, the rest light the pillars
from both sides (an unshadowed light passes through: by design); no visible pop is asserted by the test, not by the picture. `moon_shadow`:
'night' is a faint blue light with the pillars', poles', bench's and trees' shadows (and the sun's gone); 'low' (16:00) and 'dawn' (7:54) have both the
low sun's long soft shadows and the moon's, and are **over-exposed** by the night's exposure of 5 (one exposure for a scene: white sky,
saturated colours; a game would adapt it). `point_shadow`: a lamp in a closed room: shadows of the pillars, poles, bench and tree on the floor,
the walls and the ceiling in every direction, the ceiling lit brighter than the walls; no seams at the cube's edges and no leaks at the walls' corners that I
could find; a small round shadow of the lamp's own post under it. Sponza Walk (`--preset spots|flashlight|moon|lamps`, looked at headless): a pool
of lamplight on the nave's floor with the columns' shadows; the torch's disc at the nave's end; the moonlit nave dim and blue; the lamps' pools
fogged. **Ugly or weak**: the lamp post's cap shadow; an unshadowed light shining through walls (the 8 of 12 in `spot_budget`: the
budget is the cure, and a light that should not leak has to be among the most important); the over-exposed twilight pictures; blocky cut-out
shadows from the test texture; the PCF's nine taps show as faint stepping at `softness` over about 1.5; a beam in fog is missing.

**Cost** (NVIDIA RTX 4080 Laptop, GPU ms of a frame with the view rendered 8 times in it, divided by 8, median of 120 frames; noise
+-0.03 ms; maps at 1024; the moon's 2 cascades at the scene's 2048 (sponza 1 sun cascade); caches are `--shadow-cache`):

*sponza*: GPU ms of the frame (the view rendered 8 times in it, divided by 8; median of 120 frames; noise +-0.03 ms), and the change from no lights

| What | maps (MiB) | nave 1280x720 | roof 1280x720 | nave 1920x1080 | roof 1920x1080 |
|---|---|---|---|---|---|
| no lights | 0 | 0.32 | 0.28 | 0.57 | 0.49 |
| 1 spot | 0 | 0.36 (+0.04) | 0.30 (+0.02) | 0.63 (+0.06) | 0.54 (+0.05) |
| 4 spots | 0 | 0.41 (+0.10) | 0.35 (+0.07) | 0.75 (+0.18) | 0.63 (+0.14) |
| 8 spots | 0 | 0.48 (+0.16) | 0.41 (+0.14) | 0.88 (+0.31) | 0.75 (+0.26) |
| 1 spot shadowed | 4 | 0.43 (+0.12) | 0.35 (+0.07) | 0.68 (+0.11) | 0.61 (+0.11) |
| 4 spots shadowed | 16 | 0.66 (+0.34) | 0.52 (+0.24) | 1.06 (+0.49) | 0.85 (+0.36) |
| 8 spots shadowed | 32 | 0.91 (+0.60) | 0.71 (+0.44) | 1.39 (+0.82) | 1.16 (+0.67) |
| 4 spots shadowed, cached | 16 | 0.45 (+0.13) | 0.37 (+0.09) | 0.89 (+0.32) | 0.72 (+0.22) |
| 8 spots shadowed, cached | 32 | 0.51 (+0.20) | 0.46 (+0.18) | 1.06 (+0.49) | 0.96 (+0.47) |
| 1 point shadowed | 24 | 0.41 (+0.09) | 0.40 (+0.13) | 0.73 (+0.16) | 0.68 (+0.19) |
| 2 points shadowed | 48 | 0.49 (+0.18) | 0.52 (+0.24) | 0.84 (+0.27) | 0.81 (+0.32) |
| 2 points shadowed, cached | 48 | 0.35 (+0.03) | 0.29 (+0.02) | 0.73 (+0.16) | 0.59 (+0.10) |
| moon, 2 cascades | 32 | 0.56 (+0.24) | 0.47 (+0.19) | 0.88 (+0.30) | 0.75 (+0.26) |
| moon, 2 cascades, cached | 32 | 0.31 (-0.01) | 0.27 (-0.00) | 0.61 (+0.04) | 0.57 (+0.08) |

*kiln_near* (`--lod --lod-clusters`): GPU ms of the frame (the view rendered 8 times in it, divided by 8; median of 120 frames; noise +-0.03 ms), and the change from no lights

| What | maps (MiB) | dome_close 1280x720 | grazing 1280x720 | far 1280x720 | dome_close 1920x1080 | grazing 1920x1080 | far 1920x1080 |
|---|---|---|---|---|---|---|---|
| no lights | 0 | 0.34 | 0.25 | 0.27 | 0.55 | 0.36 | 0.38 |
| 1 spot | 0 | 0.36 (+0.02) | 0.26 (+0.01) | 0.27 (+0.00) | 0.60 (+0.05) | 0.38 (+0.02) | 0.42 (+0.04) |
| 4 spots | 0 | 0.40 (+0.05) | 0.29 (+0.04) | 0.32 (+0.05) | 0.67 (+0.12) | 0.44 (+0.08) | 0.49 (+0.11) |
| 8 spots | 0 | 0.44 (+0.10) | 0.33 (+0.08) | 0.34 (+0.08) | 0.79 (+0.23) | 0.48 (+0.12) | 0.52 (+0.13) |
| 1 spot shadowed | 4 | 0.38 (+0.04) | 0.29 (+0.04) | 0.34 (+0.07) | 0.62 (+0.07) | 0.40 (+0.04) | 0.52 (+0.14) |
| 4 spots shadowed | 16 | 0.42 (+0.08) | 0.31 (+0.06) | 1.41 (+1.14) | 0.69 (+0.14) | 0.45 (+0.09) | 1.56 (+1.18) |
| 8 spots shadowed | 32 | 0.47 (+0.12) | 0.35 (+0.10) | 1.58 (+1.31) | 0.79 (+0.24) | 0.52 (+0.15) | 1.75 (+1.36) |
| 4 spots shadowed, cached | 16 | 0.32 (-0.03) | 0.22 (-0.03) | 0.32 (+0.05) | 0.58 (+0.03) | 0.37 (+0.00) | 0.56 (+0.18) |
| 8 spots shadowed, cached | 32 | 0.36 (+0.02) | 0.26 (+0.01) | 0.34 (+0.07) | 0.67 (+0.12) | 0.41 (+0.05) | 0.59 (+0.21) |
| 1 point shadowed | 24 | 0.49 (+0.15) | 0.36 (+0.11) | 0.29 (+0.02) | 0.81 (+0.26) | 0.52 (+0.16) | 0.42 (+0.04) |
| 2 points shadowed | 48 | 0.58 (+0.24) | 0.43 (+0.18) | 0.31 (+0.04) | 1.03 (+0.48) | 0.68 (+0.32) | 0.44 (+0.06) |
| 2 points shadowed, cached | 48 | 0.44 (+0.10) | 0.31 (+0.06) | 0.19 (-0.08) | 0.87 (+0.32) | 0.52 (+0.16) | 0.34 (-0.04) |
| moon, 2 cascades | 32 | 1.99 (+1.65) | 1.92 (+1.67) | 1.96 (+1.69) | 2.10 (+1.54) | 1.93 (+1.57) | 1.96 (+1.57) |
| moon, 2 cascades, cached | 32 | 0.32 (-0.03) | 0.23 (-0.02) | 0.21 (-0.06) | 0.59 (+0.03) | 0.35 (-0.02) | 0.35 (-0.04) |


What it says: an **unshadowed spot costs 0.02 to 0.04 ms each at 720p and 0.03 to 0.06 at 1080p** (the light shaders' loop for every pixel; the
light shaders as such are in the first spot: Sponza 4 spots +0.10, 8 spots +0.16 ms at 720p). A **shadowed spot adds 0.04 to 0.08 ms over an
unshadowed one** at both sizes from a fresh 1024 map each frame (100 to 220 casters on Sponza), and **0.01 to 0.04** with the map kept (the
3x3 taps and a depth read): 4 shadowed spots on Sponza are +0.34 / +0.49 ms (720p / 1080p), +0.13 / +0.32 kept. A shadowed point light is +0.09 to
0.13 (720p) and +0.16 to 0.19 (1080p), two +0.18 to 0.24 and +0.27 to 0.32; kept, +0.02 to 0.03 at 720p and +0.10 to 0.16 at 1080p (six faces' taps
over a larger picture). The moon's two cascades are +0.19 to 0.30 on Sponza and **+1.6 ms on the kiln** (the full-detail kiln is the work: its cut
from a 2048 orthographic cascade is near the leaves), kept -0.02 to +0.08. A big clustered mesh in the maps of four spots is the expensive case
(kiln `far` +1.1 ms): the cut from the light finds more detail than the camera's does (9228 against 7628 triangles in the test's torus at 0.05 px)
and is drawn four times; a map kept costs none of it. Shadow memory is `side^2 x 4 bytes` a layer: 4 MiB a spot at 1024 (1 MiB at 512), 24 MiB a
point light's cube, the moon's cascades 16 MiB each at 2048; the array is only as big as the slots in use.
CPU: `render()` on the 20,000-enemy crowd: 0.2 ms without, **0.35 to 0.6 ms** with four shadowed spots in four maps (22,000 copies' spheres and
four frustum tests each on the workers; 1.8 ms before the work was spread); the cache's byte comparison adds 0.1 ms.

**Limits and not done.**
- **No in-scattering**: a lamp in mist shows no beam, and no light shafts (the P3a note stands: fog is not a volume). The optional cone in
  fog was not built. Contact shadows are still the sun's.
- Custom material shaders (`Material3D::shader`) get no spot lights, no local or moon shadows and no fog; the light shaders are for the stock ones.
- The overlay (a gun) is lit by spots and point lights, without shadows from them.
- A light that does not hold a map shines through everything: the choice is by importance, not by distance to the wall; 16 spots and 16 point
  lights (shared with the points' own limit) is the most a frame uses; 8 spot maps and 2 cubes.
- The map's resolution is per light but its memory is the largest asked for, for all layers; the array is made again (nothing kept) when the
  largest changes or more slots are first used. A face of a cube that no visible surface can fall in is not drawn (and a map's
  cache entry is kept for when it is wanted again).
- A directional light's second shadow set is the fill light only (one more is not an API: `fill` is a single light); its cascades are fitted
  to the camera as the sun's (4 + 4 maps at most at dusk).
- Levels of detail follow the camera's choice in every local map (as the sun's); clustered meshes are cut from the light on one CPU
  thread per map (1.8 ms for the four-spot kiln_near far view uncached).
- CPU cost is O(lights x casters) of sphere tests: fine for thousands, a million copies would want the sun's hierarchy of cells.
- Not run: Windows or MSVC, Metal; the Sponza Walk panel's click path (no display); a moving moon under TAA, point shadows or cut-out
  casters under TAA (the spot under TAA is tested); MSAA with local shadows beyond the harness's pictures (they are MSAA 4); a cache hit
  across two views of one renderer with different cameras (the spot's map is camera-independent by construction, the moon's is not and its
  key carries the cascades' matrices).

**Mutation run** (28 deliberate breaks, each in a saved copy of its source that is put back and compared afterwards: the four files
byte for byte equal; the tests are `kin_render3d_tests` filtered by the test's name). All 28 are caught; the first run found three survivors, which
sharpened the tests (below the table).

| # | Break | Caught by |
|---|---|---|
| 01 | spot direction's sign (the shader's `dot(-toward, axis)`) | `test_gpu_spot_cone_matches_cpu`, `test_gpu_spot_cone_edges` |
| 02 | inner and outer angles swapped (the uniform's cosine) | `test_gpu_spot_cone_matches_cpu`, `test_gpu_spot_cone_edges` |
| 03 | falloff curve linear, not squared | `test_gpu_spot_cone_matches_cpu` |
| 04 | radius fade missing in the spot loop | `test_gpu_spot_radius_fade`, `test_gpu_spot_cone_matches_cpu` |
| 05 | shadow bias's sign | `test_gpu_spot_shadow_acne` |
| 06 | normal offset ignored | `test_gpu_spot_shadow_acne` (the sphere with the bias at 0) |
| 07 | the map's matrix fov 20% too narrow | `test_local_shadow_views` (**not** by `test_gpu_spot_shadow_position`: the matrix draws and samples the map alike, so the shadow stays where it is; the texel and the edge of the cone move, which the CPU test sees) |
| 08 | the cone cull ignores the light (every caster in the radius is drawn) | `test_gpu_spot_shadow_casters` (the box behind the light) |
| 09 | importance inverted (in the renderer) | `test_gpu_spot_shadow_budget` |
| 10 | importance without its coverage (in the function) | `test_shadow_slots` (the budget's expectation uses the same function, so only the CPU test can tell) |
| 11 | hysteresis removed | `test_shadow_slots`, `test_gpu_spot_shadow_budget` |
| 12 | cache ignores that the light moved | `test_gpu_spot_shadow_cache` |
| 13 | cache ignores that a caster moved | `test_gpu_spot_shadow_cache` |
| 14 | cut-out alpha ignored in the local maps | `test_gpu_spot_shadow_casters` |
| 15 | a skinned caster drawn static (the unposed pipeline) | `test_gpu_spot_shadow_casters` |
| 16 | cube face index swapped (+x and -x) | `test_gpu_point_shadow_cube` |
| 17 | the moon's shadow ignores its elevation | `test_time_of_day_shadows` (the partial strength while it is low) |
| 18 | the sun's shadow not disabled below the horizon | `test_time_of_day_shadows`, `test_gpu_moon_shadow` |
| 19 | a shadow slot not released | `test_shadow_slots`, `test_gpu_spot_shadow_budget` |
| 20 | fog not applied to spot-lit pixels | `test_gpu_spot_light_fogged` |
| 21 | `receives_shadow` ignored by spots | `test_gpu_spot_receives_shadow_flag` |
| 22 | the corner scale of a smaller map ignored | `test_gpu_spot_shadow_position` (the 2048 light beside it) |
| 23 | the shadow's fade at its distance ignored | `test_gpu_spot_shadow_distance` (after the test was given the 7.5 m ramp) |
| 24 | the sun's shadow strength ignored in the shader | `test_gpu_spot_lights_off_is_unchanged` |
| 25 | the moon's strength ignored in the shader | `test_gpu_moon_shadow` |
| 26 | no face of a cube wanted | `test_gpu_point_shadow_cube` (first run: **survived**, the test skipped itself when no map was drawn) |
| 27 | a clustered cut judged from the camera's place | `test_gpu_spot_shadow_clustered_caster` (first run: **survived**: one error where the two cuts happen to agree; now eight errors against the CPU's cut) |
| 28 | cache ignores the cut's settings | `test_gpu_spot_shadow_clustered_caster` |

The three first-run survivors (23, 26, 27): the distance fade is only seen on a ramp between the shadow and the light (the first test counted dark
pixels); 26 showed that a test that returns when no map was drawn passes a renderer that draws none (the tests now ask
`Renderer3DCapabilities::local_shadows` and assert maps); 27 that a cut count can agree by chance at one error. No equivalent mutant was
left standing except 07 against the GPU position test alone, as said; one more that is equivalent by construction and was not run: removing the
sphere `bounded` pre-check of a view (the frustum's far plane makes it redundant).

#### P3c — particles and weather — results

(2026-10-12, `exp/3d-rendering`, working tree on `7f35305`; not committed.)

**What exists.**
- `kin/render3d/particles3d.hpp` / `particles3d.cpp` (no GPU): `ParticleSystem3D` with `ParticleDesc3D` (shapes `Point`, `Box`, `Sphere`, `Cone`, `Disc`; `rate`
  and `burst()`; lifetime, speed, extra velocity vectors, size, rotation and spin ranges; gravity, drag, `wind_influence`; size, colour and
  alpha over life, `fade_in`, `colour_variation`; a flipbook (`frames_per_life`, `frame_random`); a ground that kills or bounces), `Wind3D`
  (a direction, a speed, three gust sines, three turbulence sines a component: `velocity()`, `gust_factor()`, closed-form `displacement()`),
  `ParticleLook3D` (blend `Alpha`/`Additive`/`Premultiplied`, facing `Camera`/`Stretched`/`Axis`/`Flat`, texture or atlas or procedural disc with
  `softness` and `ring`, `soft` and `soft_distance`, `lit`, `fogged`, `sort`), `Weather3D` (four `WeatherLayer3D`: rain, snow, dust, mist; `Wind3D`;
  a ground; a clock; `apply()`, `wetness()`, `sun_disc_scale()`), the pure `weather_sprites()` / `weather_particle()` / `weather_splash()`, and the
  CPU's twins of what the shaders do: `particle_quad`, `particle_streak`, `soft_particle_fade`, `particle_disc_alpha`, `particle_light_tint`.
- `Scene3D::draw_particles(look, sprites)` and `(system)`, `Scene3D::weather`, `particle_draws()`, `particle_sprites()`; ECS `ParticleEmitter3D`
  (a system on an entity, stepped by `WorldRenderState3D::simulate_particles(dt, wind)`, drawn by `collect()`), `Weather3D` as a component
  (`advance_weather`, `apply_weather(scene, base_fog)`).
- `Renderer3DCapabilities::{particles, soft_particles}`, `Render3DStats::{particles_submitted, particles_drawn, particle_batches, particle_soft,
  weather_particles, particles_dropped, particle_cpu_ms}`.
- Shaders: three new .spv, `particle.vert` (the quad is built in the vertex shader from a storage buffer of sprites; fog is evaluated at each
  corner), `particle.frag` and `particle_soft.frag` (`#define KIN_SOFT`, both over `kin_particle_fragment.inc.glsl`). No old shader or include was
  touched: **all 81 .spv of the build before the stage are byte for byte the ones after** (81 same, 0 different, 3 new).
- Harness (`kin_reference`): JSON `weather` and `particles`, `procedural: "stage"`'s `stage.ground: [w, d]`, options `--rain`, `--snow`, `--wind`,
  `--no-weather`, `--no-particles`, `--soft 0|1`, `--particles N`; scenes `rain_street`, `snow_valley`, `particles_soft`, `particles_blend`,
  `particles_budget` (default pictures in `reference_out/p3c_<scene>/`).
- Sponza Walk: "Particles and weather (P3c)" (rain, snow, dust, mist, wind speed/direction/gusts/turbulence, dims-the-light, ground, splashes, soft
  particles and their distance, fires: in the model's middle, F drops one at the camera; each a flame, sparks and a flickering light), stats line,
  presets `rain`, `snow`, `storm`, `fire`.

**Models and why.**
- *A system is simulated, the weather is a formula.* A burst of sparks has a history (it was born at a place, with a velocity, and has been pushed
  since), so `ParticleSystem3D` is state: a fixed step of 1/120 s taken out of the time given (`update(dt)` of any size: 60 fps, 144 fps and ragged
  frames give one state, tested to the hash), semi-implicit Euler without drag and the **exact solution of constant air** with it (velocity relaxes
  to `air + gravity/drag`, so drag is stable at any step), births spread over the step (a particle is born part-way and carried the rest, so a stream
  is smooth and not clumped), and every random a PCG hash of (seed, birth number, purpose): no stream to keep in step, the same on every machine.
  Rain and snow have no history worth keeping, and a game wants thousands round the camera that must not swim when it moves, so the weather is a
  **pure function of (clock, camera)**: particle `i` is at `base_i + fall_i * t + drift(t) * influence_i + flutter_i(t)` (base a hash of `i`), wrapped
  into the box round the camera by `lo + mod(p - lo, size)` in world space. Nothing is stored or saved, a rewound clock rewinds the rain, the same
  particle is in the same world place whatever the camera did (tested: unwrapped places identical, wrapped ones differ by whole boxes, 1,000 m away the
  same number are in the volume), and the count is bounded by construction. The cost is that the weather is not interactive (no particle is
  pushed by anything, and rain does not stop at a roof) and that its drift is the wind at the world's origin (no variation with place inside a
  36 m box; the gusts' length is 60 m).
- *Wind as sums of sines.* A gust is three sines in time and place (periods 1, 0.43 and 0.19 of `gust_period`, lengths 1, 0.55 and 0.27 of `gust_length`,
  weights 0.6, 0.3, 0.1), travelling with the wind; turbulence two sines for each component. Chosen over value noise because the integral of a
  sine at a fixed place is closed, so the weather's drift needs no integration over time and no state (`displacement`, tested against a 5 ms
  trapezoid of `velocity` to 1 cm over 30 s). The statistics are those of three sines: mean 0, deviation 0.48 of the strength, range within +-1
  (tested over 80,000 samples: mean 0.00005, deviation 0.4796). The cost is a visible regularity over minutes: a game that wants a different
  rhythm sets `gust_period`; real wind is not periodic.
- *One instanced draw a batch, the quad in the vertex shader.* The CPU writes a 64-byte sprite (position and width, linear colour and alpha,
  velocity and rotation, flipbook frame) into a storage buffer (the same upload path as the mesh instances: cycled, written from the caller's
  thread into the transfer memory) and `particle.vert` makes the four corners from `gl_VertexIndex`: six vertices a sprite, no vertex buffer.
  Chosen over a quad mesh with instances because a stretched streak and an axis-locked sprite need the camera and the velocity at every
  corner, and a mesh-based draw would need a different instance transform per facing; over compute-simulated GPU particles because a
  deterministic, savable, hashable state is what a rollback game wants and 20,000 sprites cost 1.5 ms of CPU on one thread (below).
- *Order.* Within a batch: **a stable LSD radix sort on the view depth's bits, furthest first** (3 passes of 11 bits, O(n): 24,000 sprites 0.2 ms
  where `std::sort` took 1.5 ms), per batch because alpha needs it and additive does not; equal depths keep their order, so a picture is the same
  every run. Between batches: by the batch's **mean depth**, furthest first. A per-particle sort across systems would be a draw per run of one
  system's particles in between another's, with no bound; what is given up is two interpenetrating systems (a smoke column through a rain
  curtain): each is drawn whole. Blended and additive *meshes* are all drawn before any particle (particles come last in the scene's pass, before the
  overlay): a glass pane behind rain is right, a pane in front of rain is not.
- *Depth: tested, not written, and read for soft particles from the pre-pass.* A particle's pipeline tests the main pass's depth (the sample the
  mesh wrote, so MSAA clips it per sample) and writes none, so two overlapping sprites both draw (tested: an unsorted far sprite drawn after a
  near one is not rejected). **Soft particles read the pre-pass's depth** (`_ao_depth`: D32F, one sample, the view's size), by `texelFetch` at the
  fragment's pixel, in view space with the inverse projection's z row (so under TAA the jitter does not matter and an orthographic camera
  works); the main pass's own depth cannot be read in the pass that writes it, and under MSAA it is a multisampled texture. The fade is a smoothstep of the gap
  in view depth over `soft_distance`. The pre-pass is made only when a batch asks for soft particles (and the opaque meshes it holds are drawn
  a second time: its cost is the AO's pre-pass, `ao_draws`); with TAA the velocity then comes from it, as with AO. A blended mesh is not in it.
- *Fog and light.* The fog is `kin_fog.inc.glsl`'s functions called from the vertex shader at each corner and interpolated: the optical depth is
  smooth over a sprite, and the 8-step sky colour is then 4 evaluations a sprite and not one a fragment (a 20,000-particle sky-coloured fog at the
  fragment rate would be the P3a table's +0.1 to 0.3 ms a million pixels over each layer). Tested to 1 level of 255 against `fog_apply` and against a
  mesh of the same colour at the same distance. Alpha particles mix toward the fog by its opacity (the fog's colour premultiplied by the
  particle's alpha); additive ones are dimmed by `1 - opacity` (what is behind them has the fog), as meshes are. Light: **one tint a batch**
  (`particle_light_tint`: the mean of ambient and ground ambient, plus half the sun's and half the fill's colour: what a lit white ball
  shows), times `lit`: no per-pixel light, no shadows, no spot or point lights. Cheap (no uniform beyond one vec4), good enough that snow is
  not white at night (tested through `TimeOfDay3D::apply`: noon (255, 255, 255), night (55, 58, 67) for the same flake), wrong under a
  roof (the sun's tint reaches a particle in a shadow).
- *Weather's effect on the scene is a helper, not a coupling.* `Weather3D::apply(lights, fog, base_fog)` dims sun, fill and ambient by
  `wetness()` (the strongest of rain, 0.6 snow, 0.5 mist) and returns the fog as `base_fog` plus the weather's, greyer and without its sun
  glare. It takes the base fog and writes the result (so it does not compound), and must come after `TimeOfDay3D::apply` (which rewrites the
  lights it dims). The sky's sun disc is a setting, not rewritten, so the scale is a function (`sun_disc_scale()`) for the game to apply to its own value.
- *Splashes, without a physics query.* A drop's age under the ground is `(ground - y) / fall`: the same formula that hides it under the ground
  shows its splash there (a flat ring on the ground, growing and fading over `splash_time`, at the place the drop was `age` ago), so a splash
  costs a few instructions a drop that is under the ground and needs no state, and its place is world-stable. The ground is one height.

- New tests (25 in `kin-render3d`; the CPU ones need no device; limits are the ones in the asserts):
  `test_particles_deterministic_and_restorable` (10 s of a cone with wind at 60 fps, at 144 fps and at ragged 1 to 50 ms frames: one state of
  601 particles, **hashes equal**; another seed differs; a saved state restored into a system of another seed continues to the same hash after
  120 steps; truncated and mistagged bytes are refused with the state unchanged; a flipped bit changes the hash),
  `test_particles_emitter_distributions` (20,000 particles of each shape: a point's speeds fill its range and its mean is the middle to 0.05;
  a spread of 45 degrees stays inside it and its mean cosine is the solid-angle uniform one to 0.004; a box's coordinates have mean 0 and the
  variance `(2e)^2 / 12` to 3%, its shell the faces by area to 1.5%; a sphere's radius is r^3 distributed (an eighth inside half the radius to
  0.01) and its velocity is radial; a cone's base is area-uniform to 1.2% and its directions inside the angle with the solid-angle mean cosine
  to 0.003; a disc turned a quarter about x throws along +z; added velocity vectors and the cap on live particles),
  `test_particles_forces` (a parabola to 0.06 m in 1 s; terminal velocity `g/k` to 1 cm/s and the exact position over 6 s to 1 mm; wind pulls
  to its speed with drag, half the speed at half the influence, none at 0; in gusts a dragged particle's mean along-wind velocity is 4.8 to
  6.3 and it swings between under 4 and over 6), `test_wind_model` (gust mean 5e-5, deviation 0.4796, range -0.9987 to 0.9991 over
  80,000 samples; the velocity is along the direction when there is no turbulence; **the closed-form displacement equals a 5 ms trapezoid of the
  velocity within 1 cm over 30 s** and is additive; turbulence is zero-mean with the rms of half its amplitude; no direction, no wind),
  `test_particles_life_and_ground` (the curves, fade-in, colour, flipbook frame; a ground kills, or bounces at half speed to a quarter of the
  height), `test_particle_quads_cpu` (corners of each facing against their geometry; a streak's head at the particle and its width square to the
  streak and the way to the eye; least length; the soft fade and the disc), `test_weather_volume_cpu` (a pure function: equal
  twice and after a rewind to the byte; intensity 0 makes none; the first n are shown and the others hidden and the shown keep their places as
  intensity rises; **unwrapped places do not depend on the camera, wrapped ones differ by whole boxes and 89% of them are exactly where they were
  after the camera moved 2.6 m**; a camera 1,000 m away has the same number in its volume, all of them within it; drops' velocity is the fall plus
  0.8 to 1 of the wind; the pattern drifts downwind by 48.6 m in 10 s at 6 m/s; with a ground there are splashes (flat, on the ground) and no drop below it),
  `test_weather_apply` (dims, thickens from the base so it does not compound, snow 0.6, mist 0.5), `test_ecs_particles` (an emitter at its
  entity's place and turned with it, disabled neither makes nor draws, `collect()` draws, a `Weather3D` component's clock and `apply_weather`),
  `test_gpu_particles_off_is_unchanged` (weather with every layer at 0 and everything else turned, empty batches, an empty system, soft and lit
  set: the plain picture **to the byte** in 8-bit 1 sample, 8-bit MSAA 4, HDR with bloom, and TAA, with 0 batches and 0.0 ms),
  `test_gpu_particle_billboard_corners` (six sprites: turned, stretched, axis-locked, flat, a streak toward the camera, a streak whose head is out
  of the view: the lit pixels are those inside the CPU's projected corners, **0 wrong at more than 0.75 px from an edge**, areas within
  3%), `test_gpu_particle_orthographic`, `test_gpu_particle_blend_math` (alpha with a white and a half-opaque texel, additive, additive with
  texel alpha, premultiplied opaque and with alpha 0: worst **1** of 255 against the formula),
  `test_gpu_particle_sorting_and_depth_write` (near and far sprites in one batch in either order and in two batches submitted near first: back to
  front to 1 level; unsorted: the array's order, the far one drawn last and not rejected by the depth of the near one; additive in either
  order the same sum), `test_gpu_particle_depth_test` (1 and 4 samples: half behind a wall is cut at the wall's edge to the pixel, one wholly behind is
  hidden, one in front shows), `test_gpu_soft_particles` (a sprite in front of a wall that slants through it: 3,081 pixels (1 sample) and 2,847 (4
  samples) against `kin::soft_particle_fade` with the wall's own plane: **worst 1** of 255; 585 faded, 1,716 whole, 780 behind the wall; no
  pre-pass for particles that are not soft: `ao_draws` 0), `test_gpu_particle_fog_matches_meshes` (three fogs: fixed colour, sky colour with a
  lobe, height fog from above: a particle and a mesh of the same colour at the same distance both **within 1 of `fog_apply`**; `fogged = false`
  is its own colour; additive dimmed), `test_gpu_particle_light_tint` (unlit, lit and half lit against `particle_light_tint`; noon and night from
  `TimeOfDay3D::apply`: (255, 255, 255) and (55, 58, 67) for the same flake, within 1), `test_gpu_particle_flipbook` (the four frames of a 2 x 2
  atlas; mixed frames at 0.25, 0.5 and 0.75: (191, 64, 0), (127, 127, 0), (64, 191, 0)), `test_gpu_weather_places_and_no_swim` (the CPU's
  discs are lit where they project for three cameras: 33 of 33, 33 of 33, and 28 of 28 a thousand metres away, and no lit pixel is far from
  a particle; intensity 0 draws nothing: 0 batches, 0.0 ms, a picture of black), `test_gpu_rain_streaks_follow_the_wind` (30 and 43 streaks:
  all lit along the segment the CPU's head and tail project to; mean run over fall 0 and **0.603** against the wind's 0.6),
  `test_gpu_weather_cache_follows_every_change` (28 single changes of the weather, the camera and the field of view, each picture that of a
  second renderer that made the sprites afresh, 28 of 28 changing the picture; the same weather twice the same picture),
  `test_gpu_particle_budget` (24,000 sprites: 24,000 drawn in one draw, 1.5 ms of CPU in `render()`, the frame with the read-back 3 ms),
  `test_gpu_particles_taa_and_msaa` (TAA still frames differ by what the scene's own edges do, 19 levels and mean 0.09 against 19 and 0.087 without
  the particles, with the main-pass velocity and with the pre-pass's for soft ones; MSAA 4 against 1 sample: mean difference 0.29 of 255),
  `test_gpu_particles_leave_skinned_alone` (the tube's side of the picture is the same to the byte with sprites on the other).

**Checks run.**
- *Off is off*: default and all-on (`--cascades 4 --ao --contact-shadows --taa`, 120 frames) renders of crowd, shadows, sponza, helmet, motion,
  spheres, cornell, the skinned and physics scenes and the P3a and P3b scenes, made by the binary of this stage: **17 of 17** pictures byte for byte
  those of `reference_out/p1a_base_def` and **17 of 17** of `p1a_base_all`, kiln_near (`--lod --lod-clusters`) **3 of 3** of `p1a_base_kiln`; the
  8 skinned scenes (20 pictures) default **20 of 20** and all-on **20 of 20** byte for byte those of the P3b binary (kept from that stage);
  against the stored pictures `p2c` 5, `p2d` 8, `p2e` 14, `p3a` 18 and `p3b` 12 of 13: **57 of 58** equal; the one that is not is
  `p3b_spot_budget/start.png`, which the P3b binary itself no longer reproduces either (three runs of each binary: the same picture, not the
  stored one: a stale stored picture, the new binary equals the old). The final binary (after the last edits to the particle code: a cap on
  births, the weather's splash look and near fade) again: default 17 of 17 and all-on 17 of 17 (the seven scenes), kiln 3 of 3, and 28 of 28
  pictures of eight others the same as the first binary's (so the P3b binary's) and 6 of 6 all-on skinned. Plus
  `test_gpu_particles_off_is_unchanged`: every new setting configured and off is the plain picture to the byte in 8-bit, MSAA 4, HDR with bloom
  and TAA. **All 81 .spv of the build before the stage are byte for byte the ones after** (81 same, 0 different, 3 new). Two runs of the five
  new scenes (13 pictures): byte for byte equal.
- `ctest` (`KIN_REQUIRE_GPU_TESTS=1`): **75 of 75** (`kin-render3d` 115 s), `build-no3d` **65 of 65** (no Jolt, no physics3d in its build log; 6
  warnings, all lua's `tmpnam`), `kin_render3d_tests` whole on NVIDIA (RTX 4080 Laptop) and on lavapipe (`VK_ICD_FILENAMES=.../lvp_icd.json`),
  both exit 0 (each with `KIN_REQUIRE_GPU_TESTS=1`). New warnings of the build: none (the `AnimationPlayer3D` ones of the tests existed).
- NVIDIA and lavapipe: every new test passes on both, with the same numbers to a level (the budget test's frame with the read-back 3 ms on
  NVIDIA, 26 ms on lavapipe; the tint's night pixel (55, 58, 67) and (56, 58, 67); the flipbook's half mix (127, 127, 0) and (128, 128, 0)).

**Pictures looked at** (1280x720, `reference_out/p3c_<scene>/`, two runs byte for byte equal; Sponza Walk's four presets headless at 1280x800).
`rain_street`: `street`, a dusk street, the horizon orange, three warm lamp pools on the floor and the bench, rain as thin pale streaks leaning
with the wind (5 m/s on 9 m/s of fall), long and clear near the camera and fine and faint far off, fogged into the sky's colour, the lamp
bulbs bloomed; `lamp`, from under a lamp: streaks over the lit and the dark wall; `ground`, from 45 cm up: a dozen faint thin rings on the
floor (20 to 60 cm across, the nearest the clearest). `snow_valley`: `valley`, a thousand soft cream flakes (warm: the afternoon sun's tint) over
the green valley, large near the camera and points far off, the pillars hazed; `close`, flakes of several sizes (the nearest two metres
from the lens as soft discs), the sun's disc through them; `drift`, side-on to the wind. `particles_soft`: `front`, two clouds of dark smoke on a pale floor
with a pillar through each: the left (hard) one's puffs cut the floor and the pillar in straight lines, the right (soft) one fades into both;
`low`, the same from floor level; `top`, from above: the left cloud's quads show where they cut the floor (a straight edge and the pillar's
silhouette), the right's do not. `particles_blend`: `front` two streams of puffs flowing at the camera, red young and far, blue old and near,
an additive green one between: the right (sorted) is blue over red as it should be, the left (unsorted, nearest first) is red over blue in
its middle (the wrong order: orange where the nearest blue should hide red); `side`, the sorted one from the side, a clean red to blue
gradient. `particles_budget`: `front`, a fountain of about 28,000 sparks (stretched, additive, gold) over the stage with snow falling
through; they bounce and lie on the floor as a haze; `close`, the spray at 3 m, the sparks as elongated bright flecks, the snow's flakes
soft discs. Sponza Walk: `rain` from the nave, streaks and a hazed far end (**rain falls under the arcade's roof**: the weather has none),
`snow` (sparse: the volume is 30 m wide in a 60 m nave), `storm` (slanted streaks, the sun through haze), `fire` (two flames with sparks and
warm light on the columns and banners, the nave dark).
**Ugly or weak**: splashes are thin faint rings (they are small and a flat disc's ring is all there is: no droplets, no spray); rain is a
volume round the camera only (nothing beyond 18 m, so a view across a plain has no rain on the horizon: a game adds a fogged backdrop);
a flake or a drop near the lens fades out over two metres, so a drop never crosses the lens; the budget scene's bounced sparks form a bright
haze on the floor; the weather's particles are not lit by the lamps (a streak is as bright in a pool as out of it); the snow in the nave is
sparse and the rain stops nowhere; the pale test floor makes the soft-particle comparison less striking than it should be.

**Cost** (NVIDIA RTX 4080 Laptop; GPU ms of a frame with the view rendered 8 times in it, divided by 8, median of 240 frames; a quiet run,
noise about +-0.03 ms; two earlier runs, with other projects using the GPU and CPU, were 0.1 to 0.3 ms noisy and, with a weather made again by
each of the 8 repeats, CPU-bound: the sprites are now kept while nothing changes, which also makes the repeats honest):

| What (Sponza) | sprites in view (nave / roof) | nave 1280x720 | roof 1280x720 | nave 1920x1080 | roof 1920x1080 |
|---|---|---|---|---|---|
| none | 0 | 0.33 | 0.29 | 0.58 | 0.51 |
| rain 1.0 (16,000 streaks) | 3,003 / 4,226 | 0.35 (+0.02) | 0.28 (-0.01) | 0.59 (+0.01) | 0.56 (+0.05) |
| rain 1.0, soft | 3,003 / 4,226 | 0.33 (+0.00) | 0.33 (+0.04) | 0.65 (+0.07) | 0.88* |
| snow 1.0 (9,000 flakes) | 1,613 / 2,118 | 0.34 (+0.01) | 0.28 (-0.01) | 0.56 (-0.02) | 0.52 (+0.01) |
| rain 1.0 + snow 0.5, wind | 3,882 / 5,366 | 0.32 (-0.01) | 0.28 (-0.01) | 0.58 (+0.00) | 0.57 (+0.06) |
| 20,000 alpha sprites, sorted | 20,000 / 351 | 0.38 (+0.05) | 0.32 (+0.03) | 0.63 (+0.05) | 0.63 (+0.12) |
| 20,000 alpha sprites, soft | 20,000 / 351 | 0.41 (+0.08) | 0.38 (+0.09) | 0.75 (+0.17) | 0.74 (+0.23) |

(*: one frame of 30 ms in that run's p90; its p10 is 0.61, +0.10.) And the two scenes of the harness:

| Scene | sprites in view | 1280x720 | 1920x1080 |
|---|---|---|---|
| `particles_budget` without particles or weather | 0 | 0.17 | 0.35 |
| `particles_budget` `front` (28,700 sparks + 800 flakes, 2 batches) | 28,686 | 0.37 (+0.20) | 0.54 (+0.19) |
| `particles_budget` `close` | 16,596 | 0.46 (+0.29) | 0.55 (+0.20) |
| `rain_street` without weather (`street`, `lamp`, `ground`) | 0 | 0.26, 0.34, 0.37 | 0.38, 0.38, 0.40 |
| `rain_street` with rain 0.85 and splashes | 1,800 / 2,850 / 1,559 | 0.28, 0.42, 0.40 (+0.02, +0.08, +0.03) | 0.44, 0.44, 0.42 (+0.06, +0.06, +0.02) |

What it says: **a rain or snow layer is within the noise (0 to +0.1 ms)**: a few thousand thin streaks or soft discs in view are cheap
fragments. **20,000 alpha-blended, sorted sprites are +0.05 ms at 720p and +0.05 to +0.12 at 1080p; 28,700 additive streaks +0.2 ms.**
Soft particles add the pre-pass: +0.03 to +0.12 ms over the same sprites at 720p and 1080p on this scene. The CPU is the larger cost, as given below.


CPU (`render()` on the camera thread, a camera that moves each frame so nothing is kept; `--repeat 1`): Sponza nave 0.30 ms with none; rain
1.0 (+0.7: 16,000 hashed, 3,000 of them in view, split over the job system's workers), rain with snow 1.6 (+1.3), 20,000 alpha sprites (all in
view, sorted) 2.2 (+1.9). Kept from the last render while nothing changed: +0.1 to 0.3 (the cull, the sort and the upload only).
The upload is 64 bytes a sprite in view (a 28,000-sprite frame 1.8 MB); the pre-pass of a soft batch is the AO pre-pass's cost (opaque
depth and normal) and nothing else.

**Limits and not done.**
- **Not lit by spot or point lights, no shadows on particles.** The tint is the sun's, the fill's and the ambient colour: a flake in a lamp's
  pool is as dark as one beside it, and under a roof (Sponza's nave) it takes the sun. The "lights variant" is not used by particles (the
  stretch goal was not attempted).
- **Rain falls through roofs.** The weather is a volume round the camera with a ground height and nothing else: no occlusion by geometry
  (Sponza Walk's nave shows rain under the arcade). A game cuts the layer's intensity by where the player is (or draws a second, narrower layer).
- **Splashes at one ground height only** (no depth-hit splashes: the depth buffer is not read for them). Dust and mist have no splashes.
- **TAA and moving particles.** Particles are drawn into the main pass with the velocity target masked, so under TAA a moving particle takes the
  velocity of what is behind it and trails by the resolve's blend (0.9: about ten frames). Stationary ones are stable (tested: still frames
  differ by what the scene's own edges do); rain under TAA streaks into longer lines, and MSAA is what the weather scenes use. Giving particles a
  velocity needs a second, blended-compatible velocity write; not done.
- **A sort inside a batch, not across batches** (above), and not against blended meshes (particles are last).
- **CPU-made.** `render()` builds the weather's sprites (about 0.1 us each, split over the job system's workers in chunks of 2,048; kept from
  the last render while the weather, the camera and the view's frustum are unchanged) and writes every batch's sprites each frame. 16,000 rain
  particles are 2 to 2.5 ms of CPU on this machine (about a third of them in view); a game with a worse CPU budget lowers `max_particles` or the
  box. The upload is 64 bytes a visible sprite; the frame takes at most 1,048,576 (the rest are dropped, once in the log, `particles_dropped`).
- **Soft particles cost the pre-pass** (a draw of the opaque meshes' depth and normal): +0.1 to 0.3 ms on Sponza at 720p, more at 1080p (the cost
  table); a custom shader's opaque mesh is in it as any opaque mesh is. A scene whose soft batch is the weather asks for it every frame it has any.
- **No custom shaders for particles** (no `Material3D`; a texture, an atlas or a disc), no particle trails, no collisions (a ground height
  that kills or bounces is the whole of it), no emitter shapes from a mesh, no curl-noise fields, no GPU simulation.
- The overlay (`Scene3D::overlay`) gets no particles and no weather. `Scene3D::draw_particles` batches are copied each frame (a game that
  steps one system and draws it pays the copy: 52 bytes a particle, a vector append).
- A `ParticleSystem3D`'s `save()` is the state of the simulation and not its desc, transform or look: put those back yourself (the seed is
  saved). The format is versioned but not promised stable.
- Not run: Windows or MSVC, Metal; Sponza Walk's panel click path (no display); a moving camera under the weather with TAA beyond the harness's
  pictures; ECS `ParticleEmitter3D` through a headless GPU run beyond the unit-level tests (`test_ecs_particles` is CPU; the GPU tests draw what
  `collect()` hands over through `draw_particles`).

**Mutation run** (35 deliberate breaks, each in the source it names, rebuilt and run against the test(s) that should catch it, each source put back
from a saved copy afterwards: the four files byte for byte equal, sha256 before and after; the tests are `kin_render3d_tests` filtered by the name). All 35 are caught,
none survived, no equivalent mutant was found. (The suite was strengthened while it was written, so this is not a first-run score: the "streak
whose head is out of the view" case of the corners test exists because the cull's streak length was found untested; the weather's
tests were run by name filters that matched a wrong subset once, which found `test_particle_quads_cpu`'s own wrong expectation.)

| # | Break | Caught by |
|---|---|---|
| 01 | the camera-facing quad's up axis turns the other way (a shear) | `test_gpu_particle_billboard_corners` |
| 02 | a streak's head and tail swapped in the vertex shader | `test_gpu_particle_billboard_corners` |
| 03 | additive ignores the sprite's alpha | `test_gpu_particle_blend_math`, `test_gpu_particle_sorting_and_depth_write` |
| 04 | premultiplied colour not scaled by the particle's alpha | `test_gpu_particle_blend_math` |
| 05 | the soft fade ignored | `test_gpu_soft_particles` |
| 06 | the soft fade linear, not a smoothstep | `test_gpu_soft_particles` (the profile against the formula at 1 level: a linear ramp is 16 levels off at a quarter) |
| 07 | soft: the inverse projection's wrong entries uploaded | `test_gpu_soft_particles` |
| 08 | fog not applied to particles | `test_gpu_particle_fog_matches_meshes` |
| 09 | an additive particle mixed toward the fog's colour, not dimmed | `test_gpu_particle_fog_matches_meshes` |
| 10 | no depth test | `test_gpu_particle_depth_test` |
| 11 | depth written by the particles | `test_gpu_particle_sorting_and_depth_write` (the unsorted far sprite drawn after the near one is rejected) |
| 12 | no sort within a batch | `test_gpu_particle_sorting_and_depth_write` |
| 13 | sorted near to far (the radix key not flipped) | `test_gpu_particle_sorting_and_depth_write` |
| 14 | batches drawn near to far | `test_gpu_particle_sorting_and_depth_write` (the two-batch case) |
| 15 | `lit` ignored (always tinted) | `test_gpu_particle_light_tint` |
| 16 | the tint takes all of the sun, not half | `test_gpu_particle_light_tint` |
| 17 | the weather wraps about the origin, not the camera | `test_weather_volume_cpu`, `test_gpu_weather_places_and_no_swim` |
| 18 | the wind's drift runs against the wind | `test_weather_volume_cpu` |
| 19 | the weather's ground ignored | `test_weather_volume_cpu` |
| 20 | drag ignored in the simulation | `test_particles_forces` |
| 21 | the seed ignored by the random numbers | `test_particles_deterministic_and_restorable` |
| 22 | one step a call (no fixed step) | `test_particles_deterministic_and_restorable` |
| 23 | the axis facing uses the camera's up | `test_particle_quads_cpu` |
| 24 | the displacement leaves the turbulence out | `test_wind_model` |
| 25 | `restore()` forgets the births not yet whole | `test_particles_deterministic_and_restorable` |
| 26 | the cull ignores a streak's length | `test_gpu_particle_billboard_corners` (the head-out-of-view case) |
| 27 | the flipbook's rows and columns swapped | `test_gpu_particle_flipbook` |
| 28 | the encoding to sRGB left out | `test_gpu_particle_blend_math` |
| 29 | the weather leaves the ambient light alone | `test_weather_apply` |
| 30 | the weather's fog compounds (not from the base) | `test_weather_apply` |
| 31 | rain's streak ignores the wind | `test_gpu_rain_streaks_follow_the_wind`, `test_weather_volume_cpu` |
| 32 | the weather's cache key leaves the clock out | `test_gpu_weather_cache_follows_every_change` |
| 33 | ... the camera's place | `test_gpu_weather_cache_follows_every_change` |
| 34 | ... the view's matrix (the cull's frustum) | `test_gpu_weather_cache_follows_every_change` |
| 35 | ... the wind's speed | `test_gpu_weather_cache_follows_every_change` |

Not mutated, so not known to be tested: the per-corner fog's interpolation at large sprites (a 14 m mist wisp is fogged at its corners),
the overlay's skipping, `particles_dropped` at the frame's limit, the ECS `apply_weather` fog thickness, the cache key's other fields (28
single changes are tested together, but only four of their fields were broken one at a time).

### P4 — The vertical slice, "Summit" (a one-player Peak-like)

A new example (`examples/summit`): a small mountain of hand-assembled rock segments with
collision, the animated character, climbing with stamina, falling and respawn, a few status
effects, rope and food items, a weather change, a ui2 HUD, audio (footsteps, wind), all from
a seed and a scripted input run that can be checked headless. It is where the missing things
show up; each finding becomes a step. Gamepad input (aiming and moving; kin has none in
`platform`) lands here if the slice needs it, along with camera controllers and rebinding.

#### P4 — Summit — design

Written 2026-10-12 before the code, for P4a (the simulation core, by one agent); P4b (HUD, audio, environment presentation, polish)
follows by another and must be able to add to the game without touching the simulation. Where the numbers below changed while
building, "P4a - results" says so.

**Goals.** (1) A one-player vertical slice you can finish: a procedural mountain of about 150 m, a character that walks, sprints,
jumps, grabs, climbs on stamina, falls, is knocked out, gets up and respawns at camps, and a rope. (2) The netcode-ready core (5a):
the whole simulation advances only by a fixed 60 Hz step that consumes plain-data commands; its state hashes, saves and restores,
and a recorded command log replays to the same final hash. (3) Everything about the sim can be checked on the CPU in seconds,
headless, without a GPU; pictures come second.

**The simulation is a library, the example a view of it.** `examples/summit/` holds `summit_sim` (no renderer, no window: level,
`Game`, the command and input mapper, the camera rig, the scripted bot) and the executable (the scene, the view, the debug line).
`Game` owns a `World3D` (Jolt, single-threaded by default so that the run does not depend on a pool), the `SummitLevel` it was
built from, `std::vector<Player>`, a seeded RNG key, the step counter and the wind. The renderer, the camera and the sky read it
(const) and never write it; the only way in is `Game::step(std::span<const PlayerCommand>)`.

**The level.** A seed makes a `SummitLevel`: plain data (a list of `Piece`s: oriented boxes with a kind, a surface and an authored
hold or not; camps; rope anchors; the summit box; a `Route` of legs for the bot and for the climbability check). The same pieces
make the colliders (`Game` makes a static body each) and the render meshes (`build_render_meshes`), so what you see is what you
hit. The shape is a stack of cliffs and terraces rising along a wandering heading, about 150 m from base camp to the top:

| Stage | What | Rise | The skill |
|---|---|---|---|
| 0 Base camp | flat field, tents' place, checkpoint 0 | 0 | walk, sprint, jump |
| 1 Scramble | a 28 degree approach, a 4 m wall with a bench on top | ~25 m | the first grab and pull-up |
| 2 Face | a smooth slab with authored holds beside a cracked face of two pitches and a rest bench between | ~30 m | stamina: rest on the bench, or take the hold line (cheaper, slower) |
| 3 Chimney and traverse | two walls 1.4 m apart (cheaper to climb: wedged), then a shimmy along a ledge-less face | ~30 m | commit to a traverse that takes most of a bar |
| 4 Overhang | a leaning face (more drain) with no bench for 6 m, a rope-less shortcut (the leaning line) and a long way round (a ramp of loose steep scree) | ~30 m | stamina management, a route choice |
| 5 Rope | a smooth wall of 6 m no hand can hold, a fixed rope beside it, a lip on top | ~15 m | the rope, a hand over to the lip |
| 6 Plateau and summit | a wide flat rest spot (camp 5), a last ramp and a boulder field, the summit box | ~20 m | walk the last bit |

Camps (checkpoints) sit at the tops of stages 1 to 5. A route is "climbable" when the stamina model, run over its legs in
order with rests on the benches, never runs out between two rest places (checked on paper by `plan_route`, and by the scripted
run for real). Shortcuts: each stage has a straight line that skips a bench and costs more than one bar of stamina (so it needs
a rest in the middle that the face does not give: a fall), a cheaper but longer line, or a hold line.

**The player model.** A capsule (0.3 m radius, 1.95 m) of kin's character controller with grab and climb on; `Player` adds what a
game needs: health, injury, stamina caps, a state (`Alive`, `Stunned`, `GettingUp`, `Dead`), the look angles, the facing, the last
camp, the ragdoll. Numbers (all in `PlayerRules`, tested): walk 4, sprint 7 m/s (sprint 15 stamina/s), jump 5 m/s up for 6
stamina; stamina 120, comes back at 25/s after 1 s of rest, climb 0.8 m/s, two hands 7 and one hand 12 stamina/s (+4 while
moving), a grab 2, a pull-up 8, standing on ground costs nothing; fall: landing speed under 7 m/s is free, above it 7 health per
m/s, 9 m/s (a 4 m drop) knocks out for at least 1.5 s then a 0.8 s get-up; health 100, at 0 you are dead and respawn at the last
camp with health and stamina full; injury: the stamina cap is 120 x (0.5 + 0.5 x health/100), health comes back at 0.5/s while
standing still and at 10/s at a camp. Wind and cold are not in P4a (the wind exists as state for P4b's sound and snow).

**Controls (the mapper's default table, rebindable).** WASD move, mouse look, Shift sprint, Space jump (jump off a grip), left and
right mouse (or Q and E) the left and right hand's grab, F pull up (it also happens by itself when both hands are on a lip and
you press forward), X let go, R at a camp rest (Q and E also take hold). Gamepad: left stick move, right stick look, A jump,
LT/RT the hands, X pull up, B let go, Y rest, left stick click sprint. The mapper turns raw device state into a `PlayerCommand`; nothing else reads a device.

**Systems, in order of a step.** (1) commands in (a missing command is the default one); (2) per player: look, facing, rules
(rest, rope, grab edges), the controller's command and the limbs' command into the world; (3) `World3D::step` (characters, then
bodies, ropes, ragdolls); (4) per player: read events (landed, fell, exhausted, mantle), damage, stun, ragdoll hand-over, get-up
and respawn, camps, the summit; (5) the wind and the RNG; (6) the step counter. No clock, no global RNG, no render state.

**State, hash, save.** `state_hash()` = the world's hash and every `Player` field, the RNG key, the wind and the step counter,
by bytes in a fixed order (FNV-1a 64). `save()` writes the same fields and `World3D::save_state()`; `restore()` takes it back
into a `Game` made from the same seed. The scripted run's hash is stored as a constant: it is "same machine" determinism (Jolt
claims more; the look and facing use `std::sin`, `std::atan2`).

**The camera** is presentation: a rig that reads the player's state and the world's queries (a sphere cast from the head along
the arm, eased out slowly and pulled in at once, kept 0.3 m from rock), third or first person; look enters the sim as
commands, the camera follows the sim's own look angles (interpolated between steps). It never writes to the sim.

**The bot and the scripted run.** `summit --headless --script climb` (also `fall`, `rope`) builds the game, lets the route
follower (a state machine over the route's legs: walk, climb, rest for stamina, rope, summit) produce commands from the
state, records them, and checks: the summit reached within a step bound, the final hash, no non-finite number, never inside
a collider. `--screenshot DIR` renders the key moments (low resolution on lavapipe) and prints the timing.

**Not in P4a.** HUD, audio, particles at the player, a stamina bar, the sky's time of day as a mechanic, weather effects on
grip, items (food, rope placing), more than one player on screen, a network, a save file of the game, the death screen. They are
P4b's or later; the sim's events (`GameEvent`: landed, fell, camp, summit, stun, get-up, death, step) are how they will hear of
things.

**Acceptance.** (a) Same seed, same level bytes; another seed another level, both climbable by the planner. (b) Command replay,
save/restore, frame-rate independence and step purity give equal hashes. (c) The mapper: dead zones, rebinding, a keyboard and
a gamepad press give the same command. (d) The stamina numbers above, measured. (e) A fall of 5 m knocks out, ragdolls, gets up
and the character walks; a fall of 40 m kills and respawns at the camp. (f) The rope climbs. (g) The camera is never inside
rock along the scripted run. (h) The scripted run reaches the summit under 40 000 steps. (i) 14 or more mutation checks caught.
(j) Nothing existing changes: no .spv, no picture, `build-no3d` 65 of 65.

#### P4a — results

Done 2026-10-12 on `exp/3d-rendering` (head e2b170d plus the working tree, not committed). The simulation core of Summit
(`examples/summit`, about 5000 lines); P4b (HUD, sound, presentation, polish) builds on `SummitView` and the `GameEvent` list without
touching `Game`.

**What was built.**

| Piece | Where | Notes |
|---|---|---|
| Level | `summit/level.*` | `generate_level(seed)`: 55 to 70 boxes (`Piece`), 7 camps, a rope, a 33-leg route; `bytes()`/`hash()`; `plan_route` (stamina planner); `build_render_meshes` (faces cut into cells, noise-coloured: from the same pieces) |
| Game | `summit/game.*` | `Game::step(commands)`, `state_hash`, `save`, `restore`, events, `std::vector<Player>`, seeded RNG key, wind, `FrameStepper`, two test hooks (`debug_teleport`, `debug_set_health`) |
| Mapper | `summit/input_map.*` | `RawInput` (plain data), `InputBindings` (rebinding table, text round trip, dead zone, sensitivities, invert), `map_input` |
| Camera | `summit/camera.*` | `CameraRig`: third person spring arm (sphere cast, pulled in at once, eased out), first person, the view swings round while hanging |
| Bot, logs | `summit/script.*` | route follower state machine, command log files, `script_config` (climb, fall, rope) |
| View | `summit/view.*`, `main.cpp` | meshes, person (ECS animator, IK hands and tail, ragdoll pose override), rope, camps, 7 looks, debug line, scripted shots |
| Engine | `kin/platform/gamepad.hpp`, `src/platform/gamepad.cpp`; `physics3d/character3d_impl.hpp`, `world3d.cpp` | two gaps, below |

**Decisions where the design was open.**
1. *The simulation owns a raw `World3D`, not the ECS bridge.* The ECS physics state is a view of entities; a `Game` that must hash,
   save and replay wants the world, its characters, ragdolls and rope handles and nothing else. The presentation uses an ECS world
   only for the skinned person (animator, IK, pose override), driven from the const state.
2. *The ragdoll is parked (`animated_bodies = false`) and started from the T pose at the capsule's place*: no clip is evaluated in the
   simulation; a get-up blends to the rest pose. Stun and death share one path (hand-over, capsule disabled, the capsule follows the
   pelvis's ground point, a get-up place and clearance check, respawn after a timeout when there is no room).
3. *Stamina is the engine's `Resource3D`*; the rules' numbers are written into `TraversalDesc3D`, the jump cost and the injury cap
   (`set_character_desc` live) are the game's. Standing on the ground with a hand on the wall costs nothing (the engine's rest rule), so
   the planner's climb starts from the first hold at 2 m.
4. *Level = a staircase of boxes.* Pitches are boxes whose top is the next bench: the engine's pull-up cannot go over a protruding slab
   and a flat face is what its hand projection follows, so rock "features" are benches, a roof slab (the overhang), a chimney (two
   walls, drain x0.6), a smooth wall with a lip and a rope on a beam, and a smooth slab with authored holds. Shortcuts are the lines
   that skip a bench (they need more than a bar and the planner says so).
5. *Auto pull-up*: both hands on a ledge grip and forward pressed alternate the engine's edge each step (`pull_up_sent`).
6. *Seeds are mixed* (splitmix of the seed): the first version's stream was the same stream shifted for seeds 8 apart (found by seeds 1 and
   9 having one height).
7. *Input is sampled once a step* from `Input::mouse_motion()` (the update-time reading) and held keys; the look delta is part of the
   command, the camera interpolates the sim's own yaw.

**Gaps found in the engine.**
- *No gamepad in `kin::platform`* (plan P4 predicted it). Added `kin/platform/gamepad.hpp` and `src/platform/gamepad.cpp` (a new
  source in the always-built platform list): `GamepadState` (plain data), `Gamepads::poll()` on SDL's gamepad API, started only when a
  `Gamepads` is made. `Input` and the app loop are untouched, so nothing changes when it is not used: `build-no3d` 65 of 65 and its
  `build.ninja` has no Jolt, physics3d or summit. Not exercised with a device here (none): the mapper is tested with made-up states, and
  `Gamepads` without a device is checked to report nothing (`test_gamepad_poll_without_a_device`).
- *A character's own movement collided with bodies set to collide with nothing* (`set_no_collisions`, `ignore_all`: a parked ragdoll's
  bodies, a disabled character), because `OwnBodyFilter` knew groups only. With two players 1.2 m apart the first one's T-pose arms held
  the second's capsule (it could not move). Fix: `CharacterEnv::collides_nothing` (the rule flag already in the world) checked in
  `OwnBodyFilter::ShouldCollide`. Behaviour changes only for a body with that flag set against a character's sweeps; the 5 physics3d
  test programs (character, traversal, ragdoll, rope, ecs) and `kin-physics3d` pass unchanged (79 of 79 in the whole suite), and the
  reference pictures are byte-equal (below). Mutation: removing the check is caught by `test_two_players`.

**Numbers** (this machine; `kin_summit_tests -v`).

| Behaviour | Measured |
|---|---|
| Hang, two hands, still / moving up / one hand | 7.00 / 11.00 / 12.00 stamina a second |
| Climb speed | 0.80 m/s along the face |
| Jump | costs 6.0 (150 to 144.0); sprint 7.0 m/s at 15.0 a second; regeneration 25 a second after 1 s |
| Injury cap | health 40: 105.25 (150 x (0.5 + 0.5 x 0.4) with 0.25 of health regenerated), health 0: 75.25 |
| Fall of 2 m (6.3 m/s) / 5 m (9.97 m/s) | no damage, no stun / health 100 - (v - 7) x 7 = 79, knocked out, lies at least 90 steps, gets up in 48 |
| Fall of 40 m onto a camp / below the kill plane | dies (`Died`), back at the camp in 2 s with health 100 and stamina 150 / `Died` and respawn |
| Level | 146 to 158 m of altitude over seeds 1 to 60; seed 1: 155.5 m |
| Scripted climb, seed 1 | summit at step 9769 (162.8 s of game time), no falls; 60 seeds: all reach it, none fall; lowest stamina 31.4 (seed 10: 6.0) |
| Planner against the run | seeds 1 to 4: the planner's least margin and the run's lowest stamina agree to about 10 units (the planner is the lower) |
| Wall time of the run (CPU) | 1.0 s for 9769 steps: 100 microseconds a step including the bot (23 800 steps a second in the last run, a quiet machine) |
| Scripted run with pictures, lavapipe, 480x270, 1 sample | 2.6 s wall, 7 pictures; NVIDIA 640x360 `fall`: 2.8 s, 8 pictures |
| Hash, seed 1 climb | `c80d269809d52fb0` at step 9769 ("same machine" determinism) |

**Tests.** `kin-summit` (CPU, 12 s, 23 functions): level determinism and altitude over 20 seeds, colliders and meshes are one list (a body
for each piece at its place, holds are holds, a ray down onto a ground piece and a mesh vertex on that face), the planner (30 seeds
climbable, a worked leg to 0.01, an infeasible mountain says so, the run's lowest stamina within the planner's margin), the stamina
numbers above, look clamp and non-finite commands, the mapper (a key and a stick give the same command, jump, hands, sprint, release,
triggers at the threshold, dead zone values, unit length, mouse and stick look, invert, rebinding, text round trip, refusing a bad
name, a disconnected pad), the gamepad poll, command log files (round trip, cut file refused), replay (same hash; another seed and a
changed command differ), save and restore (mid-hang into a fresh game, back into the same game, refusing another seed, player count,
cut and damaged bytes), the state covering the RNG, wind, health and step, frame-rate independence (30, 60, 144, 240 fps and jitter,
camera updated every frame: one hash), step purity (pool against one thread with another game stepped in between, a missing command),
two players, falls (free, stun, ragdoll, get-up, walk on, death, respawn at camp 3, kill plane), the rope (200 steps, 4 m up it, the
leg done), the camera (never inside rock along the run, shortened by the chimney, eased out within the rate, first person, writes
nothing), the scripted climb (summit, steps, hash, no non-finite number, never inside a collider, 6 other seeds), the scripted fall,
authored holds in reach and climbed hand over hand by a test's own state machine up to a pull-up over the lip, and the cost of a step.
ctest: `kin-summit`, `summit-script-climb` and `-fall` (CPU, expected hash and steps) and `summit-script-shots` (GPU, 320x180,
skips without a device): 79 of 79 in `build` with `KIN_REQUIRE_GPU_TESTS=1` (75 and the four), 65 of 65 in `build-no3d`.

**Mutation checks** (break a source, build, run `kin_summit_tests`, restore from a copy and `cmp`; 31 mutants, 31 caught, all sources
restored identical). The first run had one survivor that is an equivalent mutant: fall damage "from any landing speed" (`speed > 0`
for `speed > safe_landing`) gives negative damage under 7 m/s, which the camp's health cap undoes the next step; it was replaced by the
formula mutant below, which is caught. After the bot's last change the three bot and damage mutants were run again (caught).

| Mutation | Result |
|---|---|
| damage not reduced by the safe speed | caught (1) |
| stun threshold ignored | caught (12) |
| injury does not lower the stamina cap | caught (3) |
| jump costs no stamina | caught (1) |
| RNG key not in the state bytes | caught (3) |
| restore skips the world | caught (5) |
| respawn keeps the health | caught (6) |
| respawn always at camp 0 | caught (2) |
| camp never recorded | caught (5) |
| hand-over keeps the capsule | caught (1) |
| never gets up | caught (7) |
| kill plane ignored | caught (3) |
| look pitch not clamped | caught (2) |
| no automatic pull-up | caught (29) |
| summit box of no size | caught (15) |
| hidden state in step (a call counter) | caught (5) |
| first pitch three times too high | caught (115) |
| holds too far apart sideways | caught (11) |
| rope's pole where the hands go | caught (8) |
| planner forgets the pull-up | caught (1) |
| dead zone ignored | caught (2) |
| invert_y ignored | caught (1) |
| trigger threshold ignored | caught (1) |
| right key goes left | caught (1) |
| keys at 45 degrees faster | caught (1) |
| camera does not collide | caught (5) |
| camera arm snaps out | caught (1) |
| bot never rests | caught (6) |
| bot's handover aimed at the rope | caught (23) |
| frame stepper drops the remainder | caught (3) |
| characters collide with parked bodies (engine) | caught (1) |

Bugs the tests and runs found: a character's capsule held by another player's parked ragdoll (engine, above); the landing slab of a
stage floated over the previous bench and blocked the route (found by the first run: 0.8 m short of the top of a ramp); the rope's
pole stood in the pull-up's clearance (NoRoom) and the beam in the head's; the rope handover had to aim left of the rope or the probe
found the rope again; a grip on the lip's face is not a ledge (retry with another aim); the frame stepper test hung under a mutant (now
bounded); the planner was 30 units pessimistic (the hands take hold 2 m up) and later 10 (a settle time); a held `release` was
overridden by the bot's re-grab during a scripted fall; presentation clips advanced once a shot (T-pose arms in the first pictures).

**Checks run.** `ninja -C build -j4` and `build-no3d`: no new warnings. `build`: `KIN_REQUIRE_GPU_TESTS=1 ctest -j2 --timeout 600`
79 of 79; `build-no3d`: 65 of 65, no Jolt, physics3d or summit in `build.ninja`. `kin_render3d_tests` on lavapipe
(`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`): exit 0; also inside ctest on NVIDIA. No `.glsl` was touched, so the .spv
are the HEAD's. Identity: `render def` and `render all` (`--cascades 4 --ao --contact-shadows --taa`), 120 frames, crowd, shadows,
sponza, helmet, motion, spheres, cornell: every picture byte for byte equal to `reference_out/p1a_base_def` and `p1a_base_all` (only
`results.json`, the timings, differs). The scripted run on lavapipe and on NVIDIA: the same summit step and hash (the simulation is on
the CPU: the GPU only draws); windowed `summit --screenshot` on NVIDIA 1280x720 drew the base camp in the `dusk` look.

**Pictures looked at** (320x180 to 1280x720). Base camp: the person standing arms down, the mountain's first ramp behind, a camp flag at
the side; first wall and second pitch: the person hanging, arms up to the face, from behind and to the left (the view swings round
while it hangs), the face a mottled grey-brown slab; chimney: arms bent up, close; overhang: the person under the roof's brown
underside with one arm up the face; rope: the person on the rope with its end curled on the ground, the smooth blue-grey wall behind;
summit: the pole and red flag over the flat top, the person with its arms down; `fall`'s ragdoll shot: the camera sits at the pelvis
and shows only a pair of legs (ugly). Dusk in the window: warm and soft, a hazy ground. **Ugly parts, plainly:** the mountain is boxes
(stacked slabs with noise-coloured faces): flat walls, no silhouette to speak of, the same grey-brown everywhere; the person is the
fixture's tubes (arms bent at odd elbows when hanging, a stiff pose standing, a tail hung by IK); the ragdoll lies at the pelvis with
the camera inside the chimney; the ground shows faint banding from the cell triangulation; fog makes far slabs a pale blue grey.

**Limits and not done.**
- Not tested with a real controller or by hand: the keyboard, mouse capture and the gamepad path through `Gamepads` (no device; the
  window opened and drew on NVIDIA, no key was pressed); the pictures' `--first-person` and the looks `night`, `snow`, `storm` were not
  looked at (only the default, `dusk`).
- The route is one line of staircase and the bot knows only it; the hold line is climbed by a test's own machine, not the bot;
  "shortcuts with risk" are the lines the planner calls infeasible from a full bar (no second route, no ice, no loose rock, no
  wind or cold effect on grip). Overhang is a roof slab, not a leaning face.
- Cross-platform determinism is not claimed: the look and facing use `std::sin`, `std::atan2` and `Quatf::axis_angle`; the stored hash
  is for this machine and this compiler. Only one machine was run.
- The person has no idle, hang or tumble clips of its own (an `Idle` made in the view; hanging is the `Fall` pose with IK hands); a
  get-up blends from the lying pose to the T pose standing.
- `GameEvent` carries no entity or item data yet; there are no items (food, ropes to place), no HUD and no sound (P4b).
- The ragdoll's camera while lying is the pelvis (see the pictures); a death has no screen.
- Two test hooks (`debug_teleport`, `debug_set_health`) change the state outside commands: for tests only.
- Memory and disk: `/mnt/windows` was down to 393 MB at the end (this build's objects and another project's); pictures were kept in the
  scratchpad and deleted.

#### Blockfield — editable voxel workload

Added 2026-10-14 in `examples/blockfield/`, following the request for a Minecraft-style game that is easier to scale than Summit.
This separate creative sandbox has seeded hills, trees and underground passages, walking/jumping and flight, six placeable block
types, mining, a hotbar and optional saved edits. Geometry and textures are procedural and original. Survival, crafting, mobs,
water simulation and multiplayer are outside this first slice; the Astroneer track below remains a separate smooth-terrain plan.

Columns are 16×64×16 cells, greedily meshed with neighboring halos. Only sparse changes are stored persistently (16,384 cells
maximum); one worker builds one mesh at a time. Distance bounds resident columns and an independent mesh budget bounds their
vertex/index data. Near-player edits take priority over far meshes; rejected chunks are remembered to avoid rebuilding every
frame. Only the nearest 3×3 columns have Jolt colliders, made from the same mesh. Walking waits for these colliders and for pending
near edits. This is a resident geometry budget, not a total process/driver memory cap.

The benchmark renders every frame: distance-dependent warm-up, flight across chunk boundaries and back, repeated edits and
eviction. Reports distinguish CPU update, scene construction, render submission and delayed GPU device timings; optional CSV
records every frame. Headless update time includes waiting for the single worker, while windowed play polls it. Timing summaries
exclude warm-up and store at most 6,000 samples per series.

Sequential Vulkan smoke measurements on the RTX 4080 laptop GPU, at 1280×720, seed 1, 64 MiB mesh budget:

| Distance | Frames | Resident columns at end | Resident mesh MiB | GPU p95 ms | Peak process RSS MiB |
|---|---:|---:|---:|---:|---:|
| 3 | 900 | 49 | 3.4 | 0.49 | 226 |
| 8 | 1,200 | 278 / 289 | 19.1 | 0.59 | 226 |
| 16 | 2,400 | 909 / 1,089 | 64.0 (budget reached) | 0.76 | 211 |

These are single runs with different durations and camera endpoints, not a controlled estimate of scaling or the engine ceiling.
At distance 16 the budget was respected and near collision stayed ready; this hardware still has substantial GPU headroom.
There are no measurements yet on a weaker GPU, D3D12 or Metal. RAM readings are process RSS and exclude GPU memory.
An additional 1,000-frame distance-8 run with a 1 MiB mesh budget ended with 13 resident columns and collision ready, while
reporting budget backpressure. Every recorded frame in the distance-8/16 CSVs stayed within its chunk and resident mesh bounds.

Validation: `kin-blockfield` covers greedy merging, winding, hidden faces/seams, negative coordinates, picking, capped edits,
save/restore and an actual Jolt ray hit disappearing after a mined block's collider is rebuilt. `examples/blockfield/smoke.py`
drives jumping/landing, mining, placement, overlap rejection, selection, save/reload and chunk eviction through the scene server.
Screenshots were inspected. Builds use `--parallel 1`, GPU runs are sequential, and the examples README has launch/test commands.
No engine API changes are needed for this sample.

##### Blockfield wider-view sweep — 2026-10-14

Camera FOV is now configurable (vertical 45–120 degrees, `--fov`, PgUp/PgDn); the playable default is 90 degrees with distance 8.
Distance accepts up to 32 columns (512 metres in each horizontal direction, subject to the mesh cap). Automatic benchmarks
choose warm-up plus at least 900 frames, so larger views get the same measured flight interval instead of spending the entire
run loading. The one-worker and 256 MiB maximum mesh budget remain. No engine code changed.

Five sequential runs at 1280×720, seed 1, fixed trajectory and 900 measured frames after each distance's warm-up, on the RTX 4080
laptop GPU / Vulkan. Distance 16 used a 128 MiB cap; distances 24/32 used 256 MiB. Counts and triangles are at the final frame;
GPU and CPU columns are p95 over measured frames. Process RSS excludes GPU allocations.

| Distance | Vertical FOV | Resident columns / limit | Mesh MiB | Drawn triangles | GPU p95 ms | CPU submission p95 ms | Peak RSS MiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| 16 | 75 | 1,071 / 1,089 | 76.1 | 358,520 | 0.637 | 0.367 | 230 |
| 16 | 90 | 1,071 / 1,089 | 76.1 | 402,612 | 0.632 | 0.441 | 230 |
| 16 | 110 | 1,071 / 1,089 | 76.1 | 443,440 | 0.629 | 0.442 | 213 |
| 24 | 110 | 2,353 / 2,401 | 170.6 | 937,006 | 0.813 | 1.167 | 215 |
| 32 | 110 | 3,504 / 4,225 | 256.0 | 1,275,436 | 1.107 | 1.982 | 220 |

Wider camera angles had little GPU impact in these smoke runs; increased distance raised both GPU time and CPU submission cost.
Distance 32 reached the chosen mesh cap, so that row represents a budget-limited world rather than the complete requested square.
Resident chunks can also lag the requested count while streaming at the moving boundary. Rendering still has headroom; these
single runs do not identify the engine's ceiling or promise this throughput on weaker GPUs. Fog and fixed 48-metre shadows remain
the same across the sweep; this measures more terrain rather than farther shadow coverage or new effects.

Reports, per-frame CSVs, screenshots, RSS records and summary CSV/JSON were written to `/tmp/blockfield-view-sweep/`.
Every recorded frame respected the chunk and mesh bounds, every run retained ready near-player collision, and GPU timing samples
were nonzero (810–899 per run). A monitor stopped runs if process RSS exceeded 768 MiB or starting available RAM fell below
1.5 GiB; neither threshold was reached. `kin-blockfield` and the input-driven smoke passed, including the new FOV and distance
controls; the distance-32 screenshot was inspected. Build parallelism was 1.

##### Blockfield CPU profiling — 2026-10-14

Profiled the distance-32 / vertical-FOV-110 workload at 1280×720 with the 256 MiB mesh cap: 4,285 warm-up frames followed by
6,000 measured frames. `perf record` sampled user-space cycles at 199 Hz with bounded 4 KiB call stacks; the engine's existing
`--profile` supplied runtime timings and the sample's CSV supplied streaming/preparation/submission timings. No gameplay or
renderer code changed for this investigation. Peak process RSS was 282.9 MiB including profiling overhead; GPU memory is excluded.
Every recorded frame respected the chunk and mesh bounds, near collision remained ready, and perf reported zero lost samples.

| Measured operation | Mean ms | p95 ms |
|---|---:|---:|
| CPU frame (including headless job waits) | 3.034 | 3.448 |
| World update | 0.489 | 0.711 |
| Scene preparation | 0.178 | 0.194 |
| 3D render submission | 2.264 | 2.489 |
| Present | 0.058 | 0.069 |
| GPU whole frame | 1.227 | 1.390 |

These scopes overlap: scene preparation and render submission are inside `render.scene`, and GPU work runs alongside CPU work.
They must not be added to estimate total latency. Profile overhead and the longer moving trajectory also prevent direct speed
comparisons with the earlier 900-frame sweep. GPU wait p95 was under 0.001 ms; presenting and waiting are small here. GPU stages
were not separately timed, so this does not identify a shader or shadow-pass bottleneck.

**First target: shared geometry buffers.** The main-thread P-core sampled cycles in the middle 50–75% of the recording show
37.37% in `VULKAN_BindVertexBuffers` and `VULKAN_BindIndexBuffer`, after normalising the filtered thread's sampled periods.
Source inspection explains the cost: SDL's Vulkan `TRACK_RESOURCE` linearly searches the command buffer's tracked resources on
each new buffer bind. The sample gives every chunk separate vertex/index buffers, so the number of comparisons grows roughly
quadratically with unique buffers. `perf annotate` shows about 96% of the index-binding function's P-core sampled cycles in that
scan loop, providing evidence beyond the function name. The low-sample E-core event was not used for this estimate.

The proposed engine change is a small set of vertex/index buffer pages with reusable per-chunk ranges, keeping chunk bounds,
draws and independent edits. It should reduce unique resources and tracking/binding work without keeping another full terrain
copy in RAM. It needs bounded allocation, range reuse and GPU-fence-safe retirement; simply merging the entire world would make
editing and streaming expensive. The improvement has not been implemented or measured yet.

**Second target: mesh generation.** In the same steady window across P-core threads, height/terrain functions and sine/cosine
account for roughly 21% of sampled cycles, while greedy meshing and its `std::function` cell callback account for roughly 12%.
`World::generated` recalculates column height, cave parameters and tree inputs per cell in the halo. Calculate these once per
column/nearby tree during a job, using a small job-local cache, and mesh directly from the halo span to avoid the callback dispatch.
This targets loading/edit response and worker utilisation; it does not promise the same reduction in displayed frame time.

**Third target: incremental streaming work.** The streamer scans the resident map and walks requested rings each update. It also
clears all budget-blocked keys on each edit, while any global world revision change discards an in-flight result, even if the edit
is elsewhere. Prefer bounded work queues refreshed on camera chunk changes and affected edits, with chunk-specific revisions and
budget generation stamps. Reuse the scene draw list until residency or geometry changes; scene construction alone averaged
0.178 ms. Map traversal/lookup appears in the CPU samples. Add job queued/rejected/stale counters before attributing further cost
to repeated budget-rejected meshes; current counters measure accepted meshes only.

The run's startup took 10.786 seconds of CPU-frame wall time and the measured interval 18.202 seconds. The selected middle window
therefore excludes terrain startup and profile-file export. Raw reports, CSV, samples, disassembly and compact findings are in
`/tmp/blockfield-profile/`; `findings.json` and `findings.txt` are small summaries. The 79 MiB runtime JSON contains all frame
events; keep profile runs explicitly bounded on this machine. Reproduce with one process at a time:

```sh
KIN_RENDER_BACKEND=gpu perf record -F 199 -e cycles:u -o /tmp/blockfield.perf.data -- \
  ./build/bin/blockfield --benchmark --headless --distance 32 --fov 110 --mesh-budget-mib 256 \
  --frames 10285 --width 1280 --height 720 --profile --profile-json /tmp/blockfield-profile.json \
  --report /tmp/blockfield-report.json --metrics /tmp/blockfield.csv
DEBUGINFOD_URLS= perf report -i /tmp/blockfield.perf.data --stdio --no-children -g none --percent-limit 1
```

The reproduction omits call stacks to reduce recording size; leaf symbols and instruction annotation remain available. Filter
runtime JSON/CSV by the reported warm-up frame count, and use a perf time window inside steady execution rather than comparing
its whole-run summary with steady frame metrics. Optimisations should be validated with identical hashes/counts and screenshots,
same-path sequential before/after runs, and both RAM and GPU resource lifetimes checked.

##### Shared mesh buffers — implemented and remeasured, 2026-10-14

The first CPU profile target is now implemented in Kin (`gpu_mesh_pool.*`, `gpu_renderer3d.*` and the 2D backend's submission
hook). Small non-skinned, non-clustered meshes use 1 MiB vertex pages and 256 KiB index pages, binding byte offsets with local
indices. Each mesh retains its bounds, levels, serial and independent draw. Eligibility is at most 512 KiB of vertices / 128 KiB
of indices; larger, skinned or clustered meshes retain their existing path. Small static LOD chains can share pages too.

First-fit ranges coalesce when freed. Retired allocations cannot be reused until an independent GPU fence covering their
submitted draws/uploads signals. A normal frame submission seals pending retirements; bursts over 16 MiB cause submission/wait
backpressure. Empty pages are released. The pool reserves at most 512 MiB, with dedicated-buffer fallback if it cannot allocate;
this is a pool cap, not a total GPU memory cap. Page metadata is mutex-protected and meshes retain the pool/device handle when
they outlive the renderer. No second full CPU copy of terrain is kept.

An initial implementation sealed retirements before the draw and started the device's fence-based frame timer early, including
later CPU preparation in its tail. The final hook seals after normal submission without changing that timer's start. The frame
timer is a submission-to-fence observation, including queue/scheduling effects, rather than isolated GPU timestamp scopes.

Three alternating original/shared pairs used the **same binary**, distance 32, vertical FOV 110, 1280×720, seed 1, 256 MiB resident
mesh budget, 4,285 warm-up and 900 measured frames. `KIN_SHARED_MESH_BUFFERS=0/1` selected allocation at mesh creation. Values below
are medians of the three runs' percentiles; GPU/CPU scopes overlap.

| Measurement | Dedicated buffers | Shared pages | Reduction |
|---|---:|---:|---:|
| CPU submission p50 ms | 1.900 | 0.999 | 47% |
| CPU submission p95 ms | 2.057 | 1.069 | 48% |
| GPU frame timer p95 ms | 1.107 | 0.961 | 13% |
| Tracked CPU stage sum p95 ms | 2.602 | 2.416 | 7% |
| Median peak process RSS MiB | 219.9 | 217.6 | — |

The tracked sum is update + scene preparation + 3D submission per CSV row, excluding HUD/presentation; it is not total frame
latency. Faster rendering exposes more headless waiting for the single mesher: update p95 rose from 0.491 to 1.182 ms. Windowed
play polls this worker instead. This is therefore a renderer improvement, not a claim of 48% higher game frame rate.

All six runs matched world hash `2586239188960919588`, 3,504 resident columns, 1,396 last-frame draws and 1,275,436 last-frame
triangles. Pixels matched exactly outside the timing HUD row (x 12–661, y 74–109 excluded); the independent GPU test compares
complete reference/shared pictures with different geometries and offsets. Every CSV frame stayed inside chunk/resident mesh
bounds, and near-player collision stayed ready. Pool reservation was 259 MiB (370 pages), versus about 256 MiB of live geometry.
At 10,285 frames it still reserved 259 MiB, with 570 range reuses and zero retired bytes at the final render.

A second 199 Hz cycle sample, without stacks to keep its recording small, reduced buffer binding from 37.37% to 2.18% of the
filtered main-thread P-core sample periods in a steady middle window. These recordings differ in instrumentation overhead and
are hotspot evidence, not an exact isolated time comparison. Culling/upload, scene collection and map lookups are now more visible.
The sampler wrote 0.29 MiB for 5,904 samples; reports and alternating-run summaries are in `/tmp/blockfield-shared/`.

Validation: full `kin_render3d_tests` on NVIDIA Vulkan passed (peak RSS 240.5 MiB); dedicated/shared image identity, multiple pages,
fragmentation/reuse, old recorded draws surviving replacement, complete empty-page release, and meshes outliving the device are
covered. The shared-page test also passed on lavapipe. Blockfield CPU and input-driven jump/edit/save/stream/view-control tests
passed. Builds were sequential with `--parallel 1`. The new 2D submission hook is guarded by `KIN_EXPERIMENTAL_3D`; a syntax-only
compile with that define removed checks the disabled path (no existing full 3D-off build directory is available). Measurements
and GPU checks here are Vulkan; D3D12/Metal have not been run.

Next useful work is job-local terrain column caching and incremental chunk scheduling, followed by examining the remaining
Kin culling/upload overhead. Keep the buffer toggle for same-binary before/after checks and preserve fence/offset tests.

##### Wider FOV scaling after shared buffers — 2026-10-14

Extended Blockfield's optional vertical FOV limit from 120 to 150 degrees (CLI and PgUp), retaining the 90-degree default.
The wider range is for stress testing; perspective distortion is pronounced above 120. Input smoke checks the 150-degree clamp
and returns to 90 before its gameplay checks. No Kin renderer code changed for this sweep.

At distance 32 / 1280×720 / seed 1 / 256 MiB resident mesh cap / shared buffers enabled, each angle ran three times in differing
orders. Every run had 4,285 warm-up and 900 measured frames. Triangle/draw counts are averages over that interval, identical
across repeats; timing values are medians of each run's percentiles.

| Vertical FOV | Mean drawn triangles | Mean draws | CPU submission p95 ms | GPU frame timer p95 ms |
|---|---:|---:|---:|---:|
| 90 | 1,119,143 | 1,225 | 1.285 | 1.216 |
| 110 | 1,271,375 | 1,397 | 1.482 | 1.121 |
| 120 | 1,334,277 | 1,468 | 1.215 | 1.003 |
| 135 | 1,420,002 | 1,563 | 1.332 | 1.095 |
| 150 | 1,494,771 | 1,647 | 1.303 | 1.038 |

The machine had background parallel compilation during part of the first traversal: CPU p95 varied substantially and GPU clocks
also varied. These timings therefore show approximate cost, not a precise monotonic slope or evidence that a wider FOV is faster.
The geometry result is stable: 110→150 adds 17.6% drawn triangles and 17.9% draws, with GPU p95 roughly 1.0–1.2 ms. Fixed resolution
keeps pixel count constant, and widening the angle shrinks projected geometry while revealing more of the resident world.

FOV did not change residency: all 15 runs had 3,504 columns, world hash `2586239188960919588`, about 256 MiB of resident geometry
and 259 MiB reserved in shared GPU pages. Peak process RSS stayed at or below 220.2 MiB. Every CSV frame respected the resident
chunk/mesh bounds, and each final report had ready near-player collision and nonzero GPU samples. The requested distance-32
square is budget-limited; this does not test all 4,225 requested columns at full residency.

Three additional 150-degree runs at distance 24 (2,461 warm-up + 900 measured frames) used 2,353 columns without reaching the cap:

| Distance at FOV 150 | Mean drawn triangles | Mesh MiB | Shared page MiB | CPU submission p95 ms | GPU frame timer p95 ms |
|---|---:|---:|---:|---:|---:|
| 24 | 1,083,199 | 170.6 | 176.3 | 1.079 | 0.913 |
| 32 | 1,494,771 | 256.0 | 259.0 | 1.303 | 1.038 |

That distance increase adds 38.0% drawn triangles and 50.0% resident geometry in this budget-limited comparison. Requested
horizontal column counts grow as `(2*distance+1)^2`: 2,401 at 24 versus 4,225 at 32. Actual geometry varies with terrain and
backpressure. Farther distance remains the stronger memory growth factor; increasing angle alone does not load extra chunks.

Artifacts are in `/tmp/blockfield-fov-scale/` (all runs, median summary JSON/CSV and final screenshots at each angle). GPU runs
were sequential with a 768 MiB process RSS / 1.5 GiB starting available-RAM guard on the FOV sweep. The 150-degree screenshot
was inspected; input-driven jump/mining/placement/save/streaming/FOV checks passed and `git diff --check` was clean. The results
remain specific to this fixed-resolution voxel workload on the RTX 4080 laptop GPU / Vulkan and do not identify the engine ceiling.

##### Unlit shader specialization — implemented and remeasured, 2026-10-14

Kin now selects colour-only or base-textured shaders for unlit and wireframe main-pass draws, with separate masked fragments.
The vertex shader omits normals, world-position/picking outputs and (without a texture) UVs. The fragment shader binds no
samplers for colour-only materials and just the base sampler for textured ones. Shared colour functions and uniform declarations
preserve the existing ABI and colour math. Generic shaders handle fog, skinning, TAA/velocity and custom materials. Other passes
keep their existing shaders. Warm-up prepares the selected pipeline; `unlit_draws` and `unlit_textured_draws` report its use.
Vertex/instance formats remain 36/128 bytes; CPU/shader compiler flags were not changed. Disable with `KIN_UNLIT_SHADERS=0`.

Three alternating repeats used one Kin binary with that override off/on and the Godot 4.7.2 Mobile release reference.
The same snapshot, 3,504 draws, disjoint tiles, FOV 90 and 1280×720 were used; no AA, fog, shadows, streaming or physics.
Each run warmed up 300 frames, then measured 180 at 20× or 120 at 100×. Times below are medians of run p50 / p95 frame intervals,
in milliseconds, including the render loop rather than isolated GPU timestamps.

| Workload | General Kin | Specialized Kin | Godot Mobile |
|---|---:|---:|---:|
| 20× / 63.91 million | 10.99 / 11.25 | 7.97 / 8.19 | 8.13 / 8.69 |
| 100× / 319.56 million | 58.99 / 60.62 | 40.40 / 41.36 | 41.28 / 42.01 |

Median paired p50 reductions were 27.5% at 20× and 29.8% at 100×. Individual 100× pairs ranged from 26.4% to 31.5%.
GPU clocks were not locked: the longer sweep's timings rose across repeats, and a late observation was 87 C / 1455 MHz graphics.
This supports an approximately 30% improvement and roughly Mobile-level performance for this scene; the small apparent lead
is not an engine ranking. The cached Godot package's report still prints its input FOV default (110), although its stress camera
uses 90; its terrain masks match Kin's exactly at both scales.

Both Kin captures match RGBA exactly. Every measured row submitted the requested 63,911,000 / 319,555,000 triangles;
reports confirm 70,080 / 350,400 instances and 3,504 specialized draws only with the override on. Shared mesh reservation is
270,270,464 bytes in both paths; no mesh data is duplicated per tile. Peak render process RSS was 422.3 MiB for general Kin,
418.5 MiB for specialized Kin and 618.3 MiB for Mobile, excluding GPU memory. Runs stayed sequential, with a 1,536 MiB process
RSS limit, 2 GiB starting available-RAM requirement and 1 GiB running guard; artifacts are on Linux disk.

Validation: full NVIDIA Vulkan `kin_render3d_tests` passed (199.6 MiB peak RSS). The specialization equivalence test passed
on NVIDIA and lavapipe, including 60 colour/texture/blend/mask/wireframe/mixed-pipeline cases across MSAA, gamma/HDR/linear
outputs, varying vertex alpha, emission and tint, plus fog/TAA fallbacks. Full-suite skinning, custom materials, depth, shadows,
picking and warm-up tests passed. Source and measurements are in `$HOME/kin-bench/unlit-specialization/`, with `findings.json`,
raw CSV/reports, images, bounded process metadata and the reproduction script `run.py`.

##### Lighting, materials and sampling — implemented and measured, 2026-10-15

Blockfield's comparison now has configurable lit/PBR materials, four shared procedural 64×64 maps (albedo, ORM, normal,
emissive: 64 KiB), three metallic/roughness families, a 128×64 linear RGB environment (96 KiB before filtering), point/spot lights,
sun and local shadows, HDR, MSAA, TAA, AO/contact shadows and a 2× render-resolution sampling case. UVs are scaled to repeat
across eight blocks in the comparison's PBR modes. `--camera detail` provides a fixed close-up; strict full-field counts apply to
the default top-down view. The default unlit path and snapshot format remain compatible. No render-engine or compiler-flag
change was needed; this exercises existing Kin features through new workloads.

The playable game adds `--quality pbr|high`. PBR uses dielectric terrain maps, four points / one spot, HDR and MSAA 4×.
High uses eight points / two spots, requests shadows from one point and two spots, TAA, half-resolution AO and 16-step contact
shadows. Its nearby lights follow the player at gentler intensities than the wide-field stress rig. Existing sky/daylight/fog,
streaming, mesh budgets, collision and edits remain active. `basic` is the existing default. Reports expose actual feature work.

The GPU sweep completed 90 sequential runs: three alternating repeats of 30 supported engine/workload groups, with 300 warm-up
and 180 measured frames each. Every measured main pass submitted exactly 3,195,550 triangles and 3,504 instances/draws from
one shared snapshot at FOV 90. Ordinary cases render 1280×720; the 2× resolution case renders 2560×1440 and downsamples to
1280×720. Geometry residency and sharing are fixed. Values are medians of run p50 / p95 frame intervals in milliseconds:

| Workload | Kin | Godot Mobile | Godot Forward+ |
|---|---:|---:|---:|
| Unlit HDR | 1.32 / 1.42 | 1.35 / 1.74 | 2.37 / 2.94 |
| Lit HDR | 1.30 / 1.40 | 1.32 / 1.64 | 2.41 / 2.93 |
| PBR maps + IBL, flat normals | 1.45 / 1.52 | 1.84 / 2.31 | 3.07 / 3.99 |
| PBR + normal detail | 1.45 / 1.53 | 1.82 / 2.50 | 3.15 / 4.20 |
| + 8 points / 2 spots | 1.52 / 1.56 | 4.36 / 5.56 | 6.04 / 7.11 |
| + sun shadow | 2.31 / 2.42 | 6.34 / 8.15 | 7.91 / 9.64 |
| + local shadows | 6.49 / 6.78 | 6.55 / 8.38 | 7.94 / 9.61 |
| Local shadows + MSAA 4× | 6.12 / 6.47 | 6.53 / 8.17 | 7.98 / 9.56 |
| Local shadows + 2× resolution | 6.15 / 6.58 | 6.34 / 7.97 | 7.57 / 9.43 |
| Local shadows + TAA | 6.83 / 7.18 | — | 8.17 / 10.05 |
| TAA + AO + contact shadows | 8.27 / 8.73 | — | — |

The rich material stack and ten lights are modest additions in Kin for this view. Sun shadows raise main-frame cost, and local
shadows dominate: Kin's median CPU submission reaches 6.12 ms of a 6.49 ms frame interval, with 3,504 sun-map commands and
roughly 4,300 local-map commands on top of the 3,504 main draws. The local array holds eight layers / 32 MiB. This identifies
CPU shadow preparation/recording/submission as a useful next investigation target. The sampling variants remain largely on
that CPU critical path, so MSAA/2× resolution's slightly lower loop medians do not imply they are free or faster. The host-fence
observation rises from 2.63 ms (local shadows) to 3.67 / 3.58 ms (MSAA/2× resolution), but it is not an isolated GPU timestamp.
TAA raises CPU submission to 6.47 ms, and TAA+AO/contact to 7.90 ms; the latter's optional view targets occupy about 37.4 MiB.

Godot's moving-light case raises its CPU setup measurement to about 2 ms (versus roughly 0.01 ms without the additional lamps).
Large overlapping light bounds require per-object light pairing; Kin's flat global light list has a different cost. Renderers
also differ in ambient/IBL shading, BRDFs, normal mapping, shadow-map layout/fitting/filtering, tonemapping and defaults such
as Forward+'s depth prepass. Background tonemapping differs too. Thus these are feature-workload comparisons with captures
for inspection, not pixel-identical rendering or a universal engine ranking. All runtime/import logs were checked for errors.

Peak benchmark RSS: Kin 219.6 MiB, Mobile 632.2 MiB, Forward+ 699.3 MiB, excluding GPU memory. One process at a time; guards
require 2 GiB available before starting and stop above 1,536 MiB process RSS or below 1 GiB available. Assets/results are on Linux
disk; no large texture/model packs were downloaded. Unsupported combinations are recorded: Godot Mobile TAA and the unmatched
AO/contact-shadow combination. Mobile requests over eight lights of either type and TAA combined with MSAA are rejected.

Validation: Blockfield CPU tests passed. Input-driven basic/PBR/high smoke tests passed jumping, editing, placement/overlap,
save/reload, streaming and view controls; all finished with world hash `8823336400901242737`, 25 resident columns and the same
resident mesh bytes. Requested lighting/AA/compute work was checked. Final peak smoke process-tree RSS was 241.2 / 245.5 / 250.0
MiB. Benchmark and close-up captures and the toned playable high-preset image were inspected. Rich-rendering captures intentionally
include fine detail that exposes aliasing; benchmark textures use linear filtering without mipmaps. Close-ups were captured
for no AA, MSAA 4×, 2× resolution, TAA and TAA+AO/contact (single inspection runs, not cross-engine performance comparisons).

A bounded 199 Hz main-thread cycle sample (no stacks/inheritance) recorded only 128 KiB and peaked at 231.4 MiB process-tree
RSS. In a steady +3..+6 second P-core window, the leading sampled symbols were local-shadow preparation and Vulkan buffer
binding/draw recording; `profile-findings.json` holds weighted periods and counts. These are hotspot shares, not wall-time
measurements, and they exclude helper threads. They support investigating shadow caster preparation and command recording.

Reproduce via `examples/blockfield/run_render_compare.py --kin build/bin/blockfield_render_compare --lighting --frames 180
--repeats 3` and `examples/blockfield/smoke.py build/bin/blockfield OUTPUT --quality high`. Raw CSV, reports, process metadata,
images, binary/snapshot hashes, close-ups and `findings.json` are in `$HOME/kin-bench/lighting-materials/`.

### Blockfield — flatten local-shadow lists and retain GPU page bindings (2026-10-15)

Implemented in Kin, with no Blockfield geometry, material, light or sampling changes. Local-shadow gather tasks write one
classification byte per candidate copy and per-level counts into disjoint worker ranges. A single accepted-copy allocation
backs spans in the per-view draw entries; clustered copies are compacted in place. This removes the allocating vectors in
individual tasks and draw buckets while preserving (view, draw, level, copy) order, cache keys, skin bounds and clustered cuts.
Scratch is proportional to the current render, freed after preparation, rather than a retained cache for every mesh/light pair.

DrawBinder retains vertex/index page bindings at zero and supplies mesh offsets through indexed draws' first index/base vertex.
All direct mesh passes use the same helper: main, sun/local shadows, occluders, AO/contact depth, velocity and picking. LOD first
indices are relative to their mesh's index slice. Skin/cluster vertices remain dedicated with zero base vertex; compacted and
indirect cluster indices retain their existing addressing. Pool allocations contain whole 36-byte vertices and 4-byte indices;
page changes and sky resets invalidate the corresponding bindings. Uploads, geometry residency, draw order and retirement fences
are unchanged. `KIN_MESH_PAGE_BINDINGS=0`, read at renderer construction, retains the former binding-offset draw path for diagnosis.

The preserved pre-change executable and final Release executable ran three alternating pairs per workload, one process at a
time, with 300 warm-up and 180 measured frames. Device: RTX 4080 Laptop / Vulkan. Same distance-32 snapshot, FOV 90, 1280×720
output, 3,195,550 main-pass triangles / 3,504 draws on every measured frame, PBR maps/IBL, eight points / two spots. Shadows
remain uncached and animate by frame index. Values below are medians of run p50 frame intervals, in milliseconds:

| Workload | Before | After | Frame-time reduction |
|---|---:|---:|---:|
| PBR + ten lights | 1.24 | 1.16 | 6.7% |
| + sun/local shadows | 5.31 | 3.05 | 42.6% |
| Local shadows + MSAA 4× | 5.59 | 4.39 | 21.4% |
| Local shadows + 2× resolution | 5.66 | 4.43 | 21.7% |
| Local shadows + TAA | 6.61 | 3.71 | 43.8% |
| TAA + AO + contact | 7.54 | 4.20 | 44.3% |

Local-shadow CPU submission falls from 5.01 to 2.46 ms, while p95 loop intervals fall from 5.56 to 3.29 ms. With page binding
disabled in the new executable, three gather-only runs reach 4.21 ms loop / 3.87 ms submission; these diagnostic runs follow
the paired sweep, so they are indicative of attribution, not another alternating comparison. MSAA and 2× resolution now expose
more GPU waiting: their host-fence observations are about 4.38 / 4.45 ms. Those are not GPU timestamps and cannot isolate a pass.
One baseline TAA repeat had an 8.98 ms median; the other two were 6.50 / 6.61 ms. CPU/GPU clocks and host scheduling vary, so use
these paired results rather than subtracting the earlier 90-run sweep. Godot was not rerun for these Kin-only changes.

All six paired first-repeat screenshots match byte-for-byte after decoding RGBA. Every measured row retains identical main,
sun/local-shadow, light, sampling, TAA/AO/contact and target-byte work. Shared geometry still reserves 270,270,464 GPU bytes.
Across the 39 benchmark runs, peak process-tree RSS is 255.0 MiB before and 244.4 MiB after (GPU allocations are excluded);
the high-quality playable smoke peaks at 252.1 MiB. This is an allocation/recording optimization with no additional geometry
or image targets. Builds remain single-job, runs sequential, with the existing 2 GiB start / 1 GiB running RAM guards.

Validation: full GPU 3D suite on NVIDIA; focused shared-buffer, local caster and clustered local-shadow checks on lavapipe;
high-quality Blockfield input smoke. Shared-buffer coverage compares dedicated, pooled binding-offset and pooled page-addressed
pictures, including retirement/reuse. The caster test compares five visible poles with five accepted copies interspersed among
9,000 instances across 2,048-copy boundaries, serially and with two workers. Shadow cache, mask, skin, LOD, clustered, picking
and temporal/prepass coverage remain in the full suite. Driver validation remains Vulkan-only; D3D12/Metal need separate runs.

Fresh 199 Hz main-thread cycle profiles (no inheritance/stacks, sampling delayed 2 s, steady +1..+3 s P-core window) contain
401 baseline / 397 optimized samples. Vulkan index/vertex binding symbols fall from a combined 20.6% to 6.3% of sampled periods.
The local-shadow function's own leaf share falls from 26.4% to 16.9%, with its separate gather helper still present. These are
sampled hotspot shares, not wall time or inclusive costs; unknown symbols account for 14.7% / 17.1%. The recordings occupy
189 / 122 KiB and peak below 244 MiB process-tree RSS. Remaining CPU work includes culling/upload and scene instance setup.

Raw CSV, reports, screenshot pairs, process metadata, binary/snapshot hashes, profiles, the baseline executable, measurement
helpers and `findings.json`/`summary.json` are under `$HOME/kin-bench/shadow-binding-opt/`. `measure.py` reproduces sequential
pairs using the preserved baseline and current build; `profile.py` reproduces the bounded CPU sampling.

### Blockfield — optimized Kin versus Godot lighting rerun (2026-10-15)

Reran the complete lighting/material/sampling comparison after the Kin shadow-list/page-binding optimizations: 90 sequential
runs, three alternating repeats of 30 supported renderer/workload groups, with 300 warm-up / 180 measured frames per run.
Same RTX 4080 Laptop / Vulkan device, 1280×720 output, FOV 90, one shared distance-32 snapshot, 3,195,550 main triangles and
3,504 draws on every measured frame (16,200 total measured frames). The higher-resolution case renders 2560×1440. Kin is the
optimized Release executable; Godot Mobile/Forward+ use the same official 4.7.2 release binary and unchanged exported pack
as the earlier sweep. No new Godot download/export or geometry copy was needed; hashes of binaries, pack, scripts and snapshot
are recorded. Page binding, shared meshes and unlit specialization are enabled explicitly in the Kin environment.

The table gives medians of three run p50 / p95 frame intervals, in milliseconds; smaller is faster:

| Workload | Kin | Godot Mobile | Godot Forward+ |
|---|---:|---:|---:|
| Unlit HDR | 1.15 / 1.22 | 1.25 / 1.32 | 2.14 / 2.63 |
| Lit HDR | 1.14 / 1.23 | 1.25 / 1.46 | 2.10 / 2.54 |
| PBR maps + IBL, flat normals | 1.26 / 1.33 | 1.73 / 2.01 | 2.83 / 3.73 |
| PBR + normal detail | 1.28 / 1.36 | 1.75 / 2.25 | 2.84 / 3.69 |
| + 8 points / 2 spots | 1.30 / 1.36 | 3.88 / 5.19 | 5.18 / 6.72 |
| + sun shadow | 1.97 / 2.18 | 5.96 / 7.39 | 7.18 / 9.02 |
| + local shadows | 3.09 / 3.36 | 6.03 / 7.58 | 7.36 / 9.74 |
| Local shadows + MSAA 4× | 4.35 / 4.55 | 5.72 / 7.15 | 6.97 / 8.50 |
| Local shadows + 2× resolution | 4.32 / 4.64 | 6.21 / 7.57 | 7.04 / 8.62 |
| Local shadows + TAA | 3.42 / 3.65 | — | 7.19 / 8.99 |
| TAA + AO + contact | 4.00 / 4.25 | — | — |

In the local-shadow workload Kin reaches 1.95× Mobile's loop throughput and 2.38× Forward+'s. Its p95 interval is 3.36 ms
against 7.58 / 9.74 ms. With MSAA 4× the throughput ratios narrow to 1.32× / 1.60×, and with 2× resolution to 1.44× / 1.63×.
TAA is 2.10× Forward+'s loop throughput. Basic PBR + normal detail is a smaller 1.36× advantage over Mobile; the ten-light
case is 2.99×. This remains a draw-heavy voxel scene with large overlapping moving lights: Godot's per-object light pairing
contributes CPU setup cost, while Kin uses its flat global list. These ratios are specific to this workload and device.

Local-shadow Kin CPU submission is 2.55 ms; Mobile/Forward+ report 1.83 / 1.93 ms setup and 2.70 / 3.51 ms viewport CPU time.
These timers have different boundaries and must not be added or compared as equivalent render phases. Kin's host-fence
observation of 3.09 ms is not equivalent to Godot's GPU timestamps (1.44 / 1.94 ms), so the loop result does not establish
that Kin's shaders or GPU passes are faster. The MSAA/resolution cases continue to show more GPU waiting in Kin. Host clocks
and scheduling differ from earlier sweeps; use this fresh interleaved run for the comparison, and the preceding paired
pre/post-Kin section for the optimization's causal measurement.

Validation checked the exact geometry/draw counts on every measured row, requested/actual Kin GPU feature counters, Godot
release/version/adapter/renderer, and equal camera/material/light/sampling/shadow request fields across 57 paired reports.
All runtime logs are free of errors. All 30 first-repeat captures match their respective engine's earlier capture byte-for-byte
after RGBA decoding; this confirms output continuity across the optimization/retest. It does not imply equality between
engines: BRDF/normal/IBL shading, background tonemapping, shadow fitting/atlas/filtering and defaults still differ. Mobile TAA
and the unmatched Godot AO/contact cases remain unsupported in this harness, not zero-cost results.

Peak process-tree RSS: Kin 241.0 MiB, Mobile 606.2 MiB, Forward+ 700.1 MiB; these exclude GPU memory. Kin's shared geometry
reservation remains 270,270,464 GPU bytes. One renderer ran at a time under the existing 1.5 GiB RSS cap, 2 GiB start and
1 GiB running available-RAM guards. The sweep reused existing releases and the snapshot, with no build or install.

CSV, reports, first-repeat images, process metadata, `runs.json`, `summary.json`, `findings.json`, `unsupported.json`, and
reproduction/analysis helpers are in `$HOME/kin-bench/lighting-after-cpu-opt/`. `run.py` reruns the 90 cases using the existing
verified Godot export and current Kin build; `analyze.py` verifies cross-engine requests and continuity against the original
captures. The earlier `$HOME/kin-bench/lighting-materials/` results and captures are preserved.

### Blockfield — ground tour, foliage and transparency (2026-10-15)

Added the next three workload dimensions in both engines: a frame-indexed ground camera route, alpha-cutout vegetation and
layered transparent mist cards. `--preset grove` selects a 24-second route, 10,000 plants / 512 cards, PBR/IBL, HDR, four
points / one spot, and sun/local shadows. `--foliage 0..100000` and `--transparent 0..20000` vary independently. `--camera
ground --dressing FILE` uses just the route with the caller's material/light settings. Explicit overrides work regardless of
preset argument order. Kin `--preview` opens a window at a 60 FPS cap, starts the route immediately, and closes with Escape
or the window; its paced timings are not benchmark timings. Bare/foliage/transparency/combined CLI variants are documented in
`examples/README.md`. Crowd animation, streaming/destruction and authored interiors remain future work.

One generated BFD1 scene recipe contains all shared camera poses, prefix-stable transforms, two meshes and two textures.
It is 3,477,132 bytes (3.32 MiB), generated from the same seed-1 terrain as the BFC snapshot. Camera poses are baked above
terrain and existing voxel tree bounds; a clear annular lane keeps new plants out of the route. Each engine streams only the
requested transform prefixes. Plants have three crossed masked cards (six triangles), mist one alpha-blended card (two
triangles). Their 128×128 and 64×64 RGBA textures total 81,920 bytes; meshes/textures are shared across every instance. No
new terrain copy, unique per-plant geometry, per-instance physics or downloaded assets are needed. Populations are static;
the camera and existing lights move by frame index. An explicit unlit material also makes the plant cards unlit.

The bounded sweep completed 72 sequential release GPU runs: three alternating repeats of eight cases in all three renderers,
300 warm-up / 300 measured frames each (21,600 measured frames). Same RTX 4080 Laptop / Vulkan, 1280×720, vertical FOV 75,
one distance-32 terrain snapshot with 3,504 resident chunks / 3,195,550 source triangles. The native and Godot work requests
match, but this ground view enables normal frustum culling, so submitted triangles differ and are recorded per frame.
Values are medians of run p50 / p95 frame intervals in milliseconds:

| Workload | Kin p50 / p95 | Godot Mobile p50 / p95 | Godot Forward+ p50 / p95 |
|---|---:|---:|---:|
| Bare ground route | 2.20 / 2.47 | 0.86 / 1.41 | 0.99 / 1.48 |
| 10k plants | 3.52 / 3.87 | 1.01 / 1.69 | 1.01 / 1.35 |
| 50k plants | 8.18 / 8.70 | 1.75 / 2.07 | 1.03 / 1.44 |
| 100k plants | 14.39 / 15.46 | 2.57 / 2.96 | 1.53 / 1.73 |
| 512 transparent cards | 1.81 / 1.88 | 0.74 / 0.91 | 0.87 / 1.30 |
| 5k transparent cards | 2.05 / 2.15 | 0.78 / 1.12 | 0.97 / 1.31 |
| 10k plants + 512 cards | 2.95 / 3.21 | 0.87 / 1.00 | 0.83 / 1.18 |
| 100k plants + 5k cards | 14.28 / 15.29 | 2.62 / 3.02 | 1.55 / 1.68 |

This scene exposes a different limit from the full-field terrain comparison. Kin's 100k-plant case is 14.39 ms p50 / 16.33 ms
p99, with CPU submission about 13 ms in the first repeat. Godot Mobile reaches 2.57 ms and Forward+ 1.53 ms p50. Kin's 100k
repeat medians are 14.39 / 13.80 / 19.52 ms: host scheduling/clocks affect the result, and a median does not imply every run
fits a 16.67 ms budget. Small nonmonotonic component timings likewise must not be read as negative feature costs. Inspect the
per-run CSV/p99 values rather than subtracting separately measured cases.

The current code paths explain an important architectural difference: Godot sets its static MultiMesh instance transforms at
load and retains their GPU buffers, using 64 spatial plant batches and eight mist batches. Kin copies scene instances and
culls/packs visible and caster copies every frame; it may merge consecutive equal mesh/material batches. Frustum tests also
differ (for example, sphere versus tighter batch bounds), so this is a same-scene comparison, not an equal-submitted-triangle
throughput test. The existing full-field triangle workload remains available separately. Shadows, material shading and
background tonemapping remain renderer-specific; screenshots are not expected to match between engines.

A bounded 199 Hz main-thread CPU sample of the heavy case (P-core affinity 0–15, no inheritance/stacks, 4 s delay, steady
+1..+3 s after sampling begins) records 399 samples / 141 KiB. Local-shadow gather and preparation account for 43.6% of
sampled periods, with culling/packing callbacks and GPU-memory copying prominent next. These are leaf sample shares, not
wall time or inclusive costs, and exclude helper threads. The next Kin-side targets are coarser/static-instance culling and
retaining instance/caster data on the GPU, before assuming more shader work is the main problem.

Peak density-sweep process RSS: Kin 308.8 MiB, Mobile 600.8 MiB, Forward+ 618.7 MiB (GPU memory excluded).
The largest combined case uses only shared meshes/textures plus its bounded instance data. Builds ran with one job, renderers
one at a time under the existing RAM guards. The source snapshot and previous Godot lighting package are preserved; a
separate grove export is hard-linked to the existing Linux template rather than another 71 MiB copy.

Validation includes the 72-run counter/feature checks, matching 48 Kin/Godot scene request reports, visible pixel changes for
every added component/density, deterministic recipe regeneration, continuous route wrapping, invalid limit/context rejection,
both population caps together (100k/20k), and a real preview window that advances/closes normally. Existing Kin lighting and
local-shadow captures, and both modified Godot local-shadow paths with extras off, match the earlier captures exactly. A final
unlit-option correction is checked in all three renderers; final default combined captures and all per-frame draw/triangle
counts match the density-sweep versions. Measured binaries/pack are retained separately from the final binaries for provenance.

Results, captures, process RSS, hashes, validation logs, archived measured binary/pack, profile and `findings.json` are in
`$HOME/kin-bench/grove/`; the initial smoke run is in `smoke-grove/`. Reproduce with
`python3 examples/blockfield/run_render_compare.py --kin build/bin/blockfield_render_compare --grove --frames 300 --repeats 3`.
The helper now records p99 as well as p50/p95/mean; regular lighting/triangle sweeps keep their original defaults.

### Grove — CPU profile and proposed engine work (2026-10-15)

The heavy grove is primarily limited by CPU instance/shadow preparation on this machine. This investigation adds only
optional benchmark clocks and renderer worker selection; the engine optimizations below are proposals. The earlier Godot
comparison is preserved. Fresh Kin timings use a different affinity and zero mist, so they are not an optimization gain
against the earlier 14.39 ms result.

**Controlled measurements.** Same Release engine (`-O2`, benchmark wrapper `-O3`), RTX 4080 Laptop / Vulkan, 1280×720,
ground FOV 75, distance-32 snapshot, 100,000 static plants / zero mist, PBR/IBL/HDR, four points / one spot. Runs are sequential,
pinned to P-core logical CPUs 0–15, with 300 warm-up / 180 measured frames and three alternating repeats per condition.
Values below are medians of each run's p50, in milliseconds; thread/process CPU clocks cover `render()` only.

| Condition | Frame interval | Render elapsed | Caller CPU | Process CPU |
|---|---:|---:|---:|---:|
| No added plants, all shadows | 1.51 | 1.17 | 1.17 | 1.18 |
| 100k plants, no shadows | 3.50 | 2.48 | 2.48 | 2.50 |
| Sun shadows only | 4.79 | 3.79 | 3.79 | 3.82 |
| Local shadows only | 10.63 | 9.53 | 9.53 | 9.64 |
| All shadows, serial | 12.04 | 10.92 | 10.92 | 11.05 |
| All shadows, two helpers | 9.41 | 8.21 | 8.21 | 17.15 |
| All shadows, four helpers | 8.12 | 6.97 | 6.95 | 19.88 |
| All shadows, serial, 2560×1440 | 11.81 | 10.69 | 10.69 | 10.81 |

Caller CPU nearly equals elapsed render time, and four times the pixels barely changes frame time. This supports CPU
preparation as the present limit; it is not a measurement of GPU pass durations. The higher resolution is not inherently
faster: run-to-run scheduling/clocks vary. Serial heavy run medians are 11.57 / 12.04 / 14.52 ms. Two/four helpers improve the
heavy case in every repeat (about 22% / 33% reduction in the aggregate), but increase total CPU consumed, with power and
contention implications. The benchmark previously did not attach a job system, so its existing parallel paths were inactive.

A separate alternating three-repeat check of the **bare** route finds serial / two / four helpers at **2.16 / 4.46 / 3.49 ms**.
Absolute timings shifted with concurrent host activity, but both worker counts lose to serial in every repeat. Do not apply
the heavy-case worker improvement as an unconditional default. Local-shadow sphere/gather pieces are per draw: thousands of
one-instance terrain draws become thousands of tiny items. `JobSystem::run_parallel_items` performs atomic work claiming and
completion per item; batching these items is a concrete first candidate, rather than adding threads alone.

**Sampled paths.** A separate 99 Hz main-thread `cycles:u` capture, no inheritance, DWARF stacks limited to 4 KiB, is 3.7 MiB.
Aligning samples with the CSV monotonic clock selects all 600 measured frames (6.74 seconds): 579 P-core samples, zero lost
samples, 0.39% unresolved leaf periods. Call chains often stop above the renderer; they distinguish these branches but are not
a complete application call tree. Exclusive caller groups account for **56.4% local-shadow preparation**, **30.8% camera/sun
culling, packing and upload**, **9.5% scene submission**, and 3.4% other. These are sampled CPU-cycle shares on the calling
thread, not wall-time percentages or helper-thread totals. Within those groups, gather's own leaf is 23.9%, the rest of
`build_local_shadows`' own leaf 19.2%, the main cull callback 10.5%, instance packing 6.0%, GPU-memory copying 5.7%, sphere
calculation 5.2%, and directional shadow reach calculation 4.2%. Do not add caller and leaf shares together.

**Upload counts.** GDB entry probes count `GpuDevice::upload_storage_buffer` arguments for frame 480 (route frame 180), then
stop. Debugger timings are discarded. The three instance/list uploads total **17,577,616 bytes (16.76 MiB)**:

| Buffer | Records | Bytes |
|---|---:|---:|
| Camera/sun instances | 35,847 × 128 | 4,588,416 |
| Local-shadow union instances | 96,088 × 128 | 12,299,264 |
| Local-shadow view indices | 172,484 × 4 | 689,936 |

These are CPU upload payloads at one pose, not measured PCIe traffic or all GPU bandwidth. Geometry/textures are already
resident. Every plant transform is static, yet the immediate scene API copies them into `MeshInstance` records, camera/sun
culling computes world spheres and GPU records, and local shadows recompute those spheres and build another GPU record union.
The local union zeroes full 128-byte records although the shadow shaders only need model/alpha. Existing shadow-map caching
checks after caster preparation, and moving benchmark lights invalidate maps, so simply enabling that cache cannot remove
this work. Godot's local benchmark source retains static MultiMesh records and 64 spatial plant batches; Kin merges those
equal mesh/material submissions into one draw and tests individual copies for camera and light views.

**Proposed order, all Kin-side unless noted:**

1. **Batch shadow jobs and make worker dispatch depend on useful work.** Coalesce small draws into ranges of roughly a few
   thousand candidates, as the main culler already does, instead of one scheduled item per tiny draw. Retain deterministic
   view/draw/level/copy output order. Attach a bounded shared renderer job pool, initially test two helpers, and keep cheap
   stages serial. Re-measure bare, 10k and 100k cases before choosing thresholds; the bare-route regression above is a required
   acceptance check. Task counts, CPU time and aggregate process CPU should accompany frame time. No new per-instance cache.
2. **Compute bounds once per frame and remove redundant packing passes.** Share world spheres between camera/sun/local
   culling, preserving skin/LOD and offscreen-caster rules. Reuse scratch capacity; count the local union, resize once, and
   write disjoint records directly rather than grow/zero/copy each record. Use the staging fill callback where safe. A shared
   100k sphere array is about 1.53 MiB and should replace `_local_spheres`, not be a second permanent copy. These are bounded
   changes before an API redesign; measured leaf costs support them, but no speedup is promised yet.
3. **Preserve spatial cells and reject batches before individual instances.** Add conservative aggregate bounds to instance
   groups and preserve their identity through scene merging. Use the recipe's 64 plant cells for the first test. Cull each
   cell against the camera and each shadow view, then refine partial intersections; keep wholly visible cells cheap. A cell
   outside the camera may still cast a visible shadow, so camera visibility must not decide light visibility. Sixty-four
   AABBs cost about 1.5 KiB. Compare accepted instance/caster sets against the serial reference; record candidate/test counts,
   draw counts and CPU time to ensure more batches pay for their draw overhead. Grove needs to use the new grouping API.
4. **Retain static instance records on the GPU and share them across passes.** Introduce explicit instance handles/revisions
   and dirty ranges; scene submission refers to a retained batch instead of copying every transform. Cache bounds and normal
   transforms on edits. Camera/sun/local passes use one stable record table plus 4-byte per-view visibility indices. Local
   shadows already support index lists; the main, unlit, depth, velocity and picking paths need matching indirection, so this
   is not just deleting the local buffer. Start with built-in opaque/masked static instances and retain the immediate path
   for dynamic/skinned/custom/ordered-alpha work. Material/tint/ID and mesh-bound changes must invalidate the right records.
   At 100k plants the current record format costs 12.21 MiB on the GPU; cached spheres cost 1.53 MiB on the CPU, with per-view
   indices typically a few MiB. Replace transient duplicates rather than retain both paths' buffers indefinitely; bound fence
   versions and wait under pressure. Report actual allocation and retirement peaks, including GPU memory. Goal: no transform
   upload for an unchanged static batch after warm-up, only visibility lists, with correct partial updates and destruction.
5. **Then evaluate GPU culling and indirect draws.** Once stable records and visibility lists exist, move opaque/masked cell
   and instance classification/compaction to compute and issue bounded indirect draws, following the engine's existing GPU
   cluster machinery. Start with camera visibility, then shadow views. Keep alpha ordering and unsupported paths on the CPU.
   Budget bounds/list buffers explicitly and avoid readback per frame. This is a later experiment, justified only if CPU
   culling remains material after steps 1–4; shader tuning and GPU occlusion are not the first response to this capture.

Each implementation should retain exact image/counter equivalence where draw order is unchanged, and reference accepted
sets where grouping changes it. Cover nonuniform/negative scales, masked shadows, offscreen casters, picking, TAA history,
dirty ranges, removal and fence reuse. Re-run paired three-repeat bare/10k/100k cases at 720p and the 2× resolution control;
report p50/p95/p99, main/process CPU, candidate counts, upload bytes and CPU/GPU peak allocation. Do not accept a heavy-scene
win that silently regresses the bare scene or grows memory without a bound. Numerical gains beyond the worker experiment
remain unmeasured proposals.

Normal 24-run matrix peak RSS is 271.8 MiB; the stack capture reaches 279.7 MiB and the debugger count probe 414.6 MiB.
These exclude GPU memory. Runs use 1 GiB RSS guards (1.5 GiB for debugger/perf), one process at a time, and Linux disk for
artifacts. Optional clocks/worker wiring are checked against the archived pre-investigation executable: exact RGBA and all
180 per-frame work counters. Two/four helpers match serial RGBA and all counters in the heavy case; bare worker captures also
match. Disabled CPU columns are zero and out-of-range worker counts are rejected. A single-job build and diff whitespace
check pass. No core renderer changes were made for this investigation.

Raw CSV/reports, screenshots, process commands/RSS, `summary.json`, `findings.json`, `profile-analysis.json`, `uploads.json`,
`validation.json`, bounded capture scripts and the archived pre-investigation executable are in
`$HOME/kin-bench/grove-profile/`. `measure.py` repeats the 24-run matrix; `small.py` repeats the nine-run worker-overhead check.
The README gives a single-run command and explains the optional clock columns. Host background load varies; these are
within-machine diagnostic results, not isolated laboratory or new cross-engine throughput claims.

### Grove — batched shadow jobs and shared bounds results (2026-10-15)

Implemented the first two profile priorities in Kin's renderer. Local-shadow gather keeps its original view/draw/level/copy
order but schedules groups of about 2,048 or more candidates across tiny draws, rather than one scheduled item per tiny draw.
The existing 8,192-candidate parallel threshold is retained; no global job-system policy or worker default changes.

Light/view selection now precedes main culling. When local/moon shadows need bounds, that cull saves the same current-render
world spheres into the former local-sphere scratch array, including posed bounds and copies outside the camera. Local
shadows consume them without another transform/skin-bound pass. If camera culling and sun shadows are off, local casters
still explicitly compute their spheres. No cross-frame transform cache or additional permanent sphere array is introduced.

The local union now retains compact `(ref, copy)` pairs and writes full GPU records once into mapped transfer memory, in
disjoint 2,048-record ranges when workers pay. The 128-byte CPU record mirror and its incremental resizing/zeroing/copying
are gone. GPU record layout, uploads, caster selection and shaders are preserved. This step does **not** retain static
instance records on the GPU: the previous profile's 16.76 MiB upload payload at route frame 180 remains a later target.

**Paired measurements:** 54 sequential release runs, three alternating before/after repeats for bare / 10k / 100k plants,
each with zero / two / four helpers, 300 warm-up / 180 measured frames. Same snapshot, zero mist, 720p, FOV 75, PBR/IBL/HDR,
four points / one spot, sun/local shadows, P-core affinity 0–15. Each entry is median run p50 frame time in milliseconds:

| Plants | Helpers | Before | After | Frame-time reduction |
|---|---:|---:|---:|---:|
| Bare | 0 | 1.65 | 1.67 | effectively unchanged |
| Bare | 2 | 2.97 | 1.64 | 44.8% |
| Bare | 4 | 2.92 | 1.68 | 42.4% |
| 10k | 0 | 2.75 | 2.74 | effectively unchanged |
| 10k | 2 | 4.02 | 2.16 | 46.3% |
| 10k | 4 | 3.89 | 2.29 | 41.2% |
| 100k | 0 | 14.64 | 11.92 | 18.6% |
| 100k | 2 | 10.77 | 7.32 | 32.0% |
| 100k | 4 | 9.77 | 6.42 | 34.3% |

The small-scene worker penalty is largely removed; two/four helpers are now close to the serial bare route. Bare serial p50
is 1.4% higher, with p95 effectively unchanged and p99 slightly lower; do not interpret this noise as a useful speed change.
The heavy scene improves in every same-worker-count pair. Its median p95 values fall from 15.70 / 11.75 / 11.13 ms to
12.57 / 8.02 / 7.38 ms for zero / two / four helpers; p99 falls from 17.16 / 11.99 / 11.50 to 12.67 / 8.67 / 7.61 ms.
Host clocks and concurrent work vary, so these fresh paired results supersede neither the original Godot comparison nor
its absolute timings. The serial 10k case is also effectively unchanged; gains depend on the workload and worker count.

For the nine heavy before/after pairs, median process-RSS reduction is **13.44 MiB**, consistent with removing the large CPU
record mirror. Optimized heavy runs peak at 243.2 MiB. Some baseline/small-run RSS outliers reflect allocator/driver behavior;
do not attribute all peak differences to this change. RSS excludes GPU allocations. Existing GPU buffers and instance upload
sizes are unchanged. Builds used one job, renderers ran one at a time under 1 GiB RSS guards, and artifacts are on Linux disk.

The 54 runs validate all requested GPU features and every measured frame's work counters (9,720 measured frames). All nine
before/after RGBA pairs match exactly; optimized two/four-helper images also match serial at each density. Extended caster
coverage compares five poles with sparse 9,000-instance and 9,000-separate-draw scenes, including mirrored/nonuniform scales,
no sun/camera culling, worker-range boundaries, exact caster/triangle counts and serial/parallel pixels. Original mask/skin
checks remain. The full NVIDIA GPU 3D suite passes (88.7 seconds, 252.0 MiB peak process-tree RSS), as do the focused caster
checks on lavapipe (209.7 MiB). Shader code is unchanged; D3D12/Metal still need separate validation. Single-job builds and
the diff whitespace check pass.

A bounded follow-up 99 Hz main-thread DWARF capture selects all 600 measured frames by the monotonic clock: 568 samples,
zero lost samples, 1.41% unresolved leaf periods. Local-shadow preparation remains the largest caller group at 47.4%, followed
by main/sun culling and packing at 35.5%, and scene submission at 13.3%. GPU-memory copying's own leaf is now 0.18%; the new
direct shadow-record fill accounts for 9.0%. These are relative CPU-cycle shares, not wall-time savings, and exclude workers;
do not subtract them as absolute timings from the earlier profile. The capture is about 4.45 MiB and reaches 268.0 MiB RSS.
This supports spatial culling and retaining static instances as the next targets, after the bounded CPU work removed here.

Raw before/after CSV, process commands/RSS, images, `summary.json`, `comparison.json`, `findings.json`, baseline source and
executable, build/test logs and measurement scripts are in `$HOME/kin-bench/grove-cpu-opt/`. `compare.py` repeats the matrix;
`verify.py` runs the full GPU suite, lavapipe caster checks and a bounded follow-up CPU capture. Spatial-cell culling and
retained static GPU records remain the next architectural work; these changes do not close the earlier Godot gap by themselves.

### Grove — spatial cells through draw merging (2026-10-15)

Added Kin's optional `draw_instanced_group(mesh, copies, world_bounds, material)` API, with transform and `MeshInstance`
overloads, plus `instance_bounds()` to compute a conservative box around the renderer's transformed culling spheres.
Applications cache bounds until the mesh/transforms change. Invalid/nonfinite boxes use per-copy culling. Plain instance
groups stay separate as CPU metadata while equal mesh/material submissions retain their merged GPU draw and instance order.
Ungrouped gaps are supported; global alpha sorting, picking and temporal history retain their original order. Skinned draws
continue to use posed per-copy bounds. No shader or GPU record-format changes.

The main culler classifies each cell against the camera and sun maps. Local/moon views classify it independently, using their
frustum and bounded-light reach; rejecting a cell for the camera never rejects its offscreen shadow casters. Whole-cell
rejection avoids copy tests, full containment avoids plane tests, and partial cells refine spheres as before. Sun/moon swept
shadow-reach tests remain per copy. World spheres still refresh when any relevant view needs them; rejected cells cannot read
stale cached bounds. Float margins preserve conservative classification at boundaries.

The grove caches its existing 64 plant-cell / eight mist-cell bounds at load and submits those groups. It retains the same
recipe, geometry, textures, GPU draw batching and instance uploads. Cell metadata is a few KiB, with small per-draw/piece
indices, replacing no geometry or transforms. There is no new per-instance cache or GPU buffer. An initial implementation
scanned all refs even without cells; the final build classifies groups as refs are built and bypasses group setup for empty
scenes. `KIN_SPATIAL_CULLING=0` before renderer creation is the reference per-copy path, documented in the rendering API.

**Final same-binary measurements:** 54 sequential release runs, three alternating off/on repeats at 0/10k/100k plants and
0/2/4 helpers; 300 warm-up / 180 measured frames each. Zero mist, PBR/IBL/HDR, four points / one spot, sun/local shadows,
720p, FOV 75, same distance-32 snapshot, P-core affinity 0–15. “Off/on” changes only the environment switch. Median run p50
frame intervals in milliseconds (positive reduction means faster):

| Plants | Helpers | Cells off | Cells on | Reduction |
|---|---:|---:|---:|---:|
| Bare | 0 | 2.11 | 2.03 | 3.7% |
| Bare | 2 | 1.88 | 1.85 | 2.1% |
| Bare | 4 | 2.00 | 2.48 | -24.0% |
| 10k | 0 | 3.08 | 3.38 | -9.8% |
| 10k | 2 | 2.96 | 2.96 | unchanged |
| 10k | 4 | 2.90 | 2.43 | 16.2% |
| 100k | 0 | 12.75 | 10.95 | 14.1% |
| 100k | 2 | 8.34 | 8.08 | 3.0% |
| 100k | 4 | 10.09 | 8.71 | 13.7% |

Host load/clocks vary substantially, including in the bare control with no group work. Small-case results are mixed, so this
does not establish a universal speedup or justify a new worker default. In particular, the bare four-helper and 10k serial
regressions above must not be hidden. More isolated measurements are needed to quantify cheap-scene impact. The 100k
two-helper p95 is also slightly worse (9.09 to 9.22 ms); serial/four-helper p95 improve (13.22 to 12.41 / 11.08 to 9.41 ms).
These are fresh controls, not an absolute speed comparison with earlier sessions or another Godot run.

The actual reduction in work is deterministic. Across the same 180-frame 100k route segment, camera sphere tests average
103,504 off versus **24,484 on (76.3% fewer)**. Local-shadow candidates average 606,648 versus **279,687 (53.9% fewer)**.
World-sphere calculations remain 103,504 because the sun/local views still need them. Recorded main/shadow draw counts and
main visible instances/triangles match frame by frame. GPU instance upload payloads remain a later target.

Validation on the final build: all 54 runs' requested features and recorded work counters, nine exact off/on RGBA pairs,
and all worker images/counters match. The full NVIDIA GPU 3D suite passes (83.5 s, 251.8 MiB process-tree RSS), including
new grouped/ungrouped tests with 9,600 mirrored/nonuniform copies, ungrouped gaps, sun/local/moon/cascades, camera culling
disabled, alpha sorting, picking IDs and moving TAA views. Focused group tests pass on lavapipe (344.3 MiB). Two full-route
off/on runs with two helpers check all 1,440 recorded frames and exact RGBA at wraparound; another two full-route runs with
four helpers match those recorded counters too. Original pre-cell-build captures/counters also match for heavy, mist,
unlit and TAA/AO/contact cases (before the empty-scene fast-path follow-up). Builds use one job; diff checks pass. Validation
remains Vulkan-only; D3D12/Metal are untested.

RAM: most final density runs peak around 234–243 MiB, but one enabled four-helper run reaches **296.7 MiB** versus a 242.9 MiB
maximum in the disabled sweep. That outlier is retained rather than attributed to the cell metadata without evidence.
The two-helper full-route control is 260.0 / 259.7 MiB off/on; the extra four-helper full-route check is **251.3 / 251.0 MiB**
under a 512 MiB RSS guard. It does not reproduce sustained memory growth. RSS excludes GPU allocations. All renderers run
sequentially and artifacts stay on Linux disk; no extra terrain/assets or unbounded instance cache were introduced.

Results and provenance are in `$HOME/kin-bench/grove-cells/`: `summary.json`, `comparison.json`, `findings.json`, CSV/PNG,
process commands/RSS, archived pre-cell and final executables/source, build/test logs and reproducible scripts. `compare.py`
runs the final 54-condition off/on sweep; `verify.py` runs GPU/lavapipe/full-route validation; `memory4.py` checks the long
four-helper case. `trial1/` preserves the initial measurements and executable before the empty-scene fast-path follow-up.
The next architectural target is retaining static GPU records and avoiding the per-frame scene copies and instance uploads.

### Grove — retained CPU/GPU instance batches (2026-10-15)

Implemented the next architectural step in Kin, with Grove as its first caller. `InstanceBatch3D` owns fixed-size CPU
records and ordered spatial cells behind a shared handle. Scene submission retains the handle rather than copying all
records into `Scene3D`. Explicit range edits refresh intersecting cell bounds and increment the shared revision; edits are
made between renders. Grove flattens its existing cell order once at load and releases the original transform vectors.
Mist remains on its ordinary globally alpha-sorted path. Geometry, textures, camera route and draw counts are unchanged.

The GPU renderer retains stable 128-byte records in per-batch tables and uploads compact 32-bit visibility lists for each
view. Main rendering, depth/normal prepass and picking use indexed vertex variants; sun/local/moon shadows use their existing
indexed shader interface. Unchanged tables upload zero records, and local views share the table instead of building another
128-byte caster record for the same copy. Normal packing, material/tint colour and picking IDs preserve the old ABI.

Memory is bounded to **32 MiB of live tables and 128 cache entries per renderer**. Overflow uses the existing streaming path;
weak ownership allows collection of expired batches between GPU frames after waiting for submitted GPU work. Staging,
streamed buffers and in-flight backing versions are additional to this live-table cap. A changed revision/material colour
reuploads the full table, at most once per table per GPU frame. Conflicting material colours or edits in a later view of that
frame use streaming. TAA, alpha/additive, custom shaders, selected multi-level/cluster LOD, occlusion culling and unavailable
indexed shaders also use streaming. CPU handles still avoid scene copies in these cases. No persistent world-sphere cache
was added: the existing per-render sphere scratch is unchanged. No worker default or compiler optimisation flag changed.

**Measurements:** 63 sequential release runs: three alternating repeats of GPU retention off/on at 0/10k/100k plants and
0/2/4 helpers (54 runs), plus nine 100k controls restoring the original CPU submission too. Each has 300 warm-up and 180
measured frames, zero mist, PBR/IBL/HDR, four point lights, one spot, sun/local shadows, 720p/FOV75 and the distance-32 snapshot;
CPU affinity stays 0–15. Engine Release remains `-O2`; the benchmark wrapper remains `-O3`. Off/on uses the same executable,
shared CPU batch handles and cell bounds, isolating GPU retention. Median run p50 frame intervals, milliseconds:

| Plants | Helpers | GPU retention off | On | Reduction |
|---|---:|---:|---:|---:|
| Bare | 0 | 2.41 | 2.39 | 0.9% |
| Bare | 2 | 2.36 | 2.63 | -11.4% |
| Bare | 4 | 2.62 | 2.67 | -2.0% |
| 10k | 0 | 3.23 | 3.14 | 2.6% |
| 10k | 2 | 2.77 | 2.69 | 2.8% |
| 10k | 4 | 3.43 | 2.62 | 23.8% |
| 100k | 0 | 12.15 | 9.46 | 22.1% |
| 100k | 2 | 6.50 | 5.10 | 21.5% |
| 100k | 4 | 5.35 | 4.27 | 20.2% |

100k p95 improves 12.88→9.93 / 6.92→5.41 / 5.88→4.43 ms for 0/2/4 helpers. Including the eliminated CPU scene copies,
original-submission controls versus the retained path are **12.14→9.46 / 6.98→5.10 / 7.22→4.27 ms** (22.1/26.8/40.8%
reductions). For example, two-helper scene-submission caller CPU drops 1.23→0.41 ms. Host clocks/load remain noisy: retain
the bare-control regressions above, and do not infer universal speedups, change worker defaults or compare these absolute
times directly with older sessions/Godot. There is no new Godot run in this step.

The upload reduction is deterministic. Mean streamed main/local **record** payload over the measured 100k segment drops
**17,638,665→92,297 bytes/frame (99.5% less)**, with **150,977 bytes/frame of new main-view indices** and zero retained-record
uploads after warm-up. Existing sun/local index lists still upload and are not included in those record-byte figures.
The persistent foliage table is 12,800,000 bytes (**12.21 MiB**) and initially uploads once. CPU foliage records occupy
7.2 MB rather than keeping both 6.4 MB of source matrices and 7.2 MB of copied scene records.

RAM: the 100k matrix peaks at 220.3 MiB RSS with retention versus 244.2 MiB with GPU retention off, or 243.3 MiB with original
CPU submission. Some smaller cases have RSS variation (10k serial on reaches 237.1 MiB), so there is no blanket
per-case RAM-reduction claim. A full 1,440-frame four-helper tour stays at **220.4 MiB on versus 255.2 MiB off**, under the
512 MiB guard. RSS excludes GPU allocations. All rendering/builds remain sequential, builds use one job, artifacts stay on
Linux disk, and the terrain snapshot is reused.

Validation: full NVIDIA Vulkan 3D suite passes (85.5 s, 273.8 MiB RSS), focused retained tests pass on lavapipe (6.7 s,
427.4 MiB) and with GPU retention disabled. New tests check exact RGBA and picking IDs over moving views/edits, tint/colour,
mirrored/nonuniform transforms, serial/two-helper execution, masks/unlit/lit, sun/cascades/point/spot shadows, MSAA,
AO/contact and TAA/alpha fallback. Additional cases check same-frame multiple views and edits, material conflicts, the
128-entry and 32 MiB limits, overflow fallback and expired-cache collection. All 63 benchmark runs match recorded work
counters; 12 density/worker/submission RGBA pairs match. The full tour matches all 1,440 recorded frames and exact RGBA at
wrap. Archived pre-change binary RGBA/counters also match. Separate 60-frame off/on mist/unlit/TAA/AO-contact checks match
exact RGBA and recorded counters. Bench CSV does not record every shadow caster/triangle count; the GPU tests compare those
caster counts and local-shadow triangles explicitly. D3D12/Metal remain untested.

Reprofiled the caller with no helpers, 99 Hz user-cycle samples and 4 KiB DWARF stacks, aligned to 600 measured frames.
Off/on retains 417/306 samples. With retention, main cull/list preparation is about 47.9% of sampled caller cycles and local
shadow preparation 42.8%; record-packing routines disappear from the dominant leaves. The largest remaining leaves are local
candidate gather (~25.7%), world-sphere calculation (~10.0%), and shadow-reach helpers `to_leave`/`shadow_in` (~16.2% together).
These are CPU samples, not GPU pass timestamps. The next target is cached transformed spheres for retained batches,
replacing their transient sphere scratch instead of adding another full array, then reducing shadow-reach refinement.

Artifacts: `$HOME/kin-bench/grove-retained/` contains `summary.json`, `comparison.json`, `validation.json`,
`features-validation.json`, CSV/PNG, bounded commands/RSS, both perf recordings/summaries, source/executable archives and
single-job build logs. `run.py` reproduces the main suite/matrix/tour/profile; `features.py` runs feature checks;
`analyze.py` makes the comparison/profile summaries. `KIN_RETAINED_INSTANCES=0` selects streaming GPU records;
`KIN_GROVE_RETAINED=0` additionally restores Grove's original CPU submission.

### Grove — cached transformed spheres, compact scratch (2026-10-15)

Implemented the next CPU step on top of retained instance batches. Resident GPU tables optionally own a fixed-size CPU
array of transformed culling spheres (16 bytes/copy). The existing 32 MiB/128-table GPU admission limit bounds this payload
to **4 MiB per renderer**, plus small container/allocation overhead. No CPU asset clone or GPU/shader changes were added.
Main camera/sun and local/moon culling read these same spheres; they still run their ordinary per-view visibility tests.
Spheres are calculated with the original `sphere_of` routine, preserving transformed centre/radius rounding.

A batch revision refreshes its full sphere cache on the next render, once per revision; material-colour changes alone do
not rebuild bounds. CPU validity is independent of GPU record versions: later views with edits, TAA/alpha, or conflicting
material colours may stream GPU records while using current CPU spheres from an already-resident table. Batches that have
never been admitted to the GPU cache keep dynamic bounds. Selected multi-level LOD retains the original scale calculation
rather than reconstructing scale from a rounded radius; posed bounds likewise stay dynamic.

Uncached local casters now have compact scratch offsets, separate from their offsets in visibility/LOD flags. Cached copies,
noncasters and overlays do not reserve local-sphere scratch. When a new cache would leave a large old scratch allocation,
the renderer releases that oversized capacity with hysteresis. This avoids retaining both full arrays after a streamed-to-
cached transition. Expired tables release GPU records and CPU spheres together. Ordinary non-batch draws bypass retained/
bounds setup calls. `KIN_CACHED_INSTANCE_BOUNDS=0` before renderer creation is the same-binary reference, with GPU retention
still enabled; `KIN_RETAINED_INSTANCES=0` prevents admission to either cache. There is no public submission API change.

**Measurements:** 54 sequential runs, three alternating off/on repeats at 0/10k/100k plants and 0/2/4 helpers. Each uses
300 warm-up and 180 measured frames, zero mist, PBR/IBL/HDR, four point lights, one spot, sun/local shadows, 720p/FOV75,
the existing distance-32 snapshot and P-core affinity 0–15. GPU retention/cell culling remain enabled on both sides.
Engine `-O2`, wrapper `-O3`, worker defaults and assets are unchanged. Median run p50 frame intervals in milliseconds:

| Plants | Helpers | Sphere cache off | On | Reduction |
|---|---:|---:|---:|---:|
| Bare | 0 | 1.62 | 1.66 | -3.0% |
| Bare | 2 | 1.78 | 1.93 | -8.1% |
| Bare | 4 | 1.91 | 1.91 | -0.3% |
| 10k | 0 | 2.59 | 2.40 | 7.2% |
| 10k | 2 | 2.17 | 1.98 | 8.9% |
| 10k | 4 | 1.96 | 1.94 | 1.1% |
| 100k | 0 | 6.67 | 5.35 | 19.8% |
| 100k | 2 | 4.23 | 3.70 | 12.6% |
| 100k | 4 | 3.55 | 3.45 | 2.8% |

100k p95 changes 6.92→5.57 / 4.54→3.94 / 3.80→3.81 ms for 0/2/4 helpers. The four-helper improvement is small relative to
host variation and its p95 does not improve. Bare controls allocate/use no sphere cache and still vary by up to 8.1% in the
median comparison; retain these regressions and do not claim a universal speedup. Serial/two-helper dense cases benefit more
than four helpers. These controls are fresh; absolute times are not comparable with older sessions or Godot. No Godot rerun.

The work/memory change is deterministic: **103,504→3,504 sphere calculations per frame** at 100k (96.6% fewer), with zero
cache rebuilds after warm-up. Camera sphere tests remain 24,484/frame on average and local-shadow candidates 279,687/frame;
all recorded draw/instance/triangle/shadow/upload counters remain equal. Sphere payload capacity never increases in the measured
matrix; at 100k it is **1,656,064 bytes off = 1,600,000 cached + 56,064 scratch bytes on**. The cache is 1.53 MiB for 100k.
Counters `bounds_cache_bytes`, `bounds_scratch_bytes`, `bounds_cache_instances` and `bounds_cache_rebuilt` are public and
included in the benchmark CSV/report. `bounds_instance_tests` includes actual cache rebuild calculations on cold/edited frames.

Matrix 100k RSS peaks are 219.9 MiB off/on; the full 1,440-frame four-helper route is **219.7/219.9 MiB**, under a 512 MiB
process-tree guard. Small-case RSS varies: a bare enabled run reaches 227.8 MiB with zero cache bytes; a 10k four-helper run
reaches 217.6 MiB. There is no blanket process-memory-reduction claim. Camera-only scenes previously needed no local-sphere
scratch and can add bounded resident CPU storage: the unlit feature check is 211.5→213.1 MiB RSS, consistent with the 1.53 MiB
sphere payload. TAA-only scenes without prior GPU admission do not allocate it. GPU allocations are separate from RSS.
Builds use one job, renderers run sequentially, artifacts stay on Linux disk and no terrain/assets were duplicated.

Validation passes: full NVIDIA Vulkan 3D suite (78.2 s, 258.9 MiB RSS); retained tests and new mixed-cache tests on lavapipe
(423.3/297.0 MiB peaks); focused caching off and GPU streaming fallback. Extended tests cover cold/hot/edited cache counts,
no rebuild for material colour alone, same-GPU-frame views/edits and cache eviction/budget accounting. The new mixed test
checks exact RGBA/picking and main/sun/cascade/local/moon work for cached ranges interleaved with uncached casters and a
noncaster gap, serial/two-helper execution, mirrored/nonuniform scales, TAA/alpha transitions, and disabled camera/shadows.
It first streams a large reference scene, then verifies that caching releases the old large scratch allocation. Existing
skinning/LOD tests pass. All 54 matrix runs match recorded work and verify no increase in sphere payload; nine off/on RGBA
pairs and cross-worker images match. Full-route recorded work and exact wrap RGBA match; archived pre-change RGBA/work and
60-frame mist/unlit/TAA/AO-contact pairs also match. Benchmark CSV does not contain complete shadow caster/triangle counts;
GPU tests compare caster/cascade counts and local-shadow triangles. D3D12/Metal remain untested.

Reprofiled with no helpers, 99 Hz user cycles / 4 KiB DWARF stacks, aligned to 600 measured frames. Off/on retains 271/194
samples. `sphere_of` falls from about **6.9% to 0.5%** of sampled caller cycles. Local-shadow preparation is now about 49.9%
(candidate gather ~28.5% as a leaf); main cull/list/upload preparation is 38.0%. Shadow-reach helpers `shadow_in`/`to_leave`
remain ~14.6% combined. These are coarse CPU samples, not GPU pass timestamps. The next target is local-shadow candidate
preparation and redundant per-copy reach/plane tests; keep offscreen casters and the same memory bounds when investigating it.

Artifacts: `$HOME/kin-bench/grove-bounds/` holds before/after executable/source archives, build/test logs, all CSV/PNG,
commands/RSS, `summary.json`, `comparison.json`, `validation.json`, `features-validation.json`, and both perf recordings/
summaries. `run.py` reproduces the suite, 54-run matrix, tour and profile; `features.py` checks feature outputs;
`analyze.py` produces the timing/profile summaries. No commits were created.

### Grove — local-shadow plane masks and accepted ranges (2026-10-15)

Implemented the next CPU gather step. Each spatial cell retains a mask of unresolved frustum planes for every wanted
local/moon view. Whole-cell rejection remains the same conservative box test. Partial ranges copy only the unresolved planes,
in original order, into a small worker-stack frustum once per task. Strict containment of the cell's whole sphere-enclosing
box inside a bounded light's radius separately proves that individual distance tests cannot reject any copy.

A non-clustered, single-level range fully inside both the frustum and light radius is accepted directly. It contributes its
count and consecutive source indices without reading spheres, writing classification bytes or scanning those bytes again.
Other tasks keep individual sphere/LOD/cluster classification. Moon views still test shadow reach into the camera/slice when
camera culling is enabled; camera rejection never discards a local offscreen caster. With camera culling disabled, unused
moon reach calculations are skipped. Material masking, accepted-copy order, shadow caches and GPU records/shaders are unchanged.

New persistent metadata is **24 bytes per cell** with current view limits (1.5 KiB for Grove's 64 foliage cells). GatherTask's
extra flags fit its existing padding on the tested ABI. A six-plane subset lives on the worker stack; no per-copy persistent
cache is added. The classification byte array now allocates only for refined candidates, and job thresholds use that work
rather than ranges already accepted by cells. Existing accepted-copy lists and retained-instance/sphere budgets are unchanged.
`KIN_LOCAL_SHADOW_CULLING=0` before renderer creation restores the previous gather refinement, keeping earlier cell rejection,
GPU retention and cached bounds. Ungrouped draws keep full tests on both sides.

Diagnostics now distinguish `local_candidates` after cell rejection, `local_instance_tests` receiving per-copy gather work,
and `local_group_accepted` accepted directly (counts are copy/view pairs, not distinct objects). `local_radius_tests` counts
actual distance checks. `local_plane_budget` is potential plane tests **before** per-copy radius rejection/early exits, not
executed dot products. `local_bucket_bytes` is temporary classification payload. All appear in stats, CSV and report.

**Measurements:** 54 sequential same-binary off/on runs: three alternating repeats at 0/10k/100k plants and 0/2/4 helpers;
300 warm-up / 180 measured frames each. Same PBR/IBL/HDR, four points/one spot, sun/local shadows, zero mist, 720p/FOV75,
distance-32 snapshot, affinity 0–15, engine `-O2` and wrapper `-O3`. Earlier retention/bounds/cell optimisations stay enabled.
Median run p50 frame intervals in milliseconds:

| Plants | Helpers | Local refinement off | On | Reduction |
|---|---:|---:|---:|---:|
| Bare | 0 | 1.64 | 1.59 | 3.4% |
| Bare | 2 | 1.71 | 1.73 | -1.1% |
| Bare | 4 | 1.73 | 1.78 | -2.9% |
| 10k | 0 | 2.02 | 1.91 | 5.5% |
| 10k | 2 | 1.89 | 1.84 | 2.6% |
| 10k | 4 | 1.95 | 1.93 | 1.2% |
| 100k | 0 | 4.97 | 4.31 | 13.4% |
| 100k | 2 | 3.36 | 3.00 | 10.7% |
| 100k | 4 | 3.12 | 2.89 | 7.4% |

100k p95 improves 5.12→4.43 / 3.50→3.13 / 3.27→3.01 ms. Small cases remain mixed and their single-digit differences overlap
control variation. Keep the bare regressions rather than claiming a universal speedup. These are fresh controls; do not
compare absolute times with older sessions/Godot or change worker defaults from them. There is no new Godot run.

At 100k, per-frame candidate averages are identical at 279,687. Cell proofs accept **76,790 copy/view pairs directly**;
individual refinement drops to **202,897 (27.5% less)**. Actual radius checks fall **279,687→21,336 (92.4% less)**.
The potential plane-test budget falls **1,217,380→377,059 (69.0% less)**. Classification payload drops by 76,790 bytes/frame,
from 279,687 to 202,897 bytes. These are deterministic work reductions; accepted main/shadow work and record/index uploads
match. Cached-sphere/scratch capacity remains 1,600,000 + 56,064 bytes in both conditions, with no warm-up rebuilds.

RAM: matrix dense peaks stay around **220.2–220.4 MiB RSS**. The full 1,440-frame four-helper tour peaks at **220.4 MiB off /
222.6 MiB on** under a 512 MiB guard. That 2.2 MiB RSS increase is retained; lower classification payload does not establish
a blanket process-memory reduction. Feature checks peak at 226.9 MiB with AO/contact and 265.8 MiB with TAA. RSS excludes GPU
allocations. Builds use one job; heavy processes remain sequential; artifacts stay on Linux disk and the snapshot is reused.

Validation: full NVIDIA Vulkan suite passes (77.5 s, 260.4 MiB RSS). Focused old/new gather tests pass, including retained
instances, cached bounds and grouped/ungrouped scenes. New independent math checks compare 120,000 sphere decisions against
all-plane/radius reference predicates, including touching boundaries, nonfinite fallback, zero radii, five/six planes and
large coordinates. New GPU fixtures force fully accepted cells, partial planes, radius-boundary/outside cells, moving lights,
moon cascades, culling disabled, masks and mirrored/nonuniform transforms; they compare exact RGBA/picking, draw/instance/
triangle counts, sun/cascade caster counts and local caster/triangle/draw counts for grouped immediate and retained copies,
serial/two-helper execution. These fixtures pass on lavapipe (221.7 MiB) and with GPU retention disabled; mixed cached-bounds
fixtures also pass on lavapipe (301.9 MiB). Existing LOD/skinning/shadow-cache tests pass in the full suite.

All 54 matrix runs match recorded rendering/upload counters, with equal candidates, no increased plane budget/classification
payload and unchanged sphere payload. Nine off/on RGBA pairs and cross-worker images match. Full-route recorded work and
exact wrap RGBA match; archived pre-change executable pixels/work also match. Separate 60-frame mist/unlit/TAA/AO-contact
pairs match exact RGBA and recorded work. CSV still lacks complete shadow caster/triangle counts; GPU tests compare them as
above. D3D12/Metal remain untested; shader sources and ABI were not changed.

The initial 99 Hz / 600-frame caller profile was sparse (183/138 measured samples), so it is archived under `profile-initial/`.
Final profiles use 499 Hz user cycles, 4 KiB DWARF stacks, no helpers and 1,200 measured frames, aligned by monotonic CSV
timestamps to exclude loading/warm-up: **1,952/1,671 off/on samples**. Local-shadow preparation falls from ~51.4% to ~43.8%
of sampled caller cycles; its candidate-gather leaf falls ~26.5%→19.2%. Main cull/list/upload preparation is ~41.2% of the
remaining sample, scene submission ~9.1%, with sun reach helpers `to_leave`/`shadow_in` ~13.4% combined. These are CPU
samples, not GPU pass timestamps, and profiling overhead is excluded from the timing sweep. Next targets: local task setup/
accepted-list packing (the main local-preparation body is still ~20.5% as a leaf), then repeated sun reach tests.

Artifacts: `$HOME/kin-bench/grove-local-cull/` contains before/after source/executable archives, build/test logs, all CSV/PNG,
commands/RSS, `summary.json`, `comparison.json`, validation records, and both final perf recordings/summaries. `run.py`
reproduces the suite/matrix/tour/final profile, `features.py` checks feature outputs, and `analyze.py` produces summaries.
`reprofile.py` records the denser final profile while preserving the initial capture. No commits were created.

### Grove — compact shadow tasks and in-place caster lists (2026-10-15)

Implemented the next Kin-side preparation step. Ordinary and clustered shadow tasks use one counter in a **32-byte**
record rather than nine counters in a 64-byte record. Only selected multi-level tasks need extra counters: a single flat
array is sized once to the exact sum of their level counts. At eight levels a task plus counters is 64 bytes, preserving
memory use even in an all-LOD workload without geometric vector-growth slack. Single-bucket packing skips empty tasks,
reduces one count instead of nine, and fills consecutive indices directly whenever every candidate passed.

The accepted-copy backing store reuses the existing upload-list vector. Cache keys and cluster cuts finish before the
list is compacted/remapped in place: source order guarantees the write cursor cannot overtake unread indices. Retained
draws keep raw batch indices; streamed draws still build the same shared caster union and skin offsets. Cached views and
empty cluster cuts remove ranges without leaving upload holes. This removes a simultaneous accepted/upload-list copy.

After visibility/LOD no longer needs the original source prefixes, those prefixes are reused for streamed copies only.
The union slot map excludes retained draws; oversized prior storage is released above twice the required slots, with a
4,096-slot floor. A TAA/retention fallback can still grow it and subsequent retained draws shrink it again. No new
per-instance persistent cache, GPU buffer layout or shader is introduced. Existing instance/sphere cache budgets stay fixed.

`KIN_LOCAL_SHADOW_PACKING=0` restores full task counters, separate copy lists and slots for every source copy, independently
of the earlier culling switch. `local_task_bytes`, `local_slot_bytes`, `local_copy_bytes` expose corresponding vector payload
capacities in stats/CSV/report. They exclude allocator overhead, other scratch and GPU storage; task and list capacities
are recorded at different preparation phases and their sum is not a process peak-memory measurement.

**Final measurements:** 54 sequential same-binary off/on runs, three alternating repeats at 0/10k/100k plants × 0/2/4 helpers.
Each has 300 warm-up / 180 measured frames, affinity 0–15, the same distance-32 Grove snapshot, PBR/IBL/HDR, four points/one
spot, sun/local shadows, no mist, 720p/FOV75. Engine `-O2` and wrapper `-O3` are unchanged; prior optimisations stay enabled.
Median run p50 frame intervals:

| Plants | Helpers | Previous packing (ms) | Compact packing (ms) | Reduction |
|---|---:|---:|---:|---:|
| bare | 0 | 1.841 | 1.603 | 12.9% |
| bare | 2 | 1.814 | 1.589 | 12.4% |
| bare | 4 | 1.882 | 1.625 | 13.7% |
| 10k | 0 | 2.299 | 1.917 | 16.6% |
| 10k | 2 | 2.034 | 1.719 | 15.5% |
| 10k | 4 | 2.107 | 1.789 | 15.1% |
| 100k | 0 | 4.768 | 4.237 | 11.1% |
| 100k | 2 | 3.525 | 2.961 | 16.0% |
| 100k | 4 | 3.203 | 2.706 | 15.5% |

All nine conditions improve in this sweep. These are fresh controls; absolute times from older sessions or Godot runs
are not comparable. There is no new Godot measurement. Profiling runs below are separate from these timing results.

At 100k plants, mean capacity counters are:

| CPU payload | Previous (KiB) | Compact (KiB) |
|---|---:|---:|
| Tasks and per-level counters | 1307.0 | 653.5 |
| Caster-union slots | 404.3 | 13.7 |
| Accepted/upload copies | 1716.6 | 1388.8 |

The sum falls by about 1.34 MiB, but **RSS does not reliably fall**. Keep the actual process peaks:

- Initial full 1,440-frame route, off: 222.6 MiB.
- Initial full 1,440-frame route, on: 238.0 MiB.
- Additional alternating full-route pair, memory-on-r1: 227.1 MiB.
- Additional alternating full-route pair, memory-off-r1: 211.4 MiB.
- Additional alternating full-route pair, memory-off-r2: 228.3 MiB.
- Additional alternating full-route pair, memory-on-r2: 227.1 MiB.

The larger parallel-process peaks remain an unresolved limitation; payload savings do not explain total allocator/driver
memory. No cause is asserted from these runs. All render benchmarks stay under a 512 MiB RSS guard, GPU tests under 1 GiB;
builds use one job, heavy runs are sequential and artifacts stay on Linux disk. RSS excludes GPU allocations.

Validation: full NVIDIA Vulkan suite passes. A new mixed GPU fixture compares exact RGBA and main/sun/local work across
old/new packing and serial/two-helper execution, mixing retained and streamed draws, selected multi-level meshes, cluster
cuts, full/partial shadow-cache reuse, a forced TAA shadow upload, batch edits and post-fallback slot shrinkage. It also
passes on lavapipe, with retention disabled, without spatial cells and with original local refinement. Existing skinning,
LOD and cache tests pass in the full suite. All 54 matrix runs match every recorded work/upload/culling counter; nine
off/on RGBA pairs and cross-worker images match. Full-tour work/wrap RGBA and archived pre-stage executable work/pixels
match. Separate mist/unlit/TAA/AO-contact pairs also match exact images/work. D3D12/Metal remain untested.

Final off profile: 2188 measured samples; local-shadow preparation 42.8%, main cull/list/upload 42.0%.
Final on profile: 1885 measured samples; local-shadow preparation 36.0%, main cull/list/upload 47.0%.
Profiles use 499 Hz user cycles, 4 KiB DWARF stacks, no helpers and 1,200 measured frames, aligned to CSV monotonic
intervals to exclude loading/warm-up. These are CPU samples, not GPU timestamps. Next candidates are repeated sun/moon
reach calculations (`to_leave`/`shadow_in`) and camera/sun classification; total RSS deserves further investigation.

Artifacts: `$HOME/kin-bench/grove-shadow-packing/` contains before/after source/executable hashes, final CSV/PNG, commands/RSS,
validation records, `comparison.json`, and perf recordings/summaries. `final.py` reproduces validation, timing, feature and
additional memory sweeps after a single-job build; `analyze.py` summarizes results. `initial/` preserves the first iteration
before exact multi-level counter sizing; `measured/` preserves that executable. No commits were created.

### Grove — upload staging residency investigation (2026-10-15)

The earlier RSS difference came with whole 16 MiB driver mappings. Initial `/proc/PID/smaps` probes of the archived
packing versions found both reaching three resident 16 MiB `/dev/nvidiactl` mappings; resident heap differed by less
and the known engine payload counters had decreased. Local SDL Vulkan source confirms transfer buffers use dedicated
allocations and cycling retains additional backings while commands use them. This points to transfer staging residency
and timing rather than an added 16 MiB per-instance cache. Some whole-process peaks occur during final PNG capture,
after the measured route; runtime RAM and process high-water RSS are therefore reported separately.

Implemented device-wide adaptive shared staging. It starts at the original 16 MiB loading floor, records maximum **total
bytes per submitted upload batch** in 120-batch windows, and releases the current transfer buffer when the rounded working
set needs at most half its capacity (minimum 1 MiB). The next upload creates that capacity; large writes grow it immediately.
Aggregate occupancy preserves space for batches made from many small writes. The original grow/shrink behavior remains
available with `KIN_ADAPTIVE_STAGING=0`. SDL defers buffer disposal until submitted copies finish; there is no new GPU wait,
instance cache or shader/layout change. No-upload idle frames do not advance the window. Active large uploads keep large
buffers; at worst a burst takes two subsequent windows to age out of the observed maximum.

New `RendererBackendStats::upload_staging_bytes` is current single-handle capacity (zero after release until the next
allocation); `upload_staging_allocations` is cumulative since device creation. These exclude cycle backings, retired buffers
and driver pools. Blockfield CSV adds these plus cumulative `upload_count` and `upload_batches`. They separate live logical
capacity from actual residency; no total-RAM claim is made from a capacity counter alone.

**Untraced measurements:** 24 sequential full-route runs, 0/100k plants × 0/4 helpers × off/on × three alternating repeats.
Each has 300 warm-up / 1,440 measured frames, affinity 0–15, the same distance-32 snapshot, 720p/FOV75, PBR/IBL/HDR, sun/local
shadows, four points/one spot, no mist, prior optimisations enabled, engine `-O2` and wrapper `-O3`. Timings below are medians
of run p50 values; RSS is the maximum process peak across the three runs. Only repeat 1 includes the end-of-run PNG.

| Plants | Helpers | Fixed p50 (ms) | Adaptive p50 (ms) | Fixed peak RSS (MiB) | Adaptive peak RSS (MiB) |
|---|---:|---:|---:|---:|---:|
| bare | 0 | 2.035 | 2.016 | 208.0 | 180.7 |
| bare | 4 | 2.118 | 2.070 | 208.2 | 181.0 |
| 100k | 0 | 4.769 | 4.696 | 218.7 | 206.8 |
| 100k | 4 | 3.082 | 2.989 | 225.9 | 206.7 |

The median timing differences are small and paired runs have mixed signs with substantial control variation; this is
a memory improvement, not an established speedup. Do not compare absolute times with older sessions or Godot. Every
measured plain-Grove frame uses 16 MiB fixed / **1 MiB adaptive** staging, with no further staging allocations during the
measured tour (1 lifetime allocation fixed, 3/4 adaptive at bare/100k). Both submit **one upload batch per measured frame**.
Rendering, upload requests/record bytes, candidates and all other recorded work counters match. Cumulative batch totals
can differ from loading, and are deliberately distinguished from per-frame measured batch counts.

**Separate mapping probes:** two balanced off/on pairs at 100k plants/four helpers, 1,440 measured frames, sampling smaps
every 100 ms and aligning with CSV monotonic timestamps. These probes are excluded from timing results.

| Probe | Measured RSS median (MiB) | Measured device-mapping RSS median (MiB) | Whole sampled peak (MiB) |
|---|---:|---:|---:|
| staging-trace-off-r1 | 205.8 | 55.7 | 218.6 |
| staging-trace-off-r2 | 225.4 | 71.7 | 236.7 |
| staging-trace-on-r1 | 176.1 | 25.7 | 205.2 |
| staging-trace-on-r2 | 175.8 | 25.7 | 204.9 |

Steady RSS falls by about **30–50 MiB**, with resident device mappings falling by **30–46 MiB**. At matched low-residency
controls, resident heap remains about 30.8 MiB while device mappings fall 55.7→25.7 MiB. The old path can retain a third
16 MiB backing; the new small-upload path uses 1 MiB backings. Whole-process peaks remain higher than steady RSS because
loading, cold uploads and PNG readback/encoding have different allocations. These are CPU-resident mappings; RSS excludes
other GPU/VRAM allocations and counts shared library pages too.

Validation: full 2D suite passes (17.5 s, 234.0 MiB RSS) and full NVIDIA 3D suite passes (88.4 s, 264.0 MiB). The new GPU test
checks shrinkage, subsequent 4 MiB regrowth, retained texture pixels across retirement, and three 512 KiB writes per batch
requiring 2 MiB rather than 1 MiB. It passes with the old policy and on lavapipe. All 24 route runs match recorded work;
four paired PNGs and cross-worker images match. The archived pre-change executable also matches work/PNG. Mist, unlit,
TAA and AO/contact feature pairs match exact images/work. Default renderer behavior and GPU-side selection remain the same.

TAA is a limit to the result: its ~18 MiB/frame of instance uploads keeps 16 MiB staging and two batches/frame in both
policies. The first 60-frame TAA feature pair peaked at **264.1→284.2 MiB**, which is retained rather than hidden. Two extra
balanced 600-frame TAA mapping pairs did not reproduce that increase (whole sampled peaks ~262.1/262.2 MiB fixed versus
~260.6 MiB adaptive); exact images/work match, capacity stays 16 MiB, and no TAA memory reduction is claimed. There is no
universal process-peak guarantee from this policy. D3D12/Metal remain untested.

Builds use one job, render benchmarks a 512 MiB guard, and GPU suites a 1 GiB guard. Heavy processes remain sequential.
An ENOSPC interrupted the first source write; both touched files were restored from pre-stage copies before reapplying
the change, and the final build/tests pass. Generated `build/` (about 1.1 GiB) moved to
`/home/bamedeo/kin-build/3d-rendering`, with the worktree path preserved by a locally ignored symlink. This frees space on
the Windows volume and keeps compiler outputs on Linux storage. Sources and unrelated work remain in the original worktree.

Artifacts: `$HOME/kin-bench/grove-rss/` contains before/after sources/executable hashes, commands/RSS, full CSV/PNG, raw mapping
probes, `comparison.json`, `memory-phases.json` and validation records. `run.py` reproduces the suites/matrix/default probes,
`features.py` the feature checks, `taa_trace.py` the TAA follow-up, and `analyze.py` the summaries. No commits were created.
Next CPU candidate remains repeated sun/moon reach calculations, around 15% of the preceding sampled CPU profile.

### Grove — directional shadow reach (2026-10-15)

Implemented private engine CPU sun/moon reach refinement. Ray exit faces/active axes are prepared outside candidate
loops; swept endpoints are computed only when a camera/slice plane rejects a sphere's start and reused across sun
cascades and CPU cluster tests. Spatial cells carry pending camera/slice plane masks. An outside starting cell is never
rejected on that basis alone: its shadows may enter view. Plane order, coefficients, float divisions and min/max order
match the original arithmetic; reciprocal multiplication is deliberately avoided. CPU clusters use full frustums because
cluster spheres need not fit inside the whole-mesh sphere used for instance-cell bounds. No shader/GPU record changes.

The main loop compiles original and prepared forms separately. Prepared sun reach is selected for spatial groups and
multiple cascades. Ordinary single-map draws and scenes without sun shadows use the original loop. This choice matters:
the initial generic loop slowed unlit/small-draw cases; merely guarding the sweep object was insufficient. Separate
compilation and selective dispatch avoid imposing the helper/state cost on those cases. `KIN_SHADOW_REACH=0` before
renderer creation restores eager directional reach, independently of local culling/packing controls.

Memory cost is eight bytes per allocated spatial cell (512 bytes for Grove's 64 live cells), with small worker-stack
subsets/rays rather than another per-instance cache. Radius/accept flags share one byte so a moon mask fits in the
existing **32-byte compact local task**, retaining the 64-byte reference task too. All recorded bounds/local-task/slot/
copy payload capacities, upload traffic and culling/work counters match. `local_plane_budget` still covers light frustums,
not directional camera/slice reach, and is not an executed-plane counter. Camera-culling-off behavior is preserved.

**Final short stress segment:** 24 sequential runs: bare/100k plants × 0/4 helpers × off/on × three alternating repeats,
300 warm-up / 180 measured frames, 720p/FOV75, distance-32 snapshot and Grove dressing, affinity 0–15, no mist,
PBR/IBL/HDR, sun/local shadows, four points/one spot. Prior optimisations/adaptive staging remain enabled. Engine `-O2`,
wrapper `-O3`, NVIDIA RTX 4080 Laptop Vulkan. Values are medians of run p50/p95; RSS is maximum process peak. Grove has
one sun map and no moon: cascade/moon correctness is tested separately, not a measured moon speedup.

| Plants | Helpers | Eager p50 | Prepared p50 | Eager p95 | Prepared p95 | Peak RSS eager/prepared (MiB) | Paired p50 reductions |
|---|---:|---:|---:|---:|---:|---:|---|
| bare | 0 | 1.673 | 1.609 | 1.765 | 1.716 | 183.2/181.4 | -1.8%, +1.7%, +9.3% |
| bare | 4 | 1.678 | 1.654 | 1.851 | 1.780 | 181.4/181.4 | -1.1%, -5.4%, +2.8% |
| 100k | 0 | 4.307 | 4.221 | 4.493 | 4.599 | 207.5/207.5 | +1.2%, +13.0%, -0.1% |
| 100k | 4 | 2.679 | 2.715 | 2.918 | 2.843 | 209.6/209.8 | +5.2%, -1.3%, -2.8% |

Milliseconds per frame; positive paired reductions mean faster. Paired signs are mixed, with large host drift. Bare
scenes select exactly the same original loop with either flag, illustrating the noise floor. Unpaired median differences
must not be treated as reliable speedups. Absolute timings cannot be compared with previous sessions/Godot runs.

**Full 1,440-frame tour:** three additional alternating pairs at zero/four helpers. The initial four-helper route pair
from the final executable is also included (four pairs total), rather than dropping an adverse result. No perf/smaps
tracing in these timing runs. Table entries are medians of run p50/p95; reduction is median of paired p50 reductions.

| Helpers | Pairs | Eager p50 | Prepared p50 | Eager p95 | Prepared p95 | Median paired reduction | Peak RSS eager/prepared (MiB) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | 3 | 4.257 | 4.039 | 4.813 | 4.577 | 5.1% | 207.9/207.5 |
| 4 | 4 | 2.855 | 2.855 | 3.147 | 3.142 | -0.4% | 207.3/207.6 |

Full-tour paired reductions: 0 helpers: +1.7%, +5.1%, +9.7%; 4 helpers: -2.7%, +0.5%, -0.1%, -0.7%.
End-to-end frame results remain workload- and host-dependent; this is not a universal FPS improvement. Dense plain
Grove stays around 207–210 MiB RSS. All measured full-tour frames retain 1 MiB staging without reallocating it, preserving
the preceding staging benefit. No new total-RAM saving is claimed; TAA still requires large staging.

**Main-thread CPU profile:** same-binary off/on, 499 Hz user cycles, 4 KiB DWARF stacks, no-inherit, zero helpers,
2,400 measured frames aligned with CSV monotonic timestamps to exclude loading/warm-up. Reach falls from
14.97→12.81% of sampled cycles; weighted reach periods fall about 16.5%,
while total sampled periods fall about 2.4%. Process render CPU averages
3.967→3.808 ms (4.0% lower); frame averages
4.360→4.219 ms. These are CPU samples/times, not GPU
pass timestamps or exact per-function timers. The remaining local-shadow preparation is
37.3% inclusive, with candidate gathering 22.9% as a leading
leaf. Main cull/upload is 45.0% inclusive and scene submission
10.0%. Next CPU candidate: gather several cached spheres at a time without
another bounds cache. Pass-level GPU timing would help explain why CPU reductions translate unevenly to frame rate.

Validation: independent eager-reference tests compare 160,000 enclosed spheres, bit-exact exit distances, full/reduced
plane predicates, endpoint reuse, touching/nextafter boundaries, large coordinates, near-zero/zero directions and
nonfinite inputs. GPU cell-range fixtures compare original/prepared paths in fresh renderers, ungrouped/grouped/
retained inputs, serial/two helpers, single/four sun maps, single/three moon maps, moving camera/lights, mask/mirrored
instances and camera-culling-off frames. RGBA, picks and main/cascade/local caster/triangle/draw counts match.
The final full NVIDIA 3D suite passes (83.2 s, 259.8 MiB); focused comparisons pass on lavapipe
(264.4 MiB), without GPU instance retention, without spatial cells and with reference local culling/packing.
The full suite covers CPU/GPU cluster cuts, skin, LOD, cache and TAA paths. All 24 short runs and seven full-tour pairs
match every recorded non-timing column. Four paired short PNGs, worker images, route PNGs and archived pre-change
executable image/work match exactly. Mist, unlit, TAA and AO/contact feature images/work match too. D3D12/Metal untested.

Archived pre-change timing controls (three alternating pairs per case, same final binary as above):

- dense, zero helpers: paired p50 reductions +4.8%, -1.2%, +2.0%.
- unlit, zero helpers: paired p50 reductions -1.4%, -0.6%, +3.9%.

These expose common-path overhead and drift rather than replacing same-binary controls. Earlier generic, guarded and
unconditionally prepared sun-loop versions/measurements are preserved under `initial/`, `gated/`, `split/`; their results
are excluded from the final tables. The completed earlier matrix included 10k plants/two helpers too, but the final
measurement focuses on bare/dense controls with zero/four helpers.

The initial 60-frame AO/contact pair peaked at 209.1→229.7 MiB, an adverse outlier.
Two further balanced 600-frame pairs had peaks: off r1: 209.3 MiB, on r1: 209.2 MiB, on r2: 209.3 MiB, off r2: 209.2 MiB.
They match exact RGBA/work and do not reproduce the increase; no universal process-peak RAM guarantee is claimed.

One heavy process at a time, one-job builds, 512 MiB benchmark/1 GiB GPU-suite guards. Artifacts:
`$HOME/kin-bench/grove-reach/` contains before/after source/executable hashes, commands/RSS, CSV/PNG, full-tour and archived
comparisons, perf recordings/summaries and validation. `run.py`, `routes.py`, `features.py`, `archived_pairs.py`, `profile.py`,
`analyze.py` and `profile_analyze.py` reproduce the checks after a one-job build. No commits created.

### Grove — four-sphere local-shadow tests (2026-10-15)

Implemented private engine batching of local-shadow candidate tests. Large ranges load four existing 16-byte world-bound
records directly, transpose into SIMD registers, and test the radius and unresolved light-frustum planes together.
Plane/radius broadcasts are prepared once per task on the stack. Rejection uses the same ordered comparisons and
left-to-right float operations; touching and unordered inputs preserve scalar decisions. There is no reciprocal,
approximate geometry or changed sphere size. Empty lane sets stop early in the geometric test. Single-bucket survivors
are counted/classified in groups; multi-LOD and culled moon ranges process surviving bits in ascending source order,
retaining moon reach, cluster refinement and cache keys. Fully accepted cell ranges retain their existing shortcut.

`KIN_LOCAL_SHADOW_SIMD=0` before renderer creation restores scalar gathering, independently of local-culling/packing and
sun/moon reach controls. Ranges below 16 candidates use the scalar loop; final one-to-three candidates form its scalar
tail. Native support requires SSE2 with scalar SSE math, GCC/Clang, and no FMA/fast-math build flags. Other builds,
including MSVC, retain the scalar gather. This does not raise the existing build's ISA requirement. The native and scalar
loops are compiled separately to keep SIMD setup out of small terrain tasks.

Memory: no heap buffer, persistent bounds layout, per-copy cache or new culling metadata. Cached and streamed/scratch
spheres share the same helper. The prepared native test is **464 bytes** on this x86-64 build, temporary per active task;
portable fallback is 40 bytes (verified by standalone builds). One enable flag is added to the renderer. Existing compact/
reference tasks remain **32/64 bytes**, and all recorded bounds/task/slot/copy vector capacities and upload traffic match.
`local_plane_budget` remains the potential scalar-plane count rather than executed vector operations.

**Short segment:** 24 sequential runs, bare/100k plants × 0/4 helpers × off/on × three alternating repeats, 300 warm-up /
180 measured frames, 720p/FOV75, distance-32 snapshot and Grove dressing, affinity 0–15, PBR/IBL/HDR, sun/local shadows,
four point lights/one spot, no mist, previous optimisations enabled. Engine `-O2`, wrapper `-O3`; NVIDIA RTX 4080 Laptop,
Linux Vulkan. Table values are medians of run p50/p95 in milliseconds; RSS is maximum whole-process peak.

| Plants | Helpers | Scalar p50 | Four-sphere p50 | Scalar p95 | Four-sphere p95 | Peak RSS scalar/four (MiB) | Paired p50 reductions |
|---|---:|---:|---:|---:|---:|---:|---|
| bare | 0 | 2.294 | 2.194 | 2.407 | 2.359 | 183.3/181.3 | -6.1%, +2.5%, +4.3% |
| bare | 4 | 2.350 | 2.129 | 2.514 | 2.419 | 182.5/182.8 | +1.8%, +11.9%, +5.3% |
| 100k | 0 | 5.130 | 4.553 | 5.766 | 5.095 | 209.6/210.6 | +11.2%, +10.8%, +10.2% |
| 100k | 4 | 3.445 | 3.409 | 3.905 | 3.920 | 226.7/226.6 | -1.6%, +1.1%, +2.7% |

Positive reductions mean faster. Dense serial pairs improve consistently, while four-helper gains are much smaller.
Bare scenes still use scalar single-copy ranges; their apparent gains and adverse pairs expose host timing variation,
not a SIMD gain. Absolute timings must not be compared with previous sessions or Godot measurements.

**Full 1,440-frame tour:** three alternating pairs at zero/four helpers, plus the initial four-helper full-route pair
from the same final executable. No perf/mapping tracing during these runs. P50/p95 are medians of run percentiles;
reduction is the median of paired p50 reductions. Individual paired results are retained, including adverse runs.

| Helpers | Pairs | Scalar p50 | Four-sphere p50 | Scalar p95 | Four-sphere p95 | Median paired reduction | Peak RSS scalar/four (MiB) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0 | 3 | 5.535 | 4.968 | 6.262 | 6.041 | 11.5% | 210.6/209.7 |
| 4 | 4 | 3.716 | 3.671 | 5.156 | 4.591 | 0.9% | 226.8/226.9 |

Paired full-tour reductions: 0 helpers: +13.2%, +7.7%, +11.5%; 4 helpers: -0.3%, +1.6%, +1.8%, +0.2%.
Every measured plain full-tour frame retains 1 MiB staging without further staging allocations. Matched peak RSS is
similar; some runs in both versions reach ~227 MiB, while other serial runs are ~210 MiB. Driver/loading/readback
residency makes process high-water RSS variable, so no RAM reduction or universal peak guarantee is claimed. The previous
staging policy and large-upload/TAA limits remain unchanged.

**Main-thread profile:** same-binary scalar/four-sphere, zero helpers, 499 Hz user cycles, no-inherit, 4 KiB DWARF
stacks, 2,400 measured frames, aligned to CSV monotonic timestamps to exclude warm-up/loading. Gathering accounts for
20.90→15.97% of sampled cycles. Weighted gather periods fall
36.9%, but total periods and unchanged
caller phases also shift; these are sampled costs, not exact timers or a clean attribution of the entire speedup.
Process render CPU averages 5.655→4.579 ms (19.0% lower) in this profile pair;
frame averages 6.165→5.212 ms. Host drift is substantial,
so repeated untraced route pairs are the primary frame-rate evidence. Four helpers retain limited end-to-end gains.
The remaining main cull/upload path is 47.9% inclusive, local-shadow
preparation 32.4%, and scene submission
11.5%. A next CPU candidate is range acceptance for fully visible
single-level camera cells, reusing existing proofs/cache rather than allocating another per-instance array.

Validation: an independent scalar reference checks **400,000 lanes per build**, zero-to-six planes, optional radius
checks, mixed/full/empty masks, deliberately unaligned records, zero radii, exact/nextafter boundaries, large/overflowing
coordinates, NaNs and infinities. Native, forced-scalar and FMA-enabled scalar-fallback builds pass; static assertions
verify which implementation each build selects. GPU cell tests cross scalar/four-sphere with eager/prepared directional
reach, ungrouped/grouped/retained data, 701/702/703-copy cells (all scalar-tail lengths), serial/two helpers, single/four
sun and single/three moon maps, moving camera/lights, masks/mirrors and camera-culling-off frames. RGBA, picks, main/local/
cascade caster/triangle/draw counts match. The packing test now uses 21-copy LOD and cluster batches, checking scalar and
SIMD in full/compact packing, workers, full/partial cached maps, TAA, edits and retained-slot shrinkage.
Full NVIDIA 3D suite passes (96.6 s, 262.0 MiB). Both focused GPU fixtures pass on lavapipe
(up to 379.4 MiB), without retained instances, without spatial groups and with reference local culling/packing.
All 24 timing runs and seven full-tour pairs match all recorded non-timing columns; four paired short PNGs, worker
images, full-tour PNGs, and the archived pre-change executable image/work match exactly. Mist, unlit, TAA and AO/contact
feature images/work also match. Other GPU backends remain untested.

Archived pre-change timing checks, three alternating pairs per case, zero helpers:

- dense: paired p50 reductions -9.1%, -1.8%, +3.5%.
- unlit: paired p50 reductions +4.8%, -6.2%, -10.7%.

The unlit path has no local views, and its mixed results are not evidence of SIMD benefit or regression. In its last
adverse pair the unchanged scene-build time rises much more than render CPU time, consistent with substantial external
variation; retain these controls rather than claiming all workloads improve.

Feature process peaks (first paired screenshots; capacity/work are equal):

| Feature | Scalar RSS (MiB) | Four-sphere RSS (MiB) |
|---|---:|---:|
| mist | 213.4 | 228.6 |
| unlit | 201.6 | 220.8 |
| taa | 284.3 | 284.5 |
| ao | 229.7 | 212.1 |

The AO peak variation is not counted as an engine allocation saving; no instance payload or buffer-capacity reduction
was made. Builds use one job, benchmarks a 512 MiB RSS guard, GPU suites 1 GiB, and heavy processes stay sequential.
Artifacts: `$HOME/kin-bench/grove-simd/` holds before/after source/executable hashes, commands/RSS, CSV/PNG, profiles,
full-route/archived comparisons and validation. `fallback.py`, `run.py`, `profile.py`, `routes.py`, `features.py`,
`archived_pairs.py`, `analyze.py` and `profile_analyze.py` reproduce validation and measurement after a one-job build.
No commits created.

#### Grove — camera range acceptance and persistent comparison commands (2026-10-15)

The fresh idle-GPU comparison measured Kin 2.339 ms, Godot Mobile 1.959 ms and Forward+ 1.202 ms median over one
1,440-frame 100k-plant route each at 720p. Kin used four helpers. A subsequent CPU profile reproduced 2.307 ms/frame:
scene submission 0.393 ms CPU, renderer 1.789 ms main-thread CPU / 3.648 ms summed process CPU. Main-thread cycle samples
were 36.3% cull/upload preparation, 32.9% local-shadow preparation and 17.4% scene submission. Worker call stacks were
unavailable in this sample; these shares describe the main thread only. Kin's fence-based frame metric is not a hardware
GPU timestamp and cannot be added to CPU time or compared directly with Godot's GPU measurements.

**Engine change:** camera-inside spatial pieces classify single-level, non-skinned, non-clustered copies together when
no sun-cascade membership or uncached local-shadow sphere is required. Existing visibility bytes are filled in one range,
and the piece's existing count is set directly. Fully visible single-level pieces fill retained indices consecutively,
or pack streamed records without per-copy visibility branches. Partial/offscreen cells, LOD, clusters, skinning,
local bounds, cascade lists, TAA ordering and alpha sorting retain their required paths. No per-instance storage or
larger piece/task record is added. `KIN_CAMERA_RANGES=0` restores the old loops. `camera_range_accepted` counts copies
classified together (17,182–23,441 per dense tour frame, median 20,306); packing alone is not included.
`bounds_instance_tests` still counts actual sphere transformations and can decrease for streamed accepted cells.

Engine-only full-route pairs, still rebuilding scene commands each frame, three alternating pairs per helper setting:

| Helpers | Off p50 / p95 (ms) | On p50 / p95 (ms) | Median paired reduction | Peak RSS off/on (MiB) |
|---|---:|---:|---:|---:|
| 4 | 2.320 / 2.599 | 2.309 / 2.609 | -0.4% | 223.2 / 223.1 |
| 0 | 3.448 / 4.081 | 3.416 / 3.818 | +0.9% | 224.1 / 203.8 |

Pair reductions: four helpers -0.4%, +2.0%, -3.3%; zero helpers -5.2%, +9.6%, +0.9%.
These do **not** establish a reliable frame-rate improvement from range acceptance alone. Differences in RSS high-water
marks are not treated as allocation savings; payload capacities and rendered work match.

**Sample/API-use change:** the comparison harness now keeps its static terrain/foliage/mist commands in the existing
`Scene3D`, updating camera and the animated point/spot rig each frame. Previously it cleared/resubmitted 3,504 terrain
chunks every frame, while the Godot sample retained its nodes. This requires no new engine caching API, extra scene copy,
or extra vector payload capacity. `--rebuild-scene` restores the original workload; reports expose `static_scene_commands`.
This also applies to flight and tiled-triangle comparison modes, whose geometry/materials are static.

Combined comparison (old engine loops plus rebuilding versus range acceptance plus persistent commands), three alternating
1,440-frame pairs per setting, 300 warm-up frames, 100k plants / no mist, same 720p light/shadow recipe and CPU affinity:

| Helpers | Before p50 / p95 (ms) | After p50 / p95 (ms) | Median paired reduction | Scene CPU p50 before/after (ms) | Peak RSS before/after (MiB) |
|---|---:|---:|---:|---:|---:|
| 4 | 2.326 / 2.613 | 2.352 / 2.606 | -1.1% | 0.405 / 0.00138 | 206.0 / 222.9 |
| 0 | 3.442 / 3.815 | 2.934 / 3.350 | +9.6% | 0.360 / 0.00061 | 203.7 / 203.7 |

The p50/p95 entries are medians of run percentiles; paired reductions are calculated within each pair, so percentages
need not equal the ratio of table medians. Four-helper pairs: -1.1%, -1.2%, -0.2%; zero-helper pairs: +9.1%, +15.8%, +9.6%.
The scene submission saving is clear, and zero-helper frames improve consistently in these pairs. Four-helper throughput
is effectively flat/slightly slower despite ~0.4 ms less CPU work, consistent with GPU/completion throughput becoming the
limit. This is not proof of a specific slow GPU pass: hardware pass timestamps are still needed. Four-helper process RSS
is ~17 MiB higher in this combined set, despite unchanged recorded payload capacities; report it rather than claiming
unchanged process RAM. All paired full-route timing peaks remain below 225 MiB, with a 512 MiB process guard and no concurrent heavy job.

Validation and reproduction: `$HOME/kin-bench/grove-camera-ranges/` holds the archived executable/source, engine-only
pairs, combined pairs under `static/`, commands/RSS, CSV/PNG, GPU telemetry and scripts. Full-route and archived executable
checks compare all shared non-timing work/upload/capacity columns (except actual bounds-work and new accepted-range counts)
and decoded RGBA. A deliberate mutation replacing consecutive retained indices with repeated first indices is caught by
the image/picking fixture; production source is restored and both targets rebuilt afterwards.

Final validation: full NVIDIA 3D suite passes (90.2 s, 258.6 MiB). Extended cell fixtures cross range off/on with
scalar/four-sphere and eager/prepared reach, streamed/retained input, serial/two helpers, masks, mirrored transforms,
picking and single/multiple sun/moon maps. Packing fixtures cross range off/on with full/compact local preparation,
LOD/cluster buckets, workers, cache reuse, edits and retained→TAA→retained transitions. A streamed-cell case with no local
shadow bounds checks actual transformed-bound reduction. Focused lavapipe fixtures pass (up to 386.8 MiB); streamed,
uncached, spatial-off and range-off controls pass. Paired mist, TAA, AO/contact, unlit, flight and 5x tiled-triangle
images/work match, as does the final archived-executable comparison. No new shader or 2D pipeline changes; no commits.

#### Grove — external GPU pass profile (2026-10-15)

Added optional `KIN_GPU_PASS_MARKERS=1` labels around the 3D render stages. SDL debug groups remain in the existing
command buffer, with balanced stack guards and no internal timestamp query pools, readbacks, GPU waits or submission
splits. Markers are off by default. Comparison `--visible` opens an uncapped window while preserving the usual 300-frame
warm-up and frame-indexed route; it stops from the scene's own render count so the CSV has exactly `--measured` rows.
Hidden mode remains the throughput protocol. Visible/compositor/profiler intervals are not substituted for hidden times.

Tool: installed Nsight Systems 2025.1.3, Vulkan annotations + individual GPU workload tracing, CPU sampling/backtraces/
context-switch tracing off. The first hidden probe produced no usable GPU workloads: without presents, Nsight exhausted
its 8,192-timestamp per-frame budget. Visible captures provide frame boundaries and contain GPU workloads/labels without
that overflow. Nsight warns that installed Vulkan 1.4 is newer than its supported 1.3 tracing; the recorded core submit/
label paths are usable, but this is not a general compatibility claim.

**Selection:** join `VULKAN_DEBUG_API` label correlation IDs to `VULKAN_WORKLOAD`, using CPU marker ordinals as native frame
indices. Require all named ranges for each frame, exclude loading and frames <=300. The full 1,440-row dense tour yields
1,433 complete measured GPU frames; Nsight does not export seven buffered tail frames. Controls yield 293 complete frames
from 300 measured rows. Their baseline uses the same first 293 measured camera frames. Do not assume the nth GPU marker
is the nth native frame without checking the CPU correlation; tail samples can be absent.

Full-tour means, hardware timestamps:

| GPU interval | Mean (ms) | Approximate share of render submission |
|---|---:|---:|
| Sun shadow marker | 0.0258 | 1.4% |
| Local-shadow marker | 0.8264 | 44.8% |
| Main start through submission completion (including output and final 2D/copy) | 0.9926 | 53.8% |
| Enclosing render submission | 1.8453 | 100% |

The enclosing submission includes the final copy/presentation work but not earlier separate upload submissions, CPU
preparation or all application pacing. It is not a second cost to add to contained spans. Marked 3D span alone is 1.8182 ms.
The main marker alone is 0.3391 ms and output alone 0.6263 ms, but **these are pipeline spans, not isolated shader costs**.
The output pass's dependency waits for earlier foliage fragments. Omitting HDR moves ~0.6 ms into the later 2D copy while
the enclosing submission remains the same, so changing tone mapping is not supported as the main optimisation target.

Matched initial-route controls (single captures; means over 293 complete GPU frames):

| Recipe | Enclosing GPU submission (ms) | Local marker (ms) | Main through completion (ms) | Submission change |
|---|---:|---:|---:|---:|
| 100k / 1024 shadows / PBR / HDR | 1.8545 | 0.8879 | 0.9384 | baseline |
| Local shadows off | 0.7729 | <0.0001 | 0.7479 | -58.3% |
| Terrain PBR→lit; plants unchanged | 1.8222 | 0.8817 | 0.9126 | -1.7% |
| HDR off | 1.8624 | 0.8918 | 0.9422 | +0.4% |
| Plants removed | 0.5349 | 0.1991 | 0.3053 | -71.2% |
| Shadow resolution 1024→512 | 1.4583 | 0.6098 | 0.8256 | -21.4% |

These are diagnostic recipe changes, **not equal-quality speedups or additive savings**. Disabling local shadows removes
map generation and receiving-side filtering; removing plants changes geometry/coverage as well as shading. The 512 setting
reduces shadow quality and changes both sun/local resolution. Resolution sensitivity points to a significant raster/
filtering component, but the remaining cost is not separated into vertex, draw-front-end and mask-fragment costs by this
trace. GPU clocks and profiling overhead vary; small control differences such as terrain-lit/HDR are not reliable wins.
The full-tour captures did not retain their periodic NVML log; the final shadow-512 capture has a separate telemetry file.

**Next measured candidates:** an optional masked-foliage depth prepass that populates the actual main depth target,
followed by shading against that depth, to test wasted cutout/overlapping-fragment work; then a foliage material variant
specialised for its constant roughness/no material-map features, retaining its lighting/shadows. Neither is implemented
or proven by this profile. Separately evaluate local-shadow resolution/distance budgets as an explicit quality/performance
setting and match effective map texel budgets before making further Godot quality/performance claims. More CPU-only cell
work and simplifying the terrain material have weak evidence as the next route-throughput targets.

**Memory/validation:** external captures are sequential, on Linux-disk scratch. Target processes have their own 512 MiB
RSS guard and 1 GiB free-memory stop; the outer Nsight session guard is 1.5 GiB. Nsight reparents processes, so its ~47 MiB
wrapper-tree peak excludes the actual game; target peaks are measured separately at 214–239 MiB. Exports are guarded at
1 GiB; native test fixtures at 1 GiB and lavapipe peaks at 384 MiB. Hidden markers off/on full-tour images and every recorded
non-timing work/capacity counter match; combined TAA/AO/contact/mist and archived-executable images/work also match.
Marker-enabled native packing, spatial groups, GPU occlusion and overlay fixtures pass, as does focused lavapipe packing.
Untraced hidden marker off/on p50 is 2.302/2.250 ms in one pair: this is a compatibility check, not an optimisation claim.
No rendering quality default or shader is changed, and no commits are created.

Artifacts: `$HOME/kin-bench/grove-gpu-profile/` contains `.nsys-rep`/SQLite captures, bounded target commands/RSS, CPU/GPU
CSV, correlation-based summaries, control comparison, images, validation and archived source/executable. `captures.py`,
`capture_shadow512.py`, `profile_target.py`, `analyze.py` and `validate.py` reproduce the experiment. Source docs describe
capture commands; no additional profiler download or Vulkan-specific native-handle bridge was needed.

#### Grove — optional masked main-depth prepass (2026-10-15)

The GPU profile motivated a depth-only cutout experiment, rather than changing tone mapping. With
`KIN_MASKED_DEPTH_PREPASS=1` before renderer creation, eligible stock lit masks first draw into the **existing main depth**
using the same ordinary/retained vertex shader as shading. A small new depth fragment samples base alpha and applies the
same tint and `max(alpha_cutoff, 1e-6)` cutoff. Main rendering loads that depth with `cycle=false`, uses depth-test-only
pipelines for prepassed cutouts, and leaves other draws' depth writes and source order intact. Preflight makes every
required depth pipeline before clearing the target. Views without eligible draws still clear depth normally.

No new target, per-instance cache, or copy list is added. One optional shader, cached pipeline variants and a renderer
flag are the new resources. The pass adds one draw over 29,689–33,716 visible plant copies per dense route frame (median
30,886.5), tracked separately as `masked_depth_draws` / `masked_depth_instances`; original main/shadow work counters keep
their meaning. `kin3d.masked_depth` labels the pass. `warm_up` prepares ordinary/retained depth and read-only variants.
All 94 pre-existing SPIR-V files remain byte-identical; only `mesh_depth_mask.frag.spv` is added.

**Boundaries:** off by default. Skip the view with TAA, MSAA, an overlay, active GPU occlusion or any submitted custom
shader. Skip unlit/wireframe, skinned, multi-level and clustered draws. These conservative boundaries preserve their
existing ordering/velocity/depth paths while testing the plain Grove workload. The fixture also covers orthographic
projection, repeat/linear sampling, vertex/tint alpha, zero/negative/exact-cutoff values, overlapping/mirrored cards,
opaque foreground/background, empty subsequent frames, HDR, picking and serial/two-helper/streamed input.

Untraced timing: 100k plants / no mist, 720p, shared terrain/route/light rig, 300 warm-up + 1,440 measured frames;
three alternating same-binary off/on pairs for each helper count, one heavy process at a time:

| Helpers | Off p50 / p95 (ms) | On p50 / p95 (ms) | Median paired frame-time reduction | Peak RSS off/on (MiB) |
|---|---:|---:|---:|---:|
| 4 | 2.296 / 2.556 | 1.842 / 2.057 | **19.7%** | 226.0 / 225.4 |
| 0 | 2.789 / 3.117 | 2.786 / 3.125 | **-3.3%** | 206.0 / 205.9 |

The percentile entries are medians of run percentiles; paired reductions use each pair's ratio and need not equal the
ratio of table medians. Four-helper pairs improve +18.4%, +19.7%, +20.6%; zero-helper pairs -3.7%, +0.1%, -3.3%.
Thus this is useful for the measured GPU-limited four-helper case, not an unconditional default-on change. The extra
GPU pass also costs CPU setup/draw recording; the serial case has no reliable benefit and is slower in two pairs.

Hardware trace: installed Nsight Systems, visible uncapped captures, same-binary off/on full tours, 1,433 complete GPU
samples per capture after correlation-based warm-up/tail exclusion. Mean enclosing render-submission time falls
**1.8580→1.4548 ms (21.7%)**. The new depth pass costs 0.1205 ms; main start through output/final copy falls
1.0000→0.5382 ms. The local-shadow marker shifts 0.8314→0.7713 ms despite identical work: do not attribute that change
to a shadow optimisation; GPU clocks/scheduling and pipeline overlap can shift costs between stages. The whole-frame
and repeated untraced reductions are the primary evidence. A following off control over the initial 293 camera samples
is 1.8622 ms; use the matching initial-route slice, not the whole-tour average, as its comparison. Visible profiler
intervals themselves remain separate from hidden frame-time measurements.

**Memory:** paired steady full-tour process peaks remain ~225–226 MiB with four helpers and ~206 MiB without. The first
short on-run peaked at 251.9 MiB versus 225.4 off, consistent with first-use shader/pipeline compilation; do not promise
that only the warm peak occurs. GPU capture targets peak at 236.9/236.8 MiB off/on. Live target/cache/vector capacity
counters match; no extra depth allocation is made. Native benchmarks retain a 512 MiB process guard and 1 GiB free-RAM
stop, builds use one job, and captures/exports/tests remain sequential on Linux-disk scratch.

A deliberate mutation clearing main depth after the new pass is caught by the independent RGBA/picking fixture.
Production source is restored and both targets rebuilt afterwards. NVIDIA and lavapipe fixtures pass, including ordinary
streamed fallback and disabled spatial cells. The first short and full-tour pairs preserve RGBA and every shared old
non-timing work/upload/capacity column; the two new pass counters intentionally differ. Full NVIDIA 3D suite with the
experiment enabled passes (90.8 s, 259.5 MiB). The focused fixture passes on lavapipe (323.4 MiB), streamed input and
spatial-culling-off. Paired mist and AO/contact images/work match with the pass active; TAA, MSAA, unlit and bare cases
match with zero depth draws. Additional frame-600/frame-1200 checkpoints, cross-worker output/work and the archived
pre-change off executable match exactly. Combined-feature test peaks reach 264.0 MiB; paired full-tour peaks are the
lower figures above. Matched initial-route off GPU control drift is -0.63%, rather than a whole-tour/short-route ratio.
Other graphics backends remain untested.

Artifacts: `$HOME/kin-bench/grove-mask-prepass/` stores archived source/executable/shader hashes, paired CSV/PNG and RSS,
GPU captures/SQLite/correlation-based summaries, mutation evidence and scripts (`base.py`, `measure.py`, `captures.py`,
`profile_target.py`, `analyze.py`, `validate.py`, `mutate.py`). No commits created. This experiment does not lower shadow
resolution or simplify the material/lighting recipe.

#### Grove — Kin and Godot Forward+ CPU/GPU profile (2026-10-15)

The rendering/Blockfield work was committed as `70da884`, preserving the pending Summit changes separately. This profile
uses that Kin Release build (`-O3 -DNDEBUG`), four helpers and `KIN_MASKED_DEPTH_PREPASS=1`. Godot is the existing official
4.7.2 release template, Forward+, Vulkan, safe render thread. No source build or additional engine installation was needed.
Artifacts, scripts, CSVs, CPU samples, Nsight reports/SQLite exports, hashes and validation are under
`/home/bamedeo/kin-bench/grove-dual-profile/` (`summary.json`, `provenance.json`, `validation.json`).

**A benchmark correctness finding changes the comparison.** The former Godot recipe used four one-slot positional-shadow
atlas quadrants. An omni needs two adjacent slots in the *same* quadrant: none could be allocated. The light remained
marked `shadow_enabled`, but its requested point shadow was absent. This was confirmed both in the GPU timeline (no omni
shadow stage) and by an isolated point-shadow-only run. All earlier Godot local-shadow timings in this document, including
Mobile results, omit that work and must not be treated as full-feature comparisons. Unlit and sun-only results are unaffected.

The recipe now uses four slots per quadrant and a `4 * resolution` atlas, preserving `resolution` as the slot width. Actual
shadow draw/triangle counters, atlas dimensions and point cube-face dimensions are exported. A point-shadow-only case is
included in the lighting sweep and smoke sweep; the runner rejects a requested shadow workload that produces no shadow
work during the measured interval. Individual frames may legitimately reuse cached maps. The installed Grove PCK was also
updated, so a direct invocation of the previously exported comparison binary uses the corrected recipe.

The allocation regression is an actual render check: with the same current measurement code, restore the old atlas layout.
It produces **zero shadow draws/triangles**, and the validator rejects it. The corrected Forward+ case has median **899
shadow draws / 1,682,982 shadow triangles**; Mobile also produces the same median triangle count (185 draws, different
batching). Main camera draws/triangles and route counters match across the old/corrected Forward+ runs, while **70,678
screenshot pixels change** when the formerly missing point shadow appears. No production renderer code was changed for this fix.

**Throughput, after correcting the recipe.** Shared distance-32 snapshot (3,504 terrain chunks, 3,195,550 resident terrain
triangles), 100,000 plants, no mist, one terrain copy, ground route/FOV 75, 1280x720, PBR/IBL/HDR, four point lights and one
spot, sun plus local shadows, nominal shadow resolution 1024, one sample, no TAA/AO/contact. Each run warms 300 frames and
measures the complete 1,440-frame tour. CPU affinity is 0–15. Three paired repeats alternate engine order, with cooling
between individual runs; starting GPU temperatures were 76–77 C.

| Median of three run summaries | Kin, masked prepass, 4 helpers | Godot Forward+ |
|---|---:|---:|
| Frame interval p50 | 1.880928 ms | 1.826 ms |
| Frame interval p95 | 2.082729 ms | 1.958 ms |
| Frame interval p99 | 2.175236 ms | 2.065 ms |
| Mean frame interval | 1.911859 ms | 1.821107 ms |
| Maximum application RSS across these runs | 226.61 MiB | 612.45 MiB |

Paired p50 Kin/Godot differences were **+0.07%, +4.16%, +4.50%**. This is near parity on this workload, not evidence of a
large engine-wide speed difference. The earlier 1.188 ms Forward+ result used the missing-point-shadow recipe. Before the
cooling repeats, sustained corrected runs gave 2.011/1.825 ms; those numbers also mix thermal conditions.

Telemetry matters on this laptop: sustained sweeps reached 87–88 C and graphics clocks around 1290 MHz. The cooled
throughput runs mostly sampled 1665–1770 MHz; brief thermal-slowdown flags still occurred in two Godot runs. A few percent
cannot establish a clean winner under these conditions. No clocks, power limits or cooling settings were changed.

**CPU profile.** `perf record -F 499 --call-graph dwarf,4096 -e cycles:u`, sequential 5,760-frame runs. Kin's CSV monotonic
clock selects steady samples; the Godot profiling-only script emits the measured window's `/proc/uptime` and process CPU
counters. Both include four tours, exclude loading/warm-up, and retain raw samples. Leaf-IP output supplements call chains
that fail to unwind on helper threads. Kin had 9,218 steady samples; Godot's release binary is stripped, so its internal
function-level attribution is unavailable. Its built-in CPU timestamps and DSO-level samples supply the supported detail.

| Kin main-thread sampled cycles, inclusive by preparation stage | Share |
|---|---:|
| `cull_and_upload` | 46.24% |
| `build_local_shadows` | 38.36% |
| Shadow pass recording | 3.64% |
| Masked depth recording | 2.06% |
| Main pass recording | 0.96% |

These are sampled **CPU cycles**, not elapsed GPU time. The Kin profiling run used **1.790 ms of main-thread render CPU /
3.522 ms of aggregate process render CPU per frame**, with 1.860 ms mean frame intervals. Scene-command building is already
about 0.0008 ms; the remaining work is inside the renderer. Helper leaf samples are primarily culling, swept shadow reach
and local caster gathering rather than idle waits. `shadow_reach::Sweep::reaches` alone accounts for about 13.85% of aggregate
sampled cycles; incomplete helper stacks prevent assigning every such sample to an enclosing stage.

The corrected Godot CPU run used approximately **1.479 ms of whole-process CPU per frame**, including driver/other threads,
with 2.176 ms mean frame intervals in the warmer sampling run. This is a different timer boundary from Kin's render-only
counter and must not be added to viewport CPU timers. About 76% of aggregate sampled user cycles land in the stripped Godot
binary and 15% in NVIDIA's driver. In separate timestamp diagnostics, CPU recording averages about 1.052 ms: omni-shadow
recording 0.265 ms, directional/spot recording 0.141 ms, frame setup 0.138 ms, directional caster culling 0.126 ms, and individual
cube-face culls around 0.03–0.04 ms each. Those diagnostic dumps add overhead and are not throughput measurements.

Godot's unprofiled cooled runs report mean viewport CPU around **0.78–0.81 ms**, setup around **0.123 ms**, and viewport GPU
around **1.77–1.82 ms**. Its GPU timer nearly tracks the frame interval. Kin's main CPU nearly fills its frame interval.
This supports CPU preparation as Kin's next target and GPU/shadow work as Godot's larger remaining cost in this scene.

**GPU profile.** Nsight Systems 2025.1.3, Vulkan annotations and individual workloads, no CPU sampling. Captures present
uncapped frames (Kin `--visible`; a Godot profiling-only `force_draw(true, ...)`) so Nsight resets its timestamp allocation.
The initial offscreen Godot probe exceeded Nsight's 8,192-timestamp limit and is retained only as a failed/legacy probe.
Godot needs `--verbose` to emit release-build GPU debug labels. Both usable cooled captures have 1,433 complete measured
frames after correlation with CPU marker ordinals; seven buffered tail frames are omitted. No overflow or error diagnostics
occur in these captures. The profiler warns that the machine's Vulkan runtime is newer than its supported version; recently
added API calls may be absent from API counts.

| Raw mean GPU capture duration | Kin | Godot Forward+ |
|---|---:|---:|
| Render submission enclosing the main pass | 1.449 ms | 1.963 ms |
| Sun + local shadow render labels | 0.024 + 0.768 ms | 1.009 ms combined |
| Depth prepass | 0.120 ms, masked foliage | 0.213 ms, full scene |
| Main/opaque render label | 0.311 ms | 0.452 ms |
| Main start through submission completion | 0.536 ms | 0.483 ms |
| Light cluster copy/draw/compute labels | none | 0.017 / 0.044 / 0.006 ms |

GPU label durations can overlap and include dependencies; they are not additive shader costs. In particular, Kin's output
label (0.200 ms) includes late fragment completion, and Godot's empty transparent-pass label (0.104 ms) also carries prior
work. The previous no-HDR control already showed the Kin wait migrating into the final copy. Neither is evidence that
simple tone mapping or zero transparent objects intrinsically cost those amounts.

Do not use the raw GPU totals to claim greater shader efficiency: the sampled median graphics clocks were **2220 MHz for
Kin / 1740 MHz for Godot** in these captures, and their annotation/timestamp counts differ. CPU/GPU overlap and differing
capture/presentation paths also matter. The useful finding is the location of costs within each renderer.

CPU Vulkan API timing gives a further cross-check: Godot spends about 0.074 ms/frame in `vkWaitForFences` in the visible
capture, versus 0.0013 ms for Kin. Both have about 0.07–0.08 ms in uncapped presentation. Kin issues about 1,166 descriptor
updates/frame, but their measured driver CPU total is only **0.020 ms/frame**; descriptor reuse is a secondary target,
rather than an explanation for the much larger preparation cost. Counts cover the APIs this profiler supports.

**Controlled depth test.** With the corrected Godot recipe, toggle only its built-in depth-prepass setting in the profiling
PCK. Two alternating cooled pairs give p50s **1.834 / 1.820 ms on**, **3.440 / 3.489 ms off**: roughly **47% lower frame time**
with the full prepass. The first pair's final RGBA screenshots match exactly (zero changed pixels). This confirms substantial
cutout/opaque overdraw sensitivity. Kin's new masked prepass addresses much of the same issue; this result does **not** imply
that adding opaque terrain to Kin's existing masked prepass would give another 47% gain.

Other two-pair controls were run in the thermally warm sweep. Medians (Kin/Godot ms): no local shadows **1.249/1.004**;
no shadows **0.935/0.754**; no plants **1.340/1.366**; no local lights or shadows **0.928/0.564**; 2x render dimensions
(**four times the pixels**) **3.672/3.803**. These are diagnostics, not deltas to subtract from the cooled baseline. The
terrain-lit sweep does not establish an improvement. A single generic triangle count or main shader instruction count
cannot explain the performance of this scene.

**What Godot does differently, and proposals.** Godot keeps persistent scene geometry in a spatial BVH and queries that
structure for shadow views. A MultiMesh is culled by its aggregate box, trading finer CPU work for extra GPU copies.
Kin's median main triangle count is about 377,614 versus Godot's 406,732, despite similar camera draw counts (219/210).
Kin still refines about 25,386 camera copies and 162,452 local-shadow candidates per frame. Its coarse-cell acceptance already
helps, but the remaining partial cells and rebuilding per-frame draw/task metadata are substantial.

1. **Retain view-independent renderer preparation for unchanged scenes.** Keep mesh/material classification, draw metadata
   and task structure until scene edits invalidate them; continue refreshing camera/light visibility. Reuse the existing
   vectors and bound any cache. Target the 46%/38% CPU preparation groups, rather than the already negligible scene builder.
2. **Add a hierarchy over persistent draw/batch bounds and refine smaller foliage cells.** Query camera and light volumes
   before iterating all terrain draws or individual copies. Test 128/256 cells against the current 64, with bounded metadata
   and no second transform/sphere array. Preserve offscreen casters and image/picking equivalence. Godot's coarser culling is
   evidence for reducing CPU refinement, not permission to drop visible geometry or shadows.
3. **Extend GPU culling to ordinary retained instance batches.** The existing GPU cut handles clustered meshes, not these
   plain plants. Compact per-view indices and indirect counts on the GPU within a strict buffer budget, retaining scalar
   fallbacks and stable IDs. Avoid duplicating CPU instance tables per light. This is a larger engine change, justified only
   if simpler persistent metadata/hierarchy changes leave per-copy selection dominant.
4. **Measure an optional opaque + masked main-depth prepass.** Use the existing depth target and test high-overdraw/high-pixel
   scenes. Godot's depth toggle supports the mechanism, but extra CPU draws can hurt Kin's current bottleneck. Keep it opt-in
   unless paired timings and exact images support a broader default.
5. **Then specialize lit cutout/material/light paths and reduce binding work.** Profile foliage separately from terrain PBR;
   remove unused maps/branches and consider clustered light assignment at larger light counts. Five broad lights do not prove
   clustering will help Kin here. Descriptor-update API time and command recording are presently small, so neither should
   displace the preparation work above. No proposed speedup is claimed without implementing and measuring it.

**Comparison boundaries and validation.** The shared scene recipe is not pixel-identical across engines. In particular,
Godot's 1024-wide positional atlas slots use **512-square point cube faces** before dual-paraboloid conversion; Kin uses
**1024-square perspective cube faces** directly. Filtering, sun projection, culling and shadow caching differ. Record these
physical dimensions rather than treating a shared nominal `--shadow-resolution` value as equal shadow quality.

All 27 current Forward+ result CSVs passed actual shadow-work checks; the isolated point-only allocation regression passes
on Forward+ and Mobile and fails under the restored old layout. The post-commit Kin masked-prepass GPU fixture passed,
Python syntax/diff checks passed, and all 94 pre-existing SPIR-V hashes remain unchanged after trimming two trailing blank
lines for the commit. The original full Kin GPU suite/lavapipe/image validations are recorded in the preceding section.

One heavy process at a time throughout, serial builds, Linux-disk artifacts, guarded target processes and RSS/available-memory
timeouts. Cooled applications peak at 227/613 MiB rounded up; GPU capture targets at about 238/708 MiB, the legacy-mutation
export at 689 MiB, and the cold no-depth shader control at 681 MiB. The Nsight parent itself is only about 47 MiB and is not
reported as game RSS. No new per-instance cache, render target or renderer allocation was introduced by this profiling work.

Primary implementation references for the Godot interpretation (4.7.2 source, also saved in the artifact directory):
[atlas allocation](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/renderer_rd/storage_rd/light_storage.cpp),
[depth/shadow ordering and cube-face size](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp),
[scene and shadow BVH queries](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/renderer_scene_cull.cpp).

#### Grove — reuse renderer draw/task layout (2026-10-15)

Implemented the first CPU proposal from the dual-engine profile. The GPU renderer keeps one bounded, view-independent
mesh-command layout across renders of an unchanged `Scene3D`, reusing its existing `_refs`, `_pieces` and `_tasks` arrays.
Previously, 3,505 Grove references (mostly one-copy terrain draws) were reconstructed each frame, including their large
fixed-size counter arrays and worker partitions, even though the sample already retained its scene commands.

Scene commands now have an identity and revision. Mesh submissions and `clear()` increment a local revision; an atomic
identity is assigned only on scene creation/moves, not per submitted draw. A move also invalidates its source. The renderer
checks scene identity/revision, command and instance storage, and exact instance-batch revisions before accessing old raw
draw pointers. It keeps no owning scene handles. Placement/address reuse, moves, append/merge operations, clear/rebuild
with unchanged counts/allocations, and edits through shared batch handles cannot revive stale metadata.

Camera frustums, sun caster flags, spatial-group proofs, visibility, shadow selection, retained GPU-table slots, bounds
cache attachment and uploads still refresh every render. Ordinary writes through a valid `add_instances()` span remain
visible without rebuilding mesh metadata. The transient counters and output placements are recomputed; no visibility
result or instance transform is cached by this change. LOD-enabled scenes and skinned layouts use the original construction
path. Layout eligibility is bounded to 8,192 valid draw references, 16,384 pieces, 4,096 spatial groups, 128 batch stamps and
16 MiB of existing vector payload capacity. Alternating scenes replace the one layout, rather than accumulating caches.

This is **Kin-side work**; Blockfield only exports diagnostic counters. Enabled by default, with `KIN_PREPARED_SCENES=0`
selecting the reference path before renderer creation. Stats/CSV/report expose `prepared_reused`, `prepared_draws`,
`prepared_cache_bytes` (key/stamp payload) and `prepared_layout_bytes` (existing layout-vector capacities).

**Measurements.** Artifacts and reproducible commands are under `/home/bamedeo/kin-bench/grove-prepared/`, including
`comparison.json`, `measurements.json`, `summary.json`, `validation.json`, `mutation.json` and `provenance.json`. Same shared
100k-plant Grove recipe as the corrected Forward+ comparison, 1280x720/FOV 75, no mist, PBR/IBL/HDR, sun/local shadows at
nominal 1024, 1 sample, no TAA/AO/contact; `KIN_MASKED_DEPTH_PREPASS=1` in both paths. All previous optimisations stay enabled.
Release/O3, CPU affinity 0–15, 300 warm-up + 1,440 measured frames/full tour. Three alternating pairs at each of 0/4 helpers,
one heavy process at a time, with cooling between individual runs.

| Median of three run summaries | Rebuild layout | Reuse layout |
|---|---:|---:|
| 4 helpers: main-thread render CPU p50 | 1.686762 ms | 1.520215 ms |
| 4 helpers: aggregate render process CPU p50 | 3.363406 ms | 3.187135 ms |
| 4 helpers: frame p50 | 1.827476 ms | 1.836398 ms |
| 4 helpers: frame p95 | 2.067388 ms | 2.072931 ms |
| 4 helpers: peak process RSS | 226.03 MiB | 225.85 MiB |
| 0 helpers: frame p50 | 2.607796 ms | 2.414021 ms |
| 0 helpers: frame p95 | 2.902122 ms | 2.721227 ms |
| 0 helpers: peak process RSS | 206.50 MiB | 206.48 MiB |

Four-helper paired main-thread CPU reductions: **12.25%, 10.47%, 9.87%** (median 10.47%). Aggregate render CPU drops about 5%.
Frame time is essentially flat: paired changes **-6.44%, +0.12%, -0.49%** reduction (negative is slower), rather than a claimed
FPS gain. The first pair is a timing outlier despite lower CPU use; GPU boost clocks varied and no clock-normalised GPU
claim is made. The remaining GPU/frame work limits this particular four-helper workload.

Without helpers, paired frame-time reductions are **8.02%, 7.43%, 8.07%** (median 8.02%); paired main-thread CPU reductions
are **8.20%, 7.68%, 8.13%**. These are paired ratios, which differ from ratios of the run-summary medians. Start temperatures
were 70–74 C, measured active samples peaked at 81 C, and no sampled software thermal-slowdown flag was active. Graphics
boost still varied; `summary.json` includes per-run clocks, rather than assuming equal frequency.

All 1,440 measured rows of each enabled run reused its layout. Both paths keep exactly **3,854,592 bytes (3.68 MiB)** of
existing layout vector capacity. Key/stamp payload is **125 bytes without reuse / 141 bytes with one cached batch**; the
additional 16-byte batch stamp is independent of the 100,000 instances. Scene identity/revision storage is 16 bytes per
scene. Counters describe payload, not structure padding, allocator overhead or total process RAM. There is no duplicate
instance/sphere array, GPU buffer, shader, render target or new draw pass. RSS differences above are allocator/driver noise,
not a claimed memory saving.

**Correctness and limits.** The new GPU fixture compares cache-off, cache-on serial, and cache-on with two workers over
26 renders plus the over-budget case. It covers camera/cull/light/cascade changes; HDR/MSAA views and resize; valid mutable
instance spans; batch edits; same-count mesh edits in reused allocations; overlays; alternate scenes; move destinations and
sources; placement/address reuse; LOD fallback; TAA transitions; and 129 batch stamps falling back. RGBA, picks, main/cascade/
local-shadow work, feature bytes and retained GPU storage match. Cached slots/pointers must be refreshed even when TAA
changes whether a retained batch can use its GPU table.

A mutation replaces only the command revision in the renderer key with zero. The unchanged-count mesh edit then fails the
independent **RGBA/picking assertion**, proving the fixture detects stale metadata rather than merely checking a reuse
counter. Production source was restored in a `finally` block, both binaries rebuilt, and the fixture passed again.

Validation completed:
- New fixture on NVIDIA (about 1.7 s / 174 MiB), lavapipe (5.6 s / 390 MiB), streamed retention-off and spatial-culling-off paths.
- Full NVIDIA 3D suite with prepared reuse and masked depth enabled: **91.0 s / 261.5 MiB**, passed.
- All 12 paired timing runs: identical existing work/capacity counters; first-pair RGBA images also identical across paths
  and helper counts. Extra 1,200-frame checkpoint, AO/contact, mist, TAA, unlit and per-frame scene-rebuild pairs also match.
- `--rebuild-scene` correctly reports no layout reuse. The new disabled path's 300-frame image/work matches the archived
  pre-change executable. All **95 pre-existing SPIR-V hashes** are unchanged.
- Final serial build reports no work to do; `git diff --check` passes. Other GPU backends and a 3D-disabled build were not
  run for this change (no 3D-disabled build is configured here).

The implementation reduces CPU preparation, not the per-instance camera/shadow tests that still execute. It creates room
for game-side CPU work and helps the serial case, but does not establish a four-helper frame-rate improvement or a new
Godot ranking. Hierarchical visibility queries/smaller partial cells remain the next larger CPU target from the profile.

#### Forward+ — multi-scale comparison after prepared-layout reuse (2026-10-15)

Current Kin Release (`-O3 -DNDEBUG`), four render helpers, prepared-layout reuse and masked foliage depth enabled;
Godot 4.7.2 official release template, Forward+, Vulkan and safe render thread. Kin includes the uncommitted prepared-layout
change above, on top of `02a7796`; executable and source hashes plus the exact renderer diff identify this tested state.
The existing Godot project limits `threading/worker_pool/max_threads` to one for RAM use. A separate four-worker control
below checks that choice. This comparison adds no engine code or new installation.

Artifacts: `/home/bamedeo/kin-bench/forward-scale-prepared/`. `matrix.json`, `measure.py`, `analyze.py`,
`provenance.json`, `build-flags.txt`, per-run `.process.json` commands, raw CSV/reports/PNGs and `telemetry.csv` retain the
setup. `summary.json`, `table.md`, `analyzed_runs.json` and `validation.json` contain the complete results. Worker controls
have their own subdirectory, PCK and source provenance; production Godot scripts/PCK are unchanged.

**Method.** Two repeats of 13 workloads on each engine: **52 main runs**. Reverse both workload and engine order on the
second pass. Each warms 300 frames; Grove measures the full 1,440-frame ground-camera route, while the triangle test measures
300 frames. Both render uncapped offscreen with the final framebuffer copy, using CPU affinity 0–15. Values below are the
median of each run's p50/p95, rather than percentiles pooled across runs. No CPU sampling or optional CPU-clock instrumentation
is active. One GPU application runs at a time. Cool at least 15 seconds between runs, waiting for at most 78 C up to a
60-second limit. Target RSS guards are 512 MiB for Kin and 1,024 MiB for Godot, with available-RAM and 300-second time guards.

**Rich scene: density and pixel scaling.** Shared distance-32 terrain: 3,504 chunks / 3,195,550 resident terrain triangles.
Add 0–100,000 alpha-cutout plants, no transparent mist. Same FOV 75 ground route, one terrain copy, PBR maps, HDR/IBL,
four point lights plus one spot, sun and local shadows, nominal shadow size 1024, one sample, no TAA/AO/contact. The camera
culls the scene: resident counts are not the number of triangles visible in every frame. Rendering at 2560x1440 has **four
times** the pixels of 1280x720, with the same scene and camera.

| Resident plants | Kin 720p p50 | Forward+ 720p p50 | Kin 1440p p50 | Forward+ 1440p p50 |
|---:|---:|---:|---:|---:|
| 0 | 0.884 ms | 1.227 ms | 1.213 ms | 1.539 ms |
| 10,000 | 1.026 ms | 1.231 ms | 1.504 ms | 1.781 ms |
| 50,000 | 1.276 ms | 1.301 ms | 2.183 ms | 2.407 ms |
| 100,000 | 1.822 ms | 1.821 ms | 3.055 ms | 3.027 ms |

At 100k plants the 720p paired Kin/Godot p50 ratios are **0.986 / 1.014**, changing leader; at 1440p they are
**1.005 / 1.013**. This is near parity, not a reliable one-percent ranking. Four times the pixels increases frame time by
about **1.68x Kin / 1.66x Godot**. At 100k, p95 is **2.040 / 1.980 ms** (Kin/Godot) at 720p and **3.298 / 3.337 ms** at 1440p.
Maximum rich-scene application RSS is **226.0 MiB Kin / 635.4 MiB Godot**. These peaks include loading and exclude dedicated VRAM.

**Triangle throughput, separately.** Unlit vertex colour, no lighting/HDR/shadows/foliage, 720p. The top-down camera frames
all disjoint tiled copies. Every measured row on both engines submits exactly the requested triangle count, with 3,504 main
draws throughout. Instances share mesh buffers; this scales geometry work, not unique mesh storage or independent draw count.
Counts describe main-view submitted geometry, not all rasterized fragments or additional engine depth passes.

| Multiplier | Submitted triangles | Kin p50 / p95 ms | Forward+ p50 / p95 ms | Peak RSS Kin / Godot MiB |
|---:|---:|---:|---:|---:|
| 1x | 3,195,550 | 1.010 / 1.055 | 2.261 / 2.751 | 180.1 / 597.9 |
| 5x | 15,977,750 | 2.366 / 2.488 | 4.341 / 4.551 | 188.3 / 596.2 |
| 10x | 31,955,500 | 4.295 / 4.411 | 8.379 / 8.911 | 203.1 / 604.1 |
| 20x | 63,911,000 | 8.112 / 8.380 | 16.779 / 17.411 | 233.1 / 609.9 |
| 100x | 319,555,000 | 41.607 / 42.962 | 96.674 / 101.056 | 418.4 / 660.7 |

Kin retains a large observed lead in this unlit instanced-geometry test, including both reversed-order repeats. It does not
establish the same lead for a game with PBR, cutouts and shadows: that scene is close above. The 100x result also includes
substantial thermal/boost differences. Active-run median graphics clocks were **1920 / 1860 MHz for Kin**, **1590 / 1500 MHz
for Godot**, across the two repeats. Godot reached 88 C with thermal flags in both runs; Kin reached 86/87 C, with thermal
flags in its second run. Do not interpret the roughly 2.3x observed ratio as a normalized shader-efficiency measurement.
Clocks were neither locked nor changed. Telemetry uses 500 ms samples within each application run (including loading),
filtered to GPU utilization above 30% and clocks above 500 MHz; the shortest runs may have few or no qualifying samples.

**Godot worker-pool control.** Only the project worker limit changes from one to four, in an isolated exported pack sharing
the same official release binary. Two repeats in opposite case order add eight runs, with the same cooling/guards. All main
camera draw/triangle/route counters match the corresponding one-worker runs, and first-repeat RGBA screenshots are exactly equal.
These controls run later, so small differences are not isolated causal speedups from worker count alone.

| Case | One-worker Forward+ p50 | Four-worker Forward+ p50 | Four-worker peak RSS |
|---|---:|---:|---:|
| No plants, 720p | 1.227 ms | 1.144 ms | 619.9 MiB |
| 100k plants, 720p | 1.821 ms | 1.798 ms | 638.1 MiB |
| 100k plants, 1440p | 3.027 ms | 3.011 ms | 637.7 MiB |
| 3.20M unlit triangles | 2.261 ms | 2.264 ms | 613.9 MiB |

The four-worker control preserves the dense-scene/triangle conclusions. It also gives a faster no-plant sample than the
original two-run median; Kin remains faster there. The export itself peaked at 688.3 MiB, under its 1,024 MiB guard.

**Boundaries and checks.** The corrected point-shadow atlas is in use: Godot reports 4096-wide atlas / 1024-wide slots /
512-square point cube faces, while Kin uses 1024-square point cube faces directly. Projections, filtering, culling and caching
also differ. Shared nominal settings are not identical shadow quality or pixels. Actual shadow work is checked, not just
requested flags. All 60 runs passed row-count, feature/route/geometry checks; all main Kin measured rows reused the prepared
layout. Godot reports release mode and `forward_plus`. The 33 shared scene/report fields match for every main pair, and
binary/source/asset hashes remain unchanged at completion. No new renderer tests are needed for this measurement-only change;
the implementation validation is in the preceding section.

The useful result is that current Kin is competitive in this rich scene and uses substantially less application RAM, while
its stronger unlit geometry throughput does not remove the cost of dense foliage, visibility and shadows. New profiling or
controlled feature toggles would be needed to attribute individual scaling costs to functions or GPU stages.

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
