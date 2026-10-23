#include "../../tests/result3d_helpers.hpp"
// Summit's presentation, on the CPU (docs/3d_plan.md, P4b): the HUD model and its layout, the menu, the sound cues and
// curves, the environment curves, the settings file, and the one thing that matters most: none of it changes the
// simulation. No window, no GPU, no sound card (audio mixes into the engine's null backend).

#include "audio.hpp"
#include "camera.hpp"
#include "climber.hpp"
#include "environment.hpp"
#include "game.hpp"
#include "hud.hpp"
#include "input_map.hpp"
#include "level.hpp"
#include "script.hpp"
#include "settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace summit;
namespace fs = std::filesystem;

int failures = 0;
bool verbose = false;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            std::fprintf(stderr, "presentation_tests:%d: CHECK failed: %s\n", __LINE__, #cond);                        \
        }                                                                                                              \
    } while (0)

#define CHECK_NEAR(value, expected, eps)                                                                               \
    do {                                                                                                               \
        const double v_ = static_cast<double>(value), e_ = static_cast<double>(expected);                              \
        if (!(std::fabs(v_ - e_) <= static_cast<double>(eps))) {                                                       \
            ++failures;                                                                                                \
            std::fprintf(stderr, "presentation_tests:%d: %s = %.5f, expected %.5f +- %.5f\n", __LINE__, #value, v_,    \
                         e_, static_cast<double>(eps));                                                                \
        }                                                                                                              \
    } while (0)

using Clock = std::chrono::steady_clock;

struct Outcome {
    u64 hash = 0;
    u64 steps = 0;
    bool summit = false;
};

// Plays a script; `each` is called after every step with the game and the step's command.
Outcome play(u64 seed, const std::string& script, const std::function<void(const Game&)>& each = {},
             u64 max_steps = 40000) {
    Game game(seed);
    BotConfig config;
    script_config(script, game.level(), config);
    Bot bot(game, 0, config);
    Outcome o;
    for (u64 s = 0; s < max_steps && !bot.finished() && !game.player(0).summit; ++s) {
        const PlayerCommand c = bot.next();
        game.step({&c, 1});
        if (each)
            each(game);
        ++o.steps;
    }
    o.hash = game.state_hash();
    o.summit = game.player(0).summit;
    return o;
}

bool inside(const Rectf& r, Vec2f screen) {
    constexpr f32 e = 0.01f;
    return r.x >= -e && r.y >= -e && r.x + r.w <= screen.x + e && r.y + r.h <= screen.y + e && r.w > 0.0f && r.h > 0.0f;
}
bool apart(const Rectf& a, const Rectf& b) {
    constexpr f32 e = 0.01f;
    return a.x + a.w <= b.x + e || b.x + b.w <= a.x + e || a.y + a.h <= b.y + e || b.y + b.h <= a.y + e;
}

// ---- HUD model
// ----------------------------------------------------------------------------------------------------------------

// Compare presentation with a bare simulation built by this same toolchain.
// Transcendental math and hash layout have no cross-release golden contract.
const Outcome& baseline(const std::string& script) {
    static const auto climb = play(1, "climb");
    static const auto fall = play(1, "fall");
    return script == "climb" ? climb : fall;
}

void test_stamina_tone_and_colour() {
    const HudRules rules;
    CHECK(stamina_tone(1.0f, false, rules) == StaminaTone::Good);
    CHECK(stamina_tone(0.5f, false, rules) == StaminaTone::Good);
    CHECK(stamina_tone(0.29f, false, rules) == StaminaTone::Low);
    CHECK(stamina_tone(0.31f, false, rules) == StaminaTone::Good);
    CHECK(stamina_tone(0.5f, true, rules) == StaminaTone::Spent);
    CHECK(stamina_tone(0.0f, false, rules) == StaminaTone::Spent);
    // The colour changes at low: green up high, amber under the threshold, red when nearly gone.
    const Color high = stamina_colour(0.9f, false, rules), low = stamina_colour(0.2f, false, rules),
                gone = stamina_colour(0.05f, false, rules);
    CHECK(high.g > high.r && high.g > high.b);
    CHECK(low.r > low.b && low.g > 100);
    CHECK(gone.r > gone.g + 100);
    CHECK(!(high == low) && !(low == gone));
    // Steady inside a tone: nearly equal at neighbouring values above the blend.
    CHECK(stamina_colour(0.8f, false, rules) == stamina_colour(0.9f, false, rules));
    CHECK(stamina_colour(0.5f, true, rules) == stamina_colour(0.0f, false, rules));
}

void test_fall_warning_thresholds() {
    const PlayerRules r;
    const HudRules h;
    CHECK(fall_warning_for(0.0f, r, h) == FallWarning::None);
    CHECK(fall_warning_for(r.safe_landing * h.brace_fraction - 0.1f, r, h) == FallWarning::None);
    CHECK(fall_warning_for(r.safe_landing * h.brace_fraction, r, h) == FallWarning::Brace);
    CHECK(fall_warning_for(r.safe_landing, r, h) == FallWarning::Brace);
    CHECK(fall_warning_for(r.safe_landing + 0.1f, r, h) == FallWarning::Hurt);
    CHECK(fall_warning_for(r.stun_speed - 0.1f, r, h) == FallWarning::Hurt);
    CHECK(fall_warning_for(r.stun_speed, r, h) == FallWarning::KnockOut);
    // On a real drop: the warning rises through Brace and Hurt to KnockOut as it speeds up, then goes with the landing.
    Game g(1);
    g.debug_teleport(0, g.level().start + Vec3f{0.0f, 12.0f, 0.0f});
    std::vector<FallWarning> seen;
    HudNotices n;
    for (int i = 0; i < 240; ++i) {
        g.step({});
        n.observe(g);
        const HudModel m = build_hud_model(g, 0, n);
        if (seen.empty() || seen.back() != m.fall_warning)
            seen.push_back(m.fall_warning);
    }
    CHECK(seen.size() >= 4);
    bool ordered = true;
    for (std::size_t i = 1; i + 1 < seen.size(); ++i)
        ordered = ordered && seen[i] > seen[i - 1];
    CHECK(ordered);
    CHECK(std::find(seen.begin(), seen.end(), FallWarning::Brace) != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), FallWarning::KnockOut) != seen.end());
    CHECK(seen.back() == FallWarning::None);
}

void test_hud_model_along_the_climb() {
    int low_steps = 0, good_steps = 0, two_hands = 0, one_hand = 0, hanging = 0, at_camp = 0;
    Color first_good{}, first_low{};
    bool have_good = false, have_low = false;
    f32 best = 0.0f;
    bool best_monotone = true;
    bool fractions_ok = true;
    HudNotices notices;
    HudModel last;
    int left_grabs = 0, right_grabs = 0, left_shown = 0, right_shown = 0;
    const Outcome o = play(1, "climb", [&](const Game& g) {
        notices.observe(g);
        const HudModel m = build_hud_model(g, 0, notices);
        last = m;
        // The hand that took hold is the hand the model shows (hand 0 is the left).
        for (const GameEvent& e : g.events()) {
            if (e.kind != GameEventKind::Gripped)
                continue;
            if (e.value < 0.5f) {
                ++left_grabs;
                if (m.left != HandState::Free)
                    ++left_shown;
            } else {
                ++right_grabs;
                if (m.right != HandState::Free)
                    ++right_shown;
            }
        }
        if (m.tone == StaminaTone::Low) {
            ++low_steps;
            if (!have_low)
                first_low = m.stamina_colour, have_low = true;
        }
        if (m.tone == StaminaTone::Good) {
            ++good_steps;
            if (!have_good)
                first_good = m.stamina_colour, have_good = true;
        }
        if (m.hanging && m.hands_held == 2 && m.left == HandState::Held && m.right == HandState::Held)
            ++two_hands;
        if (m.hanging && m.hands_held == 1 && (m.left == HandState::Held) != (m.right == HandState::Held))
            ++one_hand;
        if (m.hanging)
            ++hanging;
        if (m.at_camp)
            ++at_camp;
        if (m.best_altitude + 1e-4f < best)
            best_monotone = false;
        best = std::max(best, m.best_altitude);
        fractions_ok = fractions_ok && m.stamina_fraction >= 0.0f && m.stamina_fraction <= m.cap_fraction + 1e-4f &&
                       m.progress >= 0.0f && m.progress <= 1.0f;
    });
    CHECK(o.summit);
    CHECK(low_steps > 0 && good_steps > 0);
    CHECK(have_good && have_low && !(first_good == first_low));
    CHECK(two_hands > 100);
    CHECK(one_hand > 0);
    CHECK(hanging > two_hands);
    CHECK(at_camp > 0);
    CHECK(best_monotone);
    CHECK(fractions_ok);
    CHECK(left_grabs > 3 && right_grabs > 3);
    CHECK(left_shown == left_grabs && right_shown == right_grabs);
    // The summit: the message, with the run's time and no falls.
    CHECK(last.summit);
    CHECK(last.summit_message.rfind("Summit!", 0) == 0);
    CHECK(last.summit_message.find(clock_text(f32(o.steps) / 60.0f)) != std::string::npos);
    CHECK(last.summit_message.find("0 falls") != std::string::npos);
    CHECK_NEAR(last.run_seconds, double(o.steps) / 60.0, .03);
    CHECK_NEAR(last.altitude, last.summit_altitude, 4.0);
    CHECK(last.progress > 0.95f);
    if (verbose)
        std::printf("  climb: low %d good %d two hands %d one hand %d hanging %d; %s\n", low_steps, good_steps,
                    two_hands, one_hand, hanging, last.summit_message.c_str());
}

void test_one_hand_is_the_hand_that_holds() {
    // Hanging by two hands, then the right one is let go: the model shows the left held and the right free, and the
    // other way round.
    for (const bool keep_left : {true, false}) {
        Game g(1);
        Bot bot(g);
        for (int s = 0; s < 20000 && !(g.traversal(0).mode == TraversalMode3D::Hanging && g.traversal(0).held == 2 &&
                                       g.altitude(0) > 3.0f);
             ++s) {
            const PlayerCommand c = bot.next();
            g.step({&c, 1});
        }
        CHECK(g.traversal(0).held == 2);
        PlayerCommand c;
        c.grab_left = keep_left;
        c.grab_right = !keep_left;
        for (int i = 0; i < 20; ++i)
            g.step({&c, 1});
        const HudModel m = build_hud_model(g, 0, HudNotices{});
        CHECK(m.hands_held == 1);
        CHECK((m.left == HandState::Held) == keep_left);
        CHECK((m.right == HandState::Held) == !keep_left);
    }
}

