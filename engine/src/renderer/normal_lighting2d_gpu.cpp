#include <algorithm>
#include <cmath>
#include <kin/renderer/normal_lighting2d.hpp>
#include <map>

namespace kin {
bool draw_normal_sprite2d_gpu(Renderer2D& renderer, const SurfaceMaterial2D& m, const SurfaceGeometry2D& g,
                              Color ambient, std::span<const SurfaceLight2D> lights,
                              std::span<const LightOccluder2D> occluders, u64 fingerprint,
                              NormalLightingGpuCache2D& cache, SurfaceGpuTextures2D supplied) {
    cache.used = false;
    const auto caps = renderer.capabilities();
    if (!caps.shader_geometry || !caps.data_buffers || !caps.data_textures || !m.albedo || !m.albedo->valid() ||
        lights.size() > 256)
        return false;
    const auto shader = renderer.builtin_shader(BuiltinShader::NormalSprite2D);
    if (!shader)
        return false;
    auto source = g.source;
    if (source.w <= 0 || source.h <= 0)
        source = {0, 0, static_cast<f32>(m.albedo->size.x), static_cast<f32>(m.albedo->size.y)};
    if (!std::isfinite(g.dest.x) || !std::isfinite(g.dest.y) || !std::isfinite(g.dest.w) || !std::isfinite(g.dest.h) ||
        g.dest.w <= 0 || g.dest.h <= 0 || !std::isfinite(g.rotation) || !std::isfinite(g.pivot.x) ||
        !std::isfinite(g.pivot.y) || !std::isfinite(source.x) || !std::isfinite(source.y) || !std::isfinite(source.w) ||
        !std::isfinite(source.h) || source.x < 0 || source.y < 0 || source.w < 1 || source.h < 1 ||
        source.x + source.w > m.albedo->size.x || source.y + source.h > m.albedo->size.y ||
        !std::isfinite(m.normal_strength) || m.normal_strength < 0 || !std::isfinite(m.specular_strength) ||
        m.specular_strength < 0 || !std::isfinite(m.shininess) || m.shininess <= 0)
        return false;
    if (cache.renderer != renderer.id()) {
        cache = {};
        cache.renderer = renderer.id();
    }
    const std::array<std::shared_ptr<const Image>, 3> images{m.albedo, m.normal, m.specular};
    std::array<Texture*, 3> textures{&cache.textures.albedo, &cache.textures.normal, &cache.textures.specular};
    const std::array<Texture, 3> provided{supplied.albedo, supplied.normal, supplied.specular};
    for (std::size_t i = 0; i < 3; ++i) {
        if (provided[i]) {
            *textures[i] = provided[i];
            cache.images[i] = images[i];
            continue;
        }
        if (cache.images[i] != images[i] || !*textures[i]) {
            if (images[i] && images[i]->valid())
                *textures[i] = renderer.create_texture(images[i]->size, TextureFormat::Rgba8, images[i]->rgba.data());
            else {
                const u8 pixel[4] = {255, 255, 255, 255};
                *textures[i] = renderer.create_texture({1, 1}, TextureFormat::Rgba8, pixel);
            }
            cache.images[i] = images[i];
        }
        if (!*textures[i])
            return false;
    }
    if (!cache.lights.valid() || cache.fingerprint != fingerprint) {
        using V = std::array<f32, 4>;
        std::vector<V> data(std::max<std::size_t>(1, lights.size() * 6));
        std::map<const Image*, std::size_t> cookies;
        constexpr std::size_t max_vectors = 2 * 1024 * 1024; // 32 MiB, offsets remain exact as floats
        for (std::size_t i = 0; i < lights.size(); ++i) {
            const auto& l = lights[i];
            if (!std::isfinite(l.position.x) || !std::isfinite(l.position.y) || !std::isfinite(l.radius) ||
                l.radius <= 0 || !std::isfinite(l.height) || !std::isfinite(l.intensity) || l.intensity < 0 ||
                !std::isfinite(l.direction.x) || !std::isfinite(l.direction.y) || !std::isfinite(l.shadow_strength) ||
                !std::isfinite(l.shadow_softness) || l.shadow_softness < 0 || !std::isfinite(l.rotation))
                return false;
            const auto color = to_linear(l.color);
            const auto at = i * 6;
            const auto mask = static_cast<u32>(l.mask);
            data[at] = {l.position.x, l.position.y, l.height, l.radius};
            data[at + 1] = {color.r, color.g, color.b, l.intensity};
            data[at + 2] = {l.direction.x, l.direction.y, l.directional ? 1.0f : 0.0f, l.rotation * .01745329252f};
            data[at + 4][3] = static_cast<f32>(mask & 65535);
            data[at + 5][0] = static_cast<f32>(mask >> 16);
            const auto samples = surface_shadow_samples2d(l, occluders);
            if (samples.size() > max_vectors - data.size())
                return false;
            const auto headers = data.size();
            data.resize(headers + samples.size());
            data[at + 3] = {l.shadow_strength, static_cast<f32>(samples.size()), static_cast<f32>(headers), 0};
            for (std::size_t s = 0; s < samples.size(); ++s) {
                if (samples[s].size() > (max_vectors - data.size()) / 2)
                    return false;
                data[headers + s] = {static_cast<f32>(data.size()), static_cast<f32>(samples[s].size()), 0, 0};
                for (const auto& q : samples[s]) {
                    data.push_back({q[0].x, q[0].y, q[1].x, q[1].y});
                    data.push_back({q[2].x, q[2].y, q[3].x, q[3].y});
                }
            }
            if (!l.directional && l.shape && l.shape->valid()) {
                if (!cookies.contains(l.shape.get())) {
                    const auto count = (l.shape->rgba.size() / 4 + 3) / 4;
                    if (count > max_vectors - data.size())
                        return false;
                    const auto start = data.size();
                    cookies[l.shape.get()] = start;
                    data.resize(start + count);
                    for (std::size_t pixel = 0; pixel < l.shape->rgba.size() / 4; ++pixel)
                        data[start + pixel / 4][pixel % 4] = l.shape->rgba[pixel * 4 + 3] / 255.0f;
                }
                data[at + 3][3] = 1;
                data[at + 4][0] = static_cast<f32>(cookies.at(l.shape.get()));
                data[at + 4][1] = static_cast<f32>(l.shape->size.x);
                data[at + 4][2] = static_cast<f32>(l.shape->size.y);
            }
        }
        auto buffer = renderer.create_data_buffer(data.size() * sizeof(V), data.data());
        if (!buffer.valid())
            return false;
        cache.lights = std::move(buffer);
        cache.fingerprint = fingerprint;
    }
    const bool fx = (static_cast<int>(g.flip) & static_cast<int>(Flip::X)) != 0,
               fy = (static_cast<int>(g.flip) & static_cast<int>(Flip::Y)) != 0;
    const auto rotation = Affine2::rotation(g.rotation);
    const auto pivot = Vec2f{g.dest.x + g.dest.w * g.pivot.x, g.dest.y + g.dest.h * g.pivot.y};
    std::array<ShaderVertex, 4> vertices;
    constexpr Vec2f corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto p = corners[i];
        const auto q =
            rotation.apply_vector({g.dest.x + p.x * g.dest.w - pivot.x, g.dest.y + p.y * g.dest.h - pivot.y});
        const Vec2f world{q.x + pivot.x, q.y + pivot.y};
        if (!std::isfinite(world.x) || !std::isfinite(world.y))
            return false;
        vertices[i] = {.position = world,
                       .uv = {(source.x + (fx ? 1 - p.x : p.x) * source.w) / m.albedo->size.x,
                              (source.y + (fy ? 1 - p.y : p.y) * source.h) / m.albedo->size.y},
                       .custom = {world.x, world.y, 0, 0}};
    }
    ShaderParams params;
    params.uniforms.resize(24);
    const auto a = to_linear(ambient), t = to_linear(m.tint);
    const auto mask = static_cast<u32>(m.light_mask);
    params.uniforms = {a.r,
                       a.g,
                       a.b,
                       1,
                       t.r,
                       t.g,
                       t.b,
                       t.a,
                       m.normal_strength,
                       m.specular_strength,
                       m.shininess,
                       static_cast<f32>(lights.size()),
                       rotation.a,
                       rotation.b,
                       fx ? -1.0f : 1.0f,
                       fy ? -1.0f : 1.0f,
                       m.normal && m.normal->valid() ? 1.0f : 0.0f,
                       m.specular && m.specular->valid() ? 1.0f : 0.0f,
                       renderer.color_space() == ColorSpace::Linear ? 1.0f : 0.0f,
                       static_cast<f32>(mask & 65535),
                       static_cast<f32>(mask >> 16),
                       0,
                       0,
                       0};
    const std::array<u32, 6> indices{0, 1, 2, 0, 2, 3};
    const std::array<Texture, 3> sources{cache.textures.albedo, cache.textures.normal, cache.textures.specular};
    const std::array<DataBuffer, 1> buffers{cache.lights};
    renderer.draw_shader_geometry(vertices, indices, shader, params, sources, buffers);
    cache.used = true;
    cache.color_space = renderer.color_space();
    return true;
}
} // namespace kin
