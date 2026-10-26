#pragma once

// UDP datagrams over IPv4, or IPv6 with IPv4 peers mapped in (docs/networking.md). Not in web builds: a browser has
// no UDP (a WebRTC transport is planned). The socket is non-blocking; receive() takes what has arrived.
//
//     std::string error;
//     auto udp = kin::net::UdpTransport::open({.port = 7777}, &error);
//     const kin::net::PeerId host = udp->add_peer("192.168.1.20", 7777, &error);
//     udp->send(host, bytes);

#include <kin/net/transport.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace kin::net {

struct UdpOptions {
    u16 port = 0;      // 0 lets the system pick one (local_port() says which)
    bool ipv6 = false; // dual-stack: IPv6 peers and IPv4 ones
    // Datagrams from an address that add_peer() did not name get a new id (a host learns its players this way), up
    // to this many peers in all; later strangers are ignored.
    u32 max_peers = 64;
    bool accept_unknown = true;
};

class UdpTransport final : public Transport {
public:
    // Opens and binds a socket; nullptr (and `error` says why) on failure.
    static std::unique_ptr<UdpTransport> open(const UdpOptions& options = {}, std::string* error = nullptr);
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    bool send(PeerId to, std::span<const u8> bytes) override;
    void receive(std::vector<Datagram>& out) override;

    u16 local_port() const;
    // Resolves `host` (a name or an address) and returns the peer that sends there: the same id for the same address.
    // no_peer, and `error` says why, if it does not resolve or the peer table is full.
    PeerId add_peer(std::string_view host, u16 port, std::string* error = nullptr);
    // "address:port" of a peer, or "" for an unknown one.
    std::string peer_address(PeerId peer) const;

private:
    struct Impl;
    explicit UdpTransport(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};

} // namespace kin::net
