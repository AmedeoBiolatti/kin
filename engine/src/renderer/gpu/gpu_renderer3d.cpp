#include "gpu_renderer3d.hpp"

#include "gpu_device.hpp"
#include "gpu_renderer2d_backend.hpp"

#include <kin/core/jobs.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/color.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace kin::gpu {

namespace {

struct DepthFormat {
    SDL_GPUTextureFormat format;
    u32 bits;
    bool floating;
};

// Reverse Z keeps a float depth buffer precise out to the far plane; the
// fixed-point ones lose detail with distance.
constexpr std::array<DepthFormat, 3> depth_formats{{
    {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, 32, true},
    {SDL_GPU_TEXTUREFORMAT_D24_UNORM, 24, false},
    {SDL_GPU_TEXTUREFORMAT_D16_UNORM, 16, false},
}};

const DepthFormat* best_depth_format(SDL_GPUDevice* device) {
    for (const DepthFormat& depth : depth_formats) {
        if (SDL_GPUTextureSupportsFormat(device, depth.format, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            return &depth;
        }
    }
    return nullptr;
}

// Samples a pixel can take with both `colour` and `depth` at that count.
u32 max_samples(SDL_GPUDevice* device, SDL_GPUTextureFormat colour, SDL_GPUTextureFormat depth) {
    constexpr std::array<std::pair<SDL_GPUSampleCount, u32>, 3> counts{
        {{SDL_GPU_SAMPLECOUNT_8, 8}, {SDL_GPU_SAMPLECOUNT_4, 4}, {SDL_GPU_SAMPLECOUNT_2, 2}}};
    for (const auto& [count, samples] : counts) {
        if (SDL_GPUTextureSupportsSampleCount(device, colour, count) &&
            SDL_GPUTextureSupportsSampleCount(device, depth, count)) {
            return samples;
        }
    }
    return 1;
}

SDL_GPUSampleCount sample_count(u32 samples) {
    return samples >= 8 ? SDL_GPU_SAMPLECOUNT_8
         : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4
         : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2
                        : SDL_GPU_SAMPLECOUNT_1;
}

constexpr SDL_GPUTextureFormat hdr_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
constexpr SDL_GPUTextureFormat pick_format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT; // position, id
constexpr u32 max_lights = 16;
// The overlay's slice of the depth range: reverse Z, so in front of the scene's.
constexpr f32 overlay_split = 0.9f;

// mesh.vert's uniforms, a draw.
struct DrawUniforms {
    f32 view_projection[16];
    u32 first[4]; // x: the draw's first instance
};

// mesh.frag's: the frame's (slot 0) and the draw's (slot 1).
struct FrameUniforms {
    f32 sun_direction[4];
    f32 sun_color[4];
    f32 ambient[4];
    f32 ground_ambient[4];
    f32 fill_direction[4];
    f32 fill_color[4];
    f32 flags[4]; // encode to sRGB, decode textures, shadows, point lights
    f32 shadow_matrix[16];
    f32 shadow[4]; // depth bias, normal offset, a texel (uv)
    f32 lights[max_lights * 2 * 4];
};
struct MaterialUniforms {
    f32 params[4]; // emissive, unlit, receives the sun's shadow
};

// A view's six bounding planes from its view-projection matrix, reverse Z:
// near at depth 1, far at 0. An infinite far plane has no normal.
GpuFrustum frustum_of(const Mat4f& m) {
    const auto row = [&](int r) { return Vec4f{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)}; };
    const Vec4f r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const auto add = [](Vec4f a, Vec4f b, f32 k) { return Vec4f{a.x + k * b.x, a.y + k * b.y, a.z + k * b.z, a.w + k * b.w}; };
    GpuFrustum f;
    for (const Vec4f p : {add(r3, r0, 1.0f), add(r3, r0, -1.0f), add(r3, r1, 1.0f), add(r3, r1, -1.0f),
                          add(r3, r2, -1.0f), r2}) {
        const f32 n = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (n > 1e-6f) {
            f.planes[f.count++] = {p.x / n, p.y / n, p.z / n, p.w / n};
        }
    }
    return f;
}

bool sphere_in(const GpuFrustum& f, Vec3f c, f32 r) {
    for (u32 k = 0; k < f.count; ++k) {
        const Vec4f& p = f.planes[k];
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r) {
            return false;
        }
    }
    return true;
}

// A copy's bounding sphere: its mesh's, moved and scaled with it.
void sphere_of(const Mat4f& t, Vec3f center, f32 radius, Vec3f& c, f32& r) {
    c = t.apply(center);
    const f32 sx = t.m[0] * t.m[0] + t.m[1] * t.m[1] + t.m[2] * t.m[2];
    const f32 sy = t.m[4] * t.m[4] + t.m[5] * t.m[5] + t.m[6] * t.m[6];
    const f32 sz = t.m[8] * t.m[8] + t.m[9] * t.m[9] + t.m[10] * t.m[10];
    r = radius * std::sqrt(std::max({sx, sy, sz}));
}

void put(f32* out, Vec3f v, f32 w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

} // namespace

GpuMesh3D::~GpuMesh3D() {
    if (device && *device) {
        for (SDL_GPUBuffer* buffer : {vertices, indices}) {
            if (buffer) {
                SDL_ReleaseGPUBuffer(*device, buffer); // freed once the GPU is done with it
            }
        }
    }
}

Texture GpuView3D::texture() const {
    return Texture{_resolved};
}

bool GpuRenderer3D::supported(SDL_GPUDevice* device) {
    return device && best_depth_format(device);
}

