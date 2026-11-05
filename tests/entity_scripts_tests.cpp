// Scripts on entities: started, moved on each update, their self their own;
// they read input actions and axes, read and set components, drive an
// Animator, spawn prefabs, find entities; a failing call or a script that
// does not read is told and skipped; a changed file is read again.

#include <kin/ecs/animator.hpp>
#include <kin/ecs/camera.hpp>
#include <kin/ecs/particles.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/scene_flow.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/tween.hpp>
#include <kin/ecs/world.hpp>
#include <kin/platform/input.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/entity_scripts.hpp>
#include <kin/scripting/lua_script.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#include <kin/physics/physics.hpp>
#endif

#include <cassert>
#include <cstdio>
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

    // Changed: preserve the live self and do not rerun on_start.
    const auto before_reload = hero.get<kin::Transform2D>()->pos;
    write(dir / "scripts/mover.lua", "function on_start(self) self:set_position(-1, -2) end\n");
    fs::last_write_time(dir / "scripts/mover.lua", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    scripts.update(world, input, 0.1f);
    assert(hero.get<kin::Transform2D>()->pos == before_reload);
    // Its script taken away: it stops.
    hero.raw().remove<kin::ScriptComponent>();
    scripts.update(world, input, 0.1f);
    assert(scripts.running() == 1);

    std::string error;
    assert(kin::check_lua_syntax("local x = 1", "fine.lua", error) && error.empty());
    assert(!kin::check_lua_syntax("function (", "bad.lua", error) && error.find("bad.lua:1") != std::string::npos);
    fs::remove_all(dir);
}

// Local players: the same script on two characters, each reading its own
// player's controller; a script naming a player itself.
void test_player_scripts() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_scripts_players";
    fs::remove_all(dir);
    write(dir / "scripts/player.lua", R"lua(
function on_update(self, dt)
    self:move(input.axis("move_x") * 100 * dt, 0)
    if input.pressed("jump") then self:set_rotation(input.player() * 10) end
end
)lua");
    write(dir / "scripts/watcher.lua", R"lua(
function on_update(self, dt)
    if input.held("jump", 2) then self:set_rotation(45) end
    if input.player() ~= 0 or input.held("jump", 1) then self:set_rotation(-1) end
end
)lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    const kin::EcsEntity one =
        world.entity("One").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "scripts/player.lua"}, .player = 1});
    const kin::EcsEntity two =
        world.entity("Two").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "scripts/player.lua"}, .player = 2});
    const kin::EcsEntity watcher =
        world.entity("Watcher").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "scripts/watcher.lua"}});

    kin::InputMap map;
    map.bind("jump", kin::PadControl::A);
    map.set_axis({.name = "move_x", .sticks = {{.axis = kin::PadAxis::LeftX}}, .dead_zone = 0.0f});
    kin::Input input;
    input.set_map(map);
    kin::GamepadState first{.connected = true};
    kin::GamepadState second{.connected = true, .left_x = 1.0f};
    input.set_player_gamepad(0, first);
    input.set_player_gamepad(1, second);

    kin::EntityScripts scripts{{.content = dir, .check_frames = 1}};
    scripts.update(world, input, 0.1f);
    assert(scripts.problems().empty());
    assert(near(one.get<kin::Transform2D>()->pos.x, 0.0f) && near(two.get<kin::Transform2D>()->pos.x, 10.0f));
    input.advance_step_edges();

    second.set_button(kin::GamepadButton::A, true);
    input.set_player_gamepad(1, second);
    scripts.update(world, input, 0.1f);
    assert(near(two.get<kin::Transform2D>()->rotation, 20.0f) && near(one.get<kin::Transform2D>()->rotation, 0.0f));
    assert(near(watcher.get<kin::Transform2D>()->rotation, 45.0f));
    input.advance_step_edges();
    first.set_button(kin::GamepadButton::A, true);
    input.set_player_gamepad(0, first);
    scripts.update(world, input, 0.1f);
    assert(scripts.problems().empty());
    assert(near(watcher.get<kin::Transform2D>()->rotation, -1.0f) && near(one.get<kin::Transform2D>()->rotation, 10.0f));

    // Touch: the fingers down, and two fingers' pinch.
    write(dir / "scripts/fingers.lua", R"lua(
function on_update(self, dt)
    local fingers = input.touches()
    if #fingers == 2 then
        local pan_x, pan_y = input.pan()
        self:set_position(fingers[2].x, input.pinch() * 10 + pan_x)
    end
end
)lua");
    const kin::EcsEntity hand = world.entity("Hand").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "scripts/fingers.lua"}});
    input.set_touch_down(1, {100, 100});
    input.set_touch_down(2, {200, 100});
    input.set_touch_moved(2, {300, 100});
    scripts.update(world, input, 0.1f);
    assert(scripts.problems().empty());
    assert(hand.get<kin::Transform2D>()->pos == (kin::Vec2f{300, 70})); // pinch 2 * 10 + pan 50
    fs::remove_all(dir);
}

