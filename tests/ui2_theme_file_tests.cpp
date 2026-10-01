#include <kin/assets/file_watcher.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/ui2/theme_file.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kin::Color;

constexpr std::string_view sample = R"(
# A game's look.
[theme]
base = editor
palette = slate
transition = 0.08

[colors]
glow = accent@40       # names may come before what they name
accent = 5cdebe
panel_dark = 0a1015
edge = 244244

[tokens]
text = dcf0e8
solid_accent = accent
surface_panel = panel_dark

[style]
radius = 6
draw_shadows = true

[surface.panel]
gradient = 121d22 090e12 vertical
border = edge
radius = 8
shadow = 000000aa 0 8 24

[surface.tooltip]
like = panel
shadow = none

[skin]
button = edge panel_dark 5 1
input = edge 0f161b 4 1 48
tint = ffffffcc
)";

bool has_error(const std::vector<std::string>& errors, std::string_view part) {
    for (const std::string& error : errors) {
        if (error.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void test_a_theme_from_data() {
    kin::ui2::ThemeFile file;
    std::vector<std::string> errors;
    assert(kin::ui2::parse_theme_file(sample, file, errors));
    assert(errors.empty());
    const kin::ui2::Theme& theme = file.theme;

    // Named colours, by any order, with an alpha.
    assert((file.color("accent") == Color::rgb(0x5c, 0xde, 0xbe)));
    assert((file.color("glow") == Color::rgba(0x5c, 0xde, 0xbe, 0x40)));
    assert((file.color("missing", kin::colors::white) == kin::colors::white));

    // Tokens, and everything derived from them.
    assert((theme.colors.text == Color::rgb(0xdc, 0xf0, 0xe8)));
    assert((theme.body_text.color == theme.colors.text));
    assert((theme.colors.solid_accent == file.color("accent")));
    assert(theme.preset.radius == 6.0f && theme.preset.draw_shadows);
    assert(theme.transition_duration == 0.08f);

    // A surface, and one made like it.
    const kin::ui2::SurfaceStyle& panel = theme.panel_surface;
    assert(panel.fill_kind == kin::ui2::SurfaceFill::Gradient && panel.draw_fill);
    assert((panel.gradient.start == Color::rgb(0x12, 0x1d, 0x22)));
    assert((panel.border == Color::rgb(0x24, 0x42, 0x44)) && panel.border_width == 1.0f);
    assert(panel.border_mode == kin::ui2::BorderMode::Inside && panel.radius == 8.0f);
    assert(panel.shadow.enabled && panel.shadow.offset.y == 8.0f && panel.shadow.spread == 24.0f);
    assert(theme.tooltip_surface.radius == 8.0f && !theme.tooltip_surface.shadow.enabled);

    assert(file.skin.size() == 2 && file.skin[1].first == "input" && file.skin[1].second.size == 48);
    assert(file.skin_tint.a == 0xcc);
}

void test_mistakes_are_reported_and_change_nothing() {
    kin::ui2::ThemeFile file;
    std::vector<std::string> errors;
    assert(kin::ui2::parse_theme_file("[colors]\nmine = 102030\n", file, errors));
    const Color before = file.color("mine");

    const std::string_view broken = R"([theme]
base = fancy
[colors]
a = b
b = a
odd = 12345g
[tokens]
txt = ffffff
text = nowhere
[surface.window]
fill = 000000
[surface.panel]
shadow = 000000 1
[skin]
knob = 000000 000000 1 1
not a pair
[sizes]
)";
    errors.clear();
    assert(!kin::ui2::parse_theme_file(broken, file, errors));
    assert(has_error(errors, "line 2: base"));
    assert(has_error(errors, "names itself in a loop"));
    assert(has_error(errors, "line 6: odd"));
    assert(has_error(errors, "line 8: no token 'txt'"));
    assert(has_error(errors, "line 9: text: no colour 'nowhere'"));
    assert(has_error(errors, "line 10: no surface 'window'"));
    assert(has_error(errors, "line 13: shadow"));
    assert(has_error(errors, "line 15: no skin 'knob'"));
    assert(has_error(errors, "line 16: expected key = value"));
    assert(has_error(errors, "line 17: no section [sizes]"));
    assert((file.color("mine") == before)); // a failed parse leaves what was loaded
}

void test_skins_are_drawn_with_a_renderer() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window = app.create_window({.title = "theme-skin", .width = 64, .height = 64, .hidden = true});
    kin::Renderer2D renderer{window};
    {
        kin::ui2::ThemeFile file;
        std::vector<std::string> errors;
        assert(kin::ui2::parse_theme_file(sample, file, errors));
        assert(!file.theme.button.surface.normal.use_skin);
        file.build_skin(renderer);
        assert(file.theme.button.surface.normal.use_skin);
        assert(file.theme.input.surface.normal.use_skin);
    } // the theme, and its frames, go before the renderer
}

void test_a_theme_reloads_when_edited() {
    const fs::path dir = fs::temp_directory_path() / "kin-theme-file-tests";
    fs::create_directories(dir);
    const fs::path path = dir / "look.kintheme";
    const auto write = [&](std::string_view text, int second) {
        std::ofstream{path, std::ios::binary | std::ios::trunc} << text;
        fs::last_write_time(path, fs::file_time_type::clock::now() + std::chrono::seconds(second));
    };
    write("[tokens]\ntext = 111111\n", 1);
    kin::ui2::ThemeFile file;
    kin::FileWatcher files;
    assert(files.load_and_watch(path, [&](std::string_view text, std::vector<std::string>& errors) {
        return kin::ui2::parse_theme_file(text, file, errors);
    }));
    assert((file.theme.colors.text == Color::rgb(0x11, 0x11, 0x11)));
    write("[tokens]\ntext = 222222\n", 3);
    files.poll_now();
    files.poll_now();
    assert((file.theme.colors.text == Color::rgb(0x22, 0x22, 0x22)));
    write("[tokens]\ntext = 33\n", 5); // a mistake: the last good theme stays
    files.poll_now();
    files.poll_now();
    assert((file.theme.colors.text == Color::rgb(0x22, 0x22, 0x22)));
    fs::remove_all(dir);
}

} // namespace

int main() {
    test_a_theme_from_data();
    test_mistakes_are_reported_and_change_nothing();
    test_skins_are_drawn_with_a_renderer();
    test_a_theme_reloads_when_edited();
    return 0;
}
