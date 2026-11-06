#include <kin/renderer/shader_compiler.hpp>

#include <kin/assets/content.hpp>
#include <kin/platform/log.hpp>
#include <kin/platform/process.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

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

// What glslc reads of a content snapshot: the shader and what it may include.
bool shader_source_file(std::string_view path) {
    constexpr std::array<std::string_view, 9> extensions{".glsl", ".vert", ".frag", ".comp", ".geom",
                                                         ".tesc", ".tese", ".inc",  ".h"};
    return std::ranges::any_of(extensions, [&](std::string_view extension) { return path.ends_with(extension); });
}

// A directory removed with what is in it when this goes.
struct ScratchDirectory {
    std::filesystem::path root;

    ~ScratchDirectory() {
        if (!root.empty()) {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    }
};

// The snapshot's shader sources written under a new directory next to `out`
// (glslc reads files); the source's path there. An error when they can't be.
std::filesystem::path write_shader_sources(const MemoryContent& snapshot, const std::filesystem::path& source,
                                           const std::filesystem::path& out, ScratchDirectory& sources) {
    constexpr std::size_t max_files = 4096;
    constexpr std::size_t max_bytes = 64 * 1024 * 1024;
    std::filesystem::path directory{out.string() + ".sources"};
    while (!std::filesystem::create_directory(directory)) {
        directory += "-next";
    }
    sources.root = directory;
    const std::string relative = source.lexically_relative(snapshot.root()).generic_string();
    std::size_t files = 0;
    std::size_t bytes = 0;
    for (const auto& [path, data] : snapshot.files()) {
        if (path != relative && !shader_source_file(path)) {
            continue;
        }
        bytes += data.size();
        if (++files > max_files || bytes > max_bytes) {
            throw std::runtime_error("shader sources exceed 4096 files or 64 MiB");
        }
        const std::filesystem::path file = directory / path;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream stream(file, std::ios::binary);
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!stream) {
            throw std::runtime_error("cannot prepare shader source files");
        }
    }
    return directory / relative;
}

} // namespace

bool shader_compiler_available() {
    const std::filesystem::path glslc = glslc_path();
    std::error_code error;
    return !glslc.empty() && std::filesystem::exists(glslc, error);
}

std::optional<std::vector<u8>> compile_glsl(const std::filesystem::path& source, ShaderStage stage,
                                            std::string* error, std::span<const char* const> defines,
                                            bool optimize) {
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
    // A shader in a content snapshot (a dev reload's): glslc reads files.
    ScratchDirectory sources;
    std::filesystem::path compiled_source = source;
    const std::shared_ptr<const MemoryContent> snapshot = memory_content_for(source);
    if (snapshot) {
        try {
            compiled_source = write_shader_sources(*snapshot, source, out, sources);
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    }
    Process glslc;
    ProcessOptions options;
    options.args = {glslc_path().string(),
                    stage == ShaderStage::Compute  ? "-fshader-stage=compute"
                    : stage == ShaderStage::Vertex ? "-fshader-stage=vertex"
                                                   : "-fshader-stage=fragment",
                    "-o", out.string(), compiled_source.string()};
    for (const char* define : defines)
        options.args.insert(options.args.end() - 1, std::string("-D") + define);
    if (optimize)
        options.args.insert(options.args.end() - 1, "-O");
#ifdef KIN_SHADER_INCLUDE_DIR
    // `#include "kin_material.inc.glsl"` (3D material shaders) from the engine's own.
    options.args.insert(options.args.begin() + 2, {"-I", KIN_SHADER_INCLUDE_DIR});
#endif
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
    if (snapshot) {
        // Errors name the snapshot's files, not their copies.
        const std::string copies = sources.root.string();
        const std::string original = snapshot->root().string();
        for (std::size_t at = messages.find(copies); at != std::string::npos; at = messages.find(copies, at)) {
            messages.replace(at, copies.size(), original);
            at += original.size();
        }
    }
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
