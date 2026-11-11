#include <algorithm>
#include <cstring>
#include <kin/core/hash.hpp>
#include <kin/net/input_sync.hpp>
#include <limits>

namespace kin::net {
namespace {

// Model messages (after the link header).
//   inputs: u8 sender slot, u32 sender's next tick, i16 how far the sender thinks it is ahead of the receiver
//           (1/256 ticks), u32 the next of the receiver's ticks the sender lacks, u32 the leave decisions the sender
//           applied, u32 first tick, u8 count, count * input_size bytes (the sender's commands from the first tick on)
//           then u8 joins, joins * (u32 tick, u8 slot, u64 joiner's id): the joins the sender decided at the ticks
//           those commands are for (a receiver takes a join with the sender's command for its tick, so every peer
//           applies it there)
//   hash:   u8 sender slot, u32 tick, u64 state hash after that tick (confirmed)
//   join request (joiner -> every peer; the host answers): u8 joiner's slot, u64 joiner's id
//   join state (host -> joiner): u8 joiner's slot, u32 base tick, u32 slots present before it, u32 leave decisions
//           the state has seen, u32 total bytes, u32 offset, the bytes from there
//   join ack (joiner -> host): u8 joiner's slot, u32 bytes received from the start
//   join ready (joiner -> every peer, once it has caught up; the host decides): u8 joiner's slot, u64 joiner's id
//   lost (every peer that lost a player -> every peer, repeated until the game let them go): u8 sender slot, u32
//           leave decisions applied, u32 lost players not yet decided on, u32 lost players, then for each lost
//           player (lowest slot first) i32 the last tick the sender has their command for
//   relay (to a peer that has fewer of a lost player's commands): u8 the lost player's slot, u32 first tick, u8
//           count, count * input_size bytes, u8 joins, joins * (u32 tick, u8 slot, u64 id) the lost player decided
//           at those ticks
//   leave (the decider -> every peer; any peer -> one that lacks it): u32 the decision's number, u32 the players who
//           leave, then for each of them (lowest slot first) u32 the tick they are gone from
constexpr u8 inputs_message = first_model_message;
constexpr u8 hash_message = first_model_message + 1;
constexpr u8 join_request_message = first_model_message + 2;
constexpr u8 join_state_message = first_model_message + 3;
constexpr u8 join_ack_message = first_model_message + 4;
constexpr u8 join_ready_message = first_model_message + 5;
constexpr u8 lost_message = first_model_message + 6;
constexpr u8 relay_message = first_model_message + 7;
constexpr u8 leave_message = first_model_message + 8;
constexpr std::size_t inputs_header = 1 + 4 + 2 + 4 + 4 + 4 + 1;
// Room for the joins one message carries: those decided at the ticks of its commands, at most one a slot (a slot
// whose player leaves is taken again only after its leave, which takes longer than a message's ticks).
constexpr std::size_t join_entry = 4 + 1 + 8;
constexpr std::size_t join_trailer = 1 + join_entry * (max_players - 1);
constexpr std::size_t join_state_header = 1 + 4 + 4 + 4 + 4 + 4;
constexpr std::size_t relay_header = 1 + 4 + 1;
// Leaving: how often a peer that lost a player says so, and how many decisions each keeps to send a peer that missed
// them.
constexpr u64 lost_repeat_us = 100'000;
constexpr std::size_t kept_decisions = 32;

// Joining: how often requests and readiness are repeated, how many state bytes may be in flight, how long without
// progress before the host sends again from the last acknowledged byte, and how many ticks a joiner runs in one
// update while it catches up.
constexpr u64 join_repeat_us = 100'000;
constexpr u32 join_window_chunks = 16;
constexpr u64 join_resend_us = 60'000;
constexpr u32 join_catchup_ticks = 120;
// The largest state a joiner accepts (the size comes from the wire), and what it reserves before the bytes arrive.
constexpr u64 max_join_state = 256ull << 20;
constexpr u64 join_state_reserve = 1ull << 20;

// A peer that hears nothing new still sends this often, so acks and its clock keep flowing.
constexpr u64 keepalive_us = 50'000;
constexpr std::size_t kept_hashes = 64;

u64 config_hash(const InputSyncConfig& config, u32 input_size) {
    const u32 fields[] = {config.players,
                          input_size,
                          config.tick_rate,
                          config.input_delay,
                          config.hash_interval,
                          config.joinable ? 1u : 0u,
                          config.players_may_leave ? 1u : 0u};
    return hash_bytes(fields, sizeof(fields));
}

} // namespace

std::string_view session_status_name(SessionStatus status) {
    switch (status) {
    case SessionStatus::connecting:
        return "connecting";
    case SessionStatus::running:
        return "running";
    case SessionStatus::desynced:
        return "desynced";
    case SessionStatus::peer_lost:
        return "peer lost";
    case SessionStatus::rejected:
        return "rejected";
    case SessionStatus::ended:
        return "ended";
    case SessionStatus::failed:
        return "failed";
    }
    return "unknown";
}

InputSyncCore::InputSyncCore(Transport& transport, InputSyncHooks& hooks, const InputSyncConfig& config,
                             std::span<const u8> default_command, bool can_rollback)
    : _hooks(hooks), _config(config), _input_size(static_cast<u32>(default_command.size())),
      _can_rollback(can_rollback),
      _links(transport,
             SessionIdentity{.model = input_sync_model,
                             .game = config.game,
                             .config = config_hash(config, static_cast<u32>(default_command.size())),
                             .session = config.session},
             [&] {
                 LinksConfig links = config.links;
                 links.local_slot = config.local_player;
                 return links;
             }()) {
    const u32 players = _config.players;
    if (players < 1 || players > max_players) {
        start_failed("players must be 1 to " + std::to_string(max_players));
        return;
    }
    if (_config.local_player >= players) {
        start_failed("local_player must be below players");
        return;
    }
    if (_config.joining && !_config.joinable) {
        start_failed("joining needs a joinable session");
        return;
    }
    if (_config.joinable && !_can_rollback) {
        start_failed("joining needs a simulation with save() and restore(): a joiner starts from a saved state");
        return;
    }
    if ((players > 1 || _config.joinable) && _config.peers.size() != players) {
        start_failed("peers must name one transport peer per player slot");
        return;
    }
    if (_config.tick_rate < 1 || _config.tick_rate > 1000) {
        start_failed("tick_rate must be 1 to 1000");
        return;
    }
    if (_config.input_delay > max_input_delay || _config.max_prediction > max_max_prediction) {
        start_failed("input_delay must be at most " + std::to_string(max_input_delay) + " and max_prediction at most " +
                     std::to_string(max_max_prediction));
        return;
    }
    if (_config.max_prediction > 0 && !_can_rollback) {
        start_failed("max_prediction > 0 (rollback) needs a simulation with save() and restore(); use 0 (lockstep)");
        return;
    }
    const std::size_t header = inputs_header + (_config.joinable ? join_trailer : 0);
    if (_input_size == 0 || header + _input_size > _links.max_payload()) {
        start_failed("a command must be 1 to " +
                     std::to_string(_links.max_payload() > header ? _links.max_payload() - header : 0) + " bytes");
        return;
    }
    _tick_us = 1'000'000 / _config.tick_rate;
    _inputs.assign(static_cast<std::size_t>(players) * ring_ticks * _input_size, 0);
    _input_tick.assign(static_cast<std::size_t>(players) * ring_ticks, std::numeric_limits<u32>::max());
    _predicted.assign(static_cast<std::size_t>(players) * ring_ticks, 0);
    _known.assign(players, static_cast<i64>(_config.input_delay) - 1);
    _default_command.assign(default_command.begin(), default_command.end());
    _local_input = _default_command;
    _step_buffer.assign(static_cast<std::size_t>(players) * _input_size, 0);
    // Every player's first input_delay ticks have the default command, and every peer knows it.
    for (u32 player = 0; player < players; ++player) {
        for (u32 tick = 0; tick < _config.input_delay; ++tick) {
            std::ranges::copy(_default_command, input(player, tick).begin());
            _input_tick[player * ring_ticks + tick % ring_ticks] = tick;
        }
    }
    _peers.resize(players);
    _active_from.assign(players, never);
    _active_until.assign(players, never);
    if (!_config.joining) {
        _active_from[_config.local_player] = 0;
    }
    for (u32 slot = 0; slot < players; ++slot) {
        if (slot == _config.local_player) {
            continue;
        }
        if (_config.peers[slot] == no_peer) {
            if (_config.joinable) {
                continue; // an empty slot
            }
            start_failed("no transport peer for player " + std::to_string(slot));
            return;
        }
        // A joiner's links are optional until the state says who is in the game.
        _peers[slot].link = _links.add(_config.peers[slot], slot, _config.joining);
        if (_peers[slot].link == Links::no_link) {
            start_failed("player " + std::to_string(slot) + " has the same transport peer as another player");
            return;
        }
        _peers[slot].linked = true;
        _peers[slot].acked = static_cast<i64>(_config.input_delay) - 1;
        if (!_config.joining) {
            _active_from[slot] = 0;
        }
    }
    if (_config.joining) {
        // Nothing of ours is in the game yet: our commands start at the tick we join.
        _join_id = random_id();
        _known[_config.local_player] = -1;
        for (PeerTrack& peer : _peers) {
            peer.acked = -1;
        }
    }
    _stats.present = present_mask(0);
    if (_config.max_prediction > 0) {
        _snapshots.resize(_config.max_prediction + 2);
        _snapshot_tick.assign(_config.max_prediction + 2, -1);
    }
}

InputSyncCore::~InputSyncCore() = default;

bool InputSyncCore::start_failed(std::string detail) {
    _status = SessionStatus::failed;
    _failure = std::move(detail);
    return false;
}

std::span<u8> InputSyncCore::input(u32 player, u32 tick) {
    const std::size_t slot = static_cast<std::size_t>(player) * ring_ticks + tick % ring_ticks;
    return {_inputs.data() + slot * _input_size, _input_size};
}

i64 InputSyncCore::confirmed_tick() const {
    i64 confirmed = never;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        // A player who left counts until this peer has all their commands from before they went.
        const bool missing = _active_until[slot] != never && _known[slot] < _active_until[slot] - 1;
        if (in_game(slot) || missing) {
            confirmed = std::min(confirmed, _known[slot]);
        }
    }
    return confirmed == never ? -1 : confirmed;
}

