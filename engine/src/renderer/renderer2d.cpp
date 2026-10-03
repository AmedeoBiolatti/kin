#include <kin/renderer/renderer2d.hpp>

#include <kin/platform/log.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/shader_reflect.hpp>

#include <cmath>

#include "sdl_renderer2d_backend.hpp"
#include "gpu/gpu_renderer2d_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace kin {

std::unique_ptr<IRenderer2DBackend> make_render_backend(Window& window, bool vsync, bool allow_gpu) {
    // The SDL_GPU/Vulkan backend is now the default when a real GPU is available
    // (`allow_gpu` — false for headless/dummy-video). Opt out with KIN_RENDER_BACKEND=sdl
    // to force the SDL_Renderer path; KIN_RENDER_BACKEND=gpu also forces an attempt. If
    // the GPU device can't be created (no Vulkan, etc.) we fall back to SDL_Renderer, so
    // the default degrades gracefully on machines without GPU support.
    const char* choice = std::getenv("KIN_RENDER_BACKEND");
    const bool force_sdl = choice && std::strcmp(choice, "sdl") == 0;
    const bool force_gpu = choice && std::strcmp(choice, "gpu") == 0;
    if ((allow_gpu || force_gpu) && !force_sdl) {
        try {
            return std::make_unique<GpuRenderer2DBackend>(window, vsync);
        } catch (const std::exception& e) {
            KIN_LOG_ERROR_F("render",
                            "SDL_GPU backend unavailable; falling back to SDL_Renderer",
                            (LogFields{{.name = "error", .value = e.what()}}));
        }
    }
    return std::make_unique<SdlRenderer2DBackend>(window, vsync);
}

Texture::Texture(std::shared_ptr<ITextureBackend> backend)
    : _backend(std::move(backend)) {
}

Vec2i Texture::size() const {
    return _backend ? _backend->size() : Vec2i{};
}

TextureFormat Texture::format() const {
    return _backend ? _backend->format() : TextureFormat::Rgba8;
}

