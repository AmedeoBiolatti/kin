#pragma once

// What every session needs from its peers before and beside its own messages (docs/networking.md): a handshake
// that refuses a peer of another game, build, sync model or configuration; round-trip times; and timeouts. A sync
// model (input_sync.hpp; later a replication model) owns one Links and sends its messages through it, so each model
// is only what goes on the wire for it.
//
// Every datagram starts with a 12-byte header: 'k' 'n', the protocol version, the message type, and the receiver's
// token: a random number each side picks per link and sends in its hello. A message whose token is not the one this
// side picked (a late one from an earlier match, or a forged one) is ignored, so only the peer of this handshake can
// end or drive the link. Types below first_model_message are the links' own; a model numbers its messages from there.

#include <kin/net/bytes.hpp>
#include <kin/net/transport.hpp>

#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace kin::net {

inline constexpr u8 protocol_version = 1;
inline constexpr u8 first_model_message = 32;
inline constexpr std::size_t message_header_size = 12;

// What two peers must agree on to play together. A peer whose identity differs is refused, with the first field that
// differs as the reason.
struct SessionIdentity {
    u32 model = 0;   // the sync model (input sync, replication, ...): two models never talk to each other
    u64 game = 0;    // the game's compatibility hash: id, version, build, content (see compatibility_hash())
    u64 config = 0;  // the model's settings both sides must share (tick rate, players, ...)
    u64 session = 0; // this match, as a lobby numbers it (0 if there is none)
    friend bool operator==(const SessionIdentity&, const SessionIdentity&) = default;
};

// A compatibility hash from the parts of a game that must match: its id and version, and optionally a build id and a
// content hash. Peers built from other sources, or with other content, then refuse each other instead of desyncing.
u64 compatibility_hash(std::string_view game_id, std::string_view version, std::string_view build = {},
                       u64 content_hash = 0);

enum class LinkState : u8 {
    connecting, // handshaking
    connected,
    rejected,  // the identities differ (reason says how), either side found it
    timed_out, // nothing heard for too long
    left,      // the peer said goodbye
};

enum class RejectReason : u8 { none, protocol, model, game, config, session, slot };

std::string_view link_state_name(LinkState state);
std::string_view reject_reason_name(RejectReason reason);

struct LinkInfo {
    PeerId peer = no_peer;
    u32 slot = 0; // the player (or role) the peer is expected to be
    LinkState state = LinkState::connecting;
    RejectReason reason = RejectReason::none;
    f32 rtt_ms = 0.0f; // smoothed round trip; 0 until the first answer
    u64 last_heard_us = 0;
    u64 packets_sent = 0, packets_received = 0;
    u64 bytes_sent = 0, bytes_received = 0;
};

struct LinksConfig {
    u32 local_slot = 0;
    u32 handshake_interval_us = 100'000;
    u32 ping_interval_us = 250'000;
    u32 connect_timeout_us = 10'000'000; // a peer that never answers
    u32 timeout_us = 5'000'000;          // a connected peer that goes quiet
};

class Links {
public:
    // A model message from a connected peer: the link's index, the type and the bytes after the header.
    using MessageHandler = std::function<void(std::size_t link, u8 type, ByteReader& payload)>;

    Links(Transport& transport, const SessionIdentity& identity, const LinksConfig& config = {});

    static constexpr std::size_t no_link = static_cast<std::size_t>(-1);

    // A peer expected in `slot`. Connecting starts at the next update(); returns the link's index, or no_link if the
    // peer already has a link (its datagrams could reach only one of them).
    std::size_t add(PeerId peer, u32 slot);

    // Reads every datagram, answers the links' own messages, hands model messages from connected peers to
    // `on_message`, then sends handshakes and pings that are due. `now_us` is any monotonic clock that never goes back.
    void update(u64 now_us, const MessageHandler& on_message);

    // Sends a model message (type >= first_model_message) to one link. False if the link is not connected or the
    // message does not fit in one datagram.
    bool send(std::size_t link, u8 type, std::span<const u8> payload);
    // The largest payload send() takes.
    std::size_t max_payload() const;
    // Says goodbye to every connected peer (the session ends for them at once instead of timing out).
    void leave();

    std::span<const LinkInfo> links() const { return _links; }
    const LinkInfo& link(std::size_t index) const { return _links[index]; }
    bool all_connected() const;
    // The first link that failed (rejected, timed out or left), or links().size() if none did.
    std::size_t first_failed() const;
    const SessionIdentity& identity() const { return _identity; }
    u64 now() const { return _now_us; }

private:
    struct Track {
        bool heard = false; // a hello from the peer arrived
        u64 token = 0;      // ours: the peer's messages must carry it
        u64 their_token = 0;
        // A hello that differed but may be a late one from an earlier match: the reason, if the link times out.
        RejectReason mismatch = RejectReason::none;
        u64 added_us = 0;
        u64 next_hello_us = 0;
        u64 next_ping_us = 0;
        bool started = false;
    };

    void receive(const Datagram& datagram, const MessageHandler& on_message);
    void on_hello(std::size_t index, ByteReader& reader, bool heard_you, std::size_t size);
    void heard(LinkInfo& link, std::size_t bytes);
    void send_hello(std::size_t index);
    void send_reject(PeerId peer, RejectReason reason, u64 token);
    bool send_raw(std::size_t index, u8 type, std::span<const u8> payload);
    void connected(std::size_t index);
    std::size_t find(PeerId peer) const;

    Transport& _transport;
    SessionIdentity _identity;
    LinksConfig _config;
    std::vector<LinkInfo> _links;
    std::vector<Track> _tracks;
    std::vector<Datagram> _received;
    std::vector<u8> _scratch;
    u64 _now_us = 0;
};

} // namespace kin::net
