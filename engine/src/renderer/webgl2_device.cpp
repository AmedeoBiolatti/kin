#include "webgl2_device.hpp"

#include "embedded_webgl_shader.hpp"

#include <algorithm>
#include <kin/platform/window.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace kin {
namespace {
GLuint compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint size = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &size);
        std::vector<char> log(static_cast<std::size_t>(std::max(1, size)));
        glGetShaderInfoLog(shader, size, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string{"WebGL2 shader: "} + log.data());
    }
    return shader;
}
} // namespace

WebGLTexture::WebGLTexture(Vec2i size, std::shared_ptr<bool> alive)
    : ITextureBackend(Kind::WebGL2), dimensions(size), alive(std::move(alive)) {}
WebGLTexture::~WebGLTexture() {
    if (*alive) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteRenderbuffers(1, &depth);
        glDeleteTextures(1, &handle);
    }
}

WebGLDevice::WebGLDevice(Window& window, bool vsync) {
    _context = SDL_GL_CreateContext(static_cast<SDL_Window*>(window.native_handle()));
    if (!_context)
        throw std::runtime_error(std::string{"WebGL2 context: "} + SDL_GetError());
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &_max_texture_size);
    SDL_GL_SetSwapInterval(vsync ? 1 : 0);
}
WebGLDevice::~WebGLDevice() {
    *_alive = false;
    SDL_GL_DestroyContext(_context);
}

Texture WebGLDevice::create_texture(const u8* pixels, Vec2i size, ScaleMode mode) {
    if (size.x <= 0 || size.y <= 0 || size.x > _max_texture_size || size.y > _max_texture_size)
        return {};
    auto value = std::make_shared<WebGLTexture>(size, _alive);
    value->scale_mode = mode;
    glGenTextures(1, &value->handle);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, value->handle);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const GLint filter = mode == ScaleMode::Nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    value->mipmapped = mode == ScaleMode::Mipmapped;
    if (value->mipmapped) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }
    return Texture{std::move(value)};
}
Texture WebGLDevice::create_surface(Vec2i size, bool depth, ScaleMode mode) {
    GLint limit = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &limit);
    if (depth && (size.x > limit || size.y > limit))
        return {};
    auto value = create_texture(nullptr, size, mode == ScaleMode::Mipmapped ? ScaleMode::Linear : mode);
    auto surface = this->surface(value);
    if (!surface)
        return {};
    surface->flip_v = true;
    glGenFramebuffers(1, &surface->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, surface->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, surface->handle, 0);
    if (depth) {
        glGenRenderbuffers(1, &surface->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, surface->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.x, size.y);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, surface->depth);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("WebGL2 RGBA8/depth framebuffer is incomplete");
    return value;
}
const WebGLTexture* WebGLDevice::texture(const Texture& value) const {
    if (!value.backend() || value.backend()->kind() != ITextureBackend::Kind::WebGL2)
        return nullptr;
    const auto* result = static_cast<const WebGLTexture*>(value.backend().get());
    return result->alive == _alive && *_alive ? result : nullptr;
}
std::shared_ptr<WebGLTexture> WebGLDevice::surface(const Texture& value) const {
    return texture(value) ? std::static_pointer_cast<WebGLTexture>(value.backend()) : nullptr;
}
std::string WebGLDevice::shader_source(std::string_view name) {
    const auto bytes = embedded_webgl_shader(name);
    if (bytes.empty())
        throw std::runtime_error("Missing embedded WebGL2 shader: " + std::string(name));
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
GLuint WebGLDevice::program(const char* vertex, const char* fragment) {
    const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
    GLuint fs = 0;
    try {
        fs = compile(GL_FRAGMENT_SHADER, fragment);
    } catch (...) {
        glDeleteShader(vs);
        throw;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint size = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &size);
        std::vector<char> log(static_cast<std::size_t>(std::max(1, size)));
        glGetProgramInfoLog(program, size, nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error(std::string{"WebGL2 program: "} + log.data());
    }
    return program;
}
} // namespace kin
