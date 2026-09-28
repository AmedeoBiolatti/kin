#include "gpu_renderer2d_backend.hpp"

#include <kin/platform/log.hpp>

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <stdexcept>
#include <utility>

namespace kin {

namespace {

// Tag-checked downcast — replaces dynamic_cast on the textured-draw hot path. The
// kind tag (set in GpuTextureBackend's ctor) guarantees the concrete type, so the
// static_cast is safe and the RTTI lookup is avoided. Returns null for foreign textures.
const gpu::GpuTextureBackend* as_gpu(const ITextureBackend* backend) {
    return (backend && backend->kind() == ITextureBackend::Kind::Gpu)
               ? static_cast<const gpu::GpuTextureBackend*>(backend)
               : nullptr;
}

gpu::GpuDeviceOptions device_options(bool vsync) {
    gpu::GpuDeviceOptions options;
    options.present_mode = vsync ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
    options.debug = std::getenv("KIN_GPU_DEBUG") != nullptr;
    return options;
}

SDL_FColor to_fcolor(Color color) {
    constexpr f32 inv = 1.0f / 255.0f;
    return {static_cast<f32>(color.r) * inv, static_cast<f32>(color.g) * inv,
            static_cast<f32>(color.b) * inv, static_cast<f32>(color.a) * inv};
}

SDL_Rect to_sdl_rect(Rectf rect) {
    const int x = static_cast<int>(std::floor(rect.x));
    const int y = static_cast<int>(std::floor(rect.y));
    const int w = static_cast<int>(std::ceil(rect.x + rect.w)) - x;
    const int h = static_cast<int>(std::ceil(rect.y + rect.h)) - y;
    return {x, y, std::max(0, w), std::max(0, h)};
}

SDL_Rect intersect(SDL_Rect a, SDL_Rect b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.w, b.x + b.w);
    const int y2 = std::min(a.y + a.h, b.y + b.h);
    return {x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1)};
}

Color alpha_scaled(Color color, f32 alpha) {
    color.a = static_cast<u8>(std::clamp(std::round(static_cast<f32>(color.a) * alpha), 0.0f, 255.0f));
    return color;
}

// Aspect-fit `scene` into `target`, centered (letterbox). With `integer`, snaps the
// scale to a whole multiple when upscaling. Returns the destination rect.
Rectf letterbox_rect(Vec2i scene, Vec2i target, bool integer) {
    if (scene.x <= 0 || scene.y <= 0 || target.x <= 0 || target.y <= 0) {
        return {0.0f, 0.0f, static_cast<f32>(std::max(0, target.x)), static_cast<f32>(std::max(0, target.y))};
    }
    f32 scale = std::min(static_cast<f32>(target.x) / static_cast<f32>(scene.x),
                         static_cast<f32>(target.y) / static_cast<f32>(scene.y));
    if (integer && scale >= 1.0f) {
        scale = std::floor(scale);
    }
    const f32 w = static_cast<f32>(scene.x) * scale;
    const f32 h = static_cast<f32>(scene.y) * scale;
    return {(static_cast<f32>(target.x) - w) * 0.5f, (static_cast<f32>(target.y) - h) * 0.5f, w, h};
}

Rectf inset_rect(Rectf rect, f32 inset) {
    return {rect.x + inset, rect.y + inset, std::max(0.0f, rect.w - inset * 2.0f),
            std::max(0.0f, rect.h - inset * 2.0f)};
}

// Perimeter points of a (rounded) rect — ported from sdl_renderer2d_backend.cpp.
// Fills `out` (cleared first) instead of returning a fresh vector so callers can
// reuse a scratch buffer across frames (no per-call heap churn). Geometry identical
// to the prior return-by-value form.
void rounded_rect_loop(Rectf rect, f32 radius, std::vector<Vec2f>& out) {
    out.clear();
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const f32 r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (r <= 0.0f) {
        out.push_back({rect.x, rect.y});
        out.push_back({rect.x + rect.w, rect.y});
        out.push_back({rect.x + rect.w, rect.y + rect.h});
        out.push_back({rect.x, rect.y + rect.h});
        return;
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
    out.reserve(static_cast<std::size_t>((segments + 1) * 4));
    for (std::size_t corner = 0; corner < centers.size(); ++corner) {
        for (i32 i = 0; i <= segments; ++i) {
            const f32 t = starts[corner] + (static_cast<f32>(i) / static_cast<f32>(segments)) * half_pi;
            out.push_back({centers[corner].x + std::cos(t) * r, centers[corner].y + std::sin(t) * r});
        }
    }
}

gpu::GpuVertex gv(Vec2f p, Color c) {
    return gpu::GpuVertex{p.x, p.y, 0.0f, 0.0f, c.r, c.g, c.b, c.a};
}

// Triangle-fan a closed loop into `out` (target-pixel coords, solid color).
void append_filled_loop(std::vector<gpu::GpuVertex>& out, const std::vector<Vec2f>& loop, Color color) {
    if (loop.size() < 3 || color.a == 0) {
        return;
    }
    Vec2f center{};
    for (Vec2f p : loop) {
        center.x += p.x;
        center.y += p.y;
    }
    center.x /= static_cast<f32>(loop.size());
    center.y /= static_cast<f32>(loop.size());
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const Vec2f a = loop[i];
        const Vec2f b = loop[(i + 1) % loop.size()];
        out.push_back(gv(center, color));
        out.push_back(gv(a, color));
        out.push_back(gv(b, color));
    }
}

// Emit a ring (quad strip) between two equal-length loops; per-loop colors enable
// the 1px alpha=0 feathered fringe (AA).
void append_loop_ring(std::vector<gpu::GpuVertex>& out, const std::vector<Vec2f>& outer, Color outer_color,
                      const std::vector<Vec2f>& inner, Color inner_color) {
    if (outer.size() < 3 || outer.size() != inner.size()) {
        return;
    }
    for (std::size_t i = 0; i < outer.size(); ++i) {
        const std::size_t j = (i + 1) % outer.size();
        out.push_back(gv(outer[i], outer_color));
        out.push_back(gv(inner[i], inner_color));
        out.push_back(gv(inner[j], inner_color));
        out.push_back(gv(outer[i], outer_color));
        out.push_back(gv(inner[j], inner_color));
        out.push_back(gv(outer[j], outer_color));
    }
}

} // namespace