GpuRenderer3D::GpuRenderer3D(GpuRenderer2DBackend& owner, GpuDevice& device) : _owner(&owner), _device(&device) {
    for (std::size_t i = 0; i < _srgb_to_linear.size(); ++i) {
        _srgb_to_linear[i] = srgb_to_linear(static_cast<f32>(i) / 255.0f);
    }
    SDL_GPUDevice* handle = device.handle();
    const DepthFormat* depth = best_depth_format(handle);
    if (!depth) {
        KIN_LOG_WARN("render", "3D: the device has no depth format");
        return;
    }
    _depth_format = depth->format;
    _capabilities.depth_bits = depth->bits;
    _capabilities.depth_float = depth->floating;
    _capabilities.max_samples = max_samples(handle, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, _depth_format);
    _capabilities.max_samples_hdr = max_samples(handle, hdr_format, _depth_format);
    // Shadow maps: a depth format that can also be sampled.
    for (const SDL_GPUTextureFormat format : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D16_UNORM}) {
        if (SDL_GPUTextureSupportsFormat(handle, format, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
            _shadow_format = format;
            break;
        }
    }
    const char* name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(handle), SDL_PROP_GPU_DEVICE_NAME_STRING, "");
    KIN_LOG_INFO_F("render", "3D views available",
                   (LogFields{{.name = "device", .value = name ? name : ""},
                              {.name = "depth_bits", .value = std::to_string(_capabilities.depth_bits)},
                              {.name = "max_samples", .value = std::to_string(_capabilities.max_samples)},
                              {.name = "max_samples_hdr", .value = std::to_string(_capabilities.max_samples_hdr)},
                              {.name = "shadows", .value = _shadow_format != SDL_GPU_TEXTUREFORMAT_INVALID ? "true" : "false"}}));
    try {
        _vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh.vert.spv",
                                                  /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/1);
        _fragment_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh.frag.spv",
                                                    /*uniform_buffers=*/2, /*samplers=*/2);
        _shadow_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "shadow.frag.spv");
        _fullscreen_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "fullscreen.vert.spv");
        _bloom_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "bloom.frag.spv",
                                                 /*uniform_buffers=*/1, /*samplers=*/1);
        _output_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "view_output.frag.spv",
                                                  /*uniform_buffers=*/1, /*samplers=*/2);
        _pick_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "pick.frag.spv");
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: meshes not drawn", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    if (_shadow_format != SDL_GPU_TEXTUREFORMAT_INVALID) {
        // Bilinear comparisons: each tap is itself four, softening the edge.
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter = SDL_GPU_FILTER_LINEAR;
        info.mag_filter = SDL_GPU_FILTER_LINEAR;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.enable_compare = true;
        info.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL; // reverse Z: lit where nothing nearer the sun
        _compare_sampler = SDL_CreateGPUSampler(handle, &info);
        try {
            _no_shadow = device.create_target(1, 1, _shadow_format,
                                              SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        } catch (const std::exception&) {
            _shadow_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        }
    }
}

GpuRenderer3D::~GpuRenderer3D() {
    if (_compare_sampler) {
        SDL_ReleaseGPUSampler(_device->handle(), _compare_sampler);
    }
}

u32 GpuRenderer3D::usable_samples(u32 wanted, SDL_GPUTextureFormat format) const {
    for (const u32 samples : {8u, 4u, 2u}) {
        if (samples <= wanted && SDL_GPUTextureSupportsSampleCount(_device->handle(), format, sample_count(samples)) &&
            SDL_GPUTextureSupportsSampleCount(_device->handle(), _depth_format, sample_count(samples))) {
            return samples;
        }
    }
    return 1;
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull, Pass pass) const {
    if (pass == Pass::Shadow) {
        // Both faces cast: thin and open meshes still shade.
        return _owner->_pipelines.get(GpuPipelineKey{.vertex = _vertex_shader.handle(),
                                                     .fragment = _shadow_shader.handle(),
                                                     .blend = GpuBlendMode::Replace,
                                                     .layout = GpuVertexLayout::MeshVertices,
                                                     .depth_stencil = _shadow_format,
                                                     .depth = GpuDepthTest::TestWrite,
                                                     .depth_only = true});
    }
    if (pass == Pass::Pick) {
        return _owner->_pipelines.get(GpuPipelineKey{.vertex = _vertex_shader.handle(),
                                                     .fragment = _pick_shader.handle(),
                                                     .blend = GpuBlendMode::Replace,
                                                     .format = pick_format,
                                                     .layout = GpuVertexLayout::MeshVertices,
                                                     .depth_stencil = _depth_format,
                                                     .depth = GpuDepthTest::TestWrite,
                                                     .cull = cull ? GpuCull::Back : GpuCull::None});
    }
    // Opaque: written over what is there, alpha and all; transparent: blended,
    // tested against the depth but not writing it; wireframes over the faces.
    const GpuBlendMode blend = pass == Pass::Opaque || pass == Pass::Wire ? GpuBlendMode::Replace
                             : pass == Pass::Alpha                       ? GpuBlendMode::Premultiplied
                                                                         : GpuBlendMode::PremultipliedAdditive;
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = _vertex_shader.handle(),
                                                 .fragment = _fragment_shader.handle(),
                                                 .blend = blend,
                                                 .format = format,
                                                 .layout = GpuVertexLayout::MeshVertices,
                                                 .depth_stencil = _depth_format,
                                                 .depth = pass == Pass::Opaque ? GpuDepthTest::TestWrite : GpuDepthTest::Test,
                                                 .cull = cull && pass != Pass::Wire ? GpuCull::Back : GpuCull::None,
                                                 .samples = sample_count(samples),
                                                 .wireframe = pass == Pass::Wire,
                                                 .depth_bias = pass == Pass::Wire});
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::fullscreen(SDL_GPUShader* fragment, SDL_GPUTextureFormat format) const {
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = _fullscreen_shader.handle(),
                                                 .fragment = fragment,
                                                 .blend = GpuBlendMode::Replace,
                                                 .format = format,
                                                 .layout = GpuVertexLayout::None});
}

