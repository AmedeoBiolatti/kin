#include <algorithm>
#include <cstring>
#include <kin/core/hash.hpp>
#include <kin/net/input_sync.hpp>
#include <limits>

namespace kin::net {
namespace {

// Model messages (after the 4-byte link header).
//   inputs: u8 sender slot, u32 sender's next tick, i16 how far the sender thinks it is ahead of the receiver
//           (1/256 ticks), u32 the next of the receiver's ticks the sender lacks, u32 first tick, u8 count,
//           count * input_size bytes (the sender's commands from the first tick on)
//           then u8 joins, joins * (u32 tick, u8 slot): the joins the sender decided at the ticks those commands are
//           for (a receiver takes a join with the sender's command for its tick, so every peer applies it there)
//   hash:   u8 sender slot, u32 tick, u64 state hash after that tick (confirmed)
//   join request (joiner -> every peer; the host answers): u8 joiner's slot
//   join state (host -> joiner): u8 joiner's slot, u32 base tick, u32 slots present at it, u32 total bytes,
//           u32 offset, the bytes from there
//   join ack (joiner -> host): u8 joiner's slot, u32 bytes received from the start
//   join ready (joiner -> host, once it has caught up): u8 joiner's slot
constexpr u8 inputs_message = first_model_message;
constexpr u8 hash_message = first_model_message + 1;
constexpr u8 join_request_message = first_model_message + 2;
constexpr u8 join_state_message = first_model_message + 3;
constexpr u8 join_ack_message = first_model_message + 4;
constexpr u8 join_ready_message = first_model_message + 5;
constexpr std::size_t inputs_header = 1 + 4 + 2 + 4 + 4 + 1;
constexpr std::size_t join_trailer = 1 + 8 * 5; // room kept for a few joins
constexpr std::size_t join_state_header = 1 + 4 + 4 + 4 + 4;

// Joining: how often requests and readiness are repeated, how many state bytes may be in flight, how long without
// progress before the host sends again from the last acknowledged byte, and how many ticks a joiner runs in one
// update while it catches up.
constexpr u64 join_repeat_us = 100'000;
constexpr u32 join_window_chunks = 16;
constexpr u64 join_resend_us = 60'000;
constexpr u32 join_catchup_ticks = 120;

// A peer that hears nothing new still sends this often, so acks and its clock keep flowing.
constexpr u64 keepalive_us = 50'000;
constexpr std::size_t kept_hashes = 64;

u64 config_hash(const InputSyncConfig& config, u32 input_size) {
    const u32 fields[] = {config.players,     input_size,           config.tick_rate,
                          config.input_delay, config.hash_interval, config.joinable ? 1u : 0u};
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
        _peers[slot].linked = true;
        _peers[slot].acked = static_cast<i64>(_config.input_delay) - 1;
        if (!_config.joining) {
            _active_from[slot] = 0;
        }
    }
    if (_config.joining) {
        // Nothing of ours is in the game yet: our commands start at the tick we join.
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
        if (in_game(slot)) {
            confirmed = std::min(confirmed, _known[slot]);
        }
    }
    return confirmed == never ? -1 : confirmed;
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

bool InputSyncCore::allow_join(u32 slot, PeerId peer) {
    if (!_config.joinable || slot >= _config.players || slot == _config.local_player || peer == no_peer ||
        session_over(_status)) {
        return false;
    }
    PeerTrack& track = _peers[slot];
    if (track.linked) {
        if (_links.link(track.link).peer == peer) {
            return true;
        }
        if (in_game(slot)) {
            return false; // taken
        }
        // Someone else was to join and has not: they give way (their link times out unheard).
        std::erase_if(_transfers, [slot](const Transfer& t) { return t.slot == slot; });
    }
    // Not in the game yet (or only just, as another peer decided): the link counts once the player plays.
    track = PeerTrack{};
    track.link = _links.add(peer, slot, !in_game(slot));
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
    if (const std::size_t failed = _links.first_failed(); failed < _links.links().size()) {
        _status = _links.link(failed).state == LinkState::rejected ? SessionStatus::rejected : SessionStatus::peer_lost;
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
    roll_back();
    confirm();
    compare_hashes();
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
        if (payload.read_u8() == slot && payload.ok()) {
            on_join_request(slot);
        }
    } else if (type == join_state_message) {
        on_join_state(slot, payload);
    } else if (type == join_ack_message) {
        on_join_ack(slot, payload);
    } else if (type == join_ready_message) {
        if (payload.read_u8() == slot && payload.ok()) {
            on_join_ready(slot);
        }
    }
}

void InputSyncCore::on_inputs(u32 slot, ByteReader& payload) {
    const u32 sender = payload.read_u8();
    const u32 sender_tick = payload.read_u32();
    const auto reported_advantage = static_cast<i16>(payload.read_u16());
    const u32 ack_next = payload.read_u32();
    const u32 first = payload.read_u32();
    const u32 count = payload.read_u8();
    const std::span<const u8> commands = payload.read_bytes(static_cast<std::size_t>(count) * _input_size);
    if (!payload.ok() || sender != slot || (_config.joining && !_have_state)) {
        return;
    }
    JoinEvent joins[8];
    u32 join_count = 0;
    if (payload.remaining() > 0) {
        const u32 n = payload.read_u8();
        for (u32 i = 0; i < n && i < std::size(joins); ++i) {
            joins[join_count].tick = payload.read_u32();
            joins[join_count].slot = payload.read_u8();
            if (!payload.ok() || joins[join_count].slot >= _config.players) {
                return;
            }
            ++join_count;
        }
    }
    PeerTrack& peer = _peers[slot];
    if (static_cast<i64>(sender_tick) >= peer.remote_tick) {
        peer.remote_tick = sender_tick;
        peer.remote_tick_at_us = _now_us;
    }
    peer.reported_advantage = static_cast<f32>(reported_advantage) / 256.0f;
    peer.acked = std::max(peer.acked, static_cast<i64>(ack_next) - 1);
    if (!in_game(slot)) {
        return; // a joiner still catching up: it sends only acks
    }
    // A tick this far ahead would overwrite ring slots still in use; the sender resends it later.
    const i64 limit = static_cast<i64>(_tick) + ring_ticks - _config.max_prediction - 4;
    for (u32 i = 0; i < count; ++i) {
        const i64 tick = static_cast<i64>(first) + i;
        if (tick <= _known[slot]) {
            continue;
        }
        if (tick != _known[slot] + 1 || tick >= limit) {
            if (_stats.spectating && tick > _known[slot] + 1 && peer.acked >= 0) {
                // The commands after our state are gone from the peer's history: the game moved on too far.
                start_failed("the game moved on before the state arrived: join again");
            }
            break;
        }
        const u32 t = static_cast<u32>(tick);
        for (u32 j = 0; j < join_count; ++j) {
            if (joins[j].tick == t) {
                apply_join(joins[j].slot, t); // with the command for its tick, as on every peer
            }
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
    if (!payload.ok() || sender != slot) {
        return;
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
    for (u32 player = 0; player < _config.players; ++player) {
        if (!present(player, tick)) {
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
    _hooks.step(_step_buffer, StepInfo{.tick = tick,
                                       .resimulating = resimulating,
                                       .confirmed = all_known,
                                       .present = _config.joinable ? present_mask(tick) : ~0u,
                                       .joined = joined});
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
        for (u32 slot = 0; slot < _config.players; ++slot) {
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
                return;
            }
        }
    }
}

f32 InputSyncCore::remote_now(u32 slot) const {
    // Where the peer's clock is now: its last report, plus the time since, plus half a round trip in flight.
    const PeerTrack& peer = _peers[slot];
    const f32 rtt_us = _links.link(peer.link).rtt_ms * 1000.0f;
    const f32 since_us = static_cast<f32>(_now_us - peer.remote_tick_at_us) + rtt_us * 0.5f;
    return static_cast<f32>(peer.remote_tick) + since_us / static_cast<f32>(_tick_us);
}

f32 InputSyncCore::own_view(u32 slot) const {
    if (!_peers[slot].linked || _peers[slot].remote_tick < 0) {
        return 0.0f;
    }
    return static_cast<f32>(_tick) - remote_now(slot);
}

f32 InputSyncCore::measure_advantage() const {
    f32 advantage = 0.0f;
    bool any = false;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        const PeerTrack& peer = _peers[slot];
        // A joiner catching up is far behind: it is not waited for.
        if (slot == _config.local_player || !peer.linked || peer.remote_tick < 0 || !in_game(slot) ||
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
        if (slot == local || !_peers[slot].linked) {
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
        writer.write_u32(static_cast<u32>(first));
        writer.write_u8(static_cast<u8>(count));
        for (std::size_t i = 0; i < count; ++i) {
            writer.write_bytes(input(local, static_cast<u32>(first + static_cast<i64>(i))));
        }
        if (_config.joinable) {
            // The joins we decided at the ticks of these commands travel with them.
            std::vector<JoinEvent> joins;
            for (const JoinEvent& join : _created) {
                if (static_cast<i64>(join.tick) >= first &&
                    static_cast<i64>(join.tick) < first + static_cast<i64>(count) && joins.size() < 5) {
                    joins.push_back(join);
                }
            }
            writer.write_u8(static_cast<u8>(joins.size()));
            for (const JoinEvent& join : joins) {
                writer.write_u32(join.tick);
                writer.write_u8(static_cast<u8>(join.slot));
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

void InputSyncCore::on_join_request(u32 slot) {
    if (!_config.joinable || _status != SessionStatus::running || in_game(slot) || host() != _config.local_player) {
        return;
    }
    for (Transfer& transfer : _transfers) {
        if (transfer.slot == slot) {
            transfer.progress_us = 0; // asked again: send what it waits for now
            return;
        }
    }
    // The state from before the first tick that is not yet final everywhere: from then on the joiner gets every
    // player's real commands, as the others do.
    const i64 confirmed = confirmed_tick();
    Transfer transfer{.slot = slot, .progress_us = _now_us};
    if (confirmed + 1 >= static_cast<i64>(_tick)) {
        transfer.base = _tick;
        _hooks.save(transfer.bytes);
    } else {
        transfer.base = static_cast<u32>(confirmed + 1);
        const std::size_t ring = transfer.base % _snapshots.size();
        if (_snapshot_tick[ring] != static_cast<i64>(transfer.base)) {
            return; // cannot happen: every tick past the confirmed one ran on a prediction and was saved
        }
        transfer.bytes = _snapshots[ring];
    }
    transfer.present = present_mask(transfer.base);
    // The joiner's commands to us start at its join tick; ours to it at the base.
    _peers[slot].acked = static_cast<i64>(transfer.base) - 1;
    _transfers.push_back(std::move(transfer));
}

void InputSyncCore::serve_transfers() {
    if (_links.max_payload() <= join_state_header) {
        return;
    }
    const u32 chunk = static_cast<u32>(_links.max_payload() - join_state_header);
    std::vector<u8> payload;
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
        }
    }
    // Every byte arrived: what is left is the joiner's catching up.
    std::erase_if(_transfers, [](const Transfer& t) { return t.acked >= t.bytes.size() && !t.bytes.empty(); });
}

void InputSyncCore::on_join_state(u32 slot, ByteReader& payload) {
    const u32 joiner = payload.read_u8();
    const u32 base = payload.read_u32();
    const u32 present_slots = payload.read_u32();
    const u32 total = payload.read_u32();
    const u32 offset = payload.read_u32();
    const std::span<const u8> bytes = payload.read_bytes(payload.remaining());
    if (!payload.ok() || !_config.joining || joiner != _config.local_player || _have_state) {
        return;
    }
    if (!_state_started || _state_from != slot || _state_base != base || _state_total != total ||
        _state_present != present_slots) {
        // The first chunk, or the host started over: begin again.
        _state_started = true;
        _state_from = slot;
        _state_base = base;
        _state_total = total;
        _state_present = present_slots;
        _state.clear();
        _state.reserve(total);
        _state_early.clear();
    }
    if (offset == _state.size() && _state.size() + bytes.size() <= total) {
        _state.insert(_state.end(), bytes.begin(), bytes.end());
        _stats.state_bytes_received += bytes.size();
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
        start_failed("the simulation refused the state the game sent");
        return;
    }
    _state = {};
    _state_early.clear();
    _tick = _state_base;
    _stats.start_tick = _state_base;
    _stats.spectating = true;
    _reported = static_cast<i64>(_state_base) - 1;
    _accumulator_us = 0;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if ((_state_present >> slot) & 1u) {
            _active_from[slot] = 0;
            _known[slot] = static_cast<i64>(_state_base) - 1;
            if (_peers[slot].linked) {
                _links.set_optional(_peers[slot].link, false); // in the game: needed from now on
            }
        }
    }
    std::ranges::fill(_snapshot_tick, -1);
    _status = SessionStatus::running;
}

u32 InputSyncCore::catch_up() {
    if (!_stats.spectating) {
        return 0;
    }
    f32 target = 0.0f;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot != _config.local_player && _peers[slot].linked && in_game(slot) && _peers[slot].remote_tick >= 0) {
            target = std::max(target, remote_now(slot));
        }
    }
    u32 ran = 0;
    while (static_cast<f32>(_tick) + 1.5f < target && ran < join_catchup_ticks && _status == SessionStatus::running &&
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
    if (!_have_state) {
        // Ask every player we reach; the game's host answers.
        for (u32 slot = 0; slot < _config.players; ++slot) {
            if (_peers[slot].linked) {
                _links.send(_peers[slot].link, join_request_message, payload);
            }
        }
        _next_join_message_us = _now_us + join_repeat_us;
        return;
    }
    // Ready once caught up and connected to everyone in the game: then the host picks the tick we join at.
    bool ready = _status == SessionStatus::running;
    for (u32 slot = 0; slot < _config.players && ready; ++slot) {
        if (slot != _config.local_player && in_game(slot)) {
            const PeerTrack& peer = _peers[slot];
            ready = peer.linked && _links.link(peer.link).state == LinkState::connected && peer.remote_tick >= 0 &&
                    remote_now(slot) - static_cast<f32>(_tick) < 2.5f;
        }
    }
    const u32 to = host();
    if (ready && to < _config.players && _peers[to].linked) {
        _links.send(_peers[to].link, join_ready_message, payload);
        _next_join_message_us = _now_us + join_repeat_us;
    }
}

void InputSyncCore::on_join_ready(u32 slot) {
    if (!_config.joinable || _status != SessionStatus::running || in_game(slot) || host() != _config.local_player ||
        !_peers[slot].linked) {
        return;
    }
    // The first tick none of our commands has been sent for: no peer can have finished it, and each takes the join
    // with our command for it.
    const u32 tick = static_cast<u32>(std::max<i64>(_known[_config.local_player] + 1, _tick));
    _created.push_back({.tick = tick, .slot = slot});
    apply_join(slot, tick);
}

void InputSyncCore::apply_join(u32 slot, u32 tick) {
    if (in_game(slot)) {
        return;
    }
    _active_from[slot] = tick;
    _known[slot] = static_cast<i64>(tick) - 1;
    ++_stats.joins;
    if (_peers[slot].linked) {
        _links.set_optional(_peers[slot].link, false); // it plays: losing it now ends the session, as for anyone
    }
    if (slot == _config.local_player) {
        // We play from `tick`: none of our commands before it exist, so every peer has all of them.
        _stats.spectating = false;
        for (PeerTrack& peer : _peers) {
            peer.acked = std::max(peer.acked, static_cast<i64>(tick) - 1);
        }
    }
    if (tick < _tick) {
        // Ticks that ran without the player run again with them.
        _rollback_to = _rollback_to < 0 ? tick : std::min<i64>(_rollback_to, tick);
    }
}

} // namespace kin::net
