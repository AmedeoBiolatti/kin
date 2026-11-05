#pragma once

// On-screen controls for touch screens: a stick and buttons drawn over the
// game that hold its actions and axes (Input::set_screen_action and
// set_screen_axis), each following a finger of its own, so a player moves and
// presses at once. A finger that lands on one is taken from the pointer
// (Input::claim_touch): it clicks nothing under the control. Run them each
// frame between Context::begin() and end(), as any widget; a control not run
// for a frame lets go of what it held. A screen file places them with its
// touch_stick and touch_button elements (kin/ui2/screen.hpp).
//
//     ui2::TouchStick stick{.id = ui2::make_id("move"), .bounds = {0, 0, 640, 720}};
//     ui2::TouchButton jump{.id = ui2::make_id("jump"), .bounds = {1120, 560, 110, 110}, .action = "jump", .label = "Jump"};
//     ui2::run(ui, stick); // move_x and move_y, then
//     ui2::run(ui, jump);  // input.pressed("jump") in the next update()

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/ui2/state.hpp>
#include <kin/ui2/widgets.hpp> // TextStyle

#include <optional>
#include <string>

namespace kin::ui2 {

class Context;

enum class TouchControlsShown : u8 {
    Touch,  // while a finger is the pointer (Input::pointer_is_touch), and always on a phone
    Always, // a desktop with a mouse too: the mouse works them
};

// Whether on-screen controls show now, as `shown` says.
bool touch_controls_shown(const Context& ctx, TouchControlsShown shown = TouchControlsShown::Touch);

// A stick: a finger that lands in `bounds` pushes a knob from the middle, and
// its two axes are how far, -1 to 1 each way: x to the right, y downwards, as
// the screen shows them (an input file reads a controller's stick the same
// way with `axis move_x stick left_x` and `axis move_y stick -left_y`).
struct TouchStick {
    Id id{};
    Rectf bounds{};               // where a finger starts it (drawing coordinates)
    std::string axis_x = "move_x"; // empty: not set
    std::string axis_y = "move_y";
    f32 radius = 64.0f;           // how far the knob goes from the middle
    // The middle where the finger lands (a stick anywhere in `bounds`, which
    // may be half the screen); else the middle of `bounds`.
    bool floating = true;
    f32 dead_zone = 0.12f; // of the radius: 0 until past it, then stretched to the end
    TouchControlsShown shown = TouchControlsShown::Touch;
    Color base = Color::rgba(255, 255, 255, 40);
    Color knob = Color::rgba(255, 255, 255, 110);

    // Out: where it is pushed, and whether a finger (or the mouse) has it.
    Vec2f value{};
    bool active = false;
    // Kept from frame to frame: keep the TouchStick.
    std::optional<u64> finger;
    bool mouse = false;
    Vec2f origin{};
};

// A button: held while the finger that landed on it stays over it, and its
// action with it.
struct TouchButton {
    Id id{};
    Rectf bounds{}; // drawing coordinates; drawn round (a pill when not square)
    std::string action;
    std::string label;
    TextStyle text_style{}; // the theme's body text when no font is given
    TouchControlsShown shown = TouchControlsShown::Touch;
    Color fill = Color::rgba(255, 255, 255, 48);
    Color held_fill = Color::rgba(255, 255, 255, 120);
    Color label_color = Color::rgba(255, 255, 255, 220);

    // Out: held now, and went down this frame.
    bool held = false;
    bool pressed = false;
    // Kept from frame to frame: keep the TouchButton.
    std::optional<u64> finger;
    bool mouse = false;
};

void run(Context& ctx, TouchStick& stick);
void run(Context& ctx, TouchButton& button);
// Their size where a layout fits them: the stick's base, a thumb-sized button.
Vec2f measure(const TouchStick& stick);
Vec2f measure(const TouchButton& button);

} // namespace kin::ui2
