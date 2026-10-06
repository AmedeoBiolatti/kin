#include "lua_paths.hpp"

#include <kin/assets/content.hpp>

#include <algorithm>
#include <string>

namespace kin::scripting_detail {

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
    return (error ? absolute : canonical).lexically_normal();
}

bool path_within_root(const std::filesystem::path& path, const std::filesystem::path& root) {
    const std::wstring path_text = normalized_path(path).wstring();
    const std::wstring root_text = normalized_path(root).wstring();
    if (path_text.size() < root_text.size()) {
        return false;
    }
    if (!std::equal(root_text.begin(), root_text.end(), path_text.begin())) {
        return false;
    }
    if (path_text.size() == root_text.size()) {
        return true;
    }
    const wchar_t separator = path_text[root_text.size()];
    return separator == L'\\' || separator == L'/';
}

bool module_name_allowed(std::string_view module) {
    if (module.empty() || module.find("..") != std::string_view::npos) {
        return false;
    }
    if (module.find('/') != std::string_view::npos || module.find('\\') != std::string_view::npos || module.find(':') != std::string_view::npos) {
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> module_path(const std::filesystem::path& root, std::string_view module) {
    if (!module_name_allowed(module)) {
        return std::nullopt;
    }
    std::string relative{module};
    std::ranges::replace(relative, '.', '/');
    relative += ".lua";
    const std::filesystem::path candidate = root / relative;
    if (!path_within_root(candidate, root) || !content_file_exists(candidate)) {
        return std::nullopt;
    }
    return normalized_path(candidate);
}

} // namespace kin::scripting_detail
