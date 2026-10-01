#pragma once

#include <kin/anim/value.hpp>
#include <kin/ui2/color_scale.hpp>
#include <kin/ui2/surface.hpp>
#include <kin/ui2/widgets.hpp>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kin {
class Renderer2D;
}

namespace kin::ui2 {

enum class PalettePreset {
    Default,
    Slate,
    Ember,
    Verdant,
    Parchment,
    HighContrast,
};

enum class StylePresetKind {
    Game,
    Editor,
    Compact,
};

enum class RadixThemePreset {
    Shadcn,
    Midnight,
    Aurora,
    Candy,
    Forest,
    Ember,
    Terminal,
    Sandstone,
};

// Single source of truth for the default color values shared by Palette,
// ThemeColorTokens, and Theme's loose color fields. A raw Palette{} / Theme{}
// stays consistent because every duplicated default points at one constant here.
// Values mirror the scale-derived Default preset (Radix slate_dark + blue_dark):
// real surface steps, white-alpha hairline borders, restrained selection tints.
namespace theme_defaults {
inline constexpr Color background = Color::rgb(17, 17, 19);
inline constexpr Color surface = Color::rgb(24, 25, 27);
inline constexpr Color surface_alt = Color::rgb(33, 34, 37);
inline constexpr Color surface_hover = Color::rgb(39, 42, 45);
inline constexpr Color surface_pressed = Color::rgb(46, 49, 53);
inline constexpr Color surface_disabled = Color::rgba(33, 34, 37, 28);
inline constexpr Color text = Color::rgb(237, 238, 240);
inline constexpr Color text_emphasis = Color::rgb(246, 247, 248);
inline constexpr Color text_muted = Color::rgb(176, 180, 186);
inline constexpr Color text_disabled = Color::rgb(90, 97, 105);
inline constexpr Color border = Color::rgba(255, 255, 255, 38);
inline constexpr Color border_strong = Color::rgba(255, 255, 255, 77);
inline constexpr Color accent = Color::rgb(0, 144, 255);
inline constexpr Color accent_muted = Color::rgba(0, 144, 255, 28);
inline constexpr Color success = Color::rgb(48, 164, 108);
inline constexpr Color success_muted = Color::rgba(48, 164, 108, 28);
inline constexpr Color warning = Color::rgb(255, 197, 61);
inline constexpr Color danger = Color::rgb(229, 72, 77);
inline constexpr Color info = Color::rgb(0, 144, 255);
inline constexpr Color overlay = Color::rgba(0, 0, 0, 153);
inline constexpr Color shadow = Color::rgba(0, 0, 0, 102);
inline constexpr Color chip_fill = Color::rgba(33, 34, 37, 28);
} // namespace theme_defaults

struct Palette {
    Color background = theme_defaults::background;
    Color surface = theme_defaults::surface;
    Color surface_alt = theme_defaults::surface_alt;
    Color surface_hover = theme_defaults::surface_hover;
    Color surface_pressed = theme_defaults::surface_pressed;
    Color surface_disabled = theme_defaults::surface_disabled;

    Color text = theme_defaults::text;
    Color text_muted = theme_defaults::text_muted;
    Color text_emphasis = theme_defaults::text_emphasis;
    Color text_disabled = theme_defaults::text_disabled;
    Color text_on_accent = colors::white;

    Color border = theme_defaults::border;
    Color border_strong = theme_defaults::border_strong;
    Color accent = theme_defaults::accent;
    Color accent_muted = theme_defaults::accent_muted;

    Color success = theme_defaults::success;
    Color warning = theme_defaults::warning;
    Color danger = theme_defaults::danger;
    Color info = theme_defaults::info;

    Color overlay = theme_defaults::overlay;
    Color shadow = theme_defaults::shadow;
    Color transparent = colors::transparent;
};

struct StylePreset {
    f32 text_scale = 2.0f;
    f32 title_scale = 3.0f;
    f32 small_scale = 1.5f;

    f32 padding_x = 12.0f;
    f32 padding_y = 6.0f;
    f32 spacing = 8.0f;
    f32 row_height = 40.0f;
    f32 compact_row_height = 28.0f;

