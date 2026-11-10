#pragma once

// What the CPU and GPU paths of normal-mapped sprites share
// (kin/renderer/normal_lighting2d.hpp): which inputs can be drawn, where the
// surface lands, and the shadows lights cast.

#include <kin/renderer/normal_lighting2d.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace kin::normal_lighting2d_detail {

// The albedo pixels the sprite shows (its source, or the whole image); none
// when the material or the geometry can't be drawn.
std::optional<Rectf> surface_source(const SurfaceMaterial2D& material, const SurfaceGeometry2D& geometry);

// The lights shaded with: those with usable values, at most max_surface_lights.
std::vector<const SurfaceLight2D*> usable_lights(std::span<const SurfaceLight2D> lights);

// Where the point `at` (a fraction of the sprite's dest, before flipping)
// lands once the sprite is turned about its pivot.
Vec2f surface_point(const SurfaceGeometry2D& geometry, Vec2f at);

// The shadows a light casts, as quads: one list per shadow sample (none for a
// light without shadows).
std::vector<std::vector<std::array<Vec2f, 4>>> shadow_samples(const SurfaceLight2D& light,
                                                              std::span<const LightOccluder2D> occluders);

} // namespace kin::normal_lighting2d_detail
