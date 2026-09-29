#include <kin/ecs/system.hpp>

#include <kin/core/profile.hpp>
#include <kin/ecs/component.hpp>
#include <kin/platform/log.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kin {
namespace {

constexpr std::array<SystemPhase, system_phase_count> phases{{
    SystemPhase::Input,
    SystemPhase::PreUpdate,
    SystemPhase::Update,
    SystemPhase::PostUpdate,
    SystemPhase::PrePhysics,
    SystemPhase::Physics,
    SystemPhase::PostPhysics,
    SystemPhase::Animation,
    SystemPhase::Audio,
    SystemPhase::PreRender,
    SystemPhase::Render,
    SystemPhase::PostRender,
    SystemPhase::Editor,
}};

constexpr std::size_t phase_index(SystemPhase phase) {
    return static_cast<std::size_t>(phase);
}

std::string phase_entity_name(SystemPhase phase) {
    std::string name = "kin::ecs::phase::";
    name += system_phase_name(phase);
    return name;
}

flecs::entity entity_from_id(EcsWorld& world, flecs::entity_t id) {
    return flecs::entity{world.raw().c_ptr(), id};
}

bool validate_system_contract(EcsWorld& world, const SystemDescriptor& descriptor, std::string& error) {
    if (descriptor.execution_policy == SystemExecutionPolicy::ParallelEligible &&
        descriptor.structural_mutation_policy == SystemStructuralMutationPolicy::RawWorldAllowed) {
        error = "parallel-eligible system '" + descriptor.id + "' cannot allow raw world mutation";
        return false;
    }
    if (descriptor.execution_policy == SystemExecutionPolicy::ParallelEligible) {
        for (const std::string& name : descriptor.reads) {
            if (!world.components().find(name)) {
                error = "parallel-eligible system '" + descriptor.id + "' reads unknown component '" + name + "'";
                return false;
            }
        }
        for (const std::string& name : descriptor.writes) {
            if (!world.components().find(name)) {
                error = "parallel-eligible system '" + descriptor.id + "' writes unknown component '" + name + "'";
                return false;
            }
        }
        for (const SystemRelationAccess& access : descriptor.relations) {
            if (!world.relations().find(access.relation)) {
                error = "parallel-eligible system '" + descriptor.id + "' references unknown relation '" + access.relation + "'";
                return false;
            }
        }
    }
    return true;
}

struct FlecsReadonlyStageGuard {
    flecs::world& world;
    i32 previous_stage_count = 0;
    bool readonly = false;

    FlecsReadonlyStageGuard(flecs::world& target, i32 stage_count)
        : world(target),
          previous_stage_count(target.get_stage_count()) {
        world.set_stage_count(stage_count);
    }

    FlecsReadonlyStageGuard(const FlecsReadonlyStageGuard&) = delete;
    FlecsReadonlyStageGuard& operator=(const FlecsReadonlyStageGuard&) = delete;

    ~FlecsReadonlyStageGuard() noexcept {
        std::string ignored;
        (void)cleanup(&ignored);
    }

    bool cleanup(std::string* error = nullptr) noexcept {
        if (readonly) {
            try {
                world.readonly_end();
                readonly = false;
            } catch (const std::exception& exception) {
                if (error && error->empty()) {
                    *error = exception.what();
                }
                return false;
            } catch (...) {
                if (error && error->empty()) {
                    *error = "unknown Flecs readonly cleanup error";
                }
                return false;
            }
        }
        try {
            world.set_stage_count(previous_stage_count);
        } catch (const std::exception& exception) {
            if (error && error->empty()) {
                *error = exception.what();
            }
            return false;
        } catch (...) {
            if (error && error->empty()) {
                *error = "unknown Flecs stage cleanup error";
            }
            return false;
        }
        return true;
    }

    void begin_readonly() {
        world.readonly_begin(true);
        readonly = true;
    }

    bool end_readonly(std::string* error = nullptr) noexcept {
        if (readonly) {
            try {
                world.readonly_end();
                readonly = false;
            } catch (const std::exception& exception) {
                if (error && error->empty()) {
                    *error = exception.what();
                }
                return false;
            } catch (...) {
                if (error && error->empty()) {
                    *error = "unknown Flecs readonly cleanup error";
                }
                return false;
            }
        }
        return true;
    }
};

struct TaskSystemCallbackState final : detail::SystemCallbackState {
    std::function<void(SystemContext&)> run;
    SystemContext context;

    void set_context(SystemContext next) override {
        context = next;
    }

    void run_task() override {
        run(context);
    }
};

} // namespace

EcsDeferredEntity EcsCommandBuffer::create_entity(std::string name) {
    const EcsDeferredEntity deferred{_next_deferred_entity++};
    _commands.push_back([index = deferred.index, name = std::move(name)](EcsWorld& world,
                                                                         std::vector<EcsEntity>& created,
                                                                         std::string&) {
        if (created.size() <= index) {
            created.resize(static_cast<std::size_t>(index) + 1);
        }
        created[index] = world.entity(name);
        return true;
    });
    return deferred;
}

void EcsCommandBuffer::destroy(EcsEntity entity) {
    destroy(entity.id());
}

void EcsCommandBuffer::destroy(EcsId entity) {
    add_command({TargetKind::ExistingId, entity, 0}, [](EcsEntity target) {
        target.destroy();
    });
}

void EcsCommandBuffer::destroy(EcsDeferredEntity entity) {
    add_command({TargetKind::Deferred, 0, entity.index}, [](EcsEntity target) {
        target.destroy();
    });
}

void EcsCommandBuffer::add(EcsEntity entity, std::string component) {
    add(entity.id(), std::move(component));
}

void EcsCommandBuffer::add(EcsId entity, std::string component) {
    add_world_command({TargetKind::ExistingId, entity, 0},
                      [component = std::move(component)](EcsWorld& world, EcsEntity target, std::string& error) {
                          if (!world.components().add(target, component)) {
                              error = world.components().last_error();
                              return false;
                          }
                          return true;
                      });
}

void EcsCommandBuffer::add(EcsDeferredEntity entity, std::string component) {
    add_world_command({TargetKind::Deferred, 0, entity.index},
                      [component = std::move(component)](EcsWorld& world, EcsEntity target, std::string& error) {
                          if (!world.components().add(target, component)) {
                              error = world.components().last_error();
                              return false;
                          }
                          return true;
                      });
}

void EcsCommandBuffer::remove(EcsEntity entity, std::string component) {
    remove(entity.id(), std::move(component));
}

void EcsCommandBuffer::remove(EcsId entity, std::string component) {
    add_world_command({TargetKind::ExistingId, entity, 0},
                      [component = std::move(component)](EcsWorld& world, EcsEntity target, std::string& error) {
                          if (!world.components().remove(target, component)) {
                              error = world.components().last_error();
                              return false;
                          }
                          return true;
                      });
}

void EcsCommandBuffer::remove(EcsDeferredEntity entity, std::string component) {
    add_world_command({TargetKind::Deferred, 0, entity.index},
                      [component = std::move(component)](EcsWorld& world, EcsEntity target, std::string& error) {
                          if (!world.components().remove(target, component)) {
                              error = world.components().last_error();
                              return false;
                          }
                          return true;
                      });
}

void EcsCommandBuffer::patch_field(EcsEntity entity, std::string component, std::string field, ComponentFieldValue value) {
    patch_field(entity.id(), std::move(component), std::move(field), std::move(value));
}

void EcsCommandBuffer::patch_field(EcsId entity, std::string component, std::string field, ComponentFieldValue value) {
    add_world_command({TargetKind::ExistingId, entity, 0},
                      [component = std::move(component), field = std::move(field), value = std::move(value)](EcsWorld& world,
                                                                                                            EcsEntity target,
                                                                                                            std::string& error) {
                          if (!world.components().has(target, component) && !world.components().add(target, component)) {
                              error = world.components().last_error();
                              return false;
                          }
                          if (!world.components().patch_field(target, component, field, value)) {
                              error = world.components().last_error();
                              return false;
                          }
                          return true;
                      });
}

