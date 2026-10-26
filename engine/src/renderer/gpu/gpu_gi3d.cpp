// The SDL_GPU backend's global illumination (docs/3d_plan.md, G5): the GI scene (G5a), the scene in voxels of
// Scene3D::global_illumination's area in windows round the camera, made again where it changes (G5c); the lit scene and
// the probes (G5b); the debug view and the read-back.
#include "embedded_gpu_shader.hpp"
#include "gpu_device.hpp"
#include "gpu_geometry_batch.hpp"
#include "gpu_renderer2d_backend.hpp"
#include "gpu_renderer3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <kin/platform/log.hpp>
#include <kin/render3d/skeleton3d.hpp>
#include <map>
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

// kin_gi.inc.glsl's KinGiScene and KinGiProbes.
struct GiSceneUniforms {
    f32 size[4];     // voxels a window, levels
    f32 level[4][4]; // each window's first voxel, its voxel's side
};
struct GiProbeUniforms {
    f32 counts[4];   // probes a window, 1 when they have light to give
    f32 bias[4];     // along the normal and toward the eye (in spacings), their light's strength, levels
    f32 level[4][4]; // each window's first probe, the spacing (world)
};
static_assert(sizeof(GiProbeUniforms) == 96);

// gi_voxelize.comp's and gi_voxelize_attributes.comp's Params.
struct GiVoxelizeUniforms {
    f32 origin[4];
    u32 size[4];
    u32 mesh[4];
    u32 flags[4];
    f32 material[4];
    u32 first[4]; // the draw's first copy, the level's first word of the dirty blocks' bits
    i32 window[4];
    i32 low[4], high[4]; // the box round the blocks to make (the world's voxels)
};
static_assert(sizeof(GiVoxelizeUniforms) == 144);

// gi_clear.comp's.
struct GiClearUniforms {
    u32 size[4]; // voxels a window, blocks in the list
};

// gi_flood.comp's.
struct GiFloodUniforms {
    u32 size[4]; // voxels a window, the level
    i32 window[4];
    i32 box[4];
    i32 extent[4];
    i32 write[4];
    i32 write_extent[4];
    i32 mode[4];
};

// gi_view.frag's View.
struct GiViewUniforms {
    f32 inverse_view_projection[16];
    f32 camera[4];
    f32 forward[4];
    f32 target[4];
    GiSceneUniforms scene;
    GiProbeUniforms probes;
};
static_assert(sizeof(GiViewUniforms) == 288);

// gi_light.comp's Params, and a light as it reads them.
struct GiLightUniforms {
    GiSceneUniforms scene;
    u32 count[4]; // lights, blocks
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
    GiSceneUniforms scene;
    GiProbeUniforms probes;
    u32 count[4];
    f32 misc[4]; // y: the distance past which a probe sleeps (voxels)
};

// gi_probe_trace.comp's Params.
struct GiTraceUniforms {
    GiSceneUniforms scene;
    GiProbeUniforms probes;
    u32 batch[4]; // the most steps a ray takes, the batch's probes, rays a probe
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
    f32 misc[4]; // hysteresis, the farthest distance kept (in spacings)
};

// gi_resolve.comp's Params.
struct GiResolveUniforms {
    f32 to_world[16];
    f32 unproject_xy[4];
    f32 unproject_zw[4];
    f32 sizes[4];
    f32 eye[4];
    f32 forward[4];
    GiProbeUniforms probes;
    f32 last_view_projection[16];
    f32 last_eye[4]; // w: 1 with a render before
    f32 last_forward[4];
};
static_assert(sizeof(GiResolveUniforms) == 336);

// The most rays a batch casts (16 bytes each in the rays' buffer).
constexpr u32 max_gi_rays = 1u << 20;
// The lit scene's turn: each render lights its blocks made again and an eighth of the rest.
constexpr u32 gi_light_turns = 8;
// The flood's rounds (the distances are kept to 8), a pass each.
constexpr std::array<i32, 5> gi_flood_rounds{8, 4, 2, 1, 1};

constexpr u32 gi_threads = 64;                 // the GI compute shaders' workgroup (gi_probe_update's is 16 x 16)
static_assert(gi_run_triangles == gi_threads); // the voxelizer's workgroup takes a run

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

