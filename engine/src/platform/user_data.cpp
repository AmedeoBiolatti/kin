#include <kin/platform/user_data.hpp>

#include <cstdlib>
#include <string>

namespace kin {
namespace {

std::filesystem::path& user_data_override() {
    static std::filesystem::path path;
    return path;
}

std::filesystem::path home_dir() {
#if defined(_WIN32)
    if (const char* user_profile = std::getenv("USERPROFILE")) {
        return std::filesystem::path{user_profile};
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path{home};
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path platform_user_data_root() {
#if defined(_WIN32)
    if (const char* app_data = std::getenv("APPDATA")) {
        return std::filesystem::path{app_data};
    }
    return home_dir() / "AppData" / "Roaming";
#else
    if (const char* xdg_data_home = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path{xdg_data_home};
    }
    return home_dir() / ".local" / "share";
#endif
}

} // namespace

std::filesystem::path user_data_dir(std::string_view app_id) {
    const std::filesystem::path& override_path = user_data_override();
    if (!override_path.empty()) {
        return override_path / std::string{app_id};
    }
    return platform_user_data_root() / std::string{app_id};
}

void set_user_data_dir_override(std::filesystem::path path) {
    user_data_override() = std::move(path);
}

void clear_user_data_dir_override() {
    user_data_override().clear();
}

} // namespace kin
