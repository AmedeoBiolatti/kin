#include <kin/platform/user_data.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#endif

namespace kin {
namespace {

std::filesystem::path& user_data_override() {
    static std::filesystem::path path;
    return path;
}

std::string& game_id_setting() {
    static std::string id;
    return id;
}

bool fail(std::string* error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return false;
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
#if defined(__EMSCRIPTEN__)
    // web/persistent.js mounts the browser's storage there (KIN_WEB_PERSISTENCE).
    return std::filesystem::path{"/persistent"};
#elif defined(__ANDROID__)
    // The app's private files folder (/data/user/0/<package>/files): no
    // permission needed, removed with the app. Android has no HOME.
    if (const char* internal = SDL_GetAndroidInternalStoragePath()) {
        return std::filesystem::path{internal};
    }
    return std::filesystem::current_path();
#elif defined(_WIN32)
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

void user_data_written() {
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if (Module.kinStorage)
            Module.kinStorage.flush();
    });
#endif
}

bool write_user_file(const std::filesystem::path& path, std::string_view text, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return fail(error, "failed to create folder: " + ec.message());
        }
    }
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) {
            return fail(error, "failed to open temporary file " + temp.string());
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            return fail(error, "failed to write temporary file " + temp.string());
        }
    }
#if defined(_WIN32)
    if (!MoveFileExW(temp.native().c_str(), path.native().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD failure = GetLastError();
        std::filesystem::remove(temp, ec);
        return fail(error, "failed to replace " + path.string() + ": Windows error " + std::to_string(failure));
    }
#else
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        const std::string why = ec.message();
        std::filesystem::remove(temp, ec);
        return fail(error, "failed to replace " + path.string() + ": " + why);
    }
#endif
    user_data_written(); // in a browser, kept once in its storage
    return true;
}

void set_user_data_game_id(std::string game_id) {
    game_id_setting() = std::move(game_id);
}

const std::string& user_data_game_id() {
    return game_id_setting();
}

std::filesystem::path game_data_dir(std::string_view game_id) {
    return user_data_dir("kin") / std::string{game_id};
}

} // namespace kin
