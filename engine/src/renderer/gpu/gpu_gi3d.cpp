// The SDL_GPU backend's global illumination (docs/3d_plan.md, G5): the GI scene (G5a), the scene in voxels of
// Scene3D::global_illumination's area, built in compute; the lit scene and the probes (G5b); the debug view and the
// read-back.
#include "embedded_gpu_shader.hpp"
#include "gpu_device.hpp"
#include "gpu_geometry_batch.hpp"
#include "gpu_renderer2d_backend.hpp"
#include "gpu_renderer3d.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <kin/platform/log.hpp>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>

namespace kin::gpu {

namespace {

// A copy as the voxelizer reads it (kin_gi_voxelize.inc.glsl's Copy, std430): 144 bytes.
struct GiCopy {
    f32 model[16];
    f32 normal[12]; // the cofactors' columns, each padded to four (as GpuInstance3D's)
    f32 colour[4];  // the material's colour times the tint, linear
    u32 skin[4];    // x: its first joint matrix
};
static_assert(sizeof(GiCopy) == 144);

// gi_voxelize.comp's and gi_voxelize_attributes.comp's Params.
struct GiVoxelizeUniforms {
    f32 origin[4];
    u32 size[4];
    u32 mesh[4];
    u32 flags[4];
    f32 material[4];
    u32 first[4];
};
static_assert(sizeof(GiVoxelizeUniforms) == 96);

// gi_clear.comp's and gi_flood.comp's.
struct GiFloodUniforms {
    u32 size[4];
    i32 step[4];
};

// kin_gi.inc.glsl's KinGiProbes.
struct GiProbeUniforms {
    f32 origin[4]; // probe 0's place, the spacing
    f32 counts[4]; // probes a side, 1 when they have light to give
    f32 bias[4];   // along the normal, toward the eye (world units), their light's strength
};

// gi_view.frag's View.
struct GiViewUniforms {
    f32 inverse_view_projection[16];
    f32 camera[4];
    f32 forward[4];
    f32 origin[4];
    f32 size[4];
    f32 target[4];
    GiProbeUniforms probes;
};
static_assert(sizeof(GiViewUniforms) == 192);

// gi_light.comp's Params, and a light as it reads them.
struct GiLightUniforms {
    f32 origin[4];
    f32 size[4];
    f32 sun_direction[4];
    f32 sun_colour[4];
    f32 fill_direction[4];
    f32 fill_colour[4];
    GiProbeUniforms probes;
};
struct GiLight {
    f32 at[4];     // place, reach
    f32 colour[4]; // linear, the square of the size
    f32 axis[4];   // a spot's way, the cosine of its outer angle (-2: a point light)
    f32 cone[4];   // 1 / (cos inner - cos outer)
};
static_assert(sizeof(GiLight) == 64);
constexpr u32 max_gi_lights = 256;

// gi_probe_place.comp's Params.
struct GiPlaceUniforms {
    f32 origin[4];
    f32 size[4];
    GiProbeUniforms probes;
    f32 misc[4]; // empty the tiles, the distance past which a probe sleeps (voxels)
};

// gi_probe_trace.comp's Params.
struct GiTraceUniforms {
    f32 origin[4];
    f32 size[4]; // w: the most steps a ray takes
    GiProbeUniforms probes;
    u32 batch[4]; // first, probes, rays a probe, all probes
    f32 rotation[16];
    f32 sky[4]; // strength, the level to read, 1 with an environment
    f32 environment_matrix[16];
    f32 ambient[4];
    f32 ground[4];
};

// gi_probe_update.comp's Params.
struct GiUpdateUniforms {
    GiProbeUniforms probes;
    u32 batch[4];
    f32 rotation[16];
    f32 misc[4]; // hysteresis, the farthest distance kept (world)
};

// The most rays a batch casts (16 bytes each in the rays' buffer).
constexpr u32 max_gi_rays = 1u << 20;

constexpr u32 gi_threads = 64; // every GI compute shader's workgroup

SDL_GPUComputePipeline* make_gi_compute(SDL_GPUDevice* device, std::string_view file, u32 samplers,
                                        u32 readonly_buffers, u32 readwrite_textures, u32 readwrite_buffers,
                                        u32 threads_x = gi_threads, u32 threads_y = 1) {
    const std::span<const unsigned char> code = embedded_gpu_shader(file);
    if (code.empty()) {
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = code.data();
    info.code_size = code.size();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers = samplers;
    info.num_readonly_storage_buffers = readonly_buffers;
    info.num_readwrite_storage_textures = readwrite_textures;
    info.num_readwrite_storage_buffers = readwrite_buffers;
    info.num_uniform_buffers = 1;
    info.threadcount_x = threads_x;
    info.threadcount_y = threads_y;
    info.threadcount_z = 1;
    return SDL_CreateGPUComputePipeline(device, &info);
}

// Workgroups for `count` threads, a row of at most 32768 (SDL_GPU allows 65535 a dimension) and as many rows as that
// takes: the shaders number them back as row * columns + column.
std::array<u32, 2> gi_groups(u64 count) {
    const u64 groups = std::max<u64>((count + gi_threads - 1) / gi_threads, 1);
    const u32 across = static_cast<u32>(std::min<u64>(groups, 32768));
    return {across, static_cast<u32>((groups + across - 1) / across)};
}

// FNV-1a over what a build is made from.
struct GiKey {
    u64 value = 1469598103934665603ull;
    void add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const u8*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value = (value ^ bytes[i]) * 1099511628211ull;
        }
    }
    template <class T> void add(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        add(&v, sizeof(v));
    }
};

f32 half_to_float(u16 h) {
    const u32 sign = static_cast<u32>(h >> 15) << 31;
    const u32 exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    u32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else { // subnormal: normalise
            u32 e = 113, m = mantissa;
            while (!(m & 1024u)) {
                m <<= 1;
                --e;
            }
            bits = sign | (e << 23) | ((m & 1023u) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    f32 f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

f32 srgb_to_linear(u8 c) {
    const f32 v = static_cast<f32>(c) / 255.0f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

} // namespace

void GpuRenderer3D::make_gi() {
    SDL_GPUDevice* handle = _device->handle();
    // Bound where there are no probes (the light variants read three 3D textures whether or not): a probe asleep,
    // giving nothing (never read then).
    try {
        _gi_none =
            _device->create_volume(1, 1, 1, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        const std::array<u8, 8> none{};
        _device->update_texture(_gi_none.handle(), 0, 0, 1, 1, none.data(), 8, true);
    } catch (const std::exception&) {
    }
    try {
        const SDL_GPUTextureUsageFlags usage =
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        for (const SDL_GPUTextureFormat format : {SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                  SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT}) {
            if (!SDL_GPUTextureSupportsFormat(handle, format, SDL_GPU_TEXTURETYPE_3D, usage)) {
                throw std::runtime_error("no 3D storage texture of a format the GI scene needs");
            }
        }
        // The probes' atlases are read and written by their update.
        for (const SDL_GPUTextureFormat format :
             {SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT}) {
            if (!SDL_GPUTextureSupportsFormat(handle, format, SDL_GPU_TEXTURETYPE_3D,
                                              usage | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE)) {
                throw std::runtime_error("no 3D storage texture of a format the probes need");
            }
        }
        _gi_clear = make_gi_compute(handle, "gi_clear.comp.spv", 0, 0, 3, 2);
        _gi_voxelize = make_gi_compute(handle, "gi_voxelize.comp.spv", 2, 5, 0, 2);
        _gi_attributes = make_gi_compute(handle, "gi_voxelize_attributes.comp.spv", 2, 7, 3, 0);
        _gi_flood = make_gi_compute(handle, "gi_flood.comp.spv", 0, 1, 1, 1);
        _gi_light = make_gi_compute(handle, "gi_light.comp.spv", 7, 1, 2, 0);
        _gi_place = make_gi_compute(handle, "gi_probe_place.comp.spv", 1, 0, 3, 0);
        _gi_trace = make_gi_compute(handle, "gi_probe_trace.comp.spv", 7, 0, 0, 1);
        _gi_update = make_gi_compute(handle, "gi_probe_update.comp.spv", 0, 1, 3, 0, 16, 16);
        if (!_gi_clear || !_gi_voxelize || !_gi_attributes || !_gi_flood || !_gi_light || !_gi_place || !_gi_trace ||
            !_gi_update) {
            throw std::runtime_error(std::string{"compute pipelines not made: "} + SDL_GetError());
        }
        _gi_view_shader = GpuShader::from_embedded(*_device, SDL_GPU_SHADERSTAGE_FRAGMENT, "gi_view.frag.spv",
                                                   /*uniform_buffers=*/1, /*samplers=*/9);
        SDL_GPUSamplerCreateInfo sampler{};
        sampler.min_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        _gi_sampler = SDL_CreateGPUSampler(handle, &sampler);
        sampler.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
        _gi_linear = SDL_CreateGPUSampler(handle, &sampler);
        if (!_gi_sampler || !_gi_linear) {
            throw std::runtime_error(std::string{"sampler not made: "} + SDL_GetError());
        }
        // Bound for the skin and the joints of a draw without them (every binding a shader declares is bound).
        const std::array<u8, 64> zeros{};
        _gi_empty = std::make_shared<GpuBuffer>(
            _device->create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, zeros.data(), zeros.size()));
        _gi_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: global illumination not available",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        release_gi_pipelines();
    }
    _capabilities.global_illumination = _gi_available;
}

void GpuRenderer3D::release_gi_pipelines() {
    SDL_GPUDevice* handle = _device->handle();
    for (SDL_GPUComputePipeline** compute :
         {&_gi_clear, &_gi_voxelize, &_gi_attributes, &_gi_flood, &_gi_light, &_gi_place, &_gi_trace, &_gi_update}) {
        if (*compute) {
            SDL_ReleaseGPUComputePipeline(handle, *compute);
            *compute = nullptr;
        }
    }
    for (SDL_GPUSampler** sampler : {&_gi_sampler, &_gi_linear}) {
        if (*sampler) {
            SDL_ReleaseGPUSampler(handle, *sampler);
            *sampler = nullptr;
        }
    }
    _gi_available = false;
}

void GpuRenderer3D::release_gi(GpuView3D& view) {
    view._gi = {};
}

bool GpuRenderer3D::gi_scene(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene,
                             RenderDebugStats3D& stats) {
    const GlobalIllumination3D& gi = scene.global_illumination;
    if (!gi.enabled) {
        return false;
    }
    if (!_gi_available) {
        if (!_gi_warned) {
            KIN_LOG_INFO("render", "3D: global illumination is not available on this device: it is off");
            _gi_warned = true;
        }
        return false;
    }
    const Vec3f extent = gi.area.size();
    if (!(extent.x > 0.0f && extent.y > 0.0f && extent.z > 0.0f) || !std::isfinite(extent.x + extent.y + extent.z) ||
        !std::isfinite(gi.area.min.x + gi.area.min.y + gi.area.min.z) || !(gi.cell > 0.0f) || !std::isfinite(gi.cell)) {
        return false;
    }
    // The level: voxels of `cell`, larger where the area would need more than a level has.
    const f32 side = static_cast<f32>(max_gi_side);
    f32 cell = std::max({gi.cell, extent.x / side, extent.y / side, extent.z / side,
                         std::cbrt(extent.x * extent.y * extent.z / static_cast<f32>(max_gi_voxels))});
    std::array<u32, 3> size{};
    for (;;) {
        const auto count = [&](f32 e) { return std::clamp(static_cast<u32>(std::ceil(e / cell)), 1u, max_gi_side); };
        size = {count(extent.x), count(extent.y), count(extent.z)};
        if (static_cast<u64>(size[0]) * size[1] * size[2] <= max_gi_voxels) {
            break;
        }
        cell *= 1.01f;
    }
    const Vec3f origin = gi.area.min;
    const u64 voxels = static_cast<u64>(size[0]) * size[1] * size[2];
    const bool decode =
        _owner->_color_texture_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // textures stored encoded

    // The draws it takes in, their copies (those that reach into the area; every skinned one) and what they are made
    // of.
    struct Job {
        const Scene3D::Draw* draw;
        const GpuMesh3D* mesh;
        u32 copy_first, copies;
        u32 triangles, index_first, base_vertex;
        u32 item_first;
        bool skinned;
    };
    std::vector<Job> jobs;
    _gi_copy_bytes.clear();
    GiKey key;
    key.add(size);
    key.add(origin);
    key.add(cell);
    key.add(decode);
    const std::span<const Mat4f> joints = scene.joint_matrices();
    const std::span<const u32> skin_offsets = scene.skin_offsets();
    bool any_skinned = false;
    u64 items = 0;
    for (const Scene3D::Draw& draw : scene.draws()) {
        const Material3D& m = draw.material;
        if (draw.count == 0 || m.unlit || m.wireframe || (m.blend != Blend3D::Opaque && m.blend != Blend3D::Mask)) {
            continue;
        }
        const auto* mesh = dynamic_cast<const GpuMesh3D*>(draw.mesh.backend().get());
        if (!mesh || !mesh->vertices || !mesh->indices || mesh->levels.empty() || mesh->levels[0].index_count < 3) {
            continue;
        }
        const bool skinned = draw.skinned && mesh->skin && !joints.empty();
        const MeshInstance3D* instances =
            draw.batch ? draw.batch.instances().data() : scene.instances().data() + draw.first;
        const u32 triangles = mesh->levels[0].index_count / 3;
        const Bounds3D rest = draw.mesh.bounds();
        const Vec3f centre = rest.center();
        const f32 radius = rest.radius();
        Job job{&draw,
                mesh,
                static_cast<u32>(_gi_copy_bytes.size() / sizeof(GiCopy)),
                0,
                triangles,
                static_cast<u32>(mesh->index_offset / sizeof(u32)) + mesh->levels[0].first_index,
                static_cast<u32>(mesh->vertex_offset / sizeof(GpuMeshVertex)),
                0,
                skinned};
        for (u32 i = 0; i < draw.count; ++i) {
            const MeshInstance3D& copy = instances[i];
            const Mat4f& t = copy.transform;
            if (!skinned) {
                // The copy's bounding sphere against the area.
                const Vec3f c0{t.m[0], t.m[1], t.m[2]}, c1{t.m[4], t.m[5], t.m[6]}, c2{t.m[8], t.m[9], t.m[10]};
                const f32 scale = std::sqrt(std::max({length_squared(c0), length_squared(c1), length_squared(c2)}));
                const Vec3f at = t.apply(centre);
                const f32 r = radius * scale;
                if (at.x + r < gi.area.min.x || at.y + r < gi.area.min.y || at.z + r < gi.area.min.z ||
                    at.x - r > gi.area.max.x || at.y - r > gi.area.max.y || at.z - r > gi.area.max.z) {
                    continue;
                }
            }
            if (items + triangles >= 0x7fffffffull) {
                break; // items are numbered in 31 bits (the owners' top bit puts the emissive first)
            }
            GiCopy g{};
            std::memcpy(g.model, t.m, sizeof(g.model));
            const Vec3f c0{t.m[0], t.m[1], t.m[2]}, c1{t.m[4], t.m[5], t.m[6]}, c2{t.m[8], t.m[9], t.m[10]};
            const Vec3f x = cross(c1, c2);
            const f32 sign = dot(c0, x) < 0.0f ? -1.0f : 1.0f; // a mirror does not turn normals in
            const Vec3f n0 = x * sign, n1 = cross(c2, c0) * sign, n2 = cross(c0, c1) * sign;
            const f32 normal[12] = {n0.x, n0.y, n0.z, 0.0f, n1.x, n1.y, n1.z, 0.0f, n2.x, n2.y, n2.z, 0.0f};
            std::memcpy(g.normal, normal, sizeof(g.normal));
            g.colour[0] = srgb_to_linear(m.color.r) * srgb_to_linear(copy.tint.r);
            g.colour[1] = srgb_to_linear(m.color.g) * srgb_to_linear(copy.tint.g);
            g.colour[2] = srgb_to_linear(m.color.b) * srgb_to_linear(copy.tint.b);
            g.colour[3] = static_cast<f32>(m.color.a) / 255.0f * static_cast<f32>(copy.tint.a) / 255.0f;
            if (skinned) {
                const u32 first = skin_offsets[draw.skin_first + i];
                g.skin[0] = first;
                if (first < joints.size()) {
                    const u32 count =
                        std::min<u32>(draw.mesh.skin_joint_count(), static_cast<u32>(joints.size()) - first);
                    key.add(joints.data() + first, count * sizeof(Mat4f));
                }
            }
            const auto* bytes = reinterpret_cast<const u8*>(&g);
            _gi_copy_bytes.insert(_gi_copy_bytes.end(), bytes, bytes + sizeof(g));
            ++job.copies;
            items += triangles;
        }
        if (job.copies == 0) {
            continue;
        }
        job.item_first = static_cast<u32>(items - static_cast<u64>(job.copies) * triangles);
        any_skinned = any_skinned || skinned;
        key.add(mesh->serial);
        key.add(job.copies);
        key.add(m.blend);
        key.add(m.alpha_cutoff);
        key.add(m.emissive);
        key.add(m.double_sided);
        for (const Texture* texture : {&m.texture, &m.emissive_texture}) {
            key.add(texture->backend().get());
        }
        key.add(m.wrap_u);
        key.add(m.wrap_v);
        jobs.push_back(job);
    }
    key.add(_gi_copy_bytes.data(), _gi_copy_bytes.size());

    view._idle_gi = 0;
    stats.gi_voxels = static_cast<u32>(voxels);
    if (view._gi.valid && view._gi.key == key.value) {
        return true; // nothing it is made of changed
    }

    // The textures and the build's words, made for the level's size.
    try {
        if (view._gi.size != size || !view._gi.distance) {
            view._gi = {};
            const SDL_GPUTextureUsageFlags usage =
                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            view._gi.distance =
                _device->create_volume(size[0], size[1], size[2], SDL_GPU_TEXTUREFORMAT_R16_FLOAT, usage);
            view._gi.albedo =
                _device->create_volume(size[0], size[1], size[2], SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, usage);
            view._gi.normal =
                _device->create_volume(size[0], size[1], size[2], SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, usage);
            view._gi.emission =
                _device->create_volume(size[0], size[1], size[2], SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            const SDL_GPUBufferUsageFlags words =
                SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
            view._gi.owners = std::make_shared<GpuBuffer>(
                _device->create_buffer(words, nullptr, static_cast<u32>(voxels * sizeof(u32))));
            view._gi.seeds = std::make_shared<GpuBuffer>(
                _device->create_buffer(words, nullptr, static_cast<u32>(voxels * sizeof(u32))));
            view._gi.size = size;
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the GI scene could not be made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        view._gi = {};
        return false;
    }
    if (!_gi_copy_bytes.empty()) {
        ensure_buffer(_gi_copies, static_cast<u32>(_gi_copy_bytes.size()), 64 * sizeof(GiCopy),
                      SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(_gi_copies->handle(), 0, static_cast<u32>(_gi_copy_bytes.size()),
                                       _gi_copy_bytes.data(), /*cycle=*/true);
    }
    if (any_skinned) {
        const u32 bytes = static_cast<u32>(joints.size_bytes());
        ensure_buffer(_gi_joints, bytes, 64 * sizeof(Mat4f), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(_gi_joints->handle(), 0, bytes, joints.data(), /*cycle=*/true);
    }

    u32 dispatches = 0;
    const GiFloodUniforms whole{{size[0], size[1], size[2], 0}, {0, 0, 0, 0}};
    const std::array<u32, 2> voxel_groups = gi_groups(voxels);
    // 1. Every voxel empty (the textures and the two words are written whole: their old storage can go). The seeds'
    // word holds the faces' ways until the flood.
    {
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.albedo.handle(), .cycle = true},
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.normal.handle(), .cycle = true},
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.emission.handle(), .cycle = true}};
        const std::array<SDL_GPUStorageBufferReadWriteBinding, 2> words{
            SDL_GPUStorageBufferReadWriteBinding{.buffer = view._gi.owners->handle(), .cycle = true},
            SDL_GPUStorageBufferReadWriteBinding{.buffer = view._gi.seeds->handle(), .cycle = true}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, words.data(), 2);
        if (!pass) {
            view._gi.valid = false;
            return false;
        }
        SDL_BindGPUComputePipeline(pass, _gi_clear);
        SDL_PushGPUComputeUniformData(commands, 0, &whole, sizeof(whole));
        SDL_DispatchGPUCompute(pass, voxel_groups[0], voxel_groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }
    // 2 and 3. The owners, then their attributes: a dispatch a draw, with its buffers and textures.
    // Dispatched in the order of the buffers they read: SDL_GPU puts a barrier between dispatches each time another
    // buffer is bound for reading, which leaves the GPU idle between small dispatches. The voxels do not depend on the
    // order (the items are numbered in draw order).
    std::vector<u32> order(jobs.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const Job &x = jobs[a], &y = jobs[b];
        return std::tuple{x.mesh->vertices, x.mesh->indices, x.skinned ? x.mesh->skin : nullptr} <
               std::tuple{y.mesh->vertices, y.mesh->indices, y.skinned ? y.mesh->skin : nullptr};
    });
    const auto voxelize = [&](SDL_GPUComputePass* pass, bool attributes) {
        for (const u32 j : order) {
            const Job& job = jobs[j];
            const Material3D& m = job.draw->material;
            const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
                texture_binding(m.texture, m.wrap_u, m.wrap_v),
                texture_binding(m.emissive_texture, m.wrap_u, m.wrap_v)};
            SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), 2);
            const std::array<SDL_GPUBuffer*, 7> buffers{job.mesh->vertices,
                                                        job.mesh->indices,
                                                        job.skinned ? job.mesh->skin : _gi_empty->handle(),
                                                        job.skinned ? _gi_joints->handle() : _gi_empty->handle(),
                                                        _gi_copies->handle(),
                                                        view._gi.owners->handle(),
                                                        view._gi.seeds->handle()};
            SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), attributes ? 7 : 5);
            GiVoxelizeUniforms u{};
            u.origin[0] = origin.x;
            u.origin[1] = origin.y;
            u.origin[2] = origin.z;
            u.origin[3] = 1.0f / cell;
            u.size[0] = size[0];
            u.size[1] = size[1];
            u.size[2] = size[2];
            u.size[3] = job.item_first;
            u.mesh[0] = job.index_first;
            u.mesh[1] = job.triangles;
            u.mesh[2] = job.base_vertex;
            u.mesh[3] = job.copies;
            const bool emissive_map = static_cast<bool>(m.emissive_texture.backend());
            u.flags[0] = job.skinned ? 1u : 0u;
            u.flags[1] = m.blend == Blend3D::Mask ? 1u : 0u;
            u.flags[2] = decode ? 1u : 0u;
            u.flags[3] = emissive_map ? 1u : 0u;
            u.material[0] = std::max(m.emissive, 0.0f);
            u.material[1] = m.alpha_cutoff;
            u.material[2] = m.double_sided ? 1.0f : 0.0f;
            u.first[0] = job.copy_first;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            const std::array<u32, 2> groups = gi_groups(static_cast<u64>(job.copies) * job.triangles);
            SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
            ++dispatches;
        }
    };
    if (!jobs.empty()) {
        const std::array<SDL_GPUStorageBufferReadWriteBinding, 2> words{
            SDL_GPUStorageBufferReadWriteBinding{.buffer = view._gi.owners->handle(), .cycle = false},
            SDL_GPUStorageBufferReadWriteBinding{.buffer = view._gi.seeds->handle(), .cycle = false}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, words.data(), 2);
        if (!pass) {
            view._gi.valid = false;
            return false;
        }
        SDL_BindGPUComputePipeline(pass, _gi_voxelize);
        voxelize(pass, false);
        SDL_EndGPUComputePass(pass);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.albedo.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.normal.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.emission.handle()}};
        pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, nullptr, 0);
        if (!pass) {
            view._gi.valid = false;
            return false;
        }
        SDL_BindGPUComputePipeline(pass, _gi_attributes);
        voxelize(pass, true);
        SDL_EndGPUComputePass(pass);
    }
    // 4. The flood: the owners seed it, rounds at half the reach each time down to one voxel (and one more at one),
    // then the distances. The owners' and the seeds' words take turns as source and target.
    {
        const u32 longest = std::max({size[0], size[1], size[2]});
        std::vector<i32> rounds;
        for (u32 reach = std::bit_ceil(longest) / 2; reach >= 1; reach /= 2) {
            rounds.push_back(static_cast<i32>(reach));
        }
        rounds.push_back(1);
        SDL_GPUBuffer* source = view._gi.owners->handle();
        SDL_GPUBuffer* target = view._gi.seeds->handle();
        const auto flood = [&](i32 reach, i32 step, bool written) {
            const SDL_GPUStorageTextureReadWriteBinding distance{.texture = view._gi.distance.handle(),
                                                                 .cycle = step == 2};
            const SDL_GPUStorageBufferReadWriteBinding to{.buffer = target, .cycle = written};
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, &distance, 1, &to, 1);
            if (!pass) {
                return false;
            }
            SDL_BindGPUComputePipeline(pass, _gi_flood);
            SDL_BindGPUComputeStorageBuffers(pass, 0, &source, 1);
            GiFloodUniforms u = whole;
            u.step[0] = reach;
            u.step[1] = step;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            SDL_DispatchGPUCompute(pass, voxel_groups[0], voxel_groups[1], 1);
            SDL_EndGPUComputePass(pass);
            ++dispatches;
            if (step != 2) {
                std::swap(source, target);
            }
            return true;
        };
        // The seeds are written whole by the first pass, so their old storage can go; after that each round reads
        // what the last wrote.
        bool ok = flood(0, 0, true);
        for (const i32 reach : rounds) {
            ok = ok && flood(reach, 1, false);
        }
        ok = ok && flood(0, 2, false);
        if (!ok) {
            view._gi.valid = false;
            return false;
        }
    }
    view._gi.origin = origin;
    view._gi.cell = cell;
    view._gi.key = key.value;
    view._gi.valid = true;
    view._gi.placed = false; // the probes move with the scene
    stats.gi_built = true;
    stats.gi_copies = static_cast<u32>(_gi_copy_bytes.size() / sizeof(GiCopy));
    stats.gi_triangles = items;
    stats.gi_dispatches = dispatches;
    return true;
}

