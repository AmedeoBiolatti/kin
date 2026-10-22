#include <kin/platform/resource_paths.hpp>

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace kin {

std::filesystem::path executable_dir() {
    const char* base = SDL_GetBasePath();
    if (!base || !*base) {
        throw std::runtime_error(std::string{"Cannot locate executable directory: "} + SDL_GetError());
    }
    return std::filesystem::u8path(base);
}

std::filesystem::path game_assets_dir(std::string_view game_name,
                                      const std::filesystem::path& development_root) {
    if (game_name.empty() || game_name == "." || game_name == ".." ||
        game_name.find_first_of("/\\:") != std::string_view::npos) {
        throw std::invalid_argument("game_assets_dir requires a single game directory name");
    }
    if (!development_root.empty()) {
        return std::filesystem::absolute(development_root);
    }
    return executable_dir() / "assets" / std::filesystem::u8path(game_name);
}

} // namespace kin
