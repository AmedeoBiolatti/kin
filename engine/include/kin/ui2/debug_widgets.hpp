#pragma once

#include <kin/ecs/ui2.hpp>
#include <kin/ui2/theme.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kin::ui2 {

struct DebugWidgetStyle {
    f32 row_height = 26.0f;
    f32 section_height = 28.0f;
    f32 label_width = 132.0f;
    f32 control_width = 168.0f;
    f32 value_width = 72.0f;
    f32 metric_width = 112.0f;
    f32 gap = 8.0f;
    UiPadding panel_padding{10.0f, 8.0f, 10.0f, 8.0f};
    TextStyle title_text{};
    TextStyle label_text{};
    TextStyle value_text{};
    WidgetStyle panel{};
    WidgetStyle row{};
    WidgetStyle control{};
};

struct DebugInspectorPanel {
    std::string title;
    bool scrollable = true;
    bool draw_surface = true;
    f32 wheel_step = 56.0f;
};

struct DebugMetric {
    std::string label;
    std::string value;
};

struct DebugMetricRow {
    std::vector<DebugMetric> metrics;
    f32 metric_width = 0.0f;
};

struct DebugProperty {
    std::string label;
    std::string value;
    bool modified = false;
};

struct DebugPropertyGroup {
    std::string title;
    std::vector<DebugProperty> rows;
};

struct DebugToggleRow {
    std::string label;
    bool value = false;
    bool enabled = true;
};

struct DebugEnumRow {
    std::string label;
    std::vector<std::string> options;
    i32 selected = 0;
    bool enabled = true;
};

struct DebugNumericSliderRow {
    std::string label;
    std::string value_text;
    f32 value = 0.0f;
    f32 min = 0.0f;
    f32 max = 1.0f;
    f32 step = 0.0f;
    bool enabled = true;
};

struct DebugMiniTable {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    bool draw_header = true;
    f32 row_height = 20.0f;
    f32 header_height = 22.0f;
};

inline DebugWidgetStyle debug_widget_style(const Theme& theme) {
    return {
        .row_height = std::max(24.0f, theme.sizes.compact_control_height),
        .section_height = std::max(26.0f, theme.sizes.compact_control_height),
        .label_width = 132.0f,
        .control_width = 168.0f,
        .value_width = 72.0f,
        .metric_width = 112.0f,
        .gap = std::max(6.0f, theme.sizes.gap * 0.75f),
        .panel_padding = {10.0f, 8.0f, 10.0f, 8.0f},
        .title_text = theme.emphasis_text,
        .label_text = theme.small_text,
        .value_text = theme.small_text,
        .panel = theme.panel,
        .row = theme.list_item,
        .control = theme.input,
    };
}

inline std::string debug_widget_child_name(std::string_view name, std::string_view suffix) {
    std::string out{name};
    out += "_";
    out += suffix;
    return out;
}

inline LayoutStyle debug_row_layout(const DebugWidgetStyle& style) {
    return row(grow(), fixed(style.row_height), UiAlign::Start, UiAlign::Center, style.gap);
}

inline kin::Ui2EntityBuilder debug_inspector_panel(flecs::world& world,
                                                   std::string_view name,
                                                   const kin::Ui2EntityBuilder& parent,
                                                   const Theme& theme,
                                                   DebugInspectorPanel panel = {},
                                                   DebugWidgetStyle style = {}) {
    if (!style.title_text.font.valid()) {
        style = debug_widget_style(theme);
    }

    LayoutStyle panel_layout = column(grow(), grow(), UiAlign::Start, UiAlign::Stretch, style.gap);
    panel_layout.padding = style.panel_padding;
    panel_layout.draw_surface = panel.draw_surface;
    panel_layout.surface = theme.card_surface;

    kin::Ui2EntityBuilder root = kin::ui2_entity(world, name);
    root.layout(panel_layout).child_of(parent);

    if (!panel.title.empty()) {
        kin::ui2_entity(world, debug_widget_child_name(name, "title"))
            .label({.text = std::move(panel.title), .text_style = style.title_text})
            .layout(row(grow(), fixed(style.section_height), UiAlign::Start, UiAlign::Center))
            .child_of(root);
        kin::ui2_entity(world, debug_widget_child_name(name, "rule"))
            .separator({.color = theme.colors.border})
            .layout(row(grow(), fixed(1.0f)))
            .child_of(root);
    }

    kin::Ui2EntityBuilder body = kin::ui2_entity(world, debug_widget_child_name(name, "body"));
    LayoutStyle body_layout = column(grow(), panel.scrollable ? grow() : fit(), UiAlign::Start, UiAlign::Stretch, style.gap);
    body.layout(body_layout).child_of(root);
    if (panel.scrollable) {
        body.scroll_container({
            .wheel_step = panel.wheel_step,
            .scrollbar_thickness = 8.0f,
            .scrollbar_track = theme.colors.surface_subtle,
            .scrollbar_thumb = theme.colors.solid_accent,
        });
    }
    return body;
}