GpuRenderer2DBackend::GpuRenderer2DBackend(Window& window, bool vsync)
    : _window(&window), _device(window, device_options(vsync)) {
    const std::filesystem::path dir{KIN_GPU_SHADER_DIR};
    _vertex_shader = gpu::GpuShader::from_file(_device, SDL_GPU_SHADERSTAGE_VERTEX,
                                               SDL_GPU_SHADERFORMAT_SPIRV,
                                               dir / "textured_quad.vert.spv",
                                               /*uniform_buffers=*/1, /*samplers=*/0);
    _fragment_shader = gpu::GpuShader::from_file(_device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                 SDL_GPU_SHADERFORMAT_SPIRV,
                                                 dir / "textured_quad.frag.spv",
                                                 /*uniform_buffers=*/0, /*samplers=*/1);

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    _sampler_linear = SDL_CreateGPUSampler(_device.handle(), &sampler_info);
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    _sampler_nearest = SDL_CreateGPUSampler(_device.handle(), &sampler_info);

    const std::array<u8, 4> white{255, 255, 255, 255};
    _white = _device.create_texture_from_rgba(white.data(), 1, 1);

    _pipelines.init(_device.handle());
    ensure_scene();

    KIN_LOG_INFO_F("render", "gpu backend created",
                   (LogFields{{.name = "driver", .value = std::string{_device.driver_name()}},
                              {.name = "vsync", .value = vsync ? "true" : "false"}}));
}

GpuRenderer2DBackend::~GpuRenderer2DBackend() {
    if (_frame) {
        _frame->submit();
        _frame.reset();
    }
    _device.wait_idle();
    _pipelines.destroy();
    if (_sampler_linear) {
        SDL_ReleaseGPUSampler(_device.handle(), _sampler_linear);
    }
    if (_sampler_nearest) {
        SDL_ReleaseGPUSampler(_device.handle(), _sampler_nearest);
    }
}

RendererBackendCapabilities GpuRenderer2DBackend::capabilities() const {
    return {
        .immediate_2d = true,
        .queued_2d = false,
        .render_targets = true,
        .blend_modes = true,
        .materials_2d = true, // G3: real SPIR-V fragment-shader materials
        .gradients = true,
        .text = false,
        .rendering_3d = false,
    };
}

