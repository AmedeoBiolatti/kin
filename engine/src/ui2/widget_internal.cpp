#include <kin/core/utf8.hpp>
#include <kin/platform/log.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/widgets.hpp>

#include "style_internal.hpp"
#include "widget_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kin::ui2::detail {

f32 clamp_margin(f32 margin, f32 limit) {
    return std::clamp(margin, 0.0f, std::max(0.0f, limit));
}

Sprite image_sprite(const Image& widget) {
    if (widget.sprite.valid()) {
        return widget.sprite;
    }
    if (widget.texture.valid()) {
        return full_sprite(widget.texture);
    }
    return {};
}

Vec2f explicit_or_sprite_size(Vec2f explicit_size, const Sprite& sprite) {
    if (explicit_size.x > 0.0f && explicit_size.y > 0.0f) {
        return explicit_size;
    }
    if (sprite.valid()) {
        return {sprite.source.w, sprite.source.h};
    }
    return {};
}

Rectf fitted_rect(Rectf bounds, Vec2f size, ImageFit fit) {
    if (fit == ImageFit::Stretch || size.x <= 0.0f || size.y <= 0.0f) {
        return bounds;
    }
    return aspect_rect(bounds, size, fit == ImageFit::Cover ? UiFitMode::Cover : UiFitMode::Contain);
}

void draw_nine_slice(Context& ctx, const UiNineSlice& skin, Rectf bounds) {
    if (!skin.sprite.valid() || bounds.w <= 0.0f || bounds.h <= 0.0f) {
        return;
    }

    const Rectf src = skin.sprite.source;
    const f32 src_left = clamp_margin(skin.left, src.w);
    const f32 src_right = clamp_margin(skin.right, src.w - src_left);
    const f32 src_top = clamp_margin(skin.top, src.h);
    const f32 src_bottom = clamp_margin(skin.bottom, src.h - src_top);

    const f32 dst_left = clamp_margin(skin.left, bounds.w * 0.5f);
    const f32 dst_right = clamp_margin(skin.right, bounds.w - dst_left);
    const f32 dst_top = clamp_margin(skin.top, bounds.h * 0.5f);
    const f32 dst_bottom = clamp_margin(skin.bottom, bounds.h - dst_top);

    const std::array<f32, 4> sx{src.x, src.x + src_left, src.x + src.w - src_right, src.x + src.w};
    const std::array<f32, 4> sy{src.y, src.y + src_top, src.y + src.h - src_bottom, src.y + src.h};
    const std::array<f32, 4> dx{bounds.x, bounds.x + dst_left, bounds.x + bounds.w - dst_right, bounds.x + bounds.w};
    const std::array<f32, 4> dy{bounds.y, bounds.y + dst_top, bounds.y + bounds.h - dst_bottom, bounds.y + bounds.h};

    for (i32 y = 0; y < 3; ++y) {
        for (i32 x = 0; x < 3; ++x) {
            const Rectf part_src{sx[x], sy[y], sx[x + 1] - sx[x], sy[y + 1] - sy[y]};
            const Rectf part_dst{dx[x], dy[y], dx[x + 1] - dx[x], dy[y + 1] - dy[y]};
            if (part_src.w > 0.0f && part_src.h > 0.0f && part_dst.w > 0.0f && part_dst.h > 0.0f) {
                ctx.sprite({.texture = skin.sprite.texture, .source = part_src}, part_dst);
            }
        }
    }
}

Vec2f icon_button_content_size(const IconButton& widget) {
    const Vec2f icon = explicit_or_sprite_size(widget.icon_size, widget.icon);
    if (widget.label.empty() || widget.label_placement == IconLabelPlacement::None) {
        return icon;
    }
    const Vec2f text = measure_text(widget.text_style.font, widget.label, widget.text_style.scale);
    if (widget.label_placement == IconLabelPlacement::Below) {
        return {std::max(icon.x, text.x), icon.y + widget.style.padding.top + text.y};
    }
    return {icon.x + widget.style.padding.left + text.x, std::max(icon.y, text.y)};
}

std::pair<std::size_t, std::size_t> selection_range(const UiTextInputState& state) {
    return {state.selection_start(), state.selection_end()};
}

bool erase_selection(UiTextInputState& state) {
    if (!state.has_selection()) {
        return false;
    }
    const auto [start, end] = selection_range(state);
    state.text.erase(start, end - start);
    state.caret = start;
    state.clear_selection();
    return true;
}

std::string selected_text(const UiTextInputState& state) {
    if (!state.has_selection()) {
        return {};
    }
    const auto [start, end] = selection_range(state);
    return state.text.substr(start, end - start);
}

void move_text_caret(UiTextInputState& state, std::size_t caret, bool select) {
    caret = std::min(caret, state.text.size());
    if (!select) {
        state.caret = caret;
        state.clear_selection();
        return;
    }
    if (!state.has_selection()) {
        state.selection_anchor = state.caret;
    }
    state.caret = caret;
}

std::size_t caret_from_text_pos(const UiTextInputState& state, f32 x, f32 text_x, const Font& font, f32 scale) {
    if (state.text.empty()) {
        return 0;
    }
    return caret_at(font, state.text, x - text_x, scale);
}

f32 text_input_text_x(Rectf bounds, const TextInputLayoutMetrics& metrics, std::string_view text, const TextStyle& text_style) {
    if (paragraph_direction(text) != TextDirection::RightToLeft) {
        return bounds.x + metrics.padding.left;
    }
    // Right to left: the text sits against the right edge, as it reads from there.
    const f32 width = text.empty() ? 0.0f : measure_text(text_style.font, text, text_style.scale).x;
    return bounds.x + bounds.w - metrics.padding.right - width;
}

