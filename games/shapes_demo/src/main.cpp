// Shapes demo: an orrery of vector shapes. Planets and moons are entities
// whose ShapeRenderers turn with their parents (flecs ChildOf); the planets
// and the sun are composed in code, the rocket read from SVG-lite; the HUD is
// drawn with the immediate shape calls.
//
//   Q / E    turn the camera      - / =  or the wheel   zoom
//   Space    pause                Esc   quit

#include <kin/core/json.hpp>
#include <kin/ecs/render.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/renderer/svg.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/text.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace demo {
namespace {

constexpr kin::Vec2f logical_size{960.0f, 600.0f};

// The rocket, as a vector editor would save it: a body, a window, and one fin
// used twice (the second mirrored).
constexpr std::string_view rocket_svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="-20 -40 40 70">
  <defs>
    <path id="fin" d="M6 4 L16 20 L6 16 Z" fill="#e5533d"/>
  </defs>
  <path d="M0 -36 C12 -24 12 0 8 18 L-8 18 C-12 0 -12 -24 0 -36 Z" fill="#f2f0ea" stroke="#20283a" stroke-width="2" stroke-linejoin="round"/>
  <circle cx="0" cy="-10" r="5" fill="#5fb4d9" stroke="#20283a" stroke-width="2"/>
  <use href="#fin"/>
  <use href="#fin" transform="scale(-1 1)"/>
  <path d="M-5 18 Q0 32 5 18 Z" fill="#ffb347"/>
</svg>)svg";

kin::GameInfo make_game_info() {
    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);
    input.bind("turn_left", kin::Key::Q);
    input.bind("turn_right", kin::Key::E);
    input.bind("zoom_out", kin::Key::Minus);
    input.bind("zoom_in", kin::Key::Equals);
    input.bind("pause", kin::Key::Space);
    return {
        .id = "shapes_demo",
        .title = "Kin Shapes Demo",
        .version = "0.1",
        .description = "Vector shapes composed in code and read from SVG, drawn through the ECS hierarchy and a camera.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<kin::i32>(logical_size.x),
            .height = static_cast<kin::i32>(logical_size.y),
            .logical_width = static_cast<kin::i32>(logical_size.x),
            .logical_height = static_cast<kin::i32>(logical_size.y),
            .resizable = true,
        },
        .tags = {"sample", "shapes"},
        .fields = {},
        .input_map = input,
    };
}

// A planet: a disc with a darker band and a ring of craters, made once.
kin::Shape planet(float radius, kin::Color body, kin::Color band) {
    kin::Shape shape;
    shape.fill(kin::Path::circle({0, 0}, radius), body);
    kin::Path stripe = kin::Path::ellipse({0, radius * 0.25f}, {radius * 0.95f, radius * 0.18f});
    shape.fill(stripe, band);
    kin::Shape crater;
    crater.fill_and_stroke(kin::Path::circle({0, 0}, radius * 0.12f), band, kin::Color::rgba(0, 0, 0, 60),
                           {.width = 1.0f});
    for (int i = 0; i < 3; ++i) {
        const float a = static_cast<float>(i) * 2.1f + 0.4f;
        shape.add(crater, kin::Affine2::translation({std::cos(a) * radius * 0.55f, std::sin(a) * radius * 0.45f - radius * 0.2f}));
    }
    shape.stroke(kin::Path::circle({0, 0}, radius), kin::Color::rgba(0, 0, 0, 90), {.width = 1.5f});
    return shape;
}

kin::Shape sun() {
    kin::Shape shape;
    shape.fill(kin::Path::star({0, 0}, 70, 52, 16), kin::Color::rgb(255, 196, 64));
    shape.fill(kin::Path::circle({0, 0}, 48), kin::Color::rgb(255, 150, 40));
    shape.stroke(kin::Path::circle({0, 0}, 36), kin::Color::rgb(255, 214, 120), {.width = 3.0f});
    return shape;
}

