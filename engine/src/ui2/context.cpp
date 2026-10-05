#include <kin/ui2/context.hpp>

#include <kin/anim/value.hpp>
#include <kin/platform/log.hpp>
#include <kin/renderer/post_blur.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace kin::ui2 {
namespace {

// --- Debug wireframe theme color helpers ---------------------------------------------------
// Colors are derived from stable keys (geometry / scope identity), never a per-frame draw
// counter, so a component keeps its color frame to frame even when other draws appear/vanish.
u32 debug_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

f32 debug_hash01(u32 x) {
    return static_cast<f32>(debug_hash(x) & 0xFFFFFFU) / static_cast<f32>(0x1000000U);
}

u32 debug_mix(i32 a, i32 b) {
    return debug_hash(static_cast<u32>(a) * 73856093U ^ static_cast<u32>(b) * 19349663U);
}

Color hsv_color(f32 hue, f32 sat, f32 val, u8 alpha) {
    const f32 h = std::fmod(hue, 1.0f) * 6.0f;
    const i32 sector = static_cast<i32>(h) % 6;
    const f32 f = h - std::floor(h);
    const f32 p = val * (1.0f - sat);
    const f32 q = val * (1.0f - sat * f);
    const f32 t = val * (1.0f - sat * (1.0f - f));
    f32 r = 0.0f;
    f32 g = 0.0f;
    f32 b = 0.0f;
    switch (sector) {
    case 0: r = val; g = t; b = p; break;
    case 1: r = q; g = val; b = p; break;
    case 2: r = p; g = val; b = t; break;
    case 3: r = p; g = q; b = val; break;
    case 4: r = t; g = p; b = val; break;
    default: r = val; g = p; b = q; break;
    }
    return Color::rgba(static_cast<u8>(r * 255.0f), static_cast<u8>(g * 255.0f), static_cast<u8>(b * 255.0f), alpha);
}

// WCAG relative luminance and contrast ratio (sRGB), per the WCAG 2.x definitions.
f32 srgb_to_linear(f32 c) {
    return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

f32 relative_luminance(Color c) {
    return 0.2126f * srgb_to_linear(static_cast<f32>(c.r) / 255.0f) +
           0.7152f * srgb_to_linear(static_cast<f32>(c.g) / 255.0f) +
           0.0722f * srgb_to_linear(static_cast<f32>(c.b) / 255.0f);
}

f32 contrast_ratio(Color a, Color b) {
    const f32 la = relative_luminance(a);
    const f32 lb = relative_luminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

std::string rgb_str(Color c) {
    return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + ")";
}

Rectf default_screen(Renderer2D* renderer) {
    if (!renderer) {
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const Vec2i size = renderer->output_size();
    return {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)};
}

bool same_type(std::string_view actual, std::string_view accepted) {
    return accepted.empty() || accepted == "*" || actual == accepted;
}

std::string readable_action(std::string_view action) {
    std::string result{action};
    for (char& c : result) {
        if (c == '_' || c == '-') {
            c = ' ';
        }
    }
    return result;
}

std::string readable_key(Key key, bool compact) {
    switch (key) {
    case Key::Escape: return compact ? "Esc" : "Escape";
    case Key::Space: return "Space";
    case Key::Enter: return "Enter";
    case Key::Tab: return "Tab";
    case Key::Backspace: return compact ? "Back" : "Backspace";
    case Key::Delete: return compact ? "Del" : "Delete";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::F1: return "F1";
    case Key::F2: return "F2";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::A: return "A";
    case Key::B: return "B";
    case Key::C: return "C";
    case Key::D: return "D";
    case Key::E: return "E";
    case Key::F: return "F";
    case Key::G: return "G";
    case Key::H: return "H";
    case Key::I: return "I";
    case Key::J: return "J";
    case Key::K: return "K";
    case Key::L: return "L";
    case Key::M: return "M";
    case Key::N: return "N";
    case Key::O: return "O";
    case Key::P: return "P";
    case Key::Q: return "Q";
    case Key::R: return "R";
    case Key::S: return "S";
    case Key::T: return "T";
    case Key::U: return "U";
    case Key::V: return "V";
    case Key::W: return "W";
    case Key::X: return "X";
    case Key::Y: return "Y";
    case Key::Z: return "Z";
    case Key::Backslash: return "\\";
    case Key::Grave: return "`";
    case Key::LeftAlt: return "Alt";
    case Key::F3: return "F3";
    case Key::F4: return "F4";
    case Key::F5: return "F5";
    case Key::F6: return "F6";
    case Key::F7: return "F7";
    case Key::F8: return "F8";
    case Key::F9: return "F9";
    case Key::F10: return "F10";
    case Key::F11: return "F11";
    case Key::F12: return "F12";
    case Key::PageUp: return compact ? "PgUp" : "Page Up";
    case Key::PageDown: return compact ? "PgDn" : "Page Down";
    case Key::Unknown: break;
    }
    return compact ? "?" : "Unknown";
}

std::string readable_mouse(MouseButton button) {
    switch (button) {
    case MouseButton::Left: return "Mouse Left";
    case MouseButton::Middle: return "Mouse Middle";
    case MouseButton::Right: return "Mouse Right";
    case MouseButton::X1: return "Mouse X1";
    case MouseButton::X2: return "Mouse X2";
    }
    return "Mouse";
}

std::string prompt_binding_label(InputBinding binding, const PromptOptions& options) {
    std::string result;
    if (any(binding.modifiers)) {
        if (any(binding.modifiers & KeyModifiers::Ctrl)) {
            result += "Ctrl+";
        }
        if (any(binding.modifiers & KeyModifiers::Shift)) {
            result += "Shift+";
        }
        if (any(binding.modifiers & KeyModifiers::Alt)) {
            result += "Alt+";
        }
    }
    if (binding.device == InputBindingDevice::Key) {
        result += readable_key(static_cast<Key>(binding.code), options.compact);
    } else {
        result += readable_mouse(static_cast<MouseButton>(binding.code));
    }
    if (options.brackets) {
        return "[" + result + "]";
    }
    return result;
}

bool contains_any(const std::vector<Rectf>& rects, Vec2f point) {
    for (Rectf rect : rects) {
        if (contains(rect, point)) {
            return true;
        }
    }
    return false;
}

Rectf flipped_popup_bounds(Rectf anchor, Context::PopupOptions options, Rectf screen) {
    if (options.match_anchor_width) {
        options.size.x = std::max(options.size.x, anchor.w);
    }
    Rectf bounds = anchor_rect(anchor, options.anchor, options.size, options.offset);
    if (screen.w <= 0.0f || screen.h <= 0.0f) {
        return bounds;
    }
    const f32 screen_right = screen.x + screen.w;
    const f32 screen_bottom = screen.y + screen.h;
    if (options.flip_x && bounds.x + bounds.w > screen_right) {
        bounds.x = anchor.x - bounds.w - std::max(0.0f, options.offset.x);
    }
    if (options.flip_x && bounds.x < screen.x) {
        bounds.x = anchor.x + anchor.w + std::max(0.0f, options.offset.x);
    }
    if (options.flip_y && bounds.y + bounds.h > screen_bottom) {
        const f32 below_offset = std::max(0.0f, options.offset.y - anchor.h);
        bounds.y = anchor.y - bounds.h - below_offset;
    }
    if (options.flip_y && bounds.y < screen.y) {
        bounds.y = anchor.y + anchor.h + std::max(0.0f, options.offset.y);
    }
    return clamp_rect_to_bounds(bounds, screen);
}

f32 distance_sq(Vec2f a, Vec2f b) {
    const f32 dx = a.x - b.x;
    const f32 dy = a.y - b.y;
    return dx * dx + dy * dy;
}

f32 clamp_margin(f32 margin, f32 limit) {
    return std::clamp(margin, 0.0f, std::max(0.0f, limit));
}

Color alpha_scaled(Color color, f32 alpha) {
    color.a = static_cast<u8>(std::clamp(static_cast<f32>(color.a) * std::clamp(alpha, 0.0f, 1.0f), 0.0f, 255.0f));
    return color;
}

// Premultiply a straight-alpha color (rgb *= a). Used to tint a premultiplied
// coverage mask (e.g. the baked soft-shadow mask) without over-brightening.
Color premultiply(Color color) {
    const auto m = [&](u8 v) { return static_cast<u8>(static_cast<u32>(v) * color.a / 255u); };
    return {m(color.r), m(color.g), m(color.b), color.a};
}

void draw_nine_slice(Context& ctx, const UiNineSlice& skin, Rectf bounds, Color tint) {
    if (!skin.sprite.valid() || bounds.w <= 0.0f || bounds.h <= 0.0f || tint.a == 0) {
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
                ctx.sprite({.texture = skin.sprite.texture, .source = part_src}, part_dst, tint);
            }
        }
    }
}

} // namespace

SurfaceStyle resolve(const InteractiveSurfaceStyle& style, const Interaction& it, bool selected, bool enabled) {
    SurfaceStyle out = style.normal;
    if (!enabled) {
        out = style.disabled;
    } else if (selected) {
        out = style.selected;
    } else if (it.active) {
        out = style.pressed;
    } else if (it.hot) {
        out = style.hovered;
    }
    if (enabled && it.focused) {
        out.border = style.focused.border;
        out.border_width = std::max(out.border_width, style.focused.border_width);
        out.border_mode = style.focused.border_mode;
        if (style.focused.glow_color.a > 0 && style.focused.glow_size > 0.0f) {
            out.glow_color = style.focused.glow_color;
            out.glow_size = style.focused.glow_size;
        }
    }
    return out;
}

