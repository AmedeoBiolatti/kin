#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/sprite.hpp>
#include <kin/renderer/texture.hpp>
#include <kin/ui2/image.hpp>
#include <kin/ui2/layout.hpp> // UiPadding
#include <kin/ui2/state.hpp>
#include <kin/ui2/table.hpp>
#include <kin/ui2/text.hpp>
#include <kin/ui2/tree.hpp>

#include <functional>
#include <limits>
#include <optional>
#include <concepts>
#include <string>
#include <variant>
#include <vector>

namespace kin::ui2 {

class Context;

struct TextStyle {
    Font font{};
    f32 scale = 2.0f;
    Color color = Color::rgb(232, 245, 236);

    // --- advanced look (Layer B) ---
    Color shadow_color = colors::transparent; // drop shadow drawn at pos + shadow_offset
    Vec2f shadow_offset{0.0f, 0.0f};
    Color outline_color = colors::transparent; // 4-way outline when outline_width > 0
    f32 outline_width = 0.0f;
    // Fields at their defaults take the theme's (a widget's text); false: used
    // as it is, defaults too (the default colour, scale 2).
    bool inherit = true;
};

// Shared palette for interactive widgets.
struct WidgetStyle {
    InteractiveSurfaceStyle surface{};
    Color accent{};
    Color track{};
    UiPadding padding{};
    // Shared control-row height. 0 = auto (size to content); when set, the four form
    // controls (Button/TextInput/ComboBox/NumberInput) floor their measured height at
    // this value so they line up in a row, and still grow past it for large content.
    // Themed factories set it to the theme's compact control height; raw styles leave 0.
    f32 min_height = 0.0f;
    // Fields at their defaults take the theme's; false: used as it is, defaults
    // too (square corners, no border, no fill): copy the theme's
    // (Context::theme().button) and change what differs.
    bool inherit = true;
};

enum class ImageFit {
    Stretch,
    Contain,
    Cover,
};

enum class IconLabelPlacement {
    None,
    Right,
    Below,
};

enum class SeparatorAxis {
    Horizontal,
    Vertical,
};

enum class ScrollAxis {
    Vertical,
    Horizontal,
};

// What a widget does with text wider than its bounds.
//   Visible   draws it all, past the bounds (reported as an overflow)
//   Clip      cuts it off at the bounds (reported as an overflow)
//   Ellipsis  ends it with "…" where it would leave the bounds
//   Shrink    draws it smaller to fit (to 70%), then with "…"
enum class TextOverflow {
    Visible,
    Clip,
    Ellipsis,
    Shrink,
};

struct ScrollState {
    Vec2f offset{};
    Vec2f content_size{};
    Vec2f viewport_size{};
};

struct ScrollOptions {
    Vec2f wheel_step{48.0f, 48.0f};
    f32 scrollbar_thickness = 10.0f;
    f32 min_thumb = 18.0f;
    bool show_scrollbar = true;
    bool enabled = true;
    bool wheel_enabled = true;
};

struct ScrollbarLayout {
    Rectf track{};
    Rectf thumb{};
    bool scrollable = false;
};

struct ScrollResult {
    ScrollState state{};
    bool changed = false;
    bool dragging = false;
    bool consumed_wheel = false;
};

// Every widget is a plain struct: usable as a stack local (immediate) or, later, as an
// ECS component. `bounds` is filled by the layout solver before `run` draws it. Result
// fields (clicked/changed/interaction) are written in place by `run`.
struct Panel {
    Id id{};
    Rectf bounds{};
    Color color = Color::rgba(7, 9, 14, 208);
    Color border = Color::rgb(78, 88, 110);
    // When true the panel consumes pointer interaction over its bounds, so widgets behind
    // it (at lower or equal z) stop receiving hover/clicks — e.g. a modal backdrop. Requires
    // a non-zero id (the ECS front-end assigns one automatically). `z` sets the block depth.
    bool block_input = false;
    i32 z = 0;
    Interaction interaction{};
};

struct Label {
    Id id{};
    Rectf bounds{};
    std::string text;
    TextStyle text_style{};
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Center;
    TextOverflow overflow = TextOverflow::Visible;
};

struct WrappedText {
    Rectf bounds{};
    std::string text;
    TextStyle text_style{};
    f32 line_spacing = 2.0f;
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Start;
    TextOverflow overflow = TextOverflow::Visible;
};

struct Separator {
    Rectf bounds{};
    SeparatorAxis axis = SeparatorAxis::Horizontal;
    Color color = Color::rgb(78, 88, 110);
    f32 thickness = 1.0f;
};

struct Button {
    Id id{};
    Rectf bounds{};
    std::string label;
    TextStyle text_style{};
    WidgetStyle style{};
    TextOverflow overflow = TextOverflow::Visible;
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    bool clicked = false;
};

struct Toggle {
    Id id{};
    Rectf bounds{};
    std::string label;
    TextStyle text_style{};
    WidgetStyle style{};
    bool value = false;
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    bool changed = false;
};

struct Slider {
    Id id{};
    Rectf bounds{};
    f32 value = 0.0f;
    f32 min = 0.0f;
    f32 max = 1.0f;
    f32 step = 0.0f;
    WidgetStyle style{};
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    bool changed = false;
};

struct ProgressBar {
    Id id{};
    Rectf bounds{};
    f32 value = 0.0f;
    Color fill = Color::rgb(128, 226, 160);
    Color background = Color::rgb(24, 28, 38);
    Color border = colors::transparent;
};

struct Image {
    Rectf bounds{};
    Texture texture{};
    Sprite sprite{};
    Vec2f size{};
    Color tint = colors::white;
    ImageFit fit = ImageFit::Stretch;
};

struct NineSlicePanel {
    Rectf bounds{};
    UiNineSlice skin{};
};

struct Spacer {
    Rectf bounds{};
    Vec2f size{};
};

struct IconButton {
    Id id{};
    Rectf bounds{};
    Sprite icon{};
    Vec2f icon_size{};
    std::string label;
    IconLabelPlacement label_placement = IconLabelPlacement::None;
    TextStyle text_style{};
    WidgetStyle style{};
    Color icon_tint = colors::white;
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    bool clicked = false;
};

struct MenuListItem {
    std::string id;
    std::string label;
    std::string submenu_id;
    Sprite icon{};
    bool enabled = true;
    bool checked = false;
    bool separator = false;
    bool danger = false;
    bool default_item = false;
    std::string prompt;
    std::string tooltip;
};

struct MenuList {
    Id id{};
    Rectf bounds{};
    std::vector<MenuListItem> items;
    i32 selected = 0;
    f32 row_height = 26.0f;
    f32 row_spacing = 2.0f;
    bool wrap = true;
    bool scrollable = false;
    bool show_scrollbar = true;
    f32 scroll_offset = 0.0f;
    f32 wheel_step = 48.0f;
    f32 scrollbar_thickness = 8.0f;
    bool enabled = true;
    bool keys_without_focus = true; // a game's menu: the keys while nothing else has focus too
    bool popup_keys = false;        // set by popup_menu(): the topmost open popup's menu takes the keys
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 activated = -1;
    std::string activated_id;
    i32 hovered = -1;
    std::string hovered_id;
    Rectf hovered_rect{};
    bool submenu_requested = false;
    std::string submenu_id;
    Rectf submenu_anchor{};
    f32 content_height = 0.0f;
    f32 viewport_height = 0.0f;
    bool scroll_changed = false;
    bool changed = false;
    bool cancelled = false;
};

struct MenuBarItem {
    std::string id;
    std::string label;
    Id menu_id{};
    bool enabled = true;
};

struct MenuBar {
    Id id{};
    Rectf bounds{};
    std::vector<MenuBarItem> items;
    i32 selected = -1; // the title the keys are on (-1: none: idle)
    bool keys_without_focus = false; // the keys only while it has focus (a click or Tab gives it); true: also while nothing has
    Vec2f pointer{};   // where the pointer was last frame: a title it moves onto switches an open menu
    f32 item_gap = 2.0f;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 opened = -1;
    std::string opened_id;
    Id opened_menu{};
    bool changed = false;
    bool cancelled = false;
};

struct TabBarItem {
    std::string id;
    std::string label;
    Sprite icon{};
    bool dirty = false;
    bool closable = false;
    bool enabled = true;
};

struct TabBar {
    Id id{};
    Rectf bounds{};
    std::vector<TabBarItem> items;
    i32 selected = 0;
    f32 offset = 0.0f;
    f32 tab_height = 28.0f;
    f32 min_tab_width = 72.0f;
    f32 max_tab_width = 180.0f;
    f32 gap = 2.0f;
    bool wrap = false;
    bool enabled = true;
    bool keys_without_focus = false; // the keys only while it has focus (a click or Tab gives it); true: also while nothing has
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 activated = -1;
    std::string activated_id;
    i32 closed = -1;
    std::string closed_id;
    bool changed = false;
};

struct TextRun {
    std::string text;
    TextStyle style{};
    std::string id;
    bool underline = false;
    bool strike = false;
    bool link = false;
};

struct IconRun {
    Sprite sprite{};
    Vec2f size{16.0f, 16.0f};
    Color tint = colors::white;
    std::string id;
    bool link = false;
    UiAlign vertical = UiAlign::Center;
};

using AdvancedTextRun = std::variant<TextRun, IconRun>;

struct AdvancedTextContent {
    std::vector<AdvancedTextRun> runs;
};

enum class AdvancedTextOverflow {
    Visible,
    Clip,
};

struct AdvancedTextLayoutOptions {
    f32 max_width = 0.0f;
    f32 line_spacing = 2.0f;
    i32 max_lines = -1;
    bool wrap = true;
    bool break_long_words = false;
    UiPadding padding{};
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Start;
    AdvancedTextOverflow overflow = AdvancedTextOverflow::Visible;
};

// Markup: [b]bold[/b], [i]italic[/i], [u]underlined[/u], [s]struck[/s],
// [color=ff8800]...[/color], [scale=1.5]...[/scale], [font=name]...[/font],
// [link=id]...[/link], [icon=name]; [[ is a [. A tag without its closing one
// stays as text.
struct AdvancedTextMarkupOptions {
    TextStyle base_style{};
    std::function<Sprite(std::string_view)> icon_resolver;
    std::function<Font(std::string_view)> font_resolver;
    // The fonts [b] and [i] draw in; without one, the text inside keeps its font.
    Font bold_font{};
    Font italic_font{};
};

struct AdvancedTextBox {
    Rectf rect{};
    std::string id;
    bool link = false;
    bool icon = false;
};

struct AdvancedTextRenderBox {
    Rectf rect{};
    std::string text;
    TextStyle style{};
    Sprite sprite{};
    Color tint = colors::white;
    std::string id;
    bool underline = false;
    bool strike = false;
    bool link = false;
    bool icon = false;
    UiAlign vertical = UiAlign::Center;
};

struct AdvancedTextLine {
    Rectf rect{};
    std::vector<std::size_t> boxes;
};

struct AdvancedTextLayout {
    Vec2f measured{};
    std::vector<AdvancedTextLine> lines;
    std::vector<AdvancedTextRenderBox> boxes;
    std::vector<AdvancedTextBox> hits;
};

struct AdvancedText {
    Id id{};
    Rectf bounds{};
    AdvancedTextContent content{};
    std::string markup;
    AdvancedTextMarkupOptions markup_options{};
    AdvancedTextLayoutOptions layout_options{};
    i32 z = 0;
    std::string hovered_id;
    std::string clicked_id;
    Rectf hovered_rect{};
    Rectf clicked_rect{};
    AdvancedTextLayout layout{};
    bool cache_enabled = true;
    mutable bool parsed_cache_valid = false;
    mutable std::string cached_markup;
    mutable AdvancedTextContent cached_content{};
    mutable bool layout_cache_valid = false;
    mutable std::string cached_layout_markup;
    mutable AdvancedTextLayoutOptions cached_layout_options{};
    mutable AdvancedTextLayout cached_layout{};
};

struct ScrollView {
    Id id{};
    Rectf bounds{};
    f32 content_height = 0.0f;
    f32 offset = 0.0f;
    f32 wheel_step = 48.0f;
    f32 scrollbar_thickness = 10.0f;
    ScrollState scroll{};
    Vec2f content_size{};
    Vec2f wheel_step2{48.0f, 48.0f};
    f32 min_thumb = 18.0f;
    bool draw_background = true;
    bool show_scrollbar = true;
    bool horizontal = false;
    bool enabled = true;
    Color background = Color::rgba(7, 9, 14, 208);
    Color border = Color::rgb(78, 88, 110);
    Color scrollbar_track = Color::rgb(24, 28, 38);
    Color scrollbar_thumb = Color::rgb(128, 226, 160);
    Rectf viewport{};
    Rectf content{};
    Rectf track{};
    Rectf thumb{};
    Rectf h_track{};
    Rectf h_thumb{};
    f32 v_grab_offset = 0.0f;
    f32 h_grab_offset = 0.0f;
    bool dragging = false;
    bool changed = false;
};

struct RichTextSpan {
    std::string text;
    Color color = Color::rgb(232, 245, 236);
};

struct RichTextLine {
    Rectf bounds{};
    std::vector<RichTextSpan> spans;
    TextStyle text_style{};
};

struct TextArea {
    Rectf bounds{};
    std::string text;
    f32 offset = 0.0f;
    f32 line_spacing = 2.0f;
    f32 wheel_step = 48.0f;
    bool draw_background = true;
    TextStyle text_style{};
    WidgetStyle style{};
};

struct ToastItem {
    std::string text;
    Sprite icon{};
    Color color = colors::white;
    f32 age = 0.0f;
    f32 lifetime = 3.0f;
};

struct ToastStack {
    Rectf bounds{};
    std::vector<ToastItem> items;
    Vec2f item_size{240.0f, 40.0f};
    Vec2f icon_size{18.0f, 18.0f};
    f32 spacing = 6.0f;
    UiPadding padding{8.0f, 8.0f, 8.0f, 8.0f};
    UiAnchor anchor = UiAnchor::BottomRight;
    i32 max_visible = 4;
    bool fade = true;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 visible = 0;
};

struct AnimatedValue {
    Rectf bounds{};
    f32 value = 0.0f;
    f32 display = 0.0f;
    f32 speed = 240.0f;
    std::string prefix;
    std::string suffix;
    i32 decimals = 0;
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Center;
    TextStyle text_style{};
    std::string rendered_text;
    bool changed = false;
};

struct TextInput {
    Id id{};
    Rectf bounds{};
    UiTextInputState state{};
    TextStyle text_style{};
    WidgetStyle style{};
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    UiTextInputResult result{};
};

// Editing state of a TextEdit. Keep one per editor across frames: in the TextEdit
// itself, or in the Context via Context::text_edit_state(). Callers may read and
// change `text` (e.g. clear a chat box after sending); the rest is kept by the widget.
struct UiTextEditState {
    std::string text;              // UTF-8
    std::size_t caret = 0;         // byte offset, always at the start of a character
    std::size_t selection_anchor = UiTextInputState::unset_selection;
    f32 preferred_x = -1.0f;       // column kept by Up/Down; -1 = the caret's own
    Vec2f scroll{};                // px
    bool active = false;

