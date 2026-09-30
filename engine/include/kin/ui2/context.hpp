#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/render_view.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/layout.hpp>
#include <kin/ui2/state.hpp>
#include <kin/ui2/theme.hpp>
#include <kin/ui2/widgets.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace kin::ui2 {

// One persistent Context per UI. Drives interaction (region), layout (the scope
// builder), and drawing. Create once; call begin()/.../end() each frame.
class Context {
public:
    Context();

    struct ModalOptions {
        Color overlay = Color::rgba(0, 0, 0, 144);
        i32 z = 15000;
        bool close_on_escape = true;
        bool close_on_outside_click = false;
        bool block_background_input = true;
    };

    struct ModalResult {
        bool open = false;
        bool opened = false;
        bool closed = false;
        bool cancelled = false;
        bool outside_clicked = false;
        Rectf bounds{};
    };

    struct PopupOptions {
        Vec2f size{};
        Vec2f offset{0.0f, 4.0f};
        UiAnchor anchor = UiAnchor::BottomLeft;
        Rectf screen{};
        i32 z = 10000;
        bool close_on_outside_click = true;
        bool flip_x = false;
        bool flip_y = false;
        bool match_anchor_width = false;
    };

    struct PopupResult {
        bool open = false;
        bool opened = false;
        bool closed = false;
        bool outside_clicked = false;
        Rectf bounds{};
    };

    struct TooltipOptions {
        u64 delay_frames = 30;
        // Once a tooltip has shown, another shows at once if hovered within this
        // many frames, so moving along a row of controls does not wait each time.
        u64 warm_frames = 20;
        UiPadding padding{8.0f, 5.0f, 8.0f, 5.0f};
        Vec2f offset{8.0f, 8.0f};
        TextStyle text_style{};
        Color fill = Color::rgba(7, 9, 14, 240);
        Color border = Color::rgb(78, 88, 110);
        i32 z = 20000;
    };

    struct TooltipResult {
        bool visible = false;
        Rectf bounds{};
    };

    struct DragPayload {
        std::string type;
        std::string text;
        u64 value = 0;
    };

    struct DragSourceResult {
        Interaction interaction{};
        bool started = false;
        bool dragging = false;
        bool released = false;
    };

    struct DropTargetResult {
        bool hot = false;
        bool accepts = false;
        bool dropped = false;
        DragPayload payload{};
    };

    struct NavResult {
        i32 index = 0;
        bool changed = false;
        bool activated = false;
        bool cancelled = false;
    };

    struct DebugOptions {
        // Logical overflow: a widget's measured content is larger than the bounds it was given.
        bool detect_overflow = false;
        bool log_overflow = false;
        // Draw overflow: a widget paints pixels outside the draw scope it declared (see
        // draw_scope()). The check uses the *actually painted* region (draw rect intersected
        // with the active clip), so virtualized rows that rely on clipping do not trip it.
        bool detect_draw_overflow = false;
        bool log_draw_overflow = false;
        // Frame invariants: unbalanced clip/scope stacks at end(), duplicate interaction ids
        // within a frame, and non-finite (NaN/inf) draw coordinates. Cheap, pure-signal sanity.
        bool detect_invariants = false;
        bool log_invariants = false;
        // WCAG contrast: flag text whose colour fails the AA contrast ratio against its
        // background (the last opaque fill drawn). Threshold relaxes for large text.
        bool detect_contrast = false;
        bool log_contrast = false;
        // Draw recording: capture every primitive (geometry, colour, owning component, shadow
        // flag, clip) in order so pixel_causation() can attribute any pixel to its draw stack.
        bool record_draws = false;
    };

    enum class DiagnosticKind {
        Overflow,        // measured content exceeded bounds
        DrawOverflow,    // painted pixels escaped the widget's draw scope
        UnbalancedStack, // a clip/scope push had no matching pop by end()
        DuplicateId,     // two interactions shared an id within one frame
        DegenerateRect,  // a draw had non-finite (NaN/inf) coordinates
        LowContrast,     // text failed the WCAG AA contrast ratio against its background
    };

    struct Diagnostic {
        DiagnosticKind kind = DiagnosticKind::Overflow;
        std::string widget;
        std::string detail;
        Rectf bounds{};
        Vec2f wanted{};
        Vec2f overflow{};
    };

