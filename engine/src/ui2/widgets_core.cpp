#include "widget_chunk_preamble.hpp"

namespace kin::ui2 {
Vec2f max_scroll_offset(const ScrollState& state) {
    return {
        std::max(0.0f, state.content_size.x - state.viewport_size.x),
        std::max(0.0f, state.content_size.y - state.viewport_size.y),
    };
}

bool clamp_scroll(ScrollState& state) {
    const Vec2f before = state.offset;
    const Vec2f max_offset = max_scroll_offset(state);
    state.offset.x = std::clamp(state.offset.x, 0.0f, max_offset.x);
    state.offset.y = std::clamp(state.offset.y, 0.0f, max_offset.y);
    return state.offset.x != before.x || state.offset.y != before.y;
}

bool scroll_by(ScrollState& state, Vec2f delta) {
    const Vec2f before = state.offset;
    state.offset.x += delta.x;
    state.offset.y += delta.y;
    clamp_scroll(state);
    return state.offset.x != before.x || state.offset.y != before.y;
}

bool ensure_visible(ScrollState& state, Rectf item) {
    const Vec2f before = state.offset;
    if (item.x < state.offset.x) {
        state.offset.x = item.x;
    } else if (item.x + item.w > state.offset.x + state.viewport_size.x) {
        state.offset.x = item.x + item.w - state.viewport_size.x;
    }
    if (item.y < state.offset.y) {
        state.offset.y = item.y;
    } else if (item.y + item.h > state.offset.y + state.viewport_size.y) {
        state.offset.y = item.y + item.h - state.viewport_size.y;
    }
    clamp_scroll(state);
    return state.offset.x != before.x || state.offset.y != before.y;
}

i32 first_visible_index(f32 offset, f32 step) {
    return step > 0.0f ? std::max(0, static_cast<i32>(std::floor(offset / step))) : 0;
}

ScrollbarLayout layout_scrollbar(Rectf track, const ScrollState& state, ScrollAxis axis, ScrollOptions options) {
    ScrollbarLayout layout{.track = track, .thumb = track};
    const f32 content = axis_value(state.content_size, axis);
    const f32 viewport = axis_value(state.viewport_size, axis);
    const f32 offset = axis_value(state.offset, axis);
    const f32 track_length = rect_axis_size(track, axis);
    if (content <= 0.0f || viewport <= 0.0f || content <= viewport || track_length <= 0.0f) {
        return layout;
    }
    const f32 max_offset = std::max(0.0f, content - viewport);
    const f32 normalized = max_offset > 0.0f ? std::clamp(offset / max_offset, 0.0f, 1.0f) : 0.0f;
    const f32 thumb_size = std::clamp(track_length * (viewport / content), std::min(options.min_thumb, track_length), track_length);
    const f32 travel = std::max(0.0f, track_length - thumb_size);
    layout.thumb = axis_thumb(track, axis, rect_axis_pos(track, axis) + normalized * travel, thumb_size);
    layout.scrollable = travel > 0.0f;
    return layout;
}

ScrollResult scroll_region(Context& ctx,
                           Id id,
                           Rectf viewport,
                           ScrollState state,
                           ScrollOptions options,
                           i32 z,
                           f32* vertical_grab,
                           f32* horizontal_grab) {
    ScrollResult result{.state = state};
    const Interaction viewport_it = ctx.region(id, viewport, z);
    result.changed = clamp_scroll(result.state);
    if (options.enabled && options.wheel_enabled && contains(viewport, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        result.consumed_wheel = scroll_by(result.state, {0.0f, -ctx.mouse_wheel_y() * options.wheel_step.y});
        result.changed = result.changed || result.consumed_wheel;
    }
    if (options.enabled && viewport_it.focused) {
        bool key_changed = false;
        if (ctx.action_pressed("menu_up") || ctx.key_pressed(Key::Up)) {
            key_changed = scroll_by(result.state, {0.0f, -options.wheel_step.y}) || key_changed;
        }
        if (ctx.action_pressed("menu_down") || ctx.key_pressed(Key::Down)) {
            key_changed = scroll_by(result.state, {0.0f, options.wheel_step.y}) || key_changed;
        }
        if (ctx.action_pressed("menu_left") || ctx.key_pressed(Key::Left)) {
            key_changed = scroll_by(result.state, {-options.wheel_step.x, 0.0f}) || key_changed;
        }
        if (ctx.action_pressed("menu_right") || ctx.key_pressed(Key::Right)) {
            key_changed = scroll_by(result.state, {options.wheel_step.x, 0.0f}) || key_changed;
        }
        if (ctx.key_pressed(Key::Home)) {
            const Vec2f before = result.state.offset;
            result.state.offset = {};
            key_changed = clamp_scroll(result.state) || before.x != result.state.offset.x || before.y != result.state.offset.y || key_changed;
        }
        if (ctx.key_pressed(Key::End)) {
            const Vec2f before = result.state.offset;
            result.state.offset = max_scroll_offset(result.state);
            key_changed = before.x != result.state.offset.x || before.y != result.state.offset.y || key_changed;
        }
        result.changed = result.changed || key_changed;
    }
    const f32 bar = options.show_scrollbar ? options.scrollbar_thickness : 0.0f;
    if (options.show_scrollbar && bar > 0.0f) {
        const Rectf v_track{viewport.x + viewport.w, viewport.y, bar, viewport.h};
        const Rectf h_track{viewport.x, viewport.y + viewport.h, viewport.w, bar};
        ScrollResult v = scroll_axis_interaction(ctx, id, ScrollAxis::Vertical, viewport, v_track, result.state, options, z, vertical_grab);
        result.state = v.state;
        result.changed = result.changed || v.changed;
        result.dragging = result.dragging || v.dragging;
        ScrollResult h = scroll_axis_interaction(ctx, id, ScrollAxis::Horizontal, viewport, h_track, result.state, options, z, horizontal_grab);
        result.state = h.state;
        result.changed = result.changed || h.changed;
        result.dragging = result.dragging || h.dragging;
    }
    return result;
}

AdvancedTextContent parse_advanced_text(std::string_view markup, AdvancedTextMarkupOptions options) {
    AdvancedTextContent content;
    AdvancedTextParseState state{.style = options.base_style};
    std::vector<AdvancedTextParseState> stack;
    std::string text;

    for (std::size_t i = 0; i < markup.size();) {
        if (markup[i] != '[') {
            text.push_back(markup[i++]);
            continue;
        }
        if (i + 1 < markup.size() && markup[i + 1] == '[') {
            text.push_back('[');
            i += 2;
            continue;
        }
        const std::size_t close = markup.find(']', i + 1);
        if (close == std::string_view::npos) {
            text.push_back(markup[i++]);
            continue;
        }

        const std::string_view tag = markup.substr(i + 1, close - i - 1);
        if (starts_with(tag, "icon=")) {
            push_text_run(content, std::move(text), state);
            text.clear();
            const std::string name{tag.substr(5)};
            const Sprite sprite = options.icon_resolver ? options.icon_resolver(name) : Sprite{};
            content.runs.push_back(IconRun{
                .sprite = sprite,
                .size = explicit_or_sprite_size({}, sprite).x > 0.0f ? explicit_or_sprite_size({}, sprite) : Vec2f{16.0f, 16.0f},
                .tint = state.style.color,
                .id = state.id.empty() ? name : state.id,
                .link = state.link,
            });
            i = close + 1;
            continue;
        }

        if (starts_with(tag, "/")) {
            if (!stack.empty()) {
                push_text_run(content, std::move(text), state);
                text.clear();
                state = stack.back();
                stack.pop_back();
                i = close + 1;
                continue;
            }
            text.append(markup.substr(i, close - i + 1));
            i = close + 1;
            continue;
        }

        const std::string required_close = closing_tag_for(tag);
        AdvancedTextParseState next = state;
        if (!required_close.empty() && markup.find(required_close, close + 1) != std::string_view::npos && apply_markup_tag(tag, next, options)) {
            push_text_run(content, std::move(text), state);
            text.clear();
            stack.push_back(state);
            state = std::move(next);
            i = close + 1;
            continue;
        }

        text.append(markup.substr(i, close - i + 1));
        i = close + 1;
    }

    push_text_run(content, std::move(text), state);
    return content;
}

AdvancedTextLayout layout_advanced_text(const AdvancedTextContent& content, AdvancedTextLayoutOptions options) {
    AdvancedTextLayout layout;
    const f32 available_width = options.max_width > 0.0f
        ? std::max(1.0f, options.max_width - options.padding.left - options.padding.right)
        : std::numeric_limits<f32>::max();
    const f32 measured_width = options.max_width > 0.0f ? options.max_width : 0.0f;

    std::vector<std::size_t> line_boxes;
    f32 x = options.padding.left;
    f32 y = options.padding.top;
    f32 line_width = 0.0f;
    f32 line_height = 0.0f;
    bool stopped = false;

    const auto finish_line = [&]() {
        if (line_boxes.empty()) {
            return;
        }
        AdvancedTextLine line;
        line.rect = {options.padding.left, y, line_width, line_height};
        line.boxes = line_boxes;
        layout.lines.push_back(std::move(line));
        layout.measured.x = std::max(layout.measured.x, line_width + options.padding.left + options.padding.right);
        y += line_height + options.line_spacing;
        x = options.padding.left;
        line_width = 0.0f;
        line_height = 0.0f;
        line_boxes.clear();
        if (options.max_lines >= 0 && static_cast<i32>(layout.lines.size()) >= options.max_lines) {
            stopped = true;
        }
    };

    const auto add_box = [&](AdvancedTextRenderBox box, f32 width, f32 height, bool whitespace) {
        if (stopped) {
            return;
        }
        if (box.text == "\n") {
            finish_line();
            return;
        }
        if (options.wrap && !line_boxes.empty() && width > 0.0f && (x - options.padding.left + width) > available_width) {
            finish_line();
        }
        if (stopped) {
            return;
        }
        if (whitespace && line_boxes.empty()) {
            return;
        }
        box.rect = {x, y, width, height};
        const std::size_t index = layout.boxes.size();
        layout.boxes.push_back(std::move(box));
        line_boxes.push_back(index);
        x += width;
        line_width = std::max(line_width, x - options.padding.left);
        line_height = std::max(line_height, height);
    };

    for (const AdvancedTextRun& run : content.runs) {
        if (stopped) {
            break;
        }
        if (const auto* text = std::get_if<TextRun>(&run)) {
            std::string token;
            const auto add_text_piece = [&](const std::string& piece, bool whitespace) {
                const Vec2f size = measure_text(text->style.font, piece, text->style.scale);
                add_box({
                            .text = piece,
                            .style = text->style,
                            .id = text->id,
                            .underline = text->underline,
                            .strike = text->strike,
                            .link = text->link,
                        },
                        size.x,
                        size.y,
                        whitespace);
            };
            const auto flush = [&](bool whitespace) {
                if (token.empty()) {
                    return;
                }
                const Vec2f size = measure_text(text->style.font, token, text->style.scale);
                if (!whitespace && options.wrap && options.break_long_words && size.x > available_width) {
                    for (char ch : token) {
                        add_text_piece(std::string{ch}, false);
                    }
                    token.clear();
                    return;
                }
                add_text_piece(token, whitespace);
                token.clear();
            };
            for (char ch : text->text) {
                if (ch == '\n') {
                    flush(false);
                    add_box({.text = "\n"}, 0.0f, 0.0f, false);
                } else if (ch == ' ' || ch == '\t' || ch == '\r') {
                    flush(false);
                    token = " ";
                    flush(true);
                } else {
                    token.push_back(ch);
                }
            }
            flush(false);
        } else if (const auto* icon = std::get_if<IconRun>(&run)) {
            add_box({
                        .sprite = icon->sprite,
                        .tint = icon->tint,
                        .id = icon->id,
                        .link = icon->link,
                        .icon = true,
                        .vertical = icon->vertical,
                    },
                    icon->size.x,
                    icon->size.y,
                    false);
        }
    }
    finish_line();

    if (layout.lines.empty()) {
        layout.measured = {options.padding.left + options.padding.right, options.padding.top + options.padding.bottom};
        return layout;
    }

    layout.measured.x = measured_width > 0.0f ? measured_width : layout.measured.x;
    layout.measured.y = y - options.line_spacing + options.padding.bottom;
    const f32 content_width = std::max(0.0f, layout.measured.x - options.padding.left - options.padding.right);
    for (AdvancedTextLine& line : layout.lines) {
        const f32 dx = line_align_offset(line.rect.w, content_width, options.horizontal);
        if (dx > 0.0f) {
            line.rect.x += dx;
            for (std::size_t box_index : line.boxes) {
                layout.boxes[box_index].rect.x += dx;
            }
        }
        for (std::size_t box_index : line.boxes) {
            AdvancedTextRenderBox& box = layout.boxes[box_index];
            if (box.icon && box.vertical == UiAlign::Start) {
                box.rect.y += 0.0f;
            } else if (box.icon && box.vertical == UiAlign::End) {
                box.rect.y += std::max(0.0f, line.rect.h - box.rect.h);
            } else {
                box.rect.y += (line.rect.h - box.rect.h) * 0.5f;
            }
            if (box.link || !box.id.empty()) {
                layout.hits.push_back({
                    .rect = box.rect,
                    .id = box.id,
                    .link = box.link,
                    .icon = box.icon,
                });
            }
        }
    }
    return layout;
}

Vec2f measure_advanced_text(const AdvancedTextContent& content, AdvancedTextLayoutOptions options) {
    return layout_advanced_text(content, options).measured;
}

void draw_advanced_text(Context& ctx, const AdvancedTextLayout& layout, Rectf bounds, AdvancedTextLayoutOptions options) {
    const Rectf aligned = align_rect(bounds, layout.measured, options.horizontal, options.vertical);
    if (options.overflow == AdvancedTextOverflow::Clip) {
        ctx.push_clip(bounds);
    }
    for (const AdvancedTextRenderBox& box : layout.boxes) {
        const Rectf rect{aligned.x + box.rect.x, aligned.y + box.rect.y, box.rect.w, box.rect.h};
        if (box.icon) {
            if (box.sprite.valid()) {
                ctx.sprite(box.sprite, rect, box.tint);
            } else {
                ctx.fill_rect(rect, box.tint);
            }
            continue;
        }
        ctx.text(box.text, {rect.x, rect.y}, box.style);
        if (box.underline) {
            ctx.fill_rect({rect.x, rect.y + rect.h - std::max(1.0f, box.style.scale), rect.w, std::max(1.0f, box.style.scale * 0.5f)}, box.style.color);
        }
        if (box.strike) {
            ctx.fill_rect({rect.x, rect.y + rect.h * 0.55f, rect.w, std::max(1.0f, box.style.scale * 0.5f)}, box.style.color);
        }
    }
    if (options.overflow == AdvancedTextOverflow::Clip) {
        ctx.pop_clip();
    }
}

Vec2f measure(const Panel&) {
    return {0.0f, 0.0f};
}

Vec2f measure(const Label& widget) {
    return measure_text(widget.text_style.font, widget.text, widget.text_style.scale);
}

Vec2f measure(const WrappedText& widget) {
    return measure_wrapped_text(widget.text_style.font,
                                widget.text,
                                {.max_width = widget.bounds.w > 0.0f ? widget.bounds.w : 240.0f,
                                 .scale = widget.text_style.scale,
                                 .line_spacing = widget.line_spacing});
}

Vec2f measure(const Separator& widget) {
    return widget.axis == SeparatorAxis::Horizontal ? Vec2f{1.0f, widget.thickness} : Vec2f{widget.thickness, 1.0f};
}

Vec2f measure(const Button& widget) {
    const Vec2f text = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
    const UiPadding padding = intrinsic_padding(widget.style);
    return {
        text.x + padding.left + padding.right,
        // Floor at the shared control height (themed) so a Button lines up with text
        // inputs in a row; raw styles (min_height 0) stay purely text-derived.
        std::max(widget.style.min_height, text.y + padding.top + padding.bottom),
    };
}

Vec2f measure(const Toggle& widget) {
    const Vec2f text = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
    const f32 box = std::max(14.0f, text.y);
    const UiPadding padding = intrinsic_padding(widget.style);
    return {
        box + 8.0f + text.x + padding.left + padding.right,
        box + padding.top + padding.bottom, // box >= text.y by construction
    };
}

struct SliderLayoutMetrics {
    UiPadding padding{};
    f32 track_height = 0.0f;
    f32 handle_width = 0.0f;
    f32 handle_height = 0.0f;
    f32 height = 0.0f;
};

SliderLayoutMetrics slider_layout_metrics(const Slider& widget, const StylePreset& preset = {}) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 preset_track = std::max(4.0f, preset.compact_row_height * 0.5f);
    const f32 track_h = std::max(4.0f, preset_track);
    const f32 handle_h = std::max(track_h, preset.icon_size * 0.75f);
    return {
        .padding = padding,
        .track_height = track_h,
        .handle_width = std::max(4.0f, handle_h * 0.4f),
        .handle_height = handle_h,
        .height = std::max(18.0f, handle_h + padding.top + padding.bottom),
    };
}

Vec2f measure(const Slider& widget) {
    const SliderLayoutMetrics metrics = slider_layout_metrics(widget);
    return {120.0f, metrics.height};
}

struct ProgressBarLayoutMetrics {
    f32 height = 0.0f;
};

ProgressBarLayoutMetrics progress_bar_layout_metrics(const StylePreset& preset = {}) {
    return {
        .height = std::max(14.0f, preset.compact_row_height * 0.5f),
    };
}

Vec2f measure(const ProgressBar&) {
    const ProgressBarLayoutMetrics metrics = progress_bar_layout_metrics();
    return {120.0f, metrics.height};
}

Vec2f measure(const Image& widget) {
    return explicit_or_sprite_size(widget.size, image_sprite(widget));
}

Vec2f measure(const NineSlicePanel& widget) {
    return {widget.skin.left + widget.skin.right, widget.skin.top + widget.skin.bottom};
}

Vec2f measure(const Spacer& widget) {
    return widget.size;
}

Vec2f measure(const IconButton& widget) {
    const Vec2f content = icon_button_content_size(widget);
    const UiPadding padding = intrinsic_padding(widget.style);
    return {
        content.x + padding.left + padding.right,
        content.y + padding.top + padding.bottom,
    };
}

struct IconSlotLayoutMetrics {
    UiPadding padding{};
    Vec2f icon_size{};
    Vec2f text_size{};
    Vec2f badge_size{};
    Vec2f fallback_size{};
};

IconSlotLayoutMetrics icon_slot_layout_metrics(const IconSlot& widget, const StylePreset& preset = {}) {
    const UiPadding padding = intrinsic_padding(widget.style);
    Vec2f icon = explicit_or_sprite_size(widget.icon_size, widget.icon);
    const bool has_icon_size_source = icon.x > 0.0f && icon.y > 0.0f;
    if (icon.x <= 0.0f || icon.y <= 0.0f) {
        icon = {preset.icon_size, preset.icon_size};
    }
    const Vec2f text = widget.text.empty()
                           ? Vec2f{}
                           : measure_text(widget.text_style.font, widget.text, widget.text_style.scale);
    Vec2f badge{};
    if (widget.count > 0) {
        const std::string count = std::to_string(widget.count);
        const Vec2f badge_text = measure_text(widget.text_style.font, count, widget.text_style.scale);
        badge = {badge_text.x + 6.0f, badge_text.y + 4.0f};
    }

    const f32 content_w = std::max({icon.x, text.x, badge.x});
    const f32 content_h = std::max(icon.y + (text.y > 0.0f ? text.y : 0.0f), badge.y);
    return {
        .padding = padding,
        .icon_size = has_icon_size_source ? icon : Vec2f{},
        .text_size = text,
        .badge_size = badge,
        .fallback_size = {
            std::max(48.0f, content_w + padding.left + padding.right),
            std::max(48.0f, content_h + padding.top + padding.bottom),
        },
    };
}

Vec2f measure(const IconSlot& widget) {
    if (widget.size.x > 0.0f && widget.size.y > 0.0f) {
        return widget.size;
    }
    return icon_slot_layout_metrics(widget).fallback_size;
}

struct MeterLayoutMetrics {
    f32 segment_width = 0.0f;
    f32 segment_height = 0.0f;
};

MeterLayoutMetrics meter_layout_metrics(const Meter& widget, const StylePreset& preset = {}) {
    return {
        .segment_width = std::max(4.0f, preset.icon_size),
        .segment_height = std::max(14.0f, preset.compact_row_height * 0.5f),
    };
}

Vec2f measure(const Meter& widget) {
    const MeterLayoutMetrics metrics = meter_layout_metrics(widget);
    const i32 segments = std::max(0, widget.segments);
    if (segments <= 0) {
        return {0.0f, metrics.segment_height};
    }
    const f32 gap = std::max(0.0f, widget.gap);
    return {
        static_cast<f32>(segments) * metrics.segment_width + static_cast<f32>(segments - 1) * gap,
        metrics.segment_height,
    };
}

Vec2f measure(const PromptLabel& widget) {
    const Vec2f prompt = measure_text(widget.prompt_style.font, widget.prompt, widget.prompt_style.scale);
    const Vec2f text = measure_text(widget.text_style.font, widget.text, widget.text_style.scale);
    const Vec2f icon = explicit_or_sprite_size(widget.icon_size, widget.icon);
    Vec2f prompt_box = prompt;
    if (!widget.prompt.empty() && widget.show_chip) {
        prompt_box.x += widget.chip_padding.left + widget.chip_padding.right;
        prompt_box.y += widget.chip_padding.top + widget.chip_padding.bottom;
    }
    if (widget.icon.valid()) {
        prompt_box.x += (prompt_box.x > 0.0f ? widget.gap * 0.5f : 0.0f) + icon.x;
        prompt_box.y = std::max(prompt_box.y, icon.y);
    }
    const f32 separator_w = (!widget.prompt.empty() && !widget.text.empty() && !widget.separator.empty())
                                ? measure_text(widget.text_style.font, widget.separator, widget.text_style.scale).x
                                : 0.0f;
    const f32 gap = (!widget.prompt.empty() && !widget.text.empty()) ? widget.gap : 0.0f;
    return {
        widget.padding.left + prompt_box.x + separator_w + gap + text.x + widget.padding.right,
        widget.padding.top + std::max(prompt_box.y, text.y) + widget.padding.bottom,
    };
}

Vec2f measure(const PromptRow& widget) {
    Vec2f size{};
    for (i32 i = 0; i < static_cast<i32>(widget.items.size()); ++i) {
        const PromptRowItem& item = widget.items[static_cast<std::size_t>(i)];
        PromptLabel label = widget.item_style;
        label.prompt = item.prompt.empty() ? item.action : item.prompt;
        label.text = item.text;
        label.icon = item.icon;
        label.prompt_options = widget.prompt_options;
        if (default_text_style(label.prompt_style)) {
            label.prompt_style = widget.prompt_style;
        }
        if (default_text_style(label.text_style)) {
            label.text_style = widget.text_style;
        }
        const Vec2f item_size = measure(label);
        if (widget.axis == UiLayoutAxis::Horizontal) {
            size.x += item_size.x + (i > 0 ? widget.spacing : 0.0f);
            size.y = std::max(size.y, item_size.y);
        } else {
            size.x = std::max(size.x, item_size.x);
            size.y += item_size.y + (i > 0 ? widget.spacing : 0.0f);
        }
    }
    return {widget.padding.left + size.x + widget.padding.right, widget.padding.top + size.y + widget.padding.bottom};
}

Vec2f measure(const DialogBox& widget) {
    AdvancedTextLayoutOptions title_options = widget.title_layout;
    AdvancedTextLayoutOptions body_options = widget.body_layout;
    title_options.max_width = title_options.max_width > 0.0f ? title_options.max_width : std::max(0.0f, widget.bounds.w - widget.padding.left - widget.padding.right);
    body_options.max_width = body_options.max_width > 0.0f ? body_options.max_width : std::max(0.0f, widget.bounds.w - widget.padding.left - widget.padding.right);
    const AdvancedTextContent title_content = dialog_advanced_content(widget.title_content, widget.title_markup, widget.markup_options, widget.title_style, widget.title);
    const AdvancedTextContent body_content = dialog_advanced_content(widget.body_content, widget.body_markup, widget.markup_options, widget.body_style, widget.body);
    const Vec2f title = measure_advanced_text(title_content, title_options);
    const Vec2f body = measure_advanced_text(body_content, body_options);
    const f32 gap = (title.y > 0.0f && body.y > 0.0f) ? widget.gap : 0.0f;
    return {
        widget.padding.left + std::max(title.x, body.x) + widget.padding.right,
        widget.padding.top + title.y + gap + body.y + widget.padding.bottom,
    };
}

struct DialogueViewLayoutMetrics {
    f32 speaker_height = 0.0f;
    f32 choice_height = 0.0f;
    f32 button_size = 0.0f;
    f32 portrait_gap = 0.0f;
    f32 body_choice_gap = 0.0f;
};

DialogueViewLayoutMetrics dialogue_view_layout_metrics(const DialogueView& widget, const TextStyle& speaker_style, const TextStyle& choice_style) {
    const f32 speaker_h = measure_text(speaker_style.font, widget.model.speaker_name.empty() ? "Mg" : widget.model.speaker_name, speaker_style.scale).y;
    f32 choice_h = measure_text(choice_style.font, "Mg", choice_style.scale).y;
    for (const DialogueChoiceView& choice : widget.model.choices) {
        choice_h = std::max(choice_h, measure_text(choice_style.font, choice.text_markup.empty() ? "Mg" : choice.text_markup, choice_style.scale).y);
    }
    const f32 pad_y = std::max(4.0f, widget.padding.top * 0.5f);
    return {
        .speaker_height = std::max(widget.speaker_height, speaker_h + pad_y),
        .choice_height = std::max(widget.choice_height, choice_h + pad_y * 2.0f),
        .button_size = std::max(20.0f, speaker_h + pad_y),
        .portrait_gap = std::max(8.0f, widget.padding.left * 0.75f),
        .body_choice_gap = std::max(6.0f, widget.padding.bottom * 0.5f),
    };
}

Vec2f measure(const DialogueView& widget) {
    const DialogueViewLayoutMetrics metrics = dialogue_view_layout_metrics(widget, widget.speaker_style, widget.choice_style);
    const f32 choices_h = widget.model.choices.empty()
                              ? 0.0f
                              : static_cast<f32>(widget.model.choices.size()) * metrics.choice_height +
                                    static_cast<f32>(std::max(0, static_cast<i32>(widget.model.choices.size()) - 1)) * widget.choice_spacing;
    const f32 portrait_w = widget.portrait.valid() || !widget.model.speaker_portrait.empty() ? widget.portrait_size.x + metrics.portrait_gap : 0.0f;
    const f32 speaker_w = measure_text(widget.speaker_style.font, widget.model.speaker_name, widget.speaker_style.scale).x;
    return {std::max(420.0f, widget.padding.left + portrait_w + std::max(260.0f, speaker_w) + widget.padding.right),
            std::max(180.0f, widget.padding.top + std::max(widget.portrait_size.y, metrics.speaker_height + metrics.body_choice_gap + choices_h) + widget.padding.bottom)};
}

Vec2f measure(const Nameplate& widget) {
    const Vec2f label = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
    const f32 pad_x = std::max(6.0f, label.y * 0.4f);
    const f32 pad_y = std::max(4.0f, label.y * 0.25f);
    const f32 bar_h = widget.show_bar ? std::max(4.0f, label.y * 0.25f) : 0.0f;
    const f32 gap = widget.show_bar ? std::max(2.0f, label.y * 0.15f) : 0.0f;
    return {std::max(80.0f, label.x + pad_x * 2.0f), label.y + pad_y * 2.0f + bar_h + gap};
}

Vec2f measure(const SelectionRect& widget) {
    return {widget.bounds.w, widget.bounds.h};
}

Vec2f measure(const TargetReticle& widget) {
    return {widget.bounds.w, widget.bounds.h};
}

Vec2f measure(const ScrollView&) {
    return {160.0f, 120.0f};
}

Vec2f measure(const ResourceRow& widget) {
    f32 width = 0.0f;
    f32 height = widget.icon_size.y;
    for (std::size_t i = 0; i < widget.items.size(); ++i) {
        const ResourceItem& item = widget.items[i];
        Vec2f icon_size = widget.icon_size;
        if (icon_size.x <= 0.0f && item.icon.valid()) {
            icon_size.x = item.icon.source.w;
        }
        if (icon_size.y <= 0.0f && item.icon.valid()) {
            icon_size.y = item.icon.source.h;
        }
        width += icon_size.x;
        height = std::max(height, icon_size.y);
        if (!item.label.empty()) {
            const Vec2f label = measure_text(widget.text_style.font, item.label, widget.text_style.scale);
            width += widget.gap + label.x;
            height = std::max(height, label.y);
        }
        if (!item.value.empty()) {
            const Vec2f value = measure_text(widget.text_style.font, item.value, widget.text_style.scale);
            width += widget.gap + value.x;
            height = std::max(height, value.y);
        }
        if (i + 1 < widget.items.size()) {
            width += widget.item_gap;
        }
    }
    return {width, height};
}

struct LabeledBarLayoutMetrics {
    UiPadding padding{};
    f32 bar_height = 0.0f;
    f32 height = 0.0f;
    f32 min_bar_width = 0.0f;
};

LabeledBarLayoutMetrics labeled_bar_layout_metrics(const LabeledBar& widget, const StylePreset& preset = {}) {
    const f32 text_h = measure_text(widget.text_style.font, "Mg", widget.text_style.scale).y;
    const f32 pad_x = std::max(6.0f, preset.padding_x * 0.5f);
    const f32 pad_y = std::max(4.0f, preset.padding_y * 0.5f);
    const f32 bar_h = std::max(14.0f, preset.compact_row_height * 0.5f);
    return {
        .padding = {pad_x, pad_y, pad_x, pad_y},
        .bar_height = bar_h,
        .height = std::max(24.0f, std::max(text_h, bar_h) + pad_y * 2.0f),
        .min_bar_width = 80.0f,
    };
}

Vec2f measure(const LabeledBar& widget) {
    const LabeledBarLayoutMetrics metrics = labeled_bar_layout_metrics(widget);
    const Vec2f label = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
    const Vec2f value = measure_text(widget.text_style.font, widget.value_text, widget.text_style.scale);
    const f32 text_w = label.x + (label.x > 0.0f && value.x > 0.0f ? metrics.padding.left : 0.0f) + value.x;
    return {
        std::max(160.0f, std::max(metrics.min_bar_width, text_w) + metrics.padding.left + metrics.padding.right),
        metrics.height,
    };
}

Vec2f measure(const IconMeter& widget) {
    const i32 max_count = std::max(0, widget.max);
    Vec2f icon_size = widget.icon_size;
    if (icon_size.x <= 0.0f && widget.icon.valid()) {
        icon_size.x = widget.icon.source.w;
    }
    if (icon_size.y <= 0.0f && widget.icon.valid()) {
        icon_size.y = widget.icon.source.h;
    }
    if (icon_size.x <= 0.0f && widget.empty_icon.valid()) {
        icon_size.x = widget.empty_icon.source.w;
    }
    if (icon_size.y <= 0.0f && widget.empty_icon.valid()) {
        icon_size.y = widget.empty_icon.source.h;
    }
    return {static_cast<f32>(max_count) * icon_size.x + static_cast<f32>(std::max(0, max_count - 1)) * widget.gap, icon_size.y};
}

Vec2f measure(const RichTextLine& widget) {
    Vec2f size{};
    for (const RichTextSpan& span : widget.spans) {
        const Vec2f span_size = measure_text(widget.text_style.font, span.text, widget.text_style.scale);
        size.x += span_size.x;
        size.y = std::max(size.y, span_size.y);
    }
    return size;
}

Vec2f measure(const TextArea& widget) {
    const WidgetStyle style = widget.style;
    const UiPadding padding = intrinsic_padding(style);
    const f32 width = std::max(240.0f, widget.bounds.w > 0.0f ? widget.bounds.w : 0.0f);
    const TextWrapOptions wrap{.max_width = std::max(1.0f, width - padding.left - padding.right), .scale = widget.text_style.scale, .line_spacing = widget.line_spacing};
    const std::vector<std::string> lines = wrap_text(widget.text_style.font, widget.text, wrap);
    f32 text_h = 0.0f;
    for (const std::string& line : lines) {
        text_h += measure_text(widget.text_style.font, line, widget.text_style.scale).y + widget.line_spacing;
    }
    if (!lines.empty()) {
        text_h -= widget.line_spacing;
    }
    return {width, std::max(120.0f, padding.top + text_h + padding.bottom)};
}

Vec2f measure(const AdvancedText& widget) {
    AdvancedTextLayoutOptions options = widget.layout_options;
    if (options.max_width <= 0.0f) {
        options.max_width = widget.bounds.w > 0.0f ? widget.bounds.w : 240.0f;
    }
    return advanced_text_layout(widget, options).measured;
}

Vec2f measure(const ToastStack& widget) {
    const i32 visible = widget.max_visible < 0 ? static_cast<i32>(widget.items.size()) : std::min(widget.max_visible, static_cast<i32>(widget.items.size()));
    const i32 count = std::max(1, visible);
    return {
        widget.item_size.x,
        static_cast<f32>(count) * widget.item_size.y + static_cast<f32>(std::max(0, count - 1)) * widget.spacing,
    };
}

Vec2f measure(const FloatingText& widget) {
    return measure_text(widget.text_style.font, widget.text, widget.text_style.scale);
}

Vec2f measure(const AnimatedValue& widget) {
    const std::string text = widget.prefix + format_value(widget.display, widget.decimals) + widget.suffix;
    return measure_text(widget.text_style.font, text, widget.text_style.scale);
}

void run(Context& ctx, Panel& widget) {
    const Panel defaults{};
    const Color fill = widget.color == defaults.color ? ctx.theme().colors.surface_panel : widget.color;
    const Color border = widget.border == defaults.border ? ctx.theme().colors.border : widget.border;
    ctx.surface(widget.bounds, with_fill_border(ctx.theme().panel_surface, fill, border));

    // Consume the pointer so widgets behind the panel don't receive it (modal backdrops,
    // draggable windows). region() registers a hit at `z`, winning hover over anything at
    // lower/equal z behind it, which leaves those widgets non-hot and unclickable.
    if (widget.block_input && widget.id) {
        widget.interaction = ctx.region(widget.id, widget.bounds, widget.z);
    } else {
        widget.interaction = {};
    }
}

void run(Context& ctx, Label& widget) {
    const TextStyle text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const Vec2f size = measure_text(text_style.font, widget.text, text_style.scale);
    ctx.report_overflow("Label", widget.bounds, size, widget.text);
    const Rectf aligned = align_rect(widget.bounds, size, widget.horizontal, widget.vertical);
    const Vec2f pos{aligned.x, aligned.y};
    if (widget.overflow == TextOverflow::Clip) {
        ctx.push_clip(widget.bounds);
    }
    ctx.text(widget.text, pos, text_style);
    if (widget.overflow == TextOverflow::Clip) {
        ctx.pop_clip();
    }
}

void run(Context& ctx, WrappedText& widget) {
    const TextStyle text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const TextWrapOptions wrap{
        .max_width = widget.bounds.w,
        .scale = text_style.scale,
        .line_spacing = widget.line_spacing,
    };
    const std::vector<std::string> lines = wrap_text(text_style.font, widget.text, wrap);
    f32 height = 0.0f;
    f32 width = 0.0f;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Vec2f line = measure_text(text_style.font, lines[i], text_style.scale);
        width = std::max(width, line.x);
        height += line.y;
        if (i + 1 < lines.size()) {
            height += widget.line_spacing;
        }
    }
    Rectf area = align_rect(widget.bounds, {width, height}, widget.horizontal, widget.vertical);
    f32 y = area.y;
    if (widget.overflow == TextOverflow::Clip) {
        ctx.push_clip(widget.bounds);
    }
    for (const std::string& line : lines) {
        const Vec2f line_size = measure_text(text_style.font, line, text_style.scale);
        const Rectf line_rect = align_rect({area.x, y, area.w, line_size.y}, line_size, widget.horizontal, UiAlign::Start);
        ctx.text(line, {line_rect.x, line_rect.y}, text_style);
        y += line_size.y + widget.line_spacing;
    }
    if (widget.overflow == TextOverflow::Clip) {
        ctx.pop_clip();
    }
}

