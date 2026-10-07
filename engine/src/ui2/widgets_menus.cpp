#include "widget_chunk_preamble.hpp"

namespace kin::ui2 {

struct MenuListLayoutMetrics {
    UiPadding padding{};
    f32 row_height = 0.0f;
    f32 icon_size = 0.0f;
    f32 check_width = 0.0f;
    f32 label_x = 0.0f;
    f32 prompt_gap = 0.0f;
    f32 arrow_width = 0.0f;
    f32 content_padding = 0.0f;
};

MenuListLayoutMetrics menu_list_layout_metrics(const MenuList& widget, const TextStyle& text_style, const WidgetStyle& style) {
    const UiPadding padding = intrinsic_padding(style);
    const f32 text_h = measure_text(text_style.font, "Mg", text_style.scale).y;
    // Only reserve the check / icon gutters when an item actually uses them, so a
    // plain menu doesn't carry dead left columns (F7 rhythm).
    bool has_check = false;
    bool has_icon = false;
    for (const MenuListItem& item : widget.items) {
        has_check = has_check || item.checked;
        has_icon = has_icon || item.icon.valid();
    }
    const f32 icon = has_icon ? std::max(text_h, 16.0f) : 0.0f;
    const f32 check_w = has_check ? text_h + padding.left : 0.0f;
    const f32 icon_gap = has_icon ? padding.left : 0.0f;
    return {
        .padding = padding,
        .row_height = std::max(widget.row_height, std::max(text_h, icon) + padding.top + padding.bottom),
        .icon_size = icon,
        .check_width = check_w,
        .label_x = padding.left + check_w + icon + icon_gap,
        .prompt_gap = std::max(4.0f, padding.left),
        .arrow_width = measure_text(text_style.font, ">", text_style.scale).x + padding.left,
        .content_padding = std::max(2.0f, std::min(padding.top, std::max(widget.row_height, text_h) * 0.25f)),
    };
}
Vec2f measure(const MenuList& widget) {
    const MenuListLayoutMetrics metrics = menu_list_layout_metrics(widget, widget.text_style, widget.style);
    f32 width = 120.0f;
    for (const MenuListItem& item : widget.items) {
        f32 row_w = metrics.label_x + measure_text(widget.text_style.font, item.label, widget.text_style.scale).x + metrics.padding.right;
        if (!item.prompt.empty()) {
            row_w += metrics.prompt_gap + measure_text(widget.text_style.font, item.prompt, widget.text_style.scale).x;
        }
        if (!item.submenu_id.empty()) {
            row_w += metrics.arrow_width;
        }
        width = std::max(width, row_w + metrics.content_padding * 2.0f);
    }
    const i32 count = static_cast<i32>(widget.items.size());
    const f32 height = count > 0
                           ? static_cast<f32>(count) * metrics.row_height + static_cast<f32>(count - 1) * widget.row_spacing + metrics.content_padding * 2.0f
                           : metrics.row_height + metrics.content_padding * 2.0f;
    return {width, height};
}

Vec2f measure(const MenuBar& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 item_pad_x = std::max(4.0f, padding.left);
    f32 width = padding.left + padding.right;
    f32 height = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y + padding.top + padding.bottom;
    for (const MenuBarItem& item : widget.items) {
        const Vec2f text = measure_text(widget.text_style.font, item.label, widget.text_style.scale);
        width += text.x + item_pad_x * 2.0f + widget.item_gap;
        height = std::max(height, text.y + padding.top + padding.bottom);
    }
    return {std::max(width, 80.0f), std::max(height, 24.0f)};
}

Vec2f measure(const TabBar& widget) {
    const TabBarLayoutMetrics metrics = tab_bar_layout_metrics(widget, widget.text_style, widget.style);
    f32 width = 0.0f;
    const f32 content_pad = std::max(2.0f, std::min(metrics.padding.top, metrics.tab_height * 0.2f));
    const f32 available_w = widget.wrap && widget.bounds.w > 0.0f
                                ? std::max(0.0f, widget.bounds.w - content_pad * 2.0f)
                                : 0.0f;
    i32 rows = 1;
    f32 row_w = 0.0f;
    for (const TabBarItem& item : widget.items) {
        const f32 tab_w = tab_width(widget, item, widget.text_style, metrics);
        if (widget.wrap && available_w > 0.0f) {
            const f32 effective_w = std::min(tab_w, available_w);
            const f32 next_w = row_w <= 0.0f ? effective_w : row_w + widget.gap + effective_w;
            if (row_w > 0.0f && next_w > available_w) {
                width = std::max(width, row_w);
                row_w = effective_w;
                ++rows;
            } else {
                row_w = next_w;
            }
        } else {
            width += tab_w;
        }
    }
    if (widget.wrap && available_w > 0.0f) {
        width = std::max(width, row_w);
    } else {
        width += static_cast<f32>(std::max(0, static_cast<i32>(widget.items.size()) - 1)) * widget.gap;
    }
    const f32 height = static_cast<f32>(std::max(1, rows)) * metrics.tab_height +
                       static_cast<f32>(std::max(0, rows - 1)) * widget.gap +
                       content_pad * 2.0f;
    return {std::max(240.0f, width + content_pad * 2.0f), height};
}

void run(Context& ctx, MenuList& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    widget.activated = -1;
    widget.activated_id.clear();
    widget.hovered = -1;
    widget.hovered_id.clear();
    widget.hovered_rect = {};
    widget.submenu_requested = false;
    widget.submenu_id.clear();
    widget.submenu_anchor = {};
    widget.scroll_changed = false;
    widget.cancelled = false;
    widget.changed = false;
    const WidgetStyle style = themed_widget_style(widget.style, ctx.theme().menu);
    const TextStyle base_text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const MenuListLayoutMetrics metrics = menu_list_layout_metrics(widget, base_text_style, style);
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        ctx.surface(widget.bounds, with_fill_border(ctx.theme().menu_surface, style.track, widget_border(style, WidgetColorState::Normal)));
        return;
    }

