#include <kin/core/jobs.hpp>
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
#include <sstream>
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
    try {
        _instance_shader = gpu::GpuShader::from_file(_device, SDL_GPU_SHADERSTAGE_VERTEX,
                                                     SDL_GPU_SHADERFORMAT_SPIRV,
                                                     dir / "sprite_instanced.vert.spv",
                                                     /*uniform_buffers=*/1, /*samplers=*/0);
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "instanced sprites unavailable; drawing them quad by quad",
                       (LogFields{{.name = "reason", .value = e.what()}}));
    }
    try {
        _shader_vertex_shader = gpu::GpuShader::from_file(_device, SDL_GPU_SHADERSTAGE_VERTEX,
                                                          SDL_GPU_SHADERFORMAT_SPIRV,
                                                          dir / "shader_geometry.vert.spv",
                                                          /*uniform_buffers=*/1, /*samplers=*/0);
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "shader geometry unavailable",
                       (LogFields{{.name = "reason", .value = e.what()}}));
    }
    try {
        _overdraw_count_shader = gpu::GpuShader::from_file(_device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                           SDL_GPU_SHADERFORMAT_SPIRV, dir / "overdraw_count.frag.spv",
                                                           /*uniform_buffers=*/0, /*samplers=*/1);
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "overdraw view unavailable", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    try {
        _shape_shader = gpu::GpuShader::from_file(_device, SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERFORMAT_SPIRV,
                                                  dir / "shape.frag.spv", /*uniform_buffers=*/0, /*samplers=*/1);
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "shapes drawn with soft edges only", (LogFields{{.name = "reason", .value = e.what()}}));
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
    sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
    _sampler_linear = SDL_CreateGPUSampler(_device.handle(), &sampler_info);
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler_info.max_lod = 1000.0f; // every level
    _sampler_mipmapped = SDL_CreateGPUSampler(_device.handle(), &sampler_info);
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.max_lod = 0.0f;
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    _sampler_nearest = SDL_CreateGPUSampler(_device.handle(), &sampler_info);
    _data_textures = true;
    for (const SDL_GPUTextureFormat format :
         {SDL_GPU_TEXTUREFORMAT_R16_UINT, SDL_GPU_TEXTUREFORMAT_R16G16_UINT, SDL_GPU_TEXTUREFORMAT_R32_FLOAT}) {
        _data_textures = _data_textures && SDL_GPUTextureSupportsFormat(_device.handle(), format, SDL_GPU_TEXTURETYPE_2D,
                                                                        SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }

    const std::array<u8, 4> white{255, 255, 255, 255};
    _white = _device.create_texture_from_rgba(white.data(), 1, 1);

    _pipelines.init(_device.handle());
    if (_shape_shader.handle() && _shader_vertex_shader.handle()) {
        // Shapes' pipeline now rather than at the first one drawn.
        _pipelines.get(_shader_vertex_shader.handle(), _shape_shader.handle(), gpu::GpuBlendMode::Alpha,
                       SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, gpu::GpuVertexLayout::ShaderVertices);
    }
    ensure_scene();

    KIN_LOG_INFO_F("render", "gpu backend created",
                   (LogFields{{.name = "driver", .value = std::string{_device.driver_name()}},
                              {.name = "vsync", .value = vsync ? "true" : "false"}}));
}

GpuRenderer2DBackend::~GpuRenderer2DBackend() {
    if (_frame) {
        _frame->submit();
        end_frame();
    }
    _device.wait_idle();
    _gpu_timer.reset();
    _pipelines.destroy();
    for (const ComputePipeline& compute : _compute_pipelines) {
        SDL_ReleaseGPUComputePipeline(_device.handle(), compute.pipeline);
    }
    if (_sampler_linear) {
        SDL_ReleaseGPUSampler(_device.handle(), _sampler_linear);
    }
    if (_sampler_mipmapped) {
        SDL_ReleaseGPUSampler(_device.handle(), _sampler_mipmapped);
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
        .min_max_blend = true,
        .shader_geometry = static_cast<bool>(_shader_vertex_shader.handle()),
        .data_buffers = true,
        .compute = true,
        .transforms = true,
        .shapes = true,
        .materials_2d = true, // G3: real SPIR-V fragment-shader materials
        .gradients = true,
        .text = false,
        .rendering_3d = false,
        .data_textures = _data_textures,
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
    return _rt_stack.empty() ? _scene : *_rt_stack.back().texture;
}

f32 GpuRenderer2DBackend::coordinates_to_pixels() const {
    return static_cast<f32>(current_size().x) / std::max(1.0f, coordinate_size().x);
}

Vec2i GpuRenderer2DBackend::coordinate_extent() const {
    const Vec2f size = coordinate_size();
    return {static_cast<i32>(std::ceil(size.x)), static_cast<i32>(std::ceil(size.y))};
}

Vec2f GpuRenderer2DBackend::coordinate_size() const {
    if (!_rt_stack.empty() && _rt_stack.back().coords.x > 0.0f) {
        return _rt_stack.back().coords;
    }
    if (scene_uses_logical_coordinates()) {
        return {static_cast<f32>(_logical_size.x), static_cast<f32>(_logical_size.y)};
    }
    const Vec2i size = current_size();
    return {static_cast<f32>(size.x), static_cast<f32>(size.y)};
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

void GpuRenderer2DBackend::end_frame() {
    _frame.reset();
    _retained.clear();
    _retained_buffers.clear();
    _retired_textures.clear();
    _last_retained = nullptr;
    ++_frame_serial;
}

void GpuRenderer2DBackend::retain(const Texture& texture) {
    const ITextureBackend* backend = texture.backend().get();
    if (backend != _last_retained) {
        _retained.push_back(texture.backend());
        _last_retained = backend;
        if (const auto* gpu = as_gpu(backend)) {
            gpu->mark_used(_frame_serial);
        }
    }
}

void GpuRenderer2DBackend::flush_to_frame() {
    // Draw coordinates are in "coordinate space": logical for the scene under logical
    // presentation, else the target's texture size. The uniform maps coord -> NDC so
    // logical coords fill the (native-res) scene texture; the rasterizer then renders
    // at native res. The batch sets the GPU viewport to the target's texture size.
    const Vec2f coord = coordinate_size();
    gpu::GpuGeometryBatch::FlushContext ctx{};
    ctx.vertex_shader = _vertex_shader.handle();
    ctx.instance_shader = _instance_shader.handle();
    ctx.shader_vertex_shader = _shader_vertex_shader.handle();
    ctx.default_fragment = _fragment_shader.handle();
    ctx.white_texture = _white.handle();
    ctx.sampler = _sampler_linear; // default for solids/white & null-sampler ranges
    ctx.target_format = current_target().format();
    if (_overdraw_view) {
        ctx.override_fragment = _overdraw_count_shader.handle();
    }
    ctx.view.scale[0] = 2.0f / std::max(1.0f, coord.x);
    ctx.view.scale[1] = 2.0f / std::max(1.0f, coord.y);
    ctx.view.translate[0] = -1.0f;
    ctx.view.translate[1] = -1.0f;
    _batch.flush(*_frame, _device, _pipelines, ctx);
}

void GpuRenderer2DBackend::clear(Color color) {
    ensure_scene();
    // The overdraw view counts from nothing.
    _clear_color = _overdraw_view && _overdraw_count_shader ? SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f} : to_fcolor(color);
    if (!_frame) {
        _frame.emplace(_device.begin_frame());
    } else {
        flush_to_frame(); // unusual mid-frame clear: emit prior content first
    }
    _batch.begin(current_target(), _clear_color, /*do_clear=*/true);
    _clip_stack.clear();
}

SDL_GPUFence* GpuRenderer2DBackend::submit_for_scope() {
    ensure_frame();
    flush_to_frame();
    SDL_GPUFence* fence = _frame->submit_with_fence();
    end_frame();
    return fence;
}

std::string GpuRenderer2DBackend::pipeline_record() const {
    // One line a pipeline: fragment id, blend, target format, vertex layout.
    std::string out = "kin.pipelines/1\n";
    _pipelines.for_each([&](SDL_GPUShader*, SDL_GPUShader* fragment, gpu::GpuBlendMode blend,
                            SDL_GPUTextureFormat format, gpu::GpuVertexLayout layout) {
        u64 id = 0;
        if (fragment != _fragment_shader.handle()) {
            const auto it = _fragment_ids.find(fragment);
            if (it == _fragment_ids.end()) {
                return; // a shader made some other way: not known next run
            }
            id = it->second;
        }
        out += std::to_string(id) + ' ' + std::to_string(static_cast<int>(blend)) + ' ' +
               std::to_string(static_cast<int>(format)) + ' ' + std::to_string(static_cast<int>(layout)) + '\n';
    });
    return out;
}

void GpuRenderer2DBackend::prewarm_pipelines(std::string_view record) {
    if (!record.starts_with("kin.pipelines/1\n")) {
        return; // none, or from another version
    }
    std::istringstream in{std::string{record.substr(16)}};
    u64 id = 0;
    int blend = 0, format = 0, layout = 0;
    while (in >> id >> blend >> format >> layout) {
        if (blend < 0 || blend > static_cast<int>(gpu::GpuBlendMode::Min) || layout < 0 ||
            layout > static_cast<int>(gpu::GpuVertexLayout::ShaderVertices)) {
            continue;
        }
        _pipeline_hints.push_back(PipelineHint{.fragment = id,
                                               .blend = static_cast<gpu::GpuBlendMode>(blend),
                                               .format = static_cast<SDL_GPUTextureFormat>(format),
                                               .layout = static_cast<gpu::GpuVertexLayout>(layout)});
    }
    make_hinted_pipelines(0, _fragment_shader.handle()); // the engine's own, now
    for (const auto& [fragment, fragment_id] : _fragment_ids) {
        make_hinted_pipelines(fragment_id, fragment); // shaders already made
    }
}

void GpuRenderer2DBackend::make_hinted_pipelines(u64 fragment_id, SDL_GPUShader* fragment) {
    for (const PipelineHint& hint : _pipeline_hints) {
        if (hint.fragment != fragment_id) {
            continue;
        }
        SDL_GPUShader* vertex = hint.layout == gpu::GpuVertexLayout::SpriteInstances ? _instance_shader.handle()
                              : hint.layout == gpu::GpuVertexLayout::ShaderVertices  ? _shader_vertex_shader.handle()
                                                                                     : _vertex_shader.handle();
        if (vertex) {
            _pipelines.get(vertex, fragment, hint.blend, hint.format, hint.layout);
        }
    }
}

void GpuRenderer2DBackend::begin_gpu_scope(std::string_view name) {
    if (!_gpu_timer || _scope) {
        return; // timing off, or inside a scope already
    }
    _scope = std::string{name};
    // Room for both its fences, or it goes untimed (a fence must never be
    // dropped unsignalled). Only the render thread adds fences, and present
    // ends the scope before adding the frame's.
    _scope_timed = !_gpu_timer->full(2);
    if (_scope_timed) {
        _gpu_timer->track_scope_start(submit_for_scope());
        _scope_pixels = _batch.pixels(); // flushed: everything before is counted
    }
}

void GpuRenderer2DBackend::end_gpu_scope() {
    if (!_scope) {
        return;
    }
    std::string name = std::move(*_scope);
    _scope.reset();
    if (!_scope_timed || !_gpu_timer) {
        return;
    }
    const u64 submit_ns = SDL_GetTicksNS();
    SDL_GPUFence* fence = submit_for_scope(); // flushes the scope's draws, counting their pixels
    _gpu_timer->track_scope_end(std::move(name), fence, submit_ns, _batch.pixels() - _scope_pixels);
}

void GpuRenderer2DBackend::set_gpu_timing_enabled(bool enabled) {
    if (enabled && !_gpu_timer) {
        _gpu_timer = std::make_unique<gpu::GpuFrameTimer>(_device.handle());
    } else if (!enabled && _gpu_timer) {
        _scope.reset(); // its start fence goes with the timer
        _gpu_timer.reset();
        _stats.last_gpu_frame_ms = 0.0;
        _untimed_frames = 0;
    }
}

void GpuRenderer2DBackend::present() {
    const auto ms_between = [](u64 start_ns, u64 end_ns) {
        return static_cast<f64>(end_ns - start_ns) / 1'000'000.0;
    };
    end_gpu_scope(); // a scope ends with its frame, before the frame's fence

    const u64 flush_start = SDL_GetTicksNS();
    ensure_frame();
    flush_to_frame();
    // Full-scene post-processing: run the chain over the scene texture (ping-pong
    // scratch RTs); the result (scene-sized) is what gets blitted to the swapchain.
    const gpu::GpuTexture* presented = run_post_chain();
    const u64 acquire_start = SDL_GetTicksNS();
    const bool acquired = _frame->acquire_swapchain();
    const u64 acquire_end = SDL_GetTicksNS();
    _stats.last_present_flush_ms = ms_between(flush_start, acquire_start);
    _stats.last_gpu_wait_ms = ms_between(acquire_start, acquire_end);
    if (acquired && _frame->swapchain_texture()) {
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
    // Only the render thread adds frames to the timer, so it cannot fill up
    // between the check and track().
    if (_gpu_timer && !_gpu_timer->full()) {
        SDL_GPUFence* fence = _frame->submit_with_fence();
        _gpu_timer->track(fence, _device.take_first_submit_ns(), std::exchange(_untimed_frames, 0));
    } else {
        _frame->submit();
        _device.take_first_submit_ns();
        if (_gpu_timer) {
            ++_untimed_frames;
        }
    }
    if (_gpu_timer) {
        gpu::GpuTimerSamples samples = _gpu_timer->collect();
        if (const std::optional<gpu::GpuFrameSample>& sample = samples.frame) {
            // A span over untimed frames is shared out evenly.
            _stats.last_gpu_frame_ms = sample->ms / static_cast<f64>(sample->frames);
            _stats.last_gpu_frame_span = sample->frames;
            ++_stats.gpu_frames_sampled;
        }
        const f64 screen = static_cast<f64>(_scene_size.x) * _scene_size.y;
        for (gpu::GpuScopeSample& scope : samples.scopes) {
            _scope_timings.push_back(GpuScopeTiming{.name = std::move(scope.name), .ms = scope.ms,
                                                    .pixels = scope.pixels,
                                                    .overdraw = screen > 0.0 ? scope.pixels / screen : 0.0});
        }
    }
    end_frame();
    // In the targets' own pixels, over the screen's: a scene drawn in logical
    // coordinates is shaded at its native size.
    _stats.last_pixels_drawn = _batch.take_pixels();
    _stats.last_overdraw = _scene_size.x > 0 && _scene_size.y > 0
                               ? _stats.last_pixels_drawn / (static_cast<f64>(_scene_size.x) * _scene_size.y)
                               : 0.0;
    _stats.last_present_backend_ms = ms_between(acquire_start, SDL_GetTicksNS());
}

SDL_Rect GpuRenderer2DBackend::current_scissor() const {
    // Clips are stored in coordinate space (logical for the scene under logical
    // presentation, target pixels otherwise). The GPU scissor needs native texture
    // pixels, so scale by the coord->texture factor (a logical scene's or a layer's;
    // 1 for plain render targets). Asked once a draw, it is kept until what it is
    // made from changes.
    const Vec2i tex = current_size();
    const f32 sc = coordinates_to_pixels();
    const SDL_Rect top = _clip_stack.empty() ? SDL_Rect{0, 0, -1, -1} : _clip_stack.back();
    ScissorKey key{top, sc, tex};
    if (_scissor_cache && _scissor_cache->first == key) {
        return _scissor_cache->second;
    }
    SDL_Rect c;
    if (!_clip_stack.empty()) {
        const SDL_Rect& r = _clip_stack.back();
        c = {static_cast<int>(std::floor(r.x * sc)), static_cast<int>(std::floor(r.y * sc)),
             static_cast<int>(std::ceil(r.w * sc)), static_cast<int>(std::ceil(r.h * sc))};
    } else {
        c = {0, 0, tex.x, tex.y};
    }
    const SDL_Rect scissor = intersect(c, SDL_Rect{0, 0, tex.x, tex.y});
    _scissor_cache.emplace(key, scissor);
    return scissor;
}

void GpuRenderer2DBackend::place(std::span<gpu::GpuVertex> verts) const {
    if (_transformed) {
        for (gpu::GpuVertex& v : verts) {
            const Vec2f p = place(Vec2f{v.x, v.y});
            v.x = p.x;
            v.y = p.y;
        }
        return;
    }
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
    case BlendMode::Max: return gpu::GpuBlendMode::Max;
    case BlendMode::Min: return gpu::GpuBlendMode::Min;
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
    place(tris); // no-op without a transform or viewport offset
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
    const f32 u0 = uv.x;
    const f32 v0 = uv.y;
    const f32 u1 = uv.x + uv.w;
    const f32 v1 = uv.y + uv.h;
    const u8 r = color.r, g = color.g, b = color.b, a = color.a;
    if (_transformed) {
        // One corner mapped, the others along the mapped edges.
        const Vec2f p = place(Vec2f{dest.x, dest.y});
        const Vec2f ex = _transform.apply_vector({dest.w, 0.0f});
        const Vec2f ey = _transform.apply_vector({0.0f, dest.h});
        std::array<gpu::GpuVertex, 4> corners{{
            {p.x, p.y, u0, v0, r, g, b, a},
            {p.x + ex.x, p.y + ex.y, u1, v0, r, g, b, a},
            {p.x + ex.x + ey.x, p.y + ex.y + ey.y, u1, v1, r, g, b, a},
            {p.x + ey.x, p.y + ey.y, u0, v1, r, g, b, a},
        }};
        _batch.push_quad(corners, texture, current_scissor(), resolve_blend(blend), sampler);
        return;
    }
    const f32 x0 = dest.x + _view_offset.x;
    const f32 y0 = dest.y + _view_offset.y;
    const f32 x1 = x0 + dest.w;
    const f32 y1 = y0 + dest.h;
    std::array<gpu::GpuVertex, 4> corners{{
        {x0, y0, u0, v0, r, g, b, a},
        {x1, y0, u1, v0, r, g, b, a},
        {x1, y1, u1, v1, r, g, b, a},
        {x0, y1, u0, v1, r, g, b, a},
    }};
    _batch.push_quad(corners, texture, current_scissor(), resolve_blend(blend), sampler);
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
    std::array<gpu::GpuVertex, 4> corners{{
        {a.x + nx, a.y + ny, 0.0f, 0.0f, r, g, bl, al},
        {b.x + nx, b.y + ny, 1.0f, 0.0f, r, g, bl, al},
        {b.x - nx, b.y - ny, 1.0f, 1.0f, r, g, bl, al},
        {a.x - nx, a.y - ny, 0.0f, 1.0f, r, g, bl, al},
    }};
    ensure_frame();
    place(corners);
    _batch.push_quads(corners, nullptr, nullptr, current_scissor(), resolve_blend(gpu::GpuBlendMode::Alpha));
}

void GpuRenderer2DBackend::draw_shape_mesh(std::span<const ShapeVertex> vertices, std::span<const u32> indices,
                                           Color tint) {
    if (indices.empty() || tint.a == 0) {
        return;
    }
    ensure_frame();
    const auto shade = [&](Color c, f32 cover) {
        return Color{static_cast<u8>((c.r * tint.r + 127) / 255), static_cast<u8>((c.g * tint.g + 127) / 255),
                     static_cast<u8>((c.b * tint.b + 127) / 255),
                     static_cast<u8>(static_cast<f32>((c.a * tint.a + 127) / 255) * cover + 0.5f)};
    };
    if (!_shape_shader.handle() || !_shader_vertex_shader.handle()) {
        // Without the shape shader: the soft edge as vertex alpha, in pixels
        // from the transform's scale.
        // A soft edge wider than a pixel is pulled in to one, along `outward`.
        const f32 pixels_per_unit = coordinates_to_pixels() * std::sqrt(std::abs(_transform.determinant()));
        const f32 rim = -1.0f / std::max(pixels_per_unit, 1e-6f);
        _scratch_verts.clear();
        for (const u32 i : indices) {
            const ShapeVertex& v = vertices[i];
            Vec2f p = v.position;
            f32 edge = v.edge;
            if (edge < rim) {
                p = {p.x + v.outward.x * (edge - rim), p.y + v.outward.y * (edge - rim)};
                edge = rim;
            }
            _scratch_verts.push_back(gv(p, shade(v.color, std::clamp(1.0f + edge * pixels_per_unit, 0.0f, 1.0f))));
        }
        place(_scratch_verts);
        _batch.push(_scratch_verts, nullptr, nullptr, current_scissor(), resolve_blend(gpu::GpuBlendMode::Alpha));
        return;
    }
    _shader_vertex_scratch.clear();
    _shader_vertex_scratch.reserve(indices.size());
    for (const u32 i : indices) {
        const ShapeVertex& v = vertices[i];
        const Vec2f p = place(v.position);
        const Color c = shade(v.color, 1.0f);
        gpu::GpuShaderVertex out{};
        out.x = p.x;
        out.y = p.y;
        out.r = c.r;
        out.g = c.g;
        out.b = c.b;
        out.a = c.a;
        out.custom[0] = v.edge;
        _shader_vertex_scratch.push_back(out);
    }
    _batch.push_shader_vertices(_shader_vertex_scratch, _shape_shader.handle(), nullptr, current_scissor(),
                                resolve_blend(gpu::GpuBlendMode::Alpha), nullptr, 0, nullptr, {}, {});
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

namespace {

// A shader known across runs (pipeline records): FNV-1a of its SPIR-V, never 0
// (the default shader's).
u64 spirv_id(ShaderBlob spirv) {
    u64 id = 1469598103934665603ull;
    for (u32 i = 0; i < spirv.size; ++i) {
        id = (id ^ spirv.code[i]) * 1099511628211ull;
    }
    return id | 1;
}

SDL_GPUTextureFormat sdl_format(TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgba8: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case TextureFormat::R16Uint: return SDL_GPU_TEXTUREFORMAT_R16_UINT;
    case TextureFormat::Rg16Uint: return SDL_GPU_TEXTUREFORMAT_R16G16_UINT;
    case TextureFormat::R32Float: return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

} // namespace

Texture GpuRenderer2DBackend::create_texture(Vec2i size, TextureFormat format, const void* pixels) {
    if (size.x <= 0 || size.y <= 0 || (format != TextureFormat::Rgba8 && !_data_textures)) {
        return {};
    }
    gpu::GpuTexture tex = _device.create_texture(pixels, static_cast<u32>(size.x), static_cast<u32>(size.y),
                                                 sdl_format(format), texture_format_bytes(format));
    return Texture{std::make_shared<gpu::GpuTextureBackend>(std::move(tex), false, ScaleMode::Nearest, format)};
}

bool GpuRenderer2DBackend::update_texture(const Texture& texture, Vec2i at, Vec2i size, const u8* pixels) {
    const auto* backend = as_gpu(texture.backend().get());
    if (!backend || !backend->texture()) {
        return false;
    }
    _device.update_texture(backend->texture().handle(), static_cast<u32>(at.x), static_cast<u32>(at.y),
                           static_cast<u32>(size.x), static_cast<u32>(size.y), pixels,
                           texture_format_bytes(backend->format()), may_cycle(*backend, at, size));
    if (backend->mipmapped()) {
        _device.generate_mipmaps(backend->texture().handle());
    }
    return true;
}

bool GpuRenderer2DBackend::write_texture(const Texture& texture, Vec2i at, Vec2i size, std::size_t /*bytes*/,
                                         const std::function<void(std::span<u8>)>& fill) {
    const auto* backend = as_gpu(texture.backend().get());
    if (!backend || !backend->texture()) {
        return false;
    }
    _device.write_texture(backend->texture().handle(), static_cast<u32>(at.x), static_cast<u32>(at.y),
                          static_cast<u32>(size.x), static_cast<u32>(size.y), texture_format_bytes(backend->format()),
                          may_cycle(*backend, at, size), fill);
    if (backend->mipmapped()) {
        _device.generate_mipmaps(backend->texture().handle());
    }
    return true;
}

bool GpuRenderer2DBackend::may_cycle(const gpu::GpuTextureBackend& texture, Vec2i at, Vec2i size) const {
    // Replacing the whole texture lets its storage be cycled, so the upload does
    // not wait for earlier frames still reading it. Not when this frame already
    // draws it: its queued draws bind the texture only when flushed, and must see
    // the update as they would without cycling.
    return at == Vec2i{0, 0} && size == texture.size() && texture.used_in_frame() != _frame_serial;
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
        sampler_for(*backend);
    retain(texture);
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
    std::array<gpu::GpuVertex, 4> corners{{
        {p0.x, p0.y, u0, v0, r, g, b, a},
        {p1.x, p1.y, u1, v0, r, g, b, a},
        {p2.x, p2.y, u1, v1, r, g, b, a},
        {p3.x, p3.y, u0, v1, r, g, b, a},
    }};
    place(corners);
    ensure_frame();
    const gpu::GpuBlendMode blend = backend->premultiplied() ? gpu::GpuBlendMode::Premultiplied
                                                             : gpu::GpuBlendMode::Alpha;
    SDL_GPUSampler* sampler =
        sampler_for(*backend);
    retain(texture);
    _batch.push_quads(corners, nullptr, backend->texture().handle(), current_scissor(), resolve_blend(blend), nullptr,
                      0, sampler);
}

void GpuRenderer2DBackend::draw_sprites(const Texture& texture, std::span<const SpriteInstance> sprites) {
    const auto* backend = as_gpu(texture.backend().get());
    if (!backend || !_instance_shader.handle()) {
        IRenderer2DBackend::draw_sprites(texture, sprites);
        return;
    }
    const Vec2i size = backend->size();
    if (size.x <= 0 || size.y <= 0) {
        return;
    }
    // Under a transform that turns and scales evenly a sprite is still a sprite:
    // its pivot moves, its size scales and its angle adds up. Anything else
    // (mirrored, sheared, squashed when turned) is drawn quad by quad.
    if (_transformed && !_transform.is_similarity()) {
        IRenderer2DBackend::draw_sprites(texture, sprites);
        return;
    }
    const bool transformed = _transformed;
    const Affine2 transform = _transform;
    const f32 scale = transformed ? transform.uniform_scale() : 1.0f;
    const f32 angle = transformed ? transform.rotation_degrees() : 0.0f;
    // The same corners, texture coordinates and colours draw_texture() computes per
    // quad; the vertex shader expands each instance.
    const f32 tex_w = static_cast<f32>(size.x);
    const f32 tex_h = static_cast<f32>(size.y);
    constexpr f32 pi = 3.14159265358979323846f;
    // Each instance depends only on its sprite, so large batches are filled in
    // parallel chunks; a zero-sized sprite leaves a zero-width instance, removed
    // afterwards in order.
    const Vec2f offset = _view_offset;
    const auto fill = [&](std::size_t begin, std::size_t end) {
        bool dropped = false;
        // Sprites in a batch often share an angle (all of them, under a camera).
        f32 last_rotation = 0.0f, last_cos = 1.0f, last_sin = 0.0f;
        for (std::size_t i = begin; i < end; ++i) {
            const SpriteInstance& sprite = sprites[i];
            gpu::GpuSpriteInstance& out = _instance_scratch[i];
            Rectf dest = sprite.dest;
            if (dest.w <= 0.0f || dest.h <= 0.0f) {
                out = {};
                out.w = 0.0f;
                dropped = true;
                continue;
            }
            f32 rotation = sprite.rotation;
            if (transformed) {
                const Vec2f p = transform.apply({dest.x + dest.w * sprite.pivot.x, dest.y + dest.h * sprite.pivot.y});
                dest = {p.x - dest.w * scale * sprite.pivot.x, p.y - dest.h * scale * sprite.pivot.y, dest.w * scale,
                        dest.h * scale};
                rotation += angle;
            }
            const Rectf source =
                sprite.source.w > 0.0f && sprite.source.h > 0.0f ? sprite.source : Rectf{0.0f, 0.0f, tex_w, tex_h};
            out.x = dest.x + offset.x;
            out.y = dest.y + offset.y;
            out.w = dest.w;
            out.h = dest.h;
            out.r = sprite.tint.r;
            out.g = sprite.tint.g;
            out.b = sprite.tint.b;
            out.a = sprite.tint.a;
            out.u0 = source.x / tex_w;
            out.v0 = source.y / tex_h;
            if (rotation == 0.0f) {
                out.u1 = out.u0 + source.w / tex_w;
                out.v1 = out.v0 + source.h / tex_h;
                out.pivot_x = 0.0f;
                out.pivot_y = 0.0f;
                out.cos = 1.0f;
                out.sin = 0.0f;
            } else {
                out.u1 = (source.x + source.w) / tex_w;
                out.v1 = (source.y + source.h) / tex_h;
                if (rotation != last_rotation) {
                    const f32 radians = rotation * pi / 180.0f;
                    last_rotation = rotation;
                    last_cos = std::cos(radians);
                    last_sin = std::sin(radians);
                }
                out.cos = last_cos;
                out.sin = last_sin;
                out.pivot_x = dest.x + dest.w * sprite.pivot.x + offset.x;
                out.pivot_y = dest.y + dest.h * sprite.pivot.y + offset.y;
            }
        }
        return dropped;
    };
    _instance_scratch.resize(sprites.size());
    bool dropped = false;
    constexpr std::size_t parallel_min = 16384, chunk = 8192;
    if (_jobs && sprites.size() >= parallel_min) {
        const i32 chunks = static_cast<i32>((sprites.size() + chunk - 1) / chunk);
        std::vector<char> chunk_dropped(static_cast<std::size_t>(chunks), 0);
        _jobs->parallel_for(chunks, [&](i32 c) {
            const std::size_t begin = static_cast<std::size_t>(c) * chunk;
            chunk_dropped[static_cast<std::size_t>(c)] = fill(begin, std::min(sprites.size(), begin + chunk)) ? 1 : 0;
        });
        dropped = std::find(chunk_dropped.begin(), chunk_dropped.end(), 1) != chunk_dropped.end();
    } else {
        dropped = fill(0, sprites.size());
    }
    if (dropped) {
        std::erase_if(_instance_scratch, [](const gpu::GpuSpriteInstance& instance) { return instance.w <= 0.0f; });
    }
    if (_instance_scratch.empty()) {
        return;
    }
    ensure_frame();
    const gpu::GpuBlendMode blend = backend->premultiplied() ? gpu::GpuBlendMode::Premultiplied
                                                             : gpu::GpuBlendMode::Alpha;
    SDL_GPUSampler* sampler =
        sampler_for(*backend);
    retain(texture);
    _batch.push_instances(_instance_scratch, backend->texture().handle(), current_scissor(), resolve_blend(blend),
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
        end_frame();
    } else if (scene_target && _scene && (!_post_passes.empty() || (_overdraw_view && _overdraw_count_shader))) {
        _frame.emplace(_device.begin_frame());
        src = run_post_chain();
        src_size = {static_cast<i32>(src->width()), static_cast<i32>(src->height())};
        _frame->submit();
        end_frame();
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
    end_frame();

    std::vector<u8> full;
    if (!_device.read_texture_rgba(target, full)) {
        return false;
    }
    const int sw = target_size.x;
    const int sh = target_size.y;
    // Under logical presentation the scene texture is native-res; scale the
    // logical-space region up to native pixels before reading.
    const f32 sc = coordinates_to_pixels();
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
    const f32 sc = coordinates_to_pixels();
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
    const f32 sc = coordinates_to_pixels();
    return {static_cast<i32>(region.w * sc + 0.5f), static_cast<i32>(region.h * sc + 0.5f)};
}

void GpuRenderer2DBackend::set_logical_size(Vec2i size) {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        end_frame();
    }
    _logical_size = size; // scene renders at this size; present letterboxes to the window
    _integer_scale = false;
    ensure_scene();
}

void GpuRenderer2DBackend::set_integer_logical_size(Vec2i size) {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        end_frame();
    }
    _logical_size = size;
    _integer_scale = true;
    ensure_scene();
}

void GpuRenderer2DBackend::push_native_coordinates() {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        end_frame();
    }

    _native_stack.push_back(NativeState{
        .logical_size = _logical_size,
        .integer_scale = _integer_scale,
        .view_scale = _view_scale,
        .clip_stack = _clip_stack,
        .view_offset = _view_offset,
        .view_offset_stack = _view_offset_stack,
        .transform = _transform,
    });

    _clip_stack.clear();
    _view_offset = {0.0f, 0.0f};
    _view_offset_stack.clear();
    set_transform({});
}

void GpuRenderer2DBackend::pop_native_coordinates() {
    if (_frame) {
        flush_to_frame();
        _frame->submit();
        end_frame();
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
        set_transform(state.transform);
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
    retain(target.texture());
    if (_frame) {
        flush_to_frame();
    }
    _saved_clip_stacks.push_back(_clip_stack);
    _clip_stack.clear();
    _saved_view_offsets.push_back(_view_offset);
    _view_offset = {0.0f, 0.0f}; // RT-internal draws use the target's native origin
    _saved_transforms.push_back(_transform);
    set_transform({});
    _rt_stack.push_back(TargetEntry{.texture = &backend->texture()});
    if (_frame) {
        _batch.begin(current_target(), _clear_color, /*do_clear=*/false);
    }
}

bool GpuRenderer2DBackend::push_layer_target(const RenderTarget& target) {
    const auto* backend = as_gpu(target.texture().backend().get());
    if (!backend || !backend->texture()) {
        return false;
    }
    retain(target.texture());
    const Vec2f coords = coordinate_size(); // the layer keeps these, at its own resolution
    ensure_frame();
    flush_to_frame();
    // Clips, the viewport offset and the transform stay as they are: draws in
    // the layer land where they would have outside it.
    _saved_clip_stacks.push_back(_clip_stack);
    _saved_view_offsets.push_back(_view_offset);
    _saved_transforms.push_back(_transform);
    _rt_stack.push_back(TargetEntry{.texture = &backend->texture(), .coords = coords});
    _batch.begin(current_target(), SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f}, /*do_clear=*/true);
    _batch.begin_bounds();
    return true;
}

std::optional<IRenderer2DBackend::LayerBounds> GpuRenderer2DBackend::pop_layer_target() {
    if (_rt_stack.empty() || _rt_stack.back().coords.x <= 0.0f) {
        return std::nullopt;
    }
    const Vec2f coords = _rt_stack.back().coords;
    const Vec2f pixels{static_cast<f32>(current_size().x) / coords.x, static_cast<f32>(current_size().y) / coords.y};
    const std::optional<Rectf> drawn = _batch.end_bounds(); // in absolute draw coordinates
    pop_render_target();
    if (!drawn) {
        return LayerBounds{}; // nothing drawn
    }
    // Within the layer, and back to the coordinates draws are given in.
    const f32 x0 = std::max(0.0f, std::floor(drawn->x)), y0 = std::max(0.0f, std::floor(drawn->y));
    const f32 x1 = std::min(coords.x, std::ceil(drawn->x + drawn->w));
    const f32 y1 = std::min(coords.y, std::ceil(drawn->y + drawn->h));
    if (x1 <= x0 || y1 <= y0) {
        return LayerBounds{};
    }
    return LayerBounds{.dest = {x0 - _view_offset.x, y0 - _view_offset.y, x1 - x0, y1 - y0},
                       .source = {x0 * pixels.x, y0 * pixels.y, (x1 - x0) * pixels.x, (y1 - y0) * pixels.y}};
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
    if (!_saved_transforms.empty()) {
        set_transform(_saved_transforms.back());
        _saved_transforms.pop_back();
    } else {
        set_transform({});
    }
    if (_frame) {
        _batch.begin(current_target(), _clear_color, /*do_clear=*/false);
    }
}

void GpuRenderer2DBackend::set_scale_mode(const Texture& texture, ScaleMode mode) {
    // Per-texture sampling: draws pick their sampler from this (sampler_for).
    const auto* backend = as_gpu(texture.backend().get());
    if (!backend) {
        return;
    }
    backend->set_scale_mode(mode);
    if (mode == ScaleMode::Mipmapped && !backend->mipmapped() && !backend->premultiplied() &&
        backend->format() == TextureFormat::Rgba8 && backend->texture()) {
        // Remade with room for its mips; the old one stays alive until the
        // frame's draws (which may name it) are submitted.
        _retired_textures.push_back(backend->replace_texture(_device.make_mipmapped(backend->texture())));
    }
}

SDL_GPUSampler* GpuRenderer2DBackend::sampler_for(const gpu::GpuTextureBackend& texture) const {
    switch (texture.scale_mode()) {
    case ScaleMode::Nearest: return _sampler_nearest;
    case ScaleMode::Linear: return _sampler_linear;
    case ScaleMode::Mipmapped: return texture.mipmapped() ? _sampler_mipmapped : _sampler_linear;
    }
    return _sampler_linear;
}

void GpuRenderer2DBackend::set_viewport(Rectf rect) {
    // Mirror SDL_SetRenderViewport: the viewport rect is absolute, it clips to that
    // rect, AND subsequent draw coords become viewport-relative (origin at rect.x/y).
    // Clips are kept in coordinate space (logical for the scene under logical
    // presentation, target pixels otherwise); current_scissor() scales them.
    const Vec2i coord = coordinate_extent();
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
    const Vec2i coord = coordinate_extent();
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
    const Vec2i coord = coordinate_extent();
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
        desc.num_uniform_buffers, desc.num_samplers, desc.num_storage_buffers);
    if (!shader) {
        KIN_LOG_ERROR_F("render", "create_shader: GpuShader::from_bytes failed",
                        (LogFields{{.name = "error", .value = SDL_GetError()}}));
        return {};
    }
    // Its usual pipeline now, while the game loads, rather than at its first
    // draw: on a cold driver cache that is a ~20 ms hitch. Every 2D target is
    // RGBA8, and shader surfaces blend as alpha unless a blend mode is set.
    _pipelines.get(_vertex_shader.handle(), shader.handle(), gpu::GpuBlendMode::Alpha,
                    SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, gpu::GpuVertexLayout::Triangles);
    // And whatever else an earlier run drew it with.
    const u64 id = spirv_id(desc.spirv);
    _fragment_ids[shader.handle()] = id;
    make_hinted_pipelines(id, shader.handle());
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
    draw_shader_surface(rect, handle, params, sources, std::span<const DataBuffer>{});
}

void GpuRenderer2DBackend::draw_shader_surface(Rectf rect, ShaderHandle handle, const ShaderParams& params,
                                               std::span<const Texture> sources,
                                               std::span<const DataBuffer> buffers) {
    if (handle.value == 0 || handle.value > _shaders.size() || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const gpu::GpuShader& shader = _shaders[static_cast<std::size_t>(handle.value) - 1];
    if (!shader) {
        return;
    }
    const std::optional<std::span<SDL_GPUBuffer* const>> storage = bind_buffers(shader, buffers);
    if (!storage) {
        return;
    }
    ensure_frame();
    const SourceBindings bindings = bind_sources(shader, sources);
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
    place(verts);
    _batch.push(verts, shader.handle(), bindings.slot0.texture, current_scissor(),
                resolve_blend(gpu::GpuBlendMode::Alpha), params.uniforms.data(),
                static_cast<u32>(params.uniforms.size() * sizeof(f32)), bindings.slot0.sampler,
                std::span<const SDL_GPUTextureSamplerBinding>{bindings.extra.data(), bindings.extra_count}, *storage);
}

std::optional<std::span<SDL_GPUBuffer* const>> GpuRenderer2DBackend::bind_buffers(const gpu::GpuShader& shader,
                                                                                std::span<const DataBuffer> buffers) {
    if (buffers.size() < shader.storage_buffers()) {
        // An unbound storage buffer is an error on the GPU: skip the draw.
        KIN_LOG_ERROR_F("render", "shader draw skipped: fewer data buffers than the shader reads",
                        (LogFields{{.name = "given", .value = std::to_string(buffers.size())},
                                   {.name = "shader", .value = std::to_string(shader.storage_buffers())}}));
        return std::nullopt;
    }
    _storage_scratch.clear();
    for (const DataBuffer& buffer : buffers.first(shader.storage_buffers())) {
        const auto* gpu = dynamic_cast<const gpu::GpuDataBuffer*>(buffer.backend().get());
        if (!gpu || !gpu->buffer()) {
            KIN_LOG_ERROR("render", "shader draw skipped: a data buffer from another backend or invalid");
            return std::nullopt;
        }
        gpu->mark_used(_frame_serial);
        _retained_buffers.push_back(buffer.backend());
        _storage_scratch.push_back(gpu->buffer().handle());
    }
    return std::span<SDL_GPUBuffer* const>{_storage_scratch};
}

bool GpuRenderer2DBackend::reload_shader(ShaderHandle handle, const ShaderDesc& desc) {
    if (handle.value == 0 || handle.value > _shaders.size() || !desc.spirv.valid()) {
        return false;
    }
    gpu::GpuShader shader;
    try {
        shader = gpu::GpuShader::from_bytes(_device, SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERFORMAT_SPIRV,
                                            std::span<const u8>{desc.spirv.code, desc.spirv.size},
                                            desc.num_uniform_buffers, desc.num_samplers, desc.num_storage_buffers);
    } catch (const std::exception& e) {
        KIN_LOG_ERROR_F("render", "reload_shader failed", (LogFields{{.name = "error", .value = e.what()}}));
        return false;
    }
    // Queued draws name the old shader: record them before it goes.
    if (_frame) {
        flush_to_frame();
        _batch.begin(current_target(), _clear_color, /*do_clear=*/false);
    }
    gpu::GpuShader& slot = _shaders[static_cast<std::size_t>(handle.value) - 1];
    _pipelines.forget(slot.handle());
    _fragment_ids.erase(slot.handle());
    slot = std::move(shader);
    _pipelines.get(_vertex_shader.handle(), slot.handle(), gpu::GpuBlendMode::Alpha,
                   SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, gpu::GpuVertexLayout::Triangles);
    const u64 id = spirv_id(desc.spirv);
    _fragment_ids[slot.handle()] = id;
    make_hinted_pipelines(id, slot.handle());
    return true;
}

ComputeShaderHandle GpuRenderer2DBackend::create_compute_shader(ShaderBlob spirv, const ShaderLayout& layout) {
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = spirv.code;
    info.code_size = spirv.size;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers = layout.samplers;
    info.num_readonly_storage_textures = layout.storage_textures;
    info.num_readonly_storage_buffers = layout.storage_buffers;
    info.num_readwrite_storage_textures = layout.readwrite_storage_textures;
    info.num_readwrite_storage_buffers = layout.readwrite_storage_buffers;
    info.num_uniform_buffers = layout.uniform_buffers;
    info.threadcount_x = layout.local_size[0];
    info.threadcount_y = layout.local_size[1];
    info.threadcount_z = layout.local_size[2];
    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(_device.handle(), &info);
    if (!pipeline) {
        KIN_LOG_ERROR_F("render", "create_compute_shader: SDL_CreateGPUComputePipeline failed",
                        (LogFields{{.name = "error", .value = SDL_GetError()}}));
        return {};
    }
    _compute_pipelines.push_back(ComputePipeline{.pipeline = pipeline, .samplers = layout.samplers});
    return ComputeShaderHandle{static_cast<u64>(_compute_pipelines.size())};
}

Texture GpuRenderer2DBackend::create_storage_texture(Vec2i size, TextureFormat format) {
    gpu::GpuTexture tex = _device.create_texture(nullptr, static_cast<u32>(size.x), static_cast<u32>(size.y),
                                                 sdl_format(format), texture_format_bytes(format),
                                                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
    return Texture{std::make_shared<gpu::GpuTextureBackend>(std::move(tex), false, ScaleMode::Nearest, format)};
}

bool GpuRenderer2DBackend::dispatch_compute(ComputeShaderHandle handle, Vec2i groups, const ComputeBindings& bindings) {
    if (handle.value == 0 || handle.value > _compute_pipelines.size()) {
        return false;
    }
    const ComputePipeline& compute = _compute_pipelines[static_cast<std::size_t>(handle.value) - 1];
    std::vector<SDL_GPUStorageTextureReadWriteBinding> outputs;
    for (const Texture& output : bindings.outputs) {
        const auto* backend = as_gpu(output.backend().get());
        if (!backend || !backend->texture()) {
            KIN_LOG_ERROR("render", "dispatch_compute: an output is not a storage texture of this renderer");
            return false;
        }
        retain(output);
        outputs.push_back(SDL_GPUStorageTextureReadWriteBinding{.texture = backend->texture().handle()});
    }
    std::vector<SDL_GPUStorageBufferReadWriteBinding> output_buffers;
    std::vector<SDL_GPUBuffer*> buffers;
    for (const auto* list : {&bindings.output_buffers, &bindings.buffers}) {
        for (const DataBuffer& buffer : *list) {
            const auto* gpu = dynamic_cast<const gpu::GpuDataBuffer*>(buffer.backend().get());
            if (!gpu || !gpu->buffer()) {
                KIN_LOG_ERROR("render", "dispatch_compute: a data buffer is not of this renderer");
                return false;
            }
            gpu->mark_used(_frame_serial);
            _retained_buffers.push_back(buffer.backend());
            if (list == &bindings.output_buffers) {
                output_buffers.push_back(SDL_GPUStorageBufferReadWriteBinding{.buffer = gpu->buffer().handle()});
            } else {
                buffers.push_back(gpu->buffer().handle());
            }
        }
    }
    // Draws so far go first; the dispatch records between them and the next.
    ensure_frame();
    flush_to_frame();
    SDL_GPUCommandBuffer* commands = _frame->command_buffer();
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, outputs.data(), static_cast<u32>(outputs.size()),
                                                       output_buffers.data(), static_cast<u32>(output_buffers.size()));
    SDL_BindGPUComputePipeline(pass, compute.pipeline);
    if (compute.samplers > 0) {
        std::vector<SDL_GPUTextureSamplerBinding> samplers(compute.samplers);
        for (std::size_t i = 0; i < samplers.size(); ++i) {
            const auto* backend = i < bindings.sources.size() ? as_gpu(bindings.sources[i].backend().get()) : nullptr;
            if (backend && backend->texture()) {
                retain(bindings.sources[i]);
                samplers[i] = {.texture = backend->texture().handle(), .sampler = _sampler_nearest};
            } else {
                samplers[i] = {.texture = _white.handle(), .sampler = _sampler_nearest};
            }
        }
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
    }
    if (!buffers.empty()) {
        SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), static_cast<u32>(buffers.size()));
    }
    if (bindings.params && !bindings.params->uniforms.empty()) {
        SDL_PushGPUComputeUniformData(commands, 0, bindings.params->uniforms.data(),
                                      static_cast<u32>(bindings.params->uniforms.size() * sizeof(f32)));
    }
    SDL_DispatchGPUCompute(pass, static_cast<u32>(groups.x), static_cast<u32>(groups.y), 1);
    SDL_EndGPUComputePass(pass);
    _batch.begin(current_target(), _clear_color, /*do_clear=*/false);
    return true;
}

DataBuffer GpuRenderer2DBackend::create_data_buffer(std::size_t bytes, const void* data) {
    gpu::GpuBuffer buffer = _device.create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
                                                      SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                                      SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                                  nullptr, static_cast<u32>(bytes));
    _device.upload_storage_buffer(buffer.handle(), 0, static_cast<u32>(bytes), data, /*cycle=*/false);
    return DataBuffer{std::make_shared<gpu::GpuDataBuffer>(std::move(buffer))};
}

