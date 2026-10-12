// Summit's simulation, on the CPU (docs/3d_plan.md, P4a): the level (determinism, climbability by the stamina model), the player
// model's numbers, input as commands (the mapper, replay, save and restore, frame-rate independence, step purity), falls and
// ragdolls, the rope, the camera, and the scripted runs. No window and no GPU.

#include "camera.hpp"
#include "game.hpp"
#include "input_map.hpp"
#include "level.hpp"
#include "script.hpp"

#include <kin/platform/gamepad.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace summit;

int failures = 0;
bool verbose = false;

#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        if (!(cond)) {                                                                                \
            ++failures;                                                                               \
            std::fprintf(stderr, "summit_tests:%d: CHECK failed: %s\n", __LINE__, #cond);              \
        }                                                                                             \
    } while (0)

#define CHECK_NEAR(value, expected, eps)                                                                              \
    do {                                                                                                              \
        const double v_ = static_cast<double>(value), e_ = static_cast<double>(expected);                             \
        if (verbose) std::printf("  %s = %.4f (expected %.4f +- %.4f)\n", #value, v_, e_, static_cast<double>(eps));    \
        if (!(std::fabs(v_ - e_) <= static_cast<double>(eps))) {                                                      \
            ++failures;                                                                                               \
            std::fprintf(stderr, "summit_tests:%d: %s = %.5f, expected %.5f +- %.5f\n", __LINE__, #value, v_, e_,        \
                         static_cast<double>(eps));                                                                   \
        }                                                                                                             \
    } while (0)

using Clock = std::chrono::steady_clock;

// The seed the stored numbers are for.
constexpr u64 test_seed = 1;
// The final state hash of the scripted climb of seed 1 on this machine ("same machine" determinism: Jolt claims more, the look and
// facing use std::sin and std::atan2), and the steps it takes.
constexpr u64 expected_climb_hash = 0xc80d269809d52fb0ull;
constexpr u64 expected_climb_steps = 9769;

// ---- helpers ------------------------------------------------------------------------------------------------------------------

struct Observed {
    RunReport report;
    // The rope: steps both or one hand held it, how far up it the player got (m), and its top.
    int rope_steps = 0;
    f32 rope_gain = 0.0f;
    // Feet or middle of the capsule inside a collider, a non-finite number.
    int inside_steps = 0;
    // The camera along the run.
    int camera_steps = 0;
    int camera_inside = 0;
    int camera_obstructed = 0;
    f32 camera_worst_ease = 0.0f;
    f32 camera_farthest = 0.0f;
    u64 got_up_events = 0;
    u64 camp_events = 0;
    u64 summit_events = 0;
    u64 pull_up_blocked = 0;
};

// Plays a script on a fresh game, watching what the tests ask about.
Observed observe(u64 seed, const std::string& script, u64 max_steps = 40000) {
    Observed o;
    Game game(seed);
    BotConfig config;
    script_config(script, game.level(), config);
    Bot bot(game, 0, config);
    CameraRig rig;
    f32 rope_start = 0.0f;
    bool rope_seen = false;
    f32 prev_arm = -1.0f;
    for (u64 s = 0; s < max_steps && !bot.finished() && !game.player(0).summit; ++s) {
        const PlayerCommand c = bot.next();
        o.report.log.push_back(c);
        game.step({&c, 1});
        const Vec3f f = game.feet(0);
        if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z)) o.report.non_finite = true;
        o.report.min_stamina = std::min(o.report.min_stamina, game.stamina(0));
        o.report.highest = std::max(o.report.highest, f.y - game.level().start.y);
        for (const GameEvent& e : game.events()) {
            if (e.kind == GameEventKind::GotUp) ++o.got_up_events;
            if (e.kind == GameEventKind::Camp) ++o.camp_events;
            if (e.kind == GameEventKind::Summit) ++o.summit_events;
            if (e.kind == GameEventKind::PullUpBlocked) ++o.pull_up_blocked;
        }
        // Never through a collider: the middle of the capsule is not inside a piece.
        if (game.player(0).state == PlayerState::Alive) {
            QueryFilter3D filter;
            filter.mask = layer_bit(layers3d::Static);
            if (!game.world().point_inside(f + Vec3f{0.0f, 0.9f, 0.0f}, filter).empty()) ++o.inside_steps;
        }
        const TraversalState3D ts = game.traversal(0);
        const bool on_rope = ts.held >= 1 && (ts.grips[0].kind == GripKind3D::Rope || ts.grips[1].kind == GripKind3D::Rope) &&
                             (ts.grips[0].phase == GripPhase3D::Held || ts.grips[1].phase == GripPhase3D::Held);
        if (on_rope) {
            if (!rope_seen) {
                rope_seen = true;
                rope_start = f.y;
            }
            ++o.rope_steps;
            o.rope_gain = std::max(o.rope_gain, f.y - rope_start);
        }
        // The camera, a frame a step.
        rig.observe(game, 0);
        const CameraView v = rig.frame(game.world(), 1.0f, fixed_step);
        ++o.camera_steps;
        QueryFilter3D wall;
        wall.mask = layer_bit(layers3d::Static);
        if (!game.world().point_inside(v.position, wall).empty()) ++o.camera_inside;
        if (v.obstructed) ++o.camera_obstructed;
        if (prev_arm >= 0.0f && v.arm > prev_arm) o.camera_worst_ease = std::max(o.camera_worst_ease, v.arm - prev_arm);
        prev_arm = v.arm;
        o.camera_farthest = std::max(o.camera_farthest, v.arm);
    }
    const Player& p = game.player(0);
    o.report.steps = o.report.log.size();
    o.report.summit = p.summit;
    o.report.summit_step = p.summit_step;
    o.report.falls = p.falls;
    o.report.stuns = p.stuns;
    o.report.deaths = p.deaths;
    o.report.final_hash = game.state_hash();
    o.report.legs_done = bot.leg();
    return o;
}

const Observed& climb() {
    static const Observed o = observe(test_seed, "climb");
    return o;
}

// Steps `n` steps of a held command.
void hold(Game& g, const PlayerCommand& c, int n) {
    for (int i = 0; i < n; ++i) g.step({&c, 1});
}

// Brings a fresh game to the moment the player hangs by both hands at the first wall (after the walk, the face, the grab).
Game* hang_at_first_wall(Game& g, Bot& bot) {
    for (int s = 0; s < 4000; ++s) {
        const PlayerCommand c = bot.next();
        g.step({&c, 1});
        const TraversalState3D ts = g.traversal(0);
        if (ts.mode == TraversalMode3D::Hanging && ts.held == 2 && ts.grips[0].phase == GripPhase3D::Held && ts.grips[1].phase == GripPhase3D::Held) {
            return &g;
        }
    }
    return nullptr;
}

// ---- the level -----------------------------------------------------------------------------------------------------------------

