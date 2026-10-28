#pragma once

#include <kin/core/types.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/window.hpp>
#include <kin/renderer/render_view.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/layout.hpp>
#include <kin/ui2/scale.hpp>
#include <kin/ui2/state.hpp>
#include <kin/ui2/theme.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kin {
class App;
}

namespace kin::ui2 {

// One persistent Context per UI. Drives interaction (region), layout (the scope
// builder), and drawing. Create once; call begin()/.../end() each frame.
// A widget whose content did not fit its bounds (Context::report_overflow),
// gathered from every Context: once per widget and text, with how many
// times it was seen.
struct OverflowRecord {
    std::string widget;
    std::string detail; // the text, for text widgets
    Rectf bounds{};
    Vec2f wanted{};
    u64 seen = 0;
};
// While on, every Context's overflows are gathered whatever its debug
// options (run_scene_app turns this on for its run report). At most 256.
void collect_overflows(bool enabled);
bool collecting_overflows();
std::vector<OverflowRecord> collected_overflows();
void clear_collected_overflows();

// Composition (experimental): open containers for the immediate layout builder.
// A box is a layout node with a surface, a hit region and any children. It is
// hit-tested against where it was drawn last frame (so it can answer hovered and
// clicked while its children are still being declared), and drawn at end_layout().
struct BoxStyle {
    LayoutStyle layout{};
    InteractiveSurfaceStyle surface{}; // `hovered` shows while the pointer is over the box or its children
    bool draw_surface = false;
    bool interactive = true; // registers a hit region (hot/clicked); false = layout and drawing only
    bool clip = false;       // children are clipped to the box
    // Clip to the box's rounded corners (surface.normal.radius). A path clip draws
    // through two layers, a render pass each: use it where the corners show.
    bool clip_rounded = false;
    bool selected = false;
    bool enabled = true;
    // In the Tab order (by tab_index, then as built); Enter or Space while focused clicks it.
    bool focusable = false;
    bool traps_focus = false; // focus_trap()
    // Shown by the pointer resting on the box (tooltip()), in the Context's
    // tooltip style. Read while the box is built: a view is enough.
    std::string_view tooltip_text{};
    // Dragged (draggable()): a press moved 4 units starts a drag carrying these
    // (DragPayload: type, text, value), its text shown by the pointer. A box
    // that accepts_drop() the type is told when it is over it and let go.
    std::string_view drag_type{};
    std::string_view drag_text{};
    u64 drag_value = 0;
    std::string_view drop_type{};
    i32 tab_index = 0;
    // Key bindings with this context apply while focus is in the box (see Context::bind).
    std::string_view key_context{};
    // The pointer's shape over the box and its children (Default: the parent's).
    SystemCursor cursor = SystemCursor::Default;
    // Pressing it moves the window, as a system title bar does (except over the
    // interactive boxes inside it): a window that draws its own (borderless).
    bool window_drag = false;
    i32 z = 0;

