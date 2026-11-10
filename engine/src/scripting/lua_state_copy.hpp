#pragma once
#include <functional>
#include <optional>
#include <sol/sol.hpp>
namespace kin::scripting_detail {
// Copies reload data, preserving integer representation. Closures, threads,
// cycles and unregistered userdata cannot cross VM boundaries.
sol::object copy_lua_data(const sol::object& source, sol::state_view target,
                          const std::function<std::optional<sol::object>(const sol::object&)>& extra = {},
                          bool entity_self = false);
} // namespace kin::scripting_detail
