// Step cost of kin/physics3d: piles and stacks of 500 and 2000 dynamic boxes, stepped
// at 60 Hz with no job workers, 1, 4 and 8. Prints milliseconds a step as the mean and
// the worst of three windows: the first 120 steps (falling and landing), then steps 120
// to 360 (settling and sleeping). docs/physics_3d.md.
//
// With the argument `queries`: the cost of P2b instead: 1000 raycasts into a pile of 2000 bodies and into a mesh of 100,000
// triangles, 100 shape casts, 100 overlaps, the step with contact events on and off for 500 bodies, and the ECS system's
// overhead (WorldPhysicsState3D::step against World3D::step) for 500 dynamic bodies.
//
// With the argument `characters`: the cost of P2c: the controller's step in the test level (tests/character_level.hpp), 0, 1, 10
// and 100 characters walking about it, with and without 500 dynamic boxes round them (a heap they walk through), world steps on the
// calling thread and with 4 workers.

#include <kin/core/jobs.hpp>
#include <kin/ecs/physics3d.hpp>
#include <kin/ecs/world.hpp>
#include <kin/physics3d/world3d.hpp>

#include "character_level.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <cstdio>
#include <vector>

using namespace kin;

namespace {

struct Lcg {
    u32 s = 12345;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<f32>(s >> 8) / static_cast<f32>(1u << 24);
    }
};

void yard(World3D& world, f32 half) {
    world.create_body({.shape = Shape3D::box({half + 2.0f, 0.5f, half + 2.0f}), .motion = Body3DMotion::Static,
                       .position = {0.0f, -0.5f, 0.0f}, .friction = 0.5f});
    const Shape3D wx = Shape3D::box({0.5f, 20.0f, half + 1.0f});
    const Shape3D wz = Shape3D::box({half + 1.0f, 20.0f, 0.5f});
    for (const f32 s : {-1.0f, 1.0f}) {
        world.create_body({.shape = wx, .motion = Body3DMotion::Static, .position = {s * (half + 0.5f), 20.0f, 0.0f}});
        world.create_body({.shape = wz, .motion = Body3DMotion::Static, .position = {0.0f, 20.0f, s * (half + 0.5f)}});
    }
}

void make_pile(World3D& world, int count) {
    const f32 half = count > 1000 ? 12.0f : 7.0f;
    yard(world, half);
    const Shape3D box = Shape3D::box({0.4f, 0.3f, 0.35f});
    Lcg rng;
    for (int i = 0; i < count; ++i) {
        const f32 x = (rng.next() - 0.5f) * 2.0f * (half - 1.0f);
        const f32 z = (rng.next() - 0.5f) * 2.0f * (half - 1.0f);
        const f32 y = 1.0f + 0.7f * static_cast<f32>(i / 80);
        const Quatf q = Quatf::axis_angle({rng.next() + 0.1f, rng.next(), rng.next()}, rng.next() * 360.0f);
        world.create_body({.shape = box, .position = {x, y, z}, .rotation = q, .friction = 0.4f});
    }
    world.optimize_broad_phase();
}

// Columns of 10 boxes in a grid, started a hair apart so that they must be solved to stay up.
void make_stacks(World3D& world, int count) {
    const f32 half = count > 1000 ? 12.0f : 7.0f;
    yard(world, half);
    const Shape3D box = Shape3D::box({0.4f, 0.25f, 0.4f});
    const int columns = count / 10;
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(columns))));
    const f32 pitch = 2.0f * (half - 1.0f) / static_cast<f32>(side);
    for (int c = 0; c < columns; ++c) {
        const f32 x = -half + 1.0f + pitch * static_cast<f32>(c % side);
        const f32 z = -half + 1.0f + pitch * static_cast<f32>(c / side);
        for (int k = 0; k < 10; ++k) {
            world.create_body({.shape = box, .position = {x + 0.001f * static_cast<f32>(k % 3), 0.25f + 0.502f * static_cast<f32>(k), z},
                               .friction = 0.5f});
        }
    }
    world.optimize_broad_phase();
}

struct Window {
    double mean = 0.0;
    double worst = 0.0;
};