inline kin::Ui2EntityBuilder debug_inspector_panel(kin::EcsWorld& world,
                                                   std::string_view name,
                                                   const kin::Ui2EntityBuilder& parent,
                                                   const Theme& theme,
                                                   DebugInspectorPanel panel = {},
                                                   DebugWidgetStyle style = {}) {
    return debug_inspector_panel(world.raw(), name, parent, theme, std::move(panel), style);
}

inline kin::Ui2EntityBuilder debug_metric_row(flecs::world& world,
                                              std::string_view name,
                                              const kin::Ui2EntityBuilder& parent,
                                              const Theme& theme,
                                              DebugMetricRow metrics,
                                              DebugWidgetStyle style = {}) {
    if (!style.title_text.font.valid()) {
        style = debug_widget_style(theme);
    }
    const f32 metric_w = metrics.metric_width > 0.0f ? metrics.metric_width : style.metric_width;
    kin::Ui2EntityBuilder row_builder = kin::ui2_entity(world, name);
    row_builder.layout(row(grow(), fit(), UiAlign::Start, UiAlign::Start, style.gap)).child_of(parent);

    for (std::size_t i = 0; i < metrics.metrics.size(); ++i) {
        const DebugMetric& metric = metrics.metrics[i];
        kin::Ui2EntityBuilder cell = kin::ui2_entity(world, debug_widget_child_name(name, "metric_" + std::to_string(i)));
        cell.layout(column(fixed(metric_w), fit(), UiAlign::Start, UiAlign::Stretch, 2.0f)).child_of(row_builder);
        kin::ui2_entity(world, debug_widget_child_name(name, "metric_label_" + std::to_string(i)))
            .label({.text = metric.label, .text_style = style.label_text})
            .layout(row(grow(), fixed(style.row_height * 0.7f)))
            .child_of(cell);
        kin::ui2_entity(world, debug_widget_child_name(name, "metric_value_" + std::to_string(i)))
            .label({.text = metric.value, .text_style = style.value_text})
            .layout(row(grow(), fixed(style.row_height * 0.85f)))
            .child_of(cell);
    }
    return row_builder;
}

inline kin::Ui2EntityBuilder debug_metric_row(kin::EcsWorld& world,
                                              std::string_view name,
                                              const kin::Ui2EntityBuilder& parent,
                                              const Theme& theme,
                                              DebugMetricRow metrics,
                                              DebugWidgetStyle style = {}) {
    return debug_metric_row(world.raw(), name, parent, theme, std::move(metrics), style);
}

