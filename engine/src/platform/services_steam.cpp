#include "services_detail.hpp"

#if defined(__EMSCRIPTEN__)

namespace kin::services_detail {

std::unique_ptr<PlatformServices> start_steam(const ServicesConfig&, ServicesStart& start) {
    start = ServicesStart::Offline;
    return nullptr;
}

} // namespace kin::services_detail

#else

#include "steam_flat.hpp"

#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <kin/platform/log.hpp>
#include <kin/platform/resource_paths.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace kin::services_detail {
namespace {

namespace sf = steam_flat;

constexpr double store_interval_seconds = 2.0;

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

template <class T> T payload(const u8* data, int size) {
    T value{};
    if (data && size > 0) {
        std::memcpy(&value, data, std::min<std::size_t>(sizeof(T), static_cast<std::size_t>(size)));
    }
    return value;
}

std::optional<u64> parse_u64(std::string_view text) {
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
    }
    return value;
}

// The Steam API library, opened, with every function kin calls.
struct SteamApi {
    SDL_SharedObject* library = nullptr;
#define KIN_STEAM_FIELD(field, symbol, ret, params) sf::field##_fn field = nullptr;
    KIN_STEAM_FUNCTIONS(KIN_STEAM_FIELD)
    KIN_STEAM_OPTIONAL_FUNCTIONS(KIN_STEAM_FIELD)
    KIN_STEAM_NETWORK_FUNCTIONS(KIN_STEAM_FIELD)
    KIN_STEAM_LEGACY_FUNCTIONS(KIN_STEAM_FIELD)
#undef KIN_STEAM_FIELD
    std::string missing;
    std::string missing_network; // lobbies and messages need every one of theirs

    ~SteamApi() {
        if (library) {
            SDL_UnloadObject(library);
        }
    }

    bool open(const std::filesystem::path& where) {
        std::vector<std::filesystem::path> candidates;
        if (!where.empty()) {
            std::error_code error;
            candidates.push_back(std::filesystem::is_directory(where, error) ? where / sf::library_name : where);
        } else {
            candidates.push_back(executable_dir() / sf::library_name);
            candidates.emplace_back(sf::library_name);
        }
        for (const std::filesystem::path& path : candidates) {
            library = SDL_LoadObject(path.string().c_str());
            if (library) {
                KIN_LOG_INFO_F("services", "steam api library", (LogFields{{.name = "path", .value = path.string()}}));
                break;
            }
        }
        if (!library) {
            return false;
        }
#define KIN_STEAM_LOAD(field, symbol, ret, params)                                                                     \
    field = reinterpret_cast<sf::field##_fn>(SDL_LoadFunction(library, #symbol));                                      \
    if (!field) {                                                                                                      \
        missing += missing.empty() ? #symbol : ", " #symbol;                                                           \
    }
        KIN_STEAM_FUNCTIONS(KIN_STEAM_LOAD)
#undef KIN_STEAM_LOAD
#define KIN_STEAM_LOAD_OPTIONAL(field, symbol, ret, params)                                                            \
    field = reinterpret_cast<sf::field##_fn>(SDL_LoadFunction(library, #symbol));
        KIN_STEAM_OPTIONAL_FUNCTIONS(KIN_STEAM_LOAD_OPTIONAL)
        KIN_STEAM_LEGACY_FUNCTIONS(KIN_STEAM_LOAD_OPTIONAL)
#undef KIN_STEAM_LOAD_OPTIONAL
#define KIN_STEAM_LOAD_NETWORK(field, symbol, ret, params)                                                             \
    field = reinterpret_cast<sf::field##_fn>(SDL_LoadFunction(library, #symbol));                                      \
    if (!field) {                                                                                                      \
        missing_network += missing_network.empty() ? #symbol : ", " #symbol;                                           \
    }
        KIN_STEAM_NETWORK_FUNCTIONS(KIN_STEAM_LOAD_NETWORK)
#undef KIN_STEAM_LOAD_NETWORK
        return missing.empty();
    }

    template <std::size_t N> void* interface(const char* prefix, const char* const (&versions)[N]) const {
        for (const char* version : versions) {
            const std::string symbol = std::string{"SteamAPI_"} + prefix + "_" + version;
            if (auto accessor = reinterpret_cast<void* (*)()>(SDL_LoadFunction(library, symbol.c_str()))) {
                if (void* found = accessor()) {
                    return found;
                }
            }
        }
        return nullptr;
    }
};

std::string steam_player_id(u64 id) {
    return "steam:" + std::to_string(id);
}

std::string steam_lobby_id(u64 id) {
    return "lobby:" + std::to_string(id);
}

// "steam:<id>" (a player) or "lobby:<id>" (a lobby), or the bare number.
std::optional<u64> steam_id_of(std::string_view text, std::string_view prefix) {
    if (text.starts_with(prefix)) {
        text.remove_prefix(prefix.size());
    }
    const std::optional<u64> id = parse_u64(text);
    return id && *id != 0 ? id : std::nullopt;
}

sf::NetworkingIdentity identity(u64 steam_id) {
    sf::NetworkingIdentity out{};
    out.type = sf::identity_steam_id;
    out.size = sizeof(u64);
    out.steam_id = steam_id;
    return out;
}

u64 steam_id_of(const sf::NetworkingIdentity& identity) {
    return identity.type == sf::identity_steam_id ? identity.steam_id : 0;
}

// Where Steam started us to join: "+connect_lobby <id>" or "+connect <text>".
std::string join_from_args(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "+connect_lobby") {
            return "lobby:" + args[i + 1];
        }
        if (args[i] == "+connect") {
            return args[i + 1];
        }
    }
    return {};
}

