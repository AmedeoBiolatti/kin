#include "gpu_renderer3d.hpp"

#include "embedded_gpu_shader.hpp"
#include "gpu_device.hpp"
#include "gpu_renderer2d_backend.hpp"

#include <kin/core/jobs.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/shadow_cascades.hpp>
#include <kin/render3d/taa.hpp>
#include <kin/renderer/color.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
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
// Ambient occlusion: the pre-pass's world normals (octahedral, two floats), and the result.
constexpr SDL_GPUTextureFormat ao_normal_format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
constexpr SDL_GPUTextureFormat ao_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
constexpr u64 ao_chain_min_pixels = 1'500'000; // the linear depth chain pays from about 1080p up (1280 x 720 is 0.92 M, 1920 x 1080 2.07 M)
constexpr u32 ao_chain_levels = 5; // the occlusion's linear depth chain
constexpr SDL_GPUTextureFormat hiz_format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT; // the depth pyramid (Lod3D::occlusion)
constexpr u32 max_lights = 16;
// Storage buffers' smallest sizes, so that the first frames of a growing scene don't each remake them.
constexpr u32 min_instance_buffer = 64 * 1024;
constexpr u32 min_previous_buffer = 32 * 1024;
constexpr u32 min_caster_list_buffer = 16 * 1024;
constexpr u32 min_joint_buffer = 64 * 1024;

// What _visible holds for a copy: whether the camera sees it, whether only the sun does (it may cast
// into view), and a bit for each cascade it casts into.
constexpr u8 camera_bit = 1;
constexpr u8 sun_bit = 2;
constexpr u8 in_view_bits = camera_bit | sun_bit;
constexpr u32 first_cascade_bit = 2;
static_assert(first_cascade_bit + max_shadow_cascades <= 8, "_visible is a byte a copy");
constexpr u8 cascade_bit(u32 k) { return static_cast<u8>(1u << (first_cascade_bit + k)); }
constexpr bool in_cascade(u8 visible, u32 k) { return (visible & cascade_bit(k)) != 0; }
// The overlay's slice of the depth range: reverse Z, so in front of the scene's.
constexpr f32 overlay_split = 0.9f;

// mesh.vert's uniforms, a draw.
struct DrawUniforms {
    f32 view_projection[16];
    u32 first[4]; // x: the draw's first instance
};
static_assert(sizeof(DrawUniforms) == 80);

// mesh.frag's: the frame's (slot 0) and the draw's (slot 1). The sizes below
// are those of the std140 blocks in the GLSL (cascade_end and _blend are vec4s):
// change a struct and its shader together.
static_assert(max_shadow_cascades == 4, "mesh.frag's cascade arrays");
struct FrameUniforms {
    f32 sun_direction[4];
    f32 sun_color[4];
    f32 ambient[4];
    f32 ground_ambient[4];
    f32 fill_direction[4];
    f32 fill_color[4];
    f32 flags[4]; // encode to sRGB, decode textures, shadows, point lights
    f32 eye[4];   // the camera, in the scene's world space
    f32 environment[4];         // intensity, the specular's last level, 0, 1 with an environment
    f32 environment_matrix[16]; // the scene's space to the environment's
    f32 shadow_matrix[max_shadow_cascades * 16]; // a cascade each (one map: the first)
    f32 shadow[4];             // cascades, 0, a texel (uv)
    f32 cascade_bias[max_shadow_cascades * 4];   // per cascade: depth bias, normal offset
    f32 cascade_end[max_shadow_cascades];        // the view depth where each cascade ends,
    f32 cascade_blend[max_shadow_cascades];      // and where it starts to fade into the next
    f32 cascade_forward[4];    // the camera's forward
    f32 occlusion[4];          // 1 with ambient occlusion, 0, 1 / the view's size
    f32 lights[max_lights * 2 * 4];
};
static_assert(sizeof(FrameUniforms) == 1120);
// A custom material shader's: kin_param(0..3).
struct CustomUniforms {
    f32 values[16];
};
static_assert(sizeof(CustomUniforms) == 64);
struct MaterialUniforms {
    f32 params[4]; // emissive, unlit, receives the sun's shadow, alpha cutoff (0: none)
    f32 pbr[4];    // metallic, roughness, normal scale, occlusion strength
    f32 maps[4];   // has a normal map, has an emissive map, opaque (in the occlusion pre-pass)
};
static_assert(sizeof(MaterialUniforms) == 48);
// gtao.comp's.
struct OcclusionUniforms {
    f32 view[16];
    f32 unproject_xy[4]; // the inverse projection's a, b, c, d (see gtao.comp)
    f32 unproject_zw[4]; // e, f, g, h
    f32 sizes[4]; // the result's size, the depth's
    f32 shape[4]; // radius, power, pixels a unit spans at depth 1, 1 if orthographic
};
static_assert(sizeof(OcclusionUniforms) == 128);
// ao_blur.comp's.
// ao_depth_chain.comp's.
struct OcclusionChainUniforms {
    f32 unproject_zw[4];
    f32 weights[4]; // x, y: a texel's weight is clamp((farthest - its distance) * x + y, 0, 1)
    u32 sizes[12];  // the levels' sizes (x, y), then padding to 3 vec4
};
static_assert(sizeof(OcclusionChainUniforms) == 80);
struct OcclusionBlurUniforms {
    f32 sizes[4];
    f32 step[4];
    f32 depth[4]; // a view-space z of a depth is (a * depth + b) / (c * depth + d)
};
static_assert(sizeof(OcclusionBlurUniforms) == 48);
// contact.comp's.
struct ContactUniforms {
    f32 view[16];
    f32 projection[16];
    f32 unproject_xy[4];
    f32 unproject_zw[4];
    f32 sizes[4];
    f32 sun[4];   // toward the sun in view space, 1 if orthographic
    f32 shape[4]; // distance, thickness, intensity, steps
};
static_assert(sizeof(ContactUniforms) == 208);
// prepass_velocity.vert's and mesh_velocity.vert's, a draw. DrawUniforms is its first 80 bytes, so that a pipeline with
// mesh.vert (a custom shader's) can be given the same block in the main pass.
struct VelocityUniforms {
    f32 view_projection[16]; // jittered: where the pixel is drawn
    u32 first[4];            // x: the draw's first instance
    f32 current[16];         // this frame's, without the jitter
    f32 prior[16];           // the last frame's
};
static_assert(sizeof(VelocityUniforms) == 208);
// taa.comp's.
struct TaaUniforms {
    f32 sizes[4];  // the picture's width and height, and 1 over them
    f32 params[4]; // history weight, 1 when there is no history, 1 to sharpen, sharpness
    f32 jitter[4]; // this frame's offset in pixels
};
static_assert(sizeof(TaaUniforms) == 48);
// shadow_mask.frag's and pick_mask.frag's.
struct CutoffUniforms {
    f32 cutoff[4]; // the alpha below which nothing is drawn
};
static_assert(sizeof(CutoffUniforms) == 16);
// The sky's and the fog's (kin_sky.inc.glsl's KinSky and kin_fog.inc.glsl's KinFog, vec4s all, so no padding to get wrong).
struct AtmosphereUniforms {
    f32 sun[4];    // toward the sun (unit), its light above the atmosphere
    f32 shape[4];  // Mie's optical depth at the zenith, its anisotropy, the sky's intensity, the moon's light
    f32 ground[4]; // the ground's colour
    f32 moon[4];   // toward the moon (unit)
    f32 night[4];  // the night sky's own light
};
static_assert(sizeof(AtmosphereUniforms) == 80);
struct FogParamUniforms {
    f32 colour[4];  // the fixed colour, 1 to take it from the sky
    f32 density[4]; // per unit of distance, the height fog's at its base, its fall-off, the base height
    f32 limits[4];  // the start, the greatest opacity, the sun's in-scattering and its anisotropy
    f32 view[4];    // 1 for an orthographic camera
};
static_assert(sizeof(FogParamUniforms) == 64);
// mesh_fog.frag's slot 2.
struct FogBlockUniforms {
    FogParamUniforms fog;
    AtmosphereUniforms sky;
};
static_assert(sizeof(FogBlockUniforms) == 144);
// sky.frag's slot 1 (slot 0 of sky_velocity_only.frag).
struct SkyUniforms {
    f32 inverse_view_projection[16];
    f32 view_projection[16];
    f32 prior_view_projection[16];
    f32 target[4];     // the picture's size, 1 to show the panorama, 1 when orthographic
    f32 camera[4];
    f32 forward[4];
    f32 disc[4];       // the sun's angular radius, the moon's, the sun disc's radiance over its light, the stars' brightness
    f32 moon_color[4]; // the moon disc's radiance, 1 when drawn
    AtmosphereUniforms atmosphere;
    FogParamUniforms fog;
};
static_assert(sizeof(SkyUniforms) == 416);
// sky_env.frag's.
struct SkyEnvUniforms {
    f32 target[4];
    AtmosphereUniforms atmosphere;
};
static_assert(sizeof(SkyEnvUniforms) == 96);
// How much brighter than the sky's light the moon's disc is drawn: the moon's light is a sky-scale one (0.1 at full moon).
constexpr f32 moon_disc_gain = 20.0f;
// The sky's panorama for image-based light: its size (2:1), as the environment filter wants it.
constexpr u32 sky_env_width = 256, sky_env_height = 128;


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

// Whether a sphere's shadow may fall in `f`: the sphere swept from `c` along
// `down` (the way the sun's light goes) as far as `length`, a capsule, not
// wholly outside any one plane.
bool shadow_in(const GpuFrustum& f, Vec3f c, f32 r, Vec3f down, f32 length) {
    const Vec3f e = c + down * length;
    for (u32 k = 0; k < f.count; ++k) {
        const Vec4f& p = f.planes[k];
        if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -r && p.x * e.x + p.y * e.y + p.z * e.z + p.w < -r) {
            return false;
        }
    }
    return true;
}

// How far from `c` along `down` until the box is left (0 outside it).
f32 to_leave(const Bounds3& box, Vec3f c, Vec3f down) {
    f32 t = 1e30f;
    const f32 from[3] = {c.x, c.y, c.z}, d[3] = {down.x, down.y, down.z};
    const f32 lo[3] = {box.min.x, box.min.y, box.min.z}, hi[3] = {box.max.x, box.max.y, box.max.z};
    for (int k = 0; k < 3; ++k) {
        if (d[k] > 1e-6f) {
            t = std::min(t, (hi[k] - from[k]) / d[k]);
        } else if (d[k] < -1e-6f) {
            t = std::min(t, (lo[k] - from[k]) / d[k]);
        }
    }
    return std::max(t, 0.0f);
}

// A copy's bounding sphere: its mesh's, moved and scaled with it. Returns the scale (its largest axis').
f32 sphere_of(const Mat4f& t, Vec3f center, f32 radius, Vec3f& c, f32& r) {
    c = t.apply(center);
    const f32 sx = t.m[0] * t.m[0] + t.m[1] * t.m[1] + t.m[2] * t.m[2];
    const f32 sy = t.m[4] * t.m[4] + t.m[5] * t.m[5] + t.m[6] * t.m[6];
    const f32 sz = t.m[8] * t.m[8] + t.m[9] * t.m[9] + t.m[10] * t.m[10];
    const f32 scale = std::sqrt(std::max({sx, sy, sz}));
    r = radius * scale;
    return scale;
}

// What picks a copy's level of detail (Scene3D::lod) for one scene's camera.
struct LodView {
    bool on = false;
    i32 force = -1;
    bool orthographic = false;
    f32 pixels = 1.0f;     // the error allowed on the screen
    f32 per_unit = 1.0f;   // pixels a world unit spans: at one unit's distance (perspective), or anywhere (orthographic)
    Vec3f eye{}, forward{};
};

LodView lod_view_of(const Lod3D& lod, const Camera3D& camera, f32 viewport_height) {
    LodView v;
    v.on = lod.enabled;
    v.force = lod.force_level;
    v.orthographic = camera.projection == Projection3D::Orthographic;
    v.pixels = std::max(lod.error_pixels, 0.0f) / std::max(lod.bias, 1e-3f);
    v.per_unit = v.orthographic ? viewport_height / std::max(camera.height, 1e-6f)
                                : 0.5f * viewport_height / std::tan(0.5f * camera.fov * (3.14159265358979f / 180.0f));
    v.eye = camera.position;
    v.forward = camera.forward();
    return v;
}

// The coarsest of a mesh's `levels` whose error, `scale` times the mesh's own, spans no more than the allowed
// pixels at the copy's bounding sphere (`c`, `r`): the sphere's nearest point to the camera, so a big mesh with a
// part close to it is drawn finely whole. Levels' errors rise with the level.
u32 lod_level(const LodView& v, const GpuMesh3D& mesh, u32 levels, Vec3f c, f32 r, f32 scale) {
    if (v.force >= 0) {
        return std::min<u32>(static_cast<u32>(v.force), levels - 1);
    }
    f32 distance = 1.0f;
    if (!v.orthographic) {
        const Vec3f to = c - v.eye;
        const f32 depth = dot(to, v.forward);
        // Wholly behind the camera (a shadow caster): its distance; astride its plane: as near as can be.
        distance = depth > r ? depth - r : depth < -r ? std::max(length(to) - r, 0.0f) : 0.0f;
    }
    // pixels = error * scale * per_unit / distance, at most `v.pixels`.
    const f32 reach = v.pixels * (v.orthographic ? 1.0f : distance);
    const f32 k = scale * v.per_unit;
    for (u32 l = levels - 1; l > 0; --l) {
        if (mesh.levels[l].error * k <= reach) {
            return l;
        }
    }
    return 0;
}

// A compute pipeline of an embedded shader: `samplers` read textures, one
// storage texture written, one uniform block, 8 x 8 threads.
SDL_GPUComputePipeline* make_compute(SDL_GPUDevice* device, std::string_view file, u32 samplers, u32 storage_textures = 1) {
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
    info.num_readwrite_storage_textures = storage_textures;
    info.num_uniform_buffers = 1;
    info.threadcount_x = 8;
    info.threadcount_y = 8;
    info.threadcount_z = 1;
    return SDL_CreateGPUComputePipeline(device, &info);
}

// A compute pipeline of an embedded shader that works on storage buffers only: `readonly` read-only and `readwrite`
// read-write ones, one uniform block, `threads` in a group.
SDL_GPUComputePipeline* make_buffer_compute(SDL_GPUDevice* device, std::string_view file, u32 readonly, u32 readwrite, u32 threads) {
    const std::span<const unsigned char> code = embedded_gpu_shader(file);
    if (code.empty()) {
        return nullptr;
    }
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = code.data();
    info.code_size = code.size();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_readonly_storage_buffers = readonly;
    info.num_readwrite_storage_buffers = readwrite;
    info.num_uniform_buffers = 1;
    info.threadcount_x = threads;
    info.threadcount_y = 1;
    info.threadcount_z = 1;
    return SDL_CreateGPUComputePipeline(device, &info);
}

// A compute pipeline of an embedded shader with any mix of resources (Lod3D::occlusion's).
SDL_GPUComputePipeline* make_mixed_compute(SDL_GPUDevice* device, std::string_view file, u32 samplers, u32 readonly_buffers, u32 readwrite_buffers,
                                           u32 readwrite_textures, u32 threads_x, u32 threads_y) {
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
    info.num_readwrite_storage_buffers = readwrite_buffers;
    info.num_readwrite_storage_textures = readwrite_textures;
    info.num_uniform_buffers = 1;
    info.threadcount_x = threads_x;
    info.threadcount_y = threads_y;
    info.threadcount_z = 1;
    return SDL_CreateGPUComputePipeline(device, &info);
}

void put(f32* out, Vec3f v, f32 w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}

void fill_atmosphere(AtmosphereUniforms& a, const Sky3D& sky, Vec3f sun_direction) {
    const Vec3f sun = length_squared(sun_direction) > 0.0f ? normalize(sun_direction) : Vec3f{0.0f, 1.0f, 0.0f};
    const Vec3f moon = length_squared(sky.moon_direction) > 0.0f ? normalize(sky.moon_direction) : Vec3f{0.0f, 1.0f, 0.0f};
    put(a.sun, sun, sky.sun_intensity);
    put(a.shape, {sky_model::mie_per_turbidity * std::clamp(sky.turbidity, 1.0f, 20.0f), sky_model::mie_anisotropy, sky.intensity},
        sky.moon ? std::max(sky.moon_light, 0.0f) : 0.0f);
    put(a.ground, sky.ground_color, 0.0f);
    put(a.moon, moon, 0.0f);
    put(a.night, sky.night_color, 0.0f);
}

void fill_fog(FogParamUniforms& f, const Fog3D& fog, bool orthographic) {
    put(f.colour, fog.colour, fog.colour_from_sky ? 1.0f : 0.0f);
    put(f.density, {std::max(fog.density, 0.0f), std::max(fog.height_density, 0.0f), fog.height_falloff}, fog.base_height);
    put(f.limits, {std::max(fog.start, 0.0f), std::clamp(fog.max_opacity, 0.0f, 1.0f), std::max(fog.sun_scatter, 0.0f)},
        std::clamp(fog.sun_anisotropy, -0.95f, 0.95f));
    put(f.view, {orthographic ? 1.0f : 0.0f, 0.0f, 0.0f}, 0.0f);
}

// A depth target cleared to the far plane (reverse Z: 0), whose stencil is not used.
SDL_GPUDepthStencilTargetInfo depth_target(SDL_GPUTexture* texture, SDL_GPUStoreOp store_op, bool cycle = false, u32 layer = 0) {
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = texture;
    depth.layer = static_cast<Uint8>(layer);
    depth.clear_depth = 0.0f;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = store_op;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.cycle = cycle;
    return depth;
}

void set_viewport(SDL_GPURenderPass* pass, u32 width, u32 height, f32 min_depth = 0.0f, f32 max_depth = 1.0f) {
    const SDL_GPUViewport viewport{0.0f, 0.0f, static_cast<f32>(width), static_cast<f32>(height), min_depth, max_depth};
    SDL_SetGPUViewport(pass, &viewport);
}

// One compute pass over `target`'s texels in groups of 8 x 8: `inputs` sampled, the uniforms pushed.
// False if the pass could not begin (nothing was recorded).
bool compute_pass(SDL_GPUCommandBuffer* commands, SDL_GPUComputePipeline* pipeline, const GpuTexture& target,
                  std::span<const SDL_GPUTextureSamplerBinding> inputs, const void* uniforms, u32 bytes) {
    const SDL_GPUStorageTextureReadWriteBinding output{.texture = target.handle(), .mip_level = 0, .layer = 0, .cycle = false};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, &output, 1, nullptr, 0);
    if (!pass) {
        return false;
    }
    SDL_BindGPUComputePipeline(pass, pipeline);
    SDL_BindGPUComputeSamplers(pass, 0, inputs.data(), static_cast<u32>(inputs.size()));
    SDL_PushGPUComputeUniformData(commands, 0, uniforms, bytes);
    SDL_DispatchGPUCompute(pass, (target.width() + 7) / 8, (target.height() + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
    return true;
}

// The same into two textures of one size (ao_blur_both.comp).
bool compute_pass_two(SDL_GPUCommandBuffer* commands, SDL_GPUComputePipeline* pipeline, const GpuTexture& first, const GpuTexture& second,
                      std::span<const SDL_GPUTextureSamplerBinding> inputs, const void* uniforms, u32 bytes) {
    const std::array<SDL_GPUStorageTextureReadWriteBinding, 2> outputs{
        SDL_GPUStorageTextureReadWriteBinding{.texture = first.handle(), .mip_level = 0, .layer = 0, .cycle = false},
        SDL_GPUStorageTextureReadWriteBinding{.texture = second.handle(), .mip_level = 0, .layer = 0, .cycle = false}};
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, outputs.data(), 2, nullptr, 0);
    if (!pass) {
        return false;
    }
    SDL_BindGPUComputePipeline(pass, pipeline);
    SDL_BindGPUComputeSamplers(pass, 0, inputs.data(), static_cast<u32>(inputs.size()));
    SDL_PushGPUComputeUniformData(commands, 0, uniforms, bytes);
    SDL_DispatchGPUCompute(pass, (first.width() + 7) / 8, (first.height() + 7) / 8, 1);
    SDL_EndGPUComputePass(pass);
    return true;
}

// The compute shaders' way back from a depth to a view-space position (see gtao.comp): the
// inverse projection's entries. A projection has no skew, so x and y over w follow from ndc.x
// and ndc.y alone.
struct Unprojection {
    f32 xy[4]; // a, b, c, d
    f32 zw[4]; // e, f, g, h
};

Unprojection unproject_coefficients(const Mat4f& projection) {
    const Mat4f inverse = projection.inverse();
    return {{inverse.at(0, 0), inverse.at(1, 1), inverse.at(0, 3), inverse.at(1, 3)},
            {inverse.at(2, 2), inverse.at(2, 3), inverse.at(3, 2), inverse.at(3, 3)}};
}

// Says `message` the first time only: `warned` is the caller's flag.
void warn_once(bool& warned, const char* message) {
    if (!warned) {
        warned = true;
        KIN_LOG_WARN("render", message);
    }
}

// What a pass of mesh draws has bound, so that consecutive draws bind only what changed.
class DrawBinder {
public:
    explicit DrawBinder(SDL_GPURenderPass* pass) : _pass(pass) {}

    // The pipeline and the vertex shader's storage buffers, unless the pipeline is bound already: true if bound now.
    bool bind_pipeline(SDL_GPUGraphicsPipeline* pipeline, SDL_GPUBuffer* const* storage, u32 count) {
        if (pipeline == _pipeline) {
            return false;
        }
        SDL_BindGPUGraphicsPipeline(_pass, pipeline);
        SDL_BindGPUVertexStorageBuffers(_pass, 0, storage, count);
        _pipeline = pipeline;
        return true;
    }

    // The mesh's vertices and, unless `indices` is given (a clustered mesh's compacted ones), its own indices.
    void bind_mesh(const GpuMesh3D* mesh, SDL_GPUBuffer* indices = nullptr) {
        if (mesh == _mesh && indices == _indices) {
            return;
        }
        if (mesh != _mesh) {
            const SDL_GPUBufferBinding vertices{mesh->vertices, 0};
            SDL_BindGPUVertexBuffers(_pass, 0, &vertices, 1);
        }
        const SDL_GPUBufferBinding index_binding{indices ? indices : mesh->indices, 0};
        SDL_BindGPUIndexBuffer(_pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        _mesh = mesh;
        _indices = indices;
    }

    // A skinned mesh's skin stream, the second vertex buffer (after bind_mesh; the pipeline is a skinned one).
    void bind_skin(const GpuMesh3D* mesh) {
        if (mesh == _skin_mesh) {
            return;
        }
        const SDL_GPUBufferBinding skin{mesh->skin, 0};
        SDL_BindGPUVertexBuffers(_pass, 1, &skin, 1);
        _skin_mesh = mesh;
    }

    // Forget what is bound: something else (the sky pass) bound its own pipeline, in the same render pass.
    void reset() {
        _pipeline = nullptr;
        _skin_mesh = nullptr;
        _mesh = nullptr;
        _indices = nullptr;
    }

private:
    SDL_GPURenderPass* _pass;
    SDL_GPUGraphicsPipeline* _pipeline = nullptr;
    const GpuMesh3D* _skin_mesh = nullptr;
    const GpuMesh3D* _mesh = nullptr;
    SDL_GPUBuffer* _indices = nullptr;
};

} // namespace

GpuClusterChunk::~GpuClusterChunk() {
    if (device && *device) {
        for (SDL_GPUBuffer* buffer : {indices, table}) {
            if (buffer) {
                SDL_ReleaseGPUBuffer(*device, buffer);
            }
        }
    }
}

