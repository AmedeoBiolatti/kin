#include <kin/ecs/render.hpp>
#include <kin/ecs/component_schema.hpp>
#include <kin/scene/scene_asset.hpp>
#include <kin/scene/scene_asset_runtime.hpp>
#include <kin/scripting/script_scene.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

void write_prefab_file(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << R"json(
{
  "schema": "kin.prefab/1",
  "id": "forge",
  "root": "root",
  "entities": [
    {
      "id": "root",
      "name": "Forge",
      "components": {
        "Transform2D": {
          "pos": { "x": 0, "y": 0 },
          "rotation": 0
        }
      }
    },
    {
      "id": "core",
      "name": "Core",
      "parent": "root",
      "components": {
        "Transform2D": {
          "pos": { "x": 2, "y": 3 },
          "rotation": 0
        },
        "RectRenderer": {
          "size": { "x": 8, "y": 9 },
          "color": { "r": 10, "g": 20, "b": 30, "a": 255 },
          "layer": 1,
          "order": 2,
          "visible": true
        }
      }
    }
  ]
}
)json";
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

void advance_write_time(const std::filesystem::path& path, int seconds) {
    const auto current = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, current + std::chrono::seconds(seconds));
}

void write_runtime_scene_file(const std::filesystem::path& path, float x, std::string_view component = "Transform2D") {
    write_text(path, std::string{R"json(
{
  "schema": "kin.scene/1",
  "name": "HotReloadScene",
  "script": "scripts/main.lua",
  "entities": [
    {
      "id": "thing",
      "name": "Thing",
      "components": {
        ")json"} + std::string{component} + R"json(": {
          "pos": { "x": )json" + std::to_string(x) + R"json(, "y": 2 },
          "rotation": 0
        }
      }
    }
  ],
  "prefabs": []
}
)json");
}

void write_runtime_prefab_file(const std::filesystem::path& path, float w, float h) {
    write_text(path, std::string{R"json(
{
  "schema": "kin.prefab/1",
  "id": "forge",
  "root": "root",
  "entities": [
    {
      "id": "root",
      "name": "Forge",
      "components": {
        "Transform2D": {
          "pos": { "x": 0, "y": 0 },
          "rotation": 0
        }
      }
    },
    {
      "id": "core",
      "name": "Core",
      "parent": "root",
      "components": {
        "RectRenderer": {
          "size": { "x": )json"} + std::to_string(w) + R"json(, "y": )json" + std::to_string(h) + R"json( },
          "color": { "r": 10, "g": 20, "b": 30, "a": 255 },
          "layer": 1,
          "order": 2,
          "visible": true
        }
      }
    }
  ]
}
)json");
}

void write_runtime_prefab_scene_file(const std::filesystem::path& path) {
    write_text(path, R"json(
{
  "schema": "kin.scene/1",
  "name": "HotReloadPrefabScene",
  "script": "scripts/main.lua",
  "entities": [],
  "prefabs": [
    {
      "id": "main_forge",
      "asset": "prefabs/forge.kinprefab",
      "name": "Main Forge",
      "overrides": {
        "core": {
          "RectRenderer": {
            "visible": false
          }
        }
      }
    }
  ]
}
)json");
}

void test_scene_asset_instantiates_entities_and_prefabs() {
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "kin-scene-asset-tests";
    const std::filesystem::path prefab_path = root / "prefabs" / "forge.kinprefab";
    write_prefab_file(prefab_path);

    const std::string scene_json = R"json(
{
  "schema": "kin.scene/1",
  "name": "SceneOne",
  "script": "scripts/main.lua",
  "clear_color": [1, 2, 3],
  "entities": [
    {
      "id": "floor",
      "name": "Floor",
      "components": {
        "Transform2D": {
          "pos": { "x": 4, "y": 5 },
          "rotation": 0
        },
        "RectRenderer": {
          "size": { "x": 100, "y": 12 },
          "color": { "r": 40, "g": 50, "b": 60, "a": 255 },
          "layer": 1,
          "order": 0,
          "visible": true
        }
      }
    }
  ],
  "prefabs": [
    {
      "id": "main_forge",
      "asset": "prefabs/forge.kinprefab",
      "name": "Main Forge",
      "parent": "floor",
      "overrides": {
        "core": {
          "RectRenderer": {
            "visible": false,
            "size": { "x": 16, "y": 18 }
          }
        }
      }
    }
  ]
}
)json";

    kin::SceneLoadResult loaded = kin::parse_scene_asset(scene_json, world.components());
    assert(loaded.ok());
    assert(loaded.scene->name == "SceneOne");
    assert(loaded.scene->prefabs.size() == 1);

    kin::SceneInstance instance = kin::instantiate_scene_asset(world, *loaded.scene, root, world.components());
    assert(instance.ok());
    assert(instance.by_authored_id.at("floor"));
    assert(instance.by_authored_id.at("main_forge"));
    assert(instance.by_authored_id.at("main_forge.core"));

    kin::EcsEntity forge = instance.by_authored_id.at("main_forge");
    kin::EcsEntity core = instance.by_authored_id.at("main_forge.core");
    assert(forge.parent().id() == instance.by_authored_id.at("floor").id());
    assert(world.entities().find_by_authored_id("main_forge.core").id() == core.id());

    const kin::PrefabInstanceComponent* instance_metadata = forge.get<kin::PrefabInstanceComponent>();
    assert(instance_metadata != nullptr);
    assert(instance_metadata->instance_id == "main_forge");
    assert(instance_metadata->prefab_id == "forge");

    const kin::PrefabEntityComponent* entity_metadata = core.get<kin::PrefabEntityComponent>();
    assert(entity_metadata != nullptr);
    assert(entity_metadata->authored_entity_id == "core");

    const kin::RectRenderer* rect = core.get<kin::RectRenderer>();
    assert(rect != nullptr);
    assert(!rect->visible);
    assert((rect->size == kin::Vec2f{16.0f, 18.0f}));

    std::ostringstream out;
    kin::write_scene_asset(out, *loaded.scene);
    kin::SceneLoadResult reloaded = kin::parse_scene_asset(out.str(), world.components());
    assert(reloaded.ok());
    assert(reloaded.scene->prefabs[0].id == "main_forge");
}

