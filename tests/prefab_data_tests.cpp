#include <kin/ecs/render.hpp>
#include <kin/prefab/prefab_asset.hpp>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

void register_prefab_components(kin::EcsWorld& world) {
    world.components().native<kin::Transform2D>("Transform2D")
        .field("pos", &kin::Transform2D::pos)
        .field("rotation", &kin::Transform2D::rotation);
    world.components().native<kin::RectRenderer>("RectRenderer")
        .field("size", &kin::RectRenderer::size)
        .field("color", &kin::RectRenderer::color)
        .field("visible", &kin::RectRenderer::visible)
        .field("layer", &kin::RectRenderer::layer)
        .field("order", &kin::RectRenderer::order);
}

void register_prefab_data_components(kin::EcsWorld& world) {
    world.components().data("DataStats")
        .field_i32("hp", 10)
        .field_string("name", "default")
        .field_color("tint", kin::Color::rgba(1, 2, 3, 4))
        .field_entity_ref("target")
        .field_asset_ref("asset");
}

kin::PrefabAsset make_hierarchy_prefab() {
    return {
        .id = "forge_drone",
        .name = "Forge Drone",
        .root = "drone",
        .entities = {
            {
                .id = "drone",
                .name = "Drone",
                .components = {
                    {
                        .name = "Transform2D",
                        .fields = {
                            {.name = "pos", .value = kin::Vec2f{1.0f, 2.0f}},
                            {.name = "rotation", .value = kin::f32{0.0f}},
                        },
                    },
                },
            },
            {
                .id = "body",
                .name = "Body",
                .parent = "drone",
                .components = {
                    {
                        .name = "Transform2D",
                        .fields = {
                            {.name = "pos", .value = kin::Vec2f{3.0f, 4.0f}},
                        },
                    },
                    {
                        .name = "RectRenderer",
                        .fields = {
                            {.name = "size", .value = kin::Vec2f{16.0f, 10.0f}},
                            {.name = "color", .value = kin::Color::rgba(80, 180, 255, 255)},
                            {.name = "visible", .value = true},
                            {.name = "layer", .value = kin::i32{2}},
                        },
                    },
                },
            },
        },
    };
}

const kin::ComponentFieldSnapshot* find_field(const kin::ComponentSnapshot& snapshot, std::string_view name) {
    const auto found = std::ranges::find_if(snapshot.fields, [&](const kin::ComponentFieldSnapshot& field) {
        return field.name == name;
    });
    assert(found != snapshot.fields.end());
    return &*found;
}

bool has_event(const std::vector<kin::EcsEvent>& events, kin::EcsEventKind kind) {
    return std::ranges::any_of(events, [&](const kin::EcsEvent& event) {
        return event.kind == kind;
    });
}

void test_instantiates_hierarchy_and_applies_spawn() {
    kin::EcsWorld world;
    register_prefab_components(world);
    world.component<kin::WorldTransform>("WorldTransform");

    kin::EcsEntity parent = world.entity("fleet");
    kin::PrefabInstance instance = kin::instantiate_prefab(world,
                                                          make_hierarchy_prefab(),
                                                          {
                                                              .pos = {10.0f, 20.0f},
                                                              .name = "drone-a",
                                                              .parent = parent,
                                                          },
                                                          world.components());

    assert(instance.ok());
    assert(instance.root);
    assert(instance.root.name() == "drone-a");
    assert(instance.root.parent().id() == parent.id());
    assert(instance.entities.size() == 2);
    assert(instance.by_authored_id.size() == 2);
    assert(world.entities().registered(instance.root));
    assert(world.entities().authored_id(instance.root).value().empty());

    kin::EcsEntity body = instance.by_authored_id.at("body");
    assert(world.entities().registered(body));
    assert(body.parent().id() == instance.root.id());
    assert((instance.root.get<kin::Transform2D>()->pos == kin::Vec2f{10.0f, 20.0f}));
    assert((body.get<kin::Transform2D>()->pos == kin::Vec2f{3.0f, 4.0f}));
    assert((body.get<kin::RectRenderer>()->size == kin::Vec2f{16.0f, 10.0f}));

    kin::WorldRenderState render_state{world};
    render_state.propagate_transforms();
    assert((body.get<kin::WorldTransform>()->pos == kin::Vec2f{13.0f, 24.0f}));
}