namespace {

// Data textures hold numbers for shaders; sampling one as an image is invalid.
bool drawable(const Texture& texture) {
    if (texture.format() == TextureFormat::Rgba8) {
        return true;
    }
    KIN_LOG_ERROR("render", "draw_texture: data textures can only be read by shaders");
    return false;
}

bool valid_params(const ShaderParams& params) {
    if (params.uniforms.size() <= MaxShaderUniformFloats) {
        return true;
    }
    KIN_LOG_ERROR_F("render", "shader params too large",
                    (LogFields{{.name = "floats", .value = std::to_string(params.uniforms.size())},
                               {.name = "max", .value = std::to_string(MaxShaderUniformFloats)}}));
    return false;
}

u64 next_renderer_id() {
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Renderer2D::ViewportGuard::ViewportGuard(Renderer2D& renderer, Rectf rect)
    : _renderer(&renderer) {
    _renderer->push_viewport(rect);
}

Renderer2D::ViewportGuard::~ViewportGuard() {
    if (_renderer) {
        _renderer->pop_viewport();
    }
}

Renderer2D::ViewportGuard::ViewportGuard(ViewportGuard&& other) noexcept
    : _renderer(other._renderer) {
    other._renderer = nullptr;
}

Renderer2D::NativeCoordinateGuard::NativeCoordinateGuard(Renderer2D& renderer)
    : _renderer(&renderer) {
    _renderer->_backend->push_native_coordinates();
}

Renderer2D::NativeCoordinateGuard::~NativeCoordinateGuard() {
    if (_renderer) {
        _renderer->_backend->pop_native_coordinates();
    }
}

Renderer2D::NativeCoordinateGuard::NativeCoordinateGuard(NativeCoordinateGuard&& other) noexcept
    : _renderer(other._renderer) {
    other._renderer = nullptr;
}

Renderer2D::BlendModeGuard::BlendModeGuard(Renderer2D& renderer, BlendMode mode)
    : _renderer(&renderer), _previous(renderer.blend_mode()) {
    _renderer->set_blend_mode(mode);
}

Renderer2D::BlendModeGuard::~BlendModeGuard() {
    if (_renderer) {
        _renderer->set_blend_mode(_previous);
    }
}

Renderer2D::BlendModeGuard::BlendModeGuard(BlendModeGuard&& other) noexcept
    : _renderer(other._renderer), _previous(other._previous) {
    other._renderer = nullptr;
}

void Renderer2D::set_blend_mode(BlendMode mode) {
    if (mode == _blend_mode) {
        return;
    }
    _blend_mode = mode;
    _backend->set_blend_mode(mode);
}

Renderer2D::BlendModeGuard Renderer2D::scoped_blend_mode(BlendMode mode) {
    return BlendModeGuard{*this, mode};
}

Renderer2D::RenderTargetGuard::RenderTargetGuard(Renderer2D& renderer, const RenderTarget& target)
    : _renderer(&renderer) {
    _renderer->push_render_target(target);
}

Renderer2D::RenderTargetGuard::~RenderTargetGuard() {
    if (_renderer) {
        _renderer->pop_render_target();
    }
}

Renderer2D::RenderTargetGuard::RenderTargetGuard(RenderTargetGuard&& other) noexcept
    : _renderer(other._renderer) {
    other._renderer = nullptr;
}

Renderer2D::Renderer2D(Window& window, bool vsync)
    : Renderer2D(std::make_unique<SdlRenderer2DBackend>(window, vsync)) {
}

Renderer2D::Renderer2D(std::unique_ptr<IRenderer2DBackend> backend)
    : _id(next_renderer_id()), _backend(std::move(backend)) {
    if (!_backend) {
        KIN_LOG_ERROR("render", "renderer backend missing");
        throw std::runtime_error("Renderer2D requires a backend");
    }
    KIN_LOG_INFO_F("render",
                   "renderer created",
                   (LogFields{{.name = "backend", .value = std::string{_backend->name()}}}));
}

Renderer2D::~Renderer2D() = default;

Renderer2D::Renderer2D(Renderer2D&&) noexcept = default;
Renderer2D& Renderer2D::operator=(Renderer2D&&) noexcept = default;

std::string_view Renderer2D::backend_name() const {
    return _backend->name();
}

RendererBackendCapabilities Renderer2D::capabilities() const {
    RendererBackendCapabilities capabilities = _backend->capabilities();
    capabilities.queued_2d = true;
    return capabilities;
}

RendererBackendStats Renderer2D::backend_stats() const {
    return _backend->stats();
}

void Renderer2D::reset_backend_stats() {
    _backend->reset_stats();
}

Renderer2D::GpuScope::~GpuScope() {
    if (_renderer) {
        _renderer->_backend->end_gpu_scope();
    }
}

Renderer2D::GpuScope Renderer2D::gpu_scope(std::string_view name) {
    _backend->begin_gpu_scope(name);
    return GpuScope{this};
}

std::vector<GpuScopeTiming> Renderer2D::take_gpu_scope_timings() {
    return _backend->take_gpu_scope_timings();
}

void Renderer2D::set_gpu_timing_enabled(bool enabled) {
    _backend->set_gpu_timing_enabled(enabled);
}

void Renderer2D::set_texture_batching_enabled(bool enabled) {
    _backend->set_texture_batching_enabled(enabled);
}

bool Renderer2D::texture_batching_enabled() const {
    return _backend->texture_batching_enabled();
}

void Renderer2D::clear(Color color) {
    _backend->clear(color);
}

void Renderer2D::clear(u8 r, u8 g, u8 b, u8 a) {
    clear(Color{r, g, b, a});
}

void Renderer2D::present() {
    _backend->present();
}

bool Renderer2D::save_png(const std::string& path) {
    const bool ok = _backend->save_png(path.c_str());
    if (ok) {
        const Vec2i size = output_size();
        KIN_LOG_INFO_F("render",
                       "screenshot saved",
                       (LogFields{
                           {.name = "path", .value = path},
                           {.name = "width", .value = std::to_string(size.x)},
                           {.name = "height", .value = std::to_string(size.y)},
                       }));
    } else {
        KIN_LOG_WARN_F("render",
                       "screenshot failed",
                       (LogFields{{.name = "path", .value = path}}));
    }
    return ok;
}

bool Renderer2D::read_rgba(Rectf logical_region, std::vector<u8>& out, Vec2i& out_size) {
    return _backend->read_rgba(logical_region, out, out_size);
}

void Renderer2D::set_logical_size(i32 width, i32 height) {
    set_logical_size(Vec2i{width, height});
}

void Renderer2D::set_logical_size(Vec2i size) {
    _backend->set_logical_size(size);
    KIN_LOG_INFO_F("render",
                   "logical size set",
                   (LogFields{
                       {.name = "logical_width", .value = std::to_string(size.x)},
                       {.name = "logical_height", .value = std::to_string(size.y)},
                       {.name = "integer_scale", .value = "false"},
                   }));
}

void Renderer2D::set_integer_logical_size(i32 width, i32 height) {
    set_integer_logical_size(Vec2i{width, height});
}

void Renderer2D::set_integer_logical_size(Vec2i size) {
    _backend->set_integer_logical_size(size);
    KIN_LOG_INFO_F("render",
                   "logical size set",
                   (LogFields{
                       {.name = "logical_width", .value = std::to_string(size.x)},
                       {.name = "logical_height", .value = std::to_string(size.y)},
                       {.name = "integer_scale", .value = "true"},
                   }));
}

Vec2i Renderer2D::output_size() const {
    return _backend->output_size();
}

Vec2f Renderer2D::window_to_logical(Vec2f window_px) const {
    return _backend->window_to_logical(window_px);
}

Vec2f Renderer2D::logical_to_window(Vec2f logical) const {
    return _backend->logical_to_window(logical);
}

Texture Renderer2D::create_texture_from_rgba(const u8* pixels, i32 width, i32 height) {
    return create_texture_from_rgba(pixels, Vec2i{width, height});
}

bool Renderer2D::update_texture(const Texture& texture, Vec2i at, Vec2i size, const u8* pixels) {
    const Vec2i whole = texture.size();
    if (!texture.valid() || !pixels || size.x <= 0 || size.y <= 0 || at.x < 0 || at.y < 0 || at.x + size.x > whole.x ||
        at.y + size.y > whole.y) {
        return false;
    }
    try {
        return _backend->update_texture(texture, at, size, pixels);
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("render", "texture update failed", (LogFields{{.name = "error", .value = std::string{error.what()}}}));
        return false;
    }
}

bool Renderer2D::write_texture(const Texture& texture, Vec2i at, Vec2i size,
                               const std::function<void(std::span<u8>)>& fill) {
    const Vec2i whole = texture.size();
    if (!texture.valid() || !fill || size.x <= 0 || size.y <= 0 || at.x < 0 || at.y < 0 || at.x + size.x > whole.x ||
        at.y + size.y > whole.y) {
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) *
                              texture_format_bytes(texture.format());
    try {
        return _backend->write_texture(texture, at, size, bytes, fill);
    } catch (const std::logic_error&) {
        throw; // a misused fill
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("render", "texture write failed", (LogFields{{.name = "error", .value = std::string{error.what()}}}));
        return false;
    }
}

Texture Renderer2D::create_texture(Vec2i size, TextureFormat format, const void* pixels) {
    if (size.x <= 0 || size.y <= 0) {
        return {};
    }
    if (format != TextureFormat::Rgba8 && !capabilities().data_textures) {
        KIN_LOG_ERROR("render", "create_texture: this backend has no data texture formats");
        return {};
    }
    try {
        return _backend->create_texture(size, format, pixels);
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("render", "texture creation failed", (LogFields{{.name = "error", .value = error.what()}}));
        return {};
    }
}

Texture Renderer2D::create_texture_from_rgba(const u8* pixels, Vec2i size) {
    try {
        return _backend->create_texture_from_rgba(pixels, size);
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("render",
                        "texture creation failed",
                        (LogFields{
                            {.name = "width", .value = std::to_string(size.x)},
                            {.name = "height", .value = std::to_string(size.y)},
                            {.name = "error", .value = error.what()},
                        }));
        throw;
    }
}

void Renderer2D::draw_texture(const Texture& texture, Rectf dest) {
    if (drawable(texture)) {
        _backend->draw_texture(texture, dest);
    }
}

void Renderer2D::draw_texture(const Texture& texture, Rectf source, Rectf dest) {
    if (drawable(texture)) {
        _backend->draw_texture(texture, source, dest);
    }
}

void Renderer2D::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint) {
    if (drawable(texture)) {
        _backend->draw_texture(texture, source, dest, tint);
    }
}

void Renderer2D::draw_texture(const Texture& texture, Rectf source, Rectf dest, Color tint, f32 rotation, Vec2f pivot) {
    if (drawable(texture)) {
        _backend->draw_texture(texture, source, dest, tint, rotation, pivot);
    }
}

JobSystem* Renderer2D::set_job_system(JobSystem* jobs) {
    JobSystem* previous = _jobs;
    _jobs = jobs;
    _backend->set_job_system(jobs);
    return previous;
}

void Renderer2D::draw_sprites(const Texture& texture, std::span<const SpriteInstance> sprites) {
    if (!sprites.empty() && drawable(texture)) {
        _backend->draw_sprites(texture, sprites);
    }
}

void Renderer2D::draw_texture(const Texture& texture, Vec2f pos, Vec2f size) {
    draw_texture(texture, Rectf{pos.x, pos.y, size.x, size.y});
}

void Renderer2D::draw_texture(const Texture& texture, Vec2f pos) {
    const Vec2i size = texture.size();
    draw_texture(texture, Rectf{pos.x, pos.y, static_cast<f32>(size.x), static_cast<f32>(size.y)});
}

void Renderer2D::draw_sprite(const Sprite& sprite, Rectf dest) {
    if (!sprite.valid()) {
        return;
    }
    draw_texture(sprite.texture, sprite.source, dest);
}

void Renderer2D::draw_sprite(const Sprite& sprite, Rectf dest, Color tint, f32 rotation, Vec2f pivot) {
    if (!sprite.valid()) {
        return;
    }
    draw_texture(sprite.texture, sprite.source, dest, tint, rotation, pivot);
}

void Renderer2D::draw_sprite(const Sprite& sprite, Vec2f pos, Vec2f size) {
    draw_sprite(sprite, Rectf{pos.x, pos.y, size.x, size.y});
}

void Renderer2D::draw_sprite(const Sprite& sprite, Vec2f pos) {
    draw_sprite(sprite, Rectf{pos.x, pos.y, sprite.source.w, sprite.source.h});
}

void Renderer2D::fill_rect(Rectf rect, Color color) {
    _backend->fill_rect(rect, color);
}

void Renderer2D::fill_rect(Vec2f pos, Vec2f size, Color color) {
    fill_rect(Rectf{pos.x, pos.y, size.x, size.y}, color);
}

void Renderer2D::fill_rect(Vec2f pos, Vec2f size, u8 r, u8 g, u8 b, u8 a) {
    fill_rect(pos, size, Color{r, g, b, a});
}

void Renderer2D::draw_rect(Rectf rect, Color color) {
    _backend->draw_rect(rect, color);
}

void Renderer2D::draw_rect(Vec2f pos, Vec2f size, Color color) {
    draw_rect(Rectf{pos.x, pos.y, size.x, size.y}, color);
}

void Renderer2D::draw_rect(Vec2f pos, Vec2f size, u8 r, u8 g, u8 b, u8 a) {
    draw_rect(pos, size, Color{r, g, b, a});
}

void Renderer2D::fill_rounded_rect(Rectf rect, f32 radius, Color color) {
    _backend->fill_rounded_rect(rect, radius, color);
}

void Renderer2D::draw_rounded_rect(Rectf rect, f32 radius, Color color, f32 width) {
    _backend->draw_rounded_rect(rect, radius, color, width);
}

void Renderer2D::fill_gradient_rect(Rectf rect, const Gradient& gradient) {
    _backend->fill_gradient_rect(rect, gradient);
}

ShaderHandle Renderer2D::create_shader(const ShaderDesc& desc) {
    ShaderDesc used = desc;
    std::shared_ptr<const ShaderLayout> layout;
    if (desc.spirv.valid()) {
        std::string error;
        if (std::optional<ShaderLayout> read = reflect_spirv(desc.spirv, &error)) {
            const auto agree = [&](const char* what, u32 given, u32 declared, u32& into) {
                // The default of one sampler stands for a shader that samples none.
                if (given != declared && !(given == 1 && declared == 0 && std::string_view{what} == "samplers")) {
                    KIN_LOG_WARN_F("render", "create_shader: ShaderDesc disagrees with the shader; using the shader's",
                                   (LogFields{{.name = "what", .value = what},
                                              {.name = "desc", .value = std::to_string(given)},
                                              {.name = "shader", .value = std::to_string(declared)}}));
                    into = declared;
                }
            };
            agree("samplers", desc.num_samplers, read->samplers, used.num_samplers);
            agree("uniform_buffers", desc.num_uniform_buffers, read->uniform_buffers, used.num_uniform_buffers);
            agree("storage_buffers", desc.num_storage_buffers, read->storage_buffers, used.num_storage_buffers);
            layout = std::make_shared<const ShaderLayout>(std::move(*read));
        } else {
            KIN_LOG_WARN_F("render", "create_shader: SPIR-V not readable; trusting the ShaderDesc",
                           (LogFields{{.name = "error", .value = error}}));
        }
    }
    const ShaderHandle handle = _backend->create_shader(used);
    if (handle && layout) {
        _shader_layouts[handle.value] = std::move(layout);
    }
    return handle;
}

std::shared_ptr<const ShaderLayout> Renderer2D::shader_layout(ShaderHandle shader) const {
    const auto it = _shader_layouts.find(shader.value);
    return it == _shader_layouts.end() ? nullptr : it->second;
}

ShaderParams Renderer2D::shader_params(ShaderHandle shader) const {
    ShaderParams params;
    params.layout = shader_layout(shader);
    if (params.layout) {
        params.uniforms.assign(std::max<std::size_t>(16, (params.layout->uniform_bytes + 3) / 4), 0.0f);
    }
    return params;
}

void Renderer2D::draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params) {
    if (valid_params(params)) {
        _backend->draw_shader_surface(rect, shader, params);
    }
}

