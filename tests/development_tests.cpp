#include <cassert>
#include <kin/renderer/renderer2d.hpp>
#include <chrono>
#include <fstream>
#include <kin/core/json.hpp>
#include <kin/ecs/render.hpp>
#include <kin/renderer/shader_compiler.hpp>
#include <kin/runtime/development.hpp>
#include <kin/scene/live_scene2d.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/scene/scene_manager.hpp>
#include <kin/scripting/script_reload.hpp>
#include <kin/scripting/script_scene.hpp>
#include <sol/sol.hpp>
#include <sstream>
#include <thread>

namespace {
using namespace kin;
struct Scratch {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("kin-development-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(root); }
    ~Scratch() {
        std::error_code e;
        std::filesystem::remove_all(root, e);
    }
    void write(std::string_view name, std::string_view text) {
        std::ofstream file(root / name, std::ios::binary);
        file << text;
        assert(file.good());
    }
};
JsonValue json(std::string_view text) {
    auto value = parse_json(text);
    assert(value.ok());
    return std::move(*value.value);
}
JsonValue status(const DevelopmentSession& session) {
    std::ostringstream out;
    JsonWriter writer(out, false);
    session.write_status(writer);
    return json(out.str());
}
void coordinator() {
    Scratch scratch;
    scratch.write("value.txt", "one");
    DevelopmentSession session{scratch.root};
    int a = 1, b = 1;
    std::string error;
    session.add_target("A", [&](const DevelopmentRevision& revision, std::string&) {
        const int desired = revision.content->files().at("value.txt") == "two" ? 2 : 3;
        const int old = a;
        return std::make_unique<DevelopmentChange>(DevelopmentChange{[&, desired](std::string&) {
                                                                         a = desired;
                                                                         return true;
                                                                     },
                                                                     [&, old] { a = old; }});
    });
    session.add_target("B", [&](const DevelopmentRevision& revision, std::string& failure) {
        if (revision.content->files().contains("reject.txt")) {
            failure = "bad dependency";
            return std::unique_ptr<DevelopmentChange>{};
        }
        const bool fail = revision.content->files().contains("activate.txt");
        const int old = b;
        return std::make_unique<DevelopmentChange>(DevelopmentChange{[&, fail](std::string& e) {
                                                                         b = 2;
                                                                         e = "cannot activate";
                                                                         return !fail;
                                                                     },
                                                                     [&, old] { b = old; }});
    });
    const auto initial = session.active().id;
    assert(session.request(json(R"({"value.txt":"two"})"), error));
    assert(a == 1 && b == 1 && session.generation() == 0);
    auto edits = session.pending_changes();
    assert(edits.find("value.txt")->is_object());
    session.advance(1);
    assert(a == 2 && b == 2 && session.generation() == 1 && session.active().id != initial);
    session.advance(1, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    session.advance(1, true);
    assert(session.generation() == 1); // an unchanged disk cannot undo virtual edits
    assert(session.request(json(R"({"reject.txt":"bad"})"), error));
    session.advance(2);
    assert(a == 2 && b == 2 && session.generation() == 1);
    assert(session.request(json(R"({"value.txt":"three","activate.txt":"fail"})"), error));
    session.advance(3);
    assert(a == 2 && b == 2 && session.generation() == 1);
    assert(!session.request(json(R"({"../escape":"x"})"), error));
    assert(!session.request(json(R"({"value.txt":{"hex":"xx"}})"), error));
    scratch.write(".kin-import-publishing", "pending");
    assert(!session.request(error));
    assert(session.generation() == 1);
    std::filesystem::remove(scratch.root / ".kin-import-publishing");
    // The same explicit edits replay into the same revision fingerprint.
    DevelopmentSession replay{scratch.root};
    assert(replay.request(edits, error));
    replay.advance(1);
    assert(replay.active().id == session.active().id);
}
struct Stats {
    inline static u32 next_token = 1;
    i32 hp = 100;
    std::vector<int> runtime{7, 8};
    u32 token = next_token++;
};
struct Obsolete {
    i32 marker = 1;
};
void patch_scene() {
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    world.components().native<Stats>("Stats").field("hp", &Stats::hp).range(0, 1000);
    world.components().native<Obsolete>("Obsolete");
    SceneDocument before{{{.id = "player",
                           .name = "Player",
                           .components = {{"Transform2D", {{"pos", ComponentFieldKind::Vec2f, Vec2f{1, 2}}}},
                                          {"Stats", {{"hp", ComponentFieldKind::I32, i32{100}}}},
                                          {"Obsolete", {}}}}}};
    std::string error;
    assert(instantiate_scene(world, before, error));
    auto player = world.entities().find_by_authored_id("player");
    const auto id = player.id();
    player.get_mut<Transform2D>()->pos = {20, 30};
    player.get_mut<Stats>()->hp = 50;
    player.get_mut<Stats>()->runtime.push_back(9);
    auto after = before;
    after.entities[0].name = "Renamed";
    after.entities[0].components[1].fields[0].value = i32{200};
    after.entities[0].components.pop_back();
    after.entities.push_back({.id = "child",
                              .name = "Child",
                              .parent = "player",
                              .components = {{"Transform2D", {{"pos", ComponentFieldKind::Vec2f, Vec2f{3, 4}}}}}});
    auto plan = prepare_scene_patch2d(world, before, after, error);
    assert(plan);
    assert(player.get<Stats>()->hp == 50);
    assert(plan->activate(error));
    assert(player.id() == id && player.get<Stats>()->hp == 200);
    assert(player.get<Transform2D>()->pos == Vec2f(20, 30));
    assert(player.get<Stats>()->runtime == std::vector<int>({7, 8, 9}));
    assert(!player.has<Obsolete>() && world.entities().find_by_authored_id("child").parent().id() == player.id());
    plan->rollback();
    assert(player.get<Stats>()->hp == 50 && player.has<Obsolete>() && !world.entities().find_by_authored_id("child"));
    assert(player.name() == "Player" && player.get<Stats>()->runtime.size() == 3);
    plan = prepare_scene_patch2d(world, before, after, error);
    assert(plan && plan->activate(error));
    plan->finalize();
    auto invalid = after;
    invalid.entities[0].components[1].fields[0].value = i32{-1};
    assert(!prepare_scene_patch2d(world, after, invalid, error));
    assert(player.get<Stats>()->hp == 200);
    auto runtime = world.entities().create({.name = "Runtime child", .parent = player});
    auto removed = prepare_scene_patch2d(world, after, {}, error);
    assert(removed && removed->activate(error));
    removed->finalize();
    assert(runtime.alive() && !runtime.parent() && !player.alive());
}
void scripts() {
    Scratch scratch;
    const std::string initial = R"lua(
local amount=require('amount')
reload_state={count=5.25}
local function system(scene, amount)
    assert(scene:register_system('reload.counter',{all={'Stats'},reads={'Stats'},writes={'Stats'}},function(e)
        local stats=e:component('Stats');stats.hp=stats.hp+amount;stats:commit()
    end))
end
function on_load(scene) system(scene,1) end
function on_reload(saved, scene) assert(saved.count>=6.25 and saved.count%1==.25);reload_state=saved;system(scene,amount) end
function update(ctx) reload_state.count=reload_state.count+1;effect() end
)lua";
    scratch.write("game.lua", initial);
    scratch.write("amount.lua", "return 2");
    kin::EcsWorld world;
    world.components().native<Stats>("Stats").field("hp", &Stats::hp);
    auto player = world.entities().create({.name = "Player"});
    player.set(Stats{});
    int effects = 0;
    auto active = std::make_unique<ScriptEngine>([&](sol::state& lua) { lua["effect"] = [&] { ++effects; }; });
    assert(active->load_file(scratch.root, "game.lua"));
    assert(active->call_on_load({.world = &world}));
    assert(active->call_update(ScriptFrameContext{.dt = .1f}));
    assert(effects == 1 && world.run_frame(.1f));
    assert(player.get<Stats>()->hp == 101);
    DevelopmentSession session{scratch.root};
    session.add_target("Lua", [&](const DevelopmentRevision& revision, std::string& error) {
        return prepare_script_reload(active, revision, "game.lua", error);
    });
    std::string error;
    assert(session.request(json(R"({})"), error));
    session.advance(1);
    assert(session.generation() == 1 && effects == 1);
    assert(world.run_frame(.1f));
    assert(player.get<Stats>()->hp == 103);
    assert(active->call_update(ScriptFrameContext{}));
    assert(effects == 2);
    const auto reject = [&](std::string code, std::string_view message) {
        assert(session.request(JsonValue{JsonValue::Object{{"game.lua", JsonValue{std::move(code)}}}}, error));
        session.advance(2);
        assert(session.generation() == 1 && session.error().find(message) != std::string::npos);
        assert(world.run_frame(.1f));
    };
    reject("function on_reload(saved, scene) scene:entity('bad') end", "unavailable");
    assert(!world.lookup("bad"));
    reject("function on_reload(saved) effect() end", "nil");
    assert(effects == 2);
    reject("while true do end", "budget");
    reject("function broken(", "expected");
    assert(active->call_update(ScriptFrameContext{}));
    assert(effects == 3);
    const auto hp = player.get<Stats>()->hp;
    assert(session.request(json(R"({"amount.lua":"return 3"})"), error));
    session.advance(3);
    assert(session.generation() == 2);
    assert(world.run_frame(.1f));
    assert(player.get<Stats>()->hp == hp + 3);
    active.reset(); // Lua references in stored system definitions close before the VM.
}
void prefabs() {
    Scratch scratch;
    const std::string scene = R"({"schema":"kin.scene/1","prefabs":[{"id":"one","asset":"unit.kinprefab"}]})";
    const std::string before =
        R"({"schema":"kin.prefab/1","id":"unit","root":"root","entities":[{"id":"root","components":{"Transform2D":{"pos":[1,2]},"Stats":{"hp":100},"RectRenderer":{"size":[4,5]}}}]})";
    const std::string after =
        R"({"schema":"kin.prefab/1","id":"unit","root":"root","entities":[{"id":"root","components":{"Transform2D":{"pos":[1,2]},"Stats":{"hp":200}}},{"id":"child","parent":"root","components":{"Transform2D":{}}}]})";
    scratch.write("scene.kinscene", scene);
    scratch.write("unit.kinprefab", before);
    kin::EcsWorld world;
    register_builtin_editor_components(world);
    world.components().native<Stats>("Stats").field("hp", &Stats::hp).field("token", &Stats::token).readonly();
    SceneAssetRuntime runtime;
    assert(runtime.load(scratch.root / "scene.kinscene", scratch.root, world.components()));
    assert(runtime.instantiate(world));
    auto player = runtime.instance().prefab_instances.at("one").by_authored_id.at("root");
    const auto id = player.id();
    const auto token = player.get<Stats>()->token;
    player.get_mut<Transform2D>()->pos = {7, 8};
    player.get_mut<Stats>()->hp = 50;
    player.get_mut<Stats>()->runtime.push_back(9);
    auto content = MemoryContent::create({{"scene.kinscene", scene}, {"unit.kinprefab", after}});
    std::string error;
    // Loads keep no authored copy unless asked: a patch needs one.
    assert(!runtime.tracks_authored() && !runtime.prepare_patch(world, content->root(), error));
    assert(runtime.track_authored(world.components()) && runtime.tracks_authored());
    auto plan = runtime.prepare_patch(world, content->root(), error);
    if (!plan)
        std::fprintf(stderr, "prefab: %s\n", error.c_str());
    assert(plan);
    assert(plan->activate(error));
    assert(player.get<Stats>()->hp == 200 && !player.has<RectRenderer>());
    plan->rollback();
    assert(player.get<Stats>()->hp == 50 && player.has<RectRenderer>());
    assert(player.raw().target(flecs::IsA));
    plan = runtime.prepare_patch(world, content->root(), error);
    assert(plan && plan->activate(error));
    plan->finalize();
    plan.reset();
    assert(runtime.instance().prefab_instances.at("one").by_authored_id.at("root").id() == id);
    assert(runtime.instance().prefab_instances.at("one").by_authored_id.at("child").parent().id() == id);
    assert(player.get<Transform2D>()->pos == Vec2f(7, 8) && player.get<Stats>()->runtime.size() == 3);
    assert(player.get<Stats>()->token == token);
}
void shaders() {
    if (!shader_compiler_available())
        return;
    auto content = MemoryContent::create(
        {{"shaders/main.frag", "#version 450\n#extension GL_GOOGLE_include_directive : require\n#include "
                               "\"color.inc\"\nlayout(location=0) out vec4 color;void main(){color=VALUE;}\n"},
         {"shaders/color.inc", "#define VALUE vec4(1.0)\n"}});
    std::string error;
    auto compiled = compile_glsl(content->root() / "shaders/main.frag", ShaderStage::Fragment, &error);
    if (!compiled)
        std::fprintf(stderr, "shader: %s\n", error.c_str());
    assert(compiled && !compiled->empty());
}
void shared_scene() {
    Scratch scratch;
    const std::string script = "reload_state={counter=1}\nfunction on_reload(s) assert(s.counter>=1);reload_state=s "
                               "end\nfunction update(ctx) reload_state.counter=reload_state.counter+1 end\n";
    const std::string source =
        R"({"schema":"kin.scene/1","name":"Original","script":"game.lua","entities":[{"id":"player","components":{"Transform2D":{"pos":[1,2]},"Stats":{"hp":100}}}]})";
    const std::string edited =
        R"({"schema":"kin.scene/1","name":"Edited","script":"new.lua","entities":[{"id":"player","components":{"Transform2D":{"pos":[1,2]},"Stats":{"hp":200}}}]})";
    scratch.write("game.lua", script);
    scratch.write("scene.kinscene", source);
    App app{{.mode = AppMode::Headless}};
    auto& window = app.create_window({.width = 64, .height = 64, .hidden = true});
    Renderer2D renderer{window};
    SceneManager scenes;
    auto runtime = std::make_shared<SceneAssetRuntime>();
    ScriptScene scene{
        {.asset_root = scratch.root,
         .script_path = "game.lua",
         .before_load =
             [&](ScriptScene& host) {
                 register_builtin_editor_components(host.ecs());
                 host.ecs()
                     .components()
                     .native<Stats>("Stats")
                     .field("hp", &Stats::hp)
                     .field("token", &Stats::token)
                     .readonly();
                 assert(runtime->load(scratch.root / "scene.kinscene", scratch.root, host.ecs().components()));
                 assert(runtime->instantiate(host.ecs()));
             },
         .hot_reload = false,
         .render_enabled = false,
         .scene_runtime = runtime}};
    SceneContext ctx{.app = app,
                     .window = window,
                     .renderer = renderer,
                     .input = app.input(),
                     .scenes = scenes,
                     .dt = .1f,
                     .development = true};
    scene.on_enter(ctx);
    auto* development = scene.development_session();
    assert(development);
    auto player = scene.ecs().entities().find_by_authored_id("player");
    const auto id = player.id();
    player.get_mut<Transform2D>()->pos = {50, 60};
    std::string error;
    assert(development->request(
        JsonValue{JsonValue::Object{{"scene.kinscene", JsonValue{edited}}, {"new.lua", JsonValue{script}}}}, error));
    development->advance(1);
    if (development->generation() != 1)
        std::fprintf(stderr, "shared scene: %s\n", development->error().c_str());
    assert(development->generation() == 1 && scene.config().script_path == "new.lua");
    assert(player.id() == id && player.get<Stats>()->hp == 200 && player.get<Transform2D>()->pos == Vec2f(50, 60));
    assert(runtime->scene()->name == "Edited");
    scene.update(ctx);
    assert(scene.last_script_error().empty());
    scratch.write("scene.kinscene", "invalid");
    assert(!runtime->reload_if_changed(scene));
    assert(player.alive());
    assert(scene.reload_script() && development->pending());
    development->advance(2);
    assert(development->generation() == 1 && player.get<Stats>()->hp == 200);
    scratch.write("scene.kinscene", source);
    assert(scene.reload_script());
    development->advance(3);
    assert(development->generation() == 2 && scene.config().script_path == "game.lua");
    assert(player.id() == id && player.get<Stats>()->hp == 100 && player.get<Transform2D>()->pos == Vec2f(50, 60));
    scene.on_exit(ctx);
}
void disk_watch() {
    Scratch scratch;
    scratch.write("value.txt", "old");
    DevelopmentSession session{scratch.root};
    int applied = 0;
    session.add_target("value", [&](const DevelopmentRevision&, std::string&) {
        return std::make_unique<DevelopmentChange>(DevelopmentChange{[&](std::string&) {
                                                                         ++applied;
                                                                         return true;
                                                                     },
                                                                     [] {}});
    });
    // The watcher compares sizes and times, as FileWatcher does.
    scratch.write("value.txt", "newer");
    session.advance(1, true);
    assert(applied == 0); // seen once: it may still be being written
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    session.advance(2, true);
    assert(applied == 1 && session.generation() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    session.advance(3, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    session.advance(4, true);
    assert(applied == 1); // an unchanged folder does not reload again
}
} // namespace
int main() {
    coordinator();
    patch_scene();
    prefabs();
    scripts();
    shaders();
    shared_scene();
    disk_watch();
}
