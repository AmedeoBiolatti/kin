// Studio demo: an editor-style window built only from ui2's composition
// primitives (box, scroll, list, split) and labels, no prebuilt widgets.
//
//   Sections   OPEN EDITORS and KIN open and close on a click on their header;
//              each keeps that itself (Context::state), not the scene
//   Explorer   click to select, double-click (or Enter) to open a file; a folder
//              opens and closes on click (or Left/Right); Up/Down move; right-click
//              for a menu. "generated" holds 20000 files, of which only the visible
//              rows are built
//   Tabs       click to switch, x to close; Ctrl+W closes, Ctrl+Tab moves on
//   Editor     scrolls both ways (Shift+wheel sideways)
//   Splitter   drag the line between the explorer and the editor; Ctrl+B hides it
//   Tab        moves focus between the explorer and the editor
//   Esc        quit
//
// The window draws only when something changes (GameWindowInfo::wait_for_events).

#include <kin/core/json.hpp>
#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/text.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace demo {
namespace {

using namespace kin;
namespace ui = kin::ui2;

constexpr Vec2f logical_size{1280.0f, 800.0f};

GameInfo make_game_info() {
    return {
        .id = "studio_demo",
        .title = "Kin Studio Demo",
        .version = "0.1",
        .description = "An editor-style window built from ui2 boxes, scroll boxes, lists and splits.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<i32>(logical_size.x),
            .height = static_cast<i32>(logical_size.y),
            .logical_width = static_cast<i32>(logical_size.x),
            .logical_height = static_cast<i32>(logical_size.y),
            .resizable = true,
            .wait_for_events = true, // a tool: idle while nobody touches it
        },
        .tags = {"sample", "ui"},
        .fields = {},
        .input_map = {},
    };
}

namespace palette {
constexpr Color activity = Color::rgb(44, 44, 48);
constexpr Color sidebar = Color::rgb(37, 37, 40);
constexpr Color editor = Color::rgb(30, 30, 32);
constexpr Color tabs = Color::rgb(37, 37, 40);
constexpr Color hover = Color::rgb(52, 54, 60);
constexpr Color selected = Color::rgb(4, 57, 94);
constexpr Color accent = Color::rgb(0, 122, 204);
constexpr Color text = Color::rgb(204, 204, 204);
constexpr Color muted = Color::rgb(133, 133, 133);
constexpr Color modified = Color::rgb(226, 192, 141);
} // namespace palette

// A section with a header that opens and closes it, as a component: whether it
// is open is its own state (Context::state), so a caller only says what is in it.
template <class Body>
void section(ui::Context& ui, ui::Id id, std::string_view title, const ui::TextStyle& text, Body&& body) {
    struct Section {
        bool open = true;
    };
    Section& state = ui.state<Section>(id);
    ui.keep_state(id); // stays open or shut while the sidebar is hidden
    {
        auto header = ui.box(ui::make_id(id, "header"), ui::hbox().w(ui::grow()).h(22.0f).pad(8, 0).gap(6).items(ui::UiAlign::Center)
                                                            .hover_bg(palette::hover).cursor_shape(SystemCursor::Pointer));
        ui.label(state.open ? "v" : ">", text, {.width = ui::fixed(10.0f)});
        ui.label(title, text);
        if (header.clicked) {
            state.open = !state.open;
        }
    }
    if (state.open) {
        body();
    }
}

struct Entry {
    std::string name;
    i32 depth = 0;
    i32 parent = -1;
    bool folder = false;
    bool open = false;
    bool modified = false;
    i32 lines = 0;
};

