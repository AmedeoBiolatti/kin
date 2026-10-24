#include "webgl2_renderer2d_backend.hpp"

#ifdef KIN_EXPERIMENTAL_3D
#include "webgl2_renderer3d.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <kin/platform/log.hpp>
#include <stdexcept>
#include <string>

namespace kin {
namespace {
Rectf intersection(Rectf a, Rectf b) {
    const f32 x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0f, std::min(a.x + a.w, b.x + b.w) - x),
            std::max(0.0f, std::min(a.y + a.h, b.y + b.h) - y)};
}

Color multiply(Color a, Color b) {
    return {u8(u32(a.r) * b.r / 255), u8(u32(a.g) * b.g / 255), u8(u32(a.b) * b.b / 255), u8(u32(a.a) * b.a / 255)};
}
} // namespace

WebGL2Renderer2DBackend::WebGL2Renderer2DBackend(Window& window, bool vsync) : _window(window) {
    _device = std::make_unique<WebGLDevice>(window, vsync);
    try {
        constexpr auto vertex_source = R"(#version 300 es
precision highp float;
layout(location=0) in vec2 aPosition;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aColor;
uniform vec2 uOutput;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = vec4(aPosition / uOutput * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
})";
        constexpr auto fragment_source = R"(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
uniform bool uPremultiplied;
in vec2 vUV;
in vec4 vColor;
out vec4 outColor;
void main() {
    outColor = texture(uTexture, vUV) * vColor;
    if (uPremultiplied) outColor.rgb *= vColor.a;
}
)";
        _program = WebGLDevice::program(vertex_source, fragment_source);
        glUseProgram(_program);
        _output_uniform = glGetUniformLocation(_program, "uOutput");
        _premultiplied_uniform = glGetUniformLocation(_program, "uPremultiplied");
        glUniform1i(glGetUniformLocation(_program, "uTexture"), 0);
        glGenVertexArrays(1, &_vao);
        glBindVertexArray(_vao);
        glGenBuffers(1, &_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, _buffer);
        glGenBuffers(1, &_element_buffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _element_buffer);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, color)));
        const auto sdf_vertex = WebGLDevice::shader_source("sdf2d.vert.glsl");
        const auto sdf_fragment = WebGLDevice::shader_source("sdf2d.frag.glsl");
        _sdf_program = WebGLDevice::program(sdf_vertex.c_str(), sdf_fragment.c_str());
        _sdf_output_uniform = glGetUniformLocation(_sdf_program, "uOutput");
        glGenVertexArrays(1, &_sdf_vao);
        glBindVertexArray(_sdf_vao);
        glGenBuffers(1, &_sdf_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, _sdf_buffer);
        for (GLuint i = 0; i < 5; ++i) {
            glEnableVertexAttribArray(i);
            const bool color = i >= 3;
            const std::size_t offset =
                color ? offsetof(SdfInstance, fill) + (i - 3) * sizeof(Color) : i * 4 * sizeof(f32);
            glVertexAttribPointer(i, 4, color ? GL_UNSIGNED_BYTE : GL_FLOAT, color ? GL_TRUE : GL_FALSE,
                                  sizeof(SdfInstance), reinterpret_cast<void*>(offset));
            glVertexAttribDivisor(i, 1);
        }
        // A divisor-zero attribute is required for portable WebGL instancing.
        constexpr std::array<Vec2f, 6> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}};
        glGenBuffers(1, &_sdf_corner_buffer);
        glBindBuffer(GL_ARRAY_BUFFER, _sdf_corner_buffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(Vec2f), nullptr);
        glActiveTexture(GL_TEXTURE0);
        const std::array<u8, 4> white{255, 255, 255, 255};
        _white = create_texture_from_rgba(white.data(), {1, 1});
        set_blend_mode(BlendMode::Alpha);
#ifdef KIN_EXPERIMENTAL_3D
        _renderer3d = std::make_unique<WebGLRenderer3D>(*this);