    // One recorded draw primitive (see DebugOptions::record_draws). For Line, `rect` stores the
    // segment as start (x,y) and delta (w,h); for everything else it is the draw rectangle.
    struct DrawOp {
        enum class Kind { Fill, RoundedFill, Outline, RoundedOutline, Line, Sprite, Texture, Text, Gradient };
        Kind kind = Kind::Fill;
        Rectf rect{};
        f32 radius = 0.0f;
        f32 width = 0.0f; // outline stroke width
        Color color{};
        bool shadow = false;  // painted while drawing a drop shadow
        bool clipped = false; // a clip was active
        Rectf clip{};
        std::string widget; // owning draw scope, if any
    };

    // One layer that touched a queried pixel (pixel_causation). `source` is the layer's own
    // colour; `composite` is the running src-over result after applying it.
    struct PixelLayer {
        std::string widget;
        std::string role; // "fill" | "shadow" | "border" | "text" | "sprite" | "texture" | "gradient" | "line"
        Color source{};
        Color composite{};
    };
    struct PixelCausation {
        Vec2f point{};
        Color result{};               // final composite over a transparent base
        std::vector<PixelLayer> layers; // bottom-to-top draw order
    };

    // Attribute a pixel to the ordered stack of recorded draws that painted it. Requires the
    // frame to have been drawn with debug_options().record_draws set; query after end() and
    // before the next begin() (which clears the recording).
    PixelCausation pixel_causation(Vec2f point) const;

    // RAII scope declaring that everything drawn while it is alive belongs to `bounds`.
    // While debug_options().detect_draw_overflow is set, any primitive whose painted region
    // escapes `bounds` is reported (and optionally logged). Move-only; pop on destruction.
    class DrawScope {
    public:
        DrawScope() = default;
        explicit DrawScope(Context* ctx) : _ctx(ctx) {}
        ~DrawScope() {
            if (_ctx) {
                _ctx->pop_draw_scope();
            }
        }
        DrawScope(DrawScope&& other) noexcept : _ctx(other._ctx) { other._ctx = nullptr; }
        DrawScope& operator=(DrawScope&& other) noexcept {
            if (this != &other) {
                if (_ctx) {
                    _ctx->pop_draw_scope();
                }
                _ctx = other._ctx;
                other._ctx = nullptr;
            }
            return *this;
        }
        DrawScope(const DrawScope&) = delete;
        DrawScope& operator=(const DrawScope&) = delete;

    private:
        Context* _ctx = nullptr;
    };

    void begin(Input& input, Renderer2D& renderer);
    // Overload that threads the frame delta-time for state-transition animation (B6).
    // The 2-arg form forwards dt = 0 → instant (snap), preserving existing callers/tests.
    void begin(Input& input, Renderer2D& renderer, f32 dt);
    void end();

    State& state() { return _state; }
    const State& state() const { return _state; }
    void set_theme(const Theme& theme) { _theme = theme; }
    Theme& theme() { return _theme; }
    const Theme& theme() const { return _theme; }
    void set_debug_options(DebugOptions options) { _debug = options; }
    DebugOptions debug_options() const { return _debug; }
    const std::vector<Diagnostic>& diagnostics() const { return _diagnostics; }
    void report_overflow(std::string_view widget, Rectf bounds, Vec2f wanted, std::string_view detail = {});

    // Declare a draw scope: subsequent draws should stay within `bounds` (its parent box).
    // Prefer the RAII form `auto scope = ctx.draw_scope("Table", widget.bounds);` at the top
    // of a widget's run(). The manual push/pop are exposed for the RAII type and rare callers.
    //
    // `corner_radius` is the panel's outer corner radius. When > 0, square (non-rounded)
    // primitives are additionally checked against the four corner arcs, catching content that
    // pokes past a rounded corner while still inside the bounding rectangle (square-over-rounded).
    [[nodiscard]] DrawScope draw_scope(std::string_view widget, Rectf bounds, f32 corner_radius = 0.0f);
    void push_draw_scope(std::string_view widget, Rectf bounds, f32 corner_radius = 0.0f);
    void pop_draw_scope();

    // Interaction core — the single resolver. `z` is depth (higher == on top).
    // `button` selects which mouse button drives pressed/released/active; defaults to Left.
    // Note: `active` state is shared across buttons — only one widget can be active at a time.
    Interaction region(Id id, Rectf bounds, i32 z = 0, MouseButton button = MouseButton::Left);

