#pragma once

// A route follower for Summit (docs/3d_plan.md, P4a): a small state machine over the level's route legs that reads the
// game's state and answers with a PlayerCommand each step: walk to the foot of a wall, take hold, climb, pull up, rest
// for stamina, climb the rope, reach the top. It is the "player" of the scripted run and of the tests; its commands are
// recorded and replayed as a log (the replay needs no bot). It only reads the Game it is given.

#include "game.hpp"

#include <string>
#include <vector>

namespace summit {

struct BotConfig {
    bool sheltered_route = false;
    // Rests until this share of the stamina cap before a climb.
    f32 rest_to = 0.97f;
    // Gives up on a leg after this many steps without leaving it.
    i32 leg_timeout = 6000;
    // Finished once this leg (an index of the route) is done (-1: at the end).
    i32 stop_after_leg = -1;
    // On this leg it lets go (the release command for a few steps) once the player's feet are `drop_height` above the
    // leg's start: a scripted fall (-1: never).
    i32 drop_leg = -1;
    f32 drop_height = 5.0f;
};

class Bot {
public:
    explicit Bot(const Game& game, i32 player = 0, BotConfig config = {});

    // The command for the next step, from the game as it is now.
    PlayerCommand next();
    bool finished() const {
        return _leg >= i32(_route.size()) || (_config.stop_after_leg >= 0 && _leg > _config.stop_after_leg);
    }
    i32 leg() const { return _leg; }
    // What it is doing, for logs.
    std::string status() const;
    // Starts at a leg (a test that puts the player at the foot of the third pitch).
    void start_at(i32 leg);
    // The sub-state's name ("climb", "shimmy", "rope"...).
    const char* phase() const;
    i32 restarts() const { return _restarts; }

private:
    enum class Sub : u8 { Start, Rest, Face, Approach, Climb, Shimmy, Climb2, Mantle, Rope, Handover, Step, Done };
    void advance_leg();
    void aim(PlayerCommand& c, f32 yaw, f32 pitch) const;
    f32 yaw_error(f32 yaw) const;

    const Game* _game;
    i32 _player;
    BotConfig _config;
    std::vector<Leg> _route;
    i32 _leg = 0;
    Sub _sub = Sub::Start;
    i32 _leg_steps = 0; // steps in this leg
    i32 _sub_steps = 0; // steps in this sub-state
    i32 _stuck = 0;
    Vec3f _last_feet{};
    i32 _restarts = 0;
    f32 _origin_lateral = 0.0f;
    Vec3f _mark{};
    i32 _hand = 0;
    i32 _hand_steps = 0;
    i32 _retry = 0;
    i32 _drops = 0; // steps of release still to send
    bool _dropped = false;
    bool _falling = false;
};

// What a scripted run reports.
struct RunReport {
    bool summit = false;
    u64 steps = 0;
    u64 final_hash = 0;
    i32 falls = 0, stuns = 0, deaths = 0;
    i32 summit_step = -1;
    bool non_finite = false;
    f32 min_stamina = 1e9f;
    f32 highest = 0.0f;
    i32 legs_done = 0;
    std::vector<PlayerCommand> log;
};

// Plays `bot` against `game` for at most `max_steps` (stopping at the summit when `stop_at_summit`), recording the
// commands.
RunReport run_bot(Game& game, Bot& bot, u64 max_steps, bool stop_at_summit = true);

// Plays a recorded log (one command a step, player 0; other players' commands default) and returns the final hash.
u64 replay(Game& game, const std::vector<PlayerCommand>& log);

// A command log as a file (the same bytes on every machine): seed, count, then each command's numbers and flags. False
// when the file cannot be written or read, is not a log, or is cut short.
bool write_command_log(const std::string& path, u64 seed, const std::vector<PlayerCommand>& log,
                       bool expedition = false, bool dense_scenery = false);
bool read_command_log(const std::string& path, u64& seed, std::vector<PlayerCommand>& log, bool* expedition = nullptr,
                      bool* dense_scenery = nullptr);

// The bot's settings for a scripted run by name: "climb" (walk and climb to the summit), "fall" (the same, but it lets
// go near the top of the chimney once: a 5 m fall, a knock-out, a get-up, then it climbs on), "rope" (stops once it is
// over the rope). False for another name.
bool script_config(const std::string& name, const SummitLevel& level, BotConfig& out);

} // namespace summit
