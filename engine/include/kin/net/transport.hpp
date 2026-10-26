#pragma once

// A transport moves datagrams between this process and its peers (docs/networking.md): unreliable, unordered,
// possibly duplicated, each at most max_datagram() bytes. Everything else (handshakes, acks, redundancy, clocks) is
// built once on top by the session layer, so a transport (loopback, UDP; later Steam or WebRTC) only does this.
//
// Transports are not thread-safe: one thread sends and receives.

#include <kin/core/types.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace kin::net {

// A remote endpoint, as one transport numbers them. 0 is never a peer.
using PeerId = u32;
inline constexpr PeerId no_peer = 0;

struct Datagram {
    PeerId from = no_peer;
    std::vector<u8> bytes;
};

class Transport {
public:
    virtual ~Transport() = default;

    // Queues one datagram for `to`. False, and nothing sent, if `to` is unknown or the datagram is larger than
    // max_datagram(). True does not mean it will arrive.
    virtual bool send(PeerId to, std::span<const u8> bytes) = 0;
    // Appends every datagram that arrived since the last call to `out`.
    virtual void receive(std::vector<Datagram>& out) = 0;
    // The largest datagram send() accepts. 1200 bytes stays under the path MTU of practically every route, so a
    // datagram is never fragmented (one lost fragment would lose all of it).
    virtual std::size_t max_datagram() const { return 1200; }
};

} // namespace kin::net
