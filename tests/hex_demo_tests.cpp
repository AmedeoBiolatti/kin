// Exercise the shipped scene directly: the same controls, map and HUD.
#define KIN_HEX_DEMO_TEST
#include "../games/hex_demo/src/main.cpp"
#include <kin/core/json_value.hpp>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sstream>

int main() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    auto& window = app.create_window({.title = "hex regression", .width = 960, .height = 600, .hidden = true});
    kin::Renderer2D renderer{kin::make_render_backend(window, false, false)};
    renderer.set_logical_size(960, 600);
    app.input().set_map(*demo::make_game_info().input_map);
    kin::SceneManager scenes;
    demo::HexDemoScene scene;
    kin::SceneContext ctx{app, window, renderer, app.input(), scenes};
    const auto state = [&] {
        std::ostringstream out;
        kin::JsonWriter json(out);
        json.begin_object();
        scene.write_report(json);
        json.end_object();
        auto parsed = kin::parse_json(out.str());
        assert(parsed.ok());
        return std::move(*parsed.value);
    };
    const auto cell = [](const kin::JsonValue& state, std::string_view key) {
        const auto* value = state.find(key);
        assert(value && value->items().size() == 2);
        return kin::Vec2i{int(value->items()[0].as_int()), int(value->items()[1].as_int())};
    };
    const auto key = [&](kin::Key key) {
        app.input().begin_frame();
        app.input().set_key_pressed(key);
        scene.update(ctx);
        app.input().set_key_released(key);
        app.input().begin_frame();
    };
    std::vector<kin::u8> pixels;
    kin::Vec2i size;
    const auto render = [&] {
        scene.render(ctx);
        assert(renderer.read_rgba({0, 0, 960, 600}, pixels, size));
        assert((size == kin::Vec2i{960, 600}));
    };
    const auto pick_color = [&](kin::Color color) {
        for (int y = 95; y < 575; ++y) {
            for (int x = 25; x < 935; ++x) {
                const auto at = (y * 960 + x) * 4;
                if (pixels[at] == color.r && pixels[at + 1] == color.g && pixels[at + 2] == color.b) {
                    return kin::Vec2f{float(x), float(y)};
                }
            }
        }
        throw std::runtime_error("Expected hex terrain/marker was not rendered");
    };
    for (int layout = 0; layout < 4; ++layout) {
        key(kin::Key::R);
        if (layout & 1) key(kin::Key::Tab);
        if (layout & 2) key(kin::Key::O);
        auto before = state();
        assert(before.string_at("orientation") == ((layout & 1) ? "flat-top" : "pointy-top"));
        assert(before.string_at("offset") == ((layout & 2) ? "even" : "odd"));
        assert(before.int_at("path_cells") > 1 && before.int_at("reachable_cells") > 1);
        render();
        if (const char* capture = std::getenv("KIN_HEX_TEST_CAPTURE")) {
            std::ofstream image(std::string(capture) + "-" + std::to_string(layout) + ".ppm", std::ios::binary);
            image << "P6\n960 600\n255\n";
            for (std::size_t at = 0; at < pixels.size(); at += 4) image.write(reinterpret_cast<const char*>(pixels.data() + at), 3);
        }
        // Pick the rendered unit itself: goal must move to its cell, reducing
        // the route to one cell. Picking stays correct in every layout.
        const auto unit = pick_color(kin::Color::rgb(236, 92, 84));
        const auto plains = pick_color(kin::Color::rgb(148, 176, 98));
        app.input().begin_frame();
        app.input().set_mouse_pos(renderer.logical_to_window(unit), window.id());
        app.input().set_mouse_pressed(kin::MouseButton::Left);
        scene.update(ctx);
        assert(cell(state(), "goal") == cell(before, "start"));
        assert(state().int_at("path_cells") == 1);
        app.input().begin_frame();
        app.input().set_mouse_pos(renderer.logical_to_window(plains), window.id());
        app.input().set_mouse_pressed(kin::MouseButton::Right);
        scene.update(ctx);
        assert(cell(state(), "start") != cell(before, "start"));
        assert(state().int_at("path_cells") > 1);
    }
    key(kin::Key::R);
    const auto reset = state();
    assert(reset.int_at("path_cells") == 17 && reset.int_at("path_cost") == 170);
    assert(reset.int_at("reachable_cells") == 28);
}