Context::Context()
    : _theme(default_theme()) {}

void Context::begin(Input& input, Renderer2D& renderer) {
    begin(input, renderer, 0.0f);
}

void Context::begin(Input& input, Renderer2D& renderer, f32 dt) {
    _input = &input;
    _renderer = &renderer;
    _dt = dt;
    _state.begin_frame();
    // GC stale surface-animation entries (widgets not drawn for a few frames).
    if (!_surface_anim.empty()) {
        const u64 frame = _state.frame();
        std::erase_if(_surface_anim, [frame](const auto& kv) {
            return kv.second.last_frame + 4 < frame;
        });
    }
    _nodes.clear();
    _stack.clear();
    _pending.clear();
    _diagnostics.clear();
    _draw_scopes.clear();
    _clip_stack.clear();
    _frame_region_ids.clear();
    _frame_dup_reported.clear();
    _draw_ops.clear();
    _glass_captured = false;
    _glass_backdrop = {}; // release last frame's backdrop back to the pool
    _in_shadow = false;
    _debug_last_hue = 0.0f;
    _surface_fill_under = colors::black;
    _wants_text_input = false;
    _root = {};
    _popup_opened_this_frame = {};
    _popup_frame_bounds.clear();
    _modal_opened_this_frame = {};
    _drag.started_this_frame = false;
    _drag.released_this_frame = false;
    if (!_input->mouse_held(MouseButton::Left) && !_drag.dragging) {
        _drag = {};
    }
}

SurfaceStyle Context::resolve_animated(Id id, const InteractiveSurfaceStyle& style, const Interaction& it,
                                       bool selected, bool enabled) {
    const SurfaceStyle target = resolve(style, it, selected, enabled);
    const f32 dur = _theme.transition_duration;
    // Instant: no dt (2-arg begin / tests), no duration, or no id. Touch no state so apps
    // that never animate (the common case) don't churn the retained map; a later animated
    // frame just snaps from the target on first use.
    if (_dt <= 0.0f || dur <= 0.0f || !static_cast<bool>(id)) {
        return target;
    }
    const u64 frame = _state.frame();
    const u64 key = id.value;
    const auto found = _surface_anim.find(key);
    if (found == _surface_anim.end()) {
        _surface_anim.emplace(key, SurfaceAnim{target, frame}); // snap on first appearance
        return target;
    }

    SurfaceStyle& current = found->second.current;
    const f32 t = ease(_theme.transition_easing, std::clamp(_dt / dur, 0.0f, 1.0f));
    const auto lerp_f = [t](f32 a, f32 b) { return a + (b - a) * t; };
    current.fill = mix(current.fill, target.fill, t);
    current.border = mix(current.border, target.border, t);
    current.glow_color = mix(current.glow_color, target.glow_color, t);
    current.glass_tint = mix(current.glass_tint, target.glass_tint, t);
    current.border_width = lerp_f(current.border_width, target.border_width);
    current.glow_size = lerp_f(current.glow_size, target.glow_size);
    current.opacity = lerp_f(current.opacity, target.opacity);
    found->second.last_frame = frame;

    // Everything else (radius, fill_kind, gradient, shadow, skin, ...) snaps to target.
    SurfaceStyle out = target;
    out.fill = current.fill;
    out.border = current.border;
    out.glow_color = current.glow_color;
    out.glass_tint = current.glass_tint;
    out.border_width = current.border_width;
    out.glow_size = current.glow_size;
    out.opacity = current.opacity;
    return out;
}

void Context::end() {
    if (_drag.released_this_frame || (!_input || !_input->mouse_held(MouseButton::Left))) {
        _drag = {};
    }
    // Failsafe: active is only cleared by region() when the active widget processes the
    // release. If that widget vanished or the release landed off its region, _active would
    // stick and block every future press (region()'s !active() guard). Enforce the
    // invariant that nothing is active while its button is up. Use _active_button (the
    // button that originally called set_active) so non-Left activations are handled too.
    if (_input && !_input->mouse_held(_state.active_button()) && _state.active()) {
        _state.clear_active();
    }
    // Clear focus when the user clicks empty space — but only if set_focused() was NOT
    // called this frame. Without that guard, a single-frame tap (press+release in one
    // frame) would erase the focus just granted: region() calls clear_active() on the
    // release path, so by the time end() runs, !active() is true even though a widget
    // was focused this frame.
    // A click swallowed by a blocking modal (landed outside the modal rect) is NOT a click
    // on empty space — region() returned early for every widget, so focused_this_frame stays
    // false. Without this guard, such a click would wrongly clear focus from a widget that
    // lives inside the modal (e.g. a focused text input).
    const bool blocked_by_modal = _modal_blocking && _input && !contains(_modal_bounds, pointer());
    if (_input && pointer_pressed(MouseButton::Left) && !_state.active() &&
        !_state.focused_this_frame() && !blocked_by_modal) {
        _state.clear_focused();
    }
    // Frame invariant: every push must have a matching pop by end(). A leftover entry means a
    // forgotten pop_clip()/pop_draw_scope(), which silently mis-clips everything drawn after it.
    if (_debug.detect_invariants) {
        if (!_clip_stack.empty()) {
            emit_diagnostic(DiagnosticKind::UnbalancedStack, {},
                            "clip stack not empty at end(): depth " + std::to_string(_clip_stack.size()),
                            _debug.log_invariants);
        }
        if (!_draw_scopes.empty()) {
            emit_diagnostic(DiagnosticKind::UnbalancedStack, _draw_scopes.back().widget,
                            "draw scope stack not empty at end(): depth " + std::to_string(_draw_scopes.size()),
                            _debug.log_invariants);
        }
    }
    _popup_last_bounds = _popup_frame_bounds;
    _popup_stack.clear();
    _modal_stack.clear();
    _input = nullptr;
    _renderer = nullptr;
}

void Context::report_overflow(std::string_view widget, Rectf bounds, Vec2f wanted, std::string_view detail) {
    if (!_debug.detect_overflow) {
        return;
    }
    constexpr f32 epsilon = 0.5f;
    const Vec2f overflow{
        std::max(0.0f, wanted.x - bounds.w),
        std::max(0.0f, wanted.y - bounds.h),
    };
    if (overflow.x <= epsilon && overflow.y <= epsilon) {
        return;
    }
    Diagnostic diagnostic{
        .widget = std::string{widget},
        .detail = std::string{detail},
        .bounds = bounds,
        .wanted = wanted,
        .overflow = overflow,
    };
    if (_debug.log_overflow) {
        KIN_LOG_WARN_F("ui",
                       "ui2 overflow",
                       (LogFields{{.name = "widget", .value = diagnostic.widget},
                                  {.name = "detail", .value = diagnostic.detail},
                                  {.name = "bounds", .value = std::to_string(bounds.w) + "x" + std::to_string(bounds.h)},
                                  {.name = "wanted", .value = std::to_string(wanted.x) + "x" + std::to_string(wanted.y)},
                                  {.name = "overflow", .value = std::to_string(overflow.x) + "x" + std::to_string(overflow.y)}}));
    }
    _diagnostics.push_back(std::move(diagnostic));
}

Interaction Context::region(Id id, Rectf bounds, i32 z, MouseButton button) {
    Interaction out;
    check_region_id(id);
    if (!_input) {
        return out;
    }
    if (_modal_blocking) {
        const Rectf allowed = !_modal_stack.empty() ? _modal_stack.back() : _modal_bounds;
        if (!contains(allowed, pointer())) {
            return out;
        }
    }

    const Vec2f p = pointer();
    if (contains(bounds, p)) {
        _state.register_hit(id, z);
    }

    out.hot = _state.is_hot(id);
    out.focused = _state.is_focused(id);

    const bool press = pointer_pressed(button);
    const bool release = pointer_released(button);

    if (press && out.hot && !_state.active()) {
        _state.set_active(id, button);
        _state.set_focused(id);
        // Claim the press so it does not persist past this frame (it survives 0-step
        // frames until read, but once a widget owns it, re-reading on later persisted
        // frames must not re-fire `pressed` — e.g. re-capturing a drag/grab origin).
        _input->consume_mouse_frame_pressed(button);
    }
    // active is owned per-button: a widget is only "active" for the button that claimed it.
    // This keeps a release (or query) of a different button from stealing/clearing the
    // activation, and confines pressed/released/clicked to the owning button.
    out.active = _state.is_active(id) && _state.active_button() == button;
    out.pressed = press && out.hot && out.active;
    if (release && out.active) {
        out.released = true;
        out.clicked = out.hot;
        _state.clear_active();
    }
    return out;
}

void Context::open_modal(Id id) {
    _open_modal = id;
    _modal_opened_this_frame = id;
}

void Context::close_modal(Id id) {
    if (modal_open(id)) {
        _open_modal = {};
        _modal_blocking = false;
        _modal_bounds = {};
    }
}

bool Context::modal_open(Id id) const {
    return static_cast<bool>(id) && _open_modal == id;
}