GpuMesh3D::~GpuMesh3D() {
    if (device && *device) {
        for (SDL_GPUBuffer* buffer : {vertices, skin, chunk ? nullptr : indices}) {
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
    // Shadow maps: a depth format that can also be sampled, as an array (a
    // layer a cascade; one layer is the single map).
    for (const SDL_GPUTextureFormat format : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D16_UNORM}) {
        if (SDL_GPUTextureSupportsFormat(handle, format, SDL_GPU_TEXTURETYPE_2D_ARRAY,
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
                                                    /*uniform_buffers=*/2, /*samplers=*/10);
        _shadow_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "shadow.frag.spv");
        // The shadow vertex shaders also read a cascade's list of copies.
        _shadow_mask_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "shadow_mask.vert.spv",
                                                              /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/2);
        _shadow_mask_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "shadow_mask.frag.spv",
                                                       /*uniform_buffers=*/1, /*samplers=*/1);
        _shadow_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "shadow.vert.spv",
                                                         /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/2);
        _fullscreen_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "fullscreen.vert.spv");
        _bloom_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "bloom.frag.spv",
                                                 /*uniform_buffers=*/1, /*samplers=*/1);
        _output_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "view_output.frag.spv",
                                                  /*uniform_buffers=*/1, /*samplers=*/2);
        _pick_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "pick.frag.spv");
        _pick_mask_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "pick_mask.frag.spv",
                                                     /*uniform_buffers=*/1, /*samplers=*/1);
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: meshes not drawn", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    // Skinned meshes (Scene3D::draw_skinned): the same placing with the vertex posed by its joints first. Storage buffers: the
    // instances, [the shadow vertex shaders' list of copies,] the frame's joint matrices and each copy's first one.
    try {
        _mesh_skinned_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_skinned.vert.spv",
                                                               /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/3);
        // The velocity variants also read the copies' matrices of the frame before, and the joints of the frame before with where
        // each copy's start there.
        _mesh_skinned_velocity_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_skinned_velocity.vert.spv",
                                                                        /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/6);
        _prepass_skinned_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass_skinned.vert.spv",
                                                                  /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/3);
        _prepass_velocity_skinned_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass_velocity_skinned.vert.spv",
                                                                           /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/6);
        _shadow_skinned_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "shadow_skinned.vert.spv",
                                                                 /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/4);
        _shadow_mask_skinned_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "shadow_mask_skinned.vert.spv",
                                                                      /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/4);
        _skinning_available = true;
        _capabilities.skinning = true;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: skinned meshes not drawn", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    // Ambient occlusion and contact shadows: a pre-pass and compute shaders, needing a depth
    // that can be sampled, a two-float colour target and an R8 storage
    // texture. Without any of them it is off (said once, when asked for).
    try {
        _prepass_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass.vert.spv",
                                                          /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/1);
        _prepass_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "prepass.frag.spv");
        _prepass_mask_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "prepass_mask.frag.spv",
                                                        /*uniform_buffers=*/1, /*samplers=*/1);
        for (const SDL_GPUTextureFormat format : {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D24_UNORM}) {
            if (SDL_GPUTextureSupportsFormat(handle, format, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
                _ao_depth_format = format;
                break;
            }
        }
        if (_ao_depth_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
            throw std::runtime_error("no depth format that can be sampled");
        }
        if (!SDL_GPUTextureSupportsFormat(handle, ao_normal_format, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
            throw std::runtime_error("no two-float colour target");
        }
        if (!SDL_GPUTextureSupportsFormat(handle, ao_format, SDL_GPU_TEXTURETYPE_2D,
                                          SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
            throw std::runtime_error("no R8 storage texture");
        }
        SDL_GPUSamplerCreateInfo point{};
        point.min_filter = point.mag_filter = SDL_GPU_FILTER_NEAREST;
        point.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        point.address_mode_u = point.address_mode_v = point.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        _point_sampler = SDL_CreateGPUSampler(handle, &point);
        _gtao_pipeline = make_compute(handle, "gtao.comp.spv", 2);
        _ao_blur_pipeline = make_compute(handle, "ao_blur.comp.spv", 2);
        _ao_blur_both_pipeline = make_compute(handle, "ao_blur_both.comp.spv", 3, 2); // optional: two dispatches without
        _contact_pipeline = make_compute(handle, "contact.comp.spv", 2);
        // The occlusion over a linear depth chain; without these (or R32F storage) the first one is used.
        if (SDL_GPUTextureSupportsFormat(handle, hiz_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
            _gtao_chain_pipeline = make_compute(handle, "gtao_chain.comp.spv", 2);
            _ao_blur_chain_pipeline = make_compute(handle, "ao_blur_chain.comp.spv", 2);
            _ao_blur_both_chain_pipeline = make_compute(handle, "ao_blur_both_chain.comp.spv", 3, 2);
            _ao_chain_pipeline = make_mixed_compute(handle, "ao_depth_chain.comp.spv", 1, 0, 0, ao_chain_levels, 16, 16);
        }
        if (!_point_sampler || !_gtao_pipeline || !_ao_blur_pipeline || !_contact_pipeline) {
            throw std::runtime_error(std::string{"compute pipelines not made: "} + SDL_GetError());
        }
        _ao_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: ambient occlusion and contact shadows not available", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    // Temporal anti-aliasing: the velocity pre-pass, four-float colour targets
    // to draw it into and a float storage texture for the history, over what
    // the pre-pass already needs.
    if (_ao_available) {
        try {
            _prepass_velocity_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass_velocity.vert.spv",
                                                                       /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/2);
            _prepass_velocity_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "prepass_velocity.frag.spv");
            _prepass_velocity_mask_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "prepass_velocity_mask.frag.spv",
                                                                     /*uniform_buffers=*/1, /*samplers=*/1);
            _mesh_velocity_vertex_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_velocity.vert.spv",
                                                                    /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/2);
            _mesh_velocity_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_velocity.frag.spv",
                                                             /*uniform_buffers=*/2, /*samplers=*/10);
            if (!SDL_GPUTextureSupportsFormat(handle, hdr_format, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER) ||
                !SDL_GPUTextureSupportsFormat(handle, hdr_format, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
                throw std::runtime_error("no four-float colour target or storage texture");
            }
            _taa_pipeline = make_compute(handle, "taa.comp.spv", 4);
            if (!_taa_pipeline) {
                throw std::runtime_error(std::string{"compute pipeline not made: "} + SDL_GetError());
            }
            _taa_available = true;
        } catch (const std::exception& e) {
            KIN_LOG_INFO_F("render", "3D: temporal anti-aliasing not available", (LogFields{{.name = "reason", .value = e.what()}}));
        }
    }
    _capabilities.ambient_occlusion = _capabilities.contact_shadows = _ao_available;
    _capabilities.temporal_aa = _taa_available;
    // Fog and the sky (P3a): the fog variants of the stock mesh shaders (one more uniform block, in the slot custom parameters
    // have elsewhere), the sky pass's shaders, the panorama pass of the sky's image-based light. Each its own try: a device
    // without them draws as before (said once when a scene asks).
    try {
        _mesh_fog_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_fog.frag.spv", /*uniform_buffers=*/3, /*samplers=*/10);
        if (_mesh_velocity_shader) {
            _mesh_velocity_fog_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_velocity_fog.frag.spv",
                                                                 /*uniform_buffers=*/3, /*samplers=*/10);
        }
        _fog_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: fog not available", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    try {
        _sky_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "sky.frag.spv", /*uniform_buffers=*/2, /*samplers=*/1);
        if (_mesh_velocity_shader) {
            _sky_velocity_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "sky_velocity.frag.spv", /*uniform_buffers=*/2,
                                                            /*samplers=*/1);
            _sky_velocity_only_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "sky_velocity_only.frag.spv",
                                                                 /*uniform_buffers=*/1, /*samplers=*/0);
        }
        _sky_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: sky not available", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    try {
        _output_dither_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "view_output_dither.frag.spv", /*uniform_buffers=*/1,
                                                         /*samplers=*/2);
        _sky_env_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "sky_env.frag.spv", /*uniform_buffers=*/1, /*samplers=*/0);
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: image-based light from the sky not available", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    // The cluster cut in compute (Lod3D::gpu_culling): four compute pipelines over storage buffers, and buffers that are
    // storage and index or indirect arguments at once. KIN_GPU_CLUSTERS=0 turns it off (to compare, and to test the fallback).
    {
        const char* off = std::getenv("KIN_GPU_CLUSTERS");
        const bool allowed = !(off && off[0] == '0');
        if (allowed) {
            _gc_cull = make_buffer_compute(handle, "cluster_cull.comp.spv", 3, 3, 64);
            _gc_scan = make_buffer_compute(handle, "cluster_scan.comp.spv", 0, 2, 256);
            _gc_args = make_buffer_compute(handle, "cluster_args.comp.spv", 2, 1, 64);
            _gc_gather = make_buffer_compute(handle, "cluster_gather.comp.spv", 5, 2, 64);
            bool usable = _gc_cull && _gc_scan && _gc_args && _gc_gather;
            for (const SDL_GPUBufferUsageFlags usage :
                 {SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                  SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                  SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                  SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ}) {
                const SDL_GPUBufferCreateInfo probe{.usage = usage, .size = 256, .props = 0};
                if (SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(handle, &probe)) {
                    SDL_ReleaseGPUBuffer(handle, buffer);
                } else {
                    usable = false;
                }
            }
            _gpu_clusters = usable;
        }
        _capabilities.gpu_driven_clusters = _gpu_clusters;
        // Occlusion culling (Lod3D::occlusion): the cut's second build, and the depth pyramid's two compute shaders over
        // R32F storage textures; it also uses the pre-pass's sampled depth and the point sampler.
        if (_gpu_clusters && _ao_available &&
            SDL_GPUTextureSupportsFormat(handle, hiz_format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
            _gc_cull_occ = make_mixed_compute(handle, "cluster_cull_occ.comp.spv", 12, 5, 4, 0, 64, 1);
            _hiz_depth_pipeline = make_mixed_compute(handle, "hiz_depth.comp.spv", 1, 0, 0, 1, 8, 8);
            _hiz_reduce_pipeline = make_mixed_compute(handle, "hiz_reduce.comp.spv", 1, 0, 0, 4, 16, 16);
            try {
                if (_gc_cull_occ && _hiz_depth_pipeline && _hiz_reduce_pipeline) {
                    for (GpuTexture& spare : _hiz_spare) {
                        spare = device.create_target(1, 1, hiz_format, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
                    }
                    _occlusion_available = true;
                }
            } catch (const std::exception&) {
            }
        }
        _capabilities.occlusion_culling = _occlusion_available;
        if (!_gpu_clusters) {
            KIN_LOG_INFO("render", allowed ? "3D: cluster cut in compute not available on this device: the CPU cuts" : "3D: cluster cut in compute turned off (KIN_GPU_CLUSTERS=0): the CPU cuts");
        }
    }
    {
        // Image-based light: compute filters an environment into mips; sampled
        // by direction, so u wraps and v stops at the poles.
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter = SDL_GPU_FILTER_LINEAR;
        info.mag_filter = SDL_GPU_FILTER_LINEAR;
        info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        info.max_lod = 1000.0f;
        info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        _environment_sampler = SDL_CreateGPUSampler(handle, &info);
        const std::span<const unsigned char> code = embedded_gpu_shader("env_filter.comp.spv");
        if (!code.empty() && SDL_GPUTextureSupportsFormat(handle, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                                           SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
            SDL_GPUComputePipelineCreateInfo pipeline{};
            pipeline.code = code.data();
            pipeline.code_size = code.size();
            pipeline.entrypoint = "main";
            pipeline.format = SDL_GPU_SHADERFORMAT_SPIRV;
            pipeline.num_samplers = 1;
            pipeline.num_readwrite_storage_textures = 1;
            pipeline.num_uniform_buffers = 1;
            pipeline.threadcount_x = 8;
            pipeline.threadcount_y = 8;
            pipeline.threadcount_z = 1;
            _filter_pipeline = SDL_CreateGPUComputePipeline(handle, &pipeline);
        }
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
            _no_shadow = device.create_target_array(1, 1, 1, _shadow_format,
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
    if (_environment_sampler) {
        SDL_ReleaseGPUSampler(_device->handle(), _environment_sampler);
    }
    for (SDL_GPUComputePipeline* compute : {_filter_pipeline, _ao_blur_chain_pipeline, _ao_blur_both_chain_pipeline, _gtao_pipeline, _gtao_chain_pipeline, _ao_chain_pipeline, _ao_blur_pipeline, _ao_blur_both_pipeline, _contact_pipeline, _taa_pipeline, _gc_cull, _gc_scan, _gc_args, _gc_gather, _gc_cull_occ, _hiz_depth_pipeline, _hiz_reduce_pipeline}) {
        if (compute) {
            SDL_ReleaseGPUComputePipeline(_device->handle(), compute);
        }
    }
    if (_point_sampler) {
        SDL_ReleaseGPUSampler(_device->handle(), _point_sampler);
    }
    for (SDL_GPUSampler* sampler : _samplers) {
        if (sampler) {
            SDL_ReleaseGPUSampler(_device->handle(), sampler);
        }
    }
}

SDL_GPUSampler* GpuRenderer3D::sampler_for(const GpuTextureBackend& texture, Wrap3D u, Wrap3D v) {
    if (u == Wrap3D::Clamp && v == Wrap3D::Clamp) {
        return _owner->sampler_for(texture); // as 2D draws it
    }
    const ScaleMode scale = texture.scale_mode() == ScaleMode::Mipmapped && !texture.mipmapped() ? ScaleMode::Linear : texture.scale_mode();
    const std::size_t index = (static_cast<std::size_t>(scale) * 3 + static_cast<std::size_t>(u)) * 3 + static_cast<std::size_t>(v);
    if (index < _samplers.size() && _samplers[index]) {
        return _samplers[index];
    }
    const auto mode = [](Wrap3D wrap) {
        return wrap == Wrap3D::Repeat ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
             : wrap == Wrap3D::Mirror ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT
                                      : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    };
    SDL_GPUSamplerCreateInfo info{};
    info.address_mode_u = mode(u);
    info.address_mode_v = mode(v);
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    const bool nearest = scale == ScaleMode::Nearest;
    info.min_filter = info.mag_filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    info.mipmap_mode = scale == ScaleMode::Mipmapped ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.max_lod = scale == ScaleMode::Mipmapped ? 1000.0f : 0.0f;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(_device->handle(), &info);
    if (index < _samplers.size()) {
        _samplers[index] = sampler;
    }
    return sampler ? sampler : _owner->sampler_for(texture);
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

SDL_GPUGraphicsPipeline* GpuRenderer3D::pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull, Pass pass, bool mask, SDL_GPUShader* custom,
                                                 bool velocity, bool skinned, bool fog) const {
    if (skinned && !_skinning_available) {
        return nullptr;
    }
    const GpuVertexLayout layout = skinned ? GpuVertexLayout::SkinnedMeshVertices : GpuVertexLayout::MeshVertices;
    if (pass == Pass::Shadow) {
        // A closed mesh's back faces are behind its front ones from the sun
        // too: culled, the map is the same for half the triangles. Double-sided
        // (thin, open) meshes cast with both. shadow.vert places the vertices
        // and does nothing else; a cut-out's also carry its UV and alpha to the
        // fragment shader that discards.
        SDL_GPUShader* vertex = skinned ? (mask ? _shadow_mask_skinned_vertex_shader : _shadow_skinned_vertex_shader).handle()
                                        : (mask ? _shadow_mask_vertex_shader : _shadow_vertex_shader).handle();
        return _owner->_pipelines.get(GpuPipelineKey{.vertex = vertex,
                                                     .fragment = (mask ? _shadow_mask_shader : _shadow_shader).handle(),
                                                     .blend = GpuBlendMode::Replace,
                                                     .layout = layout,
                                                     .depth_stencil = _shadow_format,
                                                     .depth = GpuDepthTest::TestWrite,
                                                     .cull = cull ? GpuCull::Back : GpuCull::None,
                                                     .depth_only = true});
    }
    if (pass == Pass::Occluder) {
        // Lod3D::occlusion's depth of the frame's opaque meshes, when no pre-pass draws one: the shadow pass's shaders (a
        // cut-out's discard included) into the single-sample depth the pyramid is built from.
        return _owner->_pipelines.get(GpuPipelineKey{.vertex = skinned ? (mask ? _shadow_mask_skinned_vertex_shader : _shadow_skinned_vertex_shader).handle()
                                                                       : (mask ? _shadow_mask_vertex_shader : _shadow_vertex_shader).handle(),
                                                     .fragment = (mask ? _shadow_mask_shader : _shadow_shader).handle(),
                                                     .blend = GpuBlendMode::Replace,
                                                     .layout = layout,
                                                     .depth_stencil = _ao_depth_format,
                                                     .depth = GpuDepthTest::TestWrite,
                                                     .cull = cull ? GpuCull::Back : GpuCull::None,
                                                     .depth_only = true});
    }
    if (pass == Pass::Prepass) {
        // Depth and the world normal, one sample, in the occlusion's own targets.
        return _owner->_pipelines.get(GpuPipelineKey{.vertex = (skinned ? _prepass_skinned_vertex_shader : _prepass_vertex_shader).handle(),
                                                     .fragment = (mask ? _prepass_mask_shader : _prepass_shader).handle(),
                                                     .blend = GpuBlendMode::Replace,
                                                     .format = ao_normal_format,
                                                     .layout = layout,
                                                     .depth_stencil = _ao_depth_format,
                                                     .depth = GpuDepthTest::TestWrite,
                                                     .cull = cull ? GpuCull::Back : GpuCull::None});
    }
    if (pass == Pass::Pick) {
        // The pick fragment shaders read vWorld and vId, which mesh.vert and mesh_skinned.vert both write.
        return _owner->_pipelines.get(GpuPipelineKey{.vertex = (skinned ? _mesh_skinned_vertex_shader : _vertex_shader).handle(),
                                                     .fragment = (mask ? _pick_mask_shader : _pick_shader).handle(),
                                                     .blend = GpuBlendMode::Replace,
                                                     .format = pick_format,
                                                     .layout = layout,
                                                     .depth_stencil = _depth_format,
                                                     .depth = GpuDepthTest::TestWrite,
                                                     .cull = cull ? GpuCull::Back : GpuCull::None});
    }
    // Opaque: written over what is there, alpha and all; transparent: blended,
    // tested against the depth but not writing it; wireframes over the faces.
    const GpuBlendMode blend = pass == Pass::Opaque || pass == Pass::Wire ? GpuBlendMode::Replace
                             : pass == Pass::Alpha                       ? GpuBlendMode::Premultiplied
                                                                         : GpuBlendMode::PremultipliedAdditive;
    // With the velocity target, the stock shaders write it (an opaque mesh) or the pipeline masks it; a custom shader
    // writes nothing there, so its draws are masked too (a frame with an opaque custom shader has its pre-pass instead).
    SDL_GPUShader* vertex = skinned ? (velocity && !custom ? _mesh_skinned_velocity_vertex_shader : _mesh_skinned_vertex_shader).handle()
                                    : velocity && !custom ? _mesh_velocity_vertex_shader.handle() : _vertex_shader.handle();
    // The fog's variants (P3a): only for a stock shader, and only where they were made.
    const bool fogged = fog && !custom && _fog_available && (!velocity || _mesh_velocity_fog_shader);
    SDL_GPUShader* fragment = custom ? custom
                            : velocity ? (fogged ? _mesh_velocity_fog_shader : _mesh_velocity_shader).handle()
                                       : (fogged ? _mesh_fog_shader : _fragment_shader).handle();
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = vertex,
                                                 .fragment = fragment,
                                                 .blend = blend,
                                                 .format = format,
                                                 .layout = layout,
                                                 .depth_stencil = _depth_format,
                                                 .depth = pass == Pass::Opaque ? GpuDepthTest::TestWrite : GpuDepthTest::Test,
                                                 .cull = cull && pass != Pass::Wire ? GpuCull::Back : GpuCull::None,
                                                 .samples = sample_count(samples),
                                                 .wireframe = pass == Pass::Wire,
                                                 .depth_bias = pass == Pass::Wire,
                                                 .extra_format = velocity ? hdr_format : SDL_GPU_TEXTUREFORMAT_INVALID,
                                                 .extra_write = velocity && pass == Pass::Opaque && !custom});
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::sky_pipeline(SDL_GPUTextureFormat format, u32 samples, bool velocity) const {
    if (!_sky_available || (velocity && !_sky_velocity_shader)) {
        return nullptr;
    }
    // Reverse Z: the sky is drawn at depth 0 and passes where the depth buffer still holds the far plane (cleared to 0) and
    // nowhere something was drawn (greater than 0). Replaces: it is opaque.
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = _fullscreen_shader.handle(),
                                                 .fragment = (velocity ? _sky_velocity_shader : _sky_shader).handle(),
                                                 .blend = GpuBlendMode::Replace,
                                                 .format = format,
                                                 .layout = GpuVertexLayout::None,
                                                 .depth_stencil = _depth_format,
                                                 .depth = GpuDepthTest::Test,
                                                 .samples = sample_count(samples),
                                                 .extra_format = velocity ? hdr_format : SDL_GPU_TEXTUREFORMAT_INVALID,
                                                 .extra_write = velocity});
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::sky_velocity_pipeline(SDL_GPUTextureFormat depth) const {
    if (!_sky_velocity_only_shader) {
        return nullptr;
    }
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = _fullscreen_shader.handle(),
                                                 .fragment = _sky_velocity_only_shader.handle(),
                                                 .blend = GpuBlendMode::Replace,
                                                 .format = hdr_format,
                                                 .layout = GpuVertexLayout::None,
                                                 .depth_stencil = depth,
                                                 .depth = GpuDepthTest::Test});
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::velocity_pipeline(SDL_GPUTextureFormat depth, bool cull, bool mask, bool skinned) const {
    if (skinned && !_skinning_available) {
        return nullptr;
    }
    // The pre-pass with velocity: one sample, the normal and the velocity in four floats.
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = (skinned ? _prepass_velocity_skinned_vertex_shader : _prepass_velocity_vertex_shader).handle(),
                                                 .fragment = (mask ? _prepass_velocity_mask_shader : _prepass_velocity_shader).handle(),
                                                 .blend = GpuBlendMode::Replace,
                                                 .format = hdr_format,
                                                 .layout = skinned ? GpuVertexLayout::SkinnedMeshVertices : GpuVertexLayout::MeshVertices,
                                                 .depth_stencil = depth,
                                                 .depth = GpuDepthTest::TestWrite,
                                                 .cull = cull ? GpuCull::Back : GpuCull::None});
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::fullscreen(SDL_GPUShader* fragment, SDL_GPUTextureFormat format) const {
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = _fullscreen_shader.handle(),
                                                 .fragment = fragment,
                                                 .blend = GpuBlendMode::Replace,
                                                 .format = format,
                                                 .layout = GpuVertexLayout::None});
}

std::shared_ptr<GpuClusterChunk> GpuRenderer3D::allocate_cluster_range(u32 indices, u32 clusters, u32& index_first, u32& table_first) {
    // Chunks no mesh uses go first.
    std::erase_if(_chunks, [](const std::shared_ptr<GpuClusterChunk>& chunk) { return chunk.use_count() == 1; });
    for (const std::shared_ptr<GpuClusterChunk>& chunk : _chunks) {
        if (static_cast<u64>(chunk->index_used) + indices <= chunk->index_capacity && static_cast<u64>(chunk->table_used) + clusters <= chunk->table_capacity) {
            index_first = chunk->index_used;
            table_first = chunk->table_used;
            chunk->index_used += indices;
            chunk->table_used += clusters;
            return chunk;
        }
    }
    // A new chunk: 16 MB of indices and 16 thousand clusters, or what the mesh needs.
    constexpr u32 default_indices = 4u * 1024u * 1024u, default_clusters = 16384;
    auto chunk = std::make_shared<GpuClusterChunk>(_device->shared());
    chunk->index_capacity = std::max(default_indices, indices);
    chunk->table_capacity = std::max(default_clusters, clusters);
    chunk->id = _next_chunk_id++;
    const SDL_GPUBufferCreateInfo index_info{.usage = SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                             .size = static_cast<u32>(static_cast<u64>(chunk->index_capacity) * sizeof(u32)), .props = 0};
    const SDL_GPUBufferCreateInfo table_info{.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                             .size = static_cast<u32>(static_cast<u64>(chunk->table_capacity) * sizeof(GpuCluster)), .props = 0};
    chunk->indices = SDL_CreateGPUBuffer(_device->handle(), &index_info);
    chunk->table = SDL_CreateGPUBuffer(_device->handle(), &table_info);
    if (!chunk->indices || !chunk->table) {
        throw std::runtime_error(std::string{"SDL_CreateGPUBuffer failed: "} + SDL_GetError());
    }
    _device->poison_buffer(chunk->indices, index_info.size); // KIN_POISON_BUFFERS: a debug aid
    _device->poison_buffer(chunk->table, table_info.size);
    index_first = 0;
    table_first = 0;
    chunk->index_used = indices;
    chunk->table_used = clusters;
    _chunks.push_back(chunk);
    return chunk;
}

std::shared_ptr<IMeshBackend> GpuRenderer3D::create_mesh(const MeshData& source, const MeshLodChain* chain, std::shared_ptr<const MeshClusters> clusters) {
    // With clusters the mesh is the hierarchy's welded vertices and its leaves' triangles (full detail, for the draws
    // that do not cut); the hierarchy itself stays on the CPU, for the cut.
    std::vector<u32> leaf_indices;
    if (clusters) {
        leaf_indices = clusters->leaf_indices();
        chain = nullptr;
    }
    struct Source {
        const std::vector<MeshVertex>& vertices;
        const std::vector<u32>& indices;
    };
    const Source data{clusters ? clusters->vertices : source.vertices, clusters ? leaf_indices : source.indices};
    const std::size_t vertex_count = data.vertices.size();
    const std::size_t level0_count = data.indices.empty() ? vertex_count : data.indices.size();
    // The levels' indices follow level 0's in the one index buffer. A mesh the GPU cuts (compute reads every cluster's
    // triangles from the index buffer) has those of the clusters above the leaves after the leaves' own, in cluster order.
    const bool gpu_table = clusters && _capabilities.gpu_driven_clusters;
    std::vector<u64> pool_first; // the first index of cluster leaf_count + i
    std::size_t index_count = level0_count;
    if (gpu_table) {
        pool_first.reserve(clusters->clusters.size() - clusters->leaf_count + 1);
        for (u32 c = clusters->leaf_count; c < clusters->clusters.size(); ++c) {
            pool_first.push_back(index_count);
            index_count += static_cast<std::size_t>(clusters->clusters[c].triangle_count) * 3;
        }
        pool_first.push_back(index_count);
    }
    if (chain) {
        for (std::size_t l = 1; l < chain->levels.size(); ++l) {
            index_count += chain->levels[l].indices.size();
        }
    }
    const std::size_t extra_vertices = chain ? chain->extra_vertices.size() : 0;
    if (vertex_count + extra_vertices > std::numeric_limits<u32>::max() / sizeof(GpuMeshVertex)) {
        KIN_LOG_WARN("render", "3D: mesh not made: its levels are too big for one buffer");
        return {};
    }
    if (index_count > std::numeric_limits<u32>::max() / sizeof(u32)) {
        KIN_LOG_WARN("render", "3D: mesh not made: its levels are too big for one buffer");
        return {};
    }
    try {
        auto mesh = std::make_shared<GpuMesh3D>(_device->shared());
        mesh->serial = _next_mesh_serial++;
        u32 index_first = 0, table_first = 0; // the mesh's ranges in its chunk, with the cut in compute
        if (gpu_table) {
            mesh->chunk = allocate_cluster_range(static_cast<u32>(index_count), static_cast<u32>(clusters->clusters.size()), index_first, table_first);
            mesh->table_first = table_first;
        }
        const u32 vertex_bytes = static_cast<u32>((vertex_count + extra_vertices) * sizeof(GpuMeshVertex));
        const u32 index_bytes = static_cast<u32>(index_count * sizeof(u32));
        const SDL_GPUBufferCreateInfo vertex_info{.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = vertex_bytes, .props = 0};
        const SDL_GPUBufferCreateInfo index_info{.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = index_bytes, .props = 0};
        mesh->vertices = SDL_CreateGPUBuffer(_device->handle(), &vertex_info);
        mesh->indices = mesh->chunk ? mesh->chunk->indices : SDL_CreateGPUBuffer(_device->handle(), &index_info);
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
        const auto upload = [&](SDL_GPUBuffer* buffer, std::size_t count, std::size_t size, const auto& write, std::size_t base_bytes = 0) {
            const std::size_t per_piece = std::max<std::size_t>(1, piece_bytes / size);
            for (std::size_t first = 0; first < count; first += per_piece) {
                const std::size_t n = std::min(per_piece, count - first);
                const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
                    in_slices(n, [&](std::size_t begin, std::size_t end) { write(out.data(), first, begin, end); });
                };
                _device->upload_storage_buffer(buffer, static_cast<u32>(base_bytes + first * size), static_cast<u32>(n * size), nullptr, false, &fill);
            }
        };
        upload(mesh->vertices, vertex_count + extra_vertices, sizeof(GpuMeshVertex), [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
            auto* to = reinterpret_cast<GpuMeshVertex*>(out);
            for (std::size_t i = begin; i < end; ++i) {
                const std::size_t g = first + i; // the mesh's vertices, then the levels' own
                const MeshVertex& v = g < vertex_count ? data.vertices[g] : chain->extra_vertices[g - vertex_count];
                to[i] = {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y,
                         v.color.r, v.color.g, v.color.b, v.color.a};
            }
        });
        if (!clusters && !chain && source.skinned() && source.skin.size() == vertex_count) {
            // The skin stream: a second vertex buffer, 8 bytes a vertex. A joint whose weight came to nothing points at joint 0
            // (always there), so that the shader never reads a matrix that is not.
            const SDL_GPUBufferCreateInfo skin_info{.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = static_cast<u32>(vertex_count * sizeof(GpuSkinVertex)), .props = 0};
            mesh->skin = SDL_CreateGPUBuffer(_device->handle(), &skin_info);
            if (!mesh->skin) {
                throw std::runtime_error(std::string{"SDL_CreateGPUBuffer failed: "} + SDL_GetError());
            }
            upload(mesh->skin, vertex_count, sizeof(GpuSkinVertex), [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
                auto* to = reinterpret_cast<GpuSkinVertex*>(out);
                for (std::size_t i = begin; i < end; ++i) {
                    const MeshSkinVertex& k = source.skin[first + i];
                    const std::array<u8, 4> w = quantise_skin_weights(k.weights);
                    to[i] = {w[0] ? k.joints[0] : u8{0}, w[1] ? k.joints[1] : u8{0}, w[2] ? k.joints[2] : u8{0}, w[3] ? k.joints[3] : u8{0},
                             w[0], w[1], w[2], w[3]};
                }
            });
            mesh->skin_bounds = compute_skin_bounds(source, source.skin_joint_count());
        }
        upload(mesh->indices, index_count, sizeof(u32), [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
            auto* to = reinterpret_cast<u32*>(out);
            // The slice [first + begin, first + end) of the buffer: level 0's indices, then each level's.
            const std::size_t lo = first + begin, hi = first + end;
            if (lo < level0_count) {
                const std::size_t to_end = std::min(hi, level0_count);
                if (data.indices.empty()) {
                    for (std::size_t g = lo; g < to_end; ++g) {
                        to[g - first] = static_cast<u32>(g); // in order: three vertices a triangle
                    }
                } else {
                    std::memcpy(to + (lo - first), data.indices.data() + lo, (to_end - lo) * sizeof(u32));
                }
            }
            std::size_t level_start = level0_count;
            for (std::size_t l = 1; chain && l < chain->levels.size(); ++l) {
                const std::vector<u32>& indices = chain->levels[l].indices;
                const std::size_t from = std::max(lo, level_start), to_end = std::min(hi, level_start + indices.size());
                if (from < to_end) {
                    std::memcpy(to + (from - first), indices.data() + (from - level_start), (to_end - from) * sizeof(u32));
                }
                level_start += indices.size();
            }
            // The clusters above the leaves, each from its first index.
            if (gpu_table && hi > level0_count) {
                std::size_t at = std::max(lo, level0_count);
                std::size_t i = static_cast<std::size_t>(std::upper_bound(pool_first.begin(), pool_first.end(), static_cast<u64>(at)) - pool_first.begin()) - 1;
                std::array<u32, max_cluster_triangles * 3> own;
                while (at < hi && i + 1 < pool_first.size()) {
                    const u32 c = clusters->leaf_count + static_cast<u32>(i);
                    clusters->append_indices(c, own.data());
                    const std::size_t begin = pool_first[i], end = pool_first[i + 1];
                    const std::size_t stop = std::min<std::size_t>(end, hi);
                    std::memcpy(to + (at - first), own.data() + (at - begin), (stop - at) * sizeof(u32));
                    at = stop;
                    ++i;
                }
            }
        }, static_cast<std::size_t>(index_first) * sizeof(u32));
        if (gpu_table) {
            // The clusters' table, for the cut in compute.
            std::vector<u32> first_index(clusters->clusters.size());
            std::size_t running = index_first;
            for (std::size_t c = 0; c < first_index.size(); ++c) {
                first_index[c] = static_cast<u32>(running);
                running += static_cast<std::size_t>(clusters->clusters[c].triangle_count) * 3;
            }
            upload(mesh->chunk->table, clusters->clusters.size(), sizeof(GpuCluster), [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
                auto* to = reinterpret_cast<GpuCluster*>(out);
                for (std::size_t i = begin; i < end; ++i) {
                    const std::size_t c = first + i;
                    const MeshCluster& k = clusters->clusters[c];
                    to[i] = {{k.center.x, k.center.y, k.center.z}, k.radius,
                             {k.parent_center.x, k.parent_center.y, k.parent_center.z}, k.parent_radius,
                             {k.cull_center.x, k.cull_center.y, k.cull_center.z}, k.cull_radius,
                             k.error, k.parent_error, 0.0f, 0.0f,
                             first_index[c], k.triangle_count, k.depth, k.parent_depth};
                }
            }, static_cast<std::size_t>(table_first) * sizeof(GpuCluster));
        }
        mesh->clusters = std::move(clusters);
        mesh->index_count = static_cast<u32>(level0_count);
        mesh->levels.push_back({index_first, static_cast<u32>(level0_count), 0.0f});
        std::size_t level_start = level0_count;
        for (std::size_t l = 1; chain && l < chain->levels.size(); ++l) {
            mesh->levels.push_back({static_cast<u32>(index_first + level_start), static_cast<u32>(chain->levels[l].indices.size()), chain->levels[l].error});
            level_start += chain->levels[l].indices.size();
        }
        return mesh;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: mesh not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return {};
    }
}

namespace {

// An IEEE half of `x` (round to nearest, clamped to what a half holds).
u16 to_half(f32 x) {
    u32 bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const u32 sign = (bits >> 16) & 0x8000u;
    if (!(x == x)) {
        return static_cast<u16>(sign | 0x7e00u);
    }
    x = std::clamp(std::abs(x), 0.0f, 65504.0f);
    std::memcpy(&bits, &x, sizeof(bits));
    const i32 exponent = static_cast<i32>((bits >> 23) & 0xffu) - 127 + 15;
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<u16>(sign);
        }
        const u32 mantissa = (bits & 0x7fffffu) | 0x800000u;
        const u32 shift = static_cast<u32>(14 - exponent);
        return static_cast<u16>(sign | ((mantissa + (1u << (shift - 1))) >> shift));
    }
    const u32 rounded = (bits & 0x7fffffu) + 0x1000u;
    return static_cast<u16>(sign | (static_cast<u32>(exponent) << 10) + (rounded >> 13));
}

constexpr u32 environment_samples = 256;
constexpr u32 irradiance_samples = 512;

} // namespace

std::shared_ptr<IMaterialShaderBackend> GpuRenderer3D::create_material_shader(std::span<const u8> spirv) {
    try {
        auto shader = std::make_shared<GpuMaterialShader3D>();
        // As mesh.frag: the frame, the material and the custom parameters;
        // the material's nine textures (kin_material.inc.glsl declares them all).
        shader->shader = GpuShader::from_bytes(*_device, SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERFORMAT_SPIRV, spirv,
                                               /*uniform_buffers=*/3, /*samplers=*/10);
        _material_shaders.push_back(shader); // kept: pipelines name it for good
        return shader;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: material shader not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return {};
    }
}