void test_scene_validation_rejects_bad_component() {
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);

    const std::string scene_json = R"json(
{
  "schema": "kin.scene/1",
  "entities": [
    {
      "id": "bad",
      "components": {
        "Missing": {}
      }
    }
  ]
}
)json";
    kin::SceneLoadResult loaded = kin::parse_scene_asset(scene_json, world.components());
    assert(!loaded.ok());
    assert(!loaded.diagnostics.empty());
}

void test_prefab_scene_asset_instantiates_after_script_owned_component() {
    kin::EcsWorld world;
    kin::register_builtin_editor_components(world);

    const std::filesystem::path repo_root = std::filesystem::path{__FILE__}.parent_path().parent_path();
    // A scene with a data component, a script reference and a prefab instance.
    const std::filesystem::path root = repo_root / "tests" / "fixtures" / "scene_with_prefab" / "assets";
    kin::ComponentSchemaLoadResult schemas = kin::load_component_schema_asset(root / "components" / "gameplay.kincomponents");
    assert(schemas.ok());
    std::vector<std::string> schema_diagnostics;
    assert(kin::apply_component_schema_asset(world.components(), *schemas.document, schema_diagnostics));
    kin::SceneLoadResult loaded = kin::load_scene_asset(root / "scenes" / "main.kinscene", world.components());
    assert(loaded.ok());

    kin::SceneInstance instance = kin::instantiate_scene_asset(world, *loaded.scene, root, world.components());
    world.component<kin::ScriptOwned>("ScriptOwned");
    assert(instance.ok());
    assert(world.entities().find_by_authored_id("main_prefab"));
    assert(world.entities().find_by_authored_id("main_prefab.core"));
}

