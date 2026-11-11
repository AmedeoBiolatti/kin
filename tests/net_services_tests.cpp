// Lobbies and messages through PlatformServices (FakeServices on a FakeNetwork), and a whole match over them:
// lobby, start_lobby_match, ServicesTransport, InputSync.

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <kin/core/hash.hpp>
#include <kin/net/input_sync.hpp>
#include <kin/net/lobby_match.hpp>
#include <kin/net/services_transport.hpp>
#include <kin/platform/services.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace kin;
using namespace kin::net;

std::vector<ServiceEvent> take(FakeServices& services) {
    services.update();
    return {services.events().begin(), services.events().end()};
}

const ServiceEvent* find_event(const std::vector<ServiceEvent>& events, ServiceEventKind kind, int line) {
    for (const ServiceEvent& event : events) {
        if (event.kind == kind) {
            return &event;
        }
    }
    std::fprintf(stderr, "line %d: no event of kind %d among %zu\n", line, static_cast<int>(kind), events.size());
    std::abort();
}
#define find(events, kind) find_event(events, kind, __LINE__)

void test_lobbies() {
    FakeNetwork network;
    FakeServices ann(network, {.id = "fake:ann", .name = "Ann"});
    FakeServices bob(network, {.id = "fake:bob", .name = "Bob"});
    FakeServices cyd(network, {.id = "fake:cyd", .name = "Cyd"});

    const u32 made = ann.create_lobby(
        {.max_members = 2, .visibility = LobbyVisibility::Public, .data = {{"mode", "coop"}, {"level", "3"}}});
    assert(made != 0);
    auto events = take(ann);
    const ServiceEvent* entered = find(events, ServiceEventKind::LobbyEntered);
    assert(entered && entered->request == made && entered->ok && entered->id == ann.lobby());
    assert(ann.lobby().starts_with("lobby:") && ann.lobby_owner() == "fake:ann");
    assert(ann.lobby_data("mode") == "coop");

    // Found by its data; a rule that does not match finds nothing.
    const u32 search = bob.find_lobbies(
        {.rules = {{.key = "mode", .value = "coop"},
                   {.key = "level", .value = "2", .compare = LobbyCompare::AtLeast, .numeric = true}}});
    events = take(bob);
    const ServiceEvent* list = find(events, ServiceEventKind::LobbyList);
    assert(list && list->request == search && list->lobbies.size() == 1 && list->lobbies[0].id == ann.lobby());
    assert(list->lobbies[0].members == 1 && list->lobbies[0].max_members == 2);
    bob.find_lobbies({.rules = {{.key = "mode", .value = "versus"}}});
    events = take(bob);
    assert(find(events, ServiceEventKind::LobbyList)->lobbies.empty());

    // Bob joins; Ann hears. Cyd finds it full.
    const u32 joined = bob.join_lobby(ann.lobby());
    events = take(bob);
    assert(find(events, ServiceEventKind::LobbyEntered)->request == joined);
    events = take(ann);
    const ServiceEvent* arrived = find(events, ServiceEventKind::LobbyMemberJoined);
    assert(arrived && arrived->id == "fake:bob" && arrived->text == "Bob");
    assert(ann.lobby_members().size() == 2 && bob.lobby_members()[0].id == "fake:ann");
    cyd.join_lobby(ann.lobby());
    events = take(cyd);
    const ServiceEvent* failed = find(events, ServiceEventKind::LobbyFailed);
    assert(failed && failed->text == "the lobby is full" && cyd.lobby().empty());
    cyd.join_lobby("lobby:999");
    assert(find(take(cyd), ServiceEventKind::LobbyFailed)->text == "no such lobby");

    // Only the owner sets the lobby's data; every member sets its own.
    assert(!bob.set_lobby_data("mode", "versus") && ann.lobby_data("mode") == "coop");
    assert(bob.set_member_data("ready", "1"));
    events = take(ann);
    const ServiceEvent* changed = find(events, ServiceEventKind::LobbyDataChanged);
    assert(changed && changed->id == "fake:bob");
    assert(ann.member_data("fake:bob", "ready") == "1" && ann.member_data("fake:ann", "ready").empty());

    // Chat reaches every member, the sender too.
    assert(bob.send_lobby_message("hello"));
    events = take(ann);
    const ServiceEvent* chat = find(events, ServiceEventKind::LobbyMessage);
    assert(chat && chat->id == "fake:bob" && chat->text == "hello");
    assert(find(take(bob), ServiceEventKind::LobbyMessage));

    // The owner leaves: Bob owns it.
    ann.leave_lobby();
    events = take(bob);
    const ServiceEvent* left = find(events, ServiceEventKind::LobbyMemberLeft);
    assert(left && left->id == "fake:ann" && bob.lobby_owner() == "fake:bob" && ann.lobby().empty());
    assert(bob.set_lobby_data("mode", "versus"));

    // A closed lobby is neither found nor joined.
    assert(bob.set_lobby_joinable(false));
    ann.find_lobbies({});
    assert(find(take(ann), ServiceEventKind::LobbyList)->lobbies.empty());
    ann.join_lobby(bob.lobby());
    assert(find(take(ann), ServiceEventKind::LobbyFailed)->text == "the lobby is closed");

    // A private lobby is joined by its id (an invitation), never found.
    cyd.create_lobby({.visibility = LobbyVisibility::Private});
    ann.find_lobbies({});
    assert(find(take(ann), ServiceEventKind::LobbyList)->lobbies.empty());
    std::printf("  lobbies ok\n");
}

