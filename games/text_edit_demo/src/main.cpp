// TextEdit demo: a chat box, its history and a notes pane, all ui2::TextEdit.
//
//   Chat box   Enter sends, Shift+Enter starts a new line; it grows up to four
//              lines, then scrolls
//   History    read-only: select with the mouse, copy with Ctrl+C
//   Notes      Tab indents; Ctrl+Z / Ctrl+Y undo and redo
//   Esc        leave the focused editor (quits when none is)

#include <kin/core/json.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/text.hpp>
#include <kin/ui2/widgets.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace demo {
namespace {

constexpr kin::Vec2f logical_size{960.0f, 600.0f};

kin::GameInfo make_game_info() {
    return {
        .id = "text_edit_demo",
        .title = "Kin TextEdit Demo",
        .version = "0.1",
        .description = "A chat box, a read-only history and a notes pane built with kin::ui2::TextEdit.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<kin::i32>(logical_size.x),
            .height = static_cast<kin::i32>(logical_size.y),
            .logical_width = static_cast<kin::i32>(logical_size.x),
            .logical_height = static_cast<kin::i32>(logical_size.y),
            .resizable = true,
        },
        .tags = {"sample", "ui"},
        .fields = {},
        .input_map = {},
    };
}

constexpr std::string_view history_intro =
    "ada: Morning! The lighting pass is in, screenshots attached.\n"
    "ben: Looks great. The campfire flicker reads well even at night.\n"
    "ada: I left notes on the crystal: it could use a slower pulse.\n"
    "ben: Agreed. Also the flashlight cone clips the wall at the top right; "
    "probably the shape texture, not the light itself.\n"
    "ada: I'll check the texture's alpha. Café in ten minutes? \xE2\x98\x95\n"
    "ben: Sure. Bring the review list.\n"
    "ada: Review list: crystal pulse, flashlight edge, night ambient 10% brighter, "
    "lamp radius on the south side.\n"
    "ben: Added two: the path dots overlap the lamps, and the HUD font looks soft at 125% scaling.\n"
    "ada: Noted. Select any of this with the mouse and Ctrl+C copies it.\n";

class TextEditDemoScene final : public kin::Scene {
public:
    TextEditDemoScene() {
        // An earlier backlog, so the history starts out scrolling.
        for (int build = 1; build <= 24; ++build) {
            _history.state.text += "ci: build " + std::to_string(4800 + build) + " passed on all platforms\n";
        }
        _history.state.text += history_intro;
        _history.state.scroll.y = 1.0e9f; // open on the newest message
        _chat.state.text = "Two lines here:\nthe box grows to fit them.";
        _chat.state.caret = _chat.state.text.size();
        _notes.state.text = "Review notes\n\n- crystal: slower pulse (2s?)\n- flashlight: check the cone texture\n";
    }

    void update(kin::SceneContext& ctx) override {
        if (!_editing && ctx.input.pressed(kin::Key::Escape)) {
            ctx.app.quit();
        }
    }

    void render(kin::SceneContext& ctx) override {
        kin::Renderer2D& r = ctx.renderer;
        r.clear(kin::Color::rgb(24, 27, 33));
        _ui.begin(ctx.input, r, ctx.dt);

        const kin::ui2::TextStyle label{.font = kin::ui2::system_ui_font(14), .scale = 1.0f,
                                        .color = kin::Color::rgb(150, 160, 175)};
        const kin::ui2::TextStyle body{.font = kin::ui2::system_ui_font(15), .scale = 1.0f};
        _history.text_style = _chat.text_style = _notes.text_style = body;

        // Chat: the box sits at the bottom and grows upward; the history fills the rest.
        const float left = 24.0f;
        const float width = 560.0f;
        const float bottom = logical_size.y - 24.0f;
        _ui.text("Chat", {left, 18.0f}, label);
        _chat.bounds = {left, bottom - _chat.bounds.h, width, _chat.bounds.h};
        kin::ui2::run(_ui, _chat);
        if (_chat.result.submitted && !_chat.state.text.empty()) {
            _history.state.text += "you: " + _chat.state.text + "\n";
            _chat.state.text.clear();
            _chat.state.caret = 0;
            _history.state.scroll.y = 1.0e9f; // show the newest message
        }
        _history.bounds = {left, 42.0f, width, _chat.bounds.y - 12.0f - 42.0f};
        kin::ui2::run(_ui, _history);

        _ui.text("Notes", {612.0f, 18.0f}, label);
        _notes.bounds = {612.0f, 42.0f, logical_size.x - 612.0f - 24.0f, bottom - 42.0f};
        kin::ui2::run(_ui, _notes);

        _editing = _chat.state.active || _notes.state.active || _history.state.active;
        _ui.end();
    }

    void write_report(kin::JsonWriter& json) const override {
        json.field("history_bytes", static_cast<kin::i64>(_history.state.text.size()));
        json.field("history_scrollbar", _history.state.scrollbar);
        json.field("chat_lines", static_cast<kin::i64>(_chat.state.layout.lines.size()));
        json.field("chat_height", static_cast<double>(_chat.bounds.h));
        json.field("notes_bytes", static_cast<kin::i64>(_notes.state.text.size()));
    }

private:
    kin::ui2::Context _ui;
    kin::ui2::TextEdit _history{
        .id = kin::ui2::make_id("history"),
        .read_only = true,
    };
    kin::ui2::TextEdit _chat{
        .id = kin::ui2::make_id("chat"),
        .bounds = {0.0f, 0.0f, 0.0f, 32.0f},
        .placeholder = "Message: Enter sends, Shift+Enter starts a new line",
        .max_bytes = 2000,
        .submit = kin::ui2::UiSubmitKey::Enter,
        .auto_grow = true,
        .max_height = 104.0f,
    };
    kin::ui2::TextEdit _notes{
        .id = kin::ui2::make_id("notes"),
        .placeholder = "Notes",
        .tab = kin::ui2::UiTabKey::InsertSpaces,
        .tab_spaces = 2,
    };
    bool _editing = false;
};

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [](kin::SceneManager& target) {
        target.push(std::make_unique<demo::TextEditDemoScene>());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .render_headless = true, // headless runs lay the editors out, so the report shows it
        .reset_scenes = build_scenes,
    }, scenes);
}