void GpuRenderer2DBackend::ensure_scene() {
    // Render at NATIVE resolution for crispness: under logical presentation the
    // scene texture is the logical size scaled up by the letterbox fit (so logical
    // draw coords rasterize at window resolution, then present is a ~1:1 blit). With
    // no logical size, the scene tracks the window 1:1.
    const Vec2i window = _window ? _window->pixel_size() : Vec2i{1280, 720};
    Vec2i wanted;
    if (_logical_size.x > 0 && _logical_size.y > 0) {
        f32 scale = std::min(static_cast<f32>(window.x) / static_cast<f32>(_logical_size.x),
                             static_cast<f32>(window.y) / static_cast<f32>(_logical_size.y));
        if (scale <= 0.0f) {
            scale = 1.0f;
        }
        if (_integer_scale && scale >= 1.0f) {
            scale = std::floor(scale);
        }
        _view_scale = scale;
        wanted = {std::max(1, static_cast<i32>(std::lround(_logical_size.x * scale))),
                  std::max(1, static_cast<i32>(std::lround(_logical_size.y * scale)))};
    } else {
        _view_scale = 1.0f;
        wanted = {std::max(1, window.x), std::max(1, window.y)};
    }
    if (_scene && wanted == _scene_size) {
        return;
    }
    _device.wait_idle();
    _scene = _device.create_render_texture(static_cast<u32>(wanted.x), static_cast<u32>(wanted.y),
                                           SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    _scene_size = wanted;
}

const gpu::GpuTexture& GpuRenderer2DBackend::current_target() const {
    return _rt_stack.empty() ? _scene : *_rt_stack.back();
}

Vec2i GpuRenderer2DBackend::current_size() const {
    const gpu::GpuTexture& t = current_target();
    return {static_cast<i32>(t.width()), static_cast<i32>(t.height())};
}

bool GpuRenderer2DBackend::scene_uses_logical_coordinates() const {
    return _rt_stack.empty() && _native_stack.empty() && _logical_size.x > 0 && _logical_size.y > 0;
}

void GpuRenderer2DBackend::ensure_frame() {
    if (_frame) {
        return;
    }
    ensure_scene();
    _frame.emplace(_device.begin_frame());
    _batch.begin(current_target(), _clear_color, /*do_clear=*/false);
}

void GpuRenderer2DBackend::flush_to_frame() {
    // Draw coordinates are in "coordinate space": logical for the scene under logical
    // presentation, else the target's texture size. The uniform maps coord -> NDC so
    // logical coords fill the (native-res) scene texture; the rasterizer then renders
    // at native res. The batch sets the GPU viewport to the target's texture size.
    const bool scene_logical = scene_uses_logical_coordinates();
    const Vec2i coord = scene_logical ? _logical_size : current_size();
    gpu::GpuGeometryBatch::FlushContext ctx{};
    ctx.vertex_shader = _vertex_shader.handle();
    ctx.default_fragment = _fragment_shader.handle();
    ctx.white_texture = _white.handle();
    ctx.sampler = _sampler_linear; // default for solids/white & null-sampler ranges
    ctx.target_format = current_target().format();
    ctx.view.scale[0] = 2.0f / static_cast<f32>(std::max(1, coord.x));
    ctx.view.scale[1] = 2.0f / static_cast<f32>(std::max(1, coord.y));
    ctx.view.translate[0] = -1.0f;
    ctx.view.translate[1] = -1.0f;
    _batch.flush(*_frame, _device, _pipelines, ctx);
}

void GpuRenderer2DBackend::clear(Color color) {
    ensure_scene();
    _clear_color = to_fcolor(color);
    if (!_frame) {
        _frame.emplace(_device.begin_frame());
    } else {
        flush_to_frame(); // unusual mid-frame clear: emit prior content first
    }
    _batch.begin(current_target(), _clear_color, /*do_clear=*/true);
    _clip_stack.clear();
}

void GpuRenderer2DBackend::present() {
    ensure_frame();
    flush_to_frame();
    // Full-scene post-processing: run the chain over the scene texture (ping-pong
    // scratch RTs); the result (scene-sized) is what gets blitted to the swapchain.
    const gpu::GpuTexture* presented = run_post_chain();
    if (_frame->acquire_swapchain() && _frame->swapchain_texture()) {
        const Vec2i target{static_cast<i32>(_frame->swapchain_width()),
                           static_cast<i32>(_frame->swapchain_height())};
        const Rectf d = letterbox_rect(_scene_size, target, _integer_scale);
        SDL_GPUBlitInfo blit{};
        blit.source.texture = presented->handle();
        blit.source.w = static_cast<u32>(_scene_size.x);
        blit.source.h = static_cast<u32>(_scene_size.y);
        blit.destination.texture = _frame->swapchain_texture();
        blit.destination.x = static_cast<u32>(std::max(0.0f, d.x));
        blit.destination.y = static_cast<u32>(std::max(0.0f, d.y));
        blit.destination.w = static_cast<u32>(std::max(1.0f, d.w));
        blit.destination.h = static_cast<u32>(std::max(1.0f, d.h));
        blit.load_op = SDL_GPU_LOADOP_CLEAR; // black bars
        blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
        blit.filter = _integer_scale ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(_frame->command_buffer(), &blit);
    }
    _frame->submit();
    _frame.reset();
}

SDL_Rect GpuRenderer2DBackend::current_scissor() const {
    // Clips are stored in coordinate space (logical for the scene under logical
    // presentation, target pixels otherwise). The GPU scissor needs native texture
    // pixels, so scale by the coord->texture factor (_view_scale for the scene, 1 for RTs).
    const bool scene_logical = scene_uses_logical_coordinates();
    const f32 sc = scene_logical ? _view_scale : 1.0f;
    const Vec2i tex = current_size();
    SDL_Rect c;
    if (!_clip_stack.empty()) {
        const SDL_Rect& r = _clip_stack.back();
        c = {static_cast<int>(std::floor(r.x * sc)), static_cast<int>(std::floor(r.y * sc)),
             static_cast<int>(std::ceil(r.w * sc)), static_cast<int>(std::ceil(r.h * sc))};
    } else {
        c = {0, 0, tex.x, tex.y};
    }
    return intersect(c, SDL_Rect{0, 0, tex.x, tex.y});
}

void GpuRenderer2DBackend::apply_view_offset(std::span<gpu::GpuVertex> verts) const {
    if (_view_offset.x == 0.0f && _view_offset.y == 0.0f) {
        return;
    }
    for (gpu::GpuVertex& v : verts) {
        v.x += _view_offset.x;
        v.y += _view_offset.y;
    }
}

gpu::GpuBlendMode GpuRenderer2DBackend::resolve_blend(gpu::GpuBlendMode natural) const {
    switch (_blend) {
    case BlendMode::Additive: return gpu::GpuBlendMode::Additive;
    case BlendMode::Multiply: return gpu::GpuBlendMode::Multiply;
    case BlendMode::Replace: return gpu::GpuBlendMode::Replace;
    case BlendMode::Alpha: break;
    }
    return natural;
}

void GpuRenderer2DBackend::push_triangles(std::span<gpu::GpuVertex> tris,
                                          SDL_GPUTexture* texture, gpu::GpuBlendMode blend) {
    if (tris.empty()) {
        return;
    }
    ensure_frame();
    apply_view_offset(tris); // no-op when no viewport offset is active
    _batch.push(tris, nullptr, texture, current_scissor(), resolve_blend(blend));
}

void GpuRenderer2DBackend::push_quad(
    Rectf dest,
    Rectf uv,
    Color color,
    SDL_GPUTexture* texture,
    gpu::GpuBlendMode blend,
    SDL_GPUSampler* sampler
) {
    ensure_frame();
    const f32 x0 = dest.x;
    const f32 y0 = dest.y;
    const f32 x1 = dest.x + dest.w;
    const f32 y1 = dest.y + dest.h;
    const f32 u0 = uv.x;
    const f32 v0 = uv.y;
    const f32 u1 = uv.x + uv.w;
    const f32 v1 = uv.y + uv.h;
    const u8 r = color.r, g = color.g, b = color.b, a = color.a;
    std::array<gpu::GpuVertex, 6> verts{{
        {x0, y0, u0, v0, r, g, b, a},
        {x1, y0, u1, v0, r, g, b, a},
        {x1, y1, u1, v1, r, g, b, a},
        {x0, y0, u0, v0, r, g, b, a},
        {x1, y1, u1, v1, r, g, b, a},
        {x0, y1, u0, v1, r, g, b, a},
    }};
    apply_view_offset(verts);
    _batch.push(verts, nullptr, texture, current_scissor(), resolve_blend(blend), nullptr, 0, sampler);
}

void GpuRenderer2DBackend::fill_rect(Rectf rect, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) {
        return;
    }
    push_quad(rect, {0.0f, 0.0f, 1.0f, 1.0f}, color, nullptr);
}

void GpuRenderer2DBackend::draw_rect(Rectf rect, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) {
        return;
    }
    const f32 t = 1.0f;
    fill_rect({rect.x, rect.y, rect.w, t}, color);                       // top
    fill_rect({rect.x, rect.y + rect.h - t, rect.w, t}, color);          // bottom
    fill_rect({rect.x, rect.y, t, rect.h}, color);                       // left
    fill_rect({rect.x + rect.w - t, rect.y, t, rect.h}, color);          // right
}

void GpuRenderer2DBackend::draw_line(Vec2f a, Vec2f b, Color color) {
    if (color.a == 0) {
        return;
    }
    // The 1px quad is centered on the segment, which (with the top-left fill rule) lands a
    // pixel up-and-left of fill_rect / SDL_RenderLine. Shift onto pixel centers so an
    // integer-coord line fills the same pixel a fill_rect would (matches the SDL backend).
    a.x += 0.5f;
    a.y += 0.5f;
    b.x += 0.5f;
    b.y += 0.5f;
    const f32 dx = b.x - a.x;
    const f32 dy = b.y - a.y;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0001f) {
        return;
    }
    const f32 nx = -dy / len * 0.5f;
    const f32 ny = dx / len * 0.5f;
    const u8 r = color.r, g = color.g, bl = color.b, al = color.a;
    std::array<gpu::GpuVertex, 6> verts{{
        {a.x + nx, a.y + ny, 0.0f, 0.0f, r, g, bl, al},
        {b.x + nx, b.y + ny, 1.0f, 0.0f, r, g, bl, al},
        {b.x - nx, b.y - ny, 1.0f, 1.0f, r, g, bl, al},
        {a.x + nx, a.y + ny, 0.0f, 0.0f, r, g, bl, al},
        {b.x - nx, b.y - ny, 1.0f, 1.0f, r, g, bl, al},
        {a.x - nx, a.y - ny, 0.0f, 1.0f, r, g, bl, al},
    }};
    ensure_frame();
    apply_view_offset(verts);
    _batch.push(verts, nullptr, nullptr, current_scissor(), resolve_blend(gpu::GpuBlendMode::Alpha));
}

