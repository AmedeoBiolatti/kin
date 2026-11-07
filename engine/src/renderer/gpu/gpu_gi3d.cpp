// The SDL_GPU backend's global illumination (docs/3d_plan.md, G5): the GI scene (G5a), the scene in voxels of
// Scene3D::global_illumination's area in windows round the camera, made again where it changes (G5c); the lit scene and
// the probes (G5b); the debug view and the read-back.
#include "embedded_gpu_shader.hpp"
#include "gpu_area_lights3d.hpp"
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
#include <kin/render3d/skeleton3d.hpp>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_set>

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
    f32 centre[4];   // unsnapped camera; signed half block in probe steps (negative: fixed outer level)
};
static_assert(sizeof(GiProbeUniforms) == 112);

// gi_voxelize.comp's and gi_voxelize_attributes.comp's Params.
struct GiVoxelizeUniforms {
    f32 origin[4];
    u32 size[4];
    u32 mesh[4];
    u32 flags[4];
    f32 material[4];
    u32 first[4]; // the draw's first copy, the level's first word of the dirty blocks' bits
    i32 window[4];
    i32 low[4], high[4]; // xyz: the box round the blocks to make; low.w: every block inside the box is dirty
    f32 emission[4];
};
static_assert(sizeof(GiVoxelizeUniforms) == 160);

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
static_assert(sizeof(GiViewUniforms) == 304);

// gi_light.comp's Params, and a light as it reads them.
struct GiLightUniforms {
    GiSceneUniforms scene;
    u32 count[4]; // lights, blocks
    f32 sun_direction[4];
    f32 sun_colour[4];
    f32 fill_direction[4];
    f32 fill_colour[4];
    GiProbeUniforms probes;
    f32 sun_matrix[16]; // the GI's sun map's view and projection (GpuRenderer3D::_gi_sun_matrix)
    f32 sun_map[4];     // x: 1 with the map, y: its depth bias, z: a texel (uv), w: a texel (world)
    f32 area[4];        // area-light emitters, 1 with area lights, GI emitters in the explicit flags
    f32 area_scene[4];  // where the copies start in the scene buffer (vec4s); the settled hierarchy's first node and
                        // first triangle, 1 when there is one (bits are floats: exact below 2^24)
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
static_assert(sizeof(GiResolveUniforms) == 352);

// Renders a copy stays unchanged before it joins the area lights' settled hierarchy (GpuView3D::Gi::area_seen).
constexpr u32 area_settle = 8;
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

// The most a transform's 3 x 3 part grows an area: the product of its two largest singular values (the square roots of
// the two largest eigenvalues of M^T M, a symmetric 3 x 3 matrix's, found in closed form).
f32 area_growth(const Mat4f& t) {
    const f64 c[3][3] = {{t.m[0], t.m[1], t.m[2]}, {t.m[4], t.m[5], t.m[6]}, {t.m[8], t.m[9], t.m[10]}}; // columns
    f64 a[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            a[i][j] = c[i][0] * c[j][0] + c[i][1] * c[j][1] + c[i][2] * c[j][2];
        }
    }
    const f64 off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    std::array<f64, 3> e{a[0][0], a[1][1], a[2][2]};
    if (off > 1e-30) {
        const f64 q = (a[0][0] + a[1][1] + a[2][2]) / 3.0;
        const f64 p2 =
            (a[0][0] - q) * (a[0][0] - q) + (a[1][1] - q) * (a[1][1] - q) + (a[2][2] - q) * (a[2][2] - q) + 2.0 * off;
        const f64 p = std::sqrt(p2 / 6.0);
        f64 b[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                b[i][j] = (a[i][j] - (i == j ? q : 0.0)) / p;
            }
        }
        const f64 det = b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) -
                        b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) +
                        b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0]);
        const f64 phi = std::acos(std::clamp(det / 2.0, -1.0, 1.0)) / 3.0;
        e[0] = q + 2.0 * p * std::cos(phi);
        e[2] = q + 2.0 * p * std::cos(phi + 2.0943951023931957);
        e[1] = 3.0 * q - e[0] - e[2];
    }
    std::sort(e.begin(), e.end());
    return static_cast<f32>(std::sqrt(std::max(e[1], 0.0) * std::max(e[2], 0.0)) * 1.0001); // (a hair over: a bound)
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
        _gi_light = make_gi_compute(handle, "gi_light.comp.spv", 8, 8, 3, 0);
        _gi_place = make_gi_compute(handle, "gi_probe_place.comp.spv", 1, 1, 3, 0);
        _gi_trace = make_gi_compute(handle, "gi_probe_trace.comp.spv", 7, 1, 0, 1);
        _gi_update = make_gi_compute(handle, "gi_probe_update.comp.spv", 0, 2, 3, 0, 16, 16);
        _gi_resolve = make_gi_compute(handle, "gi_resolve.comp.spv", 8, 0, 3, 0, 8, 8);
        _gi_area = make_gi_compute(handle, "gi_area.comp.spv", 3, 6, 1, 0, 8, 8);
        _gi_area_fill = make_gi_compute(handle, "gi_area_fill.comp.spv", 7, 0, 1, 2, 8, 8);
        if (!_gi_clear || !_gi_voxelize || !_gi_attributes || !_gi_flood || !_gi_light || !_gi_place || !_gi_trace ||
            !_gi_update || !_gi_resolve || !_gi_area || !_gi_area_fill) {
            throw std::runtime_error(std::string{"compute pipelines not made: "} + SDL_GetError());
        }
        _gi_view_shader = GpuShader::from_embedded(*_device, SDL_GPU_SHADERSTAGE_FRAGMENT, "gi_view.frag.spv",
                                                   /*uniform_buffers=*/1, /*samplers=*/9, /*storage_buffers=*/1);
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
        _area_ltc = std::make_shared<GpuBuffer>(_device->create_buffer(
            SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, area_light_ltc, sizeof(f32) * area_light_ltc_floats));
        _area_dummy = std::make_shared<GpuBuffer>(
            _device->create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, zeros.data(), zeros.size()));
        _area_cache_none = _device->create_volume(1, 1, 1, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                                  SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                                      SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
        _gi_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: global illumination not available",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        release_gi_pipelines();
    }
    _capabilities.global_illumination = _gi_available;
}

