#pragma once

// The interface every sync model's session shares (docs/networking.md), so the app loop, a lobby screen and the debug
// overlay drive and show a session without knowing its model. Each model adds its own typed calls (input_sync.hpp:
// set_local_input(); a replication model: its snapshots).

#include <kin/net/link.hpp>

#include <span>
#include <string>
#include <string_view>

namespace kin::net {

enum class SessionStatus : u8 {
    connecting, // handshaking with the peers
    running,
    desynced,  // a peer's state hash differs from ours: the simulations no longer agree
    peer_lost, // a peer timed out or left
    rejected,  // a peer refused us (or we refused it): another game, build, model or setting
    ended,     // we left
    failed,    // the session could not start (the reason is in status_detail())
};

std::string_view session_status_name(SessionStatus status);

inline bool session_over(SessionStatus status) {
    return status != SessionStatus::connecting && status != SessionStatus::running;
}

class Session {
public:
    virtual ~Session() = default;

    // Receives, simulates what is due and sends. `now_us` is a monotonic clock that never goes back; tests pass a
    // virtual one. Call it every rendered frame.
    virtual void update(u64 now_us) = 0;
    virtual SessionStatus status() const = 0;
    // Why the session is not running, in a sentence ("" while it is).
    virtual std::string status_detail() const = 0;
    virtual std::span<const LinkInfo> links() const = 0;
    // Says goodbye to the peers; the session ends.
    virtual void leave() = 0;
};

} // namespace kin::net
