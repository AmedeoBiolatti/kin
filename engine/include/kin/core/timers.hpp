#pragma once

// Things to do later: once after a while, or every so often, by the time a
// game moves on (paused when it is not updated, as fast as it is).
//
//     kin::Timers timers;
//     timers.after(0.5f, [&] { door.open(); });
//     const kin::u32 clock = timers.every(1.0f, [&] { ++seconds; });
//     ...each frame
//     timers.update(dt);
//     ...later
//     timers.cancel(clock);
//
// What falls due in an update is done in the order it fell due (an `every` as
// many times as it did, up to a limit). An action may start and cancel timers,
// its own too.

#include <kin/core/types.hpp>

#include <algorithm>
#include <functional>
#include <vector>

namespace kin {

class Timers {
public:
    // Done once, `seconds` from now; its id.
    u32 after(f32 seconds, std::function<void()> action) { return add(seconds, 0.0f, std::move(action)); }
    // Done every `seconds` (the first time `seconds` from now) until cancelled.
    u32 every(f32 seconds, std::function<void()> action) { return add(seconds, std::max(seconds, min_interval), std::move(action)); }
    bool cancel(u32 id) {
        return std::erase_if(_entries, [&](const Entry& entry) { return entry.id == id; }) > 0;
    }
    bool pending(u32 id) const {
        return std::ranges::any_of(_entries, [&](const Entry& entry) { return entry.id == id; });
    }
    std::size_t size() const { return _entries.size(); }
    void clear() { _entries.clear(); }
    f64 now() const { return _now; } // seconds of updates so far

    void update(f32 dt) {
        if (!(dt > 0.0f)) {
            return;
        }
        _now += dt;
        for (i32 done = 0; done < max_per_update; ++done) {
            const auto due = std::ranges::min_element(_entries, [](const Entry& a, const Entry& b) {
                return a.at != b.at ? a.at < b.at : a.id < b.id;
            });
            if (due == _entries.end() || due->at > _now) {
                return;
            }
            std::function<void()> action;
            if (due->interval > 0.0f) {
                due->at += due->interval;
                action = due->action; // it stays
            } else {
                action = std::move(due->action);
                _entries.erase(due);
            }
            action();
        }
    }

private:
    static constexpr f32 min_interval = 1e-3f;
    static constexpr i32 max_per_update = 10000;

    struct Entry {
        u32 id = 0;
        f64 at = 0.0;
        f32 interval = 0.0f; // an every's
        std::function<void()> action;
    };

    u32 add(f32 seconds, f32 interval, std::function<void()> action) {
        const u32 id = _next_id++;
        _entries.push_back({.id = id, .at = _now + std::max(seconds, 0.0f), .interval = interval, .action = std::move(action)});
        return id;
    }

    std::vector<Entry> _entries;
    f64 _now = 0.0;
    u32 _next_id = 1;
};

} // namespace kin
