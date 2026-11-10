#include "services_detail.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/platform/log.hpp>
#include <kin/platform/services.hpp>
#include <kin/platform/user_data.hpp>
#include <map>
#include <sstream>
#include <utility>

namespace kin {
namespace {

// How often changed stats are kept at most (platforms throttle stores).
constexpr f64 store_interval_seconds = 2.0;

f64 now_seconds() {
    using namespace std::chrono;
    return duration<f64>(steady_clock::now().time_since_epoch()).count();
}

bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 128;
}

// Achievements, stats and the player's own best scores, kept in the player's
// save folder: what a game played without a platform still remembers.
class OfflineServices final : public PlatformServices {
public:
    explicit OfflineServices(const ServicesConfig& config) {
        if (!config.offline_in_memory && !config.game_id.empty()) {
            _path = user_data_dir("kin") / config.game_id / "services" / "offline.json"; // beside SaveStore's saves
            load();
        }
    }
    ~OfflineServices() override { flush(); }

    std::string_view backend() const override { return "offline"; }
    bool online() const override { return false; }

    void update() override {
        _events.clear();
        _events.swap(_pending);
        if (_dirty && now_seconds() - _last_store >= store_interval_seconds) {
            flush();
        }
    }
    std::span<const ServiceEvent> events() const override { return _events; }

    void flush() override {
        if (!_dirty) {
            return;
        }
        _dirty = false;
        _last_store = now_seconds();
        if (!_path.empty() && !save()) {
            _pending.push_back({.kind = ServiceEventKind::StatsFailed, .text = "could not write " + _path.string()});
            return;
        }
        _pending.push_back({.kind = ServiceEventKind::StatsStored, .ok = true});
    }

    ServicePlayer player() const override { return {.id = "offline", .name = {}}; }
    std::string language() const override { return {}; }
    bool handheld() const override { return false; }
    bool big_picture() const override { return false; }
    bool cloud_saves() const override { return false; }

    bool unlock(std::string_view achievement) override {
        if (!valid_id(achievement)) {
            return false;
        }
        if (std::ranges::find(_unlocked, achievement) == _unlocked.end()) {
            _unlocked.emplace_back(achievement);
            _dirty = true;
            _pending.push_back(
                {.kind = ServiceEventKind::AchievementUnlocked, .id = std::string{achievement}, .ok = true});
        }
        return true;
    }
    bool clear(std::string_view achievement) override {
        if (!valid_id(achievement)) {
            return false;
        }
        if (const auto it = std::ranges::find(_unlocked, achievement); it != _unlocked.end()) {
            _unlocked.erase(it);
            _dirty = true;
        }
        return true;
    }
    std::optional<bool> unlocked(std::string_view achievement) const override {
        if (!valid_id(achievement)) {
            return std::nullopt;
        }
        return std::ranges::find(_unlocked, achievement) != _unlocked.end();
    }
    bool show_progress(std::string_view achievement, u32, u32) override { return valid_id(achievement); }

    bool set_stat(std::string_view stat, i32 value) override { return put(stat, static_cast<f64>(value)); }
    bool set_stat(std::string_view stat, f32 value) override {
        return std::isfinite(value) && put(stat, static_cast<f64>(value));
    }
    std::optional<i32> stat_int(std::string_view stat) const override {
        if (const auto it = _stats.find(std::string{stat}); it != _stats.end()) {
            // A decimal stat is not a whole-number one (as on Steam).
            if (it->second != std::floor(it->second)) {
                return std::nullopt;
            }
            return static_cast<i32>(it->second);
        }
        return valid_id(stat) ? std::optional<i32>{0} : std::nullopt;
    }
    std::optional<f32> stat_float(std::string_view stat) const override {
        if (const auto it = _stats.find(std::string{stat}); it != _stats.end()) {
            return static_cast<f32>(it->second);
        }
        return valid_id(stat) ? std::optional<f32>{0.0f} : std::nullopt;
    }