std::shared_ptr<IMeshBackend> GpuRenderer3D::create_mesh(const MeshData& data) {
    const std::size_t vertex_count = data.vertices.size();
    const std::size_t index_count = data.indices.empty() ? vertex_count : data.indices.size();
    try {
        auto mesh = std::make_shared<GpuMesh3D>(_device->shared());
        const u32 vertex_bytes = static_cast<u32>(vertex_count * sizeof(GpuMeshVertex));
        const u32 index_bytes = static_cast<u32>(index_count * sizeof(u32));
        const SDL_GPUBufferCreateInfo vertex_info{.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vertex_bytes, .props = 0};
        const SDL_GPUBufferCreateInfo index_info{.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = index_bytes, .props = 0};
        mesh->vertices = SDL_CreateGPUBuffer(_device->handle(), &vertex_info);
        mesh->indices = SDL_CreateGPUBuffer(_device->handle(), &index_info);
        if (!mesh->vertices || !mesh->indices) {
            throw std::runtime_error(std::string{"SDL_CreateGPUBuffer failed: "} + SDL_GetError());
        }
        // Written straight into the upload's memory (no copy of a large
        // model's vertices on the way), in slices on the workers when there
        // are many; a large mesh in pieces the device's usual staging memory
        // takes (not a new staging buffer as large as it, written for the
        // first time).
        constexpr std::size_t slice = 64u * 1024u;
        constexpr std::size_t piece_bytes = 8u * 1024u * 1024u;
        const auto in_slices = [&](std::size_t count, const auto& write) {
            const auto slices = static_cast<i32>((count + slice - 1) / slice);
            if (_owner->_jobs && slices > 2) {
                _owner->_jobs->parallel_for(slices, [&](i32 i) {
                    const std::size_t begin = static_cast<std::size_t>(i) * slice;
                    write(begin, std::min(count, begin + slice));
                });
            } else {
                write(0, count);
            }
        };
        // With the texture uploads: on the GPU before the next frame draws.
        const auto upload = [&](SDL_GPUBuffer* buffer, std::size_t count, std::size_t size, const auto& write) {
            const std::size_t per_piece = std::max<std::size_t>(1, piece_bytes / size);
            for (std::size_t first = 0; first < count; first += per_piece) {
                const std::size_t n = std::min(per_piece, count - first);
                const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
                    in_slices(n, [&](std::size_t begin, std::size_t end) { write(out.data(), first, begin, end); });
                };
                _device->upload_storage_buffer(buffer, static_cast<u32>(first * size), static_cast<u32>(n * size), nullptr, false, &fill);
            }
        };
        upload(mesh->vertices, vertex_count, sizeof(GpuMeshVertex), [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
            auto* to = reinterpret_cast<GpuMeshVertex*>(out);
            for (std::size_t i = begin; i < end; ++i) {
                const MeshVertex& v = data.vertices[first + i];
                to[i] = {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y,
                         v.color.r, v.color.g, v.color.b, v.color.a};
            }
        });
        upload(mesh->indices, index_count, sizeof(u32), [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
            auto* to = reinterpret_cast<u32*>(out);
            if (data.indices.empty()) {
                for (std::size_t i = begin; i < end; ++i) {
                    to[i] = static_cast<u32>(first + i); // in order: three vertices a triangle
                }
            } else {
                std::memcpy(to + begin, data.indices.data() + first + begin, (end - begin) * sizeof(u32));
            }
        });
        mesh->index_count = static_cast<u32>(index_count);
        return mesh;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: mesh not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return {};
    }
}

