#include "x11_host.hpp"
#if KIN_X11_STAGE >= 2
#include "compact_renderer2d_backend.hpp"
#include "hex_game.hpp"
#endif
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

#if KIN_X11_STAGE == 2 || KIN_X11_STAGE == 3
static void render_sample(kin::Renderer2D& r) {
    r.clear(kin::Color::rgb(20, 24, 30));
    kin::HexLayout layout;
    layout.size = {28, 28};
    layout.origin = {480, 300};
    const auto extent = layout.hex_extent();
    const kin::Vec2i size{int(std::ceil(extent.x)) + 2, int(std::ceil(extent.y)) + 2};
    static const auto hex = demo::create_hex_texture(r, layout, size);
    r.draw_texture(hex, {0, 0, float(size.x), float(size.y)},
        {480 - size.x * 0.5f, 300 - size.y * 0.5f, float(size.x), float(size.y)},
        kin::Color::rgb(148, 176, 98));
#if KIN_X11_STAGE == 3
    kin::ui2::draw_text(r, demo::hud_font::get(), "HEX GRID  pointy-top, odd rows offset",
                       {24, 14}, 20.0f / 15, kin::Color::rgb(226, 232, 240));
#endif
}
#endif

int main(int argc, char** argv) try {
    bool headless = false, hidden = false, report = false;
    int frames = 0;
    const char* capture = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--headless") headless = true;
        else if (arg == "--hidden") hidden = true;
        else if (arg == "--render" || arg == "--seed=7") {} // compatible measurement tools
        else if (arg == "--report=-") report = true;
        else if (arg.starts_with("--capture=")) capture = argv[i] + 10;
        else if (arg.starts_with("--frames=")) {
            const auto value = arg.substr(9);
            const auto result = std::from_chars(value.data(), value.data() + value.size(), frames);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || frames < 1)
                throw std::invalid_argument("--frames requires a positive integer");
        } else throw std::invalid_argument("Unknown argument: " + std::string(arg));
    }
    if (headless && !frames) frames = 1;
    kin::experimental::X11Host host({960, 600}, headless, hidden);
#if KIN_X11_STAGE >= 2
    kin::Renderer2D renderer(std::make_unique<kin::CompactRenderer2DBackend>(host));
    renderer.set_logical_size(960, 600);
#if KIN_X11_STAGE == 4
    demo::HexGame game;
#endif
#endif
#if KIN_X11_STAGE == 1
    std::vector<kin::u8> pixels(960 * 600 * 4, 255);
#endif
    for (int frame = 0; !frames || frame < frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        const auto input = host.poll();
        if (input.quit) break;
#if KIN_X11_STAGE >= 2
#if KIN_X11_STAGE == 4
        if (!game.update({.quit = input.quit, .reset = input.reset, .orientation = input.orientation,
                          .offset = input.offset, .left = input.left, .right = input.right,
                          .mouse = renderer.window_to_logical(input.mouse)})) break;
        game.render(renderer);
#else
        render_sample(renderer);
#endif
        renderer.present();
#elif KIN_X11_STAGE == 1
        const auto size = host.pixel_size();
        pixels.resize(std::size_t(size.x) * size.y * 4, 255);
        host.present_rgba(pixels, size);
#endif
        if (!headless && !frames) std::this_thread::sleep_until(start + std::chrono::milliseconds(16));
    }
#if KIN_X11_STAGE >= 2
    if (capture) {
        std::vector<kin::u8> pixels;
        kin::Vec2i size;
        if (!renderer.read_rgba({0, 0, 960, 600}, pixels, size)) throw std::runtime_error("Capture failed");
        std::ofstream out(capture, std::ios::binary);
        out << "P6\n" << size.x << ' ' << size.y << "\n255\n";
        for (std::size_t p = 0; p < pixels.size(); p += 4) out.write(reinterpret_cast<const char*>(pixels.data() + p), 3);
        if (!out) throw std::runtime_error("Cannot write capture");
    }
#if KIN_X11_STAGE == 4
    if (report) {
        kin::JsonWriter json(std::cout);
        json.begin_object();
        game.write_report(json);
        json.end_object();
        std::cout << '\n';
    }
#else
    if (report) throw std::invalid_argument("This ladder stage has no game report");
#endif
#else
    if (capture || report) throw std::invalid_argument("This ladder stage has no game/capture");
#endif
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
