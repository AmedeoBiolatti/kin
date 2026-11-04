#pragma once
#include <algorithm>
#include <map>
#include <sol/sol.hpp>
#include <string>
#include <vector>
namespace kin::scripting_detail {
template <typename T> void record_lua_type(sol::state& lua, const char* name) {
    sol::object raw = lua.registry()[sol::usertype_traits<T>::metatable()];
    if (raw.get_type() != sol::type::table)
        return;
    sol::table registry = lua.registry()["kin.api_surface"].get_or_create<sol::table>();
    registry[name] = raw;
}
inline std::map<std::string, std::vector<std::string>> lua_api_surface(sol::state& lua) {
    std::map<std::string, std::vector<std::string>> result;
    for (const auto& [key, value] : lua.globals()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table)
            continue;
        const auto name = key.as<std::string>();
        if (name == "_G" || name == "base" || name == "math" || name == "table" || name == "string" || name == "utf8" ||
            name == "package" || name.starts_with("sol."))
            continue;
        auto& fields = result[name];
        for (const auto& [member, item] : value.as<sol::table>()) {
            if (member.get_type() != sol::type::string)
                continue;
            auto field = member.as<std::string>();
            if (!field.starts_with("__") && field != "new")
                fields.push_back(field);
        }
        std::ranges::sort(fields);
    }
    sol::optional<sol::table> types = lua.registry()["kin.api_surface"];
    if (types)
        for (const auto& [key, value] : *types) {
            auto& fields = result[key.as<std::string>()];
            for (const auto& [member, item] : value.as<sol::table>()) {
                if (member.get_type() != sol::type::string)
                    continue;
                auto field = member.as<std::string>();
                if (!field.starts_with("__") && field != "new" && field != "class_check" && field != "class_cast")
                    fields.push_back(field);
            }
            std::ranges::sort(fields);
            fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
        }
    return result;
}
} // namespace kin::scripting_detail
