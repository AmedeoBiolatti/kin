#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/input.hpp> // MouseButton

#include <concepts>
#include <filesystem>
#include <string>
#include <string_view>

namespace kin::ui2 {

struct Id {
    u64 value = 0;

    explicit constexpr operator bool() const { return value != 0; }
    friend constexpr bool operator==(Id, Id) = default;
};

// Stable hashed ids. The (Id, u64) overload lets an ECS front-end derive identity
// directly from an entity id later, with no separate string id.
Id make_id(std::string_view label);
Id make_id(Id parent, std::string_view label);
Id make_id(Id parent, u64 value);
// A part of a labelled group: a row by its name or index ("file", name).
Id make_id(std::string_view label, std::string_view part);
Id make_id(std::string_view label, u64 part);
// By a path (a file's row): its UTF-8 text with '/' between its parts, so
// the same file has the same id on every system.
Id make_path_id(Id parent, const std::filesystem::path& path);
template <typename P>
    requires std::same_as<P, std::filesystem::path>
Id make_id(Id parent, const P& path) {
    return make_path_id(parent, path);
}

struct Interaction {
    bool hot = false;      // pointer is over this region (resolved from last frame)
    bool active = false;   // press started here and button still held
    bool focused = false;  // keyboard/gamepad focus
    bool pressed = false;  // press began over this region this frame
    bool released = false; // release happened while active this frame
    bool clicked = false;  // press + release completed over this region (activation)
    bool quiet_focus = false; // focused by a pointer press: no focus ring (the keys' focus shows one)
};

enum class UiButtonState {
    Normal,
    Hovered,
    Focused,
    Pressed,
};

struct UiTextInputState {
    static constexpr std::size_t unset_selection = static_cast<std::size_t>(-1);

    std::string text;
    std::size_t caret = 0;
    std::size_t selection_anchor = unset_selection;
    bool active = false;
    // How far text longer than the field is slid left, so the caret stays in view
    // while editing (0 when not: the field shows the text's start). Kept by the widget.
    f32 scroll_x = 0.0f;

    bool has_selection() const { return selection_anchor != unset_selection && selection_anchor != caret; }
    std::size_t selection_start() const { return !has_selection() ? caret : (selection_anchor < caret ? selection_anchor : caret); }
    std::size_t selection_end() const { return !has_selection() ? caret : (selection_anchor < caret ? caret : selection_anchor); }
    void clear_selection() { selection_anchor = caret; }
    bool operator==(const UiTextInputState&) const = default;
};

struct UiTextInputResult {
    UiButtonState state = UiButtonState::Normal;
    bool changed = false;
    bool committed = false;
    bool cancelled = false;
};

struct UiMenuState {
    i32 selected = 0;
    bool wrap = true;
};

struct UiComboState {
    i32 selected = 0;
    bool open = false;
    UiMenuState menu;
};

struct UiComboResult {
    UiButtonState state = UiButtonState::Normal;
    i32 selected = 0;
    bool changed = false;
    bool opened = false;
    bool closed = false;
};

// Persists across frames; one instance per Context. Hot is resolved one frame late
// (standard immediate-mode trick) so call order never matters and overlap is correct.
class State {
public:
    void begin_frame();
    void clear();

    u64 frame() const { return _frame; }
    Id hot() const { return _hot; }
    i32 hot_z() const { return _hot_z; } // the depth hot() was hit at
    Id active() const { return _active; }
    Id focused() const { return _focused; }

    bool is_hot(Id id) const { return static_cast<bool>(id) && _hot == id; }
    bool is_active(Id id) const { return static_cast<bool>(id) && _active == id; }
    bool is_focused(Id id) const { return static_cast<bool>(id) && _focused == id; }

    // Which button last called set_active(). The end() failsafe uses this to test
    // the correct button rather than hardcoding Left.
    MouseButton active_button() const { return _active_button; }

    void set_active(Id id, MouseButton button) { _active = id; _active_button = button; }
    void clear_active() { _active = {}; _active_button = MouseButton::Left; }
    // Focus given by the keys (Tab) or the app shows its ring; by a press
    // (`visible` false) it does not.
    void set_focused(Id id, bool visible = true) {
        _focused = id;
        _focus_visible = visible;
        _focused_this_frame = true;
    }
    bool focus_visible() const { return _focus_visible; }
    void clear_focused() { _focused = {}; }

    // True if set_focused() was called at least once this frame (reset by begin_frame()).
    // Used by end() to distinguish "clicked empty space" from "single-frame tap on a widget".
    bool focused_this_frame() const { return _focused_this_frame; }

    // Record a region containing the pointer this frame; `z` is depth (higher == on top).
    // The topmost recorded region becomes next frame's hot.
    void register_hit(Id id, i32 z);

    // A click on `id` at `pos` at `now` (seconds, Context::now()): true when it
    // follows a click on the same id within double_click_seconds and a few
    // pixels (a double click). Timed, not counted in frames: a window that
    // waits for events draws few.
    static constexpr f64 double_click_seconds = 0.45;
    bool register_click(Id id, Vec2f pos, f64 now);

private:
    u64 _frame = 0;
    Id _hot{};
    i32 _hot_z = 0;
    Id _active{};
    MouseButton _active_button = MouseButton::Left; // button that called set_active()
    Id _focused{};
    bool _focused_this_frame = false; // cleared each begin_frame(); guards the focus-clear in end()
    bool _focus_visible = false;
    Id _hit_id{};
    i32 _hit_z = 0;
    bool _has_hit = false;
    Id _last_click_id{};
    f64 _last_click_time = 0.0;
    Vec2f _last_click_pos{};
};

} // namespace kin::ui2
