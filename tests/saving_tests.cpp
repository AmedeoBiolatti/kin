// Saving (kin/ecs/saving.hpp): entities marked Saved keep the components they
// name while their scene is left and entered again; destroyed ones stay
// destroyed, prefab instances made while it ran are made again; the game's
// own values and the scenes kept go into a slot and come back from it; a
// script's values go through the hooks it gives.

#include <kin/ecs/render.hpp>
#include <kin/ecs/saving.hpp>
#include <kin/ecs/world.hpp>
#include <kin/prefab/prefab_asset.hpp>
#include <kin/prefab/prefab_registry.hpp>
#include <kin/scene/scene_asset.hpp>
#if KIN_ENABLE_PHYSICS
#include <kin/ecs/physics.hpp>
#endif

#include <cassert>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace kin;
namespace fs = std::filesystem;

constexpr const char* scene_path = "scenes/field.kinscene";

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

bool near(Vec2f a, Vec2f b) {
    return std::abs(a.x - b.x) < 1e-3f && std::abs(a.y - b.y) < 1e-3f;
}

// The field scene: a hero that keeps where it is (and its colour), two coins
// that keep only that they are gone, a rock not kept, and a placed barrel (a
// prefab instance) that keeps where it is.
SceneInstance read_field(kin::EcsWorld& world, const fs::path& content) {
    register_builtin_editor_components(world);
    std::ifstream in(content / scene_path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const SceneLoadResult read = parse_scene_asset(text, world.components());
    assert(read.ok());
    SceneInstance made = instantiate_scene_asset(world, *read.scene, content, world.components());
    assert(made.ok());
    return made;
}

fs::path make_content(const fs::path& dir) {
    fs::remove_all(dir);
    write(dir / scene_path, R"({
  "schema": "kin.scene/1",
  "entities": [
    {"id": "hero", "name": "Hero", "components": {
      "Transform2D": {"pos": {"x": 10, "y": 20}},
      "RectRenderer": {"size": {"x": 8, "y": 8}},
      "Saved": {"keep": "Transform2D, RectRenderer", "script": false}}},
    {"id": "coin_1", "name": "Coin", "components": {"Transform2D": {"pos": {"x": 50, "y": 0}}, "Saved": {"keep": ""}}},
    {"id": "coin_2", "name": "Coin", "components": {"Transform2D": {"pos": {"x": 60, "y": 0}}, "Saved": {"keep": ""}}},
    {"id": "rock", "name": "Rock", "components": {"Transform2D": {"pos": {"x": 1, "y": 1}}}}
  ],
  "prefabs": [{"id": "barrel", "asset": "prefabs/barrel.kinprefab", "pos": {"x": 100, "y": 100}}]
})");
    write(dir / "prefabs/barrel.kinprefab", R"({"schema": "kin.prefab/1", "id": "barrel", "root": "barrel", "entities": [
      {"id": "barrel", "name": "Barrel", "components": {"Transform2D": {}, "Saved": {}}}]})");
    write(dir / "prefabs/crate.kinprefab", R"({"schema": "kin.prefab/1", "id": "crate", "root": "crate", "entities": [
      {"id": "crate", "name": "Crate", "components": {"Transform2D": {}, "Saved": {}}}]})");
    return dir;
}

void test_saved_components() {
    assert((saved_components({.keep = " Transform2D ,Health,, Transform2D "}) == std::vector<std::string>{"Transform2D", "Health"}));
    assert(saved_components({.keep = ""}).empty());
}

