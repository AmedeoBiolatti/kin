#pragma once

#include "webgl2_device.hpp"

#include <array>
#include <kin/platform/window.hpp>
#include <kin/renderer/backend.hpp>

namespace kin {
class WebGLRenderer3D;

// Browser sprites, geometry and offscreen 3D views share one GL context.
class WebGL2Renderer2DBackend final : public IRenderer2DBackend {
public:
    explicit WebGL2Renderer2DBackend(Window& window, bool vsync);
    ~WebGL2Renderer2DBackend() override;
    WebGL2Renderer2DBackend(const WebGL2Renderer2DBackend&) = delete;
    WebGL2Renderer2DBackend& operator=(const WebGL2Renderer2DBackend&) = delete;
    std::string_view name() const override { return "WebGL2"; }
    RendererBackendCapabilities capabilities() const override;
    RendererBackendStats stats() const override { return _stats; }
    void reset_stats() override { _stats = {}; }
    void clear(Color color) override;
    void present() override;
    void set_logical_size(Vec2i size) override;
    void set_integer_logical_size(Vec2i size) override;
    void push_native_coordinates() override;
    void pop_native_coordinates() override;
    Vec2i output_size() const override { return _window.pixel_size(); }
    Vec2f window_to_logical(Vec2f point) const override;
    Vec2f logical_to_window(Vec2f point) const override;
    Texture create_texture_from_rgba(const u8* pixels, Vec2i size) override;
    bool update_texture(const Texture& texture, Vec2i at, Vec2i size, const u8* pixels) override;
    void set_scale_mode(const Texture& texture, ScaleMode mode) override;
    void draw_texture(const Texture& texture, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) override;
    void fill_rect(Rectf rect, Color color) override;
    void draw_rect(Rectf rect, Color color) override;
    void draw_line(Vec2f a, Vec2f b, Color color) override;
    void draw_shape_mesh(std::span<const ShapeVertex> vertices, std::span<const u32> indices, u32 index_base,
                         Color tint) override;
    bool draw_shape_primitives(std::span<const ShapePrimitive> primitives, Color tint) override;
    void set_transform(const Affine2& value) override { _transform = value; }
    void set_blend_mode(BlendMode mode) override;
    void set_viewport(Rectf rect) override;
    void reset_viewport() override;
    void push_viewport(Rectf rect) override;
    void pop_viewport() override;
    void push_clip(Rectf rect) override;
    void pop_clip() override;
    Vec2i current_target_pixels() const override;
    RenderTarget create_render_target(Vec2i size, ScaleMode mode) override;
    void push_render_target(const RenderTarget& target) override;
    void pop_render_target() override;
    bool read_rgba(Rectf region, std::vector<u8>& pixels, Vec2i& size) override;
    IRendererBackend3D* renderer_3d() override;

    // 3D passes flush queued 2D work. Every 2D flush reapplies its own state.
    void before_3d() { flush(); }
    WebGLDevice& device() { return *_device; }

private:
    struct Vertex {
        Vec2f position;
        Vec2f uv;
        Color color;
    };
    struct Mapping {
        f32 scale;
        Vec2f offset;
    };
    struct SdfInstance {
        std::array<f32, 12> params;
        Color fill, stroke;
    };
    Mapping mapping() const;
    Vec2f to_pixel(Vec2f point) const;
    void flush();
    void destroy();
    void scissor();
    void bind_target();
    void apply_blend(bool premultiplied);
    void use_texture(const Texture& texture);
    void vertex(Vec2f point, Vec2f uv, Color color);
    void quad_indices(u32 first);
    Window& _window;
    std::unique_ptr<WebGLDevice> _device;
#ifdef KIN_EXPERIMENTAL_3D
    std::unique_ptr<WebGLRenderer3D> _renderer3d;
#endif
    GLuint _program = 0, _vao = 0, _buffer = 0, _element_buffer = 0;
    GLuint _sdf_program = 0, _sdf_vao = 0, _sdf_buffer = 0, _sdf_corner_buffer = 0;
    GLint _sdf_output_uniform = -1;
    GLint _output_uniform = -1, _premultiplied_uniform = -1;
    Texture _white, _texture;
    std::vector<Vertex> _vertices;
    std::vector<u32> _indices;
    std::vector<SdfInstance> _sdf_instances;
    Vec2i _logical{};
    bool _integer = false;
    i32 _native_depth = 0;
    Affine2 _transform{};
    std::optional<Rectf> _viewport, _clip;
    std::vector<std::optional<Rectf>> _viewports, _clips;
    BlendMode _blend = BlendMode::Alpha;
    std::shared_ptr<WebGLTexture> _target;
    struct TargetState {
        std::shared_ptr<WebGLTexture> target;
        Vec2i logical;
        bool integer;
        i32 native_depth;
        std::optional<Rectf> viewport, clip;
        std::vector<std::optional<Rectf>> viewports, clips;
    };
    std::vector<TargetState> _targets;
    RendererBackendStats _stats{};
};
} // namespace kin