bool InputSyncCore::present(u32 slot, u32 tick) const {
    const i64 t = tick;
    if (_active_from[slot] <= t && t < _active_until[slot]) {
        return true;
    }
    return std::ranges::any_of(_past,
                               [&](const PastPlayer& p) { return p.slot == slot && p.from <= t && t < p.until; });
}

u32 InputSyncCore::present_mask(u32 tick) const {
    u32 mask = 0;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (present(slot, tick)) {
            mask |= 1u << slot;
        }
    }
    return mask;
}

// The peer that sends joiners the state and decides when they join: the lowest slot in the game.
u32 InputSyncCore::host() const {
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (in_game(slot)) {
            return slot;
        }
    }
    return _config.players;
}

// The peer that may decide joins at `tick`: the lowest slot in the game the tick before. Every peer agrees on it, as on
// every join: the host changes only by a join the host itself decided.
u32 InputSyncCore::host_at(u32 tick) const {
    const u32 before = tick > 0 ? tick - 1 : 0;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (present(slot, before)) {
            return slot;
        }
    }
    return _config.players;
}

bool InputSyncCore::allow_join(u32 slot, PeerId peer) {
    if (!_config.joinable || slot >= _config.players || slot == _config.local_player || peer == no_peer ||
        session_over(_status)) {
        return false;
    }
    PeerTrack& track = _peers[slot];
    if (track.linked) {
        if (_links.link(track.link).peer == peer && !_links.failed(track.link)) {
            return true;
        }
        if (in_game(slot)) {
            return false; // taken
        }
    }
    // Not in the game yet (or only just, as another peer decided): the link counts once the player plays.
    const std::size_t link = _links.add(peer, slot, !in_game(slot));
    if (link == Links::no_link) {
        return false; // the peer has another slot
    }
    if (track.linked) {
        // Someone else was to join and has not: they give way (their link times out unheard).
        std::erase_if(_transfers, [slot](const Transfer& t) { return t.slot == slot; });
    }
    const u64 expected_id = track.expected_id;
    const u64 joined_at_us = track.joined_at_us;
    track = PeerTrack{};
    track.expected_id = expected_id; // a player the game let in, whom this peer had no link to yet
    track.joined_at_us = joined_at_us;
    track.link = link;
    track.linked = true;
    track.acked = in_game(slot) ? _active_from[slot] - 1 : -1;
    return true;
}

void InputSyncCore::set_local_input(std::span<const u8> command) {
    if (command.size() == _input_size) {
        std::ranges::copy(command, _local_input.begin());
    }
}

void InputSyncCore::set_on_confirmed(std::function<void(u32, std::span<const u8>)> on_confirmed) {
    _on_confirmed = std::move(on_confirmed);
}

f32 InputSyncCore::alpha() const {
    if (_tick_us == 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<f32>(_accumulator_us) / static_cast<f32>(_tick_us), 0.0f, 1.0f);
}

std::string InputSyncCore::status_detail() const {
    switch (_status) {
    case SessionStatus::connecting:
    case SessionStatus::running:
        return {};
    case SessionStatus::failed:
        return _failure;
    case SessionStatus::ended:
        return "left the session";
    case SessionStatus::desynced:
        return "the state after tick " + std::to_string(_desync.tick) + " differs from player " +
               std::to_string(_desync.player) + "'s";
    case SessionStatus::rejected:
    case SessionStatus::peer_lost: {
        if (!_failure.empty()) {
            return _failure;
        }
        const std::size_t failed = _links.first_failed();
        if (failed >= _links.links().size()) {
            return std::string(session_status_name(_status));
        }
        const LinkInfo& link = _links.link(failed);
        std::string text = "player " + std::to_string(link.slot) + ": " + std::string(link_state_name(link.state));
        if (link.state == LinkState::rejected) {
            text += " (" + std::string(reject_reason_name(link.reason)) + ")";
        }
        return text;
    }
    }
    return {};
}

void InputSyncCore::leave() {
    _links.leave();
    if (!session_over(_status)) {
        _status = SessionStatus::ended;
    }
}

void InputSyncCore::update(u64 now_us) {
    _now_us = std::max(_now_us, now_us);
    const auto handler = [this](std::size_t link, u8 type, ByteReader& payload) { on_message(link, type, payload); };
    if (session_over(_status)) {
        // Keep answering, so peers learn our last hashes and acks; nothing runs.
        if (_status != SessionStatus::failed) {
            _links.update(_now_us, {});
        }
        return;
    }
    _links.update(_now_us, handler);
    if (session_over(_status)) {
        return; // a message ended it (a joiner that failed): keep that reason
    }
    if (!check_links()) {
        return;
    }
    drop_failed_joiners();
    if (_status == SessionStatus::connecting) {
        if (_config.joining) {
            if (!_have_state) {
                bool any = false;
                for (u32 slot = 0; slot < _config.players; ++slot) {
                    any = any || (_peers[slot].linked && !_links.failed(_peers[slot].link));
                }
                if (!any) {
                    start_failed("no player in the game answered");
                    return;
                }
                if (_state_started && _peers[_state_from].linked && _links.failed(_peers[_state_from].link)) {
                    _state_started = false; // the host sending it went: the next host sends it from the start
                    _state_progress_us = _now_us;
                }
                if (_state_started && _now_us - _state_progress_us > _config.links.timeout_us) {
                    fail_joining("the game's state stopped arriving");
                    return;
                }
                send_join_messages();
                return;
            }
            start_from_state();
            if (_status != SessionStatus::running) {
                return;
            }
        } else {
            if (!_links.all_connected()) {
                return;
            }
            _status = SessionStatus::running;
        }
        _last_update_us = _now_us;
    }
    const u64 elapsed = _now_us - _last_update_us;
    _last_update_us = _now_us;
    resolve_leaves();
    if (_status != SessionStatus::running) {
        return;
    }
    roll_back();
    confirm();
    compare_hashes();
    if (_status != SessionStatus::running) {
        return;
    }
    // After the rollback: a joiner's state must not be one that ran on a wrong prediction.
    serve_join_requests();
    check_joined_players();
    if (_status != SessionStatus::running) {
        return;
    }
    advance(elapsed);
    roll_back(); // a joiner's first commands, scheduled while it ran, can be for ticks it ran on a prediction
    confirm();
    compare_hashes();
    send_inputs();
    serve_transfers();
    send_join_messages();
    _stats.present = present_mask(_tick);
}

