#include "webgl2_renderer3d.hpp"

#include "webgl2_renderer2d_backend.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <emscripten/html5.h>
#include <kin/platform/log.hpp>
#include <kin/render3d/local_lights3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/render3d/skeleton3d.hpp>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace kin {
namespace {
// Uniform values belong to the linked program. Other programs and texture
// uploads do not invalidate them; edits, shadow passes and skinning still do.
template <class T> struct CachedUniform {
    GLint location = -1;
    std::optional<T> value;
    CachedUniform(GLint at = -1) : location(at) {}
    void set(const T& next) {
        if (value && *value == next)
            return;
        if constexpr (std::is_same_v<T, GLint>)
            glUniform1i(location, next);
        else if constexpr (std::is_same_v<T, GLfloat>)
            glUniform1f(location, next);
        else if constexpr (std::is_same_v<T, Color>)
            glUniform4f(location, next.r / 255.0f, next.g / 255.0f, next.b / 255.0f, next.a / 255.0f);
        else if constexpr (std::is_same_v<T, LinearRgb>)
            glUniform3f(location, next.r, next.g, next.b);
        else if constexpr (std::is_same_v<T, std::array<GLfloat, 4>>)
            glUniform4fv(location, 1, next.data());
        else {
            static_assert(std::is_same_v<T, std::array<GLint, 5>>);
            glUniform1iv(location, GLsizei(next.size()), next.data());
        }
        value = next;
    }
};
struct Level {
    u32 first = 0, count = 0;
};
struct Mesh final : IMeshBackend3D {
    GLuint vao = 0, vertices = 0, indices = 0, skin = 0;
    u32 vertex_count = 0;
    bool dynamic = false; // made with morph targets: update_mesh_vertices rewrites `vertices`
    std::vector<Level> levels;
    std::shared_ptr<bool> alive;
    ~Mesh() override {
        if (alive && *alive) {
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &vertices);
            glDeleteBuffers(1, &indices);
            glDeleteBuffers(1, &skin);
        }
    }
};
struct Shadow {
    GLuint texture = 0, framebuffer = 0;
    u32 resolution = 0, count = 0;
    std::shared_ptr<bool> alive;
    ~Shadow() {
        if (alive && *alive) {
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(1, &texture);
        }
    }
};
// A float colour target (EXT_color_buffer_float) of an HDR view, with an optional depth buffer.
struct FloatTarget {
    GLuint texture = 0, framebuffer = 0, depth = 0;
    Vec2i size{};
    u32 levels = 1;
    std::shared_ptr<bool> alive;
    FloatTarget() = default;
    FloatTarget(const FloatTarget&) = delete;
    FloatTarget& operator=(const FloatTarget&) = delete;
    ~FloatTarget() {
        if (alive && *alive) {
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteRenderbuffers(1, &depth);
            glDeleteTextures(1, &texture);
        }
    }
};
std::shared_ptr<FloatTarget> float_target(std::shared_ptr<bool> alive, Vec2i size, GLenum format, bool depth,
                                          u32 levels = 1) {
    auto t = std::make_shared<FloatTarget>();
    t->alive = std::move(alive);
    t->size = size;
    t->levels = levels;
    glGenTextures(1, &t->texture);
    glBindTexture(GL_TEXTURE_2D, t->texture);
    glTexStorage2D(GL_TEXTURE_2D, GLsizei(levels), format, size.x, size.y);
    const bool filtered = format == GL_RGBA16F;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    !filtered    ? GL_NEAREST
                    : levels > 1 ? GL_LINEAR_MIPMAP_NEAREST
                                 : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtered ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->texture, 0);
    if (depth) {
        glGenRenderbuffers(1, &t->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size.x, size.y);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return {};
    return t;
}
// An HDR view's float picture and what its output needs (bloom, auto exposure), as native GpuView3D's.
struct HdrTargets {
    std::shared_ptr<FloatTarget> scene, bloom_a, bloom_b, meter;
    std::array<std::shared_ptr<FloatTarget>, 2> state; // auto exposure's, read one, write the other
    u32 state_read = 0, meter_level = 0;
    bool exposure_valid = false, exposure_time_seen = false;
    f32 exposure_time = 0;
    std::chrono::steady_clock::time_point exposure_wall{};
};
// Fog3D::volumetric's volume of a view: the lit cells and their sums along the rays (RGBA16F 3D textures).
struct FogVolume {
    GLuint lit = 0, sum = 0, framebuffer = 0;
    std::array<u32, 3> cells{};
    std::shared_ptr<bool> alive;
    FogVolume() = default;
    FogVolume(const FogVolume&) = delete;
    FogVolume& operator=(const FogVolume&) = delete;
    ~FogVolume() {
        if (alive && *alive) {
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(1, &lit);
            glDeleteTextures(1, &sum);
        }
    }
};
// What a view's scene is drawn into: its colour (the SDR texture or the HDR float picture), a depth texture and the
// surfaces' normals (both read by decals and soft particles), and a framebuffer of the colour alone, through which
// those draw while they read the depth.
struct SceneTargets {
    GLuint framebuffer = 0, plain = 0, depth = 0, normals = 0;
    std::shared_ptr<bool> alive;
    SceneTargets() = default;
    SceneTargets(const SceneTargets&) = delete;
    SceneTargets& operator=(const SceneTargets&) = delete;
    ~SceneTargets() {
        if (alive && *alive) {
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteFramebuffers(1, &plain);
            glDeleteTextures(1, &depth);
            glDeleteTextures(1, &normals);
        }
    }
};
std::shared_ptr<SceneTargets> scene_targets(std::shared_ptr<bool> alive, Vec2i size, GLuint colour) {
    auto t = std::make_shared<SceneTargets>();
    t->alive = std::move(alive);
    const auto texture = [&](GLuint& handle, GLenum format) {
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        glTexStorage2D(GL_TEXTURE_2D, 1, format, size.x, size.y);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    texture(t->depth, GL_DEPTH_COMPONENT24);
    texture(t->normals, GL_RGBA8);
    glGenFramebuffers(1, &t->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, t->normals, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t->depth, 0);
    const std::array<GLenum, 2> one{GL_COLOR_ATTACHMENT0, GL_NONE};
    glDrawBuffers(2, one.data());
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return {};
    glGenFramebuffers(1, &t->plain);
    glBindFramebuffer(GL_FRAMEBUFFER, t->plain);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return {};
    return t;
}
struct PreparedInstances;
struct View final : IViewBackend3D {
    Texture color;
    std::shared_ptr<WebGLTexture> surface;
    ViewOptions3D options;
    std::shared_ptr<Shadow> shadow;
    ShadowCascades3D cascades;
    sky_model::AmbientLight3D ambient, overlay_ambient;
    std::vector<std::shared_ptr<PreparedInstances>> prepared, overlay_prepared;
    std::shared_ptr<HdrTargets> hdr; // null: an SDR view, drawn straight into `color`
    std::shared_ptr<FogVolume> fog;  // Fog3D::volumetric's, while it is on
    std::shared_ptr<SceneTargets> targets;
    Texture texture() const override { return color; }
    Vec2i size() const override { return color.size(); }
    u32 samples() const override { return 1; }
};
struct Instance {
    Mat4f model;
    std::array<f32, 9> normal;
    std::array<f32, 4> tint;
    u32 joint_offset = 0;
};
struct Skin {
    std::array<u8, 4> joints, weights;
};
Instance instance(const MeshInstance3D& copy, u32 joint_offset = 0) {
    Instance value{.model = copy.transform};
    value.joint_offset = joint_offset;
    const Mat4f normal = copy.transform.inverse().transposed();
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            value.normal[column * 3 + row] = normal.at(row, column);
    value.tint = {copy.tint.r / 255.0f, copy.tint.g / 255.0f, copy.tint.b / 255.0f, copy.tint.a / 255.0f};
    return value;
}
// All views share these limits. Refused draws retain the streaming path.
struct InstanceBudget {
    static constexpr u64 cpu_limit = 4 * 1024 * 1024, gpu_limit = 16 * 1024 * 1024;
    static constexpr u32 buffer_limit = 512;
    u64 cpu_bytes = 0, gpu_bytes = 0;
    u32 buffers = 0;
};
struct ResidentInstances {
    std::shared_ptr<InstanceBudget> budget;
    std::shared_ptr<bool> alive;
    GLuint vao = 0, buffer = 0;
    u64 bytes = 0, revision = 0;
    std::vector<u32> order;
    ResidentInstances(std::shared_ptr<InstanceBudget> owner, std::shared_ptr<bool> liveness)
        : budget(std::move(owner)), alive(std::move(liveness)) {
        ++budget->buffers;
    }
    ~ResidentInstances() {
        if (*alive) {
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &buffer);
        }
        budget->gpu_bytes -= bytes;
        --budget->buffers;
    }
};
struct PreparedInstances {
    std::shared_ptr<InstanceBudget> budget;
    std::weak_ptr<IMeshBackend3D> mesh;
    std::vector<Instance> records;
    std::array<std::unique_ptr<ResidentInstances>, max_shadow_cascades + max_mesh_levels> passes;
    u64 batch_id = 0, batch_revision = 0, revision = 0;
    u32 first = 0;
    PreparedInstances(std::shared_ptr<InstanceBudget> owner, const std::shared_ptr<IMeshBackend3D>& source,
                      std::size_t count)
        : budget(std::move(owner)), mesh(source), records(count) {
        budget->cpu_bytes += records.size() * sizeof(Instance);
    }
    ~PreparedInstances() { budget->cpu_bytes -= records.size() * sizeof(Instance); }
};
void instance_layout(GLuint buffer) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    for (GLuint c = 0; c < 4; ++c) {
        glEnableVertexAttribArray(4 + c);
        glVertexAttribPointer(4 + c, 4, GL_FLOAT, GL_FALSE, sizeof(Instance),
                              reinterpret_cast<void*>(offsetof(Instance, model) + c * 16));
        glVertexAttribDivisor(4 + c, 1);
    }
    for (GLuint c = 0; c < 3; ++c) {
        glEnableVertexAttribArray(8 + c);
        glVertexAttribPointer(8 + c, 3, GL_FLOAT, GL_FALSE, sizeof(Instance),
                              reinterpret_cast<void*>(offsetof(Instance, normal) + c * 12));
        glVertexAttribDivisor(8 + c, 1);
    }
    glEnableVertexAttribArray(11);
    glVertexAttribPointer(11, 4, GL_FLOAT, GL_FALSE, sizeof(Instance),
                          reinterpret_cast<void*>(offsetof(Instance, tint)));
    glVertexAttribDivisor(11, 1);
    glEnableVertexAttribArray(14);
    glVertexAttribIPointer(14, 1, GL_UNSIGNED_INT, sizeof(Instance),
                           reinterpret_cast<void*>(offsetof(Instance, joint_offset)));
    glVertexAttribDivisor(14, 1);
}
void mesh_layout(const Mesh& mesh, GLuint buffer) {
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vertices);
    const std::array<GLint, 4> counts{3, 3, 2, 4};
    const std::array<std::size_t, 4> offsets{offsetof(MeshVertex3D, position), offsetof(MeshVertex3D, normal),
                                             offsetof(MeshVertex3D, uv), offsetof(MeshVertex3D, color)};
    for (GLuint i = 0; i < 4; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, counts[i], i == 3 ? GL_UNSIGNED_BYTE : GL_FLOAT, i == 3 ? GL_TRUE : GL_FALSE,
                              sizeof(MeshVertex3D), reinterpret_cast<void*>(offsets[i]));
    }
    if (mesh.skin) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh.skin);
        glEnableVertexAttribArray(12);
        glVertexAttribIPointer(12, 4, GL_UNSIGNED_BYTE, sizeof(Skin), reinterpret_cast<void*>(offsetof(Skin, joints)));
        glEnableVertexAttribArray(13);
        glVertexAttribPointer(13, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Skin),
                              reinterpret_cast<void*>(offsetof(Skin, weights)));
    }
    instance_layout(buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.indices);
}
bool visible(const Bounds3D& bounds, const Mat4f& matrix) {
    // Test the transformed box against Kin's six clip planes (reverse Z, 0..1).
    // Testing its corners also handles shear and negative/nonuniform scales.
    std::array<bool, 6> outside{true, true, true, true, true, true};
    for (u32 i = 0; i < 8; ++i) {
        const Vec4f p = matrix * Vec4f{i & 1 ? bounds.max.x : bounds.min.x, i & 2 ? bounds.max.y : bounds.min.y,
                                       i & 4 ? bounds.max.z : bounds.min.z, 1};
        const std::array<f32, 6> distances{p.w + p.x, p.w - p.x, p.w + p.y, p.w - p.y, p.z, p.w - p.z};
        for (u32 k = 0; k < 6; ++k)
            outside[k] = outside[k] && distances[k] < -1e-5f;
    }
    return std::none_of(outside.begin(), outside.end(), [](bool value) { return value; });
}
GLint wrap(Wrap3D mode) {
    return mode == Wrap3D::Repeat ? GL_REPEAT : mode == Wrap3D::Mirror ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}