class SteamServices final : public PlatformServices {
public:
    SteamServices(std::unique_ptr<SteamApi> api, const ServicesConfig& config)
        : _api(std::move(api)), _app_id(config.steam_app_id) {
        _pipe = _api->get_pipe();
        _user = _api->interface("SteamUser", sf::user_versions);
        _friends = _api->interface("SteamFriends", sf::friends_versions);
        _utils = _api->interface("SteamUtils", sf::utils_versions);
        _apps = _api->interface("SteamApps", sf::apps_versions);
        _stats = _api->interface("SteamUserStats", sf::stats_versions);
        _storage = _api->interface("SteamRemoteStorage", sf::storage_versions);
        if (_api->missing_network.empty()) {
            _matchmaking = _api->interface("SteamMatchmaking", sf::matchmaking_versions);
            _messages = _api->interface("SteamNetworkingMessages_SteamAPI", sf::messages_versions);
            _net_utils = _api->interface("SteamNetworkingUtils_SteamAPI", sf::networking_utils_versions);
        } else {
            KIN_LOG_WARN_F("services", "steam api library lacks lobby and message functions: no lobbies",
                           (LogFields{{.name = "missing", .value = _api->missing_network}}));
        }
        if (_utils) {
            _app_id = _api->utils_app_id(_utils);
        }
        // Since SDK 1.61 the player's stats arrive by themselves; before, ask.
        if (_stats && _api->stats_request_current) {
            _stats_ready = false;
            _api->stats_request_current(_stats);
        }
        _join_on_launch = launch_join(config);
    }

    ~SteamServices() override {
        if (_lobby != 0 && _matchmaking) {
            _api->mm_leave(_matchmaking, _lobby);
        }
        flush();
        _api->shutdown();
    }

    bool complete() const { return _user && _friends && _utils && _apps && _stats; }

    std::string_view backend() const override { return "steam"; }
    bool online() const override { return _connected && _user && _api->user_logged_on(_user); }

    void update() override {
        _events.clear();
        _events.swap(_pending);
        dispatch();
        accept_held_requests();
        if (!_stats_ready && _stats_retry_at > 0.0 && now_seconds() >= _stats_retry_at) {
            _stats_retry_at = 0.0;
            _api->stats_request_current(_stats);
        }
        if (_dirty && _stats_ready && now_seconds() - _last_store >= store_interval_seconds) {
            flush();
        }
    }
    std::span<const ServiceEvent> events() const override { return _events; }

    void flush() override {
        if (!_dirty || !_stats_ready) {
            return;
        }
        _dirty = false;
        _last_store = now_seconds();
        if (!_api->stats_store(_stats)) {
            _pending.push_back({.kind = ServiceEventKind::StatsFailed, .text = "StoreStats refused"});
        }
    }

    ServicePlayer player() const override {
        ServicePlayer p;
        if (_user) {
            p.id = steam_player_id(_api->user_steam_id(_user));
        }
        if (const char* name = _friends ? _api->friends_persona_name(_friends) : nullptr) {
            p.name = name;
        }
        return p;
    }
    std::string language() const override {
        const char* language = _apps ? _api->apps_language(_apps) : nullptr;
        return language ? language_from_steam(language) : std::string{};
    }
    // A Steam Deck, or a handheld Steam tunes like one (a Legion Go S).
    bool handheld() const override {
        if (_api->utils_hardware_config) {
            return _api->utils_hardware_config(_utils) == sf::hardware_config_steam_deck;
        }
        if (_api->utils_hardware_type) {
            return _api->utils_hardware_type(_utils) == sf::hardware_steam_deck;
        }
        return _api->utils_on_deck && _api->utils_on_deck(_utils);
    }
    bool big_picture() const override { return _api->utils_big_picture && _api->utils_big_picture(_utils); }
    bool cloud_saves() const override {
        return _storage && _api->storage_cloud_account && _api->storage_cloud_app &&
               _api->storage_cloud_account(_storage) && _api->storage_cloud_app(_storage);
    }

    bool unlock(std::string_view achievement) override { return achievement_op(Op::Unlock, achievement); }
    bool clear(std::string_view achievement) override { return achievement_op(Op::Clear, achievement); }
    std::optional<bool> unlocked(std::string_view achievement) const override {
        bool done = false;
        if (!_stats_ready || !_api->stats_get_achievement(_stats, std::string{achievement}.c_str(), &done)) {
            return std::nullopt;
        }
        return done;
    }
    bool show_progress(std::string_view achievement, u32 current, u32 max) override {
        return _stats_ready && _api->stats_indicate_progress(_stats, std::string{achievement}.c_str(), current, max);
    }

    bool set_stat(std::string_view stat, i32 value) override {
        if (!_stats_ready) {
            _waiting.push_back({.op = Op::SetInt, .id = std::string{stat}, .int_value = value});
            return true;
        }
        const bool ok = _api->stats_set_int(_stats, std::string{stat}.c_str(), value);
        _dirty |= ok;
        return ok;
    }
    bool set_stat(std::string_view stat, f32 value) override {
        if (!_stats_ready) {
            _waiting.push_back({.op = Op::SetFloat, .id = std::string{stat}, .float_value = value});
            return true;
        }
        const bool ok = _api->stats_set_float(_stats, std::string{stat}.c_str(), value);
        _dirty |= ok;
        return ok;
    }
    std::optional<i32> stat_int(std::string_view stat) const override {
        i32 value = 0;
        if (!_stats_ready || !_api->stats_get_int(_stats, std::string{stat}.c_str(), &value)) {
            return std::nullopt;
        }
        return value;
    }
    std::optional<f32> stat_float(std::string_view stat) const override {
        f32 value = 0.0f;
        if (!_stats_ready || !_api->stats_get_float(_stats, std::string{stat}.c_str(), &value)) {
            return std::nullopt;
        }
        return value;
    }

    u32 upload_score(std::string_view board, i32 score, std::span<const i32> details, LeaderboardUpload upload,
                     LeaderboardSort sort, LeaderboardDisplay display) override {
        BoardOp op{.request = _next_request++,
                   .upload = true,
                   .score = score,
                   .details = {details.begin(), details.end()},
                   .replace = upload == LeaderboardUpload::Replace};
        return board_op(board, std::move(op), sort, display);
    }
    u32 request_scores(std::string_view board, LeaderboardRange range, i32 first, i32 last) override {
        BoardOp op{.request = _next_request++, .upload = false, .range = range, .first = first, .last = last};
        return board_op(board, std::move(op), LeaderboardSort::Descending, LeaderboardDisplay::Number);
    }

    bool set_presence(std::string_view key, std::string_view value) override {
        return _api->friends_set_rich_presence(_friends, std::string{key}.c_str(), std::string{value}.c_str());
    }
    void clear_presence() override { _api->friends_clear_rich_presence(_friends); }