void InputSyncCore::on_message(std::size_t link, u8 type, ByteReader& payload) {
    const u32 slot = _links.link(link).slot;
    if (slot >= _config.players || slot == _config.local_player || !_peers[slot].linked || _peers[slot].link != link) {
        return; // a joiner whose slot went to someone else
    }
    if (type == inputs_message) {
        on_inputs(slot, payload);
    } else if (type == hash_message) {
        on_hash(slot, payload);
    } else if (type == join_request_message) {
        const u32 joiner = payload.read_u8();
        const u64 id = payload.read_u64();
        if (payload.ok() && joiner == slot && id != 0) {
            on_join_request(slot, id);
        }
    } else if (type == join_state_message) {
        on_join_state(slot, payload);
    } else if (type == join_ack_message) {
        on_join_ack(slot, payload);
    } else if (type == join_ready_message) {
        const u32 joiner = payload.read_u8();
        const u64 id = payload.read_u64();
        if (payload.ok() && joiner == slot && id != 0) {
            on_join_ready(slot, id);
        }
    } else if (type == lost_message) {
        on_report(slot, payload);
    } else if (type == relay_message) {
        on_relay(slot, payload);
    } else if (type == leave_message) {
        const u32 index = payload.read_u32();
        LeaveDecision decision{.mask = payload.read_u32()};
        for (u32 p = 0; p < _config.players && payload.ok(); ++p) {
            if ((decision.mask >> p) & 1u) {
                decision.ticks[p] = payload.read_u32();
            }
        }
        if (payload.ok() && _status == SessionStatus::running) {
            apply_leave(index, decision);
        }
    }
}

void InputSyncCore::on_inputs(u32 slot, ByteReader& payload) {
    const u32 sender = payload.read_u8();
    const u32 sender_tick = payload.read_u32();
    const auto reported_advantage = static_cast<i16>(payload.read_u16());
    const u32 ack_next = payload.read_u32();
    const u32 epoch = payload.read_u32();
    const u32 first = payload.read_u32();
    const u32 count = payload.read_u8();
    const std::span<const u8> commands = payload.read_bytes(static_cast<std::size_t>(count) * _input_size);
    if (!payload.ok() || sender != slot || (_config.joining && !_have_state)) {
        return;
    }
    std::array<JoinEvent, max_players> joins;
    u32 join_count = 0;
    if (payload.remaining() > 0) {
        join_count = payload.read_u8();
        if (join_count >= _config.players) {
            return; // more than there are slots to join
        }
        for (u32 i = 0; i < join_count; ++i) {
            joins[i].tick = payload.read_u32();
            joins[i].slot = payload.read_u8();
            joins[i].id = payload.read_u64();
            if (!payload.ok() || joins[i].slot >= _config.players) {
                return;
            }
        }
    }
    PeerTrack& peer = _peers[slot];
    if (static_cast<i64>(sender_tick) >= peer.remote_tick) {
        peer.remote_tick = sender_tick;
        peer.remote_tick_at_us = _now_us;
    }
    peer.reported_advantage = static_cast<f32>(reported_advantage) / 256.0f;
    peer.acked = std::max(peer.acked, static_cast<i64>(ack_next) - 1);
    if (epoch < _epoch && _status == SessionStatus::running && _now_us - peer.decisions_sent_us >= lost_repeat_us) {
        send_decisions(slot, epoch); // it missed a leave the game agreed on
        peer.decisions_sent_us = _now_us;
    }
    if (!in_game(slot) || lost(slot)) {
        return; // a joiner still catching up sends only acks; a lost player's commands stay where they stopped
    }
    take_commands(slot, first, count, commands, std::span<const JoinEvent>(joins.data(), join_count), false);
}

// A player's commands from `first` on, from the player itself or (`relayed`) from a peer that has more of a lost
// player's commands than this one.
void InputSyncCore::take_commands(u32 slot, u32 first, u32 count, std::span<const u8> commands,
                                  std::span<const JoinEvent> joins, bool relayed) {
    const PeerTrack& peer = _peers[slot];
    // A tick this far ahead would overwrite ring slots still in use; the sender resends it later.
    const i64 limit = static_cast<i64>(_tick) + ring_ticks - _config.max_prediction - 4;
    for (u32 i = 0; i < count; ++i) {
        const i64 tick = static_cast<i64>(first) + i;
        if (tick <= _known[slot]) {
            continue;
        }
        if (tick >= _active_until[slot]) {
            break; // gone from there
        }
        if (tick != _known[slot] + 1 || tick >= limit) {
            if (!relayed && _stats.spectating && tick > _known[slot] + 1 && peer.acked >= 0) {
                // The commands after our state are gone from the peer's history: the game moved on too far. Usually
                // because a player's commands never came: say which.
                for (u32 other = 0; other < _config.players; ++other) {
                    if (other != _config.local_player && in_game(other) &&
                        (!_peers[other].linked || _links.link(_peers[other].link).state != LinkState::connected)) {
                        fail_joining("player " + std::to_string(other) +
                                     " does not answer this joiner (did it allow_join() this player?)");
                        return;
                    }
                }
                fail_joining("the game moved on before the state arrived: join again");
            }
            break;
        }
        const u32 t = static_cast<u32>(tick);
        bool wait = false;
        for (const JoinEvent& join : joins) {
            if (join.tick != t) {
                continue;
            }
            if (host_at(t) != slot) {
                if (_frozen != 0) {
                    wait = true; // a leave this peer has not applied yet may make the sender the host: later
                    break;
                }
                start_failed("player " + std::to_string(slot) + " let a player in at tick " + std::to_string(t) +
                             ", where it is not the host");
                return;
            }
            if (join.slot < _config.players && lost(join.slot)) {
                wait = true; // the slot's last player's commands are still on their way
                break;
            }
            apply_join(join.slot, t, join.id); // with the command for its tick, as on every peer
            if (_status != SessionStatus::running && _status != SessionStatus::connecting) {
                return;
            }
        }
        if (wait) {
            break; // the sender sends these again
        }
        const std::span<const u8> command = commands.subspan(static_cast<std::size_t>(i) * _input_size, _input_size);
        const std::size_t index = static_cast<std::size_t>(slot) * ring_ticks + t % ring_ticks;
        std::span<u8> stored = input(slot, t);
        if (_predicted[index] && _input_tick[index] == t && t < _tick &&
            !std::equal(command.begin(), command.end(), stored.begin())) {
            _rollback_to = _rollback_to < 0 ? tick : std::min(_rollback_to, tick);
        }
        std::ranges::copy(command, stored.begin());
        _input_tick[index] = t;
        _predicted[index] = 0;
        _known[slot] = tick;
    }
}

void InputSyncCore::on_hash(u32 slot, ByteReader& payload) {
    const u32 sender = payload.read_u8();
    const u32 tick = payload.read_u32();
    const u64 hash = payload.read_u64();
    if (!payload.ok() || sender != slot || !in_game(slot) || lost(slot)) {
        return; // a joiner's hashes count once it plays: until then a bad state of its own fails only itself
    }
    auto& hashes = _peers[slot].hashes;
    hashes.emplace_back(tick, hash);
    if (hashes.size() > kept_hashes) {
        hashes.pop_front();
    }
}

void InputSyncCore::schedule_local(u32 tick) {
    const u32 local = _config.local_player;
    if (!in_game(local)) {
        return; // a joiner, until it plays
    }
    const i64 target = static_cast<i64>(tick) + _config.input_delay;
    while (_known[local] < target) {
        const u32 next = static_cast<u32>(_known[local] + 1);
        const std::size_t index = static_cast<std::size_t>(local) * ring_ticks + next % ring_ticks;
        std::span<u8> stored = input(local, next);
        // Only a joiner's first commands can be for ticks it already ran (on a prediction, before it knew when it
        // joins): those run again with them.
        if (next < _tick && _predicted[index] && _input_tick[index] == next &&
            !std::equal(_local_input.begin(), _local_input.end(), stored.begin())) {
            _rollback_to = _rollback_to < 0 ? next : std::min<i64>(_rollback_to, next);
        }
        std::ranges::copy(_local_input, stored.begin());
        _input_tick[index] = next;
        _predicted[index] = 0;
        _known[local] = next;
        _input_changed = true;
    }
}

