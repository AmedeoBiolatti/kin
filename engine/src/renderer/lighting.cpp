#include <kin/renderer/lighting.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kin {
namespace {

constexpr i32 falloff_size = 128;

bool reaches(const Light2D& light, Rectf area) {
    return light.position.x + light.radius > area.x && light.position.x - light.radius < area.x + area.w &&
           light.position.y + light.radius > area.y && light.position.y - light.radius < area.y + area.h;
}

// Twice the signed area: positive for a loop clockwise on a screen with y down.
f32 winding(std::span<const Vec2f> points) {
    f32 area = 0.0f;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec2f a = points[i];
        const Vec2f b = points[(i + 1) % points.size()];
        area += a.x * b.y - b.x * a.y;
    }
    return area;
}

Vec2f away(Vec2f from, Vec2f p, f32 reach) {
    const Vec2f d{p.x - from.x, p.y - from.y};
    const f32 length = std::sqrt(d.x * d.x + d.y * d.y);
    if (length <= 1e-6f) {
        return p;
    }
    return {p.x + d.x / length * reach, p.y + d.y / length * reach};
}

// The shadow of each edge of `occluder` facing away from a light (each edge,
// for an occluder not `lit`): `toward(p)` is the direction from `p` to the
// light, `beyond(p)` where the shadow of `p` ends.
template <typename Toward, typename Beyond>
void edge_shadows(const LightOccluder2D& occluder, Toward toward, Beyond beyond,
                  std::vector<std::array<Vec2f, 4>>& quads) {
    const std::vector<Vec2f>& p = occluder.points;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vec2f a = p[i];
        const Vec2f b = p[(i + 1) % p.size()];
        // Its outside is on the left going along it: facing away from the
        // light when the light is on its inside.
        const Vec2f outward{b.y - a.y, a.x - b.x};
        const Vec2f light = toward(a);
        const f32 facing = outward.x * light.x + outward.y * light.y;
        if (occluder.lit && facing >= 0.0f) {
            continue; // lit side: what is behind it is behind another edge
        }
        quads.push_back({a, b, beyond(b), beyond(a)});
    }
}

} // namespace

std::vector<Vec2f> clockwise(std::vector<Vec2f> points) {
    if (winding(points) < 0.0f) {
        std::reverse(points.begin(), points.end());
    }
    return points;
}

std::vector<std::array<Vec2f, 4>> shadow_quads(Vec2f light, f32 reach, std::span<const LightOccluder2D> occluders) {
    std::vector<std::array<Vec2f, 4>> quads;
    const f32 far = std::max(reach, 0.0f) * 2.0f + 1.0f; // past where the light reaches, from any point within it
    for (const LightOccluder2D& occluder : occluders) {
        const std::vector<Vec2f>& p = occluder.points;
        if (p.size() < 2) {
            continue;
        }
        // Not near enough to the light to matter.
        f32 x0 = p[0].x, y0 = p[0].y, x1 = p[0].x, y1 = p[0].y;
        for (const Vec2f q : p) {
            x0 = std::min(x0, q.x), y0 = std::min(y0, q.y), x1 = std::max(x1, q.x), y1 = std::max(y1, q.y);
        }
        if (x1 < light.x - reach || x0 > light.x + reach || y1 < light.y - reach || y0 > light.y + reach) {
            continue;
        }
        edge_shadows(
            occluder, [&](Vec2f at) { return Vec2f{light.x - at.x, light.y - at.y}; },
            [&](Vec2f at) { return away(light, at, far); }, quads);
    }
    return quads;
}

std::vector<std::array<Vec2f, 4>> directional_shadow_quads(Vec2f toward, f32 reach,
                                                           std::span<const LightOccluder2D> occluders) {
    std::vector<std::array<Vec2f, 4>> quads;
    const f32 length = std::sqrt(toward.x * toward.x + toward.y * toward.y);
    if (!(length > 1e-6f)) {
        return quads;
    }
    const Vec2f back{-toward.x / length * reach, -toward.y / length * reach};
    for (const LightOccluder2D& occluder : occluders) {
        if (occluder.points.size() < 2) {
            continue;
        }
        edge_shadows(
            occluder, [&](Vec2f) { return toward; }, [&](Vec2f at) { return Vec2f{at.x + back.x, at.y + back.y}; },
            quads);
    }
    return quads;
}

std::span<const Vec2f> shadow_sample_offsets(f32 softness) {
    static constexpr std::array<Vec2f, 1> centre{{{0.0f, 0.0f}}};
    static constexpr std::array<Vec2f, 4> around{{{-0.5f, 0.0f}, {0.5f, 0.0f}, {0.0f, -0.5f}, {0.0f, 0.5f}}};
    if (softness > 0.0f) {
        return around;
    }
    return centre;
}

const Texture& LightLayer::falloff(Renderer2D& renderer) {
    if (_falloff.valid()) {
        return _falloff;
    }
    std::vector<u8> pixels(static_cast<std::size_t>(falloff_size * falloff_size * 4));
    const f32 half = static_cast<f32>(falloff_size) * 0.5f;
    for (i32 y = 0; y < falloff_size; ++y) {
        for (i32 x = 0; x < falloff_size; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5f - half) / half;
            const f32 dy = (static_cast<f32>(y) + 0.5f - half) / half;
            const f32 d2 = std::min(1.0f, dx * dx + dy * dy);
            const f32 a = (1.0f - d2) * (1.0f - d2);
            u8* px = &pixels[static_cast<std::size_t>((y * falloff_size + x) * 4)];
            px[0] = px[1] = px[2] = 255;
            px[3] = static_cast<u8>(std::lround(a * 255.0f));
        }
    }
    _falloff = renderer.create_texture_from_rgba(pixels.data(), {falloff_size, falloff_size});
    if (_falloff.valid()) {
        renderer.set_scale_mode(_falloff, ScaleMode::Linear);
    }
    return _falloff;
}

