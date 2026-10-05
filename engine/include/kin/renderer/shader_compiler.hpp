#pragma once

// GLSL compiled to SPIR-V at runtime, with glslc (the Vulkan SDK's: the one
// kin was built with, or KIN_GLSLC), for tools and for editing shaders while a
// game runs. Shipped games load precompiled SPIR-V; this is for development.
//
//     kin::ShaderFile lava{renderer, "shaders/lava.frag.glsl"};
//     ...each frame:
//     lava.poll();                       // recompiled and reloaded when saved
//     renderer.draw_shader_surface(rect, lava.handle(), params);
//
// A shader that fails to compile is logged and the last good one kept.

#include <kin/core/types.hpp>
#include <kin/renderer/shader.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kin {

class Renderer2D;

enum class ShaderStage : u8 { Fragment, Compute };

// Whether glslc can be found (KIN_GLSLC, or the one kin was built with).
bool shader_compiler_available();
// SPIR-V for `source`, or nothing, with glslc's messages in `error`.
std::optional<std::vector<u8>> compile_glsl(const std::filesystem::path& source, ShaderStage stage,
                                            std::string* error = nullptr);

// A fragment shader kept compiled from a GLSL file: poll() recompiles it when
// the file changes and reloads it in place (the handle stays the same).
class ShaderFile {
public:
    ShaderFile(Renderer2D& renderer, std::filesystem::path source);

    ShaderHandle handle() const { return _handle; }
    // Recompiles and reloads when the file changed since the last look. True
    // when a new version is in use.
    bool poll();
    // The last compile's messages ("" when it succeeded).
    const std::string& error() const { return _error; }

private:
    bool load();

    Renderer2D* _renderer = nullptr;
    std::filesystem::path _source;
    std::filesystem::file_time_type _seen{};
    ShaderHandle _handle{};
    std::string _error;
};

} // namespace kin