void test_scene_asset_runtime_reloads_scene_and_keeps_old_on_invalid() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "kin-scene-runtime-tests-scene";
    const std::filesystem::path scene_path = root / "scenes" / "main.kinscene";
    write_runtime_scene_file(scene_path, 1.0f);
    write_text(root / "scripts" / "main.lua", R"lua(
function on_load(scene)
    assert(scene:authored("thing") ~= nil)
end
)lua");

    kin::EcsWorld validation_world;
    kin::register_builtin_editor_components(validation_world);
    auto runtime = std::make_shared<kin::SceneAssetRuntime>();
    assert(runtime->load(scene_path, root, validation_world.components()));

    kin::ScriptScene script_scene{{
        .asset_root = root,
        .script_path = "scripts/main.lua",
        .name = "RuntimeReloadScene",
        .before_load = [runtime](kin::ScriptScene& scene) {
            kin::register_builtin_editor_components(scene.ecs());
            assert(runtime->instantiate(scene.ecs()));
        },
        .hot_reload = false,
    }};

    kin::EcsEntity thing = script_scene.ecs().entities().find_by_authored_id("thing");
    assert(thing);
    assert(thing.get<kin::Transform2D>()->pos.x == 1.0f);

    write_runtime_scene_file(scene_path, 9.0f);
    advance_write_time(scene_path, 2);
    assert(runtime->reload_if_changed(script_scene));
    thing = script_scene.ecs().entities().find_by_authored_id("thing");
    assert(thing);
    assert(thing.get<kin::Transform2D>()->pos.x == 9.0f);

    write_runtime_scene_file(scene_path, 11.0f, "MissingComponent");
    advance_write_time(scene_path, 2);
    assert(!runtime->reload_if_changed(script_scene));
    thing = script_scene.ecs().entities().find_by_authored_id("thing");
    assert(thing);
    assert(thing.get<kin::Transform2D>()->pos.x == 9.0f);
}

void test_scene_asset_runtime_reloads_prefab_and_preserves_overrides() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "kin-scene-runtime-tests-prefab";
    const std::filesystem::path scene_path = root / "scenes" / "main.kinscene";
    const std::filesystem::path prefab_path = root / "prefabs" / "forge.kinprefab";
    write_runtime_prefab_file(prefab_path, 8.0f, 9.0f);
    write_runtime_prefab_scene_file(scene_path);
    write_text(root / "scripts" / "main.lua", R"lua(
function on_load(scene)
    assert(scene:authored("main_forge") ~= nil)
    assert(scene:authored("main_forge.core") ~= nil)
end
)lua");

    kin::EcsWorld validation_world;
    kin::register_builtin_editor_components(validation_world);
    auto runtime = std::make_shared<kin::SceneAssetRuntime>();
    assert(runtime->load(scene_path, root, validation_world.components()));

    kin::ScriptScene script_scene{{
        .asset_root = root,
        .script_path = "scripts/main.lua",
        .name = "RuntimePrefabReloadScene",
        .before_load = [runtime](kin::ScriptScene& scene) {
            kin::register_builtin_editor_components(scene.ecs());
            assert(runtime->instantiate(scene.ecs()));
        },
        .hot_reload = false,
    }};

    kin::EcsEntity core = script_scene.ecs().entities().find_by_authored_id("main_forge.core");
    assert(core);
    assert((core.get<kin::RectRenderer>()->size == kin::Vec2f{8.0f, 9.0f}));
    assert(!core.get<kin::RectRenderer>()->visible);

    write_runtime_prefab_file(prefab_path, 12.0f, 14.0f);
    advance_write_time(prefab_path, 2);
    assert(runtime->reload_if_changed(script_scene));
    core = script_scene.ecs().entities().find_by_authored_id("main_forge.core");
    assert(core);
    assert((core.get<kin::RectRenderer>()->size == kin::Vec2f{12.0f, 14.0f}));
    assert(!core.get<kin::RectRenderer>()->visible);
}

} // namespace

int main() {
    test_scene_asset_instantiates_entities_and_prefabs();
    test_scene_validation_rejects_bad_component();
    test_prefab_scene_asset_instantiates_after_script_owned_component();
    test_scene_asset_runtime_reloads_scene_and_keeps_old_on_invalid();
    test_scene_asset_runtime_reloads_prefab_and_preserves_overrides();
    return 0;
}
