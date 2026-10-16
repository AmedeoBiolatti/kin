#include "hud_draw.hpp"

#include <kin/ui2/text.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <cmath>

namespace summit {

namespace {

const Color ink = Color::rgb(238, 242, 246);
const Color muted = Color::rgb(160, 172, 186);
const Color glass = Color::rgba(10, 14, 20, 176);
const Color rim = Color::rgba(90, 104, 124, 200);
const Color track = Color::rgba(0, 0, 0, 150);

Color with_alpha(Color c, f32 a) { return Color::rgba(c.r, c.g, c.b, u8(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }

// A filled rectangle (a panel with no visible border).
void fill(ui2::Context& ui, Rectf r, Color c) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    ui2::Panel p{.bounds = r, .color = c, .border = c};
    ui2::run(ui, p);
}
void box(ui2::Context& ui, Rectf r, Color c, Color border) {
    ui2::Panel p{.bounds = r, .color = c, .border = border};
    ui2::run(ui, p);
}

// Text in a rectangle, at `points` (scaled by the layout's u), shrunk to fit its width.
void text_in(ui2::Context& ui, std::string_view value, Rectf r, f32 points, f32 u, Color colour, ui2::UiAlign horizontal = ui2::UiAlign::Start,
             bool bold = false) {
    if (value.empty() || r.w <= 0.0f) return;
    const f32 size = std::max(6.0f, points * u);
    ui2::TextStyle st{.font = bold ? ui2::system_ui_font_bold(size) : ui2::system_ui_font(size), .scale = 1.0f, .color = colour};
    Vec2f m = ui2::measure_text(st.font, value, 1.0f);
    if (m.x > r.w && m.x > 0.0f) {
        st.scale = r.w / m.x;
        m = ui2::measure_text(st.font, value, st.scale);
    }
    f32 x = r.x;
    if (horizontal == ui2::UiAlign::Center) x = r.x + (r.w - m.x) * 0.5f;
    else if (horizontal == ui2::UiAlign::End) x = r.x + r.w - m.x;
    ui.text(value, {x, r.y + (r.h - m.y) * 0.5f}, st);
}

Rectf shrink(Rectf r, f32 a) { return {r.x + a, r.y + a, std::max(0.0f, r.w - 2 * a), std::max(0.0f, r.h - 2 * a)}; }

void bar(ui2::Context& ui, Rectf r, f32 fraction, Color colour, f32 u) {
    box(ui, r, track, rim);
    const Rectf in = shrink(r, std::max(1.0f, 2.0f * u));
    fill(ui, {in.x, in.y, in.w * std::clamp(fraction, 0.0f, 1.0f), in.h}, colour);
}

const char* fall_text(FallWarning w) {
    switch (w) {
    case FallWarning::Brace: return "BRACE";
    case FallWarning::Hurt: return "BRACE: HARD LANDING";
    case FallWarning::KnockOut: return "BRACE: KNOCK-OUT";
    default: return "";
    }
}

} // namespace

void draw_hud(ui2::Context& ui, const HudModel& m, const HudLayout& l, const InputBindings& bindings, f64 time, const std::string& debug) {
    const f32 u = l.u;
    char buf[96];
    const auto interact_keys = [&]() {
        std::string value;
        for (const Binding& b : bindings.bindings(Action::Interact)) {
            if (!value.empty()) value += " / ";
            value += binding_label(b);
        }
        return value.empty() ? std::string("Unbound") : value;
    };
    if (m.expedition && (m.phase == ExpeditionPhase::Briefing || m.phase == ExpeditionPhase::Complete)) {
        fill(ui, {0, 0, l.screen.x, l.screen.y}, Color::rgba(4, 8, 14, 150));
        box(ui, l.expedition_panel, Color::rgba(10, 18, 28, 235), Color::rgb(226, 183, 84));
        const Rectf in = shrink(l.expedition_panel, 28 * u);
        const auto row = [&](f32 y, f32 h) { return Rectf{in.x, in.y + y * u, in.w, h * u}; };
        text_in(ui, m.objective, row(0, 44), 30, u, Color::rgb(250, 214, 120), ui2::UiAlign::Center, true);
        text_in(ui, m.objective_detail, row(49, 30), 17, u, ink, ui2::UiAlign::Center);
        if (m.phase == ExpeditionPhase::Briefing) {
            text_in(ui, "ORANGE TRAIL: a short, exposed climb. BLUE TRAIL: a longer cave passage.", row(95, 27), 16, u, ink);
            text_in(ui, "Rest in the cave or at a camp to recover. Camps are your fall checkpoints.", row(127, 27), 16, u, ink);
            const auto keys = [&](Action a) {
                const auto& b = bindings.bindings(a);
                return b.empty() ? std::string("Unbound") : binding_label(b.front());
            };
            text_in(ui, keys(Action::Forward) + " move forward   " + keys(Action::Jump) + " jump   " + keys(Action::Sprint) + " sprint", row(166, 25), 15, u, muted);
            text_in(ui, keys(Action::GrabLeft) + " / " + keys(Action::GrabRight) + " hold hands; move to climb   " + keys(Action::PullUp) + " pull up", row(195, 25), 15, u, muted);
            text_in(ui, "Press " + interact_keys() + " to begin", row(224, 24), 14, u, ink, ui2::UiAlign::Center);
        } else {
            std::snprintf(buf, sizeof buf, "Time %s    Falls %d", clock_text(m.run_seconds).c_str(), m.falls);
            text_in(ui, buf, row(102, 32), 22, u, ink, ui2::UiAlign::Center);
            text_in(ui, m.shelter_visited ? "Cave shelter discovered" : "Direct ascent / cave shelter undiscovered", row(150, 30), 16, u, muted, ui2::UiAlign::Center);
            text_in(ui, "Enter / pad A: try again    Esc / Start: menu", row(224, 24), 14, u, ink, ui2::UiAlign::Center);
        }
        return;
    }
    // Altitude and status (top left).
    {
        box(ui, l.altitude, glass, rim);
        const Rectf in = shrink(l.altitude, 8 * u);
        std::snprintf(buf, sizeof buf, "%d m", int(std::lround(m.altitude)));
        text_in(ui, buf, {in.x, in.y, in.w * 0.55f, in.h * 0.5f}, 22.0f, u, ink, ui2::UiAlign::Start, true);
        std::snprintf(buf, sizeof buf, "best %d m", int(std::lround(m.best_altitude)));
        text_in(ui, buf, {in.x + in.w * 0.55f, in.y, in.w * 0.45f, in.h * 0.5f}, 12.0f, u, muted, ui2::UiAlign::End);
        // The climb: the share of the way to the top.
        const Rectf pr{in.x, in.y + in.h * 0.55f, in.w, in.h * 0.12f};
        bar(ui, pr, m.progress, Color::rgb(110, 170, 230), u);
        text_in(ui, status_text(m.status), {in.x, in.y + in.h * 0.7f, in.w, in.h * 0.3f}, 11.0f, u, muted);
    }
    // Health (top right).
    {
        box(ui, l.health, glass, m.hurt_flash > 0.0f ? with_alpha(Color::rgb(230, 60, 50), 0.4f + 0.6f * m.hurt_flash) : rim);
        const Rectf in = shrink(l.health, 8 * u);
        text_in(ui, "Health", {in.x, in.y, in.w * 0.5f, in.h * 0.4f}, 11.0f, u, muted);
        text_in(ui, m.injury.empty() ? "Fit" : m.injury, {in.x + in.w * 0.4f, in.y, in.w * 0.6f, in.h * 0.4f}, 11.0f, u,
                m.injury.empty() ? muted : m.health_colour, ui2::UiAlign::End);
        bar(ui, {in.x, in.y + in.h * 0.5f, in.w, in.h * 0.5f}, m.health_fraction, m.health_colour, u);
    }
    // Stamina (bottom centre): the bar, what injury has taken off its end, a pulse when nearly gone.
    {
        const Rectf r = l.stamina;
        box(ui, r, track, rim);
        const Rectf in = shrink(r, std::max(1.0f, 2.0f * u));
        // What injury took: the end of the bar, dimmed and hatched in a darker red.
        if (m.cap_fraction < 0.995f) {
            const f32 x = in.x + in.w * m.cap_fraction;
            fill(ui, {x, in.y, in.x + in.w - x, in.h}, Color::rgba(120, 30, 30, 170));
        }
        f32 pulse = 1.0f;
        if (m.stamina_pulse) pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(f32(time) * 10.0f));
        fill(ui, {in.x, in.y, in.w * std::clamp(m.stamina_fraction, 0.0f, 1.0f), in.h}, with_alpha(m.stamina_colour, pulse));
        std::snprintf(buf, sizeof buf, m.exhausted ? "Spent  %d" : "Stamina  %d", int(std::lround(m.stamina)));
        text_in(ui, buf, {r.x + 8 * u, r.y, r.w - 16 * u, r.h}, 12.0f, u, ink);
    }
    // The hands: grey open, amber reaching, green holding, red slipping.
    {
        const auto hand = [&](Rectf r, HandState s, const char* name) {
            Color c = Color::rgba(30, 36, 46, 190);
            Color b = rim;
            if (s == HandState::Held) c = Color::rgb(96, 190, 112), b = Color::rgb(190, 240, 200);
            else if (s == HandState::Reaching) c = Color::rgb(232, 178, 60), b = Color::rgb(250, 220, 140);
            else if (s == HandState::Slipping) c = Color::rgb(220, 70, 56), b = Color::rgb(255, 170, 150);
            box(ui, r, c, b);
            // A palm and four fingers when it holds.
            if (s != HandState::Free) {
                const Rectf in = shrink(r, 6 * u);
                for (i32 f = 0; f < 4; ++f) {
                    const f32 fw = in.w / 5.0f;
                    fill(ui, {in.x + fw * 0.2f + f32(f) * fw * 1.2f, in.y, fw * 0.9f, in.h * 0.55f}, with_alpha(ink, 0.9f));
                }
            }
            text_in(ui, name, {r.x, r.y + r.h * 0.55f, r.w, r.h * 0.45f}, 11.0f, u, s == HandState::Free ? muted : Color::rgb(10, 14, 20), ui2::UiAlign::Center, true);
        };
        hand(l.hand_left, m.left, "L");
        hand(l.hand_right, m.right, "R");
    }
    // The fall warning, or the state of being down.
    {
        const char* t = fall_text(m.fall_warning);
        Color c = m.fall_warning == FallWarning::Brace ? Color::rgb(236, 190, 64) : m.fall_warning == FallWarning::Hurt ? Color::rgb(240, 130, 50) : Color::rgb(236, 70, 56);
        if (t[0] == '\0') {
            if (m.status == StatusKind::KnockedOut) t = "KNOCKED OUT", c = Color::rgb(236, 70, 56);
            else if (m.status == StatusKind::GettingUp) t = "Getting up", c = Color::rgb(236, 190, 64);
            else if (m.status == StatusKind::Dead) t = "You fell. Back to camp", c = Color::rgb(236, 70, 56);
        }
        if (t[0] != '\0') {
            const f32 pulse = 0.65f + 0.35f * (0.5f + 0.5f * std::sin(f32(time) * 14.0f));
            box(ui, l.warning, with_alpha(Color::rgb(10, 12, 16), 0.72f), with_alpha(c, pulse));
            text_in(ui, t, shrink(l.warning, 6 * u), 20.0f, u, with_alpha(c, 0.7f + 0.3f * pulse), ui2::UiAlign::Center, true);
            if (m.fall_warning != FallWarning::None) {
                std::snprintf(buf, sizeof buf, "%.1f m/s", double(m.fall_speed));
                text_in(ui, buf, {l.warning.x, l.warning.y + l.warning.h * 0.72f, l.warning.w, l.warning.h * 0.28f}, 9.0f, u, muted, ui2::UiAlign::Center);
            }
        }
    }
    // Camp notice (top centre), fading.
    if (!m.camp_notice.empty()) {
        box(ui, l.notice, with_alpha(Color::rgb(10, 14, 20), 0.7f * m.camp_notice_alpha), with_alpha(Color::rgb(120, 200, 140), m.camp_notice_alpha));
        text_in(ui, m.camp_notice, shrink(l.notice, 6 * u), 17.0f, u, with_alpha(ink, m.camp_notice_alpha), ui2::UiAlign::Center, true);
    }
    // The summit.
    if (m.summit && !m.expedition) {
        box(ui, l.summit, with_alpha(Color::rgb(8, 12, 18), 0.82f), Color::rgb(240, 200, 70));
        const Rectf in = shrink(l.summit, 10 * u);
        text_in(ui, "SUMMIT", {in.x, in.y, in.w, in.h * 0.45f}, 30.0f, u, Color::rgb(250, 214, 90), ui2::UiAlign::Center, true);
        std::snprintf(buf, sizeof buf, "Time %s     Falls %d", clock_text(m.run_seconds).c_str(), m.falls);
        text_in(ui, buf, {in.x, in.y + in.h * 0.48f, in.w, in.h * 0.28f}, 16.0f, u, ink, ui2::UiAlign::Center);
        text_in(ui, "Esc: menu    restart from camp to try again", {in.x, in.y + in.h * 0.76f, in.w, in.h * 0.24f}, 10.0f, u, muted, ui2::UiAlign::Center);
    }
    if (m.expedition) {
        box(ui, l.objective, glass, m.sheltered ? Color::rgb(88, 176, 214) : rim);
        const Rectf r = shrink(l.objective, 8 * u);
        text_in(ui, m.objective, {r.x, r.y, r.w, r.h * 0.55f}, 16, u, ink, ui2::UiAlign::Center, true);
        text_in(ui, m.objective_detail, {r.x, r.y + r.h * 0.55f, r.w, r.h * 0.45f}, 12, u, muted, ui2::UiAlign::Center);
        if (!m.interaction.empty()) {
            box(ui, l.interaction, glass, Color::rgb(226, 183, 84));
            const bool recover = m.sheltered;
            const std::string hint = recover ? m.interaction : interact_keys() + ": " + m.interaction;
            text_in(ui, hint, shrink(l.interaction, 8 * u), 16, u, ink, ui2::UiAlign::Center, true);
            if (!recover) bar(ui, {l.interaction.x + 8 * u, l.interaction.y + l.interaction.h - 8 * u, l.interaction.w - 16 * u, 4 * u},
                              m.interaction_progress, Color::rgb(250, 214, 120), u);
        } else if (m.hanging) {
            text_in(ui, "Hold both hands and move up. Rest before committing to the next pitch.", l.interaction, 14, u, ink, ui2::UiAlign::Center);
        }
    }
    // The hint line.
    text_in(ui, hud_hint_line(bindings), l.hint, 10.0f, u, with_alpha(muted, 0.85f));
    if (!debug.empty()) {
        box(ui, l.debug, with_alpha(Color::rgb(8, 11, 16), 0.72f), rim);
        text_in(ui, debug, shrink(l.debug, 3 * u), 10.0f, u, muted, ui2::UiAlign::Center);
    }
}

void draw_menu(ui2::Context& ui, const Menu& menu, const MenuLayout& l, const InputBindings& bindings, Vec2f screen, Vec2f pointer) {
    fill(ui, {0.0f, 0.0f, screen.x, screen.y}, Color::rgba(4, 6, 10, 120));
    const f32 u = l.u;
    box(ui, l.panel, Color::rgba(12, 16, 24, 236), Color::rgb(92, 110, 136));
    if (menu.screen() == MenuScreen::Controls) {
        text_in(ui, "Controls", l.title, 22.0f, u, ink, ui2::UiAlign::Start, true);
        const std::vector<ControlLine> lines = controls_screen(bindings);
        const f32 row_h = (l.rows.empty() ? 30.0f * u : l.rows.front().h);
        // The same rows as the main menu, as many as there are lines.
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const Rectf r{l.panel.x + 16 * u, l.title.y + l.title.h + row_h * f32(i), l.panel.w - 32 * u, row_h};
            text_in(ui, lines[i].action, {r.x, r.y, r.w * 0.38f, r.h}, 13.0f, u, muted);
            text_in(ui, lines[i].keys, {r.x + r.w * 0.40f, r.y, r.w * 0.60f, r.h}, 13.0f, u, ink);
        }
        char b[160];
        std::snprintf(b, sizeof b, "Look: mouse (%.2f deg a pixel) or right stick%s    Esc or Enter: back", double(bindings.mouse_sensitivity),
                      bindings.invert_y ? ", inverted" : "");
        text_in(ui, b, l.footer, 10.0f, u, muted);
        return;
    }
    text_in(ui, "SUMMIT   paused", l.title, 22.0f, u, ink, ui2::UiAlign::Start, true);
    for (std::size_t i = 0; i < l.rows.size() && i < menu_row_count; ++i) {
        const MenuRow row = MenuRow(i);
        const Rectf r = l.rows[i];
        const bool focus = menu.focus() == i32(i);
        const bool hover = ui2::contains(r, pointer);
        if (focus) box(ui, r, with_alpha(Color::rgb(70, 100, 150), 0.55f), Color::rgb(130, 170, 230));
        else if (hover) fill(ui, r, with_alpha(Color::rgb(70, 100, 150), 0.25f));
        const Rectf in = shrink(r, 4 * u);
        text_in(ui, menu.label(row), {in.x + 4 * u, in.y, in.w * 0.5f, in.h}, 14.0f, u, focus ? ink : Color::rgb(210, 218, 228));
        const f32 s = menu.slider(row);
        if (s >= 0.0f) {
            const Rectf t = menu_row_track(r);
            bar(ui, t, s, Color::rgb(110, 170, 230), u);
            text_in(ui, menu.value(row), {t.x + t.w + 8 * u, in.y, r.x + r.w - (t.x + t.w) - 12 * u, in.h}, 13.0f, u, ink, ui2::UiAlign::End);
        } else {
            const std::string v = menu.value(row);
            const bool choice = !v.empty();
            text_in(ui, choice ? "<  " + v + "  >" : "", {in.x + in.w * 0.5f, in.y, in.w * 0.5f - 4 * u, in.h}, 13.0f, u, ink, ui2::UiAlign::End);
        }
    }
    text_in(ui, "Up/Down select    Left/Right change    Enter accept    Esc resume", l.footer, 10.0f, u, muted);
}

} // namespace summit
