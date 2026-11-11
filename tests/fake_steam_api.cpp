// A stand-in for Steam's API library (libsteam_api.so / steam_api64.dll) for
// platform_services_tests: it exports the flat C functions kin's Steam backend
// opens, with the same names, and kin_fake_steam_* functions the test drives
// it with. It follows kin's own declarations (steam_flat.hpp), so it tests how
// the backend loads, dispatches and answers, not Steam's real layout: that is
// steam_abi_check.cpp's job, against the SDK.

#include "steam_flat.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
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
    // Lobbies and messages.
    struct Lobby {
        u64 id = 0;
        u64 owner = 0;
        std::vector<u64> members;
        int limit = 4;
        bool joinable = true;
        std::map<std::string, std::string> data;
        std::map<std::pair<u64, std::string>, std::string> member_data;
    };
    std::map<u64, Lobby> lobbies;
    u64 next_lobby = 109775240000000001ull;
    std::vector<u64> listed; // the last RequestLobbyList's answer
    std::string filters;     // what the last request was filtered by
    std::vector<std::string> chat;
    struct Sent {
        u64 to = 0;
        std::vector<u8> bytes;
        int flags = 0;
        int channel = 0;
    };
    std::vector<Sent> sent;
    std::deque<std::pair<int, sf::NetworkingMessage*>> inbox; // channel, message
    std::set<u64> accepted, closed;
    std::map<u64, std::string> names; // players whose names are not "First" (or "Second")
    std::set<u64> requested;          // RequestUserInformation was called
    int released = 0;
    int relays = 0;
} g;