Window time_steps(World3D& world, int frames) {
    Window w;
    for (int i = 0; i < frames; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        world.step(1.0f / 60.0f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        w.mean += ms;
        w.worst = std::max(w.worst, ms);
    }
    w.mean /= frames;
    return w;
}

double since_ms(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A grid mesh of `n` x `n` cells, two triangles each (n = 224: 100,352), hilly.
Shape3D hill_mesh(int n) {
    std::vector<Vec3f> v;
    std::vector<u32> idx;
    for (int z = 0; z <= n; ++z) {
        for (int x = 0; x <= n; ++x) {
            const f32 fx = static_cast<f32>(x), fz = static_cast<f32>(z);
            v.push_back({fx * 0.25f, 1.5f * std::sin(fx * 0.11f) * std::cos(fz * 0.09f), fz * 0.25f});
        }
    }
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            const u32 i = static_cast<u32>(z * (n + 1) + x);
            idx.insert(idx.end(), {i, i + static_cast<u32>(n) + 1, i + 1, i + 1, i + static_cast<u32>(n) + 1, i + static_cast<u32>(n) + 2});
        }
    }
    return Shape3D::triangle_mesh(v, idx);
}

int queries() {
    World3DConfig cfg;
    cfg.max_bodies = 4096;
    cfg.max_body_pairs = 262144;
    cfg.max_contact_constraints = 65536;
    cfg.single_threaded = true;
    {
        World3D pile(cfg);
        make_pile(pile, 2000);
        for (int i = 0; i < 200; ++i) {
            pile.step(1.0f / 60.0f);
        }
        Lcg rng;
        std::vector<Vec3f> starts;
        for (int i = 0; i < 1000; ++i) {
            starts.push_back({(rng.next() - 0.5f) * 20.0f, 12.0f, (rng.next() - 0.5f) * 20.0f});
        }
        int hits = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (const Vec3f& s : starts) {
            hits += pile.raycast(s, s + Vec3f{0.0f, -15.0f, 0.0f}).has_value() ? 1 : 0;
        }
        const double ray_ms = since_ms(t0);
        std::size_t all_hits = 0;
        t0 = std::chrono::steady_clock::now();
        for (const Vec3f& s : starts) {
            all_hits += pile.raycast_all(s, s + Vec3f{0.0f, -15.0f, 0.0f}).size();
        }
        const double all_ms = since_ms(t0);
        std::printf("pile of 2000 bodies (%d awake): 1000 raycasts %.2f ms (%.1f us each, %d hit), 1000 raycast_all %.2f ms (%.1f us each, %.1f hits)\n",
                    pile.active_body_count(), ray_ms, ray_ms, hits, all_ms, all_ms, static_cast<double>(all_hits) / 1000.0);
        const Shape3D sphere = Shape3D::sphere(0.3f);
        const Shape3D box = Shape3D::box({0.3f, 0.3f, 0.3f});
        int cast_hits = 0;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) {
            cast_hits += pile.shape_cast(i % 2 ? sphere : box, starts[static_cast<std::size_t>(i)], {}, {0.0f, -15.0f, 0.0f}).has_value() ? 1 : 0;
        }
        const double cast_ms = since_ms(t0);
        std::size_t overlaps = 0;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) {
            overlaps += pile.overlap(sphere, {starts[static_cast<std::size_t>(i)].x, 1.0f, starts[static_cast<std::size_t>(i)].z}, {}).size();
        }
        const double overlap_ms = since_ms(t0);
        std::printf("pile of 2000 bodies: 100 shape casts %.2f ms (%.1f us each, %d hit), 100 overlaps %.2f ms (%.1f us each, %.1f bodies)\n",
                    cast_ms, cast_ms * 10.0, cast_hits, overlap_ms, overlap_ms * 10.0, static_cast<double>(overlaps) / 100.0);
    }
    {
        World3D world(cfg);
        const Shape3D mesh = hill_mesh(224);
        world.create_body({.shape = mesh, .motion = Body3DMotion::Static});
        world.optimize_broad_phase();
        Lcg rng;
        int hits = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) {
            const f32 x = rng.next() * 56.0f, z = rng.next() * 56.0f;
            hits += world.raycast({x, 10.0f, z}, {x, -10.0f, z}).has_value() ? 1 : 0;
        }
        const double ms = since_ms(t0);
        std::printf("mesh of 100,352 triangles: 1000 raycasts %.2f ms (%.1f us each, %d hit)\n", ms, ms, hits);
        int casts = 0;
        const auto t1 = std::chrono::steady_clock::now();
        const Shape3D sphere = Shape3D::sphere(0.3f);
        for (int i = 0; i < 100; ++i) {
            const f32 x = rng.next() * 56.0f, z = rng.next() * 56.0f;
            casts += world.shape_cast(sphere, {x, 10.0f, z}, {}, {0.0f, -20.0f, 0.0f}).has_value() ? 1 : 0;
        }
        std::printf("mesh of 100,352 triangles: 100 sphere casts %.2f ms (%.1f us each, %d hit)\n", since_ms(t1), since_ms(t1) * 10.0, casts);
    }
    // Events: a pile of 500 stepped with contact events off, on, and on with persist events.
    std::printf("%-26s %12s %12s %14s\n", "500 body pile, one thread", "steps 0-120", "steps 120-360", "events");
    for (int mode = 0; mode < 3; ++mode) {
        World3DConfig c = cfg;
        c.contact_events = mode > 0;
        c.persist_events = mode == 2;
        World3D world(c);
        make_pile(world, 500);
        std::size_t events = 0;
        Window a, b;
        for (int phase = 0; phase < 2; ++phase) {
            Window& w = phase == 0 ? a : b;
            const int n = phase == 0 ? 120 : 240;
            for (int i = 0; i < n; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                world.step(1.0f / 60.0f);
                const double ms = since_ms(t0);
                w.mean += ms;
                w.worst = std::max(w.worst, ms);
                events += world.drain_events().size();
            }
            w.mean /= n;
        }
        std::printf("%-26s %8.3f ms %9.3f ms %14zu\n", mode == 0 ? "events off" : mode == 1 ? "begin and end events" : "with persist events",
                    a.mean, b.mean, events);
    }
    // The ECS system against the raw world: 500 dynamic boxes (the same recipe), the same steps.
    {
        Window raw_a, raw_b, ecs_a, ecs_b;
        {
            World3D world(cfg);
            make_pile(world, 500);
            raw_a = time_steps(world, 120);
            raw_b = time_steps(world, 240);
        }
        {
            kin::EcsWorld ecs;
            World3D world(cfg);
            WorldPhysicsState3D bodies{ecs, world};
            const f32 half = 7.0f;
            Collider3D floor;
            floor.half_extents = {half + 2.0f, 0.5f, half + 2.0f};
            ecs.raw().entity().set(Transform3D{.position = {0.0f, -0.5f, 0.0f}}).set(floor).set(RigidBody3D{.motion = Body3DMotion::Static, .friction = 0.5f});
            Collider3D wx;
            wx.half_extents = {0.5f, 20.0f, half + 1.0f};
            Collider3D wz;
            wz.half_extents = {half + 1.0f, 20.0f, 0.5f};
            for (const f32 sgn : {-1.0f, 1.0f}) {
                ecs.raw().entity().set(Transform3D{.position = {sgn * (half + 0.5f), 20.0f, 0.0f}}).set(wx);
                ecs.raw().entity().set(Transform3D{.position = {0.0f, 20.0f, sgn * (half + 0.5f)}}).set(wz);
            }
            Collider3D box;
            box.half_extents = {0.4f, 0.3f, 0.35f};
            Lcg rng;
            for (int i = 0; i < 500; ++i) {
                const f32 x = (rng.next() - 0.5f) * 2.0f * (half - 1.0f);
                const f32 z = (rng.next() - 0.5f) * 2.0f * (half - 1.0f);
                const f32 y = 1.0f + 0.7f * static_cast<f32>(i / 80);
                const Quatf q = Quatf::axis_angle({rng.next() + 0.1f, rng.next(), rng.next()}, rng.next() * 360.0f);
                ecs.raw().entity().set(Transform3D{.position = {x, y, z}, .rotation = q}).set(box).set(RigidBody3D{.friction = 0.4f});
            }
            bodies.sync_bodies();
            const auto run = [&](int frames) {
                Window w;
                for (int i = 0; i < frames; ++i) {
                    const auto t0 = std::chrono::steady_clock::now();
                    bodies.step(1.0f / 60.0f);
                    const double ms = since_ms(t0);
                    w.mean += ms;
                    w.worst = std::max(w.worst, ms);
                }
                w.mean /= frames;
                return w;
            };
            ecs_a = run(120);
            ecs_b = run(240);
        }
        std::printf("ECS, 500 dynamic bodies, one thread: World3D::step %.3f / %.3f ms (steps 0-120 / 120-360), WorldPhysicsState3D::step %.3f / %.3f ms, overhead %.3f / %.3f ms\n",
                    raw_a.mean, raw_b.mean, ecs_a.mean, ecs_b.mean, ecs_a.mean - raw_a.mean, ecs_b.mean - raw_b.mean);
    }
    return 0;
}

