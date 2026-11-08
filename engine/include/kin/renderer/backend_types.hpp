#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/data_buffer.hpp>
#include <kin/renderer/gpu_memory_stats.hpp>
#include <kin/renderer/texture.hpp>

#include <span>
#include <string>

namespace kin {

struct ShaderParams;

// How Renderer2D::draw_distance_field reads a distance-field texture: its
// alpha is 0.5 on the outline, rising inside and falling outside by 0.5 over
// `spread` texels.
struct DistanceFieldStyle {
    f32 spread = 8.0f;
    f32 outline = 0.0f; // its width outside the outline, in texels
    Color outline_color = colors::black;
};

struct RendererBackendCapabilities {
    bool immediate_2d = true;
    bool queued_2d = false;
    bool render_targets = false;
    bool blend_modes = false;
    bool min_max_blend = false;    // BlendMode::Max and Min honoured
    bool shader_geometry = false;  // draw_shader_geometry() draws
    bool data_buffers = false;     // create_data_buffer() and shaders reading them
    bool compute = false;          // compute shaders (Renderer2D::dispatch_compute)
    bool transforms = false;       // set_transform() honoured (Renderer2D::push_transform)
    bool shapes = false;           // draw_shape_mesh() draws (Renderer2D::draw_shape and friends)
    bool shape_primitives = false; // draw_shape_primitives() draws primitives whole
    bool distance_fields = false;  // draw_distance_field() draws
    bool masks = false;            // layers, and draw_masked() lays one over through another
    bool stencil_clips = false;    // push_stencil_clip(): hard-edged clips without layers
    bool linear_color = false;     // set_color_space(Linear), and an HDR scene
    bool color_output = false;     // set_color_output(): exposure, tonemapping, LUTs, dithering
    bool materials_2d = false;
    bool gradients = false; // fill_gradient_rect honored (else flat mid-color fill)
    bool text = false;
    // 3D views (kin/render3d, experimental; renderer_3d() is not null).
    bool rendering_3d = false;
    // create_texture() accepts the data formats (R16Uint, Rg16Uint, R32Float).
    bool data_textures = false;
    bool render_attachments = false; // sampled RG16F/RGBA16F colour and D32F depth targets
    bool render_attachment_arrays = false; // the same formats as explicit 2D arrays
    bool texture_copies = false; // exact base-level copies of compatible textures, including attachment arrays
};

// A named part of a frame's GPU work (Renderer2D::gpu_scope) and its GPU time.
struct GpuScopeTiming {
    std::string name{};
    f64 ms = 0.0;
    // The pixels its draws shaded (as RendererBackendStats::last_pixels_drawn),
    // and that over the screen's: its share of the frame's overdraw.
    f64 pixels = 0.0;
    f64 overdraw = 0.0;
};

// What a compute dispatch reads and writes (Renderer2D::dispatch_compute). In
// the shader, set 0 holds the sampled `sources` then the read-only `buffers`,
// set 1 the `outputs` (storage textures, from create_storage_texture) then the
// `output_buffers`, and set 2 the uniform block (`params`).
struct ComputeBindings {
    std::span<const Texture> sources{};
    std::span<const DataBuffer> buffers{};
    std::span<const Texture> outputs{};
    std::span<const DataBuffer> output_buffers{};
    const ShaderParams* params = nullptr;
};

struct RendererBackendStats {
    GpuMemoryStats gpu_memory{};
    u64 texture_draws_submitted = 0;
    u64 texture_batch_flushes = 0;
    u64 texture_batch_breaks = 0;
    u64 direct_rect_fills = 0;
    u64 rect_fills_submitted = 0;
    u64 rect_batch_flushes = 0;
    u64 direct_rect_outlines = 0;
    u64 direct_lines = 0;
    f64 last_present_flush_ms = 0.0;
    f64 last_present_backend_ms = 0.0;
    // SDL_GPU only. The time present() blocked acquiring a swapchain image: the
    // CPU waiting for the GPU (or the display) to catch up.
    f64 last_gpu_wait_ms = 0.0;
    // SDL_GPU with GPU timing on: the GPU time of the latest frame that finished,
    // usually one or two frames behind. Measured with fences, not timestamps.
    // With the GPU far behind, some frames go untimed and the next sample spans
    // them (last_gpu_frame_span frames): this is then their average.
    f64 last_gpu_frame_ms = 0.0;
    u32 last_gpu_frame_span = 1;
    // Samples taken so far: last_gpu_frame_ms is new when this has grown.
    u64 gpu_frames_sampled = 0;
    // SDL_GPU: the pixels the last frame's draws covered, in each target's own
    // pixels (a logical scene at its native size; overlaps counted each time,
    // clipping not taken off), and that over the screen's: how many times each
    // pixel was shaded on average. Render targets count too.
    f64 last_pixels_drawn = 0.0;
    f64 last_overdraw = 0.0;
    // SDL_GPU: texture uploads (create_texture, update_texture) so far, and the
    // command buffers that carried them (batched: many uploads, one submit).
    u64 texture_copies = 0, texture_copy_bytes = 0; // explicit whole-image base-level copies (all layers)
    u64 texture_uploads = 0;
    u64 texture_upload_submits = 0;
    // SDL_GPU shared upload staging: current single-handle capacity and allocations
    // since device creation. Excludes SDL cycle backings, retired buffers and driver pools.
    u64 upload_staging_bytes = 0, upload_staging_allocations = 0;
    u64 saved_texture_draws() const {
        return texture_draws_submitted > texture_batch_flushes ? texture_draws_submitted - texture_batch_flushes : 0;
    }
    u64 saved_rect_draws() const {
        return rect_fills_submitted > rect_batch_flushes ? rect_fills_submitted - rect_batch_flushes : 0;
    }
};

} // namespace kin
