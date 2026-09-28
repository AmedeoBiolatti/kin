#include <kin/ecs/component.hpp>

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

namespace {

struct Transform {
    kin::Vec2f position{};
    kin::f32 rotation = 0.0f;
};

struct Stats {
    kin::i32 hp = 0;
    std::string name;
};

const kin::EntitySnapshot* find_entity(const kin::WorldSnapshot& snapshot, std::string_view name) {
    const auto found = std::ranges::find_if(snapshot.entities, [&](const kin::EntitySnapshot& entity) {
        return entity.descriptor.name == name;
    });
    return found == snapshot.entities.end() ? nullptr : &*found;
}

void register_components(kin::EcsWorld& world) {
    world.components().native<Transform>("Transform")
        .field("position", &Transform::position)
        .field("rotation", &Transform::rotation);
    world.components().native<Stats>("Stats")
        .field("hp", &Stats::hp)
        .field("name", &Stats::name);
}

void test_create_lookup_handle_and_enabled_state() {
    kin::EcsWorld world;
    kin::EcsEntity root = world.entities().create({
        .name = "root",
        .authored_id = "root-id",
        .enabled = false,
    });
    assert(root);
    assert(!root.raw().enabled());
    assert(world.entities().find_by_id(root.id()).id() == root.id());
    assert(world.entities().find_by_authored_id("root-id").id() == root.id());
    assert(world.entities().find_by_name("root").id() == root.id());

    const kin::EcsEntityHandle handle = world.entities().handle(root);
    assert(handle.id == root.id());
    assert(world.entities().valid(handle));
    assert(world.entities().resolve(handle).id() == root.id());
    assert(!world.entities().create({.name = "dupe", .authored_id = "root-id"}));
    assert(world.entities().last_error().find("duplicate authored") != std::string::npos);

    assert(world.entities().enable(handle));
    assert(root.raw().enabled());
    assert(world.entities().destroy(handle));
    assert(!world.entities().valid(handle));
    assert(!world.entities().find_by_authored_id("root-id"));
}

void test_rename_reparent_detach_and_cycle_rejection() {
    kin::EcsWorld world;
    kin::EcsEntity root = world.entities().create({.name = "root", .authored_id = "root"});
    kin::EcsEntity child = world.entities().create({.name = "child", .authored_id = "child", .parent = root});
    kin::EcsEntity grandchild = world.entities().create({.name = "grandchild", .authored_id = "grandchild", .parent = child});

    assert(child.parent().id() == root.id());
    assert(world.entities().rename(child, "renamed"));
    assert(child.name() == "renamed");
    assert(world.entities().find_by_name("renamed").id() == child.id());
    assert(!world.entities().reparent(root, grandchild));
    assert(world.entities().last_error().find("cycle") != std::string::npos);

    assert(world.entities().detach_parent(child));
    assert(!child.parent());
    assert(grandchild.parent().id() == child.id());
}

void test_snapshot_reports_identity_metadata_and_raw_fallbacks() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity root = world.entities().create({.name = "root", .authored_id = "root"});
    kin::EcsEntity child = world.entity("child").set(Stats{.hp = 5, .name = "Scout"}).child_of(root);
    world.entities().set_authored_id(child, "child");
    world.raw().entity("raw.component").set(Transform{});

    const kin::WorldSnapshot snapshot = world.snapshot();
    const kin::EntitySnapshot* root_snapshot = find_entity(snapshot, "root");
    const kin::EntitySnapshot* child_snapshot = find_entity(snapshot, "child");
    const kin::EntitySnapshot* raw_snapshot = find_entity(snapshot, "raw.component");
    assert(root_snapshot != nullptr);
    assert(child_snapshot != nullptr);
    assert(raw_snapshot != nullptr);
    assert(root_snapshot->descriptor.authored_id == "root");
    assert(root_snapshot->descriptor.registered);
    assert(root_snapshot->descriptor.handle_generation == 1);
    assert(child_snapshot->descriptor.authored_id == "child");
    assert(child_snapshot->descriptor.parent == root.id());
    assert(!raw_snapshot->descriptor.registered);
    assert(raw_snapshot->descriptor.authored_id.empty());
}

void test_deep_clone_copies_registered_components_and_hierarchy() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity root = world.entities().create({.name = "root", .authored_id = "root"});
    root.set(Transform{.position = {1.0f, 2.0f}, .rotation = 45.0f});
    kin::EcsEntity child = world.entities().create({.name = "child", .authored_id = "child", .parent = root});
    child.set(Stats{.hp = 7, .name = "Scout"});

    kin::EntityCloneResult clone = world.entities().clone(root, {.authored_id = "clone-root"});
    assert(clone.root);
    assert(clone.original_to_clone.size() == 2);
    assert(world.entities().authored_id(clone.root).value() == "clone-root");
    kin::EcsEntity cloned_child = clone.original_to_clone.at(child.id());
    assert(cloned_child.parent().id() == clone.root.id());
    assert(world.entities().authored_id(cloned_child).value().empty());
    assert(clone.root.get<Transform>()->rotation == 45.0f);
    assert(cloned_child.get<Stats>()->hp == 7);

    kin::EntityCloneResult duplicate = world.entities().clone(root, {
        .authored_id = "root",
        .preserve_child_authored_ids = true,
    });
    assert(!duplicate.root);
    assert(!duplicate.diagnostics.empty());
}

void test_scene_documents_use_authored_ids() {
    kin::EcsWorld source;
    register_components(source);
    kin::EcsEntity root = source.entities().create({.name = "root", .authored_id = "root"});
    source.entities().create({.name = "child", .authored_id = "child", .parent = root})
        .set(Stats{.hp = 9, .name = "Ranger"});

    const kin::SceneDocument document = kin::scene_document_from_world(source);
    assert(document.entities.size() == 2);
    assert(document.entities[0].id == "root");
    assert(document.entities[1].id == "child");
    assert(document.entities[1].parent == "root");

    kin::EcsWorld target;
    register_components(target);
    std::string error;
    assert(kin::instantiate_scene(target, document, error));
    kin::EcsEntity child = target.entities().find_by_authored_id("child");
    assert(child);
    assert(child.parent().id() == target.entities().find_by_authored_id("root").id());
    assert(child.get<Stats>()->hp == 9);

    assert(!kin::instantiate_scene(target, document, error));
    assert(error.find("duplicate authored") != std::string::npos);
}

} // namespace

int main() {
    test_create_lookup_handle_and_enabled_state();
    test_rename_reparent_detach_and_cycle_rejection();
    test_snapshot_reports_identity_metadata_and_raw_fallbacks();
    test_deep_clone_copies_registered_components_and_hierarchy();
    test_scene_documents_use_authored_ids();
    return 0;
}
