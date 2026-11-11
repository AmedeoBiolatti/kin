#include <kin/net/udp.hpp>

#include <array>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: winsock2.h must come first.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// clang-format on
#ifndef SIO_UDP_CONNRESET
// mstcpip.h declares it only for some Windows versions; its value is fixed.
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kin::net {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
using AddressLength = int;

void close_socket(Socket socket) {
    closesocket(socket);
}

bool would_block() {
    const int error = WSAGetLastError();
    // WSAECONNRESET: an earlier datagram met an ICMP "port unreachable"; it says nothing about this receive.
    return error == WSAEWOULDBLOCK || error == WSAECONNRESET || error == WSAEMSGSIZE;
}

std::string last_error() {
    return "WSA error " + std::to_string(WSAGetLastError());
}

bool start_sockets(std::string* error) {
    // Winsock is started once for the process and left running: WSACleanup at exit would race other users.
    static std::once_flag once;
    static int result = 0;
    std::call_once(once, [] {
        WSADATA data{};
        result = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (result != 0 && error) {
        *error = "WSAStartup failed: " + std::to_string(result);
    }
    return result == 0;
}
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
using AddressLength = socklen_t;

void close_socket(Socket socket) {
    ::close(socket);
}

bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED;
}

std::string last_error() {
    return std::strerror(errno);
}

bool start_sockets(std::string*) {
    return true;
}
#endif

bool set_non_blocking(Socket socket) {
#ifdef _WIN32
    u_long non_blocking = 1;
    if (ioctlsocket(socket, FIONBIO, &non_blocking) != 0) {
        return false;
    }
    // Without this, an ICMP "port unreachable" for an earlier datagram fails the next recvfrom (a peer that left).
    BOOL report_resets = FALSE;
    DWORD returned = 0;
    WSAIoctl(socket, SIO_UDP_CONNRESET, &report_resets, sizeof(report_resets), nullptr, 0, &returned, nullptr, nullptr);
    return true;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    // Not inherited by processes the game starts (SOCK_CLOEXEC where socket() takes it).
    fcntl(socket, F_SETFD, FD_CLOEXEC);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

std::string resolve_error(int status) {
#ifdef _WIN32
    return gai_strerrorA(status); // gai_strerror is the wide version in UNICODE builds
#else
    return gai_strerror(status);
#endif
}

struct Address {
    sockaddr_storage storage{};
    AddressLength length = 0;

    // The bytes that name an endpoint: family, address and port (not the padding the storage has around them).
    bool same(const Address& other) const {
        if (storage.ss_family != other.storage.ss_family) {
            return false;
        }
        if (storage.ss_family == AF_INET) {
            const auto& a = reinterpret_cast<const sockaddr_in&>(storage);
            const auto& b = reinterpret_cast<const sockaddr_in&>(other.storage);
            return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr;
        }
        const auto& a = reinterpret_cast<const sockaddr_in6&>(storage);
        const auto& b = reinterpret_cast<const sockaddr_in6&>(other.storage);
        return a.sin6_port == b.sin6_port && std::memcmp(&a.sin6_addr, &b.sin6_addr, sizeof(a.sin6_addr)) == 0 &&
               a.sin6_scope_id == b.sin6_scope_id;
    }

    std::string text() const {
        std::array<char, INET6_ADDRSTRLEN> buffer{};
        u16 port = 0;
        if (storage.ss_family == AF_INET) {
            const auto& a = reinterpret_cast<const sockaddr_in&>(storage);
            inet_ntop(AF_INET, &a.sin_addr, buffer.data(), static_cast<AddressLength>(buffer.size()));
            port = ntohs(a.sin_port);
            return std::string(buffer.data()) + ":" + std::to_string(port);
        }
        const auto& a = reinterpret_cast<const sockaddr_in6&>(storage);
        inet_ntop(AF_INET6, &a.sin6_addr, buffer.data(), static_cast<AddressLength>(buffer.size()));
        port = ntohs(a.sin6_port);
        return "[" + std::string(buffer.data()) + "]:" + std::to_string(port);
    }
};

} // namespace

struct UdpTransport::Impl {
    Socket socket = invalid_socket;
    UdpOptions options;
    std::vector<Address> peers; // peers[id - 1]
    std::vector<u8> buffer = std::vector<u8>(65536);

    ~Impl() {
        if (socket != invalid_socket) {
            close_socket(socket);
        }
    }

    PeerId find(const Address& address) const {
        for (std::size_t i = 0; i < peers.size(); ++i) {
            if (peers[i].same(address)) {
                return static_cast<PeerId>(i + 1);
            }
        }
        return no_peer;
    }

    PeerId insert(const Address& address) {
        if (const PeerId known = find(address); known != no_peer) {
            return known;
        }
        if (peers.size() >= options.max_peers) {
            return no_peer;
        }
        peers.push_back(address);
        return static_cast<PeerId>(peers.size());
    }
};

UdpTransport::UdpTransport(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

UdpTransport::~UdpTransport() = default;

std::unique_ptr<UdpTransport> UdpTransport::open(const UdpOptions& options, std::string* error) {
    if (!start_sockets(error)) {
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    const int family = options.ipv6 ? AF_INET6 : AF_INET;
#ifdef SOCK_CLOEXEC
    impl->socket = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
#else
    impl->socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
#endif
    if (impl->socket == invalid_socket) {
        if (error) {
            *error = "socket: " + last_error();
        }
        return nullptr;
    }
    if (options.ipv6) {
        int v6_only = 0;
        setsockopt(impl->socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6_only), sizeof(v6_only));
    }
    Address bind_address;
    if (options.ipv6) {
        auto& a = reinterpret_cast<sockaddr_in6&>(bind_address.storage);
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(options.port);
        bind_address.length = sizeof(sockaddr_in6);
    } else {
        auto& a = reinterpret_cast<sockaddr_in&>(bind_address.storage);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(options.port);
        bind_address.length = sizeof(sockaddr_in);
    }
    if (::bind(impl->socket, reinterpret_cast<const sockaddr*>(&bind_address.storage), bind_address.length) != 0) {
        if (error) {
            *error = "bind to port " + std::to_string(options.port) + ": " + last_error();
        }
        return nullptr;
    }
    if (!set_non_blocking(impl->socket)) {
        if (error) {
            *error = "cannot make the socket non-blocking: " + last_error();
        }
        return nullptr;
    }
    return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(impl)));
}

bool UdpTransport::send(PeerId to, std::span<const u8> bytes) {
    if (to == no_peer || to > _impl->peers.size() || bytes.size() > max_datagram()) {
        return false;
    }
    const Address& address = _impl->peers[to - 1];
    // A full socket buffer or an unreachable host loses this datagram, which the layers above expect of UDP.
    ::sendto(_impl->socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
             reinterpret_cast<const sockaddr*>(&address.storage), address.length);
    return true;
}

void UdpTransport::receive(std::vector<Datagram>& out) {
    for (;;) {
        Address from;
        from.length = sizeof(from.storage);
        const auto received = ::recvfrom(_impl->socket, reinterpret_cast<char*>(_impl->buffer.data()),
                                         static_cast<int>(_impl->buffer.size()), 0,
                                         reinterpret_cast<sockaddr*>(&from.storage), &from.length);
        if (received < 0) {
            if (would_block()) {
                // A reset or an oversized datagram on Windows is reported per datagram: keep reading after it.
#ifdef _WIN32
                if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    continue;
                }
#endif
            }
            return;
        }
        PeerId peer = _impl->find(from);
        // Only a datagram that starts like kin's (a hello) earns a stranger a place: port scans and stray packets
        // would otherwise fill the table for good.
        if (peer == no_peer && _impl->options.accept_unknown && received >= 2 && _impl->buffer[0] == 'k' &&
            _impl->buffer[1] == 'n') {
            peer = _impl->insert(from);
        }
        if (peer == no_peer) {
            continue;
        }
        out.push_back(
            {.from = peer,
             .bytes = {_impl->buffer.begin(), _impl->buffer.begin() + static_cast<std::ptrdiff_t>(received)}});
    }
}