bool same_padding(UiPadding a, UiPadding b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

UiPadding intrinsic_padding(const WidgetStyle& style) {
    const UiPadding defaults{};
    if (same_padding(style.padding, defaults)) {
        return {12.0f, 6.0f, 12.0f, 6.0f};
    }
    return style.padding;
}

TextInputLayoutMetrics text_input_layout_metrics(const TextStyle& text_style, const WidgetStyle& style) {
    const UiPadding padding = intrinsic_padding(style);
    const f32 text_h = measure_text(text_style.font, "Mg", text_style.scale).y;
    return {
        .padding = padding,
        .text_height = text_h,
        // Floor at the shared control height (themed) so this control lines up with
        // Button in a row; 24px absolute floor keeps raw (min_height 0) sizing intact.
        .height = std::max(style.min_height, std::max(24.0f, text_h + padding.top + padding.bottom)),
    };
}

Color frame_color(const WidgetStyle& style, const Interaction& it, bool enabled) {
    // Text inputs intentionally use hover/press fill tokens as the frame color.
    if (!enabled) {
        return widget_fill(style, WidgetColorState::Disabled);
    }
    if (it.active) {
        return widget_fill(style, WidgetColorState::Pressed);
    }
    if (it.hot) {
        return widget_fill(style, WidgetColorState::Hovered);
    }
    if (it.focused) {
        return widget_border(style, WidgetColorState::Focused);
    }
    return widget_border(style, WidgetColorState::Normal);
}

void normalize_text_state(UiTextInputState& state) {
    state.caret = utf8_floor(state.text, state.caret);
    if (state.selection_anchor == UiTextInputState::unset_selection) {
        state.selection_anchor = state.caret;
    } else {
        state.selection_anchor = utf8_floor(state.text, state.selection_anchor);
    }
    if (!state.active) {
        state.clear_selection();
    }
}

UiTextInputResult edit_text_input(Context& ctx,
                                  UiTextInputState& state,
                                  Rectf bounds,
                                  const TextStyle& text_style,
                                  const WidgetStyle& style,
                                  const Interaction& it,
                                  bool enabled) {
    UiTextInputResult result;
    normalize_text_state(state);
    const TextInputLayoutMetrics metrics = text_input_layout_metrics(text_style, style);

    const bool was_active = state.active;
    if ((it.pressed || it.focused) && enabled) {
        state.active = true;
        if (it.pressed) {
            const f32 text_x = text_input_text_x(bounds, metrics, state.text, text_style);
            move_text_caret(state,
                            caret_from_text_pos(state, ctx.pointer().x, text_x, text_style.font, text_style.scale),
                            false);
        }
    }
    if (state.active && enabled && ctx.pointer_pressed(MouseButton::Left) && !contains(bounds, ctx.pointer())) {
        state.active = false;
        state.clear_selection();
    }
    if (!enabled) {
        state.active = false;
    }

    // While an input method composes, the keys are its own.
    const bool composing = state.active && !ctx.text_composition().empty();
    if (state.active) {
        ctx.request_text_input();
        // Typed (or committed by an input method) text, even mid-composition.
        const std::string_view input_text = ctx.text_input();
        if (!input_text.empty() && !ctx.modifier_held(KeyModifiers::Ctrl)) {
            erase_selection(state);
            state.text.insert(state.caret, input_text.data(), input_text.size());
            state.caret += input_text.size();
            state.clear_selection();
            result.changed = true;
        }
    }
    if (state.active && !composing) {
        // Right to left, the arrows still move the way they point.
        const bool rtl = paragraph_direction(state.text) == TextDirection::RightToLeft;
        const Key left_key = rtl ? Key::Right : Key::Left;
        const Key right_key = rtl ? Key::Left : Key::Right;
        const bool shift = ctx.modifier_held(KeyModifiers::Shift);
        const bool ctrl = ctx.modifier_held(KeyModifiers::Ctrl);
        const bool select_left = shift || (!ctx.key_pressed(Key::Left) && ctx.action_pressed("text_select_left"));
        const bool select_right = shift || (!ctx.key_pressed(Key::Right) && ctx.action_pressed("text_select_right"));
        const bool select_home = shift || (!ctx.key_pressed(Key::Home) && ctx.action_pressed("text_select_home"));
        const bool select_end = shift || (!ctx.key_pressed(Key::End) && ctx.action_pressed("text_select_end"));

        if (ctx.action_pressed("text_select_all") || (ctrl && ctx.key_pressed(Key::A))) {
            state.selection_anchor = 0;
            state.caret = state.text.size();
        }
        if (ctrl && ctx.key_pressed(Key::C)) {
            const std::string copy = selected_text(state);
            if (!copy.empty()) {
                ctx.set_clipboard_text(copy);
            }
        }
        if (ctrl && ctx.key_pressed(Key::X)) {
            const std::string copy = selected_text(state);
            if (!copy.empty()) {
                ctx.set_clipboard_text(copy);
                erase_selection(state);
                result.changed = true;
            }
        }
        if (ctrl && ctx.key_pressed(Key::V)) {
            // One line: pasted line breaks become spaces.
            std::string paste = normalize_newlines(ctx.clipboard_text());
            std::ranges::replace(paste, '\n', ' ');
            if (!paste.empty()) {
                erase_selection(state);
                state.text.insert(state.caret, paste);
                state.caret += paste.size();
                state.clear_selection();
                result.changed = true;
            }
        }

        // The arrows move the caret on screen, through text that runs both
        // ways; by word (Ctrl) they step through the text, the way they point.
        const auto visual = [&](i32 step) {
            return caret_move(text_style.font, state.text, state.caret, step, text_style.scale).value_or(state.caret);
        };
        if (ctx.key_typed(Key::Left) && !ctrl) {
            move_text_caret(state, visual(-1), select_left);
        } else if ((ctrl && ctx.key_typed(left_key)) || ctx.action_pressed("text_left")) {
            move_text_caret(state, ctrl ? utf8_word_left(state.text, state.caret) : utf8_prev(state.text, state.caret), select_left);
        }
        if (ctx.key_typed(Key::Right) && !ctrl) {
            move_text_caret(state, visual(1), select_right);
        } else if ((ctrl && ctx.key_typed(right_key)) || ctx.action_pressed("text_right")) {
            move_text_caret(state, ctrl ? utf8_word_right(state.text, state.caret) : utf8_next(state.text, state.caret), select_right);
        }
        if (ctx.key_pressed(Key::Home) || ctx.action_pressed("text_home")) {
            move_text_caret(state, 0, select_home);
        }
        if (ctx.key_pressed(Key::End) || ctx.action_pressed("text_end")) {
            move_text_caret(state, state.text.size(), select_end);
        }

        if (ctx.key_typed(Key::Backspace) || ctx.action_pressed("text_backspace")) {
            if (erase_selection(state)) {
                result.changed = true;
            } else if (state.caret > 0) {
                const std::size_t from = ctrl ? utf8_word_left(state.text, state.caret) : utf8_prev(state.text, state.caret);
                state.text.erase(from, state.caret - from);
                state.caret = from;
                state.clear_selection();
                result.changed = true;
            }
        }
        if (ctx.key_typed(Key::Delete) || ctx.action_pressed("text_delete")) {
            if (erase_selection(state)) {
                result.changed = true;
            } else if (state.caret < state.text.size()) {
                const std::size_t to = ctrl ? utf8_word_right(state.text, state.caret) : utf8_next(state.text, state.caret);
                state.text.erase(state.caret, to - state.caret);
                state.clear_selection();
                result.changed = true;
            }
        }
        if (was_active && ctx.action_pressed("accept")) {
            state.active = false;
            state.clear_selection();
            result.committed = true;
        } else if (was_active && ctx.action_pressed("quit")) {
            state.active = false;
            state.clear_selection();
            result.cancelled = true;
        }
    }
    return result;
}

void draw_text_input(Context& ctx, Rectf bounds, const UiTextInputState& state, const TextStyle& text_style, const WidgetStyle& style, Color frame) {
    ctx.surface(bounds, with_fill_border(ctx.theme().input_surface, style.track, frame));
    const TextInputLayoutMetrics metrics = text_input_layout_metrics(text_style, style);
    // What an input method is composing shows at the caret, underlined, until it is typed.
    const std::string_view composition = state.active ? ctx.text_composition() : std::string_view{};
    std::string shown;
    std::size_t caret = state.caret;
    if (!composition.empty()) {
        shown = state.text;
        shown.insert(state.caret, composition);
        caret = state.caret + static_cast<std::size_t>(std::clamp(ctx.text_composition_cursor(), 0, static_cast<i32>(composition.size())));
    }
    const std::string_view text = composition.empty() ? std::string_view{state.text} : std::string_view{shown};
    const Vec2f text_size = measure_text(text_style.font, text.empty() ? "Mg" : text, text_style.scale);
    const Vec2f text_pos{text_input_text_x(bounds, metrics, text, text_style), bounds.y + (bounds.h - text_size.y) * 0.5f};
    const f32 top = bounds.y + metrics.padding.top * 0.5f;
    const f32 height = std::max(0.0f, bounds.h - metrics.padding.top * 0.5f - metrics.padding.bottom * 0.5f);
    if (state.has_selection() && composition.empty()) {
        const auto [start, end] = selection_range(state);
        for (const auto& [x0, x1] : selection_spans(text_style.font, text, start, end, text_style.scale)) {
            ctx.fill_rect({text_pos.x + x0, top, std::max(1.0f, x1 - x0), height}, widget_border(style, WidgetColorState::Focused));
        }
    }
    ctx.text(text, text_pos, text_style);
    if (!composition.empty()) {
        for (const auto& [x0, x1] : selection_spans(text_style.font, text, state.caret, state.caret + composition.size(), text_style.scale)) {
            ctx.fill_rect({text_pos.x + x0, text_pos.y + text_size.y, std::max(1.0f, x1 - x0), std::max(1.0f, text_style.scale)},
                          text_style.color);
        }
    }
    if (state.active) {
        const f32 caret_x = text_pos.x + ui2::caret_x(text_style.font, text, caret, text_style.scale);
        ctx.fill_rect({caret_x, top, 1.0f, height}, text_style.color);
        ctx.set_text_input_area({bounds.x, top, bounds.w, height}, caret_x);
    }
}

bool parse_f32(std::string_view text, f32& out) {
    char* end = nullptr;
    std::string copy{text};
    const float value = std::strtof(copy.c_str(), &end);
    if (!end || end == copy.c_str()) {
        return false;
    }
    out = value;
    return true;
}

std::string format_f32(f32 value) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.3g", static_cast<double>(value));
    return buffer;
}

