#pragma once

// The rollback sync test (docs/networking.md): checks, without any network, that save() and restore() capture
// everything step() depends on. It plays `ticks` ticks with the commands `commands(tick, player)` gives and, after
// every tick, takes the state back `depth` ticks and plays them again: each tick run again must give the same state
// hash as its first run. State that save() leaves out (a counter, a cache, an RNG kept elsewhere) shows up as the
// first tick that differs.
//
//     const auto result = kin::net::sync_test<Game, Command>(game, 2, 600, 7, [](u32 tick, u32 player) { ... });
//     if (!result.ok) std::println("{}", result.detail);

#include <kin/net/sim.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace kin::net {

struct SyncTestResult {
    bool ok = true;
    u32 ticks = 0;    // ticks played (the first runs)
    u32 bad_tick = 0; // the first tick whose second run differed
    u64 expected = 0; // its hash the first time
    u64 actual = 0;   // and after the restore
    std::string detail;
};

template <class Sim, class Command>
    requires RollbackSim<Sim, Command>
SyncTestResult sync_test(Sim& sim, u32 players, u32 ticks, u32 depth,
                         const std::function<Command(u32 tick, u32 player)>& commands) {
    SyncTestResult result;
    depth = std::max<u32>(depth, 1);
    // states[t % ring]: the state before tick t; hashes[t % ring]: the hash after it.
    const u32 ring = depth + 1;
    std::vector<std::vector<u8>> states(ring);
    std::vector<u64> hashes(ring);
    std::vector<Command> step_commands(players);
    const auto fill = [&](u32 tick) {
        for (u32 player = 0; player < players; ++player) {
            step_commands[player] = commands(tick, player);
        }
    };
    const auto fail = [&](u32 tick, u64 expected, u64 actual, std::string what) {
        result.ok = false;
        result.bad_tick = tick;
        result.expected = expected;
        result.actual = actual;
        result.detail = std::move(what);
        return result;
    };
    for (u32 tick = 0; tick < ticks; ++tick) {
        sim_detail::save(sim, states[tick % ring]);
        const u64 before = static_cast<u64>(sim.state_hash());
        if (!sim.restore(states[tick % ring])) {
            return fail(tick, before, 0,
                        "restore() refused the state save() just made, before tick " + std::to_string(tick));
        }
        if (const u64 restored = static_cast<u64>(sim.state_hash()); restored != before) {
            return fail(tick, before, restored,
                        "a restore changed the state hash before tick " + std::to_string(tick) +
                            ": save() misses state that state_hash() covers");
        }
        fill(tick);
        sim_detail::step<Sim, Command>(sim, step_commands, StepInfo{.tick = tick});
        hashes[tick % ring] = static_cast<u64>(sim.state_hash());
        result.ticks = tick + 1;
        if (tick + 1 < depth) {
            continue;
        }
        const u32 from = tick + 1 - depth;
        if (!sim.restore(states[from % ring])) {
            return fail(from, 0, 0, "restore() refused the saved state of tick " + std::to_string(from));
        }
        for (u32 again = from; again <= tick; ++again) {
            fill(again);
            sim_detail::step<Sim, Command>(sim, step_commands, StepInfo{.tick = again, .resimulating = true});
            if (const u64 hash = static_cast<u64>(sim.state_hash()); hash != hashes[again % ring]) {
                return fail(again, hashes[again % ring], hash,
                            "tick " + std::to_string(again) + " gave another state when run again from tick " +
                                std::to_string(from) + ": save() misses state that step() reads");
            }
        }
    }
    return result;
}

} // namespace kin::net
