// next_code look: the window of next_code (a Rust IDE on gpui) rebuilt from ui2
// boxes and labels, to see how close composition gets to it. Colours, sizes and
// menus are next_code's (its Dark theme, shell.rs, docks.rs, panes.rs); what
// the controls do is mostly left out.
//
//   Title bar  the window's own (it has no system one): drag it to move the window,
//              double-click to maximize; minimize, maximize and close at the right
//   Menu bar   click a title to open its menu, point at another title to switch,
//              Esc or a click outside closes it; submenus open on hover
//   Files      click a folder to open or close it, a file to select it
//   Tabs       click to switch
//
// --menu=<title> opens that menu at start, --submenu=<label> a submenu in it (for
// screenshots).

#include <kin/platform/input.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scene/scene.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/text.hpp>

#include <nextcode_icons.hpp> // next_code's SVG icons (see CMakeLists.txt)

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace demo {
namespace {

using namespace kin;
namespace ui = kin::ui2;
using ui::UiAlign;
using ui::UiAnchor;

constexpr Vec2f logical_size{1440.0f, 900.0f};

GameInfo make_game_info() {
    return {
        .id = "nextcode_look",
        .title = "nextcode - next_code",
        .version = "0.1",
        .description = "next_code's window rebuilt from ui2 boxes and labels.",
        .author = "Kin contributors",
        .window = {
            .width = static_cast<i32>(logical_size.x),
            .height = static_cast<i32>(logical_size.y),
            .logical_width = static_cast<i32>(logical_size.x),
            .logical_height = static_cast<i32>(logical_size.y),
            .resizable = true,
            .borderless = true, // next_code draws its own title bar (on Windows)
            .wait_for_events = true,
        },
        .tags = {"sample", "ui"},
        .fields = {},
        .input_map = {},
    };
}

constexpr Color hex(u32 rgb, u8 alpha = 255) {
    return Color::rgba(static_cast<u8>(rgb >> 16), static_cast<u8>(rgb >> 8), static_cast<u8>(rgb), alpha);
}

// next_code's built-in Dark theme (crates/ui/themes/dark.json).
namespace theme {
constexpr Color background = hex(0x1e1f22);
constexpr Color panel = hex(0x2b2d30);
constexpr Color border = hex(0x393b40);
constexpr Color text = hex(0xbcbec4);
constexpr Color muted = hex(0x6f737a);
constexpr Color accent = hex(0x3574f0);
constexpr Color accent_text = hex(0x6ca2ff);
constexpr Color on_accent = hex(0xffffff);
constexpr Color error = hex(0xf75464);
constexpr Color active_row = hex(0x2e436e);
constexpr Color hover_row = hex(0x35373b);
constexpr Color cursor = hex(0xced0d6);
constexpr Color current_line = hex(0x26282e);
constexpr Color indent_guide = hex(0x34363b);
constexpr Color git_added = hex(0x73bd79);
constexpr Color git_modified = hex(0x70aeff);
constexpr Color run_marker = hex(0x5fb865);
constexpr Color scrollbar_thumb = hex(0xbcbec4, 0x26);
constexpr Color scrollbar_thumb_hover = hex(0xbcbec4, 0x4d);
} // namespace theme

namespace syntax {
constexpr Color attribute = hex(0xb3ae60);
constexpr Color boolean = hex(0x2aacb8);
constexpr Color comment = hex(0x7a7e85);
constexpr Color doc = hex(0x5f826b);
constexpr Color constant = hex(0xc77dbb);
constexpr Color function = hex(0x56a8f5);
constexpr Color keyword = hex(0xcf8e6d);
constexpr Color number = hex(0x2aacb8);
constexpr Color property = hex(0xc77dbb);
constexpr Color string = hex(0x6aab73);
constexpr Color type = hex(0x6fafbd);
} // namespace syntax

// Sizes from shell.rs, docks.rs, panes.rs and file_tree.rs.
constexpr f32 menu_bar_height = 36.0f;
constexpr f32 menu_width = 280.0f;
constexpr f32 project_tabs_height = 30.0f;
constexpr f32 dock_header_height = 30.0f;
constexpr f32 strip_size = 26.0f;
constexpr f32 handle_size = 5.0f;
constexpr f32 tab_bar_height = 34.0f;
constexpr f32 tree_row_height = 24.0f;
constexpr f32 line_height = 20.0f;
constexpr f32 minimap_width = 126.0f;

// --- menus (menus.rs, labels from commands.rs) ---------------------------------------

struct MenuItem {
    std::string label; // empty: a separator
    std::string keys;
    bool enabled = true;
    std::vector<MenuItem> items; // a submenu
};

struct Menu {
    std::string title;
    std::vector<MenuItem> items;
};

MenuItem item(std::string label, std::string keys = {}) {
    return {.label = std::move(label), .keys = std::move(keys), .enabled = true, .items = {}};
}

MenuItem off(std::string label, std::string keys = {}) {
    return {.label = std::move(label), .keys = std::move(keys), .enabled = false, .items = {}};
}

MenuItem sub(std::string label, std::vector<MenuItem> items) {
    return {.label = std::move(label), .keys = {}, .enabled = true, .items = std::move(items)};
}

MenuItem sep() {
    return {};
}

std::vector<Menu> make_menus() {
    return {
        {"File",
         {item("New File…"), sep(), item("Open Project…", "Ctrl+O"), item("Open Project in New Window…"),
          item("Open Recent…", "Ctrl+R"),
          sub("Recent Projects", {item("nextcode  —  /home/bamedeo/Downloads/nextcode"),
                                  item("kin  —  /mnt/windows/Users/amedeo/Desktop/gamedev/kin"),
                                  item("ui2-compose  —  /mnt/windows/Users/amedeo/Desktop/gamedev/kin-wt/ui2-compose")}),
          sep(), item("Open Workspace from File…"), item("Add Folder to Workspace…"), item("Save Workspace As…"), sep(),
          off("Reopen in Dev Container"), sep(), item("Save", "Ctrl+S"), item("Save All"), sep(),
          item("Close Tab", "Ctrl+W"), item("Close Project"), sep(), item("Settings…", "Ctrl+,"),
          item("Open Settings (JSON)"), item("Customize Menus…"), item("Select Theme…"), item("Customize Theme…"),
          item("Manage Extensions…"), sep(), item("Exit")}},
        {"Edit",
         {item("Undo", "Ctrl+Z"), item("Redo", "Ctrl+Y"), sep(), item("Cut", "Ctrl+X"), item("Copy", "Ctrl+C"),
          item("Paste", "Ctrl+V"), item("Copy Reference", "Ctrl+Alt+Shift+C"), item("Add Note"), sep(),
          item("Find…", "Ctrl+F"), item("Replace…", "Ctrl+H"), item("Find in Files", "Ctrl+Shift+F"),
          item("Replace in Files", "Ctrl+Shift+H"), sep(), item("Toggle Line Comment", "Ctrl+/"),
          item("Toggle Block Comment", "Alt+Shift+A"), item("Indent Lines", "Ctrl+]"), item("Outdent Lines", "Ctrl+["), sep(),
          item("Show Completions", "Ctrl+Space")}},
        {"Selection",
         {item("Select All", "Ctrl+A"), sep(), item("Copy Line Up", "Alt+Shift+Up"), item("Copy Line Down", "Alt+Shift+Down"),
          item("Move Line Up", "Alt+Up"), item("Move Line Down", "Alt+Down"), item("Delete Line", "Ctrl+Shift+K"),
          item("Insert Line Above", "Ctrl+Shift+Enter"), item("Insert Line Below", "Ctrl+Enter"), sep(),
          item("Add Cursor Above", "Ctrl+Alt+Up"), item("Add Cursor Below", "Ctrl+Alt+Down"),
          item("Add Next Occurrence", "Ctrl+D"), item("Select All Occurrences", "Ctrl+Shift+L")}},
        {"View",
         {item("Command Palette…", "Ctrl+Shift+P"), sep(), item("Files Panel"), item("Search Panel"), item("Commit"),
          item("Git"), item("Python Panel"), item("Problems Panel"), item("Notes"), item("Ports"), item("Tests"),
          item("Call Hierarchy"), item("Outline"), item("Debug"), item("Debug Console"), item("Terminal Panel"),
          item("Toggle Bottom Dock"), sep(), item("Split Editor Right"), item("Split Editor Down"),
          item("Focus Next Editor Pane"), item("Move Tab to Next Pane"), item("Move Tab to Previous Pane"),
          item("Close Pane"), sep(), item("Word Wrap", "Alt+Z"), item("Inlay Hints"), item("Vim Mode"),
          sub("Folding", {item("Fold", "Ctrl+Shift+["), item("Unfold", "Ctrl+Shift+]"), item("Toggle Fold"),
                          item("Fold Recursively"), item("Unfold Recursively"), sep(), item("Fold All"), item("Unfold All")}),
          sep(), item("Save Layout as Default"), item("Reset Layout"), sep(), item("Next Project Tab"),
          item("Previous Project Tab")}},
        {"Go",
         {item("Go to File…", "Ctrl+P"), item("Go to Symbol in File…", "Ctrl+Shift+O"), item("Go to Symbol in Workspace…", "Ctrl+T"),
          sep(), item("Back", "Alt+Left"), item("Forward", "Alt+Right"), sep(), item("Go to Definition", "F12"),
          item("Go to Declaration"), item("Go to Type Definition"), item("Go to Implementation", "Ctrl+F12"),
          item("Find References", "Alt+F7"), item("Show Call Hierarchy", "Ctrl+Alt+H"), item("Go to Line…", "Ctrl+G"),
          item("Go to Matching Bracket", "Ctrl+Shift+\\"), sep(), item("Next Problem", "F8"),
          item("Previous Problem", "Shift+F8"), item("Next Change"), item("Previous Change")}},
        {"Refactor",
         {item("Rename Symbol…", "F2"), item("Quick Fix…", "Ctrl+."), sep(), item("Format Document", "Ctrl+Alt+L"),
          item("Format Selection")}},
        {"Run",
         {item("Run", "Shift+F10"), item("Debug", "Shift+F9"), item("Select Configuration…"), item("Edit Configurations…"),
          sep(), item("Run All Tests"), off("Run Failed Tests"), off("Run Tests in Current File"), off("Run Test at Cursor"),
          item("Show Tests"), sep(), off("Run Current Python File"), sep(), off("Run Cell"), off("Run All Cells"),
          off("Interrupt Kernel"), off("Restart Kernel"), sep(), off("Sync Environment (uv sync)"),
          off("Update Lock File (uv lock)"), sep(), item("Open Workflow…"), item("Open Workflow File…"),
          item("Add Workflow Folder…"), item("New Workflow…"), sep(), item("New Terminal", "Ctrl+Shift+`"),
          item("Forward a Port…")}},
        {"Debug",
         {item("Start Debugging", "F5"), item("Debug Current File"), sep(), off("Continue", "F5"), off("Pause", "F6"),
          off("Step Over", "F10"), off("Step Into", "F11"), off("Step Out", "Shift+F11"), off("Restart Debugging", "Ctrl+Shift+F5"),
          off("Stop Debugging", "Shift+F5"), sep(), item("Toggle Breakpoint", "F9"), item("Edit Breakpoint…"),
          item("Enable/Disable All Breakpoints"), item("Remove All Breakpoints"), sep(), item("Debug Panel"),
          item("Debug Console")}},
        {"Git",
         {item("Commit…", "Ctrl+K"), item("Push…", "Ctrl+Shift+K"), item("Update Project…", "Ctrl+T"), item("Pull"),
          item("Fetch"), sep(), item("Branches…"), item("New Branch…"), item("Checkout Tag or Revision…"),
          item("Worktrees…"), sep(), item("Rollback…"), item("Stash Changes…"), item("Unstash Changes…"),
          item("Shelve Changes…"), item("Show Shelf"), sep(), item("Show Git Log"), item("Show History for Current File"),
          item("Compare Revisions…")}},
    };
}

// --- the project ---------------------------------------------------------------------

enum class FolderRole : u8 { Plain, Sources, Tests, Generated };

struct Entry {
    std::string name;
    i32 depth = 0;
    i32 parent = -1;
    bool folder = false;
    bool open = false;
};

std::vector<Entry> make_tree() {
    std::vector<Entry> tree;
    const auto folder = [&](std::string name, i32 depth, bool open = false) {
        tree.push_back({.name = std::move(name), .depth = depth, .folder = true, .open = open});
    };
    const auto file = [&](std::string name, i32 depth) { tree.push_back({.name = std::move(name), .depth = depth}); };
    folder("crates", 0, true);
    for (const char* name : {"agent", "dap", "devcontainer", "git", "language", "lsp", "notebook", "remote", "syntax",
                             "terminal", "text"}) {
        folder(name, 1);
        file("Cargo.toml", 2);
    }
    folder("ui", 1, true);
    folder("assets", 2);
    folder("src", 2, true);
    for (const char* name : {"editor", "settings_page", "workspace"}) {
        folder(name, 3);
        file("mod.rs", 4);
    }
    for (const char* name : {"app_state.rs", "commands.rs", "editor.rs", "file_icons.rs", "file_tree.rs", "layout.rs",
                             "main.rs", "menus.rs", "settings.rs", "shell.rs", "theme.rs", "workspace.rs"}) {
        file(name, 3);
    }
    folder("themes", 2);
    file("dark.json", 3);
    file("light.json", 3);
    file("build.rs", 2);
    file("Cargo.toml", 2);
    folder("docs", 0);
    file("notes-for-agents.md", 1);
    folder("examples", 0);
    folder("grammars", 0);
    folder("scripts", 0);
    file("install.ps1", 1);
    folder("target", 0);
    folder("vendor", 0);
    file(".gitignore", 0);
    file("Cargo.lock", 0);
    file("Cargo.toml", 0);
    file("HANDOFF.md", 0);
    file("README.md", 0);

    std::vector<i32> parents;
    for (std::size_t i = 0; i < tree.size(); ++i) {
        parents.resize(static_cast<std::size_t>(tree[i].depth));
        tree[i].parent = parents.empty() ? -1 : parents.back();
        if (tree[i].folder) {
            parents.push_back(static_cast<i32>(i));
        }
    }
    return tree;
}

FolderRole folder_role(std::string_view name) {
    for (std::string_view s : {"src", "lib", "crates", "packages", "app"}) {
        if (name == s) {
            return FolderRole::Sources;
        }
    }
    for (std::string_view s : {"test", "tests", "spec", "specs"}) {
        if (name == s) {
            return FolderRole::Tests;
        }
    }
    for (std::string_view s : {"target", "build", "dist", "out", "node_modules", ".venv", ".git"}) {
        if (name == s) {
            return FolderRole::Generated;
        }
    }
    return FolderRole::Plain;
}

Color folder_color(std::string_view name) {
    switch (folder_role(name)) {
    case FolderRole::Sources: return hex(0x5a95e6);
    case FolderRole::Tests: return hex(0x5fae68);
    case FolderRole::Generated: return hex(0xc08a5b);
    case FolderRole::Plain: break;
    }
    return hex(0x8496b0);
}

// next_code's icon for a file (file_icons.rs): a full-colour picture of its type,
// or a plain page (drawn in the muted colour).
std::string_view file_svg(std::string_view name) {
    const auto ends = [&](std::string_view suffix) { return name.ends_with(suffix); };
    if (name == "Cargo.lock") {
        return icons::lock;
    }
    if (name == ".gitignore") {
        return icons::git;
    }
    if (ends(".rs")) {
        return icons::rust;
    }
    if (ends(".toml")) {
        return icons::toml;
    }
    if (ends(".md")) {
        return icons::markdown;
    }
    if (ends(".json")) {
        return icons::json;
    }
    if (ends(".ps1")) {
        return icons::powershell;
    }
    return icons::file;
}

// --- the file in the editor: crates/ui/src/theme.rs, lines 1-96 ----------------------

constexpr std::string_view source_file = R"rs(//! Themes: the colors of the UI, the terminal and syntax highlighting.
//!
//! Built-in themes (`Dark`, `Light`) are embedded; user themes are JSON
//! files in `<config dir>/next_code/themes/`. `"theme": "<name>"` in
//! `settings.json` picks one (default `Dark`):
//!
//! ```json
//! { "name": "Mine", "extends": "Dark",
//!   "ui": { "accent": "#3574f0" },
//!   "terminal": { "cursor": "#ced0d6", "ansi": ["#1e1f22", …16 colors] },
//!   "syntax": { "keyword": "#cf8e6d", "comment.doc": { "color": "#5f826b", "italic": true } },
//!   "languages": { "python": { "syntax": { "keyword": { "bold": true } } } },
//!   "semantic_modifiers": { "deprecated": { "strikethrough": true } } }
//! ```
//!
//! A style is a color (`#rrggbb` or `#rrggbbaa`) or an object with
//! `color`, `background`, `bold`, `italic`, `underline`, `strikethrough`.
//! A scope is styled by every prefix the theme defines, most specific
//! last (`function.method` takes what `function` sets, then what
//! `function.method` sets); a language's `syntax` layers over the global
//! one. `extends` merges key by key, the child winning.

use std::{
    collections::{BTreeMap, HashMap},
    path::{Path, PathBuf},
    sync::{
        Arc, LazyLock,
        atomic::{AtomicBool, Ordering},
    },
    time::SystemTime,
};

use gpui::{App, Hsla, Rgba, rgba};
use parking_lot::{Mutex, RwLock};
use serde::{Deserialize, Deserializer, de::Error as _};
use serde_json::Value;
use syntax::{Highlight, Language, SCOPES};
use terminal::{Palette, Rgb};

use crate::settings::{self, Settings};

pub const EDITOR_FONT_SIZE: f32 = 14.;
pub const EDITOR_LINE_HEIGHT: f32 = 20.;

#[cfg(target_os = "windows")]
pub const MONO_FONT: &str = "Consolas";
#[cfg(target_os = "macos")]
pub const MONO_FONT: &str = "Menlo";
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
pub const MONO_FONT: &str = "DejaVu Sans Mono";

/// gpui alias for the platform UI font.
pub const UI_FONT: &str = ".SystemUIFont";

/// The theme used when none is chosen or the chosen one fails to load.
pub const DEFAULT: &str = "Dark";

/// Built-in themes; their names are reserved.
const BUILT_IN: &[(&str, &str)] = &[
    ("Dark", include_str!("../themes/dark.json")),
    ("Light", include_str!("../themes/light.json")),
];

/// `#rrggbb` or `#rrggbbaa`, kept as `0xrrggbbaa`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Color(u32);

impl Color {
    fn parse(text: &str) -> Result<Self, String> {
        let invalid = || format!("invalid color \"{text}\": expected #rrggbb or #rrggbbaa");
        let hex = text.strip_prefix('#').ok_or_else(invalid)?;
        if !hex.chars().all(|c| c.is_ascii_hexdigit()) {
            return Err(invalid());
        }
        let value = u32::from_str_radix(hex, 16).map_err(|_| invalid())?;
        match hex.len() {
            6 => Ok(Self(value << 8 | 0xff)),
            8 => Ok(Self(value)),
            _ => Err(invalid()),
        }
    }

    fn rgb(self) -> Rgb {
        let [r, g, b, _] = self.0.to_be_bytes();
        Rgb { r, g, b }
    }
}

impl From<Color> for Hsla {
    fn from(color: Color) -> Self {
        rgba(color.0).into()
    }
}

impl<'de> Deserialize<'de> for Color {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {)rs";

struct Token {
    std::string text; // with the spaces after it
    Color color;
    i32 chars = 0; // visible characters (the minimap's block)
};

struct Line {
    i32 indent = 0; // leading spaces
    std::vector<Token> tokens;
};

bool ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// A line of Rust split into coloured runs, as next_code's tree-sitter queries
// would colour them (near enough: no cross-line state, no semantic tokens).
Line highlight(std::string_view line) {
    static constexpr std::string_view keywords[] = {
        "as", "break", "const", "continue", "crate", "else", "enum", "fn", "for", "if", "impl", "in", "let", "loop",
        "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static", "struct", "super", "trait",
        "type", "use", "where", "while", "dyn", "unsafe", "async", "await"};
    Line out;
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ') {
        ++i;
    }
    out.indent = static_cast<i32>(i);
    const auto push = [&](std::size_t from, std::size_t to, Color color) {
        const i32 chars = static_cast<i32>(to - from);
        while (to < line.size() && line[to] == ' ') {
            ++to;
        }
        out.tokens.push_back({std::string(line.substr(from, to - from)), color, chars});
        i = to;
    };
    while (i < line.size()) {
        const char c = line[i];
        const std::string_view rest = line.substr(i);
        if (rest.starts_with("///") || rest.starts_with("//!")) {
            push(i, line.size(), syntax::doc);
        } else if (rest.starts_with("//")) {
            push(i, line.size(), syntax::comment);
        } else if (rest.starts_with("#[") || rest.starts_with("#![")) {
            std::size_t end = rest.find(']');
            end = end == std::string_view::npos ? line.size() : i + end + 1;
            push(i, end, syntax::attribute);
        } else if (c == '"') {
            std::size_t end = i + 1;
            while (end < line.size() && line[end] != '"') {
                end += line[end] == '\\' ? 2 : 1;
            }
            push(i, std::min(end + 1, line.size()), syntax::string);
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t end = i;
            while (end < line.size() && (ident_char(line[end]) || line[end] == '.')) {
                ++end;
            }
            push(i, end, syntax::number);
        } else if (ident_char(c)) {
            std::size_t end = i;
            while (end < line.size() && ident_char(line[end])) {
                ++end;
            }
            const std::string_view word = line.substr(i, end - i);
            const char next = end < line.size() ? line[end] : '\0';
            const char before = i > 0 ? line[i - 1] : '\0';
            Color color = theme::text;
            if (std::ranges::find(keywords, word) != std::end(keywords)) {
                color = syntax::keyword;
            } else if (word == "true" || word == "false") {
                color = syntax::boolean;
            } else if (next == '!') {
                color = syntax::attribute; // a macro
                ++end;
            } else if (next == '(') {
                color = syntax::function;
            } else if (word.size() > 1 && std::ranges::all_of(word, [](char ch) { return std::isupper(static_cast<unsigned char>(ch)) != 0 || ch == '_'; })) {
                color = syntax::constant;
            } else if (std::isupper(static_cast<unsigned char>(word[0])) != 0) {
                color = syntax::type;
            } else if (before == '.') {
                color = syntax::property;
            }
            push(i, end, color);
        } else {
            std::size_t end = i + 1;
            while (end < line.size() && !ident_char(line[end]) && line[end] != '"' && line[end] != ' ' && line[end] != '#' &&
                   !line.substr(end).starts_with("//")) {
                ++end;
            }
            push(i, end, theme::text);
        }
    }
    return out;
}

std::vector<Line> highlight_file(std::string_view text) {
    std::vector<Line> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        lines.push_back(highlight(text.substr(0, end)));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    return lines;
}

// Monospace like next_code's: Consolas, Menlo or DejaVu Sans Mono.
ui::Font mono_font(f32 size) {
    for (const char* path : {"C:/Windows/Fonts/consola.ttf", "/System/Library/Fonts/Menlo.ttc",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"}) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            return ui::load_ttf_font(path, size);
        }
    }
    return ui::system_ui_font(size);
}

// next_code's shadow_lg under menus.
void with_shadow(ui::BoxStyle& style) {
    for (ui::SurfaceStyle* state : {&style.surface.normal, &style.surface.hovered, &style.surface.pressed}) {
        state->shadow = {.color = Color::rgba(0, 0, 0, 90), .offset = {0.0f, 6.0f}, .spread = 0.0f, .radius = 14.0f,
                         .layers = 6, .enabled = true};
    }
}

ui::Id id_of(std::string_view name, u64 index) {
    return ui::make_id(ui::make_id(name), index);
}

class NextCodeScene final : public Scene {
public:
    NextCodeScene(std::string_view open_menu, std::string_view open_submenu)
        : _menus(make_menus()), _tree(make_tree()), _lines(highlight_file(source_file)) {
        for (std::size_t i = 0; i < _menus.size(); ++i) {
            if (_menus[i].title != open_menu) {
                continue;
            }
            _open_menu = static_cast<i32>(i);
            for (std::size_t row = 0; row < _menus[i].items.size(); ++row) {
                if (_menus[i].items[row].label == open_submenu) {
                    _submenu = static_cast<i32>(row);
                }
            }
        }
        _selected = static_cast<i32>(std::ranges::find_if(_tree, [](const Entry& e) { return e.name == "theme.rs"; }) - _tree.begin());
    }

    void update(SceneContext& ctx) override {
        if (ctx.input.pressed(Key::Escape)) {
            close_menu();
        }
    }

    void render(SceneContext& ctx) override {
        Renderer2D& r = ctx.renderer;
        r.clear(theme::background);
        if (!_fonts_loaded) {
            _sm = {.font = ui::system_ui_font(14), .scale = 1.0f, .color = theme::text};
            _xs = {.font = ui::system_ui_font(12), .scale = 1.0f, .color = theme::muted};
            _bold = {.font = ui::system_ui_font_bold(14), .scale = 1.0f, .color = theme::text};
            _code = {.font = mono_font(14), .scale = 1.0f, .color = theme::text};
            _char_width = ui::measure_text(_code.font, "0").x;
            ui::SurfaceStyle thumb{.fill = theme::scrollbar_thumb, .draw_fill = true};
            _ui.theme().scrollbar_thumb_surface = thumb;
            _ui.theme().scrollbar_track_surface = {};
            _fonts_loaded = true;
        }

        // Icons are drawn at the window's pixels, not its layout units.
        _oversample = std::max(1.0f, static_cast<f32>(r.output_size().x) / logical_size.x);
        _maximized = ctx.window.maximized();

        _ui.begin(ctx.input, r, ctx.dt);
        _ui.begin_layout({0.0f, 0.0f, logical_size.x, logical_size.y});
        {
            auto window = _ui.box(ui::make_id("window"), ui::vbox().fill().passive());
            menu_bar();
            project_tabs();
            {
                auto workspace = _ui.box(ui::make_id("workspace"), ui::hbox().fill().passive());
                files_dock();
                {
                    auto middle = _ui.box(ui::make_id("middle"), ui::vbox().fill().passive());
                    pane();
                    bottom_strip();
                }
                right_strip();
            }
        }
        _ui.end_layout();
        _ui.end();
        _ui.apply(ctx.app, ctx.window); // also tells the window which parts of the bar drag it
        switch (std::exchange(_caption, Caption::None)) {
        case Caption::Minimize: ctx.window.minimize(); break;
        case Caption::Maximize: ctx.window.toggle_maximized(); break;
        case Caption::Close: ctx.window.request_close(); break;
        case Caption::None: break;
        }
    }

private:
    ui::TextStyle with_color(ui::TextStyle style, Color color) const {
        style.color = color;
        return style;
    }

    void line_x(std::string_view name) { leaf(ui::make_id(name), ui::hbox().w(ui::grow()).h(1.0f).bg(theme::border).passive()); }
    void line_y(std::string_view name) { leaf(ui::make_id(name), ui::vbox().w(1.0f).h(ui::grow()).bg(theme::border).passive()); }

    // A box with nothing in it: a line, a bar, a block.
    void leaf(ui::Id id, const ui::BoxStyle& style) {
        _ui.begin_box(id, style);
        _ui.end_box();
    }

    void close_menu() {
        _open_menu = -1;
        _submenu = -1;
    }

    // --- title bar: icon, menus, branch, command bar, run configuration -----------------

    void menu_bar() {
        {
            // Its empty parts move the window, as a system title bar would.
            auto bar = _ui.box(ui::make_id("menu_bar"), ui::hbox().w(ui::grow()).h(menu_bar_height - 1.0f).pad(8, 0, 0, 0).gap(2)
                                                               .items(UiAlign::Center).bg(theme::panel).passive().drags_window());
            app_icon();
            for (std::size_t i = 0; i < _menus.size(); ++i) {
                menu_title(static_cast<i32>(i));
            }
            branch_widget();
            _ui.spacer({.width = ui::grow()});
            command_bar();
            _ui.spacer({.width = ui::grow()});
            run_selector();
            caption_buttons();
        }
        line_x("menu_bar_line");
    }

    // Minimize, maximize (restore) and close, drawn like Windows 11's.
    void caption_buttons() {
        const auto button = [&](const char* name, Caption action, Color hover) {
            auto box = _ui.box(ui::make_id(name), ui::hbox().w(46).h(ui::grow()).center().hover_bg(hover));
            if (box.clicked) {
                _caption = action;
            }
            return box;
        };
        {
            auto min = button("caption_min", Caption::Minimize, theme::hover_row);
            leaf(ui::make_id("caption_min_glyph"), ui::hbox().size(10, 1).bg(theme::text).passive());
        }
        {
            auto max = button("caption_max", Caption::Maximize, theme::hover_row);
            if (_maximized) {
                // Restore: two windows, one behind the other.
                auto glyph = _ui.box(ui::make_id("caption_restore"), ui::hbox().size(10, 10).passive());
                leaf(ui::make_id("caption_restore_back"),
                     ui::hbox().size(8, 8).border(theme::text).at(UiAnchor::TopLeft, {2, 0}).passive());
                leaf(ui::make_id("caption_restore_front"),
                     ui::hbox().size(8, 8).bg(theme::panel).border(theme::text).at(UiAnchor::TopLeft, {0, 2}).passive());
            } else {
                leaf(ui::make_id("caption_max_glyph"), ui::hbox().size(10, 10).border(theme::text).passive());
            }
        }
        {
            auto close = button("caption_close", Caption::Close, theme::error);
            _ui.svg(icons::close, {14, 14}, close.hovered ? theme::on_accent : theme::text, {.oversample = _oversample, .mask = true});
        }
    }

    void app_icon() {
        ui::BoxStyle style = ui::hbox().size(18, 18).center().rounded(4).margins(0, 0, 4, 0).passive();
        style.surface.normal.fill_kind = ui::SurfaceFill::Gradient;
        style.surface.normal.gradient = {.start = hex(0x4f7cff), .end = hex(0x8e5cf6), .direction = GradientDirection::Horizontal, .mix = {}};
        style.surface.normal.draw_fill = true;
        style.draw_surface = true;
        auto icon = _ui.box(ui::make_id("app_icon"), style);
        _ui.label("N", with_color(_bold, colors::white), {.width = ui::fit()});
    }

    void menu_title(i32 index) {
        const bool open = _open_menu == index;
        const Menu& menu = _menus[static_cast<std::size_t>(index)];
        auto title = _ui.box(id_of("menu_title", static_cast<u64>(index)),
                             ui::hbox().h(24).pad(8, 0).items(UiAlign::Center).rounded(2).hover_bg(theme::hover_row)
                                 .selected_bg(theme::active_row).selected_if(open).cursor_shape(SystemCursor::Pointer));
        _ui.label(menu.title, _sm);
        if (title.pressed) {
            const bool reopen = !open;
            close_menu();
            _open_menu = reopen ? index : -1;
        } else if (title.hovered && _open_menu >= 0 && !open) {
            // As in native menu bars: with a menu open, pointing at another title opens it.
            close_menu();
            _open_menu = index;
        }
        if (_open_menu == index) {
            // next_code hangs it MENU_BAR_HEIGHT - 4 below the title's top.
            const bool outside = menu_panel(id_of("menu_panel", static_cast<u64>(index)), menu.items, ui::UiFloat::Below,
                                            {0.0f, menu_bar_height - 4.0f - 24.0f}, 0);
            // A press elsewhere closes it, unless on the bar (another title, or this one, decides).
            if (outside && _ui.pointer().y >= menu_bar_height) {
                close_menu();
            }
        }
    }

    // One menu panel, floating `side` of the box it is built in: the title (a
    // menu) or its row (a submenu, `depth` 1). True when a button went down
    // outside it and its open submenu.
    bool menu_panel(ui::Id id, const std::vector<MenuItem>& items, ui::UiFloat side, Vec2f offset, i32 depth) {
        ui::BoxStyle style = ui::vbox().w(menu_width).pad(4).bg(theme::panel).border(theme::border).rounded(6).floating(side, offset);
        with_shadow(style);
        auto panel = _ui.box(id, style);
        bool outside_submenu = true;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const MenuItem& entry = items[i];
            const ui::Id row_id = ui::make_id(id, static_cast<u64>(i));
            if (entry.label.empty()) {
                leaf(row_id, ui::hbox().w(ui::grow()).h(1).margins(0, 4).bg(theme::border).passive());
                continue;
            }
            const bool submenu_open = depth == 0 && _submenu == static_cast<i32>(i);
            auto row = _ui.box(row_id, ui::hbox().w(ui::grow()).h(26).pad(12, 0).gap(8).items(UiAlign::Center).rounded(2)
                                           .hover_bg(theme::active_row).selected_bg(theme::active_row).selected_if(submenu_open)
                                           .disabled_if(!entry.enabled));
            _ui.label(entry.label, entry.enabled ? _sm : with_color(_sm, theme::muted), {.width = ui::grow()},
                      ui::TextOverflow::Ellipsis);
            if (!entry.keys.empty()) {
                _ui.label(entry.keys, _xs);
            }
            if (!entry.items.empty()) {
                _ui.label("›", with_color(_sm, theme::muted));
            }
            if (depth == 0 && row.hovered && entry.enabled) {
                _submenu = entry.items.empty() ? -1 : static_cast<i32>(i); // next_code: on hover
            }
            if (submenu_open) {
                // next_code: MENU_WIDTH - 8 right of the row's left (its right edge), 4 above it.
                outside_submenu = menu_panel(ui::make_id(row_id, "submenu"), entry.items, ui::UiFloat::Right, {0.0f, -4.0f}, 1);
            }
            if (row.clicked && entry.items.empty()) {
                close_menu(); // runs the command in next_code
            }
        }
        return panel.pressed_outside && outside_submenu;
    }

    // PyCharm's branch widget: ⎇ main ▾
    void branch_widget() {
        auto widget = _ui.box(ui::make_id("branch"), ui::hbox().h(24).pad(8, 0).margins(8, 0, 0, 0).gap(4).items(UiAlign::Center)
                                                         .rounded(6).hover_bg(theme::hover_row).cursor_shape(SystemCursor::Pointer));
        _ui.svg(icons::branch, {14, 14}, theme::muted, {.oversample = _oversample, .mask = true});
        _ui.label("main", _sm);
        _ui.label("▾", _xs);
    }

    void command_bar() {
        ui::BoxStyle style = ui::hbox().w(440).h(24).pad(8, 0).gap(8).items(UiAlign::Center).rounded(6).border(theme::border)
                                 .bg(theme::background).cursor_shape(SystemCursor::Pointer);
        style.surface.hovered.border = theme::accent;
        style.surface.pressed.border = theme::accent;
        auto bar = _ui.box(ui::make_id("command_bar"), style);
        _ui.label("⌕", _xs);
        _ui.label("Search commands", with_color(_sm, theme::muted), {.width = ui::grow()});
        _ui.label("Ctrl+Shift+P", _xs);
    }

    void run_selector() {
        auto group = _ui.box(ui::make_id("run"), ui::hbox().gap(2).items(UiAlign::Center).margins(0, 0, 8, 0).passive());
        const auto button = [] {
            return ui::hbox().h(24).pad(8, 0).gap(4).items(UiAlign::Center).rounded(6).hover_bg(theme::hover_row)
                .cursor_shape(SystemCursor::Pointer);
        };
        {
            auto config = _ui.box(ui::make_id("run_config"), button().border(theme::border));
            _ui.label("Add Configuration…", with_color(_sm, theme::muted));
            _ui.label("▾", _xs);
        }
        {
            auto run = _ui.box(ui::make_id("run_run"), button());
            _ui.label("▶", with_color(_sm, theme::run_marker));
        }
        {
            auto debug = _ui.box(ui::make_id("run_debug"), button());
            _ui.label("Debug", with_color(_xs, theme::accent_text));
        }
    }

    // --- project tabs ------------------------------------------------------------------

    void project_tabs() {
        {
            auto bar = _ui.box(ui::make_id("project_tabs"), ui::hbox().w(ui::grow()).h(project_tabs_height - 1.0f).bg(theme::panel).passive());
            static constexpr const char* projects[] = {"nextcode", "kin"};
            for (u64 i = 0; i < 2; ++i) {
                const bool active = _project == static_cast<i32>(i);
                underlined_tab(id_of("project_tab", i), projects[i], active, 12, 8, [&](const ui::BoxResult& tab) {
                    _ui.label("×", with_color(_sm, theme::muted), {.width = ui::fixed(14)});
                    if (tab.clicked) {
                        _project = static_cast<i32>(i);
                    }
                });
                line_y(std::string("project_tab_line") + projects[i]);
            }
            auto add = _ui.box(ui::make_id("project_add"), ui::hbox().h(ui::grow()).pad(12, 0).items(UiAlign::Center)
                                                             .cursor_shape(SystemCursor::Pointer));
            _ui.label("+", with_color(_sm, add.hovered ? theme::text : theme::muted));
        }
        line_x("project_tabs_line");
    }

    // A tab whose selection is a 2 px accent line along its bottom (project tabs,
    // dock tabs, editor tabs). `extra` adds what follows the title.
    template <class Extra>
    void underlined_tab(ui::Id id, std::string_view title, bool active, f32 pad_left, f32 pad_right, Extra&& extra,
                        Color fill = theme::background, bool hover_fill = false) {
        ui::BoxStyle style = ui::vbox().h(ui::grow()).cursor_shape(SystemCursor::Pointer);
        if (active) {
            style.bg(fill);
        } else if (hover_fill) {
            style.hover_bg(theme::hover_row);
        }
        auto tab = _ui.box(id, style);
        {
            auto body = _ui.box(ui::make_id(id, "body"), ui::hbox().h(ui::grow()).pad(pad_left, 0, pad_right, 0).gap(8)
                                                             .items(UiAlign::Center).passive());
            _ui.label(title, with_color(_sm, active || tab.hovered ? theme::text : theme::muted), {}, ui::TextOverflow::Ellipsis);
            extra(tab);
        }
        leaf(ui::make_id(id, "underline"), ui::hbox().w(ui::grow()).h(2).bg(active ? theme::accent : colors::transparent).passive());
    }

    // --- left dock: Files ---------------------------------------------------------------

    void files_dock() {
        {
            auto dock = _ui.box(ui::make_id("left_dock"), ui::vbox().w(280).h(ui::grow()).bg(theme::panel).passive());
            {
                auto header = _ui.box(ui::make_id("left_dock_header"), ui::hbox().w(ui::grow()).h(dock_header_height - 1.0f).passive());
                static constexpr const char* panels[] = {"Files", "Search", "Commit", "Python", "Tests"};
                for (u64 i = 0; i < 5; ++i) {
                    underlined_tab(id_of("dock_tab", i), panels[i], i == 0, 6, 6, [](const ui::BoxResult&) {}, colors::transparent);
                }
                _ui.spacer({.width = ui::grow()});
                auto hide = _ui.box(ui::make_id("dock_hide"), ui::hbox().h(ui::grow()).pad(8, 0).items(UiAlign::Center)
                                                                .cursor_shape(SystemCursor::Pointer));
                _ui.label("–", with_color(_sm, hide.hovered ? theme::text : theme::muted));
            }
            line_x("left_dock_header_line");
            {
                auto header = _ui.box(ui::make_id("tree_header"), ui::hbox().w(ui::grow()).pad(12, 8).passive());
                _ui.label("NEXTCODE", _xs);
            }
            tree();
        }
        leaf(ui::make_id("left_handle"), ui::vbox().w(handle_size).h(ui::grow()).bg(theme::border).hover_bg(theme::accent)
                                                .cursor_shape(SystemCursor::ResizeEW));
    }

    void tree() {
        std::vector<i32> rows;
        for (i32 i = 0; i < static_cast<i32>(_tree.size()); ++i) {
            bool visible = true;
            for (i32 p = _tree[static_cast<std::size_t>(i)].parent; p >= 0; p = _tree[static_cast<std::size_t>(p)].parent) {
                visible = visible && _tree[static_cast<std::size_t>(p)].open;
            }
            if (visible) {
                rows.push_back(i);
            }
        }
        auto scroll = _ui.scroll(ui::make_id("tree"), ui::vbox().fill(), {.scrollbar_thickness = 8.0f});
        _ui.list(ui::make_id("tree_rows"), static_cast<i32>(rows.size()), tree_row_height, [&](i32 row) {
            const i32 index = rows[static_cast<std::size_t>(row)];
            Entry& entry = _tree[static_cast<std::size_t>(index)];
            auto item = _ui.box(id_of("tree_row", static_cast<u64>(index)),
                                ui::hbox().fill().pad(6.0f + 14.0f * static_cast<f32>(entry.depth), 0, 8, 0).gap(5).items(UiAlign::Center)
                                    .hover_bg(theme::hover_row).selected_bg(theme::active_row).selected_if(index == _selected)
                                    .cursor_shape(SystemCursor::Pointer));
            _ui.label(entry.folder ? (entry.open ? "▾" : "▸") : "", _xs, {.width = ui::fixed(10)});
            const ui::SvgOptions mask{.oversample = _oversample, .mask = true};
            if (entry.folder) {
                _ui.svg(entry.open ? icons::folder_open : icons::folder, {16, 16}, folder_color(entry.name), mask);
            } else if (const std::string_view svg = file_svg(entry.name); svg == icons::file) {
                _ui.svg(svg, {16, 16}, theme::muted, mask);
            } else {
                _ui.svg(svg, {16, 16}, colors::white, {.oversample = _oversample});
            }
            _ui.label(entry.name, _sm, {}, ui::TextOverflow::Ellipsis);
            if (item.clicked) {
                _selected = index;
                entry.open = entry.folder && !entry.open;
            }
        });
    }

    // --- editor pane ---------------------------------------------------------------------

    void pane() {
        auto pane = _ui.box(ui::make_id("pane"), ui::vbox().fill().passive());
        {
            auto bar = _ui.box(ui::make_id("tab_bar"), ui::hbox().w(ui::grow()).h(tab_bar_height - 1.0f).bg(theme::panel).passive());
            static constexpr const char* files[] = {"theme.rs", "shell.rs", "menus.rs", "Cargo.toml"};
            for (u64 i = 0; i < 4; ++i) {
                const bool active = _tab == static_cast<i32>(i);
                underlined_tab(id_of("tab", i), files[i], active, 12, 8, [&](const ui::BoxResult& tab) {
                    auto close = _ui.box(ui::make_id(id_of("tab", i), "close"), ui::hbox().size(18, 18).center().rounded(2)
                                                                                   .hover_bg(theme::hover_row));
                    _ui.label(i == 1 ? "●" : "×", with_color(_sm, close.hovered ? theme::text : theme::muted));
                    if (tab.clicked) {
                        _tab = static_cast<i32>(i);
                    }
                }, theme::background, true);
                line_y(std::string("tab_line") + files[i]);
            }
        }
        line_x("tab_bar_line");
        breadcrumbs();
        editor();
    }

    void breadcrumbs() {
        {
            auto bar = _ui.box(ui::make_id("breadcrumbs"), ui::hbox().w(ui::grow()).h(21).pad(12, 0).gap(4).items(UiAlign::Center)
                                                               .bg(theme::background).passive());
            static constexpr const char* crumbs[] = {"crates", "ui", "src", "theme.rs", "Color", "parse"};
            for (u64 i = 0; i < 6; ++i) {
                if (i > 0) {
                    _ui.label("›", _xs);
                }
                auto crumb = _ui.box(id_of("crumb", i), ui::hbox().pad(4, 1).rounded(2).hover_bg(theme::hover_row)
                                                            .cursor_shape(SystemCursor::Pointer));
                _ui.label(crumbs[i], with_color(_xs, crumb.hovered ? theme::text : theme::muted));
            }
        }
        line_x("breadcrumbs_line");
    }

    void editor() {
        constexpr i32 caret_line = 69; // 0-based: in Color::parse
        constexpr i32 caret_column = 26;
        const i32 digits = 3;
        const f32 gutter = 8.0f + _char_width * static_cast<f32>(digits) + 6.0f + 3.0f + 6.0f + _char_width * 1.5f + 6.0f;
        const ui::Id scroll_id = ui::make_id("code");
        auto row = _ui.box(ui::make_id("editor_row"), ui::hbox().fill().bg(theme::background).passive());
        {
            auto code = _ui.scroll(scroll_id, ui::vbox().fill().cursor_shape(SystemCursor::Text), {.scrollbar_thickness = 12.0f});
            _editor_height = code.bounds.h;
            _ui.list(ui::make_id("code_lines"), static_cast<i32>(_lines.size()), line_height, [&](i32 n) {
                const Line& line = _lines[static_cast<std::size_t>(n)];
                ui::BoxStyle style = ui::hbox().w(ui::grow()).h(line_height).items(UiAlign::Center).passive();
                if (n == caret_line) {
                    style.bg(theme::current_line);
                }
                auto text = _ui.box(id_of("code_line", static_cast<u64>(n)), style);
                // Gutter: line number, change bar, fold column.
                {
                    auto number = _ui.box(id_of("line_number", static_cast<u64>(n)),
                                          ui::hbox().w(8.0f + _char_width * digits).justify(UiAlign::End).passive());
                    _ui.label(std::to_string(n + 1), with_color(_code, n == caret_line ? theme::text : theme::muted));
                }
                const Color change = n >= 63 && n <= 66 ? theme::git_modified : n >= 84 && n <= 85 ? theme::git_added : colors::transparent;
                leaf(id_of("change_bar", static_cast<u64>(n)), ui::hbox().size(3, line_height).margins(6, 0, 6, 0).bg(change).passive());
                _ui.spacer({.width = ui::fixed(_char_width * 1.5f + 6.0f)});
                // Indentation guides, then the coloured runs.
                for (i32 level = 0; level < line.indent / 4; ++level) {
                    auto guide = _ui.box(ui::make_id(id_of("guide", static_cast<u64>(n)), static_cast<u64>(level)),
                                         ui::hbox().w(_char_width * 4.0f).h(line_height).passive());
                    leaf(ui::make_id(id_of("guide_line", static_cast<u64>(n)), static_cast<u64>(level)),
                            ui::hbox().w(1).h(ui::grow()).bg(theme::indent_guide).passive());
                }
                if (line.indent % 4 != 0) {
                    _ui.spacer({.width = ui::fixed(_char_width * static_cast<f32>(line.indent % 4))});
                }
                // The coloured runs, laid out as one line.
                ui::AdvancedText runs;
                for (const Token& token : line.tokens) {
                    ui::TextRun run;
                    run.text = token.text;
                    run.style = with_color(_code, token.color);
                    runs.content.runs.emplace_back(std::move(run));
                }
                runs.layout_options.wrap = false;
                _ui.add(std::move(runs));
                if (n == caret_line) {
                    leaf(ui::make_id("caret"), ui::hbox().size(2, 18).bg(theme::cursor)
                                                      .at(UiAnchor::TopLeft, {gutter + _char_width * caret_column, 1}).passive());
                }
            });
        }
        minimap(scroll_id);
    }

    // The document scaled down: a 1 px column per character, a 2 px row per line,
    // and the slider over the lines in view.
    void minimap(ui::Id scroll_id) {
        auto map = _ui.box(ui::make_id("minimap"), ui::vbox().w(minimap_width).h(ui::grow()).pad(6, 0, 0, 0).passive());
        for (std::size_t n = 0; n < _lines.size(); ++n) {
            const Line& line = _lines[n];
            auto row = _ui.box(id_of("minimap_line", n), ui::hbox().w(ui::grow()).h(2).passive());
            _ui.spacer({.width = ui::fixed(static_cast<f32>(line.indent))});
            for (std::size_t t = 0; t < line.tokens.size(); ++t) {
                const Token& token = line.tokens[t];
                Color color = token.color;
                color.a = 150;
                leaf(ui::make_id(id_of("minimap_block", n), t), ui::hbox().size(static_cast<f32>(token.chars), 1.5f).bg(color).passive());
                const f32 spaces = static_cast<f32>(token.text.size()) - static_cast<f32>(token.chars);
                if (spaces > 0.0f) {
                    _ui.spacer({.width = ui::fixed(spaces)});
                }
            }
        }
        const f32 top = _ui.scroll_offset(scroll_id).y / line_height * 2.0f;
        const f32 height = std::max(8.0f, _editor_height / line_height * 2.0f);
        auto slider = _ui.box(ui::make_id("minimap_slider"), ui::vbox().size(minimap_width, height).bg(theme::scrollbar_thumb)
                                                               .hover_bg(theme::scrollbar_thumb_hover).at(UiAnchor::TopLeft, {-6.0f, top}));
    }

    // --- collapsed docks -----------------------------------------------------------------

    void bottom_strip() {
        line_x("bottom_strip_line");
        auto strip = _ui.box(ui::make_id("bottom_strip"), ui::hbox().w(ui::grow()).h(strip_size - 1.0f).pad(8, 0).gap(4)
                                                              .items(UiAlign::Center).bg(theme::panel).passive());
        static constexpr const char* panels[] = {"Terminal", "Problems", "Git", "Ports", "Call Hierarchy", "Debug Console", "Notes", "Extensions"};
        for (u64 i = 0; i < 8; ++i) {
            strip_button(id_of("bottom_strip_button", i), panels[i]);
        }
    }

    void right_strip() {
        line_y("right_strip_line");
        auto strip = _ui.box(ui::make_id("right_strip"), ui::vbox().w(strip_size - 1.0f).h(ui::grow()).pad(0, 8, 0, 0).gap(4)
                                                             .items(UiAlign::Center).bg(theme::panel).passive());
        strip_button(ui::make_id("strip_outline"), "O");
        strip_button(ui::make_id("strip_debug"), "D");
    }

    void strip_button(ui::Id id, std::string_view label) {
        auto button = _ui.box(id, ui::hbox().pad(4, 2).rounded(2).hover_bg(theme::hover_row).cursor_shape(SystemCursor::Pointer));
        _ui.label(label, with_color(_xs, button.hovered ? theme::text : theme::muted));
    }

    ui::Context _ui;
    ui::TextStyle _sm{};
    ui::TextStyle _xs{};
    ui::TextStyle _bold{};
    ui::TextStyle _code{};
    bool _fonts_loaded = false;
    f32 _oversample = 1.0f; // texels per layout unit, for icons
    f32 _char_width = 8.0f;
    f32 _editor_height = 600.0f;
    std::vector<Menu> _menus;
    std::vector<Entry> _tree;
    std::vector<Line> _lines;
    i32 _open_menu = -1;
    i32 _submenu = -1; // the open submenu's row in the open menu
    enum class Caption : u8 { None, Minimize, Maximize, Close };
    Caption _caption = Caption::None; // a caption button clicked this frame
    bool _maximized = false;
    i32 _selected = -1;
    i32 _project = 0;
    i32 _tab = 0;
};

} // namespace
} // namespace demo

int main(int argc, char** argv) {
    kin::GameInfo game = demo::make_game_info();
    std::string open_menu;
    std::string open_submenu;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--menu=")) {
            open_menu = std::string(arg.substr(7));
        } else if (arg.starts_with("--submenu=")) {
            open_submenu = std::string(arg.substr(10));
        }
    }
    kin::SceneManager scenes;
    const auto build_scenes = [open_menu, open_submenu](kin::SceneManager& target) {
        target.push(std::make_unique<demo::NextCodeScene>(open_menu, open_submenu));
    };
    build_scenes(scenes);

    return kin::run_scene_app({
        .window = kin::window_config(game),
        .headless = kin::parse_headless_options(argc, argv),
        .game = &game,
        .render_headless = true,
        .reset_scenes = build_scenes,
    }, scenes);
}
