#pragma once

#include <kin/ui2/state.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace kin {
class JsonWriter;
}
namespace kin::ui2 {

struct SemanticElement {
    Id id{};
    Id parent{};
    Id modal{}; // owning modal, if any
    std::string key; // optional author-facing id, e.g. a .kinui element's id
    std::string role = "region";
    std::string label;
    std::string value;
    Rectf bounds{}; // logical drawing coordinates, after mirrors/clipping
    bool enabled = true;
    bool visible = true;
    bool blocked = false; // outside the active modal or focus trap
    bool focusable = false;
    bool focused = false; // as Context::end() left it
    bool checked = false;
    std::optional<f64> numeric_value, numeric_min, numeric_max, numeric_step;

    // Whether it can take an action now: enabled, visible and not blocked.
    bool available() const { return enabled && visible && !blocked; }
};

enum class SemanticAction { Click, Focus, SetText, Increment, Decrement, SetNumber };

// Whether a role has `action`: every role clicks and focuses; set_text needs a
// text input ("text_input", "text_edit"), increment/decrement/set_number a
// slider or number input ("slider", "spinbutton").
bool semantic_role_accepts(std::string_view role, SemanticAction action);

struct ControllerNavigation {
    bool enabled = true;
    f32 dead_zone = 0.5f;
    f32 repeat_delay = 0.4f;
    f32 repeat_interval = 0.1f;
    // Whose controller moves it: Input::player_gamepad(player); -1 the one
    // Input::gamepad() gives (player 0's, else the first).
    i32 player = -1;
    // The input map's menu_up/menu_down/menu_left/menu_right move it too, and
    // accept and quit accept and back out, on whatever they are bound to (that
    // player's, with `player`). They do not make controller_active().
    bool actions = true;
};

} // namespace kin::ui2