// A hash, eight bytes at a time.
struct GiKey {
    u64 value = 0x9e3779b97f4a7c15ull;
    void add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const u8*>(data);
        std::size_t i = 0;
        for (; i + 8 <= size; i += 8) {
            u64 word;
            std::memcpy(&word, bytes + i, 8);
            mix(word);
        }
        if (i < size) {
            u64 word = 0;
            std::memcpy(&word, bytes + i, size - i);
            mix(word ^ (static_cast<u64>(size - i) << 56));
        }
    }
    void mix(u64 word) {
        value = (value ^ word) * 0xbf58476d1ce4e5b9ull;
        value ^= value >> 31;
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
    static const std::array<f32, 256> table = [] {
        std::array<f32, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const f32 v = static_cast<f32>(i) / 255.0f;
            t[i] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table[c];
}

f32 axis_of(Vec3f v, int a) {
    return a == 0 ? v.x : a == 1 ? v.y : v.z;
}

i32 floor_div(i32 a, i32 b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// `v` wrapped round into [0, s).
u32 wrap(i32 v, i32 s) {
    const i32 r = v % s;
    return static_cast<u32>(r < 0 ? r + s : r);
}

// A box of a level's window, in its blocks of 8 x 8 x 8 (window coordinates), from `lo` up to, not with, `hi`.
struct GiBlocks {
    std::array<i32, 3> lo{}, hi{};
    bool empty() const { return lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2]; }
};

// A debug group round a pass (RenderDebugOptions3D::gpu_pass_markers), as the renderer's own.
struct GiMarker {
    SDL_GPUCommandBuffer* commands;
    GiMarker(SDL_GPUCommandBuffer* buffer, bool enabled, const char* name) : commands(enabled ? buffer : nullptr) {
        if (commands) {
            SDL_PushGPUDebugGroup(commands, name);
        }
    }
    ~GiMarker() {
        if (commands) {
            SDL_PopGPUDebugGroup(commands);
        }
    }
    GiMarker(const GiMarker&) = delete;
    GiMarker& operator=(const GiMarker&) = delete;
};

bool meet(const GiBlocks& a, const GiBlocks& b) {
    return a.lo[0] < b.hi[0] && b.lo[0] < a.hi[0] && a.lo[1] < b.hi[1] && b.lo[1] < a.hi[1] && a.lo[2] < b.hi[2] &&
           b.lo[2] < a.hi[2];
}

// The box round both.
GiBlocks round_both(const GiBlocks& a, const GiBlocks& b) {
    GiBlocks r;
    for (int i = 0; i < 3; ++i) {
        r.lo[i] = std::min(a.lo[i], b.lo[i]);
        r.hi[i] = std::max(a.hi[i], b.hi[i]);
    }
    return r;
}

GiBlocks grown(GiBlocks b, i32 by, const std::array<i32, 3>& limit) {
    for (int a = 0; a < 3; ++a) {
        b.lo[a] = std::max(b.lo[a] - by, 0);
        b.hi[a] = std::min(b.hi[a] + by, limit[a]);
    }
    return b;
}

// At most `most` boxes round `boxes`: those whose centres are in one cell of a grid (8 units a side, doubled till there
// are few enough cells) made one, the box round them. Many small changes far apart stay many small boxes, not one round
// them all.
void gather_boxes(std::vector<Bounds3D>& boxes, std::size_t most) {
    for (f32 side = 8.0f; boxes.size() > most; side *= 2.0f) {
        std::map<std::array<i64, 3>, Bounds3D> cells;
        for (const Bounds3D& b : boxes) {
            const Vec3f c = (b.min + b.max) * (0.5f / side);
            const std::array<i64, 3> key{static_cast<i64>(std::floor(c.x)), static_cast<i64>(std::floor(c.y)),
                                         static_cast<i64>(std::floor(c.z))};
            const auto [at, fresh] = cells.try_emplace(key, b);
            if (!fresh) {
                Bounds3D& all = at->second;
                all = {{std::min(all.min.x, b.min.x), std::min(all.min.y, b.min.y), std::min(all.min.z, b.min.z)},
                       {std::max(all.max.x, b.max.x), std::max(all.max.y, b.max.y), std::max(all.max.z, b.max.z)}};
            }
        }
        boxes.clear();
        for (const auto& [key, b] : cells) {
            boxes.push_back(b);
        }
    }
}

bool overlaps(const Bounds3D& a, const Bounds3D& b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y && a.min.z <= b.max.z &&
           b.min.z <= a.max.z;
}

// The turn for the rays of update `n`, the same each run: an axis and an angle from a hash of n.
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
        _gi_clear = make_gi_compute(handle, "gi_clear.comp.spv", 0, 1, 3, 2);
        _gi_voxelize = make_gi_compute(handle, "gi_voxelize.comp.spv", 2, 6, 0, 2);
        _gi_attributes = make_gi_compute(handle, "gi_voxelize_attributes.comp.spv", 2, 8, 3, 0);
        _gi_flood = make_gi_compute(handle, "gi_flood.comp.spv", 1, 1, 1, 1);
        _gi_light = make_gi_compute(handle, "gi_light.comp.spv", 7, 2, 2, 0);
        _gi_place = make_gi_compute(handle, "gi_probe_place.comp.spv", 1, 1, 3, 0);
        _gi_trace = make_gi_compute(handle, "gi_probe_trace.comp.spv", 7, 1, 0, 1);
        _gi_update = make_gi_compute(handle, "gi_probe_update.comp.spv", 0, 2, 3, 0, 16, 16);
        _gi_resolve = make_gi_compute(handle, "gi_resolve.comp.spv", 7, 0, 2, 0, 8, 8);
        if (!_gi_clear || !_gi_voxelize || !_gi_attributes || !_gi_flood || !_gi_light || !_gi_place || !_gi_trace ||
            !_gi_update || !_gi_resolve) {
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
    for (SDL_GPUComputePipeline** compute : {&_gi_clear, &_gi_voxelize, &_gi_attributes, &_gi_flood, &_gi_light,
                                             &_gi_place, &_gi_trace, &_gi_update, &_gi_resolve}) {
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

void GpuRenderer3D::gi_uniforms(const GpuView3D& view, f32 intensity, bool lit, f32* scene, f32* probes) const {
    const GpuView3D::Gi& g = view._gi;
    if (scene) {
        GiSceneUniforms u{};
        u.size[0] = static_cast<f32>(g.size[0]);
        u.size[1] = static_cast<f32>(g.size[1]);
        u.size[2] = static_cast<f32>(g.size[2]);
        u.size[3] = static_cast<f32>(g.levels);
        for (u32 k = 0; k < g.levels; ++k) {
            for (int a = 0; a < 3; ++a) {
                u.level[k][a] = static_cast<f32>(g.origin[k][a]);
            }
            u.level[k][3] = g.cell * static_cast<f32>(1u << k);
        }
        std::memcpy(scene, &u, sizeof(u));
    }
    if (probes) {
        GiProbeUniforms u{};
        for (int a = 0; a < 3; ++a) {
            u.counts[a] = static_cast<f32>(g.probe_counts[a]);
        }
        u.counts[3] = lit ? 1.0f : 0.0f;
        // The look-up moves out from a surface by three tenths of the spacing (past the voxel the probes' rays stop at,
        // a voxel before the surface) and a tenth toward the eye.
        u.bias[0] = 0.3f;
        u.bias[1] = 0.1f;
        u.bias[2] = std::max(intensity, 0.0f);
        u.bias[3] = static_cast<f32>(g.levels);
        for (u32 k = 0; k < g.levels; ++k) {
            for (int a = 0; a < 3; ++a) {
                u.level[k][a] = static_cast<f32>(g.probe_origin[k][a]);
            }
            u.level[k][3] = static_cast<f32>(g.spacing) * g.cell * static_cast<f32>(1u << k);
        }
        std::memcpy(probes, &u, sizeof(u));
    }
}

bool GpuRenderer3D::gi_scene(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene,
                             RenderDebugStats3D& stats) {
    const GlobalIllumination3D& gi = scene.global_illumination;
    GpuView3D::Gi& g = view._gi;
    g.made.clear();
    g.rewritten.clear();
    if (!gi.enabled) {
        return false;
    }
    view._idle_gi = 0;
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
    // A window's voxels: multiples of 8, within the limits.
    const auto round8 = [](u32 v) { return std::clamp<u32>((v + 7) / 8 * 8, 32, max_gi_side); };
    std::array<u32, 3> size{round8(gi.side), round8(gi.height), round8(gi.side)};
    while (static_cast<u64>(size[0]) * size[1] * size[2] > max_gi_voxels) {
        if (size[1] > 32) {
            size[1] -= 8;
        } else {
            size[0] -= 8;
            size[2] -= 8;
        }
    }
    // The levels: windows round the camera (snapped to whole blocks), each next level's voxels twice the last's, up to
    // the first that holds the whole area, which stays over it (the first: a window only as big as the area).
    const f32 cell = gi.cell;
    const u32 wanted = std::clamp(gi.levels, 1u, max_gi_levels);
    std::array<std::array<i32, 3>, max_gi_levels> origin{};
    u32 levels = 0;
    const Vec3f camera = scene.camera.position;
    for (u32 k = 0; k < wanted; ++k) {
        const f32 c = cell * static_cast<f32>(1u << k);
        std::array<i32, 3> lo{}, need{};
        bool holds = true;
        for (int a = 0; a < 3; ++a) {
            lo[a] = floor_div(static_cast<i32>(std::floor(axis_of(gi.area.min, a) / c)), 8) * 8;
            need[a] = static_cast<i32>(std::ceil(axis_of(gi.area.max, a) / c)) - lo[a];
            holds = holds && need[a] <= static_cast<i32>(size[a]);
        }
        levels = k + 1;
        if (holds) {
            if (k == 0) {
                for (int a = 0; a < 3; ++a) {
                    size[a] = static_cast<u32>(std::max(8, (need[a] + 7) / 8 * 8));
                }
            }
            origin[k] = lo;
            break;
        }
        for (int a = 0; a < 3; ++a) {
            origin[k][a] = 8 * static_cast<i32>(std::lround((axis_of(camera, a) / c - size[a] * 0.5f) / 8.0f));
        }
    }
    const bool decode =
        _owner->_color_texture_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // textures stored encoded
    GiKey settings;
    settings.add(size);
    settings.add(levels);
    settings.add(cell);
    settings.add(decode);
    settings.add(gi.area);
    const std::array<i32, 3> blocks{static_cast<i32>(size[0] / 8), static_cast<i32>(size[1] / 8),
                                    static_cast<i32>(size[2] / 8)};
    const u32 per_level = static_cast<u32>(blocks[0] * blocks[1] * blocks[2]);
    const u32 words = (per_level + 31) / 32;
    const u64 window_voxels = static_cast<u64>(size[0]) * size[1] * size[2];
    const bool whole = !g.valid || g.key != settings.value;
    try {
        if (whole && (g.size != size || g.levels != levels || !g.distance)) {
            const std::vector<std::pair<u64, Bounds3D>> none;
            g = {};
            const SDL_GPUTextureUsageFlags usage =
                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            const u32 depth = size[2] * levels;
            g.distance = _device->create_volume(size[0], size[1], depth, SDL_GPU_TEXTUREFORMAT_R16_FLOAT, usage);
            g.albedo = _device->create_volume(size[0], size[1], depth, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, usage);
            g.normal = _device->create_volume(size[0], size[1], depth, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, usage);
            g.emission =
                _device->create_volume(size[0], size[1], depth, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            const SDL_GPUBufferUsageFlags words_usage =
                SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
            const u32 bytes = static_cast<u32>(window_voxels * levels * sizeof(u32));
            g.owners = std::make_shared<GpuBuffer>(_device->create_buffer(words_usage, nullptr, bytes));
            g.seeds = std::make_shared<GpuBuffer>(_device->create_buffer(words_usage, nullptr, bytes));
            g.size = size;
            g.levels = levels;
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the GI scene could not be made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        g = {};
        return false;
    }

    // The draws it takes in and their copies that reach into the area (the skinned in their pose), each with a hash of
    // all it is made of and its box.
    struct Job {
        const Scene3D::Draw* draw;
        const GpuMesh3D* mesh;
        u32 triangles, index_first, base_vertex;
        bool skinned;
    };
    struct Copy {
        u32 job;
        const MeshInstance3D* instance;
        u32 skin; // its first joint matrix
        Bounds3D box;
        u64 hash;
    };
    std::vector<Job> jobs;
    std::vector<Copy> copies;
    const std::span<const Mat4f> joints = scene.joint_matrices();
    const std::span<const u32> skin_offsets = scene.skin_offsets();
    bool any_skinned = false;
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
        GiKey material;
        material.add(mesh->serial);
        material.add(m.color);
        material.add(m.blend);
        material.add(m.alpha_cutoff);
        material.add(m.emissive);
        material.add(m.double_sided);
        for (const Texture* texture : {&m.texture, &m.emissive_texture}) {
            material.add(texture->backend().get());
        }
        material.add(m.wrap_u);
        material.add(m.wrap_v);
        const u32 job = static_cast<u32>(jobs.size());
        const Bounds3D rest = draw.mesh.bounds();
        bool used = false;
        for (u32 i = 0; i < draw.count; ++i) {
            const MeshInstance3D& copy = instances[i];
            const Mat4f& t = copy.transform;
            Copy c{job, &copy, 0, {}, 0};
            GiKey key = material;
            key.add(t);
            key.add(copy.tint);
            if (skinned) {
                // A posed copy's box: the sphere round its joints' moved spheres (skin_sphere), else its rest box.
                const u32 first = skin_offsets[draw.skin_first + i];
                c.skin = first;
                const u32 count = first < joints.size() ? std::min<u32>(draw.mesh.skin_joint_count(),
                                                                        static_cast<u32>(joints.size()) - first)
                                                        : 0u;
                if (count > 0) {
                    key.add(joints.data() + first, count * sizeof(Mat4f));
                }
                Vec3f centre = rest.center();
                f32 radius = rest.radius();
                if (count > 0) {
                    skin_sphere(mesh->skin_bounds, {joints.data() + first, count}, centre, radius);
                }
                const Vec3f at = t.apply(centre);
                const f32 r = radius * stretch_bound(t);
                c.box = {at - Vec3f{r, r, r}, at + Vec3f{r, r, r}};
            } else {
                // The rest box's centre moved, and its half-size through the absolute values of the transform.
                const Vec3f at = t.apply(rest.center()), half = (rest.max - rest.min) * 0.5f;
                const Vec3f reach{std::abs(t.m[0]) * half.x + std::abs(t.m[4]) * half.y + std::abs(t.m[8]) * half.z,
                                  std::abs(t.m[1]) * half.x + std::abs(t.m[5]) * half.y + std::abs(t.m[9]) * half.z,
                                  std::abs(t.m[2]) * half.x + std::abs(t.m[6]) * half.y + std::abs(t.m[10]) * half.z};
                c.box = {at - reach, at + reach};
            }
            if (!overlaps(c.box, gi.area)) {
                continue;
            }
            c.hash = key.value;
            copies.push_back(c);
            used = true;
        }
        if (used) {
            any_skinned = any_skinned || skinned;
            jobs.push_back({&draw, mesh, mesh->levels[0].index_count / 3,
                            static_cast<u32>(mesh->index_offset / sizeof(u32)) + mesh->levels[0].first_index,
                            static_cast<u32>(mesh->vertex_offset / sizeof(GpuMeshVertex)), skinned});
        }
    }
    // A copy as the voxelizer reads it (made only for those voxelized).
    const auto record_of = [&](const Copy& c) {
        GiCopy record{};
        const Mat4f& t = c.instance->transform;
        const Material3D& m = jobs[c.job].draw->material;
        std::memcpy(record.model, t.m, sizeof(record.model));
        const Vec3f c0{t.m[0], t.m[1], t.m[2]}, c1{t.m[4], t.m[5], t.m[6]}, c2{t.m[8], t.m[9], t.m[10]};
        const Vec3f x = cross(c1, c2);
        const f32 sign = dot(c0, x) < 0.0f ? -1.0f : 1.0f; // a mirror does not turn normals in
        const Vec3f n0 = x * sign, n1 = cross(c2, c0) * sign, n2 = cross(c0, c1) * sign;
        const f32 normal[12] = {n0.x, n0.y, n0.z, 0.0f, n1.x, n1.y, n1.z, 0.0f, n2.x, n2.y, n2.z, 0.0f};
        std::memcpy(record.normal, normal, sizeof(record.normal));
        const Color tint = c.instance->tint;
        record.colour[0] = srgb_to_linear(m.color.r) * srgb_to_linear(tint.r);
        record.colour[1] = srgb_to_linear(m.color.g) * srgb_to_linear(tint.g);
        record.colour[2] = srgb_to_linear(m.color.b) * srgb_to_linear(tint.b);
        record.colour[3] = static_cast<f32>(m.color.a) / 255.0f * static_cast<f32>(tint.a) / 255.0f;
        record.skin[0] = c.skin;
        return record;
    };
    // What changed: the copies that came or went (a copy that moved or changed is one of each), by their hashes.
    std::vector<std::pair<u64, Bounds3D>> now;
    now.reserve(copies.size());
    for (const Copy& c : copies) {
        now.emplace_back(c.hash, c.box);
    }
    std::sort(now.begin(), now.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Bounds3D> changed;
    if (!whole) {
        std::size_t i = 0, j = 0;
        while (i < g.copies.size() || j < now.size()) {
            if (j == now.size() || (i < g.copies.size() && g.copies[i].first < now[j].first)) {
                changed.push_back(g.copies[i++].second);
            } else if (i == g.copies.size() || now[j].first < g.copies[i].first) {
                changed.push_back(now[j++].second);
            } else {
                ++i;
                ++j;
            }
        }
        gather_boxes(changed, 64);
    }

    // The blocks to make again, each level's: all of them on a whole build, else those its window gained (the camera
    // moved) and those a change reaches into (its box, a voxel more each way). Each as a bit a stored block, a list
    // entry (stored x, y, z and the level), and a box of window blocks (a core). And the floods that give the distances
    // again: round each core, written over it and a block more (the voxels whose nearest surface may have come or
    // gone), over two blocks more (the seeds a voxel a block out can be nearest to: the distances are kept to 8); and
    // where a window moved, at the side it left, the block along its new edge (whose nearest surfaces may have been
    // past it), over two.
    std::vector<u32> dirty(static_cast<std::size_t>(words) * levels, 0u);
    std::vector<u32>& made = g.made;
    std::array<std::vector<GiBlocks>, max_gi_levels> cores;
    struct Flood {
        GiBlocks write, over;
    };
    std::array<std::vector<Flood>, max_gi_levels> floods;
    const auto mark = [&](u32 k, const GiBlocks& b) {
        if (b.empty()) {
            return;
        }
        cores[k].push_back(b);
        floods[k].push_back({grown(b, 1, blocks), grown(b, 2, blocks)});
        for (i32 z = b.lo[2]; z < b.hi[2]; ++z) {
            for (i32 y = b.lo[1]; y < b.hi[1]; ++y) {
                for (i32 x = b.lo[0]; x < b.hi[0]; ++x) {
                    const std::array<i32, 3> local{x, y, z};
                    std::array<u32, 3> st{};
                    for (int a = 0; a < 3; ++a) {
                        st[a] = wrap(origin[k][a] / 8 + local[a], blocks[a]);
                    }
                    const u32 index =
                        st[0] + static_cast<u32>(blocks[0]) * (st[1] + static_cast<u32>(blocks[1]) * st[2]);
                    u32& word = dirty[static_cast<std::size_t>(k) * words + index / 32];
                    if (!(word & (1u << (index % 32)))) {
                        word |= 1u << (index % 32);
                        made.push_back(st[0] | (st[1] << 8) | (st[2] << 16) | (k << 24));
                    }
                }
            }
        }
    };
    // A change that does not go on from one in the last four renders (a game may render more often than it steps, so
    // that something moving does not change in every render).
    const bool recent = g.ticks - g.last_change_tick <= 4;
    const bool started = std::any_of(changed.begin(), changed.end(), [&](const Bounds3D& b) {
        return !recent || std::none_of(g.last_changes.begin(), g.last_changes.end(),
                                       [&](const Bounds3D& before) { return overlaps(b, before); });
    });
    for (u32 k = 0; k < levels; ++k) {
        const f32 c = cell * static_cast<f32>(1u << k);
        if (whole) {
            mark(k, {{0, 0, 0}, blocks});
            g.waiting_changes[k].clear();
            continue;
        }
        // The slabs the window gained, an axis at a time.
        const std::array<i32, 3> old = g.origin[k];
        bool jumped = false;
        for (int a = 0; a < 3; ++a) {
            jumped = jumped || std::abs(origin[k][a] - old[a]) >= static_cast<i32>(size[a]);
        }
        if (jumped) {
            mark(k, {{0, 0, 0}, blocks});
            continue;
        }
        for (int a = 0; a < 3; ++a) {
            const i32 moved = (origin[k][a] - old[a]) / 8;
            if (moved == 0) {
                continue;
            }
            GiBlocks slab{{0, 0, 0}, blocks}, edge = slab, over = slab;
            if (moved > 0) {
                slab.lo[a] = blocks[a] - moved;
                edge.hi[a] = 1;
                over.hi[a] = 2;
            } else {
                slab.hi[a] = -moved;
                edge.lo[a] = blocks[a] - 1;
                over.lo[a] = blocks[a] - 2;
            }
            mark(k, slab);
            floods[k].push_back({edge, over});
        }
        // What changed, kept till its level's turn: the first level's at once after a render in which nothing changed
        // there, else (something that keeps moving: a character's pose, a falling prop) every fourth render; level k's
        // (further out, coarser) every 4 x 2^k renders, the levels' turns in different renders.
        std::vector<Bounds3D>& waiting = g.waiting_changes[k];
        for (const Bounds3D& b : changed) {
            // Not one a box already waiting holds (an animation's, render after render).
            const bool held = std::any_of(waiting.begin(), waiting.end(), [&](const Bounds3D& w) {
                return w.min.x <= b.min.x && w.min.y <= b.min.y && w.min.z <= b.min.z && b.max.x <= w.max.x &&
                       b.max.y <= w.max.y && b.max.z <= w.max.z;
            });
            if (!held) {
                waiting.push_back(b);
            }
        }
        gather_boxes(waiting, 64);
        const bool turn = (k == 0 && started) || g.ticks % (4u << k) == (2u << k) - 2u;
        if (!turn) {
            continue;
        }
        for (const Bounds3D& b : waiting) {
            GiBlocks box;
            for (int a = 0; a < 3; ++a) {
                const i32 lo = static_cast<i32>(std::floor(axis_of(b.min, a) / c)) - 1 - origin[k][a];
                const i32 hi = static_cast<i32>(std::ceil(axis_of(b.max, a) / c)) + 1 - origin[k][a];
                box.lo[a] = std::max(floor_div(lo, 8), 0);
                box.hi[a] = std::min(floor_div(hi + 7, 8), blocks[a]);
            }
            mark(k, box);
        }
        waiting.clear();
    }
    if (!changed.empty()) {
        g.last_changes = std::move(changed);
        g.last_change_tick = g.ticks;
    }
    ++g.ticks;
    const u64 total_voxels = window_voxels * levels;
    stats.gi_voxels = static_cast<u32>(total_voxels);
    stats.gi_levels = levels;
    g.origin = origin;
    g.cell = cell;
    g.key = settings.value;
    g.copies = std::move(now);
    if (made.empty()) {
        g.valid = true;
        return true; // nothing it is made of changed, and the windows stayed
    }

    // Each level's jobs: the copies that reach into one of its cores (a voxel more each way), numbered in draw order,
    // and of each the runs of triangles that do (`pairs`: the copy in the job, the run; a skinned copy's all of them,
    // its runs' boxes being at rest).
    struct LevelJob {
        u32 job, copy_first, copies, item_first, pair_first, pairs;
    };
    std::array<std::vector<LevelJob>, max_gi_levels> level_jobs;
    std::vector<GiCopy> records;
    std::vector<u32> pairs;
    u64 voxelized = 0;
    for (u32 k = 0; k < levels; ++k) {
        const f32 c = cell * static_cast<f32>(1u << k);
        std::vector<Bounds3D> regions;
        for (const GiBlocks& b : cores[k]) {
            Vec3f lo{}, hi{};
            for (int a = 0; a < 3; ++a) {
                const f32 l = static_cast<f32>(origin[k][a] + b.lo[a] * 8 - 1) * c;
                const f32 h = static_cast<f32>(origin[k][a] + b.hi[a] * 8 + 1) * c;
                (a == 0 ? lo.x : a == 1 ? lo.y : lo.z) = l;
                (a == 0 ? hi.x : a == 1 ? hi.y : hi.z) = h;
            }
            regions.push_back({lo, hi});
        }
        u64 items = 0;
        std::size_t at = 0;
        std::vector<Bounds3D> local;
        while (at < copies.size()) {
            const u32 job = copies[at].job;
            const Job& j = jobs[job];
            const u32 runs = (j.triangles + gi_run_triangles - 1) / gi_run_triangles;
            const bool by_run = !j.skinned && j.mesh->gi_runs.size() == runs;
            LevelJob lj{
                job, static_cast<u32>(records.size()), 0, static_cast<u32>(items), static_cast<u32>(pairs.size() / 2),
                0};
            for (; at < copies.size() && copies[at].job == job; ++at) {
                const bool reaches = std::any_of(regions.begin(), regions.end(),
                                                 [&](const Bounds3D& r) { return overlaps(copies[at].box, r); });
                if (!reaches || items + j.triangles >= 0x7fffffffull) {
                    continue; // items are numbered in 31 bits (the owners' top bit puts the emissive first)
                }
                // The regions in the mesh's space (each its box's corners' box), to find the runs that reach in.
                bool all = !by_run;
                local.clear();
                if (by_run) {
                    const Mat4f to_mesh = copies[at].instance->transform.inverse();
                    for (const Bounds3D& r : regions) {
                        Bounds3D box{to_mesh.apply(r.min), to_mesh.apply(r.min)};
                        for (int corner = 1; corner < 8; ++corner) {
                            const Vec3f q =
                                to_mesh.apply({corner & 1 ? r.max.x : r.min.x, corner & 2 ? r.max.y : r.min.y,
                                               corner & 4 ? r.max.z : r.min.z});
                            box.min = {std::min(box.min.x, q.x), std::min(box.min.y, q.y), std::min(box.min.z, q.z)};
                            box.max = {std::max(box.max.x, q.x), std::max(box.max.y, q.y), std::max(box.max.z, q.z)};
                        }
                        all = all ||
                              !std::isfinite(box.min.x + box.min.y + box.min.z + box.max.x + box.max.y + box.max.z);
                        local.push_back(box);
                    }
                }
                for (u32 r = 0; r < runs; ++r) {
                    if (all || std::any_of(local.begin(), local.end(),
                                           [&](const Bounds3D& b) { return overlaps(j.mesh->gi_runs[r], b); })) {
                        pairs.push_back(lj.copies);
                        pairs.push_back(r);
                        voxelized += std::min(gi_run_triangles, j.triangles - r * gi_run_triangles);
                    }
                }
                records.push_back(record_of(copies[at]));
                ++lj.copies;
                items += j.triangles;
            }
            lj.pairs = static_cast<u32>(pairs.size() / 2) - lj.pair_first;
            if (lj.pairs > 0) {
                level_jobs[k].push_back(lj);
            }
        }
    }

    // The uploads: the copies, the poses, each level's dirty blocks' bits and the list of the blocks.
    if (!records.empty()) {
        const u32 bytes = static_cast<u32>(records.size() * sizeof(GiCopy));
        ensure_buffer(_gi_copies, bytes, 64 * sizeof(GiCopy), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(_gi_copies->handle(), 0, bytes, records.data(), /*cycle=*/true);
    }
    if (any_skinned) {
        const u32 bytes = static_cast<u32>(joints.size_bytes());
        ensure_buffer(_gi_joints, bytes, 64 * sizeof(Mat4f), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(_gi_joints->handle(), 0, bytes, joints.data(), /*cycle=*/true);
    }
    {
        const u32 pairs_first = static_cast<u32>(dirty.size());
        dirty.insert(dirty.end(), pairs.begin(), pairs.end()); // the runs to voxelize, after the bits
        const u32 bytes = static_cast<u32>(dirty.size() * sizeof(u32));
        ensure_buffer(_gi_dirty, bytes, 1024, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(_gi_dirty->handle(), 0, bytes, dirty.data(), /*cycle=*/true);
        for (std::vector<LevelJob>& list : level_jobs) {
            for (LevelJob& lj : list) {
                lj.pair_first = pairs_first + lj.pair_first * 2; // in words
            }
        }
        const u32 list = static_cast<u32>(made.size() * sizeof(u32));
        ensure_buffer(_gi_blocks, list, 4096, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(_gi_blocks->handle(), 0, list, made.data(), /*cycle=*/true);
    }

    u32 dispatches = 0;
    // 1. The blocks emptied.
    {
        const GiMarker marker{commands, _gpu_markers, "kin3d.gi_clear"};
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.albedo.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.normal.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.emission.handle()}};
        const std::array<SDL_GPUStorageBufferReadWriteBinding, 2> words_rw{
            SDL_GPUStorageBufferReadWriteBinding{.buffer = g.owners->handle()},
            SDL_GPUStorageBufferReadWriteBinding{.buffer = g.seeds->handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, words_rw.data(), 2);
        if (!pass) {
            g.valid = false;
            return false;
        }
        const GiClearUniforms u{{size[0], size[1], size[2], static_cast<u32>(made.size())}};
        SDL_GPUBuffer* list = _gi_blocks->handle();
        SDL_BindGPUComputePipeline(pass, _gi_clear);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &list, 1);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(static_cast<u64>(made.size()) * 512);
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }
    // 2 and 3. The owners, then their attributes: a dispatch a draw a level, with its buffers and textures, in the
    // order of the buffers they read (SDL_GPU puts a barrier between dispatches each time another buffer is bound for
    // reading; the voxels do not depend on the order: the items are numbered in draw order).
    const auto voxelize = [&](SDL_GPUComputePass* pass, bool attributes) {
        for (u32 k = 0; k < levels; ++k) {
            std::vector<LevelJob>& list = level_jobs[k];
            std::stable_sort(list.begin(), list.end(), [&](const LevelJob& a, const LevelJob& b) {
                const Job &x = jobs[a.job], &y = jobs[b.job];
                return std::tuple{x.mesh->vertices, x.mesh->indices, x.skinned ? x.mesh->skin : nullptr} <
                       std::tuple{y.mesh->vertices, y.mesh->indices, y.skinned ? y.mesh->skin : nullptr};
            });
            const f32 c = cell * static_cast<f32>(1u << k);
            GiBlocks round = cores[k].empty() ? GiBlocks{} : cores[k][0];
            for (const GiBlocks& b : cores[k]) {
                round = round_both(round, b);
            }
            for (const LevelJob& lj : list) {
                const Job& job = jobs[lj.job];
                const Material3D& m = job.draw->material;
                const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
                    texture_binding(m.texture, m.wrap_u, m.wrap_v),
                    texture_binding(m.emissive_texture, m.wrap_u, m.wrap_v)};
                SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), 2);
                const std::array<SDL_GPUBuffer*, 8> buffers{job.mesh->vertices,
                                                            job.mesh->indices,
                                                            job.skinned ? job.mesh->skin : _gi_empty->handle(),
                                                            job.skinned ? _gi_joints->handle() : _gi_empty->handle(),
                                                            _gi_copies->handle(),
                                                            _gi_dirty->handle(),
                                                            g.owners->handle(),
                                                            g.seeds->handle()};
                SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), attributes ? 8 : 6);
                GiVoxelizeUniforms u{};
                for (int a = 0; a < 3; ++a) {
                    u.size[a] = size[a];
                    u.window[a] = origin[k][a];
                    u.low[a] = origin[k][a] + round.lo[a] * 8;
                    u.high[a] = origin[k][a] + round.hi[a] * 8 - 1;
                }
                u.origin[3] = 1.0f / c;
                u.size[3] = lj.item_first;
                u.window[3] = static_cast<i32>(k * size[2]);
                u.mesh[0] = job.index_first;
                u.mesh[1] = job.triangles;
                u.mesh[2] = job.base_vertex;
                u.mesh[3] = lj.copies;
                u.flags[0] = job.skinned ? 1u : 0u;
                u.flags[1] = m.blend == Blend3D::Mask ? 1u : 0u;
                u.flags[2] = decode ? 1u : 0u;
                u.flags[3] = m.emissive_texture.backend() ? 1u : 0u;
                u.material[0] = std::max(m.emissive, 0.0f);
                u.material[1] = m.alpha_cutoff;
                u.material[2] = m.double_sided ? 1.0f : 0.0f;
                u.first[0] = lj.copy_first;
                u.first[1] = k * words;
                u.first[2] = lj.pair_first;
                u.first[3] = lj.pairs;
                SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
                const std::array<u32, 2> groups = gi_groups(static_cast<u64>(lj.pairs) * gi_run_triangles);
                SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
                ++dispatches;
            }
        }
    };
    if (!records.empty()) {
        const std::array<SDL_GPUStorageBufferReadWriteBinding, 2> words_rw{
            SDL_GPUStorageBufferReadWriteBinding{.buffer = g.owners->handle()},
            SDL_GPUStorageBufferReadWriteBinding{.buffer = g.seeds->handle()}};
        {
            const GiMarker marker{commands, _gpu_markers, "kin3d.gi_voxelize"};
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, words_rw.data(), 2);
            if (!pass) {
                g.valid = false;
                return false;
            }
            SDL_BindGPUComputePipeline(pass, _gi_voxelize);
            voxelize(pass, false);
            SDL_EndGPUComputePass(pass);
        }
        const GiMarker marker{commands, _gpu_markers, "kin3d.gi_attributes"};
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.albedo.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.normal.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.emission.handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, nullptr, 0);
        if (!pass) {
            g.valid = false;
            return false;
        }
        SDL_BindGPUComputePipeline(pass, _gi_attributes);
        voxelize(pass, true);
        SDL_EndGPUComputePass(pass);
    }
    // 4. The distances, by the floods: a pass a step, every flood's at once (floods whose boxes overlap made one first,
    // over the box round both: two at once would race for the voxels both cover). The seeds' and the owners' words
    // take turns as source and target.
    std::vector<GiFloodUniforms> flood_list;
    for (u32 k = 0; k < levels; ++k) {
        std::vector<Flood>& list = floods[k];
        for (bool merged = true; merged;) {
            merged = false;
            for (std::size_t i = 0; i < list.size() && !merged; ++i) {
                for (std::size_t j = i + 1; j < list.size() && !merged; ++j) {
                    if (meet(list[i].over, list[j].over)) {
                        list[i] = {round_both(list[i].write, list[j].write), round_both(list[i].over, list[j].over)};
                        list.erase(list.begin() + static_cast<std::ptrdiff_t>(j));
                        merged = true;
                    }
                }
            }
        }
        for (const auto& [write, flood] : list) {
            g.rewritten.push_back(
                {static_cast<i32>(k), write.lo[0], write.lo[1], write.lo[2], write.hi[0], write.hi[1], write.hi[2]});
            GiFloodUniforms u{};
            for (int a = 0; a < 3; ++a) {
                u.size[a] = size[a];
                u.window[a] = origin[k][a];
                u.box[a] = flood.lo[a] * 8;
                u.extent[a] = (flood.hi[a] - flood.lo[a]) * 8;
                u.write[a] = write.lo[a] * 8;
                u.write_extent[a] = (write.hi[a] - write.lo[a]) * 8;
            }
            u.size[3] = k;
            flood_list.push_back(u);
        }
    }
    SDL_GPUBuffer* source = g.owners->handle();
    SDL_GPUBuffer* target = g.seeds->handle();
    const auto flood_pass = [&](i32 mode, i32 reach) {
        const GiMarker marker{commands, _gpu_markers, "kin3d.gi_flood"};
        const SDL_GPUStorageTextureReadWriteBinding distance{.texture = g.distance.handle()};
        const SDL_GPUStorageBufferReadWriteBinding to{.buffer = target};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, &distance, 1, &to, 1);
        if (!pass) {
            return false;
        }
        const SDL_GPUTextureSamplerBinding albedo{g.albedo.handle(), _gi_sampler};
        SDL_BindGPUComputePipeline(pass, _gi_flood);
        SDL_BindGPUComputeSamplers(pass, 0, &albedo, 1);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &source, 1);
        for (GiFloodUniforms& u : flood_list) {
            u.mode[0] = mode;
            u.mode[1] = reach;
            SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
            const std::array<u32, 2> groups = gi_groups(static_cast<u64>(u.extent[0]) * u.extent[1] * u.extent[2]);
            SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
            ++dispatches;
        }
        SDL_EndGPUComputePass(pass);
        std::swap(source, target);
        return true;
    };
    if (!flood_list.empty()) {
        bool ok = true;
        for (std::size_t r = 0; r < gi_flood_rounds.size(); ++r) {
            ok = ok && flood_pass((r == 0 ? 1 : 0) | (r + 1 == gi_flood_rounds.size() ? 2 : 0), gi_flood_rounds[r]);
        }
        if (!ok) {
            g.valid = false;
            return false;
        }
    }
    g.valid = true;
    stats.gi_built = true;
    stats.gi_blocks = static_cast<u32>(made.size());
    stats.gi_copies = static_cast<u32>(records.size());
    stats.gi_triangles = voxelized;
    stats.gi_dispatches = dispatches;
    return true;
}

