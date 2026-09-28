#pragma once

// Private organizer header for UI2 widget implementation chunks.
// Public UI2 APIs remain declared in <kin/ui2/widgets.hpp>.

#include <kin/ui2/widgets.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kin::ui2 {

// Implemented by widgets_core.cpp. The helper implementation uses these
// operations without depending on another widget translation unit's include
// order.
AdvancedTextContent parse_advanced_text(std::string_view markup, AdvancedTextMarkupOptions options);
AdvancedTextLayout layout_advanced_text(const AdvancedTextContent& content, AdvancedTextLayoutOptions options);

enum class GlyphDir { Down, Right };
void draw_chevron(Context& ctx, Vec2f center, f32 half, GlyphDir dir, f32 thickness, Color color);
void draw_check(Context& ctx, Rectf box, f32 thickness, Color color);

struct CollectionFrame {
    Rectf content{};
    f32 inner_radius = 0.0f;
    f32 border = 0.0f;
};

CollectionFrame collection_background(Context& ctx, Rectf bounds, const SurfaceStyle& panel);
void collection_outline(Context& ctx, Rectf bounds, const SurfaceStyle& panel);
void draw_panel_body_fill(Context& ctx, Rectf body, f32 inner_radius, Color fill);

void run_combo_popup(Context& ctx, ComboBox& widget);

