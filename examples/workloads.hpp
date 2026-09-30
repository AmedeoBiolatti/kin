#pragma once

#include <kin/core/rng.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ui2/context.hpp>
#include <array>
#include <deque>
#include <memory>

namespace kin { class JobSystem; }

namespace examples {
using namespace kin;
using kin::EcsWorld;
using kin::EcsEntity;

struct ArenaInput { Vec2f move{}, aim{1, 0}; bool fire = false, dash = false; };
struct Enemy { int slot = 0, kind = 0; float hp = 1, cooldown = 0; };
struct Shot { Vec2f pos{}, velocity{}; float life = 0; bool hostile = false; };
struct Spark { Vec2f pos{}, velocity{}; float life = 0; };
struct Pickup { Vec2f pos{}; float life = 15; };
struct Impact { Vec2f pos{}; float life = .3f; };
// Something a renderer may want to show: an effect cue, never read by the simulation.
struct ArenaEvent {
    enum class Kind : u8 { Hit, Kill, Pickup, Dash, Hurt, Fire, EnemyFire };
    Kind kind = Kind::Hit;
    Vec2f pos{};
    int enemy_kind = 0;
};
enum class PowerEffect { None, Move, DashCooldown, DashDuration, DashSpeed, Damage, Fire,
    Magnet, Hull, Pull, Repair, ShotSpeed, ShotLife, Pellets, Armor, Regen };
struct PowerUp { std::string_view name, description; int parent, cost; PowerEffect effect=PowerEffect::None; float value=0; };
inline constexpr std::array<PowerUp, 37> power_ups{{
    {"UPLINK", "Collect green cores to expand your power grid.", -1, 0},
    {"DRIVE", "+60 movement speed.", 0, 3,PowerEffect::Move,60},
    {"BLINK", "Dash cooldown reduced by 0.6 seconds.", 1, 5,PowerEffect::DashCooldown,-.6f},
    {"AMPLIFIER", "+1 projectile damage.", 0, 3,PowerEffect::Damage,1},
    {"OVERCLOCK", "Fire interval reduced by 25%.", 3, 5,PowerEffect::Fire,.75f},
    {"MAGNET", "+120 core attraction radius.", 0, 3,PowerEffect::Magnet,120},
    {"HULL", "+40 maximum health; repair 40 health.", 5, 5,PowerEffect::Hull,40},
    {"AFTERBURN", "+200 dash speed.",2,6,PowerEffect::DashSpeed,200},
    {"PHASE", "+0.08 seconds of dash immunity.",7,8,PowerEffect::DashDuration,.08f},
    {"VECTOR", "+40 movement speed.",8,10,PowerEffect::Move,40},
    {"SLIPSTREAM", "Dash cooldown reduced by 0.2 seconds.",9,12,PowerEffect::DashCooldown,-.2f},
    {"HOT LOAD", "+0.5 projectile damage.",4,6,PowerEffect::Damage,.5f},
    {"FAST CYCLE", "Fire interval reduced by another 15%.",11,8,PowerEffect::Fire,.85f},
    {"BREAKER", "+1 projectile damage.",12,10,PowerEffect::Damage,1},
    {"REDLINE", "Fire interval reduced by another 15%.",13,12,PowerEffect::Fire,.85f},
    {"TRACTOR", "+160 core pulling speed.",6,6,PowerEffect::Pull,160},
    {"HARVEST", "+1 health repaired per collected core.",15,8,PowerEffect::Repair,1},
    {"LONG REACH", "+120 core attraction radius.",16,10,PowerEffect::Magnet,120},
    {"VACUUM", "+240 core pulling speed.",17,12,PowerEffect::Pull,240},
    {"ACCELERATOR", "+150 friendly projectile speed.",0,3,PowerEffect::ShotSpeed,150},
    {"STABILIZER", "+0.4 seconds of friendly projectile life.",19,5,PowerEffect::ShotLife,.4f},
    {"SPLITTER", "+1 spread projectile per volley.",20,6,PowerEffect::Pellets,1},
    {"RAIL COIL", "+200 friendly projectile speed.",21,8,PowerEffect::ShotSpeed,200},
    {"LONG SHOT", "+0.5 seconds of friendly projectile life.",22,10,PowerEffect::ShotLife,.5f},
    {"FAN SHOT", "+2 spread projectiles per volley.",23,12,PowerEffect::Pellets,2},
    {"PLATING", "Reduce incoming hit damage by 1 (minimum 1).",0,3,PowerEffect::Armor,1},
    {"BULKHEAD", "+20 maximum health; repair 20 health.",25,5,PowerEffect::Hull,20},
    {"ABLATIVE", "Reduce incoming hit damage by another 1.",26,6,PowerEffect::Armor,1},
    {"FORTRESS", "+30 maximum health; repair 30 health.",27,8,PowerEffect::Hull,30},
    {"REACTIVE", "Reduce incoming hit damage by another 1.",28,10,PowerEffect::Armor,1},
    {"BASTION", "+40 maximum health; repair 40 health.",29,12,PowerEffect::Hull,40},
    {"NANITES", "Regenerate 1 health per second.",0,3,PowerEffect::Regen,1},
    {"RECYCLER", "+1 health repaired per collected core.",31,5,PowerEffect::Repair,1},
    {"REBUILD", "Regenerate another 1 health per second.",32,6,PowerEffect::Regen,1},
    {"RESERVES", "+20 maximum health; repair 20 health.",33,8,PowerEffect::Hull,20},
    {"SALVAGER", "+2 health repaired per collected core.",34,10,PowerEffect::Repair,2},
    {"LIFELINE", "Regenerate another 2 health per second.",35,12,PowerEffect::Regen,2},
}};
inline constexpr std::array<std::array<int,6>,6> power_branches{{
    {{1,2,7,8,9,10}},{{3,4,11,12,13,14}},{{5,6,15,16,17,18}},
    {{19,20,21,22,23,24}},{{25,26,27,28,29,30}},{{31,32,33,34,35,36}}}};
struct PowerStats {
    float move=240, dash_cooldown=1.5f, dash_duration=.16f, dash_speed=950;
    float damage=1, fire=1, magnet=120, pull=340, shot_speed=850, shot_life=1.3f, regen=0;
    int hull=100, repair=2, pellets=0, armor=0;
};

class Arena {
public:
    static constexpr int max_enemies = 100000;
    explicit Arena(int enemies = 300, u64 seed = 7);
    ~Arena();
    void reset(u64 seed);
    void step(float dt, ArenaInput input, bool invincible = false);
    ArenaInput autopilot() const;
    void collect(RenderQueue& queue, const RenderView& view);
    u64 checksum() const;
    EcsWorld world;
    Vec2f player{1536, 1024};
    std::vector<Shot> shots;
    std::vector<Spark> sparks;
    std::vector<Pickup> pickups;
    int kills = 0, collected = 0, health = 100, ticks = 0;
    float elapsed = 0, dash_cooldown = 0;
    bool won() const { return elapsed >= 90; }
    int enemy_count() const { return static_cast<int>(_enemies.size()); }
    bool has_power(int id) const { return id >= 0 && id < int(power_ups.size()) && (_powers & (u64{1} << id)); }
    int available_cores() const { return 3 + collected - _spent; }
    bool can_buy_power(int id) const;
    bool buy_power(int id);
    int max_health() const { return _stats.hull; }
    const PowerStats& power_stats() const { return _stats; }

