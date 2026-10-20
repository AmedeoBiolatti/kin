// Grab and climb of kin/physics3d (kin/physics3d/traversal3d.hpp), without a GPU: behaviours against hand-computed values
// on the climb level (tests/climb_level.hpp), determinism with 0, 1 and 4 workers, saved states. docs/physics_3d.md
// ("Grab and climb").

#include "climb_level.hpp"

#include <kin/core/jobs.hpp>
#include <kin/core/json_value.hpp>
#include <kin/physics3d/debug_draw3d.hpp>
#include <kin/physics3d/world3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace kin;

int failures = 0;
bool verbose = false;

#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        if (!(cond)) {                                                                                \
            ++failures;                                                                               \
            std::fprintf(stderr, "physics3d_traversal_tests:%d: CHECK failed: %s\n", __LINE__, #cond); \
        }                                                                                             \
    } while (0)

#define CHECK_NEAR(value, expected, eps)                                                                              \
    do {                                                                                                              \
        const double v_ = static_cast<double>(value), e_ = static_cast<double>(expected);                             \
        if (verbose) std::printf("  %s = %.4f (expected %.4f +- %.4f)\n", #value, v_, e_, static_cast<double>(eps));    \
        if (!(std::fabs(v_ - e_) <= static_cast<double>(eps))) {                                                      \
            ++failures;                                                                                               \
            std::fprintf(stderr, "physics3d_traversal_tests:%d: %s = %.5f, expected %.5f +- %.5f\n", __LINE__, #value, v_, \
                         e_, static_cast<double>(eps));                                                               \
        }                                                                                                             \
    } while (0)

constexpr f32 dt = 1.0f / 60.0f;

World3DConfig serial() {
    World3DConfig c;
    c.single_threaded = true;
    return c;
}

CharacterDesc3D climber() {
    CharacterDesc3D d;
    d.traversal.enabled = true;
    return d;
}

// A world with the climb level and a character with traversal on.
struct Rig {
    World3D world;
    climb_level::Level level;
    Character3D hero;
    CharacterInput3D input;
    TraversalInput3D tin;
    int frame = 0;
    std::vector<CharacterEvent3D> events;

    explicit Rig(World3DConfig cfg = serial(), CharacterDesc3D desc = climber(), Vec3f feet = {0.0f, 0.0f, 0.7f},
                 Quatf frame_rotation = {}, bool with_level = true)
        : world(cfg) {
        if (with_level) {
            level = climb_level::build(world, frame_rotation);
        }
        desc.position = feet;
        hero = world.create_character(desc);
        CHECK(hero.valid());
        input.forward = {0.0f, 0.0f, -1.0f};
        world.set_character_input(hero, input);
    }

    void step(int n = 1) {
        for (int i = 0; i < n; ++i) {
            world.set_character_input(hero, input);
            world.set_traversal_input(hero, tin);
            level.drive(world, frame, dt);
            world.step(dt);
            for (const CharacterEvent3D& e : world.drain_character_events()) {
                events.push_back(e);
            }
            ++frame;
        }
    }
    CharacterState3D state() const { return world.character(hero); }
    TraversalState3D trav() const { return world.traversal(hero); }
    int count(CharacterEventKind3D kind) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const auto& e) { return e.kind == kind; }));
    }
};


// Runs until `done` is true, at most `limit` steps; returns the steps taken (limit when it never was).
template <class F>
int run_until(Rig& r, int limit, F done) {
    for (int i = 0; i < limit; ++i) {
        if (done()) {
            return i;
        }
        r.step();
    }
    return limit;
}

// The usual opening: both hands on the wall at x = 0, reaching up and forward.
void grab_wall(Rig& r, Vec3f reach = {0.0f, 0.45f, -1.0f}) {
    r.tin.grab = {true, true};
    r.tin.reach = reach;
}

// From the ground to hanging by both hands from the top of the wall at x = 0.
void climb_to_top(Rig& r) {
    grab_wall(r);
    r.step(30);
    r.input.move = {0.0f, 1.0f};
    for (int i = 0; i < 400; ++i) {
        const TraversalState3D t = r.trav();
        if (t.held == 2 && t.grips[0].ledge && t.grips[1].ledge) {
            break;
        }
        r.step();
    }
    r.input.move = {};
    r.step(30);
}

// --- the probe --------------------------------------------------------------------------------------------------------

void test_probe() {
    World3D world(serial());
    const climb_level::Level level = climb_level::build(world);
    GripProbeDesc3D p;
    p.origin = {0.0f, 1.45f, 0.7f};
    p.direction = {0.0f, 0.0f, -1.0f};
    p.reach = 0.8f;
    // A plain wall in front: one surface candidate on its face, the normal out of it, a frame across it.
    std::vector<GripCandidate3D> found = world.probe_grips(p);
    CHECK(!found.empty());
    if (!found.empty()) {
        const GripCandidate3D& c = found.front();
        CHECK(c.body == level.wall);
        CHECK(c.kind == GripKind3D::Surface);
        CHECK_NEAR(c.point.z, 0.0, 1e-3);
        CHECK_NEAR(c.normal.z, 1.0, 1e-3);
        CHECK_NEAR(c.right.x, 1.0, 1e-3);
        CHECK_NEAR(c.surface_up.y, 1.0, 1e-3);
        CHECK(c.user_id == 200 + 1);
        CHECK_NEAR(c.distance, 0.7, 0.02);
        CHECK(c.quality > 0.99f);
        CHECK(!c.ledge);
    }
    // The same probe twice gives the same list, bit for bit.
    const std::vector<GripCandidate3D> again = world.probe_grips(p);
    CHECK(again.size() == found.size());
    for (std::size_t i = 0; i < std::min(again.size(), found.size()); ++i) {
        CHECK(again[i].point == found[i].point && again[i].score == found[i].score && again[i].body == found[i].body);
    }
    // Out of reach: nothing.
    p.reach = 0.5f;
    CHECK(world.probe_grips(p).empty());
    p.reach = 0.8f;
    // The layer mask: the wall is a static body (layer 0); a mask without it finds nothing.
    p.mask = all_layers3d & ~layer_bit(layers3d::Static);
    CHECK(world.probe_grips(p).empty());
    // And a mask with only it does.
    p.mask = layer_bit(layers3d::Static);
    CHECK(!world.probe_grips(p).empty());
    p.mask = all_layers3d;
    // The ground is to be stood on, not held.
    GripProbeDesc3D down = p;
    down.origin = {5.0f, 1.0f, 3.0f};
    down.direction = {0.0f, -1.0f, 0.0f};
    down.reach = 1.5f;
    CHECK(world.probe_grips(down).empty());
    // A surface that is not grippable offers nothing; with a hold in front of it, the hold.
    GripProbeDesc3D smooth;
    smooth.origin = {20.0f, 1.0f, 0.7f};
    smooth.direction = {0.0f, 0.0f, -1.0f};
    smooth.reach = 0.8f;
    CHECK(world.probe_grips(smooth).empty());
    smooth.origin = {19.75f, 1.7f, 0.7f};
    found = world.probe_grips(smooth);
    CHECK(!found.empty());
    if (!found.empty()) {
        CHECK(found.front().kind == GripKind3D::Hold);
        CHECK(found.front().body == level.holds[0]);
        CHECK_NEAR(found.front().point.z, 0.15, 1e-3);
    }
    // With holds not wanted the hold is only a body: an ordinary surface (its material is the default one).
    smooth.holds = false;
    found = world.probe_grips(smooth);
    CHECK(!found.empty() && found.front().kind != GripKind3D::Hold && found.front().body == level.holds[0]);
    smooth.holds = true;
    // The surfaces not wanted and holds wanted: still the hold.
    smooth.surfaces = false;
    found = world.probe_grips(smooth);
    CHECK(!found.empty() && found.front().kind == GripKind3D::Hold);
    smooth.surfaces = true;
    // The surface's tag, slip and drain are in the candidate.
    GripProbeDesc3D ice;
    ice.origin = {50.0f, 1.45f, 0.7f};
    ice.direction = {0.0f, 0.0f, -1.0f};
    found = world.probe_grips(ice);
    CHECK(!found.empty());
    if (!found.empty()) {
        CHECK(found.front().surface_tag == 1);
        CHECK_NEAR(found.front().slip, 1.0, 1e-6);
        CHECK_NEAR(found.front().drain, 2.0, 1e-6);
    }
    // A ledge: the top edge of the wall within reach above the hand, with room over it (and not under the low roof).
    GripProbeDesc3D ledge;
    ledge.origin = {0.0f, 3.7f, 0.7f};
    ledge.direction = {0.0f, 0.0f, -1.0f};
    ledge.reach = 0.8f;
    found = world.probe_grips(ledge);
    CHECK(!found.empty());
    if (!found.empty()) {
        const GripCandidate3D& c = found.front();
        CHECK(c.kind == GripKind3D::Ledge && c.ledge);
        CHECK_NEAR(c.point.y, 4.0, 1e-3);
        CHECK(c.clear);
        CHECK_NEAR(c.ledge_top.y, 4.005, 0.01);
        CHECK_NEAR(c.ledge_top.z, -0.35, 0.02); // a radius and 5 cm in from the lip
    }
    ledge.origin = {60.0f, 2.7f, 0.7f};
    found = world.probe_grips(ledge);
    CHECK(!found.empty());
    if (!found.empty()) {
        CHECK(found.front().kind == GripKind3D::Ledge);
        CHECK(!found.front().clear); // the roof is 1.4 m over the top
    }
    // Ledges not wanted: the same wall is a surface.
    ledge.ledges = false;
    found = world.probe_grips(ledge);
    CHECK(!found.empty() && found.front().kind == GripKind3D::Surface);
    // A fan of casts finds a surface that is beside the line.
    GripProbeDesc3D off;
    off.origin = {2.9f, 1.45f, 0.7f};
    off.direction = {0.6f, 0.0f, -1.0f};
    off.reach = 1.0f;
    CHECK(!world.probe_grips(off).empty());
    off.spread = 0.0f;
    // (the straight cast goes past the end of the wall at x = 3 + ... : still the wall's end face may be hit; no claim)
    // max_candidates.
    GripProbeDesc3D many = p;
    many.origin = {2.8f, 1.45f, 0.4f};
    many.direction = {0.0f, 0.0f, -1.0f};
    many.max_candidates = 1;
    CHECK(world.probe_grips(many).size() <= 1);
    // Invalid input: nothing, no crash.
    GripProbeDesc3D bad;
    bad.direction = {0, 0, 0};
    CHECK(world.probe_grips(bad).empty());
    bad.direction = {0, 0, -1};
    bad.reach = std::nanf("");
    CHECK(world.probe_grips(bad).empty());
}

// A ledge whose top is another body (a face of thin slabs with the rock behind them, a mountain of boxes): found when the held
// face runs up to the top and ends there; not across a gap, not on a top the face rises past, not on a roof over a wall.
Body3D add_block(World3D& w, Vec3f lo, Vec3f hi, u64 user_id) {
    return w.create_body({.shape = Shape3D::box((hi - lo) * 0.5f), .motion = Body3DMotion::Static, .position = (lo + hi) * 0.5f,
                          .user_id = user_id});
}

// The best ledge candidate on `body` of a probe from (0, y, 0.7) at the face z = 0 (none: body invalid).
GripCandidate3D ledge_on(const World3D& w, Body3D body, f32 y = 2.7f, f32 search = 0.5f) {
    GripProbeDesc3D p;
    p.origin = {0.0f, y, 0.7f};
    p.direction = {0.0f, 0.0f, -1.0f};
    p.reach = 0.8f;
    p.ledge_search_height = search;
    for (const GripCandidate3D& c : w.probe_grips(p)) {
        if (c.ledge && c.body == body) {
            return c;
        }
    }
    return {};
}

