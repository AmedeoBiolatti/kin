#include "gpu_renderer3d.hpp"

#include "gpu_device.hpp"
#include "gpu_renderer2d_backend.hpp"

#include <kin/core/jobs.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/color.hpp>

#include <algorithm>
#include <array>
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

// mesh.vert's uniforms, a draw.
struct DrawUniforms {
    f32 view_projection[16];
    u32 first[4]; // x: the draw's first instance
};

// mesh.frag's.
struct MaterialUniforms {
    f32 sun_direction[4];
    f32 sun_color[4];
    f32 ambient[4];
    f32 params[4]; // emissive, unlit, encode to sRGB
};

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
    _capabilities.max_samples_hdr = max_samples(handle, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, _depth_format);
    const char* name = SDL_GetStringProperty(SDL_GetGPUDeviceProperties(handle), SDL_PROP_GPU_DEVICE_NAME_STRING, "");
    KIN_LOG_INFO_F("render", "3D views available",
                   (LogFields{{.name = "device", .value = name ? name : ""},
                              {.name = "depth_bits", .value = std::to_string(_capabilities.depth_bits)},
                              {.name = "max_samples", .value = std::to_string(_capabilities.max_samples)},
                              {.name = "max_samples_hdr", .value = std::to_string(_capabilities.max_samples_hdr)}}));
    try {
        _vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh.vert.spv",
                                                  /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/1);
        _fragment_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh.frag.spv",
                                                    /*uniform_buffers=*/1, /*samplers=*/0);
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: meshes not drawn", (LogFields{{.name = "reason", .value = e.what()}}));
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

SDL_GPUGraphicsPipeline* GpuRenderer3D::pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull) const {
    // Opaque: written over what is there (Replace), alpha and all.
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = _vertex_shader.handle(),
                                                 .fragment = _fragment_shader.handle(),
                                                 .blend = GpuBlendMode::Replace,
                                                 .format = format,
                                                 .layout = GpuVertexLayout::MeshVertices,
                                                 .depth_stencil = _depth_format,
                                                 .depth = GpuDepthTest::TestWrite,
                                                 .cull = cull ? GpuCull::Back : GpuCull::None,
                                                 .samples = sample_count(samples)});
}

std::shared_ptr<IMeshBackend> GpuRenderer3D::create_mesh(const MeshData& data) {
    std::vector<GpuMeshVertex> vertices;
    vertices.reserve(data.vertices.size());
    for (const MeshVertex& v : data.vertices) {
        vertices.push_back({v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y,
                            v.color.r, v.color.g, v.color.b, v.color.a});
    }
    std::vector<u32> indices = data.indices;
    if (indices.empty()) {
        indices.resize(data.vertices.size());
        for (u32 i = 0; i < static_cast<u32>(indices.size()); ++i) {
            indices[i] = i;
        }
    }
    try {
        auto mesh = std::make_shared<GpuMesh3D>(_device->shared());
        const u32 vertex_bytes = static_cast<u32>(vertices.size() * sizeof(GpuMeshVertex));
        const u32 index_bytes = static_cast<u32>(indices.size() * sizeof(u32));
        const SDL_GPUBufferCreateInfo vertex_info{.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vertex_bytes, .props = 0};
        const SDL_GPUBufferCreateInfo index_info{.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = index_bytes, .props = 0};
        mesh->vertices = SDL_CreateGPUBuffer(_device->handle(), &vertex_info);
        mesh->indices = SDL_CreateGPUBuffer(_device->handle(), &index_info);
        if (!mesh->vertices || !mesh->indices) {
            throw std::runtime_error(std::string{"SDL_CreateGPUBuffer failed: "} + SDL_GetError());
        }
        // With the texture uploads: on the GPU before the next frame draws.
        _device->upload_storage_buffer(mesh->vertices, 0, vertex_bytes, vertices.data(), false);
        _device->upload_storage_buffer(mesh->indices, 0, index_bytes, indices.data(), false);
        mesh->index_count = static_cast<u32>(indices.size());
        return mesh;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: mesh not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return {};
    }
}