#endif
        KIN_LOG_INFO("render", "WebGL2 renderer created (OpenGL ES 3.0)");
    } catch (...) {
        destroy();
        throw;
    }
}

WebGL2Renderer2DBackend::~WebGL2Renderer2DBackend() {
    destroy();
}

void WebGL2Renderer2DBackend::destroy() {
    _texture = {};
    _white = {};
#ifdef KIN_EXPERIMENTAL_3D
    _renderer3d.reset();
#endif
    _target.reset();
    _targets.clear();
    if (_buffer)
        glDeleteBuffers(1, &_buffer);
    if (_element_buffer)
        glDeleteBuffers(1, &_element_buffer);
    if (_vao)
        glDeleteVertexArrays(1, &_vao);
    if (_program)
        glDeleteProgram(_program);
    if (_sdf_buffer)
        glDeleteBuffers(1, &_sdf_buffer);
    if (_sdf_corner_buffer)
        glDeleteBuffers(1, &_sdf_corner_buffer);
    if (_sdf_vao)
        glDeleteVertexArrays(1, &_sdf_vao);
    if (_sdf_program)
        glDeleteProgram(_sdf_program);
    _device.reset();
}

RendererBackendCapabilities WebGL2Renderer2DBackend::capabilities() const {
    RendererBackendCapabilities result{.immediate_2d = true,
                                       .queued_2d = true,
                                       .render_targets = true,
                                       .blend_modes = true,
                                       .min_max_blend = true,
                                       .transforms = true,
                                       .shapes = true};
    result.shape_primitives = true;
#ifdef KIN_EXPERIMENTAL_3D
    result.rendering_3d = bool(_renderer3d);
#endif
    return result;
}

WebGL2Renderer2DBackend::Mapping WebGL2Renderer2DBackend::mapping() const {
    if (_native_depth > 0 || _logical.x <= 0 || _logical.y <= 0)
        return {1.0f, {}};
    const Vec2i size = current_target_pixels();
    f32 scale = std::min(f32(size.x) / _logical.x, f32(size.y) / _logical.y);
    if (_integer && scale >= 1.0f)
        scale = std::floor(scale);
    scale = std::max(scale, 0.0001f);
    return {scale, {(size.x - _logical.x * scale) * 0.5f, (size.y - _logical.y * scale) * 0.5f}};
}

Vec2f WebGL2Renderer2DBackend::to_pixel(Vec2f point) const {
    const auto m = mapping();
    return {point.x * m.scale + m.offset.x, point.y * m.scale + m.offset.y};
}

Vec2f WebGL2Renderer2DBackend::window_to_logical(Vec2f point) const {
    const Vec2i pixels = output_size(), window = _window.size();
    const auto m = mapping();
    return {(point.x * pixels.x / std::max(1, window.x) - m.offset.x) / m.scale,
            (point.y * pixels.y / std::max(1, window.y) - m.offset.y) / m.scale};
}

Vec2f WebGL2Renderer2DBackend::logical_to_window(Vec2f point) const {
    const Vec2f p = to_pixel(point);
    const Vec2i pixels = output_size(), window = _window.size();
    return {p.x * window.x / std::max(1, pixels.x), p.y * window.y / std::max(1, pixels.y)};
}

void WebGL2Renderer2DBackend::set_logical_size(Vec2i size) {
    flush();
    _logical = size;
    _integer = false;
}
void WebGL2Renderer2DBackend::set_integer_logical_size(Vec2i size) {
    set_logical_size(size);
    _integer = true;
}
void WebGL2Renderer2DBackend::push_native_coordinates() {
    flush();
    ++_native_depth;
}
void WebGL2Renderer2DBackend::pop_native_coordinates() {
    flush();
    _native_depth = std::max(0, _native_depth - 1);
}

