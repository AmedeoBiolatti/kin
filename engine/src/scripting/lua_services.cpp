#include "lua_services.hpp"

#include <algorithm>
#include <kin/platform/services.hpp>
#include <sol/sol.hpp>
#include <span>
#include <string>
#include <vector>

namespace kin::scripting_detail {
namespace {

PlatformServices* active() {
    return active_services();
}

std::string_view event_name(ServiceEventKind kind) {
    switch (kind) {
    case ServiceEventKind::OverlayShown:
        return "overlay_shown";
    case ServiceEventKind::OverlayHidden:
        return "overlay_hidden";
    case ServiceEventKind::KeyboardClosed:
        return "keyboard_closed";
    case ServiceEventKind::TextEntered:
        return "text_entered";
    case ServiceEventKind::TextCancelled:
        return "text_cancelled";
    case ServiceEventKind::JoinRequested:
        return "join_requested";
    case ServiceEventKind::AchievementUnlocked:
        return "achievement_unlocked";
    case ServiceEventKind::StatsStored:
        return "stats_stored";
    case ServiceEventKind::StatsFailed:
        return "stats_failed";
    case ServiceEventKind::LeaderboardScores:
        return "leaderboard_scores";
    case ServiceEventKind::LeaderboardUploaded:
        return "leaderboard_uploaded";
    case ServiceEventKind::LeaderboardFailed:
        return "leaderboard_failed";
    case ServiceEventKind::DlcInstalled:
        return "dlc_installed";
    case ServiceEventKind::Disconnected:
        return "disconnected";
    case ServiceEventKind::Reconnected:
        return "reconnected";
    case ServiceEventKind::LobbyEntered:
        return "lobby_entered";
    case ServiceEventKind::LobbyFailed:
        return "lobby_failed";
    case ServiceEventKind::LobbyList:
        return "lobby_list";
    case ServiceEventKind::LobbyMemberJoined:
        return "lobby_member_joined";
    case ServiceEventKind::LobbyMemberNamed:
        return "lobby_member_named";
    case ServiceEventKind::LobbyMemberLeft:
        return "lobby_member_left";
    case ServiceEventKind::LobbyDataChanged:
        return "lobby_data_changed";
    case ServiceEventKind::LobbyMessage:
        return "lobby_message";
    case ServiceEventKind::LobbyLeft:
        return "lobby_left";
    case ServiceEventKind::MessagesFailed:
        return "messages_failed";
    }
    return "unknown";
}

std::optional<OverlayPage> overlay_page(std::string_view name) {
    if (name == "friends")
        return OverlayPage::Friends;
    if (name == "community")
        return OverlayPage::Community;
    if (name == "players")
        return OverlayPage::Players;
    if (name == "settings")
        return OverlayPage::Settings;
    if (name == "achievements")
        return OverlayPage::Achievements;
    if (name == "stats")
        return OverlayPage::Stats;
    return std::nullopt;
}

std::vector<i32> int_list(const sol::optional<sol::table>& table) {
    std::vector<i32> out;
    if (table) {
        for (std::size_t i = 1; i <= table->size(); ++i) {
            out.push_back(table->get<sol::optional<i32>>(i).value_or(0));
        }
    }
    return out;
}

sol::table entries_table(sol::state_view lua, const std::vector<LeaderboardEntry>& entries) {
    sol::table out = lua.create_table();
    int i = 1;
    for (const LeaderboardEntry& entry : entries) {
        sol::table row = lua.create_table();
        row["player_id"] = entry.player_id;
        row["player_name"] = entry.player_name;
        row["rank"] = entry.rank;
        row["score"] = entry.score;
        sol::table details = lua.create_table();
        for (std::size_t d = 0; d < entry.details.size(); ++d) {
            details[d + 1] = entry.details[d];
        }
        row["details"] = details;
        out[i++] = row;
    }
    return out;
}

sol::table data_table(sol::state_view lua, const std::vector<std::pair<std::string, std::string>>& data) {
    sol::table out = lua.create_table();
    for (const auto& [key, value] : data) {
        out[key] = value;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> data_pairs(const sol::optional<sol::table>& table) {
    std::vector<std::pair<std::string, std::string>> out;
    if (table) {
        for (const auto& [key, value] : *table) {
            if (key.get_type() != sol::type::string) {
                continue;
            }
            if (value.get_type() == sol::type::string) {
                out.emplace_back(key.as<std::string>(), value.as<std::string>());
            } else if (value.get_type() == sol::type::number) {
                value.push();
                const bool whole = lua_isinteger(value.lua_state(), -1);
                lua_pop(value.lua_state(), 1);
                out.emplace_back(key.as<std::string>(),
                                 whole ? std::to_string(value.as<lua_Integer>()) : std::to_string(value.as<double>()));
            }
        }
        std::ranges::sort(out); // a Lua table has no order: keep one
    }
    return out;
}

// A count from a script's table: negative, fractional or absurd values do not wrap around.
u32 count_or(const sol::table& table, const char* key, u32 fallback) {
    const sol::optional<double> value = table.get<sol::optional<double>>(key);
    return value ? static_cast<u32>(std::clamp(*value, 0.0, 1'000'000.0)) : fallback;
}

LobbyVisibility visibility(std::string_view name) {
    if (name == "private")
        return LobbyVisibility::Private;
    if (name == "public")
        return LobbyVisibility::Public;
    if (name == "invisible")
        return LobbyVisibility::Invisible;
    return LobbyVisibility::FriendsOnly;
}

} // namespace

void bind_services(sol::state_view lua) {
    sol::table t = lua.create_named_table("services");
    t["backend"] = []() -> std::string {
        PlatformServices* s = active();
        return s ? std::string{s->backend()} : std::string{};
    };
    t["online"] = [] { return active() && active()->online(); };
    t["player"] = [](sol::this_state state) {
        sol::state_view view{state};
        sol::table out = view.create_table();
        if (PlatformServices* s = active()) {
            const ServicePlayer p = s->player();
            out["id"] = p.id;
            out["name"] = p.name;
        }
        return out;
    };
    t["language"] = []() -> std::string { return active() ? active()->language() : std::string{}; };
    t["handheld"] = [] { return active() && active()->handheld(); };
    t["big_picture"] = [] { return active() && active()->big_picture(); };
    t["cloud_saves"] = [] { return active() && active()->cloud_saves(); };

    t["unlock"] = [](const std::string& id) { return active() && active()->unlock(id); };
    t["clear_achievement"] = [](const std::string& id) { return active() && active()->clear(id); };
    t["unlocked"] = [](const std::string& id) -> sol::optional<bool> {
        if (PlatformServices* s = active()) {
            if (const std::optional<bool> done = s->unlocked(id)) {
                return *done;
            }
        }
        return sol::nullopt;
    };
    // Without a most: the catalog's (services_catalog.hpp).
    t["progress"] = [](const std::string& id, u32 current, const sol::optional<u32>& max) {
        return active() && active()->show_progress(id, current, max.value_or(0));
    };
    t["set_stat"] = [](const std::string& id, sol::stack_object value) {
        PlatformServices* s = active();
        if (!s || value.get_type() != sol::type::number) {
            return false;
        }
        if (lua_isinteger(value.lua_state(), value.stack_index())) {
            return s->set_stat(id, static_cast<i32>(value.as<lua_Integer>()));
        }
        return s->set_stat(id, static_cast<f32>(value.as<f64>()));
    };
    t["add_stat"] = [](const std::string& id, i32 delta) { return active() && active()->add_stat(id, delta); };
    t["stat"] = [](const std::string& id, sol::this_state state) -> sol::object {
        sol::state_view view{state};
        if (PlatformServices* s = active()) {
            if (const std::optional<i32> whole = s->stat_int(id)) {
                return sol::make_object(view, static_cast<lua_Integer>(*whole));
            }
            if (const std::optional<f32> decimal = s->stat_float(id)) {
                return sol::make_object(view, static_cast<f64>(*decimal));
            }
        }
        return sol::lua_nil;
    };

    t["upload_score"] = [](const std::string& board, i32 score, const sol::optional<sol::table>& details,
                           const sol::optional<sol::table>& options) -> u32 {
        PlatformServices* s = active();
        if (!s) {
            return 0;
        }
        LeaderboardUpload upload = LeaderboardUpload::KeepBest;
        LeaderboardSort sort = LeaderboardSort::Descending;
        LeaderboardDisplay display = LeaderboardDisplay::Number;
        if (options) {
            if (options->get_or<std::string>("keep", "best") == "replace")
                upload = LeaderboardUpload::Replace;
            if (options->get_or<std::string>("sort", "desc") == "asc")
                sort = LeaderboardSort::Ascending;
            const std::string shown = options->get_or<std::string>("display", "number");
            if (shown == "seconds")
                display = LeaderboardDisplay::Seconds;
            if (shown == "milliseconds")
                display = LeaderboardDisplay::Milliseconds;
        }
        const std::vector<i32> extra = int_list(details);
        return s->upload_score(board, score, extra, upload, sort, display);
    };
    t["request_scores"] = [](const std::string& board, const sol::optional<std::string>& range,
                             const sol::optional<i32>& first, const sol::optional<i32>& last) -> u32 {
        PlatformServices* s = active();
        if (!s) {
            return 0;
        }
        const std::string r = range.value_or("top");
        const LeaderboardRange which = r == "around"    ? LeaderboardRange::AroundPlayer
                                       : r == "friends" ? LeaderboardRange::Friends
                                                        : LeaderboardRange::Top;
        const i32 a = first.value_or(which == LeaderboardRange::AroundPlayer ? -4 : 1);
        const i32 b = last.value_or(which == LeaderboardRange::AroundPlayer ? 5 : 10);
        return s->request_scores(board, which, a, b);
    };

    t["presence"] = [](const std::string& key, const std::string& value) {
        return active() && active()->set_presence(key, value);
    };
    t["clear_presence"] = [] {
        if (PlatformServices* s = active())
            s->clear_presence();
    };
    t["join_command"] = [](const std::string& command) { return active() && active()->set_join_command(command); };
    t["overlay_active"] = [] { return active() && active()->overlay_active(); };
    t["open_overlay"] = [](const sol::optional<std::string>& page) {
        const std::optional<OverlayPage> which = overlay_page(page.value_or("friends"));
        return active() && which && active()->open_overlay(*which);
    };
    t["open_url"] = [](const std::string& url) { return active() && active()->open_url(url); };
    t["open_store"] = [](const sol::optional<std::string>& dlc) {
        return active() && active()->open_store(dlc.value_or(""));
    };
    t["invite"] = [](const std::string& lobby) { return active() && active()->invite_friends(lobby); };
    t["ask_text"] = [](const std::string& prompt, const sol::optional<std::string>& initial,
                       const sol::optional<u32>& max, const sol::optional<bool>& password) {
        return active() && active()->ask_text(prompt, initial.value_or(""), max.value_or(0), password.value_or(false));
    };
    t["owns"] = [](const std::string& dlc) { return active() && active()->owns(dlc); };
    t["join_on_launch"] = []() -> std::string { return active() ? active()->join_on_launch() : std::string{}; };
    // Lobbies.
    t["create_lobby"] = [](const sol::optional<sol::table>& options) -> u32 {
        PlatformServices* s = active();
        if (!s) {
            return 0;
        }
        LobbyOptions lobby;
        if (options) {
            lobby.max_members = count_or(*options, "max_members", lobby.max_members);
            lobby.visibility = visibility(options->get_or<std::string>("visibility", "friends"));
            lobby.data = data_pairs(options->get<sol::optional<sol::table>>("data"));
        }
        return s->create_lobby(lobby);
    };
    t["join_lobby"] = [](const std::string& lobby) -> u32 { return active() ? active()->join_lobby(lobby) : 0; };
    t["leave_lobby"] = [] {
        if (PlatformServices* s = active())
            s->leave_lobby();
    };
    // find_lobbies({ data = { mode = "coop" }, open_slots = 1, worldwide = true, max = 20 })
    t["find_lobbies"] = [](const sol::optional<sol::table>& options) -> u32 {
        PlatformServices* s = active();
        if (!s) {
            return 0;
        }
        LobbyFilter filter;
        if (options) {
            for (auto& [key, value] : data_pairs(options->get<sol::optional<sol::table>>("data"))) {
                filter.rules.push_back({.key = std::move(key), .value = std::move(value)});
            }
            filter.open_slots = count_or(*options, "open_slots", 0);
            if (options->get_or("worldwide", false)) {
                filter.distance = LobbyDistance::Worldwide;
            }
            filter.max_results = count_or(*options, "max", filter.max_results);
        }
        return s->find_lobbies(filter);
    };
    t["lobby"] = []() -> std::string { return active() ? active()->lobby() : std::string{}; };
    t["lobby_owner"] = []() -> std::string { return active() ? active()->lobby_owner() : std::string{}; };
    t["lobby_members"] = [](sol::this_state state) {
        sol::state_view view{state};
        sol::table out = view.create_table();
        if (PlatformServices* s = active()) {
            int i = 1;
            for (const LobbyMember& member : s->lobby_members()) {
                sol::table row = view.create_table();
                row["id"] = member.id;
                row["name"] = member.name;
                out[i++] = row;
            }
        }
        return out;
    };
    t["set_lobby_data"] = [](const std::string& key, const std::string& value) {
        return active() && active()->set_lobby_data(key, value);
    };
    t["lobby_data"] = [](const std::string& key) -> std::string {
        return active() ? active()->lobby_data(key) : std::string{};
    };
    t["set_member_data"] = [](const std::string& key, const std::string& value) {
        return active() && active()->set_member_data(key, value);
    };
    t["member_data"] = [](const std::string& member, const std::string& key) -> std::string {
        return active() ? active()->member_data(member, key) : std::string{};
    };
    t["set_lobby_joinable"] = [](bool joinable) { return active() && active()->set_lobby_joinable(joinable); };
    t["lobby_message"] = [](const std::string& text) { return active() && active()->send_lobby_message(text); };
    // Messages between players, as strings of bytes:
    //   services.send_message(player, bytes, { reliable = true, channel = 2 })
    //   for _, m in ipairs(services.receive_messages(2)) do ... m.from, m.bytes ... end
    t["send_message"] = [](const std::string& player, const std::string& bytes, const sol::optional<sol::table>& how) {
        PlatformServices* s = active();
        if (!s) {
            return false;
        }
        const bool reliable = how && how->get_or("reliable", false);
        const u32 channel = how ? count_or(*how, "channel", 0) : 0;
        const auto* data = reinterpret_cast<const u8*>(bytes.data());
        return s->send_message(player, std::span<const u8>(data, bytes.size()),
                               reliable ? MessageDelivery::Reliable : MessageDelivery::Unreliable, channel);
    };
    t["receive_messages"] = [](const sol::optional<double>& channel, sol::this_state state) {
        sol::state_view view{state};
        sol::table out = view.create_table();
        if (PlatformServices* s = active()) {
            std::vector<PlayerMessage> messages;
            s->receive_messages(channel ? static_cast<u32>(std::clamp(*channel, 0.0, 1'000'000.0)) : 0, messages);
            int i = 1;
            for (const PlayerMessage& message : messages) {
                sol::table row = view.create_table();
                row["from"] = message.from;
                row["bytes"] = std::string(reinterpret_cast<const char*>(message.bytes.data()), message.bytes.size());
                out[i++] = row;
            }
        }
        return out;
    };
    t["close_messages"] = [](const std::string& player) {
        if (PlatformServices* s = active())
            s->close_messages(player);
    };
    t["max_message"] = []() -> std::size_t { return active() ? active()->max_message() : 0; };

    t["flush"] = [] {
        if (PlatformServices* s = active())
            s->flush();
    };
    t["events"] = [](sol::this_state state) {
        sol::state_view view{state};
        sol::table out = view.create_table();
        if (PlatformServices* s = active()) {
            int i = 1;
            for (const ServiceEvent& event : s->events()) {
                sol::table row = view.create_table();
                row["kind"] = std::string{event_name(event.kind)};
                row["id"] = event.id;
                row["text"] = event.text;
                row["request"] = event.request;
                row["ok"] = event.ok;
                row["changed"] = event.changed;
                row["rank"] = event.rank;
                if (!event.entries.empty()) {
                    row["entries"] = entries_table(view, event.entries);
                }
                if (event.kind == ServiceEventKind::LobbyList) {
                    sol::table lobbies = view.create_table();
                    int l = 1;
                    for (const LobbyInfo& lobby : event.lobbies) {
                        sol::table entry = view.create_table();
                        entry["id"] = lobby.id;
                        entry["members"] = lobby.members;
                        entry["max_members"] = lobby.max_members;
                        entry["data"] = data_table(view, lobby.data);
                        lobbies[l++] = entry;
                    }
                    row["lobbies"] = lobbies;
                }
                out[i++] = row;
            }
        }
        return out;
    };
}

} // namespace kin::scripting_detail