inline kin::Ui2EntityBuilder debug_property_group(flecs::world& world,
                                                  std::string_view name,
                                                  const kin::Ui2EntityBuilder& parent,
                                                  const Theme& theme,
                                                  DebugPropertyGroup group,
                                                  DebugWidgetStyle style = {}) {
    if (!style.title_text.font.valid()) {
        style = debug_widget_style(theme);
    }
    kin::Ui2EntityBuilder group_builder = kin::ui2_entity(world, name);
    group_builder.layout(column(grow(), fit(), UiAlign::Start, UiAlign::Stretch, 2.0f)).child_of(parent);
    if (!group.title.empty()) {
        kin::ui2_entity(world, debug_widget_child_name(name, "title"))
            .label({.text = std::move(group.title), .text_style = style.title_text})
            .layout(row(grow(), fixed(style.section_height)))
            .child_of(group_builder);
    }
    for (std::size_t i = 0; i < group.rows.size(); ++i) {
        const DebugProperty& property = group.rows[i];
        kin::Ui2EntityBuilder row_builder = kin::ui2_entity(world, debug_widget_child_name(name, "row_" + std::to_string(i)));
        row_builder.layout(debug_row_layout(style)).child_of(group_builder);
        kin::ui2_entity(world, debug_widget_child_name(name, "label_" + std::to_string(i)))
            .label({.text = property.modified ? "* " + property.label : property.label, .text_style = style.label_text})
            .layout(row(fixed(style.label_width), fixed(style.row_height)))
            .child_of(row_builder);
        kin::ui2_entity(world, debug_widget_child_name(name, "value_" + std::to_string(i)))
            .label({.text = property.value, .text_style = style.value_text})
            .layout(row(grow(), fixed(style.row_height)))
            .child_of(row_builder);
    }
    return group_builder;
}

inline kin::Ui2EntityBuilder debug_property_group(kin::EcsWorld& world,
                                                  std::string_view name,
                                                  const kin::Ui2EntityBuilder& parent,
                                                  const Theme& theme,
                                                  DebugPropertyGroup group,
                                                  DebugWidgetStyle style = {}) {
    return debug_property_group(world.raw(), name, parent, theme, std::move(group), style);
}

inline kin::Ui2EntityBuilder debug_toggle_row(flecs::world& world,
                                              std::string_view name,
                                              const kin::Ui2EntityBuilder& parent,
                                              const Theme& theme,
                                              DebugToggleRow row_data,
                                              DebugWidgetStyle style = {}) {
    if (!style.title_text.font.valid()) {
        style = debug_widget_style(theme);
    }
    kin::Ui2EntityBuilder row_builder = kin::ui2_entity(world, name);
    row_builder.layout(debug_row_layout(style)).child_of(parent);
    kin::ui2_entity(world, debug_widget_child_name(name, "label"))
        .label({.text = std::move(row_data.label), .text_style = style.label_text})
        .layout(row(fixed(style.label_width), fixed(style.row_height)))
        .child_of(row_builder);
    kin::ui2_entity(world, debug_widget_child_name(name, "toggle"))
        .toggle({.label = "",
                 .text_style = style.value_text,
                 .style = theme.button,
                 .value = row_data.value,
                 .enabled = row_data.enabled})
        .layout(row(fixed(style.control_width), fixed(style.row_height)))
        .child_of(row_builder);
    return row_builder;
}

inline kin::Ui2EntityBuilder debug_toggle_row(kin::EcsWorld& world,
                                              std::string_view name,
                                              const kin::Ui2EntityBuilder& parent,
                                              const Theme& theme,
                                              DebugToggleRow row_data,
                                              DebugWidgetStyle style = {}) {
    return debug_toggle_row(world.raw(), name, parent, theme, std::move(row_data), style);
}

inline kin::Ui2EntityBuilder debug_enum_row(flecs::world& world,
                                            std::string_view name,
                                            const kin::Ui2EntityBuilder& parent,
                                            const Theme& theme,
                                            DebugEnumRow row_data,
                                            DebugWidgetStyle style = {}) {
    if (!style.title_text.font.valid()) {
        style = debug_widget_style(theme);
    }
    kin::Ui2EntityBuilder row_builder = kin::ui2_entity(world, name);
    row_builder.layout(debug_row_layout(style)).child_of(parent);
    kin::ui2_entity(world, debug_widget_child_name(name, "label"))
        .label({.text = std::move(row_data.label), .text_style = style.label_text})
        .layout(row(fixed(style.label_width), fixed(style.row_height)))
        .child_of(row_builder);
    row_data.selected = row_data.options.empty() ? 0 : std::clamp(row_data.selected, 0, static_cast<i32>(row_data.options.size()) - 1);
    kin::ui2_entity(world, debug_widget_child_name(name, "combo"))
        .combo_box({.items = std::move(row_data.options),
                    .state = {.selected = row_data.selected},
                    .text_style = style.value_text,
                    .style = style.control,
                    .enabled = row_data.enabled})
        .layout(row(fixed(style.control_width), fixed(style.row_height)))
        .child_of(row_builder);
    return row_builder;
}