void test_level_determinism() {
    const SummitLevel a = generate_level(7), b = generate_level(7), c = generate_level(8);
    CHECK(a.bytes() == b.bytes());
    CHECK(a.hash() == b.hash());
    CHECK(a.hash() != c.hash());
    CHECK(a.bytes() != c.bytes());
    CHECK(a.pieces.size() > 40);
    CHECK(a.camps.size() == 7);
    CHECK(a.ropes.size() == 1);
    // The render meshes are a function of the same data.
    const std::vector<RenderMeshData> ma = build_render_meshes(a), mb = build_render_meshes(b);
    CHECK(ma.size() == mb.size() && !ma.empty());
    bool same = ma.size() == mb.size();
    for (std::size_t i = 0; same && i < ma.size(); ++i) {
        same = ma[i].mesh.vertices.size() == mb[i].mesh.vertices.size() &&
               std::memcmp(ma[i].mesh.vertices.data(), mb[i].mesh.vertices.data(), ma[i].mesh.vertices.size() * sizeof(MeshVertex)) == 0 &&
               ma[i].mesh.indices == mb[i].mesh.indices;
    }
    CHECK(same);
    // The route has every kind of leg.
    bool kinds[6] = {};
    for (const Leg& l : a.route) kinds[int(l.kind)] = true;
    for (bool k : kinds) CHECK(k);
    // Altitude: the design asks for about 120 to 180 m, over many seeds.
    for (u64 seed = 1; seed <= 20; ++seed) {
        const SummitLevel l = generate_level(seed);
        CHECK(l.height() > 120.0f && l.height() < 180.0f);
        CHECK(l.kill_y < l.start.y - 5.0f);
    }
}

void test_colliders_and_meshes_are_one_list() {
    Game g(test_seed);
    const SummitLevel& level = g.level();
    int holds = 0;
    for (std::size_t i = 0; i < level.pieces.size(); ++i) {
        const Piece& p = level.pieces[i];
        const Body3D b = g.piece_body(i);
        CHECK(g.world().contains(b));
        const Vec3f d = g.world().position(b) - p.centre;
        CHECK(length(d) < 1e-3f);
        CHECK(g.world().shape(b).kind() == Shape3DKind::Box);
        CHECK(g.world().motion(b) == Body3DMotion::Static);
        if (p.hold) {
            ++holds;
            CHECK(g.world().is_hold(b));
        }
        CHECK(g.world().surface_material(b).grippable == p.grippable);
    }
    CHECK(holds >= 8);
    // A ray down onto a ground piece's top finds that piece's body, and the mesh has a vertex on that face (what is drawn is
    // where it is hit).
    const std::vector<RenderMeshData> meshes = build_render_meshes(level);
    int rays = 0;
    for (std::size_t i = 0; i < level.pieces.size(); ++i) {
        const Piece& p = level.pieces[i];
        if (p.kind != PieceKind::Ground || p.half.y < 1.0f) continue;
        const Vec3f top = p.centre + p.rotation.rotate({0.0f, p.half.y, 0.0f});
        const auto hit = g.world().raycast(top + Vec3f{0.0f, 0.5f, 0.0f}, top - Vec3f{0.0f, 0.5f, 0.0f});
        if (!hit || hit->fraction <= 0.0f) continue; // (covered by something higher: the ray starts inside it)
        ++rays;
        // Whichever ground piece covers that spot is hit first: a body of the list, at that height.
        CHECK(hit->user_id >= 1 && hit->user_id <= level.pieces.size());
        CHECK_NEAR(hit->point.y, top.y, 0.06f);
        bool vertex_there = false;
        for (const RenderMeshData& m : meshes) {
            for (const MeshVertex& v : m.mesh.vertices) {
                if (std::fabs(v.position.y - top.y) < 0.05f && std::fabs(v.position.x - top.x) < 1.0f && std::fabs(v.position.z - top.z) < 1.0f) {
                    vertex_there = true;
                    break;
                }
            }
            if (vertex_there) break;
        }
        CHECK(vertex_there);
    }
    CHECK(rays > 5);
    // The meshes' bounds hold every piece's centre.
    Vec3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const RenderMeshData& m : build_render_meshes(level)) {
        const Bounds3 bb = m.mesh.bounds();
        lo = {std::min(lo.x, bb.min.x), std::min(lo.y, bb.min.y), std::min(lo.z, bb.min.z)};
        hi = {std::max(hi.x, bb.max.x), std::max(hi.y, bb.max.y), std::max(hi.z, bb.max.z)};
    }
    for (const Piece& p : level.pieces) {
        CHECK(p.centre.x >= lo.x && p.centre.x <= hi.x && p.centre.y >= lo.y && p.centre.y <= hi.y && p.centre.z >= lo.z && p.centre.z <= hi.z);
    }
    // The meshes touch the level's bounds: they are drawn from the pieces that make them.
    CHECK_NEAR(lo.y, level.bounds.min.y, 0.05f);
    CHECK_NEAR(hi.y, level.bounds.max.y, 0.05f);
}

void test_route_is_climbable_by_the_stamina_model() {
    const ClimbCosts costs;
    int climbing_legs = 0;
    for (u64 seed = 1; seed <= 30; ++seed) {
        const SummitLevel level = generate_level(seed);
        const RoutePlan plan = plan_route(level, costs);
        CHECK(plan.climbable);
        CHECK(plan.worst_cost < costs.max_stamina - 4.0f);
        CHECK(plan.least_margin > 4.0f);
        CHECK(plan.total_seconds > 90.0f && plan.total_seconds < 400.0f);
        for (const LegPlan& l : plan.legs) {
            if (l.kind == LegKind::Climb || l.kind == LegKind::Overhang || l.kind == LegKind::Rope) ++climbing_legs;
        }
    }
    CHECK(climbing_legs >= 30 * 6);
    // A worked leg: a climb costs two grabs, two hands moving the height at the climb speed, and the pull-up.
    const SummitLevel level = generate_level(test_seed);
    const RoutePlan plan = plan_route(level, costs);
    for (std::size_t i = 0; i < level.route.size(); ++i) {
        if (level.route[i].kind != LegKind::Climb) continue;
        const f32 t = (level.route[i].height - costs.first_hold_height) / costs.climb_speed + costs.settle_seconds;
        const f32 expected = 2.0f * costs.grab_cost + (costs.drain_two_hands + costs.drain_moving) * t + costs.pull_up_cost;
        CHECK_NEAR(plan.legs[i].cost, expected, 0.01f);
        break;
    }
    // A leg that cannot be done from a full bar (the same wall 2.5 times as high) is infeasible and says so.
    {
        SummitLevel one = level;
        for (Leg& l : one.route) {
            if (l.kind == LegKind::Climb) l.height *= 2.5f;
        }
        const RoutePlan p = plan_route(one, costs);
        CHECK(!p.climbable);
        bool some_infeasible = false;
        for (const LegPlan& l : p.legs) some_infeasible = some_infeasible || !l.feasible;
        CHECK(some_infeasible);
    }
    // A mountain whose pitches are three times as high is not climbable: the planner says so.
    SummitLevel steep = level;
    for (Leg& l : steep.route) l.height *= 3.0f;
    CHECK(!plan_route(steep, costs).climbable);
    // And the planner is not far from the run: the lowest stamina the bot reached is no lower than the dearest leg allows (a
    // margin for the reach, the grabs and the pull-up's start).
    const Observed& o = climb();
    CHECK(o.report.min_stamina > plan.least_margin - 3.0f);
    CHECK(o.report.min_stamina < plan.least_margin + 14.0f);
}

