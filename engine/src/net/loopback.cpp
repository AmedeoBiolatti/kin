#include <kin/net/loopback.hpp>

#include <algorithm>

namespace kin::net {
namespace {

bool later(const auto& a, const auto& b) {
    return a.arrival_us != b.arrival_us ? a.arrival_us > b.arrival_us : a.sequence > b.sequence;
}

} // namespace

bool LoopbackEndpoint::send(PeerId to, std::span<const u8> bytes) {
    return _network.send(_index, to, bytes);
}

void LoopbackEndpoint::receive(std::vector<Datagram>& out) {
    _network.receive(_index, out);
}

std::size_t LoopbackEndpoint::max_datagram() const {
    return _network.max_datagram();
}

LoopbackNetwork::LoopbackNetwork(u64 seed) : _rng(make_key(seed)) {}

LoopbackNetwork::~LoopbackNetwork() = default;

LoopbackEndpoint& LoopbackNetwork::add_endpoint() {
    const std::size_t old_count = _endpoints.size();
    const std::size_t count = old_count + 1;
    std::vector<Link> links(count * count, Link{.conditions = _default});
    for (std::size_t from = 0; from < old_count; ++from) {
        for (std::size_t to = 0; to < old_count; ++to) {
            links[from * count + to] = _links[from * old_count + to];
        }
    }
    _links = std::move(links);
    _endpoints.push_back(std::unique_ptr<LoopbackEndpoint>(new LoopbackEndpoint(*this, old_count)));
    return *_endpoints.back();
}

void LoopbackNetwork::set_conditions(const LinkConditions& conditions) {
    _default = conditions;
    for (Link& link : _links) {
        link.conditions = conditions;
    }
}

void LoopbackNetwork::set_conditions(std::size_t from, std::size_t to, const LinkConditions& conditions) {
    link(from, to).conditions = conditions;
}

void LoopbackNetwork::set_connected(std::size_t from, std::size_t to, bool connected) {
    link(from, to).connected = connected;
}

void LoopbackNetwork::set_time(u64 now_us) {
    _now_us = std::max(_now_us, now_us);
}

LoopbackNetwork::Link& LoopbackNetwork::link(std::size_t from, std::size_t to) {
    const std::size_t count = _endpoints.size();
    return _links[std::min(from, count - 1) * count + std::min(to, count - 1)];
}

f32 LoopbackNetwork::next_unit() {
    auto [next, sub] = split(_rng);
    _rng = next;
    return rng_f32(sub, 0.0f, 1.0f);
}

bool LoopbackNetwork::send(std::size_t from, PeerId to, std::span<const u8> bytes) {
    if (to == no_peer || to > _endpoints.size() || to - 1 == from || bytes.size() > _max_datagram) {
        return false;
    }
    const std::size_t target = to - 1;
    ++_stats.sent;
    const Link& route = link(from, target);
    // Always draw the same numbers per datagram, so conditions change what happens to a datagram, not which random
    // numbers later datagrams get.
    const f32 loss_draw = next_unit();
    const f32 duplicate_draw = next_unit();
    const f32 jitter_draws[2] = {next_unit(), next_unit()};
    if (!route.connected || loss_draw < route.conditions.loss) {
        ++_stats.dropped;
        return true;
    }
    const int copies = duplicate_draw < route.conditions.duplicate ? 2 : 1;
    if (copies == 2) {
        ++_stats.duplicated;
    }
    for (int copy = 0; copy < copies; ++copy) {
        const u64 jitter = static_cast<u64>(jitter_draws[copy] * static_cast<f32>(route.conditions.jitter_us));
        InFlight flight{
            .arrival_us = _now_us + route.conditions.latency_us + jitter,
            .sequence = _sequence++,
            .to = target,
            .datagram = {.from = peer(from), .bytes = {bytes.begin(), bytes.end()}},
        };
        _in_flight.push_back(std::move(flight));
        std::push_heap(_in_flight.begin(), _in_flight.end(),
                       [](const InFlight& a, const InFlight& b) { return later(a, b); });
    }
    return true;
}

void LoopbackNetwork::receive(std::size_t to, std::vector<Datagram>& out) {
    // Datagrams for other endpoints stay in flight; the heap is small (a few hundred at most in a test).
    std::vector<InFlight> keep;
    const auto by_arrival = [](const InFlight& a, const InFlight& b) { return later(a, b); };
    while (!_in_flight.empty() && _in_flight.front().arrival_us <= _now_us) {
        std::pop_heap(_in_flight.begin(), _in_flight.end(), by_arrival);
        InFlight flight = std::move(_in_flight.back());
        _in_flight.pop_back();
        if (flight.to == to) {
            ++_stats.delivered;
            out.push_back(std::move(flight.datagram));
        } else {
            keep.push_back(std::move(flight));
        }
    }
    for (InFlight& flight : keep) {
        _in_flight.push_back(std::move(flight));
        std::push_heap(_in_flight.begin(), _in_flight.end(), by_arrival);
    }
}

} // namespace kin::net
