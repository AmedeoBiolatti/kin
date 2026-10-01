#include <kin/ui2/theme.hpp>
#include <kin/ui2/radix_colors.hpp>
#include <kin/renderer/renderer2d.hpp>

#include "style_internal.hpp"

#include <algorithm>
#include <utility>

namespace kin::ui2 {
namespace {

TextStyle text_style_from(Font font, f32 scale, Color color) {
    return {
        .font = font,
        .scale = scale,
        .color = color,
    };
}

// `elevated` is the single switch for whether a surface casts a shadow. Shadows are an
// elevation cue: only surfaces that float above the content plane (panels, cards, popups,
// menus, tooltips) get one. Inset/control surfaces (inputs, headers, rows, buttons) pass
// elevated=false so they read as flush, not floating. This keeps shadow intent declarative
// at construction instead of every widget remembering to disable it.
SurfaceStyle surface_from(const ThemeColorTokens& colors, const StylePreset& preset, Color fill, Color border,
                          bool elevated = false, bool force_shadow = false) {
    // Floating overlays (menus/popups/tooltips) carry a soft shadow in every
    // preset via force_shadow, even when the preset keeps panels flat (F5).
    const bool shadows = force_shadow || (elevated && preset.draw_shadows);
    const Vec2f shadow_offset = preset.shadow_offset.y > 0.0f ? preset.shadow_offset : Vec2f{0.0f, 4.0f};
    const f32 shadow_spread = preset.shadow_spread > 0.0f ? preset.shadow_spread : 2.0f;
    const i32 shadow_layers = preset.shadow_layers > 0 ? preset.shadow_layers : 3;
    return {
        .fill = fill,
        .border = border,
        .shadow = {
            .color = colors.shadow,
            .offset = shadow_offset,
            .spread = shadows ? shadow_spread : 0.0f,
            .radius = preset.radius,
            .layers = shadows ? shadow_layers : 1,
            .enabled = shadows,
        },
        .border_width = preset.border_width,
        .radius = preset.radius,
        .border_mode = preset.draw_outlines ? BorderMode::Inside : BorderMode::None,
        .draw_fill = preset.draw_surfaces,
    };
}

ColorScale flat_scale(Color color) {
    ColorScale scale{};
    scale.steps.fill(color);
    return scale;
}

ColorScale alpha_scale(Color color) {
    ColorScale scale{};
    constexpr u8 alpha[] = {8, 16, 28, 40, 56, 72, 96, 120, 150, 180, 210, 238};
    for (std::size_t i = 0; i < scale.steps.size(); ++i) {
        scale.steps[i] = Color::rgba(color.r, color.g, color.b, alpha[i]);
    }
    return scale;
}

ThemeColorScales scales_from_palette(const Palette& palette) {
    ThemeColorScales scales{};
    scales.neutral = {{
        palette.background,
        palette.surface,
        palette.surface_alt,
        palette.surface_hover,
        palette.surface_pressed,
        palette.border,
        palette.border_strong,
        palette.text_disabled,
        palette.text_muted,
        palette.text_muted,
        palette.text,
        palette.text_emphasis,
    }};
    scales.neutral_alpha = alpha_scale(palette.surface_alt);
    scales.accent = flat_scale(palette.accent);
    scales.accent_alpha = alpha_scale(palette.accent);
    scales.success = flat_scale(palette.success);
    scales.warning = flat_scale(palette.warning);
    scales.danger = flat_scale(palette.danger);
    scales.info = flat_scale(palette.info);
    scales.black_alpha = radix::black_alpha;
    scales.white_alpha = radix::white_alpha;
    return scales;
}

ThemeColorTokens tokens_from_scales(const ThemeColorScales& scales, Color text_on_solid = colors::white) {
    ThemeColorTokens tokens{};
    tokens.app_background = scales.neutral[0];
    tokens.surface_panel = scales.neutral[1];
    tokens.surface_subtle = scales.neutral[2];
    tokens.surface_card = scales.neutral[2];
    tokens.surface_overlay = scales.black_alpha[7];
    tokens.interactive_normal = scales.neutral[2];
    tokens.interactive_hovered = scales.neutral[3];
    tokens.interactive_pressed = scales.neutral[4];
    const Color bg = scales.neutral[0];
    const bool dark = (static_cast<i32>(bg.r) + bg.g + bg.b) < 3 * 128;
    tokens.interactive_selected = scales.accent_alpha[2];
    tokens.interactive_disabled = scales.neutral_alpha[2];
    // Border policy: alpha hairlines against the surface (white-alpha on dark
    // themes, black-alpha on light), never opaque. Separation comes from
    // surface steps; outlines are quiet by default.
    tokens.border = dark ? scales.white_alpha[2] : scales.black_alpha[2];        // ~15% hairline
    tokens.border_strong = dark ? scales.white_alpha[4] : scales.black_alpha[4]; // ~30%
    tokens.focus = scales.accent[8];
    tokens.text_disabled = scales.neutral[7];
    tokens.text_muted = scales.neutral[10];
    tokens.text = scales.neutral[11];
    // Emphasis is one tier above body text: pushed toward the contrast pole of
    // the theme. Weight contrast comes from the optional emphasis font.
    tokens.text_emphasis = mix(scales.neutral[11], dark ? colors::white : colors::black, 0.5f);
    tokens.solid_accent = scales.accent[8];
    tokens.solid_success = scales.success[8];
    tokens.solid_warning = scales.warning[8];
    tokens.solid_danger = scales.danger[8];
    tokens.solid_info = scales.info[8];
    tokens.text_on_solid = text_on_solid;
    tokens.shadow = scales.black_alpha[5];
    return tokens;
}

Palette palette_from_tokens(const ThemeColorTokens& tokens) {
    Palette palette;
    palette.background = tokens.app_background;
    palette.surface = tokens.surface_panel;
    palette.surface_alt = tokens.surface_card;
    palette.surface_hover = tokens.interactive_hovered;
    palette.surface_pressed = tokens.interactive_pressed;
    palette.surface_disabled = tokens.interactive_disabled;
    palette.text = tokens.text;
    palette.text_muted = tokens.text_muted;
    palette.text_emphasis = tokens.text_emphasis;
    palette.text_disabled = tokens.text_disabled;
    palette.text_on_accent = tokens.text_on_solid;
    palette.border = tokens.border;
    palette.border_strong = tokens.border_strong;
    palette.accent = tokens.solid_accent;
    palette.accent_muted = tokens.interactive_selected;
    palette.success = tokens.solid_success;
    palette.warning = tokens.solid_warning;
    palette.danger = tokens.solid_danger;
    palette.info = tokens.solid_info;
    palette.overlay = tokens.surface_overlay;
    palette.shadow = tokens.shadow;
    palette.transparent = colors::transparent;
    return palette;
}

ThemeSizeTokens size_tokens_from_preset(const StylePreset& preset) {
    ThemeSizeTokens sizes{};
    const f32 base = preset.spacing > 0.0f ? preset.spacing : 8.0f;
    for (std::size_t i = 0; i < sizes.spacing.size(); ++i) {
        sizes.spacing[i] = base * static_cast<f32>(i + 1) * 0.5f;
    }
    constexpr f32 radius_steps[] = {3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f};
    for (std::size_t i = 0; i < sizes.radius.size(); ++i) {
        sizes.radius[i] = radius_steps[i];
    }
    sizes.gap = preset.spacing;
    sizes.padding_x = preset.padding_x;
    sizes.padding_y = preset.padding_y;
    sizes.control_height = preset.row_height;
    sizes.compact_control_height = preset.compact_row_height;
    sizes.icon_size = preset.icon_size;
    sizes.scrollbar_thickness = preset.scrollbar_thickness;
    return sizes;
}

struct RadixThemeRecipe {
    const ColorScale* neutral = nullptr;
    const ColorScale* neutral_alpha = nullptr;
    const ColorScale* accent = nullptr;
    const ColorScale* accent_alpha = nullptr;
    StylePresetKind kind = StylePresetKind::Game;
    RadixConfig config{};
};

RadixThemeRecipe radix_recipe(RadixThemePreset preset) {
    switch (preset) {
    case RadixThemePreset::Shadcn:
        return {.neutral = &radix::slate_dark, .neutral_alpha = &radix::slate_dark_alpha, .accent = &radix::indigo_dark, .accent_alpha = &radix::indigo_dark_alpha, .kind = StylePresetKind::Game, .config = {.spacing = 2, .radius = 2}};
    case RadixThemePreset::Midnight:
        return {.neutral = &radix::mauve_dark, .neutral_alpha = &radix::mauve_dark_alpha, .accent = &radix::iris_dark, .accent_alpha = &radix::iris_dark_alpha, .kind = StylePresetKind::Game, .config = {.spacing = 3, .radius = 3}};
    case RadixThemePreset::Aurora:
        return {.neutral = &radix::sage_dark, .neutral_alpha = &radix::sage_dark_alpha, .accent = &radix::cyan_dark, .accent_alpha = &radix::cyan_dark_alpha, .kind = StylePresetKind::Game, .config = {.spacing = 3, .radius = 4}};
    case RadixThemePreset::Candy:
        return {.neutral = &radix::mauve_dark, .neutral_alpha = &radix::mauve_dark_alpha, .accent = &radix::pink_dark, .accent_alpha = &radix::pink_dark_alpha, .kind = StylePresetKind::Game, .config = {.spacing = 3, .radius = 5}};
    case RadixThemePreset::Forest:
        return {.neutral = &radix::olive_dark, .neutral_alpha = &radix::olive_dark_alpha, .accent = &radix::grass_dark, .accent_alpha = &radix::grass_dark_alpha, .kind = StylePresetKind::Editor, .config = {.spacing = 2, .radius = 2}};
    case RadixThemePreset::Ember:
        return {.neutral = &radix::sand_dark, .neutral_alpha = &radix::sand_dark_alpha, .accent = &radix::orange_dark, .accent_alpha = &radix::orange_dark_alpha, .kind = StylePresetKind::Game, .config = {.spacing = 3, .radius = 3}};
    case RadixThemePreset::Terminal:
        return {.neutral = &radix::gray_dark, .neutral_alpha = &radix::gray_dark_alpha, .accent = &radix::lime_dark, .accent_alpha = &radix::lime_dark_alpha, .kind = StylePresetKind::Compact, .config = {.spacing = 2, .radius = 1}};
    case RadixThemePreset::Sandstone:
        return {.neutral = &radix::sand_dark, .neutral_alpha = &radix::sand_dark_alpha, .accent = &radix::gold_dark, .accent_alpha = &radix::gold_dark_alpha, .kind = StylePresetKind::Editor, .config = {.spacing = 2, .radius = 2}};
    }
    return {.neutral = &radix::slate_dark, .neutral_alpha = &radix::slate_dark_alpha, .accent = &radix::indigo_dark, .accent_alpha = &radix::indigo_dark_alpha, .kind = StylePresetKind::Game, .config = {.spacing = 2, .radius = 2}};
}

WidgetStyle widget_style_from(const ThemeColorTokens& colors, const StylePreset& preset) {
    SurfaceStyle base = surface_from(colors, preset, colors.interactive_normal, colors.border);
    return {
        .surface = detail::interactive_surface_from(base,
                                                    {
                                                        .hovered = colors.interactive_hovered,
                                                        .pressed = colors.interactive_pressed,
                                                        .focused = colors.focus,
                                                        .disabled = colors.interactive_disabled,
                                                        .selected = colors.interactive_selected,
                                                    }),
        .accent = colors.solid_accent,
        .track = colors.interactive_disabled,
        .padding = {preset.padding_x, preset.padding_y, preset.padding_x, preset.padding_y},
        .min_height = preset.compact_row_height,
    };
}

SurfaceStyle flat_role_surface(const ThemeColorTokens& colors,
                               const StylePreset& preset,
                               Color fill,
                               Color border,
                               bool draw_fill,
                               bool draw_border) {
    SurfaceStyle surface = surface_from(colors, preset, fill, border);
    surface.shadow.enabled = false;
    surface.draw_fill = draw_fill && preset.draw_surfaces;
    surface.border_mode = draw_border && preset.draw_outlines ? BorderMode::Inside : BorderMode::None;
    return surface;
}

WidgetStyle item_style_from(const ThemeColorTokens& colors, const StylePreset& preset) {
    WidgetStyle style{};
    style.surface.normal = flat_role_surface(colors, preset, colors::transparent, colors::transparent, false, false);
    style.surface.hovered = flat_role_surface(colors, preset, colors.surface_subtle, colors::transparent, true, false);
    style.surface.pressed = flat_role_surface(colors, preset, colors.interactive_pressed, colors::transparent, true, false);
    style.surface.focused = flat_role_surface(colors, preset, colors::transparent, colors.focus, false, true);
    style.surface.disabled = flat_role_surface(colors, preset, colors.text_disabled, colors::transparent, false, false);
    style.surface.selected = flat_role_surface(colors, preset, colors.interactive_selected, colors::transparent, true, false);
    style.accent = colors.solid_accent;
    style.track = colors::transparent;
    style.padding = {preset.padding_x, preset.padding_y, preset.padding_x, preset.padding_y};
    style.min_height = preset.compact_row_height;
    return style;
}

WidgetStyle tab_style_from(const ThemeColorTokens& colors, const StylePreset& preset) {
    WidgetStyle style = item_style_from(colors, preset);
    style.surface.selected = flat_role_surface(colors, preset, colors.surface_panel, colors.border, true, true);
    style.track = colors.interactive_disabled;
    return style;
}

} // namespace

// All preset palettes are derived from Radix scale pairs (neutral + accent), so
// they inherit the professional token relationships: real surface steps, alpha
// hairline borders, restrained selection tints. Raw `Palette{}` keeps the
// legacy `theme_defaults` constants purely as struct fallbacks.
const Palette& default_palette() {
    static const Palette value = make_palette(radix::slate_dark, radix::blue_dark);
    return value;
}

const Palette& slate_palette() {
    static const Palette value = make_palette(radix::slate_dark, radix::cyan_dark);
    return value;
}

const Palette& ember_palette() {
    static const Palette value = make_palette(radix::sand_dark, radix::orange_dark);
    return value;
}

const Palette& verdant_palette() {
    static const Palette value = make_palette(radix::sage_dark, radix::grass_dark);
    return value;
}

const Palette& parchment_palette() {
    static const Palette value = [] {
        Palette palette = make_palette(radix::sand_light,
                                       radix::gold_light,
                                       radix::green_light,
                                       radix::amber_light,
                                       radix::red_light,
                                       radix::blue_light);
        palette.text_on_accent = colors::white;
        return palette;
    }();
    return value;
}

const Palette& high_contrast_palette() {
    static const Palette value = [] {
        Palette palette = make_palette(radix::gray_dark, radix::yellow_dark);
        // Accessibility preset: push text and borders to the contrast poles.
        palette.background = colors::black;
        palette.surface = colors::black;
        palette.text = Color::rgb(235, 235, 235);
        palette.text_emphasis = colors::white;
        palette.text_muted = Color::rgb(178, 178, 178);
        palette.border = Color::rgba(255, 255, 255, 140);
        palette.border_strong = colors::white;
        palette.text_on_accent = colors::black;
        palette.overlay = Color::rgba(0, 0, 0, 200);
        return palette;
    }();
    return value;
}

const Palette& palette(PalettePreset preset) {
    switch (preset) {
    case PalettePreset::Default: return default_palette();
    case PalettePreset::Slate: return slate_palette();
    case PalettePreset::Ember: return ember_palette();
    case PalettePreset::Verdant: return verdant_palette();
    case PalettePreset::Parchment: return parchment_palette();
    case PalettePreset::HighContrast: return high_contrast_palette();
    }
    return default_palette();
}

Palette make_palette(const ColorScale& neutral,
                     const ColorScale& accent,
                     const ColorScale& success_scale,
                     const ColorScale& warning_scale,
                     const ColorScale& danger_scale,
                     const ColorScale& info_scale) {
    ThemeColorScales scales{};
    scales.neutral = neutral;
    scales.neutral_alpha = alpha_scale(neutral[2]);
    scales.accent = accent;
    scales.accent_alpha = alpha_scale(accent[8]);
    scales.success = success_scale;
    scales.warning = warning_scale;
    scales.danger = danger_scale;
    scales.info = info_scale;
    scales.black_alpha = radix::black_alpha;
    scales.white_alpha = radix::white_alpha;
    return palette_from_tokens(tokens_from_scales(scales));
}

Palette make_palette(const ColorScale& neutral, const ColorScale& accent) {
    return make_palette(neutral, accent, radix::green_dark, radix::amber_dark, radix::red_dark, radix::blue_dark);
}

const StylePreset& game_style_preset() {
    static const StylePreset value = [] {
        StylePreset preset{};
        preset.border_width = 1.0f; // hairline; 2px is reserved for focus treatments
        preset.radius = 6.0f;
        preset.draw_shadows = true;
        // Offset must dominate spread, otherwise the outer shadow layers bleed above/left of
        // the surface and read as an omnidirectional glow instead of a directional drop shadow.
        preset.shadow_offset = {0.0f, 4.0f};
        preset.shadow_spread = 2.0f;
        return preset;
    }();
    return value;
}

const StylePreset& editor_style_preset() {
    static const StylePreset value = [] {
        StylePreset preset{};
        preset.text_scale = 1.5f;
        preset.title_scale = 2.0f;
        preset.small_scale = 1.0f;
        preset.padding_x = 8.0f;
        preset.padding_y = 4.0f;
        preset.spacing = 6.0f;
        preset.row_height = 30.0f;
        preset.compact_row_height = 24.0f;
        preset.border_width = 1.0f;
        // Editor chrome (VS Code / Linear) is crisp: square corners, flat panels.
        // Elevation comes from floating overlays only (menus/popups/tooltips get a
        // forced shadow in make_theme_from_tokens, in every preset — see F5).
        preset.radius = 0.0f;
        preset.draw_shadows = false;
        preset.shadow_offset = {0.0f, 3.0f};
        preset.shadow_spread = 1.0f;
        preset.shadow_layers = 3;
        preset.icon_size = 18.0f;
        return preset;
    }();
    return value;
}

const StylePreset& compact_style_preset() {
    static const StylePreset value = [] {
        StylePreset preset = editor_style_preset();
        preset.padding_x = 6.0f;
        preset.padding_y = 3.0f;
        preset.spacing = 4.0f;
        preset.row_height = 24.0f;
        preset.compact_row_height = 20.0f;
        preset.draw_surfaces = true;
        preset.draw_outlines = false;
        preset.draw_shadows = false;
        preset.radius = 0.0f;
        return preset;
    }();
    return value;
}

const StylePreset& style_preset(StylePresetKind kind) {
    switch (kind) {
    case StylePresetKind::Game: return game_style_preset();
    case StylePresetKind::Editor: return editor_style_preset();
    case StylePresetKind::Compact: return compact_style_preset();
    }
    return game_style_preset();
}

StylePreset make_preset_from_radix(StylePresetKind kind, RadixConfig cfg) {
    constexpr float space[] = {4.0f, 8.0f, 12.0f, 16.0f, 24.0f, 32.0f, 40.0f, 48.0f, 64.0f};
    constexpr float radii[] = {3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f};
    StylePreset preset = style_preset(kind);
    const int spacing = std::clamp(cfg.spacing, 1, 9);
    const int radius = std::clamp(cfg.radius, 1, 6);
    const float scale = std::max(0.0f, cfg.scaling);
    const float sp = space[spacing - 1] * scale;
    preset.padding_x = sp * 1.5f;
    preset.padding_y = sp * 0.75f;
    preset.spacing = sp;
    preset.row_height = sp * 4.0f;
    preset.compact_row_height = sp * 3.0f;
    preset.radius = radii[radius - 1];
    preset.icon_size = sp * 2.5f;
    preset.scrollbar_thickness = sp * 1.25f;
    return preset;
}

Theme make_theme_from_tokens(const StylePreset& style, ThemeColorScales scales, ThemeColorTokens tokens, Font font, Font emphasis_font) {
    Theme theme;
    theme.scales = scales;
    theme.colors = tokens;
    theme.sizes = size_tokens_from_preset(style);
    theme.palette = palette_from_tokens(tokens);
    theme.preset = style;
    // Accent restraint (P1): generic indicators (progress, meter, slider fill)
    // use the muted accent step, not saturated success-green. Success-green is
    // reserved for actual status (theme.valid). The scale's step[7] is the
    // "solid-muted" tone for real Radix scales; flat palettes collapse it to the
    // single accent color, which is acceptable.
    const Color indicator_fill = scales.accent[7];
    const Color indicator_muted = tokens.interactive_selected;
    // Restrained scrollbar thumb: a neutral that never competes with accent.
    const Color scrollbar_thumb = scales.neutral[6];

    const Font heading_font = emphasis_font ? emphasis_font : font;
    theme.body_text = text_style_from(font, style.text_scale, tokens.text);
    theme.title_text = text_style_from(heading_font, style.title_scale, tokens.text_emphasis);
    theme.small_text = text_style_from(font, style.small_scale, tokens.text_muted);
    theme.muted_text = text_style_from(font, style.text_scale, tokens.text_muted);
    theme.emphasis_text = text_style_from(heading_font, style.text_scale, tokens.text_emphasis);
    theme.danger_text = text_style_from(font, style.text_scale, tokens.solid_danger);
    theme.disabled_text = text_style_from(font, style.text_scale, tokens.text_disabled);

    theme.panel = widget_style_from(tokens, style);
    theme.panel.surface.normal.fill = tokens.surface_panel;
    theme.panel.track = tokens.interactive_disabled;

    theme.button = widget_style_from(tokens, style);

    theme.danger_button = theme.button;
    theme.danger_button.surface.normal.border = tokens.solid_danger;
    theme.danger_button.surface.focused.border = tokens.solid_danger;
    theme.danger_button.surface.selected.border = tokens.solid_danger;
    theme.danger_button.accent = tokens.solid_danger;

    theme.input = theme.button;
    theme.input.surface.normal.fill = tokens.surface_panel;
    theme.input.track = tokens.interactive_disabled;

    theme.menu = item_style_from(tokens, style);
    theme.tab = tab_style_from(tokens, style);
    theme.list_item = item_style_from(tokens, style);
    theme.inspector = theme.input;
    theme.graph_node = theme.button;

    // Elevated surfaces (shadow) float above the content plane: panels, cards, and the
    // overlay layers (popups/menus/tooltips). Everything inset/flush passes elevated=false.
    theme.panel_surface = surface_from(tokens, style, tokens.surface_panel, tokens.border, /*elevated=*/true);
    theme.overlay_surface = surface_from(tokens, style, tokens.surface_overlay, colors::transparent);
    theme.button_surface = surface_from(tokens, style, tokens.interactive_normal, tokens.border);
    theme.input_surface = surface_from(tokens, style, tokens.surface_panel, tokens.border);
    theme.menu_surface = surface_from(tokens, style, tokens.surface_panel, tokens.border, /*elevated=*/true, /*force_shadow=*/true);
    theme.popup_surface = surface_from(tokens, style, tokens.surface_panel, tokens.border_strong, /*elevated=*/true, /*force_shadow=*/true);
    theme.tooltip_surface = surface_from(tokens, style, tokens.surface_panel, tokens.border_strong, /*elevated=*/true, /*force_shadow=*/true);
    theme.danger_surface = surface_from(tokens, style, tokens.interactive_normal, tokens.solid_danger);
    theme.selected_surface = surface_from(tokens, style, tokens.interactive_selected, tokens.focus);
    theme.disabled_surface = surface_from(tokens, style, tokens.interactive_disabled, colors::transparent);
    theme.header_surface = surface_from(tokens, style, tokens.surface_card, tokens.border);
    theme.row_surface = surface_from(tokens, style, tokens.surface_card, tokens.border);
    theme.card_surface = surface_from(tokens, style, tokens.surface_card, tokens.border, /*elevated=*/true);
    theme.subtle_surface = surface_from(tokens, style, tokens.interactive_disabled, colors::transparent);

    theme.subtle_surface.border_mode = BorderMode::None;
    theme.disabled_surface.border_mode = BorderMode::None;

    // Skinnable indicator surfaces — defaults match the existing flat-color look exactly.
    theme.progress_track_surface = surface_from(tokens, style, tokens.interactive_disabled, colors::transparent);
    theme.progress_track_surface.border_mode = BorderMode::None;
    theme.progress_fill_surface = surface_from(tokens, style, indicator_fill, colors::transparent);
    theme.progress_fill_surface.border_mode = BorderMode::None;
    theme.scrollbar_track_surface = surface_from(tokens, style, tokens.interactive_disabled, colors::transparent);
    theme.scrollbar_track_surface.border_mode = BorderMode::None;
    theme.scrollbar_thumb_surface = surface_from(tokens, style, scrollbar_thumb, colors::transparent);
    theme.scrollbar_thumb_surface.border_mode = BorderMode::None;
    theme.meter_fill_surface = surface_from(tokens, style, indicator_fill, colors::transparent);
    theme.meter_fill_surface.border_mode = BorderMode::None;
    theme.meter_empty_surface = surface_from(tokens, style, tokens.interactive_disabled, colors::transparent);
    theme.meter_empty_surface.border_mode = BorderMode::None;

    theme.progress_fill = indicator_fill;
    theme.progress_background = tokens.interactive_disabled;
    theme.meter_fill = indicator_fill;
    theme.meter_empty = tokens.interactive_disabled;
    theme.prompt_chip_fill = tokens.interactive_disabled;
    theme.prompt_chip_border = tokens.border;
    theme.selection_fill = indicator_muted;
    theme.selection_border = indicator_fill;
    theme.valid = tokens.solid_success;
    theme.invalid = tokens.solid_danger;

    // B6: gentle state-transition easing by default (only active when the app threads
    // dt via Context::begin(in, rend, dt); dt==0 callers/tests stay instant).
    theme.transition_duration = 0.12f;
    theme.transition_easing = Easing::EaseOut;
    return theme;
}

Theme make_theme(const StylePreset& style, const Palette& palette_value, Font font, Font emphasis_font) {
    ThemeColorScales scales = scales_from_palette(palette_value);
    ThemeColorTokens tokens = tokens_from_scales(scales, palette_value.text_on_accent);
    tokens.app_background = palette_value.background;
    tokens.surface_panel = palette_value.surface;
    tokens.surface_subtle = palette_value.surface_disabled;
    tokens.surface_card = palette_value.surface_alt;
    tokens.surface_overlay = palette_value.overlay;
    tokens.interactive_normal = palette_value.surface_alt;
    tokens.interactive_hovered = palette_value.surface_hover;
    tokens.interactive_pressed = palette_value.surface_pressed;
    tokens.interactive_selected = palette_value.accent_muted;
    tokens.interactive_disabled = palette_value.surface_disabled;
    tokens.border = palette_value.border;
    tokens.border_strong = palette_value.border_strong;
    tokens.focus = palette_value.accent;
    tokens.text = palette_value.text;
    tokens.text_muted = palette_value.text_muted;
    tokens.text_emphasis = palette_value.text_emphasis;
    tokens.text_disabled = palette_value.text_disabled;
    tokens.solid_accent = palette_value.accent;
    tokens.solid_success = palette_value.success;
    tokens.solid_warning = palette_value.warning;
    tokens.solid_danger = palette_value.danger;
    tokens.solid_info = palette_value.info;
    tokens.text_on_solid = palette_value.text_on_accent;
    tokens.shadow = palette_value.shadow;
    Theme theme = make_theme_from_tokens(style, scales, tokens, font, emphasis_font);
    theme.palette.transparent = palette_value.transparent;
    return theme;
}

void apply_system_fonts(Theme& theme, f32 base_pt) {
    if (!system_ui_font_available()) {
        return; // bitmap fallback: keep the preset's scale-based styles
    }
    const f32 small_pt = std::max(10.0f, base_pt * 0.8f);
    const f32 title_pt = base_pt * 1.35f;
    const auto set = [](TextStyle& style, Font font) {
        style.font = font;
        style.scale = 1.0f;
    };
    set(theme.body_text, system_ui_font(base_pt));
    set(theme.muted_text, system_ui_font(base_pt));
    set(theme.danger_text, system_ui_font(base_pt));
    set(theme.disabled_text, system_ui_font(base_pt));
    set(theme.small_text, system_ui_font(small_pt));
    set(theme.emphasis_text, system_ui_font_bold(base_pt));
    set(theme.title_text, system_ui_font_bold(title_pt));
}

Theme default_theme(Font font) {
    return make_theme(game_style_preset(), default_palette(), font);
}

Theme game_theme(PalettePreset preset, Font font) {
    return make_theme(game_style_preset(), palette(preset), font);
}

Theme editor_theme(PalettePreset preset, Font font) {
    return make_theme(editor_style_preset(), palette(preset), font);
}

Theme shadcn_theme(Font font) {
    return radix_theme(RadixThemePreset::Shadcn, font);
}

Theme radix_theme(RadixThemePreset preset, Font font) {
    const RadixThemeRecipe recipe = radix_recipe(preset);
    return radix_theme(preset, recipe.kind, recipe.config, font);
}

Theme radix_theme(RadixThemePreset preset, StylePresetKind kind, RadixConfig cfg, Font font) {
    const RadixThemeRecipe recipe = radix_recipe(preset);
    ThemeColorScales scales{};
    scales.neutral = *recipe.neutral;
    scales.neutral_alpha = recipe.neutral_alpha ? *recipe.neutral_alpha : alpha_scale((*recipe.neutral)[2]);
    scales.accent = *recipe.accent;
    scales.accent_alpha = recipe.accent_alpha ? *recipe.accent_alpha : alpha_scale((*recipe.accent)[8]);
    scales.success = radix::green_dark;
    scales.warning = radix::amber_dark;
    scales.danger = radix::red_dark;
    scales.info = radix::blue_dark;
    scales.black_alpha = radix::black_alpha;
    scales.white_alpha = radix::white_alpha;
    return make_theme_from_tokens(make_preset_from_radix(kind, cfg), scales, tokens_from_scales(scales), font);
}

Theme radix_theme(const ColorScale& neutral, const ColorScale& accent, StylePresetKind kind, RadixConfig cfg, Font font) {
    ThemeColorScales scales{};
    scales.neutral = neutral;
    scales.neutral_alpha = alpha_scale(neutral[2]);
    scales.accent = accent;
    scales.accent_alpha = alpha_scale(accent[8]);
    scales.success = radix::green_dark;
    scales.warning = radix::amber_dark;
    scales.danger = radix::red_dark;
    scales.info = radix::blue_dark;
    scales.black_alpha = radix::black_alpha;
    scales.white_alpha = radix::white_alpha;
    return make_theme_from_tokens(make_preset_from_radix(kind, cfg), scales, tokens_from_scales(scales), font);
}

namespace {

// Convert an already-drawn surface into a frosted-glass one in place, preserving its
// radius/border/shadow. draw_fill is forced on so the glass branch in Context::surface()
// actually paints (an inset/flush surface may have had draw_fill off).
void set_glass(SurfaceStyle& s, Color tint, f32 blur, f32 highlight) {
    s.fill_kind = SurfaceFill::Glass;
    s.glass_tint = tint;
    s.glass_blur = blur;
    s.glass_highlight = highlight;
    s.draw_fill = true;
}

} // namespace

void apply_glass(Theme& theme, GlassOptions options) {
    const Color base = theme.colors.surface_panel;
    // Overlay tint stays lighter (menus/popups read see-through); panel tint is more
    // opaque so body text over a vivid backdrop keeps contrast (audit G1).
    Color overlay_tint = options.tint;
    if (overlay_tint == colors::transparent) {
        overlay_tint = Color::rgba(base.r, base.g, base.b, 205);
    }
    Color panel_tint = options.panel_tint;
    if (panel_tint == colors::transparent) {
        panel_tint = Color::rgba(base.r, base.g, base.b, 224);
    }
    // Overlay layer always frosts (the spec's intended glass scope); the modal scrim
    // (overlay_surface) stays a plain dim — frosting it would weaken the dimming.
    set_glass(theme.menu_surface, overlay_tint, options.blur, options.highlight);
    set_glass(theme.popup_surface, overlay_tint, options.blur, options.highlight);
    set_glass(theme.tooltip_surface, overlay_tint, options.blur, options.highlight);
    if (options.frost_panels) {
        set_glass(theme.panel_surface, panel_tint, options.blur, options.highlight);
        set_glass(theme.card_surface, panel_tint, options.blur, options.highlight);
    }
    if (options.text_shadow) {
        // A 1px dark drop shadow lifts glyphs off the frosted backdrop without the
        // heavier look of a full outline. Applied to the readable text tiers only.
        const Color shadow = Color::rgba(0, 0, 0, 115);
        const Vec2f offset{0.0f, 1.0f};
        for (TextStyle* t : {&theme.body_text, &theme.title_text, &theme.small_text,
                             &theme.muted_text, &theme.emphasis_text, &theme.danger_text}) {
            t->shadow_color = shadow;
            t->shadow_offset = offset;
        }
    }
}

Theme glass_theme(Font font) {
    return glass_theme(PalettePreset::Default, font);
}

Theme glass_theme(PalettePreset base, Font font) {
    Theme theme = game_theme(base, font);
    apply_glass(theme);
    return theme;
}

void draw_glass_backdrop(Renderer2D& renderer, Rectf area, const Theme& theme) {
    if (area.w <= 0.0f || area.h <= 0.0f) {
        return;
    }
    // Soft vertical lift from the app background, then translucent accent blobs so the
    // frosted surfaces above have high-frequency content to blur.
    const Color bg = theme.colors.app_background;
    const Color top = mix(bg, theme.colors.surface_card, 0.5f);
    renderer.fill_gradient_rect(area, Gradient{top, bg, GradientDirection::Vertical});
    const auto blob = [&](f32 fx, f32 fy, f32 fw, f32 fh, Color c, int a) {
        const Rectf b{area.x + area.w * fx, area.y + area.h * fy, area.w * fw, area.h * fh};
        renderer.fill_rounded_rect(b, std::min(b.w, b.h) * 0.5f,
                                   Color::rgba(c.r, c.g, c.b, static_cast<u8>(a)));
    };
    blob(0.02f, 0.10f, 0.52f, 0.34f, theme.colors.solid_accent, 90);
    blob(0.52f, 0.40f, 0.46f, 0.50f, theme.colors.solid_success, 70);
    blob(0.24f, 0.56f, 0.44f, 0.42f, theme.colors.solid_warning, 56);
    blob(0.60f, 0.04f, 0.30f, 0.30f, theme.colors.solid_danger, 52);
}

Panel themed_panel(const Theme& theme) {
    return {
        .color = theme.palette.surface,
        .border = theme.palette.border,
    };
}

Label themed_label(const Theme& theme, std::string text) {
    return {
        .text = std::move(text),
        .text_style = theme.body_text,
    };
}

Button themed_button(const Theme& theme, std::string label) {
    return {
        .label = std::move(label),
        .text_style = theme.body_text,
        .style = theme.button,
    };
}

Button themed_danger_button(const Theme& theme, std::string label) {
    return {
        .label = std::move(label),
        .text_style = theme.danger_text,
        .style = theme.danger_button,
    };
}

ProgressBar themed_progress_bar(const Theme& theme, f32 value) {
    return {
        .value = value,
        .fill = theme.progress_fill,
        .background = theme.progress_background,
        .border = theme.palette.transparent,
    };
}

PromptLabel themed_prompt_label(const Theme& theme, std::string action, std::string text) {
    // Derive paddings from the theme gap so prompt chips share the spacing rhythm and
    // scale with the theme (at the default gap of 8 this reproduces the struct defaults).
    const f32 g = theme.sizes.gap;
    return {
        .action = std::move(action),
        .text = std::move(text),
        .prompt_style = theme.emphasis_text,
        .text_style = theme.body_text,
        .padding = {g, g * 0.5f, g, g * 0.5f},
        .chip_padding = {g, g * 0.5f, g, g * 0.5f},
        .gap = g,
        .chip_fill = theme.prompt_chip_fill,
        .chip_border = theme.prompt_chip_border,
    };
}

namespace {

// Containers default their child spacing to the theme gap token; a caller-set
// spacing (non-zero) always wins.
void apply_container_gap(const Theme& theme, LayoutStyle& style) {
    if (style.spacing == 0.0f) {
        style.spacing = theme.sizes.gap;
    }
}

} // namespace

LayoutStyle panel_container(const Theme& theme, LayoutStyle style) {
    style.surface = theme.panel_surface;
    style.draw_surface = true;
    apply_container_gap(theme, style);
    return style;
}

LayoutStyle card_container(const Theme& theme, LayoutStyle style) {
    style.surface = theme.card_surface;
    style.draw_surface = true;
    apply_container_gap(theme, style);
    return style;
}

LayoutStyle menu_container(const Theme& theme, LayoutStyle style) {
    style.surface = theme.menu_surface;
    style.draw_surface = true;
    apply_container_gap(theme, style);
    return style;
}

namespace {

// Spacing steps derived from the theme gap token: at the default gap (8) these
// reproduce the widgets' historical hardcoded values (2 and 4).
f32 dense_spacing(const Theme& theme) { return theme.sizes.gap * 0.25f; }
f32 loose_spacing(const Theme& theme) { return theme.sizes.gap * 0.5f; }

} // namespace

// apply_theme wires ThemeSizeTokens into widget size fields. Sizes only —
// color styling is resolved against the context theme at draw time, and baking
// it here would freeze widgets against later theme switches.
void apply_theme(const Theme& theme, MenuList& w) {
    w.row_height = theme.sizes.compact_control_height;
    w.row_spacing = dense_spacing(theme);
}

void apply_theme(const Theme& theme, MenuBar& w) {
    w.item_gap = dense_spacing(theme);
}

void apply_theme(const Theme& theme, TabBar& w) {
    w.tab_height = theme.sizes.compact_control_height;
    w.gap = dense_spacing(theme);
}

void apply_theme(const Theme& theme, ScrollView& w) {
    w.scrollbar_thickness = theme.sizes.scrollbar_thickness;
}

void apply_theme(const Theme& theme, PropertyInspector& w) {
    w.row_height = theme.sizes.compact_control_height;
    w.section_height = theme.sizes.compact_control_height;
    w.control_height = theme.sizes.compact_control_height - loose_spacing(theme);
    w.row_spacing = loose_spacing(theme);
}

void apply_theme(const Theme& theme, PropertyGrid& w) {
    w.row_height = theme.sizes.compact_control_height;
    w.control_height = theme.sizes.compact_control_height - loose_spacing(theme);
    w.row_spacing = loose_spacing(theme);
}

void apply_theme(const Theme& theme, LogConsole& w) {
    w.row_height = theme.sizes.compact_control_height;
}

void apply_theme(const Theme& theme, AssetBrowser& w) {
    w.row_height = theme.sizes.compact_control_height;
    w.spacing = {loose_spacing(theme), loose_spacing(theme)};
}

void apply_theme(const Theme& theme, NodeGraph& w) {
    w.header_height = theme.sizes.compact_control_height;
    w.port_spacing = theme.sizes.control_height * 0.55f;
}

void apply_theme(const Theme& theme, ListView& w) {
    w.row_height = theme.sizes.compact_control_height;
    w.row_spacing = dense_spacing(theme);
}

void apply_theme(const Theme& theme, Table& w) {
    w.header_height = theme.sizes.compact_control_height;
    w.row_height = theme.sizes.compact_control_height;
    w.cell_padding_x = theme.sizes.padding_x * 0.5f;
}

void apply_theme(const Theme& theme, TreeView& w) {
    w.row_height = theme.sizes.compact_control_height;
    w.row_spacing = dense_spacing(theme);
}

void apply_theme(const Theme& theme, ColorPicker& w) {
    w.row_height = std::max(16.0f, theme.sizes.compact_control_height * 0.75f);
    w.row_spacing = loose_spacing(theme);
}

namespace {

const UiNineSlice& skin_or(const UiNineSlice& skin, const UiNineSlice& fallback) {
    return skin.sprite.valid() ? skin : fallback;
}

} // namespace

void set_skin(SurfaceStyle& surface, const UiNineSlice& skin, Color tint) {
    if (!skin.sprite.valid()) {
        return; // partial application is fine — leave the flat fill in place
    }
    surface.use_skin = true;
    surface.skin = skin;
    surface.skin_tint = tint;
}

void set_skin(InteractiveSurfaceStyle& surface, const UiNineSlice& skin, Color tint) {
    set_skin(surface.normal, skin, tint);
    set_skin(surface.hovered, skin, tint);
    set_skin(surface.pressed, skin, tint);
    set_skin(surface.focused, skin, tint);
    set_skin(surface.disabled, skin, tint);
    set_skin(surface.selected, skin, tint);
}

void set_skin(InteractiveSurfaceStyle& surface, const WidgetSkin& skin) {
    set_skin(surface.normal, skin.normal, skin.tint);
    set_skin(surface.hovered, skin_or(skin.hovered, skin.normal), skin.tint);
    set_skin(surface.pressed, skin_or(skin.pressed, skin.normal), skin.tint);
    set_skin(surface.focused, skin_or(skin.focused, skin.normal), skin.tint);
    set_skin(surface.disabled, skin_or(skin.disabled, skin.normal), skin.tint);
    set_skin(surface.selected, skin_or(skin.selected, skin_or(skin.pressed, skin.normal)), skin.tint);
}

WidgetStyle skinned_style(WidgetStyle base, const UiNineSlice& skin, Color tint) {
    set_skin(base.surface, skin, tint);
    return base;
}

WidgetStyle skinned_style(WidgetStyle base, const WidgetSkin& skin) {
    set_skin(base.surface, skin);
    return base;
}

void apply_skin_pack(Theme& theme, const SkinPack& pack) {
    const Color tint = pack.skin_tint;

    // Panels / popups / overlays (single-state surfaces use the panel skin).
    set_skin(theme.panel.surface, pack.panel, tint);
    set_skin(theme.panel_surface, pack.panel, tint);
    set_skin(theme.overlay_surface, pack.panel, tint);
    set_skin(theme.header_surface, pack.panel, tint);

    // Cards — distinct skin, falling back to the panel skin.
    set_skin(theme.card_surface, skin_or(pack.card, pack.panel), tint);

    // Buttons — per interaction state, each falling back to the base button skin.
    set_skin(theme.button.surface.normal, pack.button, tint);
    set_skin(theme.button.surface.hovered, skin_or(pack.button_hovered, pack.button), tint);
    set_skin(theme.button.surface.pressed, skin_or(pack.button_pressed, pack.button), tint);
    set_skin(theme.button.surface.focused, pack.button, tint);
    set_skin(theme.button.surface.disabled, skin_or(pack.button_disabled, pack.button), tint);
    set_skin(theme.button.surface.selected, skin_or(pack.button_pressed, pack.button), tint);

    // Danger buttons — fall back to the regular button skins when unset.
    const UiNineSlice& danger = skin_or(pack.danger_button, pack.button);
    set_skin(theme.danger_button.surface.normal, danger, tint);
    set_skin(theme.danger_button.surface.hovered,
             skin_or(pack.danger_button_hovered, skin_or(pack.danger_button, pack.button_hovered)), tint);
    set_skin(theme.danger_button.surface.pressed,
             skin_or(pack.danger_button_pressed, skin_or(pack.danger_button, pack.button_pressed)), tint);
    set_skin(theme.danger_button.surface.focused, danger, tint);
    set_skin(theme.danger_button.surface.disabled, skin_or(pack.button_disabled, danger), tint);
    set_skin(theme.danger_button.surface.selected,
             skin_or(pack.danger_button_pressed, danger), tint);
    set_skin(theme.danger_surface, danger, tint);

    // Text inputs.
    set_skin(theme.input.surface, pack.input, tint);
    set_skin(theme.input_surface, pack.input, tint);

    // Menus / popups / tooltips (menu skin, falling back to panel).
    const UiNineSlice& menu = skin_or(pack.menu, pack.panel);
    set_skin(theme.menu.surface, menu, tint);
    set_skin(theme.menu_surface, menu, tint);
    set_skin(theme.popup_surface, menu, tint);
    set_skin(theme.tooltip_surface, menu, tint);

    // Tabs — fall back to the button skin.
    set_skin(theme.tab.surface, skin_or(pack.tab, pack.button), tint);

    // List / grid / tree rows (list_item skin, falling back to menu then panel).
    const UiNineSlice& list_item = skin_or(pack.list_item, menu);
    set_skin(theme.list_item.surface, list_item, tint);
    set_skin(theme.row_surface, list_item, tint);
    set_skin(theme.selected_surface, list_item, tint);

    // Progress bar — track and fill share the skin; fill is additionally tinted with the
    // progress fill color so it shows the accent color through the 9-slice texture.
    if (pack.progress.sprite.valid()) {
        set_skin(theme.progress_track_surface, pack.progress, tint);
        // Fill tint: blend the pack tint with the fill indicator color so the bar stands out.
        set_skin(theme.progress_fill_surface, pack.progress, theme.progress_fill);
    }

    // Scrollbar track (falls back to panel) and thumb (falls back to button).
    set_skin(theme.scrollbar_track_surface, skin_or(pack.scrollbar_track, pack.panel), tint);
    set_skin(theme.scrollbar_thumb_surface, skin_or(pack.scrollbar_thumb, pack.button), tint);

    // Meter — filled segments tinted with the meter fill color; empty with the empty color.
    if (pack.meter.sprite.valid()) {
        set_skin(theme.meter_fill_surface, pack.meter, theme.meter_fill);
        set_skin(theme.meter_empty_surface, pack.meter, theme.meter_empty);
    }
}

void Theme::set_variant(std::string name, WidgetStyle style) {
    variants[std::move(name)] = std::move(style);
}

const WidgetStyle* Theme::variant(std::string_view name) const {
    const auto it = variants.find(std::string(name));
    return it == variants.end() ? nullptr : &it->second;
}

} // namespace kin::ui2