namespace {

// A probe grid's uniforms (KinGiProbes): `lit` when they have light to give. The look-up moves out from a surface by
// three tenths of the spacing (past the voxel the probes' rays stop at, a voxel before the surface) and a tenth toward
// the eye.
void probe_uniforms(Vec3f origin, f32 spacing, const std::array<u32, 3>& counts, const GiProbes3D& settings, bool lit,
                    GiProbeUniforms& u) {
    u.origin[0] = origin.x;
    u.origin[1] = origin.y;
    u.origin[2] = origin.z;
    u.origin[3] = spacing;
    for (int a = 0; a < 3; ++a) {
        u.counts[a] = static_cast<f32>(counts[a]);
    }
    u.counts[3] = lit ? 1.0f : 0.0f;
    u.bias[0] = 0.3f * spacing;
    u.bias[1] = 0.1f * spacing;
    u.bias[2] = std::max(settings.intensity, 0.0f);
    u.bias[3] = 0.0f;
}

// A turn for the rays of update `n`, the same each run: an axis and an angle from a hash of n.
Mat4f ray_rotation(u64 n) {
    const auto next = [&n] {
        n += 0x9e3779b97f4a7c15ull;
        u64 z = n;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        z ^= z >> 31;
        return static_cast<f32>(z >> 40) / static_cast<f32>(1u << 24);
    };
    const f32 z = next() * 2.0f - 1.0f, a = next() * 6.2831853f, r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Mat4f::rotation(Quatf::axis_angle({r * std::cos(a), r * std::sin(a), z}, next() * 360.0f));
}

} // namespace