void EcsCommandBuffer::patch_field(EcsDeferredEntity entity, std::string component, std::string field, ComponentFieldValue value) {
    add_world_command({TargetKind::Deferred, 0, entity.index},
                      [component = std::move(component), field = std::move(field), value = std::move(value)](EcsWorld& world,
                                                                                                            EcsEntity target,
                                                                                                            std::string& error) {
                          if (!world.components().has(target, component) && !world.components().add(target, component)) {
                              error = world.components().last_error();
                              return false;
                          }
                          if (!world.components().patch_field(target, component, field, value)) {
                              error = world.components().last_error();
                              return false;
                          }
                          return true;
                      });
}

void EcsCommandBuffer::clear() {
    _commands.clear();
    _next_deferred_entity = 0;
}

bool EcsCommandBuffer::flush(EcsWorld& world, std::string& error) {
    std::vector<EcsEntity> created(_next_deferred_entity);
    for (Command& command : _commands) {
        try {
            if (!command(world, created, error)) {
                return false;
            }
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        } catch (...) {
            error = "unknown ECS command error";
            return false;
        }
    }
    clear();
    return true;
}

EcsEntity EcsCommandBuffer::entity_from_id(EcsWorld& world, EcsId id) {
    return EcsEntity{flecs::entity{world.raw().c_ptr(), static_cast<flecs::entity_t>(id)}};
}

bool EcsCommandBuffer::resolve_target(EcsWorld& world,
                                      const std::vector<EcsEntity>& created,
                                      Target target,
                                      EcsEntity& entity,
                                      std::string& error) {
    if (target.kind == TargetKind::Deferred) {
        if (target.deferred_index >= created.size()) {
            error = "unknown deferred entity";
            return false;
        }
        entity = created[target.deferred_index];
    } else {
        entity = entity_from_id(world, target.entity);
    }
    if (!entity.valid() || !entity.alive()) {
        error = "ECS command target entity is not alive";
        return false;
    }
    return true;
}

void EcsCommandBuffer::add_command(Target target, std::function<void(EcsEntity)> apply) {
    _commands.push_back([target, apply = std::move(apply)](EcsWorld& world,
                                                           std::vector<EcsEntity>& created,
                                                           std::string& error) {
        EcsEntity entity;
        if (!resolve_target(world, created, target, entity, error)) {
            return false;
        }
        apply(entity);
        return true;
    });
}

void EcsCommandBuffer::add_world_command(Target target, std::function<bool(EcsWorld&, EcsEntity, std::string&)> apply) {
    _commands.push_back([target, apply = std::move(apply)](EcsWorld& world,
                                                           std::vector<EcsEntity>& created,
                                                           std::string& error) {
        EcsEntity entity;
        if (!resolve_target(world, created, target, entity, error)) {
            return false;
        }
        return apply(world, entity, error);
    });
}

std::string_view system_phase_name(SystemPhase phase) {
    switch (phase) {
    case SystemPhase::Input: return "input";
    case SystemPhase::PreUpdate: return "pre_update";
    case SystemPhase::Update: return "update";
    case SystemPhase::PostUpdate: return "post_update";
    case SystemPhase::PrePhysics: return "pre_physics";
    case SystemPhase::Physics: return "physics";
    case SystemPhase::PostPhysics: return "post_physics";
    case SystemPhase::Animation: return "animation";
    case SystemPhase::Audio: return "audio";
    case SystemPhase::PreRender: return "pre_render";
    case SystemPhase::Render: return "render";
    case SystemPhase::PostRender: return "post_render";
    case SystemPhase::Editor: return "editor";
    }
    return "unknown";
}

std::string_view system_execution_mode_name(SystemExecutionMode mode) {
    switch (mode) {
    case SystemExecutionMode::SerialGraph: return "serial_graph";
    case SystemExecutionMode::ParallelBatches: return "parallel_batches";
    }
    return "unknown";
}

std::string_view system_execution_decision_name(SystemExecutionDecision decision) {
    switch (decision) {
    case SystemExecutionDecision::Serial: return "serial";
    case SystemExecutionDecision::ParallelAcrossSystems: return "parallel_across_systems";
    case SystemExecutionDecision::ParallelWithinNativeSystem: return "parallel_within_native_system";
    }
    return "unknown";
}

std::array<SystemPhase, system_phase_count> system_phases() {
    return phases;
}

EcsWorld::EcsWorld()
    : _events(std::make_unique<EcsEventRegistry>(*this)),
      _relations(std::make_unique<EcsRelationRegistry>(*this)),
      _components(std::make_unique<EcsComponentRegistry>(*this)),
      _entities(std::make_unique<EcsEntityRegistry>(*this)),
      _systems(std::make_unique<EcsSystemRegistry>(*this)) {
}

EcsWorld::~EcsWorld() = default;

EcsWorld::EcsWorld(EcsWorld&& other) noexcept
    : _world(std::move(other._world)),
      _events(std::make_unique<EcsEventRegistry>(*this)),
      _relations(std::make_unique<EcsRelationRegistry>(*this)),
      _components(std::make_unique<EcsComponentRegistry>(*this)),
      _entities(std::make_unique<EcsEntityRegistry>(*this)),
      _systems(std::make_unique<EcsSystemRegistry>(*this)) {
}

EcsWorld& EcsWorld::operator=(EcsWorld&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    _events.reset();
    _relations.reset();
    _components.reset();
    _entities.reset();
    _systems.reset();
    _world = std::move(other._world);
    _events = std::make_unique<EcsEventRegistry>(*this);
    _relations = std::make_unique<EcsRelationRegistry>(*this);
    _components = std::make_unique<EcsComponentRegistry>(*this);
    _entities = std::make_unique<EcsEntityRegistry>(*this);
    _systems = std::make_unique<EcsSystemRegistry>(*this);
    return *this;
}

EcsComponentRegistry& EcsWorld::components() {
    return *_components;
}

const EcsComponentRegistry& EcsWorld::components() const {
    return *_components;
}

EcsEventRegistry& EcsWorld::events() {
    return *_events;
}

const EcsEventRegistry& EcsWorld::events() const {
    return *_events;
}

EcsRelationRegistry& EcsWorld::relations() {
    return *_relations;
}

const EcsRelationRegistry& EcsWorld::relations() const {
    return *_relations;
}

EcsSystemRegistry& EcsWorld::systems() {
    return *_systems;
}

const EcsSystemRegistry& EcsWorld::systems() const {
    return *_systems;
}

bool EcsWorld::run_system(std::string_view id, f32 dt) {
    return _systems->run_system(id, dt);
}

bool EcsWorld::run_phase(SystemPhase phase, f32 dt) {
    return _systems->run_phase(phase, dt);
}

bool EcsWorld::run_phase(SystemPhase phase, f32 dt, SystemExecutionMode mode) {
    return _systems->run_phase(phase, dt, mode);
}

bool EcsWorld::run_frame(f32 dt) {
    _events->begin_frame();
    return _systems->run_frame(dt);
}

bool EcsWorld::run_frame(f32 dt, SystemExecutionMode mode) {
    _events->begin_frame();
    return _systems->run_frame(dt, mode);
}

EcsSystemRegistry::EcsSystemRegistry(EcsWorld& world)
    : _world(&world), _jobs(&default_job_system()) {
    for (SystemPhase phase : phases) {
        const std::string name = phase_entity_name(phase);
        _phase_entities[phase_index(phase)] = _world->raw().entity(name.c_str());
    }
}

EcsSystemRegistry::~EcsSystemRegistry() = default;

bool EcsSystemRegistry::contains(std::string_view id) const {
    return find_record(id) != nullptr;
}