std::shared_ptr<GpuEnvironment3D> GpuRenderer3D::allocate_environment(u32 w, u32 h) {
    constexpr SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    auto environment = std::make_shared<GpuEnvironment3D>();
    const auto make = [&](u32 w, u32 h, u32 levels, SDL_GPUTextureUsageFlags usage) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = format;
        info.usage = usage;
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = 1;
        info.num_levels = levels;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(_device->handle(), &info);
        if (!texture) {
            throw std::runtime_error(std::string{"SDL_CreateGPUTexture failed: "} + SDL_GetError());
        }
        _device->poison_texture(texture, format, w, h, 1, levels, usage); // KIN_POISON_TEXTURES: a debug aid, nothing when unset
        return GpuTexture{_device->shared(), texture, w, h, format};
    };
    // The levels the GPU can fill: by the shorter side, not the longer. A 2:1 picture's 1 x 1 level was
    // never written (SDL's blit chain stops at an empty extent), and the filter's widest lobes read it:
    // whatever its memory held went into the specular levels (stale values, now and then a NaN: a green sky).
    const u32 source_levels = GpuDevice::complete_mip_levels(w, h);
    constexpr SDL_GPUTextureUsageFlags source_usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    environment->source = make(w, h, source_levels, source_usage);
    // The mirror level as wide as the picture, up to 1024; a level more
    // rough each, to the roughest at a sixteenth of that.
    const u32 sw = std::clamp(w, 64u, 1024u);
    u32 levels = 1;
    while (levels < 7 && (sw >> levels) >= 16) {
        ++levels;
    }
    environment->levels = levels;
    environment->specular = make(sw, sw / 2, levels, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
    environment->irradiance = make(64, 32, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
    return environment;
}

std::shared_ptr<IEnvironmentBackend> GpuRenderer3D::create_environment(Vec2i size, std::span<const f32> rgb) {
    constexpr SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    if (!_filter_pipeline || !_environment_sampler) {
        return {};
    }
    try {
        const u32 w = static_cast<u32>(size.x), h = static_cast<u32>(size.y);
        std::shared_ptr<GpuEnvironment3D> environment = allocate_environment(w, h);
        std::vector<u16> texels(static_cast<std::size_t>(w) * h * 4);
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
            for (int c = 0; c < 3; ++c) {
                texels[i * 4 + static_cast<std::size_t>(c)] = to_half(rgb[i * 3 + static_cast<std::size_t>(c)]);
            }
            texels[i * 4 + 3] = to_half(1.0f);
        }
        _device->update_texture(environment->source.handle(), 0, 0, w, h, reinterpret_cast<const u8*>(texels.data()), 8, true);
        // KIN_POISON_TEXTURES: the levels to be generated, filled with the poison after level 0's (cycling) upload.
        _device->poison_texture(environment->source.handle(), format, w, h, 1, GpuDevice::complete_mip_levels(w, h),
                                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, SDL_GPU_SAMPLECOUNT_1, /*first_level=*/1);
        _device->generate_mipmaps(environment->source.handle());
        return environment;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: environment not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return {};
    }
}

void GpuRenderer3D::filter_environment(SDL_GPUCommandBuffer* commands, GpuEnvironment3D& environment) {
    // One compute pass a level: the picture's own, then each rougher, then the diffuse.
    const auto filter = [&](const GpuTexture& target, u32 level, u32 mode, f32 roughness, u32 samples) {
        const SDL_GPUStorageTextureReadWriteBinding output{.texture = target.handle(), .mip_level = level};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, &output, 1, nullptr, 0);
        if (!pass) {
            return;
        }
        SDL_BindGPUComputePipeline(pass, _filter_pipeline);
        const SDL_GPUTextureSamplerBinding source{environment.source.handle(), _environment_sampler};
        SDL_BindGPUComputeSamplers(pass, 0, &source, 1);
        const u32 w = std::max(1u, target.width() >> level), h = std::max(1u, target.height() >> level);
        const f32 params[8] = {static_cast<f32>(w), static_cast<f32>(h), roughness, static_cast<f32>(mode),
                               static_cast<f32>(environment.source.width()), static_cast<f32>(environment.source.height()),
                               static_cast<f32>(samples), 0.0f};
        SDL_PushGPUComputeUniformData(commands, 0, params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (w + 7) / 8, (h + 7) / 8, 1);
        SDL_EndGPUComputePass(pass);
    };
    for (u32 level = 0; level < environment.levels; ++level) {
        const f32 roughness = environment.levels > 1 ? static_cast<f32>(level) / static_cast<f32>(environment.levels - 1) : 0.0f;
        filter(environment.specular, level, level == 0 ? 0u : 1u, roughness, environment_samples);
    }
    filter(environment.irradiance, 0, 2, 1.0f, irradiance_samples);
    environment.filtered = true;
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
        release_taa(view); // sized for the old targets
        view._hiz = {};    // Lod3D::occlusion's pyramid and depth are sized for them too, and the history of clusters is of no use
        view._hiz_levels = 0;
        view._occ_depth = {};
        view._occ_reset = true;
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

u32 GpuRenderer3D::ensure_shadow_map(u32 resolution, u32 layers) {
    if (_shadow_format == SDL_GPU_TEXTUREFORMAT_INVALID || !_shadow_shader || !_shadow_vertex_shader) {
        return 0;
    }
    resolution = std::clamp(resolution, 64u, 8192u);
    layers = std::clamp(layers, 1u, max_shadow_cascades);
    // A map with enough layers serves fewer cascades too (the others are not drawn).
    if (_shadow_map && _shadow_size == resolution && _shadow_layers >= std::min(layers, _shadow_layers_limit)) {
        return std::min(layers, _shadow_layers);
    }
    // Asked for more layers than a device made before: not tried again.
    for (u32 attempt = std::min(layers, _shadow_layers_limit); attempt >= 1; --attempt) {
        try {
            _shadow_map = _device->create_target_array(resolution, resolution, attempt, _shadow_format,
                                                       SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            _shadow_size = resolution;
            _shadow_layers = attempt;
            ++_shadow_generation; // a new map: nothing cached is in it
            if (attempt < layers) {
                _shadow_layers_limit = attempt;
                KIN_LOG_WARN_F("render", "3D: fewer shadow cascades than asked",
                               (LogFields{{.name = "asked", .value = std::to_string(layers)},
                                          {.name = "made", .value = std::to_string(attempt)}}));
            }
            return attempt;
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: no shadow map", (LogFields{{.name = "layers", .value = std::to_string(attempt)},
                                                                      {.name = "reason", .value = e.what()}}));
        }
    }
    _shadow_map = {};
    _shadow_size = 0;
    _shadow_layers = 0;
    ++_shadow_generation;
    return 0;
}

