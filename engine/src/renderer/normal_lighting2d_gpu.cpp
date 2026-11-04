#include <kin/renderer/normal_lighting2d.hpp>

#include "normal_lighting2d_internal.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace kin {
namespace {

using namespace normal_lighting2d_detail;
using Vec4 = std::array<f32, 4>;

// Per light, in the buffer: position, height, radius | colour, intensity |
// direction, directional, rotation | shadow strength, samples, first sample
// header, shaped | shape offset, width, height, mask (low 16 bits) | mask
// (high 16 bits). The shader reads it so (normal_sprite.frag.glsl).
constexpr std::size_t vectors_per_light = 6;
// 32 MiB, so offsets stay exact as floats.
constexpr std::size_t max_vectors = 2 * 1024 * 1024;
constexpr f32 radians_per_degree = 0.01745329252f;

} // namespace

bool normal_lighting2d_gpu_supported(const RendererBackendCapabilities& capabilities) {
    return capabilities.shader_geometry && capabilities.data_buffers && capabilities.data_textures;
}

SurfaceLightBuffer2D pack_surface_lights2d(Renderer2D& renderer, std::span<const SurfaceLight2D> lights,
                                           std::span<const LightOccluder2D> occluders) {
    SurfaceLightBuffer2D packed{.renderer = renderer.id()};
    if (!normal_lighting2d_gpu_supported(renderer.capabilities())) {
        return packed;
    }
    const std::vector<const SurfaceLight2D*> usable = usable_lights(lights);
    std::vector<Vec4> data(std::max<std::size_t>(1, usable.size() * vectors_per_light));
    std::map<const Image*, std::size_t> shapes; // where each shape's alpha is, shared by its lights
    for (std::size_t i = 0; i < usable.size(); ++i) {
        const SurfaceLight2D& light = *usable[i];
        const LinearColor color = to_linear(light.color);
        const std::size_t at = i * vectors_per_light;
        const u32 mask = static_cast<u32>(light.mask);
        data[at] = {light.position.x, light.position.y, light.height, light.radius};
        data[at + 1] = {color.r, color.g, color.b, std::min(light.intensity, max_light_intensity)};
        data[at + 2] = {light.direction.x, light.direction.y, light.directional ? 1.0f : 0.0f,
                        light.rotation * radians_per_degree};
        data[at + 4][3] = static_cast<f32>(mask & 65535);
        data[at + 5][0] = static_cast<f32>(mask >> 16);
        const auto samples = shadow_samples(light, occluders);
        if (samples.size() > max_vectors - data.size()) {
            return packed;
        }
        const std::size_t headers = data.size();
        data.resize(headers + samples.size());
        data[at + 3] = {light.shadow_strength, static_cast<f32>(samples.size()), static_cast<f32>(headers), 0};
        for (std::size_t s = 0; s < samples.size(); ++s) {
            if (samples[s].size() > (max_vectors - data.size()) / 2) {
                return packed;
            }
            data[headers + s] = {static_cast<f32>(data.size()), static_cast<f32>(samples[s].size()), 0, 0};
            for (const auto& quad : samples[s]) {
                data.push_back({quad[0].x, quad[0].y, quad[1].x, quad[1].y});
                data.push_back({quad[2].x, quad[2].y, quad[3].x, quad[3].y});
            }
        }
        if (!light.directional && light.shape && light.shape->valid()) {
            if (!shapes.contains(light.shape.get())) {
                const std::size_t pixels = light.shape->rgba.size() / 4;
                const std::size_t count = (pixels + 3) / 4;
                if (count > max_vectors - data.size()) {
                    return packed;
                }
                const std::size_t start = data.size();
                shapes[light.shape.get()] = start;
                data.resize(start + count);
                for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
                    data[start + pixel / 4][pixel % 4] = light.shape->rgba[pixel * 4 + 3] / 255.0f;
                }
            }
            data[at + 3][3] = 1;
            data[at + 4][0] = static_cast<f32>(shapes.at(light.shape.get()));
            data[at + 4][1] = static_cast<f32>(light.shape->size.x);
            data[at + 4][2] = static_cast<f32>(light.shape->size.y);
        }
    }
    packed.buffer = renderer.create_data_buffer(data.size() * sizeof(Vec4), data.data());
    packed.lights = packed.buffer.valid() ? static_cast<u32>(usable.size()) : 0;
    return packed;
}

