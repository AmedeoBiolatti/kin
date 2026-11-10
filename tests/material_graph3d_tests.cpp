// Material graphs (kin/render3d/material_graph3d.hpp), on the CPU: the compiler's GLSL (parameters packed, dead nodes
// left out, types cast, outputs as kin takes them), its errors naming their nodes, and, with glslc, every node type
// compiled into every variant of kin's mesh shader.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <kin/render3d/material_graph3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <string>

namespace {
using namespace kin;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __FILE__ << ":" << __LINE__ << ": " #condition "\n";                                          \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

bool contains(const std::string& text, std::string_view what) {
    return text.find(what) != std::string::npos;
}
bool error_at(const CompiledMaterialGraph3D& g, std::string_view node, std::string_view what) {
    return std::any_of(g.errors.begin(), g.errors.end(),
                       [&](const MaterialGraphError3D& e) { return e.node == node && contains(e.message, what); });
}
std::string graph(const std::string& parameters, const std::string& nodes, const std::string& outputs) {
    return R"({"schema": "kin.material-graph/1", "name": "test", "parameters": [)" + parameters + R"(], "nodes": [)" +
           nodes + R"(], "outputs": {)" + outputs + "}}";
}

void compiles() {
    const auto g = compile_material_graph3d(graph(
        R"({"name": "tint", "type": "color", "default": [1, 0.5, 0.25, 1]},
           {"name": "scale", "type": "float", "default": 4},
           {"name": "shift", "type": "vec2", "default": [0.5, 0.25]},
           {"name": "albedo", "type": "texture", "slot": "base"},
           {"name": "noise", "type": "texture", "slot": "occlusion", "role": "data"})",
        R"({"id": "map", "type": "texture", "texture": "albedo"},
           {"id": "scaled", "type": "multiply", "inputs": {"a": "position", "b": "scale"}},
           {"id": "rust", "type": "noise_gradient", "inputs": {"p": "scaled"}},
           {"id": "color", "type": "mix", "inputs": {"a": "map.rgb", "b": "tint.rgb", "t": "rust"}},
           {"id": "unused", "type": "sin", "inputs": {"x": "time"}})",
        R"("base_color": "color", "roughness": 0.8, "alpha": "map.a")"));
    for (const auto& e : g.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(g.ok() && g.name == "test");
    // Parameters: tint a vec4 of its own, scale then shift packed after it, textures in their slots.
    CHECK(g.parameters.size() == 5);
    CHECK(g.parameters[0].offset == 0 && g.parameters[0].size == 4);
    CHECK(g.parameters[1].offset == 4 && g.parameters[2].offset == 6 && g.parameters[2].size == 2);
    CHECK(g.parameters[3].type == GraphType3D::Texture && g.parameters[3].slot == MaterialMap3D::BaseColor &&
          g.parameters[3].srgb && !g.parameters[3].data);
    CHECK(g.parameters[4].slot == MaterialMap3D::Occlusion && !g.parameters[4].srgb && g.parameters[4].data);
    const auto params = material_graph_params3d(g, {{"scale", {7, 0, 0, 0}}});
    CHECK(params[0] == 1 && params[1] == 0.5f && params[4] == 7 && params[6] == 0.5f && params[7] == 0.25f);
    const std::string& s = g.surface;
    CHECK(contains(s, "void kin_surface(inout KinSurface s)") && contains(s, "#include \"kin_graph.inc.glsl\""));
    CHECK(contains(s, "const vec4 p_tint = kin_surface_param(0);"));
    CHECK(contains(s, "const float p_scale = kin_surface_param(1).x;"));
    CHECK(contains(s, "const vec2 p_shift = kin_surface_param(1).zw;"));
    CHECK(contains(s, "s.occlusion = 1.0;")); // the data texture's slot: kin's reading of it undone
    CHECK(contains(s, "kin_graph_sample(uBase, kin_uv(), true)"));
    CHECK(contains(s, "(kin_position() * vec3(p_scale))")); // the float widened to the vector
    CHECK(contains(s, "s.roughness = clamp(0.8, 0.045, 1.0);"));
    CHECK(contains(s, "s.base_color = max(n_color, vec3(0.0));"));
    CHECK(!contains(s, "n_unused") && !contains(s, "s.metallic")); // dead nodes and unwired outputs left out
    // Nodes in their inputs' order.
    CHECK(s.find("n_scaled") < s.find("n_rust") && s.find("n_rust") < s.find("const vec3 n_color"));
}

