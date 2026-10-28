// A value changed by dragging over a widget: a dial's, a fader's, a number
// scrubbed.

#include <kin/ui2/context.hpp>

#include <algorithm>
#include <cmath>

namespace kin::ui2 {
namespace {

struct DragValueState {
    f64 fraction = 0.0; // where the drag has it, before steps (they would stall a slow drag)
    Vec2f last{};       // the pointer last frame
    Vec2f pressed_at{};
    bool moved = false; // a drag, not a click
};

} // namespace

DragValueResult Context::drag_value(Id id, Rectf bounds, f64& value, const DragValueOptions& options) {
    DragValueResult out;
    static_cast<Interaction&>(out) = region(id, bounds, options.z);
    const Scale& scale = options.scale;
    if (!scale.valid()) {
        return out;
    }
    const f64 before = value;
    const auto settle = [&](f64 fraction) {
        f64 v = scale.value(std::clamp(fraction, 0.0, 1.0));
        if (options.step > 0.0) {
            v = scale.min + std::round((v - scale.min) / options.step) * options.step;
        }
        value = std::clamp(v, scale.min, scale.max);
    };
    // Moved `along` (of the range) from where it is; at least a step.
    const auto nudge = [&](f64 along) {
        const f64 from = value;
        settle(std::clamp(scale.fraction(value), 0.0, 1.0) + along);
        if (value == from && options.step > 0.0) {
            value = std::clamp(from + (along > 0.0 ? options.step : -options.step), scale.min, scale.max);
        }
    };
    const f64 slow = modifier_held(KeyModifiers::Shift) ? options.fine : 1.0;

    DragValueState& drag = state<DragValueState>(id);
    const Vec2f p = pointer();
    if (out.pressed) {
        drag = {.fraction = std::clamp(scale.fraction(value), 0.0, 1.0), .last = p, .pressed_at = p};
    } else if (out.active) {
        drag.moved = drag.moved || std::abs(p.x - drag.pressed_at.x) + std::abs(p.y - drag.pressed_at.y) >= 3.0f;
        const f32 along = options.axis == UiLayoutAxis::Vertical ? drag.last.y - p.y : p.x - drag.last.x;
        if (along != 0.0f && options.travel > 0.0f) {
            drag.fraction = std::clamp(drag.fraction + static_cast<f64>(along / options.travel) * slow, 0.0, 1.0);
            settle(drag.fraction);
        }
        drag.last = p;
    }
    out.dragging = out.active && !out.released && drag.moved;
    if (out.clicked && !drag.moved && double_click(id) && options.reset) {
        value = std::clamp(*options.reset, scale.min, scale.max);
    }

    if (options.wheel > 0.0f) {
        take_wheel(id);
        if (out.hot && !out.active && mouse_wheel_y() != 0.0f) {
            nudge(static_cast<f64>(mouse_wheel_y() * options.wheel) * slow);
        }
    }
    if (options.focusable) {
        register_focusable(id, options.tab_index);
        if (out.focused && !_wants_text_input_last) {
            const i32 steps = (key_typed(Key::Up) || key_typed(Key::Right) ? 1 : 0) -
                              (key_typed(Key::Down) || key_typed(Key::Left) ? 1 : 0);
            if (steps != 0) {
                const f64 unit = options.step > 0.0 && scale.kind == ScaleKind::Linear ? options.step / (scale.max - scale.min) : 0.01;
                nudge(static_cast<f64>(steps) * unit * slow);
            }
        }
    }
    if (out.hot || out.active) {
        set_cursor(options.axis == UiLayoutAxis::Vertical ? SystemCursor::ResizeNS : SystemCursor::ResizeEW);
    }
    out.changed = value != before;
    return out;
}

} // namespace kin::ui2
