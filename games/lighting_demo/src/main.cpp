// 2D lighting demo: a courtyard at night lit by lamps, a campfire, a crystal and
// the player's flashlight, using kin::LightLayer. Drawn in linear light with an
// HDR scene: light adds up past white and is tonemapped (ACES), then graded
// through a LUT for the time of day, cross-faded as it changes, and dithered.
//
//   WASD     move          1-4  dawn / day / dusk / night
//   Mouse    aim the flashlight   F  flashlight on / off
//   C        linear HDR / the old gamma pipeline     G  grading on / off
//   Esc      quit

#include <kin/core/json.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/color_grading.hpp>
#include <kin/renderer/lighting.hpp>
#include <kin/renderer/renderer2d.hpp>
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
#include <string_view>
#include <vector>

namespace demo {
namespace {

constexpr kin::Vec2f logical_size{960.0f, 600.0f};
constexpr kin::Rectf world_area{0.0f, 0.0f, 960.0f, 600.0f};
constexpr float pi = 3.14159265f;

struct TimeOfDay {
    std::string_view name;
    kin::Color ambient;
};

constexpr std::array<TimeOfDay, 4> times{{
    {"dawn", kin::Color::rgb(150, 120, 140)},
    {"day", kin::Color::rgb(255, 255, 255)},
    {"dusk", kin::Color::rgb(120, 80, 90)},
    {"night", kin::Color::rgb(28, 32, 56)},
}};

kin::GameInfo make_game_info() {
    kin::InputMap input;
    input.bind("quit", kin::Key::Escape);
    input.bind("up", kin::Key::W);
    input.bind("down", kin::Key::S);
    input.bind("left", kin::Key::A);
    input.bind("right", kin::Key::D);
    input.bind("flashlight", kin::Key::F);
    input.bind("dawn", kin::Key::Num1);
    input.bind("day", kin::Key::Num2);
    input.bind("dusk", kin::Key::Num3);
    input.bind("night", kin::Key::Num4);
    input.bind("color_space", kin::Key::C);
    input.bind("grading", kin::Key::G);

    return {
        .id = "lighting_demo",
        .title = "Kin Lighting Demo",
        .version = "0.1",
        .description = "Ambient light, point lights and a shaped flashlight with kin::LightLayer.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<kin::i32>(logical_size.x),
            .height = static_cast<kin::i32>(logical_size.y),
            .logical_width = static_cast<kin::i32>(logical_size.x),
            .logical_height = static_cast<kin::i32>(logical_size.y),
            .resizable = true,
            .color_space = kin::ColorSpace::Linear,
            .hdr = true,
        },
        .tags = {"sample", "lighting"},
        .fields = {},
        .input_map = input,
    };
}

kin::Color lerp(kin::Color a, kin::Color b, float t) {
    const auto ch = [t](kin::u8 x, kin::u8 y) {
        return static_cast<kin::u8>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - x) * t));
    };
    return kin::Color::rgb(ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b));
}

