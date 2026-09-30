#include <kin/core/utf8.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/text.hpp>
#include <kin/ui2/widgets.hpp>

#include <SDL3/SDL.h>

#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kin {
// Feeds native SDL events to an Input, as App does.
struct InputFrameTestHook {
    static void event(Input& input, const SDL_Event& event) { input.process_native_event(&event); }
};
} // namespace kin

namespace {

using namespace kin;

class NullBackend final : public IRenderer2DBackend {
public:
    std::string_view name() const override { return "null"; }
    void clear(Color) override {}
    void present() override {}
    void set_logical_size(Vec2i) override {}
    void set_integer_logical_size(Vec2i) override {}
    Vec2i output_size() const override { return {640, 360}; }
    Vec2f window_to_logical(Vec2f v) const override { return v; }
    Vec2f logical_to_window(Vec2f v) const override { return v; }
    Texture create_texture_from_rgba(const u8*, Vec2i) override { return {}; }
    void draw_texture(const Texture&, Rectf) override {}
    void draw_texture(const Texture&, Rectf, Rectf) override {}
    void fill_rect(Rectf, Color) override {}
    void draw_rect(Rectf, Color) override {}
    void draw_line(Vec2f, Vec2f, Color) override {}
    void set_viewport(Rectf) override {}
    void reset_viewport() override {}
    void push_viewport(Rectf) override {}
    void pop_viewport() override {}
};

// Drives one TextEdit frame by frame. The bitmap font at scale 1 and explicit
// padding make every position below exact.
struct Harness {
    Input input;
    Renderer2D renderer{std::make_unique<NullBackend>()};
    ui2::Context ui;
    ui2::TextEdit edit;
    f32 advance = 0.0f; // one character's width
    f32 pitch = 0.0f;   // one line's height plus spacing

    explicit Harness(ui2::TextEdit widget) : edit(std::move(widget)) {
        edit.text_style.font = ui2::bitmap_font();
        edit.text_style.scale = 1.0f;
        edit.style.padding = {4.0f, 4.0f, 4.0f, 4.0f};
        if (edit.bounds.w <= 0.0f) {
            edit.bounds = {0.0f, 0.0f, 200.0f, 100.0f};
        }
        if (!edit.id) {
            edit.id = ui2::make_id("edit");
        }
        advance = ui2::measure_text(edit.text_style.font, "a", 1.0f).x;
        pitch = ui2::measure_text(edit.text_style.font, "Mg", 1.0f).y + edit.line_spacing;
    }

    void frame(const std::function<void(Input&)>& setup = {}) {
        input.begin_frame();
        if (setup) {
            setup(input);
        }
        ui.begin(input, renderer);
        ui2::run(ui, edit);
        ui.end();
    }

    // The screen point of the character boundary at `column` on displayed `line`.
    Vec2f at(i32 column, i32 line = 0) const {
        return {edit.bounds.x + 4.0f + advance * static_cast<f32>(column) + 1.0f,
                edit.bounds.y + 4.0f + pitch * static_cast<f32>(line) + 2.0f};
    }

    void press(Vec2f point) {
        frame([&](Input& in) { in.set_mouse_pos(point); });
        frame([&](Input& in) {
            in.set_mouse_pos(point);
            in.set_mouse_pressed(MouseButton::Left);
        });
    }
    void release(Vec2f point) {
        frame([&](Input& in) {
            in.set_mouse_pos(point);
            in.set_mouse_held(MouseButton::Left, false);
        });
    }
    void click(Vec2f point) {
        press(point);
        release(point);
    }
    void activate() { click(at(0)); }

