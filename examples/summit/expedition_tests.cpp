#include "camera.hpp"
#include "game.hpp"
#include "hud.hpp"
#include "script.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {
using namespace summit;
int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            ++failures;                                                                                                \
            std::fprintf(stderr, "expedition_tests:%d: %s\n", __LINE__, #condition);                                   \
        }                                                                                                              \
    } while (0)

void step(Game& game, PlayerCommand c = {}, int n = 1) {
    for (int i = 0; i < n; ++i)
        game.step({&c, 1});
}

void begin(Game& game) {
    step(game, {}, 10);
    CHECK(game.interaction() == ExpeditionInteraction::Begin);
    step(game, {.interact = true});
    CHECK(game.player(0).expedition.phase == ExpeditionPhase::Climbing);
}

void progression() {
    Game game(1, {.expedition = true});
    const Vec3f start = game.feet(0);
    step(game, {.move = {0, 1}, .jump = true, .sprint = true}, 60);
    CHECK(length(game.feet(0) - start) < 0.05f);
    begin(game);
    const auto& e = game.level().expedition;
    // Entering the extraction zone before repairing the beacon cannot finish a run.
    game.debug_teleport(0, e.extraction);
    step(game, {.interact = true}, 180);
    CHECK(!game.player(0).summit);
    CHECK(game.player(0).expedition.phase == ExpeditionPhase::Climbing);
    game.debug_teleport(0, e.beacon + Vec3f{0, 20, 0});
    step(game, {.interact = true}, 20);
    CHECK(game.player(0).expedition.beacon_steps == 0);
    // Walking away and releasing both interrupt the repair hold.
    game.debug_teleport(0, e.beacon);
    step(game, {}, 10);
    CHECK(game.interaction() == ExpeditionInteraction::Beacon);
    step(game, {.interact = true}, 40);
    CHECK(game.player(0).expedition.beacon_steps == 40);
    step(game);
    CHECK(game.player(0).expedition.beacon_steps == 0);
    step(game, {.interact = true}, 40);
    game.debug_teleport(0, e.beacon + e.rotation.rotate({-4, 0, 0}));
    step(game, {.interact = true}, 10);
    CHECK(game.player(0).expedition.beacon_steps == 0);
    game.debug_teleport(0, e.beacon);
    step(game, {}, 10);
    step(game, {.interact = true}, 45);
    const auto saved = game.save();
    const auto saved_hash = game.state_hash();
    Game other(1, {.expedition = true});
    CHECK(other.restore(saved));
    CHECK(other.state_hash() == saved_hash);
    Game classic(1);
    CHECK(!classic.restore(saved));
    CHECK(!game.restore(classic.save()));
    step(game, {.interact = true}, beacon_hold_steps - 45);
    step(other, {.interact = true}, beacon_hold_steps - 45);
    CHECK(game.state_hash() == other.state_hash());
    CHECK(game.player(0).expedition.phase == ExpeditionPhase::Extraction);
    CHECK(!game.player(0).summit);
    int activated = 0;
    for (const auto& event : game.events())
        activated += event.kind == GameEventKind::BeaconActivated;
    CHECK(activated == 1);
    game.debug_teleport(0, e.extraction);
    step(game, {}, 10);
    step(game, {.interact = true}, extraction_hold_steps);
    CHECK(game.player(0).expedition.phase == ExpeditionPhase::Complete);
    CHECK(game.player(0).summit);
    const auto complete = game.save();
    CHECK(other.restore(complete));
    const HudModel result = build_hud_model(game, 0, {});
    CHECK(result.objective == "EXPEDITION COMPLETE");
    const Vec3f finish = game.feet(0);
    step(game, {.move = {0, 1}, .jump = true, .sprint = true}, 60);
    CHECK(length(game.feet(0) - finish) < 0.05f);
    CHECK(build_hud_model(game, 0, {}).run_seconds == result.run_seconds);
}