    bool overlay_active() const override { return _overlay; }
    bool open_overlay(OverlayPage page) override {
        static constexpr const char* dialogs[] = {"friends",  "community",    "players",
                                                  "settings", "achievements", "stats"};
        _api->friends_overlay(_friends, dialogs[static_cast<std::size_t>(page)]);
        return true;
    }
    bool open_url(std::string_view url) override {
        _api->friends_overlay_web(_friends, std::string{url}.c_str(), 0);
        return true;
    }
    bool open_store(std::string_view dlc) override {
        u32 app = _app_id;
        if (!dlc.empty()) {
            const std::optional<u64> id = parse_u64(dlc);
            if (!id || *id > 0xffffffffu) {
                return false;
            }
            app = static_cast<u32>(*id);
        }
        _api->friends_overlay_store(_friends, app, 0);
        return true;
    }
    bool invite_friends(std::string_view lobby) override {
        const std::optional<u64> id = lobby_id(lobby);
        if (!id) {
            return false;
        }
        _api->friends_overlay_invite(_friends, *id);
        return true;
    }

    bool show_keyboard(KeyboardMode mode, Rectf field) override {
        if (!_api->utils_show_floating) {
            return false;
        }
        return _api->utils_show_floating(_utils, static_cast<i32>(mode), static_cast<int>(field.x),
                                         static_cast<int>(field.y), static_cast<int>(field.w),
                                         static_cast<int>(field.h));
    }
    void hide_keyboard() override {
        if (_api->utils_dismiss_floating) {
            _api->utils_dismiss_floating(_utils);
        }
    }
    bool ask_text(std::string_view prompt, std::string_view initial, u32 max_chars, bool password) override {
        return _api->utils_show_text(_utils, password ? 1 : 0, 0, std::string{prompt}.c_str(),
                                     max_chars == 0 ? 256 : max_chars, std::string{initial}.c_str());
    }

    bool owns(std::string_view dlc) const override {
        const std::optional<u64> id = parse_u64(dlc);
        if (!id || *id > 0xffffffffu) {
            return false;
        }
        const u32 app = static_cast<u32>(*id);
        return _api->apps_subscribed_app(_apps, app) || _api->apps_dlc_installed(_apps, app);
    }
    std::string join_on_launch() const override { return _join_on_launch; }

    // Lobbies.

    u32 create_lobby(const LobbyOptions& options) override {
        if (!_matchmaking) {
            return 0;
        }
        start_relays();
        leave_lobby();
        const sf::SteamAPICall call = _api->mm_create(_matchmaking, static_cast<i32>(options.visibility),
                                                      static_cast<int>(std::clamp<u32>(options.max_members, 1, 250)));
        if (call == 0) {
            return 0;
        }
        const u32 request = _next_request++;
        _calls[call] = {.kind = Call::Kind::LobbyCreate, .request = request};
        _lobby_options[request] = options.data;
        _lobby_request = request;
        return request;
    }
    u32 join_lobby(std::string_view lobby) override {
        const std::optional<u64> id = steam_id_of(lobby, "lobby:");
        if (!_matchmaking || !id) {
            return 0;
        }
        start_relays();
        if (*id == _lobby && _lobby_request == 0) {
            const u32 request = _next_request++;
            _pending.push_back(
                {.kind = ServiceEventKind::LobbyEntered, .id = steam_lobby_id(_lobby), .request = request, .ok = true});
            return request;
        }
        leave_lobby();
        const sf::SteamAPICall call = _api->mm_join(_matchmaking, *id);
        if (call == 0) {
            return 0;
        }
        const u32 request = _next_request++;
        _calls[call] = {.kind = Call::Kind::LobbyJoin, .request = request};
        _lobby_request = request;
        _lobby_joining = *id;
        return request;
    }
    // Also cancels a create_lobby or join_lobby still on its way: its lobby is left when the answer comes.
    void leave_lobby() override {
        if (_lobby != 0 && _matchmaking) {
            _api->mm_leave(_matchmaking, _lobby);
        }
        _lobby = 0;
        _lobby_request = 0;
        _lobby_joining = 0;
    }
    u32 find_lobbies(const LobbyFilter& filter) override {
        if (!_matchmaking) {
            return 0;
        }
        for (const LobbyFilter::Rule& rule : filter.rules) {
            if (rule.numeric) {
                _api->mm_filter_number(_matchmaking, rule.key.c_str(), std::atoi(rule.value.c_str()),
                                       static_cast<i32>(rule.compare));
            } else {
                _api->mm_filter_string(_matchmaking, rule.key.c_str(), rule.value.c_str(),
                                       static_cast<i32>(rule.compare));
            }
        }
        if (filter.open_slots > 0) {
            _api->mm_filter_slots(_matchmaking, static_cast<int>(filter.open_slots));
        }
        _api->mm_filter_distance(_matchmaking, static_cast<i32>(filter.distance));
        _api->mm_filter_count(_matchmaking, static_cast<int>(std::max<u32>(filter.max_results, 1)));
        // Steam keeps one search: a new one ends the last, which is answered as failed here.
        if (const auto last = _calls.find(_list_call); last != _calls.end()) {
            _pending.push_back({.kind = ServiceEventKind::LobbyList,
                                .text = "replaced by a newer search",
                                .request = last->second.request});
            _calls.erase(last);
        }
        const sf::SteamAPICall call = _api->mm_request_list(_matchmaking);
        if (call == 0) {
            return 0;
        }
        const u32 request = _next_request++;
        _calls[call] = {.kind = Call::Kind::LobbyList, .request = request};
        _list_call = call;
        return request;
    }
    std::string lobby() const override { return _lobby != 0 ? steam_lobby_id(_lobby) : std::string{}; }
    std::vector<LobbyMember> lobby_members() const override {
        std::vector<LobbyMember> out;
        if (_lobby == 0) {
            return out;
        }
        const int count = _api->mm_member_count(_matchmaking, _lobby);
        for (int i = 0; i < count; ++i) {
            const u64 member = _api->mm_member_by_index(_matchmaking, _lobby, i);
            out.push_back({.id = steam_player_id(member), .name = player_name(member)});
        }
        return out;
    }
    std::string lobby_owner() const override {
        const u64 owner = _lobby != 0 ? _api->mm_owner(_matchmaking, _lobby) : 0;
        return owner != 0 ? steam_player_id(owner) : std::string{};
    }
    bool set_lobby_data(std::string_view key, std::string_view value) override {
        return _lobby != 0 && !key.empty() &&
               _api->mm_set_data(_matchmaking, _lobby, std::string{key}.c_str(), std::string{value}.c_str());
    }
    std::string lobby_data(std::string_view key) const override {
        const char* value = _lobby != 0 ? _api->mm_get_data(_matchmaking, _lobby, std::string{key}.c_str()) : nullptr;
        return value ? value : "";
    }
    bool set_member_data(std::string_view key, std::string_view value) override {
        if (_lobby == 0 || key.empty()) {
            return false;
        }
        _api->mm_set_member_data(_matchmaking, _lobby, std::string{key}.c_str(), std::string{value}.c_str());
        return true;
    }
    std::string member_data(std::string_view member, std::string_view key) const override {
        const std::optional<u64> id = steam_id_of(member, "steam:");
        const char* value =
            _lobby != 0 && id ? _api->mm_get_member_data(_matchmaking, _lobby, *id, std::string{key}.c_str()) : nullptr;
        return value ? value : "";
    }
    bool set_lobby_joinable(bool joinable) override {
        return _lobby != 0 && _api->mm_set_joinable(_matchmaking, _lobby, joinable);
    }
    bool send_lobby_message(std::string_view text) override {
        // Steam counts the terminating zero in a chat message.
        const std::string line{text};
        return _lobby != 0 && !line.empty() &&
               _api->mm_send_chat(_matchmaking, _lobby, line.c_str(), static_cast<int>(line.size() + 1));
    }

