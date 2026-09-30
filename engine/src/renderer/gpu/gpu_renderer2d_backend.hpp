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
#include "gpu_geometry_batch.hpp"
#include "gpu_pipeline_cache.hpp"
#include "gpu_shader.hpp"
#include "gpu_texture.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
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
    // Render targets store premultiplied alpha; sampling them out uses the
    // premultiplied blend (matches the SDL backend's BLEND_PREMULTIPLIED).
    bool premultiplied() const { return _premultiplied; }
    // Per-texture sampling. Defaults to Nearest to match the SDL backend's
    // create_texture_from_rgba (crisp bitmap fonts / pixel art); render targets use
    // Linear (blur). Mutable so set_scale_mode works through a const Texture&.
    ScaleMode scale_mode() const { return _scale; }
    void set_scale_mode(ScaleMode mode) const { _scale = mode; }

private:
    GpuTexture _texture;
    bool _premultiplied = false;
    mutable ScaleMode _scale = ScaleMode::Nearest;
    TextureFormat _format = TextureFormat::Rgba8;
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
    void draw_texture(const Texture& texture, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) override;
    void draw_sprites(const Texture& texture, std::span<const SpriteInstance> sprites) override;

    void fill_rect(Rectf rect, Color color) override;
    void draw_rect(Rectf rect, Color color) override;
    void draw_line(Vec2f a, Vec2f b, Color color) override;
    void fill_rounded_rect(Rectf rect, f32 radius, Color color) override;
    void draw_rounded_rect(Rectf rect, f32 radius, Color color, f32 width) override;
    void fill_gradient_rect(Rectf rect, const Gradient& gradient) override;

    RenderTarget create_render_target(Vec2i size, ScaleMode mode) override;
    void push_render_target(const RenderTarget& target) override;
    void pop_render_target() override;
    void set_scale_mode(const Texture& texture, ScaleMode mode) override;

    void set_blend_mode(BlendMode mode) override { _blend = mode; }
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

    void set_post_process(std::span<const PostProcessPass> passes) override;

private:
    // The blend a draw uses: its own (`natural`) under BlendMode::Alpha, else
    // the one set with set_blend_mode.
    gpu::GpuBlendMode resolve_blend(gpu::GpuBlendMode natural) const;
    BlendMode _blend = BlendMode::Alpha;

    void ensure_scene();          // (re)create the scene texture to the window size
    void ensure_frame();          // lazily begin a frame + batch on the active target
    void flush_to_frame();        // emit the current batch's pass into the frame
    const gpu::GpuTexture& current_target() const; // pushed render target, else the scene
    Vec2i current_size() const;
    bool scene_uses_logical_coordinates() const;
    SDL_Rect current_scissor() const;
    void push_quad(Rectf dest, Rectf uv, Color color, SDL_GPUTexture* texture,
                   gpu::GpuBlendMode blend = gpu::GpuBlendMode::Alpha,
                   SDL_GPUSampler* sampler = nullptr);
    // Applies the active viewport offset in place on `tris` (a caller-owned scratch
    // span) and pushes it into the batch — no allocation or copy on the caller side.
    void push_triangles(std::span<gpu::GpuVertex> tris, SDL_GPUTexture* texture,
                        gpu::GpuBlendMode blend);
    // Shifts vertices by the active viewport origin (SDL_SetRenderViewport semantics:
    // draw coords are viewport-relative). No-op when no viewport offset is set.
    void apply_view_offset(std::span<gpu::GpuVertex> verts) const;
    // Run the post-process chain over `_scene` (ping-pong scratch RTs) and return the
    // final texture to present. Returns `&_scene` when the chain is empty / degraded.
    const gpu::GpuTexture* run_post_chain();

    Window* _window = nullptr;
    gpu::GpuDevice _device;
    gpu::GpuShader _vertex_shader;
    gpu::GpuShader _fragment_shader;
    gpu::GpuShader _instance_shader; // sprite_instanced.vert; without it draw_sprites draws quad by quad
    std::vector<gpu::GpuSpriteInstance> _instance_scratch;
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
    };
    std::vector<NativeState> _native_stack;
    std::vector<SDL_Rect> _clip_stack;
    std::vector<const gpu::GpuTexture*> _rt_stack;       // pushed render targets
    std::vector<std::vector<SDL_Rect>> _saved_clip_stacks; // clip per pushed target
    std::vector<gpu::GpuShader> _shaders;                // custom material fragment shaders (handle = index+1)
    Vec2f _view_offset{0.0f, 0.0f};                      // active viewport origin (coord space); added to all verts
    std::vector<Vec2f> _view_offset_stack;              // saved offsets for push/pop_viewport
    std::vector<Vec2f> _saved_view_offsets;             // saved viewport origin per pushed render target
    // Reusable scratch for the rounded-rect / gradient geometry path — cleared (not
    // freed) each call so the per-frame UI rebuild doesn't churn the heap. The render
    // path is single-threaded and each builder fully consumes these before returning.
    std::vector<gpu::GpuVertex> _scratch_verts;
    std::vector<Vec2f> _scratch_loop[4]; // perimeter point buffers (draw_rounded_rect uses all 4)
};

} // namespace kin
