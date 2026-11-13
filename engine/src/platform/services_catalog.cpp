#include <kin/platform/services_catalog.hpp>

#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <utility>

namespace kin {
namespace {

constexpr std::string_view schema = "kin.services/1";

template <class Entry> const Entry* find_entry(const std::vector<Entry>& entries, std::string_view id) {
    const auto it = std::ranges::find(entries, id, &Entry::id);
    return it == entries.end() ? nullptr : &*it;
}

std::string_view sort_name(LeaderboardSort sort) {
    return sort == LeaderboardSort::Ascending ? "ascending" : "descending";
}

std::string_view display_name(LeaderboardDisplay display) {
    switch (display) {
    case LeaderboardDisplay::Seconds: return "seconds";
    case LeaderboardDisplay::Milliseconds: return "milliseconds";
    case LeaderboardDisplay::Number: break;
    }
    return "number";
}

// Reads the catalog's JSON, saying the first thing wrong.
class Reader {
public:
    explicit Reader(std::string* error) : _error(error) {}

    bool fail(std::string what) {
        if (_error) {
            *_error = std::move(what);
        }
        return false;
    }

    // An entry's id, its translation key (`name`, `description`) and its stores' ids.
    bool common(const JsonValue& entry, std::string_view kind, std::size_t index, std::string& id, std::set<std::string>& seen,
                ServicesStoreIds& stores) {
        const std::string where = std::string(kind) + " " + std::to_string(index + 1);
        if (!entry.is_object()) {
            return fail(where + " is not an object");
        }
        const JsonValue* given = entry.find("id");
        if (!given || !given->is_string() || given->as_string().empty()) {
            return fail(where + " has no id");
        }
        id = given->as_string();
        if (id.size() > 128) {
            return fail(std::string(kind) + " " + id + ": its id is over 128 characters");
        }
        if (!seen.insert(id).second) {
            return fail(std::string(kind) + " " + id + " is there twice");
        }
        for (const char* key : {"name", "description"}) {
            if (const JsonValue* text = entry.find(key); text && !text->is_string()) {
                return fail(std::string(kind) + " " + id + ": its " + key + " is not text");
            }
        }
        if (const JsonValue* given_stores = entry.find("stores")) {
            if (!given_stores->is_object()) {
                return fail(std::string(kind) + " " + id + ": its stores are not an object");
            }
            for (const auto& [store, there] : given_stores->members()) {
                if (!there.is_string() || there.as_string().empty() || there.as_string().size() > 128) {
                    return fail(std::string(kind) + " " + id + ": its id on " + store + " is not 1 to 128 characters of text");
                }
                stores[store] = there.as_string();
            }
        }
        return true;
    }

private:
    std::string* _error;
};

void write_stores(JsonWriter& json, const ServicesStoreIds& stores) {
    if (stores.empty()) {
        return;
    }
    json.key("stores").begin_object();
    for (const auto& [store, there] : stores) {
        json.field(store, there);
    }
    json.end_object();
}

void write_text(JsonWriter& json, std::string_view key, const std::string& text) {
    if (!text.empty()) {
        json.field(key, text);
    }
}

// The game's ids to the store's, and back in its events.
class CatalogServices final : public PlatformServices {
public:
    CatalogServices(PlatformServices& inner, ServicesCatalog catalog)
        : _inner(inner), _catalog(std::move(catalog)), _store(inner.backend()) {}

    std::string_view backend() const override { return _inner.backend(); }
    bool online() const override { return _inner.online(); }

    void update() override {
        _inner.update();
        _events.assign(_inner.events().begin(), _inner.events().end());
        for (ServiceEvent& event : _events) {
            switch (event.kind) {
            case ServiceEventKind::AchievementUnlocked: event.id = game_id(_catalog.achievements, event.id); break;
            case ServiceEventKind::LeaderboardUploaded:
            case ServiceEventKind::LeaderboardScores:
            case ServiceEventKind::LeaderboardFailed: event.id = game_id(_catalog.leaderboards, event.id); break;
            default: break;
            }
        }
    }
    std::span<const ServiceEvent> events() const override { return _events; }
    void flush() override { _inner.flush(); }

    ServicePlayer player() const override { return _inner.player(); }
    std::string language() const override { return _inner.language(); }
    bool handheld() const override { return _inner.handheld(); }
    bool big_picture() const override { return _inner.big_picture(); }
    bool cloud_saves() const override { return _inner.cloud_saves(); }