    void open_modal(Id id);
    void close_modal(Id id);
    bool modal_open(Id id) const;
    ModalResult begin_modal(Id id, Rectf bounds, ModalOptions options);
    ModalResult begin_modal(Id id, Rectf bounds) { return begin_modal(id, bounds, ModalOptions{}); }
    void end_modal();

    void open_popup(Id id);
    void open_subpopup(Id parent, Id id);
    void close_popup(Id id);
    bool popup_open(Id id) const;
    bool popup_hovered_or_open(Id id) const;
    PopupResult begin_popup(Id id, Rectf anchor, PopupOptions options);
    void end_popup();

    TooltipResult tooltip(Id id, Rectf anchor, std::string_view text, TooltipOptions options);
    TooltipResult tooltip(Id id, Rectf anchor, std::string_view text) {
        return tooltip(id, anchor, text, TooltipOptions{});
    }

    DragSourceResult drag_source(Id id, Rectf bounds, DragPayload payload, i32 z = 0);
    DropTargetResult drop_target(Id id, Rectf bounds, std::string_view accepted_type, i32 z = 0);
    NavResult nav_index(i32 count, i32 current, i32 columns = 1, bool wrap = true);
    std::string prompt_for_action(std::string_view action) const;
    std::string prompt_for_action(std::string_view action, const PromptOptions& options) const;
    bool dragging() const { return _drag.dragging; }
    const DragPayload& drag_payload() const { return _drag.payload; }

    // Pointer / input access in logical space.
    Vec2f pointer() const;
    bool pointer_pressed(MouseButton button = MouseButton::Left) const;
    bool pointer_held(MouseButton button = MouseButton::Left) const;
    bool pointer_released(MouseButton button = MouseButton::Left) const;
    f32 mouse_wheel_y() const;
    bool key_pressed(Key key) const;
    // Pressed this frame, or auto-repeated while held: for keys that act again
    // while held, such as a text caret's arrows and Backspace.
    bool key_typed(Key key) const;
    bool action_pressed(std::string_view action) const;
    bool modifier_held(KeyModifiers modifiers) const;
    std::string_view text_input() const;
    bool wants_text_input() const { return _wants_text_input; }
    void request_text_input() { _wants_text_input = true; }
    std::string clipboard_text() const;
    void set_clipboard_text(std::string_view text);
    UiTextInputState& text_input_state(Id id, std::string_view value = {});
    // A TextEdit's state kept per id; `initial` is its text the first time only.
    UiTextEditState& text_edit_state(Id id, std::string_view initial = {});
    UiComboState& combo_state(Id id, i32 selected = 0);
    ColorPickerMode& color_picker_mode(Id id, ColorPickerMode mode = ColorPickerMode::Hsv);

    // Immediate draw helpers (logical space).
    void push_clip(Rectf bounds);
    void pop_clip();
    void fill_rect(Rectf rect, Color color);
    void outline_rect(Rectf rect, Color color);
    void outline_rect(Rectf rect, Color color, f32 width);
    void fill_rounded_rect(Rectf rect, f32 radius, Color color);
    void fill_gradient_rect(Rectf rect, const Gradient& gradient);
    void outline_rounded_rect(Rectf rect, f32 radius, Color color, f32 width = 1.0f);
    void surface(Rectf rect, const SurfaceStyle& style);

    // Resolve an interactive surface for `id`, easing between interaction states over
    // the theme's transition duration (B6). Degrades to resolve() (instant) when dt==0
    // (the 2-arg begin / tests), duration==0, or id is null, and snaps on first use.
    SurfaceStyle resolve_animated(Id id, const InteractiveSurfaceStyle& style, const Interaction& it,
                                  bool selected = false, bool enabled = true);
    // Draws a layout node's container surface (skin/surface, or background + border).
    // Shared by both front-ends so a container draws identically immediate vs ECS.
    void draw_layout_surface(Rectf bounds, const LayoutStyle& style);
    void line(Vec2f a, Vec2f b, Color color);
    void texture(const Texture& texture, Rectf bounds, Color tint = colors::white);
    // corner_exempt: skip the per-corner overflow test (set when the sprite is the fill
    // of a rounded surface, e.g. the frosted-glass backdrop, where the rect sprite
    // intentionally covers the rounded rect and a rounded tint is composited on top).
    void sprite(const Sprite& sprite, Rectf bounds, Color tint = colors::white, bool corner_exempt = false);
    void text(std::string_view value, Vec2f pos, const TextStyle& style);
    Renderer2D* renderer() { return _renderer; }