void Renderer2D::draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     const Texture& source) {
    if (valid_params(params)) {
        _backend->draw_shader_surface(rect, shader, params, source);
    }
}

void Renderer2D::draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     const Texture& source0, const Texture& source1) {
    if (valid_params(params)) {
        _backend->draw_shader_surface(rect, shader, params, source0, source1);
    }
}

void Renderer2D::draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     std::span<const Texture> sources) {
    if (!valid_params(params)) {
        return;
    }
    if (sources.size() > MaxShaderSamplers) {
        KIN_LOG_ERROR_F("render", "draw_shader_surface: too many sources",
                        (LogFields{{.name = "sources", .value = std::to_string(sources.size())},
                                   {.name = "max", .value = std::to_string(MaxShaderSamplers)}}));
        return;
    }
    _backend->draw_shader_surface(rect, shader, params, sources);
}

void Renderer2D::draw_shader_surface(Rectf rect, ShaderHandle shader, const ShaderParams& params,
                                     std::span<const Texture> sources, std::span<const DataBuffer> buffers) {
    if (!valid_params(params)) {
        return;
    }
    if (sources.size() > MaxShaderSamplers || buffers.size() > MaxShaderStorageBuffers) {
        KIN_LOG_ERROR("render", "draw_shader_surface: too many sources or buffers");
        return;
    }
    _backend->draw_shader_surface(rect, shader, params, sources, buffers);
}

