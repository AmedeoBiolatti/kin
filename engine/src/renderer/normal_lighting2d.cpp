#include <algorithm>
#include <cmath>
#include <kin/renderer/normal_lighting2d.hpp>

namespace kin {
namespace {
struct V3 {
    f32 x = 0, y = 0, z = 1;
};
f32 dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
V3 unit(V3 v) {
    const auto d = std::hypot(static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z));
    return d > 1e-8 ? V3{static_cast<f32>(v.x / d), static_cast<f32>(v.y / d), static_cast<f32>(v.z / d)} : V3{};
}
Color sample(const Image& image, f32 u, f32 v) {
    const auto x = std::clamp(static_cast<i32>(u * image.size.x), 0, image.size.x - 1);
    const auto y = std::clamp(static_cast<i32>(v * image.size.y), 0, image.size.y - 1);
    const auto offset = (static_cast<std::size_t>(y) * image.size.x + x) * 4;
    return {image.rgba[offset], image.rgba[offset + 1], image.rgba[offset + 2], image.rgba[offset + 3]};
}
bool inside(Vec2f p, const std::array<Vec2f, 4>& quad) {
    bool in = false;
    for (std::size_t i = 0, j = 3; i < 4; j = i++) {
        const auto a = quad[i], b = quad[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}
struct Shadows {
    std::vector<std::vector<std::array<Vec2f, 4>>> samples;
};
Shadows shadows(const SurfaceLight2D& l, std::span<const LightOccluder2D> occluders) {
    Shadows out;
    if (!l.shadows || l.shadow_strength <= 0 || occluders.empty())
        return out;
    constexpr Vec2f jitter[] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    const int count = l.shadow_softness > 0 ? 4 : 1;
    for (int i = 0; i < count; ++i) {
        const auto offset =
            count == 1 ? Vec2f{} : Vec2f{jitter[i].x * l.shadow_softness * .5f, jitter[i].y * l.shadow_softness * .5f};
        const auto from = Vec2f{l.position.x + offset.x, l.position.y + offset.y};
        if (!l.directional)
            out.samples.push_back(shadow_quads(from, l.radius + l.shadow_softness, occluders));
        else {
            std::vector<std::array<Vec2f, 4>> quads;
            const auto direction = unit({l.direction.x + offset.x * .001f, l.direction.y + offset.y * .001f, l.height});
            for (const auto& o : occluders)
                for (std::size_t j = 0; j < o.points.size(); ++j) {
                    const auto a = o.points[j], b = o.points[(j + 1) % o.points.size()];
                    const auto facing = (b.y - a.y) * direction.x + (a.x - b.x) * direction.y;
                    if (o.lit && facing >= 0)
                        continue;
                    constexpr f32 reach = 1e6f;
                    quads.push_back({a, b, Vec2f{b.x - direction.x * reach, b.y - direction.y * reach},
                                     Vec2f{a.x - direction.x * reach, a.y - direction.y * reach}});
                }
            out.samples.push_back(std::move(quads));
        }
    }
    return out;
}
bool finite(f32 v) {
    return std::isfinite(v);
}
} // namespace
std::vector<std::vector<std::array<Vec2f, 4>>> surface_shadow_samples2d(const SurfaceLight2D& light,
                                                                        std::span<const LightOccluder2D> occluders) {
    return shadows(light, occluders).samples;
}
Image shade_normal_sprite2d(const SurfaceMaterial2D& m, const SurfaceGeometry2D& g, Color ambient,
                            std::span<const SurfaceLight2D> lights, std::span<const LightOccluder2D> occluders) {
    if (!m.albedo || !m.albedo->valid() || lights.size() > 256 || !finite(m.normal_strength) || m.normal_strength < 0 ||
        !finite(m.specular_strength) || m.specular_strength < 0 || !finite(m.shininess) || m.shininess <= 0 ||
        !finite(g.dest.x) || !finite(g.dest.y) || !finite(g.dest.w) || !finite(g.dest.h) || g.dest.w <= 0 ||
        g.dest.h <= 0 || !finite(g.rotation) || !finite(g.pivot.x) || !finite(g.pivot.y))
        return {};
    auto source = g.source;
    if (source.w <= 0 || source.h <= 0)
        source = {0, 0, static_cast<f32>(m.albedo->size.x), static_cast<f32>(m.albedo->size.y)};
    if (!finite(source.x) || !finite(source.y) || !finite(source.w) || !finite(source.h) || source.x < 0 ||
        source.y < 0 || source.x + source.w > m.albedo->size.x || source.y + source.h > m.albedo->size.y ||
        source.w < 1 || source.h < 1 || static_cast<f64>(source.w) * source.h > 4194304)
        return {};
    const Vec2i size{static_cast<i32>(source.w), static_cast<i32>(source.h)};
    Image output{size, std::vector<u8>(static_cast<std::size_t>(size.x) * size.y * 4)};
    const auto rotate = Affine2::rotation(g.rotation);
    const Vec2f pivot{g.dest.x + g.dest.w * g.pivot.x, g.dest.y + g.dest.h * g.pivot.y};
    const bool flip_x = (static_cast<int>(g.flip) & static_cast<int>(Flip::X)) != 0;
    const bool flip_y = (static_cast<int>(g.flip) & static_cast<int>(Flip::Y)) != 0;
    const auto a = to_linear(ambient), tint = to_linear(m.tint);
    std::vector<Shadows> shadow_maps;
    shadow_maps.reserve(lights.size());
    for (const auto& light : lights) {
        if (!finite(light.position.x) || !finite(light.position.y) || !finite(light.height) || !finite(light.radius) ||
            light.radius <= 0 || !finite(light.intensity) || light.intensity < 0 || !finite(light.direction.x) ||
            !finite(light.direction.y) || !finite(light.shadow_strength) || !finite(light.shadow_softness) ||
            light.shadow_softness < 0 || !finite(light.rotation))
            return {};
        shadow_maps.push_back(shadows(light, occluders));
    }
    for (i32 y = 0; y < size.y; ++y)
        for (i32 x = 0; x < size.x; ++x) {
            const f32 u = (x + .5f) / size.x, v = (y + .5f) / size.y;
            const f32 tex_u = (source.x + u * source.w) / m.albedo->size.x,
                      tex_v = (source.y + v * source.h) / m.albedo->size.y;
            const auto albedo = to_linear(sample(*m.albedo, tex_u, tex_v));
            const auto offset = (static_cast<std::size_t>(y) * size.x + x) * 4;
            if (albedo.a == 0)
                continue;
            V3 normal{};
            if (m.normal && m.normal->valid()) {
                const auto n = sample(*m.normal, tex_u, tex_v);
                normal = {((n.r / 255.0f) * 2 - 1) * m.normal_strength, ((n.g / 255.0f) * 2 - 1) * m.normal_strength,
                          (n.b / 255.0f) * 2 - 1};
            }
            normal.x *= flip_x ? -1.0f : 1.0f;
            normal.y *= flip_y ? -1.0f : 1.0f;
            normal = unit(normal);
            const auto xy = rotate.apply_vector({normal.x, normal.y});
            normal = unit({xy.x, xy.y, normal.z});
            const auto p = rotate.apply_vector({g.dest.x + (flip_x ? 1 - u : u) * g.dest.w - pivot.x,
                                                g.dest.y + (flip_y ? 1 - v : v) * g.dest.h - pivot.y});
            const Vec2f world{p.x + pivot.x, p.y + pivot.y};
            if (!finite(world.x) || !finite(world.y))
                return {};
            const f32 specular =
                m.specular_strength *
                (m.specular && m.specular->valid() ? sample(*m.specular, tex_u, tex_v).r / 255.0f : 1.0f);
            f32 r = a.r * albedo.r * tint.r, green = a.g * albedo.g * tint.g, b = a.b * albedo.b * tint.b;
            for (std::size_t index = 0; index < lights.size(); ++index) {
                const auto& l = lights[index];
                if (!(static_cast<u32>(l.mask) & static_cast<u32>(m.light_mask)) || l.intensity == 0)
                    continue;
                const f32 dx = l.directional ? 0 : l.position.x - world.x,
                          dy = l.directional ? 0 : l.position.y - world.y;
                if (!finite(dx) || !finite(dy))
                    return {};
                const f64 d2 =
                    (static_cast<f64>(dx) * dx + static_cast<f64>(dy) * dy) / (static_cast<f64>(l.radius) * l.radius);
                const bool shaped = !l.directional && l.shape && l.shape->valid();
                if (!l.directional && !shaped && d2 >= 1)
                    continue;
                f32 attenuation = l.directional || shaped ? 1 : static_cast<f32>((1 - d2) * (1 - d2));
                if (shaped) {
                    const auto q = Affine2::rotation(-l.rotation).apply_vector({-dx, -dy});
                    if (!finite(q.x) || !finite(q.y) || std::abs(q.x) >= l.radius || std::abs(q.y) >= l.radius)
                        continue;
                    attenuation = sample(*l.shape, q.x / (2 * l.radius) + .5f, q.y / (2 * l.radius) + .5f).a / 255.0f;
                }
                const auto direction =
                    l.directional ? unit({l.direction.x, l.direction.y, l.height}) : unit({dx, dy, l.height});
                const auto& sm = shadow_maps[index].samples;
                if (!sm.empty()) {
                    f32 blocked = 0;
                    for (const auto& quads : sm)
                        if (std::ranges::any_of(quads, [&](const auto& q) { return inside(world, q); }))
                            blocked += 1.0f / sm.size();
                    attenuation *= 1 - blocked * std::clamp(l.shadow_strength, 0.0f, 1.0f);
                }
                const auto color = to_linear(l.color);
                const f32 diffuse = std::max(0.0f, dot(normal, direction));
                const auto halfway = unit({direction.x, direction.y, direction.z + 1});
                const f32 spec =
                    diffuse > 0 ? specular * std::pow(std::max(0.0f, dot(normal, halfway)), m.shininess) : 0;
                const f32 power = attenuation * std::min(l.intensity, 16.0f);
                r += power * color.r * (albedo.r * tint.r * diffuse + spec);
                green += power * color.g * (albedo.g * tint.g * diffuse + spec);
                b += power * color.b * (albedo.b * tint.b * diffuse + spec);
            }
            const auto c = to_srgb({r, green, b, albedo.a * tint.a});
            output.rgba[offset] = c.r;
            output.rgba[offset + 1] = c.g;
            output.rgba[offset + 2] = c.b;
            output.rgba[offset + 3] = c.a;
        }
    return output;
}
} // namespace kin
