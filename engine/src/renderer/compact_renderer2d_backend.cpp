#include "compact_renderer2d_backend.hpp"

#include <chrono>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace kin {
namespace {
class CpuTexture final : public ITextureBackend {
public:
    CpuTexture(const u8* source, Vec2i size, std::size_t bytes)
        : ITextureBackend(Kind::Compact), dimensions(size), pixels(source, source + bytes) {
        for (std::size_t i = 0; i < bytes; i += 4) {
            if (pixels[i] != 255 || pixels[i + 1] != 255 || pixels[i + 2] != 255) {
                white_mask = false;
                break;
            }
        }
    }
    Vec2i size() const override { return dimensions; }
    Vec2i dimensions;
    std::vector<u8> pixels;
    ScaleMode sampling = ScaleMode::Nearest;
    bool white_mask = true;
};

std::size_t rgba_bytes(Vec2i size) {
    if (size.x <= 0 || size.y <= 0) throw std::invalid_argument("compact: positive dimensions required");
    const auto count = u64(size.x) * u64(size.y);
    if (count > std::vector<u8>{}.max_size() / 4) throw std::length_error("compact: image too large");
    return std::size_t(count) * 4;
}

Rectf intersect(Rectf a, Rectf b) {
    const float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    const float w = std::min(a.x + a.w, b.x + b.w) - x;
    const float h = std::min(a.y + a.h, b.y + b.h) - y;
    return w > 0 && h > 0 ? Rectf{x, y, w, h} : Rectf{};
}

bool finite(Rectf r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h)
        && std::isfinite(r.x + r.w) && std::isfinite(r.y + r.h);
}

struct Bounds { int x0, y0, x1, y1; };
Bounds bounds(Rectf rect) {
    return {int(std::ceil(rect.x - 0.5f)), int(std::ceil(rect.y - 0.5f)),
            int(std::ceil(rect.x + rect.w - 0.5f)), int(std::ceil(rect.y + rect.h - 0.5f))};
}

