#include "gpu_renderer3d.hpp"

#include "embedded_gpu_shader.hpp"
#include "gpu_device.hpp"
#include "gpu_renderer2d_backend.hpp"
#include "gpu_shadow_cull.hpp"
#include "gpu_shadow_reach.hpp"
#include "gpu_sphere_cull4.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <kin/core/jobs.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/shadow_cascades3d.hpp>
#include <kin/render3d/taa3d.hpp>
#include <kin/renderer/color.hpp>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace kin::gpu {

namespace {

// Fingerprint the inputs already packed for the GPU. Reject transient history
// while those inputs or their opaque backdrop change; a paused effect retains
// ordinary temporal smoothing. All callers supply initialized scalar data.
void taa_hash(u64& key, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    while (size >= sizeof(u64)) {
        u64 word;
        std::memcpy(&word, bytes, sizeof(word));
        key = (key ^ word) * 1099511628211ull;
        bytes += sizeof(word);
        size -= sizeof(word);
    }
    while (size--)
        key = (key ^ *bytes++) * 1099511628211ull;
}
template <class T> void taa_hash(u64& key, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    taa_hash(key, &value, sizeof(value));
}
bool taa_texture_hash(u64& key, const ITextureBackend* texture) {
    const auto* backend = dynamic_cast<const GpuTextureBackend*>(texture);
    taa_hash(key, backend);
    if (!backend)
        return false;
    taa_hash(key, backend->version());
    taa_hash(key, backend->scale_mode());
    // A render target can be drawn into without a version bump.
    return backend->premultiplied();
}

// Transparent motion and appearance use the same material partition. Reordering
// anonymous draws with different appearances cannot swap their histories.
bool taa_material_hash(u64& key, const Material3D& material, const ITextureBackend* texture) {
    taa_hash(key, material.color);
    taa_hash(key, material.blend);
    taa_hash(key, material.shader_params);
    taa_hash(key, material.wrap_u);
    taa_hash(key, material.wrap_v);
    const auto shader = material.shader.backend().get();
    taa_hash(key, shader);
    return taa_texture_hash(key, texture);
}

// Cache payloads intentionally retain their native scalar representation and
// field order. This writer adds no headers, padding, hashing or normalization.
class CacheBytes {
public:
    explicit CacheBytes(std::vector<u8>& bytes) : _bytes(bytes) {}
    void append(const void* data, std::size_t size) {
        const auto* begin = static_cast<const u8*>(data);
        _bytes.insert(_bytes.end(), begin, begin + size);
    }
    template <class T> void value(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        append(&value, sizeof(value));
    }
    void matrix(const Mat4f& m) { append(m.m, sizeof(m.m)); }
    void frustum(const GpuFrustum& f) {
        value(f.count);
        append(f.planes.data(), sizeof(Vec4f) * f.count);
    }

private:
    std::vector<u8>& _bytes;
};

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
    return samples >= 8   ? SDL_GPU_SAMPLECOUNT_8
           : samples >= 4 ? SDL_GPU_SAMPLECOUNT_4
           : samples >= 2 ? SDL_GPU_SAMPLECOUNT_2
                          : SDL_GPU_SAMPLECOUNT_1;
}

constexpr SDL_GPUTextureFormat hdr_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
constexpr SDL_GPUTextureFormat pick_format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT; // position, id
// Ambient occlusion: the pre-pass's world normals (octahedral, two floats), and the result.
constexpr SDL_GPUTextureFormat ao_normal_format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
constexpr SDL_GPUTextureFormat ao_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
constexpr u64 ao_chain_min_pixels =
    1'500'000; // the linear depth chain pays from about 1080p up (1280 x 720 is 0.92 M, 1920 x 1080 2.07 M)
constexpr u32 ao_chain_levels = 5;                                           // the occlusion's linear depth chain
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
constexpr u8 cascade_bit(u32 k) {
    return static_cast<u8>(1u << (first_cascade_bit + k));
}
constexpr bool in_cascade(u8 visible, u32 k) {
    return (visible & cascade_bit(k)) != 0;
}
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
    f32 flags[4];                                // encode to sRGB, decode textures, shadows, point lights
    f32 eye[4];                                  // the camera, in the scene's world space
    f32 environment[4];                          // intensity, the specular's last level, 0, 1 with an environment
    f32 environment_matrix[16];                  // the scene's space to the environment's
    f32 shadow_matrix[max_shadow_cascades * 16]; // a cascade each (one map: the first)
    f32 shadow[4];                               // cascades, 0, a texel (uv)
    f32 cascade_bias[max_shadow_cascades * 4];   // per cascade: depth bias, normal offset
    f32 cascade_end[max_shadow_cascades];        // the view depth where each cascade ends,
    f32 cascade_blend[max_shadow_cascades];      // and where it starts to fade into the next
    f32 cascade_forward[4];                      // the camera's forward
    f32 occlusion[4];                            // 1 with ambient occlusion, 0, 1 / the view's size
    f32 lights[max_lights * 2 * 4];
    f32 gi[4]; // x: 1 when global illumination's probes light the scene (gi_resolve.comp read their light on the view)
};
static_assert(sizeof(FrameUniforms) == 1136);
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
    f32 sizes[4];        // the result's size, the depth's
    f32 shape[4];        // radius, power, pixels a unit spans at depth 1, 1 if orthographic
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
    f32 depth[4];  // x: 1 when the main pass's depth is bound (the nearest surface's velocity is taken at edges);
                   // y: where the overlay's slice of it starts (2: no overlay)
};
static_assert(sizeof(TaaUniforms) == 64);
// shadow_mask.frag's and pick_mask.frag's.
struct CutoffUniforms {
    f32 cutoff[4]; // the alpha below which nothing is drawn
};
static_assert(sizeof(CutoffUniforms) == 16);
// The sky's and the fog's (kin_sky.inc.glsl's KinSky and kin_fog.inc.glsl's KinFog, vec4s all, so no padding to get
// wrong).
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
    f32 table[sky_model::FogTable3D::rows * 16]; // the sky's light for colour_from_sky (sky_model::FogTable3D's rows)
};
static_assert(sizeof(FogBlockUniforms) == 144 + 3072);
static_assert(sizeof(FogBlockUniforms) <= 4096, "SDL_GPU binds 4 KiB of a pushed uniform block");
// The legacy custom parameter prefix remains at slot 2. Fog follows it;
// local-light data stays in slot 3, keeping every pushed block below 4 KiB.
struct CustomFogUniforms {
    CustomUniforms custom;
    FogBlockUniforms fog;
};
static_assert(sizeof(CustomFogUniforms) == 3280 && sizeof(CustomFogUniforms) <= 4096);
// sky.frag's slot 1 (slot 0 of sky_velocity_only.frag).
struct SkyUniforms {
    f32 inverse_view_projection[16];
    f32 view_projection[16];
    f32 prior_view_projection[16];
    f32 target[4]; // the picture's size, 1 to show the panorama, 1 when orthographic
    f32 camera[4];
    f32 forward[4];
    f32 disc[4]; // the sun's angular radius, the moon's, the sun disc's radiance over its light, the stars' brightness
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
// mesh_light.frag's block (kin_material.inc.glsl's LocalLights), pushed in slot 2 (slot 3 with the fog): all vec4s and
// mat4s, std140 with no padding to get wrong.
struct LocalLightUniforms {
    f32 counts[4]; // spot lights, the moon's cascades, the moon's strength, the sun's
    f32 local[4];  // 1 / the local array's side
    f32 moon[4];   // a texel of the moon's map (uv)
    f32 moon_matrix[max_shadow_cascades * 16];
    f32 moon_bias[max_shadow_cascades * 4]; // per cascade: depth bias, normal offset
    f32 moon_end[4];
    f32 moon_blend[4];
    f32 spots[max_spot_lights * 16];
    f32 point_shadow[max_lights * 4];
    f32 local_matrix[max_local_shadow_layers * 16];
    f32 local_a[max_local_shadow_layers * 4];
    f32 local_b[max_local_shadow_layers * 4];
};
static_assert(sizeof(LocalLightUniforms) == 3600);
// P3c. A sprite in the storage buffer (particle.vert's Particle), and the draw's two blocks (particle.vert's
// ParticleDraw in vertex slot 0, kin_particle_fragment.inc.glsl's ParticleFrag in fragment slot 0): vec4s and mat4s
// all, so no padding to get wrong.
struct GpuParticle3D {
    f32 position_size[4]; // the centre, the width
    f32 colour[4];        // linear, straight alpha
    f32 axis_rotation[4]; // the velocity (Stretched), the rotation
    f32 frame[4];         // the flipbook frame
};
static_assert(sizeof(GpuParticle3D) == 64);
GpuParticle3D pack_particle(const ParticleSprite3D& p) {
    return {{p.position.x, p.position.y, p.position.z, p.size},
            {p.colour.r, p.colour.g, p.colour.b, p.alpha},
            {p.velocity.x, p.velocity.y, p.velocity.z, p.rotation},
            {p.frame, 0, 0, 0}};
}

struct ParticleDrawUniforms {
    f32 view_projection[16];
    f32 right[4];
    f32 up[4];
    f32 eye[4]; // w: 1 orthographic
    f32 forward[4];
    f32 axis[4];    // w: the facing
    f32 atlas[4];   // columns, rows, frames, 1 to mix two frames
    f32 stretch[4]; // seconds, least length
    u32 first[4];   // the batch's first sprite, 1 when fogged
    f32 sun_direction[4];
    f32 sun_light[4];
    FogParamUniforms fog;
    AtmosphereUniforms sky;
};
static_assert(sizeof(ParticleDrawUniforms) == 368);
struct ParticleFragUniforms {
    f32 tint[4];
    f32 flags[4];     // encode, decode the texture, the blend, 1 with a texture
    f32 disc[4];      // the disc's softness, its ring, the soft fade's distance
    f32 unproject[4]; // the inverse projection's e f g h
};
static_assert(sizeof(ParticleFragUniforms) == 64);
struct ParticleLightUniforms {
    f32 counts[4];
    f32 points[max_lights * 8];
    f32 spots[max_spot_lights * 16];
};
static_assert(sizeof(ParticleLightUniforms) == 1552);
// The most sprites a render draws (64 bytes each in video memory); the rest are dropped, said once.
constexpr u32 max_particles_a_frame = 1u << 20;
constexpr u32 min_particle_buffer = 64u * 1024u;
// How much brighter than the sky's light the moon's disc is drawn: the moon's light is a sky-scale one (0.1 at full
// moon).
constexpr f32 moon_disc_gain = 20.0f;
// The sky's panorama for image-based light: its size (2:1), as the environment filter wants it.
constexpr u32 sky_env_width = 256, sky_env_height = 128;

// A view's six bounding planes from its view-projection matrix, reverse Z:
// near at depth 1, far at 0. An infinite far plane has no normal.
GpuFrustum frustum_of(const Mat4f& m) {
    const auto row = [&](int r) { return Vec4f{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)}; };
    const Vec4f r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const auto add = [](Vec4f a, Vec4f b, f32 k) {
        return Vec4f{a.x + k * b.x, a.y + k * b.y, a.z + k * b.z, a.w + k * b.w};
    };
    GpuFrustum f;
    for (const Vec4f p :
         {add(r3, r0, 1.0f), add(r3, r0, -1.0f), add(r3, r1, 1.0f), add(r3, r1, -1.0f), add(r3, r2, -1.0f), r2}) {
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

// A box containing every copy's culling sphere. A margin makes both rejection and
// full acceptance conservative at plane boundaries despite float rounding.
u8 box_relation(const GpuFrustum& f, const Bounds3D& b) {
    const Vec3f c = b.center(), h = b.size() * 0.5f;
    const f32 margin = 1e-5f * (1 + std::max({std::abs(c.x), std::abs(c.y), std::abs(c.z), h.x, h.y, h.z}));
    if (!std::isfinite(margin) || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z))
        return 1;
    bool inside = true;
    for (u32 k = 0; k < f.count; ++k) {
        const Vec4f& p = f.planes[k];
        const f32 d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        const f32 r = std::abs(p.x) * h.x + std::abs(p.y) * h.y + std::abs(p.z) * h.z;
        if (d + r < -margin)
            return 0;
        if (d - r <= margin)
            inside = false;
    }
    return inside ? 2 : 1;
}
bool box_misses_sphere(const Bounds3D& b, Vec3f centre, f32 radius) {
    const Vec3f nearest{std::clamp(centre.x, b.min.x, b.max.x), std::clamp(centre.y, b.min.y, b.max.y),
                        std::clamp(centre.z, b.min.z, b.max.z)};
    const Vec3f d = nearest - centre;
    const f32 margin = 1e-5f * (1 + radius + std::max({std::abs(centre.x), std::abs(centre.y), std::abs(centre.z)}));
    return dot(d, d) > (radius + margin) * (radius + margin);
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

GpuFrustum reach_subset(const GpuFrustum& full, u8 pending) {
    GpuFrustum subset;
    for (u32 k = 0; k < full.count; ++k)
        if (pending & (1u << k))
            subset.planes[subset.count++] = full.planes[k];
    return subset;
}

bool reach_in(const GpuFrustum& f, shadow_reach::Sweep& sweep) {
    return sweep.reaches({f.planes.data(), f.count});
}

// How far from `c` along `down` until the box is left (0 outside it).
f32 to_leave(const Bounds3D& box, Vec3f c, Vec3f down) {
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
    f32 pixels = 1.0f;   // the error allowed on the screen
    f32 per_unit = 1.0f; // pixels a world unit spans: at one unit's distance (perspective), or anywhere (orthographic)
    Vec3f eye{}, forward{};
};

LodView lod_view_of(const Lod3D& lod, const RenderDebugOptions3D& debug, const Camera3D& camera, f32 viewport_height) {
    LodView v;
    v.on = lod.enabled;
    v.force = debug.force_level;
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

// All compute shaders use one uniform block. Resource counts and workgroup
// dimensions are explicit at the call site; unspecified create-info fields stay zero.
struct ComputeSpec {
    u32 samplers = 0;
    u32 readonly_buffers = 0, readwrite_buffers = 0;
    u32 readwrite_textures = 1;
    u32 threads_x = 8, threads_y = 8;
};
SDL_GPUComputePipeline* make_compute(SDL_GPUDevice* device, std::string_view file, ComputeSpec spec) {
    const std::span<const unsigned char> code = embedded_gpu_shader(file);
    if (code.empty())
        return nullptr;
    SDL_GPUComputePipelineCreateInfo info{};
    info.code = code.data();
    info.code_size = code.size();
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.num_samplers = spec.samplers;
    info.num_readonly_storage_buffers = spec.readonly_buffers;
    info.num_readwrite_storage_buffers = spec.readwrite_buffers;
    info.num_readwrite_storage_textures = spec.readwrite_textures;
    info.num_uniform_buffers = 1;
    info.threadcount_x = spec.threads_x;
    info.threadcount_y = spec.threads_y;
    info.threadcount_z = 1;
    return SDL_CreateGPUComputePipeline(device, &info);
}

void put(f32* out, Vec3f v, f32 w) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = w;
}
void put_color(f32* to, LinearRgb color, f32 w) {
    to[0] = color.r;
    to[1] = color.g;
    to[2] = color.b;
    to[3] = w;
}

void fill_atmosphere(AtmosphereUniforms& a, const Sky3D& sky, Vec3f sun_direction) {
    const Vec3f sun = length_squared(sun_direction) > 0.0f ? normalize(sun_direction) : Vec3f{0.0f, 1.0f, 0.0f};
    const Vec3f moon =
        length_squared(sky.moon_direction) > 0.0f ? normalize(sky.moon_direction) : Vec3f{0.0f, 1.0f, 0.0f};
    put(a.sun, sun, sky.sun_intensity);
    put(a.shape,
        {sky_model::mie_per_turbidity * std::clamp(sky.turbidity, 1.0f, 20.0f), sky_model::mie_anisotropy,
         sky.intensity},
        sky.moon ? std::max(sky.moon_light, 0.0f) : 0.0f);
    put_color(a.ground, sky.ground_color, 0.0f);
    put(a.moon, moon, 0.0f);
    put_color(a.night, sky.night_color, 0.0f);
}

// `colour_from_sky` takes the procedural atmosphere's colour; under SkyMode3D::Image's panorama the fixed colour stands
// in, in every pass alike (the particles' fog is made per vertex, where the panorama cannot be read), as in
// kin::fog_color.
void fill_fog(FogParamUniforms& f, const Fog3D& fog, const Sky3D& sky, bool orthographic) {
    put_color(f.colour, fog.colour, fog.colour_from_sky && sky.mode == SkyMode3D::Procedural ? 1.0f : 0.0f);
    put(f.density, {std::max(fog.density, 0.0f), std::max(fog.height_density, 0.0f), fog.height_falloff},
        fog.base_height);
    put(f.limits, {std::max(fog.start, 0.0f), std::clamp(fog.max_opacity, 0.0f, 1.0f), std::max(fog.sun_scatter, 0.0f)},
        std::clamp(fog.sun_anisotropy, -0.95f, 0.95f));
    put(f.view, {orthographic ? 1.0f : 0.0f, 0.0f, 0.0f}, 0.0f);
}

// A depth target cleared to the far plane (reverse Z: 0), whose stencil is not used.
SDL_GPUDepthStencilTargetInfo depth_target(SDL_GPUTexture* texture, SDL_GPUStoreOp store_op, bool cycle = false,
                                           u32 layer = 0) {
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
    const SDL_GPUStorageTextureReadWriteBinding output{
        .texture = target.handle(), .mip_level = 0, .layer = 0, .cycle = false};
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
bool compute_pass_two(SDL_GPUCommandBuffer* commands, SDL_GPUComputePipeline* pipeline, const GpuTexture& first,
                      const GpuTexture& second, std::span<const SDL_GPUTextureSamplerBinding> inputs,
                      const void* uniforms, u32 bytes) {
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

    // Retained batches share pipelines but have independent instance buffers.
    // Rebind changed storage even when the pipeline stays put; the return value
    // still reports pipeline changes for the caller's fragment binding cache.
    bool bind_pipeline(SDL_GPUGraphicsPipeline* pipeline, SDL_GPUBuffer* const* storage, u32 count) {
        const bool changed = pipeline != _pipeline;
        if (changed) {
            SDL_BindGPUGraphicsPipeline(_pass, pipeline);
            _pipeline = pipeline;
        }
        if (changed || count != _storage_count || !std::equal(storage, storage + count, _storage.begin())) {
            SDL_BindGPUVertexStorageBuffers(_pass, 0, storage, count);
            std::copy_n(storage, count, _storage.begin());
            _storage_count = count;
        }
        return changed;
    }

    // The mesh's vertices and, unless `indices` is given (a clustered mesh's compacted ones), its own indices.
    void bind_mesh(const GpuMesh3D* mesh, SDL_GPUBuffer* indices = nullptr) {
        // Pool allocations contain whole GpuMeshVertex/u32 elements. Keep the page bound at zero and
        // address a mesh with base_vertex/first_index, including its LOD index ranges. Skins and
        // clustered meshes have dedicated vertices (zero offset); compacted indices start at zero.
        const u32 vertex_offset = 0u;
        const SDL_GPUBuffer* index_buffer = indices ? indices : mesh->indices;
        const u32 index_offset = 0u;
        if (mesh->vertices != _vertices) {
            const SDL_GPUBufferBinding vertices{mesh->vertices, vertex_offset};
            SDL_BindGPUVertexBuffers(_pass, 0, &vertices, 1);
            _vertices = mesh->vertices;
        }
        if (index_buffer != _indices) {
            const SDL_GPUBufferBinding binding{indices ? indices : mesh->indices, index_offset};
            SDL_BindGPUIndexBuffer(_pass, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            _indices = index_buffer;
        }
        _first_index = !indices ? mesh->index_offset / sizeof(u32) : 0u;
        _base_vertex = static_cast<i32>(mesh->vertex_offset / sizeof(GpuMeshVertex));
    }

    void draw(u32 indices, u32 instances, u32 first_index) const {
        SDL_DrawGPUIndexedPrimitives(_pass, indices, instances, first_index + _first_index, _base_vertex, 0);
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
        _vertices = nullptr;
        _indices = nullptr;
    }

private:
    SDL_GPURenderPass* _pass;
    SDL_GPUGraphicsPipeline* _pipeline = nullptr;
    std::array<SDL_GPUBuffer*, 6> _storage{}; // largest vertex storage layout is skinned velocity
    u32 _storage_count = 0;
    const GpuMesh3D* _skin_mesh = nullptr;
    SDL_GPUBuffer* _vertices = nullptr;
    const SDL_GPUBuffer* _indices = nullptr;
    u32 _first_index = 0;
    i32 _base_vertex = 0;
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
    if (buffer_pool)
        buffer_pool->retire(std::move(allocation));
    if (device && *device) {
        for (SDL_GPUBuffer* buffer :
             {buffer_pool ? nullptr : vertices, skin, chunk || buffer_pool ? nullptr : indices}) {
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
    // SDL makes its devices with anisotropic filtering (Vulkan's samplerAnisotropy, which promises 16; Direct3D 12's
    // and Metal's most is 16).
    _capabilities.max_anisotropy = 16;
    // Metal's samplers have no mip bias (SDL drops it there).
    _capabilities.texture_lod_bias = std::string_view(device.driver_name()) != "metal";
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
    {
        std::string lowered = name ? name : "";
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* software : {"llvmpipe", "lavapipe", "swiftshader", "microsoft basic render"}) {
            if (lowered.find(software) != std::string::npos) {
                _capabilities.software = true;
            }
        }
    }
    KIN_LOG_INFO_F(
        "render", "3D views available",
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
        _shadow_mask_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "shadow_mask.vert.spv",
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
    {
        try {
            _indexed_vertex =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_indexed.vert.spv", 1, 0, 2);
            _indexed_unlit_vertex =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_unlit_indexed.vert.spv", 1, 0, 2);
            _indexed_unlit_textured_vertex = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX,
                                                                      "mesh_unlit_textured_indexed.vert.spv", 1, 0, 2);
            _indexed_prepass_vertex =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass_indexed.vert.spv", 1, 0, 2);
            _retained_enabled = true;
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: retained instances unavailable",
                           (LogFields{{.name = "reason", .value = e.what()}}));
        }
    }
    {
        try {
            _unlit_vertex_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_unlit.vert.spv", 1, 0, 1);
            _unlit_textured_vertex_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_unlit_textured.vert.spv", 1, 0, 1);
            _unlit_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_unlit.frag.spv", 2, 0);
            _unlit_textured_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_unlit_textured.frag.spv", 2, 1);
            _unlit_mask_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_unlit_mask.frag.spv", 2, 0);
            _unlit_textured_mask_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                                   "mesh_unlit_textured_mask.frag.spv", 2, 1);
            _unlit_available = true;
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: unlit specialization unavailable",
                           (LogFields{{.name = "reason", .value = e.what()}}));
        }
    }
    // Skinned meshes (Scene3D::draw_skinned): the same placing with the vertex posed by its joints first. Storage
    // buffers: the instances, [the shadow vertex shaders' list of copies,] the frame's joint matrices and each copy's
    // first one.
    try {
        _mesh_skinned_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_skinned.vert.spv",
                                     /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/3);
        // The velocity variants also read the copies' matrices of the frame before, and the joints of the frame before
        // with where each copy's start there.
        _mesh_skinned_velocity_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_skinned_velocity.vert.spv",
                                     /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/6);
        _prepass_skinned_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass_skinned.vert.spv",
                                     /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/3);
        _prepass_velocity_skinned_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass_velocity_skinned.vert.spv",
                                     /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/6);
        _shadow_skinned_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "shadow_skinned.vert.spv",
                                     /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/4);
        _shadow_mask_skinned_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "shadow_mask_skinned.vert.spv",
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
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                                 SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
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
        _gtao_pipeline = make_compute(handle, "gtao.comp.spv", {.samplers = 2});
        _ao_blur_pipeline = make_compute(handle, "ao_blur.comp.spv", {.samplers = 2});
        _ao_blur_both_pipeline =
            make_compute(handle, "ao_blur_both.comp.spv",
                         {.samplers = 3, .readwrite_textures = 2}); // optional: two dispatches without
        _contact_pipeline = make_compute(handle, "contact.comp.spv", {.samplers = 2});
        // The occlusion over a linear depth chain; without these (or R32F storage) the first one is used.
        if (SDL_GPUTextureSupportsFormat(handle, hiz_format, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
            _gtao_chain_pipeline = make_compute(handle, "gtao_chain.comp.spv", {.samplers = 2});
            _ao_blur_chain_pipeline = make_compute(handle, "ao_blur_chain.comp.spv", {.samplers = 2});
            _ao_blur_both_chain_pipeline =
                make_compute(handle, "ao_blur_both_chain.comp.spv", {.samplers = 3, .readwrite_textures = 2});
            _ao_chain_pipeline = make_compute(handle, "ao_depth_chain.comp.spv",
                                              {.samplers = 1,
                                               .readonly_buffers = 0,
                                               .readwrite_buffers = 0,
                                               .readwrite_textures = ao_chain_levels,
                                               .threads_x = 16,
                                               .threads_y = 16});
        }
        if (!_point_sampler || !_gtao_pipeline || !_ao_blur_pipeline || !_contact_pipeline) {
            throw std::runtime_error(std::string{"compute pipelines not made: "} + SDL_GetError());
        }
        _ao_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: ambient occlusion and contact shadows not available",
                       (LogFields{{.name = "reason", .value = e.what()}}));
    }
    // Temporal anti-aliasing: the velocity pre-pass, four-float colour targets
    // to draw it into and a float storage texture for the history, over what
    // the pre-pass already needs.
    if (_ao_available) {
        try {
            _prepass_velocity_vertex_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "prepass_velocity.vert.spv",
                                         /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/2);
            _prepass_velocity_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "prepass_velocity.frag.spv");
            _prepass_velocity_mask_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "prepass_velocity_mask.frag.spv",
                                         /*uniform_buffers=*/1, /*samplers=*/1);
            _mesh_velocity_vertex_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "mesh_velocity.vert.spv",
                                         /*uniform_buffers=*/1, /*samplers=*/0, /*storage_buffers=*/2);
            _mesh_velocity_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_velocity.frag.spv",
                                         /*uniform_buffers=*/2, /*samplers=*/10);
            if (!SDL_GPUTextureSupportsFormat(handle, hdr_format, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER) ||
                !SDL_GPUTextureSupportsFormat(handle, hdr_format, SDL_GPU_TEXTURETYPE_2D,
                                              SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                                  SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
                throw std::runtime_error("no four-float colour target or storage texture");
            }
            _taa_pipeline = make_compute(handle, "taa.comp.spv", {.samplers = 6, .readwrite_textures = 2});
            if (!_taa_pipeline) {
                throw std::runtime_error(std::string{"compute pipeline not made: "} + SDL_GetError());
            }
            _particle_temporal_vertex =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "particle_temporal.vert.spv", 4, 0, 2);
            _reactive_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "taa_reactive.frag.spv", 1, 1);
            _taa_available = true;
            _taa_depth_sampled =
                SDL_GPUTextureSupportsFormat(handle, _depth_format, SDL_GPU_TEXTURETYPE_2D,
                                             SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        } catch (const std::exception& e) {
            KIN_LOG_INFO_F("render", "3D: temporal anti-aliasing not available",
                           (LogFields{{.name = "reason", .value = e.what()}}));
        }
    }
    _capabilities.ambient_occlusion = _capabilities.contact_shadows = _ao_available;
    _capabilities.temporal_aa = _taa_available;
    // Global illumination (G5): its compute, before any mesh is made (their buffers are made for it to read).
    make_gi();
    // Fog and the sky (P3a): the fog variants of the stock mesh shaders (one more uniform block, in the slot custom
    // parameters have elsewhere), the sky pass's shaders, the panorama pass of the sky's image-based light. Each its
    // own try: a device without them draws as before (said once when a scene asks).
    try {
        _mesh_fog_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_fog.frag.spv",
                                                    /*uniform_buffers=*/3, /*samplers=*/10);
        if (_mesh_velocity_shader) {
            _mesh_velocity_fog_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_velocity_fog.frag.spv",
                                         /*uniform_buffers=*/3, /*samplers=*/10);
        }
        _fog_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: fog not available", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    try {
        _sky_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "sky.frag.spv",
                                               /*uniform_buffers=*/2, /*samplers=*/1);
        if (_mesh_velocity_shader) {
            _sky_velocity_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                            "sky_velocity.frag.spv", /*uniform_buffers=*/2,
                                                            /*samplers=*/1);
            _sky_velocity_only_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "sky_velocity_only.frag.spv",
                                         /*uniform_buffers=*/1, /*samplers=*/0);
        }
        _sky_available = true;
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: sky not available", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    // Particles (P3c): billboards from a storage buffer, fogged in the vertex shader; the soft variant reads the
    // pre-pass's depth.
    try {
        _particle_vertex_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "particle.vert.spv", /*uniform_buffers=*/2,
                                     /*samplers=*/0, /*storage_buffers=*/1);
        _particle_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle.frag.spv",
                                                    /*uniform_buffers=*/1, /*samplers=*/1);
        _particle_reactive_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_reactive.frag.spv", 1, 1);
        _particle_light_vertex =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "particle_light.vert.spv", 3, 0, 1);
        _particle_light_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_light.frag.spv", 1, 1);
        _particle_reactive_light_shader =
            GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_reactive_light.frag.spv", 1, 1);
        _particles_available = true;
        _capabilities.particles = true;
        try {
            _particle_soft_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                             "particle_soft.frag.spv", /*uniform_buffers=*/1,
                                                             /*samplers=*/2);
            _particle_soft_reactive_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_soft_reactive.frag.spv", 1, 2);
            _particle_soft_light_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_soft_light.frag.spv", 1, 2);
            _particle_soft_reactive_light_shader = GpuShader::from_embedded(
                device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_soft_reactive_light.frag.spv", 1, 2);
            _soft_particles_available = true;
            _capabilities.soft_particles = true;
        } catch (const std::exception& e) {
            KIN_LOG_INFO_F("render", "3D: soft particles not available",
                           (LogFields{{.name = "reason", .value = e.what()}}));
        }
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: particles not available", (LogFields{{.name = "reason", .value = e.what()}}));
    }
    if (_particles_available && _shadow_format != SDL_GPU_TEXTUREFORMAT_INVALID) {
        try {
            _particle_shadow_vertex =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_VERTEX, "particle_shadow.vert.spv", 2, 0, 1);
            _particle_shadow_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_shadow.frag.spv", 3, 5);
            _particle_soft_shadow_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_soft_shadow.frag.spv", 3, 5);
            _particle_reactive_shadow_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                                        "particle_reactive_shadow.frag.spv", 3, 5);
            _particle_soft_reactive_shadow_shader = GpuShader::from_embedded(
                device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_soft_reactive_shadow.frag.spv", 3, 5);
            _particle_cast_shader =
                GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "particle_cast.frag.spv", 1, 1);
            _capabilities.particle_shadows = true;
        } catch (const std::exception& e) {
            KIN_LOG_INFO_F("render", "3D: particle shadows not available",
                           (LogFields{{.name = "reason", .value = e.what()}}));
        }
    }
    try {
        _output_dither_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                         "view_output_dither.frag.spv", /*uniform_buffers=*/1,
                                                         /*samplers=*/2);
        _sky_env_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "sky_env.frag.spv",
                                                   /*uniform_buffers=*/1, /*samplers=*/0);
    } catch (const std::exception& e) {
        KIN_LOG_INFO_F("render", "3D: image-based light from the sky not available",
                       (LogFields{{.name = "reason", .value = e.what()}}));
    }
    // The cluster cut in compute (Scene3D::debug.gpu_culling): four compute pipelines over storage buffers, and buffers
    // that are storage and index or indirect arguments at once. Unsupported devices use the CPU cut.
    {
        {
            _gc_cull = make_compute(handle, "cluster_cull.comp.spv",
                                    {.readonly_buffers = 3,
                                     .readwrite_buffers = 3,
                                     .readwrite_textures = 0,
                                     .threads_x = 64,
                                     .threads_y = 1});
            _gc_scan = make_compute(handle, "cluster_scan.comp.spv",
                                    {.readonly_buffers = 0,
                                     .readwrite_buffers = 2,
                                     .readwrite_textures = 0,
                                     .threads_x = 256,
                                     .threads_y = 1});
            _gc_args = make_compute(handle, "cluster_args.comp.spv",
                                    {.readonly_buffers = 2,
                                     .readwrite_buffers = 1,
                                     .readwrite_textures = 0,
                                     .threads_x = 64,
                                     .threads_y = 1});
            _gc_gather = make_compute(handle, "cluster_gather.comp.spv",
                                      {.readonly_buffers = 5,
                                       .readwrite_buffers = 2,
                                       .readwrite_textures = 0,
                                       .threads_x = 64,
                                       .threads_y = 1});
            bool usable = _gc_cull && _gc_scan && _gc_args && _gc_gather;
            for (const SDL_GPUBufferUsageFlags usage :
                 {SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                  SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
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
            // The indirect draws carry the copy's slot as the argument's first instance and the vertex shader reads
            // instances[first + gl_InstanceIndex]: Vulkan's gl_InstanceIndex and Metal's instance_id include it,
            // Direct3D 12's SV_InstanceID does not (SDL_GPU promises nothing), so there every clustered copy would
            // take slot 0's transform. The cut stays on the CPU on any other driver.
            const std::string_view driver = device.driver_name();
            if (usable && driver != "vulkan" && driver != "metal") {
                usable = false;
                KIN_LOG_INFO_F("render",
                               "3D: cluster cut in compute off on this driver (its indirect draws' first instance "
                               "does not reach the vertex shader): the CPU cuts",
                               (LogFields{{.name = "driver", .value = std::string{driver}}}));
            }
            _gpu_clusters = usable;
        }
        _capabilities.gpu_driven_clusters = _gpu_clusters;
        // Occlusion culling (Lod3D::occlusion): the cut's second build, and the depth pyramid's two compute shaders
        // over R32F storage textures; it also uses the pre-pass's sampled depth and the point sampler.
        if (_gpu_clusters && _ao_available &&
            SDL_GPUTextureSupportsFormat(handle, hiz_format, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
            _gc_cull_occ = make_compute(handle, "cluster_cull_occ.comp.spv",
                                        {.samplers = 12,
                                         .readonly_buffers = 5,
                                         .readwrite_buffers = 4,
                                         .readwrite_textures = 0,
                                         .threads_x = 64,
                                         .threads_y = 1});
            _hiz_depth_pipeline = make_compute(handle, "hiz_depth.comp.spv",
                                               {.samplers = 1,
                                                .readonly_buffers = 0,
                                                .readwrite_buffers = 0,
                                                .readwrite_textures = 1,
                                                .threads_x = 8,
                                                .threads_y = 8});
            _hiz_reduce_pipeline = make_compute(handle, "hiz_reduce.comp.spv",
                                                {.samplers = 1,
                                                 .readonly_buffers = 0,
                                                 .readwrite_buffers = 0,
                                                 .readwrite_textures = 4,
                                                 .threads_x = 16,
                                                 .threads_y = 16});
            try {
                if (_gc_cull_occ && _hiz_depth_pipeline && _hiz_reduce_pipeline) {
                    for (GpuTexture& spare : _hiz_spare) {
                        spare = device.create_target(1, 1, hiz_format,
                                                     SDL_GPU_TEXTUREUSAGE_SAMPLER |
                                                         SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
                    }
                    _occlusion_available = true;
                }
            } catch (const std::exception&) {
            }
        }
        _capabilities.occlusion_culling = _occlusion_available;
        if (!_gpu_clusters) {
            KIN_LOG_INFO("render", "3D: cluster cut in compute not available on this device: the CPU cuts");
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
        if (!code.empty() &&
            SDL_GPUTextureSupportsFormat(handle, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
            _filter_pipeline = make_compute(handle, "env_filter.comp.spv", {.samplers = 1});
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
            _no_shadow = device.create_target_array(
                1, 1, 1, _shadow_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        } catch (const std::exception&) {
            _shadow_format = SDL_GPU_TEXTUREFORMAT_INVALID;
        }
    }
    // Spot lights and the shadows of lights that are not the sun (P3b): the light variants of the stock mesh shaders.
    // They sample two more depth arrays (the dummy `_no_shadow` where there is no map), and global illumination's
    // light on the view (G5; dummies where there is none), so they need the shadow format;
    // absent, and the features off (said once, when a scene asks).
    if (_shadow_format != SDL_GPU_TEXTUREFORMAT_INVALID && _no_shadow) {
        try {
            _mesh_light_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_light.frag.spv",
                                                          /*uniform_buffers=*/3,
                                                          /*samplers=*/14);
            _mesh_light_fog_shader = GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                              "mesh_light_fog.frag.spv", /*uniform_buffers=*/4,
                                                              /*samplers=*/14);
            if (_mesh_velocity_shader) {
                _mesh_velocity_light_shader =
                    GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_velocity_light.frag.spv",
                                             /*uniform_buffers=*/3, /*samplers=*/14);
                _mesh_velocity_light_fog_shader =
                    GpuShader::from_embedded(device, SDL_GPU_SHADERSTAGE_FRAGMENT, "mesh_velocity_light_fog.frag.spv",
                                             /*uniform_buffers=*/4, /*samplers=*/14);
            }
            _lights_available = true;
            _capabilities.local_shadows = true;
        } catch (const std::exception& e) {
            KIN_LOG_INFO_F("render", "3D: spot lights and local shadows not available",
                           (LogFields{{.name = "reason", .value = e.what()}}));
        }
    }
}

GpuRenderer3D::~GpuRenderer3D() {
    release_gi_pipelines();
    if (_compare_sampler) {
        SDL_ReleaseGPUSampler(_device->handle(), _compare_sampler);
    }
    if (_environment_sampler) {
        SDL_ReleaseGPUSampler(_device->handle(), _environment_sampler);
    }
    for (SDL_GPUComputePipeline* compute :
         {_filter_pipeline, _ao_blur_chain_pipeline, _ao_blur_both_chain_pipeline, _gtao_pipeline, _gtao_chain_pipeline,
          _ao_chain_pipeline, _ao_blur_pipeline, _ao_blur_both_pipeline, _contact_pipeline, _taa_pipeline, _gc_cull,
          _gc_scan, _gc_args, _gc_gather, _gc_cull_occ, _hiz_depth_pipeline, _hiz_reduce_pipeline}) {
        if (compute) {
            SDL_ReleaseGPUComputePipeline(_device->handle(), compute);
        }
    }
    if (_point_sampler) {
        SDL_ReleaseGPUSampler(_device->handle(), _point_sampler);
    }
    for (const auto& [key, sampler] : _samplers) {
        if (sampler) {
            SDL_ReleaseGPUSampler(_device->handle(), sampler);
        }
    }
}

SDL_GPUSampler* GpuRenderer3D::sampler_for(const GpuTextureBackend& texture, Wrap3D u, Wrap3D v) {
    const ScaleMode scale =
        texture.scale_mode() == ScaleMode::Mipmapped && !texture.mipmapped() ? ScaleMode::Linear : texture.scale_mode();
    const bool mipmapped = scale == ScaleMode::Mipmapped;
    const u32 anisotropy = mipmapped ? _anisotropy : 1;
    const i32 bias = mipmapped ? _lod_bias : 0;
    if (u == Wrap3D::Clamp && v == Wrap3D::Clamp && anisotropy == 1 && bias == 0) {
        return _owner->sampler_for(texture); // as 2D draws it
    }
    const u32 key = static_cast<u32>(scale) | static_cast<u32>(u) << 2 | static_cast<u32>(v) << 4 | anisotropy << 6 |
                    static_cast<u32>(bias + 64) << 11;
    if (const auto found = _samplers.find(key); found != _samplers.end()) {
        return found->second ? found->second : _owner->sampler_for(texture);
    }
    const auto mode = [](Wrap3D wrap) {
        return wrap == Wrap3D::Repeat   ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
               : wrap == Wrap3D::Mirror ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT
                                        : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    };
    SDL_GPUSamplerCreateInfo info{};
    info.address_mode_u = mode(u);
    info.address_mode_v = mode(v);
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    const bool nearest = scale == ScaleMode::Nearest;
    info.min_filter = info.mag_filter = nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
    info.mipmap_mode =
        scale == ScaleMode::Mipmapped ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.max_lod = scale == ScaleMode::Mipmapped ? 1000.0f : 0.0f;
    info.enable_anisotropy = anisotropy > 1;
    info.max_anisotropy = static_cast<f32>(anisotropy);
    info.mip_lod_bias = static_cast<f32>(bias) / 16.0f;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(_device->handle(), &info);
    _samplers.emplace(key, sampler);
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

SDL_GPUGraphicsPipeline* GpuRenderer3D::pipeline(SDL_GPUTextureFormat format, u32 samples, bool cull, Pass pass,
                                                 bool mask, SDL_GPUShader* custom, bool velocity, bool skinned,
                                                 bool fog, bool lights, Unlit unlit, bool indexed) const {
    if (skinned && !_skinning_available) {
        return nullptr;
    }
    const GpuVertexLayout layout = skinned ? GpuVertexLayout::SkinnedMeshVertices : GpuVertexLayout::MeshVertices;

    if (pass == Pass::Reactive || pass == Pass::ReactiveWire) {
        const bool wire = pass == Pass::ReactiveWire;
        return _owner->_pipelines.get(GpuPipelineKey{
            .vertex = (skinned ? _mesh_skinned_velocity_vertex_shader : _mesh_velocity_vertex_shader).handle(),
            .fragment = _reactive_shader.handle(),
            .blend = GpuBlendMode::Replace,
            .format = hdr_format,
            .layout = layout,
            .depth_stencil = _depth_format,
            .depth = GpuDepthTest::Test,
            .cull = cull && !wire ? GpuCull::Back : GpuCull::None,
            .wireframe = wire,
            .depth_bias = wire,
            .write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A});
    }

    if (pass == Pass::Shadow) {
        // A closed mesh's back faces are behind its front ones from the sun
        // too: culled, the map is the same for half the triangles. Double-sided
        // (thin, open) meshes cast with both. shadow.vert places the vertices
        // and does nothing else; a cut-out's also carry its UV and alpha to the
        // fragment shader that discards.
        SDL_GPUShader* vertex =
            skinned ? (mask ? _shadow_mask_skinned_vertex_shader : _shadow_skinned_vertex_shader).handle()
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
        // Lod3D::occlusion's depth of the frame's opaque meshes, when no pre-pass draws one: the shadow pass's shaders
        // (a cut-out's discard included) into the single-sample depth the pyramid is built from.
        return _owner->_pipelines.get(GpuPipelineKey{
            .vertex = skinned ? (mask ? _shadow_mask_skinned_vertex_shader : _shadow_skinned_vertex_shader).handle()
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
        return _owner->_pipelines.get(
            GpuPipelineKey{.vertex = (indexed   ? _indexed_prepass_vertex
                                      : skinned ? _prepass_skinned_vertex_shader
                                                : _prepass_vertex_shader)
                                         .handle(),
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
        return _owner->_pipelines.get(GpuPipelineKey{.vertex = (indexed   ? _indexed_vertex
                                                                : skinned ? _mesh_skinned_vertex_shader
                                                                          : _vertex_shader)
                                                                   .handle(),
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
                               : pass == Pass::Alpha                      ? GpuBlendMode::Premultiplied
                                                                          : GpuBlendMode::PremultipliedAdditive;
    // With the velocity target, the stock shaders write it (an opaque mesh) or the pipeline masks it; a custom shader
    // writes nothing there, so its draws are masked too (a frame with an opaque custom shader has its pre-pass
    // instead).
    SDL_GPUShader* vertex =
        skinned ? (velocity && !custom ? _mesh_skinned_velocity_vertex_shader : _mesh_skinned_vertex_shader).handle()
        : velocity && !custom ? _mesh_velocity_vertex_shader.handle()
        : indexed             ? _indexed_vertex.handle()
                              : _vertex_shader.handle();
    // The fog's variants (P3a): only for a stock shader, and only where they were made.
    const bool fogged = fog && !custom && _fog_available && (!velocity || _mesh_velocity_fog_shader);
    // The light variants (P3b), likewise (they have their own fog variants).
    const bool lit = lights && !custom && _lights_available && (!velocity || _mesh_velocity_light_shader);
    SDL_GPUShader* fragment = custom ? custom
                              : velocity
                                  ? (lit ? (fogged ? _mesh_velocity_light_fog_shader : _mesh_velocity_light_shader)
                                         : (fogged ? _mesh_velocity_fog_shader : _mesh_velocity_shader))
                                        .handle()
                                  : (lit ? (fogged ? _mesh_light_fog_shader : _mesh_light_shader)
                                         : (fogged ? _mesh_fog_shader : _fragment_shader))
                                        .handle();

    if (unlit != Unlit::Off) {
        vertex = (indexed ? (unlit == Unlit::Textured ? _indexed_unlit_textured_vertex : _indexed_unlit_vertex)
                          : (unlit == Unlit::Textured ? _unlit_textured_vertex_shader : _unlit_vertex_shader))
                     .handle();
        fragment = (unlit == Unlit::Textured ? (mask ? _unlit_textured_mask_shader : _unlit_textured_shader)
                                             : (mask ? _unlit_mask_shader : _unlit_shader))
                       .handle();
    }
    return _owner->_pipelines.get(
        GpuPipelineKey{.vertex = vertex,
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

GpuRenderer3D::MaterialPipeline GpuRenderer3D::material_pipeline(const Material3D& material, bool skinned, bool taa,
                                                                 bool velocity, bool fog) const {
    const auto* custom = dynamic_cast<const GpuMaterialShader3D*>(material.shader.backend().get());
    const Unlit unlit =
        _unlit_available && (material.unlit || material.wireframe) && !custom && !skinned && !taa && !velocity && !fog
            ? (material.texture ? Unlit::Textured : Unlit::Colour)
            : Unlit::Off;
    return {custom, unlit, !material.double_sided, material.blend == Blend3D::Mask, pass_of(material)};
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::pipeline(const MeshPipelineRequest& request) const {
    const auto& m = request.material;
    return pipeline(request.format, request.samples, m.cull, m.pass, m.mask,
                    m.custom ? m.custom->shader.handle() : nullptr, request.velocity, request.skinned, request.fog,
                    request.lights, m.unlit, request.indexed);
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::sky_pipeline(SDL_GPUTextureFormat format, u32 samples, bool velocity) const {
    if (!_sky_available || (velocity && !_sky_velocity_shader)) {
        return nullptr;
    }
    // Reverse Z: the sky is drawn at depth 0 and passes where the depth buffer still holds the far plane (cleared to 0)
    // and nowhere something was drawn (greater than 0). Replaces: it is opaque.
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

SDL_GPUGraphicsPipeline* GpuRenderer3D::particle_pipeline(SDL_GPUTextureFormat format, u32 samples,
                                                          ParticleBlend3D blend, bool soft, bool velocity,
                                                          bool reactive, bool local, bool shadows) const {
    if (!_particles_available || (soft && !_soft_particles_available)) {
        return nullptr;
    }
    // Tested against the depth, never written; premultiplied targets (additive adds, and keeps the destination's
    // alpha).
    return _owner->_pipelines.get(GpuPipelineKey{
        .vertex = (reactive  ? _particle_temporal_vertex
                   : shadows ? _particle_shadow_vertex
                   : local   ? _particle_light_vertex
                             : _particle_vertex_shader)
                      .handle(),
        .fragment =
            (shadows ? (reactive ? (soft ? _particle_soft_reactive_shadow_shader : _particle_reactive_shadow_shader)
                                 : (soft ? _particle_soft_shadow_shader : _particle_shadow_shader))
             : local ? (reactive ? (soft ? _particle_soft_reactive_light_shader : _particle_reactive_light_shader)
                                 : (soft ? _particle_soft_light_shader : _particle_light_shader))
                     : (reactive ? (soft ? _particle_soft_reactive_shader : _particle_reactive_shader)
                                 : (soft ? _particle_soft_shader : _particle_shader)))
                .handle(),
        .blend = reactive                             ? GpuBlendMode::Replace
                 : blend == ParticleBlend3D::Additive ? GpuBlendMode::PremultipliedAdditive
                                                      : GpuBlendMode::Premultiplied,
        .format = format,
        .layout = GpuVertexLayout::None,
        .depth_stencil = _depth_format,
        .depth = GpuDepthTest::Test,
        .cull = GpuCull::None,
        .samples = sample_count(samples),
        .extra_format = velocity ? hdr_format : SDL_GPU_TEXTUREFORMAT_INVALID,
        .extra_write = false,
        .write_mask = SDL_GPUColorComponentFlags(reactive ? SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_B |
                                                                SDL_GPU_COLORCOMPONENT_A
                                                          : SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                                                                SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A)});
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

SDL_GPUGraphicsPipeline* GpuRenderer3D::velocity_pipeline(SDL_GPUTextureFormat depth, bool cull, bool mask,
                                                          bool skinned) const {
    if (skinned && !_skinning_available) {
        return nullptr;
    }
    // The pre-pass with velocity: one sample, the normal and the velocity in four floats.
    return _owner->_pipelines.get(GpuPipelineKey{
        .vertex = (skinned ? _prepass_velocity_skinned_vertex_shader : _prepass_velocity_vertex_shader).handle(),
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

std::shared_ptr<GpuClusterChunk> GpuRenderer3D::allocate_cluster_range(u32 indices, u32 clusters, u32& index_first,
                                                                       u32& table_first) {
    // Chunks no mesh uses go first.
    std::erase_if(_chunks, [](const std::shared_ptr<GpuClusterChunk>& chunk) { return chunk.use_count() == 1; });
    for (const std::shared_ptr<GpuClusterChunk>& chunk : _chunks) {
        if (static_cast<u64>(chunk->index_used) + indices <= chunk->index_capacity &&
            static_cast<u64>(chunk->table_used) + clusters <= chunk->table_capacity) {
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
    const SDL_GPUBufferCreateInfo index_info{
        .usage = SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
        .size = static_cast<u32>(static_cast<u64>(chunk->index_capacity) * sizeof(u32)),
        .props = 0};
    const SDL_GPUBufferCreateInfo table_info{
        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
        .size = static_cast<u32>(static_cast<u64>(chunk->table_capacity) * sizeof(GpuCluster)),
        .props = 0};
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

void GpuRenderer3D::on_frame_submitted() {
    if (!_mesh_buffers || !_mesh_buffers->pending())
        return;
    // An independent fence following the submitted draws/uploads. The frame timer owns its own fence; neither may be
    // released while unsignalled. This empty command does not mark an earlier start for the device's frame timer.
    auto* commands = SDL_AcquireGPUCommandBuffer(_device->handle());
    if (!commands)
        throw std::runtime_error(std::string{"mesh retirement command failed: "} + SDL_GetError());
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence)
        throw std::runtime_error(std::string{"mesh retirement fence failed: "} + SDL_GetError());
    _mesh_buffers->seal(fence);
}

void GpuRenderer3D::collect_mesh_buffers() {
    if (!_mesh_buffers)
        return;
    if (_mesh_buffers->pending() && _mesh_buffers->stats().retired > 16ull * 1024 * 1024) {
        // Normal retirements are sealed after present/readback. Large bursts without a frame also need bounded
        // backpressure.
        if (_owner->_frame) {
            _owner->flush_to_frame();
            _owner->_frame->submit();
            _owner->end_frame();
        } else {
            _device->flush_uploads();
            on_frame_submitted();
        }
    }
    _mesh_buffers->collect();
}

std::shared_ptr<IMeshBackend3D> GpuRenderer3D::create_mesh(const MeshData3D& source, const MeshLodChain3D* chain,
                                                           std::shared_ptr<const MeshClusters3D> clusters) {
    collect_mesh_buffers();
    // With clusters `source` is the hierarchy's welded vertices and its leaves' triangles (full detail, for the draws
    // that do not cut); the hierarchy itself stays on the CPU, for the cut, and has no vertices (they are only here).
    if (clusters) {
        chain = nullptr;
    }
    const MeshData3D& data = source;
    const std::size_t vertex_count = data.vertices.size();
    const std::size_t level0_count = data.indices.empty() ? vertex_count : data.indices.size();
    // The levels' indices follow level 0's in the one index buffer. A mesh the GPU cuts (compute reads every cluster's
    // triangles from the index buffer) has those of the clusters above the leaves after the leaves' own, in cluster
    // order.
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
            mesh->chunk = allocate_cluster_range(static_cast<u32>(index_count),
                                                 static_cast<u32>(clusters->clusters.size()), index_first, table_first);
            mesh->table_first = table_first;
        }
        const u32 vertex_bytes = static_cast<u32>((vertex_count + extra_vertices) * sizeof(GpuMeshVertex));
        const u32 index_bytes = static_cast<u32>(index_count * sizeof(u32));
        // Where global illumination can be had, its compute reads every mesh's vertices, indices and skin.
        const SDL_GPUBufferUsageFlags gi_read =
            _capabilities.global_illumination ? SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ : 0;
        const SDL_GPUBufferCreateInfo vertex_info{
            .usage = SDL_GPU_BUFFERUSAGE_VERTEX | gi_read, .size = vertex_bytes, .props = 0};
        const SDL_GPUBufferCreateInfo index_info{
            .usage = SDL_GPU_BUFFERUSAGE_INDEX | gi_read, .size = index_bytes, .props = 0};
        if (!clusters && !source.skinned()) {
            if (!_mesh_buffers)
                _mesh_buffers = std::make_shared<GpuMeshBufferPool>(_device->shared(), gi_read);
            mesh->allocation = _mesh_buffers->allocate(vertex_bytes, index_bytes);
            if (mesh->allocation) {
                mesh->buffer_pool = _mesh_buffers;
                mesh->vertices = mesh->allocation.vertices.page->buffer;
                mesh->indices = mesh->allocation.indices.page->buffer;
                mesh->vertex_offset = mesh->allocation.vertices.offset;
                mesh->index_offset = mesh->allocation.indices.offset;
            }
        }
        if (!mesh->buffer_pool) {
            mesh->vertices = SDL_CreateGPUBuffer(_device->handle(), &vertex_info);
            mesh->indices = mesh->chunk ? mesh->chunk->indices : SDL_CreateGPUBuffer(_device->handle(), &index_info);
        }
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
        const auto upload = [&](SDL_GPUBuffer* buffer, std::size_t count, std::size_t size, const auto& write,
                                std::size_t base_bytes = 0) {
            const std::size_t per_piece = std::max<std::size_t>(1, piece_bytes / size);
            for (std::size_t first = 0; first < count; first += per_piece) {
                const std::size_t n = std::min(per_piece, count - first);
                const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
                    in_slices(n, [&](std::size_t begin, std::size_t end) { write(out.data(), first, begin, end); });
                };
                _device->upload_storage_buffer(buffer, static_cast<u32>(base_bytes + first * size),
                                               static_cast<u32>(n * size), nullptr, false, &fill);
            }
        };
        upload(
            mesh->vertices, vertex_count + extra_vertices, sizeof(GpuMeshVertex),
            [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
                auto* to = reinterpret_cast<GpuMeshVertex*>(out);
                for (std::size_t i = begin; i < end; ++i) {
                    const std::size_t g = first + i; // the mesh's vertices, then the levels' own
                    const MeshVertex3D& v =
                        g < vertex_count ? data.vertices[g] : chain->extra_vertices[g - vertex_count];
                    to[i] = {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
                             v.uv.x,       v.uv.y,       v.color.r,    v.color.g,  v.color.b,  v.color.a};
                }
            },
            mesh->vertex_offset);
        if (!clusters && !chain && source.skinned() && source.skin.size() == vertex_count) {
            // The skin stream: a second vertex buffer, 8 bytes a vertex. A joint whose weight came to nothing points at
            // joint 0 (always there), so that the shader never reads a matrix that is not.
            const SDL_GPUBufferCreateInfo skin_info{.usage = SDL_GPU_BUFFERUSAGE_VERTEX | gi_read,
                                                    .size = static_cast<u32>(vertex_count * sizeof(GpuSkinVertex)),
                                                    .props = 0};
            mesh->skin = SDL_CreateGPUBuffer(_device->handle(), &skin_info);
            if (!mesh->skin) {
                throw std::runtime_error(std::string{"SDL_CreateGPUBuffer failed: "} + SDL_GetError());
            }
            upload(mesh->skin, vertex_count, sizeof(GpuSkinVertex),
                   [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
                       auto* to = reinterpret_cast<GpuSkinVertex*>(out);
                       for (std::size_t i = begin; i < end; ++i) {
                           const MeshSkinVertex3D& k = source.skin[first + i];
                           const std::array<u8, 4> w = quantise_skin_weights(k.weights);
                           to[i] = {w[0] ? k.joints[0] : u8{0},
                                    w[1] ? k.joints[1] : u8{0},
                                    w[2] ? k.joints[2] : u8{0},
                                    w[3] ? k.joints[3] : u8{0},
                                    w[0],
                                    w[1],
                                    w[2],
                                    w[3]};
                       }
                   });
            mesh->skin_bounds = compute_skin_bounds(source, source.skin_joint_count());
        }
        upload(
            mesh->indices, index_count, sizeof(u32),
            [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
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
                    const std::size_t from = std::max(lo, level_start),
                                      to_end = std::min(hi, level_start + indices.size());
                    if (from < to_end) {
                        std::memcpy(to + (from - first), indices.data() + (from - level_start),
                                    (to_end - from) * sizeof(u32));
                    }
                    level_start += indices.size();
                }
                // The clusters above the leaves, each from its first index.
                if (gpu_table && hi > level0_count) {
                    std::size_t at = std::max(lo, level0_count);
                    std::size_t i = static_cast<std::size_t>(
                                        std::upper_bound(pool_first.begin(), pool_first.end(), static_cast<u64>(at)) -
                                        pool_first.begin()) -
                                    1;
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
            },
            static_cast<std::size_t>(index_first) * sizeof(u32) + mesh->index_offset);
        if (gpu_table) {
            // The clusters' table, for the cut in compute.
            std::vector<u32> first_index(clusters->clusters.size());
            std::size_t running = index_first;
            for (std::size_t c = 0; c < first_index.size(); ++c) {
                first_index[c] = static_cast<u32>(running);
                running += static_cast<std::size_t>(clusters->clusters[c].triangle_count) * 3;
            }
            upload(
                mesh->chunk->table, clusters->clusters.size(), sizeof(GpuCluster),
                [&](u8* out, std::size_t first, std::size_t begin, std::size_t end) {
                    auto* to = reinterpret_cast<GpuCluster*>(out);
                    for (std::size_t i = begin; i < end; ++i) {
                        const std::size_t c = first + i;
                        const MeshCluster3D& k = clusters->clusters[c];
                        to[i] = {{k.center.x, k.center.y, k.center.z},
                                 k.radius,
                                 {k.parent_center.x, k.parent_center.y, k.parent_center.z},
                                 k.parent_radius,
                                 {k.cull_center.x, k.cull_center.y, k.cull_center.z},
                                 k.cull_radius,
                                 k.error,
                                 k.parent_error,
                                 0.0f,
                                 0.0f,
                                 first_index[c],
                                 k.triangle_count,
                                 k.depth,
                                 k.parent_depth};
                    }
                },
                static_cast<std::size_t>(table_first) * sizeof(GpuCluster));
        }
        // For global illumination: the box of each run of level 0's triangles.
        if (_capabilities.global_illumination) {
            const std::size_t triangles = level0_count / 3;
            mesh->gi_runs.resize((triangles + gi_run_triangles - 1) / gi_run_triangles);
            for (std::size_t r = 0; r < mesh->gi_runs.size(); ++r) {
                constexpr f32 far = std::numeric_limits<f32>::max();
                Bounds3D box{{far, far, far}, {-far, -far, -far}};
                const std::size_t end = std::min<std::size_t>(level0_count, (r + 1) * gi_run_triangles * 3);
                for (std::size_t i = r * gi_run_triangles * 3; i < end; ++i) {
                    const std::size_t v = data.indices.empty() ? i : data.indices[i];
                    if (v < vertex_count) {
                        const Vec3f q = data.vertices[v].position;
                        box.min = {std::min(box.min.x, q.x), std::min(box.min.y, q.y), std::min(box.min.z, q.z)};
                        box.max = {std::max(box.max.x, q.x), std::max(box.max.y, q.y), std::max(box.max.z, q.z)};
                    }
                }
                mesh->gi_runs[r] = box;
            }
        }
        mesh->clusters = std::move(clusters);
        mesh->index_count = static_cast<u32>(level0_count);
        mesh->levels.push_back({index_first, static_cast<u32>(level0_count), 0.0f});
        std::size_t level_start = level0_count;
        for (std::size_t l = 1; chain && l < chain->levels.size(); ++l) {
            mesh->levels.push_back({static_cast<u32>(index_first + level_start),
                                    static_cast<u32>(chain->levels[l].indices.size()), chain->levels[l].error});
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

std::shared_ptr<IMaterialShaderBackend3D> GpuRenderer3D::create_material_shader(std::span<const u8> spirv) {
    try {
        auto shader = std::make_shared<GpuMaterialShader3D>();
        // Frame, material, custom parameters with fog, and local lights;
        // all twelve material samplers (kin_material.inc.glsl).
        shader->shader =
            GpuShader::from_bytes(*_device, SDL_GPU_SHADERSTAGE_FRAGMENT, SDL_GPU_SHADERFORMAT_SPIRV, spirv,
                                  /*uniform_buffers=*/4, /*samplers=*/12);
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
        _device->poison_texture(texture, format, w, h, 1, levels,
                                usage); // KIN_POISON_TEXTURES: a debug aid, nothing when unset
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
    environment->specular =
        make(sw, sw / 2, levels, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
    environment->irradiance =
        make(64, 32, 1, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
    return environment;
}

std::shared_ptr<IEnvironmentBackend3D> GpuRenderer3D::create_environment(Vec2i size, std::span<const f32> rgb) {
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
        _device->update_texture(environment->source.handle(), 0, 0, w, h, reinterpret_cast<const u8*>(texels.data()), 8,
                                true);
        // KIN_POISON_TEXTURES: the levels to be generated, filled with the poison after level 0's (cycling) upload.
        _device->poison_texture(environment->source.handle(), format, w, h, 1, GpuDevice::complete_mip_levels(w, h),
                                SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, SDL_GPU_SAMPLECOUNT_1,
                                /*first_level=*/1);
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
        const f32 params[8] = {static_cast<f32>(w),
                               static_cast<f32>(h),
                               roughness,
                               static_cast<f32>(mode),
                               static_cast<f32>(environment.source.width()),
                               static_cast<f32>(environment.source.height()),
                               static_cast<f32>(samples),
                               0.0f};
        SDL_PushGPUComputeUniformData(commands, 0, params, sizeof(params));
        SDL_DispatchGPUCompute(pass, (w + 7) / 8, (h + 7) / 8, 1);
        SDL_EndGPUComputePass(pass);
    };
    for (u32 level = 0; level < environment.levels; ++level) {
        const f32 roughness =
            environment.levels > 1 ? static_cast<f32>(level) / static_cast<f32>(environment.levels - 1) : 0.0f;
        filter(environment.specular, level, level == 0 ? 0u : 1u, roughness, environment_samples);
    }
    filter(environment.irradiance, 0, 2, 1.0f, irradiance_samples);
    environment.filtered = true;
}

bool GpuRenderer3D::make_targets(GpuView3D& view) {
    const SDL_GPUTextureFormat output = _owner->_target_format;
    const bool want_hdr = view._options.hdr || view._options.bloom > 0.0f;
    const bool hdr =
        want_hdr && SDL_GPUTextureSupportsFormat(_device->handle(), hdr_format, SDL_GPU_TEXTURETYPE_2D,
                                                 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    const SDL_GPUTextureFormat format = hdr ? hdr_format : output;
    const u32 samples = usable_samples(std::max(1u, view._options.samples), format);
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    constexpr SDL_GPUTextureUsageFlags drawn = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    try {
        GpuTexture resolved = _device->create_target(w, h, output, drawn);
        GpuTexture scene = hdr ? _device->create_target(w, h, hdr_format, drawn) : GpuTexture{};
        GpuTexture colour =
            samples > 1 ? _device->create_target(w, h, format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, sample_count(samples))
                        : GpuTexture{};
        GpuTexture depth = _device->create_target(w, h, _depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
                                                  sample_count(samples));
        GpuTexture pick, pick_depth;
        if (view._options.pick && _pick_shader &&
            SDL_GPUTextureSupportsFormat(_device->handle(), pick_format, SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) {
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
        view._resolved =
            std::make_shared<GpuTextureBackend>(std::move(resolved), /*premultiplied=*/true, ScaleMode::Linear);
        view._scene = std::move(scene);
        view._colour = std::move(colour);
        view._depth = std::move(depth);
        view._bloom_a = std::move(bloom_a);
        view._bloom_b = std::move(bloom_b);
        view._pick = std::move(pick);
        view._pick_depth = std::move(pick_depth);
        view._pick_valid = false; // nothing has drawn into the new one yet
        release_taa(view);        // sized for the old targets
        view._hiz =
            {}; // Lod3D::occlusion's pyramid and depth are sized for them too, and the history of clusters is of no use
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

std::shared_ptr<IViewBackend3D> GpuRenderer3D::create_view(Vec2i size, const ViewOptions3D& options) {
    auto view = std::make_shared<GpuView3D>();
    view->_size = size;
    view->_options = options;
    if (!make_targets(*view)) {
        return {};
    }
    return view;
}

bool GpuRenderer3D::resize_view(IViewBackend3D& base, Vec2i size) {
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
            _shadow_map =
                _device->create_target_array(resolution, resolution, attempt, _shadow_format,
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
            KIN_LOG_WARN_F("render", "3D: no shadow map",
                           (LogFields{{.name = "layers", .value = std::to_string(attempt)},
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
    const bool have_base =
        sized(view._ao_depth, w, h) && view._ao_normals && view._ao_normals.format() == normals_format;
    const bool have_ao = !want_ao || (sized(view._ao_raw, aw, ah) && view._ao_across);
    const bool have_contact = !want_contact || (sized(view._contact_raw, w, h) && view._contact_across);
    if (have_base && have_ao && have_contact) {
        return true;
    }
    try {
        constexpr SDL_GPUTextureUsageFlags stored =
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        if (!have_base) {
            GpuTexture depth = _device->create_target(
                w, h, _ao_depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            GpuTexture normals = _device->create_target(
                w, h, normals_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            view._ao_depth = std::move(depth);
            view._ao_normals = std::move(normals);
        }
        if (!have_ao) {
            // The blur goes raw -> across -> raw: the second pass writes over the first dispatch's result, which
            // nothing reads again, so the result is in _ao_raw and two textures do.
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
        KIN_LOG_WARN_F("render", "3D: ambient occlusion targets not made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
        for (GpuTexture* texture : {&view._ao_chain, &view._ao_depth, &view._ao_normals, &view._ao_raw,
                                    &view._ao_across, &view._contact_raw, &view._contact_across}) {
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
    if (view._ao_normals && view._ao_normals.width() == w && view._ao_normals.height() == h &&
        view._ao_normals.format() == hdr_format) {
        return true;
    }
    try {
        view._ao_normals =
            _device->create_target(w, h, hdr_format, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
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

// The linear depth chain of the pre-pass's depth (ao_depth_chain.comp): made with the first use and when the view's
// size changes. False when it cannot be made or built: the caller then reads the depth itself.
bool GpuRenderer3D::build_ao_chain(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Mat4f& projection,
                                   f32 radius) {
    if (!_gtao_chain_pipeline || !_ao_chain_pipeline) {
        return false;
    }
    const u32 w = view._ao_depth.width(), h = view._ao_depth.height();
    if (!view._ao_chain || view._ao_chain.width() != w || view._ao_chain.height() != h) {
        try {
            view._ao_chain = _device->create_target(
                w, h, hiz_format, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
                SDL_GPU_SAMPLECOUNT_1, ao_chain_levels);
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: the occlusion's depth chain not made",
                           (LogFields{{.name = "reason", .value = e.what()}}));
            view._ao_chain = {};
            return false;
        }
    }
    const Unprojection unproject = unproject_coefficients(projection);
    OcclusionChainUniforms u{};
    std::memcpy(u.unproject_zw, unproject.zw, sizeof(unproject.zw));
    // The weights of a texel in the average (the falloff of gtao_chain.comp, over XeGTAO's 0.75 of the radius): a texel
    // nearer than the farthest of its four by the range counts for nothing.
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

void GpuRenderer3D::compute_occlusion(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera,
                                      const Mat4f& projection, const AmbientOcclusion3D& ao, RenderDebugStats3D& stats,
                                      bool blur_too) {
    const Unprojection unproject = unproject_coefficients(projection);
    const f32 w = static_cast<f32>(view._ao_raw.width()), h = static_cast<f32>(view._ao_raw.height());
    const f32 depth_w = static_cast<f32>(view._ao_depth.width()), depth_h = static_cast<f32>(view._ao_depth.height());
    _chain_ready = false;
    SDL_GPUTextureSamplerBinding depth{view._ao_depth.handle(), _point_sampler};
    const auto dispatch = [&](SDL_GPUComputePipeline* pipeline, const GpuTexture& target, const GpuTexture& second,
                              const void* uniforms, u32 bytes) {
        const std::array<SDL_GPUTextureSamplerBinding, 2> inputs{
            depth, SDL_GPUTextureSamplerBinding{second.handle(), _point_sampler}};
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
    const f32 shape[4] = {std::max(ao.radius, 0.001f), std::max(ao.intensity, 0.0f),
                          projection.at(1, 1) * 0.5f * depth_h,
                          camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f};
    std::memcpy(u.shape, shape, sizeof(shape));
    const bool use_chain = static_cast<u64>(depth_w) * static_cast<u64>(depth_h) >= ao_chain_min_pixels;
    if (use_chain && build_ao_chain(commands, view, projection, u.shape[0])) {
        // The chain stands in for the depth as the first input.
        const std::array<SDL_GPUTextureSamplerBinding, 2> inputs{
            SDL_GPUTextureSamplerBinding{view._ao_chain.handle(), _point_sampler},
            SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _point_sampler}};
        if (compute_pass(commands, _gtao_chain_pipeline, view._ao_raw, inputs, &u, sizeof(u))) {
            ++stats.ao_dispatches;
            stats.ao_chain_built = 1;
            // The blurs (this one's, the contact shadows', the merged) read the chain's first level instead of the
            // depth.
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

void GpuRenderer3D::compute_contact_shadows(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Camera3D& camera,
                                            const Mat4f& projection, const ContactShadows3D& contact, const Vec3f& sun,
                                            RenderDebugStats3D& stats, bool blur_too) {
    const Unprojection unproject = unproject_coefficients(projection);
    const f32 w = static_cast<f32>(view._contact_raw.width()), h = static_cast<f32>(view._contact_raw.height());
    SDL_GPUTextureSamplerBinding depth{
        view._ao_depth.handle(), _point_sampler}; // (the march reads the depth; its blur the chain, when there is one)
    const auto dispatch = [&](SDL_GPUComputePipeline* pipeline, const GpuTexture& target, const GpuTexture& second,
                              const void* uniforms, u32 bytes) {
        const std::array<SDL_GPUTextureSamplerBinding, 2> inputs{
            depth, SDL_GPUTextureSamplerBinding{second.handle(), _point_sampler}};
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
    const f32 sun_view[4] = {to_sun.x, to_sun.y, to_sun.z,
                             camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f};
    std::memcpy(u.sun, sun_view, sizeof(sun_view));
    const f32 shape[4] = {std::max(contact.distance, 0.001f), std::max(contact.thickness, 0.001f),
                          std::clamp(contact.intensity, 0.0f, 1.0f),
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
    std::swap(view._contact_raw, view._contact_across);
}

void GpuRenderer3D::blur_screen_space_both(SDL_GPUCommandBuffer* commands, GpuView3D& view, const Mat4f& projection,
                                           RenderDebugStats3D& stats) {
    // ao_blur_both.comp: the occlusion's and the contact shadows' two blur dispatches as one each (the same size, so
    // the same depth weights). Each feature is counted for the dispatches it is in.
    const Unprojection unproject = unproject_coefficients(projection);
    const f32 w = static_cast<f32>(view._ao_raw.width()), h = static_cast<f32>(view._ao_raw.height());
    const f32 sizes[4] = {w, h, static_cast<f32>(view._ao_depth.width()), static_cast<f32>(view._ao_depth.height())};
    OcclusionBlurUniforms blur{};
    std::memcpy(blur.sizes, sizes, sizeof(sizes));
    std::memcpy(blur.depth, unproject.zw, sizeof(unproject.zw));
    const auto dispatch = [&](const GpuTexture& ao_into, const GpuTexture& ao_from, const GpuTexture& contact_into,
                              const GpuTexture& contact_from) {
        const std::array<SDL_GPUTextureSamplerBinding, 3> inputs{
            SDL_GPUTextureSamplerBinding{_chain_ready ? view._ao_chain.handle() : view._ao_depth.handle(),
                                         _point_sampler},
            SDL_GPUTextureSamplerBinding{ao_from.handle(), _point_sampler},
            SDL_GPUTextureSamplerBinding{contact_from.handle(), _point_sampler}};
        if (compute_pass_two(commands, _chain_ready ? _ao_blur_both_chain_pipeline : _ao_blur_both_pipeline, ao_into,
                             contact_into, inputs, &blur, sizeof(blur))) {
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

void GpuRenderer3D::finish_view(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& source, bool plain,
                                bool dither) {
    const auto pass_into = [&](const GpuTexture& target, SDL_GPUGraphicsPipeline* p,
                               std::span<const SDL_GPUTexture* const> sources, const f32(&params)[4]) {
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
                     : output == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM      ? 0.0f
                     : output == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ? 1.0f
                                                                           : 2.0f;
    const std::array<const SDL_GPUTexture*, 2> sources{source.handle(),
                                                       bloom ? view._bloom_a.handle() : source.handle()};
    const f32 params[4] = {bloom ? view._options.bloom : 0.0f, plain ? 1.0f : view._options.exposure, mode, 0.0f};
    pass_into(
        view._resolved->texture(),
        fullscreen(dither && _output_dither_shader ? _output_dither_shader.handle() : _output_shader.handle(), output),
        sources, params);
}

void GpuRenderer3D::suspend_taa(GpuView3D& view) {
    if (view._taa_valid || view._taa_frame != 0 || !view._taa_last.groups.empty() || !view._taa_now.groups.empty() ||
        !view._taa_last.ids.empty() || !view._taa_now.ids.empty()) {
        view._taa_valid = false;
        view._taa_coverage_key = 0;
        view._taa_reactive = false;
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
    for (const GpuTexture* t : {&view._ao_chain, &view._ao_depth, &view._ao_normals, &view._ao_raw, &view._ao_across,
                                &view._contact_raw, &view._contact_across, &view._taa_depth, &view._taa_in,
                                &view._taa_sharp, &view._taa_motion, &view._taa_history[0], &view._taa_history[1],
                                &view._taa_dilated[0], &view._taa_dilated[1], &view._occ_depth}) {
        bytes += texture_bytes(*t);
    }
    for (const GpuTexture& t : view._hiz) {
        bytes += texture_bytes(t);
    }
    for (const std::shared_ptr<GpuBuffer>& h : view._occ_history) {
        bytes += h ? h->size() : 0u;
    }
    for (const std::shared_ptr<GpuBuffer>& b :
         {_gcb[1].scan, _gcb[1].blocks, _gcb[1].args, _gcb[1].list, _gcb[1].counts, _gcb[1].indices, _gc_job_history}) {
        bytes += b ? b->size() : 0u;
    }
    // The GI scene, every level's: a half, two 8-bit texels and four halves a voxel, and the build's two words (in
    // owners and seeds); the lit scene's four halves on each side; the probes' atlases (8 x 8 of four halves, 16 x 16
    // of two, four halves of data a probe), their rays and lists; their light on the view (four halves a texel, twice).
    const GpuView3D::Gi& gi = view._gi;
    const u64 voxels = static_cast<u64>(gi.size[0]) * gi.size[1] * gi.size[2] * gi.levels;
    bytes += gi.distance ? voxels * (2 + 4 + 4 + 8) : 0u;
    bytes += gi.radiance ? voxels * 16 : 0u;
    bytes += gi.irradiance ? static_cast<u64>(gi.probe_counts[0]) * gi.probe_counts[1] * gi.probe_counts[2] *
                                 gi.probe_levels * (64 * 8 + 256 * 4 + 8)
                           : 0u;
    bytes += gi.light ? static_cast<u64>(gi.light.width()) * gi.light.height() * 16 : 0u;
    bytes += gi.last_light ? static_cast<u64>(gi.last_light.width()) * gi.last_light.height() * 16 : 0u;
    for (const std::shared_ptr<GpuBuffer>& b :
         {gi.owners, gi.seeds, gi.rays, gi.place_list, gi.batch_list, gi.light_list}) {
        bytes += b ? b->size() : 0u;
    }
    return bytes;
}

void GpuRenderer3D::release_taa(GpuView3D& view) {
    // Nothing made, nothing to forget: the common case of a view that never used it.
    if (!view._taa_depth && !view._taa_history[0] && !view._taa_valid) {
        return;
    }
    std::vector<GpuView3D::ParticleHistory>().swap(view._taa_particles);
    view._taa_depth = {};
    view._taa_in = {};
    view._taa_sharp = {};
    view._taa_motion = {};
    view._taa_history = {};
    view._taa_dilated = {};
    view._taa_valid = false;
    view._taa_coverage_key = 0;
    view._taa_reactive = false;
    view._taa_frame = 0;
    view._taa_last = {};
    view._taa_now = {};
}

bool GpuRenderer3D::ensure_taa_targets(GpuView3D& view, bool sharpen) {
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    const auto sized = [&](const GpuTexture& t) { return t && t.width() == w && t.height() == h; };
    bool made = false;
    try {
        constexpr SDL_GPUTextureUsageFlags stored =
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        if (!sized(view._taa_depth)) {
            view._taa_depth = _device->create_target(w, h, _depth_format,
                                                     SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                                                         (_taa_depth_sampled ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0));
            made = true;
        }
        for (GpuTexture& history : view._taa_history) {
            if (!sized(history)) {
                history = _device->create_target(w, h, hdr_format, stored);
                view._taa_valid = false; // nothing in it
                made = true;
            }
        }
        for (GpuTexture& dilated : view._taa_dilated) {
            if (!sized(dilated)) {
                dilated = _device->create_target(w, h, SDL_GPU_TEXTUREFORMAT_R32_UINT, stored);
                view._taa_valid = false;
                made = true;
            }
        }
        if (!sized(view._taa_motion)) {
            view._taa_motion = _device->create_target(w, h, hdr_format,
                                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            view._taa_valid = false;
            made = true;
        }
        if (!view._hdr && !sized(view._taa_in)) {
            view._taa_in = _device->create_target(w, h, hdr_format,
                                                  SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            made = true;
        }
        if (sharpen && !sized(view._taa_sharp)) {
            view._taa_sharp = _device->create_target(w, h, hdr_format, stored);
            made = true;
        } else if (!sharpen) {
            view._taa_sharp = {};
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: temporal anti-aliasing targets not made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
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
    // same scene (the world or the overlay): by its explicit history_id, else its legacy id
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
        u32 effect_first = ~0u, past_effect_first = ~0u;
        bool transparent = false;
        bool grid_made = false;
        f32 cell = 1.0f;
        std::unordered_multimap<u64, u32> grid; // last frame's copies' centres by cell (made only if needed)
    };
    std::vector<Group> groups;
    const u32 ref_count = static_cast<u32>(_refs.size());
    // By the mesh's serial, not its address: a mesh freed and another made in the same place would else inherit its
    // copies' past.
    const auto key_of_ref = [&](u32 r) {
        const auto& ref = _refs[r];
        u64 key = mix64(ref.mesh->serial * 2 + ref.scene);
        if (pass_of(ref.draw->material) != Pass::Opaque)
            (void)taa_material_hash(key, ref.draw->material, ref.draw->material.texture.backend().get());
        return key;
    };
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
                groups.back().transparent = pass_of(_refs[r].draw->material) != Pass::Opaque;
                group_of[r] = id;
            } else {
                group_of[r] = group_of[group_of[r]];
            }
            groups[group_of[r]].count += _refs[r].draw->count;
        }
    }
    u32 total = 0, effect_count = 0;
    for (Group& g : groups) {
        g.first = total;
        total += g.count;
        if (g.transparent) {
            g.effect_first = effect_count;
            effect_count += g.count;
        }
        now.groups.push_back({g.key, g.first, g.count, g.effect_first});
        const auto past = std::lower_bound(last.groups.begin(), last.groups.end(), g.key,
                                           [](const GpuCopyHistory::Span& s, u64 key) { return s.key < key; });
        if (history_usable && past != last.groups.end() && past->key == g.key) {
            g.past_first = past->first;
            g.past_count = past->count;
            g.past_effect_first = past->effect_first;
        } else {
            g.past_first = g.past_count = ~0u;
        }
    }
    std::sort(now.groups.begin(), now.groups.end(),
              [](const GpuCopyHistory::Span& a, const GpuCopyHistory::Span& b) { return a.key < b.key; });
    now.matrices.resize(total);
    now.effects.assign(effect_count, 0);
    _taa_matched.assign(effect_count, ~0u);
    _taa_prev.resize(total);
    _taa_slow.resize(total);
    // Skinned copies: each copy's joints in the frame's (a place in `now.joints`), and where its past is, for the
    // joints of last frame to be found by the same match as its model matrix.
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
            const auto offset = used[group_of[r]];
            _taa_copy_refs[r] = {g.first + offset, same_count ? g.past_first + offset : ~0u,
                                 g.transparent ? g.effect_first + offset : ~0u,
                                 same_count && g.past_effect_first != ~0u ? g.past_effect_first + offset : ~0u};
            used[group_of[r]] += _refs[r].draw->count;
        }
    }
    const auto settle = [&](const Piece& piece) {
        const DrawRef& ref = _refs[piece.ref];
        const CopyRef& place = _taa_copy_refs[piece.ref];
        for (u32 i = piece.begin; i < piece.end; ++i) {
            const MeshInstance3D& instance = ref.instances[i];
            const u32 flat = piece.flat + (i - piece.begin);
            now.matrices[place.slot + i] = instance.transform;
            if (skinned) {
                now.joint_first[place.slot + i] = ref.skinned ? ref.joint_base + ref.skin_first[i] : ~0u;
            }
            Mat4f before = instance.transform;
            bool settled = instance.history_id == 0 && instance.id == 0;
            if (settled && history_usable) {
                // Most copies have not moved at all: no need to measure how far.
                settled = place.past != ~0u && (before = last.matrices[place.past + i],
                                                std::memcmp(before.m, instance.transform.m, sizeof(before.m)) == 0);
            }
            _taa_slow[flat] = settled ? 0 : 1;
            if (settled) {
                before.m[3] = 0.0f;
                _taa_prev[flat] = before;
                if (history_usable && place.effect != ~0u && place.past_effect != ~0u)
                    _taa_matched[place.effect + i] = place.past_effect + i;
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
            const MeshInstance3D& instance = ref.instances[i];
            const u32 slot = _taa_copy_refs[r].slot + i;
            Mat4f before = instance.transform;
            bool fresh = false;
            u32 past_slot = ~0u; // where the copy was last frame (its record's place)
            {
                bool by_id = false;
                if (instance.history_id != 0 || instance.id != 0) {
                    const u64 identity = instance.history_id ? instance.history_id : instance.id;
                    const u64 domain = instance.history_id ? 0xd6e8feb86659fd93ULL : 0;
                    const u64 id_key = mix64(g.key ^ (identity * 0x9e3779b97f4a7c15ULL) ^ domain);
                    // Recorded whether or not there is a past to look in: the next frame's would else find none.
                    if (now.ids.emplace(id_key, slot).second &&
                        history_usable) { // a second copy with the id is matched by place
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
                        // Stable collection order is the common moving-copy path. A
                        // tighter transparent radius still permits distant reorders
                        // to fall back to spatial matching.
                        const auto reach = g.transparent ? match_by_place_reach : match_by_place_move;
                        matched = same_count && length_squared(c - before.apply(ref.center)) <=
                                                    reach * reach * radius * radius + 1e-6f;
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
                            return mix64((static_cast<u64>(static_cast<u32>(x)) << 42) ^
                                         (static_cast<u64>(static_cast<u32>(y)) << 21) ^
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
                                        const f32 d = length_squared(
                                            last.matrices[g.past_first + it->second].apply(ref.center) - c);
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
                    // Keep the wider ordinal motion fallback for a fast copy
                    // whose old location lies outside the spatial search. It
                    // runs last so separated reorders still match by place.
                    if (!matched && same_count && g.transparent) {
                        before = last.matrices[g.past_first + (slot - g.first)];
                        Vec3f centre;
                        f32 radius;
                        sphere_of(instance.transform, ref.center, ref.radius, centre, radius);
                        matched = length_squared(centre - before.apply(ref.center)) <=
                                  match_by_place_move * match_by_place_move * radius * radius + 1e-6f;
                        if (matched)
                            past_slot = g.past_first + (slot - g.first);
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
            const auto effect = _taa_copy_refs[r].effect;
            if (effect != ~0u)
                _taa_matched[effect + i] = past_slot != ~0u && g.past_effect_first != ~0u
                                               ? g.past_effect_first + (past_slot - g.past_first)
                                               : ~0u;
            if (skinned) {
                _taa_past[flat] = past_slot;
            }
        }
    }
    if (skinned) {
        // The joints of the copy's past: where that copy's range started in last frame's, if it was a skinned copy
        // then.
        for (u32 f = 0; f < total; ++f) {
            const u32 past = _taa_past[f];
            _taa_prev_joint[f] = past != ~0u && past < last.joint_first.size() ? last.joint_first[past] : ~0u;
        }
    }
}

const GpuTexture* GpuRenderer3D::resolve_taa(SDL_GPUCommandBuffer* commands, GpuView3D& view, const GpuTexture& input,
                                             const TemporalAA3D& taa, bool reset, Vec2f jitter, bool reactive,
                                             bool reject, bool fixed_jitter, const GpuTexture* depth, bool overlaid,
                                             RenderDebugStats3D& stats) {
    const f32 w = static_cast<f32>(view._size.x), h = static_cast<f32>(view._size.y);
    const GpuTexture& past_motion = reset ? view._ao_normals : view._taa_motion;
    const auto dispatch = [&](const GpuTexture& target, const GpuTexture& current, const GpuTexture& history,
                              f32 sharpen, f32 sharpness) {
        const std::array<SDL_GPUTextureSamplerBinding, 6> inputs{
            SDL_GPUTextureSamplerBinding{current.handle(), _point_sampler},
            SDL_GPUTextureSamplerBinding{history.handle(), _owner->_sampler_linear},
            SDL_GPUTextureSamplerBinding{view._ao_normals.handle(), _point_sampler},
            SDL_GPUTextureSamplerBinding{past_motion.handle(), _point_sampler},
            SDL_GPUTextureSamplerBinding{depth ? depth->handle() : _owner->_white.handle(), _point_sampler},
            SDL_GPUTextureSamplerBinding{view._taa_dilated[view._taa_read].handle(), _point_sampler}};
        const TaaUniforms u{{w, h, 1.0f / w, 1.0f / h},
                            {std::clamp(taa.blend, 0.0f, 0.98f), reset ? 1.0f : 0.0f, sharpen, sharpness},
                            {jitter.x, jitter.y, (reactive || view._taa_reactive) ? (fixed_jitter ? 2.0f : 1.0f) : 0.0f,
                             reject ? 1.0f : 0.0f},
                            {depth ? 1.0f : 0.0f, overlaid ? overlay_split : 2.0f, 0.0f, 0.0f}};
        // (The sharpening dispatch binds this frame's velocities as its second output too, and writes nothing there.)
        if (!compute_pass_two(commands, _taa_pipeline, target, view._taa_dilated[1 - view._taa_read], inputs, &u,
                              sizeof(u))) {
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
    view._taa_reactive = reactive;
    const GpuTexture* shown = &view._taa_history[write];
    const f32 sharpness = std::clamp(taa.sharpness, 0.0f, 1.0f);
    if (sharpness > 0.0f && view._taa_sharp && dispatch(view._taa_sharp, *shown, *shown, 1.0f, sharpness)) {
        shown = &view._taa_sharp;
    }
    return shown;
}

// What render()'s stages share. Filled in the order they run: setup_frame,
// fit_sun_shadow, choose_point_lights, choose_local_lights, cull_and_upload, order_draws, begin_commands,
// then the passes.
struct GpuRenderer3D::FrameCtx {
    FrameCtx(GpuView3D* v, const Scene3D& s, RenderDebugStats3D& st) : view(v), scene(s), stats(st) {}
    GpuView3D* view;
    const Scene3D& scene;
    RenderDebugStats3D& stats;

    // The camera, and the overlay's (null without one).
    f32 aspect = 1.0f;
    std::array<const Scene3D*, 2> scenes{};
    std::array<Mat4f, 2> view_projections{}; // exact
    std::array<Mat4f, 2> drawn{};            // with the temporal anti-aliasing's jitter
    std::array<GpuFrustum, 2> frustums{};
    bool sun_shines = false;
    bool contact_wanted =
        false; // contact shadows asked for, with a sun that shines and casts (sun_shadow_strength > 0)

    // Temporal anti-aliasing for this frame (off where the device or the targets cannot do it).
    bool taa = false;
    bool taa_reset = false;
    bool taa_reactive = false;
    bool shadow_history = false; // selected lit receivers actually consume content revisions
    bool mesh_coverage = false;
    u64 coverage_key = 1469598103934665603ull;
    u64 shadow_revision = 0;
    bool shadow_changed = false;
    Vec2f jitter{};
    Mat4f main_projection; // what the pre-pass's compute unprojects with: the camera's, jittered with temporal
                           // anti-aliasing

    // The sun's shadow: one map over the area, or a plan of cascades.
    bool sun_shadow_cached = false;
    bool shadows = false;
    u32 cascades = 0; // the maps to draw
    Mat4f light_vp{};
    GpuFrustum light_frustum{};
    f32 texel_world = 0.0f, depth_range = 1.0f;
    ShadowCascades3D plan; // with more than one
    std::array<GpuFrustum, max_shadow_cascades> cascade_frustums{}, slice_frustums{};

    std::vector<std::pair<f32, const PointLightSource3D*>> chosen;
    // Spot lights and the shadows of lights that are not the sun (P3b). `spots`: indices into Lights3D::spots of those
    // used. `lights_variant`: the main pass uses the light shaders. `lu`: their block for the scene's own space.
    // `moon_*`: the fill light's cascades (a single map is one cascade). The joints are uploaded for the frame (the
    // local views' skinned casters).
    std::vector<u32> spots;
    bool lights_variant = false;
    LocalLightUniforms lu{};
    u32 moon_cascades = 0;
    ShadowCascades3D moon_plan;
    bool joints_uploaded = false;
    u32 joint_total = 0;
    std::array<u32, 2> joint_base{};
    GpuEnvironment3D* environment = nullptr; // null: the hemisphere ambient
    bool sky_env_used =
        false; // `environment` is the sky's own (Lights3D::sky_lights_scene): in world space, no rotation
    // The sky and the fog (P3a): asked for and possible; `sky_pass` once the image sky's environment is known to be
    // there.
    bool sky = false;
    bool sky_pass = false;
    bool fog = false;

    // Recording.
    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPUBuffer* storage = nullptr;                // the instances
    SDL_GPUBuffer* previous_storage = nullptr;       // last frame's matrices (temporal anti-aliasing)
    SDL_GPUBuffer* joints = nullptr;                 // skinned copies: the joint matrices,
    SDL_GPUBuffer* joint_offsets = nullptr;          // and each copy's first
    SDL_GPUBuffer* previous_joints = nullptr;        // with temporal anti-aliasing: last frame's joint matrices
    SDL_GPUBuffer* previous_joint_offsets = nullptr; // and where each copy's start in them
    bool overlaid = false;                           // the overlay has draws in view
    bool occluded = false;       // the world has ambient occlusion this render (set before it is drawn)
    bool contacted = false;      // and contact shadows
    bool velocity_drawn = false; // the pre-pass (or the main pass) began, so _ao_normals holds this frame's velocity
    bool prepass_ran = false;    // the depth and normal pre-pass began this render
    bool main_velocity = false;  // the main pass writes the velocity (taa with nothing else to need a pre-pass)
    const GpuTexture* shown = nullptr; // the picture after the temporal resolve
    std::array<LodView, 2> lod{};      // Scene3D::lod's choosing, for the scene and the overlay
    bool gpu_clusters = false;         // clustered meshes are cut in compute this render
    bool occlusion =
        false; // Lod3D::occlusion: the cut runs in two passes (the first drawn before the pyramid is built)
    bool occlusion_said =
        false; // the cut turned it off for a reason of its own (nothing to test, or too many clusters)
    bool occlusion_second = false; // and the second is recorded
    bool occlusion_tests =
        false; // the second pass tests against a pyramid (else it keeps every cluster the first pass did not draw)
    // Particles (P3c): some batch is soft (the pre-pass is made for its depth), and there are batches to draw in the
    // main pass.
    bool soft_particles = false;
    bool particles = false;
    bool gi = false;          // the view has a GI scene for this render (Scene3D::global_illumination)
    bool gi_lit = false;      // and its probes light the scene (the light variants read them)
    bool gi_resolved = false; // and their light on what the pre-pass drew was read (gi_resolve)
};

void GpuRenderer3D::age_targets(FrameCtx& ctx) {
    GpuView3D& v = *ctx.view;
    RenderDebugStats3D& stats = ctx.stats;
    // Used this render: the group's idle count restarts. Idle: it counts up and, on reaching the limit, gives the group
    // back.
    const auto age = [&](u32& idle, bool used, bool held, const std::function<void()>& release) {
        if (used) {
            idle = 0;
        } else if (held && ++idle >= _release_after) {
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
    age(v._idle_normals, ctx.prepass_ran || ctx.main_velocity, static_cast<bool>(v._ao_normals),
        [&] { v._ao_normals = {}; });
    age(v._idle_taa, ctx.taa,
        v._taa_depth || v._taa_history[0] || v._taa_history[1] || v._taa_in || v._taa_sharp || v._taa_motion,
        [&] { release_taa(v); });
    age(v._idle_occlusion, ctx.occlusion, v._hiz_levels > 0 || v._occ_depth || v._occ_history[0] || v._occ_history[1],
        [&] {
            v._hiz = {};
            v._hiz_levels = 0;
            v._occ_depth = {};
            v._occ_history = {};
            v._occ_layout.clear();
            v._occ_words = 0;
            v._occ_reset = true;
        });
    age(v._idle_gi, ctx.gi, static_cast<bool>(v._gi.distance), [&] { release_gi(v); });
    // The renderer's second gathered buffer (occlusion culling's 12 bytes a triangle of the room) goes when no view has
    // used it.
    const bool second_held = _gcb[1].indices || _gcb[1].scan || _gc_job_history;
    if (ctx.occlusion) {
        _idle_occlusion_renders = 0;
    } else if (second_held && ++_idle_occlusion_renders >= _release_after) {
        _gcb[1] = {};
        _gc_job_history = {};
        _idle_occlusion_renders = 0;
        ++stats.targets_released;
    }
    stats.feature_bytes = feature_bytes(v);
}

void GpuRenderer3D::ensure_buffer(std::shared_ptr<GpuBuffer>& buffer, u32 bytes, u32 minimum,
                                  SDL_GPUBufferUsageFlags usage) {
    if (!buffer || buffer->size() < bytes) {
        const u32 size = std::max({bytes, buffer ? buffer->size() * 2 : 0u, minimum});
        buffer = std::make_shared<GpuBuffer>(_device->create_buffer(usage, nullptr, size));
    }
}

// P3c. Sorts (depth, index) pairs furthest first, stably (equal depths keep their order): a least-significant-digit
// radix sort on the depth's bits, flipped so that larger floats come first. 24,000 pairs take about 0.2 ms where
// std::sort took 1.5.
static void radix_sort_far_first(std::vector<std::pair<f32, u32>>& keys, std::vector<std::pair<f32, u32>>& scratch) {
    const std::size_t n = keys.size();
    if (n < 2) {
        return;
    }
    scratch.resize(n);
    const auto bits_of = [](f32 v) {
        u32 u;
        std::memcpy(&u, &v, sizeof(u));
        u = (u & 0x80000000u) ? ~u : (u | 0x80000000u); // ascending as floats
        return ~u;                                      // so descending first
    };
    std::vector<std::pair<f32, u32>>* from = &keys;
    std::vector<std::pair<f32, u32>>* to = &scratch;
    for (u32 pass = 0; pass < 3; ++pass) {
        const u32 shift = pass * 11u;
        std::array<u32, 2048> count{};
        for (const auto& k : *from) {
            ++count[(bits_of(k.first) >> shift) & 2047u];
        }
        u32 sum = 0;
        for (u32& c : count) {
            const u32 here = c;
            c = sum;
            sum += here;
        }
        for (const auto& k : *from) {
            (*to)[count[(bits_of(k.first) >> shift) & 2047u]++] = k;
        }
        std::swap(from, to);
    }
    if (from != &keys) {
        keys = *from; // three passes: the result is in the scratch
    }
}

// P3c. The frame's particle batches: the scene's draw_particles and the weather's layers (made here, from the clock and
// the camera), each culled against the camera's view, sorted back to front (alpha and premultiplied; additive and `sort
// = false` keep their order), written as GpuParticle3D into one storage buffer, batch after batch. The batches are then
// drawn far to near by their mean depth. Cost: O(sprites) for the tests, O(n log n) for the sort of each sorted batch,
// and the 64 bytes a sprite of the upload.
void GpuRenderer3D::prepare_weather(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    const bool has_weather = scene.weather.active();
    // The weather's sprites are a function of the weather's numbers, the camera and the view's frustum (for the cull):
    // the last render's are kept while all of those are the same (a view drawn twice, a paused game, a photo mode; a
    // benchmark that renders one scene many times).
    std::vector<f32>& key = _weather_key_now;
    key.clear();
    if (has_weather) {
        const Weather3D& w = scene.weather;
        const auto put_f = [&](f32 v) { key.push_back(v); };
        const auto put_v = [&](Vec3f v) {
            put_f(v.x);
            put_f(v.y);
            put_f(v.z);
        };
        const auto put_u = [&](u64 v) {
            put_f(std::bit_cast<f32>(static_cast<u32>(v)));
            put_f(std::bit_cast<f32>(static_cast<u32>(v >> 32)));
        };
        for (const WeatherLayer3D* l : {&w.rain, &w.snow, &w.dust, &w.mist}) {
            for (const f32 v : {l->intensity,
                                static_cast<f32>(l->max_particles),
                                l->above,
                                l->fall_speed,
                                l->fall_variation,
                                l->wind_influence,
                                l->flutter,
                                l->flutter_rate,
                                l->size_min,
                                l->size_max,
                                l->edge_fade,
                                l->alpha,
                                l->splash,
                                l->splash_size,
                                l->splash_time,
                                l->look.stretch,
                                l->look.stretch_min,
                                l->look.softness,
                                l->look.ring,
                                l->look.soft ? 1.0f : 0.0f,
                                l->look.soft_distance,
                                l->look.lit,
                                l->look.receives_shadow ? 1.0f : 0.0f,
                                l->look.casts_shadow ? 1.0f : 0.0f,
                                l->look.shadow_cutoff,
                                l->look.fogged ? 1.0f : 0.0f,
                                l->look.sort ? 1.0f : 0.0f,
                                static_cast<f32>(static_cast<u32>(l->look.blend)),
                                static_cast<f32>(static_cast<u32>(l->look.facing)),
                                static_cast<f32>(l->look.atlas_columns),
                                static_cast<f32>(l->look.atlas_rows),
                                static_cast<f32>(l->look.atlas_frames),
                                l->look.blend_frames ? 1.0f : 0.0f}) {
                put_f(v);
            }
            put_v(l->box);
            put_f(l->colour.r);
            put_f(l->colour.g);
            put_f(l->colour.b);
            put_v(l->look.axis);
            put_u(reinterpret_cast<std::uintptr_t>(l->look.texture.backend().get()));
        }
        put_v(w.wind.direction);
        for (const f32 v : {w.wind.speed, w.wind.gust_strength, w.wind.gust_period, w.wind.gust_length,
                            w.wind.turbulence, w.wind.turbulence_length, w.has_ground ? 1.0f : 0.0f, w.ground_height,
                            static_cast<f32>(w.seed), scene.cull ? 1.0f : 0.0f}) {
            put_f(v);
        }
        put_u(std::bit_cast<u64>(w.time));
        put_v(scene.camera.position);
        for (const f32 v : ctx.view_projections[0].m) {
            put_f(v);
        }
    }
    const std::array weather_layers{&scene.weather.rain, &scene.weather.snow, &scene.weather.dust, &scene.weather.mist};
    const bool weather_casts =
        has_weather && std::any_of(weather_layers.begin(), weather_layers.end(),
                                   [](const auto* l) { return l->intensity > 0 && l->look.casts_shadow; });
    std::vector<GpuFrustum> caster_frustums;
    if (weather_casts && scene.cull) {
        const auto map = [&](const Mat4f& matrix) {
            key.insert(key.end(), std::begin(matrix.m), std::end(matrix.m));
            caster_frustums.push_back(frustum_of(matrix));
        };
        if (ctx.shadows) {
            if (ctx.cascades > 1)
                for (u32 c = 0; c < ctx.cascades; ++c)
                    map(ctx.plan.cascade[c].view_projection);
            else
                map(ctx.light_vp);
        }
        for (const auto& view : _local_views)
            if (view.wanted)
                map(view.view_projection);
    }
    const bool weather_kept = has_weather && key == _weather_key;
    if (!weather_kept) {
        _weather_sprites.clear();
        _weather_batches.clear();
        if (!has_weather) {
            _weather_key
                .clear(); // (a later render with the weather on makes them: nothing is kept for a key it matches)
        }
    }
    if (has_weather && !weather_kept) {
        JobSystem* jobs = _owner->_jobs;
        const ParallelFor3D parallel =
            jobs && jobs->worker_count() > 1
                ? ParallelFor3D{[jobs](i32 n, const std::function<void(i32)>& body) { jobs->parallel_for(n, body); }}
                : ParallelFor3D{};
        const GpuFrustum& view_frustum = ctx.frustums[0];
        const SphereTest3D in_view = scene.cull ? SphereTest3D{[&](Vec3f centre, f32 radius) {
            if (sphere_in(view_frustum, centre, radius))
                return true;
            return std::any_of(caster_frustums.begin(), caster_frustums.end(),
                               [&](const auto& frustum) { return sphere_in(frustum, centre, radius); });
        }}
                                                : SphereTest3D{};
        weather_sprites(scene.weather, scene.camera, _weather_sprites, _weather_batches, parallel, in_view);
        _weather_key = key;
    }
}

std::vector<GpuRenderer3D::ParticleSource> GpuRenderer3D::particle_sources(const Scene3D& scene) const {
    std::vector<ParticleSource> sources;
    sources.reserve(scene.particle_draws().size() + _weather_batches.size());
    for (const Scene3D::ParticleDraw& d : scene.particle_draws()) {
        u64 history = 1469598103934665603ull;
        const bool explicit_id = d.history_id != 0;
        taa_hash(history, explicit_id);
        taa_hash(history, explicit_id ? d.history_id : u64(sources.size()));
        sources.push_back({&d.look, scene.particle_sprites().data() + d.first, d.count, false, history});
    }
    for (const WeatherBatch3D& b : _weather_batches) {
        u64 history = 1469598103934665603ull;
        taa_hash(history, b.kind); // enum type and a salt keep weather outside immediate emitter IDs
        taa_hash(history, scene.weather.seed);
        const u64 salt = 0xa7be92cd17ull;
        taa_hash(history, salt);
        sources.push_back({&b.look, _weather_sprites.data() + b.first, b.count, true, history});
    }
    return sources;
}

void GpuRenderer3D::prepare_particle_casts(FrameCtx& ctx, std::span<const ParticleSource> sources) {
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
    // Casting uses a separate bounded stream, including off-camera sprites.
    // The colour stream below retains its existing cull/order and counters.
    const bool maps = ctx.shadows || ctx.moon_cascades > 0 || !_local_views.empty();
    if (maps && _capabilities.particle_shadows) {

        for (const ParticleSource& src : sources) {
            if (!src.look->casts_shadow || !src.count)
                continue;
            const u32 room = max_particles_a_frame - static_cast<u32>(_particle_cast_sprites.size());
            const u32 count = std::min(src.count, room);
            stats.particle_shadow_dropped += src.count - count;
            if (!count)
                continue;
            ParticleCastBatch batch;
            batch.look = *src.look;
            batch.first = static_cast<u32>(_particle_cast_sprites.size());
            batch.count = count;
            Vec3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (u32 i = 0; i < count; ++i) {
                const auto& p = src.sprites[i];
                _particle_cast_sprites.push_back(p);
                const f32 r =
                    p.size * .75f +
                    (src.look->facing == ParticleFacing3D::Stretched ? length(particle_streak(p, *src.look)) : 0.0f);
                lo = {std::min(lo.x, p.position.x - r), std::min(lo.y, p.position.y - r),
                      std::min(lo.z, p.position.z - r)};
                hi = {std::max(hi.x, p.position.x + r), std::max(hi.y, p.position.y + r),
                      std::max(hi.z, p.position.z + r)};
            }
            batch.centre = (lo + hi) * .5f;
            batch.radius = length(hi - lo) * .5f;

            _particle_cast_batches.push_back(std::move(batch));
        }
        if (!_particle_cast_sprites.empty()) {
            _particle_cast_idle = 0;

            // Cache keys need culled contents, but cached maps need neither
            // packed sprites nor a GPU upload. Packing follows cache selection.
            if (ctx.shadows) {
                if (ctx.cascades > 1)
                    for (u32 c = 0; c < ctx.cascades; ++c)
                        build_particle_cast_view(ctx, ctx.plan.cascade[c].view_projection, false);
                else
                    build_particle_cast_view(ctx, ctx.light_vp, false);
            }
            for (const auto& view : _local_views)
                if (view.wanted)
                    build_particle_cast_view(ctx, view.view_projection, false);
            stats.particle_shadow_submitted = static_cast<u32>(_particle_cast_sprites.size());
        }
    } else if (maps && !_capabilities.particle_shadows &&
               std::any_of(sources.begin(), sources.end(),
                           [](const auto& s) { return s.look->casts_shadow && s.count; })) {
        warn_once(_particle_shadows_warned, "3D: particle shadows are unavailable: particle casting disabled");
    }
}

u32 GpuRenderer3D::select_particles(FrameCtx& ctx, std::span<const ParticleSource> sources) {
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
    const bool maps = ctx.shadows || ctx.moon_cascades > 0 || !_local_views.empty();
    const GpuFrustum& frustum = ctx.frustums[0];
    const Vec3f eye = scene.camera.position;
    const Vec3f forward = scene.camera.forward();
    _particle_order.clear();
    _particle_source.clear();
    u32 submitted = 0, weather_count = 0, dropped = 0;
    for (u32 source = 0; source < sources.size(); ++source) {
        const auto& src = sources[source];
        submitted += src.count;
        weather_count += src.weather ? src.count : 0u;
        ParticleBatch batch;
        batch.look = *src.look;
        batch.texture = src.look->texture;
        batch.source = src.history;
        u64 look_key = 1469598103934665603ull;
        const auto& l = *src.look;
        taa_hash(look_key, l.facing);
        taa_hash(look_key, l.axis);
        taa_hash(look_key, l.stretch);
        taa_hash(look_key, l.stretch_min);
        taa_hash(look_key, l.softness);
        taa_hash(look_key, l.ring);
        taa_hash(look_key, l.atlas_columns);
        taa_hash(look_key, l.atlas_rows);
        taa_hash(look_key, l.atlas_frames);
        taa_hash(look_key, l.blend_frames);
        taa_hash(look_key, l.blend);
        taa_hash(look_key, l.lit);
        taa_hash(look_key, l.receives_shadow);
        const bool receiving_maps = l.receives_shadow && l.lit > 0 && maps && _capabilities.particle_shadows;
        taa_hash(look_key, receiving_maps);
        taa_hash(look_key, l.fogged);
        taa_hash(look_key, l.soft);
        taa_hash(look_key, l.soft_distance);
        // Lighting changes are resolved by colour/shadow rejection per pixel;
        // they never change a sprite's geometric history identity.
        batch.unstable = taa_texture_hash(look_key, l.texture.backend().get());
        batch.look_key = look_key;
        if (ctx.taa) {
            taa_hash(ctx.coverage_key, batch.source);
            taa_hash(ctx.coverage_key, look_key);
        }
        batch.first = static_cast<u32>(_particle_order.size());
        _particle_keys.clear();
        f64 depth_sum = 0.0;
        const bool stretched = src.look->facing == ParticleFacing3D::Stretched;
        for (u32 j = 0; j < src.count; ++j) {
            const ParticleSprite3D& p = src.sprites[j];
            // The quad's reach: its width, and for a streak its length too (a sphere that holds both).
            f32 radius = p.size * 0.75f;
            if (stretched) {
                radius += length(particle_streak(p, *src.look));
            }
            if (scene.cull && !sphere_in(frustum, p.position, radius)) {
                continue;
            }
            const f32 key = dot(p.position - eye, forward);
            _particle_keys.push_back({key, j});
            depth_sum += static_cast<f64>(key);
        }
        if (_particle_keys.empty()) {
            continue;
        }
        const u32 room = max_particles_a_frame > _particle_order.size()
                             ? max_particles_a_frame - static_cast<u32>(_particle_order.size())
                             : 0u;
        if (_particle_keys.size() > room) {
            dropped += static_cast<u32>(_particle_keys.size()) - room;
            _particle_keys.resize(room);
            if (room == 0) {
                continue;
            }
        }
        if (src.look->sort && src.look->blend != ParticleBlend3D::Additive) {
            // Furthest first; equal depths in the order they were given (a stable radix sort on the depth's bits: three
            // passes of 11 bits, O(n), and the same order every run).
            radix_sort_far_first(_particle_keys, _particle_scratch);
        }
        for (const auto& [key, j] : _particle_keys) {
            _particle_order.push_back(j);
        }
        batch.count = static_cast<u32>(_particle_keys.size());
        batch.depth = static_cast<f32>(depth_sum / static_cast<f64>(_particle_keys.size()));
        batch.soft = src.look->soft && src.look->soft_distance > 0.0f;
        _particle_batches.push_back(std::move(batch));
        _particle_source.push_back(src.sprites);
    }
    const u32 total = static_cast<u32>(_particle_order.size());
    if (dropped > 0) {
        warn_once(_particles_overflow_warned, "3D: more particles than a frame draws: the rest are dropped");
    }
    stats.particles_submitted = submitted;
    stats.weather_particles = weather_count;
    stats.particles_dropped = dropped;
    stats.particles_drawn = total;
    return total;
}

void GpuRenderer3D::match_particles(FrameCtx& ctx, u32 total) {
    const Scene3D& scene = ctx.scene;
    _particle_taa_idle = 0;
    constexpr u32 history_budget = 65536;
    auto& last = ctx.view->_taa_particles;
    const std::size_t slots = std::bit_ceil(std::max(std::size_t{2}, last.size() * 2));
    _particle_history_lookup.assign(slots, ~0u);
    const auto slot_of = [&](u64 source, u64 id) { return std::size_t(mix64(source ^ mix64(id))) & (slots - 1); };
    for (u32 i = 0; i < last.size(); ++i) {
        auto slot = slot_of(last[i].source, last[i].id);
        while (_particle_history_lookup[slot] != ~0u) {
            const auto& found = last[_particle_history_lookup[slot]];
            if (found.source == last[i].source && found.id == last[i].id)
                break;
            slot = (slot + 1) & (slots - 1);
        }
        if (_particle_history_lookup[slot] == ~0u)
            _particle_history_lookup[slot] = i;
    }
    const auto find = [&](u64 source, u64 id) -> const GpuView3D::ParticleHistory* {
        auto slot = slot_of(source, id);
        while (_particle_history_lookup[slot] != ~0u) {
            const auto& record = last[_particle_history_lookup[slot]];
            if (record.source == source && record.id == id)
                return &record;
            slot = (slot + 1) & (slots - 1);
        }
        return nullptr;
    };
    _particle_history_now.clear();
    struct Candidate {
        f32 distance;
        u32 batch, original;
    };
    std::vector<Candidate> candidates;
    if (total > history_budget)
        candidates.reserve(total);
    ensure_buffer(_particle_previous, total * 64u, min_particle_buffer);
    const std::function<void(std::span<u8>)> previous = [&](std::span<u8> out) {
        u32 at = 0;
        for (u32 b = 0; b < _particle_batches.size(); ++b) {
            const auto& batch = _particle_batches[b];
            for (u32 n = 0; n < batch.count; ++n, ++at) {
                const u32 original = _particle_order[batch.first + n];
                const auto& p = _particle_source[b][original];
                const u64 id = p.id ? p.id : u64(original) + 1;
                const auto* before = find(batch.source, id);
                const auto quad = particle_quad(p, batch.look, scene.camera);
                const bool known =
                    !ctx.taa_reset && before && before->look == batch.look_key && !batch.unstable &&
                    std::abs(before->alpha - p.alpha) < .15f && std::abs(before->frame - p.frame) < 1 &&
                    (batch.look.blend_frames || std::floor(before->frame + .5f) == std::floor(p.frame + .5f)) &&
                    length_squared(before->centre - p.position) < std::max(1.f, p.size * p.size * 16) &&
                    std::abs(before->colour.r - p.colour.r) < .15f && std::abs(before->colour.g - p.colour.g) < .15f &&
                    std::abs(before->colour.b - p.colour.b) < .15f;
                f32 data[4][4];
                for (u32 c = 0; c < 4; ++c) {
                    const auto corner = known ? before->quad.corner[c] : quad.corner[c];
                    data[c][0] = corner.x;
                    data[c][1] = corner.y;
                    data[c][2] = corner.z;
                    data[c][3] = known ? 0.f : 1.f;
                }
                std::memcpy(out.data() + std::size_t(at) * 64, data, 64);
                const GpuView3D::ParticleHistory record{batch.source, id,       batch.look_key, quad,
                                                        p.position,   p.colour, p.alpha,        p.frame};
                if (total <= history_budget)
                    _particle_history_now.push_back(record);
                else
                    candidates.push_back({length_squared(p.position - scene.camera.position), b, original});
            }
        }
    };
    _device->upload_storage_buffer(_particle_previous->handle(), 0, total * 64u, nullptr, true, &previous);
    if (!candidates.empty()) {
        const auto less = [](const Candidate& a, const Candidate& b) {
            return std::tuple{a.distance, a.batch, a.original} < std::tuple{b.distance, b.batch, b.original};
        };
        std::nth_element(candidates.begin(), candidates.begin() + history_budget, candidates.end(), less);
        for (u32 i = 0; i < history_budget; ++i) {
            const auto c = candidates[i];
            const auto& batch = _particle_batches[c.batch];
            const auto& sprite = _particle_source[c.batch][c.original];
            _particle_history_now.push_back({batch.source, sprite.id ? sprite.id : u64(c.original) + 1, batch.look_key,
                                             particle_quad(sprite, batch.look, scene.camera), sprite.position,
                                             sprite.colour, sprite.alpha, sprite.frame});
        }
    }
}

void GpuRenderer3D::upload_particles(FrameCtx& ctx, u32 total) {
    const u32 bytes = total * static_cast<u32>(sizeof(GpuParticle3D));
    const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
        u32 at = 0;
        for (std::size_t b = 0; b < _particle_batches.size(); ++b) {
            const ParticleBatch& batch = _particle_batches[b];
            const ParticleSprite3D* src = _particle_source[b];
            for (u32 n = 0; n < batch.count; ++n) {
                const ParticleSprite3D& p = src[_particle_order[batch.first + n]];
                const GpuParticle3D g = pack_particle(p);
                if (ctx.taa)
                    taa_hash(ctx.coverage_key, g);
                std::memcpy(out.data() + static_cast<std::size_t>(at) * sizeof(GpuParticle3D), &g, sizeof(g));
                ++at;
            }
        }
    };
    _device->upload_storage_buffer(_particle_buffer->handle(), 0, bytes, nullptr, /*cycle=*/true, &fill);
}

void GpuRenderer3D::order_particle_batches() {
    // Drawn far to near by the batch's mean depth (the instances stay where they were written); equal depths in
    // submission order.
    std::vector<u32> by_depth(_particle_batches.size());
    for (u32 b = 0; b < by_depth.size(); ++b) {
        by_depth[b] = b;
    }
    std::stable_sort(by_depth.begin(), by_depth.end(),
                     [&](u32 a, u32 b) { return _particle_batches[a].depth > _particle_batches[b].depth; });
    std::vector<ParticleBatch> ordered;
    ordered.reserve(by_depth.size());
    for (const u32 b : by_depth) {
        ordered.push_back(std::move(_particle_batches[b]));
    }
    _particle_batches = std::move(ordered);
}

void GpuRenderer3D::prepare_particles(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
    _particle_batches.clear();
    _particle_history_now.clear();
    if (++_particle_taa_idle >= _release_after) {
        _particle_previous.reset();
        std::vector<u32>().swap(_particle_history_lookup);
        decltype(_particle_history_now)().swap(_particle_history_now);
        _particle_taa_idle = _release_after;
    }
    _particle_cast_batches.clear();
    _particle_cast_views.clear();
    _particle_cast_sprites.clear();
    _particle_cast_order.clear();
    if (_particle_cast_buffer && ++_particle_cast_idle >= _release_after) {
        _particle_cast_buffer.reset();
        std::vector<ParticleSprite3D>().swap(_particle_cast_sprites);
        std::vector<u32>().swap(_particle_cast_order);
    }
    stats.particle_shadow_bytes = _particle_cast_buffer ? _particle_cast_buffer->size() : 0;
    ctx.particles = false;
    if (scene.particle_draws().empty() && !scene.weather.active()) {
        ctx.view->_taa_particles.clear();
        return;
    }
    if (!_particles_available) {
        warn_once(_particles_warned,
                  "3D: particles asked for, but this device's shaders for them were not made: not drawn");
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    prepare_weather(ctx);
    const auto sources = particle_sources(scene);
    prepare_particle_casts(ctx, sources);
    const u32 total = select_particles(ctx, sources);
    if (total == 0) {
        ctx.view->_taa_particles.clear();
        stats.particle_cpu_ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
        return;
    }
    const u32 bytes = total * static_cast<u32>(sizeof(GpuParticle3D));
    ensure_buffer(_particle_buffer, bytes, min_particle_buffer);
    if (ctx.taa)
        match_particles(ctx, total);
    upload_particles(ctx, total);
    order_particle_batches();
    ctx.particles = true;
    stats.particle_cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
}

SDL_GPUGraphicsPipeline* GpuRenderer3D::particle_cast_pipeline() const {
    if (!_capabilities.particle_shadows || !_particle_cast_shader)
        return nullptr;
    return _owner->_pipelines.get(GpuPipelineKey{.vertex = _particle_vertex_shader.handle(),
                                                 .fragment = _particle_cast_shader.handle(),
                                                 .blend = GpuBlendMode::Replace,
                                                 .layout = GpuVertexLayout::None,
                                                 .depth_stencil = _shadow_format,
                                                 .depth = GpuDepthTest::TestWrite,
                                                 .cull = GpuCull::None,
                                                 .depth_only = true});
}

const GpuRenderer3D::ParticleCastView* GpuRenderer3D::particle_cast_view(const Mat4f& matrix) const {
    for (const auto& view : _particle_cast_views)
        if (std::memcmp(view.matrix.m, matrix.m, sizeof(matrix.m)) == 0)
            return &view;
    return nullptr;
}

const GpuRenderer3D::ParticleCastView& GpuRenderer3D::build_particle_cast_view(FrameCtx& ctx, const Mat4f& matrix,
                                                                               bool pack) {
    auto* found = const_cast<ParticleCastView*>(particle_cast_view(matrix));
    if (found && (!pack || found->packed))
        return *found;
    // Every map is registered before cache decisions and before any upload.
    // Draw-time lookup can never append a new view or grow the packed stream.
    auto& view = found ? *found : _particle_cast_views.emplace_back();
    view.matrix = matrix;
    view.packed = pack;
    const auto frustum = frustum_of(matrix);
    // A hard cap per map, including each point-light face. Selection is stable
    // submission order and independent of camera visibility or colour budgets.
    constexpr u32 budget = 16384;
    u32 used = 0, dropped = 0;
    u64 key = 1469598103934665603ull;
    for (u32 b = 0; b < _particle_cast_batches.size(); ++b) {
        const auto& batch = _particle_cast_batches[b];
        if (!sphere_in(frustum, batch.centre, batch.radius))
            continue;
        const auto& l = batch.look;
        bool selected = false;
        for (u32 i = 0; i < batch.count; ++i) {
            const auto& p = _particle_cast_sprites[batch.first + i];
            const f32 radius =
                p.size * .75f + (l.facing == ParticleFacing3D::Stretched ? length(particle_streak(p, l)) : 0.f);
            if (!sphere_in(frustum, p.position, radius))
                continue;
            if (used == budget) {
                ++dropped;
                continue;
            }
            selected = true;
            if (pack) {
                if (view.ranges.empty() || view.ranges.back().batch != b)
                    view.ranges.push_back({b, static_cast<u32>(_particle_cast_order.size()), 0});
                ++view.ranges.back().count;
                _particle_cast_order.push_back(batch.first + i);
            }
            ++used;
            taa_hash(key, particle_quad(p, l, ctx.scene.camera));
            taa_hash(key, p.alpha);
            taa_hash(key, p.velocity);
            taa_hash(key, p.frame);
        }
        if (!selected)
            continue;
        taa_hash(key, l.facing);
        taa_hash(key, l.axis);
        taa_hash(key, l.stretch);
        taa_hash(key, l.stretch_min);
        taa_hash(key, l.softness);
        taa_hash(key, l.ring);
        taa_hash(key, l.shadow_cutoff);
        taa_hash(key, l.atlas_columns);
        taa_hash(key, l.atlas_rows);
        taa_hash(key, l.atlas_frames);
        taa_hash(key, l.blend_frames);
        view.unstable |= taa_texture_hash(key, l.texture.backend().get());
    }
    if (!pack)
        ctx.stats.particle_shadow_map_dropped += dropped;
    view.key = used ? key : 0;
    return view;
}

void GpuRenderer3D::upload_particle_casts(FrameCtx& ctx) {
    if (_particle_cast_batches.empty())
        return;
    if (ctx.shadows && !ctx.sun_shadow_cached) {
        if (ctx.cascades > 1)
            for (u32 c = 0; c < ctx.cascades; ++c)
                build_particle_cast_view(ctx, ctx.plan.cascade[c].view_projection, true);
        else
            build_particle_cast_view(ctx, ctx.light_vp, true);
    }
    for (const auto& view : _local_views)
        if (view.drawn)
            build_particle_cast_view(ctx, view.view_projection, true);
    const auto count = u32(_particle_cast_order.size());
    if (!count)
        return;
    const auto bytes = count * u32(sizeof(GpuParticle3D));
    ensure_buffer(_particle_cast_buffer, bytes, min_particle_buffer);
    const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
        for (u32 i = 0; i < count; ++i) {
            const auto packed = pack_particle(_particle_cast_sprites[_particle_cast_order[i]]);
            std::memcpy(out.data() + std::size_t(i) * sizeof(packed), &packed, sizeof(packed));
        }
    };
    _device->upload_storage_buffer(_particle_cast_buffer->handle(), 0, bytes, nullptr, true, &fill);
    ctx.stats.particle_shadow_uploaded_bytes = bytes;
    ctx.stats.particle_shadow_bytes = _particle_cast_buffer->size();
}

bool GpuRenderer3D::draw_particle_shadows(FrameCtx& ctx, SDL_GPURenderPass* pass, const Mat4f& light_view, bool local,
                                          u32 cascade) {
    if (_particle_cast_batches.empty() || !_particle_cast_buffer)
        return true;
    auto* pipeline = particle_cast_pipeline();
    if (!pipeline)
        return false;
    const auto* selected = particle_cast_view(light_view);
    if (!selected)
        return false;
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    auto* storage = _particle_cast_buffer->handle();
    SDL_BindGPUVertexStorageBuffers(pass, 0, &storage, 1);
    SDL_PushGPUVertexUniformData(ctx.commands, 1, _fog_table.values.data(), sizeof(_fog_table.values));
    const auto& camera = ctx.scene.camera;
    ParticleDrawUniforms v{};
    std::memcpy(v.view_projection, light_view.m, sizeof(v.view_projection));
    put(v.right, camera.right(), 0);
    put(v.up, camera.up(), 0);
    put(v.eye, camera.position, camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f);
    put(v.forward, camera.forward(), 0);
    for (const auto& range : selected->ranges) {
        const auto& batch = _particle_cast_batches[range.batch];
        const auto& l = batch.look;
        const auto axis = length_squared(l.axis) > 0 ? normalize(l.axis) : Vec3f{0, 1, 0};
        put(v.axis, axis, static_cast<f32>(static_cast<u32>(l.facing)));
        const u32 columns = std::max(l.atlas_columns, 1u), rows = std::max(l.atlas_rows, 1u);
        const u32 frames = l.atlas_frames > 0 ? std::min(l.atlas_frames, columns * rows) : columns * rows;
        put(v.atlas, {f32(columns), f32(rows), f32(frames)}, l.blend_frames ? 1.0f : 0.0f);
        put(v.stretch, {l.stretch, l.stretch_min, 0}, 0);
        v.first[0] = range.first;
        SDL_PushGPUVertexUniformData(ctx.commands, 0, &v, sizeof(v));
        const auto picture = texture_binding(l.texture, Wrap3D::Clamp, Wrap3D::Clamp);
        ParticleFragUniforms f{};
        f.flags[3] = picture.texture != _owner->_white.handle() ? 1.0f : 0.0f;
        f.disc[0] = l.softness;
        f.disc[1] = l.ring;
        f.disc[3] = std::isfinite(l.shadow_cutoff) ? std::clamp(l.shadow_cutoff, 0.0f, 1.0f) : .5f;
        SDL_PushGPUFragmentUniformData(ctx.commands, 0, &f, sizeof(f));
        SDL_BindGPUFragmentSamplers(pass, 0, &picture, 1);
        SDL_DrawGPUPrimitives(pass, 6, range.count, 0, 0);
        ++ctx.stats.particle_shadow_draws;
        ctx.stats.particle_shadow_instances += range.count;
        if (local) {
            ++ctx.stats.local_shadow_draws;
            ctx.stats.local_shadow_casters += range.count;
            ctx.stats.local_shadow_triangles += u64(range.count) * 2;
        } else {
            ++ctx.stats.shadow_draws;
            ctx.stats.shadow_casters += range.count;
            ctx.stats.cascade_casters[cascade] += range.count;
        }
    }
    return true;
}

void GpuRenderer3D::draw_particles(FrameCtx& ctx, SDL_GPURenderPass* pass, bool reactive) {
    const GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    if (_particle_batches.empty() || !_particle_buffer) {
        return;
    }
    const bool orthographic = scene.camera.projection == Projection3D::Orthographic;
    const bool encode = !ctx.taa && view->_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    const bool decode = _owner->_color_texture_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    const bool depth_ready = ctx.prepass_ran && view->_ao_depth && _soft_particles_available;
    const LinearRgb light_tint = particle_light_tint(scene.lights);
    const Camera3D& camera = scene.camera;
    const Unprojection un = unproject_coefficients(camera.projection_matrix(ctx.aspect));

    const bool any_local =
        (!ctx.chosen.empty() || ctx.lu.counts[0] > 0) &&
        std::any_of(_particle_batches.begin(), _particle_batches.end(), [](const auto& b) { return b.look.lit > 0; });
    const bool maps =
        ctx.shadows || ctx.moon_cascades > 0 || (ctx.lights_variant && _local_map && !_local_views.empty());
    const bool wants_shadows = std::any_of(_particle_batches.begin(), _particle_batches.end(),
                                           [](const auto& b) { return b.look.receives_shadow && b.look.lit > 0; });
    if (wants_shadows && !_capabilities.particle_shadows)
        warn_once(_particle_shadows_warned, "3D: particle shadow reception is unavailable: unshadowed particles");
    const bool shadowed = wants_shadows && maps && _capabilities.particle_shadows;
    ParticleLightUniforms local_lights{};
    local_lights.counts[0] = local_lights.counts[1] = 0;
    if (any_local) {
        local_lights = {};
        local_lights.counts[0] = static_cast<f32>(ctx.chosen.size());
        local_lights.counts[1] = ctx.lu.counts[0];
        for (std::size_t i = 0; i < ctx.chosen.size(); ++i) {
            const auto& light = *ctx.chosen[i].second;
            put(local_lights.points + i * 8, light.position, light.radius);
            const f32 size = std::max(light.size, .01f);
            put_color(local_lights.points + i * 8 + 4, light.color, size * size);
        }
        std::copy(std::begin(ctx.lu.spots), std::end(ctx.lu.spots), std::begin(local_lights.spots));
    }
    ParticleDrawUniforms v{};
    std::memcpy(v.view_projection, ctx.drawn[0].m, sizeof(v.view_projection));
    put(v.right, camera.right(), 0.0f);
    put(v.up, camera.up(), 0.0f);
    put(v.eye, camera.position, orthographic ? 1.0f : 0.0f);
    put(v.forward, camera.forward(), 0.0f);
    put(v.sun_direction, normalize(scene.lights.sun.direction), 0.0f);
    put_color(v.sun_light, scene.lights.sun.color, 0.0f);
    if (ctx.fog) {
        fill_fog(v.fog, scene.fog, scene.sky, orthographic);
        fill_atmosphere(v.sky, scene.sky, scene.lights.sun.direction);
        if (v.fog.colour[3] > 0.5f)
            _fog_table.update(scene.sky, scene.lights.sun.direction);
    }
    SDL_PushGPUVertexUniformData(commands, 1, _fog_table.values.data(), sizeof(_fog_table.values));
    const SDL_GPUTextureFormat format = ctx.taa ? hdr_format : view->_format;
    const u32 samples = ctx.taa ? 1u : view->_samples;
    SDL_GPUGraphicsPipeline* bound = nullptr;
    SDL_GPUBuffer* storage = _particle_buffer->handle();
    if (reactive) {
        const std::array<Mat4f, 2> temporal{ctx.view_projections[0], ctx.view->_taa_prior[0]};
        SDL_PushGPUVertexUniformData(commands, 3, temporal.data(), sizeof(temporal));
    }
    u32 draws = 0, soft_draws = 0, shadow_draws = 0;
    for (const ParticleBatch& batch : _particle_batches) {
        const bool soft = batch.soft && depth_ready;
        const ParticleLook3D& look = batch.look;
        const bool receive = shadowed && look.receives_shadow && look.lit > 0;
        const bool lit_locally = any_local && look.lit > 0 && !receive;
        SDL_GPUGraphicsPipeline* p = particle_pipeline(format, samples, batch.look.blend, soft,
                                                       ctx.main_velocity && !reactive, reactive, lit_locally, receive);
        if (!p) {
            continue;
        }
        if (p != bound) {
            SDL_BindGPUGraphicsPipeline(pass, p);
            const std::array<SDL_GPUBuffer*, 2> buffers{storage, reactive ? _particle_previous->handle() : nullptr};
            SDL_BindGPUVertexStorageBuffers(pass, 0, buffers.data(), reactive ? 2 : 1);
            bound = p;
        }
        if (receive) {
            push_frame_uniforms(ctx, 0, 1, !reactive);
            push_local_lights(ctx, 0, 2);
        }
        if (lit_locally || reactive) {
            local_lights.counts[2] = lit_locally ? std::clamp(look.lit, 0.0f, 1.0f) : 0;
            SDL_PushGPUVertexUniformData(commands, 2, &local_lights, sizeof(local_lights));
        }
        const Vec3f axis = length_squared(look.axis) > 0.0f ? normalize(look.axis) : Vec3f{0.0f, 1.0f, 0.0f};
        put(v.axis, axis, static_cast<f32>(static_cast<u32>(look.facing)));
        const u32 columns = std::max(look.atlas_columns, 1u), rows = std::max(look.atlas_rows, 1u);
        const u32 frames = look.atlas_frames > 0 ? std::min(look.atlas_frames, columns * rows) : columns * rows;
        put(v.atlas, {static_cast<f32>(columns), static_cast<f32>(rows), static_cast<f32>(frames)},
            look.blend_frames ? 1.0f : 0.0f);
        put(v.stretch, {look.stretch, look.stretch_min, 0.0f}, 0.0f);
        v.first[0] = batch.first;
        v.first[1] = ctx.fog && look.fogged ? 1u : 0u;
        SDL_PushGPUVertexUniformData(commands, 0, &v, sizeof(v));
        const SDL_GPUTextureSamplerBinding picture = texture_binding(batch.texture, Wrap3D::Clamp, Wrap3D::Clamp);
        const bool textured = picture.texture != _owner->_white.handle();
        ParticleFragUniforms f{};
        const LinearRgb tint = mix(LinearRgb{1.0f, 1.0f, 1.0f}, light_tint, std::clamp(look.lit, 0.0f, 1.0f));
        put_color(f.tint, tint, reactive && (receive || lit_locally) ? (ctx.shadow_changed ? 1.f : 0.f) : 1.f);
        put(f.flags, {encode ? 1.0f : 0.0f, decode ? 1.0f : 0.0f, static_cast<f32>(static_cast<u32>(look.blend))},
            textured ? 1.0f : 0.0f);
        put(f.disc, {look.softness, look.ring, soft ? look.soft_distance : 0.0f},
            receive ? std::clamp(look.lit, 0.0f, 1.0f) : 0.0f);
        std::memcpy(f.unproject, un.zw, sizeof(f.unproject));
        SDL_PushGPUFragmentUniformData(commands, 0, &f, sizeof(f));
        const std::array<SDL_GPUTextureSamplerBinding, 2> samplers{
            picture,
            SDL_GPUTextureSamplerBinding{soft ? view->_ao_depth.handle() : _owner->_white.handle(), _point_sampler}};
        if (receive) {
            const std::array<SDL_GPUTextureSamplerBinding, 5> shadow_samplers{
                samplers[0], samplers[1],
                SDL_GPUTextureSamplerBinding{ctx.shadows ? _shadow_map.handle() : _no_shadow.handle(),
                                             _compare_sampler},
                SDL_GPUTextureSamplerBinding{ctx.lights_variant && _local_map && !_local_views.empty()
                                                 ? _local_map.handle()
                                                 : _no_shadow.handle(),
                                             _compare_sampler},
                SDL_GPUTextureSamplerBinding{
                    ctx.moon_cascades > 0 && _moon_map ? _moon_map.handle() : _no_shadow.handle(), _compare_sampler}};
            SDL_BindGPUFragmentSamplers(pass, 0, shadow_samplers.data(), 5);
        } else {
            SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), soft ? 2u : 1u);
        }
        SDL_DrawGPUPrimitives(pass, 6, batch.count, 0, 0);
        ++draws;
        soft_draws += soft ? 1u : 0u;
        shadow_draws += receive ? 1u : 0u;
    }
    if (reactive) {
        stats.taa_reactive_draws += draws;
    } else {
        stats.particle_batches = draws;
        stats.particle_soft = soft_draws;
        stats.particle_shadow_batches = shadow_draws;
        stats.draws += draws;
    }
}

void GpuRenderer3D::resume_2d() {
    _owner->_batch.begin(_owner->current_target(), _owner->_clear_color, /*do_clear=*/false);
}

SDL_GPUTextureSamplerBinding GpuRenderer3D::texture_binding(const Texture& texture, Wrap3D wrap_u, Wrap3D wrap_v) {
    const ITextureBackend* backend = texture.backend().get();
    const auto* gpu = backend && backend->kind() == ITextureBackend::Kind::Gpu
                          ? static_cast<const GpuTextureBackend*>(backend)
                          : nullptr;
    return gpu && gpu->texture()
               ? SDL_GPUTextureSamplerBinding{gpu->texture().handle(), sampler_for(*gpu, wrap_u, wrap_v)}
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
    return m.wireframe                                              ? Pass::Wire
           : m.blend == Blend3D::Opaque || m.blend == Blend3D::Mask ? Pass::Opaque
           : m.blend == Blend3D::Alpha                              ? Pass::Alpha
                                                                    : Pass::Additive;
}

namespace {

// Whether a scene has a particle batch that asks for soft particles (its own, or an active weather layer's).
bool scene_has_soft_particles(const Scene3D& scene) {
    for (const Scene3D::ParticleDraw& d : scene.particle_draws()) {
        if (d.look.soft && d.look.soft_distance > 0.0f) {
            return true;
        }
    }
    const Weather3D& w = scene.weather;
    for (const WeatherLayer3D* layer : {&w.rain, &w.snow, &w.dust, &w.mist}) {
        if (layer->intensity > 0.0f && layer->look.soft && layer->look.soft_distance > 0.0f) {
            return true;
        }
    }
    return false;
}

} // namespace

void GpuRenderer3D::setup_frame(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
    ctx.aspect = static_cast<f32>(view->_size.x) / static_cast<f32>(view->_size.y);
    const Scene3D* overlay = scene.overlay_if_any();
    ctx.scenes = {&scene, overlay && !overlay->draws().empty() ? overlay : nullptr};
    for (u32 s = 0; s < 2; ++s) {
        if (ctx.scenes[s]) {
            ctx.view_projections[s] = ctx.scenes[s]->camera.view_projection(ctx.aspect);
            ctx.frustums[s] = frustum_of(ctx.view_projections[s]);
        }
    }
    // Every pass of this render, the overlay's too, reads mipmapped textures with the scene's anisotropy and mip bias.
    _anisotropy = std::clamp(scene.anisotropy, 1u, _capabilities.max_anisotropy);
    _lod_bias = _capabilities.texture_lod_bias && std::isfinite(scene.texture_lod_bias)
                    ? static_cast<i32>(std::lround(std::clamp(scene.texture_lod_bias, -2.0f, 2.0f) * 16.0f))
                    : 0;

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
    ctx.sun_shines = scene.lights.sun.color.r + scene.lights.sun.color.g + scene.lights.sun.color.b > 0.0f;
    // Contact shadows are the sun's shadow: they fade with sun_shadow_strength (in the shader, through the lights
    // variant), and at 0 there is nothing to march for.
    ctx.contact_wanted = scene.contact_shadows.enabled && ctx.sun_shines && scene.lights.sun_shadow_strength > 0.0f;
    // The velocity comes from the main pass when nothing else needs a pre-pass (prepass() decides, as it knows the
    // frame's draws); the pre-pass's targets are made here only for what does.
    _gpu_markers = scene.debug.gpu_pass_markers;
    // Soft particles read the pre-pass's depth: it is made for them where nothing else needs it (and the velocity then
    // comes from it).
    ctx.soft_particles = _soft_particles_available && _ao_available && scene_has_soft_particles(scene);
    // Global illumination's probes are read on what the pre-pass drew (gi_resolve).
    const bool gi_wanted =
        scene.global_illumination.enabled && scene.global_illumination.probes.enabled && _gi_available;
    const bool needs_prepass = scene.ambient_occlusion.enabled || ctx.contact_wanted || scene.lod.occlusion.enabled ||
                               ctx.soft_particles || gi_wanted;
    if (ctx.taa &&
        !(ensure_taa_targets(*view, taa_settings.sharpness > 0.0f) &&
          (needs_prepass ? ensure_ao_targets(*view, scene.ambient_occlusion.half_resolution,
                                             scene.ambient_occlusion.enabled, ctx.contact_wanted, /*velocity=*/true)
                         : ensure_velocity_target(*view)))) {
        ctx.taa = false;
    }
    if (!ctx.taa) {
        suspend_taa(*view);
    }
    ctx.drawn = ctx.view_projections;
    if (ctx.taa) {
        const u32 period = std::clamp(taa_settings.jitter_samples, 1u, max_taa_jitter_samples);
        const Vec2f raw = taa_jitter(scene.debug.freeze_jitter ? 0u : view->_taa_frame++, period);
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
    const Bounds3D& area = lights.shadow_area;
    const Vec3f area_size = area.size();
    ctx.cascades = 0;
    ctx.shadows = ctx.sun_shines && lights.sun_shadow_strength > 0.0f && area_size.x > 0 && area_size.y >= 0 &&
                  area_size.z > 0 && length_squared(lights.sun.direction) > 0 &&
                  (ctx.cascades = ensure_shadow_map(lights.shadow_resolution, lights.shadow_cascades)) > 0;
    if (ctx.shadows && ctx.cascades > 1) {
        ctx.plan = fit_shadow_cascades(scene.camera, ctx.aspect, normalize(lights.sun.direction), area, ctx.cascades,
                                       lights.shadow_distance, _shadow_size);
        ctx.cascades = ctx.plan.count;
        for (u32 k = 0; k < ctx.cascades; ++k) {
            ctx.cascade_frustums[k] = frustum_of(ctx.plan.cascade[k].view_projection);
            const ShadowCascade3D& c = ctx.plan.cascade[k];
            ctx.slice_frustums[k] =
                frustum_of(slice_of(scene.camera, c.slice_near, c.slice_far).view_projection(ctx.aspect));
        }
    } else if (ctx.shadows) {
        const SunBox3D box = fit_sun_box(area, normalize(lights.sun.direction));
        ctx.light_vp =
            Mat4f::orthographic(box.lo.x, box.hi.x, box.lo.y, box.hi.y, box.z_near, box.z_far) * box.light_view;
        ctx.light_frustum = frustum_of(ctx.light_vp);
        ctx.texel_world = std::max(box.hi.x - box.lo.x, box.hi.y - box.lo.y) / static_cast<f32>(_shadow_size);
        ctx.depth_range = std::max(box.z_far - box.z_near, 0.01f);
    }
}

bool GpuRenderer3D::ensure_local_map(u32 side, u32 spot_layers, u32 point_slots) {
    if (_shadow_format == SDL_GPU_TEXTUREFORMAT_INVALID || spot_layers + point_slots == 0) {
        return false;
    }
    side = std::clamp(side, 64u, 4096u);
    spot_layers = std::min(spot_layers, max_spot_shadow_maps);
    point_slots = std::min(point_slots, max_point_shadow_maps);
    // The layers: the spots' first, then six a point light's. Only ever more of either at one side (a light going out
    // of view does not shrink the array, and coming back does not make it again), as the array's layout is part of what
    // the cache keeps.
    if (_local_map && _local_side == side) {
        spot_layers = std::max(spot_layers, _local_spot_layers);
        point_slots = std::max(point_slots, _local_point_slots);
        if (spot_layers == _local_spot_layers && point_slots == _local_point_slots) {
            return true;
        }
    }
    const u32 layers = spot_layers + cube_faces * point_slots;
    try {
        _local_map =
            _device->create_target_array(side, side, layers, _shadow_format,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        _local_side = side;
        _local_layers = layers;
        _local_spot_layers = spot_layers;
        _local_point_slots = point_slots;
        ++_local_generation; // a new array: nothing cached is in it
        return true;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: no shadow map for local lights",
                       (LogFields{{.name = "side", .value = std::to_string(side)},
                                  {.name = "layers", .value = std::to_string(layers)},
                                  {.name = "reason", .value = e.what()}}));
    }
    _local_map = {};
    _local_side = 0;
    _local_layers = _local_spot_layers = _local_point_slots = 0;
    ++_local_generation;
    return false;
}

u32 GpuRenderer3D::ensure_moon_map(u32 side, u32 layers) {
    if (_shadow_format == SDL_GPU_TEXTUREFORMAT_INVALID || layers == 0) {
        return 0;
    }
    side = std::clamp(side, 64u, 8192u);
    layers = std::clamp(layers, 1u, max_shadow_cascades);
    if (_moon_map && _moon_side == side && _moon_layers >= layers) {
        return _moon_layers;
    }
    const u32 want = _moon_map && _moon_side == side ? std::max(layers, _moon_layers) : layers;
    try {
        _moon_map = _device->create_target_array(
            side, side, want, _shadow_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        _moon_side = side;
        _moon_layers = want;
        ++_moon_generation;
        return want;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F(
            "render", "3D: no shadow map for the fill light",
            (LogFields{{.name = "side", .value = std::to_string(side)}, {.name = "reason", .value = e.what()}}));
    }
    _moon_map = {};
    _moon_side = 0;
    _moon_layers = 0;
    ++_moon_generation;
    return 0;
}

namespace {

// A shadow-casting light's map side: its own, or the default; at most what the array is made for.
u32 local_resolution(const auto& light, const Lights3D& lights) {
    return std::clamp(light.shadow_resolution != 0 ? light.shadow_resolution : lights.local_shadow_resolution, 64u,
                      4096u);
}

// Whether a mesh's draw casts into the extra shadow maps: as the sun's `caster` says, apart from the sun being on.
bool casts_into_local(const Material3D& m) {
    return (m.blend == Blend3D::Opaque || m.blend == Blend3D::Mask) && m.casts_shadow && !m.wireframe;
}

// Whether the view could see anything of a pyramid from `apex` out along `axis` (the face of a cube map: `tan_half` the
// tangent of its half angle, `far` its depth): the corners all behind one of the view's planes say no; conservative.
bool pyramid_in(const GpuFrustum& view, Vec3f apex, Vec3f axis, Vec3f up, f32 tan_half, f32 far) {
    const Vec3f right = normalize(cross(axis, up));
    const Vec3f u = cross(right, axis);
    const Vec3f centre = apex + axis * far;
    const f32 h = tan_half * far;
    const std::array<Vec3f, 5> corners{apex, centre + right * h + u * h, centre - right * h + u * h,
                                       centre + right * h - u * h, centre - right * h - u * h};
    for (u32 k = 0; k < view.count; ++k) {
        const Vec4f& p = view.planes[k];
        bool any_in = false;
        for (const Vec3f& c : corners) {
            if (p.x * c.x + p.y * c.y + p.z * c.z + p.w >= 0.0f) {
                any_in = true;
                break;
            }
        }
        if (!any_in) {
            return false;
        }
    }
    return true;
}

} // namespace

void GpuRenderer3D::choose_local_lights(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    const Lights3D& lights = scene.lights;
    RenderDebugStats3D& stats = ctx.stats;
    stats.spot_slot.fill(-1);
    stats.point_slot.fill(-1);
    _local_views.clear();
    ctx.spots.clear();
    ctx.lights_variant = false;
    ctx.moon_cascades = 0;
    ctx.lu = {};
    LocalLightUniforms& lu = ctx.lu;

    // The spot lights that reach the view: the strongest and nearest first, as the point lights are chosen.
    {
        std::vector<std::pair<f32, u32>> reaching;
        for (u32 i = 0; i < static_cast<u32>(lights.spots.size()); ++i) {
            const SpotLightSource3D& light = lights.spots[i];
            if (light.radius <= 0.0f || light.color.r + light.color.g + light.color.b <= 0.0f) {
                continue;
            }
            Vec3f c;
            f32 r;
            spot_bounding_sphere(light, c, r);
            if (ctx.scenes[0] && ctx.scene.cull && !sphere_in(ctx.frustums[0], c, r)) {
                continue;
            }
            const f32 strength = std::max({light.color.r, light.color.g, light.color.b});
            const f32 distance = std::max(1.0f, length(light.position - scene.camera.position) - light.radius);
            reaching.push_back({strength / (distance * distance), i});
        }
        std::stable_sort(reaching.begin(), reaching.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        reaching.resize(std::min<std::size_t>(reaching.size(), max_spot_lights));
        for (const auto& [strength, i] : reaching) {
            ctx.spots.push_back(i);
        }
        stats.spots = static_cast<u32>(ctx.spots.size());
    }

    // Which of them, and of the point lights in view, have a shadow map: the most important, kept while they stay among
    // them.
    const bool maps_possible =
        _lights_available && _shadow_format != SDL_GPU_TEXTUREFORMAT_INVALID && _shadow_shader && _shadow_vertex_shader;
    const u32 spot_cap = std::min(lights.max_shadowed_spots, max_spot_shadow_maps);
    const u32 point_cap = std::min(lights.max_shadowed_points, max_point_shadow_maps);
    const bool asked = std::any_of(lights.spots.begin(), lights.spots.end(),
                                   [](const SpotLightSource3D& l) { return l.shadow_enabled; }) ||
                       std::any_of(lights.points.begin(), lights.points.end(),
                                   [](const PointLightSource3D& l) { return l.shadow_enabled; });
    const bool moon_asked = lights.fill_shadow.enabled && lights.fill_shadow.strength > 0.0f &&
                            lights.fill.color.r + lights.fill.color.g + lights.fill.color.b > 0.0f &&
                            length_squared(lights.fill.direction) > 0.0f;
    const bool needs_variant = !ctx.spots.empty() || asked || moon_asked ||
                               (lights.sun_shadow_strength < 1.0f && lights.sun_shadow_strength > 0.0f);
    if (needs_variant && !_lights_available) {
        warn_once(_lights_warned, "3D: spot lights, local shadows or the fill light's shadow asked for, but this "
                                  "device's shaders for them were not made: off");
    }
    std::vector<i32> spot_slot_of(ctx.spots.size(), -1), point_slot_of(ctx.chosen.size(), -1);
    if (maps_possible && asked) {
        std::vector<ShadowRequest3D> requests;
        // Spots.
        _spot_slots.held.resize(spot_cap, 0);
        for (u32 k = 0; k < ctx.spots.size(); ++k) {
            const SpotLightSource3D& l = lights.spots[ctx.spots[k]];
            Vec3f c;
            f32 r;
            spot_bounding_sphere(l, c, r);
            const u64 key = (1ull << 48) | (l.id != 0 ? (1ull << 32) | l.id : static_cast<u64>(ctx.spots[k]) + 1);
            requests.push_back(
                {l.shadow_enabled ? key : 0u, shadow_importance(std::max({l.color.r, l.color.g, l.color.b}), l.radius,
                                                                c, r, scene.camera.position)});
        }
        assign_shadow_slots(_spot_slots, requests, shadow_hysteresis, spot_slot_of);
        // Point lights (those the camera reaches: `chosen`).
        requests.clear();
        _point_slots.held.resize(point_cap, 0);
        for (const auto& [strength, light] : ctx.chosen) {
            const u32 index = static_cast<u32>(light - lights.points.data());
            const u64 key = (2ull << 48) | (light->id != 0 ? (1ull << 32) | light->id : static_cast<u64>(index) + 1);
            requests.push_back(
                {light->shadow_enabled ? key : 0u,
                 shadow_importance(std::max({light->color.r, light->color.g, light->color.b}), light->radius,
                                   light->position, light->radius, scene.camera.position)});
        }
        assign_shadow_slots(_point_slots, requests, shadow_hysteresis, point_slot_of);
    } else {
        _spot_slots.held.assign(spot_cap, 0);
        _point_slots.held.assign(point_cap, 0);
    }
    bool any_spot_map = false, any_point_map = false;
    for (const i32 s : spot_slot_of) {
        any_spot_map = any_spot_map || s >= 0;
    }
    for (const i32 s : point_slot_of) {
        any_point_map = any_point_map || s >= 0;
    }
    u32 spot_layers = 0, point_slots = 0;
    for (const i32 s : spot_slot_of) {
        spot_layers = std::max(spot_layers, static_cast<u32>(s + 1));
    }
    for (const i32 s : point_slot_of) {
        point_slots = std::max(point_slots, static_cast<u32>(s + 1));
    }
    // The array's side: the largest map of the lights that have one this render, not of every light that asks (one out
    // of view kept a 4096 array for 1024 maps). It grows at once, and shrinks (made again) once a smaller one has done
    // for _release_after renders in a row, so lights coming and going do not remake it.
    u32 side = 0;
    for (u32 k = 0; k < ctx.spots.size(); ++k) {
        side = spot_slot_of[k] >= 0 ? std::max(side, local_resolution(lights.spots[ctx.spots[k]], lights)) : side;
    }
    for (u32 j = 0; j < ctx.chosen.size(); ++j) {
        side = point_slot_of[j] >= 0 ? std::max(side, local_resolution(*ctx.chosen[j].second, lights)) : side;
    }
    if ((any_spot_map || any_point_map) && _local_map && side < _local_side) {
        if (++_local_smaller < _release_after)
            side = _local_side;
        else
            _local_smaller = 0;
    } else if (any_spot_map || any_point_map) {
        _local_smaller = 0;
    }
    if ((any_spot_map || any_point_map) && !ensure_local_map(side, spot_layers, point_slots)) {
        // Not made: no local shadows this render (the lights still light).
        std::fill(spot_slot_of.begin(), spot_slot_of.end(), -1);
        std::fill(point_slot_of.begin(), point_slot_of.end(), -1);
        any_spot_map = any_point_map = false;
    }

    // The shader's block: the spots, and each layer's matrix and numbers.
    const f32 side_f = static_cast<f32>(std::max(_local_side, 1u));
    lu.local[0] = any_spot_map || any_point_map ? 1.0f / side_f : 0.0f;
    for (u32 k = 0; k < ctx.spots.size(); ++k) {
        const SpotLightSource3D& l = lights.spots[ctx.spots[k]];
        const SpotCone3D cone = spot_cone(l);
        f32* o = lu.spots + 16 * k;
        put(o, l.position, l.radius);
        const f32 size = std::max(l.size, 0.01f);
        put_color(o + 4, l.color, size * size);
        put(o + 8, cone.axis, cone.cos_outer);
        put(o + 12, {cone.scale, static_cast<f32>(spot_slot_of[k]), 0.0f}, 0.0f);
        if (ctx.spots[k] < RenderDebugStats3D::max_reported_lights) {
            stats.spot_slot[ctx.spots[k]] = spot_slot_of[k];
        }
    }
    for (u32 i = 0; i < max_lights; ++i) {
        lu.point_shadow[4 * i] = -1.0f;
    }
    const auto set_layer = [&](u32 layer, const LocalShadowView3D& v, u32 res, const auto& light, f32 radius) {
        std::memcpy(lu.local_matrix + 16 * layer, v.view_projection.m, 16 * sizeof(f32));
        const f32 r = static_cast<f32>(res);
        const f32 fade_end = local_shadow_far(light.shadow_distance, radius);
        put(lu.local_a + 4 * layer, {r / side_f, 2.0f * v.tan_half / r, v.near_plane}, v.far_plane);
        put(lu.local_b + 4 * layer,
            {std::max(light.shadow_bias, 0.0f), std::max(light.shadow_normal_offset, 0.0f),
             std::clamp(light.shadow_softness, 0.25f, 4.0f)},
            fade_end);
    };
    const auto add_view = [&](const LocalShadowView3D& v, u32 layer, u32 res, u32 kind, u32 owner, Vec3f centre,
                              f32 radius, Vec3f eye, Vec3f axis) {
        LocalView view;
        view.eye = eye;
        view.axis = axis;
        view.near_plane = v.near_plane;
        view.per_unit = 0.5f * static_cast<f32>(std::min(res, std::max(_local_side, 1u))) / std::max(v.tan_half, 1e-3f);
        view.view_projection = v.view_projection;
        view.frustum = frustum_of(v.view_projection);
        view.texture = _local_map.handle();
        view.layer = layer;
        view.cache = layer;
        view.size = std::min(res, std::max(_local_side, 1u));
        view.kind = kind;
        view.owner = owner;
        view.centre = centre;
        view.radius = radius;
        view.bounded = true;
        _local_views.push_back(std::move(view));
        return static_cast<u32>(_local_views.size() - 1);
    };
    for (u32 k = 0; k < ctx.spots.size(); ++k) {
        if (spot_slot_of[k] < 0) {
            continue;
        }
        const SpotLightSource3D& l = lights.spots[ctx.spots[k]];
        const u32 res = std::min(local_resolution(l, lights), _local_side);
        const LocalShadowView3D v = spot_shadow_view(l, res);
        Vec3f c;
        f32 r;
        spot_bounding_sphere(l, c, r);
        const u32 layer = static_cast<u32>(spot_slot_of[k]);
        set_layer(layer, v, res, l, l.radius);
        add_view(v, layer, res, 0, ctx.spots[k], c, r, l.position, kin::spot_cone(l).axis);
    }
    for (u32 j = 0; j < ctx.chosen.size(); ++j) {
        if (point_slot_of[j] < 0) {
            continue;
        }
        const PointLightSource3D& l = *ctx.chosen[j].second;
        const u32 index = static_cast<u32>(&l - lights.points.data());
        const u32 res = std::min(local_resolution(l, lights), _local_side);
        const u32 base = _local_spot_layers + cube_faces * static_cast<u32>(point_slot_of[j]);
        lu.point_shadow[4 * j] = static_cast<f32>(base);
        if (index < RenderDebugStats3D::max_reported_lights) {
            stats.point_slot[index] = point_slot_of[j];
        }
        static const Vec3f axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (u32 f = 0; f < cube_faces; ++f) {
            const LocalShadowView3D v = cube_face_view(l, f, res);
            set_layer(base + f, v, res, l, l.radius);
            const u32 view = add_view(v, base + f, res, 1, index, l.position, l.radius, l.position, axes[f]);
            const Vec3f up = std::abs(axes[f].y) > 0.99f ? Vec3f{0.0f, 0.0f, -1.0f} : Vec3f{0.0f, 1.0f, 0.0f};
            // A face no visible surface can fall in is not drawn.
            _local_views[view].wanted =
                !ctx.scene.cull || pyramid_in(ctx.frustums[0], l.position, axes[f], up, v.tan_half, v.far_plane);
        }
    }

    // The fill light's cascades (the moon).
    if (maps_possible && moon_asked && lights.shadow_area.size().x > 0 && lights.shadow_area.size().z > 0 &&
        lights.shadow_area.size().y >= 0) {
        const DirectionalShadow3D& fs = lights.fill_shadow;
        const u32 res = std::clamp(fs.resolution != 0 ? fs.resolution : lights.shadow_resolution, 64u, 8192u);
        const u32 wanted = std::clamp(fs.cascades, 1u, max_shadow_cascades);
        const u32 got = ensure_moon_map(res, wanted);
        if (got > 0) {
            const Vec3f to_moon = normalize(lights.fill.direction);
            const f32 distance = fs.distance > 0.0f ? fs.distance : lights.shadow_distance;
            ShadowCascades3D plan;
            std::array<GpuFrustum, max_shadow_cascades> slices{};
            if (std::min(wanted, got) > 1) {
                plan = fit_shadow_cascades(scene.camera, ctx.aspect, to_moon, lights.shadow_area, std::min(wanted, got),
                                           distance, _moon_side);
                for (u32 k = 0; k < plan.count; ++k) {
                    const ShadowCascade3D& c = plan.cascade[k];
                    slices[k] =
                        frustum_of(slice_of(scene.camera, c.slice_near, c.slice_far).view_projection(ctx.aspect));
                }
            } else {
                const SunBox3D box = fit_sun_box(lights.shadow_area, to_moon);
                plan.count = 1;
                plan.cascade[0].view_projection =
                    Mat4f::orthographic(box.lo.x, box.hi.x, box.lo.y, box.hi.y, box.z_near, box.z_far) * box.light_view;
                plan.cascade[0].texel_world =
                    std::max(box.hi.x - box.lo.x, box.hi.y - box.lo.y) / static_cast<f32>(_moon_side);
                plan.cascade[0].depth_range = std::max(box.z_far - box.z_near, 0.01f);
            }
            ctx.moon_plan = plan;
            ctx.moon_cascades = plan.count;
            lu.counts[1] = static_cast<f32>(plan.count);
            lu.counts[2] = std::clamp(fs.strength, 0.0f, 1.0f);
            lu.moon[0] = 1.0f / static_cast<f32>(_moon_side);
            for (u32 k = 0; k < plan.count; ++k) {
                const ShadowCascade3D& c = plan.cascade[k];
                std::memcpy(lu.moon_matrix + 16 * k, c.view_projection.m, 16 * sizeof(f32));
                lu.moon_bias[4 * k] = c.texel_world / c.depth_range;
                lu.moon_bias[4 * k + 1] = c.texel_world * 1.5f;
                lu.moon_end[k] = c.slice_far;
                lu.moon_blend[k] = c.blend_from;
                LocalView view;
                view.view_projection = c.view_projection;
                view.frustum = frustum_of(c.view_projection);
                view.slice = slices[k];
                view.texture = _moon_map.handle();
                view.layer = k;
                view.cache = max_local_shadow_layers + k;
                view.size = _moon_side;
                view.moon = true;
                view.single_map = plan.count == 1;
                view.down = to_moon * -1.0f;
                view.axis = view.down;
                view.eye = scene.camera.position;
                view.per_unit = 1.0f / std::max(c.texel_world, 1e-6f); // orthographic: pixels a world unit spans
                view.kind = 2;
                _local_views.push_back(std::move(view));
            }
        }
    }
    stats.moon_cascades = ctx.moon_cascades;
    lu.counts[0] = static_cast<f32>(ctx.spots.size());
    lu.counts[3] = std::clamp(lights.sun_shadow_strength, 0.0f, 1.0f);
    ctx.lights_variant =
        _lights_available && (!ctx.spots.empty() || any_spot_map || any_point_map || ctx.moon_cascades > 0 ||
                              (lights.sun_shadow_strength < 1.0f && lights.sun_shadow_strength > 0.0f));

    // Idle arrays go back after a while, as the view's targets do.
    const bool local_used = any_spot_map || any_point_map;
    if (local_used) {
        _local_idle = 0;
    } else if (_local_map && ++_local_idle >= _release_after) {
        _local_map = {};
        _local_side = _local_layers = _local_spot_layers = _local_point_slots = 0;
        ++_local_generation;
        _local_idle = 0;
    }
    if (ctx.moon_cascades > 0) {
        _moon_idle = 0;
    } else if (_moon_map && ++_moon_idle >= _release_after) {
        _moon_map = {};
        _moon_side = _moon_layers = 0;
        ++_moon_generation;
        _moon_idle = 0;
    }
    const u32 texel_bytes = _shadow_format == SDL_GPU_TEXTUREFORMAT_D16_UNORM ? 2u : 4u;
    stats.local_shadow_bytes = static_cast<u64>(_local_side) * _local_side * _local_layers * texel_bytes +
                               static_cast<u64>(_moon_side) * _moon_side * _moon_layers * texel_bytes;
}

void GpuRenderer3D::upload_joints(FrameCtx& ctx) {
    const u32 joint_bytes = ctx.joint_total * static_cast<u32>(sizeof(Mat4f));
    ensure_buffer(_joints, joint_bytes, min_joint_buffer);
    const std::function<void(std::span<u8>)> fill_joints = [&](std::span<u8> out) {
        for (u32 s = 0; s < 2; ++s) {
            if (ctx.scenes[s] && !ctx.scenes[s]->joint_matrices().empty()) {
                std::memcpy(out.data() + static_cast<std::size_t>(ctx.joint_base[s]) * sizeof(Mat4f),
                            ctx.scenes[s]->joint_matrices().data(),
                            ctx.scenes[s]->joint_matrices().size() * sizeof(Mat4f));
            }
        }
    };
    _device->upload_storage_buffer(_joints->handle(), 0, joint_bytes, nullptr, /*cycle=*/true, &fill_joints);
    ctx.joints_uploaded = true;
}

// The local views' casters: each view's copies by sphere against its frustum (a spot's cone and its radius, a cube's
// face, a moon cascade as the sun's are tested), the exact bytes each draw is a function of when the cache is on (a
// view whose bytes are those of the render before is not drawn again), and what the others draw: the union of their
// copies as instances, a list of them for each (view, draw, level), and the posed ones' joint offsets.
void GpuRenderer3D::build_local_shadows(FrameCtx& ctx) {
    RenderDebugStats3D& stats = ctx.stats;
    const Lights3D& lights = ctx.scene.lights;
    if (_local_views.empty()) {
        return;
    }
    // Each copy's flat place, shared with _visible and the bounds computed by cull_and_upload.
    std::vector<u32> base(_refs.size() + 1, 0);
    for (u32 r = 0; r < _refs.size(); ++r) {
        base[r + 1] = base[r] + _refs[r].draw->count;
    }
    std::vector<u8> caster(_refs.size(), 0);
    constexpr u32 local_chunk = 2048;
    JobSystem* jobs = _owner->_jobs;
    bool any_skinned = false;
    for (u32 r = 0; r < _refs.size(); ++r) {
        const DrawRef& ref = _refs[r];
        if (ref.scene != 0 || !casts_into_local(ref.draw->material)) {
            continue;
        }
        if (ref.skinned && (!_skinning_available || !ref.mesh->skin)) {
            continue;
        }
        caster[r] = 1;
        any_skinned = any_skinned || ref.skinned;
    }
    // Preserve draw/copy order for packing, but schedule ranges containing many copies,
    // not one job for each tiny draw (terrain commonly has thousands of one-copy draws).
    struct Piece2 {
        u32 ref, begin, end, group;
    };
    std::vector<Piece2> sphere_pieces;
    for (const Piece& p : _pieces) {
        if (!caster[p.ref])
            continue;
        if (!sphere_pieces.empty()) {
            Piece2& last = sphere_pieces.back();
            if (last.ref == p.ref && last.group == p.group && last.end == p.begin &&
                p.end - last.begin <= local_chunk) {
                last.end = p.end;
                continue;
            }
        }
        sphere_pieces.push_back({p.ref, p.begin, p.end, p.group});
    }
    const bool cache_on = lights.cache_shadows;
    const GpuFrustum& camera = ctx.frustums[0];
    const bool culled = ctx.scene.cull;
    const Bounds3D& area = lights.shadow_area;
    std::array<shadow_reach::Ray, max_shadow_cascades> moon_rays;
    if (culled)
        for (const LocalView& view : _local_views)
            if (view.moon && view.wanted)
                moon_rays[view.layer] = shadow_reach::Ray{area, view.down};
    _local_entries.clear();
    // The copies of each view, by (draw, level): per view, per caster draw, the copies passing the tests bucketed by
    // level. A clustered mesh's copies have each their cut, as the light's map sees them: a run of compacted indices,
    // and the clusters' numbers (what the cache keys on).
    struct Found {
        u32 ref, level;
        std::span<u32> copies;
        bool clustered = false;               // its copies are cut for the light's map (below)
        std::vector<std::array<u32, 2>> runs; // clustered: first index and count, a copy each
    };
    struct ViewCull {
        const GpuFrustum* frustum;
        bool reaches(Vec3f c, f32 r) const { return sphere_in(*frustum, c, r); }
        u32 views(Vec3f c, f32 r) const { return sphere_in(*frustum, c, r) ? 1u : 0u; }
    };
    _local_cluster_indices.clear();
    std::vector<SelectedCluster3D> picks;
    std::vector<u8> seen;
    std::vector<u32> stack;
    std::vector<std::vector<Found>> found(_local_views.size());
    // What each view can see of each caster draw, in tasks of a piece of a draw: all of them in one call to the workers
    // when there are many copies. One classification byte per candidate copy instead of an allocating vector per
    // task/level. Workers own disjoint byte ranges and counts. A flat copy list is packed after they finish; Found only
    // holds spans into it, so even many one-copy draws need no per-draw allocation.
    constexpr u8 cut_bucket = max_mesh_levels, rejected = 255;
    std::vector<u32> found_copies;
    // Reuse the upload list as the accepted-copy backing store. Its spans stay valid
    // through cache keys and cluster cuts, then are compacted/remapped in place.
    found_copies.swap(_local_list);
    const auto find_copies = [&]() {
        struct GatherTask {
            u32 view, ref, begin, end;
            std::size_t first;
            u32 counts{};
            u8 plane_mask = 0;
            u8 flags = 0, reach_mask = 0; // bit 0: radius inside, bit 1: accept all
            u8 count_bucket = 0;          // single bucket, or rejected: counts indexes the rare multi-LOD array
        };
        static_assert(sizeof(GatherTask) <= 32);
        std::vector<u32> level_counts;
        u32 level_words = 0;
        const auto count_at = [&](GatherTask& task, u32 bucket) -> u32& {
            return task.count_bucket == rejected ? level_counts[task.counts + bucket] : task.counts;
        };
        const auto count_of = [&](const GatherTask& task, u32 bucket) -> u32 {
            if (task.count_bucket == rejected)
                return bucket < _refs[task.ref].levels ? level_counts[task.counts + bucket] : 0u;
            return task.count_bucket == bucket ? task.counts : 0u;
        };
        std::vector<GatherTask> tasks;
        const std::size_t wanted =
            std::count_if(_local_views.begin(), _local_views.end(), [](const LocalView& v) { return v.wanted; });
        tasks.reserve(wanted * sphere_pieces.size());
        std::size_t candidates = 0, classified = 0;
        std::vector<u32> batches{0};
        u32 in_batch = 0;
        for (u32 v = 0; v < _local_views.size(); ++v) {
            if (!_local_views[v].wanted)
                continue;
            for (const Piece2& piece : sphere_pieces) {
                const CullGroup* group = piece.group != ~0u ? &_cull_groups[piece.group] : nullptr;
                if (group && !(group->local_mask & (1u << v))) {
                    stats.local_group_rejected += piece.end - piece.begin;
                    continue;
                }
                const LocalView& view = _local_views[v];
                const DrawRef& ref = _refs[piece.ref];
                const u32 n = piece.end - piece.begin;
                const u8 planes = group ? group->local_planes[v] : static_cast<u8>((1u << view.frustum.count) - 1);
                const bool radius_inside = group && (group->local_radius_inside & (1u << v));
                const bool accept_all = group && !planes && (!view.bounded || radius_inside) &&
                                        (!view.moon || !culled) && ref.levels == 1 && !ref.clustered;
                const bool clustered = ref.clustered && ctx.lod[0].on;
                const u8 count_bucket = clustered ? cut_bucket : ref.levels > 1 ? rejected : 0;
                const GpuFrustum& reach = view.single_map ? camera : view.slice;
                const u8 reach_mask = group && view.moon ? group->moon_reach_planes[view.layer]
                                                         : static_cast<u8>((1u << reach.count) - 1);
                tasks.push_back({v,
                                 piece.ref,
                                 piece.begin,
                                 piece.end,
                                 classified,
                                 {},
                                 planes,
                                 static_cast<u8>((radius_inside ? 1u : 0u) | (accept_all ? 2u : 0u)),
                                 reach_mask,
                                 count_bucket});
                {
                    if (count_bucket == rejected) {
                        tasks.back().counts = level_words;
                        level_words += ref.levels;
                    }
                }
                candidates += n;
                if (accept_all)
                    count_at(tasks.back(), 0) = n;
                else {
                    classified += n;
                    in_batch += n;
                }
                stats.local_plane_budget += static_cast<u64>(group ? std::popcount(planes) : view.frustum.count) * n;
                if (view.bounded && !radius_inside)
                    stats.local_radius_tests += n;
                if (in_batch >= local_chunk) {
                    batches.push_back(static_cast<u32>(tasks.size()));
                    in_batch = 0;
                }
            }
        }
        // One allocation, with exactly the required level counters: even an all-LOD
        // scene needs at most 32 extra bytes per 32-byte task, without vector growth slack.
        level_counts.resize(level_words, 0u);
        if (batches.back() != tasks.size())
            batches.push_back(static_cast<u32>(tasks.size()));
        stats.local_candidates = candidates;
        stats.local_instance_tests = classified;
        stats.local_group_accepted = candidates - classified;
        stats.local_bucket_bytes = classified;
        std::vector<u8> buckets(classified, rejected);
        const auto gather = [&]<bool four>(GatherTask& task) {
            if (task.flags & 2u)
                return; // counts and contiguous accepted range are already known
            const LocalView& view = _local_views[task.view];
            const DrawRef& ref = _refs[task.ref];
            const u32 levels = std::max(ref.levels, 1u);
            const bool clustered = ref.clustered && ctx.lod[0].on;
            // Independent props need one scalar test, not a compacted frustum
            // and the multi-copy/LOD gather setup. Visit the same active planes
            // in their original order, with the same radius and plane arithmetic.
            if (task.end - task.begin == 1 && ref.levels <= 1 && !clustered && !view.moon) {
                const Vec4f& sp =
                    ref.cached_spheres ? ref.cached_spheres[task.begin] : _world_spheres[ref.sphere_first + task.begin];
                const Vec3f c{sp.x, sp.y, sp.z};
                if (view.bounded && !(task.flags & 1u)) {
                    const Vec3f d = c - view.centre;
                    const f32 reach = sp.w + view.radius;
                    if (dot(d, d) > reach * reach)
                        return;
                }
                u32 planes = task.plane_mask;
                while (planes) {
                    const Vec4f& p = view.frustum.planes[std::countr_zero(planes)];
                    planes &= planes - 1;
                    if (p.x * c.x + p.y * c.y + p.z * c.z + p.w < -sp.w)
                        return;
                }
                buckets[task.first] = 0;
                ++task.counts;
                return;
            }
            // Compact the active planes once per range, keeping their original order
            // and arithmetic. The hot loop reads a short contiguous frustum, not a bitset.
            GpuFrustum subset;
            const GpuFrustum* frustum = &view.frustum;
            if (task.plane_mask != (1u << view.frustum.count) - 1) {
                for (u32 k = 0; k < view.frustum.count; ++k)
                    if (task.plane_mask & (1u << k))
                        subset.planes[subset.count++] = view.frustum.planes[k];
                frustum = &subset;
            }
            const Vec4f* spheres = ref.cached_spheres ? ref.cached_spheres : _world_spheres.data() + ref.sphere_first;
            const bool test_radius = view.bounded && !(task.flags & 1u);
            const GpuFrustum& reach = view.single_map ? camera : view.slice;
            std::optional<GpuFrustum> reach_planes;
            if (view.moon && culled)
                reach_planes = reach_subset(reach, task.reach_mask);
            const auto accept = [&](u32 i) {
                const Vec4f& sp = spheres[i];
                const Vec3f c{sp.x, sp.y, sp.z};
                const f32 rad = sp.w;
                if (view.moon && culled) {
                    // As the sun's: the shadow of the copy must be able to fall in the camera's view (its slice's
                    // depths with cascades).
                    {
                        shadow_reach::Sweep sweep{moon_rays[view.layer], c, rad};
                        if (!reach_in(*reach_planes, sweep))
                            return;
                    }
                }
                if (clustered) {
                    buckets[task.first + i - task.begin] = cut_bucket;
                    ++count_at(task, cut_bucket);
                    return;
                }
                const u32 level = ref.levels > 1 ? std::min<u32>(_level_of[base[task.ref] + i], levels - 1) : 0u;
                buckets[task.first + i - task.begin] = static_cast<u8>(level);
                ++count_at(task, level);
            };
            u32 i = task.begin;
            if constexpr (four) {
                const sphere_cull4::Test test{
                    {frustum->planes.data(), frustum->count}, view.centre, view.radius, test_radius};
                const bool one_bucket = (clustered || ref.levels <= 1) && !(view.moon && culled);
                const u8 bucket = clustered ? cut_bucket : 0;
                for (; task.end - i >= 4; i += 4) {
                    u32 mask = test.mask(spheres + i);
                    if (one_bucket) {
                        count_at(task, bucket) += static_cast<u32>(std::popcount(mask));
                        if (mask == 15)
                            std::fill_n(buckets.begin() + task.first + i - task.begin, 4, bucket);
                        else
                            while (mask) {
                                const u32 k = std::countr_zero(mask);
                                mask &= mask - 1;
                                buckets[task.first + i + k - task.begin] = bucket;
                            }
                    } else
                        while (mask) {
                            const u32 k = std::countr_zero(mask);
                            mask &= mask - 1;
                            accept(i + k);
                        }
                }
            }
            for (; i < task.end; ++i) {
                const Vec4f& sp = spheres[i];
                const Vec3f c{sp.x, sp.y, sp.z};
                const f32 rad = sp.w;
                if (test_radius) {
                    const Vec3f d = c - view.centre;
                    const f32 reach = rad + view.radius;
                    if (dot(d, d) > reach * reach) {
                        continue;
                    }
                }
                if (!sphere_in(*frustum, c, rad)) {
                    continue;
                }
                accept(i);
            }
        };
        const auto gather_batch = [&](u32 batch) {
            for (u32 t = batches[batch]; t < batches[batch + 1]; ++t) {
                // Tiny terrain draws keep their scalar loop and do not prepare SIMD planes.
                if (sphere_cull4::available && tasks[t].end - tasks[t].begin >= 16)
                    gather.template operator()<true>(tasks[t]);
                else
                    gather.template operator()<false>(tasks[t]);
            }
        };
        if (jobs && batches.size() > 2 && classified >= 4 * local_chunk) {
            jobs->parallel_for(static_cast<i32>(batches.size() - 1), [&](i32 b) { gather_batch(static_cast<u32>(b)); });
        } else {
            for (u32 b = 0; b + 1 < batches.size(); ++b)
                gather_batch(b);
        }
        // Put together in the original (view, draw, level, copy) order. Size the backing store
        // before creating Found spans; cache decisions and cluster cuts must not invalidate them.
        std::size_t accepted = 0;
        for (const GatherTask& task : tasks) {
            {
                if (task.count_bucket != rejected) {
                    accepted += task.counts;
                    continue;
                }
            }
            for (u32 l = 0; l <= max_mesh_levels; ++l)
                accepted += count_of(task, l);
        }
        stats.local_task_bytes =
            tasks.capacity() * sizeof(GatherTask) + level_counts.capacity() * sizeof(level_counts[0]);
        found_copies.resize(accepted);
        std::size_t next = 0;
        for (std::size_t t = 0; t < tasks.size();) {
            const std::size_t begin = t;
            const u32 v = tasks[t].view, r = tasks[t].ref;
            {
                // Most meshes (including cluster cuts) use one bucket. Avoid nine counters
                // and their reductions/prefixes for every tiny terrain draw/view pair.
                if (tasks[t].count_bucket != rejected) {
                    u32 count = 0;
                    for (; t < tasks.size() && tasks[t].view == v && tasks[t].ref == r; ++t)
                        count += tasks[t].counts;
                    if (!count)
                        continue;
                    found[v].push_back(
                        {r, 0, {found_copies.data() + next, count}, tasks[begin].count_bucket == cut_bucket, {}});
                    for (std::size_t k = begin; k < t; ++k) {
                        const GatherTask& task = tasks[k];
                        if (!task.counts)
                            continue;
                        if (task.counts == task.end - task.begin) {
                            std::iota(found_copies.begin() + next, found_copies.begin() + next + task.counts,
                                      task.begin);
                            next += task.counts;
                        } else {
                            for (u32 i = task.begin; i < task.end; ++i)
                                if (buckets[task.first + i - task.begin] != rejected)
                                    found_copies[next++] = i;
                        }
                    }
                    continue;
                }
            }
            std::array<u32, max_mesh_levels + 1> counts{};
            for (; t < tasks.size() && tasks[t].view == v && tasks[t].ref == r; ++t) {
                for (u32 l = 0; l <= max_mesh_levels; ++l)
                    counts[l] += count_of(tasks[t], l);
            }
            std::array<std::size_t, max_mesh_levels + 1> cursor{};
            const auto add = [&](u32 bucket, bool clustered) {
                cursor[bucket] = next;
                if (!counts[bucket])
                    return;
                found[v].push_back(
                    {r, clustered ? 0u : bucket, {found_copies.data() + next, counts[bucket]}, clustered, {}});
                next += counts[bucket];
            };
            add(cut_bucket, true);
            for (u32 l = 0; l < std::max(_refs[r].levels, 1u); ++l)
                add(l, false);
            for (std::size_t k = begin; k < t; ++k) {
                const GatherTask& task = tasks[k];
                if (task.flags & 2u) {
                    const u32 n = task.end - task.begin;
                    std::iota(found_copies.begin() + cursor[0], found_copies.begin() + cursor[0] + n, task.begin);
                    cursor[0] += n;
                    continue;
                }
                for (u32 i = task.begin; i < task.end; ++i) {
                    const u8 bucket = buckets[task.first + i - task.begin];
                    if (bucket != rejected)
                        found_copies[cursor[bucket]++] = i;
                }
            }
        }
    };
    find_copies();

    // Exact: the bytes a view's draw is a function of. The same bytes as in the cache, the same picture.
    const u64 generation_local = _local_generation, generation_moon = _moon_generation;
    u32 drawing = 0;
    for (u32 v = 0; v < _local_views.size(); ++v) {
        LocalView& view = _local_views[v];
        view.drawn = view.cached = false;
        if (!view.wanted) {
            continue;
        }
        LocalCache& cache = _local_cache[view.cache];
        const auto* particles = particle_cast_view(view.view_projection);
        bool cacheable = (cache_on || ctx.shadow_history) && (!particles || !particles->unstable);
        if (cacheable) {
            std::vector<u8>& bytes = _local_payload;
            bytes.clear();
            CacheBytes payload{bytes};
            payload.value(view.moon ? generation_moon : generation_local);
            payload.value(view.layer);
            payload.value(view.size);
            payload.value(particles ? particles->key : u64{0});
            payload.append(view.view_projection.m, sizeof(view.view_projection.m));
            for (const Found& f : found[v]) {
                const DrawRef& ref = _refs[f.ref];
                const Material3D& m = ref.draw->material;
                const auto* texture = dynamic_cast<const GpuTextureBackend*>(m.texture.backend().get());
                if (m.blend == Blend3D::Mask && texture && texture->premultiplied()) {
                    cacheable = false; // a render target: drawn into without a trace
                    break;
                }
                payload.value(ref.mesh->serial);
                payload.value(f.level);
                payload.value(static_cast<u32>(f.copies.size()));
                payload.value(ref.skinned);
                payload.value(m.double_sided);
                payload.value(m.blend);
                if (m.blend == Blend3D::Mask) {
                    payload.value(m.alpha_cutoff);
                    payload.value(m.wrap_u);
                    payload.value(m.wrap_v);
                    payload.value(texture);
                    payload.value(texture ? texture->version() : u64{0});
                }
                for (const u32 i : f.copies) {
                    payload.append(ref.instances[i].transform.m, sizeof(f32) * 16);
                    if (ref.skinned) {
                        payload.append(ref.joints + ref.skin_first[i], ref.mesh->skin_bounds.size() * sizeof(Mat4f));
                    }
                }
                if (f.clustered) {
                    // The cut is a function of these and the hierarchy (the view's matrix and size are in already): not
                    // made for a map kept.
                    payload.value(ctx.lod[0].pixels);
                    payload.value(ctx.lod[0].force);
                }
            }
            if (cacheable) {
                const bool same = cache.valid && cache.payload == bytes;
                if (ctx.shadow_history && (!same || !cache.fingerprinted)) {
                    // Views share map slots, but each view's temporal identity
                    // follows its own content even after another view used it.
                    cache.revision = 1469598103934665603ull;
                    taa_hash(cache.revision, bytes.data(), bytes.size());
                    cache.fingerprinted = true;
                } else if (!same) {
                    ++cache.revision;
                    cache.fingerprinted = false;
                }
                if (cache_on && same) {
                    view.cached = true;
                    ++stats.local_shadow_cached;
                    continue;
                }
                cache.payload.swap(bytes);
                cache.valid = true;
            }
        }
        if (!cacheable) {
            ++cache.revision;
            cache.fingerprinted = false;
            cache.valid = false;
        }
        view.drawn = true;
        ++drawing;
    }
    // The cuts of the clustered copies, for the maps to be drawn (a map kept has not had them made).
    for (u32 v = 0; v < _local_views.size(); ++v) {
        const LocalView& view = _local_views[v];
        if (!view.drawn) {
            continue;
        }
        for (Found& f : found[v]) {
            if (!f.clustered) {
                continue;
            }
            const DrawRef& ref = _refs[f.ref];
            const LodView& lv = ctx.lod[0];
            u32 kept = 0;
            for (const u32 i : f.copies) {
                // The map is an image: the error is judged in its texels, from where the light is.
                ClusterCutView3D cv;
                cv.transform = ref.instances[i].transform;
                cv.eye = view.eye;
                cv.forward = view.axis;
                cv.orthographic = view.moon;
                cv.per_unit = view.per_unit;
                cv.pixels = lv.pixels;
                cv.nearest = view.near_plane;
                cv.force_depth = lv.force;
                select_clusters(*ref.mesh->clusters, cv, ViewCull{&view.frustum}, picks, seen, stack);
                u32 triangles = 0;
                for (const SelectedCluster3D& pick : picks) {
                    triangles += ref.mesh->clusters->clusters[pick.cluster].triangle_count;
                }
                if (triangles == 0) {
                    continue;
                }
                const u32 first = static_cast<u32>(_local_cluster_indices.size());
                _local_cluster_indices.resize(first + triangles * 3u);
                u32 at = first;
                for (const SelectedCluster3D& pick : picks) {
                    ref.mesh->clusters->append_indices(pick.cluster, _local_cluster_indices.data() + at);
                    at += ref.mesh->clusters->clusters[pick.cluster].triangle_count * 3u;
                }
                f.copies[kept++] = i;
                f.runs.push_back({first, triangles * 3u});
            }
            f.copies = f.copies.first(kept);
        }
        // A clustered draw none of whose copies has a triangle in the view has nothing to draw.
        std::erase_if(found[v], [](const Found& f) { return f.copies.empty(); });
    }
    // Only the maps the cache has drawn from before are `valid`: a map not drawn at all (a face not wanted) keeps its
    // entry.
    stats.local_shadow_maps = 0;
    for (const LocalView& view : _local_views) {
        stats.local_shadow_maps += view.wanted ? 1u : 0u;
    }
    stats.local_shadow_drawn = drawing;
    if (drawing == 0) {
        stats.local_copy_bytes = (found_copies.capacity() + _local_list.capacity() + _local_batch_list.capacity() +
                                  _local_batch_refs.capacity()) *
                                 sizeof(u32);
        found_copies.swap(_local_list);
        stats.local_slot_bytes = _local_slot.capacity() * sizeof(u32);
        return;
    }

    // Visibility/LOD no longer needs the original prefixes. Retained draws index their
    // own GPU table directly and never need a slot in the streamed instance union.
    {
        for (u32 r = 0; r < _refs.size(); ++r)
            base[r + 1] = base[r] + (_refs[r].retained == ~0u ? _refs[r].draw->count : 0u);
        if (_local_slot.capacity() > std::max<std::size_t>(4096, std::size_t(base.back()) * 2))
            std::vector<u32>().swap(_local_slot);
    }
    // The union of the copies the drawn views use, a list for each (view, draw, level).
    _local_slot.assign(base.back(), ~0u);
    stats.local_slot_bytes = _local_slot.capacity() * sizeof(u32);
    _local_copies.clear();
    _local_joint_list.clear();
    _local_list.clear();
    _local_runs.clear();
    u32 union_count = 0;
    bool skinned_in_use = false;
    std::size_t written = 0;
    const auto slot_of = [&](u32 r, u32 i) {
        if (_refs[r].retained != ~0u)
            return i;
        u32& slot = _local_slot[base[r] + i];
        if (slot == ~0u) {
            const DrawRef& ref = _refs[r];
            slot = union_count++;
            _local_copies.push_back({r, i});
            _local_joint_list.push_back(ref.skinned ? ref.joint_base + ref.skin_first[i] : 0u);
        }
        return slot;
    };
    // Intern shadow state once per source ref. Sorting the light maps then
    // compares small integer IDs instead of repeatedly visiting large DrawRefs.
    struct BatchKey {
        u64 mesh;
        std::uintptr_t texture;
        u32 retained, cutoff, state;
        bool operator==(const BatchKey&) const = default;
    };
    struct BatchHash {
        std::size_t operator()(const BatchKey& k) const {
            u64 h = (k.mesh ^ k.texture) * 0x9e3779b97f4a7c15ull;
            h = (h ^ k.retained) * 0x9e3779b97f4a7c15ull;
            h = (h ^ k.cutoff) * 0x9e3779b97f4a7c15ull;
            h = (h ^ k.state) * 0x9e3779b97f4a7c15ull;
            return static_cast<std::size_t>(h ^ (h >> 32));
        }
    };
    std::unordered_map<BatchKey, u32, BatchHash> batch_refs;
    batch_refs.reserve(std::min<std::size_t>(_refs.size(), 1024));
    _local_batch_refs.assign(_refs.size(), ~0u);
    const auto batch_of = [&](u32 r) {
        u32& batch = _local_batch_refs[r];
        if (batch != ~0u)
            return batch;
        const DrawRef& ref = _refs[r];
        const Material3D& material = ref.draw->material;
        const bool cutout = material.blend == Blend3D::Mask;
        // Opaque lighting state is unused. bind_cutout's texture, sampler and
        // cutoff must match; tint alpha is already in each instance's record.
        const BatchKey key{ref.mesh->serial,
                           cutout ? reinterpret_cast<std::uintptr_t>(material.texture.backend().get()) : 0,
                           ref.retained, cutout ? std::bit_cast<u32>(material.alpha_cutoff) : 0,
                           u32(material.double_sided) | (u32(ref.skinned) << 1) | (u32(cutout) << 2) |
                               (cutout ? (u32(material.wrap_u) << 8) | (u32(material.wrap_v) << 16) : 0)};
        batch = batch_refs.try_emplace(key, r).first->second;
        return batch;
    };
    for (u32 v = 0; v < _local_views.size(); ++v) {
        LocalView& view = _local_views[v];
        view.entry_first = static_cast<u32>(_local_entries.size());
        view.entry_count = 0;
        if (!view.drawn) {
            continue;
        }
        for (const Found& f : found[v]) {
            LocalEntry entry;
            entry.ref = f.ref;
            (void)batch_of(f.ref);
            entry.level = f.level;
            entry.list_first = static_cast<u32>(written);
            entry.clustered = !f.runs.empty();
            entry.run_first = static_cast<u32>(_local_runs.size());
            {
                // Found spans are in packing order. Cached views and cluster cuts only
                // remove copies, so the write cursor never overtakes unread input.
                if (_refs[f.ref].retained != ~0u) {
                    std::memmove(found_copies.data() + written, f.copies.data(), f.copies.size_bytes());
                    written += f.copies.size();
                } else {
                    for (const u32 i : f.copies)
                        found_copies[written++] = slot_of(f.ref, i);
                }
            }
            _local_runs.insert(_local_runs.end(), f.runs.begin(), f.runs.end());
            entry.count = static_cast<u32>(f.copies.size());
            skinned_in_use = skinned_in_use || _refs[f.ref].skinned;
            _local_entries.push_back(entry);
            ++view.entry_count;
        }
    }
    {
        found_copies.resize(written);
        found_copies.swap(_local_list);
    }
    // Cache keys and the streamed union retain their original order. Only the
    // depth draws change: compatible entries share one contiguous caster list.
    const auto shadow_less = [&](const LocalEntry& a, const LocalEntry& b) {
        const u32 ar = _local_batch_refs[a.ref], br = _local_batch_refs[b.ref];
        return ar != br ? ar < br : a.level < b.level;
    };
    _local_batch_list.resize(_local_list.size());
    u32 batches = 0, copies = 0;
    for (LocalView& view : _local_views) {
        if (!view.entry_count) {
            view.entry_first = batches;
            continue;
        }
        const auto first = _local_entries.begin() + view.entry_first;
        const auto last = first + view.entry_count;
        // Keep nearby source draws together: sorting the entire map by mesh
        // destroys caster locality and can increase GPU depth-pass work.
        constexpr std::ptrdiff_t batch_window = 128;
        for (auto block = first; block != last;) {
            const auto end = block + std::min(batch_window, last - block);
            std::stable_sort(block, end, shadow_less);
            block = end;
        }
        view.entry_first = batches;
        view.entry_count = 0;
        for (auto it = first; it != last; ++it) {
            LocalEntry entry = *it;
            std::copy_n(_local_list.data() + entry.list_first, entry.count, _local_batch_list.data() + copies);
            entry.list_first = copies;
            copies += entry.count;
            // Cluster cuts have a different index run per copy. Keep their
            // draw records intact, including their correspondence to the list.
            if (view.entry_count && !entry.clustered && !_local_entries[batches - 1].clustered &&
                _local_batch_refs[_local_entries[batches - 1].ref] == _local_batch_refs[entry.ref] &&
                _local_entries[batches - 1].level == entry.level) {
                _local_entries[batches - 1].count += entry.count;
            } else {
                _local_entries[batches++] = entry;
                ++view.entry_count;
            }
        }
    }
    _local_entries.resize(batches);
    _local_batch_list.swap(_local_list);
    stats.local_copy_bytes = (found_copies.capacity() + _local_list.capacity() + _local_batch_list.capacity() +
                              _local_batch_refs.capacity()) *
                             sizeof(u32);
    if (union_count > 0) {
        const u32 instance_bytes = union_count * static_cast<u32>(sizeof(GpuInstance3D));
        ensure_buffer(_local_instances, instance_bytes, min_instance_buffer);
        // No CPU mirror of the 128-byte GPU records: build each once in disjoint staging ranges.
        const std::function<void(std::span<u8>)> fill_instances = [&](std::span<u8> out) {
            const auto fill = [&](u32 begin, u32 end) {
                for (u32 slot = begin; slot < end; ++slot) {
                    const auto [r, i] = _local_copies[slot];
                    const DrawRef& ref = _refs[r];
                    GpuInstance3D g{};
                    std::memcpy(g.model, ref.instances[i].transform.m, sizeof(g.model));
                    const Color m = ref.draw->material.color, tint = ref.instances[i].tint;
                    g.color[3] = static_cast<f32>(m.a) / 255.0f * static_cast<f32>(tint.a) / 255.0f;
                    std::memcpy(out.data() + static_cast<std::size_t>(slot) * sizeof(g), &g, sizeof(g));
                }
            };
            if (jobs && union_count >= 4 * local_chunk) {
                jobs->parallel_for(static_cast<i32>((union_count + local_chunk - 1) / local_chunk), [&](i32 b) {
                    const u32 begin = static_cast<u32>(b) * local_chunk;
                    fill(begin, std::min(union_count, begin + local_chunk));
                });
            } else {
                fill(0, union_count);
            }
        };
        _device->upload_storage_buffer(_local_instances->handle(), 0, instance_bytes, nullptr, /*cycle=*/true,
                                       &fill_instances);
        stats.instance_upload_bytes += instance_bytes;
    }
    const u32 list_bytes = static_cast<u32>(_local_list.size() * sizeof(u32));
    ensure_buffer(_local_lists, std::max(list_bytes, 4u), min_caster_list_buffer);
    _device->upload_storage_buffer(_local_lists->handle(), 0, std::max(list_bytes, 4u), _local_list.data(),
                                   /*cycle=*/true);
    if (!_local_cluster_indices.empty()) {
        const u32 index_bytes = static_cast<u32>(_local_cluster_indices.size() * sizeof(u32));
        ensure_buffer(_local_index_buffer, index_bytes, 64 * 1024, SDL_GPU_BUFFERUSAGE_INDEX);
        _device->upload_storage_buffer(_local_index_buffer->handle(), 0, index_bytes, _local_cluster_indices.data(),
                                       /*cycle=*/true);
    }
    if (skinned_in_use && any_skinned) {
        if (!ctx.joints_uploaded) {
            upload_joints(ctx);
        }
        const u32 offset_bytes = union_count * static_cast<u32>(sizeof(u32));
        ensure_buffer(_local_joint_offsets, offset_bytes, min_caster_list_buffer);
        _device->upload_storage_buffer(_local_joint_offsets->handle(), 0, offset_bytes, _local_joint_list.data(),
                                       /*cycle=*/true);
    }
}

void GpuRenderer3D::local_shadow_pass(FrameCtx& ctx) {
    RenderDebugStats3D& stats = ctx.stats;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    if (_local_views.empty() || !_local_lists) {
        return;
    }
    SDL_GPUBuffer* instances = _local_instances ? _local_instances->handle() : nullptr;
    SDL_GPUBuffer* lists = _local_lists ? _local_lists->handle() : instances;
    SDL_GPUBuffer* joint_offsets = _local_joint_offsets ? _local_joint_offsets->handle() : nullptr;
    // These eight variants are fixed for this call. Avoid rebuilding and
    // searching the full pipeline key for every caster in every light map.
    std::array<SDL_GPUGraphicsPipeline*, 8> pipelines{};
    for (LocalView& view : _local_views) {
        if (!view.drawn) {
            continue;
        }
        // Not cycled, as the sun's: the frames before are done with it in queue order.
        const SDL_GPUDepthStencilTargetInfo depth =
            depth_target(view.texture, SDL_GPU_STOREOP_STORE, /*cycle=*/false, view.layer);
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
        if (!pass) {
            _local_cache[view.cache].valid = false;
            continue;
        }
        set_viewport(pass, view.size, view.size);
        DrawUniforms u{};
        std::memcpy(u.view_projection, view.view_projection.m, sizeof(u.view_projection));
        DrawBinder binder{pass};
        for (u32 e = view.entry_first; e < view.entry_first + view.entry_count; ++e) {
            const LocalEntry& entry = _local_entries[e];
            const DrawRef& ref = _refs[entry.ref];
            const Material3D& caster = ref.draw->material;
            const bool cutout = caster.blend == Blend3D::Mask;
            if (ref.skinned && (!ctx.joints || !joint_offsets)) {
                continue;
            }
            const u32 variant = u32(!caster.double_sided) | (u32(cutout) << 1) | (u32(ref.skinned) << 2);
            SDL_GPUGraphicsPipeline*& p = pipelines[variant];
            if (!p)
                p = pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, !caster.double_sided, Pass::Shadow, cutout, nullptr,
                             false, ref.skinned);
            if (!p) {
                continue;
            }
            SDL_GPUBuffer* const plain[2] = {ref.retained != ~0u ? _retained[ref.retained].buffer->handle() : instances,
                                             lists};
            SDL_GPUBuffer* const posed[4] = {instances, lists, ctx.joints, joint_offsets};
            binder.bind_pipeline(p, ref.skinned ? posed : plain, ref.skinned ? 4 : 2);
            // A clustered mesh's copies are drawn from the cut the light's map made of each (a run of the compacted
            // indices); else the mesh whole.
            if (entry.clustered && !_local_index_buffer) {
                continue;
            }
            binder.bind_mesh(ref.mesh, entry.clustered ? _local_index_buffer->handle() : nullptr);
            if (ref.skinned) {
                binder.bind_skin(ref.mesh);
            }
            if (cutout) {
                bind_cutout(commands, pass, caster);
            }
            if (entry.clustered) {
                for (u32 n = 0; n < entry.count; ++n) {
                    const std::array<u32, 2>& run = _local_runs[entry.run_first + n];
                    u.first[0] = _local_list[entry.list_first + n];
                    u.first[1] = 0;
                    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                    (void)binder.draw(run[1], 1, run[0]);
                    ++stats.local_shadow_draws;
                    ++stats.local_shadow_casters;
                    stats.local_shadow_triangles += run[1] / 3;
                }
                continue;
            }
            const GpuMesh3D::Level& level =
                ref.mesh->levels[std::min<std::size_t>(entry.level, ref.mesh->levels.size() - 1)];
            u.first[0] = 0;
            u.first[1] = entry.list_first + 1;
            SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
            (void)binder.draw(level.index_count, entry.count, level.first_index);
            ++stats.local_shadow_draws;
            stats.local_shadow_casters += entry.count;
            stats.local_shadow_triangles += static_cast<u64>(level.index_count / 3) * entry.count;
        }
        if (!draw_particle_shadows(ctx, pass, view.view_projection, true))
            _local_cache[view.cache].valid = false;
        SDL_EndGPURenderPass(pass);
    }
}

// The light variants' block for scene `s`, into `slot`. The overlay's lights are carried into its space and have no
// shadows.
void GpuRenderer3D::push_local_lights(const FrameCtx& ctx, u32 s, u32 slot) {
    LocalLightUniforms u = ctx.lu;
    if (s != 0) {
        const Mat4f to_space = ctx.scenes[1]->camera.view().inverse() * ctx.scene.camera.view();
        for (u32 k = 0; k < static_cast<u32>(u.counts[0]); ++k) {
            f32* o = u.spots + 16 * k;
            const Vec3f position = to_space.apply({o[0], o[1], o[2]});
            const Vec3f axis = normalize(to_space.apply_vector({o[8], o[9], o[10]}));
            put(o, position, o[3]);
            put(o + 8, axis, o[11]);
            o[13] = -1.0f;
        }
        for (u32 i = 0; i < max_lights; ++i) {
            u.point_shadow[4 * i] = -1.0f;
        }
        u.counts[1] = 0.0f;
    }
    SDL_PushGPUFragmentUniformData(ctx.commands, slot, &u, sizeof(u));
}

GpuInstance3D GpuRenderer3D::pack_instance(const MeshInstance3D& instance, Color material) const {
    const Color m = material;
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
    return g;
}

// The retained instance tables' whole budget (make_room): a batch over it can never be retained, so it is never
// "wanted".
constexpr u64 retained_budget = 32 * 1024 * 1024;

void GpuRenderer3D::retain_instances(DrawRef& ref, FrameCtx& ctx) {
    const auto& draw = *ref.draw;
    if (!_retained_enabled || !draw.batch || ctx.taa || ctx.scene.lod.occlusion.enabled || ref.skinned ||
        ref.clustered || ref.levels != 1 || draw.material.shader.backend() ||
        (draw.material.blend != Blend3D::Opaque && draw.material.blend != Blend3D::Mask))
        return;
    const auto& batch = draw.batch;
    const u64 id = batch.identity(), frame = _retained_frames;
    const Color color = draw.material.color;
    // The batch's table in this colour; else one of its tables in another colour unused for a whole GPU frame (an
    // animated colour uploads once a frame into the same table or two, as one table did, rather than leaving a table
    // behind for every colour); else a new one.
    u32 slot = ~0u, idle = ~0u;
    for (u32 i = 0; i < _retained.size(); ++i) {
        const RetainedTable& t = _retained[i];
        if (!t.buffer || t.id != id)
            continue;
        if (t.color == color) {
            slot = i;
            break;
        }
        if (t.frame + 1 < frame && (idle == ~0u || t.used < _retained[idle].used))
            idle = i;
    }
    if (slot == ~0u)
        slot = idle;
    if (slot == ~0u) {
        const u64 bytes = batch.instances().size_bytes() / sizeof(MeshInstance3D) * sizeof(GpuInstance3D);
        if (!make_room(id, bytes, false, ctx.stats)) {
            ++ctx.stats.retained_refused;
            if (bytes <= retained_budget && bytes > _retained_wanted.bytes)
                _retained_wanted = {id, bytes, batch.lifetime()};
            return;
        }
        auto buffer = std::make_shared<GpuBuffer>(
            _device->create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, nullptr, static_cast<u32>(bytes)));
        slot = 0;
        while (slot < _retained.size() && _retained[slot].buffer)
            ++slot;
        if (slot == _retained.size())
            _retained.emplace_back();
        RetainedTable& fresh = _retained[slot];
        fresh.id = id;
        fresh.lifetime = batch.lifetime();
        fresh.buffer = std::move(buffer);
        _retained_bytes += static_cast<u32>(bytes);
        if (_retained_reserved.id == id)
            _retained_reserved = {};
    }
    auto& table = _retained[slot];
    const u64 serial = _owner->_frame_serial;
    // Earlier views in the same GPU frame must keep their backing allocation, so at most one upload per table/frame.
    const bool dirty = table.revision != batch.revision() || table.color != color;
    if (dirty && (table.claimed == _retained_epoch || table.upload_frame == serial))
        return;
    if (dirty) {
        // An edit to a table holding this batch in this colour (same layout) sends only the copies edited since,
        // written in place. Not when a draw of this GPU frame reads the table already: the frame's uploads land before
        // all its draws, so an earlier view would see the edit (the whole table, cycled, leaves its draws the old
        // storage); nor when over half changed (the whole table then, cycled, waits for no read of the last frame).
        u32 changed = draw.count;
        if (table.color == color && table.revision != 0 && table.layout == batch.layout_revision() &&
            table.serial != serial) {
            _retained_ranges.clear();
            batch.changed_ranges_since(table.revision, _retained_ranges);
            changed = 0;
            for (const auto& range : _retained_ranges)
                changed += range.count;
        }
        constexpr u32 chunk = 2048;
        const bool partial = 2 * static_cast<u64>(changed) <= draw.count;
        const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
            // Cycling needs a complete table, including the unchanged records; a partial write holds the changed ones,
            // range after range. Pack independent slices directly into staging, without a CPU mirror.
            const auto pack = [&](u32 first, u32 end) {
                if (!partial) {
                    for (u32 i = first; i < end; ++i) {
                        const auto record = pack_instance(ref.instances[i], color);
                        std::memcpy(out.data() + static_cast<std::size_t>(i) * sizeof(record), &record, sizeof(record));
                    }
                    return;
                }
                auto r =
                    static_cast<std::size_t>(std::upper_bound(_retained_starts.begin(), _retained_starts.end(), first) -
                                             _retained_starts.begin()) -
                    1;
                for (u32 k = first; k < end; ++k) {
                    while (r + 1 < _retained_starts.size() && _retained_starts[r + 1] <= k)
                        ++r;
                    const auto record =
                        pack_instance(ref.instances[_retained_ranges[r].first + (k - _retained_starts[r])], color);
                    std::memcpy(out.data() + static_cast<std::size_t>(k) * sizeof(record), &record, sizeof(record));
                }
            };
            const u32 records = partial ? changed : draw.count;
            if (_owner->_jobs && records >= 4 * chunk)
                _owner->_jobs->parallel_for(static_cast<i32>((records + chunk - 1) / chunk), [&](i32 task) {
                    const u32 first = static_cast<u32>(task) * chunk;
                    pack(first, std::min(first + chunk, records));
                });
            else
                pack(0, records);
        };
        if (partial) {
            _retained_spans.clear();
            _retained_starts.clear();
            u32 at = 0;
            for (const auto& range : _retained_ranges) {
                constexpr u32 record = sizeof(GpuInstance3D);
                _retained_spans.push_back({range.first * record, range.count * record});
                _retained_starts.push_back(at);
                at += range.count;
            }
            _device->upload_storage_ranges(table.buffer->handle(), _retained_spans, fill);
            ctx.stats.retained_upload_bytes += static_cast<u64>(changed) * sizeof(GpuInstance3D);
        } else {
            _device->upload_storage_buffer(table.buffer->handle(), 0, table.buffer->size(), nullptr, true, &fill);
            ctx.stats.retained_upload_bytes += table.buffer->size();
        }
        table.revision = batch.revision();
        table.color = color;
        table.layout = batch.layout_revision();
        table.upload_frame = serial;
    }
    table.claimed = _retained_epoch;
    table.frame = frame;
    table.serial = serial;
    table.used = ++_retained_claims;
    ref.retained = slot;
    ctx.stats.retained_instances += draw.count;
}

void GpuRenderer3D::drop_table(RetainedTable& table) {
    // No wait for the GPU: SDL_ReleaseGPUBuffer defers the destruction until the submitted work that uses the buffer is
    // done.
    if (table.buffer)
        _retained_bytes -= table.buffer->size();
    _bounds_cache_bytes -= static_cast<u32>(table.bounds.capacity() * sizeof(Vec4f));
    table = {};
}

bool GpuRenderer3D::make_room(u64 id, u64 bytes, bool active_too, RenderDebugStats3D& stats) {
    // Room for a new table of `bytes` within the 32 MiB / 128 tables, less what the collection kept for another batch.
    // It evicts, least recently claimed first, tables not claimed for a whole GPU frame; with `active_too` (the
    // collection, for the last frame's largest refused batch) tables claimed last frame too, while all of those it
    // evicts are smaller than the batch, so that retention settles on the larger batches instead of trading places
    // every frame. Never a table claimed this frame: this frame's draws hold its slot (and maybe its spheres).
    constexpr u64 budget = retained_budget;
    constexpr u32 max_tables = 128;
    if (bytes > budget)
        return false;
    const u64 frame = _retained_frames;
    const bool others = _retained_reserved.id && _retained_reserved.id != id;
    const u64 held = others ? _retained_reserved.bytes : 0;
    u32 live = others ? 1u : 0u;
    for (const RetainedTable& t : _retained)
        live += t.buffer ? 1u : 0u;
    const auto fits = [&](u64 freed, u32 count) {
        return live - count < max_tables && _retained_bytes - freed + held + bytes <= budget;
    };
    if (fits(0, 0))
        return true;
    _retained_victims.clear();
    for (u32 i = 0; i < _retained.size(); ++i) {
        const RetainedTable& t = _retained[i];
        if (t.buffer && t.frame != frame && (active_too || t.frame + 1 < frame))
            _retained_victims.push_back(i);
    }
    std::sort(_retained_victims.begin(), _retained_victims.end(),
              [&](u32 a, u32 b) { return _retained[a].used < _retained[b].used; });
    u64 freed = 0, active = 0;
    u32 count = 0;
    for (const u32 v : _retained_victims) {
        if (fits(freed, count))
            break;
        const RetainedTable& t = _retained[v];
        if (t.frame + 1 >= frame && (active += t.buffer->size()) >= bytes)
            break; // later ones are all claimed last frame too
        freed += t.buffer->size();
        ++count;
    }
    if (!fits(freed, count))
        return false;
    for (u32 k = 0; k < count; ++k)
        drop_table(_retained[_retained_victims[k]]);
    stats.retained_evictions += count;
    return true;
}

void GpuRenderer3D::cache_instance_bounds(DrawRef& ref, RenderDebugStats3D& stats) {
    // LOD needs sphere_of's original scale too: dividing its rounded radius back
    // out can change a threshold. Posed bounds likewise stay on the existing path.
    if (ref.skinned || ref.levels != 1)
        return;
    // A batch's spheres are kept once, in whichever of its tables (they differ in colour only) has them. They move to
    // the table this draw uses; one it does not use is marked claimed this frame, so that no eviction frees them under
    // this render.
    const u64 id = ref.draw->batch.identity();
    u32 holder = ~0u;
    for (u32 i = 0; i < _retained.size(); ++i) {
        const RetainedTable& t = _retained[i];
        if (!t.buffer || t.id != id || (holder != ~0u && t.bounds.empty()))
            continue;
        holder = i;
        if (!t.bounds.empty())
            break;
    }
    if (holder == ~0u)
        return;
    const u32 slot = ref.retained != ~0u ? ref.retained : holder;
    if (slot != holder && !_retained[holder].bounds.empty()) {
        std::swap(_retained[slot].bounds, _retained[holder].bounds);
        std::swap(_retained[slot].bounds_revision, _retained[holder].bounds_revision);
    }
    auto& table = _retained[slot];
    table.frame = _retained_frames;
    if (table.bounds.empty()) {
        // Fixed-size batches: one exact-size allocation per table, never a growing
        // per-copy mirror alongside the old scratch. GPU tables cap this at 4 MiB.
        table.bounds = std::vector<Vec4f>(ref.draw->count);
        _bounds_cache_bytes += static_cast<u32>(table.bounds.capacity() * sizeof(Vec4f));
        _bounds_cache_grew = true;
    }
    if (table.bounds_revision != ref.draw->batch.revision()) {
        const auto changed = ref.draw->batch.changed_since(table.bounds_revision);
        for (u32 i = changed.first; i < changed.first + changed.count; ++i) {
            Vec3f centre;
            f32 radius;
            sphere_of(ref.instances[i].transform, ref.center, ref.radius, centre, radius);
            table.bounds[i] = {centre.x, centre.y, centre.z, radius};
        }
        table.bounds_revision = ref.draw->batch.revision();
        stats.bounds_cache_rebuilt += changed.count;
        stats.bounds_instance_tests += changed.count;
    }
    // Updating CPU bounds is independent of the GPU record version/material colour.
    // A later view that must stream GPU records can still use these current bounds.
    ref.cached_spheres = table.bounds.data();
    stats.bounds_cache_instances += ref.draw->count;
}

void GpuRenderer3D::cull_and_upload(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
    const Lights3D& lights = scene.lights;
    const Bounds3D& area = lights.shadow_area;
    // The draws of both scenes, each with its instances where the scene keeps them.
    std::array<PreparedKey, 2> keys{};
    bool eligible = true;
    for (u32 s = 0; s < 2; ++s)
        if (const Scene3D* submitted = ctx.scenes[s]) {
            keys[s] = {submitted,
                       submitted->draws().data(),
                       submitted->instances().data(),
                       submitted->draws().size(),
                       submitted->draw_identity(),
                       submitted->draw_revision()};
            eligible = eligible && !submitted->lod.enabled;
        }
    std::size_t draw_count = 0, group_count = 0;
    for (const auto* submitted : ctx.scenes)
        if (submitted) {
            draw_count += submitted->draws().size();
            group_count += submitted->instance_groups().size();
        }
    eligible = eligible && draw_count <= 8192 && group_count <= 4096;
    bool reuse = eligible && _prepared_valid && keys == _prepared_keys;
    const auto same_bounds = [](const Bounds3D& a, const Bounds3D& b) { return a.min == b.min && a.max == b.max; };
    if (eligible && _prepared_valid && !reuse && draw_count == _prepared_draws.size() &&
        group_count == _prepared_groups.size()) {
        // Same scene/storage, newly submitted commands: validate every layout
        // input before consulting cached draw pointers. Content/quality can change.
        reuse = true;
        for (u32 s = 0; s < 2; ++s) {
            auto live = keys[s], old = _prepared_keys[s];
            live.revision = old.revision = 0;
            reuse = reuse && live == old;
        }
        std::size_t d = 0, g = 0;
        for (const auto* submitted : ctx.scenes)
            if (submitted && reuse) {
                for (const auto& draw : submitted->draws()) {
                    const auto& old = _prepared_draws[d++];
                    if (old.mesh.lock() != draw.mesh.backend() || !same_bounds(old.bounds, draw.mesh.bounds()) ||
                        old.batch != draw.batch.identity() || old.first != draw.first || old.count != draw.count ||
                        old.skinned != draw.skinned || old.skin_first != draw.skin_first ||
                        old.group_first != draw.group_first || old.group_count != draw.group_count) {
                        reuse = false;
                        break;
                    }
                }
                for (const auto& group : submitted->instance_groups()) {
                    const auto& old = _prepared_groups[g++];
                    if (old.first != group.first || old.count != group.count ||
                        !same_bounds(old.world_bounds, group.world_bounds)) {
                        reuse = false;
                        break;
                    }
                }
            }
    }
    if (reuse)
        for (const PreparedBatch& batch : _prepared_batches) {
            const auto& live = _refs[batch.ref].draw->batch;
            if (live.layout_revision() != batch.revision) {
                reuse = false;
                break;
            }
        }
    _prepared_valid = false;
    if (!reuse) {
        _refs.clear();
        _prepared_batches.clear();
    }
    _bounds_cache_grew = false;
    ++_retained_epoch;
    if (_retained_gc_frame != _owner->_frame_serial) {
        // Between GPU frames (no render's draws hold a slot): expired batches' tables go, without waiting for the GPU.
        _retained_gc_frame = _owner->_frame_serial;
        ++_retained_frames;
        for (RetainedTable& t : _retained)
            if (t.buffer && t.lifetime.expired())
                drop_table(t);
        std::erase_if(_retained, [](const RetainedTable& t) { return !t.buffer; });
        // The largest batch refused a table last frame may evict smaller ones claimed then; the room is kept for it
        // this frame. At most one such change a frame.
        _retained_reserved = {};
        const RetainedWant want = std::exchange(_retained_wanted, {});
        const u32 evicted = stats.retained_evictions;
        if (!want.lifetime.expired() && make_room(want.id, want.bytes, true, stats) &&
            stats.retained_evictions != evicted)
            _retained_reserved = want;
    }
    _cull_groups.clear();
    _gc.active = false;
    std::array<LodView, 2>& lod = ctx.lod;
    for (u32 s = 0; s < 2; ++s) {
        if (ctx.scenes[s] && ctx.scenes[s]->lod.enabled) {
            lod[s] = lod_view_of(ctx.scenes[s]->lod, ctx.scenes[s]->debug, ctx.scenes[s]->camera,
                                 static_cast<f32>(view->_size.y));
        }
    }
    static_assert(max_local_shadow_layers + max_shadow_cascades <= 32);
    const auto classify_groups = [&](DrawRef& ref) {
        ref.group_first = static_cast<u32>(_cull_groups.size());
        const auto groups = ref.draw->batch ? ref.draw->batch.groups()
                                            : ctx.scenes[ref.scene]->instance_groups().subspan(ref.draw->group_first,
                                                                                               ref.draw->group_count);
        for (const Scene3D::InstanceGroup& group : groups) {
            const auto& bounds = group.world_bounds;
            if (!std::isfinite(bounds.min.x) || !std::isfinite(bounds.min.y) || !std::isfinite(bounds.min.z) ||
                !std::isfinite(bounds.max.x) || !std::isfinite(bounds.max.y) || !std::isfinite(bounds.max.z) ||
                bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z)
                continue;
            CullGroup g;
            g.first = group.first;
            g.end = group.first + group.count;
            const Bounds3D& b = group.world_bounds;
            g.camera = ref.frustum ? box_relation(*ref.frustum, b) : 2;
            if (ref.caster) {
                for (u32 k = 0; k < ctx.cascades; ++k) {
                    const u8 state = box_relation(ctx.cascades > 1 ? ctx.cascade_frustums[k] : ctx.light_frustum, b);
                    if (state)
                        g.sun_mask |= static_cast<u8>(1u << k);
                    if (state == 2)
                        g.sun_inside |= static_cast<u8>(1u << k);
                    if (state) {
                        const GpuFrustum& reach = ctx.cascades > 1 ? ctx.slice_frustums[k] : ctx.frustums[ref.scene];
                        g.sun_reach_planes[k] = shadow_reach::pending_planes({reach.planes.data(), reach.count}, b);
                    }
                }
            }
            if (ref.scene == 0 && casts_into_local(ref.draw->material)) {
                for (u32 v = 0; v < _local_views.size(); ++v) {
                    const LocalView& view = _local_views[v];
                    if (!view.wanted || (view.bounded && box_misses_sphere(b, view.centre, view.radius)))
                        continue;
                    u8 planes;
                    planes = shadow_cull::pending_planes({view.frustum.planes.data(), view.frustum.count}, b);
                    if (planes == shadow_cull::outside)
                        continue;
                    g.local_mask |= 1u << v;
                    g.local_planes[v] = planes;
                    if (view.moon && ctx.scene.cull) {
                        const GpuFrustum& reach = view.single_map ? ctx.frustums[0] : view.slice;
                        g.moon_reach_planes[view.layer] =
                            shadow_reach::pending_planes({reach.planes.data(), reach.count}, b);
                    }
                    if (view.bounded && shadow_cull::inside_radius(b, view.centre, view.radius))
                        g.local_radius_inside |= 1u << v;
                }
            }
            _cull_groups.push_back(g);
        }
        ref.group_count = static_cast<u32>(_cull_groups.size()) - ref.group_first;
    };
    bool any_levels = false, any_clusters = false, any_skinned = false;
    // The frame's joint matrices: the scene's, then the overlay's.
    u32 joint_total = 0;
    std::array<u32, 2> joint_base{};
    for (u32 s = 0; s < 2; ++s) {
        if (!ctx.scenes[s]) {
            continue;
        }
        const bool has_groups = !ctx.scenes[s]->instance_groups().empty();
        joint_base[s] = joint_total;
        joint_total += static_cast<u32>(ctx.scenes[s]->joint_matrices().size());
        if (!reuse)
            for (const Scene3D::Draw& draw : ctx.scenes[s]->draws()) {
                const auto* mesh = dynamic_cast<const GpuMesh3D*>(draw.mesh.backend().get());
                if (draw.count == 0 || !mesh) {
                    continue;
                }
                if (draw.skinned && (!_skinning_available || !mesh->skin)) {
                    warn_once(_skin_warned, "3D: skinned meshes cannot be drawn on this device (the skinning shaders "
                                            "were not made): not drawn");
                    continue;
                }
                const Bounds3D& b = draw.mesh.bounds();
                const bool caster = ctx.shadows && s == 0 &&
                                    (draw.material.blend == Blend3D::Opaque || draw.material.blend == Blend3D::Mask) &&
                                    draw.material.casts_shadow && !draw.material.wireframe;
                _refs.push_back(
                    {&draw, draw.batch ? draw.batch.instances().data() : ctx.scenes[s]->instances().data() + draw.first,
                     mesh, s, ctx.scenes[s]->cull ? &ctx.frustums[s] : nullptr, caster, b.center(), b.radius(), 0, 0,
                     0});
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
                if (!draw.skinned && ((has_groups && draw.group_count) || (draw.batch && !draw.batch.groups().empty())))
                    classify_groups(_refs.back());
                if (draw.batch) {
                    if (eligible) {
                        if (_prepared_batches.size() < 128)
                            _prepared_batches.push_back(
                                {static_cast<u32>(_refs.size() - 1), draw.batch.layout_revision()});
                        else
                            eligible = false;
                    }
                    retain_instances(_refs.back(), ctx);
                    cache_instance_bounds(_refs.back(), stats);
                }
            }
    }
    if (reuse)
        for (DrawRef& ref : _refs) {
            ref.frustum = ctx.scenes[ref.scene]->cull ? &ctx.frustums[ref.scene] : nullptr;
            ref.caster = ctx.shadows && ref.scene == 0 &&
                         (ref.draw->material.blend == Blend3D::Opaque || ref.draw->material.blend == Blend3D::Mask) &&
                         ref.draw->material.casts_shadow && !ref.draw->material.wireframe;
            ref.retained = ~0u;
            ref.cached_spheres = nullptr;
            if (ref.skinned) {
                // Only the command layout is reused. Poses, bounds, shadow membership
                // and current/previous GPU joint tables are refreshed every render.
                ref.skin_first = ctx.scenes[ref.scene]->skin_offsets().data() + ref.draw->skin_first;
                ref.joints = ctx.scenes[ref.scene]->joint_matrices().data();
                ref.joint_base = joint_base[ref.scene];
                any_skinned = true;
            }
            if (ref.group_count)
                classify_groups(ref);
            if (ref.draw->batch) {
                retain_instances(ref, ctx);
                cache_instance_bounds(ref, stats);
            }
        }

    stats.retained_gpu_bytes = _retained_bytes;
    stats.bounds_cache_bytes = _bounds_cache_bytes;
    stats.spatial_groups = static_cast<u32>(_cull_groups.size());

    eligible = eligible && _refs.size() <= 8192 && _cull_groups.size() <= 4096;
    ctx.joint_total = joint_total;
    ctx.joint_base = joint_base;

    // Pieces of draws, and tasks of about `chunk` instances each (a big draw
    // split, small ones taken together), for the workers when there are many.
    // Small tasks balance over many workers (22,000 copies in chunks of 4096 made six tasks on a 32-thread
    // machine); the scene must be big enough for waking them to pay (measured: a loss at 1,500 copies, a gain from
    // 2,500).
    constexpr u32 chunk = 512, parallel_from = 2048;
    u32 total = reuse ? _prepared_total : 0, in_task = 0;
    if (!reuse) {
        _pieces.clear();
        _tasks.assign(1, 0);
        for (u32 r = 0; r < static_cast<u32>(_refs.size()); ++r) {
            const DrawRef& ref = _refs[r];
            const u32 count = ref.draw->count;
            u32 next_group = ref.group_first, groups_end = ref.group_first + ref.group_count;
            for (u32 begin = 0; begin < count;) {
                while (next_group < groups_end && _cull_groups[next_group].end <= begin)
                    ++next_group;
                u32 group = ~0u, boundary = count;
                if (next_group < groups_end) {
                    const CullGroup& g = _cull_groups[next_group];
                    if (begin >= g.first) {
                        group = next_group;
                        boundary = g.end;
                    } else
                        boundary = g.first;
                }
                const u32 end = std::min(boundary, begin + (chunk - in_task));
                _pieces.push_back({r, begin, end, total + begin, group});
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
    }
    const u64 layout_bytes = _refs.capacity() * sizeof(DrawRef) + _pieces.capacity() * sizeof(Piece) +
                             _tasks.capacity() * sizeof(u32) + _cull_groups.capacity() * sizeof(CullGroup);
    eligible = eligible && _pieces.size() <= 16384 && layout_bytes <= 16 * 1024 * 1024;
    if (eligible) {
        if (!reuse) {
            _prepared_draws.clear();
            _prepared_groups.clear();
            for (const auto* submitted : ctx.scenes)
                if (submitted) {
                    for (const auto& draw : submitted->draws())
                        _prepared_draws.push_back({draw.mesh.backend(), draw.mesh.bounds(), draw.batch.identity(),
                                                   draw.first, draw.count, draw.skin_first, draw.group_first,
                                                   draw.group_count, draw.skinned});
                    _prepared_groups.insert(_prepared_groups.end(), submitted->instance_groups().begin(),
                                            submitted->instance_groups().end());
                }
        }
        _prepared_keys = keys;
        _prepared_total = total;
        _prepared_valid = true;
    } else {
        std::vector<PreparedBatch>().swap(_prepared_batches);
        std::vector<PreparedDraw>().swap(_prepared_draws);
        std::vector<Scene3D::InstanceGroup>().swap(_prepared_groups);
    }
    stats.prepared_reused = reuse ? 1u : 0u;
    stats.prepared_draws = static_cast<u32>(_refs.size());
    stats.prepared_layout_bytes = layout_bytes;
    stats.prepared_cache_bytes = sizeof(_prepared_keys) + sizeof(_prepared_batches) + sizeof(_prepared_draws) +
                                 sizeof(_prepared_groups) + sizeof(_prepared_total) + sizeof(_prepared_valid) +
                                 _prepared_batches.capacity() * sizeof(PreparedBatch) +
                                 _prepared_draws.capacity() * sizeof(PreparedDraw) +
                                 _prepared_groups.capacity() * sizeof(Scene3D::InstanceGroup);
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
    const shadow_reach::Ray sun_ray{lights.shadow_area, down};
    _visible.resize(total);
    // Local-light selection precedes this cull. Only keep bounds when a local/moon view
    // needs them, and only for its caster draws; no second transform/skin-bound pass.
    const bool local_bounds = !_local_views.empty();
    u32 scratch = 0;
    if (local_bounds)
        for (DrawRef& ref : _refs) {
            if (ref.cached_spheres || ref.scene != 0 || !casts_into_local(ref.draw->material))
                continue;
            ref.sphere_first = scratch;
            scratch += ref.draw->count;
        }
    // When a formerly streamed batch becomes cached, release its old scratch
    // capacity instead of quietly retaining a second full array. Hysteresis avoids
    // shrinking on ordinary frame-to-frame changes; no previous contents are needed.
    if (_bounds_cache_grew && _world_spheres.capacity() > std::max<std::size_t>(2ull * scratch, 4096))
        std::vector<Vec4f>(scratch).swap(_world_spheres);
    else
        _world_spheres.resize(scratch);
    stats.bounds_scratch_bytes = _world_spheres.capacity() * sizeof(Vec4f);
    if (any_levels) {
        _level_of.resize(total);
    }
    // A camera-inside cell has the same result for every single-level copy.
    // Keep refinement when another consumer needs its sphere, level or cascade bits.
    const auto camera_range = [&](const Piece& piece) {
        if (piece.group == ~0u)
            return false;
        const DrawRef& ref = _refs[piece.ref];
        const CullGroup& group = _cull_groups[piece.group];
        return group.camera == 2 && ref.levels == 1 && !ref.clustered && !ref.skinned &&
               (!ref.caster || ctx.cascades <= 1) &&
               (!local_bounds || ref.cached_spheres || ref.scene != 0 || !casts_into_local(ref.draw->material) ||
                !group.local_mask);
    };
    // Inactive slots are never consumed. Clear each active prefix on every
    // render, including when a later view enables more levels or cascades.
    const auto clear_counts = [&](auto& counts, u32 active) {
        if (active == 1)
            counts[0] = 0;
        else
            std::fill_n(counts.begin(), active, 0u);
    };
    const u32 counter_cascades = ctx.cascades;
    const auto classify = [&]<bool fast>(Piece& piece) {
        const DrawRef& ref = _refs[piece.ref];
        const CullGroup* group = piece.group != ~0u ? &_cull_groups[piece.group] : nullptr;
        const u8 camera = group ? group->camera : ref.frustum ? 1 : 2;
        const u8 sun_mask = group ? group->sun_mask : static_cast<u8>((1u << ctx.cascades) - 1);
        const bool save_bounds = local_bounds && !ref.cached_spheres && ref.scene == 0 &&
                                 casts_into_local(ref.draw->material) && (!group || group->local_mask);
        piece.camera_tests = camera == 1 ? piece.end - piece.begin : 0;
        piece.bounds_tests = 0;
        clear_counts(piece.seen, ref.levels);
        clear_counts(piece.extra, ref.levels);
        for (u32 k = 0; k < counter_cascades; ++k)
            clear_counts(piece.cascade[k], ref.levels);
        if (camera_range(piece)) {
            std::fill_n(_visible.begin() + piece.flat, piece.end - piece.begin, camera_bit);
            piece.seen[0] = piece.end - piece.begin;
            return;
        }
        if (group && camera == 0 && sun_mask == 0 && group->local_mask == 0) {
            std::fill_n(_visible.begin() + piece.flat, piece.end - piece.begin, u8{0});
            if (any_levels)
                std::fill_n(_level_of.begin() + piece.flat, piece.end - piece.begin, u8{0});
            return;
        }
        // Resident, single-level copies already have current spheres for local
        // shadows. With no sun membership to produce, only camera bits remain:
        // preserve whole-range proofs and refine partial ranges four at a time.
        if (ref.cached_spheres && ref.levels == 1 && !ref.caster && !ref.skinned && !ref.clustered) {
            const u32 n = piece.end - piece.begin;
            u8* visible = _visible.data() + piece.flat;
            if (camera != 1) {
                std::fill_n(visible, n, camera == 2 ? camera_bit : u8{0});
                piece.seen[0] = camera == 2 ? n : 0;
                return;
            }
            const sphere_cull4::Test test{{ref.frustum->planes.data(), ref.frustum->count}, {}, 0, false};
            u32 i = piece.begin;
            for (; piece.end - i >= 4; i += 4) {
                const u32 mask = test.mask(ref.cached_spheres + i);
                piece.seen[0] += static_cast<u32>(std::popcount(mask));
                for (u32 k = 0; k < 4; ++k)
                    visible[i - piece.begin + k] = mask & (1u << k) ? camera_bit : u8{0};
            }
            for (; i < piece.end; ++i) {
                const Vec4f& s = ref.cached_spheres[i];
                const bool seen = sphere_in(*ref.frustum, {s.x, s.y, s.z}, s.w);
                visible[i - piece.begin] = seen ? camera_bit : u8{0};
                piece.seen[0] += seen ? 1u : 0u;
            }
            return;
        }
        // These small subsets are per spatial piece, never per copy. Leave the
        // optional disengaged for the thousands of ordinary one-copy terrain draws.
        std::optional<std::array<GpuFrustum, max_shadow_cascades>> reach_planes;
        if (fast && group && ref.caster) {
            reach_planes.emplace();
            for (u32 k = 0; k < ctx.cascades; ++k)
                (*reach_planes)[k] = reach_subset(ctx.cascades > 1 ? ctx.slice_frustums[k] : ctx.frustums[ref.scene],
                                                  group->sun_reach_planes[k]);
        }
        const bool transform_bounds = camera == 1 || (ref.caster && sun_mask) || ref.levels > 1 || save_bounds;
        piece.bounds_tests = ref.skinned || (transform_bounds && !ref.cached_spheres) ? piece.end - piece.begin : 0;
        for (u32 i = piece.begin; i < piece.end; ++i) {
            Vec3f c{};
            f32 r = 0.0f, scale = 1.0f;
            if (ref.skinned) {
                // A posed copy's bound is the sphere round its joints' moved spheres, never smaller than the mesh posed
                // (skin_sphere): the pose, not the mesh at rest. If no joint can be bounded, the rest bound stands.
                Vec3f centre = ref.center;
                f32 radius = ref.radius;
                skin_sphere(ref.mesh->skin_bounds, {ref.joints + ref.skin_first[i], ref.mesh->skin_bounds.size()},
                            centre, radius);
                scale = sphere_of(ref.instances[i].transform, centre, radius, c, r);
            } else if (ref.cached_spheres) {
                const Vec4f& sphere = ref.cached_spheres[i];
                c = {sphere.x, sphere.y, sphere.z};
                r = sphere.w;
            } else if (transform_bounds) {
                scale = sphere_of(ref.instances[i].transform, ref.center, ref.radius, c, r);
            }
            if (save_bounds)
                _world_spheres[ref.sphere_first + i] = {c.x, c.y, c.z, r};
            u32 level = 0;
            if (ref.levels > 1) {
                level = lod_level(lod[ref.scene], *ref.mesh, ref.levels, c, r, scale);
                _level_of[piece.flat + i - piece.begin] = static_cast<u8>(level);
            }
            const bool seen = camera == 2 || (camera == 1 && sphere_in(*ref.frustum, c, r));
            // With cascades, each takes the casters whose shadow reaches its
            // slice of the view (seen ones too: one far off isn't drawn into the near map).
            u8 members = 0;
            bool sun = false;
            // A seen copy in a single sun map needs no reach test; neither do non-casters.
            if (ref.caster && (ctx.cascades > 1 || !seen)) {
                shadow_reach::Sweep sweep{sun_ray, c, r};
                if (ctx.cascades > 1) {
                    const f32 leave = fast ? 0 : to_leave(area, c, down) + r;
                    for (u32 k = 0; k < ctx.cascades; ++k) {
                        if ((sun_mask & (1u << k)) &&
                            ((group && (group->sun_inside & (1u << k))) || sphere_in(ctx.cascade_frustums[k], c, r)) &&
                            (fast ? reach_in(reach_planes ? (*reach_planes)[k] : ctx.slice_frustums[k], sweep)
                                  : shadow_in(ctx.slice_frustums[k], c, r, down, leave))) {
                            members |= cascade_bit(k);
                        }
                    }
                }
                sun = !seen && (ctx.cascades > 1
                                    ? members != 0
                                    : (sun_mask & 1u) &&
                                          ((group && (group->sun_inside & 1u)) || sphere_in(ctx.light_frustum, c, r)) &&
                                          (fast ? reach_in(reach_planes ? (*reach_planes)[0] : *ref.frustum, sweep)
                                                : shadow_in(*ref.frustum, c, r, down, to_leave(area, c, down) + r)));
            }
            _visible[piece.flat + i - piece.begin] = static_cast<u8>((seen ? camera_bit : sun ? sun_bit : 0) | members);
            piece.seen[level] += seen ? 1u : 0u;
            piece.extra[level] += sun ? 1u : 0u;
            for (u32 k = 0; k < ctx.cascades && ctx.cascades > 1; ++k) {
                piece.cascade[k][level] += in_cascade(members, k) ? 1u : 0u;
            }
        }
    };
    // Keep the original loop free of reach state when shadows are disabled.
    // Single-map draws without cells have no plane proofs or endpoint reuse; their
    // original loop is cheaper. Compile the reference separately, without per-copy flag branches.
    if (ctx.shadows && (!_cull_groups.empty() || ctx.cascades > 1))
        each_task([&](Piece& piece) {
            if (piece.group != ~0u || ctx.cascades > 1)
                classify.operator()<true>(piece);
            else
                classify.operator()<false>(piece);
        });
    else
        each_task([&](Piece& piece) { classify.operator()<false>(piece); });
    // Each draw's copies: by level, and in each those seen, then those only the sun sees.
    for (DrawRef& ref : _refs) {
        ref.out_count = ref.out_extra = 0;
        clear_counts(ref.cascade_count, ctx.cascades);
        clear_counts(ref.level_seen, ref.levels);
        clear_counts(ref.level_extra, ref.levels);
        for (u32 k = 0; k < counter_cascades; ++k)
            clear_counts(ref.cascade_level_count[k], ref.levels);
    }
    for (const Piece& piece : _pieces) {
        stats.camera_instance_tests += piece.camera_tests;
        stats.bounds_instance_tests += piece.bounds_tests;
        if (camera_range(piece))
            stats.camera_range_accepted += piece.end - piece.begin;
        if (piece.group != ~0u && _cull_groups[piece.group].camera == 0)
            stats.camera_group_rejected += piece.end - piece.begin;
        DrawRef& ref = _refs[piece.ref];
        for (u32 l = 0; l < ref.levels; ++l) {
            ref.level_seen[l] += piece.seen[l];
            ref.level_extra[l] += piece.extra[l];
            for (u32 k = 0; k < ctx.cascades; ++k) {
                ref.cascade_level_count[k][l] += piece.cascade[k][l];
            }
        }
    }
    u32 written = 0, retained_written = 0, listed = 0;
    for (DrawRef& ref : _refs) {
        u32& cursor = ref.retained != ~0u ? retained_written : written;
        ref.out_first = cursor;
        for (u32 l = 0; l < ref.levels; ++l) {
            ref.level_first[l] = cursor;
            cursor += ref.level_seen[l] + ref.level_extra[l];
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
                clear_counts(seen, _refs[current].levels);
                clear_counts(extra, _refs[current].levels);
                for (u32 k = 0; k < counter_cascades; ++k)
                    clear_counts(in_list[k], _refs[current].levels);
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
        // This frame's joints are the next frame's past (the record is swapped in resolve_history); the matrices are
        // kept whole, so that a copy's range there needs no copying of its own and copies sharing one range share it
        // there too.
        GpuCopyHistory& record = view->_taa_now;
        record.joints.clear();
        if (any_skinned) {
            record.joints.reserve(joint_total);
            for (u32 s = 0; s < 2; ++s) {
                if (ctx.scenes[s]) {
                    record.joints.insert(record.joints.end(), ctx.scenes[s]->joint_matrices().begin(),
                                         ctx.scenes[s]->joint_matrices().end());
                }
            }
        }
        match_copies(*view, !ctx.taa_reset, jobs, any_skinned);
    }
    if (any_clusters) {
        ctx.gpu_clusters = _gpu_clusters && ctx.scene.debug.gpu_culling;
        ctx.occlusion = ctx.gpu_clusters && ctx.scene.lod.occlusion.enabled && _occlusion_available;
        cut_clusters(ctx, _owner->_jobs);
        if (!ctx.gpu_clusters) {
            ctx.occlusion = false;
        }
    }
    if (ctx.scene.lod.occlusion.enabled && !ctx.occlusion && !ctx.occlusion_said) {
        warn_once(_occlusion_warned, "3D: occlusion culling asked for, but it needs Lod3D::enabled and gpu_culling, "
                                     "clustered meshes, and a device that can: off");
    }

    if (retained_written) {
        _retained_visible.resize(retained_written);
        each_task([&](Piece& piece) {
            const DrawRef& ref = _refs[piece.ref];
            if (ref.retained == ~0u)
                return;
            u32 at = piece.out[0], extra = piece.out_extra[0];
            if (ref.levels == 1 && piece.seen[0] == piece.end - piece.begin) {
                std::iota(_retained_visible.begin() + at, _retained_visible.begin() + at + piece.seen[0], piece.begin);
                return;
            }
            for (u32 i = piece.begin; i < piece.end; ++i) {
                const u8 v = _visible[piece.flat + i - piece.begin];
                if (v & in_view_bits)
                    _retained_visible[(v & camera_bit) ? at++ : extra++] = i;
            }
        });
        const u32 bytes = retained_written * static_cast<u32>(sizeof(u32));
        ensure_buffer(_retained_indices, bytes, min_caster_list_buffer);
        _device->upload_storage_buffer(_retained_indices->handle(), 0, bytes, _retained_visible.data(), true);
        stats.retained_index_bytes = bytes;
    }
    // The copies, straight into the upload memory; alpha-blended ones furthest first.
    if (written > 0) {
        const u32 bytes = static_cast<u32>(written * sizeof(GpuInstance3D));
        ensure_buffer(_instances, bytes, min_instance_buffer,
                      SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
                          (_gpu_clusters ? SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ : 0u));
        if (ctx.taa) {
            _taa_ordered.resize(written);
        }
        if (any_skinned) {
            _skin_ordered.resize(written);
            if (ctx.taa) {
                _skin_prev_ordered.resize(written);
            }
        }
        const auto write = [&](const DrawRef& ref, const MeshInstance3D& instance, u8* out, u32 at, u32 flat) {
            if (ctx.taa) {
                _taa_ordered[at] = _taa_prev[flat]; // each copy's own slot: safe from the workers
            }
            if (any_skinned) {
                // The copy's first joint matrix in the frame's buffer (0 for a copy that has none), in its place among
                // the instances.
                _skin_ordered[at] = ref.skinned ? ref.joint_base + ref.skin_first[&instance - ref.instances] : 0u;
                if (ctx.taa) {
                    _skin_prev_ordered[at] = ref.skinned ? _taa_prev_joint[flat] : ~0u;
                }
            }
            const GpuInstance3D g = pack_instance(instance, ref.draw->material.color);
            std::memcpy(out + static_cast<std::size_t>(at) * sizeof(GpuInstance3D), &g, sizeof(g));
        };
        const std::function<void(std::span<u8>)> fill = [&](std::span<u8> out) {
            each_task([&](Piece& piece) {
                const DrawRef& ref = _refs[piece.ref];
                if (ref.retained != ~0u || ref.draw->material.blend == Blend3D::Alpha) {
                    return; // retained separately, or sorted below
                }
                std::array<u32, max_mesh_levels> at = piece.out, at_extra = piece.out_extra;
                if (ref.levels == 1 && piece.seen[0] == piece.end - piece.begin) {
                    for (u32 i = piece.begin; i < piece.end; ++i)
                        write(ref, ref.instances[i], out.data(), at[0]++, piece.flat + i - piece.begin);
                    return;
                }
                for (u32 i = piece.begin; i < piece.end; ++i) {
                    const u8 v = _visible[piece.flat + i - piece.begin];
                    if ((v & in_view_bits) != 0) {
                        const u32 level = ref.levels > 1 ? _level_of[piece.flat + i - piece.begin] : 0u;
                        write(ref, ref.instances[i], out.data(), (v & camera_bit) ? at[level]++ : at_extra[level]++,
                              piece.flat + i - piece.begin);
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
                std::sort(_sorted.begin(), _sorted.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
                u32 at = ref.out_first;
                for (const auto& [d, i] : _sorted) {
                    write(ref, ref.instances[i], out.data(), at++, flat + i);
                }
            }
        };
        // Cycled: a view rendered earlier this frame keeps the instances it drew.
        _device->upload_storage_buffer(_instances->handle(), 0, bytes, nullptr, /*cycle=*/true, &fill);
        stats.instance_upload_bytes += bytes;

        // Skinned copies: the frame's joint matrices (both scenes', straight into the upload memory) and each copy's
        // first one, in the places the instances are. One buffer each for the frame, grown as the previous ones are: no
        // upload per draw.
        if (any_skinned) {
            const u32 joint_bytes = joint_total * static_cast<u32>(sizeof(Mat4f));
            ensure_buffer(_joints, joint_bytes, min_joint_buffer);
            const std::function<void(std::span<u8>)> fill_joints = [&](std::span<u8> out) {
                for (u32 s = 0; s < 2; ++s) {
                    if (ctx.scenes[s] && !ctx.scenes[s]->joint_matrices().empty()) {
                        std::memcpy(out.data() + static_cast<std::size_t>(joint_base[s]) * sizeof(Mat4f),
                                    ctx.scenes[s]->joint_matrices().data(),
                                    ctx.scenes[s]->joint_matrices().size() * sizeof(Mat4f));
                    }
                }
            };
            _device->upload_storage_buffer(_joints->handle(), 0, joint_bytes, nullptr, /*cycle=*/true, &fill_joints);
            ctx.joints_uploaded = true;
            const u32 offset_bytes = written * static_cast<u32>(sizeof(u32));
            ensure_buffer(_joint_offsets, offset_bytes, min_caster_list_buffer);
            _device->upload_storage_buffer(_joint_offsets->handle(), 0, offset_bytes, _skin_ordered.data(),
                                           /*cycle=*/true);
            if (ctx.taa) {
                // Last frame's matrices, as the record holds them (whole), and where each copy's own start in them.
                const std::vector<Mat4f>& past = view->_taa_last.joints;
                const Mat4f none{};
                const u32 past_bytes = static_cast<u32>(std::max<std::size_t>(past.size(), 1) * sizeof(Mat4f));
                ensure_buffer(_previous_joints, past_bytes, min_joint_buffer);
                _device->upload_storage_buffer(_previous_joints->handle(), 0, past_bytes,
                                               past.empty() ? &none : past.data(), /*cycle=*/true);
                ensure_buffer(_previous_joint_offsets, offset_bytes, min_caster_list_buffer);
                _device->upload_storage_buffer(_previous_joint_offsets->handle(), 0, offset_bytes,
                                               _skin_prev_ordered.data(), /*cycle=*/true);
            }
        }

        // Temporal anti-aliasing: each copy's matrix of the frame before, in the same places (the velocity pre-pass
        // reads it).
        if (ctx.taa) {
            const u32 previous_bytes = written * static_cast<u32>(sizeof(Mat4f));
            ensure_buffer(_previous, previous_bytes, min_previous_buffer);
            _device->upload_storage_buffer(_previous->handle(), 0, previous_bytes, _taa_ordered.data(), /*cycle=*/true);
        }
    }
    {
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
                            _caster_list[to[k][level]++] = ref.retained != ~0u ? i : index;
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
    const Bounds3D* area;
    const GpuFrustum* cascade_frustums;
    const GpuFrustum* slice_frustums;
    const GpuFrustum* light_frustum;
    u32 cascades;
    const shadow_reach::Ray* ray;

    u32 views(Vec3f c, f32 r) const {
        u32 mask = 0;
        if (seen && (!camera || sphere_in(*camera, c, r))) {
            mask |= 1u;
        }
        if (caster && ray) {
            shadow_reach::Sweep sweep{*ray, c, r};
            if (cascades > 1) {
                for (u32 k = 0; k < cascades; ++k)
                    if (in_cascade(visible, k) && sphere_in(cascade_frustums[k], c, r) &&
                        reach_in(slice_frustums[k], sweep))
                        mask |= 2u << k;
            } else if (sphere_in(*light_frustum, c, r) && (!camera || reach_in(*camera, sweep)))
                mask |= 2u;
        } else if (caster) {
            if (cascades > 1) {
                const f32 leave = to_leave(*area, c, down) + r;
                for (u32 k = 0; k < cascades; ++k) {
                    if (in_cascade(visible, k) && sphere_in(cascade_frustums[k], c, r) &&
                        shadow_in(slice_frustums[k], c, r, down, leave)) {
                        mask |= 2u << k;
                    }
                }
            } else if (sphere_in(*light_frustum, c, r) &&
                       (!camera || shadow_in(*camera, c, r, down, to_leave(*area, c, down) + r))) {
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
    RenderDebugStats3D& stats = ctx.stats;
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
    const shadow_reach::Ray sun_ray{lights.shadow_area, down};
    const auto cut = [&](u32 j) {
        ClusterJob& job = _cluster_jobs[j];
        const DrawRef& ref = _refs[job.ref];
        const LodView& lod = ctx.lod[ref.scene];
        const Camera3D& camera = ctx.scenes[ref.scene]->camera;
        ClusterCutView3D view;
        view.transform = ref.instances[job.copy].transform;
        view.eye = lod.eye;
        view.forward = lod.forward;
        view.orthographic = lod.orthographic;
        view.per_unit = lod.per_unit;
        view.pixels = lod.pixels;
        view.nearest = camera.z_near;
        view.force_depth = lod.force;
        const ClusterViews cull{ref.frustum,
                                ref.caster,
                                (job.visible & camera_bit) != 0,
                                job.visible,
                                down,
                                &lights.shadow_area,
                                ctx.cascade_frustums.data(),
                                ctx.slice_frustums.data(),
                                &ctx.light_frustum,
                                ctx.cascades,
                                &sun_ray};
        thread_local std::vector<u8> seen;
        thread_local std::vector<u32> stack;
        select_clusters(*ref.mesh->clusters, view, cull, job.picks, seen, stack, &job.tested);
    };
    // Measured on the kiln (13 copies, one of them nearly all of the clusters): on workers the cut took 1.1 ms against
    // 0.8 serial, the dispatch costing more than the little work that can be shared; from a couple of dozen copies it
    // pays.
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
        const MeshClusters3D& hierarchy = *_refs[job.ref].mesh->clusters;
        std::array<u32, cluster_views> triangles{}, clusters{};
        for (const SelectedCluster3D& pick : job.picks) {
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
                for (const SelectedCluster3D& pick : job.picks) {
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
                const MeshClusters3D& hierarchy = *_refs[job.ref].mesh->clusters;
                std::array<u32, cluster_views> at{};
                for (u32 v = 0; v < views; ++v) {
                    at[v] = job.runs[v].first;
                }
                for (const SelectedCluster3D& pick : job.picks) {
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
    u32 occ[4]; // x the pass (1, 2), y where the statistics start in the counts, z the test is on, w the pixel margin
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
// Workgroups that gather one mesh's chosen clusters: each takes every n-th (a few thousand clusters are chosen of tens
// of thousands).
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
    return _gc.active && _gcb[set].indices ? _gcb[set].indices->handle()
           : _cluster_indices              ? _cluster_indices->handle()
                                           : nullptr;
}

// The GPU cut's set-up on the CPU: the copies to cut as a table, the sizes of the buffers; the compute is recorded by
// cluster_pass_gpu once the frame's command buffer exists. Clears ctx.gpu_clusters when it cannot (the CPU then cuts).
void GpuRenderer3D::cut_clusters_gpu(FrameCtx& ctx, std::size_t count) {
    const auto started = std::chrono::steady_clock::now();
    RenderDebugStats3D& stats = ctx.stats;
    GcFrame& gc = _gc;
    gc.active = false;
    gc.dispatches.clear();
    gc.bases.clear();
    const u32 views = ctx.shadows ? 1 + std::max(ctx.cascades > 1 ? ctx.cascades : 1u, 1u) : 1;
    // Copies of meshes in one chunk go together (one dispatch covers them): the copies sorted by chunk, keeping their
    // order within it, so that a draw's copies stay one run.
    for (std::size_t j = 0; j < count; ++j) {
        if (!_refs[_cluster_jobs[j].ref].mesh->chunk) {
            ctx.gpu_clusters = false; // a mesh made without the GPU's chunk
            return;
        }
        _refs[_cluster_jobs[j].ref].job_count = 0;
    }
    std::stable_sort(_cluster_jobs.begin(), _cluster_jobs.begin() + static_cast<std::ptrdiff_t>(count),
                     [&](const ClusterJob& a, const ClusterJob& b) {
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
        const MeshClusters3D& hierarchy = *ref.mesh->clusters;
        const u32 per_copy = static_cast<u32>(hierarchy.clusters.size());
        if (gc.dispatches.empty() || gc.dispatches.back().chunk != ref.mesh->chunk.get()) {
            gc.dispatches.push_back(
                {ref.mesh->chunk.get(), static_cast<u32>(j), 0, 0, static_cast<u32>(clusters), 0, 0});
        }
        GcDispatch& d = gc.dispatches.back();
        const Mat4f& transform = ref.instances[job.copy].transform;
        jobs[j] = {job.slot,
                   ref.scene | (ref.caster ? 0x100u : 0u),
                   job.visible,
                   static_cast<u32>(clusters),
                   cluster_scale_of(transform),
                   per_copy,
                   ref.mesh->table_first,
                   d.threads};
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
        constexpr SDL_GPUBufferUsageFlags read_write =
            SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
        ensure_buffer(_gcb[0].scan, static_cast<u32>(elements * 4), 64 * 1024, read_write);
        ensure_buffer(_gcb[0].blocks, static_cast<u32>(((elements + 1023) / 1024) * 4), 4096, read_write);
        ensure_buffer(_gcb[0].args, static_cast<u32>(views * count * 20), 4096,
                      SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        ensure_buffer(_gcb[0].list, static_cast<u32>(queued * 4), 64 * 1024, SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        // The counters (eight statistics, then a queue's length each), zeroed by the upload.
        const u32 counts_bytes = static_cast<u32>((8 + gc.dispatches.size()) * 4);
        ensure_buffer(_gcb[0].counts, counts_bytes, 1024,
                      SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        gc.zeros.assign(counts_bytes, 0);
        _device->upload_storage_buffer(_gcb[0].counts->handle(), 0, counts_bytes, gc.zeros.data(), /*cycle=*/true);
        ensure_buffer(_gcb[0].indices, static_cast<u32>(room * 12), 1024 * 1024,
                      SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
        gc.table_bytes = job_bytes;
        gc.occlusion = false;
        if (ctx.occlusion) {
            // Lod3D::occlusion (G3d). The history of which clusters showed last frame has a word for each cluster of
            // each copy of each clustered draw of the scene, in the order of the draws; a draw that comes or goes (or a
            // mesh or a count that changes) makes a new layout, and a new layout starts from nothing showing (the first
            // pass then draws nothing and the second tests everything, as in a first frame). A stale history costs
            // speed, never a wrong picture.
            GpuView3D& view = *ctx.view;
            std::vector<GpuView3D::OccRegion> layout;
            u64 words = 0;
            for (DrawRef& ref : _refs) {
                if (!ref.clustered || ref.scene != 0) {
                    continue;
                }
                const u32 per_copy = static_cast<u32>(ref.mesh->clusters->clusters.size());
                layout.push_back({0, static_cast<u32>(ref.draw - ctx.scenes[0]->draws().data()), ref.mesh->serial,
                                  ref.draw->count, per_copy});
                ref.hist_first = static_cast<u32>(std::min<u64>(words, ~0u));
                words += static_cast<u64>(ref.draw->count) * per_copy;
            }
            constexpr u64 max_history_words = 32'000'000;
            if (words == 0 || words > max_history_words) {
                // No clustered draw in the scene itself (the overlay's are never tested): nothing to test, so off
                // without a word.
                if (words != 0) {
                    warn_once(
                        _occlusion_history_warned,
                        "3D: occlusion culling: too many clusters in the scene's copies for the history (32 M): off");
                }
                ctx.occlusion = false;
                ctx.occlusion_said = true; // cull_and_upload's line is for the other reasons
            } else {
                const u32 history_bytes = static_cast<u32>(words * 4);
                const bool same = layout == view._occ_layout && !view._occ_reset && view._occ_history[0] &&
                                  view._occ_history[1] && view._occ_words == words;
                if (!same) {
                    constexpr SDL_GPUBufferUsageFlags read_write =
                        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
                    for (std::shared_ptr<GpuBuffer>& h : view._occ_history) {
                        if (!h || h->size() < history_bytes) {
                            h = std::make_shared<GpuBuffer>(
                                _device->create_buffer(read_write, nullptr, std::max(history_bytes, 1024u)));
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
                    gc.job_history[j] =
                        ref.hist_first + job.copy * static_cast<u32>(ref.mesh->clusters->clusters.size());
                }
                ensure_buffer(_gc_job_history, static_cast<u32>(count * 4), 4096,
                              SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ);
                _device->upload_storage_buffer(_gc_job_history->handle(), 0, static_cast<u32>(count * 4),
                                               gc.job_history.data(), /*cycle=*/true);
                // The second pass: the camera's view alone, so queues and rooms for one view.
                gc.dispatches_second = gc.dispatches;
                u64 queued_second = 0;
                for (GcDispatch& d : gc.dispatches_second) {
                    d.queue = static_cast<u32>(queued_second);
                    d.queue_size = d.threads;
                    queued_second += d.queue_size;
                }
                const u64 room_second =
                    std::clamp<u64>(std::min<u64>(ctx.scene.lod.max_cluster_triangles, bound), 1, 100'000'000);
                constexpr SDL_GPUBufferUsageFlags read_write =
                    SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
                GcBuffers& second = _gcb[1];
                ensure_buffer(second.scan, static_cast<u32>((clusters + 1) * 4), 64 * 1024, read_write);
                ensure_buffer(second.blocks, static_cast<u32>(((clusters + 1 + 1023) / 1024) * 4), 4096, read_write);
                ensure_buffer(second.args, static_cast<u32>(count * 20), 4096,
                              SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
                ensure_buffer(second.list, static_cast<u32>(queued_second * 4), 64 * 1024,
                              SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
                // The counters, and after the queues' lengths four statistics (clusters and triangles occluded, then
                // wasted).
                const u32 second_counts = static_cast<u32>((8 + gc.dispatches_second.size() + 4) * 4);
                ensure_buffer(second.counts, second_counts, 1024, read_write);
                gc.zeros.assign(second_counts, 0);
                _device->upload_storage_buffer(second.counts->handle(), 0, second_counts, gc.zeros.data(),
                                               /*cycle=*/true);
                ensure_buffer(second.indices, static_cast<u32>(room_second * 12), 1024 * 1024,
                              SDL_GPU_BUFFERUSAGE_INDEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
                gc.room_second = static_cast<u32>(room_second);
                gc.occlusion = true;
            }
        }
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "3D: the cluster cut in compute could not allocate: the CPU cuts",
                       (LogFields{{.name = "reason", .value = e.what()}}));
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

// The depth pyramid of Lod3D::occlusion from a single-sample depth (the pre-pass's, or the occluder pass's): a first
// level a quarter of the depth's size (4 x 4 pixels a texel), then each half the one before until a level is at most 2
// x 2, the farthest depth (reverse Z: the smallest) of what it covers. A pass makes the first level and a pass each
// four more.
bool GpuRenderer3D::ensure_pyramid(GpuView3D& view, u32 w, u32 h,
                                   std::array<std::pair<u32, u32>, GpuView3D::max_hiz_levels>& size, u32& levels) {
    levels = 0;
    for (u32 lw = (w + 3) / 4, lh = (h + 3) / 4;; lw = (lw + 1) / 2, lh = (lh + 1) / 2) {
        size[levels++] = {lw, lh};
        if ((lw <= 2 && lh <= 2) || levels == GpuView3D::max_hiz_levels) {
            break;
        }
    }
    if (view._hiz_levels != levels || !view._hiz[0] || view._hiz[0].width() != size[0].first ||
        view._hiz[0].height() != size[0].second) {
        try {
            view._hiz = {};
            for (u32 i = 0; i < levels; ++i) {
                view._hiz[i] =
                    _device->create_target(size[i].first, size[i].second, hiz_format,
                                           SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE);
            }
            view._hiz_levels = levels;
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: occlusion culling: the depth pyramid could not be made",
                           (LogFields{{.name = "reason", .value = e.what()}}));
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
    if (!compute_pass(commands, _hiz_depth_pipeline, view._hiz[0],
                      std::span<const SDL_GPUTextureSamplerBinding>{&source, 1}, first, sizeof(first))) {
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

// Lod3D::occlusion without a pre-pass: the frame's opaque meshes (and the first pass's clusters) into a depth of their
// own, the pyramid from it, and the second pass of the cut. With a pre-pass, prepass() does this between its two
// halves.
void GpuRenderer3D::ensure_occluder_depth(GpuView3D& view) {
    const u32 w = static_cast<u32>(view._size.x), h = static_cast<u32>(view._size.y);
    if (!view._occ_depth || view._occ_depth.width() != w || view._occ_depth.height() != h) {
        try {
            view._occ_depth = _device->create_target(
                w, h, _ao_depth_format, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
        } catch (const std::exception& e) {
            KIN_LOG_WARN_F("render", "3D: occlusion culling: the occluder depth could not be made",
                           (LogFields{{.name = "reason", .value = e.what()}}));
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
        const SDL_GPUDepthStencilTargetInfo depth =
            depth_target(view->_occ_depth.handle(), SDL_GPU_STOREOP_STORE, /*cycle=*/true);
        if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth)) {
            drawn = true;
            set_viewport(pass, view->_size.x, view->_size.y);
            DrawUniforms u{};
            std::memcpy(u.view_projection, ctx.drawn[0].m, sizeof(u.view_projection));
            // the shadow vertex shaders' second buffer is a list this does not use; a skinned copy's also read the
            // joints
            const std::array<SDL_GPUBuffer*, 4> buffers{storage, storage, ctx.joints, ctx.joint_offsets};
            DrawBinder binder{pass};
            for (const u32 index : _order) {
                const DrawRef& ref = _refs[index];
                if (ref.scene != 0 || pass_of(ref.draw->material) != Pass::Opaque ||
                    dynamic_cast<const GpuMaterialShader3D*>(ref.draw->material.shader.backend().get())) {
                    continue; // a shader of its own may discard where the pyramid would take the mesh for solid
                }
                if (ref.skinned && (!ctx.joints || !ctx.joint_offsets)) {
                    continue;
                }
                const Material3D& occluder = ref.draw->material;
                const bool cutout = occluder.blend == Blend3D::Mask;
                SDL_GPUGraphicsPipeline* p = pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, !occluder.double_sided,
                                                      Pass::Occluder, cutout, nullptr, false, ref.skinned);
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
                    (void)binder.draw(ref.mesh->levels[l].index_count, ref.level_seen[l],
                                      ref.mesh->levels[l].first_index);
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
    // Set 0 is the whole cut (all views); with occlusion, phase 1 makes it (its camera view the first pass's: what
    // showed last frame) and phase 2 makes set 1: the camera's view alone, the rest tested against the pyramid.
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
    const Bounds3D& area = lights.shadow_area;
    const f32 lo[4] = {area.min.x, area.min.y, area.min.z, 0.0f}, hi[4] = {area.max.x, area.max.y, area.max.z, 0.0f};
    std::memcpy(p.area_lo, lo, sizeof(lo));
    std::memcpy(p.area_hi, hi, sizeof(hi));
    p.shadow[0] = ctx.cascades;
    p.shadow[1] = ctx.shadows ? 1u : 0u;
    const Occlusion3D& setting = ctx.scene.lod.occlusion;
    if (occ) {
        std::memcpy(p.view_projection, ctx.drawn[0].m, sizeof(p.view_projection));
        const f32 screen[4] = {static_cast<f32>(view._size.x), static_cast<f32>(view._size.y),
                               static_cast<f32>(view._hiz_levels), std::clamp(setting.depth_margin, 0.0f, 0.5f)};
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
        SDL_GPUStorageBufferReadWriteBinding rw[4] = {
            {buf.scan->handle(), /*cycle=*/true}, {buf.list->handle(), true}, {buf.counts->handle(), false}, {}};
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
                    levels[i] = {have ? view._hiz[i].handle() : _owner->_white.handle(),
                                 have ? _point_sampler : _owner->_sampler_linear};
                }
                SDL_BindGPUComputeSamplers(pass, 0, levels, GpuView3D::max_hiz_levels);
            }
            u32 number = 0;
            for (const GcDispatch& d : plan) {
                SDL_GPUBuffer* ro[5] = {d.chunk->table, _gc_jobs->handle(), ctx.storage,
                                        occ ? _gc_job_history->handle() : nullptr,
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
        const SDL_GPUStorageBufferReadWriteBinding rw[2] = {{buf.scan->handle(), false},
                                                            {buf.blocks->handle(), mode == 0}};
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
        const SDL_GPUStorageBufferReadWriteBinding rw[2] = {{buf.indices->handle(), true},
                                                            {buf.counts->handle(), false}};
        if (SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, rw, 2)) {
            SDL_BindGPUComputePipeline(pass, _gc_gather);
            u32 number = 0;
            for (const GcDispatch& d : plan) {
                SDL_GPUBuffer* ro[5] = {d.chunk->table, d.chunk->indices, buf.scan->handle(), buf.list->handle(),
                                        _gc_jobs->handle()};
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
void GpuRenderer3D::draw_cluster_indirect(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
                                          Uniforms& u, u32 view, u32 set) const {
    if (ref.job_count == 0) {
        return;
    }
    u.first[0] = 0;
    u.first[1] = 0;
    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, _gcb[set].args->handle(), (view * _gc.jobs + ref.job_first) * 20u,
                                         ref.job_count);
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
    RenderDebugStats3D& stats = ctx.stats;
    bool complete = true;
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
        } else
            complete = false;
    }
    stats.triangles_complete = complete;
    if (stats.cluster_overflow > 0 && !_gc_overflow_warned) {
        _gc_overflow_warned = true;
        KIN_LOG_WARN_F("render", "3D: the cluster cut passed Lod3D::max_cluster_triangles: some clusters are not drawn",
                       (LogFields{{.name = "dropped_clusters", .value = std::to_string(v[2])},
                                  {.name = "dropped_triangles", .value = std::to_string(v[3])},
                                  {.name = "room", .value = std::to_string(_gc.room)}}));
    }
}

bool GpuRenderer3D::read_cluster_draws(IViewBackend3D&, std::vector<ClusterDraw3D>& out) {
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

bool GpuRenderer3D::read_cluster_picks(IViewBackend3D&, std::vector<ClusterPick3D>& out) {
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
        if (!_device->read_buffer(_gcb[set].scan->handle(), 0, static_cast<u32>((elements + 1) * 4), bytes) ||
            bytes.size() < (elements + 1) * 4) {
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
        return m.wireframe                                              ? Pass::Wire
               : m.blend == Blend3D::Opaque || m.blend == Blend3D::Mask ? Pass::Opaque
               : m.blend == Blend3D::Alpha                              ? Pass::Alpha
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
    // Sort compact values instead of repeatedly chasing large draw/material
    // records. Equal keys still keep input order; alpha uses only depth after
    // scene/pass, while the other passes use culling and mesh serial.
    struct SortRef {
        u64 group, mesh;
        f32 distance;
        u32 ref;
    };
    std::vector<SortRef> sorted;
    sorted.reserve(_order.size());
    for (const u32 r : _order) {
        const DrawRef& ref = _refs[r];
        const Pass pass = pass_of(ref.draw->material);
        const bool alpha = pass == Pass::Alpha;
        const u64 group = (u64(ref.scene) << 32) | (u64(pass) << 1) | u64(!alpha && ref.draw->material.double_sided);
        sorted.push_back({group, ref.mesh->serial, alpha ? draw_distance(ref) : 0.0f, r});
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const SortRef& a, const SortRef& b) {
        if (a.group != b.group)
            return a.group < b.group;
        if ((a.group & 0xffffffffu) == (u64(Pass::Alpha) << 1))
            return a.distance > b.distance;
        return a.mesh < b.mesh;
    });
    for (std::size_t i = 0; i < sorted.size(); ++i)
        _order[i] = sorted[i].ref;
    // The overlay in front of the world, seen by its own camera, and the velocity pass for it.
    ctx.overlaid = std::any_of(_order.begin(), _order.end(), [&](u32 r) { return _refs[r].scene == 1; });
}

void GpuRenderer3D::choose_point_lights(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    const Lights3D& lights = scene.lights;
    RenderDebugStats3D& stats = ctx.stats;
    // Point lights: those reaching the view, strongest and nearest first.
    for (const PointLightSource3D& light : lights.points) {
        if (light.radius <= 0 || !sphere_in(ctx.frustums[0], light.position, light.radius)) {
            continue;
        }
        // What it adds a unit from the camera's nearest surface it reaches:
        // its intensity over the square of the distance, at least a unit.
        const f32 strength = std::max({light.color.r, light.color.g, light.color.b});
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
void GpuRenderer3D::push_frame_uniforms(FrameCtx& ctx, u32 s, u32 slot, bool record_history) {
    const Scene3D& scene = ctx.scene;
    const Lights3D& lights = scene.lights;
    const GpuView3D* view = ctx.view;
    // A temporally anti-aliased picture is drawn in linear light into float and encoded after the resolve.
    const bool encode = !ctx.taa && view->_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // a gamma target
    const bool decode =
        _owner->_color_texture_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; // textures stored encoded
    FrameUniforms f{};
    const Mat4f to_space = s == 0 ? Mat4f{} : ctx.scenes[1]->camera.view().inverse() * scene.camera.view();
    put(f.sun_direction, normalize(to_space.apply_vector(normalize(lights.sun.direction))), 0.0f);
    put_color(f.sun_color, lights.sun.color, 0.0f);
    put_color(f.ambient, lights.ambient, 0.0f);
    put_color(f.ground_ambient, (lights.ground_ambient.enabled ? lights.ground_ambient.color : lights.ambient), 0.0f);
    const bool fill = length_squared(lights.fill.direction) > 0;
    put(f.fill_direction, fill ? normalize(to_space.apply_vector(normalize(lights.fill.direction))) : Vec3f{}, 0.0f);
    put_color(f.fill_color, fill ? lights.fill.color : LinearRgb{}, 0.0f);
    const bool lit_by_sun_shadow = ctx.shadows && s == 0;
    put(f.flags, {encode ? 1.0f : 0.0f, decode ? 1.0f : 0.0f, lit_by_sun_shadow ? 1.0f : 0.0f},
        static_cast<f32>(ctx.chosen.size()));
    put(f.eye, ctx.scenes[s]->camera.position, 1.0f);
    if (ctx.environment) {
        // The picture turned by the light's rotation; in the overlay's space the
        // directions come back to the scene's first.
        const Mat4f to_environment = Mat4f::rotation_y(ctx.sky_env_used ? 0.0f : -lights.environment_rotation) *
                                     (s == 0 ? Mat4f{} : to_space.inverse());
        put(f.environment, {lights.environment_intensity, static_cast<f32>(ctx.environment->levels - 1), 0.0f}, 1.0f);
        std::memcpy(f.environment_matrix, to_environment.m, sizeof(f.environment_matrix));
    }
    // Receiver bias in each cascade's own texels; the shader optionally
    // corrects each PCF tap for the geometric receiver plane.
    put(f.shadow,
        {static_cast<f32>(std::max(ctx.cascades, 1u)), lights.shadow_receiver_plane_bias ? 1.0f : 0.0f,
         _shadow_size > 0 ? 1.0f / static_cast<f32>(_shadow_size) : 0.0f},
        0.0f);
    put(f.cascade_forward, scene.camera.forward(), 0.0f);
    // The overlay is not occluded: it is not in the pre-pass (the world's
    // occlusion would darken a gun by what is behind it, not by its own creases).
    put(f.occlusion,
        {ctx.occluded && s == 0 ? 1.0f : 0.0f, ctx.contacted && s == 0 ? 1.0f : 0.0f,
         1.0f / static_cast<f32>(view->_size.x)},
        1.0f / static_cast<f32>(view->_size.y));
    if (ctx.cascades > 1) {
        for (u32 k = 0; k < ctx.cascades; ++k) {
            const ShadowCascade3D& c = ctx.plan.cascade[k];
            std::memcpy(f.shadow_matrix + 16 * k, c.view_projection.m, 16 * sizeof(f32));
            f.cascade_bias[4 * k] = std::max(lights.shadow_bias, 0.0f) * c.texel_world / c.depth_range;
            f.cascade_bias[4 * k + 1] = std::max(lights.shadow_normal_offset, 0.0f) * c.texel_world;
            f.cascade_end[k] = c.slice_far;
            f.cascade_blend[k] = c.blend_from;
        }
    } else {
        std::memcpy(f.shadow_matrix, ctx.light_vp.m, 16 * sizeof(f32));
        f.cascade_bias[0] = std::max(lights.shadow_bias, 0.0f) * ctx.texel_world / ctx.depth_range;
        f.cascade_bias[1] = std::max(lights.shadow_normal_offset, 0.0f) * ctx.texel_world;
    }
    for (std::size_t i = 0; i < ctx.chosen.size(); ++i) {
        put(f.lights + i * 8, to_space.apply(ctx.chosen[i].second->position), ctx.chosen[i].second->radius);
        const f32 size = std::max(ctx.chosen[i].second->size, 0.01f);
        put_color(f.lights + i * 8 + 4, ctx.chosen[i].second->color, size * size);
    }
    // The probes are in the scene's world: the overlay keeps the sky's light.
    if (ctx.gi_resolved && s == 0) {
        f.gi[0] = 1.0f;
    }

    SDL_PushGPUFragmentUniformData(ctx.commands, slot, &f, sizeof(f));
}

void GpuRenderer3D::sky_environment(FrameCtx& ctx) {
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
    const Sky3D& sky = scene.sky;
    stats.sky_env_generations = _sky_env_generations;
    if (!scene.lights.sky_lights_scene || sky.mode != SkyMode3D::Procedural || !_sky_env_shader || !_filter_pipeline ||
        !_environment_sampler) {
        return;
    }
    // What the panorama is a function of: the sun and moon, the haze, the intensities and the colours. It is made again
    // when any of them has moved by more than the threshold, and not sooner than `light_interval` renders after the
    // last time.
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
        KIN_LOG_WARN_F("render", "3D: the sky's environment not made",
                       (LogFields{{.name = "reason", .value = e.what()}}));
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

// The sky triangle into `pass`, which is at the scene's viewport: mode 0 the colour, 1 the colour and the velocity (the
// main pass of temporal anti-aliasing with no pre-pass), 2 the velocity alone (into the pre-pass's, over its depth).
void GpuRenderer3D::draw_sky(FrameCtx& ctx, SDL_GPURenderPass* pass, u32 mode) {
    const GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    const Sky3D& sky = scene.sky;
    SDL_GPUGraphicsPipeline* p =
        mode == 2 ? sky_velocity_pipeline(_ao_depth_format)
                  : sky_pipeline(ctx.taa ? hdr_format : view->_format, ctx.taa ? 1u : view->_samples, mode == 1);
    if (!p) {
        return;
    }
    const bool orthographic = scene.camera.projection == Projection3D::Orthographic;
    SkyUniforms u{};
    // Where each pixel looks: the jittered view-projection (the meshes are drawn with it, and the sky's samples must be
    // where theirs are, or the edge between them would crawl under temporal anti-aliasing), unprojected at the near
    // plane.
    std::memcpy(u.inverse_view_projection, ctx.drawn[0].inverse().m, sizeof(u.inverse_view_projection));
    std::memcpy(u.view_projection, ctx.view_projections[0].m, sizeof(u.view_projection));
    std::memcpy(u.prior_view_projection, (ctx.taa_reset || !ctx.taa ? ctx.view_projections : view->_taa_prior)[0].m,
                sizeof(u.prior_view_projection));
    const bool panorama = sky.mode == SkyMode3D::Image && ctx.environment;
    put(u.target, {static_cast<f32>(view->_size.x), static_cast<f32>(view->_size.y), panorama ? 1.0f : 0.0f},
        orthographic ? 1.0f : 0.0f);
    put(u.camera, scene.camera.position, 1.0f);
    put(u.forward, scene.camera.forward(), 0.0f);
    constexpr f32 degrees_to_radians = 3.14159265f / 180.0f;
    put(u.disc,
        {std::max(sky.sun_size, 0.01f) * 0.5f * degrees_to_radians,
         std::max(sky.moon_size, 0.01f) * 0.5f * degrees_to_radians, sky.sun_disc},
        sky.stars ? sky.star_brightness : 0.0f);
    const bool moon = sky.moon && sky.moon_light > 0.0f;
    put_color(u.moon_color, sky.moon_color * (sky.moon_light * moon_disc_gain), moon ? 1.0f : 0.0f);
    fill_atmosphere(u.atmosphere, sky, scene.lights.sun.direction);
    if (ctx.fog) {
        fill_fog(u.fog, scene.fog, sky, orthographic);
    }
    if (mode != 2) {
        const SDL_GPUTextureSamplerBinding environment =
            ctx.environment && panorama
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
    RenderDebugStats3D& stats = ctx.stats;
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
    if ((!ctx.shadows || ctx.lights_variant) && _no_shadow && !_no_shadow_cleared) {
        const SDL_GPUDepthStencilTargetInfo clear = depth_target(_no_shadow.handle(), SDL_GPU_STOREOP_STORE);
        if (SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &clear)) {
            SDL_EndGPURenderPass(pass);
            _no_shadow_cleared = true;
        }
    }
}

void GpuRenderer3D::shadow_pass(FrameCtx& ctx) {
    RenderDebugStats3D& stats = ctx.stats;
    SDL_GPUCommandBuffer* commands = ctx.commands;
    SDL_GPUBuffer* storage = ctx.storage;
    // The shadow maps: casters (seen or casting into view) from the sun, a
    // pass to each cascade's layer.
    stats.shadow_cascades = ctx.shadows ? ctx.cascades : 0;
    if (ctx.sun_shadow_cached) {
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
        const SDL_GPUDepthStencilTargetInfo depth =
            depth_target(_shadow_map.handle(), SDL_GPU_STOREOP_STORE, /*cycle=*/false, k);
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, nullptr, 0, &depth);
        complete = complete && pass != nullptr;
        if (pass) {
            set_viewport(pass, _shadow_size, _shadow_size);
            DrawUniforms u{};
            std::memcpy(u.view_projection, (ctx.cascades > 1 ? ctx.plan.cascade[k].view_projection : ctx.light_vp).m,
                        sizeof(u.view_projection));
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
                SDL_GPUGraphicsPipeline* p = pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, !caster.double_sided,
                                                      Pass::Shadow, cutout, nullptr, false, ref.skinned);
                if (!p) {
                    continue;
                }
                // A skinned copy's shader also reads the frame's joint matrices and each copy's first.
                SDL_GPUBuffer* const skinned_storage[4] = {storage, shadow_storage[1], ctx.joints, ctx.joint_offsets};
                SDL_GPUBuffer* const retained_storage[2] = {
                    ref.retained != ~0u ? _retained[ref.retained].buffer->handle() : nullptr,
                    ctx.cascades > 1    ? shadow_storage[1]
                    : _retained_indices ? _retained_indices->handle()
                                        : nullptr};
                binder.bind_pipeline(p,
                                     ref.retained != ~0u ? retained_storage
                                     : ref.skinned       ? skinned_storage
                                                         : shadow_storage,
                                     ref.skinned ? 4 : 2);
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
                        (void)binder.draw(run.count, 1, run.first);
                        ++stats.shadow_draws;
                        ++stats.shadow_casters;
                        ++stats.cascade_casters[k];
                    }
                    continue;
                }
                // A draw for each level of detail the casters are at (the one level, without).
                for (u32 l = 0; l < ref.levels; ++l) {
                    const u32 count =
                        ctx.cascades > 1 ? ref.cascade_level_count[k][l] : ref.level_seen[l] + ref.level_extra[l];
                    if (count == 0) {
                        continue;
                    }
                    u.first[0] = ref.level_first[l];
                    u.first[1] = ctx.cascades > 1      ? ref.cascade_level_first[k][l] + 1
                                 : ref.retained != ~0u ? ref.level_first[l] + 1
                                                       : 0; // the list, or the run from level_first
                    SDL_PushGPUVertexUniformData(commands, 0, &u, sizeof(u));
                    (void)binder.draw(ref.mesh->levels[l].index_count, count, ref.mesh->levels[l].first_index);
                    ++stats.shadow_draws;
                    stats.shadow_casters += count;
                    stats.cascade_casters[k] += count;
                }
            }
            complete =
                draw_particle_shadows(ctx, pass, ctx.cascades > 1 ? ctx.plan.cascade[k].view_projection : ctx.light_vp,
                                      false, k) &&
                complete;
            SDL_EndGPURenderPass(pass);
        }
    }
    shadow_drawn(ctx, complete);
}

// The shadow cache (Lights3D::cache_shadows). The pass is a function of: the map, the cascades' views, the camera (it
// decides which casters reach the view, and the cascades' fit), the levels of detail, and each caster's mesh, material
// and copies. Everything but the copies is written into `key` (bytes, compared exactly); the copies, which are most of
// the data, are compared against a snapshot taken only after two renders agreed on the key and on a sample of them, so
// that a scene that moves costs a small key and the sample.
// The same ordered byte spans are compared on a cache hit and copied after a
// draw. Keep instance records followed by each posed copy's joint matrices.
template <class Consume> bool GpuRenderer3D::visit_shadow_copy_bytes(Consume&& consume) const {
    for (const DrawRef& ref : _refs) {
        if (!ref.caster)
            continue;
        if (!consume(ref.instances, static_cast<std::size_t>(ref.draw->count) * sizeof(MeshInstance3D)))
            return false;
        if (ref.skinned) {
            const std::size_t pose_bytes = ref.mesh->skin_bounds.size() * sizeof(Mat4f);
            for (u32 i = 0; i < ref.draw->count; ++i)
                if (!consume(ref.joints + ref.skin_first[i], pose_bytes))
                    return false;
        }
    }
    return true;
}

bool GpuRenderer3D::shadow_unchanged(FrameCtx& ctx) {
    ShadowCache& c = _shadow_cache;
    c.eligible = false;
    if (!ctx.scene.lights.cache_shadows && !ctx.shadow_history) {
        c.valid = false;
        c.prev_key.clear();
        return false;
    }
    std::vector<u8>& key = c.scratch;
    key.clear();
    CacheBytes payload{key};
    const Lights3D& lights = ctx.scene.lights;
    const Lod3D& lod = ctx.scene.lod;
    payload.value(_shadow_generation);
    const u32 maps = ctx.cascades > 1 ? ctx.cascades : 1;
    for (u32 i = 0; i < maps; ++i) {
        const auto* particles =
            particle_cast_view(ctx.cascades > 1 ? ctx.plan.cascade[i].view_projection : ctx.light_vp);
        if (particles && particles->unstable) {
            c.valid = false;
            c.prev_key.clear();
            return false;
        }
        payload.value(particles ? particles->key : u64{0});
    }
    payload.value(_shadow_size);
    payload.value(_shadow_layers);
    payload.value(ctx.cascades);
    payload.value(ctx.view->_size);
    payload.value(ctx.scene.cull);
    payload.value(ctx.gpu_clusters);
    payload.value(_gc.active);
    payload.value(lights.sun.direction);
    payload.value(lights.shadow_area);
    payload.value(lod.enabled);
    payload.value(lod.error_pixels);
    payload.value(lod.bias);
    payload.value(ctx.scene.debug.force_level);
    payload.value(ctx.scene.debug.gpu_culling);
    payload.value(lod.max_cluster_triangles);
    const LodView& lv = ctx.lod[0];
    payload.value(lv.on);
    payload.value(lv.force);
    payload.value(lv.orthographic);
    payload.value(lv.pixels);
    payload.value(lv.per_unit);
    // Mesh caster reach/LOD and cascade fitting depend on the camera. A
    // particle-only fixed map instead depends on its selected quad keys.
    if (ctx.cascades > 1 || std::any_of(_refs.begin(), _refs.end(), [](const auto& ref) { return ref.caster; })) {
        payload.value(lv.eye);
        payload.value(lv.forward);
        payload.matrix(ctx.view_projections[0]);
        payload.frustum(ctx.frustums[0]);
    }
    payload.matrix(ctx.light_vp);
    payload.frustum(ctx.light_frustum);
    if (ctx.cascades > 1) {
        for (u32 k = 0; k < ctx.cascades; ++k) {
            payload.matrix(ctx.plan.cascade[k].view_projection);
            payload.frustum(ctx.cascade_frustums[k]);
            payload.frustum(ctx.slice_frustums[k]);
        }
    }
    u64 sample = 0x9E3779B97F4A7C15ull;
    const auto mix = [&](u64 w) {
        sample = ((sample ^ w) * 0xFF51AFD7ED558CCDull);
        sample ^= sample >> 29;
    };
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
        payload.value(ref.mesh->serial); // a skinned mesh's skeleton is the mesh's (its skin, and so its joint count),
                                         // so this is its identity
        payload.value(ref.draw->count);
        payload.value(ref.levels);
        payload.value(ref.clustered);
        payload.value(ref.skinned);
        payload.value(ref.skinned ? ref.mesh->skin_bounds.size() : std::size_t{0});
        payload.value(m.double_sided);
        payload.value(m.blend);
        if (m.blend == Blend3D::Mask) {
            payload.value(m.alpha_cutoff);
            payload.value(m.wrap_u);
            payload.value(m.wrap_v);
            payload.value(texture);
            payload.value(texture ? texture->version() : u64{0});
        }
        // A sample of the copies: up to eight, spread over the draw, and its ends.
        const u32 n = ref.draw->count;
        const u32 step = std::max(1u, n / 8);
        const auto mix_instance = [&](const MeshInstance3D& instance) {
            for (std::size_t w = 0; w < sizeof(MeshInstance3D) / 8; ++w) {
                u64 word;
                std::memcpy(&word, reinterpret_cast<const u8*>(&instance) + w * 8, 8);
                mix(word);
            }
        };
        // A posed copy's pose is in the sample as well (the matrices are what moves the shadow, the copy's own placing
        // is the same).
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
    static_assert(sizeof(MeshInstance3D) % 8 == 0, "the sample reads whole words of a MeshInstance3D");
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
        const bool same = visit_shadow_copy_bytes([&](const void* data, std::size_t bytes) {
            if (at + bytes > c.copies.size() || std::memcmp(data, c.copies.data() + at, bytes) != 0)
                return false;
            at += bytes;
            return true;
        });
        if (same && at == c.copies.size()) {
            return ctx.scene.lights.cache_shadows;
        }
        c.valid = false;
        c.backoff =
            8; // the sample agreed and the copies did not: a scene that moves a little; do not copy it every render
    }
    return false;
}

void GpuRenderer3D::shadow_drawn(FrameCtx& ctx, bool complete) {
    ShadowCache& c = _shadow_cache;
    if (complete) {
        if (ctx.shadow_history && c.eligible) {
            u64 content = 1469598103934665603ull;
            taa_hash(content, c.scratch.data(), c.scratch.size());
            visit_shadow_copy_bytes([&](const void* bytes, std::size_t size) {
                taa_hash(content, bytes, size);
                return true;
            });
            _shadow_revision = content;
        } else {
            ++_shadow_revision; // invalidate future TAA readers without hashing unused content
        }
    }
    if (!ctx.scene.lights.cache_shadows) {
        c.valid = false;
        c.prev_key.clear();
        return;
    }
    if (!c.eligible || !complete) {
        c.valid = false;
        c.prev_key.clear();
        c.prev_sample = 0;
        return;
    }
    const bool still = !c.prev_key.empty() && c.prev_sample == c.now && c.prev_key == c.scratch;
    if (still && c.backoff == 0) {
        c.copies.clear();
        CacheBytes payload{c.copies};
        visit_shadow_copy_bytes([&](const void* data, std::size_t bytes) {
            payload.append(data, bytes);
            return true;
        });
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
// camera's view); `stats`, when given, counts them as the pre-pass's. With Lod3D::occlusion the camera's clusters are
// in two index buffers (the first pass's and the second's): `phase_parts` says which this pass draws.
template <typename Binder, typename Uniforms>
void GpuRenderer3D::draw_cluster_copies(const DrawRef& ref, SDL_GPUCommandBuffer* commands, SDL_GPURenderPass* pass,
                                        Binder& binder, Uniforms& u, RenderDebugStats3D* stats) const {
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
        (void)binder.draw(run.count, 1, run.first);
        if (stats) {
            ++stats->ao_draws;
            ++stats->ao_instances;
        }
    }
}

// The velocity pre-pass's draws of one scene's opaque meshes (the world's with the pre-pass, the overlay's alone).
void GpuRenderer3D::draw_velocity(FrameCtx& ctx, SDL_GPURenderPass* target_pass, u32 which,
                                  SDL_GPUTextureFormat depth_format) {
    RenderDebugStats3D& stats = ctx.stats;
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
    // A skinned copy's shader also reads the joints (the frame's, each copy's first), and those of the frame before
    // (and where each copy's own start in them).
    const std::array<SDL_GPUBuffer*, 6> buffers{storage,           previous_storage,    ctx.joints,
                                                ctx.joint_offsets, ctx.previous_joints, ctx.previous_joint_offsets};
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
            (void)binder.draw(ref.mesh->levels[l].index_count, ref.level_seen[l], ref.mesh->levels[l].first_index);
            if (which == 0) {
                ++stats.ao_draws;
                stats.ao_instances += ref.level_seen[l];
            }
        }
    }
}

// AO/contact shading has consumed the normals. Reuse R for coverage/irradiance,
// preserve the opaque motion marker in G, and write layer reprojection in BA.
// Replaying coverage also works with a velocity pre-pass and arbitrary custom
// fragment shaders without changing their ABI.
bool GpuRenderer3D::reactive_pass(FrameCtx& ctx) {
    auto* view = ctx.view;
    // Revisions belong to each view's history, not just the renderer's current
    // cache hit: another view may have updated the same map in between.
    const bool map_receivers = ctx.shadow_history;
    u64 revision = 1469598103934665603ull;
    if (ctx.shadows && map_receivers) {
        taa_hash(revision, _shadow_generation);
        taa_hash(revision, _shadow_revision);
    }
    for (const auto& local : _local_views) {
        if (!local.wanted || !map_receivers)
            continue;
        taa_hash(revision, local.cache);
        taa_hash(revision, _local_cache[local.cache].revision);
    }
    // Each used local map's fingerprint already includes its allocation
    // generation. Expiring an unused array must not reset another map's readers.
    taa_hash(revision, ctx.lu);
    taa_hash(revision, ctx.scene.lights.sun.color);
    taa_hash(revision, ctx.scene.lights.fill.color);
    taa_hash(revision, ctx.scene.lights.ambient);
    taa_hash(revision, ctx.scene.lights.ground_ambient.color);
    for (const auto& chosen : ctx.chosen) {
        taa_hash(revision, chosen.second->position);
        taa_hash(revision, chosen.second->color);
        taa_hash(revision, chosen.second->radius);
        taa_hash(revision, chosen.second->size);
    }
    ctx.shadow_revision = revision;
    ctx.shadow_changed = revision != view->_taa_shadow_revision;
    SDL_GPUColorTargetInfo target{};
    target.texture = view->_ao_normals.handle();
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    auto depth = depth_target(view->_taa_depth.handle(),
                              _taa_depth_sampled ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE, false);
    depth.load_op = SDL_GPU_LOADOP_LOAD;
    auto* pass = SDL_BeginGPURenderPass(ctx.commands, &target, 1, &depth);
    if (!pass)
        return false;
    set_viewport(pass, view->_size.x, view->_size.y, 0, ctx.overlaid ? overlay_split : 1);
    bool particles_ok = true;
    const auto particles = [&] {
        const auto before = ctx.stats.taa_reactive_draws;
        draw_particles(ctx, pass, true);
        particles_ok = ctx.stats.taa_reactive_draws - before == ctx.stats.particle_batches;
    };
    DrawBinder binder{pass};
    _phase = Phase::All;
    for (u32 which = 0; which < 2; ++which) {
        if (!ctx.scenes[which])
            continue;
        if (which == 1)
            particles(); // match colour composition: world meshes, particles, overlay
        set_viewport(pass, view->_size.x, view->_size.y, which ? overlay_split : 0,
                     !which && ctx.overlaid ? overlay_split : 1);
        VelocityUniforms u{};
        std::memcpy(u.view_projection, ctx.drawn[which].m, sizeof(u.view_projection));
        std::memcpy(u.current, ctx.view_projections[which].m, sizeof(u.current));
        std::memcpy(u.prior, view->_taa_prior[which].m, sizeof(u.prior));
        for (const auto index : _order) {
            const auto& ref = _refs[index];
            const auto& material = ref.draw->material;
            const bool custom = bool(material.shader.backend());
            if (ref.scene != which || pass_of(material) == Pass::Opaque)
                continue;
            if (ref.skinned && !(ctx.joints && ctx.joint_offsets))
                continue;
            if (ref.retained != ~0u) {
                SDL_EndGPURenderPass(pass);
                return false;
            }
            const auto coverage = material.wireframe ? Pass::ReactiveWire : Pass::Reactive;
            auto* pipeline = this->pipeline(hdr_format, 1, !material.double_sided, coverage, false, nullptr, false,
                                            ref.skinned, false, false, Unlit::Off, ref.retained != ~0u);
            if (!pipeline) {
                SDL_EndGPURenderPass(pass);
                return false;
            }
            const std::array<SDL_GPUBuffer*, 6> buffers{ctx.storage,         ctx.previous_storage,
                                                        ctx.joints,          ctx.joint_offsets,
                                                        ctx.previous_joints, ctx.previous_joint_offsets};
            binder.bind_pipeline(pipeline, buffers.data(), ref.skinned ? 6 : 2);
            binder.bind_mesh(ref.mesh, ref.clustered ? cluster_index_buffer() : nullptr);
            if (ref.skinned)
                binder.bind_skin(ref.mesh);
            const auto picture = texture_binding(material.texture, material.wrap_u, material.wrap_v);
            SDL_BindGPUFragmentSamplers(pass, 0, &picture, 1);
            u64 key = 1469598103934665603ull;
            taa_hash(key, ref.mesh->serial);
            const bool unstable = taa_material_hash(key, material, material.texture.backend().get());
            // Appearance follows the same per-copy match as velocity and skinning.
            // Anonymous draws sharing a mesh can disappear or reorder independently.
            const auto first = _taa_copy_refs[index].effect;
            bool changed = unstable;
            for (u32 i = 0; i < ref.draw->count; ++i) {
                u64 effect = key;
                taa_hash(effect, ref.instances[i].tint);
                view->_taa_now.effects[first + i] = effect;
                const auto previous = _taa_matched[first + i];
                changed |= previous >= view->_taa_last.effects.size() || view->_taa_last.effects[previous] != effect;
                taa_hash(ctx.coverage_key, effect);
                taa_hash(ctx.coverage_key, ref.instances[i].transform);
            }
            const Vec4f flags{custom ? 1.0f : 0.0f, changed ? 1.0f : 0.0f, 0, 0};
            SDL_PushGPUFragmentUniformData(ctx.commands, 0, &flags, sizeof(flags));
            if (ref.clustered) {
                draw_cluster_copies(ref, ctx.commands, pass, binder, u, nullptr);
                if (_gc.active) {
                    const auto parts = phase_parts(ref);
                    ctx.stats.taa_reactive_draws += (parts & 1 ? 1 : 0) + (parts & 2 ? 1 : 0);
                } else {
                    for (u32 n = 0; n < ref.level_seen[0]; ++n)
                        ctx.stats.taa_reactive_draws += _cluster_runs[ref.level_first[0] + n][0].count ? 1 : 0;
                }
                continue;
            }
            for (u32 level = 0; level < ref.levels; ++level) {
                if (!ref.level_seen[level])
                    continue;
                u.first[0] = ref.level_first[level];
                SDL_PushGPUVertexUniformData(ctx.commands, 0, &u, sizeof(u));
                const auto& mesh = ref.mesh->levels[level];
                (void)binder.draw(mesh.index_count, ref.level_seen[level], mesh.first_index);
                ++ctx.stats.taa_reactive_draws;
            }
        }
    }
    if (!ctx.scenes[1])
        particles();
    SDL_EndGPURenderPass(pass);
    return particles_ok;
}

void GpuRenderer3D::prepass(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    const Scene3D& scene = ctx.scene;
    RenderDebugStats3D& stats = ctx.stats;
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
    const bool want_contact = ctx.contact_wanted;
    if (want_contact && !_ao_available) {
        warn_once(_contact_warned, "3D: contact shadows asked for, but this device cannot do it: off");
    }
    ctx.previous_storage = ctx.taa && _previous ? _previous->handle() : nullptr;
    ctx.previous_joints = ctx.taa && _previous_joints ? _previous_joints->handle() : nullptr;
    ctx.previous_joint_offsets = ctx.taa && _previous_joint_offsets ? _previous_joint_offsets->handle() : nullptr;
    ctx.velocity_drawn = false; // the pre-pass began, so _ao_normals holds this frame's velocity
    // Only stock mesh shaders define the main pass's velocity output. Colour-only particle/custom shaders must
    // stay off that attachment: masking an undefined shader output does not preserve it on every backend.
    ctx.main_velocity = ctx.taa && !ao.enabled && !want_contact && !ctx.occlusion && !ctx.soft_particles &&
                        !ctx.gi_lit && _particle_batches.empty() && ctx.previous_storage &&
                        view->_ao_normals.format() == hdr_format &&
                        std::none_of(_refs.begin(), _refs.end(),
                                     [](const DrawRef& ref) { return ref.draw->material.shader.backend() != nullptr; });
    if (!ctx.main_velocity && (ao.enabled || want_contact || ctx.taa || ctx.soft_particles || ctx.gi_lit) &&
        _ao_available && (storage || ctx.taa || stats.retained_instances) &&
        ensure_ao_targets(*view, ao.half_resolution, ao.enabled, want_contact, ctx.taa)) {
        SDL_GPUColorTargetInfo target{};
        target.texture = view->_ao_normals.handle();
        target.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
        target.load_op = SDL_GPU_LOADOP_CLEAR;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.cycle = true;
        // Reverse Z: the far plane, which the compute reads as sky.
        const SDL_GPUDepthStencilTargetInfo prepass_depth =
            depth_target(view->_ao_depth.handle(), SDL_GPU_STOREOP_STORE, /*cycle=*/true);
        // With Lod3D::occlusion the pre-pass is two passes: the opaque meshes and the first pass's clusters, then (once
        // the depth pyramid is built from that and the second pass has tested the rest against it) the second pass's
        // clusters.
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
                SDL_GPUGraphicsPipeline* p =
                    pipeline(ao_normal_format, 1, !occluder.double_sided, Pass::Prepass, cutout, nullptr, false,
                             ref.skinned, false, false, Unlit::Off, ref.retained != ~0u);
                if (!p) {
                    continue;
                }
                const std::array<SDL_GPUBuffer*, 3> skinned_buffers{storage, ctx.joints, ctx.joint_offsets};
                const std::array<SDL_GPUBuffer*, 2> retained_buffers{
                    ref.retained != ~0u ? _retained[ref.retained].buffer->handle() : nullptr,
                    _retained_indices ? _retained_indices->handle() : nullptr};
                binder.bind_pipeline(p,
                                     ref.retained != ~0u ? retained_buffers.data()
                                     : ref.skinned       ? skinned_buffers.data()
                                                         : &storage,
                                     ref.retained != ~0u ? 2
                                     : ref.skinned       ? 3
                                                         : 1);
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
                    (void)binder.draw(ref.mesh->levels[l].index_count, ref.level_seen[l],
                                      ref.mesh->levels[l].first_index);
                    ++stats.ao_draws;
                    stats.ao_instances += ref.level_seen[l];
                }
            }
            // The camera's turn over the pixels nothing was drawn on (the far depth), so that the resolve reprojects
            // the sky.
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
            const bool merge = ao.enabled && want_contact && _ao_blur_both_pipeline &&
                               view->_ao_raw.width() == view->_contact_raw.width() &&
                               view->_ao_raw.height() == view->_contact_raw.height();
            _chain_ready = false;
            if (ao.enabled) {
                compute_occlusion(commands, *view, scene.camera, ctx.main_projection, ao, stats, !merge);
            }
            if (want_contact) {
                compute_contact_shadows(commands, *view, scene.camera, ctx.main_projection, contact,
                                        lights.sun.direction, stats, !merge);
            }
            if (merge) {
                blur_screen_space_both(commands, *view, ctx.main_projection, stats);
            }
            ctx.occluded = ao.enabled && stats.ao_dispatches > 0;
            ctx.contacted = want_contact && stats.contact_dispatches > 0;
            ctx.gi_resolved = ctx.gi_lit && gi_resolve(commands, *view, scene, ctx.main_projection, ctx.taa, stats);
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
        const SDL_GPUDepthStencilTargetInfo overlay_depth =
            depth_target(view->_taa_depth.handle(), SDL_GPU_STOREOP_DONT_CARE, /*cycle=*/true);
        if (SDL_GPURenderPass* gun = SDL_BeginGPURenderPass(commands, &target, 1, &overlay_depth)) {
            set_viewport(gun, view->_size.x, view->_size.y);
            draw_velocity(ctx, gun, 1, _depth_format);
            SDL_EndGPURenderPass(gun);
        }
    }
}

bool GpuRenderer3D::main_pass(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    RenderDebugStats3D& stats = ctx.stats;
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
    SDL_GPUTexture* resolved = ctx.taa      ? (view->_hdr ? view->_scene.handle() : view->_taa_in.handle())
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
    ctx.mesh_coverage = ctx.taa && std::any_of(_order.begin(), _order.end(), [&](u32 index) {
                            return pass_of(_refs[index].draw->material) != Pass::Opaque;
                        });
    ctx.taa_reactive = ctx.taa && (!_particle_batches.empty() || ctx.mesh_coverage);
    // Reverse Z: the far plane.
    SDL_GPUDepthStencilTargetInfo depth = depth_target(
        ctx.taa ? view->_taa_depth.handle() : view->_depth.handle(),
        ctx.taa_reactive || (ctx.taa && _taa_depth_sampled) ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE,
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
    // Fog (P3a): the scene's block, and for the overlay a block of none (a gun in the hand is not fogged). The block is
    // in slot 2, which a custom shader's parameters use too: it is pushed again before the first fogged draw after a
    // custom one.
    std::array<FogBlockUniforms, 2> fog_blocks{};
    if (ctx.fog) {
        const bool orthographic = ctx.scene.camera.projection == Projection3D::Orthographic;
        fill_fog(fog_blocks[0].fog, ctx.scene.fog, ctx.scene.sky, orthographic);
        fill_atmosphere(fog_blocks[0].sky, ctx.scene.sky, ctx.scene.lights.sun.direction);
        if (fog_blocks[0].fog.colour[3] > 0.5f) {
            // The sky's colour, from the table (made again only when the sky or the sun's height has changed).
            _fog_table.update(ctx.scene.sky, ctx.scene.lights.sun.direction);
            std::memcpy(fog_blocks[0].table, _fog_table.values.data(), sizeof(fog_blocks[0].table));
        }
        stats.fog_applied = true;
    }
    bool fog_bound = false;
    // The light variants' block (P3b): slot 3 after the fog's, else the fog's own slot 2, which a custom shader's
    // parameters take as well (pushed again before the first stock draw after one).
    const u32 lights_slot = ctx.fog ? 3u : 2u;
    bool lights_bound = false;
    bool custom_lights_bound = false;
    const auto begin_scene = [&](u32 s) {
        const f32 lo = s == 0 ? 0.0f : overlay_split, hi = s == 0 && ctx.overlaid ? overlay_split : 1.0f;
        set_viewport(pass, view->_size.x, view->_size.y, lo, hi);
        push_frame_uniforms(ctx, s);
        fog_bound = false;
        lights_bound = false;
        custom_lights_bound = false;
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
    const SDL_GPUTextureSamplerBinding occlusion_binding{
        ctx.occluded ? view->_ao_raw.handle() : _owner->_white.handle(), _owner->_sampler_linear};
    const SDL_GPUTextureSamplerBinding contact_binding{
        ctx.contacted ? view->_contact_raw.handle() : _owner->_white.handle(), _owner->_sampler_linear};
    const SDL_GPUTextureSamplerBinding shadow_binding{ctx.shadows ? _shadow_map.handle() : _no_shadow.handle(),
                                                      _compare_sampler};
    // The light variants' two more maps: the local array and the moon's (the dummy where there is none; never read
    // then).
    const bool local_maps = ctx.lights_variant && _local_map && !_local_views.empty();
    const SDL_GPUTextureSamplerBinding local_binding{local_maps ? _local_map.handle() : _no_shadow.handle(),
                                                     _compare_sampler};
    const SDL_GPUTextureSamplerBinding moon_binding{
        ctx.lights_variant && ctx.moon_cascades > 0 && _moon_map ? _moon_map.handle() : _no_shadow.handle(),
        _compare_sampler};
    const std::array<SDL_GPUTextureSamplerBinding, 2> gi_binding = gi_bindings(*view, ctx.gi_resolved);
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
    // The particles come last in the scene's own space: after the sky and every blended mesh, before the overlay (they
    // bind pipelines and uniforms of their own, so the binder and the texture cache forget what they had).
    bool particles_done = !ctx.particles;
    const auto particles_now = [&] {
        particles_done = true;
        draw_particles(ctx, pass);
        binder.reset();
        bound_textures = unbound;
        fog_bound = false;
        lights_bound = false;
    };
    bool sky_done = !ctx.sky_pass;
    const auto sky_now = [&] {
        sky_done = true;
        draw_sky(ctx, pass, ctx.main_velocity ? 1u : 0u);
        // The sky uses fragment slots 0/1 too. A blended draw can follow it
        // in the same scene, so restore the mesh frame before that draw.
        push_frame_uniforms(ctx, current_scene);
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
            if (!particles_done && ref.scene != 0) {
                particles_now();
            }
            if (ref.scene != current_scene) {
                current_scene = ref.scene;
                begin_scene(current_scene);
                fill_uniforms(draw_uniforms, current_scene);
            }
            const auto selection = material_pipeline(material, ref.skinned, ctx.taa, ctx.main_velocity, ctx.fog);
            const auto* custom = selection.custom;
            if (ref.skinned && (!ctx.joints || !ctx.joint_offsets)) {
                continue;
            }
            const Unlit unlit = selection.unlit;
            SDL_GPUGraphicsPipeline* p = pipeline(MeshPipelineRequest{
                ctx.taa ? hdr_format : view->_format, ctx.taa ? 1u : view->_samples, selection, ctx.main_velocity,
                ref.skinned, ctx.fog && !custom, ctx.lights_variant && !custom, ref.retained != ~0u});
            if (!p) {
                continue;
            }
            const bool lit_variant = ctx.lights_variant && !custom && unlit == Unlit::Off;
            // The velocity variant also has last frame's matrices, and the joints of the frame before with where each
            // copy's start.
            const bool skinned_velocity = ref.skinned && ctx.main_velocity && !custom;
            if (skinned_velocity && !(ctx.previous_joints && ctx.previous_joint_offsets)) {
                continue;
            }
            const std::array<SDL_GPUBuffer*, 6> skinned_buffers{storage, ctx.joints, ctx.joint_offsets,
                                                                nullptr, nullptr,    nullptr};
            const std::array<SDL_GPUBuffer*, 6> skinned_velocity_buffers{
                storage,           ctx.previous_storage, ctx.joints,
                ctx.joint_offsets, ctx.previous_joints,  ctx.previous_joint_offsets};
            const Textures textures{material.texture.backend().get(), material.metallic_roughness.backend().get(),
                                    material.normal.backend().get(), material.occlusion.backend().get(),
                                    material.emissive_texture.backend().get()};
            const std::array<SDL_GPUBuffer*, 2> retained_buffers{
                ref.retained != ~0u ? _retained[ref.retained].buffer->handle() : nullptr,
                _retained_indices ? _retained_indices->handle() : nullptr};
            if (binder.bind_pipeline(p,
                                     ref.retained != ~0u ? retained_buffers.data()
                                     : skinned_velocity  ? skinned_velocity_buffers.data()
                                     : ref.skinned       ? skinned_buffers.data()
                                                         : buffers.data(),
                                     ref.retained != ~0u            ? 2
                                     : skinned_velocity             ? 6
                                     : ref.skinned                  ? 3
                                     : ctx.main_velocity && !custom ? 2
                                                                    : 1)) {
                bound_textures = unbound;
            }
            const std::pair<Wrap3D, Wrap3D> wrap{material.wrap_u, material.wrap_v};
            if (textures != bound_textures || wrap != bound_wrap) {
                if (unlit == Unlit::Textured) {
                    const auto base = texture_binding(material.texture, material.wrap_u, material.wrap_v);
                    SDL_BindGPUFragmentSamplers(pass, 0, &base, 1);
                } else if (unlit == Unlit::Off) {
                    const std::array<SDL_GPUTextureSamplerBinding, 14> samplers{
                        texture_binding(material.texture, material.wrap_u, material.wrap_v),
                        shadow_binding,
                        texture_binding(material.metallic_roughness, material.wrap_u, material.wrap_v),
                        texture_binding(material.normal, material.wrap_u, material.wrap_v),
                        texture_binding(material.occlusion, material.wrap_u, material.wrap_v),
                        texture_binding(material.emissive_texture, material.wrap_u, material.wrap_v),
                        environment_bindings[0],
                        environment_bindings[1],
                        occlusion_binding,
                        contact_binding,
                        local_binding,
                        moon_binding,
                        gi_binding[0],
                        gi_binding[1]};
                    // Custom shaders have twelve; the stock light variants fourteen (global illumination's light).
                    SDL_BindGPUFragmentSamplers(pass, 0, samplers.data(), custom ? 12u : lit_variant ? 14u : 10u);
                }
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
            const MaterialUniforms m{
                {material.emissive, material.unlit || material.wireframe ? 1.0f : 0.0f,
                 material.receives_shadow ? 1.0f : 0.0f,
                 material.blend == Blend3D::Mask ? std::max(material.alpha_cutoff, 1e-6f) : 0.0f},
                {material.metallic, material.roughness, material.normal_scale, material.occlusion_strength},
                {material.normal ? 1.0f : 0.0f, material.emissive_texture ? 1.0f : 0.0f,
                 pass_of(material) == Pass::Opaque ? 1.0f : 0.0f, pass_of(material) == Pass::Additive ? 1.0f : 0.0f}};

            SDL_PushGPUFragmentUniformData(commands, 1, &m, sizeof(m));
            if (custom) {
                CustomFogUniforms custom_data;
                std::memcpy(custom_data.custom.values, material.shader_params.data(), sizeof(CustomUniforms));
                const bool fogged_custom = ctx.fog && current_scene == 0;
                if (fogged_custom) {
                    custom_data.fog = fog_blocks[current_scene];
                    custom_data.fog.fog.view[1] = 1.0f;
                } else {
                    custom_data.fog.fog = {};
                }
                SDL_PushGPUFragmentUniformData(commands, 2, &custom_data,
                                               fogged_custom ? sizeof(custom_data)
                                                             : sizeof(CustomUniforms) + sizeof(FogParamUniforms));
                if (!custom_lights_bound) {
                    push_local_lights(ctx, current_scene, 3);
                    custom_lights_bound = true;
                }
                fog_bound = false;
                // Stock fog+lights also uses slot 3; without fog, stock lights
                // uses slot 2, which the custom parameter/fog block replaced.
                lights_bound = ctx.fog;
            } else if (ctx.fog && !fog_bound) {
                SDL_PushGPUFragmentUniformData(commands, 2, &fog_blocks[current_scene], sizeof(FogBlockUniforms));
                fog_bound = true;
            }
            if (lit_variant && !lights_bound) {
                push_local_lights(ctx, current_scene, lights_slot);
                lights_bound = true;
            }
            // A clustered mesh: a draw for each copy, over its run of the frame's compacted indices.
            if (ref.clustered && _gc.active) {
                stats.triangles_complete = false;
                // With Lod3D::occlusion the camera's clusters are in two index buffers: those drawn before the pyramid,
                // and the rest.
                for (u32 set = 0; set < (_gc.occlusion ? 2u : 1u); ++set) {
                    binder.bind_mesh(ref.mesh, cluster_index_buffer(set));
                    draw_cluster_indirect(ref, commands, pass, draw_uniforms, 0, set);
                    ++stats.draws;
                    stats.unlit_draws += unlit != Unlit::Off ? 1u : 0u;
                    stats.unlit_textured_draws += unlit == Unlit::Textured ? 1u : 0u;
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
                    (void)binder.draw(run.count, 1, run.first);
                    ++stats.draws;
                    stats.unlit_draws += unlit != Unlit::Off ? 1u : 0u;
                    stats.unlit_textured_draws += unlit == Unlit::Textured ? 1u : 0u;
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
                (void)binder.draw(level.index_count, count, level.first_index);
                ++stats.draws;
                stats.unlit_draws += unlit != Unlit::Off ? 1u : 0u;
                stats.unlit_textured_draws += unlit == Unlit::Textured ? 1u : 0u;
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
    if (!particles_done) {
        particles_now();
    }
    if (ctx.gi && ctx.scene.debug.gi_view != GiView3D::Off) {
        // The GI scene instead of the picture (RenderDebugOptions3D::gi_view).
        draw_gi_view(commands, pass, *view, ctx.scene, ctx.drawn[0], ctx.taa ? hdr_format : view->_format,
                     ctx.taa ? 1u : view->_samples, ctx.main_velocity ? hdr_format : SDL_GPU_TEXTUREFORMAT_INVALID,
                     !ctx.taa && view->_format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    }
    SDL_EndGPURenderPass(pass);
    if (ctx.taa_reactive && !reactive_pass(ctx)) {
        // The colour pass is complete. Show it and reset temporal history;
        // an unavailable optional coverage pass must not drop the frame.
        ctx.velocity_drawn = false;
        ctx.taa_reset = true;
        view->_taa_valid = false;
        view->_taa_coverage_key = 0;
    }
    return true;
}

void GpuRenderer3D::resolve_history(FrameCtx& ctx) {
    GpuView3D* view = ctx.view;
    RenderDebugStats3D& stats = ctx.stats;
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
            warn_once(_taa_prepass_warned,
                      "3D: temporal anti-aliasing: the velocity pass could not begin, so this frame has no history");
        }
        // The main pass's depth (the overlay's in front, in its slice of the range): the resolve takes the velocity
        // of the nearest surface round each pixel at an edge.
        const bool depth = _taa_depth_sampled && view->_taa_depth;
        ctx.shown = resolve_taa(commands, *view, view->_hdr ? view->_scene : view->_taa_in, taa_settings,
                                ctx.taa_reset || !ctx.velocity_drawn, ctx.jitter, ctx.taa_reactive,
                                ctx.coverage_key != view->_taa_coverage_key, ctx.scene.debug.freeze_jitter,
                                depth ? &view->_taa_depth : nullptr, ctx.overlaid, stats);
        view->_taa_valid = ctx.velocity_drawn;
        for (u32 s = 0; s < 2; ++s) {
            if (ctx.scenes[s]) {
                view->_taa_prior[s] = ctx.view_projections[s];
            }
        }
        std::swap(view->_taa_last, view->_taa_now);
        view->_taa_particles.swap(_particle_history_now);
        view->_taa_shadow_revision = ctx.shadow_revision;
        view->_taa_coverage_key = ctx.coverage_key;
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
        const SDL_GPUDepthStencilTargetInfo pick_depth =
            depth_target(view->_pick_depth.handle(), SDL_GPU_STOREOP_DONT_CARE, /*cycle=*/true);
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
                SDL_GPUGraphicsPipeline* p =
                    pipeline(pick_format, 1, !picked.double_sided, Pass::Pick, cutout, nullptr, false, ref.skinned,
                             false, false, Unlit::Off, ref.retained != ~0u);
                if (!p) {
                    continue;
                }
                const std::array<SDL_GPUBuffer*, 3> skinned_buffers{storage, ctx.joints, ctx.joint_offsets};
                const std::array<SDL_GPUBuffer*, 2> retained_buffers{
                    ref.retained != ~0u ? _retained[ref.retained].buffer->handle() : nullptr,
                    _retained_indices ? _retained_indices->handle() : nullptr};
                binder.bind_pipeline(p,
                                     ref.retained != ~0u ? retained_buffers.data()
                                     : ref.skinned       ? skinned_buffers.data()
                                                         : &storage,
                                     ref.retained != ~0u ? 2
                                     : ref.skinned       ? 3
                                                         : 1);
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
                    (void)binder.draw(ref.mesh->levels[l].index_count, ref.level_seen[l],
                                      ref.mesh->levels[l].first_index);
                }
            }
            SDL_EndGPURenderPass(picking);
            view->_pick_valid = true;
        }
    }
}

bool GpuRenderer3D::render(IViewBackend3D& base, const Scene3D& scene, RenderDebugStats3D& stats) {
    struct CountWaits { // on every way out
        RenderDebugStats3D& stats;
        const GpuDevice& device;
        u64 before = device.idle_waits();
        ~CountWaits() { stats.device_waits = static_cast<u32>(device.idle_waits() - before); }
    } count_waits{stats, *_device};
    collect_mesh_buffers();
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
    choose_point_lights(ctx);
    choose_local_lights(ctx);
    cull_and_upload(ctx);
    prepare_particles(ctx);
    ctx.shadow_history = ctx.taa && std::ranges::any_of(_particle_batches, [](const auto& batch) {
                             return batch.look.receives_shadow && batch.look.lit > 0;
                         });
    order_draws(ctx);
    build_local_shadows(ctx);
    ctx.sun_shadow_cached = ctx.shadows && shadow_unchanged(ctx);
    upload_particle_casts(ctx);
    begin_commands(ctx);
    // Debug markers stay in the existing command buffer. External profilers can
    // timestamp them without the submission splits of the fence-based scopes.
    struct PassMarker {
        SDL_GPUCommandBuffer* commands;
        PassMarker(SDL_GPUCommandBuffer* buffer, bool enabled, const char* name)
            : commands(enabled ? buffer : nullptr) {
            if (commands)
                SDL_PushGPUDebugGroup(commands, name);
        }
        ~PassMarker() {
            if (commands)
                SDL_PopGPUDebugGroup(commands);
        }
    };
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.clusters"};
        cluster_pass_gpu(ctx, ctx.occlusion ? 1 : 0);
    }
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.gi_scene"};
        ctx.gi = gi_scene(ctx.commands, *view, scene, stats);
    }
    if (ctx.gi) {
        // The probes' light is read by the light variants of the stock shaders: where it is on, they draw.
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.gi_probes"};
        const Mat4f to_environment = Mat4f::rotation_y(ctx.sky_env_used ? 0.0f : -scene.lights.environment_rotation);
        ctx.gi_lit = _lights_available && gi_probes(ctx.commands, *view, scene, ctx.environment, to_environment, stats);
        ctx.lights_variant = ctx.lights_variant || ctx.gi_lit;
    }
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.sun_shadow"};
        shadow_pass(ctx);
    }
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.local_shadows"};
        local_shadow_pass(ctx);
    }
    stats.lights_variant = ctx.lights_variant;
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.prepass"};
        prepass(ctx);
    }
    stats.taa_main_velocity = ctx.main_velocity;
    if (ctx.occlusion && !ctx.occlusion_second) {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.occluders"};
        occluder_pass(ctx); // no pre-pass made the depth: a pass of its own
    }
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.overlay_velocity"};
        overlay_velocity(ctx);
    }
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.main"};
        if (!main_pass(ctx))
            return false;
    }
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.temporal_resolve"};
        resolve_history(ctx);
    }
    {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.picking"};
        pick_pass(ctx);
    }
    if (ctx.taa) {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.output"};
        finish_view(ctx.commands, *view, *ctx.shown, /*plain=*/!view->_hdr, ctx.sky_pass);
    } else if (view->_hdr) {
        const PassMarker marker{ctx.commands, _gpu_markers, "kin3d.output"};
        finish_view(ctx.commands, *view, view->_scene, /*plain=*/false, ctx.sky_pass);
    }
    age_targets(ctx);
    stats.pipelines_made = static_cast<u32>(_owner->_pipelines.size() - pipelines_before);
    resume_2d();
    if (ctx.occlusion && ctx.occlusion_second) {
        view->_occ_read ^= 1; // what the second pass wrote is what the next render's first pass reads
        stats.occlusion = true;
    }
    if (ctx.gpu_clusters && scene.debug.stats_readback) {
        read_cluster_stats(ctx);
    }
    if (_mesh_buffers) {
        const auto pooled = _mesh_buffers->stats();
        stats.shared_mesh_allocated_bytes = pooled.allocated;
        stats.shared_mesh_live_bytes = pooled.live;
        stats.shared_mesh_retired_bytes = pooled.retired;
        stats.shared_mesh_range_reuses = pooled.reused;
        stats.shared_mesh_pages = pooled.pages;
    }
    return true;
}

bool GpuRenderer3D::warm_up(IViewBackend3D& base, const Scene3D& scene) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view || !_vertex_shader || !_fragment_shader) {
        return false;
    }
    // What setup_frame, prepass and the passes would ask for on a first render of `scene`, asked for now (pipeline()
    // and the ensure_ functions make what is missing and keep it).
    _gpu_markers = scene.debug.gpu_pass_markers;
    if (view->_output_format != _owner->_target_format && !make_targets(*view)) {
        return false;
    }
    const Lights3D& lights = scene.lights;
    const bool sun_shines = lights.sun.color.r + lights.sun.color.g + lights.sun.color.b > 0.0f;
    const Bounds3D& area = lights.shadow_area;
    const Vec3f area_size = area.size();
    const bool shadows = sun_shines && lights.sun_shadow_strength > 0.0f && area_size.x > 0 && area_size.y >= 0 &&
                         area_size.z > 0 && length_squared(lights.sun.direction) > 0;
    const bool ao = scene.ambient_occlusion.enabled && _ao_available;
    const bool contact =
        scene.contact_shadows.enabled && sun_shines && lights.sun_shadow_strength > 0.0f && _ao_available;
    const bool occlusion = scene.lod.enabled && scene.debug.gpu_culling && scene.lod.occlusion.enabled &&
                           _occlusion_available && _gpu_clusters;
    const std::array<const Scene3D*, 2> scenes{&scene, scene.overlay_if_any()};
    const auto custom_of = [](const Material3D& m) {
        return dynamic_cast<const GpuMaterialShader3D*>(m.shader.backend().get());
    };
    bool custom_shader = false;
    for (const Scene3D* s : scenes) {
        if (!s) {
            continue;
        }
        for (const Scene3D::Draw& draw : s->draws()) {
            custom_shader = custom_shader || custom_of(draw.material);
        }
    }
    bool taa = scene.taa.enabled && _taa_available;
    const bool soft = _soft_particles_available && _ao_available && scene_has_soft_particles(scene);
    const bool particles = _particles_available && (!scene.particle_draws().empty() || scene.weather.active());
    const bool gi_wanted =
        scene.global_illumination.enabled && scene.global_illumination.probes.enabled && _gi_available;
    const bool needs_prepass = ao || contact || scene.lod.occlusion.enabled || soft || gi_wanted;
    if (taa) {
        taa = ensure_taa_targets(*view, scene.taa.sharpness > 0.0f) &&
              (needs_prepass || custom_shader || particles
                   ? ensure_ao_targets(*view, scene.ambient_occlusion.half_resolution, ao, contact, /*velocity=*/true)
                   : ensure_velocity_target(*view));
    } else if (ao || contact || soft || gi_wanted) {
        ensure_ao_targets(*view, scene.ambient_occlusion.half_resolution, ao, contact, /*velocity=*/false);
    }
    if (shadows) {
        ensure_shadow_map(lights.shadow_resolution, lights.shadow_cascades);
    }
    // Spot lights and local shadows (P3b): the light variants, and the maps the configured lights will want.
    const bool spot_shadows = std::any_of(lights.spots.begin(), lights.spots.end(),
                                          [](const SpotLightSource3D& l) { return l.shadow_enabled; });
    const bool point_shadows = std::any_of(lights.points.begin(), lights.points.end(),
                                           [](const PointLightSource3D& l) { return l.shadow_enabled; });
    const bool moon_shadow =
        lights.fill_shadow.enabled && lights.fill_shadow.strength > 0.0f && area_size.x > 0 && area_size.z > 0;
    const bool gi_lit = scene.global_illumination.enabled && scene.global_illumination.probes.enabled && _gi_available;
    const bool lights_on = _lights_available && (!lights.spots.empty() || spot_shadows || point_shadows ||
                                                 moon_shadow || lights.sun_shadow_strength < 1.0f || gi_lit);
    if (lights_on && (spot_shadows || point_shadows) && _shadow_shader && _shadow_vertex_shader) {
        u32 side = 0;
        for (const SpotLightSource3D& l : lights.spots) {
            side = l.shadow_enabled ? std::max(side, local_resolution(l, lights)) : side;
        }
        for (const PointLightSource3D& l : lights.points) {
            side = l.shadow_enabled ? std::max(side, local_resolution(l, lights)) : side;
        }
        const u32 spot_cap = std::min(lights.max_shadowed_spots, max_spot_shadow_maps);
        const u32 point_cap = std::min(lights.max_shadowed_points, max_point_shadow_maps);
        const auto count = [](const auto& list) {
            return static_cast<u32>(
                std::count_if(list.begin(), list.end(), [](const auto& l) { return l.shadow_enabled; }));
        };
        ensure_local_map(side, std::min(spot_cap, count(lights.spots)), std::min(point_cap, count(lights.points)));
    }
    if (lights_on && moon_shadow && _shadow_shader && _shadow_vertex_shader) {
        const u32 res = lights.fill_shadow.resolution != 0 ? lights.fill_shadow.resolution : lights.shadow_resolution;
        ensure_moon_map(res, lights.fill_shadow.cascades);
    }
    const bool main_velocity = taa && !needs_prepass && !custom_shader && !particles;
    // Submitted particles may all be culled. Prepare the stock-mesh MRT path too,
    // so visibility cannot introduce a pipeline compile after warm_up().
    const bool culled_particle_velocity = taa && !needs_prepass && !custom_shader && particles;
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
            if (s &&
                std::any_of(s->draws().begin(), s->draws().end(), [](const Scene3D::Draw& d) { return d.skinned; })) {
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
    const bool fog_on =
        fog.enabled && (fog.density > 0.0f || fog.height_density > 0.0f) && fog.max_opacity > 0.0f && _fog_available;
    if (scene.sky.enabled && _sky_available) {
        if ((view->_hdr || taa) && _output_dither_shader) {
            fullscreen(_output_dither_shader.handle(), view->_output_format);
        }
        sky_pipeline(format, samples, main_velocity);
        if (culled_particle_velocity)
            sky_pipeline(format, samples, true);
        if (taa && !main_velocity) {
            sky_velocity_pipeline(_ao_depth_format);
        }
    }
    if (scene.lights.sky_lights_scene && _sky_env_shader) {
        fullscreen(_sky_env_shader.handle(), SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
    }
    // Particles and weather: the blend modes and soft variants the scene's batches use.
    if (_particles_available) {
        const auto warm = [&](const ParticleLook3D& look) {
            if (look.casts_shadow && _capabilities.particle_shadows &&
                (shadows || spot_shadows || point_shadows || moon_shadow))
                particle_cast_pipeline();
            const bool can_light = look.lit > 0 && (!lights.points.empty() || !lights.spots.empty());
            for (const bool local : {false, true}) {
                if (local && !can_light)
                    continue;
                const bool soft_batch = soft && look.soft;
                particle_pipeline(format, samples, look.blend, soft_batch, main_velocity, false, local);
                if (taa)
                    particle_pipeline(hdr_format, 1, look.blend, soft_batch, false, true, local);
            }
            if (look.receives_shadow && look.lit > 0 && _capabilities.particle_shadows &&
                (shadows || spot_shadows || point_shadows || moon_shadow)) {
                particle_pipeline(format, samples, look.blend, soft && look.soft, main_velocity, false, false, true);
                if (taa)
                    particle_pipeline(hdr_format, 1, look.blend, soft && look.soft, false, true, false, true);
            }
        };
        for (const Scene3D::ParticleDraw& d : scene.particle_draws()) {
            warm(d.look);
        }
        for (const WeatherLayer3D* layer :
             {&scene.weather.rain, &scene.weather.snow, &scene.weather.dust, &scene.weather.mist}) {
            if (layer->intensity > 0.0f) {
                warm(layer->look);
                if (layer->splash > 0.0f && scene.weather.has_ground) {
                    warm(ParticleLook3D{}); // splashes are unlit, non-soft alpha rings
                }
            }
        }
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
            const auto selection = material_pipeline(m, draw.skinned, taa, main_velocity, fog_on);
            const auto* custom = selection.custom;
            const MeshPipelineRequest request{format, samples, selection, main_velocity, draw.skinned};
            const auto warm_mesh = [&](const MeshPipelineRequest& candidate) {
                pipeline(candidate);
                if (fog_on && !custom) {
                    auto fog = candidate;
                    fog.material.mask = false;
                    fog.material.unlit = Unlit::Off;
                    fog.fog = true;
                    pipeline(fog);
                }
                if (lights_on && !custom) {
                    auto lit = candidate;
                    lit.material.mask = false;
                    lit.material.unlit = Unlit::Off;
                    lit.lights = true;
                    pipeline(lit);
                    if (fog_on) {
                        lit.fog = true;
                        pipeline(lit);
                    }
                }
            };
            warm_mesh(request);
            if (culled_particle_velocity) {
                auto alternate = request;
                alternate.velocity = true;
                warm_mesh(alternate);
            }

            const bool opaque = pass_of(m) == Pass::Opaque;
            const bool cutout = m.blend == Blend3D::Mask;
            if (taa && (!opaque || custom)) {
                pipeline(hdr_format, 1, !m.double_sided, m.wireframe ? Pass::ReactiveWire : Pass::Reactive, false,
                         nullptr, false, draw.skinned);
            }

            // Warm the retained variants too. Capacity conflicts can still select the ordinary
            // variants at render time; preparing both avoids a first-visible-frame compile.
            if (_retained_enabled && draw.batch && !taa && !draw.skinned && !custom &&
                !scenes[0]->lod.occlusion.enabled && (m.blend == Blend3D::Opaque || m.blend == Blend3D::Mask)) {
                auto retained = request;
                retained.fog = fog_on;
                retained.lights = lights_on;
                retained.indexed = true;
                pipeline(retained);
                if (index == 0 && view->_pick)
                    pipeline(pick_format, 1, !m.double_sided, Pass::Pick, cutout, nullptr, false, false, false, false,
                             Unlit::Off, true);
                if (index == 0 && _ao_available && (ao || contact || soft))
                    pipeline(ao_normal_format, 1, !m.double_sided, Pass::Prepass, cutout, nullptr, false, false, false,
                             false, Unlit::Off, true);
            }
            if (index == 0 && (shadows || (lights_on && (spot_shadows || point_shadows || moon_shadow))) && opaque &&
                m.casts_shadow) {
                pipeline(SDL_GPU_TEXTUREFORMAT_INVALID, 1, !m.double_sided, Pass::Shadow, cutout, nullptr, false,
                         draw.skinned);
            }
            if (index == 0 && view->_pick && (opaque || m.blend == Blend3D::Alpha)) {
                pipeline(pick_format, 1, !m.double_sided, Pass::Pick, cutout, nullptr, false, draw.skinned);
            }
            if (draw.skinned && opaque && !custom && (ao || contact || taa || soft) && _ao_available) {
                // The pre-pass's skinned variants: with the velocity when temporal anti-aliasing has a pre-pass, plain
                // else.
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

Result3D<PickResult3D> GpuRenderer3D::pick(IViewBackend3D& base, Vec2i pixel) {
    auto* view = dynamic_cast<GpuView3D*>(&base);
    if (!view || !view->_pick || !view->_pick_valid)
        return {std::nullopt, {"GPU picking requires a rendered view with picking enabled"}};
    if (pixel.x < 0 || pixel.y < 0 || pixel.x >= view->_size.x || pixel.y >= view->_size.y)
        return result3d(PickResult3D{});
    // What is recorded so far goes to the GPU first (2D carries on in a new
    // command buffer, as after a read-back).
    if (_owner->_frame) {
        _owner->flush_to_frame();
        _owner->_frame->submit();
        _owner->end_frame();
    }
    std::vector<u8> texel;
    if (!_device->read_texture_region(view->_pick, static_cast<u32>(pixel.x), static_cast<u32>(pixel.y), 1, 1, 16,
                                      texel) ||
        texel.size() < 16) {
        return {std::nullopt, {"GPU picking readback failed"}};
    }
    f32 values[4];
    std::memcpy(values, texel.data(), sizeof(values));
    u32 id = 0;
    std::memcpy(&id, &values[3], sizeof(id));
    if (id == 0) {
        return result3d(PickResult3D{});
    }
    return result3d(PickResult3D{.hit = true, .id = id - 1, .position = {values[0], values[1], values[2]}});
}

} // namespace kin::gpu