void test_ledge_across_bodies() {
    // Flush: a slab 6 cm thick (thinner than the 10 cm the ledge ray looks behind the face) and the block behind it, their
    // tops level. The hand holds the slab, the lip is on its face, and what is stood on is the block.
    {
        World3D w(serial());
        const Body3D slab = add_block(w, {-2.0f, 0.0f, -0.06f}, {2.0f, 3.0f, 0.0f}, 1);
        const Body3D block = add_block(w, {-2.0f, 0.0f, -3.06f}, {2.0f, 3.0f, -0.06f}, 2);
        const GripCandidate3D c = ledge_on(w, slab);
        CHECK(c.body == slab && c.kind == GripKind3D::Ledge && c.ledge_body == block);
        CHECK_NEAR(c.point.y, 3.0, 1e-3);
        CHECK_NEAR(c.point.z, 0.0, 1e-3);
        CHECK(c.clear);
        CHECK_NEAR(c.ledge_top.y, 3.005, 0.01);
        CHECK_NEAR(c.ledge_top.z, -0.35, 0.02);
    }
    // The block's top 3 cm under the slab's: the face ends within 5 cm of it, still its ledge (at the top found).
    {
        World3D w(serial());
        const Body3D slab = add_block(w, {-2.0f, 0.0f, -0.06f}, {2.0f, 3.0f, 0.0f}, 1);
        const Body3D block = add_block(w, {-2.0f, 0.0f, -3.06f}, {2.0f, 2.97f, -0.06f}, 2);
        const GripCandidate3D c = ledge_on(w, slab);
        CHECK(c.body == slab && c.ledge_body == block);
        CHECK_NEAR(c.point.y, 2.97, 1e-3);
    }
    // Not when the slab rises 20 cm past the block's top (a parapet: that top is not the face's edge), nor across a gap of
    // 30 cm between the slab and the block (the ray falls into it), nor when the slab ends 20 cm under the block's top (the
    // face has a gap; the block's own face up there has its own ledge).
    {
        World3D w(serial());
        const Body3D slab = add_block(w, {-2.0f, 0.0f, -0.06f}, {2.0f, 3.0f, 0.0f}, 1);
        add_block(w, {-2.0f, 0.0f, -3.06f}, {2.0f, 2.8f, -0.06f}, 2);
        CHECK(!ledge_on(w, slab).body.valid());
    }
    {
        World3D w(serial());
        const Body3D slab = add_block(w, {-2.0f, 0.0f, -0.06f}, {2.0f, 3.0f, 0.0f}, 1);
        add_block(w, {-2.0f, 0.0f, -3.36f}, {2.0f, 3.0f, -0.36f}, 2);
        CHECK(!ledge_on(w, slab).body.valid());
    }
    {
        World3D w(serial());
        const Body3D slab = add_block(w, {-2.0f, 0.0f, -0.06f}, {2.0f, 2.8f, 0.0f}, 1);
        add_block(w, {-2.0f, 0.0f, -3.06f}, {2.0f, 3.0f, -0.06f}, 2);
        CHECK(!ledge_on(w, slab, 2.6f).body.valid());
    }
    // A roof 2 m over a wall (out over its face, or set back from it as a terrace): a ledge search tall enough to start over
    // it meets the roof's top first, which is not the wall's ledge. Without the roof the same search finds the wall's own top.
    for (int roof = 0; roof < 3; ++roof) {
        World3D w(serial());
        const Body3D wall = add_block(w, {-2.0f, 0.0f, -2.0f}, {2.0f, 3.0f, 0.0f}, 1);
        if (roof == 1) add_block(w, {-2.0f, 5.0f, -1.0f}, {2.0f, 5.2f, 1.6f}, 3);
        if (roof == 2) add_block(w, {-2.0f, 5.0f, -1.5f}, {2.0f, 5.2f, -0.04f}, 3);
        const GripCandidate3D c = ledge_on(w, wall, 2.7f, 2.6f);
        if (roof == 0) {
            CHECK(c.body == wall && c.ledge_body == wall);
            CHECK_NEAR(c.point.y, 3.0, 1e-3);
        } else {
            CHECK(!c.body.valid());
        }
    }
    // Climbed: up the slab's face the hands find the lip and take it, and the pull-up stands the character on the block.
    {
        Rig r(serial(), climber(), {0.0f, 0.0f, 0.7f}, {}, false);
        r.world.create_body({.shape = Shape3D::box({20.0f, 0.5f, 20.0f}), .motion = Body3DMotion::Static, .position = {0.0f, -0.5f, 0.0f}});
        const Body3D slab = add_block(r.world, {-2.0f, 0.0f, -0.06f}, {2.0f, 3.0f, 0.0f}, 1);
        const Body3D block = add_block(r.world, {-2.0f, 0.0f, -3.06f}, {2.0f, 3.0f, -0.06f}, 2);
        r.step(30);
        climb_to_top(r);
        TraversalState3D t = r.trav();
        CHECK(t.held == 2 && t.grips[0].ledge && t.grips[1].ledge);
        CHECK(t.grips[0].body == slab);
        CHECK_NEAR(t.grips[0].point.y, 3.0, 2e-3);
        r.tin.pull_up = true;
        r.step(62);
        t = r.trav();
        CHECK(t.mode == TraversalMode3D::Walking && r.count(CharacterEventKind3D::MantleFinished) == 1);
        r.step(30);
        CHECK(r.state().ground == CharacterGround3D::OnGround && r.state().ground_body == block);
        CHECK_NEAR(r.state().position.y, 3.0, 0.02);
    }
}

// Pushed while hanging (World3D::add_character_impulse): the swing takes it, the grips hold, and it settles back.
void test_pushed_while_hanging() {
    Rig r;
    grab_wall(r);
    r.step(90);
    CHECK(r.trav().mode == TraversalMode3D::Hanging && r.trav().held == 2);
    const Vec3f at = r.state().position;
    r.world.add_character_impulse(r.hero, {70.0f * 2.0f, 0.0f, 0.0f});
    r.step();
    CHECK(r.state().position.x > at.x + 0.02f);
    CHECK(r.trav().mode == TraversalMode3D::Hanging);
    r.step(20);
    CHECK(r.state().position.x > at.x + 0.1f);
    r.step(240);
    CHECK(r.trav().mode == TraversalMode3D::Hanging && r.trav().held == 2);
    CHECK_NEAR(r.state().position.x, at.x, 0.02);
    CHECK(r.count(CharacterEventKind3D::Fell) == 0);
}

// --- grips and hanging -----------------------------------------------------------------------------------------------

void test_climb_and_mantle() {
    Rig r;
    // Standing at rest first: nothing held, walking, full of stamina.
    r.step(30);
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    CHECK_NEAR(r.trav().resource.value, 100.0, 1e-3);
    grab_wall(r);
    // The hands reach for 0.15 s, then hold.
    r.step(2);
    CHECK(r.trav().grips[0].phase == GripPhase3D::Reaching);
    CHECK(r.trav().grips[0].reach_progress > 0.0f && r.trav().grips[0].reach_progress < 1.0f);
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    r.step(12);
    TraversalState3D t = r.trav();
    CHECK(t.grips[0].phase == GripPhase3D::Held && t.grips[1].phase == GripPhase3D::Held);
    CHECK(t.mode == TraversalMode3D::Hanging && t.held == 2 && t.hanging());
    CHECK(r.state().traversal_mode == TraversalMode3D::Hanging);
    CHECK(t.grips[0].body == r.level.wall && t.grips[0].body_user_id == 201);
    CHECK_NEAR(t.grips[0].point.z, 0.0, 1e-3);
    CHECK(t.grips[0].point.x < t.grips[1].point.x); // left, right
    CHECK_NEAR(t.grips[0].strength, 1.0, 1e-3);
    CHECK_NEAR(t.resource.value, 100.0 - 2 * 2.0 /* two grabs */ - 0.0, 3.5);
    // It hangs a capsule's width out from the wall (it swings in to it over half a second).
    r.step(40);
    CHECK_NEAR(r.state().position.z, 0.3 + 0.04, 0.03);
    // Climb: the move command's y goes up the surface.
    r.input.move = {0.0f, 1.0f};
    const int climbed = run_until(r, 400, [&] { return r.trav().grips[0].ledge && r.trav().grips[1].ledge; });
    CHECK(climbed < 400);
    // 4 m less what it reached up at the start, at 0.6 m/s.
    const f32 start_y = 1.6f;
    CHECK_NEAR(static_cast<f32>(climbed) * dt * 0.6f, 4.0f - start_y, 0.5);
    t = r.trav();
    CHECK(t.grips[0].kind == GripKind3D::Ledge && t.grips[1].kind == GripKind3D::Ledge);
    CHECK_NEAR(t.grips[0].point.y, 4.0, 2e-3);
    CHECK_NEAR(t.grips[1].point.y, 4.0, 2e-3);
    CHECK(t.resource.value < 70.0f && t.resource.value > 30.0f);
    r.input.move = {};
    r.step(40);
    // Hanging from the lip: the feet 1.7 m under the hands, still.
    CHECK_NEAR(r.state().position.y, 4.0 - 1.7, 0.03);
    CHECK(r.count(CharacterEventKind3D::MantleStarted) == 0);
    // Pull up: a transition over 0.7 s, and walking on the top.
    const f32 before = r.trav().resource.value;
    r.tin.pull_up = true;
    r.step(2);
    CHECK(r.trav().mode == TraversalMode3D::Mantling);
    CHECK(r.count(CharacterEventKind3D::MantleStarted) == 1);
    r.step(30);
    CHECK_NEAR(r.trav().mantle_progress, 32.0 / 42.0, 0.06);
    r.step(30);
    t = r.trav();
    CHECK(t.mode == TraversalMode3D::Walking && t.held == 0);
    CHECK(r.count(CharacterEventKind3D::MantleFinished) == 1);
    CHECK_NEAR(r.state().position.y, 4.0, 0.02);
    CHECK_NEAR(r.state().position.z, -0.4, 0.03);
    CHECK(r.state().ground == CharacterGround3D::OnGround || r.state().ground == CharacterGround3D::InAir);
    r.step(30);
    CHECK(r.state().ground == CharacterGround3D::OnGround);
    CHECK(r.state().ground_user_id == 201);
    CHECK(t.resource.value < before); // the pull-up cost something
    // Events: two grips, one mantle, both hands released for it, no fall, no exhaustion.
    CHECK(r.count(CharacterEventKind3D::Gripped) == 2);
    CHECK(r.count(CharacterEventKind3D::GripReleased) == 2);
    CHECK(r.count(CharacterEventKind3D::Fell) == 0);
    CHECK(r.count(CharacterEventKind3D::Exhausted) == 0);
    int released_by_mantle = 0;
    for (const CharacterEvent3D& e : r.events) {
        if (e.kind == CharacterEventKind3D::GripReleased && e.reason == static_cast<u8>(GripRelease3D::Mantle)) {
            ++released_by_mantle;
        }
    }
    CHECK(released_by_mantle == 2);
    // Walking again: it can walk away over the top.
    r.input.move = {0.0f, 1.0f};
    r.tin.grab = {false, false};
    r.tin.pull_up = false;
    r.step(8);
    CHECK(r.state().position.z < -0.6f);
    CHECK_NEAR(r.state().position.y, 4.0, 0.03);
}

