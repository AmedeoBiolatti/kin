#pragma once

// The bindings a VM offers, as `kin_lua api` lists them: every global table
// (a host's services) and every type bound with new_api_type().

#include <kin/scripting/lua_runtime.hpp>

#include <sol/sol.hpp>
#include <string>
#include <utility>
#include <vector>

namespace kin::scripting_detail {

// Keep API bookkeeping out of each bound type's registration templates.
void record_api_type(sol::state& lua, const char* name, const std::string& metatable,
                     const std::vector<std::string>& properties);

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
    record_api_type(lua, name, sol::usertype_traits<T>::metatable(), properties);
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
    record_api_type(lua, name, sol::usertype_traits<T>::metatable(), properties);
    return type;
}

LuaApiSurface lua_api_surface(sol::state& lua);

} // namespace kin::scripting_detail
