#pragma once

// An in-process network with a link simulator (docs/networking.md): endpoints exchange datagrams through it, each
// delayed, dropped or duplicated as the link's conditions say. Its clock is whatever time the owner sets, so a test
// plays minutes of a laggy match in milliseconds and the same seed always loses the same datagrams.
//
//     kin::net::LoopbackNetwork network{seed};
//     auto& a = network.add_endpoint();               // endpoint 0: peer 1 for everyone
//     auto& b = network.add_endpoint();               // endpoint 1: peer 2
//     network.set_conditions({.latency_us = 40'000, .jitter_us = 10'000, .loss = 0.05f});
//     a.send(kin::net::LoopbackNetwork::peer(1), bytes);
//     network.set_time(now_us);                        // delivers what has arrived by then
//     b.receive(datagrams);

#include <kin/core/rng.hpp>
#include <kin/net/transport.hpp>

#include <memory>
#include <vector>

namespace kin::net {

struct LinkConditions {
    u32 latency_us = 0;   // one way
    u32 jitter_us = 0;    // each datagram adds a uniform [0, jitter_us]; datagrams can overtake each other
    f32 loss = 0.0f;      // probability that a datagram is dropped
    f32 duplicate = 0.0f; // probability that it also arrives a second time (with its own delay)
    friend bool operator==(const LinkConditions&, const LinkConditions&) = default;
};

struct LoopbackStats {
    u64 sent = 0;
    u64 delivered = 0;
    u64 dropped = 0;
    u64 duplicated = 0;
};

class LoopbackNetwork;

class LoopbackEndpoint final : public Transport {
public:
    bool send(PeerId to, std::span<const u8> bytes) override;
    void receive(std::vector<Datagram>& out) override;
    std::size_t max_datagram() const override;

    std::size_t index() const { return _index; }

private:
    friend class LoopbackNetwork;
    LoopbackEndpoint(LoopbackNetwork& network, std::size_t index) : _network(network), _index(index) {}

    LoopbackNetwork& _network;
    std::size_t _index;
};

class LoopbackNetwork {
public:
    explicit LoopbackNetwork(u64 seed = 1);
    ~LoopbackNetwork();
    LoopbackNetwork(const LoopbackNetwork&) = delete;
    LoopbackNetwork& operator=(const LoopbackNetwork&) = delete;

    // A new endpoint; the reference lives as long as the network.
    LoopbackEndpoint& add_endpoint();
    std::size_t endpoint_count() const { return _endpoints.size(); }
    // The id every endpoint uses for endpoint `index`.
    static PeerId peer(std::size_t index) { return static_cast<PeerId>(index + 1); }

    // Conditions of every link, then of one direction of one link (`from` sending to `to`).
    void set_conditions(const LinkConditions& conditions);
    void set_conditions(std::size_t from, std::size_t to, const LinkConditions& conditions);
    // Cuts one direction of a link, or restores it with the conditions it had.
    void set_connected(std::size_t from, std::size_t to, bool connected);

    // The network's clock, in microseconds; it never goes back. A datagram is received once the clock reaches its
    // arrival time.
    void set_time(u64 now_us);
    u64 time() const { return _now_us; }

    void set_max_datagram(std::size_t bytes) { _max_datagram = bytes; }
    std::size_t max_datagram() const { return _max_datagram; }
    const LoopbackStats& stats() const { return _stats; }

private:
    friend class LoopbackEndpoint;
    struct InFlight {
        u64 arrival_us = 0;
        u64 sequence = 0; // ties keep send order
        std::size_t to = 0;
        Datagram datagram;
    };
    struct Link {
        LinkConditions conditions;
        bool connected = true;
    };

    bool send(std::size_t from, PeerId to, std::span<const u8> bytes);
    void receive(std::size_t to, std::vector<Datagram>& out);
    Link& link(std::size_t from, std::size_t to);
    f32 next_unit();

    std::vector<std::unique_ptr<LoopbackEndpoint>> _endpoints;
    std::vector<Link> _links; // from * endpoint_count + to, grown with the endpoints
    LinkConditions _default;
    std::vector<InFlight> _in_flight; // a heap ordered by (arrival, sequence)
    RngKey _rng;
    u64 _now_us = 0;
    u64 _sequence = 0;
    std::size_t _max_datagram = 1200;
    LoopbackStats _stats;
};

} // namespace kin::net
