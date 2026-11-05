#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <kin/assets/content.hpp>
#include <kin/platform/log.hpp>
#include <kin/render3d/renderer3d.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <optional>
#include <string>
#include <vector>

extern "C" {
float* kin_hdr_load(const unsigned char* bytes, int size, int* width, int* height);
void kin_hdr_free(float* pixels);
}

namespace kin {

Result3D<Environment3D> Renderer3D::create_environment(Vec2i size, std::span<const f32> rgb) {
    auto value = create_environment_operation(size, rgb);
    return result3d(std::move(value), _errors);
}

Environment3D Renderer3D::create_environment_operation(Vec2i size, std::span<const f32> rgb) {
    _errors.clear();
    if (!_backend || size.x < 8 || size.y < 4 ||
        rgb.size() != static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 3) {
        fail3d(_errors, "Renderer3D::create_environment",
               _backend ? "invalid environment dimensions/pixel count" : "3D is not available");
        return {};
    }
    std::shared_ptr<IEnvironmentBackend3D> backend = _backend->create_environment(size, rgb);
    if (!backend)
        fail3d(_errors, "Renderer3D::create_environment", "device refused environment textures");
    return backend ? Environment3D{std::move(backend), size} : Environment3D{};
}

Result3D<Environment3D> Renderer3D::load_environment(const std::filesystem::path& path) {
    auto value = load_environment_operation(path);
    return result3d(std::move(value), _errors);
}

Environment3D Renderer3D::load_environment_operation(const std::filesystem::path& path) {
    _errors.clear();
    std::string why;
    Environment3D environment;
    if (!_backend) {
        why = "3D is not available";
    } else if (const std::optional<std::string> bytes = read_content_file(path); !bytes) {
        why = "cannot read " + path.string();
    } else {
        int w = 0, h = 0;
        float* pixels = kin_hdr_load(reinterpret_cast<const unsigned char*>(bytes->data()),
                                     static_cast<int>(bytes->size()), &w, &h);
        if (!pixels) {
            why = path.string() + ": not a Radiance .hdr picture";
        } else {
            environment = create_environment_operation(
                {w, h}, {pixels, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3});
            kin_hdr_free(pixels);
            if (!environment.valid()) {
                why = path.string() + ": cannot make an environment of it (compute shaders, or a picture under 8 x 4)";
            }
        }
    }
    if (!environment.valid()) {
        KIN_LOG_WARN_F("render", "3D: environment not loaded",
                       (LogFields{{.name = "path", .value = path.string()}, {.name = "reason", .value = why}}));
    }
    if (!environment)
        fail3d(_errors, "Renderer3D::load_environment(" + path.string() + ")", why);
    else
        _errors.clear();
    return environment;
}

Result3D<MaterialShader3D> Renderer3D::create_material_shader(std::span<const u8> spirv) {
    auto value = create_material_shader_operation(spirv);
    return result3d(std::move(value), _errors);
}

MaterialShader3D Renderer3D::create_material_shader_operation(std::span<const u8> spirv) {
    _errors.clear();
    if (!_backend || spirv.empty()) {
        fail3d(_errors, "Renderer3D::create_material_shader", _backend ? "empty SPIR-V" : "3D is not available");
        return {};
    }
    MaterialShader3D shader{_backend->create_material_shader(spirv)};
    if (!shader)
        fail3d(_errors, "Renderer3D::create_material_shader", "device refused shader");
    return shader;
}

Result3D<MaterialShader3D> Renderer3D::compile_material_shader(const std::filesystem::path& source) {
    auto value = compile_material_shader_operation(source);
    return result3d(std::move(value), _errors);
}

MaterialShader3D Renderer3D::compile_material_shader_operation(const std::filesystem::path& source) {
    _errors.clear();
    std::string why;
    MaterialShader3D shader;
    if (!_backend) {
        why = "3D is not available";
    } else {
#if KIN_HAS_GPU_RENDERER
        if (const auto spirv = compile_glsl(source, ShaderStage::Fragment, &why)) {
            shader = create_material_shader_operation(*spirv);
            if (!shader)
                why = "the device refused the shader";
        }
#else
        why = "runtime SPIR-V compilation is not available on this renderer; export a target-specific shader asset";
#endif
    }
    if (!shader.valid()) {
        KIN_LOG_WARN_F("render", "3D: material shader not made",
                       (LogFields{{.name = "path", .value = source.string()}, {.name = "reason", .value = why}}));
    }
    if (!shader)
        fail3d(_errors, "Renderer3D::compile_material_shader(" + source.string() + ")", why);
    else
        _errors.clear();
    return shader;
}

std::string surface_shader_source(std::string_view surface) {
    // KIN_SURFACE before the first half: its Material block then holds the surface's parameters. The surface's own
    // lines are numbered from 1, so glslc's messages point into its file.
    std::string source = "#version 450 core\n"
                         "#define KIN_SURFACE kin_surface\n"
                         "#include \"kin_mesh_surface.inc.glsl\"\n"
                         "#line 1\n";
    source += surface;
    if (!surface.empty() && surface.back() != '\n')
        source += '\n';
    source += "#include \"kin_mesh_main.inc.glsl\"\n";
    return source;
}

std::span<const char* const> surface_variant_defines(SurfaceVariant3D variant) {
    // As mesh.frag, mesh_fog.frag, mesh_light.frag ... mesh_velocity_light_fog.frag define them.
    static constexpr std::array<const char*, 5> all{"KIN_VELOCITY", "KIN_LIGHTS", "KIN_GI", "KIN_LIGHT_GRID",
                                                    "KIN_FOG"};
    static constexpr std::array<const char*, 4> lights_fog{"KIN_LIGHTS", "KIN_GI", "KIN_LIGHT_GRID", "KIN_FOG"};
    static constexpr std::array<const char*, 2> velocity_fog{"KIN_VELOCITY", "KIN_FOG"};
    const std::span<const char* const> every{all}, lit{lights_fog}, moving{velocity_fog};
    switch (variant) {
    case SurfaceVariant3D::Plain:
        return {};
    case SurfaceVariant3D::Fog:
        return lit.subspan(3, 1);
    case SurfaceVariant3D::Lights:
        return lit.first(3);
    case SurfaceVariant3D::LightsFog:
        return lit;
    case SurfaceVariant3D::Velocity:
        return moving.first(1);
    case SurfaceVariant3D::VelocityFog:
        return moving;
    case SurfaceVariant3D::VelocityLights:
        return every.first(4);
    case SurfaceVariant3D::VelocityLightsFog:
        return every;
    case SurfaceVariant3D::Count:
        break;
    }
    return {};
}

Result3D<SurfaceShader3D>
Renderer3D::create_surface_shader(std::span<const std::span<const u8>, surface_variant_count3d> spirv) {
    auto value = create_surface_shader_operation(spirv);
    return result3d(std::move(value), _errors);
}

SurfaceShader3D
Renderer3D::create_surface_shader_operation(std::span<const std::span<const u8>, surface_variant_count3d> spirv) {
    _errors.clear();
    if (!_backend) {
        fail3d(_errors, "Renderer3D::create_surface_shader", "3D is not available");
        return {};
    }
    for (const auto& variant : spirv)
        if (variant.empty()) {
            fail3d(_errors, "Renderer3D::create_surface_shader", "empty SPIR-V");
            return {};
        }
    SurfaceShader3D shader{_backend->create_surface_shader(spirv)};
    if (!shader)
        fail3d(_errors, "Renderer3D::create_surface_shader", "the backend refused the surface shader");
    return shader;
}

Result3D<SurfaceShader3D> Renderer3D::compile_surface_shader(const std::filesystem::path& source) {
    auto value = compile_surface_shader_operation(source);
    return result3d(std::move(value), _errors);
}

SurfaceShader3D Renderer3D::compile_surface_shader_operation(const std::filesystem::path& source) {
    _errors.clear();
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        fail3d(_errors, "Renderer3D::compile_surface_shader(" + source.string() + ")", "the surface could not be read");
        return {};
    }
    const std::string surface{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    return compile_surface_text_operation(surface, "Renderer3D::compile_surface_shader(" + source.string() + ")");
}

SurfaceShader3D Renderer3D::compile_surface_text_operation(const std::string& surface, const std::string& label) {
    _errors.clear();
    std::string why;
    SurfaceShader3D shader;
    if (!_backend) {
        why = "3D is not available";
    } else {
        // glslc compiles files: the wrapped source goes to one beside the others it makes.
        static std::atomic<u32> counter{0};
        const std::filesystem::path wrapped = std::filesystem::temp_directory_path() /
                                              ("kin-surface-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                                               "-" + std::to_string(counter++) + ".frag.glsl");
        {
            std::ofstream out(wrapped, std::ios::binary);
            out << surface_shader_source(surface);
        }
        std::array<std::vector<u8>, surface_variant_count3d> compiled;
        bool ok = true;
        for (u32 i = 0; i < surface_variant_count3d && ok; ++i) {
            auto spirv =
                compile_glsl(wrapped, ShaderStage::Fragment, &why, surface_variant_defines(SurfaceVariant3D(i)));
            ok = spirv.has_value();
            if (ok)
                compiled[i] = std::move(*spirv);
        }
        std::error_code ignored;
        std::filesystem::remove(wrapped, ignored);
        if (ok) {
            std::array<std::span<const u8>, surface_variant_count3d> views;
            for (u32 i = 0; i < surface_variant_count3d; ++i)
                views[i] = compiled[i];
            shader = create_surface_shader_operation(views);
            if (!shader)
                why = "the device refused the surface shader";
        }
    }
    if (!shader.valid())
        KIN_LOG_WARN_F("render", "3D: surface shader not made",
                       (LogFields{{.name = "what", .value = label}, {.name = "reason", .value = why}}));
    if (!shader)
        fail3d(_errors, label, why);
    else
        _errors.clear();
    return shader;
}

Result3D<MaterialGraphShader3D> Renderer3D::compile_material_graph(const std::filesystem::path& graph) {
    _errors.clear();
    const std::string label = "Renderer3D::compile_material_graph(" + graph.string() + ")";
    std::ifstream in(graph, std::ios::binary);
    if (!in) {
        fail3d(_errors, label, "the graph could not be read");
        return result3d(MaterialGraphShader3D{}, _errors);
    }
    MaterialGraphShader3D made;
    made.graph =
        compile_material_graph3d(std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}});
    if (!made.graph.ok()) {
        for (const auto& e : made.graph.errors)
            fail3d(_errors, label, (e.node.empty() ? std::string() : e.node + ": ") + e.message);
        return result3d(MaterialGraphShader3D{}, _errors);
    }
    made.surface = compile_surface_text_operation(made.graph.surface, label);
    if (!made.surface)
        return result3d(MaterialGraphShader3D{}, _errors);
    return result3d(std::move(made), {});
}

Result3D<MaterialGraphShader3D> Renderer3D::create_material_graph(const CookedMaterialGraph3D& cooked) {
    std::array<std::span<const u8>, surface_variant_count3d> views;
    for (u32 i = 0; i < surface_variant_count3d; ++i)
        views[i] = cooked.spirv[i];
    MaterialGraphShader3D made{create_surface_shader_operation(views), cooked.graph};
    if (!made.surface)
        return result3d(MaterialGraphShader3D{}, _errors);
    return result3d(std::move(made), {});
}

} // namespace kin
