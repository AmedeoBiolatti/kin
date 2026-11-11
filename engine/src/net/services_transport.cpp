#include <algorithm>
#include <kin/net/services_transport.hpp>
#include <kin/platform/services.hpp>

namespace kin::net {

ServicesTransport::ServicesTransport(PlatformServices& services, const ServicesTransportOptions& options)
    : _services(services), _options(options), _self(services.player().id) {}

ServicesTransport::~ServicesTransport() = default;

PeerId ServicesTransport::find(std::string_view player) const {
    for (std::size_t i = 0; i < _peers.size(); ++i) {
        if (_peers[i] == player) {
            return static_cast<PeerId>(i + 1);
        }
    }
    return no_peer;
}

PeerId ServicesTransport::add_peer(std::string_view player) {
    if (player.empty() || player == _self) {
        return no_peer;
    }
    if (const PeerId known = find(player); known != no_peer) {
        return known;
    }
    if (_peers.size() >= _options.max_peers) {
        return no_peer;
    }
    _peers.emplace_back(player);
    return static_cast<PeerId>(_peers.size());
}

std::string ServicesTransport::peer_player(PeerId peer) const {
    return peer != no_peer && peer <= _peers.size() ? _peers[peer - 1] : std::string{};
}

bool ServicesTransport::send(PeerId to, std::span<const u8> bytes) {
    if (to == no_peer || to > _peers.size() || bytes.size() > max_datagram()) {
        return false;
    }
    return _services.send_message(_peers[to - 1], bytes, MessageDelivery::Unreliable, _options.channel);
}

void ServicesTransport::receive(std::vector<Datagram>& out) {
    std::vector<PlayerMessage> messages;
    _services.receive_messages(_options.channel, messages);
    for (PlayerMessage& message : messages) {
        PeerId from = find(message.from);
        if (from == no_peer && _options.accept_unknown) {
            from = add_peer(message.from);
        }
        if (from != no_peer) {
            out.push_back({.from = from, .bytes = std::move(message.bytes)});
        }
    }
}

std::size_t ServicesTransport::max_datagram() const {
    return std::min<std::size_t>(Transport::max_datagram(), _services.max_message());
}

void ServicesTransport::close() {
    for (const std::string& player : _peers) {
        _services.close_messages(player);
    }
}

} // namespace kin::net
