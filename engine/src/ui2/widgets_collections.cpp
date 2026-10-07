#include "widget_chunk_preamble.hpp"

namespace kin::ui2 {

struct AssetBrowserLayoutMetrics {
    f32 header_height = 0.0f;
    f32 row_height = 0.0f;
    Vec2f cell_size{};
    UiPadding padding{};
};

AssetBrowserLayoutMetrics asset_browser_layout_metrics(const AssetBrowser& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    const f32 list_row_h = std::max(widget.row_height, text_h + padding.top + padding.bottom);
    const Vec2f cell_min{
        std::max(widget.cell_size.x, text_h * 4.0f + padding.left + padding.right),
        std::max(widget.cell_size.y, text_h * 2.0f + padding.top + padding.bottom),
    };
    return {
        .header_height = widget.mode == AssetBrowserMode::List ? list_row_h : 0.0f,
        .row_height = list_row_h,
        .cell_size = cell_min,
        .padding = padding,
    };
}
Vec2f measure(const AssetBrowser& widget) {
    const AssetBrowserLayoutMetrics metrics = asset_browser_layout_metrics(widget);
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        return {320.0f, 220.0f};
    }
    if (widget.mode == AssetBrowserMode::List) {
        f32 name_w = measure_text(widget.text_style.font, "Name", widget.text_style.scale).x;
        f32 kind_w = measure_text(widget.text_style.font, "Kind", widget.text_style.scale).x;
        f32 modified_w = measure_text(widget.text_style.font, "Modified", widget.text_style.scale).x;
        for (const AssetBrowserItem& item : widget.items) {
            name_w = std::max(name_w, measure_text(widget.text_style.font, item.name, widget.text_style.scale).x);
            kind_w = std::max(kind_w, measure_text(widget.text_style.font, item.kind, widget.text_style.scale).x);
            modified_w = std::max(modified_w, measure_text(widget.text_style.font, item.modified, widget.text_style.scale).x);
        }
        const f32 width = name_w + kind_w + modified_w + metrics.padding.left * 6.0f;
        return {std::max(320.0f, width), metrics.header_height + static_cast<f32>(count) * metrics.row_height};
    }
    const i32 columns = std::max(1, widget.columns > 0 ? widget.columns : std::min(count, 3));
    const i32 rows = (count + columns - 1) / columns;
    return {
        static_cast<f32>(columns) * metrics.cell_size.x + static_cast<f32>(std::max(0, columns - 1)) * widget.spacing.x,
        static_cast<f32>(rows) * metrics.cell_size.y + static_cast<f32>(std::max(0, rows - 1)) * widget.spacing.y,
    };
}

struct StatusBarLayoutMetrics {
    UiPadding padding{};
    f32 height = 0.0f;
    f32 item_gap = 0.0f;
    f32 icon_size = 0.0f;
    f32 progress_height = 0.0f;
};

StatusBarLayoutMetrics status_bar_layout_metrics(const StatusBar& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    return {
        .padding = padding,
        .height = text_h + padding.top + padding.bottom,
        .item_gap = std::max(4.0f, padding.left),
        .icon_size = std::max(0.0f, text_h),
        .progress_height = std::max(2.0f, text_h * 0.35f),
    };
}

f32 status_bar_item_width(const StatusBarItem& item, const TextStyle& text_style, const StatusBarLayoutMetrics& metrics) {
    return metrics.padding.left +
           (item.icon.valid() ? metrics.icon_size + metrics.item_gap : 0.0f) +
           measure_text(text_style.font, item.text, text_style.scale).x +
           metrics.padding.right;
}

Vec2f measure(const StatusBar& widget) {
    const StatusBarLayoutMetrics metrics = status_bar_layout_metrics(widget);
    f32 width = 0.0f;
    for (const StatusBarItem& item : widget.left) {
        width += status_bar_item_width(item, widget.text_style, metrics);
    }
    for (const StatusBarItem& item : widget.right) {
        width += status_bar_item_width(item, widget.text_style, metrics);
    }
    if (widget.progress >= 0.0f) {
        width += std::max(80.0f, metrics.height * 4.0f) + metrics.item_gap * 2.0f;
    }
    if (widget.warnings > 0 || widget.errors > 0) {
        const std::string counters = "W:" + std::to_string(widget.warnings) + " E:" + std::to_string(widget.errors);
        width += measure_text(widget.text_style.font, counters, widget.text_style.scale).x + metrics.padding.left + metrics.padding.right;
    }
    return {std::max(320.0f, width), metrics.height};
}

struct LogConsoleLayoutMetrics {
    f32 row_height = 0.0f;
    f32 toolbar_height = 0.0f;
    f32 button_height = 0.0f;
    f32 button_pad_x = 0.0f;
    f32 button_gap = 0.0f;
    f32 timestamp_width = 0.0f;
    UiPadding padding{};
};

LogConsoleLayoutMetrics log_console_layout_metrics(const LogConsole& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    const f32 copy_h = measure_text(widget.text_style.font, "Copy", widget.text_style.scale).y;
    const f32 clear_h = measure_text(widget.text_style.font, "Clear", widget.text_style.scale).y;
    f32 timestamp_w = measure_text(widget.text_style.font, "00:00", widget.text_style.scale).x;
    for (const LogEntry& entry : widget.entries) {
        timestamp_w = std::max(timestamp_w, measure_text(widget.text_style.font, entry.timestamp, widget.text_style.scale).x);
    }
    const f32 button_h = std::max(text_h, std::max(copy_h, clear_h)) + padding.top + padding.bottom;
    return {
        .row_height = std::max(widget.row_height, text_h + padding.top + padding.bottom),
        .toolbar_height = button_h + padding.top + padding.bottom,
        .button_height = button_h,
        .button_pad_x = padding.left,
        .button_gap = std::max(1.0f, padding.left * 0.5f),
        .timestamp_width = timestamp_w + padding.left + padding.right,
        .padding = padding,
    };
}

Vec2f measure(const LogConsole& widget) {
    if (widget.entries.empty()) {
        return {360.0f, 220.0f};
    }
    const LogConsoleLayoutMetrics metrics = log_console_layout_metrics(widget);
    f32 text_w = 0.0f;
    for (const LogEntry& entry : widget.entries) {
        text_w = std::max(text_w, measure_text(widget.text_style.font, entry.text, widget.text_style.scale).x);
    }
    return {
        std::max(360.0f, metrics.timestamp_width + text_w + metrics.padding.left + metrics.padding.right),
        metrics.toolbar_height + static_cast<f32>(widget.entries.size()) * metrics.row_height,
    };
}

Vec2f measure(const IconGrid& widget) {
    const i32 count = static_cast<i32>(widget.items.size());
    const i32 columns = std::max(1, widget.columns);
    const i32 rows = count > 0 ? (count + columns - 1) / columns : 1;
    Vec2f cell_size = widget.cell_size;
    for (const IconGridItem& item : widget.items) {
        IconSlot slot{
            .icon = item.icon,
            .icon_size = item.icon_size,
            .text = item.text,
            .count = item.count,
            .text_style = widget.text_style,
            .style = widget.style,
        };
        const Vec2f slot_size = measure(slot);
        cell_size.x = std::max(cell_size.x, slot_size.x);
        cell_size.y = std::max(cell_size.y, slot_size.y);
    }
    return {
        static_cast<f32>(columns) * cell_size.x + static_cast<f32>(std::max(0, columns - 1)) * widget.spacing.x,
        static_cast<f32>(rows) * cell_size.y + static_cast<f32>(std::max(0, rows - 1)) * widget.spacing.y,
    };
}

