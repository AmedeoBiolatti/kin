#include <kin/ecs/component.hpp>
#include <kin/ecs/system.hpp>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Transform {
    kin::Vec2f position{};
    kin::f32 rotation = 0.0f;
};

struct Velocity {
    kin::f32 value = 0.0f;
};

const kin::EcsEvent* find_event(const std::vector<kin::EcsEvent>& events, kin::EcsEventKind kind) {
    const auto found = std::ranges::find_if(events, [&](const kin::EcsEvent& event) {
        return event.kind == kind;
    });
    return found == events.end() ? nullptr : &*found;
}

const kin::EcsEvent* find_event(const std::vector<kin::EcsEvent>& events, kin::EcsEventKind kind, kin::EcsEventSource source) {
    const auto found = std::ranges::find_if(events, [&](const kin::EcsEvent& event) {
        return event.kind == kind && event.source == source;
    });
    return found == events.end() ? nullptr : &*found;
}

void register_components(kin::EcsWorld& world) {
    world.components().native<Transform>("Transform")
        .field("position", &Transform::position)
        .field("rotation", &Transform::rotation);
    world.components().native<Velocity>("Velocity")
        .field("value", &Velocity::value);
    world.events().clear();
}

void test_entity_events_and_dirty_flags() {
    kin::EcsWorld world;
    kin::EcsEntity root = world.entities().create({.name = "root", .authored_id = "root"});
    kin::EcsEntity child = world.entities().create({.name = "child", .parent = root, .enabled = false});
    assert(world.entities().rename(child, "renamed"));
    assert(world.entities().enable(child));
    assert(world.entities().disable(child));
    assert(world.entities().detach_parent(child));
    assert(world.entities().destroy(child));

    const std::vector<kin::EcsEvent>& events = world.events().snapshot();
    assert(find_event(events, kin::EcsEventKind::EntityCreated) != nullptr);
    assert(find_event(events, kin::EcsEventKind::EntityRenamed) != nullptr);
    assert(find_event(events, kin::EcsEventKind::EntityEnabled) != nullptr);
    assert(find_event(events, kin::EcsEventKind::EntityDisabled) != nullptr);
    assert(find_event(events, kin::EcsEventKind::EntityReparented) != nullptr);
    assert(find_event(events, kin::EcsEventKind::EntityDestroyed) != nullptr);
    const kin::EcsDirtyState& dirty = world.events().dirty_state();
    assert(dirty.world_dirty);
    assert(dirty.hierarchy_dirty);
    assert(dirty.last_event_sequence == events.back().sequence);
}

void test_component_events_deduplicate_kin_api_and_observers() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("actor");

    assert(world.components().add(entity, "Transform"));
    assert(world.components().patch_field(entity, "Transform", "rotation", kin::f32{90.0f}));
    assert(world.components().remove(entity, "Transform"));

    const std::vector<kin::EcsEvent>& events = world.events().snapshot();
    const kin::EcsEvent* add = find_event(events, kin::EcsEventKind::ComponentAdded, kin::EcsEventSource::KinApi);
    const kin::EcsEvent* changed = find_event(events, kin::EcsEventKind::ComponentChanged, kin::EcsEventSource::KinApi);
    const kin::EcsEvent* removed = find_event(events, kin::EcsEventKind::ComponentRemoved, kin::EcsEventSource::KinApi);
    assert(add != nullptr);
    assert(changed != nullptr);
    assert(changed->field_name == "rotation");
    assert(removed != nullptr);
    assert(world.events().dirty_state().components_dirty);
    assert(world.events().dirty_state().render_dirty);
}

void test_flecs_observer_component_events_for_raw_changes() {
    kin::EcsWorld world;
    register_components(world);
    kin::EcsEntity entity = world.entity("raw");
    world.events().clear();

    entity.add<Transform>();
    entity.set(Transform{.rotation = 12.0f});
    entity.remove<Transform>();

    const std::vector<kin::EcsEvent>& events = world.events().snapshot();
    assert(find_event(events, kin::EcsEventKind::ComponentAdded) != nullptr);
    assert(find_event(events, kin::EcsEventKind::ComponentChanged) != nullptr);
    assert(find_event(events, kin::EcsEventKind::ComponentRemoved) != nullptr);
    for (const kin::EcsEvent& event : events) {
        assert(event.source == kin::EcsEventSource::FlecsObserver);
        assert(event.component_name == "Transform");
    }
}

void test_system_events_and_errors() {
    kin::EcsWorld world;
    const kin::SystemId ok = world.systems().register_task({
        .id = "events.ok",
        .reads = {"Transform"},
    }, [](kin::SystemContext&) {});
    assert(ok == "events.ok");
    assert(world.systems().disable(ok));
    assert(world.systems().enable(ok));
    assert(world.systems().remove(ok));

    world.systems().register_task({
        .id = "events.fail",
        .reads = {"Transform"},
    }, [](kin::SystemContext&) {
        throw std::runtime_error("event failure");
    });
    assert(!world.run_system("events.fail"));

    const std::vector<kin::EcsEvent>& events = world.events().snapshot();
    assert(find_event(events, kin::EcsEventKind::SystemAdded) != nullptr);
    assert(find_event(events, kin::EcsEventKind::SystemDisabled) != nullptr);
    assert(find_event(events, kin::EcsEventKind::SystemEnabled) != nullptr);
    assert(find_event(events, kin::EcsEventKind::SystemRemoved) != nullptr);
    const kin::EcsEvent* error = find_event(events, kin::EcsEventKind::SystemError);
    assert(error != nullptr);
    assert(error->system_id == "events.fail");
    assert(error->message.find("event failure") != std::string::npos);
    assert(world.events().dirty_state().systems_dirty);
}

void test_since_clear_and_sequence_ordering() {
    kin::EcsWorld world;
    kin::EcsEntity first = world.entity("first");
    const kin::u64 sequence = world.events().dirty_state().last_event_sequence;
    kin::EcsEntity second = world.entity("second");
    (void)first;
    (void)second;

    const std::vector<kin::EcsEvent> since = world.events().since(sequence);
    assert(since.size() == 1);
    assert(since[0].entity == second.id());
    assert(since[0].sequence > sequence);

    world.events().clear_dirty();
    assert(!world.events().dirty_state().world_dirty);
    assert(world.events().dirty_state().last_event_sequence == since[0].sequence);

    world.events().clear();
    assert(world.events().snapshot().empty());
    assert(!world.events().dirty_state().world_dirty);
    kin::EcsEntity third = world.entity("third");
    assert(world.events().snapshot().back().entity == third.id());
    assert(world.events().snapshot().back().sequence > since[0].sequence);
}

} // namespace

int main() {
    test_entity_events_and_dirty_flags();
    test_component_events_deduplicate_kin_api_and_observers();
    test_flecs_observer_component_events_for_raw_changes();
    test_system_events_and_errors();
    test_since_clear_and_sequence_ordering();
    return 0;
}