    // Messages between players (ISteamNetworkingMessages: across NATs, relayed where needed).

    bool send_message(std::string_view player, std::span<const u8> bytes, MessageDelivery delivery,
                      u32 channel) override {
        const std::optional<u64> id = steam_id_of(player, "steam:");
        if (!_messages || !id || bytes.empty() ||
            (delivery == MessageDelivery::Unreliable && bytes.size() > max_message())) {
            return false;
        }
        start_relays();
        // Unreliable messages still open a session first (no NoDelay), and a broken session starts again.
        const int flags = (delivery == MessageDelivery::Reliable ? sf::send_reliable : sf::send_unreliable) |
                          sf::send_no_nagle | sf::send_auto_restart;
        const sf::NetworkingIdentity to = identity(*id);
        return _api->msg_send(_messages, to, bytes.data(), static_cast<u32>(bytes.size()), flags,
                              static_cast<int>(channel)) == sf::result_ok;
    }
    void receive_messages(u32 channel, std::vector<PlayerMessage>& out) override {
        if (!_messages) {
            return;
        }
        constexpr int batch = 64;
        sf::NetworkingMessage* messages[batch] = {};
        for (;;) {
            const int count = _api->msg_receive(_messages, static_cast<int>(channel), messages, batch);
            for (int i = 0; i < count; ++i) {
                sf::NetworkingMessage* m = messages[i];
                const u64 from = steam_id_of(m->peer);
                if (from != 0 && m->size > 0) {
                    const auto* data = static_cast<const u8*>(m->data);
                    out.push_back({.from = steam_player_id(from), .bytes = {data, data + m->size}});
                }
                _api->msg_release(m);
            }
            if (count < batch) {
                break;
            }
        }
    }
    void close_messages(std::string_view player) override {
        if (const std::optional<u64> id = steam_id_of(player, "steam:"); id && _messages) {
            _api->msg_close(_messages, identity(*id));
        }
    }
    std::size_t max_message() const override { return _messages ? 1200 : 0; }

private:
    // Relays are found once, on first use: the first connection is quicker for it.
    void start_relays() {
        if (!_relays_started && _net_utils) {
            _relays_started = true;
            _api->net_init_relay(_net_utils);
        }
    }

    bool lobby_member(u64 player) const {
        if (_lobby == 0) {
            return false;
        }
        const int count = _api->mm_member_count(_matchmaking, _lobby);
        for (int i = 0; i < count; ++i) {
            if (_api->mm_member_by_index(_matchmaking, _lobby, i) == player) {
                return true;
            }
        }
        return false;
    }

    // The answer to a create_lobby or join_lobby that put the player in `lobby`: entered, or left again at once if
    // the request was cancelled (left, or replaced by a newer one) on the way, so no ghost member stays behind.
    bool entered(u32 request, u64 lobby) {
        if (request != _lobby_request) {
            if (lobby != _lobby && lobby != _lobby_joining) {
                _api->mm_leave(_matchmaking, lobby);
            }
            _pending.push_back({.kind = ServiceEventKind::LobbyFailed, .text = "cancelled", .request = request});
            return false;
        }
        _lobby_request = 0;
        _lobby_joining = 0;
        _lobby = lobby;
        return true;
    }
    // A player's name, or "" while Steam does not have it (it says "[unknown]").
    std::string player_name(u64 player) const {
        const char* name = _api->friends_friend_name(_friends, player);
        return name && std::string_view{name} != "[unknown]" ? name : "";
    }
    void accept_held_requests() {
        if (_held_requests.empty()) {
            return;
        }
        const double now = now_seconds();
        std::erase_if(_held_requests, [&](const Held& held) {
            if (lobby_member(held.from)) {
                _api->msg_accept(_messages, held.identity);
                return true;
            }
            return now >= held.until;
        });
    }
    void lobby_lost() {
        const u64 lost = _lobby;
        _lobby = 0;
        _pending.push_back({.kind = ServiceEventKind::LobbyLeft, .id = steam_lobby_id(lost)});
    }

