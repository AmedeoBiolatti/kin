#include <kin/renderer/normal_lighting2d.hpp>

#include "normal_lighting2d_internal.hpp"

#include <algorithm>
#include <cmath>

namespace kin {
namespace normal_lighting2d_detail {
namespace {

bool finite(f32 value) {
    return std::isfinite(value);
}

bool finite(Vec2f value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(Rectf rect) {
    return finite(rect.x) && finite(rect.y) && finite(rect.w) && finite(rect.h);
}

bool usable(const SurfaceLight2D& light) {
    return finite(light.position) && finite(light.height) && finite(light.radius) && light.radius > 0 &&
           finite(light.intensity) && light.intensity >= 0 && finite(light.direction) &&
           finite(light.shadow_strength) && finite(light.shadow_softness) && finite(light.rotation);
}

} // namespace

std::optional<Rectf> surface_source(const SurfaceMaterial2D& material, const SurfaceGeometry2D& geometry) {
    if (!material.albedo || !material.albedo->valid() || !finite(material.normal_strength) ||
        material.normal_strength < 0 || !finite(material.specular_strength) || material.specular_strength < 0 ||
        !finite(material.shininess) || material.shininess <= 0) {
        return std::nullopt;
    }
    if (!finite(geometry.dest) || geometry.dest.w <= 0 || geometry.dest.h <= 0 || !finite(geometry.rotation) ||
        !finite(geometry.pivot)) {
        return std::nullopt;
    }
    const Vec2f image{static_cast<f32>(material.albedo->size.x), static_cast<f32>(material.albedo->size.y)};
    Rectf source = geometry.source;
    if (source.w <= 0 || source.h <= 0) {
        source = {0, 0, image.x, image.y};
    }
    if (!finite(source) || source.x < 0 || source.y < 0 || source.w < 1 || source.h < 1 ||
        source.x + source.w > image.x || source.y + source.h > image.y) {
        return std::nullopt;
    }
    return source;
}

std::vector<const SurfaceLight2D*> usable_lights(std::span<const SurfaceLight2D> lights) {
    std::vector<const SurfaceLight2D*> out;
    for (const SurfaceLight2D& light : lights) {
        if (out.size() == max_surface_lights) {
            break;
        }
        if (usable(light)) {
            out.push_back(&light);
        }
    }
    return out;
}

Vec2f surface_point(const SurfaceGeometry2D& geometry, Vec2f at) {
    const Rectf& dest = geometry.dest;
    const Vec2f pivot{dest.x + dest.w * geometry.pivot.x, dest.y + dest.h * geometry.pivot.y};
    const Vec2f turned = Affine2::rotation(geometry.rotation)
                             .apply_vector({dest.x + at.x * dest.w - pivot.x, dest.y + at.y * dest.h - pivot.y});
    return {turned.x + pivot.x, turned.y + pivot.y};
}

std::vector<std::vector<std::array<Vec2f, 4>>> shadow_samples(const SurfaceLight2D& light,
                                                              std::span<const LightOccluder2D> occluders) {
    std::vector<std::vector<std::array<Vec2f, 4>>> samples;
    if (!light.shadows || light.shadow_strength <= 0 || occluders.empty()) {
        return samples;
    }
    // As a light map's: the same sample points, sharp below no softness.
    const f32 softness = std::max(light.shadow_softness, 0.0f);
    for (const Vec2f offset : shadow_sample_offsets(softness)) {
        if (light.directional) {
            // A sun's size turns its direction a little either way.
            constexpr f32 spread = 0.001f;
            constexpr f32 reach = 1e6f;
            const Vec2f toward{light.direction.x + offset.x * softness * spread,
                               light.direction.y + offset.y * softness * spread};
            samples.push_back(directional_shadow_quads(toward, reach, occluders));
        } else {
            const Vec2f from{light.position.x + offset.x * softness, light.position.y + offset.y * softness};
            samples.push_back(shadow_quads(from, light.radius + softness, occluders));
        }
    }
    return samples;
}

} // namespace normal_lighting2d_detail

namespace {

using namespace normal_lighting2d_detail;

// The CPU path's largest sprite, in source pixels.
constexpr f64 max_shaded_pixels = 4194304;

struct Vec3 {
    f32 x = 0;
    f32 y = 0;
    f32 z = 1;
};

f32 dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// `v` at length 1; straight up when it has none.
Vec3 unit(Vec3 v) {
    const f64 length = std::hypot(static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z));
    if (length <= 1e-8) {
        return {};
    }
    return {static_cast<f32>(v.x / length), static_cast<f32>(v.y / length), static_cast<f32>(v.z / length)};
}

// The nearest pixel to (u, v), each 0..1 across the image.
Color sample(const Image& image, f32 u, f32 v) {
    const i32 x = std::clamp(static_cast<i32>(u * image.size.x), 0, image.size.x - 1);
    const i32 y = std::clamp(static_cast<i32>(v * image.size.y), 0, image.size.y - 1);
    const std::size_t offset = (static_cast<std::size_t>(y) * image.size.x + x) * 4;
    return {image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2], image.rgba[offset + 3]};
}

// Even-odd: whether `p` is inside the quad.
bool inside(Vec2f p, const std::array<Vec2f, 4>& quad) {
    bool in = false;
    for (std::size_t i = 0, j = 3; i < 4; j = i++) {
        const Vec2f a = quad[i];
        const Vec2f b = quad[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
            in = !in;
        }
    }
    return in;
}

struct ShadedLight {
    const SurfaceLight2D* light = nullptr;
    LinearColor color;
    std::vector<std::vector<std::array<Vec2f, 4>>> shadows;
};

} // namespace

