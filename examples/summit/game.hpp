#pragma once

// Summit's simulation (docs/3d_plan.md, P4a and P5a): everything that decides what happens, with no window, no clock and no
// renderer. A `Game` advances by one fixed step (60 Hz) per call to step(), from plain-data commands, one per player:
//
//     summit::Game game(seed);
//     std::array<summit::PlayerCommand, 1> commands;       // from summit::InputMapper, a bot, a replay or the network
//     game.step(commands);
//     game.state_hash(); game.save(); game.restore(bytes);
//
// What step() reads is the commands and the state; what it writes is the state (and the events of that step). It reads no clock,
// no device, no global random number (the one RNG is a key in the state) and nothing of rendering. A view (the example's scene,
// the camera, the HUD of P4b, the sound) reads the state through the const accessors and writes nothing back: look direction
// enters as commands.
//
// The state is a World3D (Jolt: bodies, the characters, ropes and ragdolls), `std::vector<Player>`, the RNG key, the wind and the
// step counter; state_hash(), save() and restore() cover all of it.
//
// Determinism: the same level seed, the same commands, the same bits (checked on this machine, one thread or a pool). The look
// and facing use std::sin and std::atan2, so cross-platform equality is Jolt's claim and not this file's.

#include "level.hpp"
#include "props.hpp"

#include <kin/core/rng.hpp>
#include <kin/physics3d/world3d.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

namespace summit {
using namespace kin;

inline constexpr f32 fixed_step = 1.0f / 60.0f;

// What a player asks of the game in one step. Plain data, held by whoever makes it.
struct PlayerCommand {
    Vec2f move{};   // x right, y forward, each -1 to 1 (the view's frame)
    Vec2f look{};   // degrees turned this step: x to the right, y up
    bool jump = false;       // a level; also jumps off a grip
    bool sprint = false;
    bool grab_left = false;  // a level: held, the hand holds; released, it lets go
    bool grab_right = false;
    bool release = false;    // let go of everything
    bool pull_up = false;    // up and over a lip (also done by itself when both hands are on one and forward is pressed)
    bool interact = false;   // rest at a camp

