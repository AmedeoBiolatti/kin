#pragma once

#include "lua_scene3d.hpp"

#include <initializer_list>
#include <sol/sol.hpp>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace kin::scripting_detail::lua_scene3d_detail {
template <class T> T checked(const sol::object& value, const char* name) {
    const auto type = value.get_type();
    bool matches = value.is<T>();
    if constexpr (std::is_same_v<T, sol::table>)
        matches = type == sol::type::table;
    else if constexpr (std::is_same_v<T, std::string>)
        matches = type == sol::type::string;
    else if constexpr (std::is_same_v<T, bool>)
        matches = type == sol::type::boolean;
    else if constexpr (std::is_arithmetic_v<T>)
        matches = type == sol::type::number && matches;
    if (!matches)
        throw std::runtime_error(std::string("invalid 3D option type: ") + name);
    return value.as<T>();
}
// Optional Lua parameters must distinguish nil/absent from a wrong type.
// sol::optional<T> as a function parameter intentionally erases that distinction.
template <class T> sol::optional<T> optional_argument(const sol::object& value, const char* name) {
    if (!value.valid() || value == sol::nil)
        return sol::nullopt;
    return checked<T>(value, name);
}

// Enable positional argument checks only for this table's 3D functions. No
// engine-wide sol2 configuration (and no 2D numeric conversion policy) changes.
struct CheckedBindings {
    sol::table table;
    struct Slot {
        sol::table table;
        const char* name;
        template <class F> void operator=(F&& function) {
            table.set_function(name, sol::protect(std::forward<F>(function)));
        }
    };
    Slot operator[](const char* name) const { return {table, name}; }
};

template <class T> T option(sol::table options, const char* name, T fallback) {
    sol::object value = options[name];
    return value == sol::nil ? fallback : checked<T>(value, name);
}
void fields(sol::table, std::initializer_list<std::string_view>);
Vec3f vector(sol::table);
sol::table vector(sol::state_view, Vec3f);
Quatf rotation(sol::table);
f32 number(sol::table, const char*, f32 fallback, f32 minimum, f32 maximum);
EcsCommandBuffer& command_buffer(const LuaScene3DBindings&);
EcsEntity resolve(const LuaScene3DBindings&, SceneEntity3D);
Character3D native_character(const LuaScene3DBindings&, EcsEntity);
void physics(const LuaScene3DBindings&);
SceneEntity3D body_entity(const LuaScene3DBindings&, Body3D);
void entity_field(sol::table, const char*, SceneEntity3D);
void bind_physics(sol::state&, sol::table, std::shared_ptr<LuaScene3DBindings>);
} // namespace kin::scripting_detail::lua_scene3d_detail