void errors() {
    const auto bad = compile_material_graph3d(graph(
        R"({"name": "albedo", "type": "texture", "slot": "base"},
           {"name": "again", "type": "texture", "slot": "base"},
           {"name": "uv", "type": "float"})",
        R"({"id": "a", "type": "add", "inputs": {"a": "b", "b": 1}},
           {"id": "b", "type": "add", "inputs": {"a": "a", "b": 1}},
           {"id": "c", "type": "warp"},
           {"id": "d", "type": "multiply", "inputs": {"a": 1}},
           {"id": "e", "type": "texture"},
           {"id": "f", "type": "add", "inputs": {"a": "albedo", "b": 1, "q": 2}},
           {"id": "g", "type": "add", "inputs": {"a": "nowhere.x", "b": [1, 2, 3, 4, 5]}},
           {"id": "h", "type": "add", "inputs": {"a": "uv.z", "b": 1}})",
        R"("base_color": "a", "metallic": "c", "roughness": "d", "occlusion": "e", "alpha": "f",
           "emission": "g", "normal": "h", "sheen": 1)"));
    CHECK(!bad.ok() && bad.surface.empty());
    CHECK(error_at(bad, "again", "two textures in the base slot"));
    CHECK(error_at(bad, "uv", "built-in"));
    CHECK(error_at(bad, "a", "cycle") || error_at(bad, "b", "cycle"));
    CHECK(error_at(bad, "c", "unknown node type 'warp'"));
    CHECK(error_at(bad, "d", "needs its input 'b'"));
    CHECK(error_at(bad, "e", "needs 'texture'"));
    CHECK(error_at(bad, "f", "has no input 'q'") && error_at(bad, "f", "a texture is not a value"));
    CHECK(error_at(bad, "g", "'nowhere' is no node"));
    CHECK(error_at(bad, "h", "not a swizzle of a vec2"));
    CHECK(error_at(bad, "sheen", "not an output"));
    CHECK(error_at(compile_material_graph3d("{\"schema\": \"x\"}"), "", "not a kin.material-graph/1"));
    CHECK(error_at(compile_material_graph3d("nonsense"), "", "not JSON"));
    // Material3D::shader_params' 64 floats: 16 vec4s fill them (the last at 60), a 17th is refused.
    std::string many;
    for (int i = 0; i < 16; ++i)
        many += std::string(i ? "," : "") + R"({"name": "v)" + std::to_string(i) + R"(", "type": "vec4"})";
    const auto full = compile_material_graph3d(graph(many, "", ""));
    CHECK(full.ok() && full.parameters.back().offset == 60);
    CHECK(error_at(compile_material_graph3d(graph(many + R"(, {"name": "v16", "type": "float"})", "", "")), "v16",
                   "64 floats"));
}

