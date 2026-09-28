#include "widget_chunk_preamble.hpp"

namespace kin::ui2 {

Vec2f measure(const TextInput& widget) {
    const TextInputLayoutMetrics metrics = text_input_layout_metrics(widget.text_style, widget.style);
    const f32 text_w = measure_text(widget.text_style.font, widget.state.text.empty() ? "Text" : widget.state.text, widget.text_style.scale).x;
    return {std::max(160.0f, text_w + metrics.padding.left + metrics.padding.right), metrics.height};
}

Vec2f measure(const NumberInput& widget) {
    const TextInputLayoutMetrics metrics = text_input_layout_metrics(widget.text_style, widget.style);
    const std::string current = widget.text.text.empty() ? format_f32(widget.value) : widget.text.text;
    const std::string min_text = format_f32(widget.min);
    const std::string max_text = format_f32(widget.max);
    f32 text_w = measure_text(widget.text_style.font, current, widget.text_style.scale).x;
    text_w = std::max(text_w, measure_text(widget.text_style.font, min_text, widget.text_style.scale).x);
    text_w = std::max(text_w, measure_text(widget.text_style.font, max_text, widget.text_style.scale).x);
    return {std::max(96.0f, text_w + metrics.padding.left + metrics.padding.right), metrics.height};
}

Vec2f measure(const ComboBox& widget) {
    const TextInputLayoutMetrics metrics = text_input_layout_metrics(widget.text_style, widget.style);
    f32 text_w = 0.0f;
    for (const std::string& item : widget.items) {
        text_w = std::max(text_w, measure_text(widget.text_style.font, item, widget.text_style.scale).x);
    }
    const f32 arrow_w = measure_text(widget.text_style.font, "v", widget.text_style.scale).x + metrics.padding.left;
    return {std::max(140.0f, text_w + arrow_w + metrics.padding.left + metrics.padding.right), metrics.height};
}

struct ColorPickerLayoutMetrics {
    UiPadding padding{};
    f32 tab_height = 0.0f;
    f32 row_height = 0.0f;
    f32 label_width = 0.0f;
    f32 swatch_width = 0.0f;
    f32 picker_size = 0.0f;
    f32 hue_height = 0.0f;
    f32 swatch_size = 0.0f;
    f32 slider_gap = 0.0f;
    f32 marker_width = 0.0f;
};

ColorPickerLayoutMetrics color_picker_layout_metrics(const ColorPicker& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    f32 tab_w = 0.0f;
    const std::array<ColorPickerMode, 3> modes{ColorPickerMode::Rgba, ColorPickerMode::Hsv, ColorPickerMode::Suggested};
    for (ColorPickerMode mode : modes) {
        tab_w = std::max(tab_w, measure_text(widget.text_style.font, color_picker_mode_label(mode), widget.text_style.scale).x);
    }
    const f32 label_w = std::max({widget.label_width,
                                  measure_text(widget.text_style.font, "R", widget.text_style.scale).x,
                                  measure_text(widget.text_style.font, "A", widget.text_style.scale).x});
    return {
        .padding = padding,
        .tab_height = std::max(widget.tab_height, text_h + padding.top + padding.bottom),
        .row_height = std::max(widget.row_height, text_h + padding.top + padding.bottom),
        .label_width = label_w,
        .swatch_width = std::max(widget.swatch_width, text_h * 2.0f),
        .picker_size = std::max(widget.picker_size, text_h * 6.0f),
        .hue_height = std::max(widget.hue_height, std::max(12.0f, text_h + padding.top * 0.5f + padding.bottom * 0.5f)),
        .swatch_size = std::max(widget.swatch_size, text_h + padding.top + padding.bottom),
        .slider_gap = std::max(4.0f, padding.left * 0.5f),
        .marker_width = std::max(6.0f, text_h * 0.35f),
    };
}

f32 color_picker_natural_height(const ColorPicker& widget, const ColorPickerLayoutMetrics& metrics) {
    const f32 rgba_rows = static_cast<f32>(widget.show_alpha ? 5 : 4) * metrics.row_height +
                          static_cast<f32>(widget.show_alpha ? 4 : 3) * widget.row_spacing;
    const f32 hsv_rows = metrics.picker_size + widget.row_spacing + metrics.hue_height +
                         (widget.show_alpha ? widget.row_spacing + metrics.hue_height : 0.0f) +
                         widget.row_spacing + std::max(metrics.row_height, metrics.swatch_size);
    const i32 swatch_rows = 3;
    const f32 suggested_rows = static_cast<f32>(swatch_rows) * metrics.swatch_size + static_cast<f32>(swatch_rows - 1) * widget.swatch_gap +
                               widget.row_spacing + std::max(metrics.row_height, metrics.swatch_size);
    return metrics.padding.top + metrics.tab_height + widget.row_spacing + std::max({rgba_rows, hsv_rows, suggested_rows}) + metrics.padding.bottom;
}

Vec2f measure(const ColorPicker& widget) {
    const ColorPickerLayoutMetrics metrics = color_picker_layout_metrics(widget);
    const f32 hex_w = measure_text(widget.text_style.font, "#FFFFFFFF", widget.text_style.scale).x;
    const f32 tab_w = (measure_text(widget.text_style.font, color_picker_mode_label(ColorPickerMode::Suggested), widget.text_style.scale).x + metrics.padding.left + metrics.padding.right) * 3.0f +
                      widget.tab_gap * 2.0f;
    const f32 slider_w = metrics.label_width + metrics.slider_gap + 120.0f;
    const f32 preview_w = metrics.swatch_width + metrics.padding.left + hex_w + metrics.padding.right;
    return {std::max({260.0f, metrics.padding.left + std::max({metrics.picker_size, tab_w, slider_w, preview_w}) + metrics.padding.right}),
            color_picker_natural_height(widget, metrics)};
}

void run(Context& ctx, TextInput& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().input);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const Interaction it = ctx.region(widget.id, widget.bounds, widget.z);
    widget.interaction = it;
    widget.result = edit_text_input(ctx, widget.state, widget.bounds, widget.text_style, widget.style, it, widget.enabled);
    draw_text_input(ctx, widget.bounds, widget.state, widget.text_style, widget.style, frame_color(widget.style, it, widget.enabled));
}

