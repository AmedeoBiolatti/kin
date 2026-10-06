#include <kin/platform/app.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/backend.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/text.hpp>
#include <kin/ecs/ui2.hpp>
#include <kin/ecs/ui2_sync.hpp>
#include <kin/l10n/localization.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/layout.hpp>
#include <kin/ui2/radix_colors.hpp>
#include <kin/ui2/state.hpp>
#include <kin/ui2/theme.hpp>
#include <kin/ui2/widgets.hpp>

#include <cassert>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace kin {
// Friend seam: the public Input::begin_frame() always advances transients, so tests
// emulate a frame that ran zero fixed-update steps (advance_transients == false) here.
struct InputFrameTestHook {
    static void begin_frame(Input& input, bool advance_transients) {
        input.begin_frame(advance_transients);
    }
};
} // namespace kin

namespace {

using namespace kin;

// Identity backend: window space == logical space, so test pointer coordinates are
// also logical coordinates. It records primitive destinations for small geometry probes.
class IdentityBackend final : public IRenderer2DBackend {
public:
    explicit IdentityBackend(std::vector<Rectf>* fills = nullptr)
        : fills(fills) {}
    IdentityBackend(std::vector<Rectf>* fills, std::vector<Color>* fill_colors)
        : fills(fills),
          fill_colors(fill_colors) {}
    IdentityBackend(std::vector<Rectf>* fills, std::vector<Color>* fill_colors, i32* rounded_fills, i32* rounded_outlines)
        : fills(fills),
          fill_colors(fill_colors),
          rounded_fills(rounded_fills),
          rounded_outlines(rounded_outlines) {}
    IdentityBackend(std::vector<Rectf>* fills,
                    std::vector<Color>* fill_colors,
                    i32* rounded_fills,
                    i32* rounded_outlines,
                    std::vector<Rectf>* rounded_outline_rects,
                    std::vector<Rectf>* clips)
        : fills(fills),
          fill_colors(fill_colors),
          rounded_fills(rounded_fills),
          rounded_outlines(rounded_outlines),
          rounded_outline_rects(rounded_outline_rects),
          clips(clips) {}

    std::string_view name() const override { return "identity"; }
    void clear(Color) override {}
    void present() override {}
    void set_logical_size(Vec2i) override {}
    void set_integer_logical_size(Vec2i) override {}
    Vec2i output_size() const override { return {640, 360}; }
    Vec2f window_to_logical(Vec2f v) const override { return v; }
    Vec2f logical_to_window(Vec2f v) const override { return v; }
    Texture create_texture_from_rgba(const u8*, Vec2i) override { return {}; }
    void draw_texture(const Texture&, Rectf dest) override {
        if (fills) {
            fills->push_back(dest);
        }
    }
    void draw_texture(const Texture&, Rectf, Rectf dest) override {
        if (fills) {
            fills->push_back(dest);
        }
    }
    void fill_rect(Rectf rect, Color color) override {
        if (fills) {
            fills->push_back(rect);
        }
        if (fill_colors) {
            fill_colors->push_back(color);
        }
    }
    void draw_rect(Rectf, Color) override {}
    void fill_rounded_rect(Rectf rect, f32, Color color) override {
        if (rounded_fills) {
            ++*rounded_fills;
        }
        fill_rect(rect, color);
    }
    void draw_rounded_rect(Rectf rect, f32, Color, f32) override {
        if (rounded_outlines) {
            ++*rounded_outlines;
        }
        if (rounded_outline_rects) {
            rounded_outline_rects->push_back(rect);
        }
    }
    void draw_line(Vec2f a, Vec2f b, Color) override {
        if (fills) {
            fills->push_back({std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(a.x - b.x), std::fabs(a.y - b.y)});
        }
    }
    void set_viewport(Rectf) override {}
    void reset_viewport() override {}
    void push_clip(Rectf rect) override {
        if (clips) {
            clips->push_back(rect);
        }
    }
    void push_viewport(Rectf) override {}
    void pop_viewport() override {}

private:
    std::vector<Rectf>* fills = nullptr;
    std::vector<Color>* fill_colors = nullptr;
    i32* rounded_fills = nullptr;
    i32* rounded_outlines = nullptr;
    std::vector<Rectf>* rounded_outline_rects = nullptr;
    std::vector<Rectf>* clips = nullptr;
};

class FakeTextureBackend final : public ITextureBackend {
public:
    explicit FakeTextureBackend(Vec2i size)
        : _size(size) {}

    Vec2i size() const override { return _size; }

private:
    Vec2i _size{};
};

bool approx(f32 a, f32 b) { return std::fabs(a - b) < 0.01f; }
bool rect_eq(Rectf r, f32 x, f32 y, f32 w, f32 h) {
    return approx(r.x, x) && approx(r.y, y) && approx(r.w, w) && approx(r.h, h);
}

Renderer2D make_renderer() {
    return Renderer2D{std::make_unique<IdentityBackend>()};
}

Renderer2D make_recording_renderer(std::vector<Rectf>& fills) {
    return Renderer2D{std::make_unique<IdentityBackend>(&fills)};
}

Renderer2D make_color_recording_renderer(std::vector<Rectf>& fills, std::vector<Color>& colors) {
    return Renderer2D{std::make_unique<IdentityBackend>(&fills, &colors)};
}

Renderer2D make_surface_recording_renderer(std::vector<Rectf>& fills, std::vector<Color>& colors, i32& rounded_fills, i32& rounded_outlines) {
    return Renderer2D{std::make_unique<IdentityBackend>(&fills, &colors, &rounded_fills, &rounded_outlines)};
}

Renderer2D make_surface_geometry_recording_renderer(std::vector<Rectf>& fills,
                                                     std::vector<Color>& colors,
                                                     i32& rounded_fills,
                                                     i32& rounded_outlines,
                                                     std::vector<Rectf>& rounded_outline_rects,
                                                     std::vector<Rectf>& clips) {
    return Renderer2D{std::make_unique<IdentityBackend>(&fills, &colors, &rounded_fills, &rounded_outlines, &rounded_outline_rects, &clips)};
}

Texture make_texture(Vec2i size = {32, 32}) {
    return Texture{std::make_shared<FakeTextureBackend>(size)};
}

Sprite make_sprite(Vec2i size = {16, 16}) {
    return {
        .texture = make_texture(size),
        .source = {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)},
    };
}

// --- solve(): Fixed / Fit / Grow, padding, spacing, auto-size ------------------------

void test_solve_grow_and_spacing() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(200);
    root.height = ui2::fixed(100);
    root.axis = ui2::UiLayoutAxis::Vertical;
    root.spacing = 10.0f;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle child;
    child.width = ui2::grow();
    child.height = ui2::fixed(30);
    n.push_back({child, {}, {}, {}, 0});
    n[0].children.push_back(1);
    n.push_back({child, {}, {}, {}, 0});
    n[0].children.push_back(2);

    ui2::solve(n, 0, {0, 0, 200, 100});
    assert(rect_eq(n[1].solved, 0, 0, 200, 30));
    assert(rect_eq(n[2].solved, 0, 40, 200, 30));
}

// Right to left: rows run from the right, nested boxes mirror inside their
// parents, Start and End alignment swap; ECS layouts solve again on a switch.
void test_right_to_left_layout() {
    const auto build = [] {
        std::vector<ui2::LayoutNode> n;
        ui2::LayoutStyle root;
        root.width = ui2::fixed(200);
        root.height = ui2::fixed(100);
        root.axis = ui2::UiLayoutAxis::Horizontal;
        root.padding = {.left = 10.0f};
        root.spacing = 5.0f;
        n.push_back({root, {}, {}, {}, -1});
        ui2::LayoutStyle box;
        box.width = ui2::fixed(50);
        box.height = ui2::fixed(20);
        box.axis = ui2::UiLayoutAxis::Horizontal;
        n.push_back({box, {}, {}, {}, 0});
        n.push_back({box, {}, {}, {}, 0});
        n[0].children = {1, 2};
        ui2::LayoutStyle inner;
        inner.width = ui2::fixed(10);
        inner.height = ui2::fixed(10);
        n.push_back({inner, {}, {}, {}, 2});
        n[2].children = {3};
        return n;
    };
    std::vector<ui2::LayoutNode> ltr = build();
    ui2::solve(ltr, 0, {20, 0, 200, 100});
    assert(rect_eq(ltr[1].solved, 30, 0, 50, 20));
    assert(rect_eq(ltr[2].solved, 85, 0, 50, 20));
    assert(rect_eq(ltr[3].solved, 85, 0, 10, 10));

    ui2::set_ui_direction(TextDirection::RightToLeft);
    assert(ui2::text_base_direction() == TextDirection::RightToLeft);
    std::vector<ui2::LayoutNode> rtl = build();
    ui2::solve(rtl, 0, {20, 0, 200, 100});
    assert(rect_eq(rtl[1].solved, 160, 0, 50, 20)); // the first child at the right, padding on the right
    assert(rect_eq(rtl[2].solved, 105, 0, 50, 20));
    assert(rect_eq(rtl[3].solved, 145, 0, 10, 10)); // at its parent's right
    assert(rect_eq(ui2::align_rect({0, 0, 100, 10}, {20, 10}, ui2::UiAlign::Start, ui2::UiAlign::Start), 80, 0, 20, 10));
    assert(rect_eq(ui2::align_rect({0, 0, 100, 10}, {20, 10}, ui2::UiAlign::Center, ui2::UiAlign::Start), 40, 0, 20, 10));

    // A retained tree solves again when the direction changes.
    kin::EcsWorld world;
    register_ui2_components(world);
    auto root = ui2_entity(world, "root").root({0, 0, 200, 100});
    auto row = ui2_entity(world, "row").layout(ui2::row(ui2::grow(), ui2::fit())).child_of(root);
    kin::EcsEntity first = ui2_entity(world, "first")
                               .label(ui2::Label{.text = "A"})
                               .layout(ui2::fixed_box(40, 20))
                               .child_of(row)
                               .entity();
    ui2::Context ui;
    Input input;
    std::vector<Rectf> fills;
    Renderer2D renderer = make_recording_renderer(fills);
    input.begin_frame();
    ui.begin(input, renderer);
    kin::update_ui2_world(world, ui);
    ui.end();
    assert(approx(first.get<Ui2Layout>()->solved.x, 160.0f));
    ui2::set_ui_direction(TextDirection::LeftToRight);
    assert(!ui2::text_base_direction());
    input.begin_frame();
    ui.begin(input, renderer);
    kin::update_ui2_world(world, ui);
    ui.end();
    assert(approx(first.get<Ui2Layout>()->solved.x, 0.0f));
}

void test_solve_fit_autosize() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(300);
    root.height = ui2::fixed(300);
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle col;
    col.width = ui2::fit();
    col.height = ui2::fit();
    col.padding = {8, 8, 8, 8};
    n.push_back({col, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::LayoutStyle leaf;
    leaf.width = ui2::fixed(50);
    leaf.height = ui2::fixed(20);
    n.push_back({leaf, {}, {}, {}, 1});
    n[1].children.push_back(2);

    ui2::solve(n, 0, {0, 0, 300, 300});
    assert(approx(n[1].intrinsic.x, 66) && approx(n[1].intrinsic.y, 36)); // child + padding
    assert(rect_eq(n[1].solved, 0, 0, 66, 36));
    assert(rect_eq(n[2].solved, 8, 8, 50, 20));
}

void test_solve_grow_weights() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(300);
    root.height = ui2::fixed(10);
    root.axis = ui2::UiLayoutAxis::Horizontal;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle a;
    a.width = ui2::grow(1);
    a.height = ui2::fixed(10);
    ui2::LayoutStyle b;
    b.width = ui2::grow(2);
    b.height = ui2::fixed(10);
    n.push_back({a, {}, {}, {}, 0});
    n[0].children.push_back(1);
    n.push_back({b, {}, {}, {}, 0});
    n[0].children.push_back(2);

    ui2::solve(n, 0, {0, 0, 300, 10});
    assert(rect_eq(n[1].solved, 0, 0, 100, 10));
    assert(rect_eq(n[2].solved, 100, 0, 200, 10));
}

void test_solve_margins_reserve_flow_space() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(200);
    root.height = ui2::fixed(100);
    root.axis = ui2::UiLayoutAxis::Vertical;
    root.cross = ui2::UiAlign::Center;
    root.spacing = 5.0f;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle a;
    a.width = ui2::fixed(40);
    a.height = ui2::fixed(10);
    a.margin = {10, 2, 20, 3};
    n.push_back({a, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::LayoutStyle b;
    b.width = ui2::fixed(20);
    b.height = ui2::fixed(10);
    b.margin = {0, 4, 0, 0};
    n.push_back({b, {}, {}, {}, 0});
    n[0].children.push_back(2);

    ui2::solve(n, 0, {0, 0, 200, 100});
    assert(rect_eq(n[1].solved, 75, 2, 40, 10));
    assert(rect_eq(n[2].solved, 90, 24, 20, 10));
}

void test_solve_percent_sizes_against_parent_content() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(200);
    root.height = ui2::fixed(100);
    root.padding = {10, 5, 30, 15};
    root.cross = ui2::UiAlign::Start;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle child;
    child.width = ui2::percent(0.5f);
    child.height = ui2::percent(0.25f);
    n.push_back({child, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::solve(n, 0, {0, 0, 200, 100});
    assert(rect_eq(n[1].solved, 10, 5, 80, 20));
}

void test_solve_grow_min_max_constraints() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(300);
    root.height = ui2::fixed(20);
    root.axis = ui2::UiLayoutAxis::Horizontal;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle a;
    a.width = ui2::grow(1.0f, 0.0f, 50.0f);
    a.height = ui2::grow();
    n.push_back({a, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::LayoutStyle b;
    b.width = ui2::grow(1.0f, 80.0f);
    b.height = ui2::grow();
    n.push_back({b, {}, {}, {}, 0});
    n[0].children.push_back(2);

    ui2::solve(n, 0, {0, 0, 300, 20});
    assert(rect_eq(n[1].solved, 0, 0, 50, 20));
    assert(rect_eq(n[2].solved, 50, 0, 150, 20));
}

void test_solve_overlay_excluded_from_fit() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fit();
    root.height = ui2::fit();
    root.spacing = 10.0f;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle flow;
    flow.width = ui2::fixed(40);
    flow.height = ui2::fixed(20);
    n.push_back({flow, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::LayoutStyle overlay;
    overlay.width = ui2::fixed(200);
    overlay.height = ui2::fixed(100);
    overlay.overlay = true;
    n.push_back({overlay, {}, {}, {}, 0});
    n[0].children.push_back(2);

    ui2::solve(n, 0, {0, 0, 500, 500});
    assert(approx(n[0].intrinsic.x, 40));
    assert(approx(n[0].intrinsic.y, 20));
}

void test_solve_overlay_grow_fills_content() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(100);
    root.height = ui2::fixed(80);
    root.padding = {10, 5, 20, 15};
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle overlay;
    overlay.width = ui2::grow();
    overlay.height = ui2::grow();
    overlay.overlay = true;
    n.push_back({overlay, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::solve(n, 0, {0, 0, 100, 80});
    assert(rect_eq(n[1].solved, 10, 5, 70, 60));
}

void test_solve_overlay_anchors() {
    const auto solve_anchor = [](ui2::UiAnchor anchor) {
        std::vector<ui2::LayoutNode> n;
        ui2::LayoutStyle root;
        root.width = ui2::fixed(100);
        root.height = ui2::fixed(80);
        n.push_back({root, {}, {}, {}, -1});

        ui2::LayoutStyle overlay;
        overlay.width = ui2::fixed(20);
        overlay.height = ui2::fixed(10);
        overlay.overlay = true;
        overlay.anchor = anchor;
        n.push_back({overlay, {}, {}, {}, 0});
        n[0].children.push_back(1);

        ui2::solve(n, 0, {0, 0, 100, 80});
        return n[1].solved;
    };

    assert(rect_eq(solve_anchor(ui2::UiAnchor::TopLeft), 0, 0, 20, 10));
    assert(rect_eq(solve_anchor(ui2::UiAnchor::Center), 40, 35, 20, 10));
    assert(rect_eq(solve_anchor(ui2::UiAnchor::BottomRight), 80, 70, 20, 10));
}

void test_solve_overlay_anchor_offset_and_margin() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(100);
    root.height = ui2::fixed(80);
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle overlay;
    overlay.width = ui2::fixed(20);
    overlay.height = ui2::fixed(10);
    overlay.margin = {4, 5, 6, 7};
    overlay.overlay = true;
    overlay.anchor = ui2::UiAnchor::BottomRight;
    overlay.anchor_offset = {-2, -3};
    n.push_back({overlay, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::solve(n, 0, {0, 0, 100, 80});
    assert(rect_eq(n[1].solved, 72, 60, 20, 10));
}

void test_solve_overlay_preserves_flow() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(100);
    root.height = ui2::fixed(100);
    root.spacing = 5.0f;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle flow;
    flow.width = ui2::fixed(30);
    flow.height = ui2::fixed(10);
    n.push_back({flow, {}, {}, {}, 0});
    n[0].children.push_back(1);
    n.push_back({flow, {}, {}, {}, 0});
    n[0].children.push_back(2);

    ui2::LayoutStyle overlay = flow;
    overlay.overlay = true;
    overlay.anchor = ui2::UiAnchor::BottomRight;
    n.push_back({overlay, {}, {}, {}, 0});
    n[0].children.push_back(3);

    ui2::solve(n, 0, {0, 0, 100, 100});
    assert(rect_eq(n[1].solved, 0, 0, 30, 10));
    assert(rect_eq(n[2].solved, 0, 15, 30, 10));
    assert(rect_eq(n[3].solved, 70, 90, 30, 10));
}

void test_solve_wrap_basic_gaps_and_oversized() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(100);
    root.height = ui2::fixed(100);
    root.axis = ui2::UiLayoutAxis::Horizontal;
    root.wrap = true;
    root.spacing = 5.0f;
    root.line_spacing = 3.0f;
    n.push_back({root, {}, {}, {}, -1});

    auto add = [&](f32 w, f32 h) {
        ui2::LayoutStyle child;
        child.width = ui2::fixed(w);
        child.height = ui2::fixed(h);
        const i32 index = static_cast<i32>(n.size());
        n.push_back({child, {}, {}, {}, 0});
        n[0].children.push_back(index);
        return index;
    };
    const i32 a = add(40, 10);
    const i32 b = add(40, 20);
    const i32 c = add(40, 15);

    ui2::solve(n, 0, {0, 0, 100, 100});
    assert(rect_eq(n[a].solved, 0, 0, 40, 10));
    assert(rect_eq(n[b].solved, 45, 0, 40, 20));
    assert(rect_eq(n[c].solved, 0, 23, 40, 15));

    n.clear();
    n.push_back({root, {}, {}, {}, -1});
    const i32 wide_a = add(80, 10);
    const i32 wide_b = add(150, 10);
    const i32 wide_c = add(20, 10);
    ui2::solve(n, 0, {0, 0, 100, 100});
    assert(rect_eq(n[wide_a].solved, 0, 0, 80, 10));
    assert(rect_eq(n[wide_b].solved, 0, 13, 150, 10));
    assert(rect_eq(n[wide_c].solved, 0, 26, 20, 10));
}

void test_solve_wrap_grow_stretch_line_cross_and_overlay() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(100);
    root.height = ui2::fixed(80);
    root.axis = ui2::UiLayoutAxis::Horizontal;
    root.wrap = true;
    root.spacing = 10.0f;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle fixed_child;
    fixed_child.width = ui2::fixed(60);
    fixed_child.height = ui2::fixed(10);
    ui2::LayoutStyle grow_child;
    grow_child.width = ui2::grow(1.0f, 20.0f);
    grow_child.height = ui2::fixed(10);
    for (int i = 0; i < 2; ++i) {
        const i32 fixed_index = static_cast<i32>(n.size());
        n.push_back({fixed_child, {}, {}, {}, 0});
        n[0].children.push_back(fixed_index);
        const i32 grow_index = static_cast<i32>(n.size());
        n.push_back({grow_child, {}, {}, {}, 0});
        n[0].children.push_back(grow_index);
    }

    ui2::LayoutStyle overlay = fixed_child;
    overlay.width = ui2::fixed(200);
    overlay.height = ui2::fixed(30);
    overlay.overlay = true;
    overlay.anchor = ui2::UiAnchor::BottomRight;
    n.push_back({overlay, {}, {}, {}, 0});
    n[0].children.push_back(5);

    ui2::solve(n, 0, {0, 0, 100, 80});
    assert(rect_eq(n[1].solved, 0, 0, 60, 10));
    assert(rect_eq(n[2].solved, 70, 0, 30, 10));
    assert(rect_eq(n[3].solved, 0, 10, 60, 10));
    assert(rect_eq(n[4].solved, 70, 10, 30, 10));
    assert(rect_eq(n[5].solved, 0, 50, 100, 30));

    n.clear();
    root.spacing = 5.0f;
    root.cross = ui2::UiAlign::Stretch;
    n.push_back({root, {}, {}, {}, -1});
    ui2::LayoutStyle a;
    a.width = ui2::fixed(40);
    a.height = ui2::fixed(10);
    ui2::LayoutStyle b = a;
    b.height = ui2::fixed(20);
    ui2::LayoutStyle c = a;
    c.height = ui2::fixed(15);
    n.push_back({a, {}, {}, {}, 0});
    n[0].children.push_back(1);
    n.push_back({b, {}, {}, {}, 0});
    n[0].children.push_back(2);
    n.push_back({c, {}, {}, {}, 0});
    n[0].children.push_back(3);
    ui2::solve(n, 0, {0, 0, 100, 80});
    assert(rect_eq(n[1].solved, 0, 0, 40, 20));
    assert(rect_eq(n[2].solved, 45, 0, 40, 20));
    assert(rect_eq(n[3].solved, 0, 20, 40, 15));

    root.cross = ui2::UiAlign::Start;
    root.line_cross = ui2::UiAlign::Center;
    root.line_spacing = 10.0f;
    n[0].style = root;
    ui2::solve(n, 0, {0, 0, 100, 100});
    assert(rect_eq(n[1].solved, 0, 27.5f, 40, 10));
    assert(rect_eq(n[2].solved, 45, 27.5f, 40, 20));
    assert(rect_eq(n[3].solved, 0, 57.5f, 40, 15));

    n[0].style.line_cross = ui2::UiAlign::End;
    ui2::solve(n, 0, {0, 0, 100, 100});
    assert(rect_eq(n[1].solved, 0, 55, 40, 10));
    assert(rect_eq(n[2].solved, 45, 55, 40, 20));
    assert(rect_eq(n[3].solved, 0, 85, 40, 15));
}

void test_solve_wrap_fit_resolve_updates_parent() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(100);
    root.height = ui2::fixed(200);
    root.axis = ui2::UiLayoutAxis::Vertical;
    n.push_back({root, {}, {}, {}, -1});

    ui2::LayoutStyle wrap;
    wrap.width = ui2::fixed(100);
    wrap.height = ui2::fit();
    wrap.axis = ui2::UiLayoutAxis::Horizontal;
    wrap.wrap = true;
    wrap.line_spacing = 5.0f;
    n.push_back({wrap, {}, {}, {}, 0});
    n[0].children.push_back(1);

    ui2::LayoutStyle child;
    child.width = ui2::fixed(60);
    child.height = ui2::fixed(10);
    n.push_back({child, {}, {}, {}, 1});
    n[1].children.push_back(2);
    n.push_back({child, {}, {}, {}, 1});
    n[1].children.push_back(3);

    ui2::LayoutStyle sibling;
    sibling.width = ui2::fixed(20);
    sibling.height = ui2::fixed(10);
    n.push_back({sibling, {}, {}, {}, 0});
    n[0].children.push_back(4);

    ui2::solve(n, 0, {0, 0, 100, 200});
    assert(approx(n[1].intrinsic.y, 25));
    assert(rect_eq(n[1].solved, 0, 0, 100, 25));
    assert(rect_eq(n[4].solved, 0, 25, 20, 10));
}

void test_solve_grid_columns_gaps_grow_and_alignment() {
    std::vector<ui2::LayoutNode> n;
    ui2::LayoutStyle root;
    root.width = ui2::fixed(200);
    root.height = ui2::fit();
    root.grid_columns = 2;
    root.spacing = 5.0f;
    root.line_spacing = 7.0f;
    root.main = ui2::UiAlign::Start;
    root.cross = ui2::UiAlign::Center;
    n.push_back({root, {}, {}, {}, -1});

    auto add = [&](ui2::LayoutStyle style) {
        const i32 index = static_cast<i32>(n.size());
        n.push_back({style, {}, {}, {}, 0});
        n[0].children.push_back(index);
        return index;
    };
    ui2::LayoutStyle label_a;
    label_a.width = ui2::fixed(50);
    label_a.height = ui2::fixed(20);
    ui2::LayoutStyle field_a;
    field_a.width = ui2::grow(1.0f, 40.0f);
    field_a.height = ui2::fixed(20);
    ui2::LayoutStyle label_b;
    label_b.width = ui2::fixed(80);
    label_b.height = ui2::fixed(30);
    ui2::LayoutStyle field_b;
    field_b.width = ui2::fixed(30);
    field_b.height = ui2::fixed(10);

    const i32 a = add(label_a);
    const i32 b = add(field_a);
    const i32 c = add(label_b);
    const i32 d = add(field_b);

    ui2::solve(n, 0, {0, 0, 200, 100});
    assert(approx(n[0].intrinsic.y, 57));
    assert(rect_eq(n[a].solved, 0, 0, 50, 20));
    assert(rect_eq(n[b].solved, 85, 0, 115, 20));
    assert(rect_eq(n[c].solved, 0, 27, 80, 30));
    assert(rect_eq(n[d].solved, 85, 37, 30, 10));
}

// --- ids -----------------------------------------------------------------------------

void test_ids() {
    assert(ui2::make_id("play") == ui2::make_id("play"));
    assert(ui2::make_id("play") != ui2::make_id("quit"));
    const ui2::Id menu = ui2::make_id("menu");
    assert(ui2::make_id(menu, u64{3}) != ui2::make_id(menu, u64{4}));
    assert(!ui2::Id{0});
    assert(ui2::make_id("x"));
}

// --- measure(): auto-size leaf -------------------------------------------------------

void test_measure_button() {
    ui2::Button b{.label = "Play"};
    const Vec2f text = ui2::measure_text(b.text_style.font, "Play", b.text_style.scale);
    const Vec2f m = ui2::measure(b);
    assert(approx(m.x, text.x + 24.0f)); // padding 12 + 12
    assert(approx(m.y, text.y + 12.0f)); // padding 6 + 6
}

void test_bitmap_font_draws_lowercase_fallback() {
    std::vector<Rectf> fills;
    Renderer2D renderer = make_recording_renderer(fills);

    ui2::draw_text(renderer, "map", {0, 0}, 1.0f, colors::white);

    assert(!fills.empty());
    assert(std::any_of(fills.begin(), fills.end(), [](Rectf rect) { return rect.x >= 6.0f; }));
    assert(std::any_of(fills.begin(), fills.end(), [](Rectf rect) { return rect.x >= 12.0f; }));

    fills.clear();
    ui2::draw_text(renderer, "+", {0, 0}, 1.0f, colors::white);
    assert(!fills.empty());
}

void test_label_alignment() {
    std::vector<Rectf> fills;
    Renderer2D renderer = make_recording_renderer(fills);
    Input input;
    ui2::Context ui;
    ui2::Label label{
        .bounds = {10, 20, 100, 50},
        .text = "E",
        .text_style = {.scale = 2.0f},
        .horizontal = ui2::UiAlign::Center,
        .vertical = ui2::UiAlign::End,
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, label);
    ui.end();

    const Vec2f measured = ui2::measure_text(label.text_style.font, label.text, label.text_style.scale);
    assert(!fills.empty());
    assert(approx(fills[0].x, label.bounds.x + (label.bounds.w - measured.x) * 0.5f));
    assert(approx(fills[0].y, label.bounds.y + label.bounds.h - measured.y));
}

void test_ui2_overflow_diagnostics_are_opt_in() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;

    ui2::Label label{
        .bounds = {0, 0, 8, 8},
        .text = "Overflow",
        .text_style = {.scale = 1.0f},
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, label);
    ui.end();
    assert(ui.diagnostics().empty());

    ui.set_debug_options({.detect_overflow = true});
    ui2::Button button{
        .id = ui2::make_id("too-small"),
        .bounds = {0, 0, 20, 12},
        .label = "Overflow",
        .text_style = {.scale = 1.0f},
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, label);
    ui2::run(ui, button);
    ui.end();

    bool saw_label = false;
    bool saw_button = false;
    for (const ui2::Context::Diagnostic& diagnostic : ui.diagnostics()) {
        saw_label = saw_label || diagnostic.widget == "Label";
        saw_button = saw_button || diagnostic.widget == "Button";
        assert(diagnostic.overflow.x > 0.0f || diagnostic.overflow.y > 0.0f);
    }
    assert(saw_label);
    assert(saw_button);
}

void test_ui2_text_overflow_clip_limits_draw_region() {
    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    std::vector<Rectf> rounded_outline_rects;
    std::vector<Rectf> clips;
    Renderer2D renderer = make_surface_geometry_recording_renderer(fills,
                                                                    colors,
                                                                    rounded_fills,
                                                                    rounded_outlines,
                                                                    rounded_outline_rects,
                                                                    clips);
    ui2::Context ui;
    ui.set_debug_options({.detect_draw_overflow = true, .log_draw_overflow = false});

    const Rectf label_bounds{10, 10, 8, 8};
    ui2::Label label{
        .bounds = label_bounds,
        .text = "Overflow",
        .text_style = {.scale = 1.0f},
    };

    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("LabelClipProbe", label_bounds);
        ui2::run(ui, label);
    }
    ui.end();
    bool saw_visible_overflow = false;
    for (const ui2::Context::Diagnostic& diagnostic : ui.diagnostics()) {
        saw_visible_overflow = saw_visible_overflow || diagnostic.kind == ui2::Context::DiagnosticKind::DrawOverflow;
    }
    assert(saw_visible_overflow);

    label.overflow = ui2::TextOverflow::Clip;
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("LabelClipProbe", label_bounds);
        ui2::run(ui, label);
    }
    ui.end();
    for (const ui2::Context::Diagnostic& diagnostic : ui.diagnostics()) {
        assert(diagnostic.kind != ui2::Context::DiagnosticKind::DrawOverflow);
    }
    assert(std::any_of(clips.begin(), clips.end(), [&](Rectf rect) {
        return rect_eq(rect, label_bounds.x, label_bounds.y, label_bounds.w, label_bounds.h);
    }));

    const Rectf wrapped_bounds{20, 20, 24, 8};
    ui2::WrappedText wrapped{
        .bounds = wrapped_bounds,
        .text = "One two three four",
        .text_style = {.scale = 1.0f},
        .line_spacing = 0.0f,
        .overflow = ui2::TextOverflow::Clip,
    };
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("WrappedClipProbe", wrapped_bounds);
        ui2::run(ui, wrapped);
    }
    ui.end();
    for (const ui2::Context::Diagnostic& diagnostic : ui.diagnostics()) {
        assert(diagnostic.kind != ui2::Context::DiagnosticKind::DrawOverflow);
    }
    assert(std::any_of(clips.begin(), clips.end(), [&](Rectf rect) {
        return rect_eq(rect, wrapped_bounds.x, wrapped_bounds.y, wrapped_bounds.w, wrapped_bounds.h);
    }));
}