bool InputSyncCore::can_step(u32 tick) {
    schedule_local(tick);
    return static_cast<i64>(tick) - confirmed_tick() <= static_cast<i64>(_config.max_prediction);
}

void InputSyncCore::step_tick(u32 tick, bool resimulating) {
    bool all_known = true;
    for (u32 player = 0; player < _config.players; ++player) {
        all_known = all_known && (!present(player, tick) || _known[player] >= static_cast<i64>(tick));
    }
    if (!all_known && !_snapshots.empty()) {
        // A rollback may come back to this tick: keep the state it starts from.
        const std::size_t slot = tick % _snapshots.size();
        _hooks.save(_snapshots[slot]);
        _snapshot_tick[slot] = tick;
    }
    u32 joined = 0;
    u32 left = 0;
    for (u32 player = 0; player < _config.players; ++player) {
        if (!present(player, tick)) {
            if (tick > 0 && present(player, tick - 1)) {
                left |= 1u << player;
            }
            std::ranges::copy(_default_command,
                              _step_buffer.begin() + static_cast<std::ptrdiff_t>(player * _input_size));
            continue;
        }
        if (_active_from[player] == static_cast<i64>(tick) && tick > 0) {
            joined |= 1u << player;
        }
        std::span<u8> command = input(player, tick);
        if (_known[player] < static_cast<i64>(tick)) {
            // Predict that the player still does what they did last: commands are levels, held for many ticks.
            const i64 last_tick = _known[player];
            const bool have_last =
                last_tick >= 0 && last_tick >= _active_from[player] &&
                _input_tick[static_cast<std::size_t>(player) * ring_ticks + static_cast<u32>(last_tick) % ring_ticks] ==
                    static_cast<u32>(last_tick);
            if (have_last) {
                const std::span<u8> last = input(player, static_cast<u32>(last_tick));
                std::copy(last.begin(), last.end(), command.begin());
            } else {
                std::ranges::copy(_default_command, command.begin());
            }
            const std::size_t index = static_cast<std::size_t>(player) * ring_ticks + tick % ring_ticks;
            _input_tick[index] = tick;
            _predicted[index] = 1;
        }
        std::ranges::copy(command, _step_buffer.begin() + static_cast<std::ptrdiff_t>(player * _input_size));
    }
    _hooks.step(_step_buffer,
                StepInfo{.tick = tick,
                         .resimulating = resimulating,
                         .confirmed = all_known,
                         .present = _config.joinable || _config.players_may_leave ? present_mask(tick) : ~0u,
                         .joined = joined,
                         .left = left});
    if (_config.hash_interval > 0 && (tick + 1) % _config.hash_interval == 0) {
        _pending_hashes.emplace_back(tick, _hooks.state_hash());
    }
}

void InputSyncCore::roll_back() {
    if (_rollback_to < 0) {
        return;
    }
    const u32 from = static_cast<u32>(_rollback_to);
    _rollback_to = -1;
    if (from >= _tick) {
        return;
    }
    const std::size_t slot = from % _snapshots.size();
    if (_snapshot_tick[slot] != static_cast<i64>(from)) {
        start_failed("internal error: no saved state for tick " + std::to_string(from));
        return;
    }
    if (!_hooks.restore(_snapshots[slot])) {
        start_failed("the simulation refused its own saved state of tick " + std::to_string(from));
        return;
    }
    std::erase_if(_pending_hashes, [from](const auto& entry) { return entry.first >= from; });
    for (u32 tick = from; tick < _tick; ++tick) {
        step_tick(tick, true);
    }
    const u32 depth = _tick - from;
    ++_stats.rollbacks;
    _stats.resimulated_ticks += depth;
    _stats.max_rollback = std::max(_stats.max_rollback, depth);
}

void InputSyncCore::confirm() {
    const i64 confirmed = confirmed_tick();
    _stats.tick = _tick;
    _stats.confirmed_tick = confirmed;
    if (_on_confirmed) {
        while (_reported < confirmed) {
            const u32 tick = static_cast<u32>(++_reported);
            for (u32 player = 0; player < _config.players; ++player) {
                const std::span<const u8> command =
                    present(player, tick) ? std::span<const u8>(input(player, tick)) : _default_command;
                std::ranges::copy(command, _step_buffer.begin() + static_cast<std::ptrdiff_t>(player * _input_size));
            }
            _on_confirmed(tick, _step_buffer);
        }
    } else {
        _reported = std::max(_reported, confirmed);
    }
    // A hash of a tick that ran on real commands only is final: share it.
    while (!_pending_hashes.empty() && static_cast<i64>(_pending_hashes.front().first) <= confirmed) {
        const auto [tick, hash] = _pending_hashes.front();
        _pending_hashes.pop_front();
        _local_hashes.emplace_back(tick, hash);
        if (_local_hashes.size() > kept_hashes) {
            _local_hashes.pop_front();
        }
        std::vector<u8> payload;
        ByteWriter writer(payload);
        writer.write_u8(static_cast<u8>(_config.local_player));
        writer.write_u32(tick);
        writer.write_u64(hash);
        for (u32 slot = 0; slot < _config.players && in_game(_config.local_player); ++slot) {
            if (slot != _config.local_player && _peers[slot].linked) {
                _links.send(_peers[slot].link, hash_message, payload);
            }
        }
    }
}

void InputSyncCore::compare_hashes() {
    const i64 newest = _local_hashes.empty() ? -1 : static_cast<i64>(_local_hashes.back().first);
    for (u32 slot = 0; slot < _config.players && _status == SessionStatus::running; ++slot) {
        auto& theirs = _peers[slot].hashes;
        while (!theirs.empty() && static_cast<i64>(theirs.front().first) <= newest) {
            const auto [tick, hash] = theirs.front();
            theirs.pop_front();
            const auto mine = std::ranges::find(_local_hashes, tick, &std::pair<u32, u64>::first);
            if (mine == _local_hashes.end()) {
                continue; // older than what we keep
            }
            ++_stats.hashes_compared;
            if (mine->second != hash) {
                _desync = {.tick = tick, .player = slot, .local_hash = mine->second, .remote_hash = hash};
                _status = SessionStatus::desynced;
                if (!in_game(_config.local_player)) {
                    _links.leave(); // a joiner whose state is wrong: it goes, the game does not wait for it
                }
                return;
            }
        }
    }
}

f64 InputSyncCore::remote_now(u32 slot) const {
    // Where the peer's clock is now: its last report, plus the time since, plus half a round trip in flight. In f64:
    // tick numbers pass f32's 2^24 after three days at 60 Hz.
    const PeerTrack& peer = _peers[slot];
    const f64 rtt_us = static_cast<f64>(_links.link(peer.link).rtt_ms) * 1000.0;
    const f64 since_us = static_cast<f64>(_now_us - peer.remote_tick_at_us) + rtt_us * 0.5;
    return static_cast<f64>(peer.remote_tick) + since_us / static_cast<f64>(_tick_us);
}

f32 InputSyncCore::own_view(u32 slot) const {
    if (!_peers[slot].linked || _peers[slot].remote_tick < 0) {
        return 0.0f;
    }
    return static_cast<f32>(static_cast<f64>(_tick) - remote_now(slot));
}

f32 InputSyncCore::measure_advantage() const {
    f32 advantage = 0.0f;
    bool any = false;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        const PeerTrack& peer = _peers[slot];
        // A joiner catching up is far behind: it is not waited for.
        if (slot == _config.local_player || !peer.linked || peer.remote_tick < 0 || !in_game(slot) || lost(slot) ||
            !in_game(_config.local_player)) {
            continue;
        }
        // Half the difference of the two sides' views (as GGPO does): an error both make the same way, such as a
        // clock that stood still while its peer waited, cancels.
        const f32 ahead = (own_view(slot) - peer.reported_advantage) * 0.5f;
        advantage = any ? std::max(advantage, ahead) : ahead;
        any = true;
    }
    return advantage;
}

