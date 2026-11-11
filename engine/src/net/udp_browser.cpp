// UdpTransport in a browser (Emscripten): a page cannot open a UDP socket, so open() fails and says so, as it does
// where a port cannot be bound. A game links the same code everywhere and offers what needs UDP only where it opened.

#include <kin/net/udp.hpp>

namespace kin::net {

struct UdpTransport::Impl {};

UdpTransport::UdpTransport(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

UdpTransport::~UdpTransport() = default;

std::unique_ptr<UdpTransport> UdpTransport::open(const UdpOptions&, std::string* error) {
    if (error) {
        *error = "a browser cannot open a UDP socket";
    }
    return nullptr;
}

bool UdpTransport::send(PeerId, std::span<const u8>) {
    return false;
}

void UdpTransport::receive(std::vector<Datagram>&) {}

u16 UdpTransport::local_port() const {
    return 0;
}

PeerId UdpTransport::add_peer(std::string_view, u16, std::string* error) {
    if (error) {
        *error = "a browser cannot open a UDP socket";
    }
    return no_peer;
}

std::string UdpTransport::peer_address(PeerId) const {
    return {};
}

} // namespace kin::net
