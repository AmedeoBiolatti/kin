#pragma once
#include <sol/sol.hpp>
namespace kin::scripting_detail {
extern thread_local bool reload_staging;
sol::optional<sol::object> clone_workflow_reload_value(const sol::object&, sol::state_view);
} // namespace kin::scripting_detail