    if (widget.selected >= count) {
        widget.selected = count - 1;
    }
    bool selection_normalized = false;
    if (widget.selected >= 0 && !menu_item_selectable(widget.items[static_cast<std::size_t>(widget.selected)])) {
        widget.selected = first_menu_index(widget.items);
        selection_normalized = true;
    }
    const i32 previous = widget.selected;
    if (widget.enabled && ctx.action_pressed("menu_up")) {
        widget.selected = widget.selected < 0 ? next_menu_index(widget.items, 0, -1, widget.wrap)
                                              : next_menu_index(widget.items, widget.selected, -1, widget.wrap);
    }
    if (widget.enabled && ctx.action_pressed("menu_down")) {
        widget.selected = widget.selected < 0 ? first_menu_index(widget.items)
                                              : next_menu_index(widget.items, widget.selected, 1, widget.wrap);
    }
    widget.cancelled = ctx.action_pressed("quit");

    const f32 natural_height = static_cast<f32>(count) * metrics.row_height + static_cast<f32>(std::max(0, count - 1)) * widget.row_spacing;
    const f32 available_pad = std::max(0.0f, (widget.bounds.h - natural_height) * 0.5f);
    const f32 desired_pad = metrics.content_padding;
    const f32 content_pad = std::min(desired_pad, available_pad);
    const bool can_scroll = widget.scrollable && natural_height > widget.bounds.h - content_pad * 2.0f;
    const f32 scrollbar_w = can_scroll && widget.show_scrollbar ? std::max(0.0f, widget.scrollbar_thickness) : 0.0f;
    const Rectf content{widget.bounds.x + content_pad,
                        widget.bounds.y + content_pad,
                        std::max(0.0f, widget.bounds.w - content_pad * 2.0f - scrollbar_w),
                        std::max(0.0f, widget.bounds.h - content_pad * 2.0f)};
    widget.content_height = natural_height;
    widget.viewport_height = content.h;
    const f32 max_scroll = std::max(0.0f, natural_height - content.h);
    const f32 before_scroll = widget.scroll_offset;
    widget.scroll_offset = can_scroll ? std::clamp(widget.scroll_offset, 0.0f, max_scroll) : 0.0f;
    if (can_scroll && widget.enabled && contains(widget.bounds, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        widget.scroll_offset = std::clamp(widget.scroll_offset - ctx.mouse_wheel_y() * widget.wheel_step, 0.0f, max_scroll);
    }
    const f32 step = metrics.row_height + widget.row_spacing;
    if (can_scroll && widget.enabled && widget.selected >= 0 && widget.selected < count && widget.selected != previous) {
        const f32 selected_y = static_cast<f32>(widget.selected) * step;
        if (selected_y < widget.scroll_offset) {
            widget.scroll_offset = selected_y;
        } else if (selected_y + metrics.row_height > widget.scroll_offset + content.h) {
            widget.scroll_offset = selected_y + metrics.row_height - content.h;
        }
        widget.scroll_offset = std::clamp(widget.scroll_offset, 0.0f, max_scroll);
    }
    widget.scroll_changed = widget.scroll_offset != before_scroll;

    ctx.surface(widget.bounds, with_fill_border(ctx.theme().menu_surface, style.track, widget_border(style, WidgetColorState::Normal)));
    if (can_scroll && widget.show_scrollbar && scrollbar_w > 0.0f) {
        const Rectf track{content.x + content.w + std::max(2.0f, content_pad * 0.5f),
                          content.y,
                          std::max(2.0f, scrollbar_w),
                          content.h};
        ctx.surface(track, ctx.theme().scrollbar_track_surface);
        const f32 thumb_h = std::clamp(content.h * (content.h / std::max(content.h, natural_height)), std::min(18.0f, content.h), content.h);
        const f32 travel = std::max(0.0f, content.h - thumb_h);
        const f32 thumb_y = track.y + (max_scroll > 0.0f ? (widget.scroll_offset / max_scroll) * travel : 0.0f);
        ctx.surface({track.x, thumb_y, track.w, thumb_h}, ctx.theme().scrollbar_thumb_surface);
    }
    if (can_scroll) {
        ctx.push_clip(content);
    }
    for (i32 i = 0; i < count; ++i) {
        const MenuListItem& item = widget.items[static_cast<std::size_t>(i)];
        const Rectf row{content.x, content.y + static_cast<f32>(i) * step - widget.scroll_offset, content.w, metrics.row_height};
        const Rectf visible_row = intersect_rect(row, content);
        if (visible_row.w <= 0.0f || visible_row.h <= 0.0f) {
            continue;
        }
        if (item.separator) {
            ctx.fill_rect({row.x + metrics.padding.left, row.y + row.h * 0.5f, std::max(0.0f, row.w - metrics.padding.left - metrics.padding.right), 1.0f}, widget_border(style, WidgetColorState::Normal));
            continue;
        }
        const Id row_id = make_id(widget.id, static_cast<u64>(i));
        const Interaction it = ctx.region(row_id, visible_row, widget.z);
        if (it.hot && item.enabled && widget.enabled) {
            widget.selected = i;
            widget.hovered = i;
            widget.hovered_id = menu_item_result_id(item);
            widget.hovered_rect = row;
        }
        if (widget.selected >= 0 && widget.selected == i && !selection_normalized && !item.submenu_id.empty() && widget.enabled && item.enabled &&
            (it.hot || ctx.action_pressed("menu_right") || ctx.action_pressed("accept"))) {
            widget.submenu_requested = true;
            widget.submenu_id = item.submenu_id;
            widget.submenu_anchor = ctx.to_screen(row);
        }
        if ((it.clicked || (widget.selected >= 0 && !selection_normalized && ctx.action_pressed("accept") && widget.selected == i)) && item.enabled && widget.enabled) {
            if (item.submenu_id.empty()) {
                widget.activated = i;
                widget.activated_id = menu_item_result_id(item);
            } else {
                widget.submenu_requested = true;
                widget.submenu_id = item.submenu_id;
                widget.submenu_anchor = ctx.to_screen(row);
            }
        }
        const bool selected = i == widget.selected;
        SurfaceStyle row_surface = ctx.resolve_animated(row_id, style.surface, it, selected, widget.enabled && item.enabled);
        if (item.default_item && !selected) {
            row_surface.fill = Color::rgba(style.accent.r, style.accent.g, style.accent.b, 42);
        }
        if (item.default_item) {
            row_surface.border = style.accent;
        }
        ctx.surface(row, row_surface);
        if (item.checked) {
            const f32 glyph = measure_text(base_text_style.font, "M", base_text_style.scale).y;
            const Rectf check_box{row.x + metrics.padding.left, row.y + (row.h - glyph) * 0.5f, glyph, glyph};
            draw_check(ctx, check_box, 1.5f, base_text_style.color);
        }
        TextStyle text_style = base_text_style;
        if (!item.enabled) {
            text_style.color = ctx.theme().colors.text_disabled;
        } else if (item.danger) {
            text_style.color = ctx.theme().colors.solid_danger;
        } else if (selected) {
            text_style.color = ctx.theme().colors.text_emphasis;
        }
        if (item.icon.valid()) {
            const Vec2f icon_size{std::min(metrics.icon_size, item.icon.source.w), std::min(metrics.icon_size, item.icon.source.h)};
            ctx.sprite(item.icon, align_rect({row.x + metrics.padding.left + metrics.check_width, row.y, metrics.icon_size, row.h}, icon_size, UiAlign::Center, UiAlign::Center));
        }
        const Vec2f label_size = measure_text(text_style.font, item.label, text_style.scale);
        const f32 label_y = row.y + (row.h - label_size.y) * 0.5f;
        ctx.text(item.label, {row.x + metrics.label_x, label_y}, text_style);
        if (!item.prompt.empty()) {
            const Vec2f prompt = measure_text(text_style.font, item.prompt, text_style.scale);
            const f32 arrow_space = item.submenu_id.empty() ? 0.0f : metrics.arrow_width;
            ctx.report_overflow("MenuListItem", row, {metrics.label_x + label_size.x + prompt.x + metrics.prompt_gap + arrow_space + metrics.padding.right, label_size.y}, item.label);
            ctx.text(item.prompt, {row.x + row.w - prompt.x - metrics.padding.right - arrow_space, row.y + (row.h - prompt.y) * 0.5f}, text_style);
        } else {
            ctx.report_overflow("MenuListItem", row, {metrics.label_x + label_size.x + metrics.padding.right, label_size.y}, item.label);
        }
        if (!item.submenu_id.empty()) {
            const f32 half = std::max(3.0f, measure_text(text_style.font, "M", text_style.scale).y * 0.22f);
            draw_chevron(ctx, {row.x + row.w - metrics.padding.right - half, row.y + row.h * 0.5f},
                         half, GlyphDir::Right, 1.5f, text_style.color);
        }
        if (!item.tooltip.empty()) {
            ctx.tooltip(make_id(row_id, "tooltip"), row, item.tooltip);
        }
    }
    if (can_scroll) {
        ctx.pop_clip();
    }
    widget.changed = widget.selected != previous;
}

void run(Context& ctx, MenuBar& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: menus run from the right
    widget.opened = -1;
    widget.opened_id.clear();
    widget.opened_menu = {};
    widget.changed = false;
    widget.cancelled = ctx.action_pressed("quit");
    const WidgetStyle style = themed_widget_style(widget.style, ctx.theme().menu);
    const TextStyle base_text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        ctx.surface(widget.bounds, with_fill_border(ctx.theme().menu_surface, style.track, widget_border(style, WidgetColorState::Normal)));
        return;
    }