void test_injury_and_status() {
    Game g(1);
    HudNotices n;
    const auto model = [&] { return build_hud_model(g, 0, n); };
    g.step({});
    n.observe(g);
    HudModel m = model();
    CHECK(m.injury.empty());
    CHECK(m.status == StatusKind::Standing || m.status == StatusKind::InAir);
    g.debug_set_health(0, 60.0f);
    m = model();
    CHECK(m.injury == "Hurt");
    g.debug_set_health(0, 30.0f);
    m = model();
    CHECK(m.injury == "Badly hurt");
    g.debug_set_health(0, 10.0f);
    m = model();
    CHECK(m.injury == "Critical");
    CHECK_NEAR(m.health_fraction, 0.1, 1e-4);
    // The injury lowers the stamina cap, and the model shows it as a share of the bar.
    for (int i = 0; i < 10; ++i)
        g.step({});
    m = model();
    CHECK(m.cap_fraction < 0.9f);
    CHECK(m.cap_fraction >= 0.5f - 1e-3f);
    // Walking and sprinting are told apart by the command and the speed.
    Game w(1);
    for (int i = 0; i < 40; ++i) {
        PlayerCommand c;
        c.move = {0.0f, 1.0f};
        w.step({&c, 1});
    }
    CHECK(build_hud_model(w, 0, HudNotices{}).status == StatusKind::Walking);
    for (int i = 0; i < 60; ++i) {
        PlayerCommand c;
        c.move = {0.0f, 1.0f};
        c.sprint = true;
        w.step({&c, 1});
    }
    CHECK(build_hud_model(w, 0, HudNotices{}).status == StatusKind::Sprinting);
}

void test_notices_come_and_go() {
    // The camp notice: Camp events raise it, it fades out after notice_seconds; hurt flashes and fades.
    HudNotices n;
    Game g(1);
    // A drop of 5 m onto the ground: Hurt events.
    g.debug_teleport(0, g.level().start + Vec3f{0.0f, 5.5f, 0.0f});
    bool hurt_seen = false;
    for (int i = 0; i < 200; ++i) {
        g.step({});
        n.observe(g);
        if (n.hurt > 0.0f)
            hurt_seen = true;
    }
    CHECK(hurt_seen);
    const HudRules rules;
    // ...and it has faded out by now (a second and more later).
    CHECK(n.hurt_seconds <= 0.0f);
    CHECK_NEAR(build_hud_model(g, 0, n, rules).hurt_flash, 0.0, 1e-4);
    // Camp notices along the climb.
    int shown = 0;
    std::string text;
    f32 alpha_max = 0.0f;
    HudNotices cn;
    play(1, "climb", [&](const Game& game) {
        cn.observe(game);
        const HudModel m = build_hud_model(game, 0, cn);
        if (!m.camp_notice.empty()) {
            ++shown;
            text = m.camp_notice;
            alpha_max = std::max(alpha_max, m.camp_notice_alpha);
        }
    });
    CHECK(shown > 100);
    CHECK(text.find("Camp") != std::string::npos);
    CHECK_NEAR(alpha_max, 1.0, 1e-4);
    // After the last camp (and the summit's walk) it has timed out: 6 camps of notice_seconds each at most.
    CHECK(shown <= 7 * int(rules.notice_seconds * 60.0f) + 2);
}

// ---- layout
// -------------------------------------------------------------------------------------------------------------------

void test_layout_fits_at_four_sizes() {
    const Vec2f sizes[] = {{1280, 720}, {1024, 768}, {640, 360}, {320, 180}, {1920, 1080}, {800, 600}, {2560, 1440}};
    for (const Vec2f s : sizes) {
        const HudLayout l = hud_layout(s);
        const Rectf all[] = {l.altitude, l.health, l.stamina, l.hand_left, l.hand_right,
                             l.warning,  l.notice, l.summit,  l.hint,      l.debug};
        for (const Rectf& r : all)
            CHECK(inside(r, s));
        CHECK(inside(l.hands, s));
        for (std::size_t i = 0; i < std::size(all); ++i) {
            for (std::size_t j = i + 1; j < std::size(all); ++j)
                CHECK(apart(all[i], all[j]));
        }
        // The hands sit inside their row and above the bar.
        CHECK(l.hands.y + l.hands.h <= l.stamina.y + 0.01f);
        CHECK(l.hand_left.x >= l.hands.x - 0.01f && l.hand_right.x + l.hand_right.w <= l.hands.x + l.hands.w + 0.01f);
        // The bar is centred, and the layout scales with the window (the bar is a third of the width at 16:9).
        CHECK_NEAR(l.stamina.x + l.stamina.w * 0.5f, s.x * 0.5f, 0.6);
        // The menu: a panel inside the screen, rows inside the panel, none overlapping.
        const MenuLayout m = menu_layout(s, i32(menu_row_count));
        CHECK(inside(m.panel, s));
        CHECK(m.rows.size() == menu_row_count);
        for (std::size_t i = 0; i < m.rows.size(); ++i) {
            CHECK(inside(m.rows[i], s));
            CHECK(m.rows[i].x >= m.panel.x - 0.01f && m.rows[i].x + m.rows[i].w <= m.panel.x + m.panel.w + 0.01f);
            CHECK(m.rows[i].y >= m.panel.y - 0.01f && m.rows[i].y + m.rows[i].h <= m.panel.y + m.panel.h + 0.01f);
            if (i > 0)
                CHECK(apart(m.rows[i], m.rows[i - 1]));
        }
        CHECK(apart(m.title, m.rows.front()));
    }
    // Bigger window, bigger bar; the same share of the width at 16:9.
    const HudLayout a = hud_layout({1280, 720}), b = hud_layout({2560, 1440});
    CHECK_NEAR(b.stamina.w / a.stamina.w, 2.0, 0.01);
    CHECK(hud_layout({1280, 720}, 1.25f).stamina.w > a.stamina.w);
}

// ---- menu and controls
// --------------------------------------------------------------------------------------------------------

RawInput keys(std::initializer_list<Key> ks) {
    RawInput r;
    for (Key k : ks)
        r.set_key(k);
    return r;
}

void test_menu_navigation() {
    SummitSettings s;
    Menu menu(&s);
    CHECK(!menu.open());
    CHECK(menu.toggle() == MenuAction::None);
    CHECK(menu.open() && menu.screen() == MenuScreen::Main && menu.focus() == 0);
    // Down moves the focus; a held key moves it once (edges).
    RawInput none, down = keys({Key::Down});
    MenuEdges e = menu_edges(none, down);
    CHECK(e.input.down && !e.input.up && !e.toggle);
    menu.navigate(e.input);
    CHECK(menu.focus() == 1);
    e = menu_edges(down, down);
    CHECK(!e.input.any());
    // Up wraps from the top row to the last; down wraps back.
    menu.set_focus(0);
    MenuInput up;
    up.up = true;
    menu.navigate(up);
    CHECK(menu.focus() == i32(menu_row_count) - 1);
    MenuInput dn;
    dn.down = true;
    menu.navigate(dn);
    CHECK(menu.focus() == 0);
    // The same moves from a pad: the D-pad and the stick give the same edges as the keys.
    RawInput pad_down;
    pad_down.pad.connected = true;
    pad_down.pad.set_button(GamepadButton::DPadDown, true);
    CHECK(menu_edges(none, pad_down).input == menu_edges(none, down).input);
    RawInput stick;
    stick.pad.connected = true;
    stick.pad.left_y = -0.9f;
    CHECK(menu_edges(none, stick).input.down);
    stick.pad.left_y = -0.3f;
    CHECK(!menu_edges(none, stick).input.any());
    RawInput pad_a;
    pad_a.pad.connected = true;
    pad_a.pad.set_button(GamepadButton::A, true);
    CHECK(menu_edges(none, pad_a).input.accept);
    CHECK(menu_edges(none, keys({Key::Enter})).input.accept);
    RawInput pad_b;
    pad_b.pad.set_button(GamepadButton::B, true);
    CHECK(menu_edges(none, pad_b).input.back);
    RawInput start;
    start.pad.set_button(GamepadButton::Start, true);
    CHECK(menu_edges(none, start).toggle);
    CHECK(menu_edges(none, keys({Key::Escape})).toggle);

    // Rows: Resume closes the menu; Restart asks the owner for it.
    menu.set_focus(i32(MenuRow::Resume));
    MenuInput ok;
    ok.accept = true;
    CHECK(menu.navigate(ok) == MenuAction::Resume);
    CHECK(!menu.open());
    menu.open_main();
    menu.set_focus(i32(MenuRow::Restart));
    CHECK(menu.navigate(ok) == MenuAction::RestartFromCamp);
    menu.open_main();
    // Look cycles through the looks and wraps; the settings follow.
    menu.set_focus(i32(MenuRow::Look));
    const std::string before = s.look;
    CHECK(menu.navigate(ok) == MenuAction::Changed);
    CHECK(s.look != before);
    for (std::size_t i = 1; i < looks().size(); ++i)
        menu.navigate(ok);
    CHECK(s.look == before);
    MenuInput left;
    left.left = true;
    menu.navigate(left);
    CHECK(s.look == looks().back().name);
    // Camera toggles; volumes step by a tenth and clamp.
    menu.set_focus(i32(MenuRow::Camera));
    CHECK(!s.first_person);
    menu.navigate(ok);
    CHECK(s.first_person);
    menu.set_focus(i32(MenuRow::MasterVolume));
    s.master_volume = 0.8f;
    MenuInput right;
    right.right = true;
    menu.navigate(right);
    CHECK_NEAR(s.master_volume, 0.9, 1e-4);
    menu.navigate(right);
    menu.navigate(right);
    CHECK_NEAR(s.master_volume, 1.0, 1e-4);
    for (int i = 0; i < 12; ++i)
        menu.navigate(left);
    CHECK_NEAR(s.master_volume, 0.0, 1e-4);
    CHECK(menu.value(MenuRow::MasterVolume) == "0%");
    // Sfx and ambience have their own rows; invert flips; sensitivity moves inside its range.
    menu.set_focus(i32(MenuRow::SfxVolume));
    menu.navigate(left);
    CHECK_NEAR(s.sfx_volume, 0.9, 1e-4);
    menu.set_focus(i32(MenuRow::AmbienceVolume));
    menu.navigate(left);
    CHECK_NEAR(s.ambience_volume, 0.6, 1e-4);
    menu.set_focus(i32(MenuRow::InvertLook));
    menu.navigate(ok);
    CHECK(s.bindings.invert_y);
    menu.set_focus(i32(MenuRow::Sensitivity));
    const f32 sens = s.bindings.mouse_sensitivity;
    menu.navigate(right);
    CHECK(s.bindings.mouse_sensitivity > sens);
    for (int i = 0; i < 40; ++i)
        menu.navigate(right);
    CHECK(s.bindings.mouse_sensitivity <= 0.30f + 1e-4f);
    // The time of day freezes and runs again.
    menu.set_focus(i32(MenuRow::TimeOfDay));
    s.time_speed = 0.25f;
    menu.navigate(ok);
    CHECK(s.time_speed == 0.0f && menu.value(MenuRow::TimeOfDay) == "Frozen");
    menu.navigate(ok);
    CHECK(s.time_speed > 0.0f);
    // Sliders by a click on the track.
    CHECK(menu.set_slider(MenuRow::MasterVolume, 0.35f));
    CHECK_NEAR(s.master_volume, 0.35, 1e-4);
    CHECK(!menu.set_slider(MenuRow::Quit, 0.5f));
    CHECK(menu.slider(MenuRow::Resume) < 0.0f);
    // Controls screen: accept opens it, back (or the toggle key) returns to the main one, Quit quits.
    menu.set_focus(i32(MenuRow::Controls));
    menu.navigate(ok);
    CHECK(menu.screen() == MenuScreen::Controls);
    MenuInput back;
    back.back = true;
    menu.navigate(back);
    CHECK(menu.screen() == MenuScreen::Main);
    menu.set_focus(i32(MenuRow::Controls));
    menu.navigate(ok);
    CHECK(menu.toggle() == MenuAction::None && menu.screen() == MenuScreen::Main);
    menu.set_focus(i32(MenuRow::Quit));
    CHECK(menu.navigate(ok) == MenuAction::Quit);
    // Back from the main screen resumes; a closed menu does nothing.
    MenuInput b2;
    b2.back = true;
    CHECK(menu.navigate(b2) == MenuAction::Resume);
    CHECK(menu.navigate(ok) == MenuAction::None);
    // Every row has a label.
    for (std::size_t i = 0; i < menu_row_count; ++i)
        CHECK(!menu.label(MenuRow(i)).empty());
}

