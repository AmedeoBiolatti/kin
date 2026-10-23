#include "skin_fixtures.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <kin/core/json.hpp>
#include <kin/ecs/system.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <sol/sol.hpp>
#include <sstream>
#include <stdexcept>

namespace {
using namespace kin;
void check(const auto& value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-lua-advanced-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream(path) << text;
}
void ok(SceneSession3D& session, const auto& value) {
    if (!value) {
        std::string error = "advanced Lua scene failed";
        for (const auto& message : session.diagnostics())
            error += '\n' + message;
        throw std::runtime_error(error);
    }
}
SceneSession3DOptions options() {
    SceneSession3DOptions out;
    out.viewport = {160, 120};
    out.warm_up = false;
    out.view.samples = 1;
    out.physics.emplace();
    out.physics->single_threaded = true;
    out.physics->gravity = {};
    out.physics->max_bodies = 128;
    out.physics->max_body_pairs = 512;
    out.physics->max_contact_constraints = 512;
    out.max_runtime_prefabs = 16;
    out.max_runtime_entities = 64;
    return out;
}
void report(SceneSession3D& session) {
    std::ostringstream output;
    JsonWriter json{output, false};
    json.begin_object();
    ok(session, session.write_report(json));
    json.end_object();
}
void resources(Renderer3D& rendering, bool physics = true) {
    Scratch source;
    write(source.path / "game.kinscene", R"({"script":"game.lua","entities":[
      {"id":"a","components":{"Transform3D":{},"Collider3D":{}}},
      {"id":"b","components":{"Transform3D":{"position":[0,0,-2]},"Collider3D":{}}},
      {"id":"sensor","components":{"Transform3D":{"position":[0,0,-4]},"Collider3D":{},"RigidBody3D":{"motion":"static","sensor":true}}},
      {"id":"actor","components":{"Transform3D":{"position":[-5,0,0]},"Animator3D":{"clip":"Idle","fade_seconds":0}}}
    ]})");
    write(source.path / "prop.kinprefab", R"({"schema":"kin.prefab/1","id":"prop","root":"root","entities":[
      {"id":"root","components":{"Transform3D":{},"Collider3D":{}}}]})");
    write(source.path / "game.lua", R"lua(
local prepare, prepared, pool_request, pool, first, old_root, second, direct, retired
local release_pool, release_prepared, refused, exhausted, bad_pool, again, missing
local actor, ticks = nil, 0
local function v(x,y,z) return {x=x,y=y,z=z} end
local sphere = scene3d.shape{kind='sphere',radius=.5}
function on_load(world)
    assert(not pcall(scene3d.preload,'prop.kinprefab'))
    assert(not pcall(scene3d.shape,{kind='sphere',radius=-1}))
    assert(not pcall(scene3d.shape,{kind='box',half_extents=v(0,1,1)}))
    assert(not pcall(scene3d.shape,{kind='mesh'}))
    assert(not pcall(scene3d.shape,{kind='convex_hull',points={v(0,0,0)}}))
end
function on_enter(ctx)
    actor=scene3d.entity('actor')
    assert(not pcall(function() scene3d.resolve(actor):patch('SkinnedMeshRenderer',{visible=false}) end))
    prepare=scene3d.preload('prop.kinprefab')
    assert(prepare.status=='pending' and prepare.prepared==nil and prepare.pool==nil)
    if not scene3d.physics_available() then return end
    local hits,overflow=scene3d.raycast_all(v(0,0,3),v(0,0,-6),{capacity=1})
    assert(#hits==1 and overflow and hits[1].entity.id==scene3d.entity('a').id)
    hits,overflow=scene3d.raycast_all(v(0,0,3),v(0,0,-6),{capacity=8,sensors=true})
    assert(#hits==3 and not overflow)
    assert(#scene3d.raycast_all(v(0,0,3),v(0,0,-6),{mask=0})==0)
    local hit=scene3d.shape_cast(sphere,v(0,0,3),v(0,0,-6))
    assert(hit and hit.entity.id==scene3d.entity('a').id and hit.normal.z>.9 and hit.shape_position.z>.9)
    assert(scene3d.shape_cast(sphere,v(.7,0,0),v(1,0,0))==nil)
    hit=scene3d.shape_cast(sphere,v(.7,0,0),v(1,0,0),{initial_overlap=true})
    assert(hit and hit.fraction==0 and hit.penetration>0)
    local big=scene3d.shape{kind='box',half_extents=v(1,1,2)}
    hits,overflow=scene3d.overlap(big,v(0,0,-1),{capacity=1})
    assert(#hits==1 and overflow and hits[1].penetration>0)
    hits,overflow=scene3d.overlap(big,v(0,0,-1),{ignore=scene3d.entity('a')})
    assert(#hits==1 and not overflow and hits[1].entity.id==scene3d.entity('b').id)
    assert(not pcall(scene3d.overlap,big,v(0,0,0),{capacity=1.5}))
    assert(not pcall(scene3d.raycast_all,v(0,0,3),v(0,0,-6),{capacity=257}))
    assert(not pcall(scene3d.shape_cast,sphere,v(0,0,3),v(0,0,-6),{rotation={x=0,y=0,z=0,w=2}}))
    assert(not pcall(scene3d.overlap,big,v(0/0,0,0)))
    for _,kind in ipairs{'capsule','cylinder'} do assert(scene3d.shape{kind=kind,radius=.2,half_height=.5}) end
    assert(scene3d.shape{kind='convex_hull',points={v(0,0,0),v(1,0,0),v(0,1,0),v(0,0,1)}})
end
function update(ctx)
    ticks=ticks+1
    if ticks==1 then assert(prepare.status=='pending')
    elseif ticks==2 then
        assert(prepare.status=='succeeded',prepare.error);prepared=prepare.prepared
        pool_request=scene3d.create_pool(prepared,{capacity=1})
        bad_pool=scene3d.create_pool(prepared,{capacity=10000})
        direct=scene3d.spawn(prepared,{position=v(8,0,0)})
        assert(not pcall(scene3d.create_pool,prepared,{capacity=0}))
        assert(not pcall(scene3d.create_pool,prepared,{capacity=1.5}))
        assert(not pcall(scene3d.preload,'../outside.kinprefab'))
    elseif ticks==3 then
        assert(pool_request.status=='succeeded',pool_request.error);pool=pool_request.pool
        assert(bad_pool.status=='failed' and direct.status=='succeeded')
        assert(scene3d.pool_stats(pool).available==1)
        first=scene3d.spawn(pool,{position=v(4,1,0)})
        exhausted=scene3d.spawn(pool)
    elseif ticks==4 then
        assert(first.status=='succeeded' and exhausted.status=='failed')
        old_root=first.prefab.root
        local stats=scene3d.pool_stats(pool);assert(stats.active==1 and stats.exhausted==1)
        stats.active=999;assert(scene3d.pool_stats(pool).active==1)
        assert(scene3d.set_ik(actor,'hand',{target=old_root,offset=v(0,.25,0),weight=1,fade_seconds=0,
            pole_direction=v(0,0,1),target_rotation={x=0,y=0,z=0,w=1},match_rotation=true,rotation_weight=.5}))
    elseif ticks==5 then
        assert(scene3d.ik_state(actor,'hand').target_valid)
        retired=scene3d.despawn(first)
        refused=scene3d.release(prepared)
    elseif ticks==6 then
        assert(retired.status=='succeeded' and refused.status=='failed' and not old_root:alive())
        assert(not scene3d.ik_state(actor,'hand').target_valid)
        assert(not pcall(scene3d.overlap,sphere,v(0,0,0),{ignore=old_root}))
        second=scene3d.spawn(pool,{position=v(4,2,0)})
    elseif ticks==7 then
        assert(second.status=='succeeded' and second.prefab.root.id==old_root.id and second.prefab.root:alive())
        assert(not scene3d.ik_state(actor,'hand').target_valid) -- same ECS entity, different lease
        assert(not pcall(scene3d.set_ik,actor,'hand',{target=old_root}))
        assert(not pcall(scene3d.set_ik,actor,'hand',{target=v(0/0,0,0),weight=0}))
        assert(scene3d.ik_state(actor,'hand').weight==1)
        assert(not scene3d.set_ik(actor,'missing',{target=v(0,0,0)}))
        assert(scene3d.set_ik(actor,'hand',{target=second.prefab.root}))
        assert(scene3d.pool_stats(pool).reused==1)
        release_pool=scene3d.release(pool)
        release_prepared=scene3d.release(prepared)
        again=scene3d.spawn(pool)
    elseif ticks==8 then
        assert(release_pool.status=='succeeded',release_pool.error)
        assert(release_prepared.status=='succeeded',release_prepared.error)
        assert(again.status=='failed' and scene3d.pool_stats(pool)==nil and not second.prefab.root:alive())
        assert(not scene3d.ik_state(actor,'hand').target_valid)
        assert(scene3d.set_ik(actor,'hand',{target=v(-4,1,0),pole_position=v(-5,0,1),weight=.8}))
        assert(scene3d.set_ik(actor,'hand',{target=false}))
        scene3d.despawn(direct)
        missing=scene3d.preload('missing.kinprefab')
        remember(prepared,pool)
    elseif ticks==9 then assert(missing.status=='failed' and #missing.error>0)
    end
end
function write_report()
    assert(not pcall(scene3d.preload,'prop.kinprefab'))
    assert(not pcall(scene3d.set_ik,actor,'hand',{weight=0}))
    return {ticks=ticks}
end
)lua");
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(skin_fixtures::make_tube().skeleton);
    AnimClip3D idle;
    idle.name = "Idle";
    idle.duration = 1;
    model->clips.push_back(idle);
    auto config = options();
    if (!physics)
        config.physics.reset();
    config.initialize = [model](SceneSession3DContext& ctx) {
        const auto found = ctx.instance.by_authored_id.find("actor");
        if (found == ctx.instance.by_authored_id.end())
            return;
        auto actor = found->second;
        actor.set(SkinnedMeshRenderer3D{.model = model});
        actor.get_mut<Animator3D>()->add_ik(
            {.name = "hand", .kind = IKKind3D::TwoBone, .joints = {"base", "middle", "top"}});
    };
    ScenePreparedPrefab3D previous;
    ScenePrefabPool3D previous_pool;
    SceneRequest3D cancelled;
    config.scripts.bind = [&](sol::state& lua) {
        lua["remember"] = [&](ScenePreparedPrefab3D a, ScenePrefabPool3D b) {
            previous = a;
            previous_pool = b;
        };
        lua["old_prepared"] = [&] { return previous; };
        lua["old_pool"] = [&] { return previous_pool; };
        lua["capture"] = [&](SceneRequest3D request) { cancelled = request; };
    };
    Assets3D assets{rendering, source.path};
    SceneSession3D session{rendering, assets, config};
    ok(session, session.load("game.kinscene"));
    for (int i = 0; i < 9; ++i) {
        ok(session, session.step(1.f / 60));
        session.rendering()->apply_ik();
        if (i == 3) {
            const auto* animator = session.entity("actor").get<Animator3D>();
            check(animator->ik_world.size() == 3 && animator->ik_world.back().m[12] > 1,
                  "Lua IK did not change the solved pose");
            check(std::fabs(animator->find_ik("hand")->last_target.y - 1.25f) < .001f,
                  "Lua IK entity offset was not applied");
        }
    }
    check(session.runtime_prefabs() == 0 && session.reserved_entities() == 0,
          "Lua resource release leaked a prefab/reference");
    report(session);
    EcsCommandBuffer commands;
    std::string error;
    auto pending = session.commands().preload(commands, "prop.kinprefab");
    check(commands.flush(*session.world(), error), "resource request flush failed");
    write(source.path / "pending.kinscene", R"({"entities":[]})");
    ok(session, session.prepare("pending.kinscene"));
    ok(session, session.step(.01f));
    check(pending.status() == SceneRequestStatus3D::Cancelled,
          "prepared replacement did not cancel resource preparation");
    assert(session.cancel());
    write(source.path / "failed.lua",
          "function on_enter(c) capture(scene3d.preload('prop.kinprefab')); error('reject') end");
    write(source.path / "failed.kinscene", R"({"script":"failed.lua","entities":[]})");
    auto* world = session.world();
    check(!session.load("failed.kinscene") && session.world() == world, "resource callback failure replaced scene");
    check(cancelled.status() == SceneRequestStatus3D::Cancelled, "failed enter retained executable resource work");
    // Old handles cannot address the fresh scene, even when resource counters happen to agree.
    write(source.path / "next.lua", R"lua(
local a,b
function on_enter(ctx) a=scene3d.spawn(old_prepared());b=scene3d.spawn(old_pool()) end
function update(ctx) if a.status~='pending' then assert(a.status=='failed' and b.status=='failed') end end
)lua");
    write(source.path / "next.kinscene", R"({"script":"next.lua","entities":[]})");
    config.initialize = {};
    // The initialized actor is required only by the first fixture.
    SceneSession3D next{rendering, assets, config};
    ok(next, next.load("next.kinscene"));
    ok(next, next.step(.01f));
    ok(next, next.step(.01f));
}
void controls(Renderer3D& rendering) {
    Scratch source;
    write(source.path / "game.kinscene", R"({"script":"game.lua","entities":[
      {"id":"floor","components":{"Transform3D":{"position":[0,-0.5,0]},"Collider3D":{"half_extents":[30,0.5,30]}}},
      {"id":"wall","components":{"Transform3D":{"position":[0,1.5,-0.65]},"Collider3D":{"half_extents":[2,1.5,0.1]}}},
      {"id":"climber","components":{"Transform3D":{"position":[0,0.05,0]},"CharacterController3D":{"radius":0.2,"height":1.8}}},
      {"id":"hero","components":{"Transform3D":{"position":[4,0.05,0]},"Animator3D":{"clip":"Idle","fade_seconds":0},"CharacterController3D":{}}},
      {"id":"rope","components":{"Transform3D":{"position":[8,4,0]}}},
      {"id":"fragile","components":{"Transform3D":{"position":[12,4,0]}}}
    ]})");
    write(source.path / "game.lua", R"lua(
local climber,hero,rope,fragile
local ticks,grips,releases,breaks,blocked,recovered=0,0,0,0,0,0
local function v(x,y,z) return {x=x,y=y,z=z} end
function on_load(world)
    assert(world:register_system('lua.advanced-events',{phase='post_physics',all={'Animator3D'},
        reads={'Ragdoll3D','Rope3D','CharacterController3D'}, writes={'Ragdoll3D','CharacterController3D'}},function(e,dt,ctx)
        if ticks==1 then assert(scene3d.recover_from_ragdoll(hero,{mask=0}).blocked) end
        for _,event in ipairs(scene3d.character_events(climber)) do
            if event.kind=='gripped' then grips=grips+1;assert(event.other.id==scene3d.entity('wall').id) end
            if event.kind=='grip_released' then releases=releases+1 end
        end
        local events=scene3d.ragdoll_events(hero)
        assert(#events==#scene3d.ragdoll_events(hero))
        for _,event in ipairs(events) do
            assert(event.entity.id==hero.id)
            if event.kind=='blocked' then blocked=blocked+1 end
            if event.kind=='recovered' then recovered=recovered+1 end
        end
        for _,event in ipairs(scene3d.rope_events(fragile)) do
            assert(event.kind=='broke' and event.entity.id==fragile.id and event.force>0)
            breaks=breaks+1
        end
    end))
end
function on_enter(ctx)
    climber=scene3d.entity('climber');hero=scene3d.entity('hero');rope=scene3d.entity('rope');fragile=scene3d.entity('fragile')
    assert(scene3d.traversal_state(climber).enabled)
    assert(scene3d.traversal_input(climber,{grab={true,true},reach=v(0,0,-1)}))
    assert(not scene3d.traversal_input(hero,{}))
    assert(not pcall(scene3d.traversal_input,climber,{grab={[5]=true}}))
    assert(not pcall(scene3d.traversal_input,climber,{reach=v(0/0,0,0)}))
    assert(scene3d.set_traversal_resource(climber,80))
    assert(not pcall(scene3d.set_traversal_resource,climber,-1))
    assert(scene3d.ragdoll_state(hero).mode=='animated')
    assert(not pcall(scene3d.start_ragdoll,hero,{impulse_bone=17}))
    assert(not pcall(scene3d.hand_over_to_ragdoll,hero,{impulse=v(1,0,0)}))
    assert(scene3d.ragdoll_state(hero).mode=='animated')
    assert(scene3d.start_ragdoll(hero,{velocity=v(0,0,0),blend_seconds=0}))
    assert(scene3d.ragdoll_state(hero).dynamic_bodies==17)
    assert(scene3d.blend_ragdoll(hero,.5,0))
    assert(scene3d.set_ragdoll_subtree(hero,'shoulder_L',0))
    assert(not scene3d.set_ragdoll_subtree(hero,'absent',1))
    assert(scene3d.animate_ragdoll(hero))
    assert(scene3d.hand_over_to_ragdoll(hero,{blend_seconds=0,inherit_velocity=false}))
    assert(not pcall(scene3d.blend_ragdoll,hero,2,0))
    local refusal=scene3d.recover_from_ragdoll(hero,{mask=0})
    assert(refusal.blocked and not refusal.recovered and not refusal.place.found)
    local state=scene3d.rope_state(rope);assert(state.segments==8 and not state.broken)
    local points=scene3d.rope_points(rope);assert(#points==9 and points[1].position.y>3.9)
    points[1].position.y=999;assert(scene3d.rope_points(rope)[1].position.y~=999)
    local closest=scene3d.rope_closest_point(rope,v(8.1,3,0));assert(closest and closest.segment>=1 and closest.distance<.2)
    local point=scene3d.rope_point_at(rope,closest.segment,closest.t)
    assert(math.abs(point.position.y-closest.position.y)<.001)
    assert(not pcall(scene3d.rope_point_at,rope,9,.5))
    assert(not pcall(scene3d.reel_rope,rope,2,-1))
    assert(scene3d.set_rope_length(rope,3))
end
function update(ctx)
    ticks=ticks+1
    if ticks==2 then
        assert(math.abs(scene3d.rope_state(rope).rest_length-3)<.001)
        assert(scene3d.reel_rope(rope,2,.5))
    elseif ticks==3 then
        local length=scene3d.rope_state(rope).rest_length;assert(length<3 and length>2.9)
    elseif ticks==10 then
        local state=scene3d.traversal_state(climber)
        assert(state.mode=='hanging' and state.held==2 and #state.grips==2 and state.resource.value<80, state.mode..' held='..state.held..' resource='..state.resource.value..' phases='..state.grips[1].phase..','..state.grips[2].phase)
        assert(state.grips[1].entity.id==scene3d.entity('wall').id)
        state.resource.value=999;assert(scene3d.traversal_state(climber).resource.value~=999)
        assert(scene3d.traversal_input(climber,{drop=true}))
        assert(scene3d.set_traversal_resource(climber,20))
    elseif ticks==11 then
        assert(scene3d.traversal_state(climber).held==0)
        assert(scene3d.traversal_input(climber,{}))
        assert(scene3d.ragdoll_get_up(hero,.05))
    elseif ticks==20 then
        assert(scene3d.ragdoll_state(hero).mode=='animated')
        assert(scene3d.hand_over_to_ragdoll(hero,{blend_seconds=0}))
    elseif ticks==21 then
        local result=scene3d.recover_from_ragdoll(hero,{get_up_seconds=.05,mask=0xffffffff})
        assert(result.recovered and not result.blocked and result.place.found,'recovery found='..tostring(result.place.found)..' blocked='..tostring(result.blocked)..' y='..result.place.position.y..' height='..result.place.height)
    elseif ticks==30 then
        assert(scene3d.ragdoll_state(hero).mode=='animated')
        assert(grips>=2 and releases>=2 and blocked>0 and recovered>0 and breaks>0)
        assert(scene3d.rope_state(fragile).broken)
    end
end
function write_report()
    assert(not pcall(scene3d.traversal_input,climber,{}))
    assert(not pcall(scene3d.animate_ragdoll,hero))
    assert(not pcall(scene3d.set_rope_length,rope,2))
    return {steps=ticks,grips=grips,releases=releases,breaks=breaks,blocked=blocked,recovered=recovered}
end
)lua");
    auto model = std::make_shared<Model3D>();
    model->skeletons.push_back(skin_fixtures::make_biped());
    AnimClip3D idle;
    idle.name = "Idle";
    idle.duration = 1;
    idle.channels.resize(model->skeletons[0].joint_count());
    model->clips.push_back(idle);
    auto config = options();
    config.physics->gravity = {0, -9.81f, 0};
    config.initialize = [model](SceneSession3DContext& ctx) {
        auto climber = ctx.instance.by_authored_id.at("climber");
        climber.get_mut<CharacterController3D>()->desc.traversal.enabled = true;
        climber.get_mut<CharacterController3D>()->desc.traversal.reach_seconds = .05f;
        auto hero = ctx.instance.by_authored_id.at("hero");
        hero.set(SkinnedMeshRenderer3D{.model = model});
        hero.set(Ragdoll3D{});
        Rope3D rope;
        rope.start = RopeAttach3D::at({8, 4, 0});
        rope.end = RopeAttach3D::free({0, -2, 0});
        rope.length = 2;
        rope.segments = 8;
        auto rope_entity = ctx.instance.by_authored_id.at("rope");
        rope_entity.set(rope);
        rope.start = RopeAttach3D::at({12, 4, 0});
        rope.end = RopeAttach3D::at({15, 4, 0});
        rope.length = 3;
        rope.break_tension = .0001f;
        auto fragile = ctx.instance.by_authored_id.at("fragile");
        fragile.set(rope);
    };
    Assets3D assets{rendering, source.path};
    SceneSession3D session{rendering, assets, config};
    ok(session, session.load("game.kinscene"));
    for (int i = 0; i < 30; ++i)
        ok(session, session.step(1.f / 60));
    check(session.physics()->ragdoll_count() == 1 && session.physics()->rope_count() == 2,
          "Lua controls changed native ownership");
    report(session);
    assert(session.unload());
    check(!session.active(), "advanced Lua unload failed");
    write(source.path / "none.kinscene",
          R"({"script":"none.lua","entities":[{"id":"actor","components":{"Transform3D":{}}}]})");
    write(source.path / "none.lua", R"lua(
function on_enter(ctx)
    local e=scene3d.entity('actor')
    assert(scene3d.traversal_state(e)==nil and not scene3d.traversal_input(e,{}))
    assert(scene3d.ragdoll_state(e)==nil and not scene3d.start_ragdoll(e))
    assert(not scene3d.animate_ragdoll(e) and scene3d.recover_from_ragdoll(e)==nil)
    assert(scene3d.rope_state(e)==nil and scene3d.rope_points(e)==nil and not scene3d.set_rope_length(e,2))
    assert(not pcall(scene3d.rope_events) and not pcall(scene3d.ragdoll_events))
    assert(not pcall(scene3d.overlap,scene3d.shape{kind='sphere',radius=.5},{x=0,y=0,z=0}))
end
)lua");
    config.physics.reset();
    config.initialize = {};
    SceneSession3D none{rendering, assets, config};
    ok(none, none.load("none.kinscene"));
}
} // namespace
void test_lua_advanced3d(kin::Renderer2D& renderer) {
    kin::Renderer3D rendering{renderer};
    resources(rendering);
    resources(rendering, false);
    controls(rendering);
}
