#pragma once
// Thin IRenderer2DBackend adapter over the modular kin::gpu layer (device +
// pipeline cache + geometry batch). Renders the frame into a "scene" render
// texture and blits it to the swapchain on present (logical-presentation model).
// G1a: frame/pass lifecycle + the unified white-texture pipeline + solid/line/
// texture primitives. Rounded/gradient/AA (G1b), clip/render-target stacks +
// read_rgba (G1c), and shader materials (G3) build on this without rewrites.
#include <kin/platform/window.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/texture.hpp>

#include "gpu_device.hpp"
#include "gpu_frame_timer.hpp"
#include "gpu_geometry_batch.hpp"
#include "gpu_pipeline_cache.hpp"
#include "gpu_shader.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <tuple>
#include <optional>
#include <unordered_map>
#include <span>
#include <vector>

namespace kin {

namespace gpu {
// Wraps a GpuTexture so it lives behind a kin::Texture.
class GpuTextureBackend : public ITextureBackend {
public:
    explicit GpuTextureBackend(GpuTexture texture, bool premultiplied = false,
                               ScaleMode scale = ScaleMode::Nearest,
                               TextureFormat format = TextureFormat::Rgba8)
        : ITextureBackend(ITextureBackend::Kind::Gpu),
          _texture(std::move(texture)), _premultiplied(premultiplied), _scale(scale), _format(format) {}
    Vec2i size() const override {
        return {static_cast<i32>(_texture.width()), static_cast<i32>(_texture.height())};
    }
    TextureFormat format() const override { return _format; }
    const GpuTexture& texture() const { return _texture; }
    // Puts `texture` in place of the current one (made mipmapped), returning
    // the old for the caller to keep until the frame's draws are submitted.
    GpuTexture replace_texture(GpuTexture texture) const {
        _mipmapped = true;
        return std::exchange(_texture, std::move(texture));
    }
    bool mipmapped() const { return _mipmapped; }
    // Render targets store premultiplied alpha; sampling them out uses the
    // premultiplied blend (matches the SDL backend's BLEND_PREMULTIPLIED).
    bool premultiplied() const { return _premultiplied; }
    // Per-texture sampling. Defaults to Nearest to match the SDL backend's
    // create_texture_from_rgba (crisp bitmap fonts / pixel art); render targets use
    // Linear (blur). Mutable so set_scale_mode works through a const Texture&.
    ScaleMode scale_mode() const { return _scale; }
    void set_scale_mode(ScaleMode mode) const { _scale = mode; }
    // The backend's frame serial when a draw last used it (see retain()).
    u64 used_in_frame() const { return _used_in_frame; }
    void mark_used(u64 frame) const { _used_in_frame = frame; }

private:
    mutable GpuTexture _texture;
    mutable bool _mipmapped = false;
    bool _premultiplied = false;
    mutable ScaleMode _scale = ScaleMode::Nearest;
    TextureFormat _format = TextureFormat::Rgba8;
    mutable u64 _used_in_frame = 0;
};
// A storage buffer (kin/renderer/data_buffer.hpp).
class GpuDataBuffer : public IDataBufferBackend {
public:
    explicit GpuDataBuffer(GpuBuffer buffer) : _buffer(std::move(buffer)) {}
    std::size_t size() const override { return _buffer.size(); }
    const GpuBuffer& buffer() const { return _buffer; }
    u64 used_in_frame() const { return _used_in_frame; }
    void mark_used(u64 frame) const { _used_in_frame = frame; }

private:
    GpuBuffer _buffer;
    mutable u64 _used_in_frame = 0;
};
} // namespace gpu

class GpuRenderer2DBackend final : public IRenderer2DBackend {
public:
    explicit GpuRenderer2DBackend(Window& window, bool vsync = true);
    ~GpuRenderer2DBackend() override;

