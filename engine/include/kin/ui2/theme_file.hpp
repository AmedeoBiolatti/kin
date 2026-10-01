#pragma once

#include <kin/renderer/color.hpp>
#include <kin/ui2/theme.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kin {
class Renderer2D;
}

namespace kin::ui2 {

// A nine-slice frame a theme file asks for: drawn by make_frame_skin() once a
// renderer exists (see ThemeFile::build_skin).
struct ThemeSkinFrame {
    Color border{};
    Color fill{};
    f32 radius = 4.0f;
    f32 border_width = 1.0f;
    i32 size = 32; // texture side, px
};

// A ui2 theme as data: a .kintheme file a game edits without rebuilding.
//
//   [theme]                 # all optional
//   base = game             # game | editor | compact
//   palette = slate         # default | slate | ember | verdant | parchment | high_contrast
//   system_font = 15        # use the system UI font at this size (pt)
//   transition = 0.08       # seconds widgets take to change state
//
//   [colors]                # the game's own colours, by name
//   panel_dark = 0a1015     # rrggbb or rrggbbaa (no '#': it starts a comment)
//   glow = accent@40        # another colour by name, with alpha 40 (hex)
//
//   [tokens]                # kin's colour tokens; the rest of the theme follows them
//   surface_panel = panel_dark
//   text = dcf0e8
//
//   [style]                 # sizes: padding_x, spacing, row_height, radius, text_scale, ...
//   radius = 6
//
//   [surface.panel]         # one surface, after the tokens: panel, card, button, input, ...
//   gradient = 121d22 090e12 vertical
//   border = 2c464a
//   border_width = 1
//   radius = 8
//   shadow = 000000aa 0 8 24
//
//   [skin]                  # nine-slice frames for kin's widgets: border fill radius border_width [size]
//   button = 213135 0f161b 5 1
//
// A colour is hex, `transparent`, or a name from [colors] (in any order), each
// optionally followed by @aa to set its alpha. Unknown sections, keys or names
// are errors, so a typo never passes quietly.
struct ThemeFile {
    Theme theme;                                          // ready to use (without [skin] until build_skin)
    std::unordered_map<std::string, Color> colors;        // the [colors] section
    std::vector<std::pair<std::string, ThemeSkinFrame>> skin; // the [skin] section, in file order
    Color skin_tint = colors::white;

    // A colour from [colors], or `fallback`.
    Color color(std::string_view name, Color fallback = colors::transparent) const;

    // Draws the [skin] frames and applies them to `theme` (apply_skin_pack). The
    // frames are GPU textures: let the theme go before the renderer does.
    void build_skin(Renderer2D& renderer);
};

// Parses a .kintheme. On success fills `out` and returns true; otherwise leaves
// `out` untouched and adds "line N: ..." messages to `errors`. Its signature
// fits FileWatcher::load_and_watch, for themes that reload as they are edited.
bool parse_theme_file(std::string_view text, ThemeFile& out, std::vector<std::string>& errors);
// Reads and parses a file; false (with errors) if it cannot be read or parsed.
bool load_theme_file(const std::filesystem::path& path, ThemeFile& out, std::vector<std::string>& errors);

} // namespace kin::ui2