void run(Context& ctx, NumberInput& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().input);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    if (widget.text.text.empty() && !widget.text.active) {
        widget.text.text = format_f32(widget.value);
        widget.text.caret = widget.text.text.size();
        widget.text.clear_selection();
    }

    const Interaction it = ctx.region(widget.id, widget.bounds, widget.z);
    widget.interaction = it;
    UiTextInputResult result = edit_text_input(ctx, widget.text, widget.bounds, widget.text_style, widget.style, it, widget.enabled);

    f32 parsed = widget.value;
    widget.changed = false;
    if (result.changed && parse_f32(widget.text.text, parsed)) {
        parsed = std::clamp(parsed, widget.min, widget.max);
        widget.changed = parsed != widget.value;
        widget.value = parsed;
    }
    if (widget.enabled && it.focused && (ctx.action_pressed("menu_left") || ctx.key_pressed(Key::Left))) {
        widget.value = std::clamp(widget.value - widget.step, widget.min, widget.max);
        widget.text.text = format_f32(widget.value);
        widget.text.caret = widget.text.text.size();
        widget.text.clear_selection();
        widget.changed = true;
    }
    if (widget.enabled && it.focused && (ctx.action_pressed("menu_right") || ctx.key_pressed(Key::Right))) {
        widget.value = std::clamp(widget.value + widget.step, widget.min, widget.max);
        widget.text.text = format_f32(widget.value);
        widget.text.caret = widget.text.text.size();
        widget.text.clear_selection();
        widget.changed = true;
    }

    draw_text_input(ctx, widget.bounds, widget.text, widget.text_style, widget.style, frame_color(widget.style, it, widget.enabled));
}

