#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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

namespace {
// A Radiance .hdr picture (RGBE, 2:1 panoramas: what kin_hdr_load reads): the header, a "-Y h +X w" size, then each
// scanline in the new run-length coding (its four components apart, each in literal runs of up to 128: a flat scanline
// whose first texel began 2, 2 would read as a coded one).
std::string encode_hdr(Vec2i size, std::span<const f32> rgb) {
    const auto w = static_cast<std::size_t>(size.x), h = static_cast<std::size_t>(size.y);
    std::string out =
        "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(h) + " +X " + std::to_string(w) + "\n";
    std::vector<u8> texel(w * 4);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const f32* c = &rgb[(y * w + x) * 3];
            const f32 r = std::isfinite(c[0]) ? std::max(c[0], 0.0f) : 0.0f;
            const f32 g = std::isfinite(c[1]) ? std::max(c[1], 0.0f) : 0.0f;
            const f32 b = std::isfinite(c[2]) ? std::max(c[2], 0.0f) : 0.0f;
            const f32 m = std::max({r, g, b});
            u8* t = &texel[x * 4];
            if (m < 1e-32f) {
                t[0] = t[1] = t[2] = t[3] = 0;
            } else {
                int e = 0;
                const f32 scale = std::frexp(m, &e) * 256.0f / m;
                t[0] = static_cast<u8>(std::min(r * scale, 255.0f));
                t[1] = static_cast<u8>(std::min(g * scale, 255.0f));
                t[2] = static_cast<u8>(std::min(b * scale, 255.0f));
                t[3] = static_cast<u8>(e + 128);
            }
        }
        if (w < 8 || w >= 32768) { // (the coding's limits: flat)
            out.append(reinterpret_cast<const char*>(texel.data()), texel.size());
            continue;
        }
        out += static_cast<char>(2);
        out += static_cast<char>(2);
        out += static_cast<char>(w >> 8);
        out += static_cast<char>(w & 0xFF);
        for (std::size_t c = 0; c < 4; ++c)
            for (std::size_t x = 0; x < w;) {
                const std::size_t n = std::min<std::size_t>(128, w - x);
                out += static_cast<char>(n);
                for (std::size_t k = 0; k < n; ++k)
                    out += static_cast<char>(texel[(x + k) * 4 + c]);
                x += n;
            }
    }
    return out;
}
} // namespace

Status3D Renderer3D::save_reflection_capture(const ReflectionCapture3D& capture, const std::filesystem::path& file) {
    _errors.clear();
    const auto fail = [&](const std::string& why) {
        fail3d(_errors, "Renderer3D::save_reflection_capture(" + file.string() + ")", why);
        return status3d(false, _errors);
    };
    if (!capture || !_backend)
        return fail("an empty capture");
    if (capture.backend()->generation() == 0)
        return fail("not captured yet");
    std::vector<f32> rgb;
    Vec2i size{};
    if (!_backend->read_reflection_capture(*capture.backend(), rgb, size) || size.x <= 0 || size.y <= 0 ||
        rgb.size() != static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 3)
        return fail("its picture could not be read back");
    std::error_code error;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary);
    const std::string bytes = encode_hdr(size, rgb);
    if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return fail("cannot write it");
    return status3d(true, _errors);
}

Result3D<ReflectionCapture3D> Renderer3D::load_reflection_capture(const std::filesystem::path& path) {
    _errors.clear();
    const auto fail = [&](const std::string& why) {
        fail3d(_errors, "Renderer3D::load_reflection_capture(" + path.string() + ")", why);
        return result3d(ReflectionCapture3D{}, _errors);
    };
    if (!_backend)
        return fail("3D is not available");
    const std::optional<std::string> bytes = read_content_file(path);
    if (!bytes)
        return fail("cannot read it");
    int w = 0, h = 0;
    float* pixels =
        kin_hdr_load(reinterpret_cast<const unsigned char*>(bytes->data()), static_cast<int>(bytes->size()), &w, &h);
    if (!pixels)
        return fail("not a Radiance .hdr picture");
    std::shared_ptr<IReflectionCaptureBackend3D> backend;
    if (w >= 16 && w == 2 * h)
        backend = _backend->create_reflection_capture_from(
            {w, h}, {pixels, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3});
    kin_hdr_free(pixels);
    if (!(w >= 16 && w == 2 * h))
        return fail("a panorama must be 2:1 and at least 16 wide");
    if (!backend)
        return fail("not supported here (needs the GPU renderer and compute)");
    return result3d(ReflectionCapture3D{std::move(backend)}, _errors);
}