Context::ModalResult Context::begin_modal(Id id, Rectf bounds, ModalOptions options) {
    ModalResult result;
    if (!modal_open(id)) {
        return result;
    }
    result.open = true;
    result.opened = _modal_opened_this_frame == id;
    result.bounds = bounds;
    _modal_bounds = bounds;
    _modal_blocking = options.block_background_input;

    Rectf screen = default_screen(_renderer);
    if (screen.w > 0.0f && screen.h > 0.0f) {
        fill_rect(screen, options.overlay);
    }
    region(id, bounds, options.z);
    _modal_stack.push_back(bounds);

    if (options.close_on_escape && action_pressed("quit")) {
        close_modal(id);
        result.open = false;
        result.closed = true;
        result.cancelled = true;
        return result;
    }
    if (options.close_on_outside_click && _modal_opened_this_frame != id && pointer_pressed() && !contains(bounds, pointer())) {
        close_modal(id);
        _input->consume_mouse_frame_pressed(MouseButton::Left);
        result.open = false;
        result.closed = true;
        result.outside_clicked = true;
        return result;
    }
    return result;
}

void Context::end_modal() {
    if (!_modal_stack.empty()) {
        _modal_stack.pop_back();
    }
}

void Context::open_popup(Id id) {
    _open_popup = id;
    _open_subpopup = {};
    _popup_opened_this_frame = id;
}

void Context::open_subpopup(Id parent, Id id) {
    if (popup_open(parent)) {
        _open_subpopup = id;
        _popup_opened_this_frame = id;
    }
}

void Context::close_popup(Id id) {
    if (static_cast<bool>(id) && _open_subpopup == id) {
        _open_subpopup = {};
        return;
    }
    if (static_cast<bool>(id) && _open_popup == id) {
        _open_popup = {};
        _open_subpopup = {};
    }
}

bool Context::popup_open(Id id) const {
    return static_cast<bool>(id) && (_open_popup == id || _open_subpopup == id);
}

bool Context::popup_hovered_or_open(Id id) const {
    return popup_open(id) || (_input && (contains_any(_popup_stack, pointer()) || contains_any(_popup_last_bounds, pointer())));
}

Context::PopupResult Context::begin_popup(Id id, Rectf anchor, PopupOptions options) {
    PopupResult result;
    if (!popup_open(id)) {
        return result;
    }

    Rectf screen = options.screen;
    if (screen.w <= 0.0f || screen.h <= 0.0f) {
        screen = default_screen(_renderer);
    }
    Rectf bounds = flipped_popup_bounds(anchor, options, screen);
    result.open = true;
    result.opened = _popup_opened_this_frame == id;
    result.bounds = bounds;

    region(id, bounds, options.z);
    _popup_stack.push_back(bounds);
    _popup_frame_bounds.push_back(bounds);

    if (options.close_on_outside_click && _input && _popup_opened_this_frame != id && pointer_pressed() &&
        !contains(bounds, pointer()) && !contains_any(_popup_stack, pointer()) && !contains_any(_popup_last_bounds, pointer())) {
        close_popup(id);
        _input->consume_mouse_frame_pressed(MouseButton::Left);
        result.open = false;
        result.closed = true;
        result.outside_clicked = true;
    }
    return result;
}

void Context::end_popup() {
    if (!_popup_stack.empty()) {
        _popup_stack.pop_back();
    }
}

Context::TooltipResult Context::tooltip(Id id, Rectf anchor, std::string_view value, TooltipOptions options) {
    TooltipResult result;
    if (!_input || value.empty() || !contains(anchor, pointer())) {
        if (_tooltip_candidate == id) {
            _tooltip_candidate = {};
        }
        return result;
    }

    if (_tooltip_candidate != id) {
        _tooltip_candidate = id;
        _tooltip_first_frame = _state.frame();
        _tooltip_warm = _tooltip_shown_frame && _state.frame() <= *_tooltip_shown_frame + options.warm_frames;
        if (!_tooltip_warm) {
            return result;
        }
    }
    if (!_tooltip_warm && _state.frame() < _tooltip_first_frame + options.delay_frames) {
        return result;
    }
    _tooltip_shown_frame = _state.frame();

    const Vec2f text_size = measure_text(options.text_style.font, value, options.text_style.scale);
    const Vec2f size{
        text_size.x + options.padding.left + options.padding.right,
        text_size.y + options.padding.top + options.padding.bottom,
    };
    Rectf screen = default_screen(_renderer);
    Rectf bounds{pointer().x + options.offset.x, pointer().y + options.offset.y, size.x, size.y};
    if (screen.w > 0.0f && screen.h > 0.0f) {
        bounds = clamp_rect_to_bounds(bounds, screen);
    }

    region(id, bounds, options.z);
    fill_rect(bounds, options.fill);
    outline_rect(bounds, options.border);
    text(value, {bounds.x + options.padding.left, bounds.y + options.padding.top}, options.text_style);
    result.visible = true;
    result.bounds = bounds;
    return result;
}

Context::DragSourceResult Context::drag_source(Id id, Rectf bounds, DragPayload payload, i32 z) {
    DragSourceResult result;
    result.interaction = region(id, bounds, z);
    if (!_input) {
        return result;
    }

    if (result.interaction.pressed) {
        _drag.source = id;
        _drag.start = pointer();
        _drag.payload = std::move(payload);
        _drag.dragging = false;
    }

    if (_drag.source == id && pointer_held()) {
        if (!_drag.dragging && distance_sq(pointer(), _drag.start) >= 16.0f) {
            _drag.dragging = true;
            _drag.started_this_frame = true;
        }
    }

    if (_drag.source == id && pointer_released()) {
        _drag.released_this_frame = true;
    }

    result.started = _drag.source == id && _drag.started_this_frame;
    result.dragging = _drag.source == id && _drag.dragging;
    result.released = _drag.source == id && _drag.released_this_frame;
    return result;
}

Context::DropTargetResult Context::drop_target(Id id, Rectf bounds, std::string_view accepted_type, i32 z) {
    DropTargetResult result;
    const Interaction it = region(id, bounds, z);
    result.hot = it.hot || (_input && contains(bounds, pointer()));
    result.accepts = _drag.dragging && same_type(_drag.payload.type, accepted_type);
    if (result.hot && result.accepts && pointer_released()) {
        result.dropped = true;
        result.payload = _drag.payload;
        _drag.released_this_frame = true;
    }
    return result;
}

Context::NavResult Context::nav_index(i32 count, i32 current, i32 columns, bool wrap) {
    NavResult result;
    if (count <= 0) {
        result.index = 0;
        result.activated = action_pressed("accept");
        result.cancelled = action_pressed("quit");
        return result;
    }

    const i32 safe_columns = std::max(1, columns);
    i32 next = std::clamp(current, 0, count - 1);
    const auto move = [&](i32 delta) {
        if (wrap) {
            next = (next + delta) % count;
            if (next < 0) {
                next += count;
            }
        } else {
            next = std::clamp(next + delta, 0, count - 1);
        }
    };

    if (action_pressed("menu_left")) {
        move(-1);
    }
    if (action_pressed("menu_right")) {
        move(1);
    }
    if (action_pressed("menu_up")) {
        move(-safe_columns);
    }
    if (action_pressed("menu_down")) {
        move(safe_columns);
    }

    result.index = next;
    result.changed = next != current;
    result.activated = action_pressed("accept");
    result.cancelled = action_pressed("quit");
    return result;
}

std::string Context::prompt_for_action(std::string_view action) const {
    return prompt_for_action(action, {});
}

std::string Context::prompt_for_action(std::string_view action, const PromptOptions& options) const {
    if (!_input) {
        return options.fallback.empty() ? readable_action(action) : options.fallback;
    }
    const std::vector<InputBinding>* bindings = _input->map().bindings(action);
    if (!bindings || bindings->empty()) {
        return options.fallback.empty() ? readable_action(action) : options.fallback;
    }
    if (options.mode == PromptBindingMode::First) {
        return prompt_binding_label(bindings->front(), options);
    }
    std::string result;
    for (std::size_t i = 0; i < bindings->size(); ++i) {
        if (i > 0) {
            result += options.separator;
        }
        result += prompt_binding_label((*bindings)[i], options);
    }
    return result;
}

Vec2f Context::pointer() const {
    if (!_input) {
        return {};
    }
    if (_renderer) {
        return _renderer->window_to_logical(_input->mouse_pos());
    }
    return _input->mouse_pos();
}

bool Context::pointer_pressed(MouseButton button) const {
    return _input && _input->mouse_frame_pressed(button);
}

bool Context::pointer_held(MouseButton button) const {
    return _input && _input->mouse_held(button);
}

bool Context::pointer_released(MouseButton button) const {
    return _input && _input->mouse_frame_released(button);
}

f32 Context::mouse_wheel_y() const {
    return _input ? _input->mouse_wheel_y() : 0.0f;
}

bool Context::key_pressed(Key key) const {
    return _input && _input->frame_pressed(key);
}

bool Context::key_typed(Key key) const {
    return _input && (_input->frame_pressed(key) || _input->frame_repeated(key));
}

bool Context::action_pressed(std::string_view action) const {
    return _input && _input->frame_pressed(action);
}

bool Context::modifier_held(KeyModifiers modifiers) const {
    return _input && _input->modifier_held(modifiers);
}

std::string_view Context::text_input() const {
    return _input ? _input->text_input() : std::string_view{};
}

std::string Context::clipboard_text() const {
    return _input ? _input->clipboard_text() : std::string{};
}