void run(Context& ctx, Separator& widget) {
    Rectf rect = widget.bounds;
    if (widget.axis == SeparatorAxis::Horizontal) {
        rect.y += (rect.h - widget.thickness) * 0.5f;
        rect.h = widget.thickness;
    } else {
        rect.x += (rect.w - widget.thickness) * 0.5f;
        rect.w = widget.thickness;
    }
    const Separator defaults{};
    ctx.fill_rect(rect, widget.color == defaults.color ? ctx.theme().colors.border : widget.color);
}

void run(Context& ctx, Button& widget) {
    const Interaction it = ctx.region(widget.id, widget.bounds, widget.z);
    widget.interaction = it;
    widget.clicked = it.clicked && widget.enabled;
    const WidgetStyle style = themed_widget_style(widget.style, ctx.theme().button);
    const TextStyle text_style = themed_text_style(widget.text_style, ctx.theme().body_text);

    ctx.surface(widget.bounds, ctx.resolve_animated(widget.id, style.surface, it, false, widget.enabled));

    const Vec2f text = measure_text(text_style.font, widget.label, text_style.scale);
    const UiPadding padding = intrinsic_padding(style);
    ctx.report_overflow("Button",
                        widget.bounds,
                        {text.x + padding.left + padding.right, text.y + padding.top + padding.bottom},
                        widget.label);
    const Vec2f pos{
        widget.bounds.x + (widget.bounds.w - text.x) * 0.5f,
        widget.bounds.y + (widget.bounds.h - text.y) * 0.5f,
    };
    ctx.text(widget.label, pos, text_style);
}

