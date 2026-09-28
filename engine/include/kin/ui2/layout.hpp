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
};

constexpr SizeAxis fixed(f32 px) { return {SizeMode::Fixed, px, 0.0f, 0.0f}; }
constexpr SizeAxis fit(f32 min = 0.0f, f32 max = 0.0f) { return {SizeMode::Fit, 0.0f, min, max}; }
constexpr SizeAxis grow(f32 weight = 1.0f) { return {SizeMode::Grow, weight, 0.0f, 0.0f}; }
constexpr SizeAxis grow(f32 weight, f32 min, f32 max = 0.0f) { return {SizeMode::Grow, weight, min, max}; }
constexpr SizeAxis percent(f32 fraction, f32 min = 0.0f, f32 max = 0.0f) { return {SizeMode::Percent, fraction, min, max}; }

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
};

// Two-pass solver. Measures Fit sizes bottom-up, then arranges (Fixed/Fit/Grow +
// align + padding + spacing) top-down. nodes[root].solved is seeded with `available`.
// The overload with scratch reuses allocations across calls (preferred in hot paths).
void solve(std::vector<LayoutNode>& nodes, i32 root, Rectf available, LayoutScratch& scratch);
void solve(std::vector<LayoutNode>& nodes, i32 root, Rectf available); // creates a temporary scratch

} // namespace kin::ui2
