#include <kin/core/json.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/window.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/scene_app.hpp>

#include <algorithm>
#include <memory>

namespace {
class Game final : public kin::Scene {
public:
    std::string_view name() const override { return "Game"; }

    void update(kin::SceneContext& ctx) override {
        if (ctx.window.close_requested() || ctx.input.pressed(kin::Key::Escape)) {
            ctx.app.quit();
            return;
        }
        const float dx = float(ctx.input.held(kin::Key::Right)) - float(ctx.input.held(kin::Key::Left));
        const float dy = float(ctx.input.held(kin::Key::Down)) - float(ctx.input.held(kin::Key::Up));
        _position.x = std::clamp(_position.x + dx * 180.0f * ctx.dt, 0.0f, 608.0f);
        _position.y = std::clamp(_position.y + dy * 180.0f * ctx.dt, 0.0f, 328.0f);
        ++_steps;
    }

    void render(kin::SceneContext& ctx) override {
        ctx.renderer.clear(kin::Color::rgb(24, 28, 36));
        ctx.renderer.fill_rect(_position, {32, 32}, kin::Color::rgb(110, 210, 170));
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("steps", _steps).field("x", _position.x).field("y", _position.y);
    }

private:
    kin::Vec2f _position{304, 164};
    int _steps = 0;
};
} // namespace

int main(int argc, char** argv) {
    kin::SceneManager scenes;
    scenes.push(std::make_unique<Game>());
    return kin::run_scene_app({.window = {.title = "Kin Starter",
                                          .width = 640,
                                          .height = 360,
                                          .logical_width = 640,
                                          .logical_height = 360,
                                          .max_fps = 60},
                               .headless = kin::parse_headless_options(argc, argv)},
                              scenes);
}
