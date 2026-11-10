#pragma once

#include <kin/core/types.hpp>
#include <kin/renderer/color.hpp>
#include <kin/ui2/geometry.hpp>
#include <kin/ui2/surface.hpp>

#include <vector>

namespace kin::ui2 {

// How a node is sized on one axis.
//   Fixed - exactly `value` pixels.
//   Fit   - shrink to content (this is "auto-size"); clamped to [min, max].
//   Grow  - fill leftover space, sharing it with sibling Grows by `value` weight.
//   Percent - fraction of parent content size on this axis.
enum class SizeMode { Fixed, Fit, Grow, Percent };

struct SizeAxis {
    SizeMode mode = SizeMode::Fit;
    f32 value = 0.0f; // Fixed: pixels; Grow: weight
    f32 min = 0.0f;
    f32 max = 0.0f; // 0 == unbounded

    friend constexpr bool operator==(const SizeAxis&, const SizeAxis&) = default;
};

constexpr SizeAxis fixed(f32 px) { return {SizeMode::Fixed, px, 0.0f, 0.0f}; }
constexpr SizeAxis fit(f32 min = 0.0f, f32 max = 0.0f) { return {SizeMode::Fit, 0.0f, min, max}; }
constexpr SizeAxis grow(f32 weight = 1.0f) { return {SizeMode::Grow, weight, 0.0f, 0.0f}; }
constexpr SizeAxis grow(f32 weight, f32 min, f32 max = 0.0f) { return {SizeMode::Grow, weight, min, max}; }
constexpr SizeAxis percent(f32 fraction, f32 min = 0.0f, f32 max = 0.0f) { return {SizeMode::Percent, fraction, min, max}; }

// Where a floating node hangs (LayoutStyle::floating). It goes to the other side
// when it would run out of the layout's root there and fits better on that side,
// then moves along so it stays inside the root.
enum class UiFloat : u8 {
    Below, // top left at the parent's bottom left: a dropdown (else above)
    Above, // bottom left at the parent's top left (else below)
    Right, // top left at the parent's top right: a submenu (else to its left)
    Left,  // top right at the parent's top left (else to its right)
    Point, // top left at `float_offset`, in root coordinates: a context menu at the pointer (else up or left of it)
};

struct LayoutStyle {
    SizeAxis width{};
    SizeAxis height{};
    UiPadding padding{};
    UiPadding margin{};
    f32 spacing = 0.0f;
    f32 line_spacing = 0.0f;
    UiLayoutAxis axis = UiLayoutAxis::Vertical; // main axis children flow along
    UiAlign main = UiAlign::Start;              // justify along main axis
    UiAlign cross = UiAlign::Start;             // align on cross axis
    UiAlign line_cross = UiAlign::Start;        // pack wrapped lines / grid rows on the cross axis
    bool wrap = false;                          // flow children into multiple lines along cross axis
    i32 grid_columns = 0;                       // >0: document-order rows of fixed column count
    bool overlay = false;                       // out-of-flow child anchored to parent content
    UiAnchor anchor = UiAnchor::TopLeft;
    Vec2f anchor_offset{};
    SurfaceStyle surface{};
    bool draw_surface = false;
    i32 layer = 0; // draw order: lower draws first (behind). Ties broken by document order.
    // Out of flow and not held inside the parent: placed next to it (float_side,
    // moved by float_offset), sized against the root, kept inside the root. Drawn
    // above the rest and outside its ancestors' clips: menus, popovers, tooltips.
    bool floats = false;
    // When its line is too long for its parent, it gives up room along the line (down
    // to its min), with the others that do, in proportion to their size: CSS's
    // flex-shrink. A label whose text is cut with "…" does; so may a tab.
    bool shrink = false;
    UiFloat float_side = UiFloat::Below;
    Vec2f float_offset{};
    // Children are laid out in a content box as large as they need on these axes
    // (at least the node's own), so they overflow it instead of being squeezed:
    // scrolled content.
    bool scroll_x = false;
    bool scroll_y = false;