// Every node type, wired into one graph and compiled by glslc into every variant: the GLSL each writes compiles.
void every_node_compiles() {
    if (!shader_compiler_available()) {
        std::cout << "every_node_compiles: skipped (no glslc)\n";
        return;
    }
    std::string nodes, sum;
    int k = 0;
    for (const auto& n : material_graph_nodes3d()) {
        if (n.type == "object_to_world" || n.type == "world_to_object")
            continue; // (at a vertex only: offset_reads_the_object's)
        std::string inputs, extra;
        for (const auto& i : n.inputs) {
            if (i == "z" || i == "w")
                continue;                                       // combine's optional ones
            const std::string v = n.type == "append" ? "\"uv\"" // vec2s: 4 components
                                  : i == "direction" || i == "a" || i == "b" || i == "normal" || i == "position"
                                      ? "\"view\""
                                  : i == "uv"                     ? "\"uv\""
                                  : i == "center" || i == "speed" ? "[0.5, 0.5]"
                                                                  : "0.5";
            inputs += std::string(inputs.empty() ? "" : ", ") + "\"" + i + "\": " + v;
        }
        if (n.type == "constant")
            extra = R"(, "value": [0.1, 0.2, 0.3])";
        else if (n.type == "glsl")
            extra = R"(, "glsl": "x * 2.0", "result": "float", "inputs": {"x": "time"})";
        if (n.type == "texture" || n.type == "normal_map" || n.type == "triplanar" || n.type == "texture_equirect")
            extra = R"(, "texture": "map")";
        const std::string id = "n" + std::to_string(k++);
        nodes += std::string(nodes.empty() ? "" : ",\n") + R"({"id": ")" + id + R"(", "type": ")" + n.type + "\"" +
                 (n.type == "constant" || n.type == "glsl" ? "" : R"(, "inputs": {)" + inputs + "}") + extra + "}";
        nodes += R"(, {"id": "l)" + id + R"(", "type": "length", "inputs": {"x": ")" + id + R"("}})";
        sum = sum.empty() ? "\"l" + id + "\"" : sum;
        nodes += R"(, {"id": "s)" + id + R"(", "type": "add", "inputs": {"a": "l)" + id + R"(", "b": )" +
                 (k == 1 ? std::string("0") : "\"s" + ("n" + std::to_string(k - 2)) + "\"") + "}}";
    }
    const std::string last = "sn" + std::to_string(k - 1);
    const auto g = compile_material_graph3d(graph(R"({"name": "map", "type": "texture", "slot": "normal"})", nodes,
                                                  "\"emission\": \"" + last + "\", \"normal\": \"surface.normal\""));
    for (const auto& e : g.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(g.ok());
    const fs::path dir = fs::temp_directory_path() /
                         ("kin-graph-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const fs::path file = dir / "every.frag.glsl";
    for (u32 v = 0; v < surface_mask_variant3d; ++v) {
        {
            std::ofstream out(file);
            out << surface_shader_source(g.surface, SurfaceVariant3D(v));
        }
        std::string why;
        if (!compile_glsl(file, ShaderStage::Fragment, &why, surface_variant_defines(SurfaceVariant3D(v)))) {
            std::cerr << "variant " << v << ": " << why << '\n' << g.surface << '\n';
            CHECK(false);
        }
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    std::cout << "every node type (" << material_graph_nodes3d().size() << ") compiles in all "
              << surface_mask_variant3d << " shading variants\n";
}

// The graph docs/rendering_3d.md ("Material graphs") shows compiles, and so does its GLSL (with glslc).
void documented_example() {
    const auto g = compile_material_graph3d(R"__({"schema": "kin.material-graph/1", "name": "Rust",
 "parameters": [{"name": "tint", "type": "color", "default": [0.45, 0.2, 0.08, 1]},
                {"name": "scale", "type": "float", "default": 4},
                {"name": "albedo", "type": "texture", "slot": "base"}],
 "nodes": [{"id": "map", "type": "texture", "texture": "albedo"},
           {"id": "scaled", "type": "multiply", "inputs": {"a": "position", "b": "scale"}},
           {"id": "rust", "type": "fbm", "inputs": {"p": "scaled"}},
           {"id": "amount", "type": "smoothstep", "inputs": {"edge0": 0.45, "edge1": 0.6, "x": "rust"}},
           {"id": "color", "type": "mix", "inputs": {"a": "map.rgb", "b": "tint.rgb", "t": "amount"}},
           {"id": "rough", "type": "mix", "inputs": {"a": "surface.roughness", "b": 0.95, "t": "amount"}}],
 "outputs": {"base_color": "color", "roughness": "rough"}})__");
    for (const auto& e : g.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(g.ok() && g.name == "Rust" && g.parameters.size() == 3);
    if (!shader_compiler_available())
        return;
    const fs::path file = fs::temp_directory_path() / "kin-graph-documented.frag.glsl";
    {
        std::ofstream out(file);
        out << surface_shader_source(g.surface);
    }
    std::string why;
    const bool ok =
        compile_glsl(file, ShaderStage::Fragment, &why, surface_variant_defines(SurfaceVariant3D::LightsFog))
            .has_value();
    std::error_code ignored;
    fs::remove(file, ignored);
    if (!ok)
        std::cerr << why << '\n';
    CHECK(ok);
}

// The layout a cooked graph carries, and the cooked file: read back as written; damaged files refused. With glslc, a
// graph cooked now (every variant SPIR-V); with cmake's kin_cook_material_graph (KIN_TEST_COOKED_GRAPH), one cooked at
// build time, which reads back with its parameters, and a content folder's (KIN_TEST_COOKED_CONTENT).
void cooked() {
    const auto g = compile_material_graph3d(graph(
        R"({"name": "tint", "type": "color", "default": [0.25, 0.5, 0.75, 1]},
           {"name": "scale", "type": "vec2", "default": [2, 3]},
           {"name": "noise", "type": "texture", "slot": "occlusion", "role": "data"})",
        R"({"id": "t", "type": "texture", "texture": "noise"})", R"("base_color": "tint.rgb", "alpha": "t.r")"));
    CHECK(g.ok());
    std::string why;
    const auto back = read_material_graph_layout3d(write_material_graph_layout3d(g), &why);
    CHECK(back && back->name == "test" && back->parameters.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        const auto &a = g.parameters[i], &b = back->parameters[i];
        CHECK(a.name == b.name && a.type == b.type && a.offset == b.offset && a.size == b.size && a.value == b.value &&
              a.slot == b.slot && a.srgb == b.srgb && a.data == b.data);
    }
    CHECK(material_graph_params3d(*back) == material_graph_params3d(g));
    CookedMaterialGraph3D made{g, {}};
    for (auto& v : made.spirv)
        v = {3, 2, 35, 7, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto bytes = pack_cooked_material_graph3d(made);
    const auto unpacked = unpack_cooked_material_graph3d(bytes, &why);
    CHECK(unpacked && unpacked->graph.parameters.size() == 3 && unpacked->spirv[7] == made.spirv[7]);
    auto cut = bytes;
    cut.resize(cut.size() / 2); // the data cut, not only zlib's trailing check
    CHECK(!unpack_cooked_material_graph3d(cut, &why) && contains(why, "cut short"));
    auto flipped = bytes;
    flipped[flipped.size() / 2] ^= 0x40; // a byte changed: the payload's hash refuses it
    CHECK(!unpack_cooked_material_graph3d(flipped, &why));
    CHECK(bytes.size() < 400 && "the payload deflated (8 variants of 24 bytes and the layout)");
    CHECK(bytes[8] == 4 && !unpacked->spirv[u32(SurfaceVariant3D::Material)].empty());
    // Version 3 (16 floats of parameters): its merged-draw variants left out (they read the table's old stride).
    auto old = bytes;
    old[8] = 3; // the hash covers the payload, not the header
    const auto three = unpack_cooked_material_graph3d(old, &why);
    CHECK(three && three->spirv[7] == made.spirv[7] && !three->spirv[surface_mask_variant3d].empty());
    for (u32 i = u32(SurfaceVariant3D::Material); i <= u32(SurfaceVariant3D::MaterialLightsFog); ++i)
        CHECK(three->spirv[i].empty());
    auto wrong = bytes;
    wrong[0] = 'X';
    CHECK(!unpack_cooked_material_graph3d(wrong));
    CHECK(!read_material_graph_layout3d("{\"schema\": \"x\"}"));
    // Parameters anywhere in the 64 floats (a cooked graph's past the first 16 once refused it), none past them.
    const auto at = [](u32 offset) {
        return "{\"schema\": \"kin.material-graph-layout/1\", \"parameters\": [{\"name\": \"far\", \"type\": "
               "\"vec4\", \"offset\": " +
               std::to_string(offset) + ", \"size\": 4, \"value\": [1, 2, 3, 4]}]}";
    };
    const auto far = read_material_graph_layout3d(at(surface_param_count3d - 4), &why);
    CHECK(far && far->parameters.size() == 1 && far->parameters[0].offset == surface_param_count3d - 4 &&
          far->parameters[0].value[3] == 4);
    CHECK(!read_material_graph_layout3d(at(surface_param_count3d - 3), &why) && contains(why, "beyond"));
    if (shader_compiler_available()) {
        const auto now = cook_material_graph3d(graph("", "", R"("roughness": 0.25)"), &why);
        CHECK(now.has_value());
        for (u32 i = 0; i < surface_mask_variant3d; ++i) {
            const auto& v = now->spirv[i];
            CHECK(v.size() > 20 && v[0] == 3 && v[1] == 2 && v[2] == 35 && v[3] == 7);
        }
        for (u32 i = surface_mask_variant3d; i < surface_variant_count3d; ++i)
            CHECK(now->spirv[i].empty()); // no alpha: no cut-out variants
        CHECK(!cook_material_graph3d(graph("", "", R"("roughness": "nowhere")"), &why) && contains(why, "nowhere"));
    }
#ifdef KIN_TEST_COOKED_GRAPH
    std::ifstream in(KIN_TEST_COOKED_GRAPH, std::ios::binary);
    const std::vector<u8> file{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const auto built = unpack_cooked_material_graph3d(file, &why);
    if (!built)
        std::cerr << why << '\n';
    CHECK(built && built->graph.parameters.size() == 1 && built->graph.parameters[0].name == "tint");
    std::cout << "the graph cooked at build time reads back\n";
#endif
#ifdef KIN_TEST_COOKED_CONTENT
    // A content folder's graphs (cmake's kin_cook_material_graphs), each cooked beside it.
    std::ifstream folder_in(fs::path(KIN_TEST_COOKED_CONTENT) / "sub/tinted.kingraph.spv", std::ios::binary);
    const std::vector<u8> folder_file{std::istreambuf_iterator<char>{folder_in}, std::istreambuf_iterator<char>{}};
    const auto beside = unpack_cooked_material_graph3d(folder_file, &why);
    CHECK(beside && beside->graph.parameters.size() == 1 && beside->graph.parameters[0].name == "tint");
    std::cout << "a content folder's graph cooked beside it reads back\n";
#endif
}

// The alpha in the cut-out passes: a graph's alpha of what they have (UV, position, vertex alpha, time, parameters,
// maps, noise) is compiled for them too, and its cut-out variants compile; one of what they don't have (the view, a
// fresnel) compiles with a warning naming its node, and no cut-out variants.
void cutouts() {
    const auto dissolve = compile_material_graph3d(graph(
        R"({"name": "amount", "type": "float", "default": 0.5},
           {"name": "albedo", "type": "texture", "slot": "base"})",
        R"({"id": "map", "type": "texture", "texture": "albedo"},
           {"id": "noise", "type": "noise_value", "inputs": {"p": "position", "scale": 3}},
           {"id": "wave", "type": "sin", "inputs": {"x": "time"}},
           {"id": "edge", "type": "add", "inputs": {"a": "amount", "b": "wave"}},
           {"id": "keep", "type": "step", "inputs": {"edge": "edge", "x": "noise"}},
           {"id": "alpha", "type": "multiply", "inputs": {"a": "keep", "b": "surface.alpha"}})",
        R"("alpha": "alpha", "base_color": "map.rgb")"));
    for (const auto& e : dissolve.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(dissolve.ok() && dissolve.cutout && dissolve.warnings.empty());
    CHECK(surface_has_alpha(dissolve.surface) && contains(dissolve.surface, "#ifdef KIN_SURFACE_MASK"));
    CHECK(contains(dissolve.surface, "material.cutoff.z") &&
          contains(dissolve.surface, "(vertex_alpha * texture(uBase, uv).a)"));
    const auto rim = compile_material_graph3d(
        graph("", R"({"id": "edge", "type": "fresnel"}, {"id": "a", "type": "one_minus", "inputs": {"x": "edge"}})",
              R"("alpha": "a")"));
    CHECK(rim.ok() && !rim.cutout && !surface_has_alpha(rim.surface) && rim.warnings.size() == 1 &&
          rim.warnings[0].node == "edge" && contains(rim.warnings[0].message, "base map's alpha"));
    const auto plain = compile_material_graph3d(graph("", "", R"("roughness": 0.5)"));
    CHECK(plain.ok() && !plain.cutout && plain.warnings.empty());
    if (!shader_compiler_available())
        return;
    std::string why;
    const auto cooked =
        cook_material_graph3d(graph(R"({"name": "amount", "type": "float", "default": 0.5})",
                                    R"({"id": "noise", "type": "noise_value", "inputs": {"p": "position", "scale": 3}},
           {"id": "keep", "type": "step", "inputs": {"edge": "amount", "x": "noise"}})",
                                    R"("alpha": "keep")"),
                              &why);
    if (!cooked)
        std::cerr << why << '\n';
    CHECK(cooked.has_value() && cooked->graph.cutout);
    for (u32 i = 0; i < surface_vertex_variant3d; ++i)
        CHECK(cooked->spirv[i].size() > 20);
    std::cout << "a graph's alpha compiles into the 5 cut-out passes\n";
}

// Parameters packed in the first place free (a float in a vec3's fourth); textures past the five slots in the graph
// maps' layers (Material3D::graph_maps): read from the array, kept in the cooked layout, refused twice in a layer or
// past the last, and not in the cut-out passes (their alpha falls back to the base map's, said). With glslc, every
// variant compiles.
void layers_and_packing() {
    const auto packed = compile_material_graph3d(graph(
        R"({"name": "a", "type": "vec3", "default": [1, 2, 3]}, {"name": "b", "type": "float", "default": 4},
           {"name": "c", "type": "float", "default": 5})",
        "", R"("base_color": "a", "roughness": "b", "metallic": "c")"));
    CHECK(packed.ok() && packed.parameters[0].offset == 0 && packed.parameters[1].offset == 3 &&
          packed.parameters[2].offset == 4);
    CHECK(material_graph_params3d(packed)[3] == 4 && material_graph_params3d(packed)[4] == 5);
    const std::string params = R"({"name": "albedo", "type": "texture", "slot": "base"},
           {"name": "moss", "type": "texture", "slot": "layers", "layer": 0, "color_space": "srgb"},
           {"name": "foam", "type": "texture", "slot": "layers", "layer": 3, "color_space": "linear"})";
    const std::string nodes = R"({"id": "m", "type": "texture", "texture": "moss"},
           {"id": "f", "type": "texture", "texture": "foam"},
           {"id": "t", "type": "triplanar", "texture": "moss"},
           {"id": "n", "type": "normal_map", "texture": "foam"},
           {"id": "mix", "type": "mix", "inputs": {"a": "m.rgb", "b": "t.rgb", "t": "f.r"}})";
    const std::string text = graph(params, nodes, R"("base_color": "mix", "normal": "n")");
    const auto g = compile_material_graph3d(text);
    for (const auto& e : g.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(g.ok() && g.parameters.size() == 3 && !g.parameters[0].layered() && g.parameters[1].layer == 0 &&
          g.parameters[2].layer == 3 && g.parameters[1].slot == MaterialMap3D::Count);
    CHECK(contains(g.surface, "kin_graph_sample(uGraphMaps, 0.0, kin_uv(), true)") &&
          contains(g.surface, "kin_graph_sample(uGraphMaps, 3.0, kin_uv(), false)") &&
          contains(g.surface, "kin_graph_triplanar(uGraphMaps, 0.0, "));
    std::string why;
    const auto back = read_material_graph_layout3d(write_material_graph_layout3d(g), &why);
    CHECK(back && back->parameters[2].layer == 3 && back->parameters[1].layered() && !back->parameters[0].layered());
    CHECK(!compile_material_graph3d(graph(R"({"name": "a", "type": "texture", "slot": "layers", "layer": 1},
                                            {"name": "b", "type": "texture", "slot": "layers", "layer": 1})",
                                          "", ""))
               .ok());
    CHECK(!compile_material_graph3d(graph(R"({"name": "a", "type": "texture", "slot": "layers", "layer": 16})", "", ""))
               .ok());
    const auto cut =
        compile_material_graph3d(graph(R"({"name": "mask", "type": "texture", "slot": "layers", "layer": 0})",
                                       R"({"id": "m", "type": "texture", "texture": "mask"})", R"("alpha": "m.r")"));
    CHECK(cut.ok() && !cut.cutout && !cut.warnings.empty() && contains(cut.warnings[0].message, "base map's alpha"));
    if (!shader_compiler_available())
        return;
    const auto cooked = cook_material_graph3d(text, &why);
    if (!cooked)
        std::cerr << why << '\n';
    CHECK(cooked.has_value());
    for (u32 i = 0; i < surface_mask_variant3d; ++i)
        CHECK(cooked->spirv[i].size() > 20);
    std::cout << "a graph's maps past the five slots compile into every variant\n";
}

