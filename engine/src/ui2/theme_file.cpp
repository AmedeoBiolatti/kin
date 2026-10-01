#include <kin/ui2/theme_file.hpp>

#include <kin/assets/file_watcher.hpp>
#include <kin/ui2/skin_loader.hpp>

#include <array>
#include <cstdlib>
#include <optional>
#include <string>

namespace kin::ui2 {

namespace {

struct Entry {
    i32 line = 0;
    std::string key;
    std::string value;
};

struct Section {
    std::string name;
    i32 line = 0;
    std::vector<Entry> entries;
};

std::string_view trim(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r");
    return text.substr(begin, end - begin + 1);
}

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t begin = text.find_first_not_of(" \t", at);
        if (begin == std::string_view::npos) {
            break;
        }
        const std::size_t end = text.find_first_of(" \t", begin);
        out.push_back(text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        at = end == std::string_view::npos ? text.size() : end;
    }
    return out;
}

void error(std::vector<std::string>& errors, i32 line, std::string message) {
    errors.push_back("line " + std::to_string(line) + ": " + std::move(message));
}

std::vector<Section> split_sections(std::string_view text, std::vector<std::string>& errors) {
    std::vector<Section> sections;
    i32 number = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find('\n', at);
        std::string_view line = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++number;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                error(errors, number, "a section name ends with ']'");
                continue;
            }
            sections.push_back({std::string(trim(line.substr(1, line.size() - 2))), number, {}});
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            error(errors, number, "expected key = value");
            continue;
        }
        if (sections.empty()) {
            error(errors, number, "a key before any [section]");
            continue;
        }
        sections.back().entries.push_back(
            {number, std::string(trim(line.substr(0, equals))), std::string(trim(line.substr(equals + 1)))});
    }
    return sections;
}

std::optional<u8> hex_byte(std::string_view text) {
    if (text.size() != 2) {
        return std::nullopt;
    }
    u32 value = 0;
    for (const char c : text) {
        const u32 digit = c >= '0' && c <= '9' ? static_cast<u32>(c - '0')
                        : c >= 'a' && c <= 'f' ? static_cast<u32>(c - 'a' + 10)
                        : c >= 'A' && c <= 'F' ? static_cast<u32>(c - 'A' + 10)
                                               : 99u;
        if (digit > 15) {
            return std::nullopt;
        }
        value = value * 16 + digit;
    }
    return static_cast<u8>(value);
}

std::optional<Color> hex_color(std::string_view text) {
    if (text.size() != 6 && text.size() != 8) {
        return std::nullopt;
    }
    std::array<u8, 4> rgba{0, 0, 0, 255};
    for (std::size_t i = 0; i < text.size() / 2; ++i) {
        const std::optional<u8> byte = hex_byte(text.substr(i * 2, 2));
        if (!byte) {
            return std::nullopt;
        }
        rgba[i] = *byte;
    }
    return Color::rgba(rgba[0], rgba[1], rgba[2], rgba[3]);
}

