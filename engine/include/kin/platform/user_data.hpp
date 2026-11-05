#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kin {

// Where a game keeps what it writes for the player (saves, settings): the
// system's per-user data folder (XDG_DATA_HOME or ~/.local/share, %APPDATA%),
// or the override; in a browser, /persistent (the browser's storage, IndexedDB,
// when the game is built with KIN_WEB_PERSISTENCE; memory alone otherwise).
std::filesystem::path user_data_dir(std::string_view app_id);
void set_user_data_dir_override(std::filesystem::path path);
void clear_user_data_dir_override();

// Says files under user_data_dir changed. In a browser they are kept only once
// copied into its storage, which this starts (soon after, not before it
// returns); elsewhere they already are, and it does nothing. SaveStore calls it
// after each write; call it after writing there yourself.
void user_data_written();

// Writes `text` to `path` under user_data_dir: whole or not at all (a temporary
// file beside it, then a rename over the old one), its folders made, then
// user_data_written(). False with `error` (when given) saying what failed.
bool write_user_file(const std::filesystem::path& path, std::string_view text, std::string* error = nullptr);

// The running game's id (GameInfo::id): the folder under user_data_dir("kin")
// that the files kin keeps for it go in, as SaveStore keeps its saves there
// (game_data_dir). Empty until the game sets it; input profiles need it.
void set_user_data_game_id(std::string game_id);
const std::string& user_data_game_id();
// user_data_dir("kin") / game_id: SaveStore's folder for that game.
std::filesystem::path game_data_dir(std::string_view game_id);

} // namespace kin
