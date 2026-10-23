# Request: renderer features for GPU-drawn shadows, cheaper uploads, steadier pacing

From: the XC-121 session (a game on kin), 2026-10-03.
Status: done in kin 0.2.4 — 1 `draw_shader_geometry`, 2 `BlendMode::Max`/`Min`,
3 batched uploads with cycling, 4 `gpu_scope`, 5 `FrameTimeSnapper`,
6 `--profile` keeps the window (see docs/changelog.md).

Context: a game drew pre-rendered building shadows with a fragment shader
(the render studio's `krs::GroundShadowsGpu`: per object a convex hull,
filled from pre-rendered sun images, combined by max). Working around what
kin's 2D renderer offers today took tiles, a render-target cache and
worker threads. Each item below would have made that simpler or cheaper,
and is general enough for other games. Numbers are from an RTX 4080
Laptop, Linux/X11, Vulkan, Release builds.

## 1. Draw triangles with a custom fragment shader

**Want:** something like

```cpp
void Renderer2D::draw_shader_geometry(std::span<const ShaderVertex> vertices,
                                      std::span<const u32> indices,
                                      ShaderHandle shader, const ShaderParams& params,
                                      std::span<const Texture> sources);
// ShaderVertex: position (logical px), uv, color, plus a few free floats
// (e.g. vec4 `custom`) the fragment shader reads as an input.
```

**Why:** today a material shader can only cover an axis-aligned rect
(`draw_shader_surface`). A shape that is really a polygon (a hull, a
shadow, a beam) has to be a rect over its bounding box, with the shader
discarding most pixels and re-deriving the shape per pixel. With
geometry, the rasterizer does the inside test and only covered pixels run.
Per-vertex free floats let one draw carry many objects, each with its own
parameters, such as an index into a data texture.

**Notes:** the fragment-stage contract can stay as it is (sets 2 and 3).
Only the vertex input grows.

## 2. `BlendMode::Max` (and `Min`)

**Want:** blend modes using `SDL_GPU_BLENDOP_MAX` / `MIN` on color and alpha.
`gpu_pipeline_cache.cpp` already maps the other modes to blend ops, so this
looks like two more cases plus the public enum.

**Why:**
- Overlapping shadows (and fog of war, light coverage, heat maps) combine by
  maximum, not by "over". Without Max, overlapping draws can't each be drawn
  separately; one shader has to loop over everything that might overlap a
  pixel and take the max itself. That forced tile binning and an
  all-objects loop.
- With items 1 and 2 together, each object could be its own small polygon,
  max-blended into one target.

## 3. Uploads that don't submit a command buffer each

**Seen:**
- `Renderer2D::create_texture` for a 1024 x N R32Float data texture costs
  about 2-3 ms on the main thread.
- `update_texture` costs about 0.4-2 ms. Each call creates a transfer buffer,
  acquires a command buffer and submits it (`GpuDevice::update_texture`).
- When many small uploads land in one frame, they each pay that cost, and
  in a test with 50 uploads queued in one frame the GPU stalled for
  30-120 ms.

**Want, any of:**
- Uploads recorded into the frame's own command buffer (a copy pass before
  the frame's render passes), batched, with one transfer buffer reused or
  ringed across frames.
- An option to cycle (`SDL_UploadToGPUTexture(..., cycle = true)`) when the
  whole texture is replaced, so a texture the previous frame still reads
  doesn't serialize the upload.
- A data-texture ring helper: N textures of a fixed size that a caller
  writes in turn, each released only after its frame's fence.

## 4. GPU timing per pass or per draw

**Want:** GPU timestamps around a named scope, for example
`renderer.gpu_scope("shadows")`, reported by `--profile` and the debug
overlay as GPU milliseconds.

**Why:**
- Today the only GPU figure is `gpu.frame` for the whole frame.
- To learn what a shader costs, a game has to write a separate tool that
  draws N times and reads back.
- `gpu.frame` can also mislead when the GPU is behind (a timed frame includes
  the untimed ones before it). That's already known on the kin side.

## 5. Frame pacing: snap frame times to the fixed step

**Seen:**
- On a 240 Hz display, with vsync on and `max_fps = 60` (`fixed_dt = 1/60`),
  the measured frame time jitters around 16.67 ms.
- On a quiet machine, 0.1-0.4% of frames ran 0 or 2 fixed updates.
- Under CPU load from other processes, 30-50% did, which shows as judder
  in anything moved in `update()`.

**Want:** in `App::run`, when the measured frame time is within a small
tolerance of a whole number of `fixed_dt` steps (about 0.5-1 ms, or a
fraction of a vsync interval), count it as exactly that many steps. This is
the usual "snap delta to vsync multiples" fix.

- Large gaps (real hitches) still accumulate as now.
- `ctx.alpha` is already passed to scenes for interpolation, so games that
  interpolate keep working. Games that don't would stop stuttering on
  ordinary jitter.

## 6. Small things noticed

- `--profile` quietly forces a headless run (`scene_app.cpp`), even without
  `--headless`. A "windowed --profile" run then behaves like a headless one;
  in XC-121's case it loaded every asset eagerly and was killed by the OOM
  killer. A log line saying so, or `--profile` honouring the window, would
  have saved an afternoon.
