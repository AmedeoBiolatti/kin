#include "sdl_renderer2d_backend.hpp"

#include <kin/platform/log.hpp>

#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <cstring>

namespace kin {
namespace {

// Tag-checked downcast — replaces dynamic_cast on the textured-draw hot path. The
// kind tag (set in each backend's texture ctor) guarantees the concrete type, so a
// static_cast is safe and the RTTI lookup is avoided. Returns null for foreign textures.
const SdlTextureBackend* as_sdl(const ITextureBackend* backend) {
    return (backend && backend->kind() == ITextureBackend::Kind::Sdl)
               ? static_cast<const SdlTextureBackend*>(backend)
               : nullptr;
}

SDL_FColor to_sdl_color(Color color) {
    constexpr f32 inv = 1.0f / 255.0f;
    return {
        static_cast<f32>(color.r) * inv,
        static_cast<f32>(color.g) * inv,
        static_cast<f32>(color.b) * inv,
        static_cast<f32>(color.a) * inv,
    };
}

SDL_FRect to_sdl_frect(Rectf rect) {
    return {rect.x, rect.y, rect.w, rect.h};
}

SDL_Rect to_sdl_rect(Rectf rect) {
    return {
        static_cast<int>(std::round(rect.x)),
        static_cast<int>(std::round(rect.y)),
        std::max(0, static_cast<int>(std::round(rect.w))),
        std::max(0, static_cast<int>(std::round(rect.h))),
    };
}

SDL_Rect intersect_rects(SDL_Rect a, SDL_Rect b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.w, b.x + b.w);
    const int y2 = std::min(a.y + a.h, b.y + b.h);
    return {
        x1,
        y1,
        std::max(0, x2 - x1),
        std::max(0, y2 - y1),
    };
}

f64 ms_since(std::chrono::steady_clock::time_point start) {
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<f64, std::milli>(end - start).count();
}

bool drawable_rect(Rectf rect, Color color) {
    return color.a > 0 && rect.w > 0.0f && rect.h > 0.0f;
}

f32 rounded_radius(Rectf rect, f32 radius) {
    return std::floor(std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f));
}

Color alpha_scaled(Color color, f32 alpha) {
    color.a = static_cast<u8>(std::clamp(std::round(static_cast<f32>(color.a) * alpha), 0.0f, 255.0f));
    return color;
}

Rectf inset_rect(Rectf rect, f32 inset) {
    return {
        rect.x + inset,
        rect.y + inset,
        std::max(0.0f, rect.w - inset * 2.0f),
        std::max(0.0f, rect.h - inset * 2.0f),
    };
}

std::vector<Vec2f> rounded_rect_loop(Rectf rect, f32 radius) {
    std::vector<Vec2f> points;
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return points;
    }

    const f32 r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (r <= 0.0f) {
        points.push_back({rect.x, rect.y});
        points.push_back({rect.x + rect.w, rect.y});
        points.push_back({rect.x + rect.w, rect.y + rect.h});
        points.push_back({rect.x, rect.y + rect.h});
        return points;
    }

    const i32 segments = std::max(8, static_cast<i32>(std::ceil(r * 2.0f)));
    constexpr f32 pi = 3.14159265358979323846f;
    constexpr f32 half_pi = pi * 0.5f;
    const std::array<Vec2f, 4> centers{{
        {rect.x + rect.w - r, rect.y + r},
        {rect.x + rect.w - r, rect.y + rect.h - r},
        {rect.x + r, rect.y + rect.h - r},
        {rect.x + r, rect.y + r},
    }};
    const std::array<f32, 4> starts{{-half_pi, 0.0f, half_pi, pi}};
    points.reserve(static_cast<std::size_t>((segments + 1) * 4));
    for (std::size_t corner = 0; corner < centers.size(); ++corner) {
        for (i32 i = 0; i <= segments; ++i) {
            const f32 t = starts[corner] + (static_cast<f32>(i) / static_cast<f32>(segments)) * half_pi;
            points.push_back({
                centers[corner].x + std::cos(t) * r,
                centers[corner].y + std::sin(t) * r,
            });
        }
    }
    return points;
}

void render_filled_loop(SDL_Renderer* renderer, const std::vector<Vec2f>& loop, Color color) {
    if (!renderer || loop.size() < 3 || color.a == 0) {
        return;
    }

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(loop.size() + 1);
    indices.reserve((loop.size() - 2) * 3);

    Vec2f center{};
    for (Vec2f point : loop) {
        center.x += point.x;
        center.y += point.y;
    }
    center.x /= static_cast<f32>(loop.size());
    center.y /= static_cast<f32>(loop.size());

    const SDL_FColor sdl_color = to_sdl_color(color);
    vertices.push_back({.position = {center.x, center.y}, .color = sdl_color, .tex_coord = {0.0f, 0.0f}});
    for (Vec2f point : loop) {
        vertices.push_back({.position = {point.x, point.y}, .color = sdl_color, .tex_coord = {0.0f, 0.0f}});
    }

    for (std::size_t i = 0; i < loop.size(); ++i) {
        indices.push_back(0);
        indices.push_back(static_cast<int>(i + 1));
        indices.push_back(static_cast<int>(((i + 1) % loop.size()) + 1));
    }
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(), static_cast<int>(indices.size()));
}

void render_loop_ring(SDL_Renderer* renderer, const std::vector<Vec2f>& outer, Color outer_color, const std::vector<Vec2f>& inner, Color inner_color) {
    if (!renderer || outer.size() < 3 || outer.size() != inner.size()) {
        return;
    }

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(outer.size() * 2);
    indices.reserve(outer.size() * 6);
    const SDL_FColor outer_sdl = to_sdl_color(outer_color);
    const SDL_FColor inner_sdl = to_sdl_color(inner_color);

    for (std::size_t i = 0; i < outer.size(); ++i) {
        vertices.push_back({.position = {outer[i].x, outer[i].y}, .color = outer_sdl, .tex_coord = {0.0f, 0.0f}});
        vertices.push_back({.position = {inner[i].x, inner[i].y}, .color = inner_sdl, .tex_coord = {0.0f, 0.0f}});
    }

    for (std::size_t i = 0; i < outer.size(); ++i) {
        const int outer_a = static_cast<int>(i * 2);
        const int inner_a = outer_a + 1;
        const int outer_b = static_cast<int>(((i + 1) % outer.size()) * 2);
        const int inner_b = outer_b + 1;
        indices.push_back(outer_a);
        indices.push_back(inner_a);
        indices.push_back(inner_b);
        indices.push_back(outer_a);
        indices.push_back(inner_b);
        indices.push_back(outer_b);
    }
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), indices.data(), static_cast<int>(indices.size()));
}

// Under BlendMode::Alpha a texture keeps its own blend (straight or, for a
// render target, premultiplied); the others replace it for the draw.
SDL_BlendMode to_sdl_blend(BlendMode mode) {
    switch (mode) {
    case BlendMode::Additive: return SDL_BLENDMODE_ADD;
    case BlendMode::Multiply: return SDL_BLENDMODE_MOD;
    case BlendMode::Replace: return SDL_BLENDMODE_NONE;
    case BlendMode::Max:
    case BlendMode::Min: {
        const SDL_BlendOperation op = mode == BlendMode::Max ? SDL_BLENDOPERATION_MAXIMUM : SDL_BLENDOPERATION_MINIMUM;
        return SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE, op, SDL_BLENDFACTOR_ONE,
                                          SDL_BLENDFACTOR_ONE, op);
    }
    case BlendMode::Alpha: break;
    }
    return SDL_BLENDMODE_BLEND;
}