    friend bool operator==(const PlayerCommand&, const PlayerCommand&) = default;
};

// The numbers of the player model, in one place (the design table of docs/3d_plan.md "P4 - Summit - design").
struct PlayerRules {
    // Walking.
    f32 walk_speed = 4.0f;
    f32 sprint_speed = 7.0f;
    f32 jump_speed = 5.0f;
    f32 turn_rate = 720.0f;  // degrees a second the body turns to face the way it goes
    // Stamina.
    f32 max_stamina = 150.0f;
    f32 regen_rate = 25.0f;  // units a second, after `regen_delay` seconds without spending
    f32 regen_delay = 1.0f;
    f32 drain_sprint = 15.0f;
    f32 jump_cost = 6.0f;
    // Grips.
    f32 reach = 0.95f;
    f32 climb_speed = 0.8f;
    f32 shimmy_speed = 1.0f;
    f32 drain_two_hands = 7.0f;
    f32 drain_one_hand = 12.0f;
    f32 drain_moving = 4.0f;
    f32 grab_cost = 2.0f;
    f32 pull_up_cost = 8.0f;
    f32 pull_up_min = 8.0f;
    // Health and falls.
    f32 max_health = 100.0f;
    f32 safe_landing = 7.0f;        // m/s: a landing at or under it costs nothing
    f32 damage_per_speed = 7.0f;    // health for each m/s above it
    f32 stun_speed = 9.0f;          // m/s (a drop of about 4 m): knocked out
    f32 stun_seconds = 1.5f;        // the least it lies
    f32 get_up_seconds = 0.8f;
    f32 stun_timeout = 12.0f;       // it lies no longer than this (no room, or no ground)
    f32 respawn_seconds = 2.0f;     // dead, before it is at the camp again
    f32 health_regen = 0.5f;        // a second, standing still on the ground
    f32 camp_health_regen = 10.0f;  // a second, in a camp
    f32 injury_stamina_floor = 0.5f; // the stamina cap at 0 health, as a share of max_stamina
    // Looking.
    f32 pitch_min = -80.0f;
    f32 pitch_max = 80.0f;
};

enum class PlayerState : u8 {
    Alive,
    Stunned,   // a ragdoll, hurt
    GettingUp, // the clip blending back in; commands are not read
    Dead,      // a ragdoll until it respawns
};

enum class ExpeditionPhase : u8 { Briefing, Climbing, Extraction, Complete };
enum class ExpeditionInteraction : u8 { None, Begin, Beacon, Extract, Recover };
inline constexpr i32 beacon_hold_steps = 120;
inline constexpr i32 extraction_hold_steps = 60;

struct ExpeditionState {
    ExpeditionPhase phase = ExpeditionPhase::Briefing;
    i32 start_step = -1, finish_step = -1;
    i32 beacon_steps = 0, extraction_steps = 0;
    bool shelter_visited = false;
};

struct Player {
    Character3D character{};
    RagdollHandle3D ragdoll{};
    PlayerState state = PlayerState::Alive;
    f32 state_time = 0.0f;      // seconds in the state
    f32 yaw = 0.0f;             // where it looks, degrees about +Y (0 looks along -Z), pitch up positive
    f32 pitch = 0.0f;
    f32 facing_yaw = 0.0f;      // which way the body faces (the person model faces +Z at 0)
    f32 health = 100.0f;
    i32 camp = 0;               // the last camp reached: where a respawn puts it
    i32 falls = 0;              // times it let go and fell (any distance)
    i32 stuns = 0;
    i32 deaths = 0;
    bool summit = false;
    i32 summit_step = -1;       // the step it first stood on the top
    f32 air_speed = 0.0f;       // fastest it fell in this fall (m/s, positive down): for the ragdoll's start
    Vec3f air_velocity{};       // its velocity the step before it landed
    f32 best_altitude = 0.0f;
    bool pull_up_sent = false;  // the level sent last step (an edge is made by alternating it)
    f32 last_stamina_cap = 0.0f;
    PlayerCommand command{};    // the last one read
    ExpeditionState expedition;
};

// Wind as state (nothing in the simulation depends on it yet: P4b's sound and snow read it).
struct Wind {
    f32 speed = 3.0f;   // m/s
    f32 direction = 0.0f; // degrees about +Y
    f32 target_speed = 3.0f;
    f32 target_direction = 0.0f;
};

enum class GameEventKind : u8 {
    Jumped,
    Landed,     // value: the speed (m/s)
    Gripped,
    Released,
    Fell,       // lost its grips and fell
    Exhausted,
    Mantled,    // pulled up over a lip
    Stunned,
    GotUp,
    Died,
    Respawned,
    Camp,       // value: the camp's number
    Summit,
    Hurt,       // value: health lost
    PullUpBlocked, // value: why (MantleBlock3D: 0 no ledge, 1 no room, 2 tired)
    ExpeditionStarted,
    ShelterReached,
    BeaconActivated,
};

struct GameEvent {
    u64 step = 0;
    i32 player = 0;
    GameEventKind kind = GameEventKind::Landed;
    Vec3f point{};
    f32 value = 0.0f;
};

struct GameConfig {
    PlayerRules rules{};
    u32 players = 1;
    // Where the world's steps run: false uses the engine's job pool (the same bits).
    bool single_threaded = true;
    // Player i starts this far to the side of the camp, so that two do not share a place.
    f32 spawn_spacing = 1.2f;
    bool expedition = false;
    bool dense_scenery = false;
};

// A decor chunk's collider: the convex hull of its mesh's vertices, in the body's own space (origin at the chunk's centre, turned
// with its rotation). Invalid only for a degenerate chunk.
Shape3D decor_shape(const SummitLevel& level, const DecorChunk& chunk);

class Game {
public:
    explicit Game(u64 seed, GameConfig config = {});
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // One fixed step. `commands[i]` is player i's; missing ones are the default command (nothing pressed).
    void step(std::span<const PlayerCommand> commands);

    // --- the state ---
    u64 state_hash() const;
    std::vector<u8> save() const;
    // False (and nothing changed) for bytes of another seed, another player count, a wrong version or damage.
    bool restore(std::span<const u8> bytes);

    // --- test hooks (not commands: a network layer would never send these; they are state, so they hash and save) ---
    // Puts a player's feet somewhere, at rest (a drop from a height, the foot of the rope).
    void debug_teleport(i32 player, Vec3f feet);
    void debug_set_health(i32 player, f32 health);