void GpuRenderer3D::release_gi_pipelines() {
    release_gi_proxies();
    SDL_GPUDevice* handle = _device->handle();
    for (SDL_GPUComputePipeline** compute :
         {&_gi_clear, &_gi_voxelize, &_gi_attributes, &_gi_flood, &_gi_light, &_gi_place, &_gi_trace, &_gi_update,
          &_gi_resolve, &_gi_area, &_gi_area_fill}) {
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

namespace {

// A GI stand-in's picture: gi_proxy_side texels a side (and its mips), over a UV square gi_proxy_extent units a side
// (what a world-aligned surface, a ground most, sees of the world). At most gi_proxy_bakes are made a render (the rest
// the next); one unused for gi_proxy_idle frames is let go.
constexpr u32 gi_proxy_side = 64;
constexpr u32 gi_proxy_levels = 7;
constexpr f32 gi_proxy_extent = 4.0f;
constexpr u32 gi_proxy_bakes = 32;
constexpr u64 gi_proxy_idle = 600;

} // namespace

bool GpuRenderer3D::gi_proxied(const Material3D& m) {
    // As the GI scene takes draws in (gi_scene): lit, opaque or cut out. A custom shader stands in for a surface.
    return !m.shader.backend() && dynamic_cast<const GpuSurfaceShader3D*>(m.surface.backend().get()) && !m.unlit &&
           !m.wireframe && (m.blend == Blend3D::Opaque || m.blend == Blend3D::Mask);
}

u64 GpuRenderer3D::gi_proxy_key(const Material3D& m) {
    // All a surface can read of its material but the lighting's own (a surface shader is kept as long as the
    // renderer: its address is its own).
    GiKey key;
    key.add(m.surface.backend().get());
    key.add(m.shader_params);
    key.add(m.color);
    key.add(m.emissive);
    key.add(m.emission_mode);
    key.add(m.emission_color);
    key.add(m.blend);
    key.add(m.alpha_cutoff);
    key.add(m.metallic);
    key.add(m.roughness);
    key.add(m.normal_scale);
    key.add(m.occlusion_strength);
    key.add(m.uv_scale);
    key.add(m.uv_offset);
    key.add(m.wrap_u);
    key.add(m.wrap_v);
    for (const Texture* texture :
         {&m.texture, &m.metallic_roughness, &m.normal, &m.occlusion, &m.emissive_texture, &m.graph_maps}) {
        key.add(texture->backend().get());
        if (const auto* gpu = dynamic_cast<const GpuTextureBackend*>(texture->backend().get())) {
            key.add(gpu->version());
            key.add(gpu->scale_mode());
        }
    }
    for (const auto& sampler : m.texture_samplers) {
        key.add(bool(sampler));
        if (sampler) {
            key.add(sampler->wrap_u);
            key.add(sampler->wrap_v);
            key.add(sampler->min);
            key.add(sampler->mag);
            key.add(sampler->mip);
        }
    }
    return key.value;
}

const GpuRenderer3D::GiProxy* GpuRenderer3D::gi_proxy(const Material3D& material) {
    if (_gi_proxies.empty() || !gi_proxied(material)) {
        return nullptr;
    }
    const auto it = _gi_proxies.find(gi_proxy_key(material));
    if (it == _gi_proxies.end() || !it->second.base) {
        return nullptr;
    }
    it->second.used = _owner->_frame_serial;
    return &it->second;
}

void GpuRenderer3D::bake_gi_proxies(const Scene3D& scene) {
    const u64 frame = _owner->_frame_serial;
    std::erase_if(_gi_proxies, [&](const auto& entry) { return entry.second.used + gi_proxy_idle < frame; });
    if (!scene.global_illumination.enabled || !_gi_available || !_capabilities.surface_pass || _gi_proxy_broken) {
        return;
    }
    u32 baked = 0;
    for (const Scene3D::Draw& draw : scene.draws()) {
        if (draw.count == 0 || !gi_proxied(draw.material)) {
            continue;
        }
        const auto [it, fresh] = _gi_proxies.try_emplace(gi_proxy_key(draw.material));
        it->second.used = frame;
        if (!fresh) {
            continue;
        }
        if (baked == gi_proxy_bakes) {
            _gi_proxies.erase(it); // the next render's
            continue;
        }
        ++baked;
        // One that cannot be made stays, empty (the material's maps then, as without): not tried again each render.
        if (!bake_gi_proxy(draw.material, scene.time, it->second) && _gi_proxy_broken) {
            return;
        }
    }
}

bool GpuRenderer3D::bake_gi_proxy(const Material3D& material, f32 time, GiProxy& out) {
    constexpr SDL_GPUTextureUsageFlags target = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    constexpr SDL_GPUTextureFormat linear = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    const SDL_GPUTextureFormat colour = _owner->_color_texture_format;
    const f32 h = 0.5f * gi_proxy_extent;
    if (!_gi_proxy_quad) {
        try {
            if (!SDL_GPUTextureSupportsFormat(_device->handle(), colour, SDL_GPU_TEXTURETYPE_2D, target)) {
                throw std::runtime_error("the maps' colour format is no render target here");
            }
            _gi_proxy_shader = GpuShader::from_embedded(*_device, SDL_GPU_SHADERSTAGE_FRAGMENT, "gi_proxy.frag.spv",
                                                        /*uniform_buffers=*/1, /*samplers=*/2, /*storage_buffers=*/0);
            if (!_gi_proxy_shader) {
                throw std::runtime_error("gi_proxy.frag not made");
            }
            // u along x and v along z, so that from above (up -z) the picture's columns and rows are the UV's.
            MeshData3D quad;
            const Vec3f up{0.0f, 1.0f, 0.0f};
            quad.vertices = {{{-h, 0.0f, -h}, up, {0.0f, 0.0f}},
                             {{h, 0.0f, -h}, up, {1.0f, 0.0f}},
                             {{h, 0.0f, h}, up, {1.0f, 1.0f}},
                             {{-h, 0.0f, h}, up, {0.0f, 1.0f}}};
            quad.indices = {0, 2, 1, 0, 3, 2};
            auto mesh = create_mesh(quad, nullptr, {});
            if (!mesh) {
                throw std::runtime_error("the square not made");
            }
            for (Texture* scratch : {&_gi_proxy_unlit, &_gi_proxy_dark}) {
                *scratch = Texture{std::make_shared<GpuTextureBackend>(
                    _device->create_target(gi_proxy_side, gi_proxy_side, linear, target), false, ScaleMode::Nearest,
                    TextureFormat::Rgba16Float)};
            }
            _gi_proxy_depth = Texture{std::make_shared<GpuTextureBackend>(
                _device->create_target(gi_proxy_side, gi_proxy_side, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                                       SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER),
                false, ScaleMode::Nearest, TextureFormat::Depth32Float)};
            _gi_proxy_quad = Mesh3D{std::move(mesh), 4, 6, Bounds3D{{-h, 0.0f, -h}, {h, 0.0f, h}}};
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: surface materials' GI stand-ins not made: the GI reads their maps",
                           (LogFields{{.name = "reason", .value = e.what()}}));
            release_gi_proxies();
            _gi_proxy_broken = true;
            return false;
        }
    }
    // The material drawn into `into` over the square, seen from above, lit by nothing: unlit, its base colour plus its
    // emission; lit, its emission alone (nothing reaches it to reflect).
    RenderDebugStats3D stats;
    const auto draw = [&](bool unlit, const Texture& into) {
        Scene3D scene;
        scene.camera = Camera3D::look_at({0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 60.0f, {0.0f, 0.0f, -1.0f});
        scene.camera.projection = Projection3D::Orthographic;
        scene.camera.height = gi_proxy_extent;
        scene.camera.z_near = 0.5f;
        scene.camera.z_far = 2.0f;
        scene.lights.sun.color = {0.0f, 0.0f, 0.0f};
        scene.lights.fill.color = {0.0f, 0.0f, 0.0f};
        scene.lights.ambient = {0.0f, 0.0f, 0.0f};
        scene.lights.ground_ambient.enabled = false;
        scene.time = time;
        Material3D look = material;
        look.unlit = unlit;
        look.double_sided = true;
        look.casts_shadow = false;
        look.receives_shadow = false;
        if (!scene.draw(_gi_proxy_quad, Mat4f::identity(), look).ok()) {
            return false;
        }
        const auto prepared = prepare(scene, PrepareOptions3D{}, stats);
        return prepared &&
               render_surface({.depth = _gi_proxy_depth, .linear_colour = into}, *prepared, SurfaceOptions3D{}, stats);
    };
    // The emission is kept (with its mips) only for a material that emits by its own fields; else drawn in scratch.
    const bool emits = material.emits_light();
    GpuTexture base =
        _device->create_target(gi_proxy_side, gi_proxy_side, colour, target, SDL_GPU_SAMPLECOUNT_1, gi_proxy_levels);
    Texture lit = _gi_proxy_dark;
    if (emits) {
        GpuTexture emission = _device->create_target(gi_proxy_side, gi_proxy_side, linear, target,
                                                     SDL_GPU_SAMPLECOUNT_1, gi_proxy_levels);
        if (!emission) {
            return false;
        }
        lit = Texture{std::make_shared<GpuTextureBackend>(std::move(emission), false, ScaleMode::Mipmapped,
                                                          TextureFormat::Rgba16Float, true)};
    }
    if (!base || !draw(true, _gi_proxy_unlit) || !draw(false, lit)) {
        return false;
    }
    // The base colour: the one less the other, a texel each; then both pictures' mips.
    _owner->ensure_frame();
    _owner->flush_to_frame();
    SDL_GPUCommandBuffer* commands = _owner->_frame->command_buffer();
    SDL_GPUColorTargetInfo info{};
    info.texture = base.handle();
    info.load_op = SDL_GPU_LOADOP_DONT_CARE;
    info.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &info, 1, nullptr);
    if (!pass) {
        return false;
    }
    const auto* unlit = static_cast<const GpuTextureBackend*>(_gi_proxy_unlit.backend().get());
    const auto* dark = static_cast<const GpuTextureBackend*>(lit.backend().get());
    SDL_BindGPUGraphicsPipeline(pass, fullscreen(_gi_proxy_shader.handle(), colour));
    const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
        SDL_GPUTextureSamplerBinding{unlit->texture().handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{dark->texture().handle(), _gi_sampler}};
    SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
    const std::array<f32, 4> flags{colour == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    SDL_PushGPUFragmentUniformData(commands, 0, flags.data(), sizeof(flags));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    SDL_GenerateMipmapsForGPUTexture(commands, base.handle());
    if (emits) {
        SDL_GenerateMipmapsForGPUTexture(commands, dark->texture().handle());
        out.emission = std::move(lit);
    }
    out.base = Texture{
        std::make_shared<GpuTextureBackend>(std::move(base), false, ScaleMode::Mipmapped, TextureFormat::Rgba8, true)};
    return true;
}

void GpuRenderer3D::release_gi_proxies() {
    _gi_proxies.clear();
    _gi_proxy_quad = {};
    _gi_proxy_unlit = {};
    _gi_proxy_dark = {};
    _gi_proxy_depth = {};
    _gi_proxy_shader = {};
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
        u.centre[0] = g.centre.x;
        u.centre[1] = g.centre.y;
        u.centre[2] = g.centre.z;
        u.centre[3] = (g.fixed_outer ? -4.0f : 4.0f) / static_cast<f32>(std::max(g.spacing, 1u));
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
    bool fixed_outer = false;
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
            fixed_outer = true;
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

    g.centre = camera;
    g.fixed_outer = fixed_outer;

    // The draws it takes in and their copies that reach into the area (the skinned in their pose), each with a hash of
    // all it is made of and its box.
    struct Job {
        const Scene3D::Draw* draw;
        const GpuMesh3D* mesh;
        u32 triangles, index_first, base_vertex;
        bool skinned;
        u32 emitter;          // its emitter's number, 0: none (its emission's strength is in its voxels)
        const GiProxy* proxy; // a surface material's stand-in (its maps' place), or null
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
    // The emitters: numbered afresh with a whole build (every voxel is made again), else kept.
    if (whole) {
        g.emitter_of.clear();
        g.emitter_strength.clear();
        g.emitter_tick.clear();
    }
    // A draw's emitter (its material's, `key`), given `strength` this render; 0 when another draw already gave it
    // another strength this render, or there is no number left.
    const auto emitter_for = [&](u64 key, f32 strength) -> u32 {
        const auto [it, fresh] = g.emitter_of.try_emplace(key, 0u);
        if (fresh) {
            if (g.emitter_strength.size() >= max_gi_emitters) {
                g.emitter_of.erase(it);
                return 0u;
            }
            g.emitter_strength.push_back(strength);
            g.emitter_tick.push_back(g.ticks);
            it->second = static_cast<u32>(g.emitter_strength.size());
            return it->second;
        }
        const u32 slot = it->second - 1;
        if (g.emitter_tick[slot] == g.ticks && g.emitter_strength[slot] != strength) {
            return 0u;
        }
        g.emitter_strength[slot] = strength;
        g.emitter_tick[slot] = g.ticks;
        return it->second;
    };
    const std::span<const Mat4f> joints = scene.joint_matrices();
    const std::span<const u32> skin_offsets = scene.skin_offsets();
    bool any_skinned = false;
    for (const Scene3D::Draw& draw : scene.draws()) {
        const Material3D& m = draw.material;
        if (draw.count == 0 || m.unlit || m.wireframe || (m.blend != Blend3D::Opaque && m.blend != Blend3D::Mask) ||
            (draw.skinned && !gi.skinned)) {
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
        material.add(m.emits_light()); // emitter membership, not strength
        material.add(m.double_sided);
        material.add(m.emission_mode);
        material.add(m.emission_color);
        for (const Texture* texture : {&m.texture, &m.emissive_texture}) {
            material.add(texture->backend().get());
            if (const auto* gpu = dynamic_cast<const GpuTextureBackend*>(texture->backend().get())) {
                material.add(gpu->version());
                material.add(gpu->scale_mode());
            }
        }
        for (const u32 slot : {0u, 4u}) {
            const auto& sampler = m.texture_samplers[slot];
            material.add(bool(sampler));
            if (sampler) {
                material.add(sampler->wrap_u);
                material.add(sampler->wrap_v);
                material.add(sampler->min);
                material.add(sampler->mag);
                material.add(sampler->mip);
            }
        }
        material.add(m.wrap_u);
        material.add(m.wrap_v);
        // A surface material is seen through its stand-in, whose emission holds its strength (as its surface makes it).
        const GiProxy* proxy = gi_proxy(m);
        material.add(proxy ? proxy->base.backend().get() : nullptr);
        material.add(proxy ? proxy->emission.backend().get() : nullptr);
        const f32 strength = proxy && proxy->emission ? 1.0f : m.emissive;
        // An emissive draw's voxels hold its emission at a strength of one, and its emitter's number: the GI's light
        // takes the strength from the emitters' list each render, so a strength that changes (a lamp dimmed, a game's
        // exposure applied to its emissives) makes nothing again. Without a number to give, its strength stays in its
        // voxels, and a change of it is a change of what they are made of.
        const u32 emitter = m.emits_light() ? emitter_for(material.value, strength) : 0u;
        if (m.emits_light() && emitter == 0) {
            material.add(strength);
        }
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
                            static_cast<u32>(mesh->vertex_offset / sizeof(GpuMeshVertex)), skinned, emitter, proxy});
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
        const Color colour = jobs[c.job].proxy ? colors::white : m.color; // (a stand-in has the material's in it)
        record.colour[0] = srgb_to_linear(colour.r) * srgb_to_linear(tint.r);
        record.colour[1] = srgb_to_linear(colour.g) * srgb_to_linear(tint.g);
        record.colour[2] = srgb_to_linear(colour.b) * srgb_to_linear(tint.b);
        record.colour[3] = static_cast<f32>(colour.a) / 255.0f * static_cast<f32>(tint.a) / 255.0f;
        record.skin[0] = c.skin;
        return record;
    };
    // What changed: the copies that came or went (a copy that moved or changed is one of each), by their hashes.
    const bool same_copies = !whole && copies.size() == g.copy_order.size() &&
                             std::equal(copies.begin(), copies.end(), g.copy_order.begin(),
                                        [](const Copy& c, u64 previous) { return c.hash == previous; });
    std::vector<std::pair<u64, Bounds3D>> now;
    std::vector<Bounds3D> changed;
    if (!same_copies) {
        now.reserve(copies.size());
        g.copy_order.reserve(copies.size());
        for (const Copy& c : copies) {
            now.emplace_back(c.hash, c.box);
        }
        std::sort(now.begin(), now.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
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
    std::array<u32, max_gi_levels> made_per_level{};
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
                        ++made_per_level[k];
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
    if (!same_copies) {
        g.copies = std::move(now);
        g.copy_order.resize(copies.size());
        std::transform(copies.begin(), copies.end(), g.copy_order.begin(), [](const Copy& c) { return c.hash; });
    }
    // Area lights (emitters Exact): the copies' triangles and emitters, made again when the copies change; nothing
    // without a copy that can be one (an emitter the GI numbers, without emissive or base-colour maps), so that such a
    // scene costs what it does with the probes alone.
    const auto& area_settings = scene.global_illumination.area_lights;
    const bool exact = scene.global_illumination.emitters == GiEmitters3D::Exact;
    // The emitters (each copy's emissive triangles in world space, a group of their own for the pairing) and those of
    // them that light as area lights: made first, so that without one nothing more is (the occluders' hierarchy
    // neither).
    const f32 smallest = area_settings.min_size > 0.0f ? area_settings.min_size : 0.5f * cell;
    // A GI emitter none of whose copies can have a piece min_size across is never kept, so its triangles are not even
    // gathered: a bound per copy from its mesh's largest piece (area_light_pieces, found once a mesh) and the most its
    // transform grows an area (area_growth); nor are those with more pieces than max_emitters. The kept are those the
    // builder keeps.
    const auto collect_emitting = [&] {
        std::unordered_set<u32> possible;
        std::unordered_map<u32, u32> pieces; // by GI emitter: all its copies' pieces (past max_emitters: never kept)
        for (const Copy& c : copies) {
            const Job& j = jobs[c.job];
            if (j.skinned || j.mesh->gi_triangles.empty() || j.emitter == 0) {
                continue;
            }
            auto [made, unknown] = g.area_pieces.try_emplace(j.mesh->serial);
            if (unknown) {
                made->second = area_light_pieces(j.mesh->gi_triangles);
            }
            pieces[j.emitter] += made->second.count;
            if (made->second.largest * area_growth(c.instance->transform) >= smallest * smallest) {
                possible.insert(j.emitter);
            }
        }
        std::erase_if(possible, [&](u32 e) { return pieces[e] > area_settings.max_emitters; });
        std::vector<AreaLightEmitterTriangle> emitting;
        for (u32 ci = 0; ci < copies.size() && !possible.empty(); ++ci) {
            const Copy& c = copies[ci];
            const Job& j = jobs[c.job];
            if (j.skinned || j.mesh->gi_triangles.empty() || j.emitter == 0 || !possible.contains(j.emitter)) {
                continue;
            }
            const std::vector<Vec3f>& corners = j.mesh->gi_triangles;
            const Mat4f& t = c.instance->transform;
            const Material3D& m = j.draw->material;
            const bool independent = m.emission_mode == EmissionMode3D::Independent;
            if (j.proxy || m.emissive_texture.backend() || (!independent && m.texture.backend())) {
                continue; // (an emissive map's light is the voxels', as a surface's stand-in's)
            }
            const Color tint = c.instance->tint;
            const Vec3f radiance = independent ? Vec3f{m.emission_color.r, m.emission_color.g, m.emission_color.b}
                                               : Vec3f{srgb_to_linear(m.color.r) * srgb_to_linear(tint.r),
                                                       srgb_to_linear(m.color.g) * srgb_to_linear(tint.g),
                                                       srgb_to_linear(m.color.b) * srgb_to_linear(tint.b)};
            const Vec3f c0{t.m[0], t.m[1], t.m[2]}, c1{t.m[4], t.m[5], t.m[6]}, c2{t.m[8], t.m[9], t.m[10]};
            const bool mirrored = dot(c0, cross(c1, c2)) < 0.0f;
            for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
                const Vec3f a = t.apply(corners[i]), b = t.apply(corners[i + 1]), d = t.apply(corners[i + 2]);
                emitting.push_back({a, mirrored ? d : b, mirrored ? b : d, radiance, j.emitter, m.double_sided, ci});
            }
        }
        return emitting;
    };
    // Made again only when an emitting copy (or what decides which emitters are kept) changed: copies that move
    // without emitting (a game's characters, every render) change nothing here.
    if (exact) {
        u64 signature =
            0x2545f4914f6cdd1dull + std::bit_cast<u32>(smallest) * 0x9e3779b97f4a7c15ull + area_settings.max_emitters;
        for (const Copy& c : copies) {
            if (jobs[c.job].emitter != 0) {
                signature += (c.hash * 0xff51afd7ed558ccdull) ^ (c.hash >> 29); // order does not matter
            }
        }
        if (!g.area_scanned || signature != g.area_emitter_signature) {
            g.area_lights = build_area_light_emitters(collect_emitting(), area_settings.max_emitters, smallest);
            g.area_emitting = g.area_lights.count > 0;
            g.area_emitter_signature = signature;
            g.area_scanned = true;
        }
    }
    if (!exact || !g.area_emitting) {
        if (g.area_triangles || g.area_cache || g.area) {
            // What the area lights kept, given back (the probes have the emitters now).
            g.area_triangles = g.area_nodes = g.area_emitters = g.area_explicit = g.area_scene = g.area_shading = {};
            g.area_retry_args = g.area_retry_list = {};
            g.area_cache = {};
            g.area = {};
            g.area_before = {};
            g.area_has_before = false;
            g.area_meshes.clear();
            g.area_occluders.clear();
            g.area_pieces.clear();
            g.area_closed.clear();
            g.area_seen.clear();
            g.area_mesh_triangles = {};
            g.area_mesh_nodes = {};
            g.area_settled_signature = 0;
            g.area_settled_triangles = 0;
        }
        g.area_built = false;
        g.area_emitter_count = g.area_explicit_count = 0;
        g.area_scanned = g.area_scanned && exact; // (the copies may change while the probes have the emitters)
    } else if (!same_copies || !g.area_built || (g.area_moving > 0 && g.ticks - g.area_last_change >= area_settle)) {
        if (!same_copies) {
            g.area_last_change = g.ticks;
        }
        // Each mesh's hierarchy once (kept while it is drawn), for the moving copies; the settled ones' when they
        // change; the copies' and the emitters every time.
        const bool first = g.area_seen.empty();
        std::unordered_map<u64, u32> seen;
        seen.reserve(copies.size());
        for (const Copy& c : copies) {
            const auto it = g.area_seen.find(c.hash);
            seen[c.hash] = it != g.area_seen.end() ? it->second : (first ? g.ticks - area_settle : g.ticks);
        }
        g.area_seen = std::move(seen);
        const auto settled = [&](const Copy& c) { return g.ticks - g.area_seen[c.hash] >= area_settle; };
        // The occluders' budget (area_lights.max_triangles): the fewest halvings of every mesh's triangles (but the
        // emitters', which stay whole) that bring all the copies' within it.
        std::size_t occluding = 0;
        for (const Copy& c : copies) {
            occluding += jobs[c.job].skinned ? 0 : jobs[c.job].mesh->gi_triangles.size() / 3;
        }
        u32 coarse = 0;
        while (coarse < 8 && (occluding >> coarse) > std::max<u32>(area_settings.max_triangles, 1)) {
            ++coarse;
        }
        if (coarse != g.area_coarse) {
            KIN_LOG_INFO_F("render", "3D: area lights' occluders made coarser",
                           (LogFields{{.name = "triangles", .value = std::to_string(occluding)},
                                      {.name = "budget", .value = std::to_string(area_settings.max_triangles)},
                                      {.name = "halvings", .value = std::to_string(coarse)}}));
            g.area_coarse = coarse;
        }
        u64 signature = 0x9e3779b97f4a7c15ull + coarse * 0xc2b2ae3d27d4eb4full;
        for (const Copy& c : copies) {
            if (settled(c)) {
                signature += (c.hash * 0xff51afd7ed558ccdull) ^ (c.hash >> 29); // order does not matter
            }
        }
        const auto gather = [&](bool& made_meshes, std::size_t& used) {
            std::vector<AreaLightCopy> area_copies;
            std::vector<AreaLightTriangle> settled_triangles;
            const bool remake_settled = signature != g.area_settled_signature;
            constexpr f32 far = std::numeric_limits<f32>::max();
            Bounds3D settled_box{{far, far, far}, {-far, -far, -far}};
            used = 0;
            g.area_moving = 0;
            for (u32 ci = 0; ci < copies.size(); ++ci) {
                const Copy& c = copies[ci];
                const Job& j = jobs[c.job];
                if (j.skinned || j.mesh->gi_triangles.empty()) {
                    continue;
                }
                const std::vector<Vec3f>& corners = j.mesh->gi_triangles;
                // Its occluder: its triangles, or (past the budget, but for an emitter's) a coarser shape of them.
                const u32 level = j.emitter == 0 ? coarse : 0;
                const u64 occluder = j.mesh->serial * 16 + level;
                const std::vector<Vec3f>* blockers = &corners;
                if (level > 0) {
                    auto [o, made] = g.area_occluders.try_emplace(occluder);
                    if (made) {
                        o->second = simplify_area_light_occluder(corners, (corners.size() / 3) >> level);
                    }
                    blockers = &o->second;
                }
                auto [closed_at, unknown] = g.area_closed.try_emplace(occluder, false);
                if (unknown) {
                    closed_at->second = area_light_closed(*blockers);
                }
                // (Not for a double-sided material: its inside can be seen, and lit, from within.)
                const bool closed = closed_at->second && !j.draw->material.double_sided;
                const Mat4f& t = c.instance->transform;
                const bool still = settled(c);
                used += blockers->size() / 3;
                if (still) {
                    if (remake_settled) {
                        const std::vector<Vec3f>& q = *blockers;
                        for (std::size_t i = 0; i + 2 < q.size(); i += 3) {
                            settled_triangles.push_back({t.apply(q[i]), t.apply(q[i + 1]), t.apply(q[i + 2]), closed});
                        }
                        settled_box.min = {std::min(settled_box.min.x, c.box.min.x),
                                           std::min(settled_box.min.y, c.box.min.y),
                                           std::min(settled_box.min.z, c.box.min.z)};
                        settled_box.max = {std::max(settled_box.max.x, c.box.max.x),
                                           std::max(settled_box.max.y, c.box.max.y),
                                           std::max(settled_box.max.z, c.box.max.z)};
                    }
                } else {
                    ++g.area_moving;
                    // (By mesh and closedness: a mesh drawn double-sided too has a hierarchy of each kind.)
                    auto [it, fresh] = g.area_meshes.try_emplace(occluder * 2 + (closed ? 1 : 0), std::array<u32, 2>{});
                    if (fresh) {
                        const std::vector<Vec3f>& q = *blockers;
                        std::vector<AreaLightTriangle> own(q.size() / 3);
                        for (std::size_t i = 0; i < own.size(); ++i) {
                            own[i] = {q[i * 3], q[i * 3 + 1], q[i * 3 + 2], closed};
                        }
                        const AreaLightMesh mesh = build_area_light_mesh(own);
                        it->second = {static_cast<u32>(g.area_mesh_triangles.size() / area_light_floats_triangle),
                                      static_cast<u32>(g.area_mesh_nodes.size() / area_light_floats_node)};
                        g.area_mesh_triangles.insert(g.area_mesh_triangles.end(), mesh.triangles.begin(),
                                                     mesh.triangles.end());
                        g.area_mesh_nodes.insert(g.area_mesh_nodes.end(), mesh.nodes.begin(), mesh.nodes.end());
                        made_meshes = true;
                    }
                    area_copies.push_back({t, c.box.min, c.box.max, it->second[1], it->second[0]});
                }
            }
            if (remake_settled) {
                g.area_settled_signature = signature;
                g.area_settled_triangles = static_cast<u32>(settled_triangles.size());
                g.area_settled_box = settled_box;
                if (!settled_triangles.empty()) {
                    const AreaLightMesh mesh = build_area_light_mesh(settled_triangles);
                    g.area_settled_at = {static_cast<u32>(g.area_mesh_triangles.size() / area_light_floats_triangle),
                                         static_cast<u32>(g.area_mesh_nodes.size() / area_light_floats_node)};
                    g.area_mesh_triangles.insert(g.area_mesh_triangles.end(), mesh.triangles.begin(),
                                                 mesh.triangles.end());
                    g.area_mesh_nodes.insert(g.area_mesh_nodes.end(), mesh.nodes.begin(), mesh.nodes.end());
                    made_meshes = true;
                }
            }
            if (g.area_settled_triangles > 0) {
                area_copies.push_back({Mat4f::identity(), g.area_settled_box.min, g.area_settled_box.max,
                                       g.area_settled_at[1], g.area_settled_at[0], /*world=*/true});
            }
            return area_copies;
        };
        bool made_meshes = false;
        std::size_t used = 0;
        std::vector<AreaLightCopy> area_copies = gather(made_meshes, used);
        if (g.area_mesh_triangles.size() / area_light_floats_triangle > 2 * used + 65536) {
            // Mostly meshes no longer drawn: make the kept hierarchies again from those still drawn.
            g.area_meshes.clear();
            g.area_closed.clear();
            g.area_occluders.clear();
            g.area_pieces.clear();
            g.area_mesh_triangles.clear();
            g.area_mesh_nodes.clear();
            g.area_settled_signature = 0;
            made_meshes = false;
            area_copies = gather(made_meshes, used);
            made_meshes = true;
        }
        const auto upload = [&](std::shared_ptr<GpuBuffer>& buffer, const void* bytes, std::size_t size) {
            const u32 n = static_cast<u32>(std::max<std::size_t>(size, 16));
            ensure_buffer(buffer, n, 1024, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
            if (size > 0) {
                _device->upload_storage_buffer(buffer->handle(), 0, static_cast<u32>(size), bytes, /*cycle=*/true);
            }
        };
        if (made_meshes || !g.area_triangles) {
            upload(g.area_triangles, g.area_mesh_triangles.data(), g.area_mesh_triangles.size() * sizeof(f32));
            upload(g.area_nodes, g.area_mesh_nodes.data(), g.area_mesh_nodes.size() * sizeof(f32));
        }
        const AreaLightCopies scene_data = build_area_light_copies(area_copies);
        upload(g.area_scene, scene_data.data.data(), scene_data.data.size() * sizeof(f32));
        g.area_copies_at = scene_data.node_count * 2;
        const AreaLightEmitters& lights = g.area_lights;
        upload(g.area_emitters, lights.data.data(), lights.data.size() * sizeof(f32));
        std::vector<u32> flags(std::max<std::size_t>(g.emitter_strength.size(), 1), 0u);
        for (const u32 e : lights.explicit_emitters) {
            if (e >= 1 && e <= flags.size()) {
                flags[e - 1] = 1u;
            }
        }
        upload(g.area_explicit, flags.data(), flags.size() * sizeof(u32));
        g.area_emitter_count = scene_data.copy_count > 0 ? lights.count : 0;
        // The voxels' kept light is for these emitters and the settled copies' shadows: a new epoch when either
        // changes, not when only moving copies move (a moving copy's own blocks are made again, and lit afresh; its
        // shadow on the rest of the voxels follows when the next change comes).
        if (made_meshes || lights.data != g.area_emitter_data) {
            g.area_epoch = g.area_epoch % 1024 + 1;
        }
        g.area_emitter_data = lights.data;
        g.area_explicit_count = static_cast<u32>(flags.size());
        g.area_built = true;
        if (made_meshes) {
            KIN_LOG_INFO_F(
                "render", "3D: area lights' meshes made",
                (LogFields{{.name = "meshes", .value = std::to_string(g.area_meshes.size())},
                           {.name = "settled", .value = std::to_string(g.area_settled_triangles)},
                           {.name = "moving", .value = std::to_string(g.area_moving)},
                           {.name = "triangles",
                            .value = std::to_string(g.area_mesh_triangles.size() / area_light_floats_triangle)},
                           {.name = "copies", .value = std::to_string(scene_data.copy_count)},
                           {.name = "emitters", .value = std::to_string(lights.count)},
                           {.name = "explicit", .value = std::to_string(lights.explicit_emitters.size())}}));
        }
    }
    // The emitters' strengths this render, for the GI's light and its view.
    {
        const u32 bytes = static_cast<u32>(std::max<std::size_t>(g.emitter_strength.size(), 1) * sizeof(f32));
        ensure_buffer(_gi_emitters, bytes, 256 * sizeof(f32),
                      SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
        if (!g.emitter_strength.empty()) {
            _device->upload_storage_buffer(_gi_emitters->handle(), 0, bytes, g.emitter_strength.data(), /*cycle=*/true);
        }
    }
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
    std::array<GiBlocks, max_gi_levels> voxel_bounds{};
    for (u32 k = 0; k < levels; ++k) {
        std::vector<LevelJob>& list = level_jobs[k];
        std::stable_sort(list.begin(), list.end(), [&](const LevelJob& a, const LevelJob& b) {
            const Job &x = jobs[a.job], &y = jobs[b.job];
            return std::tuple{x.mesh->vertices, x.mesh->indices, x.skinned ? x.mesh->skin : nullptr} <
                   std::tuple{y.mesh->vertices, y.mesh->indices, y.skinned ? y.mesh->skin : nullptr};
        });
        GiBlocks& round = voxel_bounds[k];
        for (const GiBlocks& b : cores[k]) {
            round = round.empty() ? b : round_both(round, b);
        }
    }
    const auto voxelize = [&](SDL_GPUComputePass* pass, bool attributes) {
        for (u32 k = 0; k < levels; ++k) {
            const std::vector<LevelJob>& list = level_jobs[k];
            const f32 c = cell * static_cast<f32>(1u << k);
            const GiBlocks& round = voxel_bounds[k];
            const u32 box_blocks = static_cast<u32>((round.hi[0] - round.lo[0]) * (round.hi[1] - round.lo[1]) *
                                                    (round.hi[2] - round.lo[2]));
            // The union fills its box on full builds and single slabs; only disjoint/concave unions need a mask.
            const bool dense = made_per_level[k] == box_blocks;
            for (const LevelJob& lj : list) {
                const Job& job = jobs[lj.job];
                const Material3D& m = job.draw->material;
                const GiProxy* proxy = job.proxy;
                const bool proxy_emits = proxy && proxy->emission;
                const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
                    proxy ? texture_binding(proxy->base, Wrap3D::Repeat, Wrap3D::Repeat)
                          : texture_binding(m.texture, m.wrap_u, m.wrap_v, m.texture_samplers[0]),
                    proxy_emits ? texture_binding(proxy->emission, Wrap3D::Repeat, Wrap3D::Repeat)
                                : texture_binding(m.emissive_texture, m.wrap_u, m.wrap_v, m.texture_samplers[4])};
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
                u.low[3] = dense ? 1 : 0;
                u.size[3] = lj.item_first;
                u.window[3] = static_cast<i32>(k * size[2]);
                u.mesh[0] = job.index_first;
                u.mesh[1] = job.triangles;
                u.mesh[2] = job.base_vertex;
                u.mesh[3] = lj.copies;
                u.flags[0] = job.skinned ? 1u : 0u;
                u.flags[1] = m.blend == Blend3D::Mask ? 1u : 0u;
                u.flags[2] = decode ? 1u : 0u;
                u.flags[3] = proxy_emits ? 2u : m.emissive_texture.backend() ? 1u : 0u; // (2: linear, not encoded)
                u.material[0] = m.emits_light() ? (proxy_emits ? 1.0f : m.emissive) : 0.0f;
                u.material[3] = static_cast<f32>(job.emitter);
                u.material[1] = m.alpha_cutoff;
                u.material[2] = m.double_sided ? 1.0f : 0.0f;
                u.emission[0] = proxy_emits ? 1.0f : m.emission_color.r;
                u.emission[1] = proxy_emits ? 1.0f : m.emission_color.g;
                u.emission[2] = proxy_emits ? 1.0f : m.emission_color.b;
                u.emission[3] = proxy_emits || m.emission_mode == EmissionMode3D::Independent ? 1.0f : 0.0f;
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

std::array<SDL_GPUTextureSamplerBinding, 3> GpuRenderer3D::gi_bindings(const GpuView3D& view, bool resolved) const {
    const GpuView3D::Gi& g = view._gi;
    if (!resolved || !g.light) {
        const SDL_GPUTextureSamplerBinding flat{_owner->_white.handle(), _owner->_sampler_linear};
        return {flat, flat, flat};
    }
    return {
        SDL_GPUTextureSamplerBinding{g.light.handle(), _gi_linear},
        SDL_GPUTextureSamplerBinding{g.slope.handle(), _gi_linear},
        SDL_GPUTextureSamplerBinding{g.area_resolved && g.area ? g.area.handle() : g.coverage.handle(), _gi_linear}};
}

// gi_area.comp's Params.
struct GiAreaUniforms {
    f32 to_world[16];
    f32 unproject_xy[4];
    f32 unproject_zw[4];
    f32 sizes[4];
    f32 eye[4];
    f32 area[4];  // emitters, the light's strength, the shadow rays' offset (base, a unit away)
    f32 scene[4]; // where the copies start in the scene buffer (vec4s); the settled hierarchy (see GiLightUniforms)
    f32 mode[4];  // x: 0 every texel, 1 half, 2 a quarter; y: the half's parity or the quarter's column; z: its row
    f32 share[4]; // the share's tiles across, its tiles, the list's first word
};
static_assert(sizeof(GiAreaUniforms) == 192);
// gi_area_fill.comp's Params.
struct GiAreaFillUniforms {
    f32 to_world[16];
    f32 unproject_xy[4];
    f32 unproject_zw[4];
    f32 sizes[4];
    f32 view_projection[16]; // without the jitter
    f32 last_view_projection[16];
    f32 last_eye[4]; // w: 1 with a render before
    f32 last_forward[4];
    f32 retry[4]; // the share's tiles; this turn's and the other's list's first word; this turn's arguments' first
    f32 mode[4];  // x: 2 a quarter, else half; y: the half's parity, or the quarter's column (w its row); z: 1
};
static_assert(sizeof(GiAreaFillUniforms) == 304);

bool GpuRenderer3D::gi_area(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Scene3D& scene,
                            const Mat4f& projection, RenderDebugStats3D& stats) {
    GpuView3D::Gi& g = view._gi;
    if (scene.global_illumination.emitters != GiEmitters3D::Exact || !g.area_built || g.area_emitter_count == 0 ||
        !_gi_area || !g.coverage || !g.area_triangles || !g.area_nodes || !g.area_emitters || !g.area_scene ||
        !_gi_emitters) {
        return false;
    }
    const u32 hw = g.coverage.width(), hh = g.coverage.height();
    const u32 interleave = scene.global_illumination.area_lights.interleave;
    const bool checker = interleave > 1 && _gi_area_fill;
    const bool quarter = interleave >= 4;
    // This render's share as a grid of its own (each texel a thread, packed), in 8 x 8 tiles: half the columns, and a
    // quarter's half the rows too.
    const u32 share_across = ((hw + 1) / 2 + 7) / 8,
              share_tiles = share_across * (((quarter ? (hh + 1) / 2 : hh) + 7) / 8);
    try {
        const SDL_GPUTextureUsageFlags usage =
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        for (GpuTexture* t : {&g.area, &g.area_before}) {
            if ((t == &g.area || checker) && (!*t || t->width() != hw || t->height() != hh)) {
                *t = _device->create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
                g.area_has_before = false;
            }
        }
        if (!g.area_retry_list || g.area_retry_texels != hw * hh || g.area_retry_tiles != share_tiles) {
            const std::array<u32, 8> args{share_tiles, 1, 1, 0, share_tiles, 1, 1, 0};
            g.area_retry_args = std::make_shared<GpuBuffer>(
                _device->create_buffer(SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                           SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                       args.data(), sizeof(args)));
            const std::vector<u32> lists(2 * (std::size_t{hw} * hh + 1), 0u);
            g.area_retry_list = std::make_shared<GpuBuffer>(_device->create_buffer(
                SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, lists.data(),
                static_cast<u32>(lists.size() * sizeof(u32))));
            g.area_retry_texels = hw * hh;
            g.area_retry_tiles = share_tiles;
            g.area_retry_turn = 0;
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the area lights' light on the view could not be made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        g.area = {};
        g.area_before = {};
        g.area_has_before = false;
        g.area_retry_list = {};
        g.area_retry_args = {};
        return false;
    }
    // Interleaved, with a render before (its whole kept, the light's depths both renders', the temporal anti-aliasing's
    // normals of it): the fill takes what it can from that render and lists the rest, then one dispatch finds this
    // render's share and the listed texels. Else every texel, in one dispatch.
    const bool halve = checker && g.area_has_before && g.last_light && g.last_light.width() == hw &&
                       g.last_light.height() == hh && view._taa_motion;
    constexpr f32 quarter_x[4] = {0.0f, 1.0f, 1.0f, 0.0f}, quarter_y[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    const u32 phase = g.area_parity & 3u;
    if (checker) {
        std::swap(g.area, g.area_before);
    }
    const GiMarker marker{commands, _gpu_markers, "kin3d.gi_area"};
    // The view-projection without the temporal anti-aliasing's jitter: what the fill measures motion with.
    const Mat4f steady =
        scene.camera.projection_matrix(static_cast<f32>(view._size.x) / static_cast<f32>(std::max(view._size.y, 1))) *
        scene.camera.view();
    GiAreaUniforms u{};
    const Camera3D& camera = scene.camera;
    std::memcpy(u.to_world, camera.view().inverse().m, sizeof(u.to_world));
    const Mat4f inverse = projection.inverse();
    const f32 xy[4] = {inverse.at(0, 0), inverse.at(1, 1), inverse.at(0, 3), inverse.at(1, 3)};
    const f32 zw[4] = {inverse.at(2, 2), inverse.at(2, 3), inverse.at(3, 2), inverse.at(3, 3)};
    std::memcpy(u.unproject_xy, xy, sizeof(xy));
    std::memcpy(u.unproject_zw, zw, sizeof(zw));
    const f32 sizes[4] = {static_cast<f32>(hw), static_cast<f32>(hh), static_cast<f32>(view._ao_depth.width()),
                          static_cast<f32>(view._ao_depth.height())};
    std::memcpy(u.sizes, sizes, sizeof(sizes));
    const f32 eye[4] = {camera.position.x, camera.position.y, camera.position.z, 0.0f};
    std::memcpy(u.eye, eye, sizeof(eye));
    // Shadow rays leave a little off the surface: the depth's and the normal's error grow with the distance, and an
    // orthographic view's with its depth range (its depth is linear over it: 10 km without a far plane, about half a
    // millimetre a step near the camera, enough for a floor to shade itself).
    f32 lift = 1e-4f;
    if (camera.projection == Projection3D::Orthographic) {
        lift += 5e-7f * (std::isinf(camera.z_far) ? 10000.0f : std::max(camera.z_far - camera.z_near, 0.0f));
    }
    const f32 area[4] = {static_cast<f32>(g.area_emitter_count), scene.global_illumination.probes.intensity, lift,
                         2e-4f};
    std::memcpy(u.area, area, sizeof(area));
    u.scene[0] = static_cast<f32>(g.area_copies_at);
    u.scene[1] = static_cast<f32>(g.area_settled_at[1]);
    u.scene[2] = static_cast<f32>(g.area_settled_at[0]);
    u.scene[3] = g.area_settled_triangles > 0 ? 1.0f : 0.0f;
    u.mode[0] = halve ? (quarter ? 2.0f : 1.0f) : 0.0f;
    u.mode[1] = quarter ? quarter_x[phase] : static_cast<f32>(g.area_parity & 1u);
    u.mode[2] = quarter ? quarter_y[phase] : 0.0f;
    const u32 turn = g.area_retry_turn, list_words = hw * hh + 1;
    u.share[0] = static_cast<f32>(share_across);
    u.share[1] = static_cast<f32>(share_tiles);
    u.share[2] = static_cast<f32>(turn * list_words);
    if (halve) {
        GiAreaFillUniforms f{};
        std::memcpy(f.to_world, u.to_world, sizeof(f.to_world));
        std::memcpy(f.unproject_xy, u.unproject_xy, sizeof(f.unproject_xy));
        std::memcpy(f.unproject_zw, u.unproject_zw, sizeof(f.unproject_zw));
        std::memcpy(f.sizes, u.sizes, sizeof(f.sizes));
        std::memcpy(f.view_projection, steady.m, sizeof(f.view_projection));
        std::memcpy(f.last_view_projection, g.area_before_view_projection.m, sizeof(f.last_view_projection));
        const f32 last_eye[4] = {g.area_before_eye.x, g.area_before_eye.y, g.area_before_eye.z, 1.0f};
        const f32 last_way[4] = {g.area_before_forward.x, g.area_before_forward.y, g.area_before_forward.z, 0.0f};
        std::memcpy(f.last_eye, last_eye, sizeof(last_eye));
        std::memcpy(f.last_forward, last_way, sizeof(last_way));
        f.retry[0] = static_cast<f32>(share_tiles);
        f.retry[1] = static_cast<f32>(turn * list_words);
        f.retry[2] = static_cast<f32>((turn ^ 1u) * list_words);
        f.retry[3] = static_cast<f32>(turn * 4u);
        f.mode[0] = quarter ? 2.0f : 1.0f;
        f.mode[1] = quarter ? quarter_x[phase] : static_cast<f32>(g.area_parity & 1u);
        f.mode[3] = quarter ? quarter_y[phase] : 0.0f;
        f.mode[2] = 1.0f; // the render before's normals (view._taa_motion, as gi_resolve reads them)
        const SDL_GPUStorageTextureReadWriteBinding whole{.texture = g.area.handle(), .cycle = true};
        const std::array<SDL_GPUStorageBufferReadWriteBinding, 2> lists{
            SDL_GPUStorageBufferReadWriteBinding{.buffer = g.area_retry_args->handle()},
            SDL_GPUStorageBufferReadWriteBinding{.buffer = g.area_retry_list->handle()}};
        SDL_GPUComputePass* fill = SDL_BeginGPUComputePass(commands, &whole, 1, lists.data(), 2);
        if (!fill) {
            return false;
        }
        const std::array<SDL_GPUTextureSamplerBinding, 7> taken{
            SDL_GPUTextureSamplerBinding{view._ao_depth.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.area_before.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.light.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.last_light.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.coverage.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{view._taa_motion.handle(), _gi_sampler}};
        SDL_BindGPUComputePipeline(fill, _gi_area_fill);
        SDL_BindGPUComputeSamplers(fill, 0, taken.data(), static_cast<u32>(taken.size()));
        SDL_PushGPUComputeUniformData(commands, 0, &f, sizeof(f));
        SDL_DispatchGPUCompute(fill, (hw + 7) / 8, (hh + 7) / 8, 1);
        SDL_EndGPUComputePass(fill);
        ++stats.gi_dispatches;
    }
    // (Interleaved, the fill wrote the other texels of this render's whole: kept, no cycling.)
    const SDL_GPUStorageTextureReadWriteBinding out{.texture = g.area.handle(), .cycle = !halve};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, &out, 1, nullptr, 0);
    if (!pass) {
        return false;
    }
    const std::array<SDL_GPUTextureSamplerBinding, 3> samplers{
        SDL_GPUTextureSamplerBinding{view._ao_depth.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{g.coverage.handle(), _gi_sampler}};
    const std::array<SDL_GPUBuffer*, 6> buffers{g.area_triangles->handle(), g.area_nodes->handle(),
                                                g.area_emitters->handle(),  _gi_emitters->handle(),
                                                g.area_scene->handle(),     g.area_retry_list->handle()};
    SDL_BindGPUComputePipeline(pass, _gi_area);
    SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
    SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), static_cast<u32>(buffers.size()));
    SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
    if (halve) {
        // The share's tiles and the list's groups, as the fill counted them.
        SDL_DispatchGPUComputeIndirect(pass, g.area_retry_args->handle(), turn * 4u * sizeof(u32));
        g.area_retry_turn ^= 1u;
    } else {
        SDL_DispatchGPUCompute(pass, (hw + 7) / 8, (hh + 7) / 8, 1);
    }
    SDL_EndGPUComputePass(pass);
    ++stats.gi_dispatches;
    g.area_before_view_projection = steady;
    g.area_before_eye = camera.position;
    g.area_before_forward = camera.forward();
    g.area_has_before = checker;
    g.area_parity = quarter ? (g.area_parity + 1u) & 3u : (g.area_parity ^ 1u) & 1u;
    // The emitters for the main pass's reflections: the GI's strengths of this render applied to their radiance.
    std::vector<f32> shading(g.area_emitter_data.begin(),
                             g.area_emitter_data.begin() +
                                 std::min<std::size_t>(g.area_emitter_data.size(),
                                                       std::size_t{g.area_emitter_count} * area_light_floats_emitter));
    for (std::size_t e = 0; e + area_light_floats_emitter <= shading.size(); e += area_light_floats_emitter) {
        const u32 number = static_cast<u32>(shading[e + 7] + 0.5f);
        const f32 strength = number > 0 && number <= g.emitter_strength.size() ? g.emitter_strength[number - 1] : 1.0f;
        for (int c = 16; c < 19; ++c) {
            shading[e + c] *= strength;
        }
    }
    const u32 bytes = static_cast<u32>(std::max<std::size_t>(shading.size() * sizeof(f32), 16));
    ensure_buffer(g.area_shading, bytes, 1024, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
    if (!shading.empty()) {
        _device->upload_storage_buffer(g.area_shading->handle(), 0, bytes, shading.data(), /*cycle=*/true);
    }
    return true;
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
    const bool keep = jittered && view._taa_valid && view._taa_motion && g.has_last && g.last_light &&
                      g.last_light.width() == hw && g.last_light.height() == hh;
    try {
        if (!g.light || g.light.width() != hw || g.light.height() != hh) {
            const SDL_GPUTextureUsageFlags usage =
                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            g.light = _device->create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
            g.slope = _device->create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, usage);
        }
        if (!g.coverage || g.coverage.width() != hw || g.coverage.height() != hh) {
            g.coverage =
                _device->create_target(hw, hh, SDL_GPU_TEXTUREFORMAT_R16_FLOAT,
                                       SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the GI's light on the view could not be made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        g.light = {};
        g.slope = {};
        g.last_light = {};
        g.last_slope = {};
        g.coverage = {};
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
    const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> out{
        SDL_GPUStorageTextureReadWriteBinding{.texture = g.light.handle(), .cycle = true},
        SDL_GPUStorageTextureReadWriteBinding{.texture = g.slope.handle(), .cycle = true},
        SDL_GPUStorageTextureReadWriteBinding{.texture = g.coverage.handle(), .cycle = true}};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, out.data(), static_cast<u32>(out.size()), nullptr, 0);
    if (!pass) {
        return false;
    }
    const SDL_GPUTextureSamplerBinding flat{_owner->_white.handle(), _owner->_sampler_linear};
    const std::array<SDL_GPUTextureSamplerBinding, 8> samplers{
        SDL_GPUTextureSamplerBinding{view._ao_depth.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _gi_sampler},
        SDL_GPUTextureSamplerBinding{g.irradiance.handle(), _gi_linear},
        SDL_GPUTextureSamplerBinding{g.moments.handle(), _gi_linear},
        SDL_GPUTextureSamplerBinding{g.probe_data.handle(), _gi_sampler},
        keep ? SDL_GPUTextureSamplerBinding{g.last_light.handle(), _gi_linear} : flat,
        keep ? SDL_GPUTextureSamplerBinding{g.last_slope.handle(), _gi_linear} : flat,
        keep ? SDL_GPUTextureSamplerBinding{view._taa_motion.handle(), _gi_sampler} : flat};
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
            g.area_cache = {};
            g.area_epoch = g.area_epoch % 1024 + 1;
            g.probe_counts = counts;
            g.spacing = spacing;
            g.probe_levels = levels;
            g.next = {};
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
        if (_gi_sun_on && _gi_sun_map) {
            std::memcpy(u.sun_matrix, _gi_sun_matrix.m, sizeof(u.sun_matrix));
            put4(u.sun_map, 1.0f, _gi_sun_bias, 1.0f / static_cast<f32>(_gi_sun_side), _gi_sun_texel);
        }
        const bool area_on =
            scene.global_illumination.emitters == GiEmitters3D::Exact && g.area_built && g.area_emitter_count > 0;
        if (area_on) {
            // A new epoch when the strengths changed (the cached light has them in it).
            if (g.area_strengths != g.emitter_strength) {
                g.area_strengths = g.emitter_strength;
                g.area_epoch = g.area_epoch % 1024 + 1;
            }
            if (!g.area_cache || g.area_cache.width() != g.radiance.width() ||
                g.area_cache.height() != g.radiance.height()) {
                g.area_cache = _device->create_volume(g.radiance.width(), g.radiance.height(), g.size[2] * g.levels,
                                                      SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                                      SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                                          SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE);
                g.area_epoch = g.area_epoch % 1024 + 1;
            }
            u.count[2] = static_cast<u32>(g.made.size()); // the first entries, made this render: lit afresh
        }
        if (area_on) {
            put4(u.area, static_cast<f32>(g.area_emitter_count), static_cast<f32>(g.area_epoch),
                 static_cast<f32>(g.area_explicit_count), static_cast<f32>(g.area_copies_at));
            put4(u.area_scene, static_cast<f32>(g.area_copies_at), static_cast<f32>(g.area_settled_at[1]),
                 static_cast<f32>(g.area_settled_at[0]), g.area_settled_triangles > 0 ? 1.0f : 0.0f);
        }
        // (Without area lights the cache's binding is a 1 x 1 x 1 stand-in, never touched.)
        const std::array<SDL_GPUStorageTextureReadWriteBinding, 3> radiance{
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.radiance.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = g.radiance_back.handle()},
            SDL_GPUStorageTextureReadWriteBinding{.texture = area_on && g.area_cache ? g.area_cache.handle()
                                                                                     : _area_cache_none.handle()}};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, radiance.data(), 3, nullptr, 0);
        if (!pass) {
            return false;
        }
        const std::array<SDL_GPUTextureSamplerBinding, 3> probes = gi_atlases(view, g.lit);
        const std::array<SDL_GPUTextureSamplerBinding, 8> samplers{
            SDL_GPUTextureSamplerBinding{g.distance.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.albedo.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.normal.handle(), _gi_sampler},
            SDL_GPUTextureSamplerBinding{g.emission.handle(), _gi_sampler},
            probes[0],
            probes[1],
            probes[2],
            SDL_GPUTextureSamplerBinding{_gi_sun_on && _gi_sun_map ? _gi_sun_map.handle() : _no_shadow.handle(),
                                         _compare_sampler}};
        const bool area =
            scene.global_illumination.emitters == GiEmitters3D::Exact && g.area_built && g.area_emitter_count > 0;
        const auto or_empty = [&](const std::shared_ptr<GpuBuffer>& b) {
            return area && b ? b->handle() : _gi_empty->handle();
        };
        const std::array<SDL_GPUBuffer*, 8> buffers{_gi_lights->handle(),
                                                    g.light_list->handle(),
                                                    _gi_emitters ? _gi_emitters->handle() : _gi_empty->handle(),
                                                    or_empty(g.area_triangles),
                                                    or_empty(g.area_nodes),
                                                    or_empty(g.area_emitters),
                                                    or_empty(g.area_explicit),
                                                    or_empty(g.area_scene)};
        SDL_BindGPUComputePipeline(pass, _gi_light);
        SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
        SDL_BindGPUComputeStorageBuffers(pass, 0, buffers.data(), static_cast<u32>(buffers.size()));
        SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
        const std::array<u32, 2> groups = gi_groups(static_cast<u64>(lit_blocks.size()) * 512);
        SDL_DispatchGPUCompute(pass, groups[0], groups[1], 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }

    // 3. This render's batch casts its rays: the probes waiting first (new to their windows: the nearest level's
    // first), then the others in turn, each level its share: 4 of 7 to the first, 2 to the next, 1 to the third (and
    // likewise with other counts), so that the probes round the camera, which light most of the picture, are updated
    // more often than the far, coarse ones, whose light changes less across the view (none twice in a batch: they would
    // race for their tiles).
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
    {
        const u32 room = static_cast<u32>(batch_size - batch.size());
        u32 weights = 0;
        for (u32 k = 0; k < levels; ++k) {
            weights += 1u << (levels - 1 - k);
        }
        u32 given = 0;
        for (u32 k = 0; k < levels; ++k) {
            // The last level takes what the others' rounding left.
            const u32 share = k + 1 == levels ? room - given : room * (1u << (levels - 1 - k)) / weights;
            given += share;
            const u32 until = static_cast<u32>(batch.size()) + share;
            u32& next = g.next[k];
            for (u32 tried = 0; batch.size() < until && tried < per_level; ++tried) {
                const u32 i = next;
                next = (next + 1) % per_level;
                if (g.waiting[k * per_level + i] == 0) {
                    batch.push_back(i | (k << 24));
                }
            }
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
    if (!view._gi.valid || !_gi_view_shader || !_gi_emitters || scene.debug.gi_view == GiView3D::Off) {
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
    SDL_GPUBuffer* emitters = _gi_emitters->handle();
    SDL_BindGPUFragmentStorageBuffers(pass, 0, &emitters, 1);
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
                // An emitter's voxel: its colour times its emitter's strength (the alpha its number).
                std::memcpy(&h, emission.data() + from * 8 + 6, sizeof(h));
                const u32 emitter = static_cast<u32>(std::lround(half_to_float(h)));
                const f32 strength =
                    emitter > 0 && emitter <= gi.emitter_strength.size() ? gi.emitter_strength[emitter - 1] : 1.0f;
                for (std::size_t c = 0; c < 3; ++c) {
                    std::memcpy(&h, emission.data() + from * 8 + c * 2, sizeof(h));
                    out.emission[i * 3 + c] = half_to_float(h) * strength;
                }
            }
        }
    }
    return true;
}

} // namespace kin::gpu
