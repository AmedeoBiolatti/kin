#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/texture.hpp>

#include <span>

namespace kin {

// 2D lighting: darken what is already drawn to an ambient light, and brighten it
// where lights reach.
//
//   renderer.clear(...);
//   draw_world(renderer);
//   lighting.apply(renderer, {0, 0, 960, 540}, night_ambient, lights);
//   draw_ui(renderer);                       // drawn after: not lit
//
// apply() builds a light map (the ambient colour plus every light, added
// together) in a render target and multiplies it over `area`. It needs render
// targets and blend modes, which both real backends have, and no shaders.

struct Light2D {
    // Centre, in the coordinates apply() is called in (usually logical pixels).
    Vec2f position{};
    // Nothing further than this is lit. The light fades smoothly to zero there.
    f32 radius = 128.0f;
    Color color = colors::white;
    // Scales the light; 1 adds `color` in full at the centre. Up to 4.
    f32 intensity = 1.0f;
    // Optional light shape drawn instead of the round falloff, stretched over the
    // light's 2 * radius square: its alpha is how much light reaches each point.
    // For cones, flashlights and windows.
    Texture shape{};
    // Rotation of `shape` about the centre, in degrees clockwise.
    f32 rotation = 0.0f;
};

class LightLayer {
public:
    // Multiplies `area` of the current target by the ambient light plus
    // `lights`. `resolution` is light-map pixels per unit of `area` (lights are
    // smooth, so half resolution is plenty). Returns false and changes nothing
    // when the backend lacks render targets or blend modes: the scene stays
    // unlit.
    bool apply(Renderer2D& renderer, Rectf area, Color ambient, std::span<const Light2D> lights,
               f32 resolution = 0.5f);

    // The round falloff texture every Light2D without a shape uses: white,
    // with alpha (1 - d^2)^2 at distance d (0..1) from the centre.
    const Texture& falloff(Renderer2D& renderer);

private:
    Texture _falloff;
};

} // namespace kin
