#include <kin/net/input_sync.hpp>

#include <kin/core/hash.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace kin::net {
namespace {

// Model messages (after the 4-byte link header).
//   inputs: u8 sender slot, u32 sender's next tick, i16 how far the sender thinks it is ahead of the receiver
//           (1/256 ticks), u32 the next of the receiver's ticks the sender lacks, u32 first tick, u8 count,
//           count * input_size bytes (the sender's commands from the first tick on)
//   hash:   u8 sender slot, u32 tick, u64 state hash after that tick (confirmed)
constexpr u8 inputs_message = first_model_message;
constexpr u8 hash_message = first_model_message + 1;
constexpr std::size_t inputs_header = 1 + 4 + 2 + 4 + 4 + 1;

// A peer that hears nothing new still sends this often, so acks and its clock keep flowing.
constexpr u64 keepalive_us = 50'000;
constexpr std::size_t kept_hashes = 64;

u64 config_hash(const InputSyncConfig& config, u32 input_size) {
    const u32 fields[] = {config.players, input_size, config.tick_rate, config.input_delay, config.hash_interval};
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
    if (players > 1 && _config.peers.size() != players) {
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
    if (_input_size == 0 || inputs_header + _input_size > _links.max_payload()) {
        start_failed("a command must be 1 to " + std::to_string(_links.max_payload() - inputs_header) + " bytes");
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
    for (u32 slot = 0; slot < players; ++slot) {
        if (slot == _config.local_player) {
            continue;
        }
        if (_config.peers[slot] == no_peer) {
            start_failed("no transport peer for player " + std::to_string(slot));
            return;
        }
        _peers[slot].link = _links.add(_config.peers[slot], slot);
        _peers[slot].acked = static_cast<i64>(_config.input_delay) - 1;
    }
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
    return *std::ranges::min_element(_known);
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
    if (_status == SessionStatus::connecting) {
        if (!_links.all_connected()) {
            return;
        }
        _status = SessionStatus::running;
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
    confirm();
    compare_hashes();
    send_inputs();
}

void InputSyncCore::on_message(std::size_t link, u8 type, ByteReader& payload) {
    const u32 slot = _links.link(link).slot;
    if (type == inputs_message) {
        on_inputs(slot, payload);
    } else if (type == hash_message) {
        on_hash(slot, payload);
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
    if (!payload.ok() || sender != slot) {
        return;
    }
    PeerTrack& peer = _peers[slot];
    if (static_cast<i64>(sender_tick) >= peer.remote_tick) {
        peer.remote_tick = sender_tick;
        peer.remote_tick_at_us = _now_us;
    }
    peer.reported_advantage = static_cast<f32>(reported_advantage) / 256.0f;
    peer.acked = std::max(peer.acked, static_cast<i64>(ack_next) - 1);
    // A tick this far ahead would overwrite ring slots still in use; the sender resends it later.
    const i64 limit = static_cast<i64>(_tick) + ring_ticks - _config.max_prediction - 4;
    for (u32 i = 0; i < count; ++i) {
        const i64 tick = static_cast<i64>(first) + i;
        if (tick <= _known[slot]) {
            continue;
        }
        if (tick != _known[slot] + 1 || tick >= limit) {
            break;
        }
        const u32 t = static_cast<u32>(tick);
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
    const i64 target = static_cast<i64>(tick) + _config.input_delay;
    while (_known[local] < target) {
        const u32 next = static_cast<u32>(_known[local] + 1);
        std::ranges::copy(_local_input, input(local, next).begin());
        const std::size_t index = static_cast<std::size_t>(local) * ring_ticks + next % ring_ticks;
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
    for (const i64 known : _known) {
        all_known = all_known && known >= static_cast<i64>(tick);
    }
    if (!all_known && !_snapshots.empty()) {
        // A rollback may come back to this tick: keep the state it starts from.
        const std::size_t slot = tick % _snapshots.size();
        _hooks.save(_snapshots[slot]);
        _snapshot_tick[slot] = tick;
    }
    for (u32 player = 0; player < _config.players; ++player) {
        std::span<u8> command = input(player, tick);
        if (_known[player] < static_cast<i64>(tick)) {
            // Predict that the player still does what they did last: commands are levels, held for many ticks.
            if (_known[player] >= 0) {
                const std::span<u8> last = input(player, static_cast<u32>(_known[player]));
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
    _hooks.step(_step_buffer, StepInfo{.tick = tick, .resimulating = resimulating, .confirmed = all_known});
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
                const std::span<u8> command = input(player, tick);
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
            if (slot != _config.local_player) {
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

f32 InputSyncCore::own_view(u32 slot) const {
    const PeerTrack& peer = _peers[slot];
    if (peer.remote_tick < 0) {
        return 0.0f;
    }
    // Where the peer's clock is now: its last report, plus the time since, plus half a round trip in flight.
    const f32 rtt_us = _links.link(peer.link).rtt_ms * 1000.0f;
    const f32 since_us = static_cast<f32>(_now_us - peer.remote_tick_at_us) + rtt_us * 0.5f;
    const f32 remote_now = static_cast<f32>(peer.remote_tick) + since_us / static_cast<f32>(_tick_us);
    return static_cast<f32>(_tick) - remote_now;
}

f32 InputSyncCore::measure_advantage() const {
    f32 advantage = 0.0f;
    bool any = false;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        const PeerTrack& peer = _peers[slot];
        if (slot == _config.local_player || peer.remote_tick < 0) {
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
    const std::size_t max_count = std::min<std::size_t>(255, (_links.max_payload() - inputs_header) / _input_size);
    std::vector<u8> payload;
    for (u32 slot = 0; slot < _config.players; ++slot) {
        if (slot == local) {
            continue;
        }
        PeerTrack& peer = _peers[slot];
        // Every command the peer has not acknowledged, oldest first: a lost datagram costs nothing, the next one
        // carries the same commands again.
        const i64 first = std::max(peer.acked + 1, _known[local] - static_cast<i64>(ring_ticks) + 1);
        const i64 pending = std::max<i64>(0, _known[local] - first + 1);
        const std::size_t count = std::min<std::size_t>(static_cast<std::size_t>(pending), max_count);
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
        if (_links.send(peer.link, inputs_message, payload)) {
            peer.last_sent_us = _now_us;
            peer.last_ack_sent = ack_next;
        }
    }
    _input_changed = false;
}

} // namespace kin::net