std::string format_value(f32 value, i32 decimals) {
    char buffer[48]{};
    const i32 clamped = std::clamp(decimals, 0, 3);
    std::snprintf(buffer, sizeof(buffer), "%.*f", clamped, static_cast<double>(value));
    return buffer;
}

Color alpha_scaled(Color color, f32 alpha) {
    color.a = static_cast<u8>(std::clamp(static_cast<f32>(color.a) * std::clamp(alpha, 0.0f, 1.0f), 0.0f, 255.0f));
    return color;
}

f32 life_alpha(f32 age, f32 lifetime) {
    if (lifetime <= 0.0f) {
        return 1.0f;
    }
    const f32 t = std::clamp(age / lifetime, 0.0f, 1.0f);
    if (t < 0.1f) {
        return std::clamp(t / 0.1f, 0.0f, 1.0f);
    }
    if (t > 0.82f) {
        return std::clamp((1.0f - t) / 0.18f, 0.0f, 1.0f);
    }
    return 1.0f;
}

std::string format_color_hex(Color color) {
    char buffer[16]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "#%02X%02X%02X%02X",
                  static_cast<unsigned>(color.r),
                  static_cast<unsigned>(color.g),
                  static_cast<unsigned>(color.b),
                  static_cast<unsigned>(color.a));
    return buffer;
}

