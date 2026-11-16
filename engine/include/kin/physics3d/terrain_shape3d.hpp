#pragma once

// A heightmap terrain's collision (docs/rendering_3d.md, Terrain; docs/physics_3d.md): a Jolt height field of its
// heights, for a static body.
//
//   kin::Body3D ground = world.create_body({.shape = kin::terrain_shape3d(terrain), .motion = kin::Motion3D::Static});
//   ... after a stroke: kin::update_terrain_collision(world, ground, terrain, terrain.end_stroke());

#include <kin/physics3d/shape3d.hpp>
#include <kin/physics3d/world3d.hpp>
#include <kin/render3d/terrain3d.hpp>

namespace kin {

// The terrain's surface as a height field shape at its place (world units; the body at the origin): Jolt's height
// fields are square with an even number of samples, so a terrain of another size is padded with holes past its edges.
// Heights are kept to about a 256th of each 2 x 2 block's range; its whole height range (min_height to max_height)
// is kept open for update_terrain_collision. Invalid for an empty terrain.
Shape3D terrain_shape3d(const Terrain3D& terrain);

// A body of terrain_shape3d(terrain) brought up to the terrain's heights over a changed rectangle of samples (a
// stroke's: Terrain3D::brush, end_stroke, undo), in place (World3D::set_heights): far cheaper than a new shape. False
// when it could not (then set_shape a new one).
bool update_terrain_collision(World3D& world, Body3D body, const Terrain3D& terrain, Terrain3D::Changed changed);

} // namespace kin