namespace {
// A range of a data buffer that lies inside it.
bool within(const DataBuffer& buffer, std::size_t offset, std::size_t bytes) {
    return buffer.valid() && bytes > 0 && offset <= buffer.size() && bytes <= buffer.size() - offset;
}
} // namespace

DataBuffer Renderer2D::create_data_buffer(std::size_t bytes, const void* data) {
    if (bytes == 0 || bytes > 0xFFFFFFFFu || !capabilities().data_buffers) {
        return {};
    }
    try {
        return _backend->create_data_buffer(bytes, data);
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("render", "data buffer creation failed", (LogFields{{.name = "error", .value = error.what()}}));
        return {};
    }
}

bool Renderer2D::update_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes, const void* data) {
    if (!data || !within(buffer, offset, bytes)) {
        return false;
    }
    try {
        return _backend->update_data_buffer(buffer, offset, bytes, data);
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("render", "data buffer update failed", (LogFields{{.name = "error", .value = error.what()}}));
        return false;
    }
}

bool Renderer2D::write_data_buffer(const DataBuffer& buffer, std::size_t offset, std::size_t bytes,
                                   const std::function<void(std::span<u8>)>& fill) {
    if (!fill || !within(buffer, offset, bytes)) {
        return false;
    }
    try {
        return _backend->write_data_buffer(buffer, offset, bytes, fill);
    } catch (const std::logic_error&) {
        throw; // a misused fill
    } catch (const std::exception& error) {
        KIN_LOG_ERROR_F("render", "data buffer write failed", (LogFields{{.name = "error", .value = error.what()}}));
        return false;
    }
}

