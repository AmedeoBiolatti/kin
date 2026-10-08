#pragma once

// Signal Breach (docs/3d_plan.md, M2): Signal Siege's fight seen from inside,
// first person. Its own simulation beside Signal Siege's: the same three
// enemy kinds (star, plate, ring) lifted off the floor, cores to collect, a
// 90-second run; the player walks a deck of cells at a few heights, jumps,
// dashes and fires along the view. Deterministic from its seed; drawing only
// reads it. Built with KIN_EXPERIMENTAL_3D only.

#include "example_common.hpp"

#include <kin/core/jobs.hpp>
#include <kin/core/math3d.hpp>
#include <kin/core/rng.hpp>
#include <kin/ecs/world.hpp>

#include <kin/ecs/render3d.hpp>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace examples {

// The deck: a grid of one-metre cells, each a floor at a height, a wall, or
// a ramp between two heights. Up is +y; the grid's x and z are the world's.
class BreachLevel {
public:
    static constexpr int width = 48, depth = 32;
    static constexpr float wall_height = 3.0f;
    // Ground up to this much higher than the feet is stepped onto.
    static constexpr float step = 0.45f;

    enum class Kind : u8 { Floor, Wall, Ramp };
    struct Cell {
        Kind kind = Kind::Floor;
        float low = 0.0f, high = 0.0f; // a floor's height (both), a ramp's ends
        u8 axis = 0;                   // a ramp rises along: 0 +x, 1 -x, 2 +z, 3 -z
    };

    BreachLevel();
    const Cell& at(int x, int z) const { return _cells[static_cast<std::size_t>(z * width + x)]; }
    bool inside(int x, int z) const { return x >= 0 && z >= 0 && x < width && z < depth; }
    // The ground's height at a point (a wall: its top).
    float ground(float x, float z) const;
    // Whether a circle at (x, z) of `radius` with its feet at `feet` runs into
    // a wall or ground too high to step onto.
    bool blocked(float x, float z, float radius, float feet) const;
    // How far along `direction` (length 1) a ray from `origin` first meets the
    // deck or a wall, up to `max`; nothing when it meets neither.
    std::optional<float> raycast(Vec3f origin, Vec3f direction, float max) const;
    bool line_of_sight(Vec3f a, Vec3f b) const;

private:
    std::vector<Cell> _cells;
};

struct BreachInput {
    Vec2f move{};            // x right, y forward, each -1..1
    float yaw = 0, pitch = 0; // the view, degrees (Quatf::yaw_pitch: yaw 0 looks along -z)
    bool fire = false, jump = false, dash = false;
};

// An enemy (an ECS component, with Transform3D): Signal Siege's kinds, 0 a
// star that rams, 1 a plate that keeps its distance and lobs slow shots, 2 a
// ring that circles and fires.
struct BreachEnemy {
    int slot = 0, kind = 0;
    float hp = 1, cooldown = 0, spin = 0;
};
struct BreachShot { Vec3f pos{}, velocity{}; float life = 0; };
struct BreachPickup { Vec3f pos{}; float life = 15; };

// Cues for what draws it (the simulation never reads them).
struct BreachEvent {
    enum class Kind : u8 { Hit, Kill, Pickup, Hurt, Fire, EnemyFire, Jump };
    Kind kind = Kind::Hit;
    Vec3f pos{};      // where (a shot: the muzzle)
    Vec3f to{};       // a shot: where it hit
    int enemy_kind = 0;
};

class Breach {
public:
    static constexpr int max_enemies = 100000;
    static constexpr float radius = 0.35f, height = 1.7f, eye = 1.55f;
    explicit Breach(int enemies = 300, u64 seed = 7);
    ~Breach();
    void reset(u64 seed);
    void step(float dt, BreachInput input, bool invincible = false);
    BreachInput autopilot() const;
    u64 checksum() const;

    kin::EcsWorld world;
    Vec3f player{};          // the feet
    Vec3f velocity{};
    float yaw = 0, pitch = 0; // the view the last step used
    bool grounded = true;
    std::vector<BreachShot> shots;
    std::vector<BreachPickup> pickups;
    std::vector<BreachEvent> events; // appended by step(), at most 1024; what draws them clears them
    int kills = 0, collected = 0, health = 100, ticks = 0;
    float elapsed = 0, dash_cooldown = 0;

    bool won() const { return elapsed >= 90; }
    bool over() const { return health <= 0 || won(); }
    bool hurt() const { return _hurt > 0; }
    bool dashing() const { return _dash > 0; }
    int enemy_count() const { return static_cast<int>(_enemies.size()); }
    const BreachLevel& level() const { return _level; }
    Vec3f eye_position() const { return player + Vec3f{0.0f, eye, 0.0f}; }
    Vec3f view_direction() const { return Quatf::yaw_pitch(yaw, pitch).rotate({0.0f, 0.0f, -1.0f}); }
    // Where enemies' shots aim: the chest.
    Vec3f chest() const { return player + Vec3f{0.0f, 1.2f, 0.0f}; }
    // By slot: positions (kept by the movement pass), kinds and health.
    const std::vector<Vec3f>& enemy_positions() const { return _pos; }
    static int enemy_kind(int slot) { return slot % 3; }
    float enemy_hp(int slot) const { return _hp[static_cast<std::size_t>(slot)]; }
    static float enemy_max_hp(int kind) { return kind == 1 ? 3.0f : 1.0f; }
    JobSystem* jobs() const { return _jobs.get(); }

    // Gives every enemy a MeshRenderer of its kind's mesh and material, for
    // WorldRenderState3D to draw.
    void use_enemy_meshes(const std::array<Mesh, 3>& meshes, const std::array<Material3D, 3>& materials);
    WorldRenderState3D& render_state() { return _render; }

private:
    float random(float lo, float hi);
    void respawn(kin::EcsEntity entity);
    void event(BreachEvent::Kind kind, Vec3f pos, int enemy_kind = 0, Vec3f to = {}) {
        if (events.size() < 1024) events.push_back({kind, pos, to, enemy_kind});
    }
    void move_player(float dt, const BreachInput& input);
    void fire(const BreachInput& input);

    BreachLevel _level;
    WorldRenderState3D _render;
    flecs::query<Transform3D, BreachEnemy> _movement;
    std::vector<kin::EcsEntity> _enemies;
    std::vector<Vec3f> _pos;
    std::vector<BreachEnemy*> _enemy_at; // fresh within a step only (tables move between)
    std::vector<float> _hp;
    std::vector<float> _distance; // per storage row, this step
    std::vector<Vec3f> _aim;
    std::vector<int> _killed;
    std::unique_ptr<JobSystem> _jobs;
    RngKey _rng{};
    float _fire = 0, _hurt = 0, _dash = 0;
};

} // namespace examples
