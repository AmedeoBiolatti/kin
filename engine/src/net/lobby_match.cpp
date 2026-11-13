#include <algorithm>
#include <charconv>
#include <kin/net/lobby_match.hpp>
#include <kin/platform/services.hpp>
#include <random>

namespace kin::net {
namespace {

// "1\n<session>\n<slot>\n<slot>...", a version ("2": joinable), the session in hex, then one line a slot: a player's
// id, "+<id>" for one who joined late, "-" for a free slot.
std::string encode(const LobbyMatch& match) {
    char hex[17] = {};
    const auto end = std::to_chars(hex, hex + 16, match.session, 16).ptr;
    std::string text = std::string(match.joinable ? "2" : "1") + "\n" + std::string(hex, end);
    for (u32 slot = 0; slot < match.players.size(); ++slot) {
        text += '\n';
        if (match.players[slot].empty()) {
            text += '-';
        } else {
            text += match.late[slot] ? "+" : "";
            text += match.players[slot];
        }
    }
    return text;
}

std::optional<LobbyMatch> decode(std::string_view text) {
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start <= text.size();) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        lines.emplace_back(text.data() + start, end - start);
        start = end + 1;
    }
    if (lines.size() < 3 || (lines[0] != "1" && lines[0] != "2") || lines.size() - 2 > max_players) {
        return std::nullopt;
    }
    LobbyMatch match;
    match.joinable = lines[0] == "2";
    const auto [end, error] = std::from_chars(lines[1].data(), lines[1].data() + lines[1].size(), match.session, 16);
    if (error != std::errc{} || end != lines[1].data() + lines[1].size() || match.session == 0) {
        return std::nullopt;
    }
    for (std::size_t i = 2; i < lines.size(); ++i) {
        std::string_view line = lines[i];
        const bool late = line.starts_with('+');
        if (late) {
            line.remove_prefix(1);
        }
        // A player listed twice would have two slots and one peer, so the whole match is refused, as are a
        // mangled line (from an owner of another build, or one that writes the data by hand) and a match with no one.
        if (line.empty() || (line == "-" && late) || line.starts_with('+') || line.find('\r') != line.npos ||
            (line != "-" && std::ranges::find(match.players, line) != match.players.end())) {
            return std::nullopt;
        }
        match.players.emplace_back(line == "-" ? std::string_view{} : line);
        match.late.push_back(late);
    }
    if (!std::ranges::any_of(match.players, [](const std::string& p) { return !p.empty(); })) {
        return std::nullopt;
    }
    return match;
}

bool owns_lobby(const PlatformServices& services) {
    return !services.lobby().empty() && services.lobby_owner() == services.player().id;
}

bool has_free_slot(const LobbyMatch& match) {
    return std::ranges::any_of(match.players, [](const std::string& p) { return p.empty(); });
}

} // namespace

bool start_lobby_match(PlatformServices& services, std::span<const std::string> players, u64 session, u32 free_slots) {
    if (!owns_lobby(services)) {
        return false;
    }
    const std::vector<LobbyMember> members = services.lobby_members();
    LobbyMatch match;
    if (players.empty()) {
        for (const LobbyMember& member : members) {
            match.players.push_back(member.id);
        }
    } else {
        for (const std::string& player : players) {
            const bool member = std::ranges::any_of(members, [&](const LobbyMember& m) { return m.id == player; });
            if (!member || player.empty() || std::ranges::find(match.players, player) != match.players.end() ||
                player.find_first_of("\r\n") != std::string::npos || player.starts_with('+') || player == "-") {
                return false;
            }
            match.players.push_back(player);
        }
    }
    if (match.players.empty() || match.players.size() + free_slots > max_players) {
        return false;
    }
    match.players.resize(match.players.size() + free_slots);
    match.late.assign(match.players.size(), false);
    match.joinable = free_slots > 0;
    while (session == 0) {
        std::random_device device;
        session = (static_cast<u64>(device()) << 32) ^ device();
    }
    match.session = session;
    services.set_lobby_joinable(has_free_slot(match));
    return services.set_lobby_data(lobby_match_key, encode(match));
}

bool end_lobby_match(PlatformServices& services) {
    if (!owns_lobby(services) || !services.set_lobby_data(lobby_match_key, "")) {
        return false;
    }
    services.set_lobby_joinable(true);
    return true;
}

std::optional<LobbyMatch> read_lobby_match(const PlatformServices& services) {
    std::optional<LobbyMatch> match = decode(services.lobby_data(lobby_match_key));
    if (!match) {
        return std::nullopt;
    }
    const auto self = std::ranges::find(match->players, services.player().id);
    if (self == match->players.end()) {
        return std::nullopt;
    }
    match->local_player = static_cast<u32>(self - match->players.begin());
    return match;
}

u32 seat_lobby_joiners(PlatformServices& services) {
    if (!owns_lobby(services)) {
        return 0;
    }
    std::optional<LobbyMatch> match = decode(services.lobby_data(lobby_match_key));
    if (!match || !match->joinable) {
        return 0;
    }
    u32 seated = 0;
    for (const LobbyMember& member : services.lobby_members()) {
        if (std::ranges::find(match->players, member.id) != match->players.end() || member.id.empty() ||
            member.id.starts_with('+') || member.id == "-" || member.id.find_first_of("\r\n") != std::string::npos) {
            continue;
        }
        const auto free = std::ranges::find(match->players, std::string{});
        if (free == match->players.end()) {
            break;
        }
        *free = member.id;
        match->late[static_cast<std::size_t>(free - match->players.begin())] = true;
        ++seated;
    }
    if (seated > 0 && !services.set_lobby_data(lobby_match_key, encode(*match))) {
        return 0;
    }
    services.set_lobby_joinable(has_free_slot(*match));
    return seated;
}

bool free_lobby_slot(PlatformServices& services, std::string_view player) {
    if (!owns_lobby(services)) {
        return false;
    }
    std::optional<LobbyMatch> match = decode(services.lobby_data(lobby_match_key));
    if (!match) {
        return false;
    }
    const auto it = std::ranges::find(match->players, player);
    if (it == match->players.end() || !match->late[static_cast<std::size_t>(it - match->players.begin())]) {
        return false; // only a latecomer's slot is freed: one who started the match is part of it
    }
    match->late[static_cast<std::size_t>(it - match->players.begin())] = false;
    it->clear();
    services.set_lobby_joinable(true);
    return services.set_lobby_data(lobby_match_key, encode(*match));
}

void configure_input_sync(const LobbyMatch& match, ServicesTransport& transport, InputSyncConfig& config) {
    config.players = static_cast<u32>(match.players.size());
    config.local_player = match.local_player;
    config.session = match.session;
    config.joinable = match.joinable;
    config.joining = match.joinable && match.late[match.local_player];
    config.peers.clear();
    for (u32 slot = 0; slot < match.players.size(); ++slot) {
        const bool other = slot != match.local_player && !match.players[slot].empty();
        // A joiner meets everyone who plays; a player in the game meets latecomers through allow_lobby_joiners.
        const bool now = other && (config.joining || !match.late[slot]);
        config.peers.push_back(now ? transport.add_peer(match.players[slot]) : no_peer);
    }
}

} // namespace kin::net