bool GpuRenderer3D::ensure_ao_targets(GpuView3D& view, bool half, bool want_ao, bool want_contact, bool velocity) {
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    const SDL_GPUTextureFormat normals_format = velocity ? hdr_format : ao_normal_format;
    const u32 aw = half ? std::max(1u, (w + 1) / 2) : w, ah = half ? std::max(1u, (h + 1) / 2) : h;
    const auto sized = [](const GpuTexture& t, u32 tw, u32 th) { return t && t.width() == tw && t.height() == th; };
    const bool have_base = sized(view._ao_depth, w, h) && view._ao_normals && view._ao_normals.format() == normals_format;
    const bool have_ao = !want_ao || (sized(view._ao_raw, aw, ah) && view._ao_across);
    const bool have_contact = !want_contact || (sized(view._contact_raw, w, h) && view._contact_across);
    if (have_base && have_ao && have_contact) {
        return true;
    }
    try {
        constexpr SDL_GPUTextureUsageFlags stored = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        if (!have_base) {
            GpuTexture depth = _device->create_target(w, h, _ao_depth_format,
                                                      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            GpuTexture normals = _device->create_target(w, h, normals_format,
                                                        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            view._ao_depth = std::move(depth);
            view._ao_normals = std::move(normals);
        }
        if (!have_ao) {
            // The blur goes raw -> across -> raw: the second pass writes over the first dispatch's result, which nothing
            // reads again, so the result is in _ao_raw and two textures do.
            GpuTexture raw = _device->create_target(aw, ah, ao_format, stored);
            GpuTexture across = _device->create_target(aw, ah, ao_format, stored);
            view._ao_raw = std::move(raw);
            view._ao_across = std::move(across);
        }
        if (!have_contact) {
            GpuTexture raw = _device->create_target(w, h, ao_format, stored);
            GpuTexture across = _device->create_target(w, h, ao_format, stored);
            view._contact_raw = std::move(raw);
            view._contact_across = std::move(across);
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: ambient occlusion targets not made", (LogFields{{.name = "reason", .value = e.what()}}));
        for (GpuTexture* texture : {&view._ao_chain, &view._ao_depth, &view._ao_normals, &view._ao_raw, &view._ao_across,
                                    &view._contact_raw, &view._contact_across}) {
            *texture = {};
        }
        return false;
    }
    // Made now rather than on the first draw, as the view's own.
    for (const bool cull : {true, false}) {
        if (velocity) {
            for (const bool mask : {false, true}) {
                velocity_pipeline(_ao_depth_format, cull, mask);
                velocity_pipeline(_depth_format, cull, mask);
            }
        } else {
            pipeline(ao_normal_format, 1, cull, Pass::Prepass, false);
            pipeline(ao_normal_format, 1, cull, Pass::Prepass, true);
        }
    }
    return true;
}

bool GpuRenderer3D::ensure_velocity_target(GpuView3D& view) {
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    if (view._ao_normals && view._ao_normals.width() == w && view._ao_normals.height() == h && view._ao_normals.format() == hdr_format) {
        return true;
    }
    try {
        view._ao_normals = _device->create_target(w, h, hdr_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: velocity target not made", (LogFields{{.name = "reason", .value = e.what()}}));
        view._ao_normals = {};
        return false;
    }
    // Made now rather than on the first draw, as the view's own.
    for (const bool cull : {true, false}) {
        for (const Pass pass : {Pass::Opaque, Pass::Alpha, Pass::Additive}) {
            pipeline(hdr_format, 1, cull, pass, false, nullptr, /*velocity=*/true);
        }
    }
    return true;
}

// The linear depth chain of the pre-pass's depth (ao_depth_chain.comp): made with the first use and when the view's size changes.
// False when it cannot be made or built: the caller then reads the depth itself.
bool GpuRenderer3D::build_ao_chain(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Mat4f& projection, f32 radius) {
    if (!_gtao_chain_pipeline || !_ao_chain_pipeline) {
        return false;
    }
    const u32 w = view._ao_depth.width(), h = view._ao_depth.height();
    if (!view._ao_chain || view._ao_chain.width() != w || view._ao_chain.height() != h) {
        try {
            view._ao_chain = _device->create_target(w, h, hiz_format, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
                                                    SDL_GPU_SAMPLECOUNT_1, ao_chain_levels);
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: the occlusion's depth chain not made", (LogFields{{.name = "reason", .value = e.what()}}));
            view._ao_chain = {};
            return false;
        }
    }
    const Unprojection unproject = unproject_coefficients(projection);
    OcclusionChainUniforms u{};
    std::memcpy(u.unproject_zw, unproject.zw, sizeof(unproject.zw));
    // The weights of a texel in the average (the falloff of gtao_chain.comp, over XeGTAO's 0.75 of the radius): a texel nearer than
    // the farthest of its four by the range counts for nothing.
    const f32 effect = 0.75f * radius;
    const f32 range = 0.615f * effect;
    u.weights[0] = -1.0f / range;
    u.weights[1] = (effect - range) / range + 1.0f;
    for (u32 level = 0; level < ao_chain_levels; ++level) {
        u.sizes[2 * level] = std::max(1u, w >> level);
        u.sizes[2 * level + 1] = std::max(1u, h >> level);
    }
    SDL_GPUStorageTextureReadWriteBinding out[ao_chain_levels];
    for (u32 level = 0; level < ao_chain_levels; ++level) {
        out[level] = {view._ao_chain.handle(), level, 0, false};
    }
    SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, out, ao_chain_levels, nullptr, 0);
    if (!pass) {
        return false;
    }
    const SDL_GPUTextureSamplerBinding depth{view._ao_depth.handle(), _point_sampler};
    SDL_BindGPUComputePipeline(pass, _ao_chain_pipeline);
    SDL_BindGPUComputeSamplers(pass, 0, &depth, 1);
    SDL_PushGPUComputeUniformData(commands, 0, &u, sizeof(u));
    SDL_DispatchGPUCompute(pass, (w + 15) / 16, (h + 15) / 16, 1);
    SDL_EndGPUComputePass(pass);
    return true;
}

void GpuRenderer3D::compute_occlusion(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera, const Mat4f& projection,
                                      const AmbientOcclusion3D& ao, Render3DStats& stats, bool blur_too) {
    const Unprojection unproject = unproject_coefficients(projection);
    const f32 w = static_cast<f32>(view._ao_raw.width()), h = static_cast<f32>(view._ao_raw.height());
    const f32 depth_w = static_cast<f32>(view._ao_depth.width()), depth_h = static_cast<f32>(view._ao_depth.height());
    _chain_ready = false;
    SDL_GPUTextureSamplerBinding depth{view._ao_depth.handle(), _point_sampler};
    const auto dispatch = [&](SDL_GPUComputePipeline* pipeline, const GpuTexture& target, const GpuTexture& second, const void* uniforms, u32 bytes) {
        const std::array<SDL_GPUTextureSamplerBinding, 2> inputs{depth, SDL_GPUTextureSamplerBinding{second.handle(), _point_sampler}};
        if (compute_pass(commands, pipeline, target, inputs, uniforms, bytes)) {
            ++stats.ao_dispatches;
        }
    };
    OcclusionUniforms u{};
    std::memcpy(u.unproject_xy, unproject.xy, sizeof(unproject.xy));
    std::memcpy(u.unproject_zw, unproject.zw, sizeof(unproject.zw));
    std::memcpy(u.view, camera.view().m, sizeof(u.view));
    const f32 sizes[4] = {w, h, depth_w, depth_h};
    std::memcpy(u.sizes, sizes, sizeof(sizes));
    // Pixels a unit spans at distance 1 (an orthographic view: at any distance).
    const f32 shape[4] = {std::max(ao.radius, 0.001f), std::max(ao.intensity, 0.0f), projection.at(1, 1) * 0.5f * depth_h,
                          camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f};
    std::memcpy(u.shape, shape, sizeof(shape));
    const bool use_chain = _ao_chain_mode == 2 || (_ao_chain_mode == 0 && static_cast<u64>(depth_w) * static_cast<u64>(depth_h) >= ao_chain_min_pixels);
    if (use_chain && build_ao_chain(commands, view, projection, u.shape[0])) {
        // The chain stands in for the depth as the first input.
        const std::array<SDL_GPUTextureSamplerBinding, 2> inputs{SDL_GPUTextureSamplerBinding{view._ao_chain.handle(), _point_sampler},
                                                                 SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _point_sampler}};
        if (compute_pass(commands, _gtao_chain_pipeline, view._ao_raw, inputs, &u, sizeof(u))) {
            ++stats.ao_dispatches;
            stats.ao_chain_built = 1;
            // The blurs (this one's, the contact shadows', the merged) read the chain's first level instead of the depth.
            _chain_ready = _ao_blur_chain_pipeline && _ao_blur_both_chain_pipeline;
            if (_chain_ready) {
                depth = inputs[0];
            }
        }
    } else {
        dispatch(_gtao_pipeline, view._ao_raw, view._ao_normals, &u, sizeof(u));
    }
    if (!blur_too) {
        return;
    }

    OcclusionBlurUniforms blur{};
    std::memcpy(blur.sizes, sizes, sizeof(sizes));
    std::memcpy(blur.depth, unproject.zw, sizeof(unproject.zw));
    blur.step[0] = 1.0f;
    SDL_GPUComputePipeline* const blur_pipeline = _chain_ready ? _ao_blur_chain_pipeline : _ao_blur_pipeline;
    dispatch(blur_pipeline, view._ao_across, view._ao_raw, &blur, sizeof(blur));
    blur.step[0] = 0.0f;
    blur.step[1] = 1.0f;
    dispatch(blur_pipeline, view._ao_raw, view._ao_across, &blur, sizeof(blur));
}

void GpuRenderer3D::compute_contact_shadows(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera, const Mat4f& projection,
                                            const ContactShadows3D& contact, const Vec3f& sun, Render3DStats& stats, bool blur_too) {
    const Unprojection unproject = unproject_coefficients(projection);
    const f32 w = static_cast<f32>(view._contact_raw.width()), h = static_cast<f32>(view._contact_raw.height());
    SDL_GPUTextureSamplerBinding depth{view._ao_depth.handle(), _point_sampler}; // (the march reads the depth; its blur the chain, when there is one)
    const auto dispatch = [&](SDL_GPUComputePipeline* pipeline, const GpuTexture& target, const GpuTexture& second, const void* uniforms, u32 bytes) {
        const std::array<SDL_GPUTextureSamplerBinding, 2> inputs{depth, SDL_GPUTextureSamplerBinding{second.handle(), _point_sampler}};
        if (compute_pass(commands, pipeline, target, inputs, uniforms, bytes)) {
            ++stats.contact_dispatches;
        }
    };
    ContactUniforms u{};
    std::memcpy(u.view, camera.view().m, sizeof(u.view));
    std::memcpy(u.projection, projection.m, sizeof(u.projection));
    std::memcpy(u.unproject_xy, unproject.xy, sizeof(unproject.xy));
    std::memcpy(u.unproject_zw, unproject.zw, sizeof(unproject.zw));
    const f32 sizes[4] = {w, h, w, h};
    std::memcpy(u.sizes, sizes, sizeof(sizes));
    const Vec3f to_sun = normalize(camera.view().apply_vector(normalize(sun)));
    const f32 sun_view[4] = {to_sun.x, to_sun.y, to_sun.z, camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f};
    std::memcpy(u.sun, sun_view, sizeof(sun_view));
    const f32 shape[4] = {std::max(contact.distance, 0.001f), std::max(contact.thickness, 0.001f), std::clamp(contact.intensity, 0.0f, 1.0f),
                          static_cast<f32>(std::clamp<u32>(contact.steps, min_contact_steps, max_contact_steps))};
    std::memcpy(u.shape, shape, sizeof(shape));
    dispatch(_contact_pipeline, view._contact_raw, view._ao_normals, &u, sizeof(u));
    if (!blur_too) {
        return;
    }

    // The AO's blur, depth-aware, on a full-size result.
    OcclusionBlurUniforms blur{};
    std::memcpy(blur.sizes, sizes, sizeof(sizes));
    std::memcpy(blur.depth, unproject.zw, sizeof(unproject.zw));
    blur.step[0] = 1.0f;
    SDL_GPUComputePipeline* const blur_pipeline = _chain_ready ? _ao_blur_chain_pipeline : _ao_blur_pipeline;
    depth.texture = _chain_ready ? view._ao_chain.handle() : view._ao_depth.handle();
    dispatch(blur_pipeline, view._contact_across, view._contact_raw, &blur, sizeof(blur));
    blur.step[0] = 0.0f;
    blur.step[1] = 1.0f;
    if (!_contact_classic) {
        // One pass (contact shadows alone; with the occlusion the merged blur does both of its passes for both, sharing the weights, and
        // costs no more): the second pass (down) changed the pictures by under 0.3 levels (measured). The result is in _contact_across,
        // and the two textures change places.
        std::swap(view._contact_raw, view._contact_across);
        return;
    }
    dispatch(blur_pipeline, view._contact_raw, view._contact_across, &blur, sizeof(blur));
}

void GpuRenderer3D::blur_screen_space_both(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Mat4f& projection, Render3DStats& stats) {
    // ao_blur_both.comp: the occlusion's and the contact shadows' two blur dispatches as one each (the same size, so the same depth
    // weights). Each feature is counted for the dispatches it is in.
    const Unprojection unproject = unproject_coefficients(projection);
    const f32 w = static_cast<f32>(view._ao_raw.width()), h = static_cast<f32>(view._ao_raw.height());
    const f32 sizes[4] = {w, h, static_cast<f32>(view._ao_depth.width()), static_cast<f32>(view._ao_depth.height())};
    OcclusionBlurUniforms blur{};
    std::memcpy(blur.sizes, sizes, sizeof(sizes));
    std::memcpy(blur.depth, unproject.zw, sizeof(unproject.zw));
    const auto dispatch = [&](const GpuTexture& ao_into, const GpuTexture& ao_from, const GpuTexture& contact_into, const GpuTexture& contact_from) {
        const std::array<SDL_GPUTextureSamplerBinding, 3> inputs{SDL_GPUTextureSamplerBinding{_chain_ready ? view._ao_chain.handle() : view._ao_depth.handle(), _point_sampler},
                                                                 SDL_GPUTextureSamplerBinding{ao_from.handle(), _point_sampler},
                                                                 SDL_GPUTextureSamplerBinding{contact_from.handle(), _point_sampler}};
        if (compute_pass_two(commands, _chain_ready ? _ao_blur_both_chain_pipeline : _ao_blur_both_pipeline, ao_into, contact_into, inputs, &blur, sizeof(blur))) {
            ++stats.ao_dispatches;
            ++stats.contact_dispatches;
            ++stats.blur_merged;
        }
    };
    blur.step[0] = 1.0f;
    dispatch(view._ao_across, view._ao_raw, view._contact_across, view._contact_raw);
    blur.step[0] = 0.0f;
    blur.step[1] = 1.0f;
    dispatch(view._ao_raw, view._ao_across, view._contact_raw, view._contact_across);
}

void GpuRenderer3D::finish_view(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& source, bool plain, bool dither) {
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
        set_viewport(pass, target.width(), target.height());
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
    const bool bloom = !plain && view._options.bloom > 0.0f && view._bloom_a && view._bloom_b;
    if (bloom) {
        SDL_GPUGraphicsPipeline* p = fullscreen(_bloom_shader.handle(), hdr_format);
        const SDL_GPUTexture* scene = source.handle();
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
    // A plain (8-bit) view has no exposure or tonemap: its light is cut off at white, as drawn without
    // temporal anti-aliasing, and only encoded for a gamma target.
    const f32 mode = plain ? (output == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ? 3.0f : 2.0f)
                   : output == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ? 0.0f
                   : output == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ? 1.0f
                                                                         : 2.0f;
    const std::array<const SDL_GPUTexture*, 2> sources{source.handle(), bloom ? view._bloom_a.handle() : source.handle()};
    const f32 params[4] = {bloom ? view._options.bloom : 0.0f, plain ? 1.0f : view._options.exposure, mode, 0.0f};
    pass_into(view._resolved->texture(), fullscreen(dither && _output_dither_shader ? _output_dither_shader.handle() : _output_shader.handle(), output),
              sources, params);
}

void GpuRenderer3D::suspend_taa(GpuView3D& view) {
    if (view._taa_valid || view._taa_frame != 0 || !view._taa_last.groups.empty() || !view._taa_now.groups.empty() ||
        !view._taa_last.ids.empty() || !view._taa_now.ids.empty()) {
        view._taa_valid = false;
        view._taa_frame = 0;
        view._taa_last = {};
        view._taa_now = {};
    }
}

namespace {
u64 texture_bytes(const GpuTexture& t) {
    return t ? static_cast<u64>(SDL_CalculateGPUTextureFormatSize(t.format(), t.width(), t.height(), 1)) : 0;
}
} // namespace

u64 GpuRenderer3D::feature_bytes(const GpuView3D& view) const {
    u64 bytes = 0;
    for (const GpuTexture* t : {&view._ao_chain, &view._ao_depth, &view._ao_normals, &view._ao_raw, &view._ao_across, &view._contact_raw, &view._contact_across,
                                &view._taa_depth, &view._taa_in, &view._taa_sharp, &view._taa_motion, &view._taa_history[0], &view._taa_history[1],
                                &view._occ_depth}) {
        bytes += texture_bytes(*t);
    }
    for (const GpuTexture& t : view._hiz) {
        bytes += texture_bytes(t);
    }
    for (const std::shared_ptr<GpuBuffer>& h : view._occ_history) {
        bytes += h ? h->size() : 0u;
    }
    for (const std::shared_ptr<GpuBuffer>& b : {_gcb[1].scan, _gcb[1].blocks, _gcb[1].args, _gcb[1].list, _gcb[1].counts, _gcb[1].indices, _gc_job_history}) {
        bytes += b ? b->size() : 0u;
    }
    return bytes;
}

void GpuRenderer3D::release_taa(GpuView3D& view) {
    // Nothing made, nothing to forget: the common case of a view that never used it.
    if (!view._taa_depth && !view._taa_history[0] && !view._taa_valid) {
        return;
    }
    view._taa_depth = {};
    view._taa_in = {};
    view._taa_sharp = {};
    view._taa_motion = {};
    view._taa_history = {};
    view._taa_valid = false;
    view._taa_frame = 0;
    view._taa_last = {};
    view._taa_now = {};
}

bool GpuRenderer3D::ensure_taa_targets(GpuView3D& view, bool sharpen) {
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    const auto sized = [&](const GpuTexture& t) { return t && t.width() == w && t.height() == h; };
    bool made = false;
    try {
        constexpr SDL_GPUTextureUsageFlags stored = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        if (!sized(view._taa_depth)) {
            view._taa_depth = _device->create_target(w, h, _depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
            made = true;
        }
        for (GpuTexture& history : view._taa_history) {
            if (!sized(history)) {
                history = _device->create_target(w, h, hdr_format, stored);
                view._taa_valid = false; // nothing in it
                made = true;
            }
        }
        if (!sized(view._taa_motion)) {
            view._taa_motion = _device->create_target(w, h, hdr_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            view._taa_valid = false;
            made = true;
        }
        if (!view._hdr && !sized(view._taa_in)) {
            view._taa_in = _device->create_target(w, h, hdr_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            made = true;
        }
        if (sharpen && !sized(view._taa_sharp)) {
            view._taa_sharp = _device->create_target(w, h, hdr_format, stored);
            made = true;
        } else if (!sharpen) {
            view._taa_sharp = {};
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: temporal anti-aliasing targets not made", (LogFields{{.name = "reason", .value = e.what()}}));
        release_taa(view);
        return false;
    }
    if (made) {
        // Made now rather than on the first draw, as the view's own.
        for (const Pass pass : {Pass::Opaque, Pass::Alpha, Pass::Additive}) {
            pipeline(hdr_format, 1, true, pass);
            pipeline(hdr_format, 1, false, pass);
        }
        fullscreen(_output_shader.handle(), view._output_format);
    }
    return true;
}

namespace {

// Matching a copy with no id to last frame's: a copy in the same place in its mesh's list is
// taken to be the same if it has moved no more than match_by_place_move times its size, else
// the nearest of the mesh's copies within match_by_place_reach times its size is looked for.
constexpr f32 match_by_place_move = 8.0f;
constexpr f32 match_by_place_reach = 2.0f;

// Hashes for matching copies from one frame to the next.
u64 mix64(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

} // namespace

void GpuRenderer3D::match_copies(GpuView3D& view, bool history_usable, JobSystem* jobs, bool skinned) {
    // A copy's past is looked for in the last frame's copies of the same mesh in the
    // same scene (the world or the overlay): by its MeshInstance::id when it has one
    // (a copy of that mesh with that id), else by its place in the mesh's copies in
    // draw order, when there are as many as before and it has not moved more than
    // eight times its size (else it is probably another one). Failing that, by where it
    // is: the nearest copy of the mesh last frame within twice its size (a crowd that
    // lost one in the middle of its list, or gained one, still matches). A copy none of
    // these find has no past: new. The camera's own motion is not part of any of this.
    //
    // Most copies need none of that: no id, and the same matrix in the same place as
    // last frame (or no history to look in). Those are settled first, in pieces on the
    // workers when there are many; the rest are marked and taken here in draw order, so
    // that "the first copy with an id wins" holds however the pieces were run.
    GpuCopyHistory& now = view._taa_now;
    const GpuCopyHistory& last = view._taa_last;
    now.groups.clear();
    now.ids.clear();
    struct Group {
        u64 key;
        u32 first, count, past_first, past_count;
        bool grid_made = false;
        f32 cell = 1.0f;
        std::unordered_multimap<u64, u32> grid; // last frame's copies' centres by cell (made only if needed)
    };
    std::vector<Group> groups;
    const u32 ref_count = static_cast<u32>(_refs.size());
    const auto key_of_ref = [&](u32 r) { return mix64(reinterpret_cast<std::uintptr_t>(_refs[r].mesh) * 2 + _refs[r].scene); };
    // Groups in order of first appearance. Found by sorting the keys, not by a map made anew each frame.
    std::vector<u32>& group_of = _taa_group_of; // first the first draw of the same group, then its group
    {
        _taa_keyed.resize(ref_count);
        for (u32 r = 0; r < ref_count; ++r) {
            _taa_keyed[r] = {key_of_ref(r), r};
        }
        std::sort(_taa_keyed.begin(), _taa_keyed.end());
        group_of.resize(ref_count);
        for (u32 i = 0; i < ref_count;) {
            u32 j = i;
            for (; j < ref_count && _taa_keyed[j].first == _taa_keyed[i].first; ++j) {
                group_of[_taa_keyed[j].second] = _taa_keyed[i].second;
            }
            i = j;
        }
        for (u32 r = 0; r < ref_count; ++r) {
            if (group_of[r] == r) { // the first of its group (a later draw's first is before it)
                const u32 id = static_cast<u32>(groups.size());
                groups.emplace_back(Group{key_of_ref(r), 0, 0, 0, 0});
                group_of[r] = id;
            } else {
                group_of[r] = group_of[group_of[r]];
            }
            groups[group_of[r]].count += _refs[r].draw->count;
        }
    }
    u32 total = 0;
    for (Group& g : groups) {
        g.first = total;
        total += g.count;
        now.groups.push_back({g.key, g.first, g.count});
        const auto past = std::lower_bound(last.groups.begin(), last.groups.end(), g.key,
                                           [](const GpuCopyHistory::Span& s, u64 key) { return s.key < key; });
        if (history_usable && past != last.groups.end() && past->key == g.key) {
            g.past_first = past->first;
            g.past_count = past->count;
        } else {
            g.past_first = g.past_count = ~0u;
        }
    }
    std::sort(now.groups.begin(), now.groups.end(), [](const GpuCopyHistory::Span& a, const GpuCopyHistory::Span& b) { return a.key < b.key; });
    now.matrices.resize(total);
    _taa_prev.resize(total);
    _taa_slow.resize(total);
    // Skinned copies: each copy's joints in the frame's (a place in `now.joints`), and where its past is, for the joints of last
    // frame to be found by the same match as its model matrix.
    now.joint_first.resize(skinned ? total : 0);
    if (skinned) {
        _taa_past.resize(total);
        _taa_prev_joint.resize(total);
    }
    // Each draw's first slot among its group's, and where the same place was last frame.
    _taa_copy_refs.resize(ref_count);
    {
        std::vector<u32> used(groups.size(), 0);
        for (u32 r = 0; r < ref_count; ++r) {
            const Group& g = groups[group_of[r]];
            const bool same_count = g.past_count == g.count;
            _taa_copy_refs[r] = {g.first + used[group_of[r]], same_count ? g.past_first + used[group_of[r]] : ~0u};
            used[group_of[r]] += _refs[r].draw->count;
        }
    }
    const auto settle = [&](const Piece& piece) {
        const DrawRef& ref = _refs[piece.ref];
        const CopyRef& place = _taa_copy_refs[piece.ref];
        for (u32 i = piece.begin; i < piece.end; ++i) {
            const MeshInstance& instance = ref.instances[i];
            const u32 flat = piece.flat + (i - piece.begin);
            now.matrices[place.slot + i] = instance.transform;
            if (skinned) {
                now.joint_first[place.slot + i] = ref.skinned ? ref.joint_base + ref.skin_first[i] : ~0u;
            }
            Mat4f before = instance.transform;
            bool settled = instance.id == 0;
            if (settled && history_usable) {
                // Most copies have not moved at all: no need to measure how far.
                settled = place.past != ~0u && (before = last.matrices[place.past + i],
                                                std::memcmp(before.m, instance.transform.m, sizeof(before.m)) == 0);
            }
            _taa_slow[flat] = settled ? 0 : 1;
            if (settled) {
                before.m[3] = 0.0f;
                _taa_prev[flat] = before;
                if (skinned) {
                    // Not moved at all (or no history to look in: nothing to compare with, posed as now).
                    _taa_past[flat] = history_usable && place.past != ~0u ? place.past + i : ~0u;
                }
            }
        }
    };
    if (jobs && _tasks.size() > 2) {
        jobs->parallel_for(static_cast<i32>(_tasks.size() - 1), [&](i32 t) {
            for (u32 p = _tasks[static_cast<u32>(t)]; p < _tasks[static_cast<u32>(t) + 1]; ++p) {
                settle(_pieces[p]);
            }
        });
    } else {
        for (const Piece& piece : _pieces) {
            settle(piece);
        }
    }
    u32 flat = 0;
    for (u32 r = 0; r < ref_count; ++r) {
        const DrawRef& ref = _refs[r];
        Group& g = groups[group_of[r]];
        const bool known = g.past_first != ~0u;
        const bool same_count = known && g.past_count == g.count;
        for (u32 i = 0; i < ref.draw->count; ++i, ++flat) {
            if (!_taa_slow[flat]) {
                continue;
            }
            const MeshInstance& instance = ref.instances[i];
            const u32 slot = _taa_copy_refs[r].slot + i;
            Mat4f before = instance.transform;
            bool fresh = false;
            u32 past_slot = ~0u; // where the copy was last frame (its record's place)
            {
                bool by_id = false;
                if (instance.id != 0) {
                    const u64 id_key = mix64(g.key ^ (static_cast<u64>(instance.id) * 0x9e3779b97f4a7c15ULL));
                    // Recorded whether or not there is a past to look in: the next frame's would else find none.
                    if (now.ids.emplace(id_key, slot).second && history_usable) { // a second copy with the id is matched by place
                        const auto it = last.ids.find(id_key);
                        if (it != last.ids.end()) {
                            before = last.matrices[it->second];
                            past_slot = it->second;
                        } else {
                            fresh = true;
                        }
                        by_id = true;
                    }
                }
                if (!by_id && history_usable) {
                    bool matched = false;
                    if (same_count) {
                        before = last.matrices[g.past_first + (slot - g.first)];
                        matched = std::memcmp(before.m, instance.transform.m, sizeof(before.m)) == 0;
                    }
                    if (matched) {
                        past_slot = g.past_first + (slot - g.first);
                    }
                    if (!matched && known) {
                        Vec3f c;
                        f32 radius = 0.0f;
                        sphere_of(instance.transform, ref.center, ref.radius, c, radius);
                        matched = same_count && length_squared(c - before.apply(ref.center)) <= match_by_place_move * match_by_place_move * radius * radius + 1e-6f;
                        if (matched) {
                            past_slot = g.past_first + (slot - g.first);
                        }
                    }
                    if (!matched && known) {
                        Vec3f c;
                        f32 radius = 0.0f;
                        sphere_of(instance.transform, ref.center, ref.radius, c, radius);
                        // By place in the world: the nearest of last frame's copies of this mesh within twice its size.
                        const f32 reach = std::max(match_by_place_reach * radius, 1e-3f);
                        const auto cell_of = [&](f32 v) { return static_cast<i32>(std::floor(v / g.cell)); };
                        const auto key_of = [](i32 x, i32 y, i32 z) {
                            return mix64((static_cast<u64>(static_cast<u32>(x)) << 42) ^ (static_cast<u64>(static_cast<u32>(y)) << 21) ^
                                         static_cast<u64>(static_cast<u32>(z)));
                        };
                        if (!g.grid_made) {
                            g.grid_made = true;
                            g.cell = reach;
                            for (u32 k = 0; k < g.past_count; ++k) {
                                const Vec3f p = last.matrices[g.past_first + k].apply(ref.center);
                                g.grid.emplace(key_of(cell_of(p.x), cell_of(p.y), cell_of(p.z)), k);
                            }
                        }
                        f32 best = reach * reach;
                        u32 found = ~0u;
                        const i32 cx = cell_of(c.x), cy = cell_of(c.y), cz = cell_of(c.z);
                        const i32 span = std::max(1, static_cast<i32>(std::ceil(reach / g.cell)));
                        for (i32 dz = -span; dz <= span; ++dz) {
                            for (i32 dy = -span; dy <= span; ++dy) {
                                for (i32 dx = -span; dx <= span; ++dx) {
                                    const auto [from, to] = g.grid.equal_range(key_of(cx + dx, cy + dy, cz + dz));
                                    for (auto it = from; it != to; ++it) {
                                        const f32 d = length_squared(last.matrices[g.past_first + it->second].apply(ref.center) - c);
                                        if (d < best) {
                                            best = d;
                                            found = it->second;
                                        }
                                    }
                                }
                            }
                        }
                        matched = found != ~0u;
                        if (matched) {
                            before = last.matrices[g.past_first + found];
                            past_slot = g.past_first + found;
                        }
                    }
                    fresh = !matched;
                }
            }
            if (fresh) {
                before = instance.transform;
                past_slot = ~0u;
            }
            before.m[3] = fresh ? 1.0f : 0.0f; // a zero in any placing: free to say "no past"
            _taa_prev[flat] = before;
            if (skinned) {
                _taa_past[flat] = past_slot;
            }
        }
    }
    if (skinned) {
        // The joints of the copy's past: where that copy's range started in last frame's, if it was a skinned copy then.
        for (u32 f = 0; f < total; ++f) {
            const u32 past = _taa_past[f];
            _taa_prev_joint[f] = past != ~0u && past < last.joint_first.size() ? last.joint_first[past] : ~0u;
        }
    }
}

const GpuTexture* GpuRenderer3D::resolve_taa(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& input,
                                             const TemporalAA3D& taa, bool reset, Vec2f jitter, Render3DStats& stats) {
    const f32 w = static_cast<f32>(view._size.x), h = static_cast<f32>(view._size.y);
    const GpuTexture& past_motion = reset ? view._ao_normals : view._taa_motion;
    const auto dispatch = [&](const GpuTexture& target, const GpuTexture& current, const GpuTexture& history, f32 sharpen, f32 sharpness) {
        const std::array<SDL_GPUTextureSamplerBinding, 4> inputs{
            SDL_GPUTextureSamplerBinding{current.handle(), _point_sampler}, SDL_GPUTextureSamplerBinding{history.handle(), _owner->_sampler_linear},
            SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _point_sampler}, SDL_GPUTextureSamplerBinding{past_motion.handle(), _point_sampler}};
        const TaaUniforms u{{w, h, 1.0f / w, 1.0f / h}, {std::clamp(taa.blend, 0.0f, 0.98f), reset ? 1.0f : 0.0f, sharpen, sharpness}, {jitter.x, jitter.y, 0.0f, 0.0f}};
        if (!compute_pass(commands, _taa_pipeline, target, inputs, &u, sizeof(u))) {
            return false;
        }
        ++stats.taa_dispatches;
        return true;
    };
    const u32 write = 1 - view._taa_read;
    // A history never written is not read (the shader stops at "no history"), but something valid is bound.
    if (!dispatch(view._taa_history[write], input, reset ? input : view._taa_history[view._taa_read], 0.0f, 0.0f)) {
        return &input;
    }
    view._taa_read = write;
    const GpuTexture* shown = &view._taa_history[write];
    const f32 sharpness = std::clamp(taa.sharpness, 0.0f, 1.0f);
    if (sharpness > 0.0f && view._taa_sharp && dispatch(view._taa_sharp, *shown, *shown, 1.0f, sharpness)) {
        shown = &view._taa_sharp;
    }
    return shown;
}

// What render()'s stages share. Filled in the order they run: setup_frame,
// fit_sun_shadow, cull_and_upload, order_draws, choose_point_lights, begin_commands,
// then the passes.
struct GpuRenderer3D::FrameCtx {
    FrameCtx(GpuView3D* v, const Scene3D& s, Render3DStats& st) : view(v), scene(s), stats(st) {}
    GpuView3D* view;
    const Scene3D& scene;
    Render3DStats& stats;

    // The camera, and the overlay's (null without one).
    f32 aspect = 1.0f;
    std::array<const Scene3D*, 2> scenes{};
    std::array<Mat4f, 2> view_projections{}; // exact
    std::array<Mat4f, 2> drawn{};            // with the temporal anti-aliasing's jitter
    std::array<GpuFrustum, 2> frustums{};
    bool sun_shines = false;

    // Temporal anti-aliasing for this frame (off where the device or the targets cannot do it).
    bool taa = false;
    bool taa_reset = false;
    Vec2f jitter{};
    Mat4f main_projection; // what the pre-pass's compute unprojects with: the camera's, jittered with temporal anti-aliasing

    // The sun's shadow: one map over the area, or a plan of cascades.
    bool shadows = false;
    u32 cascades = 0; // the maps to draw
    Mat4f light_vp{};
    GpuFrustum light_frustum{};
    f32 texel_world = 0.0f, depth_range = 1.0f;
    ShadowCascades plan; // with more than one
    std::array<GpuFrustum, max_shadow_cascades> cascade_frustums{}, slice_frustums{};

    std::vector<std::pair<f32, const PointLight*>> chosen;
    GpuEnvironment3D* environment = nullptr; // null: the hemisphere ambient
    bool sky_env_used = false;               // `environment` is the sky's own (Lights3D::sky_lights_scene): in world space, no rotation
    // The sky and the fog (P3a): asked for and possible; `sky_pass` once the image sky's environment is known to be there.
    bool sky = false;
    bool sky_pass = false;
    bool fog = false;

    // Recording.
    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPUBuffer* storage = nullptr;          // the instances
    SDL_GPUBuffer* previous_storage = nullptr; // last frame's matrices (temporal anti-aliasing)
    SDL_GPUBuffer* joints = nullptr;           // skinned copies: the joint matrices,
    SDL_GPUBuffer* joint_offsets = nullptr;    // and each copy's first
    SDL_GPUBuffer* previous_joints = nullptr;        // with temporal anti-aliasing: last frame's joint matrices
    SDL_GPUBuffer* previous_joint_offsets = nullptr; // and where each copy's start in them
    bool overlaid = false;                     // the overlay has draws in view
    bool occluded = false;                     // the world has ambient occlusion this render (set before it is drawn)
    bool contacted = false;                    // and contact shadows
    bool velocity_drawn = false;               // the pre-pass (or the main pass) began, so _ao_normals holds this frame's velocity
    bool prepass_ran = false;                  // the depth and normal pre-pass began this render
    bool main_velocity = false;                // the main pass writes the velocity (taa with nothing else to need a pre-pass)
    const GpuTexture* shown = nullptr;         // the picture after the temporal resolve
    std::array<LodView, 2> lod{};              // Scene3D::lod's choosing, for the scene and the overlay
    bool gpu_clusters = false;                 // clustered meshes are cut in compute this render
    bool occlusion = false;                    // Lod3D::occlusion: the cut runs in two passes (the first drawn before the pyramid is built)
    bool occlusion_second = false;             // and the second is recorded
    bool occlusion_tests = false;              // the second pass tests against a pyramid (else it keeps every cluster the first pass did not draw)
};

void GpuRenderer3D::age_targets(FrameCtx& ctx) {
    GpuView3D& v = *ctx.view;
    Render3DStats& stats = ctx.stats;
    // Used this render: the group's idle count restarts. Idle: it counts up and, on reaching the limit, gives the group back.
    const auto age = [&](u32& idle, bool used, bool held, const std::function<void()>& release) {
        if (used) {
            idle = 0;
        } else if (held && _release_after > 0 && ++idle >= _release_after) {
            release();
            idle = 0;
            ++stats.targets_released;
        } else if (!held) {
            idle = 0;
        }
    };
    age(v._idle_ao, ctx.occluded, v._ao_raw || v._ao_across, [&] {
        v._ao_raw = {};
        v._ao_across = {};
    });
    age(v._idle_contact, ctx.contacted, v._contact_raw || v._contact_across, [&] {
        v._contact_raw = {};
        v._contact_across = {};
    });
    age(v._idle_depth, ctx.prepass_ran, static_cast<bool>(v._ao_depth), [&] {
        v._ao_depth = {};
        v._ao_chain = {};
    });
    // The normals are also the main pass's velocity, and the TAA's last velocity swaps with them while it runs.
    age(v._idle_normals, ctx.prepass_ran || ctx.main_velocity, static_cast<bool>(v._ao_normals), [&] { v._ao_normals = {}; });
    age(v._idle_taa, ctx.taa, v._taa_depth || v._taa_history[0] || v._taa_history[1] || v._taa_in || v._taa_sharp || v._taa_motion,
        [&] { release_taa(v); });
    age(v._idle_occlusion, ctx.occlusion, v._hiz_levels > 0 || v._occ_depth || v._occ_history[0] || v._occ_history[1], [&] {
        v._hiz = {};
        v._hiz_levels = 0;
        v._occ_depth = {};
        v._occ_history = {};
        v._occ_layout.clear();
        v._occ_words = 0;
        v._occ_reset = true;
    });
    // The renderer's second gathered buffer (occlusion culling's 12 bytes a triangle of the room) goes when no view has used it.
    const bool second_held = _gcb[1].indices || _gcb[1].scan || _gc_job_history;
    if (ctx.occlusion) {
        _idle_occlusion_renders = 0;
    } else if (second_held && _release_after > 0 && ++_idle_occlusion_renders >= _release_after) {
        _gcb[1] = {};
        _gc_job_history = {};
        _idle_occlusion_renders = 0;
        ++stats.targets_released;
    }
    stats.feature_bytes = feature_bytes(v);
}

void GpuRenderer3D::ensure_buffer(std::shared_ptr<GpuBuffer>& buffer, u32 bytes, u32 minimum, SDL_GPUBufferUsageFlags usage) {
    if (!buffer || buffer->size() < bytes) {
        const u32 size = std::max({bytes, buffer ? buffer->size() * 2 : 0u, minimum});
        buffer = std::make_shared<GpuBuffer>(_device->create_buffer(usage, nullptr, size));
    }
}

void GpuRenderer3D::resume_2d() {
    _owner->_batch.begin(_owner->current_target(), _owner->_clear_color, /*do_clear=*/false);
}

SDL_GPUTextureSamplerBinding GpuRenderer3D::texture_binding(const Texture& texture, Wrap3D wrap_u, Wrap3D wrap_v) {
    const ITextureBackend* backend = texture.backend().get();
    const auto* gpu = backend && backend->kind() == ITextureBackend::Kind::Gpu ? static_cast<const GpuTextureBackend*>(backend) : nullptr;
    return gpu && gpu->texture() ? SDL_GPUTextureSamplerBinding{gpu->texture().handle(), sampler_for(*gpu, wrap_u, wrap_v)}
                                 : SDL_GPUTextureSamplerBinding{_owner->_white.handle(), _owner->_sampler_linear};
}

// What a pass with cut-outs binds for one: the base colour's alpha, and the cutoff.
void GpuRenderer3D::bind_cutout(SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, const Material3D& material) {
    const SDL_GPUTextureSamplerBinding base = texture_binding(material.texture, material.wrap_u, material.wrap_v);
    SDL_BindGPUFragmentSamplers(pass, 0, &base, 1);
    const CutoffUniforms u{{material.alpha_cutoff, 0.0f, 0.0f, 0.0f}};
    SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
}

GpuRenderer3D::Pass GpuRenderer3D::pass_of(const Material3D& m) {
    return m.wireframe                ? Pass::Wire
         : m.blend == Blend3D::Opaque || m.blend == Blend3D::Mask ? Pass::Opaque
         : m.blend == Blend3D::Alpha  ? Pass::Alpha
                                      : Pass::Additive;
}

void GpuRenderer3D::setup_frame(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    Render3DStats& stats = ctx.stats;
    ctx.aspect = static_cast<f32>(view->_size.x) / static_cast<f32>(view->_size.y);
    const Scene3D* overlay = scene.overlay_if_any();
    ctx.scenes = {&scene, overlay && !overlay->draws().empty() ? overlay : nullptr};
    for (u32 s = 0; s < 2; ++s) {
        if (ctx.scenes[s]) {
            ctx.view_projections[s] = ctx.scenes[s]->camera.view_projection(ctx.aspect);
            ctx.frustums[s] = frustum_of(ctx.view_projections[s]);
        }
    }

    // Temporal anti-aliasing (Scene3D::taa): every pass that draws the picture
    // uses a projection moved by this frame's sub-pixel jitter (`drawn`), while
    // what decides what is seen (the frustums, the cascades' fit) and what is
    // asked of the picture (the pick pass, Camera3D::project and ray) stay
    // exact. It replaces MSAA, and the sums that need the main pass's own
    // depth (the occlusion's, the contact shadows') are given the jittered
    // projection, so that they unproject what the pre-pass drew.
    const TemporalAA3D& taa_settings = scene.taa;
    ctx.taa = taa_settings.enabled;
    if (ctx.taa && !_taa_available) {
        warn_once(_taa_warned, "3D: temporal anti-aliasing asked for, but this device cannot do it: off");
        ctx.taa = false;
    }
    // A sun with no colour lights nothing and casts nothing: no shadow map, no contact shadows.
    ctx.sun_shines = scene.lights.sun.color.x + scene.lights.sun.color.y + scene.lights.sun.color.z > 0.0f;
    // The velocity comes from the main pass when nothing else needs a pre-pass (prepass() decides, as it knows the
    // frame's draws); the pre-pass's targets are made here only for what does.
    read_switches();
    const bool needs_prepass = scene.ambient_occlusion.enabled || (scene.contact_shadows.enabled && ctx.sun_shines) || scene.lod.occlusion.enabled ||
                               !_main_velocity_allowed;
    if (ctx.taa && !(ensure_taa_targets(*view, taa_settings.sharpness > 0.0f) &&
                     (needs_prepass ? ensure_ao_targets(*view, scene.ambient_occlusion.half_resolution, scene.ambient_occlusion.enabled,
                                                       scene.contact_shadows.enabled && ctx.sun_shines, /*velocity=*/true)
                                    : ensure_velocity_target(*view)))) {
        ctx.taa = false;
    }
    if (!ctx.taa) {
        suspend_taa(*view);
    }
    ctx.drawn = ctx.view_projections;
    if (ctx.taa) {
        const u32 period = std::clamp(taa_settings.jitter_samples, 1u, max_taa_jitter_samples);
        const Vec2f raw = taa_jitter(taa_settings.freeze_jitter ? 0u : view->_taa_frame++, period);
        ctx.jitter = {raw.x * taa_settings.jitter_scale, raw.y * taa_settings.jitter_scale};
        for (u32 s = 0; s < 2; ++s) {
            if (ctx.scenes[s]) {
                ctx.drawn[s] = jitter_projection(ctx.view_projections[s], ctx.jitter, view->_size);
            }
        }
        ctx.main_projection = jitter_projection(scene.camera.projection_matrix(ctx.aspect), ctx.jitter, view->_size);
        // The history is of no use after a resize, a cut, or a change of the kind of camera
        // (its pixels no longer line up).
        const bool ortho = scene.camera.projection == Projection3D::Orthographic;
        ctx.taa_reset = !view->_taa_valid || view->_taa_reset_asked || ortho != view->_taa_ortho;
        view->_taa_ortho = ortho;
        view->_taa_reset_asked = false;
    }
    stats.taa_reset = ctx.taa_reset;
    stats.taa_jitter = ctx.jitter;
    stats.samples = ctx.taa ? 1u : view->_samples;
    // The sky and the fog, where the device made their shaders.
    ctx.sky = scene.sky.enabled;
    if (ctx.sky && !_sky_available) {
        warn_once(_sky_warned, "3D: the sky asked for, but this device's shaders for it were not made: off");
        ctx.sky = false;
    }
    const Fog3D& fog = scene.fog;
    ctx.fog = fog.enabled && (fog.density > 0.0f || fog.height_density > 0.0f) && fog.max_opacity > 0.0f;
    if (ctx.fog && !_fog_available) {
        warn_once(_fog_warned, "3D: fog asked for, but this device's shaders for it were not made: off");
        ctx.fog = false;
    }
}

void GpuRenderer3D::fit_sun_shadow(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    // The sun's shadow: an orthographic view along its light over the area.
    const Lights3D& lights = scene.lights;
    const Bounds3& area = lights.shadow_area;
    const Vec3f area_size = area.size();
    ctx.cascades = 0;
    ctx.shadows = ctx.sun_shines && area_size.x > 0 && area_size.y >= 0 && area_size.z > 0 && length_squared(lights.sun.direction) > 0 &&
                  (ctx.cascades = ensure_shadow_map(lights.shadow_resolution, lights.shadow_cascades)) > 0;
    if (ctx.shadows && ctx.cascades > 1) {
        ctx.plan = fit_shadow_cascades(scene.camera, ctx.aspect, normalize(lights.sun.direction), area, ctx.cascades, lights.shadow_distance,
                                       _shadow_size);
        ctx.cascades = ctx.plan.count;
        for (u32 k = 0; k < ctx.cascades; ++k) {
            ctx.cascade_frustums[k] = frustum_of(ctx.plan.cascade[k].view_projection);
            const ShadowCascade& c = ctx.plan.cascade[k];
            ctx.slice_frustums[k] = frustum_of(slice_of(scene.camera, c.slice_near, c.slice_far).view_projection(ctx.aspect));
        }
    } else if (ctx.shadows) {
        const SunBox box = fit_sun_box(area, normalize(lights.sun.direction));
        ctx.light_vp = Mat4f::orthographic(box.lo.x, box.hi.x, box.lo.y, box.hi.y, box.z_near, box.z_far) * box.light_view;
        ctx.light_frustum = frustum_of(ctx.light_vp);
        ctx.texel_world = std::max(box.hi.x - box.lo.x, box.hi.y - box.lo.y) / static_cast<f32>(_shadow_size);
        ctx.depth_range = std::max(box.z_far - box.z_near, 0.01f);
    }
}


void GpuRenderer3D::cull_and_upload(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    Render3DStats& stats = ctx.stats;
    const Lights3D& lights = scene.lights;
    const Bounds3& area = lights.shadow_area;
    // The draws of both scenes, each with its instances where the scene keeps them.
    _refs.clear();
    _gc.active = false;
    std::array<LodView, 2>& lod = ctx.lod;
    for (u32 s = 0; s < 2; ++s) {
        if (ctx.scenes[s] && ctx.scenes[s]->lod.enabled) {
            lod[s] = lod_view_of(ctx.scenes[s]->lod, ctx.scenes[s]->camera, static_cast<f32>(view->_size.y));
        }
    }
    bool any_levels = false, any_clusters = false, any_skinned = false;
    // The frame's joint matrices: the scene's, then the overlay's.
    u32 joint_total = 0;
    std::array<u32, 2> joint_base{};
    for (u32 s = 0; s < 2; ++s) {
        if (!ctx.scenes[s]) {
            continue;
        }
        joint_base[s] = joint_total;
        joint_total += static_cast<u32>(ctx.scenes[s]->joint_matrices().size());
        for (const Scene3D::Draw& draw : ctx.scenes[s]->draws()) {
            const auto* mesh = dynamic_cast<const GpuMesh3D*>(draw.mesh.backend().get());
            if (draw.count == 0 || !mesh) {
                continue;
            }
            if (draw.skinned && (!_skinning_available || !mesh->skin)) {
                warn_once(_skin_warned, "3D: skinned meshes cannot be drawn on this device (the skinning shaders were not made): not drawn");
                continue;
            }
            const Bounds3& b = draw.mesh.bounds();
            const bool caster = ctx.shadows && s == 0 && (draw.material.blend == Blend3D::Opaque || draw.material.blend == Blend3D::Mask) &&
                                draw.material.casts_shadow && !draw.material.wireframe;
            _refs.push_back({&draw, ctx.scenes[s]->instances().data() + draw.first, mesh, s,
                             ctx.scenes[s]->cull ? &ctx.frustums[s] : nullptr, caster, b.center(), b.radius(), 0, 0, 0});
            if (draw.skinned) {
                DrawRef& ref = _refs.back();
                ref.skinned = true;
                ref.skin_first = ctx.scenes[s]->skin_offsets().data() + draw.skin_first;
                ref.joints = ctx.scenes[s]->joint_matrices().data();
                ref.joint_base = joint_base[s];
                any_skinned = true;
            }
            // Levels of detail: only for a mesh that has them, drawn in an order that does not matter (blended
            // copies are drawn furthest first, which a draw per level would break).
            if (lod[s].on && mesh->levels.size() > 1 && draw.material.blend != Blend3D::Alpha) {
                _refs.back().levels = static_cast<u32>(std::min<std::size_t>(mesh->levels.size(), max_mesh_levels));
                any_levels = true;
            }
            // A mesh with clusters is cut instead (its copies stay in the one run, seen first, then the sun's).
            if (lod[s].on && mesh->clusters && draw.material.blend != Blend3D::Alpha) {
                _refs.back().clustered = true;
                any_clusters = true;
            }
        }
    }

    // Pieces of draws, and tasks of about `chunk` instances each (a big draw
    // split, small ones taken together), for the workers when there are many.
    // Small tasks balance over many workers (22,000 copies in chunks of 4096 made six tasks on a 32-thread
    // machine); the scene must be big enough for waking them to pay (measured: a loss at 1,500 copies, a gain from 2,500).
    constexpr u32 chunk = 512, parallel_from = 2048;
    _pieces.clear();
    _tasks.assign(1, 0);
    u32 total = 0, in_task = 0;
    for (u32 r = 0; r < static_cast<u32>(_refs.size()); ++r) {
        const u32 count = _refs[r].draw->count;
        for (u32 begin = 0; begin < count;) {
            const u32 end = std::min(count, begin + (chunk - in_task));
            _pieces.push_back({r, begin, end, total + begin});
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
    JobSystem* jobs = _owner->_jobs && total >= parallel_from ? _owner->_jobs : nullptr;
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

    // What the camera sees, and what only the sun does and may cast a shadow
    // into view (its sphere swept down the light, through the shadow area,
    // reaching the camera's view): each copy's sphere against the planes.
    const Vec3f down = ctx.shadows ? normalize(lights.sun.direction) * -1.0f : Vec3f{};
    _visible.resize(total);
    if (any_levels) {
        _level_of.resize(total);
    }
    each_task([&](Piece& piece) {
        const DrawRef& ref = _refs[piece.ref];
        piece.seen.fill(0);
        piece.extra.fill(0);
        for (auto& cascade : piece.cascade) {
            cascade.fill(0);
        }
        for (u32 i = piece.begin; i < piece.end; ++i) {
            Vec3f c{};
            f32 r = 0.0f, scale = 1.0f;
            if (ref.skinned) {
                // A posed copy's bound is the sphere round its joints' moved spheres, never smaller than the mesh posed
                // (skin_sphere): the pose, not the mesh at rest. If no joint can be bounded, the rest bound stands.
                Vec3f centre = ref.center;
                f32 radius = ref.radius;
                skin_sphere(ref.mesh->skin_bounds, {ref.joints + ref.skin_first[i], ref.mesh->skin_bounds.size()}, centre, radius);
                scale = sphere_of(ref.instances[i].transform, centre, radius, c, r);
            } else if (ref.frustum || ref.caster || ref.levels > 1) {
                scale = sphere_of(ref.instances[i].transform, ref.center, ref.radius, c, r);
            }
            u32 level = 0;
            if (ref.levels > 1) {
                level = lod_level(lod[ref.scene], *ref.mesh, ref.levels, c, r, scale);
                _level_of[piece.flat + i - piece.begin] = static_cast<u8>(level);
            }
            const bool seen = !ref.frustum || sphere_in(*ref.frustum, c, r);
            // With cascades, each takes the casters whose shadow reaches its
            // slice of the view (seen ones too: one far off isn't drawn into the near map).
            u8 members = 0;
            if (ctx.cascades > 1 && ref.caster) {
                const f32 leave = to_leave(area, c, down) + r;
                for (u32 k = 0; k < ctx.cascades; ++k) {
                    if (sphere_in(ctx.cascade_frustums[k], c, r) && shadow_in(ctx.slice_frustums[k], c, r, down, leave)) {
                        members |= cascade_bit(k);
                    }
                }
            }
            const bool sun = !seen && ref.caster &&
                             (ctx.cascades > 1 ? members != 0
                                           : sphere_in(ctx.light_frustum, c, r) && shadow_in(*ref.frustum, c, r, down, to_leave(area, c, down) + r));
            _visible[piece.flat + i - piece.begin] = static_cast<u8>((seen ? camera_bit : sun ? sun_bit : 0) | members);
            piece.seen[level] += seen ? 1u : 0u;
            piece.extra[level] += sun ? 1u : 0u;
            for (u32 k = 0; k < ctx.cascades && ctx.cascades > 1; ++k) {
                piece.cascade[k][level] += in_cascade(members, k) ? 1u : 0u;
            }
        }
    });
    // Each draw's copies: by level, and in each those seen, then those only the sun sees.
    for (DrawRef& ref : _refs) {
        ref.out_count = ref.out_extra = 0;
        ref.cascade_count.fill(0);
        ref.level_seen.fill(0);
        ref.level_extra.fill(0);
        for (u32 k = 0; k < max_shadow_cascades; ++k) {
            ref.cascade_level_count[k].fill(0);
        }
    }
    for (const Piece& piece : _pieces) {
        DrawRef& ref = _refs[piece.ref];
        for (u32 l = 0; l < ref.levels; ++l) {
            ref.level_seen[l] += piece.seen[l];
            ref.level_extra[l] += piece.extra[l];
            for (u32 k = 0; k < ctx.cascades; ++k) {
                ref.cascade_level_count[k][l] += piece.cascade[k][l];
            }
        }
    }
    u32 written = 0, listed = 0;
    for (DrawRef& ref : _refs) {
        ref.out_first = written;
        for (u32 l = 0; l < ref.levels; ++l) {
            ref.level_first[l] = written;
            written += ref.level_seen[l] + ref.level_extra[l];
            ref.out_count += ref.level_seen[l];
            ref.out_extra += ref.level_extra[l];
            ctx.stats.lod_copies_by_level[l] += ref.level_seen[l];
        }
        for (u32 k = 0; k < ctx.cascades && ctx.cascades > 1; ++k) {
            ref.cascade_first[k] = listed;
            for (u32 l = 0; l < ref.levels; ++l) {
                ref.cascade_level_first[k][l] = listed;
                listed += ref.cascade_level_count[k][l];
                ref.cascade_count[k] += ref.cascade_level_count[k][l];
            }
        }
    }
    {
        u32 current = ~0u;
        std::array<u32, max_mesh_levels> seen{}, extra{};
        std::array<std::array<u32, max_mesh_levels>, max_shadow_cascades> in_list{};
        for (Piece& piece : _pieces) {
            if (piece.ref != current) {
                current = piece.ref;
                seen.fill(0);
                extra.fill(0);
                for (auto& list : in_list) {
                    list.fill(0);
                }
            }
            const DrawRef& ref = _refs[piece.ref];
            for (u32 l = 0; l < ref.levels; ++l) {
                piece.out[l] = ref.level_first[l] + seen[l];
                piece.out_extra[l] = ref.level_first[l] + ref.level_seen[l] + extra[l];
                seen[l] += piece.seen[l];
                extra[l] += piece.extra[l];
                for (u32 k = 0; k < ctx.cascades; ++k) {
                    piece.cascade_out[k][l] = ref.cascade_level_first[k][l] + in_list[k][l];
                    in_list[k][l] += piece.cascade[k][l];
                }
            }
        }
    }
    u32 visible_total = 0;
    for (const DrawRef& ref : _refs) {
        visible_total += ref.out_count;
        stats.skinned_copies += ref.skinned ? ref.out_count : 0u;
    }
    stats.culled = total - visible_total;
    if (any_skinned) {
        stats.skin_matrices = joint_total;
    }
    if (ctx.taa) {
        // This frame's joints are the next frame's past (the record is swapped in resolve_history); the matrices are kept whole,
        // so that a copy's range there needs no copying of its own and copies sharing one range share it there too.
        GpuCopyHistory& record = view->_taa_now;
        record.joints.clear();
        if (any_skinned) {
            record.joints.reserve(joint_total);
            for (u32 s = 0; s < 2; ++s) {
                if (ctx.scenes[s]) {
                    record.joints.insert(record.joints.end(), ctx.scenes[s]->joint_matrices().begin(), ctx.scenes[s]->joint_matrices().end());
                }
            }
        }
        match_copies(*view, !ctx.taa_reset, jobs, any_skinned);
    }
    if (any_clusters) {
        ctx.gpu_clusters = _gpu_clusters && ctx.scene.lod.gpu_culling;
        ctx.occlusion = ctx.gpu_clusters && ctx.scene.lod.occlusion.enabled && _occlusion_available;
        cut_clusters(ctx, _owner->_jobs);
        if (!ctx.gpu_clusters) {
            ctx.occlusion = false;
        }
    }
    if (ctx.scene.lod.occlusion.enabled && !ctx.occlusion) {
        warn_once(_occlusion_warned, "3D: occlusion culling asked for, but it needs Lod3D::enabled and gpu_culling, clustered meshes, and a device that can: off");
    }

    // The copies, straight into the upload memory; alpha-blended ones furthest first.
    if (written > 0) {
        const u32 bytes = static_cast<u32>(written * sizeof(GpuInstance3D));
        ensure_buffer(_instances, bytes, min_instance_buffer,
                      SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | (_gpu_clusters ? SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ : 0u));
        if (ctx.taa) {
            _taa_ordered.resize(written);
        }
        if (any_skinned) {
            _skin_ordered.resize(written);
            if (ctx.taa) {
                _skin_prev_ordered.resize(written);
            }
        }
        const auto write = [&](const DrawRef& ref, const MeshInstance& instance, u8* out, u32 at, u32 flat) {
            if (ctx.taa) {
                _taa_ordered[at] = _taa_prev[flat]; // each copy's own slot: safe from the workers
            }
            if (any_skinned) {
                // The copy's first joint matrix in the frame's buffer (0 for a copy that has none), in its place among the instances.
                _skin_ordered[at] = ref.skinned ? ref.joint_base + ref.skin_first[&instance - ref.instances] : 0u;
                if (ctx.taa) {
                    _skin_prev_ordered[at] = ref.skinned ? _taa_prev_joint[flat] : ~0u;
                }
            }
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
                std::array<u32, max_mesh_levels> at = piece.out, at_extra = piece.out_extra;
                for (u32 i = piece.begin; i < piece.end; ++i) {
                    const u8 v = _visible[piece.flat + i - piece.begin];
                    if ((v & in_view_bits) != 0) {
                        const u32 level = ref.levels > 1 ? _level_of[piece.flat + i - piece.begin] : 0u;
                        write(ref, ref.instances[i], out.data(), (v & camera_bit) ? at[level]++ : at_extra[level]++, piece.flat + i - piece.begin);
                    }
                }
            });
            for (const Piece& first : _pieces) {
                const DrawRef& ref = _refs[first.ref];
                if (first.begin != 0 || ref.draw->material.blend != Blend3D::Alpha) {
                    continue;
                }
                const Vec3f eye = ctx.scenes[ref.scene]->camera.position;
                _sorted.clear();
                const u32 flat = first.flat;
                for (u32 i = 0; i < ref.draw->count; ++i) {
                    if (_visible[flat + i] & camera_bit) {
                        const Mat4f& t = ref.instances[i].transform;
                        _sorted.push_back({length_squared(Vec3f{t.m[12], t.m[13], t.m[14]} - eye), i});
                    }
                }
                std::sort(_sorted.begin(), _sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                u32 at = ref.out_first;
                for (const auto& [d, i] : _sorted) {
                    write(ref, ref.instances[i], out.data(), at++, flat + i);
                }
            }
        };
        // Cycled: a view rendered earlier this frame keeps the instances it drew.
        _device->upload_storage_buffer(_instances->handle(), 0, bytes, nullptr, /*cycle=*/true, &fill);

        // Skinned copies: the frame's joint matrices (both scenes', straight into the upload memory) and each copy's first one, in
        // the places the instances are. One buffer each for the frame, grown as the previous ones are: no upload per draw.
        if (any_skinned) {
            const u32 joint_bytes = joint_total * static_cast<u32>(sizeof(Mat4f));
            ensure_buffer(_joints, joint_bytes, min_joint_buffer);
            const std::function<void(std::span<u8>)> fill_joints = [&](std::span<u8> out) {
                for (u32 s = 0; s < 2; ++s) {
                    if (ctx.scenes[s] && !ctx.scenes[s]->joint_matrices().empty()) {
                        std::memcpy(out.data() + static_cast<std::size_t>(joint_base[s]) * sizeof(Mat4f), ctx.scenes[s]->joint_matrices().data(),
                                    ctx.scenes[s]->joint_matrices().size() * sizeof(Mat4f));
                    }
                }
            };
            _device->upload_storage_buffer(_joints->handle(), 0, joint_bytes, nullptr, /*cycle=*/true, &fill_joints);
            const u32 offset_bytes = written * static_cast<u32>(sizeof(u32));
            ensure_buffer(_joint_offsets, offset_bytes, min_caster_list_buffer);
            _device->upload_storage_buffer(_joint_offsets->handle(), 0, offset_bytes, _skin_ordered.data(), /*cycle=*/true);
            if (ctx.taa) {
                // Last frame's matrices, as the record holds them (whole), and where each copy's own start in them.
                const std::vector<Mat4f>& past = view->_taa_last.joints;
                const Mat4f none{};
                const u32 past_bytes = static_cast<u32>(std::max<std::size_t>(past.size(), 1) * sizeof(Mat4f));
                ensure_buffer(_previous_joints, past_bytes, min_joint_buffer);
                _device->upload_storage_buffer(_previous_joints->handle(), 0, past_bytes, past.empty() ? &none : past.data(), /*cycle=*/true);
                ensure_buffer(_previous_joint_offsets, offset_bytes, min_caster_list_buffer);
                _device->upload_storage_buffer(_previous_joint_offsets->handle(), 0, offset_bytes, _skin_prev_ordered.data(), /*cycle=*/true);
            }
        }

        // Temporal anti-aliasing: each copy's matrix of the frame before, in the same places (the velocity pre-pass reads it).
        if (ctx.taa) {
            const u32 previous_bytes = written * static_cast<u32>(sizeof(Mat4f));
            ensure_buffer(_previous, previous_bytes, min_previous_buffer);
            _device->upload_storage_buffer(_previous->handle(), 0, previous_bytes, _taa_ordered.data(), /*cycle=*/true);
        }

        // The cascades' lists: where each copy that casts into one went.
        if (ctx.cascades > 1 && listed > 0) {
            _caster_list.resize(listed);
            each_task([&](Piece& piece) {
                const DrawRef& ref = _refs[piece.ref];
                if (!ref.caster) {
                    return;
                }
                std::array<u32, max_mesh_levels> at = piece.out, at_extra = piece.out_extra;
                auto to = piece.cascade_out;
                for (u32 i = piece.begin; i < piece.end; ++i) {
                    const u8 v = _visible[piece.flat + i - piece.begin];
                    if ((v & in_view_bits) == 0) {
                        continue;
                    }
                    const u32 level = ref.levels > 1 ? _level_of[piece.flat + i - piece.begin] : 0u;
                    const u32 index = (v & camera_bit) ? at[level]++ : at_extra[level]++;
                    for (u32 k = 0; k < ctx.cascades; ++k) {
                        if (in_cascade(v, k)) {
                            _caster_list[to[k][level]++] = index;
                        }
                    }
                }
            });
            const u32 list_bytes = listed * static_cast<u32>(sizeof(u32));
            ensure_buffer(_caster_lists, list_bytes, min_caster_list_buffer);
            _device->upload_storage_buffer(_caster_lists->handle(), 0, list_bytes, _caster_list.data(), /*cycle=*/true);
        }
    }
}

namespace {

// What a cluster of a clustered copy needs to be drawn into: the views (bit 0 the camera's, bit 1 + k the shadow
// map's or cascade k's) whose frustum its sphere touches, as the copy as a whole is tested in cull_and_upload.
struct ClusterViews {
    const GpuFrustum* camera; // null: the camera sees everything of a copy it sees
    bool caster;
    bool seen; // the copy is in the camera's view (else only the sun's)
    u8 visible;
    Vec3f down;
    const Bounds3* area;
    const GpuFrustum* cascade_frustums;
    const GpuFrustum* slice_frustums;
    const GpuFrustum* light_frustum;
    u32 cascades;

    u32 views(Vec3f c, f32 r) const {
        u32 mask = 0;
        if (seen && (!camera || sphere_in(*camera, c, r))) {
            mask |= 1u;
        }
        if (caster) {
            if (cascades > 1) {
                const f32 leave = to_leave(*area, c, down) + r;
                for (u32 k = 0; k < cascades; ++k) {
                    if (in_cascade(visible, k) && sphere_in(cascade_frustums[k], c, r) && shadow_in(slice_frustums[k], c, r, down, leave)) {
                        mask |= 2u << k;
                    }
                }
            } else if (sphere_in(*light_frustum, c, r) && (!camera || shadow_in(*camera, c, r, down, to_leave(*area, c, down) + r))) {
                mask |= 2u;
            }
        }
        return mask;
    }
    bool reaches(Vec3f c, f32 r) const { return views(c, r) != 0; }
};

} // namespace

void GpuRenderer3D::cut_clusters(FrameCtx& ctx, JobSystem* jobs) {
    const auto started = std::chrono::steady_clock::now();
    Render3DStats& stats = ctx.stats;
    const Lights3D& lights = ctx.scene.lights;
    // The copies to cut, and where they stand in the instance buffer (counted as the fill below does).
    std::size_t count = 0;
    for (const Piece& piece : _pieces) {
        DrawRef& ref = _refs[piece.ref];
        if (!ref.clustered) {
            continue;
        }
        if (ref.job_count == 0) {
            ref.job_first = static_cast<u32>(count);
        }
        u32 at = piece.out[0], at_extra = piece.out_extra[0];
        for (u32 i = piece.begin; i < piece.end; ++i) {
            const u8 v = _visible[piece.flat + i - piece.begin];
            if ((v & in_view_bits) == 0) {
                continue;
            }
            if (count == _cluster_jobs.size()) {
                _cluster_jobs.emplace_back();
            }
            ClusterJob& job = _cluster_jobs[count++];
            job.ref = piece.ref;
            job.copy = i;
            job.visible = v;
            job.slot = (v & camera_bit) ? at++ : at_extra++;
            job.scene_draw = static_cast<u32>(ref.draw - ctx.scenes[ref.scene]->draws().data());
            ++ref.job_count;
        }
    }
    _cluster_jobs.resize(std::max(count, _cluster_jobs.size()));
    if (count == 0) {
        return;
    }
    if (ctx.gpu_clusters) {
        cut_clusters_gpu(ctx, count);
        if (ctx.gpu_clusters) {
            return;
        }
    }
    const Vec3f down = ctx.shadows ? normalize(lights.sun.direction) * -1.0f : Vec3f{};
    const auto cut = [&](u32 j) {
        ClusterJob& job = _cluster_jobs[j];
        const DrawRef& ref = _refs[job.ref];
        const LodView& lod = ctx.lod[ref.scene];
        const Camera3D& camera = ctx.scenes[ref.scene]->camera;
        ClusterCutView view;
        view.transform = ref.instances[job.copy].transform;
        view.eye = lod.eye;
        view.forward = lod.forward;
        view.orthographic = lod.orthographic;
        view.per_unit = lod.per_unit;
        view.pixels = lod.pixels;
        view.nearest = camera.z_near;
        view.force_depth = lod.force;
        const ClusterViews cull{ref.frustum, ref.caster, (job.visible & camera_bit) != 0, job.visible, down, &lights.shadow_area,
                                ctx.cascade_frustums.data(), ctx.slice_frustums.data(), &ctx.light_frustum, ctx.cascades};
        thread_local std::vector<u8> seen;
        thread_local std::vector<u32> stack;
        select_clusters(*ref.mesh->clusters, view, cull, job.picks, seen, stack, &job.tested);
    };
    // Measured on the kiln (13 copies, one of them nearly all of the clusters): on workers the cut took 1.1 ms against
    // 0.8 serial, the dispatch costing more than the little work that can be shared; from a couple of dozen copies it pays.
    constexpr std::size_t parallel_from_copies = 24;
    const bool parallel = jobs && count >= parallel_from_copies;
    if (parallel) {
        jobs->parallel_for(static_cast<i32>(count), [&](i32 j) { cut(static_cast<u32>(j)); });
    } else {
        for (u32 j = 0; j < count; ++j) {
            cut(j);
        }
    }

    // Runs: for each copy and view, the clusters whose view bit is set, one after another; a view that has the
    // clusters of an earlier one is that run (one cascade's casters are often the camera's).
    const u32 views = ctx.shadows ? 1 + std::max(ctx.cascades > 1 ? ctx.cascades : 1u, 1u) : 1;
    u64 total = 0;
    for (u32 j = 0; j < count; ++j) {
        ClusterJob& job = _cluster_jobs[j];
        const MeshClusters& hierarchy = *_refs[job.ref].mesh->clusters;
        std::array<u32, cluster_views> triangles{}, clusters{};
        for (const SelectedCluster& pick : job.picks) {
            for (u32 v = 0; v < views; ++v) {
                if (pick.views & (1u << v)) {
                    triangles[v] += hierarchy.clusters[pick.cluster].triangle_count;
                    ++clusters[v];
                }
            }
        }
        job.same_as.fill(0);
        for (u32 v = 0; v < views; ++v) {
            job.runs[v] = {0, triangles[v] * 3};
            for (u32 u = 0; u < v && triangles[v] > 0; ++u) {
                if (clusters[u] != clusters[v] || triangles[u] != triangles[v]) {
                    continue;
                }
                // Equal in number: the same clusters if every one with a bit has the other's too.
                bool same = true;
                for (const SelectedCluster& pick : job.picks) {
                    same = same && ((pick.views >> u) & 1u) == ((pick.views >> v) & 1u);
                }
                if (same) {
                    job.same_as[v] = static_cast<u8>(u + 1);
                    break;
                }
            }
            if (job.same_as[v]) {
                job.runs[v] = job.runs[job.same_as[v] - 1];
            } else if (triangles[v] > 0) {
                job.runs[v].first = static_cast<u32>(total);
                total += triangles[v] * 3ull;
            }
        }
        stats.clusters_tested += job.tested;
        stats.clusters_drawn += clusters[0];
        ++stats.cluster_copies;
    }
    u32 slots = 0;
    for (u32 j = 0; j < count; ++j) {
        slots = std::max(slots, _cluster_jobs[j].slot + 1);
    }
    _cluster_runs.resize(slots);
    for (u32 j = 0; j < count; ++j) {
        const ClusterJob& job = _cluster_jobs[j];
        _cluster_runs[job.slot] = job.runs;
    }
    const auto compacting = std::chrono::steady_clock::now();
    if (total > 0 && total * sizeof(u32) <= std::numeric_limits<u32>::max() / 2) {
        const u32 bytes = static_cast<u32>(total * sizeof(u32));
        ensure_buffer(_cluster_indices, bytes, 256 * 1024, SDL_GPU_BUFFERUSAGE_INDEX);
        const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
            auto* indices = reinterpret_cast<u32*>(out.data());
            const auto write = [&](u32 j) {
                const ClusterJob& job = _cluster_jobs[j];
                const MeshClusters& hierarchy = *_refs[job.ref].mesh->clusters;
                std::array<u32, cluster_views> at{};
                for (u32 v = 0; v < views; ++v) {
                    at[v] = job.runs[v].first;
                }
                for (const SelectedCluster& pick : job.picks) {
                    for (u32 v = 0; v < views; ++v) {
                        if ((pick.views & (1u << v)) && !job.same_as[v]) {
                            hierarchy.append_indices(pick.cluster, indices + at[v]);
                            at[v] += hierarchy.clusters[pick.cluster].triangle_count * 3u;
                        }
                    }
                }
            };
            if (parallel) {
                jobs->parallel_for(static_cast<i32>(count), [&](i32 j) { write(static_cast<u32>(j)); });
            } else {
                for (u32 j = 0; j < count; ++j) {
                    write(j);
                }
            }
        };
        _device->upload_storage_buffer(_cluster_indices->handle(), 0, bytes, nullptr, /*cycle=*/true, &fill);
        stats.cluster_upload_bytes = bytes;
    }
    const auto finished = std::chrono::steady_clock::now();
    stats.cluster_ms = std::chrono::duration<f64, std::milli>(finished - started).count();
    stats.cluster_upload_ms = std::chrono::duration<f64, std::milli>(finished - compacting).count();
}

namespace {

// cluster_cull.comp's Params, cluster_scan.comp's and the others' (std140).
struct CullParams {
    f32 eye[2][4];
    f32 forward[2][4];
    f32 cut[2][4];
    f32 planes[66][4];
    u32 counts[3][4];
    f32 down[4];
    f32 area_lo[4];
    f32 area_hi[4];
    u32 shadow[4];
    u32 range[4];
    u32 sizes[4];
};
// ... and cluster_cull_occ.comp's, which adds the pyramid's view-projection, its size and levels, and the pass.
struct CullParamsOcc : CullParams {
    f32 view_projection[16];
    f32 screen[4]; // x, y the pyramid's base size, z its levels, w the relative depth margin
    u32 occ[4];    // x the pass (1, 2), y where the statistics start in the counts, z the test is on, w the pixel margin
};
struct GcJob {
    u32 slot, scene_caster, visible, first_cluster;
    f32 scale;
    u32 clusters;    // its mesh's
    u32 table_first; // where they start in the chunk's table
    u32 thread_base; // this copy's first thread in its dispatch
};
static_assert(sizeof(GcJob) == 32);

// The most clusters x views the scan handles (its blocks are one dispatch of at most 65535 groups).
constexpr u64 max_gpu_cut_elements = 60'000'000;
// Threads of a group in the cull and gather shaders, and the dispatch's width in groups (a row may not pass 65535).
constexpr u32 gc_threads = 64;
constexpr u32 gc_row = 32768;
// Workgroups that gather one mesh's chosen clusters: each takes every n-th (a few thousand clusters are chosen of tens of thousands).
constexpr u32 gather_groups = 512;

void dispatch_rows(SDL_GPUComputePass* pass, u64 groups, u32& row) {
    if (groups == 0) {
        return;
    }
    row = static_cast<u32>(std::min<u64>(groups, gc_row));
    SDL_DispatchGPUCompute(pass, row, static_cast<u32>((groups + row - 1) / std::max(row, 1u)), 1);
}

} // namespace

SDL_GPUBuffer* GpuRenderer3D::cluster_index_buffer(u32 set) const {
    return _gc.active && _gcb[set].indices ? _gcb[set].indices->handle() : _cluster_indices ? _cluster_indices->handle() : nullptr;
}

// The GPU cut's set-up on the CPU: the copies to cut as a table, the sizes of the buffers; the compute is recorded by
// cluster_pass_gpu once the frame's command buffer exists. Clears ctx.gpu_clusters when it cannot (the CPU then cuts).
void GpuRenderer3D::cut_clusters_gpu(FrameCtx& ctx, std::size_t count) {
    const auto started = std::chrono::steady_clock::now();
    Render3DStats& stats = ctx.stats;
    GcFrame& gc = _gc;
    gc.active = false;
    gc.dispatches.clear();
    gc.bases.clear();
    const u32 views = ctx.shadows ? 1 + std::max(ctx.cascades > 1 ? ctx.cascades : 1u, 1u) : 1;
    // Copies of meshes in one chunk go together (one dispatch covers them): the copies sorted by chunk, keeping their order
    // within it, so that a draw's copies stay one run.
    for (std::size_t j = 0; j < count; ++j) {
        if (!_refs[_cluster_jobs[j].ref].mesh->chunk) {
            ctx.gpu_clusters = false; // a mesh made without the GPU's chunk
            return;
        }
        _refs[_cluster_jobs[j].ref].job_count = 0;
    }
    std::stable_sort(_cluster_jobs.begin(), _cluster_jobs.begin() + static_cast<std::ptrdiff_t>(count), [&](const ClusterJob& a, const ClusterJob& b) {
        return _refs[a.ref].mesh->chunk->id < _refs[b.ref].mesh->chunk->id;
    });
    u64 clusters = 0, bound = 0;
    gc.job_data.resize(count * sizeof(GcJob));
    auto* jobs = reinterpret_cast<GcJob*>(gc.job_data.data());
    for (std::size_t j = 0; j < count; ++j) {
        const ClusterJob& job = _cluster_jobs[j];
        DrawRef& ref = _refs[job.ref];
        if (ref.job_count++ == 0) {
            ref.job_first = static_cast<u32>(j);
        }
        const MeshClusters& hierarchy = *ref.mesh->clusters;
        const u32 per_copy = static_cast<u32>(hierarchy.clusters.size());
        if (gc.dispatches.empty() || gc.dispatches.back().chunk != ref.mesh->chunk.get()) {
            gc.dispatches.push_back({ref.mesh->chunk.get(), static_cast<u32>(j), 0, 0, static_cast<u32>(clusters), 0, 0});
        }
        GcDispatch& d = gc.dispatches.back();
        const Mat4f& transform = ref.instances[job.copy].transform;
        jobs[j] = {job.slot, ref.scene | (ref.caster ? 0x100u : 0u), job.visible, static_cast<u32>(clusters), cluster_scale_of(transform), per_copy,
                   ref.mesh->table_first, d.threads};
        ++d.jobs;
        d.threads += per_copy;
        gc.bases.push_back(static_cast<u32>(clusters));
        clusters += per_copy;
        bound += hierarchy.leaf_triangles;
    }
    if (clusters * views + 1 > max_gpu_cut_elements) {
        ctx.gpu_clusters = false;
        return;
    }
    // Each dispatch's queue of chosen elements has room for all of its (view, cluster)s, one after another in `chosen`.
    u64 queued = 0;
    for (GcDispatch& d : gc.dispatches) {
        d.queue = static_cast<u32>(queued);
        d.queue_size = d.threads * views;
        queued += d.queue_size;
    }
    const u64 room = std::clamp<u64>(std::min<u64>(ctx.scene.lod.max_cluster_triangles, bound * views), 1, 100'000'000);
    try {
        const u32 job_bytes = static_cast<u32>(count * sizeof(GcJob));
        ensure_buffer(_gc_jobs, job_bytes, 4096, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
        _device->upload_storage_buffer(_gc_jobs->handle(), 0, job_bytes, gc.job_data.data(), /*cycle=*/true);
        const u64 elements = clusters * views + 1;
        constexpr SDL_GPUBufferUsageFlags read_write = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        ensure_buffer(_gcb[0].scan, static_cast<u32>(elements * 4), 64 * 1024, read_write);
        ensure_buffer(_gcb[0].blocks, static_cast<u32>(((elements + 1023) / 1024) * 4), 4096, read_write);
        ensure_buffer(_gcb[0].args, static_cast<u32>(views * count * 20), 4096, SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        ensure_buffer(_gcb[0].list, static_cast<u32>(queued * 4), 64 * 1024, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        // The counters (eight statistics, then a queue's length each), zeroed by the upload.
        const u32 counts_bytes = static_cast<u32>((8 + gc.dispatches.size()) * 4);
        ensure_buffer(_gcb[0].counts, counts_bytes, 1024, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        gc.zeros.assign(counts_bytes, 0);
        _device->upload_storage_buffer(_gcb[0].counts->handle(), 0, counts_bytes, gc.zeros.data(), /*cycle=*/true);
        ensure_buffer(_gcb[0].indices, static_cast<u32>(room * 12), 1024 * 1024, SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        gc.table_bytes = job_bytes;
        gc.occlusion = false;
        if (ctx.occlusion) {
            // Lod3D::occlusion (G3d). The history of which clusters showed last frame has a word for each cluster of each copy of
            // each clustered draw of the scene, in the order of the draws; a draw that comes or goes (or a mesh or a count that
            // changes) makes a new layout, and a new layout starts from nothing showing (the first pass then draws nothing and the
            // second tests everything, as in a first frame). A stale history costs speed, never a wrong picture.
            GpuView3D& view = *ctx.view;
            std::vector<GpuView3D::OccRegion> layout;
            u64 words = 0;
            for (DrawRef& ref : _refs) {
                if (!ref.clustered || ref.scene != 0) {
                    continue;
                }
                const u32 per_copy = static_cast<u32>(ref.mesh->clusters->clusters.size());
                layout.push_back({0, static_cast<u32>(ref.draw - ctx.scenes[0]->draws().data()), ref.mesh, ref.draw->count, per_copy});
                ref.hist_first = static_cast<u32>(std::min<u64>(words, ~0u));
                words += static_cast<u64>(ref.draw->count) * per_copy;
            }
            constexpr u64 max_history_words = 32'000'000;
            if (words == 0 || words > max_history_words) {
                KIN_LOG_WARN("render", "3D: occlusion culling: too many clusters in the scene's copies for the history (32 M): off");
                ctx.occlusion = false;
            } else {
                const u32 history_bytes = static_cast<u32>(words * 4);
                const bool same = layout == view._occ_layout && !view._occ_reset && view._occ_history[0] && view._occ_history[1] &&
                                  view._occ_words == words;
                if (!same) {
                    constexpr SDL_GPUBufferUsageFlags read_write = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
                    for (std::shared_ptr<GpuBuffer>& h : view._occ_history) {
                        if (!h || h->size() < history_bytes) {
                            h = std::make_shared<GpuBuffer>(_device->create_buffer(read_write, nullptr, std::max(history_bytes, 1024u)));
                        }
                        _device->upload_storage_buffer(h->handle(), 0, history_bytes, nullptr, /*cycle=*/false);
                    }
                    view._occ_layout = std::move(layout);
                    view._occ_words = static_cast<u32>(words);
                    view._occ_reset = false;
                    view._occ_read = 0;
                }
                gc.job_history.resize(count);
                for (std::size_t j = 0; j < count; ++j) {
                    const ClusterJob& job = _cluster_jobs[j];
                    const DrawRef& ref = _refs[job.ref];
                    gc.job_history[j] = ref.hist_first + job.copy * static_cast<u32>(ref.mesh->clusters->clusters.size());
                }
                ensure_buffer(_gc_job_history, static_cast<u32>(count * 4), 4096, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
                _device->upload_storage_buffer(_gc_job_history->handle(), 0, static_cast<u32>(count * 4), gc.job_history.data(), /*cycle=*/true);
                // The second pass: the camera's view alone, so queues and rooms for one view.
                gc.dispatches_second = gc.dispatches;
                u64 queued_second = 0;
                for (GcDispatch& d : gc.dispatches_second) {
                    d.queue = static_cast<u32>(queued_second);
                    d.queue_size = d.threads;
                    queued_second += d.queue_size;
                }
                const u64 room_second = std::clamp<u64>(std::min<u64>(ctx.scene.lod.max_cluster_triangles, bound), 1, 100'000'000);
                constexpr SDL_GPUBufferUsageFlags read_write = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
                GcBuffers& second = _gcb[1];
                ensure_buffer(second.scan, static_cast<u32>((clusters + 1) * 4), 64 * 1024, read_write);
                ensure_buffer(second.blocks, static_cast<u32>(((clusters + 1 + 1023) / 1024) * 4), 4096, read_write);
                ensure_buffer(second.args, static_cast<u32>(count * 20), 4096, SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
                ensure_buffer(second.list, static_cast<u32>(queued_second * 4), 64 * 1024, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
                // The counters, and after the queues' lengths four statistics (clusters and triangles occluded, then wasted).
                const u32 second_counts = static_cast<u32>((8 + gc.dispatches_second.size() + 4) * 4);
                ensure_buffer(second.counts, second_counts, 1024, read_write);
                gc.zeros.assign(second_counts, 0);
                _device->upload_storage_buffer(second.counts->handle(), 0, second_counts, gc.zeros.data(), /*cycle=*/true);
                ensure_buffer(second.indices, static_cast<u32>(room_second * 12), 1024 * 1024, SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
                gc.room_second = static_cast<u32>(room_second);
                gc.occlusion = true;
            }
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the cluster cut in compute could not allocate: the CPU cuts", (LogFields{{.name = "reason", .value = e.what()}}));
        ctx.gpu_clusters = false;
        return;
    }
    gc.active = true;
    gc.jobs = static_cast<u32>(count);
    gc.views = views;
    gc.clusters = static_cast<u32>(clusters);
    gc.room = static_cast<u32>(room);
    stats.cluster_gpu = true;
    stats.cluster_copies = gc.jobs;
    stats.cluster_upload_bytes = gc.table_bytes;
    stats.clusters_tested = clusters;
    stats.cluster_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
}

// The depth pyramid of Lod3D::occlusion from a single-sample depth (the pre-pass's, or the occluder pass's): a first level a
// quarter of the depth's size (4 x 4 pixels a texel), then each half the one before until a level is at most 2 x 2, the
// farthest depth (reverse Z: the smallest) of what it covers. A pass makes the first level and a pass each four more.
bool GpuRenderer3D::ensure_pyramid(GpuView3D& view, u32 w, u32 h, std::array<std::pair<u32, u32>, GpuView3D::max_hiz_levels>& size, u32& levels) {
    levels = 0;
    for (u32 lw = (w + 3) / 4, lh = (h + 3) / 4;; lw = (lw + 1) / 2, lh = (lh + 1) / 2) {
        size[levels++] = {lw, lh};
        if ((lw <= 2 && lh <= 2) || levels == GpuView3D::max_hiz_levels) {
            break;
        }
    }
    if (view._hiz_levels != levels || !view._hiz[0] || view._hiz[0].width() != size[0].first || view._hiz[0].height() != size[0].second) {
        try {
            view._hiz = {};
            for (u32 i = 0; i < levels; ++i) {
                view._hiz[i] = _device->create_target(size[i].first, size[i].second, hiz_format,
                                                      SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
            }
            view._hiz_levels = levels;
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: occlusion culling: the depth pyramid could not be made", (LogFields{{.name = "reason", .value = e.what()}}));
            view._hiz = {};
            view._hiz_levels = 0;
            return false;
        }
    }
    return true;
}

bool GpuRenderer3D::build_depth_pyramid(FrameCtx& ctx, const GpuTexture& depth) {
    GpuView3D& view = *ctx.view;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    ctx.occlusion_tests = false;
    const u32 w = depth.width(), h = depth.height();
    std::array<std::pair<u32, u32>, GpuView3D::max_hiz_levels> size{};
    u32 levels = 0;
    if (!ensure_pyramid(view, w, h, size, levels)) {
        return false;
    }
    u32 dispatches = 0;
    const SDL_GPUTextureSamplerBinding source{depth.handle(), _point_sampler};
    const u32 first[4] = {w, h, size[0].first, size[0].second};
    if (!compute_pass(commands, _hiz_depth_pipeline, view._hiz[0], std::span<const SDL_GPUTextureSamplerBinding>{&source, 1}, first, sizeof(first))) {
        return false;
    }
    ++dispatches;
    for (u32 base = 0; base + 1 < levels; base += 4) {
        SDL_GPUStorageTextureReadWriteBinding out[4];
        u32 sizes[20] = {};
        sizes[0] = size[base].first;
        sizes[1] = size[base].second;
        for (u32 k = 0; k < 4; ++k) {
            const u32 i = base + 1 + k;
            const bool real = i < levels;
            out[k] = {real ? view._hiz[i].handle() : _hiz_spare[k].handle(), 0, 0, false};
            sizes[4 * (k + 1)] = real ? size[i].first : 0;
            sizes[4 * (k + 1) + 1] = real ? size[i].second : 0;
        }
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, out, 4, nullptr, 0);
        if (!pass) {
            return false;
        }
        const SDL_GPUTextureSamplerBinding level{view._hiz[base].handle(), _point_sampler};
        SDL_BindGPUComputePipeline(pass, _hiz_reduce_pipeline);
        SDL_BindGPUComputeSamplers(pass, 0, &level, 1);
        SDL_PushGPUComputeUniformData(commands, 0, sizes, sizeof(sizes));
        SDL_DispatchGPUCompute(pass, (size[base].first + 15) / 16, (size[base].second + 15) / 16, 1);
        SDL_EndGPUComputePass(pass);
        ++dispatches;
    }
    ctx.stats.occlusion_dispatches += dispatches;
    ctx.stats.occlusion_levels = levels;
    ctx.occlusion_tests = true;
    return true;
}

// Lod3D::occlusion without a pre-pass: the frame's opaque meshes (and the first pass's clusters) into a depth of their own,
// the pyramid from it, and the second pass of the cut. With a pre-pass, prepass() does this between its two halves.
void GpuRenderer3D::ensure_occluder_depth(GpuView3D& view) {
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    if (!view._occ_depth || view._occ_depth.width() != w || view._occ_depth.height() != h) {
        try {
            view._occ_depth = _device->create_target(w, h, _ao_depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: occlusion culling: the occluder depth could not be made", (LogFields{{.name = "reason", .value = e.what()}}));
            view._occ_depth = {};
        }
    }
}

void GpuRenderer3D::occluder_pass(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* storage = ctx.storage;
    bool drawn = false;
    ensure_occluder_depth(*view);
    if (view->_occ_depth) {
        const SDL_GPUDepthStencilTargetInfo depth = depth_target(view->_occ_depth.handle(), SDL_GPU_STOREOP_STORE, /*cycle=*/true);
        if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth)) {
            drawn = true;
            set_viewport(pass, view->_size.x, view->_size.y);
            DrawUniforms u{};
            std::memcpy(u.view_projection, ctx.drawn[0].m, sizeof(u.view_projection));
            // the shadow vertex shaders' second buffer is a list this does not use; a skinned copy's also read the joints
            const std::array<SDL_GPUBuffer*, 4> buffers{storage, storage, ctx.joints, ctx.joint_offsets};
            DrawBinder binder{pass};
            for (const u32 index : _order) {
                const DrawRef& ref = _refs[index];
                if (ref.scene != 0 || pass_of(ref.draw->material) != Pass::Opaque || dynamic_cast<const GpuMaterialShader3D*>(ref.draw->material.shader.backend().get())) {
                    continue; // a shader of its own may discard where the pyramid would take the mesh for solid
                }
                if (ref.skinned && (!ctx.joints || !ctx.joint_offsets)) {
                    continue;
                }
                const Material3D& occluder = ref.draw->material;
                const bool cutout = occluder.blend == Blend3D::Mask;
                SDL_GPUGraphicsPipeline* p = pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, !occluder.double_sided, Pass::Occluder, cutout, nullptr, false, ref.skinned);
                if (!p) {
                    continue;
                }
                binder.bind_pipeline(p, buffers.data(), ref.skinned ? 4 : 2);
                if (ref.clustered && !cluster_index_buffer(0)) {
                    continue;
                }
                binder.bind_mesh(ref.mesh, ref.clustered ? cluster_index_buffer(0) : nullptr);
                if (ref.skinned) {
                    binder.bind_skin(ref.mesh);
                }
                if (cutout) {
                    bind_cutout(commands, pass, occluder);
                }
                if (ref.clustered) {
                    draw_cluster_indirect(ref, commands, pass, u, 0, 0); // the first pass's clusters
                    continue;
                }
                for (u32 l = 0; l < ref.levels; ++l) {
                    if (ref.level_seen[l] == 0) {
                        continue;
                    }
                    u.first[0] = ref.level_first[l];
                    u.first[1] = 0;
                    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                    SDL_DrawGPUIndexedPrimitives(pass, ref.mesh->levels[l].index_count, ref.level_seen[l], ref.mesh->levels[l].first_index, 0, 0);
                }
            }
            SDL_EndGPURenderPass(pass);
        }
    }
    if (drawn) {
        build_depth_pyramid(ctx, view->_occ_depth);
    }
    cluster_pass_gpu(ctx, 2);
}

void GpuRenderer3D::cluster_pass_gpu(FrameCtx& ctx, u32 phase) {
    GcFrame& gc = _gc;
    if (!ctx.gpu_clusters || !gc.active || !ctx.storage) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    SDL_GPUCommandBuffer* commands = ctx.commands;
    const Lights3D& lights = ctx.scene.lights;
    GpuView3D& view = *ctx.view;
    const bool occ = phase != 0;
    // Set 0 is the whole cut (all views); with occlusion, phase 1 makes it (its camera view the first pass's: what showed
    // last frame) and phase 2 makes set 1: the camera's view alone, the rest tested against the pyramid.
    const u32 set = phase == 2 ? 1u : 0u;
    GcBuffers& buf = _gcb[set];
    const std::vector<GcDispatch>& plan = phase == 2 ? gc.dispatches_second : gc.dispatches;
    const u32 views = phase == 2 ? 1u : gc.views;
    const u32 room = phase == 2 ? gc.room_second : gc.room;
    CullParamsOcc p{};
    for (u32 s = 0; s < 2; ++s) {
        if (!ctx.scenes[s]) {
            continue;
        }
        const LodView& lod = ctx.lod[s];
        const Camera3D& camera = ctx.scenes[s]->camera;
        const f32 eye[4] = {lod.eye.x, lod.eye.y, lod.eye.z, camera.z_near};
        const f32 forward[4] = {lod.forward.x, lod.forward.y, lod.forward.z, lod.orthographic ? 1.0f : 0.0f};
        const f32 cut[4] = {lod.per_unit, lod.pixels, static_cast<f32>(lod.force), ctx.scenes[s]->cull ? 1.0f : 0.0f};
        std::memcpy(p.eye[s], eye, sizeof(eye));
        std::memcpy(p.forward[s], forward, sizeof(forward));
        std::memcpy(p.cut[s], cut, sizeof(cut));
    }
    const auto put_frustum = [&](u32 f, const GpuFrustum& frustum) {
        for (u32 k = 0; k < frustum.count; ++k) {
            const Vec4f& plane = frustum.planes[k];
            const f32 v[4] = {plane.x, plane.y, plane.z, plane.w};
            std::memcpy(p.planes[f * 6 + k], v, sizeof(v));
        }
        p.counts[f >> 2][f & 3] = frustum.count;
    };
    for (u32 s = 0; s < 2; ++s) {
        if (ctx.scenes[s]) {
            put_frustum(s, ctx.frustums[s]);
        }
    }
    for (u32 k = 0; k < ctx.cascades && ctx.cascades > 1; ++k) {
        put_frustum(2 + k, ctx.cascade_frustums[k]);
        put_frustum(6 + k, ctx.slice_frustums[k]);
    }
    put_frustum(10, ctx.light_frustum);
    if (ctx.shadows) {
        const Vec3f down = normalize(lights.sun.direction) * -1.0f;
        const f32 d[4] = {down.x, down.y, down.z, 0.0f};
        std::memcpy(p.down, d, sizeof(d));
    }
    const Bounds3& area = lights.shadow_area;
    const f32 lo[4] = {area.min.x, area.min.y, area.min.z, 0.0f}, hi[4] = {area.max.x, area.max.y, area.max.z, 0.0f};
    std::memcpy(p.area_lo, lo, sizeof(lo));
    std::memcpy(p.area_hi, hi, sizeof(hi));
    p.shadow[0] = ctx.cascades;
    p.shadow[1] = ctx.shadows ? 1u : 0u;
    const Occlusion3D& setting = ctx.scene.lod.occlusion;
    if (occ) {
        std::memcpy(p.view_projection, ctx.drawn[0].m, sizeof(p.view_projection));
        const f32 screen[4] = {static_cast<f32>(view._size.x), static_cast<f32>(view._size.y), static_cast<f32>(view._hiz_levels),
                               std::clamp(setting.depth_margin, 0.0f, 0.5f)};
        std::memcpy(p.screen, screen, sizeof(screen));
        p.occ[0] = phase;
        p.occ[1] = 8 + static_cast<u32>(plan.size());
        p.occ[2] = phase == 2 && ctx.occlusion_tests ? 1u : 0u;
        p.occ[3] = std::min(setting.pixel_margin, 64u);
    }
    const u32 jobs = gc.jobs, total = gc.clusters;
    const u64 elements = static_cast<u64>(total) * views;
    u32 dispatches = 0;
    u32 row = 0;

    // The cull: a count of triangles a (view, cluster), 0 where not drawn.
    {
        SDL_GPUStorageBufferReadWriteBinding rw[4] = {{buf.scan->handle(), /*cycle=*/true}, {buf.list->handle(), true}, {buf.counts->handle(), false}, {}};
        u32 rw_count = 3;
        if (occ) {
            rw[3] = {view._occ_history[view._occ_read ^ 1]->handle(), false};
            rw_count = 4;
        }
        if (SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, rw_count)) {
            SDL_BindGPUComputePipeline(pass, occ ? _gc_cull_occ : _gc_cull);
            if (occ) {
                // The pyramid's levels (all of them dummies in the first pass, which does not look at it).
                SDL_GPUTextureSamplerBinding levels[GpuView3D::max_hiz_levels];
                for (u32 i = 0; i < GpuView3D::max_hiz_levels; ++i) {
                    const bool have = phase == 2 && ctx.occlusion_tests && i < view._hiz_levels;
                    levels[i] = {have ? view._hiz[i].handle() : _owner->_white.handle(), have ? _point_sampler : _owner->_sampler_linear};
                }
                SDL_BindGPUComputeSamplers(pass, 0, levels, GpuView3D::max_hiz_levels);
            }
            u32 number = 0;
            for (const GcDispatch& d : plan) {
                SDL_GPUBuffer* ro[5] = {d.chunk->table, _gc_jobs->handle(), ctx.storage, occ ? _gc_job_history->handle() : nullptr,
                                        occ ? view._occ_history[view._occ_read]->handle() : nullptr};
                SDL_BindGPUComputeStorageBuffers(pass, 0, ro, occ ? 5 : 3);
                const u64 threads = d.threads;
                p.range[0] = d.first_job;
                p.range[1] = 0;
                p.range[2] = d.jobs;
                p.range[3] = d.threads;
                p.sizes[0] = total;
                p.sizes[1] = views;
                p.sizes[2] = gc_row;
                p.sizes[3] = number++;
                p.shadow[2] = d.queue;
                SDL_PushGPUComputeUniformData(commands, 0, &p, occ ? sizeof(CullParamsOcc) : sizeof(CullParams));
                dispatch_rows(pass, (threads + gc_threads - 1) / gc_threads, row);
                ++dispatches;
            }
            SDL_EndGPUComputePass(pass);
        }
    }
    // The scan: block sums, their scan, the blocks scanned and offset.
    const u32 blocks = static_cast<u32>((elements + 1 + 1023) / 1024);
    for (u32 mode = 0; mode < 3; ++mode) {
        const SDL_GPUStorageBufferReadWriteBinding rw[2] = {{buf.scan->handle(), false}, {buf.blocks->handle(), mode == 0}};
        if (SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2)) {
            SDL_BindGPUComputePipeline(pass, _gc_scan);
            const u32 u[4] = {mode, static_cast<u32>(elements), blocks, 0};
            SDL_PushGPUComputeUniformData(commands, 0, u, sizeof(u));
            SDL_DispatchGPUCompute(pass, mode == 1 ? 1 : blocks, 1, 1);
            SDL_EndGPUComputePass(pass);
            ++dispatches;
        }
    }
    // The indirect arguments: each (view, copy)'s run of the gathered indices.
    {
        const SDL_GPUStorageBufferReadWriteBinding rw{buf.args->handle(), true};
        if (SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &rw, 1)) {
            SDL_BindGPUComputePipeline(pass, _gc_args);
            SDL_GPUBuffer* ro[2] = {_gc_jobs->handle(), buf.scan->handle()};
            SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 2);
            const u32 u[8] = {jobs, views, total, room, gc_row, 0, 0, 0};
            SDL_PushGPUComputeUniformData(commands, 0, u, sizeof(u));
            dispatch_rows(pass, (static_cast<u64>(jobs) * views + gc_threads - 1) / gc_threads, row);
            SDL_EndGPUComputePass(pass);
            ++dispatches;
        }
    }
    // The gather: each chosen cluster's indices to their place.
    {
        const SDL_GPUStorageBufferReadWriteBinding rw[2] = {{buf.indices->handle(), true}, {buf.counts->handle(), false}};
        if (SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2)) {
            SDL_BindGPUComputePipeline(pass, _gc_gather);
            u32 number = 0;
            for (const GcDispatch& d : plan) {
                SDL_GPUBuffer* ro[5] = {d.chunk->table, d.chunk->indices, buf.scan->handle(), buf.list->handle(), _gc_jobs->handle()};
                SDL_BindGPUComputeStorageBuffers(pass, 0, ro, 5);
                const u32 groups = std::min(gather_groups, d.queue_size);
                const u32 u[8] = {number++, d.queue, jobs, room, total, 0, groups, 0};
                SDL_PushGPUComputeUniformData(commands, 0, u, sizeof(u));
                SDL_DispatchGPUCompute(pass, groups, 1, 1);
                ++dispatches;
            }
            SDL_EndGPUComputePass(pass);
        }
    }
    ctx.stats.cluster_dispatches += dispatches;
    ctx.stats.cluster_ms += std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (phase == 2) {
        ctx.occlusion_second = true;
    }
}

// The indirect draws of `ref`'s copies in view `view` of the GPU cut: one multi-draw over its run of arguments; a copy
// that nothing draws there has a count of 0. The copy's slot is the argument's first instance.
template <typename Uniforms>
void GpuRenderer3D::draw_cluster_indirect(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, Uniforms& u, u32 view, u32 set) const {
    if (ref.job_count == 0) {
        return;
    }
    u.first[0] = 0;
    u.first[1] = 0;
    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, _gcb[set].args->handle(), (view * _gc.jobs + ref.job_first) * 20u, ref.job_count);
}

// Waits for the GPU (as pick does) and reads the cut's counters.
void GpuRenderer3D::read_cluster_stats(FrameCtx& ctx) {
    if (!_gc.active || !_gcb[0].counts) {
        return;
    }
    if (_owner->_frame) {
        _owner->flush_to_frame();
        _owner->_frame->submit();
        _owner->end_frame();
    }
    std::vector<u8> bytes;
    if (!_device->read_buffer(_gcb[0].counts->handle(), 0, 24, bytes) || bytes.size() < 24) {
        return;
    }
    u32 v[6];
    std::memcpy(v, bytes.data(), sizeof(v));
    Render3DStats& stats = ctx.stats;
    stats.clusters_drawn = v[0];
    stats.triangles += v[1]; // the main pass counted the meshes that are not cut
    stats.cluster_overflow = v[2];
    stats.cluster_triangles_all = v[5];
    if (ctx.occlusion && ctx.occlusion_second) {
        // The second pass's clusters (set 1) add to the camera's; its four statistics follow the queues' lengths.
        const u32 words = 8 + static_cast<u32>(_gc.dispatches_second.size()) + 4;
        std::vector<u8> second;
        if (_device->read_buffer(_gcb[1].counts->handle(), 0, words * 4, second) && second.size() >= words * 4u) {
            std::vector<u32> w(words);
            std::memcpy(w.data(), second.data(), words * 4);
            stats.clusters_first_pass = v[0];
            stats.clusters_drawn += w[0];
            stats.triangles += w[1];
            stats.cluster_overflow += w[2];
            stats.cluster_triangles_all += w[5];
            const u32 at = 8 + static_cast<u32>(_gc.dispatches_second.size());
            stats.clusters_occluded = w[at];
            stats.triangles_occluded = w[at + 1];
            stats.clusters_wasted = w[at + 2];
            stats.triangles_wasted = w[at + 3];
        }
    }
    if (stats.cluster_overflow > 0 && !_gc_overflow_warned) {
        _gc_overflow_warned = true;
        KIN_LOG_WARN_F("render", "3D: the cluster cut passed Lod3D::max_cluster_triangles: some clusters are not drawn",
                       (LogFields{{.name = "dropped_clusters", .value = std::to_string(v[2])},
                                  {.name = "dropped_triangles", .value = std::to_string(v[3])},
                                  {.name = "room", .value = std::to_string(_gc.room)}}));
    }
}

bool GpuRenderer3D::read_cluster_draws(IView3DBackend&, std::vector<ClusterDraw>& out) {
    out.clear();
    if (!_gc.active || !_gcb[0].args) {
        return false;
    }
    if (_owner->_frame) {
        _owner->flush_to_frame();
        _owner->_frame->submit();
        _owner->end_frame();
    }
    // With Lod3D::occlusion the second pass's draws (the camera's view, view 0) follow the first set's.
    for (u32 set = 0; set < (_gc.occlusion ? 2u : 1u); ++set) {
        const u32 views = set == 0 ? _gc.views : 1u;
        const u32 count = views * _gc.jobs;
        std::vector<u8> bytes;
        if (!_device->read_buffer(_gcb[set].args->handle(), 0, count * 20, bytes) || bytes.size() < count * 20u) {
            return false;
        }
        for (u32 i = 0; i < count; ++i) {
            u32 a[5];
            std::memcpy(a, bytes.data() + i * 20u, sizeof(a));
            out.push_back({i / _gc.jobs, i % _gc.jobs, a[0], a[2], a[4]});
        }
    }
    return true;
}

bool GpuRenderer3D::read_cluster_picks(IView3DBackend&, std::vector<ClusterPick>& out) {
    out.clear();
    if (!_gc.active || !_gcb[0].scan) {
        return false;
    }
    if (_owner->_frame) {
        _owner->flush_to_frame();
        _owner->_frame->submit();
        _owner->end_frame();
    }
    // With Lod3D::occlusion the second pass's clusters (set 1, the camera's view) are read too.
    for (u32 set = 0; set < (_gc.occlusion ? 2u : 1u); ++set) {
        const u32 views = set == 0 ? _gc.views : 1u;
        const u32 room = set == 0 ? _gc.room : _gc.room_second;
        const u64 elements = static_cast<u64>(_gc.clusters) * views;
        std::vector<u8> bytes;
        if (!_device->read_buffer(_gcb[set].scan->handle(), 0, static_cast<u32>((elements + 1) * 4), bytes) || bytes.size() < (elements + 1) * 4) {
            return false;
        }
        std::vector<u32> scan(elements + 1);
        std::memcpy(scan.data(), bytes.data(), scan.size() * 4);
        u32 job = 0;
        for (u64 e = 0; e < elements; ++e) {
            const u32 n = scan[e + 1] - scan[e];
            if (n == 0 || static_cast<u64>(scan[e]) + n > room) {
                continue;
            }
            const u32 view = static_cast<u32>(e / _gc.clusters), g = static_cast<u32>(e % _gc.clusters);
            while (job + 1 < _gc.jobs && _gc.bases[job + 1] <= g) {
                ++job;
            }
            while (job > 0 && _gc.bases[job] > g) {
                --job;
            }
            const ClusterJob& j = _cluster_jobs[job];
            out.push_back({_refs[j.ref].scene, j.scene_draw, j.copy, view, g - _gc.bases[job]});
        }
    }
    return true;
}

void GpuRenderer3D::order_draws(FrameCtx& ctx) {
    // The draws in order: the scene, then the overlay; in each, opaque, then
    // alpha-blended (furthest first), then additive; then pipeline and mesh.
    const auto pass_of = [](const Material3D& m) {
        return m.wireframe                ? Pass::Wire
             : m.blend == Blend3D::Opaque || m.blend == Blend3D::Mask ? Pass::Opaque
             : m.blend == Blend3D::Alpha  ? Pass::Alpha
                                          : Pass::Additive;
    };
    _order.clear();
    for (u32 r = 0; r < static_cast<u32>(_refs.size()); ++r) {
        if (_refs[r].out_count > 0) {
            _order.push_back(r);
        }
    }
    const auto draw_distance = [&](const DrawRef& ref) {
        const Mat4f& t = ref.instances[0].transform;
        return length_squared(Vec3f{t.m[12], t.m[13], t.m[14]} - ctx.scenes[ref.scene]->camera.position);
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
        return x.mesh->serial < y.mesh->serial; // not the pointer: a tie in depth between two draws would flip with the heap
    });
    // The overlay in front of the world, seen by its own camera, and the velocity pass for it.
    ctx.overlaid = std::any_of(_order.begin(), _order.end(), [&](u32 r) { return _refs[r].scene == 1; });
}

void GpuRenderer3D::choose_point_lights(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    const Lights3D& lights = scene.lights;
    Render3DStats& stats = ctx.stats;
    // Point lights: those reaching the view, strongest and nearest first.
    for (const PointLight& light : lights.points) {
        if (light.radius <= 0 || !sphere_in(ctx.frustums[0], light.position, light.radius)) {
            continue;
        }
        // What it adds a unit from the camera's nearest surface it reaches:
        // its intensity over the square of the distance, at least a unit.
        const f32 strength = std::max({light.color.x, light.color.y, light.color.z});
        const f32 distance = std::max(1.0f, length(light.position - scene.camera.position) - light.radius);
        ctx.chosen.push_back({strength / (distance * distance), &light});
    }
    std::sort(ctx.chosen.begin(), ctx.chosen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    ctx.chosen.resize(std::min<std::size_t>(ctx.chosen.size(), max_lights));
    stats.lights = static_cast<u32>(ctx.chosen.size());
}

// The frame's uniforms, the scene's (s = 0) and the overlay's (1). The overlay's space is its
// camera's: the scene's lights are carried into it (seen from where the scene's camera is),
// without shadows.
void GpuRenderer3D::push_frame_uniforms(const FrameCtx& ctx, u32 s) {
    const Scene3D& scene = ctx.scene;
    const Lights3D& lights = scene.lights;
    const GpuView3D* view = ctx.view;
    // A temporally anti-aliased picture is drawn in linear light into float and encoded after the resolve.
    const bool encode = !ctx.taa && view->_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;     // a gamma target
    const bool decode = _owner->_color_texture_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // textures stored encoded
    FrameUniforms f{};
    const Mat4f to_space = s == 0 ? Mat4f{} : ctx.scenes[1]->camera.view().inverse() * scene.camera.view();
    put(f.sun_direction, normalize(to_space.apply_vector(normalize(lights.sun.direction))), 0.0f);
    put(f.sun_color, lights.sun.color, 0.0f);
    put(f.ambient, lights.ambient, 0.0f);
    put(f.ground_ambient, lights.ground_ambient.value_or(lights.ambient), 0.0f);
    const bool fill = length_squared(lights.fill.direction) > 0;
    put(f.fill_direction, fill ? normalize(to_space.apply_vector(normalize(lights.fill.direction))) : Vec3f{}, 0.0f);
    put(f.fill_color, fill ? lights.fill.color : Vec3f{}, 0.0f);
    const bool lit_by_sun_shadow = ctx.shadows && s == 0;
    put(f.flags, {encode ? 1.0f : 0.0f, decode ? 1.0f : 0.0f, lit_by_sun_shadow ? 1.0f : 0.0f},
        static_cast<f32>(ctx.chosen.size()));
    put(f.eye, ctx.scenes[s]->camera.position, 1.0f);
    if (ctx.environment) {
        // The picture turned by the light's rotation; in the overlay's space the
        // directions come back to the scene's first.
        const Mat4f to_environment = Mat4f::rotation_y(ctx.sky_env_used ? 0.0f : -lights.environment_rotation) * (s == 0 ? Mat4f{} : to_space.inverse());
        put(f.environment, {lights.environment_intensity, static_cast<f32>(ctx.environment->levels - 1), 0.0f}, 1.0f);
        std::memcpy(f.environment_matrix, to_environment.m, sizeof(f.environment_matrix));
    }
    // A texel's worth of depth toward the sun, and one and a half off the
    // surface (a cascade's texel is its own).
    put(f.shadow, {static_cast<f32>(std::max(ctx.cascades, 1u)), 0.0f, _shadow_size > 0 ? 1.0f / static_cast<f32>(_shadow_size) : 0.0f}, 0.0f);
    put(f.cascade_forward, scene.camera.forward(), 0.0f);
    // The overlay is not occluded: it is not in the pre-pass (the world's
    // occlusion would darken a gun by what is behind it, not by its own creases).
    put(f.occlusion, {ctx.occluded && s == 0 ? 1.0f : 0.0f, ctx.contacted && s == 0 ? 1.0f : 0.0f, 1.0f / static_cast<f32>(view->_size.x)},
        1.0f / static_cast<f32>(view->_size.y));
    if (ctx.cascades > 1) {
        for (u32 k = 0; k < ctx.cascades; ++k) {
            const ShadowCascade& c = ctx.plan.cascade[k];
            std::memcpy(f.shadow_matrix + 16 * k, c.view_projection.m, 16 * sizeof(f32));
            f.cascade_bias[4 * k] = c.texel_world / c.depth_range;
            f.cascade_bias[4 * k + 1] = c.texel_world * 1.5f;
            f.cascade_end[k] = c.slice_far;
            f.cascade_blend[k] = c.blend_from;
        }
    } else {
        std::memcpy(f.shadow_matrix, ctx.light_vp.m, 16 * sizeof(f32));
        f.cascade_bias[0] = ctx.texel_world / ctx.depth_range;
        f.cascade_bias[1] = ctx.texel_world * 1.5f;
    }
    for (std::size_t i = 0; i < ctx.chosen.size(); ++i) {
        put(f.lights + i * 8, to_space.apply(ctx.chosen[i].second->position), ctx.chosen[i].second->radius);
        const f32 size = std::max(ctx.chosen[i].second->size, 0.01f);
        put(f.lights + i * 8 + 4, ctx.chosen[i].second->color, size * size);
    }
    SDL_PushGPUFragmentUniformData(ctx.commands, 0, &f, sizeof(f));
}

void GpuRenderer3D::sky_environment(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    Render3DStats& stats = ctx.stats;
    const Sky3D& sky = scene.sky;
    stats.sky_env_generations = _sky_env_generations;
    if (!scene.lights.sky_lights_scene || sky.mode != SkyMode3D::Procedural || !_sky_env_shader || !_filter_pipeline || !_environment_sampler) {
        return;
    }
    // What the panorama is a function of: the sun and moon, the haze, the intensities and the colours. It is made again when
    // any of them has moved by more than the threshold, and not sooner than `light_interval` renders after the last time.
    AtmosphereUniforms atmosphere{};
    fill_atmosphere(atmosphere, sky, scene.lights.sun.direction);
    std::array<f32, 16> key{};
    std::size_t k = 0;
    for (const f32* part : {atmosphere.sun, atmosphere.moon}) {
        for (int i = 0; i < 3; ++i) {
            key[k++] = part[i];
        }
    }
    key[k++] = atmosphere.sun[3];
    key[k++] = atmosphere.shape[0];
    key[k++] = atmosphere.shape[2];
    key[k++] = atmosphere.shape[3];
    for (const f32* part : {atmosphere.ground, atmosphere.night}) {
        for (int i = 0; i < 3 && k < key.size(); ++i) {
            key[k++] = part[i];
        }
    }
    f32 moved = 0.0f;
    for (std::size_t i = 0; i < key.size(); ++i) {
        // Relative to a light of 1 and above: the sun's intensity of 3 moving by a hundredth is a third of a percent.
        moved = std::max(moved, std::abs(key[i] - _sky_key[i]) / std::max(1.0f, std::abs(_sky_key[i])));
    }
    const bool have = _sky_env && _sky_env->filtered;
    const bool changed = !have || moved > sky.light_threshold;
    if (have && (!changed || _sky_env_age < sky.light_interval)) {
        ++_sky_env_age;
        ctx.environment = _sky_env.get();
        ctx.sky_env_used = true;
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    try {
        if (!_sky_env) {
            _sky_env = allocate_environment(sky_env_width, sky_env_height);
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the sky's environment not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return;
    }
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUColorTargetInfo target{};
    target.texture = _sky_env->source.handle();
    target.load_op = SDL_GPU_LOADOP_DONT_CARE; // all of it is written
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.cycle = true;
    SDL_GPUGraphicsPipeline* p = fullscreen(_sky_env_shader.handle(), SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    if (!p) {
        return;
    }
    if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, nullptr)) {
        set_viewport(pass, sky_env_width, sky_env_height);
        SDL_BindGPUGraphicsPipeline(pass, p);
        SkyEnvUniforms u{};
        put(u.target, {static_cast<f32>(sky_env_width), static_cast<f32>(sky_env_height), 0.0f}, 0.0f);
        u.atmosphere = atmosphere;
        SDL_PushGPUFragmentUniformData(commands, 0, &u, sizeof(u));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
    SDL_GenerateMipmapsForGPUTexture(commands, _sky_env->source.handle());
    filter_environment(commands, *_sky_env);
    _sky_key = key;
    _sky_env_age = 0;
    ++_sky_env_generations;
    stats.sky_env_generations = _sky_env_generations;
    stats.sky_env_regenerated = true;
    stats.sky_env_cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    ctx.environment = _sky_env.get();
    ctx.sky_env_used = true;
}

// The sky triangle into `pass`, which is at the scene's viewport: mode 0 the colour, 1 the colour and the velocity (the main
// pass of temporal anti-aliasing with no pre-pass), 2 the velocity alone (into the pre-pass's, over its depth).
void GpuRenderer3D::draw_sky(FrameCtx& ctx, SDL_GPURenderPass* pass, u32 mode) {
    const GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    const Sky3D& sky = scene.sky;
    SDL_GPUGraphicsPipeline* p = mode == 2 ? sky_velocity_pipeline(_ao_depth_format)
                                           : sky_pipeline(ctx.taa ? hdr_format : view->_format, ctx.taa ? 1u : view->_samples, mode == 1);
    if (!p) {
        return;
    }
    const bool orthographic = scene.camera.projection == Projection3D::Orthographic;
    SkyUniforms u{};
    // Where each pixel looks: the jittered view-projection (the meshes are drawn with it, and the sky's samples must be
    // where theirs are, or the edge between them would crawl under temporal anti-aliasing), unprojected at the near plane.
    std::memcpy(u.inverse_view_projection, ctx.drawn[0].inverse().m, sizeof(u.inverse_view_projection));
    std::memcpy(u.view_projection, ctx.view_projections[0].m, sizeof(u.view_projection));
    std::memcpy(u.prior_view_projection, (ctx.taa_reset || !ctx.taa ? ctx.view_projections : view->_taa_prior)[0].m,
                sizeof(u.prior_view_projection));
    const bool panorama = sky.mode == SkyMode3D::Image && ctx.environment;
    put(u.target, {static_cast<f32>(view->_size.x), static_cast<f32>(view->_size.y), panorama ? 1.0f : 0.0f}, orthographic ? 1.0f : 0.0f);
    put(u.camera, scene.camera.position, 1.0f);
    put(u.forward, scene.camera.forward(), 0.0f);
    constexpr f32 degrees_to_radians = 3.14159265f / 180.0f;
    put(u.disc, {std::max(sky.sun_size, 0.01f) * 0.5f * degrees_to_radians, std::max(sky.moon_size, 0.01f) * 0.5f * degrees_to_radians, sky.sun_disc},
        sky.stars ? sky.star_brightness : 0.0f);
    const bool moon = sky.moon && sky.moon_light > 0.0f;
    put(u.moon_color, sky.moon_color * (sky.moon_light * moon_disc_gain), moon ? 1.0f : 0.0f);
    fill_atmosphere(u.atmosphere, sky, scene.lights.sun.direction);
    if (ctx.fog) {
        fill_fog(u.fog, scene.fog, orthographic);
    }
    if (mode != 2) {
        const SDL_GPUTextureSamplerBinding environment = ctx.environment && panorama
                                                             ? SDL_GPUTextureSamplerBinding{ctx.environment->specular.handle(), _environment_sampler}
                                                             : SDL_GPUTextureSamplerBinding{_owner->_white.handle(), _owner->_sampler_linear};
        SDL_BindGPUGraphicsPipeline(pass, p);
        SDL_BindGPUFragmentSamplers(pass, 0, &environment, 1);
        SDL_PushGPUFragmentUniformData(ctx.commands, 1, &u, sizeof(u));
    } else {
        SDL_BindGPUGraphicsPipeline(pass, p);
        SDL_PushGPUFragmentUniformData(ctx.commands, 0, &u, sizeof(u));
    }
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

void GpuRenderer3D::begin_commands(FrameCtx& ctx) {
    Render3DStats& stats = ctx.stats;
    ctx.environment = dynamic_cast<GpuEnvironment3D*>(ctx.scene.lights.environment.backend().get());
    if (ctx.environment && (!_filter_pipeline || !ctx.environment->specular)) {
        ctx.environment = nullptr;
    }

    // The 2D draws so far first; the passes record between them and the next.
    _owner->ensure_frame();
    _owner->flush_to_frame();
    ctx.commands = _owner->_frame->command_buffer();
    ctx.storage = _instances ? _instances->handle() : nullptr;
    ctx.joints = _joints ? _joints->handle() : nullptr;
    ctx.joint_offsets = _joint_offsets ? _joint_offsets->handle() : nullptr;
    SDL_GPUCommandBuffer* commands = ctx.commands;

    // Lights3D::sky_lights_scene: the environment is the sky's, made again when the sky has moved.
    sky_environment(ctx);
    ctx.sky_pass = ctx.sky && (ctx.scene.sky.mode == SkyMode3D::Procedural || ctx.environment != nullptr);

    // An environment used for the first time: its filtered copies, once.
    if (ctx.environment && !ctx.environment->filtered) {
        filter_environment(commands, *ctx.environment);
        ++stats.environment_prefilters;
    }

    // Without shadows the shader still samples a map: one that is all far.
    if (!ctx.shadows && _no_shadow && !_no_shadow_cleared) {
        const SDL_GPUDepthStencilTargetInfo clear = depth_target(_no_shadow.handle(), SDL_GPU_STOREOP_STORE);
        if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &clear)) {
            SDL_EndGPURenderPass(pass);
            _no_shadow_cleared = true;
        }
    }
}

void GpuRenderer3D::shadow_pass(FrameCtx& ctx) {
    Render3DStats& stats = ctx.stats;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* storage = ctx.storage;
    // The shadow maps: casters (seen or casting into view) from the sun, a
    // pass to each cascade's layer.
    stats.shadow_cascades = ctx.shadows ? ctx.cascades : 0;
    if (ctx.shadows && shadow_unchanged(ctx)) {
        stats.shadow_cached = true; // the maps hold what this render would draw into them
        return;
    }
    bool complete = ctx.shadows;
    SDL_GPUBuffer* shadow_storage[2] = {storage, _caster_lists && ctx.cascades > 1 ? _caster_lists->handle() : storage};
    SDL_GPUBuffer* cluster_index = cluster_index_buffer();
    for (u32 k = 0; ctx.shadows && k < ctx.cascades; ++k) {
        // Not cycled: the frames before are done with it in queue order, and
        // a frame rendering several views would otherwise be handed a new
        // map for each (64 MB at 4096: 7 ms a render, measured, instead of 2).
        const SDL_GPUDepthStencilTargetInfo depth = depth_target(_shadow_map.handle(), SDL_GPU_STOREOP_STORE, /*cycle=*/false, k);
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
        complete = complete && pass != nullptr;
        if (pass) {
            set_viewport(pass, _shadow_size, _shadow_size);
            DrawUniforms u{};
            std::memcpy(u.view_projection, (ctx.cascades > 1 ? ctx.plan.cascade[k].view_projection : ctx.light_vp).m, sizeof(u.view_projection));
            DrawBinder binder{pass};
            for (const DrawRef& ref : _refs) {
                const u32 total = ctx.cascades > 1 ? ref.cascade_count[k] : ref.out_count + ref.out_extra;
                if (!ref.caster || total == 0) {
                    continue;
                }
                const Material3D& caster = ref.draw->material;
                const bool cutout = caster.blend == Blend3D::Mask;
                if (ref.skinned && (!storage || !ctx.joints || !ctx.joint_offsets)) {
                    continue;
                }
                SDL_GPUGraphicsPipeline* p = pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, !caster.double_sided, Pass::Shadow, cutout, nullptr, false, ref.skinned);
                if (!p) {
                    continue;
                }
                // A skinned copy's shader also reads the frame's joint matrices and each copy's first.
                SDL_GPUBuffer* const skinned_storage[4] = {storage, shadow_storage[1], ctx.joints, ctx.joint_offsets};
                binder.bind_pipeline(p, ref.skinned ? skinned_storage : shadow_storage, ref.skinned ? 4 : 2);
                if (ref.clustered && !cluster_index) {
                    continue;
                }
                binder.bind_mesh(ref.mesh, ref.clustered ? cluster_index : nullptr);
                if (ref.skinned) {
                    binder.bind_skin(ref.mesh);
                }
                if (cutout) {
                    bind_cutout(commands, pass, caster);
                }
                // A clustered mesh: a draw for each copy, over its run of the frame's compacted indices (this map's).
                if (ref.clustered && _gc.active) {
                    draw_cluster_indirect(ref, commands, pass, u, ctx.cascades > 1 ? 1 + k : 1);
                    ++stats.shadow_draws;
                    stats.shadow_casters += total;
                    stats.cascade_casters[k] += total;
                    continue;
                }
                if (ref.clustered) {
                    const u32 view = ctx.cascades > 1 ? 1 + k : 1;
                    for (u32 n = 0; n < total; ++n) {
                        const u32 position = ctx.cascades > 1 ? ref.cascade_level_first[k][0] + n : 0;
                        const u32 slot = ctx.cascades > 1 ? _caster_list[position] : ref.level_first[0] + n;
                        const ClusterRun run = _cluster_runs[slot][view];
                        if (run.count == 0) {
                            continue;
                        }
                        u.first[0] = ctx.cascades > 1 ? 0 : slot;
                        u.first[1] = ctx.cascades > 1 ? position + 1 : 0;
                        SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                        SDL_DrawGPUIndexedPrimitives(pass, run.count, 1, run.first, 0, 0);
                        ++stats.shadow_draws;
                        ++stats.shadow_casters;
                        ++stats.cascade_casters[k];
                    }
                    continue;
                }
                // A draw for each level of detail the casters are at (the one level, without).
                for (u32 l = 0; l < ref.levels; ++l) {
                    const u32 count = ctx.cascades > 1 ? ref.cascade_level_count[k][l] : ref.level_seen[l] + ref.level_extra[l];
                    if (count == 0) {
                        continue;
                    }
                    u.first[0] = ref.level_first[l];
                    u.first[1] = ctx.cascades > 1 ? ref.cascade_level_first[k][l] + 1 : 0; // the list, or the run from level_first
                    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                    SDL_DrawGPUIndexedPrimitives(pass, ref.mesh->levels[l].index_count, count, ref.mesh->levels[l].first_index, 0, 0);
                    ++stats.shadow_draws;
                    stats.shadow_casters += count;
                    stats.cascade_casters[k] += count;
                }
            }
            SDL_EndGPURenderPass(pass);
        }
    }
    shadow_drawn(ctx, complete);
}

// The shadow cache (Lights3D::cache_shadows). The pass is a function of: the map, the cascades' views, the camera (it decides which
// casters reach the view, and the cascades' fit), the levels of detail, and each caster's mesh, material and copies. Everything but
// the copies is written into `key` (bytes, compared exactly); the copies, which are most of the data, are compared against a snapshot
// taken only after two renders agreed on the key and on a sample of them, so that a scene that moves costs a small key and the sample.
bool GpuRenderer3D::shadow_unchanged(FrameCtx& ctx) {
    ShadowCache& c = _shadow_cache;
    c.eligible = false;
    if (!ctx.scene.lights.cache_shadows || !_shadow_cache_allowed) {
        c.valid = false;
        c.prev_key.clear();
        return false;
    }
    std::vector<u8>& key = c.scratch;
    key.clear();
    const auto put = [&](const void* data, std::size_t bytes) {
        const auto* b = static_cast<const u8*>(data);
        key.insert(key.end(), b, b + bytes);
    };
    const auto value = [&](auto v) { put(&v, sizeof(v)); };
    const auto matrix = [&](const Mat4f& m) { put(m.m, sizeof(m.m)); };
    const auto frustum = [&](const GpuFrustum& f) {
        value(f.count);
        put(f.planes.data(), sizeof(Vec4f) * f.count);
    };
    const Lights3D& lights = ctx.scene.lights;
    const Lod3D& lod = ctx.scene.lod;
    value(_shadow_generation);
    value(_shadow_size);
    value(_shadow_layers);
    value(ctx.cascades);
    value(ctx.view->_size);
    value(ctx.scene.cull);
    value(ctx.gpu_clusters);
    value(_gc.active);
    value(lights.sun.direction);
    value(lights.shadow_area);
    value(lod.enabled);
    value(lod.error_pixels);
    value(lod.bias);
    value(lod.force_level);
    value(lod.gpu_culling);
    value(lod.max_cluster_triangles);
    const LodView& lv = ctx.lod[0];
    value(lv.on);
    value(lv.force);
    value(lv.orthographic);
    value(lv.pixels);
    value(lv.per_unit);
    value(lv.eye);
    value(lv.forward);
    matrix(ctx.view_projections[0]);
    frustum(ctx.frustums[0]);
    matrix(ctx.light_vp);
    frustum(ctx.light_frustum);
    if (ctx.cascades > 1) {
        for (u32 k = 0; k < ctx.cascades; ++k) {
            matrix(ctx.plan.cascade[k].view_projection);
            frustum(ctx.cascade_frustums[k]);
            frustum(ctx.slice_frustums[k]);
        }
    }
    u64 sample = 0x9E3779B97F4A7C15ull;
    const auto mix = [&](u64 w) { sample = ((sample ^ w) * 0xFF51AFD7ED558CCDull); sample ^= sample >> 29; };
    for (const DrawRef& ref : _refs) {
        if (!ref.caster) {
            continue;
        }
        const Material3D& m = ref.draw->material;
        const auto* texture = dynamic_cast<const GpuTextureBackend*>(m.texture.backend().get());
        if (m.blend == Blend3D::Mask && texture && texture->premultiplied()) {
            c.valid = false; // a render target: drawn into without a trace
            c.prev_key.clear();
            return false;
        }
        value(ref.mesh->serial); // a skinned mesh's skeleton is the mesh's (its skin, and so its joint count), so this is its identity
        value(ref.draw->count);
        value(ref.levels);
        value(ref.clustered);
        value(ref.skinned);
        value(ref.skinned ? ref.mesh->skin_bounds.size() : std::size_t{0});
        value(m.double_sided);
        value(m.blend);
        if (m.blend == Blend3D::Mask) {
            value(m.alpha_cutoff);
            value(m.wrap_u);
            value(m.wrap_v);
            value(texture);
            value(texture ? texture->version() : u64{0});
        }
        // A sample of the copies: up to eight, spread over the draw, and its ends.
        const u32 n = ref.draw->count;
        const u32 step = std::max(1u, n / 8);
        const auto mix_instance = [&](const MeshInstance& instance) {
            for (std::size_t w = 0; w < sizeof(MeshInstance) / 8; ++w) {
                u64 word;
                std::memcpy(&word, reinterpret_cast<const u8*>(&instance) + w * 8, 8);
                mix(word);
            }
        };
        // A posed copy's pose is in the sample as well (the matrices are what moves the shadow, the copy's own placing is the same).
        const auto mix_pose = [&](u32 i) {
            if (!ref.skinned) {
                return;
            }
            const std::span<const Mat4f> pose{ref.joints + ref.skin_first[i], ref.mesh->skin_bounds.size()};
            for (const Mat4f& m : pose) {
                for (std::size_t w = 0; w < sizeof(Mat4f) / 8; ++w) {
                    u64 word;
                    std::memcpy(&word, reinterpret_cast<const u8*>(&m) + w * 8, 8);
                    mix(word);
                }
            }
        };
        for (u32 i = 0; i < n; i += step) {
            mix_instance(ref.instances[i]);
            mix_pose(i);
        }
        mix_instance(ref.instances[n - 1]);
        mix_pose(n - 1);
    }
    static_assert(sizeof(MeshInstance) % 8 == 0, "the sample reads whole words of a MeshInstance");
    for (std::size_t i = 0; i + 8 <= key.size(); i += 8) {
        u64 w;
        std::memcpy(&w, key.data() + i, 8);
        mix(w);
    }
    mix(key.size());
    c.eligible = true;
    c.now = sample;
    if (c.valid && c.sample == sample && c.key == key) {
        // The copies, in full: stops at the first that differs.
        std::size_t at = 0;
        bool same = true;
        for (const DrawRef& ref : _refs) {
            if (!ref.caster) {
                continue;
            }
            const std::size_t bytes = static_cast<std::size_t>(ref.draw->count) * sizeof(MeshInstance);
            if (at + bytes > c.copies.size() || std::memcmp(ref.instances, c.copies.data() + at, bytes) != 0) {
                same = false;
                break;
            }
            at += bytes;
            // And each posed copy's matrices, in full: a pose that is not what the map was drawn from is a different shadow.
            if (ref.skinned) {
                const std::size_t pose_bytes = ref.mesh->skin_bounds.size() * sizeof(Mat4f);
                for (u32 i = 0; i < ref.draw->count && same; ++i) {
                    if (at + pose_bytes > c.copies.size() || std::memcmp(ref.joints + ref.skin_first[i], c.copies.data() + at, pose_bytes) != 0) {
                        same = false;
                    }
                    at += pose_bytes;
                }
                if (!same) {
                    break;
                }
            }
        }
        if (same && at == c.copies.size()) {
            return true;
        }
        c.valid = false;
        c.backoff = 8; // the sample agreed and the copies did not: a scene that moves a little; do not copy it every render
    }
    return false;
}

void GpuRenderer3D::shadow_drawn(FrameCtx& ctx, bool complete) {
    static_cast<void>(ctx);
    ShadowCache& c = _shadow_cache;
    if (!c.eligible || !complete) {
        c.valid = false;
        c.prev_key.clear();
        c.prev_sample = 0;
        return;
    }
    const bool still = !c.prev_key.empty() && c.prev_sample == c.now && c.prev_key == c.scratch;
    if (still && c.backoff == 0) {
        c.copies.clear();
        for (const DrawRef& ref : _refs) {
            if (ref.caster) {
                const auto* bytes = reinterpret_cast<const u8*>(ref.instances);
                c.copies.insert(c.copies.end(), bytes, bytes + static_cast<std::size_t>(ref.draw->count) * sizeof(MeshInstance));
                if (ref.skinned) {
                    const std::size_t pose_bytes = ref.mesh->skin_bounds.size() * sizeof(Mat4f);
                    for (u32 i = 0; i < ref.draw->count; ++i) {
                        const auto* pose = reinterpret_cast<const u8*>(ref.joints + ref.skin_first[i]);
                        c.copies.insert(c.copies.end(), pose, pose + pose_bytes);
                    }
                }
            }
        }
        c.key = c.scratch;
        c.sample = c.now;
        c.valid = true;
    } else {
        c.valid = false;
        c.backoff -= c.backoff > 0 ? 1u : 0u;
    }
    c.prev_key = c.scratch;
    c.prev_sample = c.now;
}

bool GpuRenderer3D::phase_skips(const DrawRef& ref) const {
    if (_phase == Phase::All) {
        return false;
    }
    const bool occluder = !dynamic_cast<const GpuMaterialShader3D*>(ref.draw->material.shader.backend().get());
    return _phase == Phase::First ? !occluder : occluder && !ref.clustered;
}

u32 GpuRenderer3D::phase_parts(const DrawRef& ref) const {
    if (_phase == Phase::All) {
        return cluster_both;
    }
    const bool occluder = !dynamic_cast<const GpuMaterialShader3D*>(ref.draw->material.shader.backend().get());
    return _phase == Phase::First ? cluster_first : occluder ? cluster_second : cluster_both;
}

// A clustered mesh's copies the camera sees, one draw each over its run of the frame's compacted indices (the
// camera's view); `stats`, when given, counts them as the pre-pass's. With Lod3D::occlusion the camera's clusters are in two
// index buffers (the first pass's and the second's): `phase_parts` says which this pass draws.
template <typename Binder, typename Uniforms>
void GpuRenderer3D::draw_cluster_copies(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass, Binder& binder, Uniforms& u,
                                        Render3DStats* stats) const {
    if (_gc.active) {
        for (u32 set = 0; set < (_gc.occlusion ? 2u : 1u); ++set) {
            if ((phase_parts(ref) & (1u << set)) == 0) {
                continue;
            }
            binder.bind_mesh(ref.mesh, cluster_index_buffer(set));
            draw_cluster_indirect(ref, commands, pass, u, 0, set);
            if (stats) {
                ++stats->ao_draws;
                stats->ao_instances += ref.level_seen[0];
            }
        }
        return;
    }
    for (u32 n = 0; n < ref.level_seen[0]; ++n) {
        const u32 slot = ref.level_first[0] + n;
        const ClusterRun run = _cluster_runs[slot][0];
        if (run.count == 0) {
            continue;
        }
        u.first[0] = slot;
        SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
        SDL_DrawGPUIndexedPrimitives(pass, run.count, 1, run.first, 0, 0);
        if (stats) {
            ++stats->ao_draws;
            ++stats->ao_instances;
        }
    }
}

// The velocity pre-pass's draws of one scene's opaque meshes (the world's with the pre-pass, the overlay's alone).
void GpuRenderer3D::draw_velocity(FrameCtx& ctx, SDL_GPURenderPass* target_pass, u32 which, SDL_GPUTextureFormat depth_format) {
    Render3DStats& stats = ctx.stats;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* storage = ctx.storage;
    SDL_GPUBuffer* previous_storage = ctx.previous_storage;
    const GpuView3D* view = ctx.view;
    if (!previous_storage) {
        return;
    }
    VelocityUniforms u{};
    std::memcpy(u.view_projection, ctx.drawn[which].m, sizeof(u.view_projection));
    std::memcpy(u.current, ctx.view_projections[which].m, sizeof(u.current));
    std::memcpy(u.prior, (ctx.taa_reset ? ctx.view_projections : view->_taa_prior)[which].m, sizeof(u.prior));
    // A skinned copy's shader also reads the joints (the frame's, each copy's first), and those of the frame before (and where each
    // copy's own start in them).
    const std::array<SDL_GPUBuffer*, 6> buffers{storage, previous_storage, ctx.joints, ctx.joint_offsets, ctx.previous_joints, ctx.previous_joint_offsets};
    SDL_GPUBuffer* cluster_index = cluster_index_buffer();
    DrawBinder binder{target_pass};
    for (const u32 index : _order) {
        const DrawRef& ref = _refs[index];
        if (ref.scene != which || pass_of(ref.draw->material) != Pass::Opaque || (which == 0 && phase_skips(ref))) {
            continue;
        }
        if (ref.skinned && !(ctx.joints && ctx.joint_offsets && ctx.previous_joints && ctx.previous_joint_offsets)) {
            continue;
        }
        const Material3D& occluder = ref.draw->material;
        const bool cutout = occluder.blend == Blend3D::Mask;
        SDL_GPUGraphicsPipeline* p = velocity_pipeline(depth_format, !occluder.double_sided, cutout, ref.skinned);
        if (!p) {
            continue;
        }
        binder.bind_pipeline(p, buffers.data(), ref.skinned ? 6 : 2);
        if (ref.clustered && !cluster_index) {
            continue;
        }
        binder.bind_mesh(ref.mesh, ref.clustered ? cluster_index : nullptr);
        if (ref.skinned) {
            binder.bind_skin(ref.mesh);
        }
        if (cutout) {
            bind_cutout(commands, target_pass, occluder);
        }
        if (ref.clustered) {
            draw_cluster_copies(ref, commands, target_pass, binder, u, which == 0 ? &stats : nullptr);
            continue;
        }
        for (u32 l = 0; l < ref.levels; ++l) {
            if (ref.level_seen[l] == 0) {
                continue;
            }
            u.first[0] = ref.level_first[l];
            SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
            SDL_DrawGPUIndexedPrimitives(target_pass, ref.mesh->levels[l].index_count, ref.level_seen[l], ref.mesh->levels[l].first_index, 0, 0);
            if (which == 0) {
                ++stats.ao_draws;
                stats.ao_instances += ref.level_seen[l];
            }
        }
    }
}

void GpuRenderer3D::prepass(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    Render3DStats& stats = ctx.stats;
    const Lights3D& lights = scene.lights;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* storage = ctx.storage;
    // Ambient occlusion and contact shadows: the opaque meshes' depth and
    // normals (one sample whatever the view's: the main pass, multisampled,
    // can't share the depth; drawn once for both), then GTAO and its blur, and
    // the march toward the sun and its blur, in compute. The same copies the
    // camera sees, from the same instance buffer. The contact shadows are
    // marched in compute into a texture rather than in mesh.frag: the main
    // pass shades every fragment of the overdraw, the march only each pixel
    // once, and mesh.frag stays one texture read.
    const AmbientOcclusion3D& ao = scene.ambient_occlusion;
    if (ao.enabled && !_ao_available) {
        warn_once(_ao_warned, "3D: ambient occlusion asked for, but this device cannot do it: off");
    }
    const ContactShadows3D& contact = scene.contact_shadows;
    const bool want_contact = contact.enabled && ctx.sun_shines;
    if (want_contact && !_ao_available) {
        warn_once(_contact_warned, "3D: contact shadows asked for, but this device cannot do it: off");
    }
    ctx.previous_storage = ctx.taa && _previous ? _previous->handle() : nullptr;
    ctx.previous_joints = ctx.taa && _previous_joints ? _previous_joints->handle() : nullptr;
    ctx.previous_joint_offsets = ctx.taa && _previous_joint_offsets ? _previous_joint_offsets->handle() : nullptr;
    ctx.velocity_drawn = false; // the pre-pass began, so _ao_normals holds this frame's velocity
    // Temporal anti-aliasing alone: the main pass writes the velocity, one geometry pass fewer. A custom shader's opaque
    // mesh can't (it has no velocity output), so a frame with one keeps the pre-pass.
    ctx.main_velocity = ctx.taa && _main_velocity_allowed && !ao.enabled && !want_contact && !ctx.occlusion && ctx.previous_storage &&
                        view->_ao_normals.format() == hdr_format &&
                        std::none_of(_refs.begin(), _refs.end(), [](const DrawRef& ref) {
                            return ref.draw->material.shader.backend() && pass_of(ref.draw->material) == Pass::Opaque;
                        });
    if (!ctx.main_velocity && (ao.enabled || want_contact || ctx.taa) && _ao_available && (storage || ctx.taa) &&
        ensure_ao_targets(*view, ao.half_resolution, ao.enabled, want_contact, ctx.taa)) {
        SDL_GPUColorTargetInfo target{};
        target.texture = view->_ao_normals.handle();
        target.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.cycle = true;
        // Reverse Z: the far plane, which the compute reads as sky.
        const SDL_GPUDepthStencilTargetInfo prepass_depth = depth_target(view->_ao_depth.handle(), SDL_GPU_STOREOP_STORE, /*cycle=*/true);
        // With Lod3D::occlusion the pre-pass is two passes: the opaque meshes and the first pass's clusters, then (once the
        // depth pyramid is built from that and the second pass has tested the rest against it) the second pass's clusters.
        const auto draw_all = [&](SDL_GPURenderPass* prepass, bool second) {
            set_viewport(prepass, view->_size.x, view->_size.y);
            DrawUniforms u{};
            std::memcpy(u.view_projection, ctx.drawn[0].m, sizeof(u.view_projection));
            SDL_GPUBuffer* cluster_index = cluster_index_buffer();
            DrawBinder binder{prepass};
            _phase = !ctx.occlusion ? Phase::All : second ? Phase::Second : Phase::First;
            if (ctx.taa) {
                draw_velocity(ctx, prepass, 0, _ao_depth_format);
            }
            for (const u32 index : _order) {
                const DrawRef& ref = _refs[index];
                if (ctx.taa || ref.scene != 0 || pass_of(ref.draw->material) != Pass::Opaque || phase_skips(ref)) {
                    continue;
                }
                if (ref.skinned && (!ctx.joints || !ctx.joint_offsets)) {
                    continue;
                }
                const Material3D& occluder = ref.draw->material;
                const bool cutout = occluder.blend == Blend3D::Mask;
                SDL_GPUGraphicsPipeline* p = pipeline(ao_normal_format, 1, !occluder.double_sided, Pass::Prepass, cutout, nullptr, false, ref.skinned);
                if (!p) {
                    continue;
                }
                const std::array<SDL_GPUBuffer*, 3> skinned_buffers{storage, ctx.joints, ctx.joint_offsets};
                binder.bind_pipeline(p, ref.skinned ? skinned_buffers.data() : &storage, ref.skinned ? 3 : 1);
                if (ref.clustered && !cluster_index) {
                    continue;
                }
                binder.bind_mesh(ref.mesh, ref.clustered ? cluster_index : nullptr);
                if (ref.skinned) {
                    binder.bind_skin(ref.mesh);
                }
                if (cutout) {
                    bind_cutout(commands, prepass, occluder);
                }
                if (ref.clustered) {
                    draw_cluster_copies(ref, commands, prepass, binder, u, &stats);
                    continue;
                }
                for (u32 l = 0; l < ref.levels; ++l) {
                    if (ref.level_seen[l] == 0) {
                        continue;
                    }
                    u.first[0] = ref.level_first[l];
                    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                    SDL_DrawGPUIndexedPrimitives(prepass, ref.mesh->levels[l].index_count, ref.level_seen[l], ref.mesh->levels[l].first_index, 0, 0);
                    ++stats.ao_draws;
                    stats.ao_instances += ref.level_seen[l];
                }
            }
            // The camera's turn over the pixels nothing was drawn on (the far depth), so that the resolve reprojects the sky.
            if (ctx.taa && ctx.sky_pass && (!ctx.occlusion || second)) {
                draw_sky(ctx, prepass, 2);
            }
            _phase = Phase::All;
        };
        if (SDL_GPURenderPass* prepass = SDL_BeginGPURenderPass(commands, &target, 1, &prepass_depth)) {
            ctx.velocity_drawn = true;
            ctx.prepass_ran = true;
            draw_all(prepass, /*second=*/false);
            SDL_EndGPURenderPass(prepass);
            if (ctx.occlusion) {
                build_depth_pyramid(ctx, view->_ao_depth);
                cluster_pass_gpu(ctx, 2);
                SDL_GPUColorTargetInfo keep = target;
                keep.load_op = SDL_GPU_LOADOP_LOAD;
                keep.cycle = false;
                SDL_GPUDepthStencilTargetInfo keep_depth = prepass_depth;
                keep_depth.load_op = SDL_GPU_LOADOP_LOAD;
                keep_depth.cycle = false;
                if (SDL_GPURenderPass* rest = SDL_BeginGPURenderPass(commands, &keep, 1, &keep_depth)) {
                    draw_all(rest, /*second=*/true);
                    SDL_EndGPURenderPass(rest);
                }
            }
            if (!ctx.taa) {
                ctx.main_projection = scene.camera.projection_matrix(ctx.aspect);
            }
            // Both of them, at one size (the occlusion not at half): one blur serves both.
            const bool merge = ao.enabled && want_contact && _ao_blur_both_pipeline && _merge_blurs &&
                               view->_ao_raw.width() == view->_contact_raw.width() && view->_ao_raw.height() == view->_contact_raw.height();
            _chain_ready = false;
            if (ao.enabled) {
                compute_occlusion(commands, *view, scene.camera, ctx.main_projection, ao, stats, !merge);
            }
            if (want_contact) {
                compute_contact_shadows(commands, *view, scene.camera, ctx.main_projection, contact, lights.sun.direction, stats, !merge);
            }
            if (merge) {
                blur_screen_space_both(commands, *view, ctx.main_projection, stats);
            }
            ctx.occluded = ao.enabled && stats.ao_dispatches > 0;
            ctx.contacted = want_contact && stats.contact_dispatches > 0;
        }
    }
}

void GpuRenderer3D::overlay_velocity(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* previous_storage = ctx.previous_storage;
    // The overlay's velocity (temporal anti-aliasing): drawn over the pre-pass's
    // normals and velocity, against a depth of its own (the main pass's, which it
    // clears after), so that the gun is not given the velocity of the world behind
    // it. The compute passes that read the pre-pass are recorded already.
    if (ctx.taa && !ctx.main_velocity && ctx.overlaid && previous_storage) {
        SDL_GPUColorTargetInfo target{};
        target.texture = view->_ao_normals.handle();
        target.load_op = SDL_GPU_LOADOP_LOAD;
        target.store_op = SDL_GPU_STOREOP_STORE;
        const SDL_GPUDepthStencilTargetInfo overlay_depth = depth_target(view->_taa_depth.handle(), SDL_GPU_STOREOP_DONT_CARE, /*cycle=*/true);
        if (SDL_GPURenderPass* gun = SDL_BeginGPURenderPass(commands, &target, 1, &overlay_depth)) {
            set_viewport(gun, view->_size.x, view->_size.y);
            draw_velocity(ctx, gun, 1, _depth_format);
            SDL_EndGPURenderPass(gun);
        }
    }
}

bool GpuRenderer3D::main_pass(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    Render3DStats& stats = ctx.stats;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* storage = ctx.storage;
    // The scene (and its overlay): into the multisampled colour, resolved into
    // what 2D draws, or into the float scene when HDR. With temporal
    // anti-aliasing: one sample, into the float picture the resolve reads.
    // An HDR view holds linear light whatever the 2D pipeline stores.
    SDL_FColor clear = _owner->stored_color(view->_options.background);
    if (view->_hdr || ctx.taa) {
        const LinearColor l = to_linear(view->_options.background);
        clear = {l.r, l.g, l.b, l.a};
    }
    clear = {clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a};
    SDL_GPUTexture* resolved = ctx.taa ? (view->_hdr ? view->_scene.handle() : view->_taa_in.handle())
                             : view->_hdr ? view->_scene.handle()
                                          : view->_resolved->texture().handle();
    SDL_GPUColorTargetInfo colour{};
    colour.clear_color = clear;
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.cycle = true; // a frame still reading the last picture keeps it
    if (view->_colour && !ctx.taa) {
        colour.texture = view->_colour.handle();
        colour.store_op = SDL_GPU_STOREOP_RESOLVE;
        colour.resolve_texture = resolved;
        colour.cycle_resolve_texture = true;
    } else {
        colour.texture = resolved;
        colour.store_op = SDL_GPU_STOREOP_STORE;
    }
    // Reverse Z: the far plane.
    const SDL_GPUDepthStencilTargetInfo depth = depth_target(ctx.taa ? view->_taa_depth.handle() : view->_depth.handle(), SDL_GPU_STOREOP_DONT_CARE,
                                                             /*cycle=*/true);
    // With the velocity written here: a second colour target, cleared to no motion as the pre-pass clears it.
    std::array<SDL_GPUColorTargetInfo, 2> targets{colour, SDL_GPUColorTargetInfo{}};
    if (ctx.main_velocity) {
        targets[1].texture = view->_ao_normals.handle();
        targets[1].clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
        targets[1].load_op = SDL_GPU_LOADOP_CLEAR;
        targets[1].store_op = SDL_GPU_STOREOP_STORE;
        targets[1].cycle = true;
    }
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, targets.data(), ctx.main_velocity ? 2 : 1, &depth);
    if (!pass) {
        resume_2d();
        return false;
    }
    ctx.velocity_drawn = ctx.velocity_drawn || ctx.main_velocity;
    // With an overlay, the scene's depths go in [0, 0.9] and the overlay's in
    // [0.9, 1]: nearer (reverse Z) than all of the scene, yet depth-tested
    // against itself, in one pass.
    // Fog (P3a): the scene's block, and for the overlay a block of none (a gun in the hand is not fogged). The block is in
    // slot 2, which a custom shader's parameters use too: it is pushed again before the first fogged draw after a custom one.
    std::array<FogBlockUniforms, 2> fog_blocks{};
    if (ctx.fog) {
        const bool orthographic = ctx.scene.camera.projection == Projection3D::Orthographic;
        fill_fog(fog_blocks[0].fog, ctx.scene.fog, orthographic);
        fill_atmosphere(fog_blocks[0].sky, ctx.scene.sky, ctx.scene.lights.sun.direction);
        stats.fog_applied = true;
    }
    bool fog_bound = false;
    const auto begin_scene = [&](u32 s) {
        const f32 lo = s == 0 ? 0.0f : overlay_split, hi = s == 0 && ctx.overlaid ? overlay_split : 1.0f;
        set_viewport(pass, view->_size.x, view->_size.y, lo, hi);
        push_frame_uniforms(ctx, s);
        fog_bound = false;
    };
    begin_scene(0);

    // What a draw pushes: the jittered view-projection, and with the velocity the two cameras it is measured between
    // (the pre-pass's, from the same frame: the overlay has its own).
    const auto fill_uniforms = [&](auto& u, u32 s) {
        std::memcpy(u.view_projection, ctx.drawn[s].m, sizeof(u.view_projection));
        if constexpr (std::is_same_v<std::decay_t<decltype(u)>, VelocityUniforms>) {
            std::memcpy(u.current, ctx.view_projections[s].m, sizeof(u.current));
            std::memcpy(u.prior, (ctx.taa_reset ? ctx.view_projections : view->_taa_prior)[s].m, sizeof(u.prior));
        }
    };
    const SDL_GPUTextureSamplerBinding environment_bindings[2] = {
        ctx.environment ? SDL_GPUTextureSamplerBinding{ctx.environment->specular.handle(), _environment_sampler}
                    : SDL_GPUTextureSamplerBinding{_owner->_white.handle(), _owner->_sampler_linear},
        ctx.environment ? SDL_GPUTextureSamplerBinding{ctx.environment->irradiance.handle(), _environment_sampler}
                    : SDL_GPUTextureSamplerBinding{_owner->_white.handle(), _owner->_sampler_linear}};
    // Smooth, so a half-size occlusion is read up; white (and never read) without.
    const SDL_GPUTextureSamplerBinding occlusion_binding{ctx.occluded ? view->_ao_raw.handle() : _owner->_white.handle(), _owner->_sampler_linear};
    const SDL_GPUTextureSamplerBinding contact_binding{ctx.contacted ? view->_contact_raw.handle() : _owner->_white.handle(), _owner->_sampler_linear};
    const SDL_GPUTextureSamplerBinding shadow_binding{ctx.shadows ? _shadow_map.handle() : _no_shadow.handle(), _compare_sampler};
    SDL_GPUBuffer* cluster_index = cluster_index_buffer();
    DrawBinder binder{pass};
    // The textures bound (the material's five), to bind again only when they change.
    using Textures = std::array<const ITextureBackend*, 5>;
    Textures unbound;
    unbound.fill(reinterpret_cast<const ITextureBackend*>(1)); // none yet
    Textures bound_textures = unbound;
    std::pair<Wrap3D, Wrap3D> bound_wrap{};
    u32 current_scene = 0;
    // The sky: after the scene's opaque meshes (their depth is in, so the triangle at the far plane shades only what is
    // left), before its blended and additive ones and the overlay. It binds a pipeline of its own: what the binder
    // and the texture cache remember is forgotten.
    bool sky_done = !ctx.sky_pass;
    const auto sky_now = [&] {
        sky_done = true;
        draw_sky(ctx, pass, ctx.main_velocity ? 1u : 0u);
        stats.sky_drawn = true;
        binder.reset();
        bound_textures = unbound;
    };
    const auto draw_all = [&](auto& draw_uniforms) {
    fill_uniforms(draw_uniforms, 0);
    const std::array<SDL_GPUBuffer*, 2> buffers{storage, ctx.previous_storage};
    for (const u32 index : _order) {
        const DrawRef& ref = _refs[index];
        const Material3D& material = ref.draw->material;
        if (!sky_done && (ref.scene != 0 || pass_of(material) != Pass::Opaque)) {
            sky_now();
        }
        if (ref.scene != current_scene) {
            current_scene = ref.scene;
            begin_scene(current_scene);
            fill_uniforms(draw_uniforms, current_scene);
        }
        const auto* custom = dynamic_cast<const GpuMaterialShader3D*>(material.shader.backend().get());
        if (ref.skinned && (!ctx.joints || !ctx.joint_offsets)) {
            continue;
        }
        SDL_GPUGraphicsPipeline* p = pipeline(ctx.taa ? hdr_format : view->_format, ctx.taa ? 1u : view->_samples, !material.double_sided, pass_of(material), false,
                                              custom ? custom->shader.handle() : nullptr, ctx.main_velocity, ref.skinned, ctx.fog && !custom);
        if (!p) {
            continue;
        }
        // The velocity variant also has last frame's matrices, and the joints of the frame before with where each copy's start.
        const bool skinned_velocity = ref.skinned && ctx.main_velocity && !custom;
        if (skinned_velocity && !(ctx.previous_joints && ctx.previous_joint_offsets)) {
            continue;
        }
        const std::array<SDL_GPUBuffer*, 6> skinned_buffers{storage, ctx.joints, ctx.joint_offsets, nullptr, nullptr, nullptr};
        const std::array<SDL_GPUBuffer*, 6> skinned_velocity_buffers{storage, ctx.previous_storage, ctx.joints, ctx.joint_offsets,
                                                                     ctx.previous_joints, ctx.previous_joint_offsets};
        const Textures textures{material.texture.backend().get(), material.metallic_roughness.backend().get(),
                                material.normal.backend().get(), material.occlusion.backend().get(),
                                material.emissive_texture.backend().get()};
        if (binder.bind_pipeline(p, skinned_velocity ? skinned_velocity_buffers.data() : ref.skinned ? skinned_buffers.data() : buffers.data(),
                                 skinned_velocity ? 6 : ref.skinned ? 3 : ctx.main_velocity && !custom ? 2 : 1)) {
            bound_textures = unbound;
        }
        const std::pair<Wrap3D, Wrap3D> wrap{material.wrap_u, material.wrap_v};
        if (textures != bound_textures || wrap != bound_wrap) {
            const std::array<SDL_GPUTextureSamplerBinding, 10> samplers{
                texture_binding(material.texture, material.wrap_u, material.wrap_v), shadow_binding,
                texture_binding(material.metallic_roughness, material.wrap_u, material.wrap_v),
                texture_binding(material.normal, material.wrap_u, material.wrap_v),
                texture_binding(material.occlusion, material.wrap_u, material.wrap_v),
                texture_binding(material.emissive_texture, material.wrap_u, material.wrap_v), environment_bindings[0],
                environment_bindings[1], occlusion_binding, contact_binding};
            SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), static_cast<u32>(samplers.size()));
            bound_textures = textures;
            bound_wrap = wrap;
        }
        if (ref.clustered && !cluster_index) {
            continue;
        }
        binder.bind_mesh(ref.mesh, ref.clustered ? cluster_index : nullptr);
        if (ref.skinned) {
            binder.bind_skin(ref.mesh);
        }
        const MaterialUniforms m{{material.emissive, material.unlit || material.wireframe ? 1.0f : 0.0f,
                                  material.receives_shadow ? 1.0f : 0.0f,
                                  material.blend == Blend3D::Mask ? std::max(material.alpha_cutoff, 1e-6f) : 0.0f},
                                 {material.metallic, material.roughness, material.normal_scale, material.occlusion_strength},
                                 {material.normal ? 1.0f : 0.0f, material.emissive_texture ? 1.0f : 0.0f,
                                  pass_of(material) == Pass::Opaque ? 1.0f : 0.0f, pass_of(material) == Pass::Additive ? 1.0f : 0.0f}};
        SDL_PushGPUFragmentUniformData(commands, 1, &m, sizeof(m));
        if (custom) {
            SDL_PushGPUFragmentUniformData(commands, 2, material.shader_params.data(), sizeof(CustomUniforms));
            fog_bound = false;
        } else if (ctx.fog && !fog_bound) {
            SDL_PushGPUFragmentUniformData(commands, 2, &fog_blocks[current_scene], sizeof(FogBlockUniforms));
            fog_bound = true;
        }
        // A clustered mesh: a draw for each copy, over its run of the frame's compacted indices.
        if (ref.clustered && _gc.active) {
            // With Lod3D::occlusion the camera's clusters are in two index buffers: those drawn before the pyramid, and the rest.
            for (u32 set = 0; set < (_gc.occlusion ? 2u : 1u); ++set) {
                binder.bind_mesh(ref.mesh, cluster_index_buffer(set));
                draw_cluster_indirect(ref, commands, pass, draw_uniforms, 0, set);
                ++stats.draws;
            }
            stats.instances += ref.level_seen[0];
            continue;
        }
        if (ref.clustered) {
            for (u32 n = 0; n < ref.level_seen[0]; ++n) {
                const u32 slot = ref.level_first[0] + n;
                const ClusterRun run = _cluster_runs[slot][0];
                if (run.count == 0) {
                    continue;
                }
                draw_uniforms.first[0] = slot;
                SDL_PushGPUVertexUniformData(commands, 0, &draw_uniforms, sizeof(draw_uniforms));
                SDL_DrawGPUIndexedPrimitives(pass, run.count, 1, run.first, 0, 0);
                ++stats.draws;
                ++stats.instances;
                stats.triangles += run.count / 3;
            }
            continue;
        }
        // A draw for each level of detail the copies are at (the one, without).
        for (u32 l = 0; l < ref.levels; ++l) {
            const u32 count = ref.level_seen[l];
            if (count == 0) {
                continue;
            }
            const GpuMesh3D::Level& level = ref.mesh->levels[l];
            draw_uniforms.first[0] = ref.level_first[l];
            SDL_PushGPUVertexUniformData(commands, 0, &draw_uniforms, sizeof(draw_uniforms));
            SDL_DrawGPUIndexedPrimitives(pass, level.index_count, count, level.first_index, 0, 0);
            ++stats.draws;
            stats.skinned_draws += ref.skinned ? 1u : 0u;
            stats.instances += count;
            stats.triangles += static_cast<u64>(level.index_count / 3) * count;
        }
    }
    };
    if (ctx.main_velocity) {
        VelocityUniforms velocity_uniforms{};
        draw_all(velocity_uniforms);
    } else {
        DrawUniforms draw_uniforms{};
        draw_all(draw_uniforms);
    }
    if (!sky_done) {
        sky_now(); // nothing but opaque meshes (or none) in the scene
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

void GpuRenderer3D::resolve_history(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    Render3DStats& stats = ctx.stats;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    const TemporalAA3D& taa_settings = ctx.scene.taa;
    ctx.shown = nullptr;
    // The resolve: this frame into the history, before bloom and the tonemap,
    // which see the finished picture. The next frame finds the cameras and the
    // copies as they are now.
    if (ctx.taa) {
        // Without this frame's velocity (the pre-pass would not begin) the resolve must not trust the texture, nor
        // the next frame the history: it is a reset, and the stale velocity stays where it is.
        if (!ctx.velocity_drawn) {
            warn_once(_taa_prepass_warned, "3D: temporal anti-aliasing: the velocity pass could not begin, so this frame has no history");
        }
        ctx.shown = resolve_taa(commands, *view, view->_hdr ? view->_scene : view->_taa_in, taa_settings, ctx.taa_reset || !ctx.velocity_drawn, ctx.jitter,
                            stats);
        view->_taa_valid = ctx.velocity_drawn;
        for (u32 s = 0; s < 2; ++s) {
            if (ctx.scenes[s]) {
                view->_taa_prior[s] = ctx.view_projections[s];
            }
        }
        std::swap(view->_taa_last, view->_taa_now);
        if (ctx.velocity_drawn) {
            std::swap(view->_ao_normals, view->_taa_motion); // this frame's velocity is the next one's past
        }
    }
}

void GpuRenderer3D::pick_pass(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* storage = ctx.storage;
    // What is under each pixel: the scene's opaque and alpha meshes (not its
    // wireframes, glows or overlay) again, writing position and id.
    if (view->_pick) {
        SDL_GPUColorTargetInfo target{};
        target.texture = view->_pick.handle();
        target.clear_color = {0.0f, 0.0f, 0.0f, 0.0f}; // id 0: nothing
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.cycle = true;
        const SDL_GPUDepthStencilTargetInfo pick_depth = depth_target(view->_pick_depth.handle(), SDL_GPU_STOREOP_DONT_CARE, /*cycle=*/true);
        if (SDL_GPURenderPass* picking = SDL_BeginGPURenderPass(commands, &target, 1, &pick_depth)) {
            set_viewport(picking, view->_size.x, view->_size.y);
            DrawUniforms u{};
            std::memcpy(u.view_projection, ctx.view_projections[0].m, sizeof(u.view_projection));
            SDL_GPUBuffer* cluster_index = cluster_index_buffer();
            DrawBinder binder{picking};
            for (const u32 index : _order) {
                const DrawRef& ref = _refs[index];
                const Pass kind = pass_of(ref.draw->material);
                if (ref.scene != 0 || (kind != Pass::Opaque && kind != Pass::Alpha)) {
                    continue;
                }
                if (ref.skinned && (!ctx.joints || !ctx.joint_offsets)) {
                    continue;
                }
                const Material3D& picked = ref.draw->material;
                const bool cutout = picked.blend == Blend3D::Mask;
                SDL_GPUGraphicsPipeline* p = pipeline(pick_format, 1, !picked.double_sided, Pass::Pick, cutout, nullptr, false, ref.skinned);
                if (!p) {
                    continue;
                }
                const std::array<SDL_GPUBuffer*, 3> skinned_buffers{storage, ctx.joints, ctx.joint_offsets};
                binder.bind_pipeline(p, ref.skinned ? skinned_buffers.data() : &storage, ref.skinned ? 3 : 1);
                if (ref.clustered && !cluster_index) {
                    continue;
                }
                binder.bind_mesh(ref.mesh, ref.clustered ? cluster_index : nullptr);
                if (ref.skinned) {
                    binder.bind_skin(ref.mesh);
                }
                if (cutout) {
                    bind_cutout(commands, picking, picked);
                }
                if (ref.clustered) {
                    draw_cluster_copies(ref, commands, picking, binder, u, nullptr);
                    continue;
                }
                for (u32 l = 0; l < ref.levels; ++l) {
                    if (ref.level_seen[l] == 0) {
                        continue;
                    }
                    u.first[0] = ref.level_first[l];
                    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                    SDL_DrawGPUIndexedPrimitives(picking, ref.mesh->levels[l].index_count, ref.level_seen[l], ref.mesh->levels[l].first_index, 0, 0);
                }
            }
            SDL_EndGPURenderPass(picking);
        }
    }
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
    const std::size_t pipelines_before = _owner->_pipelines.size();
    FrameCtx ctx{view, scene, stats};
    setup_frame(ctx);
    fit_sun_shadow(ctx);
    cull_and_upload(ctx);
    order_draws(ctx);
    choose_point_lights(ctx);
    begin_commands(ctx);
    cluster_pass_gpu(ctx, ctx.occlusion ? 1 : 0);
    shadow_pass(ctx);
    prepass(ctx);
    stats.taa_main_velocity = ctx.main_velocity;
    if (ctx.occlusion && !ctx.occlusion_second) {
        occluder_pass(ctx); // no pre-pass made the depth: a pass of its own
    }
    overlay_velocity(ctx);
    if (!main_pass(ctx)) {
        return false;
    }
    resolve_history(ctx);
    pick_pass(ctx);
    if (ctx.taa) {
        finish_view(ctx.commands, *view, *ctx.shown, /*plain=*/!view->_hdr, ctx.sky_pass);
    } else if (view->_hdr) {
        finish_view(ctx.commands, *view, view->_scene, /*plain=*/false, ctx.sky_pass);
    }
    age_targets(ctx);
    stats.pipelines_made = static_cast<u32>(_owner->_pipelines.size() - pipelines_before);
    resume_2d();
    if (ctx.occlusion && ctx.occlusion_second) {
        view->_occ_read ^= 1; // what the second pass wrote is what the next render's first pass reads
        stats.occlusion = true;
    }
    if (ctx.gpu_clusters && scene.lod.stats_readback) {
        read_cluster_stats(ctx);
    }
    return true;
}

void GpuRenderer3D::read_switches() {
        // Read each render, so that a test can switch them.
        const char* release = std::getenv("KIN_RELEASE_FRAMES");
        _release_after = release ? static_cast<u32>(std::max(0, std::atoi(release))) : 120u;
        const char* no_cache = std::getenv("KIN_SHADOW_CACHE");
        _shadow_cache_allowed = !(no_cache && no_cache[0] == '0');
        // The occlusion over the linear depth chain: by default only where the view is big enough for the chain to pay (its reads
        // miss the cache far more at 1080p and up); KIN_AO_CLASSIC=1 never uses it, KIN_AO_CHAIN=1 always does.
        const char* classic = std::getenv("KIN_AO_CLASSIC");
        const char* chain = std::getenv("KIN_AO_CHAIN");
        const char* contact_classic = std::getenv("KIN_CONTACT_CLASSIC");
        _contact_classic = contact_classic && contact_classic[0] == '1';
        _ao_chain_mode = classic && classic[0] == '1' ? 1 : chain && chain[0] == '1' ? 2 : 0;
        const char* separate = std::getenv("KIN_BLUR_SEPARATE");
        _merge_blurs = !(separate && separate[0] == '1');
        const char* prepass = std::getenv("KIN_TAA_PREPASS");
        _main_velocity_allowed = !(prepass && prepass[0] == '1');
}

bool GpuRenderer3D::warm_up(IView3DBackend& base, const Scene3D& scene) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view || !_vertex_shader || !_fragment_shader) {
        return false;
    }
    // What setup_frame, prepass and the passes would ask for on a first render of `scene`, asked for now (pipeline() and the ensure_ functions make
    // what is missing and keep it).
    read_switches();
    if (view->_output_format != _owner->_target_format && !make_targets(*view)) {
        return false;
    }
    const Lights3D& lights = scene.lights;
    const bool sun_shines = lights.sun.color.x + lights.sun.color.y + lights.sun.color.z > 0.0f;
    const Bounds3& area = lights.shadow_area;
    const Vec3f area_size = area.size();
    const bool shadows = sun_shines && area_size.x > 0 && area_size.y >= 0 && area_size.z > 0 && length_squared(lights.sun.direction) > 0;
    const bool ao = scene.ambient_occlusion.enabled && _ao_available;
    const bool contact = scene.contact_shadows.enabled && sun_shines && _ao_available;
    const bool occlusion = scene.lod.enabled && scene.lod.gpu_culling && scene.lod.occlusion.enabled && _occlusion_available && _gpu_clusters;
    const std::array<const Scene3D*, 2> scenes{&scene, scene.overlay_if_any()};
    const auto custom_of = [](const Material3D& m) { return dynamic_cast<const GpuMaterialShader3D*>(m.shader.backend().get()); };
    bool custom_opaque = false;
    for (const Scene3D* s : scenes) {
        if (!s) {
            continue;
        }
        for (const Scene3D::Draw& draw : s->draws()) {
            custom_opaque = custom_opaque || (custom_of(draw.material) && pass_of(draw.material) == Pass::Opaque);
        }
    }
    bool taa = scene.taa.enabled && _taa_available;
    const bool needs_prepass = ao || contact || scene.lod.occlusion.enabled || !_main_velocity_allowed;
    if (taa) {
        taa = ensure_taa_targets(*view, scene.taa.sharpness > 0.0f) &&
              (needs_prepass || custom_opaque ? ensure_ao_targets(*view, scene.ambient_occlusion.half_resolution, ao, contact, /*velocity=*/true)
                                              : ensure_velocity_target(*view));
    } else if (ao || contact) {
        ensure_ao_targets(*view, scene.ambient_occlusion.half_resolution, ao, contact, /*velocity=*/false);
    }
    if (shadows) {
        ensure_shadow_map(lights.shadow_resolution, lights.shadow_cascades);
    }
    const bool main_velocity = taa && !needs_prepass && !custom_opaque;
    if (occlusion) {
        std::array<std::pair<u32, u32>, GpuView3D::max_hiz_levels> size{};
        u32 levels = 0;
        ensure_pyramid(*view, static_cast<u32>(view->_size.x), static_cast<u32>(view->_size.y), size, levels);
        if (!ao && !contact && !taa) {
            ensure_occluder_depth(*view); // no pre-pass makes the depth: a pass of its own
        }
        for (const bool cull : {true, false}) {
            for (const bool mask : {false, true}) {
                pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, cull, Pass::Occluder, mask);
            }
        }
        for (const Scene3D* s : scenes) {
            if (s && std::any_of(s->draws().begin(), s->draws().end(), [](const Scene3D::Draw& d) { return d.skinned; })) {
                for (const bool cull : {true, false}) {
                    for (const bool mask : {false, true}) {
                        pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, cull, Pass::Occluder, mask, nullptr, false, true);
                    }
                }
                break;
            }
        }
    }
    // The draws' own pipelines, for each kind of draw the scene has.
    const SDL_GPUTextureFormat format = taa ? hdr_format : view->_format;
    const u32 samples = taa ? 1u : view->_samples;
    const Fog3D& fog = scene.fog;
    const bool fog_on = fog.enabled && (fog.density > 0.0f || fog.height_density > 0.0f) && fog.max_opacity > 0.0f && _fog_available;
    if (scene.sky.enabled && _sky_available) {
        if ((view->_hdr || taa) && _output_dither_shader) {
            fullscreen(_output_dither_shader.handle(), view->_output_format);
        }
        sky_pipeline(format, samples, main_velocity);
        if (taa && !main_velocity) {
            sky_velocity_pipeline(_ao_depth_format);
        }
    }
    if (scene.lights.sky_lights_scene && _sky_env_shader) {
        fullscreen(_sky_env_shader.handle(), SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    }
    for (u32 index = 0; index < 2; ++index) {
        if (!scenes[index]) {
            continue;
        }
        for (const Scene3D::Draw& draw : scenes[index]->draws()) {
            if (draw.count == 0 || !dynamic_cast<const GpuMesh3D*>(draw.mesh.backend().get())) {
                continue;
            }
            const Material3D& m = draw.material;
            const GpuMaterialShader3D* custom = custom_of(m);
            pipeline(format, samples, !m.double_sided, pass_of(m), false, custom ? custom->shader.handle() : nullptr, main_velocity, draw.skinned);
            if (fog_on && !custom) {
                pipeline(format, samples, !m.double_sided, pass_of(m), false, nullptr, main_velocity, draw.skinned, /*fog=*/true);
            }
            const bool opaque = pass_of(m) == Pass::Opaque;
            const bool cutout = m.blend == Blend3D::Mask;
            if (index == 0 && shadows && opaque && m.casts_shadow) {
                pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, !m.double_sided, Pass::Shadow, cutout, nullptr, false, draw.skinned);
            }
            if (index == 0 && view->_pick && (opaque || m.blend == Blend3D::Alpha)) {
                pipeline(pick_format, 1, !m.double_sided, Pass::Pick, cutout, nullptr, false, draw.skinned);
            }
            if (draw.skinned && opaque && !custom && (ao || contact || taa) && _ao_available) {
                // The pre-pass's skinned variants: with the velocity when temporal anti-aliasing has a pre-pass, plain else.
                if (taa) {
                    if (!main_velocity) {
                        velocity_pipeline(_ao_depth_format, !m.double_sided, cutout, true);
                        if (index == 1) {
                            velocity_pipeline(_depth_format, !m.double_sided, cutout, true);
                        }
                    }
                } else if (index == 0) {
                    pipeline(ao_normal_format, 1, !m.double_sided, Pass::Prepass, cutout, nullptr, false, true);
                }
            }
        }
    }
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