inline kin::Ui2EntityBuilder debug_enum_row(kin::EcsWorld& world,
                                            std::string_view name,
                                            const kin::Ui2EntityBuilder& parent,
                                            const Theme& theme,
                                            DebugEnumRow row_data,
                                            DebugWidgetStyle style = {}) {
    return debug_enum_row(world.raw(), name, parent, theme, std::move(row_data), style);
}

inline kin::Ui2EntityBuilder debug_numeric_slider_row(flecs::world& world,
                                                      std::string_view name,
                                                      const kin::Ui2EntityBuilder& parent,
                                                      const Theme& theme,
                                                      DebugNumericSliderRow row_data,
                                                      DebugWidgetStyle style = {}) {
    if (!style.title_text.font.valid()) {
        style = debug_widget_style(theme);
    }
    kin::Ui2EntityBuilder row_builder = kin::ui2_entity(world, name);
    row_builder.layout(debug_row_layout(style)).child_of(parent);
    kin::ui2_entity(world, debug_widget_child_name(name, "label"))
        .label({.text = std::move(row_data.label), .text_style = style.label_text})
        .layout(row(fixed(style.label_width), fixed(style.row_height)))
        .child_of(row_builder);
    kin::ui2_entity(world, debug_widget_child_name(name, "slider"))
        .slider({.value = row_data.value,
                 .min = row_data.min,
                 .max = row_data.max,
                 .step = row_data.step,
                 .style = theme.button,
                 .enabled = row_data.enabled})
        .layout(row(grow(), fixed(style.row_height)))
        .child_of(row_builder);
    kin::ui2_entity(world, debug_widget_child_name(name, "value"))
        .label({.text = std::move(row_data.value_text), .text_style = style.value_text})
        .layout(row(fixed(style.value_width), fixed(style.row_height), UiAlign::End, UiAlign::Center))
        .child_of(row_builder);
    return row_builder;
}

inline kin::Ui2EntityBuilder debug_numeric_slider_row(kin::EcsWorld& world,
                                                      std::string_view name,
                                                      const kin::Ui2EntityBuilder& parent,
                                                      const Theme& theme,
                                                      DebugNumericSliderRow row_data,
                                                      DebugWidgetStyle style = {}) {
    return debug_numeric_slider_row(world.raw(), name, parent, theme, std::move(row_data), style);
}

inline kin::Ui2EntityBuilder debug_mini_table(flecs::world& world,
                                              std::string_view name,
                                              const kin::Ui2EntityBuilder& parent,
                                              const Theme& theme,
                                              DebugMiniTable mini_table,
                                              DebugWidgetStyle style = {}) {
    if (!style.title_text.font.valid()) {
        style = debug_widget_style(theme);
    }
    std::vector<UiTableColumn> columns;
    columns.reserve(mini_table.columns.size());
    for (std::string& column : mini_table.columns) {
        columns.push_back({.label = std::move(column), .width = 64.0f, .sizing = UiTableColumnSizing::Stretch});
    }
    const f32 table_height = (mini_table.draw_header ? mini_table.header_height : 0.0f) +
                             static_cast<f32>(mini_table.rows.size()) * mini_table.row_height;
    kin::Ui2EntityBuilder table_builder = kin::ui2_entity(world, name);
    table_builder
        .table({.columns = std::move(columns),
                .rows = std::move(mini_table.rows),
                .header_height = mini_table.header_height,
                .row_height = mini_table.row_height,
                .cell_padding_x = 4.0f,
                .draw_header = mini_table.draw_header,
                .text_style = style.value_text,
                .style = theme.list_item})
        .layout(column(grow(), fixed(std::max(style.row_height, table_height))))
        .child_of(parent);
    return table_builder;
}

inline kin::Ui2EntityBuilder debug_mini_table(kin::EcsWorld& world,
                                              std::string_view name,
                                              const kin::Ui2EntityBuilder& parent,
                                              const Theme& theme,
                                              DebugMiniTable mini_table,
                                              DebugWidgetStyle style = {}) {
    return debug_mini_table(world.raw(), name, parent, theme, std::move(mini_table), style);
}

} // namespace kin::ui2
