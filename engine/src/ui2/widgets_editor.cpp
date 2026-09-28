#include "widget_chunk_preamble.hpp"

namespace kin::ui2 {

Vec2f measure(const Splitter&) {
    return {240.0f, 160.0f};
}

Rectf splitter_rule_rect(Rectf handle, bool horizontal) {
    if (horizontal) {
        const f32 width = std::fmod(std::floor(handle.w), 2.0f) == 0.0f && handle.w >= 4.0f ? 2.0f : 1.0f;
        const f32 x = handle.x + std::max(0.0f, handle.w - width) * 0.5f;
        return {x, handle.y, width, handle.h};
    }
    const f32 height = std::fmod(std::floor(handle.h), 2.0f) == 0.0f && handle.h >= 4.0f ? 2.0f : 1.0f;
    const f32 y = handle.y + std::max(0.0f, handle.h - height) * 0.5f;
    return {handle.x, y, handle.w, height};
}

Rectf splitter_grip_rect(Rectf handle, bool horizontal) {
    if (horizontal) {
        const f32 width = 2.0f;
        const f32 height = std::clamp(handle.h * 0.12f, 18.0f, 32.0f);
        return {
            handle.x + (handle.w - width) * 0.5f,
            handle.y + (handle.h - height) * 0.5f,
            width,
            height,
        };
    }
    const f32 width = std::clamp(handle.w * 0.12f, 18.0f, 32.0f);
    const f32 height = 2.0f;
    return {
        handle.x + (handle.w - width) * 0.5f,
        handle.y + (handle.h - height) * 0.5f,
        width,
        height,
    };
}

struct DockPanelLayoutMetrics {
    UiPadding padding{};
    f32 header_height = 0.0f;
    f32 icon_size = 0.0f;
    f32 close_size = 0.0f;
    f32 gap = 0.0f;
};

DockPanelLayoutMetrics dock_panel_layout_metrics(const DockPanel& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    const f32 icon = std::max(0.0f, text_h);
    const f32 close = std::max(0.0f, text_h);
    return {
        .padding = padding,
        .header_height = std::max(widget.header_height, std::max(text_h, std::max(icon, close)) + padding.top + padding.bottom),
        .icon_size = icon,
        .close_size = close,
        .gap = std::max(4.0f, padding.left * 0.5f),
    };
}

Vec2f measure(const DockPanel& widget) {
    const DockPanelLayoutMetrics metrics = dock_panel_layout_metrics(widget);
    const f32 title_w = measure_text(widget.text_style.font, widget.title.empty() ? "Panel" : widget.title, widget.text_style.scale).x;
    const f32 chrome_w = metrics.padding.left +
                         (widget.icon.valid() ? metrics.icon_size + metrics.gap : 0.0f) +
                         title_w +
                         (widget.closable ? metrics.gap + metrics.close_size : 0.0f) +
                         metrics.padding.right;
    return {std::max(220.0f, chrome_w), std::max(160.0f, metrics.header_height)};
}

struct BreadcrumbBarLayoutMetrics {
    UiPadding padding{};
    f32 height = 0.0f;
    f32 icon_size = 0.0f;
    f32 segment_gap = 0.0f;
    f32 separator_gap = 0.0f;
};

BreadcrumbBarLayoutMetrics breadcrumb_bar_layout_metrics(const BreadcrumbBar& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    return {
        .padding = padding,
        .height = text_h + padding.top + padding.bottom,
        .icon_size = std::max(0.0f, text_h),
        .segment_gap = std::max(4.0f, padding.left * 0.5f),
        .separator_gap = std::max(widget.gap, padding.left * 0.5f),
    };
}

f32 breadcrumb_segment_width(const BreadcrumbSegment& segment, const TextStyle& text_style, const BreadcrumbBarLayoutMetrics& metrics) {
    return metrics.padding.left +
           (segment.icon.valid() ? metrics.icon_size + metrics.segment_gap : 0.0f) +
           measure_text(text_style.font, segment.label, text_style.scale).x +
           metrics.padding.right;
}

Vec2f measure(const BreadcrumbBar& widget) {
    const BreadcrumbBarLayoutMetrics metrics = breadcrumb_bar_layout_metrics(widget);
    const Vec2f separator = measure_text(widget.text_style.font, "/", widget.text_style.scale);
    f32 width = metrics.padding.left + metrics.padding.right;
    for (std::size_t i = 0; i < widget.segments.size(); ++i) {
        width += breadcrumb_segment_width(widget.segments[i], widget.text_style, metrics);
        if (i + 1 < widget.segments.size()) {
            width += separator.x + metrics.separator_gap * 2.0f;
        }
    }
    return {std::max(240.0f, width), metrics.height};
}

struct PropertyGridLayoutMetrics {
    f32 row_height = 0.0f;
    f32 control_height = 0.0f;
    f32 label_width = 0.0f;
    f32 label_gap = 0.0f;
    UiPadding padding{};
};

PropertyGridLayoutMetrics property_grid_layout_metrics(const PropertyGrid& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 label_text_h = measure_text(widget.label_style.font, "Mg", widget.label_style.scale).y;
    const f32 value_text_h = measure_text(widget.value_style.font, "Mg", widget.value_style.scale).y;
    f32 label_w = std::max(0.0f, widget.label_width);
    for (const PropertyGridRow& row : widget.rows) {
        label_w = std::max(label_w, measure_text(widget.label_style.font, row.label, widget.label_style.scale).x + padding.left);
    }
    const f32 natural_control = std::max(text_input_layout_metrics(widget.value_style, widget.style).height, value_text_h + padding.top + padding.bottom);
    return {
        .row_height = std::max({widget.row_height, label_text_h + padding.top + padding.bottom, natural_control}),
        .control_height = std::max(widget.control_height, natural_control),
        .label_width = label_w,
        .label_gap = std::max(widget.label_gap, padding.left * 0.5f),
        .padding = padding,
    };
}

f32 property_grid_content_height(const PropertyGrid& widget, const PropertyGridLayoutMetrics& metrics) {
    const i32 count = static_cast<i32>(widget.rows.size());
    if (count <= 0) {
        return 0.0f;
    }
    return static_cast<f32>(count) * metrics.row_height + static_cast<f32>(count - 1) * widget.row_spacing;
}

Vec2f measure(const PropertyGrid& widget) {
    const PropertyGridLayoutMetrics metrics = property_grid_layout_metrics(widget);
    f32 control_w = std::max(140.0f, measure(NumberInput{}).x);
    for (const PropertyGridRow& row : widget.rows) {
        control_w = std::max(control_w, measure_text(widget.value_style.font, row.value_text, widget.value_style.scale).x + metrics.padding.left + metrics.padding.right);
    }
    return {metrics.label_width + metrics.label_gap + control_w, std::max(0.0f, property_grid_content_height(widget, metrics))};
}

Vec2f measure(const PropertyInspector& widget) {
    const PropertyInspectorLayoutMetrics metrics = property_inspector_layout_metrics(widget);
    f32 control_w = 140.0f;
    for (const PropertyInspectorRow& row : widget.rows) {
        control_w = std::max(control_w, measure_text(widget.value_style.font, row.value_text.empty() ? row.text_value : row.value_text, widget.value_style.scale).x + metrics.padding.left + metrics.padding.right);
    }
    return {
        std::max(320.0f, metrics.label_width + metrics.label_gap + control_w + metrics.padding.right),
        widget.rows.empty() ? 240.0f : property_inspector_content_height(widget, metrics),
    };
}

Vec2f measure(const NodeGraph&) {
    return {480.0f, 320.0f};
}

void run(Context& ctx, Splitter& widget) {
    widget.changed = false;
    widget.dragging = false;
    const bool horizontal = widget.axis == UiLayoutAxis::Horizontal;
    const f32 total = std::max(0.0f, (horizontal ? widget.bounds.w : widget.bounds.h) - widget.thickness);
    const f32 min_total = widget.min_first + widget.min_second;
    const f32 usable = std::max(total, min_total);
    f32 first_size = std::clamp(widget.ratio * usable, widget.min_first, std::max(widget.min_first, usable - widget.min_second));
    widget.ratio = usable > 0.0f ? std::clamp(first_size / usable, 0.0f, 1.0f) : 0.5f;
    if (horizontal) {
        widget.first = {widget.bounds.x, widget.bounds.y, std::min(first_size, total), widget.bounds.h};
        widget.handle = {widget.bounds.x + widget.first.w, widget.bounds.y, widget.thickness, widget.bounds.h};
        widget.second = {widget.handle.x + widget.handle.w, widget.bounds.y, std::max(0.0f, widget.bounds.x + widget.bounds.w - widget.handle.x - widget.handle.w), widget.bounds.h};
    } else {
        widget.first = {widget.bounds.x, widget.bounds.y, widget.bounds.w, std::min(first_size, total)};
        widget.handle = {widget.bounds.x, widget.bounds.y + widget.first.h, widget.bounds.w, widget.thickness};
        widget.second = {widget.bounds.x, widget.handle.y + widget.handle.h, widget.bounds.w, std::max(0.0f, widget.bounds.y + widget.bounds.h - widget.handle.y - widget.handle.h)};
    }
    const Interaction it = ctx.region(widget.id ? widget.id : make_id("splitter"), widget.handle, widget.z);
    if (widget.enabled && it.active && ctx.pointer_held()) {
        const f32 pointer_axis = horizontal ? ctx.pointer().x - widget.bounds.x : ctx.pointer().y - widget.bounds.y;
        const f32 next_first = std::clamp(pointer_axis - widget.thickness * 0.5f, widget.min_first, std::max(widget.min_first, total - widget.min_second));
        const f32 next_ratio = total > 0.0f ? next_first / total : widget.ratio;
        widget.changed = next_ratio != widget.ratio;
        widget.ratio = next_ratio;
        widget.dragging = true;
    }
    const bool active = widget.dragging || (it.active && ctx.pointer_held());
    const Color rule_color = !widget.enabled
                                 ? ctx.theme().colors.text_disabled
                                 : active
                                       ? ctx.theme().colors.solid_accent
                                       : (it.hot || it.focused ? ctx.theme().colors.border_strong : ctx.theme().colors.border);
    ctx.fill_rect(splitter_rule_rect(widget.handle, horizontal), rule_color);

    if (widget.enabled && (it.hot || it.focused || active)) {
        SurfaceStyle grip = with_fill_border(ctx.theme().subtle_surface, rule_color, colors::transparent);
        grip.radius = std::max(1.0f, ctx.theme().preset.radius);
        ctx.surface(splitter_grip_rect(widget.handle, horizontal), grip);
    }
}

void run(Context& ctx, DockPanel& widget) {
    const auto _draw_scope = ctx.draw_scope("DockPanel", widget.bounds, ctx.theme().card_surface.radius);
    widget.style = themed_widget_style(widget.style, ctx.theme().panel);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const DockPanelLayoutMetrics metrics = dock_panel_layout_metrics(widget);
    widget.toggled = false;
    widget.close_requested = false;
    widget.header = {widget.bounds.x, widget.bounds.y, widget.bounds.w, std::min(metrics.header_height, widget.bounds.h)};
    widget.body = widget.collapsed ? Rectf{} : Rectf{widget.bounds.x, widget.bounds.y + widget.header.h, widget.bounds.w, std::max(0.0f, widget.bounds.h - widget.header.h)};
    const Color outer_border = widget_border(widget.style, WidgetColorState::Normal);
    const SurfaceStyle outer_surface = with_fill_border(ctx.theme().card_surface, widget.style.track, outer_border);
    // Draw shadow + fill only — border suppressed here so the header/body fills drawn next don't
    // overpaint the corner arcs. collection_outline() strokes the border on top at the very end.
    const CollectionFrame outer_frame = collection_background(ctx, widget.bounds, outer_surface);
    const Interaction header_it = ctx.region(widget.id ? widget.id : make_id("dock_panel"), widget.header, widget.z);
    if (header_it.clicked) {
        widget.toggled = true;
    }
    const Color header_fill = widget.active
                                  ? widget_fill(widget.style, WidgetColorState::Hovered)
                                  : widget_fill(widget.style, WidgetColorState::Normal);
    SurfaceStyle header_surface = with_fill_border(ctx.theme().header_surface, header_fill, colors::transparent);
    header_surface.border_mode = BorderMode::None;
    header_surface.shadow.enabled = false;
    ctx.surface(widget.header, header_surface);
    if (header_surface.radius > 0.0f) {
        const f32 border = std::max(1.0f, outer_surface.border_width);
        const f32 strip_h = std::min(header_surface.radius, widget.header.h);
        ctx.fill_rect({widget.header.x + border,
                       widget.header.y + widget.header.h - strip_h,
                       std::max(0.0f, widget.header.w - border * 2.0f),
                       strip_h},
                      header_surface.fill);
    }
    // Header/body divider as a rect (not a line): a line's endpoints are inclusive, so its right
    // tip overhangs the exclusive right edge of the strip/body fills by 1px, leaving a stray
    // pixel poking past the panel edge. A fill_rect shares the fills' exclusive edges — flush.
    const f32 divider_h = std::max(1.0f, outer_surface.border_width);
    ctx.fill_rect({widget.header.x + outer_surface.border_width,
                   widget.header.y + widget.header.h - divider_h,
                   std::max(0.0f, widget.header.w - outer_surface.border_width * 2.0f),
                   divider_h},
                  widget_border(widget.style, WidgetColorState::Normal));
    f32 x = widget.header.x + metrics.padding.left;
    if (widget.icon.valid()) {
        ctx.sprite(widget.icon, {x, widget.header.y + (widget.header.h - metrics.icon_size) * 0.5f, metrics.icon_size, metrics.icon_size});
        x += metrics.icon_size + metrics.gap;
    }
    const Vec2f title_size = measure_text(widget.text_style.font, widget.title, widget.text_style.scale);
    ctx.text(widget.title, {x, widget.header.y + (widget.header.h - title_size.y) * 0.5f}, widget.text_style);
    if (widget.closable) {
        const Rectf close{widget.header.x + widget.header.w - metrics.padding.right - metrics.close_size,
                          widget.header.y + (widget.header.h - metrics.close_size) * 0.5f,
                          metrics.close_size,
                          metrics.close_size};
        const Interaction close_it = ctx.region(make_id(widget.id ? widget.id : make_id("dock_panel"), "close"), close, widget.z + 1);
        if (close_it.clicked) {
            widget.close_requested = true;
        }
        const Vec2f close_size = measure_text(widget.text_style.font, "x", widget.text_style.scale);
        ctx.text("x", {close.x + (close.w - close_size.x) * 0.5f, close.y + (close.h - close_size.y) * 0.5f}, widget.text_style);
    }
    if (!widget.collapsed) {
        // Bottom-rounded body fill: extends above the clip so the top edge is straight
        // (flush with the header separator) but the bottom two corners follow the panel radius.
        const Rectf body_rect{widget.body.x + outer_surface.border_width,
                              widget.body.y,
                              std::max(0.0f, widget.body.w - outer_surface.border_width * 2.0f),
                              std::max(0.0f, widget.body.h - outer_surface.border_width)};
        draw_panel_body_fill(ctx, body_rect, outer_frame.inner_radius,
                             widget_fill(widget.style, WidgetColorState::Normal));
    }
    // Stroke the border last so nothing drawn above (header fill, body fill) overpaints the corners.
    collection_outline(ctx, widget.bounds, outer_surface);
}

void run(Context& ctx, BreadcrumbBar& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().menu);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const BreadcrumbBarLayoutMetrics metrics = breadcrumb_bar_layout_metrics(widget);
    widget.activated = -1;
    widget.activated_id.clear();
    widget.overflowed = false;
    f32 x = widget.bounds.x + metrics.padding.left;
    const f32 right = widget.bounds.x + widget.bounds.w - metrics.padding.right;
    i32 start = 0;
    std::vector<f32> widths;
    widths.reserve(widget.segments.size());
    const Vec2f separator_size = measure_text(widget.text_style.font, "/", widget.text_style.scale);
    const f32 separator_w = separator_size.x + metrics.separator_gap * 2.0f;
    f32 total = 0.0f;
    for (std::size_t i = 0; i < widget.segments.size(); ++i) {
        const BreadcrumbSegment& segment = widget.segments[i];
        const f32 w = breadcrumb_segment_width(segment, widget.text_style, metrics);
        widths.push_back(w);
        total += w;
        if (i + 1 < widget.segments.size()) {
            total += separator_w;
        }
    }
    if (total > widget.bounds.w) {
        widget.overflowed = true;
        x += measure_text(widget.text_style.font, "...", widget.text_style.scale).x + metrics.segment_gap;
        const Vec2f ellipsis_size = measure_text(widget.text_style.font, "...", widget.text_style.scale);
        ctx.text("...", {widget.bounds.x + metrics.padding.left, widget.bounds.y + (widget.bounds.h - ellipsis_size.y) * 0.5f}, widget.text_style);
        while (start < static_cast<i32>(widths.size()) && total > widget.bounds.w - metrics.padding.left - metrics.padding.right - ellipsis_size.x - metrics.segment_gap) {
            total -= widths[static_cast<std::size_t>(start)] + separator_w;
            ++start;
        }
    }
    for (i32 i = start; i < static_cast<i32>(widget.segments.size()); ++i) {
        const BreadcrumbSegment& segment = widget.segments[static_cast<std::size_t>(i)];
        const Rectf rect{x, widget.bounds.y, std::min(widths[static_cast<std::size_t>(i)], right - x), widget.bounds.h};
        if (rect.w <= 0.0f) {
            break;
        }
        const Interaction it = ctx.region(make_id(widget.id, static_cast<u64>(i)), rect, widget.z);
        if (it.clicked && segment.enabled) {
            widget.activated = i;
            widget.activated_id = item_result_id(segment.id, segment.label);
        }
        ctx.surface(rect, ctx.resolve_animated(make_id(widget.id, static_cast<u64>(i)), widget.style.surface, it, false, segment.enabled));
        TextStyle text_style = widget.text_style;
        if (!segment.enabled) {
            text_style.color = widget_fill(widget.style, WidgetColorState::Disabled);
        } else if (i + 1 == static_cast<i32>(widget.segments.size())) {
            text_style.color = ctx.theme().colors.text_emphasis;
        } else if (it.hot) {
            text_style.color = ctx.theme().colors.text;
        }
        const Vec2f label_size = measure_text(text_style.font, segment.label, text_style.scale);
        f32 label_x = rect.x + metrics.padding.left;
        if (segment.icon.valid()) {
            ctx.sprite(segment.icon, {label_x, rect.y + (rect.h - metrics.icon_size) * 0.5f, metrics.icon_size, metrics.icon_size});
            label_x += metrics.icon_size + metrics.segment_gap;
        }
        ctx.text(segment.label, {label_x, rect.y + (rect.h - label_size.y) * 0.5f}, text_style);
        if (i + 1 < static_cast<i32>(widget.segments.size())) {
            const Rectf separator_rect{rect.x + rect.w, widget.bounds.y, std::min(separator_w, right - rect.x - rect.w), widget.bounds.h};
            if (separator_rect.w > 0.0f) {
                ctx.text("/", {separator_rect.x + (separator_rect.w - separator_size.x) * 0.5f, separator_rect.y + (separator_rect.h - separator_size.y) * 0.5f}, text_style);
            }
        }
        x += rect.w + (i + 1 < static_cast<i32>(widget.segments.size()) ? separator_w : 0.0f);
    }
}