void Context::set_clipboard_text(std::string_view text) {
    if (_input) {
        _input->set_clipboard_text(text);
    }
}

UiTextEditState& Context::text_edit_state(Id id, std::string_view initial) {
    const auto [it, inserted] = _text_edits.try_emplace(id.value);
    if (inserted) {
        it->second.text = std::string{initial};
        it->second.caret = it->second.text.size();
        it->second.clear_selection();
    }
    return it->second;
}

UiTextInputState& Context::text_input_state(Id id, std::string_view value) {
    UiTextInputState& state = _text_inputs[id.value];
    if (state.text.empty() && state.caret == 0 && state.selection_anchor == UiTextInputState::unset_selection && !state.active) {
        state.text = std::string{value};
        state.caret = state.text.size();
        state.clear_selection();
        return state;
    }
    if (!state.active && std::string_view{state.text} != value) {
        state.text = std::string{value};
        state.caret = std::min(state.caret, state.text.size());
        state.clear_selection();
    }
    return state;
}

UiComboState& Context::combo_state(Id id, i32 selected) {
    UiComboState& state = _combos[id.value];
    if (!state.open) {
        state.selected = selected;
        state.menu.selected = selected;
    }
    return state;
}

ColorPickerMode& Context::color_picker_mode(Id id, ColorPickerMode mode) {
    const auto [it, inserted] = _color_picker_modes.emplace(id.value, mode);
    if (!static_cast<bool>(id) && !inserted) {
        it->second = mode;
    }
    return it->second;
}

namespace {

Rectf rect_intersect(Rectf a, Rectf b) {
    const f32 x0 = std::max(a.x, b.x);
    const f32 y0 = std::max(a.y, b.y);
    const f32 x1 = std::min(a.x + a.w, b.x + b.w);
    const f32 y1 = std::min(a.y + a.h, b.y + b.h);
    return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

} // namespace

Context::DrawScope Context::draw_scope(std::string_view widget, Rectf bounds, f32 corner_radius) {
    push_draw_scope(widget, bounds, corner_radius);
    return DrawScope{this};
}

void Context::push_draw_scope(std::string_view widget, Rectf bounds, f32 corner_radius) {
    _draw_scopes.push_back({std::string{widget}, bounds, std::max(0.0f, corner_radius)});
}

void Context::pop_draw_scope() {
    if (!_draw_scopes.empty()) {
        _draw_scopes.pop_back();
    }
}

void Context::emit_diagnostic(DiagnosticKind kind, std::string_view widget, std::string detail, bool log) {
    if (log) {
        KIN_LOG_WARN_F("ui",
                       "ui2 invariant",
                       (LogFields{{.name = "widget", .value = std::string{widget}},
                                  {.name = "detail", .value = detail}}));
    }
    _diagnostics.push_back(Diagnostic{
        .kind = kind,
        .widget = std::string{widget},
        .detail = std::move(detail),
    });
}

Color Context::debug_fill_color(Rectf rect) {
    // Keys depend only on POSITION (x / y), never SIZE (w / h): a fill whose width or height
    // animates each frame (e.g. progress bars) must keep a stable color rather than flicker.
    // Hue identifies the component: the active draw scope when present (so every part of one
    // widget — body, header, each zebra row — shares a hue), else the fill's horizontal slot
    // (so side-by-side elements differ while a vertical stack stays coherent).
    u32 hue_key = 0;
    if (!_draw_scopes.empty()) {
        const DrawScopeEntry& scope = _draw_scopes.back();
        u32 h = 2166136261U;
        for (const char c : scope.widget) {
            h = (h ^ static_cast<u32>(static_cast<unsigned char>(c))) * 16777619U;
        }
        hue_key = h ^ debug_mix(static_cast<i32>(std::round(scope.bounds.x)), static_cast<i32>(std::round(scope.bounds.y)));
    } else {
        hue_key = debug_hash(static_cast<u32>(static_cast<i32>(std::round(rect.x))));
    }
    // Vertical position drives the variation so stacked rows read as a coherent-but-distinct
    // zebra within a shared hue family, instead of one flat block: a wide brightness spread
    // plus a small hue jitter (±0.04) keeps adjacent rows clearly different yet related.
    const u32 row_key = debug_hash(static_cast<u32>(static_cast<i32>(std::round(rect.y))));
    const f32 hue = std::fmod(debug_hash01(hue_key) + (debug_hash01(row_key) - 0.5f) * 0.08f + 1.0f, 1.0f);
    const f32 value = 0.35f + 0.6f * debug_hash01(row_key);
    const f32 sat = 0.75f + 0.2f * debug_hash01(hue_key);
    _debug_last_hue = hue;
    return hsv_color(hue, sat, value, 230);
}

void Context::record_draw(DrawOp::Kind kind, Rectf rect, f32 radius, f32 width, Color color) {
    if (!_debug.record_draws || color.a == 0) {
        return;
    }
    DrawOp op;
    op.kind = kind;
    op.rect = rect;
    op.radius = radius;
    op.width = width;
    op.color = color;
    op.shadow = _in_shadow;
    if (!_clip_stack.empty()) {
        op.clipped = true;
        op.clip = _clip_stack.back();
    }
    if (!_draw_scopes.empty()) {
        op.widget = _draw_scopes.back().widget;
    }
    _draw_ops.push_back(std::move(op));
}

namespace {

bool rounded_contains(Rectf r, f32 radius, Vec2f p) {
    if (p.x < r.x || p.y < r.y || p.x > r.x + r.w || p.y > r.y + r.h) {
        return false;
    }
    const f32 rad = std::min(radius, std::min(r.w, r.h) * 0.5f);
    if (rad <= 0.0f) {
        return true;
    }
    // Outside the corner squares the point is always inside; inside a corner square it must lie
    // within the corner circle.
    const f32 cx = p.x < r.x + rad ? r.x + rad : (p.x > r.x + r.w - rad ? r.x + r.w - rad : p.x);
    const f32 cy = p.y < r.y + rad ? r.y + rad : (p.y > r.y + r.h - rad ? r.y + r.h - rad : p.y);
    const f32 dx = p.x - cx;
    const f32 dy = p.y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

bool draw_op_contains(const Context::DrawOp& op, Vec2f p) {
    using Kind = Context::DrawOp::Kind;
    switch (op.kind) {
    case Kind::Fill:
    case Kind::Gradient:
    case Kind::Sprite:
    case Kind::Texture:
    case Kind::Text:
        return contains(op.rect, p);
    case Kind::RoundedFill:
        return rounded_contains(op.rect, op.radius, p);
    case Kind::Outline: {
        const f32 w = std::max(1.0f, op.width);
        const Rectf inner{op.rect.x + w, op.rect.y + w, std::max(0.0f, op.rect.w - w * 2.0f), std::max(0.0f, op.rect.h - w * 2.0f)};
        return contains(op.rect, p) && !contains(inner, p);
    }
    case Kind::RoundedOutline: {
        const f32 w = std::max(1.0f, op.width);
        const Rectf inner{op.rect.x + w, op.rect.y + w, std::max(0.0f, op.rect.w - w * 2.0f), std::max(0.0f, op.rect.h - w * 2.0f)};
        return rounded_contains(op.rect, op.radius, p) && !rounded_contains(inner, std::max(0.0f, op.radius - w), p);
    }
    case Kind::Line: {
        const Vec2f a{op.rect.x, op.rect.y};
        const Vec2f b{op.rect.x + op.rect.w, op.rect.y + op.rect.h};
        const f32 len2 = op.rect.w * op.rect.w + op.rect.h * op.rect.h;
        const f32 t = len2 > 0.0f ? std::clamp(((p.x - a.x) * op.rect.w + (p.y - a.y) * op.rect.h) / len2, 0.0f, 1.0f) : 0.0f;
        const f32 dx = p.x - (a.x + t * op.rect.w);
        const f32 dy = p.y - (a.y + t * op.rect.h);
        (void)b;
        return dx * dx + dy * dy <= 1.5f * 1.5f;
    }
    }
    return false;
}

// Standard straight-alpha src-over compositing.
Color over(Color src, Color dst) {
    const f32 sa = static_cast<f32>(src.a) / 255.0f;
    const f32 da = static_cast<f32>(dst.a) / 255.0f;
    const f32 oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f) {
        return Color::rgba(0, 0, 0, 0);
    }
    auto chan = [&](u8 s, u8 d) {
        const f32 v = (static_cast<f32>(s) * sa + static_cast<f32>(d) * da * (1.0f - sa)) / oa;
        return static_cast<u8>(std::clamp(v, 0.0f, 255.0f));
    };
    return Color::rgba(chan(src.r, dst.r), chan(src.g, dst.g), chan(src.b, dst.b), static_cast<u8>(std::clamp(oa * 255.0f, 0.0f, 255.0f)));
}

std::string_view draw_op_role(const Context::DrawOp& op) {
    using Kind = Context::DrawOp::Kind;
    if (op.shadow) {
        return "shadow";
    }
    switch (op.kind) {
    case Kind::Outline:
    case Kind::RoundedOutline: return "border";
    case Kind::Text: return "text";
    case Kind::Sprite: return "sprite";
    case Kind::Texture: return "texture";
    case Kind::Gradient: return "gradient";
    case Kind::Line: return "line";
    default: return "fill";
    }
}

} // namespace

std::vector<SolitaryPixel> find_solitary_pixels(const u8* rgba, i32 width, i32 height,
                                                SolitaryPixelOptions options, std::size_t max_results) {
    std::vector<SolitaryPixel> out;
    if (!rgba || width < 3 || height < 3) {
        return out;
    }
    const auto px = [&](i32 x, i32 y) -> const u8* {
        return rgba + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    };
    const auto adiff = [](const u8* a, i32 r, i32 g, i32 b) {
        const i32 dr = static_cast<i32>(a[0]) - r;
        const i32 dg = static_cast<i32>(a[1]) - g;
        const i32 db = static_cast<i32>(a[2]) - b;
        return (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db);
    };
    constexpr i32 dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    constexpr i32 dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (i32 y = 1; y < height - 1 && out.size() < max_results; ++y) {
        for (i32 x = 1; x < width - 1 && out.size() < max_results; ++x) {
            const u8* nb[8];
            i32 sr = 0;
            i32 sg = 0;
            i32 sb = 0;
            for (i32 i = 0; i < 8; ++i) {
                nb[i] = px(x + dx[i], y + dy[i]);
                sr += nb[i][0];
                sg += nb[i][1];
                sb += nb[i][2];
            }
            const i32 ar = sr / 8;
            const i32 ag = sg / 8;
            const i32 ab = sb / 8;
            bool uniform = true;
            for (i32 i = 0; i < 8; ++i) {
                if (adiff(nb[i], ar, ag, ab) > options.uniformity) {
                    uniform = false;
                    break;
                }
            }
            if (!uniform) {
                continue;
            }
            const u8* p = px(x, y);
            if (adiff(p, ar, ag, ab) <= options.threshold) {
                continue;
            }
            out.push_back(SolitaryPixel{{x, y}, Color::rgba(p[0], p[1], p[2], p[3])});
        }
    }
    return out;
}

Context::PixelCausation Context::pixel_causation(Vec2f point) const {
    PixelCausation out;
    out.point = point;
    Color acc = Color::rgba(0, 0, 0, 0);
    for (const DrawOp& op : _draw_ops) {
        if (op.clipped && !contains(op.clip, point)) {
            continue;
        }
        if (!draw_op_contains(op, point)) {
            continue;
        }
        acc = over(op.color, acc);
        out.layers.push_back(PixelLayer{op.widget, std::string{draw_op_role(op)}, op.color, acc});
    }
    out.result = acc;
    return out;
}

void Context::check_degenerate(std::string_view kind, Rectf rect) {
    const bool finite = std::isfinite(rect.x) && std::isfinite(rect.y) &&
                        std::isfinite(rect.w) && std::isfinite(rect.h);
    if (finite) {
        return;
    }
    const std::string_view widget = _draw_scopes.empty() ? std::string_view{} : std::string_view{_draw_scopes.back().widget};
    emit_diagnostic(DiagnosticKind::DegenerateRect, widget, std::string{kind} + ": non-finite coordinates", _debug.log_invariants);
}

void Context::check_region_id(Id id) {
    if (!_debug.detect_invariants || !id) {
        return;
    }
    // First sight registers the id; a second sight in the same frame is a collision (shared
    // hot/active/focus state). Report each colliding id once per frame.
    if (_frame_region_ids.insert(id.value).second) {
        return;
    }
    if (_frame_dup_reported.insert(id.value).second) {
        const std::string_view widget = _draw_scopes.empty() ? std::string_view{} : std::string_view{_draw_scopes.back().widget};
        emit_diagnostic(DiagnosticKind::DuplicateId, widget,
                        "duplicate interaction id " + std::to_string(id.value), _debug.log_invariants);
    }
}

void Context::check_draw(std::string_view kind, Rectf rect, bool corner_exempt) {
    if (_in_shadow) {
        return;
    }
    if (_debug.detect_invariants) {
        check_degenerate(kind, rect);
    }
    if (!_debug.detect_draw_overflow || _draw_scopes.empty()) {
        return;
    }
    // Only count pixels that survive clipping: the renderer clips to the active clip rect,
    // so the truly painted region is the draw rect intersected with it. This lets virtualized
    // rows (which over-draw and rely on the body clip) pass while real bleed still trips.
    Rectf painted = rect;
    if (!_clip_stack.empty()) {
        painted = rect_intersect(rect, _clip_stack.back());
    }
    if (painted.w <= 0.0f || painted.h <= 0.0f) {
        return;
    }
    const DrawScopeEntry& scope = _draw_scopes.back();
    const Rectf b = scope.bounds;
    // 1px slack absorbs pixel-snapping (Context::surface rounds edges) and AA fringes.
    constexpr f32 epsilon = 1.0f;

    auto report = [&](std::string detail, Vec2f overflow) {
        Diagnostic diagnostic{
            .kind = DiagnosticKind::DrawOverflow,
            .widget = scope.widget,
            .detail = std::move(detail),
            .bounds = b,
            .wanted = {painted.x + painted.w, painted.y + painted.h},
            .overflow = overflow,
        };
        if (_debug.log_draw_overflow) {
            KIN_LOG_WARN_F("ui",
                           "ui2 draw overflow",
                           (LogFields{{.name = "widget", .value = diagnostic.widget},
                                      {.name = "primitive", .value = diagnostic.detail},
                                      {.name = "bounds", .value = std::to_string(b.x) + "," + std::to_string(b.y) + " " +
                                                                  std::to_string(b.w) + "x" + std::to_string(b.h)},
                                      {.name = "painted", .value = std::to_string(painted.x) + "," + std::to_string(painted.y) + " " +
                                                                   std::to_string(painted.w) + "x" + std::to_string(painted.h)},
                                      {.name = "overflow", .value = std::to_string(overflow.x) + "x" + std::to_string(overflow.y)}}));
        }
        _diagnostics.push_back(std::move(diagnostic));
    };

    // 1) Rectangular containment against the bounding box.
    const Vec2f overflow{
        std::max(0.0f, std::max(b.x - painted.x, (painted.x + painted.w) - (b.x + b.w))),
        std::max(0.0f, std::max(b.y - painted.y, (painted.y + painted.h) - (b.y + b.h))),
    };
    if (overflow.x > epsilon || overflow.y > epsilon) {
        report(std::string{kind}, overflow);
        return;
    }

    // 2) Per-corner arc test for square primitives inside a rounded panel. A square fill that
    // reaches into a corner quadrant but lands beyond the outer corner arc pokes past the
    // visible rounded border even though it is inside the bounding rectangle.
    if (corner_exempt || scope.radius <= epsilon) {
        return;
    }
    const f32 R = scope.radius;
    const f32 l = painted.x;
    const f32 t = painted.y;
    const f32 r2 = painted.x + painted.w;
    const f32 btm = painted.y + painted.h;
    f32 worst = 0.0f;
    auto corner = [&](bool reach, f32 cx, f32 cy, f32 ox, f32 oy) {
        if (!reach) {
            return;
        }
        const f32 dx = cx - ox;
        const f32 dy = cy - oy;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy) - R);
    };
    corner(l < b.x + R && t < b.y + R, l, t, b.x + R, b.y + R);                                 // top-left
    corner(r2 > b.x + b.w - R && t < b.y + R, r2, t, b.x + b.w - R, b.y + R);                   // top-right
    corner(l < b.x + R && btm > b.y + b.h - R, l, btm, b.x + R, b.y + b.h - R);                 // bottom-left
    corner(r2 > b.x + b.w - R && btm > b.y + b.h - R, r2, btm, b.x + b.w - R, b.y + b.h - R);   // bottom-right
    if (worst > epsilon) {
        report(std::string{kind} + " (corner)", {worst, worst});
    }
}

