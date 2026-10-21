#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <kin/assets/content.hpp>
#include <kin/core/json.hpp>
#include <kin/core/json_value.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_session3d.hpp>
#include <sol/sol.hpp>
#include <sstream>
#include <stdexcept>

void test_lua_advanced3d(kin::Renderer2D&);

namespace {
using namespace kin;
struct Stats {
    i32 ticks = 0;
};
void check(const auto& value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Scratch {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("kin-script3d-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream(path, std::ios::binary) << text;
    static int revision = 0;
    std::filesystem::last_write_time(path,
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(++revision));
}
constexpr auto scene = R"({"schema":"kin.scene/1","script":"logic.lua","entities":[
 {"id":"camera","components":{"Transform3D":{"position":[0,0,8]},"SceneCamera3D":{}}},
 {"id":"environment","components":{"SceneEnvironment3D":{"samples":1,"ambient":[1,1,1]}}},
 {"id":"crate","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"parts.glb"},
  "Collider3D":{"half_extents":[0.5,0.5,0.5]},"RigidBody3D":{"mass":1,"linear_damping":0}}}]})";
constexpr auto prefab = R"({"schema":"kin.prefab/1","id":"prop","root":"root","entities":[
 {"id":"root","components":{"Transform3D":{},"ModelRenderer3D":{"asset":"parts.glb"},
 "Collider3D":{"half_extents":[0.5,0.5,0.5]},"RigidBody3D":{"mass":1,"linear_damping":0}}}]})";
constexpr auto logic = R"lua(
local tuning = require("tuning")
local world, crate, spawned, removed, saved_root
local ticks = 0
function on_load(scene)
    world = scene
    assert(not scene3d.available())
    crate = scene:authored("crate")
    crate:patch("TestStats", {ticks=0})
    assert(scene:register_system("lua.post", {phase="post_physics", all={"TestStats"},
        reads={"Transform3D"}, writes={"TestStats"}}, function(e, dt, ctx)
        require('system_tuning')
        assert(ctx.phase == "post_physics")
        local stats = e:get("TestStats")
        e:patch("TestStats", {ticks=stats.ticks+1})
        if ticks == 2 then removed = scene3d.despawn(spawned) end
    end))
end
function on_enter(ctx)
    assert(ctx.dt == 0 and scene3d.physics_available())
    local hit = scene3d.raycast({x=0,y=0,z=4}, {x=0,y=0,z=-4})
    assert(scene3d.handle(world:authored("crate")).id == scene3d.entity("crate").id)
    assert(scene3d.resolve(scene3d.entity("crate")):id() == world:authored("crate"):id())
    assert(not pcall(scene3d.handle, 17))
    local h = scene3d.entity("crate")
    assert(h:alive() and not pcall(h.alive) and not pcall(h.alive, 17))
    assert(pcall(scene3d.raycast, {x=0,y=0,z=4}, {x=0,y=0,z=-4}, {mask=2^3}))
    for _, bad in ipairs({8.5, -1, 2^64, math.huge, 0/0}) do
        assert(not pcall(scene3d.raycast, {x=0,y=0,z=4}, {x=0,y=0,z=-4}, {mask=bad}))
    end
    assert(hit and hit.entity.id == scene3d.entity("crate").id and hit.normal.z > 0.9)
    assert(scene3d.raycast({x=0,y=0,z=4}, {x=0,y=0,z=-4}, {ignore=hit.entity}) == nil)
    assert(scene3d.impulse(hit.entity, {x=1,y=0,z=0}))
    -- Each wrong-typed option/argument is a Lua error, never a native dereference.
    for _, bad in ipairs({17, true, "wrong", {}}) do
        assert(not pcall(scene3d.spawn, "prop.kinprefab", {parent=bad}))
        assert(not pcall(scene3d.raycast, {x=0,y=0,z=4}, {x=0,y=0,z=-4}, {ignore=bad}))
        assert(not pcall(scene3d.impulse, bad, {x=1,y=0,z=0}))
    end
    for _, bad in ipairs({world:authored("crate"), 17, true, "wrong", {}}) do
        for _, query in ipairs({scene3d.contact_events, scene3d.character_events, scene3d.ragdoll_events, scene3d.rope_events}) do
            assert(not pcall(query, bad))
        end
        assert(not pcall(scene3d.raycast, bad, {x=0,y=0,z=-4}))
        if type(bad) ~= "table" then
            assert(not pcall(scene3d.character_input, hit.entity, bad))
            assert(not pcall(scene3d.raycast, {x=0,y=0,z=4}, {x=0,y=0,z=-4}, bad))
            assert(not pcall(scene3d.spawn, "prop.kinprefab", bad))
        end
    end
    assert(scene3d.raycast({x=0,y=0,z=4}, {x=0,y=0,z=-4}, nil))
    assert(not pcall(scene3d.character_input, hit.entity, {move=17}))
    assert(not pcall(scene3d.character_input, hit.entity, {forward=true}))
    assert(not pcall(scene3d.spawn, "prop.kinprefab", {rotation=17}))
    assert(not pcall(scene3d.shape, {kind="box", half_extents=17}))
    assert(not pcall(scene3d.shape, {kind="sphere", radius="wide"}))
    assert(not pcall(scene3d.traversal_input, hit.entity, {grab=17}))
    assert(not pcall(scene3d.start_ragdoll, hit.entity, {velocity=true}))
