#include <kin/ui2/touch_controls.hpp>

#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/geometry.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <cmath>

namespace kin::ui2 {
namespace {

constexpr bool phone =
#if defined(__ANDROID__)
    true;
#else
    false;
#endif

// A finger's place (window coordinates) where the context draws.
Vec2f drawing(const Context& ctx, Vec2f window) {
    const Renderer2D* renderer = ctx.renderer();
    return renderer ? renderer->window_to_logical(window) : window;
}

// The finger a control has, if it is still down; else one that has just
// landed in `bounds` and that nothing has taken, now the control's.
const TouchPoint* take_finger(const Context& ctx, Input& input, std::optional<u64>& finger, Rectf bounds) {
    if (finger) {
        if (const TouchPoint* still = input.touch(*finger)) {
            return still;
        }
        finger.reset();
    }
    for (const TouchPoint& t : input.touches()) {
        if (!t.claimed && contains(bounds, drawing(ctx, t.start))) {
            finger = t.id;
            input.claim_touch(t.id); // no click on what is under the control
            return input.touch(t.id);
        }
    }
    return nullptr;
}

f32 length(Vec2f v) {
    return std::sqrt(v.x * v.x + v.y * v.y);
}

} // namespace

Vec2f measure(const TouchStick& stick) {
    const f32 side = std::max(stick.radius, 1.0f) * 2.0f;
    return {side, side};
}

Vec2f measure(const TouchButton&) {
    return {96.0f, 96.0f};
}

bool touch_controls_shown(const Context& ctx, TouchControlsShown shown) {
    if (shown == TouchControlsShown::Always) {
        return true;
    }
    const Input* input = ctx.input();
    return phone || (input && (input->pointer_is_touch() || !input->touches().empty()));
}

void run(Context& ctx, TouchStick& stick) {
    stick.value = {};
    stick.active = false;
    Input* input = ctx.input();
    if (!input || !touch_controls_shown(ctx, stick.shown)) {
        stick.finger.reset();
        stick.mouse = false;
        return;
    }
    const f32 radius = std::max(stick.radius, 1.0f);
    // Where it rests: the middle of its bounds, or, floating, near its bottom
    // left corner (the thumb's place on a stick under the left hand).
    const Vec2f middle{stick.bounds.x + stick.bounds.w * 0.5f, stick.bounds.y + stick.bounds.h * 0.5f};
    Vec2f rest = middle;
    if (stick.floating) {
        rest = {std::min(stick.bounds.x + radius * 1.6f, middle.x), std::max(stick.bounds.y + stick.bounds.h - radius * 1.6f, middle.y)};
    }

    const Interaction it = ctx.region(stick.id, stick.bounds);
    const bool had = stick.finger.has_value();
    Vec2f at{};
    if (const TouchPoint* finger = take_finger(ctx, *input, stick.finger, stick.bounds)) {
        stick.mouse = false;
        if (!had) {
            stick.origin = stick.floating ? drawing(ctx, finger->start) : middle;
        }
        at = drawing(ctx, finger->pos);
        stick.active = true;
    } else if (!input->pointer_is_touch()) {
        // The mouse: a desktop showing the controls (TouchControlsShown::Always).
        if (it.pressed) {
            stick.mouse = true;
            stick.origin = stick.floating ? ctx.pointer() : middle;
        }
        stick.mouse = stick.mouse && ctx.pointer_held();
        if (stick.mouse) {
            at = ctx.pointer();
            stick.active = true;
        }
    } else {
        stick.mouse = false;
    }

    // How far from the middle, the knob's length at most.
    Vec2f pushed{};
    if (stick.active) {
        pushed = {(at.x - stick.origin.x) / radius, (at.y - stick.origin.y) / radius};
        const f32 far = length(pushed);
        if (far > 1.0f) {
            pushed = {pushed.x / far, pushed.y / far};
        }
        const f32 reach = std::min(far, 1.0f);
        const f32 dead = std::clamp(stick.dead_zone, 0.0f, 0.95f);
        if (reach > dead) {
            const f32 stretch = (reach - dead) / (1.0f - dead) / reach;
            stick.value = {pushed.x * stretch, pushed.y * stretch};
        }
    }
    // Set each frame, at 0 too, so the axis is let go a frame after the stick goes.
    if (!stick.axis_x.empty()) {
        input->set_screen_axis(stick.axis_x, stick.value.x);
    }
    if (!stick.axis_y.empty()) {
        input->set_screen_axis(stick.axis_y, stick.value.y);
    }

    const Vec2f centre = stick.active ? stick.origin : rest;
    const Vec2f knob{centre.x + pushed.x * radius, centre.y + pushed.y * radius};
    ctx.fill_circle(centre, radius, stick.base);
    ctx.fill_circle(knob, radius * 0.45f, stick.active ? stick.knob : Color::rgba(stick.knob.r, stick.knob.g, stick.knob.b, stick.knob.a / 2));
}

void run(Context& ctx, TouchButton& button) {
    const bool was = button.held;
    button.held = false;
    button.pressed = false;
    Input* input = ctx.input();
    if (!input || !touch_controls_shown(ctx, button.shown)) {
        button.finger.reset();
        button.mouse = false;
        return;
    }
    const Interaction it = ctx.region(button.id, button.bounds);
    // A finger that slides off a little still holds it; further, it lets go
    // (and holds it again coming back).
    const f32 slop = std::min(button.bounds.w, button.bounds.h) * 0.25f;
    const Rectf over{button.bounds.x - slop, button.bounds.y - slop, button.bounds.w + slop * 2.0f, button.bounds.h + slop * 2.0f};
    if (const TouchPoint* finger = take_finger(ctx, *input, button.finger, button.bounds)) {
        button.mouse = false;
        button.held = contains(over, drawing(ctx, finger->pos));
    } else if (!input->pointer_is_touch()) {
        if (it.pressed) {
            button.mouse = true;
        }
        button.mouse = button.mouse && ctx.pointer_held();
        button.held = button.mouse && contains(over, ctx.pointer());
    } else {
        button.mouse = false;
    }
    button.pressed = button.held && !was;
    if (!button.action.empty()) {
        input->set_screen_action(button.action, button.held);
    }

    const f32 round = std::min(button.bounds.w, button.bounds.h) * 0.5f;
    ctx.fill_rounded_rect(button.bounds, round, button.held ? button.held_fill : button.fill);
    if (!button.label.empty()) {
        TextStyle style = button.text_style.font ? button.text_style : ctx.theme().body_text;
        style.color = button.label_color;
        if (style.font) {
            const Vec2f size = measure_text(style.font, button.label, style.scale);
            ctx.text(button.label,
                     {button.bounds.x + (button.bounds.w - size.x) * 0.5f, button.bounds.y + (button.bounds.h - size.y) * 0.5f},
                     style);
        }
    }
}

} // namespace kin::ui2