struct ListViewLayoutMetrics {
    f32 row_height = 0.0f;
    UiPadding padding{};
};

ListViewLayoutMetrics list_view_layout_metrics(const ListView& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    return {
        .row_height = std::max(widget.row_height, text_h + padding.top + padding.bottom),
        .padding = padding,
    };
}

Vec2f measure(const ListView& widget) {
    const ListViewLayoutMetrics metrics = list_view_layout_metrics(widget);
    f32 width = 180.0f;
    for (const std::string& item : widget.items) {
        width = std::max(width, measure_text(widget.text_style.font, item, widget.text_style.scale).x + metrics.padding.left + metrics.padding.right);
    }
    const i32 count = static_cast<i32>(widget.items.size());
    const f32 height = count > 0
                           ? static_cast<f32>(count) * metrics.row_height + static_cast<f32>(std::max(0, count - 1)) * widget.row_spacing
                           : 160.0f;
    return {width, height};
}

struct TableLayoutMetrics {
    f32 header_height = 0.0f;
    f32 row_height = 0.0f;
    f32 text_height = 0.0f;
    UiPadding padding{};
};

TableLayoutMetrics table_layout_metrics(const Table& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    const f32 natural_h = text_h + padding.top + padding.bottom;
    return {
        .header_height = widget.draw_header ? std::max(widget.header_height, natural_h) : 0.0f,
        .row_height = std::max(widget.row_height, natural_h),
        .text_height = text_h,
        .padding = padding,
    };
}

f32 table_content_width(const Table& widget) {
    if (widget.columns.empty()) {
        return 260.0f;
    }

    f32 width = 0.0f;
    for (i32 c = 0; c < static_cast<i32>(widget.columns.size()); ++c) {
        const UiTableColumn& column = widget.columns[static_cast<std::size_t>(c)];
        f32 column_w = column.width;
        column_w = std::max(column_w, measure_text(widget.text_style.font, column.label, widget.text_style.scale).x + widget.cell_padding_x * 2.0f);
        for (const std::vector<std::string>& row : widget.rows) {
            if (c < static_cast<i32>(row.size())) {
                column_w = std::max(column_w, measure_text(widget.text_style.font, row[static_cast<std::size_t>(c)], widget.text_style.scale).x + widget.cell_padding_x * 2.0f);
            }
        }
        width += column_w;
    }
    return width;
}

// ---- Shared collection panel scaffolding ------------------------------------------------
// Table/ListView/TreeView share one frame: a filled, shadowed background with the inside
// border drawn LAST (on top of the rows), and a clipped, virtualized row body. Centralizing
// it here stops the list widgets from drifting — e.g. one forgetting to clip its rows, or
// hand-rolling the outline. AssetBrowser/LogConsole have bespoke (grid / toolbar) bodies and
// keep their own layout, but share the same shadow/elevation model via the theme.
// Fills the panel background (border suppressed, shadow kept) and returns the content rect.
// Pair with collection_outline(), which strokes the border on top after the rows are drawn.
CollectionFrame collection_background(Context& ctx, Rectf bounds, const SurfaceStyle& panel) {
    const f32 border = panel.border_mode == BorderMode::Inside ? std::max(0.0f, panel.border_width) : 0.0f;
    SurfaceStyle background = panel;
    background.border_mode = BorderMode::None;
    background.border = colors::transparent;
    ctx.surface(bounds, background);
    return {
        .content = {bounds.x + border,
                    bounds.y + border,
                    std::max(0.0f, bounds.w - border * 2.0f),
                    std::max(0.0f, bounds.h - border * 2.0f)},
        .inner_radius = std::max(0.0f, panel.radius - border),
        .border = border,
    };
}

// Strokes the panel's inside border on top of the rows. Canonical idiom: pass bounds straight
// to ctx.surface with the fill disabled — draw_rounded_rect strokes inward from the edge, so
// the outline sits flush with the background fill (no 1px float, no halo).
void collection_outline(Context& ctx, Rectf bounds, const SurfaceStyle& panel) {
    SurfaceStyle outline = panel;
    outline.draw_fill = false;
    outline.shadow.enabled = false;
    ctx.surface(bounds, outline);
}