// ---- the player model's numbers -------------------------------------------------------------------------------------------

void test_stamina_numbers() {
    // Hanging by both hands, still: 7 a second; moving up: 7 + 4; one hand: 12.
    {
        Game g(test_seed);
        Bot bot(g);
        CHECK(hang_at_first_wall(g, bot) != nullptr);
        PlayerCommand c;
        c.grab_left = c.grab_right = true;
        // The toes on the ground are rest: no drain until the feet are off it (a metre up the face).
        const f32 r0 = g.stamina(0);
        hold(g, c, 20);
        CHECK(g.stamina(0) >= r0 - 0.01f);
        c.move.y = 1.0f;
        hold(g, c, 40);
        // The climb speed: 0.8 m/s along the face.
        const f32 y0 = g.feet(0).y;
        hold(g, c, 60);
        CHECK_NEAR(g.feet(0).y - y0, 0.8f, 0.12f);
        hold(g, c, 20);
        CHECK(g.feet(0).y - g.level().route[2].from.y > 1.0f);
        c.move.y = 0.0f;
        hold(g, c, 6);
        const f32 s0 = g.stamina(0);
        hold(g, c, 60);
        CHECK_NEAR(s0 - g.stamina(0), 7.0f, 0.4f);
        const f32 s1 = g.stamina(0);
        c.move.y = 1.0f;
        hold(g, c, 60);
        CHECK_NEAR(s1 - g.stamina(0), 11.0f, 0.5f);
    }
    {
        Game g(test_seed);
        Bot bot(g);
        CHECK(hang_at_first_wall(g, bot) != nullptr);
        PlayerCommand up;
        up.grab_left = up.grab_right = true;
        up.move.y = 1.0f;
        hold(g, up, 130);
        PlayerCommand c;
        c.grab_left = true;
        c.grab_right = false;
        hold(g, c, 20); // the right hand lets go
        const f32 s0 = g.stamina(0);
        hold(g, c, 60);
        CHECK(g.traversal(0).held == 1);
        CHECK_NEAR(s0 - g.stamina(0), 12.0f, 0.8f);
    }
    // A jump costs 6; sprinting 15 a second and 7 m/s; the stamina comes back at 25 a second after a second of rest.
    {
        Game g(test_seed);
        hold(g, {}, 90);
        CHECK_NEAR(g.stamina(0), 150.0f, 0.01f);
        PlayerCommand j;
        j.jump = true;
        g.step({&j, 1});
        g.step({&j, 1});
        CHECK_NEAR(g.stamina(0), 144.0f, 0.2f);
        hold(g, {}, 120);
        // Regeneration: no gain inside the first second, then 25 a second (back to full in under a second here).
        CHECK_NEAR(g.stamina(0), 150.0f, 0.01f);
    }
    {
        Game g(test_seed);
        hold(g, {}, 60);
        PlayerCommand s;
        s.move.y = 1.0f;
        s.sprint = true;
        hold(g, s, 30); // up to speed
        const f32 s0 = g.stamina(0);
        const f32 sp = g.character_state(0).ground_speed;
        CHECK_NEAR(sp, 7.0f, 0.3f);
        hold(g, s, 60);
        CHECK_NEAR(s0 - g.stamina(0), 15.0f, 0.8f);
        // Rest: a second of nothing, then 25 a second.
        hold(g, {}, 30);
        const f32 low = g.stamina(0);
        hold(g, {}, 30 + 30);
        CHECK_NEAR(g.stamina(0) - low, 25.0f * 0.5f, 3.0f);
    }
    // Injury lowers the cap: health 40 gives 150 x (0.5 + 0.5 x 0.4) = 105.
    {
        Game g(test_seed);
        hold(g, {}, 10);
        CHECK_NEAR(g.stamina_cap(0), 150.0f, 0.01f);
        g.debug_set_health(0, 40.0f);
        hold(g, {}, 3);
        CHECK_NEAR(g.stamina_cap(0), 105.0f, 0.5f);
        CHECK(g.stamina(0) <= 105.01f);
        g.debug_set_health(0, 0.0f);
        hold(g, {}, 3);
        CHECK_NEAR(g.stamina_cap(0), 75.0f, 0.5f);
    }
}

void test_look_is_clamped() {
    Game g(test_seed);
    PlayerCommand c;
    c.look = {0.0f, 40.0f};
    hold(g, c, 10);
    CHECK_NEAR(g.player(0).pitch, 80.0f, 0.001f);
    c.look = {0.0f, -40.0f};
    hold(g, c, 10);
    CHECK_NEAR(g.player(0).pitch, -80.0f, 0.001f);
    // Turning right lowers the yaw (it is a rotation about +Y); a full turn comes back to the same view.
    const f32 y0 = g.player(0).yaw;
    c.look = {90.0f, 0.0f};
    hold(g, c, 1);
    CHECK_NEAR(std::fmod(std::fabs(y0 - g.player(0).yaw) + 360.0f, 360.0f), 90.0f, 0.01f);
    // Non-finite commands are ignored.
    PlayerCommand bad;
    bad.move = {std::nanf(""), 1.0f};
    bad.look = {std::nanf(""), 0.0f};
    hold(g, bad, 5);
    const Vec3f f = g.feet(0);
    CHECK(std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z) && std::isfinite(g.player(0).yaw));
}

// ---- input as commands -----------------------------------------------------------------------------------------------------