void GpuRenderer2DBackend::fill_rounded_rect(Rectf rect, f32 radius, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) {
        return;
    }

    std::vector<Vec2f>& core = _scratch_loop[0];
    std::vector<Vec2f>& outer = _scratch_loop[1];
    rounded_rect_loop(inset_rect(rect, 1.0f), std::max(0.0f, radius - 1.0f), core);
    rounded_rect_loop(rect, radius, outer);
    _scratch_verts.clear();
    append_filled_loop(_scratch_verts, core, color);
    append_loop_ring(_scratch_verts, outer, alpha_scaled(color, 0.0f), core, color);
    push_triangles(_scratch_verts, nullptr, gpu::GpuBlendMode::Alpha);
}

void GpuRenderer2DBackend::draw_rounded_rect(Rectf rect, f32 radius, Color color, f32 width) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0 || width <= 0.0f) {
        return;
    }

    const f32 fringe = 1.0f;
    std::vector<Vec2f>& outer = _scratch_loop[0];
    std::vector<Vec2f>& stroke_outer = _scratch_loop[1];
    std::vector<Vec2f>& stroke_inner = _scratch_loop[2];
    std::vector<Vec2f>& inner = _scratch_loop[3];
    const f32 inner_inset = fringe + width;
    rounded_rect_loop(rect, radius, outer);
    rounded_rect_loop(inset_rect(rect, fringe), std::max(0.0f, radius - fringe), stroke_outer);
    rounded_rect_loop(inset_rect(rect, inner_inset), std::max(0.0f, radius - inner_inset), stroke_inner);
    rounded_rect_loop(inset_rect(rect, inner_inset + fringe), std::max(0.0f, radius - inner_inset - fringe), inner);
    _scratch_verts.clear();
    append_loop_ring(_scratch_verts, outer, alpha_scaled(color, 0.0f), stroke_outer, color);
    append_loop_ring(_scratch_verts, stroke_outer, color, stroke_inner, color);
    append_loop_ring(_scratch_verts, stroke_inner, color, inner, alpha_scaled(color, 0.0f));
    push_triangles(_scratch_verts, nullptr, gpu::GpuBlendMode::Alpha);
}

void GpuRenderer2DBackend::fill_gradient_rect(Rectf rect, const Gradient& gradient) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }

    const bool vertical = gradient.direction == GradientDirection::Vertical;
    const Color top_left = gradient.start;
    const Color top_right = vertical ? gradient.start : gradient.end;
    const Color bottom_right = gradient.end;
    const Color bottom_left = vertical ? gradient.end : gradient.start;
    const f32 x0 = rect.x;
    const f32 y0 = rect.y;
    const f32 x1 = rect.x + rect.w;
    const f32 y1 = rect.y + rect.h;
    _scratch_verts.clear();
    _scratch_verts.push_back(gv({x0, y0}, top_left));
    _scratch_verts.push_back(gv({x1, y0}, top_right));
    _scratch_verts.push_back(gv({x1, y1}, bottom_right));
    _scratch_verts.push_back(gv({x0, y0}, top_left));
    _scratch_verts.push_back(gv({x1, y1}, bottom_right));
    _scratch_verts.push_back(gv({x0, y1}, bottom_left));
    push_triangles(_scratch_verts, nullptr, gpu::GpuBlendMode::Alpha);
}

Texture GpuRenderer2DBackend::create_texture_from_rgba(const u8* pixels, Vec2i size) {
    if (!pixels || size.x <= 0 || size.y <= 0) {
        return {};
    }
    gpu::GpuTexture tex = _device.create_texture_from_rgba(pixels, static_cast<u32>(size.x),
                                                           static_cast<u32>(size.y));
    return Texture{std::make_shared<gpu::GpuTextureBackend>(std::move(tex))};
}

bool GpuRenderer2DBackend::update_texture(const Texture& texture, Vec2i at, Vec2i size, const u8* pixels) {
    const auto* backend = as_gpu(texture.backend().get());
    if (!backend || !backend->texture()) {
        return false;
    }
    _device.update_texture(backend->texture().handle(), static_cast<u32>(at.x), static_cast<u32>(at.y),
                           static_cast<u32>(size.x), static_cast<u32>(size.y), pixels);
    return true;
}

void GpuRenderer2DBackend::draw_texture(const Texture& texture, Rectf dest) {
    draw_texture(texture, {0.0f, 0.0f, static_cast<f32>(texture.size().x), static_cast<f32>(texture.size().y)},
                 dest, Color{255, 255, 255, 255});
}

void GpuRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest) {
    draw_texture(texture, source, dest, Color{255, 255, 255, 255});
}

void GpuRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) {
    const auto* backend = as_gpu(texture.backend().get());
    if (!backend || dest.w <= 0.0f || dest.h <= 0.0f) {
        return;
    }
    const Vec2i size = backend->size();
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    const Rectf uv{source.x / static_cast<f32>(size.x), source.y / static_cast<f32>(size.y),
                   source.w / static_cast<f32>(size.x), source.h / static_cast<f32>(size.y)};
    // Render targets store premultiplied alpha — composite them with the
    // premultiplied blend (matches the SDL backend).
    const gpu::GpuBlendMode blend = backend->premultiplied() ? gpu::GpuBlendMode::Premultiplied
                                                             : gpu::GpuBlendMode::Alpha;
    SDL_GPUSampler* sampler =
        backend->scale_mode() == ScaleMode::Linear ? _sampler_linear : _sampler_nearest;
    push_quad(dest, uv, tint, backend->texture().handle(), blend, sampler);
}

