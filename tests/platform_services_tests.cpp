// kin's platform services (kin/platform/services.hpp): the offline backend,
// FakeServices, the Lua `services` table, the game info's steam_app_id, and
// the Steam backend driven through a stand-in for Steam's library
// (fake_steam_api.cpp), loaded the way the real one is.

#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_timer.h>

#ifdef KIN_REAL_STEAM_DIR
#include "steam_flat.hpp"
#endif
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <kin/platform/services.hpp>
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
    // Without services: nothing, no errors.
    kin::LuaScript bare;
    assert(bare.load_string("function run() return services.unlock('X') == false and services.stat('x') == nil end"));
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
        std::vector<ServiceEvent> seen;
        for (int i = 0; i < 2; ++i) { // find results arrive and the queued requests go out; then their results
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
#define KIN_REQUIRE(field, symbol, ret, params)                                    \
    if (!SDL_LoadFunction(library, #symbol)) {                                     \
        std::fprintf(stderr, "real steam library lacks %s\n", #symbol);           \
        ++missing;                                                                 \
    }
    KIN_STEAM_FUNCTIONS(KIN_REQUIRE)
    KIN_STEAM_OPTIONAL_FUNCTIONS(KIN_REQUIRE)
#undef KIN_REQUIRE
    const std::pair<const char*, const char*> newest[] = {
        {"SteamUser", sf::user_versions[0]},   {"SteamFriends", sf::friends_versions[0]},
        {"SteamUtils", sf::utils_versions[0]}, {"SteamApps", sf::apps_versions[0]},
        {"SteamUserStats", sf::stats_versions[0]}, {"SteamRemoteStorage", sf::storage_versions[0]},
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
    auto services = kin::start_platform_services({.game_id = "kin_steam_probe", .steam_app_id = 480,
                                                  .steam_restart_through_client = false,
                                                  .steam_library = KIN_REAL_STEAM_DIR, .offline_in_memory = true},
                                                 &start);
    assert(services && start != kin::ServicesStart::Restarting);
    std::printf("real steam: backend=%s online=%d player=%s language=%s handheld=%d\n",
                std::string{services->backend()}.c_str(), services->online() ? 1 : 0,
                services->player().name.c_str(), services->language().c_str(), services->handheld() ? 1 : 0);
    if (services->backend() != "steam") {
        return; // Steam not running or not signed in: loading and starting were checked
    }
    // Read-only, on Spacewar's own achievements, stats and leaderboard: nothing
    // is unlocked, uploaded or shown to friends.
    const kin::u32 request = services->request_scores("Feet Traveled", kin::LeaderboardRange::Top, 1, 3);
    const ServiceEvent* scores = nullptr;
    std::vector<ServiceEvent> seen;
    for (int i = 0; i < 200 && !scores; ++i) {
        services->update();
        for (const ServiceEvent& event : services->events()) {
            seen.push_back(event);
            std::printf("event %d request=%u text=%s\n", static_cast<int>(event.kind), event.request,
                        event.text.c_str());
        }
        for (const ServiceEvent& event : seen) {
            if (event.request == request &&
                (event.kind == ServiceEventKind::LeaderboardScores || event.kind == ServiceEventKind::LeaderboardFailed)) {
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
    test_steam_backend_through_the_library();
    return 0;
}
