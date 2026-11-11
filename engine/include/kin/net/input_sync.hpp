#pragma once

// Deterministic input sync (docs/networking.md): only each player's commands cross the network, and every peer runs
// the whole simulation from them. One session covers lockstep (max_prediction = 0: a tick runs once every command
// for it has arrived) and rollback (max_prediction > 0: missing commands are predicted, and a tick whose prediction
// was wrong is played again from a saved state when the real command arrives).
//
//     summit::Game game(seed);
//     kin::net::InputSync<summit::Game, summit::PlayerCommand> session(game, transport, {
//         .players = 2, .local_player = 0, .peers = {0, host_peer},
//         .input_delay = 2, .max_prediction = 8, .game = kin::net::compatibility_hash("summit", "0.3")});
//     // each rendered frame:
//     session.set_local_input(mapper.command());
//     session.update(now_us);          // steps `game` as many ticks as are due (and again after a rollback)
//     view.draw(game, session.alpha());
//
// Peers start together once every handshake is done; the first input_delay ticks have the default command for every
// player. Commands are levels (held or not), not edges: a step that wants "pressed" compares with the previous
// tick's command, which it keeps in its state. The peers' clocks are kept within about a tick of each other: the
// one ahead runs a little slower until the other catches up.

#include <array>
#include <deque>
#include <functional>
#include <kin/net/session.hpp>
#include <kin/net/sim.hpp>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace kin::net {

// The model's id in a SessionIdentity: 'I' 'S' 'Y' 'N'.
inline constexpr u32 input_sync_model = 0x4e595349;

struct InputSyncConfig {
    u32 players = 2;      // 1 to max_players
    u32 local_player = 0; // this peer's slot
    // peers[slot] is the transport peer that plays `slot`; the local slot's entry is ignored.
    std::vector<PeerId> peers;
    u32 tick_rate = 60;
    u32 input_delay = 2;    // ticks between sampling a local command and the tick it applies to
    u32 max_prediction = 8; // ticks run past the last tick with every real command; 0 is lockstep
    u32 hash_interval = 30; // compare a state hash with the peers every this many ticks; 0 never
    u32 max_catchup = 8;    // most ticks one update() runs after a hitch
    u64 game = 0;           // SessionIdentity::game, e.g. compatibility_hash("my_game", "1.2")
    u64 session = 0;        // SessionIdentity::session
    LinksConfig links;      // timings; its local_slot is set from local_player
    // Players may join while the game runs (docs/networking.md#joining-a-game-in-progress): a slot whose peers
    // entry is no_peer starts empty (its command is the default one), and allow_join() lets a player take it. A
    // joiner starts from a saved state, so this needs a simulation with save() and restore(). Must match on every
    // peer.
    bool joinable = false;
    // This peer joins a game already running (joinable too): `peers` names the players in it, no_peer for empty
    // slots. One of them sends the state; this peer catches up as a spectator, then plays from a tick the game
    // agrees on (StepInfo::joined says which).
    bool joining = false;
};

inline constexpr u32 max_players = 16;
inline constexpr u32 max_input_delay = 30;
inline constexpr u32 max_max_prediction = 60;

struct InputSyncStats {
    u32 tick = 0;            // the next tick to run
    i64 confirmed_tick = -1; // the last tick with every player's real command
    u64 rollbacks = 0;       // times a wrong prediction took the state back
    u64 resimulated_ticks = 0;
    u32 max_rollback = 0;   // the deepest one, in ticks
    u64 waits = 0;          // updates that could not run a due tick (lockstep, or out of prediction)
    u64 slowed_updates = 0; // updates whose clock ran slow to let the peers catch up
    u64 hashes_compared = 0;
    f32 advantage = 0.0f;    // ticks this peer is ahead of the furthest-behind one (smoothed)
    u32 present = 0;         // the slots in the game at the current tick, a bit each
    u32 joins = 0;           // players who joined while this peer played (itself too, for a joiner)
    u32 start_tick = 0;      // this peer's first tick: 0, or for a joiner the tick of the state it was sent
    bool spectating = false; // a joiner that has the state and follows the game, before it plays
    u64 state_bytes_sent = 0, state_bytes_received = 0;
};

struct DesyncReport {
    u32 tick = 0; // the state after this tick differs
    u32 player = 0;
    u64 local_hash = 0;
    u64 remote_hash = 0;
};

// The machinery on bytes, under the typed InputSync below. A game uses InputSync; this is what it adapts.
class InputSyncHooks {
public:
    virtual ~InputSyncHooks() = default;
    // `commands` holds players * input_size bytes, player 0 first.
    virtual void step(std::span<const u8> commands, const StepInfo& info) = 0;
    virtual void save(std::vector<u8>& out) = 0;
    virtual bool restore(std::span<const u8> bytes) = 0;
    virtual u64 state_hash() = 0;
};