kin::Shape orbit(float radius) {
    kin::Shape shape;
    shape.stroke(kin::Path::circle({0, 0}, radius), kin::Color::rgba(255, 255, 255, 40), {.width = 1.0f});
    return shape;
}

std::shared_ptr<const kin::ShapeMesh> mesh_of(const kin::Shape& shape) {
    return std::make_shared<const kin::ShapeMesh>(shape.mesh());
}

class ShapesDemoScene final : public kin::Scene {
public:
    ShapesDemoScene() {
        _world.component<kin::Transform2D>("Transform2D");
        _world.component<kin::ShapeRenderer>("ShapeRenderer");
        _state = std::make_unique<kin::WorldRenderState>(_world);

        std::string error;
        std::vector<std::string> warnings;
        const std::optional<kin::Shape> rocket = kin::read_svg(rocket_svg, &error, &warnings);
        _svg_elements = rocket ? static_cast<int>(rocket->elements.size()) : 0;
        _svg_warnings = static_cast<int>(warnings.size());

        const auto body = [&](const char* name, kin::Transform2D at, std::shared_ptr<const kin::ShapeMesh> mesh,
                              kin::EcsEntity* parent, int order) {
            kin::EcsEntity e = _world.entity(name).set(at).set(kin::ShapeRenderer{.mesh = std::move(mesh), .order = order});
            if (parent) {
                e.child_of(*parent);
            }
            ++_bodies;
            return e;
        };
        _sun = body("sun", {}, mesh_of(sun()), nullptr, 2);
        // Planets ride on invisible arms turning round the sun; moons on arms
        // turning round their planet; all through the hierarchy.
        const std::array<float, 3> distance{150.0f, 230.0f, 320.0f};
        const std::array<kin::Shape, 3> looks{planet(18, kin::Color::rgb(120, 180, 230), kin::Color::rgb(80, 130, 190)),
                                              planet(26, kin::Color::rgb(200, 120, 90), kin::Color::rgb(150, 80, 60)),
                                              planet(14, kin::Color::rgb(150, 210, 140), kin::Color::rgb(90, 160, 90))};
        const auto moon = mesh_of(planet(6, kin::Color::rgb(210, 210, 220), kin::Color::rgb(160, 160, 175)));
        for (std::size_t i = 0; i < distance.size(); ++i) {
            body("orbit", {}, mesh_of(orbit(distance[i])), &_sun, 0);
            kin::EcsEntity arm = _world.entity().set(kin::Transform2D{.rotation = 70.0f * static_cast<float>(i)});
            arm.child_of(_sun);
            _arms.push_back(arm);
            kin::EcsEntity p = body("planet", {.pos = {distance[i], 0}}, mesh_of(looks[i]), &arm, 3);
            if (i != 1) {
                kin::EcsEntity moon_arm = _world.entity().set(kin::Transform2D{});
                moon_arm.child_of(p);
                _arms.push_back(moon_arm);
                body("moon", {.pos = {distance[i] * 0.22f, 0}}, moon, &moon_arm, 4);
            }
        }
        // The rocket orbits wider, scaled and turned to face along its path.
        if (rocket) {
            kin::EcsEntity arm = _world.entity().set(kin::Transform2D{});
            arm.child_of(_sun);
            _arms.push_back(arm);
            body("rocket", {.pos = {400, 0}, .rotation = 180.0f, .scale = {0.9f, 0.9f}}, mesh_of(*rocket), &arm, 5);
        }
        _camera.viewport = logical_size;
        _camera.look_at({0.0f, 0.0f});
    }

    std::string_view name() const override { return "Shapes Demo"; }