// Clipped, virtualized vertical row body. Owns wheel scrolling, offset clamping, the clip, and
// the visible-window math, then calls draw_row(index, cell_rect) for each visible row (cell
// height == step). The single place row clipping lives, so no list can forget it.
template <class DrawRow>
void scrolled_rows(Context& ctx,
                   Rectf body,
                   f32 step,
                   i32 count,
                   f32 wheel_step,
                   bool wheel_enabled,
                   f32& offset,
                   i32& first,
                   i32& visible,
                   std::array<f32, 4> corners, // of the panel the body touches: rows keep inside them
                   DrawRow&& draw_row) {
    ScrollState scroll{
        .offset = {0.0f, offset},
        .content_size = {body.w, static_cast<f32>(std::max(0, count)) * step},
        .viewport_size = {body.w, body.h},
    };
    clamp_scroll(scroll);
    if (wheel_enabled && contains(body, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        scroll_by(scroll, {0.0f, -ctx.mouse_wheel_y() * wheel_step});
    }
    offset = scroll.offset.y;
    first = step > 0.0f ? std::max(0, static_cast<i32>(std::floor(offset / step))) : 0;
    visible = std::max(0, std::min(count - first, static_cast<i32>(std::ceil(body.h / std::max(1.0f, step))) + 1));
    ctx.push_clip(body, corners);
    const bool debug_zebra = ctx.theme().debug_component_tint;
    for (i32 i = first; i < first + visible && i < count; ++i) {
        const Rectf row{body.x, body.y + static_cast<f32>(i) * step - offset, body.w, step};
        // In the debug wireframe theme, paint every row so the zebra is always visible: the fill
        // is recolored by Context to a coherent-hue / per-row-brightness shade (the input color
        // is a placeholder that gets overridden).
        if (debug_zebra) {
            ctx.fill_rect(row, colors::white);
        }
        draw_row(i, row);
    }
    ctx.pop_clip();
}

void draw_table_header_fill(Context& ctx, Rectf header, f32 inner_radius, Color fill) {
    if (fill.a <= 0 || header.w <= 0.0f || header.h <= 0.0f) {
        return;
    }
    if (inner_radius <= 0.0f) {
        ctx.fill_rect(header, fill);
        return;
    }

    ctx.push_clip(header);
    ctx.fill_rounded_rect({header.x, header.y, header.w, header.h + inner_radius}, inner_radius, fill);
    ctx.pop_clip();
}

// Bottom-rounded panel body fill (symmetric to draw_table_header_fill).
// Extends the rounded rect *above* the clip so only the bottom two corners are rounded;
// the top edge stays straight to flush against the header/separator above it.
void draw_panel_body_fill(Context& ctx, Rectf body, f32 inner_radius, Color fill) {
    if (fill.a <= 0 || body.w <= 0.0f || body.h <= 0.0f) {
        return;
    }
    if (inner_radius <= 0.0f) {
        ctx.fill_rect(body, fill);
        return;
    }
    ctx.push_clip(body);
    ctx.fill_rounded_rect({body.x, body.y - inner_radius, body.w, body.h + inner_radius}, inner_radius, fill);
    ctx.pop_clip();
}

void draw_table_row_stripe(Context& ctx, Rectf row, Rectf content, f32 inner_radius, Color fill) {
    if (fill.a <= 0 || row.w <= 0.0f || row.h <= 0.0f) {
        return;
    }
    if (inner_radius <= 0.0f) {
        ctx.fill_rect(row, fill);
        return;
    }

    const f32 content_bottom = content.y + content.h;
    const f32 corner_zone_top = content_bottom - inner_radius;
    const f32 row_bottom = row.y + row.h;
    if (row_bottom <= corner_zone_top) {
        ctx.fill_rect(row, fill);
        return;
    }

    const f32 full_bottom = std::clamp(corner_zone_top, row.y, row_bottom);
    if (full_bottom > row.y) {
        ctx.fill_rect({row.x, row.y, row.w, full_bottom - row.y}, fill);
    }

    const f32 trimmed_y = std::max(row.y, corner_zone_top);
    const f32 trimmed_h = row_bottom - trimmed_y;
    const f32 trim = std::min(inner_radius, row.w * 0.5f);
    if (trimmed_h > 0.0f && row.w > trim * 2.0f) {
        ctx.fill_rect({row.x + trim, trimmed_y, row.w - trim * 2.0f, trimmed_h}, fill);
    }
}

Vec2f measure(const Table& widget) {
    const TableLayoutMetrics metrics = table_layout_metrics(widget);
    const f32 width = table_content_width(widget);
    const f32 height = widget.rows.empty()
                           ? 180.0f
                           : metrics.header_height + static_cast<f32>(widget.rows.size()) * metrics.row_height;
    return {width, height};
}

struct TreeViewLayoutMetrics {
    f32 row_height = 0.0f;
    f32 indent = 0.0f;
    f32 disclosure_width = 0.0f;
    UiPadding padding{};
};

TreeViewLayoutMetrics tree_view_layout_metrics(const TreeView& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    return {
        .row_height = std::max(widget.row_height, text_h + padding.top + padding.bottom),
        .indent = std::max(widget.indent, padding.left + padding.right),
        .disclosure_width = std::max(widget.disclosure_width, text_h),
        .padding = padding,
    };
}

Vec2f measure(const TreeView& widget) {
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        return {220.0f, 180.0f};
    }
    const TreeViewLayoutMetrics metrics = tree_view_layout_metrics(widget);
    f32 width = 0.0f;
    for (const UiTreeItem& item : widget.items) {
        const f32 indent = static_cast<f32>(std::max(0, item.depth)) * metrics.indent;
        width = std::max(width, metrics.padding.left + indent + metrics.disclosure_width + measure_text(widget.text_style.font, item.label, widget.text_style.scale).x + metrics.padding.right);
    }
    return {
        std::max(220.0f, width),
        static_cast<f32>(count) * metrics.row_height + static_cast<f32>(std::max(0, count - 1)) * widget.row_spacing,
    };
}