void InputSyncCore::advance(u64 elapsed_us) {
    // The peer that is ahead runs a tenth slower until the others catch up: no visible stall, and rollbacks (or
    // lockstep waits) stay even between the peers.
    const f32 measured = measure_advantage();
    _stats.advantage += (measured - _stats.advantage) / 8.0f;
    if (_stats.advantage > 0.75f) {
        elapsed_us -= elapsed_us / 10;
        ++_stats.slowed_updates;
    }
    _accumulator_us += elapsed_us;
    if (catch_up() > 0) {
        _accumulator_us = 0;
        return;
    }
    u32 steps = 0;
    while (_accumulator_us >= _tick_us && _status == SessionStatus::running) {
        if (steps == _config.max_catchup) {
            // After a hitch, run at most max_catchup ticks and let the rest of the lost time go.
            _accumulator_us = std::min(_accumulator_us, _tick_us);
            break;
        }
        if (!can_step(_tick)) {
            // Waiting for a peer: the clock stops instead of banking time it would run too fast later.
            ++_stats.waits;
            _accumulator_us = std::min(_accumulator_us, _tick_us);
            break;
        }
        step_tick(_tick, false);
        ++_tick;
        _accumulator_us -= _tick_us;
        ++steps;
    }
}

void InputSyncCore::send_inputs() {
    const u32 local = _config.local_player;
    const std::size_t header = inputs_header + (_config.joinable ? join_trailer : 0);
    const std::size_t max_count = std::min<std::size_t>(255, (_links.max_payload() - header) / _input_size);
    std::vector<u8> payload;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot == local || !_peers[slot].linked || lost(slot) || _links.failed(_peers[slot].link)) {
            continue;
        }
        PeerTrack& peer = _peers[slot];
        if (!in_game(slot) && peer.remote_tick < 0) {
            continue; // a joiner that has not started yet: nothing to send it
        }
        // Every command the peer has not acknowledged, oldest first: a lost datagram costs nothing, the next one
        // carries the same commands again.
        const i64 first = std::max(peer.acked + 1, _known[local] - static_cast<i64>(ring_ticks) + 1);
        const i64 pending = std::max<i64>(0, _known[local] - first + 1);
        const std::size_t count =
            in_game(local) ? std::min<std::size_t>(static_cast<std::size_t>(pending), max_count) : 0;
        const i64 ack_next = _known[slot] + 1;
        const u64 since = _now_us - peer.last_sent_us;
        const bool due = (count > 0 && (_input_changed || since >= _tick_us)) || ack_next != peer.last_ack_sent ||
                         since >= keepalive_us;
        if (!due) {
            continue;
        }
        payload.clear();
        ByteWriter writer(payload);
        writer.write_u8(static_cast<u8>(local));
        writer.write_u32(_tick);
        writer.write_u16(static_cast<u16>(static_cast<i16>(std::clamp(own_view(slot) * 256.0f, -32000.0f, 32000.0f))));
        writer.write_u32(static_cast<u32>(ack_next));
        writer.write_u32(_epoch);
        writer.write_u32(static_cast<u32>(first));
        writer.write_u8(static_cast<u8>(count));
        for (std::size_t i = 0; i < count; ++i) {
            writer.write_bytes(input(local, static_cast<u32>(first + static_cast<i64>(i))));
        }
        if (_config.joinable) {
            // The joins we decided at the ticks of these commands travel with them.
            // _created holds at most players - 1: join_trailer has room for all of them.
            const auto in_range = [&](const JoinEvent& join) {
                return static_cast<i64>(join.tick) >= first &&
                       static_cast<i64>(join.tick) < first + static_cast<i64>(count);
            };
            writer.write_u8(static_cast<u8>(std::ranges::count_if(_created, in_range)));
            for (const JoinEvent& join : _created) {
                if (in_range(join)) {
                    writer.write_u32(join.tick);
                    writer.write_u8(static_cast<u8>(join.slot));
                    writer.write_u64(join.id);
                }
            }
        }
        if (_links.send(peer.link, inputs_message, payload)) {
            peer.last_sent_us = _now_us;
            peer.last_ack_sent = ack_next;
        }
    }
    _input_changed = false;
}

// --- joining a game in progress ---

void InputSyncCore::drop_failed_joiners() {
    for (u32 slot = 0; slot < _config.players; ++slot) {
        PeerTrack& peer = _peers[slot];
        if (peer.linked && _links.link(peer.link).optional && _links.failed(peer.link)) {
            // It went before it played: the slot stays empty, nobody waited for it.
            peer.linked = false;
            std::erase_if(_transfers, [slot](const Transfer& t) { return t.slot == slot; });
        }
    }
}

void InputSyncCore::on_join_request(u32 slot, u64 id) {
    _peers[slot].join_id = id;
    for (Transfer& transfer : _transfers) {
        if (transfer.slot == slot) {
            transfer.progress_us = 0; // asked again: send what it waits for now
            return;
        }
    }
    if (std::ranges::find(_join_requests, slot) == _join_requests.end()) {
        _join_requests.push_back(slot); // served after this update's rollback
    }
}

void InputSyncCore::serve_join_requests() {
    for (const u32 slot : _join_requests) {
        PeerTrack& peer = _peers[slot];
        if (!_config.joinable || in_game(slot) || host() != _config.local_player || !peer.linked ||
            peer.served_id == peer.join_id || _frozen != 0) {
            continue; // (a request that was on its way while the state went: that joiner has it; a leave still open:
                      // the joiner asks again)
        }
        if (std::ranges::any_of(_transfers, [slot](const Transfer& t) { return t.slot == slot; })) {
            continue;
        }
        // The state from before the first tick that is not yet final everywhere: from then on the joiner gets every
        // player's real commands, as the others do.
        const i64 confirmed = confirmed_tick();
        const u32 base = static_cast<u32>(std::min<i64>(confirmed + 1, _tick));
        if (!present(_config.local_player, base > 0 ? base - 1 : 0)) {
            continue; // we joined after the state it would get: it asks again once that is past
        }
        Transfer transfer{.slot = slot, .progress_us = _now_us, .acked_us = _now_us};
        if (confirmed + 1 >= static_cast<i64>(_tick)) {
            transfer.base = _tick;
            _hooks.save(transfer.bytes);
        } else {
            transfer.base = static_cast<u32>(confirmed + 1);
            const std::size_t ring = transfer.base % _snapshots.size();
            if (_snapshot_tick[ring] != static_cast<i64>(transfer.base)) {
                continue; // cannot happen: every tick past the confirmed one ran on a prediction and was saved
            }
            transfer.bytes = _snapshots[ring];
        }
        // Who played before the base: a join at the base itself comes with the host's command for it, as for every
        // peer (the state is from before it, so it has not happened in it).
        transfer.present = present_mask(transfer.base > 0 ? transfer.base - 1 : 0);
        transfer.epoch = _epoch;
        // The joiner's commands to us start at its join tick; ours to it at the base.
        peer.acked = static_cast<i64>(transfer.base) - 1;
        peer.served_id = peer.join_id;
        _transfers.push_back(std::move(transfer));
    }
    _join_requests.clear();
}

void InputSyncCore::serve_transfers() {
    if (_links.max_payload() <= join_state_header) {
        return;
    }
    const u32 chunk = static_cast<u32>(_links.max_payload() - join_state_header);
    std::vector<u8> payload;
    // A joiner that acknowledges nothing for a link timeout has stalled: stop sending (it may ask again).
    std::erase_if(_transfers, [&](const Transfer& t) {
        const bool stalled = _now_us - t.acked_us > _config.links.timeout_us;
        if (stalled) {
            _peers[t.slot].served_id = 0; // if it asks again, it starts over
        }
        return stalled;
    });
    for (Transfer& transfer : _transfers) {
        const PeerTrack& peer = _peers[transfer.slot];
        const u32 total = static_cast<u32>(transfer.bytes.size());
        if (!peer.linked || transfer.acked >= total + (total == 0 ? 1u : 0u)) {
            continue;
        }
        const auto send_chunk = [&](u32 offset) {
            const u32 length = std::min(chunk, total - offset);
            payload.clear();
            ByteWriter writer(payload);
            writer.write_u8(static_cast<u8>(transfer.slot));
            writer.write_u32(transfer.base);
            writer.write_u32(transfer.present);
            writer.write_u32(transfer.epoch);
            writer.write_u32(total);
            writer.write_u32(offset);
            writer.write_bytes(std::span<const u8>(transfer.bytes).subspan(offset, length));
            if (_links.send(peer.link, join_state_message, payload)) {
                _stats.state_bytes_sent += length;
            }
        };
        // Nothing acknowledged for a while: the chunk the joiner waits for was lost. It keeps the ones after it, so
        // only that one goes again.
        const u64 resend_us = std::max<u64>(join_resend_us, static_cast<u64>(_links.link(peer.link).rtt_ms * 2000.0f));
        if (transfer.next > transfer.acked && _now_us - transfer.progress_us > resend_us) {
            send_chunk(transfer.acked);
            transfer.progress_us = _now_us;
        }
        const u32 window = join_window_chunks * chunk;
        bool first = total == 0 && transfer.next == 0;
        while ((transfer.next < total && transfer.next < transfer.acked + window) || first) {
            first = false;
            const u32 length = std::min(chunk, total - transfer.next);
            send_chunk(transfer.next);
            transfer.next += length;
            if (length == 0) {
                break;
            }
        }
    }
}