void test_mapper() {
    const InputBindings b = InputBindings::defaults();
    // Nothing pressed, nothing asked.
    CHECK(map_input(RawInput{}, b) == PlayerCommand{});
    // A key and a stick are the same command.
    RawInput keys;
    keys.set_key(Key::W);
    RawInput stick;
    stick.pad.connected = true;
    stick.pad.left_y = 1.0f;
    CHECK(map_input(keys, b) == map_input(stick, b));
    CHECK_NEAR(map_input(keys, b).move.y, 1.0f, 1e-6f);
    keys = {};
    keys.set_key(Key::D);
    stick = {};
    stick.pad.connected = true;
    stick.pad.left_x = 1.0f;
    CHECK(map_input(keys, b) == map_input(stick, b));
    // Jump: space and A; the hands: the mouse buttons, Q and E, the triggers; sprint: shift and the left stick's click.
    {
        RawInput a, p;
        a.set_key(Key::Space);
        p.pad.connected = true;
        p.pad.set_button(GamepadButton::A, true);
        CHECK(map_input(a, b) == map_input(p, b));
        CHECK(map_input(a, b).jump);
        RawInput m, t;
        m.set_button(MouseButton::Left);
        t.pad.connected = true;
        t.pad.left_trigger = 1.0f;
        CHECK(map_input(m, b) == map_input(t, b));
        CHECK(map_input(m, b).grab_left && !map_input(m, b).grab_right);
        RawInput r, tr;
        r.set_key(Key::E);
        tr.pad.connected = true;
        tr.pad.right_trigger = 0.9f;
        CHECK(map_input(r, b) == map_input(tr, b));
        CHECK(map_input(r, b).grab_right);
        RawInput sh, lc;
        sh.set_key(Key::LeftShift);
        lc.pad.connected = true;
        lc.pad.set_button(GamepadButton::LeftStick, true);
        CHECK(map_input(sh, b) == map_input(lc, b));
        RawInput x, bb;
        x.set_key(Key::X);
        bb.pad.connected = true;
        bb.pad.set_button(GamepadButton::B, true);
        CHECK(map_input(x, b) == map_input(bb, b));
        CHECK(map_input(x, b).release);
    }
    // A trigger counts past the threshold (0.5) and not before.
    {
        RawInput t;
        t.pad.connected = true;
        t.pad.right_trigger = 0.45f;
        CHECK(!map_input(t, b).grab_right);
        t.pad.right_trigger = 0.55f;
        CHECK(map_input(t, b).grab_right);
    }
    // Dead zone: inside it nothing; at the edge of it nothing; full travel is 1; half way between is rescaled.
    {
        CHECK_NEAR(apply_dead_zone({0.1f, 0.0f}, 0.2f).x, 0.0f, 1e-6f);
        CHECK_NEAR(apply_dead_zone({0.2f, 0.0f}, 0.2f).x, 0.0f, 1e-6f);
        CHECK_NEAR(apply_dead_zone({1.0f, 0.0f}, 0.2f).x, 1.0f, 1e-6f);
        CHECK_NEAR(apply_dead_zone({0.6f, 0.0f}, 0.2f).x, 0.5f, 1e-5f);
        RawInput s;
        s.pad.connected = true;
        s.pad.left_x = 0.15f; // inside the default 0.18
        s.pad.right_x = 0.15f;
        CHECK(map_input(s, b) == PlayerCommand{});
        s.pad.left_x = 0.59f;
        CHECK(map_input(s, b).move.x > 0.3f && map_input(s, b).move.x < 0.5f);
    }
    // Two keys at once at right angles are not faster than one: unit length.
    {
        RawInput k;
        k.set_key(Key::W).set_key(Key::D);
        const PlayerCommand c = map_input(k, b);
        CHECK_NEAR(std::sqrt(c.move.x * c.move.x + c.move.y * c.move.y), 1.0f, 1e-5f);
        // Opposite keys cancel.
        RawInput o;
        o.set_key(Key::W).set_key(Key::S);
        CHECK_NEAR(map_input(o, b).move.y, 0.0f, 1e-6f);
    }
    // Look: the mouse in degrees a pixel, up on the screen is up; a stick in degrees a second; inverted y.
    {
        RawInput m;
        m.mouse_delta = {10.0f, -20.0f};
        PlayerCommand c = map_input(m, b);
        CHECK_NEAR(c.look.x, 1.0f, 1e-5f);
        CHECK_NEAR(c.look.y, 2.0f, 1e-5f);
        InputBindings inv = b;
        inv.invert_y = true;
        CHECK_NEAR(map_input(m, inv).look.y, -2.0f, 1e-5f);
        InputBindings fast = b;
        fast.mouse_sensitivity = 0.2f;
        CHECK_NEAR(map_input(m, fast).look.x, 2.0f, 1e-5f);
        RawInput s;
        s.pad.connected = true;
        s.pad.right_x = 1.0f;
        CHECK_NEAR(map_input(s, b, 1.0f / 60.0f).look.x, 150.0f / 60.0f, 1e-4f);
    }
    // Rebinding: Space no longer jumps, Z does; text round trip; a name that is not known changes nothing.
    {
        InputBindings r = b;
        r.set(Action::Jump, {Binding::key(Key::Z)});
        RawInput z, sp;
        z.set_key(Key::Z);
        sp.set_key(Key::Space);
        CHECK(map_input(z, r).jump);
        CHECK(!map_input(sp, r).jump);
        InputBindings parsed = InputBindings::defaults();
        CHECK(parsed.from_text(r.to_text()));
        CHECK(parsed.table == r.table);
        CHECK(parsed.dead_zone == r.dead_zone && parsed.invert_y == r.invert_y);
        InputBindings keep = b;
        CHECK(!keep.from_text("jump = key:NoSuchKey\n"));
        CHECK(keep.table == b.table);
        CHECK(!keep.from_text("nonsense = 1\n"));
    }
    // A disconnected pad's numbers are not read (a stale state).
    {
        RawInput s;
        s.pad.left_y = 1.0f;
        s.pad.set_button(GamepadButton::A, true);
        CHECK(map_input(s, b) == PlayerCommand{});
    }
}

void test_gamepad_poll_without_a_device() {
    // No window and no device here: polling reports nothing and does not fail (and the subsystem is left as it was found).
    kin::Gamepads pads;
    const kin::GamepadState& a = pads.poll();
    const bool connected = a.connected;
    const kin::GamepadState& b = pads.poll();
    CHECK(b.connected == connected);
    if (!connected) CHECK(b == kin::GamepadState{});
}

void test_command_log_files() {
    const std::vector<PlayerCommand> log = climb().report.log;
    const std::string path = "summit_test_log.bin";
    CHECK(write_command_log(path, 77, log));
    u64 seed = 0;
    std::vector<PlayerCommand> back;
    CHECK(read_command_log(path, seed, back));
    CHECK(seed == 77 && back == log);
    // Cut short: refused.
    std::FILE* f = std::fopen(path.c_str(), "rb");
    std::vector<unsigned char> bytes(64 * 1024);
    const std::size_t n = f ? std::fread(bytes.data(), 1, bytes.size(), f) : 0;
    if (f) std::fclose(f);
    f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(bytes.data(), 1, n - 7, f);
        std::fclose(f);
    }
    // (the file was longer than 64 KiB: what was read is a prefix, cut again)
    CHECK(!read_command_log(path, seed, back));
    std::remove(path.c_str());
    CHECK(!read_command_log("no_such_summit_log.bin", seed, back));
}

// ---- replay, hash, save and restore ----------------------------------------------------------------------------------------------

void test_replay_gives_the_same_hash() {
    const Observed& o = climb();
    Game g(test_seed);
    CHECK(replay(g, o.report.log) == o.report.final_hash);
    CHECK(g.player(0).summit);
    // Another mountain, the same commands: another game.
    Game other(test_seed + 1);
    CHECK(replay(other, o.report.log) != o.report.final_hash);
    // A single command changed in the middle: another hash (the log is what decides).
    std::vector<PlayerCommand> changed = o.report.log;
    changed[2000].jump = !changed[2000].jump;
    changed[2001].look.x += 0.5f;
    Game h(test_seed);
    CHECK(replay(h, changed) != o.report.final_hash);
}