void WebGL2Renderer2DBackend::scissor() {
    const Vec2i size = current_target_pixels();
    const auto m = mapping();
    Rectf bounds{0, 0, f32(size.x), f32(size.y)};
    if (_native_depth == 0 && _logical.x > 0 && _logical.y > 0) {
        bounds = {m.offset.x, m.offset.y, _logical.x * m.scale, _logical.y * m.scale};
    }
    if (_viewport) {
        const auto p = to_pixel({_viewport->x, _viewport->y});
        bounds = intersection(bounds, {p.x, p.y, _viewport->w * m.scale, _viewport->h * m.scale});
    }
    if (_clip) {
        const auto p = to_pixel({_clip->x + (_viewport ? _viewport->x : 0), _clip->y + (_viewport ? _viewport->y : 0)});
        bounds = intersection(bounds, {p.x, p.y, _clip->w * m.scale, _clip->h * m.scale});
    }
    const int x = int(std::floor(bounds.x)), y = int(std::floor(bounds.y));
    const int right = int(std::ceil(bounds.x + bounds.w)), bottom = int(std::ceil(bounds.y + bounds.h));
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, size.y - bottom, std::max(0, right - x), std::max(0, bottom - y));
}

void WebGL2Renderer2DBackend::flush() {
    if (_vertices.empty() && _sdf_instances.empty())
        return;
    const auto* texture = _device->texture(_texture);
    const Vec2i size = current_target_pixels();
    if (texture && size.x > 0 && size.y > 0) {
        bind_target();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        apply_blend(texture->flip_v);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, 0);
        glViewport(0, 0, size.x, size.y);
        scissor();
        if (!_sdf_instances.empty()) {
            glUseProgram(_sdf_program);
            glUniform2f(_sdf_output_uniform, f32(size.x), f32(size.y));
            glBindVertexArray(_sdf_vao);
            glBindBuffer(GL_ARRAY_BUFFER, _sdf_buffer);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(_sdf_instances.size() * sizeof(SdfInstance)),
                         _sdf_instances.data(), GL_STREAM_DRAW);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(_sdf_instances.size()));
        } else {
            glUseProgram(_program);
            glUniform2f(_output_uniform, f32(size.x), f32(size.y));
            glUniform1i(_premultiplied_uniform, texture->flip_v);
            glBindVertexArray(_vao);
            glBindBuffer(GL_ARRAY_BUFFER, _buffer);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(_vertices.size() * sizeof(Vertex)), _vertices.data(),
                         GL_STREAM_DRAW);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(_indices.size() * sizeof(u32)), _indices.data(),
                         GL_STREAM_DRAW);
            glBindTexture(GL_TEXTURE_2D, texture->handle);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glDrawElements(GL_TRIANGLES, GLsizei(_indices.size()), GL_UNSIGNED_INT, nullptr);
        }
        ++_stats.texture_batch_flushes;
    }
    _vertices.clear();
    _indices.clear();
    _sdf_instances.clear();
}

void WebGL2Renderer2DBackend::clear(Color color) {
    flush();
    bind_target();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const f32 alpha = color.a / 255.0f, scale = _target ? alpha / 255.0f : 1.0f / 255.0f;
    glClearColor(color.r * scale, color.g * scale, color.b * scale, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
}
void WebGL2Renderer2DBackend::present() {
    flush();
    SDL_GL_SwapWindow(static_cast<SDL_Window*>(_window.native_handle()));
}

Texture WebGL2Renderer2DBackend::create_texture_from_rgba(const u8* pixels, Vec2i size) {
    if (!pixels || size.x <= 0 || size.y <= 0)
        return {};
    flush();
    return _device->create_texture(pixels, size);
}

bool WebGL2Renderer2DBackend::update_texture(const Texture& texture, Vec2i at, Vec2i size, const u8* pixels) {
    const auto* gl = _device->texture(texture);
    if (!gl || !pixels || at.x < 0 || at.y < 0 || size.x <= 0 || size.y <= 0 || at.x > gl->dimensions.x - size.x ||
        at.y > gl->dimensions.y - size.y)
        return false;
    flush();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->handle);
    glTexSubImage2D(GL_TEXTURE_2D, 0, at.x, at.y, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    if (gl->mipmapped)
        glGenerateMipmap(GL_TEXTURE_2D);
    return true;
}

void WebGL2Renderer2DBackend::set_scale_mode(const Texture& texture, ScaleMode mode) {
    const auto* gl = _device->texture(texture);
    if (!gl)
        return;
    flush();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->handle);
    const GLint filter = mode == ScaleMode::Nearest ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    mode == ScaleMode::Mipmapped && !gl->flip_v ? GL_LINEAR_MIPMAP_LINEAR : filter);
    auto surface = _device->surface(texture);
    surface->mipmapped = mode == ScaleMode::Mipmapped && !gl->flip_v;
    surface->scale_mode = mode == ScaleMode::Mipmapped && gl->flip_v ? ScaleMode::Linear : mode;
    if (mode == ScaleMode::Mipmapped && !gl->flip_v)
        glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