u8 slider_to_channel(f32 value) {
    return static_cast<u8>(std::clamp(static_cast<i32>(std::round(value)), 0, 255));
}

f32 color_picker_height(const ColorPicker& widget) {
    const f32 rgba_rows = static_cast<f32>(widget.show_alpha ? 5 : 4) * widget.row_height +
                          static_cast<f32>(widget.show_alpha ? 4 : 3) * widget.row_spacing;
    const f32 hsv_rows = widget.picker_size + widget.row_spacing + widget.hue_height +
                         (widget.show_alpha ? widget.row_spacing + widget.hue_height : 0.0f) +
                         widget.row_spacing + std::max(widget.row_height, widget.swatch_size);
    const i32 swatch_rows = 3;
    const f32 suggested_rows = static_cast<f32>(swatch_rows) * widget.swatch_size + static_cast<f32>(swatch_rows - 1) * widget.swatch_gap +
                               widget.row_spacing + std::max(widget.row_height, widget.swatch_size);
    return 12.0f + widget.tab_height + widget.row_spacing + std::max({rgba_rows, hsv_rows, suggested_rows});
}

HsvColor rgb_to_hsv(Color color) {
    const f32 r = static_cast<f32>(color.r) / 255.0f;
    const f32 g = static_cast<f32>(color.g) / 255.0f;
    const f32 b = static_cast<f32>(color.b) / 255.0f;
    const f32 max_c = std::max({r, g, b});
    const f32 min_c = std::min({r, g, b});
    const f32 delta = max_c - min_c;
    HsvColor hsv{};
    hsv.v = max_c;
    hsv.s = max_c <= 0.0f ? 0.0f : delta / max_c;
    if (delta <= 0.0f) {
        hsv.h = 0.0f;
    } else if (max_c == r) {
        hsv.h = std::fmod((g - b) / delta, 6.0f) / 6.0f;
    } else if (max_c == g) {
        hsv.h = ((b - r) / delta + 2.0f) / 6.0f;
    } else {
        hsv.h = ((r - g) / delta + 4.0f) / 6.0f;
    }
    if (hsv.h < 0.0f) {
        hsv.h += 1.0f;
    }
    return hsv;
}

Color hsv_to_rgb(f32 h, f32 s, f32 v, u8 alpha) {
    h = h - std::floor(h);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const f32 scaled = h * 6.0f;
    const i32 sector = static_cast<i32>(std::floor(scaled));
    const f32 f = scaled - static_cast<f32>(sector);
    const f32 p = v * (1.0f - s);
    const f32 q = v * (1.0f - f * s);
    const f32 t = v * (1.0f - (1.0f - f) * s);
    f32 r = v;
    f32 g = t;
    f32 b = p;
    switch (sector % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    }
    return Color::rgba(slider_to_channel(r * 255.0f), slider_to_channel(g * 255.0f), slider_to_channel(b * 255.0f), alpha);
}

bool menu_item_selectable(const MenuListItem& item) {
    return item.enabled && !item.separator;
}

std::string menu_item_result_id(const MenuListItem& item) {
    return item.id.empty() ? item.label : item.id;
}

i32 next_menu_index(const std::vector<MenuListItem>& items, i32 current, i32 delta, bool wrap) {
    const i32 count = static_cast<i32>(items.size());
    if (count <= 0) {
        return 0;
    }
    i32 next = std::clamp(current, 0, count - 1);
    for (i32 tries = 0; tries < count; ++tries) {
        next += delta;
        if (wrap) {
            next = (next + count) % count;
        } else {
            next = std::clamp(next, 0, count - 1);
        }
        if (menu_item_selectable(items[static_cast<std::size_t>(next)])) {
            return next;
        }
        if (!wrap && (next == 0 || next == count - 1)) {
            break;
        }
    }
    return current;
}

i32 first_menu_index(const std::vector<MenuListItem>& items) {
    for (i32 i = 0; i < static_cast<i32>(items.size()); ++i) {
        if (menu_item_selectable(items[static_cast<std::size_t>(i)])) {
            return i;
        }
    }
    return 0;
}

std::string item_result_id(std::string_view id, std::string_view fallback) {
    return id.empty() ? std::string{fallback} : std::string{id};
}

TabBarLayoutMetrics tab_bar_layout_metrics(const TabBar& widget, const TextStyle& text_style, const WidgetStyle& style) {
    const UiPadding padding = intrinsic_padding(style);
    const f32 text_h = measure_text(text_style.font, "Mg", text_style.scale).y;
    const f32 icon = std::max(0.0f, text_h);
    const f32 close = std::max(0.0f, text_h);
    const f32 dirty = std::max(4.0f, text_h * 0.35f);
    return {
        .padding = padding,
        .tab_height = std::max(widget.tab_height, std::max(text_h, std::max(icon, close)) + padding.top + padding.bottom),
        .icon_size = icon,
        .close_size = close,
        .dirty_size = dirty,
        .item_gap = std::max(2.0f, padding.left * 0.5f),
    };
}

f32 tab_width(const TabBar& widget, const TabBarItem& item, const TextStyle& text_style, const TabBarLayoutMetrics& metrics) {
    const f32 text = measure_text(text_style.font, item.label, text_style.scale).x;
    const f32 icon = item.icon.valid() ? metrics.icon_size + metrics.item_gap : 0.0f;
    const f32 close = item.closable ? metrics.close_size + metrics.item_gap : 0.0f;
    const f32 dirty = item.dirty ? metrics.dirty_size + metrics.item_gap : 0.0f;
    return std::clamp(text + icon + close + dirty + metrics.padding.left + metrics.padding.right,
                      widget.min_tab_width,
                      widget.max_tab_width);
}