void test_save_and_restore() {
    const Observed& o = climb();
    const std::vector<PlayerCommand>& log = o.report.log;
    // Saved while hanging on a face, replayed to the end from a game made fresh.
    // The first moment after step 2000 that it hangs on a face.
    std::size_t at = 0;
    {
        Game probe(test_seed);
        for (std::size_t i = 0; i < log.size() && at == 0; ++i) {
            probe.step({&log[i], 1});
            if (i > 2000 && probe.traversal(0).hanging() && probe.traversal(0).held == 2) at = i + 1;
        }
    }
    CHECK(at > 2000);
    Game a(test_seed);
    for (std::size_t i = 0; i < at; ++i) a.step({&log[i], 1});
    const TraversalState3D mid = a.traversal(0);
    CHECK(mid.hanging());
    const u64 hash_at = a.state_hash();
    const std::vector<u8> blob = a.save();
    for (std::size_t i = at; i < log.size(); ++i) a.step({&log[i], 1});
    CHECK(a.state_hash() == o.report.final_hash);
    Game b(test_seed);
    CHECK(b.restore(blob));
    CHECK(b.state_hash() == hash_at);
    CHECK(b.step_count() == at);
    for (std::size_t i = at; i < log.size(); ++i) b.step({&log[i], 1});
    CHECK(b.state_hash() == o.report.final_hash);
    // The same game, after it has run on, goes back.
    CHECK(a.restore(blob));
    CHECK(a.state_hash() == hash_at);
    for (std::size_t i = at; i < at + 600; ++i) a.step({&log[i], 1});
    Game c(test_seed);
    CHECK(c.restore(blob));
    for (std::size_t i = at; i < at + 600; ++i) c.step({&log[i], 1});
    CHECK(a.state_hash() == c.state_hash());
    // Refused, and nothing changed: another mountain, another player count, a cut file, a wrong magic.
    Game other(test_seed + 1);
    const u64 other_hash = other.state_hash();
    CHECK(!other.restore(blob));
    CHECK(other.state_hash() == other_hash);
    GameConfig two;
    two.players = 2;
    Game pair(test_seed, two);
    const u64 pair_hash = pair.state_hash();
    CHECK(!pair.restore(blob));
    CHECK(pair.state_hash() == pair_hash);
    std::vector<u8> cut(blob.begin(), blob.end() - 9);
    Game d(test_seed);
    const u64 d_hash = d.state_hash();
    CHECK(!d.restore(cut));
    CHECK(d.state_hash() == d_hash);
    std::vector<u8> bad = blob;
    bad[0] ^= 0xFF;
    CHECK(!d.restore(bad));
    CHECK(!d.restore({}));
}

void test_state_covers_rng_wind_and_players() {
    Game g(test_seed);
    hold(g, {}, 130);
    const std::vector<u8> blob = g.save();
    const u64 h = g.state_hash();
    // The saved bytes: magic, version (4 + 4), seed (8), step (8), then the RNG key (4 + 4): change a byte of it.
    std::vector<u8> altered = blob;
    altered[24] ^= 0x5A;
    Game r(test_seed);
    CHECK(r.restore(altered));
    CHECK(r.state_hash() != h);
    // The wind (after the key): four floats.
    altered = blob;
    altered[32] ^= 0x01;
    altered[33] ^= 0x01;
    altered[34] ^= 0x80;
    Game w(test_seed);
    CHECK(w.restore(altered));
    CHECK(w.state_hash() != h);
    // A player's health.
    Game hp(test_seed);
    hp.restore(blob);
    hp.debug_set_health(0, 50.0f);
    CHECK(hp.state_hash() != h);
    // The key moved at step 120: the RNG is in the state and in the stepping.
    Game early(test_seed);
    hold(early, {}, 100);
    CHECK(early.rng() != g.rng());
    // The step number alone is in the hash.
    Game one(test_seed), two(test_seed);
    hold(one, {}, 5);
    hold(two, {}, 6);
    CHECK(one.state_hash() != two.state_hash());
}

void test_frame_rate_independence() {
    const Observed& o = climb();
    const std::vector<PlayerCommand>& log = o.report.log;
    const std::size_t total = 5000; // enough to include a climb and a camp
    Game ref(test_seed);
    for (std::size_t i = 0; i < total; ++i) ref.step({&log[i], 1});
    const u64 reference = ref.state_hash();
    struct Rate {
        const char* name;
        double seconds;
        bool jitter;
    };
    const Rate rates[] = {{"30 fps", 1.0 / 30.0, false}, {"60 fps", 1.0 / 60.0, false}, {"144 fps", 1.0 / 144.0, false},
                          {"240 fps", 1.0 / 240.0, false}, {"jitter", 0.0, true}};
    u32 lcg = 12345u;
    for (const Rate& rate : rates) {
        Game g(test_seed);
        FrameStepper stepper;
        CameraRig rig;
        std::size_t done = 0;
        int frames = 0;
        while (done < total && frames < 200000) {
            double frame = rate.seconds;
            if (rate.jitter) {
                lcg = lcg * 1664525u + 1013904223u;
                frame = 1.0 / 200.0 + (double(lcg >> 8) / double(1u << 24)) * (1.0 / 15.0 - 1.0 / 200.0);
            }
            u32 n = stepper.advance(frame);
            for (; n > 0 && done < total; --n) {
                g.step({&log[done], 1});
                rig.observe(g, 0);
                ++done;
            }
            // The camera is a frame's work and reads the world: it must not change a thing.
            (void)rig.frame(g.world(), stepper.alpha(), f32(frame));
            ++frames;
        }
        if (verbose) std::printf("  %s: %d frames\n", rate.name, frames);
        CHECK(g.state_hash() == reference);
    }
}

void test_step_purity() {
    const Observed& o = climb();
    const std::vector<PlayerCommand>& log = o.report.log;
    // One thread and the job pool, with another game stepped in between: the same hash every step.
    GameConfig pool;
    pool.single_threaded = false;
    Game a(test_seed), b(test_seed, pool), noise(test_seed + 5);
    PlayerCommand wander;
    wander.move = {0.5f, 1.0f};
    for (std::size_t i = 0; i < 900; ++i) {
        a.step({&log[i], 1});
        noise.step({&wander, 1});
        b.step({&log[i], 1});
        if (i % 50 == 0) {
            CHECK(a.state_hash() == b.state_hash());
        }
    }
    CHECK(a.state_hash() == b.state_hash());
    // Reading the state does not change it (the const accessors and a save).
    const u64 h = a.state_hash();
    (void)a.save();
    (void)a.head(0);
    (void)a.traversal(0);
    CHECK(a.state_hash() == h);
    // A short command list (a missing command is the default one) is the same as a full one of defaults.
    GameConfig two;
    two.players = 2;
    Game p(test_seed, two), q(test_seed, two);
    std::vector<PlayerCommand> both(2);
    both[0].move.y = 1.0f;
    for (int i = 0; i < 120; ++i) {
        p.step({both.data(), 1});
        q.step({both.data(), 2});
    }
    CHECK(p.state_hash() == q.state_hash());
}

void test_two_players() {
    GameConfig two;
    two.players = 2;
    Game g(test_seed, two);
    CHECK(g.players().size() == 2);
    CHECK(g.world().character_count() == 2);
    std::vector<PlayerCommand> c(2);
    c[1].move.y = 1.0f;
    c[1].sprint = true;
    for (int i = 0; i < 150; ++i) g.step(c);
    // Player 1 ran; player 0 stood where it began (a metre or so off the camp's middle).
    CHECK(length(g.feet(1) - g.feet(0)) > 5.0f);
    CHECK(g.player(0).state == PlayerState::Alive && g.player(1).state == PlayerState::Alive);
    CHECK(g.stamina(1) < g.stamina(0));
    const std::vector<u8> blob = g.save();
    const u64 h = g.state_hash();
    for (int i = 0; i < 60; ++i) g.step(c);
    CHECK(g.restore(blob) && g.state_hash() == h);
    // Each has its own ragdoll.
    CHECK(g.world().ragdoll_count() == 2);
}

