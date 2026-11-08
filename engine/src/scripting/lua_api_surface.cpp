#include "lua_api_surface.hpp"

#include <algorithm>
#include <iterator>

namespace kin::scripting_detail {

namespace {

std::vector<std::string> api_members(const sol::table& table, bool bound_type) {
    std::vector<std::string> members;
    for (const auto& [key, value] : table) {
        if (key.get_type() != sol::type::string) {
            continue;
        }
        auto member = key.as<std::string>();
        const bool sol_internal = bound_type && (member == "class_check" || member == "class_cast");
        if (!member.starts_with("__") && member != "new" && !sol_internal) {
            members.push_back(std::move(member));
        }
    }
    std::ranges::sort(members);
    members.erase(std::unique(members.begin(), members.end()), members.end());
    return members;
}

} // namespace

void record_api_type(sol::state& lua, const char* name, const std::string& metatable,
                     const std::vector<std::string>& properties) {
    sol::table types = lua.registry()["kin.api_types"].get_or_create<sol::table>();
    types[name] = lua.registry()[metatable];
    sol::table fields = lua.registry()["kin.api_properties"].get_or_create<sol::table>();
    fields[name] = sol::as_table(properties);
}

LuaApiSurface lua_api_surface(sol::state& lua) {
    LuaApiSurface surface;
    for (const auto& [key, value] : lua.globals()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::table) {
            continue;
        }
        const auto name = key.as<std::string>();
        if (name == "_G" || name == "base" || name == "math" || name == "table" || name == "string" || name == "utf8" ||
            name == "package" || name.starts_with("sol.") || name.starts_with("__")) {
            continue;
        }
        surface.tables[name] = api_members(value.as<sol::table>(), false);
    }
    if (sol::optional<sol::table> types = lua.registry()["kin.api_types"]) {
        for (const auto& [key, value] : *types) {
            auto& members = surface.tables[key.as<std::string>()];
            std::ranges::move(api_members(value.as<sol::table>(), true), std::back_inserter(members));
            std::ranges::sort(members);
            members.erase(std::unique(members.begin(), members.end()), members.end());
        }
    }
    if (sol::optional<sol::table> properties = lua.registry()["kin.api_properties"]) {
        for (const auto& [key, value] : *properties) {
            auto names = value.as<std::vector<std::string>>();
            if (!names.empty()) {
                std::ranges::sort(names);
                surface.properties[key.as<std::string>()] = std::move(names);
            }
        }
    }
    return surface;
}

} // namespace kin::scripting_detail
