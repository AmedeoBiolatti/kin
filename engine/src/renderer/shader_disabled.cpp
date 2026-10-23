#include <kin/renderer/shader_compiler.hpp>
#include <kin/renderer/shader_reflect.hpp>
#include <utility>

namespace kin {
namespace {
constexpr auto disabled = "Shader support is disabled by KIN_RENDERER=software";
}

bool shader_compiler_available() { return false; }

std::optional<std::vector<u8>> compile_glsl(const std::filesystem::path&, ShaderStage, std::string* error) {
    if (error) *error = disabled;
    return std::nullopt;
}

std::optional<ShaderLayout> reflect_spirv(ShaderBlob, std::string* error) {
    if (error) *error = disabled;
    return std::nullopt;
}

ShaderFile::ShaderFile(Renderer2D& renderer, std::filesystem::path source)
    : _renderer(&renderer), _source(std::move(source)), _error(disabled) {}
bool ShaderFile::poll() { return false; }
bool ShaderFile::load() { return false; }
} // namespace kin