// Thick stroke as stacked 1px lines offset along the segment normal — works on
// every backend (the renderer only exposes 1px draw_line). Used for vector
// chrome glyphs (checkmarks, chevrons).
void draw_stroke(Context& ctx, Vec2f a, Vec2f b, f32 thickness, Color color) {
    const Vec2f d{b.x - a.x, b.y - a.y};
    const f32 len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len <= 0.0001f) {
        ctx.fill_rect({a.x, a.y, std::max(1.0f, thickness), std::max(1.0f, thickness)}, color);
        return;
    }
    const Vec2f n{-d.y / len, d.x / len};
    const i32 steps = std::max(1, static_cast<i32>(std::ceil(thickness)));
    const f32 start = -(static_cast<f32>(steps) - 1.0f) * 0.5f;
    for (i32 i = 0; i < steps; ++i) {
        const f32 o = start + static_cast<f32>(i);
        ctx.line({a.x + n.x * o, a.y + n.y * o}, {b.x + n.x * o, b.y + n.y * o}, color);
    }
}

// Solid triangle filled with axis-aligned 1px scanlines. Axis-aligned rects need
// no anti-aliasing, so the result is crisp at any size — unlike stacked diagonal
// 1px lines (draw_stroke), which double and stair-step on tiny disclosure glyphs.
// `apex` is the tip; the base is the opposite edge of length 2*half_base.
void fill_triangle(Context& ctx, Vec2f apex, GlyphDir dir, f32 depth, f32 half_base, Color color) {
    const i32 steps = std::max(1, static_cast<i32>(std::ceil(depth)));
    const f32 ax = std::round(apex.x);
    const f32 ay = std::round(apex.y);
    for (i32 i = 0; i < steps; ++i) {
        // Row i is one pixel away from the apex; span grows from 0 (at the tip)
        // to half_base (at the base), so the point sits at the apex.
        const f32 span = half_base * (static_cast<f32>(i) + 1.0f) / static_cast<f32>(steps);
        const f32 w = std::max(1.0f, std::round(span * 2.0f));
        if (dir == GlyphDir::Down) {
            // apex at the bottom, base above → narrow tip points down.
            ctx.fill_rect({ax - w * 0.5f, ay - 1.0f - static_cast<f32>(i), w, 1.0f}, color);
        } else {
            // apex at the right, base to the left → narrow tip points right.
            ctx.fill_rect({ax - 1.0f - static_cast<f32>(i), ay - w * 0.5f, 1.0f, w}, color);
        }
    }
}

