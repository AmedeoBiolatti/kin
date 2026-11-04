#include <kin/ecs/lighting.hpp>

#include <kin/ecs/render.hpp>
#include <kin/ecs/tilemap.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <algorithm>
#include <cmath>

namespace kin {
namespace {

// Where a light is: its entity's place, the larger of its scales, its turn.
struct PlacedLight {
    Vec2f position{};
    f32 scale = 1.0f;
    f32 rotation = 0.0f;
};

bool shining(const LightComponent& light) {
    return light.enabled && light.radius > 0.0f && light.intensity > 0.0f;
}

PlacedLight placed_light(flecs::entity entity, const LightComponent& light) {
    const WorldTransform placed = current_world_transform(entity);
    return {.position = placed.pos,
            .scale = std::max(std::abs(placed.scale.x), std::abs(placed.scale.y)),
            .rotation = placed.rotation + light.rotation};
}

} // namespace

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

std::string_view occluder_shape_name(OccluderShape shape) {
    switch (shape) {
    case OccluderShape::Box: return "box";
    case OccluderShape::Circle: return "circle";
    case OccluderShape::Polygon: return "polygon";
    case OccluderShape::Tiles: return "tiles";
    }
    return "box";
}

std::optional<OccluderShape> occluder_shape_named(std::string_view name) {
    for (const OccluderShape shape : {OccluderShape::Box, OccluderShape::Circle, OccluderShape::Polygon, OccluderShape::Tiles}) {
        if (occluder_shape_name(shape) == name) {
            return shape;
        }
    }
    return std::nullopt;
}

const SceneLighting* scene_lighting(EcsWorld& world) {
    return scene_lighting(world.raw());
}

const SceneLighting* scene_lighting(flecs::world& world) {
    const SceneLighting* found = nullptr;
    world.each([&](const SceneLighting& lighting) {
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
        if (!shining(light)) {
            return;
        }
        const PlacedLight placed = placed_light(entity, light);
        lights.push_back({
            .position = to_screen.apply(placed.position),
            .radius = light.radius * placed.scale * view_scale,
            .color = light.color,
            .intensity = light.intensity,
            .shape = light.shape,
            .rotation = placed.rotation + view_turn,
            .shadows = light.shadows,
            .shadow_strength = light.shadow_strength,
            .shadow_softness = std::max(light.shadow_softness, 0.0f) * placed.scale * view_scale,
        });
    });
    return lights;
}

std::vector<SurfaceLight2D> scene_surface_lights(flecs::world& world) {
    std::vector<SurfaceLight2D> lights;
    world.each([&](flecs::entity entity, const LightComponent& light) {
        if (!shining(light)) {
            return;
        }
        const PlacedLight placed = placed_light(entity, light);
        lights.push_back({.position = placed.position,
                          .height = light.height * placed.scale,
                          .radius = light.radius * placed.scale,
                          .intensity = light.intensity,
                          .color = light.color,
                          .mask = light.mask,
                          .shadows = light.shadows,
                          .shadow_strength = light.shadow_strength,
                          .shadow_softness = std::max(light.shadow_softness, 0.0f) * placed.scale,
                          .shape = light.shape_image,
                          .rotation = placed.rotation});
    });
    world.each([&](flecs::entity entity, const DirectionalLight2D& light) {
        if (!light.enabled || light.intensity <= 0.0f) {
            return;
        }
        const WorldTransform placed = current_world_transform(entity);
        lights.push_back({.height = light.elevation,
                          .intensity = light.intensity,
                          .color = light.color,
                          .mask = light.mask,
                          .directional = true,
                          .direction = Affine2::rotation(placed.rotation).apply_vector(light.direction),
                          .shadows = light.shadows,
                          .shadow_strength = light.shadow_strength,
                          .shadow_softness = std::max(light.shadow_softness, 0.0f)});
    });
    return lights;
}

std::vector<LightOccluder2D> occluder_outlines(EcsEntity handle, const Affine2& to_screen) {
    constexpr f32 pi = 3.14159265358979323846f;
    std::vector<LightOccluder2D> out;
    const flecs::entity entity = handle.raw();
    const LightOccluder* found = entity && entity.is_alive() ? entity.get<LightOccluder>() : nullptr;
    if (!found || !found->enabled) {
        return out;
    }
    const LightOccluder& occluder = *found;
    const bool mirrored = to_screen.a * to_screen.d - to_screen.b * to_screen.c < 0.0f;
    const WorldTransform placed = current_world_transform(handle);
    const Affine2 to = to_screen * Affine2::trs(placed.pos, placed.rotation, placed.scale);
    const auto add = [&](std::vector<Vec2f> local) {
        if (local.size() < 3) {
            return;
        }
        for (Vec2f& p : local) {
            p = to.apply({p.x + occluder.offset.x, p.y + occluder.offset.y});
        }
        out.push_back({.points = clockwise(std::move(local)), .lit = occluder.lit});
    };
    switch (occluder.shape) {
    case OccluderShape::Box: {
        const Vec2f h{occluder.size.x * 0.5f, occluder.size.y * 0.5f};
        add({{-h.x, -h.y}, {h.x, -h.y}, {h.x, h.y}, {-h.x, h.y}});
        break;
    }
    case OccluderShape::Circle: {
        constexpr i32 sides = 24;
        std::vector<Vec2f> ring;
        for (i32 i = 0; i < sides; ++i) {
            const f32 angle = 2.0f * pi * static_cast<f32>(i) / static_cast<f32>(sides);
            ring.push_back({std::cos(angle) * occluder.radius, std::sin(angle) * occluder.radius});
        }
        add(std::move(ring));
        break;
    }
    case OccluderShape::Polygon:
        add(occluder.points);
        break;
    case OccluderShape::Tiles: {
        // Its map's solid cells, joined into outlines (holes wound the other
        // way, so kept as they are); the map is drawn from where its entity is.
        const auto* tiles = entity.get<TilemapComponent>();
        if (!tiles || !tiles->tilemap) {
            break;
        }
        const TileCollisionShapes shapes = tiles->tilemap->map.collision_shapes(occluder.layer);
        const Affine2 map = to_screen * Affine2::translation({placed.pos.x + occluder.offset.x, placed.pos.y + occluder.offset.y});
        for (const std::vector<Vec2f>& loop : shapes.loops) {
            LightOccluder2D outline{.points = loop, .lit = occluder.lit};
            for (Vec2f& p : outline.points) {
                p = map.apply(p);
            }
            if (mirrored) {
                std::reverse(outline.points.begin(), outline.points.end());
            }
            out.push_back(std::move(outline));
        }
        for (const std::vector<Vec2f>& polygon : shapes.polygons) {
            std::vector<Vec2f> points = polygon;
            for (Vec2f& p : points) {
                p = map.apply(p);
            }
            out.push_back({.points = clockwise(std::move(points)), .lit = occluder.lit});
        }
        break;
    }
    }
    return out;
}

std::vector<LightOccluder2D> scene_occluders(EcsWorld& world, const Affine2& to_screen) {
    return scene_occluders(world.raw(), to_screen);
}

std::vector<LightOccluder2D> scene_occluders(flecs::world& world, const Affine2& to_screen) {
    std::vector<flecs::entity> occluding;
    world.each([&](flecs::entity entity, const LightOccluder& occluder) {
        if (occluder.enabled) {
            occluding.push_back(entity);
        }
    });
    std::vector<LightOccluder2D> out;
    for (const flecs::entity entity : occluding) {
        std::vector<LightOccluder2D> outlines = occluder_outlines(EcsEntity{entity}, to_screen);
        out.insert(out.end(), std::make_move_iterator(outlines.begin()), std::make_move_iterator(outlines.end()));
    }
    return out;
}

bool apply_scene_lighting(Renderer2D& renderer, LightLayer& layer, EcsWorld& world, const Affine2& to_screen, Rectf area) {
    const SceneLighting* lighting = scene_lighting(world);
    if (!lighting) {
        return false;
    }
    const std::vector<Light2D> lights = scene_lights(world, to_screen);
    const bool shadows = std::ranges::any_of(lights, [](const Light2D& light) { return light.shadows; });
    const std::vector<LightOccluder2D> occluders = shadows ? scene_occluders(world, to_screen) : std::vector<LightOccluder2D>{};
    return layer.apply(renderer, area, lighting->ambient, lights, std::clamp(lighting->resolution, 0.05f, 1.0f), occluders);
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
