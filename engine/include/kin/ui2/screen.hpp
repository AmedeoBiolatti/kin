#pragma once

// A UI screen as data: a .kinui file of a game's content (a HUD, a pause
// menu, a title screen), a tree of elements laid out as ui2's composition
// lays boxes out, drawn in the look of a .kintheme file it names.
//
//     {"schema": "kin.ui/1", "theme": "ui/game.kintheme", "size": {"x": 1280, "y": 720},
//      "root": {"kind": "box", "width": "grow", "height": "grow", "children": [
//        {"id": "score", "kind": "label", "text": "Score: 0", "style": "title",
//         "anchor": "top_left", "offset": {"x": 24, "y": 16}},
//        {"id": "menu", "kind": "panel", "anchor": "center", "padding": [24, 20, 24, 20], "gap": 10, "children": [
//          {"id": "play", "kind": "button", "text": "Play", "width": 200},
//          {"id": "volume", "kind": "slider", "value": 0.8, "width": 200}]}]}}
//
// Elements: box (a container: its children in a column, or a row; a theme
// surface behind them when it names one), panel (a box on the theme's panel
// surface), label, button, image, progress, toggle, slider, text_input and
// spacer. Each may have an id the game reaches it by (UiScreen::set_text,
// clicked, ...). A size is "fit" (as big as what is in it), "grow" (the room
// left, shared), a number (points) or "50%" (of its parent's). An element
// with an anchor is placed over its parent at that corner (or middle),
// `offset` from it, out of the flow of its siblings. Text may be a
// translation key (`key`: kin::tr(key), the language's text).
//
// Draw one with UiScreen between Context::begin() and end():
//
//     kin::ui2::UiScreen hud{document};
//     hud.set_text("score", "Score: " + std::to_string(score));
//     hud.draw(ui, {0, 0, 1280, 720});
//     if (hud.clicked("play")) start();   // after drawing: this frame's results

#include <kin/renderer/color.hpp>
#include <kin/renderer/texture.hpp>
#include <kin/ui2/context.hpp>
#include <kin/ui2/layout.hpp>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin::ui2 {

enum class ScreenKind : u8 { Box, Panel, Label, Button, Image, Progress, Toggle, Slider, TextInput, Spacer };

// "box", "panel", "label", "button", "image", "progress", "toggle", "slider", "text_input", "spacer".
std::string_view screen_kind_name(ScreenKind kind);
std::optional<ScreenKind> screen_kind_named(std::string_view name);
// Whether elements of `kind` hold others (box, panel).
bool screen_kind_holds(ScreenKind kind);
// The theme's text styles an element may name: "body", "title", "small", "muted", "emphasis", "danger".
std::vector<std::string_view> screen_text_styles();
// The theme surfaces a box may stand on ("panel", "card", "header", ...).
std::vector<std::string_view> screen_surfaces();
// "top_left", "top", "top_right", "left", "center", "right", "bottom_left", "bottom", "bottom_right".
std::string_view screen_anchor_name(UiAnchor anchor);
std::optional<UiAnchor> screen_anchor_named(std::string_view name);
// A size as a file writes it ("fit", "grow", "120", "50%"), and back.
std::string screen_size_text(SizeAxis size);
std::optional<SizeAxis> screen_size_from_text(std::string_view text);

struct ScreenElement {
    std::string id;
    ScreenKind kind = ScreenKind::Box;
    // Where it goes.
    SizeAxis width = fit();
    SizeAxis height = fit();
    std::optional<UiAnchor> anchor; // placed over its parent there, out of the flow
    Vec2f offset{};                 // from the anchor
    bool visible = true;
    // A box's or panel's: its children side by side (else one under another),
    // the room inside its edges (left, top, right, bottom), between them, and
    // how they are aligned along it (justify) and across it (items).
    bool row = false;
    UiPadding padding{};
    f32 gap = 0.0f;
    UiAlign justify = UiAlign::Start;
    UiAlign items = UiAlign::Start;
    std::string surface; // a box's theme surface (empty: none); a panel's (empty: panel)
    // What it shows.
    std::string text;     // label, button, toggle: its text; text_input: what it holds at first
    std::string key;      // a translation key: the language's text in place of `text`
    std::string style;    // the theme's text style (empty: body)
    UiAlign align = UiAlign::Start; // a label's text, across it
    std::string image;    // image: a path in the content
    Color tint = colors::white;
    f32 value = 0.0f;     // progress (0..1), slider
    f32 min = 0.0f;       // slider
    f32 max = 1.0f;
    f32 step = 0.0f;
    bool checked = false; // toggle
    std::string hint;     // text_input: shown while it is empty
    std::vector<ScreenElement> children;