void test_phases_and_latch() {
    Rig r;
    r.step(20);
    grab_wall(r);
    r.step(5);
    CHECK(r.trav().grips[0].phase == GripPhase3D::Reaching);
    // Letting go while reaching cancels, with no grip and no events.
    r.tin.grab = {false, false};
    r.step(2);
    CHECK(r.trav().grips[0].phase == GripPhase3D::Free);
    CHECK(r.count(CharacterEventKind3D::Gripped) == 0);
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    // One hand only: it hangs by one.
    r.tin.grab = {false, true};
    r.step(20);
    TraversalState3D t = r.trav();
    CHECK(t.grips[0].phase == GripPhase3D::Free && t.grips[1].phase == GripPhase3D::Held);
    CHECK(t.held == 1 && t.mode == TraversalMode3D::Hanging);
    // The other hand takes hold of the wall away from the first (not on top of it).
    r.tin.grab = {true, true};
    r.step(20);
    t = r.trav();
    CHECK(t.held == 2);
    CHECK(length(t.grips[0].point - t.grips[1].point) >= 0.2f);
    // Letting one go: the phase is Released for one step, then free; the other keeps the hang.
    r.tin.grab = {false, true};
    r.step(1);
    CHECK(r.trav().grips[0].phase == GripPhase3D::Released);
    r.step(1);
    CHECK(r.trav().grips[0].phase == GripPhase3D::Free);
    CHECK(r.trav().held == 1);
    // The command is a level: still given, the hand takes hold again.
    r.tin.grab = {true, true};
    r.step(25);
    CHECK(r.trav().held == 2);
    // Drop lets go of everything at once and nothing is taken while it is given; the character falls and lands.
    r.tin.drop = true;
    r.step(1);
    CHECK(r.trav().held == 0 && r.trav().mode == TraversalMode3D::Walking);
    r.step(30);
    CHECK(r.trav().held == 0);
    CHECK(r.count(CharacterEventKind3D::Fell) == 1);
    r.tin.drop = false;
    // The grab command is still given and was let go of by the character: no new grip until it is given up and given again.
    r.step(10);
    CHECK(r.trav().held == 0);
    r.tin.grab = {false, false};
    r.step(2);
    r.tin.grab = {true, true};
    r.step(20);
    CHECK(r.trav().held == 2);
}

void test_two_hands_one_hand_and_drain() {
    // Hanging from the lip by both hands: 7 a second; by one: 12 (it takes the whole load).
    Rig r;
    climb_to_top(r);
    CHECK(r.trav().held == 2);
    r.step(70); // past the regen delay of the last spend: what is spent now is only the hang
    const f32 a0 = r.trav().resource.value;
    r.step(60);
    const f32 two = a0 - r.trav().resource.value;
    CHECK_NEAR(two, 7.0, 0.3);
    r.tin.grab = {true, false};
    r.step(10);
    const f32 b0 = r.trav().resource.value;
    CHECK(r.trav().held == 1);
    r.step(60);
    const f32 one = b0 - r.trav().resource.value;
    CHECK_NEAR(one, 12.0, 0.3);
    CHECK_NEAR(one / two, 12.0 / 7.0, 0.06);
    CHECK(one > two * 1.5f);

    // The surface's drain multiplier: the same hang on wet rock costs double (a big tank: the climb is 4.5 s of it).
    CharacterDesc3D big = climber();
    big.traversal.resource.max = 400.0f;
    Rig w(serial(), big);
    w.world.set_surface_material(w.level.wall, {.grip = 1.0f, .slip = 0.0f, .drain = 2.0f, .tag = 7});
    climb_to_top(w);
    CHECK(w.trav().held == 2);
    CHECK(w.trav().grips[0].surface_tag == 7);
    w.step(70);
    const f32 c0 = w.trav().resource.value;
    w.step(60);
    CHECK_NEAR(c0 - w.trav().resource.value, 14.0, 0.5);
}

void test_resource() {
    ResourceConfig3D c;
    c.max = 10.0f;
    c.regen_rate = 4.0f;
    c.regen_delay = 0.5f;
    c.recover_fraction = 0.5f;
    Resource3D v;
    v.value = v.max = 10.0f;
    // Drain at 5 a second for a second: half.
    for (int i = 0; i < 60; ++i) {
        v.step(c, dt, 5.0f);
    }
    CHECK_NEAR(v.value, 5.0, 0.05);
    // At rest: nothing for 0.5 s (the delay), then 4 a second.
    for (int i = 0; i < 30; ++i) {
        v.step(c, dt, 0.0f);
    }
    CHECK_NEAR(v.value, 5.0, 1e-3);
    for (int i = 0; i < 30; ++i) {
        v.step(c, dt, 0.0f);
    }
    CHECK_NEAR(v.value, 5.0 + 4.0 * 0.5, 0.1);
    // Running out: the rising edge once, exhausted until half is back.
    int edges = 0;
    for (int i = 0; i < 120; ++i) {
        edges += v.step(c, dt, 20.0f) ? 1 : 0;
    }
    CHECK(edges == 1 && v.exhausted && v.value == 0.0f);
    for (int i = 0; i < 30 + 45; ++i) {
        v.step(c, dt, 0.0f);
    }
    CHECK(v.exhausted); // 0.75 s of regen: 1.0 of 5.0 needed... not yet (the delay took half a second)
    for (int i = 0; i < 90; ++i) {
        v.step(c, dt, 0.0f);
    }
    CHECK(!v.exhausted && v.value >= 5.0f);
    // One-off costs.
    Resource3D w;
    w.value = w.max = 10.0f;
    CHECK(!w.take(c, 4.0f));
    CHECK_NEAR(w.value, 6.0, 1e-4);
    CHECK(w.take(c, 20.0f) && w.value == 0.0f && w.exhausted);
    CHECK(!w.take(c, 1.0f));
}

void test_sprint_spends_resource() {
    World3D world(serial());
    world.create_body({.shape = Shape3D::box({100.0f, 0.5f, 100.0f}), .motion = Body3DMotion::Static, .position = {0, -0.5f, 0}, .friction = 0.6f});
    CharacterDesc3D d = climber();
    d.traversal.resource.max = 20.0f; // 1 s of sprint at 20 a second
    d.traversal.resource.regen_delay = 1.0f;
    d.traversal.resource.regen_rate = 10.0f;
    d.position = {0, 0, 0};
    const Character3D c = world.create_character(d);
    CharacterInput3D in;
    in.move = {0.0f, 1.0f};
    in.forward = {1.0f, 0.0f, 0.0f};
    in.sprint = true;
    world.set_character_input(c, in);
    std::vector<f32> speeds;
    int exhausted_at = -1;
    for (int i = 0; i < 100; ++i) {
        world.step(dt);
        speeds.push_back(world.character(c).ground_speed);
        if (exhausted_at < 0 && world.traversal(c).resource.exhausted) {
            exhausted_at = i;
        }
    }
    // Sprint speed (7) while it lasts, walk speed (4) when it has run out (and the delay before it comes back is running).
    CHECK_NEAR(speeds[30], 7.0, 0.1);
    CHECK(exhausted_at > 50 && exhausted_at < 70);
    CHECK_NEAR(speeds[95], 4.0, 0.1);
    int exhausted_events = 0;
    for (const CharacterEvent3D& e : world.drain_character_events()) {
        exhausted_events += e.kind == CharacterEventKind3D::Exhausted ? 1 : 0;
    }
    CHECK(exhausted_events == 1);
    // Resting: it comes back after the delay and sprinting is allowed again.
    in.sprint = false;
    in.move = {};
    world.set_character_input(c, in);
    for (int i = 0; i < 15; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.traversal(c).resource.value, 0.0, 1e-3); // inside the delay (a second since the last spend)
    for (int i = 0; i < 100; ++i) {
        world.step(dt);
    }
    CHECK(!world.traversal(c).resource.exhausted && world.traversal(c).resource.value > 10.0f);
    in.sprint = true;
    in.move = {0.0f, 1.0f};
    world.set_character_input(c, in);
    for (int i = 0; i < 30; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.character(c).ground_speed, 7.0, 0.1);
    // A character with traversal off sprints for ever, as it always did.
    CharacterDesc3D plain;
    plain.position = {0, 0, 10};
    const Character3D p = world.create_character(plain);
    world.set_character_input(p, in);
    for (int i = 0; i < 300; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.character(p).ground_speed, 7.0, 0.1);
    CHECK(!world.traversal(p).enabled);
}

void test_exhaustion_falls() {
    // 6 units after the two grabs (2 each) leave 2: exhausted at 2 / 7 s of hanging, the hands go one at a time.
    CharacterDesc3D d = climber();
    // In the air at the wall (standing on the floor with the hands on the wall is rest): it falls to the hang place.
    Rig r(serial(), d, {0.0f, 1.6f, 0.7f});
    r.world.set_traversal_resource(r.hero, 6.0f);
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.0f, -1.0f};
    int fell_at = -1, exhausted_at = -1, gripped_at = -1;
    std::vector<int> released_at;
    for (int i = 0; i < 200; ++i) {
        r.step();
        const std::size_t n = r.events.size();
        (void)n;
        for (const CharacterEvent3D& e : r.events) {
            (void)e;
        }
        if (gripped_at < 0 && r.count(CharacterEventKind3D::Gripped) == 2) gripped_at = i;
        if (exhausted_at < 0 && r.count(CharacterEventKind3D::Exhausted) == 1) exhausted_at = i;
        if (fell_at < 0 && r.count(CharacterEventKind3D::Fell) == 1) {
            fell_at = i;
            CHECK(r.trav().resource.exhausted && r.trav().held == 0);
        }
        if (static_cast<int>(released_at.size()) < r.count(CharacterEventKind3D::GripReleased)) released_at.push_back(i);
    }
    CHECK(gripped_at > 0);
    // Exhausted: (6 - 2 x 2) / 7 a second after the second grab (the first hand's grab was a step earlier or the same).
    const f64 expected_exhaust = (6.0 - 4.0) / 7.0 * 60.0;
    CHECK_NEAR(exhausted_at - gripped_at, expected_exhaust, 4.0);
    // The weakest hand (equal: hand 0) goes after the fade (0.3 s), the other 0.4 s after that.
    CHECK(released_at.size() == 2);
    if (released_at.size() == 2) {
        CHECK_NEAR(released_at[0] - exhausted_at, 0.3 * 60.0, 3.0);
        CHECK_NEAR(released_at[1] - released_at[0], 0.4 * 60.0, 3.0);
    }
    CHECK(fell_at == (released_at.size() == 2 ? released_at[1] : -2));
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    // The released hands carry the exhaustion reason.
    for (const CharacterEvent3D& e : r.events) {
        if (e.kind == CharacterEventKind3D::GripReleased || e.kind == CharacterEventKind3D::Fell) {
            CHECK(e.reason == static_cast<u8>(GripRelease3D::Exhausted));
        }
    }
    // Exhausted: a hand does not take hold again, though the command is given.
    CHECK(r.trav().held == 0);

    // The same with no stamina at all: it cannot take hold.
    Rig z(serial(), d, {0.0f, 1.6f, 0.7f});
    z.world.set_traversal_resource(z.hero, 0.0f);
    z.tin.grab = {true, true};
    z.tin.reach = {0.0f, 0.0f, -1.0f};
    z.step(40);
    CHECK(z.trav().held == 0 && z.count(CharacterEventKind3D::Gripped) == 0);
}