    // Immediate-mode layout scope builder. Widgets handed to widget() must stay alive
    // until end_layout(), which solves the tree, assigns bounds, and runs each widget.
    void begin_layout(Rectf root);
    void begin_row(LayoutStyle style = {});
    void begin_wrap_row(LayoutStyle style = {});
    void begin_column(LayoutStyle style = {});
    void begin_grid(i32 columns, LayoutStyle style = {});
    void end_container();

    // `w` must outlive end_layout() (the write-back `w = snap` at that point is the only
    // remaining lifetime requirement; the run itself works from a copy taken here).
    template <typename W>
    void widget(W& w, LayoutStyle style = {}) {
        const i32 index = add_node(style, measure(w));
        _pending.push_back({index, [&w, snap = w](Context& ctx, Rectf solved) mutable {
                                snap.bounds = solved;
                                run(ctx, snap);
                                w = snap; // write results (clicked, interaction, …) back
                            }});
    }

    void end_layout();

private:
    struct Pending {
        i32 node = -1;
        std::function<void(Context&, Rectf)> finalize;
    };

    i32 add_node(LayoutStyle style, Vec2f intrinsic);
    void begin_container(LayoutStyle style);

    // Lazily bake (and cache) a normalized premultiplied soft-shadow coverage mask
    // for the given corner radius + blur margin. Returns {} if render targets are
    // unavailable (caller degrades to the layered-rect shadow).
    Texture shadow_mask(f32 corner, f32 margin, f32 soft);

    // Lazily capture + blur the frame-so-far into a per-frame backdrop for frosted
    // glass (B2). Returns true if a blurred backdrop is available this frame.
    // Captures once per frame on the first glass surface; degrades (returns false)
    // when render targets are unavailable. One GPU->CPU readback per frame with glass.
    bool ensure_glass_backdrop();

    // Reports any painted region (drawn rect ∩ active clip) that escapes the current draw scope.
    // `corner_exempt` skips the per-corner arc test (still does the rectangular check): set for
    // primitives that already follow the corner radius (rounded fills/outlines) and for text,
    // whose measured line-box includes leading that the glyph ink never fills.
    void check_draw(std::string_view kind, Rectf rect, bool corner_exempt = false);
    Color debug_fill_color(Rectf rect);                       // stable wireframe color for a fill
    void record_draw(DrawOp::Kind kind, Rectf rect, f32 radius, f32 width, Color color);
    void check_degenerate(std::string_view kind, Rectf rect); // NaN/inf draw coordinates
    void check_region_id(Id id);                              // duplicate interaction id this frame
    void emit_diagnostic(DiagnosticKind kind, std::string_view widget, std::string detail, bool log);

    struct DrawScopeEntry {
        std::string widget;
        Rectf bounds{};
        f32 radius = 0.0f;
    };