    friend bool operator==(const ScreenElement&, const ScreenElement&) = default;
};

struct ScreenDocument {
    std::string theme;           // a .kintheme of the content (empty: the context's)
    Vec2f size{1280.0f, 720.0f}; // the size it is made for (an editor's preview)
    ScreenElement root;

    // The element with `id` (depth first); null when none has it.
    const ScreenElement* find(std::string_view id) const;
    ScreenElement* find(std::string_view id);

    friend bool operator==(const ScreenDocument&, const ScreenDocument&) = default;
};

// A .kinui file's text read (false: `error` says where and why), and written.
bool parse_screen(std::string_view text, ScreenDocument& out, std::string& error);
std::string serialize_screen(const ScreenDocument& screen);

// What a screen draws with: images (by path in the content) and, when
// given, translations (else kin::tr).
struct ScreenAssets {
    std::function<Texture(std::string_view)> texture;
    std::function<std::string(std::string_view)> translate;
};

class UiScreen {
public:
    UiScreen() = default;
    explicit UiScreen(ScreenDocument document);

    // A new version of its document (its file changed): what the game set by id stays.
    void set_document(ScreenDocument document);
    const ScreenDocument& document() const { return _document; }
    bool has(std::string_view id) const { return _document.find(id) != nullptr; }

    // What the game sets, by element id; kept until set again.
    void set_text(std::string_view id, std::string text);
    void set_value(std::string_view id, f32 value);
    void set_checked(std::string_view id, bool checked);
    void set_visible(std::string_view id, bool visible);
    void set_image(std::string_view id, std::string path);
    void set_enabled(std::string_view id, bool enabled);

    // Its elements laid out over `bounds` (points), in the context's theme,
    // and run: inside Context::begin()/end(), outside any layout. A screen
    // that draws for the first time shows its results from its second.
    void draw(Context& ctx, Rectf bounds, const ScreenAssets& assets = {});

    // What the player did, at the last draw (until clear_events()).
    bool clicked(std::string_view id) const;
    bool changed(std::string_view id) const; // a slider moved, a toggle flipped, text typed
    std::vector<std::string> clicks() const;
    void clear_events();
    // What an element holds now (as the player left it, or the game set it).
    f32 value(std::string_view id) const;
    bool checked(std::string_view id) const;
    std::string text(std::string_view id) const;
    bool visible(std::string_view id) const;

    // Where each element was laid out at the last draw (a frame late), by its
    // path: the indices from the root ("" the root, "0/2" its first child's
    // third), for an editor's picking and outlines.
    struct Placed {
        std::string path;
        Rectf bounds{};
        bool hovered = false;
    };
    const std::vector<Placed>& placed() const { return _placed; }
    // The id seed of its ui2 boxes (screens drawn in one context differ in it).
    void set_seed(u64 seed) { _seed = seed; }

private:
    struct State {
        std::optional<std::string> text;
        std::optional<f32> value;
        std::optional<bool> checked;
        std::optional<bool> visible;
        std::optional<std::string> image;
        std::optional<bool> enabled;
        bool clicked = false;
        bool changed = false;
        UiTextInputState input{};
        bool input_made = false;
    };
    // Widgets kept until end_layout() writes their results back.
    struct Live {
        std::string id;
        ScreenKind kind = ScreenKind::Button;
        std::unique_ptr<Button> button;
        std::unique_ptr<Toggle> toggle;
        std::unique_ptr<Slider> slider;
        std::unique_ptr<TextInput> input;
    };

    void element(Context& ctx, const ScreenElement& element, const std::string& path, const ScreenAssets& assets);
    State& state(std::string_view id);
    const State* find_state(std::string_view id) const;

    ScreenDocument _document;
    std::map<std::string, State, std::less<>> _states;
    std::deque<Live> _live;
    std::vector<Placed> _placed;
    std::vector<Placed> _placing;
    u64 _seed = 0x6b696e7569; // "kinui"
};

} // namespace kin::ui2