Color texel(const CpuTexture& texture, int x, int y) {
    x = std::clamp(x, 0, texture.dimensions.x - 1);
    y = std::clamp(y, 0, texture.dimensions.y - 1);
    const u8* p = texture.pixels.data() + (std::size_t(y) * texture.dimensions.x + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

Color sample(const CpuTexture& texture, float x, float y) {
    // Validated source rectangles keep nearest coordinates nonnegative.
    if (texture.sampling == ScaleMode::Nearest) return texel(texture, int(x), int(y));
    x -= 0.5f;
    y -= 0.5f;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    const Color a = texel(texture, ix, iy), b = texel(texture, ix + 1, iy);
    const Color c = texel(texture, ix, iy + 1), d = texel(texture, ix + 1, iy + 1);
    const auto channel = [&](u8 aa, u8 bb, u8 cc, u8 dd) {
        return u8(std::clamp(std::lround((aa * (1 - fx) + bb * fx) * (1 - fy)
                                      + (cc * (1 - fx) + dd * fx) * fy), 0l, 255l));
    };
    return {channel(a.r, b.r, c.r, d.r), channel(a.g, b.g, c.g, d.g),
            channel(a.b, b.b, c.b, d.b), channel(a.a, b.a, c.a, d.a)};
}

} // namespace

CompactRenderer2DBackend::CompactRenderer2DBackend(FramebufferHost& host) : _host(&host) {
    sync_size();
}

RendererBackendCapabilities CompactRenderer2DBackend::capabilities() const {
    RendererBackendCapabilities result;
    result.text = true;
    return result;
}

Vec2i CompactRenderer2DBackend::output_size() const { return _host->pixel_size(); }

void CompactRenderer2DBackend::sync_size() {
    const auto size = output_size();
    if (size.x <= 0 || size.y <= 0) return; // minimized: retain the previous buffer
    if (size != _size) {
        _pixels.assign(rgba_bytes(size), 0);
        _size = size;
    }
}

CompactRenderer2DBackend::Mapping CompactRenderer2DBackend::mapping() const {
    const auto size = output_size();
    if (_logical.x <= 0 || !_native_stack.empty())
        return {{}, 1, {0, 0, float(size.x), float(size.y)}};
    float scale = std::min(float(size.x) / _logical.x, float(size.y) / _logical.y);
    if (_integer && scale >= 1) scale = std::floor(scale);
    scale = std::max(scale, std::numeric_limits<float>::min());
    const Vec2f origin{(size.x - _logical.x * scale) * 0.5f, (size.y - _logical.y * scale) * 0.5f};
    return {origin, scale, {origin.x, origin.y, _logical.x * scale, _logical.y * scale}};
}

Vec2f CompactRenderer2DBackend::map(Vec2f point) const {
    auto m = mapping();
    if (_view.viewport) { point.x += _view.viewport->x; point.y += _view.viewport->y; }
    return {m.origin.x + point.x * m.scale, m.origin.y + point.y * m.scale};
}

Rectf CompactRenderer2DBackend::map(Rectf rect) const {
    const auto p = map(Vec2f{rect.x, rect.y});
    const float scale = mapping().scale;
    return {p.x, p.y, rect.w * scale, rect.h * scale};
}

Rectf CompactRenderer2DBackend::clip() const {
    const auto m = mapping();
    Rectf result = intersect({0, 0, float(_size.x), float(_size.y)}, m.bounds);
    if (_view.viewport) {
        const auto v = *_view.viewport;
        result = intersect(result, {m.origin.x + v.x * m.scale, m.origin.y + v.y * m.scale,
                                    v.w * m.scale, v.h * m.scale});
    }
    if (_view.clip) result = intersect(result, map(*_view.clip));
    return result;
}

void CompactRenderer2DBackend::set_logical_size(Vec2i size) {
    if (size.x < 0 || size.y < 0 || (size.x == 0) != (size.y == 0))
        throw std::invalid_argument("compact: invalid logical dimensions");
    _logical = size;
    _integer = false;
}
void CompactRenderer2DBackend::set_integer_logical_size(Vec2i size) { set_logical_size(size); _integer = true; }
void CompactRenderer2DBackend::push_native_coordinates() { _native_stack.push_back(_view); _view = {}; }
void CompactRenderer2DBackend::pop_native_coordinates() {
    if (!_native_stack.empty()) { _view = _native_stack.back(); _native_stack.pop_back(); }
}

Vec2f CompactRenderer2DBackend::window_to_logical(Vec2f point) const {
    const auto [w, h] = _host->window_size();
    const auto size = output_size();
    const auto m = mapping();
    const Vec2f view = _view.viewport ? Vec2f{_view.viewport->x, _view.viewport->y} : Vec2f{};
    return {(point.x * size.x / std::max(1, w) - m.origin.x) / m.scale - view.x,
            (point.y * size.y / std::max(1, h) - m.origin.y) / m.scale - view.y};
}
Vec2f CompactRenderer2DBackend::logical_to_window(Vec2f point) const {
    const auto [w, h] = _host->window_size();
    const auto size = output_size();
    point = map(point);
    return {point.x * w / std::max(1, size.x), point.y * h / std::max(1, size.y)};
}

void CompactRenderer2DBackend::pixel(int x, int y, Color color) {
    u8* p = _pixels.data() + (std::size_t(y) * _size.x + x) * 4;
    if (_blend == BlendMode::Replace || color.a == 255) {
        p[0] = color.r; p[1] = color.g; p[2] = color.b; p[3] = color.a;
    } else if (color.a) {
        const unsigned inverse = 255 - color.a;
        p[0] = u8((color.r * color.a + p[0] * inverse + 127) / 255);
        p[1] = u8((color.g * color.a + p[1] * inverse + 127) / 255);
        p[2] = u8((color.b * color.a + p[2] * inverse + 127) / 255);
        p[3] = u8(color.a + (p[3] * inverse + 127) / 255);
    }
}

void CompactRenderer2DBackend::clear(Color color) {
    sync_size();
    const auto b = bounds(intersect({0, 0, float(_size.x), float(_size.y)}, mapping().bounds));
    // Copy packed RGBA words without assuming u32 alignment or aliasing rights
    // over vector<u8> storage. Clear ignores viewport/clip and blend state.
    const auto fill = [&](std::size_t first, std::size_t last, Color value) {
        static_assert(sizeof(Color) == 4);
        u32 packed;
        std::memcpy(&packed, &value, 4);
        for (auto i = first * 4; i < last * 4; i += 4)
            std::memcpy(_pixels.data() + i, &packed, 4);
    };
    const auto width = std::size_t(_size.x);
    const auto count = _pixels.size() / 4;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) {
        fill(0, count, colors::black);
    } else if (b.x0 == 0 && b.x1 == _size.x) {
        fill(0, std::size_t(b.y0) * width, colors::black);
        fill(std::size_t(b.y0) * width, std::size_t(b.y1) * width, color);
        fill(std::size_t(b.y1) * width, count, colors::black);
    } else {
        fill(0, std::size_t(b.y0) * width, colors::black);
        for (int y = b.y0; y < b.y1; ++y) {
            const auto row = std::size_t(y) * width;
            fill(row, row + b.x0, colors::black);
            fill(row + b.x0, row + b.x1, color);
            fill(row + b.x1, row + width, colors::black);
        }
        fill(std::size_t(b.y1) * width, count, colors::black);
    }
}