class InputSyncCore final : public Session {
public:
    // `default_command` is the encoded command of a player who has not sent one (its size is every command's size).
    // Fails (status() == failed) for settings out of range, or for max_prediction > 0 if `can_rollback` is false.
    InputSyncCore(Transport& transport, InputSyncHooks& hooks, const InputSyncConfig& config,
                  std::span<const u8> default_command, bool can_rollback);
    ~InputSyncCore() override;

    // The command for the next tick that has none yet (it applies input_delay ticks after the current one). The
    // latest value is used for every tick an update() schedules.
    void set_local_input(std::span<const u8> command);
    // Called once per tick, in order, when every player's real command for it is known: a replay log. A joiner's
    // starts at its first tick.
    void set_on_confirmed(std::function<void(u32 tick, std::span<const u8> commands)> on_confirmed);
    // Lets the player at transport peer `peer` join empty slot `slot` (InputSyncConfig::joinable). Every peer in
    // the game must allow it: the joiner needs each one's commands. False for a slot that is taken (or is this
    // peer's), or a session that is not joinable. A joiner that never comes, or goes before it plays, leaves the
    // slot empty again; allowing another player the slot meanwhile replaces them.
    bool allow_join(u32 slot, PeerId peer);

    void update(u64 now_us) override;
    SessionStatus status() const override { return _status; }
    std::string status_detail() const override;
    std::span<const LinkInfo> links() const override { return _links.links(); }
    void leave() override;

    const InputSyncConfig& config() const { return _config; }
    const InputSyncStats& stats() const { return _stats; }
    // The first state hash that differed, when status() is desynced.
    const DesyncReport& desync() const { return _desync; }
    // How far the clock is into the next tick, 0 to 1: for interpolating a view between the last two states.
    f32 alpha() const;

private:
    struct PeerTrack {
        std::size_t link = 0;
        i64 acked = -1; // the last of our commands the peer has
        i64 remote_tick = -1;
        u64 remote_tick_at_us = 0;
        f32 reported_advantage = 0.0f; // how far the peer thinks it is ahead of us
        u64 last_sent_us = 0;
        i64 last_ack_sent = -2;
        std::deque<std::pair<u32, u64>> hashes; // the peer's hashes not yet compared
        bool linked = false;                    // the slot has a link (a player in the game, or one joining)
    };
    struct JoinEvent {
        u32 tick = 0;
        u32 slot = 0;
    };
    // The state on its way to a joiner (this peer is the game's host).
    struct Transfer {
        u32 slot = 0;
        u32 base = 0;    // the state is from before this tick
        u32 present = 0; // the slots in the game at `base`
        std::vector<u8> bytes;
        u32 acked = 0; // bytes the joiner has, from the start
        u32 next = 0;  // the next byte to send
        u64 progress_us = 0;
    };

    bool start_failed(std::string detail);
    void on_message(std::size_t link, u8 type, ByteReader& payload);
    void on_inputs(u32 slot, ByteReader& payload);
    void on_hash(u32 slot, ByteReader& payload);
    void schedule_local(u32 tick);
    bool can_step(u32 tick);
    void step_tick(u32 tick, bool resimulating);
    void roll_back();
    void confirm();
    void compare_hashes();
    void advance(u64 elapsed_us);
    void send_inputs();
    void on_join_request(u32 slot);
    void on_join_state(u32 slot, ByteReader& payload);
    void on_join_ack(u32 slot, ByteReader& payload);
    void on_join_ready(u32 slot);
    void apply_join(u32 slot, u32 tick);
    void start_from_state();
    void serve_transfers();
    void send_join_messages();
    void drop_failed_joiners();
    u32 catch_up();
    bool in_game(u32 slot) const { return _active_from[slot] != never; }
    bool present(u32 slot, u32 tick) const { return _active_from[slot] <= static_cast<i64>(tick); }
    u32 present_mask(u32 tick) const;
    u32 host() const;
    f32 remote_now(u32 slot) const;
    f32 own_view(u32 slot) const;
    f32 measure_advantage() const;

    std::span<u8> input(u32 player, u32 tick);
    i64 confirmed_tick() const;

    InputSyncHooks& _hooks;
    InputSyncConfig _config;
    u32 _input_size;
    bool _can_rollback;
    Links _links;
    SessionStatus _status = SessionStatus::connecting;
    std::string _failure;
    DesyncReport _desync;
    InputSyncStats _stats;

