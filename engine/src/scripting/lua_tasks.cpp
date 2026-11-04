#include "lua_tasks.hpp"

#include "lua_api_surface.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace kin {
struct LuaTasks::Job {
    sol::thread thread;
    std::shared_ptr<Result> result;
    std::string owner;
    std::string waiting;
    sol::object value;
    f64 seconds = 0, timeout = -1;
    i64 frames = 0;
};
LuaTasks::LuaTasks(sol::state& lua, LuaRuntime& runtime) : _lua(lua), _runtime(runtime) {
    lua.new_usertype<Handle>(
        "KinTask", sol::no_constructor, "state", [](const Handle& h) { return h.value->state; }, "error",
        [](const Handle& h) { return h.value->error; }, "cancel",
        [](Handle& h) {
            if (h.value->state == "pending" || h.value->state == "running" || h.value->state == "waiting") {
                h.value->cancelled = true;
                h.value->state = "cancelled";
            }
        });
    scripting_detail::record_lua_type<Handle>(lua, "KinTask");
    auto table = lua.create_named_table("task");
    table["run"] = [this](sol::protected_function fn) { return run(std::move(fn)); };
    table["cancel"] = [](Handle& h) {
        if (h.value->state == "pending" || h.value->state == "running" || h.value->state == "waiting") {
            h.value->cancelled = true;
            h.value->state = "cancelled";
        }
    };
    // Keep coroutine primitives private: only managed tasks can yield.
    lua.open_libraries(sol::lib::coroutine);
    lua.script(R"lua(
        local yield = coroutine.yield
        function task.wait(seconds)
            assert(type(seconds)=='number' and seconds==seconds and seconds>=0 and seconds<math.huge, 'wait requires finite nonnegative seconds')
            return yield('time', seconds)
        end
        function task.frames(count)
            assert(type(count)=='number' and count>=1 and count%1==0 and count<=2147483647, 'frames requires a positive integer')
            return yield('frames', count)
        end
        function task.until_(predicate, timeout)
            assert(type(predicate)=='function', 'until_ requires a predicate')
            assert(timeout==nil or (type(timeout)=='number' and timeout>=0 and timeout<math.huge), 'invalid task timeout')
            return yield('predicate', predicate, timeout)
        end
        function task.await(handle, timeout)
            return task.until_(function()
                if handle.finished then
                    if handle:cancelled() then error('awaited tween was cancelled') end
                    return handle:finished()
                end
                local state = handle:state()
                if state=='failed' or state=='cancelled' then error(handle:error() or state) end
                return state=='completed' or state=='ready' or state=='finished'
            end, timeout)
        end
        coroutine = nil
    )lua");
}
LuaTasks::~LuaTasks() {
    cancel();
}
std::optional<sol::object> LuaTasks::clone_reload(const sol::object& value, sol::state_view target) {
    if (!value.is<Handle>())
        return {};
    auto result = std::make_shared<Result>();
    result->state = "cancelled";
    result->error = "task cancelled by reload; restart it in on_reload";
    result->cancelled = true;
    return sol::make_object(target, Handle{std::move(result)});
}
LuaTasks::Handle LuaTasks::run(sol::protected_function function) {
    if (_jobs.size() >= 1024)
        throw std::runtime_error("Lua task limit reached (1024)");
    auto job = std::make_unique<Job>();
    job->thread = sol::thread::create(_lua.lua_state());
    job->result = std::make_shared<Result>();
    job->owner = _owner;
    sol::stack::push(job->thread.thread_state(), function);
    Handle result{job->result};
    _jobs.push_back(std::move(job));
    return result;
}
void LuaTasks::cancel(std::string_view owner) {
    for (auto& job : _jobs)
        if (owner.empty() || job->owner == owner) {
            job->result->cancelled = true;
            job->result->state = "cancelled";
        }
}
bool LuaTasks::ready(Job& job, f32 dt) {
    if (job.waiting == "time") {
        job.seconds -= dt;
        return job.seconds <= 1e-7;
    }
    if (job.waiting == "frames")
        return --job.frames <= 0;
    if (job.waiting == "predicate") {
        LuaCall call{_runtime, _lua.lua_state(), "task.predicate"};
        sol::protected_function fn = job.value;
        auto result = fn();
        call.result(result.valid());
        if (!result.valid())
            throw sol::error(result);
        if (result.get_type() != sol::type::boolean)
            throw std::runtime_error("task predicate must return a boolean");
        if (result.get<bool>())
            return true;
        if (job.timeout >= 0) {
            job.timeout -= dt;
            if (job.timeout <= 1e-7)
                throw std::runtime_error("task wait timed out");
        }
        return false;
    }
    return true;
}
void LuaTasks::resume(Job& job) {
    auto* co = job.thread.thread_state();
    job.result->state = "running";
    LuaCall call{_runtime, co, "task.resume"};
    int count = 0;
    const int status = lua_resume(co, _lua.lua_state(), 0, &count);
    call.result(status == LUA_OK || status == LUA_YIELD);
    if (status != LUA_OK && status != LUA_YIELD) {
        const char* message = lua_tostring(co, -1);
        throw std::runtime_error(message ? message : "Lua task failed");
    }
    if (status == LUA_OK) {
        job.result->state = "completed";
        lua_settop(co, 0);
        return;
    }
    if (count < 2 || lua_type(co, -count) != LUA_TSTRING)
        throw std::runtime_error("task yielded an invalid wait");
    job.waiting = lua_tostring(co, -count);
    job.timeout = count >= 3 && lua_isnumber(co, -count + 2) ? lua_tonumber(co, -count + 2) : -1;
    if (job.waiting == "time")
        job.seconds = lua_tonumber(co, -count + 1);
    else if (job.waiting == "frames")
        job.frames = lua_tointeger(co, -count + 1);
    else if (job.waiting == "predicate")
        job.value = sol::stack::get<sol::object>(co, -count + 1);
    else
        throw std::runtime_error("task yielded an unknown wait");
    lua_pop(co, count);
    job.result->state = "waiting";
}
bool LuaTasks::advance(f32 dt, std::string& error, const std::function<bool(std::string_view)>& alive) {
    if (!std::isfinite(dt) || dt < 0) {
        error = "task step requires finite nonnegative dt";
        return false;
    }
    LuaCall step{_runtime, _lua.lua_state(), "task.step"};
    bool success = true;
    const auto owner = _owner;
    const auto count = _jobs.size(); // tasks spawned during this step start next step
    for (std::size_t i = 0; i < count; ++i) {
        auto& job = *_jobs[i]; // unique_ptr pointees survive vector growth
        if (job.result->cancelled || (alive && !job.owner.empty() && !alive(job.owner))) {
            job.result->state = "cancelled";
            continue;
        }
        try {
            _owner = job.owner;
            if (ready(job, dt)) {
                job.value = sol::object{};
                resume(job);
            }
            if (job.result->cancelled)
                job.result->state = "cancelled";
        } catch (const std::exception& e) {
            job.result->state = "failed";
            job.result->error = e.what();
            error = job.result->error;
            success = false;
        }
        if (step.exhausted()) {
            success = false;
            if (error.empty())
                error = "task step instruction budget exhausted";
            break;
        }
    }
    _owner = owner;
    std::erase_if(_jobs, [](const auto& job) {
        const auto& state = job->result->state;
        return state == "completed" || state == "cancelled" || state == "failed";
    });
    step.result(success);
    return success;
}
} // namespace kin
