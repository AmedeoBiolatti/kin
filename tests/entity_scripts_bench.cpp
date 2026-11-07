// What running a Lua script on each of many entities costs per update, as a
// horde game does: foes that each walk to the player (two positions, a
// velocity, a dialogue variable read), and seeds lying about that each check
// whether the player is near. EntityScripts::update is timed.
// Run: ninja kin_entity_scripts_bench && ./bin/kin_entity_scripts_bench [foes] [seeds] [frames] [empty]
// ("empty": foes whose on_update does nothing, the cost of calling them alone)

#include <kin/ecs/dialogue.hpp>
#include <kin/ecs/physics.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/entity_scripts.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

} // namespace

int main(int argc, char** argv) {
    const int foes = argc > 1 ? std::atoi(argv[1]) : 1000;
    const int seeds = argc > 2 ? std::atoi(argv[2]) : 300;
    const int frames = argc > 3 ? std::atoi(argv[3]) : 300;
    const bool empty = argc > 4 && std::string(argv[4]) == "empty";

    const fs::path dir = fs::temp_directory_path() / "kin_entity_scripts_bench";
    fs::remove_all(dir);
    write(dir / "scripts/foe.lua", empty ? "function on_update(self, dt) end\n" : R"lua(
local player
function on_start(self)
    self.speed = 40
    self.left = false
end
function on_update(self, dt)
    if not (player and player:alive()) then
        player = world.find("Player")
        if not player then return end
    end
    if dialogue.get("paused") then
        self:set_velocity(0, 0)
        return
    end
    local x, y = self:position()
    local px, py = player:position()
    local dx, dy = px - x, py - y
    local d = math.sqrt(dx * dx + dy * dy) + 0.001
    self:set_velocity(dx / d * self.speed, dy / d * self.speed)
    local left = dx < 0
    if left ~= self.left then
        self.left = left
        self:set("SpriteRenderer", {flip_x = left})
    end
end
)lua");
    write(dir / "scripts/seed.lua", R"lua(
local player
function on_update(self, dt)
    if self.flying then return end
    if not (player and player:alive()) then
        player = world.find("Player")
        if not player then return end
    end
    local x, y = self:position()
    local px, py = player:position()
    local dx, dy = px - x, py - y
    local reach = dialogue.get("magnet") or 40
    if dx * dx + dy * dy < reach * reach then
        local d = math.sqrt(dx * dx + dy * dy) + 0.001
        self:set_velocity(dx / d * 220, dy / d * 220)
        self.flying = true
    end
end
)lua");

    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    world.entity("Player").set(kin::Transform2D{.pos = {0.0f, 0.0f}});
    for (int i = 0; i < foes; ++i) {
        world.entity()
            .set(kin::Transform2D{.pos = {static_cast<kin::f32>(i % 40) * 30.0f - 600.0f, static_cast<kin::f32>(i / 40) * 30.0f - 400.0f}})
            .set(kin::SpriteRenderer{})
            .set(kin::PhysicsVelocity{})
            .set(kin::ScriptComponent{.asset = {.id = "scripts/foe.lua"}});
    }
    for (int i = 0; i < seeds; ++i) {
        world.entity()
            .set(kin::Transform2D{.pos = {static_cast<kin::f32>(i % 50) * 20.0f + 100.0f, static_cast<kin::f32>(i / 50) * 20.0f + 100.0f}})
            .set(kin::PhysicsVelocity{})
            .set(kin::ScriptComponent{.asset = {.id = "scripts/seed.lua"}});
    }

    kin::Input input;
    kin::SceneDialogue dialogue{dir, 0};
    kin::EntityScripts scripts{{.content = dir, .check_frames = 0}};
    scripts.set_dialogue(&dialogue);
    scripts.update(world, input, 1.0f / 60.0f); // started
    std::vector<double> ms;
    for (int frame = 0; frame < frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        scripts.update(world, input, 1.0f / 60.0f);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    for (const auto& [path, problem] : scripts.problems()) {
        std::printf("problem %s: %s\n", path.c_str(), problem.c_str());
    }
    std::ranges::sort(ms);
    std::printf("foes %d, seeds %d, frames %d: EntityScripts::update median %.3f ms, p90 %.3f ms (%.2f us per entity)\n", foes,
                seeds, frames, ms[ms.size() / 2], ms[ms.size() * 9 / 10], ms[ms.size() / 2] * 1000.0 / std::max(1, foes + seeds));
    fs::remove_all(dir);
    return 0;
}