    // Commands: a ring of `ring_ticks` per player. _known[p] is the last tick with p's real command; ticks after
    // it that already ran hold the predicted command they ran with.
    static constexpr u32 ring_ticks = 256;
    std::vector<u8> _inputs;
    std::vector<u32> _input_tick; // which tick each slot of the ring holds
    std::vector<u8> _predicted;   // the slot holds a prediction a tick ran with
    std::vector<i64> _known;
    std::vector<u8> _default_command;
    std::vector<u8> _local_input;
    std::vector<u8> _step_buffer;
    std::vector<PeerTrack> _peers; // remote players, by slot (the local slot's entry is unused)
    // The tick each slot's player is in the game from (never: an empty slot).
    static constexpr i64 never = std::numeric_limits<i64>::max();
    std::vector<i64> _active_from;
    std::vector<JoinEvent> _created; // joins this peer decided as the host, sent with its commands
    std::vector<Transfer> _transfers;
    // A joiner's incoming state.
    bool _have_state = false;
    bool _state_started = false;
    u32 _state_from = 0;
    u32 _state_base = 0;
    u32 _state_present = 0;
    u32 _state_total = 0;
    std::vector<u8> _state;
    std::map<u32, std::vector<u8>> _state_early; // chunks that came before one that was lost, by offset
    u64 _next_join_message_us = 0;

    // A saved state before every tick that ran on a prediction, in a ring of max_prediction + 2.
    std::vector<std::vector<u8>> _snapshots;
    std::vector<i64> _snapshot_tick;

    u32 _tick = 0;
    i64 _rollback_to = -1;
    i64 _reported = -1; // the last tick given to on_confirmed
    std::function<void(u32, std::span<const u8>)> _on_confirmed;
    std::deque<std::pair<u32, u64>> _pending_hashes; // ran, not yet confirmed
    std::deque<std::pair<u32, u64>> _local_hashes;   // confirmed, kept to compare with late peers

    u64 _now_us = 0;
    u64 _last_update_us = 0;
    u64 _accumulator_us = 0;
    u64 _tick_us = 0;
    bool _input_changed = false;
};

// InputSync for a game's own simulation and command types: what a game uses. `Sim` must be a DeterministicSim; with
// max_prediction > 0 it must also be a RollbackSim (else the session fails to start and says so).
template <class Sim, class Command>
    requires DeterministicSim<Sim, Command>
class InputSync final : public Session {
public:
    static constexpr bool can_rollback = RollbackSim<Sim, Command>;
    static constexpr std::size_t command_size = CommandCodec<Command>::size;

    InputSync(Sim& sim, Transport& transport, const InputSyncConfig& config)
        : _adapter(sim, config.players), _core(transport, _adapter, config, encode(Command{}), can_rollback) {}

    void set_local_input(const Command& command) { _core.set_local_input(encode(command)); }
    // Every confirmed tick's commands, in order: what a replay needs.
    void set_on_confirmed(std::function<void(u32 tick, std::span<const Command> commands)> on_confirmed) {
        if (!on_confirmed) {
            _core.set_on_confirmed({});
            return;
        }
        _core.set_on_confirmed([this, on_confirmed = std::move(on_confirmed)](u32 tick, std::span<const u8> bytes) {
            _adapter.decode(bytes, _confirmed);
            on_confirmed(tick, _confirmed);
        });
    }

    void update(u64 now_us) override { _core.update(now_us); }
    SessionStatus status() const override { return _core.status(); }
    std::string status_detail() const override { return _core.status_detail(); }
    std::span<const LinkInfo> links() const override { return _core.links(); }
    void leave() override { _core.leave(); }
    bool allow_join(u32 slot, PeerId peer) { return _core.allow_join(slot, peer); }

    const InputSyncConfig& config() const { return _core.config(); }
    const InputSyncStats& stats() const { return _core.stats(); }
    const DesyncReport& desync() const { return _core.desync(); }
    f32 alpha() const { return _core.alpha(); }

private:
    static std::array<u8, command_size> encode(const Command& command) {
        std::array<u8, command_size> bytes{};
        CommandCodec<Command>::write(command, std::span<u8, command_size>(bytes));
        return bytes;
    }

    class Adapter final : public InputSyncHooks {
    public:
        Adapter(Sim& sim, u32 players) : _sim(sim), _commands(players) {}

        void decode(std::span<const u8> bytes, std::vector<Command>& out) const {
            out.resize(bytes.size() / command_size);
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = CommandCodec<Command>::read(
                    std::span<const u8, command_size>(bytes.subspan(i * command_size, command_size)));
            }
        }
        void step(std::span<const u8> commands, const StepInfo& info) override {
            decode(commands, _commands);
            sim_detail::step<Sim, Command>(_sim, _commands, info);
        }
        void save(std::vector<u8>& out) override {
            if constexpr (can_rollback) {
                sim_detail::save(_sim, out);
            }
        }
        bool restore(std::span<const u8> bytes) override {
            if constexpr (can_rollback) {
                return _sim.restore(bytes);
            } else {
                return false;
            }
        }
        u64 state_hash() override { return static_cast<u64>(_sim.state_hash()); }

    private:
        Sim& _sim;
        std::vector<Command> _commands;
    };

    Adapter _adapter;
    InputSyncCore _core;
    std::vector<Command> _confirmed;
};

} // namespace kin::net