void Renderer2D::draw_shader_geometry(std::span<const ShaderVertex> vertices, std::span<const u32> indices,
                                      ShaderHandle shader, const ShaderParams& params,
                                      std::span<const Texture> sources, std::span<const DataBuffer> buffers) {
    if (!valid_params(params) || vertices.empty()) {
        return;
    }
    const auto refuse = [](std::string_view why) {
        KIN_LOG_ERROR_F("render", "draw_shader_geometry: nothing drawn", (LogFields{{.name = "reason", .value = std::string{why}}}));
    };
    if (sources.size() > MaxShaderSamplers || buffers.size() > MaxShaderStorageBuffers) {
        refuse("more sources or buffers than a shader can take");
        return;
    }
    if (indices.empty() ? vertices.size() % 3 != 0 : indices.size() % 3 != 0) {
        refuse("not a whole number of triangles");
        return;
    }
    if (std::any_of(indices.begin(), indices.end(), [&](u32 i) { return i >= vertices.size(); })) {
        refuse("an index past the vertices");
        return;
    }
    _backend->draw_shader_geometry(vertices, indices, shader, params, sources, buffers);
}

void Renderer2D::set_post_process(std::span<const PostProcessPass> passes) {
    for (const PostProcessPass& pass : passes) {
        if (!valid_params(pass.params)) {
            return; // the previous chain stays
        }
    }
    _backend->set_post_process(passes);
}

