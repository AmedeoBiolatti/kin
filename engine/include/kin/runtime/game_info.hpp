#pragma once

#include <kin/platform/input.hpp>
#include <kin/runtime/windowed_app.hpp>

#include <iosfwd>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct GameInfoField {
    std::string key;
    std::string value;
};

struct GameWindowInfo {
    i32 width = 1280;
    i32 height = 720;
    i32 logical_width = 0;
    i32 logical_height = 0;
    bool integer_scale = false;
    bool resizable = false;
    bool borderless = false;
    ColorSpace color_space = ColorSpace::Gamma; // see Renderer2D::set_color_space
    bool hdr = false;
    // A tool rather than a game: draw only when something changes (see
    // AppConfig::wait_for_events).
    bool wait_for_events = false;
};

// Every member has a default, so a designated initializer can leave any out
// without -Wmissing-field-initializers warnings.
struct GameInfo {
    std::string id{};
    std::string title = "Kin";
    std::string version{};
    std::string description{};
    std::string author{};
    // Its icon: a PNG in its content (the window's; kin_game makes the Windows
    // executable's from <name>.ico beside it).
    std::string icon{};
    GameWindowInfo window{};
    bool headless_supported = true;
    std::vector<std::string> tags{};
    std::vector<GameInfoField> fields{};
    std::optional<InputMap> input_map{};
};

// Its window; with `content` (its content root), its icon too.
WindowedAppConfig window_config(const GameInfo& info, const std::filesystem::path& content = {});

void set_field(GameInfo& info, std::string_view key, std::string_view value);
const std::string* field(const GameInfo& info, std::string_view key);

GameInfo load_game_info(const std::filesystem::path& path);
bool save_game_info(const GameInfo& info, const std::filesystem::path& path);
void write_game_info(std::ostream& out, const GameInfo& info);

} // namespace kin
