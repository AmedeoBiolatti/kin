// What the ECS's everyday writes cost with kin's components registered (as
// every game and tool has them), with the world's change log recording too
// (EcsEventRegistry::record, what tools turn on) and, for comparison, in a
// plain flecs world where the same structs are just flecs components:
//   - set:      entity.set(Transform2D) on every entity;
//   - modified: get_mut + modified, the way bindings write a field;
//   - spawn:    create entities with a Transform2D and a SpriteRenderer, then
//               destroy them all.
// Run: ninja kin_ecs_bench && ./bin/kin_ecs_bench [entities] [rounds]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/scene/scene_asset.hpp>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The median of `rounds` timings of `work`, in ms.
template <typename Work> double median_ms(int rounds, Work&& work) {
    std::vector<double> times;
    for (int i = 0; i < rounds; ++i) {
        const auto start = Clock::now();
        work(i);
        times.push_back(milliseconds_since(start));
    }
    std::ranges::sort(times);
    return times[times.size() / 2];
}

struct Result {
    double set = 0.0;
    double modified = 0.0;
    double spawn = 0.0;
};

enum class Setup { Kin, KinRecording, Flecs };

Result run(Setup setup, int count, int rounds) {
    kin::EcsWorld world;
    if (setup != Setup::Flecs) {
        kin::register_builtin_editor_components(world);
    }
    world.events().record(setup == Setup::KinRecording);
    std::vector<kin::EcsEntity> entities;
    for (int i = 0; i < count; ++i) {
        kin::EcsEntity entity{world.raw().entity()};
        entity.set(kin::Transform2D{.pos = {static_cast<float>(i), 0.0f}});
        entities.push_back(entity);
    }
    Result result;
    result.set = median_ms(rounds, [&](int round) {
        for (kin::EcsEntity entity : entities) {
            entity.set(kin::Transform2D{.pos = {static_cast<float>(round), 1.0f}});
        }
    });
    result.modified = median_ms(rounds, [&](int round) {
        for (kin::EcsEntity entity : entities) {
            entity.get_mut<kin::Transform2D>()->rotation = static_cast<float>(round);
            entity.modified<kin::Transform2D>();
        }
    });
    std::vector<kin::EcsEntity> spawned;
    spawned.reserve(static_cast<std::size_t>(count));
    result.spawn = median_ms(rounds, [&](int) {
        spawned.clear();
        for (int i = 0; i < count; ++i) {
            kin::EcsEntity entity{world.raw().entity()};
            entity.set(kin::Transform2D{.pos = {static_cast<float>(i), 2.0f}});
            entity.set(kin::SpriteRenderer{.order = i});
            spawned.push_back(entity);
        }
        for (kin::EcsEntity entity : spawned) {
            entity.destroy();
        }
    });
    return result;
}

} // namespace

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 10'000;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 60;
    const Result kin = run(Setup::Kin, count, rounds);
    const Result recording = run(Setup::KinRecording, count, rounds);
    const Result flecs = run(Setup::Flecs, count, rounds);
    std::printf("%d entities, median of %d rounds, ms\n", count, rounds);
    std::printf("                     kin world   recording   plain flecs\n");
    std::printf("  set                %9.3f   %9.3f   %11.3f\n", kin.set, recording.set, flecs.set);
    std::printf("  get_mut+modified   %9.3f   %9.3f   %11.3f\n", kin.modified, recording.modified, flecs.modified);
    std::printf("  spawn+destroy      %9.3f   %9.3f   %11.3f\n", kin.spawn, recording.spawn, flecs.spawn);
    return 0;
}
