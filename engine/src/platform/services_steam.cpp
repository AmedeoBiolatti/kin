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
#include <cstring>
#include <fstream>
#include <kin/platform/log.hpp>
#include <kin/platform/resource_paths.hpp>
#include <map>
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
    KIN_STEAM_LEGACY_FUNCTIONS(KIN_STEAM_FIELD)
#undef KIN_STEAM_FIELD
    std::string missing;

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
        enum class Kind : u8 { Find, Upload, Download } kind{};
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