// ---- falls, ragdoll, get-up, respawn ----------------------------------------------------------------------------------------------

void settle(Game& g, int steps = 60) { hold(g, {}, steps); }

void test_short_fall_is_free() {
    Game g(test_seed);
    settle(g);
    const Vec3f at = g.feet(0);
    g.debug_teleport(0, at + Vec3f{0.0f, 2.0f, 0.0f}); // 6.3 m/s on landing: under 7
    settle(g, 150);
    CHECK(g.player(0).state == PlayerState::Alive);
    CHECK_NEAR(g.player(0).health, 100.0f, 0.001f);
    CHECK(g.player(0).stuns == 0);
}

void test_fall_stuns_ragdolls_gets_up() {
    Game g(test_seed);
    settle(g);
    const Vec3f at = g.feet(0);
    g.debug_teleport(0, at + Vec3f{0.0f, 5.0f, 0.0f});
    bool stunned = false, capsule_off = false, got_up = false, up_again = false, ragdoll_running = false;
    f32 landing = 0.0f;
    int stun_steps = 0, getting_up_steps = 0;
    PlayerCommand ahead;
    ahead.move.y = 1.0f;
    for (int i = 0; i < 700; ++i) {
        const bool walking_now = g.player(0).state == PlayerState::Alive && got_up;
        g.step({&ahead, walking_now ? 1u : 0u});
        for (const GameEvent& e : g.events()) {
            if (e.kind == GameEventKind::Landed) landing = e.value;
            if (e.kind == GameEventKind::GotUp) got_up = true;
        }
        const Player& p = g.player(0);
        if (p.state == PlayerState::Stunned) {
            stunned = true;
            ++stun_steps;
            capsule_off = capsule_off || !g.world().character_enabled(p.character);
            const RagdollState3D rs = g.world().ragdoll_state(p.ragdoll);
            ragdoll_running = ragdoll_running || rs.mode == RagdollMode3D::Ragdoll || rs.mode == RagdollMode3D::Blend;
        }
        if (p.state == PlayerState::GettingUp) ++getting_up_steps;
        if (got_up && p.state == PlayerState::Alive) up_again = true;
    }
    CHECK(stunned);
    CHECK(capsule_off);
    CHECK(ragdoll_running);
    CHECK(got_up);
    CHECK(up_again);
    // The landing speed is sqrt(2 g h) less the step's half (a drop of 5 m): about 9.7 m/s.
    CHECK_NEAR(landing, 9.7f, 0.5f);
    // Health: 7 a m/s above 7.
    const f32 expected = 100.0f - (landing - 7.0f) * 7.0f;
    CHECK(g.player(0).health >= expected - 1.0f);
    CHECK(g.player(0).health < 100.0f);
    CHECK(g.player(0).stuns == 1);
    // It lies at least 1.5 s (90 steps) and gets up over 0.8 s (48 steps).
    CHECK(stun_steps >= 90);
    CHECK(getting_up_steps >= 46 && getting_up_steps <= 52);
    // And walks again: the capsule is back, and the player went somewhere.
    CHECK(g.world().character_enabled(g.player(0).character));
    CHECK(length(g.feet(0) - at) > 2.0f);
    // The ragdoll is put away again (animated): its bodies cost nothing.
    CHECK(g.world().ragdoll_state(g.player(0).ragdoll).mode == RagdollMode3D::Animated);
}

void test_death_respawns_at_the_camp() {
    Game g(test_seed);
    settle(g);
    // Reach camp 3 (it counts when the feet are in it), then fall from 40 m above it.
    const Camp& camp = g.level().camps[3];
    g.debug_teleport(0, camp.spawn + Vec3f{0.0f, 0.2f, 0.0f});
    settle(g, 30);
    CHECK(g.player(0).camp == 3);
    g.debug_teleport(0, camp.spawn + Vec3f{0.0f, 40.0f, 0.0f});
    bool died = false, respawned = false;
    for (int i = 0; i < 600; ++i) {
        g.step({});
        for (const GameEvent& e : g.events()) {
            died = died || e.kind == GameEventKind::Died;
            respawned = respawned || e.kind == GameEventKind::Respawned;
        }
    }
    CHECK(died && respawned);
    const Player& p = g.player(0);
    CHECK(p.deaths == 1);
    CHECK(p.state == PlayerState::Alive);
    CHECK_NEAR(p.health, 100.0f, 0.001f);
    CHECK(length(g.feet(0) - camp.spawn) < 2.5f);
    CHECK_NEAR(g.stamina(0), 150.0f, 0.01f);
    CHECK(g.world().character_enabled(p.character));
    // The kill plane: below it the player is back at the camp too.
    g.debug_teleport(0, {camp.spawn.x, g.level().kill_y - 3.0f, camp.spawn.z});
    bool lost = false;
    for (int i = 0; i < 120 && !lost; ++i) {
        g.step({});
        for (const GameEvent& e : g.events()) lost = lost || e.kind == GameEventKind::Died;
    }
    CHECK(lost);
    CHECK(length(g.feet(0) - camp.spawn) < 2.5f);
    CHECK(g.player(0).deaths == 2);
}

// ---- the rope, the camera, the scripted runs ------------------------------------------------------------------------------------

void test_rope_climb() {
    const Observed& o = climb();
    CHECK(o.rope_steps > 200);
    // The rope is 8 m long and hangs from 2 m over the lip: the climb up it is most of 6 m.
    CHECK(o.rope_gain > 4.0f);
    CHECK(o.report.deaths == 0);
    // Seen from the bot's own count: the rope leg was done.
    const SummitLevel level = generate_level(test_seed);
    int rope_leg = -1;
    for (std::size_t i = 0; i < level.route.size(); ++i) {
        if (level.route[i].kind == LegKind::Rope) rope_leg = int(i);
    }
    CHECK(rope_leg > 0 && o.report.legs_done > rope_leg);
    // The rope's physics: its anchor is where the level says; a body hangs from it with the length asked.
    Game g(test_seed);
    const RopeHandle3D r = g.ropes()[0];
    const RopeState3D st = g.world().rope_state(r);
    CHECK_NEAR(st.rest_length, level.ropes[0].length, 0.01f);
    const std::vector<RopePoint3D> pts = g.world().rope_points(r);
    CHECK_NEAR(length(pts.front().position - level.ropes[0].anchor), 0.0f, 0.06f);
}

