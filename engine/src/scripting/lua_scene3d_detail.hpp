#pragma once

#include "lua_scene3d.hpp"

#include <initializer_list>
#include <sol/sol.hpp>

namespace kin::scripting_detail::lua_scene3d_detail {
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