// self:emit() bursts its ParticleSystemComponent's effect where it is, or another of its catalog.
void test_entity_emit() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_emit_test";
    fs::remove_all(dir);
    write(dir / "scripts/burst.lua", R"lua(
function on_start(self)
    assert(self:emit())
    assert(self:emit("puff"))
    assert(not self:emit("none"))
end
)lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::ParticleCatalog catalog;
    catalog.set_effect("spark", {.burst = {.count = 3, .speed = {0.0f, 0.0f}}});
    catalog.set_effect("puff", {.burst = {.count = 2, .speed = {0.0f, 0.0f}}});
    kin::ParticleSystem system;
    system.set_catalog(std::move(catalog));
    kin::EcsEntity torch = world.entity("Torch")
                               .set(kin::Transform2D{{10.0f, 20.0f}})
                               .set(kin::ParticleSystemComponent{.system = std::move(system), .effect = "spark", .offset = {0.0f, -4.0f}})
                               .set(kin::ScriptComponent{.asset = {.id = "scripts/burst.lua"}});
    kin::Input input;
    kin::EntityScripts scripts{{.content = dir}};
    scripts.update(world, input, 0.1f);
    assert(scripts.problems().empty());
    const kin::ParticleSystem& made = torch.get<kin::ParticleSystemComponent>()->system;
    assert(made.active_count() == 5);
    assert((made.particles()[0].position == kin::Vec2f{10.0f, 16.0f}));
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

