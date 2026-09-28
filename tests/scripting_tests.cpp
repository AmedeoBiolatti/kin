#include <kin/ecs/render.hpp>
#include <kin/ecs/system.hpp>
#include <kin/platform/app.hpp>
#include <kin/renderer/renderer2d.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scripting/script_scene.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

struct ScriptingCustomStats {
    bool alive = true;
    kin::i32 hp = 0;
    kin::f32 speed = 0.0f;
    kin::Vec2f pos{};
};

namespace {

void write_text(const std::filesystem::path& path, std::string_view text) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path);
    out << text;
}

void advance_write_time(const std::filesystem::path& path, int seconds) {
    const auto current = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, current + std::chrono::seconds(seconds));
}

struct Fixture {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::Window& window;
    kin::Renderer2D renderer;
    kin::SceneManager scenes;
    kin::SceneContext ctx;

    Fixture()
        : window(app.create_window({.title = "scripting-test", .width = 64, .height = 64, .hidden = true})),
          renderer(window),
          ctx{app, window, renderer, app.input(), scenes, 0.1f, true} {
    }
};

kin::ScriptScene make_scene(const std::filesystem::path& root,
                            std::string_view script_name,
                            bool hot_reload = true) {
    return kin::ScriptScene{{
        .asset_root = root,
        .script_path = std::string{script_name},
        .name = "TestScript",
        .hot_reload = hot_reload,
    }};
}

void test_load_update_actions_and_spawn(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-load";
    std::filesystem::create_directories(dir);
    const std::filesystem::path script = dir / "scene.lua";
    write_text(script, R"lua(
function on_load(scene)
    scene:set_clear_color(1, 2, 3)
    scene:entity("script-rect"):set_transform(4, 5):set_rect(6, 7, 8, 9, 10, 2, 3)
end

function update(ctx)
end

function collect_actions(actions)
    actions:add("tick", "Tick")
end
)lua");

    kin::ScriptScene scene = make_scene(dir, "scene.lua", false);
    assert(scene.script_loaded());
    assert(scene.render_config().clear_color == kin::Color::rgb(1, 2, 3));
    assert(scene.ecs().count<kin::ScriptOwned>() == 1);
    assert((scene.ecs().count<kin::Transform2D, kin::RectRenderer>() == 1));

    kin::RectRenderer* rect = nullptr;
    scene.ecs().query<kin::RectRenderer>().each_entity([&](kin::EcsEntity, kin::RectRenderer& value) {
        rect = &value;
    });
    assert(rect != nullptr);
    assert((rect->size == kin::Vec2f{6.0f, 7.0f}));
    assert(rect->color == kin::Color::rgb(8, 9, 10));
    assert(rect->layer == 2);
    assert(rect->order == 3);

    scene.update(fixture.ctx);
    kin::InputActionContext actions{"script"};
    scene.collect_actions(actions);
    const auto flattened = actions.flattened_actions();
    assert(flattened.size() == 1);
    assert(flattened[0].name == "tick");
    assert(flattened[0].label == "Tick");
}

void test_input_query_can_quit_app(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-input";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function update(ctx)
    if ctx.input:pressed("tick") then
        ctx.app:quit()
    end
end
)lua");

    kin::InputMap input;
    input.bind("tick", kin::Key::Enter);

    kin::SceneManager scenes;
    kin::ScriptScene scene = make_scene(dir, "scene.lua", false);
    fixture.app.input().set_map(input);

    int frames = 0;
    fixture.app.run_for(3, [&](kin::f32 dt, kin::i32) {
        kin::SceneContext ctx{fixture.app, fixture.window, fixture.renderer, fixture.app.input(), scenes, dt, true};
        fixture.app.input().set_action_pressed("tick");
        scene.update(ctx);
        ++frames;
    });

    assert(frames == 1);
}