    f32 border_width = 1.0f;
    f32 radius = 0.0f;
    bool draw_surfaces = true;
    bool draw_outlines = true;
    bool draw_shadows = false;
    Vec2f shadow_offset{0.0f, 2.0f};
    f32 shadow_spread = 3.0f;
    i32 shadow_layers = 3;
    f32 icon_size = 24.0f;
    f32 scrollbar_thickness = 10.0f;
};

struct RadixConfig {
    int spacing = 2;
    int radius = 2;
    float scaling = 1.0f;
};

struct ThemeColorScales {
    ColorScale neutral{};
    ColorScale neutral_alpha{};
    ColorScale accent{};
    ColorScale accent_alpha{};
    ColorScale success{};
    ColorScale warning{};
    ColorScale danger{};
    ColorScale info{};
    ColorScale black_alpha{};
    ColorScale white_alpha{};
};

struct ThemeColorTokens {
    Color app_background = theme_defaults::background;
    Color surface_subtle = theme_defaults::surface_alt;
    Color surface_panel = theme_defaults::surface;
    Color surface_card = theme_defaults::surface_alt;
    Color surface_overlay = theme_defaults::overlay;
    Color interactive_normal = theme_defaults::surface_alt;
    Color interactive_hovered = theme_defaults::surface_hover;
    Color interactive_pressed = theme_defaults::surface_pressed;
    Color interactive_selected = theme_defaults::accent_muted;
    Color interactive_disabled = theme_defaults::surface_disabled;
    Color border = theme_defaults::border;
    Color border_strong = theme_defaults::border_strong;
    Color focus = theme_defaults::accent;
    Color text = theme_defaults::text;
    Color text_muted = theme_defaults::text_muted;
    Color text_emphasis = theme_defaults::text_emphasis;
    Color text_disabled = theme_defaults::text_disabled;
    Color solid_accent = theme_defaults::accent;
    Color solid_success = theme_defaults::success;
    Color solid_warning = theme_defaults::warning;
    Color solid_danger = theme_defaults::danger;
    Color solid_info = theme_defaults::info;
    Color text_on_solid = colors::white;
    Color shadow = theme_defaults::shadow;
};

struct ThemeSizeTokens {
    std::array<f32, 9> spacing{};
    std::array<f32, 6> radius{};
    f32 gap = 8.0f;
    f32 padding_x = 12.0f;
    f32 padding_y = 6.0f;
    f32 control_height = 40.0f;
    f32 compact_control_height = 28.0f;
    f32 icon_size = 24.0f;
    f32 scrollbar_thickness = 10.0f;
};

struct Theme {
    ThemeColorScales scales{};
    ThemeColorTokens colors{};
    ThemeSizeTokens sizes{};
    Palette palette{};
    StylePreset preset{};

    TextStyle body_text{};
    TextStyle title_text{};
    TextStyle small_text{};
    TextStyle muted_text{};
    TextStyle emphasis_text{};
    TextStyle danger_text{};
    TextStyle disabled_text{};

    WidgetStyle panel{};
    WidgetStyle button{};
    WidgetStyle danger_button{};
    WidgetStyle input{};
    WidgetStyle menu{};
    WidgetStyle tab{};
    WidgetStyle list_item{};
    WidgetStyle inspector{};
    WidgetStyle graph_node{};

    SurfaceStyle panel_surface{};
    SurfaceStyle overlay_surface{};
    SurfaceStyle button_surface{};
    SurfaceStyle input_surface{};
    SurfaceStyle menu_surface{};
    SurfaceStyle popup_surface{};
    SurfaceStyle tooltip_surface{};
    SurfaceStyle danger_surface{};
    SurfaceStyle selected_surface{};
    SurfaceStyle disabled_surface{};
    SurfaceStyle header_surface{};
    SurfaceStyle row_surface{};
    SurfaceStyle card_surface{};
    SurfaceStyle subtle_surface{};

    // Skinnable surfaces for progress bars, scrollbars, and meter segments.
    // Defaults look identical to the current flat-color draw; a SkinPack can replace them.
    SurfaceStyle progress_track_surface{};
    SurfaceStyle progress_fill_surface{};
    SurfaceStyle scrollbar_track_surface{};
    SurfaceStyle scrollbar_thumb_surface{};
    SurfaceStyle meter_fill_surface{};
    SurfaceStyle meter_empty_surface{};

    Color progress_fill = theme_defaults::success;
    Color progress_background = theme_defaults::surface_disabled;
    Color meter_fill = theme_defaults::success;
    Color meter_empty = theme_defaults::surface_disabled;
    Color prompt_chip_fill = theme_defaults::chip_fill;
    Color prompt_chip_border = theme_defaults::border;
    Color selection_fill = theme_defaults::success_muted;
    Color selection_border = theme_defaults::success;
    Color valid = theme_defaults::success;
    Color invalid = theme_defaults::danger;

    // Debug wireframe mode: when set, Context::surface() paints each component with its own
    // vivid, maximally-distinct color (like per-triangle colors in 3D debug views) so layout
    // composition, nesting, and overdraw are immediately visible. Text/sprites are left intact.
    bool debug_component_tint = false;

    // State-transition animation (B6): widgets eased through Context::resolve_animated over this
    // duration. 0 = instant (snap). Only applies when the app threads dt via begin(in, rend, dt).
    f32 transition_duration = 0.0f;
    Easing transition_easing = Easing::EaseOut;

