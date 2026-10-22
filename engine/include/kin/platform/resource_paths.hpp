#pragma once

#include <filesystem>
#include <string_view>

namespace kin {

// Absolute directory containing the desktop executable, independent of cwd.
// Throws if the platform cannot determine it; never falls back to cwd.
std::filesystem::path executable_dir();

// Packages store each game's resources at <executable>/assets/<game_name>.
// An explicit development root overrides that directory for live editing.
// Pass KIN_ASSETS_ROOT here in Debug builds to edit source assets directly.
std::filesystem::path game_assets_dir(std::string_view game_name,
                                      const std::filesystem::path& development_root = {});

} // namespace kin