void test_lua_can_query_and_mutate_existing_ecs_entities() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-ecs";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function on_load(scene)
    local native = scene:find("native")
    assert(native ~= nil)
    assert(native:has("transform"))

    local pos = native:get_transform()
    native:set_transform(pos.x + 2, pos.y + 3)
    native:set_rect(8, 9, 10, 11, 12)
    assert(scene:count("rect") == 1)
    native:remove("rect")
    assert(not native:has("rect"))

    scene:entity("script-a"):set_transform(1, 1)
    scene:entity("script-b"):set_transform(2, 2):set_rect(3, 4, 20, 30, 40)

    local transformed = scene:with("transform")
    local script_owned = scene:with("script_owned")
    scene:entity("summary"):set_transform(#transformed, #script_owned)
end
)lua");

    kin::EcsWorld world;
    world.component<kin::Transform2D>("Transform2D");
    world.entity("native").set(kin::Transform2D{{10.0f, 20.0f}});

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "EcsScript",
        .hot_reload = false,
    }, &world};

    assert(scene.script_loaded());
    kin::EcsEntity native{world.raw().lookup("native")};
    const kin::Transform2D* native_transform = native.get<kin::Transform2D>();
    assert(native_transform != nullptr);
    assert((native_transform->pos == kin::Vec2f{12.0f, 23.0f}));
    assert(!native.has<kin::RectRenderer>());

    kin::EcsEntity summary{world.raw().lookup("summary")};
    const kin::Transform2D* summary_transform = summary.get<kin::Transform2D>();
    assert(summary_transform != nullptr);
    assert((summary_transform->pos == kin::Vec2f{3.0f, 2.0f}));
    assert(world.count<kin::ScriptOwned>() == 3);
    assert(world.count<kin::RectRenderer>() == 1);
}

void test_lua_can_use_registered_custom_components() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-custom-components";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function on_load(scene)
    assert(scene:count("Stats") == 1)
    local entity = scene:with("Stats")[1]
    assert(entity:has("Stats"))

    local stats = entity:get("Stats")
    assert(stats.alive == true)
    assert(stats.hp == 7)
    assert(stats.speed == 2.5)
    assert(stats.pos.x == 1)
    assert(stats.pos.y == 2)

    entity:patch("Stats", {
        alive = false,
        hp = stats.hp + 5,
        speed = 4.25,
        pos = { x = 3, y = 4 }
    })

    local matches = scene:with("Stats")
    local summary = scene:entity("custom-summary")
    assert(not summary:has("Stats"), "summary unexpectedly has Stats")
    local stats_count = scene:count("Stats")
    assert(stats_count == 1, "stats_count=" .. stats_count)
    summary:set_transform(#matches, stats_count)
end
)lua");

    kin::ScriptComponentRegistry registry;
    registry.component<ScriptingCustomStats>("Stats")
        .field("alive", &ScriptingCustomStats::alive)
        .field("hp", &ScriptingCustomStats::hp)
        .field("speed", &ScriptingCustomStats::speed)
        .field("pos", &ScriptingCustomStats::pos);

    kin::EcsWorld world;
    world.component<kin::ScriptOwned>("ScriptOwned");
    kin::EcsEntity native = world.entity("native").set(ScriptingCustomStats{
        .alive = true,
        .hp = 7,
        .speed = 2.5f,
        .pos = {1.0f, 2.0f},
    });
    assert(native.has<ScriptingCustomStats>());

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "CustomComponentScript",
        .components = &registry,
        .hot_reload = false,
    }, &world};

    assert(scene.script_loaded());
    const ScriptingCustomStats* stats = native.get<ScriptingCustomStats>();
    assert(stats != nullptr);
    assert(!stats->alive);
    assert(stats->hp == 12);
    assert(stats->speed == 4.25f);
    assert((stats->pos == kin::Vec2f{3.0f, 4.0f}));

    kin::EcsEntity summary{world.raw().lookup("custom-summary")};
    const kin::Transform2D* summary_transform = summary.get<kin::Transform2D>();
    assert(summary_transform != nullptr);
    assert((summary_transform->pos == kin::Vec2f{1.0f, 1.0f}));

}

void test_lua_can_register_query_plan_system() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-ecs-system";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function on_load(scene)
    scene:register_system("lua.damage", {
        phase = "update",
        all = {"Stats"},
        reads = {"Stats"},
        writes = {"Stats"}
    }, function(entity, dt)
        local stats = entity:get("Stats")
        entity:patch("Stats", { hp = stats.hp + 3 })
    end)