    // Named widget-style variants (e.g. "gold_button", "stone_panel"): a game registers a
    // preset once (often via skinned_style) and references it by name without carrying the
    // WidgetStyle around. Empty by default — does not affect any standard widget.
    std::unordered_map<std::string, WidgetStyle> variants;
    void set_variant(std::string name, WidgetStyle style);   // register / overwrite
    const WidgetStyle* variant(std::string_view name) const; // nullptr if absent
};

// A per-state 9-slice skin for one interactive widget. Unset states fall back as noted,
// so the common case is just `{.normal = frame}`. Invalid skins are skipped (no-op).
struct WidgetSkin {
    UiNineSlice normal{};
    UiNineSlice hovered{};   // unset -> normal
    UiNineSlice pressed{};   // unset -> normal
    UiNineSlice focused{};   // unset -> normal
    UiNineSlice disabled{};  // unset -> normal
    UiNineSlice selected{};  // unset -> pressed, then normal
    Color tint = colors::white;
};

// Apply a skin in place. Invalid skin sprite = no-op (the flat fill stays), so partial
// application is always safe. The InteractiveSurfaceStyle overloads set every state.
void set_skin(SurfaceStyle& surface, const UiNineSlice& skin, Color tint = colors::white);
void set_skin(InteractiveSurfaceStyle& surface, const UiNineSlice& skin, Color tint = colors::white);
void set_skin(InteractiveSurfaceStyle& surface, const WidgetSkin& skin);

// Return a copy of `base` with the skin applied to all six interaction states — the
// one-liner for building a skinned preset (e.g. theme.button = skinned_style(theme.button,
// frame); or register it: theme.set_variant("gold_button", skinned_style(theme.button, gold))).
WidgetStyle skinned_style(WidgetStyle base, const UiNineSlice& skin, Color tint = colors::white);
WidgetStyle skinned_style(WidgetStyle base, const WidgetSkin& skin);

// A bundle of 9-slice skins applied to a theme's widget surfaces by apply_skin_pack().
// Any skin whose sprite is invalid is skipped, so partial packs are fine; the noted
// fallbacks let a small pack cover the whole theme. Textures are supplied by the caller
// (loaded or procedurally generated).
struct SkinPack {
    UiNineSlice panel{};           // panels / popups / overlays
    UiNineSlice button{};          // button base (states below fall back to this)
    UiNineSlice button_hovered{};
    UiNineSlice button_pressed{};
    UiNineSlice button_disabled{};
    UiNineSlice input{};           // text input / combo frame
    // --- expanded slots (each falls back to an existing field when unset) ---
    UiNineSlice card{};                  // distinct card; falls back to `panel`
    UiNineSlice danger_button{};         // falls back to `button`
    UiNineSlice danger_button_hovered{}; // falls back to danger_button -> button_hovered
    UiNineSlice danger_button_pressed{}; // falls back to danger_button -> button_pressed
    UiNineSlice menu{};                  // menu / popup / tooltip rows; falls back to `panel`
    UiNineSlice tab{};                   // falls back to `button`
    UiNineSlice list_item{};             // list / grid / tree rows; falls back to `menu` -> `panel`
    // --- indicator widgets ---
    UiNineSlice progress{};        // progress bar track + fill (tinted with accent); no fallback
    UiNineSlice scrollbar_track{}; // scrollbar track; falls back to `panel`
    UiNineSlice scrollbar_thumb{}; // scrollbar thumb; falls back to `button`
    UiNineSlice meter{};           // meter filled + empty segments; no fallback
    Color skin_tint = colors::white;
};

// Swap `pack`'s skins onto `theme`'s widget surfaces (panel/card, button + danger per state,
// input, menu, tab, list rows). Surfaces with a skin draw the 9-slice instead of their flat fill.
void apply_skin_pack(Theme& theme, const SkinPack& pack);

const Palette& default_palette();
const Palette& slate_palette();
const Palette& ember_palette();
const Palette& verdant_palette();
const Palette& parchment_palette();
const Palette& high_contrast_palette();
const Palette& palette(PalettePreset preset);

Palette make_palette(const ColorScale& neutral,
                     const ColorScale& accent,
                     const ColorScale& success_scale,
                     const ColorScale& warning_scale,
                     const ColorScale& danger_scale,
                     const ColorScale& info_scale);
Palette make_palette(const ColorScale& neutral, const ColorScale& accent);

const StylePreset& game_style_preset();
const StylePreset& editor_style_preset();
const StylePreset& compact_style_preset();
const StylePreset& style_preset(StylePresetKind kind);
StylePreset make_preset_from_radix(StylePresetKind kind, RadixConfig cfg = {});

// `emphasis_font` (optional) is used for title/emphasis text so headings get
// weight contrast, not just scale — pass system_ui_font_bold(...); invalid
// falls back to `font`.
Theme make_theme(const StylePreset& style, const Palette& palette, Font font = {}, Font emphasis_font = {});
// A theme from explicit colour tokens: every text style, widget and surface is
// derived from `tokens` (and indicators from `scales`), as make_theme() does
// from a palette. ThemeFile builds themes this way.
Theme make_theme_from_tokens(const StylePreset& style, ThemeColorScales scales, ThemeColorTokens tokens,
                             Font font = {}, Font emphasis_font = {});

// Replace the theme's text styles with system-UI fonts loaded at per-tier point
// sizes (body/muted at `base_pt`, small at 0.8x floored to 10pt, title/emphasis
// bold at 1.35x/1.0x), all at scale 1.0. TTF glyphs at fractional scales look
// resampled and blurry — loading each tier at its real size is what makes text
// read "professional". No-op when no system TTF is available (bitmap fallback
// keeps the preset scales).
void apply_system_fonts(Theme& theme, f32 base_pt);
Theme default_theme(Font font = {});
Theme game_theme(PalettePreset preset = PalettePreset::Default, Font font = {});
Theme editor_theme(PalettePreset preset = PalettePreset::Slate, Font font = {});
Theme shadcn_theme(Font font = {});
Theme radix_theme(RadixThemePreset preset, Font font = {});
Theme radix_theme(RadixThemePreset preset,
                  StylePresetKind kind,
                  RadixConfig cfg = {},
                  Font font = {});
Theme radix_theme(const ColorScale& neutral,
                  const ColorScale& accent,
                  StylePresetKind kind = StylePresetKind::Game,
                  RadixConfig cfg = {},
                  Font font = {});

// Frosted-glass surfaces (SurfaceFill::Glass) for the overlay layer (menus / popups /
// tooltips) and, when `frost_panels`, panels / cards too — the "acrylic" look. Real
// frost where render targets exist (including the software backend); degrades to a
// translucent `tint` scrim otherwise (so it is safe on every backend and in tests).
// Glass blurs the per-frame backdrop, so it reads best with content/gradient behind it.
struct GlassOptions {
    // Overlay layer (menus/popups/tooltips): lighter/more see-through. Unset -> surface_panel @ a~205.
    Color tint = colors::transparent;
    // Panels/cards: more opaque so body text stays legible over a vivid backdrop (audit G1).
    // Unset -> surface_panel @ a~224.
    Color panel_tint = colors::transparent;
    f32 blur = 7.0f;
    f32 highlight = 1.0f;
    bool frost_panels = true;
    // Subtle drop shadow on the theme's text tiers so glyphs read over frost (audit G1).
    bool text_shadow = true;
};
void apply_glass(Theme& theme, GlassOptions options = {});
// A shipped glass theme: a base theme (dark Default by default) with apply_glass() on top.
// Glass blurs the per-frame backdrop, so pair it with content behind the UI — e.g.
// draw_glass_backdrop() — or frost is invisible over a flat clear.
Theme glass_theme(Font font = {});
Theme glass_theme(PalettePreset base, Font font = {});

// Paint a backdrop suitable for a glass theme into `area`: a soft vertical gradient
// plus a few translucent accent blobs, so frosted surfaces have high-frequency content
// to refract. Call once before drawing the UI. This is the sensible default an app
// would otherwise hand-roll; colours are taken from `theme` so it tracks the palette.
void draw_glass_backdrop(Renderer2D& renderer, Rectf area, const Theme& theme);

Panel themed_panel(const Theme& theme);
Label themed_label(const Theme& theme, std::string text);
Button themed_button(const Theme& theme, std::string label);
Button themed_danger_button(const Theme& theme, std::string label);
ProgressBar themed_progress_bar(const Theme& theme, f32 value);
PromptLabel themed_prompt_label(const Theme& theme, std::string action, std::string text);
LayoutStyle panel_container(const Theme& theme, LayoutStyle style = {});
LayoutStyle card_container(const Theme& theme, LayoutStyle style = {});
LayoutStyle menu_container(const Theme& theme, LayoutStyle style = {});
void apply_theme(const Theme& theme, MenuList& w);
void apply_theme(const Theme& theme, MenuBar& w);
void apply_theme(const Theme& theme, TabBar& w);
void apply_theme(const Theme& theme, ScrollView& w);
void apply_theme(const Theme& theme, PropertyInspector& w);
void apply_theme(const Theme& theme, PropertyGrid& w);
void apply_theme(const Theme& theme, LogConsole& w);
void apply_theme(const Theme& theme, AssetBrowser& w);
void apply_theme(const Theme& theme, NodeGraph& w);
void apply_theme(const Theme& theme, ListView& w);
void apply_theme(const Theme& theme, Table& w);
void apply_theme(const Theme& theme, TreeView& w);
void apply_theme(const Theme& theme, ColorPicker& w);

} // namespace kin::ui2