    bool unlock(std::string_view achievement) override {
        const std::optional<std::string> there = asked(_catalog.achievements, "achievement", achievement);
        return there && _inner.unlock(*there);
    }
    bool clear(std::string_view achievement) override {
        const std::optional<std::string> there = asked(_catalog.achievements, "achievement", achievement);
        return there && _inner.clear(*there);
    }
    std::optional<bool> unlocked(std::string_view achievement) const override {
        const std::optional<std::string> there = asked(_catalog.achievements, "achievement", achievement);
        return there ? _inner.unlocked(*there) : std::nullopt;
    }
    bool show_progress(std::string_view achievement, u32 current, u32 max) override {
        const std::optional<std::string> there = asked(_catalog.achievements, "achievement", achievement);
        if (!there) {
            return false;
        }
        if (const ServicesAchievement* listed = _catalog.achievement(achievement)) {
            if (max == 0) {
                max = listed->progress;
            }
            if (max == 0) {
                return false; // no most to count to
            }
            current = std::min(current, max);
        }
        return _inner.show_progress(*there, current, max);
    }

    bool set_stat(std::string_view stat, i32 value) override {
        const std::optional<std::string> there = asked(_catalog.stats, "stat", stat);
        if (!there) {
            return false;
        }
        return is_float(stat) ? _inner.set_stat(*there, static_cast<f32>(value)) : _inner.set_stat(*there, value);
    }
    bool set_stat(std::string_view stat, f32 value) override {
        const std::optional<std::string> there = asked(_catalog.stats, "stat", stat);
        if (!there || !std::isfinite(value)) {
            return false;
        }
        const ServicesStat* listed = _catalog.stat(stat);
        if (listed && listed->type == ServicesStatType::Int) {
            return _inner.set_stat(*there, static_cast<i32>(std::lround(value)));
        }
        return _inner.set_stat(*there, value);
    }
    std::optional<i32> stat_int(std::string_view stat) const override {
        const std::optional<std::string> there = asked(_catalog.stats, "stat", stat);
        if (!there) {
            return std::nullopt;
        }
        if (is_float(stat)) {
            const std::optional<f32> value = _inner.stat_float(*there);
            return value ? std::optional<i32>(static_cast<i32>(std::lround(*value))) : std::nullopt;
        }
        return _inner.stat_int(*there);
    }
    std::optional<f32> stat_float(std::string_view stat) const override {
        const std::optional<std::string> there = asked(_catalog.stats, "stat", stat);
        if (!there) {
            return std::nullopt;
        }
        const ServicesStat* listed = _catalog.stat(stat);
        if (listed && listed->type == ServicesStatType::Int) {
            const std::optional<i32> value = _inner.stat_int(*there);
            return value ? std::optional<f32>(static_cast<f32>(*value)) : std::nullopt;
        }
        return _inner.stat_float(*there);
    }

    u32 upload_score(std::string_view board, i32 score, std::span<const i32> details, LeaderboardUpload upload, LeaderboardSort sort,
                     LeaderboardDisplay display) override {
        const std::optional<std::string> there = asked(_catalog.leaderboards, "leaderboard", board);
        if (!there) {
            return 0;
        }
        if (const ServicesLeaderboard* listed = _catalog.leaderboard(board)) {
            sort = listed->sort;
            display = listed->display;
        }
        return _inner.upload_score(*there, score, details, upload, sort, display);
    }
    u32 request_scores(std::string_view board, LeaderboardRange range, i32 first, i32 last) override {
        const std::optional<std::string> there = asked(_catalog.leaderboards, "leaderboard", board);
        return there ? _inner.request_scores(*there, range, first, last) : 0;
    }

    bool set_presence(std::string_view key, std::string_view value) override { return _inner.set_presence(key, value); }
    void clear_presence() override { _inner.clear_presence(); }
    bool overlay_active() const override { return _inner.overlay_active(); }
    bool open_overlay(OverlayPage page) override { return _inner.open_overlay(page); }
    bool open_url(std::string_view url) override { return _inner.open_url(url); }
    bool open_store(std::string_view dlc) override { return _inner.open_store(dlc); }
    bool invite_friends(std::string_view lobby) override { return _inner.invite_friends(lobby); }
    bool show_keyboard(KeyboardMode mode, Rectf field) override { return _inner.show_keyboard(mode, field); }
    void hide_keyboard() override { _inner.hide_keyboard(); }
    bool ask_text(std::string_view prompt, std::string_view initial, u32 max_chars, bool password) override {
        return _inner.ask_text(prompt, initial, max_chars, password);
    }
    bool owns(std::string_view dlc) const override { return _inner.owns(dlc); }
    std::string join_on_launch() const override { return _inner.join_on_launch(); }