// Vector chevron centered at `center`, half-extent `half`. Rendered as a small
// solid triangle (crisp disclosure/dropdown glyph). `thickness` is accepted for
// call-site compatibility but no longer used (kept the stroked look was jagged).
void draw_chevron(Context& ctx, Vec2f center, f32 half, GlyphDir dir, f32 /*thickness*/, Color color) {
    const f32 cx = std::round(center.x);
    const f32 cy = std::round(center.y);
    const f32 depth = std::round(half * 1.1f);   // tip-to-base distance
    const f32 base = half * 0.9f;                 // half-width of the base
    if (dir == GlyphDir::Down) {
        fill_triangle(ctx, {cx, cy + depth * 0.5f}, GlyphDir::Down, depth, base, color);
    } else {
        fill_triangle(ctx, {cx + depth * 0.5f, cy}, GlyphDir::Right, depth, base, color);
    }
}

// Vector checkmark filling `box` (two strokes: down-to-low, then up-to-tip).
void draw_check(Context& ctx, Rectf box, f32 thickness, Color color) {
    const Vec2f start{box.x + box.w * 0.22f, box.y + box.h * 0.52f};
    const Vec2f low{box.x + box.w * 0.42f, box.y + box.h * 0.72f};
    const Vec2f tip{box.x + box.w * 0.78f, box.y + box.h * 0.28f};
    draw_stroke(ctx, start, low, thickness, color);
    draw_stroke(ctx, low, tip, thickness, color);
}

void run(Context& ctx, Toggle& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    const Interaction it = ctx.region(widget.id, widget.bounds, widget.z);
    widget.interaction = it;
    widget.changed = false;
    if (it.clicked && widget.enabled) {
        widget.value = !widget.value;
        widget.changed = true;
    }
    const WidgetStyle style = themed_widget_style(widget.style, ctx.theme().input);
    const TextStyle text_style = themed_text_style(widget.text_style, ctx.theme().body_text);

    const Vec2f text = measure_text(text_style.font, widget.label, text_style.scale);
    const f32 box = std::min({widget.bounds.w, widget.bounds.h, std::max(14.0f, text.y)});
    const Rectf box_rect{widget.bounds.x, widget.bounds.y + (widget.bounds.h - box) * 0.5f, box, box};
    // Checked: accent-filled box with a drawn checkmark. Unchecked: quiet box with
    // only a hairline border, so the accent appears solely on the active state.
    const Color box_fill = widget.value ? style.accent : style.track;
    const Color box_border = widget.value ? colors::transparent : resolved_widget_border(style, it);
    ctx.surface(box_rect, with_fill_border(ctx.theme().input_surface, box_fill, box_border));
    if (widget.value) {
        draw_check(ctx, box_rect, std::max(1.5f, box * 0.12f), ctx.theme().colors.text_on_solid);
    }

    ctx.report_overflow("Toggle", widget.bounds, {box + 8.0f + text.x, std::max(box, text.y)}, widget.label);
    ctx.text(widget.label,
             {box_rect.x + box + 8.0f, widget.bounds.y + (widget.bounds.h - text.y) * 0.5f},
             text_style);
}

void run(Context& ctx, Slider& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    const WidgetStyle style = themed_widget_style(widget.style, ctx.theme().input);
    const SliderLayoutMetrics metrics = slider_layout_metrics(widget, ctx.theme().preset);
    const Interaction it = ctx.region(widget.id, widget.bounds, widget.z);
    widget.interaction = it;
    widget.changed = false;

    if (it.active && widget.enabled && widget.max > widget.min) {
        const f32 norm = std::clamp((ctx.pointer().x - widget.bounds.x) / std::max(1.0f, widget.bounds.w), 0.0f, 1.0f);
        f32 value = widget.min + norm * (widget.max - widget.min);
        if (widget.step > 0.0f) {
            value = widget.min + std::round((value - widget.min) / widget.step) * widget.step;
        }
        value = std::clamp(value, widget.min, widget.max);
        if (value != widget.value) {
            widget.value = value;
            widget.changed = true;
        }
    }

    const f32 norm = widget.max > widget.min
                         ? std::clamp((widget.value - widget.min) / (widget.max - widget.min), 0.0f, 1.0f)
                         : 0.0f;
    const Rectf track_rect{widget.bounds.x,
                           widget.bounds.y + (widget.bounds.h - metrics.track_height) * 0.5f,
                           widget.bounds.w,
                           std::min(widget.bounds.h, metrics.track_height)};
    SurfaceStyle track_surface = with_fill_border(ctx.theme().input_surface, style.track, colors::transparent);
    track_surface.border_mode = BorderMode::None;
    ctx.surface(track_rect, track_surface);
    if (norm > 0.0f) {
        SurfaceStyle fill_surface = with_fill(track_surface, style.accent);
        fill_surface.shadow.enabled = false;
        ctx.push_clip({track_rect.x, track_rect.y, track_rect.w * norm, track_rect.h});
        ctx.surface(track_rect, fill_surface);
        ctx.pop_clip();
    }
    const Color frame_border = it.hot
                                   ? widget_border(style, WidgetColorState::Focused)
                                   : widget_border(style, WidgetColorState::Normal);
    SurfaceStyle frame_surface = with_fill_border(ctx.theme().input_surface, colors::transparent, frame_border);
    frame_surface.draw_fill = false;
    frame_surface.shadow.enabled = false;
    ctx.surface(track_rect, frame_surface);
    SurfaceStyle handle_surface =
        with_fill_border(ctx.theme().button_surface, widget_border(style, WidgetColorState::Normal), colors::transparent);
    handle_surface.shadow.enabled = false;
    handle_surface.border_mode = BorderMode::None;
    const f32 handle_h = std::min(widget.bounds.h, metrics.handle_height);
    const f32 handle_w = std::min(widget.bounds.w, metrics.handle_width);
    // Keep the handle fully inside the track: centering it on the value position would let half
    // the handle hang past the ends at norm 0/1 and overlap neighboring widgets.
    const f32 handle_x = std::clamp(widget.bounds.x + widget.bounds.w * norm - handle_w * 0.5f,
                                    widget.bounds.x,
                                    widget.bounds.x + std::max(0.0f, widget.bounds.w - handle_w));
    ctx.surface({handle_x,
                 widget.bounds.y + (widget.bounds.h - handle_h) * 0.5f,
                 handle_w,
                 handle_h},
                handle_surface);
}