// EXT_texture_filter_anisotropic's names (WebGL2's header has none).
constexpr GLenum texture_max_anisotropy = 0x84FE, max_texture_max_anisotropy = 0x84FF;
u32 level_for(const Scene3D& scene, const Scene3D::Draw& draw, const MeshInstance3D& copy, f32 height) {
    const u32 count = draw.mesh.level_count();
    if (scene.debug.force_level >= 0)
        return std::min(u32(scene.debug.force_level), count - 1);
    if (!scene.lod.enabled || count <= 1)
        return 0;
    const Mat4f& m = copy.transform;
    const f32 scale = stretch_bound(m), radius = draw.mesh.bounds().radius() * scale;
    const Vec3f center = m.apply(draw.mesh.bounds().center());
    const bool ortho = scene.camera.projection == Projection3D::Orthographic;
    const f32 distance = std::max(0.0f, dot(center - scene.camera.position, scene.camera.forward()) - radius);
    const f32 pixels = std::max(0.0f, scene.lod.error_pixels) / std::max(1e-3f, scene.lod.bias);
    const f32 per_unit =
        ortho ? height / std::max(1e-6f, scene.camera.height)
              : height * 0.5f / std::tan(math3d_detail::radians(scene.camera.fov) * 0.5f) /
                    (scene.camera.windowed() && scene.camera.window.h > 0.0f ? scene.camera.window.h : 1.0f);
    for (u32 l = count - 1; l > 0; --l)
        if (draw.mesh.level_error(l) * scale * per_unit <= pixels * (ortho ? 1.0f : distance))
            return l;
    return 0;
}
// The local lights the mesh shader takes (at most `limit`): those whose light reaches into the view, the most important
// (shadow_importance: brightness, reach and the share of the view they fill) when there are more, in scene order.
template <class Light, class Sphere>
std::vector<u32> chosen_lights(std::span<const Light> lights, u32 limit, const Mat4f& view_projection, Vec3f eye,
                               Sphere sphere) {
    std::vector<std::pair<f32, u32>> ranked;
    for (u32 i = 0; i < lights.size(); ++i) {
        Vec3f centre;
        f32 radius = 0;
        sphere(lights[i], centre, radius);
        if (!(radius > 0) ||
            !visible({centre - Vec3f{radius, radius, radius}, centre + Vec3f{radius, radius, radius}}, view_projection))
            continue;
        const auto& c = lights[i].color;
        ranked.push_back({-shadow_importance(std::max({c.r, c.g, c.b}), radius, centre, radius, eye), i});
    }
    if (ranked.size() > limit) {
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        ranked.resize(limit);
    }
    std::vector<u32> order;
    for (const auto& [importance, index] : ranked)
        order.push_back(index);
    std::sort(order.begin(), order.end());
    return order;
}
// What a scene asks for that this renderer leaves out, a bit each: each is told once (a log warning) per renderer.
enum Omitted : u32 {
    omit_shader = 1u << 0,
    omit_wireframe = 1u << 1,
    omit_particles = 1u << 2,
    omit_decals = 1u << 3,
    omit_ambient_occlusion = 1u << 4,
    omit_contact_shadows = 1u << 5,
    omit_ssr = 1u << 6,
    omit_taa = 1u << 7,
    omit_gi = 1u << 8,
    omit_probes = 1u << 9,
    omit_environment = 1u << 10,
    omit_local_shadows = 1u << 11,
    omit_clip = 1u << 12,
    omit_light_count = 1u << 13,
    omit_stars = 1u << 14,
    omit_volumetric = 1u << 15,
    omit_surface = 1u << 16,
    omit_weather = 1u << 17,
    omit_motion_blur = 1u << 18,
    omit_depth_of_field = 1u << 19,
    omit_lens_film_grade = 1u << 20,
};
constexpr std::array<const char*, 21> omitted_text{
    "custom material shaders (Material3D::shader) draw as kin's stock material",
    "wireframe materials draw filled",
    "particles cast and receive the sun's shadows only (no point, spot or moon shadows)",
    "(unused)",
    "screen-space ambient occlusion is off",
    "contact shadows are off",
    "screen-space reflections are off",
    "temporal anti-aliasing is off",
    "global illumination is off (the ambient light stands in)",
    "reflection probes are not used",
    "image environments (Lights3D::environment, SkyMode3D::Image) are not used",
    "point and spot lights cast no shadows",
    "Camera3D::clip_plane is not supported; the view draws everything",
    "only the 16 most important point lights and 16 spot lights in view are drawn",
    "the night sky has no stars",
    "volumetric fog (Fog3D::volumetric) needs EXT_color_buffer_float and a perspective camera: off",
    "surface shaders and material graphs (Material3D::surface) draw as kin's stock material",
    "(unused)",
    "motion blur (CameraEffects3D::motion_blur) is off",
    "depth of field (CameraEffects3D::depth_of_field) is off",
    "the camera's lens, film and grade (CameraEffects3D::lens, film, grade) are off",
};
// A draw's uniforms, cached per program (their values belong to the linked program).
struct DrawUniforms {
    CachedUniform<GLint> shadow_pass, receives_shadow;
    CachedUniform<std::array<GLint, 5>> flip;
    CachedUniform<GLint> skinned, has_normal, has_emission;
    CachedUniform<GLfloat> metallic, roughness, normal_scale, occlusion_strength;
    CachedUniform<Color> color;
    CachedUniform<GLint> unlit;
    CachedUniform<GLfloat> emissive;
    CachedUniform<GLint> blend;
    CachedUniform<GLfloat> alpha_cutoff;
    CachedUniform<LinearRgb> emission_color;
    CachedUniform<GLint> independent_emission;
    CachedUniform<std::array<GLfloat, 4>> uv_transform;
};
// A linked mesh program: kin's stock one, or a surface shader's (mesh.frag.glsl with KIN_SURFACE), with its uniform
// locations and cached values, and the pass whose scene uniforms it holds.
struct MeshProgram {
    GLuint handle = 0;
    bool surface = false;
    std::shared_ptr<bool> alive;
    std::unordered_map<std::string_view, GLint> locations; // by name (string literals)
    DrawUniforms draw{};
    u64 serial = 0;
    MeshProgram(GLuint program, bool is_surface, std::shared_ptr<bool> liveness)
        : handle(program), surface(is_surface), alive(std::move(liveness)) {
        glUseProgram(handle);
        // Sampler units belong to the linked program, not individual draws.
        constexpr std::array names{"uTexture",     "uNormal",     "uMetalRough", "uOcclusion",
                                   "uEmission",    "uJoints",     "uFogTable",   "uShadow",
                                   "uGraphMaps",   "uFogVolume",  "uDecalDepth", "uDecalNormals",
                                   "uLocalShadow", "uShadowData", "uMoonShadow", "uLightData"};
        for (u32 unit = 0; unit < names.size(); ++unit)
            glUniform1i(location(names[unit]), GLint(unit));
        draw = {location("uShadowPass"),
                location("uReceivesShadow"),
                location("uFlip[0]"),
                location("uSkinned"),
                location("uHasNormal"),
                location("uHasEmission"),
                location("uMetallic"),
                location("uRoughness"),
                location("uNormalScale"),
                location("uOcclusionStrength"),
                location("uColor"),
                location("uUnlit"),
                location("uEmissive"),
                location("uBlend"),
                location("uAlphaCutoff"),
                location("uEmissionColor"),
                location("uIndependentEmission"),
                location("uUvTransform")};
    }
    MeshProgram(const MeshProgram&) = delete;
    MeshProgram& operator=(const MeshProgram&) = delete;
    ~MeshProgram() {
        if (alive && *alive)
            glDeleteProgram(handle);
    }
    GLint location(const char* name) {
        const auto found = locations.find(name);
        if (found != locations.end())
            return found->second;
        return locations.emplace(name, glGetUniformLocation(handle, name)).first->second;
    }
};
constexpr GLuint graph_maps_unit = 8, fog_volume_unit = 9, decal_depth_unit = 10, decal_normals_unit = 11,
                 local_shadow_unit = 12, shadow_data_unit = 13, moon_shadow_unit = 14, light_data_unit = 15;
// lights.inc.glsl's rows: 16 point lights, then 16 spots, four texels each.
constexpr u32 light_rows = 16 + max_spot_lights;
using LightRows = std::array<f32, light_rows * 16>;
// A particle sprite as the vertex shader reads it (native GpuParticle3D's layout).
struct ParticleRecord {
    std::array<f32, 4> position_size, colour, axis_rotation, frame;
};
static_assert(sizeof(ParticleRecord) == 64);
// A decal as decal.vert reads it (instanced attributes 0 to 10).
struct DecalRecord {
    Mat4f model, to_decal;
    std::array<f32, 4> colour, surface, fade;
};
static_assert(sizeof(DecalRecord) == 44 * sizeof(f32));
// A source of sprites: a Scene3D::draw_particles batch or a weather layer.
struct ParticleSource {
    const ParticleLook3D* look = nullptr;
    std::span<const ParticleSprite3D> sprites;
};
// A surface shader made by this renderer (create_surface_source): its program.
struct SurfaceProgram final : ISurfaceShaderBackend3D {
    std::shared_ptr<MeshProgram> program;
    bool valid() const override { return program && program->alive && *program->alive; }
};
// GLSL ES 3.00 takes no `const` on a value that isn't a constant expression (native's GLSL 4.5 does): dropped from a
// surface's source and the graph functions, which say it of locals.
std::string without_const(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const bool word_start = i == 0 || !(std::isalnum(u8(text[i - 1])) || text[i - 1] == '_');
        if (word_start && text.compare(i, 6, "const ") == 0) {
            i += 6;
            continue;
        }
        out += text[i++];
    }
    return out;
}
} // namespace

struct WebGLRenderer3D::Impl {
    struct MaterialBinding {
        GLuint texture = 0, sampler = 0;
    };
    WebGL2Renderer2DBackend& renderer;
    WebGLDevice& device;
    std::shared_ptr<MeshProgram> stock;                      // kin's own mesh program
    MeshProgram* current = nullptr;                          // the mesh program in use (null: another, or none)
    std::string mesh_vertex, mesh_fragment, graph_functions; // the sources surface programs are made from
    GLuint instances = 0, joints = 0, fog_texture = 0, sky_program = 0, sky_vao = 0, shadow_dummy = 0, post_program = 0,
           graph_maps_dummy = 0, fog_program = 0, fog_volume_dummy = 0, particle_program = 0, particle_vao = 0,
           particle_buffer = 0;
    std::shared_ptr<MeshProgram> decal; // mesh.frag with KIN_DECAL, decal.vert's boxes
    u64 particle_serial = 0;            // the pass whose scene uniforms the particle program holds
    std::vector<ParticleSprite3D> weather_sprites_now;
    std::vector<WeatherBatch3D> weather_batches_now;
    std::vector<ParticleRecord> particle_records, cast_records;
    std::vector<DecalRecord> decal_records;
    GLuint decal_buffer = 0, decal_vao = 0;
    // Local and moon shadows (local_maps): the arrays, the rows of uShadowData, each light's first layer (-1: none).
    struct DepthArrayHandles {
        GLuint texture = 0, framebuffer = 0;
        u32 side = 0, layers = 0;
    };
    ShadowSlots3D spot_slots, point_slots;
    std::vector<f32> point_layer_of, spot_layer_of;
    GLuint shadow_data = 0, light_data = 0, fog_light_data = 0;
    bool has_local = false;
    f32 local_texel = 1.0f;
    struct {
        u32 count = 0, row = 0;
        std::array<f32, 4> ends{}, blend{};
        f32 texel = 1.0f, strength = 0.0f;
    } moon;
    struct ParticleCast {
        const ParticleLook3D* look;
        u32 first, count;
    };
    std::vector<ParticleCast> particle_casts; // this render's casting sprites, in `cast_buffer`
    GLuint cast_buffer = 0;
    // The fog volume this pass's mesh and sky programs read (environment): its depth (0: none), whether it holds the
    // sun's in-scattering, the target's size.
    struct {
        f32 depth = 0;
        bool sun = false;
        Vec2i size{1, 1};
    } fog_state;
    bool float_targets = false; // EXT_color_buffer_float: HDR views can be drawn
    bool linear_output = false; // this render draws an HDR view's float picture
    Texture white;
    RendererCapabilities3D caps{};
    u32 omitted_told = 0; // the Omitted bits already warned about
    bool hdr_refused_told = false, pick_told = false;
    std::unordered_map<std::string_view, GLint> sky_uniforms, post_uniforms, fog_uniforms, particle_uniforms;
    // What a pass sets on every mesh program it draws with (apply), once per program: `serial` names the pass.
    struct Pass {
        u64 serial = 0;
        const Scene3D* scene = nullptr;
        const Lights3D* lights = nullptr;
        const View* view = nullptr; // its sun shadows (null: none, as the overlay's)
        bool shadow = false;        // a shadow cascade's: each layer sets its own uViewProjection
        Vec2i size{};
        Mat4f view_projection{};
        std::array<f32, 128> points{};
        std::array<f32, 16 * max_spot_lights> spots{};
        std::array<f32, 16> point_shadow{}; // each packed point light's first cube layer (-1: none)
        u32 point_count = 0, spot_count = 0;
        LinearRgb ambient{}, ground{};
    } pass;
    u64 serials = 0;
    sky_model::FogTable3D fog_table;
    // By wrap and filter, then the mipmapped ones' by anisotropy (2 to 16) and wrap; the mipmapped ones read with
    // `anisotropy`, this render's Scene3D::anisotropy, clamped.
    std::unordered_map<u32, GLuint> samplers;
    u32 anisotropy = 1;
    std::array<MaterialBinding, 5> material_bindings{};
    std::shared_ptr<InstanceBudget> instance_budget = std::make_shared<InstanceBudget>();
    std::vector<Instance> instance_scratch;

    void prepare(std::vector<std::shared_ptr<PreparedInstances>>& cache, const Scene3D& scene,
                 RenderDebugStats3D& stats) {
        // Slot reuse never assumes command identity: mesh, source range and all
        // rendered record values are checked, or a batch's explicit revision.
        cache.resize(std::min<std::size_t>(scene.draws().size(), InstanceBudget::buffer_limit));
        for (u32 slot = 0; slot < cache.size(); ++slot) {
            const auto& draw = scene.draws()[slot];
            const auto source =
                (draw.batch ? draw.batch.instances() : scene.instances()).subspan(draw.first, draw.count);
            auto& entry = cache[slot];
            if (entry && (entry->mesh.lock() != draw.mesh.backend() || entry->records.size() != source.size()))
                entry.reset();
            if (!entry) {
                const u64 bytes = source.size() * sizeof(Instance);
                if (instance_budget->cpu_bytes + bytes > InstanceBudget::cpu_limit) {
                    ++stats.retained_refused;
                    continue;
                }
                entry = std::make_shared<PreparedInstances>(instance_budget, draw.mesh.backend(), source.size());
            }
            auto& p = *entry;
            ++stats.prepared_draws;
            if (draw.batch && !draw.skinned && p.revision && p.batch_id == draw.batch.identity() &&
                p.batch_revision == draw.batch.revision() && p.first == draw.first) {
                ++stats.prepared_reused;
                continue;
            }
            bool dirty = p.revision == 0;
            for (u32 i = 0; i < source.size(); ++i) {
                const auto& copy = source[i];
                const u32 offset = draw.skinned ? scene.skin_offsets()[draw.skin_first + i] : 0;
                auto& record = p.records[i];
                const std::array<f32, 4> tint{copy.tint.r / 255.0f, copy.tint.g / 255.0f, copy.tint.b / 255.0f,
                                              copy.tint.a / 255.0f};
                if (!p.revision || std::memcmp(record.model.m, copy.transform.m, sizeof(record.model.m)) != 0 ||
                    record.tint != tint || record.joint_offset != offset) {
                    record = instance(copy, offset);
                    dirty = true;
                }
            }
            if (dirty)
                ++p.revision;
            else
                ++stats.prepared_reused;
            p.batch_id = draw.batch ? draw.batch.identity() : 0;
            p.batch_revision = draw.batch ? draw.batch.revision() : 0;
            p.first = draw.first;
        }
        stats.prepared_cache_bytes = instance_budget->cpu_bytes;
    }

