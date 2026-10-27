#include "webgl2_renderer3d.hpp"

#include "webgl2_renderer2d_backend.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <emscripten/html5.h>
#include <kin/platform/log.hpp>
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
struct PreparedInstances;
struct View final : IViewBackend3D {
    Texture color;
    std::shared_ptr<WebGLTexture> surface;
    ViewOptions3D options;
    std::shared_ptr<Shadow> shadow;
    ShadowCascades3D cascades;
    sky_model::AmbientLight3D ambient, overlay_ambient;
    std::vector<std::shared_ptr<PreparedInstances>> prepared, overlay_prepared;
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
    const f32 per_unit = ortho ? height / std::max(1e-6f, scene.camera.height)
                               : height * 0.5f / std::tan(math3d_detail::radians(scene.camera.fov) * 0.5f);
    for (u32 l = count - 1; l > 0; --l)
        if (draw.mesh.level_error(l) * scale * per_unit <= pixels * (ortho ? 1.0f : distance))
            return l;
    return 0;
}
} // namespace

struct WebGLRenderer3D::Impl {
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
    } draw_uniforms{};
    struct MaterialBinding {
        GLuint texture = 0, sampler = 0;
    };
    WebGL2Renderer2DBackend& renderer;
    WebGLDevice& device;
    GLuint program = 0, instances = 0, joints = 0, fog_texture = 0, sky_program = 0, sky_vao = 0, shadow_dummy = 0;
    Texture white;
    RendererCapabilities3D caps{};
    bool warned = false;
    std::unordered_map<std::string_view, GLint> uniforms, sky_uniforms;
    sky_model::FogTable3D fog_table;
    // By wrap and filter, then the mipmapped ones' by anisotropy (2 to 16) and wrap; the mipmapped ones read with
    // `anisotropy`, this render's Scene3D::anisotropy, clamped.
    std::array<GLuint, 27 + 15 * 9> samplers{};
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
        const auto vertex = WebGLDevice::shader_source("mesh.vert.glsl");
        const auto fragment = WebGLDevice::shader_source("mesh.frag.glsl");
        program = WebGLDevice::program(vertex.c_str(), fragment.c_str());
        const auto sky_vertex = WebGLDevice::shader_source("sky.vert.glsl");
        const auto sky_fragment = WebGLDevice::shader_source("sky.frag.glsl");
        sky_program = WebGLDevice::program(sky_vertex.c_str(), sky_fragment.c_str());
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
        for (const char* name : {"uViewProjection",
                                 "uSunDirection",
                                 "uSunColor",
                                 "uFillDirection",
                                 "uFillColor",
                                 "uAmbient",
                                 "uGround",
                                 "uEye",
                                 "uTexture",
                                 "uNormal",
                                 "uMetalRough",
                                 "uOcclusion",
                                 "uEmission",
                                 "uFlip[0]",
                                 "uColor",
                                 "uUnlit",
                                 "uEmissive",
                                 "uBlend",
                                 "uAlphaCutoff",
                                 "uMetallic",
                                 "uRoughness",
                                 "uNormalScale",
                                 "uOcclusionStrength",
                                 "uHasNormal",
                                 "uHasEmission",
                                 "uPointCount",
                                 "uPoints[0]",
                                 "uSkinned",
                                 "uJoints"})
            uniforms.emplace(name, glGetUniformLocation(program, name));
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
        draw_uniforms = {uniform("uShadowPass"),        uniform("uReceivesShadow"), uniform("uFlip[0]"),
                         uniform("uSkinned"),           uniform("uHasNormal"),      uniform("uHasEmission"),
                         uniform("uMetallic"),          uniform("uRoughness"),      uniform("uNormalScale"),
                         uniform("uOcclusionStrength"), uniform("uColor"),          uniform("uUnlit"),
                         uniform("uEmissive"),          uniform("uBlend"),          uniform("uAlphaCutoff")};
        // Sampler units belong to the linked program, not individual draws.
        glUseProgram(program);
        constexpr std::array names{"uTexture",  "uNormal", "uMetalRough", "uOcclusion",
                                   "uEmission", "uJoints", "uFogTable",   "uShadow"};
        for (u32 unit = 0; unit < names.size(); ++unit)
            glUniform1i(uniform(names[unit]), GLint(unit));
        caps.depth_bits = 24;
        caps.max_spot_lights = 0; // the initial mesh path draws no spot lights (and the first 16 point lights)
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
        glDeleteSamplers(GLsizei(samplers.size()), samplers.data());
        glDeleteBuffers(1, &instances);
        glDeleteTextures(1, &joints);
        glDeleteTextures(1, &fog_texture);
        glDeleteProgram(sky_program);
        glDeleteVertexArrays(1, &sky_vao);
        glDeleteTextures(1, &shadow_dummy);
        glDeleteProgram(program);
    }
    GLint location(GLuint handle, const char* name) {
        auto& cache = handle == program ? uniforms : sky_uniforms;
        const auto found = cache.find(name);
        if (found != cache.end())
            return found->second;
        return cache.emplace(name, glGetUniformLocation(handle, name)).first->second;
    }
    GLint uniform(const char* name) { return location(program, name); }
    GLuint sampler(Wrap3D u, Wrap3D v, ScaleMode mode) {
        const u32 filter = mode == ScaleMode::Nearest ? 0 : mode == ScaleMode::Mipmapped ? 2 : 1;
        const u32 slant = filter == 2 ? anisotropy : 1;
        auto& value =
            samplers[slant > 1 ? 27 + (slant - 2) * 9 + u32(u) * 3 + u32(v) : u32(u) * 9 + u32(v) * 3 + filter];
        if (!value) {
            glGenSamplers(1, &value);
            glSamplerParameteri(value, GL_TEXTURE_WRAP_S, wrap(u));
            glSamplerParameteri(value, GL_TEXTURE_WRAP_T, wrap(v));
            glSamplerParameteri(value, GL_TEXTURE_MIN_FILTER,
                                filter == 2   ? GL_LINEAR_MIPMAP_LINEAR
                                : filter == 0 ? GL_NEAREST
                                              : GL_LINEAR);
            glSamplerParameteri(value, GL_TEXTURE_MAG_FILTER, filter == 0 ? GL_NEAREST : GL_LINEAR);
            if (slant > 1)
                glSamplerParameterf(value, texture_max_anisotropy, GLfloat(slant));
        }
        return value;
    }
    void environment(GLuint handle, const Scene3D& scene) {
        glUseProgram(handle);
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
        if (!scene.sky.enabled)
            return;
        environment(sky_program, scene);
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
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(sky_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        stats.sky_drawn = true;
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
            if (!mesh || mesh->alive != device.alive() || (draw.skinned && !caps.skinning) || material.wireframe ||
                material.shader) {
                KIN_LOG_WARN("render", "WebGL2: foreign mesh, wireframe or custom material shader is unsupported");
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
        auto& loc = draw_uniforms;
        loc.shadow_pass.set(shadow_pass);
        loc.receives_shadow.set(material.receives_shadow);
        const std::array<const Texture*, 5> images{&material.texture, &material.normal, &material.metallic_roughness,
                                                   &material.occlusion, &material.emissive_texture};
        std::array<GLint, 5> flips{};
        for (u32 unit = 0; unit < images.size(); ++unit) {
            const auto* texture = device.texture(*images[unit] ? *images[unit] : white);
            auto& bound = material_bindings[unit];
            const GLuint sampling = sampler(material.wrap_u, material.wrap_v, texture->scale_mode);
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
    void shadows(View& view, const Scene3D& scene, RenderDebugStats3D& stats) {
        const auto& lights = scene.lights;
        const u32 resolution = std::min(lights.shadow_resolution, u32(device.max_texture_size()));
        view.cascades =
            lights.sun_shadow_strength > 0
                ? fit_shadow_cascades(scene.camera, f32(view.size().x) / view.size().y, lights.sun.direction,
                                      lights.shadow_area, lights.shadow_cascades, lights.shadow_distance, resolution)
                : ShadowCascades3D{};
        glActiveTexture(GL_TEXTURE7);
        glBindSampler(7, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_dummy);
        if (!view.cascades.count) {
            view.shadow.reset();
            return;
        }
        if (!view.shadow || view.shadow->resolution != resolution || view.shadow->count != view.cascades.count) {
            auto value = std::make_shared<Shadow>();
            value->alive = device.alive();
            value->resolution = resolution;
            value->count = view.cascades.count;
            glGenTextures(1, &value->texture);
            glBindTexture(GL_TEXTURE_2D_ARRAY, value->texture);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT24, GLsizei(resolution), GLsizei(resolution),
                         GLsizei(value->count), 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
            glGenFramebuffers(1, &value->framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, value->framebuffer);
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, value->texture, 0, 0);
            const GLenum none = GL_NONE;
            glDrawBuffers(1, &none);
            glReadBuffer(GL_NONE);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("WebGL2 cascaded depth framebuffer is incomplete");
            view.shadow = std::move(value);
            glBindTexture(GL_TEXTURE_2D_ARRAY, shadow_dummy);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, view.shadow->framebuffer);
        glViewport(0, 0, GLsizei(resolution), GLsizei(resolution));
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
        environment(program, scene);
        palette(scene);
        std::vector<u32> copies;
        for (u32 layer = 0; layer < view.cascades.count; ++layer) {
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, view.shadow->texture, 0, GLint(layer));
            glClear(GL_DEPTH_BUFFER_BIT);
            const Mat4f& matrix = view.cascades.cascade[layer].view_projection;
            glUniformMatrix4fv(uniform("uViewProjection"), 1, GL_FALSE, matrix.m);
            for (u32 slot = 0; slot < scene.draws().size(); ++slot) {
                const auto& draw = scene.draws()[slot];
                if (!draw.material.casts_shadow || draw.material.blend == Blend3D::Alpha ||
                    draw.material.blend == Blend3D::Additive)
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
                this->draw(scene, draw, 0, copies, slot < view.prepared.size() ? view.prepared[slot].get() : nullptr,
                           layer, stats, true);
            }
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        stats.shadow_cascades = view.cascades.count;
    }
    void shadow_uniforms(const View& view, const Lights3D& lights) {
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE7);
        glBindSampler(7, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, view.shadow ? view.shadow->texture : shadow_dummy);
        glUniform1i(uniform("uShadowCount"), GLint(view.cascades.count));
        glUniform1f(uniform("uShadowStrength"), std::clamp(lights.sun_shadow_strength, 0.0f, 1.0f));
        if (!view.shadow)
            return;
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
               sky_model::AmbientLight3D& ambient, const std::vector<std::shared_ptr<PreparedInstances>>& prepared) {
        environment(program, scene);
        stats.fog_applied = stats.fog_applied || scene.fog.enabled;
        palette(scene, &stats);
        glUniform3f(uniform("uEye"), scene.camera.position.x, scene.camera.position.y, scene.camera.position.z);
        std::array<f32, 128> points{};
        const u32 count = std::min<std::size_t>(16, lights.points.size());
        for (u32 i = 0; i < count; ++i) {
            const auto& point = lights.points[i];
            auto* at = points.data() + 8 * i;
            at[0] = point.position.x;
            at[1] = point.position.y;
            at[2] = point.position.z;
            at[3] = std::max(0.01f, point.radius);
            at[4] = point.color.r;
            at[5] = point.color.g;
            at[6] = point.color.b;
            at[7] = std::max(0.0001f, point.size * point.size);
        }
        glUniform1i(uniform("uPointCount"), GLint(count));
        glUniform4fv(uniform("uPoints[0]"), GLsizei(count * 2), points.data());
        stats.lights += count;
        const Mat4f vp = scene.camera.view_projection(f32(size.x) / size.y);
        glUniformMatrix4fv(uniform("uViewProjection"), 1, GL_FALSE, vp.m);
        direction("uSunDirection", lights.sun.direction);
        direction("uFillDirection", lights.fill.direction);
        rgb("uSunColor", lights.sun.color);
        rgb("uFillColor", lights.fill.color);
        const bool sky_lighting = scene.sky.enabled && lights.sky_lights_scene;
        if (sky_lighting)
            ambient.update(scene.sky, lights.sun.direction);
        rgb("uAmbient", sky_lighting ? ambient.ambient : lights.ambient);
        glUniform1f(uniform("uSpecularAa"), std::clamp(scene.specular_aa, 0.0f, 1.0f));
        rgb("uGround", sky_lighting ? ambient.ground
                                    : (lights.ground_ambient.enabled ? lights.ground_ambient.color : lights.ambient));
        struct Transparent {
            const Scene3D::Draw* draw;
            PreparedInstances* prepared;
            u32 level, copy;
            f32 depth;
        };
        std::vector<Transparent> transparent;
        std::array<std::vector<u32>, max_mesh_levels> batches;
        for (u32 slot = 0; slot < scene.draws().size(); ++slot) {
            const auto& draw = scene.draws()[slot];
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
                if (draw.material.blend == Blend3D::Alpha || draw.material.blend == Blend3D::Additive)
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
        std::stable_sort(transparent.begin(), transparent.end(),
                         [](const auto& a, const auto& b) { return a.depth > b.depth; });
        for (const auto& item : transparent) {
            draw(scene, *item.draw, item.level, {&item.copy, 1}, item.prepared, ~0u, stats);
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
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(MeshVertex3D)), vertices.data(), GL_STATIC_DRAW);
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
std::shared_ptr<IViewBackend3D> WebGLRenderer3D::create_view(Vec2i size, const ViewOptions3D& options) {
    if (options.hdr || options.bloom > 0 || options.pick) {
        KIN_LOG_WARN("render", "WebGL2: HDR, bloom and picking views are not implemented yet; use an SDR view");
        return {};
    }
    _impl->renderer.before_3d();
    auto view = std::make_shared<View>();
    view->options = options;
    view->color = _impl->device.create_surface(size, true);
    view->surface = _impl->device.surface(view->color);
    return view->surface ? view : nullptr;
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
bool WebGLRenderer3D::render(IViewBackend3D& backend, const Scene3D& scene, RenderDebugStats3D& stats) {
    auto* view = dynamic_cast<View*>(&backend);
    if (!view || !view->surface || view->surface->alive != _impl->device.alive() ||
        !_impl->supported(scene, view->surface.get()))
        return false;
    if (!_impl->warned && (!scene.lights.spots.empty() || !scene.particle_draws().empty())) {
        _impl->warned = true;
        KIN_LOG_WARN("render", "WebGL2: initial mesh path omits spot lights and particles");
    }
    _impl->renderer.before_3d();
    // The 2D pass and texture uploads can change bindings between 3D renders.
    _impl->material_bindings = {};
    // Every pass of this render, the overlay's too, reads mipmapped textures with the scene's anisotropy.
    _impl->anisotropy = std::clamp(scene.anisotropy, 1u, _impl->caps.max_anisotropy);
    _impl->prepare(view->prepared, scene, stats);
    _impl->shadows(*view, scene, stats);
    _impl->shadow_uniforms(*view, scene.lights);
    glBindFramebuffer(GL_FRAMEBUFFER, view->surface->framebuffer);
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
    glClearColor(color.r / 255.0f * alpha, color.g / 255.0f * alpha, color.b / 255.0f * alpha, alpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    _impl->sky(scene, view->size(), stats);
    _impl->scene(scene, scene.lights, view->size(), stats, view->ambient, view->prepared);
    if (const auto* overlay = scene.overlay_if_any()) {
        _impl->prepare(view->overlay_prepared, *overlay, stats);
        glUseProgram(_impl->program);
        glUniform1i(_impl->uniform("uShadowCount"), 0);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        _impl->scene(*overlay, scene.lights, view->size(), stats, view->overlay_ambient, view->overlay_prepared);
    } else
        view->overlay_prepared.clear();
    stats.retained_gpu_bytes = _impl->instance_budget->gpu_bytes;
    stats.samples = 1;
    return true;
}
Result3D<PickResult3D> WebGLRenderer3D::pick(IViewBackend3D&, Vec2i) {
    return {std::nullopt, {"WebGL2 picking is not implemented yet"}};
}
} // namespace kin