// An offset (World Position Offset): a vertex function of what a vertex has (its position, normal, UV, colour, the
// time, parameters and maps), compiled for the vertex variants; one that reads what a vertex has not (the surface's
// normal) is not drawn, warned. With glslc, every vertex variant compiles.
void offsets() {
    const std::string text = graph(
        R"({"name": "height", "type": "float", "default": 0.1},
           {"name": "waves", "type": "texture", "slot": "layers", "layer": 0})",
        R"({"id": "w", "type": "texture", "texture": "waves", "inputs": {"uv": "uv"}},
           {"id": "phase", "type": "add", "inputs": {"a": "position.x", "b": "time"}},
           {"id": "s", "type": "sin", "inputs": {"x": "phase"}},
           {"id": "lift", "type": "multiply", "inputs": {"a": "s", "b": "height"}},
           {"id": "up", "type": "multiply", "inputs": {"a": "normal", "b": "lift"}},
           {"id": "moved", "type": "add", "inputs": {"a": "up", "b": "w.rgb"}})",
        R"("base_color": [0.2, 0.4, 0.8], "offset": "moved")");
    const auto g = compile_material_graph3d(text);
    for (const auto& e : g.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(g.ok() && g.offset && g.warnings.empty());
    CHECK(contains(g.surface, "#if defined(KIN_SURFACE_VERTEX)") &&
          contains(g.surface, "vec3 kin_surface_offset() {") && contains(g.surface, "kin_v_time") &&
          contains(g.surface, "kin_graph_sample(uGraphMaps, 0.0, kin_uv(), false)"));
    const auto inside = g.surface.find("vec3 kin_surface_offset() {");
    CHECK(g.surface.find("frame.time.x", inside) > g.surface.find("#else", inside)); // (its time is the vertex's)
    const auto bad = compile_material_graph3d(graph(
        "", R"({"id": "o", "type": "multiply", "inputs": {"a": "surface.normal", "b": 0.1}})", R"("offset": "o")"));
    CHECK(bad.ok() && !bad.offset && bad.warnings.size() == 1 &&
          contains(bad.warnings[0].message, "offset is not drawn"));
    const auto none = compile_material_graph3d(graph("", "", R"("roughness": 0.5)"));
    CHECK(none.ok() && !none.offset && !contains(none.surface, "KIN_SURFACE_VERTEX"));
    if (!shader_compiler_available())
        return;
    std::string why;
    const auto cooked = cook_material_graph3d(text, &why);
    if (!cooked)
        std::cerr << why << '\n';
    CHECK(cooked.has_value());
    for (u32 i = surface_vertex_variant3d; i < surface_variant_count3d; ++i)
        CHECK(cooked->spirv[i].size() > 20);
    const auto plain = cook_material_graph3d(graph("", "", R"("roughness": 0.5)"), &why);
    CHECK(plain && plain->spirv[surface_vertex_variant3d].empty());
    std::cout << "a graph's offset compiles into the " << surface_variant_count3d - surface_vertex_variant3d
              << " vertex variants\n";
}