void test_low_stamina_climb_falls() {
    // The climb of the first test with 20 units: the grabs cost 4, the hang spends 7 a second (two hands) from then and 11 once it
    // climbs (the move's 4 on top, from step 30). It is exhausted when what is left has gone, lets go of the weakest hand 0.3 s
    // later and of the other 0.4 s after that, and falls: the expected steps are worked out here from the rates.
    CharacterDesc3D d = climber();
    d.traversal.resource.max = 20.0f;
    Rig r(serial(), d);
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.45f, -1.0f};
    int gripped_at = -1, exhausted_at = -1, first_release = -1, last_release = -1, fell = -1;
    for (int i = 0; i < 430; ++i) {
        if (i == 30) {
            r.input.move = {0.0f, 1.0f};
        }
        r.step();
        const int frame = r.frame - 1;
        if (gripped_at < 0 && r.count(CharacterEventKind3D::Gripped) == 2) gripped_at = frame;
        if (exhausted_at < 0 && r.count(CharacterEventKind3D::Exhausted) > 0) exhausted_at = frame;
        if (first_release < 0 && r.count(CharacterEventKind3D::GripReleased) > 0) first_release = frame;
        if (last_release < 0 && r.count(CharacterEventKind3D::GripReleased) == 2) last_release = frame;
        if (fell < 0 && r.count(CharacterEventKind3D::Fell) > 0) fell = frame;
    }
    CHECK(gripped_at > 0 && exhausted_at > gripped_at);
    // Worked out: from the step after the second grab, 7 a second until the climb starts at step 30, 11 a second after.
    f32 value = 20.0f - 4.0f;
    int expected = gripped_at;
    while (value > 0.0f && expected < 1000) {
        ++expected;
        value -= (expected < 30 ? 7.0f : 11.0f) / 60.0f;
    }
    CHECK_NEAR(exhausted_at, expected, 3.0);
    CHECK_NEAR((first_release - exhausted_at) / 60.0, 0.3, 0.07);
    CHECK_NEAR((last_release - first_release) / 60.0, 0.4, 0.07);
    CHECK(fell == last_release);
    // It was well short of the top, and never tried the pull-up.
    CHECK(r.state().position.y < 3.0f);
    CHECK(r.count(CharacterEventKind3D::MantleStarted) == 0);
}

void test_events_sorted() {
    // No fade and no interval: the exhaustion and both hands' release come in one step. The order is by kind, then hand
    // (the grip's own work happens in another order).
    CharacterDesc3D d = climber();
    d.traversal.exhaust_fade = 0.0f;
    d.traversal.exhaust_interval = 0.0f;
    Rig r(serial(), d, {0.0f, 1.6f, 0.7f});
    r.world.set_traversal_resource(r.hero, 4.2f);
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.0f, -1.0f};
    r.step(40);
    std::vector<CharacterEventKind3D> kinds;
    std::vector<int> hands;
    for (const CharacterEvent3D& e : r.events) {
        if (e.kind >= CharacterEventKind3D::Gripped) {
            kinds.push_back(e.kind);
            hands.push_back(e.hand);
        }
    }
    // Gripped 0, Gripped 1 (a step apart or the same), then in one step: GripReleased 0, GripReleased 1, Exhausted, Fell.
    const std::vector<CharacterEventKind3D> want = {CharacterEventKind3D::Gripped, CharacterEventKind3D::Gripped,
                                                    CharacterEventKind3D::GripReleased, CharacterEventKind3D::GripReleased,
                                                    CharacterEventKind3D::Exhausted, CharacterEventKind3D::Fell};
    CHECK(kinds == want);
    if (hands.size() == 6) {
        CHECK(hands[0] == 0 && hands[1] == 1 && hands[2] == 0 && hands[3] == 1);
    }
    // Delivered through the world's events with the character's handle and user id.
    for (const CharacterEvent3D& e : r.events) {
        CHECK(e.character == r.hero);
    }
}

void test_mantle_blocked_and_clear() {
    // Under a low roof: the pull-up is refused (an event, still hanging, nothing spent).
    Rig r(serial(), climber(), {60.0f, 0.0f, 0.7f});
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.45f, -1.0f};
    r.step(30);
    r.input.move = {0.0f, 1.0f};
    run_until(r, 400, [&] { return r.trav().grips[0].ledge && r.trav().grips[1].ledge; });
    r.input.move = {};
    r.step(20);
    CHECK(r.trav().held == 2 && r.trav().grips[0].ledge);
    const f32 before = r.trav().resource.value;
    r.tin.pull_up = true;
    r.step(2);
    CHECK(r.count(CharacterEventKind3D::MantleBlocked) == 1);
    CHECK(r.count(CharacterEventKind3D::MantleStarted) == 0);
    CHECK(r.trav().mode == TraversalMode3D::Hanging && r.trav().held == 2);
    for (const CharacterEvent3D& e : r.events) {
        if (e.kind == CharacterEventKind3D::MantleBlocked) {
            CHECK(e.reason == static_cast<u8>(MantleBlock3D::NoRoom));
        }
    }
    CHECK(r.trav().resource.value > before - 1.0f);
    // The press is an edge: held, it does not ask again each step.
    r.step(20);
    CHECK(r.count(CharacterEventKind3D::MantleBlocked) == 1);
    // Take the roof away: the same press now works.
    r.tin.pull_up = false;
    r.step(2);
    r.world.destroy_body(r.level.low_roof);
    r.tin.pull_up = true;
    r.step(2);
    CHECK(r.count(CharacterEventKind3D::MantleStarted) == 1);
    r.step(60);
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    CHECK_NEAR(r.state().position.y, 3.0, 0.03);
    CHECK_NEAR(r.state().position.z, -0.4, 0.03);

    // Not tired enough to climb either: below the cost, a pull-up is refused as Tired.
    Rig t;
    climb_to_top(t);
    t.world.set_traversal_resource(t.hero, 3.0f);
    t.tin.pull_up = true;
    t.step(2);
    int tired = 0;
    for (const CharacterEvent3D& e : t.events) {
        if (e.kind == CharacterEventKind3D::MantleBlocked && e.reason == static_cast<u8>(MantleBlock3D::Tired)) ++tired;
    }
    CHECK(tired == 1);
    CHECK(t.trav().mode == TraversalMode3D::Hanging);

    // No ledge: pulling up from the middle of a tall wall.
    Rig n;
    n.tin.grab = {true, true};
    n.tin.reach = {0.0f, 0.45f, -1.0f};
    n.step(30);
    n.tin.pull_up = true;
    n.step(2);
    int none = 0;
    for (const CharacterEvent3D& e : n.events) {
        if (e.kind == CharacterEventKind3D::MantleBlocked && e.reason == static_cast<u8>(MantleBlock3D::NoLedge)) ++none;
    }
    CHECK(none == 1);
}

void test_shimmy() {
    CharacterDesc3D big = climber();
    big.traversal.resource.max = 400.0f;
    Rig r(serial(), big);
    climb_to_top(r);
    const Vec3f p0 = r.trav().grips[0].point;
    r.input.move = {1.0f, 0.0f};
    r.step(60);
    const Vec3f p1 = r.trav().grips[0].point;
    CHECK_NEAR(p1.x - p0.x, 1.0, 0.08);
    CHECK_NEAR(p1.y, 4.0, 0.002); // along the lip
    CHECK(r.trav().held == 2);
    // Both ways, and the body follows the hands.
    r.input.move = {-1.0f, 0.0f};
    r.step(120);
    CHECK_NEAR(r.trav().grips[0].point.x - p0.x, -1.0, 0.1);
    r.input.move = {};
    r.step(60);
    // At rest the body is under the middle of the hands.
    CHECK_NEAR(r.state().position.x, 0.5 * (r.trav().grips[0].point.x + r.trav().grips[1].point.x), 0.03);
    // To the end of the wall (x = 3): the hands stop at its end, still holding.
    r.input.move = {1.0f, 0.0f};
    r.step(400);
    CHECK(r.trav().held == 2);
    CHECK(r.trav().grips[1].point.x < 3.001f && r.trav().grips[1].point.x > 2.8f);
    // Not moving across the wall's end: both hands stop together (the left one is not left behind).
    CHECK_NEAR(r.trav().grips[1].point.x - r.trav().grips[0].point.x, 0.4, 0.15);
}

void test_jump_off_and_drop() {
    Rig r;
    climb_to_top(r);
    r.tin.jump_off = true;
    r.step(1);
    CharacterState3D s = r.state();
    CHECK(r.trav().mode == TraversalMode3D::Walking);
    CHECK_NEAR(s.velocity.z, 3.0, 0.15); // away from the wall
    CHECK_NEAR(s.velocity.y, 4.0, 0.3);
    CHECK_NEAR(s.velocity.x, 0.0, 0.1);
    CHECK(r.count(CharacterEventKind3D::GripReleased) == 2);
    CHECK(r.count(CharacterEventKind3D::Fell) == 0); // a jump is not a fall
    for (const CharacterEvent3D& e : r.events) {
        if (e.kind == CharacterEventKind3D::GripReleased) CHECK(e.reason == static_cast<u8>(GripRelease3D::JumpOff));
    }
    // It flies: up, then down, landing on the floor out from the wall.
    f32 apex = 0.0f;
    for (int i = 0; i < 120; ++i) {
        r.step();
        apex = std::max(apex, r.state().position.y);
    }
    CHECK(apex > 2.3f + 0.6f);
    CHECK(r.state().ground == CharacterGround3D::OnGround);
    CHECK(r.state().position.z > 1.0f);
    // The speed of what it held goes with it: off the platform that moves along X at 1.5 m/s.
    Rig p(serial(), climber(), {27.0f, 0.0f, 1.5f});
    p.tin.grab = {true, true};
    p.tin.reach = {0.0f, 0.5f, -1.0f};
    p.step(40);
    CHECK(p.trav().held == 2);
    p.tin.jump_off = true;
    p.step(1);
    CHECK_NEAR(p.state().velocity.x, 1.5, 0.25);
    CHECK_NEAR(p.state().velocity.z, 3.0, 0.25);
}

void test_platform_carries_grip() {
    Rig r(serial(), climber(), {27.0f, 0.0f, 1.5f});
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.5f, -1.0f};
    r.step(70); // held (0.25 s), swung into the hang (a second)
    TraversalState3D t = r.trav();
    CHECK(t.held == 2);
    CHECK(t.grips[0].body == r.level.platform);
    CHECK(t.grips[0].kind == GripKind3D::Hold);
    const Vec3f local0 = t.grips[0].local_point;
    const f32 offset0 = r.state().position.x - r.world.position(r.level.platform).x;
    // Carried over its whole there-and-back (8 s): the grip is where it was ON the platform, the character with it.
    f32 worst_offset = 0.0f, worst_local = 0.0f, travelled = 0.0f;
    const f32 x_start = r.world.position(r.level.platform).x;
    for (int i = 0; i < 400; ++i) {
        r.step();
        t = r.trav();
        worst_offset = std::max(worst_offset, std::fabs(r.state().position.x - r.world.position(r.level.platform).x - offset0));
        worst_local = std::max(worst_local, length(t.grips[0].local_point - local0));
        travelled = std::max(travelled, r.world.position(r.level.platform).x - x_start);
    }
    CHECK(t.held == 2);
    CHECK(travelled > 2.0f);
    CHECK_NEAR(worst_local, 0.0, 1e-5);
    CHECK(worst_offset < 0.04f); // carried: the swing is what it does about the platform's acceleration, not a lag
    // The anchor in the world is on the platform's face.
    CHECK_NEAR(t.grips[0].point.z, r.world.position(r.level.platform).z + 0.3, 0.01);
    CHECK_NEAR(t.grips[0].point.x - r.world.position(r.level.platform).x, local0.x, 0.01);
}