void shelter_and_collision() {
    Game game(7, {.expedition = true});
    begin(game);
    const auto& e = game.level().expedition;
    const Vec3f floor = e.shelter_centre - Vec3f{0, 2, 0};
    game.debug_teleport(0, floor);
    game.debug_set_health(0, 40);
    step(game, {}, 120);
    CHECK(game.player(0).health > 59);
    CHECK(game.player(0).expedition.shelter_visited);
    CHECK(game.interaction() == ExpeditionInteraction::Recover);
    CHECK(game.character_state(0).ground == CharacterGround3D::OnGround);
    CHECK(std::abs(game.feet(0).y - floor.y) < 0.1f);
    CHECK(build_hud_model(game, 0, {}).sheltered);
    // All three enclosing faces are real colliders, including the inclined roof.
    const Vec3f head = floor + Vec3f{0, 1.8f, 0};
    CHECK(game.world().raycast(head, head + Vec3f{0, 8, 0}).has_value());
    CHECK(game.world().raycast(head, head + e.rotation.rotate({8, 0, 0})).has_value());
    CHECK(game.world().raycast(head, head + e.rotation.rotate({-8, 0, 0})).has_value());
    CameraRig rig;
    rig.observe(game);
    const auto camera = rig.frame(game.world(), 1, fixed_step);
    CHECK(std::isfinite(camera.position.x));
    CHECK(camera.arm >= 0 && camera.arm <= rig.settings.distance);
}

void prop_collision() {
    Game classic(1);
    CHECK(classic.props().empty());
    for (u64 seed : {1ull, 7ull, 30ull}) {
        Game game(seed, {.expedition = true});
        CHECK(game.props().size() == 31);
        for (std::size_t i = 0; i < game.props().size(); ++i) {
            const PropPlacement& prop = game.props()[i];
            const Body3D body = game.prop_body(i);
            CHECK(!game.world().surface_material(body).grippable);
            CHECK(length(game.world().position(body) - prop.feet) < 0.00001f);
            const Shape3D shape = game.world().shape(body);
            Vec3f a, b;
            const auto& geo = prop_geometry_data(prop.kind);
            if (!geo.indices.empty()) {
                // Shoot through the largest real triangle, avoiding the tent's thin poles and the ground below a fire
                // ring.
                f32 largest = -1;
                for (std::size_t t = 0; t < geo.indices.size(); t += 3) {
                    const Vec3f x = geo.points[geo.indices[t]] * prop.span;
                    const Vec3f y = geo.points[geo.indices[t + 1]] * prop.span;
                    const Vec3f z = geo.points[geo.indices[t + 2]] * prop.span;
                    const Vec3f normal = cross(y - x, z - x);
                    if (length(normal) <= largest)
                        continue;
                    largest = length(normal);
                    const Vec3f centre = (x + y + z) / 3;
                    a = centre + normalize(normal) * 0.03f;
                    b = centre - normalize(normal) * 0.03f;
                }
            } else {
                const Vec3f lo = shape.bounds_min(), hi = shape.bounds_max();
                a = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, hi.z + 0.05f};
                b = {a.x, a.y, lo.z - 0.05f};
            }
            const Quatf rotation = Quatf::axis_angle({0, 1, 0}, prop.yaw);
            const auto hit = game.world().raycast(prop.feet + rotation.rotate(a), prop.feet + rotation.rotate(b));
            CHECK(hit.has_value());
            if (hit)
                CHECK(hit->body == body);
        }
        begin(game);
        const PropPlacement tree = game.props().front();
        CHECK(tree.kind == Tree || tree.kind == TallTree);
        const Vec3f right = Quatf::axis_angle({0, 1, 0}, game.player(0).yaw).rotate({1, 0, 0});
        game.debug_teleport(0, tree.feet - right * 1.3f);
        step(game, {}, 5);
        const Vec3f start = game.feet(0);
        step(game, {.move = {1, 0}}, 45);
        CHECK(dot(game.feet(0) - tree.feet, right) < -0.3f);
        CHECK(dot(game.feet(0) - start, right) > 0.2f);
        auto save = game.save();
        save[4] = 2; // the previous expedition layout had no prop bodies
        CHECK(!game.restore(save));
    }
}