void InputSyncCore::on_join_ack(u32 slot, ByteReader& payload) {
    const u32 joiner = payload.read_u8();
    const u32 received = payload.read_u32();
    if (!payload.ok() || joiner != slot) {
        return;
    }
    for (Transfer& transfer : _transfers) {
        if (transfer.slot == slot && received > transfer.acked) {
            transfer.acked = std::min<u32>(received, static_cast<u32>(transfer.bytes.size()));
            transfer.next = std::max(transfer.next, transfer.acked);
            transfer.progress_us = _now_us;
            transfer.acked_us = _now_us;
        }
    }
    // Every byte arrived: what is left is the joiner's catching up.
    std::erase_if(_transfers, [](const Transfer& t) { return t.acked >= t.bytes.size() && !t.bytes.empty(); });
}

void InputSyncCore::on_join_state(u32 slot, ByteReader& payload) {
    const u32 joiner = payload.read_u8();
    const u32 base = payload.read_u32();
    const u32 present_slots = payload.read_u32();
    const u32 epoch = payload.read_u32();
    const u32 total = payload.read_u32();
    const u32 offset = payload.read_u32();
    const std::span<const u8> bytes = payload.read_bytes(payload.remaining());
    if (!payload.ok() || !_config.joining || joiner != _config.local_player || _have_state) {
        return;
    }
    if (total > max_join_state) {
        fail_joining("the game's state is " + std::to_string(total) + " bytes, more than a joiner takes");
        return;
    }
    if (!_state_started || _state_from != slot || _state_base != base || _state_total != total ||
        _state_present != present_slots || _state_epoch != epoch) {
        // The first chunk, or the host started over: begin again.
        _state_progress_us = _now_us;
        _state_started = true;
        _state_from = slot;
        _state_base = base;
        _state_total = total;
        _state_present = present_slots;
        _state_epoch = epoch;
        _state.clear();
        _state.reserve(std::min<u64>(total, join_state_reserve));
        _state_early.clear();
    }
    if (offset == _state.size() && _state.size() + bytes.size() <= total) {
        _state.insert(_state.end(), bytes.begin(), bytes.end());
        _stats.state_bytes_received += bytes.size();
        _state_progress_us = _now_us;
        // What came early follows on.
        for (auto it = _state_early.find(static_cast<u32>(_state.size())); it != _state_early.end();
             it = _state_early.find(static_cast<u32>(_state.size()))) {
            _state.insert(_state.end(), it->second.begin(), it->second.end());
            _state_early.erase(it);
        }
    } else if (offset > _state.size() && static_cast<u64>(offset) + bytes.size() <= total &&
               !_state_early.contains(offset) && _state_early.size() < 4 * join_window_chunks) {
        _state_early.emplace(offset, std::vector<u8>(bytes.begin(), bytes.end())); // after one that was lost
        _stats.state_bytes_received += bytes.size();
    }
    std::vector<u8> ack;
    ByteWriter writer(ack);
    writer.write_u8(static_cast<u8>(_config.local_player));
    writer.write_u32(static_cast<u32>(_state.size()));
    _links.send(_peers[slot].link, join_ack_message, ack);
    if (_state.size() == total && (present_slots >> slot) & 1u) {
        _have_state = true;
    }
}

void InputSyncCore::start_from_state() {
    if (!_hooks.restore(_state)) {
        fail_joining("the simulation refused the state the game sent");
        return;
    }
    _state = {};
    _state_early.clear();
    _tick = _state_base;
    _stats.start_tick = _state_base;
    _stats.spectating = true;
    _epoch = _state_epoch; // the leaves before the state are in it
    _reported = static_cast<i64>(_state_base) - 1;
    _accumulator_us = 0;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if ((_state_present >> slot) & 1u) {
            _active_from[slot] = 0;
            _known[slot] = static_cast<i64>(_state_base) - 1;
            if (_peers[slot].linked) {
                _links.set_optional(_peers[slot].link, false); // in the game: needed from now on
            }
            _peers[slot].joined_at_us = _now_us; // a player we have no link to fails us after a while
        }
    }
    std::ranges::fill(_snapshot_tick, -1);
    _status = SessionStatus::running;
}

u32 InputSyncCore::catch_up() {
    if (!_stats.spectating) {
        return 0;
    }
    f64 target = 0.0;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot != _config.local_player && _peers[slot].linked && in_game(slot) && !lost(slot) &&
            _peers[slot].remote_tick >= 0) {
            target = std::max(target, remote_now(slot));
        }
    }
    u32 ran = 0;
    while (static_cast<f64>(_tick) + 1.5 < target && ran < join_catchup_ticks && _status == SessionStatus::running &&
           can_step(_tick)) {
        step_tick(_tick, false);
        ++_tick;
        ++ran;
    }
    return ran;
}

void InputSyncCore::send_join_messages() {
    if (!_config.joining || in_game(_config.local_player) || _now_us < _next_join_message_us) {
        return;
    }
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_u8(static_cast<u8>(_config.local_player));
    writer.write_u64(_join_id);
    if (!_have_state) {
        // Ask every player we reach; the game's host answers, every peer learns our id.
        for (u32 slot = 0; slot < _config.players; ++slot) {
            if (_peers[slot].linked) {
                _links.send(_peers[slot].link, join_request_message, payload);
            }
        }
        _next_join_message_us = _now_us + join_repeat_us;
        return;
    }
    // Ready once caught up, connected to everyone in the game, and (if the game compares hashes) our state agrees
    // with theirs at least once: then the host picks the tick we join at.
    bool ready = _status == SessionStatus::running && (_config.hash_interval == 0 || _stats.hashes_compared > 0);
    for (u32 slot = 0; slot < _config.players && ready; ++slot) {
        if (slot != _config.local_player && in_game(slot)) {
            const PeerTrack& peer = _peers[slot];
            ready = peer.linked && _links.link(peer.link).state == LinkState::connected && peer.remote_tick >= 0 &&
                    remote_now(slot) - static_cast<f64>(_tick) < 2.5;
        }
    }
    if (!ready) {
        return;
    }
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot != _config.local_player && in_game(slot) && _peers[slot].linked) {
            _links.send(_peers[slot].link, join_ready_message, payload);
        }
    }
    _next_join_message_us = _now_us + join_repeat_us;
}

void InputSyncCore::on_join_ready(u32 slot, u64 id) {
    _peers[slot].join_id = id;
    if (!_config.joinable || _status != SessionStatus::running || in_game(slot) || !_peers[slot].linked ||
        _frozen != 0) {
        return; // (a leave still open: the joiner says it is ready again)
    }
    // The first tick none of our commands has been sent for: no peer can have finished it, and each takes the join
    // with our command for it.
    const u32 tick = static_cast<u32>(std::max<i64>(_known[_config.local_player] + 1, _tick));
    if (host_at(tick) != _config.local_player) {
        return; // another peer decides (one that joined before this tick): the joiner says it is ready again
    }
    _created.push_back({.tick = tick, .slot = slot, .id = id});
    apply_join(slot, tick, id);
}

void InputSyncCore::apply_join(u32 slot, u32 tick, u64 id) {
    if (in_game(slot)) {
        return;
    }
    if (slot == _config.local_player && id != _join_id) {
        fail_joining("the game let another player into this slot");
        return;
    }
    if (_active_until[slot] != never) {
        // Someone takes the slot of a player who left: that player's time in it stays as it was.
        if (static_cast<i64>(tick) < _active_until[slot]) {
            start_failed("player " + std::to_string(slot) + " joined before the player who left the slot had gone");
            return;
        }
        _past.push_back(
            {.slot = slot, .from = _active_from[slot], .until = _active_until[slot], .id = _peers[slot].expected_id});
        _active_until[slot] = never;
    }
    _active_from[slot] = tick;
    _known[slot] = static_cast<i64>(tick) - 1;
    ++_stats.joins;
    PeerTrack& peer = _peers[slot];
    peer.expected_id = id;
    peer.joined_at_us = _now_us;
    if (peer.linked) {
        _links.set_optional(peer.link, false); // it plays: losing it now ends the session, as for anyone
    }
    if (slot == _config.local_player) {
        // We play from `tick`: none of our commands before it exist, so every peer has all of them.
        _stats.spectating = false;
        for (PeerTrack& other : _peers) {
            other.acked = std::max(other.acked, static_cast<i64>(tick) - 1);
        }
    }
    if (tick < _tick) {
        // Ticks that ran without the player run again with them.
        _rollback_to = _rollback_to < 0 ? tick : std::min<i64>(_rollback_to, tick);
    }
}