void test_controls_screen_shows_current_bindings() {
    InputBindings b = InputBindings::defaults();
    std::vector<ControlLine> lines = controls_screen(b);
    CHECK(lines.size() == action_count);
    const auto find = [&](const char* action) -> std::string {
        for (const ControlLine& l : lines) {
            if (l.action == action)
                return l.keys;
        }
        return "";
    };
    CHECK(find("Jump").find("Space") != std::string::npos);
    CHECK(find("Jump").find("Pad A") != std::string::npos);
    CHECK(find("Left hand (hold)").find("Left mouse") != std::string::npos);
    CHECK(find("Right hand (hold)").find("Right trigger") != std::string::npos);
    CHECK(find("Sprint").find("Left Shift") != std::string::npos);
    // A rebinding shows up, an unbound action says so.
    b.set(Action::Jump, {Binding::key(Key::J)});
    b.clear(Action::PullUp);
    lines = controls_screen(b);
    CHECK(find("Jump") == "J");
    CHECK(find("Pull up") == "(not bound)");
    CHECK(hud_hint_line(b).find("Left mouse") != std::string::npos);
}

void test_restart_from_camp() {
    Game g(1);
    Bot bot(g);
    HudNotices n;
    // Up the first wall to the first camp and beyond: on the second pitch, mid-climb, restart.
    for (int s = 0; s < 20000 && !(g.player(0).camp >= 1 && g.traversal(0).mode == TraversalMode3D::Hanging &&
                                   g.altitude(0) > 35.0f);
         ++s) {
        const PlayerCommand c = bot.next();
        g.step({&c, 1});
    }
    CHECK(g.player(0).camp >= 1);
    CHECK(g.traversal(0).mode == TraversalMode3D::Hanging);
    g.debug_set_health(0, 40.0f);
    CHECK(restart_from_camp(g, 0));
    const Camp& camp = g.level().camps[std::size_t(g.player(0).camp)];
    for (int i = 0; i < 90; ++i)
        g.step({});
    const Vec3f d = g.feet(0) - camp.spawn;
    CHECK(std::sqrt(d.x * d.x + d.z * d.z) < 3.0f);
    CHECK(std::fabs(d.y) < 1.0f);
    CHECK(g.player(0).state == PlayerState::Alive);
    CHECK(g.traversal(0).mode == TraversalMode3D::Walking);
    CHECK(g.player(0).health >= 100.0f - 1e-3f);
    CHECK(g.character_state(0).ground == CharacterGround3D::OnGround);
    // A knocked-out player is left to the game.
    Game k(1);
    k.debug_teleport(0, k.level().start + Vec3f{0.0f, 7.0f, 0.0f});
    for (int i = 0; i < 120 && k.player(0).state == PlayerState::Alive; ++i)
        k.step({});
    CHECK(k.player(0).state != PlayerState::Alive);
    CHECK(!restart_from_camp(k, 0));
}

// ---- sound
// --------------------------------------------------------------------------------------------------------------------

void test_audio_curves() {
    // Wind: louder with altitude (any wind), louder with the wind's speed (any altitude).
    for (f32 speed : {1.0f, 3.0f, 6.0f, 11.0f}) {
        f32 prev = -1.0f;
        for (f32 alt = 0.0f; alt <= 160.0f; alt += 10.0f) {
            const f32 g = wind_gain(alt, 150.0f, speed);
            CHECK(g >= prev);
            CHECK(g >= 0.0f && g <= 1.0f);
            prev = g;
        }
        CHECK(wind_gain(150.0f, 150.0f, speed) > wind_gain(0.0f, 150.0f, speed) * 3.0f);
    }
    for (f32 alt : {0.0f, 60.0f, 150.0f}) {
        f32 prev = -1.0f;
        for (f32 s = 0.0f; s <= 12.0f; s += 1.0f) {
            const f32 g = wind_gain(alt, 150.0f, s);
            CHECK(g >= prev);
            prev = g;
        }
    }
    CHECK(wind_pitch(10.0f) > wind_pitch(2.0f));
    // Footsteps: none standing, faster cadence with speed, and a sprint is not a runaway.
    CHECK(footstep_cadence(0.0f) == 0.0f && footstep_cadence(0.4f) == 0.0f);
    f32 prev = 0.0f;
    for (f32 v = 0.6f; v <= 8.0f; v += 0.4f) {
        const f32 c = footstep_cadence(v);
        CHECK(c > prev);
        prev = c;
    }
    const f32 walk = 1.0f / footstep_cadence(4.0f), sprint = 1.0f / footstep_cadence(7.0f);
    CHECK(walk > sprint);
    CHECK(walk > 0.25f && walk < 0.6f);
    CHECK(sprint > 0.2f);
    // Breathing: none while fresh, quicker as it runs out, quickest when spent.
    CHECK(breath_interval(0.9f, false) == 0.0f);
    CHECK(breath_interval(0.3f, false) > breath_interval(0.1f, false));
    CHECK(breath_interval(0.1f, false) > breath_interval(0.0f, false) - 1e-4f);
    CHECK(breath_interval(0.5f, true) > 0.0f && breath_interval(0.5f, true) <= breath_interval(0.1f, false));
    // Falls: nothing under walking speed, rising to full; thumps by damage.
    CHECK(fall_whoosh_gain(2.0f) == 0.0f);
    CHECK(fall_whoosh_gain(10.0f) > fall_whoosh_gain(5.0f));
    CHECK(fall_whoosh_gain(30.0f) >= fall_whoosh_gain(20.0f));
    CHECK(fall_whoosh_pitch(20.0f) > fall_whoosh_pitch(5.0f));
    CHECK(impact_gain(35.0f) > impact_gain(7.0f));
    CHECK(impact_gain(500.0f) <= 1.0f + 1e-4f);
}

struct Counts {
    int c[cue_kind_count] = {};
    int operator[](CueKind k) const { return c[std::size_t(k)]; }
};

Counts count_cues(const std::vector<SoundCue>& cues) {
    Counts n;
    for (const SoundCue& c : cues)
        n.c[std::size_t(c.kind)]++;
    return n;
}

void test_cues_of_the_scripted_climb() {
    std::vector<SoundCue> cues;
    CueGenerator gen;
    int gripped = 0, released_heard = 0, camp_events = 0, summit_events = 0, landed = 0;
    const Outcome o = play(1, "climb", [&](const Game& g) {
        gen.observe(g, 0, cues);
        for (const GameEvent& e : g.events()) {
            if (e.kind == GameEventKind::Gripped)
                ++gripped;
            if (e.kind == GameEventKind::Camp)
                ++camp_events;
            if (e.kind == GameEventKind::Summit)
                ++summit_events;
            if (e.kind == GameEventKind::Landed && e.value >= 2.0f)
                ++landed;
            if (e.kind == GameEventKind::Released) {
                const GripRelease3D why = GripRelease3D(i32(e.value));
                if (why != GripRelease3D::Mantle && why != GripRelease3D::Lost)
                    ++released_heard;
            }
        }
    });
    CHECK(o.summit && o.steps == baseline("climb").steps && o.hash == baseline("climb").hash);
    const Counts n = count_cues(cues);
    if (verbose) {
        for (std::size_t k = 0; k < cue_kind_count; ++k)
            std::printf("  cue %-10s %d\n", cue_name(CueKind(k)), n.c[k]);
    }
    // One grab cue for each grip event, a cue for the camps and the summit, footsteps on the walks, no fall sounds.
    CHECK(n[CueKind::Grab] == gripped);
    CHECK(n[CueKind::Grab] >= 10);
    CHECK(n[CueKind::Release] == released_heard);
    CHECK(n[CueKind::Camp] == camp_events && camp_events >= 5);
    CHECK(n[CueKind::Summit] == 1 && summit_events == 1);
    CHECK(n[CueKind::Footstep] > 150);
    CHECK(n[CueKind::FallStart] == 0 && n[CueKind::Impact] == 0 && n[CueKind::Hurt] == 0 && n[CueKind::Died] == 0);
    CHECK(n[CueKind::Breath] > 0);
    CHECK(n[CueKind::RopeCreak] >= 3);
    CHECK(n[CueKind::Land] == landed);
    // Each cue is sane: finite, gain in range, steps rising, variants valid.
    u64 last = 0;
    bool sane = true, ordered = true;
    for (const SoundCue& c : cues) {
        sane = sane && std::isfinite(c.position.x) && std::isfinite(c.position.y) && std::isfinite(c.position.z) &&
               c.gain > 0.0f && c.gain <= 1.5f && c.pitch > 0.5f && c.pitch < 2.0f;
        ordered = ordered && c.step >= last;
        last = c.step;
        if (c.kind == CueKind::Footstep)
            sane = sane && c.variant >= 0 && c.variant <= 2;
    }
    CHECK(sane && ordered);
    // Footsteps match the ground: the first walk is on grass, and a rock face is never stepped on (nothing is "grass"
    // up a wall).
    bool grass = false;
    for (const SoundCue& c : cues)
        grass = grass || (c.kind == CueKind::Footstep && c.variant == int(SurfaceSound::Grass));
    CHECK(grass);
    // Deterministic: the same run gives the same list.
    std::vector<SoundCue> again;
    CueGenerator gen2;
    play(1, "climb", [&](const Game& g) { gen2.observe(g, 0, again); });
    CHECK(again == cues);
}

