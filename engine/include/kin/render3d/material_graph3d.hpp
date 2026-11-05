#pragma once

// Material graphs (docs/material_graph_plan.md, P2): nodes wired into what a surface is, compiled into a surface
// shader (Material3D::surface: docs/rendering_3d.md, Surface shaders), as Godot's visual shaders and Unreal's material
// editor compile theirs into their engines' own shaders. CPU-only: the compiler writes GLSL; the renderer compiles it
// (Renderer3D::compile_material_graph) or a build does (kin_compile_surface_shader on the GLSL written).
//
// A graph (kin.material-graph/1, JSON):
//
//   {"schema": "kin.material-graph/1", "name": "Rust",
//    "parameters": [{"name": "tint", "type": "color", "default": [1, 0.5, 0.2, 1]},
//                   {"name": "albedo", "type": "texture", "slot": "base"}],
//    "nodes": [{"id": "map", "type": "texture", "texture": "albedo"},
//              {"id": "rust", "type": "noise_gradient", "inputs": {"p": "scaled"}},
//              {"id": "scaled", "type": "multiply", "inputs": {"a": "position", "b": 4}},
//              {"id": "color", "type": "mix", "inputs": {"a": "map.rgb", "b": "tint.rgb", "t": "rust"}}],
//    "outputs": {"base_color": "color", "roughness": 0.8}}
//
// An input is a reference ("node", "node.output", "node.xyz", a parameter's name, a built-in input's name: "uv",
// "position", "normal", "view", "vertex_color", "time", "screen", "camera_distance", "surface.<field>") or a value (a
// number, an array of 2 to 4). Outputs not given keep what kin's material makes (the stock surface). Types are float,
// vec2, vec3, vec4: a float widens to any; a vector narrows to its first components, or widens with 0 (and 1 for the
// fourth). material_graph_nodes3d() lists the node types.

#include <array>
#include <kin/core/types.hpp>
#include <kin/render3d/scene3d.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

enum class GraphType3D : u8 { Float = 1, Vec2, Vec3, Vec4, Texture };

// A parameter: a value in Material3D::shader_params (`offset` floats in, `size` of them), or a texture bound in one of
// the material's map slots (Material3D::texture, metallic_roughness, normal, occlusion, emissive_texture).
struct MaterialGraphParameter3D {
    std::string name;
    GraphType3D type = GraphType3D::Float;
    u32 offset = 0, size = 1;
    std::array<f32, 4> value{}; // the default
    MaterialMap3D slot = MaterialMap3D::BaseColor;
    bool srgb = true;  // a texture's colour space
    bool data = false; // a texture that isn't the slot's own map (kin's stock reading of the slot is undone)
};

struct MaterialGraphError3D {
    std::string node; // the node or output it is about (empty: the graph)
    std::string message;
};

struct CompiledMaterialGraph3D {
    std::string name;
    std::string surface; // the surface shader: `void kin_surface(inout KinSurface s)` (surface_shader_source's input)
    std::vector<MaterialGraphParameter3D> parameters;
    std::vector<MaterialGraphError3D> errors;
    bool ok() const { return errors.empty(); }
};

CompiledMaterialGraph3D compile_material_graph3d(std::string_view graph);

// Material3D::shader_params for the graph's parameters: each one's default, or its value in `values` (by name).
std::array<f32, 16> material_graph_params3d(const CompiledMaterialGraph3D& graph,
                                            const std::map<std::string, std::array<f32, 4>>& values = {});

struct MaterialGraphNode3D {
    std::string type, summary;
    std::vector<std::string> inputs;  // its inputs' names, in order
    std::vector<std::string> outputs; // named outputs beyond its value (a texture's "rgb", "a", ...)
};
const std::vector<MaterialGraphNode3D>& material_graph_nodes3d();

} // namespace kin