void run(Context& ctx, PropertyInspector& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().inspector);
    widget.label_style = themed_text_style(widget.label_style, ctx.theme().body_text);
    widget.value_style = themed_text_style(widget.value_style, ctx.theme().body_text);
    const PropertyInspectorLayoutMetrics metrics = property_inspector_layout_metrics(widget);
    widget.changed_row = -1;
    widget.changed_id.clear();
    widget.clicked_id.clear();
    widget.toggled_section_id.clear();
    widget.reset_requested_id.clear();
    widget.action_requested_id.clear();
    widget.reference_pick_requested_id.clear();
    widget.drag_source_id.clear();
    widget.drop_target_id.clear();
    widget.changed = false;

    const f32 content_height = property_inspector_content_height(widget, metrics);
    ScrollState scroll{
        .offset = {0.0f, widget.offset},
        .content_size = {widget.bounds.w, content_height},
        .viewport_size = {widget.bounds.w, widget.bounds.h},
    };
    clamp_scroll(scroll);
    if (widget.enabled && contains(widget.bounds, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        scroll_by(scroll, {0.0f, -ctx.mouse_wheel_y() * widget.wheel_step});
    }
    widget.offset = scroll.offset.y;

    if (widget.draw_background) {
        ctx.fill_rect(widget.bounds, widget.style.track);
        ctx.outline_rect(widget.bounds, widget_border(widget.style, WidgetColorState::Normal));
    }

    const f32 control_x = widget.bounds.x + metrics.label_width + metrics.label_gap;
    const f32 control_w = std::max(0.0f, widget.bounds.x + widget.bounds.w - control_x - metrics.padding.right);
    const f32 control_h = std::min(metrics.control_height, metrics.row_height);
    f32 y = widget.bounds.y - widget.offset;
    widget.first = 0;
    widget.visible = 0;
    bool section_open = true;
    struct DeferredComboPopup {
        ComboBox combo;
        PropertyInspectorRow* row = nullptr;
        UiComboState* state = nullptr;
        i32 row_index = -1;
        std::string result_id;
    };
    std::vector<DeferredComboPopup> deferred_combos;
    struct DeferredColorPopup {
        Id id{};
        Rectf anchor{};
        ColorPicker picker{};
        PropertyInspectorRow* row = nullptr;
        i32 row_index = -1;
        std::string result_id;
    };
    std::vector<DeferredColorPopup> deferred_colors;

    ctx.push_clip(widget.bounds);
    for (i32 i = 0; i < static_cast<i32>(widget.rows.size()); ++i) {
        PropertyInspectorRow& row = widget.rows[static_cast<std::size_t>(i)];
        row.clicked = false;
        row.changed = false;
        row.section_toggled = false;
        if (row.kind != PropertyInspectorRowKind::Section && !section_open) {
            row.row_rect = {};
            row.control_rect = {};
            continue;
        }
        const f32 row_h = property_inspector_row_height(widget, metrics, row);
        row.row_rect = {widget.bounds.x, y, widget.bounds.w, row_h};
        row.control_rect = {control_x, y + (row_h - control_h) * 0.5f, control_w, control_h};
        y += row_h + widget.row_spacing;
        const bool section_row = row.kind == PropertyInspectorRowKind::Section;

        if (row.row_rect.y + row.row_rect.h < widget.bounds.y || row.row_rect.y > widget.bounds.y + widget.bounds.h) {
            if (section_row) {
                section_open = row.expanded;
            }
            continue;
        }
        if (widget.visible == 0) {
            widget.first = i;
        }
        ++widget.visible;

        const std::string result_id = inspector_row_result_id(row, i);
        const Id row_id = property_inspector_row_id(widget, row, i);
        const bool interactive = widget.enabled && row.enabled && !row.read_only;
        TextStyle label_style = widget.label_style;
        TextStyle value_style = widget.value_style;
        if (!row.enabled || row.read_only) {
            label_style.color = widget_fill(widget.style, WidgetColorState::Disabled);
            value_style.color = widget_fill(widget.style, WidgetColorState::Disabled);
        }
        if (!row.error.empty()) {
            value_style.color = ctx.theme().colors.solid_danger;
        }

        if (row.kind == PropertyInspectorRowKind::Separator) {
            ctx.fill_rect({row.row_rect.x + metrics.padding.left, row.row_rect.y + row.row_rect.h * 0.5f, std::max(0.0f, row.row_rect.w - metrics.padding.left - metrics.padding.right), 1.0f}, widget_border(widget.style, WidgetColorState::Normal));
            continue;
        }

        if (section_row) {
            const WidgetStyle section_style = themed_widget_style({}, ctx.theme().list_item);
            const Interaction it = ctx.region(row_id, row.row_rect, widget.z);
            if (interactive && it.clicked) {
                row.section_toggled = true;
                widget.toggled_section_id = result_id;
                widget.clicked_id = result_id;
            }
            ctx.surface(row.row_rect, ctx.resolve_animated(row_id, section_style.surface, it, false, interactive));
            if (row.modified) {
                ctx.outline_rect(row.row_rect, widget.style.accent);
            }
            const Vec2f label_size = measure_text(label_style.font, row.label, label_style.scale);
            const f32 text_y = row.row_rect.y + (row.row_rect.h - label_size.y) * 0.5f;
            ctx.text(row.expanded ? "v" : ">", {row.row_rect.x + metrics.padding.left, text_y}, label_style);
            ctx.text(row.label, {row.row_rect.x + metrics.padding.left * 2.0f + label_size.y, text_y}, label_style);
            section_open = row.expanded;
            continue;
        }

        const f32 marker_size = std::clamp(measure_text(label_style.font, "Mg", label_style.scale).y * 0.35f, 4.0f, 6.0f);
        const f32 marker_gutter = property_inspector_marker_gutter(label_style, metrics.padding);
        const Rectf label_rect{widget.bounds.x + metrics.padding.left + marker_gutter,
                               row.row_rect.y,
                               std::max(0.0f, metrics.label_width - metrics.padding.left - marker_gutter),
                               row.row_rect.h};
        const Vec2f label_size = measure_text(label_style.font, row.label, label_style.scale);
        ctx.text(row.label, {label_rect.x, label_rect.y + (label_rect.h - label_size.y) * 0.5f}, label_style);
        if (row.modified) {
            ctx.fill_rect({widget.bounds.x + metrics.padding.left,
                           row.row_rect.y + (row.row_rect.h - marker_size) * 0.5f,
                           marker_size,
                           marker_size},
                          widget.style.accent);
        }
        if (!row.tooltip.empty()) {
            ctx.tooltip(make_id(row_id, "tooltip"), row.row_rect, row.tooltip);
        }
        if (row.resettable) {
            const f32 reset_size = std::min(metrics.control_height, std::max(0.0f, row.row_rect.h - metrics.padding.top - metrics.padding.bottom));
            const Rectf reset{row.row_rect.x + row.row_rect.w - reset_size - metrics.padding.right, row.row_rect.y + (row.row_rect.h - reset_size) * 0.5f, reset_size, reset_size};
            if (ctx.region(make_id(row_id, "reset"), reset, widget.z + 1).clicked && interactive) {
                widget.reset_requested_id = result_id;
            }
            const Vec2f reset_text = measure_text(value_style.font, "R", value_style.scale);
            ctx.text("R", {reset.x + (reset.w - reset_text.x) * 0.5f, reset.y + (reset.h - reset_text.y) * 0.5f}, value_style);
        }
        Rectf control = row.control_rect;
        if (row.resettable) {
            control.w = std::max(0.0f, control.w - metrics.control_height - metrics.padding.right);
        }

        switch (row.kind) {
        case PropertyInspectorRowKind::Label: {
            const std::string value = row.mixed ? "<mixed>" : row.value_text;
            const Vec2f value_size = measure_text(value_style.font, value, value_style.scale);
            ctx.text(value, {control.x + metrics.padding.left, control.y + (control.h - value_size.y) * 0.5f}, value_style);
            break;
        }
        case PropertyInspectorRowKind::Bool: {
            Toggle toggle{.id = row_id, .bounds = control, .label = row.bool_value ? "On" : "Off", .text_style = value_style, .style = widget.style, .value = row.bool_value, .enabled = interactive, .z = widget.z};
            run(ctx, toggle);
            if (toggle.changed) {
                row.bool_value = toggle.value;
                row.changed = true;
            }
            break;
        }
        case PropertyInspectorRowKind::Number: {
            UiTextInputState& state = ctx.text_input_state(make_id(row_id, "number"), format_f32(row.number_value));
            NumberInput number{.id = row_id, .bounds = control, .text = state, .value = row.number_value, .min = row.number_min, .max = row.number_max, .step = row.number_step, .text_style = value_style, .style = widget.style, .enabled = interactive, .z = widget.z};
            run(ctx, number);
            state = number.text;
            if (number.changed) {
                row.number_value = number.value;
                row.changed = true;
            }
            break;
        }
        case PropertyInspectorRowKind::Text: {
            UiTextInputState& state = ctx.text_input_state(row_id, row.text_value);
            TextInput text{.id = row_id, .bounds = control, .state = state, .text_style = value_style, .style = widget.style, .enabled = interactive, .z = widget.z};
            run(ctx, text);
            state = text.state;
            if (text.result.changed || text.result.committed || text.result.cancelled) {
                row.text_value = text.state.text;
                row.changed = true;
            }
            break;
        }
        case PropertyInspectorRowKind::Combo:
        case PropertyInspectorRowKind::Enum: {
            UiComboState& state = ctx.combo_state(row_id, row.selected);
            ComboBox combo{.id = row_id, .bounds = control, .items = row.options, .state = state, .text_style = value_style, .style = widget.style, .draw_popup = false, .enabled = interactive, .z = widget.z};
            run(ctx, combo);
            state = combo.state;
            if (combo.result.changed) {
                row.selected = combo.state.selected;
                row.changed = true;
            }
            if (combo.state.open) {
                deferred_combos.push_back({.combo = combo, .row = &row, .state = &state, .row_index = i, .result_id = result_id});
            }
            break;
        }
        case PropertyInspectorRowKind::Color: {
            const Interaction it = ctx.region(row_id, control, widget.z);
            row.clicked = interactive && it.clicked;
            const Id picker_id = make_id(row_id, "picker");
            if (row.clicked) {
                ctx.open_popup(picker_id);
            }
            ctx.fill_rect(control, resolved_widget_fill(widget.style, it, false, interactive));
            ctx.outline_rect(control, row.error.empty() ? resolved_widget_border(widget.style, it, false, interactive) : ctx.theme().colors.solid_danger);
            const f32 swatch_size = std::max(0.0f, control.h - metrics.padding.top - metrics.padding.bottom);
            const Rectf swatch{control.x + metrics.padding.left, control.y + (control.h - swatch_size) * 0.5f, swatch_size, swatch_size};
            ctx.fill_rect(swatch, row.color_value);
            ctx.outline_rect(swatch, widget_border(widget.style, WidgetColorState::Normal));
            const std::string hex = format_color_hex(row.color_value);
            const Vec2f hex_size = measure_text(value_style.font, hex, value_style.scale);
            ctx.text(hex, {swatch.x + swatch.w + metrics.padding.left, control.y + (control.h - hex_size.y) * 0.5f}, value_style);
            if (ctx.popup_open(picker_id)) {
                deferred_colors.push_back({
                    .id = picker_id,
                    .anchor = control,
                    .picker = ColorPicker{
                        .id = picker_id,
                        .value = row.color_value,
                        .text_style = value_style,
                        .style = widget.style,
                        .z = widget.z + 10001,
                    },
                    .row = &row,
                    .row_index = i,
                    .result_id = result_id,
                });
            }
            break;
        }
        case PropertyInspectorRowKind::Vector: {
            const i32 dim = std::clamp(row.vector_dimension, 2, 4);
            row.vector_values.resize(static_cast<std::size_t>(dim), 0.0f);
            const f32 gap = metrics.label_gap;
            const f32 cell_w = (control.w - static_cast<f32>(dim - 1) * gap) / static_cast<f32>(dim);
            for (i32 component = 0; component < dim; ++component) {
                const Id component_id = make_id(row_id, static_cast<u64>(component));
                UiTextInputState& state = ctx.text_input_state(component_id, format_f32(row.vector_values[static_cast<std::size_t>(component)]));
                NumberInput number{
                    .id = component_id,
                    .bounds = {control.x + static_cast<f32>(component) * (cell_w + gap), control.y, std::max(0.0f, cell_w), control.h},
                    .text = state,
                    .value = row.vector_values[static_cast<std::size_t>(component)],
                    .min = row.number_min,
                    .max = row.number_max,
                    .step = row.number_step,
                    .text_style = value_style,
                    .style = widget.style,
                    .enabled = interactive,
                    .z = widget.z,
                };
                run(ctx, number);
                state = number.text;
                if (number.changed) {
                    row.vector_values[static_cast<std::size_t>(component)] = number.value;
                    row.changed = true;
                }
            }
            break;
        }
        case PropertyInspectorRowKind::Reference: {
            Context::DragSourceResult drag = ctx.drag_source(row_id,
                                                             control,
                                                             {.type = row.drag_payload_type,
                                                              .text = row.drag_payload_text.empty() ? row.value_text : row.drag_payload_text,
                                                              .value = row.drag_payload_value},
                                                             widget.z);
            const Context::DropTargetResult drop = ctx.drop_target(make_id(row_id, "drop"), control, row.reference_type, widget.z - 1);
            row.clicked = interactive && drag.interaction.clicked && !drag.dragging && !drag.released;
            if (row.clicked) {
                widget.reference_pick_requested_id = result_id;
            }
            if (drag.started) {
                widget.drag_source_id = result_id;
            }
            if (drop.dropped) {
                widget.drop_target_id = result_id;
            }
            ctx.fill_rect(control, resolved_widget_fill(widget.style, drag.interaction, drop.hot && drop.accepts, interactive));
            ctx.outline_rect(control, resolved_widget_border(widget.style, drag.interaction, false, interactive));
            const Vec2f value_size = measure_text(value_style.font, row.value_text, value_style.scale);
            const f32 value_y = control.y + (control.h - value_size.y) * 0.5f;
            if (row.reference_icon.valid()) {
                const Vec2f icon_size = explicit_or_sprite_size({}, row.reference_icon);
                const Vec2f effective_icon = icon_size.x > 0.0f && icon_size.y > 0.0f ? icon_size : Vec2f{ctx.theme().preset.icon_size, ctx.theme().preset.icon_size};
                ctx.sprite(row.reference_icon, {control.x + metrics.padding.left, control.y + (control.h - effective_icon.y) * 0.5f, effective_icon.x, effective_icon.y});
                ctx.text(row.value_text, {control.x + metrics.padding.left * 2.0f + effective_icon.x, value_y}, value_style);
            } else {
                ctx.text(row.value_text, {control.x + metrics.padding.left, value_y}, value_style);
            }
            break;
        }
        case PropertyInspectorRowKind::Button: {
            Button button{.id = row_id, .bounds = control, .label = row.value_text.empty() ? row.label : row.value_text, .text_style = value_style, .style = widget.style, .enabled = interactive, .z = widget.z};
            run(ctx, button);
            row.clicked = button.clicked;
            if (button.clicked) {
                widget.action_requested_id = result_id;
            }
            break;
        }
        case PropertyInspectorRowKind::Separator:
        case PropertyInspectorRowKind::Section:
            break;
        }

        if (row.changed) {
            widget.changed = true;
            widget.changed_row = i;
            widget.changed_id = result_id;
        }
        if (row.clicked && widget.clicked_id.empty()) {
            widget.clicked_id = result_id;
        }
        if (!row.error.empty()) {
            const Vec2f error_size = measure_text(value_style.font, row.error, value_style.scale);
            ctx.text(row.error, {control.x, row.row_rect.y + row.row_rect.h - error_size.y}, value_style);
        }
    }
    ctx.pop_clip();

    for (DeferredComboPopup& deferred : deferred_combos) {
        run_combo_popup(ctx, deferred.combo);
        if (deferred.state) {
            *deferred.state = deferred.combo.state;
        }
        if (deferred.row && deferred.combo.result.changed) {
            deferred.row->selected = deferred.combo.state.selected;
            deferred.row->changed = true;
            widget.changed = true;
            widget.changed_row = deferred.row_index;
            widget.changed_id = deferred.result_id;
        }
    }
    for (DeferredColorPopup& deferred : deferred_colors) {
        const Context::PopupResult popup = ctx.begin_popup(deferred.id,
                                                           deferred.anchor,
                                                           {.size = measure(deferred.picker),
                                                            .offset = {0.0f, deferred.anchor.h + 2.0f},
                                                            .anchor = UiAnchor::TopLeft,
                                                            .z = widget.z + 10000});
        if (!popup.open) {
            continue;
        }
        deferred.picker.bounds = popup.bounds;
        run(ctx, deferred.picker);
        ctx.end_popup();
        if (deferred.row && deferred.picker.changed) {
            deferred.row->color_value = deferred.picker.value;
            deferred.row->changed = true;
            widget.changed = true;
            widget.changed_row = deferred.row_index;
            widget.changed_id = deferred.result_id;
        }
    }
}