std::optional<f32> number(std::string_view text) {
    const std::string copy{text};
    char* end = nullptr;
    const f32 value = std::strtof(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> boolean(std::string_view text) {
    if (text == "true" || text == "yes" || text == "1") {
        return true;
    }
    if (text == "false" || text == "no" || text == "0") {
        return false;
    }
    return std::nullopt;
}

// Colours by name: [colors] entries resolve lazily, so they may name each
// other in any order (a cycle is an error).
class ColorTable {
public:
    ColorTable(const Section* section, std::vector<std::string>& errors) : _errors(errors) {
        if (section) {
            for (const Entry& entry : section->entries) {
                _raw.emplace(entry.key, &entry);
            }
        }
    }

    // A colour value: hex, transparent, or a name; with an optional @aa alpha.
    std::optional<Color> resolve(std::string_view value, i32 line, std::string& why) {
        std::string_view base = value;
        std::optional<u8> alpha;
        if (const std::size_t at = value.find('@'); at != std::string_view::npos) {
            base = value.substr(0, at);
            alpha = hex_byte(value.substr(at + 1));
            if (!alpha) {
                why = "the alpha after '@' is two hex digits";
                return std::nullopt;
            }
        }
        std::optional<Color> color;
        if (base == "transparent") {
            color = colors::transparent;
        } else if (const std::optional<Color> hex = hex_color(base)) {
            color = hex;
        } else {
            color = named(base, line, why);
        }
        if (color && alpha) {
            color->a = *alpha;
        }
        return color;
    }

    std::unordered_map<std::string, Color> all() {
        std::unordered_map<std::string, Color> out;
        for (const auto& [name, entry] : _raw) {
            std::string why;
            if (const std::optional<Color> color = named(name, entry->line, why)) {
                out.emplace(name, *color);
            }
        }
        return out;
    }

    // Reports each [colors] entry that does not resolve, once.
    void check() {
        for (const auto& [name, entry] : _raw) {
            std::string why;
            if (!named(name, entry->line, why) && !_reported.contains(name)) {
                _reported.insert({name, true});
                error(_errors, entry->line, name + ": " + why);
            }
        }
    }

private:
    std::optional<Color> named(std::string_view name, i32, std::string& why) {
        const std::string key{name};
        if (const auto done = _resolved.find(key); done != _resolved.end()) {
            return done->second;
        }
        const auto raw = _raw.find(key);
        if (raw == _raw.end()) {
            why = "no colour '" + key + "' (hex is rrggbb or rrggbbaa, or name one from [colors])";
            return std::nullopt;
        }
        if (_resolving.contains(key)) {
            why = "colour '" + key + "' names itself in a loop";
            return std::nullopt;
        }
        _resolving.insert({key, true});
        std::optional<Color> color = resolve(raw->second->value, raw->second->line, why);
        _resolving.erase(key);
        if (color) {
            _resolved.emplace(key, *color);
        }
        return color;
    }

    std::vector<std::string>& _errors;
    std::unordered_map<std::string, const Entry*> _raw;
    std::unordered_map<std::string, Color> _resolved;
    std::unordered_map<std::string, bool> _resolving;
    std::unordered_map<std::string, bool> _reported;
};

template <typename Field>
struct Named {
    std::string_view name;
    Field field;
};

constexpr std::array token_fields{
    Named<Color ThemeColorTokens::*>{"app_background", &ThemeColorTokens::app_background},
    Named<Color ThemeColorTokens::*>{"surface_subtle", &ThemeColorTokens::surface_subtle},
    Named<Color ThemeColorTokens::*>{"surface_panel", &ThemeColorTokens::surface_panel},
    Named<Color ThemeColorTokens::*>{"surface_card", &ThemeColorTokens::surface_card},
    Named<Color ThemeColorTokens::*>{"surface_overlay", &ThemeColorTokens::surface_overlay},
    Named<Color ThemeColorTokens::*>{"interactive_normal", &ThemeColorTokens::interactive_normal},
    Named<Color ThemeColorTokens::*>{"interactive_hovered", &ThemeColorTokens::interactive_hovered},
    Named<Color ThemeColorTokens::*>{"interactive_pressed", &ThemeColorTokens::interactive_pressed},
    Named<Color ThemeColorTokens::*>{"interactive_selected", &ThemeColorTokens::interactive_selected},
    Named<Color ThemeColorTokens::*>{"interactive_disabled", &ThemeColorTokens::interactive_disabled},
    Named<Color ThemeColorTokens::*>{"border", &ThemeColorTokens::border},
    Named<Color ThemeColorTokens::*>{"border_strong", &ThemeColorTokens::border_strong},
    Named<Color ThemeColorTokens::*>{"focus", &ThemeColorTokens::focus},
    Named<Color ThemeColorTokens::*>{"text", &ThemeColorTokens::text},
    Named<Color ThemeColorTokens::*>{"text_muted", &ThemeColorTokens::text_muted},
    Named<Color ThemeColorTokens::*>{"text_emphasis", &ThemeColorTokens::text_emphasis},
    Named<Color ThemeColorTokens::*>{"text_disabled", &ThemeColorTokens::text_disabled},
    Named<Color ThemeColorTokens::*>{"solid_accent", &ThemeColorTokens::solid_accent},
    Named<Color ThemeColorTokens::*>{"solid_success", &ThemeColorTokens::solid_success},
    Named<Color ThemeColorTokens::*>{"solid_warning", &ThemeColorTokens::solid_warning},
    Named<Color ThemeColorTokens::*>{"solid_danger", &ThemeColorTokens::solid_danger},
    Named<Color ThemeColorTokens::*>{"solid_info", &ThemeColorTokens::solid_info},
    Named<Color ThemeColorTokens::*>{"text_on_solid", &ThemeColorTokens::text_on_solid},
    Named<Color ThemeColorTokens::*>{"shadow", &ThemeColorTokens::shadow},
};

constexpr std::array style_numbers{
    Named<f32 StylePreset::*>{"text_scale", &StylePreset::text_scale},
    Named<f32 StylePreset::*>{"title_scale", &StylePreset::title_scale},
    Named<f32 StylePreset::*>{"small_scale", &StylePreset::small_scale},
    Named<f32 StylePreset::*>{"padding_x", &StylePreset::padding_x},
    Named<f32 StylePreset::*>{"padding_y", &StylePreset::padding_y},
    Named<f32 StylePreset::*>{"spacing", &StylePreset::spacing},
    Named<f32 StylePreset::*>{"row_height", &StylePreset::row_height},
    Named<f32 StylePreset::*>{"compact_row_height", &StylePreset::compact_row_height},
    Named<f32 StylePreset::*>{"border_width", &StylePreset::border_width},
    Named<f32 StylePreset::*>{"radius", &StylePreset::radius},
    Named<f32 StylePreset::*>{"icon_size", &StylePreset::icon_size},
    Named<f32 StylePreset::*>{"scrollbar_thickness", &StylePreset::scrollbar_thickness},
};

constexpr std::array style_flags{
    Named<bool StylePreset::*>{"draw_surfaces", &StylePreset::draw_surfaces},
    Named<bool StylePreset::*>{"draw_outlines", &StylePreset::draw_outlines},
    Named<bool StylePreset::*>{"draw_shadows", &StylePreset::draw_shadows},
};

constexpr std::array surfaces{
    Named<SurfaceStyle Theme::*>{"panel", &Theme::panel_surface},
    Named<SurfaceStyle Theme::*>{"overlay", &Theme::overlay_surface},
    Named<SurfaceStyle Theme::*>{"button", &Theme::button_surface},
    Named<SurfaceStyle Theme::*>{"input", &Theme::input_surface},
    Named<SurfaceStyle Theme::*>{"menu", &Theme::menu_surface},
    Named<SurfaceStyle Theme::*>{"popup", &Theme::popup_surface},
    Named<SurfaceStyle Theme::*>{"tooltip", &Theme::tooltip_surface},
    Named<SurfaceStyle Theme::*>{"danger", &Theme::danger_surface},
    Named<SurfaceStyle Theme::*>{"selected", &Theme::selected_surface},
    Named<SurfaceStyle Theme::*>{"disabled", &Theme::disabled_surface},
    Named<SurfaceStyle Theme::*>{"header", &Theme::header_surface},
    Named<SurfaceStyle Theme::*>{"row", &Theme::row_surface},
    Named<SurfaceStyle Theme::*>{"card", &Theme::card_surface},
    Named<SurfaceStyle Theme::*>{"subtle", &Theme::subtle_surface},
    Named<SurfaceStyle Theme::*>{"progress_track", &Theme::progress_track_surface},
    Named<SurfaceStyle Theme::*>{"progress_fill", &Theme::progress_fill_surface},
    Named<SurfaceStyle Theme::*>{"scrollbar_track", &Theme::scrollbar_track_surface},
    Named<SurfaceStyle Theme::*>{"scrollbar_thumb", &Theme::scrollbar_thumb_surface},
    Named<SurfaceStyle Theme::*>{"meter_fill", &Theme::meter_fill_surface},
    Named<SurfaceStyle Theme::*>{"meter_empty", &Theme::meter_empty_surface},
};

constexpr std::array skin_slots{
    Named<UiNineSlice SkinPack::*>{"panel", &SkinPack::panel},
    Named<UiNineSlice SkinPack::*>{"button", &SkinPack::button},
    Named<UiNineSlice SkinPack::*>{"button_hovered", &SkinPack::button_hovered},
    Named<UiNineSlice SkinPack::*>{"button_pressed", &SkinPack::button_pressed},
    Named<UiNineSlice SkinPack::*>{"button_disabled", &SkinPack::button_disabled},
    Named<UiNineSlice SkinPack::*>{"input", &SkinPack::input},
    Named<UiNineSlice SkinPack::*>{"card", &SkinPack::card},
    Named<UiNineSlice SkinPack::*>{"danger_button", &SkinPack::danger_button},
    Named<UiNineSlice SkinPack::*>{"danger_button_hovered", &SkinPack::danger_button_hovered},
    Named<UiNineSlice SkinPack::*>{"danger_button_pressed", &SkinPack::danger_button_pressed},
    Named<UiNineSlice SkinPack::*>{"menu", &SkinPack::menu},
    Named<UiNineSlice SkinPack::*>{"tab", &SkinPack::tab},
    Named<UiNineSlice SkinPack::*>{"list_item", &SkinPack::list_item},
    Named<UiNineSlice SkinPack::*>{"progress", &SkinPack::progress},
    Named<UiNineSlice SkinPack::*>{"scrollbar_track", &SkinPack::scrollbar_track},
    Named<UiNineSlice SkinPack::*>{"scrollbar_thumb", &SkinPack::scrollbar_thumb},
    Named<UiNineSlice SkinPack::*>{"meter", &SkinPack::meter},
};

template <typename Table>
auto find(const Table& table, std::string_view name) -> decltype(&table[0]) {
    for (const auto& entry : table) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::string names(const auto& table) {
    std::string out;
    for (const auto& entry : table) {
        out += out.empty() ? "" : ", ";
        out += entry.name;
    }
    return out;
}

} // namespace

Color ThemeFile::color(std::string_view name, Color fallback) const {
    const auto found = colors.find(std::string(name));
    return found == colors.end() ? fallback : found->second;
}

void ThemeFile::build_skin(Renderer2D& renderer) {
    if (skin.empty()) {
        return;
    }
    SkinPack pack;
    pack.skin_tint = skin_tint;
    for (const auto& [slot, frame] : skin) {
        if (const auto* entry = find(skin_slots, slot)) {
            pack.*(entry->field) = make_frame_skin(renderer, frame.size, frame.border, frame.fill, frame.radius,
                                                   frame.border_width);
        }
    }
    apply_skin_pack(theme, pack);
}

bool parse_theme_file(std::string_view text, ThemeFile& out, std::vector<std::string>& errors) {
    const std::size_t had = errors.size();
    const std::vector<Section> sections = split_sections(text, errors);

    const Section* color_section = nullptr;
    for (const Section& section : sections) {
        if (section.name == "colors") {
            color_section = &section;
        }
    }
    ColorTable table{color_section, errors};
    table.check();
    const auto color = [&](const Entry& entry) -> std::optional<Color> {
        std::string why;
        std::optional<Color> value = table.resolve(entry.value, entry.line, why);
        if (!value) {
            error(errors, entry.line, entry.key + ": " + why);
        }
        return value;
    };
    const auto real = [&](const Entry& entry) -> std::optional<f32> {
        std::optional<f32> value = number(entry.value);
        if (!value) {
            error(errors, entry.line, entry.key + ": expected a number");
        }
        return value;
    };

    // [theme]: where the rest starts from.
    StylePresetKind kind = StylePresetKind::Game;
    PalettePreset palette_preset = PalettePreset::Default;
    std::optional<f32> system_font;
    std::optional<f32> transition;
    for (const Section& section : sections) {
        if (section.name != "theme") {
            continue;
        }
        for (const Entry& entry : section.entries) {
            if (entry.key == "base") {
                if (entry.value == "game") kind = StylePresetKind::Game;
                else if (entry.value == "editor") kind = StylePresetKind::Editor;
                else if (entry.value == "compact") kind = StylePresetKind::Compact;
                else error(errors, entry.line, "base: one of game, editor, compact");
            } else if (entry.key == "palette") {
                if (entry.value == "default") palette_preset = PalettePreset::Default;
                else if (entry.value == "slate") palette_preset = PalettePreset::Slate;
                else if (entry.value == "ember") palette_preset = PalettePreset::Ember;
                else if (entry.value == "verdant") palette_preset = PalettePreset::Verdant;
                else if (entry.value == "parchment") palette_preset = PalettePreset::Parchment;
                else if (entry.value == "high_contrast") palette_preset = PalettePreset::HighContrast;
                else error(errors, entry.line, "palette: one of default, slate, ember, verdant, parchment, high_contrast");
            } else if (entry.key == "system_font") {
                system_font = real(entry);
            } else if (entry.key == "transition") {
                transition = real(entry);
            } else {
                error(errors, entry.line, "no key '" + entry.key + "' in [theme] (base, palette, system_font, transition)");
            }
        }
    }

    // [style] and [tokens], then every derived style from them.
    StylePreset style = style_preset(kind);
    const Theme base = make_theme(style, palette(palette_preset));
    ThemeColorTokens tokens = base.colors;
    for (const Section& section : sections) {
        if (section.name == "style") {
            for (const Entry& entry : section.entries) {
                if (const auto* field = find(style_numbers, entry.key)) {
                    if (const std::optional<f32> value = real(entry)) style.*(field->field) = *value;
                } else if (const auto* flag = find(style_flags, entry.key)) {
                    if (const std::optional<bool> value = boolean(entry.value)) style.*(flag->field) = *value;
                    else error(errors, entry.line, entry.key + ": expected true or false");
                } else {
                    error(errors, entry.line, "no key '" + entry.key + "' in [style] (" + names(style_numbers) + ", " +
                                                  names(style_flags) + ")");
                }
            }
        } else if (section.name == "tokens") {
            for (const Entry& entry : section.entries) {
                if (const auto* field = find(token_fields, entry.key)) {
                    if (const std::optional<Color> value = color(entry)) tokens.*(field->field) = *value;
                } else {
                    error(errors, entry.line, "no token '" + entry.key + "' (" + names(token_fields) + ")");
                }
            }
        }
    }
    ThemeFile built;
    built.theme = make_theme_from_tokens(style, base.scales, tokens);
    if (system_font) {
        apply_system_fonts(built.theme, *system_font);
    }
    if (transition) {
        built.theme.transition_duration = *transition;
    }

    // [surface.<name>], in file order, over the built theme.
    for (const Section& section : sections) {
        if (!section.name.starts_with("surface.")) {
            continue;
        }
        const std::string name = section.name.substr(8);
        const auto* target = find(surfaces, name);
        if (!target) {
            error(errors, section.line, "no surface '" + name + "' (" + names(surfaces) + ")");
            continue;
        }
        SurfaceStyle& surface = built.theme.*(target->field);
        for (const Entry& entry : section.entries) {
            const std::vector<std::string_view> parts = words(entry.value);
            if (entry.key == "like") {
                if (const auto* other = find(surfaces, entry.value)) surface = built.theme.*(other->field);
                else error(errors, entry.line, "like: no surface '" + entry.value + "'");
            } else if (entry.key == "fill") {
                if (const std::optional<Color> value = color(entry)) {
                    surface.fill = *value;
                    surface.fill_kind = SurfaceFill::Solid;
                    surface.draw_fill = true;
                }
            } else if (entry.key == "gradient") {
                const bool direction = parts.size() == 3;
                if (parts.size() != 2 && !direction) {
                    error(errors, entry.line, "gradient: two colours, then vertical or horizontal");
                    continue;
                }
                std::string why;
                const std::optional<Color> start = table.resolve(parts[0], entry.line, why);
                const std::optional<Color> end = start ? table.resolve(parts[1], entry.line, why) : std::nullopt;
                if (!end) {
                    error(errors, entry.line, "gradient: " + why);
                    continue;
                }
                GradientDirection way = GradientDirection::Vertical;
                if (direction && parts[2] == "horizontal") way = GradientDirection::Horizontal;
                else if (direction && parts[2] != "vertical") {
                    error(errors, entry.line, "gradient: vertical or horizontal");
                    continue;
                }
                surface.gradient = {*start, *end, way};
                surface.fill_kind = SurfaceFill::Gradient;
                surface.draw_fill = true;
            } else if (entry.key == "border") {
                if (const std::optional<Color> value = color(entry)) {
                    surface.border = *value;
                    if (surface.border_mode == BorderMode::None) surface.border_mode = BorderMode::Inside;
                    if (surface.border_width <= 0.0f) surface.border_width = 1.0f;
                }
            } else if (entry.key == "border_width") {
                if (const std::optional<f32> value = real(entry)) {
                    surface.border_width = *value;
                    surface.border_mode = *value > 0.0f ? BorderMode::Inside : BorderMode::None;
                }
            } else if (entry.key == "radius") {
                if (const std::optional<f32> value = real(entry)) surface.radius = *value;
            } else if (entry.key == "opacity") {
                if (const std::optional<f32> value = real(entry)) surface.opacity = *value;
            } else if (entry.key == "shadow") {
                if (entry.value == "none") {
                    surface.shadow = {};
                    continue;
                }
                if (parts.size() < 4 || parts.size() > 5) {
                    error(errors, entry.line, "shadow: colour, offset x, offset y, spread [, radius] (or none)");
                    continue;
                }
                std::string why;
                const std::optional<Color> shade = table.resolve(parts[0], entry.line, why);
                std::array<std::optional<f32>, 4> values{number(parts[1]), number(parts[2]), number(parts[3]),
                                                         parts.size() == 5 ? number(parts[4]) : std::optional<f32>{surface.radius}};
                if (!shade || !values[0] || !values[1] || !values[2] || !values[3]) {
                    error(errors, entry.line, "shadow: " + (shade ? std::string("expected numbers") : why));
                    continue;
                }
                surface.shadow = {.color = *shade, .offset = {*values[0], *values[1]}, .spread = *values[2],
                                  .radius = *values[3], .layers = 4, .enabled = true};
            } else {
                error(errors, entry.line, "no key '" + entry.key + "' in a surface (like, fill, gradient, border, "
                                          "border_width, radius, shadow, opacity)");
            }
        }
    }

    // [skin]: frames to draw once a renderer exists.
    for (const Section& section : sections) {
        if (section.name != "skin") {
            continue;
        }
        for (const Entry& entry : section.entries) {
            if (entry.key == "tint") {
                if (const std::optional<Color> value = color(entry)) built.skin_tint = *value;
                continue;
            }
            if (!find(skin_slots, entry.key)) {
                error(errors, entry.line, "no skin '" + entry.key + "' (tint, " + names(skin_slots) + ")");
                continue;
            }
            const std::vector<std::string_view> parts = words(entry.value);
            if (parts.size() < 4 || parts.size() > 5) {
                error(errors, entry.line, entry.key + ": border, fill, radius, border width [, size]");
                continue;
            }
            std::string why;
            const std::optional<Color> border = table.resolve(parts[0], entry.line, why);
            const std::optional<Color> fill = border ? table.resolve(parts[1], entry.line, why) : std::nullopt;
            const std::optional<f32> radius = number(parts[2]);
            const std::optional<f32> width = number(parts[3]);
            const std::optional<f32> size = parts.size() == 5 ? number(parts[4]) : std::optional<f32>{32.0f};
            if (!fill || !radius || !width || !size) {
                error(errors, entry.line, entry.key + ": " + (fill ? std::string("expected numbers") : why));
                continue;
            }
            built.skin.emplace_back(entry.key, ThemeSkinFrame{*border, *fill, *radius, *width, static_cast<i32>(*size)});
        }
    }

    for (const Section& section : sections) {
        const bool known = section.name == "theme" || section.name == "colors" || section.name == "style" ||
                           section.name == "tokens" || section.name == "skin" || section.name.starts_with("surface.");
        if (!known) {
            error(errors, section.line, "no section [" + section.name + "] (theme, colors, style, tokens, surface.<name>, skin)");
        }
    }

    if (errors.size() > had) {
        return false;
    }
    built.colors = table.all();
    out = std::move(built);
    return true;
}

bool load_theme_file(const std::filesystem::path& path, ThemeFile& out, std::vector<std::string>& errors) {
    const std::optional<std::string> text = read_text_file(path);
    if (!text) {
        errors.push_back("could not read " + path.string());
        return false;
    }
    return parse_theme_file(*text, out, errors);
}

} // namespace kin::ui2