void test_cadence_follows_speed_in_the_run() {
    // Walking and sprinting on the flat: the gap between footsteps shrinks with speed, measured from the cues.
    const auto gap = [](bool sprint) {
        Game g(1);
        CueGenerator gen;
        std::vector<SoundCue> cues;
        for (int i = 0; i < 400; ++i) {
            PlayerCommand c;
            c.move = {0.0f, 1.0f};
            c.sprint = sprint;
            g.step({&c, 1});
            gen.observe(g, 0, cues);
        }
        std::vector<u64> steps;
        for (const SoundCue& c : cues) {
            if (c.kind == CueKind::Footstep && c.step > 150)
                steps.push_back(c.step);
        }
        if (steps.size() < 3)
            return 0.0;
        return double(steps.back() - steps.front()) / double(steps.size() - 1) / 60.0;
    };
    const double walk = gap(false), sprint = gap(true);
    if (verbose)
        std::printf("  footstep gap: walk %.3f s, sprint %.3f s\n", walk, sprint);
    CHECK(walk > 0.0 && sprint > 0.0);
    CHECK(walk > sprint);
}

void test_cues_of_the_scripted_fall() {
    std::vector<SoundCue> cues;
    CueGenerator gen;
    f32 damage = 0.0f;
    u64 fell_step = 0, hurt_step = 0;
    const Outcome o = play(1, "fall", [&](const Game& g) {
        gen.observe(g, 0, cues);
        for (const GameEvent& e : g.events()) {
            if (e.kind == GameEventKind::Hurt)
                damage += e.value, hurt_step = e.step;
            if (e.kind == GameEventKind::Fell && fell_step == 0)
                fell_step = e.step;
        }
    });
    CHECK(o.summit && o.steps == baseline("fall").steps && o.hash == baseline("fall").hash);
    const Counts n = count_cues(cues);
    CHECK(n[CueKind::FallStart] >= 1);
    CHECK(n[CueKind::Impact] >= 1);
    CHECK(n[CueKind::Hurt] == n[CueKind::Impact]);
    CHECK(n[CueKind::GotUp] >= 1);
    CHECK(damage > 10.0f);
    CHECK(fell_step > 0 && hurt_step > fell_step);
    // The fall comes before its impact, and a hard landing has no soft landing sound over it.
    u64 fall_cue = 0, impact_cue = 0;
    for (const SoundCue& c : cues) {
        if (c.kind == CueKind::FallStart && fall_cue == 0)
            fall_cue = c.step;
        if (c.kind == CueKind::Impact && impact_cue == 0)
            impact_cue = c.step;
    }
    CHECK(fall_cue > 0 && impact_cue > fall_cue);
    for (const SoundCue& c : cues) {
        if (c.kind == CueKind::Land)
            CHECK(c.step != impact_cue);
    }
    // The impact's loudness follows the damage.
    for (const SoundCue& c : cues) {
        if (c.kind == CueKind::Impact) {
            CHECK_NEAR(c.gain, impact_gain(damage), 1e-3);
            break;
        }
    }
    // Running out of stamina has its own gasp cue only when the bar empties: a death makes a Died cue.
    Game g(1);
    g.debug_teleport(0, g.level().start + Vec3f{0.0f, 45.0f, 0.0f});
    CueGenerator gen3;
    std::vector<SoundCue> dc;
    for (int i = 0; i < 200; ++i) {
        g.step({});
        gen3.observe(g, 0, dc);
    }
    CHECK(count_cues(dc)[CueKind::Died] >= 1);
}

void test_audio_engine_plays_the_cues() {
    SummitAudio audio(true);
    CHECK(!audio.device_available());
    Game g(1);
    g.step({});
    AudioListener3D listener;
    listener.position = g.head(0);
    audio.update(g, 0, listener, fixed_step);
    const int base = audio.stats().played_requests;
    const int culled0 = audio.stats().culled_requests;
    CHECK(base == 3); // wind, whoosh, and the fire of the camp in earshot (the others are too far to start)
    // Every kind can be played.
    for (std::size_t k = 0; k < cue_kind_count; ++k) {
        SoundCue c;
        c.kind = CueKind(k);
        c.position = g.feet(0) + Vec3f{1.0f, 0.0f, 0.0f};
        audio.play(c);
    }
    // Each was asked for: played, or turned away by its own cap (a breath cue is one voice at a time).
    CHECK((audio.stats().played_requests - base) + (audio.stats().culled_requests - culled0) == int(cue_kind_count));
    CHECK(audio.stats().played_requests - base >= int(cue_kind_count) - 2);
    CHECK(audio.played_total() == int(cue_kind_count));
    CHECK(audio.played(CueKind::Grab) == 1);
    audio.update(g, 0, listener, fixed_step);
    std::vector<f32> out(2 * 4800);
    audio.engine().render(out);
    f32 peak = 0.0f;
    for (f32 v : out)
        peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.01f);
    CHECK(peak <= 1.0f + 1e-4f);
    // Master volume 0: silence; sfx volume 0: the effects go, ambience stays.
    audio.set_volumes({0.0f, 1.0f, 1.0f});
    SoundCue c;
    c.kind = CueKind::Impact;
    audio.play(c);
    audio.engine().render(out);
    audio.engine().render(out);
    peak = 0.0f;
    for (f32 v : out)
        peak = std::max(peak, std::fabs(v));
    CHECK(peak < 1e-4f);
    audio.set_volumes({1.0f, 1.0f, 1.0f});
    CHECK_NEAR(audio.engine().bus_volume("master"), 1.0, 1e-4);
    audio.set_volumes({0.5f, 0.25f, 0.75f});
    CHECK_NEAR(audio.engine().bus_volume("sfx"), 0.25, 1e-4);
    CHECK_NEAR(audio.engine().bus_volume("ambient"), 0.75, 1e-4);
    // The wind loop follows the curve: higher up and gustier is louder.
    Game hi(1);
    hi.debug_teleport(0, hi.level().summit);
    hi.step({});
    audio.update(hi, 0, listener, fixed_step);
    const f32 up = audio.wind_level();
    audio.update(g, 0, listener, fixed_step);
    CHECK(up > audio.wind_level());
    // A fall opens the whoosh.
    Game fall(1);
    fall.debug_teleport(0, fall.level().start + Vec3f{0.0f, 40.0f, 0.0f});
    for (int i = 0; i < 60; ++i)
        fall.step({});
    audio.update(fall, 0, listener, fixed_step);
    CHECK(audio.whoosh_level() > 0.2f);
}

// ---- environment
// --------------------------------------------------------------------------------------------------------------

void test_environment_curves() {
    EnvironmentInput in;
    in.look = find_look("afternoon");
    in.height = 150.0f;
    f32 prev_fog = 1e9f, prev_snow = -1.0f, prev_turb = 1e9f, prev_sun = 0.0f;
    for (f32 alt = 0.0f; alt <= 150.0f; alt += 5.0f) {
        in.altitude = alt;
        const EnvironmentState e = evaluate_environment(in);
        CHECK(e.fog_density <= prev_fog + 1e-9f);
        CHECK(e.snow >= prev_snow - 1e-6f);
        CHECK(e.turbidity <= prev_turb + 1e-6f);
        CHECK(e.sun_intensity >= prev_sun);
        prev_fog = e.fog_density;
        prev_snow = e.snow;
        prev_turb = e.turbidity;
        prev_sun = e.sun_intensity;
        if (alt < 0.5f * 150.0f)
            CHECK(e.snow == 0.0f);
        CHECK(e.fog_tint.z >= e.fog_tint.x - 1e-6f);
    }
    in.altitude = 0.0f;
    const EnvironmentState base = evaluate_environment(in);
    in.altitude = 150.0f;
    const EnvironmentState top = evaluate_environment(in);
    CHECK_NEAR(base.fog_density, find_look("afternoon")->fog_density, 1e-9);
    CHECK(top.fog_density < base.fog_density * 0.6f);
    CHECK(top.snow > 0.3f);
    CHECK(top.snow_share > 0.99f);
    CHECK(base.snow_line > 70.0f && base.snow_line < 100.0f);
    // The snow line is where the fraction says, and a flat top holds more than a wall.
    CHECK(snow_cover(10.0f, 150.0f, 1.0f) == 0.0f);
    CHECK(snow_cover(140.0f, 150.0f, 1.0f) > 0.9f);
    CHECK(snow_cover(140.0f, 150.0f, 0.0f) < 0.3f);
    CHECK(snow_cover(140.0f, 150.0f, -0.5f) == 0.0f);
    CHECK(snow_cover(140.0f, 150.0f, 1.0f) > snow_cover(140.0f, 150.0f, 0.5f));
    // Every effect off: the look as it was, at any altitude.
    in.effects = {};
    in.effects.altitude_look = in.effects.snow_line = in.effects.wind_particles = in.effects.time_of_day = false;
    for (f32 alt : {0.0f, 75.0f, 150.0f}) {
        in.altitude = alt;
        in.game_seconds = 500.0;
        const EnvironmentState e = evaluate_environment(in);
        CHECK(e.fog_density == in.look->fog_density);
        CHECK(e.turbidity == in.look->turbidity);
        CHECK(e.snow == in.look->snow && e.dust == 0.0f);
        CHECK(e.hours == in.look->hours);
        CHECK(e.fog_tint.x == 1.0f && e.fog_tint.y == 1.0f && e.fog_tint.z == 1.0f);
    }
    // Storm: denser and wetter below the snow line, snow above it.
    in.effects = {};
    in.effects.storm = true;
    in.altitude = 10.0f;
    const EnvironmentState low_storm = evaluate_environment(in);
    in.altitude = 140.0f;
    const EnvironmentState high_storm = evaluate_environment(in);
    in.effects.storm = false;
    in.altitude = 10.0f;
    CHECK(low_storm.fog_density > evaluate_environment(in).fog_density * 2.0f);
    CHECK(low_storm.rain > 0.5f && high_storm.rain < low_storm.rain);
    CHECK(high_storm.snow > 0.8f);
    // Dust follows the wind.
    in.wind_speed = 1.0f;
    in.altitude = 5.0f;
    const f32 calm = evaluate_environment(in).dust;
    in.wind_speed = 11.0f;
    CHECK(evaluate_environment(in).dust > calm);
}

void test_time_of_day_clock() {
    CHECK_NEAR(clock_hours(16.2f, 0.0, 0.25f), 16.2, 1e-4);
    CHECK_NEAR(clock_hours(16.2f, 600.0, 0.25f), 16.2 + 2.5, 1e-3);
    // A frozen clock does not move, and the day wraps.
    CHECK_NEAR(clock_hours(16.2f, 6000.0, 0.0f), 16.2, 1e-4);
    CHECK_NEAR(clock_hours(23.0f, 60.0, 1.0f), 0.0, 1e-3);
    CHECK_NEAR(clock_hours(23.0f, 150.0, 1.0f), 1.5, 1e-3);
    // Slowly: a run of 10 minutes at the default speed moves the sun by about 2.5 hours.
    EnvironmentInput in;
    in.look = find_look("afternoon");
    in.game_seconds = 0.0;
    const f32 h0 = evaluate_environment(in).hours;
    in.game_seconds = 600.0;
    const f32 h1 = evaluate_environment(in).hours;
    CHECK_NEAR(h1 - h0, 2.5, 0.01);
    in.time_speed = 0.0f;
    CHECK(evaluate_environment(in).hours == h0);
    in.time_speed = 0.25f;
    in.effects.time_of_day = false;
    CHECK(evaluate_environment(in).hours == h0);
    // A fire is a light at night and a glow by day.
    CHECK(fire_light_strength(0.0f) > 0.99f);
    CHECK(fire_light_strength(1.0f) < 0.15f);
    CHECK(fire_light_strength(0.3f) > fire_light_strength(0.6f));
}