void run(Context& ctx, ProgressBar& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    const f32 value = std::clamp(widget.value, 0.0f, 1.0f);
    const ProgressBarLayoutMetrics metrics = progress_bar_layout_metrics(ctx.theme().preset);
    const ProgressBar defaults{};
    const Color background = widget.background == defaults.background ? ctx.theme().progress_background : widget.background;
    const Color fill = widget.fill == defaults.fill ? ctx.theme().progress_fill : widget.fill;
    const Color border = widget.border == defaults.border ? ctx.theme().palette.transparent : widget.border;
    const SurfaceStyle track = with_fill_border(ctx.theme().progress_track_surface, background, border);
    const Rectf track_rect{widget.bounds.x,
                           widget.bounds.y + (widget.bounds.h - std::min(widget.bounds.h, metrics.height)) * 0.5f,
                           widget.bounds.w,
                           std::min(widget.bounds.h, metrics.height)};
    ctx.surface(track_rect, track);
    if (value <= 0.0f) {
        return;
    }

    SurfaceStyle fill_surface = with_fill_border(ctx.theme().progress_fill_surface, fill, colors::transparent);
    fill_surface.shadow.enabled = false;
    fill_surface.border_mode = BorderMode::None;
    ctx.push_clip({track_rect.x, track_rect.y, track_rect.w * value, track_rect.h});
    ctx.surface(track_rect, fill_surface);
    ctx.pop_clip();
}

void run(Context& ctx, Image& widget) {
    const Sprite sprite = image_sprite(widget);
    if (!sprite.valid()) {
        return;
    }
    ctx.sprite(sprite, fitted_rect(widget.bounds, {sprite.source.w, sprite.source.h}, widget.fit), widget.tint);
}

void run(Context& ctx, NineSlicePanel& widget) {
    draw_nine_slice(ctx, widget.skin, widget.bounds);
}

void run(Context&, Spacer&) {
}

void run(Context& ctx, IconButton& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    widget.style = themed_widget_style(widget.style, ctx.theme().button);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const Interaction it = ctx.region(widget.id, widget.bounds, widget.z);
    widget.interaction = it;
    widget.clicked = it.clicked && widget.enabled;

    ctx.surface(widget.bounds, ctx.resolve_animated(widget.id, widget.style.surface, it, false, widget.enabled));

    const Rectf content = inset(widget.bounds, widget.style.padding);
    const Vec2f icon_size = explicit_or_sprite_size(widget.icon_size, widget.icon);
    const Vec2f text_size = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
    const bool has_label = !widget.label.empty() && widget.label_placement != IconLabelPlacement::None;
    const Vec2f group = icon_button_content_size(widget);
    const Rectf group_rect = align_rect(content, group, UiAlign::Center, UiAlign::Center);

    Rectf icon_rect = align_rect(group_rect, icon_size, UiAlign::Center, UiAlign::Center);
    Vec2f text_pos{};
    if (has_label && widget.label_placement == IconLabelPlacement::Below) {
        icon_rect = align_rect({group_rect.x, group_rect.y, group_rect.w, icon_size.y}, icon_size, UiAlign::Center, UiAlign::Start);
        text_pos = {
            group_rect.x + (group_rect.w - text_size.x) * 0.5f,
            group_rect.y + icon_size.y + widget.style.padding.top,
        };
    } else if (has_label && widget.label_placement == IconLabelPlacement::Right) {
        icon_rect = {group_rect.x, group_rect.y + (group_rect.h - icon_size.y) * 0.5f, icon_size.x, icon_size.y};
        text_pos = {
            group_rect.x + icon_size.x + widget.style.padding.left,
            group_rect.y + (group_rect.h - text_size.y) * 0.5f,
        };
    }

    ctx.sprite(widget.icon, icon_rect, widget.icon_tint);
    if (has_label) {
        ctx.text(widget.label, text_pos, widget.text_style);
    }
}

void run(Context& ctx, IconSlot& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().button);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const IconSlotLayoutMetrics metrics = icon_slot_layout_metrics(widget, ctx.theme().preset);
    widget.clicked = false;
    widget.drag_started = false;
    widget.dragging = false;
    widget.dropped = false;
    widget.dropped_text.clear();
    widget.dropped_value = 0;

    Context::DragSourceResult drag;
    if (widget.drag_enabled && widget.enabled && !widget.locked) {
        drag = ctx.drag_source(widget.id,
                               widget.bounds,
                               {.type = widget.drag_payload_type,
                                .text = widget.drag_payload_text.empty() ? widget.text : widget.drag_payload_text,
                                .value = widget.drag_payload_value},
                               widget.z);
        widget.interaction = drag.interaction;
        widget.drag_started = drag.started;
        widget.dragging = drag.dragging;
    } else {
        widget.interaction = ctx.region(widget.id, widget.bounds, widget.z);
    }

    if (widget.drop_enabled) {
        const Context::DropTargetResult drop = ctx.drop_target(widget.id, widget.bounds, widget.drop_accept_type, widget.z);
        if (drop.dropped) {
            widget.dropped = true;
            widget.dropped_text = drop.payload.text;
            widget.dropped_value = drop.payload.value;
        }
    }

    widget.clicked = widget.enabled && !widget.locked && widget.interaction.clicked && !drag.dragging && !drag.released;
    ctx.surface(widget.bounds, ctx.resolve_animated(widget.id, widget.style.surface, widget.interaction, widget.selected, widget.enabled && !widget.locked));
    draw_slot_contents(ctx, widget.bounds, widget.icon, metrics.icon_size, widget.text, widget.count, widget.text_style, widget.style, widget.icon_tint);
    if (widget.locked) {
        const Vec2f lock_size = measure_text(widget.text_style.font, "X", widget.text_style.scale);
        ctx.text("X", {widget.bounds.x + (widget.bounds.w - lock_size.x) * 0.5f, widget.bounds.y + (widget.bounds.h - lock_size.y) * 0.5f}, widget.text_style);
    }
    if (!widget.tooltip.empty()) {
        ctx.tooltip(make_id(widget.id, "tooltip"), widget.bounds, widget.tooltip);
    }
}

void run(Context& ctx, Meter& widget) {
    const Meter defaults{};
    const Color fill = widget.fill == defaults.fill ? ctx.theme().meter_fill : widget.fill;
    const Color empty = widget.empty == defaults.empty ? ctx.theme().meter_empty : widget.empty;
    const Color border = widget.border == defaults.border ? ctx.theme().colors.border : widget.border;
    const i32 segments = std::max(0, widget.segments);
    if (segments <= 0 || widget.bounds.w <= 0.0f || widget.bounds.h <= 0.0f) {
        return;
    }

    const f32 safe_gap = std::max(0.0f, widget.gap);
    const MeterLayoutMetrics metrics = meter_layout_metrics(widget, ctx.theme().preset);
    const f32 width = std::max(0.0f, (widget.bounds.w - safe_gap * static_cast<f32>(segments - 1)) / static_cast<f32>(segments));
    const f32 height = std::min(widget.bounds.h, metrics.segment_height);
    const f32 y = widget.bounds.y + (widget.bounds.h - height) * 0.5f;
    const f32 ratio = widget.max > 0.0f ? std::clamp(widget.value / widget.max, 0.0f, 1.0f) : 0.0f;
    const f32 filled = ratio * static_cast<f32>(segments);
    for (i32 i = 0; i < segments; ++i) {
        const Rectf segment{
            widget.bounds.x + static_cast<f32>(i) * (width + safe_gap),
            y,
            width,
            height,
        };
        const bool is_filled = static_cast<f32>(i) < filled;
        const SurfaceStyle seg_surface = with_fill(
            is_filled ? ctx.theme().meter_fill_surface : ctx.theme().meter_empty_surface,
            is_filled ? fill : empty);
        ctx.surface(segment, seg_surface);
        if (border.a > 0) {
            ctx.outline_rect(segment, border);
        }
    }
}

void run(Context& ctx, PromptLabel& widget) {
    if (!widget.action.empty()) {
        widget.prompt = ctx.prompt_for_action(widget.action, widget.prompt_options);
    }
    const PromptLabel defaults{};
    const TextStyle prompt_style = default_text_style(widget.prompt_style) ? ctx.theme().emphasis_text : widget.prompt_style;
    const TextStyle text_style = default_text_style(widget.text_style) ? ctx.theme().body_text : widget.text_style;
    const Color chip_fill = widget.chip_fill == defaults.chip_fill ? ctx.theme().prompt_chip_fill : widget.chip_fill;
    const Color chip_border = widget.chip_border == defaults.chip_border ? ctx.theme().prompt_chip_border : widget.chip_border;
    const Vec2f measured = measure(widget);
    ctx.report_overflow("PromptLabel", widget.bounds, measured, widget.prompt.empty() ? widget.text : widget.prompt + " " + widget.text);
    const Rectf aligned = align_rect(widget.bounds, measured, widget.horizontal, widget.vertical);
    const Rectf content = inset(aligned, widget.padding);
    const Vec2f prompt = measure_text(prompt_style.font, widget.prompt, prompt_style.scale);
    const Vec2f text = measure_text(text_style.font, widget.text, text_style.scale);
    const Vec2f icon = explicit_or_sprite_size(widget.icon_size, widget.icon);
    Vec2f chip_size = prompt;
    if (!widget.prompt.empty() && widget.show_chip) {
        chip_size.x += widget.chip_padding.left + widget.chip_padding.right;
        chip_size.y += widget.chip_padding.top + widget.chip_padding.bottom;
    }
    if (widget.icon.valid()) {
        chip_size.x += (chip_size.x > 0.0f ? widget.gap * 0.5f : 0.0f) + icon.x;
        chip_size.y = std::max(chip_size.y, icon.y);
    }
    const f32 y_prompt = content.y + (content.h - chip_size.y) * 0.5f;
    const f32 y_text = content.y + (content.h - text.y) * 0.5f;
    f32 x = content.x;
    if (widget.icon.valid()) {
        ctx.sprite(widget.icon, {x, content.y + (content.h - icon.y) * 0.5f, icon.x, icon.y});
        x += icon.x + (widget.prompt.empty() ? 0.0f : widget.gap * 0.5f);
    }
    if (!widget.prompt.empty()) {
        if (widget.show_chip) {
            const Rectf chip{x, y_prompt, chip_size.x - (widget.icon.valid() ? icon.x + widget.gap * 0.5f : 0.0f), chip_size.y};
            SurfaceStyle chip_surface = ctx.theme().subtle_surface;
            chip_surface.fill = chip_fill;
            chip_surface.border = chip_border;
            chip_surface.border_width = ctx.theme().preset.border_width;
            chip_surface.border_mode = BorderMode::Inside;
            chip_surface.radius = std::min(chip_surface.radius, std::max(2.0f, ctx.theme().preset.radius * 0.5f));
            chip_surface.draw_fill = true;
            chip_surface.shadow.enabled = false;
            ctx.surface(chip, chip_surface);
            ctx.text(widget.prompt, {chip.x + widget.chip_padding.left, chip.y + widget.chip_padding.top}, prompt_style);
            x += chip.w;
        } else {
            ctx.text(widget.prompt, {x, content.y + (content.h - prompt.y) * 0.5f}, prompt_style);
            x += prompt.x;
        }
    }
    if (!widget.prompt.empty() && !widget.text.empty()) {
        x += widget.gap;
        if (!widget.separator.empty()) {
            ctx.text(widget.separator, {x, y_text}, text_style);
            x += measure_text(text_style.font, widget.separator, text_style.scale).x;
        }
    }
    if (!widget.text.empty()) {
        ctx.text(widget.text, {x, y_text}, text_style);
    }
}

