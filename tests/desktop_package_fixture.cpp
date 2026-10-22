#include <kin/platform/app.hpp>
#include <kin/platform/resource_paths.hpp>
#include <kin/renderer/renderer2d.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" const char* kin_package_fixture_marker();

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

// Run only from an extracted package, with an unrelated working directory.
int main(int argc, char** argv) {
    try {
        const bool gpu = argc > 1 && std::string_view{argv[1]} == "--gpu";
        const auto base = kin::executable_dir();
        require(base.is_absolute(), "executable path is not absolute");
        require(!std::filesystem::equivalent(base, std::filesystem::current_path()),
                "test must run from another working directory");
        const auto root = kin::game_assets_dir("package_fixture");
        require(root == base / "assets" / "package_fixture", "did not select packaged resources");
        require(kin::game_assets_dir("package_fixture", std::filesystem::current_path()) ==
                std::filesystem::current_path(), "development override ignored");
        std::ifstream in{root / "marker.txt"};
        std::string marker;
        std::getline(in, marker);
        require(marker == kin_package_fixture_marker(), "packaged resource missing or wrong");
        bool rejected = false;
        try { kin::game_assets_dir("../escape"); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid game name accepted");

        kin::App app{{.mode = gpu ? kin::AppMode::Windowed : kin::AppMode::Headless}};
        auto& window = app.create_window({.title = "Package verification", .width = 32, .height = 32, .hidden = true});
        kin::Renderer2D renderer{kin::make_render_backend(window, false, gpu)};
        if (gpu) {
            require(renderer.backend_name() == "SDL_GPU", "GPU renderer fell back after relocation");
            for (const auto shader : {kin::BuiltinShader::Vignette, kin::BuiltinShader::ColorGrade,
                 kin::BuiltinShader::Scanline, kin::BuiltinShader::Chroma, kin::BuiltinShader::BloomBright,
                 kin::BuiltinShader::BloomBlur, kin::BuiltinShader::BloomCombine,
                 kin::BuiltinShader::TransitionDissolve, kin::BuiltinShader::TransitionPixelate,
                 kin::BuiltinShader::TransitionWipe, kin::BuiltinShader::TransitionIris,
                 kin::BuiltinShader::TransitionCrossfade}) {
                require(bool(renderer.builtin_shader(shader)), "embedded post-process shader missing");
            }
        }
        renderer.clear(kin::Color::rgb(17, 51, 93));
        std::vector<kin::u8> pixels;
        kin::Vec2i size;
        require(renderer.read_rgba({0, 0, 32, 32}, pixels, size), "packaged renderer readback failed");
        require(size.x == 32 && size.y == 32 && pixels.size() == 32 * 32 * 4, "readback dimensions wrong");
        require(pixels[0] == 17 && pixels[1] == 51 && pixels[2] == 93, "readback color wrong");
        std::cout << "Relocated desktop package OK: " << renderer.backend_name() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
