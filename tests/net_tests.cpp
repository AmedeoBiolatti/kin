#include <kin/core/hash.hpp>
#include <kin/core/rng.hpp>
#include <kin/net/bytes.hpp>
#include <kin/net/input_sync.hpp>
#include <kin/net/link.hpp>
#include <kin/net/loopback.hpp>
#include <kin/net/sync_test.hpp>
#ifndef __EMSCRIPTEN__
#include <kin/net/udp.hpp>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace kin;
using namespace kin::net;

// --- a small deterministic game: players walk, a ball bounces off walls and players, presses score ---

struct Command {
    i8 dx = 0;
    i8 dy = 0;
    u8 buttons = 0;
    friend bool operator==(const Command&, const Command&) = default;
};

struct ArenaState {
    std::array<i32, max_players> x{}, y{}, score{};
    std::array<u32, max_players> buttons{}; // last tick's, for edges
    i32 ball_x = 500, ball_y = 500, ball_vx = 7, ball_vy = 3;
    u32 tick = 0, players = 0;
    u64 rng = 0x1234;
};
static_assert(std::has_unique_object_representations_v<ArenaState>);

class Arena {
public:
    explicit Arena(u32 players) {
        _state.players = players;
        for (u32 p = 0; p < players; ++p) {
            _state.x[p] = 100 + 50 * static_cast<i32>(p);
            _state.y[p] = 100;
        }
    }

    void step(std::span<const Command> commands, const StepInfo& info) {
        // The session must run ticks in order from the state it restored.
        assert(info.tick == _state.tick);
        assert(commands.size() == _state.players);
        ++steps;
        if (info.resimulating) {
            ++resimulated;
        }
        ArenaState& s = _state;
        for (u32 p = 0; p < s.players; ++p) {
            const Command& c = commands[p];
            s.x[p] = std::clamp(s.x[p] + c.dx * 9, 0, 1000);
            s.y[p] = std::clamp(s.y[p] + c.dy * 9, 0, 1000);
            const u32 pressed = c.buttons & ~s.buttons[p];
            s.buttons[p] = c.buttons;
            if (pressed & 1) {
                s.score[p] += 1 + static_cast<i32>(s.rng & 3);
            }
            if (std::abs(s.x[p] - s.ball_x) < 60 && std::abs(s.y[p] - s.ball_y) < 60) {
                s.ball_vx = -s.ball_vx + static_cast<i32>(s.rng % 5) - 2;
                s.ball_vy = -s.ball_vy + static_cast<i32>((s.rng >> 8) % 5) - 2;
                s.score[p] += 10;
                s.rng = rng_detail::mix(s.rng ^ p);
            }
        }
        s.ball_x += s.ball_vx;
        s.ball_y += s.ball_vy;
        if (s.ball_x < 0 || s.ball_x > 1000) {
            s.ball_vx = -s.ball_vx;
            s.ball_x = std::clamp(s.ball_x, 0, 1000);
        }
        if (s.ball_y < 0 || s.ball_y > 1000) {
            s.ball_vy = -s.ball_vy;
            s.ball_y = std::clamp(s.ball_y, 0, 1000);
        }
        s.rng = rng_detail::mix(s.rng + s.tick);
        if (corrupt_at && s.tick == *corrupt_at) {
            s.score[0] += 1; // a nondeterminism only this peer has
        }
        ++s.tick;
        hash_after[s.tick - 1] = state_hash();
    }

    u64 state_hash() const { return hash_bytes(&_state, sizeof(_state)); }
    std::vector<u8> save() const {
        ++saves;
        const auto* bytes = reinterpret_cast<const u8*>(&_state);
        return {bytes, bytes + sizeof(_state)};
    }
    bool restore(std::span<const u8> bytes) {
        if (bytes.size() != sizeof(_state)) {
            return false;
        }
        std::memcpy(&_state, bytes.data(), sizeof(_state));
        ++restores;
        return true;
    }
    const ArenaState& state() const { return _state; }

    // Instrumentation, not state.
    mutable u64 saves = 0;
    u64 restores = 0, steps = 0, resimulated = 0;
    std::map<u32, u64> hash_after; // the latest run of each tick
    std::optional<u32> corrupt_at;

private:
    ArenaState _state;
};

static_assert(NetCommand<Command>);
static_assert(RollbackSim<Arena, Command>);
// The session holds references to itself: a copy or a move would leave them pointing at the original.
static_assert(!std::is_copy_constructible_v<InputSync<Arena, Command>>);
static_assert(!std::is_move_constructible_v<InputSync<Arena, Command>>);

// Lockstep only: no save/restore.
class Counter {
public:
    void step(std::span<const Command> commands) {
        for (const Command& c : commands) {
            _total += c.dx;
        }
    }
    u64 state_hash() const { return static_cast<u64>(_total); }

private:
    i64 _total = 0;
};
static_assert(DeterministicSim<Counter, Command>);
static_assert(!RollbackSim<Counter, Command>);

struct Padded {
    u8 a = 0;
    u32 b = 0;
};
static_assert(!NetCommand<Padded>, "padding bytes would differ between peers: such a command needs a CommandCodec");

// A command with a float, quantized by its codec.
struct Steer {
    f32 turn = 0.0f;
    bool boost = false;
};

} // namespace