void CompactRenderer2DBackend::fill_rect(Rectf rect, Color color) {
    sync_size();
    if (!finite(rect) || rect.w <= 0 || rect.h <= 0) return;
    const auto b = bounds(intersect(map(rect), clip()));
    for (int y = b.y0; y < b.y1; ++y) for (int x = b.x0; x < b.x1; ++x) pixel(x, y, color);
    ++_stats.rect_fills_submitted; ++_stats.direct_rect_fills;
}

void CompactRenderer2DBackend::draw_line(Vec2f a, Vec2f b, Color color) {
    sync_size();
    if (!finite({a.x, a.y, b.x - a.x, b.y - a.y})) return;
    a = map(a); b = map(b);
    const auto area = bounds(clip());
    if (area.x1 <= area.x0 || area.y1 <= area.y0) return;
    if (!finite({a.x, a.y, b.x - a.x, b.y - a.y})) return;
    const Vec2f delta{b.x - a.x, b.y - a.y};
    float first = 0, last = 1;
    const auto trim = [&](float p, float q) {
        if (p == 0) return q >= 0;
        const float t = q / p;
        if (p < 0) first = std::max(first, t); else last = std::min(last, t);
        return first <= last;
    };
    if (!trim(-delta.x, a.x - area.x0) || !trim(delta.x, area.x1 - 1 - a.x)
        || !trim(-delta.y, a.y - area.y0) || !trim(delta.y, area.y1 - 1 - a.y)) return;
    b = {a.x + delta.x * last, a.y + delta.y * last};
    a = {a.x + delta.x * first, a.y + delta.y * first};
    const int steps = int(std::ceil(std::max(std::abs(b.x - a.x), std::abs(b.y - a.y))));
    for (int i = 0; i <= steps; ++i) {
        const float t = steps ? float(i) / steps : 0;
        const int x = std::clamp(int(std::lround(a.x + (b.x - a.x) * t)), area.x0, area.x1 - 1);
        const int y = std::clamp(int(std::lround(a.y + (b.y - a.y) * t)), area.y0, area.y1 - 1);
        pixel(x, y, color);
    }
    ++_stats.direct_lines;
}
void CompactRenderer2DBackend::draw_rect(Rectf r, Color color) {
    if (!finite(r) || r.w <= 0 || r.h <= 0) return;
    draw_line({r.x, r.y}, {r.x + r.w - 1, r.y}, color);
    draw_line({r.x + r.w - 1, r.y}, {r.x + r.w - 1, r.y + r.h - 1}, color);
    draw_line({r.x + r.w - 1, r.y + r.h - 1}, {r.x, r.y + r.h - 1}, color);
    draw_line({r.x, r.y + r.h - 1}, {r.x, r.y}, color);
    ++_stats.direct_rect_outlines;
}

