// FakeServices' lobbies and messages, over a FakeNetwork.

#include <algorithm>
#include <kin/platform/services.hpp>

namespace kin {
namespace {

std::string* find_value(std::vector<std::pair<std::string, std::string>>& data, std::string_view key) {
    for (auto& [k, v] : data) {
        if (k == key) {
            return &v;
        }
    }
    return nullptr;
}

void put_value(std::vector<std::pair<std::string, std::string>>& data, std::string_view key, std::string_view value) {
    if (std::string* found = find_value(data, key)) {
        *found = value;
    } else {
        data.emplace_back(std::string{key}, std::string{value});
    }
}

bool passes(const LobbyFilter::Rule& rule, const std::string& value) {
    int order = 0;
    if (rule.numeric) {
        const long long a = std::atoll(value.c_str());
        const long long b = std::atoll(rule.value.c_str());
        order = a < b ? -1 : a > b ? 1 : 0;
    } else {
        order = value < rule.value ? -1 : value > rule.value ? 1 : 0;
    }
    switch (rule.compare) {
    case LobbyCompare::AtMost:
        return order <= 0;
    case LobbyCompare::Less:
        return order < 0;
    case LobbyCompare::Equal:
        return order == 0;
    case LobbyCompare::Greater:
        return order > 0;
    case LobbyCompare::AtLeast:
        return order >= 0;
    case LobbyCompare::NotEqual:
        return order != 0;
    }
    return false;
}

} // namespace

// FakeNetwork

FakeNetwork::~FakeNetwork() {
    for (FakeServices* player : _players) {
        player->_network = nullptr;
        player->_lobby.clear();
    }
}

FakeNetwork::Lobby* FakeNetwork::find(std::string_view id) {
    for (Lobby& lobby : _lobbies) {
        if (lobby.id == id) {
            return &lobby;
        }
    }
    return nullptr;
}

const FakeNetwork::Lobby* FakeNetwork::find(std::string_view id) const {
    return const_cast<FakeNetwork*>(this)->find(id);
}

FakeServices* FakeNetwork::player(std::string_view id) const {
    for (FakeServices* player : _players) {
        if (player->player_.id == id) {
            return player;
        }
    }
    return nullptr;
}

std::vector<LobbyInfo> FakeNetwork::lobbies() const {
    std::vector<LobbyInfo> out;
    for (const Lobby& lobby : _lobbies) {
        out.push_back({.id = lobby.id,
                       .members = static_cast<u32>(lobby.members.size()),
                       .max_members = lobby.max_members,
                       .data = lobby.data});
    }
    return out;
}

void FakeNetwork::leave(FakeServices& who, bool asked) {
    Lobby* lobby = find(who._lobby);
    const std::string lobby_id = who._lobby;
    who._lobby.clear();
    if (!lobby) {
        return;
    }
    std::erase(lobby->members, &who);
    std::erase_if(lobby->member_data, [&](const auto& entry) { return entry.first == who.player_.id; });
    if (!asked) {
        who.send({.kind = ServiceEventKind::LobbyLeft, .id = lobby_id});
    }
    if (lobby->members.empty()) {
        std::erase_if(_lobbies, [&](const Lobby& l) { return l.id == lobby_id; });
        return;
    }
    if (lobby->owner == who.player_.id) {
        lobby->owner = lobby->members.front()->player_.id; // as Steam does: the next member owns it
    }
    for (FakeServices* member : lobby->members) {
        member->send({.kind = ServiceEventKind::LobbyMemberLeft, .id = who.player_.id});
    }
}

// FakeServices

FakeServices::FakeServices(FakeNetwork& network, ServicePlayer player) : player_(std::move(player)) {
    _network = &network;
    network._players.push_back(this);
}

FakeServices::~FakeServices() {
    if (_network) {
        _network->leave(*this, true);
        std::erase(_network->_players, this);
    }
}

u32 FakeServices::create_lobby(const LobbyOptions& options) {
    calls.push_back({"create_lobby", {}, std::to_string(options.max_members)});
    if (!_network || !online_) {
        return 0;
    }
    const u32 request = _next_request++;
    if (!_lobby.empty()) {
        leave_lobby();
    }
    FakeNetwork::Lobby lobby{.id = "lobby:" + std::to_string(_network->_next_lobby++),
                             .owner = player_.id,
                             .members = {this},
                             .max_members = std::max<u32>(options.max_members, 1),
                             .visibility = options.visibility,
                             .data = options.data};
    _lobby = lobby.id;
    _network->_lobbies.push_back(std::move(lobby));
    send({.kind = ServiceEventKind::LobbyEntered, .id = _lobby, .request = request, .ok = true});
    return request;
}

u32 FakeServices::join_lobby(std::string_view id) {
    calls.push_back({"join_lobby", std::string{id}});
    if (!_network || !online_) {
        return 0;
    }
    const u32 request = _next_request++;
    if (_lobby == id) {
        send({.kind = ServiceEventKind::LobbyEntered, .id = _lobby, .request = request, .ok = true});
        return request;
    }
    FakeNetwork::Lobby* lobby = _network->find(id);
    const char* why = !lobby                                        ? "no such lobby"
                      : !lobby->joinable                            ? "the lobby is closed"
                      : lobby->members.size() >= lobby->max_members ? "the lobby is full"
                                                                    : nullptr;
    if (why) {
        send({.kind = ServiceEventKind::LobbyFailed, .id = std::string{id}, .text = why, .request = request});
        return request;
    }
    if (!_lobby.empty()) {
        leave_lobby();
        lobby = _network->find(id);
    }
    for (FakeServices* member : lobby->members) {
        member->send({.kind = ServiceEventKind::LobbyMemberJoined, .id = player_.id, .text = player_.name});
    }
    lobby->members.push_back(this);
    _lobby = lobby->id;
    send({.kind = ServiceEventKind::LobbyEntered, .id = _lobby, .request = request, .ok = true});
    return request;
}

void FakeServices::leave_lobby() {
    calls.push_back({"leave_lobby", _lobby});
    if (_network) {
        _network->leave(*this, true);
    }
}

u32 FakeServices::find_lobbies(const LobbyFilter& filter) {
    calls.push_back({"find_lobbies"});
    if (!_network || !online_) {
        return 0;
    }
    const u32 request = _next_request++;
    ServiceEvent event{.kind = ServiceEventKind::LobbyList, .request = request, .ok = true};
    for (const FakeNetwork::Lobby& lobby : _network->_lobbies) {
        if (lobby.visibility != LobbyVisibility::Public && lobby.visibility != LobbyVisibility::Invisible) {
            continue;
        }
        if (!lobby.joinable || lobby.max_members - lobby.members.size() < filter.open_slots) {
            continue;
        }
        bool ok = true;
        for (const LobbyFilter::Rule& rule : filter.rules) {
            const std::string* value = find_value(const_cast<FakeNetwork::Lobby&>(lobby).data, rule.key);
            ok = ok && value && passes(rule, *value);
        }
        if (ok && event.lobbies.size() < filter.max_results) {
            event.lobbies.push_back({.id = lobby.id,
                                     .members = static_cast<u32>(lobby.members.size()),
                                     .max_members = lobby.max_members,
                                     .data = lobby.data});
        }
    }
    send(std::move(event));
    return request;
}

std::vector<LobbyMember> FakeServices::lobby_members() const {
    std::vector<LobbyMember> out;
    if (const FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr) {
        for (const FakeServices* member : lobby->members) {
            out.push_back({.id = member->player_.id, .name = member->player_.name});
        }
    }
    return out;
}

std::string FakeServices::lobby_owner() const {
    const FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr;
    return lobby ? lobby->owner : std::string{};
}

bool FakeServices::set_lobby_data(std::string_view key, std::string_view value) {
    calls.push_back({"set_lobby_data", std::string{key}, std::string{value}});
    FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr;
    if (!lobby || lobby->owner != player_.id || key.empty()) {
        return false;
    }
    put_value(lobby->data, key, value);
    for (FakeServices* member : lobby->members) {
        member->send({.kind = ServiceEventKind::LobbyDataChanged});
    }
    return true;
}

std::string FakeServices::lobby_data(std::string_view key) const {
    FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr;
    const std::string* value = lobby ? find_value(lobby->data, key) : nullptr;
    return value ? *value : std::string{};
}

bool FakeServices::set_member_data(std::string_view key, std::string_view value) {
    calls.push_back({"set_member_data", std::string{key}, std::string{value}});
    FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr;
    if (!lobby || key.empty()) {
        return false;
    }
    bool found = false;
    for (auto& [member, entry] : lobby->member_data) {
        if (member == player_.id && entry.first == key) {
            entry.second = value;
            found = true;
        }
    }
    if (!found) {
        lobby->member_data.push_back({player_.id, {std::string{key}, std::string{value}}});
    }
    for (FakeServices* member : lobby->members) {
        member->send({.kind = ServiceEventKind::LobbyDataChanged, .id = player_.id});
    }
    return true;
}

std::string FakeServices::member_data(std::string_view member, std::string_view key) const {
    if (const FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr) {
        for (const auto& [who, entry] : lobby->member_data) {
            if (who == member && entry.first == key) {
                return entry.second;
            }
        }
    }
    return {};
}

bool FakeServices::set_lobby_joinable(bool joinable) {
    calls.push_back({"set_lobby_joinable", {}, joinable ? "true" : "false"});
    FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr;
    if (!lobby || lobby->owner != player_.id) {
        return false;
    }
    lobby->joinable = joinable;
    return true;
}

bool FakeServices::send_lobby_message(std::string_view text) {
    FakeNetwork::Lobby* lobby = _network ? _network->find(_lobby) : nullptr;
    if (!lobby || text.empty()) {
        return false;
    }
    for (FakeServices* member : lobby->members) {
        member->send({.kind = ServiceEventKind::LobbyMessage, .id = player_.id, .text = std::string{text}});
    }
    return true;
}

bool FakeServices::send_message(std::string_view to, std::span<const u8> bytes, MessageDelivery delivery, u32 channel) {
    FakeServices* other = _network ? _network->player(to) : nullptr;
    if (!other || other == this || !online_ ||
        (delivery == MessageDelivery::Unreliable && bytes.size() > max_message())) {
        return false;
    }
    if (std::ranges::find(_contacted, to) == _contacted.end()) {
        _contacted.emplace_back(to);
    }
    if (delivery == MessageDelivery::Unreliable) {
        ++_network->sent;
        if (_network->drop_every != 0 && _network->sent % _network->drop_every == 0) {
            ++_network->dropped;
            return true;
        }
    }
    // The receiver takes it from a lobby member, or a player it sent to first.
    const bool same_lobby = !_lobby.empty() && other->_lobby == _lobby;
    if (!same_lobby && std::ranges::find(other->_contacted, player_.id) == other->_contacted.end()) {
        return true; // sent, never taken (as a platform drops an unaccepted session)
    }
    other->_inbox.push_back({channel, PlayerMessage{.from = player_.id, .bytes = {bytes.begin(), bytes.end()}}});
    return true;
}

void FakeServices::receive_messages(u32 channel, std::vector<PlayerMessage>& out) {
    auto rest = std::stable_partition(_inbox.begin(), _inbox.end(), [&](const auto& m) { return m.first != channel; });
    for (auto it = rest; it != _inbox.end(); ++it) {
        out.push_back(std::move(it->second));
    }
    _inbox.erase(rest, _inbox.end());
}

void FakeServices::close_messages(std::string_view player) {
    std::erase(_contacted, std::string{player});
}

} // namespace kin
