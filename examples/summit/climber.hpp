#pragma once

// The climber's look and its CPU mesh data, apart from the renderer so that a test can walk the triangles (winding, finiteness)
// without a GPU. SummitView uploads the parts and binds them to the skeleton.

#include <kin/render3d/mesh.hpp>

#include <string>
#include <vector>

namespace summit {
using namespace kin;

// How a person looks: clothes, hat and skin. `style_for_player` gives each player's own (index mod 4).
struct ClimberStyle {
    Color jacket, trousers, hat, pack, skin, hair;
};
ClimberStyle style_for_player(i32 index);

// One part of the person (a material's worth of triangles), skinned to person_rig()'s joints.
struct ClimberPart {
    std::string name;
    Color colour;
    f32 roughness = 0.7f;
    MeshData mesh;
    f32 metallic = 0.0f;
};
// The parts with triangles, in drawing order. No renderer, no GPU.
std::vector<ClimberPart> build_climber_meshes(const ClimberStyle& style);

} // namespace summit
