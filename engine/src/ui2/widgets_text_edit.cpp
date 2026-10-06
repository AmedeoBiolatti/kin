#include "widget_chunk_preamble.hpp"

#include <kin/core/utf8.hpp>

#include <functional>

namespace kin::ui2 {

namespace {

// Kinds of edit, for grouping undo steps: consecutive typing (or deleting) at
// the caret is one step; anything else starts a new one.
enum class EditKind : u8 {
    None,
    Typing,
    Deleting,
    Other,
};

constexpr u64 double_click_frames = 30;
constexpr f32 double_click_distance = 4.0f;
constexpr u64 caret_blink_frames = 32;

// Per-frame geometry of an editor.
struct EditView {
    Rectf inner{};    // bounds inside the padding
    Rectf viewport{}; // the text area: inner, less the scrollbar
    f32 line_height = 0.0f;
    f32 pitch = 0.0f; // line_height + line_spacing
    f32 content_height = 0.0f;
};

class Editor {
public:
    Editor(Context& ctx, TextEdit& widget, UiTextEditState& state)
        : _ctx(ctx), _w(widget), _s(state), _frame(ctx.state().frame()) {}

    void run();

private:
    const std::vector<TextRange>& lines() {
        ensure_layout();
        return _s.layout.lines;
    }
    void ensure_layout();
    void update_view();
    void clamp_scroll_offset();

    std::size_t line_of(std::size_t offset);
    std::size_t line_last_offset(std::size_t line);
    f32 x_in_line(std::size_t line, std::size_t offset);
    // Where a line starts: 0, or, right to left, so that it ends at the right.
    f32 line_start(std::size_t line);
    bool right_to_left() const { return paragraph_direction(_s.text) == TextDirection::RightToLeft; }
    std::size_t offset_at_x(std::size_t line, f32 x);
    std::size_t offset_at(Vec2f pointer);

    void move_caret(std::size_t offset, bool select, bool keep_column = false);
    void move_vertical(i32 lines, bool select);
    bool edit(std::size_t from, std::size_t to, std::string_view insert, EditKind kind);
    bool replace_selection(std::string_view insert, EditKind kind);
    void push_undo(std::vector<UiTextEditState::Snapshot>& steps);
    bool undo();
    bool redo();

    void stop_editing();
    void handle_mouse(const Interaction& it);
    void handle_keys();
    void reveal_caret();
    void run_scrollbar();
    void draw(const Interaction& it);