Result3D<ReflectionCapture3D> Renderer3D::create_reflection_capture(u32 side) {
    _errors.clear();
    const auto fail = [&](const std::string& why) {
        fail3d(_errors, "Renderer3D::create_reflection_capture", why);
        return result3d(ReflectionCapture3D{}, _errors);
    };
    if (!_backend)
        return fail("3D is not available");
    if (side < 16 || side > 1024)
        return fail("a side of " + std::to_string(side) + " (16 to 1024)");
    auto backend = _backend->create_reflection_capture(side);
    if (!backend)
        return fail("not supported here (needs the GPU renderer and compute)");
    return result3d(ReflectionCapture3D{std::move(backend)}, _errors);
}

Status3D Renderer3D::capture_reflection(ReflectionCapture3D& capture, Scene3D& scene, Vec3f position,
                                        const ReflectionCaptureOptions3D& options) {
    _errors.clear();
    _stats = {};
    const auto fail = [&](const char* why) {
        fail3d(_errors, "Renderer3D::capture_reflection", why);
        return status3d(false, _errors);
    };
    if (!capture || !_backend)
        return fail("an empty capture");
    if (!scene.diagnostics().empty()) {
        _errors = scene.diagnostics();
        return status3d(false, _errors);
    }
    if (!(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)) ||
        !(options.z_near > 0.0f) || !(options.z_far >= 0.0f) ||
        (options.z_far > 0.0f && options.z_far <= options.z_near))
        return fail("a position, or near and far, that is not finite and in order");
    if (!_backend->capture_reflection(*capture.backend(), scene, position, options, _stats))
        return fail("the device refused it");
    return status3d(true, _errors);
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

bool surface_has_alpha(std::string_view surface) {
    return surface.find("kin_surface_alpha") != std::string_view::npos;
}

bool surface_has_offset(std::string_view surface) {
    return surface.find("vec3 kin_surface_offset() {") != std::string_view::npos;
}

