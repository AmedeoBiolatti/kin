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
// EFloatingGamepadTextInputMode: SingleLine, MultipleLines, Email, Numeric (0-3).
// EGamepadTextInputMode Normal 0, Password 1; EGamepadTextInputLineMode SingleLine 0.

// Callback ids (k_iCallback).
inline constexpr int cb_servers_connected = 101;
inline constexpr int cb_servers_disconnected = 103;
inline constexpr int cb_overlay_activated = 331;
inline constexpr int cb_lobby_join_requested = 333;
inline constexpr int cb_rich_presence_join_requested = 337;
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
