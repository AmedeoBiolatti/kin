#pragma once

#include <kin/platform/window.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/texture.hpp>

#include <SDL3/SDL.h>

#include <vector>

namespace kin {

class SdlRenderer2DBackend final : public IRenderer2DBackend {
public:
    explicit SdlRenderer2DBackend(Window& window, bool vsync = false);
    ~SdlRenderer2DBackend() override;

    SdlRenderer2DBackend(const SdlRenderer2DBackend&) = delete;
    SdlRenderer2DBackend& operator=(const SdlRenderer2DBackend&) = delete;

    std::string_view name() const override;
    RendererBackendCapabilities capabilities() const override;
    RendererBackendStats stats() const override;
    void reset_stats() override;
    void set_texture_batching_enabled(bool enabled) override;
    bool texture_batching_enabled() const override;

    void clear(Color color) override;
    void present() override;
    bool save_png(const char* path) override;
    bool read_rgba(Rectf logical_region, std::vector<u8>& out, Vec2i& out_size) override;

    void set_logical_size(Vec2i size) override;
    void set_integer_logical_size(Vec2i size) override;
    void push_native_coordinates() override;
    void pop_native_coordinates() override;
    Vec2i output_size() const override;
    Vec2f window_to_logical(Vec2f window_px) const override;
    Vec2f logical_to_window(Vec2f logical) const override;

    Texture create_texture_from_rgba(const u8* pixels, Vec2i size) override;
    void draw_texture(const Texture& texture, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) override;

    void fill_rect(Rectf rect, Color color) override;
    void draw_rect(Rectf rect, Color color) override;
    void fill_rounded_rect(Rectf rect, f32 radius, Color color) override;
    void draw_rounded_rect(Rectf rect, f32 radius, Color color, f32 width = 1.0f) override;
    void fill_gradient_rect(Rectf rect, const Gradient& gradient) override;
    ShaderHandle create_shader(const ShaderDesc& desc) override;
    void draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params) override;
    void draw_line(Vec2f a, Vec2f b, Color color) override;

    RenderTarget create_render_target(Vec2i size, ScaleMode mode) override;
    void push_render_target(const RenderTarget& target) override;
    void pop_render_target() override;
    void set_scale_mode(const Texture& texture, ScaleMode mode) override;

    void set_blend_mode(BlendMode mode) override;
    void set_viewport(Rectf rect) override;
    void reset_viewport() override;
    void push_viewport(Rectf rect) override;
    void pop_viewport() override;
    void push_clip(Rectf rect) override;
    void pop_clip() override;

private:
    enum class BatchKind {
        None,
        Texture,
        Color,
    };

    struct GeometryBatch {
        BatchKind kind = BatchKind::None;
        Texture retained_texture;
        SDL_Texture* texture = nullptr;
        std::vector<SDL_Vertex> vertices;
        std::vector<int> indices;
    };

    struct ViewportState {
        SDL_Rect viewport = {};
        SDL_Rect clip = {};
        bool viewport_set = false;
        bool clip_enabled = false;
    };

    struct LogicalPresentationState {
        int width = 0;
        int height = 0;
        SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    };

    ViewportState capture_viewport() const;
    void restore_viewport(const ViewportState& state);
    void flush_batch();
    void begin_batch(BatchKind kind, SDL_Texture* texture, Texture retained_texture = {});
    void append_quad(Rectf dest, Rectf source, Vec2i texture_size, Color color, f32 rotation = 0.0f, Vec2f pivot = {0.5f, 0.5f});

    struct ShaderEntry {
        SDL_GPUShader* shader = nullptr;
        SDL_GPURenderState* state = nullptr;
        u32 num_uniform_buffers = 0;
    };

    SDL_Renderer* _handle = nullptr;
    SDL_GPUDevice* _gpu_device = nullptr; // non-null when the renderer is on the "gpu" driver
    std::vector<ShaderEntry> _shaders;    // ShaderHandle.value == index + 1
    Texture _white_texture;               // 1x1 white, for shader-surface quads (lazy)
    std::vector<ViewportState> _viewport_stack;
    std::vector<ViewportState> _clip_stack;
    std::vector<LogicalPresentationState> _logical_presentation_stack;
    std::vector<SDL_Texture*> _render_target_stack;
    GeometryBatch _batch;
    BlendMode _blend = BlendMode::Alpha;
    RendererBackendStats _stats;
    bool _texture_batching_enabled = true;
};

class SdlTextureBackend final : public ITextureBackend {
public:
    SdlTextureBackend(SDL_Texture* texture, Vec2i size);
    ~SdlTextureBackend() override;

    SdlTextureBackend(const SdlTextureBackend&) = delete;
    SdlTextureBackend& operator=(const SdlTextureBackend&) = delete;

    Vec2i size() const override { return _size; }
    SDL_Texture* handle() const { return _texture; }

private:
    SDL_Texture* _texture = nullptr;
    Vec2i _size{};
};

} // namespace kin