// Sets `texture`'s blend for one draw when a mode other than Alpha is in
// force, and puts the texture's own back afterwards.
class TextureBlendScope {
public:
    TextureBlendScope(SDL_Texture* texture, BlendMode mode) {
        if (texture && mode != BlendMode::Alpha && SDL_GetTextureBlendMode(texture, &_previous)) {
            _texture = texture;
            SDL_SetTextureBlendMode(texture, to_sdl_blend(mode));
        }
    }
    ~TextureBlendScope() {
        if (_texture) {
            SDL_SetTextureBlendMode(_texture, _previous);
        }
    }
    TextureBlendScope(const TextureBlendScope&) = delete;
    TextureBlendScope& operator=(const TextureBlendScope&) = delete;

private:
    SDL_Texture* _texture = nullptr;
    SDL_BlendMode _previous = SDL_BLENDMODE_BLEND;
};

// SDL's software renderer blends each untextured triangle through a scratch
// surface. Under Alpha blending an opaque color gives the same pixels unblended,
// which it fills directly, so untextured draws whose colors are all opaque turn
// blending off for the call.
class OpaqueDrawScope {
public:
    OpaqueDrawScope(SDL_Renderer* renderer, BlendMode mode, bool opaque) {
        if (mode == BlendMode::Alpha && opaque) {
            _renderer = renderer;
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
    }
    ~OpaqueDrawScope() {
        if (_renderer) {
            SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);
        }
    }
    OpaqueDrawScope(const OpaqueDrawScope&) = delete;
    OpaqueDrawScope& operator=(const OpaqueDrawScope&) = delete;

private:
    SDL_Renderer* _renderer = nullptr;
};

} // namespace