end
)lua");

    kin::EcsWorld world;
    world.component<kin::ScriptOwned>("ScriptOwned");
    world.components().data("Stats").field_i32("hp", 0);
    kin::EcsEntity native = world.entity("native");
    assert(world.components().add(native, "Stats"));
    assert(world.components().patch_field(native, "Stats", "hp", kin::i32{7}));

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "LuaSystemScript",
        .hot_reload = false,
    }, &world};

    assert(scene.script_loaded());
    assert(world.systems().contains("lua.damage"));
    assert(world.run_frame(0.1f));
    std::optional<kin::ComponentSnapshot> component_snapshot = world.components().snapshot(native, "Stats");
    assert(std::get<kin::i32>(component_snapshot->fields[0].value) == 10);
    const kin::SystemSnapshot system_snapshot = world.systems().snapshot("lua.damage");
    assert(system_snapshot.kind == kin::SystemKind::Script);
    assert(system_snapshot.stats.runs == 1);
}

void test_lua_component_helper_commits_data_component() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-component-helper";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function on_load(scene)
    scene:register_system("lua.stats_helper", {
        phase = "update",
        all = {"Stats"},
        reads = {"Stats"},
        writes = {"Stats"}
    }, function(entity, dt)
        local stats = entity:component("Stats")
        stats.hp = stats.hp + 4
        stats.label = "committed"
        stats:commit()
    end)
end
)lua");

    kin::EcsWorld world;
    world.components().data("Stats").field_i32("hp", 0).field_string("label", "");
    kin::EcsEntity native = world.entity("native");
    assert(world.components().add(native, "Stats"));
    assert(world.components().patch_field(native, "Stats", "hp", kin::i32{6}));

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "LuaComponentHelperScript",
        .hot_reload = false,
    }, &world};

    assert(scene.script_loaded());
    assert(world.run_frame(0.1f));
    std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(native, "Stats");
    assert(std::get<kin::i32>(snapshot->fields[0].value) == 10);
    assert(std::get<std::string>(snapshot->fields[1].value) == "committed");
}

void test_lua_system_can_queue_command_buffer_mutations() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-command-buffer";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function on_load(scene)
    scene:register_system("lua.spawn_commands", {
        phase = "update",
        all = {"Stats"},
        reads = {"Stats"},
        writes = {"Stats"}
    }, function(entity, dt, sys)
        local stats = entity:component("Stats")
        stats.hp = stats.hp + 2
        stats:commit()

        local spawned = sys.commands:create("spawned")
        sys.commands:patch(spawned, "Stats", { hp = stats.hp })
        sys.commands:add(spawned, "ScriptOwned")
    end)

    scene:register_system("lua.destroy_commands", {
        phase = "update",
        all = {"KillMe"}
    }, function(entity, dt, sys)
        sys.commands:destroy(entity)
    end)
end
)lua");

    kin::EcsWorld world;
    world.component<kin::ScriptOwned>("ScriptOwned");
    world.components().data("Stats").field_i32("hp", 0);
    world.components().data("KillMe").field_bool("enabled", true);
    kin::EcsEntity native = world.entity("native");
    assert(world.components().add(native, "Stats"));
    assert(world.components().patch_field(native, "Stats", "hp", kin::i32{7}));
    kin::EcsEntity victim = world.entity("victim");
    assert(world.components().add(victim, "KillMe"));

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "LuaCommandBufferScript",
        .hot_reload = false,
    }, &world};

    assert(scene.script_loaded());
    assert(world.run_frame(0.1f));

    std::optional<kin::ComponentSnapshot> native_stats = world.components().snapshot(native, "Stats");
    assert(std::get<kin::i32>(native_stats->fields[0].value) == 9);

    kin::EcsEntity spawned{world.raw().lookup("spawned")};
    assert(spawned);
    assert(spawned.has<kin::ScriptOwned>());
    std::optional<kin::ComponentSnapshot> spawned_stats = world.components().snapshot(spawned, "Stats");
    assert(std::get<kin::i32>(spawned_stats->fields[0].value) == 9);
    assert(!victim.alive());

    const kin::SystemSnapshot spawn_snapshot = world.systems().snapshot("lua.spawn_commands");
    assert(spawn_snapshot.structural_mutation_policy == kin::SystemStructuralMutationPolicy::CommandBufferOnly);
    assert(spawn_snapshot.command_stats.flushed == 3);
}