void Renderer2D::clear_post_process() {
    _backend->set_post_process({});
}

ShaderHandle Renderer2D::builtin_shader(BuiltinShader id) {
    const int key = static_cast<int>(id);
    if (auto it = _builtin_shaders.find(key); it != _builtin_shaders.end()) {
        return it->second;
    }
    ShaderHandle handle{};
#ifdef KIN_GPU_SHADER_DIR
    if (capabilities().materials_2d) {
        // Map the built-in id to its compiled SPIR-V file + the sampler count its
        // descriptor declares (2 for the two-input combine/crossfade shaders).
        const char* file = nullptr;
        u32 samplers = 1;
        switch (id) {
        case BuiltinShader::Vignette: file = "pp_vignette.frag.spv"; break;
        case BuiltinShader::ColorGrade: file = "pp_color_grade.frag.spv"; break;
        case BuiltinShader::Scanline: file = "pp_scanline.frag.spv"; break;
        case BuiltinShader::Chroma: file = "pp_chroma.frag.spv"; break;
        case BuiltinShader::BloomBright: file = "pp_bloom_bright.frag.spv"; break;
        case BuiltinShader::BloomBlur: file = "pp_bloom_blur.frag.spv"; break;
        case BuiltinShader::BloomCombine: file = "pp_bloom_combine.frag.spv"; samplers = 2; break;
        case BuiltinShader::TransitionDissolve: file = "transition_dissolve.frag.spv"; break;
        case BuiltinShader::TransitionPixelate: file = "transition_pixelate.frag.spv"; break;
        case BuiltinShader::TransitionWipe: file = "transition_wipe.frag.spv"; break;
        case BuiltinShader::TransitionIris: file = "transition_iris.frag.spv"; break;
        case BuiltinShader::TransitionCrossfade: file = "transition_crossfade.frag.spv"; samplers = 2; break;
        }
        if (file) {
            const std::filesystem::path path = std::filesystem::path{KIN_GPU_SHADER_DIR} / file;
            std::ifstream in(path, std::ios::binary);
            if (in) {
                const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                                            std::istreambuf_iterator<char>());
                if (!bytes.empty()) {
                    ShaderDesc desc;
                    desc.spirv = {bytes.data(), static_cast<u32>(bytes.size())};
                    desc.num_samplers = samplers;
                    desc.num_uniform_buffers = 1;
                    handle = create_shader(desc);
                }
            } else {
                KIN_LOG_ERROR_F("render", "builtin_shader: .spv not found",
                                (LogFields{{.name = "path", .value = path.string()}}));
            }
        }
    }
