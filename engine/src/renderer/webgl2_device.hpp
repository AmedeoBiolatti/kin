#pragma once

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>
#include <kin/renderer/render_target.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace kin {
class Window;

// Resources retain only a liveness token: a texture or mesh may outlive the
// renderer, but must never keep an SDL context alive after its window.
class WebGLTexture final : public ITextureBackend {
public:
    explicit WebGLTexture(Vec2i size, std::shared_ptr<bool> alive);
    ~WebGLTexture() override;
    Vec2i size() const override { return dimensions; }
    GLuint handle = 0, framebuffer = 0, depth = 0;
    Vec2i dimensions;
    ScaleMode scale_mode = ScaleMode::Nearest;
    bool flip_v = false, mipmapped = false;
    std::shared_ptr<bool> alive;
};

class WebGLDevice {
public:
    WebGLDevice(Window& window, bool vsync);
    ~WebGLDevice();
    WebGLDevice(const WebGLDevice&) = delete;
    WebGLDevice& operator=(const WebGLDevice&) = delete;
    Texture create_texture(const u8* pixels, Vec2i size, ScaleMode mode = ScaleMode::Nearest);
    Texture create_surface(Vec2i size, bool depth, ScaleMode mode = ScaleMode::Linear);
    const WebGLTexture* texture(const Texture& value) const;
    std::shared_ptr<WebGLTexture> surface(const Texture& value) const;
    const std::shared_ptr<bool>& alive() const { return _alive; }
    i32 max_texture_size() const { return _max_texture_size; }
    static GLuint program(const char* vertex, const char* fragment);
    static std::string shader_source(std::string_view name);

private:
    SDL_GLContext _context = nullptr;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    i32 _max_texture_size = 0;
};
} // namespace kin
