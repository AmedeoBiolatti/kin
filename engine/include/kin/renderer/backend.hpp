#pragma once

#include <kin/core/affine.hpp>
#include <kin/core/types.hpp>
#include <kin/renderer/data_buffer.hpp>
#include <kin/renderer/material.hpp>
#include <kin/renderer/shader_reflect.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/gradient.hpp>
#include <kin/renderer/post_process.hpp>
#include <kin/renderer/render_target.hpp>
#include <kin/renderer/shader.hpp>
#include <kin/renderer/texture.hpp>

#include <memory>
#include <functional>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class JobSystem;

// One quad of a draw_sprites() call: what draw_texture(texture, source, dest,
// tint, rotation, pivot) would draw.
struct SpriteInstance {
    Rectf dest{};
    Rectf source{};             // texture pixels; empty means the whole texture
    Color tint = colors::white;
    f32 rotation = 0.0f;        // degrees, clockwise, about the pivot
    Vec2f pivot{0.5f, 0.5f};    // normalized within dest
};

struct RendererBackendCapabilities {
    bool immediate_2d = true;
    bool queued_2d = false;
    bool render_targets = false;
    bool blend_modes = false;
    bool min_max_blend = false; // BlendMode::Max and Min honoured
    bool shader_geometry = false; // draw_shader_geometry() draws
    bool data_buffers = false;    // create_data_buffer() and shaders reading them
    bool compute = false;         // compute shaders (Renderer2D::dispatch_compute)
    bool transforms = false;      // set_transform() honoured (Renderer2D::push_transform)
    bool materials_2d = false;
    bool gradients = false; // fill_gradient_rect honored (else flat mid-color fill)
    bool text = false;
    bool rendering_3d = false;
    // create_texture() accepts the data formats (R16Uint, Rg16Uint, R32Float).
    bool data_textures = false;
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
    u64 texture_uploads = 0;
    u64 texture_upload_submits = 0;
    u64 saved_texture_draws() const {
        return texture_draws_submitted > texture_batch_flushes ? texture_draws_submitted - texture_batch_flushes : 0;
    }
    u64 saved_rect_draws() const {
        return rect_fills_submitted > rect_batch_flushes ? rect_fills_submitted - rect_batch_flushes : 0;
    }
};

class IRenderer2DBackend {
public:
    virtual ~IRenderer2DBackend() = default;

    virtual std::string_view name() const = 0;
    virtual RendererBackendCapabilities capabilities() const {
        return {};
    }
    virtual RendererBackendStats stats() const {
        return {};
    }
    virtual void reset_stats() {}
    // Measures each frame's GPU time (stats().last_gpu_frame_ms) where supported.
    virtual void set_gpu_timing_enabled(bool) {}
    // The pipelines this run made, as text a later run passes to
    // prewarm_pipelines to make them while it loads (before their first draw).
    virtual std::string pipeline_record() const { return {}; }
    virtual void prewarm_pipelines(std::string_view /*record*/) {}
    // Layers (Renderer2D::begin_layer): a target drawn into in the current
    // coordinates, at its own resolution; pop returns the box of what was drawn
    // (in draw coordinates), empty when nothing was. False / nullopt: not had.
    struct LayerBounds {
        Rectf dest;   // what was drawn, in draw coordinates (empty: nothing)
        Rectf source; // the same in the layer target's pixels
    };
    virtual bool push_layer_target(const RenderTarget&) { return false; }
    virtual std::optional<LayerBounds> pop_layer_target() { return std::nullopt; }
    // The current target's size in pixels (a layer's resolution is a share of it).
    virtual Vec2i current_target_pixels() const { return {}; }
    // Draws show how many times each pixel is shaded instead of themselves.
    virtual void set_overdraw_view(bool) {}
    // A named scope of GPU work, timed while GPU timing is on (else nothing).
    virtual void begin_gpu_scope(std::string_view) {}
    virtual void end_gpu_scope() {}
    // The scopes that finished on the GPU since the last call.
    virtual std::vector<GpuScopeTiming> take_gpu_scope_timings() { return {}; }
    virtual void set_texture_batching_enabled(bool) {}
    virtual bool texture_batching_enabled() const {
        return false;
    }

    virtual void clear(Color color) = 0;
    virtual void present() = 0;

    // Captures the current render target to a PNG file. Returns false if the
    // backend cannot read back pixels (e.g. fake/test backends).
    virtual bool save_png(const char*) {
        return false;
    }

    // Reads a logical-space region from the current render target as tightly
    // packed RGBA pixels. Returns false when readback is unsupported or empty.
    virtual bool read_rgba(Rectf, std::vector<u8>&, Vec2i&) {
        return false;
    }

    // Copy a logical-space region of the current render target into `dst` entirely on
    // the GPU (no CPU round-trip) — the fast path for backdrop capture / refraction.
    // Default returns false so callers fall back to read_rgba + re-upload.
    virtual bool blit_region_to_target(Rectf /*region*/, const RenderTarget& /*dst*/) {
        return false;
    }

