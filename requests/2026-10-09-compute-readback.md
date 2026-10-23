# Request: read back what a compute shader wrote (textures and data buffers)

From: the Fantasy Merchant portraits session (`../fantasy-merchant-portraits`,
branch `portraits-2.5d`), 2026-10-09.
Status: implemented on branch `feat/compute-readback` (worktree
`kin-wt/compute-readback`, from `origin/main` = v0.2.6); builds clean; all 54
tests pass (dummy drivers), and `kin-renderer` with `KIN_REQUIRE_GPU_TESTS=1`
(test_gpu_readback included) passes on the GPU. Not merged, not released.

## Why

The game's portraits are rendered from 3D models by ray marching signed
distance fields, about 1.5 s a portrait on one core (rays and light are
~1.1 s of it). The plan is to move the marching to compute shaders, and keep
the colour passes (k-means per material, a labelling by ICM) on the CPU,
which needs what the shaders wrote (each pixel's part, depth, normal, light)
back in memory. kin can create and dispatch compute shaders and write storage
textures and data buffers, but nothing public reads them back: `read_rgba`
reads the current render target only, and `GpuDevice::read_texture_rgba` is
internal and synchronous on its own command buffer (so it would read a
compute output before the frame that writes it is even submitted).

## What the branch adds

- `RendererBackendCapabilities::readback` (true on the SDL_GPU backend).
- `Renderer2D::read_texture(const Texture&, ReadDone)` and
  `Renderer2D::read_data_buffer(const DataBuffer&, ReadDone)`, with
  `using ReadDone = std::function<void(std::vector<u8> bytes, Vec2i size)>`
  (backend.hpp): a texture's rows tightly packed, `texture_format_bytes()` a
  texel; a buffer's size `{bytes, 1}`; no bytes if it failed.
- Asynchronous, nothing waits: the copy pass is recorded into the frame's own
  command buffer after all recorded so far (so after a `dispatch_compute`
  before it), into a download transfer buffer of its own. `present()` submits
  a frame with readbacks with a fence (that frame goes untimed for the GPU
  timer) and, every present, hands back (on the render thread) the reads whose
  frame's fence has passed, in order. The destructor waits for those in
  flight; reads recorded but never submitted are handed back empty.
- `IRenderer2DBackend::read_texture` / `read_data_buffer` default to false
  (the SDL_Renderer backend has no compute anyway).
- Test `test_gpu_readback` (tests/renderer_tests.cpp, beside
  `test_gpu_compute`): `compute_gradient.comp` into a 60x36 storage texture,
  read back, presented until handed over; texels checked as the compute test
  checks them; a data buffer read back equal to what was uploaded.

## Still to do

- docs/changelog.md and the renderer docs, when merged.
- Maybe later: a synchronous `read_texture_now` for tools, and reading a
  sub-region or a byte range.
