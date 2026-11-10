// A stand-in for Steam's API library (libsteam_api.so / steam_api64.dll) for
// platform_services_tests: it exports the flat C functions kin's Steam backend
// opens, with the same names, and kin_fake_steam_* functions the test drives
// it with. It follows kin's own declarations (steam_flat.hpp), so it tests how
// the backend loads, dispatches and answers, not Steam's real layout: that is
// steam_abi_check.cpp's job, against the SDK.

#include "steam_flat.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#define FAKE_API extern "C" __declspec(dllexport)
#else
#define FAKE_API extern "C" __attribute__((visibility("default")))
#endif

namespace sf = kin::steam_flat;
using kin::f32;
using kin::i32;
using kin::u32;
using kin::u64;
using kin::u8;

namespace {

struct Message {
    int id = 0;
    std::vector<u8> bytes;
};

struct State {
    bool restart = false;
    i32 init_result = sf::init_ok;
    bool deck = false;
    int shutdowns = 0;
    int stores = 0;
    int keyboards = 0;
    std::deque<Message> queue;
    Message current;
    std::map<u64, Message> results;
    u64 next_call = 1000;
    std::set<std::string> achieved;
    std::map<std::string, i32> ints;
    std::map<std::string, f32> floats;
    std::map<std::string, std::string> presence;
    std::string last_overlay;
    u32 last_store_app = 0;
    std::string text = "typed on the deck";
    std::vector<i32> uploaded;
} g;

int g_object = 0; // what the interface accessors return
void* object() {
    return &g_object;
}

template <class T> void push(int id, const T& value) {
    Message m{.id = id, .bytes = std::vector<u8>(sizeof(T))};
    std::memcpy(m.bytes.data(), &value, sizeof(T));
    g.queue.push_back(std::move(m));
}

template <class T> u64 call_result(int id, const T& value) {
    const u64 handle = g.next_call++;
    Message m{.id = id, .bytes = std::vector<u8>(sizeof(T))};
    std::memcpy(m.bytes.data(), &value, sizeof(T));
    g.results[handle] = std::move(m);
    push(sf::cb_call_completed,
         sf::SteamAPICallCompleted{.call = handle, .callback = id, .param_size = static_cast<u32>(sizeof(T))});
    return handle;
}

} // namespace

// Steam's functions.

FAKE_API bool SteamAPI_RestartAppIfNecessary(u32) {
    return g.restart;
}
FAKE_API i32 SteamAPI_InitFlat(sf::SteamErrMsg* message) {
    if (g.init_result != sf::init_ok) {
        std::strncpy(*message, "fake: steam is not running", sizeof(*message) - 1);
    }
    return g.init_result;
}
FAKE_API void SteamAPI_Shutdown() {
    ++g.shutdowns;
}
FAKE_API sf::HSteamPipe SteamAPI_GetHSteamPipe() {
    return 1;
}
FAKE_API void SteamAPI_ManualDispatch_Init() {}
FAKE_API void SteamAPI_ManualDispatch_RunFrame(sf::HSteamPipe) {}
FAKE_API bool SteamAPI_ManualDispatch_GetNextCallback(sf::HSteamPipe, sf::CallbackMsg* msg) {
    if (g.queue.empty()) {
        return false;
    }
    g.current = std::move(g.queue.front());
    g.queue.pop_front();
    msg->user = 1;
    msg->callback = g.current.id;
    msg->param = g.current.bytes.data();
    msg->param_size = static_cast<int>(g.current.bytes.size());
    return true;
}
FAKE_API void SteamAPI_ManualDispatch_FreeLastCallback(sf::HSteamPipe) {
    g.current = {};
}
FAKE_API bool SteamAPI_ManualDispatch_GetAPICallResult(sf::HSteamPipe, sf::SteamAPICall call, void* out, int size,
                                                       int id, bool* failed) {
    const auto it = g.results.find(call);
    if (it == g.results.end() || it->second.id != id) {
        return false;
    }
    std::memcpy(out, it->second.bytes.data(), std::min<std::size_t>(size, it->second.bytes.size()));
    *failed = false;
    g.results.erase(it);
    return true;
}

FAKE_API void* SteamAPI_SteamUser_v023() {
    return object();
}
FAKE_API void* SteamAPI_SteamFriends_v018() {
    return object();
}
FAKE_API void* SteamAPI_SteamUtils_v011() {
    return object();
}
FAKE_API void* SteamAPI_SteamApps_v009() {
    return object();
}
FAKE_API void* SteamAPI_SteamUserStats_v012() {
    return object();
} // the older one, which must ask for stats
FAKE_API void* SteamAPI_SteamRemoteStorage_v016() {
    return object();
}