void run_combo_popup(Context& ctx, ComboBox& widget) {
    if (!widget.enabled || !widget.state.open) {
        return;
    }
    widget.style = themed_widget_style(widget.style, ctx.theme().input);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const WidgetStyle row_style = themed_widget_style({}, ctx.theme().menu);

    const i32 previous = widget.result.selected;
    const TextInputLayoutMetrics metrics = text_input_layout_metrics(widget.text_style, widget.style);
    const f32 row_h = metrics.height;
    const Context::PopupResult popup = ctx.begin_popup(widget.id,
                                                       widget.bounds,
                                                       {.size = {widget.bounds.w, row_h * static_cast<f32>(widget.items.size())},
                                                        .offset = {0.0f, widget.bounds.h},
                                                        .anchor = UiAnchor::TopLeft,
                                                        .z = widget.z + 100});
    if (!popup.open) {
        widget.state.open = false;
        widget.result.closed = popup.closed;
        return;
    }
    ctx.surface(popup.bounds, ctx.theme().popup_surface);
    for (i32 i = 0; i < static_cast<i32>(widget.items.size()); ++i) {
        const Rectf row{popup.bounds.x, popup.bounds.y + static_cast<f32>(i) * row_h, popup.bounds.w, row_h};
        const Id row_id = make_id(widget.id, static_cast<u64>(i));
        const Interaction row_it = ctx.region(row_id, row, widget.z + 101);
        const bool selected_row = i == widget.state.selected;
        ctx.surface(row, ctx.resolve_animated(row_id, row_style.surface, row_it, selected_row));
        TextStyle row_text_style = widget.text_style;
        if (selected_row) {
            row_text_style.color = ctx.theme().colors.text_emphasis;
        }
        const Vec2f row_text = measure_text(widget.text_style.font, widget.items[static_cast<std::size_t>(i)], widget.text_style.scale);
        ctx.text(widget.items[static_cast<std::size_t>(i)],
                 {row.x + metrics.padding.left, row.y + (row.h - row_text.y) * 0.5f},
                 row_text_style);
        if (row_it.clicked) {
            widget.state.selected = i;
            widget.state.open = false;
            ctx.close_popup(widget.id);
            widget.result.selected = i;
            widget.result.changed = i != previous;
            widget.result.closed = true;
        }
    }
    ctx.end_popup();
}

void run(Context& ctx, ComboBox& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().input);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const TextInputLayoutMetrics metrics = text_input_layout_metrics(widget.text_style, widget.style);
    widget.result = {};
    widget.result.selected = widget.state.selected;
    const Interaction it = ctx.region(widget.id, widget.bounds, widget.z);
    widget.interaction = it;

    if (widget.items.empty()) {
        ctx.fill_rect(widget.bounds, widget_fill(widget.style, WidgetColorState::Disabled));
        ctx.outline_rect(widget.bounds, widget_border(widget.style, WidgetColorState::Normal));
        return;
    }

    widget.state.selected = std::clamp(widget.state.selected, 0, static_cast<i32>(widget.items.size()) - 1);
    widget.result.selected = widget.state.selected;

    if (widget.enabled && it.clicked && !widget.state.open) {
        widget.state.open = true;
        widget.state.menu.selected = widget.state.selected;
        ctx.open_popup(widget.id);
        widget.result.opened = true;
    } else if (widget.enabled && it.clicked && widget.state.open) {
        widget.state.open = false;
        ctx.close_popup(widget.id);
        widget.result.closed = true;
    }
    if (widget.enabled && widget.state.open && ctx.action_pressed("quit")) {
        widget.state.open = false;
        ctx.close_popup(widget.id);
        widget.result.closed = true;
    }

    ctx.fill_rect(widget.bounds, widget.enabled ? widget_fill(widget.style, WidgetColorState::Normal) : widget_fill(widget.style, WidgetColorState::Disabled));
    ctx.outline_rect(widget.bounds, frame_color(widget.style, it, widget.enabled));
    const std::string& selected = widget.items[static_cast<std::size_t>(widget.state.selected)];
    const Vec2f text_size = measure_text(widget.text_style.font, selected, widget.text_style.scale);
    ctx.text(selected, {widget.bounds.x + metrics.padding.left, widget.bounds.y + (widget.bounds.h - text_size.y) * 0.5f}, widget.text_style);
    const f32 chevron_half = std::max(3.0f, measure_text(widget.text_style.font, "M", widget.text_style.scale).y * 0.22f);
    draw_chevron(ctx,
                 {widget.bounds.x + widget.bounds.w - metrics.padding.right - chevron_half,
                  widget.bounds.y + widget.bounds.h * 0.5f},
                 chevron_half, GlyphDir::Down, 1.5f, widget.text_style.color);

    if (widget.draw_popup) {
        run_combo_popup(ctx, widget);
    }
}