SdlRenderer2DBackend::SdlRenderer2DBackend(Window& window, bool vsync) {
    if (!window.native_handle()) {
        throw std::runtime_error("SdlRenderer2DBackend requires a valid Window");
    }

    // Prefer SDL's "gpu" render driver: it is the only one that exposes the
    // custom fragment-shader hook (SDL_CreateGPURenderState, SDL 3.4) and a
    // backing SDL_GPUDevice for future explicit-GPU work. Fall back to the
    // best-available driver if the gpu driver is unavailable (e.g. software).
    auto* sdl_window = static_cast<SDL_Window*>(window.native_handle());
    _handle = SDL_CreateRenderer(sdl_window, "gpu");
    if (!_handle) {
        _handle = SDL_CreateRenderer(sdl_window, nullptr);
    }
    if (!_handle) {
        const std::string error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("render",
                        "renderer backend creation failed",
                        (LogFields{{.name = "error", .value = error}}));
        throw std::runtime_error(error);
    }

    // The GPU device is present only on the "gpu" driver; it gates materials_2d.
    _gpu_device = static_cast<SDL_GPUDevice*>(SDL_GetPointerProperty(
        SDL_GetRendererProperties(_handle), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
    if (_gpu_device) {
        const SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(_gpu_device);
        KIN_LOG_INFO_F("render",
                       "gpu shader formats",
                       (LogFields{
                           {.name = "spirv", .value = (fmts & SDL_GPU_SHADERFORMAT_SPIRV) ? "1" : "0"},
                           {.name = "dxil", .value = (fmts & SDL_GPU_SHADERFORMAT_DXIL) ? "1" : "0"},
                           {.name = "dxbc", .value = (fmts & SDL_GPU_SHADERFORMAT_DXBC) ? "1" : "0"},
                           {.name = "msl", .value = (fmts & SDL_GPU_SHADERFORMAT_MSL) ? "1" : "0"},
                       }));
    }

    if (vsync) {
        SDL_SetRenderVSync(_handle, 1);
    }
    // Max and Min are custom blend modes, which only some drivers take (not
    // the software one): ask once.
    _min_max_blend = SDL_SetRenderDrawBlendMode(_handle, to_sdl_blend(BlendMode::Max)) &&
                     SDL_SetRenderDrawBlendMode(_handle, to_sdl_blend(BlendMode::Min));
    SDL_SetRenderDrawBlendMode(_handle, SDL_BLENDMODE_BLEND);
    KIN_LOG_INFO_F("render",
                   "renderer backend created",
                   (LogFields{
                       {.name = "backend", .value = std::string{name()}},
                       {.name = "vsync", .value = vsync ? "true" : "false"},
                       {.name = "gpu_device", .value = _gpu_device ? "true" : "false"},
                   }));

    if (const char* diagnostics = std::getenv("KIN_RENDER_DIAGNOSTICS");
        diagnostics && diagnostics[0] == '1') {
        int actual_vsync = 0;
        const bool got_vsync = SDL_GetRenderVSync(_handle, &actual_vsync);
        const char* skip_present = std::getenv("KIN_SKIP_PRESENT");
        const char* log_frame_stats = std::getenv("KIN_LOG_FRAME_STATS");
        KIN_LOG_INFO_F("render",
                       "renderer diagnostics",
                       (LogFields{
                           {.name = "renderer", .value = std::string{name()}},
                           {.name = "requested_vsync", .value = vsync ? "1" : "0"},
                           {.name = "actual_vsync", .value = std::to_string(got_vsync ? actual_vsync : -999)},
                           {.name = "skip_present", .value = skip_present ? skip_present : ""},
                           {.name = "log_frame_stats", .value = log_frame_stats ? log_frame_stats : ""},
                       }));
    }
}

SdlRenderer2DBackend::~SdlRenderer2DBackend() {
    for (ShaderEntry& e : _shaders) {
        if (e.state) {
            SDL_DestroyGPURenderState(e.state);
        }
        if (e.shader && _gpu_device) {
            SDL_ReleaseGPUShader(_gpu_device, e.shader);
        }
    }
    _shaders.clear();
    _white_texture = {}; // free the SDL texture while the renderer is still alive
    if (_mask_scratch) {
        SDL_DestroyTexture(_mask_scratch);
    }
    *_alive = false;
    if (_handle) {
        SDL_DestroyRenderer(_handle);
    }
}

std::string_view SdlRenderer2DBackend::name() const {
    const char* renderer_name = _handle ? SDL_GetRendererName(_handle) : nullptr;
    return renderer_name ? std::string_view{renderer_name} : std::string_view{"SDL Renderer"};
}

RendererBackendCapabilities SdlRenderer2DBackend::capabilities() const {
    return {
        .immediate_2d = true,
        .queued_2d = true,
        .render_targets = true,
        .blend_modes = true,
        .min_max_blend = _min_max_blend,
        .transforms = true,
        .shapes = true,
        .masks = true,
        .materials_2d = _gpu_device != nullptr,
        .gradients = true,
        .text = false,
        .rendering_3d = false,
    };
}

RendererBackendStats SdlRenderer2DBackend::stats() const {
    return _stats;
}

void SdlRenderer2DBackend::reset_stats() {
    _stats = {};
}

void SdlRenderer2DBackend::set_texture_batching_enabled(bool enabled) {
    if (_texture_batching_enabled == enabled) {
        return;
    }
    flush_batch();
    _texture_batching_enabled = enabled;
}

bool SdlRenderer2DBackend::texture_batching_enabled() const {
    return _texture_batching_enabled;
}

void SdlRenderer2DBackend::clear(Color color) {
    flush_batch();
    apply_logical_presentation();
    const SDL_FColor sdl_color = to_sdl_color(color);
    SDL_SetRenderDrawColorFloat(_handle, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
    SDL_RenderClear(_handle);
}

void SdlRenderer2DBackend::present() {
    const auto flush_start = std::chrono::steady_clock::now();
    flush_batch();
    _stats.last_present_flush_ms = ms_since(flush_start);

    if (const char* skip_present = std::getenv("KIN_SKIP_PRESENT");
        skip_present && skip_present[0] == '1') {
        _stats.last_present_backend_ms = 0.0;
        apply_logical_presentation();
        return;
    }

    const auto present_start = std::chrono::steady_clock::now();
    SDL_RenderPresent(_handle);
    apply_logical_presentation();
    _stats.last_present_backend_ms = ms_since(present_start);
}

bool SdlRenderer2DBackend::save_png(const char* path) {
    flush_batch();
    SDL_Surface* surface = SDL_RenderReadPixels(_handle, nullptr);
    if (!surface) {
        return false;
    }
    const bool ok = IMG_SavePNG(surface, path);
    SDL_DestroySurface(surface);
    return ok;
}

bool SdlRenderer2DBackend::read_rgba(Rectf logical_region, std::vector<u8>& out, Vec2i& out_size) {
    flush_batch();
    out.clear();
    out_size = {};
    if (logical_region.w <= 0.0f || logical_region.h <= 0.0f) {
        return false;
    }

    const Vec2f a = logical_to_window({logical_region.x, logical_region.y});
    const Vec2f b = logical_to_window({logical_region.x + logical_region.w, logical_region.y + logical_region.h});
    const int x1 = static_cast<int>(std::floor(std::min(a.x, b.x)));
    const int y1 = static_cast<int>(std::floor(std::min(a.y, b.y)));
    const int x2 = static_cast<int>(std::ceil(std::max(a.x, b.x)));
    const int y2 = static_cast<int>(std::ceil(std::max(a.y, b.y)));
    const Vec2i output = output_size();
    SDL_Rect rect{
        std::clamp(x1, 0, output.x),
        std::clamp(y1, 0, output.y),
        0,
        0,
    };
    rect.w = std::clamp(x2, 0, output.x) - rect.x;
    rect.h = std::clamp(y2, 0, output.y) - rect.y;
    if (rect.w <= 0 || rect.h <= 0) {
        return false;
    }

    SDL_Surface* raw = SDL_RenderReadPixels(_handle, &rect);
    if (!raw) {
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (!rgba) {
        return false;
    }

    out_size = {rgba->w, rgba->h};
    out.resize(static_cast<std::size_t>(out_size.x) * static_cast<std::size_t>(out_size.y) * 4u);
    const auto* source = static_cast<const u8*>(rgba->pixels);
    const std::size_t row_bytes = static_cast<std::size_t>(out_size.x) * 4u;
    for (int y = 0; y < out_size.y; ++y) {
        std::memcpy(out.data() + row_bytes * static_cast<std::size_t>(y),
                    source + static_cast<std::size_t>(rgba->pitch) * static_cast<std::size_t>(y),
                    row_bytes);
    }
    SDL_DestroySurface(rgba);
    return true;
}

void SdlRenderer2DBackend::set_logical_size(Vec2i size) {
    flush_batch();
    _logical = {.width = size.x, .height = size.y, .mode = SDL_LOGICAL_PRESENTATION_LETTERBOX};
    apply_logical_presentation();
}

void SdlRenderer2DBackend::set_integer_logical_size(Vec2i size) {
    flush_batch();
    _logical = {.width = size.x, .height = size.y, .mode = SDL_LOGICAL_PRESENTATION_INTEGER_SCALE};
    apply_logical_presentation();
}

void SdlRenderer2DBackend::push_native_coordinates() {
    flush_batch();
    _saved_transforms.push_back(_transform);
    set_transform({});
    _logical_presentation_stack.push_back(_logical);
    _logical = {};
    apply_logical_presentation();
}

void SdlRenderer2DBackend::pop_native_coordinates() {
    flush_batch();
    if (!_saved_transforms.empty()) {
        set_transform(_saved_transforms.back());
        _saved_transforms.pop_back();
    }
    _logical = {};
    if (!_logical_presentation_stack.empty()) {
        _logical = _logical_presentation_stack.back();
        _logical_presentation_stack.pop_back();
    }
    apply_logical_presentation();
}

// A logical size equal to the window's output maps every coordinate to itself,
// but with logical presentation on SDL draws every line as triangles, which the
// software renderer blends one by one through a scratch surface, instead of its
// direct line path: about twice the frame time in line-heavy scenes. So SDL gets
// logical presentation only while the sizes differ; clear() and present()
// re-check, which picks up window resizes.
void SdlRenderer2DBackend::apply_logical_presentation() {
    // SDL keeps logical presentation per view and sets the current one: only
    // touch it while the window, not a render target, is bound.
    if (SDL_GetRenderTarget(_handle) != nullptr) {
        return;
    }
    LogicalPresentationState wanted = _logical;
    if (wanted.mode != SDL_LOGICAL_PRESENTATION_DISABLED) {
        Vec2i window{};
        SDL_GetRenderOutputSize(_handle, &window.x, &window.y);
        if (window == Vec2i{wanted.width, wanted.height}) {
            wanted = {};
        }
    }
    if (wanted.width == _applied_logical.width && wanted.height == _applied_logical.height &&
        wanted.mode == _applied_logical.mode) {
        return;
    }
    flush_batch();
    SDL_SetRenderLogicalPresentation(_handle, wanted.width, wanted.height, wanted.mode);
    _applied_logical = wanted;
}

Vec2i SdlRenderer2DBackend::output_size() const {
    Vec2i size{};
    SDL_GetCurrentRenderOutputSize(_handle, &size.x, &size.y);
    return size;
}

Vec2f SdlRenderer2DBackend::window_to_logical(Vec2f window_px) const {
    Vec2f logical{};
    SDL_RenderCoordinatesFromWindow(_handle, window_px.x, window_px.y, &logical.x, &logical.y);
    return logical;
}

Vec2f SdlRenderer2DBackend::logical_to_window(Vec2f logical) const {
    Vec2f window{};
    SDL_RenderCoordinatesToWindow(_handle, logical.x, logical.y, &window.x, &window.y);
    return window;
}

void SdlRenderer2DBackend::fill_rect(Rectf rect, Color color) {
    ++_stats.rect_fills_submitted;

    if (_texture_batching_enabled || _transformed) {
        // Solid quads batch into a single SDL_RenderGeometry call with a null
        // texture. They share the same _batch as textured quads, so every
        // existing flush_batch() point (clear, present, draw_rect, draw_line,
        // viewport/clip changes, a textured draw, ...) already serializes them
        // in submission order.
        begin_batch(BatchKind::Color, nullptr);
        append_quad(rect, {}, {0, 0}, color);
        return;
    }

    flush_batch();
    ++_stats.direct_rect_fills;
    const SDL_FColor sdl_color = to_sdl_color(color);
    const SDL_FRect sdl_rect = to_sdl_frect(rect);
    SDL_SetRenderDrawColorFloat(_handle, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
    const OpaqueDrawScope opaque(_handle, _blend, color.a == 255);
    SDL_RenderFillRect(_handle, &sdl_rect);
}

Texture SdlRenderer2DBackend::create_texture_from_rgba(const u8* pixels, Vec2i size) {
    if (!pixels || size.x <= 0 || size.y <= 0) {
        throw std::runtime_error("create_texture_from_rgba requires pixels and a positive size");
    }

    SDL_Texture* texture = SDL_CreateTexture(
        _handle,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC,
        size.x,
        size.y
    );
    if (!texture) {
        const std::string error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("render",
                        "texture creation failed",
                        (LogFields{
                            {.name = "width", .value = std::to_string(size.x)},
                            {.name = "height", .value = std::to_string(size.y)},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    const int pitch = size.x * 4;
    if (!SDL_UpdateTexture(texture, nullptr, pixels, pitch)) {
        SDL_DestroyTexture(texture);
        const std::string error = std::string("SDL_UpdateTexture failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("render",
                        "texture upload failed",
                        (LogFields{
                            {.name = "width", .value = std::to_string(size.x)},
                            {.name = "height", .value = std::to_string(size.y)},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    return Texture{std::make_shared<SdlTextureBackend>(texture, size, _alive)};
}

void SdlRenderer2DBackend::draw_texture(const Texture& texture, Rectf dest) {
    const Vec2i size = texture.size();
    draw_texture(texture, {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)}, dest);
}

void SdlRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest) {
    draw_texture(texture, source, dest, colors::white);
}

void SdlRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) {
    draw_texture(texture, source, dest, tint, 0.0f, {0.5f, 0.5f});
}

void SdlRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) {
    if (!texture) {
        return;
    }
    ++_stats.texture_draws_submitted;

    const auto* sdl_texture = as_sdl(texture.backend().get());
    if (!sdl_texture || !sdl_texture->handle()) {
        throw std::runtime_error("Texture was not created by SdlRenderer2DBackend");
    }

    if (_texture_batching_enabled || _transformed) {
        begin_batch(BatchKind::Texture, sdl_texture->handle(), texture);
        append_quad(dest, source, texture.size(), tint, rotation, pivot);
    } else {
        flush_batch();
        const SDL_FRect src_rect = to_sdl_frect(source);
        const SDL_FRect dst_rect = to_sdl_frect(dest);
        const SDL_FPoint center{dest.w * pivot.x, dest.h * pivot.y};
        SDL_SetTextureColorMod(sdl_texture->handle(), tint.r, tint.g, tint.b);
        SDL_SetTextureAlphaMod(sdl_texture->handle(), tint.a);
        const TextureBlendScope blend(sdl_texture->handle(), _blend);
        SDL_RenderTextureRotated(_handle, sdl_texture->handle(), &src_rect, &dst_rect, rotation, &center, SDL_FLIP_NONE);
        SDL_SetTextureColorMod(sdl_texture->handle(), 255, 255, 255);
        SDL_SetTextureAlphaMod(sdl_texture->handle(), 255);
    }
}

void SdlRenderer2DBackend::draw_shape_mesh(std::span<const ShapeVertex> vertices, std::span<const u32> indices,
                                           u32 index_base, Color tint) {
    if (indices.empty() || tint.a == 0) {
        return;
    }
    flush_batch();
    // No shader to measure pixels: `edge` (mesh units) in pixels from the
    // transform's scale, and the logical presentation's when drawing to the window.
    f32 pixels_per_unit = std::sqrt(std::abs(_transform.determinant()));
    int out_w = 0, out_h = 0;
    if (_render_target_stack.empty() && _applied_logical.mode != SDL_LOGICAL_PRESENTATION_DISABLED &&
        _applied_logical.width > 0 && _applied_logical.height > 0 && SDL_GetCurrentRenderOutputSize(_handle, &out_w, &out_h)) {
        pixels_per_unit *= std::min(static_cast<f32>(out_w) / static_cast<f32>(_applied_logical.width),
                                    static_cast<f32>(out_h) / static_cast<f32>(_applied_logical.height));
    }
    _shape_vertices.clear();
    _shape_vertices.reserve(vertices.size());
    // A soft edge wider than a pixel is pulled in to one, along `outward`.
    const f32 rim = -1.0f / std::max(pixels_per_unit, 1e-6f);
    for (const ShapeVertex& v : vertices) {
        Vec2f p = v.position;
        f32 edge = v.edge;
        if (edge < rim) {
            p = {p.x + v.outward.x * (edge - rim), p.y + v.outward.y * (edge - rim)};
            edge = rim;
        }
        p = _transformed ? _transform.apply(p) : p;
        const f32 cover = std::clamp(1.0f + edge * pixels_per_unit, 0.0f, 1.0f);
        _shape_vertices.push_back(SDL_Vertex{
            .position = {p.x, p.y},
            .color = {static_cast<f32>(v.color.r) * static_cast<f32>(tint.r) / 65025.0f,
                      static_cast<f32>(v.color.g) * static_cast<f32>(tint.g) / 65025.0f,
                      static_cast<f32>(v.color.b) * static_cast<f32>(tint.b) / 65025.0f,
                      static_cast<f32>(v.color.a) * static_cast<f32>(tint.a) / 65025.0f * cover},
            .tex_coord = {0.0f, 0.0f}});
    }
    _shape_indices.resize(indices.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        _shape_indices[i] = static_cast<int>(indices[i] - index_base);
    }
    ++_stats.direct_rect_fills;
    SDL_RenderGeometry(_handle, nullptr, _shape_vertices.data(), static_cast<int>(_shape_vertices.size()),
                       _shape_indices.data(), static_cast<int>(_shape_indices.size()));
}

void SdlRenderer2DBackend::draw_rect(Rectf rect, Color color) {
    if (_transformed) {
        // Four one-unit strips inside the rectangle, as the GPU backend draws it.
        if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) {
            return;
        }
        fill_rect({rect.x, rect.y, rect.w, 1.0f}, color);
        fill_rect({rect.x, rect.y + rect.h - 1.0f, rect.w, 1.0f}, color);
        fill_rect({rect.x, rect.y, 1.0f, rect.h}, color);
        fill_rect({rect.x + rect.w - 1.0f, rect.y, 1.0f, rect.h}, color);
        return;
    }
    flush_batch();
    ++_stats.direct_rect_outlines;
    const SDL_FColor sdl_color = to_sdl_color(color);
    const SDL_FRect sdl_rect = to_sdl_frect(rect);
    SDL_SetRenderDrawColorFloat(_handle, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
    const OpaqueDrawScope opaque(_handle, _blend, color.a == 255);
    SDL_RenderRect(_handle, &sdl_rect);
}

void SdlRenderer2DBackend::fill_rounded_rect(Rectf rect, f32 radius, Color color) {
    if (!drawable_rect(rect, color)) {
        return;
    }
    const f32 r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (r <= 0.0f) {
        fill_rect(rect, color);
        return;
    }

    flush_batch();
    ++_stats.direct_rect_fills;
    constexpr f32 fringe = 1.0f;
    const Rectf solid_rect = inset_rect(rect, fringe);
    if (solid_rect.w <= 0.0f || solid_rect.h <= 0.0f) {
        const OpaqueDrawScope opaque(_handle, _blend, color.a == 255);
        render_filled_loop(_handle, mapped(rounded_rect_loop(rect, r)), color);
        return;
    }
    const std::vector<Vec2f> solid = mapped(rounded_rect_loop(solid_rect, std::max(0.0f, r - fringe)));
    {
        const OpaqueDrawScope opaque(_handle, _blend, color.a == 255);
        render_filled_loop(_handle, solid, color);
    }
    render_loop_ring(_handle, mapped(rounded_rect_loop(rect, r)), alpha_scaled(color, 0.0f), solid, color);
}

void SdlRenderer2DBackend::draw_rounded_rect(Rectf rect, f32 radius, Color color, f32 width) {
    if (!drawable_rect(rect, color) || width <= 0.0f) {
        return;
    }
    const f32 base_radius = rounded_radius(rect, radius);
    if (base_radius <= 0.0f) {
        const i32 steps = std::max(1, static_cast<i32>(std::ceil(width)));
        for (i32 i = 0; i < steps; ++i) {
            const f32 inset = static_cast<f32>(i);
            draw_rect({rect.x + inset, rect.y + inset, std::max(0.0f, rect.w - inset * 2.0f), std::max(0.0f, rect.h - inset * 2.0f)}, color);
        }
        return;
    }

    flush_batch();
    ++_stats.direct_rect_outlines;

    constexpr f32 fringe = 1.0f;
    const f32 border_width = std::max(0.0f, width);
    const Rectf outer_solid_rect = inset_rect(rect, fringe);
    const Rectf inner_rect = inset_rect(rect, border_width + fringe);
    if (outer_solid_rect.w <= 0.0f || outer_solid_rect.h <= 0.0f) {
        return;
    }

    const std::vector<Vec2f> outer_fringe = mapped(rounded_rect_loop(rect, base_radius));
    const std::vector<Vec2f> outer_solid = mapped(rounded_rect_loop(outer_solid_rect, std::max(0.0f, base_radius - fringe)));
    render_loop_ring(_handle, outer_fringe, alpha_scaled(color, 0.0f), outer_solid, color);
    if (inner_rect.w <= 0.0f || inner_rect.h <= 0.0f) {
        const OpaqueDrawScope opaque(_handle, _blend, color.a == 255);
        render_filled_loop(_handle, outer_solid, color);
        return;
    }
    const std::vector<Vec2f> inner_solid =
        mapped(rounded_rect_loop(inner_rect, std::max(0.0f, base_radius - border_width - fringe)));
    render_loop_ring(_handle, outer_solid, color, inner_solid, color);

    const Rectf inner_fringe_rect = inset_rect(rect, border_width);
    if (inner_fringe_rect.w > 0.0f && inner_fringe_rect.h > 0.0f) {
        const std::vector<Vec2f> inner_fringe =
            mapped(rounded_rect_loop(inner_fringe_rect, std::max(0.0f, base_radius - border_width)));
        render_loop_ring(_handle, inner_fringe, alpha_scaled(color, 0.0f), inner_solid, color);
    }
}

void SdlRenderer2DBackend::fill_gradient_rect(Rectf rect, const Gradient& gradient) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    flush_batch();
    ++_stats.direct_rect_fills;

    const SDL_FColor start = to_sdl_color(gradient.start);
    const SDL_FColor end = to_sdl_color(gradient.end);
    // Per-corner colors. Vertical: start at top edge, end at bottom edge.
    // Horizontal: start at left edge, end at right edge.
    const bool vertical = gradient.direction == GradientDirection::Vertical;
    const SDL_FColor top_left = start;
    const SDL_FColor top_right = vertical ? start : end;
    const SDL_FColor bottom_right = end;
    const SDL_FColor bottom_left = vertical ? end : start;

    const std::vector<Vec2f> corners = mapped({{rect.x, rect.y},
                                               {rect.x + rect.w, rect.y},
                                               {rect.x + rect.w, rect.y + rect.h},
                                               {rect.x, rect.y + rect.h}});
    const std::array<SDL_Vertex, 4> vertices{{
        {.position = {corners[0].x, corners[0].y}, .color = top_left, .tex_coord = {0.0f, 0.0f}},
        {.position = {corners[1].x, corners[1].y}, .color = top_right, .tex_coord = {0.0f, 0.0f}},
        {.position = {corners[2].x, corners[2].y}, .color = bottom_right, .tex_coord = {0.0f, 0.0f}},
        {.position = {corners[3].x, corners[3].y}, .color = bottom_left, .tex_coord = {0.0f, 0.0f}},
    }};
    const std::array<int, 6> indices{{0, 1, 2, 0, 2, 3}};
    const OpaqueDrawScope opaque(_handle, _blend, gradient.start.a == 255 && gradient.end.a == 255);
    SDL_RenderGeometry(_handle, nullptr, vertices.data(), 4, indices.data(), 6);
}

ShaderHandle SdlRenderer2DBackend::create_shader(const ShaderDesc& desc) {
    if (!_gpu_device) {
        return {}; // not on the gpu driver → caller degrades to a fill
    }
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(_gpu_device);
    SDL_GPUShaderCreateInfo info{};
    if ((formats & SDL_GPU_SHADERFORMAT_SPIRV) && desc.spirv.valid()) {
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = desc.spirv.code;
        info.code_size = desc.spirv.size;
    } else if ((formats & SDL_GPU_SHADERFORMAT_DXIL) && desc.dxil.valid()) {
        info.format = SDL_GPU_SHADERFORMAT_DXIL;
        info.code = desc.dxil.code;
        info.code_size = desc.dxil.size;
    } else if ((formats & SDL_GPU_SHADERFORMAT_DXBC) && desc.dxbc.valid()) {
        info.format = SDL_GPU_SHADERFORMAT_DXBC;
        info.code = desc.dxbc.code;
        info.code_size = desc.dxbc.size;
    } else if ((formats & SDL_GPU_SHADERFORMAT_MSL) && desc.msl.valid()) {
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.code = desc.msl.code;
        info.code_size = desc.msl.size;
    } else {
        KIN_LOG_ERROR_F("render",
                        "create_shader: no blob matching device shader format",
                        (LogFields{{.name = "formats", .value = std::to_string(formats)}}));
        return {};
    }
    info.entrypoint = desc.entrypoint ? desc.entrypoint : "main";
    info.num_samplers = desc.num_samplers;
    info.num_uniform_buffers = desc.num_uniform_buffers;
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;

    SDL_GPUShader* shader = SDL_CreateGPUShader(_gpu_device, &info);
    if (!shader) {
        KIN_LOG_ERROR_F("render",
                        "SDL_CreateGPUShader failed",
                        (LogFields{{.name = "error", .value = SDL_GetError()}}));
        return {};
    }
    SDL_GPURenderStateCreateInfo createinfo{};
    createinfo.fragment_shader = shader;
    SDL_GPURenderState* state = SDL_CreateGPURenderState(_handle, &createinfo);
    if (!state) {
        KIN_LOG_ERROR_F("render",
                        "SDL_CreateGPURenderState failed",
                        (LogFields{{.name = "error", .value = SDL_GetError()}}));
        SDL_ReleaseGPUShader(_gpu_device, shader);
        return {};
    }
    _shaders.push_back({shader, state, desc.num_uniform_buffers});
    KIN_LOG_INFO_F("render",
                   "shader created",
                   (LogFields{{.name = "handle", .value = std::to_string(_shaders.size())},
                              {.name = "format", .value = std::to_string(info.format)}}));
    return ShaderHandle{static_cast<u64>(_shaders.size())}; // value == index + 1
}

void SdlRenderer2DBackend::draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params) {
    if (!_gpu_device || handle.value == 0 || handle.value > _shaders.size() ||
        rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const ShaderEntry& entry = _shaders[static_cast<std::size_t>(handle.value) - 1];
    if (!entry.state) {
        return;
    }
    if (!_white_texture.valid()) {
        const std::array<u8, 4> white{255, 255, 255, 255};
        _white_texture = create_texture_from_rgba(white.data(), {1, 1});
        if (!_white_texture.valid()) {
            return;
        }
    }

    const auto* white = as_sdl(_white_texture.backend().get());
    if (!white) {
        return;
    }
    flush_batch(); // emit pending normal draws before switching the fragment shader
    if (entry.num_uniform_buffers > 0) {
        SDL_SetGPURenderStateFragmentUniforms(
            entry.state, 0, params.uniforms.data(),
            static_cast<u32>(params.uniforms.size() * sizeof(f32)));
    }
    SDL_SetGPURenderState(_handle, entry.state);
    // Draw the white texture stretched over `rect`; SDL feeds v_uv 0..1 and binds
    // it at sampler slot 0, so the custom fragment shader colors the surface.
    if (_transformed) {
        const Vec2f origin = _transform.apply({rect.x, rect.y});
        const Vec2f right = _transform.apply({rect.x + rect.w, rect.y});
        const Vec2f down = _transform.apply({rect.x, rect.y + rect.h});
        const SDL_FPoint o{origin.x, origin.y}, r{right.x, right.y}, d{down.x, down.y};
        SDL_RenderTextureAffine(_handle, white->handle(), nullptr, &o, &r, &d);
    } else {
        const SDL_FRect dst = to_sdl_frect(rect);
        SDL_RenderTexture(_handle, white->handle(), nullptr, &dst);
    }
    SDL_SetGPURenderState(_handle, nullptr);
}

void SdlRenderer2DBackend::draw_line(Vec2f a, Vec2f b, Color color) {
    if (_transformed) {
        // A quad one unit wide over the pixel centres, as the GPU backend draws
        // lines, then mapped: it widens with the scale.
        a = {a.x + 0.5f, a.y + 0.5f};
        b = {b.x + 0.5f, b.y + 0.5f};
        const f32 dx = b.x - a.x, dy = b.y - a.y;
        const f32 len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0001f || color.a == 0) {
            return;
        }
        const f32 nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
        begin_batch(BatchKind::Color, nullptr);
        append_corners({{_transform.apply({a.x + nx, a.y + ny}), _transform.apply({b.x + nx, b.y + ny}),
                         _transform.apply({b.x - nx, b.y - ny}), _transform.apply({a.x - nx, a.y - ny})}},
                       {}, color);
        return;
    }
    flush_batch();
    ++_stats.direct_lines;
    const SDL_FColor sdl_color = to_sdl_color(color);
    SDL_SetRenderDrawColorFloat(_handle, sdl_color.r, sdl_color.g, sdl_color.b, sdl_color.a);
    const OpaqueDrawScope opaque(_handle, _blend, color.a == 255);
    SDL_RenderLine(_handle, a.x, a.y, b.x, b.y);
}

void SdlRenderer2DBackend::set_viewport(Rectf rect) {
    flush_batch();
    const SDL_Rect sdl_rect = to_sdl_rect(rect);
    SDL_SetRenderViewport(_handle, &sdl_rect);
}

void SdlRenderer2DBackend::reset_viewport() {
    flush_batch();
    SDL_SetRenderViewport(_handle, nullptr);
}

void SdlRenderer2DBackend::push_viewport(Rectf rect) {
    flush_batch();
    _viewport_stack.push_back(capture_viewport());
    set_viewport(rect);
}

void SdlRenderer2DBackend::pop_viewport() {
    flush_batch();
    if (_viewport_stack.empty()) {
        reset_viewport();
        return;
    }

    const ViewportState state = _viewport_stack.back();
    _viewport_stack.pop_back();
    restore_viewport(state);
}

void SdlRenderer2DBackend::push_clip(Rectf rect) {
    flush_batch();
    const ViewportState state = capture_viewport();
    _clip_stack.push_back(state);
    SDL_Rect clip = to_sdl_rect(rect);
    if (state.clip_enabled) {
        clip = intersect_rects(state.clip, clip);
    }
    SDL_SetRenderClipRect(_handle, &clip);
}

void SdlRenderer2DBackend::pop_clip() {
    flush_batch();
    if (_clip_stack.empty()) {
        SDL_SetRenderClipRect(_handle, nullptr);
        return;
    }

    const ViewportState state = _clip_stack.back();
    _clip_stack.pop_back();
    SDL_SetRenderClipRect(_handle, state.clip_enabled ? &state.clip : nullptr);
}

RenderTarget SdlRenderer2DBackend::create_render_target(Vec2i size, ScaleMode mode) {
    if (size.x <= 0 || size.y <= 0) {
        throw std::runtime_error("create_render_target requires a positive size");
    }

    SDL_Texture* texture = SDL_CreateTexture(
        _handle,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET,
        size.x,
        size.y
    );
    if (!texture) {
        const std::string error = std::string("SDL_CreateTexture (target) failed: ") + SDL_GetError();
        KIN_LOG_ERROR_F("render",
                        "render target creation failed",
                        (LogFields{
                            {.name = "width", .value = std::to_string(size.x)},
                            {.name = "height", .value = std::to_string(size.y)},
                            {.name = "error", .value = error},
                        }));
        throw std::runtime_error(error);
    }

    // Targets store premultiplied alpha: this blend mode composites them correctly
    // when sampled/drawn out, and drawing straight-alpha sources onto a
    // transparent-cleared target accumulates as premultiplied automatically.
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(texture, mode == ScaleMode::Nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);

    return RenderTarget{Texture{std::make_shared<SdlTextureBackend>(texture, size, _alive)}};
}

void SdlRenderer2DBackend::push_render_target(const RenderTarget& target) {
    flush_batch();
    _saved_transforms.push_back(_transform);
    set_transform({});
    _render_target_stack.push_back(SDL_GetRenderTarget(_handle));
    _layer_stack.push_back(false);

    SDL_Texture* handle = nullptr;
    if (const auto* sdl = as_sdl(target.texture().backend().get())) {
        handle = sdl->handle();
    }
    SDL_SetRenderTarget(_handle, handle);
}

void SdlRenderer2DBackend::pop_render_target() {
    flush_batch();
    SDL_Texture* previous = nullptr;
    if (!_render_target_stack.empty()) {
        previous = _render_target_stack.back();
        _render_target_stack.pop_back();
        _layer_stack.pop_back();
    }
    SDL_SetRenderTarget(_handle, previous);
    if (!_saved_transforms.empty()) {
        set_transform(_saved_transforms.back());
        _saved_transforms.pop_back();
    }
}

bool SdlRenderer2DBackend::push_layer_target(const RenderTarget& target) {
    const auto* sdl = as_sdl(target.texture().backend().get());
    if (!sdl || !sdl->handle()) {
        return false;
    }
    flush_batch();
    // Drawn as the current target is: its logical presentation (or its pixels,
    // stretched over the layer's), viewport and clip. The transform stays.
    int w = 0, h = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(_handle, &w, &h, &mode);
    if (mode == SDL_LOGICAL_PRESENTATION_DISABLED) {
        SDL_GetCurrentRenderOutputSize(_handle, &w, &h);
        mode = SDL_LOGICAL_PRESENTATION_STRETCH;
    } else if (mode == SDL_LOGICAL_PRESENTATION_INTEGER_SCALE && target.size() != current_target_pixels()) {
        mode = SDL_LOGICAL_PRESENTATION_LETTERBOX; // a smaller layer may not fit a whole multiple
    }
    const ViewportState view = capture_viewport();
    _render_target_stack.push_back(SDL_GetRenderTarget(_handle));
    _layer_stack.push_back(true);
    SDL_SetRenderTarget(_handle, sdl->handle());
    SDL_SetRenderLogicalPresentation(_handle, w, h, mode);
    restore_viewport(view);
    Uint8 r = 0, g = 0, b = 0, a = 0;
    SDL_GetRenderDrawColor(_handle, &r, &g, &b, &a);
    SDL_SetRenderDrawColor(_handle, 0, 0, 0, 0);
    SDL_RenderClear(_handle); // all of it, whatever the viewport and clip
    SDL_SetRenderDrawColor(_handle, r, g, b, a);
    return true;
}

std::optional<IRenderer2DBackend::LayerBounds> SdlRenderer2DBackend::pop_layer_target() {
    if (_layer_stack.empty() || !_layer_stack.back()) {
        return std::nullopt;
    }
    flush_batch();
    // All of the presentation, in draw coordinates (from the viewport's
    // origin) and in the layer's pixels.
    int w = 0, h = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_GetRenderLogicalPresentation(_handle, &w, &h, &mode);
    SDL_FRect pixels{};
    SDL_GetRenderLogicalPresentationRect(_handle, &pixels);
    SDL_Rect viewport{};
    SDL_GetRenderViewport(_handle, &viewport);
    SDL_SetRenderTarget(_handle, _render_target_stack.back());
    _render_target_stack.pop_back();
    _layer_stack.pop_back();
    return LayerBounds{.dest = {static_cast<f32>(-viewport.x), static_cast<f32>(-viewport.y), static_cast<f32>(w),
                                static_cast<f32>(h)},
                       .source = {pixels.x, pixels.y, pixels.w, pixels.h}};
}

Vec2i SdlRenderer2DBackend::current_target_pixels() const {
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(_handle, &w, &h);
    return {w, h};
}

namespace {

// `rect` of a target texture as RGBA bytes, read with its view reset (the
// target is done with: a popped layer).
bool read_target(SDL_Renderer* renderer, SDL_Texture* target, const SDL_Rect& rect, std::vector<u8>& out) {
    SDL_SetRenderTarget(renderer, target);
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderViewport(renderer, nullptr);
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, &rect);
    if (!surface) {
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    if (!rgba) {
        return false;
    }
    out.resize(static_cast<std::size_t>(rgba->w) * static_cast<std::size_t>(rgba->h) * 4u);
    const auto* src = static_cast<const u8*>(rgba->pixels);
    for (int y = 0; y < rgba->h; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rgba->w) * 4u,
                    src + static_cast<std::ptrdiff_t>(y) * rgba->pitch, static_cast<std::size_t>(rgba->w) * 4u);
    }
    SDL_DestroySurface(rgba);
    return true;
}

} // namespace

bool SdlRenderer2DBackend::draw_masked_by_blending(SDL_Texture* content, SDL_Texture* mask, Rectf source, Rectf dest,
                                                   Color tint, const MaskOptions& options) {
    // The content times the mask's alpha (or what it leaves) is a blend: the
    // mask drawn over the content keeps that much of each channel. Renderers
    // that blend so (not the software one) need no read back.
    if (options.source != MaskSource::Alpha || options.mode != MaskMode::Alpha) {
        return false;
    }
    const SDL_BlendFactor keep = options.invert ? SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_BLENDFACTOR_SRC_ALPHA;
    const SDL_BlendMode multiply = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, keep, SDL_BLENDOPERATION_ADD,
                                                              SDL_BLENDFACTOR_ZERO, keep, SDL_BLENDOPERATION_ADD);
    SDL_BlendMode previous = SDL_BLENDMODE_NONE;
    SDL_GetTextureBlendMode(mask, &previous);
    if (!SDL_SetTextureBlendMode(mask, multiply)) {
        return false;
    }
    SDL_Texture* current = SDL_GetRenderTarget(_handle);
    SDL_SetRenderTarget(_handle, content); // a popped layer: its view is free to reset
    SDL_SetRenderLogicalPresentation(_handle, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderViewport(_handle, nullptr);
    SDL_SetRenderClipRect(_handle, nullptr);
    const SDL_FRect pixels{source.x, source.y, source.w, source.h};
    SDL_SetTextureColorMod(mask, 255, 255, 255);
    SDL_SetTextureAlphaMod(mask, 255);
    const bool drawn = SDL_RenderTexture(_handle, mask, &pixels, &pixels);
    SDL_SetTextureBlendMode(mask, previous);
    SDL_SetRenderTarget(_handle, current);
    if (!drawn) {
        return false;
    }
    SDL_SetTextureColorMod(content, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(content, tint.a);
    const SDL_FRect dst = to_sdl_frect(dest);
    SDL_RenderTexture(_handle, content, &pixels, &dst);
    SDL_SetTextureColorMod(content, 255, 255, 255);
    SDL_SetTextureAlphaMod(content, 255);
    ++_stats.texture_draws_submitted;
    return true;
}

bool SdlRenderer2DBackend::draw_masked(const Texture& content, const Texture& mask, Rectf source, Rectf dest,
                                       Color tint, const MaskOptions& options) {
    const auto* c = as_sdl(content.backend().get());
    const auto* m = as_sdl(mask.backend().get());
    if (!c || !m || !c->handle() || !m->handle()) {
        return false;
    }
    flush_batch();
    if (draw_masked_by_blending(c->handle(), m->handle(), source, dest, tint, options)) {
        return true;
    }
    const Vec2i size = content.size();
    const int x0 = std::clamp(static_cast<int>(std::floor(source.x)), 0, size.x);
    const int y0 = std::clamp(static_cast<int>(std::floor(source.y)), 0, size.y);
    const int x1 = std::clamp(static_cast<int>(std::ceil(source.x + source.w)), 0, size.x);
    const int y1 = std::clamp(static_cast<int>(std::ceil(source.y + source.h)), 0, size.y);
    if (x1 <= x0 || y1 <= y0) {
        return true;
    }
    const SDL_Rect rect{x0, y0, x1 - x0, y1 - y0};
    SDL_Texture* current = SDL_GetRenderTarget(_handle);
    std::vector<u8> pixels;
    std::vector<u8> coverage;
    const bool read = read_target(_handle, c->handle(), rect, pixels) && read_target(_handle, m->handle(), rect, coverage);
    SDL_SetRenderTarget(_handle, current);
    if (!read || pixels.size() != coverage.size()) {
        return true;
    }
    // Premultiplied: every channel scales by the coverage.
    constexpr f32 inv = 1.0f / 255.0f;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const f32 k = mask_coverage(options, coverage[i] * inv, coverage[i + 1] * inv, coverage[i + 2] * inv,
                                    coverage[i + 3] * inv);
        for (std::size_t j = 0; j < 4; ++j) {
            pixels[i + j] = static_cast<u8>(static_cast<f32>(pixels[i + j]) * k + 0.5f);
        }
    }
    if (!_mask_scratch || _mask_scratch_size.x < rect.w || _mask_scratch_size.y < rect.h) {
        if (_mask_scratch) {
            SDL_DestroyTexture(_mask_scratch);
        }
        _mask_scratch_size = {std::max(rect.w, _mask_scratch_size.x), std::max(rect.h, _mask_scratch_size.y)};
        _mask_scratch = SDL_CreateTexture(_handle, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                          _mask_scratch_size.x, _mask_scratch_size.y);
        if (!_mask_scratch) {
            _mask_scratch_size = {};
            return true;
        }
        SDL_SetTextureBlendMode(_mask_scratch, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    }
    const SDL_Rect area{0, 0, rect.w, rect.h};
    SDL_UpdateTexture(_mask_scratch, &area, pixels.data(), rect.w * 4);
    SDL_SetTextureScaleMode(_mask_scratch, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureColorMod(_mask_scratch, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(_mask_scratch, tint.a);
    const SDL_FRect src{source.x - static_cast<f32>(x0), source.y - static_cast<f32>(y0), source.w, source.h};
    const SDL_FRect dst = to_sdl_frect(dest);
    SDL_RenderTexture(_handle, _mask_scratch, &src, &dst);
    ++_stats.texture_draws_submitted;
    return true;
}

void SdlRenderer2DBackend::set_scale_mode(const Texture& texture, ScaleMode mode) {
    if (const auto* sdl = as_sdl(texture.backend().get());
        sdl && sdl->handle()) {
        SDL_SetTextureScaleMode(sdl->handle(), mode == ScaleMode::Nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    }
}

SdlRenderer2DBackend::ViewportState SdlRenderer2DBackend::capture_viewport() const {
    ViewportState state{};
    state.viewport_set = SDL_RenderViewportSet(_handle);
    state.clip_enabled = SDL_RenderClipEnabled(_handle);
    SDL_GetRenderViewport(_handle, &state.viewport);
    SDL_GetRenderClipRect(_handle, &state.clip);
    return state;
}

void SdlRenderer2DBackend::restore_viewport(const ViewportState& state) {
    SDL_SetRenderViewport(_handle, state.viewport_set ? &state.viewport : nullptr);
    SDL_SetRenderClipRect(_handle, state.clip_enabled ? &state.clip : nullptr);
}

void SdlRenderer2DBackend::set_blend_mode(BlendMode mode) {
    if (mode == _blend) {
        return;
    }
    flush_batch();
    if ((mode == BlendMode::Max || mode == BlendMode::Min) && !_min_max_blend) {
        if (!_warned_min_max) {
            _warned_min_max = true;
            KIN_LOG_WARN_F("render", "blend mode not supported, drawing as alpha",
                           (LogFields{{.name = "mode", .value = mode == BlendMode::Max ? "max" : "min"},
                                      {.name = "backend", .value = std::string{name()}}}));
        }
        mode = BlendMode::Alpha;
    }
    _blend = mode;
    SDL_SetRenderDrawBlendMode(_handle, to_sdl_blend(mode));
}

void SdlRenderer2DBackend::flush_batch() {
    if (_batch.kind == BatchKind::None || _batch.vertices.empty() || _batch.indices.empty()) {
        _batch.kind = BatchKind::None;
        _batch.opaque = true;
        _batch.retained_texture = {};
        _batch.texture = nullptr;
        _batch.vertices.clear();
        _batch.indices.clear();
        return;
    }

    const TextureBlendScope blend(_batch.texture, _blend);
    const OpaqueDrawScope opaque(_handle, _blend, _batch.kind == BatchKind::Color && _batch.opaque);
    if (!SDL_RenderGeometry(_handle,
                            _batch.texture,
                            _batch.vertices.data(),
                            static_cast<int>(_batch.vertices.size()),
                            _batch.indices.data(),
                            static_cast<int>(_batch.indices.size()))) {
        throw std::runtime_error(std::string("SDL_RenderGeometry failed: ") + SDL_GetError());
    }
    if (_batch.kind == BatchKind::Texture) {
        ++_stats.texture_batch_flushes;
    } else if (_batch.kind == BatchKind::Color) {
        ++_stats.rect_batch_flushes;
    }

    _batch.kind = BatchKind::None;
    _batch.opaque = true;
    _batch.retained_texture = {};
    _batch.texture = nullptr;
    _batch.vertices.clear();
    _batch.indices.clear();
}

void SdlRenderer2DBackend::begin_batch(BatchKind kind, SDL_Texture* texture, Texture retained_texture) {
    if (_batch.kind == kind && _batch.texture == texture) {
        if (retained_texture) {
            _batch.retained_texture = std::move(retained_texture);
        }
        return;
    }
    if (_batch.kind != BatchKind::None && !_batch.vertices.empty()) {
        ++_stats.texture_batch_breaks;
    }
    flush_batch();
    _batch.kind = kind;
    _batch.retained_texture = std::move(retained_texture);
    _batch.texture = texture;
}

void SdlRenderer2DBackend::append_quad(Rectf dest, Rectf source, Vec2i texture_size, Color color, f32 rotation, Vec2f pivot) {
    const f32 u0 = texture_size.x > 0 ? source.x / static_cast<f32>(texture_size.x) : 0.0f;
    const f32 v0 = texture_size.y > 0 ? source.y / static_cast<f32>(texture_size.y) : 0.0f;
    const f32 u1 = texture_size.x > 0 ? (source.x + source.w) / static_cast<f32>(texture_size.x) : 0.0f;
    const f32 v1 = texture_size.y > 0 ? (source.y + source.h) / static_cast<f32>(texture_size.y) : 0.0f;

    std::array<Vec2f, 4> positions{{
        {dest.x, dest.y},
        {dest.x + dest.w, dest.y},
        {dest.x + dest.w, dest.y + dest.h},
        {dest.x, dest.y + dest.h},
    }};
    if (rotation != 0.0f) {
        constexpr f32 pi = 3.14159265358979323846f;
        const f32 radians = rotation * pi / 180.0f;
        const f32 c = std::cos(radians);
        const f32 s = std::sin(radians);
        const Vec2f center{dest.x + dest.w * pivot.x, dest.y + dest.h * pivot.y};
        for (Vec2f& position : positions) {
            const Vec2f delta{position.x - center.x, position.y - center.y};
            position = {
                center.x + delta.x * c - delta.y * s,
                center.y + delta.x * s + delta.y * c,
            };
        }
    }

    if (_transformed) {
        for (Vec2f& position : positions) {
            position = _transform.apply(position);
        }
    }
    append_corners(positions, {u0, v0, u1, v1}, color);
}

std::vector<Vec2f> SdlRenderer2DBackend::mapped(std::vector<Vec2f> loop) const {
    if (_transformed) {
        for (Vec2f& p : loop) {
            p = _transform.apply(p);
        }
    }
    return loop;
}

void SdlRenderer2DBackend::append_corners(const std::array<Vec2f, 4>& positions, const std::array<f32, 4>& uv,
                                          Color color) {
    constexpr int max_indices = 60'000;
    if (_batch.indices.size() + 6 > max_indices) {
        ++_stats.texture_batch_breaks;
        const BatchKind kind = _batch.kind;
        SDL_Texture* texture = _batch.texture;
        Texture retained_texture = _batch.retained_texture;
        flush_batch();
        begin_batch(kind, texture, std::move(retained_texture));
    }
    _batch.opaque = _batch.opaque && color.a == 255;
    const SDL_FColor sdl_color = to_sdl_color(color);
    const f32 u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
    const int base = static_cast<int>(_batch.vertices.size());
    _batch.vertices.push_back({.position = {positions[0].x, positions[0].y}, .color = sdl_color, .tex_coord = {u0, v0}});
    _batch.vertices.push_back({.position = {positions[1].x, positions[1].y}, .color = sdl_color, .tex_coord = {u1, v0}});
    _batch.vertices.push_back({.position = {positions[2].x, positions[2].y}, .color = sdl_color, .tex_coord = {u1, v1}});
    _batch.vertices.push_back({.position = {positions[3].x, positions[3].y}, .color = sdl_color, .tex_coord = {u0, v1}});

    _batch.indices.push_back(base + 0);
    _batch.indices.push_back(base + 1);
    _batch.indices.push_back(base + 2);
    _batch.indices.push_back(base + 0);
    _batch.indices.push_back(base + 2);
    _batch.indices.push_back(base + 3);
}

SdlTextureBackend::SdlTextureBackend(SDL_Texture* texture, Vec2i size, std::shared_ptr<const bool> renderer_alive)
    : ITextureBackend(ITextureBackend::Kind::Sdl),
      _texture(texture),
      _size(size),
      _renderer_alive(std::move(renderer_alive)) {
}

SdlTextureBackend::~SdlTextureBackend() {
    if (_texture && *_renderer_alive) {
        SDL_DestroyTexture(_texture);
    }
}

} // namespace kin