namespace kin::net {
template <> struct CommandCodec<Steer> {
    static constexpr std::size_t size = 3;
    static void write(const Steer& steer, std::span<u8, size> out) {
        const auto turn = static_cast<i16>(std::lround(std::clamp(steer.turn, -1.0f, 1.0f) * 1000.0f));
        out[0] = static_cast<u8>(static_cast<u16>(turn));
        out[1] = static_cast<u8>(static_cast<u16>(turn) >> 8);
        out[2] = steer.boost ? 1 : 0;
    }
    static Steer read(std::span<const u8, size> in) {
        const auto turn = static_cast<i16>(static_cast<u16>(in[0] | (in[1] << 8)));
        return {.turn = static_cast<f32>(turn) / 1000.0f, .boost = in[2] != 0};
    }
};
} // namespace kin::net

namespace {

class SteerSim {
public:
    void step(std::span<const Steer> steers) {
        for (const Steer& s : steers) {
            _heading += s.turn;
            _boosts += s.boost ? 1 : 0;
        }
    }
    u64 state_hash() const { return hash_bytes(&_heading, sizeof(_heading)) ^ _boosts; }
    f32 heading() const { return _heading; }

private:
    f32 _heading = 0.0f;
    u64 _boosts = 0;
};
static_assert(NetCommand<Steer>);
static_assert(DeterministicSim<SteerSim, Steer>);

// What each player does: hold a random direction and button for 7 ticks' worth of time, then pick again.
Command script(u32 player, u64 now_ms) {
    const u64 h = rng_detail::mix(player * 1000003ull + (now_ms / 16) / 7);
    return {.dx = static_cast<i8>(static_cast<i32>(h % 3) - 1),
            .dy = static_cast<i8>(static_cast<i32>((h >> 8) % 3) - 1),
            .buttons = static_cast<u8>((h >> 16) & 1)};
}

struct Peer {
    explicit Peer(u32 players) : arena(players) {}
    Arena arena;
    std::unique_ptr<InputSync<Arena, Command>> session;
    std::vector<std::vector<Command>> log; // log[tick]: every player's confirmed command
    bool frozen = false;                   // not updated (a hitch, or not started yet)
    u32 max_ticks_per_update = 0;          // the most ticks one update() moved the clock on
};

// A transport that keeps the last datagram its session sent: a test reads the header a real message carries.
struct Tap final : Transport {
    explicit Tap(Transport& inner) : inner(inner) {}
    bool send(PeerId to, std::span<const u8> bytes) override {
        last_sent.assign(bytes.begin(), bytes.end());
        return inner.send(to, bytes);
    }
    void receive(std::vector<Datagram>& out) override { inner.receive(out); }
    std::size_t max_datagram() const override { return inner.max_datagram(); }
    Transport& inner;
    std::vector<u8> last_sent;
};

struct Match {
    LoopbackNetwork network;
    std::vector<LoopbackEndpoint*> endpoints;
    std::vector<std::unique_ptr<Tap>> taps;
    std::vector<std::unique_ptr<Peer>> peers;
    u64 now_ms = 0;
    std::function<Command(u32 player, u64 now_ms)> input = script;

    Match(u32 players, const InputSyncConfig& base, u64 seed = 7,
          const std::function<void(u32 player, InputSyncConfig&)>& adjust = {})
        : network(seed) {
        for (u32 i = 0; i < players; ++i) {
            endpoints.push_back(&network.add_endpoint());
            taps.push_back(std::make_unique<Tap>(*endpoints.back()));
        }
        for (u32 i = 0; i < players; ++i) {
            InputSyncConfig config = base;
            config.players = players;
            config.local_player = i;
            config.peers.clear();
            for (u32 j = 0; j < players; ++j) {
                config.peers.push_back(LoopbackNetwork::peer(j));
            }
            if (adjust) {
                adjust(i, config);
            }
            auto peer = std::make_unique<Peer>(players);
            peer->session = std::make_unique<InputSync<Arena, Command>>(peer->arena, *taps[i], config);
            Peer* raw = peer.get();
            peer->session->set_on_confirmed([raw](u32 tick, std::span<const Command> commands) {
                assert(tick == raw->log.size());
                raw->log.emplace_back(commands.begin(), commands.end());
            });
            peers.push_back(std::move(peer));
        }
    }

    void step_ms() {
        ++now_ms;
        network.set_time(now_ms * 1000);
        for (u32 i = 0; i < peers.size(); ++i) {
            Peer& peer = *peers[i];
            if (peer.frozen) {
                continue;
            }
            peer.session->set_local_input(input(i, now_ms));
            const u32 before = peer.session->stats().tick;
            peer.session->update(now_ms * 1000);
            peer.max_ticks_per_update = std::max(peer.max_ticks_per_update, peer.session->stats().tick - before);
        }
    }

    // Every peer has run past `tick` and confirmed it.
    bool all_reached(u32 tick) const {
        for (const auto& peer : peers) {
            const InputSyncStats& stats = peer->session->stats();
            if (stats.tick <= tick || stats.confirmed_tick < static_cast<i64>(tick)) {
                return false;
            }
        }
        return true;
    }

    bool run_until(u32 tick, u64 limit_ms) {
        const u64 end = now_ms + limit_ms;
        while (now_ms < end) {
            step_ms();
            if (all_reached(tick)) {
                return true;
            }
        }
        return false;
    }

    void run_for(u64 ms) {
        for (u64 i = 0; i < ms; ++i) {
            step_ms();
        }
    }

