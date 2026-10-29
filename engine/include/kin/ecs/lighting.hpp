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

#include <kin/core/affine.hpp>
#include <kin/ecs/component.hpp>
#include <kin/ecs/world.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/color_grading.hpp>
#include <kin/renderer/lighting.hpp>
#include <kin/renderer/texture.hpp>

#include <memory>
#include <optional>
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
};

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
// The world lit, as EcsScene does it: `area` (in the coordinates drawn in,
// with no transform on: the screen's) darkened to its SceneLighting's
// ambient and lit by its lights. False, and nothing done, without a
// SceneLighting (or where the renderer can't).
bool apply_scene_lighting(Renderer2D& renderer, LightLayer& layer, EcsWorld& world, const Affine2& to_screen, Rectf area);
// The colour output its first enabled ColorGrade says; none without one.
std::optional<ColorOutput> scene_color_output(EcsWorld& world);

} // namespace kin
