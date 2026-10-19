#pragma once

// Summit's HUD and menu (docs/3d_plan.md, P4b), as data first. `build_hud_model` turns the game's state into a
// `HudModel` (plain data: bars, colours, hands, warnings, texts), `hud_layout` places its parts on a screen with ui2's
// layout solver, and `Menu` is the pause menu's state machine driven by `MenuInput` edges. None of it needs a window, a
// renderer or a font, so they are checked on the CPU; `draw_hud` and `draw_menu` (hud_draw.cpp) only paint what these
// say. Nothing here writes the game.
//
//     summit::HudNotices notices;                                  // what lasts a moment: "camp 3", the summit
//     ...once a step, after game.step():  notices.observe(game, 0);
//     const summit::HudModel m = summit::build_hud_model(game, 0, notices);
//     const summit::HudLayout l = summit::hud_layout({1280, 720}, m);

#include "game.hpp"
#include "input_map.hpp"
#include "settings.hpp"

#include <algorithm>
#include <kin/renderer/color.hpp>
#include <kin/ui2/geometry.hpp>
#include <string>
#include <vector>

namespace summit {
using namespace kin;

enum class HandState : u8 { Free, Reaching, Held, Slipping };

// How the next landing would go, from how fast it falls now: Brace (nearing the speed where a landing hurts), Hurt
// (over it), KnockOut (over the speed that knocks out).
enum class FallWarning : u8 { None, Brace, Hurt, KnockOut };

enum class StaminaTone : u8 { Good, Low, Spent };

enum class StatusKind : u8 { Standing, Walking, Sprinting, InAir, Hanging, PullingUp, KnockedOut, GettingUp, Dead };

const char* status_text(StatusKind s);

// Thresholds of the cues, in one place (tested).
struct HudRules {
    f32 low_stamina = 0.30f;      // of the full bar: the bar turns amber
    f32 critical_stamina = 0.12f; // and red and pulsing
    f32 brace_fraction = 0.65f;   // of the safe landing speed
    f32 notice_seconds = 4.0f;    // a camp notice stays this long
    f32 hurt_seconds = 1.2f;      // the health bar's flash
};

// What lasts a moment, fed by the game's events once a step. Presentation only: it is not in the game's hash or its
// saves.
struct HudNotices {
    i32 camp = -1;
    f32 camp_seconds = 0.0f;
    f32 hurt = 0.0f; // health lost lately (fades)
    f32 hurt_seconds = 0.0f;
    bool summit = false; // the summit message is up (it stays)
    i32 summit_step = -1;
    i32 restarts = 0; // the menu's "restart from camp" counted

    // One fixed step: age the notices, then take this step's events.
    void observe(const Game& game, i32 player = 0, const HudRules& rules = {});
    void reset() { *this = {}; }
};

struct HudModel {
    // Stamina: `value` of `max` is the player's bar, `cap` is what injury leaves of the bar (value never passes it).
    f32 stamina = 0.0f, stamina_max = 1.0f, stamina_cap = 1.0f;
    f32 stamina_fraction = 0.0f; // value / max, 0 to 1
    f32 cap_fraction = 1.0f;     // cap / max
    bool exhausted = false;
    StaminaTone tone = StaminaTone::Good;
    Color stamina_colour{};
    bool stamina_pulse = false; // blinks (critical or spent)

    // The hands, left then right.
    HandState left = HandState::Free, right = HandState::Free;
    i32 hands_held = 0;
    bool hanging = false;
    bool pulling_up = false;

    f32 health = 100.0f, health_max = 100.0f;
    f32 health_fraction = 1.0f;
    std::string injury; // "", "Hurt", "Badly hurt", "Critical"
    Color health_colour{};
    f32 hurt_flash = 0.0f; // 0 to 1

    f32 altitude = 0.0f, best_altitude = 0.0f, summit_altitude = 1.0f;
    f32 progress = 0.0f;   // altitude over the summit's, 0 to 1
    f32 fall_speed = 0.0f; // m/s, positive down (0 when not falling)
    FallWarning fall_warning = FallWarning::None;
    StatusKind status = StatusKind::Standing;