void test_lua_timer_helpers_and_interval_systems(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-timers";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function on_load(scene)
    local timed = scene:find("timed")
    timed:timer("fire", 0.2, false)

    scene:register_system("lua.timer_ready", {
        phase = "update",
        all = {"Timer", "Counter"},
        reads = {"Timer", "Counter"},
        writes = {"Counter"}
    }, function(entity, dt)
        if entity:ready("fire") then
            local counter = entity:component("Counter")
            counter.value = counter.value + 1
            counter:commit()
        end
    end)

    scene:register_system("lua.interval_counter", {
        phase = "update",
        all = {"IntervalCounter"},
        reads = {"IntervalCounter"},
        writes = {"IntervalCounter"},
        interval_seconds = 0.25
    }, function(entity, dt)
        local counter = entity:component("IntervalCounter")
        counter.value = counter.value + 1
        counter:commit()
    end)
end
)lua");

    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    world.components().data("Counter").field_i32("value", 0);
    world.components().data("IntervalCounter").field_i32("value", 0);
    kin::EcsEntity timed = world.entity("timed");
    assert(world.components().add(timed, "Counter"));
    kin::EcsEntity interval = world.entity("interval");
    assert(world.components().add(interval, "IntervalCounter"));

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "LuaTimerScript",
        .hot_reload = false,
    }, &world};

    assert(scene.script_loaded());
    scene.update(fixture.ctx);
    std::optional<kin::ComponentSnapshot> counter = world.components().snapshot(timed, "Counter");
    assert(std::get<kin::i32>(counter->fields[0].value) == 0);

    kin::SceneContext long_ctx = fixture.ctx;
    long_ctx.dt = 0.1f;
    scene.update(long_ctx);
    counter = world.components().snapshot(timed, "Counter");
    assert(std::get<kin::i32>(counter->fields[0].value) == 1);

    std::optional<kin::ComponentSnapshot> interval_counter = world.components().snapshot(interval, "IntervalCounter");
    assert(std::get<kin::i32>(interval_counter->fields[0].value) == 0);
    scene.update(long_ctx);
    interval_counter = world.components().snapshot(interval, "IntervalCounter");
    assert(std::get<kin::i32>(interval_counter->fields[0].value) == 1);
}

void test_lua_timer_state_survives_script_hot_reload(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-timer-hot-reload";
    std::filesystem::create_directories(dir);
    const std::filesystem::path script = dir / "scene.lua";
    write_text(script, R"lua(
local amount = 1

function on_load(scene)
    local timed = scene:find("timed")
    if not timed:has("Timer") then
        timed:timer("fire", 0.3, false)
    end
    scene:register_system("lua.reload_timer", {
        phase = "update",
        all = {"Timer", "Counter"},
        reads = {"Timer", "Counter"},
        writes = {"Counter"}
    }, function(entity, dt)
        if entity:ready("fire") then
            local counter = entity:component("Counter")
            counter.value = counter.value + amount
            counter:commit()
        end
    end)
end
)lua");

    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);
    world.components().data("Counter").field_i32("value", 0);
    kin::EcsEntity timed = world.entity("timed");
    assert(world.components().add(timed, "Counter"));

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "LuaTimerReloadScript",
        .hot_reload = true,
    }, &world};
    assert(scene.script_loaded());

    kin::SceneContext ctx = fixture.ctx;
    ctx.dt = 0.2f;
    scene.update(ctx);
    std::optional<kin::ComponentSnapshot> timer = world.components().snapshot(timed, "Timer");
    assert(std::get<kin::f32>(timer->fields[1].value) > 0.19f);

    write_text(script, R"lua(
local amount = 2

function on_load(scene)
    local timed = scene:find("timed")
    if not timed:has("Timer") then
        timed:timer("fire", 0.3, false)
    end
    scene:register_system("lua.reload_timer", {
        phase = "update",
        all = {"Timer", "Counter"},
        reads = {"Timer", "Counter"},
        writes = {"Counter"}
    }, function(entity, dt)
        if entity:ready("fire") then
            local counter = entity:component("Counter")
            counter.value = counter.value + amount
            counter:commit()
        end
    end)
end
)lua");
    advance_write_time(script, 2);
    ctx.dt = 0.1f;
    scene.update(ctx);

    std::optional<kin::ComponentSnapshot> counter = world.components().snapshot(timed, "Counter");
    assert(std::get<kin::i32>(counter->fields[0].value) == 2);
}