std::array<SDL_GPUTextureSamplerBinding, 2> GpuRenderer3D::gi_bindings(const GpuView3D& view, bool resolved) const {
    const GpuView3D::Gi& g = view._gi;
    if (!resolved || !g.light) {
        const SDL_GPUTextureSamplerBinding flat{_owner->_white.handle(), _owner->_sampler_linear};
        return {flat, flat};
    }
    return {SDL_GPUTextureSamplerBinding{g.light.handle(), _gi_linear},
            SDL_GPUTextureSamplerBinding{g.slope.handle(), _gi_linear}};
}

std::array<SDL_GPUTextureSamplerBinding, 3> GpuRenderer3D::gi_atlases(const GpuView3D& view, bool lit) const {
    if (!lit || !view._gi.irradiance) {
        const SDL_GPUTextureSamplerBinding none{_gi_none.handle(), _owner->_sampler_linear};
        return {none, none, none};
    }
    return {SDL_GPUTextureSamplerBinding{view._gi.irradiance.handle(), _gi_linear},
            SDL_GPUTextureSamplerBinding{view._gi.moments.handle(), _gi_linear},
            SDL_GPUTextureSamplerBinding{view._gi.probe_data.handle(), _gi_sampler}};
}

bool GpuRenderer3D::gi_resolve(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene,
                               const Mat4f& projection, bool jittered, RenderDebugStats3D& stats) {
    GpuView3D::Gi& g = view._gi;
    if (!g.lit || !g.irradiance || !_gi_resolve || !view._ao_depth || !view._ao_normals) {
        return false;
    }
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    const u32 hw = std::max(1u, (w + 1) / 2), hh = std::max(1u, (h + 1) / 2);
    // The last render's light becomes what this one keeps from (the textures take turns), with temporal
    // anti-aliasing's jitter (without, nothing moves under a texel: it would only lag).
    std::swap(g.light, g.last_light);
    std::swap(g.slope, g.last_slope);
    const bool keep =
        jittered && g.has_last && g.last_light && g.last_light.width() == hw && g.last_light.height() == hh;
    try {
        if (!g.light || g.light.width() != hw || g.light.height() != hh) {
            const SDL_GPUTextureUsageFlags usage =
                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            g.light = _device->create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            g.slope = _device->create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the GI's light on the view could not be made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        g.light = {};
        g.slope = {};
        g.last_light = {};
        g.last_slope = {};
        g.has_last = false;
        return false;
    }
    const GiMarker marker{commands, _gpu_markers, "kin3d.gi_resolve"};
    GiResolveUniforms u{};
    const Camera3D& camera = scene.camera;
    std::memcpy(u.to_world, camera.view().inverse().m, sizeof(u.to_world));
    // The inverse projection, as gtao.comp reads it.
    const Mat4f inverse = projection.inverse();
    const f32 xy[4] = {inverse.at(0, 0), inverse.at(1, 1), inverse.at(0, 3), inverse.at(1, 3)};
    const f32 zw[4] = {inverse.at(2, 2), inverse.at(2, 3), inverse.at(3, 2), inverse.at(3, 3)};
    std::memcpy(u.unproject_xy, xy, sizeof(xy));
    std::memcpy(u.unproject_zw, zw, sizeof(zw));
    const f32 sizes[4] = {static_cast<f32>(hw), static_cast<f32>(hh), static_cast<f32>(view._ao_depth.width()),
                          static_cast<f32>(view._ao_depth.height())};
    std::memcpy(u.sizes, sizes, sizeof(sizes));
    const Vec3f forward = camera.forward();
    const f32 eye[4] = {camera.position.x, camera.position.y, camera.position.z,
                        camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f};
    const f32 way[4] = {forward.x, forward.y, forward.z, 0.0f};
    std::memcpy(u.eye, eye, sizeof(eye));
    std::memcpy(u.forward, way, sizeof(way));
    gi_uniforms(view, scene.global_illumination.probes.intensity, true, nullptr, u.probes.counts);
    std::memcpy(u.last_view_projection, g.last_view_projection.m, sizeof(u.last_view_projection));
    const f32 last_eye[4] = {g.last_eye.x, g.last_eye.y, g.last_eye.z, keep ? 1.0f : 0.0f};
    const f32 last_way[4] = {g.last_forward.x, g.last_forward.y, g.last_forward.z, 0.0f};
    std::memcpy(u.last_eye, last_eye, sizeof(last_eye));
    std::memcpy(u.last_forward, last_way, sizeof(last_way));
    const std::array<SDL_GPUStorageTextureReadWriteBinding, 2> out{
        SDL_GPUStorageTextureReadWriteBinding{.texture = g.light.handle(), .cycle = true},
        SDL_GPUStorageTextureReadWriteBinding{.texture = g.slope.handle(), .cycle = true}};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, out.data(), 2, nullptr, 0);
    if (!pass) {
        return false;
    }
    const SDL_GPUTextureSamplerBinding flat{_owner->_white.handle(), _owner->_sampler_linear};
    const std::array<SDL_GPUTextureSamplerBinding, 7> samplers{
        SDL_GPUTextureSamplerBinding{view._ao_depth.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{g.irradiance.handle(), _gi_linear},
        SDL_GPUTextureSamplerBinding{g.moments.handle(), _gi_linear},
        SDL_GPUTextureSamplerBinding{g.probe_data.handle(), _gi_sampler},
        keep ? SDL_GPUTextureSamplerBinding{g.last_light.handle(), _gi_linear} : flat,
        keep ? SDL_GPUTextureSamplerBinding{g.last_slope.handle(), _gi_linear} : flat};
    SDL_BindGPUComputePipeline(pass, _gi_resolve);
    SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
    SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
    SDL_DispatchGPUCompute(pass, (hw + 7) / 8, (hh + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
    g.last_view_projection = projection * camera.view();
    g.last_eye = camera.position;
    g.last_forward = forward;
    g.has_last = true;
    stats.gi_resolved = true;
    ++stats.gi_dispatches;
    return true;
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
    const u32 levels = g.levels;
    const std::array<u32, 3> size = g.size;
    // The probes' windows: a probe every `spacing` voxels (2, 4 or 8; further apart where a level would have more than
    // max_gi_probes), each window the voxels' (whose first voxel and size are whole blocks).
    u32 spacing = settings.spacing <= 2 ? 2u : settings.spacing <= 5 ? 4u : 8u;
    std::array<u32, 3> counts{};
    for (;;) {
        for (int a = 0; a < 3; ++a) {
            counts[a] = std::max(size[a] / spacing, 2u);
        }
        if (static_cast<u64>(counts[0]) * counts[1] * counts[2] <= max_gi_probes || spacing >= 8) {
            break;
        }
        spacing *= 2;
    }
    const u32 per_level = counts[0] * counts[1] * counts[2];
    const u32 total = per_level * levels;
    std::array<std::array<i32, 3>, max_gi_levels> probe_origin{};
    for (u32 k = 0; k < levels; ++k) {
        for (int a = 0; a < 3; ++a) {
            probe_origin[k][a] = g.origin[k][a] / static_cast<i32>(spacing);
        }
    }
    const bool fresh = !g.probes_valid || !g.irradiance || g.probe_counts != counts || g.spacing != spacing ||
                       g.irradiance.width() != counts[0] * 8 || g.radiance.width() != size[0] ||
                       g.radiance.height() != size[1] || g.probe_levels != levels;
    try {
        if (fresh) {
            const SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                                   SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
                                                   SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE;
            const u32 layers = counts[2] * levels;
            g.irradiance = _device->create_volume(counts[0] * 8, counts[1] * 8, layers,
                                                  SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            g.moments = _device->create_volume(counts[0] * 16, counts[1] * 16, layers,
                                               SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT, usage);
            g.probe_data =
                _device->create_volume(counts[0], counts[1], layers, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            for (GpuTexture* t : {&g.radiance, &g.radiance_back}) {
                *t =
                    _device->create_volume(size[0], size[1], size[2] * levels, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                           SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
            }
            g.probe_counts = counts;
            g.spacing = spacing;
            g.probe_levels = levels;
            g.next = 0;
            g.updates = 0;
            g.lit = false;
            g.waiting.assign(total, 0);
            g.pending.clear();
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the GI probes could not be made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        g.irradiance = {};
        g.probes_valid = false;
        g.lit = false;
        return false;
    }
    // Which probes are placed: those new to their window (emptied first), and those near where the distances were
    // written again (a probe looks for room up to 0.4 spacings round it); and which wait to be traced first (the new
    // ones, until a batch has them).
    std::vector<u32> placing;
    std::vector<u8> mark(per_level);
    const auto stored_index = [&](const std::array<i32, 3>& world) {
        std::array<u32, 3> st{};
        for (int a = 0; a < 3; ++a) {
            st[a] = wrap(world[a], static_cast<i32>(counts[a]));
        }
        return st[0] + counts[0] * (st[1] + counts[1] * st[2]);
    };
    const i32 step = static_cast<i32>(spacing),
              reach = static_cast<i32>(std::ceil(0.4f * static_cast<f32>(spacing))) + 1;
    for (u32 k = 0; k < levels; ++k) {
        const std::array<i32, 3> old = g.probe_origin[k];
        std::fill(mark.begin(), mark.end(), fresh ? u8{2} : u8{0});
        bool any = fresh;
        if (!fresh && probe_origin[k] != old) {
            for (u32 z = 0; z < counts[2]; ++z) {
                for (u32 y = 0; y < counts[1]; ++y) {
                    for (u32 x = 0; x < counts[0]; ++x) {
                        const std::array<i32, 3> world{probe_origin[k][0] + static_cast<i32>(x),
                                                       probe_origin[k][1] + static_cast<i32>(y),
                                                       probe_origin[k][2] + static_cast<i32>(z)};
                        bool was = true;
                        for (int a = 0; a < 3; ++a) {
                            was = was && world[a] >= old[a] && world[a] < old[a] + static_cast<i32>(counts[a]);
                        }
                        if (!was) {
                            mark[stored_index(world)] = 2;
                            any = true;
                        }
                    }
                }
            }
        }
        for (const std::array<i32, 7>& box : g.rewritten) {
            if (fresh || box[0] != static_cast<i32>(k)) {
                continue;
            }
            std::array<i32, 3> lo{}, hi{};
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::max(floor_div(box[1 + a] * 8 - reach + step - 1, step), 0);
                hi[a] = std::min(floor_div(box[4 + a] * 8 - 1 + reach, step), static_cast<i32>(counts[a]) - 1);
            }
            for (i32 z = lo[2]; z <= hi[2]; ++z) {
                for (i32 y = lo[1]; y <= hi[1]; ++y) {
                    for (i32 x = lo[0]; x <= hi[0]; ++x) {
                        u8& m = mark[stored_index(
                            {probe_origin[k][0] + x, probe_origin[k][1] + y, probe_origin[k][2] + z})];
                        m = std::max(m, u8{1});
                        any = true;
                    }
                }
            }
        }
        if (!any) {
            continue;
        }
        for (u32 i = 0; i < per_level; ++i) {
            if (mark[i] == 2) {
                placing.push_back(i | (1u << 23) | (k << 24));
                if (!g.waiting[k * per_level + i]) {
                    g.waiting[k * per_level + i] = 1;
                    g.pending.push_back(i | (k << 24));
                }
            } else if (mark[i] == 1) {
                placing.push_back(i | (k << 24));
            }
        }
    }
    g.probe_origin = probe_origin;
    g.probes_valid = true;

    GiSceneUniforms scene_u{};
    GiProbeUniforms probes_u{};
    gi_uniforms(view, settings.intensity, g.lit, scene_u.size, probes_u.counts);
    const auto put4 = [](f32* to, f32 x, f32 y, f32 z, f32 w) {
        to[0] = x;
        to[1] = y;
        to[2] = z;
        to[3] = w;
    };
    u32 dispatches = 0;

    // 1. Where the probes stand.
    if (!placing.empty()) {
        const GiMarker marker{commands, _gpu_markers, "kin3d.gi_place"};
        const u32 bytes = static_cast<u32>(placing.size() * sizeof(u32));
        ensure_buffer(g.place_list, bytes, 4096, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(g.place_list->handle(), 0, bytes, placing.data(), /*cycle=*/true);
        GiPlaceUniforms u{};
        u.scene = scene_u;
        u.probes = probes_u;
        u.count[0] = static_cast<u32>(placing.size());
        put4(u.misc, 0.0f, std::min(1.8f * static_cast<f32>(spacing), 7.9f), 0.0f, 0.0f);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.probe_data.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.irradiance.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.moments.handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, nullptr, 0);
        if (!pass) {
            return false;
        }
        const SDL_GPUTextureSamplerBinding distance{g.distance.handle(), _gi_sampler};
        SDL_GPUBuffer* list = g.place_list->handle();
        SDL_BindGPUComputePipeline(pass, _gi_place);
        SDL_BindGPUComputeSamplers(pass, 0, &distance, 1);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &list, 1);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(placing.size());
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }

    // 2. The lit scene: the blocks made this render, and a share of all the blocks in turn (each lit every
    // gi_light_turns renders), lit by the sun, the fill, the lamps and the probes (as they were: the bounce of the
    // render before).
    const Lights3D& lights = scene.lights;
    std::vector<GiLight> list;
    list.reserve(std::min<std::size_t>(lights.points.size() + lights.spots.size(), max_gi_lights));
    for (const PointLightSource3D& l : lights.points) {
        if (list.size() >= max_gi_lights) {
            break;
        }
        const f32 extent = std::max(l.size, 0.01f);
        GiLight q{};
        put4(q.at, l.position.x, l.position.y, l.position.z, std::max(l.radius, 1e-3f));
        put4(q.colour, l.color.r, l.color.g, l.color.b, extent * extent);
        put4(q.axis, 0.0f, -1.0f, 0.0f, -2.0f);
        list.push_back(q);
    }
    for (const SpotLightSource3D& l : lights.spots) {
        if (list.size() >= max_gi_lights) {
            break;
        }
        const f32 extent = std::max(l.size, 0.01f);
        const Vec3f way = length_squared(l.direction) > 0.0f ? normalize(l.direction) : Vec3f{0.0f, -1.0f, 0.0f};
        constexpr f32 to_radians = 3.14159265f / 180.0f;
        const f32 outer = std::min(l.outer_angle, 89.9f), inner = std::clamp(l.inner_angle, 0.0f, outer - 0.1f);
        const f32 cos_outer = std::cos(outer * to_radians), cos_inner = std::cos(inner * to_radians);
        GiLight q{};
        put4(q.at, l.position.x, l.position.y, l.position.z, std::max(l.radius, 1e-3f));
        put4(q.colour, l.color.r, l.color.g, l.color.b, extent * extent);
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
        const std::array<i32, 3> blocks{static_cast<i32>(size[0] / 8), static_cast<i32>(size[1] / 8),
                                        static_cast<i32>(size[2] / 8)};
        const u32 per = static_cast<u32>(blocks[0] * blocks[1] * blocks[2]), all = per * levels;
        std::vector<u32> lit_blocks = g.made;
        const u32 share = (all + gi_light_turns - 1) / gi_light_turns;
        const u32 from = static_cast<u32>(g.renders % gi_light_turns) * share;
        for (u32 i = from; i < std::min(from + share, all); ++i) {
            const u32 k = i / per, b = i % per;
            const u32 x = b % static_cast<u32>(blocks[0]),
                      y = (b / static_cast<u32>(blocks[0])) % static_cast<u32>(blocks[1]),
                      z = b / static_cast<u32>(blocks[0] * blocks[1]);
            lit_blocks.push_back(x | (y << 8) | (z << 16) | (k << 24));
        }
        ++g.renders;
        const GiMarker marker{commands, _gpu_markers, "kin3d.gi_light"};
        const u32 bytes = static_cast<u32>(lit_blocks.size() * sizeof(u32));
        ensure_buffer(g.light_list, bytes, 4096, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(g.light_list->handle(), 0, bytes, lit_blocks.data(), /*cycle=*/true);
        GiLightUniforms u{};
        u.scene = scene_u;
        u.count[0] = static_cast<u32>(list.size());
        u.count[1] = static_cast<u32>(lit_blocks.size());
        const LinearRgb sun = lights.sun.color, fill = lights.fill.color;
        const bool sun_shines = sun.r + sun.g + sun.b > 0.0f && length_squared(lights.sun.direction) > 0.0f;
        const bool fill_shines = fill.r + fill.g + fill.b > 0.0f && length_squared(lights.fill.direction) > 0.0f;
        const Vec3f to_sun = sun_shines ? normalize(lights.sun.direction) : Vec3f{0.0f, 1.0f, 0.0f};
        const Vec3f to_fill = fill_shines ? normalize(lights.fill.direction) : Vec3f{0.0f, 1.0f, 0.0f};
        put4(u.sun_direction, to_sun.x, to_sun.y, to_sun.z, std::clamp(lights.sun_shadow_strength, 0.0f, 1.0f));
        put4(u.sun_colour, sun.r, sun.g, sun.b, sun_shines ? 1.0f : 0.0f);
        put4(u.fill_direction, to_fill.x, to_fill.y, to_fill.z, 0.0f);
        put4(u.fill_colour, fill.r, fill.g, fill.b, fill_shines ? 1.0f : 0.0f);
        u.probes = probes_u;
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 2> radiance{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.radiance.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.radiance_back.handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, radiance.data(), 2, nullptr, 0);
        if (!pass) {
            return false;
        }
        const std::array<SDL_GPUTextureSamplerBinding, 3> probes = gi_atlases(view, g.lit);
        const std::array<SDL_GPUTextureSamplerBinding, 7> samplers{
            SDL_GPUTextureSamplerBinding{g.distance.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.albedo.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.normal.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.emission.handle(), _gi_sampler},
            probes[0],
            probes[1],
            probes[2]};
        const std::array<SDL_GPUBuffer*, 2> buffers{_gi_lights->handle(), g.light_list->handle()};
        SDL_BindGPUComputePipeline(pass, _gi_light);
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
        SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), 2);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(static_cast<u64>(lit_blocks.size()) * 512);
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }

    // 3. This render's batch casts its rays: the probes waiting first (new to their windows: the nearest level's
    // first), then the others in turn (none twice in a batch: they would race for their tiles).
    const u32 rays = std::clamp(settings.rays, 16u, 256u);
    const u32 most = std::max(1u, max_gi_rays / rays);
    const u32 batch_size = std::min(settings.per_render == 0 ? most : std::min(settings.per_render, most), total);
    std::vector<u32> batch;
    batch.reserve(batch_size);
    const std::size_t taken = std::min<std::size_t>(g.pending.size(), batch_size);
    for (std::size_t i = 0; i < taken; ++i) {
        const u32 entry = g.pending[i];
        batch.push_back(entry);
        g.waiting[(entry >> 24) * per_level + (entry & 0xffffffu)] = 2;
    }
    g.pending.erase(g.pending.begin(), g.pending.begin() + static_cast<std::ptrdiff_t>(taken));
    for (u32 tried = 0; batch.size() < batch_size && tried < total; ++tried) {
        const u32 i = g.next;
        g.next = (g.next + 1) % total;
        if (g.waiting[i] == 0) {
            batch.push_back((i % per_level) | ((i / per_level) << 24));
        }
    }
    for (const u32 entry : batch) {
        u8& mark = g.waiting[(entry >> 24) * per_level + (entry & 0xffffffu)];
        mark = mark == 2 ? 0 : mark;
    }
    const Mat4f rotation = ray_rotation(g.updates);
    {
        const u32 bytes = static_cast<u32>(batch.size() * sizeof(u32));
        ensure_buffer(g.batch_list, bytes, 4096, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(g.batch_list->handle(), 0, bytes, batch.data(), /*cycle=*/true);
    }
    ensure_buffer(g.rays, static_cast<u32>(batch.size()) * rays * 16u, 4096,
                  SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
    {
        const GiMarker marker{commands, _gpu_markers, "kin3d.gi_trace"};
        GiTraceUniforms u{};
        u.scene = scene_u;
        u.probes = probes_u;
        u.batch[0] = 512;
        u.batch[1] = static_cast<u32>(batch.size());
        u.batch[2] = rays;
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
        SDL_GPUBuffer* list_buffer = g.batch_list->handle();
        SDL_BindGPUComputePipeline(pass, _gi_trace);
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
        SDL_BindGPUComputeStorageBuffers(pass, 0, &list_buffer, 1);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(static_cast<u64>(batch.size()) * rays);
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }
    // 4. And takes them in.
    {
        const GiMarker marker{commands, _gpu_markers, "kin3d.gi_update"};
        GiUpdateUniforms u{};
        u.probes = probes_u;
        u.batch[1] = static_cast<u32>(batch.size());
        u.batch[2] = rays;
        std::memcpy(u.rotation, rotation.m, sizeof(u.rotation));
        put4(u.misc, std::clamp(settings.hysteresis, 0.0f, 0.99f), 1.5f * 1.7320508f, 0.0f, 0.0f);
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> textures{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.irradiance.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.moments.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.probe_data.handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, textures.data(), 3, nullptr, 0);
        if (!pass) {
            return false;
        }
        const std::array<SDL_GPUBuffer*, 2> buffers{g.rays->handle(), g.batch_list->handle()};
        SDL_BindGPUComputePipeline(pass, _gi_update);
        SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), 2);
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const u32 count = static_cast<u32>(batch.size());
        const u32 across = std::min(count, 32768u);
        SDL_DispatchGPUCompute(pass, across, (count + across - 1) / across, 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }
    g.updates += batch.size();
    g.lit = true; // a probe gives light once traced (until then a level leaves the place to the next, or the sky)
    stats.gi_probes = total;
    stats.gi_probes_updated = static_cast<u32>(batch.size());
    stats.gi_rays = static_cast<u32>(batch.size()) * rays;
    stats.gi_dispatches += dispatches;
    stats.gi_lit = true;
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
    u.forward[3] = static_cast<f32>(static_cast<u8>(scene.debug.gi_view));
    u.target[0] = static_cast<f32>(view._size.x);
    u.target[1] = static_cast<f32>(view._size.y);
    u.target[2] = encode ? 1.0f : 0.0f;
    const bool lit = view._gi.lit && view._gi.irradiance;
    u.target[3] = lit ? 1.0f : 0.0f;
    gi_uniforms(view, scene.global_illumination.probes.intensity, lit, u.scene.size, u.probes.counts);
    const std::array<SDL_GPUTextureSamplerBinding, 3> probes = gi_atlases(view, lit);
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
    const u32 depth = gi.size[2] * gi.levels;
    std::vector<u8> distance, albedo, normal, emission;
    if (!_device->read_volume(gi.distance, depth, 2, distance) || !_device->read_volume(gi.albedo, depth, 4, albedo) ||
        !_device->read_volume(gi.normal, depth, 4, normal) || !_device->read_volume(gi.emission, depth, 8, emission)) {
        return false;
    }
    // The first level's window, in window order (it is stored wrapped round).
    const std::array<u32, 3> size = gi.size;
    const std::size_t voxels = static_cast<std::size_t>(size[0]) * size[1] * size[2];
    out.origin = {static_cast<f32>(gi.origin[0][0]) * gi.cell, static_cast<f32>(gi.origin[0][1]) * gi.cell,
                  static_cast<f32>(gi.origin[0][2]) * gi.cell};
    out.cell = gi.cell;
    out.size = size;
    out.distance.resize(voxels);
    out.albedo.resize(voxels);
    out.normal.resize(voxels);
    out.emission.resize(voxels * 3);
    for (u32 z = 0; z < size[2]; ++z) {
        for (u32 y = 0; y < size[1]; ++y) {
            for (u32 x = 0; x < size[0]; ++x) {
                const std::array<u32, 3> local{x, y, z};
                std::array<u32, 3> st{};
                for (int a = 0; a < 3; ++a) {
                    st[a] = wrap(gi.origin[0][a] + static_cast<i32>(local[a]), static_cast<i32>(size[a]));
                }
                const std::size_t from = st[0] + size[0] * (st[1] + static_cast<std::size_t>(size[1]) * st[2]);
                const std::size_t i = x + size[0] * (y + static_cast<std::size_t>(size[1]) * z);
                u16 h;
                std::memcpy(&h, distance.data() + from * 2, sizeof(h));
                out.distance[i] = half_to_float(h);
                std::memcpy(&out.albedo[i], albedo.data() + from * 4, sizeof(u32));
                std::memcpy(&out.normal[i], normal.data() + from * 4, sizeof(u32));
                for (std::size_t c = 0; c < 3; ++c) {
                    std::memcpy(&h, emission.data() + from * 8 + c * 2, sizeof(h));
                    out.emission[i * 3 + c] = half_to_float(h);
                }
            }
        }
    }
    return true;
}

} // namespace kin::gpu