i32 next_enabled_tab(const TabBar& widget, i32 current, i32 delta) {
    const i32 count = static_cast<i32>(widget.items.size());
    if (count <= 0) {
        return 0;
    }
    i32 next = std::clamp(current, 0, count - 1);
    for (i32 tries = 0; tries < count; ++tries) {
        next += delta;
        if (widget.wrap) {
            next = (next + count) % count;
        } else {
            next = std::clamp(next, 0, count - 1);
        }
        if (widget.items[static_cast<std::size_t>(next)].enabled) {
            return next;
        }
        if (!widget.wrap && (next == 0 || next == count - 1)) {
            break;
        }
    }
    return current;
}

std::string lower_copy(std::string_view value) {
    std::string out{value};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool asset_matches_filter(const LogEntry& entry, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    const std::string f = lower_copy(filter);
    return lower_copy(entry.text).find(f) != std::string::npos ||
           lower_copy(entry.timestamp).find(f) != std::string::npos;
}

Color severity_color(LogSeverity severity, const TextStyle& style, const Theme& theme) {
    switch (severity) {
    case LogSeverity::Info:
        return style.color;
    case LogSeverity::Warning:
        return theme.colors.solid_warning;
    case LogSeverity::Error:
        return theme.colors.solid_danger;
    }
    return style.color;
}

Vec2f slot_icon_size(Vec2f explicit_size, const Sprite& icon, Rectf content) {
    if (explicit_size.x > 0.0f && explicit_size.y > 0.0f) {
        return explicit_size;
    }
    if (icon.valid()) {
        return {std::min(content.w, icon.source.w), std::min(content.h, icon.source.h)};
    }
    return {0.0f, 0.0f};
}

void draw_slot_contents(Context& ctx,
                        Rectf bounds,
                        const Sprite& icon,
                        Vec2f icon_size,
                        std::string_view text,
                        i32 count,
                        const TextStyle& text_style,
                        const WidgetStyle& style,
                        Color icon_tint) {
    const Rectf content = inset(bounds, style.padding);
    const Vec2f size = slot_icon_size(icon_size, icon, content);
    if (icon.valid() && size.x > 0.0f && size.y > 0.0f) {
        ctx.sprite(icon, align_rect(content, size, UiAlign::Center, UiAlign::Center), icon_tint);
    }
    if (!text.empty()) {
        const Vec2f text_size = measure_text(text_style.font, text, text_style.scale);
        ctx.text(text, {content.x + (content.w - text_size.x) * 0.5f, content.y + content.h - text_size.y}, text_style);
    }
    if (count > 0) {
        const std::string badge = std::to_string(count);
        const Vec2f badge_text = measure_text(text_style.font, badge, text_style.scale);
        const Rectf badge_rect{bounds.x + bounds.w - badge_text.x - 8.0f, bounds.y + bounds.h - badge_text.y - 6.0f, badge_text.x + 6.0f, badge_text.y + 4.0f};
        ctx.fill_rect(badge_rect, style.track);
        ctx.outline_rect(badge_rect, widget_border(style, WidgetColorState::Normal));
        ctx.text(badge, {badge_rect.x + 3.0f, badge_rect.y + 2.0f}, text_style);
    }
}

Rectf icon_grid_cell_rect(const IconGrid& widget, i32 index) {
    const i32 columns = std::max(1, widget.columns);
    const i32 col = index % columns;
    const i32 row = index / columns;
    return {
        widget.bounds.x + static_cast<f32>(col) * (widget.cell_size.x + widget.spacing.x),
        widget.bounds.y + static_cast<f32>(row) * (widget.cell_size.y + widget.spacing.y) - widget.offset,
        widget.cell_size.x,
        widget.cell_size.y,
    };
}

u8 color_channel_slider(Context& ctx,
                        Id id,
                        Rectf row,
                        std::string_view label,
                        u8 value,
                        const ColorPicker& widget,
                        bool& changed) {
    const Rectf label_rect{row.x, row.y, widget.label_width, row.h};
    const Rectf slider_rect{row.x + widget.label_width + 4.0f, row.y, std::max(0.0f, row.w - widget.label_width - 4.0f), row.h};
    const Vec2f label_size = measure_text(widget.text_style.font, label, widget.text_style.scale);
    ctx.text(label, {label_rect.x, label_rect.y + (label_rect.h - label_size.y) * 0.5f}, widget.text_style);

    Slider slider{
        .id = id,
        .bounds = slider_rect,
        .value = static_cast<f32>(value),
        .min = 0.0f,
        .max = 255.0f,
        .step = 1.0f,
        .style = widget.style,
        .enabled = widget.enabled,
        .z = widget.z,
    };
    run(ctx, slider);
    const u8 next = slider_to_channel(slider.value);
    changed = changed || next != value;
    return next;
}

f32 color_float_slider(Context& ctx,
                       Id id,
                       Rectf row,
                       std::string_view label,
                       f32 value,
                       f32 min,
                       f32 max,
                       const ColorPicker& widget,
                       bool& changed) {
    const Rectf label_rect{row.x, row.y, widget.label_width, row.h};
    const Rectf slider_rect{row.x + widget.label_width + 4.0f, row.y, std::max(0.0f, row.w - widget.label_width - 4.0f), row.h};
    const Vec2f label_size = measure_text(widget.text_style.font, label, widget.text_style.scale);
    ctx.text(label, {label_rect.x, label_rect.y + (label_rect.h - label_size.y) * 0.5f}, widget.text_style);
    Slider slider{
        .id = id,
        .bounds = slider_rect,
        .value = value,
        .min = min,
        .max = max,
        .step = 0.0f,
        .style = widget.style,
        .enabled = widget.enabled,
        .z = widget.z,
    };
    run(ctx, slider);
    const f32 next = std::clamp(slider.value, min, max);
    changed = changed || std::abs(next - value) > 0.0001f;
    return next;
}

std::vector<Color> color_picker_suggestions(const ColorPicker& widget, const Theme& theme) {
    if (!widget.suggested.empty()) {
        return widget.suggested;
    }
    return {
        theme.colors.solid_accent,
        theme.colors.solid_success,
        theme.colors.solid_info,
        theme.colors.solid_warning,
        theme.colors.solid_danger,
        theme.colors.text_emphasis,
        theme.colors.text,
        theme.colors.text_muted,
        theme.colors.surface_panel,
        theme.colors.surface_card,
        theme.scales.white_alpha[11],
        theme.scales.black_alpha[11],
        theme.scales.danger[9],
        theme.scales.warning[9],
        theme.scales.success[9],
        theme.scales.info[9],
        theme.scales.accent[10],
        theme.scales.accent_alpha[7],
    };
}

const char* color_picker_mode_label(ColorPickerMode mode) {
    switch (mode) {
    case ColorPickerMode::Rgba: return "RGBA";
    case ColorPickerMode::Hsv: return "HSV";
    case ColorPickerMode::Suggested: return "Suggested";
    }
    return "Color";
}

Id property_row_id(const PropertyGrid& widget, const PropertyGridRow& row, i32 index) {
    if (row.id) {
        return row.id;
    }
    const Id base = widget.id ? widget.id : make_id("property_grid");
    if (!row.label.empty()) {
        return make_id(base, row.label);
    }
    return make_id(base, static_cast<u64>(index));
}

f32 property_grid_content_height(const PropertyGrid& widget) {
    const i32 count = static_cast<i32>(widget.rows.size());
    if (count <= 0) {
        return 0.0f;
    }
    return static_cast<f32>(count) * widget.row_height + static_cast<f32>(count - 1) * widget.row_spacing;
}

std::string inspector_row_result_id(const PropertyInspectorRow& row, i32 index) {
    if (!row.string_id.empty()) {
        return row.string_id;
    }
    if (!row.label.empty()) {
        return row.label;
    }
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%d", index);
    return buffer;
}

Id property_inspector_row_id(const PropertyInspector& widget, const PropertyInspectorRow& row, i32 index) {
    if (row.id) {
        return row.id;
    }
    const Id base = widget.id ? widget.id : make_id("property_inspector");
    const std::string result = inspector_row_result_id(row, index);
    return make_id(base, result);
}

f32 property_inspector_marker_gutter(const TextStyle& label_style, UiPadding padding) {
    const f32 marker_size = std::clamp(measure_text(label_style.font, "Mg", label_style.scale).y * 0.35f, 4.0f, 6.0f);
    const f32 marker_gap = std::max(4.0f, padding.left * 0.5f);
    return marker_size + marker_gap;
}

PropertyInspectorLayoutMetrics property_inspector_layout_metrics(const PropertyInspector& widget) {
    const UiPadding padding = intrinsic_padding(widget.style);
    const f32 label_text_h = measure_text(widget.label_style.font, "Mg", widget.label_style.scale).y;
    const f32 value_text_h = measure_text(widget.value_style.font, "Mg", widget.value_style.scale).y;
    const f32 text_h = std::max(label_text_h, value_text_h);
    f32 label_w = std::max(0.0f, widget.label_width);
    const f32 marker_gutter = property_inspector_marker_gutter(widget.label_style, padding);
    for (const PropertyInspectorRow& row : widget.rows) {
        if (!row.label.empty()) {
            label_w = std::max(label_w, marker_gutter + measure_text(widget.label_style.font, row.label, widget.label_style.scale).x + padding.left);
        }
    }
    const f32 natural_row_h = text_h + padding.top + padding.bottom;
    return {
        .row_height = std::max(widget.row_height, natural_row_h),
        .section_height = std::max(widget.section_height, natural_row_h),
        .control_height = std::max(widget.control_height, value_text_h + padding.top + padding.bottom),
        .label_width = label_w,
        .label_gap = std::max(widget.label_gap, padding.left * 0.5f),
        .padding = padding,
    };
}

f32 property_inspector_row_height(const PropertyInspector& widget, const PropertyInspectorLayoutMetrics& metrics, const PropertyInspectorRow& row) {
    if (row.kind == PropertyInspectorRowKind::Separator) {
        return std::max(1.0f, widget.row_spacing);
    }
    if (row.kind == PropertyInspectorRowKind::Section) {
        return metrics.section_height;
    }
    return metrics.row_height;
}

f32 property_inspector_row_height(const PropertyInspector& widget, const PropertyInspectorRow& row) {
    return property_inspector_row_height(widget, property_inspector_layout_metrics(widget), row);
}

f32 property_inspector_content_height(const PropertyInspector& widget, const PropertyInspectorLayoutMetrics& metrics) {
    f32 height = 0.0f;
    bool first = true;
    bool section_open = true;
    for (const PropertyInspectorRow& row : widget.rows) {
        if (row.kind != PropertyInspectorRowKind::Section && !section_open) {
            continue;
        }
        if (!first) {
            height += widget.row_spacing;
        }
        height += property_inspector_row_height(widget, metrics, row);
        first = false;
        if (row.kind == PropertyInspectorRowKind::Section) {
            section_open = row.expanded;
        }
    }
    return height;
}

f32 property_inspector_content_height(const PropertyInspector& widget) {
    return property_inspector_content_height(widget, property_inspector_layout_metrics(widget));
}

Vec2f node_to_screen(const NodeGraph& graph, Vec2f p) {
    return {graph.bounds.x + graph.pan.x + p.x * graph.zoom, graph.bounds.y + graph.pan.y + p.y * graph.zoom};
}

Vec2f screen_to_node(const NodeGraph& graph, Vec2f p) {
    const f32 zoom = graph.zoom != 0.0f ? graph.zoom : 1.0f;
    return {(p.x - graph.bounds.x - graph.pan.x) / zoom, (p.y - graph.bounds.y - graph.pan.y) / zoom};
}

Rectf node_rect_to_screen(const NodeGraph& graph, Rectf rect) {
    const Vec2f p = node_to_screen(graph, {rect.x, rect.y});
    return {p.x, p.y, rect.w * graph.zoom, rect.h * graph.zoom};
}

f32 length_sq(Vec2f v) {
    return v.x * v.x + v.y * v.y;
}

Vec2f normalize_or(Vec2f v, Vec2f fallback) {
    const f32 len = std::sqrt(length_sq(v));
    if (len <= 0.001f) {
        return fallback;
    }
    return {v.x / len, v.y / len};
}

f32 point_segment_distance_sq(Vec2f p, Vec2f a, Vec2f b) {
    const Vec2f ab{b.x - a.x, b.y - a.y};
    const f32 denom = length_sq(ab);
    if (denom <= 0.001f) {
        return length_sq({p.x - a.x, p.y - a.y});
    }
    const f32 t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / denom, 0.0f, 1.0f);
    const Vec2f q{a.x + ab.x * t, a.y + ab.y * t};
    return length_sq({p.x - q.x, p.y - q.y});
}

