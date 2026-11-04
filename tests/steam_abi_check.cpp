// kin's Steam declarations (engine/src/platform/steam_flat.hpp) against the
// Steamworks SDK's headers: compiled only when the build is given the SDK
// (-DKIN_STEAMWORKS_SDK_DIR=<sdk>); it fails to compile where they differ.
// Interface pointers and enums are compared by kind and size, since kin passes
// them as void* and i32.

#include "steam_flat.hpp"

#include <cstddef>
#include <string_view>
#include <kin/platform/services.hpp>
#include <steam/steam_api.h>
#include <steam/steam_api_flat.h>
#include <type_traits>

namespace {

namespace sf = kin::steam_flat;

template <std::size_t Size> struct Int {};
struct Pointer {};
struct Bool {};

template <class T> struct norm {
    using type = T;
};
template <class T>
    requires(std::is_integral_v<T> && !std::is_same_v<T, bool>) || std::is_enum_v<T>
struct norm<T> {
    using type = Int<sizeof(T)>;
};
template <> struct norm<bool> {
    using type = Bool;
};
template <class T>
    requires std::is_class_v<std::remove_cv_t<T>> || std::is_void_v<T>
struct norm<T*> {
    using type = Pointer;
};
template <class R, class... A> struct norm<R (*)(A...)> {
    using type = typename norm<R>::type (*)(typename norm<A>::type...);
};

template <class Ours, class Theirs>
inline constexpr bool same_call = std::is_same_v<typename norm<Ours>::type, typename norm<Theirs>::type>;

#define KIN_STEAM_CHECK(field, symbol, ret, params)                                                                    \
    static_assert(same_call<sf::field##_fn, decltype(&::symbol)>, #symbol " differs from the SDK's");

KIN_STEAM_FUNCTIONS(KIN_STEAM_CHECK)
KIN_STEAM_OPTIONAL_FUNCTIONS(KIN_STEAM_CHECK)
#undef KIN_STEAM_CHECK

// The newest interface versions kin asks for are the SDK's (each library
// exports only its own).
static_assert(std::is_same_v<decltype(&SteamAPI_SteamUser_v023), ISteamUser* (*)()>);
static_assert(std::is_same_v<decltype(&SteamAPI_SteamFriends_v018), ISteamFriends* (*)()>);
static_assert(std::is_same_v<decltype(&SteamAPI_SteamUtils_v011), ISteamUtils* (*)()>);
static_assert(std::is_same_v<decltype(&SteamAPI_SteamApps_v009), ISteamApps* (*)()>);
static_assert(std::is_same_v<decltype(&SteamAPI_SteamUserStats_v013), ISteamUserStats* (*)()>);
static_assert(std::is_same_v<decltype(&SteamAPI_SteamRemoteStorage_v016), ISteamRemoteStorage* (*)()>);
static_assert(std::string_view{sf::user_versions[0]} == "v023");
static_assert(std::string_view{sf::friends_versions[0]} == "v018");
static_assert(std::string_view{sf::utils_versions[0]} == "v011");
static_assert(std::string_view{sf::apps_versions[0]} == "v009");
static_assert(std::string_view{sf::stats_versions[0]} == "v013");
static_assert(std::string_view{sf::storage_versions[0]} == "v016");

static_assert(sf::init_ok == k_ESteamAPIInitResult_OK);
static_assert(sf::init_no_steam_client == k_ESteamAPIInitResult_NoSteamClient);
static_assert(sf::init_version_mismatch == k_ESteamAPIInitResult_VersionMismatch);
static_assert(sf::result_ok == k_EResultOK);
static_assert(sf::hardware_steam_deck == k_ESteamHardwareTypeSteamDeck);
static_assert(sf::hardware_config_steam_deck == k_ESteamHardwareDefaultConfigSteamDeck);
static_assert(sf::sort_ascending == k_ELeaderboardSortMethodAscending);
static_assert(sf::sort_descending == k_ELeaderboardSortMethodDescending);
static_assert(sf::display_numeric == k_ELeaderboardDisplayTypeNumeric);
static_assert(sf::display_seconds == k_ELeaderboardDisplayTypeTimeSeconds);
static_assert(sf::display_milliseconds == k_ELeaderboardDisplayTypeTimeMilliSeconds);
static_assert(sf::upload_keep_best == k_ELeaderboardUploadScoreMethodKeepBest);
static_assert(sf::upload_force_update == k_ELeaderboardUploadScoreMethodForceUpdate);
static_assert(sf::request_global == k_ELeaderboardDataRequestGlobal);
static_assert(sf::request_around_user == k_ELeaderboardDataRequestGlobalAroundUser);
static_assert(sf::request_friends == k_ELeaderboardDataRequestFriends);
// KeyboardMode is passed to ShowFloatingGamepadTextInput as it is.
static_assert(static_cast<int>(kin::KeyboardMode::SingleLine) == k_EFloatingGamepadTextInputModeModeSingleLine);
static_assert(static_cast<int>(kin::KeyboardMode::MultipleLines) == k_EFloatingGamepadTextInputModeModeMultipleLines);
static_assert(static_cast<int>(kin::KeyboardMode::Email) == k_EFloatingGamepadTextInputModeModeEmail);
static_assert(static_cast<int>(kin::KeyboardMode::Numeric) == k_EFloatingGamepadTextInputModeModeNumeric);
static_assert(k_EGamepadTextInputModePassword == 1);
static_assert(k_EGamepadTextInputLineModeSingleLine == 0);

static_assert(sf::cb_servers_connected == SteamServersConnected_t::k_iCallback);
static_assert(sf::cb_servers_disconnected == SteamServersDisconnected_t::k_iCallback);
static_assert(sf::cb_overlay_activated == GameOverlayActivated_t::k_iCallback);
static_assert(sf::cb_lobby_join_requested == GameLobbyJoinRequested_t::k_iCallback);
static_assert(sf::cb_rich_presence_join_requested == GameRichPresenceJoinRequested_t::k_iCallback);
static_assert(sf::cb_call_completed == SteamAPICallCompleted_t::k_iCallback);
static_assert(sf::cb_steam_shutdown == SteamShutdown_t::k_iCallback);
static_assert(sf::cb_text_input_dismissed == GamepadTextInputDismissed_t::k_iCallback);
static_assert(sf::cb_floating_text_input_dismissed == FloatingGamepadTextInputDismissed_t::k_iCallback);
static_assert(sf::cb_dlc_installed == DlcInstalled_t::k_iCallback);
static_assert(sf::cb_stats_received == UserStatsReceived_t::k_iCallback);
static_assert(sf::cb_stats_stored == UserStatsStored_t::k_iCallback);
static_assert(sf::cb_achievement_stored == UserAchievementStored_t::k_iCallback);
static_assert(sf::cb_leaderboard_find_result == LeaderboardFindResult_t::k_iCallback);
static_assert(sf::cb_leaderboard_scores_downloaded == LeaderboardScoresDownloaded_t::k_iCallback);
static_assert(sf::cb_leaderboard_score_uploaded == LeaderboardScoreUploaded_t::k_iCallback);

// Layouts: kin reads the first sizeof(ours) bytes, so its fields must sit where
// the SDK's do and its struct must not be longer (CallbackMsg and
// LeaderboardEntry are written by Steam, so those must be as long).
#define KIN_SAME_FIELD(ours, theirs, a, b)                                                                             \
    static_assert(offsetof(sf::ours, a) == offsetof(theirs, b), #ours "::" #a " is not where the SDK has it");
#define KIN_NOT_LONGER(ours, theirs)                                                                                   \
    static_assert(sizeof(sf::ours) <= sizeof(theirs), #ours " is longer than the SDK's");

KIN_SAME_FIELD(CallbackMsg, CallbackMsg_t, callback, m_iCallback)
KIN_SAME_FIELD(CallbackMsg, CallbackMsg_t, param, m_pubParam)
KIN_SAME_FIELD(CallbackMsg, CallbackMsg_t, param_size, m_cubParam)
static_assert(sizeof(sf::CallbackMsg) + 32 >= sizeof(CallbackMsg_t));
KIN_SAME_FIELD(SteamAPICallCompleted, SteamAPICallCompleted_t, call, m_hAsyncCall)
KIN_SAME_FIELD(SteamAPICallCompleted, SteamAPICallCompleted_t, callback, m_iCallback)
KIN_SAME_FIELD(SteamAPICallCompleted, SteamAPICallCompleted_t, param_size, m_cubParam)
KIN_NOT_LONGER(SteamAPICallCompleted, SteamAPICallCompleted_t)
KIN_SAME_FIELD(GameOverlayActivated, GameOverlayActivated_t, active, m_bActive)
KIN_SAME_FIELD(GameLobbyJoinRequested, GameLobbyJoinRequested_t, lobby, m_steamIDLobby)
KIN_SAME_FIELD(GameLobbyJoinRequested, GameLobbyJoinRequested_t, friend_id, m_steamIDFriend)
KIN_SAME_FIELD(GameRichPresenceJoinRequested, GameRichPresenceJoinRequested_t, friend_id, m_steamIDFriend)
KIN_SAME_FIELD(GameRichPresenceJoinRequested, GameRichPresenceJoinRequested_t, connect, m_rgchConnect)
KIN_NOT_LONGER(GameRichPresenceJoinRequested, GameRichPresenceJoinRequested_t)
KIN_SAME_FIELD(GamepadTextInputDismissed, GamepadTextInputDismissed_t, submitted, m_bSubmitted)
KIN_SAME_FIELD(GamepadTextInputDismissed, GamepadTextInputDismissed_t, submitted_length, m_unSubmittedText)
KIN_SAME_FIELD(DlcInstalled, DlcInstalled_t, app_id, m_nAppID)
KIN_SAME_FIELD(UserStatsReceived, UserStatsReceived_t, game_id, m_nGameID)
KIN_SAME_FIELD(UserStatsReceived, UserStatsReceived_t, result, m_eResult)
KIN_SAME_FIELD(UserStatsReceived, UserStatsReceived_t, user, m_steamIDUser)
KIN_SAME_FIELD(UserStatsStored, UserStatsStored_t, game_id, m_nGameID)
KIN_SAME_FIELD(UserStatsStored, UserStatsStored_t, result, m_eResult)
KIN_SAME_FIELD(UserAchievementStored, UserAchievementStored_t, game_id, m_nGameID)
KIN_SAME_FIELD(UserAchievementStored, UserAchievementStored_t, name, m_rgchAchievementName)
KIN_SAME_FIELD(UserAchievementStored, UserAchievementStored_t, current_progress, m_nCurProgress)
KIN_SAME_FIELD(UserAchievementStored, UserAchievementStored_t, max_progress, m_nMaxProgress)
KIN_SAME_FIELD(LeaderboardFindResult, LeaderboardFindResult_t, board, m_hSteamLeaderboard)
KIN_SAME_FIELD(LeaderboardFindResult, LeaderboardFindResult_t, found, m_bLeaderboardFound)
KIN_SAME_FIELD(LeaderboardScoresDownloaded, LeaderboardScoresDownloaded_t, board, m_hSteamLeaderboard)
KIN_SAME_FIELD(LeaderboardScoresDownloaded, LeaderboardScoresDownloaded_t, entries, m_hSteamLeaderboardEntries)
KIN_SAME_FIELD(LeaderboardScoresDownloaded, LeaderboardScoresDownloaded_t, count, m_cEntryCount)
KIN_SAME_FIELD(LeaderboardScoreUploaded, LeaderboardScoreUploaded_t, success, m_bSuccess)
KIN_SAME_FIELD(LeaderboardScoreUploaded, LeaderboardScoreUploaded_t, board, m_hSteamLeaderboard)
KIN_SAME_FIELD(LeaderboardScoreUploaded, LeaderboardScoreUploaded_t, score, m_nScore)
KIN_SAME_FIELD(LeaderboardScoreUploaded, LeaderboardScoreUploaded_t, score_changed, m_bScoreChanged)
KIN_SAME_FIELD(LeaderboardScoreUploaded, LeaderboardScoreUploaded_t, global_rank_new, m_nGlobalRankNew)
KIN_SAME_FIELD(LeaderboardEntry, LeaderboardEntry_t, user, m_steamIDUser)
KIN_SAME_FIELD(LeaderboardEntry, LeaderboardEntry_t, global_rank, m_nGlobalRank)
KIN_SAME_FIELD(LeaderboardEntry, LeaderboardEntry_t, score, m_nScore)
KIN_SAME_FIELD(LeaderboardEntry, LeaderboardEntry_t, details, m_cDetails)
KIN_SAME_FIELD(LeaderboardEntry, LeaderboardEntry_t, ugc, m_hUGC)
static_assert(sizeof(sf::LeaderboardEntry) == sizeof(LeaderboardEntry_t));

} // namespace