Texture CompactRenderer2DBackend::create_texture_from_rgba(const u8* pixels, Vec2i size) {
    const auto bytes = rgba_bytes(size);
    if (!pixels) throw std::invalid_argument("compact: null texture pixels");
    return Texture{std::make_shared<CpuTexture>(pixels, size, bytes)};
}
void CompactRenderer2DBackend::set_scale_mode(const Texture& texture, ScaleMode mode) {
    if (texture.backend() && texture.backend()->kind() == ITextureBackend::Kind::Compact)
        static_cast<CpuTexture*>(texture.backend().get())->sampling = mode;
}
void CompactRenderer2DBackend::draw_texture(const Texture& texture, Rectf dest) {
    const auto size = texture.size();
    draw_texture(texture, {0, 0, float(size.x), float(size.y)}, dest);
}
void CompactRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest) {
    draw_texture(texture, source, dest, colors::white);
}
void CompactRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) {
    sync_size();
    if (!texture) return;
    if (texture.backend()->kind() != ITextureBackend::Kind::Compact)
        throw std::invalid_argument("compact: foreign texture");
    if (!finite(source) || !finite(dest) || dest.w <= 0 || dest.h <= 0 || source.w == 0 || source.h == 0) return;
    const auto& cpu = *static_cast<const CpuTexture*>(texture.backend().get());
    if (std::min(source.x, source.x + source.w) < 0 || std::max(source.x, source.x + source.w) > cpu.dimensions.x
        || std::min(source.y, source.y + source.h) < 0 || std::max(source.y, source.y + source.h) > cpu.dimensions.y)
        throw std::invalid_argument("compact: texture source outside image");
    dest = map(dest);
    const auto b = bounds(intersect(dest, clip()));
    const float step_x = source.w / dest.w, step_y = source.h / dest.h;
    // Coverage sprites (including the procedural hexes) need only an alpha
    // fetch and the tint. Keep this common small-game path out of the generic
    // four-channel sampler, without changing how colored textures are drawn.
    if (cpu.white_mask && cpu.sampling == ScaleMode::Nearest) {
        for (int y = b.y0; y < b.y1; ++y) {
            const int ty = std::clamp(int(source.y + (y + 0.5f - dest.y) * step_y), 0, cpu.dimensions.y - 1);
            for (int x = b.x0; x < b.x1; ++x) {
                const int tx = std::clamp(int(source.x + (x + 0.5f - dest.x) * step_x), 0, cpu.dimensions.x - 1);
                const u8 alpha = cpu.pixels[(std::size_t(ty) * cpu.dimensions.x + tx) * 4 + 3];
                if (!alpha && _blend == BlendMode::Alpha) continue;
                Color color = tint;
                color.a = tint.a == 255 ? alpha : u8((unsigned(alpha) * tint.a + 127) / 255);
                pixel(x, y, color);
            }
        }
        ++_stats.texture_draws_submitted;
        return;
    }
    for (int y = b.y0; y < b.y1; ++y) for (int x = b.x0; x < b.x1; ++x) {
        Color color = sample(cpu, source.x + (x + 0.5f - dest.x) * step_x,
                                 source.y + (y + 0.5f - dest.y) * step_y);
        color.r = u8((unsigned(color.r) * tint.r + 127) / 255);
        color.g = u8((unsigned(color.g) * tint.g + 127) / 255);
        color.b = u8((unsigned(color.b) * tint.b + 127) / 255);
        color.a = u8((unsigned(color.a) * tint.a + 127) / 255);
        pixel(x, y, color);
    }
    ++_stats.texture_draws_submitted;
}
void CompactRenderer2DBackend::draw_texture(const Texture& t, Rectf s, Rectf d, Color c, f32 rotation, Vec2f) {
    if (rotation != 0) throw std::invalid_argument("compact: texture rotation unsupported");
    draw_texture(t, s, d, c);
}
void CompactRenderer2DBackend::set_blend_mode(BlendMode mode) {
    if (mode != BlendMode::Alpha && mode != BlendMode::Replace)
        throw std::invalid_argument("compact: only Alpha and Replace blending supported");
    _blend = mode;
}
void CompactRenderer2DBackend::set_transform(const Affine2& transform) {
    if (!transform.is_identity()) throw std::invalid_argument("compact: transforms unsupported");
}
void CompactRenderer2DBackend::set_viewport(Rectf rect) {
    if (!finite(rect)) throw std::invalid_argument("compact: invalid viewport");
    _view.viewport = rect;
}
void CompactRenderer2DBackend::push_viewport(Rectf rect) { _view_stack.push_back(_view); set_viewport(rect); }
void CompactRenderer2DBackend::pop_viewport() {
    if (!_view_stack.empty()) { _view = _view_stack.back(); _view_stack.pop_back(); } else reset_viewport();
}
void CompactRenderer2DBackend::push_clip(Rectf rect) {
    if (!finite(rect)) throw std::invalid_argument("compact: invalid clip");
    _clip_stack.push_back(_view);
    _view.clip = _view.clip ? intersect(*_view.clip, rect) : rect;
}
void CompactRenderer2DBackend::pop_clip() {
    if (!_clip_stack.empty()) { _view = _clip_stack.back(); _clip_stack.pop_back(); } else _view.clip.reset();
}

Vec2i CompactRenderer2DBackend::region_pixel_size(Rectf region) const {
    if (!finite(region) || region.w <= 0 || region.h <= 0) return {};
    const auto size = output_size();
    const auto b = bounds(intersect(map(region), {0, 0, float(size.x), float(size.y)}));
    return {b.x1 - b.x0, b.y1 - b.y0};
}

bool CompactRenderer2DBackend::read_rgba(Rectf region, std::vector<u8>& out, Vec2i& size) {
    sync_size();
    out.clear(); size = {};
    if (!finite(region) || region.w <= 0 || region.h <= 0) return false;
    const auto b = bounds(intersect(map(region), {0, 0, float(_size.x), float(_size.y)}));
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return false;
    size = {b.x1 - b.x0, b.y1 - b.y0};
    out.resize(rgba_bytes(size));
    for (int y = 0; y < size.y; ++y)
        std::memcpy(out.data() + std::size_t(y) * size.x * 4,
                    _pixels.data() + (std::size_t(y + b.y0) * _size.x + b.x0) * 4, std::size_t(size.x) * 4);
    return true;
}

void CompactRenderer2DBackend::present() {
    const auto start = std::chrono::steady_clock::now();
    sync_size();
    _host->present_rgba(_pixels, _size);
    _stats.last_present_backend_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}
} // namespace kin
