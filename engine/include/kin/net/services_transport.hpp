#pragma once

// A transport over a platform's messages between players (kin/platform/services.hpp): Steam's networking finds
// the way between peers behind NATs, relays what cannot go directly, and hides the players' addresses. Peers are
// players, by their ServicePlayer::id. Every backend's messages work (FakeServices on a FakeNetwork in tests);
// where the platform has none (offline), send() returns false and nothing arrives.
//
//     kin::net::ServicesTransport transport(*services);
//     const kin::net::PeerId host = transport.add_peer(services->lobby_owner());
//
// The services' update() must still run each frame (run_scene_app does it): the platform's own work happens there.

#include <kin/net/transport.hpp>
#include <string>
#include <string_view>

namespace kin {
class PlatformServices;
}

namespace kin::net {

struct ServicesTransportOptions {
    // The platform's message channel the transport uses. It is not send_message()'s default (0), where a game's own
    // messages go, so the transport never takes them.
    u32 channel = 1;
    // Messages from a player add_peer() did not name get a new peer id (a host learns its players this way), up to
    // this many peers at once (remove_peer() makes room); later strangers are ignored.
    bool accept_unknown = true;
    u32 max_peers = 64;
};

class ServicesTransport final : public Transport {
public:
    explicit ServicesTransport(PlatformServices& services, const ServicesTransportOptions& options = {});
    ~ServicesTransport() override;
    ServicesTransport(const ServicesTransport&) = delete;
    ServicesTransport& operator=(const ServicesTransport&) = delete;

    bool send(PeerId to, std::span<const u8> bytes) override;
    void receive(std::vector<Datagram>& out) override;
    // The smaller of the session layer's 1200 bytes and what the platform sends unreliably.
    std::size_t max_datagram() const override;

    // The peer that sends to `player` (a ServicePlayer::id): the same id for the same player. no_peer for an empty
    // id, the local player, or a full peer table.
    PeerId add_peer(std::string_view player);
    // Forgets a player who is gone for good (on LobbyMemberLeft, say): closes their messages and frees their place.
    // Their peer id is never given again, so nothing meant for them reaches whoever comes next; if they come back,
    // add_peer() gives them a new one. False if the player is not a peer.
    bool remove_peer(std::string_view player);
    // How many peers there are now.
    u32 peers() const { return _live; }
    // The player a peer sends to, or "".
    std::string peer_player(PeerId peer) const;
    // Stops talking to every peer (the platform closes its sessions).
    void close();

private:
    PeerId find(std::string_view player) const;

    PlatformServices& _services;
    ServicesTransportOptions _options;
    std::string _self;
    std::vector<std::string> _peers; // _peers[id - 1]; "" once removed
    u32 _live = 0;
};

} // namespace kin::net