bool node_graph_ports_compatible(const NodeGraphPort& from, const NodeGraphPort& to) {
    if (!from.enabled || !to.enabled || from.kind != NodeGraphPortKind::Output || to.kind != NodeGraphPortKind::Input) {
        return false;
    }
    return from.type.empty() || to.type.empty() || from.type == to.type;
}

Rectf normalized_rect(Vec2f a, Vec2f b) {
    const f32 x0 = std::min(a.x, b.x);
    const f32 y0 = std::min(a.y, b.y);
    const f32 x1 = std::max(a.x, b.x);
    const f32 y1 = std::max(a.y, b.y);
    return {x0, y0, x1 - x0, y1 - y0};
}

f32 axis_value(Vec2f value, ScrollAxis axis) {
    return axis == ScrollAxis::Vertical ? value.y : value.x;
}

f32 rect_axis_pos(Rectf rect, ScrollAxis axis) {
    return axis == ScrollAxis::Vertical ? rect.y : rect.x;
}

f32 rect_axis_size(Rectf rect, ScrollAxis axis) {
    return axis == ScrollAxis::Vertical ? rect.h : rect.w;
}

Rectf axis_thumb(Rectf track, ScrollAxis axis, f32 pos, f32 size) {
    if (axis == ScrollAxis::Vertical) {
        return {track.x, pos, track.w, size};
    }
    return {pos, track.y, size, track.h};
}