void test_crate_load() {
    // Hanging from the face of a crate that is a third over a pedestal's edge: its weight at the anchor tips it. Without the
    // character the crate stays where it is.
    const auto run = [](bool grab, Vec3f& crate_at, Quatf& crate_rot, Vec3f& anchor) {
        Rig r(serial(), climber(), {40.0f, 0.0f, 1.7f});
        r.tin.grab = {grab, grab};
        r.tin.reach = {0.0f, 0.8f, -0.6f};
        r.step(150);
        crate_at = r.world.position(r.level.crate);
        crate_rot = r.world.rotation(r.level.crate);
        anchor = r.trav().grips[0].point;
        return r.trav().held;
    };
    Vec3f rest_at, rest_anchor, load_at, load_anchor;
    Quatf rest_rot, load_rot;
    run(false, rest_at, rest_rot, rest_anchor);
    CHECK_NEAR(rest_at.y, 2.4, 0.01);
    CHECK_NEAR(rest_at.z, 0.9, 0.01);
    const u32 held = run(true, load_at, load_rot, load_anchor);
    CHECK(held >= 1);
    // The crate has gone over: lower by a metre or more after two and a half seconds.
    CHECK(rest_at.y - load_at.y > 1.0f);
    (void)load_rot;
    // The anchor went with it (stored in the crate's own space), and the character with the anchor.
    Rig r(serial(), climber(), {40.0f, 0.0f, 1.7f});
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.8f, -0.6f};
    r.step(25);
    const Vec3f anchor0 = r.trav().grips[0].point;
    const Vec3f local0 = r.trav().grips[0].local_point;
    r.step(25);
    // Half a second on: the crate has gone over its edge, turned by tens of degrees.
    {
        const Quatf q = r.world.rotation(r.level.crate);
        CHECK(2.0f * std::acos(std::clamp(std::fabs(q.w), 0.0f, 1.0f)) * 57.29578f > 20.0f);
    }
    r.step(60);
    const TraversalState3D t = r.trav();
    CHECK(length(t.grips[0].point - anchor0) > 0.1f);
    CHECK_NEAR(length(t.grips[0].local_point - local0), 0.0, 1e-5);
    const Vec3f world_anchor = r.world.position(r.level.crate) + r.world.rotation(r.level.crate).rotate(local0);
    CHECK_NEAR(length(t.grips[0].point - world_anchor), 0.0, 1e-3);
    CHECK(t.mode != TraversalMode3D::Walking);
    // A heavy crate (pinned: static pedestal) is just a hold: a 10 t crate does not move.
    Rig h(serial(), climber(), {40.0f, 0.0f, 1.7f});
    h.world.destroy_body(h.level.crate);
    const Body3D heavy = h.world.create_body({.shape = Shape3D::box({0.4f, 0.4f, 0.4f}), .position = {40.0f, 2.4f, 0.9f},
                                              .mass = 10000.0f, .friction = 0.5f, .user_id = 999});
    h.tin.grab = {true, true};
    h.tin.reach = {0.0f, 0.8f, -0.6f};
    h.step(150);
    CHECK_NEAR(h.world.position(heavy).y, 2.4, 0.02);
    CHECK(h.trav().held == 2);
}

void test_ice_slips() {
    Rig r(serial(), climber(), {50.0f, 2.5f, 0.7f});
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.3f, -1.0f};
    int slipped_at = -1, released_at = -1, held_at = -1;
    f32 y_first = 0.0f, y_last = 0.0f, lowest = 100.0f;
    for (int i = 0; i < 400; ++i) {
        r.step();
        const TraversalState3D t = r.trav();
        if (held_at < 0 && t.held == 2) {
            held_at = i;
            y_first = t.grips[0].point.y;
        }
        if (t.held == 2) {
            y_last = t.grips[0].point.y;
            lowest = std::min(lowest, t.resource.value);
        }
        if (slipped_at < 0 && r.count(CharacterEventKind3D::Slipped) > 0) slipped_at = i;
        if (released_at < 0 && r.count(CharacterEventKind3D::GripReleased) > 0) released_at = i;
    }
    CHECK(held_at > 0 && slipped_at >= held_at && released_at > slipped_at);
    // It slides at 1.2 m/s for 1.5 s, then lets go: 1.8 m down, and falls.
    CHECK_NEAR((released_at - slipped_at) * dt, 1.5, 0.15);
    CHECK_NEAR(y_first - y_last, 1.8, 0.2);
    CHECK(r.count(CharacterEventKind3D::Fell) == 1);
    for (const CharacterEvent3D& e : r.events) {
        if (e.kind == CharacterEventKind3D::GripReleased || e.kind == CharacterEventKind3D::Fell) {
            CHECK(e.reason == static_cast<u8>(GripRelease3D::Slipped));
        }
        if (e.kind == CharacterEventKind3D::Slipped) {
            CHECK_NEAR(e.speed, 1.2, 1e-4);
        }
    }
    // Ice is tiring: it spent twice the two hands' rate while it slid (14 a second for 1.5 s, and the two grabs).
    CHECK_NEAR(100.0f - lowest, 4.0 + 14.0 * 1.5, 2.5);
    // The surface it slid on told its tag.
    bool tagged = false;
    for (const CharacterEvent3D& e : r.events) {
        tagged = tagged || (e.kind == CharacterEventKind3D::Gripped && e.body == r.level.ice);
    }
    CHECK(tagged);
}

void test_lost_body_and_handles() {
    // The body it holds is destroyed: the grips are lost, it falls.
    Rig r(serial(), climber(), {40.0f, 0.0f, 1.7f});
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, 0.8f, -0.6f};
    r.step(25);
    CHECK(r.trav().held >= 1);
    r.world.destroy_body(r.level.crate);
    r.step(3);
    CHECK(r.trav().held == 0 && r.trav().mode == TraversalMode3D::Walking);
    CHECK(r.count(CharacterEventKind3D::Fell) == 1);
    bool lost = false;
    for (const CharacterEvent3D& e : r.events) {
        lost = lost || (e.kind == CharacterEventKind3D::GripReleased && e.reason == static_cast<u8>(GripRelease3D::Lost));
    }
    CHECK(lost);
    // A descriptor with a nonsense number is refused (no character), and so is a live change to one.
    {
        World3D w(serial());
        CharacterDesc3D bad = climber();
        bad.traversal.reach = std::nanf("");
        CHECK(!w.create_character(bad).valid());
        bad = climber();
        bad.traversal.limb_count = 5;
        CHECK(!w.create_character(bad).valid());
        bad = climber();
        bad.traversal.resource.max = 0.0f;
        CHECK(!w.create_character(bad).valid());
        const Character3D ok = w.create_character(climber());
        CHECK(ok.valid());
        CharacterDesc3D change = climber();
        change.traversal.reach = 1.3f;
        w.set_character_desc(ok, change);
        change.traversal.reach = std::nanf("");
        w.set_character_desc(ok, change); // the last good one stays
        // Whether it is on is fixed at creation: a live change to off is not taken.
        CharacterDesc3D off = climber();
        off.traversal.enabled = false;
        w.set_character_desc(ok, off);
        CHECK(w.traversal(ok).enabled);
    }
    // A character without traversal has none of it, and the calls do nothing.
    World3D world(serial());
    CharacterDesc3D plain;
    const Character3D c = world.create_character(plain);
    TraversalInput3D in;
    in.grab = {true, true};
    world.set_traversal_input(c, in);
    CHECK(!world.traversal(c).enabled);
    CHECK(!world.traversal_input(c).grab[0]);
    world.set_traversal_resource(c, 3.0f);
    CHECK(world.traversal(c).resource.max == 0.0f);
    // A handle that is not a character.
    CHECK(!world.traversal(Character3D{}).enabled);
    // Surface and hold data on a body that is not one: refused quietly.
    world.set_surface_material({}, {.grip = 0.2f});
    CHECK(!world.set_hold({}, {}));
    // The material and the hold read back.
    const Body3D b = world.create_body({.shape = Shape3D::box({1, 1, 1}), .motion = Body3DMotion::Static});
    world.set_surface_material(b, {.grippable = false, .grip = 0.5f, .slip = 0.25f, .drain = 3.0f, .tag = 9});
    const SurfaceMaterial3D m = world.surface_material(b);
    CHECK(!m.grippable && m.grip == 0.5f && m.slip == 0.25f && m.drain == 3.0f && m.tag == 9);
    CHECK(!world.is_hold(b));
    CHECK(world.set_hold(b, {.quality = 0.4f}));
    CHECK(world.is_hold(b) && world.hold(b).quality == 0.4f);
    world.clear_hold(b);
    CHECK(!world.is_hold(b));
    // A slot reused by another body has the default material.
    world.destroy_body(b);
    const Body3D b2 = world.create_body({.shape = Shape3D::box({1, 1, 1}), .motion = Body3DMotion::Static});
    CHECK(world.surface_material(b2).grippable && world.surface_material(b2).tag == 0);
}

void test_authored_hold_snaps() {
    World3D world(serial());
    const Body3D wall = world.create_body({.shape = Shape3D::box({3, 3, 0.5f}), .motion = Body3DMotion::Static, .position = {0, 3, -0.5f}});
    world.set_surface_material(wall, {.grippable = false});
    // A rung: a bar standing out of the wall; the hand goes to its middle, wherever it was met.
    const Body3D rung = world.create_body({.shape = Shape3D::box({0.5f, 0.04f, 0.04f}), .motion = Body3DMotion::Static,
                                           .position = {0.0f, 2.0f, 0.1f}});
    HoldDesc3D hd;
    hd.quality = 0.8f;
    hd.snap = true;
    hd.anchor = {0.0f, 0.0f, 0.0f};
    hd.anchor_normal = {0.0f, 0.0f, 1.0f};
    CHECK(world.set_hold(rung, hd));
    CharacterDesc3D d = climber();
    d.position = {0.3f, 0.0f, 0.55f};
    const Character3D c = world.create_character(d);
    world.set_character_input(c, {.forward = {0, 0, -1}});
    TraversalInput3D in;
    in.grab = {false, true};
    in.reach = {0.0f, 0.8f, -0.6f};
    world.set_traversal_input(c, in);
    for (int i = 0; i < 40; ++i) {
        world.step(dt);
    }
    const TraversalState3D t = world.traversal(c);
    CHECK(t.grips[1].phase == GripPhase3D::Held);
    CHECK(t.grips[1].kind == GripKind3D::Hold && t.grips[1].fixed);
    CHECK_NEAR(t.grips[1].point.x, 0.0, 1e-4); // the rung's middle, not where the hand was aimed (x = 0.2)
    CHECK_NEAR(t.grips[1].point.y, 2.0, 1e-4);
    CHECK_NEAR(t.grips[1].strength, 0.8, 1e-4);
    // A fixed grip does not shimmy.
    world.set_character_input(c, {.move = {1.0f, 0.0f}, .forward = {0, 0, -1}});
    for (int i = 0; i < 30; ++i) {
        world.step(dt);
    }
    CHECK_NEAR(world.traversal(c).grips[1].point.x, 0.0, 1e-4);
    // Both hands for the one point: the second is not taken (a hand is never put within 0.2 m of the other's anchor), and the
    // character holds by one hand; with the other hand free to take another hold.
    in.grab = {true, true};
    world.set_traversal_input(c, in);
    for (int i = 0; i < 40; ++i) {
        world.step(dt);
    }
    const TraversalState3D both = world.traversal(c);
    CHECK(both.held == 1);
    CHECK(both.grips[0].phase == GripPhase3D::Free);
    const Body3D second = world.create_body({.shape = Shape3D::box({0.1f, 0.04f, 0.04f}), .motion = Body3DMotion::Static,
                                             .position = {-0.4f, 2.0f, 0.1f}});
    HoldDesc3D other = hd;
    other.anchor = {0.0f, 0.0f, 0.0f};
    CHECK(world.set_hold(second, other));
    for (int i = 0; i < 40; ++i) {
        world.step(dt);
    }
    const TraversalState3D two = world.traversal(c);
    CHECK(two.held == 2);
    CHECK(two.grips[0].body == second || two.grips[1].body == second);
    CHECK(length(two.grips[0].point - two.grips[1].point) >= 0.2f);
}