void run(Context& ctx, AssetBrowser& widget) {
    const auto _draw_scope = ctx.draw_scope("AssetBrowser", widget.bounds, ctx.theme().panel_surface.radius);
    widget.style = themed_widget_style(widget.style, ctx.theme().list_item);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const AssetBrowserLayoutMetrics metrics = asset_browser_layout_metrics(widget);
    widget.activated = -1;
    widget.activated_id.clear();
    widget.clicked = -1;
    widget.clicked_id.clear();
    widget.drag_source = -1;
    widget.drag_started = false;
    widget.sort_requested = AssetSortField::None;
    widget.selection_changed = false;
    // Draw background (shadow + fill, border suppressed). Outline is stroked last via
    // collection_outline() so the header fill drawn next cannot overpaint the corner arcs.
    const CollectionFrame ab_frame = collection_background(ctx, widget.bounds, ctx.theme().panel_surface);
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        collection_outline(ctx, widget.bounds, ctx.theme().panel_surface);
        return;
    }
    const bool list = widget.mode == AssetBrowserMode::List;
    const f32 header_h = list ? metrics.header_height : 0.0f;
    // Viewport and header are placed inside the content rect (inset by the border).
    const Rectf ab_content = ab_frame.content;
    const Rectf viewport{ab_content.x, ab_content.y + header_h, ab_content.w, std::max(0.0f, ab_content.h - header_h)};
    if (list) {
        const Rectf name_h{ab_content.x, ab_content.y, ab_content.w * 0.5f, header_h};
        const Rectf kind_h{ab_content.x + ab_content.w * 0.5f, ab_content.y, ab_content.w * 0.25f, header_h};
        const Rectf mod_h{ab_content.x + ab_content.w * 0.75f, ab_content.y, ab_content.w * 0.25f, header_h};
        if (ctx.region(make_id(widget.id, "sort.name"), name_h, widget.z).clicked) {
            widget.sort_requested = AssetSortField::Name;
        }
        if (ctx.region(make_id(widget.id, "sort.kind"), kind_h, widget.z).clicked) {
            widget.sort_requested = AssetSortField::Kind;
        }
        if (ctx.region(make_id(widget.id, "sort.modified"), mod_h, widget.z).clicked) {
            widget.sort_requested = AssetSortField::Modified;
        }
        // Top-rounded header fill using the clip trick (same as Table) — no shadow, no border.
        const SurfaceStyle& hs = ctx.theme().header_surface;
        if (hs.draw_fill && hs.fill.a > 0) {
            draw_table_header_fill(ctx, {ab_content.x, ab_content.y, ab_content.w, header_h}, ab_frame.inner_radius, hs.fill);
        }
        const Vec2f header_text = measure_text(widget.text_style.font, "Name", widget.text_style.scale);
        const f32 header_y = ab_content.y + (header_h - header_text.y) * 0.5f;
        ctx.text("Name", {name_h.x + metrics.padding.left, header_y}, widget.text_style);
        ctx.text("Kind", {kind_h.x + metrics.padding.left, header_y}, widget.text_style);
        ctx.text("Modified", {mod_h.x + metrics.padding.left, header_y}, widget.text_style);
    }
    const i32 columns = list ? 1 : std::max(1, widget.columns > 0 ? widget.columns : static_cast<i32>(std::max(1.0f, viewport.w / std::max(1.0f, metrics.cell_size.x + widget.spacing.x))));
    const i32 rows = list ? count : (count + columns - 1) / columns;
    const f32 step = list ? metrics.row_height : metrics.cell_size.y + widget.spacing.y;
    const f32 content_h = list ? static_cast<f32>(count) * metrics.row_height : static_cast<f32>(rows) * step;
    ScrollState scroll{.offset = {0.0f, widget.offset}, .content_size = {viewport.w, content_h}, .viewport_size = {viewport.w, viewport.h}};
    clamp_scroll(scroll);
    if (contains(viewport, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        scroll_by(scroll, {0.0f, -ctx.mouse_wheel_y() * widget.wheel_step});
    }
    widget.offset = scroll.offset.y;
    ctx.push_clip(viewport);
    for (i32 i = 0; i < count; ++i) {
        const AssetBrowserItem& item = widget.items[static_cast<std::size_t>(i)];
        const i32 col = list ? 0 : i % columns;
        const i32 row = list ? i : i / columns;
        const Rectf rect = list ? Rectf{viewport.x, viewport.y + static_cast<f32>(i) * metrics.row_height - widget.offset, viewport.w, metrics.row_height}
                                : Rectf{viewport.x + static_cast<f32>(col) * (metrics.cell_size.x + widget.spacing.x),
                                        viewport.y + static_cast<f32>(row) * step - widget.offset,
                                        metrics.cell_size.x,
                                        metrics.cell_size.y};
        if (!overlaps(rect, viewport)) {
            continue;
        }
        const Id item_id = make_id(widget.id, static_cast<u64>(i));
        Interaction it;
        Context::DragSourceResult drag;
        if (widget.drag_enabled && item.enabled) {
            drag = ctx.drag_source(item_id, rect, {.type = widget.drag_payload_type, .text = item.name, .value = static_cast<u64>(i)}, widget.z);
            it = drag.interaction;
            if (drag.started) {
                widget.drag_started = true;
                widget.drag_source = i;
            }
        } else {
            it = ctx.region(item_id, rect, widget.z);
        }
        if (it.clicked && item.enabled && !drag.dragging && !drag.released) {
            widget.clicked = i;
            widget.clicked_id = item_result_id(item.id, item.name);
            widget.selected = i;
            widget.range_anchor = (ctx.modifier_held(KeyModifiers::Shift) && widget.range_anchor >= 0) ? widget.range_anchor : i;
            widget.selection_changed = true;
        }
        if (it.clicked && item.enabled && !ctx.modifier_held(KeyModifiers::Ctrl) && !ctx.modifier_held(KeyModifiers::Shift)) {
            widget.activated = i;
            widget.activated_id = item_result_id(item.id, item.name);
        }
        const bool selected = item.selected || i == widget.selected;
        ctx.surface(rect, ctx.resolve_animated(item_id, widget.style.surface, it, selected, item.enabled));
        TextStyle text_style = widget.text_style;
        if (!item.enabled) {
            text_style.color = widget_fill(widget.style, WidgetColorState::Disabled);
        }
        if (item.icon.valid()) {
            const Vec2f icon_size = explicit_or_sprite_size({}, item.icon);
            const Vec2f effective_icon = icon_size.x > 0.0f && icon_size.y > 0.0f ? icon_size : Vec2f{ctx.theme().preset.icon_size, ctx.theme().preset.icon_size};
            const Rectf icon = list ? Rectf{rect.x + metrics.padding.left, rect.y + (rect.h - effective_icon.y) * 0.5f, effective_icon.x, effective_icon.y}
                                    : align_rect({rect.x, rect.y + metrics.padding.top, rect.w, std::max(0.0f, rect.h - metrics.padding.top - metrics.padding.bottom - measure_text(text_style.font, item.name, text_style.scale).y)}, effective_icon, UiAlign::Center, UiAlign::Center);
            ctx.sprite(item.icon, icon);
        }
        if (list) {
            const Vec2f text = measure_text(text_style.font, item.name, text_style.scale);
            const f32 text_y = rect.y + (rect.h - text.y) * 0.5f;
            const f32 name_x = rect.x + metrics.padding.left + (item.icon.valid() ? ctx.theme().preset.icon_size + metrics.padding.left : 0.0f);
            ctx.text(item.name, {name_x, text_y}, text_style);
            ctx.text(item.kind, {rect.x + rect.w * 0.5f + metrics.padding.left, text_y}, text_style);
            ctx.text(item.modified, {rect.x + rect.w * 0.75f + metrics.padding.left, text_y}, text_style);
        } else {
            const Vec2f text = measure_text(text_style.font, item.name, text_style.scale);
            ctx.text(item.name, {rect.x + (rect.w - text.x) * 0.5f, rect.y + rect.h - text.y - metrics.padding.bottom}, text_style);
        }
    }
    ctx.pop_clip();
    collection_outline(ctx, widget.bounds, ctx.theme().panel_surface);
}

void run(Context& ctx, StatusBar& widget) {
    const auto _draw_scope = ctx.draw_scope("StatusBar", widget.bounds, ctx.theme().subtle_surface.radius);
    widget.style = themed_widget_style(widget.style, ctx.theme().panel);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const StatusBarLayoutMetrics metrics = status_bar_layout_metrics(widget);
    widget.clicked_id.clear();
    SurfaceStyle status_surface = ctx.theme().subtle_surface;
    status_surface.fill = ctx.theme().colors.surface_subtle;
    status_surface.draw_fill = true;
    ctx.surface(widget.bounds, status_surface);
    // Keep all content within the bar: in compact styles the text line-box is a hair taller
    // than the bar, and clipping keeps the painted region honest (only empty line leading is
    // trimmed, never glyph ink).
    ctx.push_clip(widget.bounds);
    auto draw_item = [&](const StatusBarItem& item, Rectf rect) {
        const Interaction it = ctx.region(make_id(widget.id, item.id.empty() ? item.text : item.id), rect, widget.z);
        if (it.clicked && item.enabled) {
            widget.clicked_id = item_result_id(item.id, item.text);
        }
        TextStyle text_style = widget.text_style;
        if (!item.enabled) {
            text_style.color = widget_fill(widget.style, WidgetColorState::Disabled);
        }
        f32 text_x = rect.x + metrics.padding.left;
        if (item.icon.valid()) {
            ctx.sprite(item.icon, {text_x, rect.y + (rect.h - metrics.icon_size) * 0.5f, metrics.icon_size, metrics.icon_size});
            text_x += metrics.icon_size + metrics.item_gap;
        }
        const Vec2f text = measure_text(text_style.font, item.text, text_style.scale);
        ctx.text(item.text, {text_x, rect.y + (rect.h - text.y) * 0.5f}, text_style);
    };
    f32 x = widget.bounds.x;
    for (const StatusBarItem& item : widget.left) {
        const f32 w = status_bar_item_width(item, widget.text_style, metrics);
        draw_item(item, {x, widget.bounds.y, w, widget.bounds.h});
        x += w;
    }
    f32 right_x = widget.bounds.x + widget.bounds.w;
    for (auto it = widget.right.rbegin(); it != widget.right.rend(); ++it) {
        const f32 w = status_bar_item_width(*it, widget.text_style, metrics);
        right_x -= w;
        draw_item(*it, {right_x, widget.bounds.y, w, widget.bounds.h});
    }
    if (widget.warnings > 0 || widget.errors > 0) {
        const std::string counters = "W:" + std::to_string(widget.warnings) + " E:" + std::to_string(widget.errors);
        const Vec2f counters_size = measure_text(widget.text_style.font, counters, widget.text_style.scale);
        const f32 counters_w = counters_size.x + metrics.padding.left + metrics.padding.right;
        right_x -= counters_w;
        ctx.text(counters,
                 {right_x + metrics.padding.left, widget.bounds.y + (widget.bounds.h - counters_size.y) * 0.5f},
                 widget.text_style);
    }
    if (widget.progress >= 0.0f) {
        const Rectf track{std::max(x + metrics.item_gap, widget.bounds.x + metrics.padding.left),
                          widget.bounds.y + (widget.bounds.h - metrics.progress_height) * 0.5f,
                          std::max(0.0f, right_x - x - metrics.item_gap * 2.0f),
                          metrics.progress_height};
        const SurfaceStyle track_surface = with_fill_border(ctx.theme().subtle_surface, ctx.theme().colors.interactive_disabled, colors::transparent);
        ctx.surface(track, track_surface);
        const f32 filled = std::clamp(widget.progress, 0.0f, 1.0f);
        if (filled > 0.0f) {
            // Clip the rounded fill to the progress fraction — same approach as ProgressBar —
            // so the fill's left end is never a square nub poking past the rounded track corners.
            SurfaceStyle fill_surface = with_fill_border(track_surface, widget.style.accent, colors::transparent);
            fill_surface.shadow.enabled = false;
            fill_surface.border_mode = BorderMode::None;
            ctx.push_clip({track.x, track.y, track.w * filled, track.h});
            ctx.surface(track, fill_surface);
            ctx.pop_clip();
        }
    }
    ctx.pop_clip();
}

