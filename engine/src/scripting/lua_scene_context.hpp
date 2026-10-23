#pragma once

#include <sol/forward.hpp>
#include <string_view>

namespace kin {
class EcsEntity;
class EcsCommandBuffer;
namespace scripting_detail {
inline constexpr const char* stale_entity_message = "stale or foreign entity handle";
// The scheduler's buffer while a Lua system runs, otherwise null. Native Lua
// extensions use it so a failed system discards its deferred work normally.
EcsCommandBuffer* current_script_commands();
// Native gameplay bindings follow the same access diagnostics as reflected fields.
// Checked bridge from the reflected Lua entity API; rejects foreign userdata.
EcsEntity script_entity(const sol::object&);
void script_component_access(std::string_view component, bool write);
} // namespace scripting_detail
} // namespace kin