constexpr u64 self_id = 76561197960287930ull;

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
FAKE_API void* SteamAPI_SteamMatchmaking_v009() {
    return object();
}
FAKE_API void* SteamAPI_SteamNetworkingMessages_SteamAPI_v002() {
    return object();
}
FAKE_API void* SteamAPI_SteamNetworkingUtils_SteamAPI_v004() {
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
    if (const auto it = g.names.find(id); it != g.names.end()) {
        return it->second.c_str(); // "[unknown]" until the persona arrives, as Steam has it
    }
    return id == 2 ? "Second" : "First";
}
// True: Steam is fetching the name, and a PersonaStateChange_t follows.
FAKE_API bool SteamAPI_ISteamFriends_RequestUserInformation(void*, u64 id, bool) {
    const auto it = g.names.find(id);
    if (it == g.names.end() || it->second != "[unknown]") {
        return false;
    }
    g.requested.insert(id);
    return true;
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

// Lobbies.

namespace {
State::Lobby* lobby_of(u64 id) {
    const auto it = g.lobbies.find(id);
    return it == g.lobbies.end() ? nullptr : &it->second;
}
} // namespace

FAKE_API sf::SteamAPICall SteamAPI_ISteamMatchmaking_RequestLobbyList(void*) {
    g.listed.clear();
    for (const auto& [id, lobby] : g.lobbies) {
        if (lobby.joinable && std::ranges::find(lobby.members, self_id) == lobby.members.end()) {
            g.listed.push_back(id);
        }
    }
    return call_result(sf::cb_lobby_match_list, sf::LobbyMatchList{.count = static_cast<u32>(g.listed.size())});
}
FAKE_API void SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(void*, const char* key, const char* value,
                                                                         i32 compare) {
    g.filters += std::string{key} + (compare == 0 ? "=" : "?") + value + ";";
}
FAKE_API void SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter(void*, const char* key, int value,
                                                                            i32 compare) {
    g.filters += std::string{key} + (compare == 2 ? ">=" : "#") + std::to_string(value) + ";";
}
FAKE_API void SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable(void*, int slots) {
    g.filters += "slots" + std::to_string(slots) + ";";
}
FAKE_API void SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(void*, i32 distance) {
    g.filters += "distance" + std::to_string(distance) + ";";
}
FAKE_API void SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter(void*, int count) {
    g.filters += "count" + std::to_string(count) + ";";
}
FAKE_API u64 SteamAPI_ISteamMatchmaking_GetLobbyByIndex(void*, int i) {
    return i >= 0 && i < static_cast<int>(g.listed.size()) ? g.listed[i] : 0;
}
FAKE_API sf::SteamAPICall SteamAPI_ISteamMatchmaking_CreateLobby(void*, i32, int max) {
    const u64 id = g.next_lobby++;
    g.lobbies[id] = {.id = id, .owner = self_id, .members = {self_id}, .limit = max};
    const sf::SteamAPICall call =
        call_result(sf::cb_lobby_created, sf::LobbyCreated{.result = sf::result_ok, .lobby = id});
    push(sf::cb_lobby_enter, sf::LobbyEnter{.lobby = id, .response = sf::enter_success});
    return call;
}
FAKE_API sf::SteamAPICall SteamAPI_ISteamMatchmaking_JoinLobby(void*, u64 id) {
    State::Lobby* lobby = lobby_of(id);
    u32 response = sf::enter_success;
    if (!lobby) {
        response = 2;
    } else if (static_cast<int>(lobby->members.size()) >= lobby->limit) {
        response = 4;
    } else {
        lobby->members.push_back(self_id);
        push(sf::cb_lobby_enter, sf::LobbyEnter{.lobby = id, .response = response});
    }
    return call_result(sf::cb_lobby_enter, sf::LobbyEnter{.lobby = id, .response = response});
}
FAKE_API void SteamAPI_ISteamMatchmaking_LeaveLobby(void*, u64 id) {
    if (State::Lobby* lobby = lobby_of(id)) {
        std::erase(lobby->members, self_id);
        if (lobby->owner == self_id && !lobby->members.empty()) {
            lobby->owner = lobby->members.front();
        }
    }
}
FAKE_API int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void*, u64 id) {
    const State::Lobby* lobby = lobby_of(id);
    return lobby ? static_cast<int>(lobby->members.size()) : 0;
}
FAKE_API u64 SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void*, u64 id, int i) {
    const State::Lobby* lobby = lobby_of(id);
    return lobby && i >= 0 && i < static_cast<int>(lobby->members.size()) ? lobby->members[i] : 0;
}
FAKE_API const char* SteamAPI_ISteamMatchmaking_GetLobbyData(void*, u64 id, const char* key) {
    const State::Lobby* lobby = lobby_of(id);
    const auto it = lobby ? lobby->data.find(key) : decltype(lobby->data)::const_iterator{};
    return lobby && it != lobby->data.end() ? it->second.c_str() : "";
}
FAKE_API bool SteamAPI_ISteamMatchmaking_SetLobbyData(void*, u64 id, const char* key, const char* value) {
    State::Lobby* lobby = lobby_of(id);
    if (!lobby || lobby->owner != self_id) {
        return false;
    }
    lobby->data[key] = value;
    push(sf::cb_lobby_data_update, sf::LobbyDataUpdate{.lobby = id, .member = id, .success = 1});
    return true;
}
FAKE_API int SteamAPI_ISteamMatchmaking_GetLobbyDataCount(void*, u64 id) {
    const State::Lobby* lobby = lobby_of(id);
    return lobby ? static_cast<int>(lobby->data.size()) : 0;
}
FAKE_API bool SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex(void*, u64 id, int i, char* key, int key_size, char* value,
                                                             int value_size) {
    const State::Lobby* lobby = lobby_of(id);
    if (!lobby || i < 0 || i >= static_cast<int>(lobby->data.size())) {
        return false;
    }
    auto it = std::next(lobby->data.begin(), i);
    std::snprintf(key, key_size, "%s", it->first.c_str());
    std::snprintf(value, value_size, "%s", it->second.c_str());
    return true;
}
FAKE_API bool SteamAPI_ISteamMatchmaking_DeleteLobbyData(void*, u64, const char*) {
    return false;
}
FAKE_API const char* SteamAPI_ISteamMatchmaking_GetLobbyMemberData(void*, u64 id, u64 user, const char* key) {
    const State::Lobby* lobby = lobby_of(id);
    if (!lobby) {
        return "";
    }
    const auto it = lobby->member_data.find({user, key});
    return it != lobby->member_data.end() ? it->second.c_str() : "";
}
FAKE_API void SteamAPI_ISteamMatchmaking_SetLobbyMemberData(void*, u64 id, const char* key, const char* value) {
    if (State::Lobby* lobby = lobby_of(id)) {
        lobby->member_data[{self_id, key}] = value;
        push(sf::cb_lobby_data_update, sf::LobbyDataUpdate{.lobby = id, .member = self_id, .success = 1});
    }
}
FAKE_API bool SteamAPI_ISteamMatchmaking_SendLobbyChatMsg(void*, u64 id, const void* body, int size) {
    if (!lobby_of(id) || size <= 0) {
        return false;
    }
    g.chat.emplace_back(static_cast<const char*>(body), static_cast<std::size_t>(size));
    push(sf::cb_lobby_chat_msg, sf::LobbyChatMsg{.lobby = id,
                                                 .user = self_id,
                                                 .type = static_cast<u8>(sf::chat_message),
                                                 .chat_id = static_cast<u32>(g.chat.size() - 1)});
    return true;
}
FAKE_API int SteamAPI_ISteamMatchmaking_GetLobbyChatEntry(void*, u64, int chat, sf::SteamId* user, void* out, int size,
                                                          i32* type) {
    if (chat < 0 || chat >= static_cast<int>(g.chat.size())) {
        return 0;
    }
    const std::string& line = g.chat[chat];
    const int n = std::min(size, static_cast<int>(line.size()));
    std::memcpy(out, line.data(), static_cast<std::size_t>(n));
    user->id = self_id;
    *type = sf::chat_message;
    return n;
}
FAKE_API int SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit(void*, u64 id) {
    const State::Lobby* lobby = lobby_of(id);
    return lobby ? lobby->limit : 0;
}
FAKE_API bool SteamAPI_ISteamMatchmaking_SetLobbyJoinable(void*, u64 id, bool joinable) {
    State::Lobby* lobby = lobby_of(id);
    if (!lobby || lobby->owner != self_id) {
        return false;
    }
    lobby->joinable = joinable;
    return true;
}
FAKE_API u64 SteamAPI_ISteamMatchmaking_GetLobbyOwner(void*, u64 id) {
    const State::Lobby* lobby = lobby_of(id);
    return lobby ? lobby->owner : 0;
}