    // Shorthands that chain (start from hbox() or vbox()):
    //   ui.box(id, ui::hbox().w(ui::grow()).h(22).pad(8, 0).gap(6).items(ui::UiAlign::Center)
    //                  .hover_bg(hover).selected_bg(accent).selected_if(row == current))
    // The layout ones are LayoutStyle's.
    BoxStyle& row() { layout.row(); return *this; }
    BoxStyle& col() { layout.col(); return *this; }
    BoxStyle& w(auto value) { layout.w(value); return *this; }
    BoxStyle& h(auto value) { layout.h(value); return *this; }
    BoxStyle& size(f32 w_px, f32 h_px) { layout.size(w_px, h_px); return *this; }
    BoxStyle& fill() { layout.fill(); return *this; }
    BoxStyle& pad(auto... values) { layout.pad(values...); return *this; }
    BoxStyle& margins(auto... values) { layout.margins(values...); return *this; }
    BoxStyle& gap(f32 value) { layout.gap(value); return *this; }
    BoxStyle& justify(UiAlign align) { layout.justify(align); return *this; }
    BoxStyle& items(UiAlign align) { layout.items(align); return *this; }
    BoxStyle& center() { layout.center(); return *this; }
    BoxStyle& wrapped() { layout.wrapped(); return *this; }
    BoxStyle& grid(i32 columns) { layout.grid(columns); return *this; }
    BoxStyle& at(UiAnchor where, Vec2f offset = {}) { layout.at(where, offset); return *this; }
    BoxStyle& on_layer(i32 value) { layout.on_layer(value); return *this; }
    // Next to the parent box instead of in it, over everything (see LayoutStyle::floats):
    //   if (open) { auto menu = ui.box(id, ui::vbox().floating(ui::UiFloat::Below)...); }
    // A floating box and what is in it are hit before anything under them.
    BoxStyle& shrinks() { layout.shrinks(); return *this; }
    BoxStyle& floating(UiFloat side = UiFloat::Below, Vec2f offset = {}) { layout.floating(side, offset); return *this; }
    // A fill in every state not given its own with hover_bg() and the like
    // (before or after this).
    BoxStyle& bg(Color color);
    BoxStyle& hover_bg(Color color);    // while hovered or pressed
    BoxStyle& pressed_bg(Color color);
    BoxStyle& selected_bg(Color color); // see selected_if()
    BoxStyle& rounded(f32 radius);      // every state
    BoxStyle& border(Color color, f32 width = 1.0f);
    BoxStyle& focus_border(Color color, f32 width = 1.0f);
    BoxStyle& passive() { interactive = false; return *this; } // no hit region
    BoxStyle& clipped(bool to_corners = false) { clip = true; clip_rounded = to_corners; return *this; }
    BoxStyle& selected_if(bool value) { selected = value; return *this; }
    BoxStyle& disabled_if(bool value) { enabled = !value; return *this; }
    BoxStyle& tab_stop(i32 index = 0) { focusable = true; tab_index = index; return *this; }
    // A dialog: while it is built, Tab goes round what is in it, and focus left
    // behind it is dropped (Enter does not click a button under it).
    BoxStyle& focus_trap() { traps_focus = true; return *this; }
    BoxStyle& tooltip(std::string_view text) { tooltip_text = text; return *this; }
    BoxStyle& draggable(std::string_view type, std::string_view text = {}, u64 value = 0) {
        drag_type = type;
        drag_text = text;
        drag_value = value;
        return *this;
    }
    BoxStyle& accepts_drop(std::string_view type) { drop_type = type; return *this; }
    BoxStyle& key_scope(std::string_view context) { key_context = context; return *this; }
    BoxStyle& cursor_shape(SystemCursor shape) { cursor = shape; return *this; }
    BoxStyle& depth(i32 value) { z = value; return *this; }
    BoxStyle& drags_window() { window_drag = true; return *this; }
};

// How Context::svg() draws an icon.
struct SvgOptions {
    // Texels per unit; 0: the renderer's content scale (its pixels per unit),
    // so an icon is as sharp as the text on a high-density display.
    f32 oversample = 0.0f;
    bool mask = false;     // only its shape counts, in the tint: a one-colour glyph (gpui's svg())
};

inline BoxStyle hbox() { // children side by side
    BoxStyle style;
    style.row();
    return style;
}
inline BoxStyle vbox() { // children top to bottom
    BoxStyle style;
    style.col();
    return style;
}

struct BoxResult : Interaction {
    // The pointer is over the box, including over its children. `hot` is exclusive
    // (the innermost box under the pointer); `hovered` is not.
    bool hovered = false;
    bool double_clicked = false;    // a second click soon after the first, in the same place
    bool secondary_clicked = false; // the right button released over the box
    bool pressed_outside = false;   // a button went down this frame away from the box (and the boxes in it): close a popup
    Rectf bounds{};                 // where the box was last frame; empty on its first frame
    // draggable(): a drag from it began this frame / is on (not a click then).
    bool drag_started = false;
    bool dragging = false;
    // accepts_drop(): a drag of its type is over it / was let go over it this
    // frame (Context::drag_payload() has what it carries).
    bool drop_hovered = false;
    bool dropped = false;
};

// A field editing a value as text (Context::value_field).
struct ValueFieldOptions {
    std::string_view placeholder{}; // shown while it is empty
    std::string_view prefix{};      // inside it, at its left: an axis's letter
    std::string_view suffix{};      // at its right: its units ("px/s")
    // Committed as it is typed (each change that validates), not only when
    // it is left: a colour, a search. Esc commits the value from before the
    // typing again (when it validates).
    bool live = false;
    // Its look: the theme's input (and body text) unless given; given, used
    // as it is.
    std::optional<WidgetStyle> style{};
    std::optional<TextStyle> text_style{};
    std::optional<TextStyle> faint_style{}; // the placeholder's, prefix's, suffix's: the text's in the theme's muted colour unless given
    LayoutStyle layout{.width = grow()};
};
struct ValueFieldResult {
    std::optional<std::string> committed; // what was typed, once: when it was left (or as typed, `live`)
    bool editing = false;                 // being typed in
    bool invalid = false;                 // what is typed does not validate: not committed, outlined
    bool left = false;                    // left this frame (by Enter, Tab, a press elsewhere, or Esc)
    bool cancelled = false;               // by Esc: what it shows is the value again
};
// A number's field (Context::number_field): what it takes, how it shows it.
struct NumberFieldOptions {
    f64 min = -std::numeric_limits<f64>::infinity();
    f64 max = std::numeric_limits<f64>::infinity();
    bool whole = false; // an integer
    i32 digits = 7;     // significant digits shown (a float's)
    ValueFieldOptions field{};
};
// A value as a field shows it: `digits` significant digits, never "-0".
std::string number_text(f64 value, i32 digits = 7, bool whole = false);
// What a field's text says, if it is a number within [min, max] (whole: an integer).
std::optional<f64> parse_number(std::string_view text, f64 min, f64 max, bool whole);

// A value changed by dragging over a widget (Context::drag_value): a dial's,
// a fader's, a number scrubbed.
struct DragValueOptions {
    Scale scale{};       // its range, spread along the drag (Log: a frequency)
    f64 step = 0.0;      // above 0: kept to min plus a whole number of steps
    f32 travel = 200.0f; // how far a drag goes for the whole range
    f32 fine = 0.1f;     // with Shift held, a drag (or a key) goes this much as far
    UiLayoutAxis axis = UiLayoutAxis::Vertical; // Vertical: up raises it; Horizontal: right does
    f32 wheel = 0.02f;   // how far along a turn of the wheel moves it (0: the wheel does not)
    // What a double click sets it to (none: a double click does nothing).
    std::optional<f64> reset{};
    // In the Tab order: focused, Up and Right raise it, Down and Left lower
    // it, by a step (or a hundredth of the range; Shift: finely).
    bool focusable = true;
    i32 tab_index = 0;
    i32 z = 0;
};
struct DragValueResult : Interaction {
    bool changed = false;  // the value changed this frame
    bool dragging = false; // pressed and moved since: not a click when let go
};

// Where a popup box shows (Context::popup).
struct PopupPlace {
    Rectf anchor{};                // under it (or right of it): a button's bounds, last frame's
    UiFloat side = UiFloat::Below; // Below or Right of the anchor
    Vec2f offset{};
    // With no anchor: at this point (a context menu's), or where open_popup_at() put it.
    std::optional<Vec2f> point{};
    Id opener{};           // a press on it does not close the popup (its own click toggles it)
    Rectf opener_bounds{}; // nor one here (an opener known by where it is)
};

struct ScrollBoxOptions {
    f32 wheel_step = 48.0f;
    f32 scrollbar_thickness = 8.0f;
    f32 min_thumb = 18.0f;
    bool show_scrollbar = true;
    bool vertical = true;
    bool horizontal = false; // children keep their width and scroll sideways (wheel x, or Shift+wheel)
    // Kept at the end while it is there, as the content grows (a log's last
    // lines in view); scrolled up, it stays where it was put.
    bool stick_to_end = false;
};

// A key, with exactly these modifiers, fires `action` while focus is inside a box
// whose key_context is `context` (empty: anywhere). The innermost context wins.
struct KeyBinding {
    std::string action{};
    Key key = Key::Unknown;
    KeyModifiers modifiers = KeyModifiers::None;
    std::string context{};
};

struct SplitOptions {
    LayoutStyle layout{.width = grow(), .height = grow()};
    UiLayoutAxis axis = UiLayoutAxis::Horizontal; // Horizontal: panes side by side
    f32 ratio = 0.5f;                             // the first pane's share on its first frame
    f32 min_first = 60.0f;
    f32 min_second = 60.0f;
    f32 thickness = 4.0f;
};

// Rows [first, last) of a list, the ones a list() builds this frame.
struct ListRange {
    i32 first = 0;
    i32 last = 0;
};

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
        f32 delay = 0.5f; // seconds the pointer rests before it shows
        // Once a tooltip has shown, another shows at once if hovered within this
        // many seconds, so moving along a row of controls does not wait each time.
        f32 warm = 0.35f;
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
    // segment as start (x,y) and delta (w,h); for Shape (the shapes: polylines, arcs, circles,
    // paths, meshes) the box around it; for everything else it is the draw rectangle.
    struct DrawOp {
        enum class Kind { Fill, RoundedFill, Outline, RoundedOutline, Line, Sprite, Texture, Text, Gradient, Shape };
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
        std::string role; // "fill" | "shadow" | "border" | "text" | "sprite" | "texture" | "gradient" | "line" | "shape"
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
    // What the frame drew, in order (DebugOptions::record_draws): after end()
    // and before the next begin().
    const std::vector<DrawOp>& draw_ops() const { return _draw_ops; }

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
    // Open, and no popup of its own open over it: the one the keys are for.
    bool popup_on_top(Id id) const;
    bool popup_hovered_or_open(Id id) const;
    PopupResult begin_popup(Id id, Rectf anchor, PopupOptions options);
    void end_popup();

