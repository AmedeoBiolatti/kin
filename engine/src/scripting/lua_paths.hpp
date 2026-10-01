#pragma once

// Path rules shared by kin's Lua hosts (ScriptEngine, LuaScript): a script may
// only reach files under its root, and module names are plain dotted names.

#include <filesystem>
#include <optional>
#include <string_view>

namespace kin::scripting_detail {

std::filesystem::path normalized_path(const std::filesystem::path& path);
bool path_within_root(const std::filesystem::path& path, const std::filesystem::path& root);
bool module_name_allowed(std::string_view module);
// "a.b" -> <root>/a/b.lua, if it exists and stays under `root`.
std::optional<std::filesystem::path> module_path(const std::filesystem::path& root, std::string_view module);

} // namespace kin::scripting_detail
