#pragma once

#include <kin/core/types.hpp>

#include <optional>
#include <vector>

namespace kin::ui2 {

enum class UiAlign {
    Start,
    Center,
    End,
    Stretch,
};

enum class UiAnchor {
    TopLeft,
    TopCenter,
    TopRight,
    CenterLeft,
    Center,
    CenterRight,
    BottomLeft,
    BottomCenter,
    BottomRight,
};

enum class UiLayoutAxis {
    Horizontal,
    Vertical,
};

enum class UiFitMode {
    Contain,
    Cover,
};

struct UiPadding {
    f32 left = 0.0f;
    f32 top = 0.0f;
    f32 right = 0.0f;
    f32 bottom = 0.0f;
};

struct UiRectPair {
    Rectf first{};
    Rectf second{};
};

struct UiFlowLayout {
    Rectf bounds{};
    Vec2f item_size{};
    Vec2f spacing{};
    UiLayoutAxis axis = UiLayoutAxis::Horizontal;
    UiAlign cross = UiAlign::Start;

    std::vector<Rectf> items(i32 item_count) const;
    std::optional<i32> hit_test(Vec2f pos, i32 item_count) const;
};

struct UiCursor {
    Vec2f pos{};
    f32 spacing = 0.0f;

    Vec2f current() const { return pos; }
    Rectf rect(Vec2f size) const;
    Rectf take_horizontal(Vec2f size);
    Rectf take_vertical(Vec2f size);
    void skip_horizontal(f32 amount);
    void skip_vertical(f32 amount);
};

struct UiContainer {
    Rectf bounds{};
    UiPadding padding{};

    Rectf content() const;
    UiCursor row(f32 spacing = 0.0f) const;
    UiCursor column(f32 spacing = 0.0f) const;
};

struct UiStack {
    Rectf bounds;
    Vec2f item_size;
    f32 spacing = 0.0f;
    UiAlign cross = UiAlign::Start;
};

struct UiGrid {
    Rectf bounds{};
    Vec2f cell_size{};
    Vec2f spacing{};
    i32 columns = 1;

    Rectf cell(i32 index) const;
    std::optional<i32> hit_test(Vec2f pos, i32 item_count) const;
};

struct UiHudLayout {
    Rectf bounds{};
    f32 margin = 0.0f;

    static UiHudLayout from_size(Vec2f size, f32 margin = 8.0f);
    Vec2f pos(UiAnchor anchor, Vec2f size, Vec2f offset = {}) const;
    Rectf rect(UiAnchor anchor, Vec2f size, Vec2f offset = {}) const;
};

constexpr UiPadding padding(f32 all) {
    return {all, all, all, all};
}

constexpr UiPadding padding(f32 horizontal, f32 vertical) {
    return {horizontal, vertical, horizontal, vertical};
}

constexpr UiPadding padding(f32 left, f32 top, f32 right, f32 bottom) {
    return {left, top, right, bottom};
}

Rectf inset(Rectf rect, f32 amount);
Rectf inset(Rectf rect, UiPadding amount);
Rectf outset(Rectf rect, f32 amount);
Rectf outset(Rectf rect, UiPadding amount);
Rectf safe_area(Vec2f size, f32 margin = 8.0f);
Rectf centered_rect(Vec2f center, Vec2f size);
Rectf align_rect(Rectf parent, Vec2f size, UiAlign horizontal, UiAlign vertical);
Vec2f anchor_pos(Rectf parent, UiAnchor anchor, Vec2f size, Vec2f offset = {});
Rectf anchor_rect(Rectf parent, UiAnchor anchor, Vec2f size, Vec2f offset = {});
UiRectPair split_rect(Rectf rect, UiLayoutAxis axis, f32 first_size, f32 spacing = 0.0f);
UiRectPair split_rect_fraction(Rectf rect, UiLayoutAxis axis, f32 fraction, f32 spacing = 0.0f);
std::vector<Rectf> layout_rows(Rectf bounds, i32 count, f32 spacing = 0.0f);
std::vector<Rectf> layout_columns(Rectf bounds, i32 count, f32 spacing = 0.0f);
Rectf aspect_rect(Rectf bounds, Vec2f content_size, UiFitMode mode = UiFitMode::Contain, UiAlign horizontal = UiAlign::Center, UiAlign vertical = UiAlign::Center);
Rectf clamp_rect_to_bounds(Rectf rect, Rectf bounds);
Rectf intersect_rect(Rectf a, Rectf b);
Rectf union_rect(Rectf a, Rectf b);
bool overlaps(Rectf a, Rectf b);
f32 stack_height(i32 item_count, f32 item_height, f32 spacing);
Rectf stack_item(const UiStack& stack, i32 index);
Vec2f rect_center(Rectf rect);
Vec2f rect_size(Rectf rect);
bool contains(Rectf rect, Vec2f pos);

} // namespace kin::ui2