void EcsSystemRegistry::publish_execution_status(const std::function<void(std::string_view, std::string)>& sink) const {
    if (!sink) {
        return;
    }
    for (const SystemRecord& record : _records) {
        if (!record.enabled || record.descriptor.execution_policy != SystemExecutionPolicy::ParallelEligible) {
            continue;
        }
        std::string value;
        switch (record.last_execution_decision) {
            case SystemExecutionDecision::ParallelWithinNativeSystem:
                value = "parallel x" + std::to_string(std::max(record.worker_count, 1));
                break;
            case SystemExecutionDecision::ParallelAcrossSystems:
                value = "parallel batch";
                break;
            case SystemExecutionDecision::Serial:
                value = record.execution_decision_reason.empty()
                    ? "serial"
                    : "serial: " + record.execution_decision_reason;
                break;
        }
        sink("exec." + record.descriptor.id, std::move(value));
    }
}

SystemId EcsSystemRegistry::registration_failure(std::string error, const std::source_location& location) {
    _last_error = std::move(error);
    KIN_LOG_ERROR_F("ecs",
                    "system registration failed",
                    (LogFields{
                        {.name = "reason", .value = _last_error},
                        {.name = "source", .value = std::string{location.file_name()} + ":" + std::to_string(location.line())},
                    }));
    return {};
}

SystemId EcsSystemRegistry::register_task(SystemDescriptor descriptor,
                                          std::function<void(SystemContext&)> run,
                                          std::source_location location) {
    if (descriptor.id.empty()) {
        return registration_failure("system id is required", location);
    }
    if (!run) {
        return registration_failure("task system '" + descriptor.id + "' has no run callback", location);
    }
    if (contains(descriptor.id)) {
        return registration_failure("duplicate system id '" + descriptor.id + "'", location);
    }
    if (descriptor.interval_seconds < 0.0f) {
        return registration_failure("system '" + descriptor.id + "' interval_seconds must be >= 0", location);
    }
    if (descriptor.rate < 1) {
        return registration_failure("system '" + descriptor.id + "' rate must be >= 1", location);
    }
    std::string access_error;
    if (!detail::normalize_descriptor_access(descriptor, access_error)) {
        return registration_failure(std::move(access_error), location);
    }
    if (descriptor.name.empty()) {
        descriptor.name = descriptor.id;
    }
    if (descriptor.source_file.empty()) {
        descriptor.source_file = location.file_name();
    }
    if (descriptor.kind == SystemKind::Native) {
        descriptor.kind = SystemKind::DataNative;
    }
    std::string contract_error;
    if (!validate_system_contract(*_world, descriptor, contract_error)) {
        return registration_failure(std::move(contract_error), location);
    }

    auto state = std::make_shared<TaskSystemCallbackState>();
    state->run = std::move(run);
    SystemRecord record{
        .descriptor = std::move(descriptor),
        .flecs_id = 0,
        .callback_state = std::move(state),
        .source_location = location,
        .registration_sequence = _next_registration_sequence++,
        .enabled = true,
    };
    record.enabled = record.descriptor.enabled;
    const SystemId id = record.descriptor.id;
    _records.push_back(std::move(record));
    _world->events().emit_system(EcsEventKind::SystemAdded, id);
    _last_error.clear();
    return id;
}

bool EcsSystemRegistry::enable(std::string_view id, bool enabled) {
    SystemRecord* record = find_record(id);
    if (!record) {
        _last_error = "unknown system id '" + std::string{id} + "'";
        return false;
    }
    record->enabled = enabled;
    record->descriptor.enabled = enabled;
    if (record->flecs_id != 0) {
        if (enabled) {
            entity_from_id(*_world, record->flecs_id).enable();
        } else {
            entity_from_id(*_world, record->flecs_id).disable();
        }
    }
    _world->events().emit_system(enabled ? EcsEventKind::SystemEnabled : EcsEventKind::SystemDisabled, std::string{id});
    _last_error.clear();
    return true;
}

bool EcsSystemRegistry::disable(std::string_view id) {
    return enable(id, false);
}

bool EcsSystemRegistry::remove(std::string_view id) {
    const auto found = std::ranges::find_if(_records, [&](const SystemRecord& record) {
        return record.descriptor.id == id;
    });
    if (found == _records.end()) {
        _last_error = "unknown system id '" + std::string{id} + "'";
        return false;
    }

    if (found->flecs_id != 0) {
        entity_from_id(*_world, found->flecs_id).destruct();
    }
    _world->events().emit_system(EcsEventKind::SystemRemoved, std::string{id});
    _records.erase(found);
    _last_error.clear();
    return true;
}

bool EcsSystemRegistry::run_system(std::string_view id, f32 dt) {
    SystemRecord* record = find_record(id);
    if (!record) {
        _last_error = "unknown system id '" + std::string{id} + "'";
        return false;
    }
    return run_record(*record, dt, false);
}

bool EcsSystemRegistry::rebuild_schedule() {
    _last_schedule = build_schedule(enabled_records());
    apply_schedule_diagnostics(_last_schedule);
    if (!_last_schedule.valid && !_last_schedule.diagnostics.empty()) {
        _last_error = _last_schedule.diagnostics.front();
        return false;
    }
    _last_error.clear();
    return true;
}

bool EcsSystemRegistry::run_phase(SystemPhase phase, f32 dt) {
    return run_phase(phase, dt, SystemExecutionMode::SerialGraph);
}

bool EcsSystemRegistry::run_phase(SystemPhase phase, f32 dt, SystemExecutionMode mode) {
    _last_schedule = build_schedule(eligible_records(&phase, dt), mode);
    apply_schedule_diagnostics(_last_schedule);
    if (!_last_schedule.valid) {
        _last_error = _last_schedule.diagnostics.empty() ? "invalid system schedule" : _last_schedule.diagnostics.front();
        return false;
    }
    const bool ok = run_schedule(_last_schedule, dt);
    if (ok) {
        _last_error.clear();
    }
    return ok;
}

bool EcsSystemRegistry::run_frame(f32 dt) {
    return run_frame(dt, SystemExecutionMode::SerialGraph);
}

bool EcsSystemRegistry::run_frame(f32 dt, SystemExecutionMode mode) {
    _last_schedule = build_schedule(eligible_records(nullptr, dt), mode);
    apply_schedule_diagnostics(_last_schedule);
    if (!_last_schedule.valid) {
        _last_error = _last_schedule.diagnostics.empty() ? "invalid system schedule" : _last_schedule.diagnostics.front();
        return false;
    }
    const bool ok = run_schedule(_last_schedule, dt);
    if (ok) {
        _last_error.clear();
    }
    return ok;
}

std::vector<SystemSnapshot> EcsSystemRegistry::snapshots() const {
    std::vector<SystemSnapshot> result;
    for (const SystemRecord* record : sorted_records()) {
        result.push_back(snapshot(record->descriptor.id));
    }
    return result;
}

SystemSnapshot EcsSystemRegistry::snapshot(std::string_view id) const {
    const SystemRecord* record = find_record(id);
    if (!record) {
        return {};
    }
    return {
        .id = record->descriptor.id,
        .name = record->descriptor.name,
        .kind = record->descriptor.kind,
        .enabled = record->enabled,
        .phase = record->descriptor.phase,
        .order = record->descriptor.order,
        .interval_seconds = record->descriptor.interval_seconds,
        .rate = record->descriptor.rate,
        .reads = record->descriptor.reads,
        .writes = record->descriptor.writes,
        .relations = record->descriptor.relations,
        .before = record->descriptor.before,
        .after = record->descriptor.after,
        .execution_policy = record->descriptor.execution_policy,
        .structural_mutation_policy = record->descriptor.structural_mutation_policy,
        .parallel_eligible = record->parallel_eligible,
        .mutation_notes = record->descriptor.mutation_notes,
        .stats = record->stats,
        .command_stats = record->command_stats,
        .last_error = record->last_error,
        .schedule_diagnostics = record->schedule_diagnostics,
        .parallel_diagnostics = record->parallel_diagnostics,
        .batch_index = record->batch_index,
        .last_execution_mode = record->last_execution_mode,
        .last_batch_execution_mode = record->last_batch_execution_mode,
        .last_execution_decision = record->last_execution_decision,
        .execution_decision_reason = record->execution_decision_reason,
        .worker_count = record->worker_count,
        .estimated_work_ms = record->estimated_work_ms,
        .source_file = record->descriptor.source_file,
        .owner_scope = record->descriptor.owner_scope,
    };
}