// A player in the game whom this peer cannot play with fails it, saying why, instead of a stall nobody explains.
void InputSyncCore::check_joined_players() {
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot == _config.local_player || !in_game(slot)) {
            continue;
        }
        const PeerTrack& peer = _peers[slot];
        if (peer.linked && peer.expected_id != 0 && peer.join_id != 0 && peer.join_id != peer.expected_id) {
            start_failed("player " + std::to_string(slot) +
                         " joined as another player than the one allow_join() named here");
            return;
        }
        if (!peer.linked && _now_us - peer.joined_at_us > _config.links.timeout_us) {
            start_failed("player " + std::to_string(slot) +
                         " is in the game, but this peer has no link to them: every peer must allow_join() a joiner");
            return;
        }
    }
}

void InputSyncCore::fail_joining(std::string detail) {
    start_failed(std::move(detail));
    _links.leave(); // the game frees the slot at once instead of waiting for a timeout
}

// --- leaving (docs/networking.md#leaving) ---
//
// A peer that loses a player in the game (their link timed out, or they said goodbye) freezes their commands where
// they stopped and says so to every peer, with how far it has them; a peer told that someone lost a player freezes
// them too. The peers send each other the lost player's commands that some lack. Once every remaining player reports
// the same lost players, the lowest of them decides the tick they are gone from: one past the furthest anyone has
// their commands, so no peer has run a tick with them that the game then runs without them. Decisions are numbered,
// and every peer applies them in that order.

std::string InputSyncCore::describe_link(std::size_t index) const {
    const LinkInfo& link = _links.link(index);
    std::string text = "player " + std::to_string(link.slot) + ": " + std::string(link_state_name(link.state));
    if (link.state == LinkState::rejected) {
        text += " (" + std::string(reject_reason_name(link.reason)) + ")";
    }
    return text;
}

// False when a failed link ends the session: a peer of another game, a player lost before the start, or any player
// lost when players may not leave. A player lost while the game runs leaves it instead.
bool InputSyncCore::check_links() {
    for (std::size_t i = 0; i < _links.links().size(); ++i) {
        const LinkInfo& link = _links.link(i);
        if (link.optional || !_links.failed(i)) {
            continue;
        }
        const u32 slot = link.slot;
        if (slot >= _config.players || !_peers[slot].linked || _peers[slot].link != i) {
            continue; // a link the slot no longer uses (a player who left, whose slot someone else took)
        }
        if (link.state == LinkState::rejected) {
            _status = SessionStatus::rejected;
            _failure = describe_link(i);
            return false;
        }
        if (!_config.players_may_leave || _status != SessionStatus::running) {
            _status = SessionStatus::peer_lost;
            _failure = describe_link(i);
            return false;
        }
        if (in_game(slot) && !lost(slot)) {
            freeze(slot, link.state == LinkState::timed_out);
        }
    }
    return true;
}

void InputSyncCore::freeze(u32 slot, bool timed_out) {
    _frozen |= 1u << slot;
    _undecided |= 1u << slot;
    if (timed_out) {
        _timed_out |= 1u << slot;
    }
    _next_report_us = 0; // say so at once
}

void InputSyncCore::resolve_leaves() {
    const u32 local = _config.local_player;
    // A player the game let go, and this peer has all their commands from before: done.
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (lost(slot) && !((_undecided >> slot) & 1u) && _active_until[slot] != never &&
            _known[slot] >= _active_until[slot] - 1) {
            _frozen &= ~(1u << slot);
        }
    }
    if (_undecided != 0) {
        u32 others = 0;
        for (u32 slot = 0; slot < _config.players; ++slot) {
            if (slot != local && in_game(slot)) {
                others |= 1u << slot;
            }
        }
        const bool everyone = others != 0 && (others & ~_undecided) == 0;
        if (everyone && !in_game(local)) {
            fail_joining("this joiner lost every player in the game");
            return;
        }
        if (everyone && (_timed_out & _undecided) != 0) {
            // Every other player gone quiet at once: most likely this peer's own connection. (Players who said
            // goodbye leave the game to this peer, which plays on.)
            u32 first = 0;
            while (!((_timed_out >> first) & 1u)) {
                ++first;
            }
            _status = SessionStatus::peer_lost;
            _failure = "lost every other player (" + describe_link(_peers[first].link) + ")";
            _links.leave();
            return;
        }
        if (in_game(local)) {
            decide_leave();
            if (_status != SessionStatus::running) {
                return;
            }
        }
    }
    _stats.lost = _frozen;
    if (_frozen != 0 && _now_us >= _next_report_us) {
        send_report();
    }
}

void InputSyncCore::decide_leave() {
    const u32 leaving = _undecided;
    u32 decider = _config.players;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (in_game(slot) && !((leaving >> slot) & 1u)) {
            decider = slot;
            break;
        }
    }
    if (decider != _config.local_player) {
        return;
    }
    // Every remaining player must have lost the same players (and applied the same decisions): then none takes more
    // of their commands, and the furthest any has each one's is final. Each is gone from the tick after it.
    std::array<i64, max_players> last;
    for (u32 p = 0; p < _config.players; ++p) {
        last[p] = _known[p];
    }
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot == _config.local_player || !in_game(slot) || ((leaving >> slot) & 1u)) {
            continue;
        }
        const PeerTrack& peer = _peers[slot];
        if (!peer.reported || peer.report_epoch != _epoch || peer.report_undecided != leaving) {
            return;
        }
        for (u32 p = 0; p < _config.players; ++p) {
            if ((leaving >> p) & 1u) {
                last[p] = std::max(last[p], peer.report_known[p]);
            }
        }
    }
    LeaveDecision decision{.mask = leaving};
    for (u32 p = 0; p < _config.players; ++p) {
        if ((leaving >> p) & 1u) {
            decision.ticks[p] = static_cast<u32>(std::max<i64>(last[p] + 1, 0));
        }
    }
    const u32 index = _epoch;
    apply_leave(index, decision);
    if (_status != SessionStatus::running) {
        return;
    }
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot != _config.local_player && _peers[slot].linked) {
            send_decisions(slot, index);
        }
    }
}

void InputSyncCore::apply_leave(u32 index, const LeaveDecision& decision) {
    if (index != _epoch) {
        return; // one this peer applied, or one after a decision it still lacks (each is sent again, in order)
    }
    const u32 mask = decision.mask;
    const u32 all = (1u << _config.players) - 1;
    if (mask == 0 || (mask & ~all) != 0) {
        start_failed("a leave named no player of this game");
        return;
    }
    const u32 local = _config.local_player;
    if ((mask >> local) & 1u) {
        // The others lost this peer: the game goes on without it.
        _status = SessionStatus::peer_lost;
        _failure =
            "the other players lost this peer (it left the game at tick " + std::to_string(decision.ticks[local]) + ")";
        _links.leave();
        return;
    }
    u32 earliest = std::numeric_limits<u32>::max();
    for (u32 p = 0; p < _config.players; ++p) {
        if (!((mask >> p) & 1u)) {
            continue;
        }
        if (!in_game(p)) {
            start_failed("a leave named player " + std::to_string(p) + ", who is not in the game");
            return;
        }
        // Cannot happen to a player (the tick is past every remaining player's commands of theirs); a joiner may
        // have taken commands no player reported.
        if (_known[p] >= static_cast<i64>(decision.ticks[p]) && confirmed_tick() >= decision.ticks[p]) {
            if (!in_game(local)) {
                fail_joining("a player left at a tick this joiner had already run: join again");
            } else {
                start_failed("player " + std::to_string(p) + " left at tick " + std::to_string(decision.ticks[p]) +
                             ", which this peer ran with them");
            }
            return;
        }
        earliest = std::min(earliest, decision.ticks[p]);
    }
    for (u32 p = 0; p < _config.players; ++p) {
        if (!((mask >> p) & 1u)) {
            continue;
        }
        const u32 bit = 1u << p;
        const u32 tick = decision.ticks[p];
        _active_until[p] = tick;
        _known[p] = std::min<i64>(_known[p], static_cast<i64>(tick) - 1);
        _frozen |= bit; // until this peer has their commands from before `tick`
        _undecided &= ~bit;
        _timed_out &= ~bit;
        std::erase_if(_peers[p].hashes, [tick](const auto& entry) { return entry.first >= tick; });
        ++_stats.leaves;
    }
    _decisions.push_back(decision);
    if (_decisions.size() > kept_decisions) {
        _decisions.pop_front();
    }
    ++_epoch;
    if (earliest < _tick) {
        // Ticks that ran with them (on a prediction) run again without them.
        _rollback_to = _rollback_to < 0 ? earliest : std::min<i64>(_rollback_to, earliest);
    }
    _next_report_us = 0;
}

