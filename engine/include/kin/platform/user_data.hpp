#pragma once

#include <filesystem>
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

} // namespace kin
