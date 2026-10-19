#include <kin/render3d/renderer3d.hpp>

#include <kin/assets/content.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/shader_compiler.hpp>

#include <cstddef>
#include <optional>
#include <string>

extern "C" {
float* kin_hdr_load(const unsigned char* bytes, int size, int* width, int* height);
void kin_hdr_free(float* pixels);
}

namespace kin {

Environment3D Renderer3D::create_environment(Vec2i size, std::span<const f32> rgb) {
    _errors.clear();
    if (!_backend || size.x < 8 || size.y < 4 || rgb.size() != static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y) * 3) {
        fail3d(_errors, "Renderer3D::create_environment", _backend ? "invalid environment dimensions/pixel count" : "3D is not available");
        return {};
    }
    std::shared_ptr<IEnvironmentBackend> backend = _backend->create_environment(size, rgb);
    if (!backend) fail3d(_errors, "Renderer3D::create_environment", "device refused environment textures");
    return backend ? Environment3D{std::move(backend), size} : Environment3D{};
}

Environment3D Renderer3D::load_environment(const std::filesystem::path& path, std::string* error) {
    _errors.clear();
    std::string why;
    Environment3D environment;
    if (!_backend) {
        why = "3D is not available";
    } else if (const std::optional<std::string> bytes = read_content_file(path); !bytes) {
        why = "cannot read " + path.string();
    } else {
        int w = 0, h = 0;
        float* pixels = kin_hdr_load(reinterpret_cast<const unsigned char*>(bytes->data()), static_cast<int>(bytes->size()), &w, &h);
        if (!pixels) {
            why = path.string() + ": not a Radiance .hdr picture";
        } else {
            environment = create_environment({w, h}, {pixels, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3});
            kin_hdr_free(pixels);
            if (!environment.valid()) {
                why = path.string() + ": cannot make an environment of it (compute shaders, or a picture under 8 x 4)";
            }
        }
    }
    if (!environment.valid()) {
        KIN_LOG_WARN_F("render", "3D: environment not loaded", (LogFields{{.name = "path", .value = path.string()}, {.name = "reason", .value = why}}));
    }
    if (!environment) fail3d(_errors, "Renderer3D::load_environment(" + path.string() + ")", why);
    else _errors.clear();
    if (error) *error = diagnostics3d_text(_errors);
    return environment;
}

MaterialShader3D Renderer3D::create_material_shader(std::span<const u8> spirv) {
    _errors.clear();
    if (!_backend || spirv.empty()) {
        fail3d(_errors, "Renderer3D::create_material_shader", _backend ? "empty SPIR-V" : "3D is not available");
        return {};
    }
    MaterialShader3D shader{_backend->create_material_shader(spirv)};
    if (!shader) fail3d(_errors, "Renderer3D::create_material_shader", "device refused shader");
    return shader;
}

MaterialShader3D Renderer3D::compile_material_shader(const std::filesystem::path& source, std::string* error) {
    _errors.clear();
    std::string why;
    MaterialShader3D shader;
    if (!_backend) {
        why = "3D is not available";
    } else if (const std::optional<std::vector<u8>> spirv = compile_glsl(source, ShaderStage::Fragment, &why)) {
        shader = create_material_shader(*spirv);
        if (!shader.valid()) {
            why = "the device refused the shader";
        }
    }
    if (!shader.valid()) {
        KIN_LOG_WARN_F("render", "3D: material shader not made", (LogFields{{.name = "path", .value = source.string()}, {.name = "reason", .value = why}}));
    }
    if (!shader) fail3d(_errors, "Renderer3D::compile_material_shader(" + source.string() + ")", why);
    else _errors.clear();
    if (error) *error = diagnostics3d_text(_errors);
    return shader;
}

} // namespace kin