flecs::entity EcsSystemRegistry::phase_entity(SystemPhase phase) const {
    return _phase_entities[phase_index(phase)];
}

SystemId EcsSystemRegistry::register_created_system(SystemDescriptor descriptor,
                                                    flecs::system system,
                                                    std::shared_ptr<detail::SystemCallbackState> callback_state,
                                                    std::source_location location) {
    std::string contract_error;
    if (!validate_system_contract(*_world, descriptor, contract_error)) {
        _last_error = std::move(contract_error);
        entity_from_id(*_world, system.id()).destruct();
        return {};
    }
    SystemRecord record{
        .descriptor = std::move(descriptor),
        .flecs_id = system.id(),
        .callback_state = std::move(callback_state),
        .source_location = location,
        .registration_sequence = _next_registration_sequence++,
        .enabled = true,
    };
    record.enabled = record.descriptor.enabled;
    if (!record.enabled) {
        system.disable();
    }
    const SystemId id = record.descriptor.id;
    _records.push_back(std::move(record));
    _world->events().emit_system(EcsEventKind::SystemAdded, id);
    _last_error.clear();
    return id;
}

EcsSystemRegistry::SystemRecord* EcsSystemRegistry::find_record(std::string_view id) {
    const auto found = std::ranges::find_if(_records, [&](const SystemRecord& record) {
        return record.descriptor.id == id;
    });
    return found == _records.end() ? nullptr : &*found;
}

const EcsSystemRegistry::SystemRecord* EcsSystemRegistry::find_record(std::string_view id) const {
    const auto found = std::ranges::find_if(_records, [&](const SystemRecord& record) {
        return record.descriptor.id == id;
    });
    return found == _records.end() ? nullptr : &*found;
}

bool EcsSystemRegistry::run_record(SystemRecord& record, f32 dt, bool respect_schedule) {
    EcsCommandBuffer commands;
    SystemRunResult result = execute_record(record, dt, respect_schedule, &commands);
    record.last_execution_mode = result.execution_mode;
    record.last_batch_execution_mode = SystemExecutionMode::SerialGraph;
    record.last_execution_decision = result.decision;
    record.execution_decision_reason = result.decision_reason;
    record.worker_count = result.worker_count;
    record.estimated_work_ms = result.estimated_work_ms;
    if (!result.ok) {
        _last_error = result.error;
        record.command_stats.discarded += commands.size();
        KIN_LOG_ERROR_F("ecs",
                        "system run failed",
                        (LogFields{
                            {.name = "system", .value = record.descriptor.id},
                            {.name = "error", .value = result.error},
                        }));
        _world->events().emit_system(EcsEventKind::SystemError, record.descriptor.id, result.error);
        return false;
    }
    if (!flush_commands_for_result(nullptr, nullptr, result)) {
        return false;
    }
    if (result.elapsed_ns > 0) {
        if (ProfileSession* profile = current_profile_session()) {
            profile->record(record.descriptor.id, "ecs.system", static_cast<u64>(result.elapsed_ns), record.source_location);
        }
    }
    return true;
}

EcsSystemRegistry::SystemRunResult EcsSystemRegistry::execute_record(SystemRecord& record,
                                                                     f32 dt,
                                                                     bool respect_schedule,
                                                                     EcsCommandBuffer* commands,
                                                                     bool running_parallel,
                                                                     flecs::world* stage) {
    SystemRunResult result{
        .record = &record,
        .commands = commands,
        .execution_mode = running_parallel ? SystemExecutionMode::ParallelBatches : SystemExecutionMode::SerialGraph,
        .decision = running_parallel ? SystemExecutionDecision::ParallelAcrossSystems : SystemExecutionDecision::Serial,
        .decision_reason = running_parallel ? "task batch selected parallel execution" : "serial execution",
        .worker_count = running_parallel ? 1 : 1,
        .estimated_work_ms = record.stats.average_duration_ms,
    };
    if (!record.enabled) {
        return result;
    }
    if (respect_schedule && !scheduled_run_due(record, dt)) {
        return result;
    }

    try {
        update_matched_count(record);
        record.callback_state->clear_error();
        record.callback_state->set_context(SystemContext{
            .world = _world,
            .commands = commands,
            .system_id = record.descriptor.id,
            .phase = record.descriptor.phase,
            .dt = dt,
            .running_parallel = running_parallel,
        });

        const auto start = std::chrono::steady_clock::now();
        if (record.flecs_id != 0) {
            if (stage) {
                auto runner = _world->raw().system(entity_from_id(*_world, record.flecs_id)).run_worker(0, 1, dt);
                runner.stage(*stage);
            } else {
                _world->raw().system(entity_from_id(*_world, record.flecs_id)).run(dt);
            }
        } else {
            record.callback_state->run_task();
        }
        if (std::string callback_error = record.callback_state->take_error(); !callback_error.empty()) {
            record.last_error = std::move(callback_error);
            result.ok = false;
            result.error = record.last_error;
            return result;
        }
        const auto end = std::chrono::steady_clock::now();
        result.elapsed_ns = std::max<i64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(), 0);
        const f64 elapsed_ms = static_cast<f64>(result.elapsed_ns) / 1'000'000.0;

        ++record.stats.runs;
        record.stats.last_duration_ms = elapsed_ms;
        record.stats.max_duration_ms = std::max(record.stats.max_duration_ms, elapsed_ms);
        record.total_duration_ms += elapsed_ms;
        record.stats.average_duration_ms = record.stats.runs > 0
            ? record.total_duration_ms / static_cast<f64>(record.stats.runs)
            : 0.0;
        record.last_error.clear();
        return result;
    } catch (const std::exception& error) {
        record.last_error = error.what();
        result.ok = false;
        result.error = record.last_error;
        return result;
    } catch (...) {
        record.last_error = "unknown system error";
        result.ok = false;
        result.error = record.last_error;
        return result;
    }
}