#endif
    _builtin_shaders[key] = handle; // cache (incl. null on degrade) so we don't re-probe
    return handle;
}

void Renderer2D::draw_line(Vec2f a, Vec2f b, Color color) {
    _backend->draw_line(a, b, color);
}

void Renderer2D::draw_line(Vec2f a, Vec2f b, u8 r, u8 g, u8 b_color, u8 a_color) {
    draw_line(a, b, Color{r, g, b_color, a_color});
}

void Renderer2D::set_viewport(Rectf rect) {
    _backend->set_viewport(rect);
}

void Renderer2D::reset_viewport() {
    _backend->reset_viewport();
}

Renderer2D::ViewportGuard Renderer2D::scoped_viewport(Rectf rect) {
    return ViewportGuard{*this, rect};
}

Renderer2D::NativeCoordinateGuard Renderer2D::scoped_native_coordinates() {
    return NativeCoordinateGuard{*this};
}

void Renderer2D::push_viewport(Rectf rect) {
    _backend->push_viewport(rect);
}

void Renderer2D::pop_viewport() {
    _backend->pop_viewport();
}

void Renderer2D::push_clip(Rectf rect) {
    _backend->push_clip(rect);
}

void Renderer2D::pop_clip() {
    _backend->pop_clip();
}

RenderTarget Renderer2D::create_render_target(Vec2i size, ScaleMode mode) {
    return _backend->create_render_target(size, mode);
}

void Renderer2D::push_render_target(const RenderTarget& target) {
    _backend->push_render_target(target);
}

void Renderer2D::pop_render_target() {
    _backend->pop_render_target();
}

void Renderer2D::set_scale_mode(const Texture& texture, ScaleMode mode) {
    _backend->set_scale_mode(texture, mode);
}