// The object's space in an offset: the copy's origin and its model matrix (a point local to it, swayed by its height
// above the origin, back to the world); in a fragment's output, its origin (a flat varying), its matrix refused (the
// fragments don't have it). With glslc, every vertex variant compiles.
void offset_reads_the_object() {
    const std::string text = graph("",
                                   R"({"id": "local", "type": "world_to_object", "inputs": {"x": "position"}},
           {"id": "sway", "type": "multiply", "inputs": {"a": "local.y", "b": [0.1, 0, 0]}},
           {"id": "turned", "type": "object_to_world", "inputs": {"x": "sway", "w": 0}},
           {"id": "far", "type": "distance", "inputs": {"a": "position", "b": "object_position"}},
           {"id": "moved", "type": "multiply", "inputs": {"a": "turned", "b": "far"}})",
                                   R"("roughness": 0.5, "offset": "moved")");
    const auto g = compile_material_graph3d(text);
    for (const auto& e : g.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(g.ok() && g.offset && g.warnings.empty());
    CHECK(contains(g.surface, "kin_graph_world_to_object(kin_position(), 1.0)") &&
          contains(g.surface, "kin_graph_object_to_world(") && contains(g.surface, "kin_object_position()"));
    const auto origin = compile_material_graph3d(graph("", "", R"("base_color": "object_position")"));
    CHECK(origin.ok() && contains(origin.surface, "s.base_color = max(kin_object_position()"));
    const auto bad = compile_material_graph3d(
        graph("", R"({"id": "o", "type": "object_to_world", "inputs": {"x": [0, 1, 0]}})", R"("emission": "o")"));
    CHECK(!bad.ok() && contains(bad.errors.front().message, "only at a vertex"));
    if (!shader_compiler_available())
        return;
    std::string why;
    const auto cooked = cook_material_graph3d(text, &why);
    if (!cooked)
        std::cerr << why << '\n';
    CHECK(cooked.has_value());
    for (u32 i = surface_vertex_variant3d; i < surface_variant_count3d; ++i)
        CHECK(cooked->spirv[i].size() > 20);
}