EcsSystemRegistry::SystemRunResult EcsSystemRegistry::execute_native_parallel_record(SystemRecord& record,
                                                                                    f32 dt,
                                                                                    EcsCommandBuffer* commands) {
    SystemRunResult result{
        .record = &record,
        .commands = commands,
        .execution_mode = SystemExecutionMode::SerialGraph,
        .decision = SystemExecutionDecision::Serial,
        .decision_reason = "serial native execution",
        .worker_count = 1,
        .estimated_work_ms = record.stats.average_duration_ms,
    };
    if (!record.enabled) {
        return result;
    }
    if (record.flecs_id == 0) {
        return execute_record(record, dt, false, commands, true);
    }

    try {
        update_matched_count(record);
        record.callback_state->clear_error();
        const i32 matched_entities = std::max(record.stats.matched_entities, 0);
        const f64 estimated_ms = record.stats.average_duration_ms;
        result.estimated_work_ms = estimated_ms;
        if (matched_entities < std::max(_execution_tuning.min_native_entities, 1)) {
            SystemRunResult serial = execute_record(record, dt, false, commands, false);
            serial.decision_reason = "native matched entity count below threshold";
            serial.estimated_work_ms = estimated_ms;
            return serial;
        }
        // The job system's workers plus this thread, which joins in.
        const i32 available_threads = _jobs->worker_count() + 1;
        const i32 worker_limit = _execution_tuning.max_workers > 0
            ? std::min(_execution_tuning.max_workers, available_threads)
            : available_threads;
        const i32 worker_count = std::max(1, std::min(matched_entities, std::max(worker_limit, 1)));
        if (worker_count <= 1) {
            SystemRunResult serial = execute_record(record, dt, false, commands, false);
            serial.decision_reason = "native worker count resolved to one";
            serial.estimated_work_ms = estimated_ms;
            return serial;
        }
        result.execution_mode = SystemExecutionMode::ParallelBatches;
        result.decision = SystemExecutionDecision::ParallelWithinNativeSystem;
        result.decision_reason = "native matched entity count and estimate selected Flecs workers";
        result.worker_count = worker_count;

        record.callback_state->set_context(SystemContext{
            .world = _world,
            .commands = nullptr,
            .system_id = record.descriptor.id,
            .phase = record.descriptor.phase,
            .dt = dt,
            .running_parallel = true,
        });

        FlecsReadonlyStageGuard stage_guard{_world->raw(), worker_count};
        std::vector<flecs::world> stages;
        stages.reserve(static_cast<std::size_t>(worker_count));
        for (i32 i = 0; i < worker_count; ++i) {
            stages.push_back(_world->raw().get_stage(i));
        }

        const auto start = std::chrono::steady_clock::now();
        stage_guard.begin_readonly();
        std::mutex error_mutex;
        std::string worker_error;
        try {
            _jobs->parallel_for(worker_count, [&](i32 i) {
                try {
                    auto runner = _world->raw().system(entity_from_id(*_world, record.flecs_id)).run_worker(i, worker_count, dt);
                    runner.stage(stages[static_cast<std::size_t>(i)]);
                } catch (const std::exception& error) {
                    std::lock_guard lock{error_mutex};
                    if (worker_error.empty()) {
                        worker_error = error.what();
                    }
                } catch (...) {
                    std::lock_guard lock{error_mutex};
                    if (worker_error.empty()) {
                        worker_error = "unknown native system worker error";
                    }
                }
            });
        } catch (...) {
            stages.clear();
            throw;
        }
        std::string cleanup_error;
        if (!stage_guard.end_readonly(&cleanup_error)) {
            worker_error = worker_error.empty() ? cleanup_error : worker_error;
        }
        stages.clear();
        if (!stage_guard.cleanup(&cleanup_error)) {
            worker_error = worker_error.empty() ? cleanup_error : worker_error;
        }
        const auto end = std::chrono::steady_clock::now();

        std::string callback_error = record.callback_state->take_error();
        if (!worker_error.empty() || !callback_error.empty()) {
            record.last_error = !worker_error.empty() ? worker_error : std::move(callback_error);
            result.ok = false;
            result.error = record.last_error;
            return result;
        }

        result.elapsed_ns = std::max<i64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(), 0);
        const f64 elapsed_ms = static_cast<f64>(result.elapsed_ns) / 1'000'000.0;
        ++record.stats.runs;
        record.stats.last_duration_ms = elapsed_ms;
        record.stats.max_duration_ms = std::max(record.stats.max_duration_ms, elapsed_ms);
        record.total_duration_ms += elapsed_ms;
        record.stats.average_duration_ms = record.stats.runs > 0
            ? record.total_duration_ms / static_cast<f64>(record.stats.runs)
            : 0.0;
        record.last_error.clear();
        return result;
    } catch (const std::exception& error) {
        record.last_error = error.what();
        result.ok = false;
        result.error = record.last_error;
        return result;
    } catch (...) {
        record.last_error = "unknown native system error";
        result.ok = false;
        result.error = record.last_error;
        return result;
    }
}

bool EcsSystemRegistry::flush_commands_for_result(SystemScheduleSnapshot* schedule, SystemBatch* batch, const SystemRunResult& result) {
    if (!result.record || !result.commands || result.commands->empty()) {
        return true;
    }
    const u64 queued = static_cast<u64>(result.commands->size());
    result.record->command_stats.queued += queued;
    if (batch) {
        batch->command_stats.queued += queued;
    }
    std::string error;
    if (result.commands->flush(*_world, error)) {
        result.record->command_stats.flushed += queued;
        if (batch) {
            batch->command_stats.flushed += queued;
        }
        return true;
    }
    _last_error = "system '" + result.record->descriptor.id + "' command flush failed: " + error;
    result.record->last_error = _last_error;
    KIN_LOG_ERROR_F("ecs",
                    "system command flush failed",
                    (LogFields{
                        {.name = "system", .value = result.record->descriptor.id},
                        {.name = "error", .value = _last_error},
                    }));
    _world->events().emit_system(EcsEventKind::SystemError, result.record->descriptor.id, _last_error);
    result.record->command_stats.failed += queued;
    if (batch) {
        batch->command_stats.failed += queued;
    }
    if (schedule) {
        schedule->diagnostics.push_back(_last_error);
    }
    return false;
}

bool EcsSystemRegistry::flush_command_results(SystemScheduleSnapshot& schedule, SystemBatch& batch, const std::vector<SystemRunResult>& results) {
    bool ok = true;
    for (const SystemRunResult& result : results) {
        if (!result.ok) {
            if (result.record && result.commands) {
                const u64 discarded = static_cast<u64>(result.commands->size());
                result.record->command_stats.discarded += discarded;
                batch.command_stats.discarded += discarded;
            }
            continue;
        }
        ok = flush_commands_for_result(&schedule, &batch, result) && ok;
    }
    return ok;
}

bool EcsSystemRegistry::scheduled_run_due(SystemRecord& record, f32 dt) {
    ++record.scheduled_ticks;
    const i32 rate = std::max(record.descriptor.rate, 1);
    const bool rate_due = ((record.scheduled_ticks - 1) % static_cast<u64>(rate)) == 0;

    bool interval_due = true;
    const f32 interval = record.descriptor.interval_seconds;
    if (interval > 0.0f) {
        record.interval_accumulator += std::max(dt, 0.0f);
        interval_due = record.interval_accumulator >= interval;
    }

    if (!rate_due || !interval_due) {
        return false;
    }
    if (interval > 0.0f) {
        record.interval_accumulator -= interval;
    }
    return true;
}

void EcsSystemRegistry::update_matched_count(SystemRecord& record) const {
    record.stats.matched_entities = record.flecs_id != 0
        ? _world->raw().system(entity_from_id(*_world, record.flecs_id)).query().count()
        : 0;
}

std::vector<SystemParallelDiagnostic> EcsSystemRegistry::parallel_eligibility_reasons(const SystemRecord& record) const {
    std::vector<SystemParallelDiagnostic> reasons;
    const auto add_reason = [&](std::string code, std::string message) {
        reasons.push_back({
            .system = record.descriptor.id,
            .code = std::move(code),
            .message = std::move(message),
        });
    };
    if (record.descriptor.execution_policy == SystemExecutionPolicy::MainThreadOnly) {
        add_reason("main_thread_only", "main-thread-only policy");
    }
    if (record.descriptor.reads.empty() && record.descriptor.writes.empty()) {
        add_reason("missing_access_metadata", "missing access metadata");
    }
    if (!record.descriptor.before.empty() || !record.descriptor.after.empty()) {
        add_reason("explicit_dependency", "explicit dependency");
    }
    if (!record.descriptor.mutation_notes.empty()) {
        add_reason("unsafe_structural_mutation_policy", "unsafe mutation policy: " + record.descriptor.mutation_notes);
    }
    if (record.descriptor.structural_mutation_policy == SystemStructuralMutationPolicy::RawWorldAllowed) {
        add_reason("raw_world_allowed", "raw world mutation is serial-only");
    }
    return reasons;
}