bool GpuRenderer2DBackend::update_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes,
                                              const void* data) {
    const auto* gpu = dynamic_cast<const gpu::GpuDataBuffer*>(buffer.backend().get());
    if (!gpu || !gpu->buffer()) {
        return false;
    }
    // Replacing all of one this frame hasn't drawn with lets it cycle, as textures do.
    const bool whole = offset == 0 && bytes == gpu->size() && gpu->used_in_frame() != _frame_serial;
    _device.upload_storage_buffer(gpu->buffer().handle(), static_cast<u32>(offset), static_cast<u32>(bytes), data,
                                  whole);
    return true;
}

bool GpuRenderer2DBackend::write_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes,
                                             const std::function<void(std::span<u8>)>& fill) {
    const auto* gpu = dynamic_cast<const gpu::GpuDataBuffer*>(buffer.backend().get());
    if (!gpu || !gpu->buffer()) {
        return false;
    }
    const bool whole = offset == 0 && bytes == gpu->size() && gpu->used_in_frame() != _frame_serial;
    _device.upload_storage_buffer(gpu->buffer().handle(), static_cast<u32>(offset), static_cast<u32>(bytes), nullptr,
                                  whole, &fill);
    return true;
}

GpuRenderer2DBackend::SourceBindings GpuRenderer2DBackend::bind_sources(const gpu::GpuShader& shader,
                                                                        std::span<const Texture> sources) {
    // `sources[i]` -> fragment sampler i. An invalid source leaves the binding null,
    // which the batch replaces with the white texture and default sampler. Slots the
    // shader declares beyond the sources are bound the same way, so it never samples
    // an unbound slot.
    const auto resolve = [this](const Texture& src) {
        SDL_GPUTextureSamplerBinding binding{};
        if (const auto* backend = as_gpu(src.backend().get());
            backend && backend->texture()) {
            retain(src);
            binding.texture = backend->texture().handle();
            // Integer textures cannot be filtered, and 32-bit floats may not be.
            binding.sampler = backend->format() == TextureFormat::Rgba8 ? sampler_for(*backend) : _sampler_nearest;
        }
        return binding;
    };
    const std::size_t slots = std::min<std::size_t>(
        std::max<std::size_t>({sources.size(), shader.samplers(), 1}), MaxShaderSamplers);
    SourceBindings out;
    out.slot0 = sources.empty() ? SDL_GPUTextureSamplerBinding{} : resolve(sources[0]);
    for (std::size_t i = 1; i < slots && i < sources.size(); ++i) {
        out.extra[i - 1] = resolve(sources[i]);
    }
    out.extra_count = slots - 1;
    return out;
}