    void key(Key key, KeyModifiers modifiers = KeyModifiers::None) {
        frame([&](Input& in) {
            in.set_modifier_held(modifiers, true);
            in.set_key_pressed(key);
        });
        input.set_key_released(key);
        input.set_modifier_held(modifiers, false);
    }
    void type(std::string_view text) {
        frame([&](Input& in) { in.set_text_input(text); });
    }
    const std::string& text() const { return edit.state.text; }
};

ui2::TextEdit editor(std::string text = {}) {
    ui2::TextEdit edit;
    edit.state.text = std::move(text);
    return edit;
}

void test_utf8_stepping() {
    const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z"; // a é € 😀 z
    assert(kin::utf8_next(text, 0) == 1);
    assert(kin::utf8_next(text, 1) == 3);
    assert(kin::utf8_next(text, 3) == 6);
    assert(kin::utf8_next(text, 6) == 10);
    assert(kin::utf8_next(text, 10) == 11);
    assert(kin::utf8_next(text, 11) == 11);
    assert(kin::utf8_prev(text, 11) == 10);
    assert(kin::utf8_prev(text, 10) == 6);
    assert(kin::utf8_prev(text, 6) == 3);
    assert(kin::utf8_prev(text, 3) == 1);
    assert(kin::utf8_prev(text, 1) == 0);
    assert(kin::utf8_floor(text, 2) == 1);
    assert(kin::utf8_floor(text, 8) == 6);
    assert(kin::utf8_floor(text, 6) == 6);
    assert(kin::utf8_decode(text, 1) == 0xE9u);
    assert(kin::utf8_decode(text, 3) == 0x20ACu);
    assert(kin::utf8_decode(text, 6) == 0x1F600u);

    // Malformed bytes step one at a time and never swallow a neighbour.
    const std::string broken = "a\x80\xC3z";
    assert(kin::utf8_next(broken, 1) == 2);
    assert(kin::utf8_next(broken, 2) == 3);
    assert(kin::utf8_prev(broken, 3) == 2);
    assert(kin::utf8_decode(broken, 2) == 0xFFFDu);

    const std::string words = "foo.bar  baz\xC3\xA9 x";
    assert(kin::utf8_word_right(words, 0) == 3);  // "foo" | ".bar"
    assert(kin::utf8_word_right(words, 3) == 4);  // "." | "bar"
    assert(kin::utf8_word_right(words, 4) == 9);  // "bar  " | "bazé"
    assert(kin::utf8_word_left(words, 9) == 4);
    assert(kin::utf8_word_left(words, 14) == 9);  // "bazé" is one word
    assert((kin::utf8_word_at(words, 10) == std::pair<std::size_t, std::size_t>{9, 14}));
    assert((kin::utf8_word_at(words, 7) == std::pair<std::size_t, std::size_t>{7, 9}));
    assert(kin::normalize_newlines("a\r\nb\rc\n") == "a\nb\nc\n");
}

void test_wrap_text_ranges() {
    const ui2::Font font = ui2::bitmap_font();
    const f32 w = ui2::measure_text(font, "a", 1.0f).x;
    const auto wrap = [&](std::string_view text, f32 columns, bool break_words = true) {
        return ui2::wrap_text_ranges(font, text, {.max_width = w * columns, .scale = 1.0f, .break_long_words = break_words});
    };
    using R = std::vector<ui2::TextRange>;
    // Breaks after the whitespace that fits; the space stays on its line.
    assert((wrap("hello world foo", 11) == R{{0, 12}, {12, 15}}));
    // Hard breaks, empty lines and a trailing newline.
    assert((wrap("a\n\nb", 11) == R{{0, 1}, {2, 2}, {3, 4}}));
    assert((wrap("", 11) == R{{0, 0}}));
    assert((wrap("ab\n", 11) == R{{0, 2}, {3, 3}}));
    // A word wider than a line breaks between characters, or overflows whole.
    assert((wrap("abcdefgh", 3) == R{{0, 3}, {3, 6}, {6, 8}}));
    assert((wrap("abcdefgh ij", 3, false) == R{{0, 9}, {9, 11}}));
    // Whitespace runs are kept byte for byte.
    assert((wrap("a   b", 2) == R{{0, 4}, {4, 5}}));
    // No width: '\n' only.
    assert((ui2::wrap_text_ranges(font, "a b c\nd", {.max_width = 0.0f}) == R{{0, 5}, {6, 7}}));
    // Ranges never split a character.
    const std::string accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9 \xC3\xA9\xC3\xA9";
    for (const ui2::TextRange range : wrap(accents, 3)) {
        assert(kin::utf8_floor(accents, range.begin) == range.begin);
        assert(kin::utf8_floor(accents, range.end) == range.end);
    }
}

// The OS's key repeat is its own edge: text editing reads it, menus do not.
void test_key_repeat_events() {
    Input input;
    SDL_Event down{};
    down.type = SDL_EVENT_KEY_DOWN;
    down.key.scancode = SDL_SCANCODE_BACKSPACE;
    input.begin_frame();
    InputFrameTestHook::event(input, down);
    assert(input.frame_pressed(Key::Backspace) && !input.frame_repeated(Key::Backspace));
    input.begin_frame();
    down.key.repeat = true;
    InputFrameTestHook::event(input, down);
    assert(!input.frame_pressed(Key::Backspace) && input.frame_repeated(Key::Backspace));
    input.begin_frame();
    assert(!input.frame_repeated(Key::Backspace));
    assert(kin::key_name(Key::PageDown) == std::string_view{"PageDown"});
}

void test_text_input_steps_over_multibyte_characters() {
    Input input;
    Renderer2D renderer{std::make_unique<NullBackend>()};
    ui2::Context ui;
    ui2::TextInput text{.id = ui2::make_id("text"), .bounds = {0, 0, 200, 24}};
    text.state.text = "a\xC3\xA9\xC2\xB0" "b"; // a é ° b
    text.state.caret = text.state.text.size();
    text.state.active = true;
    const auto key = [&](Key k) {
        input.begin_frame();
        input.set_key_pressed(k);
        ui.begin(input, renderer);
        ui2::run(ui, text);
        ui.end();
        input.set_key_released(k);
    };
    key(Key::Left);
    assert(text.state.caret == 5);
    key(Key::Left);
    assert(text.state.caret == 3);
    key(Key::Backspace);
    assert(text.state.text == "a\xC2\xB0" "b");
    assert(text.state.caret == 1);
    key(Key::Delete);
    assert(text.state.text == "ab");
}

void test_typing_and_deleting_multibyte_text() {
    Harness h{editor()};
    h.activate();
    assert(h.edit.state.active);
    h.type("a\xC3\xA9");
    h.type("b");
    assert(h.text() == "a\xC3\xA9" "b");
    assert(h.edit.result.changed || h.text().size() == 4);
    h.key(Key::Left);
    h.key(Key::Backspace);
    assert(h.text() == "ab");
    assert(h.edit.state.caret == 1);
    // A held key repeats.
    h.key(Key::End);
    h.frame([](Input& in) { in.set_key_pressed(Key::Backspace); });
    h.frame([](Input& in) { in.set_key_repeated(Key::Backspace); });
    h.input.set_key_released(Key::Backspace);
    assert(h.text().empty());
}

void test_up_down_keep_the_column() {
    Harness h{editor("abcdef\nab\nabcdef")};
    h.activate();
    h.key(Key::Home, KeyModifiers::Ctrl);
    for (int i = 0; i < 5; ++i) {
        h.key(Key::Right);
    }
    assert(h.edit.state.caret == 5);
    h.key(Key::Down);
    assert(h.edit.state.caret == 9); // "ab" is shorter: its end
    h.key(Key::Down);
    assert(h.edit.state.caret == 15); // back to column 5
    h.key(Key::Up, KeyModifiers::Shift);
    assert(h.edit.state.caret == 9);
    assert(h.edit.state.selection_start() == 9 && h.edit.state.selection_end() == 15);
    h.key(Key::Up);
    assert(h.edit.state.caret == 5);
    assert(!h.edit.state.has_selection());
}

void test_mouse_selection() {
    Harness h{editor("hello world\nsecond line")};
    // Drag from column 1 to column 4.
    h.press(h.at(1));
    assert(h.edit.state.caret == 1);
    h.frame([&](Input& in) {
        in.set_mouse_pos(h.at(4));
        in.set_mouse_held(MouseButton::Left, true);
    });
    h.release(h.at(4));
    assert(h.edit.state.selection_start() == 1 && h.edit.state.selection_end() == 4);

    // Shift+click extends.
    h.frame([&](Input& in) {
        in.set_modifier_held(KeyModifiers::Shift, true);
        in.set_mouse_pos(h.at(2, 1));
        in.set_mouse_pressed(MouseButton::Left);
    });
    h.input.set_modifier_held(KeyModifiers::Shift, false);
    h.release(h.at(2, 1));
    assert(h.edit.state.selection_start() == 1 && h.edit.state.selection_end() == 14);

    // Double-click a word, triple-click its line.
    for (int i = 0; i < 40; ++i) {
        h.frame(); // let the last click expire
    }
    h.click(h.at(8));
    h.click(h.at(8));
    assert(h.edit.state.selection_start() == 6 && h.edit.state.selection_end() == 11);
    h.click(h.at(8));
    assert(h.edit.state.selection_start() == 0 && h.edit.state.selection_end() == 11);
}

void test_undo_groups_typing() {
    Harness h{editor()};
    h.activate();
    h.type("a");
    h.type("b");
    h.type("c");
    h.key(Key::Left);
    h.type("X");
    assert(h.text() == "abXc");
    h.key(Key::Z, KeyModifiers::Ctrl);
    assert(h.text() == "abc");
    h.key(Key::Z, KeyModifiers::Ctrl);
    assert(h.text().empty()); // "abc" was one step
    h.key(Key::Y, KeyModifiers::Ctrl);
    assert(h.text() == "abc");
    h.key(Key::Z, KeyModifiers::Ctrl | KeyModifiers::Shift);
    assert(h.text() == "abXc");
    // Backspaces group too, and a new edit clears redo.
    h.key(Key::End);
    h.key(Key::Backspace);
    h.key(Key::Backspace);
    assert(h.text() == "ab");
    h.key(Key::Z, KeyModifiers::Ctrl);
    assert(h.text() == "abXc");
    h.type("!");
    h.key(Key::Y, KeyModifiers::Ctrl);
    assert(h.text() == "abXc!");

    // The history is bounded.
    Harness many{editor()};
    many.activate();
    for (std::size_t i = 0; i < ui2::TextEdit::undo_limit + 20; ++i) {
        many.key(Key::Enter); // each new line is its own step
    }
    assert(many.edit.state.undo_steps.size() == ui2::TextEdit::undo_limit);
}

void test_submit_keys() {
    {
        ui2::TextEdit e = editor("hi");
        e.submit = ui2::UiSubmitKey::Enter;
        Harness h{std::move(e)};
        h.activate();
        h.key(Key::End, KeyModifiers::Ctrl);
        h.key(Key::Enter);
        assert(h.edit.result.submitted);
        assert(h.text() == "hi");
        h.key(Key::Enter, KeyModifiers::Shift);
        assert(!h.edit.result.submitted);
        assert(h.text() == "hi\n");
    }
    {
        ui2::TextEdit e = editor("hi");
        e.submit = ui2::UiSubmitKey::CtrlEnter;
        Harness h{std::move(e)};
        h.activate();
        h.key(Key::End, KeyModifiers::Ctrl);
        h.key(Key::Enter);
        assert(!h.edit.result.submitted && h.text() == "hi\n");
        h.key(Key::Enter, KeyModifiers::Ctrl);
        assert(h.edit.result.submitted && h.text() == "hi\n");
    }
    {
        Harness h{editor("hi")};
        h.activate();
        h.key(Key::Enter);
        assert(!h.edit.result.submitted && h.text() == "\nhi");
        h.key(Key::Escape);
        assert(h.edit.result.cancelled && !h.edit.state.active);
    }
    {
        Harness h{editor()};
        h.activate();
        h.key(Key::Tab, KeyModifiers::Shift);
        assert(h.edit.result.tab_direction == -1 && !h.edit.state.active);
        ui2::TextEdit e = editor();
        e.tab = ui2::UiTabKey::InsertSpaces;
        e.tab_spaces = 2;
        Harness spaces{std::move(e)};
        spaces.activate();
        spaces.key(Key::Tab);
        assert(spaces.text() == "  " && spaces.edit.state.active);
    }
}

void test_read_only_selects_and_copies() {
    ui2::TextEdit e = editor("look, don't touch");
    e.read_only = true;
    Harness h{std::move(e)};
    h.activate();
    h.type("x");
    h.key(Key::Backspace);
    h.key(Key::Delete);
    h.key(Key::Enter);
    h.input.set_clipboard_text("pasted");
    h.key(Key::V, KeyModifiers::Ctrl);
    h.key(Key::X, KeyModifiers::Ctrl);
    assert(h.text() == "look, don't touch");
    h.key(Key::A, KeyModifiers::Ctrl);
    h.key(Key::C, KeyModifiers::Ctrl);
    assert(h.input.clipboard_text() == "look, don't touch");
    h.key(Key::X, KeyModifiers::Ctrl);
    assert(h.text() == "look, don't touch");
}

void test_clipboard_and_limits() {
    ui2::TextEdit e = editor();
    e.max_bytes = 8;
    Harness h{std::move(e)};
    h.activate();
    h.input.set_clipboard_text("ab\r\ncd");
    h.key(Key::V, KeyModifiers::Ctrl);
    assert(h.text() == "ab\ncd");
    // Only whole characters fit: "é" (2 bytes) twice would make 9 bytes.
    h.type("\xC3\xA9\xC3\xA9");
    assert(h.text() == "ab\ncd\xC3\xA9");
    h.key(Key::A, KeyModifiers::Ctrl);
    h.key(Key::X, KeyModifiers::Ctrl);
    assert(h.text().empty());
    assert(h.input.clipboard_text() == "ab\ncd\xC3\xA9");
}

void test_auto_grow_then_scroll() {
    ui2::TextEdit e = editor("one");
    e.bounds = {0.0f, 0.0f, 200.0f, 10.0f};
    e.auto_grow = true;
    Harness h{std::move(e)};
    const f32 line = ui2::measure_text(ui2::bitmap_font(), "Mg", 1.0f).y;
    h.edit.max_height = 8.0f + line + 2.0f * h.pitch; // three lines
    h.frame();
    assert(h.edit.bounds.h == 8.0f + line);
    h.edit.state.text = "one\ntwo";
    h.frame();
    assert(h.edit.bounds.h == 8.0f + line + h.pitch);
    assert(!h.edit.state.scrollbar);
    h.edit.state.text = "1\n2\n3\n4\n5";
    h.frame();
    assert(h.edit.bounds.h == h.edit.max_height);
    assert(h.edit.result.wanted_height > h.edit.max_height);
    assert(h.edit.state.scrollbar);
    // Moving the caret to the end scrolls it into view.
    h.activate();
    h.key(Key::End, KeyModifiers::Ctrl);
    assert(h.edit.state.scroll.y > 0.0f);
}

void test_wrapped_lines_map_the_caret() {
    ui2::TextEdit e = editor("hello world foo");
    Harness h{std::move(e)};
    // 11 columns wide: "hello world " / "foo".
    h.edit.bounds.w = 8.0f + h.advance * 11.0f;
    h.activate();
    h.key(Key::Home, KeyModifiers::Ctrl);
    h.key(Key::End);
    assert(h.edit.state.caret == 11); // before the space the line wrapped after
    h.key(Key::Down);
    assert(h.edit.state.caret == 15); // "foo" has 3 columns: its end
    h.click(h.at(1, 1));
    assert(h.edit.state.caret == 13);
}

void test_context_keeps_state() {
    Input input;
    Renderer2D renderer{std::make_unique<NullBackend>()};
    ui2::Context ui;
    const ui2::Id id = ui2::make_id("notes");
    for (int i = 0; i < 2; ++i) {
        input.begin_frame();
        ui.begin(input, renderer);
        ui2::TextEdit e{.id = id, .bounds = {0, 0, 100, 40}};
        ui2::UiTextEditState& state = ui.text_edit_state(id, "first");
        if (i == 1) {
            assert(state.text == "first!");
        }
        state.text += "!";
        ui2::run(ui, e, state);
        ui.end();
    }
    assert(ui.text_edit_state(id, "ignored").text == "first!!");
}

} // namespace

int main() {
    test_utf8_stepping();
    test_wrap_text_ranges();
    test_key_repeat_events();
    test_text_input_steps_over_multibyte_characters();
    test_typing_and_deleting_multibyte_text();
    test_up_down_keep_the_column();
    test_mouse_selection();
    test_undo_groups_typing();
    test_submit_keys();
    test_read_only_selects_and_copies();
    test_clipboard_and_limits();
    test_auto_grow_then_scroll();
    test_wrapped_lines_map_the_caret();
    test_context_keeps_state();
    return 0;
}