// A point in the object's own space: a pattern that moves and turns with the copy (noise on a rolling ball). In a
// fragment, the mesh's vertex shader's varying; at a vertex (an offset), the world position through the inverse model
// matrix. Not in the cut-out passes (as the origin isn't). With glslc, its fragment and vertex variants compile.
void reads_the_local_position() {
    const std::string text = graph("",
                                   R"({"id": "scaled", "type": "multiply", "inputs": {"a": "local_position", "b": 4}},
           {"id": "rust", "type": "fbm", "inputs": {"p": "scaled"}},
           {"id": "lift", "type": "multiply", "inputs": {"a": "local_position.y", "b": [0, 0.01, 0]}})",
                                   R"("base_color": "rust", "offset": "lift")");
    const auto g = compile_material_graph3d(text);
    for (const auto& e : g.errors)
        std::cerr << e.node << ": " << e.message << '\n';
    CHECK(g.ok() && g.offset && g.warnings.empty());
    CHECK(contains(g.surface, "kin_local_position()"));
    if (!shader_compiler_available())
        return;
    std::string why;
    const auto cooked = cook_material_graph3d(text, &why);
    if (!cooked)
        std::cerr << why << '\n';
    CHECK(cooked.has_value());
    // Its fragment variants and its offset's vertex ones (no alpha: no cut-out variants).
    for (u32 i = 0; i < surface_mask_variant3d; ++i)
        CHECK(cooked->spirv[i].size() > 20);
    for (u32 i = surface_vertex_variant3d; i < surface_variant_count3d; ++i)
        CHECK(cooked->spirv[i].size() > 20);
}