void run(Context& ctx, LogConsole& widget) {
    const auto _draw_scope = ctx.draw_scope("LogConsole", widget.bounds, ctx.theme().panel_surface.radius);
    widget.style = themed_widget_style(widget.style, ctx.theme().list_item);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const LogConsoleLayoutMetrics metrics = log_console_layout_metrics(widget);
    widget.activated = -1;
    widget.activated_id.clear();
    widget.copy_requested = false;
    widget.clear_requested = false;
    // Draw background (shadow + fill, border suppressed). Outline is stroked last via
    // collection_outline() so the scrollbar fill at the bottom-right cannot overpaint the corner arcs.
    const CollectionFrame frame = collection_background(ctx, widget.bounds, ctx.theme().panel_surface);
    const WidgetStyle toolbar_button = themed_widget_style({}, ctx.theme().button);
    const Vec2f copy_text = measure_text(widget.text_style.font, "Copy", widget.text_style.scale);
    const Vec2f clear_text = measure_text(widget.text_style.font, "Clear", widget.text_style.scale);
    const f32 button_h = metrics.button_height;
    const f32 toolbar_h = metrics.toolbar_height;
    const Rectf toolbar{frame.content.x, frame.content.y, frame.content.w, toolbar_h};
    const f32 clear_w = clear_text.x + metrics.button_pad_x * 2.0f;
    const f32 copy_w = copy_text.x + metrics.button_pad_x * 2.0f;
    const f32 clear_x = toolbar.x + toolbar.w - clear_w - metrics.padding.right;
    const f32 copy_x = clear_x - metrics.button_gap - copy_w;
    const f32 button_y = toolbar.y + (toolbar.h - button_h) * 0.5f;
    const Rectf copy{std::max(toolbar.x + metrics.padding.left, copy_x), button_y, std::max(0.0f, copy_x + copy_w - std::max(toolbar.x + metrics.padding.left, copy_x)), button_h};
    const Rectf clear{std::max(toolbar.x + metrics.padding.left, clear_x), button_y, std::max(0.0f, clear_x + clear_w - std::max(toolbar.x + metrics.padding.left, clear_x)), button_h};
    const Interaction copy_it = ctx.region(make_id(widget.id, "copy"), copy, widget.z);
    if (copy_it.clicked) {
        widget.copy_requested = true;
        if (widget.selected >= 0 && widget.selected < static_cast<i32>(widget.entries.size())) {
            ctx.set_clipboard_text(widget.entries[static_cast<std::size_t>(widget.selected)].text);
        }
    }
    const Interaction clear_it = ctx.region(make_id(widget.id, "clear"), clear, widget.z);
    if (clear_it.clicked) {
        widget.clear_requested = true;
    }
    ctx.surface(copy, ctx.resolve_animated(make_id(widget.id, "copy"), toolbar_button.surface, copy_it));
    ctx.surface(clear, ctx.resolve_animated(make_id(widget.id, "clear"), toolbar_button.surface, clear_it));
    ctx.text("Copy", {copy.x + (copy.w - copy_text.x) * 0.5f, copy.y + (copy.h - copy_text.y) * 0.5f}, widget.text_style);
    ctx.text("Clear", {clear.x + (clear.w - clear_text.x) * 0.5f, clear.y + (clear.h - clear_text.y) * 0.5f}, widget.text_style);
    ctx.fill_rect({toolbar.x + std::max(metrics.padding.left, frame.border),
                   toolbar.y + toolbar.h - ctx.theme().preset.border_width,
                   std::max(0.0f, toolbar.w - std::max(metrics.padding.left, frame.border) - std::max(metrics.padding.right, frame.border)),
                   ctx.theme().preset.border_width},
                  ctx.theme().colors.border);
    const f32 scrollbar_w = ctx.theme().preset.scrollbar_thickness;
    const Rectf viewport{frame.content.x,
                         frame.content.y + toolbar_h,
                         std::max(0.0f, frame.content.w - scrollbar_w),
                         std::max(0.0f, frame.content.h - toolbar_h)};
    const Rectf scrollbar_track{viewport.x + viewport.w,
                                viewport.y,
                                scrollbar_w,
                                viewport.h};
    std::vector<i32> visible_indices;
    for (i32 i = 0; i < static_cast<i32>(widget.entries.size()); ++i) {
        if (asset_matches_filter(widget.entries[static_cast<std::size_t>(i)], widget.filter)) {
            visible_indices.push_back(i);
        }
    }
    const f32 content_h = static_cast<f32>(visible_indices.size()) * metrics.row_height;
    ScrollState scroll{.offset = {0.0f, widget.offset}, .content_size = {viewport.w, content_h}, .viewport_size = {viewport.w, viewport.h}};
    if (widget.auto_scroll) {
        scroll.offset.y = max_scroll_offset(scroll).y;
    }
    ScrollOptions scroll_options{
        .wheel_step = {0.0f, widget.wheel_step},
        .scrollbar_thickness = scrollbar_w,
        .show_scrollbar = true,
        .enabled = true,
    };
    ScrollResult scroll_result = scroll_region(ctx,
                                               widget.id ? make_id(widget.id, "scroll") : make_id("log_console_scroll"),
                                               viewport,
                                               scroll,
                                               scroll_options,
                                               widget.z + 1);
    scroll = scroll_result.state;
    if (scroll_result.consumed_wheel || scroll_result.dragging || scroll_result.changed) {
        widget.auto_scroll = false;
    }
    widget.offset = scroll.offset.y;
    widget.visible = 0;
    ctx.push_clip(viewport);
    for (i32 row = 0; row < static_cast<i32>(visible_indices.size()); ++row) {
        const i32 entry_index = visible_indices[static_cast<std::size_t>(row)];
        const LogEntry& entry = widget.entries[static_cast<std::size_t>(entry_index)];
        const Rectf rect{viewport.x, viewport.y + static_cast<f32>(row) * metrics.row_height - widget.offset, viewport.w, metrics.row_height};
        if (!overlaps(rect, viewport)) {
            continue;
        }
        ++widget.visible;
        const Interaction it = ctx.region(make_id(widget.id, static_cast<u64>(entry_index)), rect, widget.z);
        if (it.clicked) {
            widget.selected = entry_index;
        }
        if (it.clicked && !ctx.modifier_held(KeyModifiers::Ctrl)) {
            widget.activated = entry_index;
            widget.activated_id = item_result_id(entry.id, entry.text);
        }
        if (row % 2 == 0) {
            ctx.fill_rect(rect, ctx.theme().colors.surface_subtle);
        }
        if (rect.y + rect.h < viewport.y + viewport.h) {
            ctx.fill_rect({rect.x + metrics.padding.left,
                           rect.y + rect.h - ctx.theme().preset.border_width,
                           std::max(0.0f, rect.w - metrics.padding.left - metrics.padding.right),
                           ctx.theme().preset.border_width},
                          ctx.theme().colors.border);
        }
        if (it.hot || it.active || it.focused || entry_index == widget.selected) {
            ctx.surface(rect, ctx.resolve_animated(make_id(widget.id, static_cast<u64>(entry_index)), widget.style.surface, it, entry_index == widget.selected));
        }
        TextStyle text_style = widget.text_style;
        text_style.color = severity_color(entry.severity, text_style, ctx.theme());
        const Vec2f text_size = measure_text(text_style.font, entry.text, text_style.scale);
        const f32 text_y = rect.y + (rect.h - text_size.y) * 0.5f;
        ctx.text(entry.timestamp, {rect.x + metrics.padding.left, text_y}, text_style);
        ctx.text(entry.text, {rect.x + metrics.timestamp_width + metrics.padding.left, text_y}, text_style);
    }
    ctx.pop_clip();

    if (content_h > viewport.h && scrollbar_w > 0.0f) {
        // Inset the visual track out of the panel's rounded bottom corner so the square thumb
        // never pokes past the corner arc (a square-over-rounded intrusion the rectangular
        // draw-overflow check can't see, since the thumb still sits inside widget.bounds).
        const f32 corner = std::max(0.0f, frame.inner_radius);
        const Rectf track_visual{scrollbar_track.x, scrollbar_track.y, scrollbar_track.w,
                                 std::max(0.0f, scrollbar_track.h - corner)};
        ctx.fill_rect(track_visual, ctx.theme().colors.interactive_disabled);
        const Rectf thumb = layout_scrollbar(track_visual, scroll, ScrollAxis::Vertical, scroll_options).thumb;
        ctx.fill_rect(thumb, ctx.theme().colors.border_strong);
    }
    // Stroke the outline last so it renders on top of scrollbar fills and corner arcs remain clean.
    collection_outline(ctx, widget.bounds, ctx.theme().panel_surface);
}