namespace detail {

f32 clamp_margin(f32 margin, f32 limit);
Sprite image_sprite(const Image& widget);
Vec2f explicit_or_sprite_size(Vec2f explicit_size, const Sprite& sprite);
Rectf fitted_rect(Rectf bounds, Vec2f size, ImageFit fit);
void draw_nine_slice(Context& ctx, const UiNineSlice& skin, Rectf bounds);
Vec2f icon_button_content_size(const IconButton& widget);

UiPadding intrinsic_padding(const WidgetStyle& style);

struct TextInputLayoutMetrics {
    UiPadding padding{};
    f32 text_height = 0.0f;
    f32 height = 0.0f;
};
TextInputLayoutMetrics text_input_layout_metrics(const TextStyle& text_style, const WidgetStyle& style);
Color frame_color(const WidgetStyle& style, const Interaction& it, bool enabled);
UiTextInputResult edit_text_input(Context& ctx,
                                  UiTextInputState& state,
                                  Rectf bounds,
                                  const TextStyle& text_style,
                                  const WidgetStyle& style,
                                  const Interaction& it,
                                  bool enabled);
void draw_text_input(Context& ctx,
                     Rectf bounds,
                     const UiTextInputState& state,
                     const TextStyle& text_style,
                     const WidgetStyle& style,
                     Color frame);
bool parse_f32(std::string_view text, f32& out);
std::string format_f32(f32 value);
std::string format_value(f32 value, i32 decimals);
Color alpha_scaled(Color color, f32 alpha);
f32 life_alpha(f32 age, f32 lifetime);
std::string format_color_hex(Color color);
u8 slider_to_channel(f32 value);
f32 color_picker_height(const ColorPicker& widget);

struct HsvColor {
    f32 h = 0.0f;
    f32 s = 0.0f;
    f32 v = 0.0f;
};
HsvColor rgb_to_hsv(Color color);
Color hsv_to_rgb(f32 h, f32 s, f32 v, u8 alpha = 255);

bool menu_item_selectable(const MenuListItem& item);
std::string menu_item_result_id(const MenuListItem& item);
i32 next_menu_index(const std::vector<MenuListItem>& items, i32 current, i32 delta, bool wrap);
i32 first_menu_index(const std::vector<MenuListItem>& items);
std::string item_result_id(std::string_view id, std::string_view fallback);

struct TabBarLayoutMetrics {
    UiPadding padding{};
    f32 tab_height = 0.0f;
    f32 icon_size = 0.0f;
    f32 close_size = 0.0f;
    f32 dirty_size = 0.0f;
    f32 item_gap = 0.0f;
};
TabBarLayoutMetrics tab_bar_layout_metrics(const TabBar& widget, const TextStyle& text_style, const WidgetStyle& style);
f32 tab_width(const TabBar& widget,
              const TabBarItem& item,
              const TextStyle& text_style,
              const TabBarLayoutMetrics& metrics);
i32 next_enabled_tab(const TabBar& widget, i32 current, i32 delta);

std::string lower_copy(std::string_view value);
bool asset_matches_filter(const LogEntry& entry, std::string_view filter);
Color severity_color(LogSeverity severity, const TextStyle& style, const Theme& theme);
Vec2f slot_icon_size(Vec2f explicit_size, const Sprite& icon, Rectf content);
void draw_slot_contents(Context& ctx,
                        Rectf bounds,
                        const Sprite& icon,
                        Vec2f icon_size,
                        std::string_view text,
                        i32 count,
                        const TextStyle& text_style,
                        const WidgetStyle& style,
                        Color icon_tint);
Rectf icon_grid_cell_rect(const IconGrid& widget, i32 index);

f32 color_float_slider(Context& ctx,
                       Id id,
                       Rectf row,
                       std::string_view label,
                       f32 value,
                       f32 min,
                       f32 max,
                       const ColorPicker& widget,
                       bool& changed);
u8 color_channel_slider(Context& ctx,
                        Id id,
                        Rectf row,
                        std::string_view label,
                        u8 value,
                        const ColorPicker& widget,
                        bool& changed);
std::vector<Color> color_picker_suggestions(const ColorPicker& widget, const Theme& theme);
const char* color_picker_mode_label(ColorPickerMode mode);

Id property_row_id(const PropertyGrid& widget, const PropertyGridRow& row, i32 index);
f32 property_grid_content_height(const PropertyGrid& widget);
std::string inspector_row_result_id(const PropertyInspectorRow& row, i32 index);
Id property_inspector_row_id(const PropertyInspector& widget, const PropertyInspectorRow& row, i32 index);

struct PropertyInspectorLayoutMetrics {
    f32 row_height = 0.0f;
    f32 section_height = 0.0f;
    f32 control_height = 0.0f;
    f32 label_width = 0.0f;
    f32 label_gap = 0.0f;
    UiPadding padding{};
};
f32 property_inspector_marker_gutter(const TextStyle& label_style, UiPadding padding);
PropertyInspectorLayoutMetrics property_inspector_layout_metrics(const PropertyInspector& widget);
f32 property_inspector_row_height(const PropertyInspector& widget,
                                  const PropertyInspectorLayoutMetrics& metrics,
                                  const PropertyInspectorRow& row);
f32 property_inspector_row_height(const PropertyInspector& widget, const PropertyInspectorRow& row);
f32 property_inspector_content_height(const PropertyInspector& widget,
                                      const PropertyInspectorLayoutMetrics& metrics);
f32 property_inspector_content_height(const PropertyInspector& widget);

Vec2f node_to_screen(const NodeGraph& graph, Vec2f point);
Vec2f screen_to_node(const NodeGraph& graph, Vec2f point);
Rectf node_rect_to_screen(const NodeGraph& graph, Rectf rect);
f32 length_sq(Vec2f value);
Vec2f normalize_or(Vec2f value, Vec2f fallback);
f32 point_segment_distance_sq(Vec2f point, Vec2f a, Vec2f b);
bool node_graph_ports_compatible(const NodeGraphPort& from, const NodeGraphPort& to);
Rectf normalized_rect(Vec2f a, Vec2f b);

f32 axis_value(Vec2f value, ScrollAxis axis);
f32 rect_axis_pos(Rectf rect, ScrollAxis axis);
f32 rect_axis_size(Rectf rect, ScrollAxis axis);
Rectf axis_thumb(Rectf track, ScrollAxis axis, f32 pos, f32 size);
bool set_axis_offset(ScrollState& state, ScrollAxis axis, f32 value);
f32 scroll_offset_for_thumb(Rectf track,
                            Rectf thumb,
                            ScrollAxis axis,
                            f32 pointer,
                            f32 grab,
                            const ScrollState& state);
ScrollResult scroll_axis_interaction(Context& ctx,
                                     Id id,
                                     ScrollAxis axis,
                                     Rectf viewport,
                                     Rectf track,
                                     ScrollState state,
                                     ScrollOptions options,
                                     i32 z,
                                     f32* grab_offset);

bool starts_with(std::string_view text, std::string_view prefix);
i32 hex_value(char ch);
bool parse_hex_color(std::string_view value, Color& out);
f32 parse_scale(std::string_view value, f32 fallback);
std::string closing_tag_for(std::string_view tag);

struct AdvancedTextParseState {
    TextStyle style{};
    bool underline = false;
    bool strike = false;
    bool link = false;
    std::string id;
};
bool apply_markup_tag(std::string_view tag,
                      AdvancedTextParseState& state,
                      const AdvancedTextMarkupOptions& options);
void push_text_run(AdvancedTextContent& content,
                   std::string text,
                   const AdvancedTextParseState& state);
f32 line_align_offset(f32 line_width, f32 content_width, UiAlign align);
AdvancedTextContent advanced_text_content(const AdvancedText& widget);
bool advanced_text_options_equal(const AdvancedTextLayoutOptions& lhs,
                                 const AdvancedTextLayoutOptions& rhs);
AdvancedTextLayout advanced_text_layout(const AdvancedText& widget, AdvancedTextLayoutOptions options);
bool has_advanced_content(const AdvancedTextContent& content);
AdvancedTextContent dialog_advanced_content(const AdvancedTextContent& content,
                                            std::string_view markup,
                                            AdvancedTextMarkupOptions options,
                                            TextStyle fallback_style,
                                            std::string_view fallback_text);

} // namespace detail

} // namespace kin::ui2