void run(Context& ctx, ColorPicker& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().input);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const ColorPickerLayoutMetrics metrics = color_picker_layout_metrics(widget);
    ColorPicker effective = widget;
    effective.row_height = metrics.row_height;
    effective.label_width = metrics.label_width;
    effective.swatch_width = metrics.swatch_width;
    effective.tab_height = metrics.tab_height;
    effective.picker_size = metrics.picker_size;
    effective.hue_height = metrics.hue_height;
    effective.swatch_size = metrics.swatch_size;
    widget.changed = false;
    ColorPickerMode& mode_state = ctx.color_picker_mode(widget.id, widget.mode);
    widget.mode = mode_state;
    ctx.surface(widget.bounds, with_fill_border(ctx.theme().popup_surface, widget.style.track, widget_border(widget.style, WidgetColorState::Normal)));

    Color next = widget.value;
    Rectf content = inset(widget.bounds, metrics.padding);
    const std::array<ColorPickerMode, 3> modes{ColorPickerMode::Rgba, ColorPickerMode::Hsv, ColorPickerMode::Suggested};
    const f32 tab_w = (content.w - widget.tab_gap * 2.0f) / 3.0f;
    for (i32 i = 0; i < static_cast<i32>(modes.size()); ++i) {
        const ColorPickerMode mode = modes[static_cast<std::size_t>(i)];
        const Rectf tab{content.x + static_cast<f32>(i) * (tab_w + widget.tab_gap), content.y, std::max(0.0f, tab_w), metrics.tab_height};
        const Id tab_id = make_id(make_id(widget.id, "tab"), static_cast<u64>(i));
        const Interaction it = ctx.region(tab_id, tab, widget.z + 1);
        if (widget.enabled && it.clicked) {
            mode_state = mode;
            widget.mode = mode;
        }
        const bool selected = widget.mode == mode;
        ctx.surface(tab, ctx.resolve_animated(tab_id, widget.style.surface, it, selected, widget.enabled));
        const std::string label = color_picker_mode_label(mode);
        const Vec2f label_size = measure_text(widget.text_style.font, label, widget.text_style.scale);
        ctx.report_overflow("ColorPickerTab", tab, {label_size.x + metrics.padding.left + metrics.padding.right, label_size.y + metrics.padding.top + metrics.padding.bottom}, label);
        ctx.text(label, {tab.x + (tab.w - label_size.x) * 0.5f, tab.y + (tab.h - label_size.y) * 0.5f}, widget.text_style);
    }

    content.y += metrics.tab_height + widget.row_spacing;
    content.h = std::max(0.0f, content.h - metrics.tab_height - widget.row_spacing);

    auto draw_preview = [&](Rectf row) {
        const Rectf swatch{row.x, row.y + (row.h - std::min(metrics.swatch_width, row.h)) * 0.5f, std::min(metrics.swatch_width, row.w), std::min(metrics.swatch_width, row.h)};
        ctx.surface(swatch, with_fill_border(ctx.theme().input_surface, next, widget_border(widget.style, WidgetColorState::Normal)));
        const std::string hex = format_color_hex(next);
        const Vec2f hex_size = measure_text(widget.text_style.font, hex, widget.text_style.scale);
        ctx.text(hex, {swatch.x + swatch.w + metrics.padding.left, row.y + (row.h - hex_size.y) * 0.5f}, widget.text_style);
    };

    if (widget.mode == ColorPickerMode::Rgba) {
        const f32 step = metrics.row_height + widget.row_spacing;
        i32 row = 0;
        auto row_rect = [&](i32 index) {
            return Rectf{content.x,
                         content.y + static_cast<f32>(index) * step,
                         content.w,
                         metrics.row_height};
        };
        next.r = color_channel_slider(ctx, make_id(widget.id, "r"), row_rect(row++), "R", next.r, effective, widget.changed);
        next.g = color_channel_slider(ctx, make_id(widget.id, "g"), row_rect(row++), "G", next.g, effective, widget.changed);
        next.b = color_channel_slider(ctx, make_id(widget.id, "b"), row_rect(row++), "B", next.b, effective, widget.changed);
        if (widget.show_alpha) {
            next.a = color_channel_slider(ctx, make_id(widget.id, "a"), row_rect(row++), "A", next.a, effective, widget.changed);
        }
        draw_preview(row_rect(row++));
    } else if (widget.mode == ColorPickerMode::Hsv) {
        HsvColor hsv = rgb_to_hsv(next);
        const f32 square_size = std::max(32.0f, std::min({metrics.picker_size, content.w, std::max(0.0f, content.h - metrics.hue_height * (widget.show_alpha ? 2.0f : 1.0f) - widget.row_spacing * 4.0f - metrics.row_height)}));
        const Rectf square{content.x, content.y, square_size, square_size};
        constexpr i32 grid = 16;
        const f32 cell_w = square.w / static_cast<f32>(grid);
        const f32 cell_h = square.h / static_cast<f32>(grid);
        for (i32 y = 0; y < grid; ++y) {
            for (i32 x = 0; x < grid; ++x) {
                const f32 s = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(grid);
                const f32 v = 1.0f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(grid);
                ctx.fill_rect({square.x + static_cast<f32>(x) * cell_w,
                               square.y + static_cast<f32>(y) * cell_h,
                               cell_w + 0.5f,
                               cell_h + 0.5f},
                              hsv_to_rgb(hsv.h, s, v, next.a));
            }
        }
        ctx.outline_rect(square, widget_border(widget.style, WidgetColorState::Normal));
        const Interaction square_it = ctx.region(make_id(widget.id, "sv"), square, widget.z + 1);
        if (widget.enabled && (square_it.active || (square_it.hot && ctx.pointer_pressed()))) {
            const Vec2f p = ctx.pointer();
            hsv.s = std::clamp((p.x - square.x) / std::max(1.0f, square.w), 0.0f, 1.0f);
            hsv.v = std::clamp(1.0f - (p.y - square.y) / std::max(1.0f, square.h), 0.0f, 1.0f);
            const Color updated = hsv_to_rgb(hsv.h, hsv.s, hsv.v, next.a);
            widget.changed = widget.changed || updated.r != next.r || updated.g != next.g || updated.b != next.b;
            next = updated;
        }
        const Vec2f cursor{square.x + hsv.s * square.w, square.y + (1.0f - hsv.v) * square.h};
        ctx.outline_rect({cursor.x - 4.0f, cursor.y - 4.0f, 8.0f, 8.0f}, colors::white);
        ctx.outline_rect({cursor.x - 3.0f, cursor.y - 3.0f, 6.0f, 6.0f}, colors::black);

        Rectf row{content.x, square.y + square.h + widget.row_spacing, content.w, metrics.hue_height};
        for (i32 i = 0; i < 24; ++i) {
            ctx.fill_rect({row.x + row.w * static_cast<f32>(i) / 24.0f, row.y, row.w / 24.0f + 0.5f, row.h}, hsv_to_rgb(static_cast<f32>(i) / 24.0f, 1.0f, 1.0f, next.a));
        }
        ctx.outline_rect(row, widget_border(widget.style, WidgetColorState::Normal));
        const Interaction hue_it = ctx.region(make_id(widget.id, "h"), row, widget.z + 1);
        if (widget.enabled && hue_it.active) {
            const f32 hue = std::clamp((ctx.pointer().x - row.x) / std::max(1.0f, row.w), 0.0f, 1.0f);
            widget.changed = widget.changed || std::abs(hue - hsv.h) > 0.0001f;
            hsv.h = hue;
        }
        const f32 marker_x = row.x + hsv.h * row.w;
        ctx.outline_rect({marker_x - metrics.marker_width * 0.5f, row.y - metrics.padding.top * 0.25f, metrics.marker_width, row.h + metrics.padding.top * 0.5f}, colors::white);
        ctx.outline_rect({marker_x - metrics.marker_width * 0.35f, row.y, metrics.marker_width * 0.7f, row.h}, widget_border(widget.style, WidgetColorState::Normal));
        next = hsv_to_rgb(hsv.h, hsv.s, hsv.v, next.a);

        if (widget.show_alpha) {
            row.y += metrics.hue_height + widget.row_spacing;
            next.a = slider_to_channel(color_float_slider(ctx, make_id(widget.id, "alpha"), row, "A", static_cast<f32>(next.a), 0.0f, 255.0f, effective, widget.changed));
        }
        row.y += metrics.hue_height + widget.row_spacing;
        row.h = std::max(metrics.row_height, metrics.swatch_size);
        draw_preview(row);
    } else {
        const std::vector<Color> suggestions = color_picker_suggestions(widget, ctx.theme());
        const i32 columns = std::max(1, widget.suggested_columns);
        const f32 cell = std::min(metrics.swatch_size, std::max(8.0f, (content.w - static_cast<f32>(columns - 1) * widget.swatch_gap) / static_cast<f32>(columns)));
        for (i32 i = 0; i < static_cast<i32>(suggestions.size()); ++i) {
            const i32 col = i % columns;
            const i32 row = i / columns;
            const Rectf swatch{content.x + static_cast<f32>(col) * (cell + widget.swatch_gap),
                               content.y + static_cast<f32>(row) * (cell + widget.swatch_gap),
                               cell,
                               cell};
            if (swatch.y + swatch.h > content.y + content.h - metrics.row_height - widget.row_spacing) {
                continue;
            }
            const Interaction it = ctx.region(make_id(make_id(widget.id, "suggested"), static_cast<u64>(i)), swatch, widget.z + 1);
            if (widget.enabled && it.clicked) {
                Color picked = suggestions[static_cast<std::size_t>(i)];
                picked.a = next.a;
                widget.changed = widget.changed || picked.r != next.r || picked.g != next.g || picked.b != next.b;
                next = picked;
            }
            ctx.surface(swatch, with_fill_border(ctx.theme().input_surface, suggestions[static_cast<std::size_t>(i)], it.hot ? widget_border(widget.style, WidgetColorState::Focused) : widget_border(widget.style, WidgetColorState::Normal)));
        }
        const Rectf preview{content.x, content.y + content.h - std::max(metrics.row_height, metrics.swatch_size), content.w, std::max(metrics.row_height, metrics.swatch_size)};
        draw_preview(preview);
    }


    widget.value = next;
}

} // namespace kin::ui2
