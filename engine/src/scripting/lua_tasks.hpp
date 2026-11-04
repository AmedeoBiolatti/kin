#pragma once
#include <functional>
#include <kin/scripting/lua_runtime.hpp>
#include <memory>
#include <sol/sol.hpp>
#include <string>
#include <vector>
namespace kin {
class LuaTasks {
public:
    LuaTasks(sol::state& lua, LuaRuntime& runtime);
    ~LuaTasks();
    bool advance(f32 dt, std::string& error, const std::function<bool(std::string_view)>& alive = {});
    void owner(std::string value) { _owner = std::move(value); }
    void cancel(std::string_view owner = {});
    static std::optional<sol::object> clone_reload(const sol::object& value, sol::state_view target);

private:
    struct Job;
    struct Result {
        std::string state = "pending", error;
        bool cancelled = false;
    };
    struct Handle {
        std::shared_ptr<Result> value;
    };
    Handle run(sol::protected_function function);
    bool ready(Job& job, f32 dt);
    void resume(Job& job);
    sol::state& _lua;
    LuaRuntime& _runtime;
    std::string _owner;
    std::vector<std::unique_ptr<Job>> _jobs;
};
} // namespace kin