    // A tooltip by `anchor` while the pointer rests on it, drawn over everything
    // once the frame is laid out (the overlay pass).
    TooltipResult tooltip(Id id, Rectf anchor, std::string_view text, TooltipOptions options);
    TooltipResult tooltip(Id id, Rectf anchor, std::string_view text) {
        return tooltip(id, anchor, text, tooltip_style());
    }
    // How boxes' tooltips look (BoxStyle::tooltip, and tooltip() without
    // options): the theme's tooltip surface and body text unless set.
    void set_tooltip_style(TooltipOptions options) { _tooltip_style = std::move(options); }
    TooltipOptions tooltip_style() const;


    // Drawn over everything once the frame is laid out (after end_layout()'s
    // drawing, or at end()): tooltips, a drag's label, an app's own.
    template <typename F>
    void overlay(F&& draw) {
        _overlays.add(-1, [draw = std::forward<F>(draw)](Context& ctx, Rectf) mutable { draw(ctx); });
    }

    DragSourceResult drag_source(Id id, Rectf bounds, DragPayload payload, i32 z = 0);
    DropTargetResult drop_target(Id id, Rectf bounds, std::string_view accepted_type, i32 z = 0);
    NavResult nav_index(i32 count, i32 current, i32 columns = 1, bool wrap = true);
    std::string prompt_for_action(std::string_view action) const;
    std::string prompt_for_action(std::string_view action, const PromptOptions& options) const;
    bool dragging() const { return _drag.dragging; }
    const DragPayload& drag_payload() const { return _drag.payload; }

    // The wheel is `id`'s while the pointer is on it (a plot it zooms, a dial
    // it turns): scroll boxes under it do not scroll. Ask each frame `id`
    // reads the wheel; it holds from the next (where the wheel goes is settled
    // before widgets run, by where the pointer was).
    void take_wheel(Id id);
    // Call with each click on `id` (a region's `clicked`, or a press a widget
    // counts as one): true when it is the second of a double click, soon
    // after the first and in the same place. Boxes have BoxResult::double_clicked.
    bool double_click(Id id);