// A made-up repository; folders hold the entries that follow them, one deeper.
std::vector<Entry> make_tree() {
    std::vector<Entry> tree;
    const auto folder = [&](std::string name, i32 depth, bool open) {
        tree.push_back({.name = std::move(name), .depth = depth, .folder = true, .open = open});
    };
    const auto file = [&](std::string name, i32 depth, i32 lines, bool modified = false) {
        tree.push_back({.name = std::move(name), .depth = depth, .modified = modified, .lines = lines});
    };
    folder("engine", 0, true);
    folder("include", 1, false);
    file("context.hpp", 2, 690, true);
    file("layout.hpp", 2, 83);
    file("widgets.hpp", 2, 1602);
    folder("src", 1, true);
    file("compose.cpp", 2, 420, true);
    file("context.cpp", 2, 2100, true);
    file("layout.cpp", 2, 581);
    file("widgets_core.cpp", 2, 1400);
    folder("tests", 0, false);
    file("ui2_tests.cpp", 1, 6500, true);
    file("layout_tests.cpp", 1, 300);
    folder("docs", 0, false);
    file("ui.md", 1, 272);
    file("rendering.md", 1, 640);
    folder("generated", 0, false);
    for (i32 i = 0; i < 20000; ++i) {
        file("shader_variant_" + std::to_string(i) + ".spv.h", 1, 40 + i % 300);
    }
    file("CMakeLists.txt", 0, 180);
    file("README.md", 0, 90);

    std::vector<i32> parents;
    for (i32 i = 0; i < static_cast<i32>(tree.size()); ++i) {
        parents.resize(static_cast<std::size_t>(tree[static_cast<std::size_t>(i)].depth));
        tree[static_cast<std::size_t>(i)].parent = parents.empty() ? -1 : parents.back();
        if (tree[static_cast<std::size_t>(i)].folder) {
            parents.push_back(i);
        }
    }
    return tree;
}

class StudioScene final : public Scene {
public:
    StudioScene() : _tree(make_tree()) {
        refresh_rows();
        _ui.bind({.action = "explorer.up", .key = Key::Up, .context = "explorer"});
        _ui.bind({.action = "explorer.down", .key = Key::Down, .context = "explorer"});
        _ui.bind({.action = "explorer.open", .key = Key::Enter, .context = "explorer"});
        _ui.bind({.action = "explorer.expand", .key = Key::Right, .context = "explorer"});
        _ui.bind({.action = "explorer.collapse", .key = Key::Left, .context = "explorer"});
        _ui.bind({.action = "tabs.close", .key = Key::W, .modifiers = KeyModifiers::Ctrl});
        _ui.bind({.action = "tabs.next", .key = Key::Tab, .modifiers = KeyModifiers::Ctrl});
        _ui.bind({.action = "sidebar.toggle", .key = Key::B, .modifiers = KeyModifiers::Ctrl});
        _menu.id = ui::make_id("explorer_menu_list");
        for (const auto& [id, label] : {std::pair{"open", "Open"}, std::pair{"copy", "Copy Name"}, std::pair{"close_all", "Close All Tabs"}}) {
            ui::MenuListItem item;
            item.id = id;
            item.label = label;
            _menu.items.push_back(std::move(item));
        }
    }

    void update(SceneContext& ctx) override {
        if (ctx.input.pressed(Key::Escape) && !_ui.popup_open(menu_id())) {
            ctx.app.quit();
        }
    }

    void render(SceneContext& ctx) override {
        Renderer2D& r = ctx.renderer;
        r.clear(palette::editor);
        _text = {.font = ui::system_ui_font(13), .scale = 1.0f, .color = palette::text};
        _muted = _text;
        _muted.color = palette::muted;
        _menu.text_style = _text;

        _ui.begin(ctx.input, r, ctx.dt);
        commands();
        _ui.begin_layout({0.0f, 0.0f, logical_size.x, logical_size.y});
        {
            auto window = _ui.box(ui::make_id("window"), ui::vbox().fill().passive());
            {
                auto body = _ui.box(ui::make_id("body"), ui::hbox().fill().passive());
                activity_bar();
                if (_sidebar) {
                    _ui.begin_split(ui::make_id("sidebar_split"),
                                    {.ratio = 0.24f, .min_first = 160.0f, .min_second = 300.0f, .thickness = 3.0f});
                    explorer();
                    _ui.split_next();
                    editor();
                    _ui.end_split();
                } else {
                    editor();
                }
            }
            status_bar();
        }
        _ui.end_layout();
        context_menu();
        _ui.end();
        _ui.apply(ctx.app, ctx.window); // pointer shape, redraws, text input
    }