void test_ui2_draw_overflow_check() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const Rectf parent{10, 10, 100, 100};

    auto count_draw_overflow = [&]() {
        i32 n = 0;
        for (const ui2::Context::Diagnostic& d : ui.diagnostics()) {
            if (d.kind == ui2::Context::DiagnosticKind::DrawOverflow) {
                ++n;
            }
        }
        return n;
    };

    // Opt-in: with the flag off, drawing outside a scope reports nothing.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent);
        ui.fill_rect({0, 0, 500, 500}, Color::rgba(255, 0, 0, 255)); // way outside
    }
    ui.end();
    assert(count_draw_overflow() == 0);

    ui.set_debug_options({.detect_draw_overflow = true, .log_draw_overflow = false});

    // A draw fully inside bounds does not trip.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent);
        ui.fill_rect({20, 20, 40, 40}, Color::rgba(0, 255, 0, 255));
    }
    ui.end();
    assert(count_draw_overflow() == 0);

    // A draw escaping bounds (no clip) trips, with the overshoot recorded.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent);
        ui.fill_rect({20, 20, 200, 40}, Color::rgba(0, 0, 255, 255)); // right edge past x=110
    }
    ui.end();
    assert(count_draw_overflow() == 1);
    for (const ui2::Context::Diagnostic& d : ui.diagnostics()) {
        if (d.kind == ui2::Context::DiagnosticKind::DrawOverflow) {
            assert(d.widget == "Probe");
            assert(d.overflow.x > 0.0f);
        }
    }

    // The same escaping draw is forgiven when clipped back within bounds: the painted
    // region (draw ∩ clip) stays inside, mirroring how virtualized rows rely on clipping.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent);
        ui.push_clip(parent);
        ui.fill_rect({20, 20, 200, 40}, Color::rgba(0, 0, 255, 255));
        ui.pop_clip();
    }
    ui.end();
    assert(count_draw_overflow() == 0);
}

void test_ui2_draw_overflow_corner_check() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui.set_debug_options({.detect_draw_overflow = true, .log_draw_overflow = false});
    const Rectf parent{0, 0, 100, 100};
    const f32 radius = 12.0f;

    auto count_corner = [&]() {
        i32 n = 0;
        for (const ui2::Context::Diagnostic& d : ui.diagnostics()) {
            if (d.kind == ui2::Context::DiagnosticKind::DrawOverflow && d.detail.find("corner") != std::string::npos) {
                ++n;
            }
        }
        return n;
    };

    // A square fill snug in the corner pokes past the arc (inside the rectangle, beyond the radius).
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent, radius);
        ui.fill_rect({0, 0, 100, 100}, Color::rgba(255, 0, 0, 255)); // exact bounds -> corners poke
    }
    ui.end();
    assert(count_corner() >= 1);

    // A fill inset past the radius on all sides clears every corner arc.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent, radius);
        ui.fill_rect({radius, radius, 100 - radius * 2, 100 - radius * 2}, Color::rgba(0, 255, 0, 255));
    }
    ui.end();
    assert(count_corner() == 0);

    // A rounded fill at exact bounds is corner-exempt (it already follows the radius).
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent, radius);
        ui.fill_rounded_rect({0, 0, 100, 100}, radius, Color::rgba(0, 0, 255, 255));
    }
    ui.end();
    assert(count_corner() == 0);

    // With no corner radius declared, the corner test never runs.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", parent, 0.0f);
        ui.fill_rect({0, 0, 100, 100}, Color::rgba(255, 255, 0, 255));
    }
    ui.end();
    assert(count_corner() == 0);
}

void test_ui2_invariant_checks() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui.set_debug_options({.detect_invariants = true, .log_invariants = false});

    auto count_kind = [&](ui2::Context::DiagnosticKind kind) {
        i32 n = 0;
        for (const ui2::Context::Diagnostic& d : ui.diagnostics()) {
            if (d.kind == kind) {
                ++n;
            }
        }
        return n;
    };

    // Unbalanced clip stack: a push with no matching pop is reported at end().
    input.begin_frame();
    ui.begin(input, renderer);
    ui.push_clip({0, 0, 10, 10});
    ui.end();
    assert(count_kind(ui2::Context::DiagnosticKind::UnbalancedStack) >= 1);
    ui.pop_clip(); // balance the renderer for subsequent sections

    // Balanced clip stack: no report.
    input.begin_frame();
    ui.begin(input, renderer);
    ui.push_clip({0, 0, 10, 10});
    ui.pop_clip();
    ui.end();
    assert(count_kind(ui2::Context::DiagnosticKind::UnbalancedStack) == 0);

    // Duplicate interaction id within a frame.
    input.begin_frame();
    ui.begin(input, renderer);
    const ui2::Id dup = ui2::make_id("shared");
    ui.region(dup, {0, 0, 10, 10});
    ui.region(dup, {20, 20, 10, 10});
    ui.end();
    assert(count_kind(ui2::Context::DiagnosticKind::DuplicateId) == 1); // reported once per id

    // Distinct ids: no report.
    input.begin_frame();
    ui.begin(input, renderer);
    ui.region(ui2::make_id("a"), {0, 0, 10, 10});
    ui.region(ui2::make_id("b"), {20, 20, 10, 10});
    ui.end();
    assert(count_kind(ui2::Context::DiagnosticKind::DuplicateId) == 0);

    // Degenerate (non-finite) draw coordinates.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", {0, 0, 100, 100});
        ui.fill_rect({0, 0, NAN, 10}, Color::rgba(255, 0, 0, 255));
    }
    ui.end();
    assert(count_kind(ui2::Context::DiagnosticKind::DegenerateRect) >= 1);
}

void test_ui2_contrast_check() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui.set_debug_options({.detect_contrast = true, .log_contrast = false});

    auto count_low_contrast = [&]() {
        i32 n = 0;
        for (const ui2::Context::Diagnostic& d : ui.diagnostics()) {
            if (d.kind == ui2::Context::DiagnosticKind::LowContrast) {
                ++n;
            }
        }
        return n;
    };

    // White text on a near-black fill: high contrast, no warning.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", {0, 0, 100, 40});
        ui.fill_rect({0, 0, 100, 40}, Color::rgba(20, 20, 20, 255));
        ui.text("Readable", {4, 4}, ui2::TextStyle{.scale = 1.0f, .color = Color::rgba(245, 245, 245, 255)});
    }
    ui.end();
    assert(count_low_contrast() == 0);

    // Mid-gray text on a slightly lighter gray: fails AA.
    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Probe", {0, 0, 100, 40});
        ui.fill_rect({0, 0, 100, 40}, Color::rgba(120, 120, 120, 255));
        ui.text("Faint", {4, 4}, ui2::TextStyle{.scale = 1.0f, .color = Color::rgba(150, 150, 150, 255)});
    }
    ui.end();
    assert(count_low_contrast() >= 1);
}

void test_ui2_pixel_causation() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui.set_debug_options({.record_draws = true});

    input.begin_frame();
    ui.begin(input, renderer);
    {
        auto scope = ui.draw_scope("Panel", {0, 0, 100, 100});
        ui.fill_rect({0, 0, 100, 100}, Color::rgba(40, 40, 40, 255));
    }
    {
        auto scope = ui.draw_scope("Badge", {20, 20, 30, 30});
        ui.fill_rect({20, 20, 30, 30}, Color::rgba(200, 0, 0, 128)); // semi-transparent, on top
    }
    ui.end();

    // A pixel under both: ordered Panel fill then Badge fill, composited opaque.
    const ui2::Context::PixelCausation hit = ui.pixel_causation({30, 30});
    assert(hit.layers.size() == 2);
    assert(hit.layers[0].widget == "Panel" && hit.layers[0].role == "fill");
    assert(hit.layers[1].widget == "Badge");
    assert(hit.result.a == 255);     // red(128) over opaque gray -> opaque
    assert(hit.result.r > hit.result.g); // reddened

    // A pixel under only the panel: a single layer.
    const ui2::Context::PixelCausation panel_only = ui.pixel_causation({80, 80});
    assert(panel_only.layers.size() == 1);
    assert(panel_only.layers[0].widget == "Panel");

    // A pixel outside everything: nothing, transparent result.
    const ui2::Context::PixelCausation miss = ui.pixel_causation({200, 200});
    assert(miss.layers.empty());
    assert(miss.result.a == 0);
}

void test_ui2_solitary_pixels() {
    constexpr kin::i32 W = 16;
    constexpr kin::i32 H = 16;
    std::vector<kin::u8> buf(static_cast<std::size_t>(W * H) * 4u);
    auto set = [&](kin::i32 x, kin::i32 y, kin::u8 r, kin::u8 g, kin::u8 b) {
        const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4u;
        buf[i] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = 255;
    };
    for (kin::i32 y = 0; y < H; ++y) {
        for (kin::i32 x = 0; x < W; ++x) {
            set(x, y, 40, 40, 40); // uniform field
        }
    }
    // Uniform field: nothing solitary.
    assert(kin::ui2::find_solitary_pixels(buf.data(), W, H).empty());

    // One strong outlier in the middle: found exactly.
    set(8, 8, 255, 0, 0);
    std::vector<kin::ui2::SolitaryPixel> found = kin::ui2::find_solitary_pixels(buf.data(), W, H);
    assert(found.size() == 1);
    assert(found[0].pos.x == 8 && found[0].pos.y == 8);
    assert(found[0].color.r == 255);

    // A full-height line is NOT solitary (its pixels have matching neighbours along the line),
    // while the lone outlier still is.
    for (kin::i32 y = 0; y < H; ++y) {
        set(5, y, 255, 0, 0);
    }
    found = kin::ui2::find_solitary_pixels(buf.data(), W, H);
    bool has_outlier = false;
    for (const kin::ui2::SolitaryPixel& sp : found) {
        if (sp.pos.x == 8 && sp.pos.y == 8) { has_outlier = true; }
        assert(sp.pos.x != 5); // no line pixel flagged
    }
    assert(has_outlier);
}

void test_game_widget_measure() {
    const Sprite sprite = make_sprite({16, 12});
    assert(approx(ui2::measure(ui2::Image{.sprite = sprite}).x, 16));
    assert(approx(ui2::measure(ui2::Image{.sprite = sprite}).y, 12));
    assert(approx(ui2::measure(ui2::Image{.sprite = sprite, .size = {30, 40}}).x, 30));

    assert(rect_eq({0, 0, ui2::measure(ui2::NineSlicePanel{.skin = {.sprite = sprite, .left = 3, .top = 4, .right = 5, .bottom = 6}}).x,
                    ui2::measure(ui2::NineSlicePanel{.skin = {.sprite = sprite, .left = 3, .top = 4, .right = 5, .bottom = 6}}).y},
                   0,
                   0,
                   8,
                   10));
    assert(rect_eq({0, 0, ui2::measure(ui2::Spacer{.size = {7, 9}}).x, ui2::measure(ui2::Spacer{.size = {7, 9}}).y},
                   0,
                   0,
                   7,
                   9));

    ui2::IconButton icon{.icon = sprite, .icon_size = {20, 10}};
    assert(approx(ui2::measure(icon).x, 44)); // icon + horizontal padding
    assert(approx(ui2::measure(icon).y, 22)); // icon + vertical padding

    assert(approx(ui2::measure(ui2::Slider{}).x, 120.0f));
    assert(ui2::measure(ui2::Slider{}).y >= 18.0f);
    assert(approx(ui2::measure(ui2::ProgressBar{}).x, 120.0f));
    assert(ui2::measure(ui2::ProgressBar{}).y >= 14.0f);
    assert(ui2::measure(ui2::Meter{}).x > 120.0f);
    assert(ui2::measure(ui2::Meter{}).y >= 14.0f);

    ui2::PromptLabel prompt{.prompt = "[Space]", .text = "START"};
    assert(ui2::measure(prompt).x > 0.0f);
    assert(ui2::measure(prompt).y > 0.0f);

    ui2::DialogBox dialog{.title = "TITLE", .body = "Body"};
    assert(ui2::measure(dialog).x > 0.0f);
    assert(ui2::measure(dialog).y > ui2::measure(ui2::Label{.text = "Body"}).y);

    assert(rect_eq({0, 0, ui2::measure(ui2::DialogueView{}).x, ui2::measure(ui2::DialogueView{}).y}, 0, 0, 420, 180));
}

void test_dialogue_view_render_and_choice() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    ui2::DialogueView view{
        .id = ui2::make_id("dialogue-view"),
        .bounds = {0, 0, 320, 160},
        .model = {.active = true,
                  .speaker_name = "Guide",
                  .speaker_color = Color::rgb(128, 226, 160),
                  .text_markup = "Take this?",
                  .choices = {{.id = "yes", .text_markup = "Yes", .enabled = true},
                              {.id = "no", .text_markup = "No", .enabled = false}}},
        .show_disabled_choices = true,
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, view);
    ui.end();
    assert(!draws.empty());
    assert(!view.choice_requested);

    input.begin_frame();
    input.set_mouse_pos({24, 100});
    ui.begin(input, renderer);
    ui2::run(ui, view);
    ui.end();

    input.begin_frame();
    input.set_mouse_pos({24, 100});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::run(ui, view);
    ui.end();

    input.begin_frame();
    input.set_mouse_pos({24, 100});
    input.set_mouse_held(MouseButton::Left, false);
    ui.begin(input, renderer);
    ui2::run(ui, view);
    ui.end();

    assert(view.choice_requested);
    assert(view.choice_id == "yes");
}

void test_image_and_nine_slice_draw_geometry() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    const Sprite sprite = make_sprite({16, 16});

    ui2::Image image{.bounds = {10, 20, 40, 30}, .sprite = sprite};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, image);
    ui.end();
    assert(draws.size() == 1);
    assert(rect_eq(draws[0], 10, 20, 40, 30));

    draws.clear();
    ui2::NineSlicePanel panel{.bounds = {0, 0, 30, 30}, .skin = {.sprite = sprite, .left = 4, .top = 4, .right = 4, .bottom = 4}};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, panel);
    ui.end();
    assert(draws.size() == 9);
    assert(rect_eq(draws[0], 0, 0, 4, 4));
    assert(rect_eq(draws[8], 26, 26, 4, 4));
}

void test_meter_and_icon_button_geometry() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;

    ui2::Meter meter{.bounds = {0, 0, 50, 10}, .value = 2.0f, .max = 4.0f, .segments = 4, .gap = 2.0f};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, meter);
    ui.end();
    assert(draws.size() == 4);
    assert(rect_eq(draws[0], 0, 0, 11, 10));
    assert(rect_eq(draws[3], 39, 0, 11, 10));

    draws.clear();
    ui2::IconButton icon{
        .id = ui2::make_id("icon"),
        .bounds = {0, 0, 40, 40},
        .icon = make_sprite({12, 12}),
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, icon);
    ui.end();
    assert(std::any_of(draws.begin(), draws.end(), [](Rectf draw) { return rect_eq(draw, 14, 14, 12, 12); }));
}

void test_icon_slot_measure_render_and_click() {
    assert(rect_eq({0, 0, ui2::measure(ui2::IconSlot{}).x, ui2::measure(ui2::IconSlot{}).y}, 0, 0, 48, 48));
    assert(rect_eq({0, 0, ui2::measure(ui2::IconSlot{.size = {32, 40}}).x, ui2::measure(ui2::IconSlot{.size = {32, 40}}).y}, 0, 0, 32, 40));
    ui2::IconSlot large_slot{
        .icon = make_sprite({80, 40}),
        .text = "Wide",
        .count = 100,
    };
    assert(ui2::measure(large_slot).x > 48.0f);
    assert(ui2::measure(large_slot).y > 48.0f);

    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    ui.set_theme(ui2::game_theme());
    ui2::IconSlot slot{
        .id = ui2::make_id("slot"),
        .bounds = {0, 0, 48, 48},
        .text = "A",
        .count = 3,
        .selected = true,
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, slot);
    ui.end();
    assert(draws.size() >= 3);
    bool saw_slot = false;
    bool saw_badge = false;
    const f32 bw = ui2::game_style_preset().border_width; // slot interior is inset by the border
    for (Rectf draw : draws) {
        saw_slot = saw_slot || rect_eq(draw, bw, bw, 48.0f - 2.0f * bw, 48.0f - 2.0f * bw);
        saw_badge = saw_badge || (draw.x > 30.0f && draw.y > 30.0f && draw.w > 0.0f && draw.h > 0.0f);
    }
    assert(saw_slot);
    assert(saw_badge);

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, slot);
        ui.end();
    };
    input.begin_frame();
    input.set_mouse_pos({24, 24});
    frame();
    input.begin_frame();
    input.set_mouse_pos({24, 24});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({24, 24});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(slot.clicked);
}

void test_icon_slot_drag_drop_reporting() {
    Input input;
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    ui2::Context ui;
    ui2::IconSlot source{
        .id = ui2::make_id("slot-source"),
        .bounds = {0, 0, 48, 48},
        .text = "Gem",
        .drag_enabled = true,
        .drag_payload_value = 7,
    };
    ui2::IconSlot target{
        .id = ui2::make_id("slot-target"),
        .bounds = {80, 0, 48, 48},
        .drop_enabled = true,
    };
    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, source);
        ui2::run(ui, target);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({24, 24});
    frame();
    input.begin_frame();
    input.set_mouse_pos({24, 24});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({100, 24});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(source.drag_started);
    assert(source.dragging);
    input.begin_frame();
    input.set_mouse_pos({100, 24});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(target.dropped);
    assert(target.dropped_text == "Gem");
    assert(target.dropped_value == 7);
}

void test_icon_button_click_cycle() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::IconButton icon{
        .id = ui2::make_id("icon-click"),
        .bounds = {0, 0, 40, 40},
        .icon = make_sprite({12, 12}),
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, icon);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({20, 20});
    frame();

    input.begin_frame();
    input.set_mouse_pos({20, 20});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(icon.interaction.active);

    input.begin_frame();
    input.set_mouse_pos({20, 20});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(icon.clicked);
}

// At refresh > sim rate most rendered frames run zero fixed-update steps. Per-frame
// mouse edges must survive those frames so update-driven UI still observes a click,
// while a claimed press must not persist and re-fire.
void test_input_mouse_edges_survive_zero_step_frames() {
    Input input;
    input.set_mouse_pos({10, 10});

    // A press arrives on a stepping frame.
    InputFrameTestHook::begin_frame(input, true);
    input.set_mouse_held(MouseButton::Left, true);
    assert(input.mouse_frame_pressed(MouseButton::Left));

    // Zero-step frames preserve the press edge for a later stepping frame to read.
    InputFrameTestHook::begin_frame(input, false);
    assert(input.mouse_frame_pressed(MouseButton::Left));
    InputFrameTestHook::begin_frame(input, false);
    assert(input.mouse_frame_pressed(MouseButton::Left));

    // The next stepping frame clears it.
    InputFrameTestHook::begin_frame(input, true);
    assert(!input.mouse_frame_pressed(MouseButton::Left));

    // A claimed press stays readable this frame but is cleared on the next begin_frame
    // even when that frame ran zero steps (so it cannot re-fire while persisting).
    InputFrameTestHook::begin_frame(input, true);
    input.set_mouse_pressed(MouseButton::Left);
    assert(input.mouse_frame_pressed(MouseButton::Left));
    input.consume_mouse_frame_pressed(MouseButton::Left);
    assert(input.mouse_frame_pressed(MouseButton::Left));
    InputFrameTestHook::begin_frame(input, false);
    assert(!input.mouse_frame_pressed(MouseButton::Left));

    // Per-frame keyboard edges keep their one-frame lifetime (no 0-step persistence):
    // update-driven keyboard must use the sticky pressed() API instead.
    input.bind("hop", Key::Space);
    InputFrameTestHook::begin_frame(input, true);
    input.set_action_pressed("hop");
    assert(input.frame_pressed("hop"));
    InputFrameTestHook::begin_frame(input, false);
    assert(!input.frame_pressed("hop"));
}

// A single click must register exactly once whether ui2 widgets are driven from
// update() (runs only on stepping frames) or render() (runs every frame), at
// refresh > sim rate where most frames run zero steps. Regression for the
// "press the mouse N times before it registers" bug.
void test_ui2_click_survives_zero_step_frames() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const Rectf widget{0, 0, 100, 40};
    const ui2::Id id = ui2::make_id("zstep-btn");
    input.set_mouse_pos({10, 10});

    int clicks = 0;
    const auto run = [&]() {
        ui.begin(input, renderer);
        const ui2::Interaction it = ui.region(id, widget);
        ui.end();
        if (it.clicked) {
            ++clicks;
        }
    };

    bool prev_stepped = true; // App initialises advance_input = true.
    const auto begin = [&](bool stepped_this_frame) {
        InputFrameTestHook::begin_frame(input, prev_stepped);
        prev_stepped = stepped_this_frame;
    };

    // --- Update-driven: region() runs ONLY on stepping frames. ---
    begin(true);  run();                                              // warm up (widget becomes hot)
    begin(false); input.set_mouse_held(MouseButton::Left, true);      // press on a 0-step frame
    begin(false); input.set_mouse_held(MouseButton::Left, false);     // release on a 0-step frame
    begin(true);  run();                                              // stepping frame reads the click
    begin(true);  run();                                              // and does not re-fire it
    assert(clicks == 1);

    // --- Render-driven: region() runs every frame across 0-step frames. ---
    clicks = 0;
    begin(true);  run();                                              // warm up
    begin(false); input.set_mouse_held(MouseButton::Left, true);  run();
    begin(false);                                                   run();  // still held
    begin(false); input.set_mouse_held(MouseButton::Left, false); run();  // release
    begin(false);                                                   run();
    assert(clicks == 1);
}

void test_editor_widget_measure() {
    assert(rect_eq({0, 0, ui2::measure(ui2::ScrollView{}).x, ui2::measure(ui2::ScrollView{}).y}, 0, 0, 160, 120));
    assert(rect_eq({0, 0, ui2::measure(ui2::TextInput{}).x, ui2::measure(ui2::TextInput{}).y}, 0, 0, 160, 26));
    assert(rect_eq({0, 0, ui2::measure(ui2::NumberInput{}).x, ui2::measure(ui2::NumberInput{}).y}, 0, 0, 96, 26));
    assert(rect_eq({0, 0, ui2::measure(ui2::ComboBox{}).x, ui2::measure(ui2::ComboBox{}).y}, 0, 0, 140, 26));
    ui2::TextInput tall_text{
        .state = {.text = "Tall text"},
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(tall_text).y > 26.0f);
    ui2::NumberInput tall_number{
        .value = 1234.0f,
        .min = -9999.0f,
        .max = 9999.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(tall_number).y > 26.0f);
    ui2::ComboBox tall_combo{
        .items = {"Overview", "Pipeline"},
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(tall_combo).y > 26.0f);
}

// F7: Button/TextInput/ComboBox/NumberInput share one control height when themed, so a
// row of mixed controls lines up; raw (unthemed) styles keep min_height 0 (auto-size).
void test_ui2_control_height_alignment() {
    const ui2::Theme theme = ui2::game_theme();
    const f32 ch = theme.sizes.compact_control_height;
    assert(theme.button.min_height == ch);
    assert(theme.input.min_height == ch);

    // Small content -> every themed control floors to the shared height (they align).
    const ui2::TextStyle tiny{.font = ui2::bitmap_font(), .scale = 0.5f, .color = colors::white};
    ui2::Button btn{.label = "OK", .text_style = tiny, .style = theme.button};
    ui2::TextInput txt{.text_style = tiny, .style = theme.input};
    ui2::NumberInput num{.text_style = tiny, .style = theme.input};
    ui2::ComboBox combo{.items = {"A", "B"}, .text_style = tiny, .style = theme.input};
    assert(approx(ui2::measure(btn).y, ch));
    assert(approx(ui2::measure(txt).y, ch));
    assert(approx(ui2::measure(num).y, ch));
    assert(approx(ui2::measure(combo).y, ch));

    // Large content still grows past the floor.
    ui2::Button big{.label = "Tall",
                    .text_style = {.font = ui2::bitmap_font(), .scale = 8.0f, .color = colors::white},
                    .style = theme.button};
    assert(ui2::measure(big).y > ch);

    // Raw styles are unchanged: min_height 0 -> purely content-derived (no floor).
    assert(ui2::WidgetStyle{}.min_height == 0.0f);
    ui2::Button raw{.label = "OK", .text_style = tiny};
    assert(ui2::measure(raw).y < ch);

    // themed_prompt_label derives its paddings from the theme gap (shared rhythm, scales).
    const ui2::PromptLabel prompt = ui2::themed_prompt_label(theme, "Jump", "Jump");
    assert(approx(prompt.padding.left, theme.sizes.gap));
    assert(approx(prompt.padding.top, theme.sizes.gap * 0.5f));
    assert(approx(prompt.chip_padding.top, theme.sizes.gap * 0.5f));
    assert(approx(prompt.gap, theme.sizes.gap));
}