    // --- reading it ---
    u64 seed() const { return _level.seed; }
    const SummitLevel& level() const { return _level; }
    const GameConfig& config() const { return _config; }
    const PlayerRules& rules() const { return _config.rules; }
    const World3D& world() const { return *_world; }
    u64 step_count() const { return _step; }
    f64 time() const { return f64(_step) * f64(fixed_step); }
    const Wind& wind() const { return _wind; }
    RngKey rng() const { return _rng; }
    std::span<const Player> players() const { return _players; }
    const Player& player(i32 i) const { return _players[std::size_t(i)]; }
    // Of the last step() (cleared by the next).
    std::span<const GameEvent> events() const { return _events; }

    CharacterState3D character_state(i32 player) const;
    TraversalState3D traversal(i32 player) const;
    // The stamina left, its cap now (injury lowers it), and the same as shares.
    f32 stamina(i32 player) const;
    f32 stamina_cap(i32 player) const;
    // Feet, and the head (where the camera looks): for a ragdoll the pelvis.
    Vec3f feet(i32 player) const;
    Vec3f head(i32 player) const;
    Vec3f look_direction(i32 player) const;
    // Feet above base camp.
    f32 altitude(i32 player) const;
    // The body of piece `i` and of the rope's `i`-th segment, for the view.
    Body3D piece_body(std::size_t i) const { return _piece_bodies[i]; }
    // The decor chunks (decor_chunks(level)) and the static body of each, in the list's order. User id = pieces + 1 + index.
    std::span<const DecorChunk> decor() const { return _decor; }
    Body3D decor_body(std::size_t i) const { return _decor_bodies[i]; }
    std::span<const PropPlacement> props() const { return _props; }
    Body3D prop_body(std::size_t i) const { return _prop_bodies[i]; }
    std::span<const RopeHandle3D> ropes() const { return _ropes; }
    // The ragdoll's bones as the pose writer needs them (empty while it is animated).
    void ragdoll_pose(i32 player, std::vector<RagdollBonePose3D>& out) const;
    // The skeleton joint of each ragdoll bone (the ragdoll's desc), for the presentation's pose override.
    std::span<const i32> ragdoll_joints() const { return _ragdoll_joints; }
    u32 skeleton_joints() const { return _skeleton_joints; }
    const std::vector<std::string>& skeleton_names() const { return _skeleton_names; }
    ExpeditionInteraction interaction(i32 player = 0) const;

private:
    struct Impl;
    void build();
    void step_player(i32 index, const PlayerCommand& command);
    void after_player(i32 index);
    void step_expedition(i32 index);
    void begin_stun(i32 index, Vec3f velocity, bool dead);
    bool try_get_up(i32 index);
    void respawn(i32 index);
    void emit(i32 player, GameEventKind kind, Vec3f point, f32 value = 0.0f);
    std::vector<Mat4f> rest_bones(Vec3f feet, f32 facing_yaw) const;
    void serialise(std::vector<u8>& out) const;

    SummitLevel _level;
    GameConfig _config;
    std::unique_ptr<World3D> _world;
    std::vector<Body3D> _piece_bodies;
    std::vector<PropPlacement> _props;
    std::vector<Body3D> _prop_bodies;
    std::vector<DecorChunk> _decor;
    std::vector<Body3D> _decor_bodies;
    std::vector<RopeHandle3D> _ropes;
    std::vector<Player> _players;
    RngKey _rng{};
    Wind _wind{};
    u64 _step = 0;
    std::vector<GameEvent> _events;
    // The person's skeleton (CPU only): its rest pose placed gives the ragdoll its start.
    std::unique_ptr<Impl> _impl;
    std::vector<i32> _ragdoll_joints;
    std::vector<std::string> _skeleton_names;
    u32 _skeleton_joints = 0;
};

// Frame times in, whole fixed steps out: what the app loop does, as arithmetic, so that a test can feed one command log through
// 30, 60 and 144 frames a second and get the same game.
class FrameStepper {
public:
    // Steps that are due after a frame of `frame_seconds` (the remainder is kept).
    u32 advance(f64 frame_seconds) {
        _accumulator += frame_seconds;
        const f64 n = std::floor(_accumulator / f64(fixed_step) + 1e-9);
        _accumulator -= n * f64(fixed_step);
        return u32(n);
    }
    // 0 to 1: how far the next step is (what the renderer interpolates by).
    f32 alpha() const { return f32(std::clamp(_accumulator / f64(fixed_step), 0.0, 1.0)); }

private:
    f64 _accumulator = 0.0;
};

// The viewing direction of yaw and pitch (degrees; 0, 0 is -Z, up positive), and its horizontal part.
Vec3f look_vector(f32 yaw, f32 pitch);

} // namespace summit