SystemScheduleSnapshot EcsSystemRegistry::build_schedule(const std::vector<SystemRecord*>& records,
                                                         SystemExecutionMode requested_mode) const {
    const auto record_less = [](const SystemRecord* a, const SystemRecord* b) {
        if (a->descriptor.phase != b->descriptor.phase) {
            return phase_index(a->descriptor.phase) < phase_index(b->descriptor.phase);
        }
        if (a->descriptor.order != b->descriptor.order) {
            return a->descriptor.order < b->descriptor.order;
        }
        if (a->registration_sequence != b->registration_sequence) {
            return a->registration_sequence < b->registration_sequence;
        }
        return a->descriptor.id < b->descriptor.id;
    };
    const auto normalize = [](const std::vector<std::string>& values) {
        std::set<std::string> result;
        for (const std::string& value : values) {
            if (!value.empty()) {
                result.insert(value);
            }
        }
        return result;
    };
    const auto first_conflict = [](const std::set<std::string>& a_writes,
                                   const std::set<std::string>& b_reads,
                                   const std::set<std::string>& b_writes) -> std::string {
        for (const std::string& component : a_writes) {
            if (b_reads.contains(component) || b_writes.contains(component)) {
                return component;
            }
        }
        return {};
    };
    const auto relation_reads = [](const SystemRecord* record) {
        std::set<std::string> result;
        for (const SystemRelationAccess& access : record->descriptor.relations) {
            if (access.mode == SystemAccessMode::Read && !access.relation.empty()) {
                result.insert(access.relation);
            }
        }
        return result;
    };
    const auto relation_writes = [](const SystemRecord* record) {
        std::set<std::string> result;
        for (const SystemRelationAccess& access : record->descriptor.relations) {
            if (access.mode == SystemAccessMode::Write && !access.relation.empty()) {
                result.insert(access.relation);
            }
        }
        return result;
    };

    SystemScheduleSnapshot schedule{
        .requested_execution_mode = requested_mode,
        .effective_execution_mode = SystemExecutionMode::SerialGraph,
    };
    std::unordered_set<std::string> known_ids;
    for (const SystemRecord& record : _records) {
        known_ids.insert(record.descriptor.id);
    }

    std::unordered_map<std::string, SystemRecord*> selected;
    std::vector<SystemRecord*> sorted = records;
    std::ranges::sort(sorted, record_less);
    for (SystemRecord* record : sorted) {
        selected[record->descriptor.id] = record;
    }

    const auto add_edge = [&](std::string before, std::string after, std::string reason) {
        if (before.empty() || after.empty() || before == after) {
            return;
        }
        schedule.edges.push_back({
            .before = std::move(before),
            .after = std::move(after),
            .reason = std::move(reason),
        });
    };

    for (SystemRecord* record : sorted) {
        for (const SystemId& target : record->descriptor.before) {
            if (!known_ids.contains(target)) {
                schedule.valid = false;
                schedule.diagnostics.push_back("system '" + record->descriptor.id + "' has unknown before dependency '" + target + "' (was '" + target + "' rejected at registration? check earlier 'system registration failed' log errors)");
                continue;
            }
            const auto found = selected.find(target);
            if (found != selected.end() && found->second->descriptor.phase == record->descriptor.phase) {
                add_edge(record->descriptor.id, target, "explicit before");
            }
        }
        for (const SystemId& target : record->descriptor.after) {
            if (!known_ids.contains(target)) {
                schedule.valid = false;
                schedule.diagnostics.push_back("system '" + record->descriptor.id + "' has unknown after dependency '" + target + "' (was '" + target + "' rejected at registration? check earlier 'system registration failed' log errors)");
                continue;
            }
            const auto found = selected.find(target);
            if (found != selected.end() && found->second->descriptor.phase == record->descriptor.phase) {
                add_edge(target, record->descriptor.id, "explicit after");
            }
        }
    }

    for (SystemPhase phase : phases) {
        std::vector<SystemRecord*> phase_records;
        for (SystemRecord* record : sorted) {
            if (record->descriptor.phase == phase) {
                phase_records.push_back(record);
            }
        }

        for (std::size_t i = 0; i < phase_records.size(); ++i) {
            SystemRecord* a = phase_records[i];
            const bool a_unsafe = a->descriptor.reads.empty() && a->descriptor.writes.empty();
            const std::set<std::string> a_reads = normalize(a->descriptor.reads);
            const std::set<std::string> a_writes = normalize(a->descriptor.writes);
            const std::set<std::string> a_relation_reads = relation_reads(a);
            const std::set<std::string> a_relation_writes = relation_writes(a);
            if (a_unsafe) {
                schedule.diagnostics.push_back("system '" + a->descriptor.id + "' has no read/write metadata; isolated in schedule");
            }

            for (std::size_t j = i + 1; j < phase_records.size(); ++j) {
                SystemRecord* b = phase_records[j];
                const bool b_unsafe = b->descriptor.reads.empty() && b->descriptor.writes.empty();
                if (a_unsafe || b_unsafe) {
                    add_edge(a->descriptor.id, b->descriptor.id, "unsafe access metadata");
                    continue;
                }

                const std::set<std::string> b_reads = normalize(b->descriptor.reads);
                const std::set<std::string> b_writes = normalize(b->descriptor.writes);
                const std::set<std::string> b_relation_reads = relation_reads(b);
                const std::set<std::string> b_relation_writes = relation_writes(b);
                std::string component = first_conflict(a_writes, b_reads, b_writes);
                if (component.empty()) {
                    component = first_conflict(b_writes, a_reads, a_writes);
                }
                if (!component.empty()) {
                    add_edge(a->descriptor.id, b->descriptor.id, "access conflict: " + component);
                    continue;
                }
                std::string relation = first_conflict(a_relation_writes, b_relation_reads, b_relation_writes);
                if (relation.empty()) {
                    relation = first_conflict(b_relation_writes, a_relation_reads, a_relation_writes);
                }
                if (!relation.empty()) {
                    add_edge(a->descriptor.id, b->descriptor.id, "relation access conflict: " + relation);
                }
            }
        }

        std::unordered_map<std::string, i32> indegree;
        std::unordered_map<std::string, std::vector<std::string>> adjacency;
        for (SystemRecord* record : phase_records) {
            indegree[record->descriptor.id] = 0;
        }
        for (const SystemDependencyEdge& edge : schedule.edges) {
            const auto before = selected.find(edge.before);
            const auto after = selected.find(edge.after);
            if (before == selected.end() || after == selected.end()) {
                continue;
            }
            if (before->second->descriptor.phase != phase || after->second->descriptor.phase != phase) {
                continue;
            }
            adjacency[edge.before].push_back(edge.after);
            ++indegree[edge.after];
        }

        std::unordered_set<std::string> processed;
        i32 batch_index = 0;
        while (processed.size() < phase_records.size()) {
            std::vector<SystemRecord*> ready;
            for (SystemRecord* record : phase_records) {
                if (!processed.contains(record->descriptor.id) && indegree[record->descriptor.id] == 0) {
                    ready.push_back(record);
                }
            }
            if (ready.empty()) {
                schedule.valid = false;
                schedule.diagnostics.push_back("cycle detected in phase '" + std::string{system_phase_name(phase)} + "'");
                break;
            }
            std::ranges::sort(ready, record_less);

            if (requested_mode != SystemExecutionMode::ParallelBatches) {
                SystemBatch batch{
                    .phase = phase,
                    .index = batch_index++,
                };
                bool batch_parallel_eligible = true;
                for (SystemRecord* record : ready) {
                    processed.insert(record->descriptor.id);
                    batch.systems.push_back(record->descriptor.id);
                    const std::vector<SystemParallelDiagnostic> reasons = parallel_eligibility_reasons(*record);
                    if (!reasons.empty()) {
                        batch_parallel_eligible = false;
                        for (const SystemParallelDiagnostic& reason : reasons) {
                            batch.diagnostics.push_back("system '" + record->descriptor.id + "' is not parallel eligible: " + reason.message);
                            batch.parallel_diagnostics.push_back(reason);
                        }
                    }
                    for (const std::string& dependent : adjacency[record->descriptor.id]) {
                        --indegree[dependent];
                    }
                }
                batch.parallel_eligible = batch_parallel_eligible;
                schedule.batches.push_back(std::move(batch));
                continue;
            }

            for (SystemRecord* record : ready) {
                processed.insert(record->descriptor.id);
                for (const std::string& dependent : adjacency[record->descriptor.id]) {
                    --indegree[dependent];
                }
            }

            std::vector<SystemRecord*> parallel_run;
            const auto flush_parallel_run = [&] {
                if (parallel_run.empty()) {
                    return;
                }
                SystemBatch batch{
                    .phase = phase,
                    .index = batch_index++,
                    .parallel_eligible = true,
                };
                for (SystemRecord* record : parallel_run) {
                    batch.systems.push_back(record->descriptor.id);
                }
                schedule.batches.push_back(std::move(batch));
                parallel_run.clear();
            };

            for (SystemRecord* record : ready) {
                const std::vector<SystemParallelDiagnostic> reasons = parallel_eligibility_reasons(*record);
                if (reasons.empty()) {
                    parallel_run.push_back(record);
                    continue;
                }

                flush_parallel_run();

                SystemBatch batch{
                    .phase = phase,
                    .index = batch_index++,
                    .parallel_eligible = false,
                };
                batch.systems.push_back(record->descriptor.id);
                for (const SystemParallelDiagnostic& reason : reasons) {
                    batch.diagnostics.push_back("system '" + record->descriptor.id + "' is not parallel eligible: " + reason.message);
                    batch.parallel_diagnostics.push_back(reason);
                }
                schedule.batches.push_back(std::move(batch));
            }
            flush_parallel_run();
        }
    }

    return schedule;
}