// What the GI scene's stand-in needs (SurfaceTraits3D): an emission output of its own unless a constant black; the
// vertex colour in its colour and alpha where they read it (through any node, a glsl node's vColor, the stock
// surface's fields) or are not outputs; kept in the layout, and defaults for a layout from before them.
void traits() {
    const auto of = [](const std::string& nodes, const std::string& outputs) {
        const auto g = compile_material_graph3d(graph(R"({"name": "albedo", "type": "texture", "slot": "base"})",
                                                      R"({"id": "map", "type": "texture", "texture": "albedo"})" +
                                                          std::string(nodes.empty() ? "" : ", ") + nodes,
                                                      outputs));
        for (const auto& e : g.errors)
            std::cerr << e.node << ": " << e.message << '\n';
        CHECK(g.ok());
        return g.traits;
    };
    const SurfaceTraits3D stock = of("", R"("roughness": 0.5)");
    CHECK(!stock.emission && stock.vertex_color && stock.vertex_alpha);
    const SurfaceTraits3D own = of("", R"("base_color": "map.rgb", "alpha": "map.a", "emission": [0, 0, 0])");
    CHECK(!own.emission && !own.vertex_color && !own.vertex_alpha);
    // Two steps from the vertex colour; a node the alpha shares with the colour, which doesn't read it.
    const SurfaceTraits3D tinted =
        of(R"({"id": "tint", "type": "multiply", "inputs": {"a": "map.rgb", "b": "vertex_color.rgb"}},
           {"id": "twice", "type": "multiply", "inputs": {"a": "tint", "b": 2}},
           {"id": "faded", "type": "multiply", "inputs": {"a": "map.a", "b": "vertex_color.a"}})",
           R"("base_color": "twice", "alpha": "faded", "emission": "map.rgb")");
    CHECK(tinted.emission && tinted.vertex_color && tinted.vertex_alpha);
    const SurfaceTraits3D shared = of(R"({"id": "grey", "type": "multiply", "inputs": {"a": "map.r", "b": 0.5}},
           {"id": "faded", "type": "multiply", "inputs": {"a": "grey", "b": "vertex_color.a"}})",
                                      R"("base_color": "grey", "alpha": "faded", "emission": 2)");
    CHECK(shared.emission && !shared.vertex_color && shared.vertex_alpha);
    const SurfaceTraits3D raw = of(R"({"id": "g", "type": "glsl", "glsl": "vColor.rgb * 0.5", "result": "vec3"})",
                                   R"("base_color": "g", "alpha": 1)");
    CHECK(raw.vertex_color && !raw.vertex_alpha);
    const SurfaceTraits3D kept =
        of(R"({"id": "dim", "type": "multiply", "inputs": {"a": "surface.base_color", "b": 0.5}})",
           R"("base_color": "dim", "alpha": "surface.alpha")");
    CHECK(kept.vertex_color && kept.vertex_alpha);
    // The layout keeps them; one from before them reads as the defaults.
    auto g = compile_material_graph3d(graph("", "", R"("base_color": [1, 0, 0], "alpha": 1, "emission": [0, 1, 0])"));
    CHECK(g.ok() && g.traits.emission && !g.traits.vertex_color && !g.traits.vertex_alpha);
    const auto back = read_material_graph_layout3d(write_material_graph_layout3d(g));
    CHECK(back && back->traits == g.traits);
    const auto old = read_material_graph_layout3d(R"({"schema": "kin.material-graph-layout/1", "parameters": []})");
    CHECK(old && old->traits == SurfaceTraits3D{});
}