// The characters' cost: `count` walkers in the test level over a heap of `boxes` dynamic boxes. Each walks along +X (or back)
// on its own lane, with a jump now and then, and turns round at the ends. Milliseconds a step over steps 240 to 480 (the heap
// has settled by then; the walkers wake what they touch).
int characters() {
    std::printf("%-9s %6s %11s | %-22s | %s\n", "workers", "boxes", "characters", "step mean / worst ms", "per character (mean over none)");
    for (const int workers : {-1, 4}) {
        for (const int boxes : {0, 500}) {
            double base = 0.0;
            for (const int count : {0, 1, 10, 100}) {
                World3DConfig cfg;
                cfg.max_bodies = 4096;
                cfg.max_body_pairs = 262144;
                cfg.max_contact_constraints = 65536;
                cfg.single_threaded = workers < 0;
                JobSystem jobs({.workers = std::max(1, workers), .max_background = std::max(1, workers)});
                if (workers > 0) {
                    cfg.jobs = &jobs;
                }
                World3D world(cfg);
                const character_level::Level level = character_level::build(world);
                Lcg rng;
                for (int i = 0; i < boxes; ++i) {
                    // A heap on the walkers' side of the level (x 11 to 30, z -29 to -5) that they walk into.
                    const Vec3f at{11.0f + rng.next() * 19.0f, 0.4f + 0.55f * static_cast<f32>(i / 170), -29.0f + rng.next() * 24.0f};
                    world.create_body({.shape = Shape3D::box({0.35f, 0.25f, 0.3f}), .position = at,
                                       .rotation = Quatf::axis_angle({0, 1, 0}, rng.next() * 360.0f), .friction = 0.5f});
                }
                world.optimize_broad_phase();
                std::vector<Character3D> walkers;
                for (int i = 0; i < count; ++i) {
                    CharacterDesc3D d;
                    // A grid: five columns 2.5 m apart, rows 1.2 m apart over z -29 to -5 (clear of the pieces of the level).
                    d.position = {1.0f + static_cast<f32>(i % 5) * 2.5f, 0.0f, -29.0f + 1.2f * static_cast<f32>(i / 5)};
                    walkers.push_back(world.create_character(d));
                }
                Window w;
                const int total = 480;
                for (int step = 0; step < total; ++step) {
                    for (std::size_t i = 0; i < walkers.size(); ++i) {
                        CharacterInput3D in;
                        const bool back = ((step / 150) + static_cast<int>(i / 5)) % 2 == 1;
                        in.forward = {back ? -1.0f : 1.0f, 0.0f, 0.0f};
                        in.move = {0.0f, 1.0f};
                        in.jump = step % 97 == static_cast<int>(i % 97);
                        world.set_character_input(walkers[i], in);
                    }
                    const auto t0 = std::chrono::steady_clock::now();
                    world.step(1.0f / 60.0f);
                    const double ms = since_ms(t0);
                    if (step >= 240) {
                        w.mean += ms;
                        w.worst = std::max(w.worst, ms);
                    }
                }
                w.mean /= static_cast<double>(total - 240);
                if (count == 0) {
                    base = w.mean;
                }
                std::printf("%-9s %6d %11d | %8.3f / %-11.3f | %s", workers < 0 ? "none" : std::to_string(workers).c_str(), boxes, count,
                            w.mean, w.worst, "");
                if (count > 0) {
                    std::printf("%.4f ms (%.0f us)", (w.mean - base) / count, (w.mean - base) / count * 1000.0);
                }
                std::printf("   awake bodies %d\n", world.active_body_count());
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "queries") {
        return queries();
    }
    if (argc > 1 && std::string(argv[1]) == "characters") {
        return characters();
    }
    std::printf("%-8s %6s %8s | %-24s | %-24s | %s\n", "scene", "boxes", "workers", "steps 0-120 mean/worst ms",
                "steps 120-360 mean/worst", "awake at end");
    for (const bool stacks : {false, true}) {
        for (const int count : {500, 2000}) {
            for (const int workers : {-1, 1, 4, 8}) {
                World3DConfig cfg;
                cfg.max_bodies = 4096;
                cfg.max_body_pairs = 262144;
                cfg.max_contact_constraints = 65536;
                cfg.single_threaded = workers < 0;
                JobSystem jobs({.workers = std::max(1, workers), .max_background = std::max(1, workers)});
                if (workers > 0) {
                    cfg.jobs = &jobs;
                }
                World3D world(cfg);
                if (stacks) {
                    make_stacks(world, count);
                } else {
                    make_pile(world, count);
                }
                const Window a = time_steps(world, 120);
                const Window b = time_steps(world, 240);
                std::printf("%-8s %6d %8s | %8.3f / %-13.3f | %8.3f / %-13.3f | %d\n", stacks ? "stacks" : "pile", count,
                            workers < 0 ? "none" : std::to_string(workers).c_str(), a.mean, a.worst, b.mean, b.worst,
                            world.active_body_count());
            }
        }
    }
    return 0;
}