    // Pointer / input access in logical space.
    Vec2f pointer() const;
    bool pointer_pressed(MouseButton button = MouseButton::Left) const;
    bool pointer_held(MouseButton button = MouseButton::Left) const;
    bool pointer_released(MouseButton button = MouseButton::Left) const;
    // The wheel this rendered frame (Input::frame_mouse_wheel_y).
    f32 mouse_wheel_y() const;
    f32 mouse_wheel_x() const;
    bool key_pressed(Key key) const;
    // Pressed this frame, or auto-repeated while held: for keys that act again
    // while held, such as a text caret's arrows and Backspace.
    bool key_typed(Key key) const;
    bool action_pressed(std::string_view action) const;
    bool modifier_held(KeyModifiers modifiers) const;
    std::string_view text_input() const;
    // What an input method is composing (Input::text_composition), and its
    // cursor in bytes.
    std::string_view text_composition() const;
    i32 text_composition_cursor() const;
    bool wants_text_input() const { return _wants_text_input; }
    void request_text_input() { _wants_text_input = true; }
    // Where the focused editor's text is (drawing coordinates) and its caret's
    // x there, for the input method's candidate list. Text inputs set it.
    void set_text_input_area(Rectf area, f32 caret_x);
    // The area last set this frame, in window coordinates.
    std::optional<Rectf> text_input_area() const { return _text_input_area; }
    // Starts or stops the window's text input as widgets want it, and tells
    // the input method where the caret is: call it once a frame, after end().
    void apply_text_input(Window& window) const;
    // Everything a frame asks of the window and the app, once a frame after
    // end(): text input (apply_text_input), the pointer's shape, and another
    // frame while something is still moving (App::request_redraw, which matters
    // when the app waits for events).
    void apply(App& app, Window& window) const;
    // The drags_window() boxes (and the interactive boxes inside them) as last laid
    // out, in layout units: what apply() hands a borderless window.
    const WindowChrome& window_chrome() const;

    // The pointer's shape this frame, set by what it is over: a box with a
    // BoxStyle::cursor, a text field, a splitter. The last call wins; Default
    // when nothing set it.
    void set_cursor(SystemCursor cursor) { _cursor = cursor; }
    SystemCursor cursor() const { return _cursor; }
    // Something drawn this frame is still changing (a state transition, a
    // blinking caret), so a frame should be drawn in `seconds` (0: the next)
    // though nothing happens; the soonest asked this frame is kept.
    void request_redraw(f32 seconds = 0.0f) { _redraw_in = _redraw_in < 0.0f ? seconds : std::min(_redraw_in, seconds); }
    bool wants_redraw() const { return _redraw_in >= 0.0f; }
    f32 redraw_in() const { return _redraw_in; } // < 0: none asked
    // The time, in seconds, double clicks, tooltips and a caret's blinking are
    // timed by: the steady clock's, read once a frame at begin(), unless a
    // clock is set (a test's own, stepped by hand).
    void set_clock(std::function<f64()> clock) { _clock = std::move(clock); }
    f64 now() const { return _now; }

    // State that belongs to a part of the UI rather than to the app (whether a
    // section is open, a panel's filter text): a T kept for `id`, made with
    // T{} (or `make()`) the first time it is asked for. A component can then
    // keep its own state, as a gpui view does. It goes once a frame passes
    // without it being asked for (what used it is gone), unless keep_state()
    // keeps it; the reference holds until then. Asking for the id as another
    // type starts it over.
    template <class T>
    T& state(Id id);
    template <class T, class Make>
    T& state(Id id, Make&& make);
    void keep_state(Id id, bool keep = true); // kept while not asked for too, until drop_state()
    void drop_state(Id id);
    bool has_state(Id id) const { return _states.contains(id.value); }
    std::string clipboard_text() const;
    void set_clipboard_text(std::string_view text);
    UiTextInputState& text_input_state(Id id, std::string_view value = {});
    // A TextEdit's state kept per id; `initial` is its text the first time only.
    UiTextEditState& text_edit_state(Id id, std::string_view initial = {});
    UiComboState& combo_state(Id id, i32 selected = 0);
    ColorPickerMode& color_picker_mode(Id id, ColorPickerMode mode = ColorPickerMode::Hsv);

    // Mirrors what is drawn inside `bounds`, and the pointer read there, as a
    // right-to-left interface wants a widget's insides: shapes, clips and the
    // places of text and images are reflected across the bounds, while text
    // and images still read the right way round. Scopes nest. Widgets with a
    // left and a right (a checkbox, a slider, tabs) mirror themselves this way
    // while ui_direction() is right to left; mirror_if_right_to_left does that.
    void push_mirror(Rectf bounds);
    void pop_mirror();
    class MirrorGuard {
    public:
        MirrorGuard(Context* ctx, Rectf bounds) : _ctx(ctx) {
            if (_ctx) _ctx->push_mirror(bounds);
        }
        ~MirrorGuard() {
            if (_ctx) _ctx->pop_mirror();
        }
        MirrorGuard(const MirrorGuard&) = delete;
        MirrorGuard& operator=(const MirrorGuard&) = delete;

    private:
        Context* _ctx;
    };
    [[nodiscard]] MirrorGuard mirror_if_right_to_left(Rectf bounds);
    bool mirrored() const { return !_mirror_axes.empty(); }
    // Through the open mirror scopes: where `rect` (drawing coordinates in the
    // scopes) is on screen, and the pointer on screen (pointer() is in the scopes).
    Rectf to_screen(Rectf rect) const;
    Vec2f screen_pointer() const;

