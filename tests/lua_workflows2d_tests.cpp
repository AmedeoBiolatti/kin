#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <kin/assets/content.hpp>
#include <kin/ecs/render.hpp>
#include <kin/ecs/script.hpp>
#include <kin/platform/app.hpp>
#include <kin/platform/user_data.hpp>
#include <kin/scene/scene_preload2d.hpp>
#include <kin/scripting/entity_scripts.hpp>
#include <kin/scripting/script_engine.hpp>
#include <kin/scripting/workflows2d.hpp>
#include <sol/sol.hpp>

using namespace kin;
namespace {
const char* prefab = R"({"schema":"kin.prefab/1","id":"coin","root":"root","entities":[
    {"id":"root","name":"Coin","components":{"Transform2D":{},"RectRenderer":{"size":{"x":2,"y":2}}}}]})";
void scene_host(Renderer2D& renderer) {
    const auto content = MemoryContent::create(
        {{"coin.kinprefab", prefab},
         {"room.kinscene",
          R"({"schema":"kin.scene/1","name":"Next room","entities":[{"id":"door","components":{"Transform2D":{}}}]})"},
         {"game.lua", R"(
        function on_load(scene)
            input_contexts.push('game', 'key fire Space\nkey other Enter\n')
            pools.create('coins','coin.kinprefab',1)
            pools.create('coins','coin.kinprefab',1) -- reload-safe declaration
            old=pools.acquire('coins',2,3)
            assert(old and old:alive() and not pools.acquire('coins'))
            old_entity=old:entity()
            tween=tweens.to(old_entity,'Transform2D.pos',{x=12,y=3},1,'linear')
            chained=tweens.to('Aux','Transform2D.pos',{x=10,y=20},1):parallel():to('RectRenderer.size',{x=4,y=4},1):wait(.1)
            collectgarbage('collect')
            tweens.to('Inherited','Transform2D.pos',{x=8,y=0},.2)
        end
        local frame=0
        function update(ctx)
            frame=frame+1
            if frame==1 then
                rebind.begin('fire',2)
                request=preload.scene('room.kinscene')
                assert(#controllers.devices()==0 and not controllers.state(1).connected)
                assert(not controllers.rumble(1,1,1,30))
                assert(not pcall(input_contexts.save,'game','../escape'))
            elseif frame==2 then
                assert(rebind.state()=='captured' and rebind.candidate()=='key:Enter')
                assert(#rebind.conflicts('game')==1)
                assert(not pcall(rebind.apply,'game','reject'))
                rebind.apply('game','replace')
                local diff=input_contexts.overrides('game')
                input_contexts.apply_overrides('game',diff)
                assert(input_contexts.save('game','workflow-test'))
                input_contexts.load('game','workflow-test')
                assert(old:release() and not old:alive())
                fresh=pools.acquire('coins',40,50)
                assert(fresh:alive() and not old:release())
                assert(not old_entity:alive())
                assert(not pcall(tweens.to,old_entity,'Transform2D.pos',{x=1,y=1},1))
                tweens.to(fresh:entity(),'Transform2D.pos',{x=42,y=50},.2)
            end
            if request:state()=='ready' then
                assert(request:progress()==1 and request:commit() and not request:commit())
            end
            if frame==5 then
                assert(tween:cancelled() and chained:cancelled() and fresh:alive() and pools.active('coins')==1)
                asset_memory.budget(1024,1024)
                local report=asset_memory.report()
                assert(report.cpu.budget_bytes==1024 and report.gpu.budget_bytes==1024)
                assert(type(asset_memory.residency().cpu)=='table')
                asset_memory.trim()
                fresh:release()
                do local orphan=pools.acquire('coins') end
                collectgarbage('collect')
            elseif frame==6 then
                assert(pools.active('coins')==0)
                verified()
            end
        end
        )"}});
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto auxiliary = world.entities().create({.name = "Aux"});
    auxiliary.set(Transform2D{});
    auxiliary.set(RectRenderer{});
    world.raw().component<Transform2D>().add(flecs::OnInstantiate, flecs::Inherit);
    auto base = world.raw().prefab("Base").set(Transform2D{});
    auto inherited = world.entities().create({.name = "Inherited"});
    inherited.raw().is_a(base);
    auto sibling = world.entities().create({.name = "Sibling"});
    sibling.raw().is_a(base);
    JobSystem jobs{{.single_threaded = true}};
    Assets2D borrowed_assets{renderer, content->root()};
    auto services = std::make_shared<ScriptWorkflows2D>(world, content->root());
    services->configure(&renderer, nullptr, nullptr, &jobs);
    bool verified = false;
    ScriptEngine script{[&](sol::state& lua) { lua["verified"] = [&] { verified = true; }; }};
    assert(script.load_file(content->root(), "game.lua"));
    if (!script.call_on_load({.world = &world, .workflows = services})) {
        std::fprintf(stderr, "%s\n", script.last_error().c_str());
        std::abort();
    }
    Input input;
    if (!script.call_update({.dt = 0, .input = &input})) {
        std::fprintf(stderr, "%s\n", script.last_error().c_str());
        std::abort();
    }
    auxiliary.remove<Transform2D>(); // all parallel-step components must be guarded
    input.begin_frame();
    input.set_key_pressed(Key::Enter);
    if (!script.call_update({.dt = .5f, .input = &input})) {
        std::fprintf(stderr, "%s\n", script.last_error().c_str());
        std::abort();
    }
    for (int i = 0; i < 4; ++i) {
        input.begin_frame();
        if (!script.call_update({.dt = .1f, .input = &input})) {
            std::fprintf(stderr, "%s\n", script.last_error().c_str());
            std::abort();
        }
    }
    assert(verified);
    services->configure(&renderer, nullptr, &borrowed_assets, &jobs);
    assert(borrowed_assets.manager().memory_budget() == 1024 && borrowed_assets.texture_budget() == 1024);
    assert(inherited.get<Transform2D>()->pos.x == 8 && sibling.get<Transform2D>()->pos.x == 0 &&
           base.get<Transform2D>()->pos.x == 0);
    auto next = services->take_scene();
    assert(next && next->name() == "Next room" && !services->take_scene());
}
void entity_host() {
    auto content = MemoryContent::create({{"coin.kinprefab", prefab}, {"entity.lua", R"(
        function on_start(self)
            input_contexts.push('entity','key jump Space\n')
            pools.create('coins','coin.kinprefab',1)
            self.lease=pools.acquire('coins')
            local root=self.lease:entity()
            assert(root:alive())
            self.tween=tweens.to(self,'Transform2D.pos',{x=10,y=20},1)
        end
        function on_update(self,dt)
            if self:get('Transform2D').pos.x>=10 then
                assert(self.tween:finished() and self.lease:release())
                self:set('ScriptComponent',{enabled=false})
            end
        end
        )"}});
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    auto entity = world.entities().create({.name = "Player"});
    entity.set(Transform2D{});
    entity.set(ScriptComponent{.asset = {"entity.lua"}});
    EntityScripts scripts{{.content = content->root(), .check_frames = 0}};
    Input input;
    scripts.update(world, input, 0);
    scripts.update(world, input, .5f);
    assert(std::abs(entity.get<Transform2D>()->pos.x - 5) < .001f);
    scripts.update(world, input, .5f);
    assert(scripts.problems().empty() && !entity.get<ScriptComponent>()->enabled);
}
} // namespace
int main() {
    auto profile =
        std::filesystem::temp_directory_path() /
        ("kin-workflow-lua-profile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    while (!std::filesystem::create_directory(profile))
        profile += "-next";
    set_user_data_dir_override(profile);
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 16, .height = 16, .hidden = true});
    Renderer2D renderer{window};
    scene_host(renderer);
    entity_host();
    std::filesystem::remove_all(profile);
    clear_user_data_dir_override();
}