// The save table: the game's values, slots written, listed and read (asked
// for); an entity marked Saved keeps its self's values (not its functions),
// given back after on_start, then on_load; world.spawn's instances are kept
// with their prefab.
void test_saves() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_scripts_saves_test";
    fs::remove_all(dir);
    write(dir / "scripts/keeper.lua", R"lua(
function on_start(self)
    self.coins = 0
    self.bag = {"key", "map"}
    self.where = {x = 1, y = 2}
    self.hello = function() end
end

function on_load(self)
    assert(math.type(self.coins) == "integer" and math.type(self.where.y) == "integer")
    save.set("loaded", self.coins)
end

function on_update(self, dt)
    self.coins = self.coins + 1
    save.set("coins_seen", self.coins)
    if self.coins == 3 then
        save.set("best", {score = self.coins, names = {"a", "b"}})
        assert(save.get("missing", 7) == 7)
        assert(save.get("best").names[2] == "b")
        local ok, why = save.write("slot_a", "Three coins", "3")
        assert(ok, why)
        assert(save.exists("slot_a") and not save.exists("slot_b"))
        local slots = save.slots()
        assert(#slots == 1 and slots[1].id == "slot_a" and slots[1].title == "Three coins" and slots[1].summary == "3")
        local fine, why2 = save.read("nope")
        assert(not fine and why2 ~= "")
        assert(save.read("slot_a"))
        world.spawn("prefabs/coin.kinprefab", 3, 4)
        save.set("coins_seen", nil)
    end
end
)lua");
    write(dir / "prefabs/coin.kinprefab",
          R"({"schema":"kin.prefab/1","id":"coin","root":"coin","entities":[{"id":"coin","components":{"Transform2D":{},"Saved":{}}}]})");
    const std::string scene_text = R"({"schema": "kin.scene/1", "entities": [
        {"id": "hero", "name": "Hero", "components": {"Transform2D": {}, "Saved": {},
                                                      "ScriptComponent": {"asset": "scripts/keeper.lua"}}}]})";
    const auto read_scene = [&](kin::EcsWorld& world) {
        kin::register_builtin_editor_components(world);
        const kin::SceneLoadResult read = kin::parse_scene_asset(scene_text, world.components());
        assert(read.ok());
        assert(kin::instantiate_scene_asset(world, *read.scene, dir, world.components()).ok());
    };
    kin::Input input;
    kin::SceneSaves saves{{.game_id = "scripts_saves", .game_version = "1", .root_override = dir / "store"}, dir};
    {
        kin::EcsWorld world;
        read_scene(world);
        kin::EntityScripts scripts{{.content = dir, .check_frames = 0}};
        scripts.set_saves(&saves);
        saves.enter(world, "scenes/a.kinscene");
        for (int i = 0; i < 3; ++i) {
            scripts.update(world, input, 0.1f);
        }
        for (const auto& [path, problem] : scripts.problems()) std::fprintf(stderr, "%s: %s\n", path.c_str(), problem.c_str());
        assert(scripts.problems().empty());
        assert(saves.take_read() == std::optional<std::string>("slot_a"));
        assert(!saves.value("coins_seen")); // nil took it away
        const kin::JsonValue& hero = *saves.kept("scenes/a.kinscene")->find("entities")->find("hero");
        const kin::JsonValue& script = *hero.find("script");
        assert(script.number_at("coins") == 3.0 && !script.contains("hello") && !script.contains("entity"));
        assert(script.find("bag")->is_array() && script.find("bag")->items()[1].as_string() == "map");
        assert(script.find("where")->number_at("y") == 2.0);
        // What world.spawn made knows its prefab (a save makes it again from it).
        int coins = 0;
        world.raw().each([&](flecs::entity, const kin::SpawnedPrefab& from) {
            assert(from.prefab == "prefabs/coin.kinprefab");
            ++coins;
        });
        assert(coins == 1);
    }
    // Read back: the script's values its own again after on_start, then on_load.
    std::string error;
    const std::optional<std::string> scene = saves.read("slot_a", error);
    assert(scene && *scene == "scenes/a.kinscene");
    assert(saves.value("best") && saves.value("best")->number_at("score") == 3.0);
    {
        kin::EcsWorld world;
        read_scene(world);
        kin::EntityScripts scripts{{.content = dir, .check_frames = 0}};
        scripts.set_saves(&saves);
        saves.enter(world, *scene);
        assert(saves.problems().empty());
        scripts.update(world, input, 0.1f);
        assert(scripts.problems().empty());
        assert(saves.value("loaded") && saves.value("loaded")->as_number() == 3.0);
        assert(saves.value("coins_seen") && saves.value("coins_seen")->as_number() == 4.0);
        // The coin was made after the save was written: not in it.
        int coins = 0;
        world.raw().each([&](flecs::entity, const kin::SpawnedPrefab&) { ++coins; });
        assert(coins == 0);
    }
    fs::remove_all(dir);
}

// camera.shake adds trauma to the current camera; camera.trauma() says what is left.
void test_camera_shake() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_scripts_camera_test";
    fs::remove_all(dir);
    write(dir / "scripts/thud.lua", R"lua(
function on_update(self, dt)
    if not self.done then
        self.done = true
        camera.shake(0.3)
        camera.shake(0.2)
        self:set("Transform2D", {pos = {x = camera.trauma() * 100, y = 0}})
    end
end
)lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::EcsEntity cam = world.entity("Camera").set(kin::Transform2D{}).set(kin::CameraComponent{});
    kin::EcsEntity hero = world.entity("Hero").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "scripts/thud.lua"}});
    kin::Input input;
    kin::EntityScripts scripts{{.content = dir, .check_frames = 0}};
    scripts.update(world, input, 0.1f);
    assert(scripts.problems().empty());
    assert(near(cam.get<kin::CameraComponent>()->trauma, 0.5f) && near(hero.get<kin::Transform2D>()->pos.x, 50.0f));
    fs::remove_all(dir);
}

} // namespace