    // The camp notice ("Camp 3"), empty when none; the summit message with the run's time and falls.
    std::string camp_notice;
    f32 camp_notice_alpha = 0.0f;
    bool at_camp = false;
    bool summit = false;
    std::string summit_message; // "Summit! 2:43, 0 falls"
    f32 run_seconds = 0.0f;
    i32 falls = 0;
    i32 camp = 0;
    bool expedition = false, sheltered = false, shelter_visited = false;
    ExpeditionPhase phase = ExpeditionPhase::Briefing;
    std::string objective, objective_detail, interaction;
    f32 interaction_progress = 0.0f;
};

HudModel build_hud_model(const Game& game, i32 player, const HudNotices& notices, const HudRules& rules = {});

// The stamina bar's colour for a fraction and the exhausted flag (green, amber under `low`, red under `critical` or
// spent).
Color stamina_colour(f32 fraction, bool exhausted, const HudRules& rules = {});
StaminaTone stamina_tone(f32 fraction, bool exhausted, const HudRules& rules = {});
// The fall warning for a speed down (m/s) given the rules' safe landing and stun speeds.
FallWarning fall_warning_for(f32 speed_down, const PlayerRules& rules, const HudRules& hud = {});
// "2:43".
std::string clock_text(f32 seconds);

// ---- layout
// ----------------------------------------------------------------------------------------------------------------

// Rectangles in pixels. Every rect is inside the screen and none of the visible ones overlap (tested at four sizes);
// `u` is the scale from the 1280x720 design, to which text sizes follow.
struct HudLayout {
    Vec2f screen{};
    f32 u = 1.0f;
    Rectf altitude{};
    Rectf health{};
    Rectf stamina{}; // the bar
    Rectf hands{};   // the two grips above it
    Rectf hand_left{}, hand_right{};
    Rectf warning{}; // "BRACE"
    Rectf notice{};  // camp
    Rectf summit{};  // the message
    Rectf hint{};    // the key hint line, bottom left
    Rectf debug{};   // F3's line, top centre
    Rectf objective{}, interaction{}, expedition_panel{};
};

// `scale` multiplies the size on top of the fit (a player's HUD size option).
HudLayout hud_layout(Vec2f screen, f32 scale = 1.0f);

// What it says: the labels, so that a test can look at them (one place for the wording).
std::string hud_hint_line(const InputBindings& bindings);

// ---- the menu
// --------------------------------------------------------------------------------------------------------------

enum class MenuScreen : u8 { Closed, Main, Controls };
enum class MenuRow : u8 {
    Resume,
    Restart,
    Look,
    Camera,
    MasterVolume,
    SfxVolume,
    AmbienceVolume,
    Sensitivity,
    InvertLook,
    TimeOfDay,
    Controls,
    Quit,
    Count
};
inline constexpr std::size_t menu_row_count = static_cast<std::size_t>(MenuRow::Count);

// Edges for one frame (each true once per press).
struct MenuInput {
    bool up = false, down = false, left = false, right = false, accept = false, back = false;
    friend bool operator==(const MenuInput&, const MenuInput&) = default;
    bool any() const { return up || down || left || right || accept || back; }
};

// What the owner has to do after a navigation step.
enum class MenuAction : u8 { None, Resume, RestartFromCamp, Quit, Changed };

// Edges of the menu keys and pad between two raw inputs: arrows (and W/S/A/D), Enter or Space accepts, Escape or
// Backspace backs, the pad's D-pad and left stick (past 0.6), A accepts and B backs; Start and Escape also open it
// (`toggle`).
struct MenuEdges {
    MenuInput input;
    bool toggle = false; // Escape or Start: open or close the menu
};
MenuEdges menu_edges(const RawInput& previous, const RawInput& now);

class Menu {
public:
    explicit Menu(SummitSettings* settings = nullptr) : _settings(settings) {}
    void set_settings(SummitSettings* settings) { _settings = settings; }
    void set_expedition_complete(bool complete) { _expedition_complete = complete; }

    MenuScreen screen() const { return _screen; }
    bool open() const { return _screen != MenuScreen::Closed; }
    i32 focus() const { return _focus; }
    void set_focus(i32 row) { _focus = std::clamp(row, 0, i32(menu_row_count) - 1); }
    void open_main() { _screen = MenuScreen::Main; }
    void open_controls() { _screen = MenuScreen::Controls; }
    void close() { _screen = MenuScreen::Closed; }
    // Escape or Start: closed opens, the controls screen goes back to the main one, the main one closes.
    MenuAction toggle();

    // One frame of edges. Up and down move the focus (wrapping), left and right change a choice or a slider by a step,
    // accept presses a button (Look and Camera cycle, a toggle flips), back leaves.
    MenuAction navigate(const MenuInput& in);
    // The row's label and value text ("Master volume", "80%").
    std::string label(MenuRow row) const;
    std::string value(MenuRow row) const;
    // 0 to 1 for a slider row, -1 for one that has none.
    f32 slider(MenuRow row) const;
    // Sets a slider row from a 0 to 1 position (a click on its track); true if it is a slider row.
    bool set_slider(MenuRow row, f32 fraction);

private:
    SummitSettings* _settings = nullptr;
    MenuScreen _screen = MenuScreen::Closed;
    i32 _focus = 0;
    bool _expedition_complete = false;
};

// The menu's "restart from camp": puts a live player back at the last camp with full health, as a respawn would. This
// is a deliberate act of the person at the keyboard made through the game's two state hooks (a network layer would
// carry it as a command, P5); it is the only way presentation changes the game, and it does nothing for a player who is
// knocked out or dead (the game respawns them itself). Returns whether it did.
bool restart_from_camp(Game& game, i32 player = 0);

// The controls screen: a line for each action with the keys, buttons and triggers bound to it ("Jump: Space, pad A").
struct ControlLine {
    std::string action;
    std::string keys;
};
std::vector<ControlLine> controls_screen(const InputBindings& bindings);
// One binding's name for the screen ("Space", "Left mouse", "Pad A", "Left trigger").
std::string binding_label(const Binding& b);

// The menu's own layout: the panel centred, the title, then a row each of equal height, all inside the screen.
struct MenuLayout {
    Rectf panel{};
    Rectf title{};
    std::vector<Rectf> rows;
    Rectf footer{};
    f32 u = 1.0f;
};
MenuLayout menu_layout(Vec2f screen, i32 rows);
// The slider's track inside a menu row (the right part of it): where a click sets the value.
Rectf menu_row_track(const Rectf& row);
// The row under `pointer` (-1: none).
i32 menu_row_at(const MenuLayout& layout, Vec2f pointer);

} // namespace summit