void test_flecs_backend_inherits_prefab_component_shape() {
    kin::EcsWorld world;
    register_prefab_components(world);

    kin::PrefabInstance instance = kin::instantiate_prefab(world, make_hierarchy_prefab(), {}, world.components());

    assert(instance.ok());
    kin::EcsEntity body = instance.by_authored_id.at("body");
    assert(instance.root.has<kin::Transform2D>());
    assert(body.has<kin::Transform2D>());
    assert(body.has<kin::RectRenderer>());
    assert(instance.root.raw().target(flecs::IsA));
    assert(body.raw().target(flecs::IsA));
    assert((instance.root.get<kin::Transform2D>()->pos == kin::Vec2f{1.0f, 2.0f}));
    assert((body.get<kin::RectRenderer>()->size == kin::Vec2f{16.0f, 10.0f}));

    std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(body, "RectRenderer");
    assert(snapshot.has_value());
    assert((std::get<kin::Vec2f>(find_field(*snapshot, "size")->value) == kin::Vec2f{16.0f, 10.0f}));
}

void test_repeated_instantiation_and_authored_id_prefix() {
    kin::EcsWorld world;
    register_prefab_components(world);

    kin::PrefabAsset prefab = make_hierarchy_prefab();
    kin::PrefabInstance first = kin::instantiate_prefab(world, prefab, {}, world.components());
    kin::PrefabInstance second = kin::instantiate_prefab(world, prefab, {}, world.components());
    assert(first.ok());
    assert(second.ok());
    assert(first.root.id() != second.root.id());
    assert(world.entities().authored_id(first.root).value().empty());
    assert(world.entities().authored_id(second.root).value().empty());

    kin::PrefabInstance prefixed = kin::instantiate_prefab(world,
                                                          prefab,
                                                          {.authored_id_prefix = "instance-a"},
                                                          world.components());
    assert(prefixed.ok());
    assert(world.entities().find_by_authored_id("instance-a/drone").id() == prefixed.root.id());
    assert(world.entities().find_by_authored_id("instance-a/body").id() == prefixed.by_authored_id.at("body").id());

    const std::size_t before_failed = world.entities().all().size();
    kin::PrefabInstance duplicate = kin::instantiate_prefab(world,
                                                           prefab,
                                                           {.authored_id_prefix = "instance-a"},
                                                           world.components());
    assert(!duplicate.ok());
    assert(duplicate.entities.empty());
    assert(world.entities().all().size() == before_failed);
}

void test_prefab_cache_invalidates_when_asset_changes() {
    kin::EcsWorld world;
    register_prefab_components(world);
    kin::PrefabAsset prefab = make_hierarchy_prefab();

    kin::PrefabInstance first = kin::instantiate_prefab(world, prefab, {}, world.components());
    assert(first.ok());
    prefab.entities[1].components[1].fields[0].value = kin::Vec2f{48.0f, 20.0f};
    kin::PrefabInstance changed = kin::instantiate_prefab(world, prefab, {}, world.components());
    assert(changed.ok());
    assert((changed.by_authored_id.at("body").get<kin::RectRenderer>()->size == kin::Vec2f{48.0f, 20.0f}));
    assert((first.by_authored_id.at("body").get<kin::RectRenderer>()->size == kin::Vec2f{16.0f, 10.0f}));
}

void test_overrides_patch_fields() {
    kin::EcsWorld world;
    register_prefab_components(world);

    kin::PrefabOverrideSet overrides;
    overrides.entities["body"] = {
        {
            .name = "RectRenderer",
            .fields = {
                {.name = "visible", .value = false},
                {.name = "size", .value = kin::Vec2f{24.0f, 12.0f}},
            },
        },
    };

    kin::PrefabInstance instance = kin::instantiate_prefab(world,
                                                          make_hierarchy_prefab(),
                                                          {},
                                                          world.components(),
                                                          overrides);

    assert(instance.ok());
    const kin::RectRenderer* rect = instance.by_authored_id.at("body").get<kin::RectRenderer>();
    assert(rect != nullptr);
    assert(!rect->visible);
    assert((rect->size == kin::Vec2f{24.0f, 12.0f}));
}