void test_three_limbs() {
    // A third limb is data: three hands hold, and share the load like two.
    CharacterDesc3D d = climber();
    d.traversal.limb_count = 3;
    d.traversal.limbs[2].shoulder = {0.0f, 1.2f, 0.0f};
    Rig r(serial(), d, {0.0f, 1.6f, 0.7f});
    r.tin.grab = {true, true, true, false};
    r.tin.reach = {0.0f, 0.0f, -1.0f};
    r.step(60);
    const TraversalState3D t = r.trav();
    CHECK(t.limb_count == 3);
    CHECK(t.held == 3);
    CHECK(t.grips[2].phase == GripPhase3D::Held);
    // Three points apart on the wall.
    CHECK(length(t.grips[0].point - t.grips[2].point) >= 0.2f);
    CHECK(length(t.grips[1].point - t.grips[2].point) >= 0.2f);
    r.step(70);
    const f32 a = r.trav().resource.value;
    r.step(60);
    CHECK_NEAR(a - r.trav().resource.value, 7.0, 0.3); // two or more share: the two-hand rate
    // A fourth command on a character with three limbs is ignored.
    r.tin.grab = {true, true, true, true};
    r.step(30);
    CHECK(r.trav().held == 3);
}

void test_standing_on_the_ground_is_free() {
    // Holding the wall with the feet on the floor does not tire (the default ground factor is 0).
    Rig r;
    r.tin.grab = {true, true};
    r.tin.reach = {0.0f, -0.1f, -1.0f};
    r.step(200);
    const TraversalState3D t = r.trav();
    CHECK(t.held == 2);
    CHECK(r.state().ground == CharacterGround3D::OnGround);
    CHECK_NEAR(t.resource.value, 100.0, 0.5); // the two grabs came back: rest
    // With a factor of 1 it does.
    CharacterDesc3D d = climber();
    d.traversal.ground_drain_factor = 1.0f;
    Rig w(serial(), d);
    w.tin.grab = {true, true};
    w.tin.reach = {0.0f, -0.1f, -1.0f};
    w.step(200);
    CHECK(w.trav().resource.value < 100.0f - 4.0f - 10.0f);
}

void test_walking_is_unchanged() {
    // Traversal on and unused is the walking controller: the same feet, bit for bit, as with it off (every step).
    const auto route = [](bool traversal, std::vector<Vec3f>& feet, std::vector<f32>& speed) {
        World3D world(serial());
        climb_level::build(world);
        CharacterDesc3D d;
        d.traversal.enabled = traversal;
        d.position = {2.0f, 0.0f, 3.0f};
        const Character3D c = world.create_character(d);
        for (int i = 0; i < 400; ++i) {
            CharacterInput3D in;
            in.forward = {1.0f, 0.0f, 0.0f};
            in.move = {i % 90 < 60 ? 0.3f : 0.0f, i % 150 < 100 ? 1.0f : 0.0f};
            in.sprint = i % 200 > 100;
            in.jump = i % 70 == 10;
            in.crouch = i > 250 && i < 300;
            world.set_character_input(c, in);
            world.step(dt);
            feet.push_back(world.character(c).position);
            speed.push_back(world.character(c).ground_speed);
        }
    };
    std::vector<Vec3f> a, b;
    std::vector<f32> sa, sb;
    route(false, a, sa);
    route(true, b, sb);
    CHECK(a == b);
    CHECK(sa == sb);
    // Off, a descriptor's traversal block changes nothing of the saved state's size: no bytes of it (the hash of a world
    // with a character that has none is what it was).
    World3D w1(serial()), w2(serial());
    CharacterDesc3D d1;
    d1.position = {0, 5, 0};
    CharacterDesc3D d2 = d1;
    d2.traversal.reach = 1.0f; // data of a limb system that is off
    w1.create_character(d1);
    w2.create_character(d2);
    for (int i = 0; i < 10; ++i) {
        w1.step(dt);
        w2.step(dt);
    }
    CHECK(w1.state_hash() == w2.state_hash());
}

// --- redirected gravity ------------------------------------------------------------------------------------------------

void test_redirected_gravity() {
    // The whole level turned so that its wall is a floor (up is along -X): the same climb, in the turned frame.
    const auto climb = [](Quatf frame, Vec3f& final_feet, bool& mantled, Vec3f& final_up) {
        World3D world(serial());
        climb_level::Level level = climb_level::build(world, frame);
        world.set_gravity(frame.rotate({0.0f, -9.81f, 0.0f}));
        CharacterDesc3D d = climber();
        d.position = level.at({0.0f, 0.0f, 0.7f});
        const Character3D c = world.create_character(d);
        const Vec3f forward = frame.rotate({0.0f, 0.0f, -1.0f});
        TraversalInput3D tin;
        tin.grab = {true, true};
        tin.reach = frame.rotate(normalize(Vec3f{0.0f, 0.45f, -1.0f}));
        CharacterInput3D in;
        in.forward = forward;
        int frames = 0;
        const auto step = [&](int n) {
            for (int i = 0; i < n; ++i) {
                world.set_character_input(c, in);
                world.set_traversal_input(c, tin);
                world.step(dt);
                ++frames;
            }
        };
        step(30);
        in.move = {0.0f, 1.0f};
        for (int i = 0; i < 400; ++i) {
            const TraversalState3D t = world.traversal(c);
            if (t.held == 2 && t.grips[0].ledge && t.grips[1].ledge) {
                break;
            }
            step(1);
        }
        in.move = {};
        step(30);
        tin.pull_up = true;
        step(70);
        mantled = world.traversal(c).mode == TraversalMode3D::Walking && world.traversal(c).held == 0;
        step(30);
        final_feet = world.character(c).position;
        final_up = world.character(c).up;
    };
    Vec3f upright{}, turned{}, up_a{}, up_b{};
    bool ok_a = false, ok_b = false;
    climb({}, upright, ok_a, up_a);
    CHECK(ok_a);
    CHECK_NEAR(upright.y, 4.0, 0.03);
    const Quatf quarter = Quatf::axis_angle({0.0f, 0.0f, 1.0f}, 90.0f);
    climb(quarter, turned, ok_b, up_b);
    CHECK(ok_b);
    // Back in the level's own frame it is where the upright climb ended.
    const Vec3f back = quarter.conjugate().rotate(turned);
    CHECK_NEAR(back.x, upright.x, 0.06);
    CHECK_NEAR(back.y, upright.y, 0.06);
    CHECK_NEAR(back.z, upright.z, 0.06);
    // Its up is the turned one.
    CHECK_NEAR(dot(up_b, quarter.rotate({0.0f, 1.0f, 0.0f})), 1.0, 1e-3);
    // Another frame, tilted 35 degrees about X and 20 about Y.
    const Quatf tilt = Quatf::axis_angle({0.0f, 1.0f, 0.0f}, 20.0f) * Quatf::axis_angle({1.0f, 0.0f, 0.0f}, 35.0f);
    Vec3f tilted{}, up_c{};
    bool ok_c = false;
    climb(tilt, tilted, ok_c, up_c);
    CHECK(ok_c);
    const Vec3f back_c = tilt.conjugate().rotate(tilted);
    CHECK_NEAR(length(back_c - upright), 0.0, 0.08);
}

// --- determinism, saved states -------------------------------------------------------------------------------------------

// A scene with two climbers, a platform, a crate and a slippery wall, on commands that are a function of the step.
struct Scene {
    World3D world;
    climb_level::Level level;
    Character3D wall_climber, crate_climber, ice_climber;
    int frame = 0;

    explicit Scene(const World3DConfig& cfg)
        : world(cfg) {
        level = climb_level::build(world);
        const auto make = [&](Vec3f feet, u64 user) {
            CharacterDesc3D d = climber();
            d.position = feet;
            d.user_id = user;
            return world.create_character(d);
        };
        wall_climber = make({0.0f, 0.0f, 0.7f}, 1);
        crate_climber = make({40.0f, 0.0f, 1.7f}, 2);
        ice_climber = make({50.0f, 2.5f, 0.7f}, 3);
    }

    void command() {
        const int f = frame;
        CharacterInput3D a;
        a.forward = {0.0f, 0.0f, -1.0f};
        TraversalInput3D ta;
        ta.grab = {true, true};
        ta.reach = {0.0f, 0.45f, -1.0f};
        a.move = {f > 30 && f < 300 ? 0.1f : 0.0f, f > 30 && f < 290 ? 1.0f : 0.0f};
        ta.pull_up = f > 320 && f < 330;
        ta.jump_off = f == 520;
        world.set_character_input(wall_climber, a);
        world.set_traversal_input(wall_climber, ta);

        CharacterInput3D b;
        b.forward = {0.0f, 0.0f, -1.0f};
        TraversalInput3D tb;
        tb.grab = {true, f < 200};
        tb.reach = {0.0f, 0.2f, -1.0f};
        tb.drop = f > 260 && f < 270;
        world.set_character_input(crate_climber, b);
        world.set_traversal_input(crate_climber, tb);

        CharacterInput3D c;
        c.forward = {0.0f, 0.0f, -1.0f};
        TraversalInput3D tc;
        tc.grab = {true, true};
        tc.reach = {0.0f, 0.3f, -1.0f};
        world.set_character_input(ice_climber, c);
        world.set_traversal_input(ice_climber, tc);
    }

    void step() {
        command();
        level.drive(world, frame, dt);
        world.step(dt);
        ++frame;
    }
};

std::vector<u64> hash_run(const World3DConfig& cfg, int frames) {
    Scene s(cfg);
    std::vector<u64> hashes;
    for (int i = 0; i < frames; ++i) {
        s.step();
        hashes.push_back(s.world.state_hash());
    }
    return hashes;
}

void test_determinism() {
    const int frames = 600;
    const std::vector<u64> a = hash_run(serial(), frames);
    const std::vector<u64> b = hash_run(serial(), frames);
    CHECK(a == b);
    // The state moves (it is not a constant), and the climbers did things.
    CHECK(a.front() != a.back());
    {
        Scene s(serial());
        for (int i = 0; i < frames; ++i) {
            s.step();
        }
        const auto events = s.world.drain_character_events();
        int gripped = 0, mantled = 0, slipped = 0, fell = 0;
        for (const CharacterEvent3D& e : events) {
            gripped += e.kind == CharacterEventKind3D::Gripped;
            mantled += e.kind == CharacterEventKind3D::MantleFinished;
            slipped += e.kind == CharacterEventKind3D::Slipped;
            fell += e.kind == CharacterEventKind3D::Fell;
        }
        CHECK(gripped >= 5);
        CHECK(mantled == 1);
        CHECK(slipped >= 1);
        CHECK(fell >= 2);
    }
    // Worker counts: none (the calling thread), 1 and 4.
    for (const int workers : {1, 4}) {
        JobSystem jobs({.workers = workers, .max_background = workers});
        World3DConfig cfg;
        cfg.jobs = &jobs;
        cfg.single_threaded = false;
        CHECK(hash_run(cfg, frames) == a);
    }
}