void test_messages() {
    FakeNetwork network;
    FakeServices ann(network, {.id = "fake:ann", .name = "Ann"});
    FakeServices bob(network, {.id = "fake:bob", .name = "Bob"});
    FakeServices eve(network, {.id = "fake:eve", .name = "Eve"});
    const std::vector<u8> hello = {1, 2, 3};
    std::vector<PlayerMessage> got;

    // A stranger's messages are not taken; once Ann sent to Bob, Bob's are.
    assert(eve.send_message("fake:ann", hello));
    assert(bob.send_message("fake:ann", hello));
    ann.receive_messages(0, got);
    assert(got.empty());
    assert(ann.send_message("fake:bob", hello));
    bob.receive_messages(0, got);
    assert(got.size() == 1 && got[0].from == "fake:ann" && got[0].bytes == hello);
    got.clear();
    assert(bob.send_message("fake:ann", hello, MessageDelivery::Unreliable, 1));
    ann.receive_messages(0, got);
    assert(got.empty()); // another channel
    ann.receive_messages(1, got);
    assert(got.size() == 1 && got[0].from == "fake:bob");
    got.clear();

    // Lobby members take each other's messages.
    ann.create_lobby({});
    eve.join_lobby(ann.lobby());
    assert(eve.send_message("fake:ann", hello));
    ann.receive_messages(0, got);
    assert(got.size() == 1 && got[0].from == "fake:eve");

    // Too large unreliably, to oneself, to nobody, offline: not sent. Reliable ones may be large.
    assert(!ann.send_message("fake:bob", std::vector<u8>(1201)));
    assert(ann.send_message("fake:bob", std::vector<u8>(5000), MessageDelivery::Reliable));
    assert(!ann.send_message("fake:ann", hello) && !ann.send_message("fake:nobody", hello));
    ann.online_ = false;
    assert(!ann.send_message("fake:bob", hello));

    // Without a platform there is nothing to send through.
    auto offline = make_offline_services({.offline_in_memory = true});
    assert(offline->create_lobby({}) == 0 && offline->find_lobbies({}) == 0 && offline->lobby().empty());
    assert(!offline->send_message("steam:1", hello) && offline->max_message() == 0);
    ServicesTransport none(*offline);
    assert(none.max_datagram() == 0 && !none.send(none.add_peer("steam:1"), hello));
    std::printf("  messages ok\n");
}