void test_instantiation_emits_events_and_dirty_state() {
    kin::EcsWorld world;
    register_prefab_components(world);
    world.events().clear();

    kin::PrefabInstance instance = kin::instantiate_prefab(world, make_hierarchy_prefab(), {}, world.components());
    assert(instance.ok());

    const std::vector<kin::EcsEvent>& events = world.events().snapshot();
    assert(has_event(events, kin::EcsEventKind::EntityCreated));
    assert(has_event(events, kin::EcsEventKind::EntityReparented));
    assert(has_event(events, kin::EcsEventKind::ComponentAdded));
    assert(has_event(events, kin::EcsEventKind::ComponentChanged));
    assert(world.events().dirty_state().world_dirty);
    assert(world.events().dirty_state().hierarchy_dirty);
    assert(world.events().dirty_state().components_dirty);
}

void test_parse_and_save_round_trip() {
    kin::EcsWorld world;
    register_prefab_components(world);

    const std::string json = R"json(
{
  "schema": "kin.prefab/1",
  "id": "panel",
  "root": "root",
  "entities": [
    {
      "id": "root",
      "components": {
        "Transform2D": {
          "pos": { "x": 5, "y": 6 },
          "rotation": 0
        }
      }
    },
    {
      "id": "bar",
      "parent": "root",
      "components": {
        "RectRenderer": {
          "size": [10, 4],
          "color": { "r": 1, "g": 2, "b": 3, "a": 4 },
          "visible": true,
          "order": 3
        }
      }
    }
  ]
}
)json";

    kin::PrefabLoadResult loaded = kin::parse_prefab_asset(json, world.components());
    assert(loaded.ok());
    assert(loaded.prefab->id == "panel");
    assert(loaded.prefab->entities.size() == 2);
    assert(loaded.prefab->entities[1].parent == "root");

    std::ostringstream out;
    kin::write_prefab_asset(out, *loaded.prefab);
    kin::PrefabLoadResult reloaded = kin::parse_prefab_asset(out.str(), world.components());
    assert(reloaded.ok());
    assert(reloaded.prefab->id == loaded.prefab->id);
    assert(reloaded.prefab->entities.size() == loaded.prefab->entities.size());

    kin::PrefabInstance instance = kin::instantiate_prefab(world, *reloaded.prefab, {}, world.components());
    assert(instance.ok());
    const kin::RectRenderer* rect = instance.by_authored_id.at("bar").get<kin::RectRenderer>();
    assert(rect != nullptr);
    assert((rect->size == kin::Vec2f{10.0f, 4.0f}));
    assert(rect->color == kin::Color::rgba(1, 2, 3, 4));
    assert(rect->order == 3);
}

void test_load_and_save_file() {
    kin::EcsWorld world;
    register_prefab_components(world);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "kin-prefab-data-tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "forge_drone.kinprefab";
    assert(kin::save_prefab_asset(make_hierarchy_prefab(), path));

    kin::PrefabLoadResult loaded = kin::load_prefab_asset(path, world.components());
    assert(loaded.ok());
    assert(loaded.prefab->id == "forge_drone");
}

void test_validation_reports_errors() {
    kin::EcsWorld world;
    register_prefab_components(world);

    kin::PrefabAsset invalid{
        .id = "bad",
        .root = "a",
        .entities = {
            {.id = "a", .parent = "b"},
            {.id = "b", .parent = "a"},
            {.id = "a"},
            {
                .id = "c",
                .components = {{
                    .name = "MissingComponent",
                    .fields = {{.name = "x", .value = kin::i32{1}}},
                }},
            },
            {
                .id = "d",
                .components = {{
                    .name = "Transform2D",
                    .fields = {{.name = "missing", .value = kin::i32{1}}},
                }},
            },
        },
    };

    const std::vector<std::string> diagnostics = kin::validate_prefab_asset(invalid, world.components());
    assert(!diagnostics.empty());
    const auto contains = [&](std::string_view text) {
        return std::ranges::any_of(diagnostics, [&](const std::string& diagnostic) {
            return diagnostic.find(text) != std::string::npos;
        });
    };
    assert(contains("duplicate prefab entity id"));
    assert(contains("parent cycle"));
    assert(contains("unknown component"));
    assert(contains("unknown field"));

    kin::PrefabOverrideSet overrides;
    overrides.entities["missing"] = {{{.name = "Transform2D"}}};
    const std::vector<std::string> override_diagnostics =
        kin::validate_prefab_overrides(make_hierarchy_prefab(), overrides, world.components());
    assert(!override_diagnostics.empty());
    assert(override_diagnostics[0].find("unknown entity") != std::string::npos);
}