void Context::push_clip(Rectf bounds) {
    _clip_stack.push_back(_clip_stack.empty() ? bounds : rect_intersect(_clip_stack.back(), bounds));
    if (_renderer) {
        _renderer->push_clip(bounds);
    }
}

void Context::pop_clip() {
    if (!_clip_stack.empty()) {
        _clip_stack.pop_back();
    }
    if (_renderer) {
        _renderer->pop_clip();
    }
}

void Context::fill_rect(Rectf rect, Color color) {
    if (_renderer && color.a > 0) {
        check_draw("fill_rect", rect);
        // Direct fills (e.g. zebra row stripes) get a stable wireframe color too. surface()'s
        // internal fills run with the flag temporarily off, so they aren't re-tinted here.
        if (_theme.debug_component_tint && !_in_shadow) {
            color = debug_fill_color(rect);
        }
        if (!_in_shadow && color.a >= 250) {
            _surface_fill_under = color;
        }
        record_draw(DrawOp::Kind::Fill, rect, 0.0f, 0.0f, color);
        _renderer->fill_rect(rect, color);
    }
}

void Context::outline_rect(Rectf rect, Color color) {
    if (_renderer && color.a > 0) {
        check_draw("outline_rect", rect);
        record_draw(DrawOp::Kind::Outline, rect, 0.0f, 1.0f, color);
        _renderer->draw_rect(rect, color);
    }
}

void Context::outline_rect(Rectf rect, Color color, f32 width) {
    if (!_renderer || color.a == 0 || width <= 0.0f) {
        return;
    }
    check_draw("outline_rect", rect);
    record_draw(DrawOp::Kind::Outline, rect, 0.0f, width, color);
    const i32 steps = std::max(1, static_cast<i32>(std::ceil(width)));
    for (i32 i = 0; i < steps; ++i) {
        const f32 inset_px = static_cast<f32>(i);
        Rectf line_rect{
            rect.x + inset_px,
            rect.y + inset_px,
            std::max(0.0f, rect.w - inset_px * 2.0f),
            std::max(0.0f, rect.h - inset_px * 2.0f),
        };
        if (line_rect.w > 0.0f && line_rect.h > 0.0f) {
            _renderer->draw_rect(line_rect, color);
        }
    }
}