void test_lua_require_loads_project_modules_and_hot_reloads_dependency(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-modules";
    std::filesystem::create_directories(dir);
    const std::filesystem::path script = dir / "scene.lua";
    const std::filesystem::path module = dir / "scripts" / "game" / "math.lua";
    write_text(module, R"lua(
return {
    value = function()
        return 2
    end
}
)lua");
    write_text(script, R"lua(
local math_mod = require("game.math")

function on_load(scene)
    scene:entity("module-value"):set_transform(math_mod.value(), 0)
end
)lua");

    kin::ScriptScene scene = make_scene(dir, "scene.lua", true);
    assert(scene.script_loaded());
    kin::EcsEntity value{scene.ecs().raw().lookup("module-value")};
    assert(value);
    assert(value.get<kin::Transform2D>()->pos.x == 2.0f);

    write_text(module, R"lua(
return {
    value = function()
        return 5
    end
}
)lua");
    advance_write_time(module, 2);
    scene.update(fixture.ctx);

    value = kin::EcsEntity{scene.ecs().raw().lookup("module-value")};
    assert(value);
    assert(value.get<kin::Transform2D>()->pos.x == 5.0f);
}

void test_failed_module_reload_keeps_previous_script(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-module-failed-reload";
    std::filesystem::create_directories(dir);
    const std::filesystem::path script = dir / "scene.lua";
    const std::filesystem::path module = dir / "scripts" / "game" / "value.lua";
    write_text(module, "return { value = function() return 4 end }\n");
    write_text(script, R"lua(
local mod = require("game.value")

function on_load(scene)
    scene:entity("stable"):set_transform(mod.value(), 0)
end
)lua");

    kin::ScriptScene scene = make_scene(dir, "scene.lua", true);
    assert(scene.script_loaded());
    kin::EcsEntity stable{scene.ecs().raw().lookup("stable")};
    assert(stable.get<kin::Transform2D>()->pos.x == 4.0f);

    write_text(module, "return { value = function() this is not lua end }\n");
    advance_write_time(module, 2);
    scene.update(fixture.ctx);

    assert(!scene.last_script_error().empty());
    stable = kin::EcsEntity{scene.ecs().raw().lookup("stable")};
    assert(stable);
    assert(stable.get<kin::Transform2D>()->pos.x == 4.0f);
}

void test_lua_require_reports_missing_and_rejects_outside_paths() {
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-missing-module";
        std::filesystem::create_directories(dir);
        write_text(dir / "scene.lua", "local missing = require(\"game.missing\")\n");
        kin::ScriptScene scene = make_scene(dir, "scene.lua", false);
        assert(!scene.script_loaded());
        assert(scene.last_script_error().find("game.missing") != std::string::npos);
        assert(!scene.last_script_diagnostic().message.empty());
    }
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-outside-module";
        std::filesystem::create_directories(dir);
        write_text(dir / "scene.lua", "local outside = require(\"..outside\")\n");
        kin::ScriptScene scene = make_scene(dir, "scene.lua", false);
        assert(!scene.script_loaded());
        assert(scene.last_script_error().find("..outside") != std::string::npos);
    }
}

void test_lua_system_uses_required_module_after_reload(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-module-system";
    std::filesystem::create_directories(dir);
    const std::filesystem::path script = dir / "scene.lua";
    const std::filesystem::path module = dir / "scripts" / "game" / "damage.lua";
    write_text(module, "return { amount = function() return 3 end }\n");
    write_text(script, R"lua(
local damage = require("game.damage")

function on_load(scene)
    scene:register_system("lua.damage_module", {
        phase = "update",
        all = {"Stats"},
        reads = {"Stats"},
        writes = {"Stats"}
    }, function(entity, dt)
        local stats = entity:get("Stats")
        entity:patch("Stats", { hp = stats.hp + damage.amount() })
    end)
end
)lua");

    kin::EcsWorld world;
    world.component<kin::ScriptOwned>("ScriptOwned");
    world.components().data("Stats").field_i32("hp", 0);
    kin::EcsEntity native = world.entity("native");
    assert(world.components().add(native, "Stats"));

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "LuaSystemModuleScript",
        .hot_reload = true,
    }, &world};
    assert(scene.script_loaded());
    scene.update(fixture.ctx);
    std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(native, "Stats");
    assert(std::get<kin::i32>(snapshot->fields[0].value) == 3);

    write_text(module, "return { amount = function() return 8 end }\n");
    advance_write_time(module, 2);
    scene.update(fixture.ctx);
    snapshot = world.components().snapshot(native, "Stats");
    assert(std::get<kin::i32>(snapshot->fields[0].value) == 11);
}