void test_scroll_view_geometry_and_wheel() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::ScrollView scroll{.bounds = {0, 0, 100, 50}, .content_height = 150.0f};

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_wheel_y(-1.0f);
    ui.begin(input, renderer);
    ui2::run(ui, scroll);
    ui.end();

    assert(scroll.changed);
    assert(approx(scroll.offset, 48.0f));
    assert(rect_eq(scroll.viewport, 0, 0, 90, 50));
    assert(rect_eq(scroll.content, 0, -48, 100, 150));
    assert(approx(scroll.thumb.h, 18.0f));

    ui2::ScrollView horizontal{
        .bounds = {0, 0, 100, 50},
        .content_size = {200.0f, 120.0f},
        .horizontal = true,
    };
    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_wheel_y(-1.0f);
    ui.begin(input, renderer);
    ui2::run(ui, horizontal);
    ui.end();

    assert(rect_eq(horizontal.viewport, 0, 0, 90, 40));
    assert(rect_eq(horizontal.h_track, 0, 40, 90, 10));
    assert(approx(horizontal.offset, 48.0f));
    assert(horizontal.h_thumb.w > 0.0f);
}

void test_scroll_primitives() {
    ui2::ScrollState state{
        .offset = {-10.0f, 500.0f},
        .content_size = {300.0f, 400.0f},
        .viewport_size = {100.0f, 120.0f},
    };
    assert(ui2::clamp_scroll(state));
    assert(approx(state.offset.x, 0.0f));
    assert(approx(state.offset.y, 280.0f));
    assert(ui2::scroll_by(state, {-20.0f, -80.0f}));
    assert(approx(state.offset.y, 200.0f));
    assert(ui2::ensure_visible(state, {150.0f, 40.0f, 20.0f, 30.0f}));
    assert(approx(state.offset.x, 70.0f));
    assert(approx(state.offset.y, 40.0f));
    assert(ui2::first_visible_index(47.0f, 12.0f) == 3);

    const ui2::ScrollbarLayout vertical = ui2::layout_scrollbar({100, 0, 10, 120}, state, ui2::ScrollAxis::Vertical);
    assert(vertical.scrollable);
    assert(rect_eq(vertical.track, 100, 0, 10, 120));
    assert(vertical.thumb.h >= 18.0f);

    const ui2::ScrollbarLayout horizontal = ui2::layout_scrollbar({0, 120, 100, 10}, state, ui2::ScrollAxis::Horizontal);
    assert(horizontal.scrollable);
    assert(horizontal.thumb.w >= 18.0f);
}

void test_text_input_editing() {
    Input input;
    input.bind("accept", Key::Enter);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::TextInput text{.id = ui2::make_id("text"), .bounds = {0, 0, 120, 24}};

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, text);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    frame();

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(text.state.active);

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, false);
    input.set_text_input("A");
    frame();
    assert(text.result.changed);
    assert(text.state.text == "A");

    input.begin_frame();
    input.set_text_input("");
    input.set_action_pressed("accept");
    frame();
    assert(text.result.committed);
    assert(!text.state.active);
}

void test_number_input_editing() {
    Input input;
    input.bind("menu_right", Key::Right);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::NumberInput number{.id = ui2::make_id("number"), .bounds = {0, 0, 100, 24}, .value = 1.0f, .min = 0.0f, .max = 10.0f, .step = 0.5f};

    input.begin_frame();
    ui.begin(input, renderer);
    ui.state().set_focused(number.id);
    ui2::run(ui, number);
    ui.end();

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("menu_right");
    ui.begin(input, renderer);
    ui.state().set_focused(number.id);
    ui2::run(ui, number);
    ui.end();

    assert(number.changed);
    assert(approx(number.value, 1.5f));
}

void test_combo_box_selection() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::ComboBox combo{.id = ui2::make_id("combo"), .bounds = {0, 0, 100, 20}, .items = {"Low", "High"}};

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, combo);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    frame();

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(combo.state.open);

    input.begin_frame();
    input.set_mouse_pos({10, 55});
    frame();

    input.begin_frame();
    input.set_mouse_pos({10, 55});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    input.begin_frame();
    input.set_mouse_pos({10, 55});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(combo.result.changed);
    assert(combo.state.selected == 1);
    assert(!combo.state.open);
}

void test_color_picker_slider_changes_channel() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::ColorPicker picker{.id = ui2::make_id("picker"), .bounds = {0, 0, 260, 242}, .value = Color::rgba(0, 32, 48, 255), .mode = ui2::ColorPickerMode::Rgba};

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, picker);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({120, 46});
    frame();

    input.begin_frame();
    input.set_mouse_pos({120, 46});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    assert(picker.changed);
    assert(picker.value.r > 0);
    assert(picker.value.g == 32);
    assert(picker.value.b == 48);
}

void test_icon_grid_layout_and_click() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    ui2::IconGrid grid{
        .id = ui2::make_id("grid"),
        .bounds = {0, 0, 120, 80},
        .items = {{.text = "A"}, {.text = "B"}, {.text = "C"}},
        .cell_size = {20, 20},
        .spacing = {4, 4},
        .columns = 2,
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, grid);
    ui.end();
    const Vec2f measured_grid = ui2::measure(grid);
    assert(measured_grid.x > 44.0f);
    assert(measured_grid.y > 44.0f);
    const f32 measured_cell_w = (measured_grid.x - grid.spacing.x) * 0.5f;
    const f32 measured_cell_h = (measured_grid.y - grid.spacing.y) * 0.5f;
    assert(draws.size() >= 5);
    // Cells are inset by the panel border. Compute it the same way collection_background does.
    const auto& ps = ui.theme().panel_surface;
    const f32 panel_border = (ps.border_mode == kin::ui2::BorderMode::Inside && ps.border_width > 0.0f)
                                 ? ps.border_width : 0.0f;
    // Cell A is selected (index 0 by default) so it paints its selection tint at
    // the expected position. Unselected cells are borderless/transparent (clean
    // icon-grid selection model), so only the selected cell paints a surface —
    // cell B's layout is validated by the click hit-test below.
    bool saw_cell_a = false;
    for (Rectf draw : draws) {
        saw_cell_a = saw_cell_a || rect_eq(draw, panel_border, panel_border, measured_cell_w, measured_cell_h);
    }
    assert(saw_cell_a);

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, grid);
        ui.end();
    };
    input.begin_frame();
    const f32 second_cell_x = measured_cell_w + grid.spacing.x + measured_cell_w * 0.5f;
    input.set_mouse_pos({second_cell_x, measured_cell_h * 0.5f});
    frame();
    input.begin_frame();
    input.set_mouse_pos({second_cell_x, measured_cell_h * 0.5f});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({second_cell_x, measured_cell_h * 0.5f});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(grid.clicked_index == 1);
    assert(grid.activated == 1);
    assert(grid.selected == 1);
}

void test_icon_grid_navigation() {
    Input input;
    input.bind("menu_right", Key::Right);
    input.bind("menu_down", Key::Down);
    input.bind("accept", Key::Enter);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::IconGrid grid{
        .id = ui2::make_id("grid-nav"),
        .bounds = {0, 0, 120, 80},
        .items = {{.text = "A"}, {.text = "B"}, {.text = "C"}, {.text = "D"}},
        .cell_size = {20, 20},
        .spacing = {4, 4},
        .columns = 2,
    };

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    ui.begin(input, renderer);
    ui2::run(ui, grid);
    ui.end();

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("menu_right");
    ui.begin(input, renderer);
    ui2::run(ui, grid);
    ui.end();
    assert(grid.changed);
    assert(grid.selected == 1);

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("menu_down");
    ui.begin(input, renderer);
    ui2::run(ui, grid);
    ui.end();
    assert(grid.selected == 3);

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    ui2::run(ui, grid);
    ui.end();
    assert(grid.activated == 3);
}

void test_icon_grid_drag_drop_reports_cells() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::IconGrid grid{
        .id = ui2::make_id("grid-drag"),
        .bounds = {0, 0, 120, 80},
        .items = {{.text = "A"}, {.text = "B"}, {.text = "C"}},
        .cell_size = {20, 20},
        .spacing = {4, 4},
        .columns = 2,
        .drag_enabled = true,
        .drop_enabled = true,
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, grid);
        ui.end();
    };
    const Vec2f grid_size = ui2::measure(grid);
    const f32 cell_w = (grid_size.x - grid.spacing.x) * 0.5f;
    const f32 cell_h = (grid_size.y - grid.spacing.y) * 0.5f;
    const Vec2f first_cell{cell_w * 0.5f, cell_h * 0.5f};
    const Vec2f third_cell{cell_w * 0.5f, cell_h + grid.spacing.y + cell_h * 0.5f};

    input.begin_frame();
    input.set_mouse_pos(first_cell);
    frame();
    input.begin_frame();
    input.set_mouse_pos(first_cell);
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos(third_cell);
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(grid.drag_started);
    assert(grid.drag_source == 0);
    assert(grid.drop_target == 2);
    input.begin_frame();
    input.set_mouse_pos(third_cell);
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(grid.dropped);
    assert(grid.dropped_source == 0);
    assert(grid.dropped_target == 2);
}

void test_menu_list_and_context_menu() {
    Input input;
    input.bind("menu_down", Key::Down);
    input.bind("accept", Key::Enter);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::MenuList menu{
        .id = ui2::make_id("menu-list"),
        .bounds = {0, 0, 140, 90},
        .items = {{.id = "start", .label = "Start"}, {.id = "sep", .separator = true}, {.id = "quit", .label = "Quit"}},
    };

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("menu_down");
    ui.begin(input, renderer);
    ui2::run(ui, menu);
    ui.end();
    assert(menu.changed);
    assert(menu.selected == 2);

    menu.selected = 1;
    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    ui2::run(ui, menu);
    ui.end();
    assert(menu.activated == -1);

    menu.selected = 2;
    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    ui2::run(ui, menu);
    ui.end();
    assert(menu.activated == 2);
    assert(menu.activated_id == "quit");

    ui2::MenuList passive_menu{
        .id = ui2::make_id("passive-menu"),
        .bounds = {0, 0, 140, 90},
        .items = {{.id = "overview", .label = "Overview"}, {.id = "customers", .label = "Customers"}},
        .selected = -1,
    };
    input.begin_frame();
    input.set_mouse_pos({200, 200});
    ui.begin(input, renderer);
    ui2::run(ui, passive_menu);
    ui.end();
    assert(passive_menu.selected == -1);

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    input.set_action_pressed("menu_down");
    ui.begin(input, renderer);
    ui2::run(ui, passive_menu);
    ui.end();
    assert(passive_menu.selected == 0);

    const ui2::Id popup = ui2::make_id("context-menu");
    input.begin_frame();
    ui.begin(input, renderer);
    ui.open_popup(popup);
    ui2::ContextMenuResult result = ui2::context_menu(ui, popup, {0, 0, 10, 10}, menu);
    ui.end();
    assert(result.open);
    assert(result.opened);
}

void test_menu_list_mouse_wheel_scrolls_visible_rows() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::MenuList menu{
        .id = ui2::make_id("scroll-menu-list"),
        .bounds = {0, 0, 140, 62},
        .items = {{.id = "one", .label = "One"},
                  {.id = "two", .label = "Two"},
                  {.id = "three", .label = "Three"},
                  {.id = "four", .label = "Four"}},
        .row_height = 20.0f,
        .row_spacing = 2.0f,
        .scrollable = true,
        .wheel_step = 24.0f,
    };

    input.begin_frame();
    input.set_mouse_pos({20, 20});
    input.set_mouse_wheel_y(-1.0f);
    ui.begin(input, renderer);
    ui2::run(ui, menu);
    ui.end();
    assert(menu.scroll_changed);
    assert(menu.scroll_offset > 0.0f);

    const float after_first_wheel = menu.scroll_offset;
    input.begin_frame();
    input.set_mouse_pos({20, 20});
    input.set_mouse_wheel_y(-1.0f);
    ui.begin(input, renderer);
    ui2::run(ui, menu);
    ui.end();
    assert(menu.scroll_offset >= after_first_wheel);
}

void test_popup_placement_match_width_and_flips() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id popup = ui2::make_id("popup-placement");

    input.begin_frame();
    ui.begin(input, renderer);
    ui.open_popup(popup);
    ui2::Context::PopupResult placed = ui.begin_popup(popup,
                                                      {120, 90, 80, 20},
                                                      {.size = {40, 40},
                                                       .offset = {0, 20},
                                                       .anchor = ui2::UiAnchor::TopLeft,
                                                       .screen = {0, 0, 200, 120},
                                                       .flip_y = true,
                                                       .match_anchor_width = true});
    ui.end_popup();
    ui.end();

    assert(placed.open);
    assert(rect_eq(placed.bounds, 120, 50, 80, 40));
}

void test_popup_child_click_keeps_parent_open() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id parent = ui2::make_id("parent-popup");
    const ui2::Id child = ui2::make_id("child-popup");

    input.begin_frame();
    ui.begin(input, renderer);
    ui.open_popup(parent);
    ui2::Context::PopupResult p1 = ui.begin_popup(parent, {0, 0, 10, 10}, {.size = {80, 60}, .offset = {}, .anchor = ui2::UiAnchor::TopLeft});
    ui.open_subpopup(parent, child);
    ui2::Context::PopupResult c1 = ui.begin_popup(child, {80, 0, 10, 10}, {.size = {80, 60}, .offset = {}, .anchor = ui2::UiAnchor::TopLeft});
    ui.end_popup();
    ui.end_popup();
    ui.end();
    assert(p1.open && c1.open);

    input.begin_frame();
    input.set_mouse_pos({90, 10});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::Context::PopupResult p2 = ui.begin_popup(parent, {0, 0, 10, 10}, {.size = {80, 60}, .offset = {}, .anchor = ui2::UiAnchor::TopLeft});
    ui2::Context::PopupResult c2 = ui.begin_popup(child, {80, 0, 10, 10}, {.size = {80, 60}, .offset = {}, .anchor = ui2::UiAnchor::TopLeft});
    ui.end_popup();
    ui.end_popup();
    ui.end();
    assert(p2.open);
    assert(c2.open);
    assert(ui.popup_open(parent));
}

void test_menu_list_rich_rows_and_skip_navigation() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    input.bind("menu_down", Key::Down);
    input.bind("accept", Key::Enter);
    ui2::Context ui;
    ui2::MenuList menu{
        .id = ui2::make_id("rich-menu"),
        .bounds = {0, 0, 180, 120},
        .items = {
            {.id = "disabled", .label = "Disabled", .enabled = false},
            {.id = "sep", .separator = true},
            {.id = "open", .label = "Open", .submenu_id = "recent", .icon = make_sprite({12, 12}), .default_item = true, .prompt = "Enter"},
            {.id = "delete", .label = "Delete", .danger = true, .tooltip = "Remove"},
        },
    };

    input.begin_frame();
    input.set_action_pressed("menu_down");
    ui.begin(input, renderer);
    ui2::run(ui, menu);
    ui.end();
    assert(menu.selected == 3);
    assert(!draws.empty());
    // Icon sits in the icon gutter, just past the left padding. No item is
    // checkable here, so there is no dead check column in front of it (F7).
    bool saw_icon = false;
    for (Rectf draw : draws) {
        saw_icon = saw_icon || (draw.w == 12.0f && draw.h == 12.0f && draw.x > 8.0f);
    }
    assert(saw_icon);

    menu.selected = 2;
    input.begin_frame();
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    ui2::run(ui, menu);
    ui.end();
    assert(menu.submenu_requested);
    assert(menu.submenu_id == "recent");
    assert(menu.activated == -1);
}

void test_popup_menu_submenu_and_menu_bar() {
    Input input;
    input.bind("accept", Key::Enter);
    input.bind("menu_right", Key::Right);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id file_menu_id = ui2::make_id("file-popup");
    const ui2::Id recent_menu_id = ui2::make_id("recent-popup");
    ui2::MenuBar bar{
        .id = ui2::make_id("menu-bar"),
        .bounds = {0, 0, 200, 28},
        .items = {{.id = "file", .label = "File", .menu_id = file_menu_id}},
    };
    ui2::MenuList file{
        .id = ui2::make_id("file-menu"),
        .items = {{.id = "recent", .label = "Recent", .submenu_id = "recent"}},
    };
    ui2::MenuList recent{
        .id = ui2::make_id("recent-menu"),
        .items = {{.id = "project-a", .label = "Project A"}},
    };

    input.begin_frame();
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    ui2::run(ui, bar);
    ui2::ContextMenuResult file_result = ui2::popup_menu(ui,
                                                         file_menu_id,
                                                         {0, 28, 60, 1},
                                                         file,
                                                         {.size = ui2::measure(file), .offset = {}, .anchor = ui2::UiAnchor::TopLeft});
    ui.end();
    assert(bar.opened == 0);
    assert(ui.popup_open(file_menu_id));
    assert(file_result.open);

    input.begin_frame();
    input.set_action_pressed("menu_right");
    ui.begin(input, renderer);
    file_result = ui2::popup_menu(ui,
                                  file_menu_id,
                                  {0, 28, 60, 1},
                                  file,
                                  {.size = ui2::measure(file), .offset = {}, .anchor = ui2::UiAnchor::TopLeft});
    ui2::ContextMenuResult child_result{};
    if (file_result.submenu_requested) {
        child_result = ui2::submenu(ui, file_menu_id, recent_menu_id, file_result.submenu_anchor, recent);
    }
    ui.end();
    assert(file_result.submenu_requested);
    assert(child_result.open);
    assert(ui.popup_open(recent_menu_id));

    input.begin_frame();
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    file_result = ui2::popup_menu(ui,
                                  file_menu_id,
                                  {0, 28, 60, 1},
                                  file,
                                  {.size = ui2::measure(file), .offset = {}, .anchor = ui2::UiAnchor::TopLeft});
    child_result = ui2::submenu(ui, file_menu_id, recent_menu_id, file_result.submenu_anchor.w > 0.0f ? file_result.submenu_anchor : Rectf{120, 28, 120, 26}, recent);
    ui.end();
    assert(child_result.activated_id == "project-a");
    assert(child_result.closed);
}

void test_overlay_and_hud_widgets_render_geometry() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;

    ui2::Nameplate nameplate{.bounds = {0, 0, 100, 30}, .label = "Enemy", .value = 0.5f};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, nameplate);
    ui.end();
    assert(draws.size() >= 3);
    assert(rect_eq(draws[0], 0, 0, 100, 30));

    draws.clear();
    ui2::SelectionRect selection{.start = {30, 40}, .end = {10, 20}};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, selection);
    ui.end();
    assert(selection.active);
    assert(rect_eq(selection.bounds, 10, 20, 20, 20));

    draws.clear();
    ui2::TargetReticle reticle{.bounds = {0, 0, 30, 30}, .valid = false};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, reticle);
    ui.end();
    assert(draws.size() >= 8);

    draws.clear();
    ui2::ResourceRow resources{.bounds = {0, 0, 180, 24}, .items = {{.label = "Gold", .value = "12"}}};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, resources);
    ui.end();
    assert(!draws.empty());

    draws.clear();
    ui2::LabeledBar bar{.bounds = {0, 0, 100, 20}, .label = "HP", .value_text = "5/10", .value = 0.5f};
    assert(ui2::measure(bar).x >= 160.0f);
    assert(ui2::measure(bar).y >= 24.0f);
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, bar);
    ui.end();
    assert(draws.size() >= 2);
    bool saw_bar_fill = false;
    for (Rectf draw : draws) {
        saw_bar_fill = saw_bar_fill || rect_eq(draw, 0, 0, 50, 20);
    }
    assert(saw_bar_fill);

    draws.clear();
    ui2::IconMeter icon_meter{.bounds = {0, 0, 60, 16}, .value = 2, .max = 3};
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, icon_meter);
    ui.end();
    assert(draws.size() == 3);
}

void test_rich_text_and_text_area() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;

    ui2::RichTextLine line{.bounds = {0, 0, 200, 20}, .spans = {{.text = "A"}, {.text = "B", .color = Color::rgb(255, 0, 0)}}};
    assert(ui2::measure(line).x > 0.0f);
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, line);
    ui.end();

    ui2::TextArea area{.bounds = {0, 0, 100, 40}, .text = "hello world hello world"};
    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_wheel_y(-1.0f);
    ui.begin(input, renderer);
    ui2::run(ui, area);
    ui.end();
    assert(area.offset > 0.0f);
}

void test_collection_widget_measure() {
    assert(ui2::measure(ui2::WrappedText{.text = "hello world"}).x > 0.0f);
    assert(rect_eq({0, 0, ui2::measure(ui2::Separator{.thickness = 3}).x, ui2::measure(ui2::Separator{.thickness = 3}).y}, 0, 0, 1, 3));
    assert(rect_eq({0, 0, ui2::measure(ui2::ListView{}).x, ui2::measure(ui2::ListView{}).y}, 0, 0, 180, 160));
    assert(rect_eq({0, 0, ui2::measure(ui2::Table{}).x, ui2::measure(ui2::Table{}).y}, 0, 0, 260, 180));
    ui2::Table measured_table{
        .columns = {{.label = "Name", .width = 20}, {.label = "Value", .sizing = ui2::UiTableColumnSizing::Stretch}},
        .rows = {{"Alpha", "One"}, {"Beta", "Two"}},
        .header_height = 4.0f,
        .row_height = 4.0f,
        .cell_padding_x = 3.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 2.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    const Vec2f table_size = ui2::measure(measured_table);
    assert(table_size.x > 20.0f);
    assert(approx(table_size.y, 66.0f));
    assert(rect_eq({0, 0, ui2::measure(ui2::TreeView{}).x, ui2::measure(ui2::TreeView{}).y}, 0, 0, 220, 180));
    ui2::TreeView measured_tree{
        .items = {{.label = "Root", .has_children = true}, {.label = "Child", .depth = 1}},
        .row_height = 4.0f,
        .row_spacing = 2.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(approx(ui2::measure(measured_tree).y, 74.0f));
    const Vec2f color_picker_size = ui2::measure(ui2::ColorPicker{});
    assert(color_picker_size.x >= 260.0f);
    assert(color_picker_size.y >= 240.0f);
    const Vec2f natural_grid = ui2::measure(ui2::IconGrid{.items = {{.text = "A"}, {.text = "B"}, {.text = "C"}}, .cell_size = {20, 20}, .spacing = {4, 4}, .columns = 2});
    assert(natural_grid.x > 44.0f);
    assert(natural_grid.y > 44.0f);
    ui2::ListView tall_list{.items = {"Alpha", "Beta"}, .row_height = 4.0f, .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white}, .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}}};
    assert(ui2::measure(tall_list).y > 50.0f);
    ui2::PropertyGrid grid{.rows = {{.label = "A"}, {.label = "B"}, {.label = "C"}}};
    assert(rect_eq({0, 0, ui2::measure(grid).x, ui2::measure(grid).y}, 0, 0, 268, 92));
    ui2::PropertyGrid tall_grid{.rows = {{.label = "Really Tall"}}, .row_height = 4.0f, .control_height = 4.0f, .label_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white}, .value_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white}, .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}}};
    assert(ui2::measure(tall_grid).y > 28.0f);
}

void test_advanced_editor_widget_measure() {
    assert(approx(ui2::measure(ui2::TabBar{}).x, 240.0f));
    assert(ui2::measure(ui2::TabBar{}).y >= 30.0f);
    assert(rect_eq({0, 0, ui2::measure(ui2::Splitter{}).x, ui2::measure(ui2::Splitter{}).y}, 0, 0, 240, 160));
    assert(rect_eq({0, 0, ui2::measure(ui2::DockPanel{}).x, ui2::measure(ui2::DockPanel{}).y}, 0, 0, 220, 160));
    assert(approx(ui2::measure(ui2::BreadcrumbBar{}).x, 240.0f));
    assert(ui2::measure(ui2::BreadcrumbBar{}).y >= 24.0f);
    ui2::MenuList measured_menu{
        .items = {{.label = "Open", .prompt = "Ctrl+O"}, {.label = "Export", .submenu_id = "export"}},
        .row_height = 4.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(measured_menu).y > measured_menu.row_height * 2.0f + measured_menu.row_spacing);
    ui2::TabBar measured_tabs{
        .items = {{.label = "Scene", .dirty = true, .closable = true}},
        .tab_height = 4.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(measured_tabs).y > 30.0f);
    ui2::DockPanel measured_dock{
        .title = "Inspector",
        .header_height = 4.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(measured_dock).x > 220.0f || approx(ui2::measure(measured_dock).y, 160.0f));
    ui2::BreadcrumbBar measured_breadcrumb{
        .segments = {{.label = "Workspace"}, {.label = "Accounts"}},
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(measured_breadcrumb).y > 28.0f);
    assert(rect_eq({0, 0, ui2::measure(ui2::AssetBrowser{}).x, ui2::measure(ui2::AssetBrowser{}).y}, 0, 0, 320, 220));
    ui2::AssetBrowser measured_assets{
        .items = {{.name = "Segment", .kind = "View"}, {.name = "Dashboard", .kind = "Page"}},
        .mode = ui2::AssetBrowserMode::List,
        .row_height = 4.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(approx(ui2::measure(measured_assets).y, 108.0f));
    assert(approx(ui2::measure(ui2::StatusBar{}).x, 320.0f));
    assert(ui2::measure(ui2::StatusBar{}).y >= 24.0f);
    ui2::StatusBar measured_status{
        .left = {{.text = "Ready"}},
        .right = {{.text = "Line 10"}},
        .warnings = 2,
        .errors = 1,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(ui2::measure(measured_status).y > 24.0f);
    assert(rect_eq({0, 0, ui2::measure(ui2::LogConsole{}).x, ui2::measure(ui2::LogConsole{}).y}, 0, 0, 360, 220));
    ui2::LogConsole measured_log{
        .entries = {{.timestamp = "09:14", .text = "Ready"}, {.timestamp = "09:15", .text = "Still ready"}},
        .row_height = 4.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(approx(ui2::measure(measured_log).y, 116.0f));
    assert(rect_eq({0, 0, ui2::measure(ui2::PropertyInspector{}).x, ui2::measure(ui2::PropertyInspector{}).y}, 0, 0, 320, 240));
    ui2::PropertyInspector measured_inspector{
        .rows = {
            {.kind = ui2::PropertyInspectorRowKind::Section, .label = "Account"},
            {.kind = ui2::PropertyInspectorRowKind::Text, .label = "Name", .text_value = "Northstar"},
            {.kind = ui2::PropertyInspectorRowKind::Bool, .label = "Enabled"},
        },
        .row_height = 4.0f,
        .section_height = 4.0f,
        .row_spacing = 2.0f,
        .control_height = 4.0f,
        .label_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .value_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };
    assert(approx(ui2::measure(measured_inspector).y, 112.0f));
    assert(rect_eq({0, 0, ui2::measure(ui2::NodeGraph{}).x, ui2::measure(ui2::NodeGraph{}).y}, 0, 0, 480, 320));
}

void test_property_inspector_reports_rows() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::PropertyInspector inspector{
        .id = ui2::make_id("inspector"),
        .bounds = {0, 0, 320, 240},
        .rows = {
            {.kind = ui2::PropertyInspectorRowKind::Section, .string_id = "transform", .label = "Transform"},
            {.kind = ui2::PropertyInspectorRowKind::Bool, .string_id = "visible", .label = "Visible", .bool_value = false},
            {.kind = ui2::PropertyInspectorRowKind::Vector, .string_id = "pos", .label = "Position", .vector_values = {1.0f, 2.0f}, .vector_dimension = 3},
            {.kind = ui2::PropertyInspectorRowKind::Enum, .string_id = "mode", .label = "Mode", .options = {"A", "B"}, .selected = 0},
            {.kind = ui2::PropertyInspectorRowKind::Reference, .string_id = "sprite", .label = "Sprite", .value_text = "hero.png", .reference_type = "asset", .drag_payload_type = "asset", .drag_payload_text = "hero.png", .drag_payload_value = 7},
            {.kind = ui2::PropertyInspectorRowKind::Button, .string_id = "apply", .label = "Apply", .value_text = "Apply"},
            {.kind = ui2::PropertyInspectorRowKind::Label, .string_id = "locked", .label = "Locked", .value_text = "Nope", .read_only = true},
        },
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, inspector);
        ui.end();
    };

    input.begin_frame();
    frame();
    assert(inspector.rows[2].vector_values.size() == 3);
    assert(inspector.visible > 0);

    input.begin_frame();
    input.set_mouse_pos({20, 14});
    frame();
    input.begin_frame();
    input.set_mouse_pos({20, 14});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({20, 14});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(inspector.toggled_section_id == "transform");

    input.begin_frame();
    input.set_mouse_pos({150, 42});
    frame();
    input.begin_frame();
    input.set_mouse_pos({150, 42});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({150, 42});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(inspector.changed);
    assert(inspector.changed_id == "visible");
    assert(inspector.rows[1].bool_value);

    input.begin_frame();
    input.set_mouse_pos({150, 140});
    frame();
    input.begin_frame();
    input.set_mouse_pos({150, 140});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({150, 140});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(inspector.reference_pick_requested_id == "sprite");

    input.begin_frame();
    input.set_mouse_pos({150, 172});
    frame();
    input.begin_frame();
    input.set_mouse_pos({150, 172});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({150, 172});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(inspector.action_requested_id == "apply");
}

void test_node_graph_geometry_connection_and_arrows() {
    Input input;
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    ui2::Context ui;
    ui2::NodeGraph graph{
        .id = ui2::make_id("graph"),
        .bounds = {0, 0, 360, 220},
        .nodes = {
            {.id = "a", .title = "A", .position = {20, 40}, .outputs = {{.id = "out", .label = "Out", .kind = ui2::NodeGraphPortKind::Output, .type = "float"}}},
            {.id = "b", .title = "B", .position = {210, 60}, .inputs = {{.id = "in", .label = "In", .kind = ui2::NodeGraphPortKind::Input, .type = "float"}}},
            {.id = "c", .title = "C", .position = {210, 150}, .inputs = {{.id = "bad", .label = "Bad", .kind = ui2::NodeGraphPortKind::Input, .type = "color"}}},
        },
        .edges = {{.id = "edge", .from_node = "a", .from_port = "out", .to_node = "b", .to_port = "in"}},
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, graph);
        ui.end();
    };

    input.begin_frame();
    frame();
    assert(rect_eq(graph.nodes[0].rect, 20, 40, 140, 96));
    assert(rect_eq(graph.nodes[0].outputs[0].rect, 155, 72, 10, 10));
    assert(!draws.empty());

    const Vec2f out = {160, 77};
    const Vec2f in = {210, 97};
    input.begin_frame();
    input.set_mouse_pos(out);
    frame();
    input.begin_frame();
    input.set_mouse_pos(out);
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos(in);
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos(in);
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(graph.connect_requested);
    assert(graph.connect_from_node_id == "a");
    assert(graph.connect_to_node_id == "b");

    input.begin_frame();
    input.set_mouse_pos({40, 52});
    frame();
    input.begin_frame();
    input.set_mouse_pos({40, 52});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    input.begin_frame();
    input.set_mouse_pos({60, 72});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(graph.moved_node_id == "a");
    assert(approx(graph.move_delta.x, 20));
    assert(approx(graph.move_delta.y, 20));

    input.begin_frame();
    input.set_mouse_pos({180, 87});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(graph.selected_edge_id == "edge");

    const Vec2f before_pan = graph.pan;
    input.begin_frame();
    input.set_mouse_pos({300, 20});
    input.set_mouse_wheel_y(20.0f);
    frame();
    assert(graph.zoom <= graph.max_zoom);
    assert(graph.zoom_changed);
    assert(graph.pan != before_pan);
}