void run(Context& ctx, IconGrid& widget) {
    const auto _draw_scope = ctx.draw_scope("IconGrid", widget.bounds, ctx.theme().panel_surface.radius);
    // Item collections fall back to list_item (a tinted, borderless selection),
    // not button — button.selected carries an opaque accent border that the
    // inside-border fill technique would bleed into a solid accent cell.
    widget.style = themed_widget_style(widget.style, ctx.theme().list_item);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    widget.clicked_index = -1;
    widget.activated = -1;
    widget.drag_source = -1;
    widget.dragging = -1;
    widget.drop_target = -1;
    widget.dropped_source = -1;
    widget.dropped_target = -1;
    widget.drag_started = false;
    widget.dropped = false;
    widget.changed = false;

    const i32 count = static_cast<i32>(widget.items.size());
    const SurfaceStyle panel_surface = ctx.theme().panel_surface;
    const CollectionFrame frame = collection_background(ctx, widget.bounds, panel_surface);

    if (count <= 0) {
        widget.selected = 0;
        widget.first = 0;
        widget.visible = 0;
        collection_outline(ctx, widget.bounds, panel_surface);
        return;
    }

    widget.selected = std::clamp(widget.selected, 0, count - 1);
    const i32 previous = widget.selected;
    const i32 columns = std::max(1, widget.columns);
    const i32 rows = (count + columns - 1) / columns;
    IconGrid effective = widget;
    for (const IconGridItem& item : widget.items) {
        IconSlot slot{
            .icon = item.icon,
            .icon_size = item.icon_size,
            .text = item.text,
            .count = item.count,
            .text_style = widget.text_style,
            .style = widget.style,
        };
        const Vec2f slot_size = measure(slot);
        effective.cell_size.x = std::max(effective.cell_size.x, slot_size.x);
        effective.cell_size.y = std::max(effective.cell_size.y, slot_size.y);
    }
    const f32 row_step = effective.cell_size.y + widget.spacing.y;
    const f32 content_height = static_cast<f32>(rows) * effective.cell_size.y + static_cast<f32>(std::max(0, rows - 1)) * widget.spacing.y;
    ScrollState scroll{
        .offset = {0.0f, widget.offset},
        .content_size = {frame.content.w, content_height},
        .viewport_size = {frame.content.w, frame.content.h},
    };
    clamp_scroll(scroll);
    if (widget.enabled && contains(widget.bounds, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        scroll_by(scroll, {0.0f, -ctx.mouse_wheel_y() * widget.wheel_step});
    }
    widget.offset = scroll.offset.y;

    const Context::NavResult nav = ctx.nav_index(count, widget.selected, columns, widget.wrap);
    if (widget.enabled && nav.changed) {
        widget.selected = nav.index;
    }
    if (widget.enabled && nav.activated) {
        widget.activated = widget.selected;
    }

    widget.first = row_step > 0.0f ? std::max(0, static_cast<i32>(std::floor(widget.offset / row_step)) * columns) : 0;
    const i32 visible_rows = std::max(0, static_cast<i32>(std::ceil(frame.content.h / std::max(1.0f, row_step))) + 1);
    widget.visible = std::max(0, std::min(count - widget.first, visible_rows * columns));
    ctx.push_clip(frame.content);
    for (i32 i = widget.first; i < widget.first + widget.visible && i < count; ++i) {
        const IconGridItem& item = widget.items[static_cast<std::size_t>(i)];
        // Offset cells into the content rect so they sit inside the panel border.
        const Rectf raw = icon_grid_cell_rect(effective, i);
        const Rectf cell{raw.x + frame.border, raw.y + frame.border, raw.w, raw.h};

        const Id cell_id = make_id(widget.id, static_cast<u64>(i));
        Interaction it;
        Context::DragSourceResult drag;
        const bool cell_enabled = widget.enabled && item.enabled && !item.locked;
        if (widget.drag_enabled && cell_enabled) {
            drag = ctx.drag_source(cell_id,
                                   cell,
                                   {.type = widget.drag_payload_type, .text = item.text, .value = static_cast<u64>(i)},
                                   widget.z);
            it = drag.interaction;
            if (drag.started) {
                widget.drag_started = true;
                widget.drag_source = i;
            }
            if (drag.dragging) {
                widget.dragging = i;
            }
        } else {
            it = ctx.region(cell_id, cell, widget.z);
        }
        if (widget.drop_enabled) {
            const Context::DropTargetResult drop = ctx.drop_target(cell_id, cell, widget.drop_accept_type, widget.z);
            if (drop.hot && drop.accepts) {
                widget.drop_target = i;
            }
            if (drop.dropped) {
                widget.dropped = true;
                widget.dropped_source = static_cast<i32>(drop.payload.value);
                widget.dropped_target = i;
            }
        }
        if (it.hot && cell_enabled) {
            widget.selected = i;
        }
        if (it.clicked && !drag.dragging && !drag.released && cell_enabled) {
            widget.selected = i;
            widget.clicked_index = i;
            widget.activated = i;
        }

        const bool selected_cell = i == widget.selected || i == widget.drop_target;
        const bool enabled_cell = item.enabled && !item.locked;
        ctx.surface(cell, ctx.resolve_animated(cell_id, widget.style.surface, it, selected_cell, enabled_cell));
        draw_slot_contents(ctx, cell, item.icon, item.icon_size, item.text, item.count, widget.text_style, widget.style, widget.icon_tint);
        if (!item.tooltip.empty()) {
            ctx.tooltip(make_id(cell_id, "tooltip"), cell, item.tooltip);
        }
    }
    ctx.pop_clip();

    const i32 selected_row = widget.selected / columns;
    const f32 selected_top = static_cast<f32>(selected_row) * row_step;
    const f32 selected_bottom = selected_top + effective.cell_size.y;
    scroll.offset.y = widget.offset;
    ensure_visible(scroll, {0.0f, selected_top, frame.content.w, selected_bottom - selected_top});
    widget.offset = scroll.offset.y;
    collection_outline(ctx, widget.bounds, panel_surface);

    widget.changed = widget.selected != previous;
}

void run(Context& ctx, ListView& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    const auto _draw_scope = ctx.draw_scope("ListView", widget.bounds, ctx.theme().panel_surface.radius);
    widget.style = themed_widget_style(widget.style, ctx.theme().list_item);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const ListViewLayoutMetrics metrics = list_view_layout_metrics(widget);
    widget.changed = false;
    widget.activated = false;
    widget.drag_source = -1;
    widget.dragging = -1;
    widget.drop_target = -1;
    widget.dropped_source = -1;
    widget.dropped_target = -1;
    widget.drag_started = false;
    widget.dropped = false;
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        widget.selected = 0;
        widget.first = 0;
        widget.visible = 0;
        ctx.surface(widget.bounds, ctx.theme().panel_surface);
        return;
    }

    widget.selected = std::clamp(widget.selected, 0, count - 1);
    const i32 previous = widget.selected;
    const f32 step = metrics.row_height + widget.row_spacing;
    if (widget.enabled && ctx.action_pressed("menu_up")) {
        widget.selected = widget.selected <= 0 ? (widget.wrap ? count - 1 : 0) : widget.selected - 1;
    }
    if (widget.enabled && ctx.action_pressed("menu_down")) {
        widget.selected = widget.selected + 1 >= count ? (widget.wrap ? 0 : count - 1) : widget.selected + 1;
    }
    if (widget.enabled && ctx.action_pressed("accept")) {
        widget.activated = true;
    }

    const SurfaceStyle panel_surface = ctx.theme().panel_surface;
    const CollectionFrame frame = collection_background(ctx, widget.bounds, panel_surface);

    scrolled_rows(ctx, frame.content, step, count, widget.wheel_step, widget.enabled,
                  widget.offset, widget.first, widget.visible,
                  std::array<f32, 4>{frame.inner_radius, frame.inner_radius, frame.inner_radius, frame.inner_radius},
                  [&](i32 i, Rectf cell) {
                      const Rectf row{cell.x, cell.y, cell.w, metrics.row_height};
                      const Id row_id = make_id(widget.id, static_cast<u64>(i));
                      Interaction it;
                      Context::DragSourceResult drag;
                      if (widget.drag_enabled) {
                          drag = ctx.drag_source(row_id,
                                                 row,
                                                 {.type = widget.drag_payload_type,
                                                  .text = widget.items[static_cast<std::size_t>(i)],
                                                  .value = static_cast<u64>(i)},
                                                 widget.z);
                          it = drag.interaction;
                          if (drag.started) {
                              widget.drag_started = true;
                              widget.drag_source = i;
                          }
                          if (drag.dragging) {
                              widget.dragging = i;
                          }
                      } else {
                          it = ctx.region(row_id, row, widget.z);
                      }
                      if (widget.drop_enabled) {
                          const Context::DropTargetResult drop = ctx.drop_target(row_id, row, widget.drop_accept_type, widget.z);
                          if (drop.hot && drop.accepts) {
                              widget.drop_target = i;
                          }
                          if (drop.dropped) {
                              widget.dropped = true;
                              widget.dropped_source = static_cast<i32>(drop.payload.value);
                              widget.dropped_target = i;
                          }
                      }
                      if (it.clicked && !drag.dragging && !drag.released) {
                          widget.selected = i;
                          widget.activated = true;
                      }
                      const bool selected = i == widget.selected;
                      const bool drop_row = widget.drop_target == i;
                      ctx.surface(row, ctx.resolve_animated(row_id, widget.style.surface, it, drop_row || selected));
                      const Vec2f text_size = measure_text(widget.text_style.font, widget.items[static_cast<std::size_t>(i)], widget.text_style.scale);
                      ctx.text(widget.items[static_cast<std::size_t>(i)],
                               {row.x + metrics.padding.left, row.y + (row.h - text_size.y) * 0.5f},
                               widget.text_style);
                  });

    // Keep the selected row visible (applies next frame, matching the prior behavior).
    ScrollState scroll{.offset = {0.0f, widget.offset},
                       .content_size = {frame.content.w, static_cast<f32>(count) * step},
                       .viewport_size = {frame.content.w, frame.content.h}};
    ensure_visible(scroll, {0.0f, static_cast<f32>(widget.selected) * step, frame.content.w, metrics.row_height});
    widget.offset = scroll.offset.y;

    collection_outline(ctx, widget.bounds, panel_surface);

    widget.changed = widget.selected != previous;
}

