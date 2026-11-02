#pragma once

// 2D lights as entities, and a scene's light and colour, as scene files give
// them. A scene with a SceneLighting is darkened to its ambient colour and lit
// where its LightComponents reach (LightLayer; kin/renderer/lighting.hpp):
// EcsScene does it after drawing the world and before what is on the UI layer
// and above, which stays unlit. A ColorGrade sets the game's colour output:
// exposure and tonemapping (a linear game's), a look (a LUT), dithering.
//
//     sun.set(kin::SceneLighting{.ambient = kin::Color::rgb(40, 44, 70)});
//     torch.child_of(player).set(kin::LightComponent{.color = kin::Color::rgb(255, 190, 120), .radius = 160});
//
// A light is where its entity is, turned with it, its radius scaled by its
// scale. The first SceneLighting (and ColorGrade) found is the scene's.
//
// Shadows: a light with `shadows` is stopped by LightOccluders (a wall, a
// crate, a tilemap's solid cells): what is behind them, from the light, is
// dark (`shadow_strength`: how dark; `shadow_softness`: the light's size, for
// soft edges).
//
//     torch.set(kin::LightComponent{.radius = 200, .shadows = true});
//     crate.set(kin::LightOccluder{.shape = kin::OccluderShape::Box, .size = {32, 32}});

#include <kin/core/affine.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/color_grading.hpp>
#include <kin/renderer/lighting.hpp>
#include <kin/renderer/texture.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

class Renderer2D;

struct LightComponent {
    Color color = colors::white;
    f32 radius = 128.0f;   // nothing further is lit (in the entity's units)
    f32 intensity = 1.0f;  // 1 adds `color` in full at the centre; up to 4
    // An image (its alpha) shaping the light over its 2 * radius square in
    // place of the round falloff: cones, flashlights, windows. Assets2D binds
    // `shape_asset` to `shape`.
    ComponentAssetRef shape_asset{};
    Texture shape{};
    f32 rotation = 0.0f;   // degrees, added to its entity's
    bool enabled = true;
    bool shadows = false;        // LightOccluders cast its shadows
    f32 shadow_strength = 1.0f;  // how much light they take: 1 all of it, 0 none
    f32 shadow_softness = 0.0f;  // the light's size (in its entity's units): soft edges; 0 sharp
};

enum class OccluderShape : u8 {
    Box,     // `size`, centred on the entity (and `offset` from it)
    Circle,  // `radius`
    Polygon, // `points`, the entity's units
    Tiles,   // its TilemapComponent's solid cells (those of `layer`; empty: every layer's)
};

// What stops lights that cast shadows: it turns and scales with its entity
// (a map's cells only move with it). `lit`: its own area is lit (only what is
// behind it is dark); else it is dark too.
struct LightOccluder {
    OccluderShape shape = OccluderShape::Box;
    Vec2f size{32.0f, 32.0f};
    f32 radius = 16.0f;
    std::vector<Vec2f> points;
    Vec2f offset{};
    std::string layer;
    bool lit = true;
    bool enabled = true;
};

// "box", "circle", "polygon", "tiles".
std::string_view occluder_shape_name(OccluderShape shape);
std::optional<OccluderShape> occluder_shape_named(std::string_view name);

struct SceneLighting {
    bool enabled = true;
    // What is not lit looks like this much light: black is dark, white is unlit.
    Color ambient = Color::rgb(40, 44, 64);
    // Light-map pixels per screen unit (lights are smooth: half is plenty).
    f32 resolution = 0.5f;
};

struct ColorGrade {
    bool enabled = true;
    // A linear game's (GameInfo window color_space linear): its light scaled,
    // and brought into the display's range ("none", "reinhard", "aces").
    f32 exposure = 1.0f;
    Tonemap tonemap = Tonemap::None;
    // A look: a .cube file or a LUT image of the content (ColorLut), applied
    // at `strength`. Assets2D binds `lut_asset` to `lut`.
    ComponentAssetRef lut_asset{};
    std::shared_ptr<const ColorLut> lut{};
    f32 strength = 1.0f;
    bool dither = false;
};

// "none", "reinhard", "aces".
std::string_view tonemap_name(Tonemap tonemap);
std::optional<Tonemap> tonemap_named(std::string_view name);

// The world's first enabled SceneLighting; null when it has none.
const SceneLighting* scene_lighting(EcsWorld& world);
// Its enabled lights, as LightLayer takes them, `to_screen` taking the world
// to where they are drawn (a camera's view transform; identity for none).
std::vector<Light2D> scene_lights(EcsWorld& world, const Affine2& to_screen);
// Its enabled occluders' outlines, as LightLayer takes them, through `to_screen` too.
std::vector<LightOccluder2D> scene_occluders(EcsWorld& world, const Affine2& to_screen);
// One entity's (none without an enabled LightOccluder): editors draw them.
std::vector<LightOccluder2D> occluder_outlines(EcsEntity entity, const Affine2& to_screen);
// The world lit, as EcsScene does it: `area` (in the coordinates drawn in,
// with no transform on: the screen's) darkened to its SceneLighting's
// ambient and lit by its lights. False, and nothing done, without a
// SceneLighting (or where the renderer can't).
bool apply_scene_lighting(Renderer2D& renderer, LightLayer& layer, EcsWorld& world, const Affine2& to_screen, Rectf area);
// The colour output its first enabled ColorGrade says; none without one.
std::optional<ColorOutput> scene_color_output(EcsWorld& world);

} // namespace kin