void InputSyncCore::on_report(u32 slot, ByteReader& payload) {
    const u32 sender = payload.read_u8();
    const u32 epoch = payload.read_u32();
    const u32 undecided = payload.read_u32();
    const u32 frozen = payload.read_u32();
    const u32 all = (1u << _config.players) - 1;
    if (!payload.ok() || sender != slot || (frozen & ~all) != 0 || (undecided & ~frozen) != 0) {
        return;
    }
    std::array<i64, max_players> known{};
    for (u32 p = 0; p < _config.players; ++p) {
        if ((frozen >> p) & 1u) {
            known[p] = static_cast<i32>(payload.read_u32());
        }
    }
    if (!payload.ok()) {
        return;
    }
    PeerTrack& peer = _peers[slot];
    peer.reported = true;
    peer.report_epoch = epoch;
    peer.report_undecided = undecided;
    peer.report_frozen = frozen;
    peer.report_known = known;
    if (_status != SessionStatus::running) {
        return;
    }
    if (epoch < _epoch) {
        send_decisions(slot, epoch);
    }
    if (_active_until[slot] != never) {
        return; // a player the game let go: the decisions tell it so
    }
    // Who they lost, this peer loses too: a player some peer cannot reach leaves the game. Unless they lost everyone,
    // which is their own connection (they go).
    u32 others = 0;
    for (u32 p = 0; p < _config.players; ++p) {
        if (p != slot && in_game(p)) {
            others |= 1u << p;
        }
    }
    const bool cut_off = (others & ~undecided) == 0;
    const u32 local = _config.local_player;
    if (in_game(slot) && !lost(slot) && !cut_off) {
        for (u32 p = 0; p < _config.players; ++p) {
            if (((undecided >> p) & 1u) && p != local && in_game(p) && !lost(p)) {
                freeze(p, false);
            }
        }
    }
    // Their commands it lacks, of a player this peer lost too (or has let go already: it may be done before them).
    // Only a remaining player relays: what it has of them is what it reported (a joiner's and the lost player's own
    // are not counted when the game decides).
    if (in_game(local) && !lost(local)) {
        for (u32 p = 0; p < _config.players; ++p) {
            if (((frozen >> p) & 1u) && p != slot && p != local && (lost(p) || _active_until[p] != never)) {
                send_relay(slot, p, known[p]);
            }
        }
    }
}

void InputSyncCore::on_relay(u32 slot, ByteReader& payload) {
    const u32 player = payload.read_u8();
    const u32 first = payload.read_u32();
    const u32 count = payload.read_u8();
    const std::span<const u8> commands = payload.read_bytes(static_cast<std::size_t>(count) * _input_size);
    const u32 join_count = payload.read_u8();
    if (!payload.ok() || join_count >= _config.players) {
        return;
    }
    std::array<JoinEvent, max_players> joins;
    for (u32 i = 0; i < join_count; ++i) {
        joins[i].tick = payload.read_u32();
        joins[i].slot = payload.read_u8();
        joins[i].id = payload.read_u64();
        if (!payload.ok() || joins[i].slot >= _config.players) {
            return;
        }
    }
    if (_status != SessionStatus::running || player >= _config.players || player == slot ||
        player == _config.local_player || !lost(player) || !in_game(slot) || lost(slot)) {
        return;
    }
    take_commands(player, first, count, commands, std::span<const JoinEvent>(joins.data(), join_count), true);
}

void InputSyncCore::send_report() {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_u8(static_cast<u8>(_config.local_player));
    writer.write_u32(_epoch);
    writer.write_u32(_undecided);
    writer.write_u32(_frozen);
    for (u32 p = 0; p < _config.players; ++p) {
        if (lost(p)) {
            writer.write_u32(static_cast<u32>(static_cast<i32>(std::max<i64>(_known[p], -1))));
        }
    }
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot != _config.local_player && _peers[slot].linked && !lost(slot) && !_links.failed(_peers[slot].link)) {
            _links.send(_peers[slot].link, lost_message, payload);
        }
    }
    _next_report_us = _now_us + lost_repeat_us;
}

void InputSyncCore::send_relay(u32 to, u32 player, i64 they_have) {
    const PeerTrack& peer = _peers[to];
    if (!peer.linked || _links.failed(peer.link)) {
        return;
    }
    i64 last = _known[player];
    if (_active_until[player] != never) {
        last = std::min(last, _active_until[player] - 1);
    }
    const i64 first = std::max<i64>(they_have + 1, 0);
    const std::size_t fixed = relay_header + 1 + join_entry * (_config.players - 1);
    if (first > last || _links.max_payload() < fixed + _input_size) {
        return;
    }
    const u32 max_count = static_cast<u32>(std::min<std::size_t>(255, (_links.max_payload() - fixed) / _input_size));
    u32 count = 0;
    while (count < max_count && first + count <= last) {
        const u32 t = static_cast<u32>(first + count);
        const std::size_t index = static_cast<std::size_t>(player) * ring_ticks + t % ring_ticks;
        if (_input_tick[index] != t || _predicted[index]) {
            break; // gone from the ring (cannot happen: a lost player's commands stop near the present)
        }
        ++count;
    }
    if (count == 0) {
        return;
    }
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_u8(static_cast<u8>(player));
    writer.write_u32(static_cast<u32>(first));
    writer.write_u8(static_cast<u8>(count));
    for (u32 i = 0; i < count; ++i) {
        writer.write_bytes(input(player, static_cast<u32>(first + i)));
    }
    // The joins the lost player decided at those ticks (as its own inputs would have carried them).
    std::array<JoinEvent, max_players> joins;
    u32 join_count = 0;
    const auto add = [&](u32 slot, i64 from, u64 id) {
        if (from > 0 && from >= first && from < first + count && join_count + 1 < _config.players &&
            host_at(static_cast<u32>(from)) == player) {
            joins[join_count++] = {.tick = static_cast<u32>(from), .slot = slot, .id = id};
        }
    };
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (_active_from[slot] != never) {
            add(slot, _active_from[slot], _peers[slot].expected_id);
        }
    }
    for (const PastPlayer& past : _past) {
        add(past.slot, past.from, past.id);
    }
    writer.write_u8(static_cast<u8>(join_count));
    for (u32 i = 0; i < join_count; ++i) {
        writer.write_u32(joins[i].tick);
        writer.write_u8(static_cast<u8>(joins[i].slot));
        writer.write_u64(joins[i].id);
    }
    _links.send(peer.link, relay_message, payload);
}

void InputSyncCore::send_decisions(u32 to, u32 from_epoch) {
    const PeerTrack& peer = _peers[to];
    if (!peer.linked || _links.failed(peer.link)) {
        return;
    }
    const u32 oldest = _epoch - static_cast<u32>(_decisions.size());
    std::vector<u8> payload;
    for (u32 index = std::max(from_epoch, oldest); index < _epoch; ++index) {
        const LeaveDecision& decision = _decisions[index - oldest];
        payload.clear();
        ByteWriter writer(payload);
        writer.write_u32(index);
        writer.write_u32(decision.mask);
        for (u32 p = 0; p < _config.players; ++p) {
            if ((decision.mask >> p) & 1u) {
                writer.write_u32(decision.ticks[p]);
            }
        }
        _links.send(peer.link, leave_message, payload);
    }
}

} // namespace kin::net
