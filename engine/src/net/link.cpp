#include <kin/net/link.hpp>

#include <kin/core/hash.hpp>

#include <algorithm>
#include <limits>

namespace kin::net {
namespace {

constexpr u8 magic_0 = 'k';
constexpr u8 magic_1 = 'n';

enum class LinkMessage : u8 {
    hello = 1,  // identity, sender's slot, whether the sender has heard the receiver
    reject = 2, // reason; its layout never changes, so peers of other protocol versions can still read it
    ping = 3,   // sender's clock
    pong = 4,   // the ping's clock, echoed
    bye = 5,
};

constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

} // namespace

u64 compatibility_hash(std::string_view game_id, std::string_view version, std::string_view build, u64 content_hash) {
    // Lengths first, so ("ab", "c") and ("a", "bc") differ.
    u64 hash = 0xcbf29ce484222325ull;
    for (const std::string_view part : {game_id, version, build}) {
        const u64 size = part.size();
        hash = hash_bytes(&size, sizeof(size), hash);
        hash = hash_bytes(part.data(), part.size(), hash);
    }
    return hash_bytes(&content_hash, sizeof(content_hash), hash);
}

std::string_view link_state_name(LinkState state) {
    switch (state) {
    case LinkState::connecting:
        return "connecting";
    case LinkState::connected:
        return "connected";
    case LinkState::rejected:
        return "rejected";
    case LinkState::timed_out:
        return "timed out";
    case LinkState::left:
        return "left";
    }
    return "unknown";
}

std::string_view reject_reason_name(RejectReason reason) {
    switch (reason) {
    case RejectReason::none:
        return "none";
    case RejectReason::protocol:
        return "another protocol version";
    case RejectReason::model:
        return "another sync model";
    case RejectReason::game:
        return "another game, version or content";
    case RejectReason::config:
        return "other session settings";
    case RejectReason::session:
        return "another session";
    case RejectReason::slot:
        return "an unexpected player slot";
    }
    return "unknown";
}

Links::Links(Transport& transport, const SessionIdentity& identity, const LinksConfig& config)
    : _transport(transport), _identity(identity), _config(config) {}

std::size_t Links::add(PeerId peer, u32 slot) {
    _links.push_back({.peer = peer, .slot = slot});
    _tracks.push_back({});
    return _links.size() - 1;
}

std::size_t Links::find(PeerId peer) const {
    for (std::size_t i = 0; i < _links.size(); ++i) {
        if (_links[i].peer == peer) {
            return i;
        }
    }
    return npos;
}

bool Links::all_connected() const {
    for (const LinkInfo& link : _links) {
        if (link.state != LinkState::connected) {
            return false;
        }
    }
    return true;
}

std::size_t Links::first_failed() const {
    for (std::size_t i = 0; i < _links.size(); ++i) {
        const LinkState state = _links[i].state;
        if (state == LinkState::rejected || state == LinkState::timed_out || state == LinkState::left) {
            return i;
        }
    }
    return _links.size();
}

std::size_t Links::max_payload() const {
    const std::size_t datagram = _transport.max_datagram();
    return datagram > message_header_size ? datagram - message_header_size : 0;
}

void Links::update(u64 now_us, const MessageHandler& on_message) {
    _now_us = std::max(_now_us, now_us);
    _received.clear();
    _transport.receive(_received);
    for (const Datagram& datagram : _received) {
        receive(datagram, on_message);
    }
    for (std::size_t i = 0; i < _links.size(); ++i) {
        LinkInfo& link = _links[i];
        Track& track = _tracks[i];
        if (!track.started) {
            track.started = true;
            track.added_us = _now_us;
            track.next_hello_us = _now_us;
        }
        if (link.state == LinkState::connecting) {
            if (_now_us - track.added_us > _config.connect_timeout_us) {
                link.state = LinkState::timed_out;
                continue;
            }
            if (_now_us >= track.next_hello_us) {
                send_hello(i, track.heard);
                track.next_hello_us = _now_us + _config.handshake_interval_us;
            }
        } else if (link.state == LinkState::connected) {
            if (_now_us - link.last_heard_us > _config.timeout_us) {
                link.state = LinkState::timed_out;
                continue;
            }
            if (_now_us >= track.next_ping_us) {
                std::vector<u8> payload;
                ByteWriter writer(payload);
                writer.write_u64(_now_us);
                send_raw(i, static_cast<u8>(LinkMessage::ping), payload);
                track.next_ping_us = _now_us + _config.ping_interval_us;
            }
        }
    }
}

void Links::receive(const Datagram& datagram, const MessageHandler& on_message) {
    const std::span<const u8> bytes = datagram.bytes;
    if (bytes.size() < message_header_size || bytes[0] != magic_0 || bytes[1] != magic_1) {
        return;
    }
    const std::size_t index = find(datagram.from);
    if (index == npos) {
        return;
    }
    LinkInfo& link = _links[index];
    if (link.state != LinkState::connecting && link.state != LinkState::connected) {
        return;
    }
    ++link.packets_received;
    link.bytes_received += bytes.size();
    link.last_heard_us = _now_us;
    const u8 version = bytes[2];
    const u8 type = bytes[3];
    ByteReader reader(bytes.subspan(message_header_size));
    if (type == static_cast<u8>(LinkMessage::reject)) {
        const u8 reason = reader.read_u8();
        link.state = LinkState::rejected;
        link.reason = reader.ok() && reason <= static_cast<u8>(RejectReason::slot) ? static_cast<RejectReason>(reason)
                                                                                   : RejectReason::protocol;
        return;
    }
    if (version != protocol_version) {
        send_reject(link.peer, RejectReason::protocol);
        link.state = LinkState::rejected;
        link.reason = RejectReason::protocol;
        return;
    }
    switch (static_cast<LinkMessage>(type)) {
    case LinkMessage::hello:
        on_hello(index, reader);
        return;
    case LinkMessage::ping: {
        if (link.state == LinkState::connecting && _tracks[index].heard) {
            connected(index);
        }
        const u64 sent = reader.read_u64();
        if (reader.ok() && link.state == LinkState::connected) {
            std::vector<u8> payload;
            ByteWriter writer(payload);
            writer.write_u64(sent);
            send_raw(index, static_cast<u8>(LinkMessage::pong), payload);
        }
        return;
    }
    case LinkMessage::pong: {
        const u64 sent = reader.read_u64();
        if (reader.ok() && sent <= _now_us) {
            const f32 sample = static_cast<f32>(_now_us - sent) / 1000.0f;
            link.rtt_ms = link.rtt_ms == 0.0f ? sample : link.rtt_ms + (sample - link.rtt_ms) / 8.0f;
        }
        return;
    }
    case LinkMessage::bye:
        link.state = LinkState::left;
        return;
    case LinkMessage::reject:
        return;
    }
    if (type < first_model_message) {
        return;
    }
    // A model message from a peer we have heard means it has heard us too: it only sends them once connected.
    if (link.state == LinkState::connecting && _tracks[index].heard) {
        connected(index);
    }
    if (link.state == LinkState::connected && on_message) {
        on_message(index, type, reader);
    }
}

void Links::on_hello(std::size_t index, ByteReader& reader) {
    SessionIdentity theirs;
    theirs.model = reader.read_u32();
    theirs.game = reader.read_u64();
    theirs.config = reader.read_u64();
    theirs.session = reader.read_u64();
    const u32 slot = reader.read_u32();
    const bool heard_you = reader.read_u8() != 0;
    if (!reader.ok()) {
        return;
    }
    LinkInfo& link = _links[index];
    RejectReason reason = RejectReason::none;
    if (theirs.model != _identity.model) {
        reason = RejectReason::model;
    } else if (theirs.game != _identity.game) {
        reason = RejectReason::game;
    } else if (theirs.config != _identity.config) {
        reason = RejectReason::config;
    } else if (theirs.session != _identity.session) {
        reason = RejectReason::session;
    } else if (slot != link.slot) {
        reason = RejectReason::slot;
    }
    if (reason != RejectReason::none) {
        send_reject(link.peer, reason);
        link.state = LinkState::rejected;
        link.reason = reason;
        return;
    }
    _tracks[index].heard = true;
    if (heard_you && link.state == LinkState::connecting) {
        connected(index);
    } else if (!heard_you) {
        // Answer at once: the peer is waiting to hear that we heard it.
        send_hello(index, true);
    }
}

void Links::connected(std::size_t index) {
    LinkInfo& link = _links[index];
    link.state = LinkState::connected;
    link.last_heard_us = _now_us;
    _tracks[index].next_ping_us = _now_us;
    // One hello that says we heard the peer, so its side completes without waiting for our first model message.
    send_hello(index, true);
}

void Links::send_hello(std::size_t index, bool heard_you) {
    std::vector<u8> payload;
    ByteWriter writer(payload);
    writer.write_u32(_identity.model);
    writer.write_u64(_identity.game);
    writer.write_u64(_identity.config);
    writer.write_u64(_identity.session);
    writer.write_u32(_config.local_slot);
    writer.write_u8(heard_you ? 1 : 0);
    send_raw(index, static_cast<u8>(LinkMessage::hello), payload);
}

void Links::send_reject(PeerId peer, RejectReason reason) {
    const u8 datagram[] = {magic_0, magic_1, protocol_version, static_cast<u8>(LinkMessage::reject),
                           static_cast<u8>(reason)};
    _transport.send(peer, datagram);
}

bool Links::send(std::size_t link, u8 type, std::span<const u8> payload) {
    if (link >= _links.size() || type < first_model_message || _links[link].state != LinkState::connected) {
        return false;
    }
    return send_raw(link, type, payload);
}

bool Links::send_raw(std::size_t index, u8 type, std::span<const u8> payload) {
    if (payload.size() > max_payload()) {
        return false;
    }
    _scratch.clear();
    _scratch.insert(_scratch.end(), {magic_0, magic_1, protocol_version, type});
    _scratch.insert(_scratch.end(), payload.begin(), payload.end());
    LinkInfo& link = _links[index];
    if (!_transport.send(link.peer, _scratch)) {
        return false;
    }
    ++link.packets_sent;
    link.bytes_sent += _scratch.size();
    return true;
}

void Links::leave() {
    for (std::size_t i = 0; i < _links.size(); ++i) {
        LinkInfo& link = _links[i];
        if (link.state == LinkState::connecting || link.state == LinkState::connected) {
            // Three copies: nothing answers a goodbye, and a lost one costs the peer a whole timeout.
            for (int copy = 0; copy < 3; ++copy) {
                send_raw(i, static_cast<u8>(LinkMessage::bye), {});
            }
            link.state = LinkState::left;
        }
    }
}

} // namespace kin::net