bool LightLayer::apply(Renderer2D& renderer, Rectf area, Color ambient, std::span<const Light2D> lights,
                       f32 resolution, std::span<const LightOccluder2D> occluders) {
    const RendererBackendCapabilities caps = renderer.capabilities();
    if (!caps.render_targets || !caps.blend_modes || area.w <= 0.0f || area.h <= 0.0f || resolution <= 0.0f) {
        return false;
    }
    const Vec2i size{std::max(1, static_cast<i32>(std::lround(area.w * resolution))),
                     std::max(1, static_cast<i32>(std::lround(area.h * resolution)))};
    PooledTarget map = renderer.acquire_render_target(size, ScaleMode::Linear);
    const Texture& round = falloff(renderer);
    if (!map.valid() || !round.valid()) {
        return false;
    }

    // The light map: ambient everywhere, plus each light added on top.
    {
        const auto bind = renderer.scoped_render_target(map.target());
        renderer.clear(Color::rgb(ambient.r, ambient.g, ambient.b));
        const auto blend = renderer.scoped_blend_mode(BlendMode::Additive);
        // A light drawn at `dest`, `share` of it (additive blending adds colour *
        // alpha, so the tint's alpha carries the intensity; above 1 the light is
        // added again).
        const auto draw_light = [&](const Light2D& light, const Rectf& dest, f32 share) {
            const Texture& shape = light.shape.valid() ? light.shape : round;
            const Vec2i shape_size = shape.size();
            const Rectf source{0.0f, 0.0f, static_cast<f32>(shape_size.x), static_cast<f32>(shape_size.y)};
            for (f32 left = std::min(light.intensity, max_light_intensity) * share; left > 0.0f; left -= 1.0f) {
                const u8 alpha = static_cast<u8>(std::lround(std::min(left, 1.0f) * 255.0f));
                const Color tint = Color::rgba(light.color.r, light.color.g, light.color.b, alpha);
                renderer.draw_texture(shape, source, dest, tint, light.rotation, {0.5f, 0.5f});
            }
        };
        for (const Light2D& light : lights) {
            if (light.radius <= 0.0f || light.intensity <= 0.0f || !reaches(light, area)) {
                continue;
            }
            const f32 extent = 2.0f * light.radius * resolution;
            const Rectf dest{(light.position.x - light.radius - area.x) * resolution,
                             (light.position.y - light.radius - area.y) * resolution, extent, extent};
            const f32 strength = std::clamp(light.shadow_strength, 0.0f, 1.0f);
            if (!light.shadows || occluders.empty() || strength <= 0.0f) {
                draw_light(light, dest, 1.0f);
                continue;
            }
            // What its shadows leave of it.
            if (strength < 1.0f) {
                draw_light(light, dest, 1.0f - strength);
            }
            // The rest: drawn into a target of its own, its occluders' shadows
            // filled black there, added to the map. A light with a size: four
            // shadows from around it, each a quarter (soft edges).
            const f32 softness = std::max(light.shadow_softness, 0.0f);
            const std::span<const Vec2f> offsets = shadow_sample_offsets(softness);
            const f32 share = strength / static_cast<f32>(offsets.size());
            const i32 side = std::max(1, static_cast<i32>(std::ceil(extent)));
            const Vec2f corner{light.position.x - light.radius, light.position.y - light.radius};
            for (const Vec2f offset : offsets) {
                const Vec2f from{light.position.x + offset.x * softness, light.position.y + offset.y * softness};
                const std::vector<std::array<Vec2f, 4>> quads = shadow_quads(from, light.radius + softness, occluders);
                PooledTarget scratch = quads.empty() ? PooledTarget{} : renderer.acquire_render_target({side, side}, ScaleMode::Linear);
                if (!scratch.valid()) {
                    draw_light(light, dest, share); // nothing in its way (or no target to cut it in)
                    continue;
                }
                {
                    const auto bind = renderer.scoped_render_target(scratch.target());
                    renderer.clear(Color::rgb(0, 0, 0)); // opaque: additive blending keeps it so
                    draw_light(light, {0.0f, 0.0f, extent, extent}, 1.0f);
                    const auto cut = renderer.scoped_blend_mode(BlendMode::Replace);
                    std::array<Vec2f, 4> points{};
                    for (const std::array<Vec2f, 4>& quad : quads) {
                        for (std::size_t i = 0; i < 4; ++i) {
                            points[i] = {(quad[i].x - corner.x) * resolution, (quad[i].y - corner.y) * resolution};
                        }
                        renderer.fill_polygon(points, Color::rgb(0, 0, 0));
                    }
                }
                // Added to the map (the outer additive blending), its share of the light.
                const u8 alpha = static_cast<u8>(std::lround(std::clamp(share, 0.0f, 1.0f) * 255.0f));
                renderer.draw_texture(scratch.texture(), {0.0f, 0.0f, static_cast<f32>(side), static_cast<f32>(side)},
                                      {dest.x, dest.y, static_cast<f32>(side), static_cast<f32>(side)}, Color::rgba(255, 255, 255, alpha));
            }
        }
    }

    // Multiply the scene by it.
    const auto blend = renderer.scoped_blend_mode(BlendMode::Multiply);
    renderer.draw_texture(map.texture(), {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)}, area);
    return true;
}

} // namespace kin
