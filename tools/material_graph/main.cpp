// kin_material_graph: material graphs (kin/render3d/material_graph3d.hpp) at the command line.
//
//   kin_material_graph check <graph>            its errors (each naming its node), else its parameters
//   kin_material_graph glsl <graph> <out>       its surface shader's GLSL (for kin_compile_surface_shader)
//   kin_material_graph cook <graph> [<out>]     cooked: its layout and every variant's SPIR-V, by glslc, in one file
//                                               (default <graph>.spv: where Assets3D::load_material_graph looks)
//   kin_material_graph nodes                    the node types and their inputs
//
// Exit status 1 when the graph doesn't compile, 2 for a usage error.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <kin/render3d/material_graph3d.hpp>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int usage() {
    std::cerr << "usage: kin_material_graph check <graph> | glsl <graph> <out> | cook <graph> [<out>] | nodes\n";
    return 2;
}

std::string read(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = bool(in);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

const char* type_name(kin::GraphType3D t) {
    switch (t) {
    case kin::GraphType3D::Float:
        return "float";
    case kin::GraphType3D::Vec2:
        return "vec2";
    case kin::GraphType3D::Vec3:
        return "vec3";
    case kin::GraphType3D::Vec4:
        return "vec4";
    case kin::GraphType3D::Texture:
        return "texture";
    }
    return "?";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return usage();
    const std::string_view command = argv[1];
    if (command == "nodes") {
        for (const auto& n : kin::material_graph_nodes3d()) {
            std::cout << n.type << "(";
            for (size_t i = 0; i < n.inputs.size(); ++i)
                std::cout << (i ? ", " : "") << n.inputs[i];
            std::cout << "): " << n.summary << '\n';
        }
        return 0;
    }
    if (argc < 3 || (command != "check" && command != "glsl" && command != "cook") || (command == "glsl" && argc < 4))
        return usage();
    const std::filesystem::path source = argv[2];
    bool ok = false;
    const std::string text = read(source, ok);
    if (!ok) {
        std::cerr << "kin_material_graph: cannot read " << source.string() << '\n';
        return 1;
    }
    if (command == "cook") {
        std::string why;
        const auto cooked = kin::cook_material_graph3d(text, &why);
        if (!cooked) {
            // An error a line, each naming the graph (as check's do): where a build's output points.
            std::istringstream lines(why.empty() ? std::string("it can't be cooked") : why);
            for (std::string line; std::getline(lines, line);)
                if (!line.empty())
                    std::cerr << source.string() << ": " << line << '\n';
            return 1;
        }
        const std::filesystem::path out =
            argc > 3 ? std::filesystem::path(argv[3]) : std::filesystem::path(source.string() + ".spv");
        if (out.has_parent_path())
            std::filesystem::create_directories(out.parent_path());
        const auto bytes = kin::pack_cooked_material_graph3d(*cooked);
        std::ofstream file(out, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!file) {
            std::cerr << "kin_material_graph: cannot write " << out.string() << '\n';
            return 1;
        }
        return 0;
    }
    const auto graph = kin::compile_material_graph3d(text);
    if (!graph.ok()) {
        for (const auto& e : graph.errors)
            std::cerr << source.string() << ": " << (e.node.empty() ? "" : e.node + ": ") << e.message << '\n';
        return 1;
    }
    if (command == "glsl") {
        std::ofstream file(argv[3], std::ios::binary);
        file << graph.surface;
        return file ? 0 : 1;
    }
    std::cout << (graph.name.empty() ? source.filename().string() : graph.name) << ": compiles\n";
    for (const auto& p : graph.parameters) {
        std::cout << "  " << p.name << ": " << type_name(p.type);
        if (p.type == kin::GraphType3D::Texture)
            std::cout << " (slot " << int(p.slot) << (p.srgb ? ", sRGB" : ", linear") << (p.data ? ", data" : "")
                      << ")";
        else
            std::cout << " (shader_params[" << p.offset << "..])";
        std::cout << '\n';
    }
    return 0;
}