void test_parse_rejects_bad_component_data() {
    kin::EcsWorld world;
    register_prefab_components(world);

    const std::string json = R"json(
{
  "schema": "kin.prefab/1",
  "id": "bad",
  "root": "root",
  "entities": [
    {
      "id": "root",
      "components": {
        "Transform2D": {
          "pos": "not a vec"
        }
      }
    }
  ]
}
)json";

    kin::PrefabLoadResult loaded = kin::parse_prefab_asset(json, world.components());
    assert(!loaded.ok());
    assert(!loaded.diagnostics.empty());
}

void test_scene_document_bridge_round_trip() {
    kin::EcsWorld world;
    register_prefab_components(world);

    kin::SceneDocument scene;
    scene.entities = {
        {
            .id = "root",
            .name = "Root",
            .components = {{
                .name = "Transform2D",
                .fields = {
                    {.name = "pos", .kind = kin::ComponentFieldKind::Vec2f, .value = kin::Vec2f{7.0f, 8.0f}},
                    {.name = "rotation", .kind = kin::ComponentFieldKind::F32, .value = kin::f32{15.0f}},
                },
            }},
        },
        {
            .id = "child",
            .name = "Child",
            .parent = "root",
            .components = {{
                .name = "RectRenderer",
                .fields = {
                    {.name = "size", .kind = kin::ComponentFieldKind::Vec2f, .value = kin::Vec2f{2.0f, 3.0f}},
                    {.name = "color", .kind = kin::ComponentFieldKind::Color, .value = kin::Color::rgba(4, 5, 6, 7)},
                    {.name = "visible", .kind = kin::ComponentFieldKind::Bool, .value = true},
                },
            }},
        },
    };

    kin::PrefabBuildResult built = kin::prefab_asset_from_scene_document(scene,
                                                                         {.prefab_id = "panel", .name = "Panel"},
                                                                         world.components());
    assert(built.ok());
    assert(built.prefab->root == "root");
    assert(built.prefab->entities.size() == 2);

    kin::SceneDocument converted = kin::scene_document_from_prefab_asset(*built.prefab);
    assert(converted.entities.size() == scene.entities.size());
    assert(converted.entities[1].parent == "root");
    assert(converted.entities[1].components[0].fields[0].kind == kin::ComponentFieldKind::Vec2f);

    kin::PrefabInstance instance = kin::instantiate_prefab(world,
                                                          *built.prefab,
                                                          {.authored_id_prefix = "panel-a"},
                                                          world.components());
    assert(instance.ok());
    assert(world.entities().find_by_authored_id("panel-a/child"));

    kin::SceneDocument multi_root;
    multi_root.entities = {{.id = "a"}, {.id = "b"}};
    kin::PrefabBuildResult rejected = kin::prefab_asset_from_scene_document(multi_root,
                                                                           {.prefab_id = "bad"},
                                                                           world.components());
    assert(!rejected.ok());
    assert(!rejected.diagnostics.empty());
}

void test_data_component_prefab_round_trip_and_overrides() {
    kin::EcsWorld world;
    register_prefab_data_components(world);

    const std::string json = R"json(
{
  "schema": "kin.prefab/1",
  "id": "data_actor",
  "root": "root",
  "entities": [
    {
      "id": "root",
      "components": {
        "DataStats": {
          "hp": 25,
          "name": "base",
          "tint": { "r": 5, "g": 6, "b": 7, "a": 8 },
          "target": "child",
          "asset": "texture/actor"
        }
      }
    },
    {
      "id": "child",
      "parent": "root",
      "components": {}
    }
  ]
}

)json";

    kin::PrefabLoadResult loaded = kin::parse_prefab_asset(json, world.components());
    assert(loaded.ok());

    kin::PrefabOverrideSet overrides;
    overrides.entities["root"] = {{
        .name = "DataStats",
        .fields = {
            {.name = "hp", .value = kin::i32{75}},
            {.name = "name", .value = std::string{"override"}},
        },
    }};

    kin::PrefabInstance instance = kin::instantiate_prefab(world, *loaded.prefab, {}, world.components(), overrides);
    assert(instance.ok());
    std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(instance.root, "DataStats");
    assert(snapshot.has_value());
    assert(std::get<kin::i32>(find_field(*snapshot, "hp")->value) == 75);
    assert(std::get<std::string>(find_field(*snapshot, "name")->value) == "override");
    assert((std::get<kin::Color>(find_field(*snapshot, "tint")->value) == kin::Color::rgba(5, 6, 7, 8)));
    assert(std::get<kin::ComponentEntityRef>(find_field(*snapshot, "target")->value).id == instance.by_authored_id.at("child").id());
    assert(std::get<kin::ComponentAssetRef>(find_field(*snapshot, "asset")->value).id == "texture/actor");
}

