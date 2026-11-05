// What Python systems cost (docs/python_scripting.md, Cost): N entities with
// kin's Transform2D and a script's Velocity, moved and bounced each update by
//   - a Python @system over both;
//   - a Python component's update(self, dt);
//   - plain C++ over the same components.
// Run: ninja kin_python_bench && ./bin/kin_python_bench [entities] [updates]

#include <kin/ecs/component.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/python.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const char* system_script = R"py(from kin import component, system, Transform2D, vec2

@component
class Velocity:
    vel: vec2 = vec2(30, 20)

@system(Transform2D, Velocity)
def bounce(dt, t, v):
    p = t.pos + v.vel * dt
    vel = v.vel
    if p.x < 0 or p.x > 640:
        vel = vec2(-vel.x, vel.y)
        v.vel = vel
    if p.y < 0 or p.y > 360:
        v.vel = vec2(vel.x, -vel.y)
    t.pos = p
)py";

const char* method_script = R"py(from kin import component, Transform2D, vec2

@component
class Velocity:
    vel: vec2 = vec2(30, 20)

    def update(self, dt):
        t = self.entity[Transform2D]
        p = t.pos + self.vel * dt
        vel = self.vel
        if p.x < 0 or p.x > 640:
            vel = vec2(-vel.x, vel.y)
            self.vel = vel
        if p.y < 0 or p.y > 360:
            self.vel = vec2(vel.x, -vel.y)
        t.pos = p
)py";

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::vector<kin::EcsEntity> populate(kin::EcsWorld& world, int count) {
    std::vector<kin::EcsEntity> made;
    for (int i = 0; i < count; ++i) {
        kin::EcsEntity entity = world.entities().create();
        entity.set(kin::Transform2D{.pos = {static_cast<float>(i % 640), static_cast<float>(i % 360)}});
        world.components().add(entity, "Velocity");
        made.push_back(entity);
    }
    return made;
}

void fail_on_problems(const kin::PythonScripts& scripts) {
    for (const auto& [module, problem] : scripts.problems()) {
        std::fprintf(stderr, "%s: %s\n", module.c_str(), problem.c_str());
        std::exit(1);
    }
}

double run_python(const char* script, int count, int updates) {
    const fs::path dir = fs::temp_directory_path() / "kin_python_bench";
    fs::remove_all(dir);
    fs::create_directories(dir / "scripts");
    std::ofstream(dir / "scripts/bench.py", std::ios::binary) << script;
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PythonScripts scripts{world, {.content = dir, .check_frames = 0, .instruction_limit = 0}};
    fail_on_problems(scripts);
    populate(world, count);
    kin::Input input;
    scripts.update(input, 1.0f / 60.0f); // starts, caches
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < updates; ++i) {
        scripts.update(input, 1.0f / 60.0f);
    }
    const double each = milliseconds_since(start) / updates;
    fail_on_problems(scripts);
    return each;
}

double run_cpp(int count, int updates) {
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    world.components().data("Velocity").field("vel", kin::ComponentFieldKind::Vec2f, kin::Vec2f{30.0f, 20.0f});
    populate(world, count);
    const kin::ComponentId velocity = world.components().find("Velocity")->id;
    auto query = world.raw().query_builder<kin::Transform2D>().with(static_cast<flecs::id_t>(velocity)).build();
    const float dt = 1.0f / 60.0f;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < updates; ++i) {
        query.each([&](flecs::entity e, kin::Transform2D& t) {
            std::vector<kin::ComponentFieldValue>* values = world.components().data_values(kin::EcsEntity{e}, velocity);
            kin::Vec2f& vel = std::get<kin::Vec2f>((*values)[0]);
            const kin::Vec2f p{t.pos.x + vel.x * dt, t.pos.y + vel.y * dt};
            if (p.x < 0 || p.x > 640) {
                vel.x = -vel.x;
            }
            if (p.y < 0 || p.y > 360) {
                vel.y = -vel.y;
            }
            t.pos = p;
        });
    }
    return milliseconds_since(start) / updates;
}

} // namespace

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 10'000;
    const int updates = argc > 2 ? std::atoi(argv[2]) : 120;
    std::printf("%d entities, %d updates, ms per update\n", count, updates);
    std::printf("  python @system      %8.3f\n", run_python(system_script, count, updates));
    std::printf("  python update(self) %8.3f\n", run_python(method_script, count, updates));
    std::printf("  c++                 %8.3f\n", run_cpp(count, updates));
    return 0;
}