Image shade_normal_sprite2d(const SurfaceMaterial2D& material, const SurfaceGeometry2D& geometry, Color ambient,
                            std::span<const SurfaceLight2D> lights, std::span<const LightOccluder2D> occluders) {
    const std::optional<Rectf> found = surface_source(material, geometry);
    if (!found || static_cast<f64>(found->w) * found->h > max_shaded_pixels) {
        return {};
    }
    const Rectf source = *found;
    const Vec2i size{static_cast<i32>(source.w), static_cast<i32>(source.h)};
    Image output{size, std::vector<u8>(static_cast<std::size_t>(size.x) * size.y * 4)};
    const Affine2 rotate = Affine2::rotation(geometry.rotation);
    const bool flip_x = flips_x(geometry.flip);
    const bool flip_y = flips_y(geometry.flip);
    const LinearColor ambient_light = to_linear(ambient);
    const LinearColor tint = to_linear(material.tint);
    std::vector<ShadedLight> shading;
    for (const SurfaceLight2D* light : usable_lights(lights)) {
        shading.push_back({light, to_linear(light->color), shadow_samples(*light, occluders)});
    }
    for (i32 y = 0; y < size.y; ++y) {
        for (i32 x = 0; x < size.x; ++x) {
            const f32 u = (x + .5f) / size.x;
            const f32 v = (y + .5f) / size.y;
            const f32 tex_u = (source.x + u * source.w) / material.albedo->size.x;
            const f32 tex_v = (source.y + v * source.h) / material.albedo->size.y;
            const LinearColor albedo = to_linear(sample(*material.albedo, tex_u, tex_v));
            const std::size_t offset = (static_cast<std::size_t>(y) * size.x + x) * 4;
            if (albedo.a == 0) {
                continue;
            }
            Vec3 normal{};
            if (material.normal && material.normal->valid()) {
                const Color n = sample(*material.normal, tex_u, tex_v);
                normal = {((n.r / 255.0f) * 2 - 1) * material.normal_strength,
                          ((n.g / 255.0f) * 2 - 1) * material.normal_strength, (n.b / 255.0f) * 2 - 1};
            }
            normal.x *= flip_x ? -1.0f : 1.0f;
            normal.y *= flip_y ? -1.0f : 1.0f;
            normal = unit(normal);
            const Vec2f turned = rotate.apply_vector({normal.x, normal.y});
            normal = unit({turned.x, turned.y, normal.z});
            const Vec2f world = surface_point(geometry, {flip_x ? 1 - u : u, flip_y ? 1 - v : v});
            if (!finite(world)) {
                return {};
            }
            const f32 specular = material.specular_strength * (material.specular && material.specular->valid()
                                                                   ? sample(*material.specular, tex_u, tex_v).r / 255.0f
                                                                   : 1.0f);
            f32 red = ambient_light.r * albedo.r * tint.r;
            f32 green = ambient_light.g * albedo.g * tint.g;
            f32 blue = ambient_light.b * albedo.b * tint.b;
            for (const ShadedLight& shaded : shading) {
                const SurfaceLight2D& light = *shaded.light;
                if (!(static_cast<u32>(light.mask) & static_cast<u32>(material.light_mask)) || light.intensity == 0) {
                    continue;
                }
                const f32 dx = light.directional ? 0 : light.position.x - world.x;
                const f32 dy = light.directional ? 0 : light.position.y - world.y;
                const f64 d2 = (static_cast<f64>(dx) * dx + static_cast<f64>(dy) * dy) /
                               (static_cast<f64>(light.radius) * light.radius);
                if (!std::isfinite(d2)) {
                    continue;
                }
                const bool shaped = !light.directional && light.shape && light.shape->valid();
                if (!light.directional && !shaped && d2 >= 1) {
                    continue;
                }
                f32 attenuation = light.directional || shaped ? 1 : static_cast<f32>((1 - d2) * (1 - d2));
                if (shaped) {
                    const Vec2f q = Affine2::rotation(-light.rotation).apply_vector({-dx, -dy});
                    if (!finite(q) || std::abs(q.x) >= light.radius || std::abs(q.y) >= light.radius) {
                        continue;
                    }
                    attenuation =
                        sample(*light.shape, q.x / (2 * light.radius) + .5f, q.y / (2 * light.radius) + .5f).a / 255.0f;
                }
                const Vec3 direction = light.directional ? unit({light.direction.x, light.direction.y, light.height})
                                                         : unit({dx, dy, light.height});
                if (!shaded.shadows.empty()) {
                    f32 blocked = 0;
                    for (const auto& quads : shaded.shadows) {
                        if (std::ranges::any_of(quads, [&](const auto& quad) { return inside(world, quad); })) {
                            blocked += 1.0f / shaded.shadows.size();
                        }
                    }
                    attenuation *= 1 - blocked * std::clamp(light.shadow_strength, 0.0f, 1.0f);
                }
                const f32 diffuse = std::max(0.0f, dot(normal, direction));
                const Vec3 halfway = unit({direction.x, direction.y, direction.z + 1});
                const f32 highlight =
                    diffuse > 0 ? specular * std::pow(std::max(0.0f, dot(normal, halfway)), material.shininess) : 0;
                const f32 power = attenuation * std::min(light.intensity, max_light_intensity);
                red += power * shaded.color.r * (albedo.r * tint.r * diffuse + highlight);
                green += power * shaded.color.g * (albedo.g * tint.g * diffuse + highlight);
                blue += power * shaded.color.b * (albedo.b * tint.b * diffuse + highlight);
            }
            const Color c = to_srgb({red, green, blue, albedo.a * tint.a});
            output.rgba[offset] = c.r;
            output.rgba[offset + 1] = c.g;
            output.rgba[offset + 2] = c.b;
            output.rgba[offset + 3] = c.a;
        }
    }
    return output;
}

} // namespace kin
