#pragma once

// Safe live reload, shared by the hosts: while a candidate VM loads and
// migrates, nothing it runs may touch the live game.

#include <sol/sol.hpp>

namespace kin::scripting_detail {

// True while this thread prepares a reload candidate: native mutations and
// live host services refuse (a rejected candidate leaves the game as it was).
extern thread_local bool reload_staging;

// Marks this thread as preparing a reload for its lifetime.
class ReloadStaging {
public:
    ReloadStaging() : _previous(reload_staging) { reload_staging = true; }
    ~ReloadStaging() { reload_staging = _previous; }
    ReloadStaging(const ReloadStaging&) = delete;
    ReloadStaging& operator=(const ReloadStaging&) = delete;

private:
    bool _previous;
};

// The copy in `target` of a 2D workflow handle (lease, preload, tween) kept in
// reload state, if `value` is one.
sol::optional<sol::object> clone_workflow_reload_value(const sol::object& value, sol::state_view target);

} // namespace kin::scripting_detail