    widget.selected = std::clamp(widget.selected, 0, count - 1);
    const i32 previous = widget.selected;
    const auto next_enabled = [&](i32 current, i32 delta) {
        i32 next = current;
        for (i32 tries = 0; tries < count; ++tries) {
            next = (next + delta + count) % count;
            if (widget.items[static_cast<std::size_t>(next)].enabled) {
                return next;
            }
        }
        return current;
    };
    if (ctx.action_pressed("menu_left")) {
        widget.selected = next_enabled(widget.selected, -1);
    }
    if (ctx.action_pressed("menu_right")) {
        widget.selected = next_enabled(widget.selected, 1);
    }

    ctx.surface(widget.bounds, with_fill_border(ctx.theme().menu_surface, style.track, widget_border(style, WidgetColorState::Normal)));
    const UiPadding padding = intrinsic_padding(style);
    const f32 item_pad_x = std::max(4.0f, padding.left);
    const Rectf content = inset(widget.bounds, padding);
    f32 x = content.x;
    for (i32 i = 0; i < count; ++i) {
        const MenuBarItem& item = widget.items[static_cast<std::size_t>(i)];
        const Vec2f text = measure_text(base_text_style.font, item.label, base_text_style.scale);
        const Rectf row{x, content.y, text.x + item_pad_x * 2.0f, content.h};
        const Id row_id = make_id(widget.id, static_cast<u64>(i));
        const Interaction it = ctx.region(row_id, row, widget.z);
        if (it.hot && item.enabled) {
            widget.selected = i;
        }
        const bool selected = i == widget.selected;
        const bool open = item.menu_id && ctx.popup_open(item.menu_id);
        if ((it.clicked || (selected && ctx.action_pressed("accept")) || (open && (ctx.action_pressed("menu_left") || ctx.action_pressed("menu_right")))) && item.enabled && item.menu_id) {
            ctx.open_popup(item.menu_id);
            widget.opened = i;
            widget.opened_id = item.id.empty() ? item.label : item.id;
            widget.opened_menu = item.menu_id;
        }
        ctx.surface(row, ctx.resolve_animated(row_id, style.surface, it, selected || open, item.enabled));
        TextStyle text_style = base_text_style;
        if (!item.enabled) {
            text_style.color = ctx.theme().colors.text_disabled;
        } else if (selected || open) {
            text_style.color = ctx.theme().colors.text_emphasis;
        }
        ctx.text(item.label, {row.x + item_pad_x, row.y + (row.h - text.y) * 0.5f}, text_style);
        x += row.w + widget.item_gap;
    }
    widget.changed = widget.selected != previous;
}