    std::vector<LobbyInfo> lobby_list(u32 count) const {
        std::vector<LobbyInfo> out;
        for (u32 i = 0; i < count; ++i) {
            const u64 lobby = _api->mm_lobby_by_index(_matchmaking, static_cast<int>(i));
            if (lobby == 0) {
                continue;
            }
            LobbyInfo info{.id = steam_lobby_id(lobby),
                           .members = static_cast<u32>(std::max(_api->mm_member_count(_matchmaking, lobby), 0)),
                           .max_members = static_cast<u32>(std::max(_api->mm_member_limit(_matchmaking, lobby), 0))};
            const int entries = _api->mm_data_count(_matchmaking, lobby);
            for (int e = 0; e < entries; ++e) {
                char key[256] = {};
                std::vector<char> value(8192);
                if (_api->mm_data_by_index(_matchmaking, lobby, e, key, static_cast<int>(sizeof key), value.data(),
                                           static_cast<int>(value.size()))) {
                    info.data.emplace_back(key, value.data());
                }
            }
            out.push_back(std::move(info));
        }
        return out;
    }

private:
    enum class Op : u8 { Unlock, Clear, SetInt, SetFloat };
    struct Waiting {
        Op op{};
        std::string id;
        i32 int_value = 0;
        f32 float_value = 0.0f;
    };
    struct BoardOp {
        u32 request = 0;
        bool upload = false;
        i32 score = 0;
        std::vector<i32> details{};
        bool replace = false;
        LeaderboardRange range = LeaderboardRange::Top;
        i32 first = 1;
        i32 last = 10;
    };
    struct Board {
        u64 handle = 0;
        bool finding = false;
        std::vector<BoardOp> queued;
    };
    struct Call {
        enum class Kind : u8 { Find, Upload, Download, LobbyCreate, LobbyJoin, LobbyList } kind{};
        std::string board;
        u32 request = 0;
    };

    static std::optional<u64> lobby_id(std::string_view lobby) {
        if (lobby.starts_with("lobby:")) {
            lobby.remove_prefix(6);
        }
        return parse_u64(lobby);
    }

    std::string launch_join(const ServicesConfig& config) const {
        // Steam starts a game to join a friend with "+connect_lobby <id>", or
        // the friend's "connect" presence as arguments.
        if (std::string join = join_from_args(config.args); !join.empty()) {
            return join;
        }
        char line[1024] = {};
        if (_apps && _api->apps_launch_command_line(_apps, line, static_cast<int>(sizeof line)) > 0) {
            return line;
        }
        return {};
    }

    bool achievement_op(Op op, std::string_view achievement) {
        if (achievement.empty()) {
            return false;
        }
        if (!_stats_ready) {
            _waiting.push_back({.op = op, .id = std::string{achievement}});
            return true;
        }
        const std::string id{achievement};
        const bool ok = op == Op::Unlock ? _api->stats_set_achievement(_stats, id.c_str())
                                         : _api->stats_clear_achievement(_stats, id.c_str());
        _dirty |= ok;
        return ok;
    }

    u32 board_op(std::string_view name, BoardOp op, LeaderboardSort sort, LeaderboardDisplay display) {
        if (name.empty()) {
            return 0;
        }
        const u32 request = op.request;
        Board& board = _boards[std::string{name}];
        if (board.handle != 0) {
            run_board_op(std::string{name}, board.handle, op);
            return request;
        }
        board.queued.push_back(std::move(op));
        if (!board.finding) {
            board.finding = true;
            const i32 sort_method = sort == LeaderboardSort::Ascending ? sf::sort_ascending : sf::sort_descending;
            const i32 display_type = display == LeaderboardDisplay::Seconds        ? sf::display_seconds
                                     : display == LeaderboardDisplay::Milliseconds ? sf::display_milliseconds
                                                                                   : sf::display_numeric;
            const sf::SteamAPICall call =
                _api->stats_find_or_create_board(_stats, std::string{name}.c_str(), sort_method, display_type);
            if (call == 0) {
                fail_board(std::string{name}, "FindOrCreateLeaderboard refused");
            } else {
                _calls[call] = {.kind = Call::Kind::Find, .board = std::string{name}};
            }
        }
        return request;
    }

    void run_board_op(const std::string& name, u64 handle, const BoardOp& op) {
        sf::SteamAPICall call = 0;
        if (op.upload) {
            call = _api->stats_upload_score(_stats, handle, op.replace ? sf::upload_force_update : sf::upload_keep_best,
                                            op.score, op.details.data(), static_cast<int>(op.details.size()));
        } else {
            const i32 request = op.range == LeaderboardRange::AroundPlayer ? sf::request_around_user
                                : op.range == LeaderboardRange::Friends    ? sf::request_friends
                                                                           : sf::request_global;
            call = _api->stats_download_entries(_stats, handle, request, op.first, op.last);
        }
        if (call == 0) {
            _pending.push_back({.kind = ServiceEventKind::LeaderboardFailed,
                                .id = name,
                                .text = "request refused",
                                .request = op.request});
            return;
        }
        _calls[call] = {
            .kind = op.upload ? Call::Kind::Upload : Call::Kind::Download, .board = name, .request = op.request};
    }

    void fail_board(const std::string& name, std::string why) {
        Board& board = _boards[name];
        for (const BoardOp& op : board.queued) {
            _pending.push_back(
                {.kind = ServiceEventKind::LeaderboardFailed, .id = name, .text = why, .request = op.request});
        }
        board.queued.clear();
        board.finding = false;
    }

    void dispatch() {
        _api->dispatch_run_frame(_pipe);
        // Room for a newer, larger CallbackMsg_t than kin declares.
        struct {
            sf::CallbackMsg msg;
            unsigned char spare[32];
        } holder{};
        while (_api->dispatch_next(_pipe, &holder.msg)) {
            const sf::CallbackMsg& msg = holder.msg;
            if (msg.callback == sf::cb_call_completed) {
                const auto done = payload<sf::SteamAPICallCompleted>(msg.param, msg.param_size);
                std::vector<u8> result(std::max<u32>(done.param_size, 1));
                bool failed = false;
                const bool got = _api->dispatch_call_result(_pipe, done.call, result.data(),
                                                            static_cast<int>(done.param_size), done.callback, &failed);
                on_call_result(done.call, done.callback, got && !failed, result.data(),
                               static_cast<int>(done.param_size));
            } else {
                on_callback(msg.callback, msg.param, msg.param_size);
            }
            _api->dispatch_free(_pipe);
        }
        _events.insert(_events.end(), std::make_move_iterator(_pending.begin()),
                       std::make_move_iterator(_pending.end()));
        _pending.clear();
    }

