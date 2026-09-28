#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/input.hpp> // MouseButton

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

struct Interaction {
    bool hot = false;      // pointer is over this region (resolved from last frame)
    bool active = false;   // press started here and button still held
    bool focused = false;  // keyboard/gamepad focus
    bool pressed = false;  // press began over this region this frame
    bool released = false; // release happened while active this frame
    bool clicked = false;  // press + release completed over this region (activation)
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

    bool has_selection() const { return selection_anchor != unset_selection && selection_anchor != caret; }
    std::size_t selection_start() const { return !has_selection() ? caret : (selection_anchor < caret ? selection_anchor : caret); }
    std::size_t selection_end() const { return !has_selection() ? caret : (selection_anchor < caret ? caret : selection_anchor); }
    void clear_selection() { selection_anchor = caret; }
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
    void set_focused(Id id) { _focused = id; _focused_this_frame = true; }
    void clear_focused() { _focused = {}; }

    // True if set_focused() was called at least once this frame (reset by begin_frame()).
    // Used by end() to distinguish "clicked empty space" from "single-frame tap on a widget".
    bool focused_this_frame() const { return _focused_this_frame; }

    // Record a region containing the pointer this frame; `z` is depth (higher == on top).
    // The topmost recorded region becomes next frame's hot.
    void register_hit(Id id, i32 z);

private:
    u64 _frame = 0;
    Id _hot{};
    Id _active{};
    MouseButton _active_button = MouseButton::Left; // button that called set_active()
    Id _focused{};
    bool _focused_this_frame = false; // cleared each begin_frame(); guards the focus-clear in end()
    Id _hit_id{};
    i32 _hit_z = 0;
    bool _has_hit = false;
};

} // namespace kin::ui2