end
function update(ctx)
    ticks = ticks + 1
    if ctx.input:pressed("launch") then
        assert(scene3d.impulse(scene3d.entity("crate"), {x=tuning.kick,y=0,z=0}))
    end
    if ticks == 1 then
        assert(not pcall(scene3d.spawn, "../outside.kinprefab"))
        spawned = scene3d.spawn("prop.kinprefab", {position={x=3,y=0,z=0}})
        assert(spawned.status == "pending")
        -- Runtime pose writes use the ordinary reflected ECS API.
        world:authored("camera"):patch("Transform3D", {position={x=0,y=0,z=8}})
    elseif ticks == 2 then
        assert(spawned.status == "succeeded")
        saved_root = spawned.prefab.root
        assert(saved_root:alive())
    elseif ticks == 3 then
        assert(removed.status == "succeeded" and not saved_root:alive())
        assert(not scene3d.impulse(saved_root, {x=1,y=0,z=0}))
        assert(scene3d.resolve(saved_root) == nil)
        local ok = pcall(scene3d.raycast, {x=0/0,y=0,z=4}, {x=0,y=0,z=-4})
        assert(not ok)
    end
end
function collect_actions(actions) require('action_tuning'); actions:add("launch", "Launch") end
function on_exit(ctx) record_exit() end
function write_report()
    require('report_tuning')
    local blocked = not pcall(scene3d.spawn, "prop.kinprefab")
    return {ticks=ticks, native_ticks=crate:get("TestStats").ticks, report_mutation_blocked=blocked}
end
)lua";