    u32 upload_score(std::string_view board, i32 score, std::span<const i32> details, LeaderboardUpload upload,
                     LeaderboardSort sort, LeaderboardDisplay) override {
        if (!valid_id(board)) {
            return 0;
        }
        const u32 request = _next_request++;
        Best& best = _boards[std::string{board}];
        const bool better = !best.set || upload == LeaderboardUpload::Replace ||
                            (sort == LeaderboardSort::Descending ? score > best.score : score < best.score);
        if (better) {
            best = {.set = true,
                    .ascending = sort == LeaderboardSort::Ascending,
                    .score = score,
                    .details = {details.begin(), details.end()}};
            _dirty = true;
        }
        _pending.push_back({.kind = ServiceEventKind::LeaderboardUploaded,
                            .id = std::string{board},
                            .request = request,
                            .ok = true,
                            .changed = better,
                            .rank = 1});
        return request;
    }
    u32 request_scores(std::string_view board, LeaderboardRange, i32, i32) override {
        if (!valid_id(board)) {
            return 0;
        }
        const u32 request = _next_request++;
        ServiceEvent event{
            .kind = ServiceEventKind::LeaderboardScores, .id = std::string{board}, .request = request, .ok = true};
        if (const auto it = _boards.find(std::string{board}); it != _boards.end() && it->second.set) {
            event.entries.push_back({.player_id = "offline",
                                     .player_name = {},
                                     .rank = 1,
                                     .score = it->second.score,
                                     .details = it->second.details});
        }
        _pending.push_back(std::move(event));
        return request;
    }

    bool set_presence(std::string_view, std::string_view) override { return false; }
    void clear_presence() override {}
    bool overlay_active() const override { return false; }
    bool open_overlay(OverlayPage) override { return false; }
    bool open_url(std::string_view) override { return false; }
    bool open_store(std::string_view) override { return false; }
    bool invite_friends(std::string_view) override { return false; }
    bool show_keyboard(KeyboardMode, Rectf) override { return false; }
    void hide_keyboard() override {}
    bool ask_text(std::string_view, std::string_view, u32, bool) override { return false; }
    bool owns(std::string_view) const override { return false; }
    std::string join_on_launch() const override { return {}; }

private:
    struct Best {
        bool set = false;
        bool ascending = false;
        i32 score = 0;
        std::vector<i32> details{};
    };

    bool put(std::string_view stat, f64 value) {
        if (!valid_id(stat)) {
            return false;
        }
        f64& slot = _stats[std::string{stat}];
        if (slot != value) {
            slot = value;
            _dirty = true;
        }
        return true;
    }

    void load() {
        std::ifstream in(_path, std::ios::binary);
        if (!in) {
            return;
        }
        std::ostringstream text;
        text << in.rdbuf();
        in.close();
        JsonParseResult parsed = parse_json(text.str());
        if (!parsed.ok() || !parsed.value->is_object()) {
            KIN_LOG_WARN_F(
                "services", "offline services file unreadable",
                (LogFields{{.name = "path", .value = _path.string()}, {.name = "error", .value = parsed.error}}));
            return;
        }
        const JsonValue& root = *parsed.value;
        if (const JsonValue* list = root.find("achievements")) {
            for (const JsonValue& id : list->items()) {
                if (id.is_string() && valid_id(id.as_string())) {
                    _unlocked.push_back(id.as_string());
                }
            }
        }
        if (const JsonValue* stats = root.find("stats")) {
            for (const auto& [name, value] : stats->members()) {
                if (value.is_number() && valid_id(name)) {
                    _stats[name] = value.as_number();
                }
            }
        }
        if (const JsonValue* boards = root.find("leaderboards")) {
            for (const auto& [name, value] : boards->members()) {
                if (!value.is_object() || !valid_id(name)) {
                    continue;
                }
                Best best{.set = true,
                          .ascending = value.bool_at("ascending"),
                          .score = static_cast<i32>(value.int_at("score"))};
                if (const JsonValue* details = value.find("details")) {
                    for (const JsonValue& d : details->items()) {
                        best.details.push_back(static_cast<i32>(d.as_int()));
                    }
                }
                _boards[name] = std::move(best);
            }
        }
    }

