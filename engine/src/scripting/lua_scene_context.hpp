#pragma once

#include <string_view>

namespace kin {
class EcsCommandBuffer;
namespace scripting_detail {
// The scheduler's buffer while a Lua system runs, otherwise null. Native Lua
// extensions use it so a failed system discards its deferred work normally.
EcsCommandBuffer* current_script_commands();
// Native gameplay bindings follow the same access diagnostics as reflected fields.
void script_component_access(std::string_view component, bool write);
} // namespace scripting_detail
} // namespace kin
