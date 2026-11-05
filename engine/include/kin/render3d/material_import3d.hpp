#pragma once

// Other engines' materials as kin's (docs/material_graph_plan.md, "Importing"): a material file of Godot's (and,
// later, Unity's and Unreal's) read into a kin.materials3d/1 patch, with the textures kin needs made from the ones it
// names (packed, recombined, copied). What kin can't show as the source does is said in `notes`, not dropped silently.
// CPU-only (tools, the studio); no renderer.

#include <filesystem>
#include <kin/render3d/errors3d.hpp>
#include <kin/render3d/material_asset3d.hpp>
#include <string>
#include <vector>

namespace kin {

struct MaterialImportOptions3D {
    // The source project's root: Godot's project.godot folder (where res:// starts); Unity's, holding Assets/.
    std::filesystem::path project;
    // kin's content root: the paths written are relative to it, as .kinmaterials files' are.
    std::filesystem::path content;
    // Where the .kinmaterials file and the textures made go (inside `content`); textures named from outside
    // `content` are copied here too.
    std::filesystem::path out;
    // Write the files (else only the result: what would be written).
    bool write = true;
};

struct ImportedMaterial3D {
    std::string name;                           // the source's, else its file's stem
    MaterialsAsset3D asset;                     // its base material: the import
    std::filesystem::path file;                 // the .kinmaterials (in `out`)
    std::vector<std::filesystem::path> written; // the textures made or copied
    std::vector<std::string> notes;             // what the source has that kin does not show as it does
};

// A Godot 4 StandardMaterial3D or ORMMaterial3D (.tres): albedo, metallic and roughness (their maps' channels packed
// into kin's one map), normal map, ambient occlusion, emission (its colour and map combined by its operator; in nits
// with the project's physical light units), transparency (alpha, scissor, hash as a cut-out), cull mode, unshaded,
// the blend mode's add, texture filter and repeat.
Result3D<ImportedMaterial3D> import_godot_material(const std::filesystem::path& source,
                                                   const MaterialImportOptions3D& options);

// A Unity material (.mat), by its shader's properties: the built-in pipeline's Standard, URP's Lit (Simple Lit and
// Baked Lit as dielectrics, Unlit unlit), HDRP's Lit; a custom shader or Shader Graph by the usual property names
// (noted). Textures are found by GUID through the project's .meta files (`project`: the folder holding Assets/). Base
// colour and map, its tiling and offset (every map's), metallic and smoothness (kin's map made: roughness 1 -
// smoothness, from the metallic map's or the albedo's alpha; HDRP's mask map remapped), normal map, occlusion (Unity's
// G), emission (the _EMISSION keyword; HDRP's linear), opaque / cut-out / transparent (alpha, additive), faces,
// receiving shadows.
Result3D<ImportedMaterial3D> import_unity_material(const std::filesystem::path& source,
                                                   const MaterialImportOptions3D& options);

} // namespace kin
