#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/texture.hpp>

#include <array>
#include <span>
#include <vector>

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
//
// Shadows: a light with `shadows` is drawn into a target of its own, the
// shadow of each occluder (LightOccluder2D: the space behind it, as seen from
// the light) filled black there, and the rest added to the map. Its
// `shadow_strength` says how much light the shadows take (1: all of it); its
// `shadow_softness` is the light's size, its shadows' edges blurred as by a
// light that big (four shadows from around it, averaged).

// The most a light's intensity counts for (1 adds its colour in full at its
// centre). Light maps and normal-mapped sprites (normal_lighting2d.hpp) cap
// it the same.
inline constexpr f32 max_light_intensity = 4.0f;

struct Light2D {
    // Centre, in the coordinates apply() is called in (usually logical pixels).
    Vec2f position{};
    // Nothing further than this is lit. The light fades smoothly to zero there.
    f32 radius = 128.0f;
    Color color = colors::white;
    // Scales the light; 1 adds `color` in full at the centre. Up to
    // max_light_intensity.
    f32 intensity = 1.0f;
    // Optional light shape drawn instead of the round falloff, stretched over the
    // light's 2 * radius square: its alpha is how much light reaches each point.
    // For cones, flashlights and windows.
    Texture shape{};
    // Rotation of `shape` about the centre, in degrees clockwise.
    f32 rotation = 0.0f;
    // Occluders cast its shadows.
    bool shadows = false;
    f32 shadow_strength = 1.0f; // how much light its shadows take: 1 all, 0 none
    f32 shadow_softness = 0.0f; // the light's size (soft edges); 0: a point, sharp shadows
};

// What stops light: a closed outline, in the coordinates lights are in, wound
// with what it is made of on the right going along it (clockwise on a screen
// with y down; a hole the other way round). `lit`: its own area is lit (only
// what is behind it is in shadow); else it is dark too.
struct LightOccluder2D {
    std::vector<Vec2f> points;
    bool lit = true;
};

// `points` wound clockwise on a screen with y down (reversed if they are not).
std::vector<Vec2f> clockwise(std::vector<Vec2f> points);

// The shadows occluders cast from a light at `light`, out to `reach` from it:
// for each edge facing away from it (each edge, for an occluder not `lit`),
// the quad from it out along the light's rays.
std::vector<std::array<Vec2f, 4>> shadow_quads(Vec2f light, f32 reach, std::span<const LightOccluder2D> occluders);

// The same for a light infinitely far away (a sun): `toward` is the direction
// from the scene to it (any length), and each quad reaches `reach` back from
// its edge. None when `toward` is zero (the light straight above).
std::vector<std::array<Vec2f, 4>> directional_shadow_quads(Vec2f toward, f32 reach,
                                                           std::span<const LightOccluder2D> occluders);

// Where a light casts its shadows from, as offsets from its centre in units of
// its `shadow_softness`: its centre alone when that is 0 or less (sharp
// shadows), else four points around it, each casting a quarter of them.
std::span<const Vec2f> shadow_sample_offsets(f32 softness);

class LightLayer {
public:
    // Multiplies `area` of the current target by the ambient light plus
    // `lights`. `resolution` is light-map pixels per unit of `area` (lights are
    // smooth, so half resolution is plenty). Returns false and changes nothing
    // when the backend lacks render targets or blend modes: the scene stays
    // unlit.
    bool apply(Renderer2D& renderer, Rectf area, Color ambient, std::span<const Light2D> lights,
               f32 resolution = 0.5f, std::span<const LightOccluder2D> occluders = {});

    // The round falloff texture every Light2D without a shape uses: white,
    // with alpha (1 - d^2)^2 at distance d (0..1) from the centre.
    const Texture& falloff(Renderer2D& renderer);

private:
    Texture _falloff;
};

} // namespace kin
