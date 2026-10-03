#include <kin/renderer/shader_compiler.hpp>

#include <kin/platform/log.hpp>
#include <kin/platform/process.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace kin {

namespace {

std::filesystem::path glslc_path() {
    if (const char* from_env = std::getenv("KIN_GLSLC"); from_env && *from_env) {
        return from_env;
    }
#ifdef KIN_GLSLC_PATH
    return KIN_GLSLC_PATH;
#else
    return {};
#endif
}

} // namespace

bool shader_compiler_available() {
    const std::filesystem::path glslc = glslc_path();
    std::error_code error;
    return !glslc.empty() && std::filesystem::exists(glslc, error);
}

std::optional<std::vector<u8>> compile_glsl(const std::filesystem::path& source, ShaderStage stage,
                                            std::string* error) {
    const auto fail = [&](std::string why) -> std::optional<std::vector<u8>> {
        if (error) {
            *error = std::move(why);
        }
        return std::nullopt;
    };
    if (!shader_compiler_available()) {
        return fail("glslc not found (set KIN_GLSLC)");
    }
    static std::atomic<u32> counter{0};
    const std::filesystem::path out = std::filesystem::temp_directory_path() /
        ("kin-shader-" + std::to_string(SDL_GetCurrentThreadID()) + "-" + std::to_string(counter++) + ".spv");
    Process glslc;
    ProcessOptions options;
    options.args = {glslc_path().string(),
                    stage == ShaderStage::Compute ? "-fshader-stage=compute" : "-fshader-stage=fragment",
                    "-o", out.string(), source.string()};
    options.pipe_input = false;
    options.errors_to_output = true;
    if (!glslc.start(options)) {
        return fail("could not run glslc: " + glslc.error());
    }
    std::string messages;
    while (glslc.running()) {
        messages += glslc.read();
        glslc.wait(std::chrono::milliseconds{2});
    }
    messages += glslc.read();
    if (glslc.exit_code().value_or(-1) != 0) {
        std::filesystem::remove(out);
        return fail(messages.empty() ? std::string{"glslc failed"} : messages);
    }
    std::ifstream file(out, std::ios::binary);
    std::vector<u8> spirv{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    file.close();
    std::filesystem::remove(out);
    if (spirv.empty()) {
        return fail("glslc wrote nothing");
    }
    if (error) {
        error->clear();
    }
    return spirv;
}

ShaderFile::ShaderFile(Renderer2D& renderer, std::filesystem::path source)
    : _renderer(&renderer), _source(std::move(source)) {
    std::error_code ignored;
    _seen = std::filesystem::last_write_time(_source, ignored);
    load();
}

bool ShaderFile::poll() {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(_source, error);
    if (error || written == _seen) {
        return false;
    }
    _seen = written;
    return load();
}

bool ShaderFile::load() {
    const std::optional<std::vector<u8>> spirv = compile_glsl(_source, ShaderStage::Fragment, &_error);
    if (!spirv) {
        KIN_LOG_ERROR_F("render", "shader did not compile; keeping the last good one",
                        (LogFields{{.name = "path", .value = _source.string()}, {.name = "error", .value = _error}}));
        return false;
    }
    ShaderDesc desc;
    desc.spirv = {spirv->data(), static_cast<u32>(spirv->size())};
    if (!_handle) {
        _handle = _renderer->create_shader(desc);
        return static_cast<bool>(_handle);
    }
    return _renderer->reload_shader(_handle, desc);
}

} // namespace kin