FAKE_API bool SteamAPI_ISteamUser_BLoggedOn(void*) {
    return true;
}
FAKE_API u64 SteamAPI_ISteamUser_GetSteamID(void*) {
    return 76561197960287930ull;
}
FAKE_API const char* SteamAPI_ISteamFriends_GetPersonaName(void*) {
    return "Fake Player";
}
FAKE_API const char* SteamAPI_ISteamFriends_GetFriendPersonaName(void*, u64 id) {
    return id == 2 ? "Second" : "First";
}
FAKE_API bool SteamAPI_ISteamFriends_SetRichPresence(void*, const char* key, const char* value) {
    g.presence[key] = value;
    return true;
}
FAKE_API void SteamAPI_ISteamFriends_ClearRichPresence(void*) {
    g.presence.clear();
}
FAKE_API void SteamAPI_ISteamFriends_ActivateGameOverlay(void*, const char* dialog) {
    g.last_overlay = dialog;
}
FAKE_API void SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage(void*, const char* url, i32) {
    g.last_overlay = std::string{"web:"} + url;
}
FAKE_API void SteamAPI_ISteamFriends_ActivateGameOverlayToStore(void*, u32 app, i32) {
    g.last_store_app = app;
}
FAKE_API void SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog(void*, u64 lobby) {
    g.last_overlay = "invite:" + std::to_string(lobby);
}
FAKE_API u32 SteamAPI_ISteamUtils_GetAppID(void*) {
    return 480;
}
FAKE_API i32 SteamAPI_ISteamUtils_GetSteamHardwareDefaultConfig(void*) {
    return g.deck ? 5 : 0;
}
FAKE_API bool SteamAPI_ISteamUtils_IsSteamInBigPictureMode(void*) {
    return g.deck;
}
FAKE_API bool SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput(void*, i32, int, int, int, int) {
    ++g.keyboards;
    return g.deck;
}
FAKE_API bool SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput(void*) {
    --g.keyboards;
    return true;
}
FAKE_API bool SteamAPI_ISteamUtils_ShowGamepadTextInput(void*, i32, i32, const char*, u32, const char*) {
    return true;
}
FAKE_API u32 SteamAPI_ISteamUtils_GetEnteredGamepadTextLength(void*) {
    return static_cast<u32>(g.text.size());
}
FAKE_API bool SteamAPI_ISteamUtils_GetEnteredGamepadTextInput(void*, char* out, u32 size) {
    if (size < g.text.size() + 1) {
        return false;
    }
    std::memcpy(out, g.text.c_str(), g.text.size() + 1);
    return true;
}
FAKE_API const char* SteamAPI_ISteamApps_GetCurrentGameLanguage(void*) {
    return "brazilian";
}
FAKE_API bool SteamAPI_ISteamApps_BIsSubscribedApp(void*, u32 app) {
    return app == 1234;
}
FAKE_API bool SteamAPI_ISteamApps_BIsDlcInstalled(void*, u32) {
    return false;
}
FAKE_API int SteamAPI_ISteamApps_GetLaunchCommandLine(void*, char*, int) {
    return 0;
}
FAKE_API bool SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount(void*) {
    return true;
}
FAKE_API bool SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp(void*) {
    return true;
}

