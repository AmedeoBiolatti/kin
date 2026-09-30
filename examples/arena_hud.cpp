#include "workloads.hpp"

#include <kin/anim/track.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace examples {
namespace {

constexpr Color ink = Color::rgb(226, 236, 242);
constexpr Color muted = Color::rgb(146, 170, 186);
constexpr Color teal = Color::rgb(103, 226, 206);
constexpr Color glass = Color::rgba(8, 14, 22, 205);
constexpr Color rim = Color::rgba(90, 200, 190, 80);
constexpr Color track = Color::rgba(30, 40, 50, 230);

constexpr std::array<std::string_view, 6> wave_notes{
    "HOLD THE SECTOR", "HOSTILES ACCELERATE", "PRESSURE RISING", "HALFWAY THERE", "FINAL PUSH", "LAST STAND"};

// The wave banner's motion, as kin animation property tracks: it fades in while
// settling from a larger size, holds, then fades out.
const PropertyTrack& banner_alpha() {
    static const PropertyTrack track{.property = "alpha", .keys = {
        {.time = 0, .value = 0.0f}, {.time = .25f, .value = 1.0f, .easing = Easing::EaseOut},
        {.time = 1.8f, .value = 1.0f}, {.time = 2.3f, .value = 0.0f, .easing = Easing::EaseIn}}};
    return track;
}
const PropertyTrack& banner_scale() {
    static const PropertyTrack track{.property = "scale", .keys = {
        {.time = 0, .value = 1.35f}, {.time = .4f, .value = 1.0f, .easing = Easing::EaseOut}}};
    return track;
}
float sample_f32(const PropertyTrack& t, float time) { return std::get<f32>(sample(t, time)); }

Color with_alpha(Color c, float alpha) {
    c.a = static_cast<u8>(std::clamp(static_cast<float>(c.a) * alpha, 0.0f, 255.0f));
    return c;
}
Color blend(Color a, Color b, float t) {
    const auto ch = [t](u8 x, u8 y) { return static_cast<u8>(std::lround(x + (float(y) - x) * t)); };
    return Color::rgba(ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), ch(a.a, b.a));
}

} // namespace

