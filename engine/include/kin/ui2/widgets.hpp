#pragma once

#include <kin/core/types.hpp>
#include <kin/dialogue/dialogue.hpp>
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

enum class TextOverflow {
    Visible,
    Clip,
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

struct IconSlot {
    Id id{};
    Rectf bounds{};
    Sprite icon{};
    Vec2f icon_size{};
    Vec2f size{};
    std::string text;
    i32 count = 0;
    std::string tooltip;
    TextStyle text_style{};
    WidgetStyle style{};
    Color icon_tint = colors::white;
    bool enabled = true;
    bool selected = false;
    bool locked = false;
    i32 z = 0;
    bool drag_enabled = false;
    bool drop_enabled = false;
    std::string drag_payload_type = "icon_slot";
    std::string drag_payload_text;
    u64 drag_payload_value = 0;
    std::string drop_accept_type = "icon_slot";
    Interaction interaction{};
    bool clicked = false;
    bool drag_started = false;
    bool dragging = false;
    bool dropped = false;
    std::string dropped_text;
    u64 dropped_value = 0;
};

struct Meter {
    Rectf bounds{};
    f32 value = 0.0f;
    f32 max = 1.0f;
    i32 segments = 5;
    f32 gap = 2.0f;
    Color fill = Color::rgb(128, 226, 160);
    Color empty = Color::rgb(24, 28, 38);
    Color border = colors::transparent;
};

enum class PromptBindingMode {
    First,
    All,
};

struct PromptOptions {
    PromptBindingMode mode = PromptBindingMode::First;
    std::string separator = " / ";
    bool compact = true;
    bool brackets = false;
    std::string fallback;
};

struct PromptLabel {
    Id id{};
    Rectf bounds{};
    std::string action;
    std::string prompt;
    std::string text;
    PromptOptions prompt_options{};
    Sprite icon{};
    Vec2f icon_size{};
    TextStyle prompt_style{};
    TextStyle text_style{};
    UiPadding padding{8.0f, 4.0f, 8.0f, 4.0f};
    UiPadding chip_padding{8.0f, 4.0f, 8.0f, 4.0f};
    f32 gap = 8.0f;
    std::string separator = " ";
    Color chip_fill = Color::rgba(24, 28, 38, 235);
    Color chip_border = Color::rgb(78, 88, 110);
    bool show_chip = true;
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Center;
};

struct PromptRowItem {
    std::string action;
    std::string prompt;
    std::string text;
    Sprite icon{};
    bool enabled = true;
};

struct PromptRow {
    Id id{};
    Rectf bounds{};
    std::vector<PromptRowItem> items;
    PromptOptions prompt_options{};
    UiLayoutAxis axis = UiLayoutAxis::Horizontal;
    f32 spacing = 12.0f;
    PromptLabel item_style{};
    TextStyle prompt_style{};
    TextStyle text_style{};
    UiPadding padding{8.0f, 4.0f, 8.0f, 4.0f};
    UiAlign horizontal = UiAlign::Start;
    UiAlign vertical = UiAlign::Center;
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
    i32 selected = 0;
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
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 activated = -1;
    std::string activated_id;
    i32 closed = -1;
    std::string closed_id;
    bool changed = false;
};

struct Splitter {
    Id id{};
    Rectf bounds{};
    UiLayoutAxis axis = UiLayoutAxis::Horizontal;
    f32 ratio = 0.5f;
    f32 min_first = 80.0f;
    f32 min_second = 80.0f;
    f32 thickness = 6.0f;
    bool enabled = true;
    WidgetStyle style{};
    i32 z = 0;
    Rectf first{};
    Rectf second{};
    Rectf handle{};
    bool dragging = false;
    bool changed = false;
};

struct DockPanel {
    Id id{};
    Rectf bounds{};
    std::string title;
    Sprite icon{};
    bool active = false;
    bool collapsed = false;
    bool closable = true;
    f32 header_height = 28.0f;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    Rectf header{};
    Rectf body{};
    bool toggled = false;
    bool close_requested = false;
};

struct BreadcrumbSegment {
    std::string id;
    std::string label;
    Sprite icon{};
    bool enabled = true;
};

struct BreadcrumbBar {
    Id id{};
    Rectf bounds{};
    std::vector<BreadcrumbSegment> segments;
    Sprite home_icon{};
    f32 gap = 4.0f;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 activated = -1;
    std::string activated_id;
    bool overflowed = false;
};

enum class AssetBrowserMode {
    List,
    Grid,
};

enum class AssetSortField {
    None,
    Name,
    Kind,
    Modified,
};

struct AssetBrowserItem {
    std::string id;
    std::string name;
    std::string kind;
    Sprite icon{};
    std::string modified;
    bool selected = false;
    bool enabled = true;
};

struct AssetBrowser {
    Id id{};
    Rectf bounds{};
    std::vector<AssetBrowserItem> items;
    AssetBrowserMode mode = AssetBrowserMode::Grid;
    AssetSortField sort_field = AssetSortField::None;
    Vec2f cell_size{92.0f, 72.0f};
    f32 row_height = 28.0f;
    Vec2f spacing{6.0f, 6.0f};
    i32 columns = 0;
    f32 offset = 0.0f;
    f32 wheel_step = 48.0f;
    bool multi_select = true;
    bool drag_enabled = true;
    std::string drag_payload_type = "asset";
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 selected = -1;
    i32 activated = -1;
    std::string activated_id;
    i32 clicked = -1;
    std::string clicked_id;
    i32 range_anchor = -1;
    i32 drag_source = -1;
    bool drag_started = false;
    AssetSortField sort_requested = AssetSortField::None;
    bool selection_changed = false;
};

struct StatusBarItem {
    std::string id;
    std::string text;
    Sprite icon{};
    bool enabled = true;
};

struct StatusBar {
    Id id{};
    Rectf bounds{};
    std::vector<StatusBarItem> left;
    std::vector<StatusBarItem> right;
    f32 progress = -1.0f;
    i32 warnings = 0;
    i32 errors = 0;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    std::string clicked_id;
};

enum class LogSeverity {
    Info,
    Warning,
    Error,
};

struct LogEntry {
    std::string id;
    std::string timestamp;
    std::string text;
    LogSeverity severity = LogSeverity::Info;
};

struct LogConsole {
    Id id{};
    Rectf bounds{};
    std::vector<LogEntry> entries;
    std::string filter;
    i32 selected = -1;
    f32 offset = 0.0f;
    f32 row_height = 24.0f;
    f32 wheel_step = 48.0f;
    bool auto_scroll = true;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 activated = -1;
    std::string activated_id;
    bool copy_requested = false;
    bool clear_requested = false;
    i32 visible = 0;
};

enum class PropertyInspectorRowKind {
    Label,
    Bool,
    Number,
    Text,
    Combo,
    Color,
    Vector,
    Enum,
    Reference,
    Button,
    Separator,
    Section,
};

struct PropertyInspectorRow {
    PropertyInspectorRowKind kind = PropertyInspectorRowKind::Label;
    Id id{};
    std::string string_id;
    std::string label;
    std::string value_text;
    std::string tooltip;
    bool enabled = true;
    bool read_only = false;
    bool mixed = false;
    bool modified = false;
    std::string error;
    bool resettable = false;
    bool expanded = true;
    bool bool_value = false;
    f32 number_value = 0.0f;
    f32 number_min = -100000.0f;
    f32 number_max = 100000.0f;
    f32 number_step = 1.0f;
    std::string text_value;
    std::vector<std::string> options;
    i32 selected = 0;
    Color color_value = colors::white;
    std::vector<f32> vector_values;
    i32 vector_dimension = 2;
    Sprite reference_icon{};
    std::string reference_type = "reference";
    std::string drag_payload_type = "property";
    std::string drag_payload_text;
    u64 drag_payload_value = 0;
    Rectf row_rect{};
    Rectf control_rect{};
    bool clicked = false;
    bool changed = false;
    bool section_toggled = false;
};

struct PropertyInspector {
    Id id{};
    Rectf bounds{};
    std::vector<PropertyInspectorRow> rows;
    f32 offset = 0.0f;
    f32 row_height = 28.0f;
    f32 section_height = 30.0f;
    f32 row_spacing = 4.0f;
    f32 label_width = 128.0f;
    f32 label_gap = 8.0f;
    f32 control_height = 24.0f;
    f32 wheel_step = 48.0f;
    bool draw_background = true;
    bool enabled = true;
    TextStyle label_style{};
    TextStyle value_style{};
    WidgetStyle style{};
    i32 z = 0;
    i32 first = 0;
    i32 visible = 0;
    i32 changed_row = -1;
    std::string changed_id;
    std::string clicked_id;
    std::string toggled_section_id;
    std::string reset_requested_id;
    std::string action_requested_id;
    std::string reference_pick_requested_id;
    std::string drag_source_id;
    std::string drop_target_id;
    bool changed = false;
};

enum class NodeGraphPortKind {
    Input,
    Output,
};

struct NodeGraphPort {
    std::string id;
    std::string label;
    NodeGraphPortKind kind = NodeGraphPortKind::Input;
    std::string type;
    // Neutral by default (accent is reserved for selection); set per-port for
    // type-colored ports.
    Color color = Color::rgb(150, 162, 184);
    bool enabled = true;
    Rectf rect{};
};

struct NodeGraphNode {
    std::string id;
    std::string title;
    Vec2f position{};
    Vec2f size{140.0f, 96.0f};
    bool selected = false;
    bool enabled = true;
    Sprite icon{};
    Color color = Color::rgb(78, 88, 110);
    std::vector<NodeGraphPort> inputs;
    std::vector<NodeGraphPort> outputs;
    bool collapsed = false;
    Rectf rect{};
    Rectf header_rect{};
};

struct NodeGraphEdge {
    std::string id;
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
    bool selected = false;
    bool enabled = true;
    Color color = Color::rgb(150, 162, 184); // neutral; accent only when selected
    Rectf hit_rect{};
};

struct NodeGraph {
    Id id{};
    Rectf bounds{};
    std::vector<NodeGraphNode> nodes;
    std::vector<NodeGraphEdge> edges;
    Vec2f pan{};
    f32 zoom = 1.0f;
    f32 min_zoom = 0.25f;
    f32 max_zoom = 2.5f;
    f32 grid_size = 32.0f;
    bool show_grid = true;
    bool pan_background = true;
    Vec2f node_min_size{120.0f, 64.0f};
    Vec2f port_size{10.0f, 10.0f};
    f32 header_height = 26.0f;
    f32 port_spacing = 22.0f;
    TextStyle text_style{};
    WidgetStyle style{};
    i32 z = 0;
    std::string selected_node_id;
    std::string selected_edge_id;
    std::string active_drag_node_id;
    std::string active_drag_from_node_id;
    std::string active_drag_from_port_id;
    std::string clicked_node_id;
    std::string activated_node_id;
    std::string moved_node_id;
    Vec2f move_delta{};
    std::string clicked_port_node_id;
    std::string clicked_port_id;
    std::string connect_started_node_id;
    std::string connect_started_port_id;
    std::string connect_preview_node_id;
    std::string connect_preview_port_id;
    std::string connect_from_node_id;
    std::string connect_from_port_id;
    std::string connect_to_node_id;
    std::string connect_to_port_id;
    std::string disconnect_requested_edge_id;
    std::string context_menu_anchor_id;
    Vec2f drag_start_pointer{};
    Vec2f drag_start_node_position{};
    Vec2f pan_drag_start_pointer{};
    Vec2f pan_drag_start{};
    bool selection_changed = false;
    bool pan_changed = false;
    bool zoom_changed = false;
    bool connect_requested = false;
    bool connect_invalid = false;
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

struct AdvancedTextMarkupOptions {
    TextStyle base_style{};
    std::function<Sprite(std::string_view)> icon_resolver;
    std::function<Font(std::string_view)> font_resolver;
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

struct DialogBox {
    Rectf bounds{};
    UiNineSlice skin{};
    Color fill = Color::rgba(7, 9, 14, 230);
    Color border = Color::rgb(78, 88, 110);
    std::string title;
    std::string body;
    AdvancedTextContent title_content{};
    AdvancedTextContent body_content{};
    std::string title_markup;
    std::string body_markup;
    AdvancedTextMarkupOptions markup_options{};
    AdvancedTextLayoutOptions title_layout{};
    AdvancedTextLayoutOptions body_layout{};
    TextStyle title_style{.scale = 3.0f};
    TextStyle body_style{};
    UiPadding padding{12.0f, 8.0f, 12.0f, 8.0f};
    f32 gap = 8.0f;
};

struct DialogueView {
    Id id{};
    Rectf bounds{};
    DialogueViewModel model{};
    bool show_disabled_choices = false;
    bool show_history_button = true;
    bool show_close_button = true;
    f32 speaker_height = 26.0f;
    f32 choice_height = 28.0f;
    f32 choice_spacing = 4.0f;
    Vec2f portrait_size{64.0f, 64.0f};
    Sprite portrait{};
    PromptRow prompts{};
    AdvancedTextMarkupOptions markup_options{};
    TextStyle speaker_style{.scale = 2.0f};
    TextStyle body_style{};
    TextStyle choice_style{};
    WidgetStyle style{};
    UiPadding padding{12.0f, 8.0f, 12.0f, 8.0f};
    i32 z = 0;
    bool advance_requested = false;
    bool skip_requested = false;
    bool history_requested = false;
    bool closed_requested = false;
    bool choice_requested = false;
    i32 choice_index = -1;
    std::string choice_id;
};

struct Nameplate {
    Rectf bounds{};
    std::string label;
    f32 value = 1.0f;
    bool show_bar = true;
    TextStyle text_style{};
    Color fill = Color::rgba(7, 9, 14, 208);
    Color border = Color::rgb(78, 88, 110);
    Color bar_fill = Color::rgb(128, 226, 160);
    Color bar_back = Color::rgb(24, 28, 38);
};

struct SelectionRect {
    Vec2f start{};
    Vec2f end{};
    Color fill = Color::rgba(128, 226, 160, 48);
    Color border = Color::rgb(128, 226, 160);
    Rectf bounds{};
    bool active = false;
};

struct TargetReticle {
    Rectf bounds{};
    bool valid = true;
    bool selected = false;
    bool hovered = false;
    Color valid_color = Color::rgb(128, 226, 160);
    Color invalid_color = Color::rgb(230, 88, 80);
    f32 corner = 8.0f;
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

struct ResourceItem {
    Sprite icon{};
    std::string label;
    std::string value;
    Color color = colors::white;
};

struct ResourceRow {
    Rectf bounds{};
    std::vector<ResourceItem> items;
    Vec2f icon_size{16.0f, 16.0f};
    f32 gap = 6.0f;
    f32 item_gap = 14.0f;
    TextStyle text_style{};
    WidgetStyle style{};
};

struct LabeledBar {
    Rectf bounds{};
    std::string label;
    std::string value_text;
    f32 value = 0.0f;
    TextStyle text_style{};
    Color fill = Color::rgb(128, 226, 160);
    Color background = Color::rgb(24, 28, 38);
    Color border = Color::rgb(78, 88, 110);
};

struct IconMeter {
    Rectf bounds{};
    Sprite icon{};
    Sprite empty_icon{};
    i32 value = 0;
    i32 max = 3;
    Vec2f icon_size{16.0f, 16.0f};
    f32 gap = 4.0f;
    Color fill_tint = colors::white;
    Color empty_tint = Color::rgba(255, 255, 255, 96);
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

struct FloatingText {
    Rectf bounds{};
    Vec2f position{};
    Vec2f velocity{0.0f, -24.0f};
    std::string text;
    f32 age = 0.0f;
    f32 lifetime = 1.0f;
    bool active = true;
    bool fade = true;
    TextStyle text_style{};
    Vec2f rendered_position{};
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

struct NumberInput {
    Id id{};
    Rectf bounds{};
    UiTextInputState text{};
    f32 value = 0.0f;
    f32 min = 0.0f;
    f32 max = 1.0f;
    f32 step = 1.0f;
    TextStyle text_style{};
    WidgetStyle style{};
    bool enabled = true;
    i32 z = 0;
    Interaction interaction{};
    bool changed = false;
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
enum class PropertyRowKind {
    Label,
    Bool,
    Number,
    Text,
    Combo,
    Color,
};

struct PropertyGridRow {
    PropertyRowKind kind = PropertyRowKind::Label;
    Id id{};
    std::string label;
    std::string value_text;
    bool bool_value = false;
    Color color_value = colors::white;
    ColorPicker color_picker{};
    NumberInput number{};
    TextInput text{};
    ComboBox combo{};
    bool clicked = false;
    bool changed = false;
};

struct PropertyGrid {
    Id id{};
    Rectf bounds{};
    std::vector<PropertyGridRow> rows;
    f32 offset = 0.0f;
    f32 row_height = 28.0f;
    f32 row_spacing = 4.0f;
    f32 label_width = 120.0f;
    f32 label_gap = 8.0f;
    f32 control_height = 24.0f;
    f32 wheel_step = 48.0f;
    bool draw_background = true;
    bool enabled = true;
    i32 z = 0;
    TextStyle label_style{};
    TextStyle value_style{};
    WidgetStyle style{};
    i32 first = 0;
    i32 visible = 0;
    bool changed = false;
};

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
Vec2f measure(const IconSlot& widget);
Vec2f measure(const Meter& widget);
Vec2f measure(const PromptLabel& widget);
Vec2f measure(const PromptRow& widget);
Vec2f measure(const MenuList& widget);
Vec2f measure(const MenuBar& widget);
Vec2f measure(const TabBar& widget);
Vec2f measure(const Splitter& widget);
Vec2f measure(const DockPanel& widget);
Vec2f measure(const BreadcrumbBar& widget);
Vec2f measure(const AssetBrowser& widget);
Vec2f measure(const StatusBar& widget);
Vec2f measure(const LogConsole& widget);
Vec2f measure(const PropertyInspector& widget);
Vec2f measure(const NodeGraph& widget);
Vec2f measure(const DialogBox& widget);
Vec2f measure(const DialogueView& widget);
Vec2f measure(const Nameplate& widget);
Vec2f measure(const SelectionRect& widget);
Vec2f measure(const TargetReticle& widget);
Vec2f measure(const ScrollView& widget);
Vec2f measure(const ResourceRow& widget);
Vec2f measure(const LabeledBar& widget);
Vec2f measure(const IconMeter& widget);
Vec2f measure(const RichTextLine& widget);
Vec2f measure(const TextArea& widget);
Vec2f measure(const AdvancedText& widget);
Vec2f measure(const ToastStack& widget);
Vec2f measure(const FloatingText& widget);
Vec2f measure(const AnimatedValue& widget);
Vec2f measure(const TextInput& widget);
Vec2f measure(const NumberInput& widget);
Vec2f measure(const ComboBox& widget);
Vec2f measure(const ColorPicker& widget);
Vec2f measure(const IconGrid& widget);
Vec2f measure(const ListView& widget);
Vec2f measure(const Table& widget);
Vec2f measure(const TreeView& widget);
Vec2f measure(const PropertyGrid& widget);

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
void run(Context& ctx, IconSlot& widget);
void run(Context& ctx, Meter& widget);
void run(Context& ctx, PromptLabel& widget);
void run(Context& ctx, PromptRow& widget);
void run(Context& ctx, MenuList& widget);
void run(Context& ctx, MenuBar& widget);
void run(Context& ctx, TabBar& widget);
void run(Context& ctx, Splitter& widget);
void run(Context& ctx, DockPanel& widget);
void run(Context& ctx, BreadcrumbBar& widget);
void run(Context& ctx, AssetBrowser& widget);
void run(Context& ctx, StatusBar& widget);
void run(Context& ctx, LogConsole& widget);
void run(Context& ctx, PropertyInspector& widget);
void run(Context& ctx, NodeGraph& widget);
void run(Context& ctx, DialogBox& widget);
void run(Context& ctx, DialogueView& widget);
void run(Context& ctx, Nameplate& widget);
void run(Context& ctx, SelectionRect& widget);
void run(Context& ctx, TargetReticle& widget);
void run(Context& ctx, ScrollView& widget);
void run(Context& ctx, ResourceRow& widget);
void run(Context& ctx, LabeledBar& widget);
void run(Context& ctx, IconMeter& widget);
void run(Context& ctx, RichTextLine& widget);
void run(Context& ctx, TextArea& widget);
void run(Context& ctx, AdvancedText& widget);
void run(Context& ctx, ToastStack& widget);
void run(Context& ctx, FloatingText& widget);
void run(Context& ctx, AnimatedValue& widget);
void run(Context& ctx, TextInput& widget);
void run(Context& ctx, NumberInput& widget);
void run(Context& ctx, ComboBox& widget);
void run(Context& ctx, ColorPicker& widget);
void run(Context& ctx, IconGrid& widget);
void run(Context& ctx, ListView& widget);
void run(Context& ctx, Table& widget);
void run(Context& ctx, TreeView& widget);
void run(Context& ctx, PropertyGrid& widget);

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

} // namespace kin::ui2