std::string report(SceneSession3D& session) {
    std::ostringstream out;
    JsonWriter json(out, false);
    json.begin_object();
    check(session.write_report(json), "script report failed");
    json.end_object();
    return out.str();
}
void ok(SceneSession3D& session, const auto& result, const char* message) {
    if (!result)
        for (const auto& error : session.diagnostics())
            std::cerr << error << '\n';
    check(result, message);
}
SceneSession3DOptions options(App& app, int& exits) {
    SceneSession3DOptions out;
    out.viewport = {160, 120};
    out.view.samples = 1;
    out.view.pick = true;
    out.warm_up = false;
    World3DConfig physics;
    physics.single_threaded = true;
    physics.gravity = {};
    physics.max_bodies = 16;
    physics.max_body_pairs = 64;
    physics.max_contact_constraints = 64;
    physics.temp_memory_bytes = 1024 * 1024;
    out.physics = physics;
    out.register_components = [](kin::EcsWorld& world) {
        world.components().native<Stats>("TestStats").field("ticks", &Stats::ticks);
    };
    out.scripts.app = &app;
    out.scripts.input = &app.input();
    out.scripts.bind = [&exits](sol::state& lua) { lua["record_exit"] = [&exits] { ++exits; }; };
    return out;
}
void workflow(App& app, Renderer2D& renderer, bool gpu) {
    InputMap input;
    input.bind("launch", Key::Space);
    app.input().set_map(input);
    Scratch source;
    std::filesystem::copy_file(std::filesystem::path(KIN_TEST_MODELS) / "parts.glb", source.path / "parts.glb");
    write(source.path / "test.kinscene", scene);
    write(source.path / "prop.kinprefab", prefab);
    write(source.path / "tuning.lua", "return {kick=2}");
    for (const auto* name : {"system_tuning.lua", "action_tuning.lua", "report_tuning.lua"})
        write(source.path / name, "return 1");
    write(source.path / "logic.lua", logic);
    Renderer3D rendering{renderer};
    Assets3D assets{rendering, source.path};
    int exits = 0;
    SceneSession3D session{rendering, assets, options(app, exits)};
    ok(session, session.load("test.kinscene"), "Lua scene load failed");
    auto* initial = session.world();
    const auto generation = session.generation();
    const auto body = session.bodies()->body_of(session.entity("crate").raw());
    InputActionContext actions{"lua3d"};
    check(session.collect_actions(actions) && actions.flattened_actions().size() == 1, "script actions missing");
    app.input().set_action_pressed("launch");
    ok(session, session.step(.1f), "script first step failed");
    app.input().begin_frame();
    check(std::fabs(session.physics()->position(body).x - .3f) < .001f, "input/impulse or exactly-once physics failed");
    const auto transform = session.entity("crate").get<Transform3D>()->position;
    const auto hash_before_collect = session.physics()->state_hash();
    const auto pose0 = session.bodies()->render_transform(session.entity("crate").raw(), 0);
    const auto pose1 = session.bodies()->render_transform(session.entity("crate").raw(), 1);
    check(pose0 && pose1 && std::abs(pose0->m[12]) < .001f && std::abs(pose1->m[12] - .3f) < .001f,
          "fixed-step render snapshots missing");
    ok(session, session.collect(.5f), "interpolated collection failed");
    check(session.entity("crate").get<Transform3D>()->position == transform &&
              session.physics()->state_hash() == hash_before_collect,
          "render interpolation changed gameplay");
    if (gpu) {
        ok(session, session.collect(0), "previous-pose collection failed");
        const auto previous = std::vector(session.scene()->instances().begin(), session.scene()->instances().end());
        ok(session, session.collect(.5f), "interpolated GPU collection failed");
        bool found_lerp = false;
        const auto current = session.scene()->instances();
        check(previous.size() == current.size(), "interpolation changed draw count");
        for (size_t i = 0; i < current.size(); ++i)
            if (std::abs(current[i].transform.m[12] - previous[i].transform.m[12] - .15f) < .001f)
                found_lerp = true;
        check(found_lerp, "session collect did not use interpolation alpha");
    }
    check(session.runtime_prefabs() == 1 && session.physics()->body_count() == 2,
          "Lua prefab did not bind before physics");
    if (gpu) {
        ok(session, session.render(), "Lua scene render failed");
        check(rendering.stats().instances >= 2 && rendering.stats().triangles > 0,
              "Lua spawn did not reach the renderer");
        renderer.clear();
        renderer.draw_texture(session.view()->texture(), {0, 0, 160, 120});
        std::vector<u8> pixels;
        Vec2i size;
        check(renderer.read_rgba({0, 0, 160, 120}, pixels, size), "Lua rendered image readback failed");
        std::size_t lit = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] > 20 || pixels[i + 1] > 20 || pixels[i + 2] > 20)
                ++lit;
        check(lit > 20, "Lua scene picture is empty");
        renderer.present();
    } else
        check(!session.view(), "CPU script scene allocated a GPU view");
    ok(session, session.step(.1f), "script second step failed");
    ok(session, session.step(.1f), "script third step failed");
    check(session.runtime_prefabs() == 0 && session.physics()->body_count() == 1, "Lua despawn leaked a body");
    check(session.entity("crate").get<Stats>()->ticks == 3, "Lua phase ran the wrong number of times");
    check(report(session).find("\"report_mutation_blocked\":true") != std::string::npos, "report mutated the scene");
    for (const auto* name : {"system_tuning.lua", "action_tuning.lua", "report_tuning.lua"})
        write(source.path / name, "return 2");
    check(session.changed_sources().size() == 3, "modules required by systems/actions/reports were not watched");

    // Syntax, initialization and enter failures preserve the live world/VM.
    const auto hash = session.physics()->state_hash();
    write(source.path / "logic.lua", "not valid lua");
    check(session.reload_if_changed().value_or(kin::SceneSession3DReload::Failed) == SceneSession3DReload::Failed,
          "bad Lua reload accepted");
    check(session.world() == initial && session.generation() == generation && session.physics()->state_hash() == hash,
          "failed Lua reload modified the active world");
    ok(session, session.step(.1f), "last good Lua VM did not survive reload failure");
    check(session.entity("crate").get<Stats>()->ticks == 4, "last good Lua systems lost");
    write(source.path / "bad.lua", R"lua(function on_load(s)
      s:entity('bad'):patch('TestStats', {ticks=0})
      assert(s:register_system('discarded', {all={'TestStats'}}, function(e) end))
      error('load boom') end)lua");
    write(source.path / "bad.kinscene", R"({"script":"bad.lua","entities":[]})");
    check(!session.load("bad.kinscene") && session.world() == initial, "on_load failure replaced the active world");
    write(source.path / "bad.lua", "function on_enter(c) scene3d.spawn('prop.kinprefab'); error('enter boom') end");
    check(!session.load("bad.kinscene") && session.world() == initial && !session.prepared(),
          "on_enter failure replaced the active world");
    write(source.path / "logic.lua", R"lua(
      local crate
      function on_load(scene) crate=scene:authored('crate') end
      function on_enter(ctx) crate:patch('RigidBody3D', {mass=2}) end
    )lua");
    const auto before_enter = session.physics()->state_hash();
    check(!session.load("test.kinscene") && session.world() == initial &&
              session.physics()->state_hash() == before_enter,
          "on_enter reconfigured an already bound body");
    check(exits == 0, "old scene exited before a replacement succeeded");

    write(source.path / "logic.lua", logic);
    write(source.path / "tuning.lua", "return {kick=4}");
    ok(session, session.prepare("test.kinscene"), "Lua reload preparation failed");
    write(source.path / "tuning.lua", "return {kick=5}");
    ok(session, session.activate(), "prepared Lua activation failed");
    check(exits == 1 && !session.changed_sources().empty(),
          "module changed after preparation was acknowledged without reload");
    app.input().set_action_pressed("launch");
    ok(session, session.step(.1f), "reloaded Lua step failed");
    app.input().begin_frame();
    const auto reloaded = session.bodies()->body_of(session.entity("crate").raw());
    check(std::fabs(session.physics()->position(reloaded).x - .5f) < .001f,
          "prepared Lua module snapshot was not retained");
    assert(session.unload());
    check(exits == 2, "on_exit did not run on unload");

    // Callback and ECS-system failures cancel their respective command buffers.
    for (const bool system : {false, true}) {
        write(source.path / "logic.lua", system ? R"lua(
          local request
          function on_load(scene)
            scene:authored("crate"):patch("TestStats", {ticks=0})
            assert(scene:register_system("lua.fail", {all={"TestStats"}}, function(e,dt,ctx)
              request=scene3d.spawn("prop.kinprefab"); error("system boom") end))
          end
          function write_report() return {status=request.status} end
        )lua"
                                                : R"lua(
          local request
          function update(ctx) request=scene3d.spawn("prop.kinprefab"); error("update boom") end
          function write_report() return {status=request.status} end
        )lua");
        ok(session, session.load("test.kinscene"), "failure fixture load failed");
        check(!session.step(.1f) && !session.diagnostics().empty(), "Lua runtime error was ignored");
        check(session.runtime_prefabs() == 0 && session.physics()->body_count() == 1,
              "failed callback executed queued lifecycle work");
        check(report(session).find("cancelled") != std::string::npos, "failed callback request stayed pending");
    }
    assert(session.unload());

    // A script host needs neither a physics world nor application/input pointers.
    write(source.path / "logic.lua", R"lua(
      local request, ticks= nil, 0
      function update(ctx)
        ticks=ticks+1
        assert(not ctx.input:pressed('launch') and not scene3d.physics_available())
        assert(not pcall(function() ctx.app:quit() end))
        assert(not pcall(scene3d.raycast, {x=0,y=0,z=2}, {x=0,y=0,z=-2}))
        if ticks==1 then request=scene3d.spawn('prop.kinprefab')
        elseif ticks==2 then assert(request.status=='succeeded'); scene3d.despawn(request) end
      end
    )lua");
    auto no_physics = options(app, exits);
    no_physics.physics.reset();
    no_physics.scripts.app = nullptr;
    no_physics.scripts.input = nullptr;
    {
        SceneSession3D game{rendering, assets, no_physics};
        ok(game, game.load("test.kinscene"), "Lua scene without physics failed to load");
        ok(game, game.step(.1f), "Lua no-physics spawn failed");
        check(game.runtime_prefabs() == 1 && !game.physics(), "no-physics script allocated physics");
        ok(game, game.step(.1f), "Lua no-physics despawn failed");
        check(game.runtime_prefabs() == 0, "no-physics script retained its prefab");
    }

    // Mounted content uses the same script/module/asset paths, without loose files at the mount.
    write(source.path / "logic.lua", logic);
    write(source.path / "tuning.lua", "return {kick=2}");
    Scratch package;
    check(write_content_pack(source.path, package.path / "game.kinpak").ok, "Lua content pack failed");
    const auto mount = package.path / "content";
    mount_content_pack(mount, ContentPack::open(package.path / "game.kinpak"));
    {
        Assets3D packed{rendering, mount};
        SceneSession3D game{rendering, packed, options(app, exits)};
        ok(game, game.load("test.kinscene"), "packed Lua scene/module load failed");
        ok(game, game.step(.1f), "packed Lua scene step failed");
    }
    unmount_content_pack(mount);
}