    void update(kin::SceneContext& ctx) override {
        if (ctx.window.close_requested() || ctx.input.pressed("quit")) {
            ctx.app.quit();
            return;
        }
        if (ctx.input.pressed("pause")) {
            _paused = !_paused;
        }
        const float dt = _paused ? 0.0f : ctx.dt;
        _time += dt;
        for (std::size_t i = 0; i < _arms.size(); ++i) {
            kin::Transform2D t = *_arms[i].get<kin::Transform2D>();
            t.rotation += dt * (12.0f + 9.0f * static_cast<float>(i % 4)) * (i % 2 == 0 ? 1.0f : 1.6f);
            _arms[i].set(t);
        }
        _camera.rotation += ((ctx.input.held("turn_right") ? 40.0f : 0.0f) - (ctx.input.held("turn_left") ? 40.0f : 0.0f)) * ctx.dt;
        float zoom = std::pow(1.6f, (ctx.input.held("zoom_in") ? ctx.dt : 0.0f) - (ctx.input.held("zoom_out") ? ctx.dt : 0.0f));
        zoom *= std::pow(1.15f, ctx.input.mouse_wheel_y());
        _camera.zoom = std::clamp(_camera.zoom * zoom, 0.3f, 6.0f);
    }

    void render(kin::SceneContext& ctx) override {
        kin::Renderer2D& r = ctx.renderer;
        r.clear(kin::Color::rgb(14, 18, 32));
        _state->propagate_transforms();
        kin::RenderView view;
        view.camera = &_camera;
        view.culling_enabled = true;
        _queue.clear();
        kin::collect_world(*_state, _queue, {}, {.sort_mode = kin::RenderSortMode::LayerThenOrder, .view = &view});
        _queue.flush(r, view);
        draw_hud(r);
    }

    void collect_actions(kin::InputActionContext& actions) const override {
        actions.add("turn_left", "Turn the camera");
        actions.add("turn_right", "Turn the camera");
        actions.add("zoom_in", "Zoom in");
        actions.add("zoom_out", "Zoom out");
        actions.add("pause", "Pause the orbits");
        actions.add("quit", "Quit");
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("svg_elements", static_cast<kin::i64>(_svg_elements));
        json.field("svg_warnings", static_cast<kin::i64>(_svg_warnings));
        json.field("bodies", static_cast<kin::i64>(_bodies));
        json.field("zoom", _camera.zoom);
    }

private:
    void draw_hud(kin::Renderer2D& r) const {
        r.fill_rounded_rect({12, 12, 380, 64}, 8, kin::Color::rgba(10, 12, 20, 200));
        const kin::ui2::Font font = kin::ui2::system_ui_font(15);
        kin::ui2::draw_text(r, font, "SHAPES  orrery", {24, 20}, 18.0f / 15.0f, kin::Color::rgb(226, 232, 240));
        kin::ui2::draw_text(r, font, "Q/E turn   -/= or wheel zoom   Space pause", {24, 48}, 13.0f / 15.0f,
                            kin::Color::rgb(150, 162, 178));
        // A dial: the zoom as an arc, the camera's turn as a needle.
        const kin::Vec2f c{logical_size.x - 60.0f, 60.0f};
        r.fill_circle(c, 34, kin::Color::rgba(10, 12, 20, 200));
        r.draw_arc(c, 26, -90.0f, -90.0f + 360.0f * (_camera.zoom - 0.3f) / 5.7f, kin::Color::rgb(255, 196, 64),
                   {.width = 5, .cap = kin::LineCap::Round});
        const float a = (_camera.rotation - 90.0f) * 3.14159265f / 180.0f;
        r.draw_line(c, {c.x + std::cos(a) * 18.0f, c.y + std::sin(a) * 18.0f}, kin::Color::rgb(226, 232, 240), 2.0f,
                    kin::LineCap::Round);
    }

    kin::EcsWorld _world;
    std::unique_ptr<kin::WorldRenderState> _state;
    kin::RenderQueue _queue;
    kin::Camera2D _camera;
    kin::EcsEntity _sun;
    std::vector<kin::EcsEntity> _arms;
    float _time = 0.0f;
    bool _paused = false;
    int _svg_elements = 0;
    int _svg_warnings = 0;
    int _bodies = 0;
};

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;
    const auto build_scenes = [](kin::SceneManager& target) { target.push(std::make_unique<demo::ShapesDemoScene>()); };
    build_scenes(scenes);
    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .render_headless = true,
        .reset_scenes = build_scenes,
    }, scenes);
}
