#pragma once

// From a lobby to a match (docs/networking.md): the lobby's owner says who plays which slot, and every member reads
// it from the lobby's data and starts the same session. A match may keep slots free for players who join while it
// runs (docs/networking.md#joining-a-game-in-progress).
//
//     // the owner, when everyone is ready (two free slots for latecomers):
//     kin::net::start_lobby_match(*services, {}, 0, 2);
//     // every member (the owner too), on LobbyDataChanged:
//     if (auto match = kin::net::read_lobby_match(*services)) {
//         if (!session) {
//             kin::net::InputSyncConfig config{.game = kin::net::compatibility_hash("my_game", "1.2")};
//             kin::net::configure_input_sync(*match, transport, config); // joins if this player came late
//             session = std::make_unique<kin::net::InputSync<Game, Command>>(game, transport, config);
//         }
//         kin::net::allow_lobby_joiners(*match, transport, *session);
//     }
//     // the owner, on LobbyMemberJoined:
//     kin::net::seat_lobby_joiners(*services);

#include <kin/net/input_sync.hpp>
#include <kin/net/services_transport.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {
class PlatformServices;
}

namespace kin::net {

// The lobby data key that holds the match.
inline constexpr std::string_view lobby_match_key = "kin.match";

struct LobbyMatch {
    u64 session = 0;                  // SessionIdentity::session: this match, apart from the lobby's earlier ones
    std::vector<std::string> players; // the players' ids, in slot order; "" for a free slot
    std::vector<bool> late;           // late[slot]: the player joined while the match ran
    bool joinable = false;            // started with free slots: players may join while it runs
    u32 local_player = 0;             // this player's slot
};

// The lobby's owner starts a match: `players` (the members in the order they joined, when empty) take slots in that
// order, and `free_slots` more stay open for players who join later. The lobby stays open while a slot is free.
// `session` 0 picks a random one. False when this player does not own a lobby, a player is not a member, or there
// are more than max_players slots.
bool start_lobby_match(PlatformServices& services, std::span<const std::string> players = {}, u64 session = 0,
                       u32 free_slots = 0);
// The owner ends it: the lobby opens again.
bool end_lobby_match(PlatformServices& services);
// The match the lobby's owner started, if this player has a slot in it.
std::optional<LobbyMatch> read_lobby_match(const PlatformServices& services);

// The owner gives lobby members who have no slot the free ones (in the order they joined), and closes the lobby
// when none is left. Returns how many it seated. Call it on LobbyMemberJoined.
u32 seat_lobby_joiners(PlatformServices& services);
// The owner frees a latecomer's slot when they leave the lobby (on LobbyMemberLeft), for someone else to take. One
// who had started playing ended the session by going (a lost player ends it); one who had not yet left no trace.
bool free_lobby_slot(PlatformServices& services, std::string_view player);

// Sets an InputSyncConfig's players, local_player, peers (added to `transport`), session, joinable and joining from
// a match.
void configure_input_sync(const LobbyMatch& match, ServicesTransport& transport, InputSyncConfig& config);

// Lets every player the match seated late join `session` (InputSync::allow_join). Every player in the game calls it
// when the match changes; seats already allowed are left alone.
template <class SessionType>
u32 allow_lobby_joiners(const LobbyMatch& match, ServicesTransport& transport, SessionType& session) {
    u32 allowed = 0;
    for (u32 slot = 0; slot < match.players.size(); ++slot) {
        if (slot != match.local_player && match.late[slot] && !match.players[slot].empty() &&
            session.allow_join(slot, transport.add_peer(match.players[slot]))) {
            ++allowed;
        }
    }
    return allowed;
}

} // namespace kin::net