void GpuRenderer2DBackend::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint,
                                        f32 rotation, Vec2f pivot) {
    if (rotation == 0.0f) {
        draw_texture(texture, source, dest, tint);
        return;
    }
    const auto* backend = as_gpu(texture.backend().get());
    if (!backend || dest.w <= 0.0f || dest.h <= 0.0f) {
        return;
    }
    const Vec2i size = backend->size();
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    const f32 u0 = source.x / static_cast<f32>(size.x);
    const f32 v0 = source.y / static_cast<f32>(size.y);
    const f32 u1 = (source.x + source.w) / static_cast<f32>(size.x);
    const f32 v1 = (source.y + source.h) / static_cast<f32>(size.y);
    // Rotate the four dest corners around the pivot. Matches SdlRenderer2DBackend::
    // append_quad: rotation in degrees (clockwise, y-down), pivot normalized in dest.
    constexpr f32 pi = 3.14159265358979323846f;
    const f32 radians = rotation * pi / 180.0f;
    const f32 c = std::cos(radians);
    const f32 s = std::sin(radians);
    const Vec2f center{dest.x + dest.w * pivot.x, dest.y + dest.h * pivot.y};
    const auto rot = [&](f32 x, f32 y) -> Vec2f {
        const f32 dx = x - center.x;
        const f32 dy = y - center.y;
        return {center.x + dx * c - dy * s, center.y + dx * s + dy * c};
    };
    const Vec2f p0 = rot(dest.x, dest.y);
    const Vec2f p1 = rot(dest.x + dest.w, dest.y);
    const Vec2f p2 = rot(dest.x + dest.w, dest.y + dest.h);
    const Vec2f p3 = rot(dest.x, dest.y + dest.h);
    const u8 r = tint.r, g = tint.g, b = tint.b, a = tint.a;
    std::array<gpu::GpuVertex, 6> verts{{
        {p0.x, p0.y, u0, v0, r, g, b, a},
        {p1.x, p1.y, u1, v0, r, g, b, a},
        {p2.x, p2.y, u1, v1, r, g, b, a},
        {p0.x, p0.y, u0, v0, r, g, b, a},
        {p2.x, p2.y, u1, v1, r, g, b, a},
        {p3.x, p3.y, u0, v1, r, g, b, a},
    }};
    apply_view_offset(verts);
    ensure_frame();
    const gpu::GpuBlendMode blend = backend->premultiplied() ? gpu::GpuBlendMode::Premultiplied
                                                             : gpu::GpuBlendMode::Alpha;
    SDL_GPUSampler* sampler =
        backend->scale_mode() == ScaleMode::Linear ? _sampler_linear : _sampler_nearest;
    _batch.push(verts, nullptr, backend->texture().handle(), current_scissor(), resolve_blend(blend), nullptr, 0,
                sampler);
}

bool GpuRenderer2DBackend::save_png(const char* path) {
    // Scene screenshots should match presentation, including post-processing.
    // Bound render targets are captured directly at their own size.
    // Note: called after present() resets the frame, so `_scene` already holds the last
    // rendered frame — spin up a transient frame to run the chain over it.
    const bool scene_target = _rt_stack.empty();
    const gpu::GpuTexture* src = &current_target();
    Vec2i src_size = current_size();

    if (_frame) {
        flush_to_frame();
        if (scene_target) {
            src = run_post_chain();
            src_size = {static_cast<i32>(src->width()), static_cast<i32>(src->height())};
        }
        _frame->submit();
        _frame.reset();
    } else if (scene_target && _scene && !_post_passes.empty()) {
        _frame.emplace(_device.begin_frame());
        src = run_post_chain();
        src_size = {static_cast<i32>(src->width()), static_cast<i32>(src->height())};
        _frame->submit();
        _frame.reset();
    }
    std::vector<u8> rgba;
    if (!_device.read_texture_rgba(*src, rgba)) {
        return false;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(src_size.x, src_size.y, SDL_PIXELFORMAT_RGBA32,
                                                 rgba.data(), src_size.x * 4);
    if (!surface) {
        return false;
    }
    const bool ok = IMG_SavePNG(surface, path);
    SDL_DestroySurface(surface);
    return ok;
}

bool GpuRenderer2DBackend::read_rgba(Rectf logical_region, std::vector<u8>& out, Vec2i& out_size) {
    out.clear();
    out_size = {};
    if (logical_region.w <= 0.0f || logical_region.h <= 0.0f) {
        return false;
    }
    // Render everything so far, then download the current target.
    ensure_frame();
    const gpu::GpuTexture& target = current_target();
    const Vec2i target_size = current_size();
    flush_to_frame();
    _frame->submit();
    _frame.reset();

    std::vector<u8> full;
    if (!_device.read_texture_rgba(target, full)) {
        return false;
    }
    const int sw = target_size.x;
    const int sh = target_size.y;
    // Under logical presentation the scene texture is native-res; scale the
    // logical-space region up to native pixels before reading.
    const bool scene_logical = scene_uses_logical_coordinates();
    const f32 sc = scene_logical ? _view_scale : 1.0f;
    const Rectf native_region{logical_region.x * sc, logical_region.y * sc,
                              logical_region.w * sc, logical_region.h * sc};
    SDL_Rect r = intersect(to_sdl_rect(native_region), SDL_Rect{0, 0, sw, sh});
    if (r.w <= 0 || r.h <= 0) {
        return false;
    }
    out_size = {r.w, r.h};
    out.resize(static_cast<std::size_t>(r.w) * static_cast<std::size_t>(r.h) * 4u);
    const std::size_t src_pitch = static_cast<std::size_t>(sw) * 4u;
    const std::size_t dst_pitch = static_cast<std::size_t>(r.w) * 4u;
    for (int y = 0; y < r.h; ++y) {
        std::memcpy(out.data() + dst_pitch * static_cast<std::size_t>(y),
                    full.data() + src_pitch * static_cast<std::size_t>(r.y + y) +
                        static_cast<std::size_t>(r.x) * 4u,
                    dst_pitch);
    }
    return true;
}

bool GpuRenderer2DBackend::blit_region_to_target(Rectf region, const RenderTarget& dst) {
    if (region.w <= 0.0f || region.h <= 0.0f) {
        return false;
    }
    const auto* backend = as_gpu(dst.texture().backend().get());
    if (!backend || !backend->texture()) {
        return false;
    }
    ensure_frame();
    flush_to_frame(); // finalize the active target's content-so-far (ends the render pass)

    // Map the logical region to native target pixels (scene is native-res under logical
    // presentation; render targets are 1:1).
    const bool scene_logical = scene_uses_logical_coordinates();
    const f32 sc = scene_logical ? _view_scale : 1.0f;
    const gpu::GpuTexture& src = current_target();

    SDL_GPUBlitInfo blit{};
    blit.source.texture = src.handle();
    blit.source.x = static_cast<u32>(std::max(0.0f, region.x * sc));
    blit.source.y = static_cast<u32>(std::max(0.0f, region.y * sc));
    blit.source.w = static_cast<u32>(std::max(1.0f, region.w * sc));
    blit.source.h = static_cast<u32>(std::max(1.0f, region.h * sc));
    blit.destination.texture = backend->texture().handle();
    blit.destination.w = backend->texture().width();
    blit.destination.h = backend->texture().height();
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(_frame->command_buffer(), &blit); // GPU->GPU copy, no CPU round-trip

    _batch.begin(current_target(), _clear_color, /*do_clear=*/false); // resume drawing
    return true;
}

Vec2i GpuRenderer2DBackend::region_pixel_size(Rectf region) const {
    // The scene is native-res under logical presentation; render targets are 1:1.
    const bool scene_logical = scene_uses_logical_coordinates();
    const f32 sc = scene_logical ? _view_scale : 1.0f;
    return {static_cast<i32>(region.w * sc + 0.5f), static_cast<i32>(region.h * sc + 0.5f)};
}

void GpuRenderer2DBackend::set_logical_size(Vec2i size) {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        _frame.reset();
    }
    _logical_size = size; // scene renders at this size; present letterboxes to the window
    _integer_scale = false;
    ensure_scene();
}

