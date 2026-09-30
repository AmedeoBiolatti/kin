#include <kin/core/json_value.hpp>
#include <kin/ecs/component.hpp>

#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Transform {
    kin::Vec2f position{};
    kin::f32 rotation = 0.0f;
};

struct Stats {
    bool active = true;
    kin::i32 hp = 0;
    std::string name;
    kin::Vec2i tile{};
    kin::Color tint{};
};

struct Hidden {
    kin::i32 value = 0;
};

void register_components(kin::EcsWorld& world) {
    world.components().native<Transform>("Transform")
        .field("position", &Transform::position)
        .field("rotation", &Transform::rotation);
    world.components().native<Stats>("Stats")
        .field("active", &Stats::active)
        .field("hp", &Stats::hp)
        .field("name", &Stats::name)
        .field("tile", &Stats::tile)
        .field("tint", &Stats::tint);
    world.components().native<Hidden>("Hidden")
        .field("value", &Hidden::value);
}

const kin::EntitySnapshot* find_entity(const kin::WorldSnapshot& snapshot, std::string_view name) {
    const auto found = std::ranges::find_if(snapshot.entities, [&](const kin::EntitySnapshot& entity) {
        return entity.descriptor.name == name;
    });
    return found == snapshot.entities.end() ? nullptr : &*found;
}

void test_world_snapshot_contains_hierarchy_and_components() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity root = world.entity("root").set(Transform{.position = {1.0f, 2.0f}, .rotation = 45.0f});
    kin::EcsEntity child = world.entity("child").set(Stats{.hp = 10, .name = "Scout"}).child_of(root);

    const kin::WorldSnapshot snapshot = world.snapshot();
    assert(snapshot.components.size() == 3);
    const kin::EntitySnapshot* root_snapshot = find_entity(snapshot, "root");
    const kin::EntitySnapshot* child_snapshot = find_entity(snapshot, "child");
    assert(root_snapshot != nullptr);
    assert(child_snapshot != nullptr);
    assert(root_snapshot->descriptor.children.size() == 1);
    assert(root_snapshot->descriptor.children[0] == child.id());
    assert(child_snapshot->descriptor.parent == root.id());
    assert(root_snapshot->components.size() == 1);
    assert(root_snapshot->components[0].descriptor.name == "Transform");
    assert(child_snapshot->components[0].descriptor.name == "Stats");
}

void test_query_plan_matches_all_and_none_deterministically() {
    kin::EcsWorld world;
    register_components(world);
    world.entity("b").set(Transform{}).set(Stats{});
    world.entity("a").set(Transform{});
    world.entity("c").set(Transform{}).set(Hidden{});

    kin::EcsQueryPlan plan = world.build_query_plan({
        .all = {"Transform"},
        .none = {"Hidden"},
        .reads = {"Transform"},
    });
    assert(plan.valid);
    assert(plan.matched_count == 2);

    std::vector<std::string> names;
    world.each(plan, [&](kin::EcsEntity entity) {
        names.push_back(entity.name());
    });
    assert((names == std::vector<std::string>{"a", "b"}));

    world.entity("aa").set(Transform{});
    names.clear();
    world.each(plan, [&](kin::EcsEntity entity) {
        names.push_back(entity.name());
    });
    assert((names == std::vector<std::string>{"a", "aa", "b"}));

    kin::EcsQueryPlan invalid = world.build_query_plan({.all = {"Missing"}});
    assert(!invalid.valid);
    assert(!invalid.diagnostics.empty());
}

// Queries and snapshots list entities roots first, then children grouped by
// parent id (not parent name), each group by name, with unnamed entities and
// equal names ordered by id.
void test_inspection_order_groups_by_parent_then_name() {
    kin::EcsWorld world;
    register_components(world);
    std::vector<kin::EcsEntity> made;
    const auto make = [&](std::string_view name) {
        kin::EcsEntity entity = world.entity(name);
        entity.set(Transform{});
        made.push_back(entity);
        return entity;
    };
    kin::EcsEntity zeta = make("zeta"); // created first, so its id is lower than alpha's
    kin::EcsEntity alpha = make("alpha");
    make("");
    make("mid");
    for (kin::EcsEntity parent : {alpha, zeta}) {
        for (std::string_view name : {"b", "", "a", "", "c"}) {
            make(name).child_of(parent);
        }
    }

    std::vector<kin::EcsEntity> expected = made;
    std::ranges::sort(expected, [](const kin::EcsEntity& a, const kin::EcsEntity& b) {
        const bool a_child = static_cast<bool>(a.parent());
        const bool b_child = static_cast<bool>(b.parent());
        if (a_child != b_child) {
            return !a_child;
        }
        if (a_child && a.parent().id() != b.parent().id()) {
            return a.parent().id() < b.parent().id();
        }
        if (a.name() != b.name()) {
            return a.name() < b.name();
        }
        return a.id() < b.id();
    });
    std::vector<kin::EcsId> expected_ids;
    for (const kin::EcsEntity& entity : expected) {
        expected_ids.push_back(entity.id());
    }
    // Spot-check the rule itself, not only the reference: zeta's children come
    // before alpha's because zeta's id is lower.
    assert(expected[4].parent().id() == zeta.id());

    const kin::EcsQueryPlan plan = world.build_query_plan({.all = {"Transform"}});
    std::vector<kin::EcsId> queried;
    for (const kin::EcsEntity& entity : world.query_entities(plan)) {
        queried.push_back(entity.id());
    }
    assert(queried == expected_ids);

    std::vector<kin::EcsId> snapshotted;
    for (const kin::EntitySnapshot& entity : world.snapshot({.include_components = false}).entities) {
        snapshotted.push_back(entity.descriptor.id);
    }
    assert(snapshotted == expected_ids);
}