    // Immediate draw helpers (logical space).
    void push_clip(Rectf bounds);
    // With rounded corners (top-left, top-right, bottom-right, bottom-left), so
    // what scrolls inside a rounded panel keeps off its corners. Pointer hits
    // still clip to the rectangle.
    void push_clip(Rectf bounds, std::array<f32, 4> corner_radii);
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
    // Shapes, anti-aliased (Renderer2D's): for charts, dials and the like.
    // Widths are in layout units; angles in degrees, 0 at +x, clockwise. A
    // mirror scope reflects them as it does everything else, and they are
    // checked against the draw scope (by the box around them) and recorded
    // (DrawOp::Kind::Shape) as the rest are.
    void line(Vec2f a, Vec2f b, Color color, f32 width, LineCap cap = LineCap::Butt);
    void polyline(std::span<const Vec2f> points, Color color, StrokeStyle style = {});
    void fill_polygon(std::span<const Vec2f> points, Color color);
    void fill_circle(Vec2f center, f32 radius, Color color);
    void outline_circle(Vec2f center, f32 radius, Color color, f32 width = 1.0f);
    void arc(Vec2f center, f32 radius, f32 start, f32 end, Color color, StrokeStyle style = {});
    void fill_pie(Vec2f center, f32 radius, f32 start, f32 end, Color color);
    void fill_path(const Path& path, Color color, FillRule rule = FillRule::NonZero);
    void stroke_path(const Path& path, Color color, StrokeStyle style = {});
    // A mesh made once (or each frame, by hand: the triangles under a chart's
    // line), placed by `transform` and tinted.
    void shape(const ShapeMesh& mesh, const Affine2& transform = {}, Color tint = colors::white);
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
    Rectf layout_root() const { return _root; } // what begin_layout() was given
    // A text field is typed in (it was last frame): an app's own single keys
    // (Delete, the arrows, letters) wait.
    bool typing() const { return _wants_text_input_last; }
    void begin_row(LayoutStyle style = {});
    void begin_wrap_row(LayoutStyle style = {});
    void begin_column(LayoutStyle style = {});
    void begin_grid(i32 columns, LayoutStyle style = {});
    void end_container();

    // `w` must outlive end_layout() (the write-back `w = snap` at that point is the only
    // remaining lifetime requirement; the run itself works from a copy taken here).
    // A widget with a comparable `state` (a TextInput's) that the app changes
    // after declaring it this frame (a field emptied, its text set) keeps the
    // app's state: the run's results are written back, its state is not.
    // measure(w) as it is laid out here: in this context's theme (the styles
    // left at their defaults take the theme's), as its run draws it.
    template <typename W>
    Vec2f measured(W& w) const {
        if constexpr (requires(W& x, const Theme& t) { theme_styles(t, x); }) {
            const KeepStyles keep(w);
            theme_styles(_theme, w);
            return measure(w);
        } else {
            return measure(w);
        }
    }

    template <typename W>
    void widget(W& w, LayoutStyle style = {}) {
        const i32 index = add_node(style, measured(w));
        if constexpr (requires { w.state == w.state; }) {
            _pending.add(index, [&w, snap = w, declared = w.state](Context& ctx, Rectf solved) mutable {
                                    snap.bounds = solved;
                                    run(ctx, snap);
                                    if (w.state == declared) {
                                        w = std::move(snap); // results (clicked, interaction, …) and state
                                    } else {
                                        auto changed = std::move(w.state);
                                        w = std::move(snap);
                                        w.state = std::move(changed);
                                    }
                                });
        } else {
            _pending.add(index, [&w, snap = w](Context& ctx, Rectf solved) mutable {
                                    snap.bounds = solved;
                                    run(ctx, snap);
                                    w = std::move(snap); // write results (clicked, interaction, …) back
                                });
        }
    }

    void end_layout();

    // Like widget(), but the layout keeps its own copy: for widgets whose results
    // are not needed (labels, images, spacers), so they can be temporaries.
    template <typename W>
    void add(W w, LayoutStyle style = {}) {
        const i32 index = add_node(style, measured(w));
        _pending.add(index, [w = std::move(w)](Context& ctx, Rectf solved) mutable {
                                w.bounds = solved;
                                run(ctx, w);
                            });
    }
    void label(std::string_view text, LayoutStyle style = {});
    // With `overflow` Ellipsis (or Shrink) a label shrinks (LayoutStyle::shrink): as
    // wide as its text while its line has room, narrower with its text cut with "…"
    // when it has not. A tab title, a file name in a tree row, a menu item.
    void label(std::string_view text, const TextStyle& text_style, LayoutStyle style = {},
               TextOverflow overflow = TextOverflow::Visible);
    void image(const Sprite& sprite, Vec2f size, Color tint = colors::white, LayoutStyle style = {});
    // An SVG icon `size` units big. `svg` is the document's text (embedded in the
    // program, or read from a file). It is drawn into a texture once per document,
    // size and SvgOptions and kept as long as the Context.
    void svg(std::string_view svg, Vec2f size, Color tint = colors::white, SvgOptions options = {}, LayoutStyle style = {});
    const Sprite& svg_sprite(std::string_view svg, Vec2f size, SvgOptions options = {}); // empty when unreadable
    void spacer(LayoutStyle style = {.width = grow(), .height = grow()});

    // Composition (experimental). Must be inside begin_layout()/end_layout().
    //
    //   {
    //       auto row = ui.box(make_id("row"), style); // RAII: the box ends at scope exit
    //       ui.label(name);                            // children
    //       if (row.clicked) { ... }
    //   }
    //
    // or begin_box()/end_box() explicitly.
    BoxResult begin_box(Id id, const BoxStyle& style);
    void end_box();