FAKE_API bool SteamAPI_ISteamUserStats_RequestCurrentStats(void*) {
    push(sf::cb_stats_received, sf::UserStatsReceived{.game_id = 480, .result = sf::result_ok, .user = 1});
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_GetAchievement(void*, const char* name, bool* done) {
    *done = g.achieved.contains(name);
    return std::string_view{name} != "NO_SUCH";
}
FAKE_API bool SteamAPI_ISteamUserStats_SetAchievement(void*, const char* name) {
    if (std::string_view{name} == "NO_SUCH") {
        return false;
    }
    g.achieved.insert(name);
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_ClearAchievement(void*, const char* name) {
    g.achieved.erase(name);
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_IndicateAchievementProgress(void*, const char*, u32, u32) {
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_GetStatInt32(void*, const char* name, i32* out) {
    const auto it = g.ints.find(name);
    if (it == g.ints.end()) {
        return false;
    }
    *out = it->second;
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_GetStatFloat(void*, const char* name, f32* out) {
    const auto it = g.floats.find(name);
    if (it == g.floats.end()) {
        return false;
    }
    *out = it->second;
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_SetStatInt32(void*, const char* name, i32 value) {
    g.ints[name] = value;
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_SetStatFloat(void*, const char* name, f32 value) {
    g.floats[name] = value;
    return true;
}
FAKE_API bool SteamAPI_ISteamUserStats_StoreStats(void*) {
    ++g.stores;
    push(sf::cb_stats_stored, sf::UserStatsStored{.game_id = 480, .result = sf::result_ok});
    for (const std::string& name : g.achieved) {
        sf::UserAchievementStored stored{.game_id = 480};
        std::strncpy(stored.name, name.c_str(), sizeof stored.name - 1);
        push(sf::cb_achievement_stored, stored);
    }
    return true;
}
FAKE_API sf::SteamAPICall SteamAPI_ISteamUserStats_FindOrCreateLeaderboard(void*, const char* name, i32, i32) {
    return call_result(sf::cb_leaderboard_find_result,
                       sf::LeaderboardFindResult{.board = std::string_view{name} == "missing" ? 0u : 77u, .found = 1});
}
FAKE_API sf::SteamAPICall SteamAPI_ISteamUserStats_UploadLeaderboardScore(void*, u64 board, i32, i32 score,
                                                                          const i32* details, int count) {
    g.uploaded.assign(details, details + count);
    g.uploaded.insert(g.uploaded.begin(), score);
    return call_result(sf::cb_leaderboard_score_uploaded, sf::LeaderboardScoreUploaded{.success = 1,
                                                                                       .board = board,
                                                                                       .score = score,
                                                                                       .score_changed = 1,
                                                                                       .global_rank_new = 3,
                                                                                       .global_rank_previous = 9});
}
FAKE_API sf::SteamAPICall SteamAPI_ISteamUserStats_DownloadLeaderboardEntries(void*, u64 board, i32, int, int) {
    return call_result(sf::cb_leaderboard_scores_downloaded,
                       sf::LeaderboardScoresDownloaded{.board = board, .entries = 88, .count = 2});
}
FAKE_API bool SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry(void*, u64 entries, int index,
                                                                     sf::LeaderboardEntry* entry, i32* details,
                                                                     int max) {
    if (entries != 88 || index < 0 || index > 1) {
        return false;
    }
    *entry = {.user = static_cast<u64>(index + 1),
              .global_rank = index + 1,
              .score = 500 - index * 100,
              .details = 1,
              .ugc = 0};
    if (max > 0) {
        details[0] = 7 + index;
    }
    return true;
}

// What the test drives it with.

FAKE_API void kin_fake_steam_reset() {
    g = State{};
}
FAKE_API void kin_fake_steam_set_restart(bool restart) {
    g.restart = restart;
}
FAKE_API void kin_fake_steam_set_init(i32 result) {
    g.init_result = result;
}
FAKE_API void kin_fake_steam_set_deck(bool deck) {
    g.deck = deck;
}
FAKE_API int kin_fake_steam_shutdowns() {
    return g.shutdowns;
}
FAKE_API int kin_fake_steam_stores() {
    return g.stores;
}
FAKE_API int kin_fake_steam_keyboards() {
    return g.keyboards;
}
FAKE_API bool kin_fake_steam_achieved(const char* name) {
    return g.achieved.contains(name);
}
FAKE_API const char* kin_fake_steam_presence(const char* key) {
    const auto it = g.presence.find(key);
    return it == g.presence.end() ? "" : it->second.c_str();
}
FAKE_API const char* kin_fake_steam_last_overlay() {
    return g.last_overlay.c_str();
}
FAKE_API u32 kin_fake_steam_last_store_app() {
    return g.last_store_app;
}
FAKE_API int kin_fake_steam_uploaded(int i) {
    return i < static_cast<int>(g.uploaded.size()) ? g.uploaded[i] : -1;
}
FAKE_API void kin_fake_steam_overlay(bool shown) {
    push(sf::cb_overlay_activated, sf::GameOverlayActivated{.active = shown});
}
FAKE_API void kin_fake_steam_lobby_invite(u64 lobby, u64 friend_id) {
    push(sf::cb_lobby_join_requested, sf::GameLobbyJoinRequested{.lobby = lobby, .friend_id = friend_id});
}
FAKE_API void kin_fake_steam_presence_join(const char* connect) {
    sf::GameRichPresenceJoinRequested join{.friend_id = 5};
    std::strncpy(join.connect, connect, sizeof join.connect - 1);
    push(sf::cb_rich_presence_join_requested, join);
}
FAKE_API void kin_fake_steam_text_dismissed(bool submitted) {
    push(sf::cb_text_input_dismissed,
         sf::GamepadTextInputDismissed{.submitted = submitted, .submitted_length = submitted ? 17u : 0u});
}
FAKE_API void kin_fake_steam_shutdown_signal() {
    push(sf::cb_steam_shutdown, u8{0});
}
