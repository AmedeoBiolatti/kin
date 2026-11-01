#include <kin/core/script_debugger.hpp>

#include <utility>

namespace kin {

void ScriptDebugger::set_breakpoints(Breakpoints breakpoints) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::erase_if(breakpoints, [](const auto& file) { return file.second.empty(); });
    _breakpoints = std::make_shared<const Breakpoints>(std::move(breakpoints));
    _generation.fetch_add(1, std::memory_order_acq_rel);
    rearm();
}

std::shared_ptr<const ScriptDebugger::Breakpoints> ScriptDebugger::breakpoints() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _breakpoints;
}

void ScriptDebugger::pause() {
    std::lock_guard<std::mutex> lock(_mutex);
    _pause = true;
    rearm();
}

std::string_view ScriptDebugger::should_stop(std::string_view file, i32 line, u64 call, i32 depth) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_waiter) {
        return {};
    }
    if (const auto found = _breakpoints->find(file); found != _breakpoints->end() && found->second.contains(line)) {
        return "breakpoint";
    }
    if (_pause) {
        return "pause";
    }
    if (_step) {
        // A call of the host's other than the one stepped in: the function
        // stepped from has returned to the host, so any line ends the step.
        const bool same_call = call == _step_call;
        switch (*_step) {
        case ScriptResume::Into:
            return "step";
        case ScriptResume::Over:
            if (!same_call || depth <= _step_depth) {
                return "step";
            }
            break;
        case ScriptResume::Out:
            if (!same_call || depth < _step_depth) {
                return "step";
            }
            break;
        case ScriptResume::Continue:
            break;
        }
    }
    return {};
}

ScriptResume ScriptDebugger::stopped(ScriptStop stop, u64 call, i32 depth) {
    Waiter waiter;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _pause = false;
        _step.reset();
        if (!_waiter) {
            rearm();
            return ScriptResume::Continue;
        }
        stop.serial = ++_serial;
        _stop = std::move(stop);
        waiter = _waiter; // a copy: the host may set another while it waits
        rearm();
    }
    const ScriptResume how = waiter(*this);
    std::lock_guard<std::mutex> lock(_mutex);
    _stop.reset();
    if (how != ScriptResume::Continue && _waiter) {
        _step = how;
        _step_call = call;
        _step_depth = depth;
    }
    rearm();
    return how;
}

void ScriptDebugger::set_waiter(Waiter waiter) {
    std::lock_guard<std::mutex> lock(_mutex);
    _waiter = std::move(waiter);
    if (!_waiter) {
        _pause = false;
        _step.reset();
    }
    rearm();
}

bool ScriptDebugger::waiting() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return static_cast<bool>(_waiter);
}

std::optional<ScriptStop> ScriptDebugger::stop() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _stop;
}

void ScriptDebugger::rearm() {
    const bool seeking = _waiter && (_pause || _step);
    _seeking.store(seeking, std::memory_order_relaxed);
    _armed.store(seeking || (_waiter && !_breakpoints->empty()), std::memory_order_relaxed);
}

ScriptDebugger& script_debugger() {
    static ScriptDebugger debugger;
    return debugger;
}

} // namespace kin
