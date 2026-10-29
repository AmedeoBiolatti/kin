// CPU-only four-barrier scene workload. CSV includes actual work counters.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <kin/ecs/render3d.hpp>
using namespace kin;
int main(int argc, char** argv) {
    u32 frames = 150, warmup = 50, runs = 5;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view option = argv[i];
        const u32 value = std::max(1, std::atoi(argv[i + 1]));
        if (option == "--frames")
            frames = value;
        else if (option == "--warmup")
            warmup = value;
        else if (option == "--runs")
            runs = value;
        else
            return 2;
    }
    struct Fake final : IMeshBackend3D {};
    const Mesh3D mesh{std::make_shared<Fake>(), 8, 36, {{-1, -1, -1}, {1, 1, 1}}};
    std::cout << "mode,entities,edits_percent,run,p50_ms,p95_ms,transform_visits_per_frame,recomputed_per_frame,"
                 "collection_visits_per_frame,collection_updates_per_frame,tracking_bytes\n";
    for (const auto mode : {SceneMutationMode3D::Conservative, SceneMutationMode3D::Tracked})
        for (u32 count : {10000u, 100000u}) {
            kin::EcsWorld world;
            WorldRenderState3D state{world};
            state.mutation_mode(mode);
            std::vector<flecs::entity> entities;
            for (u32 i = 0; i < count; ++i)
                entities.push_back(world.raw()
                                       .entity()
                                       .set(Transform3D{.position = {f32(i % 100), 0, f32(i / 100)}})
                                       .set(MeshRenderer3D{.mesh = mesh}));
            Scene3D scene;
            for (u32 edits : {0u, 1u})
                for (u32 run = 0; run < runs; ++run) {
                    std::vector<double> samples;
                    for (u32 frame = 0; frame < warmup + frames; ++frame) {
                        if (frame == warmup)
                            state.reset_work_stats();
                        const auto start = std::chrono::steady_clock::now();
                        if (edits)
                            for (u32 i = 0; i < count / 100; ++i) {
                                entities[i].get_mut<Transform3D>()->position.y = f32(frame % 2);
                                if (mode == SceneMutationMode3D::Tracked)
                                    entities[i].modified<Transform3D>();
                            }
                        for (int stage = 0; stage < 4; ++stage)
                            state.propagate_transforms();
                        scene.clear();
                        state.collect(scene);
                        if (frame >= warmup)
                            samples.push_back(
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                                    .count());
                    }
                    std::sort(samples.begin(), samples.end());
                    const auto stats = state.work_stats();
                    std::cout << (mode == SceneMutationMode3D::Tracked ? "tracked" : "conservative") << ',' << count
                              << ',' << edits << ',' << run << ',' << samples[samples.size() / 2] << ','
                              << samples[samples.size() * 95 / 100] << ',' << stats.transform_visits / frames << ','
                              << stats.transforms_recomputed / frames << ',' << stats.collection_visits / frames << ','
                              << stats.collection_updates / frames << ',' << stats.tracking_bytes << '\n'
                              << std::flush;
                }
        }
}