void test_save_restore_mid_hang() {
    // Saved while hanging by two hands from the wall (and again, while mantling): a restored world takes the grips back
    // and repeats the exact trajectory.
    Scene s(serial());
    for (int i = 0; i < 150; ++i) {
        s.step();
    }
    const TraversalState3D at_save = s.world.traversal(s.wall_climber);
    CHECK(at_save.mode == TraversalMode3D::Hanging && at_save.held == 2);
    const std::vector<u8> saved = s.world.save_state();
    const u64 hash_at_save = s.world.state_hash();
    const int frame_at_save = s.frame;
    std::vector<u64> first;
    for (int i = 0; i < 400; ++i) {
        s.step();
        first.push_back(s.world.state_hash());
    }
    // Later the climber has mantled (or fallen); the state is another one.
    CHECK(s.world.traversal(s.wall_climber).held == 0);
    CHECK(s.world.restore_state(saved));
    CHECK(s.world.state_hash() == hash_at_save);
    const TraversalState3D restored = s.world.traversal(s.wall_climber);
    CHECK(restored.mode == TraversalMode3D::Hanging && restored.held == 2);
    CHECK(restored.grips[0].local_point == at_save.grips[0].local_point);
    CHECK(restored.grips[1].local_point == at_save.grips[1].local_point);
    CHECK(restored.grips[0].body == at_save.grips[0].body);
    CHECK(restored.resource.value == at_save.resource.value);
    s.frame = frame_at_save;
    std::vector<u64> second;
    for (int i = 0; i < 400; ++i) {
        s.step();
        second.push_back(s.world.state_hash());
    }
    CHECK(first == second);

    // Mid-mantle: the same.
    Scene m(serial());
    for (int i = 0; i < 345; ++i) {
        m.step();
    }
    CHECK(m.world.traversal(m.wall_climber).mode == TraversalMode3D::Mantling);
    const std::vector<u8> mid = m.world.save_state();
    const int mid_frame = m.frame;
    std::vector<u64> f1, f2;
    for (int i = 0; i < 120; ++i) {
        m.step();
        f1.push_back(m.world.state_hash());
    }
    CHECK(m.world.restore_state(mid));
    CHECK(m.world.traversal(m.wall_climber).mode == TraversalMode3D::Mantling);
    m.frame = mid_frame;
    for (int i = 0; i < 120; ++i) {
        m.step();
        f2.push_back(m.world.state_hash());
    }
    CHECK(f1 == f2);

    // Another world made the same way takes the state and repeats it.
    Scene other(serial());
    for (int i = 0; i < 20; ++i) {
        other.step();
    }
    CHECK(other.world.restore_state(saved));
    CHECK(other.world.state_hash() == hash_at_save);
    other.frame = frame_at_save;
    std::vector<u64> third;
    for (int i = 0; i < 400; ++i) {
        other.step();
        third.push_back(other.world.state_hash());
    }
    CHECK(third == first);
}

void test_hash_sees_traversal() {
    // The bodies' places are the same; the hash sees the commands, the resource and the grips that the bodies do not carry.
    Scene a(serial()), b(serial());
    CHECK(a.world.state_hash() == b.world.state_hash());
    {
        Scene x(serial()), y(serial());
        for (int i = 0; i < 40; ++i) {
            x.step();
            y.step();
        }
        CHECK(x.world.state_hash() == y.world.state_hash());
        const Vec3f feet = x.world.character(x.wall_climber).position;
        CHECK(x.world.traversal(x.wall_climber).mode == TraversalMode3D::Hanging);
        // y's climber is let go of without moving (a teleport to the same place clears the grips): same bodies, other state.
        y.world.teleport_character(y.wall_climber, feet);
        CHECK(y.world.traversal(y.wall_climber).mode == TraversalMode3D::Walking);
        CHECK(y.world.state_hash() != x.world.state_hash());
    }
    TraversalInput3D in;
    in.grab = {true, false};
    b.world.set_traversal_input(b.wall_climber, in);
    CHECK(a.world.state_hash() != b.world.state_hash());
    a.world.set_traversal_input(a.wall_climber, in);
    CHECK(a.world.state_hash() == b.world.state_hash());
    b.world.set_traversal_resource(b.wall_climber, 50.0f);
    CHECK(a.world.state_hash() != b.world.state_hash());
    a.world.set_traversal_resource(a.wall_climber, 50.0f);
    CHECK(a.world.state_hash() == b.world.state_hash());
}

void dump(Rig& r, const char* tag) {
    const TraversalState3D t = r.trav();
    const CharacterState3D s = r.state();
    std::printf("%s f%d mode %d held %u ground %d pos (%.3f %.3f %.3f) res %.2f%s anchors", tag, r.frame, int(t.mode), t.held, int(s.ground), s.position.x, s.position.y, s.position.z, t.resource.value, t.resource.exhausted ? " EXH" : "");
    for (u32 h = 0; h < 2; ++h) std::printf(" [%d k%d (%.2f %.2f %.2f) s%.2f]", int(t.grips[h].phase), int(t.grips[h].kind), t.grips[h].point.x, t.grips[h].point.y, t.grips[h].point.z, t.grips[h].strength);
    std::printf("\n");
}
void events(Rig& r) {
    for (const auto& e : r.events) std::printf("event kind %d hand %d reason %d\n", int(e.kind), e.hand, e.reason);
}



void test_debug_draw_grips() {
    Rig r;
    DebugDraw3D lines;
    // Walking: nothing of grips (a character with traversal off or at rest draws none).
    lines.grips(r.world, r.hero);
    CHECK(lines.lines().empty());
    grab_wall(r);
    r.step(2);
    // Reaching: a cross, a normal, the frame and the line to the body, per hand, in the reaching colour.
    lines.grips(r.world, r.hero);
    CHECK(!lines.lines().empty());
    const Color reaching = lines.options().grip_reaching;
    CHECK(std::any_of(lines.lines().begin(), lines.lines().end(), [&](const DebugLine3D& l) { return l.color == reaching; }));
    r.step(30);
    lines.clear();
    lines.grips(r.world, r.hero);
    const Color held = lines.options().grip_held;
    const std::size_t held_lines = static_cast<std::size_t>(std::count_if(
        lines.lines().begin(), lines.lines().end(), [&](const DebugLine3D& l) { return l.color == held; }));
    CHECK(held_lines >= 2 * 4); // two hands: three cross lines, a normal and a line to the body each
    CHECK(std::any_of(lines.lines().begin(), lines.lines().end(),
                      [&](const DebugLine3D& l) { return l.color == lines.options().hang_place; }));
    // Candidates of a probe: crosses and normals by provider.
    GripProbeDesc3D p;
    p.origin = {0.0f, 3.7f, 0.7f};
    p.direction = {0.0f, 0.0f, -1.0f};
    lines.clear();
    const std::vector<GripCandidate3D> found = r.world.probe_grips(p);
    lines.candidates(found);
    CHECK(!lines.lines().empty());
    CHECK(std::any_of(lines.lines().begin(), lines.lines().end(),
                      [&](const DebugLine3D& l) { return l.color == lines.options().candidate_ledge; }));
}

// --- the scripted route -------------------------------------------------------------------------------------------------

std::string read_file(const std::string& path) {
    std::string text;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
            text.append(buf, n);
        }
        std::fclose(f);
    }
    return text;
}

struct RouteRun {
    World3D world;
    climb_level::Level level;
    climb_level::Route route;
    Character3D hero;
    climb_level::Applied applied;
    int frame = 0;
    std::vector<CharacterEvent3D> events;

    RouteRun(const World3DConfig& cfg, const climb_level::Route& r)
        : world(cfg)
        , route(r) {
        level = climb_level::build(world);
        CharacterDesc3D d = climb_level::climber_desc();
        d.position = route.start;
        hero = world.create_character(d);
        applied.input.forward = {0.0f, 0.0f, -1.0f};
    }

    void step() {
        applied = climb_level::apply_keys(applied, route, frame);
        if (applied.teleport) {
            world.teleport_character(hero, applied.teleport_to);
        }
        if (applied.stamina) {
            world.set_traversal_resource(hero, applied.stamina_value);
        }
        world.set_character_input(hero, applied.input);
        world.set_traversal_input(hero, applied.traversal);
        level.drive(world, frame, dt);
        world.step(dt);
        for (const CharacterEvent3D& e : world.drain_character_events()) {
            events.push_back(e);
        }
        ++frame;
    }
};

const char* kind_name(CharacterEventKind3D k) {
    static const char* names[] = {"Landed", "LeftGround", "HitHead", "ContactBegin", "ContactEnd", "Gripped", "Slipped",
                                  "GripReleased", "Exhausted", "MantleStarted", "MantleFinished", "MantleBlocked", "Fell"};
    return names[static_cast<int>(k)];
}

void dump_route(const std::string& path, int every) {
    const JsonParseResult parsed = parse_json(read_file(path));
    if (!parsed.ok()) {
        std::printf("no route: %s\n", parsed.error.c_str());
        return;
    }
    const climb_level::Route route = climb_level::load_route(*parsed.value->find("character"));
    RouteRun run(serial(), route);
    std::size_t shown = 0;
    for (int n = 0; n < route.frames; ++n) {
        run.step();
        for (; shown < run.events.size(); ++shown) {
            const CharacterEvent3D& e = run.events[shown];
            if (e.kind >= CharacterEventKind3D::Gripped || e.kind == CharacterEventKind3D::Landed) {
                std::printf("  %4d  %-14s hand %d reason %d body %llu at (%.2f %.2f %.2f)\n", n, kind_name(e.kind), e.hand, e.reason,
                            static_cast<unsigned long long>(e.body_user_id), e.point.x, e.point.y, e.point.z);
            }
        }
        if (n % every == 0) {
            const CharacterState3D s = run.world.character(run.hero);
            const TraversalState3D t = run.world.traversal(run.hero);
            std::printf("%4d  pos (%7.3f %6.3f %7.3f) place (%.2f %.2f %.2f) mode %d held %u res %5.1f ground %d  anchors", n, s.position.x, s.position.y, s.position.z,
                        t.hang_place.x, t.hang_place.y, t.hang_place.z, int(t.mode), t.held, t.resource.value, int(s.ground));
            for (u32 h = 0; h < 2; ++h) {
                if (t.grips[h].phase != GripPhase3D::Free) {
                    std::printf(" [%d:%d (%.2f %.2f %.2f)]", int(h), int(t.grips[h].phase), t.grips[h].point.x, t.grips[h].point.y, t.grips[h].point.z);
                }
            }
            std::printf("\n");
        }
    }
}

