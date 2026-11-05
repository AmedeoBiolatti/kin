// Material graphs (kin/render3d/material_graph3d.hpp), on the CPU: the compiler's GLSL (parameters packed, dead nodes
// left out, types cast, outputs as kin takes them), its errors naming their nodes, and, with glslc, every node type
// compiled into every variant of kin's mesh shader.
#include <kin/render3d/material_graph3d.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/shader_compiler.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    return R"({"schema": "kin.material-graph/1", "name": "test", "parameters": [)" + parameters +
           R"(], "nodes": [)" + nodes + R"(], "outputs": {)" + outputs + "}}";
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
    std::string many;
    for (int i = 0; i < 5; ++i)
        many += std::string(i ? "," : "") + R"({"name": "v)" + std::to_string(i) + R"(", "type": "vec4"})";
    CHECK(error_at(compile_material_graph3d(graph(many, "", "")), "v4", "16 floats"));
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
        std::string inputs, extra;
        for (const auto& i : n.inputs) {
            if (i == "z" || i == "w")
                continue; // combine's optional ones
            const std::string v = i == "direction" || i == "a" || i == "b" || i == "normal" || i == "position" ? "\"view\""
                                  : i == "uv"                       ? "\"uv\""
                                  : i == "center" || i == "speed"   ? "[0.5, 0.5]"
                                                                    : "0.5";
            inputs += std::string(inputs.empty() ? "" : ", ") + "\"" + i + "\": " + v;
        }
        if (n.type == "constant")
            extra = R"(, "value": [0.1, 0.2, 0.3])";
        else if (n.type == "glsl")
            extra = R"(, "glsl": "x * 2.0", "result": "float", "inputs": {"x": "time"})";
        if (n.type == "texture" || n.type == "normal_map" || n.type == "triplanar")
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
    {
        std::ofstream out(file);
        out << surface_shader_source(g.surface);
    }
    for (u32 v = 0; v < surface_variant_count3d; ++v) {
        std::string why;
        if (!compile_glsl(file, ShaderStage::Fragment, &why, surface_variant_defines(SurfaceVariant3D(v)))) {
            std::cerr << "variant " << v << ": " << why << '\n' << g.surface << '\n';
            CHECK(false);
        }
    }
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    std::cout << "every node type (" << material_graph_nodes3d().size() << ") compiles in all "
              << surface_variant_count3d << " variants\n";
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
    const bool ok = compile_glsl(file, ShaderStage::Fragment, &why, surface_variant_defines(SurfaceVariant3D::LightsFog))
                        .has_value();
    std::error_code ignored;
    fs::remove(file, ignored);
    if (!ok)
        std::cerr << why << '\n';
    CHECK(ok);
}

// The layout a cooked graph carries, and the cooked file: read back as written; damaged files refused. With glslc, a
// graph cooked now (every variant SPIR-V); with cmake's kin_cook_material_graph (KIN_TEST_COOKED_GRAPH), one cooked at
// build time, which reads back with its parameters.
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
    auto wrong = bytes;
    wrong[0] = 'X';
    CHECK(!unpack_cooked_material_graph3d(wrong));
    CHECK(!read_material_graph_layout3d("{\"schema\": \"x\"}"));
    if (shader_compiler_available()) {
        const auto now = cook_material_graph3d(graph("", "", R"("roughness": 0.25)"), &why);
        CHECK(now.has_value());
        for (const auto& v : now->spirv)
            CHECK(v.size() > 20 && v[0] == 3 && v[1] == 2 && v[2] == 35 && v[3] == 7);
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
}

} // namespace

int main() {
    compiles();
    errors();
    every_node_compiles();
    documented_example();
    cooked();
    std::cout << "material graph tests passed\n";
    return 0;
}
