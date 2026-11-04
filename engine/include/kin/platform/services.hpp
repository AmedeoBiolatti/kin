#pragma once

// What a store or platform offers a game, the same whichever one runs it
// (docs/platform_services.md): the player, their language, achievements and
// stats, leaderboards, rich presence, the overlay, the on-screen keyboard, DLC
// ownership and invitations to join a friend.
//
//     kin::ServicesConfig config{.game_id = "lantern_vale", .steam_app_id = 480};
//     kin::ServicesStart start = kin::ServicesStart::Ready;
//     std::unique_ptr<kin::PlatformServices> services = kin::start_platform_services(config, &start);
//     if (start == kin::ServicesStart::Restarting) return 0;  // Steam starts the game again itself
//     ...each frame:
//     services->update();
//     for (const kin::ServiceEvent& event : services->events()) { ... }
//     services->unlock("FIRST_WIN");
//
// Every call works with every backend. Where the platform is missing (no
// Steam, a headless run, a build for itch.io) the offline backend answers: it
// keeps achievements and stats in the player's save folder, and what only a
// platform can do (the overlay, friends) does nothing and says so by returning
// false. A game never needs to ask which backend it has.
//
// run_scene_app starts them from SceneAppConfig::services, calls update() once
// a frame and passes them to scenes (SceneContext::services) and scripts.

#include <cstdint>
#include <filesystem>
#include <kin/core/types.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct ServicesConfig {
    // The game's id (GameInfo::id): the offline backend keeps its file in
    // user_data_dir("kin")/<game_id>/services/, beside its saves. Empty: it keeps
    // everything in memory only.
    std::string game_id{};
    // Steam: the game's app id (480 is Valve's test app, Spacewar). 0: Steam is
    // not tried.
    u32 steam_app_id = 0;
    // Started outside Steam, ask Steam to start the game itself (and quit:
    // ServicesStart::Restarting). Steam skips this while a steam_appid.txt is
    // beside the executable, as kin_game writes in development builds.
    bool steam_restart_through_client = true;
    // Where the Steam API library is (a file, or a folder holding
    // steam_api64.dll / libsteam_api.so / libsteam_api.dylib). Empty: beside
    // the executable, then the system's search path.
    std::filesystem::path steam_library{};
    // Only the offline backend, whatever else is set: headless, server and
    // determinism runs, which must not depend on the machine.
    bool offline_only = false;
    // The offline backend keeps nothing on disk (tests, headless runs).
    bool offline_in_memory = false;
    // The game's command line: Steam starts a game to join a friend with
    // "+connect_lobby <id>" or "+connect <text>" (join_on_launch).
    std::vector<std::string> args{};
};

enum class ServicesStart : u8 {
    Ready,      // a platform answered (backend() says which)
    Offline,    // none did: the offline backend answers
    Restarting, // the platform starts the game again itself: quit now
};

struct ServicePlayer {
    // "steam:76561197960287930", "offline", ...: stable for the player.
    std::string id{};
    std::string name{};
};

enum class LeaderboardSort : u8 { Descending, Ascending }; // which scores are best
enum class LeaderboardDisplay : u8 { Number, Seconds, Milliseconds };
enum class LeaderboardUpload : u8 { KeepBest, Replace };
enum class LeaderboardRange : u8 { Top, AroundPlayer, Friends };

struct LeaderboardEntry {
    std::string player_id{};
    std::string player_name{};
    i32 rank = 0; // 1 is first
    i32 score = 0;
    std::vector<i32> details{};
};

enum class KeyboardMode : u8 { SingleLine, MultipleLines, Email, Numeric };

// Where the overlay opens.
enum class OverlayPage : u8 { Friends, Community, Players, Settings, Achievements, Stats };

enum class ServiceEventKind : u8 {
    OverlayShown, // pause the game while it is up
    OverlayHidden,
    KeyboardClosed,      // the floating keyboard went away (its text was typed as key presses)
    TextEntered,         // ask_text's answer: text
    TextCancelled,       // ask_text was dismissed
    JoinRequested,       // the player accepted an invitation or chose "join game": text is what to join
                         // ("lobby:<id>", or the connect string a friend set with set_join_command)
    AchievementUnlocked, // id: an unlock reached the platform
    StatsStored,         // stats and achievements were kept
    StatsFailed,
    LeaderboardScores,   // request: what request_scores returned; entries
    LeaderboardUploaded, // request: what upload_score returned; ok, rank (new global rank), changed
    LeaderboardFailed,   // request: which request failed; text: why
    DlcInstalled,        // id: the DLC
    Disconnected,        // the platform went away (Steam shut down, the player went offline)
    Reconnected,
};