    bool has_selection() const { return selection_anchor != UiTextInputState::unset_selection && selection_anchor != caret; }
    std::size_t selection_start() const { return !has_selection() ? caret : (selection_anchor < caret ? selection_anchor : caret); }
    std::size_t selection_end() const { return !has_selection() ? caret : (selection_anchor < caret ? caret : selection_anchor); }
    void clear_selection() { selection_anchor = caret; }

    // Kept by the widget between frames.
    struct Snapshot {
        std::string text;
        std::size_t caret = 0;
        std::size_t selection_anchor = 0;
    };
    std::vector<Snapshot> undo_steps; // oldest first, at most TextEdit::undo_limit
    std::vector<Snapshot> redo_steps;
    u8 last_edit = 0;                 // kind of the last edit, for grouping undo steps
    std::size_t last_edit_caret = 0;
    f64 last_click_time = -1.0e9; // Context::now() of the last press (seconds)
    Vec2f last_click_position{};
    i32 click_count = 0;
    f64 caret_time = 0.0;             // the caret last moved (Context::now()): its blink restarts
    bool scrollbar = false;
    f32 scrollbar_grab = 0.0f;
    struct Layout {
        std::vector<TextRange> lines;  // displayed lines
        std::size_t text_hash = 0;
        std::size_t text_size = 0;
        f32 width = -1.0f;
        const void* font = nullptr;
        f32 scale = 0.0f;
        bool wrap = true;
        f32 widest = 0.0f;             // widest line, for sideways scrolling without wrap
    } layout;
};

// Which key sends a TextEdit's text (sets result.submitted, inserts nothing).
enum class UiSubmitKey : u8 {
    None,      // Enter starts a new line; nothing submits
    Enter,     // Enter submits; Shift+Enter starts a new line
    CtrlEnter, // Ctrl+Enter submits; Enter starts a new line
};

enum class UiTabKey : u8 {
    LeaveField,   // stop editing and report result.tab_direction (+1, or -1 with Shift)
    InsertSpaces, // insert TextEdit::tab_spaces spaces
};

struct UiTextEditResult {
    UiButtonState state = UiButtonState::Normal;
    bool changed = false;      // the text changed this frame
    bool submitted = false;    // the submit key was pressed
    bool cancelled = false;    // Escape: editing stopped
    i32 tab_direction = 0;     // UiTabKey::LeaveField: +1 Tab, -1 Shift+Tab
    f32 wanted_height = 0.0f;  // the height that fits the text, padding included
};

// A multi-line text editor: wraps, scrolls, selects with keyboard and mouse,
// copies and pastes, and undoes. With read_only it is selectable, copyable text.
// Sized for chat messages and notes (a few KB), not for code.
struct TextEdit {
    static constexpr std::size_t undo_limit = 200;