void test_camera_stays_out_of_the_rock() {
    const Observed& o = climb();
    CHECK(o.camera_steps > 8000);
    CHECK(o.camera_inside == 0);
    // It was shortened by rock somewhere (the chimney's wall behind the player) and never longer than the arm.
    CHECK(o.camera_obstructed > 50);
    CHECK(o.camera_farthest <= CameraSettings{}.distance + 1e-4f);
    // Easing out: never more than the rate allows in a step.
    CHECK(o.camera_worst_ease <= CameraSettings{}.ease_out * fixed_step + 1e-3f);
    // The first person view has no arm.
    Game g(test_seed);
    hold(g, {}, 30);
    CameraRig rig;
    rig.settings.first_person = true;
    rig.observe(g, 0);
    const CameraView v = rig.frame(g.world(), 1.0f, fixed_step);
    CHECK_NEAR(v.arm, 0.0f, 1e-6f);
    CHECK_NEAR(v.position.y - g.feet(0).y, 1.62f, 0.05f);
    // Third person: behind the player, 4.2 m along the view (nothing in the way on the open field).
    CameraRig third;
    third.observe(g, 0);
    const CameraView t = third.frame(g.world(), 1.0f, fixed_step);
    CHECK_NEAR(t.arm, CameraSettings{}.distance, 0.01f);
    const Vec3f fwd = look_vector(g.player(0).yaw, g.player(0).pitch);
    CHECK(dot(normalize(g.head(0) - t.position), fwd) > 0.9f);
    // It never writes to the game: the state is what it was.
    const u64 before = g.state_hash();
    for (int i = 0; i < 20; ++i) (void)third.frame(g.world(), 0.5f, 1.0f / 60.0f);
    CHECK(g.state_hash() == before);
}

void test_scripted_climb() {
    const Observed& o = climb();
    CHECK(o.report.summit);
    CHECK(!o.report.non_finite);
    CHECK(o.inside_steps == 0);
    CHECK(o.report.steps == expected_climb_steps);
    CHECK(o.report.steps < 12000);
    CHECK(o.report.final_hash == expected_climb_hash);
    CHECK(o.report.falls == 0 && o.report.stuns == 0 && o.report.deaths == 0);
    CHECK(o.summit_events == 1);
    CHECK(o.camp_events == 6);
    CHECK(o.pull_up_blocked == 0);
    CHECK_NEAR(o.report.highest, generate_level(test_seed).height(), 0.5f);
    // Every stage is climbed on other seeds too (the planner says so on paper; this is the run).
    for (u64 seed : {2ull, 3ull, 7ull, 10ull, 16ull, 23ull}) {
        const Observed other = observe(seed, "climb");
        CHECK(other.report.summit);
        CHECK(other.report.falls == 0);
        CHECK(other.inside_steps == 0);
        CHECK(other.camera_inside == 0);
        CHECK(other.report.steps < 12000);
        CHECK(other.report.min_stamina > 1.0f);
    }
}

void test_scripted_fall() {
    const Observed o = observe(test_seed, "fall");
    CHECK(o.report.summit);
    CHECK(o.report.falls >= 1);
    CHECK(o.report.stuns == 1);
    CHECK(o.got_up_events == 1);
    CHECK(o.report.deaths == 0);
    CHECK(!o.report.non_finite);
    CHECK(o.report.steps > expected_climb_steps);
    CHECK(o.report.steps < 14000);
    // Replayed from its log: the same final hash.
    Game g(test_seed);
    CHECK(replay(g, o.report.log) == o.report.final_hash);
}

void test_authored_holds_are_in_reach() {
    const SummitLevel level = generate_level(test_seed);
    // The hold line: holds in order up the slab, alternating sides, each at most 0.55 m above the one before and at least 0.2 m
    // (the hand's reach is 0.95 m from a shoulder 1.65 m up, so hand over hand it is a short reach).
    std::vector<const Piece*> holds;
    for (const Piece& p : level.pieces) {
        if (p.hold) holds.push_back(&p);
    }
    CHECK(holds.size() >= 8);
    for (std::size_t i = 1; i < holds.size(); ++i) {
        const f32 dy = holds[i]->centre.y - holds[i - 1]->centre.y;
        CHECK(dy > 0.2f && dy < 0.55f);
        const f32 dx = length(Vec3f{holds[i]->centre.x - holds[i - 1]->centre.x, 0.0f, holds[i]->centre.z - holds[i - 1]->centre.z});
        CHECK(dx > 0.1f && dx < 0.9f);
        CHECK(holds[i]->hold_quality > 0.5f);
    }
}

// Hand over hand up the hold line (a smooth slab no hand can take, with an authored hold every 0.45 m alternating sides): the
// test's own tiny state machine, not the route bot's, aims each free hand at the next hold and lets go of the one behind.
struct HoldClimb {
    int steps = 0;
    int taken = 0;       // holds gripped
    f32 top_y = 0.0f;    // highest feet
    bool mantled = false;
    bool fell = false;
};