void run(Context& ctx, Table& widget) {
    const auto _draw_scope = ctx.draw_scope("Table", widget.bounds, ctx.theme().panel_surface.radius);
    widget.style = themed_widget_style(widget.style, ctx.theme().list_item);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const TableLayoutMetrics metrics = table_layout_metrics(widget);
    const i32 row_count = static_cast<i32>(widget.rows.size());
    const f32 header_h = metrics.header_height;
    const f32 row_h = metrics.row_height;
    const SurfaceStyle panel_surface = ctx.theme().panel_surface;
    const CollectionFrame frame = collection_background(ctx, widget.bounds, panel_surface);
    const Rectf table_content = frame.content;
    const f32 inner_radius = frame.inner_radius;
    const Rectf body{table_content.x, table_content.y + header_h, table_content.w, std::max(0.0f, table_content.h - header_h)};

    f32 fixed = 0.0f;
    i32 stretch_count = 0;
    for (const UiTableColumn& column : widget.columns) {
        if (column.sizing == UiTableColumnSizing::Stretch) {
            ++stretch_count;
        } else {
            fixed += column.width;
        }
    }
    const f32 stretch_w = stretch_count > 0 ? std::max(0.0f, table_content.w - fixed) / static_cast<f32>(stretch_count) : 0.0f;
    std::vector<f32> x;
    std::vector<f32> w;
    f32 cursor = table_content.x;
    for (const UiTableColumn& column : widget.columns) {
        const f32 cw = column.sizing == UiTableColumnSizing::Stretch ? stretch_w : column.width;
        x.push_back(cursor);
        w.push_back(cw);
        cursor += cw;
    }

    if (widget.draw_header) {
        SurfaceStyle header_surface = ctx.theme().header_surface;
        header_surface.shadow.enabled = false;
        header_surface.border_mode = BorderMode::None;
        header_surface.border = colors::transparent;
        if (header_surface.draw_fill && header_surface.fill.a > 0) {
            draw_table_header_fill(ctx, {table_content.x, table_content.y, table_content.w, header_h}, inner_radius, header_surface.fill);
        }
        // Clip header labels to the header strip so a long label in the last column
        // truncates at the table edge instead of spilling into neighbouring panels.
        ctx.push_clip({table_content.x, table_content.y, table_content.w, header_h});
        for (i32 c = 0; c < static_cast<i32>(widget.columns.size()); ++c) {
            const Rectf cell{x[static_cast<std::size_t>(c)], table_content.y, w[static_cast<std::size_t>(c)], header_h};
            TextStyle header_text_style = widget.text_style;
            header_text_style.color = ctx.theme().colors.text_emphasis;
            const Vec2f text_size = measure_text(widget.text_style.font, widget.columns[static_cast<std::size_t>(c)].label, widget.text_style.scale);
            ctx.text(widget.columns[static_cast<std::size_t>(c)].label,
                     {cell.x + widget.cell_padding_x, cell.y + (cell.h - text_size.y) * 0.5f},
                     header_text_style);
        }
        ctx.pop_clip();
    }

    // Below the header: only the bottom corners are the panel's.
    scrolled_rows(ctx, body, row_h, row_count, 48.0f, true, widget.offset, widget.first, widget.visible,
                  std::array<f32, 4>{0.0f, 0.0f, inner_radius, inner_radius},
                  [&](i32 r, Rectf row) {
                      if (r % 2 == 0) {
                          draw_table_row_stripe(ctx, row, table_content, inner_radius, ctx.theme().colors.surface_subtle);
                      }
                      const auto& values = widget.rows[static_cast<std::size_t>(r)];
                      for (i32 c = 0; c < static_cast<i32>(widget.columns.size()) && c < static_cast<i32>(values.size()); ++c) {
                          const Vec2f text_size = measure_text(widget.text_style.font, values[static_cast<std::size_t>(c)], widget.text_style.scale);
                          ctx.text(values[static_cast<std::size_t>(c)],
                                   {x[static_cast<std::size_t>(c)] + widget.cell_padding_x, row.y + (row.h - text_size.y) * 0.5f},
                                   widget.text_style);
                      }
                  });

    collection_outline(ctx, widget.bounds, panel_surface);
}