void run(Context& ctx, PromptRow& widget) {
    std::vector<PromptLabel> labels;
    labels.reserve(widget.items.size());
    for (i32 i = 0; i < static_cast<i32>(widget.items.size()); ++i) {
        const PromptRowItem& item = widget.items[static_cast<std::size_t>(i)];
        PromptLabel label = widget.item_style;
        label.id = make_id(widget.id ? widget.id : make_id("prompt_row"), static_cast<u64>(i));
        label.action = item.action;
        label.prompt = item.prompt;
        label.text = item.text;
        label.icon = item.icon;
        label.prompt_options = widget.prompt_options;
        label.prompt_style = default_text_style(widget.prompt_style) ? ctx.theme().emphasis_text : widget.prompt_style;
        label.text_style = default_text_style(widget.text_style) ? ctx.theme().body_text : widget.text_style;
        if (label.chip_fill == PromptLabel{}.chip_fill) {
            label.chip_fill = ctx.theme().prompt_chip_fill;
        }
        if (label.chip_border == PromptLabel{}.chip_border) {
            label.chip_border = ctx.theme().prompt_chip_border;
        }
        if (!label.action.empty()) {
            label.prompt = ctx.prompt_for_action(label.action, label.prompt_options);
            label.action.clear();
        }
        if (!item.enabled) {
            const Color disabled = ctx.theme().colors.text_disabled;
            label.prompt_style.color = disabled;
            label.text_style.color = disabled;
            label.chip_border = disabled;
            label.chip_fill = ctx.theme().colors.interactive_disabled;
        }
        labels.push_back(std::move(label));
    }

    const auto measure_labels = [&](UiPadding padding, f32 spacing) {
        Vec2f size{};
        for (i32 i = 0; i < static_cast<i32>(labels.size()); ++i) {
            const Vec2f item_size = measure(labels[static_cast<std::size_t>(i)]);
            if (widget.axis == UiLayoutAxis::Horizontal) {
                size.x += item_size.x + (i > 0 ? spacing : 0.0f);
                size.y = std::max(size.y, item_size.y);
            } else {
                size.x = std::max(size.x, item_size.x);
                size.y += item_size.y + (i > 0 ? spacing : 0.0f);
            }
        }
        return Vec2f{padding.left + size.x + padding.right, padding.top + size.y + padding.bottom};
    };

    Vec2f measured = measure_labels(widget.padding, widget.spacing);
    const bool compact = widget.axis == UiLayoutAxis::Horizontal && measured.x > widget.bounds.w;
    UiPadding row_padding = widget.padding;
    f32 spacing = widget.spacing;
    if (compact) {
        row_padding.left = std::min(row_padding.left, 2.0f);
        row_padding.right = std::min(row_padding.right, 2.0f);
        spacing = std::min(spacing, 4.0f);
        for (PromptLabel& label : labels) {
            label.padding.left = std::min(label.padding.left, 2.0f);
            label.padding.right = std::min(label.padding.right, 2.0f);
            label.chip_padding.left = std::max(5.0f, std::min(label.chip_padding.left, 8.0f));
            label.chip_padding.right = std::max(5.0f, std::min(label.chip_padding.right, 8.0f));
            label.gap = std::min(label.gap, 4.0f);
        }
        measured = measure_labels(row_padding, spacing);
    }
    ctx.report_overflow("PromptRow", widget.bounds, measured, "row");

    const Rectf aligned = compact ? widget.bounds : align_rect(widget.bounds, measured, widget.horizontal, widget.vertical);
    const Rectf content = inset(aligned, row_padding);
    f32 cursor = widget.axis == UiLayoutAxis::Horizontal ? content.x : content.y;
    ctx.push_clip(widget.bounds);
    for (PromptLabel& label : labels) {
        const Vec2f size = measure(label);
        const bool horizontal = widget.axis == UiLayoutAxis::Horizontal;
        bool constrained = false;
        if (widget.axis == UiLayoutAxis::Horizontal) {
            const f32 available = std::max(0.0f, widget.bounds.x + widget.bounds.w - cursor);
            if (available <= 0.0f) {
                break;
            }
            const f32 width = std::min(size.x, available);
            constrained = width < size.x;
            label.bounds = {cursor, content.y + (content.h - size.y) * 0.5f, width, size.y};
        } else {
            const f32 available = std::max(0.0f, widget.bounds.y + widget.bounds.h - cursor);
            if (available <= 0.0f) {
                break;
            }
            const f32 height = std::min(size.y, available);
            constrained = height < size.y;
            label.bounds = {content.x + (content.w - size.x) * 0.5f, cursor, size.x, height};
        }
        if (constrained) {
            ctx.push_clip(horizontal ? Rectf{label.bounds.x, widget.bounds.y, label.bounds.w, widget.bounds.h}
                                     : Rectf{widget.bounds.x, label.bounds.y, widget.bounds.w, label.bounds.h});
        }
        run(ctx, label);
        if (constrained) {
            ctx.pop_clip();
        }
        cursor += (widget.axis == UiLayoutAxis::Horizontal ? size.x : size.y) + spacing;
    }
    ctx.pop_clip();
}

void run(Context& ctx, DialogBox& widget) {
    if (widget.skin.sprite.valid()) {
        draw_nine_slice(ctx, widget.skin, widget.bounds);
    } else {
        const DialogBox defaults{};
        const Color fill = widget.fill == defaults.fill ? ctx.theme().colors.surface_panel : widget.fill;
        const Color border = widget.border == defaults.border ? ctx.theme().colors.border : widget.border;
        ctx.surface(widget.bounds, with_fill_border(ctx.theme().card_surface, fill, border));
    }

    const Rectf content = inset(widget.bounds, widget.padding);
    AdvancedTextLayoutOptions title_options = widget.title_layout;
    AdvancedTextLayoutOptions body_options = widget.body_layout;
    title_options.max_width = title_options.max_width > 0.0f ? title_options.max_width : content.w;
    body_options.max_width = body_options.max_width > 0.0f ? body_options.max_width : content.w;
    title_options.overflow = AdvancedTextOverflow::Clip;
    body_options.overflow = AdvancedTextOverflow::Clip;
    const TextStyle title_style = default_text_style(widget.title_style) ? ctx.theme().title_text : widget.title_style;
    const TextStyle body_style = default_text_style(widget.body_style) ? ctx.theme().body_text : widget.body_style;
    const AdvancedTextContent title_content = dialog_advanced_content(widget.title_content, widget.title_markup, widget.markup_options, title_style, widget.title);
    const AdvancedTextContent body_content = dialog_advanced_content(widget.body_content, widget.body_markup, widget.markup_options, body_style, widget.body);
    const AdvancedTextLayout title_layout = layout_advanced_text(title_content, title_options);
    const AdvancedTextLayout body_layout = layout_advanced_text(body_content, body_options);
    f32 y = content.y;
    if (title_layout.measured.y > 0.0f) {
        draw_advanced_text(ctx, title_layout, {content.x, y, content.w, title_layout.measured.y}, title_options);
        y += title_layout.measured.y;
    }
    if (title_layout.measured.y > 0.0f && body_layout.measured.y > 0.0f) {
        y += widget.gap;
    }
    if (body_layout.measured.y > 0.0f) {
        draw_advanced_text(ctx, body_layout, {content.x, y, content.w, std::max(0.0f, content.y + content.h - y)}, body_options);
    }
}