void WebGL2Renderer2DBackend::use_texture(const Texture& texture) {
    if (!_sdf_instances.empty() || _texture.backend() != texture.backend() || _vertices.size() >= 65532)
        flush();
    _texture = texture;
}

void WebGL2Renderer2DBackend::vertex(Vec2f point, Vec2f uv, Color color) {
    point = _transform.apply(point);
    if (_viewport) {
        point.x += _viewport->x;
        point.y += _viewport->y;
    }
    _vertices.push_back({to_pixel(point), uv, color});
}

bool WebGL2Renderer2DBackend::draw_shape_primitives(std::span<const ShapePrimitive> primitives, Color tint) {
    if (primitives.empty() || tint.a == 0)
        return true;
    if (!_vertices.empty())
        flush();
    _texture = _white;
    const auto coordinates = mapping();
    for (const auto& p : primitives) {
        if (p.fill.a == 0 && (p.stroke.a == 0 || p.stroke_width <= 0.0f))
            continue;
        Affine2 m = _transform * p.transform;
        m.tx += _viewport ? _viewport->x : 0;
        m.ty += _viewport ? _viewport->y : 0;
        m.a *= coordinates.scale;
        m.b *= coordinates.scale;
        m.c *= coordinates.scale;
        m.d *= coordinates.scale;
        m.tx = m.tx * coordinates.scale + coordinates.offset.x;
        m.ty = m.ty * coordinates.scale + coordinates.offset.y;
        const f32 sx = std::hypot(m.a, m.b), sy = std::hypot(m.c, m.d);
        const f32 margin = 1.5f / std::max(std::min(sx, sy), 1e-6f);
        const bool stroked = p.stroke.a > 0 && p.stroke_width > 0;
        const f32 radius = std::clamp(p.radius, 0.0f, std::min(p.half_size.x, p.half_size.y));
        const f32 kind = p.kind == PathPrimitive::Kind::Ellipse ? 2.0f : radius <= 0.0f && !p.round_join ? 1.0f : 0.0f;
        _sdf_instances.push_back({{m.a, m.b, m.c, m.d, m.tx, m.ty, p.half_size.x, p.half_size.y, radius,
                                   stroked ? p.stroke_width * 0.5f : 0.0f, margin, kind},
                                  multiply(p.fill, tint),
                                  multiply(stroked ? p.stroke : colors::transparent, tint)});
    }
    return true;
}