bool EcsSystemRegistry::run_schedule(SystemScheduleSnapshot& schedule, f32 dt) {
    bool ok = true;
    for (SystemBatch& batch : schedule.batches) {
        const auto batch_start = std::chrono::steady_clock::now();
        if (schedule.requested_execution_mode == SystemExecutionMode::ParallelBatches &&
            batch.parallel_eligible) {
            const bool batch_ok = run_parallel_batch(schedule, batch, dt);
            const auto batch_end = std::chrono::steady_clock::now();
            batch.last_duration_ms = static_cast<f64>(std::chrono::duration_cast<std::chrono::nanoseconds>(batch_end - batch_start).count()) / 1'000'000.0;
            ok = batch_ok && ok;
            continue;
        }
        batch.execution_mode = SystemExecutionMode::SerialGraph;
        std::vector<std::unique_ptr<EcsCommandBuffer>> command_buffers;
        std::vector<SystemRunResult> results;
        command_buffers.reserve(batch.systems.size());
        results.reserve(batch.systems.size());
        for (const SystemId& id : batch.systems) {
            SystemRecord* record = find_record(id);
            if (!record) {
                _last_error = "scheduled system disappeared '" + id + "'";
                ok = false;
                continue;
            }
            auto commands = std::make_unique<EcsCommandBuffer>();
            EcsCommandBuffer* commands_ptr = commands.get();
            SystemRunResult result = execute_record(*record, dt, false, commands_ptr);
            record->last_execution_mode = SystemExecutionMode::SerialGraph;
            record->last_batch_execution_mode = SystemExecutionMode::SerialGraph;
            record->last_execution_decision = result.decision;
            record->execution_decision_reason = result.decision_reason;
            record->worker_count = result.worker_count;
            record->estimated_work_ms = result.estimated_work_ms;
            command_buffers.push_back(std::move(commands));
            if (!result.ok) {
                _last_error = result.error;
                KIN_LOG_ERROR_F("ecs",
                                "system run failed",
                                (LogFields{
                                    {.name = "system", .value = record->descriptor.id},
                                    {.name = "error", .value = result.error},
                                }));
                _world->events().emit_system(EcsEventKind::SystemError, record->descriptor.id, result.error);
                ok = false;
            } else if (result.elapsed_ns > 0) {
                if (ProfileSession* profile = current_profile_session()) {
                    profile->record(record->descriptor.id,
                                    "ecs.system",
                                    static_cast<u64>(result.elapsed_ns),
                                    record->source_location);
                }
            }
            results.push_back(result);
        }
        ok = flush_command_results(schedule, batch, results) && ok;
        const auto batch_end = std::chrono::steady_clock::now();
        batch.last_duration_ms = static_cast<f64>(std::chrono::duration_cast<std::chrono::nanoseconds>(batch_end - batch_start).count()) / 1'000'000.0;
    }
    return ok;
}

bool EcsSystemRegistry::collect_batch_records(const SystemBatch& batch,
                                              std::vector<SystemRecord*>& records,
                                              bool& has_native_system) {
    records.clear();
    records.reserve(batch.systems.size());
    has_native_system = false;
    for (const SystemId& id : batch.systems) {
        SystemRecord* record = find_record(id);
        if (!record) {
            _last_error = "scheduled system disappeared '" + id + "'";
            return false;
        }
        has_native_system = has_native_system || record->flecs_id != 0;
        records.push_back(record);
    }
    return true;
}

void EcsSystemRegistry::publish_parallel_results(SystemScheduleSnapshot& schedule,
                                                 SystemBatch& batch,
                                                 const std::vector<SystemRunResult>& results,
                                                 bool& ok,
                                                 bool& any_parallel) {
    for (const SystemRunResult& result : results) {
        if (!result.record) {
            continue;
        }
        result.record->last_execution_mode = result.execution_mode;
        result.record->last_batch_execution_mode = batch.execution_mode;
        result.record->last_execution_decision = result.decision;
        result.record->execution_decision_reason = result.decision_reason;
        result.record->worker_count = result.worker_count;
        result.record->estimated_work_ms = result.estimated_work_ms;
        any_parallel = any_parallel || result.execution_mode == SystemExecutionMode::ParallelBatches;
        if (!result.ok) {
            ok = false;
            _last_error = result.error;
            KIN_LOG_ERROR_F("ecs",
                        "parallel system run failed",
                        (LogFields{
                            {.name = "system", .value = result.record->descriptor.id},
                            {.name = "error", .value = result.error},
                        }));
        _world->events().emit_system(EcsEventKind::SystemError, result.record->descriptor.id, result.error);
            schedule.diagnostics.push_back("parallel system '" + result.record->descriptor.id + "' failed: " + result.error);
        } else if (result.elapsed_ns > 0) {
            if (ProfileSession* profile = current_profile_session()) {
                profile->record(result.record->descriptor.id,
                                "ecs.system",
                                static_cast<u64>(result.elapsed_ns),
                                result.record->source_location);
            }
        }
    }
}