HoldClimb climb_hold_line(u64 seed, int max_steps = 6000) {
    HoldClimb out;
    Game g(seed);
    const SummitLevel& level = g.level();
    std::vector<const Piece*> holds;
    for (const Piece& p : level.pieces) {
        if (p.hold) holds.push_back(&p);
    }
    if (holds.size() < 4) return out;
    const Vec3f normal = holds[0]->rotation.rotate({0.0f, 0.0f, 1.0f});    // out of the wall
    const Vec3f right{normal.z, 0.0f, -normal.x};                          // to the right of someone facing the wall
    // The places the hands go for, in order: each hold's front, then the lip's edge (left hand, right hand).
    std::vector<Vec3f> targets;
    for (const Piece* h : holds) targets.push_back(h->centre + normal * 0.08f);
    const Piece* lip = nullptr;
    f32 nearest = 1e9f;
    for (const Piece& p : level.pieces) {
        const f32 d = length(p.centre - holds.back()->centre);
        if (p.kind == PieceKind::Lip && p.centre.y > holds.back()->centre.y && d < nearest) {
            nearest = d;
            lip = &p;
        }
    }
    const std::size_t hold_count = targets.size();
    if (lip != nullptr) {
        const Vec3f edge = lip->centre + normal * lip->half.z;
        const Vec3f mid = Vec3f{holds.back()->centre.x, 0.0f, holds.back()->centre.z};
        const f32 along = dot(mid - Vec3f{edge.x, 0.0f, edge.z}, right);
        const Vec3f base = Vec3f{edge.x, lip->centre.y + lip->half.y - 0.08f, edge.z} + right * along;
        targets.push_back(base - right * 0.2f);
        targets.push_back(base + right * 0.2f);
    }
    // The bench the slab stands over: the second climb leg's foot (the second pitch) is at its height.
    f32 bench_y = holds[0]->centre.y - 1.7f;
    for (const Leg& l : level.route) {
        if (l.kind == LegKind::Climb && std::fabs((l.from.y + 1.7f) - holds[0]->centre.y) < 0.8f) bench_y = l.from.y;
    }
    // Stand 0.8 m off the wall below the first hold, facing it.
    const Vec3f stand = Vec3f{holds[0]->centre.x, bench_y, holds[0]->centre.z} + normal * 0.8f;
    g.debug_teleport(0, stand);
    hold(g, {}, 30);
    const f32 yaw = std::atan2(normal.x, normal.z) * 180.0f / pi; // looking along -normal: yaw_to(-normal)
    const auto aim = [&](PlayerCommand& c, Vec3f dir) {
        const f32 want_yaw = std::atan2(-dir.x, -dir.z) * 180.0f / pi;
        const f32 want_pitch = std::asin(std::clamp(dir.y / std::max(length(dir), 1e-4f), -1.0f, 1.0f)) * 180.0f / pi;
        f32 e = std::fmod(want_yaw - g.player(0).yaw + 540.0f, 360.0f) - 180.0f;
        c.look.x = -std::clamp(e, -10.0f, 10.0f);
        c.look.y = std::clamp(want_pitch - g.player(0).pitch, -8.0f, 8.0f);
    };
    (void)yaw;
    (void)hold_count;
    std::size_t next = 0;      // the hold the free hand goes for
    bool held[2] = {false, false};
    constexpr std::size_t none = ~std::size_t{0};
    std::size_t on[2] = {none, none};
    int stall = 0;
    for (int s = 0; s < max_steps; ++s) {
        const CharacterState3D cs = g.character_state(0);
        const TraversalState3D ts = g.traversal(0);
        PlayerCommand c;
        // Which limb takes the next hold: the one on its side of the slab's middle line.
        const auto side_of = [&](std::size_t k) -> int { return int(k % 2); }; // the line alternates, the first on the left
        for (int limb = 0; limb < 2; ++limb) held[limb] = ts.grips[limb].phase == GripPhase3D::Held;
        // Taken: the limb holds the hold wanted next (its grip within 0.3 m of the hold's front), before any command is made.
        if (next < targets.size()) {
            const int limb = side_of(next);
            const Vec3f target = targets[next];
            if (held[limb] && length(ts.grips[limb].point - target) < 0.3f && on[limb] != next) {
                on[limb] = next;
                ++next;
                stall = 0;
            }
        }
        out.taken = std::max<int>(out.taken, int(next));
        out.top_y = std::max(out.top_y, cs.position.y);
        // A pull-up over the lip at the top.
        if (ts.mode == TraversalMode3D::Mantling) out.mantled = true;
        if (out.mantled && ts.mode == TraversalMode3D::Walking) break;
        if (!ts.hanging() && cs.ground != CharacterGround3D::OnGround && s > 60) out.fell = true;
        // Looking at the hold wanted next, from the shoulder of the limb that goes for it.
        if (next < targets.size()) {
            const int limb = side_of(next);
            const Vec3f shoulder = cs.position + right * (limb == 0 ? -0.2f : 0.2f) + Vec3f{0.0f, 1.65f, 0.0f};
            const Vec3f target = targets[next];
            aim(c, target - shoulder);
            // Hold what holds; the limb going for `next` asks, after the hold behind it (two back) was let go of.
            c.grab_left = held[0] && on[0] != none && on[0] + 2 > next;
            c.grab_right = held[1] && on[1] != none && on[1] + 2 > next;
            const bool behind_free = next < 2 || !held[limb] || on[limb] == none || on[limb] + 2 <= next;
            if (limb == 0) c.grab_left = c.grab_left || behind_free;
            else c.grab_right = c.grab_right || behind_free;
            // The hold behind is let go of by not asking for it: held[limb] on an old hold, `next` two ahead of it.
            if (next >= 2 && held[limb] && on[limb] != none && on[limb] + 2 == next) {
                (limb == 0 ? c.grab_left : c.grab_right) = false;
            }
        } else {
            // The hands are on the lip: up and over (the game pulls up by itself).
            c.grab_left = c.grab_right = true;
            c.move.y = 1.0f;
            aim(c, -normal + Vec3f{0.0f, 0.3f, 0.0f});
        }
        if (verbose && s % 30 == 0) {
            std::printf("    s %d feet (%.2f %.2f %.2f) next %zu phase %d/%d kind %d/%d held %u yaw %.1f pitch %.1f", s, double(cs.position.x), double(cs.position.y),
                        double(cs.position.z), next, int(ts.grips[0].phase), int(ts.grips[1].phase), int(ts.grips[0].kind), int(ts.grips[1].kind), ts.held,
                        double(g.player(0).yaw), double(g.player(0).pitch));
            if (next < targets.size()) std::printf(" target (%.2f %.2f %.2f)", double(targets[next].x), double(targets[next].y), double(targets[next].z));
            std::printf(" cmd grabs %d%d look %.1f %.1f\n", int(c.grab_left), int(c.grab_right), double(c.look.x), double(c.look.y));
        }
        if (++stall > 600) break;
        g.step({&c, 1});
        ++out.steps;
    }
    return out;
}

void test_hold_line_by_hand_over_hand() {
    const HoldClimb h = climb_hold_line(test_seed);
    if (verbose) std::printf("  holds taken %d, top %.2f, mantled %d, fell %d, steps %d\n", h.taken, double(h.top_y), int(h.mantled), int(h.fell), h.steps);
    CHECK(h.taken >= 10);
    CHECK(h.mantled);
    CHECK(!h.fell);
}

void test_cost_of_a_step() {
    // Not a pass or fail but a number for the plan: the run's wall time and steps a second.
    Game g(test_seed);
    Bot bot(g);
    const auto start = Clock::now();
    u64 steps = 0;
    while (!bot.finished() && !g.player(0).summit && steps < 20000) {
        const PlayerCommand c = bot.next();
        g.step({&c, 1});
        ++steps;
    }
    const double s = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("summit_tests: the climb: %llu steps in %.2f s (%.0f steps a second, %.0f us a step)\n", (unsigned long long)steps, s,
                double(steps) / s, s / double(steps) * 1e6);
    CHECK(steps == expected_climb_steps);
    CHECK(s < 30.0);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-v") verbose = true;
    }
    const auto run = [](const char* name, void (*f)()) {
        const auto t0 = Clock::now();
        const int before = failures;
        f();
        if (verbose || failures != before) {
            std::printf("summit_tests: %-48s %s (%.2f s)\n", name, failures == before ? "ok" : "FAILED",
                        std::chrono::duration<double>(Clock::now() - t0).count());
        }
    };
    run("level determinism", test_level_determinism);
    run("colliders and meshes are one list", test_colliders_and_meshes_are_one_list);
    run("route is climbable by the stamina model", test_route_is_climbable_by_the_stamina_model);
    run("stamina numbers", test_stamina_numbers);
    run("look is clamped", test_look_is_clamped);
    run("mapper", test_mapper);
    run("gamepad poll without a device", test_gamepad_poll_without_a_device);
    run("command log files", test_command_log_files);
    run("replay gives the same hash", test_replay_gives_the_same_hash);
    run("save and restore", test_save_and_restore);
    run("state covers rng, wind and players", test_state_covers_rng_wind_and_players);
    run("frame rate independence", test_frame_rate_independence);
    run("step purity", test_step_purity);
    run("two players", test_two_players);
    run("short fall is free", test_short_fall_is_free);
    run("fall stuns, ragdolls, gets up", test_fall_stuns_ragdolls_gets_up);
    run("death respawns at the camp", test_death_respawns_at_the_camp);
    run("rope climb", test_rope_climb);
    run("camera stays out of the rock", test_camera_stays_out_of_the_rock);
    run("scripted climb", test_scripted_climb);
    run("scripted fall", test_scripted_fall);
    run("authored holds are in reach", test_authored_holds_are_in_reach);
    run("hold line by hand over hand", test_hold_line_by_hand_over_hand);
    run("cost of a step", test_cost_of_a_step);
    if (failures != 0) {
        std::fprintf(stderr, "summit_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("summit_tests: all passed\n");
    return 0;
}