void test_data_component_prefab_uses_flecs_inherited_payload_until_override() {
    kin::EcsWorld world;
    register_prefab_data_components(world);

    kin::PrefabLoadResult loaded = kin::parse_prefab_asset(R"json(
{
  "schema": "kin.prefab/1",
  "id": "data_actor",
  "root": "root",
  "entities": [
    {
      "id": "root",
      "components": {
        "DataStats": {
          "hp": 25,
          "name": "base"
        }
      }
    }
  ]
}
)json",
                                                           world.components());
    assert(loaded.ok());

    kin::PrefabInstance base = kin::instantiate_prefab(world, *loaded.prefab, {}, world.components());
    assert(base.ok());
    assert(world.components().has(base.root, "DataStats"));
    std::optional<kin::ComponentSnapshot> base_snapshot = world.components().snapshot(base.root, "DataStats");
    assert(base_snapshot.has_value());
    assert(std::get<kin::i32>(find_field(*base_snapshot, "hp")->value) == 25);
    assert(std::get<std::string>(find_field(*base_snapshot, "name")->value) == "base");

    kin::PrefabOverrideSet overrides;
    overrides.entities["root"] = {{
        .name = "DataStats",
        .fields = {{.name = "hp", .value = kin::i32{80}}},
    }};
    kin::PrefabInstance overridden = kin::instantiate_prefab(world, *loaded.prefab, {}, world.components(), overrides);
    assert(overridden.ok());
    std::optional<kin::ComponentSnapshot> override_snapshot = world.components().snapshot(overridden.root, "DataStats");
    assert(override_snapshot.has_value());
    assert(std::get<kin::i32>(find_field(*override_snapshot, "hp")->value) == 80);
    assert(std::get<std::string>(find_field(*override_snapshot, "name")->value) == "base");
    assert(std::get<kin::i32>(find_field(*base_snapshot, "hp")->value) == 25);
}

void test_prefab_instance_registry_tracks_instances() {
    kin::EcsWorld world;
    register_prefab_components(world);
    kin::PrefabInstanceRegistry registry;

    kin::PrefabInstance instance = registry.instantiate(world,
                                                        make_hierarchy_prefab(),
                                                        {.authored_id_prefix = "tracked"},
                                                        world.components());
    assert(instance.ok());
    assert(!instance.instance_id.empty());
    assert(instance.source_prefab_id == "forge_drone");

    const kin::PrefabInstanceRecord* record = registry.find(instance.instance_id);
    assert(record != nullptr);
    assert(record->prefab_id == "forge_drone");
    assert(record->root.id() == instance.root.id());
    assert(record->by_authored_id.at("body").id() == instance.by_authored_id.at("body").id());
    assert(registry.records().size() == 1);
    assert(registry.remove(instance.instance_id));
    assert(registry.find(instance.instance_id) == nullptr);
}

} // namespace

void run_prefab_data_tests() {
    test_instantiates_hierarchy_and_applies_spawn();
    test_flecs_backend_inherits_prefab_component_shape();
    test_repeated_instantiation_and_authored_id_prefix();
    test_prefab_cache_invalidates_when_asset_changes();
    test_overrides_patch_fields();
    test_instantiation_emits_events_and_dirty_state();
    test_parse_and_save_round_trip();
    test_load_and_save_file();
    test_validation_reports_errors();
    test_parse_rejects_bad_component_data();
    test_scene_document_bridge_round_trip();
    test_data_component_prefab_round_trip_and_overrides();
    test_data_component_prefab_uses_flecs_inherited_payload_until_override();
    test_prefab_instance_registry_tracks_instances();
}