void GpuRenderer2DBackend::set_integer_logical_size(Vec2i size) {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        _frame.reset();
    }
    _logical_size = size;
    _integer_scale = true;
    ensure_scene();
}

void GpuRenderer2DBackend::push_native_coordinates() {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        _frame.reset();
    }

    _native_stack.push_back(NativeState{
        .logical_size = _logical_size,
        .integer_scale = _integer_scale,
        .view_scale = _view_scale,
        .clip_stack = _clip_stack,
        .view_offset = _view_offset,
        .view_offset_stack = _view_offset_stack,
    });

    _clip_stack.clear();
    _view_offset = {0.0f, 0.0f};
    _view_offset_stack.clear();
}

void GpuRenderer2DBackend::pop_native_coordinates() {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        _frame.reset();
    }

    if (_native_stack.empty()) {
        _logical_size = {};
        _integer_scale = false;
        _view_scale = 1.0f;
        _clip_stack.clear();
        _view_offset = {0.0f, 0.0f};
        _view_offset_stack.clear();
    } else {
        NativeState state = std::move(_native_stack.back());
        _native_stack.pop_back();
        _logical_size = state.logical_size;
        _integer_scale = state.integer_scale;
        _view_scale = state.view_scale;
        _clip_stack = std::move(state.clip_stack);
        _view_offset = state.view_offset;
        _view_offset_stack = std::move(state.view_offset_stack);
    }
    ensure_scene();
}

Vec2i GpuRenderer2DBackend::output_size() const {
    // Layout space is logical when presentation is active; native-coordinate scopes
    // and render targets expose the current texture's pixel size.
    if (scene_uses_logical_coordinates()) {
        return _logical_size;
    }
    return _scene_size;
}

Vec2f GpuRenderer2DBackend::window_to_logical(Vec2f window_px) const {
    const Vec2i win = _window ? _window->size() : _scene_size;
    const Vec2i pixels = _window ? _window->pixel_size() : _scene_size;
    if (win.x <= 0 || win.y <= 0 || pixels.x <= 0 || pixels.y <= 0) return window_px;
    const Vec2f physical{window_px.x * pixels.x / win.x, window_px.y * pixels.y / win.y};
    if (!scene_uses_logical_coordinates()) {
        return physical;
    }
    const Rectf d = letterbox_rect(_scene_size, pixels, _integer_scale);
    if (d.w <= 0.0f || d.h <= 0.0f || _view_scale <= 0.0f) {
        return window_px;
    }
    // window -> native scene pixels (present blit) -> logical (/ _view_scale).
    const f32 scene_x = (physical.x - d.x) * static_cast<f32>(_scene_size.x) / d.w;
    const f32 scene_y = (physical.y - d.y) * static_cast<f32>(_scene_size.y) / d.h;
    return {scene_x / _view_scale, scene_y / _view_scale};
}

Vec2f GpuRenderer2DBackend::logical_to_window(Vec2f logical) const {
    const Vec2i win = _window ? _window->size() : _scene_size;
    const Vec2i pixels = _window ? _window->pixel_size() : _scene_size;
    if (win.x <= 0 || win.y <= 0 || pixels.x <= 0 || pixels.y <= 0) return logical;
    if (!scene_uses_logical_coordinates()) {
        return {logical.x * win.x / pixels.x, logical.y * win.y / pixels.y};
    }
    const Rectf d = letterbox_rect(_scene_size, pixels, _integer_scale);
    // logical -> native scene pixels (* _view_scale) -> window (present blit).
    const f32 scene_x = logical.x * _view_scale;
    const f32 scene_y = logical.y * _view_scale;
    return {(d.x + scene_x * d.w / static_cast<f32>(_scene_size.x)) * win.x / pixels.x,
            (d.y + scene_y * d.h / static_cast<f32>(_scene_size.y)) * win.y / pixels.y};
}

RenderTarget GpuRenderer2DBackend::create_render_target(Vec2i size, ScaleMode mode) {
    if (size.x <= 0 || size.y <= 0) {
        throw std::runtime_error("create_render_target requires a positive size");
    }
    gpu::GpuTexture tex = _device.create_render_texture(
        static_cast<u32>(size.x),
        static_cast<u32>(size.y),
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM
    );
    return RenderTarget{Texture{std::make_shared<gpu::GpuTextureBackend>(std::move(tex), true, mode)}};
}

void GpuRenderer2DBackend::push_render_target(const RenderTarget& target) {
    const auto* backend = as_gpu(target.texture().backend().get());
    if (!backend || !backend->texture()) {
        return;
    }
    if (_frame) {
        flush_to_frame();
    }
    _saved_clip_stacks.push_back(_clip_stack);
    _clip_stack.clear();
    _saved_view_offsets.push_back(_view_offset);
    _view_offset = {0.0f, 0.0f}; // RT-internal draws use the target's native origin
    _rt_stack.push_back(&backend->texture());
    if (_frame) {
        _batch.begin(current_target(), _clear_color, /*do_clear=*/false);
    }
}