    // Native pixel dimensions a capture of `region` (logical) would have. Default = the
    // logical size (1:1); a backend whose internal target is higher-res (e.g. native
    // under logical presentation) overrides this so captures stay full resolution.
    virtual Vec2i region_pixel_size(Rectf region) const {
        return {static_cast<i32>(region.w + 0.5f), static_cast<i32>(region.h + 0.5f)};
    }

    virtual void set_logical_size(Vec2i size) = 0;
    virtual void set_integer_logical_size(Vec2i size) = 0;
    virtual void push_native_coordinates() {}
    virtual void pop_native_coordinates() {}
    virtual Vec2i output_size() const = 0;
    virtual Vec2f window_to_logical(Vec2f window_px) const = 0;
    virtual Vec2f logical_to_window(Vec2f logical) const = 0;

    virtual Texture create_texture_from_rgba(const u8* pixels, Vec2i size) = 0;
    // A texture of `format`, filled from `pixels` (size.x * size.y texels, rows
    // top to bottom) or with zeros when null. The default handles Rgba8 only and
    // returns an invalid texture for the data formats.
    virtual Texture create_texture(Vec2i size, TextureFormat format, const void* pixels) {
        if (format != TextureFormat::Rgba8) {
            return {};
        }
        if (pixels) {
            return create_texture_from_rgba(static_cast<const u8*>(pixels), size);
        }
        const std::vector<u8> zeros(static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 4u, 0);
        return create_texture_from_rgba(zeros.data(), size);
    }
    // Replace the texels of a `size` region at `at` of `texture` with `pixels`
    // (RGBA8, tightly packed, row by row). False when the backend cannot update a
    // texture in place: the caller then makes a new one. Default: false.
    virtual bool update_texture(const Texture& /*texture*/, Vec2i /*at*/, Vec2i /*size*/, const u8* /*pixels*/) {
        return false;
    }
    // As update_texture, with `fill` writing the texels (`bytes` of them). The
    // default fills a buffer and calls update_texture; SDL_GPU lets it write
    // straight into the upload memory.
    virtual bool write_texture(const Texture& texture, Vec2i at, Vec2i size, std::size_t bytes,
                               const std::function<void(std::span<u8>)>& fill) {
        std::vector<u8> texels(bytes);
        fill(texels);
        return update_texture(texture, at, size, texels.data());
    }
    virtual void draw_texture(const Texture& texture, Rectf dest) = 0;
    virtual void draw_texture(const Texture& texture, Rectf source, Rectf dest) = 0;
    virtual void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color) {
        draw_texture(texture, source, dest);
    }
    virtual void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32, Vec2f) {
        draw_texture(texture, source, dest, tint);
    }
    // Many quads from one texture, in order. Backends that can draw them as one
    // instanced batch override this; the default draws them one by one.
    virtual void draw_sprites(const Texture& texture, std::span<const SpriteInstance> sprites) {
        const Vec2i size = texture.size();
        for (const SpriteInstance& sprite : sprites) {
            const Rectf source = sprite.source.w > 0.0f && sprite.source.h > 0.0f
                ? sprite.source
                : Rectf{0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
            draw_texture(texture, source, sprite.dest, sprite.tint, sprite.rotation, sprite.pivot);
        }
    }

    virtual void fill_rect(Rectf rect, Color color) = 0;
    virtual void draw_rect(Rectf rect, Color color) = 0;
    virtual void draw_line(Vec2f a, Vec2f b, Color color) = 0;
    virtual void fill_rounded_rect(Rectf rect, f32, Color color) {
        fill_rect(rect, color);
    }
    virtual void draw_rounded_rect(Rectf rect, f32, Color color, f32 = 1.0f) {
        draw_rect(rect, color);
    }

    // Linear gradient fill. Default degrades to a flat fill of the mid color, so
    // the call is always valid; a backend that reports capabilities().gradients
    // overrides this with per-vertex interpolation.
    virtual void fill_gradient_rect(Rectf rect, const Gradient& gradient) {
        fill_rect(rect, mix(gradient.start, gradient.end, 0.5f));
    }

    // Create a custom fragment-shader material from precompiled blobs. Default
    // returns a null handle (no shader support) so callers degrade to a fill; a
    // backend reporting capabilities().materials_2d builds a real shader/state.
    virtual ShaderHandle create_shader(const ShaderDesc&) { return {}; }
    // Replaces a shader in place (its handle stays): false when it cannot.
    virtual bool reload_shader(ShaderHandle, const ShaderDesc&) { return false; }

    // Draw `rect` with a custom-shader material. Default is a no-op; the UI layer
    // reads capabilities().materials_2d (false here) and supplies a fallback fill.
    virtual void draw_shader_surface(Rectf, ShaderHandle, const ShaderParams&) {}

    // As above, but binds `source` as the material's fragment sampler 0 (e.g. a
    // captured backdrop for refraction) instead of the default white texture. Default
    // delegates to the textureless form, so backends without material support degrade.
    virtual void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     const Texture& /*source*/) {
        draw_shader_surface(rect, shader, params);
    }

    // As above, but binds a second source at fragment sampler 1 (set=2 binding=1) for
    // 2-input material shaders (cross-dissolve A<->B, bloom combine). Default delegates
    // to the single-source form (ignores `source1`), so backends degrade unchanged.
    virtual void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     const Texture& source0, const Texture& /*source1*/) {
        draw_shader_surface(rect, shader, params, source0);
    }

    // As above, with any number of sources: `sources[i]` binds at fragment sampler i
    // (at most MaxShaderSamplers; Renderer2D rejects longer lists). The default keeps
    // the first two and delegates, so backends without wider support degrade unchanged.
    virtual void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     std::span<const Texture> sources) {
        if (sources.empty()) {
            draw_shader_surface(rect, shader, params);
        } else if (sources.size() == 1) {
            draw_shader_surface(rect, shader, params, sources[0]);
        } else {
            draw_shader_surface(rect, shader, params, sources[0], sources[1]);
        }
    }

    // As above, with storage buffers bound after the textures. The default
    // draws without them.
    virtual void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     std::span<const Texture> sources, std::span<const DataBuffer> /*buffers*/) {
        draw_shader_surface(rect, shader, params, sources);
    }

    // Triangles with a material shader (Renderer2D::draw_shader_geometry); the
    // indices are already checked. Backends without it draw nothing.
    virtual void draw_shader_geometry(std::span<const ShaderVertex>, std::span<const u32>, ShaderHandle,
                                      const ShaderParams&, std::span<const Texture>, std::span<const DataBuffer>) {}

    // Compute (capabilities().compute): a pipeline from SPIR-V and its layout, a
    // texture compute shaders can write, and a dispatch of `groups` workgroups
    // recorded in the frame between the draws around it.
    virtual ComputeShaderHandle create_compute_shader(ShaderBlob /*spirv*/, const ShaderLayout& /*layout*/) {
        return {};
    }
    virtual Texture create_storage_texture(Vec2i /*size*/, TextureFormat /*format*/) { return {}; }
    virtual bool dispatch_compute(ComputeShaderHandle, Vec2i /*groups*/, const ComputeBindings&) { return false; }

    // Storage buffers for shaders (capabilities().data_buffers). `data` (or
    // zeros) fills a new one; update/write replace `bytes` at `offset`.
    virtual DataBuffer create_data_buffer(std::size_t /*bytes*/, const void* /*data*/) { return {}; }
    virtual bool update_data_buffer(const DataBuffer&, std::size_t /*offset*/, std::size_t /*bytes*/,
                                    const void* /*data*/) {
        return false;
    }
    virtual bool write_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes,
                                   const std::function<void(std::span<u8>)>& fill) {
        std::vector<u8> data(bytes);
        fill(data);
        return update_data_buffer(buffer, offset, bytes, data.data());
    }

    // Set the full-scene post-processing chain applied at present() time. Default is a
    // no-op (no shader/RT support) so the backend presents the scene unprocessed.
    // The chain persists until set again; pass an empty span to clear it.
    virtual void set_post_process(std::span<const PostProcessPass>) {}

    // See Renderer2D::set_blend_mode. The default backend ignores it.
    virtual void set_blend_mode(BlendMode) {}
    // See Renderer2D::push_transform: maps every later draw's coordinates
    // (before viewports and clips, which it leaves alone). A backend that
    // reports capabilities().transforms honours it.
    virtual void set_transform(const Affine2&) {}
    // Worker threads the backend may use for large batches (null: none).
    virtual void set_job_system(JobSystem*) {}

    virtual void set_viewport(Rectf rect) = 0;
    virtual void reset_viewport() = 0;
    virtual void push_viewport(Rectf rect) = 0;
    virtual void pop_viewport() = 0;
    virtual void push_clip(Rectf rect) {
        push_viewport(rect);
    }
    virtual void pop_clip() {
        pop_viewport();
    }

    // Render targets. Defaults report "unsupported": create returns an empty
    // target, push/pop are no-ops. A backend that reports
    // capabilities().render_targets overrides these. While a target is pushed,
    // drawing uses the target's native pixel space (1:1, origin top-left);
    // push must save and pop must restore the prior target, viewport, clip, and
    // logical-presentation state. Calls nest (stack-based).
    virtual RenderTarget create_render_target(Vec2i /*size_px*/, ScaleMode = ScaleMode::Linear) {
        return {};
    }
    virtual void push_render_target(const RenderTarget&) {}
    virtual void pop_render_target() {}

    // Per-texture sampling override (one SDL_SetTextureScaleMode call). No-op by
    // default. Lets effects bilinear-sample a texture created Nearest.
    virtual void set_scale_mode(const Texture&, ScaleMode) {}
};

} // namespace kin