bool EcsSystemRegistry::run_parallel_batch(SystemScheduleSnapshot& schedule, SystemBatch& batch, f32 dt) {
    std::vector<SystemRecord*> records;
    bool has_native_system = false;
    if (!collect_batch_records(batch, records, has_native_system)) {
        return false;
    }

    std::vector<SystemRunResult> results(records.size());
    std::vector<std::unique_ptr<EcsCommandBuffer>> command_buffers;
    command_buffers.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        command_buffers.push_back(std::make_unique<EcsCommandBuffer>());
    }

    const f64 estimated_batch_ms = [&] {
        f64 total = 0.0;
        for (const SystemRecord* record : records) {
            total += record->stats.average_duration_ms;
        }
        return total;
    }();
    batch.estimated_work_ms = estimated_batch_ms;

    if (has_native_system) {
        batch.execution_mode = SystemExecutionMode::SerialGraph;
        batch.execution_decision = SystemExecutionDecision::Serial;
        batch.execution_decision_reason = "mixed/native batch runs systems in deterministic order";
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (records[i]->flecs_id != 0) {
                results[i] = execute_native_parallel_record(*records[i], dt, command_buffers[i].get());
            } else {
                results[i] = execute_record(*records[i], dt, false, command_buffers[i].get(), false);
                results[i].decision_reason = "mixed native/task batch runs task serial in V1";
            }
        }
    } else {
        const bool enough_systems = static_cast<i32>(records.size()) >= std::max(_execution_tuning.min_parallel_systems, 2);
        if (!enough_systems) {
            batch.execution_mode = SystemExecutionMode::SerialGraph;
            batch.execution_decision = SystemExecutionDecision::Serial;
            batch.execution_decision_reason = "task batch size below threshold";
            for (std::size_t i = 0; i < records.size(); ++i) {
                results[i] = execute_record(*records[i], dt, false, command_buffers[i].get(), false);
                results[i].decision_reason = batch.execution_decision_reason;
                results[i].estimated_work_ms = records[i]->stats.average_duration_ms;
            }
        } else {
            batch.execution_mode = SystemExecutionMode::ParallelBatches;
            batch.execution_decision = SystemExecutionDecision::ParallelAcrossSystems;
            batch.execution_decision_reason = "task batch size and estimate selected worker threads";
            batch.worker_count = static_cast<i32>(records.size());
            std::string launch_error;
            try {
                _jobs->parallel_for(static_cast<i32>(records.size()), [&](i32 index) {
                    const std::size_t i = static_cast<std::size_t>(index);
                    results[i] = execute_record(*records[i], dt, false, command_buffers[i].get(), true);
                    results[i].decision = SystemExecutionDecision::ParallelAcrossSystems;
                    results[i].decision_reason = batch.execution_decision_reason;
                    results[i].worker_count = batch.worker_count;
                    results[i].estimated_work_ms = records[i]->stats.average_duration_ms;
                });
            } catch (const std::exception& error) {
                launch_error = error.what();
            } catch (...) {
                launch_error = "unknown task worker launch error";
            }
            if (!launch_error.empty()) {
                for (std::size_t i = 0; i < records.size(); ++i) {
                    if (results[i].record) {
                        continue;
                    }
                    results[i] = SystemRunResult{
                        .record = records[i],
                        .ok = false,
                        .error = "task worker launch failed: " + launch_error,
                        .commands = command_buffers[i].get(),
                        .execution_mode = SystemExecutionMode::SerialGraph,
                        .decision = SystemExecutionDecision::Serial,
                        .decision_reason = "task worker launch failed",
                        .worker_count = 1,
                        .estimated_work_ms = records[i]->stats.average_duration_ms,
                    };
                }
            }
        }
    }

    bool ok = true;
    bool any_parallel = false;
    publish_parallel_results(schedule, batch, results, ok, any_parallel);
    ok = flush_command_results(schedule, batch, results) && ok;
    if (any_parallel) {
        batch.execution_mode = SystemExecutionMode::ParallelBatches;
        if (batch.execution_decision == SystemExecutionDecision::Serial) {
            batch.execution_decision = SystemExecutionDecision::ParallelWithinNativeSystem;
            batch.execution_decision_reason = "one or more native systems selected Flecs workers";
            for (const SystemRunResult& result : results) {
                batch.worker_count = std::max(batch.worker_count, result.worker_count);
            }
        }
        for (const SystemRunResult& result : results) {
            if (result.record) {
                result.record->last_batch_execution_mode = batch.execution_mode;
            }
        }
        schedule.effective_execution_mode = SystemExecutionMode::ParallelBatches;
    }
    return ok;
}

std::vector<EcsSystemRegistry::SystemRecord*> EcsSystemRegistry::eligible_records(SystemPhase* phase, f32 dt) {
    std::vector<SystemRecord*> result;
    for (SystemRecord* record : sorted_records(phase)) {
        if (!record->enabled) {
            continue;
        }
        if (!scheduled_run_due(*record, dt)) {
            continue;
        }
        result.push_back(record);
    }
    return result;
}

std::vector<EcsSystemRegistry::SystemRecord*> EcsSystemRegistry::enabled_records(SystemPhase* phase) {
    std::vector<SystemRecord*> result;
    for (SystemRecord* record : sorted_records(phase)) {
        if (record->enabled) {
            result.push_back(record);
        }
    }
    return result;
}

void EcsSystemRegistry::apply_schedule_diagnostics(const SystemScheduleSnapshot& schedule) {
    std::unordered_map<std::string, std::vector<std::string>> diagnostics_by_system;
    for (SystemRecord& record : _records) {
        record.schedule_diagnostics.clear();
        record.parallel_diagnostics.clear();
        record.batch_index = -1;
        record.parallel_eligible = false;
    }

    for (const std::string& diagnostic : schedule.diagnostics) {
        for (const SystemRecord& record : _records) {
            if (diagnostic.find("'" + record.descriptor.id + "'") != std::string::npos) {
                diagnostics_by_system[record.descriptor.id].push_back(diagnostic);
            }
        }
    }
    for (const SystemDependencyEdge& edge : schedule.edges) {
        const std::string text = edge.reason + ": " + edge.before + " -> " + edge.after;
        diagnostics_by_system[edge.before].push_back(text);
        diagnostics_by_system[edge.after].push_back(text);
    }
    for (const SystemBatch& batch : schedule.batches) {
        for (const std::string& diagnostic : batch.diagnostics) {
            for (const SystemRecord& record : _records) {
                if (diagnostic.find("'" + record.descriptor.id + "'") != std::string::npos) {
                    diagnostics_by_system[record.descriptor.id].push_back(diagnostic);
                }
            }
        }
        for (const SystemId& id : batch.systems) {
            if (SystemRecord* record = find_record(id)) {
                record->batch_index = batch.index;
                record->parallel_eligible = batch.parallel_eligible;
                if (!batch.parallel_eligible) {
                    record->parallel_eligible = parallel_eligibility_reasons(*record).empty();
                }
            }
        }
        for (const SystemParallelDiagnostic& diagnostic : batch.parallel_diagnostics) {
            if (SystemRecord* record = find_record(diagnostic.system)) {
                record->parallel_diagnostics.push_back(diagnostic);
            }
        }
    }
    for (SystemRecord& record : _records) {
        if (const auto found = diagnostics_by_system.find(record.descriptor.id); found != diagnostics_by_system.end()) {
            record.schedule_diagnostics = found->second;
        }
    }
}

std::vector<EcsSystemRegistry::SystemRecord*> EcsSystemRegistry::sorted_records(SystemPhase* phase) {
    std::vector<SystemRecord*> result;
    for (SystemRecord& record : _records) {
        if (phase && record.descriptor.phase != *phase) {
            continue;
        }
        result.push_back(&record);
    }
    std::ranges::sort(result, [](const SystemRecord* a, const SystemRecord* b) {
        if (a->descriptor.phase != b->descriptor.phase) {
            return phase_index(a->descriptor.phase) < phase_index(b->descriptor.phase);
        }
        if (a->descriptor.order != b->descriptor.order) {
            return a->descriptor.order < b->descriptor.order;
        }
        if (a->registration_sequence != b->registration_sequence) {
            return a->registration_sequence < b->registration_sequence;
        }
        return a->descriptor.id < b->descriptor.id;
    });
    return result;
}

std::vector<const EcsSystemRegistry::SystemRecord*> EcsSystemRegistry::sorted_records(SystemPhase* phase) const {
    std::vector<const SystemRecord*> result;
    for (const SystemRecord& record : _records) {
        if (phase && record.descriptor.phase != *phase) {
            continue;
        }
        result.push_back(&record);
    }
    std::ranges::sort(result, [](const SystemRecord* a, const SystemRecord* b) {
        if (a->descriptor.phase != b->descriptor.phase) {
            return phase_index(a->descriptor.phase) < phase_index(b->descriptor.phase);
        }
        if (a->descriptor.order != b->descriptor.order) {
            return a->descriptor.order < b->descriptor.order;
        }
        if (a->registration_sequence != b->registration_sequence) {
            return a->registration_sequence < b->registration_sequence;
        }
        return a->descriptor.id < b->descriptor.id;
    });
    return result;
}

} // namespace kin