void GpuRenderer2DBackend::pop_render_target() {
    if (_rt_stack.empty()) {
        return;
    }
    if (_frame) {
        flush_to_frame();
    }
    _rt_stack.pop_back();
    if (!_saved_clip_stacks.empty()) {
        _clip_stack = std::move(_saved_clip_stacks.back());
        _saved_clip_stacks.pop_back();
    } else {
        _clip_stack.clear();
    }
    if (!_saved_view_offsets.empty()) {
        _view_offset = _saved_view_offsets.back();
        _saved_view_offsets.pop_back();
    } else {
        _view_offset = {0.0f, 0.0f};
    }
    if (_frame) {
        _batch.begin(current_target(), _clear_color, /*do_clear=*/false);
    }
}

void GpuRenderer2DBackend::set_scale_mode(const Texture& texture, ScaleMode mode) {
    // Per-texture sampling: draw_texture picks the nearest/linear sampler from this.
    if (const auto* backend = as_gpu(texture.backend().get())) {
        backend->set_scale_mode(mode);
    }
}

void GpuRenderer2DBackend::set_viewport(Rectf rect) {
    // Mirror SDL_SetRenderViewport: the viewport rect is absolute, it clips to that
    // rect, AND subsequent draw coords become viewport-relative (origin at rect.x/y).
    // Clips are kept in coordinate space (logical for the scene under logical
    // presentation, target pixels otherwise); current_scissor() scales them.
    const bool scene_logical = scene_uses_logical_coordinates();
    const Vec2i coord = scene_logical ? _logical_size : current_size();
    _clip_stack.clear();
    _clip_stack.push_back(intersect(to_sdl_rect(rect), SDL_Rect{0, 0, coord.x, coord.y}));
    _view_offset = {rect.x, rect.y};
}

void GpuRenderer2DBackend::reset_viewport() {
    _clip_stack.clear();
    _view_offset = {0.0f, 0.0f};
}

void GpuRenderer2DBackend::push_viewport(Rectf rect) {
    // Save + set both the clip (absolute viewport rect, intersected with parent) and
    // the origin offset, mirroring SdlRenderer2DBackend::push_viewport.
    const bool scene_logical = scene_uses_logical_coordinates();
    const Vec2i coord = scene_logical ? _logical_size : current_size();
    const SDL_Rect parent = _clip_stack.empty() ? SDL_Rect{0, 0, coord.x, coord.y} : _clip_stack.back();
    _view_offset_stack.push_back(_view_offset);
    _clip_stack.push_back(intersect(to_sdl_rect(rect), parent));
    _view_offset = {rect.x, rect.y};
}

void GpuRenderer2DBackend::pop_viewport() {
    if (!_clip_stack.empty()) {
        _clip_stack.pop_back();
    }
    if (!_view_offset_stack.empty()) {
        _view_offset = _view_offset_stack.back();
        _view_offset_stack.pop_back();
    } else {
        _view_offset = {0.0f, 0.0f};
    }
}

void GpuRenderer2DBackend::push_clip(Rectf rect) {
    // SDL clip rects are viewport-relative, so shift by the active viewport origin to
    // absolute coordinate space, then intersect with the parent clip (current_scissor()
    // scales the result to native texture pixels at draw time).
    const bool scene_logical = scene_uses_logical_coordinates();
    const Vec2i coord = scene_logical ? _logical_size : current_size();
    const SDL_Rect parent = _clip_stack.empty() ? SDL_Rect{0, 0, coord.x, coord.y} : _clip_stack.back();
    const Rectf shifted{rect.x + _view_offset.x, rect.y + _view_offset.y, rect.w, rect.h};
    _clip_stack.push_back(intersect(to_sdl_rect(shifted), parent));
}

void GpuRenderer2DBackend::pop_clip() {
    if (!_clip_stack.empty()) {
        _clip_stack.pop_back();
    }
}

ShaderHandle GpuRenderer2DBackend::create_shader(const ShaderDesc& desc) {
    // This backend is Vulkan/SPIR-V only; a SPIR-V blob is required (the SDL gpu
    // driver's DXIL path is a separate backend). Callers degrade on a null handle.
    if (!desc.spirv.valid()) {
        KIN_LOG_ERROR_F("render", "create_shader: GPU backend requires a SPIR-V blob",
                        (LogFields{{.name = "spirv_size", .value = std::to_string(desc.spirv.size)}}));
        return {};
    }
    if (desc.num_samplers > MaxShaderSamplers) {
        KIN_LOG_ERROR_F("render", "create_shader: too many samplers",
                        (LogFields{{.name = "samplers", .value = std::to_string(desc.num_samplers)},
                                   {.name = "max", .value = std::to_string(MaxShaderSamplers)}}));
        return {};
    }
    gpu::GpuShader shader = gpu::GpuShader::from_bytes(
        _device, SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERFORMAT_SPIRV,
        std::span<const u8>{desc.spirv.code, desc.spirv.size},
        desc.num_uniform_buffers, desc.num_samplers);
    if (!shader) {
        KIN_LOG_ERROR_F("render", "create_shader: GpuShader::from_bytes failed",
                        (LogFields{{.name = "error", .value = SDL_GetError()}}));
        return {};
    }
    _shaders.push_back(std::move(shader));
    KIN_LOG_INFO_F("render", "shader created",
                   (LogFields{{.name = "handle", .value = std::to_string(_shaders.size())}}));
    return ShaderHandle{static_cast<u64>(_shaders.size())}; // value == index + 1
}

void GpuRenderer2DBackend::draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params) {
    draw_shader_surface(rect, handle, params, std::span<const Texture>{});
}

void GpuRenderer2DBackend::draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                                               const Texture& source) {
    draw_shader_surface(rect, handle, params, std::span<const Texture>{&source, 1});
}

void GpuRenderer2DBackend::draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                                               const Texture& source0, const Texture& source1) {
    const std::array<Texture, 2> sources{source0, source1};
    draw_shader_surface(rect, handle, params, std::span<const Texture>{sources});
}