// A script's tweens (their done callbacks; one stopped never says it is
// done) and timers (every, after, cancel); mistakes said. Tweens advance with
// the world's script services; timers count the steps after the one that
// starts them and fire once a step at most.
void test_tweens_and_timers() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_tween_test";
    fs::remove_all(dir);
    write(dir / "scripts/tweener.lua", R"lua(
function on_start(self)
    self.ticks = 0
    self:tween("Transform2D.pos", {x = 100, y = 0}, 0.5, "linear", {done = function(self) self.moved = true end})
    self:tween("RectRenderer.color", {r = 0, g = 0, b = 0, a = 0}, 0.2, {delay = 0.2})
    self.tick = self:every(0.1, function(self) self.ticks = self.ticks + 1 end)
    self:after(0.3, function(self)
        assert(self:cancel(self.tick))
        self.stopped_at = self.ticks
    end)
    assert(self:tweening() and self:tweening("Transform2D.pos") and not self:tweening("Transform2D.rotation"))
    local ok, err = pcall(function() self:tween("Transform2D.pos", 3, 1) end)
    assert(not ok and tostring(err):find("Transform2D.pos takes"), tostring(err))
    ok, err = pcall(function() self:tween("Transform2D.pos", {x = 1, y = 1}, 1, "wobbly") end)
    assert(not ok and tostring(err):find("wobbly"), tostring(err))
    ok, err = pcall(function() self:tween("Transform2D.spin", 1, 1) end)
    assert(not ok and tostring(err):find("spin"), tostring(err))
end

function on_update(self, dt)
    self.frames = (self.frames or 0) + 1
    if self.frames == 7 then
        assert(self.moved, "its done ran")
        assert(self.ticks == 3 and self.stopped_at == 3, "ticks: " .. self.ticks)
        assert(not self:tweening())
        local x = self:position()
        assert(x == 100, "moved to " .. x)
        assert(self:get("RectRenderer").color.a == 0, "faded")
        self.never = false
        local id = self:tween("Transform2D.rotation", 90, 1, {done = function(self) self.never = true end})
        assert(id > 0 and self:stop_tweens("Transform2D.rotation") == 1)
    end
    if self.frames == 9 then
        assert(not self.never, "a stopped tween is not done")
        self:set("RectRenderer", {size = {x = 9, y = 9}})
    end
end
)lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    const kin::EcsEntity box = world.entity("Box").set(kin::Transform2D{}).set(kin::RectRenderer{}).set(kin::ScriptComponent{.asset = {.id = "scripts/tweener.lua"}});
    kin::Input input;
    kin::EntityScripts scripts{{.content = dir}};
    for (int frame = 0; frame < 9; ++frame) {
        scripts.update(world, input, 0.1f);
        for (const auto& [path, problem] : scripts.problems()) {
            std::fprintf(stderr, "frame %d: %s: %s\n", frame + 1, path.c_str(), problem.c_str());
        }
        assert(scripts.problems().empty());
    }
    assert(box.get<kin::RectRenderer>()->size == (kin::Vec2f{9.0f, 9.0f}));
    fs::remove_all(dir);
}