    GpuRenderer2DBackend(const GpuRenderer2DBackend&) = delete;
    GpuRenderer2DBackend& operator=(const GpuRenderer2DBackend&) = delete;

    std::string_view name() const override { return "SDL_GPU"; }
    RendererBackendCapabilities capabilities() const override;
    RendererBackendStats stats() const override {
        RendererBackendStats stats = _stats;
        std::tie(stats.texture_uploads, stats.texture_upload_submits) = _device.upload_counts();
        return stats;
    }
    void set_gpu_timing_enabled(bool enabled) override;
    std::string pipeline_record() const override;
    void prewarm_pipelines(std::string_view record) override;
    void set_overdraw_view(bool enabled) override { _overdraw_view = enabled; }
    void begin_gpu_scope(std::string_view name) override;
    void end_gpu_scope() override;
    std::vector<GpuScopeTiming> take_gpu_scope_timings() override { return std::exchange(_scope_timings, {}); }

    void clear(Color color) override;
    void present() override;
    bool save_png(const char* path) override;
    bool read_rgba(Rectf logical_region, std::vector<u8>& out, Vec2i& out_size) override;
    bool blit_region_to_target(Rectf region, const RenderTarget& dst) override;
    Vec2i region_pixel_size(Rectf region) const override;

    void set_logical_size(Vec2i size) override;
    void set_integer_logical_size(Vec2i size) override;
    void push_native_coordinates() override;
    void pop_native_coordinates() override;
    Vec2i output_size() const override;
    Vec2f window_to_logical(Vec2f window_px) const override;
    Vec2f logical_to_window(Vec2f logical) const override;

    Texture create_texture_from_rgba(const u8* pixels, Vec2i size) override;
    Texture create_texture(Vec2i size, TextureFormat format, const void* pixels) override;
    bool update_texture(const Texture& texture, Vec2i at, Vec2i size, const u8* pixels) override;
    bool write_texture(const Texture& texture, Vec2i at, Vec2i size, std::size_t bytes,
                       const std::function<void(std::span<u8>)>& fill) override;
    void draw_texture(const Texture& texture, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) override;
    void draw_sprites(const Texture& texture, std::span<const SpriteInstance> sprites) override;
    void set_job_system(JobSystem* jobs) override {
        _jobs = jobs;
        _device.set_job_system(jobs);
    }

    void fill_rect(Rectf rect, Color color) override;
    void draw_rect(Rectf rect, Color color) override;
    void draw_line(Vec2f a, Vec2f b, Color color) override;
    void fill_rounded_rect(Rectf rect, f32 radius, Color color) override;
    void draw_rounded_rect(Rectf rect, f32 radius, Color color, f32 width) override;
    void fill_gradient_rect(Rectf rect, const Gradient& gradient) override;

    RenderTarget create_render_target(Vec2i size, ScaleMode mode) override;
    void push_render_target(const RenderTarget& target) override;
    bool push_layer_target(const RenderTarget& target) override;
    std::optional<LayerBounds> pop_layer_target() override;
    Vec2i current_target_pixels() const override { return current_size(); }
    void pop_render_target() override;
    void set_scale_mode(const Texture& texture, ScaleMode mode) override;

    void set_blend_mode(BlendMode mode) override { _blend = mode; }
    void draw_shape_mesh(std::span<const ShapeVertex> vertices, std::span<const u32> indices, Color tint) override;
    void set_transform(const Affine2& transform) override {
        _transform = transform;
        _transformed = !transform.is_identity();
    }
    void set_viewport(Rectf rect) override;
    void reset_viewport() override;
    void push_viewport(Rectf rect) override;
    void pop_viewport() override;
    void push_clip(Rectf rect) override;
    void pop_clip() override;

