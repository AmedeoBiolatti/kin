#pragma once
#include <kin/assets/image.hpp>
#include <kin/core/affine.hpp>
#include <kin/renderer/lighting.hpp>

namespace kin {
struct SurfaceMaterial2D {
    std::shared_ptr<const Image> albedo, normal, specular;
    f32 normal_strength = 1, specular_strength = 0, shininess = 32;
    i32 light_mask = -1;
    Color tint = colors::white;
};
struct SurfaceLight2D {
    Vec2f position{};
    f32 height = 64, radius = 128, intensity = 1;
    Color color = colors::white;
    i32 mask = -1;
    bool directional = false;
    Vec2f direction = {0, -1}; // direction toward a directional light; height is its Z
    bool shadows = false;
    f32 shadow_strength = 1, shadow_softness = 0;
    std::shared_ptr<const Image> shape;
    f32 rotation = 0;
};
struct SurfaceGeometry2D {
    Rectf source{}; // albedo pixels; empty = whole image; maps share normalized UVs
    Rectf dest{};   // world space, before rotation about pivot
    f32 rotation = 0;
    Vec2f pivot{.5f, .5f};
    Flip flip = Flip::None;
};
// Portable reference/CPU forward shading, at source resolution, in linear
// light. RGB normal maps: +X right, +Y down, +Z toward the viewer. Albedo and
// light colours are sRGB; normal/specular maps are data. Preserves alpha.
// Point and directional lights, receiver masks, Lambert + Blinn-Phong, and
// existing polygon occluders (four shadow samples for soft lights).
Image shade_normal_sprite2d(const SurfaceMaterial2D&, const SurfaceGeometry2D&, Color ambient,
                            std::span<const SurfaceLight2D>, std::span<const LightOccluder2D> occluders = {});
} // namespace kin