bool draw_normal_sprite2d_gpu(Renderer2D& renderer, const SurfaceMaterial2D& material,
                              const SurfaceGeometry2D& geometry, Color ambient, const SurfaceLightBuffer2D& lights,
                              NormalLightingGpuCache2D& cache, SurfaceGpuTextures2D supplied) {
    cache.used = false;
    if (!normal_lighting2d_gpu_supported(renderer.capabilities()) || !lights.buffer.valid() ||
        lights.renderer != renderer.id()) {
        return false;
    }
    const ShaderHandle shader = renderer.builtin_shader(BuiltinShader::NormalSprite2D);
    const std::optional<Rectf> found = surface_source(material, geometry);
    if (!shader || !found) {
        return false;
    }
    const Rectf source = *found;
    if (cache.renderer != renderer.id()) {
        cache = {};
        cache.renderer = renderer.id();
    }
    // The material's textures: those supplied, else made from its images (white for none).
    const std::array<std::shared_ptr<const Image>, 3> images{material.albedo, material.normal, material.specular};
    const std::array<Texture*, 3> textures{&cache.textures.albedo, &cache.textures.normal, &cache.textures.specular};
    const std::array<Texture, 3> provided{supplied.albedo, supplied.normal, supplied.specular};
    for (std::size_t i = 0; i < 3; ++i) {
        if (provided[i]) {
            *textures[i] = provided[i];
            cache.images[i] = images[i];
            continue;
        }
        if (cache.images[i] != images[i] || !*textures[i]) {
            if (images[i] && images[i]->valid()) {
                *textures[i] = renderer.create_texture(images[i]->size, TextureFormat::Rgba8, images[i]->rgba.data());
            } else {
                const u8 white[4] = {255, 255, 255, 255};
                *textures[i] = renderer.create_texture({1, 1}, TextureFormat::Rgba8, white);
            }
            cache.images[i] = images[i];
        }
        if (!*textures[i]) {
            return false;
        }
    }
    const bool flip_x = flips_x(geometry.flip);
    const bool flip_y = flips_y(geometry.flip);
    std::array<ShaderVertex, 4> vertices;
    constexpr Vec2f corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const Vec2f image{static_cast<f32>(material.albedo->size.x), static_cast<f32>(material.albedo->size.y)};
    for (std::size_t i = 0; i < 4; ++i) {
        const Vec2f corner = corners[i];
        const Vec2f world = surface_point(geometry, corner);
        if (!std::isfinite(world.x) || !std::isfinite(world.y)) {
            return false;
        }
        vertices[i] = {.position = world,
                       .uv = {(source.x + (flip_x ? 1 - corner.x : corner.x) * source.w) / image.x,
                              (source.y + (flip_y ? 1 - corner.y : corner.y) * source.h) / image.y},
                       .custom = {world.x, world.y, 0, 0}};
    }
    const Affine2 rotation = Affine2::rotation(geometry.rotation);
    const LinearColor ambient_light = to_linear(ambient);
    const LinearColor tint = to_linear(material.tint);
    const u32 mask = static_cast<u32>(material.light_mask);
    ShaderParams params;
    // As normal_sprite.frag.glsl's Material block.
    params.uniforms = {ambient_light.r,
                       ambient_light.g,
                       ambient_light.b,
                       1,
                       tint.r,
                       tint.g,
                       tint.b,
                       tint.a,
                       material.normal_strength,
                       material.specular_strength,
                       material.shininess,
                       static_cast<f32>(lights.lights),
                       rotation.a,
                       rotation.b,
                       flip_x ? -1.0f : 1.0f,
                       flip_y ? -1.0f : 1.0f,
                       material.normal && material.normal->valid() ? 1.0f : 0.0f,
                       material.specular && material.specular->valid() ? 1.0f : 0.0f,
                       renderer.color_space() == ColorSpace::Linear ? 1.0f : 0.0f,
                       static_cast<f32>(mask & 65535),
                       static_cast<f32>(mask >> 16),
                       0,
                       0,
                       0};
    const std::array<u32, 6> indices{0, 1, 2, 0, 2, 3};
    const std::array<Texture, 3> sources{cache.textures.albedo, cache.textures.normal, cache.textures.specular};
    const std::array<DataBuffer, 1> buffers{lights.buffer};
    renderer.draw_shader_geometry(vertices, indices, shader, params, sources, buffers);
    cache.used = true;
    return true;
}

} // namespace kin