void test_component_field_json_conversion() {
    std::string error;
    const kin::JsonParseResult vec_json = kin::parse_json(R"({"x":3.5,"y":4.5})");
    assert(vec_json.ok());
    std::optional<kin::ComponentFieldValue> vec =
        kin::component_field_value_from_json(kin::ComponentFieldKind::Vec2f, *vec_json.value, error);
    assert(vec.has_value());
    assert((std::get<kin::Vec2f>(*vec) == kin::Vec2f{3.5f, 4.5f}));

    const kin::JsonParseResult color_json = kin::parse_json(R"({"r":1,"g":2,"b":3,"a":4})");
    assert(color_json.ok());
    std::optional<kin::ComponentFieldValue> color =
        kin::component_field_value_from_json(kin::ComponentFieldKind::Color, *color_json.value, error);
    assert(color.has_value());
    assert((std::get<kin::Color>(*color) == kin::Color::rgba(1, 2, 3, 4)));

    const kin::JsonParseResult bad_json = kin::parse_json(R"("bad")");
    assert(bad_json.ok());
    assert(!kin::component_field_value_from_json(kin::ComponentFieldKind::I32, *bad_json.value, error));
    assert(!error.empty());
}

void test_scene_document_round_trip() {
    kin::EcsWorld source;
    register_components(source);
    kin::EcsEntity root = source.entity("root").set(Transform{.position = {1.0f, 2.0f}, .rotation = 30.0f});
    source.entity("child")
        .set(Stats{.active = true, .hp = 77, .name = "Ranger", .tile = {8, 9}, .tint = kin::Color::rgb(10, 20, 30)})
        .child_of(root);

    const kin::SceneDocument document = kin::scene_document_from_world(source);
    assert(document.entities.size() == 2);
    const std::string serialized = kin::serialize_scene(source);
    assert(serialized.find("\"entities\"") != std::string::npos);
    assert(serialized.find("\"Transform\"") != std::string::npos);

    kin::EcsWorld target;
    register_components(target);
    std::string error;
    assert(kin::instantiate_scene(target, document, error));
    const kin::WorldSnapshot snapshot = target.snapshot();
    const kin::EntitySnapshot* child = find_entity(snapshot, "child");
    assert(child != nullptr);
    assert(child->descriptor.parent != 0);
    bool found_stats = false;
    target.query<Stats>().each_entity([&](kin::EcsEntity, const Stats& stats) {
        found_stats = true;
        assert(stats.hp == 77);
        assert(stats.name == "Ranger");
        assert((stats.tile == kin::Vec2i{8, 9}));
        assert((stats.tint == kin::Color::rgb(10, 20, 30)));
    });
    assert(found_stats);
}

void test_scene_instantiation_validates_before_mutating() {
    kin::EcsWorld world;
    register_components(world);

    kin::SceneDocument unknown_component;
    unknown_component.entities.push_back({
        .id = "e",
        .name = "entity",
        .components = {{.name = "Missing"}},
    });
    std::string error;
    assert(!kin::instantiate_scene(world, unknown_component, error));
    assert(error.find("unknown component") != std::string::npos);
    assert(world.snapshot().entities.empty());

    kin::SceneDocument invalid_parent;
    invalid_parent.entities.push_back({.id = "child", .parent = "missing"});
    assert(!kin::instantiate_scene(world, invalid_parent, error));
    assert(error.find("unknown parent") != std::string::npos);
    assert(world.snapshot().entities.empty());

    kin::SceneDocument wrong_kind;
    wrong_kind.entities.push_back({
        .id = "e",
        .components = {{
            .name = "Stats",
            .fields = {{.name = "hp", .kind = kin::ComponentFieldKind::String, .value = std::string{"bad"}}},
        }},
    });
    assert(!kin::instantiate_scene(world, wrong_kind, error));
    assert(error.find("field kind mismatch") != std::string::npos);
    assert(world.snapshot().entities.empty());
}

} // namespace

int main() {
    test_world_snapshot_contains_hierarchy_and_components();
    test_query_plan_matches_all_and_none_deterministically();
    test_inspection_order_groups_by_parent_then_name();
    test_component_field_json_conversion();
    test_scene_document_round_trip();
    test_scene_instantiation_validates_before_mutating();
    return 0;
}