#if KIN_ENABLE_PHYSICS
// A character's script: it reads what its CharacterController2D found, sets
// its velocity, asks the physics what is where (rays, casts, overlaps; masks,
// sensors, itself left out), hears contacts begin and end, and is put back
// (its body with it).
void test_physics_scripts() {
    const fs::path dir = fs::temp_directory_path() / "kin_entity_physics_test";
    fs::remove_all(dir);
    write(dir / "scripts/hero.lua", R"lua(
function on_update(self, dt)
    self.frames = (self.frames or 0) + 1
    if self.frames == 40 then
        assert(self:on_floor(), "landed")
        assert(not self:on_wall() and not self:on_ceiling())
        local x, y = self:position()
        local hit = physics.raycast(x, y, x, 500, {ignore = self})
        assert(hit and hit.entity:name() == "Floor", "the ray hits the floor")
        assert(math.abs(hit.y - 100) < 0.5 and math.abs(hit.normal.y + 1) < 0.01 and hit.fraction > 0)
        assert(physics.raycast(x, y - 100, x, 500).entity:name() == "Hero", "itself, not left out")
        assert(physics.raycast(x, y, x, -500, {ignore = self}) == nil)
        local cast = physics.cast_box(-200, 0, -200, 300, 20, 20)
        assert(cast and cast.entity:name() == "Floor" and math.abs(cast.y - 100) < 0.5, "a box cast")
        assert(physics.cast_circle(-200, 0, -200, 50, 5) == nil)
        assert(#physics.overlap_point(60, 84) == 0, "sensors left out")
        local found = physics.overlap_point(60, 84, {sensors = true})
        assert(#found == 1 and found[1]:name() == "Coin", "a sensor asked for")
        local under = physics.overlap_box(0, 110, 100, 10)
        assert(#under == 1 and under[1]:name() == "Floor")
        assert(#physics.overlap_circle(0, 110, 5, {mask = 2}) == 0, "masked out")
        self:set_velocity(300, 0)
        local vx, vy = self:velocity()
        assert(vx == 300 and vy == 0)
    end
    if self.frames == 80 then
        assert(self.touched == "Coin", "it touched the coin")
        assert(self.left == "Coin", "and left it")
        assert(self:on_floor())
        self:set_position(0, 0)
    end
    if self.frames == 81 then
        local x, y = self:position()
        assert(x < 6 and y < 10, "put back, its body too: " .. x .. ", " .. y)
        self:set("RectRenderer", {size = {x = 3, y = 3}})
    end
end

function on_contact(self, other)
    if other:name() == "Coin" then self.touched = "Coin" end
end

function on_contact_end(self, other)
    if other:name() == "Coin" then self.left = "Coin" end
end
)lua");

    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    kin::PhysicsWorld physics{{.gravity = {0.0f, 9.8f * 32.0f}, .units_per_meter = 32.0f}};
    kin::EcsPhysicsBridge bridge{world, physics};
    world.entity("Floor")
        .set(kin::Transform2D{.pos = {0.0f, 116.0f}})
        .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
        .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({400.0f, 16.0f})}}}});
    world.entity("Coin")
        .set(kin::Transform2D{.pos = {60.0f, 84.0f}})
        .set(kin::PhysicsBodyComponent{.def = {.type = kin::PhysicsBodyType::Static}})
        .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::circle(6.0f), .sensor = true}}}});
    const kin::EcsEntity hero = world.entity("Hero")
                                    .set(kin::Transform2D{})
                                    .set(kin::RectRenderer{})
                                    .set(kin::PhysicsBodyComponent{})
                                    .set(kin::PhysicsColliderComponent{.colliders = {{.def = {.shape = kin::PhysicsShape::box({8.0f, 16.0f})}}}})
                                    .set(kin::PhysicsContactQueue{})
                                    .set(kin::CharacterController2D{.gravity = 2000.0f})
                                    .set(kin::ScriptComponent{.asset = {.id = "scripts/hero.lua"}});
    kin::Input input;
    kin::EntityScripts scripts{{.content = dir}};
    scripts.set_physics(&physics);
    for (int frame = 0; frame < 81; ++frame) {
        kin::update_physics_world(world, physics, 1.0f / 60.0f);
        scripts.update(world, input, 1.0f / 60.0f);
        for (const auto& [path, problem] : scripts.problems()) {
            std::fprintf(stderr, "frame %d: %s: %s\n", frame + 1, path.c_str(), problem.c_str());
        }
        assert(scripts.problems().empty());
    }
    assert(hero.get<kin::RectRenderer>()->size == (kin::Vec2f{3.0f, 3.0f}));
    fs::remove_all(dir);
}
#endif

int main() {
    test_entity_scripts();
    test_player_scripts();
    test_entity_emit();
    test_scene_go();
    test_saves();
    test_camera_shake();
    test_tweens_and_timers();
#if KIN_ENABLE_PHYSICS
    test_physics_scripts();
#endif
    return 0;
}