    Input* _input = nullptr;
    Renderer2D* _renderer = nullptr;
    State _state;
    std::unordered_map<u64, UiTextInputState> _text_inputs;
    std::unordered_map<u64, UiTextEditState> _text_edits;
    std::unordered_map<u64, UiComboState> _combos;
    std::unordered_map<u64, ColorPickerMode> _color_picker_modes;
    // State-transition animation (B6): the eased surface chasing each widget's target,
    // keyed by widget Id; persists across frames, GC'd in begin() when stale.
    struct SurfaceAnim {
        SurfaceStyle current;
        u64 last_frame = 0;
    };
    std::unordered_map<u64, SurfaceAnim> _surface_anim;
    f32 _dt = 0.0f; // frame delta-time for animation; 0 = instant
    // Baked soft-shadow coverage masks (B3), keyed by quantized (corner, margin).
    // Cross-frame cache; not cleared per frame.
    std::unordered_map<u64, RenderTarget> _shadow_masks;
    // Per-frame frosted-glass backdrop (B2): blurred snapshot of the frame-so-far.
    // Reset each begin(); filled lazily by ensure_glass_backdrop() on first glass.
    PooledTarget _glass_backdrop;
    Rectf _glass_capture_region{};            // logical region the backdrop covers
    Vec2i _glass_capture_px{};                // pixel size of the captured backdrop
    Vec2i _glass_last_output{};               // last frame's output size (skip readback while resizing)
    bool _glass_captured = false;             // capture attempted this frame
    Theme _theme;
    DebugOptions _debug{};
    std::vector<Diagnostic> _diagnostics;
    std::vector<DrawScopeEntry> _draw_scopes; // declared widget bounds; check_draw tests against back()
    std::vector<Rectf> _clip_stack;           // effective (intersected) clip per push_clip
    std::vector<DrawOp> _draw_ops;            // per-frame draw recording for pixel_causation
    std::unordered_set<u64> _frame_region_ids; // interaction ids seen this frame (duplicate check)
    std::unordered_set<u64> _frame_dup_reported; // dup ids already reported this frame (dedup)
    bool _in_shadow = false;                  // suppresses check_draw while painting drop shadows
    f32 _debug_last_hue = 0.0f;               // hue of the last debug-tinted fill (font uses its complement)
    Color _surface_fill_under{};              // last opaque fill, used as text background for contrast
    bool _wants_text_input = false;

    std::vector<LayoutNode> _nodes;
    std::vector<i32> _stack;
    std::vector<Pending> _pending;
    LayoutScratch _layout_scratch; // reused across end_layout() calls, avoids per-frame alloc
    Rectf _root{};

    Id _open_popup{};
    Id _open_subpopup{};
    Id _popup_opened_this_frame{};
    std::vector<Rectf> _popup_stack;
    std::vector<Rectf> _popup_frame_bounds;
    std::vector<Rectf> _popup_last_bounds;

    Id _open_modal{};
    Id _modal_opened_this_frame{};
    Rectf _modal_bounds{};
    bool _modal_blocking = false;
    std::vector<Rectf> _modal_stack;

    Id _tooltip_candidate{};
    u64 _tooltip_first_frame = 0;
    std::optional<u64> _tooltip_shown_frame; // last frame any tooltip was visible
    bool _tooltip_warm = false;              // the candidate skips the delay

    struct DragState {
        Id source{};
        Vec2f start{};
        DragPayload payload{};
        bool dragging = false;
        bool started_this_frame = false;
        bool released_this_frame = false;
    };
    DragState _drag;
};

SurfaceStyle resolve(const InteractiveSurfaceStyle& style, const Interaction& it, bool selected = false, bool enabled = true);

struct WorldOverlayContext {
    const Camera2D* camera = nullptr;
    Rectf viewport{};
};

Vec2f world_to_ui(const WorldOverlayContext& overlay, Vec2f world);
Rectf world_anchor_rect(const WorldOverlayContext& overlay, Vec2f world, Vec2f size, UiAnchor anchor = UiAnchor::BottomCenter, Vec2f offset = {});
bool world_visible(const WorldOverlayContext& overlay, Rectf world_bounds);
Rectf clamp_to_viewport(const WorldOverlayContext& overlay, Rectf bounds);

struct SolitaryPixel {
    Vec2i pos;     // pixel coordinate in the scanned buffer
    Color color{}; // the outlier pixel's colour
};

struct SolitaryPixelOptions {
    // Summed |R|+|G|+|B| distance the centre pixel must exceed from its neighbourhood average.
    i32 threshold = 90;
    // Max summed distance each of the 8 neighbours may sit from their own average for the
    // neighbourhood to count as "uniform" (so edges/AA/lines/text are skipped, not flagged).
    i32 uniformity = 36;
};

// Scans an RGBA8 framebuffer (row-major, 4 bytes/pixel) for solitary pixels: a pixel that is a
// strong colour outlier inside an otherwise-uniform 8-neighbourhood. These are almost always
// rendering bugs (stray dots, line-endpoint overhangs). Pair the results with
// Context::pixel_causation() to attribute each one. Returns at most `max_results` hits.
std::vector<SolitaryPixel> find_solitary_pixels(const u8* rgba, i32 width, i32 height,
                                                SolitaryPixelOptions options = {}, std::size_t max_results = 256);

} // namespace kin::ui2