void test_tab_bar_splitter_and_dock_panel() {
    Input input;
    input.bind("menu_right", Key::Right);
    input.bind("accept", Key::Enter);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;

    ui2::TabBar tabs{
        .id = ui2::make_id("tabs"),
        .bounds = {0, 0, 220, 30},
        .items = {{.id = "a", .label = "A"}, {.id = "b", .label = "B", .dirty = true, .closable = true}, {.id = "c", .label = "C", .enabled = false}},
    };

    input.begin_frame();
    input.set_action_pressed("menu_right");
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    ui2::run(ui, tabs);
    ui.end();
    assert(tabs.changed);
    assert(tabs.selected == 1);
    assert(tabs.activated == 1);
    assert(tabs.activated_id == "b");

    ui2::TabBar wrapped_tabs{
        .id = ui2::make_id("wrapped-tabs"),
        .bounds = {0, 0, 150, 0},
        .items = {{.id = "segments", .label = "Segments"}, {.id = "dashboard", .label = "Dashboard"}, {.id = "copy", .label = "Email Copy"}},
        .wrap = true,
    };
    const Vec2f wrapped_size = ui2::measure(wrapped_tabs);
    assert(wrapped_size.y > ui2::measure(ui2::TabBar{.items = {{.label = "Segments"}}, .wrap = true}).y);
    wrapped_tabs.bounds.h = wrapped_size.y;
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, wrapped_tabs);
    ui.end();
    assert(wrapped_tabs.selected == 0);

    ui2::Splitter splitter{
        .id = ui2::make_id("splitter"),
        .bounds = {0, 0, 200, 100},
        .ratio = 0.25f,
        .min_first = 60.0f,
        .min_second = 80.0f,
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, splitter);
    ui.end();
    assert(splitter.first.w >= 60.0f);
    assert(splitter.second.w >= 80.0f);
    assert(approx(splitter.handle.w, splitter.thickness));

    std::vector<Rectf> splitter_draws;
    Renderer2D splitter_renderer = make_recording_renderer(splitter_draws);
    input.begin_frame();
    ui.begin(input, splitter_renderer);
    ui2::run(ui, splitter);
    ui.end();
    assert(splitter_draws.size() == 1);
    assert(approx(splitter_draws.front().w, 2.0f));
    assert(approx(splitter_draws.front().h, splitter.bounds.h));
    assert(approx(splitter_draws.front().x + splitter_draws.front().w * 0.5f,
                  splitter.handle.x + splitter.handle.w * 0.5f));

    ui2::DockPanel dock{
        .id = ui2::make_id("dock"),
        .bounds = {0, 0, 160, 100},
        .title = "Inspector",
    };
    input.begin_frame();
    input.set_mouse_pos({20, 10});
    ui.begin(input, renderer);
    ui2::run(ui, dock);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({20, 10});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::run(ui, dock);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({20, 10});
    input.set_mouse_held(MouseButton::Left, false);
    ui.begin(input, renderer);
    ui2::run(ui, dock);
    ui.end();
    assert(dock.toggled);
    assert(rect_eq(dock.header, 0, 0, 160, 28));
    assert(rect_eq(dock.body, 0, 28, 160, 72));
}

void test_tab_bar_fits_tight_tabs_inside_chrome_inset() {
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    const ui2::Theme theme = ui2::game_theme(ui2::PalettePreset::Default);
    ui.set_theme(theme);

    ui2::TabBar tabs{
        .id = ui2::make_id("tight-tabs"),
        .bounds = {0, 0, 146, 30},
        .items = {{.id = "quest", .label = "Q", .dirty = true}, {.id = "map", .label = "M"}},
        .text_style = theme.small_text,
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, tabs);
    ui.end();

    bool saw_second_tab_inside_bounds = false;
    for (Rectf draw : draws) {
        if (draw.y > 0.0f && draw.h <= tabs.bounds.h && draw.x > tabs.bounds.x && draw.x + draw.w <= tabs.bounds.x + tabs.bounds.w) {
            saw_second_tab_inside_bounds = saw_second_tab_inside_bounds || draw.x > 60.0f;
        }
    }
    assert(saw_second_tab_inside_bounds);
    assert(tabs.offset == 0.0f);
}

void test_breadcrumb_asset_status_and_log_widgets() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;

    ui2::BreadcrumbBar crumbs{
        .id = ui2::make_id("crumbs"),
        .bounds = {0, 0, 90, 28},
        .segments = {{.id = "root", .label = "Root"}, {.id = "assets", .label = "Assets"}, {.id = "textures", .label = "Textures"}},
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, crumbs);
    ui.end();
    assert(crumbs.overflowed);

    ui2::AssetBrowser assets{
        .id = ui2::make_id("assets"),
        .bounds = {0, 0, 220, 120},
        .items = {{.id = "a", .name = "A", .kind = "Texture", .modified = "Today"}, {.id = "b", .name = "B", .kind = "Scene", .modified = "Yesterday"}},
        .mode = ui2::AssetBrowserMode::List,
    };
    input.begin_frame();
    input.set_mouse_pos({10, 12});
    ui.begin(input, renderer);
    ui2::run(ui, assets);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({10, 12});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::run(ui, assets);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({10, 12});
    input.set_mouse_held(MouseButton::Left, false);
    ui.begin(input, renderer);
    ui2::run(ui, assets);
    ui.end();
    assert(assets.sort_requested == ui2::AssetSortField::Name);

    input.begin_frame();
    input.set_mouse_pos({10, 36});
    ui.begin(input, renderer);
    ui2::run(ui, assets);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({10, 36});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::run(ui, assets);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({10, 36});
    input.set_mouse_held(MouseButton::Left, false);
    ui.begin(input, renderer);
    ui2::run(ui, assets);
    ui.end();
    assert(assets.clicked == 0);
    assert(assets.activated_id == "a");

    ui2::StatusBar status{
        .id = ui2::make_id("status"),
        .bounds = {0, 0, 240, 24},
        .left = {{.id = "ready", .text = "Ready"}},
        .progress = 0.5f,
        .warnings = 1,
        .errors = 2,
    };
    input.begin_frame();
    input.set_mouse_pos({10, 12});
    ui.begin(input, renderer);
    ui2::run(ui, status);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({10, 12});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::run(ui, status);
    ui.end();
    input.begin_frame();
    input.set_mouse_pos({10, 12});
    input.set_mouse_held(MouseButton::Left, false);
    ui.begin(input, renderer);
    ui2::run(ui, status);
    ui.end();
    assert(status.clicked_id == "ready");

    ui2::LogConsole log{
        .id = ui2::make_id("log"),
        .bounds = {0, 0, 260, 90},
        .entries = {{.id = "ready", .text = "Ready"}, {.id = "oops", .timestamp = "12:00", .text = "Oops", .severity = ui2::LogSeverity::Error}},
        .filter = "oops",
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, log);
    ui.end();
    assert(log.visible == 1);
    assert(log.offset >= 0.0f);
}

void test_list_view_keyboard_and_click() {
    Input input;
    input.bind("menu_down", Key::Down);
    input.bind("accept", Key::Enter);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::ListView list{.id = ui2::make_id("list"), .bounds = {0, 0, 100, 140}, .items = {"A", "B", "C"}};

    input.begin_frame();
    input.set_action_pressed("menu_down");
    ui.begin(input, renderer);
    ui2::run(ui, list);
    ui.end();
    assert(list.changed);
    assert(list.selected == 1);

    const f32 row_h = ui2::measure(list).y / 3.0f;
    const Vec2f third_row{10.0f, row_h * 2.0f + row_h * 0.5f};

    input.begin_frame();
    input.set_mouse_pos(third_row);
    ui.begin(input, renderer);
    ui2::run(ui, list);
    ui.end();

    input.begin_frame();
    input.set_mouse_pos(third_row);
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::run(ui, list);
    ui.end();

    input.begin_frame();
    input.set_mouse_pos(third_row);
    input.set_mouse_held(MouseButton::Left, false);
    ui.begin(input, renderer);
    ui2::run(ui, list);
    ui.end();
    assert(list.activated);
    assert(list.selected == 2);
}

void test_list_view_drag_drop_reports_rows() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::ListView list{
        .id = ui2::make_id("drag-list"),
        .bounds = {0, 0, 120, 140},
        .items = {"A", "B", "C"},
        .drag_enabled = true,
        .drop_enabled = true,
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, list);
        ui.end();
    };
    const f32 row_h = ui2::measure(list).y / 3.0f;
    const Vec2f first_row{10.0f, row_h * 0.5f};
    const Vec2f third_row{10.0f, row_h * 2.0f + row_h * 0.5f};

    input.begin_frame();
    input.set_mouse_pos(first_row);
    frame();

    input.begin_frame();
    input.set_mouse_pos(first_row);
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(!list.drag_started);

    input.begin_frame();
    input.set_mouse_pos(third_row);
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(list.drag_started);
    assert(list.drag_source == 0);
    assert(list.dragging == 0);
    assert(list.drop_target == 2);

    input.begin_frame();
    input.set_mouse_pos(third_row);
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(list.dropped);
    assert(list.dropped_source == 0);
    assert(list.dropped_target == 2);
    assert(!list.activated);
}

void test_table_and_tree_view_render_state() {
    Input input;
    input.bind("menu_down", Key::Down);
    input.bind("accept", Key::Enter);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;

    ui2::Table table{
        .bounds = {0, 0, 160, 80},
        .columns = {{.label = "Name", .width = 80}, {.label = "Value", .sizing = ui2::UiTableColumnSizing::Stretch}},
        .rows = {{"A", "1"}, {"B", "2"}, {"C", "3"}},
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, table);
    ui.end();
    assert(table.first == 0);
    assert(table.visible > 0);

    ui2::TreeView tree{
        .id = ui2::make_id("tree"),
        .bounds = {0, 0, 140, 80},
        .items = {{.id = "root", .label = "Root", .has_children = true}, {.id = "child", .label = "Child", .depth = 1}},
    };
    input.begin_frame();
    input.set_action_pressed("menu_down");
    ui.begin(input, renderer);
    ui2::run(ui, tree);
    ui.end();
    assert(tree.changed);
    assert(tree.selected == 1);

    input.begin_frame();
    input.set_action_pressed("accept");
    ui.begin(input, renderer);
    ui2::run(ui, tree);
    ui.end();
    assert(tree.activated == 1);
}

void test_table_uses_natural_row_height_for_tall_text() {
    Input input;
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    ui2::Context ui;

    ui2::Table table{
        .bounds = {0, 0, 120, 70},
        .columns = {{.label = "A", .width = 60}, {.label = "B", .sizing = ui2::UiTableColumnSizing::Stretch}},
        .rows = {{"A", "1"}, {"B", "2"}, {"C", "3"}},
        .header_height = 4.0f,
        .row_height = 4.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 4.0f, .color = colors::white},
        .style = {.padding = {0.0f, 4.0f, 0.0f, 4.0f}},
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, table);
    ui.end();

    assert(table.first == 0);
    assert(table.visible == 2);
    const f32 natural_h = ui2::measure_text(ui2::bitmap_font(), "Mg", 4.0f).y + 8.0f;
    assert(approx(natural_h, 36.0f));
}

void test_table_respects_larger_explicit_row_height() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;

    ui2::Table table{
        .bounds = {0, 0, 120, 130},
        .columns = {{.label = "A", .width = 60}},
        .rows = {{"A"}, {"B"}, {"C"}},
        .header_height = 30.0f,
        .row_height = 40.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 1.0f, .color = colors::white},
        .style = {.padding = {0.0f, 2.0f, 0.0f, 2.0f}},
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, table);
    ui.end();

    assert(table.visible == 3);
}

void test_table_without_header_uses_body_from_top() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;

    ui2::Table table{
        .bounds = {0, 0, 120, 35},
        .columns = {{.label = "A", .width = 60}},
        .rows = {{"A"}, {"B"}},
        .header_height = 50.0f,
        .row_height = 4.0f,
        .draw_header = false,
        .text_style = {.font = ui2::bitmap_font(), .scale = 2.0f, .color = colors::white},
        .style = {.padding = {0.0f, 3.0f, 0.0f, 3.0f}},
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, table);
    ui.end();

    assert(table.first == 0);
    assert(table.visible == 2);
}

void test_table_frame_uses_flush_outline_and_rounded_header() {
    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    std::vector<Rectf> rounded_outline_rects;
    std::vector<Rectf> clips;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_geometry_recording_renderer(fills, colors, rounded_fills, rounded_outlines, rounded_outline_rects, clips);
    ui2::Context ui;
    ui2::Theme theme = ui2::game_theme(ui2::PalettePreset::Slate);

    ui2::Table table{
        .bounds = {10, 20, 160, 80},
        .columns = {{.label = "Name", .width = 80}},
        .rows = {{"A"}},
        .header_height = 40.0f,
        .row_height = 20.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 1.0f, .color = colors::white},
    };

    input.begin_frame();
    ui.set_theme(theme);
    ui.begin(input, renderer);
    ui2::run(ui, table);
    ui.end();

    assert(!rounded_outline_rects.empty());
    assert(rect_eq(rounded_outline_rects.back(), 10, 20, 160, 80));
    const f32 bw = theme.preset.border_width; // header content is inset by the frame border
    const f32 radius = theme.preset.radius;   // header fill extends under the rounded corners
    assert(std::any_of(clips.begin(), clips.end(), [=](Rectf rect) { return rect_eq(rect, 10.0f + bw, 20.0f + bw, 160.0f - 2.0f * bw, 40.0f); }));
    assert(std::any_of(fills.begin(), fills.end(), [=](Rectf rect) { return rect_eq(rect, 10.0f + bw, 20.0f + bw, 160.0f - 2.0f * bw, 40.0f + radius - bw); }));
}

void test_table_square_header_and_bottom_stripe_trim() {
    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;

    ui2::Table square_table{
        .bounds = {0, 0, 100, 70},
        .columns = {{.label = "Name", .width = 80}},
        .rows = {{"A"}},
        .header_height = 24.0f,
        .row_height = 20.0f,
        .text_style = {.font = ui2::bitmap_font(), .scale = 1.0f, .color = colors::white},
    };

    input.begin_frame();
    ui.set_theme(ui2::editor_theme(ui2::PalettePreset::Slate));
    ui.begin(input, renderer);
    ui2::run(ui, square_table);
    ui.end();

    assert(rounded_fills == 0);
    assert(std::any_of(fills.begin(), fills.end(), [](Rectf rect) { return rect_eq(rect, 1, 1, 98, 24); }));

    fills.clear();
    colors.clear();
    rounded_fills = 0;
    rounded_outlines = 0;
    ui2::Theme rounded_theme = ui2::game_theme(ui2::PalettePreset::Slate);
    ui2::Table rounded_table{
        .bounds = {10, 20, 160, 124},
        .columns = {{.label = "Name", .width = 80}},
        .rows = {{"A"}, {"B"}, {"C"}},
        .header_height = 0.0f,
        .row_height = 40.0f,
        .draw_header = false,
        .text_style = {.font = ui2::bitmap_font(), .scale = 1.0f, .color = colors::white},
    };

    input.begin_frame();
    ui.set_theme(rounded_theme);
    ui.begin(input, renderer);
    ui2::run(ui, rounded_table);
    ui.end();

    // Semantic checks (exact px varies with border width/radius): the last row
    // stripe is trimmed short of the rounded frame, and the remaining sliver is
    // painted at radius inset — never full width into the corners.
    const f32 bw = rounded_theme.preset.border_width;
    const f32 radius = rounded_theme.preset.radius;
    const f32 inner_w = 160.0f - 2.0f * bw;
    assert(std::any_of(fills.begin(), fills.end(), [=](Rectf rect) {
        return rect.x == 10.0f + bw && rect.w == inner_w && rect.y >= 100.0f && rect.y <= 100.0f + bw && rect.h < 40.0f;
    }));
    assert(std::any_of(fills.begin(), fills.end(), [=](Rectf rect) {
        return rect.x == 10.0f + radius && rect.w == 160.0f - 2.0f * radius && rect.h > 0.0f && rect.h <= 4.0f && rect.y >= 134.0f;
    }));
    assert(std::none_of(fills.begin(), fills.end(), [=](Rectf rect) {
        return rect.x == 10.0f + bw && rect.w == inner_w && rect.y >= 134.0f && rect.h <= 4.0f;
    }));
}

void test_tree_view_drag_drop_reports_items() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::TreeView tree{
        .id = ui2::make_id("drag-tree"),
        .bounds = {0, 0, 160, 90},
        .items = {{.id = "root", .label = "Root", .has_children = true}, {.id = "child-a", .label = "A", .depth = 1}, {.id = "child-b", .label = "B", .depth = 1}},
        .drag_enabled = true,
        .drop_enabled = true,
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, tree);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({20, 35});
    frame();

    input.begin_frame();
    input.set_mouse_pos({20, 35});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    input.begin_frame();
    input.set_mouse_pos({20, 60});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(tree.drag_started);
    assert(tree.drag_source == 1);
    assert(tree.dragging == 1);
    assert(tree.drop_target == 2);

    input.begin_frame();
    input.set_mouse_pos({20, 60});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(tree.dropped);
    assert(tree.dropped_source == 1);
    assert(tree.dropped_target == 2);
    assert(tree.activated == -1);
}

void test_property_grid_bool_and_number_interaction() {
    Input input;
    input.bind("menu_right", Key::Right);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::PropertyGrid grid{
        .id = ui2::make_id("props"),
        .bounds = {0, 0, 260, 80},
        .rows = {
            {.kind = ui2::PropertyRowKind::Bool, .id = ui2::make_id("enabled"), .label = "Enabled"},
            {.kind = ui2::PropertyRowKind::Number,
             .id = ui2::make_id("speed"),
             .label = "Speed",
             .number = {.value = 1.0f, .min = 0.0f, .max = 10.0f, .step = 0.5f}},
        },
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, grid);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({140, 12});
    frame();

    input.begin_frame();
    input.set_mouse_pos({140, 12});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    input.begin_frame();
    input.set_mouse_pos({140, 12});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(grid.changed);
    assert(grid.rows[0].changed);
    assert(grid.rows[0].bool_value);

    input.begin_frame();
    input.set_action_pressed("menu_right");
    ui.begin(input, renderer);
    ui.state().set_focused(ui2::make_id("speed"));
    ui2::run(ui, grid);
    ui.end();
    assert(grid.changed);
    assert(grid.rows[1].changed);
    assert(approx(grid.rows[1].number.value, 1.5f));
}

void test_property_grid_zero_row_height_uses_natural_layout() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::PropertyGrid grid{
        .id = ui2::make_id("props-invalid"),
        .bounds = {0, 0, 260, 80},
        .rows = {{.label = "Bad"}},
        .row_height = 0.0f,
        .row_spacing = 0.0f,
    };

    ui.begin(input, renderer);
    ui2::run(ui, grid);
    ui.end();

    assert(grid.first == 0);
    assert(grid.visible == 1);
}

void test_property_grid_color_row_click_and_geometry() {
    Input input;
    std::vector<Rectf> fills;
    Renderer2D renderer = make_recording_renderer(fills);
    ui2::Context ui;
    ui2::PropertyGrid grid{
        .id = ui2::make_id("props-color"),
        .bounds = {0, 0, 260, 32},
        .rows = {
            {.kind = ui2::PropertyRowKind::Color, .id = ui2::make_id("tint"), .label = "Tint", .color_value = Color::rgba(16, 32, 48, 128)},
        },
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, grid);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({140, 14});
    frame();

    input.begin_frame();
    input.set_mouse_pos({140, 14});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    input.begin_frame();
    input.set_mouse_pos({140, 14});
    input.set_mouse_held(MouseButton::Left, false);
    frame();

    assert(grid.rows[0].clicked);
    assert(!grid.rows[0].changed);
    assert(!grid.changed);
    bool saw_swatch = false;
    for (Rectf fill : fills) {
        saw_swatch = saw_swatch ||
                     (fill.x >= 128.0f && fill.x < 170.0f &&
                      fill.y >= 0.0f && fill.y <= 16.0f &&
                      approx(fill.w, fill.h) && fill.w >= 12.0f && fill.w <= 32.0f);
    }
    assert(saw_swatch);
}

void test_property_grid_color_row_popup_edits_color() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::PropertyGrid grid{
        .id = ui2::make_id("props-color-edit"),
        .bounds = {0, 0, 260, 32},
        .rows = {
            {.kind = ui2::PropertyRowKind::Color, .id = ui2::make_id("tint-edit"), .label = "Tint", .color_value = Color::rgba(0, 32, 48, 255), .color_picker = {.mode = ui2::ColorPickerMode::Rgba}},
        },
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, grid);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({140, 14});
    frame();

    input.begin_frame();
    input.set_mouse_pos({140, 14});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    input.begin_frame();
    input.set_mouse_pos({140, 14});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(grid.rows[0].clicked);

    input.begin_frame();
    input.set_mouse_pos({224, 72});
    frame();

    input.begin_frame();
    input.set_mouse_pos({224, 72});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    assert(grid.rows[0].changed);
    assert(grid.changed);
    assert(grid.rows[0].color_value.r > 0);
    assert(grid.rows[0].color_value.g == 32);
    assert(grid.rows[0].color_value.b == 48);
}

// --- region(): hot / active / click cycle across frames ------------------------------

void test_region_click_cycle() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id id = ui2::make_id("btn");
    const Rectf bounds{0, 0, 100, 30};

    // Frame 1: pointer over region, not yet hot (resolved next frame).
    input.begin_frame();
    input.set_mouse_pos({50, 15});
    ui.begin(input, renderer);
    ui2::Interaction f1 = ui.region(id, bounds);
    ui.end();
    assert(!f1.hot && !f1.active && !f1.clicked);

    // Frame 2: press begins; now hot and active.
    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, true); // prev=false -> pressed
    ui.begin(input, renderer);
    ui2::Interaction f2 = ui.region(id, bounds);
    ui.end();
    assert(f2.hot && f2.pressed && f2.active && !f2.clicked);

    // Frame 3: release over region -> click.
    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, false); // prev=true -> released
    ui.begin(input, renderer);
    ui2::Interaction f3 = ui.region(id, bounds);
    ui.end();
    assert(f3.released && f3.clicked);
}

// --- occlusion: topmost wins, no click-through ---------------------------------------

void test_region_occlusion() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id bottom = ui2::make_id("bottom");
    const ui2::Id top = ui2::make_id("top");
    const Rectf bounds{0, 0, 100, 100};

    // Frame 1: both regions overlap the pointer; top has higher z.
    input.begin_frame();
    input.set_mouse_pos({50, 50});
    ui.begin(input, renderer);
    ui.region(bottom, bounds, 0);
    ui.region(top, bounds, 1);
    ui.end();

    // Frame 2: press. Only the topmost becomes hot/active; bottom is occluded.
    input.begin_frame();
    input.set_mouse_pos({50, 50});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::Interaction b = ui.region(bottom, bounds, 0);
    ui2::Interaction t = ui.region(top, bounds, 1);
    ui.end();
    assert(t.hot && t.active);
    assert(!b.hot && !b.active && !b.pressed); // no click-through to the region below
}

void test_popup_clamps_and_closes_on_outside_click() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id popup = ui2::make_id("popup");

    input.begin_frame();
    ui.begin(input, renderer);
    ui.open_popup(popup);
    ui2::Context::PopupResult first = ui.begin_popup(popup,
                                                     {620, 340, 20, 20},
                                                     {.size = {100, 80}, .offset = {}, .screen = {0, 0, 640, 360}});
    ui.end_popup();
    ui.end();
    assert(first.open);
    assert(first.opened);
    assert(rect_eq(first.bounds, 540, 280, 100, 80));
    assert(ui.popup_open(popup));

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, true);
    ui.begin(input, renderer);
    ui2::Context::PopupResult second = ui.begin_popup(popup,
                                                      {620, 340, 20, 20},
                                                      {.size = {100, 80}, .offset = {}, .screen = {0, 0, 640, 360}});
    ui.end();
    assert(!second.open);
    assert(second.closed);
    assert(second.outside_clicked);
    assert(!ui.popup_open(popup));
}

void test_modal_blocks_background_and_cancels() {
    Input input;
    input.bind("quit", Key::Escape);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id modal = ui2::make_id("modal");
    const ui2::Id background = ui2::make_id("background");

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    ui.begin(input, renderer);
    ui.open_modal(modal);
    ui2::Context::ModalResult first = ui.begin_modal(modal, {40, 40, 80, 80});
    ui2::Interaction blocked = ui.region(background, {0, 0, 20, 20});
    ui.end_modal();
    ui.end();
    assert(first.open);
    assert(first.opened);
    assert(!blocked.hot && !blocked.pressed);

    input.begin_frame();
    input.set_action_pressed("quit");
    ui.begin(input, renderer);
    ui2::Context::ModalResult second = ui.begin_modal(modal, {40, 40, 80, 80});
    ui.end();
    assert(second.closed);
    assert(second.cancelled);
    assert(!ui.modal_open(modal));
}