    // Lobbies and messages have no catalog ids: straight through.
    u32 create_lobby(const LobbyOptions& options) override { return _inner.create_lobby(options); }
    u32 join_lobby(std::string_view lobby) override { return _inner.join_lobby(lobby); }
    void leave_lobby() override { _inner.leave_lobby(); }
    u32 find_lobbies(const LobbyFilter& filter) override { return _inner.find_lobbies(filter); }
    std::string lobby() const override { return _inner.lobby(); }
    std::vector<LobbyMember> lobby_members() const override { return _inner.lobby_members(); }
    std::string lobby_owner() const override { return _inner.lobby_owner(); }
    bool set_lobby_data(std::string_view key, std::string_view value) override {
        return _inner.set_lobby_data(key, value);
    }
    std::string lobby_data(std::string_view key) const override { return _inner.lobby_data(key); }
    bool set_member_data(std::string_view key, std::string_view value) override {
        return _inner.set_member_data(key, value);
    }
    std::string member_data(std::string_view member, std::string_view key) const override {
        return _inner.member_data(member, key);
    }
    bool set_lobby_joinable(bool joinable) override { return _inner.set_lobby_joinable(joinable); }
    bool send_lobby_message(std::string_view text) override { return _inner.send_lobby_message(text); }
    bool send_message(std::string_view player, std::span<const u8> bytes, MessageDelivery delivery,
                      u32 channel) override {
        return _inner.send_message(player, bytes, delivery, channel);
    }
    void receive_messages(u32 channel, std::vector<PlayerMessage>& out) override {
        _inner.receive_messages(channel, out);
    }
    void close_messages(std::string_view player) override { _inner.close_messages(player); }
    std::size_t max_message() const override { return _inner.max_message(); }

private:
    // The store's id for the game's `id`: none when the catalog lists that kind and not it.
    template <class Entry>
    std::optional<std::string> asked(const std::vector<Entry>& entries, std::string_view kind, std::string_view id) const {
        if (entries.empty()) {
            return std::string(id);
        }
        if (const Entry* listed = find_entry(entries, id)) {
            return std::string(store_id(listed->stores, listed->id, _store));
        }
        if (_warned.insert(std::string(kind) + ":" + std::string(id)).second) {
            KIN_LOG_WARN_F("services", "not in the game's services catalog",
                           (LogFields{{.name = "kind", .value = std::string(kind)}, {.name = "id", .value = std::string(id)}}));
        }
        return std::nullopt;
    }

    // The game's id for the store's `there` (as it is when none has it).
    template <class Entry> std::string game_id(const std::vector<Entry>& entries, const std::string& there) const {
        for (const Entry& entry : entries) {
            if (store_id(entry.stores, entry.id, _store) == there) {
                return entry.id;
            }
        }
        return there;
    }

    bool is_float(std::string_view stat) const {
        const ServicesStat* listed = _catalog.stat(stat);
        return listed && listed->type == ServicesStatType::Float;
    }