bool GpuRenderer3D::make_targets(GpuView3D& view) {
    const SDL_GPUTextureFormat output = _owner->_target_format;
    const bool want_hdr = view._options.hdr || view._options.bloom > 0.0f;
    const bool hdr = want_hdr && SDL_GPUTextureSupportsFormat(_device->handle(), hdr_format, SDL_GPU_TEXTURETYPE_2D,
                                                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    const SDL_GPUTextureFormat format = hdr ? hdr_format : output;
    const u32 samples = usable_samples(std::max(1u, view._options.samples), format);
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    constexpr SDL_GPUTextureUsageFlags drawn = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    try {
        GpuTexture resolved = _device->create_target(w, h, output, drawn);
        GpuTexture scene = hdr ? _device->create_target(w, h, hdr_format, drawn) : GpuTexture{};
        GpuTexture colour = samples > 1 ? _device->create_target(w, h, format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, sample_count(samples))
                                        : GpuTexture{};
        GpuTexture depth = _device->create_target(w, h, _depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
                                                  sample_count(samples));
        GpuTexture pick, pick_depth;
        if (view._options.pick && _pick_shader &&
            SDL_GPUTextureSupportsFormat(_device->handle(), pick_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) {
            pick = _device->create_target(w, h, pick_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
            pick_depth = _device->create_target(w, h, _depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
        }
        GpuTexture bloom_a, bloom_b;
        if (hdr && view._options.bloom > 0.0f) {
            bloom_a = _device->create_target(std::max(1u, w / 2), std::max(1u, h / 2), hdr_format, drawn);
            bloom_b = _device->create_target(std::max(1u, w / 2), std::max(1u, h / 2), hdr_format, drawn);
        }
        // Render targets' convention: premultiplied, sampled smoothly. SDL
        // frees the old targets once the GPU is done with them.
        view._resolved = std::make_shared<GpuTextureBackend>(std::move(resolved), /*premultiplied=*/true, ScaleMode::Linear);
        view._scene = std::move(scene);
        view._colour = std::move(colour);
        view._depth = std::move(depth);
        view._bloom_a = std::move(bloom_a);
        view._bloom_b = std::move(bloom_b);
        view._pick = std::move(pick);
        view._pick_depth = std::move(pick_depth);
        view._samples = samples;
        view._hdr = hdr;
        view._format = format;
        view._output_format = output;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: view targets not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return false;
    }
    // Made now rather than on the first draw: a cold driver cache takes tens of ms.
    for (const Pass pass : {Pass::Opaque, Pass::Alpha, Pass::Additive}) {
        pipeline(format, samples, true, pass);
        pipeline(format, samples, false, pass);
    }
    if (view._pick) {
        pipeline(pick_format, 1, true, Pass::Pick);
        pipeline(pick_format, 1, false, Pass::Pick);
    }
    if (hdr) {
        fullscreen(_bloom_shader.handle(), hdr_format);
        fullscreen(_output_shader.handle(), output);
    }
    return true;
}

std::shared_ptr<IView3DBackend> GpuRenderer3D::create_view(Vec2i size, const View3DOptions& options) {
    auto view = std::make_shared<GpuView3D>();
    view->_size = size;
    view->_options = options;
    if (!make_targets(*view)) {
        return {};
    }
    return view;
}

bool GpuRenderer3D::resize_view(IView3DBackend& base, Vec2i size) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view) {
        return false;
    }
    const Vec2i old = view->_size;
    view->_size = size;
    if (!make_targets(*view)) {
        view->_size = old;
        return false;
    }
    return true;
}

bool GpuRenderer3D::ensure_shadow_map(u32 resolution) {
    if (_shadow_format == SDL_GPU_TEXTUREFORMAT_INVALID || !_shadow_shader) {
        return false;
    }
    resolution = std::clamp(resolution, 64u, 8192u);
    if (_shadow_map && _shadow_size == resolution) {
        return true;
    }
    try {
        _shadow_map = _device->create_target(resolution, resolution, _shadow_format,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        _shadow_size = resolution;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: no shadow map", (LogFields{{.name = "reason", .value = e.what()}}));
        _shadow_map = {};
        _shadow_size = 0;
        return false;
    }
    return true;
}

void GpuRenderer3D::finish_hdr(SDL_GPUCommandBuffer* commands, GpuView3D& view) {
    const auto pass_into = [&](const GpuTexture& target, SDL_GPUGraphicsPipeline* p, std::span<const SDL_GPUTexture* const> sources,
                               const f32 (&params)[4]) {
        if (!p) {
            return;
        }
        SDL_GPUColorTargetInfo colour{};
        colour.texture = target.handle();
        colour.load_op = SDL_GPU_LOADOP_DONT_CARE; // all of it is written
        colour.store_op = SDL_GPU_STOREOP_STORE;
        colour.cycle = true;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
        if (!pass) {
            return;
        }
        const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(target.width()), static_cast<f32>(target.height()), 0.0f, 1.0f};
        SDL_SetGPUViewport(pass, &viewport);
        SDL_BindGPUGraphicsPipeline(pass, p);
        std::array<SDL_GPUTextureSamplerBinding, 2> bindings{};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            bindings[i] = {.texture = const_cast<SDL_GPUTexture*>(sources[i]), .sampler = _owner->_sampler_linear};
        }
        SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), static_cast<u32>(sources.size()));
        SDL_PushGPUFragmentUniformData(commands, 0, params, sizeof(params));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    };
    const bool bloom = view._options.bloom > 0.0f && view._bloom_a && view._bloom_b;
    if (bloom) {
        SDL_GPUGraphicsPipeline* p = fullscreen(_bloom_shader.handle(), hdr_format);
        const SDL_GPUTexture* scene = view._scene.handle();
        const SDL_GPUTexture* a = view._bloom_a.handle();
        const SDL_GPUTexture* b = view._bloom_b.handle();
        const f32 w = static_cast<f32>(view._bloom_a.width()), h = static_cast<f32>(view._bloom_a.height());
        const f32 extract[4] = {0.0f, view._options.bloom_threshold, 1.0f / static_cast<f32>(view._size.x),
                                1.0f / static_cast<f32>(view._size.y)};
        pass_into(view._bloom_a, p, std::span{&scene, 1}, extract);
        // Twice across and down, the second with taps twice as far apart: a
        // wider, smoother glow for little more work.
        for (int i = 0; i < 2; ++i) {
            const f32 step = static_cast<f32>(i + 1);
            const f32 across[4] = {1.0f, 0.0f, step / w, 0.0f};
            const f32 down[4] = {1.0f, 0.0f, 0.0f, step / h};
            pass_into(view._bloom_b, p, std::span{&a, 1}, across);
            pass_into(view._bloom_a, p, std::span{&b, 1}, down);
        }
    }
    // Out to the 2D pipeline's format: tonemapped and encoded for a gamma
    // one, tonemapped for an sRGB-stored one, left HDR for a float one.
    const SDL_GPUTextureFormat output = view._output_format;
    const f32 mode = output == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ? 0.0f
                   : output == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ? 1.0f
                                                                         : 2.0f;
    const std::array<const SDL_GPUTexture*, 2> sources{view._scene.handle(), bloom ? view._bloom_a.handle() : view._scene.handle()};
    const f32 params[4] = {bloom ? view._options.bloom : 0.0f, view._options.exposure, mode, 0.0f};
    pass_into(view._resolved->texture(), fullscreen(_output_shader.handle(), output), sources, params);
}