void test_transport() {
    FakeNetwork network;
    FakeServices ann(network, {.id = "fake:ann", .name = "Ann"});
    FakeServices bob(network, {.id = "fake:bob", .name = "Bob"});
    ServicesTransport a(ann, {.channel = 3});
    ServicesTransport b(bob, {.channel = 3, .accept_unknown = false});

    assert(a.add_peer("") == no_peer && a.add_peer("fake:ann") == no_peer);
    const PeerId to_bob = a.add_peer("fake:bob");
    assert(to_bob != no_peer && a.add_peer("fake:bob") == to_bob && a.peer_player(to_bob) == "fake:bob");
    assert(a.max_datagram() == 1200 && !a.send(to_bob, std::vector<u8>(1201)) && !a.send(99, std::vector<u8>(1)));

    // Bob does not accept unknown peers: Ann's datagram is dropped until Bob names Ann.
    const std::vector<u8> bytes = {7, 7};
    assert(a.send(to_bob, bytes));
    std::vector<Datagram> in;
    b.receive(in);
    assert(in.empty());
    const PeerId to_ann = b.add_peer("fake:ann");
    assert(b.send(to_ann, bytes));
    a.send(to_bob, bytes);
    b.receive(in);
    assert(in.size() == 1 && in[0].from == to_ann && in[0].bytes == bytes);
    in.clear();
    a.receive(in);
    assert(in.size() == 1 && in[0].from == to_bob);

    // A player gone for good frees their place, and their peer id is never handed out again.
    FakeServices cyd(network, {.id = "fake:cyd", .name = "Cyd"});
    ServicesTransport small(cyd, {.max_peers = 2});
    const PeerId p_ann = small.add_peer("fake:ann");
    const PeerId p_bob = small.add_peer("fake:bob");
    assert(small.add_peer("fake:dan") == no_peer && small.peers() == 2); // full
    assert(small.remove_peer("fake:ann") && !small.remove_peer("fake:ann") && small.peers() == 1);
    assert(!small.send(p_ann, bytes) && small.peer_player(p_ann).empty());
    const PeerId p_dan = small.add_peer("fake:dan");
    assert(p_dan != no_peer && p_dan != p_ann && p_dan != p_bob);
    assert(small.add_peer("fake:ann") == no_peer); // full again: no old place to return to
    assert(small.remove_peer("fake:bob") && small.add_peer("fake:ann") > p_dan);
    std::printf("  transport ok\n");
}

// --- a whole match: lobby, slots, InputSync over the platform's messages ---

struct Command {
    i8 dx = 0;
    u8 buttons = 0;
    friend bool operator==(const Command&, const Command&) = default;
};

struct WalkState {
    std::array<i32, max_players> x{}, score{};
    u32 tick = 0, players = 0;
    u64 seed = 0;
};
static_assert(std::has_unique_object_representations_v<WalkState>);

class Walk {
public:
    Walk(u32 players, u64 seed) {
        _state.players = players;
        _state.seed = seed;
    }
    void step(std::span<const Command> commands, const StepInfo& info) {
        for (u32 p = 0; p < _state.players; ++p) {
            if ((info.joined >> p) & 1u) {
                _state.score[p] += 100; // arrives
            }
            _state.x[p] += commands[p].dx;
            if (commands[p].buttons) {
                _state.score[p] += static_cast<i32>((_state.seed >> (p * 3)) & 7) + 1;
            }
        }
        _state.seed = _state.seed * 6364136223846793005ull + 1442695040888963407ull;
        hash_after[_state.tick] = state_hash();
        ++_state.tick;
    }
    u64 state_hash() const { return hash_bytes(&_state, sizeof _state); }
    std::vector<u8> save() const {
        const auto* p = reinterpret_cast<const u8*>(&_state);
        return {p, p + sizeof _state};
    }
    bool restore(std::span<const u8> bytes) {
        if (bytes.size() != sizeof _state) {
            return false;
        }
        std::memcpy(&_state, bytes.data(), sizeof _state);
        return true;
    }
    std::map<u32, u64> hash_after;

private:
    WalkState _state;
};
static_assert(RollbackSim<Walk, Command>);

struct Player {
    std::unique_ptr<FakeServices> services;
    std::unique_ptr<ServicesTransport> transport;
    std::unique_ptr<Walk> walk;
    std::unique_ptr<InputSync<Walk, Command>> session;
};