void test_prompt_resolver_and_prompt_label_action() {
    Input input;
    input.bind("accept", {Key::Enter, Key::Space});
    input.bind("fire", MouseButton::Left);
    input.bind("inspect", MouseButton::Right);
    input.bind("copy", Key::C, KeyModifiers::Ctrl);
    input.bind("jump", Key::Space, KeyModifiers::Shift);
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::PromptLabel prompt{.bounds = {0, 0, 160, 24}, .action = "accept", .text = "START"};

    input.begin_frame();
    ui.begin(input, renderer);
    assert(ui.prompt_for_action("accept") == "Enter");
    assert(ui.prompt_for_action("accept", {.mode = ui2::PromptBindingMode::All}) == "Enter / Space");
    assert(ui.prompt_for_action("fire") == "Mouse Left");
    assert(ui.prompt_for_action("inspect") == "Mouse Right");
    assert(ui.prompt_for_action("copy") == "Ctrl+C");
    assert(ui.prompt_for_action("jump") == "Shift+Space");
    assert(ui.prompt_for_action("quit", {.fallback = "Esc"}) == "Esc");
    assert(ui.prompt_for_action("missing") == "missing");
    ui2::run(ui, prompt);
    ui.end();
    assert(prompt.prompt == "Enter");
}

void test_prompt_label_chip_and_prompt_row() {
    Input input;
    input.bind("accept", Key::Space);
    input.bind("quit", Key::Escape);
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    ui2::Context ui;
    ui2::PromptLabel label{.bounds = {0, 0, 160, 28}, .prompt = "Space", .text = "Start"};

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, label);
    ui.end();
    assert(!draws.empty());

    ui2::PromptRow row{
        .bounds = {0, 0, 240, 40},
        .items = {{.action = "accept", .text = "Start"}, {.action = "quit", .text = "Back"}},
        .prompt_options = {.brackets = true},
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, row);
    ui.end();
    assert(ui2::measure(row).x > ui2::measure(label).x);

    row.axis = ui2::UiLayoutAxis::Vertical;
    const Vec2f vertical = ui2::measure(row);
    row.axis = ui2::UiLayoutAxis::Horizontal;
    const Vec2f horizontal = ui2::measure(row);
    assert(vertical.y > horizontal.y);
}

void test_world_overlay_projection_helpers() {
    Camera2D camera;
    camera.offset = {10, 20};
    camera.set_viewport({100, 80});
    ui2::WorldOverlayContext overlay{.camera = &camera, .viewport = {5, 6, 100, 80}};

    const Vec2f screen = ui2::world_to_ui(overlay, {30, 50});
    assert(approx(screen.x, 25));
    assert(approx(screen.y, 36));
    assert(rect_eq(ui2::world_anchor_rect(overlay, {30, 50}, {20, 10}), 15, 26, 20, 10));
    assert(ui2::world_visible(overlay, {20, 30, 5, 5}));
    assert(rect_eq(ui2::clamp_to_viewport(overlay, {90, 70, 20, 20}), 85, 66, 20, 20));
}

void test_tooltip_delay_and_bounds() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id tip = ui2::make_id("tip");
    ui2::Context::TooltipOptions options;
    options.delay_frames = 2;
    options.offset = {4, 4};

    ui2::Context::TooltipResult result;
    for (i32 frame = 0; frame < 3; ++frame) {
        input.begin_frame();
        input.set_mouse_pos({10, 10});
        ui.begin(input, renderer);
        result = ui.tooltip(tip, {0, 0, 30, 30}, "Hello", options);
        ui.end();
    }

    assert(result.visible);
    assert(result.bounds.x >= 14.0f);
    assert(result.bounds.y >= 14.0f);
    assert(result.bounds.w > 0.0f);
    assert(result.bounds.h > 0.0f);
}

void test_tooltip_warm_up() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id first = ui2::make_id("first");
    const ui2::Id second = ui2::make_id("second");
    ui2::Context::TooltipOptions options;
    options.delay_frames = 5;
    options.warm_frames = 3;
    const auto frame = [&](Vec2f pointer, ui2::Id id, Rectf anchor) {
        input.begin_frame();
        input.set_mouse_pos(pointer);
        ui.begin(input, renderer);
        const ui2::Context::TooltipResult result = ui.tooltip(id, anchor, "tip", options);
        ui.end();
        return result.visible;
    };
    const Rectf a{0, 0, 30, 30};
    const Rectf b{40, 0, 30, 30};

    // The first tooltip waits for its delay.
    bool shown = false;
    int frames = 0;
    while (!shown && frames < 20) {
        shown = frame({10, 10}, first, a);
        ++frames;
    }
    assert(shown && frames > 5);
    // Moving straight to the next control shows its tooltip at once.
    assert(frame({50, 10}, second, b));
    // After the pointer rests elsewhere past warm_frames, the delay is back.
    for (int i = 0; i < 4; ++i) {
        frame({200, 200}, second, b);
    }
    assert(!frame({10, 10}, first, a));
}

void test_drag_source_and_drop_target() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    const ui2::Id source = ui2::make_id("source");
    const ui2::Id target = ui2::make_id("target");
    ui2::Context::DragSourceResult source_result;
    ui2::Context::DropTargetResult target_result;

    const auto frame = [&]() {
        ui.begin(input, renderer);
        source_result = ui.drag_source(source, {0, 0, 40, 40}, {.type = "entity", .text = "Player", .value = 42});
        target_result = ui.drop_target(target, {100, 0, 60, 60}, "entity");
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    frame();

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(!source_result.dragging);

    input.begin_frame();
    input.set_mouse_pos({110, 10});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(source_result.started);
    assert(source_result.dragging);
    assert(ui.dragging());

    input.begin_frame();
    input.set_mouse_pos({110, 10});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(target_result.hot);
    assert(target_result.accepts);
    assert(target_result.dropped);
    assert(target_result.payload.text == "Player");
    assert(target_result.payload.value == 42);
}

void test_minimal_feedback_widgets() {
    assert(rect_eq({0, 0, ui2::measure(ui2::ToastStack{.items = {{.text = "A"}, {.text = "B"}}, .item_size = {120, 20}, .spacing = 5}).x,
                    ui2::measure(ui2::ToastStack{.items = {{.text = "A"}, {.text = "B"}}, .item_size = {120, 20}, .spacing = 5}).y},
                   0,
                   0,
                   120,
                   45));

    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    ui2::ToastStack toasts{
        .bounds = {0, 0, 300, 100},
        .items = {{.text = "old", .age = 3.0f, .lifetime = 3.0f}, {.text = "new", .age = 0.5f, .lifetime = 3.0f}},
        .item_size = {120, 20},
        .spacing = 5,
        .max_visible = 4,
        .fade = false,
    };

    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, toasts);
    ui.end();
    assert(toasts.visible == 1);
    assert(!draws.empty());

    ui2::FloatingText floating{
        .position = {10, 20},
        .velocity = {5, -10},
        .text = "10",
        .age = 0.5f,
        .lifetime = 2.0f,
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, floating);
    ui.end();
    assert(approx(floating.rendered_position.x, 12.5f));
    assert(approx(floating.rendered_position.y, 15.0f));
    assert(approx(floating.age, 0.5f));

    ui2::AnimatedValue value{.value = 100.0f, .display = 0.0f, .speed = 40.0f, .prefix = "HP ", .decimals = 0};
    assert(ui2::step_animated_value(value, 0.5f));
    assert(approx(value.display, 20.0f));
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, value);
    ui.end();
    assert(value.rendered_text == "HP 20");
}

void test_advanced_text_parse_layout_and_draw() {
    const Sprite coin = make_sprite({8, 8});
    ui2::AdvancedTextMarkupOptions markup_options{
        .base_style = {.scale = 1.0f},
        .icon_resolver = [&](std::string_view name) {
            return name == "coin" ? coin : Sprite{};
        },
    };

    ui2::AdvancedTextContent parsed = ui2::parse_advanced_text("A [color=#FF0000][u][link=go]GO[/link][/u][/color] [[x] [icon=coin]",
                                                               markup_options);
    const ui2::TextRun* link = nullptr;
    const ui2::IconRun* icon = nullptr;
    bool saw_escaped = false;
    for (const ui2::AdvancedTextRun& run : parsed.runs) {
        if (const auto* text = std::get_if<ui2::TextRun>(&run)) {
            if (text->id == "go") {
                link = text;
            }
            saw_escaped = saw_escaped || text->text.find("[x]") != std::string::npos;
        } else if (const auto* icon_run = std::get_if<ui2::IconRun>(&run)) {
            icon = icon_run;
        }
    }
    assert(link);
    assert(link->text == "GO");
    assert(link->style.color == Color::rgb(255, 0, 0));
    assert(link->underline);
    assert(link->link);
    assert(saw_escaped);
    assert(icon);
    assert(icon->sprite.valid());
    assert(rect_eq({0, 0, icon->size.x, icon->size.y}, 0, 0, 8, 8));

    ui2::AdvancedTextContent invalid = ui2::parse_advanced_text("[color=#FF0000]plain", markup_options);
    assert(invalid.runs.size() == 1);
    assert(std::get<ui2::TextRun>(invalid.runs[0]).text == "[color=#FF0000]plain");

    ui2::AdvancedTextContent structured;
    structured.runs.push_back(ui2::TextRun{.text = "HP ", .style = {.scale = 1.0f}});
    structured.runs.push_back(ui2::IconRun{.sprite = coin, .size = {8, 8}});
    structured.runs.push_back(ui2::TextRun{.text = "10", .style = {.scale = 1.0f}});
    ui2::AdvancedTextContent equivalent = ui2::parse_advanced_text("HP [icon=coin]10", markup_options);
    const Vec2f structured_size = ui2::measure_advanced_text(structured);
    const Vec2f parsed_size = ui2::measure_advanced_text(equivalent);
    assert(approx(structured_size.x, parsed_size.x));
    assert(approx(structured_size.y, parsed_size.y));

    ui2::AdvancedTextContent wrapped;
    wrapped.runs.push_back(ui2::TextRun{.text = "AAAA BBBB", .style = {.scale = 1.0f}});
    ui2::AdvancedTextLayout layout = ui2::layout_advanced_text(wrapped, {.max_width = 35.0f, .line_spacing = 3.0f});
    assert(layout.lines.size() == 2);
    assert(approx(layout.measured.y, 17.0f)); // 7 + 3 + 7

    ui2::AdvancedTextLayout icon_layout = ui2::layout_advanced_text(structured);
    assert(approx(icon_layout.measured.y, 8.0f));

    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::draw_advanced_text(ui, icon_layout, {0, 0, 100, 20});
    ui.end();
    assert(!draws.empty());
}

void test_advanced_text_link_interaction() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    ui2::AdvancedText text{
        .id = ui2::make_id("advanced"),
        .bounds = {0, 0, 80, 20},
        .markup = "[link=go]GO[/link]",
        .markup_options = {.base_style = {.scale = 1.0f}},
        .layout_options = {.max_width = 80.0f},
    };

    const auto frame = [&]() {
        ui.begin(input, renderer);
        ui2::run(ui, text);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({4, 4});
    frame();

    input.begin_frame();
    input.set_mouse_pos({4, 4});
    frame();
    assert(text.hovered_id == "go");

    input.begin_frame();
    input.set_mouse_pos({4, 4});
    input.set_mouse_held(MouseButton::Left, true);
    frame();

    input.begin_frame();
    input.set_mouse_pos({4, 4});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(text.clicked_id == "go");
    assert(text.clicked_rect.w > 0.0f);
}

void test_advanced_text_polish_features() {
    ui2::AdvancedTextContent long_word;
    long_word.runs.push_back(ui2::TextRun{.text = "AAAA", .style = {.scale = 1.0f}});
    const ui2::AdvancedTextLayout unsplit = ui2::layout_advanced_text(long_word, {.max_width = 12.0f});
    const ui2::AdvancedTextLayout split = ui2::layout_advanced_text(long_word, {.max_width = 12.0f, .break_long_words = true});
    assert(unsplit.lines.size() == 1);
    assert(split.lines.size() > 1);

    ui2::AdvancedTextContent icons;
    icons.runs.push_back(ui2::TextRun{.text = "A", .style = {.scale = 2.0f}});
    icons.runs.push_back(ui2::IconRun{.sprite = make_sprite({4, 4}), .size = {4, 4}, .vertical = ui2::UiAlign::Start});
    icons.runs.push_back(ui2::IconRun{.sprite = make_sprite({4, 4}), .size = {4, 4}, .vertical = ui2::UiAlign::End});
    const ui2::AdvancedTextLayout icon_layout = ui2::layout_advanced_text(icons);
    assert(icon_layout.boxes.size() == 3);
    assert(approx(icon_layout.boxes[1].rect.y, 0.0f));
    assert(approx(icon_layout.boxes[2].rect.y, 10.0f)); // 14px text line - 4px icon

    ui2::AdvancedText cached{
        .bounds = {0, 0, 100, 20},
        .markup = "A",
        .markup_options = {.base_style = {.scale = 1.0f}},
    };
    assert(!cached.parsed_cache_valid);
    const Vec2f first = ui2::measure(cached);
    assert(cached.parsed_cache_valid);
    assert(cached.layout_cache_valid);
    const ui2::AdvancedTextLayout first_layout = cached.cached_layout;
    const Vec2f second = ui2::measure(cached);
    assert(approx(first.x, second.x));
    assert(cached.cached_layout.boxes.size() == first_layout.boxes.size());
    cached.markup = "AA";
    ui2::measure(cached);
    assert(cached.cached_layout.boxes[0].rect.w > first_layout.boxes[0].rect.w);
    assert(cached.cached_markup == "AA");

    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    Input input;
    ui2::Context ui;
    ui2::DialogBox dialog{
        .bounds = {0, 0, 120, 60},
        .title_markup = "[u]T[/u]",
        .body_markup = "B [icon=coin]",
        .markup_options = {
            .base_style = {.scale = 1.0f},
            .icon_resolver = [](std::string_view) {
                return make_sprite({6, 6});
            },
        },
    };
    input.begin_frame();
    ui.begin(input, renderer);
    ui2::run(ui, dialog);
    ui.end();
    assert(!draws.empty());
}

// --- end-to-end: scope builder lays out + auto-sizes + drives interaction ------------

void test_layout_end_to_end() {
    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;

    ui2::Button play{.id = ui2::make_id("play"), .label = "Play"};
    ui2::Button quit{.id = ui2::make_id("quit"), .label = "Quit"};

    const auto build = [&]() {
        ui.begin(input, renderer);
        ui.begin_layout({0, 0, 200, 200});
        ui2::LayoutStyle col;
        col.width = ui2::grow();   // fill root width
        col.height = ui2::fit();   // auto height
        col.padding = {8, 8, 8, 8};
        col.spacing = 6.0f;
        ui.begin_column(col);
        ui.widget(play, {.width = ui2::grow(), .height = ui2::fixed(30)});
        ui.widget(quit, {}); // Fit/Fit -> auto-sized to its label
        ui.end_container();
        ui.end_layout();
        ui.end();
    };

    // The text label is 7*scale tall (bitmap font) + 6+6 padding = 26.
    const f32 quit_h = ui2::measure_text(quit.text_style.font, "Quit", quit.text_style.scale).y + 12.0f;

    // Frame 1: establishes geometry.
    input.begin_frame();
    input.set_mouse_pos({100, 23}); // center of where `play` will land
    build();
    assert(rect_eq(play.bounds, 8, 8, 184, 30)); // grow width fills 200-16, fixed height
    assert(approx(quit.bounds.x, 8));
    assert(approx(quit.bounds.y, 44)); // 8 + 30 + 6 spacing
    assert(approx(quit.bounds.h, quit_h));
    assert(!play.clicked);

    // Frame 2: press over play.
    input.begin_frame();
    input.set_mouse_pos({100, 23});
    input.set_mouse_held(MouseButton::Left, true);
    build();
    assert(play.interaction.hot && play.interaction.active);

    // Frame 3: release -> activation surfaces on the struct.
    input.begin_frame();
    input.set_mouse_pos({100, 23});
    input.set_mouse_held(MouseButton::Left, false);
    build();
    assert(play.clicked);
    assert(!quit.clicked);
}

// --- ECS bridge: component storage feeds the same ui2 run()/region() core ------------

void test_ecs_bridge_button_click() {
    kin::EcsWorld world;
    register_ui2_components(world);

    kin::EcsEntity entity = world.entity("play")
                                .set(ui2::Button{
                                    .bounds = {0, 0, 100, 30},
                                    .label = "Play",
                                })
                                .set(Ui2Layer{.order = 1});

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    const auto render_frame = [&]() {
        ui.begin(input, renderer);
        update_ui2_world(world, ui, scratch);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    render_frame();
    assert(scratch.size() == 1);
    assert(entity.get<ui2::Button>()->id);

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, true);
    render_frame();
    assert(entity.get<ui2::Button>()->interaction.hot);
    assert(entity.get<ui2::Button>()->interaction.active);
    assert(!entity.get<ui2::Button>()->clicked);

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, false);
    render_frame();
    assert(entity.get<ui2::Button>()->clicked);
}

void test_ecs_bridge_layer_occlusion() {
    kin::EcsWorld world;
    register_ui2_components(world);

    kin::EcsEntity bottom = world.entity("bottom")
                                .set(ui2::Button{
                                    .bounds = {0, 0, 100, 100},
                                    .label = "Bottom",
                                })
                                .set(Ui2Layer{.order = 0});
    kin::EcsEntity top = world.entity("top")
                             .set(ui2::Button{
                                 .bounds = {0, 0, 100, 100},
                                 .label = "Top",
                             })
                             .set(Ui2Layer{.order = 5});

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    const auto render_frame = [&]() {
        ui.begin(input, renderer);
        update_ui2_world(world, ui, scratch);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({50, 50});
    render_frame();

    input.begin_frame();
    input.set_mouse_pos({50, 50});
    input.set_mouse_held(MouseButton::Left, true);
    render_frame();

    assert(top.get<ui2::Button>()->interaction.hot);
    assert(top.get<ui2::Button>()->interaction.active);
    assert(!bottom.get<ui2::Button>()->interaction.hot);
    assert(!bottom.get<ui2::Button>()->interaction.active);
}

void test_ecs_layout_matches_solver() {
    kin::EcsWorld world;
    register_ui2_components(world);

    kin::EcsEntity root = world.entity("root").set(Ui2Root{.bounds = {0, 0, 200, 200}});
    ui2::LayoutStyle col;
    col.width = ui2::grow();
    col.height = ui2::fit();
    col.padding = {8, 8, 8, 8};
    col.spacing = 6.0f;
    kin::EcsEntity column = world.entity("column").set(Ui2Layout{.style = col});
    column.raw().child_of(root.raw());

    kin::EcsEntity play = world.entity("play")
                              .set(ui2::Button{.label = "Play"})
                              .set(Ui2Layout{.style = {.width = ui2::grow(), .height = ui2::fixed(30)}});
    play.raw().child_of(column.raw());
    kin::EcsEntity quit = world.entity("quit")
                              .set(ui2::Button{.label = "Quit"})
                              .set(Ui2Layout{});
    quit.raw().child_of(column.raw());

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch);
    ui.end();

    const f32 quit_h = ui2::measure_text(quit.get<ui2::Button>()->text_style.font, "Quit", quit.get<ui2::Button>()->text_style.scale).y + 12.0f;
    assert(rect_eq(play.get<ui2::Button>()->bounds, 8, 8, 184, 30));
    assert(approx(quit.get<ui2::Button>()->bounds.x, 8));
    assert(approx(quit.get<ui2::Button>()->bounds.y, 44));
    assert(approx(quit.get<ui2::Button>()->bounds.h, quit_h));
    assert(rect_eq(column.get<Ui2Layout>()->solved, 0, 0, 200, 8 + 30 + 6 + quit_h + 8));
}

void test_ecs_layout_button_click() {
    kin::EcsWorld world;
    register_ui2_components(world);

    kin::EcsEntity root = world.entity("root").set(Ui2Root{.bounds = {0, 0, 100, 30}});
    kin::EcsEntity button = world.entity("button")
                                .set(ui2::Button{.label = "Play"})
                                .set(Ui2Layout{.style = {.width = ui2::grow(), .height = ui2::grow()}});
    button.raw().child_of(root.raw());

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    const auto render_frame = [&]() {
        ui.begin(input, renderer);
        update_ui2_world(world, ui, scratch);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    render_frame();
    assert(rect_eq(button.get<ui2::Button>()->bounds, 0, 0, 100, 30));

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, true);
    render_frame();

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, false);
    render_frame();
    assert(button.get<ui2::Button>()->clicked);
}

void test_ecs_layout_preserves_explicit_bounds_widgets() {
    kin::EcsWorld world;
    register_ui2_components(world);

    kin::EcsEntity button = world.entity("button")
                                .set(ui2::Button{
                                    .bounds = {5, 6, 70, 20},
                                    .label = "Explicit",
                                });

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch);
    ui.end();

    assert(rect_eq(button.get<ui2::Button>()->bounds, 5, 6, 70, 20));
    assert(scratch.size() == 1);
}

void test_ecs_layout_multiple_roots() {
    kin::EcsWorld world;
    register_ui2_components(world);

    kin::EcsEntity root_a = world.entity("root-a").set(Ui2Root{.bounds = {0, 0, 100, 20}});
    kin::EcsEntity root_b = world.entity("root-b").set(Ui2Root{.bounds = {200, 50, 80, 30}});
    kin::EcsEntity a = world.entity("a")
                           .set(ui2::ProgressBar{})
                           .set(Ui2Layout{.style = {.width = ui2::grow(), .height = ui2::grow()}});
    kin::EcsEntity b = world.entity("b")
                           .set(ui2::ProgressBar{})
                           .set(Ui2Layout{.style = {.width = ui2::grow(), .height = ui2::grow()}});
    a.raw().child_of(root_a.raw());
    b.raw().child_of(root_b.raw());

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch);
    ui.end();

    assert(rect_eq(a.get<ui2::ProgressBar>()->bounds, 0, 0, 100, 20));
    assert(rect_eq(b.get<ui2::ProgressBar>()->bounds, 200, 50, 80, 30));
}

void test_ecs_layout_overlay_panel_and_centered_column() {
    kin::EcsWorld world;
    register_ui2_components(world);

    kin::EcsEntity root = world.entity("root").set(Ui2Root{.bounds = {0, 0, 200, 120}});
    kin::EcsEntity overlay = world.entity("overlay")
                                .set(ui2::Panel{})
                                .set(Ui2Layout{.style = {
                                                   .width = ui2::grow(),
                                                   .height = ui2::grow(),
                                                   .overlay = true,
                                               }})
                                .set(Ui2Layer{.order = 0});
    overlay.raw().child_of(root.raw());

    ui2::LayoutStyle shell;
    shell.width = ui2::grow();
    shell.height = ui2::grow();
    shell.axis = ui2::UiLayoutAxis::Vertical;
    shell.main = ui2::UiAlign::Center;
    shell.cross = ui2::UiAlign::Center;
    kin::EcsEntity centered = world.entity("centered").set(Ui2Layout{.style = shell});
    centered.raw().child_of(root.raw());

    kin::EcsEntity button = world.entity("button")
                                .set(ui2::Button{.label = "Play"})
                                .set(Ui2Layout{.style = {.width = ui2::fixed(80), .height = ui2::fixed(30)}})
                                .set(Ui2Layer{.order = 1});
    button.raw().child_of(centered.raw());

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch);
    ui.end();

    assert(rect_eq(overlay.get<ui2::Panel>()->bounds, 0, 0, 200, 120));
    assert(rect_eq(button.get<ui2::Button>()->bounds, 60, 45, 80, 30));
}

void test_ui2_layout_presets() {
    const ui2::LayoutStyle fill = ui2::fill();
    assert(fill.width.mode == ui2::SizeMode::Grow);
    assert(fill.height.mode == ui2::SizeMode::Grow);

    const ui2::LayoutStyle overlay = ui2::overlay(ui2::UiAnchor::BottomRight, {-2, -3});
    assert(overlay.width.mode == ui2::SizeMode::Grow);
    assert(overlay.height.mode == ui2::SizeMode::Grow);
    assert(overlay.overlay);
    assert(overlay.anchor == ui2::UiAnchor::BottomRight);
    assert(approx(overlay.anchor_offset.x, -2));
    assert(approx(overlay.anchor_offset.y, -3));

    const ui2::LayoutStyle fixed = ui2::fixed_box(12, 34);
    assert(fixed.width.mode == ui2::SizeMode::Fixed);
    assert(fixed.height.mode == ui2::SizeMode::Fixed);
    assert(approx(fixed.width.value, 12));
    assert(approx(fixed.height.value, 34));

    const ui2::LayoutStyle col = ui2::column(ui2::fixed(80), ui2::fit(), ui2::UiAlign::Center, ui2::UiAlign::End, 7);
    assert(col.axis == ui2::UiLayoutAxis::Vertical);
    assert(col.main == ui2::UiAlign::Center);
    assert(col.cross == ui2::UiAlign::End);
    assert(approx(col.spacing, 7));
    assert(col.width.mode == ui2::SizeMode::Fixed);

    const ui2::LayoutStyle row = ui2::row(ui2::grow(), ui2::fixed(20), ui2::UiAlign::End, ui2::UiAlign::Stretch, 3);
    assert(row.axis == ui2::UiLayoutAxis::Horizontal);
    assert(row.main == ui2::UiAlign::End);
    assert(row.cross == ui2::UiAlign::Stretch);
    assert(approx(row.spacing, 3));
    assert(row.height.mode == ui2::SizeMode::Fixed);
}

void test_ecs_ui2_builder_components_and_hierarchy() {
    kin::EcsWorld world;
    register_ui2_components(world);

    auto root = ui2_entity(world, "root").root({0, 0, 200, 100}).static_ui();
    auto column = ui2_entity(world, "column")
                      .layout(ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Center, ui2::UiAlign::Stretch, 5))
                      .child_of(root);
    kin::EcsEntity button = ui2_entity(world, "button")
                                .button(ui2::Button{.label = "Play"})
                                .layout(ui2::fixed_box(80, 30))
                                .layer(4)
                                .static_ui()
                                .child_of(column)
                                .entity();
    kin::EcsEntity label = ui2_entity(world, "label")
                               .label(ui2::Label{.text = "Child"})
                               .layout(ui2::fill())
                               .child_of(root.entity())
                               .entity();
    kin::EcsEntity dynamic = ui2_entity(world, "dynamic").static_ui().static_ui(false).entity();

    assert(root.entity().has<Ui2Root>());
    assert(root.entity().has<Ui2Static>());
    assert(column.entity().has<Ui2Layout>());
    assert(button.has<ui2::Button>());
    assert(button.has<Ui2Layout>());
    assert(button.has<Ui2Layer>());
    assert(button.has<Ui2Static>());
    assert(button.get<Ui2Layer>()->order == 4);
    assert(button.parent().id() == column.entity().id());
    assert(label.parent().id() == root.entity().id());
    assert(!dynamic.has<Ui2Static>());
}