void GpuRenderer2DBackend::draw_shader_geometry(std::span<const ShaderVertex> vertices, std::span<const u32> indices,
                                                ShaderHandle handle, const ShaderParams& params,
                                                std::span<const Texture> sources,
                                                std::span<const DataBuffer> buffers) {
    if (handle.value == 0 || handle.value > _shaders.size() || !_shader_vertex_shader.handle()) {
        return;
    }
    const gpu::GpuShader& shader = _shaders[static_cast<std::size_t>(handle.value) - 1];
    if (!shader) {
        return;
    }
    const std::optional<std::span<SDL_GPUBuffer* const>> storage = bind_buffers(shader, buffers);
    if (!storage) {
        return;
    }
    ensure_frame();
    const SourceBindings bindings = bind_sources(shader, sources);
    // The batch draws triangle lists: indices are expanded here.
    const auto convert = [this](const ShaderVertex& v) {
        gpu::GpuShaderVertex out{};
        const Vec2f p = place(v.position);
        out.x = p.x;
        out.y = p.y;
        out.u = v.uv.x;
        out.v = v.uv.y;
        out.r = v.color.r;
        out.g = v.color.g;
        out.b = v.color.b;
        out.a = v.color.a;
        std::copy(v.custom.begin(), v.custom.end(), out.custom);
        return out;
    };
    _shader_vertex_scratch.clear();
    if (indices.empty()) {
        _shader_vertex_scratch.reserve(vertices.size());
        for (const ShaderVertex& v : vertices) {
            _shader_vertex_scratch.push_back(convert(v));
        }
    } else {
        _shader_vertex_scratch.reserve(indices.size());
        for (const u32 i : indices) {
            _shader_vertex_scratch.push_back(convert(vertices[i]));
        }
    }
    _batch.push_shader_vertices(_shader_vertex_scratch, shader.handle(), bindings.slot0.texture, current_scissor(),
                                resolve_blend(gpu::GpuBlendMode::Alpha), params.uniforms.data(),
                                static_cast<u32>(params.uniforms.size() * sizeof(f32)), bindings.slot0.sampler,
                                std::span<const SDL_GPUTextureSamplerBinding>{bindings.extra.data(),
                                                                               bindings.extra_count},
                                *storage);
}

void GpuRenderer2DBackend::set_post_process(std::span<const PostProcessPass> passes) {
    _post_passes.assign(passes.begin(), passes.end());
}

const gpu::GpuTexture* GpuRenderer2DBackend::run_post_chain() {
    // The overdraw view's own last pass instead of the game's: counts to colours.
    std::vector<PostProcessPass> heat;
    if (_overdraw_view && _overdraw_count_shader) {
        if (!_overdraw_heat) {
            const std::vector<u8> code = gpu::read_shader_file(std::filesystem::path{KIN_GPU_SHADER_DIR} / "overdraw_heat.frag.spv");
            ShaderDesc desc;
            desc.spirv = {code.data(), static_cast<u32>(code.size())};
            _overdraw_heat = create_shader(desc);
        }
        heat.push_back(PostProcessPass{.shader = _overdraw_heat});
    }
    const std::vector<PostProcessPass>& passes = heat.empty() ? _post_passes : heat;
    if (passes.empty() || !_frame || !_scene) {
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
    for (const PostProcessPass& pass : passes) {
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
        ctx.instance_shader = _instance_shader.handle();
        ctx.shader_vertex_shader = _shader_vertex_shader.handle();
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
