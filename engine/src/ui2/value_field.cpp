// Fields editing a value as text: shown unless typed in, committed when left,
// Esc puts the value back.

#include <kin/ui2/context.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace kin::ui2 {

std::string number_text(f64 value, i32 digits, bool whole) {
    char text[64];
    if (whole) {
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(std::llround(value)));
    } else {
        std::snprintf(text, sizeof text, "%.*g", std::clamp(digits, 1, 17), value == 0.0 ? 0.0 : value); // never "-0"
    }
    return text;
}

std::optional<f64> parse_number(std::string_view text, f64 min, f64 max, bool whole) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    f64 value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value) || value < min ||
        value > max || (whole && std::floor(value) != value)) {
        return std::nullopt;
    }
    return value;
}

ValueFieldResult Context::checked_value_field(Id id, std::string_view shown, const ValueFieldOptions& options, const void* check,
                                              bool (*valid)(const void*, std::string_view)) {
    ValueFieldResult result;
    // Fields not built for a while are let go.
    const u64 frame = _state.frame();
    if (frame % 240 == 0) {
        std::erase_if(_value_edits, [frame](const auto& kv) { return kv.second.frame + 240 < frame; });
    }
    ValueEdit& edit = _value_edits.try_emplace(id.value).first->second;
    edit.frame = frame;
    TextInput& input = edit.input;
    UiTextInputState& state = input.state;
    input.id = id;
    const auto show = [&](std::string_view text) {
        state.text = std::string(text);
        state.caret = std::min(state.caret, state.text.size());
        state.selection_anchor = UiTextInputState::unset_selection;
        state.scroll_x = 0.0f;
    };
    const auto takes = [&](std::string_view text) { return !valid || valid(check, text); };

    if (std::exchange(edit.start, false)) {
        show(shown);
        state.active = true;
        state.caret = state.text.size();
        state.selection_anchor = 0; // typing replaces it
        edit.was_active = true;
        edit.before = std::string(shown);
    } else if (state.active && !edit.was_active) {
        edit.before = std::string(shown); // typed in from last frame's run (a click, Tab)
    }
    // Typed in until last frame's run: left now (Enter, Tab, a press elsewhere, Esc).
    result.left = edit.was_active && !state.active;
    result.cancelled = std::exchange(input.result.cancelled, false);
    const bool entered = std::exchange(input.result.committed, false);
    const bool changed = std::exchange(input.result.changed, false);
    if (result.cancelled) {
        // Esc: the value again; a live field's from before the typing.
        if (options.live && edit.before != shown && takes(edit.before)) {
            result.committed = edit.before;
            show(edit.before);
        } else {
            show(shown);
        }
    } else if (result.left || entered) {
        if (state.text != shown && takes(state.text) && !options.live) {
            result.committed = state.text;
        }
        show(shown); // the value it was given; the caller's change shows next frame
    } else if (options.live && changed && state.active && state.text != shown && takes(state.text)) {
        result.committed = state.text;
    } else if (!state.active && state.text != shown) {
        show(shown); // the value changed elsewhere (an undo, another pick)
    }
    edit.was_active = state.active;
    result.editing = state.active;
    result.invalid = state.active && !takes(state.text);

    // Its look, and what is inside it beside the text.
    input.style = options.style ? *options.style : _theme.input;
    input.text_style = options.text_style ? *options.text_style : _theme.body_text;
    TextStyle faint = input.text_style;
    faint.color = _theme.colors.text_muted;
    if (options.faint_style) {
        faint = *options.faint_style;
    }
    const f32 prefix_w = options.prefix.empty() ? 0.0f : measure_text(faint.font, options.prefix, faint.scale).x + 6.0f;
    const f32 suffix_w = options.suffix.empty() ? 0.0f : measure_text(faint.font, options.suffix, faint.scale).x + 6.0f;
    input.style.padding.left += prefix_w;
    input.style.padding.right += suffix_w;
    if (result.invalid) {
        for (SurfaceStyle* surface : {&input.style.surface.normal, &input.style.surface.hovered, &input.style.surface.pressed,
                                      &input.style.surface.focused}) {
            surface->border = _theme.colors.solid_danger;
            surface->border_width = std::max(1.0f, surface->border_width);
            surface->border_mode = BorderMode::Inside;
        }
    }
    {
        const BoxScope holder(this, begin_box(make_id(id, "field"), BoxStyle{.layout = options.layout, .interactive = false}),
                              &Context::end_box);
        widget(input, {.width = grow()});
        const f32 inset = input.style.padding.left;
        if (state.text.empty() && !options.placeholder.empty()) {
            label(options.placeholder, faint, LayoutStyle{}.at(UiAnchor::CenterLeft, {inset, 0.0f}));
        }
        if (!options.prefix.empty()) {
            label(options.prefix, faint, LayoutStyle{}.at(UiAnchor::CenterLeft, {inset - prefix_w + 1.0f, 0.0f}));
        }
        if (!options.suffix.empty()) {
            label(options.suffix, faint, LayoutStyle{}.at(UiAnchor::CenterRight, {-(input.style.padding.right - suffix_w) - 4.0f, 0.0f}));
        }
    }
    return result;
}

void Context::edit_value(Id id) {
    _value_edits[id.value].start = true;
}

void Context::stop_value_fields() {
    for (auto& [id, edit] : _value_edits) {
        edit.input.state.active = false;
        edit.input.state.clear_selection();
        edit.was_active = false;
        edit.start = false;
        if (_state.is_focused(edit.input.id)) {
            _state.clear_focused();
        }
    }
}

std::optional<f64> Context::number_field(Id id, f64 value, const NumberFieldOptions& options) {
    const std::string shown = number_text(value, options.digits, options.whole);
    const ValueFieldResult field = value_field(id, shown, options.field, [&](std::string_view text) {
        return parse_number(text, options.min, options.max, options.whole).has_value();
    });
    return field.committed ? parse_number(*field.committed, options.min, options.max, options.whole) : std::nullopt;
}

} // namespace kin::ui2