u16 UdpTransport::local_port() const {
    Address address;
    address.length = sizeof(address.storage);
    if (::getsockname(_impl->socket, reinterpret_cast<sockaddr*>(&address.storage), &address.length) != 0) {
        return 0;
    }
    if (address.storage.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in&>(address.storage).sin_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in6&>(address.storage).sin6_port);
}

PeerId UdpTransport::add_peer(std::string_view host, u16 port, std::string* error) {
    addrinfo hints{};
    // A dual-stack socket takes IPv4 peers as IPv4-mapped IPv6 addresses. They are mapped here rather than with
    // AI_V4MAPPED, which Android's resolver refuses.
    hints.ai_family = _impl->options.ipv6 ? AF_UNSPEC : AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* results = nullptr;
    const std::string host_text(host);
    const std::string port_text = std::to_string(port);
    if (const int status = getaddrinfo(host_text.c_str(), port_text.c_str(), &hints, &results);
        status != 0 || !results) {
        if (error) {
            *error = "cannot resolve " + host_text + ": " + resolve_error(status);
        }
        return no_peer;
    }
    Address address;
    if (_impl->options.ipv6 && results->ai_family == AF_INET) {
        const auto& v4 = *reinterpret_cast<const sockaddr_in*>(results->ai_addr);
        auto& v6 = reinterpret_cast<sockaddr_in6&>(address.storage);
        v6.sin6_family = AF_INET6;
        v6.sin6_port = v4.sin_port;
        auto* bytes = reinterpret_cast<u8*>(&v6.sin6_addr);
        bytes[10] = 0xff;
        bytes[11] = 0xff;
        std::memcpy(bytes + 12, &v4.sin_addr, 4);
        address.length = sizeof(sockaddr_in6);
    } else {
        std::memcpy(&address.storage, results->ai_addr, results->ai_addrlen);
        address.length = static_cast<AddressLength>(results->ai_addrlen);
    }
    freeaddrinfo(results);
    const PeerId peer = _impl->insert(address);
    if (peer == no_peer && error) {
        *error = "the peer table is full (" + std::to_string(_impl->options.max_peers) + " peers)";
    }
    return peer;
}

std::string UdpTransport::peer_address(PeerId peer) const {
    if (peer == no_peer || peer > _impl->peers.size()) {
        return {};
    }
    return _impl->peers[peer - 1].text();
}

} // namespace kin::net