    void write_report(JsonWriter& json) const override {
        json.field("tree_rows", static_cast<i64>(_rows.size()));
        json.field("tree_rows_built", static_cast<i64>(_rows_built));
        json.field("open_tabs", static_cast<i64>(_tabs.size()));
        json.field("active_file", _active >= 0 ? _tree[static_cast<std::size_t>(_active)].name : std::string{});
    }

private:
    static ui::Id menu_id() { return ui::make_id("explorer_menu"); }
    static ui::Id rows_id() { return ui::make_id("explorer_rows"); }

    // Key bindings fired this frame (dispatched by the Context from where focus is).
    void commands() {
        const i32 count = static_cast<i32>(_rows.size());
        const auto move_cursor = [&](i32 to) {
            _cursor = std::clamp(to, 0, std::max(0, count - 1));
            _ui.scroll_to_row(rows_id(), _cursor);
        };
        if (_ui.action_triggered("explorer.up")) {
            move_cursor(_cursor - 1);
        }
        if (_ui.action_triggered("explorer.down")) {
            move_cursor(_cursor + 1);
        }
        if (_cursor >= 0 && _cursor < count) {
            Entry& entry = _tree[static_cast<std::size_t>(_rows[static_cast<std::size_t>(_cursor)])];
            if (_ui.action_triggered("explorer.open")) {
                activate(_rows[static_cast<std::size_t>(_cursor)]);
            }
            if (entry.folder && entry.open == _ui.action_triggered("explorer.collapse") &&
                (_ui.action_triggered("explorer.collapse") || _ui.action_triggered("explorer.expand"))) {
                entry.open = !entry.open;
                refresh_rows();
            }
        }
        if (_ui.action_triggered("tabs.close") && _active >= 0) {
            close_tab(_active);
        }
        if (_ui.action_triggered("tabs.next") && !_tabs.empty()) {
            const auto at = std::ranges::find(_tabs, _active);
            _active = at == _tabs.end() || at + 1 == _tabs.end() ? _tabs.front() : *(at + 1);
        }
        if (_ui.action_triggered("sidebar.toggle")) {
            _sidebar = !_sidebar;
        }
    }

    void activity_bar() {
        auto bar = _ui.box(ui::make_id("activity"), ui::vbox().w(48.0f).h(ui::grow()).bg(palette::activity).passive());
        static constexpr const char* views[] = {"F", "S", "G", "D"};
        for (i32 i = 0; i < 4; ++i) {
            auto item = _ui.box(ui::make_id(ui::make_id("activity"), static_cast<u64>(i)),
                                ui::hbox().size(48.0f, 48.0f).center().hover_bg(palette::hover).selected_bg(palette::hover)
                                    .selected_if(i == _view).cursor_shape(SystemCursor::Pointer));
            ui::TextStyle glyph = i == _view ? _text : _muted;
            glyph.scale = 1.3f;
            _ui.label(views[i], glyph);
            if (item.clicked) {
                if (i == _view) {
                    _sidebar = !_sidebar;
                }
                _view = i;
            }
        }
    }

    void explorer() {
        auto side = _ui.box(ui::make_id("explorer"), ui::vbox().fill().bg(palette::sidebar).passive().key_scope("explorer"));
        {
            auto header = _ui.box(ui::make_id("explorer_header"), ui::hbox().w(ui::grow()).h(32.0f).pad(16, 0, 8, 0).items(ui::UiAlign::Center).passive());
            _ui.label("EXPLORER", _muted);
        }
        _rows_built = 0;
        section(_ui, ui::make_id("open_editors"), "OPEN EDITORS", _muted, [&] {
            for (const i32 index : _tabs) {
                auto item = _ui.box(ui::make_id(ui::make_id("open_editor"), static_cast<u64>(index)),
                                    ui::hbox().w(ui::grow()).h(22.0f).pad(26, 0, 12, 0).items(ui::UiAlign::Center)
                                        .hover_bg(palette::hover).selected_bg(palette::selected).selected_if(index == _active));
                _ui.label(_tree[static_cast<std::size_t>(index)].name, index == _active ? _text : _muted);
                if (item.clicked) {
                    _active = index;
                }
            }
        });
        section(_ui, ui::make_id("repository"), "KIN", _muted, [&] { tree(); });
    }

