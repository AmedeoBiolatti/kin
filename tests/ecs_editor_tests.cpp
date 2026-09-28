#include <kin/ecs/editor.hpp>
#include <kin/prefab/prefab_asset.hpp>

#include <cassert>
#include <string>

namespace {

struct EditorStats {
    kin::i32 hp = 0;
};

void register_editor_components(kin::EcsWorld& world) {
    world.components().native<EditorStats>("EditorStats")
        .field("hp", &EditorStats::hp)
        .label("Hit Points")
        .category("Stats")
        .description("Current health")
        .units("hp")
        .range(0, 100);
    world.components().data("DataStats")
        .field_i32("hp", 10)
        .label("Health")
        .range(0, 200);
}

void test_play_world_session_clone_step_reset_and_apply_document() {
    kin::EcsWorld edit;
    register_editor_components(edit);
    edit.entities().create({.name = "actor", .authored_id = "actor"})
        .set(EditorStats{.hp = 5});

    kin::PlayWorldSession session;
    kin::PlaySessionResult started = session.start(edit, {.configure_play_world = register_editor_components});
    assert(started.ok);
    assert(session.running());
    kin::EcsEntity actor = session.play_world()->entities().find_by_authored_id("actor");
    assert(actor);
    actor.get_mut<EditorStats>()->hp = 9;
    actor.modified<EditorStats>();

    assert(session.pause());
    assert(session.paused());
    assert(session.step(0.1f));
    kin::SceneDocument document = session.apply_document();
    assert(document.entities.size() == 1);
    assert(session.reset().ok);
    actor = session.play_world()->entities().find_by_authored_id("actor");
    assert(actor.get<EditorStats>()->hp == 5);
    session.discard();
    assert(session.state() == kin::PlaySessionState::Stopped);
}

void test_reload_coordinator_migrates_schema_and_validates_prefab() {
    kin::EcsWorld world;
    register_editor_components(world);
    kin::EcsEntity entity = world.entity("data");
    assert(world.components().add(entity, "DataStats"));
    assert(world.components().patch_field(entity, "DataStats", "hp", kin::i32{44}));

    kin::EcsReloadCoordinator reload;
    kin::ReloadResult migrated = reload.migrate_data_schema(world, "DataStats", {
        {.name = "hp", .kind = kin::ComponentFieldKind::I32, .value = kin::i32{1}},
        {.name = "label", .kind = kin::ComponentFieldKind::String, .value = std::string{"fresh"}},
    });
    assert(migrated.ok);
    std::optional<kin::ComponentSnapshot> snapshot = world.components().snapshot(entity, "DataStats");
    assert(std::get<kin::i32>(snapshot->fields[0].value) == 44);
    assert(std::get<std::string>(snapshot->fields[1].value) == "fresh");

    kin::PrefabAsset prefab{
        .id = "simple",
        .root = "root",
        .entities = {{.id = "root"}},
    };
    assert(reload.validate_prefab(prefab, world.components()).ok);
}

void test_display_metadata_and_relations() {
    kin::EcsWorld world;
    register_editor_components(world);
    const kin::ComponentDescriptor* native = world.components().find("EditorStats");
    assert(native != nullptr);
    assert(native->fields[0].label == "Hit Points");
    assert(native->fields[0].category == "Stats");
    assert(native->fields[0].min_value.value() == 0);
    assert(native->fields[0].max_value.value() == 100);

    kin::EcsEntity source = world.entities().create({.name = "source", .authored_id = "source"});
    kin::EcsEntity target = world.entities().create({.name = "target", .authored_id = "target"});
    assert(world.relations().add(source, "targets", target));
    assert(world.relations().has(source, "targets", target));
    std::vector<kin::RelationPairSnapshot> relations = world.relations().snapshots(source);
    assert(relations.size() == 1);
    assert(relations[0].relation.name == "targets");
    assert(relations[0].target_authored_id == "target");
    assert(world.relations().remove(source, "targets", target));
    assert(!world.relations().has(source, "targets", target));
}

} // namespace

int main() {
    test_play_world_session_clone_step_reset_and_apply_document();
    test_reload_coordinator_migrates_schema_and_validates_prefab();
    test_display_metadata_and_relations();
    return 0;
}