    explicit Impl(WebGL2Renderer2DBackend& renderer) : renderer(renderer), device(renderer.device()) {
        GLint attributes = 0;
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
        if (attributes < 12)
            throw std::runtime_error("WebGL2 mesh renderer requires 12 vertex attributes");
        mesh_vertex = WebGLDevice::shader_source("mesh.vert.glsl");
        mesh_fragment = WebGLDevice::shader_source("mesh.frag.glsl");
        graph_functions = without_const(WebGLDevice::shader_source("kin_graph.inc.glsl"));
        stock = std::make_shared<MeshProgram>(WebGLDevice::program(mesh_vertex.c_str(), mesh_fragment.c_str()), false,
                                              device.alive());
        const auto sky_vertex = WebGLDevice::shader_source("sky.vert.glsl");
        const auto sky_fragment = WebGLDevice::shader_source("sky.frag.glsl");
        sky_program = WebGLDevice::program(sky_vertex.c_str(), sky_fragment.c_str());
        post_program = WebGLDevice::program(sky_vertex.c_str(), WebGLDevice::shader_source("post.frag.glsl").c_str());
        glUseProgram(post_program);
        glUniform1i(glGetUniformLocation(post_program, "uSource"), 0);
        glUniform1i(glGetUniformLocation(post_program, "uBloom"), 1);
        glUniform1i(glGetUniformLocation(post_program, "uState"), 2);
        float_targets =
            emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(), "EXT_color_buffer_float");
        {
            std::string decal_fragment = mesh_fragment;
            decal_fragment.insert(decal_fragment.find('\n') + 1, "#define KIN_DECAL\n");
            decal = std::make_shared<MeshProgram>(
                WebGLDevice::program(WebGLDevice::shader_source("decal.vert.glsl").c_str(), decal_fragment.c_str()),
                false, device.alive());
            glUniform1i(decal->location("uReceivesShadow"), 1); // (a decal is lit as the surface under it)
        }
        particle_program = WebGLDevice::program(WebGLDevice::shader_source("particle.vert.glsl").c_str(),
                                                WebGLDevice::shader_source("particle.frag.glsl").c_str());
        glUseProgram(particle_program);
        glUniform1i(glGetUniformLocation(particle_program, "uTexture"), 0);
        glUniform1i(glGetUniformLocation(particle_program, "uDepth"), 1);
        glUniform1i(glGetUniformLocation(particle_program, "uShadow"), 7);
        glUniform1i(glGetUniformLocation(particle_program, "uLocalShadow"), GLint(local_shadow_unit));
        glUniform1i(glGetUniformLocation(particle_program, "uShadowData"), GLint(shadow_data_unit));
        glUniform1i(glGetUniformLocation(particle_program, "uMoonShadow"), GLint(moon_shadow_unit));
        glGenTextures(1, &shadow_data);
        glActiveTexture(GL_TEXTURE0 + shadow_data_unit);
        glBindTexture(GL_TEXTURE_2D, shadow_data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        const std::array<f32, 24> no_rows{};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 6, 1, 0, GL_RGBA, GL_FLOAT, no_rows.data());
        for (GLuint* t : {&light_data, &fog_light_data}) {
            glGenTextures(1, t);
            glActiveTexture(GL_TEXTURE0 + light_data_unit);
            glBindTexture(GL_TEXTURE_2D, *t);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 4, GLsizei(light_rows));
        }
        glUniform1i(glGetUniformLocation(particle_program, "uLightData"), GLint(light_data_unit));
        glUniform1i(glGetUniformLocation(particle_program, "uFogTable"), 6);
        glUniform1i(glGetUniformLocation(particle_program, "uFogVolume"), GLint(fog_volume_unit));
        glGenBuffers(1, &particle_buffer);
        glGenBuffers(1, &cast_buffer);
        glGenBuffers(1, &decal_buffer);
        glGenVertexArrays(1, &decal_vao);
        glGenVertexArrays(1, &particle_vao);
        fog_program =
            WebGLDevice::program(sky_vertex.c_str(), WebGLDevice::shader_source("fog_volume.frag.glsl").c_str());
        glUseProgram(fog_program);
        glUniform1i(glGetUniformLocation(fog_program, "uLit"), 0);
        glUniform1i(glGetUniformLocation(fog_program, "uShadow"), 7);
        glUniform1i(glGetUniformLocation(fog_program, "uLocalShadow"), GLint(local_shadow_unit));
        glUniform1i(glGetUniformLocation(fog_program, "uShadowData"), GLint(shadow_data_unit));
        glUniform1i(glGetUniformLocation(fog_program, "uMoonShadow"), GLint(moon_shadow_unit));
        glUniform1i(glGetUniformLocation(fog_program, "uLightData"), GLint(light_data_unit));
        glUseProgram(sky_program);
        glUniform1i(glGetUniformLocation(sky_program, "uFogTable"), 6);
        glUniform1i(glGetUniformLocation(sky_program, "uFogVolume"), GLint(fog_volume_unit));
        glGenTextures(1, &fog_volume_dummy);
        glActiveTexture(GL_TEXTURE0 + fog_volume_unit);
        glBindTexture(GL_TEXTURE_3D, fog_volume_dummy);
        const std::array<u8, 4> clear{0, 0, 0, 255};
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, clear.data());
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenVertexArrays(1, &sky_vao);
        glGenTextures(1, &shadow_dummy);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_dummy);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, 1, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT,
                     nullptr);
        glGenTextures(1, &fog_texture);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, fog_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, sky_model::FogTable3D::rows, 0, GL_RGBA, GL_FLOAT,
                     fog_table.values.data());
        glGenTextures(1, &joints);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, joints);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const Mat4f identity;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, 1, 0, GL_RGBA, GL_FLOAT, identity.m);
        caps.skinning = attributes >= 15;
        glGenBuffers(1, &instances);
        const std::array<u8, 4> pixels{255, 255, 255, 255};
        white = device.create_texture(pixels.data(), {1, 1});
        glGenTextures(1, &graph_maps_dummy);
        glActiveTexture(GL_TEXTURE0 + graph_maps_unit);
        glBindTexture(GL_TEXTURE_2D_ARRAY, graph_maps_dummy);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        caps.depth_bits = 24;
        caps.morph_targets = true;
        caps.auto_exposure = float_targets;
        caps.particles = caps.soft_particles = caps.decals = caps.particle_shadows = caps.local_shadows = true;
        caps.max_spot_lights = max_spot_lights; // the most important 16 of each in view, unshadowed
        const bool unmasked =
            emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(), "WEBGL_debug_renderer_info");
        const auto* driver = reinterpret_cast<const char*>(glGetString(unmasked ? 0x9246 : GL_RENDERER));
        const std::string name = driver ? driver : "";
        caps.software = name.find("SwiftShader") != std::string::npos || name.find("llvmpipe") != std::string::npos;
        if (emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(),
                                              "EXT_texture_filter_anisotropic")) {
            GLfloat most = 1.0f;
            glGetFloatv(max_texture_max_anisotropy, &most);
            caps.max_anisotropy = most >= 16.0f ? 16u : most >= 1.0f ? u32(most) : 1u;
        }
    }
    ~Impl() {
        for (const auto& [key, sampler] : samplers)
            glDeleteSamplers(1, &sampler);
        glDeleteBuffers(1, &instances);
        glDeleteTextures(1, &joints);
        glDeleteTextures(1, &fog_texture);
        glDeleteProgram(sky_program);
        glDeleteProgram(post_program);
        glDeleteProgram(fog_program);
        glDeleteProgram(particle_program);
        for (const auto* a : {&local, &moon_maps}) {
            glDeleteTextures(1, &a->texture);
            glDeleteFramebuffers(1, &a->framebuffer);
        }
        glDeleteTextures(1, &shadow_data);
        glDeleteTextures(1, &light_data);
        glDeleteTextures(1, &fog_light_data);
        glDeleteBuffers(1, &particle_buffer);
        glDeleteBuffers(1, &cast_buffer);
        glDeleteBuffers(1, &decal_buffer);
        glDeleteVertexArrays(1, &decal_vao);
        glDeleteVertexArrays(1, &particle_vao);
        glDeleteTextures(1, &fog_volume_dummy);
        glDeleteVertexArrays(1, &sky_vao);
        glDeleteTextures(1, &shadow_dummy);
        glDeleteTextures(1, &graph_maps_dummy);
    }
    GLint location(GLuint handle, const char* name) {
        if (current && handle == current->handle)
            return current->location(name);
        auto& cache = handle == sky_program        ? sky_uniforms
                      : handle == fog_program      ? fog_uniforms
                      : handle == particle_program ? particle_uniforms
                                                   : post_uniforms;
        const auto found = cache.find(name);
        if (found != cache.end())
            return found->second;
        return cache.emplace(name, glGetUniformLocation(handle, name)).first->second;
    }
    GLint uniform(const char* name) { return current->location(name); }
    // A surface shader's program: mesh.frag with KIN_SURFACE, the surface's source in it (its graph functions
    // included). Throws with the compiler's log when it doesn't compile.
    std::shared_ptr<MeshProgram> surface_program(std::string_view surface) {
        std::string source = without_const(std::string(surface));
        const std::string include = "#include \"kin_graph.inc.glsl\"";
        for (std::size_t at; (at = source.find(include)) != std::string::npos;)
            source.replace(at, include.size(), graph_functions);
        // Native's screen position runs down; GL's up (kin_screen turns it).
        const std::string screen = "(gl_FragCoord.xy * frame.occlusion.zw)";
        for (std::size_t at; (at = source.find(screen)) != std::string::npos;)
            source.replace(at, screen.size(), "kin_screen()");
        std::string fragment = mesh_fragment;
        const std::string marker = "// KIN_SURFACE_SOURCE";
        fragment.replace(fragment.find(marker), marker.size(), source);
        fragment.insert(fragment.find('\n') + 1, "#define KIN_SURFACE\n");
        current = nullptr;
        return std::make_shared<MeshProgram>(WebGLDevice::program(mesh_vertex.c_str(), fragment.c_str()), true,
                                             device.alive());
    }
    // The program a draw's material takes: its surface's where this renderer made it, else the stock one.
    MeshProgram& program_for(const Material3D& material) {
        if (material.surface)
            if (const auto* own = dynamic_cast<const SurfaceProgram*>(material.surface.backend().get());
                own && own->program && own->program->alive == device.alive())
                return *own->program;
        return *stock;
    }
    GLuint sampler(Wrap3D u, Wrap3D v, ScaleMode mode, std::optional<TextureSampler3D> explicit_sampler = {}) {
        const auto s = explicit_sampler.value_or(legacy_sampler3d(mode, u, v));
        const u32 slant =
            s.mip != MipFilter3D::None && s.min == TextureFilter3D::Linear && s.mag == TextureFilter3D::Linear
                ? anisotropy
                : 1;
        const u32 key =
            u32(s.wrap_u) | u32(s.wrap_v) << 2 | u32(s.min) << 4 | u32(s.mag) << 5 | u32(s.mip) << 6 | slant << 8;
        auto& value = samplers[key];
        if (!value) {
            glGenSamplers(1, &value);
            glSamplerParameteri(value, GL_TEXTURE_WRAP_S, wrap(s.wrap_u));
            glSamplerParameteri(value, GL_TEXTURE_WRAP_T, wrap(s.wrap_v));
            const bool nearest = s.min == TextureFilter3D::Nearest;
            const GLint min = s.mip == MipFilter3D::None ? (nearest ? GL_NEAREST : GL_LINEAR)
                              : s.mip == MipFilter3D::Nearest
                                  ? (nearest ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST)
                                  : (nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
            glSamplerParameteri(value, GL_TEXTURE_MIN_FILTER, min);
            glSamplerParameteri(value, GL_TEXTURE_MAG_FILTER,
                                s.mag == TextureFilter3D::Nearest ? GL_NEAREST : GL_LINEAR);
            if (slant > 1)
                glSamplerParameterf(value, texture_max_anisotropy, GLfloat(slant));
        }
        return value;
    }
    void environment(GLuint handle, const Scene3D& scene) {
        glUseProgram(handle);
        if (current && current->handle != handle)
            current = nullptr;
        const auto loc = [&](const char* name) { return location(handle, name); };
        const auto set = [&](const char* name, Vec4f value) {
            glUniform4f(loc(name), value.x, value.y, value.z, value.w);
        };
        const auto vector = [&](const char* name, Vec3f value) { glUniform3f(loc(name), value.x, value.y, value.z); };
        const auto& sky = scene.sky;
        const auto& fog = scene.fog;
        const Vec3f sun =
            length_squared(scene.lights.sun.direction) > 0 ? normalize(scene.lights.sun.direction) : Vec3f{0, 1, 0};
        const Vec3f moon = length_squared(sky.moon_direction) > 0 ? normalize(sky.moon_direction) : Vec3f{0, 1, 0};
        const std::array<Vec4f, 5> atmosphere{
            {{sun.x, sun.y, sun.z, sky.sun_intensity},
             {sky_model::mie_per_turbidity * std::clamp(sky.turbidity, 1.0f, 20.0f), sky_model::mie_anisotropy,
              sky.intensity, sky.moon ? std::max(0.0f, sky.moon_light) : 0.0f},
             {sky.ground_color.r, sky.ground_color.g, sky.ground_color.b, 0},
             {moon.x, moon.y, moon.z, 0},
             {sky.night_color.r, sky.night_color.g, sky.night_color.b, 0}}};
        constexpr std::array names{"uSky.sun", "uSky.shape", "uSky.ground", "uSky.moon", "uSky.night"};
        for (u32 i = 0; i < names.size(); ++i)
            set(names[i], atmosphere[i]);
        set("uFog.colour", {fog.colour.r, fog.colour.g, fog.colour.b,
                            fog.colour_from_sky && sky.mode == SkyMode3D::Procedural ? 1.0f : 0.0f});
        set("uFog.density",
            {std::max(0.0f, fog.density), std::max(0.0f, fog.height_density), fog.height_falloff, fog.base_height});
        set("uFog.limits", {std::max(0.0f, fog.start), std::clamp(fog.max_opacity, 0.0f, 1.0f),
                            std::max(0.0f, fog.sun_scatter), std::clamp(fog.sun_anisotropy, -0.95f, 0.95f)});
        set("uFog.view", {scene.camera.projection == Projection3D::Orthographic ? 1.0f : 0.0f, 0, 0, 0});
        glUniform1i(loc("uFogEnabled"), fog.enabled);
        glUniform1i(loc("uLinear"), linear_output);
        glUniform4f(loc("uFogVolumeAt"), fog_state.depth, fog_state.sun ? 1.0f : 0.0f, 1.0f / f32(fog_state.size.x),
                    1.0f / f32(fog_state.size.y));
        vector("uEye", scene.camera.position);
        vector("uForward", scene.camera.forward());
        glActiveTexture(GL_TEXTURE6);
        glBindSampler(6, 0);
        glBindTexture(GL_TEXTURE_2D, fog_texture);
        if (fog.enabled && fog.colour_from_sky && fog_table.update(sky, sun))
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, sky_model::FogTable3D::rows, GL_RGBA, GL_FLOAT,
                            fog_table.values.data());
        glUniform1i(loc("uFogTable"), 6);
    }
    void sky(const Scene3D& scene, Vec2i size, RenderDebugStats3D& stats) {
        // Without a sky, the fog volume's light still shows where nothing is drawn (added over the background).
        const bool volume_only = !scene.sky.enabled && fog_state.depth > 0;
        if (!scene.sky.enabled && !volume_only)
            return;
        environment(sky_program, scene);
        glUniform1i(location(sky_program, "uVolumeOnly"), volume_only);
        const auto loc = [&](const char* name) { return location(sky_program, name); };
        const Mat4f inverse = scene.camera.view_projection(f32(size.x) / size.y).inverse();
        glUniformMatrix4fv(loc("uInverseViewProjection"), 1, GL_FALSE, inverse.m);
        glUniform2f(loc("uTarget"), f32(size.x), f32(size.y));
        const auto& sky = scene.sky;
        glUniform4f(loc("uDisc"), math3d_detail::radians(std::max(0.001f, sky.sun_size) * 0.5f),
                    sky.moon ? math3d_detail::radians(sky.moon_size * 0.5f) : 0.0f, sky.sun_disc, 0);
        glUniform3f(loc("uMoonColor"), sky.moon_color.r * sky.moon_light, sky.moon_color.g * sky.moon_light,
                    sky.moon_color.b * sky.moon_light);
        glUniform3f(loc("uSunColor"), scene.lights.sun.color.r, scene.lights.sun.color.g, scene.lights.sun.color.b);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        if (volume_only) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE, GL_ONE);
        } else
            glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(sky_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        stats.sky_drawn = stats.sky_drawn || !volume_only;
    }
    void rgb(const char* name, LinearRgb value) { glUniform3f(uniform(name), value.r, value.g, value.b); }
    void direction(const char* name, Vec3f value) {
        value = normalize(value);
        glUniform3f(uniform(name), value.x, value.y, value.z);
    }
    bool supported(const Scene3D& scene, const WebGLTexture* target) {
        if (scene.joint_matrices().size() > std::size_t(device.max_texture_size())) {
            KIN_LOG_WARN("render",
                         "WebGL2: scene joint palette exceeds MAX_TEXTURE_SIZE rows; split the scene into views");
            return false;
        }
        for (const auto& matrix : scene.joint_matrices())
            for (const f32 value : matrix.m)
                if (!std::isfinite(value)) {
                    KIN_LOG_WARN("render", "WebGL2: a joint palette contains a non-finite matrix");
                    return false;
                }
        for (const auto& draw : scene.draws()) {
            const auto& material = draw.material;
            const auto* mesh = dynamic_cast<const Mesh*>(draw.mesh.backend().get());
            if (!mesh || mesh->alive != device.alive() || (draw.skinned && !caps.skinning)) {
                KIN_LOG_WARN("render", "WebGL2: a mesh of another renderer, or skinned beyond the device's attributes");
                return false;
            }
            for (const Texture* image : {&material.texture, &material.normal, &material.metallic_roughness,
                                         &material.occlusion, &material.emissive_texture}) {
                if (!*image)
                    continue;
                const auto* texture = device.texture(*image);
                if (!texture || texture == target) {
                    KIN_LOG_WARN("render",
                                 "WebGL2: material texture belongs to another renderer or samples its own view");
                    return false;
                }
            }
        }
        if (const auto* overlay = scene.overlay_if_any(); overlay && !supported(*overlay, target))
            return false;
        return true;
    }
    // Makes `program` the one in use, with this pass's scene uniforms.
    void use(MeshProgram& program) {
        if (current != &program) {
            glUseProgram(program.handle);
            current = &program;
        }
        if (program.serial != pass.serial)
            apply(program);
    }
    // Packed lights (points: 8 floats each, spots: 16, their shadow layers) into lights.inc.glsl's rows of `texture`,
    // bound to its unit.
    void upload_lights(GLuint texture, const std::array<f32, 128>& points,
                       const std::array<f32, 16 * max_spot_lights>& spots, const std::array<f32, 16>& point_shadow) {
        LightRows rows{};
        for (u32 i = 0; i < 16; ++i) {
            std::copy_n(points.data() + 8 * i, 8, rows.data() + 16 * i);
            rows[16 * i + 8] = point_shadow[i];
        }
        std::copy(spots.begin(), spots.end(), rows.data() + 16 * 16);
        glActiveTexture(GL_TEXTURE0 + light_data_unit);
        glBindSampler(light_data_unit, 0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, GLsizei(light_rows), GL_RGBA, GL_FLOAT, rows.data());
    }
    // A light's first shadow layer this render (-1: none; none for the overlay, `shadowed` null).
    static f32 layer_of(const std::vector<f32>& layers, u32 index, const void* shadowed) {
        return shadowed && index < layers.size() ? layers[index] : -1.0f;
    }
    // The pass's scene uniforms on the program in use (`current`).
    void apply(MeshProgram& program) {
        const Scene3D& scene = *pass.scene;
        environment(program.handle, scene);
        program.serial = pass.serial;
        if (pass.shadow)
            return; // (a cascade's matrix is set by each layer)
        glUniform3f(uniform("uEye"), scene.camera.position.x, scene.camera.position.y, scene.camera.position.z);
        glUniformMatrix4fv(uniform("uViewProjection"), 1, GL_FALSE, pass.view_projection.m);
        glUniform1i(uniform("uPointCount"), GLint(pass.point_count));
        glUniform1i(uniform("uSpotCount"), GLint(pass.spot_count));
        const Lights3D& lights = *pass.lights;
        direction("uSunDirection", lights.sun.direction);
        direction("uFillDirection", lights.fill.direction);
        rgb("uSunColor", lights.sun.color);
        rgb("uFillColor", lights.fill.color);
        rgb("uAmbient", pass.ambient);
        rgb("uGround", pass.ground);
        glUniform1f(uniform("uSpecularAa"), std::clamp(scene.specular_aa, 0.0f, 1.0f));
        glUniform1f(uniform("uSingleScatter"), scene.specular_multiscatter ? 0.0f : 1.0f);
        shadow_uniforms(pass.view, lights, program.handle);
        if (program.surface) { // native's frame block, what a surface reads of it
            const auto set = [&](const char* name, f32 x, f32 y, f32 z, f32 w) {
                glUniform4f(uniform(name), x, y, z, w);
            };
            const Vec3f eye = scene.camera.position, forward = scene.camera.forward();
            set("frame.time", scene.time, scene.specular_multiscatter ? 0.0f : 1.0f, 0, 0);
            set("frame.eye", eye.x, eye.y, eye.z, 1);
            set("frame.occlusion", 0, 0, 1.0f / f32(pass.size.x), 1.0f / f32(pass.size.y));
            set("frame.cascade_forward", forward.x, forward.y, forward.z, 0);
            set("frame.flags", 0, 1, 0, 0);
            set("frame.environment", 1, 0, std::clamp(scene.specular_aa, 0.0f, 1.0f), 0);
            set("frame.ambient", pass.ambient.r, pass.ambient.g, pass.ambient.b, 0);
            set("frame.ground_ambient", pass.ground.r, pass.ground.g, pass.ground.b, 0);
        }
    }
    // A surface program's material block (native's: what a surface's source reads) and its graph maps.
    void surface_material(MeshProgram& program, const Material3D& m) {
        const auto set = [&](const char* name, f32 x, f32 y, f32 z, f32 w) {
            glUniform4f(program.location(name), x, y, z, w);
        };
        set("material.pbr", m.metallic, m.roughness, m.normal_scale, m.occlusion_strength);
        set("material.emission", m.emission_color.r, m.emission_color.g, m.emission_color.b,
            m.emission_mode == EmissionMode3D::Independent ? 1.0f : 0.0f);
        set("material.params", m.emissive, m.unlit ? 1.0f : 0.0f, m.receives_shadow ? 1.0f : 0.0f,
            m.blend == Blend3D::Mask ? m.alpha_cutoff : 0.0f);
        set("material.uv", m.uv_scale.x, m.uv_scale.y, m.uv_offset.x, m.uv_offset.y);
        set("material.cutoff", 0, 0, pass.scene ? pass.scene->time : 0.0f, 0);
        set("material.maps", m.normal ? 1.0f : 0.0f, m.emissive_texture ? 1.0f : 0.0f, 0, 0);
        glUniform4fv(program.location("material.surface[0]"), GLsizei(m.shader_params.size() / 4),
                     m.shader_params.data());
        const auto* maps = m.graph_maps ? device.texture(m.graph_maps) : nullptr;
        glActiveTexture(GL_TEXTURE0 + graph_maps_unit);
        glBindSampler(graph_maps_unit, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, maps && maps->layered ? maps->handle : graph_maps_dummy);
    }
    // What `scene` (and its overlay) asks for that is left out: the Omitted bits.
    u32 omitted(const Scene3D& scene) const {
        u32 bits = 0;
        const auto& l = scene.lights;
        for (const auto& draw : scene.draws()) {
            bits |= draw.material.shader ? omit_shader : 0;
            bits |= draw.material.wireframe ? omit_wireframe : 0;
            bits |= draw.material.surface && !dynamic_cast<const SurfaceProgram*>(draw.material.surface.backend().get())
                        ? omit_surface
                        : 0;
        }
        bits |= scene.ambient_occlusion.enabled ? omit_ambient_occlusion : 0;
        bits |= scene.contact_shadows.enabled ? omit_contact_shadows : 0;
        bits |= scene.ssr.enabled ? omit_ssr : 0;
        bits |= scene.taa.enabled ? omit_taa : 0;
        bits |= scene.camera_effects.motion_blur.enabled ? omit_motion_blur : 0;
        bits |= scene.camera_effects.depth_of_field.enabled ? omit_depth_of_field : 0;
        bits |= scene.camera_effects.lens_film_or_grade() ? omit_lens_film_grade : 0;
        bits |= scene.global_illumination.enabled ? omit_gi : 0;
        bits |= l.reflection_probes.empty() ? 0 : omit_probes;
        bits |= l.environment || (scene.sky.enabled && scene.sky.mode == SkyMode3D::Image) ? omit_environment : 0;
        bits |= scene.camera.clipped() ? omit_clip : 0;
        bits |= l.points.size() > 16 || l.spots.size() > max_spot_lights ? omit_light_count : 0;
        bits |= scene.sky.enabled && scene.sky.stars ? omit_stars : 0;
        bits |= scene.fog.enabled && scene.fog.volumetric.enabled &&
                        (!float_targets || scene.camera.projection == Projection3D::Orthographic)
                    ? omit_volumetric
                    : 0;
        if (const auto* overlay = scene.overlay_if_any())
            bits |= omitted(*overlay);
        return bits;
    }
    void tell_omitted(const Scene3D& scene) {
        const u32 fresh = omitted(scene) & ~omitted_told;
        for (u32 bit = 0; bit < omitted_text.size(); ++bit)
            if (fresh & (1u << bit))
                KIN_LOG_WARN_F("render", "WebGL2: left out", (LogFields{{.name = "what", .value = omitted_text[bit]}}));
        omitted_told |= fresh;
    }
    void draw(const Scene3D& scene, const Scene3D::Draw& draw, u32 level, std::span<const u32> order,
              PreparedInstances* prepared, u32 pass, RenderDebugStats3D& stats, bool shadow_pass = false) {
        if (order.empty())
            return;
        const auto& mesh = static_cast<const Mesh&>(*draw.mesh.backend());
        const u64 bytes = order.size() * sizeof(Instance);
        ResidentInstances* retained = nullptr;
        if (prepared && pass < prepared->passes.size()) {
            auto& entry = prepared->passes[pass];
            const u64 previous_bytes = entry ? entry->bytes : 0;
            if (instance_budget->gpu_bytes - previous_bytes + bytes <= InstanceBudget::gpu_limit &&
                (entry || instance_budget->buffers < InstanceBudget::buffer_limit)) {
                if (!entry) {
                    entry = std::make_unique<ResidentInstances>(instance_budget, device.alive());
                    glGenBuffers(1, &entry->buffer);
                    glGenVertexArrays(1, &entry->vao);
                    glBindVertexArray(entry->vao);
                    mesh_layout(mesh, entry->buffer);
                }
                retained = entry.get();
            } else
                ++stats.retained_refused;
        }
        const bool upload = !retained || retained->revision != prepared->revision ||
                            !std::equal(order.begin(), order.end(), retained->order.begin(), retained->order.end());
        if (upload) {
            instance_scratch.clear();
            const auto source =
                (draw.batch ? draw.batch.instances() : scene.instances()).subspan(draw.first, draw.count);
            for (const u32 i : order)
                instance_scratch.push_back(
                    prepared ? prepared->records[i]
                             : instance(source[i], draw.skinned ? scene.skin_offsets()[draw.skin_first + i] : 0));
            glBindBuffer(GL_ARRAY_BUFFER, retained ? retained->buffer : instances);
            // Replacing storage preserves already-submitted draws when a view
            // is rendered again with edits; no explicit GPU wait is needed.
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), instance_scratch.data(), GL_STREAM_DRAW);
            if (retained) {
                instance_budget->gpu_bytes = instance_budget->gpu_bytes - retained->bytes + bytes;
                retained->bytes = bytes;
                retained->revision = prepared->revision;
                retained->order.assign(order.begin(), order.end());
                stats.retained_upload_bytes += bytes;
            } else
                stats.instance_upload_bytes += bytes;
        }
        const auto& material = draw.material;
        // Shadow cascades cut by the base map's alpha with kin's own program (as native's do without a cut-out
        // variant).
        MeshProgram& program = shadow_pass ? *stock : program_for(material);
        use(program);
        auto& loc = program.draw;
        if (program.surface)
            surface_material(program, material);
        loc.uv_transform.set({material.uv_scale.x, material.uv_scale.y, material.uv_offset.x, material.uv_offset.y});
        loc.shadow_pass.set(shadow_pass);
        loc.receives_shadow.set(material.receives_shadow);
        const std::array<const Texture*, 5> images{&material.texture, &material.normal, &material.metallic_roughness,
                                                   &material.occlusion, &material.emissive_texture};
        std::array<GLint, 5> flips{};
        for (u32 unit = 0; unit < images.size(); ++unit) {
            const auto* texture = device.texture(*images[unit] ? *images[unit] : white);
            auto& bound = material_bindings[unit];
            constexpr std::array<u32, 5> slots{0, 2, 1, 3, 4};
            auto descriptor = material.texture_samplers[slots[unit]];
            if (descriptor && !texture->mipmapped)
                descriptor->mip = MipFilter3D::None;
            const GLuint sampling = sampler(material.wrap_u, material.wrap_v, texture->scale_mode, descriptor);
            if (bound.texture != texture->handle) {
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, texture->handle);
                bound.texture = texture->handle;
            }
            if (bound.sampler != sampling) {
                glBindSampler(unit, sampling);
                bound.sampler = sampling;
            }
            flips[unit] = texture->flip_v;
        }
        loc.flip.set(flips);
        loc.skinned.set(draw.skinned);
        loc.has_normal.set(bool(material.normal));
        loc.has_emission.set(bool(material.emissive_texture));
        loc.metallic.set(material.metallic);
        loc.roughness.set(material.roughness);
        loc.normal_scale.set(material.normal_scale);
        loc.occlusion_strength.set(material.occlusion_strength);
        loc.color.set(material.color);
        loc.unlit.set(material.unlit);
        loc.emissive.set(material.emissive);
        loc.emission_color.set(material.emission_color);
        loc.independent_emission.set(material.emission_mode == EmissionMode3D::Independent);
        loc.blend.set(int(material.blend));
        loc.alpha_cutoff.set(material.blend == Blend3D::Mask ? material.alpha_cutoff : 0.0f);
        if (material.double_sided)
            glDisable(GL_CULL_FACE);
        else
            glEnable(GL_CULL_FACE);
        const bool blended = material.blend == Blend3D::Alpha || material.blend == Blend3D::Additive;
        glDepthMask(blended ? GL_FALSE : GL_TRUE);
        if (blended) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, material.blend == Blend3D::Additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA,
                                material.blend == Blend3D::Additive ? GL_ZERO : GL_ONE,
                                material.blend == Blend3D::Additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
        } else
            glDisable(GL_BLEND);
        glBindVertexArray(retained ? retained->vao : mesh.vao);
        if (!draw.skinned) {
            glVertexAttribI4ui(12, 0, 0, 0, 0);
            glVertexAttrib4f(13, 1, 0, 0, 0);
        }
        const Level range = mesh.levels[std::min<std::size_t>(level, mesh.levels.size() - 1)];
        glDrawElementsInstanced(GL_TRIANGLES, GLsizei(range.count), GL_UNSIGNED_INT,
                                reinterpret_cast<void*>(std::size_t(range.first) * sizeof(u32)), GLsizei(order.size()));
        if (shadow_pass) {
            ++stats.shadow_draws;
            stats.shadow_casters += u32(order.size());
            return;
        }
        ++stats.draws;
        if (draw.skinned) {
            ++stats.skinned_draws;
            stats.skinned_copies += u32(order.size());
        }
        stats.instances += u32(order.size());
        stats.triangles += u64(range.count / 3) * order.size();
        if (retained)
            stats.retained_instances += u32(order.size());
        if (material.unlit) {
            ++stats.unlit_draws;
            if (material.texture)
                ++stats.unlit_textured_draws;
        }
    }
    // An HDR view's targets (null where float targets can't be drawn: the view is then SDR, told once).
    std::shared_ptr<HdrTargets> hdr_targets(Vec2i size, const ViewOptions3D& options) {
        if (!options.hdr && options.bloom <= 0)
            return {};
        if (!float_targets) {
            if (!hdr_refused_told)
                KIN_LOG_WARN("render", "WebGL2: no EXT_color_buffer_float; HDR views are drawn as SDR (no bloom, "
                                       "exposure or tonemapping)");
            hdr_refused_told = true;
            return {};
        }
        auto t = std::make_shared<HdrTargets>();
        const auto alive = device.alive();
        // The meter: the picture's mean over 2^level pixels a side, at most 64 texels a side.
        while ((size.x >> t->meter_level) > 64 || (size.y >> t->meter_level) > 64)
            ++t->meter_level;
        const u32 levels = options.auto_exposure.enabled ? t->meter_level + 1 : 1;
        t->scene = float_target(alive, size, GL_RGBA16F, false, levels); // (the view's targets hold the depth)
        if (!t->scene)
            return {};
        if (options.bloom > 0) {
            const Vec2i half{std::max(1, size.x / 2), std::max(1, size.y / 2)};
            t->bloom_a = float_target(alive, half, GL_RGBA16F, false);
            t->bloom_b = float_target(alive, half, GL_RGBA16F, false);
            if (!t->bloom_a || !t->bloom_b)
                return {};
        }
        if (options.auto_exposure.enabled) {
            t->meter =
                float_target(alive, {std::max(1, size.x >> t->meter_level), std::max(1, size.y >> t->meter_level)},
                             GL_RGBA32F, false);
            for (auto& state : t->state)
                state = float_target(alive, {1, 1}, GL_RGBA32F, false);
            if (!t->meter || !t->state[0] || !t->state[1])
                return {};
        }
        return t;
    }
    // A full-screen pass of the post program into `framebuffer` (`size`), reading `source` (unit 0).
    void post_pass(GLuint framebuffer, Vec2i size, int mode, GLuint source, std::array<f32, 4> a,
                   RenderDebugStats3D& stats) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, size.x, size.y);
        glUniform1i(location(post_program, "uMode"), mode);
        glUniform2f(location(post_program, "uTarget"), f32(size.x), f32(size.y));
        glUniform4f(location(post_program, "uA"), a[0], a[1], a[2], a[3]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (mode <= 1)
            ++stats.bloom_draws;
        else if (mode <= 3)
            ++stats.exposure_dispatches;
        else
            ++stats.output_draws;
    }
    // An HDR view's float picture out to its 2D texture: bloom, auto exposure's metering and adaptation, then the
    // exposure, ACES and the sRGB encode (native finish_view's order).
    void finish_hdr(View& view, const Scene3D& scene, RenderDebugStats3D& stats) {
        auto& t = *view.hdr;
        const auto& options = view.options;
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glUseProgram(post_program);
        current = nullptr;
        glBindVertexArray(sky_vao);
        for (GLuint unit = 0; unit < 3; ++unit)
            glBindSampler(unit, 0);
        material_bindings = {};
        const bool bloom = options.bloom > 0 && t.bloom_a;
        if (bloom) {
            const Vec2i half = t.bloom_a->size;
            post_pass(t.bloom_a->framebuffer, half, 0, t.scene->texture,
                      {0, options.bloom_threshold, 1.0f / t.scene->size.x, 1.0f / t.scene->size.y}, stats);
            for (int i = 0; i < 2; ++i) {
                const f32 step = f32(i + 1);
                post_pass(t.bloom_b->framebuffer, half, 1, t.bloom_a->texture, {0, 0, step / half.x, 0}, stats);
                post_pass(t.bloom_a->framebuffer, half, 1, t.bloom_b->texture, {0, 0, 0, step / half.y}, stats);
            }
        }
        const bool metered = options.auto_exposure.enabled && t.meter;
        if (metered) {
            const AutoExposure3D& a = options.auto_exposure;
            glBindTexture(GL_TEXTURE_2D, t.scene->texture);
            glGenerateMipmap(GL_TEXTURE_2D);
            post_pass(t.meter->framebuffer, t.meter->size, 2, t.scene->texture,
                      {std::clamp(a.centre_weight, 0.0f, 1.0f), f32(t.meter_level), 0, 0}, stats);
            // dt as native meter_exposure's: the scene's clock, else the wall clock (at most 0.25 s).
            const auto now = std::chrono::steady_clock::now();
            f32 dt = 0;
            if (t.exposure_valid) {
                const f32 step = scene.time - t.exposure_time;
                dt = t.exposure_time_seen && step > 0 && step <= 1
                         ? step
                         : std::min(0.25f, std::chrono::duration<f32>(now - t.exposure_wall).count());
            }
            t.exposure_time = scene.time;
            t.exposure_time_seen = true;
            t.exposure_wall = now;
            const f32 low = std::clamp(a.low, 0.0f, 1.0f), high = std::clamp(a.high, low, 1.0f);
            glUniform4f(location(post_program, "uB"), std::log2(std::max(a.key, 1e-6f)), std::min(a.min_ev, a.max_ev),
                        std::max(a.min_ev, a.max_ev), t.exposure_valid ? 0.0f : 1.0f);
            glUniform4f(location(post_program, "uC"), 1.0f - std::exp(-dt * std::max(a.speed_up, 0.0f)),
                        1.0f - std::exp(-dt * std::max(a.speed_down, 0.0f)), 1.0f, a.compensation_ev);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, t.state[t.state_read]->texture);
            const u32 write = 1 - t.state_read;
            post_pass(t.state[write]->framebuffer, {1, 1}, 3, t.meter->texture, {0, 0, low, high}, stats);
            t.state_read = write;
            t.exposure_valid = true;
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, t.state[t.state_read]->texture);
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloom ? t.bloom_a->texture : t.scene->texture);
        // Dithered as native's for a view with a sky: a night sky's range is a few 8-bit levels.
        post_pass(
            view.surface->framebuffer, view.size(), 4, t.scene->texture,
            {bloom ? options.bloom : 0.0f, options.exposure, metered ? 1.0f : 0.0f, scene.sky.enabled ? 1.0f : 0.0f},
            stats);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }
    // Fog3D::volumetric (native fog_volume.comp and fog_volume_integrate.comp, as fragment passes): the view's volume
    // lit and summed along its rays, for this render's mesh and sky programs to add (fog_state). Needs float targets
    // and a perspective camera; the sun's cascades must be drawn (shadows) first.
    void fog_volume(View& view, const Scene3D& scene, RenderDebugStats3D& stats) {
        fog_state = {};
        const Fog3D& fog = scene.fog;
        const VolumetricFog3D& settings = fog.volumetric;
        if (!fog.enabled || !settings.enabled || !float_targets ||
            scene.camera.projection == Projection3D::Orthographic) {
            view.fog.reset();
            return;
        }
        const Lights3D& lights = scene.lights;
        const Vec2i size = view.size();
        const Mat4f vp = scene.camera.view_projection(f32(size.x) / size.y);
        const Vec3f eye = scene.camera.position;
        // The lights the main pass chose, their colour times the fog's intensity and each one's own `volumetric`.
        const f32 intensity = std::max(settings.intensity, 0.0f);
        std::array<f32, 128> points{};
        std::array<f32, 16 * max_spot_lights> spots{};
        u32 point_count = 0, spot_count = 0;
        std::array<f32, 16> point_shadow{};
        point_shadow.fill(-1.0f);
        for (const u32 i : chosen_lights(std::span{lights.points}, 16, vp, eye,
                                         [](const PointLightSource3D& p, Vec3f& centre, f32& radius) {
                                             centre = p.position;
                                             radius = p.radius;
                                         })) {
            const auto& l = lights.points[i];
            const f32 k = intensity * std::max(l.volumetric, 0.0f);
            if (k <= 0)
                continue;
            const f32 r = std::max(l.size, 0.01f);
            const std::array<f32, 8> packed{l.position.x,  l.position.y,  l.position.z,  std::max(0.01f, l.radius),
                                            l.color.r * k, l.color.g * k, l.color.b * k, r * r};
            point_shadow[point_count] = layer_of(point_layer_of, i, &view);
            std::copy(packed.begin(), packed.end(), points.data() + 8 * point_count++);
        }
        for (const u32 i : chosen_lights(std::span{lights.spots}, max_spot_lights, vp, eye, spot_bounding_sphere)) {
            const auto& l = lights.spots[i];
            const f32 k = intensity * std::max(l.volumetric, 0.0f);
            if (k <= 0)
                continue;
            const SpotCone3D cone = spot_cone(l);
            const f32 r = std::max(l.size, 0.01f);
            const std::array<f32, 16> packed{l.position.x,
                                             l.position.y,
                                             l.position.z,
                                             std::max(0.01f, l.radius),
                                             l.color.r * k,
                                             l.color.g * k,
                                             l.color.b * k,
                                             r * r,
                                             cone.axis.x,
                                             cone.axis.y,
                                             cone.axis.z,
                                             cone.cos_outer,
                                             cone.scale,
                                             layer_of(spot_layer_of, i, &view),
                                             0,
                                             0};
            std::copy(packed.begin(), packed.end(), spots.data() + 16 * spot_count++);
        }
        // The sun and the fill light (sun_and_moon), as the main pass's fog scatters the sun, each through its maps.
        const auto shines = [](const DirectionalLight3D& l) {
            return l.color.r + l.color.g + l.color.b > 0.0f && length_squared(l.direction) > 0.0f;
        };
        const bool directional = settings.sun_and_moon && fog.sun_scatter > 0.0f;
        const bool sun = directional && shines(lights.sun);
        const bool fill = directional && shines(lights.fill);
        if (!point_count && !spot_count && !sun && !fill) {
            view.fog.reset();
            return;
        }
        const u32 tile = settings.tile != 0 ? std::clamp(settings.tile, 4u, 64u)
                                            : std::max(8u, u32(std::lround(f32(size.y) / 90.0f)));
        GLint most = 256;
        glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &most);
        const std::array<u32, 3> cells{std::min((u32(size.x) + tile - 1) / tile, u32(most)),
                                       std::min((u32(size.y) + tile - 1) / tile, u32(most)),
                                       std::clamp(settings.slices, 8u, std::min(256u, u32(most)))};
        if (!view.fog || view.fog->cells != cells) {
            auto v = std::make_shared<FogVolume>();
            v->alive = device.alive();
            v->cells = cells;
            for (GLuint* t : {&v->lit, &v->sum}) {
                glGenTextures(1, t);
                glBindTexture(GL_TEXTURE_3D, *t);
                glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, GLsizei(cells[0]), GLsizei(cells[1]), GLsizei(cells[2]));
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                for (const GLenum axis : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
                    glTexParameteri(GL_TEXTURE_3D, axis, GL_CLAMP_TO_EDGE);
            }
            glGenFramebuffers(1, &v->framebuffer);
            view.fog = std::move(v);
        }
        const FogVolume& volume = *view.fog;
        const f32 depth = std::max(settings.distance, 0.01f);
        // The view's rays at view depth 1 (GL's rows up: the bottom-left corner's first).
        const Mat4f to_world = vp.inverse();
        const Vec3f forward = scene.camera.forward();
        const auto ray = [&](f32 x, f32 y) {
            const Vec4f p = to_world * Vec4f{x, y, 0.5f, 1.0f};
            const Vec3f r = Vec3f{p.x / p.w, p.y / p.w, p.z / p.w} - eye;
            return r * (1.0f / dot(r, forward));
        };
        const Vec3f ray00 = ray(-1, -1), ray_x = ray(1, -1) - ray00, ray_y = ray(-1, 1) - ray00;
        glUseProgram(fog_program);
        current = nullptr;
        const auto loc = [&](const char* name) { return location(fog_program, name); };
        const auto vec3 = [&](const char* name, Vec3f v) { glUniform3f(loc(name), v.x, v.y, v.z); };
        glUniform4f(loc("uCells"), f32(cells[0]), f32(cells[1]), f32(cells[2]), depth);
        vec3("uEye", eye);
        vec3("uRay00", ray00);
        vec3("uRayX", ray_x);
        vec3("uRayY", ray_y);
        glUniform4f(loc("uFog.colour"), fog.colour.r, fog.colour.g, fog.colour.b, 0);
        glUniform4f(loc("uFog.density"), std::max(0.0f, fog.density), std::max(0.0f, fog.height_density),
                    fog.height_falloff, fog.base_height);
        glUniform4f(loc("uFog.limits"), std::max(0.0f, fog.start), std::clamp(fog.max_opacity, 0.0f, 1.0f),
                    std::max(0.0f, fog.sun_scatter), std::clamp(fog.sun_anisotropy, -0.95f, 0.95f));
        glUniform4f(loc("uFog.view"), 0, 0, 0, 0);
        glUniform1f(loc("uAnisotropy"), std::clamp(settings.anisotropy, -0.9f, 0.9f));
        glUniform1f(loc("uLeast"), 1.0f - std::clamp(fog.max_opacity, 0.0f, 1.0f));
        glUniform1i(loc("uPointCount"), GLint(point_count));
        glUniform1i(loc("uSpotCount"), GLint(spot_count));
        upload_lights(fog_light_data, points, spots, point_shadow);
        const Vec3f to_sun = normalize(lights.sun.direction), to_fill = normalize(lights.fill.direction);
        glUniform4f(loc("uSun"), to_sun.x, to_sun.y, to_sun.z, std::clamp(lights.sun_shadow_strength, 0.0f, 1.0f));
        glUniform4f(loc("uSunLight"), sun ? lights.sun.color.r : 0, sun ? lights.sun.color.g : 0,
                    sun ? lights.sun.color.b : 0, 0);
        glUniform4f(loc("uFill"), to_fill.x, to_fill.y, to_fill.z, 0);
        glUniform4f(loc("uFillLight"), fill ? lights.fill.color.r : 0, fill ? lights.fill.color.g : 0,
                    fill ? lights.fill.color.b : 0, 0);
        // The maps (drawn this render), as the mesh programs have them.
        shadow_uniforms(&view, lights, fog_program);
        vec3("uForward", forward);
        bind_shadow_units(true);
        glActiveTexture(GL_TEXTURE7);
        glBindSampler(7, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, view.shadow ? view.shadow->texture : shadow_dummy);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, 0);
        material_bindings = {};
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glBindVertexArray(sky_vao);
        glBindFramebuffer(GL_FRAMEBUFFER, volume.framebuffer);
        glViewport(0, 0, GLsizei(cells[0]), GLsizei(cells[1]));
        // Four slices a draw: lit (mode 0), then summed front to back (mode 1, reading the lit cells).
        for (int mode = 0; mode < 2; ++mode) {
            const GLuint into = mode == 0 ? volume.lit : volume.sum;
            glBindTexture(GL_TEXTURE_3D, mode == 0 ? 0 : volume.lit);
            glUniform1i(loc("uMode"), mode);
            for (u32 first = 0; first < cells[2]; first += 4) {
                std::array<GLenum, 4> buffers{};
                for (u32 i = 0; i < 4; ++i) {
                    const bool used = first + i < cells[2];
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, used ? into : 0, 0,
                                              used ? GLint(first + i) : 0);
                    buffers[i] = used ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
                }
                glDrawBuffers(4, buffers.data());
                glUniform1i(loc("uFirst"), GLint(first));
                glDrawArrays(GL_TRIANGLES, 0, 3);
            }
        }
        const GLenum first_buffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &first_buffer);
        glActiveTexture(GL_TEXTURE0 + fog_volume_unit);
        glBindTexture(GL_TEXTURE_3D, volume.sum);
        fog_state = {depth, sun, size};
        stats.fog_volume = true;
        stats.fog_volume_lights = point_count + spot_count + (sun ? 1 : 0) + (fill ? 1 : 0);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
    }
    // Scene3D's decals (native decal.vert/decal.frag's): each box's back faces, through the view's colour alone, so
    // the fragment reads the depth and normals the opaque receivers left; lit and fogged as a material, blended over
    // them. Lower `order` first; within an order, the decals sharing their pictures (texture, normal map, wrap) are one
    // instanced draw, in the order each picture first appears (native prepare_decals').
    void draw_decals(const Scene3D& scene, const SceneTargets& targets, Vec2i size, RenderDebugStats3D& stats) {
        struct Run {
            const Decal3D* picture; // the first of the run: its texture, normal map and wrap
            i32 order;
            std::vector<const Decal3D*> decals;
        };
        std::vector<Run> runs;
        const auto same_picture = [](const Decal3D& a, const Decal3D& b) {
            return a.texture == b.texture && a.normal == b.normal && a.wrap == b.wrap;
        };
        const Bounds3D box{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        for (const auto& d : scene.decals()) {
            if (scene.cull && !visible(box, pass.view_projection * d.transform)) {
                ++stats.decals_culled;
                continue;
            }
            auto run = std::find_if(runs.begin(), runs.end(),
                                    [&](const Run& r) { return r.order == d.order && same_picture(*r.picture, d); });
            if (run == runs.end())
                run = runs.insert(runs.end(), Run{&d, d.order, {}});
            run->decals.push_back(&d);
        }
        std::stable_sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.order < b.order; });
        decal_records.clear();
        for (const auto& run : runs)
            for (const Decal3D* d : run.decals) {
                const LinearColor c = to_linear(d->color);
                DecalRecord r{.model = d->transform, .to_decal = d->transform.inverse()};
                r.colour = {c.r, c.g, c.b, c.a};
                r.surface = {d->roughness, d->metallic, d->normal ? d->normal_strength : 0.0f, d->emission};
                r.fade = {d->normal_fade, d->depth_fade, 0, 0};
                decal_records.push_back(r);
            }
        if (decal_records.empty())
            return;
        glBindBuffer(GL_ARRAY_BUFFER, decal_buffer);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(decal_records.size() * sizeof(DecalRecord)), decal_records.data(),
                     GL_STREAM_DRAW);
        glBindFramebuffer(GL_FRAMEBUFFER, targets.plain);
        glActiveTexture(GL_TEXTURE0 + decal_depth_unit);
        glBindSampler(decal_depth_unit, 0);
        glBindTexture(GL_TEXTURE_2D, targets.depth);
        glActiveTexture(GL_TEXTURE0 + decal_normals_unit);
        glBindSampler(decal_normals_unit, 0);
        glBindTexture(GL_TEXTURE_2D, targets.normals);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        use(*decal);
        const Mat4f to_world = pass.view_projection.inverse();
        glUniformMatrix4fv(uniform("uToWorld"), 1, GL_FALSE, to_world.m);
        glUniform2f(uniform("uTargetInv"), 1.0f / f32(size.x), 1.0f / f32(size.y));
        glBindVertexArray(decal_vao);
        u32 first = 0;
        for (const auto& run : runs) {
            const Decal3D& d = *run.picture;
            for (const GLuint unit : {0u, 1u}) {
                const Texture& image = unit == 0 ? d.texture : d.normal;
                const auto* texture = device.texture(image ? image : white);
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, texture->handle);
                glBindSampler(unit, sampler(d.wrap, d.wrap, texture->scale_mode));
            }
            // The run's records: two matrices (four columns each), colour, surface, fade.
            const std::size_t at = std::size_t(first) * sizeof(DecalRecord);
            for (GLuint a = 0; a < 11; ++a) {
                glEnableVertexAttribArray(a);
                glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof(DecalRecord),
                                      reinterpret_cast<void*>(at + a * 16));
                glVertexAttribDivisor(a, 1);
            }
            const u32 count = u32(run.decals.size());
            glDrawArraysInstanced(GL_TRIANGLES, 0, 36, GLsizei(count));
            stats.decals_drawn += count;
            ++stats.decal_draws;
            first += count;
        }
        material_bindings = {};
        glCullFace(GL_BACK);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glBindFramebuffer(GL_FRAMEBUFFER, targets.framebuffer);
    }
    static ParticleRecord record_of(const ParticleSprite3D& p) {
        return {{p.position.x, p.position.y, p.position.z, p.size},
                {p.colour.r, p.colour.g, p.colour.b, p.alpha},
                {p.velocity.x, p.velocity.y, p.velocity.z, p.rotation},
                {p.frame, 0, 0, 0}};
    }
    // The scene's sprite sources: its draw_particles batches, and (`weather`) the weather's layers made this render.
    std::vector<ParticleSource> particle_sources(const Scene3D& scene, bool weather) const {
        std::vector<ParticleSource> sources;
        for (const auto& batch : scene.particle_draws())
            sources.push_back({&batch.look, scene.particle_sprites().subspan(batch.first, batch.count)});
        if (weather)
            for (const auto& batch : weather_batches_now)
                sources.push_back({&batch.look, std::span{weather_sprites_now}.subspan(batch.first, batch.count)});
        return sources;
    }
    // The particle program's sprite attributes from record `first` of `buffer`.
    void particle_attributes(GLuint buffer, u32 first) {
        glBindVertexArray(particle_vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        for (GLuint a = 0; a < 4; ++a) {
            glEnableVertexAttribArray(a);
            glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, sizeof(ParticleRecord),
                                  reinterpret_cast<void*>(std::size_t(first) * sizeof(ParticleRecord) + a * 16));
            glVertexAttribDivisor(a, 1);
        }
    }
    // The look's quad and picture uniforms on the particle program (native's ParticleDrawUniforms per batch).
    void particle_look(const ParticleLook3D& look) {
        const auto loc = [&](const char* name) { return location(particle_program, name); };
        const Vec3f axis = length_squared(look.axis) > 0.0f ? normalize(look.axis) : Vec3f{0.0f, 1.0f, 0.0f};
        glUniform4f(loc("uAxis"), axis.x, axis.y, axis.z, f32(u32(look.facing)));
        const u32 columns = std::max(look.atlas_columns, 1u), rows = std::max(look.atlas_rows, 1u);
        const u32 frames = look.atlas_frames > 0 ? std::min(look.atlas_frames, columns * rows) : columns * rows;
        glUniform4f(loc("uAtlas"), f32(columns), f32(rows), f32(frames), look.blend_frames ? 1.0f : 0.0f);
        glUniform2f(loc("uStretch"), look.stretch, look.stretch_min);
        const auto* picture = look.texture ? device.texture(look.texture) : nullptr;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (picture ? picture : device.texture(white))->handle);
        glBindSampler(0, sampler(Wrap3D::Clamp, Wrap3D::Clamp, picture ? picture->scale_mode : ScaleMode::Linear));
        material_bindings = {};
        glUniform4f(loc("uFlags"), 0, 0, f32(u32(look.blend)), picture ? 1.0f : 0.0f);
    }
    // The casting sprites into the cascade being drawn (`matrix`): native draw_particle_shadows' quads, facing the
    // camera, cut by ParticleLook3D::shadow_cutoff.
    void cast_particles(const Scene3D& scene, const Mat4f& matrix, RenderDebugStats3D& stats) {
        if (particle_casts.empty())
            return;
        glUseProgram(particle_program);
        current = nullptr;
        particle_serial = 0; // (the main pass sets its own uniforms again)
        const auto loc = [&](const char* name) { return location(particle_program, name); };
        const Camera3D& camera = scene.camera;
        glUniformMatrix4fv(loc("uViewProjection"), 1, GL_FALSE, matrix.m);
        const auto vec3 = [&](const char* name, Vec3f v) { glUniform3f(loc(name), v.x, v.y, v.z); };
        vec3("uRight", camera.right());
        vec3("uUp", camera.up());
        vec3("uEye", camera.position);
        vec3("uForward", camera.forward());
        glUniform1i(loc("uOrtho"), camera.projection == Projection3D::Orthographic);
        glUniform1i(loc("uCast"), 1);
        glUniform1f(loc("uLit"), 0);
        glDisable(GL_CULL_FACE);
        for (const auto& cast : particle_casts) {
            particle_look(*cast.look);
            const auto& look = *cast.look;
            glUniform4f(loc("uDisc"), look.softness, look.ring, 0, 0);
            glUniform1f(loc("uCutoff"),
                        std::isfinite(look.shadow_cutoff) ? std::clamp(look.shadow_cutoff, 0.0f, 1.0f) : 0.5f);
            particle_attributes(cast_buffer, cast.first);
            glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(cast.count));
            ++stats.particle_shadow_draws;
            stats.particle_shadow_instances += cast.count;
            ++stats.shadow_draws;
            stats.shadow_casters += cast.count;
        }
        glUniform1i(loc("uCast"), 0);
        glEnable(GL_CULL_FACE);
    }
    // A run of `count` particle sprites of one look, from `first` of the uploaded records (native's particle pass: the
    // look's blend, light tint, picture or disc, fog). Soft ones draw through the view's colour alone, reading and
    // testing against its depth.
    void draw_particles(const Scene3D& scene, const ParticleLook3D& look, u32 first, u32 count,
                        const SceneTargets* targets, Vec2i size, RenderDebugStats3D& stats) {
        if (!count)
            return;
        glUseProgram(particle_program);
        current = nullptr;
        const auto loc = [&](const char* name) { return location(particle_program, name); };
        if (particle_serial != pass.serial) {
            environment(particle_program, scene);
            const Camera3D& camera = scene.camera;
            const auto vec3 = [&](const char* name, Vec3f v) { glUniform3f(loc(name), v.x, v.y, v.z); };
            glUniformMatrix4fv(loc("uViewProjection"), 1, GL_FALSE, pass.view_projection.m);
            const Mat4f to_world = pass.view_projection.inverse();
            glUniformMatrix4fv(loc("uToWorld"), 1, GL_FALSE, to_world.m);
            glUniform2f(loc("uTargetInv"), 1.0f / f32(size.x), 1.0f / f32(size.y));
            vec3("uRight", camera.right());
            vec3("uUp", camera.up());
            glUniform1i(loc("uOrtho"), camera.projection == Projection3D::Orthographic);
            glUniform1i(loc("uPointCount"), GLint(pass.point_count));
            glUniform1i(loc("uSpotCount"), GLint(pass.spot_count));
            const Vec3f to_sun = normalize(pass.lights->sun.direction);
            vec3("uSunDirection", to_sun);
            glUniform3f(loc("uSunColor"), pass.lights->sun.color.r, pass.lights->sun.color.g, pass.lights->sun.color.b);
            shadow_uniforms(pass.view, *pass.lights, particle_program);
            vec3("uFillDirection", normalize(pass.lights->fill.direction));
            glUniform3f(loc("uFillColor"), pass.lights->fill.color.r, pass.lights->fill.color.g,
                        pass.lights->fill.color.b);
            particle_serial = pass.serial;
        }
        particle_look(look);
        const f32 lit = std::clamp(look.lit, 0.0f, 1.0f);
        glUniform1f(loc("uLit"), lit);
        const LinearRgb tint = mix(LinearRgb{1, 1, 1}, particle_light_tint(*pass.lights), lit);
        glUniform3f(loc("uTint"), tint.r, tint.g, tint.b);
        const bool receive = look.receives_shadow && lit > 0 && pass.view && pass.view->shadow;
        glUniform1f(loc("uReceive"), receive ? lit : 0.0f);
        stats.particle_shadow_batches += receive ? 1 : 0;
        const bool soft = look.soft && targets && look.soft_distance > 0;
        glUniform4f(loc("uDisc"), look.softness, look.ring, soft ? look.soft_distance : 0.0f, 0);
        glUniform1i(loc("uFogged"), look.fogged);
        if (soft) {
            glBindFramebuffer(GL_FRAMEBUFFER, targets->plain);
            glActiveTexture(GL_TEXTURE1);
            glBindSampler(1, 0);
            glBindTexture(GL_TEXTURE_2D, targets->depth);
            ++stats.particle_soft;
        }
        material_bindings = {}; // (units 0 and 1)
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        if (look.blend == ParticleBlend3D::Additive)
            glBlendFunc(GL_ONE, GL_ONE);
        else
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        particle_attributes(particle_buffer, first);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, GLsizei(count));
        if (soft) {
            glBindTexture(GL_TEXTURE_2D, 0); // (unit 1: the depth is attached again next)
            glBindFramebuffer(GL_FRAMEBUFFER, targets->framebuffer);
        }
        glDisable(GL_BLEND);
        ++stats.particle_batches;
        stats.particles_drawn += count;
    }
    void palette(const Scene3D& scene, RenderDebugStats3D* stats = nullptr) {
        glActiveTexture(GL_TEXTURE5);
        glBindSampler(5, 0);
        glBindTexture(GL_TEXTURE_2D, joints);
        const auto values = scene.joint_matrices();
        if (!values.empty()) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, GLsizei(values.size()), 0, GL_RGBA, GL_FLOAT, values.data());
            if (stats)
                stats->skin_matrices += u32(values.size());
        }
    }
    // A depth array of `layers` layers `side` texels a side (shadow maps), with a framebuffer to draw its layers.
    using DepthArray = DepthArrayHandles;
    DepthArray local, moon_maps; // the point and spot lights' maps, the moon's cascades
    void depth_array(DepthArray& a, u32 side, u32 layers) {
        if (a.texture && a.side == side && a.layers == layers)
            return;
        glDeleteTextures(1, &a.texture);
        glDeleteFramebuffers(1, &a.framebuffer);
        a = {.side = side, .layers = layers};
        glGenTextures(1, &a.texture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, a.texture);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, GLsizei(side), GLsizei(side), GLsizei(layers));
        glGenFramebuffers(1, &a.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, a.framebuffer);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, a.texture, 0, 0);
        const GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(GL_NONE);
    }
    // The shadow casters (opaque meshes that cast, particles that cast) into the map being drawn (`matrix`'s), its
    // layer cleared first; `retained`: the prepared-instance slot of a sun cascade (~0u: stream).
    void cast_into(const Scene3D& scene, const View& view, const Mat4f& matrix, u32 retained,
                   RenderDebugStats3D& stats) {
        glClear(GL_DEPTH_BUFFER_BIT);
        use(*stock);
        glUniformMatrix4fv(uniform("uViewProjection"), 1, GL_FALSE, matrix.m);
        std::vector<u32> copies;
        for (u32 slot = 0; slot < scene.draws().size(); ++slot) {
            const auto& draw = scene.draws()[slot];
            if (!draw.material.casts_shadow || draw.material.blend == Blend3D::Alpha ||
                draw.material.blend == Blend3D::Additive || draw.clip == DrawClip3D::whole_if_cut)
                continue;
            copies.clear();
            const auto source =
                (draw.batch ? draw.batch.instances() : scene.instances()).subspan(draw.first, draw.count);
            for (u32 i = 0; i < source.size(); ++i) {
                const auto& copy = source[i];
                if (!draw.skinned && scene.cull && !visible(draw.mesh.bounds(), matrix * copy.transform))
                    continue;
                copies.push_back(i);
            }
            this->draw(scene, draw, 0, copies,
                       retained != ~0u && slot < view.prepared.size() ? view.prepared[slot].get() : nullptr, retained,
                       stats, true);
        }
        cast_particles(scene, matrix, stats);
    }
    // The shadow maps of this render: the sun's cascades (the view's), the point and spot lights' (local_maps), the
    // moon's cascades. Their texture units hold placeholders meanwhile (no map is read while it is drawn).
    void shadows(View& view, const Scene3D& scene, RenderDebugStats3D& stats) {
        const auto& lights = scene.lights;
        const u32 resolution = std::min(lights.shadow_resolution, u32(device.max_texture_size()));
        view.cascades =
            lights.sun_shadow_strength > 0
                ? fit_shadow_cascades(scene.camera, f32(view.size().x) / view.size().y, lights.sun.direction,
                                      lights.shadow_area, lights.shadow_cascades, lights.shadow_distance, resolution)
                : ShadowCascades3D{};
        for (const GLuint unit : {7u, local_shadow_unit, moon_shadow_unit}) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindSampler(unit, 0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_dummy);
        }
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GREATER);
        glDepthMask(GL_TRUE);
        glClearDepthf(0);
        glFrontFace(GL_CCW);
        glCullFace(GL_BACK);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1, -1);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        pass = {};
        pass.serial = ++serials;
        pass.scene = &scene;
        pass.lights = &scene.lights;
        pass.shadow = true;
        // The sprites that cast (ParticleLook3D::casts_shadow), uploaded once for every map.
        particle_casts.clear();
        for (const auto& source : particle_sources(scene, true)) {
            if (!source.look->casts_shadow)
                continue;
            const u32 first = u32(cast_records.size());
            for (const auto& p : source.sprites)
                cast_records.push_back(record_of(p));
            particle_casts.push_back({source.look, first, u32(source.sprites.size())});
        }
        if (!cast_records.empty()) {
            glBindBuffer(GL_ARRAY_BUFFER, cast_buffer);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(cast_records.size() * sizeof(ParticleRecord)), cast_records.data(),
                         GL_STREAM_DRAW);
            cast_records.clear();
        }
        use(*stock);
        palette(scene);
        if (!view.cascades.count) {
            view.shadow.reset();
        } else {
            if (!view.shadow || view.shadow->resolution != resolution || view.shadow->count != view.cascades.count) {
                auto value = std::make_shared<Shadow>();
                value->alive = device.alive();
                value->resolution = resolution;
                value->count = view.cascades.count;
                DepthArray made;
                depth_array(made, resolution, view.cascades.count);
                value->texture = made.texture;
                value->framebuffer = made.framebuffer;
                view.shadow = std::move(value);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, view.shadow->framebuffer);
            glViewport(0, 0, GLsizei(resolution), GLsizei(resolution));
            for (u32 layer = 0; layer < view.cascades.count; ++layer) {
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, view.shadow->texture, 0, GLint(layer));
                cast_into(scene, view, view.cascades.cascade[layer].view_projection, layer, stats);
            }
        }
        local_maps(view, scene, stats);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        stats.shadow_cascades = view.cascades.count;
    }
    // The point and spot lights' maps (native P3b: the most important lights of those that ask, held across frames by
    // assign_shadow_slots' hysteresis; a spot's perspective map, a point light's six cube faces) and the moon's
    // cascades (Lights3D::fill_shadow), into their arrays, and every map's row of uShadowData.
    void local_maps(const View& view, const Scene3D& scene, RenderDebugStats3D& stats) {
        const Lights3D& lights = scene.lights;
        point_layer_of.assign(lights.points.size(), -1.0f);
        spot_layer_of.assign(lights.spots.size(), -1.0f);
        moon = {};
        const Vec2i size = view.size();
        const Mat4f vp = scene.camera.view_projection(f32(size.x) / size.y);
        const Vec3f eye = scene.camera.position;
        struct Map {
            LocalShadowView3D view;
            u32 res;
            Vec3f position;
            f32 bias, normal_offset, softness, fade;
        };
        std::vector<Map> maps;
        const auto resolution_of = [&](u32 asked) {
            return std::clamp(asked != 0 ? asked : lights.local_shadow_resolution, 64u,
                              std::min(4096u, u32(device.max_texture_size())));
        };
        const auto add = [&](const LocalShadowView3D& v, u32 res, const auto& light) {
            maps.push_back({v, res, light.position, std::max(light.shadow_bias, 0.0f),
                            std::max(light.shadow_normal_offset, 0.0f), std::clamp(light.shadow_softness, 0.25f, 4.0f),
                            local_shadow_far(light.shadow_distance, light.radius)});
        };
        // Spots (of those the main pass draws: in view, the 16 most important), then point lights' cubes.
        const auto spot_order = chosen_lights(std::span{lights.spots}, max_spot_lights, vp, eye, spot_bounding_sphere);
        std::vector<ShadowRequest3D> requests;
        std::vector<i32> slot_of;
        for (const u32 i : spot_order) {
            const auto& l = lights.spots[i];
            Vec3f c;
            f32 r;
            spot_bounding_sphere(l, c, r);
            const u64 key = (1ull << 48) | (l.id != 0 ? (1ull << 32) | l.id : u64(i) + 1);
            requests.push_back({l.shadow_enabled ? key : 0u,
                                shadow_importance(std::max({l.color.r, l.color.g, l.color.b}), l.radius, c, r, eye)});
        }
        spot_slots.held.resize(std::min(lights.max_shadowed_spots, max_spot_shadow_maps), 0);
        assign_shadow_slots(spot_slots, requests, shadow_hysteresis, slot_of);
        for (u32 k = 0; k < spot_order.size(); ++k)
            if (slot_of[k] >= 0) {
                const auto& l = lights.spots[spot_order[k]];
                const u32 res = resolution_of(l.shadow_resolution);
                spot_layer_of[spot_order[k]] = f32(maps.size());
                if (spot_order[k] < RenderDebugStats3D::max_reported_lights)
                    stats.spot_slot[spot_order[k]] = slot_of[k];
                add(spot_shadow_view(l, res), res, l);
            }
        const auto point_order = chosen_lights(std::span{lights.points}, 16, vp, eye,
                                               [](const PointLightSource3D& p, Vec3f& centre, f32& radius) {
                                                   centre = p.position;
                                                   radius = p.radius;
                                               });
        requests.clear();
        for (const u32 i : point_order) {
            const auto& l = lights.points[i];
            const u64 key = (2ull << 48) | (l.id != 0 ? (1ull << 32) | l.id : u64(i) + 1);
            requests.push_back(
                {l.shadow_enabled ? key : 0u,
                 shadow_importance(std::max({l.color.r, l.color.g, l.color.b}), l.radius, l.position, l.radius, eye)});
        }
        point_slots.held.resize(std::min(lights.max_shadowed_points, max_point_shadow_maps), 0);
        assign_shadow_slots(point_slots, requests, shadow_hysteresis, slot_of);
        for (u32 k = 0; k < point_order.size(); ++k)
            if (slot_of[k] >= 0) {
                const auto& l = lights.points[point_order[k]];
                const u32 res = resolution_of(l.shadow_resolution);
                point_layer_of[point_order[k]] = f32(maps.size());
                if (point_order[k] < RenderDebugStats3D::max_reported_lights)
                    stats.point_slot[point_order[k]] = slot_of[k];
                for (u32 face = 0; face < cube_faces; ++face)
                    add(cube_face_view(l, face, res), res, l);
            }
        std::vector<std::array<f32, 24>> rows; // a map's row: its matrix, a, b
        if (!maps.empty()) {
            u32 side = 0;
            for (const auto& m : maps)
                side = std::max(side, m.res);
            depth_array(local, side, u32(maps.size()));
            glBindFramebuffer(GL_FRAMEBUFFER, local.framebuffer);
            for (u32 layer = 0; layer < maps.size(); ++layer) {
                const auto& m = maps[layer];
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, local.texture, 0, GLint(layer));
                // (The map's corner of the layer; the whole layer is cleared, as the scissor is off.)
                glViewport(0, 0, GLsizei(m.res), GLsizei(m.res));
                cast_into(scene, view, m.view.view_projection, ~0u, stats);
                std::array<f32, 24> row{};
                std::copy_n(m.view.view_projection.m, 16, row.data());
                const f32 r = f32(m.res);
                const std::array<f32, 8> ab{r / f32(side),
                                            2.0f * m.view.tan_half / r,
                                            m.view.near_plane,
                                            m.view.far_plane,
                                            m.bias,
                                            m.normal_offset,
                                            m.softness,
                                            m.fade};
                std::copy(ab.begin(), ab.end(), row.data() + 16);
                rows.push_back(row);
                ++stats.local_shadow_drawn;
            }
            local_texel = 1.0f / f32(side);
            stats.local_shadow_maps = u32(maps.size());
        }
        // The moon's cascades.
        const DirectionalShadow3D& fs = lights.fill_shadow;
        const Vec3f area = lights.shadow_area.size();
        if (fs.enabled && fs.strength > 0 && lights.fill.color.r + lights.fill.color.g + lights.fill.color.b > 0 &&
            length_squared(lights.fill.direction) > 0 && area.x > 0 && area.z > 0 && area.y >= 0) {
            const u32 res = std::clamp(fs.resolution != 0 ? fs.resolution : lights.shadow_resolution, 64u,
                                       std::min(8192u, u32(device.max_texture_size())));
            const u32 count = std::clamp(fs.cascades, 1u, max_shadow_cascades);
            const Vec3f to_moon = normalize(lights.fill.direction);
            ShadowCascades3D plan;
            if (count > 1) {
                plan = fit_shadow_cascades(scene.camera, f32(size.x) / size.y, to_moon, lights.shadow_area, count,
                                           fs.distance > 0 ? fs.distance : lights.shadow_distance, res);
            } else {
                const SunBox3D box = fit_sun_box(lights.shadow_area, to_moon);
                plan.count = 1;
                plan.cascade[0].view_projection =
                    Mat4f::orthographic(box.lo.x, box.hi.x, box.lo.y, box.hi.y, box.z_near, box.z_far) * box.light_view;
                plan.cascade[0].texel_world = std::max(box.hi.x - box.lo.x, box.hi.y - box.lo.y) / f32(res);
                plan.cascade[0].depth_range = std::max(box.z_far - box.z_near, 0.01f);
            }
            if (plan.count) {
                depth_array(moon_maps, res, plan.count);
                glBindFramebuffer(GL_FRAMEBUFFER, moon_maps.framebuffer);
                glViewport(0, 0, GLsizei(res), GLsizei(res));
                moon.count = plan.count;
                moon.row = u32(rows.size());
                moon.texel = 1.0f / f32(res);
                moon.strength = std::clamp(fs.strength, 0.0f, 1.0f);
                for (u32 k = 0; k < plan.count; ++k) {
                    const auto& c = plan.cascade[k];
                    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, moon_maps.texture, 0, GLint(k));
                    cast_into(scene, view, c.view_projection, ~0u, stats);
                    std::array<f32, 24> row{};
                    std::copy_n(c.view_projection.m, 16, row.data());
                    row[16] = c.texel_world / c.depth_range;
                    row[17] = c.texel_world * 1.5f;
                    rows.push_back(row);
                    moon.ends[k] = c.slice_far;
                    moon.blend[k] = c.blend_from;
                }
                stats.moon_cascades = plan.count;
            }
        }
        // Their rows: six RGBA32F texels each.
        glActiveTexture(GL_TEXTURE0 + shadow_data_unit);
        glBindTexture(GL_TEXTURE_2D, shadow_data);
        if (!rows.empty())
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 6, GLsizei(rows.size()), 0, GL_RGBA, GL_FLOAT, rows.data());
        has_local = !maps.empty();
    }
    // Every program's shadow units: the maps drawn this render (`real`), else placeholders of the right types.
    void bind_shadow_units(bool real) {
        const std::array<std::pair<GLuint, GLuint>, 2> arrays{
            {{local_shadow_unit, real && has_local ? local.texture : shadow_dummy},
             {moon_shadow_unit, real && moon.count ? moon_maps.texture : shadow_dummy}}};
        for (const auto& [unit, texture] : arrays) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindSampler(unit, 0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        }
        glActiveTexture(GL_TEXTURE0 + shadow_data_unit);
        glBindSampler(shadow_data_unit, 0);
        glBindTexture(GL_TEXTURE_2D, shadow_data);
    }
    // The sun's cascades on the program in use (`view`'s; none for null: the overlay).
    // The sun's cascades on program `handle` (the mesh program in use, or the particles').
    void shadow_uniforms(const View* shadowed, const Lights3D& lights, GLuint handle) {
        const auto uniform = [&](const char* name) { return location(handle, name); };
        // The local maps and the moon's (this render's: none for the overlay).
        glUniform1f(uniform("uLocalTexel"), local_texel);
        glUniform1i(uniform("uMoonCount"), shadowed ? GLint(moon.count) : 0);
        if (shadowed && moon.count) {
            glUniform1i(uniform("uMoonRow"), GLint(moon.row));
            glUniform4fv(uniform("uMoonEnds"), 1, moon.ends.data());
            glUniform4fv(uniform("uMoonBlend"), 1, moon.blend.data());
            glUniform1f(uniform("uMoonTexel"), moon.texel);
            glUniform1f(uniform("uMoonStrength"), moon.strength);
        }
        glUniform1i(uniform("uShadowCount"), shadowed && shadowed->shadow ? GLint(shadowed->cascades.count) : 0);
        glUniform1f(uniform("uShadowStrength"), std::clamp(lights.sun_shadow_strength, 0.0f, 1.0f));
        if (!shadowed || !shadowed->shadow)
            return;
        const View& view = *shadowed;
        std::array<Mat4f, 4> matrices;
        std::array<f32, 4> ends{}, blends{}, bias{}, offset{};
        for (u32 k = 0; k < view.cascades.count; ++k) {
            const auto& c = view.cascades.cascade[k];
            matrices[k] = c.view_projection;
            ends[k] = c.slice_far;
            blends[k] = c.blend_from;
            bias[k] = std::max(0.0f, lights.shadow_bias) * c.texel_world / std::max(1e-6f, c.depth_range);
            offset[k] = std::max(0.0f, lights.shadow_normal_offset) * c.texel_world;
        }
        glUniformMatrix4fv(uniform("uShadowMatrix[0]"), 4, GL_FALSE, matrices[0].m);
        glUniform4fv(uniform("uShadowEnds"), 1, ends.data());
        glUniform4fv(uniform("uShadowBlend"), 1, blends.data());
        glUniform4fv(uniform("uShadowBias"), 1, bias.data());
        glUniform4fv(uniform("uShadowOffset"), 1, offset.data());
        glUniform1f(uniform("uShadowTexel"), 1.0f / view.shadow->resolution);
    }
    void scene(const Scene3D& scene, const Lights3D& lights, Vec2i size, RenderDebugStats3D& stats,
               sky_model::AmbientLight3D& ambient, const std::vector<std::shared_ptr<PreparedInstances>>& prepared,
               const View* shadowed, const SceneTargets* targets) {
        stats.fog_applied = stats.fog_applied || scene.fog.enabled;
        palette(scene, &stats);
        const Mat4f vp = scene.camera.view_projection(f32(size.x) / size.y);
        pass = {};
        auto& points = pass.points;
        const auto point_order = chosen_lights(std::span{lights.points}, 16, vp, scene.camera.position,
                                               [](const PointLightSource3D& p, Vec3f& centre, f32& radius) {
                                                   centre = p.position;
                                                   radius = p.radius;
                                               });
        const u32 count = u32(point_order.size());
        for (u32 i = 0; i < count; ++i) {
            const auto& point = lights.points[point_order[i]];
            auto* at = points.data() + 8 * i;
            at[0] = point.position.x;
            at[1] = point.position.y;
            at[2] = point.position.z;
            at[3] = std::max(0.01f, point.radius);
            at[4] = point.color.r;
            at[5] = point.color.g;
            at[6] = point.color.b;
            at[7] = std::max(0.0001f, point.size * point.size);
            pass.point_shadow[i] = layer_of(point_layer_of, point_order[i], shadowed);
        }
        for (u32 i = count; i < 16; ++i)
            pass.point_shadow[i] = -1.0f;
        pass.point_count = count;
        auto& spots = pass.spots;
        const auto spot_order =
            chosen_lights(std::span{lights.spots}, max_spot_lights, vp, scene.camera.position, spot_bounding_sphere);
        for (u32 i = 0; i < spot_order.size(); ++i) {
            const auto& spot = lights.spots[spot_order[i]];
            const SpotCone3D cone = spot_cone(spot);
            auto* at = spots.data() + 16 * i;
            const std::array<f32, 16> packed{spot.position.x,
                                             spot.position.y,
                                             spot.position.z,
                                             std::max(0.01f, spot.radius),
                                             spot.color.r,
                                             spot.color.g,
                                             spot.color.b,
                                             std::max(0.0001f, spot.size * spot.size),
                                             cone.axis.x,
                                             cone.axis.y,
                                             cone.axis.z,
                                             cone.cos_outer,
                                             cone.scale,
                                             layer_of(spot_layer_of, spot_order[i], shadowed),
                                             0,
                                             0};
            std::copy(packed.begin(), packed.end(), at);
        }
        pass.spot_count = u32(spot_order.size());
        stats.lights += count;
        stats.spots += u32(spot_order.size());
        const bool sky_lighting = scene.sky.enabled && lights.sky_lights_scene;
        if (sky_lighting)
            ambient.update(scene.sky, lights.sun.direction);
        pass.ambient = sky_lighting ? ambient.ambient : lights.ambient;
        pass.ground = sky_lighting ? ambient.ground
                                   : (lights.ground_ambient.enabled ? lights.ground_ambient.color : lights.ambient);
        upload_lights(light_data, pass.points, pass.spots, pass.point_shadow);
        pass.serial = ++serials;
        pass.scene = &scene;
        pass.lights = &lights;
        pass.view = shadowed;
        pass.size = size;
        pass.view_projection = vp;
        glActiveTexture(GL_TEXTURE7);
        glBindSampler(7, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, shadowed && shadowed->shadow ? shadowed->shadow->texture : shadow_dummy);
        // Opaque meshes that take decals, the decals, the opaque meshes that don't (drawn over them: native's order),
        // then what is blended: transparent copies and particle sprites, back to front together.
        struct Transparent {
            const Scene3D::Draw* draw = nullptr; // a mesh's copy, else a sprite
            PreparedInstances* prepared = nullptr;
            u32 level = 0, copy = 0; // (a sprite: its source and its index in it)
            f32 depth = 0;
        };
        std::vector<Transparent> transparent;
        std::array<std::vector<u32>, max_mesh_levels> batches;
        const bool decals = targets && !scene.decals().empty();
        if (decals) {
            const std::array<GLenum, 2> both{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
            glDrawBuffers(2, both.data());
        }
        for (int round = 0; round < 2; ++round) {
            if (round == 1 && decals) {
                const std::array<GLenum, 2> one{GL_COLOR_ATTACHMENT0, GL_NONE};
                glDrawBuffers(2, one.data());
                draw_decals(scene, *targets, size, stats);
            }
            for (u32 slot = 0; slot < scene.draws().size(); ++slot) {
                const auto& draw = scene.draws()[slot];
                if (draw.clip == DrawClip3D::whole_if_cut)
                    continue; // (nothing is cut here: Camera3D::clip_plane is not supported)
                const bool blended = draw.material.blend == Blend3D::Alpha || draw.material.blend == Blend3D::Additive;
                if (blended ? round != 0 : draw.material.receives_decals != (round == 0))
                    continue;
                auto* records = slot < prepared.size() ? prepared[slot].get() : nullptr;
                for (auto& batch : batches)
                    batch.clear();
                const auto copies =
                    (draw.batch ? draw.batch.instances() : scene.instances()).subspan(draw.first, draw.count);
                for (u32 copy_index = 0; copy_index < copies.size(); ++copy_index) {
                    const auto& copy = copies[copy_index];
                    if (!draw.skinned && scene.cull && !visible(draw.mesh.bounds(), vp * copy.transform)) {
                        ++stats.culled;
                        continue;
                    }
                    const u32 level = level_for(scene, draw, copy, f32(size.y));
                    if (blended)
                        transparent.push_back(
                            {&draw, records, level, copy_index,
                             dot(copy.transform.apply(draw.mesh.bounds().center()) - scene.camera.position,
                                 scene.camera.forward())});
                    else
                        batches[level].push_back(copy_index);
                }
                for (u32 level = 0; level < max_mesh_levels; ++level)
                    this->draw(scene, draw, level, batches[level], records, max_shadow_cascades + level, stats);
            }
        }
        // The sprites: the scene's batches and the weather's layers, culled to the view; a source that is not sorted
        // (ParticleLook3D::sort off) is one item at its sprites' mean depth.
        const auto sources = particle_sources(scene, shadowed != nullptr); // (the overlay has no weather)
        if (shadowed)
            for (const auto& batch : weather_batches_now)
                stats.weather_particles += batch.count;
        const Vec3f eye = scene.camera.position, forward = scene.camera.forward();
        std::vector<std::vector<u32>> shown(sources.size());
        for (u32 k = 0; k < sources.size(); ++k) {
            const auto& source = sources[k];
            stats.particles_submitted += u32(source.sprites.size());
            f32 sum = 0;
            for (u32 i = 0; i < source.sprites.size(); ++i) {
                const auto& sprite = source.sprites[i];
                f32 reach = sprite.size * 0.75f;
                if (source.look->facing == ParticleFacing3D::Stretched)
                    reach += length(particle_streak(sprite, *source.look));
                const Vec3f r{reach, reach, reach};
                if (scene.cull && !visible({sprite.position - r, sprite.position + r}, vp))
                    continue;
                const f32 depth = dot(sprite.position - eye, forward);
                shown[k].push_back(i);
                if (source.look->sort)
                    transparent.push_back({nullptr, nullptr, k, i, depth});
                else
                    sum += depth;
            }
            if (!source.look->sort && !shown[k].empty())
                transparent.push_back({nullptr, nullptr, k, ~0u, sum / f32(shown[k].size())});
        }
        std::stable_sort(transparent.begin(), transparent.end(),
                         [](const auto& a, const auto& b) { return a.depth > b.depth; });
        // The sprites' records in the order they are drawn (one upload); consecutive sprites of a source are one draw.
        particle_records.clear();
        for (const auto& item : transparent) {
            if (item.draw)
                continue;
            const auto& source = sources[item.level];
            const auto put = [&](u32 i) { particle_records.push_back(record_of(source.sprites[i])); };
            if (item.copy == ~0u)
                for (const u32 i : shown[item.level])
                    put(i);
            else
                put(item.copy);
        }
        if (!particle_records.empty()) {
            glBindBuffer(GL_ARRAY_BUFFER, particle_buffer);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(particle_records.size() * sizeof(ParticleRecord)),
                         particle_records.data(), GL_STREAM_DRAW);
        }
        u32 record = 0;
        for (std::size_t i = 0; i < transparent.size();) {
            const auto& item = transparent[i];
            if (item.draw) {
                draw(scene, *item.draw, item.level, {&item.copy, 1}, item.prepared, ~0u, stats);
                ++i;
                continue;
            }
            u32 count = 0;
            std::size_t j = i;
            for (; j < transparent.size() && !transparent[j].draw && transparent[j].level == item.level; ++j)
                count += transparent[j].copy == ~0u ? u32(shown[item.level].size()) : 1u;
            draw_particles(scene, *sources[item.level].look, record, count, targets, size, stats);
            record += count;
            i = j;
        }
    }
};