void test_lobby_match() {
    FakeNetwork network;
    network.drop_every = 7; // lossy: input sync repeats what is not acknowledged
    std::vector<Player> players(3);
    const char* names[] = {"Ann", "Bob", "Cyd"};
    for (u32 i = 0; i < players.size(); ++i) {
        players[i].services = std::make_unique<FakeServices>(
            network, ServicePlayer{.id = "fake:" + std::string{names[i]}, .name = names[i]});
        players[i].transport = std::make_unique<ServicesTransport>(*players[i].services);
    }
    FakeServices& host = *players[0].services;
    host.create_lobby({.max_members = 4, .visibility = LobbyVisibility::Public, .data = {{"game", "walk"}}});
    take(host);
    // The others find it and join.
    for (u32 i = 1; i < players.size(); ++i) {
        FakeServices& guest = *players[i].services;
        guest.find_lobbies({.rules = {{.key = "game", .value = "walk"}}});
        const auto events = take(guest);
        const ServiceEvent* list = find(events, ServiceEventKind::LobbyList);
        assert(list && list->lobbies.size() == 1);
        guest.join_lobby(list->lobbies[0].id);
        assert(find(take(guest), ServiceEventKind::LobbyEntered));
    }
    // Nobody but the owner starts; no match before it does.
    assert(!start_lobby_match(*players[1].services));
    assert(!read_lobby_match(*players[1].services));
    // Slots in an order the owner picks; a stranger cannot be given one.
    const std::vector<std::string> stranger = {"fake:Ann", "fake:Zed"};
    assert(!start_lobby_match(host, stranger));
    const std::vector<std::string> order = {"fake:Cyd", "fake:Ann", "fake:Bob"};
    assert(start_lobby_match(host, order, 0x5eed));
    FakeServices late(network, {.id = "fake:Dan", .name = "Dan"});
    late.join_lobby(host.lobby());
    assert(find(take(late), ServiceEventKind::LobbyFailed)); // the lobby closed when the match started

    for (u32 i = 0; i < players.size(); ++i) {
        Player& p = players[i];
        const auto events = take(*p.services);
        assert(find(events, ServiceEventKind::LobbyDataChanged));
        const std::optional<LobbyMatch> match = read_lobby_match(*p.services);
        assert(match && match->session == 0x5eed && match->players == order);
        assert(match->players[match->local_player] == p.services->player().id);
        InputSyncConfig config{
            .input_delay = 2, .max_prediction = 8, .hash_interval = 10, .game = compatibility_hash("walk", "1")};
        configure_input_sync(*match, *p.transport, config);
        assert(config.players == 3 && config.session == 0x5eed && config.peers.size() == 3);
        assert(config.peers[match->local_player] == no_peer);
        p.walk = std::make_unique<Walk>(config.players, match->session);
        p.session = std::make_unique<InputSync<Walk, Command>>(*p.walk, *p.transport, config);
    }

    u64 now_ms = 0;
    const auto done = [&] {
        return std::ranges::all_of(players, [](const Player& p) {
            return p.session->stats().tick > 300 && p.session->stats().confirmed_tick >= 300;
        });
    };
    while (!done() && now_ms < 30'000) {
        now_ms += 4;
        for (u32 i = 0; i < players.size(); ++i) {
            Player& p = players[i];
            p.services->update();
            const u64 h = (now_ms / 50) * 31 + i * 17;
            p.session->set_local_input(
                {.dx = static_cast<i8>(static_cast<i32>(h % 3) - 1), .buttons = static_cast<u8>((h >> 3) & 1)});
            p.session->update(now_ms * 1000);
            assert(p.session->status() == SessionStatus::connecting || p.session->status() == SessionStatus::running);
        }
    }
    assert(done());
    assert(network.dropped > 0);
    for (u32 t = 0; t < 300; ++t) {
        for (const Player& p : players) {
            assert(p.walk->hash_after.at(t) == players[0].walk->hash_after.at(t));
        }
    }
    for (const Player& p : players) {
        assert(p.session->stats().hashes_compared > 0);
    }
    std::printf("  lobby match ok: %llu messages, %llu dropped, %llu ms\n",
                static_cast<unsigned long long>(network.sent), static_cast<unsigned long long>(network.dropped),
                static_cast<unsigned long long>(now_ms));

    // The owner ends it: the lobby opens again and holds no match.
    assert(end_lobby_match(host) && !read_lobby_match(host));
    late.join_lobby(host.lobby());
    assert(find(take(late), ServiceEventKind::LobbyEntered));

    // A kin.match written some other way is read only if it is one: a player listed twice would get two slots.
    const auto reads = [&](const char* text) {
        assert(host.set_lobby_data(lobby_match_key, text));
        return read_lobby_match(host).has_value();
    };
    assert(reads("1\n5eed\nfake:Ann\nfake:Bob"));
    assert(reads("2\n5eed\nfake:Ann\n-\n+fake:Bob"));
    assert(!reads("1\n5eed\nfake:Ann\nfake:Ann"));
    assert(!reads("2\n5eed\nfake:Ann\n+fake:Ann"));
    assert(!reads("1\n5eed\nfake:Ann\r\nfake:Bob"));
    assert(!reads("1\n5eed\nfake:Ann\n++fake:Bob"));
    assert(!reads("1\n5eed\nfake:Ann\n"));
    assert(!reads("1\n0\nfake:Ann"));
    assert(!reads("3\n5eed\nfake:Ann"));
    assert(!reads("2\n5eed\n-\n-")); // nobody
    assert(end_lobby_match(host));
}