void run(Context& ctx, DialogueView& widget) {
    widget.advance_requested = false;
    widget.skip_requested = false;
    widget.history_requested = false;
    widget.closed_requested = false;
    widget.choice_requested = false;
    widget.choice_index = -1;
    widget.choice_id.clear();

    const WidgetStyle style = themed_widget_style(widget.style, ctx.theme().panel);
    const TextStyle speaker_style = default_text_style(widget.speaker_style) ? ctx.theme().title_text : widget.speaker_style;
    const TextStyle body_style = default_text_style(widget.body_style) ? ctx.theme().body_text : widget.body_style;
    const TextStyle choice_style = default_text_style(widget.choice_style) ? ctx.theme().body_text : widget.choice_style;
    const DialogueViewLayoutMetrics metrics = dialogue_view_layout_metrics(widget, speaker_style, choice_style);

    ctx.surface(widget.bounds, with_fill_border(ctx.theme().card_surface, style.track, widget_border(style, WidgetColorState::Normal)));

    const Rectf content = inset(widget.bounds, widget.padding);
    f32 top = content.y;
    const f32 button = metrics.button_size;
    const bool has_portrait = widget.portrait.valid() || !widget.model.speaker_portrait.empty();
    Rectf portrait_rect{};
    f32 text_x = content.x;
    f32 text_w = content.w;
    if (has_portrait) {
        portrait_rect = {content.x, content.y, widget.portrait_size.x, widget.portrait_size.y};
        ctx.surface(portrait_rect, with_fill_border(ctx.theme().subtle_surface, widget_fill(style, WidgetColorState::Normal), widget_border(style, WidgetColorState::Normal)));
        if (widget.portrait.valid()) {
            ctx.sprite(widget.portrait, portrait_rect);
        }
        text_x += widget.portrait_size.x + metrics.portrait_gap;
        text_w = std::max(0.0f, content.x + content.w - text_x);
    }

    if (widget.show_close_button) {
        const Rectf close{content.x + content.w - button, content.y, button, button};
        if (ctx.region(make_id(widget.id, "close"), close, widget.z + 1).clicked) {
            widget.closed_requested = true;
        }
        const Vec2f close_size = measure_text(speaker_style.font, "x", speaker_style.scale);
        ctx.text("x", {close.x + (close.w - close_size.x) * 0.5f, close.y + (close.h - close_size.y) * 0.5f}, speaker_style);
    }
    if (widget.show_history_button) {
        const Rectf history{content.x + content.w - button * 2.0f - metrics.body_choice_gap * 0.5f, content.y, button, button};
        if (ctx.region(make_id(widget.id, "history"), history, widget.z + 1).clicked) {
            widget.history_requested = true;
        }
        const Vec2f history_size = measure_text(speaker_style.font, "H", speaker_style.scale);
        ctx.text("H", {history.x + (history.w - history_size.x) * 0.5f, history.y + (history.h - history_size.y) * 0.5f}, speaker_style);
    }

    if (!widget.model.speaker_name.empty()) {
        TextStyle speaker = speaker_style;
        speaker.color = widget.model.speaker_color;
        ctx.text(widget.model.speaker_name, {text_x, top}, speaker);
    }
    top += metrics.speaker_height;

    std::vector<DialogueChoiceView> visible_choices;
    visible_choices.reserve(widget.model.choices.size());
    for (const DialogueChoiceView& choice : widget.model.choices) {
        if (choice.enabled || widget.show_disabled_choices) {
            visible_choices.push_back(choice);
        }
    }

    const f32 prompt_h = widget.prompts.items.empty() ? 0.0f : std::max(22.0f, measure(widget.prompts).y);
    const f32 choice_count = static_cast<f32>(visible_choices.size());
    const f32 choices_h = choice_count <= 0.0f
                              ? 0.0f
                              : choice_count * metrics.choice_height + std::max(0.0f, choice_count - 1.0f) * widget.choice_spacing;
    f32 bottom = content.y + content.h;
    if (prompt_h > 0.0f) {
        bottom -= prompt_h;
    }
    const Rectf choice_area{text_x, std::max(top, bottom - choices_h), text_w, choices_h};
    const Rectf body{text_x, top, text_w, std::max(0.0f, choice_area.y - top - (choices_h > 0.0f ? metrics.body_choice_gap : 0.0f))};

    AdvancedTextMarkupOptions markup = widget.markup_options;
    markup.base_style = body_style;
    AdvancedText line{
        .id = make_id(widget.id, "line"),
        .bounds = body,
        .markup = widget.model.text_markup,
        .markup_options = markup,
        .layout_options = {.max_width = body.w, .overflow = AdvancedTextOverflow::Clip},
        .z = widget.z,
    };
    run(ctx, line);

    for (i32 i = 0; i < static_cast<i32>(visible_choices.size()); ++i) {
        const DialogueChoiceView& choice = visible_choices[static_cast<std::size_t>(i)];
        const Rectf row{
            choice_area.x,
            choice_area.y + static_cast<f32>(i) * (metrics.choice_height + widget.choice_spacing),
            choice_area.w,
            metrics.choice_height,
        };
        Button button_widget{
            .id = make_id(widget.id, choice.id.empty() ? std::to_string(i) : choice.id),
            .bounds = row,
            .label = choice.text_markup,
            .text_style = choice_style,
            .style = style,
            .enabled = choice.enabled,
            .z = widget.z + 1,
        };
        run(ctx, button_widget);
        if (button_widget.clicked) {
            widget.choice_requested = true;
            widget.choice_index = i;
            widget.choice_id = choice.id;
        }
    }

    if (prompt_h > 0.0f) {
        widget.prompts.id = widget.prompts.id ? widget.prompts.id : make_id(widget.id, "prompts");
        widget.prompts.bounds = {content.x, content.y + content.h - prompt_h, content.w, prompt_h};
        run(ctx, widget.prompts);
    }

    if (widget.model.choices.empty() && widget.model.active && !widget.model.ended) {
        const Interaction it = ctx.region(make_id(widget.id, "advance"), body.w > 0.0f && body.h > 0.0f ? body : content, widget.z);
        widget.advance_requested = it.clicked || ctx.action_pressed("accept");
    }
    widget.skip_requested = ctx.action_pressed("menu_right");
    widget.closed_requested = widget.closed_requested || ctx.action_pressed("quit");
}

void run(Context& ctx, Nameplate& widget) {
    ctx.fill_rect(widget.bounds, widget.fill);
    ctx.outline_rect(widget.bounds, widget.border);
    const Vec2f text_size = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
    const f32 pad_x = std::max(6.0f, text_size.y * 0.4f);
    const f32 pad_y = std::max(4.0f, text_size.y * 0.25f);
    ctx.text(widget.label, {widget.bounds.x + (widget.bounds.w - text_size.x) * 0.5f, widget.bounds.y + pad_y}, widget.text_style);
    if (widget.show_bar) {
        const f32 bar_h = std::max(4.0f, text_size.y * 0.25f);
        const Rectf bar{widget.bounds.x + pad_x, widget.bounds.y + widget.bounds.h - pad_y - bar_h, std::max(0.0f, widget.bounds.w - pad_x * 2.0f), bar_h};
        ctx.fill_rect(bar, widget.bar_back);
        ctx.fill_rect({bar.x, bar.y, bar.w * std::clamp(widget.value, 0.0f, 1.0f), bar.h}, widget.bar_fill);
    }
}

void run(Context& ctx, SelectionRect& widget) {
    widget.bounds = normalized_rect(widget.start, widget.end);
    widget.active = widget.bounds.w > 0.0f || widget.bounds.h > 0.0f;
    if (widget.active) {
        ctx.fill_rect(widget.bounds, widget.fill);
        ctx.outline_rect(widget.bounds, widget.border);
    }
}

void run(Context& ctx, TargetReticle& widget) {
    const Color color = widget.valid ? widget.valid_color : widget.invalid_color;
    ctx.outline_rect(widget.bounds, color);
    const f32 c = std::max(0.0f, widget.corner);
    ctx.fill_rect({widget.bounds.x, widget.bounds.y, c, 1.0f}, color);
    ctx.fill_rect({widget.bounds.x, widget.bounds.y, 1.0f, c}, color);
    ctx.fill_rect({widget.bounds.x + widget.bounds.w - c, widget.bounds.y, c, 1.0f}, color);
    ctx.fill_rect({widget.bounds.x + widget.bounds.w - 1.0f, widget.bounds.y, 1.0f, c}, color);
    ctx.fill_rect({widget.bounds.x, widget.bounds.y + widget.bounds.h - 1.0f, c, 1.0f}, color);
    ctx.fill_rect({widget.bounds.x, widget.bounds.y + widget.bounds.h - c, 1.0f, c}, color);
    ctx.fill_rect({widget.bounds.x + widget.bounds.w - c, widget.bounds.y + widget.bounds.h - 1.0f, c, 1.0f}, color);
    ctx.fill_rect({widget.bounds.x + widget.bounds.w - 1.0f, widget.bounds.y + widget.bounds.h - c, 1.0f, c}, color);
}

void run(Context& ctx, ScrollView& widget) {
    const ScrollView defaults{};
    if (widget.background == defaults.background) {
        widget.background = ctx.theme().colors.surface_panel;
    }
    if (widget.border == defaults.border) {
        widget.border = ctx.theme().colors.border;
    }
    if (widget.scrollbar_track == defaults.scrollbar_track) {
        widget.scrollbar_track = ctx.theme().colors.interactive_disabled;
    }
    if (widget.scrollbar_thumb == defaults.scrollbar_thumb) {
        widget.scrollbar_thumb = ctx.theme().colors.solid_accent;
    }
    widget.changed = false;
    widget.dragging = false;
    const f32 bar = widget.show_scrollbar ? widget.scrollbar_thickness : 0.0f;
    const Vec2f explicit_content{
        widget.content_size.x > 0.0f ? widget.content_size.x : widget.bounds.w,
        widget.content_size.y > 0.0f ? widget.content_size.y : widget.content_height,
    };
    // Right to left, the scrollbar is on the left. (Not a mirror scope: games
    // read viewport and content in screen coordinates.)
    const bool rtl = ui_direction() == TextDirection::RightToLeft;
    widget.viewport = {
        rtl ? widget.bounds.x + bar : widget.bounds.x,
        widget.bounds.y,
        std::max(0.0f, widget.bounds.w - bar),
        std::max(0.0f, widget.bounds.h - (widget.horizontal ? bar : 0.0f)),
    };
    widget.track = {rtl ? widget.bounds.x : widget.viewport.x + widget.viewport.w, widget.bounds.y, bar, widget.bounds.h};
    widget.h_track = {widget.viewport.x, widget.viewport.y + widget.viewport.h, widget.viewport.w, bar};

    widget.scroll.offset = widget.scroll.offset.x == 0.0f && widget.scroll.offset.y == 0.0f ? Vec2f{0.0f, widget.offset} : widget.scroll.offset;
    widget.scroll.content_size = explicit_content;
    widget.scroll.viewport_size = {widget.viewport.w, widget.viewport.h};
    ScrollOptions options{
        .wheel_step = widget.wheel_step2.x > 0.0f || widget.wheel_step2.y > 0.0f ? widget.wheel_step2 : Vec2f{widget.wheel_step, widget.wheel_step},
        .scrollbar_thickness = widget.scrollbar_thickness,
        .min_thumb = widget.min_thumb,
        .show_scrollbar = widget.show_scrollbar,
        .enabled = widget.enabled,
    };
    ScrollResult scroll = scroll_region(ctx,
                                        widget.id ? widget.id : make_id("scroll_view"),
                                        widget.viewport,
                                        widget.scroll,
                                        options,
                                        0,
                                        &widget.v_grab_offset,
                                        &widget.h_grab_offset);
    widget.scroll = scroll.state;
    widget.offset = widget.scroll.offset.y;
    widget.content_height = widget.scroll.content_size.y;
    widget.changed = scroll.changed;
    widget.dragging = scroll.dragging;

    widget.content = {widget.viewport.x - widget.scroll.offset.x, widget.viewport.y - widget.scroll.offset.y, widget.scroll.content_size.x, widget.scroll.content_size.y};
    if (widget.draw_background) {
        ctx.fill_rect(widget.bounds, widget.background);
        ctx.outline_rect(widget.bounds, widget.border);
    }

    if (widget.show_scrollbar && widget.scrollbar_thickness > 0.0f) {
        ctx.surface(widget.track, with_fill(ctx.theme().scrollbar_track_surface, widget.scrollbar_track));
        widget.thumb = layout_scrollbar(widget.track, widget.scroll, ScrollAxis::Vertical, options).thumb;
        ctx.surface(widget.thumb, with_fill(ctx.theme().scrollbar_thumb_surface, widget.scrollbar_thumb));
        if (widget.horizontal) {
            ctx.surface(widget.h_track, with_fill(ctx.theme().scrollbar_track_surface, widget.scrollbar_track));
            widget.h_thumb = layout_scrollbar(widget.h_track, widget.scroll, ScrollAxis::Horizontal, options).thumb;
            ctx.surface(widget.h_thumb, with_fill(ctx.theme().scrollbar_thumb_surface, widget.scrollbar_thumb));
        }
    }
}

void run(Context& ctx, ResourceRow& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    widget.style = themed_widget_style(widget.style, ctx.theme().panel);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    f32 x = widget.bounds.x;
    for (const ResourceItem& item : widget.items) {
        Vec2f icon_size = widget.icon_size;
        if (icon_size.x <= 0.0f && item.icon.valid()) {
            icon_size.x = item.icon.source.w;
        }
        if (icon_size.y <= 0.0f && item.icon.valid()) {
            icon_size.y = item.icon.source.h;
        }
        const Rectf icon_rect{x, widget.bounds.y + (widget.bounds.h - icon_size.y) * 0.5f, icon_size.x, icon_size.y};
        if (item.icon.valid()) {
            ctx.sprite(item.icon, icon_rect, item.color);
        } else {
            ctx.fill_rect(icon_rect, widget.style.track);
            ctx.outline_rect(icon_rect, widget_border(widget.style, WidgetColorState::Normal));
        }
        x += icon_size.x;
        TextStyle style = widget.text_style;
        style.color = item.color;
        if (!item.label.empty()) {
            x += widget.gap;
            const Vec2f label = measure_text(style.font, item.label, style.scale);
            ctx.text(item.label, {x, widget.bounds.y + (widget.bounds.h - label.y) * 0.5f}, style);
            x += label.x;
        }
        if (!item.value.empty()) {
            x += widget.gap;
            const Vec2f value = measure_text(style.font, item.value, style.scale);
            ctx.text(item.value, {x, widget.bounds.y + (widget.bounds.h - value.y) * 0.5f}, style);
            x += value.x;
        }
        x += widget.item_gap;
    }
}