    bool save() const {
        std::error_code error;
        std::filesystem::create_directories(_path.parent_path(), error);
        const std::filesystem::path temporary = _path.string() + ".new";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            JsonWriter json(out);
            json.begin_object();
            json.field("schema", "kin.offline_services/1");
            json.key("achievements").begin_array();
            for (const std::string& id : _unlocked) {
                json.value(id);
            }
            json.end_array();
            json.key("stats").begin_object();
            for (const auto& [name, value] : _stats) {
                json.key(name).value(value);
            }
            json.end_object();
            json.key("leaderboards").begin_object();
            for (const auto& [name, best] : _boards) {
                if (!best.set) {
                    continue;
                }
                json.key(name).begin_object();
                json.field("score", best.score);
                json.field("ascending", best.ascending);
                json.key("details").begin_array();
                for (const i32 d : best.details) {
                    json.value(d);
                }
                json.end_array();
                json.end_object();
            }
            json.end_object();
            json.end_object();
            out << '\n';
            if (!out) {
                return false;
            }
        }
        std::filesystem::rename(temporary, _path, error);
        if (error) {
            return false;
        }
        user_data_written();
        return true;
    }

    std::filesystem::path _path;
    std::vector<std::string> _unlocked;
    std::map<std::string, f64> _stats;
    std::map<std::string, Best> _boards;
    std::vector<ServiceEvent> _events;
    std::vector<ServiceEvent> _pending;
    u32 _next_request = 1;
    bool _dirty = false;
    f64 _last_store = 0.0;
};

} // namespace

std::unique_ptr<PlatformServices> make_offline_services(const ServicesConfig& config) {
    return std::make_unique<OfflineServices>(config);
}

std::unique_ptr<PlatformServices> start_platform_services(const ServicesConfig& config, ServicesStart* start) {
    ServicesStart result = ServicesStart::Offline;
    std::unique_ptr<PlatformServices> services;
    if (!config.offline_only && config.steam_app_id != 0) {
        services = services_detail::start_steam(config, result);
    }
    if (result == ServicesStart::Restarting) {
        KIN_LOG_INFO_F("services", "the platform starts the game again",
                       (LogFields{{.name = "app_id", .value = std::to_string(config.steam_app_id)}}));
    } else if (!services) {
        result = ServicesStart::Offline;
        services = make_offline_services(config);
    }
    if (start) {
        *start = result;
    }
    if (services) {
        KIN_LOG_INFO_F("services", "platform services",
                       (LogFields{{.name = "backend", .value = std::string{services->backend()}}}));
    }
    return services;
}

std::string language_from_steam(std::string_view steam_language) {
    // Steam's API language codes (partner.steamgames.com/doc/store/localization/languages).
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 31> table{{
        {"arabic", "ar"},       {"bulgarian", "bg"},  {"schinese", "zh-Hans"}, {"tchinese", "zh-Hant"},
        {"czech", "cs"},        {"danish", "da"},     {"dutch", "nl"},         {"english", "en"},
        {"finnish", "fi"},      {"french", "fr"},     {"german", "de"},        {"greek", "el"},
        {"hungarian", "hu"},    {"indonesian", "id"}, {"italian", "it"},       {"japanese", "ja"},
        {"koreana", "ko"},      {"norwegian", "nb"},  {"polish", "pl"},        {"portuguese", "pt"},
        {"brazilian", "pt-BR"}, {"romanian", "ro"},   {"russian", "ru"},       {"spanish", "es"},
        {"latam", "es-419"},    {"swedish", "sv"},    {"thai", "th"},          {"turkish", "tr"},
        {"ukrainian", "uk"},    {"vietnamese", "vi"}, {"korean", "ko"},
    }};
    for (const auto& [steam, tag] : table) {
        if (steam == steam_language) {
            return std::string{tag};
        }
    }
    return {};
}

// FakeServices

void FakeServices::update() {
    _events.clear();
    _events.swap(_pending);
}

void FakeServices::set_overlay(bool shown) {
    if (overlay_ != shown) {
        overlay_ = shown;
        send({.kind = shown ? ServiceEventKind::OverlayShown : ServiceEventKind::OverlayHidden});
    }
}

bool FakeServices::unlock(std::string_view achievement) {
    if (!valid_id(achievement) ||
        (!achievements.empty() && std::ranges::find(achievements, achievement) == achievements.end())) {
        return false;
    }
    calls.push_back({"unlock", std::string{achievement}});
    if (std::ranges::find(_unlocked, achievement) == _unlocked.end()) {
        _unlocked.emplace_back(achievement);
        send({.kind = ServiceEventKind::AchievementUnlocked, .id = std::string{achievement}, .ok = true});
    }
    return true;
}

