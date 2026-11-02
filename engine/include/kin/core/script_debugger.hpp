#pragma once

// Stopping scripts where a tool asks, to look at them: the breakpoints set
// (lines of script files), a pause asked for (the next line run), and the
// steps taken from a stop (into a call, over it, out of the function). What a
// stop shows is plain data, the language's own values told as text, so the
// tool on the other end of the scene server (a game run with --inspect) reads
// it as it reads the rest. kin's Lua scripts (LuaScript, and so EntityScripts)
// stop here; the language knows nothing of the tool, the tool nothing of Lua.
//
// One debugger for the process (script_debugger()). Nothing stops until a host
// waits for stops (set_waiter): a game run without a tool runs its scripts as
// it would without one. A script stops on its own thread, inside the waiter,
// which answers the tool until it says how to go on.

#include <kin/core/types.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

// A value as a stop shows it: "12.5", "\"text\"", "table (3)"; a table's fields
// under it (to a depth, and a count, a stop keeps small).
struct ScriptValue {
    std::string name;
    std::string value;
    std::string kind; // "nil", "boolean", "number", "string", "table", "function", "userdata", "thread"
    std::vector<ScriptValue> fields;
    bool more = false; // fields left out
};

// A function running at a stop: the innermost first.
struct ScriptFrame {
    std::string function; // its name where it is known, else "?"; "main chunk" for a file's own code
    std::string file;     // as breakpoints name it ("scripts/player.lua"); empty for a host's function
    i32 line = 0;         // 0 where not known
    std::vector<ScriptValue> locals;
    std::vector<ScriptValue> upvalues;
};

struct ScriptStop {
    std::string reason; // "breakpoint", "step", "pause"
    std::vector<ScriptFrame> frames;
    u64 serial = 0; // counts stops: a tool knows a new one from the one it read
};

// How a stop goes on.
enum class ScriptResume : u8 {
    Continue, // until the next breakpoint
    Into,     // the next line run, in a call if one is made
    Over,     // the next line of this function (or of the one it returns to)
    Out,      // the next line of the function this one returns to
};

class ScriptDebugger {
public:
    using Breakpoints = std::map<std::string, std::set<i32>, std::less<>>;

    // Every breakpoint, replacing those set before: lines by file (named
    // relative to the content root, '/' between folders).
    void set_breakpoints(Breakpoints breakpoints);
    std::shared_ptr<const Breakpoints> breakpoints() const;
    // Stop at the next line any script runs.
    void pause();

    // Whether a script stops anywhere (a waiter, and a breakpoint, a pause or
    // a step to stop for): a language checks lines only while it does.
    bool armed() const { return _armed.load(std::memory_order_relaxed); }
    // Whether any line may stop (a pause or a step being taken), not only
    // those breakpoints() name.
    bool seeking() const { return _seeking.load(std::memory_order_relaxed); }
    // Bumped when the breakpoints change.
    u64 generation() const { return _generation.load(std::memory_order_acquire); }

    // What the language asks at each line while armed: whether to stop at
    // `line` of `file` (a breakpoint there, a pause, or a step ending). By
    // `call` (which of the host's calls into a script runs it: each call its
    // own number) and `depth` (the functions running in that call) a step
    // knows the function it is in, and when that has returned to the host.
    // The reason, empty if not.
    std::string_view should_stop(std::string_view file, i32 line, u64 call, i32 depth);
    // A script stopped: hands `stop` to the waiter and holds until it says how
    // to go on (Continue at once with no waiter). `call` and `depth` as above.
    ScriptResume stopped(ScriptStop stop, u64 call, i32 depth);

    // The host's: answers the tool while a script is stopped, returning how it
    // goes on. Empty: none (nothing stops; breakpoints set are kept).
    using Waiter = std::function<ScriptResume(ScriptDebugger&)>;
    void set_waiter(Waiter waiter);
    bool waiting() const;

    // The stop a script is held at (none while running).
    std::optional<ScriptStop> stop() const;

private:
    void rearm(); // _mutex held

    mutable std::mutex _mutex;
    std::shared_ptr<const Breakpoints> _breakpoints = std::make_shared<Breakpoints>();
    Waiter _waiter;
    bool _pause = false;
    // A step being taken: until it ends at a line.
    std::optional<ScriptResume> _step;
    u64 _step_call = 0;
    i32 _step_depth = 0;
    std::optional<ScriptStop> _stop;
    u64 _serial = 0;
    std::atomic<bool> _armed{false};
    std::atomic<bool> _seeking{false};
    std::atomic<u64> _generation{0};
};

// The process's debugger.
ScriptDebugger& script_debugger();

} // namespace kin
