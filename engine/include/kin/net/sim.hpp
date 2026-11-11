#pragma once

// What a game's simulation provides to be networked (docs/networking.md). The capabilities build on each other, and
// each sync model asks for the ones it needs, so a game that picks a model it cannot run fails to compile with the
// missing capability named:
//
//     Simulation        step(std::span<const Command>): one fixed tick, one command per player
//     DeterministicSim  + state_hash() const: the same commands give the same bits on every peer (lockstep)
//     RollbackSim       + save() const and restore(bytes): a state can be taken back (rollback)
//
// A step may also take a `const StepInfo&` second argument, to tell first runs of a tick from runs again after a
// rollback (for sounds and effects) and ticks of real inputs from predicted ones. save_into(std::vector<u8>&) const,
// if the game has it, is used instead of save() so that a snapshot every tick reuses its buffer.

#include <concepts>
#include <cstring>
#include <kin/core/types.hpp>
#include <span>
#include <type_traits>
#include <vector>

namespace kin::net {

// How a command crosses the wire: `size` bytes, written and read back exactly (the same bytes on every peer, since
// prediction compares them). Plain data without padding or floats gets a byte copy. Anything else specializes
// CommandCodec, which is also where analog values are quantized:
//
//     template <> struct kin::net::CommandCodec<summit::PlayerCommand> {
//         static constexpr std::size_t size = 6;
//         static void write(const summit::PlayerCommand& c, std::span<u8, size> out);
//         static summit::PlayerCommand read(std::span<const u8, size> in);
//     };
template <class Command> struct CommandCodec;

template <class Command>
    requires std::is_trivially_copyable_v<Command> && std::has_unique_object_representations_v<Command>
struct CommandCodec<Command> {
    static constexpr std::size_t size = sizeof(Command);
    static void write(const Command& command, std::span<u8, size> out) { std::memcpy(out.data(), &command, size); }
    static Command read(std::span<const u8, size> in) {
        Command command;
        std::memcpy(&command, in.data(), size);
        return command;
    }
};

template <class Command>
concept NetCommand = std::default_initializable<Command> && std::copyable<Command> &&
                     requires(const Command& command, std::span<u8, CommandCodec<Command>::size> out,
                              std::span<const u8, CommandCodec<Command>::size> in) {
                         { CommandCodec<Command>::size } -> std::convertible_to<std::size_t>;
                         CommandCodec<Command>::write(command, out);
                         { CommandCodec<Command>::read(in) } -> std::same_as<Command>;
                     };

struct StepInfo {
    u32 tick = 0;
    // The tick runs again after a rollback: it was shown before, so its sounds and effects have already played.
    bool resimulating = false;
    // Every player's command is real (none is predicted): this run of the tick is final.
    bool confirmed = true;
    // The player slots in the game at this tick, a bit each (slot 0 is bit 0). An empty slot's command is the
    // default one. Slots are empty in a joinable session (InputSyncConfig::joinable) and after a player leaves.
    u32 present = ~0u;
    // The slots whose player joins at this tick: present from now on (a game spawns them here).
    u32 joined = 0;
    // The slots whose player is gone from this tick on (they quit, or their connection was lost): a game removes
    // them here, or keeps their character for them to come back to.
    u32 left = 0;
};

template <class Sim, class Command>
concept StepsWithInfo =
    requires(Sim& sim, std::span<const Command> commands, const StepInfo& info) { sim.step(commands, info); };

template <class Sim, class Command>
concept Simulation =
    NetCommand<Command> &&
    (StepsWithInfo<Sim, Command> || requires(Sim& sim, std::span<const Command> commands) { sim.step(commands); });

template <class Sim, class Command>
concept DeterministicSim = Simulation<Sim, Command> && requires(const Sim& sim) {
    { sim.state_hash() } -> std::convertible_to<u64>;
};

template <class Sim>
concept SavesInto = requires(const Sim& sim, std::vector<u8>& out) { sim.save_into(out); };

template <class Sim, class Command>
concept RollbackSim = DeterministicSim<Sim, Command> && (SavesInto<Sim> || requires(const Sim& sim) {
                          { sim.save() } -> std::convertible_to<std::vector<u8>>;
                      }) && requires(Sim& sim, std::span<const u8> bytes) {
    { sim.restore(bytes) } -> std::convertible_to<bool>;
};

namespace sim_detail {

template <class Sim, class Command> void step(Sim& sim, std::span<const Command> commands, const StepInfo& info) {
    if constexpr (StepsWithInfo<Sim, Command>) {
        sim.step(commands, info);
    } else {
        sim.step(commands);
    }
}

template <class Sim> void save(const Sim& sim, std::vector<u8>& out) {
    if constexpr (SavesInto<Sim>) {
        out.clear();
        sim.save_into(out);
    } else {
        out = sim.save();
    }
}

} // namespace sim_detail

} // namespace kin::net
