#pragma once

// Text drawn in the world at an entity (a name over a head, a sign, a score
// that floats up), moving, turning and scaling with it and with the camera,
// sorted among the world's sprites by its layer and order. Its font is a
// font file of the game's content (`font`, as scene files name it; Assets2D
// reads it, at `size`), or the system's UI font:
//
//     sign.set(kin::TextRenderer{.text = "Shop", .size = 24.0f});
//
// Lines break at '\n', and at spaces past `wrap` wide (0: only at '\n').
// `pivot` is the point of the text's box that is at the entity (0.5, 0.5:
// its centre). Text for menus and HUDs is ui2's (kin/ui2).

#include <kin/ecs/component.hpp>
#include <kin/renderer/color.hpp>
#include <kin/renderer/render_layer.hpp>
#include <kin/ui2/text.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

enum class TextAlign : u8 { Left, Center, Right };

struct TextRenderer {
    std::string text;
    // Translated (kin/l10n): the active localization's text under `key`, else
    // under `auto_key` (a scene sets it: scene_text_key()), else `text`.
    std::string key;
    std::string auto_key;
    ComponentAssetRef font; // a .ttf or .otf of the content; empty: the system's UI font
    f32 size = 16.0f;       // points
    Color color = colors::white;
    Vec2f offset{};
    Vec2f pivot{0.5f, 0.5f};
    f32 wrap = 0.0f;
    TextAlign align = TextAlign::Center; // its lines, within its box
    f32 outline = 0.0f; // points round each letter
    Color outline_color = colors::black;
    i32 layer = layer_value(RenderLayer::World);
    i32 order = 0;
    bool y_sort = false;
    f32 sort_y_offset = 0.0f;
    bool visible = true;

    // The font, set by Assets2D (bind_text_renderers) from `font` and `size`.
    ui2::Font face{};
};

// "left", "center", "right".
std::string_view text_align_name(TextAlign align);
std::optional<TextAlign> text_align_named(std::string_view name);

// Its lines laid out in the entity's own units (the entity at (0, 0)): each
// line's width, their height, their box's size and its top-left corner.
struct TextLayout {
    std::vector<std::string> lines;
    std::vector<f32> widths;
    f32 line_height = 0.0f;
    Vec2f size{};
    Vec2f at{};
};
TextLayout layout_text(const TextRenderer& text);
// What it shows: its text, translated where the active localization has its key.
std::string shown_text(const TextRenderer& text);
// The key a scene's text is translated under: "scene.<scene>.<entity id>"
// (the scene's name lower case, its spaces '_').
std::string scene_text_key(std::string_view scene_name, std::string_view entity_id);
// Its lines drawn as laid out, through the renderer's transform.
void draw_text_layout(Renderer2D& renderer, const TextRenderer& text, const TextLayout& layout);
// Its box (outline included), in the entity's own units; empty with no face.
Rectf text_bounds(const TextRenderer& text);

} // namespace kin