// Messages.

namespace {
void release_message(sf::NetworkingMessage* message) {
    ++g.released;
    delete[] static_cast<u8*>(message->data);
    delete message;
}
} // namespace

FAKE_API i32 SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void*, const sf::NetworkingIdentity& to,
                                                                 const void* data, u32 size, int flags, int channel) {
    if (to.type != sf::identity_steam_id || to.size != 8 || to.steam_id == 0) {
        return 8; // k_EResultInvalidParam
    }
    const auto* bytes = static_cast<const u8*>(data);
    g.sent.push_back({.to = to.steam_id, .bytes = {bytes, bytes + size}, .flags = flags, .channel = channel});
    return sf::result_ok;
}
FAKE_API int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void*, int channel, sf::NetworkingMessage** out,
                                                                        int max) {
    int n = 0;
    for (auto it = g.inbox.begin(); it != g.inbox.end() && n < max;) {
        if (it->first == channel) {
            out[n++] = it->second;
            it = g.inbox.erase(it);
        } else {
            ++it;
        }
    }
    return n;
}
FAKE_API bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void*, const sf::NetworkingIdentity& who) {
    g.accepted.insert(who.steam_id);
    return true;
}
FAKE_API bool SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(void*, const sf::NetworkingIdentity& who) {
    g.closed.insert(who.steam_id);
    return true;
}
FAKE_API void SteamAPI_SteamNetworkingMessage_t_Release(sf::NetworkingMessage* message) {
    message->release(message);
}
FAKE_API void SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(void*) {
    ++g.relays;
}

// What the test drives it with.