void gameplay(App& app, Renderer2D& renderer) {
    Scratch source;
    write(source.path / "game.kinscene", R"({"script":"game.lua","entities":[
      {"id":"floor","components":{"Transform3D":{"position":[0,-0.5,0]},"Collider3D":{"half_extents":[10,0.5,10]}}},
      {"id":"crate","components":{"Transform3D":{"position":[3,0.4,0]},"Collider3D":{},
        "RigidBody3D":{"mass":1,"allow_sleeping":false}}},
      {"id":"actor","components":{"Transform3D":{"position":[0,0.05,0]},
        "CharacterController3D":{"radius":0.2,"height":1.4,"crouch_height":0.8,"walk_speed":2,"jump_speed":3},
        "Animator3D":{"clip":"Idle","fade_seconds":0}}},
      {"id":"other","components":{"Transform3D":{"position":[-3,0,0]},"Animator3D":{}}}
    ]})");
    write(source.path / "game.lua", R"lua(
local actor, other, crate, action
local ticks, contacts, landed, jumps, crouches, markers = 0, 0, 0, 0, 0, 0
local stale = false
function on_load(world)
    assert(not pcall(scene3d.character_input, scene3d.entity('actor'), {}))
    world:authored('actor'):patch('TestStats', {ticks=0})
    assert(world:register_system('lua.contacts', {phase='post_physics', all={'TestStats'},
        reads={'CharacterController3D'}}, function(e,dt,ctx)
        local state = scene3d.character_state(actor)
        assert(state and state.position.y > -0.1)
        if ticks == 100 then assert(state.ground_speed > 1) end -- sprint exceeds the 0.6 m/s walking wish
        state.position.x = 999
        assert(scene3d.character_state(actor).position.x ~= 999)
        if state.crouching then crouches = crouches + 1 end
        for _, event in ipairs(scene3d.character_events(actor)) do
            assert(event.character.id == actor.id and event.position and event.normal)
            if event.kind == 'landed' then landed = landed + 1 end
            if event.kind == 'left_ground' and event.jumped then jumps = jumps + 1 end
        end
        local events = scene3d.contact_events(crate)
        for _, event in ipairs(events) do
            assert((event.a and event.a.id == crate.id) or (event.b and event.b.id == crate.id))
            -- The unmapped native obstacle deliberately has actor's user id.
            assert(not event.a or event.a.id ~= actor.id)
            assert(not event.b or event.b.id ~= actor.id)
            assert(event.kind == 'begin' or event.kind == 'persist' or event.kind == 'end')
            contacts = contacts + 1
        end
        assert(#scene3d.contact_events(crate) == #events) -- snapshots do not drain
        assert(#scene3d.contact_events(other) == 0)
    end))
    assert(world:register_system('lua.markers', {phase='audio', all={'TestStats'},
        reads={'Animator3D'}}, function(e,dt,ctx)
        local events, dropped = scene3d.animation_events(actor)
        assert(dropped == 0)
        for _, event in ipairs(events) do
            assert(event.channel == 'game' and event.value == 'payload')
            if not event.action or scene3d.action_event_valid(event.action) then markers = markers + 1 end
        end
    end))
end
function on_enter(ctx)
    actor, other, crate = scene3d.entity('actor'), scene3d.entity('other'), scene3d.entity('crate')
    assert(not scene3d.character_input(other, {}) and scene3d.character_state(other) == nil)
    assert(not scene3d.set_animation(crate, {playing=false}) and scene3d.animation_state(crate) == nil)
    assert(not pcall(scene3d.set_animation, actor, {speed=2,fade_seconds=0/0}))
    assert(not pcall(scene3d.set_animation, actor, {speed=math.huge}))
    assert(scene3d.animation_state(actor).speed == 1)
    assert(not scene3d.set_animation(actor, {clip='missing'}))
    assert(not pcall(scene3d.set_animation, actor, {bogus=true}))
    assert(not pcall(scene3d.set_animation_parameter, actor, 'bad', {}))
    assert(not pcall(scene3d.set_animation_parameter, actor, 'bad', math.huge))
    assert(not pcall(scene3d.character_input, actor, {move={x=0/0,y=0}}))
    assert(not pcall(scene3d.character_input, actor, {move={x=0,y=0},bogus=true}))
    assert(not pcall(scene3d.set_animation, other, {clip='Idle'})) -- authored machine owns selection
    assert(scene3d.set_animation(actor, {clip='Idle',speed=1,loop=true,playing=true,fade_seconds=0}))
    assert(scene3d.set_animation_parameter(actor, 'amount', 1.25))
    assert(scene3d.set_animation_parameter(actor, 'ready', true))
    assert(scene3d.trigger_animation(other, 'wave'))
    assert(scene3d.play_action(actor, 'missing') == nil)
    assert(not pcall(scene3d.play_action, actor, 'wave', 'bogus'))
    local first = scene3d.play_action(actor, 'wave')
    assert(first and scene3d.action_status(first) == 'playing')
    assert(scene3d.play_action(actor, 'wave') == nil)
    local replacement = scene3d.play_action(actor, 'wave', 'replace')
    assert(replacement and scene3d.action_status(first) == 'superseded')
    assert(not scene3d.action_event_valid(first))
    assert(not pcall(scene3d.cancel_action, replacement, 0/0))
    assert(scene3d.action_status(replacement) == 'playing')
    assert(scene3d.cancel_action(replacement, 0))
    assert(scene3d.action_status(replacement) == 'cancelled' and not scene3d.action_event_valid(replacement))
    action = scene3d.play_action(actor, 'wave')
    local second = scene3d.play_action(other, 'wave')
    assert(second and scene3d.cancel_action(second, 0))
    assert(scene3d.action_status(action) == 'playing') -- other actor's cancellation is isolated
    if previous_actor():alive() then error('old scene handle is still alive') end
    assert(not scene3d.character_input(previous_actor(), {}))
end
function update(ctx)
    ticks = ticks + 1
    if not actor:alive() then
        stale = true
        assert(not scene3d.character_input(actor, {}))
        assert(scene3d.character_state(actor) == nil and scene3d.animation_state(actor) == nil)
        assert(not scene3d.set_animation_parameter(actor, 'amount', 7))
        assert(not scene3d.trigger_animation(actor, 'wave'))
        assert(scene3d.action_status(action) == 'superseded' and not scene3d.cancel_action(action))
        assert(not scene3d.action_event_valid(action))
        assert(not pcall(scene3d.contact_events, actor))
        assert(not pcall(scene3d.character_events, actor))
        local events, dropped = scene3d.animation_events(actor)
        assert(#events == 0 and dropped == 0)
        return
    end
    local turn = ticks >= 101 and ticks <= 110
    assert(scene3d.character_input(actor, {move={x=turn and 0 or 0.3,y=turn and 0.3 or 0},
        forward=turn and {x=0,y=0,z=1} or {x=0,y=0,z=-1}, jump=ticks==40,
        crouch=ticks>=65 and ticks<=75,sprint=ticks>=90 and ticks<=100}))
    if ticks == 2 then
        assert(scene3d.set_animation(actor, {playing=false}))
    elseif ticks == 3 then
        assert(scene3d.set_animation(actor, {playing=true}))
    end
end
function write_report()
    assert(not pcall(scene3d.character_input, actor, {}))
    assert(not pcall(scene3d.set_animation, actor, {playing=false}))
    assert(not pcall(scene3d.play_action, actor, 'wave'))
    assert(not pcall(scene3d.cancel_action, action))
    assert(not pcall(scene3d.set_animation_parameter, actor, 'ready', false))
    assert(not pcall(scene3d.trigger_animation, actor, 'wave'))
    return {contacts=contacts,landed=landed,jumps=jumps,crouches=crouches,markers=markers,
        status=scene3d.action_status(action),stale=stale}
end
)lua");
    auto model = std::make_shared<Model3D>();
    Skeleton3D skeleton;
    skeleton.names = {"root"};
    skeleton.parents = {-1};
    skeleton.rest.resize(1);
    skeleton.inverse_bind.resize(1);
    model->skeletons.push_back(skeleton);
    model->clips = {{.name = "Idle", .duration = 1}, {.name = "Wave", .duration = .5f}};
    model->clips[0].channels.resize(1);
    model->clips[1].channels.resize(1);
    model->clips[1].channels[0].translation = {.times = {0, .5f}, .values = {{0, 0, 0}, {0, 1, 0}}};
    auto actions = std::make_shared<CharacterActions3D>(
        CharacterActions3D{{.name = "wave", .clip = "Wave", .playback = {.fade_in = 0, .fade_out = 0}}});
    auto tracks = std::make_shared<AnimationEventTracks3D>(
        AnimationEventTracks3D{{"Wave",
                                {{.phase = .2f, .event = {.channel = "game", .name = "first", .value = "payload"}},
                                 {.phase = .4f, .event = {.channel = "game", .name = "second", .value = "payload"}}},
                                MarkerSource3D::Action}});
    int exits = 0;
    auto config = options(app, exits);
    config.physics->gravity = {0, -9.81f, 0};
    config.physics->contact_events = true;
    config.physics->persist_events = true;
    config.initialize = [model, actions, tracks](SceneSession3DContext& ctx) {
        for (const auto* name : {"actor", "other"}) {
            auto entity = ctx.instance.by_authored_id.at(name);
            entity.set(SkinnedMeshRenderer3D{.model = model});
            auto* animator = entity.get_mut<Animator3D>();
            animator->actions = actions;
            animator->event_tracks = tracks;
            if (std::string_view(name) == "other") {
                auto machine = std::make_shared<AnimationStateMachine>();
                machine->initial = "idle";
                machine->states = {{"idle", "Idle"}, {"wave", "Wave"}};
                machine->transitions = {
                    {.from = "idle", .to = "wave", .when = {{.param = "wave", .op = CondOp::TriggerSet}}}};
                animator->machine = machine;
            }
        }
        // A foreign body's user id must not map it to an unrelated scene actor.
        Body3DDesc obstacle;
        obstacle.shape = Shape3D::box({.5f, .5f, .5f});
        obstacle.position = {3.6f, .5f, 0};
        obstacle.user_id = ctx.instance.by_authored_id.at("actor").id();
        check(bool(ctx.physics->create_body(obstacle)), "foreign physics obstacle failed");
    };
    SceneEntity3D previous;
    config.scripts.bind = [&previous](sol::state& lua) { lua["previous_actor"] = [&previous] { return previous; }; };
    Renderer3D rendering{renderer};
    Assets3D assets{rendering, source.path};
    SceneSession3D session{rendering, assets, config};
    ok(session, session.load("game.kinscene"), "Lua gameplay scene load failed");
    const auto actor = session.entity("actor");
    ok(session, session.step(1.f / 60), "Lua gameplay first step failed");
    check(session.entity("other").get<Animator3D>()->state == "wave", "Lua trigger missed native state machine");
    check(actor.get<Animator3D>()->params.get_bool("ready") &&
              actor.get<Animator3D>()->params.get_float("amount") == 1.25f,
          "Lua parameter types were lost");
    const auto time = actor.get<Animator3D>()->player.time();
    ok(session, session.step(1.f / 60), "Lua pause failed");
    check(actor.get<Animator3D>()->player.time() == time, "Lua pause still advanced animation");
    for (int i = 2; i < 120; ++i)
        ok(session, session.step(1.f / 60), "Lua gameplay step failed");
    const auto text = report(session);
    const auto parsed = parse_json(text);
    check(parsed.ok(), "Lua gameplay report missing");
    const auto& script = *parsed.value;
    check(script.find("contacts")->as_number() > 0 && script.find("landed")->as_number() > 0 &&
              script.find("jumps")->as_number() == 1 && script.find("crouches")->as_number() > 0,
          "Lua did not receive contacts/grounding/jump/crouch");
    check(script.find("markers")->as_number() == 2 && script.find("status")->as_string() == "completed",
          "Lua action markers or completion failed");
    check(actor.get<Transform3D>()->position.x > .5f, "Lua character command did not move the controller");
    check(actor.get<Transform3D>()->position.z > .05f, "Lua character forward direction did not steer movement");
    previous = session.handle(actor);
    session.world()->entities().destroy(actor);
    ok(session, session.step(1.f / 60), "stale Lua gameplay handles were accepted");
    check(report(session).find("\"stale\":true") != std::string::npos, "stale binding test did not run");
    ok(session, session.reload(), "Lua gameplay replacement failed");
    ok(session, session.step(1.f / 60), "old handles survived Lua gameplay replacement");

    // Copies of marker payloads stay readable, but cancellation revokes their action.
    write(source.path / "game.lua", R"lua(
      local actor, action, delivered = nil, nil, 0
      function on_load(world)
        world:authored('actor'):patch('TestStats',{ticks=0})
        assert(world:register_system('markers.cancel', {phase='audio',all={'TestStats'},
            reads={'Animator3D'},writes={'Animator3D'}}, function(e,dt,ctx)
          local events = scene3d.animation_events(actor)
          assert(#events == 2)
          for _, event in ipairs(events) do
            if scene3d.action_event_valid(event.action) then
              delivered = delivered + 1
              assert(scene3d.cancel_action(action,0))
            end
          end
          assert(#scene3d.animation_events(actor) == 0)
        end))
      end
      function on_enter(ctx)
        actor = scene3d.entity('actor')
        action = scene3d.play_action(actor,'wave')
      end
      function write_report() return {delivered=delivered,status=scene3d.action_status(action)} end
    )lua");
    ok(session, session.reload(), "Lua marker cancellation fixture failed");
    ok(session, session.step(.25f), "Lua marker cancellation failed");
    const auto cancelled = report(session);
    check(cancelled.find("\"delivered\":1") != std::string::npos &&
              cancelled.find("\"status\":\"cancelled\"") != std::string::npos,
          "cancelled Lua action delivered subsequent markers");

    // Animation remains usable in a scene with no physics service.
    write(source.path / "game.lua", R"lua(
      local actor, ticks = nil, 0
      function on_enter(ctx)
        actor = scene3d.entity('actor')
        assert(not scene3d.character_input(actor, {}))
        assert(scene3d.character_state(actor) == nil)
        assert(not pcall(scene3d.contact_events) and not pcall(scene3d.character_events))
        assert(scene3d.set_animation(actor,{clip='Wave',loop=true}))
        assert(scene3d.set_animation_parameter(actor,'ready',true))
      end
      function update(ctx)
        ticks = ticks + 1
        if ticks == 2 then assert(scene3d.set_animation(actor,{loop=false}))
        elseif ticks == 3 then
          assert(scene3d.animation_state(actor).finished)
          assert(scene3d.set_animation(actor,{loop=true}))
        end
      end
    )lua");
    config.physics.reset();
    config.initialize = [model](SceneSession3DContext& ctx) {
        auto actor = ctx.instance.by_authored_id.at("actor");
        actor.set(SkinnedMeshRenderer3D{.model = model});
    };
    SceneSession3D no_physics{rendering, assets, config};
    ok(no_physics, no_physics.load("game.kinscene"), "Lua animation without physics failed");
    ok(no_physics, no_physics.step(.1f), "Lua animation without physics did not step");
    check(no_physics.entity("actor").get<Animator3D>()->player.time() > .09f, "Lua animation required a physics world");
    ok(no_physics, no_physics.step(.5f), "Lua loop disable failed");
    check(no_physics.entity("actor").get<Animator3D>()->player.finished(),
          "changing Lua loop option did not stop wrapping the playing clip");
    ok(no_physics, no_physics.step(.1f), "Lua loop enable failed");
    const auto& resumed = no_physics.entity("actor").get<Animator3D>()->player;
    check(!resumed.finished() && resumed.time() > .09f && resumed.time() < .11f,
          "changing Lua loop option restarted the clock or failed to resume wrapping");
}
} // namespace
int main(int argc, char** argv) {
    const bool gpu = argc > 1 && std::string_view(argv[1]) == "gpu";
    try {
        App app{{.mode = gpu ? AppMode::Windowed : AppMode::Headless}};
        auto& window = app.create_window({.title = "script3d-test", .width = 160, .height = 120, .hidden = true});
        auto renderer = gpu ? std::make_unique<Renderer2D>(make_render_backend(window, false, true))
                            : std::make_unique<Renderer2D>(window);
        if (gpu && !renderer->capabilities().rendering_3d) {
            const auto* required = std::getenv("KIN_REQUIRE_GPU_TESTS");
            if (required && std::string_view(required) != "0")
                throw std::runtime_error("GPU required");
            return 77;
        }
        workflow(app, *renderer, gpu);
        gameplay(app, *renderer);
        test_lua_advanced3d(*renderer);
        std::cout << "Lua 3D workflow passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