void Context::fill_rounded_rect(Rectf rect, f32 radius, Color color) {
    if (_renderer && color.a > 0) {
        check_draw("fill_rounded_rect", rect, true);
        if (_theme.debug_component_tint && !_in_shadow) {
            color = debug_fill_color(rect);
        }
        if (!_in_shadow && color.a >= 250) {
            _surface_fill_under = color;
        }
        record_draw(DrawOp::Kind::RoundedFill, rect, radius, 0.0f, color);
        _renderer->fill_rounded_rect(rect, radius, color);
    }
}

void Context::outline_rounded_rect(Rectf rect, f32 radius, Color color, f32 width) {
    if (_renderer && color.a > 0 && width > 0.0f) {
        check_draw("outline_rounded_rect", rect, true);
        record_draw(DrawOp::Kind::RoundedOutline, rect, radius, width, color);
        _renderer->draw_rounded_rect(rect, radius, color, width);
    }
}

void Context::fill_gradient_rect(Rectf rect, const Gradient& gradient) {
    if (!_renderer || rect.w <= 0.0f || rect.h <= 0.0f || (gradient.start.a == 0 && gradient.end.a == 0)) {
        return;
    }
    check_draw("fill_gradient_rect", rect);
    if (rect.w > 0.0f && rect.h > 0.0f && gradient.start.a >= 250 && gradient.end.a >= 250 && !_in_shadow) {
        _surface_fill_under = mix(gradient.start, gradient.end, 0.5f);
    }
    record_draw(DrawOp::Kind::Gradient, rect, 0.0f, 0.0f, mix(gradient.start, gradient.end, 0.5f));
    _renderer->fill_gradient_rect(rect, gradient);
}

Texture Context::shadow_mask(f32 corner, f32 margin, f32 soft) {
    const u64 key = (static_cast<u64>(std::lround(corner)) << 32) | static_cast<u32>(std::lround(margin));
    if (const auto it = _shadow_masks.find(key); it != _shadow_masks.end()) {
        return it->second.texture();
    }
    if (!_renderer || !_renderer->capabilities().render_targets) {
        return {};
    }

    const i32 side = static_cast<i32>(std::ceil(2.0f * (corner + margin))) + 2;
    RenderTarget baked = _renderer->create_render_target({side, side}, ScaleMode::Linear);
    if (!baked.valid()) {
        return {};
    }
    {
        auto guard = _renderer->scoped_render_target(baked);
        _renderer->clear(colors::transparent);
        _renderer->fill_rounded_rect(
            {margin, margin, static_cast<f32>(side) - margin * 2.0f, static_cast<f32>(side) - margin * 2.0f},
            corner,
            colors::white);
    }

    const i32 passes = std::clamp(1 + static_cast<i32>(soft / 3.0f), 1, 4);
    PooledTarget blurred = blur(*_renderer, baked, {.passes = passes, .radius = 1.0f});

    RenderTarget owned = _renderer->create_render_target({side, side}, ScaleMode::Linear);
    if (!owned.valid()) {
        return {};
    }
    {
        auto guard = _renderer->scoped_render_target(owned);
        _renderer->clear(colors::transparent);
        const Texture& source = blurred.valid() ? blurred.texture() : baked.texture();
        _renderer->draw_texture(source,
                                {0.0f, 0.0f, static_cast<f32>(side), static_cast<f32>(side)},
                                {0.0f, 0.0f, static_cast<f32>(side), static_cast<f32>(side)});
    }

    _shadow_masks.emplace(key, std::move(owned));
    return _shadow_masks.at(key).texture();
}

bool Context::ensure_glass_backdrop() {
    if (_glass_captured) {
        return _glass_backdrop.valid();
    }
    _glass_captured = true; // attempt once per frame, regardless of success
    if (!_renderer || !_renderer->capabilities().render_targets) {
        return false;
    }

    // read_rgba takes a logical region; derive the full-frame region from the
    // window's pixel extent so the backdrop covers everything drawn so far.
    const Vec2i px = _renderer->output_size();
    if (px.x <= 0 || px.y <= 0) {
        return false;
    }
    // Skip the readback on any frame the output size changed: a window resize
    // recreates the swapchain, and a mid-frame SDL_RenderReadPixels then corrupts
    // the GPU command list (crash). Degrade to the flat tint until the size is
    // stable for a frame, then capture normally.
    if (px != _glass_last_output) {
        _glass_last_output = px;
        return false;
    }
    const Vec2f tl = _renderer->window_to_logical({0.0f, 0.0f});
    const Vec2f br = _renderer->window_to_logical({static_cast<f32>(px.x), static_cast<f32>(px.y)});
    const Rectf region{tl.x, tl.y, br.x - tl.x, br.y - tl.y};
    if (region.w <= 0.0f || region.h <= 0.0f) {
        return false;
    }

    // Capture the frame-so-far for `region` (GPU-side blit when supported, readback
    // fallback otherwise) into a pooled Linear target, then blur it for the frost.
    PooledTarget captured = _renderer->capture_backdrop(region);
    if (!captured.valid()) {
        return false;
    }
    PooledTarget blurred = blur(*_renderer, captured.target(), {.passes = 3, .radius = 1.5f});
    if (!blurred.valid()) {
        return false;
    }
    _glass_capture_px = captured.size();
    _glass_backdrop = std::move(blurred);
    _glass_capture_region = region;
    return true;
}

