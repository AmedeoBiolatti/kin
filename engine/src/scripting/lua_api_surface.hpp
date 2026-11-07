#pragma once

// The bindings a VM offers, as `kin_lua api` lists them: every global table
// (a host's services) and every type bound with new_api_type().

#include <kin/scripting/lua_runtime.hpp>

#include <algorithm>
#include <map>
#include <sol/sol.hpp>
#include <string>
#include <utility>
#include <vector>

namespace kin::scripting_detail {

template <typename Key, typename Value, typename... Rest>
void collect_api_properties(std::vector<std::string>& properties, const Key& key, const Value&, const Rest&... rest) {
    if constexpr (sol::is_variable_binding<Value>::value) {
        properties.emplace_back(key);
    }
    if constexpr (sizeof...(Rest) > 0) {
        collect_api_properties(properties, rest...);
    }
}

// lua.new_usertype<T>(name, constructor, members...), recorded for the API
// surface with the members read as fields (sol::property, member pointers).
template <typename T, typename Constructor, typename... Members>
sol::usertype<T> new_api_type(sol::state& lua, const char* name, Constructor&& constructor, Members&&... members) {
    std::vector<std::string> properties;
    if constexpr (sizeof...(Members) > 0) {
        collect_api_properties(properties, members...);
    }
    sol::usertype<T> type =
        lua.new_usertype<T>(name, std::forward<Constructor>(constructor), std::forward<Members>(members)...);
    sol::table types = lua.registry()["kin.api_types"].get_or_create<sol::table>();
    types[name] = lua.registry()[sol::usertype_traits<T>::metatable()];
    sol::table fields = lua.registry()["kin.api_properties"].get_or_create<sol::table>();
    fields[name] = sol::as_table(properties);
    return type;
}

// Large bindings use individual statements to keep heterogeneous member tuples
// out of sol2's forwarding layers and the optimizer. The callable types, order
// and property metadata are the same as in new_api_type().
template <typename T>
class ApiTypeDefinition {
public:
    ApiTypeDefinition(sol::usertype<T>& type, std::vector<std::string>& properties)
        : _type(type), _properties(properties) {}

    template <typename Value>
    void member(const char* name, Value&& value) {
        if constexpr (sol::is_variable_binding<Value>::value) {
            _properties.emplace_back(name);
        }
        _type.set(name, std::forward<Value>(value));
    }

private:
    sol::usertype<T>& _type;
    std::vector<std::string>& _properties;
};

template <typename T, typename Constructor, typename Define>
sol::usertype<T> define_api_type(sol::state& lua, const char* name, Constructor&& constructor, Define&& define) {
    auto type = lua.new_usertype<T>(name, std::forward<Constructor>(constructor));
    std::vector<std::string> properties;
    ApiTypeDefinition<T> definition(type, properties);
    std::forward<Define>(define)(definition);
    sol::table types = lua.registry()["kin.api_types"].get_or_create<sol::table>();
    types[name] = lua.registry()[sol::usertype_traits<T>::metatable()];
    sol::table fields = lua.registry()["kin.api_properties"].get_or_create<sol::table>();
    fields[name] = sol::as_table(properties);
    return type;
}

inline std::vector<std::string> api_members(const sol::table& table, bool bound_type) {
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

inline LuaApiSurface lua_api_surface(sol::state& lua) {
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
