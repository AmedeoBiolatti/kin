#include "workloads.hpp"
#include <algorithm>
#include <cmath>

namespace examples {
void render_arena_hud(const Arena& arena, Renderer2D& renderer, bool paused,
                      bool autoplay, int commands, float display_scale) {
    const auto native = renderer.scoped_native_coordinates();
    const auto output = renderer.output_size();
    if (output.x <= 0 || output.y <= 0) return;
    // SDL exposes the complete window here, while GPU exposes the fitted scene
    // texture. Aspect-fitting works for both and keeps HUD out of letterbox bars.
    const float fit = std::min(output.x / 1280.0f, output.y / 800.0f);
    const Rectf area{(output.x - 1280 * fit) * .5f, (output.y - 800 * fit) * .5f,
                     1280 * fit, 800 * fit};
    const auto viewport = renderer.scoped_viewport(area);
    const float dpi = std::isfinite(display_scale) && display_scale > 0 ? display_scale : 1;
    const float w = area.w / dpi, h = area.h / dpi;
    const auto font = ui2::system_ui_font(15);
    constexpr auto ink = Color::rgb(225, 233, 240), muted = Color::rgb(154, 176, 192);
    constexpr auto surface = Color::rgba(10, 17, 25, 235);
    const auto fill = [&](Rectf r, Color color) {
        renderer.fill_rect({r.x * dpi, r.y * dpi, r.w * dpi, r.h * dpi}, color);
    };
    const auto text = [&](std::string_view value, float x, float y, float points, Color color) {
        ui2::draw_text(renderer, font, value, {x * dpi, y * dpi}, points / 15 * dpi, color);
    };
    const bool compact = w < 1000;
    fill({0, 0, w, compact ? 94.0f : 76.0f}, surface);
    fill({0,0,4,compact?94.0f:76.0f},Color::rgb(103,222,200));
    text("SIGNAL SIEGE", 20, 12, 22, Color::rgb(111, 229, 210));
    fill({20, 48, 240, 8}, Color::rgb(45, 55, 64));
    fill({20, 48, 240.0f * std::clamp(arena.health, 0, arena.max_health()) / arena.max_health(), 8}, Color::rgb(102, 210, 163));
    for (int i=1;i<10;++i) fill({20+24.0f*i,48,2,8},surface);
    const std::string state = "WAVE " + std::to_string(1 + int(arena.elapsed / 15)) + "   KILLS " + std::to_string(arena.kills)
        + "   CORES " + std::to_string(arena.available_cores()) + "   SURVIVE " + std::to_string(std::max(0, 90 - int(arena.elapsed))) + "s";
    text(state, compact ? 20 : 290, compact ? 66 : 22, compact ? 14 : 16, ink);
    if (!compact) {
        const float ready=1-std::clamp(arena.dash_cooldown/arena.power_stats().dash_cooldown,0.0f,1.0f);
        text(ready>=1?"DASH READY":"DASH CHARGING",290,48,11,muted);
        fill({408,50,100,4},Color::rgb(45,55,64));
        fill({408,50,100*ready,4},Color::rgb(108,183,224));
        text("[U] POWER GRID",540,46,12,Color::rgb(111,229,210));
    }
    const float header_h=compact?94.0f:76.0f;
    fill({0,header_h,w,2},Color::rgb(37,56,69));
    fill({0,header_h,w*std::clamp(arena.elapsed/90,0.0f,1.0f),2},Color::rgb(102,184,174));

    // Wrap help rather than shrink it to unreadable bitmap sizes in small windows.
    constexpr std::string_view controls = "WASD move / mouse + hold LMB fire / SPACE dash / U power grid / P pause / R restart / B autoplay";
    const auto lines = ui2::wrap_text(font, controls, (w - 40) * dpi, 14.0f / 15 * dpi);
    const float footer_h = 34 + float(lines.size()) * 22;
    fill({0, h - footer_h, w, footer_h}, surface);
    float y = h - footer_h + 8;
    for (const auto& line : lines) { text(line, 20, y, 14, muted); y += 22; }
    const std::string stats = std::to_string(arena.enemy_count()) + " enemies   " + std::to_string(arena.shots.size()) + " shots   "
        + std::to_string(commands) + " commands   F1 timings" + (autoplay ? "   [AUTOPLAY]" : "");
    text(stats, 20, h - 23, 12, Color::rgb(130, 177, 195));

    if (paused || arena.health <= 0 || (arena.won() && !autoplay)) {
        const float panel_w = std::min(480.0f, w - 40);
        fill({(w - panel_w) * .5f, h * .5f - 75, panel_w, 150}, Color::rgba(16, 29, 39, 240));
        ui2::draw_text_centered(renderer, font, paused ? "PAUSED" : arena.health <= 0 ? "SIGNAL LOST" : "SECTOR SECURED",
                               {area.w * .5f, area.h * .5f - 28 * dpi}, 26.0f / 15 * dpi, Color::rgb(232, 211, 164));
        ui2::draw_text_centered(renderer, font, paused ? "P resume / R new run" : "R new run",
                               {area.w * .5f, area.h * .5f + 28 * dpi}, 15.0f / 15 * dpi, muted);
    }
}
}