void Context::surface(Rectf rect, const SurfaceStyle& style) {
    // Debug wireframe theme: paint each component its own stable vivid color (no shadow), with a
    // bright outline, so composition/nesting/overdraw are obvious. Honours the rect's radius.
    if (_theme.debug_component_tint && !_in_shadow) {
        // Respect draw_fill: outline-only surfaces (e.g. collection_outline, drawn last) must NOT
        // become a filled rect, or they'd paint over the component's own content (zebra rows).
        // Respect border_mode: a surface drawn borderless (e.g. a panel header that squares its
        // bottom with a strip) must stay borderless, or the forced white border leaks a rounded
        // arc where the shape should be flush — and panels would get a double outline.
        const bool show_border = style.border_mode != BorderMode::None;
        SurfaceStyle dbg{};
        dbg.draw_fill = style.draw_fill;
        dbg.fill = style.draw_fill ? debug_fill_color(rect) : colors::transparent;
        dbg.border = show_border ? Color::rgba(255, 255, 255, 220) : colors::transparent;
        dbg.border_width = show_border ? std::max(1.0f, style.border_width) : 0.0f;
        dbg.border_mode = show_border ? BorderMode::Inside : BorderMode::None;
        dbg.radius = style.radius;
        dbg.shadow.enabled = false;
        _theme.debug_component_tint = false; // avoid infinite recursion through this surface() call
        surface(rect, dbg);
        _theme.debug_component_tint = true;
        return;
    }
    // Snap rounded surfaces to integer pixels so corner AA fringes land on exact boundaries.
    // Fractional panel bounds (from splitter ratios etc.) otherwise produce blurry or
    // asymmetric arcs. All dependent rects (shadow, fill, border) are derived from rect
    // after snapping, so they stay mutually aligned.
    if (style.radius > 0.0f) {
        const f32 rx = std::round(rect.x);
        const f32 ry = std::round(rect.y);
        rect = {rx, ry, std::round(rect.x + rect.w) - rx, std::round(rect.y + rect.h) - ry};
    }
    // Per-surface opacity multiplies the alpha of every painted color (default 1 = no-op).
    const f32 op = std::clamp(style.opacity, 0.0f, 1.0f);
    const auto tint = [op](Color c) { return alpha_scaled(c, op); };
    // Focus / accent glow: an outset layered rounded-rect stroke painted behind the surface.
    if (style.glow_color.a > 0 && style.glow_size > 0.0f) {
        _in_shadow = true;
        const i32 steps = std::max(1, static_cast<i32>(std::ceil(style.glow_size)));
        for (i32 i = steps; i >= 1; --i) {
            const f32 spread = static_cast<f32>(i);
            const f32 a = 0.6f * (1.0f - static_cast<f32>(i - 1) / static_cast<f32>(steps));
            const Rectf glow_rect{rect.x - spread, rect.y - spread, rect.w + spread * 2.0f, rect.h + spread * 2.0f};
            fill_rounded_rect(glow_rect, std::max(0.0f, style.radius + spread), tint(alpha_scaled(style.glow_color, a)));
        }
        _in_shadow = false;
    }
    if (style.shadow.enabled && style.shadow.color.a > 0) {
        // Drop shadows intentionally extend past the widget's bounds; don't flag them.
        _in_shadow = true;
        const RendererBackendCapabilities caps =
            _renderer ? _renderer->capabilities() : RendererBackendCapabilities{};
        bool drew_soft = false;
        if (caps.render_targets) {
            // Soft shadow: a baked, blurred coverage mask drawn under the surface as a
            // colour-modded 9-slice (premultiplied-safe — colour mod, not alpha mod).
            const f32 soft = std::max(1.0f, style.shadow.spread);
            const f32 corner = std::max(0.0f, style.shadow.radius);
            const f32 margin = std::ceil(soft) + 1.0f;
            const Texture mask = shadow_mask(corner, margin, soft);
            if (mask.valid()) {
                const f32 slice = corner + margin;
                const Rectf dest{
                    rect.x + style.shadow.offset.x - margin,
                    rect.y + style.shadow.offset.y - margin,
                    rect.w + margin * 2.0f,
                    rect.h + margin * 2.0f,
                };
                const u8 a = static_cast<u8>(
                    std::clamp(std::round(static_cast<f32>(style.shadow.color.a) * op), 0.0f, 255.0f));
                const Color shadow_tint =
                    premultiply(Color::rgba(style.shadow.color.r, style.shadow.color.g, style.shadow.color.b, a));
                const Vec2i ms = mask.size();
                const UiNineSlice nine{
                    .sprite = {.texture = mask, .source = {0.0f, 0.0f, static_cast<f32>(ms.x), static_cast<f32>(ms.y)}},
                    .left = slice,
                    .top = slice,
                    .right = slice,
                    .bottom = slice,
                };
                draw_nine_slice(*this, nine, dest, shadow_tint);
                drew_soft = true;
            }
        }
        if (!drew_soft) {
            // Degrade: the original layered-rect halo.
            const i32 layers = std::max(1, style.shadow.layers);
            const i32 shadow_steps = layers > 1 ? layers + 1 : layers;
            for (i32 i = shadow_steps - 1; i >= 0; --i) {
                const f32 t = shadow_steps > 1 ? static_cast<f32>(i) / static_cast<f32>(shadow_steps - 1) : 1.0f;
                const f32 spread = style.shadow.spread * t;
                const f32 alpha = shadow_steps > 1 ? (0.65f / static_cast<f32>(shadow_steps)) : 1.0f;
                const Rectf shadow_rect{
                    rect.x + style.shadow.offset.x - spread,
                    rect.y + style.shadow.offset.y - spread,
                    rect.w + spread * 2.0f,
                    rect.h + spread * 2.0f,
                };
                fill_rounded_rect(shadow_rect, std::max(0.0f, style.shadow.radius + spread), tint(alpha_scaled(style.shadow.color, alpha)));
            }
        }
        _in_shadow = false;
    }
    if (style.use_skin && style.skin.sprite.valid()) {
        draw_nine_slice(*this, style.skin, rect, style.skin_tint);
        return;
    }
    Rectf fill_rect_value = rect;
    f32 fill_radius = style.radius;
    const bool inside_border = style.border_mode == BorderMode::Inside &&
                               style.border.a > 0 &&
                               style.border_width > 0.0f;
    if (inside_border) {
        const f32 inset_px = std::max(0.0f, style.border_width);
        fill_rect_value = {
            rect.x + inset_px,
            rect.y + inset_px,
            std::max(0.0f, rect.w - inset_px * 2.0f),
            std::max(0.0f, rect.h - inset_px * 2.0f),
        };
        fill_radius = std::max(0.0f, style.radius - inset_px);
    }
    if (inside_border && style.draw_fill) {
        if (style.radius > 0.0f) {
            fill_rounded_rect(rect, style.radius, tint(style.border));
        } else {
            fill_rect(rect, tint(style.border));
        }
    }
    if (style.draw_fill) {
        const RendererBackendCapabilities caps =
            _renderer ? _renderer->capabilities() : RendererBackendCapabilities{};
        SurfaceFill kind = style.fill_kind;
        if (kind == SurfaceFill::Shader && !caps.materials_2d) {
            kind = SurfaceFill::Solid; // no shader backend → fall back to the solid fill
        }
        const auto solid_fill = [&](Color color) {
            if (fill_radius > 0.0f) {
                fill_rounded_rect(fill_rect_value, fill_radius, color);
            } else {
                fill_rect(fill_rect_value, color);
            }
        };
        if (kind == SurfaceFill::Shader && style.shader && _renderer) {
            // materials_2d is true here (kind was forced to Solid above otherwise).
            check_draw("shader-fill", fill_rect_value, /*corner_exempt*/ true);
            if (!_in_shadow) {
                _surface_fill_under = style.fill; // best-effort for contrast checks
            }
            _renderer->draw_shader_surface(fill_rect_value, style.shader, style.shader_params);
        } else if (kind == SurfaceFill::Glass) {
            bool drew_glass = false;
            if (caps.render_targets && ensure_glass_backdrop()) {
                // Map this panel's logical rect to the captured-backdrop pixel space.
                const Rectf& cr = _glass_capture_region;
                const f32 sx = static_cast<f32>(_glass_capture_px.x) / cr.w;
                const f32 sy = static_cast<f32>(_glass_capture_px.y) / cr.h;
                const Rectf src{
                    (fill_rect_value.x - cr.x) * sx,
                    (fill_rect_value.y - cr.y) * sy,
                    fill_rect_value.w * sx,
                    fill_rect_value.h * sy,
                };
                // The backdrop is a rect sprite covering the (possibly rounded) surface;
                // the rounded tint below clips the visible fill, so exempt it from the
                // per-corner overflow test (it intentionally fills the corner squares).
                sprite(Sprite{.texture = _glass_backdrop.texture(), .source = src}, fill_rect_value,
                       colors::white, /*corner_exempt=*/true);
                solid_fill(tint(style.glass_tint)); // translucent tint over the blurred backdrop
                if (style.glass_highlight > 0.0f) {  // 1px frosted top-edge light
                    line({fill_rect_value.x, fill_rect_value.y + 0.5f},
                         {fill_rect_value.x + fill_rect_value.w, fill_rect_value.y + 0.5f},
                         tint(alpha_scaled(colors::white, style.glass_highlight)));
                }
                drew_glass = true;
            }
            if (!drew_glass) {
                solid_fill(tint(style.glass_tint)); // degrade: honest translucent scrim
            }
        } else if (kind == SurfaceFill::Gradient && caps.gradients && fill_radius <= 0.0f) {
            Gradient g = style.gradient;
            g.start = tint(g.start);
            g.end = tint(g.end);
            fill_gradient_rect(fill_rect_value, g);
        } else if (kind == SurfaceFill::Gradient) {
            // Rounded gradient or no gradient support → flat mid-color (deferred enhancement).
            solid_fill(tint(mix(style.gradient.start, style.gradient.end, 0.5f)));
        } else {
            solid_fill(tint(style.fill)); // Solid (and degraded Shader)
        }
    }
    // Bevel: 1px light on top/left inner edges, dark on bottom/right.
    if (style.bevel.enabled) {
        const Rectf b = fill_rect_value;
        const f32 x0 = b.x, y0 = b.y, x1 = b.x + b.w, y1 = b.y + b.h;
        if (style.bevel.light.a > 0) {
            line({x0, y0 + 0.5f}, {x1, y0 + 0.5f}, tint(style.bevel.light));
            line({x0 + 0.5f, y0}, {x0 + 0.5f, y1}, tint(style.bevel.light));
        }
        if (style.bevel.dark.a > 0) {
            line({x0, y1 - 0.5f}, {x1, y1 - 0.5f}, tint(style.bevel.dark));
            line({x1 - 0.5f, y0}, {x1 - 0.5f, y1}, tint(style.bevel.dark));
        }
    }
    // Inner shadow: a short dark vertical gradient at the top inside edge.
    if (style.inner_shadow.enabled && style.inner_shadow.depth > 0.0f && style.inner_shadow.color.a > 0) {
        const f32 depth = std::min(style.inner_shadow.depth, fill_rect_value.h);
        const Rectf inner{fill_rect_value.x, fill_rect_value.y, fill_rect_value.w, depth};
        fill_gradient_rect(inner, Gradient{tint(style.inner_shadow.color), colors::transparent, GradientDirection::Vertical});
    }
    const bool border_already_painted = inside_border && style.draw_fill;
    if (style.border_mode != BorderMode::None && !border_already_painted) {
        if (style.radius > 0.0f) {
            outline_rounded_rect(rect, style.radius, tint(style.border), style.border_width);
        } else {
            outline_rect(rect, tint(style.border), style.border_width);
        }
    }
}

void Context::draw_layout_surface(Rectf bounds, const LayoutStyle& style) {
    if (style.draw_surface) {
        surface(bounds, style.surface);
    }
}

void Context::line(Vec2f a, Vec2f b, Color color) {
    if (_renderer && color.a > 0) {
        check_draw("line", {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)});
        record_draw(DrawOp::Kind::Line, {a.x, a.y, b.x - a.x, b.y - a.y}, 0.0f, 1.0f, color);
        _renderer->draw_line(a, b, color);
    }
}

void Context::texture(const Texture& texture, Rectf bounds, Color tint) {
    if (_renderer && texture.valid() && bounds.w > 0.0f && bounds.h > 0.0f && tint.a > 0) {
        check_draw("texture", bounds);
        record_draw(DrawOp::Kind::Texture, bounds, 0.0f, 0.0f, tint);
        const Vec2i size = texture.size();
        _renderer->draw_texture(texture,
                                {0.0f, 0.0f, static_cast<f32>(size.x), static_cast<f32>(size.y)},
                                bounds,
                                tint);
    }
}

