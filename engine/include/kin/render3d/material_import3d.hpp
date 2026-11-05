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
    std::filesystem::path graph;                // a graph import's kin.material-graph/1 file (written in `out`)
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

// An Unreal material or material instance, from the kin.unreal-material/1 file tools/import_material/unreal_export.py
// writes inside Unreal's editor (`project`: that export's folder, its textures' PNGs under it). Each output's
// expressions are read as bias + gain x a texture's channel: constants, parameters (the instance's values), texture
// samples (their UV tiling), Multiply, Add, Subtract, One Minus, component masks, a Lerp by a constant 0 or 1; anything
// else is noted. Base colour (linear), metallic and roughness (an ORM map as kin's, else packed), occlusion, normal
// (DirectX's green flipped, unless the texture flips it on import), emission, opacity from the base map's alpha,
// blend mode (opaque, masked, translucent, additive), two-sided, unlit; one-colour maps folded into their factor.
Result3D<ImportedMaterial3D> import_unreal_material(const std::filesystem::path& source,
                                                    const MaterialImportOptions3D& options);

// Tier 2, the other engines' graphs (docs/material_graph_plan.md): the source's node graph as a kin material graph
// (written as <its graph's name>.kingraph in `out`, one for every material of one graph), and the material as a
// .kinmaterials naming it, with its parameters' values and textures, over what the Tier 1 import makes of it (what the
// graph can't express keeps that). Nodes kin has no counterpart for are noted, naming them, and the outputs they feed
// left to the Tier 1 values.
//
// Unreal (an unreal_export.py export's "graph"): constants, scalar, vector and texture parameters, texture samples
// (normal maps unpacked; DirectX's green turned to kin's), texture coordinates (tiling), Multiply, Add, Subtract,
// Divide, Min, Max, Power, Clamp, Lerp, Step, SmoothStep, One Minus, Abs, Floor, Ceil, Frac, Square Root, Saturate,
// Normalize, Dot, Cross, Distance, Sine and Cosine (their period), Component Mask, Append, Time, World Position
// (centimetres), Vertex Color, Camera Vector, Pixel and Vertex Normal, Fresnel, Panner, Desaturation, Reroute, Static
// Switch (its default).
Result3D<ImportedMaterial3D> import_unreal_graph(const std::filesystem::path& source,
                                                 const MaterialImportOptions3D& options);

// Unity (a .mat whose shader is a Shader Graph, found by GUID): properties (by their reference names, the material's
// values), Sample Texture 2D (normal maps unpacked), UV, Tiling And Offset, Add, Subtract, Multiply, Divide, Minimum,
// Maximum, Power, Lerp, Step, Smoothstep, Clamp, Remap, One Minus, Saturate, Absolute, Negate, Fraction, Floor,
// Ceiling, Normalize, Square Root, Sine, Cosine, Length, Dot Product, Cross Product, Distance, Split, Combine,
// Vector 1 to 4, Color, Constant, Slider, Time, Position and Normal Vector and View Direction (world), Vertex Color,
// Fresnel Effect, Simple, Gradient and Voronoi Noise (kin's, approximately), Branch, Triangle Wave, Redirect, Preview;
// the URP target's surface (opaque, transparent, alpha clip), two sides and Unlit; the blocks' own values where
// nothing feeds them.
Result3D<ImportedMaterial3D> import_unity_graph(const std::filesystem::path& source,
                                                const MaterialImportOptions3D& options);

// Godot 4 (a ShaderMaterial whose shader is a VisualShader: its own file or inline; StandardMaterial3D and
// ORMMaterial3D as import_godot_material): the fragment stage's parameters (the material's values), Texture (its
// own texture or a sampler parameter's), Input (uv, time, color; vertex, normal and view as kin's world ones, said),
// float and vector constants, operators and functions, Mix, Clamp, Step, SmoothStep, Remap, MultiplyAdd, Dot
// Product, Length, Distance, Vector Compose and Decompose, Fresnel, UV Func, If, Switch, Compare (scalars),
// Reroute, Expression (one output, one assignment); Albedo, Alpha, Metallic, Roughness, Emission, AO, Normal Map
// (and its depth), the alpha scissor threshold; cull and unshaded.
Result3D<ImportedMaterial3D> import_godot_graph(const std::filesystem::path& source,
                                                const MaterialImportOptions3D& options);

} // namespace kin
