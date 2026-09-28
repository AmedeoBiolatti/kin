#include <kin/ecs/component.hpp>
#include <kin/ecs/component_schema.hpp>

#include <algorithm>
#include <cassert>
#include <string>

namespace {

struct Transform {
    kin::Vec2f position{};
    kin::f32 rotation = 0.0f;
};

struct Stats {
    bool active = false;
    kin::i32 hp = 0;
    kin::i64 score = 0;
    kin::u32 ammo = 0;
    kin::u64 xp = 0;
    kin::f64 precision = 0.0;
    std::string name;
    kin::Vec2i tile{};
    kin::Color tint{};
};

template <typename T>
const kin::ComponentFieldSnapshot* field(const kin::ComponentSnapshot& snapshot, std::string_view name) {
    const auto found = std::ranges::find_if(snapshot.fields, [&](const kin::ComponentFieldSnapshot& field) {
        return field.name == name;
    });
    assert(found != snapshot.fields.end());
    assert(std::holds_alternative<T>(found->value));
    return &*found;
}

void register_test_components(kin::EcsWorld& world) {
    world.components().native<Transform>("Transform")
        .field("position", &Transform::position)
        .field("rotation", &Transform::rotation);

    world.components().native<Stats>("Stats")
        .field("active", &Stats::active)
        .field("hp", &Stats::hp)
        .field("score", &Stats::score)
        .field("ammo", &Stats::ammo)
        .field("xp", &Stats::xp)
        .field("precision", &Stats::precision)
        .field("name", &Stats::name)
        .field("tile", &Stats::tile)
        .field("tint", &Stats::tint);
}

void register_data_components(kin::EcsWorld& world) {
    world.components().data("DataStats")
        .field_bool("active", true)
        .field_i32("hp", 100)
        .field_i64("score", 1000)
        .field_u32("ammo", 7)
        .field_u64("xp", 9000)
        .field_f32("speed", 3.5f)
        .field_f64("precision", 0.75)
        .field_string("name", "Scout")
        .field_vec2f("position", {1.0f, 2.0f})
        .field_vec2i("tile", {3, 4})
        .field_color("tint", kin::Color::rgba(1, 2, 3, 4))
        .field_entity_ref("target")
        .field_asset_ref("asset", {.id = "asset/default"});
}

void test_registration_lookup_and_descriptors_are_deterministic() {
    kin::EcsWorld world;
    register_test_components(world);

    const kin::ComponentDescriptor* transform = world.components().find("Transform");
    const kin::ComponentDescriptor* stats = world.components().find("Stats");
    assert(transform != nullptr);
    assert(stats != nullptr);
    assert(transform->id != 0);
    assert(world.components().find(transform->id) == transform);
    assert(transform->fields.size() == 2);
    assert(transform->fields[0].name == "position");
    assert(transform->fields[0].kind == kin::ComponentFieldKind::Vec2f);
    assert(transform->fields[1].kind == kin::ComponentFieldKind::F32);

    const std::vector<kin::ComponentDescriptor> descriptors = world.components().descriptors();
    assert(descriptors.size() == 2);
    assert(descriptors[0].name == "Transform");
    assert(descriptors[1].name == "Stats");

    auto duplicate = world.components().native<Transform>("Transform");
    assert(!duplicate);
    assert(world.components().last_error().find("duplicate") != std::string::npos);
}

void test_data_registration_validation() {
    kin::EcsWorld world;
    register_data_components(world);

    const kin::ComponentDescriptor* descriptor = world.components().find("DataStats");
    assert(descriptor != nullptr);
    assert(descriptor->kind == kin::ComponentKind::Data);
    assert(descriptor->fields.size() == 13);
    assert(descriptor->fields[0].name == "active");
    assert(descriptor->fields[0].kind == kin::ComponentFieldKind::Bool);

    auto duplicate_component = world.components().data("DataStats");
    assert(!duplicate_component);
    assert(world.components().last_error().find("duplicate") != std::string::npos);

    auto duplicate_field = world.components().data("BadData");
    duplicate_field.field_i32("value", 1).field_i32("value", 2);
    assert(world.components().last_error().find("duplicate field") != std::string::npos);

    auto mismatch = world.components().data("MismatchData");
    mismatch.field("hp", kin::ComponentFieldKind::I32, std::string{"bad"});
    assert(world.components().last_error().find("type mismatch") != std::string::npos);
}

void test_component_schema_asset_registers_and_migrates_data_components() {
    const std::string schema_json = R"json(
{
  "schema": "kin.components/1",
  "components": [
    {
      "name": "Clock",
      "fields": [
        { "name": "tick", "kind": "f32", "default": 0.0, "label": "Tick" },
        { "name": "label", "kind": "string", "default": "start" }
      ]
    }
  ]
}
)json";