void run(Context& ctx, TabBar& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    widget.activated = -1;
    widget.activated_id.clear();
    widget.closed = -1;
    widget.closed_id.clear();
    widget.changed = false;
    const i32 count = static_cast<i32>(widget.items.size());
    const WidgetStyle style = themed_widget_style(widget.style, ctx.theme().tab);
    const TextStyle base_text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const TabBarLayoutMetrics metrics = tab_bar_layout_metrics(widget, base_text_style, style);
    ctx.surface(widget.bounds, with_fill_border(ctx.theme().subtle_surface, style.track, colors::transparent));
    if (count <= 0) {
        widget.selected = 0;
        return;
    }
    widget.selected = std::clamp(widget.selected, 0, count - 1);
    const i32 previous = widget.selected;
    if (widget.enabled && ctx.action_pressed("menu_left")) {
        widget.selected = next_enabled_tab(widget, widget.selected, -1);
    }
    if (widget.enabled && ctx.action_pressed("menu_right")) {
        widget.selected = next_enabled_tab(widget, widget.selected, 1);
    }
    if (widget.enabled && ctx.action_pressed("accept") && widget.items[static_cast<std::size_t>(widget.selected)].enabled) {
        widget.activated = widget.selected;
        widget.activated_id = item_result_id(widget.items[static_cast<std::size_t>(widget.selected)].id,
                                             widget.items[static_cast<std::size_t>(widget.selected)].label);
    }

    std::vector<f32> widths;
    widths.reserve(widget.items.size());
    f32 total = 0.0f;
    for (const TabBarItem& item : widget.items) {
        const f32 w = tab_width(widget, item, base_text_style, metrics);
        widths.push_back(w);
        total += w;
    }
    total += static_cast<f32>(std::max(0, count - 1)) * widget.gap;
    const f32 content_pad = std::max(2.0f, std::min(metrics.padding.top, metrics.tab_height * 0.2f));
    const Rectf content{widget.bounds.x + content_pad,
                        widget.bounds.y + content_pad,
                        std::max(0.0f, widget.bounds.w - content_pad * 2.0f),
                        std::max(0.0f, widget.bounds.h - content_pad * 2.0f)};
    if (widget.wrap && content.w > 0.0f) {
        ctx.push_clip(content);
        f32 x = content.x;
        f32 y = content.y;
        for (i32 i = 0; i < count; ++i) {
            const TabBarItem& item = widget.items[static_cast<std::size_t>(i)];
            const f32 tab_w = std::min(widths[static_cast<std::size_t>(i)], content.w);
            if (x > content.x && x + tab_w > content.x + content.w) {
                x = content.x;
                y += metrics.tab_height + widget.gap;
            }
            const Rectf tab{x, y, tab_w, std::min(metrics.tab_height, std::max(0.0f, content.y + content.h - y))};
            if (tab.h <= 0.0f) {
                break;
            }
            const Rectf close_rect{tab.x + tab.w - metrics.padding.right - metrics.close_size,
                                   tab.y + (tab.h - metrics.close_size) * 0.5f,
                                   metrics.close_size,
                                   metrics.close_size};
            const Id tab_id = make_id(widget.id, static_cast<u64>(i));
            const Interaction it = ctx.region(tab_id, tab, widget.z);
            if (it.clicked && widget.enabled && item.enabled) {
                widget.selected = i;
                widget.activated = i;
                widget.activated_id = item_result_id(item.id, item.label);
            }
            if (item.closable) {
                const Interaction close_it = ctx.region(make_id(tab_id, "close"), close_rect, widget.z + 1);
                if (close_it.clicked && widget.enabled && item.enabled) {
                    widget.closed = i;
                    widget.closed_id = item_result_id(item.id, item.label);
                }
            }
            ctx.surface(tab, ctx.resolve_animated(tab_id, style.surface, it, i == widget.selected, widget.enabled && item.enabled));
            TextStyle text_style = base_text_style;
            if (!item.enabled) {
                text_style.color = ctx.theme().colors.text_disabled;
            } else if (i == widget.selected) {
                text_style.color = ctx.theme().colors.text_emphasis;
            }
            f32 tx = tab.x + metrics.padding.left;
            if (item.icon.valid()) {
                ctx.sprite(item.icon, {tx, tab.y + (tab.h - metrics.icon_size) * 0.5f, metrics.icon_size, metrics.icon_size});
                tx += metrics.icon_size + metrics.item_gap;
            }
            const f32 used_right = metrics.padding.right +
                                   (item.closable ? metrics.close_size + metrics.item_gap : 0.0f) +
                                   (item.dirty ? metrics.dirty_size + metrics.item_gap : 0.0f);
            const Vec2f label_size = measure_text(text_style.font, item.label, text_style.scale);
            ctx.report_overflow("TabBarItem", tab, {tx - tab.x + label_size.x + used_right, label_size.y}, item.label);
            ctx.text(item.label, {tx, tab.y + (tab.h - label_size.y) * 0.5f}, text_style);
            if (item.dirty) {
                const f32 dirty_x = item.closable
                                        ? close_rect.x - metrics.item_gap - metrics.dirty_size
                                        : tab.x + tab.w - metrics.padding.right - metrics.dirty_size;
                ctx.fill_rect({dirty_x, tab.y + (tab.h - metrics.dirty_size) * 0.5f, metrics.dirty_size, metrics.dirty_size}, style.accent);
            }
            if (item.closable) {
                const Vec2f close_size = measure_text(text_style.font, "x", text_style.scale);
                ctx.text("x", {close_rect.x + (close_rect.w - close_size.x) * 0.5f, close_rect.y + (close_rect.h - close_size.y) * 0.5f}, text_style);
            }
            x += tab_w + widget.gap;
        }
        ctx.pop_clip();
        widget.changed = widget.selected != previous;
        return;
    }
    if (total > content.w && total <= widget.bounds.w && count > 0) {
        const f32 shrink = (total - content.w) / static_cast<f32>(count);
        total = static_cast<f32>(std::max(0, count - 1)) * widget.gap;
        for (f32& w : widths) {
            w = std::max(36.0f, w - shrink);
            total += w;
        }
    }
    widget.offset = std::clamp(widget.offset, 0.0f, std::max(0.0f, total - content.w));
    if (contains(widget.bounds, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        widget.offset = std::clamp(widget.offset - ctx.mouse_wheel_y() * 48.0f, 0.0f, std::max(0.0f, total - content.w));
    }

    ctx.push_clip(content);
    f32 x = content.x - widget.offset;
    for (i32 i = 0; i < count; ++i) {
        const TabBarItem& item = widget.items[static_cast<std::size_t>(i)];
        const f32 w = widths[static_cast<std::size_t>(i)];
        const Rectf tab{x, content.y, w, std::min(metrics.tab_height, content.h)};
        const Rectf close_rect{tab.x + tab.w - metrics.padding.right - metrics.close_size,
                               tab.y + (tab.h - metrics.close_size) * 0.5f,
                               metrics.close_size,
                               metrics.close_size};
        const Id tab_id = make_id(widget.id, static_cast<u64>(i));
        const Interaction it = ctx.region(tab_id, tab, widget.z);
        if (it.clicked && widget.enabled && item.enabled) {
            widget.selected = i;
            widget.activated = i;
            widget.activated_id = item_result_id(item.id, item.label);
        }
        if (item.closable) {
            const Interaction close_it = ctx.region(make_id(tab_id, "close"), close_rect, widget.z + 1);
            if (close_it.clicked && widget.enabled && item.enabled) {
                widget.closed = i;
                widget.closed_id = item_result_id(item.id, item.label);
            }
        }
        ctx.surface(tab, ctx.resolve_animated(tab_id, style.surface, it, i == widget.selected, widget.enabled && item.enabled));
        TextStyle text_style = base_text_style;
        if (!item.enabled) {
            text_style.color = ctx.theme().colors.text_disabled;
        } else if (i == widget.selected) {
            text_style.color = ctx.theme().colors.text_emphasis;
        }
        f32 tx = tab.x + metrics.padding.left;
        if (item.icon.valid()) {
            ctx.sprite(item.icon, {tx, tab.y + (tab.h - metrics.icon_size) * 0.5f, metrics.icon_size, metrics.icon_size});
            tx += metrics.icon_size + metrics.item_gap;
        }
        const f32 used_right = metrics.padding.right +
                                (item.closable ? metrics.close_size + metrics.item_gap : 0.0f) +
                                (item.dirty ? metrics.dirty_size + metrics.item_gap : 0.0f);
        const Vec2f label_size = measure_text(text_style.font, item.label, text_style.scale);
        ctx.report_overflow("TabBarItem", tab, {tx - tab.x + label_size.x + used_right, label_size.y}, item.label);
        ctx.text(item.label, {tx, tab.y + (tab.h - label_size.y) * 0.5f}, text_style);
        if (item.dirty) {
            const f32 dirty_x = item.closable
                                    ? close_rect.x - metrics.item_gap - metrics.dirty_size
                                    : tab.x + tab.w - metrics.padding.right - metrics.dirty_size;
            ctx.fill_rect({dirty_x, tab.y + (tab.h - metrics.dirty_size) * 0.5f, metrics.dirty_size, metrics.dirty_size}, style.accent);
        }
        if (item.closable) {
            const Vec2f close_size = measure_text(text_style.font, "x", text_style.scale);
            ctx.text("x", {close_rect.x + (close_rect.w - close_size.x) * 0.5f, close_rect.y + (close_rect.h - close_size.y) * 0.5f}, text_style);
        }
        x += w + widget.gap;
    }
    ctx.pop_clip();
    widget.changed = widget.selected != previous;
}

ContextMenuResult popup_menu(Context& ctx, Id id, Rectf anchor, MenuList& menu, PopupMenuOptions options) {
    ContextMenuResult result;
    if (options.size.x <= 0.0f || options.size.y <= 0.0f) {
        options.size = measure(menu);
    }
    const Context::PopupResult popup = ctx.begin_popup(id,
                                                       anchor,
                                                       {.size = options.size,
                                                        .offset = options.offset,
                                                        .anchor = options.anchor,
                                                        .screen = options.screen,
                                                        .z = options.z,
                                                        .close_on_outside_click = options.close_on_outside_click,
                                                        .flip_x = options.flip_x,
                                                        .flip_y = options.flip_y,
                                                        .match_anchor_width = options.match_anchor_width});
    result.open = popup.open;
    result.opened = popup.opened;
    result.closed = popup.closed;
    result.outside_clicked = popup.outside_clicked;
    if (!popup.open) {
        return result;
    }
    ctx.surface(popup.bounds, ctx.theme().popup_surface);
    menu.bounds = popup.bounds;
    run(ctx, menu);
    if (menu.activated >= 0 || menu.cancelled) {
        ctx.close_popup(id);
        result.closed = true;
    }
    result.activated = menu.activated;
    result.activated_id = menu.activated_id;
    result.hovered = menu.hovered;
    result.hovered_id = menu.hovered_id;
    result.submenu_anchor = menu.submenu_anchor;
    result.submenu_requested = menu.submenu_requested;
    result.submenu_id = menu.submenu_id;
    ctx.end_popup();
    return result;
}

ContextMenuResult context_menu(Context& ctx, Id id, Rectf anchor, MenuList& menu) {
    return popup_menu(ctx,
                      id,
                      anchor,
                      menu,
                      {.size = measure(menu),
                       .offset = {0.0f, anchor.h},
                       .anchor = UiAnchor::TopLeft});
}

ContextMenuResult submenu(Context& ctx, Id parent, Id id, Rectf anchor, MenuList& menu) {
    ctx.open_subpopup(parent, id);
    return popup_menu(ctx,
                      id,
                      anchor,
                      menu,
                      {.size = measure(menu),
                       .offset = {anchor.w + 4.0f, 0.0f},
                       .anchor = UiAnchor::TopLeft,
                       .z = 10001});
}

} // namespace kin::ui2