void ArenaHud::render(const Arena& arena, Renderer2D& renderer, Input& input, const HudState& state, float dt) {
    _wave_started = false;
    const auto native = renderer.scoped_native_coordinates();
    const auto output = renderer.output_size();
    if (output.x <= 0 || output.y <= 0) return;
    // SDL exposes the complete window here, while GPU exposes the fitted scene
    // texture. Aspect-fitting works for both and keeps HUD out of letterbox bars.
    const float fit = std::min(output.x / 1280.0f, output.y / 800.0f);
    const Rectf area{(output.x - 1280 * fit) * .5f, (output.y - 800 * fit) * .5f, 1280 * fit, 800 * fit};
    const auto viewport = renderer.scoped_viewport(area);
    const float dpi = std::isfinite(state.display_scale) && state.display_scale > 0 ? state.display_scale : 1;
    const float w = area.w / dpi, h = area.h / dpi;

    // Text is rasterized at its on-screen size (up to the system font's 32 pt).
    const auto style = [&](float points, Color color) {
        const float size = std::min(32.0f, points * dpi);
        return ui2::TextStyle{.font = ui2::system_ui_font(size), .scale = points * dpi / size, .color = color};
    };
    const auto px = [&](Rectf r) { return Rectf{r.x * dpi, r.y * dpi, r.w * dpi, r.h * dpi}; };
    const auto text = [&](std::string_view value, float x, float y, float points, Color color) {
        _ui.text(value, {x * dpi, y * dpi}, style(points, color));
    };
    const auto width = [&](std::string_view value, float points) {
        const ui2::TextStyle s = style(points, ink);
        return ui2::measure_text(s.font, value, s.scale).x / dpi;
    };
    const auto centered = [&](std::string_view value, float y, float points, Color color) {
        text(value, (w - width(value, points)) * .5f, y, points, color);
    };
    const auto panel = [&](Rectf r) {
        ui2::Panel p{.bounds = px(r), .color = glass, .border = rim};
        ui2::run(_ui, p);
    };
    const auto bar = [&](Rectf r, float value, Color fill) {
        ui2::ProgressBar b{.bounds = px(r), .value = std::clamp(value, 0.0f, 1.0f), .fill = fill, .background = track};
        ui2::run(_ui, b);
    };

    _ui.begin(input, renderer, dt);

    // Hull and dash, top left. The hull bar shifts green -> amber -> red, and
    // flashes briefly when hit.
    if (_health >= 0 && arena.health < _health) _hurt = .35f;
    _health = arena.health;
    _hurt = std::max(0.0f, _hurt - dt);
    const float hull = std::clamp(float(arena.health) / float(arena.max_health()), 0.0f, 1.0f);
    Color hull_color = hull > .5f ? Color::rgb(102, 210, 163) : hull > .25f ? Color::rgb(236, 190, 90) : Color::rgb(240, 96, 90);
    if (_hurt > 0) hull_color = blend(hull_color, Color::rgb(255, 170, 160), _hurt / .35f);
    panel({16, 16, 300, 86});
    text("SIGNAL SIEGE", 30, 23, 13, teal);
    text("HULL", 30, 46, 11, muted);
    bar({78, 50, 188, 9}, hull, hull_color);
    text(std::to_string(std::max(0, arena.health)), 276, 45, 12, ink);
    const float ready = 1 - std::clamp(arena.dash_cooldown / arena.power_stats().dash_cooldown, 0.0f, 1.0f);
    text("DASH", 30, 68, 11, muted);
    bar({78, 73, 188, 4}, ready, ready >= 1 ? Color::rgb(120, 220, 255) : Color::rgb(66, 124, 164));
    if (ready >= 1) text("READY", 276, 66, 10, Color::rgb(120, 220, 255));

    // Countdown and wave progress, top centre.
    const int remaining = std::max(0, 90 - int(arena.elapsed));
    char timer[16];
    std::snprintf(timer, sizeof timer, "%02d:%02d", remaining / 60, remaining % 60);
    centered(timer, 12, 30, remaining <= 10 ? Color::rgb(255, 214, 140) : ink);
    const int wave = std::min(6, 1 + int(arena.elapsed / 15));
    centered("WAVE " + std::to_string(wave) + " / 6", 54, 12, muted);
    bar({w * .5f - 110, 75, 220, 3}, arena.elapsed / 90, teal);

    // Kills and cores, top right: rolling counters.
    panel({w - 236, 16, 220, 86});
    _kills.value = float(arena.kills);
    _cores.value = float(arena.available_cores());
    ui2::step_animated_value(_kills, dt);
    ui2::step_animated_value(_cores, dt);
    _kills.bounds = px({w - 222, 22, 96, 32});
    _kills.text_style = style(26, ink);
    ui2::run(_ui, _kills);
    text("KILLS", w - 222, 58, 11, muted);
    _cores.bounds = px({w - 118, 22, 96, 32});
    _cores.text_style = style(26, Color::rgb(120, 255, 180));
    ui2::run(_ui, _cores);
    text("CORES", w - 118, 58, 11, muted);
    int upgrades = 0;
    for (int id = 1; id < int(power_ups.size()); ++id) upgrades += arena.can_buy_power(id) ? 1 : 0;
    if (upgrades > 0) {
        const float pulse = .6f + .4f * std::sin(arena.elapsed * 5);
        text("[U] " + std::to_string(upgrades) + (upgrades == 1 ? " UPGRADE READY" : " UPGRADES READY"), w - 222, 78, 10,
             with_alpha(teal, pulse));
    }

    // Wave banner: shown when a wave begins, driven by the property tracks.
    if (wave != _wave) {
        _wave = wave;
        _banner = 0;
        _wave_started = true;
    }
    if (_banner >= 0) {
        _banner += dt;
        if (_banner > 2.4f) {
            _banner = -1;
        } else {
            const float alpha = sample_f32(banner_alpha(), _banner);
            const float scale = sample_f32(banner_scale(), _banner);
            centered("WAVE " + std::to_string(wave), h * .3f - 20 * scale, 40 * scale, with_alpha(Color::rgb(236, 246, 250), alpha));
            centered(wave_notes[std::size_t(wave - 1)], h * .3f + 36, 14, with_alpha(teal, alpha));
        }
    }

    if (state.reticle) {
        const Vec2f at{state.aim.x * fit, state.aim.y * fit};
        const float r = 11 * dpi;
        ui2::TargetReticle reticle{.bounds = {at.x - r, at.y - r, r * 2, r * 2}, .valid_color = Color::rgb(183, 239, 228), .corner = 6 * dpi};
        ui2::run(_ui, reticle);
    }

    // Controls fade out after the opening seconds (and return while paused).
    const float help = state.paused ? 1 : std::clamp((12 - arena.elapsed) / 2, 0.0f, 1.0f);
    if (help > 0) {
        constexpr std::string_view controls = "WASD move / mouse + hold LMB fire / SPACE dash / U power grid / P pause / R restart / B autoplay";
        const ui2::TextStyle s = style(13, with_alpha(muted, help));
        const auto lines = ui2::wrap_text(s.font, controls, (w - 380) * dpi, s.scale);
        float y = h - 20 - float(lines.size()) * 20;
        for (const auto& line : lines) {
            text(line, 20, y, 13, with_alpha(muted, help));
            y += 20;
        }
    }
    const std::string stats = std::to_string(arena.enemy_count()) + " enemies   " + std::to_string(arena.shots.size()) +
        " shots   " + std::to_string(state.commands) + " draws   F1 timings" + (state.autoplay ? "   AUTOPLAY" : "");
    text(stats, w - 20 - width(stats, 11), h - 26, 11, Color::rgb(110, 150, 168));

    if (state.paused || arena.health <= 0 || (arena.won() && !state.autoplay)) {
        const float panel_w = std::min(480.0f, w - 40);
        ui2::Panel p{.bounds = px({(w - panel_w) * .5f, h * .5f - 75, panel_w, 150}), .color = Color::rgba(12, 22, 32, 238),
                     .border = rim};
        ui2::run(_ui, p);
        centered(state.paused ? "PAUSED" : arena.health <= 0 ? "SIGNAL LOST" : "SECTOR SECURED", h * .5f - 46, 26,
                 Color::rgb(232, 211, 164));
        centered(state.paused ? "P resume / R new run" : "R new run", h * .5f + 16, 15, muted);
    }
    _ui.end();
}

void render_arena_hud(const Arena& arena, Renderer2D& renderer, bool paused, bool autoplay, int commands, float display_scale) {
    Input input;
    ArenaHud hud;
    hud.render(arena, renderer, input, {.paused = paused, .autoplay = autoplay, .commands = commands, .display_scale = display_scale}, 0);
}

} // namespace examples