struct ServiceEvent {
    ServiceEventKind kind{};
    std::string id{};
    std::string text{};
    u32 request = 0;
    bool ok = false;
    bool changed = false;
    i32 rank = 0;
    std::vector<LeaderboardEntry> entries{};
};

class PlatformServices {
public:
    virtual ~PlatformServices() = default;

    // "steam", "offline", "fake".
    virtual std::string_view backend() const = 0;
    // A platform is there and reachable (false for offline).
    virtual bool online() const = 0;

    // Once a frame: takes what the platform sent since and makes this frame's
    // events(); keeps stats and achievements when they changed (at most every
    // couple of seconds, as platforms ask).
    virtual void update() = 0;
    // What happened since the update before; cleared by the next update().
    virtual std::span<const ServiceEvent> events() const = 0;
    // Keep stats and achievements now (at exit, at a checkpoint).
    virtual void flush() = 0;

    virtual ServicePlayer player() const = 0;
    // The language the player chose for the game on the platform, as a BCP 47
    // tag ("fr", "pt-BR", "zh-Hans"); "" when it does not say.
    virtual std::string language() const = 0;
    // Played on a handheld such as Steam Deck: show controller hints, the
    // on-screen keyboard, bigger text.
    virtual bool handheld() const = 0;
    // Played from a TV-style launcher (Steam Big Picture): likely a controller.
    virtual bool big_picture() const = 0;
    // The platform keeps the player's saves (Steam Cloud). Auto-Cloud needs no
    // code: it copies the save folder (user_data_dir("kin")/<game_id>) as set
    // on the platform.
    virtual bool cloud_saves() const = 0;

    // Achievements, by the id set on the platform. false: unknown id, or not kept.
    virtual bool unlock(std::string_view achievement) = 0;
    virtual bool clear(std::string_view achievement) = 0;
    virtual std::optional<bool> unlocked(std::string_view achievement) const = 0;
    // Shows the player how far along an achievement is (3 of 10 chests).
    virtual bool show_progress(std::string_view achievement, u32 current, u32 max) = 0;

    // Stats: whole-number or decimal, as set on the platform.
    virtual bool set_stat(std::string_view stat, i32 value) = 0;
    virtual bool set_stat(std::string_view stat, f32 value) = 0;
    virtual std::optional<i32> stat_int(std::string_view stat) const = 0;
    virtual std::optional<f32> stat_float(std::string_view stat) const = 0;
    bool add_stat(std::string_view stat, i32 delta) { return set_stat(stat, stat_int(stat).value_or(0) + delta); }

    // Leaderboards (made on first use where the platform allows). Each returns
    // a request number (0: not sent) that its LeaderboardUploaded/Scores/Failed
    // event names.
    virtual u32 upload_score(std::string_view board, i32 score, std::span<const i32> details = {},
                             LeaderboardUpload upload = LeaderboardUpload::KeepBest,
                             LeaderboardSort sort = LeaderboardSort::Descending,
                             LeaderboardDisplay display = LeaderboardDisplay::Number) = 0;
    // Entries first..last (1-based ranks; around the player: offsets, -4..5).
    virtual u32 request_scores(std::string_view board, LeaderboardRange range, i32 first, i32 last) = 0;

    // Rich presence: what friends see ("status": "Exploring the vale").
    // Steam also reads "steam_display" (a localization token) and "connect"
    // (the command friends join with): see set_join_command.
    virtual bool set_presence(std::string_view key, std::string_view value) = 0;
    virtual void clear_presence() = 0;
    // What a friend who chooses "join game" gets in their JoinRequested event.
    bool set_join_command(std::string_view command) { return set_presence("connect", command); }

    // The overlay: true when it is up (pause the game).
    virtual bool overlay_active() const = 0;
    virtual bool open_overlay(OverlayPage page) = 0;
    virtual bool open_url(std::string_view url) = 0;
    // The game's store page (dlc 0) or a DLC's.
    virtual bool open_store(std::string_view dlc = {}) = 0;
    // Invite friends to a lobby (its id as JoinRequested gives it).
    virtual bool invite_friends(std::string_view lobby) = 0;