    class BoxScope : public BoxResult {
    public:
        BoxScope(Context* ctx, BoxResult result, void (Context::*end)())
            : BoxResult(result),
              _ctx(ctx),
              _end(end) {}
        ~BoxScope() {
            if (_ctx) {
                (_ctx->*_end)();
            }
        }
        BoxScope(BoxScope&& other) noexcept
            : BoxResult(other),
              _ctx(other._ctx),
              _end(other._end) {
            other._ctx = nullptr;
        }
        BoxScope(const BoxScope&) = delete;
        BoxScope& operator=(const BoxScope&) = delete;
        BoxScope& operator=(BoxScope&&) = delete;

    private:
        Context* _ctx = nullptr;
        void (Context::*_end)() = nullptr;
    };
    // The box of an open popup (open_popup(id), or open_subpopup for a submenu):
    // floating over everything where `place` says, closed by a press outside it
    // and outside the popups opened from inside it (not on `place.opener`), or
    // by Esc while it is on top. Empty while closed:
    //   if (auto menu = ui.popup(id, ui2::vbox().w(200), {.anchor = button.bounds})) { ...rows... }
    std::optional<BoxScope> popup(Id id, BoxStyle style, const PopupPlace& place);
    void open_popup_at(Id id, Vec2f point); // open_popup(), placed at `point` (a context menu)

    // A field editing a value as text: it shows `shown` (the value now)
    // while not typed in; typed in, it is left by Enter, Tab or a press
    // elsewhere, and what was typed comes back once (`committed`), or by Esc,
    // which shows the value again. A text `valid` does not take is not
    // committed, and the field is outlined in the theme's danger colour.
    template <typename Valid>
    ValueFieldResult value_field(Id id, std::string_view shown, const ValueFieldOptions& options, const Valid& valid) {
        return checked_value_field(id, shown, options, &valid, [](const void* check, std::string_view text) {
            return static_cast<bool>((*static_cast<const Valid*>(check))(text));
        });
    }
    ValueFieldResult value_field(Id id, std::string_view shown, const ValueFieldOptions& options = {}) {
        return checked_value_field(id, shown, options, nullptr, nullptr);
    }
    // A number's field: the value typed, once it is left (within [min, max]).
    std::optional<f64> number_field(Id id, f64 value, const NumberFieldOptions& options = {});
    // `value` changed by a drag over `bounds` (up, or right), the wheel, the
    // keys while focused, and a double click (to `reset`). The press is held
    // wherever the pointer goes; a widget draws itself around it (a dial, a
    // fader, a scrubbed number). The pointer is the axis's resize shape there.
    DragValueResult drag_value(Id id, Rectf bounds, f64& value, const DragValueOptions& options = {});
    // Typing in a value field starts (a rename asked for): its text selected,
    // from its next value_field().
    void edit_value(Id id);
    // No value field is typed in any more: each shows its value again (what it
    // shows changed: an undo, another thing picked).
    void stop_value_fields();
    [[nodiscard]] BoxScope box(Id id, const BoxStyle& style) {
        return BoxScope(this, begin_box(id, style), &Context::end_box);
    }

    // A box whose children scroll. Give it a bounded size (fixed or grow); its
    // content is laid out at full size and moved by the offset, which the Context
    // keeps by id. The wheel scrolls the innermost scroll box under the pointer
    // that can move that way, in the same frame.
    BoxResult begin_scroll(Id id, const BoxStyle& style, ScrollBoxOptions options = {});
    void end_scroll();
    [[nodiscard]] BoxScope scroll(Id id, const BoxStyle& style, ScrollBoxOptions options = {}) {
        return BoxScope(this, begin_scroll(id, style, options), &Context::end_scroll);
    }
    Vec2f scroll_offset(Id id) const;
    void set_scroll_offset(Id id, Vec2f offset); // clamped at the next end_layout()

    // Rows of one height, of which only those visible in the enclosing scroll box
    // (plus a few either side) are built: `row(i)` is called for each, inside a
    // horizontal row container of that height.
    template <typename F>
    void list(Id id, i32 count, f32 row_height, F&& row) {
        list(id, count, row_height, LayoutStyle{.width = grow()}, std::forward<F>(row));
    }
    template <typename F>
    void list(Id id, i32 count, f32 row_height, LayoutStyle style, F&& row) {
        const ListRange range = begin_list(id, count, row_height, style);
        for (i32 i = range.first; i < range.last; ++i) {
            begin_list_row();
            row(i);
            end_container();
        }
        end_list();
    }
    ListRange begin_list(Id id, i32 count, f32 row_height, LayoutStyle style = {.width = grow()});
    void begin_list_row();
    void end_list();
    // Scrolls the list's scroll box so that row is in view (built or not):
    // rows built after it, this frame, are there.
    void scroll_to_row(Id list, i32 row);
    // Scrolls the scroll boxes around a box, or a widget that takes focus (a
    // text field), so it is in view (as it was laid out last frame).
    void scroll_into_view(Id id);

    // Two panes and a draggable handle between them:
    //   ui.begin_split(id, opts); ...first pane...; ui.split_next(); ...second...; ui.end_split();
    // The ratio is kept by id.
    void begin_split(Id id, SplitOptions options = {});
    void split_next();
    void end_split();
    f32 split_ratio(Id id) const;

    // Keyboard focus order. Focusable boxes join it; so do text fields, which also
    // hand Tab on. Tab and Shift+Tab move along it at end() once focus is in it (or
    // any box is focusable), and the box focused is scrolled into view.
    void register_focusable(Id id, i32 tab_index = 0);
    void request_focus_move(Id from, i32 direction); // at end(): +1 next, -1 previous
    // True for the frame after Tab (or request_focus_move) moved focus: a text field
    // that is still editing without focus stops.
    bool focus_moved() const { return _focus_moved; }