    void tree() {
        // One tab stop for the whole tree; the arrows move inside it.
        auto files = _ui.scroll(ui::make_id("explorer_files"), ui::vbox().fill().focus_border(palette::accent).tab_stop());
        bool changed = false;
        _ui.list(rows_id(), static_cast<i32>(_rows.size()), 22.0f, [&](i32 row) {
            ++_rows_built;
            const i32 index = _rows[static_cast<std::size_t>(row)];
            Entry& entry = _tree[static_cast<std::size_t>(index)];
            auto item = _ui.box(ui::make_id(ui::make_id("tree"), static_cast<u64>(index)),
                                ui::hbox().fill().pad(8.0f + 14.0f * static_cast<f32>(entry.depth), 0, 12, 0).gap(6).items(ui::UiAlign::Center)
                                    .hover_bg(palette::hover).selected_bg(palette::selected).selected_if(row == _cursor));
            _ui.label(entry.folder ? (entry.open ? "v" : ">") : " ", _muted, {.width = ui::fixed(10.0f)});
            ui::TextStyle name = _text;
            if (entry.modified) {
                name.color = palette::modified;
            }
            _ui.label(entry.name, name, {.width = ui::grow()});
            if (entry.modified) {
                _ui.label("M", name);
            }
            if (item.clicked) {
                _cursor = row;
                if (entry.folder) {
                    entry.open = !entry.open;
                    changed = true;
                } else if (item.double_clicked) {
                    open(index);
                }
            }
            if (item.secondary_clicked) {
                _cursor = row;
                _menu_anchor = {_ui.pointer().x, _ui.pointer().y, 0.0f, 0.0f};
                _ui.open_popup(menu_id());
            }
        });
        if (changed) {
            refresh_rows(); // takes effect next frame; this frame's rows are already built
        }
    }

    void context_menu() {
        const ui::ContextMenuResult menu = ui::context_menu(_ui, menu_id(), _menu_anchor, _menu);
        if (menu.activated < 0 || _cursor < 0 || _cursor >= static_cast<i32>(_rows.size())) {
            return;
        }
        const i32 index = _rows[static_cast<std::size_t>(_cursor)];
        if (menu.activated_id == "open") {
            activate(index);
        } else if (menu.activated_id == "copy") {
            _ui.set_clipboard_text(_tree[static_cast<std::size_t>(index)].name);
        } else if (menu.activated_id == "close_all") {
            _tabs.clear();
            _active = -1;
        }
    }