    // Visual state for renderers; the simulation never reads it.
    std::vector<ArenaEvent> events;  // appended by step(), at most 1024; the renderer clears it
    Vec2f facing() const { return _facing; }
    Vec2f travel() const { return _travel; }
    bool dashing() const { return _dash > 0; }
    bool hurt() const { return _hurt > 0; }
    bool muzzle_flash() const { return _muzzle > 0; }
    const std::vector<Impact>& impacts() const { return _impacts; }
    const std::vector<EcsEntity>& enemies() const { return _enemies; }
    // Calls f(position, enemy) for every enemy, in storage order: much faster than
    // looking each one up through enemies() when there are many.
    template <class F>
    void each_enemy(F&& f) const {
        _movement.each([&](const Transform2D& t, const Enemy& e) { f(t.pos, e); });
    }
    // Calls f(transforms, enemies) once per storage table, in each_enemy() order:
    // whole columns, for work split across threads.
    template <class F>
    void each_enemy_table(F&& f) const {
        _movement.run([&](flecs::iter& it) {
            while (it.next()) {
                const auto t = it.field<Transform2D>(0);
                const auto e = it.field<Enemy>(1);
                f(std::span<const Transform2D>{&t[0], it.count()}, std::span<const Enemy>{&e[0], it.count()});
            }
        });
    }
    // Worker threads, started once the arena is large enough to use them; null before.
    JobSystem* jobs() const { return _jobs.get(); }
    // Draws enemies with a texture instead of flat rects: each kind uses its
    // `sources` region of `atlas`, drawn at `sizes`.
    void use_enemy_textures(const Texture& atlas, const std::array<Rectf, 3>& sources, const std::array<Vec2f, 3>& sizes);
    // The ECS-rendered entities alone (enemies), into `queue`.
    void collect_entities(RenderQueue& queue, const RenderView& view);
private:
    void event(ArenaEvent::Kind kind, Vec2f pos, int enemy_kind = 0) {
        if (events.size() < 1024) events.push_back({kind, pos, enemy_kind});
    }
    float random(float lo, float hi);
    void respawn(EcsEntity entity);
    void burst(Vec2f pos);
    WorldRenderState _render;
    flecs::query<Transform2D, Enemy> _movement;
    std::vector<EcsEntity> _enemies;
    std::vector<int> _next;
    // Per slot: positions, kept by the movement pass and respawns (autopilot and
    // collisions read these, not the ECS), and the enemies' components, refreshed
    // by each movement pass for that step's collisions only.
    std::vector<Vec2f> _pos;
    std::vector<Enemy*> _enemy_at;
    std::vector<int> _killed; // slots killed this step
    std::vector<float> _distance; // per storage row, this step: distance to the player
    std::vector<Vec2f> _aim;      // and the unit vector toward it
    std::unique_ptr<JobSystem> _jobs; // started on the first step big enough to use it
    std::array<int, 48 * 32> _grid{};
    RngKey _rng{};
    float _fire = 0, _hurt = 0, _dash = 0;
    Vec2f _facing{1, 0}, _travel{}; // visual state only; never used by collisions
    std::vector<Impact> _impacts;
    float _muzzle = 0;
    u64 _powers = 1;
    PowerStats _stats;
    float _regen = 0;
    int _spent = 0;
};

struct HudState {
    bool paused = false, autoplay = false;
    int commands = 0;
    float display_scale = 1;
    bool reticle = false;   // draw the aim reticle at `aim` (logical coordinates)
    Vec2f aim{};
};

// Signal Siege's HUD, drawn with ui2 widgets over the fixed 1280x800 playfield at
// native pixel density. Restores the game's logical coordinates before returning
// (including mouse mapping). Keeps rolling counters and the wave banner, so keep
// one per run.
class ArenaHud {
public:
    void render(const Arena& arena, Renderer2D& renderer, Input& input, const HudState& state, float dt);
    // A wave banner started this frame (for its sound).
    bool wave_started() const { return _wave_started; }
    // Back to the start of a run: counters from zero, the first wave's banner again.
    void reset() {
        _kills = {.speed = 40}; _cores = {.speed = 30};
        _wave = 0; _banner = -1; _hurt = 0; _health = -1; _wave_started = false;
    }

private:
    ui2::Context _ui;
    ui2::AnimatedValue _kills{.speed = 40}, _cores{.speed = 30};
    int _wave = 0;
    float _banner = -1;  // seconds since the current wave's banner appeared
    float _hurt = 0;     // hull bar flash after damage
    int _health = -1;
    bool _wave_started = false;
};

// One-shot HUD for checks: a fresh ArenaHud with no input.
void render_arena_hud(const Arena& arena, Renderer2D& renderer, bool paused,
                      bool autoplay, int commands, float display_scale);
// Separate, native-resolution screen. Returns true when Return is clicked.
struct PowerGridState { int selected=1; std::string feedback; };
bool render_power_grid(Arena& arena, Input& input, Renderer2D& renderer,
                       PowerGridState& state, float display_scale);

struct TrainingRun {
    int id = 0, step = 0;
    std::string name, model, status;
    double loss = 0, tokens = 0, gpu = 0;
    std::array<float, 192> history{};
};

class Tracker {
public:
    explicit Tracker(int count = 10000, u64 seed = 7);
    void step(float dt);
    void filter(std::string text, bool best_first);
    void toggle_selected();
    u64 checksum() const;
    std::vector<TrainingRun> runs;
    std::vector<int> visible;
    std::deque<std::string> logs;
    int selected = 0, ticks = 0, revisions = 0;
    bool streaming = true;
    float elapsed = 0;
private:
    void rebuild_filter();
    std::string _filter;
    bool _best_first = false;
    float _sample_time = 0;
};

class TrackerDashboard {
public:
    void render(Tracker& model, Input& input, Renderer2D& renderer, Vec2f size, float dt, float display_scale = 1);
    void scripted_frame(Tracker& model, int frame, std::string_view scenario);
    int drawn_rows() const { return _drawn_rows; }
    bool wants_text_input() const { return _ui.wants_text_input(); }
private:
    ui2::Context _ui;
    ui2::TextInput _search;
    ui2::Theme _theme = ui2::editor_theme(ui2::PalettePreset::Slate);
    float _offset = 0;
    int _drawn_rows = 0;
    bool _best_first = false;
};
}