    // Key bindings, dispatched at begin() from where focus is (see KeyBinding).
    // Bindings without Ctrl or Alt are skipped while a text field has the keys.
    void bind(KeyBinding binding);
    void clear_bindings() { _bindings.clear(); }
    bool action_triggered(std::string_view action) const;

private:
    // What end_layout() runs for the frame's nodes (a widget's run and
    // write-back, a box's surface): closures kept in blocks reused frame after
    // frame, not a heap allocation each. clear() destroys them (begin(),
    // begin_layout()); the blocks stay.
    class PendingRuns {
    public:
        struct Run {
            i32 node = -1;
            void* closure = nullptr;
            void (*call)(void*, Context&, Rectf) = nullptr;
            void (*destroy)(void*) = nullptr;
        };
        PendingRuns() = default;
        PendingRuns(PendingRuns&& other) noexcept { *this = std::move(other); }
        PendingRuns& operator=(PendingRuns&& other) noexcept;
        PendingRuns(const PendingRuns&) = delete;
        PendingRuns& operator=(const PendingRuns&) = delete;
        ~PendingRuns() { clear(); }

        template <typename F>
        void add(i32 node, F&& f) {
            using Fn = std::decay_t<F>;
            static_assert(alignof(Fn) <= alignof(std::max_align_t), "a block is only aligned as new[] aligns");
            void* at = allocate(sizeof(Fn), alignof(Fn));
            ::new (at) Fn(std::forward<F>(f));
            _runs.push_back({.node = node,
                             .closure = at,
                             .call = [](void* c, Context& ctx, Rectf bounds) { (*static_cast<Fn*>(c))(ctx, bounds); },
                             .destroy = std::is_trivially_destructible_v<Fn> ? nullptr
                                                                             : +[](void* c) { static_cast<Fn*>(c)->~Fn(); }});
        }
        void clear();
        std::size_t size() const { return _runs.size(); }
        const Run& operator[](std::size_t i) const { return _runs[i]; }

    private:
        void* allocate(std::size_t size, std::size_t align);
        std::vector<Run> _runs;
        std::vector<std::unique_ptr<std::byte[]>> _blocks;
        std::vector<std::size_t> _block_sizes;
        std::size_t _block = 0; // the block being filled
        std::size_t _used = 0;  // bytes of it taken
    };

    i32 add_node(const LayoutStyle& style, Vec2f intrinsic);
    void clear_nodes();
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
    // A shape about to be drawn in `box`: checked and recorded; false when
    // there is nothing to draw (no renderer, a transparent colour).
    bool shape_drawn(std::string_view kind, Rectf box, f32 width, Color color);
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
    std::optional<Rectf> _text_input_area; // window coordinates
    std::vector<f32> _mirror_axes;          // 2x + w of each mirror scope's bounds, innermost last
    i32 _text_input_cursor = 0;

    std::vector<LayoutNode> _nodes;
    std::vector<std::vector<i32>> _child_lists; // the last frame's nodes' child lists, emptied, for this frame's
    std::vector<i32> _stack;
    PendingRuns _pending;
    LayoutScratch _layout_scratch; // reused across end_layout() calls, avoids per-frame alloc
    Rectf _root{};