bool set_axis_offset(ScrollState& state, ScrollAxis axis, f32 value) {
    const Vec2f max_offset = max_scroll_offset(state);
    if (axis == ScrollAxis::Vertical) {
        const f32 next = std::clamp(value, 0.0f, max_offset.y);
        const bool changed = next != state.offset.y;
        state.offset.y = next;
        return changed;
    }
    const f32 next = std::clamp(value, 0.0f, max_offset.x);
    const bool changed = next != state.offset.x;
    state.offset.x = next;
    return changed;
}

f32 scroll_offset_for_thumb(Rectf track, Rectf thumb, ScrollAxis axis, f32 pointer, f32 grab, const ScrollState& state) {
    const f32 travel = std::max(0.0f, rect_axis_size(track, axis) - rect_axis_size(thumb, axis));
    if (travel <= 0.0f) {
        return axis_value(state.offset, axis);
    }
    const f32 local = std::clamp(pointer - grab - rect_axis_pos(track, axis), 0.0f, travel);
    return (local / travel) * axis_value(max_scroll_offset(state), axis);
}

ScrollResult scroll_axis_interaction(Context& ctx,
                                     Id id,
                                     ScrollAxis axis,
                                     Rectf viewport,
                                     Rectf track,
                                     ScrollState state,
                                     ScrollOptions options,
                                     i32 z,
                                     f32* grab_offset) {
    ScrollResult result{.state = state};
    ScrollbarLayout layout = layout_scrollbar(track, state, axis, options);
    if (!layout.scrollable || !options.enabled) {
        return result;
    }

    const Id thumb_id = make_id(id, axis == ScrollAxis::Vertical ? "vthumb" : "hthumb");
    const Id track_id = make_id(id, axis == ScrollAxis::Vertical ? "vtrack" : "htrack");
    const Interaction thumb = ctx.region(thumb_id, layout.thumb, z + 1);
    const Interaction track_it = ctx.region(track_id, layout.track, z);
    const f32 pointer = axis == ScrollAxis::Vertical ? ctx.pointer().y : ctx.pointer().x;
    const f32 page = std::max(1.0f, axis_value(state.viewport_size, axis) * 0.85f);
    // Callers that don't own a grab slot (e.g. LogConsole) share this fallback. Safe because at
    // most one region is `active` at a time (State holds a single _active id), so only one thumb
    // can ever be mid-drag — there is never contention for this static.
    static f32 fallback_grab = 0.0f;
    f32& grab = grab_offset ? *grab_offset : fallback_grab;
    (void)viewport;

    if (thumb.pressed) {
        grab = pointer - rect_axis_pos(layout.thumb, axis);
    }
    if (thumb.active && ctx.pointer_held()) {
        result.changed = set_axis_offset(result.state,
                                         axis,
                                         scroll_offset_for_thumb(layout.track, layout.thumb, axis, pointer, grab, result.state));
        result.dragging = true;
    } else if (track_it.clicked && !contains(layout.thumb, ctx.pointer())) {
        const bool before = pointer < rect_axis_pos(layout.thumb, axis);
        result.changed = scroll_by(result.state, axis == ScrollAxis::Vertical
                                                     ? Vec2f{0.0f, before ? -page : page}
                                                     : Vec2f{before ? -page : page, 0.0f});
    }
    return result;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

i32 hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

bool parse_hex_color(std::string_view value, Color& out) {
    if (value.size() != 7 || value[0] != '#') {
        return false;
    }
    i32 components[6]{};
    for (std::size_t i = 0; i < 6; ++i) {
        components[i] = hex_value(value[i + 1]);
        if (components[i] < 0) {
            return false;
        }
    }
    out = Color::rgb(static_cast<u8>(components[0] * 16 + components[1]),
                     static_cast<u8>(components[2] * 16 + components[3]),
                     static_cast<u8>(components[4] * 16 + components[5]));
    return true;
}

f32 parse_scale(std::string_view value, f32 fallback) {
    char* end = nullptr;
    std::string copy{value};
    const float parsed = std::strtof(copy.c_str(), &end);
    if (!end || end == copy.c_str() || parsed <= 0.0f) {
        return fallback;
    }
    return parsed;
}

std::string closing_tag_for(std::string_view tag) {
    if (starts_with(tag, "color=")) {
        return "[/color]";
    }
    if (starts_with(tag, "scale=")) {
        return "[/scale]";
    }
    if (starts_with(tag, "font=")) {
        return "[/font]";
    }
    if (starts_with(tag, "link=")) {
        return "[/link]";
    }
    if (tag == "u") {
        return "[/u]";
    }
    if (tag == "s") {
        return "[/s]";
    }
    return {};
}

bool apply_markup_tag(std::string_view tag, AdvancedTextParseState& state, const AdvancedTextMarkupOptions& options) {
    if (starts_with(tag, "color=")) {
        Color color{};
        if (!parse_hex_color(tag.substr(6), color)) {
            return false;
        }
        state.style.color = color;
        return true;
    }
    if (starts_with(tag, "scale=")) {
        state.style.scale = parse_scale(tag.substr(6), state.style.scale);
        return true;
    }
    if (starts_with(tag, "font=")) {
        if (options.font_resolver) {
            if (Font font = options.font_resolver(tag.substr(5)); font) {
                state.style.font = font;
            }
        }
        return true;
    }
    if (starts_with(tag, "link=")) {
        state.link = true;
        state.id = std::string{tag.substr(5)};
        return !state.id.empty();
    }
    if (tag == "u") {
        state.underline = true;
        return true;
    }
    if (tag == "s") {
        state.strike = true;
        return true;
    }
    return false;
}

void push_text_run(AdvancedTextContent& content, std::string text, const AdvancedTextParseState& state) {
    if (text.empty()) {
        return;
    }
    content.runs.push_back(TextRun{
        .text = std::move(text),
        .style = state.style,
        .id = state.id,
        .underline = state.underline,
        .strike = state.strike,
        .link = state.link,
    });
}

f32 line_align_offset(f32 line_width, f32 content_width, UiAlign align) {
    if (align == UiAlign::Center) {
        return std::max(0.0f, (content_width - line_width) * 0.5f);
    }
    if (align == UiAlign::End) {
        return std::max(0.0f, content_width - line_width);
    }
    return 0.0f;
}

AdvancedTextContent advanced_text_content(const AdvancedText& widget) {
    if (widget.markup.empty()) {
        return widget.content;
    }
    if (widget.cache_enabled && widget.parsed_cache_valid && widget.cached_markup == widget.markup) {
        return widget.cached_content;
    }
    AdvancedTextContent parsed = parse_advanced_text(widget.markup, widget.markup_options);
    if (widget.cache_enabled) {
        widget.cached_markup = widget.markup;
        widget.cached_content = parsed;
        widget.parsed_cache_valid = true;
    }
    return parsed;
}

bool advanced_text_options_equal(const AdvancedTextLayoutOptions& lhs, const AdvancedTextLayoutOptions& rhs) {
    return lhs.max_width == rhs.max_width &&
        lhs.line_spacing == rhs.line_spacing &&
        lhs.max_lines == rhs.max_lines &&
        lhs.wrap == rhs.wrap &&
        lhs.break_long_words == rhs.break_long_words &&
        lhs.padding.left == rhs.padding.left &&
        lhs.padding.top == rhs.padding.top &&
        lhs.padding.right == rhs.padding.right &&
        lhs.padding.bottom == rhs.padding.bottom &&
        lhs.horizontal == rhs.horizontal &&
        lhs.vertical == rhs.vertical &&
        lhs.overflow == rhs.overflow;
}

AdvancedTextLayout advanced_text_layout(const AdvancedText& widget, AdvancedTextLayoutOptions options) {
    const AdvancedTextContent content = advanced_text_content(widget);
    if (widget.markup.empty() || !widget.cache_enabled) {
        return layout_advanced_text(content, options);
    }
    if (widget.layout_cache_valid &&
        widget.cached_layout_markup == widget.markup &&
        advanced_text_options_equal(widget.cached_layout_options, options)) {
        return widget.cached_layout;
    }
    AdvancedTextLayout layout = layout_advanced_text(content, options);
    widget.cached_layout_markup = widget.markup;
    widget.cached_layout_options = options;
    widget.cached_layout = layout;
    widget.layout_cache_valid = true;
    return layout;
}

bool has_advanced_content(const AdvancedTextContent& content) {
    return !content.runs.empty();
}

AdvancedTextContent dialog_advanced_content(const AdvancedTextContent& content,
                                            std::string_view markup,
                                            AdvancedTextMarkupOptions options,
                                            TextStyle fallback_style,
                                            std::string_view fallback_text) {
    if (!markup.empty()) {
        if (options.base_style.scale == TextStyle{}.scale && options.base_style.color == TextStyle{}.color && !options.base_style.font) {
            options.base_style = fallback_style;
        }
        return parse_advanced_text(markup, options);
    }
    if (has_advanced_content(content)) {
        return content;
    }
    if (!fallback_text.empty()) {
        return {.runs = {TextRun{.text = std::string{fallback_text}, .style = fallback_style}}};
    }
    return {};
}

} // namespace kin::ui2::detail