bool FakeServices::clear(std::string_view achievement) {
    if (!valid_id(achievement)) {
        return false;
    }
    calls.push_back({"clear", std::string{achievement}});
    std::erase(_unlocked, std::string{achievement});
    return true;
}

std::optional<bool> FakeServices::unlocked(std::string_view achievement) const {
    if (!valid_id(achievement) ||
        (!achievements.empty() && std::ranges::find(achievements, achievement) == achievements.end())) {
        return std::nullopt;
    }
    return std::ranges::find(_unlocked, achievement) != _unlocked.end();
}

bool FakeServices::show_progress(std::string_view achievement, u32 current, u32 max) {
    calls.push_back({"progress", std::string{achievement}, std::to_string(current) + "/" + std::to_string(max)});
    return valid_id(achievement);
}

bool FakeServices::set_stat(std::string_view stat, i32 value) {
    return set_stat(stat, static_cast<f32>(value));
}

bool FakeServices::set_stat(std::string_view stat, f32 value) {
    if (!valid_id(stat)) {
        return false;
    }
    std::ostringstream text;
    text << value;
    calls.push_back({"set_stat", std::string{stat}, text.str()});
    for (Stat& s : _stats) {
        if (s.id == stat) {
            s.value = value;
            return true;
        }
    }
    _stats.push_back({std::string{stat}, value});
    return true;
}

std::optional<i32> FakeServices::stat_int(std::string_view stat) const {
    if (!valid_id(stat)) {
        return std::nullopt;
    }
    for (const Stat& s : _stats) {
        if (s.id == stat) {
            if (s.value != std::floor(s.value)) {
                return std::nullopt; // a decimal stat
            }
            return static_cast<i32>(s.value);
        }
    }
    return 0;
}

std::optional<f32> FakeServices::stat_float(std::string_view stat) const {
    if (!valid_id(stat)) {
        return std::nullopt;
    }
    for (const Stat& s : _stats) {
        if (s.id == stat) {
            return static_cast<f32>(s.value);
        }
    }
    return 0.0f;
}

u32 FakeServices::upload_score(std::string_view board, i32 score, std::span<const i32>, LeaderboardUpload,
                               LeaderboardSort, LeaderboardDisplay) {
    calls.push_back({"upload_score", std::string{board}, std::to_string(score)});
    return _next_request++;
}

u32 FakeServices::request_scores(std::string_view board, LeaderboardRange, i32 first, i32 last) {
    calls.push_back({"request_scores", std::string{board}, std::to_string(first) + ".." + std::to_string(last)});
    return _next_request++;
}

bool FakeServices::set_presence(std::string_view key, std::string_view value) {
    calls.push_back({"presence", std::string{key}, std::string{value}});
    for (auto& [k, v] : presence) {
        if (k == key) {
            v = value;
            return online_;
        }
    }
    presence.emplace_back(std::string{key}, std::string{value});
    return online_;
}

void FakeServices::clear_presence() {
    calls.push_back({"clear_presence"});
    presence.clear();
}

bool FakeServices::open_overlay(OverlayPage page) {
    calls.push_back({"open_overlay", std::to_string(static_cast<int>(page))});
    return online_;
}

bool FakeServices::open_url(std::string_view url) {
    calls.push_back({"open_url", std::string{url}});
    return online_;
}

bool FakeServices::open_store(std::string_view dlc) {
    calls.push_back({"open_store", std::string{dlc}});
    return online_;
}

bool FakeServices::invite_friends(std::string_view lobby) {
    calls.push_back({"invite_friends", std::string{lobby}});
    return online_;
}

bool FakeServices::show_keyboard(KeyboardMode mode, Rectf) {
    calls.push_back({"show_keyboard", std::to_string(static_cast<int>(mode))});
    keyboard_shown = handheld_;
    return handheld_;
}

void FakeServices::hide_keyboard() {
    if (keyboard_shown) {
        calls.push_back({"hide_keyboard"});
    }
    keyboard_shown = false;
}

bool FakeServices::ask_text(std::string_view prompt, std::string_view initial, u32, bool) {
    calls.push_back({"ask_text", std::string{prompt}, std::string{initial}});
    return handheld_ || big_picture_;
}

bool FakeServices::owns(std::string_view id) const {
    return std::ranges::find(dlc, id) != dlc.end();
}

} // namespace kin
