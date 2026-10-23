#pragma once

#include <kin/platform/framebuffer_host.hpp>
#include <kin/renderer/backend.hpp>



namespace kin {
class Window;

// Experimental CPU RGBA renderer. Presentation is delegated to a platform host.
class CompactRenderer2DBackend final : public IRenderer2DBackend {
public:
    explicit CompactRenderer2DBackend(Window& window, bool vsync);
    explicit CompactRenderer2DBackend(FramebufferHost& host);
    std::string_view name() const override { return "compact"; }
    RendererBackendCapabilities capabilities() const override;
    RendererBackendStats stats() const override { return _stats; }
    void reset_stats() override { _stats = {}; }
    void clear(Color color) override;
    void present() override;
    bool read_rgba(Rectf region, std::vector<u8>& out, Vec2i& size) override;
    Vec2i region_pixel_size(Rectf region) const override;
    void set_logical_size(Vec2i size) override;
    void set_integer_logical_size(Vec2i size) override;
    void push_native_coordinates() override;
    void pop_native_coordinates() override;
    Vec2i output_size() const override;
    Vec2f window_to_logical(Vec2f point) const override;
    Vec2f logical_to_window(Vec2f point) const override;
    Texture create_texture_from_rgba(const u8* pixels, Vec2i size) override;
    void set_scale_mode(const Texture& texture, ScaleMode mode) override;
    void draw_texture(const Texture& texture, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) override;
    void draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) override;
    void fill_rect(Rectf rect, Color color) override;
    void draw_rect(Rectf rect, Color color) override;
    void draw_line(Vec2f a, Vec2f b, Color color) override;
    void set_blend_mode(BlendMode mode) override;
    void set_transform(const Affine2& transform) override;
    void set_viewport(Rectf rect) override;
    void reset_viewport() override { _view.viewport.reset(); }
    void push_viewport(Rectf rect) override;
    void pop_viewport() override;
    void push_clip(Rectf rect) override;
    void pop_clip() override;

private:
    struct View {
        std::optional<Rectf> viewport;
        std::optional<Rectf> clip;
    };
    struct Mapping { Vec2f origin; f32 scale; Rectf bounds; };
    void sync_size();
    Mapping mapping() const;
    Vec2f map(Vec2f point) const;
    Rectf map(Rectf rect) const;
    Rectf clip() const;
    void pixel(i32 x, i32 y, Color color);

    std::unique_ptr<FramebufferHost> _owned_host;
    FramebufferHost* _host;
    Vec2i _size{};
    Vec2i _logical{};
    bool _integer = false;
    View _view;
    std::vector<View> _view_stack, _clip_stack, _native_stack;
    std::vector<u8> _pixels;
    BlendMode _blend = BlendMode::Alpha;
    RendererBackendStats _stats;
};
} // namespace kin
