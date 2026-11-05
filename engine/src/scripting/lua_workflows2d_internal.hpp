#pragma once

// What entity scripts share with the 2D workflow bindings (lua_workflows2d.cpp).

#include <kin/ecs/tween.hpp>

#include <sol/sol.hpp>

#include <string>

namespace kin::scripting_detail {

// The tween a Lua call describes, as self:tween and tweens.to take it:
// (property, to, seconds[, easing][, options]) with options delay, repeats,
// yoyo, relative and easing. Throws a sol::error that says what is wrong.
TweenDef lua_tween_def(EcsWorld& world, const std::string& property, const sol::object& to, f32 seconds,
                       const sol::object& easing_or_options, const sol::object& options);

} // namespace kin::scripting_detail