    kin::ComponentSchemaLoadResult loaded = kin::parse_component_schema_asset(schema_json);
    assert(loaded.ok());

    kin::EcsWorld world;
    std::vector<std::string> diagnostics;
    assert(kin::apply_component_schema_asset(world.components(), *loaded.document, diagnostics));
    const kin::ComponentDescriptor* descriptor = world.components().find("Clock");
    assert(descriptor != nullptr);
    assert(descriptor->kind == kin::ComponentKind::Data);
    assert(descriptor->fields.size() == 2);
    assert(descriptor->fields[0].label == "Tick");

    kin::EcsEntity entity = world.entity("clock");
    assert(world.components().add(entity, "Clock"));
    assert(world.components().patch_field(entity, "Clock", "tick", kin::f32{3.0f}));

    const std::string migrated_json = R"json(
{
  "schema": "kin.components/1",
  "components": [
    {
      "name": "Clock",
      "fields": [
        { "name": "tick", "kind": "f32", "default": 1.0 },
        { "name": "extra", "kind": "i32", "default": 7 }
      ]
    }
  ]
}
)json";
    kin::ComponentSchemaLoadResult migrated = kin::parse_component_schema_asset(migrated_json);
    assert(migrated.ok());
    assert(kin::apply_component_schema_asset(world.components(), *migrated.document, diagnostics));
    std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(entity, "Clock");
    assert(snapshot.has_value());
    assert(snapshot->fields.size() == 2);
    assert(std::get<kin::f32>(field<kin::f32>(*snapshot, "tick")->value) == 3.0f);
    assert(std::get<kin::i32>(field<kin::i32>(*snapshot, "extra")->value) == 7);

    const std::string incompatible_json = R"json(
{
  "schema": "kin.components/1",
  "components": [
    {
      "name": "Clock",
      "fields": [
        { "name": "tick", "kind": "string", "default": "bad" }
      ]
    }
  ]
}
)json";
    kin::ComponentSchemaLoadResult incompatible = kin::parse_component_schema_asset(incompatible_json);
    assert(incompatible.ok());
    assert(!kin::apply_component_schema_asset(world.components(), *incompatible.document, diagnostics));
    assert(!diagnostics.empty());
    snapshot = world.components().snapshot(entity, "Clock");
    assert(snapshot->fields.size() == 2);
    assert(std::get<kin::f32>(field<kin::f32>(*snapshot, "tick")->value) == 3.0f);
}

void test_component_schema_asset_rejects_invalid_documents() {
    const std::string duplicate_json = R"json(
{
  "schema": "kin.components/1",
  "components": [
    {
      "name": "Bad",
      "fields": [
        { "name": "hp", "kind": "i32", "default": 1 },
        { "name": "hp", "kind": "i32", "default": 2 }
      ]
    }
  ]
}
)json";
    kin::ComponentSchemaLoadResult duplicate = kin::parse_component_schema_asset(duplicate_json);
    assert(!duplicate.ok());
    assert(!duplicate.diagnostics.empty());

    const std::string mismatch_json = R"json(
{
  "schema": "kin.components/1",
  "components": [
    {
      "name": "Bad",
      "fields": [
        { "name": "hp", "kind": "i32", "default": "wrong" }
      ]
    }
  ]
}
)json";
    kin::ComponentSchemaLoadResult mismatch = kin::parse_component_schema_asset(mismatch_json);
    assert(!mismatch.ok());
    assert(!mismatch.diagnostics.empty());
}

void test_add_remove_and_has_by_metadata() {
    kin::EcsWorld world;
    register_test_components(world);
    kin::EcsEntity entity = world.entity("actor");
    const kin::ComponentId transform_id = world.components().find("Transform")->id;

    assert(!world.components().has(entity, transform_id));
    assert(world.components().add(entity, transform_id));
    assert(world.components().has(entity, transform_id));
    assert(entity.has<Transform>());

    assert(world.components().remove(entity, "Transform"));
    assert(!world.components().has(entity, "Transform"));
    assert(!entity.has<Transform>());
}

