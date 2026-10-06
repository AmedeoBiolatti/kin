#include <kin/ui2/geometry.hpp>

#include <kin/ui2/text.hpp>

#include "mirror_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace kin::ui2 {
namespace {

std::atomic<TextDirection> g_ui_direction{TextDirection::LeftToRight};
thread_local int g_mirror_depth = 0;

f32 aligned_pos(f32 parent_pos, f32 parent_size, f32 item_size, UiAlign align) {
    switch (align) {
    case UiAlign::Start: return parent_pos;
    case UiAlign::Center: return parent_pos + (parent_size - item_size) * 0.5f;
    case UiAlign::End: return parent_pos + parent_size - item_size;
    case UiAlign::Stretch: return parent_pos;
    }
    return parent_pos;
}

f32 aligned_size(f32 parent_size, f32 item_size, UiAlign align) {
    return align == UiAlign::Stretch ? parent_size : item_size;
}

} // namespace

Rectf UiCursor::rect(Vec2f size) const {
    return {pos.x, pos.y, size.x, size.y};
}

Rectf UiCursor::take_horizontal(Vec2f size) {
    const Rectf result = rect(size);
    skip_horizontal(size.x);
    return result;
}

Rectf UiCursor::take_vertical(Vec2f size) {
    const Rectf result = rect(size);
    skip_vertical(size.y);
    return result;
}

void UiCursor::skip_horizontal(f32 amount) {
    pos.x += amount + spacing;
}

void UiCursor::skip_vertical(f32 amount) {
    pos.y += amount + spacing;
}

Rectf UiContainer::content() const {
    return inset(bounds, padding);
}

UiCursor UiContainer::row(f32 cursor_spacing) const {
    const Rectf area = content();
    return {{area.x, area.y}, cursor_spacing};
}

UiCursor UiContainer::column(f32 cursor_spacing) const {
    const Rectf area = content();
    return {{area.x, area.y}, cursor_spacing};
}

Rectf UiGrid::cell(i32 index) const {
    const i32 safe_columns = std::max(1, columns);
    const i32 col = index % safe_columns;
    const i32 row = index / safe_columns;
    return {
        bounds.x + static_cast<f32>(col) * (cell_size.x + spacing.x),
        bounds.y + static_cast<f32>(row) * (cell_size.y + spacing.y),
        cell_size.x,
        cell_size.y,
    };
}

std::vector<Rectf> UiFlowLayout::items(i32 item_count) const {
    std::vector<Rectf> result;
    if (item_count <= 0 || item_size.x <= 0.0f || item_size.y <= 0.0f || bounds.w <= 0.0f || bounds.h <= 0.0f) {
        return result;
    }

    result.reserve(static_cast<std::size_t>(item_count));
    if (axis == UiLayoutAxis::Horizontal) {
        const f32 step_x = item_size.x + spacing.x;
        const f32 step_y = item_size.y + spacing.y;
        const i32 per_line = std::max(1, static_cast<i32>(std::floor((bounds.w + spacing.x) / std::max(1.0f, step_x))));
        for (i32 i = 0; i < item_count; ++i) {
            const i32 col = i % per_line;
            const i32 row = i / per_line;
            result.push_back({
                bounds.x + static_cast<f32>(col) * step_x,
                bounds.y + static_cast<f32>(row) * step_y,
                item_size.x,
                item_size.y,
            });
        }
    } else {
        const f32 step_x = item_size.x + spacing.x;
        const f32 step_y = item_size.y + spacing.y;
        const i32 per_column = std::max(1, static_cast<i32>(std::floor((bounds.h + spacing.y) / std::max(1.0f, step_y))));
        for (i32 i = 0; i < item_count; ++i) {
            const i32 row = i % per_column;
            const i32 col = i / per_column;
            result.push_back({
                bounds.x + static_cast<f32>(col) * step_x,
                bounds.y + static_cast<f32>(row) * step_y,
                item_size.x,
                item_size.y,
            });
        }
    }
    return result;
}