    // Composition (experimental). Per-node tags for this layout, and what each id
    // was last frame (rects, scroll offsets, split ratios), kept across frames.
    enum class ComposeKind : u8 { Box, Scroll, ScrollbarY, ScrollbarX, List, Split, SplitHandle };
    struct ComposeNode {
        i32 node = -1;
        Id id{};
        ComposeKind kind = ComposeKind::Box;
        bool clip = false;
        f32 clip_radius = 0.0f;
        f32 row_height = 0.0f; // lists
        std::string key_context{};
        ScrollBoxOptions scroll{};
        bool interactive = false;
        bool window_drag = false;
        i32 z = 0; // hit depth (compose_z)
    };
    struct ComposeRecord {
        Rectf rect{};
        Rectf visible{}; // rect within its clips: where it can be hit
        Id parent{};     // the enclosing recorded box
        bool scroll = false;
        i32 z = 0;       // hit depth (compose_z)
        f32 row_height = 0.0f; // lists
        std::string key_context{};
        u64 frame = 0;
    };
    struct ScrollRecord {
        Vec2f offset{};
        Vec2f laid_out{}; // the offset last frame's content was placed at
        bool at_end = true; // last frame, scrolled to the end (stick_to_end keeps it there)
        Vec2f content{};  // content size, padding included
        Vec2f viewport{}; // box size
        std::array<Rectf, 2> track{};       // scrollbars as last drawn: [0] vertical, [1] horizontal
        std::array<Interaction, 2> thumb{}; // their thumbs this frame
        std::array<f32, 2> grab{-1.0f, -1.0f}; // pointer-to-thumb offset while one is dragged
        u64 frame = 0;
    };
    struct SplitFrame {
        Id id{};
        SplitOptions options{};
        Interaction handle{};
    };
    struct FocusEntry {
        Id id{};
        Id box{};        // the enclosing box, for key contexts
        i32 tab_index = 0;
        i32 order = 0;          // its layout node (document order), boxes' and fields' alike
        bool box_entry = false; // a focusable box (opts the Context into Tab)
    };
    struct Reveal {
        Id id{};        // a box, or a list with `row`
        i32 row = -1;
    };
    void begin_compose(Id id, ComposeKind kind, const BoxStyle& style, LayoutStyle layout);
    BoxResult compose_interaction(Id id, const BoxStyle& style);
    i32 compose_z(const BoxStyle& style) const;
    // How far a floating box lifts the hit depth of what is in it: its boxes
    // (compose_z) and its widgets (end_layout() runs them with _region_z_base).
    static constexpr i32 floating_hit_z = 100000;
    Rectf pixel_snap(Rectf rect) const;
    void scroll_thumbs(Id id, const ScrollBoxOptions& options, i32 z);
    void layout_compose();
    void reveal(const Reveal& target);
    void record_compose(const std::vector<i32>& clip_of, const std::vector<Rectf>& visible_clips);
    void draw_scrollbar(Id id, Rectf track, i32 axis, const ScrollBoxOptions& options);
    void add_focusable(Id id, i32 tab_index, bool box_entry);
    void finish_focus();
    void dispatch_bindings();
    std::vector<ComposeNode> _compose;
    std::vector<Id> _compose_ids;    // open boxes, innermost last (parent of the next record)
    std::vector<Id> _scroll_ids;     // open scroll boxes, innermost last
    std::vector<SplitFrame> _splits_open;
    std::vector<f32> _list_heights;  // open lists' row heights, innermost last
    std::unordered_map<u64, ComposeRecord> _compose_records;
    std::unordered_map<u64, ScrollRecord> _scroll_records;
    std::unordered_map<u64, f32> _split_ratios;
    std::vector<FocusEntry> _focus_order; // this frame's, as registered
    std::unordered_map<u64, Id> _focus_boxes; // last frame: focusable id -> enclosing box
    std::optional<std::pair<Id, i32>> _focus_move;
    std::vector<KeyBinding> _bindings;
    std::vector<std::string> _actions; // fired this frame
    bool _wants_text_input_last = false;
    bool _focus_moved = false;
    bool _any_press = false; // a mouse button went down this frame (before widgets took it)
    f32 _pixel_scale = 0.0f; // the renderer's content scale: pixels per unit, edges snapped to them (0: off)
    i32 _region_z_base = 0;  // added to region()'s z while end_layout() runs a widget in floating boxes
    i32 _running_node = -1;  // the layout node whose widget end_layout() runs
    std::vector<Id> _node_owner; // while end_layout() runs: each node's enclosing box (a box's own)
    Id _focus_trap{};            // the last box built this frame that keeps the focus in it
    WindowChrome _chrome;    // the drags_window() boxes as last laid out, in layout units
    SystemCursor _cursor = SystemCursor::Default;
    f32 _redraw_in = -1.0f; // the soonest request_redraw() this frame; < 0: none
    std::vector<u64> _wheel_takers;      // take_wheel() last frame: where the wheel does not scroll
    std::vector<u64> _wheel_takers_next; // and this frame
    std::function<f64()> _clock;
    f64 _now = 0.0;

    struct StateSlot {
        std::unique_ptr<void, void (*)(void*)> value{nullptr, nullptr};
        const void* type = nullptr; // state_type<T>()
        u64 frame = 0;              // last asked for
        bool kept = false;
    };
    template <class T>
    static const void* state_type() {
        static const char tag = 0;
        return &tag;
    }
    std::unordered_map<u64, StateSlot> _states;
    std::unordered_map<u64, Sprite> _svg_sprites;

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

    PendingRuns _overlays; // overlay(): drawn once the frame is laid out, then dropped
    // value_field(): each one's text input, and whether it was typed in when last seen.
    struct ValueEdit {
        TextInput input{};
        bool was_active = false;
        bool start = false; // edit_value(): typed in from its next call, its text selected
        std::string before; // the value when typing began: what Esc puts back on a live field
        u64 frame = 0;
    };
    std::unordered_map<u64, ValueEdit> _value_edits;
    ValueFieldResult checked_value_field(Id id, std::string_view shown, const ValueFieldOptions& options, const void* check,
                                         bool (*valid)(const void*, std::string_view));
    std::optional<TooltipOptions> _tooltip_style;
    std::optional<Vec2f> _popup_point; // open_popup_at()
    void draw_overlays();
    TooltipResult show_tooltip(Id id, bool over, Rectf anchor, std::string_view text, const TooltipOptions& options);
    bool press_inside(Id box) const;   // last frame's top hit is the box or in it (a box nested in it, a submenu)
    Id _tooltip_candidate{};
    f64 _tooltip_first_time = 0.0;
    std::optional<f64> _tooltip_shown_time; // the last time any tooltip was visible
    bool _tooltip_warm = false;              // the candidate skips the delay

    struct DragState {
        Id source{};
        Vec2f start{};
        DragPayload payload{};
        bool dragging = false;
        bool started_this_frame = false;
        bool released_this_frame = false;
        bool label = false; // a box's drag: its text shown by the pointer
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

template <class T>
T& Context::state(Id id) {
    return state<T>(id, [] { return T{}; });
}

template <class T, class Make>
T& Context::state(Id id, Make&& make) {
    StateSlot& slot = _states[id.value];
    slot.frame = _state.frame();
    if (slot.type != state_type<T>()) {
        slot.value = {new T(std::forward<Make>(make)()), [](void* value) { delete static_cast<T*>(value); }};
        slot.type = state_type<T>();
    }
    return *static_cast<T*>(slot.value.get());
}

} // namespace kin::ui2
