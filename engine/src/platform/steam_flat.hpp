#pragma once

// kin's own declarations of the part of Steam's flat C API (steam_api_flat.h)
// the Steam backend uses. The library (steam_api64.dll, libsteam_api.so,
// libsteam_api.dylib) is opened at run time, so building kin needs no
// Steamworks SDK. tests/steam_abi_check.cpp compares every signature, callback
// id and struct layout here with the SDK's own headers when a build is given
// one (KIN_STEAMWORKS_SDK_DIR).
//
// Interface pointers are void*. Enums are passed as their i32 values. Steam
// ids, leaderboard handles and API call handles are u64.

#include <cstdint>
#include <kin/core/types.hpp>

namespace kin::steam_flat {

using HSteamPipe = i32;
using HSteamUser = i32;
using SteamAPICall = u64;
using SteamErrMsg = char[1024];

// X(field, symbol, return type, parameter types)
// clang-format off
#define KIN_STEAM_FUNCTIONS(X)                                                                                   \
    X(restart_app_if_necessary, SteamAPI_RestartAppIfNecessary, bool, (u32))                                    \
    X(shutdown, SteamAPI_Shutdown, void, ())                                                                     \
    X(get_pipe, SteamAPI_GetHSteamPipe, HSteamPipe, ())                                                          \
    X(dispatch_init, SteamAPI_ManualDispatch_Init, void, ())                                                     \
    X(dispatch_run_frame, SteamAPI_ManualDispatch_RunFrame, void, (HSteamPipe))                                  \
    X(dispatch_next, SteamAPI_ManualDispatch_GetNextCallback, bool, (HSteamPipe, CallbackMsg*))                  \
    X(dispatch_free, SteamAPI_ManualDispatch_FreeLastCallback, void, (HSteamPipe))                               \
    X(dispatch_call_result, SteamAPI_ManualDispatch_GetAPICallResult, bool,                                      \
      (HSteamPipe, SteamAPICall, void*, int, int, bool*))                                                        \
    X(user_logged_on, SteamAPI_ISteamUser_BLoggedOn, bool, (void*))                                              \
    X(user_steam_id, SteamAPI_ISteamUser_GetSteamID, u64, (void*))                                               \
    X(friends_persona_name, SteamAPI_ISteamFriends_GetPersonaName, const char*, (void*))                         \
    X(friends_friend_name, SteamAPI_ISteamFriends_GetFriendPersonaName, const char*, (void*, u64))               \
    X(friends_set_rich_presence, SteamAPI_ISteamFriends_SetRichPresence, bool, (void*, const char*, const char*)) \
    X(friends_clear_rich_presence, SteamAPI_ISteamFriends_ClearRichPresence, void, (void*))                      \
    X(friends_overlay, SteamAPI_ISteamFriends_ActivateGameOverlay, void, (void*, const char*))                   \
    X(friends_overlay_web, SteamAPI_ISteamFriends_ActivateGameOverlayToWebPage, void, (void*, const char*, i32)) \
    X(friends_overlay_store, SteamAPI_ISteamFriends_ActivateGameOverlayToStore, void, (void*, u32, i32))        \
    X(friends_overlay_invite, SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog, void, (void*, u64))       \
    X(utils_app_id, SteamAPI_ISteamUtils_GetAppID, u32, (void*))                                                 \
    X(utils_show_text, SteamAPI_ISteamUtils_ShowGamepadTextInput, bool,                                          \
      (void*, i32, i32, const char*, u32, const char*))                                                          \
    X(utils_text_length, SteamAPI_ISteamUtils_GetEnteredGamepadTextLength, u32, (void*))                         \
    X(utils_text, SteamAPI_ISteamUtils_GetEnteredGamepadTextInput, bool, (void*, char*, u32))                    \
    X(apps_language, SteamAPI_ISteamApps_GetCurrentGameLanguage, const char*, (void*))                           \
    X(apps_subscribed_app, SteamAPI_ISteamApps_BIsSubscribedApp, bool, (void*, u32))                             \
    X(apps_dlc_installed, SteamAPI_ISteamApps_BIsDlcInstalled, bool, (void*, u32))                               \
    X(apps_launch_command_line, SteamAPI_ISteamApps_GetLaunchCommandLine, int, (void*, char*, int))              \
    X(stats_get_achievement, SteamAPI_ISteamUserStats_GetAchievement, bool, (void*, const char*, bool*))         \
    X(stats_set_achievement, SteamAPI_ISteamUserStats_SetAchievement, bool, (void*, const char*))                \
    X(stats_clear_achievement, SteamAPI_ISteamUserStats_ClearAchievement, bool, (void*, const char*))            \
    X(stats_indicate_progress, SteamAPI_ISteamUserStats_IndicateAchievementProgress, bool,                       \
      (void*, const char*, u32, u32))                                                                            \
    X(stats_get_int, SteamAPI_ISteamUserStats_GetStatInt32, bool, (void*, const char*, i32*))                    \
    X(stats_get_float, SteamAPI_ISteamUserStats_GetStatFloat, bool, (void*, const char*, f32*))                  \
    X(stats_set_int, SteamAPI_ISteamUserStats_SetStatInt32, bool, (void*, const char*, i32))                     \
    X(stats_set_float, SteamAPI_ISteamUserStats_SetStatFloat, bool, (void*, const char*, f32))                   \
    X(stats_store, SteamAPI_ISteamUserStats_StoreStats, bool, (void*))                                           \
    X(stats_find_or_create_board, SteamAPI_ISteamUserStats_FindOrCreateLeaderboard, SteamAPICall,               \
      (void*, const char*, i32, i32))                                                                            \
    X(stats_upload_score, SteamAPI_ISteamUserStats_UploadLeaderboardScore, SteamAPICall,                         \
      (void*, u64, i32, i32, const i32*, int))                                                                   \
    X(stats_download_entries, SteamAPI_ISteamUserStats_DownloadLeaderboardEntries, SteamAPICall,                 \
      (void*, u64, i32, int, int))                                                                               \
    X(stats_downloaded_entry, SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry, bool,                      \
      (void*, u64, int, LeaderboardEntry*, i32*, int))

// Used when the library has them (older SDKs lack some), checked against the SDK.
#define KIN_STEAM_OPTIONAL_FUNCTIONS(X)                                                                          \
    X(init_flat, SteamAPI_InitFlat, i32, (SteamErrMsg*))                                                         \
    X(utils_hardware_type, SteamAPI_ISteamUtils_IsRunningOnSteamHardware, i32, (void*))                          \
    X(utils_hardware_config, SteamAPI_ISteamUtils_GetSteamHardwareDefaultConfig, i32, (void*))                   \
    X(utils_big_picture, SteamAPI_ISteamUtils_IsSteamInBigPictureMode, bool, (void*))                            \
    X(utils_show_floating, SteamAPI_ISteamUtils_ShowFloatingGamepadTextInput, bool, (void*, i32, int, int, int, int)) \
    X(utils_dismiss_floating, SteamAPI_ISteamUtils_DismissFloatingGamepadTextInput, bool, (void*))               \
    X(storage_cloud_account, SteamAPI_ISteamRemoteStorage_IsCloudEnabledForAccount, bool, (void*))               \
    X(storage_cloud_app, SteamAPI_ISteamRemoteStorage_IsCloudEnabledForApp, bool, (void*))

// Lobbies (ISteamMatchmaking) and messages between players (ISteamNetworkingMessages): all of them, or the
// backend has neither.
#define KIN_STEAM_NETWORK_FUNCTIONS(X)                                                                           \
    X(mm_request_list, SteamAPI_ISteamMatchmaking_RequestLobbyList, SteamAPICall, (void*))                       \
    X(mm_filter_string, SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter, void,                        \
      (void*, const char*, const char*, i32))                                                                    \
    X(mm_filter_number, SteamAPI_ISteamMatchmaking_AddRequestLobbyListNumericalFilter, void,                     \
      (void*, const char*, int, i32))                                                                            \
    X(mm_filter_slots, SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable, void, (void*, int))   \
    X(mm_filter_distance, SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter, void, (void*, i32))      \
    X(mm_filter_count, SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter, void, (void*, int))      \
    X(mm_lobby_by_index, SteamAPI_ISteamMatchmaking_GetLobbyByIndex, u64, (void*, int))                          \
    X(mm_create, SteamAPI_ISteamMatchmaking_CreateLobby, SteamAPICall, (void*, i32, int))                        \
    X(mm_join, SteamAPI_ISteamMatchmaking_JoinLobby, SteamAPICall, (void*, u64))                                 \
    X(mm_leave, SteamAPI_ISteamMatchmaking_LeaveLobby, void, (void*, u64))                                       \
    X(mm_member_count, SteamAPI_ISteamMatchmaking_GetNumLobbyMembers, int, (void*, u64))                         \
    X(mm_member_by_index, SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex, u64, (void*, u64, int))              \
    X(mm_get_data, SteamAPI_ISteamMatchmaking_GetLobbyData, const char*, (void*, u64, const char*))              \
    X(mm_set_data, SteamAPI_ISteamMatchmaking_SetLobbyData, bool, (void*, u64, const char*, const char*))        \
    X(mm_data_count, SteamAPI_ISteamMatchmaking_GetLobbyDataCount, int, (void*, u64))                            \
    X(mm_data_by_index, SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex, bool,                                    \
      (void*, u64, int, char*, int, char*, int))                                                                 \
    X(mm_get_member_data, SteamAPI_ISteamMatchmaking_GetLobbyMemberData, const char*,                            \
      (void*, u64, u64, const char*))                                                                            \
    X(mm_set_member_data, SteamAPI_ISteamMatchmaking_SetLobbyMemberData, void,                                   \
      (void*, u64, const char*, const char*))                                                                    \
    X(mm_send_chat, SteamAPI_ISteamMatchmaking_SendLobbyChatMsg, bool, (void*, u64, const void*, int))           \
    X(mm_chat_entry, SteamAPI_ISteamMatchmaking_GetLobbyChatEntry, int,                                          \
      (void*, u64, int, SteamId*, void*, int, i32*))                                                             \
    X(mm_member_limit, SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit, int, (void*, u64))                        \
    X(mm_set_joinable, SteamAPI_ISteamMatchmaking_SetLobbyJoinable, bool, (void*, u64, bool))                    \
    X(mm_owner, SteamAPI_ISteamMatchmaking_GetLobbyOwner, u64, (void*, u64))                                     \
    X(msg_send, SteamAPI_ISteamNetworkingMessages_SendMessageToUser, i32,                                        \
      (void*, const NetworkingIdentity&, const void*, u32, int, int))                                            \
    X(msg_receive, SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel, int,                              \
      (void*, int, NetworkingMessage**, int))                                                                    \
    X(msg_accept, SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser, bool,                                 \
      (void*, const NetworkingIdentity&))                                                                        \
    X(msg_close, SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser, bool, (void*, const NetworkingIdentity&)) \
    X(msg_release, SteamAPI_SteamNetworkingMessage_t_Release, void, (NetworkingMessage*))                        \
    X(net_init_relay, SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess, void, (void*))

// Only in libraries from before SDK 1.65, or not declared by its headers (so not checked).
#define KIN_STEAM_LEGACY_FUNCTIONS(X)                                                                            \
    X(init_legacy, SteamAPI_InitSafe, bool, ())                                                                  \
    X(utils_on_deck, SteamAPI_ISteamUtils_IsSteamRunningOnSteamDeck, bool, (void*))                              \
    X(stats_request_current, SteamAPI_ISteamUserStats_RequestCurrentStats, bool, (void*))

// clang-format on

// Interface accessors' versions, newest first: the symbol is
// SteamAPI_<interface>_<version>. A library exports only its own SDK's, and
// the newest here are SDK 1.65's (checked by steam_abi_check.cpp).
inline constexpr const char* user_versions[] = {"v023", "v022", "v021"};
inline constexpr const char* friends_versions[] = {"v018", "v017"};
inline constexpr const char* utils_versions[] = {"v011", "v010"};
inline constexpr const char* apps_versions[] = {"v009", "v008"};
inline constexpr const char* stats_versions[] = {"v013", "v012"};
inline constexpr const char* storage_versions[] = {"v016"};
inline constexpr const char* matchmaking_versions[] = {"v009"};
// SteamAPI_SteamNetworkingMessages_SteamAPI_v002, SteamAPI_SteamNetworkingUtils_SteamAPI_v004.
inline constexpr const char* messages_versions[] = {"v002"};
inline constexpr const char* networking_utils_versions[] = {"v004"};

// ESteamAPIInitResult
inline constexpr i32 init_ok = 0;
inline constexpr i32 init_no_steam_client = 2;
inline constexpr i32 init_version_mismatch = 3;
// EResult
inline constexpr i32 result_ok = 1;
// ESteamHardwareType, ESteamHardwareDefaultConfig
inline constexpr i32 hardware_steam_deck = 1;
inline constexpr i32 hardware_config_steam_deck = 5;
// ELeaderboardSortMethod, ELeaderboardDisplayType, ELeaderboardUploadScoreMethod, ELeaderboardDataRequest
inline constexpr i32 sort_ascending = 1;
inline constexpr i32 sort_descending = 2;
inline constexpr i32 display_numeric = 1;
inline constexpr i32 display_seconds = 2;
inline constexpr i32 display_milliseconds = 3;
inline constexpr i32 upload_keep_best = 1;
inline constexpr i32 upload_force_update = 2;
inline constexpr i32 request_global = 0;
inline constexpr i32 request_around_user = 1;
inline constexpr i32 request_friends = 2;
// ELobbyType is LobbyVisibility's values, ELobbyComparison LobbyCompare's, ELobbyDistanceFilter LobbyDistance's.
// EChatRoomEnterResponse, EChatMemberStateChange, EChatEntryType
inline constexpr u32 enter_success = 1;
inline constexpr u32 member_entered = 0x1;
inline constexpr u32 member_gone = 0x2 | 0x4 | 0x8 | 0x10; // left, disconnected, kicked, banned
inline constexpr i32 chat_message = 1;
// ESteamNetworkingIdentityType
inline constexpr i32 identity_steam_id = 16;
// k_nSteamNetworkingSend_*
inline constexpr int send_unreliable = 0;
inline constexpr int send_no_nagle = 1;
inline constexpr int send_reliable = 8;
inline constexpr int send_auto_restart = 32;
// EFloatingGamepadTextInputMode: SingleLine, MultipleLines, Email, Numeric (0-3).
// EGamepadTextInputMode Normal 0, Password 1; EGamepadTextInputLineMode SingleLine 0.

// Callback ids (k_iCallback).
inline constexpr int cb_servers_connected = 101;
inline constexpr int cb_servers_disconnected = 103;
inline constexpr int cb_overlay_activated = 331;
inline constexpr int cb_lobby_join_requested = 333;
inline constexpr int cb_rich_presence_join_requested = 337;
inline constexpr int cb_lobby_enter = 504;
inline constexpr int cb_lobby_data_update = 505;
inline constexpr int cb_lobby_chat_update = 506;
inline constexpr int cb_lobby_chat_msg = 507;
inline constexpr int cb_lobby_match_list = 510;
inline constexpr int cb_lobby_kicked = 512;
inline constexpr int cb_lobby_created = 513;
inline constexpr int cb_call_completed = 703;
inline constexpr int cb_steam_shutdown = 704;
inline constexpr int cb_text_input_dismissed = 714;
inline constexpr int cb_floating_text_input_dismissed = 738;
inline constexpr int cb_dlc_installed = 1005;
inline constexpr int cb_stats_received = 1101;
inline constexpr int cb_stats_stored = 1102;
inline constexpr int cb_achievement_stored = 1103;
inline constexpr int cb_leaderboard_find_result = 1104;
inline constexpr int cb_leaderboard_scores_downloaded = 1105;
inline constexpr int cb_leaderboard_score_uploaded = 1106;
inline constexpr int cb_messages_session_request = 1251;
inline constexpr int cb_messages_session_failed = 1252;

// SteamNetworkingIdentity: packed to 1 byte everywhere.
#pragma pack(push, 1)
struct NetworkingIdentity {
    i32 type;
    i32 size;
    union {
        u64 steam_id;
        u8 raw[128];
    };
};
#pragma pack(pop)

// CSteamID, as GetLobbyChatEntry writes it.
struct SteamId {
    u64 id;
};

// SteamNetworkingMessage_t: the platform's own alignment, not the callbacks' packing.
struct NetworkingMessage {
    void* data;
    int size;
    u32 connection;
    NetworkingIdentity peer;
    i64 connection_user_data;
    i64 time_received;
    i64 message_number;
    void (*free_data)(NetworkingMessage*);
    void (*release)(NetworkingMessage*);
    int channel;
    int flags;
    i64 user_data;
    u16 lane;
};

// Steam packs its callback structs to 4 bytes on Linux and macOS, 8 elsewhere.
#if defined(__linux__) || defined(__APPLE__)
#pragma pack(push, 4)
#else
#pragma pack(push, 8)
#endif

struct CallbackMsg {
    HSteamUser user;
    int callback;
    u8* param;
    int param_size;
};

struct SteamAPICallCompleted {
    SteamAPICall call;
    int callback;
    u32 param_size;
};

struct GameOverlayActivated {
    u8 active;
};

struct GameLobbyJoinRequested {
    u64 lobby;
    u64 friend_id;
};

struct GameRichPresenceJoinRequested {
    u64 friend_id;
    char connect[256];
};

struct GamepadTextInputDismissed {
    bool submitted;
    u32 submitted_length;
};

struct DlcInstalled {
    u32 app_id;
};

struct UserStatsReceived {
    u64 game_id;
    i32 result;
    u64 user;
};

struct UserStatsStored {
    u64 game_id;
    i32 result;
};

struct UserAchievementStored {
    u64 game_id;
    bool group_achievement;
    char name[128];
    u32 current_progress;
    u32 max_progress;
};

struct LeaderboardFindResult {
    u64 board;
    u8 found;
};

struct LeaderboardScoresDownloaded {
    u64 board;
    u64 entries;
    int count;
};

struct LeaderboardScoreUploaded {
    u8 success;
    u64 board;
    i32 score;
    u8 score_changed;
    int global_rank_new;
    int global_rank_previous;
};

struct LobbyEnter {
    u64 lobby;
    u32 permissions;
    bool locked;
    u32 response;
};

struct LobbyDataUpdate {
    u64 lobby;
    u64 member;
    u8 success;
};

struct LobbyChatUpdate {
    u64 lobby;
    u64 changed;
    u64 making_change;
    u32 state_change;
};

struct LobbyChatMsg {
    u64 lobby;
    u64 user;
    u8 type;
    u32 chat_id;
};

struct LobbyMatchList {
    u32 count;
};

struct LobbyKicked {
    u64 lobby;
    u64 admin;
    u8 due_to_disconnect;
};

struct LobbyCreated {
    i32 result;
    u64 lobby;
};

struct MessagesSessionRequest {
    NetworkingIdentity remote;
};

// SteamNetConnectionInfo_t starts with the remote identity; kin reads only that and the end reason.
struct MessagesSessionFailed {
    NetworkingIdentity remote;
    i64 user_data;
    u32 listen_socket;
    u8 address[18];
    u16 pad;
    u32 pop_remote;
    u32 pop_relay;
    i32 state;
    i32 end_reason;
};

struct LeaderboardEntry {
    u64 user;
    i32 global_rank;
    i32 score;
    i32 details;
    u64 ugc;
};

#pragma pack(pop)

// field_fn: each function's pointer type.
#define KIN_STEAM_TYPE(field, symbol, ret, params) using field##_fn = ret(*) params;
KIN_STEAM_FUNCTIONS(KIN_STEAM_TYPE)
KIN_STEAM_OPTIONAL_FUNCTIONS(KIN_STEAM_TYPE)
KIN_STEAM_NETWORK_FUNCTIONS(KIN_STEAM_TYPE)
KIN_STEAM_LEGACY_FUNCTIONS(KIN_STEAM_TYPE)
#undef KIN_STEAM_TYPE

// The library's file name here.
#if defined(_WIN32)
#if defined(_WIN64)
inline constexpr const char* library_name = "steam_api64.dll";
#else
inline constexpr const char* library_name = "steam_api.dll";
#endif
#elif defined(__APPLE__)
inline constexpr const char* library_name = "libsteam_api.dylib";
#else
inline constexpr const char* library_name = "libsteam_api.so";
#endif

} // namespace kin::steam_flat