void run(Context& ctx, LabeledBar& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    const LabeledBar defaults{};
    const Color background = widget.background == defaults.background ? ctx.theme().progress_background : widget.background;
    const Color fill = widget.fill == defaults.fill ? ctx.theme().progress_fill : widget.fill;
    const Color border = widget.border == defaults.border ? ctx.theme().colors.border : widget.border;
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const LabeledBarLayoutMetrics metrics = labeled_bar_layout_metrics(widget, ctx.theme().preset);
    const SurfaceStyle track_surface = with_fill_border(ctx.theme().subtle_surface, background, border);
    ctx.surface(widget.bounds, track_surface);
    SurfaceStyle fill_surface = with_fill_border(track_surface, fill, colors::transparent);
    fill_surface.border_mode = BorderMode::None;
    fill_surface.shadow.enabled = false;
    const Rectf fill_rect{widget.bounds.x, widget.bounds.y, widget.bounds.w * std::clamp(widget.value, 0.0f, 1.0f), widget.bounds.h};
    ctx.surface(fill_rect, fill_surface);

    const auto draw_bar_text = [&](TextStyle text_style) {
        const Vec2f label = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
        const Vec2f value = measure_text(widget.text_style.font, widget.value_text, widget.text_style.scale);
        if (!widget.label.empty()) {
            ctx.text(widget.label, {widget.bounds.x + metrics.padding.left, widget.bounds.y + (widget.bounds.h - label.y) * 0.5f}, text_style);
        }
        if (!widget.value_text.empty()) {
            ctx.text(widget.value_text, {widget.bounds.x + widget.bounds.w - value.x - metrics.padding.right, widget.bounds.y + (widget.bounds.h - value.y) * 0.5f}, text_style);
        }
    };
    draw_bar_text(widget.text_style);
    if (fill_rect.w > 0.0f) {
        TextStyle solid_text = widget.text_style;
        solid_text.color = ctx.theme().colors.text_on_solid;
        ctx.push_clip(fill_rect);
        draw_bar_text(solid_text);
        ctx.pop_clip();
    }
}

void run(Context& ctx, IconMeter& widget) {
    const auto mirror = ctx.mirror_if_right_to_left(widget.bounds); // right to left: mirrored inside
    const i32 max_count = std::max(0, widget.max);
    Vec2f icon_size = widget.icon_size;
    if (icon_size.x <= 0.0f && widget.icon.valid()) {
        icon_size.x = widget.icon.source.w;
    }
    if (icon_size.y <= 0.0f && widget.icon.valid()) {
        icon_size.y = widget.icon.source.h;
    }
    if (icon_size.x <= 0.0f && widget.empty_icon.valid()) {
        icon_size.x = widget.empty_icon.source.w;
    }
    if (icon_size.y <= 0.0f && widget.empty_icon.valid()) {
        icon_size.y = widget.empty_icon.source.h;
    }
    for (i32 i = 0; i < max_count; ++i) {
        const Rectf rect{widget.bounds.x + static_cast<f32>(i) * (icon_size.x + widget.gap), widget.bounds.y + (widget.bounds.h - icon_size.y) * 0.5f, icon_size.x, icon_size.y};
        const bool filled = i < widget.value;
        const Sprite sprite = filled || !widget.empty_icon.valid() ? widget.icon : widget.empty_icon;
        if (sprite.valid()) {
            ctx.sprite(sprite, rect, filled ? widget.fill_tint : widget.empty_tint);
        } else {
            ctx.fill_rect(rect, filled ? widget.fill_tint : widget.empty_tint);
        }
    }
}

void run(Context& ctx, RichTextLine& widget) {
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    f32 x = widget.bounds.x;
    for (const RichTextSpan& span : widget.spans) {
        TextStyle style = widget.text_style;
        style.color = span.color;
        ctx.text(span.text, {x, widget.bounds.y}, style);
        x += measure_text(style.font, span.text, style.scale).x;
    }
}

void run(Context& ctx, TextArea& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().panel);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    const UiPadding padding = intrinsic_padding(widget.style);
    if (widget.draw_background) {
        ctx.fill_rect(widget.bounds, widget.style.track);
        ctx.outline_rect(widget.bounds, widget_border(widget.style, WidgetColorState::Normal));
    }
    const TextWrapOptions wrap{.max_width = std::max(1.0f, widget.bounds.w - padding.left - padding.right), .scale = widget.text_style.scale, .line_spacing = widget.line_spacing};
    const std::vector<std::string> lines = wrap_text(widget.text_style.font, widget.text, wrap);
    f32 content_height = padding.top + padding.bottom;
    for (const std::string& line : lines) {
        content_height += measure_text(widget.text_style.font, line, widget.text_style.scale).y + widget.line_spacing;
    }
    ScrollState scroll{
        .offset = {0.0f, widget.offset},
        .content_size = {widget.bounds.w, std::max(0.0f, content_height - widget.line_spacing)},
        .viewport_size = {widget.bounds.w, widget.bounds.h},
    };
    clamp_scroll(scroll);
    if (contains(widget.bounds, ctx.pointer()) && ctx.mouse_wheel_y() != 0.0f) {
        scroll_by(scroll, {0.0f, -ctx.mouse_wheel_y() * widget.wheel_step});
    }
    widget.offset = scroll.offset.y;
    ctx.push_clip(widget.bounds);
    f32 y = widget.bounds.y + padding.top - widget.offset;
    for (const std::string& line : lines) {
        const Vec2f size = measure_text(widget.text_style.font, line, widget.text_style.scale);
        ctx.text(line, {widget.bounds.x + padding.left, y}, widget.text_style);
        y += size.y + widget.line_spacing;
    }
    ctx.pop_clip();
}

void run(Context& ctx, AdvancedText& widget) {
    widget.hovered_id.clear();
    widget.clicked_id.clear();
    widget.hovered_rect = {};
    widget.clicked_rect = {};

    AdvancedTextLayoutOptions options = widget.layout_options;
    if (options.max_width <= 0.0f) {
        options.max_width = widget.bounds.w > 0.0f ? widget.bounds.w : 240.0f;
    }
    widget.layout = advanced_text_layout(widget, options);
    const Rectf aligned = align_rect(widget.bounds, widget.layout.measured, options.horizontal, options.vertical);
    for (std::size_t i = 0; i < widget.layout.hits.size(); ++i) {
        const AdvancedTextBox& hit = widget.layout.hits[i];
        if (!hit.link || hit.id.empty()) {
            continue;
        }
        const Rectf rect{aligned.x + hit.rect.x, aligned.y + hit.rect.y, hit.rect.w, hit.rect.h};
        const Id hit_id = make_id(widget.id ? widget.id : make_id("advanced_text"), hit.id);
        const Interaction it = ctx.region(hit_id, rect, widget.z);
        if (it.hot) {
            widget.hovered_id = hit.id;
            widget.hovered_rect = rect;
        }
        if (it.clicked) {
            widget.clicked_id = hit.id;
            widget.clicked_rect = rect;
        }
    }
    draw_advanced_text(ctx, widget.layout, widget.bounds, options);
}

void run(Context& ctx, ToastStack& widget) {
    widget.style = themed_widget_style(widget.style, ctx.theme().panel);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    widget.visible = 0;
    std::vector<std::size_t> visible;
    visible.reserve(widget.items.size());
    for (std::size_t i = widget.items.size(); i > 0; --i) {
        const ToastItem& item = widget.items[i - 1];
        if (item.lifetime > 0.0f && item.age >= item.lifetime) {
            continue;
        }
        visible.push_back(i - 1);
        if (widget.max_visible >= 0 && static_cast<i32>(visible.size()) >= widget.max_visible) {
            break;
        }
    }
    std::reverse(visible.begin(), visible.end());
    widget.visible = static_cast<i32>(visible.size());
    if (visible.empty()) {
        return;
    }

    const Vec2f stack_size{
        widget.item_size.x,
        static_cast<f32>(visible.size()) * widget.item_size.y + static_cast<f32>(visible.size() - 1) * widget.spacing,
    };
    const Rectf stack = anchor_rect(widget.bounds, widget.anchor, stack_size);
    for (std::size_t i = 0; i < visible.size(); ++i) {
        const ToastItem& item = widget.items[visible[i]];
        const f32 alpha = widget.fade ? life_alpha(item.age, item.lifetime) : 1.0f;
        const Rectf row{stack.x, stack.y + static_cast<f32>(i) * (widget.item_size.y + widget.spacing), widget.item_size.x, widget.item_size.y};
        ctx.fill_rect(row, alpha_scaled(widget_fill(widget.style, WidgetColorState::Normal), alpha));
        ctx.outline_rect(row, alpha_scaled(widget_border(widget.style, WidgetColorState::Normal), alpha));

        const Rectf content = inset(row, widget.padding);
        f32 x = content.x;
        if (item.icon.valid()) {
            const Rectf icon = align_rect({content.x, content.y, widget.icon_size.x, content.h}, widget.icon_size, UiAlign::Center, UiAlign::Center);
            ctx.sprite(item.icon, icon, alpha_scaled(item.color, alpha));
            x += widget.icon_size.x + widget.padding.left;
        }
        TextStyle style = widget.text_style;
        style.color = alpha_scaled(item.color, alpha);
        const Vec2f text = measure_text(style.font, item.text, style.scale);
        ctx.text(item.text, {x, content.y + (content.h - text.y) * 0.5f}, style);
    }
}

void run(Context& ctx, FloatingText& widget) {
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    widget.rendered_position = {
        widget.position.x + widget.velocity.x * widget.age,
        widget.position.y + widget.velocity.y * widget.age,
    };
    if (!widget.active || widget.text.empty()) {
        return;
    }
    TextStyle style = widget.text_style;
    if (widget.fade) {
        style.color = alpha_scaled(style.color, life_alpha(widget.age, widget.lifetime));
    }
    ctx.text(widget.text, widget.rendered_position, style);
    const Vec2f size = measure_text(style.font, widget.text, style.scale);
    widget.bounds = {widget.rendered_position.x, widget.rendered_position.y, size.x, size.y};
}

void run(Context& ctx, AnimatedValue& widget) {
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    widget.rendered_text = widget.prefix + format_value(widget.display, widget.decimals) + widget.suffix;
    const Vec2f size = measure_text(widget.text_style.font, widget.rendered_text, widget.text_style.scale);
    const Rectf aligned = align_rect(widget.bounds, size, widget.horizontal, widget.vertical);
    ctx.text(widget.rendered_text, {aligned.x, aligned.y}, widget.text_style);
}

bool step_animated_value(AnimatedValue& widget, f32 dt) {
    const f32 before = widget.display;
    const f32 max_step = std::max(0.0f, widget.speed) * std::max(0.0f, dt);
    const f32 delta = widget.value - widget.display;
    if (std::abs(delta) <= max_step || max_step <= 0.0f) {
        widget.display = max_step <= 0.0f ? widget.display : widget.value;
    } else {
        widget.display += delta > 0.0f ? max_step : -max_step;
    }
    widget.changed = widget.display != before;
    return widget.changed;
}

} // namespace kin::ui2