    Id id{};
    Rectf bounds{};
    UiTextEditState state{};
    TextStyle text_style{};
    WidgetStyle style{};
    std::string_view placeholder{};
    bool read_only = false;       // select and copy only
    bool wrap = true;             // soft-wrap at the width; otherwise scroll sideways
    f32 line_spacing = 2.0f;
    std::size_t max_bytes = 0;    // 0: no limit
    UiSubmitKey submit = UiSubmitKey::None;
    UiTabKey tab = UiTabKey::LeaveField;
    i32 tab_spaces = 4;
    bool auto_grow = false;       // bounds.h follows the text (at least one line)...
    f32 max_height = 0.0f;        // ...up to this (0: no cap), then it scrolls
    f32 wheel_step = 48.0f;
    f32 scrollbar_thickness = 8.0f;
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    UiTextEditResult result{};
};

// A number typed in: it shows `value` unless typed in; what was typed is the
// value (clamped) once it is left (Enter, Tab, a press elsewhere), and Esc
// shows the value again. Up and Down step it by `step`.
struct NumberInput {
    Id id{};
    Rectf bounds{};
    UiTextInputState text{};
    f32 value = 0.0f;
    f32 min = -std::numeric_limits<f32>::max();
    f32 max = std::numeric_limits<f32>::max();
    f32 step = 1.0f;
    TextStyle text_style{};
    WidgetStyle style{};
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    bool changed = false; // `value` changed this frame (left, or stepped)
    bool invalid = false; // what is typed is not a number in range
};

struct ComboBox {
    Id id{};
    Rectf bounds{};
    std::vector<std::string> items;
    UiComboState state{};
    TextStyle text_style{};
    WidgetStyle style{};
    bool draw_popup = true;
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    UiComboResult result{};
};

enum class ColorPickerMode {
    Rgba,
    Hsv,
    Suggested,
};

struct ColorPicker {
    Id id{};
    Rectf bounds{};
    Color value = colors::white;
    f32 row_height = 20.0f;
    f32 row_spacing = 4.0f;
    f32 label_width = 16.0f;
    f32 swatch_width = 42.0f;
    TextStyle text_style{};
    WidgetStyle style{};
    bool show_alpha = true;
    bool enabled = true;
    i32 z = 0;
    ColorPickerMode mode = ColorPickerMode::Hsv;
    f32 tab_height = 26.0f;
    f32 tab_gap = 4.0f;
    f32 picker_size = 132.0f;
    f32 hue_height = 18.0f;
    f32 swatch_size = 24.0f;
    f32 swatch_gap = 6.0f;
    i32 suggested_columns = 6;
    std::vector<Color> suggested;
    bool changed = false;
};

struct IconGridItem {
    Sprite icon{};
    Vec2f icon_size{};
    std::string text;
    i32 count = 0;
    std::string tooltip;
    bool enabled = true;
    bool locked = false;
};

struct IconGrid {
    Id id{};
    Rectf bounds{};
    std::vector<IconGridItem> items;
    Vec2f cell_size{48.0f, 48.0f};
    Vec2f spacing{4.0f, 4.0f};
    i32 columns = 4;
    i32 selected = 0;
    f32 offset = 0.0f;
    f32 wheel_step = 48.0f;
    bool wrap = true;
    bool enabled = true;
    bool keys_without_focus = false; // the keys only while it has focus (a click or Tab gives it); true: also while nothing has
    TextStyle text_style{};
    WidgetStyle style{};
    Color icon_tint = colors::white;
    i32 z = 0;
    bool drag_enabled = false;
    bool drop_enabled = false;
    std::string drag_payload_type = "icon_grid_cell";
    std::string drop_accept_type = "icon_grid_cell";
    i32 first = 0;
    i32 visible = 0;
    i32 clicked_index = -1;
    i32 activated = -1;
    i32 drag_source = -1;
    i32 dragging = -1;
    i32 drop_target = -1;
    i32 dropped_source = -1;
    i32 dropped_target = -1;
    bool drag_started = false;
    bool dropped = false;
    bool changed = false;
};

struct ListView {
    Id id{};
    Rectf bounds{};
    std::vector<std::string> items;
    i32 selected = 0;
    f32 offset = 0.0f;
    f32 row_height = 24.0f;
    f32 row_spacing = 2.0f;
    f32 wheel_step = 48.0f;
    bool wrap = true;
    bool enabled = true;
    bool keys_without_focus = false; // the keys only while it has focus (a click or Tab gives it); true: also while nothing has
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 first = 0;
    i32 visible = 0;
    bool drag_enabled = false;
    bool drop_enabled = false;
    std::string drag_payload_type = "list_row";
    std::string drop_accept_type = "list_row";
    i32 drag_source = -1;
    i32 dragging = -1;
    i32 drop_target = -1;
    i32 dropped_source = -1;
    i32 dropped_target = -1;
    bool drag_started = false;
    bool dropped = false;
    bool changed = false;
    bool activated = false;
};

struct Table {
    Rectf bounds{};
    std::vector<UiTableColumn> columns;
    std::vector<std::vector<std::string>> rows;
    f32 offset = 0.0f;
    f32 header_height = 24.0f;
    f32 row_height = 22.0f;
    f32 cell_padding_x = 6.0f;
    bool draw_header = true;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 first = 0;
    i32 visible = 0;
};

struct TreeView {
    Id id{};
    Rectf bounds{};
    std::vector<UiTreeItem> items;
    i32 selected = 0;
    f32 offset = 0.0f;
    f32 row_height = 24.0f;
    f32 row_spacing = 1.0f;
    f32 indent = 16.0f;
    f32 disclosure_width = 16.0f;
    bool wrap = true;
    bool enabled = true;
    bool keys_without_focus = false; // the keys only while it has focus (a click or Tab gives it); true: also while nothing has
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 first = 0;
    i32 visible = 0;
    i32 toggled = -1;
    i32 activated = -1;
    bool drag_enabled = false;
    bool drop_enabled = false;
    std::string drag_payload_type = "tree_item";
    std::string drop_accept_type = "tree_item";
    i32 drag_source = -1;
    i32 dragging = -1;
    i32 drop_target = -1;
    i32 dropped_source = -1;
    i32 dropped_target = -1;
    bool drag_started = false;
    bool dropped = false;
    bool changed = false;
};

// Intrinsic content size (the "Fit" size). Pure functions — testable without a renderer.
Vec2f measure(const Panel& widget);
Vec2f measure(const Label& widget);
Vec2f measure(const WrappedText& widget);
Vec2f measure(const Separator& widget);
Vec2f measure(const Button& widget);
Vec2f measure(const Toggle& widget);
Vec2f measure(const Slider& widget);
Vec2f measure(const ProgressBar& widget);
Vec2f measure(const Image& widget);
Vec2f measure(const NineSlicePanel& widget);
Vec2f measure(const Spacer& widget);
Vec2f measure(const IconButton& widget);
Vec2f measure(const MenuList& widget);
Vec2f measure(const MenuBar& widget);
Vec2f measure(const TabBar& widget);
Vec2f measure(const ScrollView& widget);
Vec2f measure(const RichTextLine& widget);
Vec2f measure(const TextArea& widget);
Vec2f measure(const AdvancedText& widget);
Vec2f measure(const ToastStack& widget);
Vec2f measure(const AnimatedValue& widget);
Vec2f measure(const TextInput& widget);
Vec2f measure(const NumberInput& widget);
Vec2f measure(const ComboBox& widget);
Vec2f measure(const ColorPicker& widget);
Vec2f measure(const IconGrid& widget);
Vec2f measure(const ListView& widget);
Vec2f measure(const Table& widget);
Vec2f measure(const TreeView& widget);

// Interaction + semantics + draw. Single source of truth shared by both front-ends.
void run(Context& ctx, Panel& widget);
void run(Context& ctx, Label& widget);
void run(Context& ctx, WrappedText& widget);
void run(Context& ctx, Separator& widget);
void run(Context& ctx, Button& widget);
void run(Context& ctx, Toggle& widget);
void run(Context& ctx, Slider& widget);
void run(Context& ctx, ProgressBar& widget);
void run(Context& ctx, Image& widget);
void run(Context& ctx, NineSlicePanel& widget);
void run(Context& ctx, Spacer& widget);
void run(Context& ctx, IconButton& widget);
void run(Context& ctx, MenuList& widget);
void run(Context& ctx, MenuBar& widget);
void run(Context& ctx, TabBar& widget);
void run(Context& ctx, ScrollView& widget);
void run(Context& ctx, RichTextLine& widget);
void run(Context& ctx, TextArea& widget);
void run(Context& ctx, AdvancedText& widget);
void run(Context& ctx, ToastStack& widget);
void run(Context& ctx, AnimatedValue& widget);
void run(Context& ctx, TextInput& widget);
void run(Context& ctx, TextEdit& widget);
// Edits `state` instead of widget.state, e.g. one kept by Context::text_edit_state().
void run(Context& ctx, TextEdit& widget, UiTextEditState& state);
void run(Context& ctx, NumberInput& widget);
void run(Context& ctx, ComboBox& widget);
void run(Context& ctx, ColorPicker& widget);
void run(Context& ctx, IconGrid& widget);
void run(Context& ctx, ListView& widget);
void run(Context& ctx, Table& widget);
void run(Context& ctx, TreeView& widget);

struct ContextMenuResult {
    bool open = false;
    bool opened = false;
    bool closed = false;
    bool outside_clicked = false;
    i32 activated = -1;
    std::string activated_id;
    i32 hovered = -1;
    std::string hovered_id;
    Rectf submenu_anchor{};
    bool submenu_requested = false;
    std::string submenu_id;
};

struct PopupMenuOptions {
    Vec2f size{};
    Vec2f offset{0.0f, 4.0f};
    UiAnchor anchor = UiAnchor::BottomLeft;
    Rectf screen{};
    i32 z = 10000;
    bool close_on_outside_click = true;
    bool flip_x = true;
    bool flip_y = true;
    bool match_anchor_width = false;
};

ContextMenuResult popup_menu(Context& ctx, Id id, Rectf anchor, MenuList& menu, PopupMenuOptions options = {});
ContextMenuResult context_menu(Context& ctx, Id id, Rectf anchor, MenuList& menu);
ContextMenuResult submenu(Context& ctx, Id parent, Id id, Rectf anchor, MenuList& menu);
Vec2f max_scroll_offset(const ScrollState& state);
bool clamp_scroll(ScrollState& state);
bool scroll_by(ScrollState& state, Vec2f delta);
bool ensure_visible(ScrollState& state, Rectf item);
i32 first_visible_index(f32 offset, f32 step);
ScrollbarLayout layout_scrollbar(Rectf track, const ScrollState& state, ScrollAxis axis, ScrollOptions options = {});
ScrollResult scroll_region(Context& ctx,
                           Id id,
                           Rectf viewport,
                           ScrollState state,
                           ScrollOptions options = {},
                           i32 z = 0,
                           f32* vertical_grab = nullptr,
                           f32* horizontal_grab = nullptr);
AdvancedTextContent parse_advanced_text(std::string_view markup, AdvancedTextMarkupOptions options = {});
AdvancedTextLayout layout_advanced_text(const AdvancedTextContent& content, AdvancedTextLayoutOptions options = {});
Vec2f measure_advanced_text(const AdvancedTextContent& content, AdvancedTextLayoutOptions options = {});
void draw_advanced_text(Context& ctx, const AdvancedTextLayout& layout, Rectf bounds, AdvancedTextLayoutOptions options = {});
bool step_animated_value(AnimatedValue& widget, f32 dt);

struct Theme;

// A widget as the theme makes it look: its styles (those at their defaults,
// unless `inherit` is false) merged with the theme's. Its run does this for
// itself; Context::measured() does it to measure it as it is laid out.
// (apply_theme, in theme.hpp, is another thing: a theme's sizes, set once.)
void theme_styles(const Theme& theme, IconGrid& widget);
void theme_styles(const Theme& theme, ListView& widget);
void theme_styles(const Theme& theme, Table& widget);
void theme_styles(const Theme& theme, TreeView& widget);
void theme_styles(const Theme& theme, WrappedText& widget);
void theme_styles(const Theme& theme, Button& widget);
void theme_styles(const Theme& theme, Toggle& widget);
void theme_styles(const Theme& theme, Slider& widget);
void theme_styles(const Theme& theme, IconButton& widget);
void theme_styles(const Theme& theme, RichTextLine& widget);
void theme_styles(const Theme& theme, TextArea& widget);
void theme_styles(const Theme& theme, ToastStack& widget);
void theme_styles(const Theme& theme, AnimatedValue& widget);
void theme_styles(const Theme& theme, TextInput& widget);
void theme_styles(const Theme& theme, NumberInput& widget);
void theme_styles(const Theme& theme, ComboBox& widget);
void theme_styles(const Theme& theme, ColorPicker& widget);
void theme_styles(const Theme& theme, MenuList& widget);
void theme_styles(const Theme& theme, MenuBar& widget);
void theme_styles(const Theme& theme, TabBar& widget);

// A widget's styles, put back as they were when the scope ends: what a run
// (or a measure) themes for itself is not left in the caller's widget, so the
// next frame themes the same style again (and a new theme shows).
template <typename W>
class KeepStyles {
public:
    explicit KeepStyles(W& widget) : _widget(widget) {
        if constexpr (requires { requires std::same_as<decltype(widget.style), WidgetStyle>; }) {
            _style = widget.style;
        }
        if constexpr (requires { requires std::same_as<decltype(widget.text_style), TextStyle>; }) {
            _text_style = widget.text_style;
        }
        if constexpr (requires { requires std::same_as<decltype(widget.label_style), TextStyle>; }) {
            _label_style = widget.label_style;
        }
        if constexpr (requires { requires std::same_as<decltype(widget.value_style), TextStyle>; }) {
            _value_style = widget.value_style;
        }
    }
    ~KeepStyles() {
        if constexpr (requires { requires std::same_as<decltype(_widget.style), WidgetStyle>; }) {
            _widget.style = std::move(*_style);
        }
        if constexpr (requires { requires std::same_as<decltype(_widget.text_style), TextStyle>; }) {
            _widget.text_style = std::move(*_text_style);
        }
        if constexpr (requires { requires std::same_as<decltype(_widget.label_style), TextStyle>; }) {
            _widget.label_style = std::move(*_label_style);
        }
        if constexpr (requires { requires std::same_as<decltype(_widget.value_style), TextStyle>; }) {
            _widget.value_style = std::move(*_value_style);
        }
    }
    KeepStyles(const KeepStyles&) = delete;
    KeepStyles& operator=(const KeepStyles&) = delete;

private:
    W& _widget;
    std::optional<WidgetStyle> _style;
    std::optional<TextStyle> _text_style;
    std::optional<TextStyle> _label_style;
    std::optional<TextStyle> _value_style;
};

} // namespace kin::ui2

// The editors' and the game screen's widgets (each header brings this one).
#include <kin/ui2/editor.hpp>
#include <kin/ui2/hud.hpp>
