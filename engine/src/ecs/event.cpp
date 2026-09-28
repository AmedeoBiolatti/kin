#include <kin/ecs/world.hpp>

#include <algorithm>
#include <utility>

namespace kin {

EcsEventRegistry::EcsEventRegistry(EcsWorld& world)
    : _world(&world) {
}

EcsEventRegistry::~EcsEventRegistry() {
    for (flecs::observer& observer : _component_observers) {
        if (observer.is_valid() && observer.is_alive()) {
            observer.destruct();
        }
    }
    _component_observers.clear();
}

std::vector<EcsEvent> EcsEventRegistry::since(u64 sequence) const {
    std::vector<EcsEvent> result;
    for (const EcsEvent& event : _events) {
        if (event.sequence > sequence) {
            result.push_back(event);
        }
    }
    return result;
}

void EcsEventRegistry::clear() {
    _events.clear();
    _observer_suppressions.clear();
    clear_dirty();
}

void EcsEventRegistry::clear_dirty() {
    _dirty = EcsDirtyState{.last_event_sequence = _dirty.last_event_sequence};
}

void EcsEventRegistry::begin_frame() {
    ++_frame;
}

void EcsEventRegistry::emit(EcsEvent event) {
    event.sequence = _next_sequence++;
    event.frame = _frame;
    _dirty.last_event_sequence = event.sequence;
    apply_dirty_flags(event);
    _events.push_back(std::move(event));
    trim_events();
}

void EcsEventRegistry::set_max_events(std::size_t max_events) {
    _max_events = max_events;
    if (_max_events != 0 && _events.size() > _max_events) {
        _events.erase(_events.begin(), _events.end() - static_cast<std::ptrdiff_t>(_max_events));
    }
}

void EcsEventRegistry::trim_events() {
    // Drop the oldest events in batches once the log grows past 2x the cap, so
    // trimming costs amortized O(1) per emit while still retaining at least the
    // most recent _max_events. A zero cap means the log is unbounded.
    if (_max_events == 0 || _events.size() < 2 * _max_events) {
        return;
    }
    const std::size_t excess = _events.size() - _max_events;
    _events.erase(_events.begin(), _events.begin() + static_cast<std::ptrdiff_t>(excess));
}

void EcsEventRegistry::emit_entity(EcsEventKind kind, EcsEntity entity, std::string message) {
    emit({
        .kind = kind,
        .source = EcsEventSource::KinApi,
        .entity = entity ? entity.id() : 0,
        .message = std::move(message),
    });
}

void EcsEventRegistry::emit_component(EcsEventKind kind,
                                      EcsEventSource source,
                                      EcsEntity entity,
                                      ComponentId component_id,
                                      std::string component_name,
                                      std::string field_name,
                                      std::string message) {
    const EcsId entity_id = entity ? entity.id() : 0;
    if (source == EcsEventSource::FlecsObserver && consume_observer_suppression(kind, entity_id, component_id)) {
        return;
    }
    emit({
        .kind = kind,
        .source = source,
        .entity = entity_id,
        .component_id = component_id,
        .component_name = std::move(component_name),
        .field_name = std::move(field_name),
        .message = std::move(message),
    });
}

void EcsEventRegistry::emit_system(EcsEventKind kind, std::string system_id, std::string message) {
    emit({
        .kind = kind,
        .source = EcsEventSource::KinApi,
        .system_id = std::move(system_id),
        .message = std::move(message),
    });
}

void EcsEventRegistry::suppress_next_observer(EcsEventKind kind, EcsId entity, ComponentId component_id) {
    _observer_suppressions.push_back({
        .kind = kind,
        .entity = entity,
        .component_id = component_id,
    });
}

bool EcsEventRegistry::consume_observer_suppression(EcsEventKind kind, EcsId entity, ComponentId component_id) {
    const auto found = std::ranges::find_if(_observer_suppressions, [&](const ObserverSuppression& suppression) {
        return suppression.kind == kind && suppression.entity == entity && suppression.component_id == component_id;
    });
    if (found == _observer_suppressions.end()) {
        return false;
    }
    _observer_suppressions.erase(found);
    return true;
}

void EcsEventRegistry::apply_dirty_flags(const EcsEvent& event) {
    _dirty.world_dirty = true;
    switch (event.kind) {
    case EcsEventKind::EntityCreated:
    case EcsEventKind::EntityDestroyed:
    case EcsEventKind::EntityRenamed:
    case EcsEventKind::EntityReparented:
        _dirty.hierarchy_dirty = true;
        break;
    case EcsEventKind::EntityEnabled:
    case EcsEventKind::EntityDisabled:
        break;
    case EcsEventKind::ComponentAdded:
    case EcsEventKind::ComponentRemoved:
    case EcsEventKind::ComponentChanged:
        _dirty.components_dirty = true;
        _dirty.render_dirty = true;
        break;
    case EcsEventKind::SystemAdded:
    case EcsEventKind::SystemRemoved:
    case EcsEventKind::SystemEnabled:
    case EcsEventKind::SystemDisabled:
    case EcsEventKind::SystemError:
        _dirty.systems_dirty = true;
        break;
    }
}

} // namespace kin
