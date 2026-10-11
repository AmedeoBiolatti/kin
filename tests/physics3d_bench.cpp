// Step cost of kin/physics3d: piles and stacks of 500 and 2000 dynamic boxes, stepped
// at 60 Hz with no job workers, 1, 4 and 8. Prints milliseconds a step as the mean and
// the worst of three windows: the first 120 steps (falling and landing), then steps 120
// to 360 (settling and sleeping). docs/physics_3d.md.

#include <kin/core/jobs.hpp>
#include <kin/physics3d/world3d.hpp>

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

} // namespace

int main() {
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