// A look for a time of day: a LUT made from the neutral one by `grade`, which
// takes and gives sRGB values from 0 to 1.
template <typename Grade>
std::shared_ptr<const kin::ColorLut> make_look(Grade grade) {
    const kin::ColorLut neutral = kin::ColorLut::neutral(32);
    std::vector<kin::u8> strip{neutral.strip().begin(), neutral.strip().end()};
    for (std::size_t i = 0; i < strip.size(); i += 4) {
        std::array<float, 3> c{strip[i] / 255.0f, strip[i + 1] / 255.0f, strip[i + 2] / 255.0f};
        grade(c);
        for (std::size_t k = 0; k < 3; ++k) {
            strip[i + k] = static_cast<kin::u8>(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return std::make_shared<const kin::ColorLut>(*kin::ColorLut::from_image(strip, {32 * 32, 32}));
}

// Dawn pink, day as it is, dusk warm and contrasty, night blue and muted.
std::array<std::shared_ptr<const kin::ColorLut>, 4> make_looks() {
    const auto luma = [](const std::array<float, 3>& c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
    return {
        make_look([](std::array<float, 3>& c) {
            c = {c[0] * 1.06f + 0.02f, c[1] * 0.97f, c[2] * 1.02f + 0.01f};
        }),
        make_look([](std::array<float, 3>&) {}),
        make_look([](std::array<float, 3>& c) {
            for (float& v : c) {
                v = v + (v - 0.5f) * 0.12f; // a little more contrast
            }
            c = {c[0] * 1.10f, c[1] * 0.96f, c[2] * 0.86f};
        }),
        make_look([&](std::array<float, 3>& c) {
            const float l = luma(c);
            for (float& v : c) {
                v = l + (v - l) * 0.7f; // muted
            }
            c = {c[0] * 0.86f + 0.01f, c[1] * 0.95f + 0.015f, c[2] * 1.12f + 0.03f};
        }),
    };
}

// A deterministic flicker around 1: a few sines of time, no randomness.
float flicker(float time, float seed) {
    return 1.0f + 0.06f * std::sin(time * 11.0f + seed) + 0.04f * std::sin(time * 23.0f + seed * 2.3f) +
           0.03f * std::sin(time * 3.1f + seed * 0.7f);
}

class LightingDemoScene final : public kin::Scene {
public:
    std::string_view name() const override { return "Lighting Demo"; }

    void update(kin::SceneContext& ctx) override {
        if (ctx.window.close_requested() || ctx.input.pressed("quit")) {
            ctx.app.quit();
            return;
        }
        _time += ctx.dt;
        for (std::size_t i = 0; i < times.size(); ++i) {
            if (ctx.input.pressed(times[i].name)) {
                _from = current_ambient();
                _from_look = _blend < 0.5f ? _from_look : _target;
                _target = i;
                _blend = 0.0f;
            }
        }
        _blend = std::min(1.0f, _blend + ctx.dt * 1.5f);
        if (ctx.input.pressed("flashlight")) {
            _flashlight = !_flashlight;
        }
        if (ctx.input.pressed("color_space")) {
            _linear = !_linear;
            ctx.renderer.set_color_space(_linear ? kin::ColorSpace::Linear : kin::ColorSpace::Gamma, _linear);
        }
        if (ctx.input.pressed("grading")) {
            _grading = !_grading;
        }

        const float speed = 180.0f * ctx.dt;
        _player.x += (ctx.input.held("right") ? speed : 0.0f) - (ctx.input.held("left") ? speed : 0.0f);
        _player.y += (ctx.input.held("down") ? speed : 0.0f) - (ctx.input.held("up") ? speed : 0.0f);
        _player.x = std::clamp(_player.x, 20.0f, logical_size.x - 20.0f);
        _player.y = std::clamp(_player.y, 90.0f, logical_size.y - 20.0f);
        const kin::Vec2f mouse = ctx.renderer.window_to_logical(ctx.input.mouse_pos());
        if (mouse.x != 0.0f || mouse.y != 0.0f) {
            _aim = std::atan2(mouse.y - _player.y, mouse.x - _player.x) * 180.0f / pi;
        }
    }

    void render(kin::SceneContext& ctx) override {
        kin::Renderer2D& r = ctx.renderer;
        r.clear(kin::Color::rgb(0, 0, 0));
        draw_world(r);
        const std::vector<kin::Light2D> lights = scene_lights(r);
        _lit = _lighting.apply(r, world_area, current_ambient(), lights);
        draw_hud(r); // after lighting: never darkened
        // Tonemapped and graded on the way to the screen: the time of day's
        // look, cross-faded from the last one.
        r.set_color_output({.exposure = 1.6f,
                            .tonemap = kin::Tonemap::Aces,
                            .lut = _grading ? _looks[_from_look] : nullptr,
                            .lut_to = _grading ? _looks[_target] : nullptr,
                            .lut_mix = _blend,
                            .dither = true});
        _linear = r.color_space() == kin::ColorSpace::Linear;
    }

    void collect_actions(kin::InputActionContext& actions) const override {
        actions.add("flashlight", "Toggle the flashlight");
        actions.add("color_space", "Switch between linear HDR and gamma");
        actions.add("grading", "Toggle colour grading");
        for (const TimeOfDay& t : times) {
            actions.add(t.name, "Set the time of day");
        }
        actions.add("quit", "Quit");
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("time_of_day", times[_target].name);
        const kin::Color a = current_ambient();
        json.key("ambient").begin_array().value(static_cast<kin::i64>(a.r)).value(static_cast<kin::i64>(a.g))
            .value(static_cast<kin::i64>(a.b)).end_array();
        json.field("flashlight", _flashlight);
        json.field("lit", _lit);
        json.field("linear", _linear);
    }

private:
    kin::Color current_ambient() const {
        return lerp(_from, times[_target].ambient, _blend);
    }

    void draw_world(kin::Renderer2D& r) const {
        // Cobbles.
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 24; ++x) {
                const int shade = ((x * 7 + y * 13) % 5) * 6;
                r.fill_rect({x * 40.0f, y * 40.0f, 39.0f, 39.0f},
                            kin::Color::rgb(static_cast<kin::u8>(120 + shade), static_cast<kin::u8>(116 + shade),
                                            static_cast<kin::u8>(108 + shade)));
            }
        }
        // Grass, a pond, buildings and trees.
        r.fill_rect({600.0f, 330.0f, 360.0f, 270.0f}, kin::Color::rgb(86, 140, 70));
        r.fill_rect({690.0f, 420.0f, 170.0f, 110.0f}, kin::Color::rgb(70, 120, 180));
        const std::array<kin::Rectf, 3> houses{{{40.0f, 110.0f, 200.0f, 140.0f},
                                               {300.0f, 110.0f, 160.0f, 120.0f},
                                               {60.0f, 400.0f, 180.0f, 150.0f}}};
        for (const kin::Rectf& h : houses) {
            r.fill_rect({h.x + 8.0f, h.y + 10.0f, h.w, h.h}, kin::Color::rgba(0, 0, 0, 80));
            r.fill_rect(h, kin::Color::rgb(176, 96, 70));
            r.fill_rect({h.x, h.y, h.w, 18.0f}, kin::Color::rgb(140, 70, 52));
            r.draw_rect(h, kin::Color::rgb(90, 50, 40));
        }
        for (const kin::Vec2f t : std::array<kin::Vec2f, 5>{{{640, 360}, {900, 380}, {620, 560}, {900, 560}, {520, 150}}}) {
            r.fill_rect({t.x - 22.0f, t.y - 22.0f, 44.0f, 44.0f}, kin::Color::rgb(40, 100, 50));
            r.fill_rect({t.x - 14.0f, t.y - 14.0f, 28.0f, 28.0f}, kin::Color::rgb(60, 130, 64));
        }
        // Lamp posts, campfire, crystal.
        for (const kin::Vec2f p : lamp_positions) {
            r.fill_rect({p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f}, kin::Color::rgb(60, 60, 64));
            r.fill_rect({p.x - 3.0f, p.y - 3.0f, 6.0f, 6.0f}, kin::Color::rgb(255, 230, 170));
        }
        r.fill_rect({campfire.x - 14.0f, campfire.y - 8.0f, 28.0f, 16.0f}, kin::Color::rgb(90, 60, 40));
        r.fill_rect({campfire.x - 7.0f, campfire.y - 10.0f, 14.0f, 12.0f}, kin::Color::rgb(255, 150, 40));
        r.fill_rect({crystal.x - 8.0f, crystal.y - 14.0f, 16.0f, 28.0f}, kin::Color::rgb(120, 230, 255));
        // The player.
        r.fill_rect({_player.x - 9.0f, _player.y - 9.0f, 18.0f, 18.0f}, kin::Color::rgb(236, 92, 84));
        r.draw_rect({_player.x - 9.0f, _player.y - 9.0f, 18.0f, 18.0f}, kin::Color::rgb(40, 20, 20));
    }

    std::vector<kin::Light2D> scene_lights(kin::Renderer2D& r) {
        std::vector<kin::Light2D> lights;
        for (std::size_t i = 0; i < lamp_positions.size(); ++i) {
            lights.push_back({.position = lamp_positions[i], .radius = 110.0f, .color = kin::Color::rgb(255, 200, 120),
                              .intensity = 0.8f * flicker(_time, static_cast<float>(i))});
        }
        lights.push_back({.position = campfire, .radius = 150.0f, .color = kin::Color::rgb(255, 130, 50),
                          .intensity = 1.3f * flicker(_time * 1.7f, 9.0f)});
        lights.push_back({.position = crystal, .radius = 120.0f, .color = kin::Color::rgb(80, 200, 255),
                          .intensity = 0.8f + 0.2f * std::sin(_time * 1.3f)});
        if (_flashlight) {
            lights.push_back({.position = _player, .radius = 320.0f, .color = kin::Color::rgb(255, 250, 230),
                              .intensity = 1.6f, .shape = cone(r), .rotation = _aim});
            // A faint glow around the player so its surroundings are not black.
            lights.push_back({.position = _player, .radius = 60.0f, .color = kin::Color::rgb(255, 250, 230),
                              .intensity = 0.35f});
        }
        return lights;
    }

    // A flashlight beam pointing along +x, fading with distance and at its edges.
    const kin::Texture& cone(kin::Renderer2D& r) {
        if (_cone.valid()) {
            return _cone;
        }
        constexpr int size = 128;
        constexpr float half_angle = 0.42f; // radians
        std::vector<kin::u8> px(static_cast<std::size_t>(size * size * 4));
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float dx = (x + 0.5f - size * 0.5f) / (size * 0.5f);
                const float dy = (y + 0.5f - size * 0.5f) / (size * 0.5f);
                const float d = std::sqrt(dx * dx + dy * dy);
                const float angle = std::abs(std::atan2(dy, dx));
                const float edge = std::clamp((half_angle - angle) / 0.12f, 0.0f, 1.0f);
                const float reach = std::clamp(1.0f - d, 0.0f, 1.0f);
                const float a = dx > 0.0f ? edge * reach : 0.0f;
                kin::u8* p = &px[static_cast<std::size_t>((y * size + x) * 4)];
                p[0] = p[1] = p[2] = 255;
                p[3] = static_cast<kin::u8>(std::lround(a * 255.0f));
            }
        }
        _cone = r.create_texture_from_rgba(px.data(), {size, size});
        r.set_scale_mode(_cone, kin::ScaleMode::Linear);
        return _cone;
    }

    void draw_hud(kin::Renderer2D& r) const {
        r.fill_rect({0.0f, 0.0f, logical_size.x, 70.0f}, kin::Color::rgba(12, 14, 20, 220));
        const kin::ui2::Font font = kin::ui2::system_ui_font(15);
        const auto text = [&](std::string_view value, float x, float y, float points, kin::Color color) {
            kin::ui2::draw_text(r, font, value, {x, y}, points / 15.0f, color);
        };
        std::string title = "LIGHTING  ";
        title += times[_target].name;
        if (!_lit) {
            title += "  (unlit: this backend has no render targets)";
        }
        title += _linear ? "   linear HDR" : "   gamma";
        title += _grading ? ", graded" : "";
        text(title, 20.0f, 12.0f, 20.0f, kin::Color::rgb(226, 232, 240));
        text("WASD: move    Mouse: aim    F: flashlight    1-4: dawn / day / dusk / night    C: linear / gamma    G: grading",
             20.0f, 42.0f, 13.0f, kin::Color::rgb(150, 162, 178));
    }

    static constexpr std::array<kin::Vec2f, 4> lamp_positions{{{270, 300}, {500, 300}, {270, 560}, {500, 560}}};
    static constexpr kin::Vec2f campfire{400.0f, 440.0f};
    static constexpr kin::Vec2f crystal{780.0f, 190.0f};

    kin::LightLayer _lighting;
    kin::Texture _cone;
    float _time = 0.0f;
    std::size_t _target = 3; // night
    kin::Color _from = times[3].ambient;
    float _blend = 1.0f;
    std::size_t _from_look = 3;
    std::array<std::shared_ptr<const kin::ColorLut>, 4> _looks = make_looks();
    bool _linear = true;
    bool _grading = true;
    bool _flashlight = true;
    bool _lit = false;
    kin::Vec2f _player{380.0f, 360.0f};
    float _aim = -30.0f;
};

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [](kin::SceneManager& target) {
        target.push(std::make_unique<demo::LightingDemoScene>());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .render_headless = true, // headless runs draw too, so the report shows the lighting ran
        .reset_scenes = build_scenes,
    }, scenes);
}
