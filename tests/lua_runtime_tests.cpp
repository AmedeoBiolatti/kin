#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <kin/ecs/render.hpp>
#include <kin/ecs/script.hpp>
#include <kin/ecs/system.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/entity_scripts.hpp>
#include <kin/scripting/lua_script.hpp>
#include <kin/scripting/script_engine.hpp>
#include <kin/scripting/script_reload.hpp>
#include <kin/scripting/workflows2d.hpp>
namespace {
namespace fs = std::filesystem;
void write(const fs::path& path, const std::string& code) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << code;
}
void limits_and_profiles() {
    kin::LuaScript script{{.limits = {.instruction_limit = 10000}, .on_error = [](std::string_view) {}}};
    assert(script.load_string(R"lua(
        function loop() while true do pcall(function() while true do end end) end end
        function memory() return string.rep('x',1024*1024) end
        function ok() return 1.25 end
    )lua"));
    assert(!script.call("loop"));
    assert(script.error().find("instruction limit") != std::string::npos);
    assert(script.call_for<double>("ok") == 1.25);
    script.runtime().set_limits({10000, script.runtime().stats().memory_bytes + 32 * 1024});
    assert(!script.call("memory"));
    assert(script.call_for<double>("ok") == 1.25);
    auto stats = script.runtime().stats();
    assert(stats.denied_allocations > 0 && stats.peak_memory_bytes > 0 && stats.allocated_bytes > 0);
    bool loop = false;
    for (const auto& p : stats.callbacks)
        if (p.name == "loop") {
            loop = true;
            assert(p.failures == 1 && p.calls == 1 && p.total_ns > 0);
        }
    assert(loop);
    script.runtime().collect(script.lua().lua_state());
    assert(script.runtime().stats().gc_steps == 1);
    assert(!script.call("ok", std::string(1024 * 1024, 'x')));
    script.runtime().collect(script.lua().lua_state(), 1024);
    script.runtime().collect(script.lua().lua_state(), 1024);
    assert(script.call_for<double>("ok") == 1.25);
    kin::LuaScript tiny{{.limits = {.memory_limit_bytes = 128 * 1024}, .on_error = [](std::string_view) {}}};
    assert(!tiny.load_string("local value=string.rep('x',1024*1024)"));
    tiny.runtime().set_limits({5000, 256 * 1024});
    assert(tiny.load_string("function ok() return true end"));
    assert(tiny.runtime().limits().instruction_limit == 5000 &&
           tiny.runtime().limits().memory_limit_bytes == 256 * 1024);
}
void tasks() {
    kin::LuaScript script{{.limits = {.instruction_limit = 10000}, .on_error = [](std::string_view) {}}};
    assert(script.load_string(R"lua(
        stage=0
        handle=task.run(function()
            stage=1; task.wait(0.2); stage=2
            task.frames(2); stage=3
            task.until_(function() return gate end,1); stage=4
        end)
        function probe() return stage end
        function state() return handle:state() end
        function open() gate=true end
        function bad() failed=task.run(function() while true do end end) end
        function bad_state() return failed:state() end
        function spawn_owned() owned=task.run(function() task.wait(1); stage=99 end) end
        function owned_state() return owned:state() end
        function timeout() timer=task.run(function() task.until_(function() return false end,0.1) end) end
        function self_cancel() stopped=task.run(function() stopped:cancel() end) end
        function stopped_state() return stopped:state() end
    )lua"));
    assert(script.call_for<int>("probe") == 0);
    assert(script.advance_tasks(0.1f));
    assert(script.call_for<int>("probe") == 1);
    assert(script.advance_tasks(0.1f));
    assert(script.call_for<int>("probe") == 1);
    assert(script.advance_tasks(0.1f));
    assert(script.call_for<int>("probe") == 2);
    assert(script.advance_tasks(0.1f));
    assert(script.call_for<int>("probe") == 2);
    assert(script.advance_tasks(0.1f));
    assert(script.call_for<int>("probe") == 3);
    assert(script.call("open"));
    assert(script.advance_tasks(0.1f));
    assert(script.call_for<int>("probe") == 4 && script.call_for<std::string>("state") == "completed");
    script.set_task_owner(7);
    assert(script.call("spawn_owned"));
    assert(script.advance_tasks(0.1f, [](kin::u64 owner) { return owner != 7; }));
    assert(script.call_for<std::string>("owned_state") == "cancelled");
    script.set_task_owner(0);
    assert(script.call("bad"));
    assert(!script.advance_tasks(0.1f));
    assert(script.call_for<std::string>("bad_state") == "failed");
    assert(script.call("timeout"));
    assert(script.advance_tasks(0));
    assert(!script.advance_tasks(0.1f));
    assert(script.error().find("timed out") != std::string::npos);
    assert(script.call("self_cancel"));
    assert(script.advance_tasks(0));
    assert(script.call_for<std::string>("stopped_state") == "cancelled");
}
void scene_limits(const fs::path& root) {
    write(root / "scene.lua", "function update() while true do end end");
    kin::ScriptEngine scene({}, {10000, 1024 * 1024});
    assert(scene.load_file(root, "scene.lua"));
    assert(!scene.call_update(kin::ScriptFrameContext{}));
    assert(scene.last_error().find("instruction limit") != std::string::npos);
    write(root / "scene.lua", R"lua(function on_load(scene)
        scene:register_system('endless',{all={'Transform2D'}},function() while true do end end)
    end)lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    world.entity().set(kin::Transform2D{});
    kin::ScriptEngine systems({}, {10000, 1024 * 1024});
    assert(systems.load_file(root, "scene.lua"));
    assert(systems.call_on_load(kin::ScriptSceneBindings{.world = &world}));
    assert(!world.systems().run_frame(0.1f));
    write(root / "memory.lua", "reload_state={blob=string.rep('x',1024*1024)}");
    kin::ScriptEngine memory;
    assert(memory.load_file(root, "memory.lua"));
    assert(memory.call_on_load(kin::ScriptSceneBindings{.world = &world}));
    memory.set_runtime_limits({10000, 512 * 1024});
    write(root / "memory.lua", "function on_reload(saved,scene) reload_state=saved end");
    std::string error;
    assert(!memory.prepare_reload(root, "memory.lua", error));
    assert(error.find("memory limit") != std::string::npos);
}
void scene_await(const fs::path& root) {
    write(root / "await.lua", R"lua(
        function on_load(scene)
            local hero=scene:find('Hero')
            local tween=tweens.to(hero,'Transform2D.pos',{x=10,y=0},0.2)
            task.run(function()
                task.await(tween)
                hero:translate(5,0)
            end)
        end
    )lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    auto hero = world.entity("Hero").set(kin::Transform2D{});
    auto workflows = std::make_shared<kin::ScriptWorkflows2D>(world, root);
    kin::ScriptEngine scene;
    assert(scene.load_file(root, "await.lua"));
    assert(scene.call_on_load(kin::ScriptSceneBindings{.world = &world, .workflows = workflows}));
    assert(scene.call_update(kin::ScriptFrameContext{0.1f}));
    assert(scene.call_update(kin::ScriptFrameContext{0.1f}));
    assert(std::abs(hero.get<kin::Transform2D>()->pos.x - 15) < 0.001);
}
void entity_reload(const fs::path& root) {
    const std::string initial = R"lua(
        function on_start(self)
            self.count=0; self.fraction=1.25; self.large=9007199254740993; self.which=self:name()
            self.other=world.find('Audit')
            self.job=task.run(function() task.wait(100); self:move(100,0) end)
        end
        function on_update(self,dt)
            self.count=self.count+1
            self:set_position(self.count,self.fraction)
        end
        function on_stop(self,reason) self.other:move(1,0) end
    )lua";
    write(root / "entity.lua", initial);
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    auto audit = world.entity("Audit").set(kin::Transform2D{});
    auto a = world.entity("A").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "entity.lua"}});
    auto b = world.entity("B").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "entity.lua"}});
    kin::Input input;
    kin::EntityScripts scripts{{.content = root, .check_frames = 0}};
    scripts.update(world, input, 0.1f);
    assert(a.get<kin::Transform2D>()->pos.x == 1);
    write(root / "entity.lua", initial + R"lua(
        function on_reload(self)
            assert(self.fraction==1.25 and self.large==9007199254740993)
            assert(self.job:state()=='cancelled')
            self.count=self.count+10
        end
        function on_update(self,dt)
            -- A reloaded state's services are the plain tables a fresh one has.
            local fields=0
            for _ in pairs(world) do fields=fields+1 end
            assert(fields>0 and getmetatable(world)==nil, 'world is not a plain table after reload')
            world.custom=true
            self.count=self.count+1
            self:set_position(self.count,self.fraction)
        end
    )lua");
    assert(scripts.reload("entity.lua"));
    scripts.update(world, input, 0.1f);
    assert(scripts.problems().empty());
    assert(a.get<kin::Transform2D>()->pos.x == 12 && b.get<kin::Transform2D>()->pos.x == 12);
    write(root / "entity.lua",
          initial + "function on_reload(self) self.count=500; if self.which=='B' then error('reject') end end");
    assert(!scripts.reload("entity.lua"));
    scripts.update(world, input, 0.1f);
    assert(a.get<kin::Transform2D>()->pos.x == 13 && b.get<kin::Transform2D>()->pos.x == 13);
    write(root / "entity.lua", initial + "function on_reload(self) self.other:move(999,0) end");
    assert(!scripts.reload("entity.lua"));
    assert(audit.get<kin::Transform2D>()->pos.x == 0);
    write(root / "entity.lua", initial + "function on_reload(self) world.find('Audit'):move(999,0) end");
    assert(!scripts.reload("entity.lua"));
    assert(audit.get<kin::Transform2D>()->pos.x == 0);
    write(root / "entity.lua", initial + "function on_reload(self) while true do end end");
    assert(!scripts.reload("entity.lua"));
    a.raw().disable();
    scripts.update(world, input, 0.1f);
    assert(audit.get<kin::Transform2D>()->pos.x == 1 && scripts.running() == 1);
    scripts.restart();
    assert(audit.get<kin::Transform2D>()->pos.x == 2);
}
// A reload drops the old VM's timers and tween callbacks: none runs (into a
// closed state) afterwards, while the tween itself, the entity's, goes on.
void entity_reload_drops_callbacks(const fs::path& root) {
    const std::string initial = R"lua(
        function on_start(self)
            self:every(0.1, function(self) world.find('Audit'):move(1,0) end)
            self:tween('Transform2D.pos',{x=50,y=0},0.3,{done=function(self) world.find('Audit'):move(100,0) end})
        end
    )lua";
    write(root / "callbacks.lua", initial);
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    auto audit = world.entity("Audit").set(kin::Transform2D{});
    auto mover =
        world.entity("Mover").set(kin::Transform2D{}).set(kin::ScriptComponent{.asset = {.id = "callbacks.lua"}});
    kin::Input input;
    kin::EntityScripts scripts{{.content = root, .check_frames = 0}};
    scripts.update(world, input, 0.1f); // on_start
    scripts.update(world, input, 0.1f); // the first tick
    assert(audit.get<kin::Transform2D>()->pos.x == 1);
    write(root / "callbacks.lua", initial + "function on_reload(self) end");
    assert(scripts.reload("callbacks.lua"));
    for (int step = 0; step < 6; ++step) {
        scripts.update(world, input, 0.1f);
    }
    assert(scripts.problems().empty());
    assert(audit.get<kin::Transform2D>()->pos.x == 1);
    assert(std::abs(mover.get<kin::Transform2D>()->pos.x - 50) < 0.001f);
}