void dense_routes() {
    for (u64 seed : {1ull, 7ull, 30ull}) {
        const auto level = generate_expedition_level(seed, true);
        const auto scenery = scenery_props(level);
        CHECK(scenery.size() > 5000 && scenery.size() <= 12000);
        CHECK(level.hash() == generate_expedition_level(seed, true).hash());
        CHECK(level.hash() != generate_expedition_level(seed).hash());
        for (bool sheltered : {false, true}) {
            Game game(seed, {.expedition = true, .dense_scenery = true});
            CHECK(game.props().size() > 500 && game.props().size() < 2000);
            Bot bot(game, 0, {.sheltered_route = sheltered});
            const auto run = run_bot(game, bot, 12000);
            std::printf("dense route seed %llu cave %d: %llu steps, %zu scenery, %zu colliders\n",
                        (unsigned long long)seed, int(sheltered), (unsigned long long)run.steps, scenery.size(),
                        game.props().size());
            CHECK(run.summit && !run.non_finite && !run.falls && !run.deaths);
            CHECK(game.player(0).expedition.shelter_visited == sheltered);
            Game compact(seed, {.expedition = true});
            CHECK(!compact.restore(game.save()));
            CHECK(!game.restore(compact.save()));
            if (seed == 1 && sheltered) {
                const auto path = (std::filesystem::temp_directory_path() / "kin-summit-dense-log.bin").string();
                CHECK(write_command_log(path, seed, run.log, true, true));
                u64 recorded_seed = 0;
                bool expedition = false, dense = false;
                std::vector<PlayerCommand> commands;
                CHECK(read_command_log(path, recorded_seed, commands, &expedition, &dense));
                CHECK(recorded_seed == seed && expedition && dense && commands == run.log);
                Game copy(seed, {.expedition = true, .dense_scenery = true});
                CHECK(replay(copy, commands) == run.final_hash);
                CHECK(copy.restore(game.save()));
            }
        }
    }
}

void routes_and_replay() {
    for (u64 seed : {1ull, 7ull, 30ull}) {
        const auto a = generate_expedition_level(seed), b = generate_expedition_level(seed);
        CHECK(a.hash() == b.hash());
        for (bool sheltered : {false, true}) {
            Game game(seed, {.expedition = true});
            Bot bot(game, 0, {.sheltered_route = sheltered});
            const RunReport run = run_bot(game, bot, 12000);
            std::printf("expedition_tests: seed %llu, %s: %llu steps, falls %d, complete %d\n",
                        (unsigned long long)seed, sheltered ? "cave" : "direct", (unsigned long long)run.steps,
                        run.falls, run.summit);
            CHECK(run.summit && !run.non_finite && run.deaths == 0 && run.falls == 0);
            CHECK(game.player(0).expedition.shelter_visited == sheltered);
            CHECK(game.player(0).camp == 2);
            Game copy(seed, {.expedition = true});
            CHECK(replay(copy, run.log) == run.final_hash);
            if (seed == 1 && sheltered) {
                const auto path = (std::filesystem::temp_directory_path() / "kin-summit-expedition-log.bin").string();
                CHECK(write_command_log(path, seed, run.log, true));
                bool expedition = false;
                u64 log_seed = 0;
                std::vector<PlayerCommand> log;
                CHECK(read_command_log(path, log_seed, log, &expedition));
                CHECK(expedition && log_seed == seed && log == run.log);
            }
        }
    }
    for (Vec2f screen : {Vec2f{1280, 720}, Vec2f{800, 600}, Vec2f{320, 180}}) {
        const auto l = hud_layout(screen);
        for (const Rectf r : {l.objective, l.interaction, l.expedition_panel}) {
            CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= screen.x && r.y + r.h <= screen.y);
        }
        CHECK(l.objective.y + l.objective.h < l.interaction.y);
        CHECK(l.interaction.y + l.interaction.h < l.hands.y);
    }
}
} // namespace

int main() {
    progression();
    shelter_and_collision();
    prop_collision();
    dense_routes();
    routes_and_replay();
    if (failures) {
        std::fprintf(stderr, "expedition_tests: %d failures\n", failures);
        return 1;
    }
    std::puts("expedition_tests: all passed");
    return 0;
}