    PlatformServices& _inner;
    ServicesCatalog _catalog;
    std::string _store;
    std::vector<ServiceEvent> _events;
    mutable std::set<std::string> _warned;
};

} // namespace

const ServicesAchievement* ServicesCatalog::achievement(std::string_view id) const {
    return find_entry(achievements, id);
}

const ServicesStat* ServicesCatalog::stat(std::string_view id) const {
    return find_entry(stats, id);
}

const ServicesLeaderboard* ServicesCatalog::leaderboard(std::string_view id) const {
    return find_entry(leaderboards, id);
}

std::string_view store_id(const ServicesStoreIds& stores, std::string_view id, std::string_view store) {
    const auto it = stores.find(store);
    return it == stores.end() ? id : std::string_view(it->second);
}

bool parse_services_catalog(std::string_view text, ServicesCatalog& out, std::string* error) {
    Reader reader(error);
    const JsonParseResult parsed = parse_json(text);
    if (!parsed.ok()) {
        return reader.fail("not JSON: " + parsed.error);
    }
    const JsonValue& root = *parsed.value;
    if (!root.is_object()) {
        return reader.fail("not a JSON object");
    }
    if (const JsonValue* given = root.find("schema"); given && (!given->is_string() || given->as_string() != schema)) {
        return reader.fail("its schema is not " + std::string(schema));
    }
    ServicesCatalog catalog;
    const auto list = [&](std::string_view key) -> const JsonValue* {
        const JsonValue* found = root.find(key);
        return found && found->is_array() ? found : nullptr;
    };
    for (const char* key : {"achievements", "stats", "leaderboards"}) {
        if (const JsonValue* found = root.find(key); found && !found->is_array()) {
            return reader.fail(std::string(key) + " is not a list");
        }
    }
    if (const JsonValue* entries = list("achievements")) {
        std::set<std::string> seen;
        for (std::size_t i = 0; i < entries->items().size(); ++i) {
            const JsonValue& entry = entries->items()[i];
            ServicesAchievement achievement;
            if (!reader.common(entry, "achievement", i, achievement.id, seen, achievement.stores)) {
                return false;
            }
            achievement.name = entry.string_at("name");
            achievement.description = entry.string_at("description");
            achievement.hidden = entry.bool_at("hidden");
            const i64 progress = entry.int_at("progress");
            if (progress < 0 || progress > 0xffffffffLL) {
                return reader.fail("achievement " + achievement.id + ": its progress is not a count");
            }
            achievement.progress = static_cast<u32>(progress);
            catalog.achievements.push_back(std::move(achievement));
        }
    }
    if (const JsonValue* entries = list("stats")) {
        std::set<std::string> seen;
        for (std::size_t i = 0; i < entries->items().size(); ++i) {
            const JsonValue& entry = entries->items()[i];
            ServicesStat stat;
            if (!reader.common(entry, "stat", i, stat.id, seen, stat.stores)) {
                return false;
            }
            stat.name = entry.string_at("name");
            const std::string type = entry.string_at("type", "int");
            if (type != "int" && type != "float") {
                return reader.fail("stat " + stat.id + ": its type is int or float, not " + type);
            }
            stat.type = type == "float" ? ServicesStatType::Float : ServicesStatType::Int;
            catalog.stats.push_back(std::move(stat));
        }
    }
    if (const JsonValue* entries = list("leaderboards")) {
        std::set<std::string> seen;
        for (std::size_t i = 0; i < entries->items().size(); ++i) {
            const JsonValue& entry = entries->items()[i];
            ServicesLeaderboard board;
            if (!reader.common(entry, "leaderboard", i, board.id, seen, board.stores)) {
                return false;
            }
            board.name = entry.string_at("name");
            const std::string sort = entry.string_at("sort", "descending");
            if (sort != "descending" && sort != "ascending") {
                return reader.fail("leaderboard " + board.id + ": its sort is descending or ascending, not " + sort);
            }
            board.sort = sort == "ascending" ? LeaderboardSort::Ascending : LeaderboardSort::Descending;
            const std::string display = entry.string_at("display", "number");
            if (display == "number") {
                board.display = LeaderboardDisplay::Number;
            } else if (display == "seconds") {
                board.display = LeaderboardDisplay::Seconds;
            } else if (display == "milliseconds") {
                board.display = LeaderboardDisplay::Milliseconds;
            } else {
                return reader.fail("leaderboard " + board.id + ": its display is number, seconds or milliseconds, not " + display);
            }
            catalog.leaderboards.push_back(std::move(board));
        }
    }
    out = std::move(catalog);
    return true;
}

bool load_services_catalog(const std::filesystem::path& file, ServicesCatalog& out, std::string* error) {
    // From the content's pack when the game ships one, else from disk.
    const std::optional<std::string> text = read_content_text(file);
    if (!text) {
        if (error) {
            *error = "could not open " + file.string();
        }
        return false;
    }
    return parse_services_catalog(*text, out, error);
}

std::string write_services_catalog(const ServicesCatalog& catalog) {
    std::ostringstream out;
    JsonWriter json(out);
    json.begin_object();
    json.field("schema", schema);
    json.key("achievements").begin_array();
    for (const ServicesAchievement& achievement : catalog.achievements) {
        json.begin_object();
        json.field("id", achievement.id);
        write_text(json, "name", achievement.name);
        write_text(json, "description", achievement.description);
        if (achievement.hidden) {
            json.field("hidden", true);
        }
        if (achievement.progress > 0) {
            json.field("progress", static_cast<i64>(achievement.progress));
        }
        write_stores(json, achievement.stores);
        json.end_object();
    }
    json.end_array();
    json.key("stats").begin_array();
    for (const ServicesStat& stat : catalog.stats) {
        json.begin_object();
        json.field("id", stat.id);
        json.field("type", stat.type == ServicesStatType::Float ? "float" : "int");
        write_text(json, "name", stat.name);
        write_stores(json, stat.stores);
        json.end_object();
    }
    json.end_array();
    json.key("leaderboards").begin_array();
    for (const ServicesLeaderboard& board : catalog.leaderboards) {
        json.begin_object();
        json.field("id", board.id);
        write_text(json, "name", board.name);
        json.field("sort", sort_name(board.sort));
        json.field("display", display_name(board.display));
        write_stores(json, board.stores);
        json.end_object();
    }
    json.end_array();
    json.end_object();
    out << '\n';
    return out.str();
}

std::unique_ptr<PlatformServices> catalog_services(PlatformServices& inner, ServicesCatalog catalog) {
    return std::make_unique<CatalogServices>(inner, std::move(catalog));
}

} // namespace kin