void Context::sprite(const Sprite& sprite, Rectf bounds, Color tint, bool corner_exempt) {
    if (_renderer && sprite.valid() && bounds.w > 0.0f && bounds.h > 0.0f && tint.a > 0) {
        check_draw("sprite", bounds, corner_exempt);
        record_draw(DrawOp::Kind::Sprite, bounds, 0.0f, 0.0f, tint);
        _renderer->draw_texture(sprite.texture, sprite.source, bounds, tint);
    }
}

void Context::text(std::string_view value, Vec2f pos, const TextStyle& style) {
    if (!_renderer) {
        return;
    }
    const bool want_overflow = _debug.detect_draw_overflow && !_draw_scopes.empty() && !value.empty();
    // Skip contrast in the debug wireframe theme (intentionally unreadable) and for faint text.
    const bool want_contrast = _debug.detect_contrast && !_theme.debug_component_tint && !value.empty() && style.color.a >= 200;
    if (want_overflow || want_contrast) {
        const Vec2f size = measure_text(style.font, value, style.scale);
        if (want_overflow) {
            // Text is corner-exempt: measure_text's line-box includes leading the glyph ink
            // never fills, which would false-positive the per-corner arc test.
            check_draw("text", {std::round(pos.x), std::round(pos.y), size.x, size.y}, true);
        }
        if (want_contrast) {
            const f32 ratio = contrast_ratio(style.color, _surface_fill_under);
            // WCAG AA: 3:1 for large text (>= ~24px), 4.5:1 otherwise.
            const f32 threshold = size.y >= 24.0f ? 3.0f : 4.5f;
            if (ratio + 0.05f < threshold) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(ratio));
                const std::string_view widget = _draw_scopes.empty() ? std::string_view{} : std::string_view{_draw_scopes.back().widget};
                emit_diagnostic(DiagnosticKind::LowContrast, widget,
                                std::string{"contrast "} + buf + ":1 < " +
                                    (threshold > 4.0f ? "4.5" : "3.0") + " text" + rgb_str(style.color) +
                                    " on" + rgb_str(_surface_fill_under) + " \"" + std::string{value.substr(0, 24)} + "\"",
                                _debug.log_contrast);
            }
        }
    }
    // Debug wireframe theme: paint the font a vivid color too — the complement of its
    // component's hue, so it stays distinct and readable over the tinted background.
    Color color = style.color;
    if (_theme.debug_component_tint) {
        color = hsv_color(std::fmod(_debug_last_hue + 0.5f, 1.0f), 1.0f, 1.0f, 255);
    }
    const Vec2f base{std::round(pos.x), std::round(pos.y)};
    // The drop shadow under the text, the outline round it (from the distance
    // field with an Sdf font, else four offset copies). Skipped in the debug
    // wireframe theme (already recoloured) for clarity.
    const bool outlined = !_theme.debug_component_tint && style.outline_color.a > 0 && style.outline_width > 0.0f;
    if (!_theme.debug_component_tint && style.shadow_color.a > 0) {
        draw_text(*_renderer, style.font, value,
                  {base.x + style.shadow_offset.x, base.y + style.shadow_offset.y}, style.scale, style.shadow_color);
    }
    if (_debug.record_draws && !value.empty()) {
        const Vec2f size = measure_text(style.font, value, style.scale);
        record_draw(DrawOp::Kind::Text, {base.x, base.y, size.x, size.y}, 0.0f, 0.0f, color);
    }
    if (outlined) {
        draw_text_outlined(*_renderer, style.font, value, base, style.scale, color, style.outline_width,
                           style.outline_color);
    } else {
        draw_text(*_renderer, style.font, value, base, style.scale, color);
    }
}

i32 Context::add_node(LayoutStyle style, Vec2f intrinsic) {
    const i32 index = static_cast<i32>(_nodes.size());
    const i32 parent = _stack.empty() ? -1 : _stack.back();

    LayoutNode node;
    node.style = style;
    node.intrinsic = intrinsic;
    node.parent = parent;
    _nodes.push_back(std::move(node));

    if (parent >= 0) {
        _nodes[static_cast<std::size_t>(parent)].children.push_back(index);
    }
    return index;
}

void Context::begin_container(LayoutStyle style) {
    const i32 index = add_node(style, {});
    _stack.push_back(index);
}

void Context::begin_layout(Rectf root) {
    _nodes.clear();
    _stack.clear();
    _pending.clear();
    _root = root;

    LayoutStyle style;
    style.width = fixed(root.w);
    style.height = fixed(root.h);
    add_node(style, {});
    _stack.push_back(0);
}

void Context::begin_row(LayoutStyle style) {
    style.axis = UiLayoutAxis::Horizontal;
    begin_container(style);
}

void Context::begin_wrap_row(LayoutStyle style) {
    style.axis = UiLayoutAxis::Horizontal;
    style.wrap = true;
    begin_container(style);
}

void Context::begin_column(LayoutStyle style) {
    style.axis = UiLayoutAxis::Vertical;
    begin_container(style);
}

void Context::begin_grid(i32 columns, LayoutStyle style) {
    style.grid_columns = std::max(1, columns);
    begin_container(style);
}

void Context::end_container() {
    if (_stack.size() > 1) {
        _stack.pop_back();
    }
}

void Context::end_layout() {
    if (_nodes.empty()) {
        return;
    }

    solve(_nodes, 0, _root, _layout_scratch);

    // Map each node to its widget (at most one per node; widget() makes a dedicated node).
    std::vector<i32> node_pending(_nodes.size(), -1);
    for (std::size_t p = 0; p < _pending.size(); ++p) {
        const i32 node = _pending[p].node;
        if (node >= 0 && node < static_cast<i32>(_nodes.size())) {
            node_pending[static_cast<std::size_t>(node)] = static_cast<i32>(p);
        }
    }

    // Single ordered draw stream, shared model with the ECS front-end: order by
    // (layer asc, document order), and a node's surface draws immediately before its own
    // widget. Nodes are stored pre-order, so a stable sort by layer keeps parents before
    // children and preserves document order within a layer. This interleaves correctly for
    // overlapping siblings instead of forcing all surfaces behind all widgets.
    struct DrawEntry {
        i32 layer = 0;
        i32 node = -1;
        bool widget = false;
    };
    // Effective layer = max(own layer, parent's effective layer). A child can never draw
    // behind an ancestor, while higher-layer subtrees still float above lower ones globally.
    // Nodes are stored pre-order (parent index < child index), so one forward pass resolves it.
    std::vector<i32> effective_layer(_nodes.size(), 0);
    for (std::size_t i = 0; i < _nodes.size(); ++i) {
        const i32 parent = _nodes[i].parent;
        const i32 base = parent >= 0 ? effective_layer[static_cast<std::size_t>(parent)] : 0;
        effective_layer[i] = std::max(_nodes[i].style.layer, base);
    }

    std::vector<DrawEntry> draws;
    draws.reserve(_nodes.size() * 2);
    for (std::size_t i = 0; i < _nodes.size(); ++i) {
        const i32 layer = effective_layer[i];
        draws.push_back({layer, static_cast<i32>(i), false});
        if (node_pending[i] >= 0) {
            draws.push_back({layer, static_cast<i32>(i), true});
        }
    }
    std::stable_sort(draws.begin(), draws.end(), [](const DrawEntry& lhs, const DrawEntry& rhs) {
        return lhs.layer < rhs.layer;
    });

    for (const DrawEntry& entry : draws) {
        const LayoutNode& node = _nodes[static_cast<std::size_t>(entry.node)];
        if (!entry.widget) {
            draw_layout_surface(node.solved, node.style);
            continue;
        }
        const i32 p = node_pending[static_cast<std::size_t>(entry.node)];
        if (p >= 0 && _pending[static_cast<std::size_t>(p)].finalize) {
            _pending[static_cast<std::size_t>(p)].finalize(*this, node.solved);
        }
    }

    _stack.clear();
}

Vec2f world_to_ui(const WorldOverlayContext& overlay, Vec2f world) {
    Vec2f screen = overlay.camera ? overlay.camera->world_to_screen(world) : world;
    screen.x += overlay.viewport.x;
    screen.y += overlay.viewport.y;
    return screen;
}

Rectf world_anchor_rect(const WorldOverlayContext& overlay, Vec2f world, Vec2f size, UiAnchor anchor, Vec2f offset) {
    const Vec2f pos = world_to_ui(overlay, world);
    return anchor_rect({pos.x, pos.y, 0.0f, 0.0f}, anchor, size, offset);
}

bool world_visible(const WorldOverlayContext& overlay, Rectf world_bounds) {
    if (overlay.camera && !overlay.camera->visible(world_bounds)) {
        return false;
    }
    const Rectf viewport = overlay.viewport.w > 0.0f && overlay.viewport.h > 0.0f ? overlay.viewport : Rectf{0, 0, 640, 360};
    const Rectf screen{
        world_to_ui(overlay, {world_bounds.x, world_bounds.y}).x,
        world_to_ui(overlay, {world_bounds.x, world_bounds.y}).y,
        world_bounds.w,
        world_bounds.h,
    };
    return overlaps(screen, viewport);
}

Rectf clamp_to_viewport(const WorldOverlayContext& overlay, Rectf bounds) {
    const Rectf viewport = overlay.viewport.w > 0.0f && overlay.viewport.h > 0.0f ? overlay.viewport : Rectf{0, 0, 640, 360};
    return clamp_rect_to_bounds(bounds, viewport);
}

} // namespace kin::ui2