void WebGL2Renderer2DBackend::quad_indices(u32 first) {
    _indices.insert(_indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

void WebGL2Renderer2DBackend::draw_texture(const Texture& texture, Rectf dest) {
    const auto size = texture.size();
    draw_texture(texture, {0, 0, f32(size.x), f32(size.y)}, dest);
}
void WebGL2Renderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest) {
    draw_texture(texture, source, dest, colors::white);
}
void WebGL2Renderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) {
    draw_texture(texture, source, dest, tint, 0, {0.5f, 0.5f});
}
void WebGL2Renderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation,
                                           Vec2f pivot) {
    const auto* gl = _device->texture(texture);
    if (!gl)
        return;
    if (_target.get() == gl)
        throw std::logic_error("WebGL2 cannot sample the active render target");
    use_texture(texture);
    const f32 radians = rotation * 0.01745329252f;
    const f32 c = std::cos(radians), s = std::sin(radians);
    const Vec2f center{dest.x + dest.w * pivot.x, dest.y + dest.h * pivot.y};
    const u32 first = u32(_vertices.size());
    constexpr std::array<Vec2f, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (const auto q : corners) {
        const Vec2f p{dest.x + q.x * dest.w - center.x, dest.y + q.y * dest.h - center.y};
        f32 v = (source.y + q.y * source.h) / gl->dimensions.y;
        if (gl->flip_v)
            v = 1.0f - v;
        vertex({center.x + p.x * c - p.y * s, center.y + p.x * s + p.y * c},
               {(source.x + q.x * source.w) / gl->dimensions.x, v}, tint);
    }
    quad_indices(first);
    ++_stats.texture_draws_submitted;
}