void test_data_add_remove_snapshot_and_patch() {
    kin::EcsWorld world;
    register_data_components(world);
    kin::EcsEntity entity = world.entity("data");
    const kin::ComponentId data_id = world.components().find("DataStats")->id;

    assert(!world.components().has(entity, data_id));
    assert(world.components().add(entity, data_id));
    assert(world.components().has(entity, "DataStats"));
    assert(entity.raw().has(static_cast<flecs::id_t>(data_id)));

    std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(entity, "DataStats");
    assert(snapshot.has_value());
    assert(snapshot->descriptor.kind == kin::ComponentKind::Data);
    assert(snapshot->fields.size() == 13);
    assert(std::get<bool>(field<bool>(*snapshot, "active")->value));
    assert(std::get<kin::i32>(field<kin::i32>(*snapshot, "hp")->value) == 100);
    assert(std::get<kin::i64>(field<kin::i64>(*snapshot, "score")->value) == 1000);
    assert(std::get<kin::u32>(field<kin::u32>(*snapshot, "ammo")->value) == 7);
    assert(std::get<kin::u64>(field<kin::u64>(*snapshot, "xp")->value) == 9000);
    assert(std::get<kin::f32>(field<kin::f32>(*snapshot, "speed")->value) == 3.5f);
    assert(std::get<kin::f64>(field<kin::f64>(*snapshot, "precision")->value) == 0.75);
    assert(std::get<std::string>(field<std::string>(*snapshot, "name")->value) == "Scout");
    assert((std::get<kin::Vec2f>(field<kin::Vec2f>(*snapshot, "position")->value) == kin::Vec2f{1.0f, 2.0f}));
    assert((std::get<kin::Vec2i>(field<kin::Vec2i>(*snapshot, "tile")->value) == kin::Vec2i{3, 4}));
    assert((std::get<kin::Color>(field<kin::Color>(*snapshot, "tint")->value) == kin::Color::rgba(1, 2, 3, 4)));
    assert(std::get<kin::ComponentEntityRef>(field<kin::ComponentEntityRef>(*snapshot, "target")->value).id == 0);
    assert(std::get<kin::ComponentAssetRef>(field<kin::ComponentAssetRef>(*snapshot, "asset")->value).id == "asset/default");

    world.events().clear();
    assert(world.components().patch_field(entity, "DataStats", "hp", kin::i32{42}));
    snapshot = world.components().snapshot(entity, data_id);
    assert(std::get<kin::i32>(field<kin::i32>(*snapshot, "hp")->value) == 42);
    assert(world.events().dirty_state().components_dirty);
    assert(std::ranges::any_of(world.events().snapshot(), [](const kin::EcsEvent& event) {
        return event.kind == kin::EcsEventKind::ComponentChanged && event.component_name == "DataStats" && event.field_name == "hp";
    }));

    assert(!world.components().patch_field(entity, "DataStats", "hp", std::string{"bad"}));
    assert(world.components().last_error().find("type mismatch") != std::string::npos);
    assert(world.components().remove(entity, data_id));
    assert(!world.components().has(entity, "DataStats"));
    assert(!entity.raw().has(static_cast<flecs::id_t>(data_id)));
    assert(!world.components().snapshot(entity, "DataStats"));
}