    // The floating on-screen keyboard over `field` (window pixels), for a text
    // field that has focus: its keys arrive as typing. false: none here.
    virtual bool show_keyboard(KeyboardMode mode, Rectf field) = 0;
    virtual void hide_keyboard() = 0;
    // A full-screen text prompt; the answer comes as TextEntered/TextCancelled.
    virtual bool ask_text(std::string_view prompt, std::string_view initial, u32 max_chars, bool password) = 0;

    // DLC, by its platform id (a Steam app id).
    virtual bool owns(std::string_view dlc) const = 0;
    // What the game was started to join ("lobby:<id>" or a connect string), or "".
    virtual std::string join_on_launch() const = 0;
};

// Starts the services `config` allows: Steam when an app id is set and Steam
// answers, else the offline backend. `start` (optional) says which happened.
std::unique_ptr<PlatformServices> start_platform_services(const ServicesConfig& config, ServicesStart* start = nullptr);
// Only the offline backend.
std::unique_ptr<PlatformServices> make_offline_services(const ServicesConfig& config);

// The services the running game uses (set by run_scene_app), or null.
PlatformServices* active_services();
void set_active_services(PlatformServices* services);

// Steam's language names ("french", "brazilian", "schinese") as BCP 47 tags.
std::string language_from_steam(std::string_view steam_language);

// Services for tests: does what it is told and records what the game asked.
class FakeServices final : public PlatformServices {
public:
    struct Call {
        std::string what; // "unlock", "set_stat", "presence", "upload_score", "open_store", ...
        std::string id{};
        std::string value{};
    };

    std::string_view backend() const override { return "fake"; }
    bool online() const override { return online_; }
    void update() override;
    std::span<const ServiceEvent> events() const override { return _events; }
    void flush() override { calls.push_back({"flush"}); }
    ServicePlayer player() const override { return player_; }
    std::string language() const override { return language_; }
    bool handheld() const override { return handheld_; }
    bool big_picture() const override { return big_picture_; }
    bool cloud_saves() const override { return cloud_; }
    bool unlock(std::string_view achievement) override;
    bool clear(std::string_view achievement) override;
    std::optional<bool> unlocked(std::string_view achievement) const override;
    bool show_progress(std::string_view achievement, u32 current, u32 max) override;
    bool set_stat(std::string_view stat, i32 value) override;
    bool set_stat(std::string_view stat, f32 value) override;
    std::optional<i32> stat_int(std::string_view stat) const override;
    std::optional<f32> stat_float(std::string_view stat) const override;
    u32 upload_score(std::string_view board, i32 score, std::span<const i32> details, LeaderboardUpload upload,
                     LeaderboardSort sort, LeaderboardDisplay display) override;
    u32 request_scores(std::string_view board, LeaderboardRange range, i32 first, i32 last) override;
    bool set_presence(std::string_view key, std::string_view value) override;
    void clear_presence() override;
    bool overlay_active() const override { return overlay_; }
    bool open_overlay(OverlayPage page) override;
    bool open_url(std::string_view url) override;
    bool open_store(std::string_view dlc) override;
    bool invite_friends(std::string_view lobby) override;
    bool show_keyboard(KeyboardMode mode, Rectf field) override;
    void hide_keyboard() override;
    bool ask_text(std::string_view prompt, std::string_view initial, u32 max_chars, bool password) override;
    bool owns(std::string_view dlc) const override;
    std::string join_on_launch() const override { return join_on_launch_; }

    // What the test sets up.
    bool online_ = true;
    ServicePlayer player_{.id = "fake:1", .name = "Player"};
    std::string language_{};
    bool handheld_ = false;
    bool big_picture_ = false;
    bool cloud_ = false;
    std::vector<std::string> achievements{}; // the ids that exist (empty: any)
    std::vector<std::string> dlc{};          // owned
    std::string join_on_launch_{};
    // Sent with the next update().
    void send(ServiceEvent event) { _pending.push_back(std::move(event)); }
    void set_overlay(bool shown);
    // What the game did, in order.
    std::vector<Call> calls{};
    std::vector<std::pair<std::string, std::string>> presence{};
    bool keyboard_shown = false;

private:
    struct Stat {
        std::string id;
        f64 value = 0.0;
    };
    std::vector<std::string> _unlocked;
    std::vector<Stat> _stats;
    std::vector<ServiceEvent> _events;
    std::vector<ServiceEvent> _pending;
    u32 _next_request = 1;
    bool overlay_ = false;
};

} // namespace kin