// Ui2Text: widgets take their text from the active localization, again when
// the language changes, and the layout is solved again.
void test_ecs_ui2_text_keys() {
    kin::EcsWorld world;
    register_ui2_components(world);

    // Without a localization the key shows.
    kin::EcsEntity bare = ui2_entity(world, "bare").label(ui2::Label{}).text_key("menu.play").entity();
    assert(bare.get<ui2::Label>()->text == "menu.play");

    kin::Localization l10n;
    kin::LanguageFile en{.locale = "en", .name = "English"};
    en.strings.emplace("menu.play", "Play");
    en.strings.emplace("hud.gold", "{gold, plural, one {# coin} other {# coins}}");
    kin::LanguageFile fr{.locale = "fr", .name = "Français"};
    fr.strings.emplace("menu.play", "Jouer");
    fr.strings.emplace("hud.gold", "{gold, plural, one {# pièce} other {# pièces}}");
    l10n.add("test", std::vector<kin::LanguageFile>{std::move(en), std::move(fr)});
    kin::set_active_localization(&l10n);

    auto root = ui2_entity(world, "root").root({0, 0, 200, 100});
    kin::EcsEntity button = ui2_entity(world, "button")
                                .button(ui2::Button{})
                                .text_key("menu.play")
                                .child_of(root)
                                .entity();
    kin::EcsEntity gold = ui2_entity(world, "gold")
                              .label(ui2::Label{})
                              .text_key("hud.gold", {{.name = "gold", .value = 1250.0}})
                              .child_of(root)
                              .entity();
    // A key set before its widget is applied once the widget is there.
    kin::EcsEntity late = ui2_entity(world, "late").text_key("menu.play").child_of(root).entity();
    late.raw().set(ui2::WrappedText{});

    assert(button.get<ui2::Button>()->label == "Play");
    assert(gold.get<ui2::Label>()->text == "1,250 coins");
    assert(kin::apply_ui2_text(world) == 2); // "bare" (now localized) and "late"
    assert(late.get<ui2::WrappedText>()->text == "Play");
    assert(kin::apply_ui2_text(world) == 0); // nothing changed since

    const kin::u64 version = root.entity().get<Ui2LayoutCache>()->tree_version;
    l10n.set_locale("fr");
    assert(kin::apply_ui2_text(world) == 4);
    assert(button.get<ui2::Button>()->label == "Jouer");
    assert(gold.get<ui2::Label>()->text == "1\xC2\xA0" "250 pi\xC3\xA8" "ces");
    assert(root.entity().get<Ui2LayoutCache>()->tree_version > version);

    // Setting the component anew applies it at once on the next pass.
    gold.raw().set(kin::Ui2Text{.key = "hud.gold", .args = {{.name = "gold", .value = 1.0}}});
    assert(kin::apply_ui2_text(world) == 1);
    assert(gold.get<ui2::Label>()->text == "1 pi\xC3\xA8" "ce");
    kin::set_active_localization(nullptr);
}

void test_ecs_ui2_builder_widget_methods() {
    kin::EcsWorld world;
    register_ui2_components(world);

    assert(ui2_entity(world, "panel").panel(ui2::Panel{}).entity().has<ui2::Panel>());
    assert(ui2_entity(world, "wrapped").wrapped_text(ui2::WrappedText{}).entity().has<ui2::WrappedText>());
    assert(ui2_entity(world, "separator").separator(ui2::Separator{}).entity().has<ui2::Separator>());
    assert(ui2_entity(world, "toggle").toggle(ui2::Toggle{}).entity().has<ui2::Toggle>());
    assert(ui2_entity(world, "slider").slider(ui2::Slider{}).entity().has<ui2::Slider>());
    assert(ui2_entity(world, "progress").progress(ui2::ProgressBar{}).entity().has<ui2::ProgressBar>());
    assert(ui2_entity(world, "image").image(ui2::Image{}).entity().has<ui2::Image>());
    assert(ui2_entity(world, "nine").nine_slice(ui2::NineSlicePanel{}).entity().has<ui2::NineSlicePanel>());
    assert(ui2_entity(world, "spacer").spacer(ui2::Spacer{}).entity().has<ui2::Spacer>());
    assert(ui2_entity(world, "icon").icon_button(ui2::IconButton{}).entity().has<ui2::IconButton>());
    assert(ui2_entity(world, "slot").icon_slot(ui2::IconSlot{}).entity().has<ui2::IconSlot>());
    assert(ui2_entity(world, "meter").meter(ui2::Meter{}).entity().has<ui2::Meter>());
    assert(ui2_entity(world, "prompt").prompt(ui2::PromptLabel{}).entity().has<ui2::PromptLabel>());
    assert(ui2_entity(world, "prompt-row").prompt_row(ui2::PromptRow{}).entity().has<ui2::PromptRow>());
    assert(ui2_entity(world, "menu").menu_list(ui2::MenuList{}).entity().has<ui2::MenuList>());
    assert(ui2_entity(world, "menubar").menu_bar(ui2::MenuBar{}).entity().has<ui2::MenuBar>());
    assert(ui2_entity(world, "tabs").tab_bar(ui2::TabBar{}).entity().has<ui2::TabBar>());
    assert(ui2_entity(world, "split").splitter(ui2::Splitter{}).entity().has<ui2::Splitter>());
    assert(ui2_entity(world, "dock").dock_panel(ui2::DockPanel{}).entity().has<ui2::DockPanel>());
    assert(ui2_entity(world, "crumbs").breadcrumb_bar(ui2::BreadcrumbBar{}).entity().has<ui2::BreadcrumbBar>());
    assert(ui2_entity(world, "assets").asset_browser(ui2::AssetBrowser{}).entity().has<ui2::AssetBrowser>());
    assert(ui2_entity(world, "status").status_bar(ui2::StatusBar{}).entity().has<ui2::StatusBar>());
    assert(ui2_entity(world, "log").log_console(ui2::LogConsole{}).entity().has<ui2::LogConsole>());
    assert(ui2_entity(world, "inspector").property_inspector(ui2::PropertyInspector{}).entity().has<ui2::PropertyInspector>());
    assert(ui2_entity(world, "graph").node_graph(ui2::NodeGraph{}).entity().has<ui2::NodeGraph>());
    assert(ui2_entity(world, "dialog").dialog(ui2::DialogBox{}).entity().has<ui2::DialogBox>());
    assert(ui2_entity(world, "dialogue").dialogue_view(ui2::DialogueView{}).entity().has<ui2::DialogueView>());
    assert(ui2_entity(world, "nameplate").nameplate(ui2::Nameplate{}).entity().has<ui2::Nameplate>());
    assert(ui2_entity(world, "selection").selection_rect(ui2::SelectionRect{}).entity().has<ui2::SelectionRect>());
    assert(ui2_entity(world, "reticle").target_reticle(ui2::TargetReticle{}).entity().has<ui2::TargetReticle>());
    assert(ui2_entity(world, "scroll").scroll_view(ui2::ScrollView{}).entity().has<ui2::ScrollView>());
    assert(ui2_entity(world, "resources").resource_row(ui2::ResourceRow{}).entity().has<ui2::ResourceRow>());
    assert(ui2_entity(world, "bar").labeled_bar(ui2::LabeledBar{}).entity().has<ui2::LabeledBar>());
    assert(ui2_entity(world, "icons").icon_meter(ui2::IconMeter{}).entity().has<ui2::IconMeter>());
    assert(ui2_entity(world, "rich").rich_text_line(ui2::RichTextLine{}).entity().has<ui2::RichTextLine>());
    assert(ui2_entity(world, "area").text_area(ui2::TextArea{}).entity().has<ui2::TextArea>());
    assert(ui2_entity(world, "advanced").advanced_text(ui2::AdvancedText{}).entity().has<ui2::AdvancedText>());
    assert(ui2_entity(world, "toasts").toast_stack(ui2::ToastStack{}).entity().has<ui2::ToastStack>());
    assert(ui2_entity(world, "floating").floating_text(ui2::FloatingText{}).entity().has<ui2::FloatingText>());
    assert(ui2_entity(world, "animated").animated_value(ui2::AnimatedValue{}).entity().has<ui2::AnimatedValue>());
    assert(ui2_entity(world, "text").text_input(ui2::TextInput{}).entity().has<ui2::TextInput>());
    assert(ui2_entity(world, "number").number_input(ui2::NumberInput{}).entity().has<ui2::NumberInput>());
    assert(ui2_entity(world, "combo").combo_box(ui2::ComboBox{}).entity().has<ui2::ComboBox>());
    assert(ui2_entity(world, "picker").color_picker(ui2::ColorPicker{}).entity().has<ui2::ColorPicker>());
    assert(ui2_entity(world, "grid").icon_grid(ui2::IconGrid{}).entity().has<ui2::IconGrid>());
    assert(ui2_entity(world, "list").list_view(ui2::ListView{}).entity().has<ui2::ListView>());
    assert(ui2_entity(world, "table").table(ui2::Table{}).entity().has<ui2::Table>());
    assert(ui2_entity(world, "tree").tree_view(ui2::TreeView{}).entity().has<ui2::TreeView>());
    assert(ui2_entity(world, "props").property_grid(ui2::PropertyGrid{}).entity().has<ui2::PropertyGrid>());
}

void test_ecs_ui2_builder_button_click() {
    kin::EcsWorld world;
    register_ui2_components(world);

    auto root = ui2_entity(world, "root").root({0, 0, 100, 30});
    kin::EcsEntity button = ui2_entity(world, "button")
                                .button(ui2::Button{.label = "Play"})
                                .layout(ui2::fill())
                                .child_of(root)
                                .entity();

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    const auto render_frame = [&]() {
        ui.begin(input, renderer);
        update_ui2_world(world, ui, scratch);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    render_frame();
    assert(rect_eq(button.get<ui2::Button>()->bounds, 0, 0, 100, 30));

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, true);
    render_frame();

    input.begin_frame();
    input.set_mouse_pos({50, 15});
    input.set_mouse_held(MouseButton::Left, false);
    render_frame();
    assert(button.get<ui2::Button>()->clicked);
}

