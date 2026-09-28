#pragma once

#include <filesystem>
#include <string_view>

namespace kin {

std::filesystem::path user_data_dir(std::string_view app_id);
void set_user_data_dir_override(std::filesystem::path path);
void clear_user_data_dir_override();

} // namespace kin