void run(Context& ctx, NodeGraph& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().graph_node);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    widget.clicked_node_id.clear();
    widget.activated_node_id.clear();
    widget.moved_node_id.clear();
    widget.move_delta = {};
    widget.clicked_port_node_id.clear();
    widget.clicked_port_id.clear();
    widget.connect_started_node_id.clear();
    widget.connect_started_port_id.clear();
    widget.connect_preview_node_id.clear();
    widget.connect_preview_port_id.clear();
    widget.connect_from_node_id.clear();
    widget.connect_from_port_id.clear();
    widget.connect_to_node_id.clear();
    widget.connect_to_port_id.clear();
    widget.disconnect_requested_edge_id.clear();
    widget.context_menu_anchor_id.clear();
    widget.selection_changed = false;
    widget.pan_changed = false;
    widget.zoom_changed = false;
    widget.connect_requested = false;
    widget.connect_invalid = false;
    widget.zoom = std::clamp(widget.zoom, widget.min_zoom, widget.max_zoom);

    ctx.fill_rect(widget.bounds, widget.style.track);
    ctx.outline_rect(widget.bounds, widget_border(widget.style, WidgetColorState::Normal));
    ctx.push_clip(widget.bounds);

    const Id graph_id = widget.id ? widget.id : make_id("node_graph");
    const Interaction bg = ctx.region(graph_id, widget.bounds, widget.z);
    if (widget.show_grid && widget.grid_size > 0.0f && widget.zoom > 0.0f) {
        const f32 step = std::max(4.0f, widget.grid_size * widget.zoom);
        const f32 start_x = widget.bounds.x + std::fmod(widget.pan.x, step);
        const f32 start_y = widget.bounds.y + std::fmod(widget.pan.y, step);
        const Color grid = alpha_scaled(widget_border(widget.style, WidgetColorState::Normal), 0.35f);
        for (f32 x = start_x; x < widget.bounds.x + widget.bounds.w; x += step) {
            ctx.line({x, widget.bounds.y}, {x, widget.bounds.y + widget.bounds.h}, grid);
        }
        for (f32 y = start_y; y < widget.bounds.y + widget.bounds.h; y += step) {
            ctx.line({widget.bounds.x, y}, {widget.bounds.x + widget.bounds.w, y}, grid);
        }
    }

    if (contains(widget.bounds, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        const Vec2f before = screen_to_node(widget, ctx.pointer());
        const f32 previous = widget.zoom;
        widget.zoom = std::clamp(widget.zoom + ctx.mouse_wheel_y() * 0.1f, widget.min_zoom, widget.max_zoom);
        if (widget.zoom != previous) {
            const Vec2f after = screen_to_node(widget, ctx.pointer());
            widget.pan.x += (after.x - before.x) * widget.zoom;
            widget.pan.y += (after.y - before.y) * widget.zoom;
            widget.zoom_changed = true;
        }
    }

    if (widget.pan_background && bg.pressed && ctx.pointer_held()) {
        widget.pan_drag_start_pointer = ctx.pointer();
        widget.pan_drag_start = widget.pan;
    }
    if (widget.pan_background && bg.active && ctx.pointer_held()) {
        widget.pan = {widget.pan_drag_start.x + ctx.pointer().x - widget.pan_drag_start_pointer.x,
                      widget.pan_drag_start.y + ctx.pointer().y - widget.pan_drag_start_pointer.y};
        widget.pan_changed = true;
    }

    auto find_node = [&](std::string_view id) -> NodeGraphNode* {
        for (NodeGraphNode& node : widget.nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    };
    auto find_port = [](NodeGraphNode& node, std::string_view id, NodeGraphPortKind kind) -> NodeGraphPort* {
        std::vector<NodeGraphPort>& ports = kind == NodeGraphPortKind::Input ? node.inputs : node.outputs;
        for (NodeGraphPort& port : ports) {
            if (port.id == id) {
                return &port;
            }
        }
        return nullptr;
    };

    for (NodeGraphNode& node : widget.nodes) {
        const f32 input_h = static_cast<f32>(node.inputs.size()) * widget.port_spacing + widget.header_height;
        const f32 output_h = static_cast<f32>(node.outputs.size()) * widget.port_spacing + widget.header_height;
        node.size.x = std::max(node.size.x, widget.node_min_size.x);
        node.size.y = node.collapsed ? widget.header_height : std::max({node.size.y, widget.node_min_size.y, input_h, output_h});
        node.rect = node_rect_to_screen(widget, {node.position.x, node.position.y, node.size.x, node.size.y});
        node.header_rect = {node.rect.x, node.rect.y, node.rect.w, std::min(node.rect.h, widget.header_height * widget.zoom)};
        for (i32 i = 0; i < static_cast<i32>(node.inputs.size()); ++i) {
            NodeGraphPort& port = node.inputs[static_cast<std::size_t>(i)];
            const Vec2f p = node_to_screen(widget, {node.position.x, node.position.y + widget.header_height + static_cast<f32>(i) * widget.port_spacing + widget.port_spacing * 0.5f});
            port.rect = {p.x - widget.port_size.x * 0.5f, p.y - widget.port_size.y * 0.5f, widget.port_size.x, widget.port_size.y};
        }
        for (i32 i = 0; i < static_cast<i32>(node.outputs.size()); ++i) {
            NodeGraphPort& port = node.outputs[static_cast<std::size_t>(i)];
            const Vec2f p = node_to_screen(widget, {node.position.x + node.size.x, node.position.y + widget.header_height + static_cast<f32>(i) * widget.port_spacing + widget.port_spacing * 0.5f});
            port.rect = {p.x - widget.port_size.x * 0.5f, p.y - widget.port_size.y * 0.5f, widget.port_size.x, widget.port_size.y};
        }
    }

    auto draw_edge = [&](Vec2f from, Vec2f to, Color color) {
        const Vec2f mid{(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f};
        ctx.line(from, {mid.x, from.y}, color);
        ctx.line({mid.x, from.y}, {mid.x, to.y}, color);
        ctx.line({mid.x, to.y}, to, color);
        const Vec2f tangent = normalize_or({to.x - mid.x, 0.0f}, {1.0f, 0.0f});
        const Vec2f side{-tangent.y, tangent.x};
        const f32 arrow = 8.0f;
        const Vec2f back{to.x - tangent.x * arrow, to.y - tangent.y * arrow};
        ctx.line(to, {back.x + side.x * 4.0f, back.y + side.y * 4.0f}, color);
        ctx.line(to, {back.x - side.x * 4.0f, back.y - side.y * 4.0f}, color);
    };

    for (i32 i = 0; i < static_cast<i32>(widget.edges.size()); ++i) {
        NodeGraphEdge& edge = widget.edges[static_cast<std::size_t>(i)];
        NodeGraphNode* from_node = find_node(edge.from_node);
        NodeGraphNode* to_node = find_node(edge.to_node);
        if (!from_node || !to_node) {
            continue;
        }
        NodeGraphPort* from_port = find_port(*from_node, edge.from_port, NodeGraphPortKind::Output);
        NodeGraphPort* to_port = find_port(*to_node, edge.to_port, NodeGraphPortKind::Input);
        if (!from_port || !to_port) {
            continue;
        }
        const Vec2f from = rect_center(from_port->rect);
        const Vec2f to = rect_center(to_port->rect);
        edge.hit_rect = normalized_rect(from, to);
        edge.hit_rect.x -= 6.0f;
        edge.hit_rect.y -= 6.0f;
        edge.hit_rect.w += 12.0f;
        edge.hit_rect.h += 12.0f;
        const Id edge_id = make_id(make_id(graph_id, "edge"), edge.id.empty() ? static_cast<u64>(i) : make_id(edge.id).value);
        const Interaction it = ctx.region(edge_id, edge.hit_rect, widget.z + 1);
        if (it.clicked || (contains(edge.hit_rect, ctx.pointer()) && point_segment_distance_sq(ctx.pointer(), from, to) < 36.0f && ctx.pointer_pressed())) {
            widget.selected_edge_id = edge.id;
            widget.selected_node_id.clear();
            widget.selection_changed = true;
        }
        const bool selected = edge.selected || (!edge.id.empty() && widget.selected_edge_id == edge.id);
        draw_edge(from, to, selected ? widget_border(widget.style, WidgetColorState::Focused) : edge.color);
    }

    for (NodeGraphNode& node : widget.nodes) {
        const Id node_id = make_id(make_id(graph_id, "node"), node.id);
        const Interaction body = ctx.region(node_id, node.rect, widget.z + 10);
        if (body.pressed && node.enabled) {
            widget.active_drag_node_id = node.id;
            widget.drag_start_pointer = ctx.pointer();
            widget.drag_start_node_position = node.position;
        }
        if (body.clicked && node.enabled) {
            widget.clicked_node_id = node.id;
            widget.selected_node_id = node.id;
            widget.selected_edge_id.clear();
            widget.selection_changed = true;
        }
        if (body.clicked && ctx.modifier_held(KeyModifiers::Ctrl)) {
            widget.activated_node_id = node.id;
        }
        if (widget.active_drag_node_id == node.id && body.active && ctx.pointer_held()) {
            widget.moved_node_id = node.id;
            widget.move_delta = {(ctx.pointer().x - widget.drag_start_pointer.x) / widget.zoom,
                                 (ctx.pointer().y - widget.drag_start_pointer.y) / widget.zoom};
        }
        if (widget.active_drag_node_id == node.id && ctx.pointer_released()) {
            widget.active_drag_node_id.clear();
        }

        const bool selected = node.selected || (!node.id.empty() && widget.selected_node_id == node.id);
        ctx.fill_rect(node.rect, node.enabled ? widget_fill(widget.style, WidgetColorState::Normal) : widget.style.track);
        // Nodes and ports carry domain colors; widget state only supplies fallback and focus affordances.
        ctx.outline_rect(node.rect, selected ? widget_border(widget.style, WidgetColorState::Focused) : node.color);
        ctx.fill_rect(node.header_rect, body.hot ? widget_fill(widget.style, WidgetColorState::Hovered) : node.color);
        f32 title_x = node.header_rect.x + 8.0f;
        if (node.icon.valid()) {
            ctx.sprite(node.icon, {title_x, node.header_rect.y + 5.0f, 16.0f, 16.0f});
            title_x += 20.0f;
        }
        ctx.text(node.title, {title_x, node.header_rect.y + 5.0f}, widget.text_style);

        auto run_port = [&](NodeGraphPort& port, const std::string& node_id_string) {
            const Id port_id = make_id(make_id(make_id(graph_id, node_id_string), "port"), port.id);
            const Interaction it = ctx.region(port_id, port.rect, widget.z + 20);
            if (it.clicked && port.enabled) {
                widget.clicked_port_node_id = node_id_string;
                widget.clicked_port_id = port.id;
            }
            if (port.kind == NodeGraphPortKind::Output && it.pressed && port.enabled) {
                widget.active_drag_from_node_id = node_id_string;
                widget.active_drag_from_port_id = port.id;
                widget.connect_started_node_id = node_id_string;
                widget.connect_started_port_id = port.id;
            }
            if (!widget.active_drag_from_port_id.empty() && it.hot && port.kind == NodeGraphPortKind::Input) {
                widget.connect_preview_node_id = node_id_string;
                widget.connect_preview_port_id = port.id;
            }
            ctx.fill_rect(port.rect, port.enabled ? port.color : widget_fill(widget.style, WidgetColorState::Disabled));
            ctx.outline_rect(port.rect, it.hot ? widget_border(widget.style, WidgetColorState::Focused) : widget_border(widget.style, WidgetColorState::Normal));
            const Vec2f label_size = measure_text(widget.text_style.font, port.label, widget.text_style.scale);
            const f32 lx = port.kind == NodeGraphPortKind::Input ? port.rect.x + port.rect.w + 6.0f : port.rect.x - label_size.x - 6.0f;
            ctx.text(port.label, {lx, port.rect.y - 2.0f}, widget.text_style);
        };
        if (!node.collapsed) {
            for (NodeGraphPort& port : node.inputs) {
                run_port(port, node.id);
            }
            for (NodeGraphPort& port : node.outputs) {
                run_port(port, node.id);
            }
        }
    }

    if (!widget.active_drag_from_port_id.empty()) {
        NodeGraphNode* from_node = find_node(widget.active_drag_from_node_id);
        NodeGraphPort* from_port = from_node ? find_port(*from_node, widget.active_drag_from_port_id, NodeGraphPortKind::Output) : nullptr;
        if (from_port) {
            draw_edge(rect_center(from_port->rect), ctx.pointer(), widget.style.accent);
        }
        if (ctx.pointer_released()) {
            NodeGraphNode* target_node = find_node(widget.connect_preview_node_id);
            NodeGraphPort* target_port = target_node ? find_port(*target_node, widget.connect_preview_port_id, NodeGraphPortKind::Input) : nullptr;
            if (from_port && target_port && node_graph_ports_compatible(*from_port, *target_port)) {
                widget.connect_requested = true;
                widget.connect_from_node_id = widget.active_drag_from_node_id;
                widget.connect_from_port_id = widget.active_drag_from_port_id;
                widget.connect_to_node_id = widget.connect_preview_node_id;
                widget.connect_to_port_id = widget.connect_preview_port_id;
            } else if (from_port) {
                widget.connect_invalid = true;
            }
            widget.active_drag_from_node_id.clear();
            widget.active_drag_from_port_id.clear();
        }
    }
    if (!widget.selected_edge_id.empty() && (ctx.key_pressed(Key::Delete) || ctx.key_pressed(Key::Backspace))) {
        widget.disconnect_requested_edge_id = widget.selected_edge_id;
    }
    if (bg.clicked) {
        widget.context_menu_anchor_id = "background";
    }
    ctx.pop_clip();
}

void run(Context& ctx, PropertyGrid& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().inspector);
    widget.label_style = themed_text_style(widget.label_style, ctx.theme().body_text);
    widget.value_style = themed_text_style(widget.value_style, ctx.theme().body_text);
    const PropertyGridLayoutMetrics metrics = property_grid_layout_metrics(widget);
    widget.changed = false;
    const i32 count = static_cast<i32>(widget.rows.size());
    const f32 step = metrics.row_height + widget.row_spacing;
    const f32 content_height = property_grid_content_height(widget, metrics);
    ScrollState scroll{
        .offset = {0.0f, widget.offset},
        .content_size = {widget.bounds.w, content_height},
        .viewport_size = {widget.bounds.w, widget.bounds.h},
    };
    clamp_scroll(scroll);
    if (widget.enabled && contains(widget.bounds, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        scroll_by(scroll, {0.0f, -ctx.mouse_wheel_y() * widget.wheel_step});
    }
    widget.offset = scroll.offset.y;

    if (widget.draw_background) {
        ctx.fill_rect(widget.bounds, widget.style.track);
        ctx.outline_rect(widget.bounds, widget_border(widget.style, WidgetColorState::Normal));
    }

    if (count <= 0 || step <= 0.0f) {
        widget.first = 0;
        widget.visible = 0;
        if (count > 0) {
            KIN_LOG_DEBUG_F("ui",
                            "ui2 property grid layout skipped",
                            (LogFields{{.name = "widget", .value = "PropertyGrid"},
                                       {.name = "row", .value = std::to_string(count)},
                                       {.name = "reason", .value = "invalid_row_step"}}));
        }
        return;
    }

    widget.first = std::max(0, static_cast<i32>(std::floor(widget.offset / step)));
    widget.visible = std::max(0, std::min(count - widget.first, static_cast<i32>(std::ceil(widget.bounds.h / std::max(1.0f, step))) + 1));
    const f32 control_x = widget.bounds.x + metrics.label_width + metrics.label_gap;
    const f32 control_w = std::max(0.0f, widget.bounds.x + widget.bounds.w - control_x);
    const f32 control_h = std::min(std::max(0.0f, metrics.control_height), std::max(0.0f, metrics.row_height));
    struct DeferredPropertyGridCombo {
        ComboBox* combo = nullptr;
        PropertyGridRow* row = nullptr;
    };
    std::vector<DeferredPropertyGridCombo> deferred_combos;
    struct DeferredPropertyGridColor {
        Id id{};
        Rectf anchor{};
        PropertyGridRow* row = nullptr;
    };
    std::vector<DeferredPropertyGridColor> deferred_colors;

    for (i32 i = widget.first; i < widget.first + widget.visible && i < count; ++i) {
        PropertyGridRow& row = widget.rows[static_cast<std::size_t>(i)];
        row.changed = false;
        row.clicked = false;
        const f32 row_y = widget.bounds.y + static_cast<f32>(i) * step - widget.offset;
        const Rectf row_rect{widget.bounds.x, row_y, widget.bounds.w, metrics.row_height};
        if (row_rect.y + row_rect.h < widget.bounds.y || row_rect.y > widget.bounds.y + widget.bounds.h) {
            continue;
        }

        const Rectf label_rect{widget.bounds.x + metrics.padding.left, row_y, std::max(0.0f, metrics.label_width - metrics.padding.left), metrics.row_height};
        const Vec2f label_size = measure_text(widget.label_style.font, row.label, widget.label_style.scale);
        ctx.text(row.label, {label_rect.x, label_rect.y + (label_rect.h - label_size.y) * 0.5f}, widget.label_style);

        const Rectf control_rect{control_x, row_y + (metrics.row_height - control_h) * 0.5f, control_w, control_h};
        const Id row_id = property_row_id(widget, row, i);
        switch (row.kind) {
        case PropertyRowKind::Label: {
            const Vec2f value_size = measure_text(widget.value_style.font, row.value_text, widget.value_style.scale);
            ctx.text(row.value_text, {control_rect.x + metrics.padding.left, control_rect.y + (control_rect.h - value_size.y) * 0.5f}, widget.value_style);
            break;
        }
        case PropertyRowKind::Bool: {
            Toggle toggle{
                .id = row_id,
                .bounds = control_rect,
                .label = row.bool_value ? "On" : "Off",
                .text_style = widget.value_style,
                .style = widget.style,
                .value = row.bool_value,
                .enabled = widget.enabled,
            };
            run(ctx, toggle);
            row.bool_value = toggle.value;
            row.changed = toggle.changed;
            break;
        }
        case PropertyRowKind::Number:
            row.number.id = row_id;
            row.number.bounds = control_rect;
            {
                UiTextInputState& state = ctx.text_input_state(make_id(row_id, "number"), format_f32(row.number.value));
                row.number.text = state;
                const bool enabled = row.number.enabled;
                row.number.enabled = widget.enabled && enabled;
                run(ctx, row.number);
                row.number.enabled = enabled;
                state = row.number.text;
            }
            row.changed = row.number.changed;
            break;
        case PropertyRowKind::Text:
            row.text.id = row_id;
            row.text.bounds = control_rect;
            {
                UiTextInputState& state = ctx.text_input_state(row_id, row.text.state.text);
                row.text.state = state;
                const bool enabled = row.text.enabled;
                row.text.enabled = widget.enabled && enabled;
                run(ctx, row.text);
                row.text.enabled = enabled;
                state = row.text.state;
            }
            row.changed = row.text.result.changed || row.text.result.committed || row.text.result.cancelled;
            break;
        case PropertyRowKind::Combo:
            row.combo.id = row_id;
            row.combo.bounds = control_rect;
            {
                UiComboState& state = ctx.combo_state(row_id, row.combo.state.selected);
                row.combo.state = state;
                row.combo.draw_popup = false;
                row.combo.z = widget.z;
                const bool enabled = row.combo.enabled;
                row.combo.enabled = widget.enabled && enabled;
                run(ctx, row.combo);
                row.combo.enabled = enabled;
                state = row.combo.state;
                if (row.combo.state.open) {
                    deferred_combos.push_back({.combo = &row.combo, .row = &row});
                }
            }
            row.changed = row.combo.result.changed;
            break;
        case PropertyRowKind::Color: {
            const Interaction it = ctx.region(row_id, control_rect, widget.z);
            row.clicked = widget.enabled && it.clicked;
            const Id picker_id = make_id(row_id, "picker");
            if (row.clicked) {
                ctx.open_popup(picker_id);
            }
            ctx.fill_rect(control_rect, resolved_widget_fill(widget.style, it, false, widget.enabled));
            ctx.outline_rect(control_rect, resolved_widget_border(widget.style, it, false, widget.enabled));

            const f32 swatch_size = std::max(0.0f, std::min(control_rect.h - metrics.padding.top - metrics.padding.bottom, std::max(16.0f, control_rect.h - metrics.padding.top)));
            const Rectf swatch{control_rect.x + metrics.padding.left, control_rect.y + (control_rect.h - swatch_size) * 0.5f, swatch_size, swatch_size};
            ctx.fill_rect(swatch, row.color_value);
            ctx.outline_rect(swatch, widget_border(widget.style, WidgetColorState::Normal));

            const std::string hex = format_color_hex(row.color_value);
            const Vec2f hex_size = measure_text(widget.value_style.font, hex, widget.value_style.scale);
            ctx.text(hex, {swatch.x + swatch.w + metrics.padding.left, control_rect.y + (control_rect.h - hex_size.y) * 0.5f}, widget.value_style);
            if (ctx.popup_open(picker_id)) {
                deferred_colors.push_back({.id = picker_id, .anchor = control_rect, .row = &row});
            }
            break;
        }
        }
        widget.changed = widget.changed || row.changed;
    }

    for (DeferredPropertyGridCombo& deferred : deferred_combos) {
        if (!deferred.combo) {
            continue;
        }
        run_combo_popup(ctx, *deferred.combo);
        UiComboState& state = ctx.combo_state(deferred.combo->id, deferred.combo->state.selected);
        state = deferred.combo->state;
        if (deferred.row && deferred.combo->result.changed) {
            deferred.row->changed = true;
        }
        widget.changed = widget.changed || deferred.combo->result.changed;
    }
    for (DeferredPropertyGridColor& deferred : deferred_colors) {
        if (!deferred.row) {
            continue;
        }
        PropertyGridRow& row = *deferred.row;
        row.color_picker.id = deferred.id;
        row.color_picker.value = row.color_value;
        row.color_picker.text_style = widget.value_style;
        row.color_picker.style = widget.style;
        row.color_picker.z = 10001;
        const Context::PopupResult popup = ctx.begin_popup(deferred.id,
                                                           deferred.anchor,
                                                           {.size = measure(row.color_picker),
                                                            .offset = {0.0f, deferred.anchor.h + 2.0f},
                                                            .anchor = UiAnchor::TopLeft,
                                                            .z = 10000});
        if (!popup.open) {
            continue;
        }
        row.color_picker.bounds = popup.bounds;
        const bool picker_enabled = row.color_picker.enabled;
        row.color_picker.enabled = widget.enabled && picker_enabled;
        run(ctx, row.color_picker);
        row.color_picker.enabled = picker_enabled;
        row.changed = row.color_picker.changed;
        row.color_value = row.color_picker.value;
        widget.changed = widget.changed || row.changed;
        ctx.end_popup();
    }
}

} // namespace kin::ui2