    // Every peer agrees on the commands of the first `ticks` ticks, and each one's state after each tick is what
    // a single offline run of those commands gives.
    void check_against_reference(u32 ticks) const {
        const u32 players = static_cast<u32>(peers.size());
        for (const auto& peer : peers) {
            assert(peer->log.size() >= ticks);
            for (u32 t = 0; t < ticks; ++t) {
                assert(peer->log[t] == peers[0]->log[t]);
            }
        }
        Arena reference(players);
        for (u32 t = 0; t < ticks; ++t) {
            reference.step(peers[0]->log[t], StepInfo{.tick = t});
            for (const auto& peer : peers) {
                assert(peer->arena.hash_after.at(t) == reference.hash_after.at(t));
            }
        }
    }
};

void print_stats(const char* name, const Match& match) {
    for (std::size_t i = 0; i < match.peers.size(); ++i) {
        const InputSyncStats& s = match.peers[i]->session->stats();
        std::printf("  %s peer %zu: tick %u confirmed %lld rollbacks %llu (resim %llu, max %u) waits %llu slowed %llu "
                    "hashes %llu advantage %.2f\n",
                    name, i, s.tick, static_cast<long long>(s.confirmed_tick),
                    static_cast<unsigned long long>(s.rollbacks), static_cast<unsigned long long>(s.resimulated_ticks),
                    s.max_rollback, static_cast<unsigned long long>(s.waits),
                    static_cast<unsigned long long>(s.slowed_updates),
                    static_cast<unsigned long long>(s.hashes_compared), s.advantage);
    }
}

// --- bytes ---

void test_bytes() {
    std::vector<u8> bytes;
    ByteWriter writer(bytes);
    writer.write_u8(0xab);
    writer.write_u16(0xbeef);
    writer.write_u32(0xdeadbeef);
    writer.write_u64(0x0123456789abcdefull);
    writer.write_f32(-1.5f);
    const u64 varints[] = {0, 1, 127, 128, 300, 1ull << 35, ~0ull};
    for (const u64 v : varints) {
        writer.write_varint(v);
    }
    assert(bytes[1] == 0xef && bytes[2] == 0xbe); // little-endian
    ByteReader reader(bytes);
    assert(reader.read_u8() == 0xab);
    assert(reader.read_u16() == 0xbeef);
    assert(reader.read_u32() == 0xdeadbeef);
    assert(reader.read_u64() == 0x0123456789abcdefull);
    assert(reader.read_f32() == -1.5f);
    for (const u64 v : varints) {
        assert(reader.read_varint() == v);
    }
    assert(reader.ok() && reader.at_end());
    // Past the end: not ok, zeros from then on.
    assert(reader.read_u32() == 0 && !reader.ok());
    assert(reader.read_u8() == 0 && reader.read_bytes(1).empty());

    const std::vector<u8> short_bytes = {1, 2, 3};
    ByteReader short_reader(short_bytes);
    assert(short_reader.read_u32() == 0 && !short_reader.ok());

    const std::vector<u8> endless(11, 0x80);
    ByteReader endless_reader(endless);
    assert(endless_reader.read_varint() == 0 && !endless_reader.ok());
    const std::vector<u8> too_big = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
    ByteReader too_big_reader(too_big);
    assert(too_big_reader.read_varint() == 0 && !too_big_reader.ok());
}

// --- the loopback network ---

void test_loopback() {
    LoopbackNetwork network{3};
    LoopbackEndpoint& a = network.add_endpoint();
    LoopbackEndpoint& b = network.add_endpoint();
    const std::vector<u8> hello = {1, 2, 3};
    std::vector<Datagram> received;

    network.set_conditions({.latency_us = 10'000});
    assert(a.send(LoopbackNetwork::peer(1), hello));
    network.set_time(9'999);
    b.receive(received);
    assert(received.empty());
    network.set_time(10'000);
    b.receive(received);
    assert(received.size() == 1 && received[0].from == LoopbackNetwork::peer(0) && received[0].bytes == hello);
    a.receive(received);
    assert(received.size() == 1); // nothing for a

    // Unknown peers, itself and oversized datagrams are refused.
    assert(!a.send(no_peer, hello));
    assert(!a.send(LoopbackNetwork::peer(0), hello));
    assert(!a.send(LoopbackNetwork::peer(5), hello));
    assert(!a.send(LoopbackNetwork::peer(1), std::vector<u8>(1201)));
    assert(a.send(LoopbackNetwork::peer(1), std::vector<u8>(1200)));

    // Loss, duplication and a cut link.
    received.clear();
    network.set_time(100'000);
    b.receive(received);
    received.clear();
    network.set_conditions({.loss = 1.0f});
    assert(a.send(LoopbackNetwork::peer(1), hello)); // accepted, then lost
    network.set_conditions({.duplicate = 1.0f});
    assert(a.send(LoopbackNetwork::peer(1), hello));
    network.set_conditions({});
    network.set_connected(0, 1, false);
    assert(a.send(LoopbackNetwork::peer(1), hello));
    network.set_connected(0, 1, true);
    b.receive(received);
    assert(received.size() == 2);
    assert(network.stats().dropped == 2 && network.stats().duplicated == 1);

    // One direction's conditions leave the other's alone.
    network.set_conditions(1, 0, {.latency_us = 50'000});
    assert(a.send(LoopbackNetwork::peer(1), hello) && b.send(LoopbackNetwork::peer(0), hello));
    received.clear();
    b.receive(received);
    assert(received.size() == 1);
    received.clear();
    a.receive(received);
    assert(received.empty());
    network.set_time(150'000);
    a.receive(received);
    assert(received.size() == 1);

    // The same seed loses the same datagrams; jitter reorders them.
    const auto play = [](u64 seed) {
        LoopbackNetwork net{seed};
        LoopbackEndpoint& from = net.add_endpoint();
        LoopbackEndpoint& to = net.add_endpoint();
        net.set_conditions({.latency_us = 1000, .jitter_us = 20'000, .loss = 0.3f});
        for (u8 i = 0; i < 100; ++i) {
            from.send(LoopbackNetwork::peer(1), std::vector<u8>{i});
        }
        net.set_time(1'000'000);
        std::vector<Datagram> got;
        to.receive(got);
        std::vector<u8> order;
        for (const Datagram& d : got) {
            order.push_back(d.bytes[0]);
        }
        return order;
    };
    const auto first = play(11);
    assert(first == play(11));
    assert(first != play(12));
    assert(first.size() > 50 && first.size() < 90);
    assert(!std::ranges::is_sorted(first));
}

// --- handshakes ---

struct LinkPair {
    LoopbackNetwork network{5};
    LoopbackEndpoint& a = network.add_endpoint();
    LoopbackEndpoint& b = network.add_endpoint();
    std::unique_ptr<Links> links_a, links_b;
    u64 now_us = 0;

    LinkPair(SessionIdentity id_a, SessionIdentity id_b, u32 slot_b_expected = 1, LinksConfig config = {}) {
        start(id_a, id_b, slot_b_expected, config);
    }
    // New links on the same endpoints: the next match between the same two addresses.
    void start(SessionIdentity id_a, SessionIdentity id_b, u32 slot_b_expected = 1, LinksConfig config = {}) {
        LinksConfig config_a = config;
        config_a.local_slot = 0;
        LinksConfig config_b = config;
        config_b.local_slot = 1;
        links_a = std::make_unique<Links>(a, id_a, config_a);
        links_b = std::make_unique<Links>(b, id_b, config_b);
        links_a->add(LoopbackNetwork::peer(1), slot_b_expected);
        links_b->add(LoopbackNetwork::peer(0), 0);
    }
    void run(u64 ms, bool update_b = true) {
        for (u64 i = 0; i < ms; ++i) {
            now_us += 1000;
            network.set_time(now_us);
            links_a->update(now_us, {});
            if (update_b) {
                links_b->update(now_us, {});
            }
        }
    }
};

void test_handshake() {
    const SessionIdentity id{.model = 1, .game = compatibility_hash("arena", "1.0"), .config = 9, .session = 4};
    {
        LinkPair pair(id, id);
        pair.network.set_conditions({.latency_us = 20'000, .jitter_us = 5'000, .loss = 0.2f});
        pair.run(2000);
        assert(pair.links_a->all_connected() && pair.links_b->all_connected());
        const f32 rtt = pair.links_a->link(0).rtt_ms;
        assert(rtt > 39.0f && rtt < 52.0f);
        // Goodbye ends it for the other side at once.
        pair.links_b->leave();
        pair.run(100);
        assert(pair.links_a->link(0).state == LinkState::left);
    }
    {
        SessionIdentity other = id;
        other.game = compatibility_hash("arena", "1.1");
        assert(other.game != id.game);
        LinkPair pair(id, other);
        pair.run(500);
        assert(pair.links_a->link(0).state == LinkState::rejected);
        assert(pair.links_a->link(0).reason == RejectReason::game);
        assert(pair.links_b->link(0).state == LinkState::rejected);
        assert(pair.links_b->link(0).reason == RejectReason::game);
        assert(pair.links_a->first_failed() == 0);
    }
    {
        SessionIdentity other = id;
        other.model = 2;
        LinkPair pair(id, other);
        pair.run(500);
        assert(pair.links_a->link(0).reason == RejectReason::model);
    }
    {
        // a expects b in slot 2; b says it is slot 1.
        LinkPair pair(id, id, 2);
        pair.run(500);
        assert(pair.links_a->link(0).state == LinkState::rejected);
        assert(pair.links_a->link(0).reason == RejectReason::slot);
    }
    {
        // b never answers.
        LinkPair pair(id, id, 1, {.connect_timeout_us = 1'000'000});
        pair.run(1500, false);
        assert(pair.links_a->link(0).state == LinkState::timed_out);
    }
    {
        // A connected peer that goes quiet times out.
        LinkPair pair(id, id, 1, {.timeout_us = 300'000});
        pair.run(500);
        assert(pair.links_a->all_connected());
        pair.network.set_connected(1, 0, false);
        pair.run(400);
        assert(pair.links_a->link(0).state == LinkState::timed_out);
    }
    {
        // Rejects are lost half the time: each side still learns the reason, long before the connect timeout.
        SessionIdentity other = id;
        other.config = 10;
        LinkPair pair(id, other);
        pair.network.set_conditions({.latency_us = 10'000, .loss = 0.5f});
        pair.run(1500);
        assert(pair.links_a->link(0).state == LinkState::rejected);
        assert(pair.links_a->link(0).reason == RejectReason::config);
        assert(pair.links_b->link(0).state == LinkState::rejected);
        assert(pair.links_b->link(0).reason == RejectReason::config);
    }
    {
        // A slot only one side objects to (a expects b in slot 2): both still end rejected.
        LinkPair pair(id, id, 2);
        pair.network.set_conditions({.latency_us = 10'000, .loss = 0.5f});
        pair.run(1500);
        assert(pair.links_a->link(0).reason == RejectReason::slot);
        assert(pair.links_b->link(0).state == LinkState::rejected);
    }
    {
        // A peer of another protocol version.
        LinkPair pair(id, id);
        pair.links_b.reset();
        for (int i = 0; i < 5; ++i) {
            const std::vector<u8> hello = {'k', 'n', protocol_version + 1, 1};
            pair.b.send(LoopbackNetwork::peer(0), hello);
            pair.run(20, false);
        }
        assert(pair.links_a->link(0).state == LinkState::rejected);
        assert(pair.links_a->link(0).reason == RejectReason::protocol);
    }
    assert(compatibility_hash("ab", "c") != compatibility_hash("a", "bc"));
}

// Datagrams that look like the links' own messages but are not from this match's peer: late ones from an earlier
// match between the same addresses, and forged ones from the peer's address. None may end or change a link.
void test_stale_and_forged_messages() {
    const SessionIdentity id{.model = 1, .game = 7, .config = 9, .session = 4};
    const auto header = [](u8 version, u8 type, u64 token) {
        std::vector<u8> bytes = {'k', 'n', version, type};
        for (int i = 0; i < 8; ++i) {
            bytes.push_back(static_cast<u8>(token >> (8 * i)));
        }
        return bytes;
    };
    {
        LinkPair pair(id, id);
        pair.run(300);
        assert(pair.links_a->all_connected() && pair.links_b->all_connected());
        const u64 tokens[] = {0, 1, 0x0123456789abcdefull};
        for (const u64 token : tokens) {
            pair.b.send(LoopbackNetwork::peer(0), header(protocol_version, 5, token)); // bye
            std::vector<u8> reject = header(protocol_version, 2, token);
            reject.push_back(static_cast<u8>(RejectReason::protocol));
            pair.b.send(LoopbackNetwork::peer(0), reject);
            std::vector<u8> hello = header(protocol_version, 1, token); // another session's hello
            ByteWriter writer(hello);
            writer.write_u32(id.model);
            writer.write_u64(id.game);
            writer.write_u64(id.config);
            writer.write_u64(id.session + 1);
            writer.write_u32(1);
            writer.write_u64(token | 1);
            pair.b.send(LoopbackNetwork::peer(0), hello);
        }
        pair.b.send(LoopbackNetwork::peer(0), header(protocol_version + 1, 1, 0)); // another version's hello
        pair.b.send(LoopbackNetwork::peer(0), std::vector<u8>{'k', 'n', protocol_version, 5}); // a bye, cut short
        pair.run(50);
        assert(pair.links_a->all_connected());
        assert(pair.links_a->link(0).reason == RejectReason::none);
    }
    {
        // b leaves the first match; its goodbyes are still on the way when the same two addresses start the next one.
        LinkPair pair(id, id);
        pair.network.set_conditions({.latency_us = 50'000});
        pair.run(500);
        assert(pair.links_a->all_connected());
        pair.links_b->leave();
        pair.start(id, id);
        pair.run(500);
        assert(pair.links_a->all_connected() && pair.links_b->all_connected());
    }
    {
        // Hellos of the first match (session 4) arrive while the next one (session 5) handshakes.
        SessionIdentity next = id;
        next.session = 5;
        LinkPair pair(id, id);
        pair.network.set_conditions({.latency_us = 50'000});
        pair.run(30);
        pair.start(next, next);
        pair.run(500);
        assert(pair.links_a->all_connected() && pair.links_b->all_connected());
    }
    assert(compatibility_hash("a", "b", "", 1) != compatibility_hash("a", "b", "", 2));
}

// --- input sync ---

void test_single_player() {
    LoopbackNetwork network;
    LoopbackEndpoint& endpoint = network.add_endpoint();
    Arena arena(1);
    InputSync<Arena, Command> session(arena, endpoint, {.players = 1, .local_player = 0});
    for (u64 ms = 1; ms <= 1000; ++ms) {
        session.set_local_input({.dx = 1});
        session.update(ms * 1000);
    }
    assert(session.status() == SessionStatus::running);
    assert(session.stats().tick >= 59 && session.stats().tick <= 61);
    assert(session.stats().rollbacks == 0 && arena.saves == 0);
    assert(network.stats().sent == 0);
    // Commands reach the game input_delay ticks after they are given.
    assert(arena.state().x[0] == 100 + 9 * static_cast<i32>(session.stats().tick - session.config().input_delay));
}

void test_lockstep() {
    InputSyncConfig config{.input_delay = 3, .max_prediction = 0, .hash_interval = 30};
    {
        Match match(2, config);
        match.network.set_conditions({.latency_us = 25'000, .jitter_us = 5'000});
        assert(match.run_until(600, 30'000));
        print_stats("lockstep 25 ms", match);
        match.check_against_reference(600);
        for (const auto& peer : match.peers) {
            assert(peer->session->status() == SessionStatus::running);
            assert(peer->arena.saves == 0 && peer->arena.restores == 0 && peer->arena.resimulated == 0);
            assert(peer->session->stats().hashes_compared >= 15);
        }
    }
    {
        // A round trip longer than the delay: lockstep waits, and stays correct.
        Match match(3, config);
        match.network.set_conditions({.latency_us = 70'000, .jitter_us = 20'000, .loss = 0.05f});
        assert(match.run_until(300, 60'000));
        print_stats("lockstep 70 ms", match);
        match.check_against_reference(300);
        u64 waits = 0;
        for (const auto& peer : match.peers) {
            waits += peer->session->stats().waits;
            assert(peer->arena.resimulated == 0);
            // The clock stops while it waits: it never runs the waited-for ticks in a burst afterwards.
            assert(peer->max_ticks_per_update == 1);
        }
        assert(waits > 0);
    }
}

void test_rollback() {
    InputSyncConfig config{.input_delay = 1, .max_prediction = 8, .hash_interval = 20};
    {
        Match match(2, config);
        match.network.set_conditions({.latency_us = 50'000, .jitter_us = 15'000, .loss = 0.1f, .duplicate = 0.05f});
        assert(match.run_until(900, 60'000));
        print_stats("rollback 2p", match);
        match.check_against_reference(900);
        for (const auto& peer : match.peers) {
            const InputSyncStats& stats = peer->session->stats();
            assert(peer->session->status() == SessionStatus::running);
            assert(stats.rollbacks > 10);
            assert(stats.max_rollback <= config.max_prediction);
            assert(peer->arena.resimulated == stats.resimulated_ticks);
            assert(stats.hashes_compared >= 35);
        }
    }
    {
        // Four players, each link its own conditions.
        Match match(4, config, 21);
        for (std::size_t from = 0; from < 4; ++from) {
            for (std::size_t to = 0; to < 4; ++to) {
                const u32 latency = static_cast<u32>(10'000 + 15'000 * ((from + to) % 4));
                match.network.set_conditions(from, to, {.latency_us = latency, .jitter_us = 8'000, .loss = 0.05f});
            }
        }
        assert(match.run_until(600, 60'000));
        print_stats("rollback 4p", match);
        match.check_against_reference(600);
        for (const auto& peer : match.peers) {
            assert(peer->session->status() == SessionStatus::running);
            assert(peer->session->stats().rollbacks > 0);
        }
    }
    {
        // Held commands are predicted right: only the first change of each player can roll back.
        Match match(2, config);
        match.input = [](u32 player, u64 now_ms) {
            return now_ms < 1000 ? Command{} : Command{.dx = static_cast<i8>(player == 0 ? 1 : -1), .buttons = 1};
        };
        match.network.set_conditions({.latency_us = 50'000, .jitter_us = 10'000});
        assert(match.run_until(300, 30'000));
        match.check_against_reference(300);
        for (const auto& peer : match.peers) {
            assert(peer->session->stats().rollbacks <= 2);
        }
    }
    {
        // Latency within the input delay: nothing to predict, nothing rolls back.
        InputSyncConfig delayed = config;
        delayed.input_delay = 4;
        Match match(2, delayed);
        match.network.set_conditions({.latency_us = 20'000});
        assert(match.run_until(300, 30'000));
        match.check_against_reference(300);
        for (const auto& peer : match.peers) {
            assert(peer->session->stats().rollbacks == 0);
        }
    }
}

void test_desync() {
    Match match(2, {.input_delay = 2, .max_prediction = 4, .hash_interval = 30});
    match.network.set_conditions({.latency_us = 30'000, .jitter_us = 5'000});
    match.peers[1]->arena.corrupt_at = 100;
    match.run_for(5000);
    for (const auto& peer : match.peers) {
        assert(peer->session->status() == SessionStatus::desynced);
        // The first compared state after the corruption: after tick 119 ((119 + 1) % 30 == 0).
        assert(peer->session->desync().tick == 119);
        assert(peer->session->desync().local_hash != peer->session->desync().remote_hash);
        assert(peer->session->status_detail().find("tick 119") != std::string::npos);
        // Nothing runs after a desync.
        assert(peer->session->stats().tick < 160);
    }
    assert(match.peers[0]->session->desync().player == 1);
    assert(match.peers[1]->session->desync().player == 0);
}

void test_time_sync() {
    Match match(2, {.input_delay = 2, .max_prediction = 8, .hash_interval = 30});
    match.network.set_conditions({.latency_us = 30'000, .jitter_us = 4'000});
    // Peer 1 starts half a second late, then has a 400 ms hitch.
    match.peers[1]->frozen = true;
    match.run_for(500);
    match.peers[1]->frozen = false;
    match.run_for(3000);
    match.peers[1]->frozen = true;
    match.run_for(400);
    match.peers[1]->frozen = false;
    match.run_for(8000);
    print_stats("time sync", match);
    const InputSyncStats& a = match.peers[0]->session->stats();
    const InputSyncStats& b = match.peers[1]->session->stats();
    assert(match.peers[0]->session->status() == SessionStatus::running);
    assert(match.peers[1]->session->status() == SessionStatus::running);
    // The clocks came back together: within a few ticks, and the peer ahead slowed down for it.
    assert(std::abs(static_cast<i64>(a.tick) - static_cast<i64>(b.tick)) <= 4);
    assert(a.slowed_updates > 0);
    assert(std::abs(a.advantage) < 2.0f && std::abs(b.advantage) < 2.0f);
    const u32 checked = static_cast<u32>(std::min(a.confirmed_tick, b.confirmed_tick));
    match.check_against_reference(std::min(checked, std::min(a.tick, b.tick) - 1));
}

void test_peer_lost_and_leave() {
    InputSyncConfig config{.input_delay = 2, .max_prediction = 8};
    config.links.timeout_us = 500'000;
    {
        Match match(2, config);
        assert(match.run_until(120, 10'000));
        match.network.set_connected(0, 1, false);
        match.network.set_connected(1, 0, false);
        match.run_for(1000);
        for (const auto& peer : match.peers) {
            assert(peer->session->status() == SessionStatus::peer_lost);
            assert(peer->session->status_detail().find("timed out") != std::string::npos);
        }
    }
    {
        Match match(2, config);
        assert(match.run_until(60, 10'000));
        match.peers[0]->session->leave();
        match.run_for(200);
        assert(match.peers[0]->session->status() == SessionStatus::ended);
        assert(match.peers[1]->session->status() == SessionStatus::peer_lost);
        assert(match.peers[1]->session->status_detail().find("left") != std::string::npos);
    }
}

void test_rejected_and_failed() {
    {
        Match match(2, {.game = compatibility_hash("arena", "1.0")}, 7, [](u32 player, InputSyncConfig& config) {
            if (player == 1) {
                config.game = compatibility_hash("arena", "1.1");
            }
        });
        match.run_for(1000);
        for (const auto& peer : match.peers) {
            assert(peer->session->status() == SessionStatus::rejected);
            assert(peer->session->status_detail().find("another game") != std::string::npos);
            assert(peer->session->stats().tick == 0);
        }
    }
    {
        // Settings that must match: the input delay.
        Match match(2, {}, 7, [](u32 player, InputSyncConfig& config) {
            if (player == 1) {
                config.input_delay = 3;
            }
        });
        match.run_for(1000);
        assert(match.peers[0]->session->status() == SessionStatus::rejected);
        assert(match.peers[0]->session->status_detail().find("settings") != std::string::npos);
    }
    LoopbackNetwork network;
    LoopbackEndpoint& endpoint = network.add_endpoint();
    Arena arena(2);
    {
        InputSync<Arena, Command> session(arena, endpoint, {.players = 2, .local_player = 2});
        assert(session.status() == SessionStatus::failed);
        session.update(1000);
        assert(session.status() == SessionStatus::failed);
    }
    {
        InputSync<Arena, Command> session(arena, endpoint, {.players = 2, .peers = {no_peer}});
        assert(session.status() == SessionStatus::failed);
    }
    {
        InputSync<Arena, Command> session(arena, endpoint, {.players = 1, .input_delay = 31});
        assert(session.status() == SessionStatus::failed);
    }
    {
        // Two players at one address: its datagrams could reach only one of them.
        Arena three(3);
        const PeerId twice = LoopbackNetwork::peer(1);
        InputSync<Arena, Command> session(three, endpoint, {.players = 3, .peers = {no_peer, twice, twice}});
        assert(session.status() == SessionStatus::failed);
        assert(session.status_detail().find("same transport peer") != std::string::npos);
    }
    {
        // A lockstep-only simulation cannot roll back.
        Counter counter;
        InputSync<Counter, Command> rollback(counter, endpoint, {.players = 1, .max_prediction = 4});
        assert(rollback.status() == SessionStatus::failed);
        assert(rollback.status_detail().find("lockstep") != std::string::npos);
        InputSync<Counter, Command> lockstep(counter, endpoint, {.players = 1, .max_prediction = 0});
        assert(lockstep.status() == SessionStatus::connecting);
        lockstep.update(1000);
        assert(lockstep.status() == SessionStatus::running);
    }
}

void test_codec_command() {
    LoopbackNetwork network;
    LoopbackEndpoint& endpoint = network.add_endpoint();
    SteerSim sim;
    InputSync<SteerSim, Steer> session(sim, endpoint, {.players = 1, .input_delay = 0, .max_prediction = 0});
    static_assert(InputSync<SteerSim, Steer>::command_size == 3);
    static_assert(!InputSync<SteerSim, Steer>::can_rollback);
    std::vector<Steer> confirmed;
    session.set_on_confirmed([&](u32, std::span<const Steer> steers) { confirmed.push_back(steers[0]); });
    for (u64 ms = 1; ms <= 100; ++ms) {
        session.set_local_input({.turn = 0.12345f, .boost = true});
        session.update(ms * 1000);
    }
    // The clock starts at the first update: 99 ms of 16.7 ms ticks.
    const u32 ticks = session.stats().tick;
    assert(ticks == 5);
    assert(!confirmed.empty());
    // The command arrives as its codec wrote it: quantized to a thousandth.
    assert(confirmed.back().turn == 0.123f && confirmed.back().boost);
    assert(std::fabs(sim.heading() - 0.123f * static_cast<f32>(ticks)) < 1e-5f);
}

void test_garbage() {
    Match match(2, {.input_delay = 2, .max_prediction = 8, .hash_interval = 30});
    match.network.set_conditions({.latency_us = 20'000});
    assert(match.run_until(60, 10'000));
    // Datagrams from peer 1's address that are not what it would send: random bytes, cut-off messages, another
    // player's slot. They carry the token peer 0 expects (read off a real message), so they reach the parsers. None
    // may crash or change a confirmed command.
    const std::vector<u8> real = match.taps[1]->last_sent;
    assert(real.size() >= message_header_size);
    RngKey key = make_key(99);
    for (int i = 0; i < 2000; ++i) {
        auto [next, sub] = split(key);
        key = next;
        std::vector<u8> datagram;
        const u32 kind = rng_u32(sub) % 4;
        const u32 size = rng_u32(split(sub).first) % 40;
        if (kind == 0) {
            for (u32 b = 0; b < size; ++b) {
                datagram.push_back(static_cast<u8>(rng_u32(make_key(i * 131 + b))));
            }
        } else {
            datagram.assign(real.begin(), real.begin() + message_header_size);
            datagram[3] = static_cast<u8>(kind == 1 ? 1 : first_model_message + kind - 2);
            if (kind == 3) {
                datagram.push_back(0); // the inputs of slot 0, from peer 1: refused
            }
            for (u32 b = 0; b < size % 9; ++b) {
                datagram.push_back(static_cast<u8>(rng_u32(make_key(i * 977 + b))));
            }
        }
        match.endpoints[1]->send(LoopbackNetwork::peer(0), datagram);
        if (i % 20 == 0) {
            match.step_ms();
        }
    }
    assert(match.run_until(400, 20'000));
    match.check_against_reference(400);
    assert(match.peers[0]->session->status() == SessionStatus::running);
}

// --- the sync test ---

// Keeps a counter that save() leaves out: a rollback changes what the step does.
class Leaky {
public:
    void step(std::span<const Command> commands) {
        ++_steps_ever;
        _value += commands[0].dx + static_cast<i64>(_steps_ever % 3);
    }
    u64 state_hash() const { return static_cast<u64>(_value); }
    std::vector<u8> save() const {
        const auto* bytes = reinterpret_cast<const u8*>(&_value);
        return {bytes, bytes + sizeof(_value)};
    }
    bool restore(std::span<const u8> bytes) {
        std::memcpy(&_value, bytes.data(), sizeof(_value));
        return true;
    }

private:
    i64 _value = 0;
    u64 _steps_ever = 0;
};

void test_sync_test() {
    const auto commands = [](u32 tick, u32 player) { return script(player, tick * 16); };
    Arena arena(3);
    const SyncTestResult good = sync_test<Arena, Command>(arena, 3, 300, 7, commands);
    assert(good.ok && good.ticks == 300);
    assert(arena.resimulated > 0);

    Leaky leaky;
    const SyncTestResult bad = sync_test<Leaky, Command>(leaky, 1, 300, 4, commands);
    assert(!bad.ok);
    assert(bad.bad_tick < 10 && bad.expected != bad.actual);
    assert(bad.detail.find("save() misses") != std::string::npos);
}

// --- UDP ---

void test_udp() {
#ifndef __EMSCRIPTEN__
    std::string error;
    auto a = UdpTransport::open({}, &error);
    auto b = UdpTransport::open({}, &error);
    if (!a || !b) {
        std::printf("  udp: skipped (%s)\n", error.c_str());
        return;
    }
    assert(a->local_port() != 0 && b->local_port() != a->local_port());
    const PeerId to_b = a->add_peer("127.0.0.1", b->local_port(), &error);
    const PeerId to_a = b->add_peer("127.0.0.1", a->local_port(), &error);
    assert(to_b != no_peer && to_a != no_peer);
    assert(a->add_peer("127.0.0.1", b->local_port()) == to_b);
    assert(a->peer_address(to_b) == "127.0.0.1:" + std::to_string(b->local_port()));
    assert(a->add_peer("no such host.invalid", 1, &error) == no_peer && !error.empty());
    assert(!a->send(to_b, std::vector<u8>(1201)));

    // Two peers over real sockets, on a virtual clock.
    Arena arena_a(2), arena_b(2);
    const InputSyncConfig config{.players = 2, .input_delay = 2, .max_prediction = 8, .hash_interval = 10};
    InputSyncConfig config_a = config, config_b = config;
    config_a.local_player = 0;
    config_a.peers = {no_peer, to_b};
    config_b.local_player = 1;
    config_b.peers = {to_a, no_peer};
    InputSync<Arena, Command> session_a(arena_a, *a, config_a);
    InputSync<Arena, Command> session_b(arena_b, *b, config_b);
    u64 now_ms = 0;
    const auto done = [](const InputSyncStats& stats) { return stats.tick > 200 && stats.confirmed_tick >= 200; };
    while (now_ms < 20'000 && !(done(session_a.stats()) && done(session_b.stats()))) {
        ++now_ms;
        session_a.set_local_input(script(0, now_ms));
        session_b.set_local_input(script(1, now_ms));
        session_a.update(now_ms * 1000);
        session_b.update(now_ms * 1000);
    }
    assert(session_a.status() == SessionStatus::running && session_b.status() == SessionStatus::running);
    assert(done(session_a.stats()) && done(session_b.stats()));
    assert(session_a.stats().hashes_compared > 0 && session_b.stats().hashes_compared > 0);
    for (u32 t = 0; t < 200; ++t) {
        assert(arena_a.hash_after.at(t) == arena_b.hash_after.at(t));
    }
#endif
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_bytes();
    test_loopback();
    test_handshake();
    test_single_player();
    test_lockstep();
    test_rollback();
    test_desync();
    test_time_sync();
    test_peer_lost_and_leave();
    test_rejected_and_failed();
    test_stale_and_forged_messages();
    test_codec_command();
    test_garbage();
    test_sync_test();
    test_udp();
    std::printf("net tests passed\n");
    return 0;
}