bool GpuRenderer3D::render(IView3DBackend& base, const Scene3D& scene, Render3DStats& stats) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view || !_vertex_shader || !_fragment_shader) {
        return false;
    }
    // The 2D pipeline's colour format changed (set_color_space): the view follows.
    if (view->_output_format != _owner->_target_format && !make_targets(*view)) {
        return false;
    }
    const f32 aspect = static_cast<f32>(view->_size.x) / static_cast<f32>(view->_size.y);
    const Scene3D* overlay = scene.overlay_if_any();
    const std::array<const Scene3D*, 2> scenes{&scene, overlay && !overlay->draws().empty() ? overlay : nullptr};
    std::array<Mat4f, 2> view_projections{};
    std::array<GpuFrustum, 2> frustums{};
    for (u32 s = 0; s < 2; ++s) {
        if (scenes[s]) {
            view_projections[s] = scenes[s]->camera.view_projection(aspect);
            frustums[s] = frustum_of(view_projections[s]);
        }
    }

    // The sun's shadow: an orthographic view along its light over the area.
    const Lights3D& lights = scene.lights;
    const Bounds3& area = lights.shadow_area;
    const Vec3f area_size = area.size();
    bool shadows = area_size.x > 0 && area_size.y >= 0 && area_size.z > 0 && length_squared(lights.sun.direction) > 0 &&
                   ensure_shadow_map(lights.shadow_resolution);
    Mat4f light_vp{};
    GpuFrustum light_frustum{};
    f32 texel_world = 0.0f, depth_range = 1.0f;
    if (shadows) {
        const Vec3f to_sun = normalize(lights.sun.direction);
        const Vec3f center = area.center();
        const f32 radius = std::max(area.radius(), 0.01f);
        const Vec3f up = std::abs(to_sun.y) > 0.99f ? Vec3f{0.0f, 0.0f, -1.0f} : Vec3f{0.0f, 1.0f, 0.0f};
        const Mat4f light_view = Mat4f::look_at(center + to_sun * (radius + 1.0f), center, up);
        Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (int i = 0; i < 8; ++i) {
            const Vec3f corner{(i & 1) ? area.max.x : area.min.x, (i & 2) ? area.max.y : area.min.y, (i & 4) ? area.max.z : area.min.z};
            const Vec3f p = light_view.apply(corner);
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        const f32 z_near = std::max(0.01f, -hi.z - 0.5f), z_far = -lo.z + 0.5f;
        light_vp = Mat4f::orthographic(lo.x, hi.x, lo.y, hi.y, z_near, z_far) * light_view;
        light_frustum = frustum_of(light_vp);
        texel_world = std::max(hi.x - lo.x, hi.y - lo.y) / static_cast<f32>(_shadow_size);
        depth_range = std::max(z_far - z_near, 0.01f);
    }

    // The draws of both scenes, each with its instances where the scene keeps them.
    _refs.clear();
    for (u32 s = 0; s < 2; ++s) {
        if (!scenes[s]) {
            continue;
        }
        for (const Scene3D::Draw& draw : scenes[s]->draws()) {
            const auto* mesh = dynamic_cast<const GpuMesh3D*>(draw.mesh.backend().get());
            if (draw.count == 0 || !mesh) {
                continue;
            }
            const Bounds3& b = draw.mesh.bounds();
            const bool caster = shadows && s == 0 && draw.material.blend == Blend3D::Opaque && draw.material.casts_shadow &&
                                !draw.material.wireframe;
            _refs.push_back({&draw, scenes[s]->instances().data() + draw.first, mesh, s,
                             scenes[s]->cull ? &frustums[s] : nullptr, caster, b.center(), b.radius(), 0, 0, 0});
        }
    }

    // Pieces of draws, and tasks of about `chunk` instances each (a big draw
    // split, small ones taken together), for the workers when there are many.
    constexpr u32 chunk = 4096;
    _pieces.clear();
    _tasks.assign(1, 0);
    u32 total = 0, in_task = 0;
    for (u32 r = 0; r < static_cast<u32>(_refs.size()); ++r) {
        const u32 count = _refs[r].draw->count;
        for (u32 begin = 0; begin < count;) {
            const u32 end = std::min(count, begin + (chunk - in_task));
            _pieces.push_back({r, begin, end, total + begin, 0, 0, 0, 0});
            in_task += end - begin;
            begin = end;
            if (in_task == chunk) {
                _tasks.push_back(static_cast<u32>(_pieces.size()));
                in_task = 0;
            }
        }
        total += count;
    }
    if (_tasks.back() != _pieces.size()) {
        _tasks.push_back(static_cast<u32>(_pieces.size()));
    }
    JobSystem* jobs = _owner->_jobs && total >= 2 * chunk ? _owner->_jobs : nullptr;
    const auto each_task = [&](const std::function<void(Piece&)>& body) {
        const auto run = [&](u32 t) {
            for (u32 p = _tasks[t]; p < _tasks[t + 1]; ++p) {
                body(_pieces[p]);
            }
        };
        if (jobs && _tasks.size() > 2) {
            jobs->parallel_for(static_cast<i32>(_tasks.size() - 1), [&](i32 t) { run(static_cast<u32>(t)); });
        } else {
            for (u32 t = 0; t + 1 < _tasks.size(); ++t) {
                run(t);
            }
        }
    };

    // What the camera sees, and what only the sun does (it may cast a shadow
    // into view): each copy's sphere against the planes.
    _visible.resize(total);
    each_task([&](Piece& piece) {
        const DrawRef& ref = _refs[piece.ref];
        piece.seen = piece.extra = 0;
        for (u32 i = piece.begin; i < piece.end; ++i) {
            Vec3f c{};
            f32 r = 0.0f;
            if (ref.frustum || ref.caster) {
                sphere_of(ref.instances[i].transform, ref.center, ref.radius, c, r);
            }
            const bool seen = !ref.frustum || sphere_in(*ref.frustum, c, r);
            const bool sun = !seen && ref.caster && sphere_in(light_frustum, c, r);
            _visible[piece.flat + i - piece.begin] = seen ? 1 : sun ? 2 : 0;
            piece.seen += seen ? 1u : 0u;
            piece.extra += sun ? 1u : 0u;
        }
    });
    // Each draw's copies: those seen, then those only the sun sees.
    for (DrawRef& ref : _refs) {
        ref.out_count = ref.out_extra = 0;
    }
    for (const Piece& piece : _pieces) {
        _refs[piece.ref].out_count += piece.seen;
        _refs[piece.ref].out_extra += piece.extra;
    }
    u32 written = 0;
    for (DrawRef& ref : _refs) {
        ref.out_first = written;
        written += ref.out_count + ref.out_extra;
    }
    {
        u32 current = ~0u, seen = 0, extra = 0;
        for (Piece& piece : _pieces) {
            if (piece.ref != current) {
                current = piece.ref;
                seen = extra = 0;
            }
            const DrawRef& ref = _refs[piece.ref];
            piece.out = ref.out_first + seen;
            piece.out_extra = ref.out_first + ref.out_count + extra;
            seen += piece.seen;
            extra += piece.extra;
        }
    }
    u32 visible_total = 0;
    for (const DrawRef& ref : _refs) {
        visible_total += ref.out_count;
    }
    stats.culled = total - visible_total;

    // The copies, straight into the upload memory; alpha-blended ones furthest first.
    if (written > 0) {
        const u32 bytes = static_cast<u32>(written * sizeof(GpuInstance3D));
        if (!_instances || _instances->size() < bytes) {
            const u32 size = std::max({bytes, _instances ? _instances->size() * 2 : 0u, 64u * 1024u});
            _instances = std::make_shared<GpuBuffer>(
                _device->create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, nullptr, size));
        }
        const auto write = [&](const DrawRef& ref, const MeshInstance& instance, u8* out, u32 at) {
            const Color m = ref.draw->material.color;
            GpuInstance3D g;
            std::memcpy(g.model, instance.transform.m, sizeof(g.model));
            const Mat4f& t = instance.transform;
            const Vec3f c0{t.m[0], t.m[1], t.m[2]}, c1{t.m[4], t.m[5], t.m[6]}, c2{t.m[8], t.m[9], t.m[10]};
            // Cofactors: the inverse transpose times the determinant, made
            // positive so a mirror doesn't turn normals in.
            const Vec3f x = cross(c1, c2);
            const f32 sign = dot(c0, x) < 0.0f ? -1.0f : 1.0f;
            const Vec3f n0 = x * sign, n1 = cross(c2, c0) * sign, n2 = cross(c0, c1) * sign;
            f32 normal[12] = {n0.x, n0.y, n0.z, 0.0f, n1.x, n1.y, n1.z, 0.0f, n2.x, n2.y, n2.z, 0.0f};
            const u32 id = instance.id + 1; // 0: nothing there
            std::memcpy(&normal[3], &id, sizeof(id));
            std::memcpy(g.normal, normal, sizeof(g.normal));
            const Color tint = instance.tint;
            g.color[0] = _srgb_to_linear[m.r] * _srgb_to_linear[tint.r];
            g.color[1] = _srgb_to_linear[m.g] * _srgb_to_linear[tint.g];
            g.color[2] = _srgb_to_linear[m.b] * _srgb_to_linear[tint.b];
            g.color[3] = static_cast<f32>(m.a) / 255.0f * static_cast<f32>(tint.a) / 255.0f;
            std::memcpy(out + static_cast<std::size_t>(at) * sizeof(GpuInstance3D), &g, sizeof(g));
        };
        const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
            each_task([&](Piece& piece) {
                const DrawRef& ref = _refs[piece.ref];
                if (ref.draw->material.blend == Blend3D::Alpha) {
                    return; // sorted below
                }
                u32 at = piece.out, at_extra = piece.out_extra;
                for (u32 i = piece.begin; i < piece.end; ++i) {
                    const u8 v = _visible[piece.flat + i - piece.begin];
                    if (v != 0) {
                        write(ref, ref.instances[i], out.data(), v == 1 ? at++ : at_extra++);
                    }
                }
            });
            for (const Piece& first : _pieces) {
                const DrawRef& ref = _refs[first.ref];
                if (first.begin != 0 || ref.draw->material.blend != Blend3D::Alpha) {
                    continue;
                }
                const Vec3f eye = scenes[ref.scene]->camera.position;
                _sorted.clear();
                const u32 flat = first.flat;
                for (u32 i = 0; i < ref.draw->count; ++i) {
                    if (_visible[flat + i] == 1) {
                        const Mat4f& t = ref.instances[i].transform;
                        _sorted.push_back({length_squared(Vec3f{t.m[12], t.m[13], t.m[14]} - eye), i});
                    }
                }
                std::sort(_sorted.begin(), _sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                u32 at = ref.out_first;
                for (const auto& [d, i] : _sorted) {
                    write(ref, ref.instances[i], out.data(), at++);
                }
            }
        };
        // Cycled: a view rendered earlier this frame keeps the instances it drew.
        _device->upload_storage_buffer(_instances->handle(), 0, bytes, nullptr, /*cycle=*/true, &fill);
    }

    // The draws in order: the scene, then the overlay; in each, opaque, then
    // alpha-blended (furthest first), then additive; then pipeline and mesh.
    const auto pass_of = [](const Material3D& m) {
        return m.wireframe ? Pass::Wire : m.blend == Blend3D::Opaque ? Pass::Opaque : m.blend == Blend3D::Alpha ? Pass::Alpha : Pass::Additive;
    };
    _order.clear();
    for (u32 r = 0; r < static_cast<u32>(_refs.size()); ++r) {
        if (_refs[r].out_count > 0) {
            _order.push_back(r);
        }
    }
    const auto draw_distance = [&](const DrawRef& ref) {
        const Mat4f& t = ref.instances[0].transform;
        return length_squared(Vec3f{t.m[12], t.m[13], t.m[14]} - scenes[ref.scene]->camera.position);
    };
    std::stable_sort(_order.begin(), _order.end(), [&](u32 a, u32 b) {
        const DrawRef& x = _refs[a];
        const DrawRef& y = _refs[b];
        if (x.scene != y.scene) {
            return x.scene < y.scene;
        }
        const Pass px = pass_of(x.draw->material), py = pass_of(y.draw->material);
        if (px != py) {
            return px < py;
        }
        if (px == Pass::Alpha) {
            return draw_distance(x) > draw_distance(y);
        }
        const bool culled_x = !x.draw->material.double_sided, culled_y = !y.draw->material.double_sided;
        if (culled_x != culled_y) {
            return culled_x;
        }
        return x.mesh < y.mesh;
    });

    // Point lights: those reaching the view, strongest and nearest first.
    std::vector<std::pair<f32, const PointLight*>> chosen;
    for (const PointLight& light : lights.points) {
        if (light.radius <= 0 || !sphere_in(frustums[0], light.position, light.radius)) {
            continue;
        }
        const f32 strength = std::max({light.color.x, light.color.y, light.color.z}) * light.radius;
        const f32 distance = length(light.position - scene.camera.position);
        chosen.push_back({strength / std::max(1.0f, distance - light.radius), &light});
    }
    std::sort(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    chosen.resize(std::min<std::size_t>(chosen.size(), max_lights));
    stats.lights = static_cast<u32>(chosen.size());

    // The frame's uniforms, the scene's and the overlay's. The overlay's
    // space is its camera's: the scene's lights are carried into it (seen
    // from where the scene's camera is), without shadows.
    const bool encode = view->_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;     // a gamma target
    const bool decode = _owner->_color_texture_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // textures stored encoded
    const auto frame_uniforms = [&](u32 s) {
        FrameUniforms f{};
        const Mat4f to_space = s == 0 ? Mat4f{} : scenes[1]->camera.view().inverse() * scene.camera.view();
        put(f.sun_direction, normalize(to_space.apply_vector(normalize(lights.sun.direction))), 0.0f);
        put(f.sun_color, lights.sun.color, 0.0f);
        put(f.ambient, lights.ambient, 0.0f);
        put(f.ground_ambient, lights.ground_ambient.value_or(lights.ambient), 0.0f);
        const bool fill = length_squared(lights.fill.direction) > 0;
        put(f.fill_direction, fill ? normalize(to_space.apply_vector(normalize(lights.fill.direction))) : Vec3f{}, 0.0f);
        put(f.fill_color, fill ? lights.fill.color : Vec3f{}, 0.0f);
        const bool lit_by_sun_shadow = shadows && s == 0;
        put(f.flags, {encode ? 1.0f : 0.0f, decode ? 1.0f : 0.0f, lit_by_sun_shadow ? 1.0f : 0.0f},
            static_cast<f32>(chosen.size()));
        std::memcpy(f.shadow_matrix, light_vp.m, sizeof(f.shadow_matrix));
        // A texel's worth of depth toward the sun, and one and a half off the surface.
        put(f.shadow, {texel_world / depth_range, texel_world * 1.5f, _shadow_size > 0 ? 1.0f / static_cast<f32>(_shadow_size) : 0.0f}, 0.0f);
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            put(f.lights + i * 8, to_space.apply(chosen[i].second->position), chosen[i].second->radius);
            put(f.lights + i * 8 + 4, chosen[i].second->color, 0.0f);
        }
        return f;
    };

    // The 2D draws so far first; the passes record between them and the next.
    _owner->ensure_frame();
    _owner->flush_to_frame();
    SDL_GPUCommandBuffer* commands = _owner->_frame->command_buffer();
    const auto resume_2d = [&] { _owner->_batch.begin(_owner->current_target(), _owner->_clear_color, /*do_clear=*/false); };
    SDL_GPUBuffer* storage = _instances ? _instances->handle() : nullptr;

    // Without shadows the shader still samples a map: one that is all far.
    if (!shadows && _no_shadow && !_no_shadow_cleared) {
        SDL_GPUDepthStencilTargetInfo clear{};
        clear.texture = _no_shadow.handle();
        clear.clear_depth = 0.0f;
        clear.load_op = SDL_GPU_LOADOP_CLEAR;
        clear.store_op = SDL_GPU_STOREOP_STORE;
        clear.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        clear.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &clear)) {
            SDL_EndGPURenderPass(pass);
            _no_shadow_cleared = true;
        }
    }

    // The shadow map: casters (seen or casting into view) from the sun.
    if (shadows) {
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = _shadow_map.handle();
        depth.clear_depth = 0.0f; // reverse Z: nothing in the way
        depth.load_op = SDL_GPU_LOADOP_CLEAR;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depth.cycle = true;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
        SDL_GPUGraphicsPipeline* p = pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, false, Pass::Shadow);
        if (pass && p) {
            const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(_shadow_size), static_cast<f32>(_shadow_size), 0.0f, 1.0f};
            SDL_SetGPUViewport(pass, &viewport);
            SDL_BindGPUGraphicsPipeline(pass, p);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &storage, 1);
            DrawUniforms u{};
            std::memcpy(u.view_projection, light_vp.m, sizeof(u.view_projection));
            const GpuMesh3D* bound_mesh = nullptr;
            for (const DrawRef& ref : _refs) {
                const u32 count = ref.out_count + ref.out_extra;
                if (!ref.caster || count == 0) {
                    continue;
                }
                if (ref.mesh != bound_mesh) {
                    const SDL_GPUBufferBinding vertices{ref.mesh->vertices, 0};
                    SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
                    const SDL_GPUBufferBinding indices{ref.mesh->indices, 0};
                    SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    bound_mesh = ref.mesh;
                }
                u.first[0] = ref.out_first;
                SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                SDL_DrawGPUIndexedPrimitives(pass, ref.mesh->index_count, count, 0, 0, 0);
                ++stats.shadow_draws;
            }
        }
        if (pass) {
            SDL_EndGPURenderPass(pass);
        }
    }

    // The scene (and its overlay): into the multisampled colour, resolved into
    // what 2D draws, or into the float scene when HDR.
    // An HDR view holds linear light whatever the 2D pipeline stores.
    SDL_FColor clear = _owner->stored_color(view->_options.background);
    if (view->_hdr) {
        const LinearColor l = to_linear(view->_options.background);
        clear = {l.r, l.g, l.b, l.a};
    }
    clear = {clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a};
    SDL_GPUTexture* resolved = view->_hdr ? view->_scene.handle() : view->_resolved->texture().handle();
    SDL_GPUColorTargetInfo colour{};
    colour.clear_color = clear;
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.cycle = true; // a frame still reading the last picture keeps it
    if (view->_colour) {
        colour.texture = view->_colour.handle();
        colour.store_op = SDL_GPU_STOREOP_RESOLVE;
        colour.resolve_texture = resolved;
        colour.cycle_resolve_texture = true;
    } else {
        colour.texture = resolved;
        colour.store_op = SDL_GPU_STOREOP_STORE;
    }
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = view->_depth.handle();
    depth.clear_depth = 0.0f; // reverse Z: the far plane
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = true;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);
    if (!pass) {
        resume_2d();
        return false;
    }
    // With an overlay, the scene's depths go in [0, 0.9] and the overlay's in
    // [0.9, 1]: nearer (reverse Z) than all of the scene, yet depth-tested
    // against itself, in one pass.
    const bool overlaid = std::any_of(_order.begin(), _order.end(), [&](u32 r) { return _refs[r].scene == 1; });
    const auto begin_scene = [&](u32 s) {
        const f32 lo = s == 0 ? 0.0f : overlay_split, hi = s == 0 && overlaid ? overlay_split : 1.0f;
        const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(view->_size.x), static_cast<f32>(view->_size.y), lo, hi};
        SDL_SetGPUViewport(pass, &viewport);
        const FrameUniforms f = frame_uniforms(s);
        SDL_PushGPUFragmentUniformData(commands, 0, &f, sizeof(f));
    };
    begin_scene(0);

    DrawUniforms draw_uniforms{};
    std::memcpy(draw_uniforms.view_projection, view_projections[0].m, sizeof(draw_uniforms.view_projection));
    const SDL_GPUTextureSamplerBinding shadow_binding{shadows ? _shadow_map.handle() : _no_shadow.handle(), _compare_sampler};
    SDL_GPUGraphicsPipeline* bound = nullptr;
    const GpuMesh3D* bound_mesh = nullptr;
    const ITextureBackend* bound_texture = reinterpret_cast<const ITextureBackend*>(1); // none yet
    u32 current_scene = 0;
    for (const u32 index : _order) {
        const DrawRef& ref = _refs[index];
        const Material3D& material = ref.draw->material;
        if (ref.scene != current_scene) {
            current_scene = ref.scene;
            begin_scene(current_scene);
            std::memcpy(draw_uniforms.view_projection, view_projections[current_scene].m, sizeof(draw_uniforms.view_projection));
        }
        SDL_GPUGraphicsPipeline* p = pipeline(view->_format, view->_samples, !material.double_sided, pass_of(material));
        if (!p) {
            continue;
        }
        const ITextureBackend* texture = material.texture.backend().get();
        if (p != bound) {
            SDL_BindGPUGraphicsPipeline(pass, p);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &storage, 1);
            bound = p;
            bound_texture = reinterpret_cast<const ITextureBackend*>(1);
        }
        if (texture != bound_texture) {
            const auto* gpu = texture && texture->kind() == ITextureBackend::Kind::Gpu ? static_cast<const GpuTextureBackend*>(texture) : nullptr;
            const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
                gpu && gpu->texture() ? SDL_GPUTextureSamplerBinding{gpu->texture().handle(), _owner->sampler_for(*gpu)}
                                      : SDL_GPUTextureSamplerBinding{_owner->_white.handle(), _owner->_sampler_linear},
                shadow_binding};
            SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
            bound_texture = texture;
        }
        if (ref.mesh != bound_mesh) {
            const SDL_GPUBufferBinding vertices{ref.mesh->vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            const SDL_GPUBufferBinding indices{ref.mesh->indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            bound_mesh = ref.mesh;
        }
        draw_uniforms.first[0] = ref.out_first;
        SDL_PushGPUVertexUniformData(commands, 0, &draw_uniforms, sizeof(draw_uniforms));
        const MaterialUniforms m{{material.emissive, material.unlit || material.wireframe ? 1.0f : 0.0f,
                                  material.receives_shadow ? 1.0f : 0.0f, 0.0f}};
        SDL_PushGPUFragmentUniformData(commands, 1, &m, sizeof(m));
        SDL_DrawGPUIndexedPrimitives(pass, ref.mesh->index_count, ref.out_count, 0, 0, 0);
        ++stats.draws;
        stats.instances += ref.out_count;
        stats.triangles += static_cast<u64>(ref.mesh->index_count / 3) * ref.out_count;
    }
    SDL_EndGPURenderPass(pass);

    // What is under each pixel: the scene's opaque and alpha meshes (not its
    // wireframes, glows or overlay) again, writing position and id.
    if (view->_pick) {
        SDL_GPUColorTargetInfo target{};
        target.texture = view->_pick.handle();
        target.clear_color = {0.0f, 0.0f, 0.0f, 0.0f}; // id 0: nothing
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.cycle = true;
        SDL_GPUDepthStencilTargetInfo pick_depth{};
        pick_depth.texture = view->_pick_depth.handle();
        pick_depth.clear_depth = 0.0f;
        pick_depth.load_op = SDL_GPU_LOADOP_CLEAR;
        pick_depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
        pick_depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        pick_depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        pick_depth.cycle = true;
        if (SDL_GPURenderPass* picking = SDL_BeginGPURenderPass(commands, &target, 1, &pick_depth)) {
            const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(view->_size.x), static_cast<f32>(view->_size.y), 0.0f, 1.0f};
            SDL_SetGPUViewport(picking, &viewport);
            DrawUniforms u{};
            std::memcpy(u.view_projection, view_projections[0].m, sizeof(u.view_projection));
            SDL_GPUGraphicsPipeline* bound_pick = nullptr;
            const GpuMesh3D* mesh_bound = nullptr;
            for (const u32 index : _order) {
                const DrawRef& ref = _refs[index];
                const Pass kind = pass_of(ref.draw->material);
                if (ref.scene != 0 || (kind != Pass::Opaque && kind != Pass::Alpha)) {
                    continue;
                }
                SDL_GPUGraphicsPipeline* p = pipeline(pick_format, 1, !ref.draw->material.double_sided, Pass::Pick);
                if (!p) {
                    continue;
                }
                if (p != bound_pick) {
                    SDL_BindGPUGraphicsPipeline(picking, p);
                    SDL_BindGPUVertexStorageBuffers(picking, 0, &storage, 1);
                    bound_pick = p;
                }
                if (ref.mesh != mesh_bound) {
                    const SDL_GPUBufferBinding vertices{ref.mesh->vertices, 0};
                    SDL_BindGPUVertexBuffers(picking, 0, &vertices, 1);
                    const SDL_GPUBufferBinding indices{ref.mesh->indices, 0};
                    SDL_BindGPUIndexBuffer(picking, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    mesh_bound = ref.mesh;
                }
                u.first[0] = ref.out_first;
                SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                SDL_DrawGPUIndexedPrimitives(picking, ref.mesh->index_count, ref.out_count, 0, 0, 0);
            }
            SDL_EndGPURenderPass(picking);
        }
    }
    if (view->_hdr) {
        finish_hdr(commands, *view);
    }
    resume_2d();
    return true;
}

PickResult GpuRenderer3D::pick(IView3DBackend& base, Vec2i pixel) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view || !view->_pick || pixel.x < 0 || pixel.y < 0 || pixel.x >= view->_size.x || pixel.y >= view->_size.y) {
        return {};
    }
    // What is recorded so far goes to the GPU first (2D carries on in a new
    // command buffer, as after a read-back).
    if (_owner->_frame) {
        _owner->flush_to_frame();
        _owner->_frame->submit();
        _owner->end_frame();
    }
    std::vector<u8> texel;
    if (!_device->read_texture_region(view->_pick, static_cast<u32>(pixel.x), static_cast<u32>(pixel.y), 1, 1, 16, texel) ||
        texel.size() < 16) {
        return {};
    }
    f32 values[4];
    std::memcpy(values, texel.data(), sizeof(values));
    u32 id = 0;
    std::memcpy(&id, &values[3], sizeof(id));
    if (id == 0) {
        return {};
    }
    return {.hit = true, .id = id - 1, .position = {values[0], values[1], values[2]}};
}

} // namespace kin::gpu