// Chains of nodes as deep as 1024 compile, on a stack of their own (a 1 MB main thread's, Windows', overflowed in a
// few hundred), in the alpha's and the offset's compiles too; deeper ones are an error naming a node, not a crash.
void deep_chains() {
    const auto chain = [](int n, const std::string& output) {
        std::string nodes = R"({"id": "n0", "type": "add", "inputs": {"a": "uv.x", "b": 0.001}})";
        for (int i = 1; i < n; ++i)
            nodes += R"(, {"id": "n)" + std::to_string(i) + R"(", "type": "add", "inputs": {"a": "n)" +
                     std::to_string(i - 1) + R"(", "b": 0.001}})";
        return compile_material_graph3d(graph("", nodes, "\"" + output + "\": \"n" + std::to_string(n - 1) + "\""));
    };
    CHECK(chain(1024, "roughness").ok());
    CHECK(chain(1024, "alpha").cutout);
    CHECK(chain(1024, "offset").offset);
    CHECK(error_at(chain(1025, "roughness"), "n0", "nodes chained more than 1024 deep"));
    CHECK(error_at(chain(20000, "roughness"), "n18975", "nodes chained more than 1024 deep"));
}

} // namespace

int main() {
    compiles();
    errors();
    every_node_compiles();
    documented_example();
    cooked();
    cutouts();
    layers_and_packing();
    offsets();
    offset_reads_the_object();
    reads_the_local_position();
    traits();
    deep_chains();
    std::cout << "material graph tests passed\n";
    return 0;
}