std::optional<i32> UiFlowLayout::hit_test(Vec2f pos, i32 item_count) const {
    const std::vector<Rectf> rects = items(item_count);
    for (i32 i = 0; i < static_cast<i32>(rects.size()); ++i) {
        if (contains(rects[static_cast<std::size_t>(i)], pos)) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<i32> UiGrid::hit_test(Vec2f pos, i32 item_count) const {
    for (i32 i = 0; i < item_count; ++i) {
        if (contains(cell(i), pos)) {
            return i;
        }
    }
    return std::nullopt;
}

UiHudLayout UiHudLayout::from_size(Vec2f size, f32 layout_margin) {
    return {
        .bounds = safe_area(size, layout_margin),
        .margin = layout_margin,
    };
}

Vec2f UiHudLayout::pos(UiAnchor anchor, Vec2f size, Vec2f offset) const {
    return anchor_pos(bounds, anchor, size, offset);
}

Rectf UiHudLayout::rect(UiAnchor anchor, Vec2f size, Vec2f offset) const {
    return anchor_rect(bounds, anchor, size, offset);
}

Rectf inset(Rectf rect, f32 amount) {
    return inset(rect, padding(amount));
}

Rectf inset(Rectf rect, UiPadding amount) {
    const f32 x = rect.x + amount.left;
    const f32 y = rect.y + amount.top;
    const f32 w = std::max(0.0f, rect.w - amount.left - amount.right);
    const f32 h = std::max(0.0f, rect.h - amount.top - amount.bottom);
    return {x, y, w, h};
}

Rectf outset(Rectf rect, f32 amount) {
    return outset(rect, padding(amount));
}

Rectf outset(Rectf rect, UiPadding amount) {
    return {
        rect.x - amount.left,
        rect.y - amount.top,
        rect.w + amount.left + amount.right,
        rect.h + amount.top + amount.bottom,
    };
}

Rectf safe_area(Vec2f size, f32 margin) {
    return {
        margin,
        margin,
        std::max(0.0f, size.x - margin * 2.0f),
        std::max(0.0f, size.y - margin * 2.0f),
    };
}

Rectf centered_rect(Vec2f center, Vec2f size) {
    return {center.x - size.x * 0.5f, center.y - size.y * 0.5f, size.x, size.y};
}

namespace mirror_detail {
int depth() { return g_mirror_depth; }
void enter() { ++g_mirror_depth; }
void leave() { --g_mirror_depth; }
} // namespace mirror_detail

void set_ui_direction(TextDirection direction) {
    g_ui_direction.store(direction, std::memory_order_relaxed);
    set_text_base_direction(direction == TextDirection::RightToLeft ? std::optional{direction} : std::nullopt);
}

TextDirection ui_direction() {
    return g_ui_direction.load(std::memory_order_relaxed);
}

Rectf align_rect(Rectf parent, Vec2f size, UiAlign horizontal, UiAlign vertical) {
    if (ui_direction() == TextDirection::RightToLeft && g_mirror_depth == 0) {
        horizontal = horizontal == UiAlign::Start ? UiAlign::End : horizontal == UiAlign::End ? UiAlign::Start : horizontal;
    }
    const f32 w = aligned_size(parent.w, size.x, horizontal);
    const f32 h = aligned_size(parent.h, size.y, vertical);
    return {
        aligned_pos(parent.x, parent.w, w, horizontal),
        aligned_pos(parent.y, parent.h, h, vertical),
        w,
        h,
    };
}

Vec2f anchor_pos(Rectf parent, UiAnchor anchor, Vec2f size, Vec2f offset) {
    Vec2f pos{parent.x, parent.y};
    switch (anchor) {
    case UiAnchor::TopLeft:
        pos = {parent.x, parent.y};
        break;
    case UiAnchor::TopCenter:
        pos = {parent.x + (parent.w - size.x) * 0.5f, parent.y};
        break;
    case UiAnchor::TopRight:
        pos = {parent.x + parent.w - size.x, parent.y};
        break;
    case UiAnchor::CenterLeft:
        pos = {parent.x, parent.y + (parent.h - size.y) * 0.5f};
        break;
    case UiAnchor::Center:
        pos = {parent.x + (parent.w - size.x) * 0.5f, parent.y + (parent.h - size.y) * 0.5f};
        break;
    case UiAnchor::CenterRight:
        pos = {parent.x + parent.w - size.x, parent.y + (parent.h - size.y) * 0.5f};
        break;
    case UiAnchor::BottomLeft:
        pos = {parent.x, parent.y + parent.h - size.y};
        break;
    case UiAnchor::BottomCenter:
        pos = {parent.x + (parent.w - size.x) * 0.5f, parent.y + parent.h - size.y};
        break;
    case UiAnchor::BottomRight:
        pos = {parent.x + parent.w - size.x, parent.y + parent.h - size.y};
        break;
    }
    return {pos.x + offset.x, pos.y + offset.y};
}

Rectf anchor_rect(Rectf parent, UiAnchor anchor, Vec2f size, Vec2f offset) {
    const Vec2f pos = anchor_pos(parent, anchor, size, offset);
    return {pos.x, pos.y, size.x, size.y};
}

UiRectPair split_rect(Rectf rect, UiLayoutAxis axis, f32 first_size, f32 spacing) {
    const f32 safe_spacing = std::max(0.0f, spacing);
    UiRectPair pair;
    if (axis == UiLayoutAxis::Horizontal) {
        const f32 available = std::max(0.0f, rect.w - safe_spacing);
        const f32 first = std::clamp(first_size, 0.0f, available);
        pair.first = {rect.x, rect.y, first, rect.h};
        pair.second = {rect.x + first + safe_spacing, rect.y, std::max(0.0f, available - first), rect.h};
    } else {
        const f32 available = std::max(0.0f, rect.h - safe_spacing);
        const f32 first = std::clamp(first_size, 0.0f, available);
        pair.first = {rect.x, rect.y, rect.w, first};
        pair.second = {rect.x, rect.y + first + safe_spacing, rect.w, std::max(0.0f, available - first)};
    }
    return pair;
}

UiRectPair split_rect_fraction(Rectf rect, UiLayoutAxis axis, f32 fraction, f32 spacing) {
    const f32 size = axis == UiLayoutAxis::Horizontal ? rect.w : rect.h;
    return split_rect(rect, axis, std::max(0.0f, size - std::max(0.0f, spacing)) * std::clamp(fraction, 0.0f, 1.0f), spacing);
}

std::vector<Rectf> layout_rows(Rectf bounds, i32 count, f32 spacing) {
    std::vector<Rectf> rows;
    if (count <= 0) {
        return rows;
    }
    rows.reserve(static_cast<std::size_t>(count));
    const f32 safe_spacing = std::max(0.0f, spacing);
    const f32 height = std::max(0.0f, (bounds.h - safe_spacing * static_cast<f32>(count - 1)) / static_cast<f32>(count));
    for (i32 i = 0; i < count; ++i) {
        rows.push_back({bounds.x, bounds.y + static_cast<f32>(i) * (height + safe_spacing), bounds.w, height});
    }
    return rows;
}

std::vector<Rectf> layout_columns(Rectf bounds, i32 count, f32 spacing) {
    std::vector<Rectf> columns;
    if (count <= 0) {
        return columns;
    }
    columns.reserve(static_cast<std::size_t>(count));
    const f32 safe_spacing = std::max(0.0f, spacing);
    const f32 width = std::max(0.0f, (bounds.w - safe_spacing * static_cast<f32>(count - 1)) / static_cast<f32>(count));
    for (i32 i = 0; i < count; ++i) {
        columns.push_back({bounds.x + static_cast<f32>(i) * (width + safe_spacing), bounds.y, width, bounds.h});
    }
    return columns;
}

Rectf aspect_rect(Rectf bounds, Vec2f content_size, UiFitMode mode, UiAlign horizontal, UiAlign vertical) {
    if (content_size.x <= 0.0f || content_size.y <= 0.0f || bounds.w <= 0.0f || bounds.h <= 0.0f) {
        return {bounds.x, bounds.y, 0.0f, 0.0f};
    }
    const f32 scale_x = bounds.w / content_size.x;
    const f32 scale_y = bounds.h / content_size.y;
    const f32 scale = mode == UiFitMode::Cover ? std::max(scale_x, scale_y) : std::min(scale_x, scale_y);
    return align_rect(bounds, {content_size.x * scale, content_size.y * scale}, horizontal, vertical);
}

Rectf clamp_rect_to_bounds(Rectf rect, Rectf bounds) {
    rect.w = std::min(rect.w, bounds.w);
    rect.h = std::min(rect.h, bounds.h);
    rect.x = std::clamp(rect.x, bounds.x, bounds.x + std::max(0.0f, bounds.w - rect.w));
    rect.y = std::clamp(rect.y, bounds.y, bounds.y + std::max(0.0f, bounds.h - rect.h));
    return rect;
}

Rectf intersect_rect(Rectf a, Rectf b) {
    const f32 x = std::max(a.x, b.x);
    const f32 y = std::max(a.y, b.y);
    const f32 right = std::min(a.x + a.w, b.x + b.w);
    const f32 bottom = std::min(a.y + a.h, b.y + b.h);
    return {x, y, std::max(0.0f, right - x), std::max(0.0f, bottom - y)};
}

Rectf union_rect(Rectf a, Rectf b) {
    const f32 x = std::min(a.x, b.x);
    const f32 y = std::min(a.y, b.y);
    const f32 right = std::max(a.x + a.w, b.x + b.w);
    const f32 bottom = std::max(a.y + a.h, b.y + b.h);
    return {x, y, std::max(0.0f, right - x), std::max(0.0f, bottom - y)};
}

bool overlaps(Rectf a, Rectf b) {
    return intersect_rect(a, b).w > 0.0f && intersect_rect(a, b).h > 0.0f;
}

f32 stack_height(i32 item_count, f32 item_height, f32 spacing) {
    if (item_count <= 0) {
        return 0.0f;
    }
    return static_cast<f32>(item_count) * item_height + static_cast<f32>(item_count - 1) * spacing;
}

Rectf stack_item(const UiStack& stack, i32 index) {
    const f32 y = stack.bounds.y + static_cast<f32>(index) * (stack.item_size.y + stack.spacing);
    const f32 w = aligned_size(stack.bounds.w, stack.item_size.x, stack.cross);
    return {
        aligned_pos(stack.bounds.x, stack.bounds.w, w, stack.cross),
        y,
        w,
        stack.item_size.y,
    };
}

Vec2f rect_center(Rectf rect) {
    return {rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f};
}

Vec2f rect_size(Rectf rect) {
    return {rect.w, rect.h};
}

bool contains(Rectf rect, Vec2f pos) {
    return pos.x >= rect.x && pos.y >= rect.y && pos.x < rect.x + rect.w && pos.y < rect.y + rect.h;
}

} // namespace kin
