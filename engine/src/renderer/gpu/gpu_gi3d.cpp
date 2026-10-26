// The SDL_GPU backend's global illumination (docs/3d_plan.md, G5): the GI scene (G5a), the scene in voxels of
// Scene3D::global_illumination's area, built in compute; its debug view and its read-back.
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

// gi_view.frag's View.
struct GiViewUniforms {
    f32 inverse_view_projection[16];
    f32 camera[4];
    f32 forward[4];
    f32 origin[4];
    f32 size[4];
    f32 target[4];
};
static_assert(sizeof(GiViewUniforms) == 144);

constexpr u32 gi_threads = 64; // every GI compute shader's workgroup

SDL_GPUComputePipeline* make_gi_compute(SDL_GPUDevice* device, std::string_view file, u32 samplers,
                                        u32 readonly_buffers, u32 readwrite_textures, u32 readwrite_buffers) {
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
    info.threadcount_x = gi_threads;
    info.threadcount_y = 1;
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
    try {
        const SDL_GPUTextureUsageFlags usage =
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        for (const SDL_GPUTextureFormat format : {SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                                  SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT}) {
            if (!SDL_GPUTextureSupportsFormat(handle, format, SDL_GPU_TEXTURETYPE_3D, usage)) {
                throw std::runtime_error("no 3D storage texture of a format the GI scene needs");
            }
        }
        _gi_clear = make_gi_compute(handle, "gi_clear.comp.spv", 0, 0, 3, 1);
        _gi_voxelize = make_gi_compute(handle, "gi_voxelize.comp.spv", 3, 5, 0, 1);
        _gi_attributes = make_gi_compute(handle, "gi_voxelize_attributes.comp.spv", 3, 6, 3, 0);
        _gi_flood = make_gi_compute(handle, "gi_flood.comp.spv", 0, 1, 1, 1);
        if (!_gi_clear || !_gi_voxelize || !_gi_attributes || !_gi_flood) {
            throw std::runtime_error(std::string{"compute pipelines not made: "} + SDL_GetError());
        }
        _gi_view_shader = GpuShader::from_embedded(*_device, SDL_GPU_SHADERSTAGE_FRAGMENT, "gi_view.frag.spv",
                                                   /*uniform_buffers=*/1, /*samplers=*/4);
        SDL_GPUSamplerCreateInfo sampler{};
        sampler.min_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        _gi_sampler = SDL_CreateGPUSampler(handle, &sampler);
        if (!_gi_sampler) {
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
    for (SDL_GPUComputePipeline** compute : {&_gi_clear, &_gi_voxelize, &_gi_attributes, &_gi_flood}) {
        if (*compute) {
            SDL_ReleaseGPUComputePipeline(handle, *compute);
            *compute = nullptr;
        }
    }
    if (_gi_sampler) {
        SDL_ReleaseGPUSampler(handle, _gi_sampler);
        _gi_sampler = nullptr;
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
            if (items + triangles >= 0xffffffffull) {
                break; // items are numbered in 32 bits
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
        key.add(m.metallic);
        key.add(m.double_sided);
        for (const Texture* texture : {&m.texture, &m.metallic_roughness, &m.emissive_texture}) {
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
    // 1. Every voxel empty (the textures and owners are written whole: their old storage can go).
    {
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.albedo.handle(), .cycle = true},
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.normal.handle(), .cycle = true},
            SDL_GPUStorageTextureReadWriteBinding{.texture = view._gi.emission.handle(), .cycle = true}};
        const SDL_GPUStorageBufferReadWriteBinding owners{.buffer = view._gi.owners->handle(), .cycle = true};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, &owners, 1);
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
            const std::array<SDL_GPUTextureSamplerBinding, 3> samplers{
                texture_binding(m.texture, m.wrap_u, m.wrap_v),
                texture_binding(m.metallic_roughness, m.wrap_u, m.wrap_v),
                texture_binding(m.emissive_texture, m.wrap_u, m.wrap_v)};
            SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), 3);
            const std::array<SDL_GPUBuffer*, 6> buffers{job.mesh->vertices,
                                                        job.mesh->indices,
                                                        job.skinned ? job.mesh->skin : _gi_empty->handle(),
                                                        job.skinned ? _gi_joints->handle() : _gi_empty->handle(),
                                                        _gi_copies->handle(),
                                                        view._gi.owners->handle()};
            SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), attributes ? 6 : 5);
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
            u.material[3] = std::clamp(m.metallic, 0.0f, 1.0f);
            u.first[0] = job.copy_first;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            const std::array<u32, 2> groups = gi_groups(static_cast<u64>(job.copies) * job.triangles);
            SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
            ++dispatches;
        }
    };
    if (!jobs.empty()) {
        const SDL_GPUStorageBufferReadWriteBinding owners{.buffer = view._gi.owners->handle(), .cycle = false};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &owners, 1);
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
    stats.gi_built = true;
    stats.gi_copies = static_cast<u32>(_gi_copy_bytes.size() / sizeof(GiCopy));
    stats.gi_triangles = items;
    stats.gi_dispatches = dispatches;
    return true;
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
    const std::array<SDL_GPUTextureSamplerBinding, 4> textures{
        SDL_GPUTextureSamplerBinding{view._gi.distance.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._gi.albedo.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._gi.normal.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._gi.emission.handle(), _gi_sampler}};
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
