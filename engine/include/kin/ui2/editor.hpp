#pragma once

// Widgets for editors and tools: docks and splitters, breadcrumbs, an asset
// browser, a log console and a status bar, property inspectors and grids, and
// node graphs. widgets.hpp includes this; on its own it brings widgets.hpp.

#include <kin/ui2/widgets.hpp>

#include <string>
#include <vector>

namespace kin::ui2 {

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
    bool keys_without_focus = false; // the keys only while it has focus (a click or Tab gives it); true: also while nothing has
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

Vec2f measure(const Splitter& widget);
Vec2f measure(const DockPanel& widget);
Vec2f measure(const BreadcrumbBar& widget);
Vec2f measure(const AssetBrowser& widget);
Vec2f measure(const StatusBar& widget);
Vec2f measure(const LogConsole& widget);
Vec2f measure(const PropertyInspector& widget);
Vec2f measure(const NodeGraph& widget);
Vec2f measure(const PropertyGrid& widget);

void run(Context& ctx, Splitter& widget);
void run(Context& ctx, DockPanel& widget);
void run(Context& ctx, BreadcrumbBar& widget);
void run(Context& ctx, AssetBrowser& widget);
void run(Context& ctx, StatusBar& widget);
void run(Context& ctx, LogConsole& widget);
void run(Context& ctx, PropertyInspector& widget);
void run(Context& ctx, NodeGraph& widget);
void run(Context& ctx, PropertyGrid& widget);

// As the theme makes them look (see theme_styles in widgets.hpp).
void theme_styles(const Theme& theme, AssetBrowser& widget);
void theme_styles(const Theme& theme, StatusBar& widget);
void theme_styles(const Theme& theme, LogConsole& widget);
void theme_styles(const Theme& theme, DockPanel& widget);
void theme_styles(const Theme& theme, BreadcrumbBar& widget);
void theme_styles(const Theme& theme, PropertyInspector& widget);
void theme_styles(const Theme& theme, NodeGraph& widget);
void theme_styles(const Theme& theme, PropertyGrid& widget);

} // namespace kin::ui2