void GpuRenderer2DBackend::draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                                               std::span<const Texture> sources) {
    if (handle.value == 0 || handle.value > _shaders.size() || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const gpu::GpuShader& shader = _shaders[static_cast<std::size_t>(handle.value) - 1];
    if (!shader) {
        return;
    }
    ensure_frame();
    // `sources[i]` -> fragment sampler i. An invalid source leaves the binding null,
    // which the batch replaces with the white texture and default sampler. Slots the
    // shader declares beyond the sources are bound the same way, so it never samples
    // an unbound slot.
    const auto resolve = [this](const Texture& src) {
        SDL_GPUTextureSamplerBinding binding{};
        if (const auto* backend = as_gpu(src.backend().get());
            backend && backend->texture()) {
            binding.texture = backend->texture().handle();
            binding.sampler = backend->scale_mode() == ScaleMode::Linear ? _sampler_linear : _sampler_nearest;
        }
        return binding;
    };
    const std::size_t slots = std::min<std::size_t>(
        std::max<std::size_t>({sources.size(), shader.samplers(), 1}), MaxShaderSamplers);
    const SDL_GPUTextureSamplerBinding slot0 = sources.empty() ? SDL_GPUTextureSamplerBinding{} : resolve(sources[0]);
    std::array<SDL_GPUTextureSamplerBinding, MaxShaderSamplers - 1> extra{};
    for (std::size_t i = 1; i < slots && i < sources.size(); ++i) {
        extra[i - 1] = resolve(sources[i]);
    }
    // Quad over `rect` (uv 0..1, white vertex color) tagged with the material fragment
    // shader + the ShaderParams uniform (fragment slot 0).
    const f32 x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.w, y1 = rect.y + rect.h;
    std::array<gpu::GpuVertex, 6> verts{{
        {x0, y0, 0.0f, 0.0f, 255, 255, 255, 255},
        {x1, y0, 1.0f, 0.0f, 255, 255, 255, 255},
        {x1, y1, 1.0f, 1.0f, 255, 255, 255, 255},
        {x0, y0, 0.0f, 0.0f, 255, 255, 255, 255},
        {x1, y1, 1.0f, 1.0f, 255, 255, 255, 255},
        {x0, y1, 0.0f, 1.0f, 255, 255, 255, 255},
    }};
    apply_view_offset(verts);
    _batch.push(verts, shader.handle(), slot0.texture, current_scissor(), resolve_blend(gpu::GpuBlendMode::Alpha),
                params.uniforms.data(), static_cast<u32>(params.uniforms.size() * sizeof(f32)),
                slot0.sampler, std::span<const SDL_GPUTextureSamplerBinding>{extra.data(), slots - 1});
}

void GpuRenderer2DBackend::set_post_process(std::span<const PostProcessPass> passes) {
    _post_passes.assign(passes.begin(), passes.end());
}

const gpu::GpuTexture* GpuRenderer2DBackend::run_post_chain() {
    if (_post_passes.empty() || !_frame || !_scene) {
        return &_scene;
    }
    // Scratch ping-pong targets sized to the scene (native resolution).
    if (!_post_a || _post_size != _scene_size) {
        _device.wait_idle();
        _post_a = _device.create_render_texture(static_cast<u32>(_scene_size.x),
                                                static_cast<u32>(_scene_size.y),
                                                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        _post_b = _device.create_render_texture(static_cast<u32>(_scene_size.x),
                                                static_cast<u32>(_scene_size.y),
                                                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
        _post_size = _scene_size;
    }

    const f32 w = static_cast<f32>(_scene_size.x);
    const f32 h = static_cast<f32>(_scene_size.y);
    // Fullscreen quad in scratch-pixel space (0..size); the same vertex shader + Y-flip
    // as the scene render keeps sampling orientation consistent (top->top).
    std::array<gpu::GpuVertex, 6> verts{{
        {0.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
        {w, 0.0f, 1.0f, 0.0f, 255, 255, 255, 255},
        {w, h, 1.0f, 1.0f, 255, 255, 255, 255},
        {0.0f, 0.0f, 0.0f, 0.0f, 255, 255, 255, 255},
        {w, h, 1.0f, 1.0f, 255, 255, 255, 255},
        {0.0f, h, 0.0f, 1.0f, 255, 255, 255, 255},
    }};
    const SDL_Rect full{0, 0, _scene_size.x, _scene_size.y};

    const gpu::GpuTexture* read = &_scene;
    gpu::GpuTexture* write = &_post_a;
    for (const PostProcessPass& pass : _post_passes) {
        if (pass.shader.value == 0 || pass.shader.value > _shaders.size()) {
            continue; // skip invalid pass (leaves `read` unchanged)
        }
        const gpu::GpuShader& sh = _shaders[static_cast<std::size_t>(pass.shader.value) - 1];
        if (!sh) {
            continue;
        }
        _batch.begin(*write, SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f}, /*do_clear=*/true);
        // Slot 1 is the original scene when requested; any other declared slots get
        // the white texture so the pass never samples an unbound slot.
        std::array<SDL_GPUTextureSamplerBinding, MaxShaderSamplers - 1> extra{};
        if (pass.sample_original) {
            extra[0] = {_scene.handle(), _sampler_linear};
        }
        const std::size_t extra_count = std::min<std::size_t>(
            std::max<std::size_t>(sh.samplers(), pass.sample_original ? 2u : 1u) - 1, extra.size());
        _batch.push(verts, sh.handle(), read->handle(), full, gpu::GpuBlendMode::Replace,
                    pass.params.uniforms.data(),
                    static_cast<u32>(pass.params.uniforms.size() * sizeof(f32)),
                    _sampler_linear, std::span<const SDL_GPUTextureSamplerBinding>{extra.data(), extra_count});

        gpu::GpuGeometryBatch::FlushContext ctx{};
        ctx.vertex_shader = _vertex_shader.handle();
        ctx.default_fragment = _fragment_shader.handle();
        ctx.white_texture = _white.handle();
        ctx.sampler = _sampler_linear;
        ctx.target_format = write->format();
        ctx.view.scale[0] = 2.0f / std::max(1.0f, w);
        ctx.view.scale[1] = 2.0f / std::max(1.0f, h);
        ctx.view.translate[0] = -1.0f;
        ctx.view.translate[1] = -1.0f;
        _batch.flush(*_frame, _device, _pipelines, ctx);

        read = write;
        write = (write == &_post_a) ? &_post_b : &_post_a;
    }
    return read;
}

} // namespace kin