    void on_callback(int id, const u8* data, int size) {
        switch (id) {
        case sf::cb_overlay_activated: {
            const bool shown = payload<sf::GameOverlayActivated>(data, size).active != 0;
            if (shown != _overlay) {
                _overlay = shown;
                _pending.push_back({.kind = shown ? ServiceEventKind::OverlayShown : ServiceEventKind::OverlayHidden});
            }
            break;
        }
        case sf::cb_persona_state_change: {
            const auto change = payload<sf::PersonaStateChange>(data, size);
            if ((change.change_flags & sf::persona_change_name) != 0 && _naming.erase(change.steam_id) > 0 &&
                lobby_member(change.steam_id)) {
                _pending.push_back({.kind = ServiceEventKind::LobbyMemberNamed,
                                    .id = steam_player_id(change.steam_id),
                                    .text = player_name(change.steam_id)});
            }
            break;
        }
        case sf::cb_lobby_join_requested: {
            const auto join = payload<sf::GameLobbyJoinRequested>(data, size);
            _pending.push_back({.kind = ServiceEventKind::JoinRequested,
                                .id = steam_player_id(join.friend_id),
                                .text = "lobby:" + std::to_string(join.lobby)});
            break;
        }
        case sf::cb_rich_presence_join_requested: {
            auto join = payload<sf::GameRichPresenceJoinRequested>(data, size);
            join.connect[sizeof join.connect - 1] = '\0';
            _pending.push_back(
                {.kind = ServiceEventKind::JoinRequested, .id = steam_player_id(join.friend_id), .text = join.connect});
            break;
        }
        case sf::cb_text_input_dismissed: {
            const auto dismissed = payload<sf::GamepadTextInputDismissed>(data, size);
            if (!dismissed.submitted) {
                _pending.push_back({.kind = ServiceEventKind::TextCancelled});
                break;
            }
            const u32 length = _api->utils_text_length(_utils);
            std::string text(length + 1, '\0');
            if (_api->utils_text(_utils, text.data(), length + 1)) {
                text.resize(std::strlen(text.c_str()));
                _pending.push_back({.kind = ServiceEventKind::TextEntered, .text = std::move(text), .ok = true});
            } else {
                _pending.push_back({.kind = ServiceEventKind::TextCancelled});
            }
            break;
        }
        case sf::cb_floating_text_input_dismissed:
            _pending.push_back({.kind = ServiceEventKind::KeyboardClosed});
            break;
        case sf::cb_dlc_installed:
            _pending.push_back({.kind = ServiceEventKind::DlcInstalled,
                                .id = std::to_string(payload<sf::DlcInstalled>(data, size).app_id)});
            break;
        case sf::cb_servers_connected:
            if (!_connected) {
                _connected = true;
                _pending.push_back({.kind = ServiceEventKind::Reconnected});
                // Steam tells no one who was dropped from a lobby while away (LobbyKicked_t is unused): look.
                if (_lobby != 0 && _user && !lobby_member(_api->user_steam_id(_user))) {
                    lobby_lost();
                }
            }
            break;
        case sf::cb_servers_disconnected:
        case sf::cb_steam_shutdown:
            if (_connected) {
                _connected = false;
                _pending.push_back({.kind = ServiceEventKind::Disconnected});
            }
            break;
        case sf::cb_stats_received: {
            const auto received = payload<sf::UserStatsReceived>(data, size);
            if (received.game_id == _app_id && !_stats_ready) {
                if (received.result == sf::result_ok) {
                    _stats_ready = true;
                    replay_waiting();
                } else {
                    // Not (yet) reachable, or not set up on the partner site: ask again later.
                    _stats_retry_at = now_seconds() + 10.0;
                    KIN_LOG_WARN_F("services", "steam stats not received",
                                   (LogFields{{.name = "result", .value = std::to_string(received.result)}}));
                }
            }
            break;
        }
        case sf::cb_stats_stored: {
            const auto stored = payload<sf::UserStatsStored>(data, size);
            if (stored.game_id == _app_id) {
                const bool ok = stored.result == sf::result_ok;
                _pending.push_back({.kind = ok ? ServiceEventKind::StatsStored : ServiceEventKind::StatsFailed,
                                    .text = ok ? std::string{} : "EResult " + std::to_string(stored.result),
                                    .ok = ok});
            }
            break;
        }
        case sf::cb_achievement_stored: {
            auto stored = payload<sf::UserAchievementStored>(data, size);
            stored.name[sizeof stored.name - 1] = '\0';
            if (stored.game_id == _app_id && stored.max_progress == 0) {
                _pending.push_back({.kind = ServiceEventKind::AchievementUnlocked, .id = stored.name, .ok = true});
            }
            break;
        }
        // LobbyEnter_t also comes as a callback, before or after the call result: the call results alone decide.
        case sf::cb_lobby_data_update: {
            const auto update = payload<sf::LobbyDataUpdate>(data, size);
            if (update.lobby == _lobby && _lobby != 0 && update.success) {
                _pending.push_back(
                    {.kind = ServiceEventKind::LobbyDataChanged,
                     .id = update.member == update.lobby ? std::string{} : steam_player_id(update.member)});
            }
            break;
        }
        case sf::cb_lobby_chat_update: {
            const auto update = payload<sf::LobbyChatUpdate>(data, size);
            if (update.lobby != _lobby || _lobby == 0) {
                break;
            }
            if (update.state_change & sf::member_entered) {
                // Steam may not know a stranger's name yet: asked for, it comes as LobbyMemberNamed.
                if (_api->friends_request_info(_friends, update.changed, true)) {
                    _naming.insert(update.changed);
                }
                _pending.push_back({.kind = ServiceEventKind::LobbyMemberJoined,
                                    .id = steam_player_id(update.changed),
                                    .text = player_name(update.changed)});
            } else if (update.state_change & sf::member_gone) {
                if (_user && update.changed == _api->user_steam_id(_user)) {
                    lobby_lost(); // this player was removed
                } else {
                    _pending.push_back(
                        {.kind = ServiceEventKind::LobbyMemberLeft, .id = steam_player_id(update.changed)});
                }
            }
            break;
        }
        case sf::cb_lobby_chat_msg: {
            const auto chat = payload<sf::LobbyChatMsg>(data, size);
            if (chat.lobby != _lobby || _lobby == 0 || chat.type != sf::chat_message) {
                break;
            }
            std::vector<char> text(4097);
            sf::SteamId user{};
            i32 type = 0;
            const int length = _api->mm_chat_entry(_matchmaking, _lobby, static_cast<int>(chat.chat_id), &user,
                                                   text.data(), static_cast<int>(text.size() - 1), &type);
            if (length > 0 && type == sf::chat_message) {
                text[static_cast<std::size_t>(length)] = '\0';
                _pending.push_back(
                    {.kind = ServiceEventKind::LobbyMessage, .id = steam_player_id(user.id), .text = text.data()});
            }
            break;
        }
        case sf::cb_lobby_kicked: {
            const auto kicked = payload<sf::LobbyKicked>(data, size);
            if (kicked.lobby == _lobby && _lobby != 0) {
                lobby_lost();
            }
            break;
        }
        case sf::cb_messages_session_request: {
            // Players the game sent to first are let in by Steam itself; here, the lobby's members. A joiner's
            // first message can come before Steam tells us they joined: held until then (or for a few seconds).
            const auto request = payload<sf::MessagesSessionRequest>(data, size);
            if (const u64 from = steam_id_of(request.remote); from != 0) {
                if (lobby_member(from)) {
                    _api->msg_accept(_messages, request.remote);
                } else if (std::ranges::none_of(_held_requests, [&](const Held& h) { return h.from == from; })) {
                    _held_requests.push_back(
                        {.from = from, .identity = request.remote, .until = now_seconds() + held_request_seconds});
                }
            }
            break;
        }
        case sf::cb_messages_session_failed: {
            const auto failed = payload<sf::MessagesSessionFailed>(data, size);
            if (const u64 with = steam_id_of(failed.remote); with != 0) {
                _pending.push_back({.kind = ServiceEventKind::MessagesFailed,
                                    .id = steam_player_id(with),
                                    .text = "end reason " + std::to_string(failed.end_reason)});
            }
            break;
        }
        default:
            break;
        }
    }