void test_ecs_bridge_game_widgets_layout_and_records() {
    kin::EcsWorld world;
    register_ui2_components(world);

    auto root = ui2_entity(world, "root").root({0, 0, 100, 180});
    auto column = ui2_entity(world, "column")
                      .layout(ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Stretch, 2))
                      .child_of(root);

    kin::EcsEntity image = ui2_entity(world, "image").image(ui2::Image{}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity nine = ui2_entity(world, "nine").nine_slice(ui2::NineSlicePanel{}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity spacer = ui2_entity(world, "spacer").spacer(ui2::Spacer{.size = {10, 4}}).layout().child_of(column).entity();
    kin::EcsEntity icon = ui2_entity(world, "icon").icon_button(ui2::IconButton{}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity slot = ui2_entity(world, "slot").icon_slot(ui2::IconSlot{}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity meter = ui2_entity(world, "meter").meter(ui2::Meter{}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity grid = ui2_entity(world, "grid").icon_grid(ui2::IconGrid{.items = {{.text = "A"}}}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity prompt = ui2_entity(world, "prompt").prompt(ui2::PromptLabel{.prompt = "A", .text = "Go"}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity prompt_row = ui2_entity(world, "prompt-row").prompt_row(ui2::PromptRow{.items = {{.prompt = "A", .text = "Go"}}}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity menu_bar = ui2_entity(world, "menu-bar").menu_bar(ui2::MenuBar{.items = {{.label = "File"}}}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();
    kin::EcsEntity dialog = ui2_entity(world, "dialog").dialog(ui2::DialogBox{.title = "T", .body = "B"}).layout(ui2::fixed_box(10, 10)).child_of(column).entity();

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch);
    ui.end();

    assert(scratch.size() == 11);
    assert(rect_eq(image.get<ui2::Image>()->bounds, 0, 0, 100, 10));
    assert(rect_eq(nine.get<ui2::NineSlicePanel>()->bounds, 0, 12, 100, 10));
    assert(rect_eq(spacer.get<ui2::Spacer>()->bounds, 0, 24, 100, 4));
    assert(rect_eq(icon.get<ui2::IconButton>()->bounds, 0, 30, 100, 10));
    assert(rect_eq(slot.get<ui2::IconSlot>()->bounds, 0, 42, 100, 10));
    assert(rect_eq(meter.get<ui2::Meter>()->bounds, 0, 54, 100, 10));
    assert(rect_eq(grid.get<ui2::IconGrid>()->bounds, 0, 66, 100, 10));
    assert(rect_eq(prompt.get<ui2::PromptLabel>()->bounds, 0, 78, 100, 10));
    assert(rect_eq(prompt_row.get<ui2::PromptRow>()->bounds, 0, 90, 100, 10));
    assert(rect_eq(menu_bar.get<ui2::MenuBar>()->bounds, 0, 102, 100, 10));
    assert(rect_eq(dialog.get<ui2::DialogBox>()->bounds, 0, 114, 100, 10));
}

void test_ecs_bridge_editor_widgets_layout_and_records() {
    kin::EcsWorld world;
    register_ui2_components(world);

    auto root = ui2_entity(world, "root").root({0, 0, 100, 450});
    auto column = ui2_entity(world, "column")
                      .layout(ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Stretch, 2))
                      .child_of(root);

    kin::EcsEntity scroll = ui2_entity(world, "scroll")
                                .scroll_view(ui2::ScrollView{.content_height = 100.0f})
                                .layout(ui2::fixed_box(50, 20))
                                .child_of(column)
                                .entity();
    kin::EcsEntity text = ui2_entity(world, "text").text_input(ui2::TextInput{}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity number = ui2_entity(world, "number").number_input(ui2::NumberInput{}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity combo = ui2_entity(world, "combo")
                               .combo_box(ui2::ComboBox{.items = {"A", "B"}})
                               .layout(ui2::fixed_box(50, 20))
                               .child_of(column)
                               .entity();
    kin::EcsEntity picker = ui2_entity(world, "picker").color_picker(ui2::ColorPicker{}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity wrapped = ui2_entity(world, "wrapped").wrapped_text(ui2::WrappedText{.text = "wrap"}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity separator = ui2_entity(world, "separator").separator(ui2::Separator{}).layout(ui2::fixed_box(50, 4)).child_of(column).entity();
    kin::EcsEntity list = ui2_entity(world, "list").list_view(ui2::ListView{.items = {"A", "B"}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity table = ui2_entity(world, "table").table(ui2::Table{.rows = {{"A"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity tree = ui2_entity(world, "tree").tree_view(ui2::TreeView{.items = {{.id = "a", .label = "A"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity props = ui2_entity(world, "props").property_grid(ui2::PropertyGrid{.rows = {{.label = "A"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity tabs = ui2_entity(world, "tabs").tab_bar(ui2::TabBar{.items = {{.label = "A"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity split = ui2_entity(world, "split").splitter(ui2::Splitter{}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity dock = ui2_entity(world, "dock").dock_panel(ui2::DockPanel{.title = "Dock"}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity crumbs = ui2_entity(world, "crumbs").breadcrumb_bar(ui2::BreadcrumbBar{.segments = {{.label = "Root"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity assets = ui2_entity(world, "assets").asset_browser(ui2::AssetBrowser{.items = {{.name = "A"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity status = ui2_entity(world, "status").status_bar(ui2::StatusBar{.left = {{.text = "Ready"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity log = ui2_entity(world, "log").log_console(ui2::LogConsole{.entries = {{.text = "Ready"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity inspector = ui2_entity(world, "inspector").property_inspector(ui2::PropertyInspector{.rows = {{.label = "A"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();
    kin::EcsEntity graph = ui2_entity(world, "graph").node_graph(ui2::NodeGraph{.nodes = {{.id = "a", .title = "A"}}}).layout(ui2::fixed_box(50, 20)).child_of(column).entity();

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch);
    ui.end();

    assert(scratch.size() == 20);
    assert(rect_eq(scroll.get<ui2::ScrollView>()->bounds, 0, 0, 100, 20));
    assert(rect_eq(text.get<ui2::TextInput>()->bounds, 0, 22, 100, 20));
    assert(rect_eq(number.get<ui2::NumberInput>()->bounds, 0, 44, 100, 20));
    assert(rect_eq(combo.get<ui2::ComboBox>()->bounds, 0, 66, 100, 20));
    assert(rect_eq(picker.get<ui2::ColorPicker>()->bounds, 0, 88, 100, 20));
    assert(rect_eq(wrapped.get<ui2::WrappedText>()->bounds, 0, 110, 100, 20));
    assert(rect_eq(separator.get<ui2::Separator>()->bounds, 0, 132, 100, 4));
    assert(rect_eq(list.get<ui2::ListView>()->bounds, 0, 138, 100, 20));
    assert(rect_eq(table.get<ui2::Table>()->bounds, 0, 160, 100, 20));
    assert(rect_eq(tree.get<ui2::TreeView>()->bounds, 0, 182, 100, 20));
    assert(rect_eq(props.get<ui2::PropertyGrid>()->bounds, 0, 204, 100, 20));
    assert(rect_eq(tabs.get<ui2::TabBar>()->bounds, 0, 226, 100, 20));
    assert(rect_eq(split.get<ui2::Splitter>()->bounds, 0, 248, 100, 20));
    assert(rect_eq(dock.get<ui2::DockPanel>()->bounds, 0, 270, 100, 20));
    assert(rect_eq(crumbs.get<ui2::BreadcrumbBar>()->bounds, 0, 292, 100, 20));
    assert(rect_eq(assets.get<ui2::AssetBrowser>()->bounds, 0, 314, 100, 20));
    assert(rect_eq(status.get<ui2::StatusBar>()->bounds, 0, 336, 100, 20));
    assert(rect_eq(log.get<ui2::LogConsole>()->bounds, 0, 358, 100, 20));
    assert(rect_eq(inspector.get<ui2::PropertyInspector>()->bounds, 0, 380, 100, 20));
    assert(rect_eq(graph.get<ui2::NodeGraph>()->bounds, 0, 402, 100, 20));
}

void test_ecs_bridge_remaining_basic_widgets_and_scroll_container() {
    kin::EcsWorld world;
    register_ui2_components(world);

    auto root = ui2_entity(world, "root").root({0, 0, 100, 120});
    auto scroll = ui2_entity(world, "scroll")
                      .layout(ui2::column(ui2::grow(), ui2::fixed(40), ui2::UiAlign::Start, ui2::UiAlign::Stretch, 2))
                      .scroll_container()
                      .child_of(root);

    kin::EcsEntity menu = ui2_entity(world, "menu").menu_list(ui2::MenuList{.items = {{.label = "A"}}}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity nameplate = ui2_entity(world, "nameplate").nameplate(ui2::Nameplate{.label = "N"}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity selection = ui2_entity(world, "selection").selection_rect(ui2::SelectionRect{.start = {0, 0}, .end = {10, 10}}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity reticle = ui2_entity(world, "reticle").target_reticle(ui2::TargetReticle{}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity resources = ui2_entity(world, "resources").resource_row(ui2::ResourceRow{.items = {{.label = "G", .value = "1"}}}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity bar = ui2_entity(world, "bar").labeled_bar(ui2::LabeledBar{.label = "HP"}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity meter = ui2_entity(world, "meter").icon_meter(ui2::IconMeter{.value = 1, .max = 2}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity rich = ui2_entity(world, "rich").rich_text_line(ui2::RichTextLine{.spans = {{.text = "R"}}}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity area = ui2_entity(world, "area").text_area(ui2::TextArea{.text = "Area"}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity advanced = ui2_entity(world, "advanced").advanced_text(ui2::AdvancedText{.markup = "A"}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity toasts = ui2_entity(world, "toasts").toast_stack(ui2::ToastStack{.items = {{.text = "T"}}}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity floating = ui2_entity(world, "floating").floating_text(ui2::FloatingText{.text = "F"}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();
    kin::EcsEntity animated = ui2_entity(world, "animated").animated_value(ui2::AnimatedValue{.value = 2.0f, .display = 1.0f}).layout(ui2::fixed_box(50, 20)).child_of(scroll).entity();

    Input input;
    std::vector<Rectf> draws;
    Renderer2D renderer = make_recording_renderer(draws);
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_wheel_y(-1.0f);
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch);
    ui.end();

    assert(scratch.size() == 13);
    assert(scroll.entity().get<Ui2ScrollContainer>()->offset > 0.0f);
    assert(scroll.entity().get<Ui2ScrollContainer>()->content_height > 40.0f);
    bool saw_scrollbar_track = false;
    for (Rectf draw : draws) {
        saw_scrollbar_track = saw_scrollbar_track || rect_eq(draw, 90, 0, 10, 40);
    }
    assert(saw_scrollbar_track);
    assert(scratch.records[0].clipped);
    assert(rect_eq(scratch.records[0].clip, 0, 0, 100, 40));
    assert(menu.get<ui2::MenuList>()->bounds.y < 0.0f);
    assert(nameplate.has<ui2::Nameplate>());
    assert(selection.has<ui2::SelectionRect>());
    assert(reticle.has<ui2::TargetReticle>());
    assert(resources.has<ui2::ResourceRow>());
    assert(bar.has<ui2::LabeledBar>());
    assert(meter.has<ui2::IconMeter>());
    assert(rich.has<ui2::RichTextLine>());
    assert(area.has<ui2::TextArea>());
    assert(advanced.has<ui2::AdvancedText>());
    assert(toasts.has<ui2::ToastStack>());
    assert(floating.has<ui2::FloatingText>());
    assert(animated.has<ui2::AnimatedValue>());
    assert(rect_eq(toasts.get<ui2::ToastStack>()->bounds, 0, 220 - scroll.entity().get<Ui2ScrollContainer>()->offset, 100, 20));
}

void test_ecs_bridge_static_dynamic_filtering() {
    kin::EcsWorld world;
    register_ui2_components(world);

    world.entity("static")
        .set(ui2::Label{.bounds = {0, 0, 10, 10}, .text = "S"})
        .add<Ui2Static>();
    world.entity("dynamic")
        .set(ui2::Label{.bounds = {0, 10, 10, 10}, .text = "D"});
    world.entity("static-prompt")
        .set(ui2::PromptLabel{.bounds = {0, 20, 10, 10}, .prompt = "P"})
        .add<Ui2Static>();
    world.entity("dynamic-prompt-row")
        .set(ui2::PromptRow{.bounds = {0, 30, 10, 10}, .items = {{.prompt = "R"}}});

    kin::EcsEntity root = world.entity("root").set(Ui2Root{.bounds = {0, 0, 20, 20}});
    kin::EcsEntity static_overlay = world.entity("static-overlay")
                                       .set(ui2::Panel{})
                                       .set(Ui2Layout{.style = {
                                                          .width = ui2::grow(),
                                                          .height = ui2::grow(),
                                                          .overlay = true,
                                                      }})
                                       .add<Ui2Static>();
    static_overlay.raw().child_of(root.raw());

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch, {.include_static = true, .include_dynamic = false});
    ui.end();
    assert(scratch.size() == 3);
    assert(scratch.records[0].label && scratch.records[0].label->text == "S");
    assert(scratch.records[1].prompt);
    assert(scratch.records[2].panel);

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(world, ui, scratch, {.include_static = false, .include_dynamic = true});
    ui.end();
    assert(scratch.size() == 2);
    assert(scratch.records[0].label && scratch.records[0].label->text == "D");
    assert(scratch.records[1].prompt_row);
}

void test_ecs_sync_stable_keys_sweep_and_state() {
    kin::EcsWorld world;
    register_ui2_components(world);

    const auto find_button = [&](std::string_view label) {
        kin::EcsEntity found;
        world.raw().each([&](flecs::entity entity, ui2::Button& button) {
            if (button.label == label) {
                found = kin::EcsEntity{entity};
            }
        });
        return found;
    };
    const auto find_text_input = [&]() {
        kin::EcsEntity found;
        world.raw().each([&](flecs::entity entity, ui2::TextInput&) {
            found = kin::EcsEntity{entity};
        });
        return found;
    };

    {
        Ui2Sync sync = begin_ui2_sync(world, "hud", {0, 0, 200, 120});
        sync.begin_column(ui2::column(ui2::grow(), ui2::fit()));
        sync.key("score").label(ui2::Label{.text = "Score: 1"});
        sync.button(ui2::Button{.label = "Restart"});
        sync.text_input();
        sync.end_column();
        sync.end();
    }

    kin::EcsEntity restart = find_button("Restart");
    kin::EcsEntity text = find_text_input();
    assert(restart);
    assert(text);
    const kin::EcsId restart_id = restart.id();
    text.get_mut<ui2::TextInput>()->state.text = "typed";

    {
        Ui2Sync sync = begin_ui2_sync(world, "hud", {0, 0, 200, 120});
        sync.begin_column(ui2::column(ui2::grow(), ui2::fit()));
        sync.key("score").label(ui2::Label{.text = "Score: 2"});
        sync.button(ui2::Button{.label = "Restart"});
        const ui2::TextInput input = sync.text_input();
        assert(input.state.text == "typed");
        sync.end_column();
        sync.end();
    }

    assert(find_button("Restart").id() == restart_id);

    {
        Ui2Sync sync = begin_ui2_sync(world, "hud", {0, 0, 200, 120});
        sync.begin_column(ui2::column(ui2::grow(), ui2::fit()));
        sync.key("score").label(ui2::Label{.text = "Score: 3"});
        sync.end_column();
        sync.end();
    }

    assert(!kin::EcsEntity{world.raw().entity(restart_id)});
}

void test_ecs_sync_explicit_keys_survive_reorder() {
    kin::EcsWorld world;
    register_ui2_components(world);

    const auto sync_slots = [&](bool reversed) {
        Ui2Sync sync = begin_ui2_sync(world, "inventory", {0, 0, 160, 40});
        sync.begin_row(ui2::row(ui2::grow(), ui2::fit()));
        if (!reversed) {
            sync.key("slot_a").button(ui2::Button{.label = "A"});
            sync.key("slot_b").button(ui2::Button{.label = "B"});
        } else {
            sync.key("slot_b").button(ui2::Button{.label = "B"});
            sync.key("slot_a").button(ui2::Button{.label = "A"});
        }
        sync.end_row();
        sync.end();
    };
    const auto button_id = [&](std::string_view label) {
        kin::EcsId id = 0;
        world.raw().each([&](flecs::entity entity, ui2::Button& button) {
            if (button.label == label) {
                id = static_cast<kin::EcsId>(entity.id());
            }
        });
        return id;
    };

    sync_slots(false);
    const kin::EcsId a = button_id("A");
    const kin::EcsId b = button_id("B");
    sync_slots(true);
    assert(button_id("A") == a);
    assert(button_id("B") == b);
}

void test_ecs_lazy_layout_solve_cache_and_dirty_mark() {
    kin::EcsWorld world;
    register_ui2_components(world);

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;
    kin::EcsEntity root;

    const auto sync_tree = [&](std::string text) {
        Ui2Sync sync = begin_ui2_sync(world, "hud", {0, 0, 200, 80});
        root = sync.root();
        sync.begin_column(ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Stretch, 4));
        sync.key("label").label(ui2::Label{.text = std::move(text)});
        sync.key("button").button(ui2::Button{.label = "Go"});
        sync.end_column();
        sync.end();
    };
    const auto render_frame = [&]() {
        input.begin_frame();
        ui.begin(input, renderer);
        update_ui2_world(world, ui, scratch);
        ui.end();
    };
    const auto solve_count = [&]() {
        return root.get<Ui2LayoutCache>()->solve_count;
    };

    sync_tree("A");
    render_frame();
    assert(solve_count() == 1);

    sync_tree("A");
    render_frame();
    assert(solve_count() == 1);

    sync_tree("B");
    render_frame();
    assert(solve_count() == 2);

    kin::EcsEntity label;
    world.raw().each([&](flecs::entity entity, ui2::Label& widget) {
        if (widget.text == "B") {
            label = kin::EcsEntity{entity};
        }
    });
    assert(label);
    label.get_mut<ui2::Label>()->text = "C";
    ui2_mark_dirty(label);
    render_frame();
    assert(solve_count() == 3);
}

void test_ecs_sync_matches_builder_layout() {
    kin::EcsWorld sync_world;
    register_ui2_components(sync_world);
    {
        Ui2Sync sync = begin_ui2_sync(sync_world, "root", {0, 0, 200, 120});
        sync.begin_column(ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Stretch, 6));
        sync.key("play").button(ui2::Button{.label = "Play"});
        sync.key("quit").button(ui2::Button{.label = "Quit"});
        sync.end_column();
        sync.end();
    }

    kin::EcsWorld builder_world;
    register_ui2_components(builder_world);
    auto root = ui2_entity(builder_world, "root").root({0, 0, 200, 120});
    auto column = ui2_entity(builder_world, "column")
                      .layout(ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Stretch, 6))
                      .child_of(root);
    kin::EcsEntity play = ui2_entity(builder_world, "play").button(ui2::Button{.label = "Play"}).layout().child_of(column).entity();
    kin::EcsEntity quit = ui2_entity(builder_world, "quit").button(ui2::Button{.label = "Quit"}).layout().child_of(column).entity();

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(sync_world, ui, scratch);
    ui.end();

    input.begin_frame();
    ui.begin(input, renderer);
    update_ui2_world(builder_world, ui, scratch);
    ui.end();

    Rectf sync_play{};
    Rectf sync_quit{};
    sync_world.raw().each([&](flecs::entity, ui2::Button& button) {
        if (button.label == "Play") {
            sync_play = button.bounds;
        } else if (button.label == "Quit") {
            sync_quit = button.bounds;
        }
    });
    assert(sync_play == play.get<ui2::Button>()->bounds);
    assert(sync_quit == quit.get<ui2::Button>()->bounds);
}

// Sibling order must follow sync call order even after a sweep destroys and
// recreates the entities (flecs recycles ids, so id order alone scrambles).
void test_ecs_sync_document_order_survives_recreate() {
    kin::EcsWorld world;
    register_ui2_components(world);

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    const auto sync_rows = [&](bool populated) {
        Ui2Sync sync = begin_ui2_sync(world, "root", {0, 0, 200, 200});
        if (populated) {
            sync.begin_column(ui2::column(ui2::grow(), ui2::fit(), ui2::UiAlign::Start, ui2::UiAlign::Stretch, 4));
            sync.key("first").button(ui2::Button{.label = "first"});
            sync.key("second").button(ui2::Button{.label = "second"});
            sync.key("third").button(ui2::Button{.label = "third"});
            sync.end_column();
        }
        sync.end();
        input.begin_frame();
        ui.begin(input, renderer);
        update_ui2_world(world, ui, scratch);
        ui.end();
    };

    const auto row_y = [&](std::string_view label) {
        f32 y = -1.0f;
        world.raw().each([&](flecs::entity, ui2::Button& button) {
            if (button.label == label) {
                y = button.bounds.y;
            }
        });
        return y;
    };

    sync_rows(true);
    const f32 first_y = row_y("first");
    const f32 second_y = row_y("second");
    const f32 third_y = row_y("third");
    assert(first_y < second_y && second_y < third_y);

    sync_rows(false); // sweep destroys all rows
    sync_rows(true);  // recreate: entity ids are recycled, order must hold
    assert(row_y("first") == first_y);
    assert(row_y("second") == second_y);
    assert(row_y("third") == third_y);
}

// A font/scale change must re-measure even when the text is unchanged —
// otherwise a theme switch leaves stale solved widths (overlapping labels).
void test_ecs_sync_text_style_change_remeasures() {
    kin::EcsWorld world;
    register_ui2_components(world);

    Input input;
    Renderer2D renderer = make_renderer();
    ui2::Context ui;
    Ui2WorldRenderScratch scratch;

    const auto sync_label = [&](f32 scale) {
        Ui2Sync sync = begin_ui2_sync(world, "root", {0, 0, 400, 100});
        sync.begin_row(ui2::row(ui2::fit(), ui2::fit()));
        sync.key("title").label({.text = "same text", .text_style = {.scale = scale}});
        sync.end_row();
        sync.end();
        input.begin_frame();
        ui.begin(input, renderer);
        update_ui2_world(world, ui, scratch);
        ui.end();
    };

    sync_label(2.0f);
    f32 narrow = 0.0f;
    world.raw().each([&](flecs::entity, ui2::Label& label) { narrow = label.bounds.w; });
    assert(narrow > 0.0f);

    sync_label(4.0f); // same text, doubled scale: solved width must grow
    f32 wide = 0.0f;
    world.raw().each([&](flecs::entity, ui2::Label& label) { wide = label.bounds.w; });
    assert(wide > narrow * 1.5f);
}

void test_ui2_theme_resolution_and_context_defaults() {
    const ui2::Theme slate_game = ui2::game_theme(ui2::PalettePreset::Slate);
    const ui2::Theme ember_game = ui2::game_theme(ui2::PalettePreset::Ember);
    const ui2::Theme slate_editor = ui2::editor_theme(ui2::PalettePreset::Slate);
    const ui2::Theme default_game = ui2::game_theme(ui2::PalettePreset::Default);

    assert(default_game.palette.accent == ui2::radix::blue_dark[8]); // scale-derived Default
    assert(default_game.palette.accent != default_game.palette.success);
    // P1 accent restraint: generic indicators use the (muted) accent step, not
    // saturated success-green. Palette-derived themes use a flat accent scale, so
    // the muted step collapses to palette.accent; success-green is reserved for
    // status (valid). A real Radix theme gets the distinct muted [7] step.
    assert(default_game.progress_fill == default_game.palette.accent);
    assert(default_game.meter_fill == default_game.palette.accent);
    assert(default_game.valid == default_game.palette.success);
    assert(!(default_game.progress_fill == default_game.palette.success));
    const ui2::Theme radix_indigo = ui2::radix_theme(ui2::RadixThemePreset::Shadcn);
    assert(radix_indigo.progress_fill == radix_indigo.scales.accent[7]);
    // P1 selection policy: item rows/cells select via a translucent accent tint
    // with NO border. An opaque border here bleeds through the inside-border fill
    // and renders the whole cell solid accent (the icon-grid regression).
    assert(default_game.list_item.surface.selected.border.a == 0);
    assert(default_game.list_item.surface.selected.border_mode == ui2::BorderMode::None);
    assert(default_game.list_item.surface.selected.fill.a < 255); // a tint, not solid
    // Scrollbar thumb is neutral, never accent.
    assert(!(default_game.scrollbar_thumb_surface.fill == default_game.palette.accent));
    // P2/F8: focused controls carry an accent focus ring (glow), and the state
    // ladder deepens monotonically (normal → hover → pressed, never lighter).
    assert(default_game.button.surface.focused.glow_size > 0.0f);
    assert(default_game.button.surface.focused.glow_color.a > 0);
    assert(!(default_game.button.surface.hovered.fill == default_game.button.surface.normal.fill));
    assert(!(default_game.button.surface.pressed.fill == default_game.button.surface.hovered.fill));
    assert(default_game.palette.text_on_accent == colors::white);
    assert(ui2::parchment_palette().text_on_accent == colors::white);
    assert(ui2::high_contrast_palette().text_on_accent == colors::black);
    assert(slate_game.palette.accent == ui2::slate_palette().accent);
    assert(slate_game.colors.solid_accent == slate_game.palette.accent);
    assert(slate_game.colors.interactive_hovered == slate_game.palette.surface_hover);
    assert(slate_game.colors.text == slate_game.palette.text);
    assert(slate_game.sizes.padding_x == slate_game.preset.padding_x);
    assert(slate_game.sizes.control_height == slate_game.preset.row_height);
    assert(slate_game.button.padding.left == ui2::game_style_preset().padding_x);
    assert(slate_game.panel_surface.radius == ui2::game_style_preset().radius);
    assert(slate_game.panel_surface.border_width == ui2::game_style_preset().border_width);
    assert(slate_game.panel_surface.shadow.enabled);
    assert(slate_game.panel_surface.shadow.layers == 3);
    assert(slate_game.button.surface.hovered.fill == slate_game.palette.surface_hover);
    assert(slate_game.button.surface.focused.border == slate_game.palette.accent);
    assert(ui2::Context{}.theme().button.surface.normal.fill == default_game.button.surface.normal.fill);

    ui2::Interaction interaction{.hot = true, .active = true, .focused = true};
    ui2::SurfaceStyle resolved = ui2::resolve(slate_game.button.surface, interaction);
    assert(resolved.fill == slate_game.button.surface.pressed.fill);
    assert(resolved.border == slate_game.button.surface.focused.border);
    resolved = ui2::resolve(slate_game.button.surface, interaction, true);
    assert(resolved.fill == slate_game.button.surface.selected.fill);
    assert(resolved.border == slate_game.button.surface.focused.border);
    resolved = ui2::resolve(slate_game.button.surface, interaction, true, false);
    assert(resolved.fill == slate_game.button.surface.disabled.fill);
    assert(resolved.border == slate_game.button.surface.disabled.border);

    assert(ember_game.button.padding.left == slate_game.button.padding.left);
    assert(ember_game.palette.accent != slate_game.palette.accent);
    assert(slate_editor.button.padding.left != slate_game.button.padding.left);
    assert(slate_editor.body_text.scale != slate_game.body_text.scale);
    assert(!slate_editor.panel_surface.shadow.enabled);
    assert(ui2::make_theme(ui2::compact_style_preset(), ui2::slate_palette()).panel_surface.border_mode == ui2::BorderMode::None);

    ui2::StylePreset custom_shadow = ui2::game_style_preset();
    custom_shadow.shadow_spread = 7.0f;
    custom_shadow.shadow_layers = 5;
    const ui2::Theme shadow_theme = ui2::make_theme(custom_shadow, ui2::slate_palette());
    assert(shadow_theme.panel_surface.shadow.spread == 7.0f);
    assert(shadow_theme.panel_surface.shadow.layers == 5);

    const ui2::Palette radix_palette = ui2::make_palette(ui2::radix::slate_dark, ui2::radix::crimson_dark);
    assert(radix_palette.background == ui2::radix::slate_dark[0]);
    assert(radix_palette.surface_disabled == Color::rgba(ui2::radix::slate_dark[2].r,
                                                         ui2::radix::slate_dark[2].g,
                                                         ui2::radix::slate_dark[2].b,
                                                         28));
    assert(radix_palette.accent == ui2::radix::crimson_dark[8]);
    assert(radix_palette.accent_muted == Color::rgba(ui2::radix::crimson_dark[8].r,
                                                     ui2::radix::crimson_dark[8].g,
                                                     ui2::radix::crimson_dark[8].b,
                                                     28));
    assert(radix_palette.success == ui2::radix::green_dark[8]);
    assert(radix_palette.text_on_accent == colors::white);

    const ui2::StylePreset radix_preset =
        ui2::make_preset_from_radix(ui2::StylePresetKind::Game, {.spacing = 99, .radius = -4, .scaling = 1.0f});
    assert(radix_preset.spacing == 64.0f);
    assert(radix_preset.row_height == 256.0f);
    assert(radix_preset.radius == 3.0f);

    const ui2::Theme shadcn = ui2::shadcn_theme();
    assert(shadcn.palette.background == ui2::radix::slate_dark[0]);
    assert(shadcn.palette.accent == ui2::radix::indigo_dark[8]);
    assert(shadcn.scales.neutral[0] == ui2::radix::slate_dark[0]);
    assert(shadcn.scales.neutral_alpha[2] == ui2::radix::slate_dark_alpha[2]);
    assert(shadcn.scales.accent[8] == ui2::radix::indigo_dark[8]);
    assert(shadcn.scales.accent_alpha[2] == ui2::radix::indigo_dark_alpha[2]);
    assert(shadcn.colors.app_background == ui2::radix::slate_dark[0]);
    assert(shadcn.colors.interactive_selected == ui2::radix::indigo_dark_alpha[2]);
    assert(shadcn.colors.interactive_disabled == ui2::radix::slate_dark_alpha[2]);
    assert(shadcn.palette.accent_muted == shadcn.colors.interactive_selected);
    assert(shadcn.button.surface.hovered.fill == shadcn.colors.interactive_hovered);
    assert(shadcn.button.surface.selected.fill == shadcn.colors.interactive_selected);
    assert(shadcn.button.surface.focused.border == shadcn.colors.focus);
    assert(!shadcn.menu.surface.normal.draw_fill);
    assert(shadcn.menu.surface.normal.fill == colors::transparent);
    assert(shadcn.menu.surface.hovered.fill == shadcn.colors.surface_subtle);
    assert(shadcn.menu.surface.selected.fill == shadcn.colors.interactive_selected);
    assert(shadcn.menu.surface.disabled.fill == shadcn.colors.text_disabled);
    assert(shadcn.list_item.surface.normal.fill == colors::transparent);
    assert(shadcn.tab.track == shadcn.colors.interactive_disabled);
    assert(shadcn.tab.surface.selected.fill == shadcn.colors.surface_panel);
    assert(shadcn.tab.surface.selected.border == shadcn.colors.border);
    assert(shadcn.prompt_chip_fill == shadcn.colors.interactive_disabled);
    assert(shadcn.button.padding.left == 12.0f);

    const ui2::Theme shadcn_named = ui2::radix_theme(ui2::RadixThemePreset::Shadcn);
    assert(shadcn_named.palette.background == shadcn.palette.background);
    assert(shadcn_named.palette.accent == shadcn.palette.accent);
    assert(shadcn_named.button.padding.left == shadcn.button.padding.left);

    const ui2::Theme aurora = ui2::radix_theme(ui2::RadixThemePreset::Aurora);
    assert(aurora.palette.background == ui2::radix::sage_dark[0]);
    assert(aurora.palette.accent == ui2::radix::cyan_dark[8]);
    assert(aurora.preset.radius == 8.0f);

    const ui2::Theme terminal = ui2::radix_theme(ui2::RadixThemePreset::Terminal);
    assert(terminal.palette.background == ui2::radix::gray_dark[0]);
    assert(terminal.palette.accent == ui2::radix::lime_dark[8]);
    assert(terminal.preset.row_height == 32.0f);

    const ui2::Theme candy_editor = ui2::radix_theme(ui2::RadixThemePreset::Candy,
                                                     ui2::StylePresetKind::Editor,
                                                     {.spacing = 1, .radius = 1});
    assert(candy_editor.palette.background == ui2::radix::mauve_dark[0]);
    assert(candy_editor.palette.accent == ui2::radix::pink_dark[8]);
    assert(candy_editor.body_text.scale == ui2::editor_style_preset().text_scale);
    assert(candy_editor.preset.padding_x == 6.0f);

    ui2::MenuList menu;
    ui2::TabBar tabs;
    ui2::ScrollView scroll;
    ui2::PropertyInspector inspector;
    ui2::LogConsole log;
    ui2::AssetBrowser assets;
    ui2::NodeGraph graph;
    ui2::apply_theme(slate_editor, menu);
    ui2::apply_theme(slate_editor, tabs);
    ui2::apply_theme(slate_editor, scroll);
    ui2::apply_theme(slate_editor, inspector);
    ui2::apply_theme(slate_editor, log);
    ui2::apply_theme(slate_editor, assets);
    ui2::apply_theme(slate_editor, graph);
    // apply_theme wires ThemeSizeTokens into widget size fields (size tokens are live).
    assert(menu.row_height == slate_editor.sizes.compact_control_height);
    assert(menu.row_spacing == slate_editor.sizes.gap * 0.25f);
    assert(tabs.tab_height == slate_editor.sizes.compact_control_height);
    assert(tabs.gap == slate_editor.sizes.gap * 0.25f);
    assert(scroll.scrollbar_thickness == slate_editor.sizes.scrollbar_thickness);
    assert(inspector.row_height == slate_editor.sizes.compact_control_height);
    assert(inspector.row_spacing == slate_editor.sizes.gap * 0.5f);
    assert(log.row_height == slate_editor.sizes.compact_control_height);
    assert(assets.row_height == slate_editor.sizes.compact_control_height);
    assert(assets.spacing.x == slate_editor.sizes.gap * 0.5f);
    assert(graph.header_height == slate_editor.sizes.compact_control_height);
    assert(graph.port_spacing == slate_editor.sizes.control_height * 0.55f);

    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;

    input.begin_frame();
    ui.set_theme(ember_game);
    ui.begin(input, renderer);
    ui2::Panel panel{.bounds = {0, 0, 20, 10}};
    ui2::run(ui, panel);
    ui.end();

    assert(colors.size() >= 5);
    assert(colors[0].a < ember_game.palette.shadow.a);
    assert(std::any_of(colors.begin(), colors.end(), [&](Color color) { return color == ember_game.palette.surface; }));
    assert(rounded_fills >= 5);
    assert(rounded_outlines == 0);
    assert(ui.theme().palette.accent == ember_game.palette.accent);
}

// Size tokens are live: apply_theme propagates ThemeSizeTokens into widget size
// fields, different presets yield different widget sizes, containers default to
// the gap token, and the struct defaults (Palette / ThemeColorTokens / Theme)
// agree because they share one set of constants.
// The shipped glass theme frosts the overlay layer (and panels by default); the modal
// scrim stays a plain dim, and the tint is translucent-but-legible. Struct-level only,
// so it is backend-independent (glass rendering itself is covered by the SDL glass test).
void test_ui2_glass_theme() {
    const ui2::Theme g = ui2::glass_theme();
    assert(g.menu_surface.fill_kind == ui2::SurfaceFill::Glass);
    assert(g.popup_surface.fill_kind == ui2::SurfaceFill::Glass);
    assert(g.tooltip_surface.fill_kind == ui2::SurfaceFill::Glass);
    assert(g.panel_surface.fill_kind == ui2::SurfaceFill::Glass); // frost_panels defaults true
    assert(g.card_surface.fill_kind == ui2::SurfaceFill::Glass);
    assert(g.menu_surface.glass_blur > 0.0f);
    assert(g.menu_surface.glass_tint.a > 0 && g.menu_surface.glass_tint.a < 255);
    // G1: panels are more opaque than overlays (body-text legibility over frost),
    // and the readable text tiers get a subtle drop shadow.
    assert(g.panel_surface.glass_tint.a > g.menu_surface.glass_tint.a);
    assert(g.panel_surface.glass_tint.a < 255);
    assert(g.body_text.shadow_color.a > 0);
    // The full-screen modal scrim must stay a plain dim, not glass.
    assert(g.overlay_surface.fill_kind == ui2::SurfaceFill::Solid);

    // frost_panels=false: overlays still glass, panels stay solid.
    ui2::Theme base = ui2::game_theme();
    ui2::apply_glass(base, {.frost_panels = false});
    assert(base.menu_surface.fill_kind == ui2::SurfaceFill::Glass);
    assert(base.panel_surface.fill_kind == ui2::SurfaceFill::Solid);
}

void test_ui2_size_tokens_and_default_consistency() {
    const ui2::Theme game = ui2::game_theme();
    const ui2::Theme compact = ui2::radix_theme(ui2::radix::gray_dark,
                                                ui2::radix::lime_dark,
                                                ui2::StylePresetKind::Compact);

    ui2::ListView game_list;
    ui2::ListView compact_list;
    ui2::apply_theme(game, game_list);
    ui2::apply_theme(compact, compact_list);
    assert(game_list.row_height == game.sizes.compact_control_height);
    assert(compact_list.row_height == compact.sizes.compact_control_height);
    assert(game_list.row_height != compact_list.row_height); // presets actually change sizing

    ui2::Table table;
    ui2::TreeView tree;
    ui2::PropertyGrid grid;
    ui2::ColorPicker picker;
    ui2::MenuBar bar;
    ui2::apply_theme(game, table);
    ui2::apply_theme(game, tree);
    ui2::apply_theme(game, grid);
    ui2::apply_theme(game, picker);
    ui2::apply_theme(game, bar);
    assert(table.row_height == game.sizes.compact_control_height);
    assert(table.header_height == game.sizes.compact_control_height);
    assert(tree.row_height == game.sizes.compact_control_height);
    assert(tree.row_spacing == game.sizes.gap * 0.25f);
    assert(grid.row_height == game.sizes.compact_control_height);
    assert(grid.control_height < grid.row_height);
    assert(picker.row_height >= 16.0f);
    assert(bar.item_gap == game.sizes.gap * 0.25f);

    // Containers pick up the gap token unless the caller set spacing explicitly.
    assert(ui2::panel_container(game).spacing == game.sizes.gap);
    assert(ui2::card_container(game).spacing == game.sizes.gap);
    assert(ui2::menu_container(game, {.spacing = 3.0f}).spacing == 3.0f);

    // Default consistency: raw Palette{} and ThemeColorTokens{} share values.
    const ui2::Palette p{};
    const ui2::ThemeColorTokens t{};
    assert(p.background == t.app_background);
    assert(p.surface == t.surface_panel);
    assert(p.surface_alt == t.surface_card);
    assert(p.surface_hover == t.interactive_hovered);
    assert(p.surface_pressed == t.interactive_pressed);
    assert(p.surface_disabled == t.interactive_disabled);
    assert(p.text == t.text);
    assert(p.text_muted == t.text_muted);
    assert(p.text_disabled == t.text_disabled);
    assert(p.border == t.border);
    assert(p.border_strong == t.border_strong);
    assert(p.accent == t.solid_accent);
    assert(p.accent == t.focus);
    assert(p.accent_muted == t.interactive_selected);
    assert(p.success == t.solid_success);
    assert(p.warning == t.solid_warning);
    assert(p.danger == t.solid_danger);
    assert(p.info == t.solid_info);
    assert(p.overlay == t.surface_overlay);
    assert(p.shadow == t.shadow);
    // Default accent stays distinct from success (selection vs. success states readable).
    assert(!(p.accent == p.success));

    const ui2::Theme raw{};
    assert(raw.progress_fill == p.success);
    assert(raw.progress_background == p.surface_disabled);
    assert(raw.prompt_chip_border == p.border);
    assert(raw.invalid == p.danger);

    // P0 professional-look invariants: the Default preset is scale-derived and
    // matches the raw struct defaults; borders are alpha hairlines; emphasis is
    // a distinct tier above body text.
    const ui2::Palette& preset_default = ui2::default_palette();
    assert(preset_default.background == p.background);
    assert(preset_default.border == p.border);
    assert(preset_default.accent == p.accent);
    assert(preset_default.text_emphasis == p.text_emphasis);
    const ui2::Theme professional = ui2::default_theme();
    assert(professional.colors.border.a < 255);
    assert(professional.colors.border_strong.a < 255);
    assert(!(professional.colors.text_emphasis == professional.colors.text));
    assert(professional.preset.border_width == 1.0f);
}

void test_ui2_surface_border_shadow_and_layout_container() {
    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;
    ui2::Theme theme = ui2::game_theme(ui2::PalettePreset::Slate);

    const ui2::SurfaceStyle empty_surface{};
    assert(empty_surface.fill == colors::transparent);
    assert(empty_surface.border == colors::transparent);
    assert(empty_surface.border_width == 0.0f);
    assert(empty_surface.border_mode == ui2::BorderMode::None);
    assert(!empty_surface.draw_fill);

    input.begin_frame();
    ui.begin(input, renderer);
    ui.surface({0, 0, 20, 20}, empty_surface);
    ui.end();

    assert(fills.empty());
    assert(colors.empty());
    assert(rounded_fills == 0);
    assert(rounded_outlines == 0);

    input.begin_frame();
    ui.set_theme(theme);
    ui.begin(input, renderer);
    ui2::SurfaceStyle surface = theme.card_surface;
    surface.shadow.layers = 2;
    surface.shadow.spread = 4.0f;
    ui.surface({4, 4, 40, 20}, surface);
    ui.end();

    assert(colors.size() >= 3);
    assert(rounded_fills >= 4);
    assert(rounded_outlines == 0);

    fills.clear();
    colors.clear();
    rounded_fills = 0;
    rounded_outlines = 0;

    input.begin_frame();
    ui.begin(input, renderer);
    ui.begin_layout({0, 0, 100, 50});
    ui.begin_column(ui2::card_container(theme, ui2::column(ui2::grow(), ui2::grow())));
    ui.end_container();
    ui.end_layout();
    ui.end();

    assert(rounded_fills > 0);
    assert(rounded_outlines == 0);

    fills.clear();
    colors.clear();
    rounded_fills = 0;
    rounded_outlines = 0;

    input.begin_frame();
    ui.begin(input, renderer);
    Texture texture{std::make_shared<FakeTextureBackend>(Vec2i{12, 12})};
    ui2::SurfaceStyle skinned = theme.panel_surface;
    skinned.use_skin = true;
    skinned.shadow.enabled = false;
    skinned.skin = {.sprite = full_sprite(texture), .left = 3.0f, .top = 3.0f, .right = 3.0f, .bottom = 3.0f};
    ui.surface({0, 0, 30, 30}, skinned);
    ui.end();

    assert(fills.size() == 9);
    assert(rounded_fills == 0);
    assert(rounded_outlines == 0);
}

void test_ui2_theme_explicit_widget_overrides_win() {
    const ui2::Theme theme = ui2::game_theme(ui2::PalettePreset::Verdant);
    const Color explicit_fill = Color::rgb(1, 2, 3);

    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;

    input.begin_frame();
    ui.set_theme(theme);
    ui.begin(input, renderer);
    ui2::Button button{
        .id = ui2::make_id("explicit-button"),
        .bounds = {0, 0, 80, 24},
        .label = "Explicit",
        .style = {
            .surface = {
                .normal = {.fill = explicit_fill, .border = Color::rgb(10, 11, 12)},
                .hovered = {.fill = Color::rgb(4, 5, 6)},
                .pressed = {.fill = Color::rgb(7, 8, 9)},
                .focused = {.border = Color::rgb(13, 14, 15)},
                .disabled = {.fill = Color::rgb(16, 17, 18)},
            },
            .accent = Color::rgb(19, 20, 21),
            .track = Color::rgb(22, 23, 24),
            .padding = {3, 3, 3, 3},
        },
    };
    ui2::run(ui, button);
    ui.end();

    assert(colors.size() >= 5);
    assert(std::any_of(colors.begin(), colors.end(), [&](Color color) { return color == explicit_fill; }));
}

void test_ui2_widget_state_surfaces_drive_drawing() {
    const ui2::Theme theme = ui2::editor_theme(ui2::PalettePreset::Slate);

    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;
    ui.set_theme(theme);

    ui2::Button button{.id = ui2::make_id("state-button"), .bounds = {0, 0, 80, 24}, .label = "State"};
    const auto saw_color = [&colors](Color wanted) {
        return std::any_of(colors.begin(), colors.end(), [&](Color color) { return color == wanted; });
    };
    const auto frame = [&] {
        fills.clear();
        colors.clear();
        ui.begin(input, renderer);
        ui2::run(ui, button);
        ui.end();
    };

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    frame();
    assert(saw_color(theme.button.surface.normal.fill));

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    frame();
    input.begin_frame();
    input.set_mouse_pos({10, 10});
    frame();
    assert(saw_color(theme.button.surface.hovered.fill));

    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, true);
    frame();
    assert(saw_color(theme.button.surface.pressed.fill));

    button.enabled = false;
    input.begin_frame();
    input.set_mouse_pos({10, 10});
    input.set_mouse_held(MouseButton::Left, false);
    frame();
    assert(saw_color(theme.button.surface.disabled.fill));

    const Color slot_selected = Color::rgb(40, 50, 60);
    ui2::IconSlot slot{
        .id = ui2::make_id("state-slot"),
        .bounds = {0, 30, 40, 32},
        .style = {.surface = {.selected = {.fill = slot_selected}}},
        .selected = true,
    };
    fills.clear();
    colors.clear();
    input.begin_frame();
    input.set_mouse_pos({200, 200});
    ui.begin(input, renderer);
    ui2::run(ui, slot);
    ui.end();
    assert(saw_color(slot_selected));
}

void test_ui2_selected_row_uses_selected_surface() {
    const ui2::Theme theme = ui2::editor_theme(ui2::PalettePreset::Slate);
    const Color selected_fill = Color::rgb(3, 44, 88);

    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;

    ui2::ListView list{
        .id = ui2::make_id("selected-list"),
        .bounds = {0, 0, 120, 80},
        .items = {"A", "B", "C"},
        .selected = 1,
        .style = {.surface = {.selected = {.fill = selected_fill}}},
    };

    input.begin_frame();
    input.set_mouse_pos({200, 200});
    ui.set_theme(theme);
    ui.begin(input, renderer);
    ui2::run(ui, list);
    ui.end();

    assert(std::any_of(colors.begin(), colors.end(), [&](Color color) { return color == selected_fill; }));
}

void test_ui2_text_input_frame_uses_state_fill_tokens() {
    const ui2::Theme theme = ui2::editor_theme(ui2::PalettePreset::Slate);
    const Color hover_frame = Color::rgb(7, 8, 9);

    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;
    ui.set_theme(theme);

    ui2::TextInput text{
        .id = ui2::make_id("frame-text"),
        .bounds = {0, 0, 120, 24},
        .style = {.surface = {.hovered = {.fill = hover_frame}}},
    };

    input.begin_frame();
    input.set_mouse_pos({8, 8});
    ui.begin(input, renderer);
    ui2::run(ui, text);
    ui.end();

    fills.clear();
    colors.clear();
    input.begin_frame();
    input.set_mouse_pos({8, 8});
    ui.begin(input, renderer);
    ui2::run(ui, text);
    ui.end();

    assert(std::any_of(colors.begin(), colors.end(), [&](Color color) { return color == hover_frame; }));
}

void test_progress_bar_fill_uses_rounded_track_shape() {
    const ui2::Theme theme = ui2::game_theme(ui2::PalettePreset::Slate);

    Input input;
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    i32 rounded_fills = 0;
    i32 rounded_outlines = 0;
    Renderer2D renderer = make_surface_recording_renderer(fills, colors, rounded_fills, rounded_outlines);
    ui2::Context ui;

    input.begin_frame();
    ui.set_theme(theme);
    ui.begin(input, renderer);
    ui2::ProgressBar bar{
        .bounds = {0, 0, 100, 12},
        .value = 0.5f,
        .fill = Color::rgb(10, 200, 40),
        .background = Color::rgb(20, 20, 20),
    };
    ui2::run(ui, bar);
    ui.end();

    assert(rounded_fills > 0);
    assert(std::any_of(fills.begin(), fills.end(), [&](Rectf rect) { return rect_eq(rect, 0, 0, 100, 12); }));
    for (std::size_t i = 0; i < fills.size() && i < colors.size(); ++i) {
        assert(!(colors[i] == bar.fill && rect_eq(fills[i], 0, 0, 50, 12)));
    }
    assert(std::any_of(colors.begin(), colors.end(), [&](Color color) { return color == bar.fill; }));
}

void test_ecs_ui2_themed_builder_helpers() {
    kin::EcsWorld world;
    register_ui2_components(world);
    const ui2::Theme theme = ui2::game_theme(ui2::PalettePreset::Parchment);

    kin::EcsEntity panel = ui2_entity(world, "theme-panel").themed_panel(theme).entity();
    kin::EcsEntity label = ui2_entity(world, "theme-label").themed_label(theme, "Label").entity();
    kin::EcsEntity button = ui2_entity(world, "theme-button").themed_button(theme, "Go").entity();
    kin::EcsEntity danger = ui2_entity(world, "theme-danger").themed_danger_button(theme, "Delete").entity();
    kin::EcsEntity progress = ui2_entity(world, "theme-progress").themed_progress(theme, 0.5f).entity();

    assert(panel.get<ui2::Panel>()->color == theme.palette.surface);
    assert(label.get<ui2::Label>()->text_style.color == theme.body_text.color);
    assert(button.get<ui2::Button>()->style.accent == theme.palette.accent);
    assert(danger.get<ui2::Button>()->style.accent == theme.palette.danger);
    assert(progress.get<ui2::ProgressBar>()->fill == theme.progress_fill);
}

void test_ui2_surface_fill_variants_and_opacity() {
    std::vector<Rectf> fills;
    std::vector<Color> colors;
    Renderer2D renderer = make_color_recording_renderer(fills, colors);
    Input input;
    ui2::Context ctx;
    input.begin_frame();
    ctx.begin(input, renderer);

    // Shader fill degrades to the solid fill (no materials_2d backend).
    {
        ui2::SurfaceStyle s;
        s.draw_fill = true;
        s.fill_kind = ui2::SurfaceFill::Shader;
        s.fill = Color::rgba(10, 20, 30, 255);
        colors.clear();
        ctx.surface({0, 0, 10, 10}, s);
        assert(!colors.empty());
        assert(colors.back() == Color::rgba(10, 20, 30, 255));
    }
    // Glass fill degrades to the translucent tint (no render targets / B2 not built).
    {
        ui2::SurfaceStyle s;
        s.draw_fill = true;
        s.fill_kind = ui2::SurfaceFill::Glass;
        s.glass_tint = Color::rgba(80, 90, 100, 120);
        colors.clear();
        ctx.surface({0, 0, 10, 10}, s);
        assert(!colors.empty());
        assert(colors.back() == Color::rgba(80, 90, 100, 120));
    }
    // Gradient without the gradients capability → flat mid color.
    {
        ui2::SurfaceStyle s;
        s.draw_fill = true;
        s.fill_kind = ui2::SurfaceFill::Gradient;
        s.gradient = {Color::rgb(0, 0, 0), Color::rgb(255, 255, 255), GradientDirection::Vertical};
        colors.clear();
        ctx.surface({0, 0, 10, 10}, s);
        assert(!colors.empty());
        const Color mid = colors.back();
        assert(mid.r > 110 && mid.r < 150); // ~128
    }
    // Per-surface opacity halves the painted alpha.
    {
        ui2::SurfaceStyle s;
        s.draw_fill = true;
        s.fill = Color::rgba(200, 100, 50, 200);
        s.opacity = 0.5f;
        colors.clear();
        ctx.surface({0, 0, 10, 10}, s);
        assert(!colors.empty());
        const Color c = colors.back();
        assert(c.r == 200 && c.g == 100 && c.b == 50);
        assert(c.a >= 95 && c.a <= 105); // 200 * 0.5
    }

    ctx.end();
}

void test_ui2_soft_shadow_renders_on_sdl_backend() {
    // Real SDL backend (headless) — exercises the B3 baked-mask soft-shadow path,
    // which the identity/fake backend can't (no render targets → it degrades).
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "soft-shadow",
        .width = 128,
        .height = 128,
        .hidden = true,
    });
    Renderer2D renderer{window};
    renderer.set_logical_size(128, 128);
    assert(renderer.capabilities().render_targets);

    ui2::Context ctx;
    Input input;
    input.begin_frame();
    ctx.begin(input, renderer);
    renderer.clear(Color::rgb(180, 180, 180)); // light bg so the dark shadow is visible
    ui2::SurfaceStyle s;
    s.draw_fill = true;
    s.fill = Color::rgb(220, 220, 220);
    s.radius = 8.0f;
    s.shadow = {.color = Color::rgba(0, 0, 0, 220),
                .offset = {0.0f, 6.0f},
                .spread = 8.0f,
                .radius = 8.0f,
                .layers = 3,
                .enabled = true};
    ctx.surface({40.0f, 40.0f, 48.0f, 48.0f}, s); // rect bottom edge at y=88
    ctx.end();
    renderer.present();

    std::vector<u8> px;
    Vec2i size{};
    const bool ok = renderer.read_rgba({0.0f, 0.0f, 128.0f, 128.0f}, px, size);
    assert(ok);
    assert(size.x == 128 && size.y == 128);
    const auto lum_at = [&](int x, int y) -> int {
        const std::size_t idx = (static_cast<std::size_t>(y) * size.x + x) * 4u;
        return px[idx]; // grayscale, red channel suffices
    };
    const int background = lum_at(4, 4);              // far corner = clear color
    const int shadow_band = lum_at(64, 96);           // just below the rect, in the shadow
    assert(background > 150);                          // ~180 background
    assert(shadow_band < background - 15);             // shadow darkened the band
}

void test_ui2_glass_blurs_backdrop_on_sdl_backend() {
    // Real SDL backend (headless software still supports render targets) — exercises
    // the B2 frosted-glass overlay-readback path. A hard vertical backdrop edge
    // (dark left, light right) must be smeared by the blur so light bleeds left
    // past the edge; the flat-tint degrade (no render targets) would not bleed.
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({
        .title = "glass",
        .width = 128,
        .height = 128,
        .hidden = true,
    });
    Renderer2D renderer{window};
    renderer.set_logical_size(128, 128);
    assert(renderer.capabilities().render_targets);

    ui2::Context ctx;
    Input input;
    // Two frames: glass skips the readback on the first frame at a new output size
    // (resize safety), so it captures + blurs on the second stable frame.
    for (int frame = 0; frame < 2; ++frame) {
        input.begin_frame();
        ctx.begin(input, renderer);
        renderer.clear(Color::rgb(30, 30, 30));            // dark left half (cleared)
        renderer.fill_rect({64.0f, 0.0f, 64.0f, 128.0f}, Color::rgb(230, 230, 230)); // light right half

        ui2::SurfaceStyle g;
        g.draw_fill = true;
        g.fill_kind = ui2::SurfaceFill::Glass;
        g.glass_tint = Color::rgba(255, 255, 255, 28);     // faint tint so the backdrop dominates
        ctx.surface({32.0f, 32.0f, 64.0f, 64.0f}, g);      // panel straddles the x=64 edge
        ctx.end();
        renderer.present();
    }

    std::vector<u8> px;
    Vec2i size{};
    const bool ok = renderer.read_rgba({0.0f, 0.0f, 128.0f, 128.0f}, px, size);
    assert(ok);
    assert(size.x == 128 && size.y == 128);
    const auto lum_at = [&](int x, int y) -> int {
        const std::size_t idx = (static_cast<std::size_t>(y) * size.x + x) * 4u;
        return px[idx];
    };
    const int dark_plateau = lum_at(40, 64);           // well left of edge, inside panel
    const int light_plateau = lum_at(88, 64);          // well right of edge, inside panel
    const int left_of_edge = lum_at(58, 64);           // 6px left of edge
    assert(light_plateau > dark_plateau + 30);         // backdrop shows through (not uniform tint)
    assert(left_of_edge > dark_plateau + 8);           // blur bled light leftward past the edge
    assert(left_of_edge < light_plateau);              // still below the light plateau (a ramp)
}

// --- B6 state-transition animation ---------------------------------------------------

void test_ui2_resolve_animated_eases_between_states() {
    Renderer2D renderer = make_renderer();
    Input input;
    ui2::Context ui; // default theme has transition_duration > 0
    const f32 dur = ui.theme().transition_duration;
    assert(dur > 0.0f);

    const ui2::Id id = ui2::make_id("anim_btn");
    ui2::InteractiveSurfaceStyle style;
    const Color a{10, 20, 30, 255};
    const Color b{200, 180, 160, 255};
    style.normal.fill = a;
    style.hovered.fill = b;
    ui2::Interaction cold; // not hot
    ui2::Interaction hot;
    hot.hot = true;

    // Frame 1 (cold, dt=dur): a fresh id snaps to its target (normal = a).
    input.begin_frame();
    ui.begin(input, renderer, dur);
    const ui2::SurfaceStyle f1 = ui.resolve_animated(id, style, cold);
    ui.end();
    assert(f1.fill == a);

    // Frame 2 (hot, dt=dur/2): eases a -> b, landing strictly between.
    input.begin_frame();
    ui.begin(input, renderer, dur * 0.5f);
    const ui2::SurfaceStyle f2 = ui.resolve_animated(id, style, hot);
    ui.end();
    assert(f2.fill.r > a.r && f2.fill.r < b.r);
    assert(f2.fill.g > a.g && f2.fill.g < b.g);

    // Frame 3 (hot, dt=dur => t=1): reaches the target b.
    input.begin_frame();
    ui.begin(input, renderer, dur);
    const ui2::SurfaceStyle f3 = ui.resolve_animated(id, style, hot);
    ui.end();
    assert(f3.fill == b);

    // dt == 0 (the 2-arg begin / tests path): instant, returns target regardless of history.
    input.begin_frame();
    ui.begin(input, renderer, 0.0f);
    const ui2::SurfaceStyle f0 = ui.resolve_animated(id, style, cold); // target = normal = a
    ui.end();
    assert(f0.fill == a);
}

// --- B5 skin pack ---------------------------------------------------------------------

void test_ui2_apply_skin_pack_sets_surface_skins() {
    struct FakeTex : kin::ITextureBackend {
        kin::Vec2i size() const override { return {64, 64}; }
    };
    const kin::Texture tex{std::make_shared<FakeTex>()};
    const kin::ui2::UiNineSlice skin{kin::Sprite{tex, {0.0f, 0.0f, 64.0f, 64.0f}}, 8.0f, 8.0f, 8.0f, 8.0f};
    assert(skin.sprite.valid());

    kin::ui2::SkinPack pack;
    pack.panel = skin;
    pack.button = skin;
    pack.input = skin;

    kin::ui2::Theme theme = kin::ui2::default_theme();
    assert(!theme.button.surface.normal.use_skin);
    assert(!theme.card_surface.use_skin);

    kin::ui2::apply_skin_pack(theme, pack);

    assert(theme.button.surface.normal.use_skin);   // base
    assert(theme.button.surface.hovered.use_skin);  // falls back to base
    assert(theme.button.surface.pressed.use_skin);
    assert(theme.panel_surface.use_skin);
    assert(theme.card_surface.use_skin);
    assert(theme.input_surface.use_skin);

    // Expanded slots: a {panel, button, input} pack themes the whole widget set via fallbacks.
    assert(theme.danger_button.surface.normal.use_skin); // -> button
    assert(theme.menu.surface.normal.use_skin);          // -> panel
    assert(theme.menu_surface.use_skin);
    assert(theme.popup_surface.use_skin);
    assert(theme.tab.surface.normal.use_skin);           // -> button
    assert(theme.list_item.surface.normal.use_skin);     // -> menu -> panel
    assert(theme.row_surface.use_skin);

    // A distinct card skin overrides the panel fallback for cards only.
    const kin::ui2::UiNineSlice card_skin{kin::Sprite{tex, {0.0f, 0.0f, 64.0f, 64.0f}}, 12.0f, 12.0f, 12.0f, 12.0f};
    kin::ui2::Theme carded = kin::ui2::default_theme();
    kin::ui2::SkinPack card_pack;
    card_pack.card = card_skin; // only card set
    kin::ui2::apply_skin_pack(carded, card_pack);
    assert(carded.card_surface.use_skin);
    assert(carded.card_surface.skin.left == 12.0f);      // the card skin, not the panel one
    assert(!carded.panel_surface.use_skin);              // panel untouched (no panel skin given)

    // Indicator widgets — progress, scrollbar, meter slots.
    kin::ui2::SkinPack indicator_pack;
    indicator_pack.panel = skin;
    indicator_pack.button = skin;
    indicator_pack.progress = skin;
    indicator_pack.scrollbar_track = skin;
    indicator_pack.scrollbar_thumb = skin;
    indicator_pack.meter = skin;
    kin::ui2::Theme indicator = kin::ui2::default_theme();
    kin::ui2::apply_skin_pack(indicator, indicator_pack);
    assert(indicator.progress_track_surface.use_skin);
    assert(indicator.progress_fill_surface.use_skin);
    assert(indicator.scrollbar_track_surface.use_skin);
    assert(indicator.scrollbar_thumb_surface.use_skin);
    assert(indicator.meter_fill_surface.use_skin);
    assert(indicator.meter_empty_surface.use_skin);

    // Scrollbar track falls back to panel, thumb falls back to button when unset.
    kin::ui2::SkinPack fallback_pack;
    fallback_pack.panel = skin;
    fallback_pack.button = skin;
    kin::ui2::Theme fallback_theme = kin::ui2::default_theme();
    kin::ui2::apply_skin_pack(fallback_theme, fallback_pack);
    assert(fallback_theme.scrollbar_track_surface.use_skin);  // fallback to panel
    assert(fallback_theme.scrollbar_thumb_surface.use_skin);  // fallback to button
    assert(!fallback_theme.progress_track_surface.use_skin);  // progress: no fallback
    assert(!fallback_theme.meter_fill_surface.use_skin);      // meter: no fallback

    // An empty pack leaves surfaces flat (partial packs are allowed).
    kin::ui2::Theme flat = kin::ui2::default_theme();
    kin::ui2::apply_skin_pack(flat, kin::ui2::SkinPack{});
    assert(!flat.button.surface.normal.use_skin);
    assert(!flat.card_surface.use_skin);
    assert(!flat.menu.surface.normal.use_skin);
    assert(!flat.danger_button.surface.normal.use_skin);
    assert(!flat.progress_track_surface.use_skin);
    assert(!flat.scrollbar_thumb_surface.use_skin);
    assert(!flat.meter_fill_surface.use_skin);
}

void test_ui2_skinned_style() {
    struct FakeTex : kin::ITextureBackend {
        kin::Vec2i size() const override { return {64, 64}; }
    };
    const kin::Texture tex{std::make_shared<FakeTex>()};
    const auto frame = [&](kin::f32 m) {
        return kin::ui2::UiNineSlice{kin::Sprite{tex, {0.0f, 0.0f, 64.0f, 64.0f}}, m, m, m, m};
    };

    const kin::ui2::WidgetStyle base = kin::ui2::default_theme().button;
    assert(!base.surface.normal.use_skin);

    // Single-skin overload: every state gets the skin.
    const kin::ui2::WidgetStyle all = kin::ui2::skinned_style(base, frame(8.0f), kin::colors::white);
    assert(all.surface.normal.use_skin);
    assert(all.surface.hovered.use_skin);
    assert(all.surface.pressed.use_skin);
    assert(all.surface.focused.use_skin);
    assert(all.surface.disabled.use_skin);
    assert(all.surface.selected.use_skin);
    assert(all.surface.normal.skin.left == 8.0f);

    // Per-state WidgetSkin: unset states fall back (hovered->normal, selected->pressed).
    kin::ui2::WidgetSkin ws;
    ws.normal = frame(8.0f);
    ws.pressed = frame(16.0f);
    const kin::ui2::WidgetStyle perstate = kin::ui2::skinned_style(base, ws);
    assert(perstate.surface.normal.skin.left == 8.0f);
    assert(perstate.surface.hovered.skin.left == 8.0f);   // -> normal
    assert(perstate.surface.pressed.skin.left == 16.0f);
    assert(perstate.surface.selected.skin.left == 16.0f); // -> pressed

    // base is taken by value: the original is untouched.
    assert(!base.surface.normal.use_skin);
}

void test_ui2_theme_variant_registry() {
    struct FakeTex : kin::ITextureBackend {
        kin::Vec2i size() const override { return {64, 64}; }
    };
    const kin::Texture tex{std::make_shared<FakeTex>()};
    const kin::ui2::UiNineSlice skin{kin::Sprite{tex, {0.0f, 0.0f, 64.0f, 64.0f}}, 8.0f, 8.0f, 8.0f, 8.0f};

    kin::ui2::Theme theme = kin::ui2::default_theme();
    assert(theme.variant("gold_button") == nullptr); // empty by default

    theme.set_variant("gold_button", kin::ui2::skinned_style(theme.button, skin));
    const kin::ui2::WidgetStyle* v = theme.variant("gold_button");
    assert(v != nullptr);
    assert(v->surface.normal.use_skin);
    assert(theme.variant("missing") == nullptr);

    // Overwrite replaces the stored style.
    theme.set_variant("gold_button", theme.button); // unskinned
    assert(theme.variant("gold_button") != nullptr);
    assert(!theme.variant("gold_button")->surface.normal.use_skin);
}

// TTF text is laid out from the face's metrics and the font's kerning as
// HarfBuzz reads it (GPOS too, which TTF_GetGlyphKerning misses), the same
// walk for drawing and measuring. "AVAVAVAV" has seven kerned pairs where
// "AAAAVVVV" has one: it measures narrower. Lines stack; other text is still
// shaped by SDL_ttf.
void test_text_kerning_and_lines() {
    if (!ui2::system_ui_font_available()) {
        return;
    }
    for (const ui2::TextRendering rendering : {ui2::TextRendering::Bitmap, ui2::TextRendering::Sdf}) {
        const ui2::Font font = ui2::system_ui_font(16, rendering);
        const Vec2f kerned = ui2::measure_text(font, "AVAVAVAV", 1.0f);
        const Vec2f apart = ui2::measure_text(font, "AAAAVVVV", 1.0f);
        assert(kerned.x < apart.x - 2.0f);
        assert(std::abs(kerned.y - apart.y) < 1e-3f);

        const Vec2f one = ui2::measure_text(font, "Ab", 1.0f);
        const Vec2f two = ui2::measure_text(font, "Ab\nAb", 1.0f);
        assert(std::abs(two.x - one.x) < 1e-3f && two.y > one.y * 1.8f);
        assert(ui2::measure_text(font, "", 1.0f).y > 0.0f); // an empty line is a line high
        assert(ui2::measure_text(font, "na\xc3\xafve", 1.0f).x > 0.0f); // UTF-8: shaped by SDL_ttf
    }
}

// Sdf fonts measure linearly: twice the scale, exactly twice the size. Bitmap
// fonts keep only a few sizes however many are asked for. Outlines: one draw
// from an Sdf font's field, four stamped copies under the text otherwise.
void test_text_rendering_modes() {
    if (ui2::system_ui_font_available()) {
        const ui2::Font sdf = ui2::system_ui_font(16, ui2::TextRendering::Sdf);
        const Vec2f one = ui2::measure_text(sdf, "Scalable text", 1.0f);
        const Vec2f two = ui2::measure_text(sdf, "Scalable text", 2.0f);
        assert(one.x > 0.0f && std::abs(two.x - 2.0f * one.x) < 1e-3f && std::abs(two.y - 2.0f * one.y) < 1e-3f);
        // Close to the hinted Bitmap measure at the same size.
        const Vec2f bitmap = ui2::measure_text(ui2::system_ui_font(16), "Scalable text", 1.0f);
        assert(std::abs(one.x - bitmap.x) < bitmap.x * 0.05f);

        // A scale changing every frame: measured and drawn, no atlas or face per value kept.
        const ui2::Font animated = ui2::system_ui_font(15);
        std::vector<Rectf> fills;
        Renderer2D renderer = make_recording_renderer(fills);
        for (int i = 0; i < 40; ++i) {
            const f32 scale = 1.0f + static_cast<f32>(i) * 0.037f;
            assert(ui2::measure_text(animated, "Pulse", scale).x > 0.0f);
            ui2::draw_text(renderer, animated, "Pulse", {0, 0}, scale, colors::white);
        }
    }
    // On a backend without distance fields, an outline is the text stamped
    // four times under it: five draws of each glyph.
    std::vector<Rectf> fills;
    Renderer2D renderer = make_recording_renderer(fills);
    ui2::draw_text(renderer, ui2::bitmap_font(), "A", {0, 0}, 1.0f, colors::white);
    const std::size_t once = fills.size();
    fills.clear();
    ui2::draw_text_outlined(renderer, ui2::bitmap_font(), "A", {0, 0}, 1.0f, colors::white, 1.0f, colors::black);
    assert(once > 0 && fills.size() == once * 5);
}

} // namespace

int main() {
    test_text_rendering_modes();
    test_text_kerning_and_lines();
#if defined(_MSC_VER)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif


    test_solve_grow_and_spacing();
    test_solve_fit_autosize();
    test_solve_grow_weights();
    test_solve_margins_reserve_flow_space();
    test_solve_percent_sizes_against_parent_content();
    test_solve_grow_min_max_constraints();
    test_solve_overlay_excluded_from_fit();
    test_solve_overlay_grow_fills_content();
    test_solve_overlay_anchors();
    test_solve_overlay_anchor_offset_and_margin();
    test_solve_overlay_preserves_flow();
    test_solve_wrap_basic_gaps_and_oversized();
    test_solve_wrap_grow_stretch_line_cross_and_overlay();
    test_solve_wrap_fit_resolve_updates_parent();
    test_solve_grid_columns_gaps_grow_and_alignment();
    test_ids();
    test_measure_button();
    test_bitmap_font_draws_lowercase_fallback();
    test_label_alignment();
    test_ui2_overflow_diagnostics_are_opt_in();
    test_ui2_text_overflow_clip_limits_draw_region();
    test_ui2_draw_overflow_check();
    test_ui2_draw_overflow_corner_check();
    test_ui2_invariant_checks();
    test_ui2_contrast_check();
    test_ui2_pixel_causation();
    test_ui2_solitary_pixels();
    test_game_widget_measure();
    test_dialogue_view_render_and_choice();
    test_image_and_nine_slice_draw_geometry();
    test_meter_and_icon_button_geometry();
    test_icon_slot_measure_render_and_click();
    test_icon_slot_drag_drop_reporting();
    test_icon_button_click_cycle();
    test_input_mouse_edges_survive_zero_step_frames();
    test_ui2_click_survives_zero_step_frames();
    test_editor_widget_measure();
    test_ui2_control_height_alignment();
    test_scroll_view_geometry_and_wheel();
    test_scroll_primitives();
    test_text_input_editing();
    test_number_input_editing();
    test_combo_box_selection();
    test_color_picker_slider_changes_channel();
    test_icon_grid_layout_and_click();
    test_icon_grid_navigation();
    test_icon_grid_drag_drop_reports_cells();
    test_menu_list_and_context_menu();
    test_menu_list_mouse_wheel_scrolls_visible_rows();
    test_popup_placement_match_width_and_flips();
    test_popup_child_click_keeps_parent_open();
    test_menu_list_rich_rows_and_skip_navigation();
    test_popup_menu_submenu_and_menu_bar();
    test_overlay_and_hud_widgets_render_geometry();
    test_rich_text_and_text_area();
    test_collection_widget_measure();
    test_advanced_editor_widget_measure();
    test_property_inspector_reports_rows();
    test_node_graph_geometry_connection_and_arrows();
    test_tab_bar_splitter_and_dock_panel();
    test_tab_bar_fits_tight_tabs_inside_chrome_inset();
    test_breadcrumb_asset_status_and_log_widgets();
    test_list_view_keyboard_and_click();
    test_list_view_drag_drop_reports_rows();
    test_table_and_tree_view_render_state();
    test_table_uses_natural_row_height_for_tall_text();
    test_table_respects_larger_explicit_row_height();
    test_table_without_header_uses_body_from_top();
    test_table_frame_uses_flush_outline_and_rounded_header();
    test_table_square_header_and_bottom_stripe_trim();
    test_tree_view_drag_drop_reports_items();
    test_property_grid_bool_and_number_interaction();
    test_property_grid_zero_row_height_uses_natural_layout();
    test_property_grid_color_row_click_and_geometry();
    test_property_grid_color_row_popup_edits_color();
    test_region_click_cycle();
    test_region_occlusion();
    test_popup_clamps_and_closes_on_outside_click();
    test_modal_blocks_background_and_cancels();
    test_prompt_resolver_and_prompt_label_action();
    test_prompt_label_chip_and_prompt_row();
    test_world_overlay_projection_helpers();
    test_tooltip_delay_and_bounds();
    test_tooltip_warm_up();
    test_drag_source_and_drop_target();
    test_minimal_feedback_widgets();
    test_advanced_text_parse_layout_and_draw();
    test_advanced_text_link_interaction();
    test_advanced_text_polish_features();
    test_layout_end_to_end();
    test_ecs_bridge_button_click();
    test_ecs_bridge_layer_occlusion();
    test_ecs_layout_matches_solver();
    test_ecs_layout_button_click();
    test_ecs_layout_preserves_explicit_bounds_widgets();
    test_ecs_layout_multiple_roots();
    test_ecs_layout_overlay_panel_and_centered_column();
    test_ui2_layout_presets();
    test_ecs_ui2_builder_components_and_hierarchy();
    test_ecs_ui2_builder_widget_methods();
    test_ecs_ui2_text_keys();
    test_right_to_left_layout();
    test_ecs_ui2_builder_button_click();
    test_ui2_theme_resolution_and_context_defaults();
    test_ui2_glass_theme();
    test_ui2_size_tokens_and_default_consistency();
    test_ui2_surface_border_shadow_and_layout_container();
    test_ui2_surface_fill_variants_and_opacity();
    test_ui2_resolve_animated_eases_between_states();
    test_ui2_apply_skin_pack_sets_surface_skins();
    test_ui2_skinned_style();
    test_ui2_theme_variant_registry();
    test_ui2_soft_shadow_renders_on_sdl_backend();
    test_ui2_glass_blurs_backdrop_on_sdl_backend();
    test_ui2_theme_explicit_widget_overrides_win();
    test_ui2_widget_state_surfaces_drive_drawing();
    test_ui2_selected_row_uses_selected_surface();
    test_ui2_text_input_frame_uses_state_fill_tokens();
    test_progress_bar_fill_uses_rounded_track_shape();
    test_ecs_ui2_themed_builder_helpers();
    test_ecs_bridge_game_widgets_layout_and_records();
    test_ecs_bridge_editor_widgets_layout_and_records();
    test_ecs_bridge_remaining_basic_widgets_and_scroll_container();
    test_ecs_bridge_static_dynamic_filtering();
    test_ecs_sync_stable_keys_sweep_and_state();
    test_ecs_sync_explicit_keys_survive_reorder();
    test_ecs_lazy_layout_solve_cache_and_dirty_mark();
    test_ecs_sync_matches_builder_layout();
    test_ecs_sync_document_order_survives_recreate();
    test_ecs_sync_text_style_change_remeasures();
    return 0;
}