std::array<SDL_GPUTextureSamplerBinding, 3> GpuRenderer3D::gi_bindings(const GpuView3D& view, bool lit) const {
    if (!lit || !view._gi.irradiance) {
        const SDL_GPUTextureSamplerBinding none{_gi_none.handle(), _owner->_sampler_linear};
        return {none, none, none};
    }
    return {SDL_GPUTextureSamplerBinding{view._gi.irradiance.handle(), _gi_linear},
            SDL_GPUTextureSamplerBinding{view._gi.moments.handle(), _gi_linear},
            SDL_GPUTextureSamplerBinding{view._gi.probe_data.handle(), _gi_sampler}};
}

bool GpuRenderer3D::gi_probes(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene,
                              const GpuEnvironment3D* environment, const Mat4f& to_environment,
                              RenderDebugStats3D& stats) {
    GpuView3D::Gi& g = view._gi;
    const GiProbes3D& settings = scene.global_illumination.probes;
    if (!g.valid || !settings.enabled) {
        g.lit = false;
        return false;
    }
    // The grid: a probe every `spacing` voxels from the area's corner to past its far one, further apart where there
    // would be more than max_gi_probes or 128 along a side (the moments' atlas's 2048 texels).
    const std::array<f32, 3> extent{static_cast<f32>(g.size[0]) * g.cell, static_cast<f32>(g.size[1]) * g.cell,
                                    static_cast<f32>(g.size[2]) * g.cell};
    f32 spacing = static_cast<f32>(std::max(settings.spacing, 2u)) * g.cell;
    std::array<u32, 3> counts{};
    for (;;) {
        for (int a = 0; a < 3; ++a) {
            counts[a] = static_cast<u32>(std::ceil(extent[a] / spacing - 1e-3f)) + 1;
        }
        if (static_cast<u64>(counts[0]) * counts[1] * counts[2] <= max_gi_probes &&
            std::max({counts[0], counts[1], counts[2]}) <= 128) {
            break;
        }
        spacing *= 1.1f;
    }
    const u32 total = counts[0] * counts[1] * counts[2];
    const bool fresh = !g.irradiance || g.probe_counts != counts || g.spacing != spacing || g.probe_origin != g.origin;
    try {
        if (fresh) {
            const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                                   SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
                                                   SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
            g.irradiance = _device->create_volume(counts[0] * 8, counts[1] * 8, counts[2],
                                                  SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            g.moments = _device->create_volume(counts[0] * 16, counts[1] * 16, counts[2],
                                               SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, usage);
            g.probe_data = _device->create_volume(counts[0], counts[1], counts[2],
                                                  SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            g.probe_counts = counts;
            g.spacing = spacing;
            g.probe_origin = g.origin;
            g.next = 0;
            g.updates = 0;
            g.placed = false;
            g.lit = false;
        }
        if (!g.radiance || g.radiance.width() != g.size[0] || g.radiance.height() != g.size[1]) {
            for (GpuTexture* t : {&g.radiance, &g.radiance_back}) {
                *t = _device->create_volume(g.size[0], g.size[1], g.size[2], SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
            }
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the GI probes could not be made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        g.irradiance = {};
        g.lit = false;
        return false;
    }
    const f32 per_unit = 1.0f / g.cell;
    const auto put4 = [](f32* to, f32 x, f32 y, f32 z, f32 w) {
        to[0] = x;
        to[1] = y;
        to[2] = z;
        to[3] = w;
    };
    const u64 voxels = static_cast<u64>(g.size[0]) * g.size[1] * g.size[2];
    u32 dispatches = 0;

    // 1. Where the probes stand (after each build of the scene; on a new grid their tiles are emptied first).
    if (!g.placed) {
        GiPlaceUniforms u{};
        put4(u.origin, g.origin.x, g.origin.y, g.origin.z, per_unit);
        put4(u.size, static_cast<f32>(g.size[0]), static_cast<f32>(g.size[1]), static_cast<f32>(g.size[2]), 0.0f);
        probe_uniforms(g.probe_origin, g.spacing, g.probe_counts, settings, false, u.probes);
        put4(u.misc, fresh ? 1.0f : 0.0f, 1.8f * spacing * per_unit, 0.0f, 0.0f);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.probe_data.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.irradiance.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.moments.handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, nullptr, 0);
        if (!pass) {
            return false;
        }
        const SDL_GPUTextureSamplerBinding distance{g.distance.handle(), _gi_linear};
        SDL_BindGPUComputePipeline(pass, _gi_place);
        SDL_BindGPUComputeSamplers(pass, 0, &distance, 1);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(total);
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
        g.placed = true;
    }

    // 2. The lit scene: what leaves each surface voxel, lit by the sun, the fill, the lamps and the probes (as they
    // were: the bounce of the render before).
    const Lights3D& lights = scene.lights;
    std::vector<GiLight> list;
    list.reserve(std::min<std::size_t>(lights.points.size() + lights.spots.size(), max_gi_lights));
    for (const PointLightSource3D& l : lights.points) {
        if (list.size() >= max_gi_lights) {
            break;
        }
        const f32 size = std::max(l.size, 0.01f);
        GiLight q{};
        put4(q.at, l.position.x, l.position.y, l.position.z, std::max(l.radius, 1e-3f));
        put4(q.colour, l.color.r, l.color.g, l.color.b, size * size);
        put4(q.axis, 0.0f, -1.0f, 0.0f, -2.0f);
        list.push_back(q);
    }
    for (const SpotLightSource3D& l : lights.spots) {
        if (list.size() >= max_gi_lights) {
            break;
        }
        const f32 size = std::max(l.size, 0.01f);
        const Vec3f way = length_squared(l.direction) > 0.0f ? normalize(l.direction) : Vec3f{0.0f, -1.0f, 0.0f};
        constexpr f32 to_radians = 3.14159265f / 180.0f;
        const f32 outer = std::min(l.outer_angle, 89.9f), inner = std::clamp(l.inner_angle, 0.0f, outer - 0.1f);
        const f32 cos_outer = std::cos(outer * to_radians), cos_inner = std::cos(inner * to_radians);
        GiLight q{};
        put4(q.at, l.position.x, l.position.y, l.position.z, std::max(l.radius, 1e-3f));
        put4(q.colour, l.color.r, l.color.g, l.color.b, size * size);
        put4(q.axis, way.x, way.y, way.z, cos_outer);
        put4(q.cone, 1.0f / std::max(cos_inner - cos_outer, 1e-4f), 0.0f, 0.0f, 0.0f);
        list.push_back(q);
    }
    const u32 light_bytes = static_cast<u32>(std::max<std::size_t>(list.size(), 1) * sizeof(GiLight));
    ensure_buffer(_gi_lights, light_bytes, 16 * sizeof(GiLight), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
    if (!list.empty()) {
        _device->upload_storage_buffer(_gi_lights->handle(), 0, light_bytes, list.data(), /*cycle=*/true);
    }
    {
        GiLightUniforms u{};
        put4(u.origin, g.origin.x, g.origin.y, g.origin.z, per_unit);
        put4(u.size, static_cast<f32>(g.size[0]), static_cast<f32>(g.size[1]), static_cast<f32>(g.size[2]),
             static_cast<f32>(list.size()));
        const LinearRgb sun = lights.sun.color, fill = lights.fill.color;
        const bool sun_shines = sun.r + sun.g + sun.b > 0.0f && length_squared(lights.sun.direction) > 0.0f;
        const bool fill_shines = fill.r + fill.g + fill.b > 0.0f && length_squared(lights.fill.direction) > 0.0f;
        const Vec3f to_sun = sun_shines ? normalize(lights.sun.direction) : Vec3f{0.0f, 1.0f, 0.0f};
        const Vec3f to_fill = fill_shines ? normalize(lights.fill.direction) : Vec3f{0.0f, 1.0f, 0.0f};
        put4(u.sun_direction, to_sun.x, to_sun.y, to_sun.z, std::clamp(lights.sun_shadow_strength, 0.0f, 1.0f));
        put4(u.sun_colour, sun.r, sun.g, sun.b, sun_shines ? 1.0f : 0.0f);
        put4(u.fill_direction, to_fill.x, to_fill.y, to_fill.z, 0.0f);
        put4(u.fill_colour, fill.r, fill.g, fill.b, fill_shines ? 1.0f : 0.0f);
        probe_uniforms(g.probe_origin, g.spacing, g.probe_counts, settings, g.lit, u.probes);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 2> radiance{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.radiance.handle(), .cycle = true},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.radiance_back.handle(), .cycle = true}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, radiance.data(), 2, nullptr, 0);
        if (!pass) {
            return false;
        }
        const std::array<SDL_GPUTextureSamplerBinding, 3> probes = gi_bindings(view, g.lit);
        const std::array<SDL_GPUTextureSamplerBinding, 7> samplers{
            SDL_GPUTextureSamplerBinding{g.distance.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.albedo.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.normal.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.emission.handle(), _gi_sampler},
            probes[0],
            probes[1],
            probes[2]};
        SDL_GPUBuffer* light_buffer = _gi_lights->handle();
        SDL_BindGPUComputePipeline(pass, _gi_light);
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
        SDL_BindGPUComputeStorageBuffers(pass, 0, &light_buffer, 1);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(voxels);
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }

    // 3. This render's batch of probes casts its rays, 4. and takes them in.
    const u32 rays = std::clamp(settings.rays, 16u, 256u);
    u32 batch = settings.per_render == 0 ? total : std::min(settings.per_render, total);
    batch = std::max(1u, std::min(batch, max_gi_rays / rays));
    const u32 first = g.next % total;
    const Mat4f rotation = ray_rotation(g.updates);
    ensure_buffer(g.rays, batch * rays * 16u, 4096,
                  SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
    {
        GiTraceUniforms u{};
        put4(u.origin, g.origin.x, g.origin.y, g.origin.z, per_unit);
        put4(u.size, static_cast<f32>(g.size[0]), static_cast<f32>(g.size[1]), static_cast<f32>(g.size[2]), 512.0f);
        probe_uniforms(g.probe_origin, g.spacing, g.probe_counts, settings, false, u.probes);
        u.batch[0] = first;
        u.batch[1] = batch;
        u.batch[2] = rays;
        u.batch[3] = total;
        std::memcpy(u.rotation, rotation.m, sizeof(u.rotation));
        if (environment) {
            put4(u.sky, lights.environment_intensity, 0.35f * static_cast<f32>(environment->levels - 1), 1.0f, 0.0f);
            std::memcpy(u.environment_matrix, to_environment.m, sizeof(u.environment_matrix));
        }
        const LinearRgb up = lights.ambient, down = lights.ground_ambient.enabled ? lights.ground_ambient.color : up;
        put4(u.ambient, up.r, up.g, up.b, 0.0f);
        put4(u.ground, down.r, down.g, down.b, 0.0f);
        const SDL_GPUStorageBufferReadWriteBinding out{.buffer = g.rays->handle(), .cycle = true};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &out, 1);
        if (!pass) {
            return false;
        }
        const std::array<SDL_GPUTextureSamplerBinding, 7> samplers{
            SDL_GPUTextureSamplerBinding{g.distance.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.albedo.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.normal.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.radiance.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.probe_data.handle(), _gi_sampler},
            environment ? SDL_GPUTextureSamplerBinding{environment->specular.handle(), _environment_sampler}
                        : SDL_GPUTextureSamplerBinding{_owner->_white.handle(), _owner->_sampler_linear},
            SDL_GPUTextureSamplerBinding{g.radiance_back.handle(), _gi_sampler}};
        SDL_BindGPUComputePipeline(pass, _gi_trace);
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(static_cast<u64>(batch) * rays);
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }
    {
        GiUpdateUniforms u{};
        probe_uniforms(g.probe_origin, g.spacing, g.probe_counts, settings, false, u.probes);
        u.batch[0] = first;
        u.batch[1] = batch;
        u.batch[2] = rays;
        u.batch[3] = total;
        std::memcpy(u.rotation, rotation.m, sizeof(u.rotation));
        put4(u.misc, std::clamp(settings.hysteresis, 0.0f, 0.99f), 1.5f * 1.7320508f * spacing, 0.0f, 0.0f);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.irradiance.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.moments.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.probe_data.handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, nullptr, 0);
        if (!pass) {
            return false;
        }
        SDL_GPUBuffer* ray_buffer = g.rays->handle();
        SDL_BindGPUComputePipeline(pass, _gi_update);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &ray_buffer, 1);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const u32 across = std::min(batch, 32768u);
        SDL_DispatchGPUCompute(pass, across, (batch + across - 1) / across, 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }
    g.next = (first + batch) % total;
    g.updates += batch;
    // Their light is shown once every probe has had an update (before, a probe not yet reached would give black).
    g.lit = g.updates >= total;
    stats.gi_probes = total;
    stats.gi_probes_updated = batch;
    stats.gi_rays = batch * rays;
    stats.gi_dispatches += dispatches;
    stats.gi_lit = g.lit;
    return g.lit;
}

void GpuRenderer3D::draw_gi_view(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const GpuView3D& view,
                                 const Scene3D& scene, const Mat4f& view_projection, SDL_GPUTextureFormat format,
                                 u32 samples, SDL_GPUTextureFormat velocity_format, bool encode) {
    if (!view._gi.valid || !_gi_view_shader || scene.debug.gi_view == GiView3D::Off) {
        return;
    }
    const SDL_GPUSampleCount count = samples >= 8   ? SDL_GPU_SAMPLECOUNT_8
                                     : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4
                                     : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2
                                                    : SDL_GPU_SAMPLECOUNT_1;
    // Over everything the pass drew, without testing or writing the depth; the velocity target, if any, untouched.
    SDL_GPUGraphicsPipeline* p = _owner->_pipelines.get(GpuPipelineKey{.vertex = _fullscreen_shader.handle(),
                                                                       .fragment = _gi_view_shader.handle(),
                                                                       .blend = GpuBlendMode::Replace,
                                                                       .format = format,
                                                                       .layout = GpuVertexLayout::None,
                                                                       .depth_stencil = _depth_format,
                                                                       .depth = GpuDepthTest::None,
                                                                       .samples = count,
                                                                       .extra_format = velocity_format,
                                                                       .extra_write = false});
    if (!p) {
        return;
    }
    GiViewUniforms u{};
    std::memcpy(u.inverse_view_projection, view_projection.inverse().m, sizeof(u.inverse_view_projection));
    const Camera3D& camera = scene.camera;
    const Vec3f forward = camera.forward();
    u.camera[0] = camera.position.x;
    u.camera[1] = camera.position.y;
    u.camera[2] = camera.position.z;
    u.camera[3] = camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f;
    u.forward[0] = forward.x;
    u.forward[1] = forward.y;
    u.forward[2] = forward.z;
    u.origin[0] = view._gi.origin.x;
    u.origin[1] = view._gi.origin.y;
    u.origin[2] = view._gi.origin.z;
    u.origin[3] = 1.0f / view._gi.cell;
    u.size[0] = static_cast<f32>(view._gi.size[0]);
    u.size[1] = static_cast<f32>(view._gi.size[1]);
    u.size[2] = static_cast<f32>(view._gi.size[2]);
    u.size[3] = static_cast<f32>(static_cast<u8>(scene.debug.gi_view));
    u.target[0] = static_cast<f32>(view._size.x);
    u.target[1] = static_cast<f32>(view._size.y);
    u.target[2] = encode ? 1.0f : 0.0f;
    u.target[3] = view._gi.lit ? 1.0f : 0.0f;
    probe_uniforms(view._gi.probe_origin, view._gi.spacing, view._gi.probe_counts, scene.global_illumination.probes,
                   view._gi.lit, u.probes);
    const std::array<SDL_GPUTextureSamplerBinding, 3> probes = gi_bindings(view, view._gi.lit);
    const std::array<SDL_GPUTextureSamplerBinding, 9> textures{
        SDL_GPUTextureSamplerBinding{view._gi.distance.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._gi.albedo.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._gi.normal.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._gi.emission.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._gi.radiance ? view._gi.radiance.handle() : _gi_none.handle(), _gi_sampler},
        probes[0],
        probes[1],
        probes[2],
        SDL_GPUTextureSamplerBinding{view._gi.radiance_back ? view._gi.radiance_back.handle() : _gi_none.handle(),
                                     _gi_sampler}};
    SDL_BindGPUGraphicsPipeline(pass, p);
    SDL_BindGPUFragmentSamplers(pass, 0, textures.data(), static_cast<u32>(textures.size()));
    SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

bool GpuRenderer3D::read_gi_scene(IViewBackend3D& base, GiScene3D& out) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view || !view->_gi.valid) {
        return false;
    }
    // What is recorded so far goes to the GPU first (2D carries on in a new command buffer, as after a read-back).
    if (_owner->_frame) {
        _owner->flush_to_frame();
        _owner->_frame->submit();
        _owner->end_frame();
    }
    const GpuView3D::Gi& gi = view->_gi;
    const u32 depth = gi.size[2];
    std::vector<u8> distance, albedo, normal, emission;
    if (!_device->read_volume(gi.distance, depth, 2, distance) || !_device->read_volume(gi.albedo, depth, 4, albedo) ||
        !_device->read_volume(gi.normal, depth, 4, normal) || !_device->read_volume(gi.emission, depth, 8, emission)) {
        return false;
    }
    const std::size_t voxels = static_cast<std::size_t>(gi.size[0]) * gi.size[1] * gi.size[2];
    out.origin = gi.origin;
    out.cell = gi.cell;
    out.size = gi.size;
    out.distance.resize(voxels);
    out.albedo.resize(voxels);
    out.normal.resize(voxels);
    out.emission.resize(voxels * 3);
    for (std::size_t i = 0; i < voxels; ++i) {
        u16 h;
        std::memcpy(&h, distance.data() + i * 2, sizeof(h));
        out.distance[i] = half_to_float(h);
        std::memcpy(&out.albedo[i], albedo.data() + i * 4, sizeof(u32));
        std::memcpy(&out.normal[i], normal.data() + i * 4, sizeof(u32));
        for (std::size_t c = 0; c < 3; ++c) {
            std::memcpy(&h, emission.data() + i * 8 + c * 2, sizeof(h));
            out.emission[i * 3 + c] = half_to_float(h);
        }
    }
    return true;
}

} // namespace kin::gpu