// ---- settings
// -----------------------------------------------------------------------------------------------------------------

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("kin_summit_settings_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void test_settings_round_trip_and_fallback() {
    TempDir dir;
    SaveStore store = make_settings_store(dir.path);
    bool found = true;
    // No file: defaults.
    SummitSettings s = load_settings(store, &found);
    CHECK(!found);
    CHECK(s == SummitSettings{});
    // A round trip of everything.
    SummitSettings w;
    w.look = "dusk";
    w.first_person = true;
    w.hud = false;
    w.master_volume = 0.35f;
    w.sfx_volume = 0.5f;
    w.ambience_volume = 0.25f;
    w.time_speed = 0.0f;
    w.effects.snow_line = false;
    w.effects.storm = true;
    w.effects.camp_lights = false;
    w.bindings.set(Action::Jump, {Binding::key(Key::J), Binding::pad(GamepadButton::X)});
    w.bindings.mouse_sensitivity = 0.07f;
    w.bindings.stick_look_speed = 210.0f;
    w.bindings.invert_y = true;
    CHECK(save_settings(store, w));
    CHECK(fs::exists(store.settings_path()));
    const SummitSettings r = load_settings(store, &found);
    CHECK(found);
    CHECK(r.look == "dusk" && r.first_person && !r.hud);
    CHECK_NEAR(r.master_volume, 0.35, 1e-6);
    CHECK_NEAR(r.sfx_volume, 0.5, 1e-6);
    CHECK_NEAR(r.ambience_volume, 0.25, 1e-6);
    CHECK(r.time_speed == 0.0f);
    CHECK(!r.effects.snow_line && r.effects.storm && !r.effects.camp_lights && r.effects.fires);
    CHECK(r.bindings.bindings(Action::Jump).size() == 2);
    CHECK(r.bindings.bindings(Action::Jump)[0] == Binding::key(Key::J));
    CHECK_NEAR(r.bindings.mouse_sensitivity, 0.07, 1e-3);
    CHECK_NEAR(r.bindings.stick_look_speed, 210.0, 0.01);
    CHECK(r.bindings.invert_y);
    CHECK(r.bindings.bindings(Action::Forward) == InputBindings::defaults().bindings(Action::Forward));
    // The same file saved twice is the same bytes (a stable format).
    const auto read_all = [&] {
        std::ifstream in(store.settings_path(), std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    };
    CHECK(save_settings(store, w));
    const std::string text1 = read_all();
    CHECK(save_settings(store, w));
    CHECK(read_all().size() == text1.size());

    const auto write_raw = [&](const std::string& text) {
        std::ofstream out(store.settings_path(), std::ios::binary | std::ios::trunc);
        out << text;
    };
    // A file that is not JSON, one that is cut short, an empty one and another game's: the defaults, no crash.
    for (const std::string& bad :
         {std::string("this is not json at all {{{"), text1.substr(0, text1.size() / 2), std::string(),
          std::string("\0\1\2\3", 4),
          std::string("{\"schema\":\"kin.settings.v1\",\"game_id\":\"other\",\"payload\":{}}")}) {
        write_raw(bad);
        found = true;
        const SummitSettings d = load_settings(store, &found);
        CHECK(!found);
        CHECK(d == SummitSettings{});
    }
    // Damaged fields fall back one by one: a wrong type, a number out of range, an unknown look, a bad bindings table.
    {
        JsonValue::Object payload;
        payload["look"] = JsonValue(std::string("no such look"));
        payload["master_volume"] = JsonValue(7.0);
        payload["sfx_volume"] = JsonValue(std::string("loud"));
        payload["ambience_volume"] = JsonValue(-3.0);
        payload["time_speed"] = JsonValue(1e9);
        payload["first_person"] = JsonValue(std::string("yes"));
        payload["bindings"] = JsonValue(std::string("jump = key:Banana\nforward = ???"));
        SummitSettings d;
        CHECK(read_settings(JsonValue(payload), d));
        CHECK(d.look == "afternoon");
        CHECK_NEAR(d.master_volume, 1.0, 1e-6);
        CHECK_NEAR(d.sfx_volume, SummitSettings{}.sfx_volume, 1e-6);
        CHECK_NEAR(d.ambience_volume, 0.0, 1e-6);
        CHECK(d.time_speed <= 12.0f);
        CHECK(!d.first_person);
        CHECK(d.bindings == InputBindings::defaults());
        CHECK(!read_settings(JsonValue(std::string("nope")), d));
    }
    // Part of the settings in an otherwise good file: what is there is used, the rest is the default.
    {
        JsonValue::Object payload;
        payload["first_person"] = JsonValue(true);
        SummitSettings d;
        CHECK(read_settings(JsonValue(payload), d));
        CHECK(d.first_person && d.look == "afternoon" && d.bindings == InputBindings::defaults());
    }
    // A regular file cannot be a settings directory on any platform, regardless of write permissions.
    const fs::path blocked = dir.path / "not_a_directory";
    {
        std::ofstream out{blocked};
        out << "blocks directory creation";
        CHECK(out.good());
    }
    SaveStore nowhere = make_settings_store(blocked);
    CHECK(!save_settings(nowhere, w));
    CHECK(load_settings(nowhere) == SummitSettings{});
}

// ---- presentation does not change the simulation
// ------------------------------------------------------------------------------

void test_presentation_does_not_change_the_simulation() {
    // The climb and the fall with everything on: the HUD model, the notices, the cue list, the audio engine's updates,
    // the environment curves, the camera and the menu are all fed every step. The hash and steps are the stored ones.
    const auto with_everything = [](const char* script, u64 seed) {
        Game game(seed);
        BotConfig config;
        script_config(script, game.level(), config);
        Bot bot(game, 0, config);
        SummitAudio audio(true);
        CueGenerator cues;
        HudNotices notices;
        CameraRig rig;
        SummitSettings settings;
        Menu menu(&settings);
        std::vector<SoundCue> list;
        f64 sink = 0.0;
        AudioListener3D listener;
        u64 steps = 0;
        for (; steps < 40000 && !bot.finished() && !game.player(0).summit; ++steps) {
            const PlayerCommand c = bot.next();
            game.step({&c, 1});
            notices.observe(game);
            const HudModel m = build_hud_model(game, 0, notices);
            sink += double(m.stamina_fraction) + double(m.altitude);
            list.clear();
            cues.observe(game, 0, list);
            for (const SoundCue& q : list)
                audio.play(q);
            rig.observe(game, 0);
            const CameraView v = rig.frame(game.world(), 1.0f, fixed_step);
            listener.position = v.position;
            listener.forward = v.rotation.rotate({0.0f, 0.0f, -1.0f});
            if (steps % 8 == 0)
                audio.update(game, 0, listener, 8 * fixed_step, menu.open());
            EnvironmentInput in;
            in.altitude = game.altitude(0);
            in.height = game.level().height();
            in.game_seconds = game.time();
            in.wind_speed = game.wind().speed;
            sink += double(evaluate_environment(in).fog_density);
            if (steps % 500 == 0)
                menu.toggle();
        }
        (void)sink;
        return std::pair<u64, u64>{game.state_hash(), steps};
    };
    const auto climb = with_everything("climb", 1);
    CHECK(climb.first == baseline("climb").hash && climb.second == baseline("climb").steps);
    const auto fall = with_everything("fall", 1);
    CHECK(fall.first == baseline("fall").hash && fall.second == baseline("fall").steps);
    // Another seed: the same as a bare run of it.
    const Outcome bare = play(2, "climb");
    const auto s2 = with_everything("climb", 2);
    CHECK(s2.first == bare.hash && s2.second == bare.steps);
    CHECK(bare.summit && bare.steps > 9000 && bare.steps < 12000);
}

void test_camera_frames_a_lying_body() {
    // Knocked out on open ground: the camera comes up and back and looks down, with the whole body in the picture.
    Game g(1);
    CameraRig rig;
    g.debug_teleport(0, g.level().start + Vec3f{0.0f, 7.0f, 0.0f});
    bool down = false;
    CameraView v;
    for (int i = 0; i < 200; ++i) {
        g.step({});
        rig.observe(g, 0);
        v = rig.frame(g.world(), 1.0f, fixed_step);
        if (g.player(0).state == PlayerState::Stunned)
            down = true;
    }
    CHECK(down);
    const Vec3f pelvis = g.head(0);
    const Vec3f to_body = pelvis - v.position;
    const Vec3f forward = v.rotation.rotate({0.0f, 0.0f, -1.0f});
    const f32 along = dot(normalize(to_body), forward);
    CHECK(length(to_body) > 3.0f);                // far enough back to show a body
    CHECK(v.position.y > pelvis.y + 1.0f);        // above it, looking down
    CHECK(along > std::cos(30.0f * pi / 180.0f)); // the body is near the middle of the picture
    CHECK(forward.y < -0.15f);
    // Standing again: the ordinary arm.
    Game w(1);
    CameraRig standing;
    for (int i = 0; i < 60; ++i) {
        w.step({});
        standing.observe(w, 0);
        v = standing.frame(w.world(), 1.0f, fixed_step);
    }
    CHECK(v.rotation.rotate({0.0f, 0.0f, -1.0f}).y > -0.15f);
}

// ---- the look pass
// ----------------------------------------------------------------------------------------------------------

f32 luminance(Color c) {
    return 0.2126f * f32(c.r) + 0.7152f * f32(c.g) + 0.0722f * f32(c.b);
}

f32 distance_to_route(const SummitLevel& level, Vec3f p) {
    f32 best = 1e9f;
    for (const Leg& l : level.route) {
        const Vec3f ab = l.to - l.from;
        const f32 t = std::clamp(dot(p - l.from, ab) / std::max(1e-6f, dot(ab, ab)), 0.0f, 1.0f);
        best = std::min(best, length(p - (l.from + ab * t)));
    }
    return best;
}

void test_look_pass_meshes() {
    for (u64 seed : {1ull, 2ull, 7ull}) {
        const SummitLevel level = generate_level(seed);
        const std::vector<RenderMeshData> rock = build_render_meshes(level);
        const std::vector<RenderMeshData> decor = build_decor_meshes(level);
        const std::vector<RenderMeshData> terrain = build_terrain_meshes(level);
        CHECK(!rock.empty() && !decor.empty() && terrain.size() == 1);
        u64 triangles = 0;
        bool finite = true, valid = true, uv_ok = true;
        for (const auto* set : {&rock, &decor, &terrain}) {
            for (const RenderMeshData& m : *set) {
                std::string why;
                valid = valid && kin::test::valid3d(m.mesh.validate(), why);
                triangles += m.mesh.triangle_count();
                for (const MeshVertex3D& v : m.mesh.vertices) {
                    finite = finite && std::isfinite(v.position.x) && std::isfinite(v.position.y) &&
                             std::isfinite(v.position.z) && std::isfinite(v.normal.x) && std::isfinite(v.normal.y) &&
                             std::isfinite(v.normal.z) && std::isfinite(v.uv.x) && std::isfinite(v.uv.y);
                    uv_ok = uv_ok && std::fabs(v.uv.x) < 1e5f && std::fabs(v.uv.y) < 1e5f;
                }
            }
        }
        CHECK(finite && valid && uv_ok);
        // A budget: rock and decor under a third of a million triangles, the terrain under 40 thousand.
        CHECK(triangles > 100000 && triangles < 450000);
        if (verbose)
            std::printf("  seed %llu: %llu triangles\n", (unsigned long long)seed, (unsigned long long)triangles);
        // The same level, the same bytes.
        const std::vector<RenderMeshData> again = build_decor_meshes(level);
        bool same = again.size() == decor.size();
        for (std::size_t i = 0; same && i < decor.size(); ++i) {
            same = decor[i].mesh.vertices.size() == again[i].mesh.vertices.size() &&
                   std::memcmp(decor[i].mesh.vertices.data(), again[i].mesh.vertices.data(),
                               decor[i].mesh.vertices.size() * sizeof(MeshVertex3D)) == 0;
        }
        CHECK(same);
        // Decor stays off the route: nothing within a metre of a leg's line (a step's width, or a hold's reach), and
        // none of it inside the camps' spawn places.
        f32 nearest = 1e9f;
        for (const RenderMeshData& m : decor) {
            for (const MeshVertex3D& v : m.mesh.vertices)
                nearest = std::min(nearest, distance_to_route(level, v.position));
        }
        CHECK(nearest > 1.0f);
        // The ground is below base camp's field near it, and the ranges rise a long way over it.
        f32 near_top = -1e9f, far_top = -1e9f;
        const Vec3f c = (level.bounds.min + level.bounds.max) * 0.5f;
        for (const MeshVertex3D& v : terrain[0].mesh.vertices) {
            const f32 d =
                std::sqrt((v.position.x - c.x) * (v.position.x - c.x) + (v.position.z - c.z) * (v.position.z - c.z));
            if (d < 30.0f)
                near_top = std::max(near_top, v.position.y);
            far_top = std::max(far_top, v.position.y);
        }
        CHECK(near_top < level.start.y - 0.4f);
        CHECK(far_top > level.start.y + 150.0f);
        // A cliff's face is where its collider is: some vertex of the mesh lies on the face plane near its middle (the
        // climbing is on the collider, so the picture may not float off it).
        int faces = 0;
        for (const Piece& p : level.pieces) {
            if (p.kind != PieceKind::Cliff || p.half.x < 10.0f)
                continue;
            const Vec3f normal = p.rotation.rotate({0.0f, 0.0f, 1.0f});
            const Vec3f centre = p.centre + normal * p.half.z;
            f32 best = 1e9f;
            for (const MeshVertex3D& v : rock[0].mesh.vertices) {
                if (length(v.position - centre) < 1.2f)
                    best = std::min(best, std::fabs(dot(v.position - centre, normal)));
            }
            if (best < 1e8f) {
                ++faces;
                CHECK(best < 0.06f);
            }
        }
        CHECK(faces >= 3);
        // Snow: the rock's up-facing vertices are paler above the snow line than far below it.
        f64 low = 0.0, high = 0.0;
        int nl = 0, nh = 0;
        for (const RenderMeshData& m : rock) {
            for (const MeshVertex3D& v : m.mesh.vertices) {
                if (v.normal.y < 0.9f)
                    continue;
                const f32 alt = v.position.y - level.start.y;
                if (alt < 0.2f * level.height())
                    low += luminance(v.color), ++nl;
                if (alt > 0.85f * level.height())
                    high += luminance(v.color), ++nh;
            }
        }
        CHECK(nl > 50 && nh > 50);
        CHECK(high / std::max(1, nh) > low / std::max(1, nl) + 25.0);
    }
}

// ---- winding: every triangle faces the way its normals point
// ----------------------------------------------------------------
//
// A mesh wound the wrong way (the climber's tubes once were) shows its inside: culled from outside, lit from the wrong
// side. The engine's convention is anticlockwise seen from the front, so (v1 - v0) x (v2 - v0) agrees with the vertex
// normals. No test looked at that, so this walks every CPU-built mesh Summit draws and counts the triangles that
// disagree.

// Triangles whose geometric normal opposes the average vertex normal, of those that have an area and a normal.
struct Winding {
    u64 triangles = 0, wrong = 0, down = 0; // `down`: geometric normal not upwards (a height field's must be)
};

Winding check_winding(const MeshData3D& mesh) {
    Winding w;
    const std::size_t n = mesh.indices.empty() ? mesh.vertices.size() : mesh.indices.size();
    for (std::size_t i = 0; i + 3 <= n; i += 3) {
        const std::size_t ia = mesh.indices.empty() ? i : mesh.indices[i];
        const std::size_t ib = mesh.indices.empty() ? i + 1 : mesh.indices[i + 1];
        const std::size_t ic = mesh.indices.empty() ? i + 2 : mesh.indices[i + 2];
        const MeshVertex3D &a = mesh.vertices[ia], &b = mesh.vertices[ib], &c = mesh.vertices[ic];
        const Vec3f g = cross(b.position - a.position, c.position - a.position);
        const Vec3f vn = a.normal + b.normal + c.normal;
        if (dot(g, g) < 1e-12f || dot(vn, vn) < 1e-6f)
            continue; // no area, or normals that cancel: nothing to compare
        ++w.triangles;
        if (dot(g, vn) <= 0.0f)
            ++w.wrong;
        if (g.y <= 0.0f)
            ++w.down;
    }
    return w;
}

// Reports one mesh; a double-sided material draws both faces, so either winding is right.
// `height_field`: the ground, whose far triangles are hundreds of metres wide and whose vertex normals are the point
// slope, so a sliver of steep ones disagree with their face by more than 90 degrees; its winding is "up" instead (and
// under 1% disagree).
void expect_wound(const char* set, const std::string& name, const MeshData3D& mesh, bool double_sided = false,
                  bool height_field = false) {
    if (double_sided)
        return;
    Winding w = check_winding(mesh);
    if (height_field) {
        if (w.wrong * 100 < w.triangles)
            w.wrong = 0;
        w.wrong += w.down;
    }
    if (verbose)
        std::printf("  %s %s: %llu triangles, %llu wrong\n", set, name.c_str(), (unsigned long long)w.triangles,
                    (unsigned long long)w.wrong);
    if (w.wrong != 0) {
        ++failures;
        std::fprintf(
            stderr,
            "presentation_tests: winding: %s '%s': %llu of %llu triangles (%.1f%%) face against their normals\n", set,
            name.c_str(), (unsigned long long)w.wrong, (unsigned long long)w.triangles,
            100.0 * double(w.wrong) / double(std::max<u64>(1, w.triangles)));
    }
}

// Edges used by exactly one triangle, after welding vertices that sit within 1e-5 of each other (the seam, the poles,
// the caps' copies of the end rings). Triangles that collapse to a line (a pole's) are ignored. Zero means the surface
// is closed.
u64 boundary_edges(const MeshData3D& mesh) {
    const std::size_t n = mesh.vertices.size();
    std::vector<u32> weld(n);
    for (std::size_t i = 0; i < n; ++i) {
        weld[i] = u32(i);
        for (std::size_t j = 0; j < i; ++j) {
            const Vec3f d = mesh.vertices[i].position - mesh.vertices[j].position;
            if (std::fabs(d.x) < 1e-5f && std::fabs(d.y) < 1e-5f && std::fabs(d.z) < 1e-5f) {
                weld[i] = weld[j];
                break;
            }
        }
    }
    std::map<std::pair<u32, u32>, int> uses;
    for (std::size_t i = 0; i + 3 <= mesh.indices.size(); i += 3) {
        const u32 t[3] = {weld[mesh.indices[i]], weld[mesh.indices[i + 1]], weld[mesh.indices[i + 2]]};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
            continue;
        for (int e = 0; e < 3; ++e) {
            const u32 a = t[e], b = t[(e + 1) % 3];
            ++uses[{std::min(a, b), std::max(a, b)}];
        }
    }
    u64 open = 0;
    for (const auto& kv : uses)
        open += kv.second == 1 ? 1 : 0;
    return open;
}

void test_winding() {
    // The person: every part of all four styles.
    for (i32 style = 0; style < 4; ++style) {
        const std::vector<ClimberPart> parts = build_climber_meshes(style_for_player(style));
        CHECK(parts.size() >= 7);
        for (const ClimberPart& p : parts) {
            const std::string label = "style " + std::to_string(style) + " " + p.name;
            CHECK(p.mesh.skinned());
            expect_wound("climber", label, p.mesh);
            // Every tube is capped and the beanie's dome is closed, so no part has an edge that only one triangle uses:
            // nothing is open by design (looking through an open end of a limb was the report).
            const u64 open = boundary_edges(p.mesh);
            if (verbose)
                std::printf("  climber %s: %llu boundary edges\n", label.c_str(), (unsigned long long)open);
            if (open != 0) {
                ++failures;
                std::fprintf(stderr, "presentation_tests: closed: %s has %llu boundary edges\n", label.c_str(),
                             (unsigned long long)open);
            }
        }
    }
    // The mountain: rock, decor and terrain of a few seeds.
    for (u64 seed : {1ull, 2ull, 7ull, 19ull}) {
        const SummitLevel level = generate_level(seed);
        const std::string tag = "seed " + std::to_string(seed) + " ";
        for (const RenderMeshData& m : build_render_meshes(level))
            expect_wound("rock", tag + m.name, m.mesh, m.material.double_sided);
        for (const RenderMeshData& m : build_decor_meshes(level))
            expect_wound("decor", tag + m.name, m.mesh, m.material.double_sided);
        for (const RenderMeshData& m : build_terrain_meshes(level))
            expect_wound("terrain", tag + m.name, m.mesh, m.material.double_sided, true);
    }
    // The primitives the view draws camps, fires, flags and the rope with.
    expect_wound("primitive", "cylinder", meshes::cylinder(1.0f, 1.0f, 10));
    expect_wound("primitive", "cone", meshes::cone(1.0f, 1.0f, 10));
    expect_wound("primitive", "cone4", meshes::cone(1.0f, 1.0f, 4));
    expect_wound("primitive", "box", meshes::box({1.0f, 1.0f, 1.0f}));
    expect_wound("primitive", "sphere", meshes::sphere(1.0f, 12, 8));
}

// ---- the person on screen: no oscillation under look and movement
// -------------------------------------------------------------
//
// A run of the interactive loop without a window: the sim steps at 60 Hz, frames come at `render_hz` (App::run's
// accumulator and alpha), the mouse motion since the last step is used up by the first step of a frame
// (Input::mouse_motion). Each frame the person is placed as the view places it and the camera is framed with the same
// alpha; what the eye sees is the person against the camera, so that is what is measured.

f32 wrap_degrees(f32 d) {
    d = std::fmod(d + 180.0f, 360.0f);
    if (d < 0.0f)
        d += 360.0f;
    return d - 180.0f;
}

struct Feel {
    f64 seconds = 0.0;
    f32 step_turn_max = 0.0f;     // the largest change of the sim's facing in one step (degrees)
    int facing_reversals = 0;     // direction changes of the facing between steps, with a dead band
    f32 lateral_p2p = 0.0f;       // the person's sideways position against the camera, high-frequency part (m)
    int lateral_reversals = 0;    // its direction changes between frames, with a dead band
    f32 facing_frame_jerk = 0.0f; // the largest second difference of the drawn facing between frames (degrees)
    f32 look_jerk = 0.0f;         // the largest second difference of the camera's yaw between frames (degrees)
    f32 camera_jerk = 0.0f;       // the largest second difference of the camera's position between frames (m)
    int frames = 0;
};

struct Scenario {
    const char* name = "";
    PlayerCommand base{};     // the keys held
    f32 sweep_degrees = 0.0f; // the look swings +- this far, sinusoidally
    f32 sweep_period = 4.0f;  // seconds
    f32 hold_look = 0.0f;     // a constant look rate (degrees a second)
    bool hang = false;        // the climb bot takes the first hold, then the keys are let be and only the look sweeps
    f64 seconds = 6.0;
};

enum class PoseSource { AsViewed, Raw };

Feel trace(const Scenario& sc, f64 render_hz, PoseSource source, const char* csv_prefix = nullptr) {
    std::FILE* csv_step = nullptr;
    std::FILE* csv_frame = nullptr;
    if (csv_prefix != nullptr) {
        csv_step = std::fopen((std::string(csv_prefix) + ".csv").c_str(), "w");
        csv_frame = std::fopen((std::string(csv_prefix) + "_frames.csv").c_str(), "w");
        const char* cols = "look_yaw,facing_yaw,char_x,char_y,char_z,cam_eye_x,cam_eye_y,cam_eye_z,cam_target_x,cam_"
                           "target_y,cam_target_z,"
                           "person_root_x,person_root_y,person_root_z,head_x,head_y,head_z";
        if (csv_step)
            std::fprintf(csv_step, "step,time_s,%s\n", cols);
        if (csv_frame)
            std::fprintf(csv_frame, "frame,time_s,step,alpha,%s\n", cols);
    }
    const auto row = [&](std::FILE* f, const Game& game, const CameraView& v, Vec3f root, f32 facing) {
        const Vec3f tgt = v.position + v.rotation.rotate({0.0f, 0.0f, -1.0f}) * v.arm;
        const Vec3f c = game.feet(0), h = game.head(0);
        std::fprintf(f, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                     double(game.player(0).yaw), double(facing), double(c.x), double(c.y), double(c.z),
                     double(v.position.x), double(v.position.y), double(v.position.z), double(tgt.x), double(tgt.y),
                     double(tgt.z), double(root.x), double(root.y), double(root.z), double(h.x), double(h.y),
                     double(h.z));
    };
    u64 step_count = 0;
    Game g(1);
    CameraRig rig;
    BotConfig config;
    script_config("climb", g.level(), config);
    Bot bot(g, 0, config);
    for (int i = 0; i < 90; ++i)
        g.step({}); // standing on the ground
    rig.observe(g, 0);
    Feel feel;
    const f64 step = f64(fixed_step);
    f64 acc = 0.0, t = 0.0, pending = 0.0, last_rate_phase = 0.0;
    std::vector<f32> step_facing;
    std::vector<f32> lat, face_frame;
    std::vector<Vec3f> cam;
    std::vector<f32> cam_yaw;
    std::vector<char> hang;
    f64 sweep_prev = 0.0;
    LookFeed feed;
    bool hung = false;
    std::size_t hung_from = 0;
    int held_steps = 0;
    PlayerCommand held_cmd;
    std::vector<char> obstructed;
    std::vector<f64> when;
    const f64 frame_dt = 1.0 / render_hz;
    (void)last_rate_phase;
    const u64 max_frames = u64(sc.seconds * render_hz);
    for (u64 frame = 0; frame < max_frames; ++frame) {
        // The mouse: the sweep's angle and the held rate, summed since the last read.
        f64 sweep = 0.0;
        if (sc.sweep_degrees != 0.0f)
            sweep = f64(sc.sweep_degrees) * std::sin(2.0 * 3.14159265358979 * t / f64(sc.sweep_period));
        pending += (sweep - sweep_prev) + f64(sc.hold_look) * frame_dt;
        sweep_prev = sweep;
        t += frame_dt;
        acc += frame_dt;
        bool first = true;
        while (acc >= step) {
            PlayerCommand c = sc.base;
            if (sc.hang && !hung) {
                // The bot works until both hands hold for a second; its last command's grips are kept after that.
                c = bot.next();
                held_cmd = c;
                const TraversalState3D ts = g.traversal(0);
                held_steps = (ts.hanging() && ts.held == 2) ? held_steps + 1 : 0;
                if (held_steps > 60) {
                    hung = true;
                    hung_from = step_facing.size() + 1;
                }
                t = 0.0;
                sweep_prev = 0.0;
            } else if (sc.hang) {
                c = held_cmd;
                c.move = {};
                c.look = {};
                c.jump = c.pull_up = c.release = false;
            }
            // look.x to the right lowers the yaw: the sweep is the yaw itself. The first step of a frame reads the
            // mouse.
            c.look = feed.take({c.look.x + (first ? f32(-pending) : 0.0f), c.look.y});
            if (first)
                pending = 0.0;
            first = false;
            g.step({&c, 1});
            rig.observe(g, 0);
            step_facing.push_back(g.player(0).facing_yaw);
            ++step_count;
            if (csv_step) {
                CameraRig copy = rig;
                const CameraView sv = copy.frame(g.world(), 1.0f, fixed_step);
                std::fprintf(csv_step, "%llu,%.5f,", (unsigned long long)step_count, double(step_count) * step);
                row(csv_step, g, sv, source == PoseSource::AsViewed ? rig.person_position(1.0f) : g.feet(0),
                    source == PoseSource::AsViewed ? rig.person_facing(1.0f) : g.player(0).facing_yaw);
            }
            acc -= step;
        }
        const f32 alpha = f32(acc / step);
        // What the hand turned that no step has taken yet (a frame without a step keeps the motion for the next one).
        rig.set_look_ahead({feed.pending().x + (first ? f32(-pending) : 0.0f), feed.pending().y});
        const CameraView v = rig.frame(g.world(), alpha, f32(frame_dt));
        Vec3f where = g.feet(0);
        f32 facing = g.player(0).facing_yaw;
        if (source == PoseSource::AsViewed) {
            where = rig.person_position(alpha);
            facing = rig.person_facing(alpha);
        }
        if (csv_frame) {
            std::fprintf(csv_frame, "%llu,%.5f,%llu,%.4f,", (unsigned long long)frame, t,
                         (unsigned long long)step_count, double(alpha));
            row(csv_frame, g, v, where, facing);
        }
        const Vec3f right = v.rotation.rotate({1.0f, 0.0f, 0.0f});
        lat.push_back(dot(where - v.position, right));
        face_frame.push_back(facing);
        cam.push_back(v.position);
        {
            const Vec3f fw = v.rotation.rotate({0.0f, 0.0f, -1.0f});
            cam_yaw.push_back(std::atan2(fw.x, fw.z) * 180.0f / 3.14159265f);
        }
        obstructed.push_back(v.obstructed ? 1 : 0);
        when.push_back(t);
        hang.push_back(sc.hang ? (hung ? 1 : 0) : 1);
    }
    if (csv_step)
        std::fclose(csv_step);
    if (csv_frame)
        std::fclose(csv_frame);
    // The sim's facing, step to step.
    {
        f32 last_delta = 0.0f;
        for (std::size_t i = std::max<std::size_t>(61, sc.hang ? hung_from + 60 : 0); i < step_facing.size(); ++i) {
            const f32 d = wrap_degrees(step_facing[i] - step_facing[i - 1]);
            feel.step_turn_max = std::max(feel.step_turn_max, std::fabs(d));
            if (std::fabs(d) > 0.05f) {
                if (last_delta != 0.0f && (d > 0.0f) != (last_delta > 0.0f))
                    ++feel.facing_reversals;
                last_delta = d;
            }
        }
    }
    // The frames: only where the person stood or hung for three frames running (a fall or a mantle is its own event).
    f32 last_d = 0.0f;
    f32 lat_hf = 0.0f;
    for (std::size_t i = 2; i < lat.size(); ++i) {
        // The first second is the body turning to its way; frames of an arm held in by rock are not the subject.
        if (!(hang[i] && hang[i - 1] && hang[i - 2]) || when[i] < 1.0 ||
            (obstructed[i] || obstructed[i - 1] || obstructed[i - 2])) {
            last_d = 0.0f;
            continue;
        }
        const f32 d = lat[i] - lat[i - 1];
        const f32 dd = lat[i] - 2.0f * lat[i - 1] + lat[i - 2];
        lat_hf = std::max(lat_hf, std::fabs(dd));
        if (std::fabs(d) > 0.0005f) {
            if (last_d != 0.0f && (d > 0.0f) != (last_d > 0.0f))
                ++feel.lateral_reversals;
            last_d = d;
        }
        const f32 fj =
            wrap_degrees(face_frame[i] - face_frame[i - 1]) - wrap_degrees(face_frame[i - 1] - face_frame[i - 2]);
        feel.facing_frame_jerk = std::max(feel.facing_frame_jerk, std::fabs(fj));
        const f32 yj = wrap_degrees(cam_yaw[i] - cam_yaw[i - 1]) - wrap_degrees(cam_yaw[i - 1] - cam_yaw[i - 2]);
        feel.look_jerk = std::max(feel.look_jerk, std::fabs(yj));
        const Vec3f cd = cam[i] - cam[i - 1] * 2.0f + cam[i - 2];
        feel.camera_jerk = std::max(feel.camera_jerk, length(cd));
        ++feel.frames;
    }
    feel.lateral_p2p = lat_hf;
    feel.seconds = f64(max_frames) * frame_dt;
    return feel;
}

void test_person_and_camera_do_not_shake() {
    // The report: "the character oscillates at high frequency, especially looking sideways", and the picture with it.
    // Two causes, both in how steps (60 Hz) meet frames (any rate): the person was drawn where the last step left it
    // against a camera drawn between two steps, and the mouse's motion went whole into the first step of a frame, so
    // that steps carried uneven turns and the camera (and the body that follows the look) shuddered at the beat of the
    // two rates. Run as the interactive loop runs, at four frame rates, nothing may reverse or jolt.
    struct Case {
        const char* name;
        Scenario sc;
    };
    std::vector<Case> cases;
    {
        Scenario s;
        s.base.move = {0.0f, 1.0f};
        s.sweep_degrees = 60.0f;
        cases.push_back({"walk forward, look sweeping", s});
    }
    {
        Scenario s;
        s.base.move = {1.0f, 0.0f};
        s.sweep_degrees = 60.0f;
        cases.push_back({"strafe, look sweeping", s});
    }
    {
        Scenario s;
        s.base.move = {0.0f, -1.0f};
        s.sweep_degrees = 60.0f;
        cases.push_back({"walk backwards, look sweeping", s});
    }
    {
        Scenario s;
        s.sweep_degrees = 60.0f;
        cases.push_back({"stand, look sweeping", s});
    }
    {
        Scenario s;
        s.hold_look = 30.0f;
        cases.push_back({"stand, steady look", s});
    }
    {
        Scenario s;
        s.base.move = {0.0f, 1.0f};
        s.base.sprint = true;
        s.hold_look = 10.0f;
        cases.push_back({"run, look turning", s});
    }
    {
        Scenario s;
        s.hang = true;
        s.sweep_degrees = 40.0f;
        s.seconds = 25.0;
        cases.push_back({"hanging, look sweeping", s});
    }
    for (const Case& c : cases) {
        for (const f64 hz : {30.0, 60.0, 90.0, 144.0}) {
            const Feel f = trace(c.sc, hz, PoseSource::AsViewed);
            const f64 dt = 1.0 / hz;
            // A look sweep of amplitude A and period T has this second difference a frame; a held rate has none.
            const f64 w = 2.0 * 3.14159265358979 / f64(c.sc.sweep_period);
            const f64 ideal = f64(c.sc.sweep_degrees) * w * w * dt * dt;
            if (verbose) {
                std::printf("  %-32s %5.0f Hz: lookjerk %.3f (ideal %.3f) lat_hf %.4f latrev %d facejerk %.2f facrev "
                            "%d stepturn %.2f\n",
                            c.name, hz, double(f.look_jerk), ideal, double(f.lateral_p2p), f.lateral_reversals,
                            double(f.facing_frame_jerk), f.facing_reversals, double(f.step_turn_max));
            }
            if (!c.sc.hang) {
                CHECK(f.frames > 100);
                CHECK(f.look_jerk <= f32(1.3 * ideal + 0.003)); // the camera turns as the hand does
            }
            // The person does not hop against the camera. (A hanging body settles on its holds once, a reversal or
            // two.)
            CHECK(f.lateral_reversals <= (c.sc.hang ? 2 : 0));
            if (!c.sc.hang)
                CHECK(f.lateral_p2p < 0.003f);
            CHECK(f.facing_frame_jerk <= 0.5f); // the body turns evenly between frames
            CHECK(f.facing_reversals <= 4);     // and only where the look turns about
            if (!c.sc.hang)
                CHECK(f.step_turn_max <= 12.01f); // never faster than turn_rate a step
            else
                CHECK(f.step_turn_max < 0.5f); // hanging: the facing is the rock's, and still
        }
    }
    // The measure itself: the person at the latest step against the camera between steps is seen to hop (what the view
    // did).
    Scenario strafe;
    strafe.base.move = {1.0f, 0.0f};
    strafe.sweep_degrees = 60.0f;
    const Feel raw = trace(strafe, 144.0, PoseSource::Raw);
    CHECK(raw.lateral_p2p > 0.03f);
    CHECK(raw.lateral_reversals > 100);
}

void test_look_feed_is_even_and_loses_nothing() {
    LookFeed feed;
    // A burst of 10 degrees: given out in shrinking even shares, all of it in the end, none lost, the rest always
    // visible.
    Vec2f total{};
    Vec2f first = feed.take({10.0f, -4.0f});
    total = first;
    CHECK_NEAR(first.x, 5.0, 1e-4);
    CHECK_NEAR(feed.pending().x + total.x, 10.0, 1e-4);
    for (int i = 0; i < 40; ++i) {
        const Vec2f s = feed.take({});
        CHECK(s.x >= 0.0f && s.x <= first.x);
        total.x += s.x;
        total.y += s.y;
    }
    CHECK_NEAR(total.x, 10.0, 1e-3);
    CHECK_NEAR(total.y, -4.0, 1e-3);
    // Even input comes out even (the first steps ease in, then it is the input itself).
    LookFeed steady;
    f32 out = 0.0f;
    for (int i = 0; i < 30; ++i)
        out = steady.take({1.0f, 0.0f}).x;
    CHECK_NEAR(out, 1.0, 1e-3);
    // The camera sees the hand: what is given plus what waits is what was turned.
    CameraRig rig;
    Game g(1);
    for (int i = 0; i < 10; ++i)
        g.step({});
    rig.observe(g, 0);
    const Quatf a = rig.frame(g.world(), 1.0f, fixed_step).rotation;
    rig.set_look_ahead({15.0f, 0.0f});
    const Quatf b = rig.frame(g.world(), 1.0f, fixed_step).rotation;
    const Vec3f fa = a.rotate({0.0f, 0.0f, -1.0f}), fb = b.rotate({0.0f, 0.0f, -1.0f});
    CHECK_NEAR(std::acos(std::clamp(fa.x * fb.x + fa.z * fb.z, -1.0f, 1.0f)) * 180.0f / 3.14159265f, 15.0, 0.3);
    // Not-a-number input is no turn.
    rig.set_look_ahead({std::nanf(""), 1.0f});
    const Vec3f fc = rig.frame(g.world(), 1.0f, fixed_step).rotation.rotate({0.0f, 0.0f, -1.0f});
    CHECK(std::isfinite(fc.x) && std::isfinite(fc.z));
}

void test_trace_numbers() {
    if (const char* dir = std::getenv("TRACE_CSV")) {
        const bool fixed = std::getenv("TRACE_FIXED") != nullptr;
        const PoseSource src = fixed ? PoseSource::AsViewed : PoseSource::Raw;
        Scenario a;
        a.name = "standing, steady lateral sweep";
        a.hold_look = 30.0f;
        a.seconds = 4.0;
        Scenario b;
        b.name = "standing, small back and forth";
        b.sweep_degrees = 5.0f;
        b.sweep_period = 0.5f;
        b.seconds = 4.0;
        trace(a, 144.0, src, (std::string(dir) + "/trace_standing_look" + (fixed ? "_fixed" : "") + "_steady").c_str());
        trace(b, 144.0, src, (std::string(dir) + "/trace_standing_look" + (fixed ? "_fixed" : "") + "_small").c_str());
        return;
    }
    std::vector<Scenario> list;
    {
        Scenario s;
        s.name = "walk forward, look sweeping";
        s.base.move = {0.0f, 1.0f};
        s.sweep_degrees = 60.0f;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "strafe right, look sweeping";
        s.base.move = {1.0f, 0.0f};
        s.sweep_degrees = 60.0f;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "walk backwards, look sweeping";
        s.base.move = {0.0f, -1.0f};
        s.sweep_degrees = 60.0f;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "stand still, look sweeping";
        s.sweep_degrees = 60.0f;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "stand still, steady look";
        s.hold_look = 30.0f;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "run forward, look held turning";
        s.base.move = {0.0f, 1.0f};
        s.base.sprint = true;
        s.hold_look = 10.0f;
        list.push_back(s);
    }
    {
        Scenario s;
        s.name = "hanging, look sweeping";
        s.hang = true;
        s.sweep_degrees = 60.0f;
        s.seconds = 60.0;
        list.push_back(s);
    }
    for (const Scenario& sc : list) {
        for (const f64 hz : {30.0, 60.0, 90.0, 144.0}) {
            for (const PoseSource src : {PoseSource::Raw, PoseSource::AsViewed}) {
                const Feel f = trace(sc, hz, src);
                std::printf("TRACE %-36s %5.0f Hz %s: frames %d stepturn %.2f deg facrev %d  lat_hf %.4f m latrev %d "
                            "facejerk %.2f lookjerk %.3f camjerk %.4f\n",
                            sc.name, hz, src == PoseSource::Raw ? "raw" : "view", f.frames, double(f.step_turn_max),
                            f.facing_reversals, double(f.lateral_p2p), f.lateral_reversals, double(f.facing_frame_jerk),
                            double(f.look_jerk), double(f.camera_jerk));
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-v")
            verbose = true;
    }
    const auto run = [](const char* name, void (*f)()) {
        const auto t0 = Clock::now();
        const int before = failures;
        f();
        if (verbose || failures != before) {
            std::printf("presentation_tests: %-46s %s (%.2f s)\n", name, failures == before ? "ok" : "FAILED",
                        std::chrono::duration<double>(Clock::now() - t0).count());
        }
    };
    run("stamina tone and colour", test_stamina_tone_and_colour);
    run("fall warning thresholds", test_fall_warning_thresholds);
    run("hud model along the climb", test_hud_model_along_the_climb);
    run("one hand is the hand that holds", test_one_hand_is_the_hand_that_holds);
    run("injury and status", test_injury_and_status);
    run("notices come and go", test_notices_come_and_go);
    run("layout fits at several sizes", test_layout_fits_at_four_sizes);
    run("menu navigation", test_menu_navigation);
    run("controls screen shows the bindings", test_controls_screen_shows_current_bindings);
    run("restart from camp", test_restart_from_camp);
    run("audio curves", test_audio_curves);
    run("cues of the scripted climb", test_cues_of_the_scripted_climb);
    run("cadence follows speed in the run", test_cadence_follows_speed_in_the_run);
    run("cues of the scripted fall", test_cues_of_the_scripted_fall);
    run("audio engine plays the cues", test_audio_engine_plays_the_cues);
    run("environment curves", test_environment_curves);
    run("time of day clock", test_time_of_day_clock);
    run("settings round trip and fallback", test_settings_round_trip_and_fallback);
    run("camera frames a lying body", test_camera_frames_a_lying_body);
    run("person and camera do not shake", test_person_and_camera_do_not_shake);
    run("look feed is even and loses nothing", test_look_feed_is_even_and_loses_nothing);
    if (std::getenv("TRACE") || std::getenv("TRACE_CSV"))
        run("trace numbers", test_trace_numbers);
    run("look pass meshes", test_look_pass_meshes);
    run("winding", test_winding);
    run("presentation does not change the simulation", test_presentation_does_not_change_the_simulation);
    if (failures != 0) {
        std::fprintf(stderr, "presentation_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("presentation_tests: all passed\n");
    return 0;
}