// A reload rolled back after the old script's systems were taken leaves the
// old script halted (its calls fail, saying why), not the process ended.
void rollback_halts(const fs::path& root) {
    write(root / "systems.lua", R"lua(
        function on_load(scene) scene:register_system('mover',{all={'Transform2D'}},function() end) end
        function on_reload(saved,scene) scene:register_system('other',{all={'Transform2D'}},function() end) end
    )lua");
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    world.entity().set(kin::Transform2D{});
    auto active = std::make_unique<kin::ScriptEngine>();
    assert(active->load_file(root, "systems.lua"));
    assert(active->call_on_load(kin::ScriptSceneBindings{.world = &world}));
    std::string error;
    auto next = active->prepare_reload(root, "systems.lua", error);
    assert(next && kin::activate_script(active, next, error));
    assert(!world.systems().register_task({.id = "mover"}, [](kin::SystemContext&) {}).empty());
    kin::rollback_script(active, next, true);
    assert(!active->call_update(kin::ScriptFrameContext{0.1f}));
    assert(active->last_error().find("rollback") != std::string::npos);
    assert(!active->activate_systems());
}
} // namespace
int main() {
    const auto root = fs::temp_directory_path() / "kin-lua-runtime-tests";
    fs::remove_all(root);
    fs::create_directories(root);
    limits_and_profiles();
    tasks();
    scene_limits(root);
    scene_await(root);
    entity_reload(root);
    entity_reload_drops_callbacks(root);
    rollback_halts(root);
    fs::remove_all(root);
}
