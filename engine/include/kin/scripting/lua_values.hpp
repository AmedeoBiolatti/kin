#pragma once

// Lua values and the data kin keeps: component fields (entity scripts'
// get/set, tweens) and JSON (saves, kin_lua's test fixtures).

#include <kin/core/json_value.hpp>
#include <kin/ecs/component.hpp>

#include <sol/sol.hpp>

#include <optional>

namespace kin {

// A component field's value as Lua holds it: a number, true/false, text,
// {x = , y = }, {r = , g = , b = , a = }, a list of points; nil for 3D values.
sol::object component_value_to_lua(sol::state_view lua, const ComponentFieldValue& value);
// And back, as a field of `kind` takes it: nullopt when `value` is not one.
std::optional<ComponentFieldValue> component_value_from_lua(ComponentFieldKind kind, const sol::object& value);

// A Lua value as JSON keeps it: true/false, finite numbers, text, and tables
// of them (one counting from 1 with nothing else: an array); nullopt for the
// rest (functions, userdata, a table nested more than 16 deep). Fields that
// cannot be kept are left out of their table.
std::optional<JsonValue> lua_to_json(const sol::object& value, int depth = 0);
// And back (a whole number an integer again).
sol::object json_to_lua(sol::state_view lua, const JsonValue& value);

} // namespace kin