void test_entity_and_asset_refs_round_trip_and_migrate_schema() {
    kin::EcsWorld world;
    register_data_components(world);
    kin::EcsEntity target = world.entities().create({.name = "target", .authored_id = "target"});
    kin::EcsEntity holder = world.entities().create({.name = "holder", .authored_id = "holder"});
    assert(world.components().add(holder, "DataStats"));
    assert(world.components().patch_field(holder, "DataStats", "target", kin::ComponentEntityRef{.id = target.id()}));
    assert(world.components().patch_field(holder, "DataStats", "asset", kin::ComponentAssetRef{.id = "texture/player"}));

    kin::SceneDocument document = kin::scene_document_from_world(world);
    const auto holder_doc = std::ranges::find_if(document.entities, [](const kin::SceneEntityDocument& entity) {
        return entity.id == "holder";
    });
    assert(holder_doc != document.entities.end());
    const auto data_doc = std::ranges::find_if(holder_doc->components, [](const kin::SceneComponentDocument& component) {
        return component.name == "DataStats";
    });
    assert(data_doc != holder_doc->components.end());
    const auto target_field = std::ranges::find_if(data_doc->fields, [](const kin::ComponentFieldSnapshot& item) {
        return item.name == "target";
    });
    assert(target_field != data_doc->fields.end());
    assert(std::get<kin::ComponentEntityRef>(target_field->value).authored_id == "target");

    kin::EcsWorld loaded;
    register_data_components(loaded);
    std::string error;
    assert(kin::instantiate_scene(loaded, document, error));
    kin::EcsEntity loaded_holder = loaded.entities().find_by_authored_id("holder");
    kin::EcsEntity loaded_target = loaded.entities().find_by_authored_id("target");
    std::optional<kin::ComponentSnapshot> loaded_snapshot = loaded.components().snapshot(loaded_holder, "DataStats");
    assert(std::get<kin::ComponentEntityRef>(field<kin::ComponentEntityRef>(*loaded_snapshot, "target")->value).id == loaded_target.id());
    assert(std::get<kin::ComponentAssetRef>(field<kin::ComponentAssetRef>(*loaded_snapshot, "asset")->value).id == "texture/player");

    assert(loaded.components().migrate_data_schema("DataStats", {
        {.name = "hp", .kind = kin::ComponentFieldKind::I32, .value = kin::i32{1}},
        {.name = "new_field", .kind = kin::ComponentFieldKind::String, .value = std::string{"new"}},
    }));
    loaded_snapshot = loaded.components().snapshot(loaded_holder, "DataStats");
    assert(loaded_snapshot->fields.size() == 2);
    assert(std::get<kin::i32>(field<kin::i32>(*loaded_snapshot, "hp")->value) == 100);
    assert(std::get<std::string>(field<std::string>(*loaded_snapshot, "new_field")->value) == "new");
}

void test_snapshot_supported_field_values() {
    kin::EcsWorld world;
    register_test_components(world);
    kin::EcsEntity entity = world.entity("stats").set(Stats{
        .active = true,
        .hp = 42,
        .score = 1000,
        .ammo = 7,
        .xp = 9000,
        .precision = 0.75,
        .name = "Scout",
        .tile = {3, 4},
        .tint = kin::Color::rgba(1, 2, 3, 4),
    });

    const std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(entity, "Stats");
    assert(snapshot.has_value());
    assert(snapshot->descriptor.name == "Stats");
    assert(snapshot->fields.size() == 9);
    assert(std::get<bool>(field<bool>(*snapshot, "active")->value));
    assert(std::get<kin::i32>(field<kin::i32>(*snapshot, "hp")->value) == 42);
    assert(std::get<kin::i64>(field<kin::i64>(*snapshot, "score")->value) == 1000);
    assert(std::get<kin::u32>(field<kin::u32>(*snapshot, "ammo")->value) == 7);
    assert(std::get<kin::u64>(field<kin::u64>(*snapshot, "xp")->value) == 9000);
    assert(std::get<kin::f64>(field<kin::f64>(*snapshot, "precision")->value) == 0.75);
    assert(std::get<std::string>(field<std::string>(*snapshot, "name")->value) == "Scout");
    assert((std::get<kin::Vec2i>(field<kin::Vec2i>(*snapshot, "tile")->value) == kin::Vec2i{3, 4}));
    assert((std::get<kin::Color>(field<kin::Color>(*snapshot, "tint")->value) == kin::Color::rgba(1, 2, 3, 4)));
}

void test_patch_fields_updates_component_data() {
    kin::EcsWorld world;
    register_test_components(world);
    kin::EcsEntity entity = world.entity("actor").set(Transform{}).set(Stats{});

    assert(world.components().patch_field(entity, "Transform", "position", kin::Vec2f{5.0f, 6.0f}));
    assert(world.components().patch_field(entity, "Transform", "rotation", kin::f32{90.0f}));
    assert(world.components().patch_field(entity, "Stats", "active", true));
    assert(world.components().patch_field(entity, "Stats", "name", std::string{"Patched"}));
    assert(world.components().patch_field(entity, "Stats", "tint", kin::Color::rgb(8, 9, 10)));

    const Transform* transform = entity.get<Transform>();
    const Stats* stats = entity.get<Stats>();
    assert((transform->position == kin::Vec2f{5.0f, 6.0f}));
    assert(transform->rotation == 90.0f);
    assert(stats->active);
    assert(stats->name == "Patched");
    assert((stats->tint == kin::Color::rgb(8, 9, 10)));

    kin::i32 matched = 0;
    world.query<Transform>().each_entity([&](kin::EcsEntity, const Transform& patched) {
        if (patched.rotation == 90.0f) {
            ++matched;
        }
    });
    assert(matched == 1);
}

