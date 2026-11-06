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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

enum class GraphType3D : u8 { Float = 1, Vec2, Vec3, Vec4, Texture };

// A parameter: a value in Material3D::shader_params (`offset` floats in, `size` of them), or a texture bound in one of
// the material's map slots (Material3D::texture, metallic_roughness, normal, occlusion, emissive_texture), or in a
// layer of its graph maps (Material3D::graph_maps: slot "layers", `layer`; `slot` is then MaterialMap3D::Count).
struct MaterialGraphParameter3D {
    std::string name;
    GraphType3D type = GraphType3D::Float;
    u32 offset = 0, size = 1;
    std::array<f32, 4> value{}; // the default
    MaterialMap3D slot = MaterialMap3D::BaseColor;
    bool srgb = true;  // a texture's colour space
    bool data = false; // a texture that isn't the slot's own map (kin's stock reading of the slot is undone)
    u32 layer = ~0u;   // a texture in the graph maps: its layer (else ~0u)
    bool layered() const { return layer != ~0u; }
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
    // What compiles but is not all it could be: an alpha the cut-out passes can't compute (they then cut by the base
    // map's alpha), naming the node.
    std::vector<MaterialGraphError3D> warnings;
    // Its alpha, in the shadow, occluder, pick, pre- and visibility passes too (kin_surface_alpha in `surface`).
    bool cutout = false;
    // Its `offset` output (World Position Offset: a vector added to each vertex's world position, in metres) compiled
    // for the vertex variants too (`vec3 kin_surface_offset()` under KIN_SURFACE_VERTEX in `surface`); false with none,
    // or one a vertex can't compute (then a warning, naming the node).
    bool offset = false;
    bool ok() const { return errors.empty(); }
};

CompiledMaterialGraph3D compile_material_graph3d(std::string_view graph);

// A material graph made ready to draw (Renderer3D::compile_material_graph, create_material_graph,
// Assets3D::load_material_graph): its surface shader, and its layout (its parameters' places in
// Material3D::shader_params: material_graph_params3d).
struct MaterialGraphShader3D {
    SurfaceShader3D surface;
    CompiledMaterialGraph3D graph;
};

// Material3D::shader_params for the graph's parameters: each one's default, or its value in `values` (by name).
std::array<f32, surface_param_count3d>
material_graph_params3d(const CompiledMaterialGraph3D& graph,
                        const std::map<std::string, std::array<f32, 4>>& values = {});

// The parameters' layout as JSON (kin.material-graph-layout/1), and back: what a cooked graph carries beside its
// SPIR-V, so that a game binds parameters without the graph's source.
std::string write_material_graph_layout3d(const CompiledMaterialGraph3D& graph);
std::optional<CompiledMaterialGraph3D> read_material_graph_layout3d(std::string_view json, std::string* why = nullptr);

// A cooked graph (<graph>.spv beside its source, `kin_material_graph cook`, cmake's kin_cook_material_graph): its
// layout and its surface shader's variants, compiled (SurfaceVariant3D's order), in one file.
struct CookedMaterialGraph3D {
    CompiledMaterialGraph3D graph; // its layout (no source: `surface` empty)
    std::array<std::vector<u8>, surface_variant_count3d> spirv;
};
std::vector<u8> pack_cooked_material_graph3d(const CookedMaterialGraph3D& cooked);
std::optional<CookedMaterialGraph3D> unpack_cooked_material_graph3d(std::span<const u8> bytes,
                                                                    std::string* why = nullptr);
// The graph compiled and cooked now by glslc (kin/renderer/shader_compiler.hpp); none, with why (the graph's errors,
// each naming its node, or glslc's messages).
std::optional<CookedMaterialGraph3D> cook_material_graph3d(std::string_view graph, std::string* why = nullptr);

struct MaterialGraphNode3D {
    std::string type, summary;
    std::vector<std::string> inputs;  // its inputs' names, in order
    std::vector<std::string> outputs; // named outputs beyond its value (a texture's "rgb", "a", ...)
};
const std::vector<MaterialGraphNode3D>& material_graph_nodes3d();

} // namespace kin