    void editor() {
        auto pane = _ui.box(ui::make_id("editor"), ui::vbox().fill().passive().key_scope("editor"));
        {
            auto strip = _ui.box(ui::make_id("tabs"), ui::hbox().w(ui::grow()).h(36.0f).bg(palette::tabs).passive());
            i32 close = -1;
            for (const i32 index : _tabs) {
                const ui::Id id = ui::make_id(ui::make_id("tab"), static_cast<u64>(index));
                auto tab = _ui.box(id, ui::hbox().h(ui::grow()).pad(12, 0, 8, 0).gap(8).items(ui::UiAlign::Center)
                                           .bg(palette::tabs).hover_bg(palette::hover).selected_bg(palette::editor).selected_if(index == _active));
                _ui.label(_tree[static_cast<std::size_t>(index)].name, index == _active ? _text : _muted);
                {
                    // The close button shows on the active tab and the one under the pointer.
                    const bool shown = index == _active || tab.hovered;
                    auto x = _ui.box(ui::make_id(id, "close"), ui::hbox().size(18.0f, 18.0f).center().rounded(3.0f).hover_bg(palette::hover));
                    _ui.label(shown ? "x" : " ", _muted);
                    if (x.clicked) {
                        close = index;
                    }
                }
                if (tab.clicked) {
                    _active = index;
                }
            }
            if (close >= 0) {
                close_tab(close);
            }
        }

        auto code = _ui.scroll(ui::make_id(ui::make_id("code"), static_cast<u64>(_active)),
                               ui::vbox().fill().pad(0, 6, 0, 0).bg(palette::editor).tab_stop().cursor_shape(SystemCursor::Text),
                               {.horizontal = true});
        if (_active < 0) {
            _ui.label("Double-click a file in the explorer.", _muted, {.margin = {24, 24, 0, 0}});
            return;
        }
        const Entry& file = _tree[static_cast<std::size_t>(_active)];
        _ui.list(ui::make_id("code_lines"), file.lines, 19.0f, [&](i32 line) {
            _ui.label(std::to_string(line + 1), _muted, {.width = ui::fixed(56.0f)});
            std::string text;
            if (line % 7 != 0) {
                text = "    // " + file.name + ", line " + std::to_string(line + 1);
            }
            if (line % 13 == 5) {
                text += "  -- a long line, so the editor scrolls sideways: the quick brown fox jumps over the lazy dog, twice over";
            }
            _ui.label(text, _text);
        });
    }

    void status_bar() {
        auto bar = _ui.box(ui::make_id("status"), ui::hbox().w(ui::grow()).h(24.0f).pad(10, 0).gap(18).items(ui::UiAlign::Center)
                                                      .bg(palette::accent).passive());
        ui::TextStyle white = _text;
        white.color = colors::white;
        _ui.label("exp/ui2-compose", white);
        _ui.spacer({.width = ui::grow()});
        _ui.label("Ctrl+B sidebar   Ctrl+W close   Ctrl+Tab next tab", white);
        _ui.label(std::to_string(_rows_built) + " of " + std::to_string(_rows.size()) + " rows built", white);
    }

    // Enter or the menu: a folder opens or closes, a file opens in a tab.
    void activate(i32 index) {
        Entry& entry = _tree[static_cast<std::size_t>(index)];
        if (entry.folder) {
            entry.open = !entry.open;
            refresh_rows();
        } else {
            open(index);
        }
    }

    void open(i32 index) {
        if (std::ranges::find(_tabs, index) == _tabs.end()) {
            _tabs.push_back(index);
        }
        _active = index;
    }

    void close_tab(i32 index) {
        std::erase(_tabs, index);
        if (_active == index) {
            _active = _tabs.empty() ? -1 : _tabs.back();
        }
    }

    // The entries whose folders are all open, in order.
    void refresh_rows() {
        _rows.clear();
        for (i32 i = 0; i < static_cast<i32>(_tree.size()); ++i) {
            bool visible = true;
            for (i32 p = _tree[static_cast<std::size_t>(i)].parent; p >= 0; p = _tree[static_cast<std::size_t>(p)].parent) {
                if (!_tree[static_cast<std::size_t>(p)].open) {
                    visible = false;
                    break;
                }
            }
            if (visible) {
                _rows.push_back(i);
            }
        }
        _cursor = std::min(_cursor, static_cast<i32>(_rows.size()) - 1);
    }

    ui::Context _ui;
    ui::TextStyle _text{};
    ui::TextStyle _muted{};
    ui::MenuList _menu;
    Rectf _menu_anchor{};
    std::vector<Entry> _tree;
    std::vector<i32> _rows; // visible tree entries
    std::vector<i32> _tabs;
    i32 _cursor = 0;  // the selected row
    i32 _active = -1; // the file shown
    i32 _view = 0;
    i32 _rows_built = 0;
    bool _sidebar = true;
};

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    kin::SceneManager scenes;

    const auto build_scenes = [](kin::SceneManager& target) {
        target.push(std::make_unique<demo::StudioScene>());
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .render_headless = true, // headless runs lay the window out, so the report shows it
        .reset_scenes = build_scenes,
    }, scenes);
}