    ShaderHandle create_shader(const ShaderDesc& desc) override;
    void draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params) override;
    void draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                             const Texture& source) override;
    void draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                             const Texture& source0, const Texture& source1) override;
    void draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                             std::span<const Texture> sources) override;
    void draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                             std::span<const Texture> sources, std::span<const DataBuffer> buffers) override;
    void draw_shader_geometry(std::span<const ShaderVertex> vertices, std::span<const u32> indices,
                              ShaderHandle handle, const ShaderParams& params,
                              std::span<const Texture> sources, std::span<const DataBuffer> buffers) override;
    bool reload_shader(ShaderHandle handle, const ShaderDesc& desc) override;
    ComputeShaderHandle create_compute_shader(ShaderBlob spirv, const ShaderLayout& layout) override;
    Texture create_storage_texture(Vec2i size, TextureFormat format) override;
    bool dispatch_compute(ComputeShaderHandle shader, Vec2i groups, const ComputeBindings& bindings) override;
    DataBuffer create_data_buffer(std::size_t bytes, const void* data) override;
    bool update_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes,
                            const void* data) override;
    bool write_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes,
                           const std::function<void(std::span<u8>)>& fill) override;

    void set_post_process(std::span<const PostProcessPass> passes) override;

private:
    // The blend a draw uses: its own (`natural`) under BlendMode::Alpha, else
    // the one set with set_blend_mode.
    gpu::GpuBlendMode resolve_blend(gpu::GpuBlendMode natural) const;
    BlendMode _blend = BlendMode::Alpha;

    void ensure_scene();          // (re)create the scene texture to the window size
    void ensure_frame();          // lazily begin a frame + batch on the active target
    void flush_to_frame();        // emit the current batch's pass into the frame
    void end_frame();             // after the frame is submitted: drop it and what it retained
    // Keeps `texture` alive until the frame is submitted. Queued draws hold only
    // its SDL handle, which SDL frees as soon as the texture is released, so a
    // texture dropped right after a draw would leave the batch a dangling handle
    // (and, once SDL destroys the image, a lost device).
    void retain(const Texture& texture);
    // Whether an upload of `size` at `at` may cycle the texture's storage.
    bool may_cycle(const gpu::GpuTextureBackend& texture, Vec2i at, Vec2i size) const;
    // A material draw's sampler bindings: `sources[i]` at slot i, the slots the
    // shader declares past them left null (the batch binds the white texture).
    struct SourceBindings {
        SDL_GPUTextureSamplerBinding slot0{};
        std::array<SDL_GPUTextureSamplerBinding, MaxShaderSamplers - 1> extra{};
        std::size_t extra_count = 0;
    };
    SourceBindings bind_sources(const gpu::GpuShader& shader, std::span<const Texture> sources);
    const gpu::GpuTexture& current_target() const; // pushed render target, else the scene
    Vec2i current_size() const;
    bool scene_uses_logical_coordinates() const;
    SDL_Rect current_scissor() const;
    // What current_scissor() was last made from, and what it made.
    struct ScissorKey {
        SDL_Rect clip{};
        f32 scale = 1.0f;
        Vec2i target{};
        bool operator==(const ScissorKey& o) const {
            return clip.x == o.clip.x && clip.y == o.clip.y && clip.w == o.clip.w && clip.h == o.clip.h &&
                   scale == o.scale && target == o.target;
        }
    };
    mutable std::optional<std::pair<ScissorKey, SDL_Rect>> _scissor_cache;
    void push_quad(Rectf dest, Rectf uv, Color color, SDL_GPUTexture* texture,
                   gpu::GpuBlendMode blend = gpu::GpuBlendMode::Alpha,
                   SDL_GPUSampler* sampler = nullptr);
    // Applies the active viewport offset in place on `tris` (a caller-owned scratch
    // span) and pushes it into the batch — no allocation or copy on the caller side.
    void push_triangles(std::span<gpu::GpuVertex> tris, SDL_GPUTexture* texture,
                        gpu::GpuBlendMode blend);
    // Maps vertices by the transform (set_transform), then shifts them by the
    // active viewport origin (SDL_SetRenderViewport semantics: draw coords are
    // viewport-relative). No-op with neither.
    void place(std::span<gpu::GpuVertex> verts) const;
    Vec2f place(Vec2f p) const {
        if (_transformed) {
            p = _transform.apply(p);
        }
        return {p.x + _view_offset.x, p.y + _view_offset.y};
    }
    // Run the post-process chain over `_scene` (ping-pong scratch RTs) and return the
    // final texture to present. Returns `&_scene` when the chain is empty / degraded.
    const gpu::GpuTexture* run_post_chain();

    Window* _window = nullptr;
    gpu::GpuDevice _device;
    gpu::GpuShader _vertex_shader;
    gpu::GpuShader _fragment_shader;
    gpu::GpuShader _instance_shader; // sprite_instanced.vert; without it draw_sprites draws quad by quad
    gpu::GpuShader _shader_vertex_shader; // shader_geometry.vert; without it draw_shader_geometry draws nothing
    gpu::GpuShader _overdraw_count_shader; // the overdraw view's: one layer a draw
    gpu::GpuShader _shape_shader;          // shape.frag: anti-aliased shapes (draw_shape_mesh)
    gpu::GpuShader _shape_vertex_shader;   // shape.vert: their indexed vertices
    std::vector<gpu::GpuShapeVertex> _shape_scratch;
    ShaderHandle _overdraw_heat{};         // its last pass: counts to colours (made on first use)
    bool _overdraw_view = false;
    std::vector<gpu::GpuShaderVertex> _shader_vertex_scratch;
    std::vector<gpu::GpuSpriteInstance> _instance_scratch;
    JobSystem* _jobs = nullptr; // splits large draw_sprites() batches when set
    SDL_GPUSampler* _sampler_linear = nullptr;  // render targets / blur
    SDL_GPUSampler* _sampler_nearest = nullptr; // default for uploaded textures (crisp text/pixel art)
    bool _data_textures = false; // the device can sample R16_UINT, R16G16_UINT and R32_FLOAT
    gpu::GpuTexture _white;
    gpu::GpuTexture _scene;
    Vec2i _scene_size{};
    gpu::GpuTexture _post_a;        // post-process ping-pong scratch (sized to _scene)
    gpu::GpuTexture _post_b;
    Vec2i _post_size{};
    std::vector<PostProcessPass> _post_passes; // full-scene post-fx chain (empty = none)
    gpu::GpuPipelineCache _pipelines;
    gpu::GpuGeometryBatch _batch;
    std::optional<gpu::GpuFrame> _frame;
    std::vector<std::shared_ptr<ITextureBackend>> _retained; // textures the frame's draws use
    std::vector<std::shared_ptr<IDataBufferBackend>> _retained_buffers; // and data buffers
    std::vector<gpu::GpuTexture> _retired_textures; // replaced this frame (made mipmapped), kept till submit
    SDL_GPUSampler* _sampler_mipmapped = nullptr;   // trilinear, for ScaleMode::Mipmapped
    SDL_GPUSampler* sampler_for(const gpu::GpuTextureBackend& texture) const;
    std::vector<SDL_GPUBuffer*> _storage_scratch;
    struct ComputePipeline {
        SDL_GPUComputePipeline* pipeline = nullptr;
        u32 samplers = 0;
    };
    std::vector<ComputePipeline> _compute_pipelines; // handle value - 1
    // The storage buffers a draw binds (retained till submit), or nullopt when
    // the shader wants more than it was given (the draw is skipped).
    std::optional<std::span<SDL_GPUBuffer* const>> bind_buffers(const gpu::GpuShader& shader,
                                                                std::span<const DataBuffer> buffers);
    const ITextureBackend* _last_retained = nullptr;         // skips repeats of the same texture
    u64 _frame_serial = 1; // counts end_frame(): which frame a texture was last drawn in
    std::unique_ptr<gpu::GpuFrameTimer> _gpu_timer; // set while GPU timing is on
    // Pipelines to make ahead: a shader is known across runs by its SPIR-V's
    // hash (0: the engine's default fragment shader).
    struct PipelineHint {
        u64 fragment = 0;
        gpu::GpuBlendMode blend = gpu::GpuBlendMode::Alpha;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        gpu::GpuVertexLayout layout = gpu::GpuVertexLayout::Triangles;
    };
    std::vector<PipelineHint> _pipeline_hints;
    std::unordered_map<SDL_GPUShader*, u64> _fragment_ids; // created shaders' hashes
    void make_hinted_pipelines(u64 fragment_id, SDL_GPUShader* fragment);
    u32 _untimed_frames = 0; // submitted without a fence since the last timed one (timer full)
    // The open gpu_scope(): its name, and whether it is timed (the timer had room).
    std::optional<std::string> _scope;
    bool _scope_timed = false;
    f64 _scope_pixels = 0.0; // the batch's pixel count when the scope began
    std::vector<GpuScopeTiming> _scope_timings; // finished, not yet taken
    // Submits what is recorded so far (the frame's batch included) with a fence
    // for the timer; the next draw starts a new command buffer.
    SDL_GPUFence* submit_for_scope();
    RendererBackendStats _stats;                    // present timings only
    SDL_FColor _clear_color{0.0f, 0.0f, 0.0f, 1.0f};
    Vec2i _logical_size{0, 0}; // 0 = render at window size (no logical presentation)
    bool _integer_scale = false;
    f32 _view_scale = 1.0f;    // logical -> native (scene texture) scale under logical presentation
    struct NativeState {
        Vec2i logical_size{};
        bool integer_scale = false;
        f32 view_scale = 1.0f;
        std::vector<SDL_Rect> clip_stack;
        Vec2f view_offset{0.0f, 0.0f};
        std::vector<Vec2f> view_offset_stack;
        Affine2 transform{};
    };
    std::vector<NativeState> _native_stack;
    std::vector<SDL_Rect> _clip_stack;
    // Pushed render targets; a layer's (push_layer_target) keeps the
    // coordinates it was pushed in, drawn at its own resolution.
    struct TargetEntry {
        const gpu::GpuTexture* texture = nullptr;
        Vec2f coords{}; // a layer's coordinate size; 0: the target's own pixels
    };
    std::vector<TargetEntry> _rt_stack;
    // The draw coordinates' extent on the current target: a layer's, the
    // logical size for a logical scene, else the target's pixels.
    Vec2f coordinate_size() const;
    Vec2i coordinate_extent() const; // the same, in whole units
    f32 coordinates_to_pixels() const; // the current target's pixels per draw unit
    std::vector<std::vector<SDL_Rect>> _saved_clip_stacks; // clip per pushed target
    std::vector<gpu::GpuShader> _shaders;                // custom material fragment shaders (handle = index+1)
    Vec2f _view_offset{0.0f, 0.0f};                      // active viewport origin (coord space); added to all verts
    std::vector<Vec2f> _view_offset_stack;              // saved offsets for push/pop_viewport
    std::vector<Vec2f> _saved_view_offsets;             // saved viewport origin per pushed render target
    Affine2 _transform{};                               // set_transform's; applied before the view offset
    bool _transformed = false;                          // _transform is not the identity
    std::vector<Affine2> _saved_transforms;             // the transform outside each pushed render target
    // Reusable scratch for the rounded-rect / gradient geometry path — cleared (not
    // freed) each call so the per-frame UI rebuild doesn't churn the heap. The render
    // path is single-threaded and each builder fully consumes these before returning.
    std::vector<gpu::GpuVertex> _scratch_verts;
    std::vector<Vec2f> _scratch_loop[4]; // perimeter point buffers (draw_rounded_rect uses all 4)
};

} // namespace kin