Renderer2D::RenderTargetGuard Renderer2D::scoped_render_target(const RenderTarget& target) {
    return RenderTargetGuard{*this, target};
}

PooledTarget Renderer2D::acquire_render_target(Vec2i size, ScaleMode mode) {
    // Reuse a free entry matching size + scale mode (format is always RGBA32).
    for (RenderTargetPoolEntry& entry : _rt_pool) {
        if (!entry.in_use && entry.target.valid() && entry.size == size && entry.mode == mode) {
            entry.in_use = true;
            PooledTarget pooled;
            pooled._pool = this;
            pooled._rt = entry.target; // shares the same SDL texture (shared_ptr)
            return pooled;
        }
    }

    RenderTarget target = create_render_target(size, mode);
    PooledTarget pooled;
    pooled._rt = target;
    if (!target.valid()) {
        // Backend without render-target support: nothing to pool; leave _pool null
        // so the empty PooledTarget releases nothing and callers degrade on !valid().
        return pooled;
    }
    pooled._pool = this;
    _rt_pool.push_back({.target = target, .size = size, .mode = mode, .in_use = true});
    return pooled;
}

PooledTarget Renderer2D::capture_backdrop(Rectf region) {
    if (region.w <= 0.0f || region.h <= 0.0f) {
        return {};
    }
    // Fast path: GPU-side copy into a native-resolution target (no CPU round-trip).
    const Vec2i px = _backend->region_pixel_size(region);
    if (px.x > 0 && px.y > 0) {
        PooledTarget rt = acquire_render_target(px, ScaleMode::Linear);
        if (rt.valid() && _backend->blit_region_to_target(region, rt.target())) {
            return rt;
        }
        // Blit unsupported (or RT alloc failed): rt releases here; fall through to readback.
    }
    // Fallback: read the region back (at its native size) and re-upload it into a target.
    std::vector<u8> pixels;
    Vec2i cap{};
    if (!read_rgba(region, pixels, cap) || cap.x <= 0 || cap.y <= 0) {
        return {};
    }
    PooledTarget rt = acquire_render_target(cap, ScaleMode::Linear);
    if (!rt.valid()) {
        return {};
    }
    const Texture captured = create_texture_from_rgba(pixels.data(), cap);
    if (!captured.valid()) {
        return {};
    }
    {
        const RenderTargetGuard guard = scoped_render_target(rt.target());
        clear(colors::transparent);
        const Rectf full{0.0f, 0.0f, static_cast<f32>(cap.x), static_cast<f32>(cap.y)};
        draw_texture(captured, full, full);
    }
    return rt;
}

void Renderer2D::release_render_target_(const RenderTarget& target) {
    if (!target.valid()) {
        return;
    }
    const ITextureBackend* key = target.texture().backend().get();
    for (RenderTargetPoolEntry& entry : _rt_pool) {
        if (entry.target.texture().backend().get() == key) {
            entry.in_use = false;
            return;
        }
    }
}

void Renderer2D::trim_render_target_pool() {
    std::erase_if(_rt_pool, [](const RenderTargetPoolEntry& entry) { return !entry.in_use; });
}

std::size_t Renderer2D::render_target_pool_size() const {
    return _rt_pool.size();
}

PooledTarget::~PooledTarget() {
    if (_pool) {
        _pool->release_render_target_(_rt);
    }
}

PooledTarget::PooledTarget(PooledTarget&& other) noexcept
    : _pool(other._pool), _rt(std::move(other._rt)) {
    other._pool = nullptr;
}

PooledTarget& PooledTarget::operator=(PooledTarget&& other) noexcept {
    if (this != &other) {
        if (_pool) {
            _pool->release_render_target_(_rt); // release what we currently hold first
        }
        _pool = other._pool;
        _rt = std::move(other._rt);
        other._pool = nullptr;
    }
    return *this;
}

} // namespace kin