    // Shorthands that chain, to write a style in one expression:
    //   LayoutStyle{}.row().w(grow()).h(32).pad(12, 0).gap(8).items(UiAlign::Center)
    LayoutStyle& row() { axis = UiLayoutAxis::Horizontal; return *this; }
    LayoutStyle& col() { axis = UiLayoutAxis::Vertical; return *this; }
    LayoutStyle& w(SizeAxis value) { width = value; return *this; }
    LayoutStyle& w(f32 px) { width = fixed(px); return *this; }
    LayoutStyle& h(SizeAxis value) { height = value; return *this; }
    LayoutStyle& h(f32 px) { height = fixed(px); return *this; }
    LayoutStyle& size(f32 w_px, f32 h_px) { width = fixed(w_px); height = fixed(h_px); return *this; }
    LayoutStyle& fill() { width = grow(); height = grow(); return *this; } // all the room the parent has
    LayoutStyle& pad(f32 all) { padding = {all, all, all, all}; return *this; }
    LayoutStyle& pad(f32 x, f32 y) { padding = {x, y, x, y}; return *this; }
    LayoutStyle& pad(f32 left, f32 top, f32 right, f32 bottom) { padding = {left, top, right, bottom}; return *this; }
    LayoutStyle& margins(f32 all) { margin = {all, all, all, all}; return *this; }
    LayoutStyle& margins(f32 x, f32 y) { margin = {x, y, x, y}; return *this; }
    LayoutStyle& margins(f32 left, f32 top, f32 right, f32 bottom) { margin = {left, top, right, bottom}; return *this; }
    LayoutStyle& gap(f32 value) { spacing = value; return *this; }
    LayoutStyle& justify(UiAlign align) { main = align; return *this; } // along the axis
    LayoutStyle& items(UiAlign align) { cross = align; return *this; }  // across it
    LayoutStyle& center() { main = UiAlign::Center; cross = UiAlign::Center; return *this; }
    LayoutStyle& wrapped() { wrap = true; return *this; }
    LayoutStyle& grid(i32 columns) { grid_columns = columns; return *this; }
    LayoutStyle& at(UiAnchor where, Vec2f offset = {}) { overlay = true; anchor = where; anchor_offset = offset; return *this; }
    LayoutStyle& on_layer(i32 value) { layer = value; return *this; }
    LayoutStyle& shrinks() { shrink = true; return *this; }
    LayoutStyle& floating(UiFloat side = UiFloat::Below, Vec2f offset = {}) {
        floats = true; float_side = side; float_offset = offset; return *this;
    }
};

// A single box in the layout tree. Leaves set `intrinsic` (content size) before solve;
// containers reference children by arena index. The arena is pre-order: a node's index
// is always less than its descendants', so a reverse sweep measures bottom-up.
struct LayoutNode {
    LayoutStyle style{};
    Vec2f intrinsic{}; // leaf: content size (input); container: measured size (computed)
    Rectf solved{};    // final placement (output)
    std::vector<i32> children;
    i32 parent = -1;
    Vec2f content{}; // measured size before the node's own Fixed size and min/max (computed)
};

// Reusable scratch buffers for solve(). Hold one per Context / ECS scratch to avoid
// per-container heap allocation during the arrange pass.
struct LayoutScratch {
    std::vector<i32> flow_children;
    std::vector<f32> main_size;
    std::vector<f32> cross_size;
    std::vector<i32> line_start;
    std::vector<f32> line_cross_extent;
    std::vector<f32> col_size;
    std::vector<f32> row_size;
    std::vector<f32> wrapped_cross_required;
    std::vector<i32> subtree_end;
    Rectf root{}; // what floating nodes are kept inside
};

// Two-pass solver. Measures Fit sizes bottom-up, then arranges (Fixed/Fit/Grow +
// align + padding + spacing) top-down. nodes[root].solved is seeded with `available`.
// The overload with scratch reuses allocations across calls (preferred in hot paths).
void solve(std::vector<LayoutNode>& nodes, i32 root, Rectf available, LayoutScratch& scratch);
void solve(std::vector<LayoutNode>& nodes, i32 root, Rectf available); // creates a temporary scratch

// Places the floating nodes again next to their parents (solve() does it once),
// moving each with its subtree: after something moved solved rects, as a scroll does.
void place_floating(std::vector<LayoutNode>& nodes, Rectf root, LayoutScratch& scratch);

} // namespace kin::ui2