    void on_call_result(sf::SteamAPICall handle, int id, bool ok, const u8* data, int size) {
        const auto found = _calls.find(handle);
        if (found == _calls.end()) {
            return;
        }
        const Call call = std::move(found->second);
        _calls.erase(found);
        if (call.kind == Call::Kind::LobbyCreate) {
            const auto created = payload<sf::LobbyCreated>(data, size);
            std::vector<std::pair<std::string, std::string>> initial = std::move(_lobby_options[call.request]);
            _lobby_options.erase(call.request);
            if (!ok || id != sf::cb_lobby_created || created.result != sf::result_ok || created.lobby == 0) {
                if (call.request == _lobby_request) {
                    _lobby_request = 0;
                }
                _pending.push_back({.kind = ServiceEventKind::LobbyFailed,
                                    .text = "CreateLobby failed: EResult " + std::to_string(created.result),
                                    .request = call.request});
                return;
            }
            if (!entered(call.request, created.lobby)) {
                return;
            }
            for (const auto& [key, value] : initial) { // set before anyone can find it
                _api->mm_set_data(_matchmaking, _lobby, key.c_str(), value.c_str());
            }
            _pending.push_back({.kind = ServiceEventKind::LobbyEntered,
                                .id = steam_lobby_id(_lobby),
                                .request = call.request,
                                .ok = true});
            return;
        }
        if (call.kind == Call::Kind::LobbyJoin) {
            const auto enter = payload<sf::LobbyEnter>(data, size);
            if (!ok || id != sf::cb_lobby_enter || enter.response != sf::enter_success) {
                static constexpr const char* reasons[] = {"",
                                                          "",
                                                          "no such lobby",
                                                          "not allowed",
                                                          "the lobby is full",
                                                          "error",
                                                          "banned",
                                                          "limited",
                                                          "clan disabled",
                                                          "community ban",
                                                          "blocked by a member",
                                                          "blocked a member",
                                                          "",
                                                          "",
                                                          "",
                                                          "too many attempts: try again shortly"};
                const u32 r = enter.response;
                const std::string why = ok && r < std::size(reasons) && reasons[r][0] ? reasons[r] : "JoinLobby failed";
                if (call.request == _lobby_request) {
                    _lobby_request = 0;
                    _lobby_joining = 0;
                }
                _pending.push_back({.kind = ServiceEventKind::LobbyFailed, .text = why, .request = call.request});
                return;
            }
            if (entered(call.request, enter.lobby)) {
                _pending.push_back({.kind = ServiceEventKind::LobbyEntered,
                                    .id = steam_lobby_id(_lobby),
                                    .request = call.request,
                                    .ok = true});
            }
            return;
        }
        if (call.kind == Call::Kind::LobbyList) {
            _list_call = 0;
            const auto list = payload<sf::LobbyMatchList>(data, size);
            if (!ok || id != sf::cb_lobby_match_list) {
                _pending.push_back(
                    {.kind = ServiceEventKind::LobbyList, .text = "RequestLobbyList failed", .request = call.request});
                return;
            }
            _pending.push_back({.kind = ServiceEventKind::LobbyList,
                                .request = call.request,
                                .ok = true,
                                .lobbies = lobby_list(list.count)});
            return;
        }
        if (call.kind == Call::Kind::Find) {
            const auto result = payload<sf::LeaderboardFindResult>(data, size);
            if (!ok || id != sf::cb_leaderboard_find_result || !result.found || result.board == 0) {
                fail_board(call.board, "no such leaderboard");
                return;
            }
            Board& board = _boards[call.board];
            board.handle = result.board;
            board.finding = false;
            const std::vector<BoardOp> queued = std::move(board.queued);
            board.queued.clear();
            for (const BoardOp& op : queued) {
                run_board_op(call.board, result.board, op);
            }
            return;
        }
        if (call.kind == Call::Kind::Upload) {
            const auto uploaded = payload<sf::LeaderboardScoreUploaded>(data, size);
            if (!ok || id != sf::cb_leaderboard_score_uploaded || !uploaded.success) {
                _pending.push_back({.kind = ServiceEventKind::LeaderboardFailed,
                                    .id = call.board,
                                    .text = "upload failed",
                                    .request = call.request});
                return;
            }
            _pending.push_back({.kind = ServiceEventKind::LeaderboardUploaded,
                                .id = call.board,
                                .request = call.request,
                                .ok = true,
                                .changed = uploaded.score_changed != 0,
                                .rank = uploaded.global_rank_new});
            return;
        }
        const auto downloaded = payload<sf::LeaderboardScoresDownloaded>(data, size);
        if (!ok || id != sf::cb_leaderboard_scores_downloaded) {
            _pending.push_back({.kind = ServiceEventKind::LeaderboardFailed,
                                .id = call.board,
                                .text = "download failed",
                                .request = call.request});
            return;
        }
        ServiceEvent event{
            .kind = ServiceEventKind::LeaderboardScores, .id = call.board, .request = call.request, .ok = true};
        constexpr int max_details = 64;
        for (int i = 0; i < downloaded.count; ++i) {
            sf::LeaderboardEntry entry{};
            i32 details[max_details] = {};
            if (!_api->stats_downloaded_entry(_stats, downloaded.entries, i, &entry, details, max_details)) {
                continue;
            }
            LeaderboardEntry out{
                .player_id = steam_player_id(entry.user), .rank = entry.global_rank, .score = entry.score};
            if (const char* name = _api->friends_friend_name(_friends, entry.user)) {
                out.player_name = name;
            }
            out.details.assign(details, details + std::clamp(entry.details, 0, max_details));
            event.entries.push_back(std::move(out));
        }
        _pending.push_back(std::move(event));
    }

