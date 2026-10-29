#include <kin/ecs/lighting.hpp>

#include <kin/ecs/render.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <cmath>

namespace kin {

std::string_view tonemap_name(Tonemap tonemap) {
    switch (tonemap) {
    case Tonemap::None: return "none";
    case Tonemap::Reinhard: return "reinhard";
    case Tonemap::Aces: return "aces";
    }
    return "none";
}

std::optional<Tonemap> tonemap_named(std::string_view name) {
    for (const Tonemap tonemap : {Tonemap::None, Tonemap::Reinhard, Tonemap::Aces}) {
        if (tonemap_name(tonemap) == name) {
            return tonemap;
        }
    }
    return std::nullopt;
}

const SceneLighting* scene_lighting(EcsWorld& world) {
    const SceneLighting* found = nullptr;
    world.raw().each([&](const SceneLighting& lighting) {
        if (!found && lighting.enabled) {
            found = &lighting;
        }
    });
    return found;
}

std::vector<Light2D> scene_lights(EcsWorld& world, const Affine2& to_screen) {
    constexpr f32 degrees = 180.0f / 3.14159265358979323846f;
    // How the screen scales and turns the world.
    const f32 view_scale = std::sqrt(to_screen.a * to_screen.a + to_screen.b * to_screen.b);
    const f32 view_turn = std::atan2(to_screen.b, to_screen.a) * degrees;
    std::vector<Light2D> lights;
    world.raw().each([&](flecs::entity entity, const LightComponent& light) {
        if (!light.enabled || light.radius <= 0.0f || light.intensity <= 0.0f) {
            return;
        }
        const WorldTransform placed = current_world_transform(entity);
        const f32 scale = std::max(std::abs(placed.scale.x), std::abs(placed.scale.y));
        lights.push_back({
            .position = to_screen.apply(placed.pos),
            .radius = light.radius * scale * view_scale,
            .color = light.color,
            .intensity = light.intensity,
            .shape = light.shape,
            .rotation = placed.rotation + light.rotation + view_turn,
        });
    });
    return lights;
}

bool apply_scene_lighting(Renderer2D& renderer, LightLayer& layer, EcsWorld& world, const Affine2& to_screen, Rectf area) {
    const SceneLighting* lighting = scene_lighting(world);
    if (!lighting) {
        return false;
    }
    const std::vector<Light2D> lights = scene_lights(world, to_screen);
    return layer.apply(renderer, area, lighting->ambient, lights, std::clamp(lighting->resolution, 0.05f, 1.0f));
}

std::optional<ColorOutput> scene_color_output(EcsWorld& world) {
    std::optional<ColorOutput> output;
    world.raw().each([&](const ColorGrade& grade) {
        if (output || !grade.enabled) {
            return;
        }
        output = ColorOutput{
            .exposure = grade.exposure,
            .tonemap = grade.tonemap,
            .lut = grade.lut,
            .lut_to = nullptr,
            .lut_mix = 0.0f,
            .lut_strength = std::clamp(grade.strength, 0.0f, 1.0f),
            .dither = grade.dither,
        };
    });
    return output;
}

} // namespace kin
