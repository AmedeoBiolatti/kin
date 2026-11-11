// kin's platform services (kin/platform/services.hpp): the offline backend,
// FakeServices, the Lua `services` table, the game info's steam_app_id, and
// the Steam backend driven through a stand-in for Steam's library
// (fake_steam_api.cpp), loaded the way the real one is.

#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_timer.h>

#ifdef KIN_REAL_STEAM_DIR
#include "steam_flat.hpp"
#endif
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <kin/platform/services.hpp>
#include <kin/platform/services_catalog.hpp>
#include <kin/platform/user_data.hpp>
#include <kin/runtime/game_info.hpp>
#include <kin/runtime/scene_app.hpp>
#include <kin/scripting/lua_script.hpp>
#include <limits>
#include <sol/sol.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using kin::ServiceEvent;
using kin::ServiceEventKind;

fs::path temp_root(std::string_view name) {
    const fs::path path = fs::temp_directory_path() / "kin-platform-services-tests" / name;
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

const ServiceEvent* find_event(kin::PlatformServices& services, ServiceEventKind kind) {
    for (const ServiceEvent& event : services.events()) {
        if (event.kind == kind) {
            return &event;
        }
    }
    return nullptr;
}

void test_language_from_steam() {
    assert(kin::language_from_steam("french") == "fr");
    assert(kin::language_from_steam("brazilian") == "pt-BR");
    assert(kin::language_from_steam("schinese") == "zh-Hans");
    assert(kin::language_from_steam("latam") == "es-419");
    assert(kin::language_from_steam("klingon").empty());
}

void test_offline_keeps_achievements_stats_and_best_scores() {
    const fs::path root = temp_root("offline");
    kin::set_user_data_dir_override(root);
    {
        kin::ServicesStart start = kin::ServicesStart::Ready;
        auto services = kin::start_platform_services({.game_id = "vale"}, &start);
        assert(start == kin::ServicesStart::Offline);
        assert(services->backend() == "offline");
        assert(!services->online());
        assert(services->unlocked("FIRST") == false);
        assert(services->unlock("FIRST"));
        assert(services->unlock("FIRST")); // again: no second event
        assert(!services->unlock(""));
        assert(services->set_stat("kills", 3));
        assert(services->add_stat("kills", 2));
        assert(services->set_stat("distance", 12.5f));
        assert(!services->set_stat("bad", std::numeric_limits<kin::f32>::infinity()));
        const kin::u32 up = services->upload_score("arena", 900);
        const kin::u32 worse = services->upload_score("arena", 400);
        assert(up != 0 && worse != 0 && up != worse);
        // What the platform alone does says so.
        assert(!services->set_presence("status", "x"));
        assert(!services->open_overlay(kin::OverlayPage::Friends));
        assert(!services->show_keyboard(kin::KeyboardMode::SingleLine, {}));
        services->update();
        int unlocked = 0;
        for (const ServiceEvent& event : services->events()) {
            unlocked += event.kind == ServiceEventKind::AchievementUnlocked;
            if (event.kind == ServiceEventKind::LeaderboardUploaded) {
                assert(event.changed == (event.request == up));
            }
        }
        assert(unlocked == 1);
        services->flush();
        services->update();
        assert(find_event(*services, ServiceEventKind::StatsStored));
    }
    const fs::path file = root / "kin" / "vale" / "services" / "offline.json";
    assert(fs::exists(file));
    {
        auto services = kin::make_offline_services({.game_id = "vale"});
        assert(services->unlocked("FIRST") == true);
        assert(services->stat_int("kills") == 5);
        assert(services->stat_float("distance") == 12.5f);
        assert(!services->stat_int("distance").has_value()); // decimal
        const kin::u32 request = services->request_scores("arena", kin::LeaderboardRange::Top, 1, 10);
        services->update();
        const ServiceEvent* scores = find_event(*services, ServiceEventKind::LeaderboardScores);
        assert(scores && scores->request == request);
        assert(scores->entries.size() == 1 && scores->entries[0].score == 900);
        assert(services->clear("FIRST"));
    }
    {
        auto services = kin::make_offline_services({.game_id = "vale"});
        assert(services->unlocked("FIRST") == false);
    }
    // In memory: nothing written.
    {
        auto services = kin::make_offline_services({.game_id = "memory", .offline_in_memory = true});
        services->unlock("X");
        services->flush();
    }
    assert(!fs::exists(root / "kin" / "memory"));
    // An app id with offline_only never tries Steam.
    {
        kin::ServicesStart start = kin::ServicesStart::Ready;
        auto services = kin::start_platform_services(
            {.game_id = "vale", .steam_app_id = 480, .offline_only = true, .offline_in_memory = true}, &start);
        assert(start == kin::ServicesStart::Offline && services->backend() == "offline");
    }
    kin::clear_user_data_dir_override();
}

void test_fake_services_record_calls() {
    kin::FakeServices fake;
    fake.achievements = {"A"};
    assert(fake.unlock("A"));
    assert(!fake.unlock("B"));
    assert(!fake.unlocked("B").has_value());
    fake.set_overlay(true);
    fake.set_join_command("lobby:7");
    assert(fake.presence.size() == 1 && fake.presence[0].second == "lobby:7");
    fake.update();
    assert(find_event(fake, ServiceEventKind::OverlayShown));
    assert(find_event(fake, ServiceEventKind::AchievementUnlocked));
    assert(fake.overlay_active());
    fake.update();
    assert(fake.events().empty());
    assert(fake.calls.front().what == "unlock");
}

void test_lua_services_table() {
    kin::FakeServices fake;
    fake.dlc = {"1234"};
    kin::set_active_services(&fake);
    kin::LuaScript script;
    const bool loaded = script.load_string(R"(
        function run()
            assert(services.backend() == "fake")
            assert(services.unlock("WIN"))
            assert(services.unlocked("WIN") == true)
            assert(services.set_stat("kills", 4))
            assert(services.add_stat("kills", 2))
            assert(services.stat("kills") == 6)
            assert(services.set_stat("time", 1.5))
            assert(services.stat("time") == 1.5)
            assert(math.type(services.stat("kills")) == "integer")
            assert(services.presence("status", "Exploring"))
            assert(services.owns("1234") and not services.owns("5"))
            assert(services.upload_score("arena", 10, {1, 2}, {keep = "replace"}) > 0)
            assert(services.player().name == "Player")
            return true
        end
        function read_events()
            local kinds = {}
            for _, e in ipairs(services.events()) do kinds[#kinds + 1] = e.kind .. ":" .. e.text end
            return table.concat(kinds, ",")
        end
    )");
    assert(loaded);
    assert(script.call_for<bool>("run") == true);
    fake.send({.kind = ServiceEventKind::JoinRequested, .text = "lobby:9"});
    fake.update();
    const std::optional<std::string> kinds = script.call_for<std::string>("read_events");
    assert(kinds && kinds->find("join_requested:lobby:9") != std::string::npos);
    assert(kinds->find("achievement_unlocked:") != std::string::npos);
    kin::set_active_services(nullptr);

    // Lobbies, on a fake network with another player.
    kin::FakeNetwork network;
    kin::FakeServices ann(network, {.id = "fake:ann", .name = "Ann"});
    kin::FakeServices bob(network, {.id = "fake:bob", .name = "Bob"});
    kin::set_active_services(&ann);
    kin::LuaScript lobby_script;
    assert(lobby_script.load_string(R"(
        function make()
            return services.create_lobby({max_members = 3, visibility = "public", data = {mode = "coop", level = 3}}) > 0
        end
        function check()
            local entered = false
            for _, e in ipairs(services.events()) do entered = entered or e.kind == "lobby_entered" end
            assert(entered)
            assert(services.lobby():sub(1, 6) == "lobby:")
            assert(services.lobby_owner() == "fake:ann")
            assert(services.lobby_data("level") == "3" and services.lobby_data("mode") == "coop")
            assert(services.set_member_data("ready", "1") and services.member_data("fake:ann", "ready") == "1")
            assert(#services.lobby_members() == 1 and services.lobby_members()[1].name == "Ann")
            assert(services.lobby_message("hi"))
            return services.find_lobbies({data = {mode = "coop"}, open_slots = 1}) > 0
        end
        function found()
            for _, e in ipairs(services.events()) do
                if e.kind == "lobby_list" then
                    return #e.lobbies == 1 and e.lobbies[1].data.mode == "coop" and e.lobbies[1].max_members == 3
                end
            end
            return false
        end
    )"));
    assert(lobby_script.call_for<bool>("make") == true);
    ann.update();
    assert(lobby_script.call_for<bool>("check") == true);
    ann.update();
    assert(lobby_script.call_for<bool>("found") == true);
    assert(bob.join_lobby(ann.lobby()) != 0 && ann.lobby_members().size() == 2);
    kin::set_active_services(nullptr);

    // Without services: nothing, no errors.
    kin::LuaScript bare;
    assert(bare.load_string("function run() return services.unlock('X') == false and services.stat('x') == nil and "
                            "services.create_lobby() == 0 and services.lobby() == '' end"));
    assert(bare.call_for<bool>("run") == true);
}

void test_game_info_steam_app_id() {
    const fs::path root = temp_root("info");
    const fs::path path = root / "project.kin";
    {
        std::ofstream out(path);
        out << "id vale\ntitle Vale\nsteam_app_id 480\n";
    }
    kin::GameInfo info = kin::load_game_info(path);
    assert(info.steam_app_id == 480);
    assert(kin::save_game_info(info, root / "again.kin"));
    assert(kin::load_game_info(root / "again.kin").steam_app_id == 480);
    {
        std::ofstream out(path);
        out << "steam_app_id lots\n";
    }
    bool threw = false;
    try {
        (void)kin::load_game_info(path);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

// The stand-in Steam library: the same file the backend opens.
struct FakeSteam {
    SDL_SharedObject* library = nullptr;
    template <class F> F get(const char* name) const {
        auto f = reinterpret_cast<F>(SDL_LoadFunction(library, name));
        assert(f);
        return f;
    }
    FakeSteam() {
        library = SDL_LoadObject((fs::path{KIN_FAKE_STEAM_DIR} / KIN_FAKE_STEAM_FILE).string().c_str());
        assert(library);
        get<void (*)()>("kin_fake_steam_reset")();
    }
    ~FakeSteam() { SDL_UnloadObject(library); }
};

std::unique_ptr<kin::PlatformServices> start_fake_steam(kin::ServicesStart& start, std::vector<std::string> args = {}) {
    return kin::start_platform_services({.game_id = "vale",
                                         .steam_app_id = 480,
                                         .steam_library = KIN_FAKE_STEAM_DIR,
                                         .offline_in_memory = true,
                                         .args = std::move(args)},
                                        &start);
}

void test_steam_backend_through_the_library() {
    FakeSteam steam;
    kin::ServicesStart start = kin::ServicesStart::Offline;
    {
        auto services = start_fake_steam(start, {"vale", "+connect_lobby", "4242"});
        assert(start == kin::ServicesStart::Ready);
        assert(services->backend() == "steam");
        assert(services->online());
        assert(services->player().id == "steam:76561197960287930");
        assert(services->player().name == "Fake Player");
        assert(services->language() == "pt-BR");
        assert(services->cloud_saves());
        assert(!services->handheld());
        assert(services->owns("1234") && !services->owns("99") && !services->owns("x"));
        assert(services->join_on_launch() == "lobby:4242");

        // Stats are asked for (an SDK before 1.61): what comes first waits for them.
        assert(services->unlock("FIRST"));
        assert(!services->unlocked("FIRST").has_value());
        assert(!steam.get<bool (*)(const char*)>("kin_fake_steam_achieved")("FIRST"));
        services->update(); // UserStatsReceived: the unlock goes through
        assert(steam.get<bool (*)(const char*)>("kin_fake_steam_achieved")("FIRST"));
        assert(services->unlocked("FIRST") == true);
        assert(services->set_stat("kills", 7));
        assert(services->stat_int("kills") == 7);
        assert(!services->stat_int("unknown").has_value());
        assert(!services->unlock("NO_SUCH"));
        const int stores = steam.get<int (*)()>("kin_fake_steam_stores")(); // the first update may have kept them
        services->flush();
        assert(steam.get<int (*)()>("kin_fake_steam_stores")() == stores + 1);
        services->flush(); // nothing changed: nothing kept
        assert(steam.get<int (*)()>("kin_fake_steam_stores")() == stores + 1);
        services->update();
        assert(find_event(*services, ServiceEventKind::StatsStored));
        const ServiceEvent* unlocked = find_event(*services, ServiceEventKind::AchievementUnlocked);
        assert(unlocked && unlocked->id == "FIRST");

        // Presence, the overlay, the store.
        assert(services->set_presence("status", "Exploring"));
        assert(std::string_view{steam.get<const char* (*)(const char*)>("kin_fake_steam_presence")("status")} ==
               "Exploring");
        services->open_overlay(kin::OverlayPage::Achievements);
        assert(std::string_view{steam.get<const char* (*)()>("kin_fake_steam_last_overlay")()} == "achievements");
        assert(services->invite_friends("lobby:55"));
        assert(std::string_view{steam.get<const char* (*)()>("kin_fake_steam_last_overlay")()} == "invite:55");
        assert(services->open_store(""));
        assert(steam.get<kin::u32 (*)()>("kin_fake_steam_last_store_app")() == 480);
        assert(services->open_store("1234"));
        assert(steam.get<kin::u32 (*)()>("kin_fake_steam_last_store_app")() == 1234);

        // Callbacks become events.
        steam.get<void (*)(bool)>("kin_fake_steam_overlay")(true);
        steam.get<void (*)(kin::u64, kin::u64)>("kin_fake_steam_lobby_invite")(99, 2);
        steam.get<void (*)(const char*)>("kin_fake_steam_presence_join")("+connect 10.0.0.1");
        services->update();
        assert(services->overlay_active());
        assert(find_event(*services, ServiceEventKind::OverlayShown));
        int joins = 0;
        for (const ServiceEvent& event : services->events()) {
            if (event.kind == ServiceEventKind::JoinRequested) {
                ++joins;
                assert(event.text == "lobby:99" || event.text == "+connect 10.0.0.1");
            }
        }
        assert(joins == 2);
        steam.get<void (*)(bool)>("kin_fake_steam_overlay")(false);
        services->update();
        assert(!services->overlay_active() && find_event(*services, ServiceEventKind::OverlayHidden));

        // The full-screen text prompt.
        assert(services->ask_text("Name", "", 32, false));
        steam.get<void (*)(bool)>("kin_fake_steam_text_dismissed")(true);
        services->update();
        const ServiceEvent* text = find_event(*services, ServiceEventKind::TextEntered);
        assert(text && text->text == "typed on the deck");

        // Leaderboards: found once, then used.
        const kin::i32 details[] = {4, 5};
        const kin::u32 upload = services->upload_score("arena", 1200, details);
        const kin::u32 scores = services->request_scores("arena", kin::LeaderboardRange::Top, 1, 2);
        const kin::u32 missing = services->upload_score("missing", 1);
        assert(upload && scores && missing);
        std::deque<ServiceEvent> seen; // events stay where they are while more arrive
        for (int i = 0; i < 2; ++i) {  // find results arrive and the queued requests go out; then their results
            services->update();
            seen.insert(seen.end(), services->events().begin(), services->events().end());
        }
        bool uploaded = false;
        bool listed = false;
        bool failed = false;
        for (const ServiceEvent& event : seen) {
            if (event.kind == ServiceEventKind::LeaderboardUploaded) {
                uploaded = event.request == upload && event.rank == 3 && event.changed;
            } else if (event.kind == ServiceEventKind::LeaderboardScores) {
                listed = event.request == scores && event.entries.size() == 2 && event.entries[0].score == 500 &&
                         event.entries[1].player_name == "Second" && event.entries[1].details.size() == 1 &&
                         event.entries[1].details[0] == 8;
            } else if (event.kind == ServiceEventKind::LeaderboardFailed) {
                failed = event.request == missing;
            }
        }
        assert(uploaded && listed && failed);
        assert(steam.get<int (*)(int)>("kin_fake_steam_uploaded")(0) == 1200);
        assert(steam.get<int (*)(int)>("kin_fake_steam_uploaded")(2) == 5);

        // Steam going away.
        steam.get<void (*)()>("kin_fake_steam_shutdown_signal")();
        services->update();
        assert(find_event(*services, ServiceEventKind::Disconnected));
        assert(!services->online());
    }
    assert(steam.get<int (*)()>("kin_fake_steam_shutdowns")() == 1);

    // On a Deck, the floating keyboard.
    steam.get<void (*)()>("kin_fake_steam_reset")();
    steam.get<void (*)(bool)>("kin_fake_steam_set_deck")(true);
    {
        auto services = start_fake_steam(start);
        assert(services->handheld() && services->big_picture());
        assert(services->show_keyboard(kin::KeyboardMode::SingleLine, {10, 20, 300, 40}));
        assert(steam.get<int (*)()>("kin_fake_steam_keyboards")() == 1);
        services->hide_keyboard();
        assert(steam.get<int (*)()>("kin_fake_steam_keyboards")() == 0);
    }

    // Started outside Steam: Steam starts the game again.
    steam.get<void (*)()>("kin_fake_steam_reset")();
    steam.get<void (*)(bool)>("kin_fake_steam_set_restart")(true);
    assert(!start_fake_steam(start));
    assert(start == kin::ServicesStart::Restarting);

    // Steam not running: offline, and nothing to shut down.
    steam.get<void (*)()>("kin_fake_steam_reset")();
    steam.get<void (*)(kin::i32)>("kin_fake_steam_set_init")(2);
    {
        auto services = start_fake_steam(start);
        assert(start == kin::ServicesStart::Offline && services->backend() == "offline");
    }
    assert(steam.get<int (*)()>("kin_fake_steam_shutdowns")() == 0);

    // No library at all.
    {
        auto services = kin::start_platform_services(
            {.steam_app_id = 480, .steam_library = temp_root("no-steam"), .offline_in_memory = true}, &start);
        assert(start == kin::ServicesStart::Offline && services->backend() == "offline");
    }
}

// Lobbies and messages through Steam's matchmaking and networking messages.
void test_steam_lobbies_and_messages() {
    using kin::u64;
    using kin::u8;
    FakeSteam steam;
    kin::ServicesStart start = kin::ServicesStart::Offline;
    auto services = start_fake_steam(start);
    assert(start == kin::ServicesStart::Ready);
    const std::string self = services->player().id;
    const auto pump = [&] { services->update(); };

    // Finding lobbies: the filters go to Steam as they are.
    steam.get<void (*)(u64, u64, int, const char*, const char*)>("kin_fake_steam_add_lobby")(500, 2, 4, "mode", "coop");
    const kin::u32 search = services->find_lobbies(
        {.rules = {{.key = "mode", .value = "coop"},
                   {.key = "level", .value = "2", .compare = kin::LobbyCompare::AtLeast, .numeric = true}},
         .open_slots = 1,
         .distance = kin::LobbyDistance::Worldwide,
         .max_results = 10});
    assert(search != 0);
    pump();
    const ServiceEvent* list = find_event(*services, ServiceEventKind::LobbyList);
    assert(list && list->request == search && list->ok && list->lobbies.size() == 1);
    assert(list->lobbies[0].id == "lobby:500" && list->lobbies[0].members == 1 && list->lobbies[0].max_members == 4);
    assert(list->lobbies[0].data.size() == 1 && list->lobbies[0].data[0].second == "coop");
    assert(std::string_view{steam.get<const char* (*)()>("kin_fake_steam_filters")()} ==
           "mode=coop;level>=2;slots1;distance3;count10;");

    // Joining: entered once (the callback and the call result both say so).
    const kin::u32 joined = services->join_lobby("lobby:500");
    pump();
    const ServiceEvent* entered = find_event(*services, ServiceEventKind::LobbyEntered);
    assert(entered && entered->request == joined && entered->id == "lobby:500");
    assert(std::ranges::count_if(services->events(),
                                 [](const ServiceEvent& e) { return e.kind == ServiceEventKind::LobbyEntered; }) == 1);
    assert(services->lobby() == "lobby:500" && services->lobby_owner() == "steam:2");
    assert(services->lobby_members().size() == 2 && services->lobby_members()[1].id == self);
    assert(services->lobby_data("mode") == "coop" && !services->set_lobby_data("mode", "x")); // not the owner
    assert(steam.get<int (*)()>("kin_fake_steam_relays")() == 1); // relays are found once, on first use

    steam.get<void (*)(u64, u64)>("kin_fake_steam_member_joins")(500, 3);
    pump();
    const ServiceEvent* arrived = find_event(*services, ServiceEventKind::LobbyMemberJoined);
    assert(arrived && arrived->id == "steam:3" && arrived->text == "First");

    // A member may open a message session with us; a stranger may not.
    steam.get<void (*)(u64)>("kin_fake_steam_session_request")(3);
    steam.get<void (*)(u64)>("kin_fake_steam_session_request")(9);
    pump();
    assert(steam.get<bool (*)(u64)>("kin_fake_steam_accepted")(3));
    assert(!steam.get<bool (*)(u64)>("kin_fake_steam_accepted")(9));

    // Messages: to a Steam id, unreliable (but not dropped before the session opens) or reliable, by channel.
    const std::vector<u8> bytes = {1, 2, 3};
    assert(services->max_message() == 1200);
    assert(services->send_message("steam:3", bytes));
    assert(services->send_message("steam:3", std::vector<u8>(5000), kin::MessageDelivery::Reliable, 2));
    assert(!services->send_message("steam:3", std::vector<u8>(1201)));
    assert(!services->send_message("steam:x", bytes) && !services->send_message("lobby:500x", bytes));
    const auto sent = steam.get<int (*)(int, u8*, int, u64*, int*, int*)>("kin_fake_steam_sent");
    u8 out[16] = {};
    u64 to = 0;
    int flags = 0, channel = 0;
    assert(sent(0, out, sizeof out, &to, &flags, &channel) == 3 && to == 3 && channel == 0 && out[2] == 3);
    assert(flags == (1 | 32));
    assert(sent(1, out, sizeof out, &to, &flags, &channel) == 5000 && channel == 2);
    assert(flags == (8 | 1 | 32));
    assert(steam.get<int (*)()>("kin_fake_steam_sent_count")() == 2);

    const auto message_from = steam.get<void (*)(u64, int, const u8*, int)>("kin_fake_steam_message_from");
    const u8 a[] = {9, 8}, b[] = {7};
    message_from(3, 0, a, 2);
    message_from(3, 1, b, 1);
    std::vector<kin::PlayerMessage> got;
    services->receive_messages(0, got);
    assert(got.size() == 1 && got[0].from == "steam:3" && got[0].bytes == std::vector<u8>({9, 8}));
    services->receive_messages(1, got);
    assert(got.size() == 2 && got[1].bytes == std::vector<u8>({7}));
    assert(steam.get<int (*)()>("kin_fake_steam_released")() == 2); // every message handed back to Steam
    services->close_messages("steam:3");
    assert(steam.get<bool (*)(u64)>("kin_fake_steam_closed")(3));
    steam.get<void (*)(u64, kin::i32)>("kin_fake_steam_session_failed")(3, 5001);
    pump();
    const ServiceEvent* failed = find_event(*services, ServiceEventKind::MessagesFailed);
    assert(failed && failed->id == "steam:3" && failed->text == "end reason 5001");

    // Member data and chat.
    assert(services->set_member_data("ready", "1"));
    assert(services->send_lobby_message("hi"));
    pump();
    const ServiceEvent* changed = find_event(*services, ServiceEventKind::LobbyDataChanged);
    assert(changed && changed->id == self && services->member_data(self, "ready") == "1");
    const ServiceEvent* chat = find_event(*services, ServiceEventKind::LobbyMessage);
    assert(chat && chat->id == self && chat->text == "hi");

    steam.get<void (*)(u64, u64)>("kin_fake_steam_member_leaves")(500, 2);
    pump();
    const ServiceEvent* left = find_event(*services, ServiceEventKind::LobbyMemberLeft);
    assert(left && left->id == "steam:2" && services->lobby_owner() == self); // Steam gave it to us
    steam.get<void (*)(u64)>("kin_fake_steam_kicked")(500);
    pump();
    assert(find_event(*services, ServiceEventKind::LobbyLeft) && services->lobby().empty());

    // Requests answered after they were cancelled leave their lobby again: no ghost member stays behind.
    const auto members = steam.get<int (*)(u64)>("kin_fake_steam_member_count");
    const auto count = [&](ServiceEventKind kind) {
        return std::ranges::count_if(services->events(), [&](const ServiceEvent& e) { return e.kind == kind; });
    };
    steam.get<void (*)(u64, u64, int, const char*, const char*)>("kin_fake_steam_add_lobby")(510, 2, 4, "mode", "coop");
    const kin::u32 cancelled = services->join_lobby("lobby:510");
    services->leave_lobby();
    pump();
    const ServiceEvent* failed_join = find_event(*services, ServiceEventKind::LobbyFailed);
    assert(failed_join && failed_join->request == cancelled && failed_join->text == "cancelled");
    assert(count(ServiceEventKind::LobbyEntered) == 0 && services->lobby().empty() && members(510) == 1);
    const kin::u32 first = services->create_lobby({});
    const kin::u32 second = services->create_lobby({});
    pump();
    const ServiceEvent* replaced = find_event(*services, ServiceEventKind::LobbyFailed);
    entered = find_event(*services, ServiceEventKind::LobbyEntered);
    assert(replaced && replaced->request == first && replaced->text == "cancelled");
    assert(entered && entered->request == second && count(ServiceEventKind::LobbyEntered) == 1);
    const u64 made_second = std::stoull(services->lobby().substr(6));
    assert(members(made_second - 1) == 0 && members(made_second) == 1);
    // Created, then joined elsewhere before the answer: only the joined lobby keeps us.
    const kin::u32 dropped = services->create_lobby({});
    const kin::u32 joined_instead = services->join_lobby("lobby:510");
    pump();
    replaced = find_event(*services, ServiceEventKind::LobbyFailed);
    entered = find_event(*services, ServiceEventKind::LobbyEntered);
    assert(replaced && replaced->request == dropped && entered && entered->request == joined_instead);
    assert(services->lobby() == "lobby:510" && members(made_second) == 0 && members(made_second + 1) == 0);

    // Removed from the lobby (the owner kicked us, or Steam dropped us): LobbyLeft, not LobbyMemberLeft.
    const u64 self_id = std::stoull(self.substr(6));
    steam.get<void (*)(u64, u64)>("kin_fake_steam_member_leaves")(510, self_id);
    pump();
    assert(count(ServiceEventKind::LobbyLeft) == 1 && count(ServiceEventKind::LobbyMemberLeft) == 0);
    assert(services->lobby().empty());
    // Dropped while the connection was gone: found out when it comes back.
    services->join_lobby("lobby:510");
    pump();
    assert(services->lobby() == "lobby:510");
    const auto connection = steam.get<void (*)(bool)>("kin_fake_steam_connection");
    connection(false);
    steam.get<void (*)(u64, u64)>("kin_fake_steam_drop_member")(510, self_id);
    pump();
    assert(count(ServiceEventKind::Disconnected) == 1 && services->lobby() == "lobby:510");
    connection(true);
    pump();
    assert(count(ServiceEventKind::Reconnected) == 1 && count(ServiceEventKind::LobbyLeft) == 1);
    assert(services->lobby().empty());
    // Still a member after a short drop: nothing changes.
    services->join_lobby("lobby:510");
    pump();
    connection(false);
    connection(true);
    pump();
    assert(count(ServiceEventKind::LobbyLeft) == 0 && services->lobby() == "lobby:510");
    services->leave_lobby();

    // Steam keeps one search: a newer one ends the last, which is answered as failed.
    const kin::u32 older = services->find_lobbies({});
    const kin::u32 newer = services->find_lobbies({});
    pump();
    assert(count(ServiceEventKind::LobbyList) == 2);
    for (const ServiceEvent& event : services->events()) {
        if (event.kind == ServiceEventKind::LobbyList) {
            assert(event.request == older ? !event.ok && event.text == "replaced by a newer search"
                                          : event.request == newer && event.ok);
        }
    }

    // Lobbies that cannot be joined.
    steam.get<void (*)(u64, u64, int, const char*, const char*)>("kin_fake_steam_add_lobby")(600, 2, 1, "mode", "coop");
    const kin::u32 full = services->join_lobby("lobby:600");
    const kin::u32 missing = services->join_lobby("lobby:700");
    pump();
    for (const ServiceEvent& event : services->events()) {
        if (event.kind == ServiceEventKind::LobbyFailed) {
            assert((event.request == full && event.text == "the lobby is full") ||
                   (event.request == missing && event.text == "no such lobby"));
        }
        assert(event.kind != ServiceEventKind::LobbyEntered);
    }
    assert(services->lobby().empty() && services->join_lobby("nonsense") == 0);

    // Making one: its data is set before anyone can see it, and only the owner closes it.
    const kin::u32 made = services->create_lobby(
        {.max_members = 3, .visibility = kin::LobbyVisibility::Public, .data = {{"mode", "versus"}}});
    pump();
    entered = find_event(*services, ServiceEventKind::LobbyEntered);
    assert(entered && entered->request == made && entered->id == services->lobby());
    assert(services->lobby_owner() == self && services->lobby_data("mode") == "versus");
    assert(services->set_lobby_joinable(false) && services->set_lobby_data("round", "2"));
    services->leave_lobby();
    assert(services->lobby().empty());
}

#ifdef KIN_REAL_STEAM_DIR
// Valve's own library (KIN_STEAMWORKS_SDK_DIR): every function and newest
// interface kin uses is there, and starting works, or falls back offline when
// Steam is not running. With Steam running and signed in, it reaches Steam as
// Spacewar (480).

void test_real_steam_library() {
    namespace sf = kin::steam_flat;
    const fs::path path = fs::path{KIN_REAL_STEAM_DIR} / sf::library_name;
    SDL_SharedObject* library = SDL_LoadObject(path.string().c_str());
    assert(library);
    int missing = 0;
#define KIN_REQUIRE(field, symbol, ret, params)                                                                        \
    if (!SDL_LoadFunction(library, #symbol)) {                                                                         \
        std::fprintf(stderr, "real steam library lacks %s\n", #symbol);                                                \
        ++missing;                                                                                                     \
    }
    KIN_STEAM_FUNCTIONS(KIN_REQUIRE)
    KIN_STEAM_OPTIONAL_FUNCTIONS(KIN_REQUIRE)
    KIN_STEAM_NETWORK_FUNCTIONS(KIN_REQUIRE)
#undef KIN_REQUIRE
    const std::pair<const char*, const char*> newest[] = {
        {"SteamUser", sf::user_versions[0]},
        {"SteamFriends", sf::friends_versions[0]},
        {"SteamUtils", sf::utils_versions[0]},
        {"SteamApps", sf::apps_versions[0]},
        {"SteamUserStats", sf::stats_versions[0]},
        {"SteamRemoteStorage", sf::storage_versions[0]},
        {"SteamMatchmaking", sf::matchmaking_versions[0]},
        {"SteamNetworkingMessages_SteamAPI", sf::messages_versions[0]},
        {"SteamNetworkingUtils_SteamAPI", sf::networking_utils_versions[0]},
    };
    for (const auto& [name, version] : newest) {
        const std::string symbol = std::string{"SteamAPI_"} + name + "_" + version;
        if (!SDL_LoadFunction(library, symbol.c_str())) {
            std::fprintf(stderr, "real steam library lacks %s\n", symbol.c_str());
            ++missing;
        }
    }
    SDL_UnloadObject(library);
    assert(missing == 0);

    kin::ServicesStart start{};
    auto services = kin::start_platform_services({.game_id = "kin_steam_probe",
                                                  .steam_app_id = 480,
                                                  .steam_restart_through_client = false,
                                                  .steam_library = KIN_REAL_STEAM_DIR,
                                                  .offline_in_memory = true},
                                                 &start);
    assert(services && start != kin::ServicesStart::Restarting);
    std::printf("real steam: backend=%s online=%d player=%s language=%s handheld=%d\n",
                std::string{services->backend()}.c_str(), services->online() ? 1 : 0, services->player().name.c_str(),
                services->language().c_str(), services->handheld() ? 1 : 0);
    if (services->backend() != "steam") {
        return; // Steam not running or not signed in: loading and starting were checked
    }
    // Read-only, on Spacewar's own achievements, stats and leaderboard: nothing
    // is unlocked, uploaded or shown to friends.
    const kin::u32 request = services->request_scores("Feet Traveled", kin::LeaderboardRange::Top, 1, 3);
    // Spacewar's public lobbies, as anyone sees them (nothing is made or joined).
    const kin::u32 search = services->find_lobbies({.distance = kin::LobbyDistance::Worldwide, .max_results = 5});
    assert(search != 0);
    const ServiceEvent* scores = nullptr;
    std::deque<ServiceEvent> seen; // events stay where they are while more arrive
    const auto found_lobbies = [&]() -> const ServiceEvent* {
        for (const ServiceEvent& event : seen) {
            if (event.kind == ServiceEventKind::LobbyList && event.request == search) {
                return &event;
            }
        }
        return nullptr;
    };
    for (int i = 0; i < 200 && (!scores || !found_lobbies()); ++i) {
        services->update();
        for (const ServiceEvent& event : services->events()) {
            seen.push_back(event);
            std::printf("event %d request=%u text=%s\n", static_cast<int>(event.kind), event.request,
                        event.text.c_str());
        }
        for (const ServiceEvent& event : seen) {
            if (event.request == request && (event.kind == ServiceEventKind::LeaderboardScores ||
                                             event.kind == ServiceEventKind::LeaderboardFailed)) {
                scores = &event;
            }
        }
        SDL_Delay(50);
    }
    const std::optional<bool> won = services->unlocked("ACH_WIN_ONE_GAME");
    const std::optional<kin::i32> games = services->stat_int("NumGames");
    const std::optional<kin::f32> feet = services->stat_float("FeetTraveled");
    std::printf("real steam: ACH_WIN_ONE_GAME=%s NumGames=%s FeetTraveled=%s\n",
                won ? (*won ? "unlocked" : "locked") : "?", games ? std::to_string(*games).c_str() : "?",
                feet ? std::to_string(*feet).c_str() : "?");
    assert(won.has_value() && games.has_value() && feet.has_value());
    assert(!services->stat_int("NoSuchStat").has_value());
    assert(scores && scores->kind == ServiceEventKind::LeaderboardScores);
    for (const kin::LeaderboardEntry& entry : scores->entries) {
        std::printf("  #%d %s %d\n", entry.rank, entry.player_name.c_str(), entry.score);
        assert(entry.rank >= 1 && entry.player_id.starts_with("steam:"));
    }
    assert(!scores->entries.empty());
    const ServiceEvent* lobbies = found_lobbies();
    assert(lobbies && lobbies->ok);
    std::printf("real steam: %zu Spacewar lobbies\n", lobbies->lobbies.size());
    for (const kin::LobbyInfo& lobby : lobbies->lobbies) {
        std::printf("  %s %u/%u, %zu data\n", lobby.id.c_str(), lobby.members, lobby.max_members, lobby.data.size());
        assert(lobby.id.starts_with("lobby:") && lobby.max_members >= lobby.members);
    }
}
#endif

// What run_scene_app starts: the game info's Steam app id; offline (in memory)
// for headless runs; offline as --services asks, kept beside the saves or not.
void test_scene_app_services() {
    const kin::GameInfo game{.id = "vale", .steam_app_id = 480};
    kin::SceneAppConfig config{.game = &game};
    kin::ServicesConfig wanted = kin::scene_app_services_config(config);
    assert(wanted.game_id == "vale" && wanted.steam_app_id == 480 && !wanted.offline_only && !wanted.offline_in_memory);

    char arg0[] = "vale";
    char offline[] = "--services=offline";
    char* offline_argv[] = {arg0, offline};
    config.headless = kin::parse_headless_options(2, offline_argv);
    assert(config.headless.services == "offline" && !config.headless.enabled);
    wanted = kin::scene_app_services_config(config);
    assert(wanted.offline_only && !wanted.offline_in_memory && wanted.game_id == "vale");

    config.headless.services = "memory";
    wanted = kin::scene_app_services_config(config);
    assert(wanted.offline_only && wanted.offline_in_memory);
    kin::ServicesStart start = kin::ServicesStart::Ready;
    auto services = kin::start_platform_services(wanted, &start);
    assert(start == kin::ServicesStart::Offline && services->backend() == "offline");

    config.headless.services = "galaxy"; // not one: the game's, and a warning
    wanted = kin::scene_app_services_config(config);
    assert(!wanted.offline_only && wanted.steam_app_id == 480);

    config.headless = {};
    config.headless.enabled = true; // headless: never the machine's Steam
    wanted = kin::scene_app_services_config(config);
    assert(wanted.offline_only && wanted.offline_in_memory);

    // A config of the game's own is taken as it is (its Steam app id with it).
    config.headless = {};
    config.services_config = kin::ServicesConfig{.game_id = "other", .steam_app_id = 0};
    wanted = kin::scene_app_services_config(config);
    assert(wanted.game_id == "other" && wanted.steam_app_id == 0 && !wanted.offline_only);
}

constexpr std::string_view catalog_text = R"({
  "schema": "kin.services/1",
  "achievements": [
    {"id": "ALL_COINS", "name": "achievement.all_coins", "description": "achievement.all_coins.how", "progress": 5,
     "stores": {"fake": "F_COINS", "steam": "ACH_COINS"}},
    {"id": "SECRET", "hidden": true}
  ],
  "stats": [{"id": "coins"}, {"id": "best_time", "type": "float", "stores": {"fake": "F_TIME"}}],
  "leaderboards": [{"id": "speedrun", "sort": "ascending", "display": "milliseconds", "stores": {"fake": "F_SPEED"}}]
})";

// The game's list of its achievements, stats and leaderboards: read, written
// and read again, and what is wrong with one said.
void test_services_catalog_file() {
    kin::ServicesCatalog catalog;
    std::string error;
    assert(kin::parse_services_catalog(catalog_text, catalog, &error));
    assert(catalog.achievements.size() == 2 && catalog.stats.size() == 2 && catalog.leaderboards.size() == 1);
    const kin::ServicesAchievement* coins = catalog.achievement("ALL_COINS");
    assert(coins && coins->name == "achievement.all_coins" && coins->description == "achievement.all_coins.how");
    assert(coins->progress == 5 && !coins->hidden && catalog.achievement("SECRET")->hidden);
    assert(kin::store_id(coins->stores, coins->id, "steam") == "ACH_COINS" && kin::store_id(coins->stores, coins->id, "gog") == "ALL_COINS");
    assert(catalog.stat("coins")->type == kin::ServicesStatType::Int && catalog.stat("best_time")->type == kin::ServicesStatType::Float);
    const kin::ServicesLeaderboard* speedrun = catalog.leaderboard("speedrun");
    assert(speedrun->sort == kin::LeaderboardSort::Ascending && speedrun->display == kin::LeaderboardDisplay::Milliseconds);
    assert(!catalog.achievement("NOPE") && !catalog.stat("NOPE") && !catalog.leaderboard("NOPE"));

    kin::ServicesCatalog again;
    assert(kin::parse_services_catalog(kin::write_services_catalog(catalog), again, &error));
    assert(kin::write_services_catalog(again) == kin::write_services_catalog(catalog));
    assert(again.achievement("ALL_COINS")->stores.at("steam") == "ACH_COINS" && again.leaderboard("speedrun")->sort == kin::LeaderboardSort::Ascending);
    kin::ServicesCatalog empty;
    assert(kin::parse_services_catalog("{}", empty, &error) && empty.achievements.empty());

    for (const char* broken : {"not json", "[]", R"({"schema": "kin.services/9"})", R"({"achievements": {}})",
                               R"({"achievements": [{"name": "x"}]})", R"({"achievements": [{"id": "A"}, {"id": "A"}]})",
                               R"({"stats": [{"id": "s", "type": "text"}]})", R"({"leaderboards": [{"id": "b", "sort": "up"}]})",
                               R"({"leaderboards": [{"id": "b", "display": "hours"}]})", R"({"stats": [{"id": "s", "stores": {"steam": ""}}]})",
                               R"({"achievements": [{"id": "A", "progress": -1}]})"}) {
        error.clear();
        kin::ServicesCatalog bad = catalog;
        assert(!kin::parse_services_catalog(broken, bad, &error) && !error.empty());
        assert(bad.achievements.size() == 2); // left as it was
    }
}

// The game asks by its ids; the store is asked by its own, and answers by them.
void test_catalog_services() {
    kin::ServicesCatalog catalog;
    assert(kin::parse_services_catalog(catalog_text, catalog));
    kin::FakeServices fake;
    const auto services = kin::catalog_services(fake, catalog);
    assert(services->backend() == "fake");

    assert(services->unlock("ALL_COINS") && fake.calls.back().what == "unlock" && fake.calls.back().id == "F_COINS");
    assert(services->unlocked("ALL_COINS") == true && fake.unlocked("F_COINS") == true);
    assert(services->unlock("SECRET") && fake.calls.back().id == "SECRET"); // no id of the store's: the game's
    const std::size_t asked = fake.calls.size();
    assert(!services->unlock("NOPE") && !services->unlocked("NOPE").has_value() && fake.calls.size() == asked); // not listed
    services->update();
    const auto unlocked = std::ranges::find(services->events(), kin::ServiceEventKind::AchievementUnlocked, &kin::ServiceEvent::kind);
    assert(unlocked != services->events().end() && unlocked->id == "ALL_COINS");

    // Progress counts to the listed most.
    assert(services->show_progress("ALL_COINS", 7, 0) && fake.calls.back().id == "F_COINS" && fake.calls.back().value == "5/5");
    assert(!services->show_progress("SECRET", 1, 0)); // nothing to count to

    // Stats as their types say.
    assert(services->set_stat("best_time", 41) && fake.stat_float("F_TIME") == 41.0f && services->stat_float("best_time") == 41.0f);
    assert(services->set_stat("coins", 2.6f) && fake.stat_int("coins") == 3 && services->stat_int("coins") == 3);
    assert(services->add_stat("coins", 2) && services->stat_int("coins") == 5);
    assert(!services->set_stat("NOPE", 1));

    // Leaderboards by the store's name, back by the game's.
    assert(services->upload_score("speedrun", 4100) != 0 && fake.calls.back().id == "F_SPEED");
    assert(services->upload_score("other", 1) == 0);
    services->update();
    fake.send({.kind = kin::ServiceEventKind::LeaderboardScores, .id = "F_SPEED", .ok = true});
    services->update();
    assert(services->events().size() == 1 && services->events()[0].id == "speedrun");

    // A kind the catalog lists none of goes as it is.
    kin::ServicesCatalog achievements_only;
    achievements_only.achievements = catalog.achievements;
    const auto passing = kin::catalog_services(fake, achievements_only);
    assert(passing->set_stat("anything", 1) && fake.calls.back().id == "anything");
    assert(passing->upload_score("anyboard", 3) != 0 && fake.calls.back().id == "anyboard");

    // Lobbies and messages go through as they are.
    kin::FakeNetwork network;
    kin::FakeServices ann(network, {.id = "fake:ann", .name = "Ann"});
    kin::FakeServices bob(network, {.id = "fake:bob", .name = "Bob"});
    const auto ann_listed = kin::catalog_services(ann, catalog);
    assert(ann_listed->create_lobby({.max_members = 2, .visibility = kin::LobbyVisibility::Public, .data = {{"mode", "coop"}}}) != 0);
    ann_listed->update();
    assert(!ann_listed->lobby().empty() && ann_listed->lobby() == ann.lobby());
    assert(ann_listed->lobby_owner() == "fake:ann" && ann_listed->lobby_data("mode") == "coop");
    assert(bob.join_lobby(ann_listed->lobby()) != 0);
    bob.update();
    ann_listed->update();
    assert(ann_listed->lobby_members().size() == 2 && ann_listed->max_message() == ann.max_message());
    const kin::u8 hello[] = {1, 2, 3};
    assert(ann_listed->send_message("fake:bob", hello, kin::MessageDelivery::Reliable, 4));
    bob.update();
    std::vector<kin::PlayerMessage> got;
    bob.receive_messages(4, got);
    assert(got.size() == 1 && got[0].bytes.size() == 3);
    ann_listed->leave_lobby();
    assert(ann.lobby().empty());

    // Lower is better, as listed: kin's offline backend keeps the best by it.
    const auto offline = kin::make_offline_services({.offline_in_memory = true});
    const auto listed = kin::catalog_services(*offline, catalog);
    listed->upload_score("speedrun", 4100);
    listed->upload_score("speedrun", 5000); // asked as higher is better: the list says otherwise
    listed->update();
    std::vector<bool> changed;
    for (const kin::ServiceEvent& event : listed->events()) {
        if (event.kind == kin::ServiceEventKind::LeaderboardUploaded) {
            assert(event.id == "speedrun");
            changed.push_back(event.changed);
        }
    }
    assert((changed == std::vector<bool>{true, false}));

    // Scripts ask the same way; progress without a most takes the list's.
    kin::set_active_services(services.get());
    kin::LuaScript script;
    assert(script.load_string(R"(
        function run()
            assert(services.progress("ALL_COINS", 2))
            assert(not services.unlock("NOPE"))
            return true
        end
    )"));
    assert(script.call_for<bool>("run") == true);
    assert(fake.calls.back().what == "progress" && fake.calls.back().value == "2/5");
    kin::set_active_services(nullptr);
}

} // namespace

int main(int argc, char** argv) {
#ifdef KIN_REAL_STEAM_DIR
    if (argc > 1 && std::string_view{argv[1]} == "--real-steam") {
        test_real_steam_library();
        return 0;
    }
#endif
    (void)argc;
    (void)argv;
    test_language_from_steam();
    test_offline_keeps_achievements_stats_and_best_scores();
    test_fake_services_record_calls();
    test_lua_services_table();
    test_game_info_steam_app_id();
    test_scene_app_services();
    test_services_catalog_file();
    test_catalog_services();
    test_steam_backend_through_the_library();
    test_steam_lobbies_and_messages();
    return 0;
}