bool GpuRenderer3D::make_targets(GpuView3D& view) {
    const SDL_GPUTextureFormat format = _owner->_target_format;
    const u32 samples = usable_samples(view._wanted_samples, format);
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    try {
        GpuTexture resolved = _device->create_target(w, h, format,
                                                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        GpuTexture colour = samples > 1
            ? _device->create_target(w, h, format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, sample_count(samples))
            : GpuTexture{};
        GpuTexture depth = _device->create_target(w, h, _depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
                                                  sample_count(samples));
        // Render targets' convention: premultiplied, sampled smoothly. SDL
        // frees the old targets once the GPU is done with them.
        view._resolved = std::make_shared<GpuTextureBackend>(std::move(resolved), /*premultiplied=*/true, ScaleMode::Linear);
        view._colour = std::move(colour);
        view._depth = std::move(depth);
        view._samples = samples;
        view._format = format;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: view targets not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return false;
    }
    // Made now rather than on the first draw: a cold driver cache takes tens of ms.
    pipeline(format, samples, true);
    pipeline(format, samples, false);
    return true;
}

std::shared_ptr<IView3DBackend> GpuRenderer3D::create_view(Vec2i size, const View3DOptions& options) {
    auto view = std::make_shared<GpuView3D>();
    view->_size = size;
    view->_wanted_samples = std::max(1u, options.samples);
    view->_background = options.background;
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

namespace {

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

} // namespace

bool GpuRenderer3D::render(IView3DBackend& base, const Scene3D& scene, Render3DStats& stats) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view || !_vertex_shader || !_fragment_shader) {
        return false;
    }
    // The 2D pipeline's colour format changed (set_color_space): the view follows.
    if (view->_format != _owner->_target_format && !make_targets(*view)) {
        return false;
    }
    const f32 aspect = static_cast<f32>(view->_size.x) / static_cast<f32>(view->_size.y);
    const Scene3D* overlay = scene.overlay_if_any();
    const std::array<const Scene3D*, 2> scenes{&scene, overlay && !overlay->draws().empty() ? overlay : nullptr};
    std::array<Mat4f, 2> view_projections{};
    std::array<GpuFrustum, 2> frustums{};

    // The draws of both, each with its instances where the scene keeps them.
    _refs.clear();
    for (u32 s = 0; s < 2; ++s) {
        if (!scenes[s]) {
            continue;
        }
        view_projections[s] = scenes[s]->camera.view_projection(aspect);
        frustums[s] = frustum_of(view_projections[s]);
        for (const Scene3D::Draw& draw : scenes[s]->draws()) {
            const auto* mesh = dynamic_cast<const GpuMesh3D*>(draw.mesh.backend().get());
            if (draw.count > 0 && mesh) {
                const bool cull = scenes[s]->cull;
                const Bounds3& b = draw.mesh.bounds();
                _refs.push_back({&draw, scenes[s]->instances().data() + draw.first, mesh, s, cull ? &frustums[s] : nullptr,
                                 b.center(), b.radius(), 0, 0});
            }
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
            _pieces.push_back({r, begin, end, total + begin, 0});
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
    const auto each_task = [&](const std::function<void(const Piece&, u32)>& body) {
        const auto run = [&](u32 t) {
            for (u32 p = _tasks[t]; p < _tasks[t + 1]; ++p) {
                body(_pieces[p], p);
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

    // What the cameras see: each copy's sphere (its mesh's, moved and scaled
    // with it) against the planes. Then where each draw's visible copies go.
    _visible.resize(_pieces.size() > 0 ? total : 0);
    std::vector<u32> piece_visible(_pieces.size(), 0);
    each_task([&](const Piece& piece, u32 index) {
        const DrawRef& ref = _refs[piece.ref];
        u32 seen = 0;
        for (u32 i = piece.begin; i < piece.end; ++i) {
            bool in = true;
            if (ref.frustum) {
                const Mat4f& t = ref.instances[i].transform;
                const Vec3f c = t.apply(ref.center);
                const f32 sx = t.m[0] * t.m[0] + t.m[1] * t.m[1] + t.m[2] * t.m[2];
                const f32 sy = t.m[4] * t.m[4] + t.m[5] * t.m[5] + t.m[6] * t.m[6];
                const f32 sz = t.m[8] * t.m[8] + t.m[9] * t.m[9] + t.m[10] * t.m[10];
                const f32 r = ref.radius * std::sqrt(std::max({sx, sy, sz}));
                for (u32 k = 0; k < ref.frustum->count && in; ++k) {
                    const Vec4f& p = ref.frustum->planes[k];
                    in = p.x * c.x + p.y * c.y + p.z * c.z + p.w >= -r;
                }
            }
            _visible[piece.flat + i - piece.begin] = in ? 1 : 0;
            seen += in ? 1u : 0u;
        }
        piece_visible[index] = seen;
    });
    u32 visible_total = 0;
    for (u32 p = 0; p < static_cast<u32>(_pieces.size()); ++p) {
        DrawRef& ref = _refs[_pieces[p].ref];
        if (_pieces[p].begin == 0) {
            ref.out_first = visible_total;
        }
        _pieces[p].out = visible_total;
        visible_total += piece_visible[p];
        ref.out_count += piece_visible[p];
    }
    stats.culled = total - visible_total;

    // The visible copies, straight into the upload memory.
    if (visible_total > 0) {
        const u32 bytes = static_cast<u32>(visible_total * sizeof(GpuInstance3D));
        if (!_instances || _instances->size() < bytes) {
            const u32 size = std::max({bytes, _instances ? _instances->size() * 2 : 0u, 64u * 1024u});
            _instances = std::make_shared<GpuBuffer>(
                _device->create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, nullptr, size));
        }
        const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
            each_task([&](const Piece& piece, u32) {
                const DrawRef& ref = _refs[piece.ref];
                const Color m = ref.draw->material.color;
                const f32 mr = _srgb_to_linear[m.r], mg = _srgb_to_linear[m.g], mb = _srgb_to_linear[m.b];
                const f32 ma = static_cast<f32>(m.a) / 255.0f;
                u32 at = piece.out;
                for (u32 i = piece.begin; i < piece.end; ++i) {
                    if (!_visible[piece.flat + i - piece.begin]) {
                        continue;
                    }
                    const MeshInstance& instance = ref.instances[i];
                    GpuInstance3D g;
                    std::memcpy(g.model, instance.transform.m, sizeof(g.model));
                    const Mat4f& t = instance.transform;
                    const Vec3f c0{t.m[0], t.m[1], t.m[2]}, c1{t.m[4], t.m[5], t.m[6]}, c2{t.m[8], t.m[9], t.m[10]};
                    // Cofactors: the inverse transpose times the determinant, made
                    // positive so a mirror doesn't turn normals in.
                    const Vec3f x = cross(c1, c2);
                    const f32 sign = dot(c0, x) < 0.0f ? -1.0f : 1.0f;
                    const Vec3f n0 = x * sign, n1 = cross(c2, c0) * sign, n2 = cross(c0, c1) * sign;
                    const f32 normal[12] = {n0.x, n0.y, n0.z, 0.0f, n1.x, n1.y, n1.z, 0.0f, n2.x, n2.y, n2.z, 0.0f};
                    std::memcpy(g.normal, normal, sizeof(g.normal));
                    const Color tint = instance.tint;
                    g.color[0] = mr * _srgb_to_linear[tint.r];
                    g.color[1] = mg * _srgb_to_linear[tint.g];
                    g.color[2] = mb * _srgb_to_linear[tint.b];
                    g.color[3] = ma * static_cast<f32>(tint.a) / 255.0f;
                    std::memcpy(out.data() + static_cast<std::size_t>(at++) * sizeof(GpuInstance3D), &g, sizeof(g));
                }
            });
        };
        // Cycled: a view rendered earlier this frame keeps the instances it drew.
        _device->upload_storage_buffer(_instances->handle(), 0, bytes, nullptr, /*cycle=*/true, &fill);
    }

    // The scene, then the overlay; within each, pipeline by pipeline and mesh
    // by mesh: fewer binds.
    _order.clear();
    for (u32 r = 0; r < static_cast<u32>(_refs.size()); ++r) {
        if (_refs[r].out_count > 0) {
            _order.push_back(r);
        }
    }
    std::stable_sort(_order.begin(), _order.end(), [&](u32 a, u32 b) {
        const DrawRef& x = _refs[a];
        const DrawRef& y = _refs[b];
        if (x.scene != y.scene) {
            return x.scene < y.scene;
        }
        const bool culled_x = !x.draw->material.double_sided, culled_y = !y.draw->material.double_sided;
        if (culled_x != culled_y) {
            return culled_x;
        }
        return x.mesh < y.mesh;
    });

    // The 2D draws so far first; the pass records between them and the next.
    _owner->ensure_frame();
    _owner->flush_to_frame();
    SDL_GPUCommandBuffer* commands = _owner->_frame->command_buffer();

    SDL_FColor clear = _owner->stored_color(view->_background);
    clear = {clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a};
    SDL_GPUTexture* resolved = view->_resolved->texture().handle();
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
    // 2D drawing resumes on its target after the pass (a flush leaves the batch without one).
    const auto resume_2d = [&] { _owner->_batch.begin(_owner->current_target(), _owner->_clear_color, /*do_clear=*/false); };
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);
    if (!pass) {
        resume_2d();
        return false;
    }
    // With an overlay, the scene's depths go in [0, 0.9] and the overlay's in
    // [0.9, 1]: nearer (reverse Z) than all of the scene, yet depth-tested
    // against itself, in one pass.
    const bool overlaid = std::any_of(_order.begin(), _order.end(), [&](u32 r) { return _refs[r].scene == 1; });
    constexpr f32 split = 0.9f;
    const auto set_viewport = [&](u32 s) {
        const f32 lo = s == 0 ? 0.0f : split, hi = s == 0 && overlaid ? split : 1.0f;
        const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(view->_size.x), static_cast<f32>(view->_size.y), lo, hi};
        SDL_SetGPUViewport(pass, &viewport);
    };
    set_viewport(0);

    DrawUniforms draw_uniforms{};
    const Vec3f sun = normalize(scene.lights.sun.direction);
    const Vec3f sun_color = scene.lights.sun.color;
    const Vec3f ambient = scene.lights.ambient;
    const bool encode = view->_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // a gamma scene
    MaterialUniforms material{{sun.x, sun.y, sun.z, 0.0f},
                              {sun_color.x, sun_color.y, sun_color.z, 0.0f},
                              {ambient.x, ambient.y, ambient.z, 0.0f},
                              {0.0f, 0.0f, encode ? 1.0f : 0.0f, 0.0f}};

    SDL_GPUBuffer* storage = _instances ? _instances->handle() : nullptr;
    SDL_GPUGraphicsPipeline* bound = nullptr;
    const GpuMesh3D* bound_mesh = nullptr;
    u32 current_scene = 0;
    std::memcpy(draw_uniforms.view_projection, view_projections[0].m, sizeof(draw_uniforms.view_projection));
    for (const u32 index : _order) {
        const DrawRef& ref = _refs[index];
        if (ref.scene != current_scene) {
            current_scene = ref.scene;
            set_viewport(current_scene);
            std::memcpy(draw_uniforms.view_projection, view_projections[current_scene].m, sizeof(draw_uniforms.view_projection));
        }
        SDL_GPUGraphicsPipeline* p = pipeline(view->_format, view->_samples, !ref.draw->material.double_sided);
        if (!p) {
            continue;
        }
        if (p != bound) {
            SDL_BindGPUGraphicsPipeline(pass, p);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &storage, 1);
            bound = p;
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
        material.params[0] = ref.draw->material.emissive;
        material.params[1] = ref.draw->material.unlit ? 1.0f : 0.0f;
        SDL_PushGPUFragmentUniformData(commands, 0, &material, sizeof(material));
        SDL_DrawGPUIndexedPrimitives(pass, ref.mesh->index_count, ref.out_count, 0, 0, 0);
        ++stats.draws;
        stats.instances += ref.out_count;
        stats.triangles += static_cast<u64>(ref.mesh->index_count / 3) * ref.out_count;
    }
    SDL_EndGPURenderPass(pass);
    resume_2d();
    return true;
}

} // namespace kin::gpu