std::string surface_shader_source(std::string_view surface, SurfaceVariant3D variant) {
    if (u32(variant) >= surface_vertex_variant3d && variant != SurfaceVariant3D::Count) {
        // A vertex variant: the offset's prelude, the surface's offset, then the stock vertex shader's body.
        static constexpr const char* bodies[] = {"kin_mesh_vertex.inc.glsl",    "kin_mesh_vertex.inc.glsl",
                                                 "kin_mesh_vertex.inc.glsl",    "kin_prepass_vertex.inc.glsl",
                                                 "kin_prepass_vertex.inc.glsl", "kin_prepass_velocity_vertex.inc.glsl",
                                                 "kin_shadow_vertex.inc.glsl",  "kin_shadow_vertex.inc.glsl"};
        std::string source = "#version 450 core\n#define KIN_SURFACE_VERTEX\n#include \"kin_surface_vertex.inc.glsl\"\n"
                             "#line 1\n";
        source += surface;
        if (!surface.empty() && surface.back() != '\n')
            source += '\n';
        source += std::string("#include \"") + bodies[u32(variant) - surface_vertex_variant3d] + "\"\n";
        return source;
    }
    if (u32(variant) >= surface_mask_variant3d && variant != SurfaceVariant3D::Count) {
        // A cut-out pass's shader: its body (inputs, the cut-out's resources, main), then the surface's alpha.
        static constexpr const char* bodies[] = {"kin_shadow_mask.inc.glsl", "kin_pick_mask.inc.glsl",
                                                 "kin_prepass_mask.inc.glsl", "kin_prepass_velocity_mask.inc.glsl",
                                                 "kin_visibility_mask.inc.glsl"};
        std::string source = std::string("#version 450 core\n#define KIN_SURFACE_MASK\n"
                                         "#define KIN_SURFACE_ALPHA kin_surface_alpha\n#include \"") +
                             bodies[u32(variant) - surface_mask_variant3d] + "\"\n#line 1\n";
        source += surface;
        if (!surface.empty() && surface.back() != '\n')
            source += '\n';
        return source;
    }
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
    static constexpr std::array<const char*, 5> materials{"KIN_DRAW_MATERIALS", "KIN_LIGHTS", "KIN_GI",
                                                          "KIN_LIGHT_GRID", "KIN_FOG"};
    static constexpr std::array<const char*, 2> materials_fog{"KIN_DRAW_MATERIALS", "KIN_FOG"};
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
    case SurfaceVariant3D::Material:
        return std::span<const char* const>{materials}.first(1);
    case SurfaceVariant3D::MaterialFog:
        return materials_fog;
    case SurfaceVariant3D::MaterialLights:
        return std::span<const char* const>{materials}.first(4);
    case SurfaceVariant3D::MaterialLightsFog:
        return materials;
    case SurfaceVariant3D::VertexMeshVelocity:
    case SurfaceVariant3D::VertexPrepassVelocity: // (the pre-pass's body is its own; the define does no harm)
        return moving.first(1);
    case SurfaceVariant3D::VertexMeshIndexed:
    case SurfaceVariant3D::VertexPrepassIndexed: {
        static constexpr std::array<const char*, 1> indexed{"KIN_INDEXED"};
        return indexed;
    }
    case SurfaceVariant3D::VertexShadowMask: {
        static constexpr std::array<const char*, 1> shadow_mask{"KIN_SHADOW_MASK"};
        return shadow_mask;
    }
    default: // the cut-outs' (their wrapper defines what they need), the plain vertex variants'
        break;
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
    for (u32 i = 0; i < surface_required_variants3d; ++i)
        if (spirv[i].empty()) {
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
        // glslc compiles files: each variant's wrapped source goes to one (the main ones share theirs).
        static std::atomic<u32> counter{0};
        const std::string stem =
            "kin-surface-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" + std::to_string(counter++);
        const bool alpha = surface_has_alpha(surface), offset = surface_has_offset(surface);
        std::array<std::vector<u8>, surface_variant_count3d> compiled;
        std::vector<std::filesystem::path> files;
        bool ok = true;
        for (u32 i = 0; i < surface_variant_count3d && ok; ++i) { // (all of them, the merged draws' too)
            const bool vertex = i >= surface_vertex_variant3d, mask = i >= surface_mask_variant3d && !vertex;
            if ((mask && !alpha) || (vertex && !offset))
                continue;
            const auto file = std::filesystem::temp_directory_path() /
                              (stem + (mask || vertex ? "-" + std::to_string(i) : std::string()) +
                               (vertex ? ".vert.glsl" : ".frag.glsl"));
            if (std::ranges::find(files, file) == files.end()) {
                std::ofstream out(file, std::ios::binary);
                out << surface_shader_source(surface, SurfaceVariant3D(i));
                files.push_back(file);
            }
            // Optimised, as cooked graphs are: what development sees is what a game ships.
            auto spirv = compile_glsl(file, vertex ? ShaderStage::Vertex : ShaderStage::Fragment, &why,
                                      surface_variant_defines(SurfaceVariant3D(i)), true);
            ok = spirv.has_value();
            if (ok)
                compiled[i] = std::move(*spirv);
        }
        std::error_code ignored;
        for (const auto& file : files)
            std::filesystem::remove(file, ignored);
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
    made.surface =
        SurfaceShader3D{compile_surface_text_operation(made.graph.surface, label).backend(), made.graph.traits};
    if (!made.surface)
        return result3d(MaterialGraphShader3D{}, _errors);
    return result3d(std::move(made), {});
}

Result3D<MaterialGraphShader3D> Renderer3D::create_material_graph(const CookedMaterialGraph3D& cooked) {
    std::array<std::span<const u8>, surface_variant_count3d> views;
    for (u32 i = 0; i < surface_variant_count3d; ++i)
        views[i] = cooked.spirv[i];
    MaterialGraphShader3D made{SurfaceShader3D{create_surface_shader_operation(views).backend(), cooked.graph.traits},
                               cooked.graph};
    if (!made.surface)
        return result3d(MaterialGraphShader3D{}, _errors);
    return result3d(std::move(made), {});
}

} // namespace kin
