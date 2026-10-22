#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <iostream>

int main() {
    try {
        kin::App app{{.mode = kin::AppMode::Headless}};
        auto& window = app.create_window({.title = "renderer policy", .width = 32, .height = 32, .hidden = true});
        kin::Renderer2D renderer{kin::make_render_backend(window, false, false)};
        std::cout << "backend=" << renderer.backend_name() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
