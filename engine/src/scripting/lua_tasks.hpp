#pragma once

#include <kin/scripting/lua_runtime.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <sol/sol.hpp>
#include <string>
#include <vector>

namespace kin {

// A VM's `task` scheduler: coroutines that wait for time, frames or a
// condition, stepped only by the host's simulation (never wall time). A task
// belongs to the owner current when it starts (EntityScripts: its entity's id).
class LuaTasks {
public:
    LuaTasks(sol::state& lua, LuaRuntime& runtime);
    ~LuaTasks();
    // Steps every task; one whose owner is not alive(owner) is cancelled.
    bool advance(f32 dt, std::string& error, const std::function<bool(u64)>& alive = {});
    // Tasks started from now on belong to `owner` (0: none).
    void set_owner(u64 owner) { _owner = owner; }
    void cancel_all();
    void cancel(u64 owner);
    static std::optional<sol::object> clone_reload(const sol::object& value, sol::state_view target);

private:
    enum class State : u8 { Pending, Running, Waiting, Completed, Cancelled, Failed };
    enum class Wait : u8 { None, Time, Frames, Predicate };
    struct Job;
    struct Result {
        State state = State::Pending;
        std::string error;
        bool cancelled = false;
    };
    struct Handle {
        std::shared_ptr<Result> value;
    };
    static std::string_view state_name(State state);
    static bool finished(State state);
    static void cancel(Result& result);
    Handle run(sol::protected_function function);
    bool ready(Job& job, f32 dt);
    void resume(Job& job);

    sol::state& _lua;
    LuaRuntime& _runtime;
    u64 _owner = 0;
    std::vector<std::unique_ptr<Job>> _jobs;
};

} // namespace kin
