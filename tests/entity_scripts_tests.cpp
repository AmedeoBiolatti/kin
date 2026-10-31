// Scripts on entities: started, moved on each update, their self their own;
// they read input actions and axes, read and set components, drive an
// Animator, spawn prefabs, find entities; a failing call or a script that
// does not read is told and skipped; a changed file is read again.

#include <kin/ecs/animator.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/entity_scripts.hpp>
#include <kin/scripting/lua_script.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

bool near(float a, float b) {
    return std::abs(a - b) < 1e-3f;
}

void test_entity_scripts() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_scripts_test";
    fs::remove_all(dir);
    write(dir / "scripts/mover.lua", R"lua(
function on_start(self)
    self.steps = 0
    self:set("RectRenderer", {color = {r = 10, g = 20, b = 30, a = 255}, size = {x = 8, y = 4}})
    self.coin = world.spawn("prefabs/coin.kinprefab", 5, 6)
end

function on_update(self, dt)
    self.steps = self.steps + 1
    self:move(input.axis("move_x") * 100 * dt, 0)
    if input.pressed("jump") then self:trigger("jump") end
    if self.steps == 3 then
        self:set_rotation(self:rotation() + 90)
        self:set_param("tired", true)
    end
    local x, y = self.coin:position()
    if world.find("Hero") then self:set("Transform2D", {scale = {x = x, y = y}}) end
end
)lua");
    write(dir / "scripts/broken.lua", "function on_update(self, dt)\n    self.missing.field = 1\nend\n");
    write(dir / "scripts/unreadable.lua", "function on_update(self dt) end\n");
    write(dir / "prefabs/coin.kinprefab",
          R"({"schema":"kin.prefab/1","id":"coin","root":"coin","entities":[{"id":"coin","components":{"Transform2D":{},"RectRenderer":{}}}]})");

    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    assert(world.components().find("ScriptComponent"));
    kin::EcsEntity hero = world.entity("Hero")
                              .set(kin::Transform2D{})
                              .set(kin::RectRenderer{})
                              .set(kin::Animator{})
                              .set(kin::ScriptComponent{.asset = {.id = "scripts/mover.lua"}});
    kin::EcsEntity other = world.entity("Other").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "scripts/broken.lua"}});
    world.entity("Third").set(kin::ScriptComponent{.asset = {.id = "scripts/unreadable.lua"}});

    kin::InputMap map;
    map.bind("jump", kin::Key::Space);
    map.bind("right", kin::Key::D);
    map.set_axis({.name = "move_x", .positive = {*kin::parse_binding_text("key:D")}});
    kin::Input input;
    input.set_map(map);
    input.set_action_held("right", true);

    kin::EntityScripts scripts{{.content = dir, .check_frames = 1}};
    scripts.update(world, input, 0.1f);
    // Started: its renderer set, a coin spawned where it said.
    assert(hero.get<kin::RectRenderer>()->color.r == 10 && hero.get<kin::RectRenderer>()->size == (kin::Vec2f{8, 4}));
    const kin::EcsEntity coin = world.entities().find_by_name("coin");
    assert(coin.valid() && coin.get<kin::Transform2D>()->pos == (kin::Vec2f{5, 6}));
    assert(near(hero.get<kin::Transform2D>()->pos.x, 10.0f));
    assert(hero.get<kin::Transform2D>()->scale == (kin::Vec2f{5, 6}));
    input.set_action_pressed("jump");
    scripts.update(world, input, 0.1f);
    assert(hero.get<kin::Animator>()->params.triggers.contains("jump"));
    scripts.update(world, input, 0.1f);
    assert(near(hero.get<kin::Transform2D>()->pos.x, 30.0f) && near(hero.get<kin::Transform2D>()->rotation, 90.0f));
    assert(hero.get<kin::Animator>()->params.get_bool("tired"));
    // A failing call is told, with where; the other scripts go on. One that does not read too.
    assert(scripts.problems().at("scripts/broken.lua").find("broken.lua:2") != std::string::npos);
    assert(scripts.problems().contains("scripts/unreadable.lua"));
    assert(scripts.running() == 2);
    (void)other;

    // Changed: read again; its entities start over (another coin).
    write(dir / "scripts/mover.lua", "function on_start(self) self:set_position(-1, -2) end\n");
    fs::last_write_time(dir / "scripts/mover.lua", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    scripts.update(world, input, 0.1f);
    assert(hero.get<kin::Transform2D>()->pos == (kin::Vec2f{-1, -2}));
    // Its script taken away: it stops.
    hero.raw().remove<kin::ScriptComponent>();
    scripts.update(world, input, 0.1f);
    assert(scripts.running() == 1);

    std::string error;
    assert(kin::check_lua_syntax("local x = 1", "fine.lua", error) && error.empty());
    assert(!kin::check_lua_syntax("function (", "bad.lua", error) && error.find("bad.lua:1") != std::string::npos);
    fs::remove_all(dir);
}

// scene.go asks the scene's flow for a travel; scene.going() says one is asked.
void test_scene_go() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_scripts_scene_test";
    fs::remove_all(dir);
    write(dir / "scripts/exit.lua", R"lua(
function on_update(self, dt)
    if not self.asked then
        self.asked = true
        self.was = scene.going()
        scene.go("scenes/cave.kinscene", "Entrance", "pixelate", 1.25)
        self:set("Transform2D", {pos = {x = scene.going() and 1 or 0, y = self.was and 1 or 0}})
    elseif not self.wrong then
        self.wrong = true
        scene.go("scenes/x.kinscene", "", "sparkle")
    end
end
)lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::EcsEntity hero = world.entity("Hero").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "scripts/exit.lua"}});
    kin::Input input;
    kin::SceneFlow flow;
    kin::EntityScripts scripts{{.content = dir, .check_frames = 0}};
    scripts.set_flow(&flow);
    scripts.update(world, input, 0.1f);
    assert(hero.get<kin::Transform2D>()->pos == (kin::Vec2f{1, 0}));
    std::optional<kin::SceneTravel> go = flow.take();
    assert(go && go->scene == "scenes/cave.kinscene" && go->arrive == "Entrance");
    assert(go->transition == kin::TransitionKind::Pixelate && near(go->duration, 1.25f));
    // A transition with no such name: an error, and nothing asked.
    scripts.update(world, input, 0.1f);
    assert(!flow.pending() && scripts.problems().at("scripts/exit.lua").find("sparkle") != std::string::npos);
    fs::remove_all(dir);
}

} // namespace

int main() {
    test_entity_scripts();
    test_scene_go();
    return 0;
}