void test_entity_component_snapshots_list_registered_present_components() {
    kin::EcsWorld world;
    register_test_components(world);
    kin::EcsEntity entity = world.entity("actor").set(Stats{});

    const std::vector<kin::ComponentSnapshot> snapshots = world.components().snapshots(entity);
    assert(snapshots.size() == 1);
    assert(snapshots[0].descriptor.name == "Stats");
}

void test_data_components_work_with_query_plans_scene_and_destroy_cleanup() {
    kin::EcsWorld world;
    register_data_components(world);
    kin::EcsEntity with_data = world.entities().create({.name = "with", .authored_id = "with"});
    kin::EcsEntity without_data = world.entities().create({.name = "without", .authored_id = "without"});
    assert(world.components().add(with_data, "DataStats"));
    assert(world.components().patch_field(with_data, "DataStats", "hp", kin::i32{64}));

    kin::EcsQueryPlan all_plan = world.build_query_plan({.all = {"DataStats"}});
    assert(all_plan.valid);
    assert(all_plan.matched_count == 1);
    std::vector<kin::EcsEntity> matched = world.query_entities(all_plan);
    assert(matched.size() == 1);
    assert(matched[0].id() == with_data.id());

    kin::EcsQueryPlan none_plan = world.build_query_plan({.none = {"DataStats"}});
    assert(none_plan.valid);
    matched = world.query_entities(none_plan);
    assert(std::ranges::any_of(matched, [&](kin::EcsEntity entity) {
        return entity.id() == without_data.id();
    }));
    assert(std::ranges::none_of(matched, [&](kin::EcsEntity entity) {
        return entity.id() == with_data.id();
    }));

    kin::SceneDocument document = kin::scene_document_from_world(world);
    kin::EcsWorld target;
    register_data_components(target);
    std::string error;
    assert(kin::instantiate_scene(target, document, error));
    kin::EcsEntity loaded = target.entities().find_by_authored_id("with");
    assert(loaded);
    std::optional<kin::ComponentSnapshot> loaded_snapshot = target.components().snapshot(loaded, "DataStats");
    assert(loaded_snapshot.has_value());
    assert(std::get<kin::i32>(field<kin::i32>(*loaded_snapshot, "hp")->value) == 64);

    assert(world.entities().destroy(with_data));
    assert(world.components().entities().empty());
}

void test_failure_paths_set_diagnostics() {
    kin::EcsWorld world;
    register_test_components(world);
    kin::EcsEntity entity = world.entity("actor").set(Stats{});

    assert(!world.components().add(entity, "Missing"));
    assert(world.components().last_error().find("unknown component") != std::string::npos);

    assert(!world.components().snapshot(entity, "Transform"));
    assert(world.components().last_error().find("does not have component") != std::string::npos);

    assert(!world.components().patch_field(entity, "Stats", "missing", kin::i32{1}));
    assert(world.components().last_error().find("unknown field") != std::string::npos);

    assert(!world.components().patch_field(entity, "Stats", "hp", std::string{"bad"}));
    assert(world.components().last_error().find("type mismatch") != std::string::npos);
}

void test_component_fields_can_be_visited_without_snapshot_ownership() {
    kin::EcsWorld world;
    register_test_components(world);
    kin::EcsEntity entity = world.entity("actor").set(Stats{.hp = 42});

    std::vector<std::string> names;
    assert(world.components().visit_fields(entity, "Stats",
        [&](std::string_view name, kin::ComponentFieldKind, const kin::ComponentFieldValue& value) {
            names.emplace_back(name);
            if (name == "hp") {
                assert(std::get<kin::i32>(value) == 42);
            }
        }));
    assert(!names.empty());
}

} // namespace

int main() {
    test_registration_lookup_and_descriptors_are_deterministic();
    test_data_registration_validation();
    test_component_schema_asset_registers_and_migrates_data_components();
    test_component_schema_asset_rejects_invalid_documents();
    test_add_remove_and_has_by_metadata();
    test_data_add_remove_snapshot_and_patch();
    test_entity_and_asset_refs_round_trip_and_migrate_schema();
    test_snapshot_supported_field_values();
    test_patch_fields_updates_component_data();
    test_entity_component_snapshots_list_registered_present_components();
    test_data_components_work_with_query_plans_scene_and_destroy_cleanup();
    test_failure_paths_set_diagnostics();
    test_component_fields_can_be_visited_without_snapshot_ownership();
    return 0;
}