// The scene file's route and its checkpoints, run with no GPU: the picture shows the same.
void test_scripted_route() {
#ifdef KIN_TEST_REFERENCE_SCENES
    const std::string path = std::string(KIN_TEST_REFERENCE_SCENES) + "/physics_climb.json";
    const JsonParseResult parsed = parse_json(read_file(path));
    CHECK(parsed.ok());
    if (!parsed.ok()) {
        return;
    }
    const JsonValue* character = parsed.value->find("character");
    CHECK(character != nullptr);
    if (character == nullptr) {
        return;
    }
    const climb_level::Route route = climb_level::load_route(*character);
    CHECK(route.frames > 1000 && !route.checkpoints.empty());
    RouteRun run(serial(), route);
    for (int n = 0; n < route.frames; ++n) {
        run.step();
        for (const climb_level::Checkpoint& cp : route.checkpoints) {
            if (cp.frame != n) {
                continue;
            }
            const CharacterState3D s = run.world.character(run.hero);
            const TraversalState3D t = run.world.traversal(run.hero);
            const bool inside = s.position.x >= cp.lo.x && s.position.x <= cp.hi.x && s.position.y >= cp.lo.y &&
                                s.position.y <= cp.hi.y && s.position.z >= cp.lo.z && s.position.z <= cp.hi.z;
            if (!inside) {
                ++failures;
                std::fprintf(stderr, "physics3d_traversal_tests: route checkpoint \"%s\" at frame %d: feet (%.3f %.3f %.3f) outside "
                             "[%.3f %.3f %.3f] to [%.3f %.3f %.3f]\n", cp.what.c_str(), n, s.position.x, s.position.y, s.position.z,
                             cp.lo.x, cp.lo.y, cp.lo.z, cp.hi.x, cp.hi.y, cp.hi.z);
            }
            if (!cp.mode.empty()) {
                const char* mode = t.mode == TraversalMode3D::Walking ? "walking" : t.mode == TraversalMode3D::Hanging ? "hanging" : "mantling";
                if (cp.mode != mode) {
                    ++failures;
                    std::fprintf(stderr, "physics3d_traversal_tests: route checkpoint \"%s\" at frame %d: mode %s, expected %s\n",
                                 cp.what.c_str(), n, mode, cp.mode.c_str());
                }
            }
            if (cp.held >= 0 && static_cast<int>(t.held) != cp.held) {
                ++failures;
                std::fprintf(stderr, "physics3d_traversal_tests: route checkpoint \"%s\" at frame %d: %u hands hold, expected %d\n",
                             cp.what.c_str(), n, t.held, cp.held);
            }
        }
    }
    // What the route did, by event: the climbs, the pull-ups, the refusal, the slide and the falls.
    int gripped = 0, finished = 0, blocked = 0, slipped = 0, fell = 0, exhausted = 0;
    for (const CharacterEvent3D& e : run.events) {
        gripped += e.kind == CharacterEventKind3D::Gripped;
        finished += e.kind == CharacterEventKind3D::MantleFinished;
        blocked += e.kind == CharacterEventKind3D::MantleBlocked;
        slipped += e.kind == CharacterEventKind3D::Slipped;
        fell += e.kind == CharacterEventKind3D::Fell;
        exhausted += e.kind == CharacterEventKind3D::Exhausted;
    }
    CHECK(gripped >= 10);
    CHECK(finished == 2); // the wall and the holds
    CHECK(blocked == 1);  // under the low roof
    CHECK(slipped >= 1);  // the ice
    CHECK(fell >= 3);
    CHECK(exhausted == 0);
    // The same route twice: the same hash at every step.
    RouteRun again(serial(), route);
    RouteRun first(serial(), route);
    for (int n = 0; n < route.frames; ++n) {
        first.step();
        again.step();
        if (first.world.state_hash() != again.world.state_hash()) {
            CHECK(false);
            break;
        }
    }
#endif
}

void trace_climb(const std::string& which) {
    if (which == "landing") {
        Rig r;
        climb_to_top(r);
        r.tin.pull_up = true;
        for (int i = 0; i < 60; ++i) {
            r.step();
            if (i >= 36) {
                const CharacterState3D s = r.state();
                std::printf("step %d mode %d ground %d y %.4f vy %.3f events:", i, int(r.trav().mode), int(s.ground), s.position.y, s.vertical_speed);
                for (const auto& e : r.events) if (e.kind == CharacterEventKind3D::Landed) std::printf(" LANDED");
                std::printf("\n");
            }
        }
        return;
    }
    if (which == "lowstamina") {
        CharacterDesc3D d = climber();
        d.traversal.resource.max = 20.0f;
        Rig r(serial(), d);
        r.tin.grab = {true, true};
        r.tin.reach = {0.0f, 0.45f, -1.0f};
        r.step(30);
        r.input.move = {0.0f, 1.0f};
        for (int i = 0; i < 100; ++i) {
            r.step();
            if (i % 4 == 0) dump(r, "low");
        }
        return;
    }
    if (which == "onehand") {
        Rig r;
        r.tin.grab = {false, true};
        r.tin.reach = {0.0f, 0.45f, -1.0f};
        for (int i = 0; i < 120; ++i) {
            r.step();
            if (i % 10 == 9) {
                const TraversalState3D t = r.trav();
                std::printf("f%d feet x %.3f place x %.3f anchor x %.3f swing x %.3f mode %d\n", i, r.state().position.x, t.hang_place.x, t.grips[1].point.x, t.swing.x, int(t.mode));
            }
        }
        return;
    }
    if (which.rfind("probe@", 0) == 0) {
        // probe@<route>@<frame>@<hand>: the candidates the hand would see at that frame
        const std::size_t a = which.find('@', 6), b = which.find('@', a + 1);
        const std::string path = which.substr(6, a - 6);
        const int frame = std::atoi(which.substr(a + 1, b - a - 1).c_str());
        const u32 hand = static_cast<u32>(std::atoi(which.substr(b + 1).c_str()));
        const JsonParseResult parsed = parse_json(read_file(path));
        const climb_level::Route route = climb_level::load_route(*parsed.value->find("character"));
        RouteRun run(serial(), route);
        for (int i = 0; i < frame; ++i) run.step();
        const CharacterState3D s = run.world.character(run.hero);
        const TraversalState3D t = run.world.traversal(run.hero);
        const CharacterDesc3D d = climb_level::climber_desc();
        std::printf("feet (%.3f %.3f %.3f) mode %d facing (%.2f %.2f %.2f)\n", s.position.x, s.position.y, s.position.z, int(t.mode), 0.0f, 0.0f, -1.0f);
        GripProbeDesc3D p;
        const Vec3f sh = d.traversal.limbs[hand].shoulder;
        p.origin = s.position + Vec3f{sh.x, sh.y, 0.0f};
        p.direction = run.applied.traversal.reach;
        p.reach = d.traversal.reach;
        p.up = {0, 1, 0};
        p.forward = {0, 0, -1};
        p.ignore = s.body;
        std::printf("origin (%.2f %.2f %.2f) dir (%.2f %.2f %.2f)\n", p.origin.x, p.origin.y, p.origin.z, p.direction.x, p.direction.y, p.direction.z);
        for (const auto& c : run.world.probe_grips(p)) {
            std::printf("cand body %u kind %d point (%.2f %.2f %.2f) q %.2f score %.2f dist %.2f\n", c.body.index, int(c.kind), c.point.x, c.point.y, c.point.z, c.quality, c.score, c.distance);
        }
        return;
    }
    if (which == "shimmy_end") {
        Rig r;
        climb_to_top(r);
        r.input.move = {1.0f, 0.0f};
        for (int i = 0; i < 300; ++i) { r.step(); if (i % 10 == 0) dump(r, "shim"); }
        events(r);
        return;
    }
    if (which == "platform") {
        Rig r(serial(), climber(), {27.0f, 0.0f, 1.5f});
        r.tin.grab = {true, true};
        r.tin.reach = {0.0f, 0.5f, -1.0f};
        for (int i = 0; i < 300; ++i) { r.step(); if (i % 10 == 0) { dump(r, "plat"); std::printf("   platform x %.3f\n", r.world.position(r.level.platform).x); } }
        events(r);
        return;
    }
    if (which == "crate2") {
        Rig r(serial(), climber(), {40.0f, 0.0f, 1.7f});
        r.tin.grab = {true, true};
        r.tin.reach = {0.0f, 0.8f, -0.6f};
        for (int i = 0; i < 150; ++i) { r.step(); if (i % 10 == 0) { dump(r, "crate"); const Vec3f c = r.world.position(r.level.crate); const Quatf q = r.world.rotation(r.level.crate); std::printf("   crate (%.3f %.3f %.3f) q (%.3f %.3f %.3f %.3f)\n", c.x, c.y, c.z, q.x, q.y, q.z, q.w); } }
        events(r);
        return;
    }
    if (which == "probe") {
        World3D world(serial());
        const climb_level::Level level = climb_level::build(world);
        GripProbeDesc3D smooth;
        smooth.origin = {20.0f, 1.0f, 0.7f};
        smooth.direction = {0.0f, 0.0f, -1.0f};
        smooth.reach = 0.8f;
        for (const auto& c : world.probe_grips(smooth)) {
            std::printf("cand body %u kind %d point (%.2f %.2f %.2f) normal (%.2f %.2f %.2f) q %.2f score %.2f\n", c.body.index, int(c.kind), c.point.x, c.point.y, c.point.z, c.normal.x, c.normal.y, c.normal.z, c.quality, c.score);
        }
        return;
    }
    if (which == "exhaust") {
        Rig r;
        r.world.set_traversal_resource(r.hero, 6.0f);
        grab_wall(r);
        for (int i = 0; i < 140; ++i) { r.step(); if (i % 5 == 0) dump(r, "exh"); }
        events(r);
    } else if (which == "wet") {
        Rig w;
        w.world.set_surface_material(w.level.wall, {.grip = 1.0f, .slip = 0.0f, .drain = 2.0f, .tag = 7});
        grab_wall(w);
        w.step(30);
        w.input.move = {0.0f, 1.0f};
        for (int i = 0; i < 200; ++i) { w.step(); if (i % 10 == 0) dump(w, "wet"); }
    } else if (which == "crate") {
        Rig r(serial(), climber(), {40.0f, 0.0f, 1.7f});
        r.tin.grab = {true, true};
        r.tin.reach = {0.0f, 0.8f, -0.6f};
        for (int i = 0; i < 100; ++i) { r.step(); if (i % 10 == 0) dump(r, "crate"); }
        events(r);
    } else if (which == "hold") {
        Rig r(serial(), climber(), {20.0f, 0.0f, 0.7f});
        r.tin.grab = {true, true};
        r.tin.reach = {-0.2f, 0.3f, -1.0f};
        for (int i = 0; i < 60; ++i) { r.step(); if (i % 10 == 0) dump(r, "hold"); }
        events(r);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "-r") { dump_route(argv[2], argc > 3 ? std::atoi(argv[3]) : 15); return 0; }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-v") verbose = true;
        if (std::string(argv[i]) == "-t" && i + 1 < argc) { trace_climb(argv[i + 1]); return 0; }
    }
    test_probe();
    test_ledge_across_bodies();
    test_pushed_while_hanging();
    test_climb_and_mantle();
    test_phases_and_latch();
    test_two_hands_one_hand_and_drain();
    test_resource();
    test_sprint_spends_resource();
    test_exhaustion_falls();
    test_low_stamina_climb_falls();
    test_events_sorted();
    test_mantle_blocked_and_clear();
    test_shimmy();
    test_jump_off_and_drop();
    test_platform_carries_grip();
    test_crate_load();
    test_ice_slips();
    test_lost_body_and_handles();
    test_authored_hold_snaps();
    test_three_limbs();
    test_standing_on_the_ground_is_free();
    test_walking_is_unchanged();
    test_redirected_gravity();
    test_determinism();
    test_save_restore_mid_hang();
    test_hash_sees_traversal();
    test_debug_draw_grips();
    test_scripted_route();
    if (failures != 0) {
        std::fprintf(stderr, "physics3d_traversal_tests: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("physics3d_traversal_tests: all passed\n");
    return 0;
}