void WebGL2Renderer2DBackend::fill_rect(Rectf rect, Color color) {
    draw_texture(_white, {0, 0, 1, 1}, rect, color);
    ++_stats.rect_fills_submitted;
}
void WebGL2Renderer2DBackend::draw_rect(Rectf r, Color color) {
    draw_line({r.x, r.y}, {r.x + r.w, r.y}, color);
    draw_line({r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, color);
    draw_line({r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}, color);
    draw_line({r.x, r.y + r.h}, {r.x, r.y}, color);
}
void WebGL2Renderer2DBackend::draw_line(Vec2f a, Vec2f b, Color color) {
    use_texture(_white);
    const f32 dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
    if (length < 0.0001f) {
        fill_rect({a.x, a.y, 1, 1}, color);
        return;
    }
    const Vec2f n{-dy / length * 0.5f, dx / length * 0.5f};
    const u32 first = u32(_vertices.size());
    for (Vec2f p : {Vec2f{a.x + n.x, a.y + n.y}, Vec2f{b.x + n.x, b.y + n.y}, Vec2f{b.x - n.x, b.y - n.y},
                    Vec2f{a.x - n.x, a.y - n.y}})
        vertex(p, {}, color);
    quad_indices(first);
}

void WebGL2Renderer2DBackend::draw_shape_mesh(std::span<const ShapeVertex> vertices, std::span<const u32> indices,
                                              u32 base, Color tint) {
    use_texture(_white);
    const u32 first = u32(_vertices.size());
    // UI meshes normally reuse vertices across many triangles. Sparse input
    // retains the expanded path so unused vertices do not become extra work.
    const bool indexed = vertices.size() <= indices.size();
    if (indexed)
        for (const auto& v : vertices)
            vertex(v.position, {}, multiply(v.color, tint));
    for (const u32 index : indices) {
        if (index < base || index - base >= vertices.size())
            throw std::out_of_range("WebGL2 shape index");
        if (indexed)
            _indices.push_back(first + index - base);
        else {
            const auto& v = vertices[index - base];
            _indices.push_back(u32(_vertices.size()));
            vertex(v.position, {}, multiply(v.color, tint));
        }
    }
}

void WebGL2Renderer2DBackend::set_blend_mode(BlendMode mode) {
    flush();
    _blend = mode;
}
void WebGL2Renderer2DBackend::apply_blend(bool premultiplied) {
    const BlendMode mode = _blend;
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    switch (mode) {
    case BlendMode::Replace:
        glDisable(GL_BLEND);
        break;
    case BlendMode::Additive:
        glBlendFuncSeparate(premultiplied ? GL_ONE : GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        break;
    case BlendMode::Multiply:
        glBlendFuncSeparate(GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
        break;
    case BlendMode::Max:
        glBlendEquation(GL_MAX);
        glBlendFunc(GL_ONE, GL_ONE);
        break;
    case BlendMode::Min:
        glBlendEquation(GL_MIN);
        glBlendFunc(GL_ONE, GL_ONE);
        break;
    default:
        glBlendFuncSeparate(premultiplied ? GL_ONE : GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}
void WebGL2Renderer2DBackend::set_viewport(Rectf r) {
    flush();
    _viewport = r;
}
void WebGL2Renderer2DBackend::reset_viewport() {
    flush();
    _viewport.reset();
}
void WebGL2Renderer2DBackend::push_viewport(Rectf r) {
    _viewports.push_back(_viewport);
    set_viewport(r);
}
void WebGL2Renderer2DBackend::pop_viewport() {
    if (_viewports.empty())
        return;
    flush();
    _viewport = _viewports.back();
    _viewports.pop_back();
}
void WebGL2Renderer2DBackend::push_clip(Rectf r) {
    flush();
    _clips.push_back(_clip);
    _clip = _clip ? intersection(*_clip, r) : r;
}
void WebGL2Renderer2DBackend::pop_clip() {
    if (_clips.empty())
        return;
    flush();
    _clip = _clips.back();
    _clips.pop_back();
}
Vec2i WebGL2Renderer2DBackend::current_target_pixels() const {
    return _target ? _target->dimensions : output_size();
}
void WebGL2Renderer2DBackend::bind_target() {
    glBindFramebuffer(GL_FRAMEBUFFER, _target ? _target->framebuffer : 0);
}
RenderTarget WebGL2Renderer2DBackend::create_render_target(Vec2i size, ScaleMode mode) {
    flush();
    return RenderTarget{_device->create_surface(size, false, mode)};
}
void WebGL2Renderer2DBackend::push_render_target(const RenderTarget& target) {
    auto value = _device->surface(target.texture());
    if (!value || !value->framebuffer || value == _target)
        throw std::invalid_argument("WebGL2 render target is invalid or already active");
    flush();
    _targets.push_back(
        {_target, _logical, _integer, _native_depth, _viewport, _clip, std::move(_viewports), std::move(_clips)});
    _target = std::move(value);
    _logical = {};
    _integer = false;
    _native_depth = 0;
    _viewport.reset();
    _clip.reset();
    _viewports.clear();
    _clips.clear();
}
void WebGL2Renderer2DBackend::pop_render_target() {
    if (_targets.empty())
        return;
    flush();
    auto value = std::move(_targets.back());
    _targets.pop_back();
    _target = std::move(value.target);
    _logical = value.logical;
    _integer = value.integer;
    _native_depth = value.native_depth;
    _viewport = value.viewport;
    _clip = value.clip;
    _viewports = std::move(value.viewports);
    _clips = std::move(value.clips);
}
IRendererBackend3D* WebGL2Renderer2DBackend::renderer_3d() {
#ifdef KIN_EXPERIMENTAL_3D
    return _renderer3d.get();
#else
    return nullptr;
#endif
}
bool WebGL2Renderer2DBackend::read_rgba(Rectf region, std::vector<u8>& pixels, Vec2i& size) {
    flush();
    bind_target();
    const Vec2i output = current_target_pixels();
    const Vec2f a = to_pixel({region.x + (_viewport ? _viewport->x : 0), region.y + (_viewport ? _viewport->y : 0)});
    const auto m = mapping();
    const i32 x = std::clamp(i32(std::floor(a.x)), 0, output.x), y = std::clamp(i32(std::floor(a.y)), 0, output.y);
    const i32 right = std::clamp(i32(std::ceil(a.x + region.w * m.scale)), x, output.x);
    const i32 bottom = std::clamp(i32(std::ceil(a.y + region.h * m.scale)), y, output.y);
    size = {right - x, bottom - y};
    if (size.x <= 0 || size.y <= 0) {
        pixels.clear();
        return false;
    }
    const std::size_t stride = std::size_t(size.x) * 4;
    pixels.resize(stride * size.y);
    glReadPixels(x, output.y - bottom, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    for (i32 row = 0; row < size.y / 2; ++row)
        std::swap_ranges(pixels.begin() + row * stride, pixels.begin() + (row + 1) * stride,
                         pixels.begin() + (size.y - 1 - row) * stride);
    return true;
}

} // namespace kin