void run(Context& ctx, TreeView& widget) {
    const auto _draw_scope = ctx.draw_scope("TreeView", widget.bounds, ctx.theme().panel_surface.radius);
    widget.style = themed_widget_style(widget.style, ctx.theme().list_item);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const TreeViewLayoutMetrics metrics = tree_view_layout_metrics(widget);
    widget.changed = false;
    widget.activated = -1;
    widget.toggled = -1;
    widget.drag_source = -1;
    widget.dragging = -1;
    widget.drop_target = -1;
    widget.dropped_source = -1;
    widget.dropped_target = -1;
    widget.drag_started = false;
    widget.dropped = false;
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        widget.selected = 0;
        ctx.surface(widget.bounds, ctx.theme().panel_surface);
        return;
    }
    widget.selected = std::clamp(widget.selected, 0, count - 1);
    const i32 previous = widget.selected;
    const f32 step = metrics.row_height + widget.row_spacing;
    if (widget.enabled && ctx.action_pressed("menu_up")) {
        widget.selected = widget.selected <= 0 ? (widget.wrap ? count - 1 : 0) : widget.selected - 1;
    }
    if (widget.enabled && ctx.action_pressed("menu_down")) {
        widget.selected = widget.selected + 1 >= count ? (widget.wrap ? 0 : count - 1) : widget.selected + 1;
    }
    if (widget.enabled && ctx.action_pressed("accept")) {
        widget.activated = widget.selected;
    }

    const SurfaceStyle panel_surface = ctx.theme().panel_surface;
    const CollectionFrame frame = collection_background(ctx, widget.bounds, panel_surface);

    scrolled_rows(ctx, frame.content, step, count, 48.0f, widget.enabled,
                  widget.offset, widget.first, widget.visible,
                  std::array<f32, 4>{frame.inner_radius, frame.inner_radius, frame.inner_radius, frame.inner_radius},
                  [&](i32 i, Rectf cell) {
                      const UiTreeItem& item = widget.items[static_cast<std::size_t>(i)];
                      const Rectf row{cell.x, cell.y, cell.w, metrics.row_height};
                      const Id row_id = make_id(widget.id, static_cast<u64>(i));
                      Interaction it;
                      Context::DragSourceResult drag;
                      if (widget.drag_enabled) {
                          drag = ctx.drag_source(row_id,
                                                 row,
                                                 {.type = widget.drag_payload_type,
                                                  .text = item.label,
                                                  .value = static_cast<u64>(i)},
                                                 widget.z);
                          it = drag.interaction;
                          if (drag.started) {
                              widget.drag_started = true;
                              widget.drag_source = i;
                          }
                          if (drag.dragging) {
                              widget.dragging = i;
                          }
                      } else {
                          it = ctx.region(row_id, row, widget.z);
                      }
                      if (widget.drop_enabled) {
                          const Context::DropTargetResult drop = ctx.drop_target(row_id, row, widget.drop_accept_type, widget.z);
                          if (drop.hot && drop.accepts) {
                              widget.drop_target = i;
                          }
                          if (drop.dropped) {
                              widget.dropped = true;
                              widget.dropped_source = static_cast<i32>(drop.payload.value);
                              widget.dropped_target = i;
                          }
                      }
                      const f32 indent = static_cast<f32>(std::max(0, item.depth)) * metrics.indent;
                      if (it.clicked && !drag.dragging && !drag.released) {
                          widget.selected = i;
                          if (item.has_children && ctx.pointer().x < row.x + indent + metrics.disclosure_width + metrics.padding.left) {
                              widget.toggled = i;
                          } else {
                              widget.activated = i;
                          }
                      }
                      const bool drop_row = widget.drop_target == i;
                      ctx.surface(row, ctx.resolve_animated(row_id, widget.style.surface, it, drop_row || i == widget.selected));
                      const Vec2f label_size = measure_text(widget.text_style.font, item.label, widget.text_style.scale);
                      const f32 text_y = row.y + (row.h - label_size.y) * 0.5f;
                      if (item.has_children) {
                          const f32 half = std::max(3.0f, label_size.y * 0.22f);
                          const Vec2f c{row.x + metrics.padding.left + indent + half, row.y + row.h * 0.5f};
                          draw_chevron(ctx, c, half, item.expanded ? GlyphDir::Down : GlyphDir::Right, 1.5f,
                                       widget.text_style.color);
                      }
                      ctx.text(item.label, {row.x + metrics.padding.left + indent + metrics.disclosure_width, text_y}, widget.text_style);
                  });

    collection_outline(ctx, widget.bounds, panel_surface);
    widget.changed = widget.selected != previous;
}

} // namespace kin::ui2