void test_scenes_kept_and_slots() {
    const fs::path content = make_content(fs::temp_directory_path() / "kin_saving_test");
    const fs::path store = content / "store";
    SceneSaves saves{{.game_id = "saving_test", .game_version = "1", .root_override = store}, content};

    {
        kin::EcsWorld world;
        const SceneInstance made = read_field(world, content);
        saves.enter(world, scene_path);
        assert(saves.problems().empty() && !saves.kept(scene_path));
        // Played: the hero moved and turned red, a coin taken, the barrel
        // pushed, a crate made, the rock moved (not kept).
        kin::EcsEntity hero = made.by_authored_id.at("hero");
        hero.raw().get_mut<Transform2D>()->pos = {300.0f, 40.0f};
        hero.raw().get_mut<RectRenderer>()->color = Color{200, 10, 10, 255};
        world.entities().destroy(made.by_authored_id.at("coin_1"));
        const kin::EcsEntity barrel = world.entities().find_by_authored_id("barrel");
        assert(barrel && barrel.raw().has<Saved>()); // kept as the prefab marks it
        barrel.raw().get_mut<Transform2D>()->pos = {120.0f, 100.0f};
        made.by_authored_id.at("rock").raw().get_mut<Transform2D>()->pos = {9.0f, 9.0f};
        const PrefabLoadResult crate = load_prefab_asset(content / "prefabs/crate.kinprefab", world.components());
        assert(crate.prefab);
        const PrefabInstance spawned = instantiate_prefab(world, *crate.prefab, PrefabSpawn{.pos = {70.0f, 80.0f}}, world.components());
        assert(spawned.root);
        spawned.root.raw().set<SpawnedPrefab>({.prefab = "prefabs/crate.kinprefab"});
        saves.set_value("coins", JsonValue{1.0});
        saves.keep(world);
    }
    const JsonValue* kept = saves.kept(scene_path);
    assert(kept && kept->find("entities")->find("coin_1")->bool_at("gone"));
    assert(!kept->find("entities")->contains("coin_2") || !kept->find("entities")->find("coin_2")->bool_at("gone"));
    assert(!kept->find("entities")->contains("rock"));
    assert(kept->find("spawned") && kept->find("spawned")->items().size() == 1);

    // Entered again: as it was left.
    const auto check = [&](kin::EcsWorld& world, const SceneInstance& made) {
        for (const std::string& problem : saves.problems()) std::fprintf(stderr, "problem: %s\n", problem.c_str());
        assert(saves.problems().empty());
        const kin::EcsEntity hero = made.by_authored_id.at("hero");
        assert(near(hero.raw().get<Transform2D>()->pos, {300.0f, 40.0f}));
        assert(hero.raw().get<RectRenderer>()->color == (Color{200, 10, 10, 255}));
        assert(!world.entities().find_by_authored_id("coin_1"));
        assert(world.entities().find_by_authored_id("coin_2"));
        assert(near(made.by_authored_id.at("rock").raw().get<Transform2D>()->pos, {1.0f, 1.0f}));
        assert(near(world.entities().find_by_authored_id("barrel").raw().get<Transform2D>()->pos, {120.0f, 100.0f}));
        int crates = 0;
        world.raw().each([&](flecs::entity entity, const SpawnedPrefab& from) {
            assert(from.prefab == "prefabs/crate.kinprefab");
            assert(near(entity.get<Transform2D>()->pos, {70.0f, 80.0f}));
            ++crates;
        });
        assert(crates == 1);
    };
    {
        kin::EcsWorld world;
        const SceneInstance made = read_field(world, content);
        saves.enter(world, scene_path);
        check(world, made);

        // Written, with the game's values; the slot lists itself.
        const SaveResult written = saves.write(world, "slot_1", "In the field", "1 coin");
        assert(written.ok);
    }
    const std::vector<SaveSlotInfo> slots = saves.store().list_slots();
    assert(slots.size() == 1 && slots[0].slot_id == "slot_1" && slots[0].title == "In the field" && slots[0].summary == "1 coin");

    // A new run reads it back: the scene to read, then entered as it was.
    SceneSaves again{{.game_id = "saving_test", .game_version = "1", .root_override = store}, content};
    std::string error;
    assert(!again.read("no_such_slot", error) && !error.empty());
    error.clear();
    const std::optional<std::string> scene = again.read("slot_1", error);
    assert(scene && *scene == scene_path && error.empty());
    assert(again.value("coins") && again.value("coins")->as_number() == 1.0);
    {
        kin::EcsWorld world;
        const SceneInstance made = read_field(world, content);
        again.enter(world, *scene);
        assert(again.problems().empty());
        assert(!world.entities().find_by_authored_id("coin_1"));
        assert(near(made.by_authored_id.at("hero").raw().get<Transform2D>()->pos, {300.0f, 40.0f}));
    }

    // A kept component the entity lost is taken away again; a new game forgets.
    {
        kin::EcsWorld world;
        const SceneInstance made = read_field(world, content);
        again.enter(world, scene_path);
        made.by_authored_id.at("hero").raw().remove<RectRenderer>();
        again.keep(world);
    }
    {
        kin::EcsWorld world;
        const SceneInstance made = read_field(world, content);
        again.enter(world, scene_path);
        assert(!made.by_authored_id.at("hero").raw().has<RectRenderer>());
    }
    again.clear();
    assert(!again.kept(scene_path) && !again.value("coins"));

    // A read asked for is taken once.
    again.ask_read("slot_1");
    assert(again.take_read() == std::optional<std::string>("slot_1") && !again.take_read());
    fs::remove_all(content);
}

void test_script_hooks() {
    const fs::path content = make_content(fs::temp_directory_path() / "kin_saving_hooks_test");
    SceneSaves saves{{.game_id = "saving_hooks", .game_version = "1", .root_override = content / "store"}, content};
    std::vector<std::string> given;
    saves.set_scripts({
        .write = [](kin::EcsEntity entity) -> std::optional<JsonValue> {
            return JsonValue{JsonValue::Object{{"name", JsonValue{std::string(entity.raw().name().c_str())}}}};
        },
        .read = [&](kin::EcsEntity, const JsonValue& values) { given.push_back(values.string_at("name")); },
    });
    {
        kin::EcsWorld world;
        read_field(world, content);
        saves.enter(world, scene_path);
        saves.keep(world);
    }
    // The hero keeps no script values (script: false); the coins and the barrel do.
    const JsonValue& entities = *saves.kept(scene_path)->find("entities");
    assert(!entities.find("hero")->contains("script"));
    assert(entities.find("coin_2")->find("script")->string_at("name").starts_with("Coin"));
    {
        kin::EcsWorld world;
        read_field(world, content);
        saves.enter(world, scene_path);
    }
    assert(given.size() == 3); // two coins and the barrel
    fs::remove_all(content);
}

#if KIN_ENABLE_PHYSICS
// A body put back where it was kept goes there with it (teleported at the next step).
void test_bodies_teleported() {
    const fs::path content = make_content(fs::temp_directory_path() / "kin_saving_bodies_test");
    SceneSaves saves{{.game_id = "saving_bodies", .game_version = "1", .root_override = content / "store"}, content};
    {
        kin::EcsWorld world;
        read_field(world, content);
        saves.enter(world, scene_path);
        saves.keep(world);
    }
    kin::EcsWorld world;
    const SceneInstance made = read_field(world, content);
    made.by_authored_id.at("hero").raw().set(PhysicsBodyComponent{});
    saves.enter(world, scene_path);
    assert(made.by_authored_id.at("hero").raw().has<PhysicsTeleport>());
    fs::remove_all(content);
}
#endif

} // namespace

int main() {
    test_saved_components();
    test_scenes_kept_and_slots();
    test_script_hooks();
#if KIN_ENABLE_PHYSICS
    test_bodies_teleported();
#endif
    return 0;
}