    Context& _ctx;
    TextEdit& _w;
    UiTextEditState& _s;
    u64 _frame = 0;
    UiPadding _padding{};
    EditView _view{};
    bool _reveal = false;
};

std::string_view view_of(const std::string& text, TextRange range) {
    return std::string_view{text}.substr(range.begin, range.end - range.begin);
}

void Editor::ensure_layout() {
    UiTextEditState::Layout& layout = _s.layout;
    const f32 width = _w.wrap ? std::max(1.0f, _view.viewport.w) : 0.0f;
    const std::size_t hash = std::hash<std::string_view>{}(_s.text);
    const void* font = _w.text_style.font.identity();
    if (!layout.lines.empty() && layout.text_hash == hash && layout.text_size == _s.text.size() &&
        layout.width == width && layout.font == font && layout.scale == _w.text_style.scale && layout.wrap == _w.wrap) {
        return;
    }
    layout.lines = wrap_text_ranges(_w.text_style.font, _s.text, {.max_width = width, .scale = _w.text_style.scale});
    if (layout.lines.empty()) {
        layout.lines.push_back({});
    }
    layout.widest = 0.0f;
    if (!_w.wrap) {
        for (const TextRange line : layout.lines) {
            layout.widest = std::max(layout.widest, measure_text(_w.text_style.font, view_of(_s.text, line), _w.text_style.scale).x);
        }
    }
    layout.text_hash = hash;
    layout.text_size = _s.text.size();
    layout.width = width;
    layout.font = font;
    layout.scale = _w.text_style.scale;
    layout.wrap = _w.wrap;
}

// Lays out the text in the current bounds: sizes the widget when it grows with
// its text, and shows the scrollbar when the text overflows.
void Editor::update_view() {
    _padding = intrinsic_padding(_w.style);
    _view.line_height = measure_text(_w.text_style.font, "Mg", _w.text_style.scale).y;
    _view.pitch = _view.line_height + _w.line_spacing;
    const auto layout_in_bounds = [&] {
        _view.inner = {_w.bounds.x + _padding.left,
                       _w.bounds.y + _padding.top,
                       std::max(1.0f, _w.bounds.w - _padding.left - _padding.right),
                       std::max(1.0f, _w.bounds.h - _padding.top - _padding.bottom)};
        const f32 bar = _s.scrollbar ? _w.scrollbar_thickness : 0.0f;
        _view.viewport = {_view.inner.x, _view.inner.y, std::max(1.0f, _view.inner.w - bar), _view.inner.h};
        const std::size_t count = lines().size();
        _view.content_height = static_cast<f32>(count) * _view.pitch - _w.line_spacing;
        _w.result.wanted_height = _view.content_height + _padding.top + _padding.bottom;
        if (_w.auto_grow) {
            const f32 one_line = _view.line_height + _padding.top + _padding.bottom;
            f32 height = std::max(one_line, _w.result.wanted_height);
            if (_w.max_height > 0.0f) {
                height = std::min(height, std::max(one_line, _w.max_height));
            }
            if (height != _w.bounds.h) {
                _w.bounds.h = height;
                _view.inner.h = std::max(1.0f, _w.bounds.h - _padding.top - _padding.bottom);
                _view.viewport.h = _view.inner.h;
            }
        }
    };
    layout_in_bounds();
    const bool overflow = _view.content_height > _view.viewport.h + 0.5f;
    if (overflow != _s.scrollbar) {
        _s.scrollbar = overflow;
        layout_in_bounds();
    }
    clamp_scroll_offset();
}

void Editor::clamp_scroll_offset() {
    const f32 max_y = std::max(0.0f, _view.content_height - _view.viewport.h);
    const f32 max_x = _w.wrap ? 0.0f : std::max(0.0f, _s.layout.widest + 1.0f - _view.viewport.w);
    _s.scroll.y = std::clamp(_s.scroll.y, 0.0f, max_y);
    _s.scroll.x = std::clamp(_s.scroll.x, 0.0f, max_x);
}

// The displayed line holding `offset`. An offset where a soft-wrapped line ends
// is also where the next begins; it belongs to the next.
std::size_t Editor::line_of(std::size_t offset) {
    const std::vector<TextRange>& all = lines();
    const auto it = std::upper_bound(all.begin(), all.end(), offset, [](std::size_t value, TextRange range) {
        return value < range.begin;
    });
    return it == all.begin() ? 0 : static_cast<std::size_t>(it - all.begin() - 1);
}

// The last caret offset on a displayed line: its end, or just before it when the
// line is soft-wrapped (its end shows at the start of the next line).
std::size_t Editor::line_last_offset(std::size_t line) {
    const std::vector<TextRange>& all = lines();
    const TextRange range = all[line];
    if (line + 1 < all.size() && all[line + 1].begin == range.end && range.end > range.begin) {
        return utf8_prev(_s.text, range.end);
    }
    return range.end;
}

f32 Editor::line_start(std::size_t line) {
    if (!right_to_left()) {
        return 0.0f;
    }
    const f32 width = measure_text(_w.text_style.font, view_of(_s.text, lines()[line]), _w.text_style.scale).x;
    const f32 room = _w.wrap ? _view.viewport.w : std::max(_view.viewport.w, _s.layout.widest);
    return std::max(0.0f, room - width);
}

f32 Editor::x_in_line(std::size_t line, std::size_t offset) {
    const TextRange range = lines()[line];
    offset = std::clamp(offset, range.begin, range.end);
    return line_start(line) +
           caret_x(_w.text_style.font, view_of(_s.text, range), offset - range.begin, _w.text_style.scale);
}

// The character boundary on `line` nearest to x (relative to the line's start).
std::size_t Editor::offset_at_x(std::size_t line, f32 x) {
    const TextRange range = lines()[line];
    const std::size_t last = line_last_offset(line);
    const std::size_t at =
        range.begin + caret_at(_w.text_style.font, view_of(_s.text, range), x - line_start(line), _w.text_style.scale);
    return std::min(at, last);
}

std::size_t Editor::offset_at(Vec2f pointer) {
    const f32 local_y = pointer.y - _view.viewport.y + _s.scroll.y;
    const auto count = static_cast<i64>(lines().size());
    const i64 line = std::clamp(static_cast<i64>(std::floor(local_y / _view.pitch)), i64{0}, count - 1);
    return offset_at_x(static_cast<std::size_t>(line), pointer.x - _view.viewport.x + _s.scroll.x);
}

void Editor::move_caret(std::size_t offset, bool select, bool keep_column) {
    offset = utf8_floor(_s.text, offset);
    if (select) {
        if (!_s.has_selection()) {
            _s.selection_anchor = _s.caret;
        }
    }
    _s.caret = offset;
    if (!select) {
        _s.clear_selection();
    }
    if (!keep_column) {
        _s.preferred_x = -1.0f;
    }
    _s.last_edit = static_cast<u8>(EditKind::None);
    _s.caret_frame = _frame;
    _reveal = true;
}

void Editor::move_vertical(i32 count, bool select) {
    const std::size_t line = line_of(_s.caret);
    if (_s.preferred_x < 0.0f) {
        _s.preferred_x = x_in_line(line, _s.caret);
    }
    const i64 target = static_cast<i64>(line) + count;
    std::size_t offset = 0;
    if (target < 0) {
        offset = 0;
    } else if (target >= static_cast<i64>(lines().size())) {
        offset = _s.text.size();
    } else {
        offset = offset_at_x(static_cast<std::size_t>(target), _s.preferred_x);
    }
    move_caret(offset, select, true);
}

void Editor::push_undo(std::vector<UiTextEditState::Snapshot>& steps) {
    steps.push_back({.text = _s.text, .caret = _s.caret, .selection_anchor = _s.selection_anchor});
    if (steps.size() > TextEdit::undo_limit) {
        steps.erase(steps.begin());
    }
}

// Replaces [from, to) with `insert`, as much of it as max_bytes allows.
bool Editor::edit(std::size_t from, std::size_t to, std::string_view insert, EditKind kind) {
    if (_w.max_bytes > 0) {
        const std::size_t kept = _s.text.size() - (to - from);
        const std::size_t room = _w.max_bytes > kept ? _w.max_bytes - kept : 0;
        if (insert.size() > room) {
            insert = insert.substr(0, utf8_floor(insert, room));
        }
    }
    if (from == to && insert.empty()) {
        return false;
    }
    const bool grouped = kind != EditKind::Other && static_cast<u8>(kind) == _s.last_edit &&
                         !_s.has_selection() && _s.caret == _s.last_edit_caret;
    if (!grouped) {
        push_undo(_s.undo_steps);
    }
    _s.redo_steps.clear();
    _s.text.replace(from, to - from, insert);
    _s.caret = from + insert.size();
    _s.clear_selection();
    _s.preferred_x = -1.0f;
    _s.last_edit = static_cast<u8>(kind);
    _s.last_edit_caret = _s.caret;
    _s.caret_frame = _frame;
    _reveal = true;
    _w.result.changed = true;
    return true;
}

bool Editor::replace_selection(std::string_view insert, EditKind kind) {
    return edit(_s.selection_start(), _s.selection_end(), insert, kind);
}

bool Editor::undo() {
    if (_s.undo_steps.empty()) {
        return false;
    }
    push_undo(_s.redo_steps);
    UiTextEditState::Snapshot step = std::move(_s.undo_steps.back());
    _s.undo_steps.pop_back();
    _s.text = std::move(step.text);
    _s.caret = step.caret;
    _s.selection_anchor = step.selection_anchor;
    _s.last_edit = static_cast<u8>(EditKind::None);
    _s.caret_frame = _frame;
    _reveal = true;
    _w.result.changed = true;
    return true;
}

bool Editor::redo() {
    if (_s.redo_steps.empty()) {
        return false;
    }
    push_undo(_s.undo_steps);
    UiTextEditState::Snapshot step = std::move(_s.redo_steps.back());
    _s.redo_steps.pop_back();
    _s.text = std::move(step.text);
    _s.caret = step.caret;
    _s.selection_anchor = step.selection_anchor;
    _s.last_edit = static_cast<u8>(EditKind::None);
    _s.caret_frame = _frame;
    _reveal = true;
    _w.result.changed = true;
    return true;
}

void Editor::handle_mouse(const Interaction& it) {
    const Vec2f pointer = _ctx.pointer();
    if (it.pressed && _w.enabled) {
        _s.active = true;
        const bool repeat = _frame - _s.last_click_frame <= double_click_frames &&
                            std::abs(pointer.x - _s.last_click_position.x) <= double_click_distance &&
                            std::abs(pointer.y - _s.last_click_position.y) <= double_click_distance;
        _s.click_count = repeat ? std::min(_s.click_count + 1, 3) : 1;
        _s.last_click_frame = _frame;
        _s.last_click_position = pointer;
        const std::size_t at = offset_at(pointer);
        if (_s.click_count == 2) {
            const auto [begin, end] = utf8_word_at(_s.text, at);
            move_caret(begin, false);
            move_caret(end, true);
        } else if (_s.click_count == 3) {
            const std::size_t newline_before = at == 0 ? std::string::npos : _s.text.rfind('\n', at - 1);
            const std::size_t begin = newline_before == std::string::npos ? 0 : newline_before + 1;
            const std::size_t end = std::min(_s.text.find('\n', at), _s.text.size());
            move_caret(begin, false);
            move_caret(end, true);
        } else {
            move_caret(at, _ctx.modifier_held(KeyModifiers::Shift));
        }
    } else if (it.active && _ctx.pointer_held() && _s.active && _s.click_count == 1) {
        // Dragging selects; past an edge the text scrolls toward the pointer.
        const Rectf vp = _view.viewport;
        const f32 step = _view.pitch * 0.5f;
        if (pointer.y < vp.y) {
            _s.scroll.y -= step;
        } else if (pointer.y > vp.y + vp.h) {
            _s.scroll.y += step;
        }
        if (!_w.wrap) {
            if (pointer.x < vp.x) {
                _s.scroll.x -= step;
            } else if (pointer.x > vp.x + vp.w) {
                _s.scroll.x += step;
            }
        }
        clamp_scroll_offset();
        const Vec2f inside{std::clamp(pointer.x, vp.x, vp.x + vp.w), std::clamp(pointer.y, vp.y, vp.y + vp.h - 1.0f)};
        const std::size_t at = offset_at(inside);
        if (at != _s.caret) {
            move_caret(at, true);
        }
    }

    if (_s.active && _ctx.pointer_pressed(MouseButton::Left) && !contains(_w.bounds, pointer)) {
        _s.active = false;
        _s.clear_selection();
    }
    if (!_w.enabled) {
        _s.active = false;
    }

    if (contains(_w.bounds, pointer) && _ctx.mouse_wheel_y() != 0.0f) {
        _s.scroll.y -= _ctx.mouse_wheel_y() * _w.wheel_step;
        clamp_scroll_offset();
    }
}

// Leaves the editor, dropping keyboard focus too so it does not reactivate.
void Editor::stop_editing() {
    _s.active = false;
    _s.clear_selection();
    if (_ctx.state().is_focused(_w.id)) {
        _ctx.state().clear_focused();
    }
}

void Editor::handle_keys() {
    const bool shift = _ctx.modifier_held(KeyModifiers::Shift);
    const bool ctrl = _ctx.modifier_held(KeyModifiers::Ctrl);
    const bool editable = !_w.read_only;
    // While an input method composes, the keys are its own; the arrows move
    // the way they point in right-to-left text.
    const bool composing = !_ctx.text_composition().empty();
    const bool rtl = right_to_left();
    const auto typed = [&](Key key) {
        if (composing) {
            return false;
        }
        if (rtl && (key == Key::Left || key == Key::Right)) {
            key = key == Key::Left ? Key::Right : Key::Left;
        }
        return _ctx.key_typed(key);
    };

    if (ctrl && typed(Key::A)) {
        move_caret(0, false);
        move_caret(_s.text.size(), true);
    }
    if (ctrl && (typed(Key::C) || typed(Key::X)) && _s.has_selection()) {
        _ctx.set_clipboard_text(std::string_view{_s.text}.substr(_s.selection_start(), _s.selection_end() - _s.selection_start()));
        if (editable && typed(Key::X)) {
            replace_selection({}, EditKind::Other);
        }
    }
    if (editable && ctrl && typed(Key::V)) {
        std::string paste = normalize_newlines(_ctx.clipboard_text());
        std::erase_if(paste, [](char c) { return static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t'; });
        replace_selection(paste, EditKind::Other);
    }
    if (editable && ctrl && typed(Key::Z)) {
        shift ? redo() : undo();
    }
    if (editable && ctrl && typed(Key::Y)) {
        redo();
    }

    if (typed(Key::Left)) {
        if (_s.has_selection() && !shift) {
            move_caret(_s.selection_start(), false);
        } else {
            move_caret(ctrl ? utf8_word_left(_s.text, _s.caret) : utf8_prev(_s.text, _s.caret), shift);
        }
    }
    if (typed(Key::Right)) {
        if (_s.has_selection() && !shift) {
            move_caret(_s.selection_end(), false);
        } else {
            move_caret(ctrl ? utf8_word_right(_s.text, _s.caret) : utf8_next(_s.text, _s.caret), shift);
        }
    }
    if (typed(Key::Up)) {
        move_vertical(-1, shift);
    }
    if (typed(Key::Down)) {
        move_vertical(1, shift);
    }
    const i32 page = std::max(1, static_cast<i32>(_view.viewport.h / _view.pitch));
    if (typed(Key::PageUp)) {
        move_vertical(-page, shift);
    }
    if (typed(Key::PageDown)) {
        move_vertical(page, shift);
    }
    if (typed(Key::Home)) {
        move_caret(ctrl ? 0 : lines()[line_of(_s.caret)].begin, shift);
    }
    if (typed(Key::End)) {
        move_caret(ctrl ? _s.text.size() : line_last_offset(line_of(_s.caret)), shift);
    }

    if (editable && typed(Key::Backspace)) {
        if (_s.has_selection()) {
            replace_selection({}, EditKind::Other);
        } else if (_s.caret > 0) {
            const std::size_t from = ctrl ? utf8_word_left(_s.text, _s.caret) : utf8_prev(_s.text, _s.caret);
            edit(from, _s.caret, {}, ctrl ? EditKind::Other : EditKind::Deleting);
        }
    }
    if (editable && typed(Key::Delete)) {
        if (_s.has_selection()) {
            replace_selection({}, EditKind::Other);
        } else if (_s.caret < _s.text.size()) {
            const std::size_t to = ctrl ? utf8_word_right(_s.text, _s.caret) : utf8_next(_s.text, _s.caret);
            edit(_s.caret, to, {}, ctrl ? EditKind::Other : EditKind::Deleting);
        }
    }

    if (typed(Key::Enter) || typed(Key::KeypadEnter)) {
        const bool submit = (_w.submit == UiSubmitKey::Enter && !shift && !ctrl) ||
                            (_w.submit == UiSubmitKey::CtrlEnter && ctrl);
        if (submit) {
            _w.result.submitted = true;
        } else if (editable) {
            replace_selection("\n", EditKind::Other);
        }
    }
    if (typed(Key::Tab)) {
        if (_w.tab == UiTabKey::InsertSpaces) {
            if (editable) {
                replace_selection(std::string(static_cast<std::size_t>(std::max(0, _w.tab_spaces)), ' '), EditKind::Typing);
            }
        } else {
            stop_editing();
            _w.result.tab_direction = shift ? -1 : 1;
        }
    }
    if (_ctx.key_pressed(Key::Escape)) {
        stop_editing();
        _w.result.cancelled = true;
    }

    const std::string_view input = _ctx.text_input();
    if (editable && _s.active && !input.empty() && !ctrl) {
        std::string text = normalize_newlines(input);
        std::erase_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 && c != '\n'; });
        replace_selection(text, text.find('\n') == std::string::npos ? EditKind::Typing : EditKind::Other);
    }
}

void Editor::reveal_caret() {
    const std::size_t line = line_of(_s.caret);
    const f32 y = static_cast<f32>(line) * _view.pitch;
    if (y < _s.scroll.y) {
        _s.scroll.y = y;
    } else if (y + _view.line_height > _s.scroll.y + _view.viewport.h) {
        _s.scroll.y = y + _view.line_height - _view.viewport.h;
    }
    if (!_w.wrap) {
        const f32 x = x_in_line(line, _s.caret);
        const f32 margin = std::min(_view.viewport.w * 0.25f, 24.0f);
        if (x < _s.scroll.x) {
            _s.scroll.x = std::max(0.0f, x - margin);
        } else if (x + 1.0f > _s.scroll.x + _view.viewport.w) {
            _s.scroll.x = x + 1.0f + margin - _view.viewport.w;
        }
    }
    clamp_scroll_offset();
}

void Editor::run_scrollbar() {
    if (!_s.scrollbar) {
        return;
    }
    const Rectf track{_view.inner.x + _view.inner.w - _w.scrollbar_thickness, _view.inner.y, _w.scrollbar_thickness, _view.inner.h};
    const ScrollOptions options{.scrollbar_thickness = _w.scrollbar_thickness, .enabled = _w.enabled};
    const ScrollState scroll{
        .offset = _s.scroll,
        .content_size = {_view.viewport.w, _view.content_height},
        .viewport_size = {_view.viewport.w, _view.viewport.h},
    };
    const ScrollResult result = scroll_axis_interaction(_ctx, _w.id, ScrollAxis::Vertical, _view.viewport, track, scroll, options, _w.z + 1, &_s.scrollbar_grab);
    _s.scroll.y = result.state.offset.y;
    clamp_scroll_offset();
}

void Editor::draw(const Interaction& it) {
    const SurfaceStyle surface = with_fill_border(_ctx.theme().input_surface, _w.style.track, frame_color(_w.style, it, _w.enabled));
    _ctx.surface(_w.bounds, surface);
    if (_s.active && _w.enabled) {
        _ctx.outline_rounded_rect(_w.bounds, surface.radius, widget_border(_w.style, WidgetColorState::Focused), 2.0f);
    }

    const Rectf vp = _view.viewport;
    _ctx.push_clip(vp);
    const std::vector<TextRange>& all = lines();
    const std::size_t first = static_cast<std::size_t>(std::max(0.0f, std::floor(_s.scroll.y / _view.pitch)));
    const std::size_t last = std::min(all.size(), static_cast<std::size_t>(std::ceil((_s.scroll.y + vp.h) / _view.pitch)) + 1);
    const f32 x0 = vp.x - _s.scroll.x;
    const std::size_t sel_start = _s.selection_start();
    const std::size_t sel_end = _s.selection_end();
    const Color selection = widget_border(_w.style, WidgetColorState::Focused);
    const f32 break_width = measure_text(_w.text_style.font, " ", _w.text_style.scale).x;
    const bool rtl = right_to_left();

    for (std::size_t i = first; i < last; ++i) {
        const TextRange range = all[i];
        const f32 y = vp.y + static_cast<f32>(i) * _view.pitch - _s.scroll.y;
        const f32 start = x0 + line_start(i);
        if (_s.has_selection() && sel_start <= range.end && sel_end > range.begin) {
            const std::string_view line = view_of(_s.text, range);
            const std::size_t a = std::max(sel_start, range.begin) - range.begin;
            const std::size_t b = std::min(sel_end, range.end) - range.begin;
            for (const auto& [xa, xb] : selection_spans(_w.text_style.font, line, a, b, _w.text_style.scale)) {
                _ctx.fill_rect({start + xa, y, std::max(1.0f, xb - xa), _view.line_height}, selection);
            }
            if (sel_end > range.end) {
                // The selection runs on past this line's break, at its end.
                const f32 width = measure_text(_w.text_style.font, line, _w.text_style.scale).x;
                _ctx.fill_rect({rtl ? start - break_width : start + width, y, break_width, _view.line_height}, selection);
            }
        }
        if (range.end > range.begin) {
            _ctx.text(view_of(_s.text, range), {start, y}, _w.text_style);
        }
    }
    if (_s.text.empty() && !_w.placeholder.empty()) {
        TextStyle hint = _w.text_style;
        hint.color = alpha_scaled(hint.color, 0.45f);
        _ctx.text(_w.placeholder, {x0, vp.y - _s.scroll.y}, hint);
    }
    const bool blink_on = ((_frame - _s.caret_frame) / caret_blink_frames) % 2 == 0;
    if (_s.active && _w.enabled && !_w.read_only && blink_on) {
        const std::size_t line = line_of(_s.caret);
        const f32 y = vp.y + static_cast<f32>(line) * _view.pitch - _s.scroll.y;
        _ctx.fill_rect({x0 + x_in_line(line, _s.caret), y, std::max(1.0f, _w.text_style.scale * 0.5f), _view.line_height}, _w.text_style.color);
    }
    // What an input method is composing, over the text at the caret, underlined.
    if (_s.active && _w.enabled && !_w.read_only) {
        const std::size_t line = line_of(_s.caret);
        const f32 y = vp.y + static_cast<f32>(line) * _view.pitch - _s.scroll.y;
        const f32 caret = x0 + x_in_line(line, _s.caret);
        const std::string_view composition = _ctx.text_composition();
        if (!composition.empty()) {
            const f32 width = measure_text(_w.text_style.font, composition, _w.text_style.scale).x;
            const f32 x = rtl ? caret - width : caret;
            _ctx.fill_rect({x, y, width, _view.line_height}, _w.style.track);
            _ctx.text(composition, {x, y}, _w.text_style);
            _ctx.fill_rect({x, y + _view.line_height - 1.0f, width, 1.0f}, _w.text_style.color);
        }
        _ctx.set_text_input_area({vp.x, y, vp.w, _view.line_height}, caret);
    }
    _ctx.pop_clip();

    if (_s.scrollbar) {
        const Rectf track{_view.inner.x + _view.inner.w - _w.scrollbar_thickness, _view.inner.y, _w.scrollbar_thickness, _view.inner.h};
        const ScrollState scroll{
            .offset = _s.scroll,
            .content_size = {vp.w, _view.content_height},
            .viewport_size = {vp.w, vp.h},
        };
        const ScrollOptions options{.scrollbar_thickness = _w.scrollbar_thickness};
        _ctx.surface(track, _ctx.theme().scrollbar_track_surface);
        _ctx.surface(layout_scrollbar(track, scroll, ScrollAxis::Vertical, options).thumb, _ctx.theme().scrollbar_thumb_surface);
    }
}

void Editor::run() {
    _w.result = {};
    _s.caret = utf8_floor(_s.text, _s.caret);
    _s.selection_anchor = _s.selection_anchor == UiTextInputState::unset_selection
        ? _s.caret
        : utf8_floor(_s.text, _s.selection_anchor);

    update_view();
    const Interaction it = _ctx.region(_w.id, _w.bounds, _w.z);
    _w.interaction = it;
    run_scrollbar();

    if (it.focused && _w.enabled) {
        _s.active = true;
    }
    handle_mouse(it);
    if (_s.active && _w.enabled) {
        _ctx.request_text_input();
        handle_keys();
    }
    if (_w.result.changed || _w.auto_grow) {
        update_view();
    }
    if (_reveal) {
        reveal_caret();
    }

    _w.result.state = it.active ? UiButtonState::Pressed
        : it.hot                ? UiButtonState::Hovered
        : _s.active             ? UiButtonState::Focused
                                : UiButtonState::Normal;
    draw(it);
}

} // namespace

void run(Context& ctx, TextEdit& widget, UiTextEditState& state) {
    widget.style = themed_widget_style(widget.style, ctx.theme().input);
    widget.text_style = themed_text_style(widget.text_style, ctx.theme().body_text);
    Editor{ctx, widget, state}.run();
}

void run(Context& ctx, TextEdit& widget) {
    run(ctx, widget, widget.state);
}

} // namespace kin::ui2