WebGLRenderer3D::WebGLRenderer3D(WebGL2Renderer2DBackend& renderer) : _impl(std::make_unique<Impl>(renderer)) {}
WebGLRenderer3D::~WebGLRenderer3D() = default;
RendererCapabilities3D WebGLRenderer3D::capabilities() const {
    return _impl->caps;
}
std::shared_ptr<IMeshBackend3D> WebGLRenderer3D::create_mesh(const MeshData3D& data, const MeshLodChain3D* chain,
                                                             std::shared_ptr<const MeshClusters3D> clusters) {
    if ((data.skinned() && !_impl->caps.skinning) || clusters) {
        KIN_LOG_WARN("render", "WebGL2: insufficient skin attributes or unsupported mesh clusters");
        return {};
    }
    _impl->renderer.before_3d();
    auto mesh = std::make_shared<Mesh>();
    mesh->alive = _impl->device.alive();
    auto vertices = data.vertices;
    if (chain)
        vertices.insert(vertices.end(), chain->extra_vertices.begin(), chain->extra_vertices.end());
    auto indices = data.indices;
    if (indices.empty()) {
        indices.resize(data.vertices.size());
        std::iota(indices.begin(), indices.end(), 0u);
    }
    mesh->levels.push_back({0, u32(indices.size())});
    if (chain)
        for (std::size_t i = 1; i < chain->levels.size(); ++i) {
            mesh->levels.push_back({u32(indices.size()), u32(chain->levels[i].indices.size())});
            indices.insert(indices.end(), chain->levels[i].indices.begin(), chain->levels[i].indices.end());
        }
    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);
    glGenBuffers(1, &mesh->vertices);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vertices);
    mesh->dynamic = data.morphed();
    mesh->vertex_count = u32(data.vertices.size());
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(MeshVertex3D)), vertices.data(),
                 mesh->dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    if (data.skinned()) {
        std::vector<Skin> skin;
        skin.reserve(data.skin.size());
        for (const auto& vertex : data.skin)
            skin.push_back({vertex.joints, quantise_skin_weights(vertex.weights)});
        glGenBuffers(1, &mesh->skin);
        glBindBuffer(GL_ARRAY_BUFFER, mesh->skin);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(skin.size() * sizeof(Skin)), skin.data(), GL_STATIC_DRAW);
    }
    glGenBuffers(1, &mesh->indices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->indices);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(indices.size() * sizeof(u32)), indices.data(), GL_STATIC_DRAW);
    mesh_layout(*mesh, _impl->instances);
    return mesh;
}
bool WebGLRenderer3D::update_mesh_vertices(IMeshBackend3D& backend, std::span<const MeshVertex3D> vertices) {
    auto* mesh = dynamic_cast<Mesh*>(&backend);
    if (!mesh || !mesh->dynamic || !mesh->alive || !*mesh->alive || vertices.size() != mesh->vertex_count)
        return false;
    _impl->renderer.before_3d();
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vertices);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(vertices.size_bytes()), vertices.data());
    return true;
}
std::shared_ptr<IViewBackend3D> WebGLRenderer3D::create_view(Vec2i size, const ViewOptions3D& options) {
    if (options.render_scale != 1.0f || options.temporal_history_scale != 1.0f)
        return {};
    if (options.pick && !_impl->pick_told) {
        _impl->pick_told = true;
        KIN_LOG_WARN("render", "WebGL2: picking is not implemented; Renderer3D::pick finds nothing");
    }
    _impl->renderer.before_3d();
    auto view = std::make_shared<View>();
    view->options = options;
    view->hdr = _impl->hdr_targets(size, options);
    // An HDR view's 2D texture takes the output pass; it needs no depth of its own.
    view->color = _impl->device.create_surface(size, false);
    view->surface = _impl->device.surface(view->color);
    if (!view->surface)
        return nullptr;
    GLint limit = 0;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &limit);
    if (size.x > limit || size.y > limit)
        return nullptr;
    view->targets =
        scene_targets(_impl->device.alive(), size, view->hdr ? view->hdr->scene->texture : view->surface->handle);
    return view->targets ? view : nullptr;
}
bool WebGLRenderer3D::resize_view(IViewBackend3D& backend, Vec2i size) {
    auto* view = dynamic_cast<View*>(&backend);
    if (!view || !view->surface || view->surface->alive != _impl->device.alive())
        return false;
    auto next = create_view(size, view->options);
    if (!next)
        return false;
    *view = static_cast<View&>(*next);
    return true;
}
bool WebGLRenderer3D::warm_up(IViewBackend3D& backend, const Scene3D&) {
    const auto* view = dynamic_cast<const View*>(&backend);
    return view && view->surface && view->surface->alive == _impl->device.alive();
}
bool WebGLRenderer3D::render(IViewBackend3D& backend, const Scene3D& scene, RenderDebugStats3D& stats) {
    auto* view = dynamic_cast<View*>(&backend);
    if (!view || !view->surface || view->surface->alive != _impl->device.alive() ||
        !_impl->supported(scene, view->surface.get()))
        return false;
    _impl->tell_omitted(scene);
    _impl->renderer.before_3d();
    // The 2D pass and texture uploads can change bindings (and the program) between 3D renders.
    _impl->material_bindings = {};
    _impl->current = nullptr;
    // Every pass of this render, the overlay's too, reads mipmapped textures with the scene's anisotropy.
    _impl->anisotropy = std::clamp(scene.anisotropy, 1u, _impl->caps.max_anisotropy);
    _impl->prepare(view->prepared, scene, stats);
    _impl->linear_output = bool(view->hdr);
    // The weather's sprites, made once: the shadows' casts and the main pass's draws read them.
    _impl->weather_sprites_now.clear();
    _impl->weather_batches_now.clear();
    if (scene.weather.active())
        weather_sprites(scene.weather, scene.camera, _impl->weather_sprites_now, _impl->weather_batches_now);
    _impl->shadows(*view, scene, stats);
    _impl->bind_shadow_units(true);
    _impl->fog_volume(*view, scene, stats);
    if (!view->fog) {
        glActiveTexture(GL_TEXTURE0 + fog_volume_unit);
        glBindTexture(GL_TEXTURE_3D, _impl->fog_volume_dummy);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, view->targets->framebuffer);
    glViewport(0, 0, view->size().x, view->size().y);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glClearDepthf(0.0f);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    const Color color = view->options.background;
    const f32 alpha = color.a / 255.0f;
    if (view->hdr) { // linear light, as native's float picture holds it
        const LinearColor l = to_linear(color);
        glClearColor(l.r * l.a, l.g * l.a, l.b * l.a, l.a);
    } else
        glClearColor(color.r / 255.0f * alpha, color.g / 255.0f * alpha, color.b / 255.0f * alpha, alpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    _impl->sky(scene, view->size(), stats);
    _impl->scene(scene, scene.lights, view->size(), stats, view->ambient, view->prepared, view, view->targets.get());
    if (const auto* overlay = scene.overlay_if_any()) {
        _impl->prepare(view->overlay_prepared, *overlay, stats);
        _impl->fog_state = {}; // (the overlay's things are not in the scene's air)
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        // (The overlay takes no decals; its soft particles still read the depth.)
        _impl->scene(*overlay, scene.lights, view->size(), stats, view->overlay_ambient, view->overlay_prepared,
                     nullptr, view->targets.get());
    } else
        view->overlay_prepared.clear();
    if (view->hdr)
        _impl->finish_hdr(*view, scene, stats);
    stats.retained_gpu_bytes = _impl->instance_budget->gpu_bytes;
    stats.samples = 1;
    return true;
}
std::shared_ptr<ISurfaceShaderBackend3D> WebGLRenderer3D::create_surface_source(std::string_view surface) {
    _impl->renderer.before_3d();
    try {
        auto made = std::make_shared<SurfaceProgram>();
        made->program = _impl->surface_program(surface);
        if (surface_has_offset(surface))
            KIN_LOG_WARN("render", "WebGL2: a surface's vertex offset (World Position Offset) is not drawn");
        return made;
    } catch (const std::exception& e) {
        KIN_LOG_WARN_F("render", "WebGL2: surface shader not made", (LogFields{{.name = "reason", .value = e.what()}}));
        return {};
    }
}
Texture WebGLRenderer3D::create_map_array(Vec2i size, std::span<const u8* const> layers) {
    if (layers.empty() || size.x <= 0 || size.y <= 0 || size.x > _impl->device.max_texture_size() ||
        size.y > _impl->device.max_texture_size())
        return {};
    _impl->renderer.before_3d();
    auto value = std::make_shared<WebGLTexture>(size, _impl->device.alive());
    value->layered = true;
    value->mipmapped = true;
    glGenTextures(1, &value->handle);
    glActiveTexture(GL_TEXTURE0 + graph_maps_unit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, value->handle);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const auto levels = GLsizei(std::floor(std::log2(f32(std::max(size.x, size.y)))) + 1);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, GL_RGBA8, size.x, size.y, GLsizei(layers.size()));
    for (u32 i = 0; i < layers.size(); ++i)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, GLint(i), size.x, size.y, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                        layers[i]);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    _impl->material_bindings = {};
    return Texture{std::move(value)};
}
Result3D<PickResult3D> WebGLRenderer3D::pick(IViewBackend3D&, Vec2i) {
    return {std::nullopt, {"WebGL2 picking is not implemented yet"}};
}
} // namespace kin