void test_lua_system_reports_invalid_data_component_patch() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-invalid-data-patch";
    std::filesystem::create_directories(dir);
    write_text(dir / "scene.lua", R"lua(
function on_load(scene)
    scene:register_system("lua.bad_patch", {
        phase = "update",
        all = {"Stats"},
        reads = {"Stats"},
        writes = {"Stats"}
    }, function(entity, dt)
        entity:patch("Stats", { missing = 1 })
    end)
end
)lua");

    kin::EcsWorld world;
    world.component<kin::ScriptOwned>("ScriptOwned");
    world.components().data("Stats").field_i32("hp", 0);
    kin::EcsEntity native = world.entity("native");
    assert(world.components().add(native, "Stats"));

    kin::ScriptScene scene{{
        .asset_root = dir,
        .script_path = "scene.lua",
        .name = "LuaSystemInvalidPatch",
        .hot_reload = false,
    }, &world};

    assert(scene.script_loaded());
    assert(!world.run_frame(0.1f));
    const kin::SystemSnapshot snapshot = world.systems().snapshot("lua.bad_patch");
    assert(snapshot.last_error.find("unknown field") != std::string::npos);
}

void test_reload_replaces_script_entities(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-reload";
    std::filesystem::create_directories(dir);
    const std::filesystem::path script = dir / "scene.lua";
    write_text(script, R"lua(
function on_load(scene)
    scene:entity("old"):set_transform(1, 1):set_rect(2, 2, 10, 20, 30)
end
)lua");

    kin::ScriptScene scene = make_scene(dir, "scene.lua", true);
    scene.ecs().entity("native").set(kin::Transform2D{{9.0f, 9.0f}});
    assert(scene.ecs().count<kin::ScriptOwned>() == 1);

    write_text(script, R"lua(
function on_load(scene)
    scene:entity("new"):set_transform(3, 4):set_rect(5, 6, 40, 50, 60)
    scene:entity("line"):set_transform(0, 0):set_line(0, 0, 1, 1, 70, 80, 90)
end
)lua");
    advance_write_time(script, 2);

    scene.update(fixture.ctx);
    assert(scene.script_loaded());
    assert(scene.ecs().count<kin::ScriptOwned>() == 2);
    assert(scene.ecs().count<kin::LineRenderer>() == 1);
    assert(scene.ecs().count<kin::Transform2D>() == 3);
}

void test_failed_reload_keeps_previous_script(Fixture& fixture) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-scripting-tests-failed-reload";
    std::filesystem::create_directories(dir);
    const std::filesystem::path script = dir / "scene.lua";
    write_text(script, R"lua(
function on_load(scene)
    scene:entity("stable"):set_transform(1, 1):set_rect(2, 2, 10, 20, 30)
end
)lua");

    kin::ScriptScene scene = make_scene(dir, "scene.lua", true);
    assert(scene.script_loaded());
    assert(scene.ecs().count<kin::ScriptOwned>() == 1);

    write_text(script, "function on_load(scene)\n  this is not lua\nend\n");
    advance_write_time(script, 2);
    scene.update(fixture.ctx);

    assert(!scene.last_script_error().empty());
    assert(scene.ecs().count<kin::ScriptOwned>() == 1);
    assert(scene.ecs().count<kin::RectRenderer>() == 1);
}

} // namespace

int main() {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    Fixture fixture;
    test_load_update_actions_and_spawn(fixture);
    test_input_query_can_quit_app(fixture);
    test_lua_can_query_and_mutate_existing_ecs_entities();
    test_lua_can_use_registered_custom_components();
    test_lua_can_register_query_plan_system();
    test_lua_component_helper_commits_data_component();
    test_lua_system_can_queue_command_buffer_mutations();
    test_lua_timer_helpers_and_interval_systems(fixture);
    test_lua_timer_state_survives_script_hot_reload(fixture);
    test_lua_require_loads_project_modules_and_hot_reloads_dependency(fixture);
    test_failed_module_reload_keeps_previous_script(fixture);
    test_lua_require_reports_missing_and_rejects_outside_paths();
    test_lua_system_uses_required_module_after_reload(fixture);
    test_lua_system_reports_invalid_data_component_patch();
    test_reload_replaces_script_entities(fixture);
    test_failed_reload_keeps_previous_script(fixture);
    return 0;
}