// allow_lobby_joiners lets latecomers into the session the match is for, not into another one.
void test_lobby_joiners_session() {
    struct Session {
        InputSyncConfig config_{.players = 3, .session = 0x5eed};
        std::vector<u32> allowed;
        const InputSyncConfig& config() const { return config_; }
        bool allow_join(u32 slot, PeerId) {
            allowed.push_back(slot);
            return true;
        }
    };
    FakeNetwork network;
    FakeServices ann(network, {.id = "fake:Ann", .name = "Ann"});
    ServicesTransport transport(ann);
    LobbyMatch match{.session = 0x5eed,
                     .players = {"fake:Ann", "fake:Bob", "fake:Cyd"},
                     .late = {false, false, true},
                     .joinable = true};
    Session session;
    assert(allow_lobby_joiners(match, transport, session) == 1 && session.allowed == std::vector<u32>{2});
    match.session = 0xbeef; // the owner ended that match and started the next
    assert(allow_lobby_joiners(match, transport, session) == 0 && session.allowed.size() == 1);
    match.session = 0x5eed;
    match.players.push_back("fake:Dan");
    match.late.push_back(true);
    assert(allow_lobby_joiners(match, transport, session) == 0 && session.allowed.size() == 1);
}

// A match with free slots: two start, two more join through the lobby while it runs, and one who was seated but left
// before playing gives the slot to the next.
void test_lobby_late_join() {
    FakeNetwork network;
    network.drop_every = 9;
    std::vector<Player> players;
    players.reserve(8); // add_player's references stay good
    const auto add_player = [&](const char* name) -> Player& {
        Player& p = players.emplace_back();
        p.services =
            std::make_unique<FakeServices>(network, ServicePlayer{.id = std::string{"fake:"} + name, .name = name});
        p.transport = std::make_unique<ServicesTransport>(*p.services);
        return p;
    };
    add_player("Ann");
    add_player("Bob");
    FakeServices& host = *players[0].services;
    host.create_lobby({.max_members = 8, .visibility = LobbyVisibility::Public, .data = {{"game", "walk"}}});
    players[1].services->join_lobby(host.lobby());
    assert(start_lobby_match(host, {}, 0x1a7e, 2));
    u64 now_ms = 0;
    const InputSyncConfig base{
        .input_delay = 2, .max_prediction = 8, .hash_interval = 5, .game = compatibility_hash("walk", "1")};
    // What every member does each frame: the owner seats newcomers; everyone follows the match.
    const auto frame = [&] {
        now_ms += 4;
        for (u32 i = 0; i < players.size(); ++i) {
            Player& p = players[i];
            p.services->update();
            for (const ServiceEvent& event : p.services->events()) {
                if (event.kind == ServiceEventKind::LobbyMemberJoined) {
                    seat_lobby_joiners(*p.services);
                }
                if (event.kind == ServiceEventKind::LobbyMemberLeft) {
                    free_lobby_slot(*p.services, event.id);
                }
            }
            if (const std::optional<LobbyMatch> match = read_lobby_match(*p.services)) {
                if (!p.session) {
                    InputSyncConfig config = base;
                    configure_input_sync(*match, *p.transport, config);
                    p.walk = std::make_unique<Walk>(config.players, match->session);
                    p.session = std::make_unique<InputSync<Walk, Command>>(*p.walk, *p.transport, config);
                }
                allow_lobby_joiners(*match, *p.transport, *p.session);
            }
            if (p.session) {
                const u64 h = (now_ms / 50) * 31 + i * 17;
                p.session->set_local_input(
                    {.dx = static_cast<i8>(static_cast<i32>(h % 3) - 1), .buttons = static_cast<u8>((h >> 3) & 1)});
                p.session->update(now_ms * 1000);
                assert(!session_over(p.session->status()));
            }
        }
    };
    const auto run_until = [&](auto&& done) {
        const u64 limit = now_ms + 30'000;
        while (!done()) {
            frame();
            assert(now_ms < limit);
        }
    };
    run_until([&] { return players[0].session && players[0].session->stats().confirmed_tick >= 150; });
    assert(players[0].session->config().players == 4 && players[0].session->stats().present == 0b0011);

    // Cyd finds the lobby open, joins it, is seated, and joins the game.
    Player& cyd = add_player("Cyd");
    cyd.services->find_lobbies({.rules = {{.key = "game", .value = "walk"}}});
    cyd.services->update();
    assert(cyd.services->events().size() == 1 && cyd.services->events()[0].lobbies.size() == 1);
    cyd.services->join_lobby(cyd.services->events()[0].lobbies[0].id);
    run_until([&] {
        return players[0].session->stats().present == 0b0111 && players[2].session &&
               !players[2].session->stats().spectating && players[2].session->stats().joins == 1;
    });
    assert(players[2].session->config().joining && players[2].session->stats().start_tick >= 150);

    // Dan is seated in the last slot and leaves before playing: the slot is free again, and Eve takes it.
    Player& dan = add_player("Dan");
    dan.services->join_lobby(host.lobby());
    for (int i = 0; i < 3; ++i) {
        frame();
    }
    const std::optional<LobbyMatch> seated = read_lobby_match(host);
    assert(seated && seated->players[3] == "fake:Dan" && seated->late[3]);
    assert(players[3].session && players[3].session->config().joining);
    players[3].session.reset(); // Dan's game closes
    players[3].services->leave_lobby();
    frame();
    const std::optional<LobbyMatch> freed = read_lobby_match(host);
    assert(freed && freed->players[3].empty());
    Player& eve = add_player("Eve");
    eve.services->join_lobby(host.lobby());
    run_until([&] {
        return players[0].session->stats().present == 0b1111 && players[4].session &&
               players[4].session->stats().joins >= 1 && !players[4].session->stats().spectating;
    });
    // Full: the lobby closed to newcomers.
    FakeServices fay(network, {.id = "fake:Fay", .name = "Fay"});
    fay.join_lobby(host.lobby());
    fay.update();
    assert(fay.events().size() == 1 && fay.events()[0].kind == ServiceEventKind::LobbyFailed);

    const std::vector<u32> playing = {0, 1, 2, 4};
    run_until([&] {
        return std::ranges::all_of(playing, [&](u32 i) {
            return players[i].session->stats().confirmed_tick >= 600 && players[i].session->stats().tick > 600;
        });
    });
    u32 compared = 0;
    for (u32 t = players[4].session->stats().start_tick; t < 600; ++t) {
        for (u32 i : playing) {
            assert(players[i].walk->hash_after.at(t) == players[0].walk->hash_after.at(t));
            ++compared;
        }
    }
    for (u32 i : playing) {
        assert(players[i].session->stats().present == 0b1111 && players[i].session->stats().hashes_compared > 0);
    }
    std::printf("  lobby late join ok: Cyd from tick %u, Eve from tick %u, %u states compared\n",
                players[2].session->stats().start_tick, players[4].session->stats().start_tick, compared);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_lobbies();
    test_messages();
    test_transport();
    test_lobby_match();
    test_lobby_joiners_session();
    test_lobby_late_join();
    std::printf("net services tests passed\n");
    return 0;
}