    void replay_waiting() {
        std::vector<Waiting> waiting = std::move(_waiting);
        _waiting.clear();
        for (const Waiting& w : waiting) {
            switch (w.op) {
            case Op::Unlock:
            case Op::Clear:
                achievement_op(w.op, w.id);
                break;
            case Op::SetInt:
                set_stat(w.id, w.int_value);
                break;
            case Op::SetFloat:
                set_stat(w.id, w.float_value);
                break;
            }
        }
    }

    std::unique_ptr<SteamApi> _api;
    u32 _app_id = 0;
    sf::HSteamPipe _pipe = 0;
    void* _user = nullptr;
    void* _friends = nullptr;
    void* _utils = nullptr;
    void* _apps = nullptr;
    void* _stats = nullptr;
    void* _storage = nullptr;
    void* _matchmaking = nullptr;
    void* _messages = nullptr;
    void* _net_utils = nullptr;
    bool _relays_started = false;
    u64 _lobby = 0;
    u32 _lobby_request = 0; // the create_lobby or join_lobby whose answer counts (0: none)
    u64 _lobby_joining = 0; // the lobby that join_lobby is entering
    sf::SteamAPICall _list_call = 0;
    std::map<u32, std::vector<std::pair<std::string, std::string>>> _lobby_options; // request, initial data
    bool _stats_ready = true;
    double _stats_retry_at = 0.0; // when to ask for the player's stats again (0: not)
    bool _connected = true;
    bool _overlay = false;
    bool _dirty = false;
    double _last_store = 0.0;
    u32 _next_request = 1;
    std::string _join_on_launch;
    std::vector<Waiting> _waiting;
    std::map<std::string, Board> _boards;
    std::map<sf::SteamAPICall, Call> _calls;
    std::vector<ServiceEvent> _events;
    std::vector<ServiceEvent> _pending;
    // Message session requests from players not (yet) in the lobby: Steam keeps such a request a few seconds.
    struct Held {
        u64 from = 0;
        sf::NetworkingIdentity identity{};
        double until = 0.0;
    };
    static constexpr double held_request_seconds = 5.0;
    std::vector<Held> _held_requests;
    std::set<u64> _naming; // lobby members whose names Steam is fetching
};

} // namespace

std::unique_ptr<PlatformServices> start_steam(const ServicesConfig& config, ServicesStart& start) {
    start = ServicesStart::Offline;
    auto api = std::make_unique<SteamApi>();
    if (!api->open(config.steam_library)) {
        if (api->library) {
            KIN_LOG_WARN_F("services", "steam api library lacks functions kin needs",
                           (LogFields{{.name = "missing", .value = api->missing}}));
        } else {
            KIN_LOG_INFO("services", "no steam api library: playing offline");
        }
        return nullptr;
    }
    // Steam reads steam_appid.txt from the current folder only: one beside the
    // executable (kin_game's, in development builds) is passed on as
    // SteamAppId, so the game also starts from elsewhere.
    if (!SDL_getenv("SteamAppId")) {
        std::ifstream file(executable_dir() / "steam_appid.txt");
        u64 id = 0;
        if (file >> id && id != 0) {
            const std::string text = std::to_string(id);
            SDL_setenv_unsafe("SteamAppId", text.c_str(), 0);
            SDL_setenv_unsafe("SteamGameId", text.c_str(), 0);
        }
    }
    if (config.steam_restart_through_client && api->restart_app_if_necessary(config.steam_app_id)) {
        start = ServicesStart::Restarting;
        return nullptr;
    }
    sf::SteamErrMsg message{};
    i32 result = sf::init_ok;
    if (api->init_flat) {
        result = api->init_flat(&message);
    } else if (!api->init_legacy) {
        KIN_LOG_WARN("services", "steam api library has no SteamAPI_InitFlat or SteamAPI_InitSafe");
        return nullptr;
    } else if (!api->init_legacy()) {
        result = 1;
    }
    if (result != sf::init_ok) {
        message[sizeof message - 1] = '\0';
        KIN_LOG_INFO_F("services", "steam did not start: playing offline",
                       (LogFields{{.name = "result", .value = std::to_string(result)},
                                  {.name = "message", .value = std::string{message}}}));
        return nullptr;
    }
    api->dispatch_init();
    auto services = std::make_unique<SteamServices>(std::move(api), config);
    if (!services->complete()) {
        KIN_LOG_WARN("services", "steam interfaces missing (an SDK too old or too new for kin): playing offline");
        return nullptr; // the destructor shuts Steam down again
    }
    start = ServicesStart::Ready;
    return services;
}

} // namespace kin::services_detail

#endif
