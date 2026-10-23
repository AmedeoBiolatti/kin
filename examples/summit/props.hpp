#pragma once

#include "level.hpp"

#include <array>
#include <kin/physics3d/shape3d.hpp>
#include <span>

namespace summit {

enum PropKind {
    Tent,
    Firepit,
    Bedroll,
    PackedBedroll,
    Crate,
    Barrel,
    Bottle,
    Wood,
    Tree,
    TallTree,
    RockA,
    RockB,
    Grass,
    LargeGrass,
    PropCount
};
inline constexpr std::array<const char*, PropCount> prop_names{
    "tent-canvas",   "campfire-pit", "bedroll",   "bedroll-packed", "box",    "barrel", "bottle",
    "resource-wood", "tree",         "tree-tall", "rock-a",         "rock-b", "grass",  "grass-large"};
struct PropPlacement {
    PropKind kind;
    Vec3f feet;
    f32 span, yaw;
};
struct PropGeometry {
    Vec3f source_min, source_max;
    std::span<const Vec3f> points;
    std::span<const u32> indices;
};
// One placement catalogue for drawing and physics; source bounds and collider vertices are baked from the pinned glTF
// files.
std::vector<PropPlacement> scenery_props(const SummitLevel& level);
std::vector<PropPlacement> camp_props(const Camp& camp, std::size_t index);
std::vector<PropPlacement> solid_props(const SummitLevel& level);
const PropGeometry& prop_geometry_data(PropKind kind);
bool solid_prop(PropKind kind);
Shape3D prop_shape(const PropPlacement& prop);

} // namespace summit
