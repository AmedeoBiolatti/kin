#include <kin/renderer/lighting.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kin {
namespace {

constexpr i32 falloff_size = 128;
constexpr f32 max_intensity = 4.0f;

bool reaches(const Light2D& light, Rectf area) {
    return light.position.x + light.radius > area.x && light.position.x - light.radius < area.x + area.w &&
           light.position.y + light.radius > area.y && light.position.y - light.radius < area.y + area.h;
}

} // namespace

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
                       f32 resolution) {
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
        for (const Light2D& light : lights) {
            if (light.radius <= 0.0f || light.intensity <= 0.0f || !reaches(light, area)) {
                continue;
            }
            const Texture& shape = light.shape.valid() ? light.shape : round;
            const Vec2i shape_size = shape.size();
            const Rectf source{0.0f, 0.0f, static_cast<f32>(shape_size.x), static_cast<f32>(shape_size.y)};
            const f32 extent = 2.0f * light.radius * resolution;
            const Rectf dest{(light.position.x - light.radius - area.x) * resolution,
                             (light.position.y - light.radius - area.y) * resolution, extent, extent};
            // Additive blending adds colour * alpha, so the tint's alpha carries the
            // intensity; above 1 the light is added again.
            for (f32 left = std::min(light.intensity, max_intensity); left > 0.0f; left -= 1.0f) {
                const u8 alpha = static_cast<u8>(std::lround(std::min(left, 1.0f) * 255.0f));
                const Color tint = Color::rgba(light.color.r, light.color.g, light.color.b, alpha);
                renderer.draw_texture(shape, source, dest, tint, light.rotation, {0.5f, 0.5f});
            }
        }
    }

    // Multiply the scene by it.
    const auto blend = renderer.scoped_blend_mode(BlendMode::Multiply);
    renderer.draw_texture(map.texture(), {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)}, area);
    return true;
}

} // namespace kin