FAKE_API void kin_fake_steam_reset() {
    for (auto& [channel, message] : g.inbox) {
        release_message(message);
    }
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
FAKE_API void kin_fake_steam_add_lobby(u64 id, u64 owner, int limit, const char* key, const char* value) {
    g.lobbies[id] = {.id = id, .owner = owner, .members = {owner}, .limit = limit, .data = {{key, value}}};
}
FAKE_API void kin_fake_steam_member_joins(u64 lobby, u64 user) {
    if (State::Lobby* l = lobby_of(lobby)) {
        l->members.push_back(user);
        push(sf::cb_lobby_chat_update,
             sf::LobbyChatUpdate{
                 .lobby = lobby, .changed = user, .making_change = user, .state_change = sf::member_entered});
    }
}
FAKE_API void kin_fake_steam_member_leaves(u64 lobby, u64 user) {
    if (State::Lobby* l = lobby_of(lobby)) {
        std::erase(l->members, user);
        if (l->owner == user && !l->members.empty()) {
            l->owner = l->members.front();
        }
        push(sf::cb_lobby_chat_update,
             sf::LobbyChatUpdate{.lobby = lobby, .changed = user, .making_change = user, .state_change = 0x4});
    }
}
// How many members a lobby has on "Steam's" side (a ghost member counts).
FAKE_API int kin_fake_steam_member_count(u64 lobby) {
    const State::Lobby* l = lobby_of(lobby);
    return l ? static_cast<int>(l->members.size()) : -1;
}
// Takes a member out without telling anyone (as when the player was offline).
FAKE_API void kin_fake_steam_drop_member(u64 lobby, u64 user) {
    if (State::Lobby* l = lobby_of(lobby)) {
        std::erase(l->members, user);
    }
}
// The connection to Steam's servers goes away, or comes back.
FAKE_API void kin_fake_steam_connection(bool connected) {
    push(connected ? sf::cb_servers_connected : sf::cb_servers_disconnected, u8{0});
}
// A player Steam has no name for yet; then the name arriving (PersonaStateChange_t), if it was asked for.
FAKE_API void kin_fake_steam_unnamed(u64 user) {
    g.names[user] = "[unknown]";
}
FAKE_API bool kin_fake_steam_name_arrives(u64 user, const char* name) {
    g.names[user] = name;
    push(sf::cb_persona_state_change, sf::PersonaStateChange{.steam_id = user, .change_flags = 0x0001});
    return g.requested.contains(user);
}
FAKE_API void kin_fake_steam_kicked(u64 lobby) {
    push(sf::cb_lobby_kicked, sf::LobbyKicked{.lobby = lobby, .admin = lobby, .due_to_disconnect = 1});
}
FAKE_API const char* kin_fake_steam_filters() {
    return g.filters.c_str();
}
FAKE_API int kin_fake_steam_sent_count() {
    return static_cast<int>(g.sent.size());
}
// The i-th message sent: its bytes into `out` (returns their count), and who, how and on which channel.
FAKE_API int kin_fake_steam_sent(int i, u8* out, int size, u64* to, int* flags, int* channel) {
    if (i < 0 || i >= static_cast<int>(g.sent.size())) {
        return -1;
    }
    const State::Sent& m = g.sent[i];
    std::memcpy(out, m.bytes.data(), std::min<std::size_t>(size, m.bytes.size()));
    *to = m.to;
    *flags = m.flags;
    *channel = m.channel;
    return static_cast<int>(m.bytes.size());
}
FAKE_API void kin_fake_steam_message_from(u64 user, int channel, const u8* data, int size) {
    auto* message = new sf::NetworkingMessage{};
    message->data = new u8[static_cast<std::size_t>(size)];
    std::memcpy(message->data, data, static_cast<std::size_t>(size));
    message->size = size;
    message->peer.type = sf::identity_steam_id;
    message->peer.size = 8;
    message->peer.steam_id = user;
    message->channel = channel;
    message->release = release_message;
    g.inbox.emplace_back(channel, message);
}
FAKE_API int kin_fake_steam_released() {
    return g.released;
}
FAKE_API int kin_fake_steam_relays() {
    return g.relays;
}
FAKE_API void kin_fake_steam_session_request(u64 user) {
    sf::MessagesSessionRequest request{};
    request.remote.type = sf::identity_steam_id;
    request.remote.size = 8;
    request.remote.steam_id = user;
    push(sf::cb_messages_session_request, request);
}
FAKE_API bool kin_fake_steam_accepted(u64 user) {
    return g.accepted.contains(user);
}
FAKE_API bool kin_fake_steam_closed(u64 user) {
    return g.closed.contains(user);
}
FAKE_API void kin_fake_steam_session_failed(u64 user, i32 reason) {
    sf::MessagesSessionFailed failed{};
    failed.remote.type = sf::identity_steam_id;
    failed.remote.size = 8;
    failed.remote.steam_id = user;
    failed.end_reason = reason;
    push(sf::cb_messages_session_failed, failed);
}
FAKE_API void kin_fake_steam_shutdown_signal() {
    push(sf::cb_steam_shutdown, u8{0});
}
